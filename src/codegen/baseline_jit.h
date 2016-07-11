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

#ifndef PYSTON_CODEGEN_BASELINEJIT_H
#define PYSTON_CODEGEN_BASELINEJIT_H

#include <llvm/ADT/ArrayRef.h>

#include "asm_writing/rewriter.h"
#include "codegen/ast_interpreter.h"
#include "codegen/patchpoints.h"

namespace pyston {

// passes MAP_32BIT to mmap when allocating the memory for the bjit code.
// it's nice for inspecting the generated asm because the debugger is able to show the name of called C/C++ functions
#define ENABLE_BASELINEJIT_MAP_32BIT 1
#define ENABLE_BASELINEJIT_ICS 1

class AST_stmt;
class Box;
class BoxedDict;
class BoxedList;
class BoxedTuple;

class TypeRecorder;

class JitFragmentWriter;

// This JIT tier is designed as Pystons entry level JIT tier (executed after a only a few dozens runs of a basic block)
// which emits code very quick. It does not do any type specializations but can use inline caches.
// It operates on a basic block at a time (=CFGBLock*) and supports very fast switching between the
// interpreter and the JITed code on every block start/end.
//
// To archive this it's tightly integrated with the AST Interpreter and always operates on an ASTInterpreter instance.
// The process works like this:
//  - in the ASTInterpreter main loop we will check on every basic block start if we have already machine code for the
//  basic block
//  - if we have, we will directly call it (='entry_code' pointer inside the CFGBlock)
//  - if we don't have but determined that the block is hot (loop backedge threshold reached or function got called
//      often enough) and and we haven't yet tried to JIT it we will start with the JITing process:
//  - create/reuse a JitCodeBlock for the function
//  - create a new JitFragmentWriter for the basic block to JIT
//  - interpret the basic block and in addition call into corresponding emit* functions of the JitFragmentWriter on
//      every AST node encountered.
//  - if a node is encountered which is not supported, abort JITing of the block and blacklist this block
//  - if we reached the control flow changing node of the basic block (e.g. a branch, return or jump node) we finish
//     JITing the block.
//
// A JITed block is allowed to jump directly to another JITed basic block or if the block is not yet JITed (or we are
// unable to JIT it) we return from the function and the interpreter will immediatelly continue interpreting the next
// block.

// JitCodeBlock manages a fixed size memory block which stores JITed code.
// It can contain a variable number of blocks generated by JitFragmentWriter instances.
// Currently a JitFragment always contains the code of a single CFGBlock*.
// A JitFragment can get called from the Interpreter by calling 'entry_code' which will jump to the fragment start or
// it can get executed by a jump from another fragment.
// At every fragment end we can jump to another fragment or exit to the interpreter.
// This means we are not allowed to assume that a register contains a specific value between JitFragments.
// This also means that we are allowed to store a Python variable which only lives in the current CFGBLock* inside a
// register or stack slot but we aren't if it outlives the block - we have to store it in the interpreter instance.
//
// We use the following callee-save regs to speed up the generated code:
//      rbx, rbp, r12, r15: temporary values
//      r13               : pointer to ASTInterpreter instance
//      r14               : pointer to the vregs array
//
// To execute a specific CFGBlock one has to call:
//      CFGBlock* block;
//      block->entry_code(ast_interpreter_instance, block, ast_interpreter_instance->vregs)
//
// Signature of a JitCodeBlock:
//  std::pair<CFGBlock*, Box*>(*entry_code)(ASTInterpreter* interp, CFGBlock* block, Box** vregs)
//  args:
//      interp:  instance to the ASTInterpreter
//      block:   block to execute
//
//  return value:
//      first:   next block to execute in the interpreter
//                  if 0, we executed a return node and should return second
//      second:  return value only used if first == 0
//
// Basic layout of generated code block is:
// entry_code:
//      push   %rbp                 ; save rbp
//      push   %r15                 ; save r15
//      push   %r14                 ; save r14
//      push   %r13                 ; save r13
//      push   %r12                 ; save r12
//      push   %rbx                 ; save rbx
//      sub    $0x118,%rsp          ; setup scratch, 0x118 = scratch_size + 16 = space for two func args passed on the
//                                                                               stack + 8 byte for stack alignment
//      mov    %rdi,%r13            ; copy the pointer to ASTInterpreter instance into r13
//      mov    %rdx,%r14            ; copy the pointer to the vregs array into r14
//      jmpq   *0x8(%rsi)           ; jump to block->code
//                                      possible values: first_JitFragment, second_JitFragment,...
//
// first_JitFragment:
//      ...
//    ; Side exit (e.g. if <val> <block1> else <block2>
//      movabs $0x1270014108,%rcx   ; rcx = True
//      cmp    %rax,%rcx            ; rax == True
//      jne    end_side_exit
//      movabs $0x215bb60,%rax      ; rax = CFGBlock* to interpret next (rax is the 1. return reg)
//      add    $0x118,%rsp          ; restore stack pointer
//      pop    %rbx                 ; restore rbx
//      pop    %r12                 ; restore r12
//      pop    %r13                 ; restore r13
//      pop    %r14                 ; restore r14
//      pop    %r15                 ; restore r15
//      pop    %rbp                 ; restore rbp
//      ret                         ; exit to the interpreter which will interpret the specified CFGBLock*
//    end_side_exit:
//      ....

// second_JitFragment:
//      ...
//    ; this shows how a AST_Return looks like
//      xor    %eax,%eax            ; rax contains the next block to interpret.
//                                    in this case 0 which means we are finished
//      movabs $0x1270014108,%rdx   ; rdx must contain the Box* value to return
//      add    $0x118,%rsp          ; restore stack pointer
//      pop    %rbx                 ; restore rbx
//      pop    %r12                 ; restore r12
//      pop    %r13                 ; restore r13
//      pop    %r14                 ; restore r14
//      pop    %r15                 ; restore r15
//      pop    %rbp                 ; restore rbp
//      ret
//
// nth_JitFragment:
//      ...                         ; direct jump previous JITed block
//      jmp first_JitFragment
//
//
class JitCodeBlock {
public:
    static constexpr int scratch_size = 256;
    static constexpr int memory_size = 32768; // must fit the EH frame + generated code
    static constexpr int num_stack_args = 2;

