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
#include <llvm/ADT/DenseSet.h>

#include "codegen/irgen/hooks.h"
#include "codegen/memmgr.h"
#include "core/cfg.h"
#include "runtime/objmodel.h"
#include "runtime/set.h"
#include "runtime/types.h"

namespace pyston {

static llvm::DenseSet<CFGBlock*> blocks_aborted;

JitCodeBlock::JitCodeBlock(llvm::StringRef name)
    : frame_manager(false /* don't omit frame pointers */),
      code(new uint8_t[code_size]),
      entry_offset(0),
      epilog_offset(0),
      a(code.get(), code_size - epilog_size),
      iscurrently_tracing(false),
      asm_failed(false) {
    static StatCounter num_jit_code_blocks("num_baselinejit_code_blocks");
    num_jit_code_blocks.log();

    // emit prolog
    a.push(assembler::RBP);
    a.mov(assembler::RSP, assembler::RBP);

    static_assert(scratch_size % 16 == 0, "stack aligment code depends on this");
    // subtract scratch size + 8bytes to align stack after the push.
    a.sub(assembler::Immediate(scratch_size + 8), assembler::RSP);
    a.push(assembler::RDI);                                               // push interpreter pointer
    a.jmp(assembler::Indirect(assembler::RSI, offsetof(CFGBlock, code))); // jump to block

    entry_offset = a.bytesWritten();

    // emit epilog
    epilog_offset = code_size - epilog_size;
    assembler::Assembler endAsm(code.get() + epilog_offset, epilog_size);
    endAsm.leave();
    endAsm.retq();
    RELEASE_ASSERT(!endAsm.hasFailed(), "");

    // generate eh frame...
    frame_manager.writeAndRegister(code.get(), code_size);

    g.func_addr_registry.registerFunction(("bjit: " + name).str(), code.get(), code_size, NULL);
}

std::unique_ptr<JitFragment> JitCodeBlock::newFragment(CFGBlock* block, int jump_offset) {
    if (iscurrently_tracing || blocks_aborted.count(block))
        return std::unique_ptr<JitFragment>();

    iscurrently_tracing = true;

    StackInfo stack_info(scratch_size, 16);
    std::unordered_set<int> live_outs;

    void* start = a.curInstPointer() - jump_offset;
    long bytes_written = a.bytesWritten() - jump_offset;
    std::unique_ptr<ICInfo> ic_info(new ICInfo(start, nullptr, nullptr, stack_info, 1, a.bytesLeft() + jump_offset,
                                               llvm::CallingConv::C, live_outs, assembler::RAX, 0));
    std::unique_ptr<ICSlotRewrite> rewrite(new ICSlotRewrite(ic_info.get(), ""));

    auto rtn
        = std::unique_ptr<JitFragment>(new JitFragment(block, std::move(ic_info), std::move(rewrite), bytes_written,
                                                       epilog_offset - bytes_written, a.getStartAddr(), *this));
    return rtn;
}

void JitCodeBlock::fragmentFinished(int size, bool not_enough_space) {
    a.setCurInstPointer(a.curInstPointer() + size);
    asm_failed = not_enough_space;
    iscurrently_tracing = false;
}


JitFragment::JitFragment(CFGBlock* block, std::unique_ptr<ICInfo> ic_info, std::unique_ptr<ICSlotRewrite> rewrite,
                         int code_offset, int epilog_offset, void* entry_code, JitCodeBlock& code_block)
    : Rewriter(std::move(rewrite), 0, {}),
      block(block),
      code_offset(code_offset),
      epilog_offset(epilog_offset),
      continue_jmp_offset(0),
      entry_code(entry_code),
      code_block(code_block),
      interp(0),
      ic_info(std::move(ic_info)) {
    interp = createNewVar();
    addLocationToVar(interp, Location(Location::Stack, 0));
    interp->setAttr(ASTInterpreterJitInterface::getCurrentBlockOffset(), imm(block));
}

RewriterVar* JitFragment::imm(uint64_t val) {
    return loadConst(val);
}

RewriterVar* JitFragment::imm(void* val) {
    return loadConst((uint64_t)val);
}

RewriterVar* JitFragment::emitAugbinop(RewriterVar* lhs, RewriterVar* rhs, int op_type) {
#if ENABLE_BASELINEJIT_ICS
    return call(false, (void*)augbinopICHelper, imm(new AugBinopIC), lhs, rhs, imm(op_type));
#else
    return call(false, (void*)augbinop, lhs, rhs, imm(op_type));
#endif
}

RewriterVar* JitFragment::emitBinop(RewriterVar* lhs, RewriterVar* rhs, int op_type) {
#if ENABLE_BASELINEJIT_ICS
    return call(false, (void*)binopICHelper, imm(new BinopIC), lhs, rhs, imm(op_type));
#else
    return call(false, (void*)binop, lhs, rhs, imm(op_type));
#endif
}

RewriterVar* JitFragment::emitCallattr(RewriterVar* obj, BoxedString* attr, CallattrFlags flags, ArgPassSpec argspec,
                                       const llvm::ArrayRef<RewriterVar*> args,
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
    call_args.push_back(obj);
    call_args.push_back(attr_var);
    call_args.push_back(flags_var);
    call_args.push_back(argspec_var);

// #if ENABLE_BASELINEJIT_ICS
#if 0 && ENABLE_BASELINEJIT_ICS // disable for now: there is a problem with test/extra/protobuf_test.py
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

#if ENABLE_BASELINEJIT_ICS
    if (use_ic)
        return call(false, (void*)callattrHelperIC, call_args);
#endif
    return call(false, (void*)callattrHelper, call_args);
}

RewriterVar* JitFragment::emitCompare(RewriterVar* lhs, RewriterVar* rhs, int op_type) {
#if ENABLE_BASELINEJIT_ICS
    return call(false, (void*)compareICHelper, imm(new CompareIC), lhs, rhs, imm(op_type));
#else
    return call(false, (void*)compare, lhs, rhs, imm(op_type));
#endif
}

RewriterVar* JitFragment::emitCreateDict(const llvm::ArrayRef<RewriterVar*> keys,
                                         const llvm::ArrayRef<RewriterVar*> values) {
    assert(keys.size() == values.size());
    if (keys.empty())
        return call(false, (void*)createDict);
    else
        return call(false, (void*)createDictHelper, imm(keys.size()), allocArgs(keys), allocArgs(values));
}

RewriterVar* JitFragment::emitCreateList(const llvm::ArrayRef<RewriterVar*> values) {
    auto num = values.size();
    if (num == 0)
        return call(false, (void*)createList);
    else
        return call(false, (void*)createListHelper, imm(num), allocArgs(values));
}

RewriterVar* JitFragment::emitCreateSet(const llvm::ArrayRef<RewriterVar*> values) {
    return call(false, (void*)createSetHelper, imm(values.size()), allocArgs(values));
}

RewriterVar* JitFragment::emitCreateSlice(RewriterVar* start, RewriterVar* stop, RewriterVar* step) {
    return call(false, (void*)createSlice, start, stop, step);
}

RewriterVar* JitFragment::emitCreateTuple(const llvm::ArrayRef<RewriterVar*> values) {
    auto num = values.size();
    if (num == 0)
        return imm(EmptyTuple);
    else if (num == 1)
        return call(false, (void*)BoxedTuple::create1, values[0]);
    else if (num == 2)
        return call(false, (void*)BoxedTuple::create2, values[0], values[1]);
    else if (num == 3)
        return call(false, (void*)BoxedTuple::create3, values[0], values[1], values[2]);
    else
        return call(false, (void*)createTupleHelper, imm(num), allocArgs(values));
}

RewriterVar* JitFragment::emitDeref(InternedString s) {
    return call(false, (void*)ASTInterpreterJitInterface::derefHelper, getInterp(),
#ifndef NDEBUG
                imm(asUInt(s).first), imm(asUInt(s).second));
#else
                imm(asUInt(s)));
#endif
}

