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

#include <memory>
#include <llvm/ADT/ArrayRef.h>

#include "asm_writing/rewriter.h"
#include "codegen/ast_interpreter.h"
#include "gc/heap.h"
#include "runtime/ics.h"

namespace pyston {

#define ENABLE_BASELINEJIT_ICS 1

class AST_stmt;
class Box;
class BoxedDict;
class BoxedList;
class BoxedTuple;

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
// To execute a specific CFGBlock one has to call:
//      CFGBlock* block;
//      block->entry_code(ast_interpreter_instance, block)
//
// Signature of a JitCodeBlock:
//  std::pair<CFGBlock*, Box*>(*entry_code)(ASTInterpreter* interp, CFGBlock* block)
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
//      push   %rbp                 ; setup frame pointer
//      mov    %rsp,%rbp
//      sub    $0x108,%rsp          ; setup scratch, 0x108 = scratch_size + 8 (=stack alignment)
//      push   %rdi                 ; save the pointer to ASTInterpreter instance
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
//      leave
//      ret                         ; exit to the interpreter which will interpret the specified CFGBLock*
//    end_side_exit:
//      ....

// second_JitFragment:
//      ...
//    ; this shows how a AST_Return looks like
//      mov    $0,%rax              ; rax contains the next block to interpret.
//                                    in this case 0 which means we are finished
//      movabs $0x1270014108,%rdx   ; rdx must contain the Box* value to return
//      leave
//      ret
//
// nth_JitFragment:
//      ...                         ; direct jump previous JITed block
//      jmp first_JitFragment
//
//
class JitCodeBlock {
private:
    static constexpr int scratch_size = 256 * 3;
    static constexpr int code_size = 4096 * 4;

    EHFrameManager frame_manager;
    std::unique_ptr<uint8_t[]> code;
    int entry_offset;
    assembler::Assembler a;
    bool is_currently_writing;
    bool asm_failed;

public:
    JitCodeBlock(llvm::StringRef name);

    std::unique_ptr<JitFragmentWriter> newFragment(CFGBlock* block, int patch_jump_offset = 0);
    bool shouldCreateNewBlock() const { return asm_failed || a.bytesLeft() < 128; }
    void fragmentAbort(bool not_enough_space);
    void fragmentFinished(int bytes_witten, int num_bytes_overlapping, void* next_fragment_start);
};

struct JitVar;
typedef std::shared_ptr<JitVar> JitVarPtr;

template <class T> class JitLocMap {
private:
    static const int N_REGS = assembler::Register::numRegs();
    static const int N_XMM = assembler::XMMRegister::numRegs();
    static const int N_SCRATCH = 256 / 8 * 3;
    static const int N_STACK = 16;

    T map_reg[N_REGS];
    T map_xmm[N_XMM];
    T map_scratch[N_SCRATCH];
    T map_stack[N_STACK];

public:
    JitLocMap() {
        memset(map_reg, 0, sizeof(map_reg));
        memset(map_xmm, 0, sizeof(map_xmm));
        memset(map_scratch, 0, sizeof(map_scratch));
        memset(map_stack, 0, sizeof(map_stack));
    }

    T& operator[](Location l) {
        switch (l.type) {
            case Location::Register:
                assert(0 <= l.regnum);
                assert(l.regnum < N_REGS);
                return map_reg[l.regnum];
            case Location::XMMRegister:
                assert(0 <= l.regnum);
                assert(l.regnum < N_XMM);
                return map_xmm[l.regnum];
            case Location::Stack:
                assert(0 <= l.stack_offset / 8);
                assert(l.stack_offset / 8 < N_STACK);
                return map_stack[l.stack_offset / 8];
            case Location::Scratch:
                assert(0 <= l.scratch_offset / 8);
                assert(l.scratch_offset / 8 < N_SCRATCH);
                return map_scratch[l.scratch_offset / 8];
            default:
                RELEASE_ASSERT(0, "%d", l.type);
        }
    };

    const T& operator[](Location l) const { return const_cast<T&>(*this)[l]; };

    size_t count(Location l) {
        if ((*this)[l].lock())
            return 1;
        return 0;
    }

    void erase(Location l) { (*this)[l].reset(); }
};

struct JitVar {
    assembler::Register reg_value;
    uint64_t const_value;
    int stack_offset;

