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

#ifndef PYSTON_CODEGEN_BASELINEJIT_H
#define PYSTON_CODEGEN_BASELINEJIT_H

#include <llvm/ADT/ArrayRef.h>

#include "asm_writing/rewriter.h"
#include "codegen/ast_interpreter.h"
#include "codegen/unwinding.h"
#include "gc/heap.h"

namespace pyston {

#define ENABLE_TRACING 1
#define ENABLE_TRACING_FUNC 1
#define ENABLE_TRACING_IC 0

class AST_expr;
class AST_stmt;
class Box;
class BoxedClosure;
class BoxedDict;
class BoxedList;
class BoxedTuple;
struct CompiledFunction;
struct LineInfo;

class SetGlobalIC;
class GetGlobalIC;
class SetAttrIC;
class GetAttrIC;
class SetItemIC;
class GetItemIC;
class CompareIC;
class AugBinopIC;
class UnaryopIC;
class RuntimeCallIC;

class JitFragment;
class JitCodeBlock {
private:
    static constexpr int code_size = 4096 * 12;
    static constexpr int epilog_size = 2; // size of [leave, ret] in bytes

    void* code;
    int entry_offset;
    int epilog_offset;
    assembler::Assembler a;
    bool iscurrently_tracing;

public:
    JitCodeBlock(llvm::StringRef name);
    std::unique_ptr<JitFragment> newFragment(CFGBlock* block, int jump_offset = 0);

private:
    void fragmentCommited(int size);
    static void writeTrivialEhFrame(void* eh_frame_addr, void* func_addr, uint64_t func_size);
    void EHwriteAndRegister(void* func_addr, uint64_t func_size);
};

class JitFragment : public Rewriter {
private:
    CFGBlock* block;
    int code_offset;
    int epilog_offset;
    int continue_jmp_offset;
    void* entry_code;
    bool& iscurrently_tracing;
    RewriterVar* interp;
    std::function<void(int)> commited_callback;
    llvm::DenseMap<InternedString, RewriterVar*> local_syms;

public:
    JitFragment(CFGBlock* block, ICSlotRewrite* rewrite, int code_offset, int epilog_offset, void* entry_code,
                bool& iscurrently_tracing, std::function<void(int)> commited_callback);

    RewriterVar* imm(void* val);
    RewriterVar* imm(uint64_t val);

    RewriterVar* emitAugbinop(Value lhs, Value rhs, int op_type);
    RewriterVar* emitBinop(Value lhs, Value rhs, int op_type);
    RewriterVar* emitBoxedLocalsGet(BoxedString* s);
    RewriterVar* emitCallattr(Value obj, BoxedString* attr, CallattrFlags flags, ArgPassSpec argspec,
                              std::vector<Value, StlCompatAllocator<Value>> args,
                              std::vector<BoxedString*>* keyword_names);
    RewriterVar* emitCompare(Value lhs, Value rhs, int op_type);
    RewriterVar* emitCreateDict();
    RewriterVar* emitCreateList(const llvm::SmallVectorImpl<Value>& values);
    RewriterVar* emitCreateSlice(Value start, Value stop, Value step);
    RewriterVar* emitCreateTuple(const llvm::SmallVectorImpl<Value>& values);
    RewriterVar* emitDeref(InternedString s);
    RewriterVar* emitExceptionMatches(Value v, Value cls);
    RewriterVar* emitGetAttr(Value obj, BoxedString* s);
    RewriterVar* emitGetBlockLocal(InternedString s);
    RewriterVar* emitGetClsAttr(Value obj, BoxedString* s);
    RewriterVar* emitGetGlobal(Box* global, BoxedString* s);
    RewriterVar* emitGetItem(Value value, Value slice);
    RewriterVar* emitGetLocal(InternedString s);
    RewriterVar* emitGetPystonIter(Value v);
    RewriterVar* emitHasnext(Value v);
    RewriterVar* emitLandingpad();
    RewriterVar* emitNonzero(Value v);
    RewriterVar* emitNotNonzero(Value v);
    RewriterVar* emitRuntimeCall(Value obj, ArgPassSpec argspec, std::vector<Value, StlCompatAllocator<Value>> args,
                                 std::vector<BoxedString*>* keyword_names);
    RewriterVar* emitUnaryop(Value v, int op_type);
    RewriterVar* emitUnpackIntoArray(Value v, uint64_t num);
    RewriterVar* emitYield(Value v);