RewriterVar* JitFragment::emitExceptionMatches(RewriterVar* v, RewriterVar* cls) {
    return call(false, (void*)exceptionMatchesHelper, v, cls);
}

RewriterVar* JitFragment::emitGetAttr(RewriterVar* obj, BoxedString* s) {
#if ENABLE_BASELINEJIT_ICS
    return call(false, (void*)getAttrICHelper, imm(new GetAttrIC), obj, imm(s));
#else
    return call(false, (void*)getattr, obj, imm(s));
#endif
}

RewriterVar* JitFragment::emitGetBlockLocal(InternedString s) {
    auto it = local_syms.find(s);
    if (it == local_syms.end())
        return emitGetLocal(s);
    return it->second;
}

RewriterVar* JitFragment::emitGetBoxedLocal(BoxedString* s) {
    return call(false, (void*)ASTInterpreterJitInterface::getBoxedLocalHelper, getInterp(), imm(s));
}

RewriterVar* JitFragment::emitGetBoxedLocals() {
    return call(false, (void*)ASTInterpreterJitInterface::getBoxedLocalsHelper, getInterp());
}

RewriterVar* JitFragment::emitGetClsAttr(RewriterVar* obj, BoxedString* s) {
    return call(false, (void*)getclsattr, obj, imm(s));
}