    bool is_constant;
    bool is_in_reg;
    bool is_in_mem;


    static JitVarPtr createFromConst(uint64_t val) { return std::make_shared<JitVar>(val); }

    static JitVarPtr createFromReg(assembler::Register reg) { return std::make_shared<JitVar>(reg); }

    static JitVarPtr createFromStack(int stack_offset) {
        JitVarPtr var = std::make_shared<JitVar>(0);
        var->is_constant = false;
        var->is_in_reg = false;
        var->is_in_mem = true;
        var->stack_offset = stack_offset;
        return var;
    }



    JitVar(uint64_t v) : reg_value(0), const_value(v), is_constant(true), is_in_reg(false), is_in_mem(false) {}
    JitVar(assembler::Register reg)
        : reg_value(reg), const_value(0), is_constant(false), is_in_reg(true), is_in_mem(false) {}
};


class JitWriter {
public:
    JitWriter(assembler::Assembler* assembler, int scratch_size, int scratch_offset, void* slot_start)
        : assembler(assembler),
          scratch_size(scratch_size),
          scratch_offset(scratch_offset),
          slot_start(slot_start),
          failed(false) {}

    assembler::Assembler* assembler;
    JitLocMap<std::weak_ptr<JitVar>> locations;

    int scratch_size;
    int scratch_offset;
    void* slot_start;
    bool failed;

    assembler::Register getFreeReg() {
        static const assembler::Register allocatable_regs[] = {
            assembler::RAX, assembler::RCX, assembler::RDX, assembler::RDI, assembler::RSI,
            assembler::R8,  assembler::R9,  assembler::R10, assembler::R11,
        };

        for (auto&& reg : allocatable_regs) {
            Location l = reg;
            if (!locations.count(l))
                return reg;
        }

        assembler::Register reg = assembler::RSI;
        spill(locations[reg].lock());
        return reg;
    }

    assembler::Register getFreeReg(assembler::Register avoid_reg) {
        static const assembler::Register allocatable_regs[] = {
            assembler::RAX, assembler::RCX, assembler::RDX, assembler::RDI, assembler::RSI,
            assembler::R8,  assembler::R9,  assembler::R10, assembler::R11,
        };

        for (auto&& reg : allocatable_regs) {
            if (reg == avoid_reg)
                continue;
            Location l = reg;
            if (!locations.count(l))
                return reg;
        }

        assembler::Register reg = avoid_reg == assembler::RSI ? assembler::RDI : assembler::RSI;
        spill(locations[reg].lock());
        return reg;
    }

    assembler::Register getInReg(JitVarPtr var, Location loc = Location::any()) {
        if (loc == Location::any()) {
            if (var->is_in_reg)
                return var->reg_value;

            loc = getFreeReg();
        } else {
            if (var->is_in_reg && var->reg_value == loc.asRegister())
                return var->reg_value;

            if (locations.count(loc))
                spill(locations[loc].lock());
        }

        if (var->is_in_reg) {
            if (var->reg_value != loc.asRegister()) {
                assembler->mov(var->reg_value, loc.asRegister());
                locations[var->reg_value].reset();
                var->reg_value = loc.asRegister();
            }
            locations[loc] = var;
            return loc.asRegister();
        }
        if (var->is_constant) {
            assembler->mov(assembler::Immediate(var->const_value), loc.asRegister());
            var->is_in_reg = true;
            var->reg_value = loc.asRegister();
            locations[loc] = var;
            return loc.asRegister();
        }
        if (var->is_in_mem) {
            assembler->mov(assembler::Indirect(assembler::RSP, var->stack_offset), loc.asRegister());
            var->is_in_reg = true;
            var->reg_value = loc.asRegister();
            locations[loc] = var;
            return loc.asRegister();
        }
        printf("f: %p\n", var.get());
        fflush(stdout);
        RELEASE_ASSERT(0, "");
    }

