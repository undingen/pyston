// Copyright (c) 2014-2015 Dropbox, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "codegen/entry.h"

#include <cstdio>
#include <iostream>
#include <unordered_map>
#include <zlib.h>

#include "llvm/Analysis/Passes.h"
#include "llvm/Bitcode/ReaderWriter.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/JITEventListener.h"
#include "llvm/ExecutionEngine/MCJIT.h"
#include "llvm/ExecutionEngine/ObjectCache.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include "codegen/codegen.h"
#include "codegen/memmgr.h"
#include "codegen/profiling/profiling.h"
#include "codegen/stackmaps.h"
#include "core/options.h"
#include "core/types.h"
#include "core/util.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

/*
 * Include this file to force the linking of non-default algorithms, such as the "basic" register allocator
 */
#include "llvm/CodeGen/LinkAllCodegenComponents.h"

namespace pyston {

GlobalState g;

extern "C" {
#ifndef BINARY_SUFFIX
#error Must define BINARY_SUFFIX
#endif
#ifndef BINARY_STRIPPED_SUFFIX
#error Must define BINARY_STRIPPED_SUFFIX
#endif

#define _CONCAT3(a, b, c) a##b##c
#define CONCAT3(a, b, c) _CONCAT3(a, b, c)
#define _CONCAT4(a, b, c, d) a##b##c##d
#define CONCAT4(a, b, c, d) _CONCAT4(a, b, c, d)

#define STDLIB_BC_START CONCAT3(_binary_stdlib, BINARY_SUFFIX, _bc_start)
#define STDLIB_BC_SIZE CONCAT3(_binary_stdlib, BINARY_SUFFIX, _bc_size)
extern char STDLIB_BC_START[];
extern int STDLIB_BC_SIZE;

#define STRIPPED_STDLIB_BC_START CONCAT4(_binary_stdlib, BINARY_SUFFIX, BINARY_STRIPPED_SUFFIX, _bc_start)
#define STRIPPED_STDLIB_BC_SIZE CONCAT4(_binary_stdlib, BINARY_SUFFIX, BINARY_STRIPPED_SUFFIX, _bc_size)
extern char STRIPPED_STDLIB_BC_START[];
extern int STRIPPED_STDLIB_BC_SIZE;
}

static llvm::Module* loadStdlib() {
    Timer _t("to load stdlib");

    llvm::StringRef data;
    if (!USE_STRIPPED_STDLIB) {
        // Make sure the stdlib got linked in correctly; check the magic number at the beginning:
        assert(STDLIB_BC_START[0] == 'B');
        assert(STDLIB_BC_START[1] == 'C');
        intptr_t size = (intptr_t)&STDLIB_BC_SIZE;
        assert(size > 0 && size < 1 << 30); // make sure the size is being loaded correctly
        data = llvm::StringRef(STDLIB_BC_START, size);
    } else {
        // Make sure the stdlib got linked in correctly; check the magic number at the beginning:
        assert(STRIPPED_STDLIB_BC_START[0] == 'B');
        assert(STRIPPED_STDLIB_BC_START[1] == 'C');
        intptr_t size = (intptr_t)&STRIPPED_STDLIB_BC_SIZE;
        assert(size > 0 && size < 1 << 30); // make sure the size is being loaded correctly
        data = llvm::StringRef(STRIPPED_STDLIB_BC_START, size);
    }

#if LLVMREV < 216583
    llvm::MemoryBuffer* buffer = llvm::MemoryBuffer::getMemBuffer(data, "", false);
#else
    std::unique_ptr<llvm::MemoryBuffer> buffer = llvm::MemoryBuffer::getMemBuffer(data, "", false);
#endif

    // llvm::ErrorOr<llvm::Module*> m_or = llvm::parseBitcodeFile(buffer, g.context);
    llvm::ErrorOr<llvm::Module*> m_or = llvm::getLazyBitcodeModule(std::move(buffer), g.context);
    RELEASE_ASSERT(m_or, "");
    llvm::Module* m = m_or.get();
    assert(m);

    for (llvm::Module::global_iterator I = m->global_begin(), E = m->global_end(); I != E; ++I) {
        if (I->getLinkage() == llvm::GlobalVariable::PrivateLinkage)
            I->setLinkage(llvm::GlobalVariable::ExternalLinkage);
    }
    m->setModuleIdentifier("  stdlib  ");
    return m;
}

class PystonObjectCache : public llvm::ObjectCache {
private:
    class HashOStream : public llvm::raw_ostream {
        unsigned int hash = 0;
        void write_impl(const char* ptr, size_t size) override { hash = crc32(hash, (const unsigned char*)ptr, size); }
        uint64_t current_pos() const override { return 0; }