RewriterVar* JitFragment::emitGetGlobal(Box* global, BoxedString* s) {
#if ENABLE_BASELINEJIT_ICS
    return call(false, (void*)getGlobalICHelper, imm(new GetGlobalIC), imm(global), imm(s));
#else
    return call(false, (void*)getGlobal, imm(global), imm(s));
#endif
}

RewriterVar* JitFragment::emitGetItem(RewriterVar* value, RewriterVar* slice) {
#if ENABLE_BASELINEJIT_ICS
    return call(false, (void*)getitemICHelper, imm(new GetItemIC), value, slice);
#else
    return call(false, (void*)getitem, value, slice);
#endif
}

RewriterVar* JitFragment::emitGetLocal(InternedString s) {
    return call(false, (void*)ASTInterpreterJitInterface::getLocalHelper, getInterp(),
#ifndef NDEBUG
                imm(asUInt(s).first), imm(asUInt(s).second));
#else
                imm(asUInt(s)));
#endif
}

RewriterVar* JitFragment::emitGetPystonIter(RewriterVar* v) {
    return call(false, (void*)getPystonIter, v);
}

RewriterVar* JitFragment::emitHasnext(RewriterVar* v) {
    return call(false, (void*)hasnextHelper, v);
}

RewriterVar* JitFragment::emitLandingpad() {
    return call(false, (void*)ASTInterpreterJitInterface::landingpadHelper, getInterp());
}

RewriterVar* JitFragment::emitNonzero(RewriterVar* v) {
    return call(false, (void*)nonzeroHelper, v);
}

RewriterVar* JitFragment::emitNotNonzero(RewriterVar* v) {
    return call(false, (void*)notHelper, v);
}

RewriterVar* JitFragment::emitRepr(RewriterVar* v) {
    return call(false, (void*)repr, v);
}

RewriterVar* JitFragment::emitRuntimeCall(RewriterVar* obj, ArgPassSpec argspec,
                                          const llvm::ArrayRef<RewriterVar*> args,
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
    call_args.push_back(obj);
    call_args.push_back(argspec_var);


#if ENABLE_BASELINEJIT_ICS
    if (!keyword_names) { // looks like runtime ICs with 7 or more args don't work right now..
        use_ic = true;
        call_args.push_back(imm(new RuntimeCallIC));
    }
#endif

    if (args_array)
        call_args.push_back(args_array);
    if (keyword_names_var)
        call_args.push_back(keyword_names_var);

#if ENABLE_BASELINEJIT_ICS
    if (use_ic)
        return call(false, (void*)runtimeCallHelperIC, call_args);
#endif
    return call(false, (void*)runtimeCallHelper, call_args);
}