    assembler::Register getInReg(JitVarPtr var, Location loc, assembler::Register avoid_reg) {
        assert(loc == Location::any() || loc.asRegister() != avoid_reg);
        if (loc == Location::any()) {
            if (var->is_in_reg && avoid_reg != var->reg_value)
                return var->reg_value;

            loc = getFreeReg(avoid_reg);
        } else {
            if (var->is_in_reg && var->reg_value == loc.asRegister())
                return var->reg_value;

            if (locations.count(loc))
                spill(locations[loc].lock());
        }
        if (var->is_in_reg) {
            if (var->reg_value != loc.asRegister()) {
                assembler->mov(var->reg_value, loc.asRegister());
                locations[var->reg_value].reset();
                var->reg_value = loc.asRegister();
            }
            locations[loc] = var;
            return loc.asRegister();
        }
        if (var->is_constant) {
            assembler->mov(assembler::Immediate(var->const_value), loc.asRegister());
            var->is_in_reg = true;
            var->reg_value = loc.asRegister();
            locations[loc] = var;
            return loc.asRegister();
        }
        if (var->is_in_mem) {
            assembler->mov(assembler::Indirect(assembler::RSP, var->stack_offset), loc.asRegister());
            var->is_in_reg = true;
            var->reg_value = loc.asRegister();
            locations[loc] = var;
            return loc.asRegister();
        }
        printf("f: %p\n", var.get());
        fflush(stdout);
        RELEASE_ASSERT(0, "");
    }

    Location allocScratch() {
        for (int i = 0; i < scratch_size; i += 8) {
            Location l(Location::Scratch, i);
            if (locations.count(l) == 0) {
                return l;
            }
        }
        RELEASE_ASSERT(0, "");
        failed = true;
        return Location(Location::None, 0);
    }

    JitVarPtr allocate(int num) {
        assert(num >= 1);
        int consec = 0;
        for (int i = 0; i < scratch_size; i += 8) {
            Location l(Location::Scratch, i);
            if (locations.count(l) == 0) {
                consec++;
                if (consec == num) {
                    int a = i / 8 - num + 1;
                    int b = i / 8;
                    // Put placeholders in so the array space doesn't get re-allocated.
                    // This won't get collected, but that's fine.
                    // Note: make sure to do this marking before the initializeInReg call
                    /*
                    for (int j = a; j <= b; j++) {
                        Location m(Location::Scratch, j * 8);
                        assert(locations.count(m) == 0);
                        //locations[m] = (JitVarPtr)0x1;
                        //locations[m] = JitVarPtr((JitVar*)0x1);
                    }*/

                    assembler::Register r = getFreeReg();
                    // TODO we could do something like we do for constants and only load
                    // this when necessary, so it won't spill. Is that worth?
                    assembler->lea(assembler::Indirect(assembler::RSP, 8 * a + scratch_offset), r);
                    auto val = JitVar::createFromReg(r);
                    locations[r] = val;

                    for (int j = a; j <= b; j++) {
                        Location m(Location::Scratch, j * 8);
                        assert(locations.count(m) == 0);
                        locations[m] = val;
                    }

                    return val;
                }
            } else {
                consec = 0;
            }
        }
        failed = true;
        return NULL;
    }

    void spill(JitVarPtr var) {
        if (var->is_in_mem || var->is_constant) {
            if (var->is_in_reg) {
                var->is_in_reg = false;
                locations[var->reg_value].reset();
            }
            return;
        }
        Location l = allocScratch();

        RELEASE_ASSERT(var->is_in_reg, "");
        assembler->mov(var->reg_value, assembler::Indirect(assembler::RSP, l.scratch_offset + scratch_offset));
        locations[l] = var;
        var->is_in_mem = true;
        var->stack_offset = l.scratch_offset + scratch_offset;
        var->is_in_reg = false;
        locations[var->reg_value].reset();
    }



