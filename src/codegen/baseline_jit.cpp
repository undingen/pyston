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

#include "codegen/baseline_jit.h"

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/StringMap.h>
#include <unordered_map>

#include "analysis/function_analysis.h"
#include "analysis/scoping_analysis.h"
#include "asm_writing/assembler.h"
#include "asm_writing/icinfo.h"
#include "codegen/codegen.h"
#include "codegen/compvars.h"
#include "codegen/irgen.h"
#include "codegen/irgen/hooks.h"
#include "codegen/irgen/irgenerator.h"
#include "codegen/irgen/util.h"
#include "codegen/memmgr.h"
#include "codegen/osrentry.h"
#include "codegen/unwinding.h"
#include "core/ast.h"
#include "core/cfg.h"
#include "core/common.h"
#include "core/contiguous_map.h"
#include "core/stats.h"
#include "core/thread_utils.h"
#include "core/util.h"
#include "runtime/generator.h"
#include "runtime/ics.h"
#include "runtime/import.h"
#include "runtime/inline/boxing.h"
#include "runtime/long.h"
#include "runtime/objmodel.h"
#include "runtime/set.h"
#include "runtime/types.h"

#ifndef NDEBUG
#define DEBUG 1
#else
#define DEBUG 0
#endif

namespace pyston {

static llvm::DenseSet<CFGBlock*> tracers_aborted;

#if 1
static const char _eh_frame_template[] =
    // CIE
    "\x14\x00\x00\x00" // size of the CIE
    "\x00\x00\x00\x00" // specifies this is an CIE
    "\x03"             // version number
    "\x7a\x52\x00"     // augmentation string "zR"
    "\x01\x78\x10"     // code factor 1, data factor -8, return address 16
    "\x01\x1b"         // augmentation data: 1b (CIE pointers as 4-byte-signed pcrel values)
    "\x0c\x07\x08\x90\x01\x00\x00"
    // Instructions:
    // - DW_CFA_def_cfa: r7 (rsp) ofs 8
    // - DW_CFA_offset: r16 (rip) at cfa-8
    // - nop, nop

    // FDE:
    "\x1c\x00\x00\x00" // size of the FDE
    "\x1c\x00\x00\x00" // offset to the CIE
    "\x00\x00\x00\x00" // prcel offset to function address [to be filled in]
    "\x10\x00\x00\x00" // function size [to be filled in]
    "\x00"             // augmentation data (none)
    "\x41\x0e\x10\x86\x02\x43\x0d\x06"
    // Instructions:
    // - DW_CFA_advance_loc: 1 to 00000001
    // - DW_CFA_def_cfa_offset: 16
    // - DW_CFA_offset: r6 (rbp) at cfa-16
    // - DW_CFA_advance_loc: 3 to 00000004
    // - DW_CFA_def_cfa_register: r6 (rbp)
    // - nops
    "\x00\x00\x00\x00\x00\x00\x00" // padding