RewriterVar* JitFragment::emitUnaryop(RewriterVar* v, int op_type) {
#if ENABLE_BASELINEJIT_ICS
    return call(false, (void*)unaryopICHelper, imm(new UnaryopIC), v, imm(op_type));
#else
    return call(false, (void*)unaryop, v, imm(op_type));
#endif
}

RewriterVar* JitFragment::emitUnpackIntoArray(RewriterVar* v, uint64_t num) {
    RewriterVar* array = call(false, (void*)unpackIntoArray, v, imm(num));
    return array;
}

RewriterVar* JitFragment::emitYield(RewriterVar* v) {
    return call(false, (void*)ASTInterpreterJitInterface::yieldHelper, getInterp(), v);
}


void JitFragment::emitExec(RewriterVar* code, RewriterVar* globals, RewriterVar* locals, FutureFlags flags) {
    if (!globals)
        globals = imm(0ul);
    if (!locals)
        locals = imm(0ul);
    call(false, (void*)exec, code, globals, locals, imm(flags));
}

void JitFragment::emitJump(CFGBlock* b) {
    RewriterVar* next = imm(b);
    addAction([=]() { _emitJump(b, next, continue_jmp_offset); }, { next }, ActionType::NORMAL);
}

void JitFragment::emitOSRPoint(AST_Jump* node) {
    RewriterVar* node_var = imm(node);
    RewriterVar* result = createNewVar();
    addAction([=]() { _emitOSRPoint(result, node_var); }, { result, node_var, getInterp() }, ActionType::NORMAL);
}

void JitFragment::emitPrint(RewriterVar* dest, RewriterVar* var, bool nl) {
    if (!dest)
        dest = call(false, (void*)getSysStdout);
    if (!var)
        var = imm(0ul);
    call(false, (void*)printHelper, dest, var, imm(nl));
}

void JitFragment::emitRaise0() {
    call(false, (void*)raise0);
}

void JitFragment::emitRaise3(RewriterVar* arg0, RewriterVar* arg1, RewriterVar* arg2) {
    call(false, (void*)raise3, arg0, arg1, arg2);
}

void JitFragment::emitReturn(RewriterVar* v) {
    addAction([=]() { _emitReturn(v); }, { v }, ActionType::NORMAL);
}

void JitFragment::emitSetAttr(RewriterVar* obj, BoxedString* s, RewriterVar* attr) {
#if ENABLE_BASELINEJIT_ICS
    call(false, (void*)setAttrICHelper, imm(new SetAttrIC), obj, imm(s), attr);
#else
    call(false, (void*)setattr, obj, imm(s), attr);
#endif
}

void JitFragment::emitSetBlockLocal(InternedString s, RewriterVar* v) {
    local_syms[s] = v;
}

void JitFragment::emitSetCurrentInst(AST_stmt* node) {
    getInterp()->setAttr(ASTInterpreterJitInterface::getCurrentInstOffset(), imm(node));
}

void JitFragment::emitSetExcInfo(RewriterVar* type, RewriterVar* value, RewriterVar* traceback) {
    call(false, (void*)ASTInterpreterJitInterface::setExcInfoHelper, getInterp(), type, value, traceback);
}

void JitFragment::emitSetGlobal(Box* global, BoxedString* s, RewriterVar* v) {
#if ENABLE_BASELINEJIT_ICS
    call(false, (void*)setGlobalICHelper, imm(new SetGlobalIC), imm(global), imm(s), v);
#else
    call(false, (void*)setGlobal, imm(global), imm(s), v);
#endif
}

void JitFragment::emitSetItem(RewriterVar* target, RewriterVar* slice, RewriterVar* value) {
#if ENABLE_BASELINEJIT_ICS
    call(false, (void*)setitemICHelper, imm(new SetItemIC), target, slice, value);
#else
    call(false, (void*)setitem, target, slice, value);
#endif
}