    void voidCall(void* func, llvm::ArrayRef<JitVarPtr> args) {
        static const assembler::Register allocatable_regs[] = {
            assembler::RAX, assembler::RCX, assembler::RDX, assembler::RDI, assembler::RSI,
            assembler::R8,  assembler::R9,  assembler::R10, assembler::R11,
        };

        std::unordered_set<int> already_inplace_reg;

        for (int arg_num = 0; arg_num < args.size(); ++arg_num) {
            JitVarPtr arg = args[arg_num];
            if (arg->is_in_reg) {
                auto loc = Location::forArg(arg_num);
                assert(loc.type == Location::Register);

                // if (locations.count(loc))
                //    continue;

                // printf("moving %d -> %d\n", arg->reg_value.regnum, loc.asRegister().regnum);

                getInReg(arg, loc);
                if (!arg->is_in_mem) {
                    Location l = allocScratch();
                    assembler->mov(arg->reg_value,
                                   assembler::Indirect(assembler::RSP, l.scratch_offset + scratch_offset));
                    locations[l] = arg;
                    arg->is_in_mem = true;
                    arg->stack_offset = l.scratch_offset + scratch_offset;
                }
                already_inplace_reg.insert(loc.asRegister().regnum);
            }
        }

        for (auto&& reg : allocatable_regs) {
            Location l = reg;
            if (locations.count(l) && !already_inplace_reg.count(reg.regnum)) {
                spill(locations[l].lock());
            }
        }

        for (int arg_num = 0; arg_num < args.size(); ++arg_num) {
            Location loc = Location::forArg(arg_num);
            assert(loc.type == Location::Register);
            getInReg(args[arg_num], loc);
        }
        /*
                for (int arg_num = 0; arg_num < args.size(); ++arg_num) {
                    Location loc = Location::forArg(arg_num);
                    assert(loc.type == Location::Register);
                    getInReg(args[arg_num], loc);
                }
        */
        uint64_t asm_address = (uint64_t)assembler->curInstPointer() + 5;
        uint64_t real_asm_address = asm_address + (uint64_t)slot_start - (uint64_t)assembler->startAddr();
        int64_t offset = (int64_t)((uint64_t)func - real_asm_address);

        if (isLargeConstant(offset)) {
            assembler->mov(assembler::Immediate(func), assembler::R11);
            assembler->callq(assembler::R11);
        } else {
            assembler->call(assembler::Immediate(offset));
        }

        for (auto&& reg : allocatable_regs) {
            Location l = reg;
            if (locations.count(l)) {
                auto var = locations[l].lock();
                // assert(var->is_in_reg);
                var->is_in_reg = false;
                locations[l].reset();
            }
        }
    }

    JitVarPtr call(void* func, llvm::ArrayRef<JitVarPtr> args) {
        voidCall(func, args);
        auto rtn = JitVar::createFromReg(assembler::RAX);
        locations[assembler::RAX] = rtn;
        return rtn;
    }

    void setAttr(JitVarPtr var, int offset, JitVarPtr value) {
        assembler::Register reg = getInReg(var);
        if (value->is_in_reg)
            assembler->mov(value->reg_value, assembler::Indirect(reg, offset));
        else if (value->is_constant) {
            if ((uint32_t)value->const_value == value->const_value) {
                assembler->movq(assembler::Immediate(value->const_value), assembler::Indirect(reg, offset));
            } else {
                assembler::Register reg_tmp = getInReg(value, Location::any(), reg);
                assembler->mov(reg_tmp, assembler::Indirect(reg, offset));
            }
        } else if (value->is_in_mem || value->is_constant) {
            assembler::Register reg_tmp = getInReg(value, Location::any(), reg);
            assembler->mov(reg_tmp, assembler::Indirect(reg, offset));
        } else
            RELEASE_ASSERT(0, "");
    }

    JitVarPtr getAttr(JitVarPtr var, int offset) {
        assembler::Register reg = getInReg(var);
        assembler::Register reg_dst = getFreeReg(reg);
        assembler->mov(assembler::Indirect(reg, offset), reg_dst);
        auto rtn = JitVar::createFromReg(reg_dst);
        locations[reg_dst] = rtn;
        return rtn;
    }