    "\x00\x00\x00\x00" // terminator
    ;
#else
static const char _eh_frame_template[] =
#if 0
    /*00*/ "\x14\x00\x00\x00" "\x00\x00\x00\x00" "\x01\x7a\x52\x00" "\x01\x78\x10\x01"  //.........zR..x..
    /*10*/ "\x1b\x0c\x07\x08" "\x90\x01\x00\x00" "\x1c\x00\x00\x00" "\x1c\x00\x00\x00"  //................
    /*20*/ "\x00\x00\x00\x00" "\x5a\x00\x00\x00" "\x00\x41\x0e\x10" "\x86\x02\x43\x0d"  //....Z....A....C.
    /*30*/ "\x06\x02\x55\x0c" "\x07\x08\x00\x00"                                        //..U.....
    ;
#else
    /*00*/ "\x14\x00\x00\x00"
           "\x00\x00\x00\x00"
           "\x01\x7a\x52\x00"
           "\x01\x78\x10\x01" //.........zR..x..
           /*10*/ "\x1b\x0c\x07\x08"
           "\x90\x01\x00\x00"
           "\x1c\x00\x00\x00"
           "\x1c\x00\x00\x00" //................
           /*20*/ "\x00\x00\x00\x00"
           "\x5a\x00\x00\x00"
           "\x00\x41\x0e\x10"
           "\x86\x02\x43\x0d" //....Z....A....C.
           /*30*/ "\x06\x02\x55\x0c"
           "\x07\x08\x00\x00" //..U.....
           "\x00\x00\x00\x00";
#endif
#endif

#define EH_FRAME_SIZE (sizeof(_eh_frame_template) - 1) // omit string-terminating null byte



JitFragment::JitFragment(CFGBlock* block, ICSlotRewrite* rewrite, int code_offset, int epilog_offset, void* entry_code,
                         bool& iscurrently_tracing, std::function<void(int)> commited_callback)
    : Rewriter(rewrite, 0, {}),
      block(block),
      code_offset(code_offset),
      epilog_offset(epilog_offset),
      continue_jmp_offset(0),
      entry_code(entry_code),
      iscurrently_tracing(iscurrently_tracing),
      interp(0),
      commited_callback(commited_callback) {
    interp = createNewVar();
    addLocationToVar(interp, Location(Location::Stack, 0));
    interp->setAttr(ASTInterpreterJitInterface::getCurrentBlockOffset(), imm(block));
}

#ifndef NDEBUG
std::pair<uint64_t, uint64_t> JitFragment::asUInt(InternedString s) {
    static_assert(sizeof(InternedString) == sizeof(uint64_t) * 2, "");
    union U {
        U(InternedString is) : is(is) {}
        InternedString is;
        uint64_t u[2];
    } u(s);
    return std::make_pair(u.u[0], u.u[1]);
}
#else
uint64_t JitFragment::asUInt(InternedString s) {
    static_assert(sizeof(InternedString) == sizeof(uint64_t), "");
    union U {
        U(InternedString is) : is(is) {}
        InternedString is;
        uint64_t u;
    } u(s);
    return u.u;
}
#endif

RewriterVar* JitFragment::getInterp() {
    return interp;
}

RewriterVar* JitFragment::imm(void* val) {
    return loadConst((uint64_t)val);
}

RewriterVar* JitFragment::imm(uint64_t val) {
    return loadConst(val);
}

RewriterVar* JitFragment::allocArgs(const llvm::ArrayRef<Value> args) {
    auto num = args.size();
    RewriterVar* array = allocate(num);
    for (int i = 0; i < num; ++i)
        array->setAttr(sizeof(void*) * i, args[i].var);
    return array;
}

Box* JitFragment::setGlobalICHelper(SetGlobalIC* ic, Box* o, BoxedString* s, Box* v) {
    return ic->call(o, s, v);
}

void JitFragment::emitSetGlobal(Box* global, BoxedString* s, Value v) {
#if ENABLE_TRACING_IC
    call(false, (void*)setGlobalICHelper, imm(new SetGlobalIC), imm(global), imm(s), v.var);
#else
    call(false, (void*)setGlobal, imm(global), imm(s), v.var);
#endif
}

Box* JitFragment::getGlobalICHelper(GetGlobalIC* ic, Box* o, BoxedString* s) {
    return ic->call(o, s);
}

RewriterVar* JitFragment::emitGetGlobal(Box* global, BoxedString* s) {
#if ENABLE_TRACING_IC
    return call(false, (void*)getGlobalICHelper, imm(new GetGlobalIC), imm(global), imm(s));
#else
    return call(false, (void*)getGlobal, imm(global), imm(s));
#endif
}

Box* JitFragment::setAttrICHelper(SetAttrIC* ic, Box* o, BoxedString* attr, Box* value) {
    return ic->call(o, attr, value);
}

void JitFragment::emitSetAttr(Value obj, BoxedString* s, Value attr) {
#if ENABLE_TRACING_IC
    call(false, (void*)setAttrICHelper, imm(new SetAttrIC), obj.var, imm(s), attr.var);
#else
    call(false, (void*)setattr, obj.var, imm(s), attr.var);
#endif
}

Box* JitFragment::getAttrICHelper(GetAttrIC* ic, Box* o, BoxedString* attr) {
    return ic->call(o, attr);
}

RewriterVar* JitFragment::emitGetAttr(Value obj, BoxedString* s) {
#if ENABLE_TRACING_IC
    return call(false, (void*)getAttrICHelper, imm(new GetAttrIC), obj.var, imm(s));
#else
    return call(false, (void*)getattr, obj.var, imm(s));
#endif
}

RewriterVar* JitFragment::emitGetClsAttr(Value obj, BoxedString* s) {
    return call(false, (void*)getclsattr, obj.var, imm(s));
}

RewriterVar* JitFragment::emitGetLocal(InternedString s) {
    return call(false, (void*)ASTInterpreterJitInterface::tracerHelperGetLocal, getInterp(),
#ifndef NDEBUG
                imm(asUInt(s).first), imm(asUInt(s).second));
#else
                imm(asUInt(s)));
#endif
}

