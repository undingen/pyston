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

#include "codegen/ast_interpreter.h"

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/StringMap.h>
#include <unordered_map>

#include "analysis/function_analysis.h"
#include "analysis/scoping_analysis.h"
#include "asm_writing/assembler.h"
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
#include "runtime/capi.h"
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

#define ENABLE_TRACING 1
#define ENABLE_TRACING_FUNC 1
#define ENABLE_TRACING_IC 1

#if INTEL_PROFILING
#include </opt/intel/vtune_amplifier_xe_2013/include/jitprofiling.h>
#else
typedef void* iJIT_Method_Load;
#endif

namespace pyston {

namespace {

static BoxedClass* astinterpreter_cls;

class ASTInterpreter;

// Map from stack frame pointers for frames corresponding to ASTInterpreter::execute() to the ASTInterpreter handling
// them. Used to look up information about that frame. This is used for getting tracebacks, for CPython introspection
// (sys._getframe & co), and for GC scanning.
static std::unordered_map<void*, ASTInterpreter*> s_interpreterMap;
static_assert(THREADING_USE_GIL, "have to make the interpreter map thread safe!");

class RegisterHelper {
private:
    void* frame_addr;
    ASTInterpreter* interpreter;

public:
    RegisterHelper();
    ~RegisterHelper();
    void doRegister(void* frame_addr, ASTInterpreter* interpreter);
    static void deregister(void* frame_addr);
};

union Value {
    bool b;
    int64_t n;
    double d;
    Box* o;

    Value(bool b) : b(b) {}
    Value(int64_t n = 0) : n(n) {}
    Value(double d) : d(d) {}
    Value(Box* o) : o(o) {
        if (DEBUG >= 2)
            ASSERT(gc::isValidGCObject(o), "%p", o);
    }
};

llvm::DenseSet<CFGBlock*> tracers_aborted;

class ASTInterpreter : public Box {
public:
    typedef ContiguousMap<InternedString, Box*> SymMap;

    ASTInterpreter(CompiledFunction* compiled_function);

    void initArguments(int nargs, BoxedClosure* closure, BoxedGenerator* generator, Box* arg1, Box* arg2, Box* arg3,
                       Box** args);

    static Value execute(ASTInterpreter& interpreter, CFGBlock* start_block = NULL, AST_stmt* start_at = NULL);
    // This must not be inlined, because we rely on being able to detect when we're inside of it (by checking whether
    // %rip is inside its instruction range) during a stack-trace in order to produce tracebacks inside interpreted
    // code.
    __attribute__((__no_inline__)) __attribute__((noinline)) static Value
        executeInner(ASTInterpreter& interpreter, CFGBlock* start_block, AST_stmt* start_at, RegisterHelper* reg);


private:
    Box* createFunction(AST* node, AST_arguments* args, const std::vector<AST_stmt*>& body);
    Value doBinOp(Box* left, Box* right, int op, BinExpType exp_type);
    void doStore(AST_expr* node, Value value);
    void doStore(InternedString name, Value value);

    Value visit_assert(AST_Assert* node);
    Value visit_assign(AST_Assign* node);
    Value visit_binop(AST_BinOp* node);
    Value visit_call(AST_Call* node);
    Value visit_compare(AST_Compare* node);
    Value visit_delete(AST_Delete* node);
    Value visit_exec(AST_Exec* node);
    Value visit_global(AST_Global* node);
    Value visit_module(AST_Module* node);
    Value visit_print(AST_Print* node);
    Value visit_raise(AST_Raise* node);
    Value visit_return(AST_Return* node);
    Value visit_stmt(AST_stmt* node);
    Value visit_unaryop(AST_UnaryOp* node);

    Value visit_attribute(AST_Attribute* node);
    Value visit_dict(AST_Dict* node);
    Value visit_expr(AST_expr* node);
    Value visit_expr(AST_Expr* node);
    Value visit_extslice(AST_ExtSlice* node);
    Value visit_index(AST_Index* node);
    Value visit_lambda(AST_Lambda* node);
    Value visit_list(AST_List* node);
    Value visit_name(AST_Name* node);
    Value visit_num(AST_Num* node);
    Value visit_repr(AST_Repr* node);
    Value visit_set(AST_Set* node);
    Value visit_slice(AST_Slice* node);
    Value visit_str(AST_Str* node);
    Value visit_subscript(AST_Subscript* node);
    Value visit_tuple(AST_Tuple* node);
    Value visit_yield(AST_Yield* node);

    Value visit_makeClass(AST_MakeClass* node);
    Value visit_makeFunction(AST_MakeFunction* node);

    // pseudo
    Value visit_augBinOp(AST_AugBinOp* node);
    Value visit_branch(AST_Branch* node);
    Value visit_clsAttribute(AST_ClsAttribute* node);
    Value visit_invoke(AST_Invoke* node);
    Value visit_jump(AST_Jump* node);
    Value visit_langPrimitive(AST_LangPrimitive* node);

    CompiledFunction* compiled_func;
    SourceInfo* source_info;
    ScopeInfo* scope_info;
    PhiAnalysis* phis;

    SymMap sym_table;
    CFGBlock* next_block, *current_block;
    AST_stmt* current_inst;
    ExcInfo last_exception;
    BoxedClosure* passed_closure, *created_closure;
    BoxedGenerator* generator;
    unsigned edgecount;
    FrameInfo frame_info;

    // This is either a module or a dict
    Box* globals;
    void* frame_addr; // used to clear entry inside the s_interpreterMap on destruction

public:
    DEFAULT_CLASS_SIMPLE(astinterpreter_cls);

    AST_stmt* getCurrentStatement() {
        assert(current_inst);
        return current_inst;
    }

    Box* getGlobals() {
        assert(globals);
        return globals;
    }

    CompiledFunction* getCF() { return compiled_func; }
    FrameInfo* getFrameInfo() { return &frame_info; }
    BoxedClosure* getPassedClosure() { return passed_closure; }
    const SymMap& getSymbolTable() { return sym_table; }
    const ScopeInfo* getScopeInfo() { return scope_info; }

    void addSymbol(InternedString name, Box* value, bool allow_duplicates);
    void setGenerator(Box* gen);
    void setPassedClosure(Box* closure);
    void setCreatedClosure(Box* closure);
    void setBoxedLocals(Box*);
    void setFrameInfo(const FrameInfo* frame_info);
    void setGlobals(Box* globals);

    static void gcHandler(GCVisitor* visitor, Box* box);
    static void simpleDestructor(Box* box) {
        ASTInterpreter* inter = (ASTInterpreter*)box;
        assert(inter->cls == astinterpreter_cls);
        if (inter->frame_addr)
            RegisterHelper::deregister(inter->frame_addr);
        inter->~ASTInterpreter();
    }

    friend class RegisterHelper;
    friend class JitFragment;
    friend class JitedCode;

    std::unique_ptr<class JitFragment> tracer;

    void abortTracing();
    void startTracing(CFGBlock* block);

    LivenessAnalysis* getLivness() {
        if (!source_info->liveness_info)
            source_info->liveness_info = computeLivenessInfo(source_info->cfg);
        return source_info->liveness_info.get();
    }



    static Box* tracerHelperGetLocal(ASTInterpreter* i, InternedString id) {
        SymMap::iterator it = i->sym_table.find(id);
        if (it != i->sym_table.end()) {
            return i->sym_table.getMapped(it->second);
        }

        assertNameDefined(0, id.c_str(), UnboundLocalError, true);
        return 0;
    }

    static void tracerHelperSetLocal(ASTInterpreter* i, InternedString id, Box* v, bool set_closure) {
        i->sym_table[id] = v;

        if (set_closure) {
            i->created_closure->elts[i->scope_info->getClosureOffset(id)] = v;
        }
    }
};

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


struct JitCallHelper {
    struct Regs {
        const assembler::Register regs[6]
            = { assembler::RDI, assembler::RSI, assembler::RDX, assembler::RCX, assembler::R8, assembler::R9 };
    };

    template <int offset> struct PopO : public Regs {
        int emit(assembler::Assembler& a, int arg_num) {
            a.pop(regs[offset == -1 ? arg_num : offset]);
            return -1;
        }
    };

    struct Pop : public PopO<-1> {};

    struct Rsp : public Regs {
        int emit(assembler::Assembler& a, int arg_num) {
            a.mov(assembler::RSP, regs[arg_num]);
            return 0;
        }
    };

    struct Imm : public Regs {
        const uint64_t val;
        Imm(int64_t val) : val(val) {}
        Imm(void* val) : val((uint64_t)val) {}
        int emit(assembler::Assembler& a, int arg_num) {
            a.mov(assembler::Immediate(val), regs[arg_num]);
            return 0;
        }
    };

    struct Mem : public Regs {
        const assembler::Indirect ind;
        Mem(assembler::Indirect ind) : ind(ind) {}
        int emit(assembler::Assembler& a, int arg_num) {
            a.mov(ind, regs[arg_num]);
            return 0;
        }
    };

    template <int arg_num, typename Arg1, typename... Args>
    static int emitCallHelper(assembler::Assembler& a, Arg1&& arg1, Args... args) {
        int stack_level = arg1.emit(a, arg_num);
        return stack_level + emitCallHelper<arg_num + 1>(a, args...);
    }

    template <int arg_num> static int emitCallHelper(assembler::Assembler& a) {
        static_assert(arg_num <= 6, "too many args");
        return 0;
    }
};

class JitFragment : public JitCallHelper {
private:
    assembler::Assembler& a;
    CFGBlock* block;
    int epilog_offset;
    int stack_level;
    void* code, *entry_code;
    std::function<void(void)> abort_callback;
    bool finished;
    bool& iscurrently_tracing;


public:
    iJIT_Method_Load* jmethod;
    JitFragment(CFGBlock* block, assembler::Assembler& a, int epilog_offset, std::function<void(void)> abort_callback,
                void* entry_code, bool& iscurrently_tracing)
        : a(a),
          block(block),
          epilog_offset(epilog_offset),
          stack_level(0),
          code(0),
          entry_code(entry_code),
          abort_callback(abort_callback),
          finished(false),
          iscurrently_tracing(iscurrently_tracing),
          jmethod(0) {
        code = (void*)a.curInstPointer();
        emitBlockEntry();
    }

    llvm::DenseMap<InternedString, uint64_t> local_syms;

    CFGBlock* getBlock() { return block; }

    uint64_t asArg(InternedString s) {
        union U {
            U() {}
            InternedString is;
            uint64_t s;
        };
        U u;
        u.is = s;
        return u.s;
    }

    template <typename... Args> void emitCall(void* func, Args&&... args) {
        emitVoidCall(func, args...);
        pushResult();
    }

    template <typename... Args> void emitVarArgCall(void* func, uint64_t stack_adjustment, Args&&... args) {
        emitVoidVarArgCall(func, stack_adjustment, args...);
        pushResult();
    }

    template <typename... Args> void emitVoidCall(void* func, Args&&... args) {
        return emitVoidVarArgCall(func, 0, args...);
    }

    template <typename... Args> void emitVoidVarArgCall(void* func, uint64_t stack_adjustment, Args&&... args) {
        stack_level += emitCallHelper<0>(a, args...);
        if (stack_level & 1) { // we have to align the stack :-(
            ++stack_adjustment;
            a.sub(assembler::Immediate(8), assembler::RSP);
            ++stack_level;
        }
        a.emitCall(func, assembler::R11);
        if (stack_adjustment) {
            a.add(assembler::Immediate(stack_adjustment * 8), assembler::RSP);
            stack_level -= stack_adjustment;
        }
    }

    void pushResult() {
        a.push(assembler::RAX);
        ++stack_level;
    }

    assembler::Indirect getInterp() { return assembler::Indirect(assembler::RBP, -8); }
    assembler::Indirect getCurInst() { return assembler::Indirect(assembler::RBP, -16); }

    void emitBlockEntry() {
        for (int i = 0; i < 7; ++i)
            a.nop();
    }


    static Box* setGlobalICHelper(SetGlobalIC* ic, Box* o, BoxedString* s, Box* v) { return ic->call(o, s, v); }