    void emitSetAttr(Value obj, BoxedString* s, Value attr);
    void emitSetBlockLocal(InternedString s, Value v);
    void emitSetCurrentInst(AST_stmt* node);
    void emitSetExcInfo(Value type, Value value, Value traceback);
    void emitSetGlobal(Box* global, BoxedString* s, Value v);
    void emitSetItemName(BoxedString* s, Value v);
    void emitSetItem(Value target, Value slice, Value value);
    void emitSetLocal(InternedString s, bool set_closure, Value v);
    void emitUncacheExcInfo();

    void emitJump(CFGBlock* b);
    void emitOSRPoint(AST_Jump* node);
    void emitReturn(Value v);
    void emitSideExit(Value v, CFGBlock* next_block);

    void abortTrace();
    int compile();

    bool finishAssembly(ICSlotInfo* picked_slot, int continue_offset) override;

private:
#ifndef NDEBUG
    std::pair<uint64_t, uint64_t> asUInt(InternedString s);
#else
    uint64_t asUInt(InternedString s);
#endif
    RewriterVar* getInterp();
    RewriterVar* allocArgs(const llvm::ArrayRef<Value> args);

    static Box* augbinopICHelper(AugBinopIC* ic, Box* lhs, Box* rhs, int op);
    static Box* binopICHelper(BinopIC* ic, Box* lhs, Box* rhs, int op);
    static Box* callattrHelper(Box* obj, BoxedString* attr, CallattrFlags flags, ArgPassSpec argspec, Box** args,
                               std::vector<BoxedString*>* keyword_names);
    static Box* compareICHelper(CompareIC* ic, Box* lhs, Box* rhs, int op);
    static Box* createListHelper(uint64_t num, Box** data);
    static Box* createTupleHelper(uint64_t num, Box** data);
    static Box* exceptionMatchesHelper(Box* obj, Box* cls);
    static Box* getAttrICHelper(GetAttrIC* ic, Box* o, BoxedString* attr);
    static Box* getGlobalICHelper(GetGlobalIC* ic, Box* o, BoxedString* s);
    static Box* getitemICHelper(GetItemIC* ic, Box* o, Box* attr);
    static Box* hasnextHelper(Box* b);
    static Box* nonzeroHelper(Box* b);
    static Box* notHelper(Box* b);
    static Box* runtimeCallHelper(Box* obj, ArgPassSpec argspec, Box** args, std::vector<BoxedString*>* keyword_names);
    static Box* setAttrICHelper(SetAttrIC* ic, Box* o, BoxedString* attr, Box* value);
    static Box* setGlobalICHelper(SetGlobalIC* ic, Box* o, BoxedString* s, Box* v);
    static Box* setitemICHelper(SetItemIC* ic, Box* o, Box* attr, Box* value);
    static Box* unaryopICHelper(UnaryopIC* ic, Box* obj, int op);

#if ENABLE_TRACING_IC
    static Box* callattrHelperIC(Box* obj, BoxedString* attr, CallattrFlags flags, ArgPassSpec argspec, CallattrIC* ic,
                                 Box** args);
    static Box* runtimeCallHelperIC(Box* obj, ArgPassSpec argspec, RuntimeCallIC* ic, Box** args);
#endif

    void _emitSideExit(RewriterVar* var, RewriterVar* val_constant, RewriterVar* false_path);
    void _emitJump(CFGBlock* b, RewriterVar* block_next);
    void _emitReturn(RewriterVar* v, RewriterVar* next);
    void _emitOSRPoint(RewriterVar* result, RewriterVar* node_var);
};
}

#endif