void JitFragment::emitSetLocal(InternedString s, bool set_closure, Value v) {
    call(false, (void*)ASTInterpreterJitInterface::tracerHelperSetLocal, getInterp(),
#ifndef NDEBUG
         imm(asUInt(s).first), imm(asUInt(s).second),
#else
         imm(asUInt(s)),
#endif
         v.var, imm((uint64_t)set_closure));
}

void JitFragment::emitSetBlockLocal(InternedString s, Value v) {
    local_syms[s] = v.var;
}

RewriterVar* JitFragment::emitGetBlockLocal(InternedString s) {
    auto it = local_syms.find(s);
    if (it == local_syms.end())
        return emitGetLocal(s);
    return it->second;
}

RewriterVar* JitFragment::emitBoxedLocalsGet(BoxedString* s) {
    return call(false, (void*)ASTInterpreterJitInterface::boxedLocalsGetHelper, getInterp(), imm(s));
}

Box* JitFragment::setitemICHelper(SetItemIC* ic, Box* o, Box* attr, Box* value) {
    return ic->call(o, attr, value);
}

void JitFragment::emitSetItem(Value target, Value slice, Value value) {
#if ENABLE_TRACING_IC
    call(false, (void*)setitemICHelper, imm(new SetItemIC), target.var, slice.var, value.var);
#else
    call(false, (void*)setitem, target.var, slice.var, value.var);
#endif
}

Box* JitFragment::getitemICHelper(GetItemIC* ic, Box* o, Box* attr) {
    return ic->call(o, attr);
}

RewriterVar* JitFragment::emitGetItem(Value value, Value slice) {
#if ENABLE_TRACING_IC
    return call(false, (void*)getitemICHelper, imm(new GetItemIC), value.var, slice.var);
#else
    return call(false, (void*)getitem, value.var, slice.var);
#endif
}

void JitFragment::emitSetItemName(BoxedString* s, Value v) {
    call(false, (void*)ASTInterpreterJitInterface::setItemNameHelper, getInterp(), imm(s), v.var);
}

RewriterVar* JitFragment::emitDeref(InternedString s) {
    return call(false, (void*)ASTInterpreterJitInterface::derefHelper, getInterp(),
#ifndef NDEBUG
                imm(asUInt(s).first), imm(asUInt(s).second));
#else
                imm(asUInt(s)));
#endif
}

Box* JitFragment::createTupleHelper(uint64_t num, Box** data) {
    return BoxedTuple::create(num, data);
}

RewriterVar* JitFragment::emitCreateTuple(const llvm::SmallVectorImpl<Value>& values) {
    auto num = values.size();
    if (num == 0)
        return call(false, (void*)BoxedTuple::create0);
    else if (num == 1)
        return call(false, (void*)BoxedTuple::create1, values[0].var);
    else if (num == 2)
        return call(false, (void*)BoxedTuple::create2, values[0].var, values[1].var);
    else if (num == 3)
        return call(false, (void*)BoxedTuple::create3, values[0].var, values[1].var, values[2].var);
    else
        return call(false, (void*)createTupleHelper, imm(num), allocArgs(values));
}