    // scratch size + space for passing additional args on the stack without having to adjust the SP when calling
    // functions with more than 6 args.
    static constexpr int sp_adjustment = scratch_size + num_stack_args * 8 + 8 /* = alignment */;
    static constexpr assembler::RegisterSet additional_regs = assembler::RBX | assembler::RBP | assembler::R12
                                                              | assembler::R15;

private:
    struct MemoryManager {
    private:
        uint8_t* addr;

    public:
        MemoryManager();
        ~MemoryManager();
        uint8_t* get() { return addr; }
    };

    // the memory block contains the EH frame directly followed by the generated machine code.
    MemoryManager memory;
    int entry_offset;
    assembler::Assembler a;
    bool is_currently_writing;
    bool asm_failed;
    // this contains all the decref infos the bjit generated inside the memory block,
    // this allows us to deregister them when we release the code
    std::vector<DecrefInfo> decref_infos;

public:
    JitCodeBlock(llvm::StringRef name);

    std::unique_ptr<JitFragmentWriter> newFragment(CFGBlock* block, int patch_jump_offset,
                                                   llvm::DenseSet<int> known_non_null_vregs);
    bool shouldCreateNewBlock() const { return asm_failed || a.bytesLeft() < 128; }
    void fragmentAbort(bool not_enough_space);
    void fragmentFinished(int bytes_witten, int num_bytes_overlapping, void* next_fragment_start, ICInfo& ic_info);
};

class JitFragmentWriter : public Rewriter {
private:
    struct ExitInfo {
        int num_bytes;    // the number of bytes for the overwriteable jump
        void* exit_start; // where that jump starts

        ExitInfo() : num_bytes(0), exit_start(NULL) {}
    };

    static constexpr int min_patch_size = 13;

    CFGBlock* block;
    int code_offset; // offset inside the JitCodeBlock to the start of this block

    // If the next block is not yet JITed we will set this field to the number of bytes we emitted for the exit to the
    // interpreter which continues interpreting the next block.
    // If we immediatelly start JITing the next block we will set 'num_bytes_overlapping' on the new fragment to this
    // value which will make the fragment start at the instruction where the last block is exiting to the interpreter to
    // interpret the new block -> we overwrite the exit with the code of the new block.
    // If there is nothing to overwrite this field will be 0.
    ExitInfo exit_info;
    int num_bytes_overlapping; // num of bytes this block overlaps with the prev. used to patch unessary jumps

    void* entry_code; // JitCodeBlock start address. Must have an offset of 0 into the code block
    JitCodeBlock& code_block;
    RewriterVar* interp;
    RewriterVar* vregs_array;
    llvm::DenseMap<InternedString, RewriterVar*> local_syms;
    // keeps track which non block local vregs are known to have a non NULL value
    llvm::DenseSet<int> known_non_null_vregs;
    std::unique_ptr<ICInfo> ic_info;
    llvm::SmallPtrSet<RewriterVar*, 4> var_is_a_python_bool;