    void emitSetGlobal(Box* global, BoxedString* s) {
#if ENABLE_TRACING_IC
        emitVoidCall((void*)setGlobalICHelper, Imm((void*)new SetGlobalIC), Imm((void*)global), Imm((void*)s), Pop());
#else
        emitVoidCall((void*)getGlobal, Imm((void*)global), Imm((void*)s), Pop());
#endif
    }

    static Box* getGlobalICHelper(GetGlobalIC* ic, Box* o, BoxedString* s) { return ic->call(o, s); }

    void emitGetGlobal(Box* global, BoxedString* s) {
#if ENABLE_TRACING_IC
        emitCall((void*)getGlobalICHelper, Imm((void*)new GetGlobalIC), Imm((void*)global), Imm((void*)s));
#else
        emitCall((void*)getGlobal, Imm((void*)global), Imm((void*)s));
#endif
    }

    static Box* setAttrICHelper(SetAttrIC* ic, Box* o, BoxedString* attr, Box* value) {
        return ic->call(o, attr, value);
    }

    void emitSetAttr(BoxedString* s) {
#if ENABLE_TRACING_IC
        emitVoidCall((void*)setAttrICHelper, Imm((void*)new SetAttrIC), Pop(), Imm((void*)s), Pop());
#else
        emitVoidCall((void*)setattr, Pop(), Imm((void*)s), Pop());
#endif
        assert(stack_level == 0);
    }

    static Box* getAttrICHelper(GetAttrIC* ic, Box* o, BoxedString* attr) { return ic->call(o, attr); }

    void emitGetAttr(BoxedString* s) {
#if ENABLE_TRACING_IC
        emitCall((void*)getAttrICHelper, Imm((void*)new GetAttrIC), Pop(), Imm((void*)s));
#else
        emitCall((void*)getattr, Pop(), Imm((void*)s));
#endif
    }

    void emitGetLocal(InternedString s) {
        emitCall((void*)ASTInterpreter::tracerHelperGetLocal, Mem(getInterp()), Imm(asArg(s)));
    }
    void emitSetLocal(InternedString s, bool set_closure) {
        emitVoidCall((void*)ASTInterpreter::tracerHelperSetLocal, Mem(getInterp()), Imm(asArg(s)), Pop(),
                     Imm((uint64_t)set_closure));
    }

    assembler::Indirect getLocalSym(uint64_t offset) {
        return assembler::Indirect(assembler::RBP, -16 - ((offset + 1) * 8));
    }

    void emitSetDeadLocal(InternedString s) {
        uint64_t offset = 0;
        auto it = local_syms.find(s);
        if (it == local_syms.end())
            local_syms[s] = offset = local_syms.size();
        else
            offset = it->second;
        // we could do much better...
        a.pop(assembler::RAX);
        --stack_level;
        a.mov(assembler::RAX, getLocalSym(offset));
    }

    void emitGetDeadLocal(InternedString s) {
        uint64_t offset = 0;
        bool found = false;
        auto it = local_syms.find(s);
        if (it == local_syms.end())
            local_syms[s] = offset = local_syms.size();
        else {
            offset = it->second;
            found = true;
        }

        if (!found) {
            // HACK:
            emitCall((void*)ASTInterpreter::tracerHelperGetLocal, Mem(getInterp()), Imm(asArg(s)));
            emitSetDeadLocal(s);
        }
        // we could do much better...
        a.mov(getLocalSym(offset), assembler::RAX);
        a.push(assembler::RAX);
        ++stack_level;
    }

    static Box* boxedLocalsGetHelper(ASTInterpreter* interp, BoxedString* s) {
        return boxedLocalsGet(interp->frame_info.boxedLocals, s, interp->globals);
    }

    void emitBoxedLocalsGet(BoxedString* s) { emitCall((void*)boxedLocalsGetHelper, Mem(getInterp()), Imm((void*)s)); }

    static Box* setitemICHelper(SetItemIC* ic, Box* o, Box* attr, Box* value) { return ic->call(o, attr, value); }

    void emitSetItem() {
#if ENABLE_TRACING_IC
        emitVoidCall((void*)setitemICHelper, Imm((void*)new SetItemIC), PopO<2>(), PopO<1>(), PopO<3>());
#else
        emitVoidCall((void*)setitem, PopO<1>(), PopO<0>(), PopO<2>());
#endif
    }

    static Box* getitemICHelper(GetItemIC* ic, Box* o, Box* attr) { return ic->call(o, attr); }

    void emitGetItem() {
#if ENABLE_TRACING_IC
        emitCall((void*)getitemICHelper, Imm((void*)new GetItemIC), PopO<2>(), PopO<1>());
#else
        emitCall((void*)getitem, PopO<1>(), PopO<0>());
#endif
    }

    static void setItemNameHelper(ASTInterpreter* interp, Box* str, Box* val) {
        assert(interp->frame_info.boxedLocals != NULL);
        setitem(interp->frame_info.boxedLocals, str, val);
    }

    void emitSetItemName(BoxedString* s) {
        emitVoidCall((void*)setItemNameHelper, Mem(getInterp()), Imm((void*)s), Pop());
        assert(stack_level == 0);
    }


    static Box* derefHelper(ASTInterpreter* i, InternedString s) {
        DerefInfo deref_info = i->scope_info->getDerefInfo(s);
        assert(i->passed_closure);
        BoxedClosure* closure = i->passed_closure;
        for (int i = 0; i < deref_info.num_parents_from_passed_closure; i++) {
            closure = closure->parent;
        }
        Box* val = closure->elts[deref_info.offset];
        if (val == NULL) {
            raiseExcHelper(NameError, "free variable '%s' referenced before assignment in enclosing scope", s.c_str());
        }
        return val;
    }

    void emitDeref(InternedString s) { emitCall((void*)derefHelper, Mem(getInterp()), Imm(asArg(s))); }

    static BoxedTuple* createTupleHelper(uint64_t num, Box** data) {
        BoxedTuple* tuple = (BoxedTuple*)BoxedTuple::create(num);
        for (uint64_t i = 0; i < num; ++i)
            tuple->elts[i] = data[num - i - 1];
        return tuple;
    }

    void emitCreateTuple(uint64_t num) {
        if (num == 0)
            emitCall((void*)BoxedTuple::create0);
        else if (num == 1)
            emitCall((void*)BoxedTuple::create1, Pop());
        else if (num == 2)
            emitCall((void*)BoxedTuple::create2, PopO<1>(), PopO<0>());
        else if (num == 3)
            emitCall((void*)BoxedTuple::create3, PopO<2>(), PopO<1>(), PopO<0>());
        else if (num == 4)
            emitCall((void*)BoxedTuple::create4, PopO<3>(), PopO<2>(), PopO<1>(), PopO<0>());
        else if (num == 5)
            emitCall((void*)BoxedTuple::create5, PopO<4>(), PopO<3>(), PopO<2>(), PopO<1>(), PopO<0>());
        else
            emitVarArgCall((void*)createTupleHelper, num, Imm(num), Rsp());
    }

    static BoxedList* createListHelper(uint64_t num, Box** data) {
        BoxedList* list = (BoxedList*)createList();
        list->ensure(num);
        for (uint64_t i = 0; i < num; ++i)
            listAppendInternal(list, data[num - i - 1]);
        return list;
    }

    void emitCreateList(uint64_t num) {
        if (num == 0)
            emitCall((void*)createList);
        else
            emitVarArgCall((void*)createListHelper, num, Imm(num), Rsp());
    }

    void emitCreateDict() { emitCall((void*)createDict); }
    void emitCreateSlice() { emitCall((void*)createSlice, PopO<2>(), PopO<1>(), PopO<0>()); }

    static Box* nonzeroHelper(Box* b) {
        bool result = b->nonzeroIC();
        if (result == true)
            return True;
        return False;
    }

    void emitNonzero() { emitCall((void*)nonzeroHelper, Pop()); }

    static Box* notHelper(Box* b) {
        bool result = b->nonzeroIC();
        if (result == true)
            return False;
        return True;
    }

    void emitNotNonzero() { emitCall((void*)notHelper, Pop()); }
    void emitGetPystonIter() { emitCall((void*)getPystonIter, Pop()); }
    void emitUnpackIntoArray(uint64_t num) {
        emitVoidCall((void*)unpackIntoArray, Pop(), Imm(num));
        for (int i = 0; i < num; ++i) {
            a.mov(assembler::Indirect(assembler::RAX, (num - i - 1) * 8), assembler::RDI);
            a.push(assembler::RDI);
            ++stack_level;
        }
    }

    static Box* compareICHelper(CompareIC* ic, Box* lhs, Box* rhs, int op) { return ic->call(lhs, rhs, op); }

    void emitCompare(int op) {
#if ENABLE_TRACING_IC
        emitCall((void*)compareICHelper, Imm((void*)new CompareIC), PopO<2>(), PopO<1>(), Imm(op));
#else
        emitCall((void*)compare, PopO<1>(), PopO<0>(), Imm(op));
#endif
    }

    static Box* augbinopICHelper(AugBinopIC* ic, Box* lhs, Box* rhs, int op) { return ic->call(lhs, rhs, op); }
    void emitAugbinop(int op) {
#if ENABLE_TRACING_IC
        emitCall((void*)augbinopICHelper, Imm((void*)new AugBinopIC), PopO<2>(), PopO<1>(), Imm(op));
#else
        emitCall((void*)augbinop, PopO<1>(), PopO<0>(), Imm(op));
#endif
    }

    static Box* binopICHelper(BinopIC* ic, Box* lhs, Box* rhs, int op) { return ic->call(lhs, rhs, op); }
    void emitBinop(int op) {
#if ENABLE_TRACING_IC
        emitCall((void*)binopICHelper, Imm((void*)new BinopIC), PopO<2>(), PopO<1>(), Imm(op));
#else
        emitCall((void*)binop, PopO<1>(), PopO<0>(), Imm(op));
#endif
    }

    static Box* unaryopICHelper(UnaryopIC* ic, Box* obj, int op) { return ic->call(obj, op); }
    void emitUnaryop(int op) {
#if ENABLE_TRACING_IC
        emitCall((void*)unaryopICHelper, Imm((void*)new UnaryopIC), Pop(), Imm(op));
#else
        emitCall((void*)unaryop, Pop(), Imm(op));
#endif
    }

    static Box* yieldHelper(ASTInterpreter* inter, Box* val) { return yield(inter->generator, val); }

    void emitYield(bool hasvalue) {
        if (hasvalue)
            emitCall((void*)yieldHelper, Mem(getInterp()), Pop());
        else
            emitCall((void*)yieldHelper, Mem(getInterp()), Imm((void*)None));
    }

    static Box* uncacheExcInfoHelper(ASTInterpreter* inter) {
        inter->getFrameInfo()->exc = ExcInfo(NULL, NULL, NULL);
        return None;
    }