Box* JitFragment::createListHelper(uint64_t num, Box** data) {
    BoxedList* list = (BoxedList*)createList();
    list->ensure(num);
    for (uint64_t i = 0; i < num; ++i) {
        assert(gc::isValidGCObject(data[i]));
        listAppendInternal(list, data[i]);
    }
    return list;
}

RewriterVar* JitFragment::emitCreateList(const llvm::SmallVectorImpl<Value>& values) {
    auto num = values.size();
    if (num == 0)
        return call(false, (void*)createList);
    else
        return call(false, (void*)createListHelper, imm(num), allocArgs(values));
}

RewriterVar* JitFragment::emitCreateDict() {
    return call(false, (void*)createDict);
}
RewriterVar* JitFragment::emitCreateSlice(Value start, Value stop, Value step) {
    return call(false, (void*)createSlice, start.var, stop.var, step.var);
}

Box* JitFragment::nonzeroHelper(Box* b) {
    return boxBool(b->nonzeroIC());
}

RewriterVar* JitFragment::emitNonzero(Value v) {
    return call(false, (void*)nonzeroHelper, v.var);
}

Box* JitFragment::notHelper(Box* b) {
    return boxBool(!b->nonzeroIC());
}

RewriterVar* JitFragment::emitNotNonzero(Value v) {
    return call(false, (void*)notHelper, v.var);
}
RewriterVar* JitFragment::emitGetPystonIter(Value v) {
    return call(false, (void*)getPystonIter, v.var);
}
RewriterVar* JitFragment::emitUnpackIntoArray(Value v, uint64_t num) {
    RewriterVar* array = call(false, (void*)unpackIntoArray, v.var, imm(num));
    return array;
}

Box* JitFragment::compareICHelper(CompareIC* ic, Box* lhs, Box* rhs, int op) {
    return ic->call(lhs, rhs, op);
}

RewriterVar* JitFragment::emitCompare(Value lhs, Value rhs, int op_type) {
#if ENABLE_TRACING_IC
    return call(false, (void*)compareICHelper, imm(new CompareIC), lhs.var, rhs.var, imm(op_type));
#else
    return call(false, (void*)compare, lhs.var, rhs.var, imm(op_type));
#endif
}

Box* JitFragment::augbinopICHelper(AugBinopIC* ic, Box* lhs, Box* rhs, int op) {
    return ic->call(lhs, rhs, op);
}
RewriterVar* JitFragment::emitAugbinop(Value lhs, Value rhs, int op_type) {
#if ENABLE_TRACING_IC
    return call(false, (void*)augbinopICHelper, imm(new AugBinopIC), lhs.var, rhs.var, imm(op_type));
#else
    return call(false, (void*)augbinop, lhs.var, rhs.var, imm(op_type));
#endif
}

Box* JitFragment::binopICHelper(BinopIC* ic, Box* lhs, Box* rhs, int op) {
    return ic->call(lhs, rhs, op);
}
RewriterVar* JitFragment::emitBinop(Value lhs, Value rhs, int op_type) {
#if ENABLE_TRACING_IC
    return call(false, (void*)binopICHelper, imm(new BinopIC), lhs.var, rhs.var, imm(op_type));
#else
    return call(false, (void*)binop, lhs.var, rhs.var, imm(op_type));
#endif
}

Box* JitFragment::unaryopICHelper(UnaryopIC* ic, Box* obj, int op) {
    return ic->call(obj, op);
}
RewriterVar* JitFragment::emitUnaryop(Value v, int op_type) {
#if ENABLE_TRACING_IC
    return call(false, (void*)unaryopICHelper, imm(new UnaryopIC), v.var, imm(op_type));
#else
    return call(false, (void*)unaryop, v.var, imm(op_type));
#endif
}