void JitFragment::emitSetItemName(BoxedString* s, RewriterVar* v) {
    call(false, (void*)ASTInterpreterJitInterface::setItemNameHelper, getInterp(), imm(s), v);
}

void JitFragment::emitSetLocal(InternedString s, bool set_closure, RewriterVar* v) {
    void* func = set_closure ? (void*)ASTInterpreterJitInterface::setLocalClosureHelper
                             : (void*)ASTInterpreterJitInterface::setLocalHelper;

    call(false, func, getInterp(),
#ifndef NDEBUG
         imm(asUInt(s).first), imm(asUInt(s).second),
#else
         imm(asUInt(s)),
#endif
         v);
}

void JitFragment::emitSideExit(RewriterVar* v, Box* cmp_value, CFGBlock* next_block) {
    RewriterVar* var = imm(cmp_value);
    RewriterVar* next_block_var = imm(next_block);
    addAction([=]() { _emitSideExit(v, var, next_block, next_block_var); }, { v, var, next_block_var },
              ActionType::NORMAL);
}

void JitFragment::emitUncacheExcInfo() {
    call(false, (void*)ASTInterpreterJitInterface::uncacheExcInfoHelper, getInterp());
}


void JitFragment::abortCompilation() {
    blocks_aborted.insert(block);
    code_block.fragmentFinished(0, false);
    abort();
}

int JitFragment::finishCompilation() {
    RELEASE_ASSERT(!assembler->hasFailed(), "");

    commit();
    if (failed) {
        blocks_aborted.insert(block);
        code_block.fragmentFinished(0, false);
        return 0;
    }

    if (assembler->hasFailed()) {
        code_block.fragmentFinished(0, true);
        return 0;
    }

    block->code = (void*)((uint64_t)entry_code + code_offset);
    block->entry_code = (decltype(block->entry_code))entry_code;
    code_block.fragmentFinished(assembler->bytesWritten(), false);
    return continue_jmp_offset;
}

bool JitFragment::finishAssembly(ICSlotInfo* picked_slot, int continue_offset) {
    return !assembler->hasFailed();
}