    void emitUncacheExcInfo() { emitCall((void*)uncacheExcInfoHelper, Mem(getInterp())); }
    // TODO remove the gc hack...
    void emitInt(long n) { emitPush((uint64_t)PyGC_AddRoot(boxInt(n))); }
    void emitFloat(double n) { emitPush((uint64_t)PyGC_AddRoot(boxFloat(n))); }
    void emitLong(llvm::StringRef s) { emitPush((uint64_t)PyGC_AddRoot(createLong(s))); }
    void emitUnicodeStr(llvm::StringRef s) {
        emitCall((void*)decodeUTF8StringPtr, Imm((void*)s.data()), Imm(s.size()));
    }

#if ENABLE_TRACING_IC
    static Box* callattrHelperIC(ArgPassSpec argspec, BoxedString* attr, CallattrFlags flags, Box** args,
                                 std::vector<BoxedString*>* keyword_names, CallattrIC* ic) {
        int num = argspec.totalPassed();
        Box* obj = args[num];
        Box* arg1 = (num > 0) ? (Box*)args[num - 1] : NULL;
        Box* arg2 = (num > 1) ? (Box*)args[num - 2] : NULL;
        Box* arg3 = (num > 2) ? (Box*)args[num - 3] : NULL;
        llvm::SmallVector<Box*, 4> addition_args;
        for (int i = 3; i < num; ++i) {
            addition_args.push_back((Box*)args[num - i - 1]);
        }
        return ic->call(obj, attr, flags, argspec, arg1, arg2, arg3, addition_args.size() ? &addition_args[0] : NULL,
                        keyword_names);
    }
#endif
    static Box* callattrHelper(ArgPassSpec argspec, BoxedString* attr, CallattrFlags flags, Box** args,
                               std::vector<BoxedString*>* keyword_names) {
        int num = argspec.totalPassed();
        Box* obj = args[num];
        Box* arg1 = (num > 0) ? (Box*)args[num - 1] : NULL;
        Box* arg2 = (num > 1) ? (Box*)args[num - 2] : NULL;
        Box* arg3 = (num > 2) ? (Box*)args[num - 3] : NULL;
        llvm::SmallVector<Box*, 4> addition_args;
        for (int i = 3; i < num; ++i) {
            addition_args.push_back((Box*)args[num - i - 1]);
        }
        return pyston::callattr(obj, attr, flags, argspec, arg1, arg2, arg3,
                                addition_args.size() ? &addition_args[0] : NULL, keyword_names);
    }


    void emitCallattr(BoxedString* attr, CallattrFlags flags, ArgPassSpec argspec,
                      std::vector<BoxedString*>* keyword_names) {
        // We could make this faster but for now: keep it simple, stupid...
        int num_stack_args = argspec.totalPassed() + 1;
        assert(num_stack_args == stack_level);
#if ENABLE_TRACING_IC
        if (argspec.totalPassed() >= 3
            || keyword_names) // looks like runtime ICs with 7 or more args don't work right now..
            emitVarArgCall((void*)callattrHelper, num_stack_args, Imm(argspec.asInt()), Imm((void*)attr),
                           Imm(flags.asInt()), Rsp(), Imm((void*)keyword_names));
        else
            emitVarArgCall((void*)callattrHelperIC, num_stack_args, Imm(argspec.asInt()), Imm((void*)attr),
                           Imm(flags.asInt()), Rsp(), Imm((void*)keyword_names), Imm((void*)new CallattrIC));
#else
        emitVarArgCall((void*)callattrHelper, num_stack_args, Imm(argspec.asInt()), Imm((void*)attr),
                       Imm(flags.asInt()), Rsp(), Imm((void*)keyword_names));
#endif
    }


#if ENABLE_TRACING_IC
    static Box* runtimeCallHelperIC(ArgPassSpec argspec, Box** args, std::vector<BoxedString*>* keyword_names,
                                    RuntimeCallIC* ic) {
        int num = argspec.totalPassed();
        Box* obj = args[num];
        Box* arg1 = (num > 0) ? (Box*)args[num - 1] : NULL;
        Box* arg2 = (num > 1) ? (Box*)args[num - 2] : NULL;
        Box* arg3 = (num > 2) ? (Box*)args[num - 3] : NULL;
        llvm::SmallVector<Box*, 4> addition_args;
        for (int i = 3; i < num; ++i) {
            addition_args.push_back((Box*)args[num - i - 1]);
        }
        return ic->call(obj, argspec, arg1, arg2, arg3, addition_args.size() ? &addition_args[0] : NULL, keyword_names);
    }
#endif
    static Box* runtimeCallHelper(ArgPassSpec argspec, Box** args, std::vector<BoxedString*>* keyword_names) {
        int num = argspec.totalPassed();
        Box* obj = args[num];
        Box* arg1 = (num > 0) ? (Box*)args[num - 1] : NULL;
        Box* arg2 = (num > 1) ? (Box*)args[num - 2] : NULL;
        Box* arg3 = (num > 2) ? (Box*)args[num - 3] : NULL;
        llvm::SmallVector<Box*, 4> addition_args;
        for (int i = 3; i < num; ++i) {
            addition_args.push_back((Box*)args[num - i - 1]);
        }
        return runtimeCall(obj, argspec, arg1, arg2, arg3, addition_args.size() ? &addition_args[0] : NULL,
                           keyword_names);
    }


    void emitRuntimeCall(ArgPassSpec argspec, std::vector<BoxedString*>* keyword_names) {
        // We could make this faster but for now: keep it simple, stupid..
        int num_stack_args = argspec.totalPassed() + 1;
        assert(num_stack_args == stack_level);
#if ENABLE_TRACING_IC
        if (keyword_names) // looks like runtime ICs with 7 or more args don't work right now..
            emitVarArgCall((void*)runtimeCallHelper, num_stack_args, Imm(argspec.asInt()), Rsp(),
                           Imm((void*)keyword_names), Imm((void*)new RuntimeCallIC));
        else
            emitVarArgCall((void*)runtimeCallHelperIC, num_stack_args, Imm(argspec.asInt()), Rsp(),
                           Imm((void*)keyword_names), Imm((void*)new RuntimeCallIC));
#else
        emitVarArgCall((void*)runtimeCallHelper, num_stack_args, Imm(argspec.asInt()), Rsp(),
                       Imm((void*)keyword_names));
#endif
    }

    void emitPush(uint64_t val) {
        a.mov(assembler::Immediate(val), assembler::RSI);
        a.push(assembler::RSI);
        ++stack_level;
    }

    void emitPop() {
        a.pop(assembler::RAX);
        --stack_level;
    }

    static Box* hasnextHelper(Box* b) { return boxBool(pyston::hasnext(b)); }

    void emitHasnext() { emitCall((void*)hasnextHelper, Pop()); }

    static void setCurrentInstHelper(ASTInterpreter* interp, AST_stmt* node) { interp->current_inst = node; }

    void emitSetCurrentInst(AST_stmt* node) {
        assert(stack_level == 0);

        // dont call the helper to save a few bytes...
        emitVoidCall((void*)setCurrentInstHelper, Mem(getInterp()), Imm((void*)node));
        /*
        assert((uint32_t)(uint64_t)node == (uint64_t)node && "only support 32 offset for now");
        a.mov(getCurInst(), assembler::RDX);
        a.movq(assembler::Immediate((void*)node), assembler::Indirect(assembler::RDX, 0));
        */
    }

    void addGuard(bool t, CFGBlock* cont) {
        a.pop(assembler::RSI);
        stack_level--;

        // a.trap();
        a.mov(assembler::Immediate((void*)(t ? False : True)), assembler::RDI);
        a.cmp(assembler::RSI, assembler::RDI);

        uint64_t num_scatch_bytes = local_syms.size() * 8;
        int code_len_exit = 0;
        if (num_scatch_bytes >= 120)
            code_len_exit = 7;
        else if (num_scatch_bytes > 0)
            code_len_exit = 4;

        a.jne(assembler::JumpDestination::fromStart(a.bytesWritten() + 17 + 11 + 1 + code_len_exit));

        emitBlockExit();

        assert(stack_level == 0);
        a.mov(assembler::Immediate(cont), assembler::RAX);
        a.mov(assembler::Indirect(assembler::RAX, 8), assembler::RSI);
        a.test(assembler::RSI, assembler::RSI);
        a.je(assembler::JumpDestination::fromStart(a.bytesWritten() + 4 + 1));
        a.emitByte(0xFF);
        a.emitByte(0x60);
        a.emitByte(0x08); // jmp qword ptr [rax+8]
        a.jmp(assembler::JumpDestination::fromStart(epilog_offset));
    }

    void emitJump(CFGBlock* b) {
        assert(stack_level == 0);

        emitBlockExit();

        if (b->code) {
            a.jmp(assembler::JumpDestination::fromStart(((uint64_t)b->code) - (uint64_t)a.getStartAddr()));
        } else {
            // TODO we could patch this later...
            a.mov(assembler::Immediate(b), assembler::RAX);
            a.mov(assembler::Indirect(assembler::RAX, 8), assembler::RSI);
            a.test(assembler::RSI, assembler::RSI);
            a.je(assembler::JumpDestination::fromStart(a.bytesWritten() + 4 + 1));
            a.emitByte(0xFF);
            a.emitByte(0x60);
            a.emitByte(0x08); // jmp qword ptr [rax+8]
            a.jmp(assembler::JumpDestination::fromStart(epilog_offset));
        }
    }

    void emitReturn() {
        assert(stack_level == 1);
        a.mov(assembler::Immediate(0ul), assembler::RAX);
        a.pop(assembler::RDX);
        emitBlockExit();
        a.jmp(assembler::JumpDestination::fromStart(epilog_offset));
        --stack_level;
    }

    void abort() {
        if (finished)
            return;
        a.setCurInstPointer((uint8_t*)code);
        a.mov(assembler::Immediate(block), assembler::RAX);
        a.jmp(assembler::JumpDestination::fromStart(epilog_offset));

        RELEASE_ASSERT(!a.hasFailed(), "");
        llvm::sys::Memory::InvalidateInstructionCache(code, (uint64_t)a.curInstPointer() - (uint64_t)code);

#if INTEL_PROFILING
        jmethod->method_load_address = code;
        jmethod->method_size = (uint64_t)a.curInstPointer() - (uint64_t)code;
        iJIT_NotifyEvent(iJVM_EVENT_TYPE_METHOD_LOAD_FINISHED, (void*)jmethod);
#endif

        stack_level = 0;
        finished = true;
        iscurrently_tracing = false;
    }

    void emitBlockExit() {
        uint64_t num_scatch = local_syms.size();
        if (num_scatch & 1) // align
            ++num_scatch;
        if (num_scatch)
            a.add(assembler::Immediate(8 * num_scatch), assembler::RSP);
    }

    void compile() {
        if (finished)
            return;
        iscurrently_tracing = false;

        if (a.hasFailed()) {
            RELEASE_ASSERT(!a.hasFailed(), "");
            a.resetFailed();
            if (abort_callback)
                abort_callback();
            tracers_aborted.insert(block);
            abort();
            return;
        }

        auto* cur_end = a.curInstPointer();
        a.setCurInstPointer((uint8_t*)code);
        uint64_t num_scatch = local_syms.size();
        if (num_scatch & 1) // align
            ++num_scatch;
        if (num_scatch)
            a.sub(assembler::Immediate(8 * num_scatch), assembler::RSP);
        a.setCurInstPointer(cur_end);

        assert(stack_level == 0);
        RELEASE_ASSERT(!a.hasFailed(), "asm failed");
        llvm::sys::Memory::InvalidateInstructionCache(code, (uint64_t)a.curInstPointer() - (uint64_t)code);

#if INTEL_PROFILING
        jmethod->method_load_address = code;
        jmethod->method_size = (uint64_t)a.curInstPointer() - (uint64_t)code;
        iJIT_NotifyEvent(iJVM_EVENT_TYPE_METHOD_LOAD_FINISHED, (void*)jmethod);
#endif

        block->code = code;
        if (entry_code)
            block->entry_code = entry_code;

        finished = true;
    }
};

static void* myalloc(uint64_t size) {
    llvm_error_code ec;
    llvm::sys::MemoryBlock MB = llvm::sys::Memory::allocateMappedMemory(
        size, 0, llvm::sys::Memory::MF_READ | llvm::sys::Memory::MF_WRITE, ec);
    return MB.base();
}

class JitedCode {
    static constexpr int code_size = 4096 * 15;
    static constexpr int epilog_size = 2;