Box* JitFragment::exceptionMatchesHelper(Box* obj, Box* cls) {
    return boxBool(exceptionMatches(obj, cls));
}

RewriterVar* JitFragment::emitExceptionMatches(Value v, Value cls) {
    return call(false, (void*)exceptionMatchesHelper, v.var, cls.var);
}

RewriterVar* JitFragment::emitLandingpad() {
    return call(false, (void*)ASTInterpreterJitInterface::landingpadHelper, getInterp());
}

RewriterVar* JitFragment::emitYield(Value v) {
    return call(false, (void*)ASTInterpreterJitInterface::yieldHelper, getInterp(), v.var);
}

void JitFragment::emitUncacheExcInfo() {
    call(false, (void*)ASTInterpreterJitInterface::uncacheExcInfoHelper, getInterp());
}

void JitFragment::emitSetExcInfo(Value type, Value value, Value traceback) {
    call(false, (void*)ASTInterpreterJitInterface::setExcInfoHelper, getInterp(), type.var, value.var, traceback.var);
}

#if ENABLE_TRACING_IC
Box* JitFragment::callattrHelperIC(Box* obj, BoxedString* attr, CallattrFlags flags, ArgPassSpec argspec,
                                   CallattrIC* ic, Box** args) {
    auto arg_tuple = getTupleFromArgsArray(&args[0], argspec.totalPassed());
    return ic->call(obj, attr, flags, argspec, std::get<0>(arg_tuple), std::get<1>(arg_tuple));
}
#endif
Box* JitFragment::callattrHelper(Box* obj, BoxedString* attr, CallattrFlags flags, ArgPassSpec argspec, Box** args,
                                 std::vector<BoxedString*>* keyword_names) {
    auto arg_tuple = getTupleFromArgsArray(&args[0], argspec.totalPassed());
    Box* r = callattr(obj, attr, flags, argspec, std::get<0>(arg_tuple), std::get<1>(arg_tuple), std::get<2>(arg_tuple),
                      std::get<3>(arg_tuple), keyword_names);
    assert(gc::isValidGCObject(r));
    return r;
}


RewriterVar* JitFragment::emitCallattr(Value obj, BoxedString* attr, CallattrFlags flags, ArgPassSpec argspec,
                                       std::vector<Value, StlCompatAllocator<Value>> args,
                                       std::vector<BoxedString*>* keyword_names) {
    // We could make this faster but for now: keep it simple, stupid...
    RewriterVar* attr_var = imm(attr);
    RewriterVar* flags_var = imm(flags.asInt());
    RewriterVar* argspec_var = imm(argspec.asInt());
    RewriterVar* keyword_names_var = keyword_names ? imm(keyword_names) : nullptr;

    RewriterVar* args_array = nullptr;
    if (args.size())
        args_array = allocArgs(args);
    else
        RELEASE_ASSERT(!keyword_names_var, "0 args but keyword names are set");

    bool use_ic = false;

    RewriterVar::SmallVector call_args;
    call_args.push_back(obj.var);
    call_args.push_back(attr_var);
    call_args.push_back(flags_var);
    call_args.push_back(argspec_var);

#if ENABLE_TRACING_IC
    if (!keyword_names_var
        && argspec.totalPassed() < 3) { // looks like runtime ICs with 7 or more args don't work right now..
        use_ic = true;
        call_args.push_back(imm(new CallattrIC));
    }
#endif

    if (args_array)
        call_args.push_back(args_array);
    if (keyword_names_var)
        call_args.push_back(keyword_names_var);

#if ENABLE_TRACING_IC
    if (use_ic)
        return call(false, (void*)callattrHelperIC, call_args);
#endif
    return call(false, (void*)callattrHelper, call_args);
}