    static bool isLargeConstant(int64_t val) { return (val < (-1L << 31) || val >= (1L << 31) - 1); }
};


class JitFragmentWriter : public ICSlotRewrite::CommitHook {
private:
    static constexpr int min_patch_size = 13;

    CFGBlock* block;
    int code_offset; // offset inside the JitCodeBlock to the start of this block

    // If the next block is not yet JITed we will set this field to the number of bytes we emitted for the exit to the
    // interpreter which continues interpreting the next block.
    // If we immediatelly start JITing the next block we will set 'num_bytes_overlapping' on the new fragment to this
    // value which will make the fragment start at the instruction where the last block is exiting to the interpreter to
    // interpret the new block -> we overwrite the exit with the code of the new block.
    // If there is nothing to overwrite this field will be 0.
    int num_bytes_exit;
    int num_bytes_overlapping; // num of bytes this block overlaps with the prev. used to patch unessary jumps

    void* entry_code; // JitCodeBlock start address. Must have an offset of 0 into the code block
    JitCodeBlock& code_block;
    JitVarPtr interp;
    llvm::DenseMap<InternedString, JitVarPtr> local_syms;
    std::unique_ptr<ICInfo> ic_info;

    // Optional points to a CFGBlock and a patch location which should get patched to a direct jump if
    // the specified block gets JITed. The patch location is guaranteed to be at least 'min_patch_size' bytes long.
    // We can't directly mark the offset for patching because JITing the current fragment may fail. That's why we store
    // it in this field and process it only when we know we successfully generated the code.
    std::pair<CFGBlock*, int /* offset from fragment start*/> side_exit_patch_location;

    JitWriter writer;
    assembler::Assembler* assembler;
    std::unique_ptr<ICSlotRewrite> rewrite;

public:
    JitFragmentWriter(CFGBlock* block, std::unique_ptr<ICInfo> ic_info, std::unique_ptr<ICSlotRewrite> rewrite,
                      int code_offset, int num_bytes_overlapping, void* entry_code, JitCodeBlock& code_block);

    JitVarPtr imm(uint64_t val);
    JitVarPtr imm(void* val);


    JitVarPtr emitAugbinop(JitVarPtr lhs, JitVarPtr rhs, int op_type);
    JitVarPtr emitBinop(JitVarPtr lhs, JitVarPtr rhs, int op_type);
    JitVarPtr emitCallattr(JitVarPtr obj, BoxedString* attr, CallattrFlags flags, const llvm::ArrayRef<JitVarPtr> args,
                           std::vector<BoxedString*>* keyword_names);
    JitVarPtr emitCompare(JitVarPtr lhs, JitVarPtr rhs, int op_type);
    JitVarPtr emitCreateDict(const llvm::ArrayRef<JitVarPtr> keys, const llvm::ArrayRef<JitVarPtr> values);
    JitVarPtr emitCreateList(const llvm::ArrayRef<JitVarPtr> values);
    JitVarPtr emitCreateSet(const llvm::ArrayRef<JitVarPtr> values);
    JitVarPtr emitCreateSlice(JitVarPtr start, JitVarPtr stop, JitVarPtr step);
    JitVarPtr emitCreateTuple(const llvm::ArrayRef<JitVarPtr> values);
    JitVarPtr emitDeref(InternedString s);
    JitVarPtr emitExceptionMatches(JitVarPtr v, JitVarPtr cls);
    JitVarPtr emitGetAttr(JitVarPtr obj, BoxedString* s);
    JitVarPtr emitGetBlockLocal(InternedString s);
    JitVarPtr emitGetBoxedLocal(BoxedString* s);
    JitVarPtr emitGetBoxedLocals();
    JitVarPtr emitGetClsAttr(JitVarPtr obj, BoxedString* s);
    JitVarPtr emitGetGlobal(Box* global, BoxedString* s);
    JitVarPtr emitGetItem(JitVarPtr value, JitVarPtr slice);
    JitVarPtr emitGetLocal(InternedString s);
    JitVarPtr emitGetPystonIter(JitVarPtr v);
    JitVarPtr emitHasnext(JitVarPtr v);
    JitVarPtr emitLandingpad();
    JitVarPtr emitNonzero(JitVarPtr v);
    JitVarPtr emitNotNonzero(JitVarPtr v);
    JitVarPtr emitRepr(JitVarPtr v);
    JitVarPtr emitRuntimeCall(JitVarPtr obj, ArgPassSpec argspec, const llvm::ArrayRef<JitVarPtr> args,
                              std::vector<BoxedString*>* keyword_names);
    JitVarPtr emitUnaryop(JitVarPtr v, int op_type);
    JitVarPtr emitUnpackIntoArray(JitVarPtr v, uint64_t num);
    JitVarPtr emitYield(JitVarPtr v);