    void* code;
    int entry_offset;
    int epilog_offset;
    assembler::Assembler a;
    bool iscurrently_tracing;
    std::function<void(void)> abort_callback;
    iJIT_Method_Load jmethod;

public:
    JitedCode(llvm::StringRef name, std::function<void(void)> abort_callback)
        : // code(malloc(code_size)),
          code(myalloc(code_size)),
          entry_offset(0),
          epilog_offset(0),
          a((uint8_t*)code, code_size - epilog_size),
          iscurrently_tracing(false),
          abort_callback(abort_callback) {
        // emit prolog
        a.push(assembler::RBP);
        a.mov(assembler::RSP, assembler::RBP);

        // create stack layout:
        // [RBP- 8] -> ASTInterpreter*
        // [RBP-16] -> &ASTInterpreter->current_inst
        a.push(assembler::RDI); // push interpreter pointer

        static_assert(offsetof(ASTInterpreter, current_inst) == 0x80, "");
        // lea rdi,[rdi+0x80]
        for (unsigned char c : { 0x48, 0x8d, 0xbf, 0x80, 0x00, 0x00, 0x00 })
            a.emitByte(c);
        // push &ASTInterpreter->current_inst
        a.push(assembler::RDI);

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


        g.func_addr_registry.registerFunction(("jit_" + name).str(), code, code_size, NULL);

#if INTEL_PROFILING
        iJIT_IsProfilingActiveFlags agent;

        memset(&jmethod, 0, sizeof(jmethod));

        /* Get the current mode of the profiler and check that it is sampling */
        if (iJIT_IsProfilingActive() != iJIT_SAMPLING_ON) {
            return; /* The profiler is not loaded */
        }

        /* Fill method information */
        jmethod.method_id = iJIT_GetNewMethodID();
        jmethod.method_name = (char*)name.data();
        jmethod.class_file_name = (char*)"JITter";
        jmethod.source_file_name = (char*)"jitter.cpp";
        jmethod.method_load_address = code;
        jmethod.method_size = code_size;

        /* Send method load notification */
        iJIT_NotifyEvent(iJVM_EVENT_TYPE_METHOD_LOAD_FINISHED, (void*)&jmethod);

/* Destroy the profiler */
#endif
    }

    std::unique_ptr<JitFragment> newFragment(CFGBlock* block) {
        if (iscurrently_tracing)
            return std::unique_ptr<JitFragment>();
        if (a.bytesLeft() < 50)
            return std::unique_ptr<JitFragment>();

        iscurrently_tracing = true;

        auto rtn = std::unique_ptr<JitFragment>(
            new JitFragment(block, a, epilog_offset, abort_callback, a.getStartAddr(), iscurrently_tracing));
        rtn->jmethod = &jmethod;
        return rtn;
    }


    static void writeTrivialEhFrame(void* eh_frame_addr, void* func_addr, uint64_t func_size) {
        memcpy(eh_frame_addr, _eh_frame_template, EH_FRAME_SIZE);

        int32_t* offset_ptr = (int32_t*)((uint8_t*)eh_frame_addr + 0x20);
        int32_t* size_ptr = (int32_t*)((uint8_t*)eh_frame_addr + 0x24);

        int64_t offset = (int8_t*)func_addr - (int8_t*)offset_ptr;
        assert(offset >= INT_MIN && offset <= INT_MAX);
        *offset_ptr = offset;

        assert(func_size <= UINT_MAX);
        *size_ptr = func_size;
    }