#if ENABLE_TRACING_IC
Box* JitFragment::runtimeCallHelperIC(Box* obj, ArgPassSpec argspec, RuntimeCallIC* ic, Box** args) {
    auto arg_tuple = getTupleFromArgsArray(&args[0], argspec.totalPassed());
    return ic->call(obj, argspec, std::get<0>(arg_tuple), std::get<1>(arg_tuple), std::get<2>(arg_tuple),
                    std::get<3>(arg_tuple));
}
#endif
Box* JitFragment::runtimeCallHelper(Box* obj, ArgPassSpec argspec, Box** args,
                                    std::vector<BoxedString*>* keyword_names) {
    auto arg_tuple = getTupleFromArgsArray(&args[0], argspec.totalPassed());
    return runtimeCall(obj, argspec, std::get<0>(arg_tuple), std::get<1>(arg_tuple), std::get<2>(arg_tuple),
                       std::get<3>(arg_tuple), keyword_names);
}


RewriterVar* JitFragment::emitRuntimeCall(Value obj, ArgPassSpec argspec,
                                          std::vector<Value, StlCompatAllocator<Value>> args,
                                          std::vector<BoxedString*>* keyword_names) {
    // We could make this faster but for now: keep it simple, stupid..
    RewriterVar* argspec_var = imm(argspec.asInt());
    RewriterVar* keyword_names_var = keyword_names ? imm(keyword_names) : nullptr;

    RewriterVar* args_array = nullptr;
    if (args.size()) {
        args_array = allocArgs(args);
    } else
        RELEASE_ASSERT(!keyword_names_var, "0 args but keyword names are set");

    bool use_ic = false;

    RewriterVar::SmallVector call_args;
    call_args.push_back(obj.var);
    call_args.push_back(argspec_var);


#if ENABLE_TRACING_IC
    if (!keyword_names) { // looks like runtime ICs with 7 or more args don't work right now..
        use_ic = true;
        call_args.push_back(imm(new RuntimeCallIC));
    }
#endif

    if (args_array)
        call_args.push_back(args_array);
    if (keyword_names_var)
        call_args.push_back(keyword_names_var);

#if ENABLE_TRACING_IC
    if (use_ic)
        return call(false, (void*)runtimeCallHelperIC, call_args);
#endif
    return call(false, (void*)runtimeCallHelper, call_args);
}

Box* JitFragment::hasnextHelper(Box* b) {
    return boxBool(pyston::hasnext(b));
}

RewriterVar* JitFragment::emitHasnext(Value v) {
    return call(false, (void*)hasnextHelper, v.var);
}

void JitFragment::emitSetCurrentInst(AST_stmt* node) {
    getInterp()->setAttr(ASTInterpreterJitInterface::getCurrentInstOffset(), imm(node));
}

void JitFragment::emitOSRPoint(AST_Jump* node) {
    RewriterVar* node_var = imm(node);
    RewriterVar* result = createNewVar();
    addAction([=]() { _emitOSRPoint(result, node_var); }, { result, node_var, getInterp() }, ActionType::NORMAL);
}

void JitFragment::_emitOSRPoint(RewriterVar* result, RewriterVar* node_var) {
    RewriterVar::SmallVector args;
    args.push_back(getInterp());
    args.push_back(node_var);
    _call(result, false, (void*)ASTInterpreterJitInterface::doOSRHelper, args, RewriterVar::SmallVector());
    auto result_reg = result->getInReg(assembler::RDX);
    result->bumpUse();

    assembler->test(result_reg, result_reg);
    {
        assembler::ForwardJump je(*assembler, assembler::COND_EQUAL);
        assembler->je(assembler::JumpDestination::fromStart(assembler->bytesWritten() + 50));
        assembler->mov(assembler::Immediate(0ul), assembler::RAX);
        assembler->jmp(assembler::JumpDestination::fromStart(epilog_offset));
    }

    assertConsistent();
}

void JitFragment::emitSideExit(Value v, CFGBlock* next_block) {
    RewriterVar* var = imm(v.o == True ? (void*)False : (void*)True);
    RewriterVar* false_path = imm(next_block);
    addAction([=]() { _emitSideExit(v.var, var, false_path); }, { v.var, var, false_path }, ActionType::NORMAL);
}

