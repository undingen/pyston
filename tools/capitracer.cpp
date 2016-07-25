// Copyright (c) 2014-2016 Dropbox, Inc.
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

#include <cstdio>
#include <iostream>
#include <unordered_map>

#include "llvm/ADT/Triple.h"
#include "llvm/Analysis/Passes.h"
#include "llvm/Bitcode/ReaderWriter.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/MCJIT.h"
#include "llvm/ExecutionEngine/ObjectCache.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/SystemUtils.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/Cloning.h"

using namespace llvm;

static cl::opt<std::string>
InputFilename(cl::Positional, cl::desc("<input bitcode file>"),
        cl::init("-"), cl::value_desc("filename"));

static cl::opt<std::string>
OutputFilename("o", cl::desc("Specify output filename"),
        cl::value_desc("filename"), cl::init("-"));

static cl::opt<bool>
Force("f", cl::desc("Enable binary output on terminals"));

bool visistFunc(Function* f) {
    bool changed = false;

    LLVMContext &c = f->getContext();


    if (f->getName() != "str_length")
        return changed;

    f->dump();


    outs()<< "\n\nvoid rewriter_" << f->getName() << "(_CallRewriteArgsBase* rewrite_args";

    llvm::DenseMap<Value*, std::string> known_values;
    for (auto&& arg : f->args()) {
        outs() << ", Box* a" + std::to_string(known_values.size());
        known_values[&arg] = "v" + std::to_string(known_values.size());
    }
    outs()<<") {\n";

    auto DL = *f->getDataLayout();

    for (auto it = inst_begin(f), end = inst_end(f); it != end; ++it) {
        Instruction* I = &*it;

        outs() << "// ";
        I->dump();


        if (isa<DbgInfoIntrinsic>(I)) {
            outs() << "//    ignored debug info\n";
            continue;
        }


        if (auto GEP = dyn_cast_or_null<GetElementPtrInst>(I)) {
            if (!known_values.count(GEP->getPointerOperand())) {
                outs() << "unknown value: ";
                GEP->getPointerOperand()->dump();
                continue;
            }

            //if (GEP->getType()->getElementType() != llvm::Type::getInt64Ty(c)) {
            if (DL.getPointerTypeSizeInBits(GEP->getType()) != 64) {
                outs() << "unknown return type: " << DL.getPointerTypeSizeInBits(GEP->getType()) << " ";
                GEP->getType()->dump();
                continue;
            }

            APInt offset(DL.getPointerSizeInBits(GEP->getPointerAddressSpace()), 0);
            if (!GEP->accumulateConstantOffset(DL, offset)) {
                outs() << "non const gep\n";
                continue;
            }

            outs() << "//    skipping\n";
            continue;


            auto v = known_values[GEP->getPointerOperand()];
            auto new_var = "v" + std::to_string(known_values.size());
            outs() << "auto " << new_var << " = " << v << "->getAttr(" << offset.getSExtValue() << "" << ")\n";

            known_values[GEP] = new_var;
        }
        else if (auto BC = dyn_cast_or_null<BitCastInst>(I)) {
            outs() << "//    skipping\n";
            continue;
        }
        else if (auto L = dyn_cast_or_null<LoadInst>(I)) {
            if (DL.getTypeSizeInBits(L->getType()) != 64) {
                outs() << "unknown return type size: " << DL.getTypeSizeInBits(L->getType()) << " ";
                L->getType()->dump();
                continue;
            }

            auto PT = L->getPointerOperand();
            auto PT_no_casts = PT->stripPointerCasts();
            if (auto GEP = dyn_cast_or_null<GetElementPtrInst>(PT_no_casts)) {
                if (!known_values.count(GEP->getPointerOperand())) {
                    outs() << "unknown value: ";
                    GEP->getPointerOperand()->dump();
                    continue;
                }

                //if (GEP->getType()->getElementType() != llvm::Type::getInt64Ty(c)) {
                if (DL.getPointerTypeSizeInBits(GEP->getType()) != 64) {
                    outs() << "unknown return type: " << DL.getPointerTypeSizeInBits(GEP->getType()) << " ";
                    GEP->getType()->dump();
                    continue;
                }

                APInt offset(DL.getPointerSizeInBits(GEP->getPointerAddressSpace()), 0);
                if (!GEP->accumulateConstantOffset(DL, offset)) {
                    outs() << "non const gep\n";
                    continue;
                }

                auto v = known_values[GEP->getPointerOperand()];
                auto new_var = "v" + std::to_string(known_values.size() + 1);
                outs() << "auto " << new_var << " = " << v << "->getAttr(" << offset.getSExtValue() << "" << ");\n";

                known_values[L] = new_var;
            }
        } else if (auto R = dyn_cast_or_null<ReturnInst>(I)) {
            if (!known_values.count(R->getReturnValue())) {
                outs() << "unknown value: ";
                R->getReturnValue()->dump();
                continue;
            }
            if (R->getReturnValue()->getType()->isPointerTy())
                assert(0);
            outs() << "rewriter_args->r_ret = " << known_values[R->getReturnValue()] << ";\n";
        }
    }
    outs() << "} \n";
    outs() << "capi_tracer[(void*)" << f->getName() << "] = " << "(void*)rewriter_" << f->getName() << ";\n";

    return changed;
}

int main(int argc, char **argv) {
    sys::PrintStackTraceOnErrorSignal();
    PrettyStackTraceProgram X(argc, argv);

    LLVMContext &Context = getGlobalContext();
    llvm_shutdown_obj Y;
    cl::ParseCommandLineOptions(argc, argv, "mcjit pre-cacher");

    SMDiagnostic Err;

#if LLVMREV < 216466
    std::unique_ptr<Module> M(ParseIRFile(InputFilename, Err, Context));
#else
    std::unique_ptr<Module> M(parseIRFile(InputFilename, Err, Context));
#endif

    if (M.get() == 0) {
        Err.print(argv[0], errs());
        return 1;
    }

    outs() << "llvm::DenseMap<void*, void*> capi_tracer;\n";

    for (Module::iterator I = M->begin(), E = M->end(); I != E; ++I) {
        visistFunc(I);
    }


    /*
    if (OutputFilename.empty())
        OutputFilename = "-";

#if LLVMREV < 216393
    std::string ErrorInfo;
    tool_output_file out(OutputFilename.c_str(), ErrorInfo, sys::fs::F_None);
    if (!ErrorInfo.empty()) {
        errs() << ErrorInfo << '\n';
        return 1;
    }
#else
    std::error_code EC;
    tool_output_file out(OutputFilename, EC, sys::fs::F_None);
    if (EC) {
        errs() << "error opening file for writing\n";
        return 1;
    }
#endif

    WriteBitcodeToFile(M.get(), out.os());

    out.keep();
    */

    return 0;
}