    void emitExec(JitVarPtr code, JitVarPtr globals, JitVarPtr locals, FutureFlags flags);
    void emitJump(CFGBlock* b);
    void emitOSRPoint(AST_Jump* node);
    void emitPrint(JitVarPtr dest, JitVarPtr var, bool nl);
    void emitRaise0();
    void emitRaise3(JitVarPtr arg0, JitVarPtr arg1, JitVarPtr arg2);
    void emitReturn(JitVarPtr v);
    void emitSetAttr(JitVarPtr obj, BoxedString* s, JitVarPtr attr);
    void emitSetBlockLocal(InternedString s, JitVarPtr v);
    void emitSetCurrentInst(AST_stmt* node);
    void emitSetExcInfo(JitVarPtr type, JitVarPtr value, JitVarPtr traceback);
    void emitSetGlobal(Box* global, BoxedString* s, JitVarPtr v);
    void emitSetItemName(BoxedString* s, JitVarPtr v);
    void emitSetItem(JitVarPtr target, JitVarPtr slice, JitVarPtr value);
    void emitSetLocal(InternedString s, bool set_closure, JitVarPtr v);
    void emitSideExit(JitVarPtr v, Box* cmp_value, CFGBlock* next_block);
    void emitUncacheExcInfo();

    void abortCompilation();
    int finishCompilation();

    bool finishAssembly(int continue_offset) override;

    JitVarPtr getAttr(JitVarPtr var, int offset) { return writer.getAttr(var, offset); }
    JitVarPtr call(void* func, llvm::ArrayRef<JitVarPtr> args = std::vector<JitVarPtr>()) {
        return writer.call(func, args);
    }
    void voidCall(void* func, llvm::ArrayRef<JitVarPtr> args = std::vector<JitVarPtr>()) {
        return writer.voidCall(func, args);
    }

private:
    JitVarPtr allocArgs(const llvm::ArrayRef<JitVarPtr> args);
#ifndef NDEBUG
    std::pair<uint64_t, uint64_t> asUInt(InternedString s);
#else
    uint64_t asUInt(InternedString s);
#endif
    JitVarPtr getInterp();

    static Box* augbinopICHelper(AugBinopIC* ic, Box* lhs, Box* rhs, int op);
    static Box* binopICHelper(BinopIC* ic, Box* lhs, Box* rhs, int op);
    static Box* callattrHelper(Box* obj, BoxedString* attr, CallattrFlags flags, Box** args,
                               std::vector<BoxedString*>* keyword_names);
    static Box* compareICHelper(CompareIC* ic, Box* lhs, Box* rhs, int op);
    static Box* createDictHelper(uint64_t num, Box** keys, Box** values);
    static Box* createListHelper(uint64_t num, Box** data);
    static Box* createSetHelper(uint64_t num, Box** data);
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

#if ENABLE_BASELINEJIT_ICS
    static Box* callattrHelperIC(Box* obj, BoxedString* attr, CallattrFlags flags, CallattrIC* ic, Box** args);
    static Box* runtimeCallHelperIC(Box* obj, ArgPassSpec argspec, RuntimeCallIC* ic, Box** args);
#endif

    void _emitJump(CFGBlock* b, JitVarPtr block_next, int& size_of_exit_to_interp);



    static bool isLargeConstant(int64_t val) { return (val < (-1L << 31) || val >= (1L << 31) - 1); }
};
}

#endif