void JitFragment::_emitSideExit(RewriterVar* var, RewriterVar* val_constant, RewriterVar* false_path) {
    assert(val_constant->is_constant);
    assert(false_path->is_constant);
    uint64_t val = val_constant->constant_value;

    assembler::Register var_reg = var->getInReg();
    if (isLargeConstant(val)) {
        assembler::Register reg = val_constant->getInReg(Location::any(), true, /* otherThan */ var_reg);
        assembler->cmp(var_reg, reg);
    } else {
        assembler->cmp(var_reg, assembler::Immediate(val));
    }

    {
        assembler::ForwardJump jne(*assembler, assembler::COND_NOT_EQUAL);
        false_path->getInReg(assembler::RAX, true);
        assembler->mov(assembler::Indirect(assembler::RAX, 8), assembler::RSI);
        assembler->test(assembler::RSI, assembler::RSI);
        assembler->je(assembler::JumpDestination::fromStart(epilog_offset));
        assembler->emitByte(0xFF);
        assembler->emitByte(0x60);
        assembler->emitByte(0x08); // jmp qword ptr [rax+8]
    }

    var->bumpUse();
    val_constant->bumpUse();
    false_path->bumpUse();

    assertConsistent();
}

void JitFragment::emitJump(CFGBlock* b) {
    RewriterVar* next = imm(b);
    addAction([=]() { _emitJump(b, next); }, { next }, ActionType::NORMAL);
}

void JitFragment::_emitJump(CFGBlock* b, RewriterVar* block_next) {
    if (b->code) {
        assembler->jmp(assembler::JumpDestination::fromStart((uint64_t)b->code - ((uint64_t)entry_code + code_offset)));
    } else {
        // TODO we could patch this later...
        int num_bytes = assembler->bytesWritten();
        block_next->getInReg(assembler::RAX, true);
        assembler->mov(assembler::Indirect(assembler::RAX, 8), assembler::RSI);
        assembler->test(assembler::RSI, assembler::RSI);
        assembler->je(assembler::JumpDestination::fromStart(epilog_offset));
        assembler->emitByte(0xFF);
        assembler->emitByte(0x60);
        assembler->emitByte(0x08); // jmp qword ptr [rax+8]
        continue_jmp_offset = assembler->bytesWritten() - num_bytes;
    }
    block_next->bumpUse();
}

void JitFragment::emitReturn(Value v) {
    RewriterVar* next = imm(0ul);
    addAction([=]() { _emitReturn(v.var, next); }, { v.var, next }, ActionType::NORMAL);
}

void JitFragment::_emitReturn(RewriterVar* v, RewriterVar* next) {
    next->getInReg(assembler::RAX, true);
    v->getInReg(assembler::RDX, true);
    assembler->jmp(assembler::JumpDestination::fromStart(epilog_offset));
    next->bumpUse();
    v->bumpUse();
}

void JitFragment::abortTrace() {
    tracers_aborted.insert(block);
    iscurrently_tracing = false;
    abort();
}

int JitFragment::compile() {
    iscurrently_tracing = false;

    RELEASE_ASSERT(!assembler->hasFailed(), "");

    commit();
    if (failed) {
        tracers_aborted.insert(block);
        iscurrently_tracing = false;
        return 0;
    }

    RELEASE_ASSERT(!assembler->hasFailed(), "");
    block->code = (void*)((uint64_t)entry_code + code_offset);
    if (entry_code)
        block->entry_code = entry_code;
    commited_callback(assembler->bytesWritten());
    return continue_jmp_offset;
}

bool JitFragment::finishAssembly(ICSlotInfo* picked_slot, int continue_offset) {
    return !assembler->hasFailed();
}

static void* myalloc(uint64_t size) {
    static llvm::sys::MemoryBlock lastBlock;
    llvm_error_code ec;
    llvm::sys::MemoryBlock MB = llvm::sys::Memory::allocateMappedMemory(
        size, &lastBlock, llvm::sys::Memory::MF_READ | llvm::sys::Memory::MF_WRITE, ec);
    lastBlock = MB;
    return MB.base();
}