    public:
        unsigned int getHash() {
            flush();
            return hash;
        }
    };


    llvm::SmallString<128> cache_dir;
    std::string module_identifier;
    std::string hash_before_codegen;

public:
    PystonObjectCache() {
        llvm::sys::fs::current_path(cache_dir);
        llvm::sys::path::append(cache_dir, "pyston_object_cache");
    }


#if LLVMREV < 216002
    virtual void notifyObjectCompiled(const llvm::Module* M, const llvm::MemoryBuffer* Obj)
#else
    virtual void notifyObjectCompiled(const llvm::Module* M, llvm::MemoryBufferRef Obj)
#endif
    {
        RELEASE_ASSERT(module_identifier == M->getModuleIdentifier(), "");
        RELEASE_ASSERT(!hash_before_codegen.empty(), "");

        llvm::SmallString<128> cache_file = cache_dir;
        llvm::sys::path::append(cache_file, hash_before_codegen);
        if (!llvm::sys::fs::exists(cache_dir.str()) && llvm::sys::fs::create_directory(cache_dir.str())) {
            fprintf(stderr, "Unable to create cache directory\n");
            return;
        }
        std::error_code error_code;
        llvm::raw_fd_ostream IRObjectFile(cache_file.c_str(), error_code, llvm::sys::fs::F_RW);
        RELEASE_ASSERT(!error_code, "");
        IRObjectFile << Obj.getBuffer();
    }

#if LLVMREV < 215566
    virtual llvm::MemoryBuffer* getObject(const llvm::Module* M)
#else
    virtual std::unique_ptr<llvm::MemoryBuffer> getObject(const llvm::Module* M)
#endif
    {
        static StatCounter jit_objectcache_hits("num_jit_objectcache_hits");
        static StatCounter jit_objectcache_misses("num_jit_objectcache_misses");

        module_identifier = M->getModuleIdentifier();

        // Generate a hash for them module
        HashOStream hash_stream;
        llvm::WriteBitcodeToFile(M, hash_stream);
        unsigned int module_hash = hash_stream.getHash();
        hash_before_codegen = std::to_string(module_hash);

        llvm::SmallString<128> cache_file = cache_dir;
        llvm::sys::path::append(cache_file, hash_before_codegen);
        if (!llvm::sys::fs::exists(cache_file.str())) {
            // This file isn't in our cache
            jit_objectcache_misses.log();
            return NULL;
        }

        auto rtn = llvm::MemoryBuffer::getFile(cache_file.str(), -1, false);
        if (!rtn) {
            jit_objectcache_misses.log();
            return NULL;
        }

        jit_objectcache_hits.log();

        // MCJIT will want to write into this buffer, and we don't want that
        // because the file has probably just been mmapped.  Instead we make
        // a copy.  The filed-based buffer will be released when it goes
        // out of scope.
        return llvm::MemoryBuffer::getMemBufferCopy((*rtn)->getBuffer());
    }
};


static void handle_sigusr1(int signum) {
    assert(signum == SIGUSR1);
    fprintf(stderr, "SIGUSR1, printing stack trace\n");
    _printStacktrace();
}

static void handle_sigint(int signum) {
    assert(signum == SIGINT);
    // TODO: this should set a flag saying a KeyboardInterrupt is pending.
    // For now, just call abort(), so that we get a traceback at least.
    fprintf(stderr, "SIGINT!\n");
    abort();
}

void initCodegen() {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    g.stdlib_module = loadStdlib();

#if LLVMREV < 215967
    llvm::EngineBuilder eb(new llvm::Module("empty_initial_module", g.context));
#else
    llvm::EngineBuilder eb(std::unique_ptr<llvm::Module>(new llvm::Module("empty_initial_module", g.context)));
#endif

#if LLVMREV < 216982
    eb.setUseMCJIT(true);
#endif

    eb.setEngineKind(llvm::EngineKind::JIT); // specify we only want the JIT, and not the interpreter fallback
#if LLVMREV < 223183
    eb.setMCJITMemoryManager(createMemoryManager().release());
#else
    eb.setMCJITMemoryManager(createMemoryManager());
#endif
    // eb.setOptLevel(llvm::CodeGenOpt::None); // -O0
    // eb.setOptLevel(llvm::CodeGenOpt::Less); // -O1
    // eb.setOptLevel(llvm::CodeGenOpt::Default); // -O2, -Os
    // eb.setOptLevel(llvm::CodeGenOpt::Aggressive); // -O3

    llvm::TargetOptions target_options;
    target_options.NoFramePointerElim = true;
    // target_options.EnableFastISel = true;
    eb.setTargetOptions(target_options);

    // TODO enable this?  should let us get better code:
    // eb.setMCPU(llvm::sys::getHostCPUName());

    g.tm = eb.selectTarget();
    assert(g.tm && "failed to get a target machine");
    g.engine = eb.create(g.tm);
    assert(g.engine && "engine creation failed?");

    // g.engine->setObjectCache(new MyObjectCache());
    g.engine->setObjectCache(new PystonObjectCache());

    g.i1 = llvm::Type::getInt1Ty(g.context);
    g.i8 = llvm::Type::getInt8Ty(g.context);
    g.i8_ptr = g.i8->getPointerTo();
    g.i32 = llvm::Type::getInt32Ty(g.context);
    g.i64 = llvm::Type::getInt64Ty(g.context);
    g.void_ = llvm::Type::getVoidTy(g.context);
    g.double_ = llvm::Type::getDoubleTy(g.context);

    std::vector<llvm::JITEventListener*> listeners = makeJITEventListeners();
    for (int i = 0; i < listeners.size(); i++) {
        g.jit_listeners.push_back(listeners[i]);
        g.engine->RegisterJITEventListener(listeners[i]);
    }

    llvm::JITEventListener* stackmap_listener = makeStackMapListener();
    g.jit_listeners.push_back(stackmap_listener);
    g.engine->RegisterJITEventListener(stackmap_listener);

#if ENABLE_INTEL_JIT_EVENTS
    llvm::JITEventListener* intel_listener = llvm::JITEventListener::createIntelJITEventListener();
    g.jit_listeners.push_back(intel_listener);
    g.engine->RegisterJITEventListener(intel_listener);
#endif

    llvm::JITEventListener* registry_listener = makeRegistryListener();
    g.jit_listeners.push_back(registry_listener);
    g.engine->RegisterJITEventListener(registry_listener);

    llvm::JITEventListener* tracebacks_listener = makeTracebacksListener();
    g.jit_listeners.push_back(tracebacks_listener);
    g.engine->RegisterJITEventListener(tracebacks_listener);

    if (SHOW_DISASM) {
#if LLVMREV < 216983
        llvm::JITEventListener* listener = new DisassemblerJITEventListener();
        g.jit_listeners.push_back(listener);
        g.engine->RegisterJITEventListener(listener);
#else
        fprintf(stderr, "The LLVM disassembler has been removed\n");
        abort();
#endif
    }

    initGlobalFuncs(g);

    setupRuntime();

    // signal(SIGFPE, &handle_sigfpe);
    signal(SIGUSR1, &handle_sigusr1);
    signal(SIGINT, &handle_sigint);


    // There are some parts of llvm that are only configurable through command line args,
    // so construct a fake argc/argv pair and pass it to the llvm command line machinery:
    std::vector<const char*> llvm_args = { "fake_name" };

    llvm_args.push_back("--enable-patchpoint-liveness");
    if (0) {
        // Enabling and debugging fast-isel:
        // llvm_args.push_back("--fast-isel");
        // llvm_args.push_back("--fast-isel-verbose");
        ////llvm_args.push_back("--fast-isel-abort");
    }

#ifndef NDEBUG
// llvm_args.push_back("--debug-only=debug-ir");
// llvm_args.push_back("--debug-only=regalloc");
// llvm_args.push_back("--debug-only=stackmaps");
#endif

    // llvm_args.push_back("--print-after-all");
    // llvm_args.push_back("--print-machineinstrs");
    if (USE_REGALLOC_BASIC)
        llvm_args.push_back("--regalloc=basic");

    llvm::cl::ParseCommandLineOptions(llvm_args.size(), &llvm_args[0], "<you should never see this>\n");
}

void teardownCodegen() {
    for (int i = 0; i < g.jit_listeners.size(); i++) {
        g.engine->UnregisterJITEventListener(g.jit_listeners[i]);
        delete g.jit_listeners[i];
    }
    g.jit_listeners.clear();
    delete g.engine;
}

void printAllIR() {
    assert(0 && "unimplemented");
    fprintf(stderr, "==============\n");
}

int joinRuntime() {
    // In the future this will have to wait for non-daemon
    // threads to finish

    if (PROFILE)
        g.func_addr_registry.dumpPerfMap();

    teardownRuntime();
    teardownCodegen();

    return 0;
}
}
