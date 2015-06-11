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


class JitFragment : public Rewriter {
private:
    CFGBlock* block;
    int code_offset;
    int epilog_offset;
    int continue_jmp_offset;
    void* entry_code;
    bool finished;
    bool& iscurrently_tracing;
    RewriterVar* interp;
    std::function<void(int)> commited_callback;

public:
    JitFragment(CFGBlock* block, ICSlotRewrite* rewrite, int code_offset, int epilog_offset, void* entry_code,
                bool& iscurrently_tracing, std::function<void(int)> commited_callback);

    llvm::DenseMap<InternedString, RewriterVar*> local_syms;

    CFGBlock* getBlock();

    uint64_t asArg(InternedString s);

    RewriterVar* getInterp();

    static Box* setGlobalICHelper(SetGlobalIC* ic, Box* o, BoxedString* s, Box* v);

    RewriterVar* Imm(void* val);

    RewriterVar* Imm(uint64_t val);


    void emitSetGlobal(Box* global, BoxedString* s, Value v);

    static Box* getGlobalICHelper(GetGlobalIC* ic, Box* o, BoxedString* s);

    RewriterVar* emitGetGlobal(Box* global, BoxedString* s);

    static Box* setAttrICHelper(SetAttrIC* ic, Box* o, BoxedString* attr, Box* value);

    void emitSetAttr(Value obj, BoxedString* s, Value attr);

    static Box* getAttrICHelper(GetAttrIC* ic, Box* o, BoxedString* attr);

    RewriterVar* emitGetAttr(Value obj, BoxedString* s);

    RewriterVar* emitGetClsAttr(Value obj, BoxedString* s);

    RewriterVar* emitGetLocal(InternedString s);

    void emitSetLocal(InternedString s, bool set_closure, Value v);

    assembler::Indirect getLocalSym(uint64_t offset);

    void emitSetDeadLocal(InternedString s, Value v);

    RewriterVar* emitGetDeadLocal(InternedString s);

    RewriterVar* emitBoxedLocalsGet(BoxedString* s);

    static Box* setitemICHelper(SetItemIC* ic, Box* o, Box* attr, Box* value);

    void emitSetItem(Value target, Value slice, Value value);

    static Box* getitemICHelper(GetItemIC* ic, Box* o, Box* attr);
    RewriterVar* emitGetItem(Value value, Value slice);

    void emitSetItemName(BoxedString* s, Value v);

    RewriterVar* emitDeref(InternedString s);

    static BoxedTuple* createTupleHelper(uint64_t num, Box** data);

    RewriterVar* emitCreateTuple(const llvm::SmallVectorImpl<Value>& values);

    static BoxedList* createListHelper(uint64_t num, Box** data);

    RewriterVar* emitCreateList(const llvm::SmallVectorImpl<Value>& values);

    RewriterVar* emitCreateDict();
    RewriterVar* emitCreateSlice(Value start, Value stop, Value step);

    static Box* nonzeroHelper(Box* b);

    RewriterVar* emitNonzero(Value v);

    static Box* notHelper(Box* b);

    RewriterVar* emitNotNonzero(Value v);
    RewriterVar* emitGetPystonIter(Value v);
    RewriterVar* emitUnpackIntoArray(Value v, uint64_t num);

    static Box* compareICHelper(CompareIC* ic, Box* lhs, Box* rhs, int op);

    RewriterVar* emitCompare(Value lhs, Value rhs, int op_type);

    static Box* augbinopICHelper(AugBinopIC* ic, Box* lhs, Box* rhs, int op);
    RewriterVar* emitAugbinop(Value lhs, Value rhs, int op_type);

    static Box* binopICHelper(BinopIC* ic, Box* lhs, Box* rhs, int op);
    RewriterVar* emitBinop(Value lhs, Value rhs, int op_type);

    static Box* unaryopICHelper(UnaryopIC* ic, Box* obj, int op);
    RewriterVar* emitUnaryop(Value v, int op_type);

    static Box* exceptionMatchesHelper(Box* obj, Box* cls);
    RewriterVar* emitExceptionMatches(Value v, Value cls);
    void emitUncacheExcInfo();
    void emitSetExcInfo(Value type, Value value, Value traceback);
    RewriterVar* emitLandingpad();

    RewriterVar* emitYield(Value v);


    // TODO remove the gc hack...
    RewriterVar* emitInt(long n);
    RewriterVar* emitFloat(double n);
    RewriterVar* emitLong(llvm::StringRef s);
    RewriterVar* emitUnicodeStr(llvm::StringRef s);

#if ENABLE_TRACING_IC
    static Box* callattrHelperIC(Box* obj, BoxedString* attr, CallattrFlags flags, ArgPassSpec argspec, CallattrIC* ic,
                                 Box** args);
#endif
    static Box* callattrHelper(Box* obj, BoxedString* attr, CallattrFlags flags, ArgPassSpec argspec, Box** args,
                               std::vector<BoxedString*>* keyword_names);


    RewriterVar* emitCallattr(Value obj, BoxedString* attr, CallattrFlags flags, ArgPassSpec argspec,
                              std::vector<Value, StlCompatAllocator<Value>> args,
                              std::vector<BoxedString*>* keyword_names);


#if ENABLE_TRACING_IC
    static Box* runtimeCallHelperIC(Box* obj, ArgPassSpec argspec, RuntimeCallIC* ic, Box** args);
#endif
    static Box* runtimeCallHelper(Box* obj, ArgPassSpec argspec, Box** args, std::vector<BoxedString*>* keyword_names);


    RewriterVar* emitRuntimeCall(Value obj, ArgPassSpec argspec, std::vector<Value, StlCompatAllocator<Value>> args,
                                 std::vector<BoxedString*>* keyword_names);
    void _runtimeCall(RewriterVar* result, RewriterVar* obj, RewriterVar* argspec_var,
                      std::vector<Value, StlCompatAllocator<Value>> args, RewriterVar* keyword_names_var);

    static Box* hasnextHelper(Box* b);
    RewriterVar* emitHasnext(Value v);


    void emitSetCurrentInst(AST_stmt* node);
    void emitOSRPoint(AST_Jump* node);
    void _osrPoint(RewriterVar* result, RewriterVar* node_var);

    void addGuard(Value v, CFGBlock* cont);
    void _addGuard(RewriterVar* var, RewriterVar* val_constant, RewriterVar* false_path);
    void emitJump(CFGBlock* b);
    void _emitJump(CFGBlock* b, RewriterVar* block_next);
    void emitReturn(Value v);
    void _emitReturn(RewriterVar* v, RewriterVar* next);
    int compile();
    void abortTrace();
    int bytesWritten() { return assembler->bytesWritten(); }

    bool finishAssembly(ICSlotInfo* picked_slot, int continue_offset);
};

class JitedCode {
    static constexpr int code_size = 4096 * 12;
    static constexpr int epilog_size = 2;

    void* code;
    int entry_offset;
    int epilog_offset;
    assembler::Assembler a;
    bool iscurrently_tracing;

public:
    JitedCode(llvm::StringRef name);
    std::unique_ptr<JitFragment> newFragment(CFGBlock* block, int jump_offset = 0);
    void fragmentCommited(int size);
    static void writeTrivialEhFrame(void* eh_frame_addr, void* func_addr, uint64_t func_size);
    void EHwriteAndRegister(void* func_addr, uint64_t func_size);
};
}

#endif