RewriterVar* JitFragment::allocArgs(const llvm::ArrayRef<RewriterVar*> args) {
    auto num = args.size();
    RewriterVar* array = allocate(num);
    for (int i = 0; i < num; ++i)
        array->setAttr(sizeof(void*) * i, args[i]);
    return array;
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


Box* JitFragment::augbinopICHelper(AugBinopIC* ic, Box* lhs, Box* rhs, int op) {
    return ic->call(lhs, rhs, op);
}


Box* JitFragment::binopICHelper(BinopIC* ic, Box* lhs, Box* rhs, int op) {
    return ic->call(lhs, rhs, op);
}

#if ENABLE_BASELINEJIT_ICS
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

Box* JitFragment::compareICHelper(CompareIC* ic, Box* lhs, Box* rhs, int op) {
    return ic->call(lhs, rhs, op);
}

Box* JitFragment::createDictHelper(uint64_t num, Box** keys, Box** values) {
    BoxedDict* dict = (BoxedDict*)createDict();
    for (uint64_t i = 0; i < num; ++i) {
        assert(gc::isValidGCObject(keys[i]));
        assert(gc::isValidGCObject(values[i]));
        dict->d[keys[i]] = values[i];
    }
    return dict;
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

Box* JitFragment::createSetHelper(uint64_t num, Box** data) {
    BoxedSet* set = (BoxedSet*)createSet();
    for (int i = 0; i < num; ++i)
        set->s.insert(data[i]);
    return set;
}

Box* JitFragment::createTupleHelper(uint64_t num, Box** data) {
    return BoxedTuple::create(num, data);
}

Box* JitFragment::exceptionMatchesHelper(Box* obj, Box* cls) {
    return boxBool(exceptionMatches(obj, cls));
}

Box* JitFragment::getAttrICHelper(GetAttrIC* ic, Box* o, BoxedString* attr) {
    return ic->call(o, attr);
}

Box* JitFragment::getGlobalICHelper(GetGlobalIC* ic, Box* o, BoxedString* s) {
    return ic->call(o, s);
}

Box* JitFragment::getitemICHelper(GetItemIC* ic, Box* o, Box* attr) {
    return ic->call(o, attr);
}

Box* JitFragment::hasnextHelper(Box* b) {
    return boxBool(pyston::hasnext(b));
}

Box* JitFragment::nonzeroHelper(Box* b) {
    return boxBool(b->nonzeroIC());
}

Box* JitFragment::notHelper(Box* b) {
    return boxBool(!b->nonzeroIC());
}

#if ENABLE_BASELINEJIT_ICS
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

Box* JitFragment::setAttrICHelper(SetAttrIC* ic, Box* o, BoxedString* attr, Box* value) {
    return ic->call(o, attr, value);
}

Box* JitFragment::setGlobalICHelper(SetGlobalIC* ic, Box* o, BoxedString* s, Box* v) {
    return ic->call(o, s, v);
}

Box* JitFragment::setitemICHelper(SetItemIC* ic, Box* o, Box* attr, Box* value) {
    return ic->call(o, attr, value);
}

Box* JitFragment::unaryopICHelper(UnaryopIC* ic, Box* obj, int op) {
    return ic->call(obj, op);
}


void JitFragment::_emitJump(CFGBlock* b, RewriterVar* block_next, int& size_of_indirect_jump) {
    size_of_indirect_jump = 0;
    if (b->code) {
        int64_t offset = (uint64_t)b->code - ((uint64_t)entry_code + code_offset);
        if (isLargeConstant(offset)) {
            assembler->mov(assembler::Immediate(b->code), assembler::R11);
            assembler->jmpq(assembler::R11);
        } else
            assembler->jmp(assembler::JumpDestination::fromStart(offset));
    } else {
        int num_bytes = assembler->bytesWritten();
        block_next->getInReg(assembler::RAX, true);
        assembler->mov(assembler::Indirect(assembler::RAX, 8), assembler::RSI);
        assembler->test(assembler::RSI, assembler::RSI);
        assembler->je(assembler::JumpDestination::fromStart(epilog_offset));
        assembler->jmp(assembler::Indirect(assembler::RAX, offsetof(CFGBlock, code)));
        size_of_indirect_jump = assembler->bytesWritten() - num_bytes;
    }
    block_next->bumpUse();
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
        assembler->mov(assembler::Immediate(0ul), assembler::RAX); // TODO: use xor
        assembler->jmp(assembler::JumpDestination::fromStart(epilog_offset));
    }

    assertConsistent();
}

void JitFragment::_emitReturn(RewriterVar* return_val) {
    return_val->getInReg(assembler::RDX, true);
    assembler->mov(assembler::Immediate(0ul), assembler::RAX); // TODO: use xor
    assembler->jmp(assembler::JumpDestination::fromStart(epilog_offset));
    return_val->bumpUse();
}

void JitFragment::_emitSideExit(RewriterVar* var, RewriterVar* val_constant, CFGBlock* next_block,
                                RewriterVar* next_block_var) {
    assert(val_constant->is_constant);
    assert(next_block_var->is_constant);
    uint64_t val = val_constant->constant_value;

    assembler::Register var_reg = var->getInReg();
    if (isLargeConstant(val)) {
        assembler::Register reg = val_constant->getInReg(Location::any(), true, /* otherThan */ var_reg);
        assembler->cmp(var_reg, reg);
    } else {
        assembler->cmp(var_reg, assembler::Immediate(val));
    }

    {
        assembler::ForwardJump jne(*assembler, assembler::COND_EQUAL);
        int bytes = 0;
        _emitJump(next_block, next_block_var, bytes);
        if (bytes) {
            // TODO: We generated an indirect jump.
            // If we later on JIT the dest block we could patch this code to a direct jump to the dest.
        }
    }

    var->bumpUse();
    val_constant->bumpUse();

    assertConsistent();
}
}