    // Optional points to a CFGBlock and a patch location which should get patched to a direct jump if
    // the specified block gets JITed. The patch location is guaranteed to be at least 'min_patch_size' bytes long.
    // We can't directly mark the offset for patching because JITing the current fragment may fail. That's why we store
    // it in this field and process it only when we know we successfully generated the code.
    std::pair<CFGBlock*, int /* offset from fragment start*/> side_exit_patch_location;

    struct PPInfo {
        void* func_addr;
        uint8_t* start_addr;
        uint8_t* end_addr;
        std::unique_ptr<ICSetupInfo> ic;
        StackInfo stack_info;
        AST* node;
        std::vector<Location> decref_infos;
    };

    llvm::SmallVector<PPInfo, 8> pp_infos;

public:
    JitFragmentWriter(CFGBlock* block, std::unique_ptr<ICInfo> ic_info, std::unique_ptr<ICSlotRewrite> rewrite,
                      int code_offset, int num_bytes_overlapping, void* entry_code, JitCodeBlock& code_block,
                      llvm::DenseSet<int> known_non_null_vregs);

    RewriterVar* getInterp();
    RewriterVar* imm(uint64_t val);
    RewriterVar* imm(void* val);

    RewriterVar* emitAugbinop(AST_expr* node, RewriterVar* lhs, RewriterVar* rhs, int op_type);
    RewriterVar* emitApplySlice(RewriterVar* target, RewriterVar* lower, RewriterVar* upper);
    RewriterVar* emitBinop(AST_expr* node, RewriterVar* lhs, RewriterVar* rhs, int op_type);
    RewriterVar* emitCallattr(AST_expr* node, RewriterVar* obj, BoxedString* attr, CallattrFlags flags,
                              const llvm::ArrayRef<RewriterVar*> args, std::vector<BoxedString*>* keyword_names);
    RewriterVar* emitCompare(AST_expr* node, RewriterVar* lhs, RewriterVar* rhs, int op_type);
    RewriterVar* emitCreateDict();
    void emitDictSet(RewriterVar* dict, RewriterVar* k, RewriterVar* v);
    RewriterVar* emitCreateList(const llvm::ArrayRef<STOLEN(RewriterVar*)> values);
    RewriterVar* emitCreateSet(const llvm::ArrayRef<RewriterVar*> values);
    RewriterVar* emitCreateSlice(RewriterVar* start, RewriterVar* stop, RewriterVar* step);
    RewriterVar* emitCreateTuple(const llvm::ArrayRef<RewriterVar*> values);
    RewriterVar* emitDeref(InternedString s);
    RewriterVar* emitExceptionMatches(RewriterVar* v, RewriterVar* cls);
    RewriterVar* emitGetAttr(RewriterVar* obj, BoxedString* s, AST_expr* node);
    RewriterVar* emitGetBlockLocal(InternedString s, int vreg);
    void emitKillTemporary(InternedString s, int vreg);
    RewriterVar* emitGetBoxedLocal(BoxedString* s);
    RewriterVar* emitGetBoxedLocals();
    RewriterVar* emitGetClsAttr(RewriterVar* obj, BoxedString* s);
    RewriterVar* emitGetGlobal(BoxedString* s);
    RewriterVar* emitGetItem(AST_expr* node, RewriterVar* value, RewriterVar* slice);
    RewriterVar* emitGetLocal(InternedString s, int vreg);
    RewriterVar* emitGetPystonIter(RewriterVar* v);
    RewriterVar* emitHasnext(RewriterVar* v);
    RewriterVar* emitImportFrom(RewriterVar* module, BoxedString* name);
    RewriterVar* emitImportName(int level, RewriterVar* from_imports, llvm::StringRef module_name);
    RewriterVar* emitImportStar(RewriterVar* module);
    RewriterVar* emitLandingpad();
    RewriterVar* emitNonzero(RewriterVar* v);
    RewriterVar* emitNotNonzero(RewriterVar* v);
    RewriterVar* emitRepr(RewriterVar* v);
    RewriterVar* emitRuntimeCall(AST_expr* node, RewriterVar* obj, ArgPassSpec argspec,
                                 const llvm::ArrayRef<RewriterVar*> args, std::vector<BoxedString*>* keyword_names);
    RewriterVar* emitUnaryop(RewriterVar* v, int op_type);
    std::vector<RewriterVar*> emitUnpackIntoArray(RewriterVar* v, uint64_t num);
    RewriterVar* emitYield(RewriterVar* v);