JitCodeBlock::JitCodeBlock(llvm::StringRef name)
    : code(myalloc(code_size)),
      entry_offset(0),
      epilog_offset(0),
      a((uint8_t*)code, code_size - epilog_size),
      iscurrently_tracing(false) {

    // emit prolog
    a.push(assembler::RBP);
    a.mov(assembler::RSP, assembler::RBP);

    a.sub(assembler::Immediate(256 + 8), assembler::RSP); // scratch
    a.push(assembler::RDI);                               // push interpreter pointer

    a.emitByte(0xFF);
    a.emitByte(0x66);
    a.emitByte(0x08); // jmp    QWORD PTR [rsi+0x8]
    entry_offset = a.bytesWritten();

    // emit epilog
    epilog_offset = code_size - epilog_size;
    assembler::Assembler endAsm((uint8_t*)code + epilog_offset, epilog_size);
    endAsm.leave();
    endAsm.retq();
    RELEASE_ASSERT(!endAsm.hasFailed(), "");

    // generate eh frame...
    EHwriteAndRegister(code, code_size);

    g.func_addr_registry.registerFunction(("bjit: " + name).str(), code, code_size, NULL);
}

std::unique_ptr<JitFragment> JitCodeBlock::newFragment(CFGBlock* block, int jump_offset) {
    if (iscurrently_tracing || tracers_aborted.count(block))
        return std::unique_ptr<JitFragment>();
    if (a.bytesLeft() < 50) {
        printf("not enough mem!\n");
        return std::unique_ptr<JitFragment>();
    }

    iscurrently_tracing = true;

    StackInfo stack_info(256, 16);
    std::unordered_set<int> live_outs;

    void* start = a.curInstPointer() - jump_offset;
    long bytes_written = a.bytesWritten() - jump_offset;
    ICInfo* ic_info = new ICInfo((void*)start, nullptr, nullptr, stack_info, 1, a.bytesLeft() + jump_offset,
                                 llvm::CallingConv::C, live_outs, assembler::RAX, 0);
    ICSlotRewrite* rewrite = new ICSlotRewrite(ic_info, "lala");

    auto callback = [this](int size) { fragmentCommited(size); };
    auto rtn = std::unique_ptr<JitFragment>(new JitFragment(
        block, rewrite, bytes_written, epilog_offset - bytes_written, a.getStartAddr(), iscurrently_tracing, callback));
    return rtn;
}

void JitCodeBlock::fragmentCommited(int size) {
    a.setCurInstPointer(a.curInstPointer() + size);
}

void JitCodeBlock::writeTrivialEhFrame(void* eh_frame_addr, void* func_addr, uint64_t func_size) {
    memcpy(eh_frame_addr, _eh_frame_template, EH_FRAME_SIZE);

    int32_t* offset_ptr = (int32_t*)((uint8_t*)eh_frame_addr + 0x20);
    int32_t* size_ptr = (int32_t*)((uint8_t*)eh_frame_addr + 0x24);

    int64_t offset = (int8_t*)func_addr - (int8_t*)offset_ptr;
    assert(offset >= INT_MIN && offset <= INT_MAX);
    *offset_ptr = offset;

    assert(func_size <= UINT_MAX);
    *size_ptr = func_size;
}

void JitCodeBlock::EHwriteAndRegister(void* func_addr, uint64_t func_size) {
    void* eh_frame_addr = 0;
    eh_frame_addr = myalloc(EH_FRAME_SIZE);
    writeTrivialEhFrame(eh_frame_addr, func_addr, func_size);
    // (EH_FRAME_SIZE - 4) to omit the 4-byte null terminator, otherwise we trip an assert in parseEhFrame.
    // TODO: can we omit the terminator in general?
    registerDynamicEhFrame((uint64_t)func_addr, func_size, (uint64_t)eh_frame_addr, EH_FRAME_SIZE - 4);
    registerEHFrames((uint8_t*)eh_frame_addr, (uint64_t)eh_frame_addr, EH_FRAME_SIZE);
}
}