    void EHwriteAndRegister(void* func_addr, uint64_t func_size) {
        void* eh_frame_addr = 0;
        eh_frame_addr = myalloc(EH_FRAME_SIZE); // malloc(EH_FRAME_SIZE);
        writeTrivialEhFrame(eh_frame_addr, func_addr, func_size);
        // (EH_FRAME_SIZE - 4) to omit the 4-byte null terminator, otherwise we trip an assert in parseEhFrame.
        // TODO: can we omit the terminator in general?
        registerDynamicEhFrame((uint64_t)func_addr, func_size, (uint64_t)eh_frame_addr, EH_FRAME_SIZE - 4);
        registerEHFrames((uint8_t*)eh_frame_addr, (uint64_t)eh_frame_addr, EH_FRAME_SIZE);
    }
};


void ASTInterpreter::abortTracing() {
    if (tracer) {
        tracer->abort();
        tracers_aborted.insert(tracer->getBlock());
        tracer.reset();
        // printf("FAILED\n");
    }
}

void ASTInterpreter::addSymbol(InternedString name, Box* value, bool allow_duplicates) {
    if (!allow_duplicates)
        assert(sym_table.count(name) == 0);
    sym_table[name] = value;
}

void ASTInterpreter::setGenerator(Box* gen) {
    assert(!this->generator); // This should only used for initialization
    assert(gen->cls == generator_cls);
    this->generator = static_cast<BoxedGenerator*>(gen);
}

void ASTInterpreter::setPassedClosure(Box* closure) {
    assert(!this->passed_closure); // This should only used for initialization
    assert(closure->cls == closure_cls);
    this->passed_closure = static_cast<BoxedClosure*>(closure);
}

void ASTInterpreter::setCreatedClosure(Box* closure) {
    assert(!this->created_closure); // This should only used for initialization
    assert(closure->cls == closure_cls);
    this->created_closure = static_cast<BoxedClosure*>(closure);
}

void ASTInterpreter::setBoxedLocals(Box* boxedLocals) {
    this->frame_info.boxedLocals = boxedLocals;
}

void ASTInterpreter::setFrameInfo(const FrameInfo* frame_info) {
    this->frame_info = *frame_info;
}

void ASTInterpreter::setGlobals(Box* globals) {
    assert(gc::isValidGCObject(globals));
    this->globals = globals;
}

void ASTInterpreter::gcHandler(GCVisitor* visitor, Box* box) {
    boxGCHandler(visitor, box);

    ASTInterpreter* interp = (ASTInterpreter*)box;
    auto&& vec = interp->sym_table.vector();
    visitor->visitRange((void* const*)&vec[0], (void* const*)&vec[interp->sym_table.size()]);
    visitor->visit(interp->passed_closure);
    visitor->visit(interp->created_closure);
    visitor->visit(interp->generator);
    visitor->visit(interp->globals);
    visitor->visit(interp->source_info->parent_module);
    interp->frame_info.gcVisit(visitor);
}

ASTInterpreter::ASTInterpreter(CompiledFunction* compiled_function)
    : compiled_func(compiled_function),
      source_info(compiled_function->clfunc->source.get()),
      scope_info(0),
      phis(NULL),
      current_block(0),
      current_inst(0),
      last_exception(NULL, NULL, NULL),
      passed_closure(0),
      created_closure(0),
      generator(0),
      edgecount(0),
      frame_info(ExcInfo(NULL, NULL, NULL)),
      frame_addr(0) {

    CLFunction* f = compiled_function->clfunc;
    if (!source_info->cfg)
        source_info->cfg = computeCFG(f->source.get(), f->source->body);

    scope_info = source_info->getScopeInfo();

    assert(scope_info);
}

void ASTInterpreter::initArguments(int nargs, BoxedClosure* _closure, BoxedGenerator* _generator, Box* arg1, Box* arg2,
                                   Box* arg3, Box** args) {
    passed_closure = _closure;
    generator = _generator;

    if (scope_info->createsClosure())
        created_closure = createClosure(passed_closure, scope_info->getClosureSize());

    std::vector<Box*, StlCompatAllocator<Box*>> argsArray{ arg1, arg2, arg3 };
    for (int i = 3; i < nargs; ++i)
        argsArray.push_back(args[i - 3]);

    const ParamNames& param_names = compiled_func->clfunc->param_names;

    int i = 0;
    for (auto& name : param_names.args) {
        doStore(source_info->getInternedStrings().get(name), argsArray[i++]);
    }

    if (!param_names.vararg.str().empty()) {
        doStore(source_info->getInternedStrings().get(param_names.vararg), argsArray[i++]);
    }

    if (!param_names.kwarg.str().empty()) {
        doStore(source_info->getInternedStrings().get(param_names.kwarg), argsArray[i++]);
    }
}

RegisterHelper::RegisterHelper() : frame_addr(NULL), interpreter(NULL) {
}

RegisterHelper::~RegisterHelper() {
    assert(interpreter);
    assert(interpreter->frame_addr == frame_addr);
    interpreter->frame_addr = nullptr;
    deregister(frame_addr);
}

void RegisterHelper::doRegister(void* frame_addr, ASTInterpreter* interpreter) {
    assert(!this->interpreter);
    assert(!this->frame_addr);
    this->frame_addr = frame_addr;
    this->interpreter = interpreter;
    interpreter->frame_addr = frame_addr;
    s_interpreterMap[frame_addr] = interpreter;
}

void RegisterHelper::deregister(void* frame_addr) {
    assert(frame_addr);
    assert(s_interpreterMap.count(frame_addr));
    s_interpreterMap.erase(frame_addr);
}

void jitError() {
    printf("jit error!\n");
}

void ASTInterpreter::startTracing(CFGBlock* block) {
    // printf("Starting trace: %d\n", block->idx);
    if (!compiled_func->jitted_code) {
        compiled_func->jitted_code = (void*)(new JitedCode(source_info->getName(), jitError));
    }
    assert(!tracer);
    JitedCode* jitted_code = (JitedCode*)compiled_func->jitted_code;
    tracer = jitted_code->newFragment(block);
}

Value ASTInterpreter::executeInner(ASTInterpreter& interpreter, CFGBlock* start_block, AST_stmt* start_at,
                                   RegisterHelper* reg) {

    void* frame_addr = __builtin_frame_address(0);
    reg->doRegister(frame_addr, &interpreter);

    Value v;

    bool trace = false;
    bool from_start = start_block == NULL && start_at == NULL;

    assert((start_block == NULL) == (start_at == NULL));
    if (start_block == NULL) {
        start_block = interpreter.source_info->cfg->getStartingBlock();
        start_at = start_block->body[0];

        if (ENABLE_TRACING_FUNC && interpreter.compiled_func->times_called == 25 && !start_block->code) {
            if (tracers_aborted.count(start_block) == 0)
                trace = true;
        }
    }

    // Important that this happens after RegisterHelper:
    interpreter.current_inst = start_at;
    threading::allowGLReadPreemption();
    interpreter.current_inst = NULL;

    interpreter.current_block = start_block;

    bool started = false;
    if (trace && from_start) {
        interpreter.startTracing(start_block);
    }

    if (!from_start) {
        for (auto s : start_block->body) {
            if (!started) {
                if (s != start_at)
                    continue;
                started = true;
            }

            interpreter.current_inst = s;
            v = interpreter.visit_stmt(s);
        }
    } else {
        interpreter.next_block = interpreter.current_block;
    }

    bool was_tracing = false;
    while (interpreter.next_block) {
        interpreter.current_block = interpreter.next_block;
        interpreter.next_block = 0;

        if (ENABLE_TRACING && !interpreter.tracer) {
            CFGBlock* b = interpreter.current_block;
            if (b->entry_code) {
                was_tracing = true;

                try {
                    typedef std::pair<CFGBlock*, Box*>(*EntryFunc)(ASTInterpreter*, CFGBlock*);
                    std::pair<CFGBlock*, Box*> rtn = ((EntryFunc)(b->entry_code))(&interpreter, b);
                    interpreter.next_block = rtn.first;
                    if (!interpreter.next_block)
                        return rtn.second;
                } catch (ExcInfo e) {
                    AST_stmt* stmt = interpreter.getCurrentStatement();
                    if (stmt->type != AST_TYPE::Invoke)
                        throw e;

                    auto source = interpreter.getCF()->clfunc->source.get();
                    exceptionCaughtInInterpreter(
                        LineInfo(stmt->lineno, stmt->col_offset, source->fn, source->getName()), &e);

                    interpreter.next_block = ((AST_Invoke*)stmt)->exc_dest;
                    interpreter.last_exception = e;
                }
                continue;
            }
        }

#if ENABLE_TRACING
        if (was_tracing && !interpreter.tracer && tracers_aborted.count(interpreter.current_block) == 0) {
            assert(!interpreter.current_block->code);
            interpreter.startTracing(interpreter.current_block);
        }
#endif
        for (AST_stmt* s : interpreter.current_block->body) {
            interpreter.current_inst = s;
            if (interpreter.tracer)
                interpreter.tracer->emitSetCurrentInst(s);
            v = interpreter.visit_stmt(s);
        }
    }
    return v;
}

Value ASTInterpreter::execute(ASTInterpreter& interpreter, CFGBlock* start_block, AST_stmt* start_at) {
    STAT_TIMER(t0, "us_timer_astinterpreter_execute");

    RegisterHelper frame_registerer;

    return executeInner(interpreter, start_block, start_at, &frame_registerer);
}

Value ASTInterpreter::doBinOp(Box* left, Box* right, int op, BinExpType exp_type) {
    switch (exp_type) {
        case BinExpType::AugBinOp:
            if (tracer)
                tracer->emitAugbinop(op);
            return augbinop(left, right, op);
        case BinExpType::BinOp:
            if (tracer)
                tracer->emitBinop(op);
            return binop(left, right, op);
        case BinExpType::Compare:
            if (tracer)
                tracer->emitCompare(op);
            return compare(left, right, op);
        default:
            RELEASE_ASSERT(0, "not implemented");
    }
    return Value();
}

void ASTInterpreter::doStore(InternedString name, Value value) {
    ScopeInfo::VarScopeType vst = scope_info->getScopeTypeOfName(name);
    if (vst == ScopeInfo::VarScopeType::GLOBAL) {
        if (tracer)
            tracer->emitSetGlobal(globals, name.getBox());
        setGlobal(globals, name.getBox(), value.o);
    } else if (vst == ScopeInfo::VarScopeType::NAME) {
        if (tracer)
            tracer->emitSetItemName(name.getBox());

        assert(frame_info.boxedLocals != NULL);
        // TODO should probably pre-box the names when it's a scope that usesNameLookup
        setitem(frame_info.boxedLocals, name.getBox(), value.o);
    } else {
        bool closure = vst == ScopeInfo::VarScopeType::CLOSURE;
        if (tracer) {
            if (!closure) {
                bool is_live = getLivness()->isLiveAtEnd(name, current_block);
                if (is_live)
                    tracer->emitSetLocal(name, closure);
                else
                    tracer->emitSetDeadLocal(name);
            } else
                tracer->emitSetLocal(name, closure);
        }

        sym_table[name] = value.o;
        if (closure) {
            created_closure->elts[scope_info->getClosureOffset(name)] = value.o;
        }
    }
}

void ASTInterpreter::doStore(AST_expr* node, Value value) {
    if (node->type == AST_TYPE::Name) {
        AST_Name* name = (AST_Name*)node;
        doStore(name->id, value);
    } else if (node->type == AST_TYPE::Attribute) {
        AST_Attribute* attr = (AST_Attribute*)node;
        Value o = visit_expr(attr->value);
        if (tracer)
            tracer->emitSetAttr(attr->attr.getBox());
        pyston::setattr(o.o, attr->attr.getBox(), value.o);
    } else if (node->type == AST_TYPE::Tuple) {
        AST_Tuple* tuple = (AST_Tuple*)node;
        Box** array = unpackIntoArray(value.o, tuple->elts.size());

        if (tracer)
            tracer->emitUnpackIntoArray(tuple->elts.size());

        unsigned i = 0;
        for (AST_expr* e : tuple->elts)
            doStore(e, array[i++]);
    } else if (node->type == AST_TYPE::List) {
        abortTracing();
        AST_List* list = (AST_List*)node;
        Box** array = unpackIntoArray(value.o, list->elts.size());
        unsigned i = 0;
        for (AST_expr* e : list->elts)
            doStore(e, array[i++]);
    } else if (node->type == AST_TYPE::Subscript) {
        AST_Subscript* subscript = (AST_Subscript*)node;

        Value target = visit_expr(subscript->value);
        Value slice = visit_expr(subscript->slice);

        if (tracer)
            tracer->emitSetItem();

        setitem(target.o, slice.o, value.o);
    } else {
        RELEASE_ASSERT(0, "not implemented");
    }
}

Value ASTInterpreter::visit_unaryop(AST_UnaryOp* node) {
    Value operand = visit_expr(node->operand);
    if (node->op_type == AST_TYPE::Not) {
        if (tracer)
            tracer->emitNotNonzero();

        return boxBool(!nonzero(operand.o));
    } else {
        if (tracer)
            tracer->emitUnaryop(node->op_type);

        return unaryop(operand.o, node->op_type);
    }
}

Value ASTInterpreter::visit_binop(AST_BinOp* node) {
    Value left = visit_expr(node->left);
    Value right = visit_expr(node->right);
    return doBinOp(left.o, right.o, node->op_type, BinExpType::BinOp);
}

Value ASTInterpreter::visit_slice(AST_Slice* node) {
    Value lower = node->lower ? visit_expr(node->lower) : None;
    if (tracer && !node->lower)
        tracer->emitPush((uint64_t)None);
    Value upper = node->upper ? visit_expr(node->upper) : None;
    if (tracer && !node->upper)
        tracer->emitPush((uint64_t)None);
    Value step = node->step ? visit_expr(node->step) : None;
    if (tracer && !node->step)
        tracer->emitPush((uint64_t)None);

    if (tracer)
        tracer->emitCreateSlice();

    return createSlice(lower.o, upper.o, step.o);
}

Value ASTInterpreter::visit_extslice(AST_ExtSlice* node) {
    abortTracing();
    int num_slices = node->dims.size();
    BoxedTuple* rtn = BoxedTuple::create(num_slices);
    for (int i = 0; i < num_slices; ++i)
        rtn->elts[i] = visit_expr(node->dims[i]).o;
    return rtn;
}

Value ASTInterpreter::visit_branch(AST_Branch* node) {
    Value v = visit_expr(node->test);
    ASSERT(v.o == True || v.o == False, "Should have called NONZERO before this branch");

    if (tracer)
        tracer->addGuard(v.o == True, v.o == True ? node->iffalse : node->iftrue);
    if (v.o == True)
        next_block = node->iftrue;
    else
        next_block = node->iffalse;


    if (tracer) {
        // if (next_block->code)
        tracer->emitJump(next_block);
        tracer->compile();
        tracer.reset();
        if (!next_block->code) {
            startTracing(next_block);
            RELEASE_ASSERT(tracer, "");
        }
    }

    return Value();
}

Value ASTInterpreter::visit_jump(AST_Jump* node) {
    bool backedge = node->target->idx < current_block->idx && compiled_func;
    if (backedge) {
        threading::allowGLReadPreemption();

        if (tracer)
            tracer->emitVoidCall((void*)threading::allowGLReadPreemption);
    }

    if (tracer) {
        // if (node->target->code)
        tracer->emitJump(node->target);
        tracer->compile();
        tracer.reset();
        if (!node->target->code) {
            startTracing(node->target);
            RELEASE_ASSERT(tracer, "");
        }
    }

    if (backedge)
        ++edgecount;

    if (ENABLE_TRACING && backedge && edgecount == 5 && !tracer && !node->target->code
        && tracers_aborted.count(node->target) == 0) {
        startTracing(node->target);
    }

    if (!ENABLE_TRACING && ENABLE_OSR && backedge && edgecount == OSR_THRESHOLD_INTERPRETER) {
        bool can_osr = !FORCE_INTERPRETER && source_info->scoping->areGlobalsFromModule();
        if (can_osr) {
            static StatCounter ast_osrs("num_ast_osrs");
            ast_osrs.log();

            // TODO: we will immediately want the liveness info again in the jit, we should pass
            // it through.
            std::unique_ptr<LivenessAnalysis> liveness = computeLivenessInfo(source_info->cfg);
            std::unique_ptr<PhiAnalysis> phis
                = computeRequiredPhis(compiled_func->clfunc->param_names, source_info->cfg, liveness.get(), scope_info);

            std::vector<InternedString> dead_symbols;
            for (auto& it : sym_table) {
                if (!liveness->isLiveAtEnd(it.first, current_block)) {
                    dead_symbols.push_back(it.first);
                } else if (phis->isRequiredAfter(it.first, current_block)) {
                    assert(scope_info->getScopeTypeOfName(it.first) != ScopeInfo::VarScopeType::GLOBAL);
                } else {
                }
            }
            for (auto&& dead : dead_symbols)
                sym_table.erase(dead);

            const OSREntryDescriptor* found_entry = nullptr;
            for (auto& p : compiled_func->clfunc->osr_versions) {
                if (p.first->cf != compiled_func)
                    continue;
                if (p.first->backedge != node)
                    continue;

                found_entry = p.first;
            }

            std::map<InternedString, Box*> sorted_symbol_table;

            for (auto& name : phis->definedness.getDefinedNamesAtEnd(current_block)) {
                auto it = sym_table.find(name);
                if (!liveness->isLiveAtEnd(name, current_block))
                    continue;

                if (phis->isPotentiallyUndefinedAfter(name, current_block)) {
                    bool is_defined = it != sym_table.end();
                    // TODO only mangle once
                    sorted_symbol_table[getIsDefinedName(name, source_info->getInternedStrings())] = (Box*)is_defined;
                    if (is_defined)
                        assert(sym_table.getMapped(it->second) != NULL);
                    sorted_symbol_table[name] = is_defined ? sym_table.getMapped(it->second) : NULL;
                } else {
                    ASSERT(it != sym_table.end(), "%s", name.c_str());
                    sorted_symbol_table[it->first] = sym_table.getMapped(it->second);
                }
            }

            // Manually free these here, since we might not return from this scope for a long time.
            liveness.reset(nullptr);
            phis.reset(nullptr);

            // LLVM has a limit on the number of operands a machine instruction can have (~255),
            // in order to not hit the limit with the patchpoints cancel OSR when we have a high number of symbols.
            if (sorted_symbol_table.size() > 225) {
                static StatCounter times_osr_cancel("num_osr_cancel_too_many_syms");
                times_osr_cancel.log();
                next_block = node->target;
                return Value();
            }

            if (generator)
                sorted_symbol_table[source_info->getInternedStrings().get(PASSED_GENERATOR_NAME)] = generator;

            if (passed_closure)
                sorted_symbol_table[source_info->getInternedStrings().get(PASSED_CLOSURE_NAME)] = passed_closure;

            if (created_closure)
                sorted_symbol_table[source_info->getInternedStrings().get(CREATED_CLOSURE_NAME)] = created_closure;

            sorted_symbol_table[source_info->getInternedStrings().get(FRAME_INFO_PTR_NAME)] = (Box*)&frame_info;

            if (found_entry == nullptr) {
                OSREntryDescriptor* entry = OSREntryDescriptor::create(compiled_func, node);

                for (auto& it : sorted_symbol_table) {
                    if (isIsDefinedName(it.first))
                        entry->args[it.first] = BOOL;
                    else if (it.first.s() == PASSED_GENERATOR_NAME)
                        entry->args[it.first] = GENERATOR;
                    else if (it.first.s() == PASSED_CLOSURE_NAME || it.first.s() == CREATED_CLOSURE_NAME)
                        entry->args[it.first] = CLOSURE;
                    else if (it.first.s() == FRAME_INFO_PTR_NAME)
                        entry->args[it.first] = FRAME_INFO;
                    else {
                        assert(it.first.s()[0] != '!');
                        entry->args[it.first] = UNKNOWN;
                    }
                }

                found_entry = entry;
            }

            OSRExit exit(compiled_func, found_entry);

            std::vector<Box*, StlCompatAllocator<Box*>> arg_array;
            for (auto& it : sorted_symbol_table) {
                arg_array.push_back(it.second);
            }

            STAT_TIMER(t0, "us_timer_astinterpreter_jump_osrexit");
            CompiledFunction* partial_func = compilePartialFuncInternal(&exit);
            auto arg_tuple = getTupleFromArgsArray(&arg_array[0], arg_array.size());
            Box* r = partial_func->call(std::get<0>(arg_tuple), std::get<1>(arg_tuple), std::get<2>(arg_tuple),
                                        std::get<3>(arg_tuple));

            // This is one of the few times that we are allowed to have an invalid value in a Box* Value.
            // Check for it, and return as an int so that we don't trigger a potential assert when
            // creating the Value.
            if (compiled_func->getReturnType() != VOID)
                assert(r);
            return (intptr_t)r;
        }
    }

    next_block = node->target;
    return Value();
}

Value ASTInterpreter::visit_invoke(AST_Invoke* node) {
    Value v;
    try {
        v = visit_stmt(node->stmt);
        next_block = node->normal_dest;
    } catch (ExcInfo e) {
        abortTracing();

        auto source = getCF()->clfunc->source.get();
        exceptionCaughtInInterpreter(LineInfo(node->lineno, node->col_offset, source->fn, source->getName()), &e);

        next_block = node->exc_dest;
        last_exception = e;
    }

    return v;
}

Value ASTInterpreter::visit_clsAttribute(AST_ClsAttribute* node) {
    abortTracing();
    return getclsattr(visit_expr(node->value).o, node->attr.getBox());
}

Value ASTInterpreter::visit_augBinOp(AST_AugBinOp* node) {
    assert(node->op_type != AST_TYPE::Is && node->op_type != AST_TYPE::IsNot && "not tested yet");

    Value left = visit_expr(node->left);
    Value right = visit_expr(node->right);
    return doBinOp(left.o, right.o, node->op_type, BinExpType::AugBinOp);
}

Value ASTInterpreter::visit_langPrimitive(AST_LangPrimitive* node) {
    Value v;
    if (node->opcode == AST_LangPrimitive::GET_ITER) {
        assert(node->args.size() == 1);

        Value val = visit_expr(node->args[0]);
        if (tracer)
            tracer->emitGetPystonIter();

        v = getPystonIter(val.o);
    } else if (node->opcode == AST_LangPrimitive::IMPORT_FROM) {
        abortTracing();
        assert(node->args.size() == 2);
        assert(node->args[0]->type == AST_TYPE::Name);
        assert(node->args[1]->type == AST_TYPE::Str);

        Value module = visit_expr(node->args[0]);
        auto ast_str = ast_cast<AST_Str>(node->args[1]);
        assert(ast_str->str_type == AST_Str::STR);
        const std::string& name = ast_str->str_data;
        assert(name.size());
        // TODO: shouldn't have to rebox here
        v = importFrom(module.o, boxString(name));
    } else if (node->opcode == AST_LangPrimitive::IMPORT_NAME) {
        abortTracing();
        assert(node->args.size() == 3);
        assert(node->args[0]->type == AST_TYPE::Num);
        assert(static_cast<AST_Num*>(node->args[0])->num_type == AST_Num::INT);
        assert(node->args[2]->type == AST_TYPE::Str);

        int level = static_cast<AST_Num*>(node->args[0])->n_int;
        Value froms = visit_expr(node->args[1]);
        auto ast_str = ast_cast<AST_Str>(node->args[2]);
        assert(ast_str->str_type == AST_Str::STR);
        const std::string& module_name = ast_str->str_data;
        v = import(level, froms.o, module_name);
    } else if (node->opcode == AST_LangPrimitive::IMPORT_STAR) {
        abortTracing();
        assert(node->args.size() == 1);
        assert(node->args[0]->type == AST_TYPE::Name);

        RELEASE_ASSERT(source_info->ast->type == AST_TYPE::Module || source_info->ast->type == AST_TYPE::Suite,
                       "import * not supported in functions");

        Value module = visit_expr(node->args[0]);

        v = importStar(module.o, globals);
    } else if (node->opcode == AST_LangPrimitive::NONE) {
        abortTracing();
        v = None;
    } else if (node->opcode == AST_LangPrimitive::LANDINGPAD) {
        abortTracing();
        assert(last_exception.type);
        Box* type = last_exception.type;
        Box* value = last_exception.value ? last_exception.value : None;
        Box* traceback = last_exception.traceback ? last_exception.traceback : None;
        v = BoxedTuple::create({ type, value, traceback });
        last_exception = ExcInfo(NULL, NULL, NULL);
    } else if (node->opcode == AST_LangPrimitive::CHECK_EXC_MATCH) {
        abortTracing();
        assert(node->args.size() == 2);
        Value obj = visit_expr(node->args[0]);
        Value cls = visit_expr(node->args[1]);

        v = boxBool(exceptionMatches(obj.o, cls.o));

    } else if (node->opcode == AST_LangPrimitive::LOCALS) {
        abortTracing();
        assert(frame_info.boxedLocals != NULL);
        v = frame_info.boxedLocals;
    } else if (node->opcode == AST_LangPrimitive::NONZERO) {
        assert(node->args.size() == 1);
        Value obj = visit_expr(node->args[0]);
        if (tracer)
            tracer->emitNonzero();
        v = boxBool(nonzero(obj.o));
    } else if (node->opcode == AST_LangPrimitive::SET_EXC_INFO) {
        abortTracing();
        assert(node->args.size() == 3);

        Value type = visit_expr(node->args[0]);
        assert(type.o);
        Value value = visit_expr(node->args[1]);
        assert(value.o);
        Value traceback = visit_expr(node->args[2]);
        assert(traceback.o);

        getFrameInfo()->exc = ExcInfo(type.o, value.o, traceback.o);
        v = None;
    } else if (node->opcode == AST_LangPrimitive::UNCACHE_EXC_INFO) {
        assert(node->args.empty());
        if (tracer)
            tracer->emitUncacheExcInfo();

        getFrameInfo()->exc = ExcInfo(NULL, NULL, NULL);
        v = None;
    } else if (node->opcode == AST_LangPrimitive::HASNEXT) {
        assert(node->args.size() == 1);
        Value obj = visit_expr(node->args[0]);

        if (tracer)
            tracer->emitHasnext();

        v = boxBool(hasnext(obj.o));
    } else
        RELEASE_ASSERT(0, "unknown opcode %d", node->opcode);
    return v;
}

Value ASTInterpreter::visit_yield(AST_Yield* node) {
    Value value = node->value ? visit_expr(node->value) : None;
    assert(generator && generator->cls == generator_cls);

    if (tracer)
        tracer->emitYield(node->value != 0);

    return yield(generator, value.o);
}

Value ASTInterpreter::visit_stmt(AST_stmt* node) {
#if ENABLE_SAMPLING_PROFILER
    threading::allowGLReadPreemption();
#endif

    if (0 && tracer) {
        printf("%20s % 2d ", source_info->getName().c_str(), current_block->idx);
        print_ast(node);
        printf("\n");
    }

    switch (node->type) {
        case AST_TYPE::Assert:
            return visit_assert((AST_Assert*)node);
        case AST_TYPE::Assign:
            return visit_assign((AST_Assign*)node);
        case AST_TYPE::Delete:
            return visit_delete((AST_Delete*)node);
        case AST_TYPE::Exec:
            return visit_exec((AST_Exec*)node);
        case AST_TYPE::Expr:
            // docstrings are str constant expression statements.
            // ignore those while interpreting.
            if ((((AST_Expr*)node)->value)->type != AST_TYPE::Str)
                return visit_expr((AST_Expr*)node);
            break;
        case AST_TYPE::Pass:
            return Value(); // nothing todo
        case AST_TYPE::Print:
            return visit_print((AST_Print*)node);
        case AST_TYPE::Raise:
            return visit_raise((AST_Raise*)node);
        case AST_TYPE::Return:
            return visit_return((AST_Return*)node);
        case AST_TYPE::Global:
            return visit_global((AST_Global*)node);

        // pseudo
        case AST_TYPE::Branch:
            return visit_branch((AST_Branch*)node);
        case AST_TYPE::Jump:
            return visit_jump((AST_Jump*)node);
        case AST_TYPE::Invoke:
            return visit_invoke((AST_Invoke*)node);
        default:
            RELEASE_ASSERT(0, "not implemented");
    };
    return Value();
}

Value ASTInterpreter::visit_return(AST_Return* node) {
    Value s(node->value ? visit_expr(node->value) : None);

    if (tracer && !node->value)
        tracer->emitPush((uint64_t)None);

    if (tracer) {
        tracer->emitReturn();
        tracer->compile();
        tracer.reset();
    }

    next_block = 0;
    return s;
}

Box* ASTInterpreter::createFunction(AST* node, AST_arguments* args, const std::vector<AST_stmt*>& body) {
    abortTracing();
    CLFunction* cl = wrapFunction(node, args, body, source_info);

    std::vector<Box*, StlCompatAllocator<Box*>> defaults;
    for (AST_expr* d : args->defaults)
        defaults.push_back(visit_expr(d).o);
    defaults.push_back(0);

    // FIXME: Using initializer_list is pretty annoying since you're not supposed to create them:
    union {
        struct {
            Box** ptr;
            size_t s;
        } d;
        std::initializer_list<Box*> il = {};
    } u;

    u.d.ptr = &defaults[0];
    u.d.s = defaults.size() - 1;

    bool takes_closure;
    // Optimization: when compiling a module, it's nice to not have to run analyses into the
    // entire module's source code.
    // If we call getScopeInfoForNode, that will trigger an analysis of that function tree,
    // but we're only using it here to figure out if that function takes a closure.
    // Top level functions never take a closure, so we can skip the analysis.
    if (source_info->ast->type == AST_TYPE::Module)
        takes_closure = false;
    else {
        takes_closure = source_info->scoping->getScopeInfoForNode(node)->takesClosure();
    }

    BoxedClosure* closure = 0;
    if (takes_closure) {
        if (scope_info->createsClosure()) {
            closure = created_closure;
        } else {
            assert(scope_info->passesThroughClosure());
            closure = passed_closure;
        }
        assert(closure);
    }

    Box* passed_globals = NULL;
    if (!getCF()->clfunc->source->scoping->areGlobalsFromModule())
        passed_globals = globals;
    return boxCLFunction(cl, closure, passed_globals, u.il);
}

Value ASTInterpreter::visit_makeFunction(AST_MakeFunction* mkfn) {
    abortTracing();
    AST_FunctionDef* node = mkfn->function_def;
    AST_arguments* args = node->args;

    std::vector<Box*, StlCompatAllocator<Box*>> decorators;
    for (AST_expr* d : node->decorator_list)
        decorators.push_back(visit_expr(d).o);

    Box* func = createFunction(node, args, node->body);

    for (int i = decorators.size() - 1; i >= 0; i--)
        func = runtimeCall(decorators[i], ArgPassSpec(1), func, 0, 0, 0, 0);

    return Value(func);
}

Value ASTInterpreter::visit_makeClass(AST_MakeClass* mkclass) {
    abortTracing();
    AST_ClassDef* node = mkclass->class_def;
    ScopeInfo* scope_info = source_info->scoping->getScopeInfoForNode(node);
    assert(scope_info);

    BoxedTuple* basesTuple = BoxedTuple::create(node->bases.size());
    int base_idx = 0;
    for (AST_expr* b : node->bases) {
        basesTuple->elts[base_idx++] = visit_expr(b).o;
    }

    std::vector<Box*, StlCompatAllocator<Box*>> decorators;
    for (AST_expr* d : node->decorator_list)
        decorators.push_back(visit_expr(d).o);

    BoxedClosure* closure = NULL;
    if (scope_info->takesClosure()) {
        if (this->scope_info->passesThroughClosure())
            closure = passed_closure;
        else
            closure = created_closure;
        assert(closure);
    }
    CLFunction* cl = wrapFunction(node, nullptr, node->body, source_info);

    Box* passed_globals = NULL;
    if (!getCF()->clfunc->source->scoping->areGlobalsFromModule())
        passed_globals = globals;
    Box* attrDict = runtimeCall(boxCLFunction(cl, closure, passed_globals, {}), ArgPassSpec(0), 0, 0, 0, 0, 0);

    Box* classobj = createUserClass(node->name.getBox(), basesTuple, attrDict);

    for (int i = decorators.size() - 1; i >= 0; i--)
        classobj = runtimeCall(decorators[i], ArgPassSpec(1), classobj, 0, 0, 0, 0);

    return Value(classobj);
}

Value ASTInterpreter::visit_raise(AST_Raise* node) {
    abortTracing();
    if (node->arg0 == NULL) {
        assert(!node->arg1);
        assert(!node->arg2);
        raise0();
    }

    raise3(node->arg0 ? visit_expr(node->arg0).o : None, node->arg1 ? visit_expr(node->arg1).o : None,
           node->arg2 ? visit_expr(node->arg2).o : None);
    return Value();
}

Value ASTInterpreter::visit_assert(AST_Assert* node) {
    abortTracing();
#ifndef NDEBUG
    // Currently we only generate "assert 0" statements
    Value v = visit_expr(node->test);
    assert(v.o->cls == int_cls && static_cast<BoxedInt*>(v.o)->n == 0);
#endif

    static BoxedString* AssertionError_str = static_cast<BoxedString*>(PyString_InternFromString("AssertionError"));
    Box* assertion_type = getGlobal(globals, AssertionError_str);
    assertFail(assertion_type, node->msg ? visit_expr(node->msg).o : 0);

    return Value();
}

Value ASTInterpreter::visit_global(AST_Global* node) {
    abortTracing();
    for (auto name : node->names)
        sym_table.erase(name);
    return Value();
}

Value ASTInterpreter::visit_delete(AST_Delete* node) {
    abortTracing();
    for (AST_expr* target_ : node->targets) {
        switch (target_->type) {
            case AST_TYPE::Subscript: {
                AST_Subscript* sub = (AST_Subscript*)target_;
                Value value = visit_expr(sub->value);
                Value slice = visit_expr(sub->slice);
                delitem(value.o, slice.o);
                break;
            }
            case AST_TYPE::Attribute: {
                AST_Attribute* attr = (AST_Attribute*)target_;
                pyston::delattr(visit_expr(attr->value).o, attr->attr.getBox());
                break;
            }
            case AST_TYPE::Name: {
                AST_Name* target = (AST_Name*)target_;

                ScopeInfo::VarScopeType vst = scope_info->getScopeTypeOfName(target->id);
                if (vst == ScopeInfo::VarScopeType::GLOBAL) {
                    delGlobal(globals, target->id.getBox());
                    continue;
                } else if (vst == ScopeInfo::VarScopeType::NAME) {
                    assert(frame_info.boxedLocals != NULL);
                    if (frame_info.boxedLocals->cls == dict_cls) {
                        auto& d = static_cast<BoxedDict*>(frame_info.boxedLocals)->d;
                        auto it = d.find(target->id.getBox());
                        if (it == d.end()) {
                            assertNameDefined(0, target->id.c_str(), NameError, false /* local_var_msg */);
                        }
                        d.erase(it);
                    } else if (frame_info.boxedLocals->cls == attrwrapper_cls) {
                        attrwrapperDel(frame_info.boxedLocals, target->id);
                    } else {
                        RELEASE_ASSERT(0, "%s", frame_info.boxedLocals->cls->tp_name);
                    }
                } else {
                    assert(vst == ScopeInfo::VarScopeType::FAST);

                    if (sym_table.count(target->id) == 0) {
                        assertNameDefined(0, target->id.c_str(), NameError, true /* local_var_msg */);
                        return Value();
                    }

                    sym_table.erase(target->id);
                }
                break;
            }
            default:
                ASSERT(0, "Unsupported del target: %d", target_->type);
                abort();
        }
    }
    return Value();
}

Value ASTInterpreter::visit_assign(AST_Assign* node) {
    assert(node->targets.size() == 1 && "cfg should have lowered it to a single target");

    Value v = visit_expr(node->value);
    for (AST_expr* e : node->targets)
        doStore(e, v);
    return Value();
}

Value ASTInterpreter::visit_print(AST_Print* node) {
    abortTracing();

    static BoxedString* write_str = static_cast<BoxedString*>(PyString_InternFromString("write"));
    static BoxedString* newline_str = static_cast<BoxedString*>(PyString_InternFromString("\n"));
    static BoxedString* space_str = static_cast<BoxedString*>(PyString_InternFromString(" "));

    STAT_TIMER(t0, "us_timer_visit_print");

    Box* dest = node->dest ? visit_expr(node->dest).o : getSysStdout();
    int nvals = node->values.size();
    assert(nvals <= 1 && "cfg should have lowered it to 0 or 1 values");
    for (int i = 0; i < nvals; i++) {
        Box* var = visit_expr(node->values[i]).o;

        // begin code for handling of softspace
        bool new_softspace = (i < nvals - 1) || (!node->nl);
        if (softspace(dest, new_softspace)) {
            callattrInternal(dest, write_str, CLASS_OR_INST, 0, ArgPassSpec(1), space_str, 0, 0, 0, 0);
        }

        Box* str_or_unicode_var = (var->cls == unicode_cls) ? var : str(var);
        callattrInternal(dest, write_str, CLASS_OR_INST, 0, ArgPassSpec(1), str_or_unicode_var, 0, 0, 0, 0);
    }

    if (node->nl) {
        callattrInternal(dest, write_str, CLASS_OR_INST, 0, ArgPassSpec(1), newline_str, 0, 0, 0, 0);
        if (nvals == 0) {
            softspace(dest, false);
        }
    }
    return Value();
}

Value ASTInterpreter::visit_exec(AST_Exec* node) {
    abortTracing();
    // TODO implement the locals and globals arguments
    Box* code = visit_expr(node->body).o;
    Box* globals = node->globals == NULL ? NULL : visit_expr(node->globals).o;
    Box* locals = node->locals == NULL ? NULL : visit_expr(node->locals).o;

    exec(code, globals, locals, this->source_info->future_flags);

    return Value();
}

Value ASTInterpreter::visit_compare(AST_Compare* node) {
    RELEASE_ASSERT(node->comparators.size() == 1, "not implemented");
    Box* left = visit_expr(node->left).o;
    Box* right = visit_expr(node->comparators[0]).o;
    return doBinOp(left, right, node->ops[0], BinExpType::Compare);
}

Value ASTInterpreter::visit_expr(AST_expr* node) {
    switch (node->type) {
        case AST_TYPE::Attribute:
            return visit_attribute((AST_Attribute*)node);
        case AST_TYPE::BinOp:
            return visit_binop((AST_BinOp*)node);
        case AST_TYPE::Call:
            return visit_call((AST_Call*)node);
        case AST_TYPE::Compare:
            return visit_compare((AST_Compare*)node);
        case AST_TYPE::Dict:
            return visit_dict((AST_Dict*)node);
        case AST_TYPE::ExtSlice:
            return visit_extslice((AST_ExtSlice*)node);
        case AST_TYPE::Index:
            return visit_index((AST_Index*)node);
        case AST_TYPE::Lambda:
            return visit_lambda((AST_Lambda*)node);
        case AST_TYPE::List:
            return visit_list((AST_List*)node);
        case AST_TYPE::Name:
            return visit_name((AST_Name*)node);
        case AST_TYPE::Num:
            return visit_num((AST_Num*)node);
        case AST_TYPE::Repr:
            return visit_repr((AST_Repr*)node);
        case AST_TYPE::Set:
            return visit_set((AST_Set*)node);
        case AST_TYPE::Slice:
            return visit_slice((AST_Slice*)node);
        case AST_TYPE::Str:
            return visit_str((AST_Str*)node);
        case AST_TYPE::Subscript:
            return visit_subscript((AST_Subscript*)node);
        case AST_TYPE::Tuple:
            return visit_tuple((AST_Tuple*)node);
        case AST_TYPE::UnaryOp:
            return visit_unaryop((AST_UnaryOp*)node);
        case AST_TYPE::Yield:
            return visit_yield((AST_Yield*)node);

        // pseudo
        case AST_TYPE::AugBinOp:
            return visit_augBinOp((AST_AugBinOp*)node);
        case AST_TYPE::ClsAttribute:
            return visit_clsAttribute((AST_ClsAttribute*)node);
        case AST_TYPE::LangPrimitive:
            return visit_langPrimitive((AST_LangPrimitive*)node);
        case AST_TYPE::MakeClass:
            return visit_makeClass((AST_MakeClass*)node);
        case AST_TYPE::MakeFunction:
            return visit_makeFunction((AST_MakeFunction*)node);
        default:
            RELEASE_ASSERT(0, "");
    };
    return Value();
}


Value ASTInterpreter::visit_call(AST_Call* node) {
    Value v;
    Value func;

    InternedString attr;

    bool is_callattr = false;
    bool callattr_clsonly = false;
    if (node->func->type == AST_TYPE::Attribute) {
        is_callattr = true;
        callattr_clsonly = false;
        AST_Attribute* attr_ast = ast_cast<AST_Attribute>(node->func);
        func = visit_expr(attr_ast->value);
        attr = attr_ast->attr;
    } else if (node->func->type == AST_TYPE::ClsAttribute) {
        is_callattr = true;
        callattr_clsonly = true;
        AST_ClsAttribute* attr_ast = ast_cast<AST_ClsAttribute>(node->func);
        func = visit_expr(attr_ast->value);
        attr = attr_ast->attr;
    } else {
        func = visit_expr(node->func);
    }

    std::vector<Box*, StlCompatAllocator<Box*>> args;
    for (AST_expr* e : node->args)
        args.push_back(visit_expr(e).o);

    std::vector<BoxedString*>* keyword_names = NULL;
    if (node->keywords.size())
        keyword_names = getKeywordNameStorage(node);

    for (AST_keyword* k : node->keywords)
        args.push_back(visit_expr(k->value).o);

    if (node->starargs)
        args.push_back(visit_expr(node->starargs).o);

    if (node->kwargs)
        args.push_back(visit_expr(node->kwargs).o);

    ArgPassSpec argspec(node->args.size(), node->keywords.size(), node->starargs, node->kwargs);

    if (is_callattr) {
        CallattrFlags flags{.cls_only = callattr_clsonly, .null_on_nonexistent = false };

        if (tracer)
            tracer->emitCallattr(attr.getBox(), flags, argspec, keyword_names);

        return callattr(func.o, attr.getBox(),
                        CallattrFlags({.cls_only = callattr_clsonly, .null_on_nonexistent = false }), argspec,
                        args.size() > 0 ? args[0] : 0, args.size() > 1 ? args[1] : 0, args.size() > 2 ? args[2] : 0,
                        args.size() > 3 ? &args[3] : 0, keyword_names);
    } else {
        if (tracer)
            tracer->emitRuntimeCall(argspec, keyword_names);

        return runtimeCall(func.o, argspec, args.size() > 0 ? args[0] : 0, args.size() > 1 ? args[1] : 0,
                           args.size() > 2 ? args[2] : 0, args.size() > 3 ? &args[3] : 0, keyword_names);
    }
}


Value ASTInterpreter::visit_expr(AST_Expr* node) {
    Value rtn = visit_expr(node->value);
    if (tracer)
        tracer->emitPop();
    return rtn;
}

Value ASTInterpreter::visit_num(AST_Num* node) {
    if (node->num_type == AST_Num::INT) {
        if (tracer)
            tracer->emitInt(node->n_int);
        return boxInt(node->n_int);
    } else if (node->num_type == AST_Num::FLOAT) {
        if (tracer)
            tracer->emitFloat(node->n_float);
        return boxFloat(node->n_float);
    } else if (node->num_type == AST_Num::LONG) {
        if (tracer)
            tracer->emitLong(node->n_long);
        return createLong(node->n_long);
    } else if (node->num_type == AST_Num::COMPLEX) {
        abortTracing();
        return boxComplex(0.0, node->n_float);
    }
    RELEASE_ASSERT(0, "not implemented");
    return Value();
}

Value ASTInterpreter::visit_index(AST_Index* node) {
    return visit_expr(node->value);
}

Value ASTInterpreter::visit_repr(AST_Repr* node) {
    abortTracing();
    return repr(visit_expr(node->value).o);
}

Value ASTInterpreter::visit_lambda(AST_Lambda* node) {
    abortTracing();
    AST_Return* expr = new AST_Return();
    expr->value = node->body;

    std::vector<AST_stmt*> body = { expr };
    return createFunction(node, node->args, body);
}

Value ASTInterpreter::visit_dict(AST_Dict* node) {
    RELEASE_ASSERT(node->keys.size() == node->values.size(), "not implemented");
    if (tracer) {
        if (node->keys.size())
            abortTracing();
        else
            tracer->emitCreateDict();
    }

    BoxedDict* dict = new BoxedDict();
    for (size_t i = 0; i < node->keys.size(); ++i) {
        Box* v = visit_expr(node->values[i]).o;
        Box* k = visit_expr(node->keys[i]).o;
        dict->d[k] = v;
    }

    return dict;
}

Value ASTInterpreter::visit_set(AST_Set* node) {
    abortTracing();
    BoxedSet::Set set;
    for (AST_expr* e : node->elts)
        set.insert(visit_expr(e).o);

    return new BoxedSet(std::move(set));
}

Value ASTInterpreter::visit_str(AST_Str* node) {
    if (node->str_type == AST_Str::STR) {
        if (tracer)
            tracer->emitPush((uint64_t)source_info->parent_module->getStringConstant(node->str_data));
        return source_info->parent_module->getStringConstant(node->str_data);
    } else if (node->str_type == AST_Str::UNICODE) {
        if (tracer)
            tracer->emitUnicodeStr(node->str_data);
        return decodeUTF8StringPtr(node->str_data);
    } else {
        RELEASE_ASSERT(0, "%d", node->str_type);
    }
}

Value ASTInterpreter::visit_name(AST_Name* node) {
    if (node->lookup_type == ScopeInfo::VarScopeType::UNKNOWN) {
        node->lookup_type = scope_info->getScopeTypeOfName(node->id);
    }

    switch (node->lookup_type) {
        case ScopeInfo::VarScopeType::GLOBAL:
            if (tracer)
                tracer->emitGetGlobal(globals, node->id.getBox());

            return getGlobal(globals, node->id.getBox());
        case ScopeInfo::VarScopeType::DEREF: {

            if (tracer)
                tracer->emitDeref(node->id);

            DerefInfo deref_info = scope_info->getDerefInfo(node->id);
            assert(passed_closure);
            BoxedClosure* closure = passed_closure;
            for (int i = 0; i < deref_info.num_parents_from_passed_closure; i++) {
                closure = closure->parent;
            }
            Box* val = closure->elts[deref_info.offset];
            if (val == NULL) {
                raiseExcHelper(NameError, "free variable '%s' referenced before assignment in enclosing scope",
                               node->id.c_str());
            }
            return val;
        }
        case ScopeInfo::VarScopeType::FAST:
        case ScopeInfo::VarScopeType::CLOSURE: {
            if (tracer) {
                bool dead = false;
                if (node->lookup_type == ScopeInfo::VarScopeType::FAST)
                    dead = !getLivness()->isLiveAtEnd(node->id, current_block);

                if (dead)
                    tracer->emitGetDeadLocal(node->id);
                else
                    tracer->emitGetLocal(node->id);
            }

            SymMap::iterator it = sym_table.find(node->id);
            if (it != sym_table.end())
                return sym_table.getMapped(it->second);

            assertNameDefined(0, node->id.c_str(), UnboundLocalError, true);
            return Value();
        }
        case ScopeInfo::VarScopeType::NAME: {
            if (tracer)
                tracer->emitBoxedLocalsGet(node->id.getBox());
            return boxedLocalsGet(frame_info.boxedLocals, node->id.getBox(), globals);
        }
        default:
            abort();
    }
}


Value ASTInterpreter::visit_subscript(AST_Subscript* node) {
    Value value = visit_expr(node->value);
    Value slice = visit_expr(node->slice);

    if (tracer)
        tracer->emitGetItem();

    return getitem(value.o, slice.o);
}

Value ASTInterpreter::visit_list(AST_List* node) {
    BoxedList* list = new BoxedList;
    list->ensure(node->elts.size());
    for (AST_expr* e : node->elts)
        listAppendInternal(list, visit_expr(e).o);

    if (tracer)
        tracer->emitCreateList(node->elts.size());

    return list;
}

Value ASTInterpreter::visit_tuple(AST_Tuple* node) {
    BoxedTuple* rtn = BoxedTuple::create(node->elts.size());
    int rtn_idx = 0;
    for (AST_expr* e : node->elts)
        rtn->elts[rtn_idx++] = visit_expr(e).o;

    if (tracer)
        tracer->emitCreateTuple(node->elts.size());

    return rtn;
}

Value ASTInterpreter::visit_attribute(AST_Attribute* node) {
    Value v = visit_expr(node->value);
    if (tracer)
        tracer->emitGetAttr(node->attr.getBox());
    return pyston::getattr(v.o, node->attr.getBox());
}
}

const void* interpreter_instr_addr = (void*)&ASTInterpreter::executeInner;

Box* astInterpretFunction(CompiledFunction* cf, int nargs, Box* closure, Box* generator, Box* globals, Box* arg1,
                          Box* arg2, Box* arg3, Box** args) {
    assert((!globals) == cf->clfunc->source->scoping->areGlobalsFromModule());
    bool can_reopt = ENABLE_REOPT && !FORCE_INTERPRETER && (globals == NULL);
    if (unlikely(can_reopt && cf->times_called > REOPT_THRESHOLD_INTERPRETER)) {
        assert(!globals);
        CompiledFunction* optimized = reoptCompiledFuncInternal(cf);
        if (closure && generator)
            return optimized->closure_generator_call((BoxedClosure*)closure, (BoxedGenerator*)generator, arg1, arg2,
                                                     arg3, args);
        else if (closure)
            return optimized->closure_call((BoxedClosure*)closure, arg1, arg2, arg3, args);
        else if (generator)
            return optimized->generator_call((BoxedGenerator*)generator, arg1, arg2, arg3, args);
        return optimized->call(arg1, arg2, arg3, args);
    }

    ++cf->times_called;
    ASTInterpreter* interpreter = new ASTInterpreter(cf);

    ScopeInfo* scope_info = cf->clfunc->source->getScopeInfo();
    SourceInfo* source_info = cf->clfunc->source.get();
    if (unlikely(scope_info->usesNameLookup())) {
        interpreter->setBoxedLocals(new BoxedDict());
    }

    assert((!globals) == cf->clfunc->source->scoping->areGlobalsFromModule());
    if (globals) {
        interpreter->setGlobals(globals);
    } else {
        interpreter->setGlobals(source_info->parent_module);
    }

    interpreter->initArguments(nargs, (BoxedClosure*)closure, (BoxedGenerator*)generator, arg1, arg2, arg3, args);
    Value v = ASTInterpreter::execute(*interpreter);

    return v.o ? v.o : None;
}

Box* astInterpretFunctionEval(CompiledFunction* cf, Box* globals, Box* boxedLocals) {
    ++cf->times_called;

    ASTInterpreter* interpreter = new ASTInterpreter(cf);
    interpreter->initArguments(0, NULL, NULL, NULL, NULL, NULL, NULL);
    interpreter->setBoxedLocals(boxedLocals);

    ScopeInfo* scope_info = cf->clfunc->source->getScopeInfo();
    SourceInfo* source_info = cf->clfunc->source.get();

    assert(!cf->clfunc->source->scoping->areGlobalsFromModule());
    assert(globals);
    interpreter->setGlobals(globals);

    Value v = ASTInterpreter::execute(*interpreter);

    return v.o ? v.o : None;
}

Box* astInterpretFrom(CompiledFunction* cf, AST_expr* after_expr, AST_stmt* enclosing_stmt, Box* expr_val,
                      FrameStackState frame_state) {
    assert(cf);
    assert(enclosing_stmt);
    assert(frame_state.locals);
    assert(after_expr);
    assert(expr_val);

    ASTInterpreter* interpreter = new ASTInterpreter(cf);

    ScopeInfo* scope_info = cf->clfunc->source->getScopeInfo();
    SourceInfo* source_info = cf->clfunc->source.get();
    assert(cf->clfunc->source->scoping->areGlobalsFromModule());
    interpreter->setGlobals(source_info->parent_module);

    for (const auto& p : frame_state.locals->d) {
        assert(p.first->cls == str_cls);
        auto name = static_cast<BoxedString*>(p.first)->s();
        if (name == PASSED_GENERATOR_NAME) {
            interpreter->setGenerator(p.second);
        } else if (name == PASSED_CLOSURE_NAME) {
            interpreter->setPassedClosure(p.second);
        } else if (name == CREATED_CLOSURE_NAME) {
            interpreter->setCreatedClosure(p.second);
        } else {
            InternedString interned = cf->clfunc->source->getInternedStrings().get(name);
            interpreter->addSymbol(interned, p.second, false);
        }
    }

    interpreter->setFrameInfo(frame_state.frame_info);

    CFGBlock* start_block = NULL;
    AST_stmt* starting_statement = NULL;
    while (true) {
        if (enclosing_stmt->type == AST_TYPE::Assign) {
            auto asgn = ast_cast<AST_Assign>(enclosing_stmt);
            assert(asgn->value == after_expr);
            assert(asgn->targets.size() == 1);
            assert(asgn->targets[0]->type == AST_TYPE::Name);
            auto name = ast_cast<AST_Name>(asgn->targets[0]);
            assert(name->id.s()[0] == '#');
            interpreter->addSymbol(name->id, expr_val, true);
            break;
        } else if (enclosing_stmt->type == AST_TYPE::Expr) {
            auto expr = ast_cast<AST_Expr>(enclosing_stmt);
            assert(expr->value == after_expr);
            break;
        } else if (enclosing_stmt->type == AST_TYPE::Invoke) {
            auto invoke = ast_cast<AST_Invoke>(enclosing_stmt);
            start_block = invoke->normal_dest;
            starting_statement = start_block->body[0];
            enclosing_stmt = invoke->stmt;
        } else {
            RELEASE_ASSERT(0, "should not be able to reach here with anything other than an Assign (got %d)",
                           enclosing_stmt->type);
        }
    }

    if (start_block == NULL) {
        // TODO innefficient
        for (auto block : cf->clfunc->source->cfg->blocks) {
            int n = block->body.size();
            for (int i = 0; i < n; i++) {
                if (block->body[i] == enclosing_stmt) {
                    ASSERT(i + 1 < n, "how could we deopt from a non-invoke terminator?");
                    start_block = block;
                    starting_statement = block->body[i + 1];
                    break;
                }
            }

            if (start_block)
                break;
        }

        ASSERT(start_block, "was unable to find the starting block??");
        assert(starting_statement);
    }

    Value v = ASTInterpreter::execute(*interpreter, start_block, starting_statement);

    return v.o ? v.o : None;
}

AST_stmt* getCurrentStatementForInterpretedFrame(void* frame_ptr) {
    ASTInterpreter* interpreter = s_interpreterMap[frame_ptr];
    assert(interpreter);
    return interpreter->getCurrentStatement();
}

Box* getGlobalsForInterpretedFrame(void* frame_ptr) {
    ASTInterpreter* interpreter = s_interpreterMap[frame_ptr];
    assert(interpreter);
    return interpreter->getGlobals();
}

CompiledFunction* getCFForInterpretedFrame(void* frame_ptr) {
    ASTInterpreter* interpreter = s_interpreterMap[frame_ptr];
    assert(interpreter);
    return interpreter->getCF();
}

FrameInfo* getFrameInfoForInterpretedFrame(void* frame_ptr) {
    ASTInterpreter* interpreter = s_interpreterMap[frame_ptr];
    assert(interpreter);
    return interpreter->getFrameInfo();
}

BoxedDict* localsForInterpretedFrame(void* frame_ptr, bool only_user_visible) {
    ASTInterpreter* interpreter = s_interpreterMap[frame_ptr];
    assert(interpreter);
    BoxedDict* rtn = new BoxedDict();
    for (auto& l : interpreter->getSymbolTable()) {
        if (only_user_visible && (l.first.s()[0] == '!' || l.first.s()[0] == '#'))
            continue;

        rtn->d[l.first.getBox()] = interpreter->getSymbolTable().getMapped(l.second);
    }

    return rtn;
}

BoxedClosure* passedClosureForInterpretedFrame(void* frame_ptr) {
    ASTInterpreter* interpreter = s_interpreterMap[frame_ptr];
    assert(interpreter);
    return interpreter->getPassedClosure();
}

void setupInterpreter() {
    astinterpreter_cls = BoxedHeapClass::create(type_cls, object_cls, ASTInterpreter::gcHandler, 0, 0,
                                                sizeof(ASTInterpreter), false, "astinterpreter");
    astinterpreter_cls->simple_destructor = ASTInterpreter::simpleDestructor;
    astinterpreter_cls->freeze();
}
}