    void emitAssignSlice(RewriterVar* target, RewriterVar* lower, RewriterVar* upper, RewriterVar* value);
    void emitDelAttr(RewriterVar* target, BoxedString* attr);
    void emitDelGlobal(BoxedString* name);
    void emitDelItem(RewriterVar* target, RewriterVar* slice);
    void emitDelName(InternedString name);
    void emitExec(RewriterVar* code, RewriterVar* globals, RewriterVar* locals, FutureFlags flags);
    void emitJump(CFGBlock* b);
    void emitOSRPoint(AST_Jump* node);
    void emitPendingCallsCheck();
    void emitPrint(RewriterVar* dest, RewriterVar* var, bool nl);
    void emitRaise0();
    void emitRaise3(RewriterVar* arg0, RewriterVar* arg1, RewriterVar* arg2);
    void emitReturn(RewriterVar* v);
    void emitSetAttr(AST_expr* node, RewriterVar* obj, BoxedString* s, STOLEN(RewriterVar*) attr);
    void emitSetBlockLocal(InternedString s, int vreg, STOLEN(RewriterVar*) v);
    void emitSetCurrentInst(AST_stmt* node);
    void emitSetExcInfo(RewriterVar* type, RewriterVar* value, RewriterVar* traceback);
    void emitSetGlobal(BoxedString* s, STOLEN(RewriterVar*) v, bool are_globals_from_module);
    void emitSetItemName(BoxedString* s, RewriterVar* v);
    void emitSetItem(RewriterVar* target, RewriterVar* slice, RewriterVar* value);
    void emitSetLocal(InternedString s, int vreg, bool set_closure, STOLEN(RewriterVar*) v);
    // emitSideExit steals a full ref from v, not just a vref
    void emitSideExit(STOLEN(RewriterVar*) v, Box* cmp_value, CFGBlock* next_block);
    void emitUncacheExcInfo();

    void abortCompilation();
    // returns pair of the number of bytes for the overwriteable jump and known non null vregs at end of current block
    std::pair<int, llvm::DenseSet<int>> finishCompilation();

    bool finishAssembly(int continue_offset, bool& should_fill_with_nops, bool& variable_size_slots) override;

private:
    RewriterVar* allocArgs(const llvm::ArrayRef<RewriterVar*> args, RewriterVar::SetattrType);
#ifndef NDEBUG
    std::pair<uint64_t, uint64_t> asUInt(InternedString s);
#else
    uint64_t asUInt(InternedString s);
#endif


    // use this function when one emits a call where one argument is variable created with allocArgs(vars).
    // it let's one specify the additional uses the call has which are unknown to the rewriter because it is hidden in
    // the allocArgs call.
    RewriterVar* emitCallWithAllocatedArgs(void* func_addr, const llvm::ArrayRef<RewriterVar*> args,
                                           const llvm::ArrayRef<RewriterVar*> additional_uses);
    std::pair<RewriterVar*, RewriterAction*> emitPPCall(void* func_addr, llvm::ArrayRef<RewriterVar*> args,
                                                        unsigned short pp_size, AST* ast_node = NULL,
                                                        TypeRecorder* type_recorder = NULL,
                                                        llvm::ArrayRef<RewriterVar*> additional_uses = {});

    static void assertNameDefinedHelper(const char* id);
    static Box* callattrHelper(Box* obj, BoxedString* attr, CallattrFlags flags, TypeRecorder* type_recorder,
                               Box** args, std::vector<BoxedString*>* keyword_names);
    static Box* createDictHelper(uint64_t num, Box** keys, Box** values);
    static Box* createListHelper(uint64_t num, Box** data);
    static Box* createSetHelper(uint64_t num, Box** data);
    static Box* createTupleHelper(uint64_t num, Box** data);
    static Box* exceptionMatchesHelper(Box* obj, Box* cls);
    static BORROWED(Box*) hasnextHelper(Box* b);
    static BORROWED(Box*) nonzeroHelper(Box* b);
    static BORROWED(Box*) notHelper(Box* b);
    static Box* runtimeCallHelper(Box* obj, ArgPassSpec argspec, TypeRecorder* type_recorder, Box** args,
                                  std::vector<BoxedString*>* keyword_names);

    void _emitGetLocal(RewriterVar* val_var, const char* name);
    void _emitJump(CFGBlock* b, RewriterVar* block_next, ExitInfo& exit_info);
    void _emitOSRPoint();
    void _emitPPCall(RewriterVar* result, void* func_addr, llvm::ArrayRef<RewriterVar*> args, unsigned short pp_size,
                     AST* ast_node, llvm::ArrayRef<RewriterVar*> vars_to_bump);
    void _emitRecordType(RewriterVar* type_recorder_var, RewriterVar* obj_cls_var);
    void _emitReturn(RewriterVar* v);
    void _emitSideExit(STOLEN(RewriterVar*) var, RewriterVar* val_constant, CFGBlock* next_block,
                       RewriterVar* false_path);
};
}

#endif
