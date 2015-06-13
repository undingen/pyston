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
#define ENABLE_TRACING_IC 0

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
    RegisterHelper(ASTInterpreter* interpreter, void* frame_addr);
    ~RegisterHelper();

    static void deregister(void* frame_addr) {
        assert(s_interpreterMap.count(frame_addr));
        s_interpreterMap.erase(frame_addr);
    }
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

llvm::DenseMap<CFGBlock*, class Tracer*> tracers;

llvm::DenseSet<CFGBlock*> tracers_aborted;

class ASTInterpreter : public Box {
public:
    typedef ContiguousMap<InternedString, Box*> SymMap;

    ASTInterpreter(CompiledFunction* compiled_function);

    void initArguments(int nargs, BoxedClosure* closure, BoxedGenerator* generator, Box* arg1, Box* arg2, Box* arg3,
                       Box** args);

    // This must not be inlined, because we rely on being able to detect when we're inside of it (by checking whether
    // %rip is inside its instruction range) during a stack-trace in order to produce tracebacks inside interpreted
    // code.
    __attribute__((__no_inline__)) static Value
        execute(ASTInterpreter& interpreter, CFGBlock* start_block = NULL, AST_stmt* start_at = NULL);

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
    friend class Tracer;

    class Tracer* tracer = 0;
    bool istracing = 0;

    void abortTracing();

    static Box* tracerHelperGetLocal(ASTInterpreter* i, InternedString id) {
        SymMap::iterator it = i->sym_table.find(id);
        if (it != i->sym_table.end()) {
            return i->sym_table.getMapped(it->second);
        }

        assertNameDefined(0, id.c_str(), UnboundLocalError, true);
        return 0;
    }

    static void tracerHelperSetLocal(ASTInterpreter* i, InternedString id, Box* v) {
        // printf("SET: %s = %s\n", id.c_str(), str(v)->data());
        i->sym_table[id] = v;
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

class Tracer {
public:
    CFGBlock* start;
    bool failed, finished;
    ASTInterpreter* interp;

    unsigned char* buf = 0;
    unsigned long entry, epilog;
    std::unique_ptr<assembler::Assembler> a;

    int stack;
    bool abort_on_backage;

    llvm::DenseMap<CFGBlock*, void*> cfg_code;

    const int code_size = 4096 * 2;
    const int epilog_size = 2; // 3+1+1;

    // assembler::Assembler* a;

    bool have_block_traced(CFGBlock* block) { return cfg_code.count(block); }

    void setCfgBlockEntry(CFGBlock* block) {
        assert(!have_block_traced(block));
        printf("Setting entry: %p %p %d %s\n", block, (a->bytesWritten() + (char*)buf), block->idx,
               block->code ? "SKIPPING" : "");

        cfg_code[block] = a->bytesWritten() + (char*)buf;
    }

    Tracer(CFGBlock* start, ASTInterpreter* interp)
        : start(start),
          failed(false),
          finished(false),
          interp(interp),
          entry(0),
          epilog(0),
          stack(0),
          abort_on_backage(false) {
        printf("beggining tracing %d\n", start->idx);
        buf = (unsigned char*)malloc(code_size + 16);
        buf = (unsigned char*)((((uint64_t)buf) + 15) & ~(15ul));
        assert(((uint64_t)buf % 16) == 0);
        a = std::unique_ptr<assembler::Assembler>(new assembler::Assembler(buf, code_size));
        // a->trap();
        a->push(assembler::RBP);
        a->mov(assembler::RSP, assembler::RBP);
        a->push(assembler::RDI); // interp
        a->push(assembler::RDI); // interp
        entry = a->bytesWritten();
        setCfgBlockEntry(start);

        epilog = code_size - (epilog_size);
        assembler::Assembler endAsm(buf + epilog, epilog_size);
        // endAsm.mov(assembler::RBP, assembler::RSP);
        // endAsm.pop(assembler::RBP);
        endAsm.leave();
        endAsm.retq();
        RELEASE_ASSERT(!endAsm.hasFailed(), "");
    }

    uint64_t toArg(InternedString s) {
        union U {
            U() {}
            InternedString is;
            uint64_t s;
        };
        U u;
        u.is = s;
        return u.s;
    }

    struct A {
        const assembler::Register regs[6]
            = { assembler::RDI, assembler::RSI, assembler::RDX, assembler::RCX, assembler::R8, assembler::R9 };
    };

    template <int offset> struct PopO : public A {
        int emit(assembler::Assembler& a, int arg_num) {
            a.pop(regs[offset == -1 ? arg_num : (offset)]);
            return -1;
        }
    };

    struct Pop : public PopO<-1> {};

    struct Imm : public A {
        const uint64_t val;
        Imm(int64_t val) : val(val) {}
        Imm(void* val) : val((uint64_t)val) {}
        int emit(assembler::Assembler& a, int arg_num) {
            a.mov(assembler::Immediate(val), regs[arg_num]);
            return 0;
        }
    };

    struct Mem : public A {
        const assembler::Indirect ind;
        Mem(assembler::Indirect ind) : ind(ind) {}
        int emit(assembler::Assembler& a, int arg_num) {
            a.mov(ind, regs[arg_num]);
            return 0;
        }
    };

    template <int arg_num, typename Arg1, typename... Args>
    static int emitCallHelper(assembler::Assembler& a, Arg1&& arg1, Args... args) {
        int stack = arg1.emit(a, arg_num);
        return stack + emitCallHelper<arg_num + 1>(a, args...);
    }

    template <int arg_num> static int emitCallHelper(assembler::Assembler& a) {
        static_assert(arg_num < 6, "to many args");
        return 0;
    }

    template <typename... Args> void emitCall(void* func, Args&&... args) {
        stack += emitCallHelper<0>(*a, args...);
        a->emitCall(func, assembler::R11);
        pushResult();
        if (a->hasFailed()) {
            interp->abortTracing();
            interp = 0;
        }
    }

    template <typename... Args> void emitVoidCall(void* func, Args&&... args) {
        stack += emitCallHelper<0>(*a, args...);
        a->emitCall(func, assembler::R11);
        if (a->hasFailed()) {
            interp->abortTracing();
            interp = 0;
        }
    }

    void pushResult() {
        a->push(assembler::RAX);
        ++stack;
    }

    assembler::Indirect getInterp() { return assembler::Indirect(assembler::RBP, -8); }

    void emitSetGlobal(Box* global, BoxedString* s) {
        emitVoidCall((void*)setGlobal, Imm(global), Imm((void*)s), Pop());
    }

    void emitGetGlobal(Box* global, BoxedString* s) { emitCall((void*)getGlobal, Imm((void*)global), Imm((void*)s)); }

    void emitSetAttr(BoxedString* s) { emitVoidCall((void*)setattr, Pop(), Imm((void*)s), Pop()); }

    void emitGetAttr(BoxedString* s) { emitCall((void*)getattr, Pop(), Imm((void*)s)); }

    void emitGetLocal(InternedString s) {
        emitCall((void*)ASTInterpreter::tracerHelperGetLocal, Mem(getInterp()), Imm(toArg(s)));
    }

    void emitSetLocal(InternedString s) {
        emitVoidCall((void*)ASTInterpreter::tracerHelperSetLocal, Mem(getInterp()), Imm(toArg(s)), Pop());
    }

    static Box* boxedLocalsGetHelper(ASTInterpreter* interp, BoxedString* s) {
        return boxedLocalsGet(interp->frame_info.boxedLocals, s, interp->globals);
    }

    void emitBoxedLocalsGet(BoxedString* s) { emitCall((void*)boxedLocalsGetHelper, Mem(getInterp()), Imm((void*)s)); }

    void emitSetItem() { emitVoidCall((void*)setitem, PopO<1>(), PopO<0>(), PopO<2>()); }
    void emitGetItem() { emitCall((void*)getitem, PopO<1>(), PopO<0>()); }

    static void setItemNameHelper(ASTInterpreter* interp, Box* str, Box* val) {
        assert(interp->frame_info.boxedLocals != NULL);
        setitem(interp->frame_info.boxedLocals, str, val);
    }

    void emitSetItemName(BoxedString* s) {
        emitVoidCall((void*)setItemNameHelper, Mem(getInterp()), Imm((void*)s), Pop());
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

    void emitDeref(InternedString s) { emitCall((void*)derefHelper, Mem(getInterp()), Imm(toArg(s))); }

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
            RELEASE_ASSERT(0, "not implemented");
    }

    void emitCreateList() { emitCall((void*)createList); }

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
            a->mov(assembler::Indirect(assembler::RAX, (num - i - 1) * 8), assembler::RDI);
            a->push(assembler::RDI);
            ++stack;
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

    void emitAugbinop(int op) { emitCall((void*)augbinop, PopO<1>(), PopO<0>(), Imm(op)); }

    static Box* binopICHelper(BinopIC* ic, Box* lhs, Box* rhs, int op) { return ic->call(lhs, rhs, op); }
    void emitBinop(int op) {
#if ENABLE_TRACING_IC
        emitCall((void*)binopICHelper, Imm((void*)new BinopIC), PopO<2>(), PopO<1>(), Imm(op));
#else
        emitCall((void*)binop, PopO<1>(), PopO<0>(), Imm(op));
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

    void emitInt(long n) { emitPush((uint64_t)boxInt(n)); }

    void emitFloat(double n) { emitPush((uint64_t)boxFloat(n)); }

    void emitUnicodeStr(llvm::StringRef s) {
        emitCall((void*)decodeUTF8StringPtr, Imm((void*)s.data()), Imm(s.size()));
    }

    static Box* callattrHelper(Box* b, BoxedString* attr, CallattrFlags flags, ArgPassSpec args, Box* arg1, Box* arg2) {
        return pyston::callattr(b, attr, flags, args, arg1, arg2, NULL, NULL, NULL);
    }

    void emitCallattr(BoxedString* attr, CallattrFlags flags, ArgPassSpec args) {
        RELEASE_ASSERT(args.num_args <= 2, "");
        if (args.num_args > 1) {
            a->pop(assembler::R9);
            --stack;
        }
        if (args.num_args > 0) {
            a->pop(assembler::R8);
            --stack;
        }

        a->pop(assembler::RDI);
        --stack;
        a->mov(assembler::Immediate((void*)attr), assembler::RSI);
        a->mov(assembler::Immediate(flags.asInt()), assembler::RDX);
        a->mov(assembler::Immediate(args.asInt()), assembler::RCX);
        a->emitCall((void*)callattrHelper, assembler::Register(11));
        pushResult();

        assert(stack == 1);
    }

    static Box* runtimeCallHelper(Box* obj, ArgPassSpec argspec, Box* arg1, Box* arg2, Box* arg3, Box** args) {
        return pyston::runtimeCall(obj, argspec, arg1, arg2, arg3, args, NULL);
    }

    void emitRuntimeCall(ArgPassSpec argspec) {
        if (argspec.num_args > 2) {
            a->pop(assembler::R8);
            --stack;
        }

        if (argspec.num_args > 1) {
            a->pop(assembler::RCX);
            --stack;
        }
        if (argspec.num_args > 0) {
            a->pop(assembler::RDX);
            --stack;
        }

        a->pop(assembler::RDI);
        --stack;
        a->mov(assembler::Immediate(argspec.asInt()), assembler::RSI);
        a->emitCall((void*)runtimeCallHelper, assembler::Register(11));
        pushResult();
    }

    void emitPush(uint64_t val) {
        a->mov(assembler::Immediate(val), assembler::RSI);
        a->push(assembler::RSI);
        ++stack;
    }

    void emitPop() {
        a->pop(assembler::RAX);
        --stack;
    }

    static Box* hasnextHelper(Box* b) { return boxBool(pyston::hasnext(b)); }

    void emitHasnext() { emitCall((void*)hasnextHelper, Pop()); }

    static void setCurrentInstHelper(ASTInterpreter* interp, AST_stmt* node) { interp->current_inst = node; }

    void emitSetCurrentInst(AST_stmt* node) {
        assert(stack == 0);
        emitVoidCall((void*)setCurrentInstHelper, Mem(getInterp()), Imm((void*)node));
    }

    void addGuard(bool t, CFGBlock* cont) {
        a->pop(assembler::RSI);
        stack--;

        a->mov(assembler::Immediate((void*)(t ? False : True)), assembler::RDI);
        a->cmp(assembler::RSI, assembler::RDI);
#if OLD
        a->jne(assembler::JumpDestination::fromStart(a->bytesWritten() + 17));
        assert(stack == 0);
        a->mov(assembler::Immediate(cont), assembler::RAX);
        a->jmp(assembler::JumpDestination::fromStart(epilog));
#else
        a->jne(assembler::JumpDestination::fromStart(a->bytesWritten() + 17 + 11 + 1));
        assert(stack == 0);
        a->mov(assembler::Immediate(cont), assembler::RAX);
        a->mov(assembler::Indirect(assembler::RAX, 8), assembler::RSI);
        a->test(assembler::RSI, assembler::RSI);
        a->je(assembler::JumpDestination::fromStart(a->bytesWritten() + 4 + 1));
        // a->emitByte(0xFF); a->emitByte(0x26); // jmp [rsi]
        a->emitByte(0xFF);
        a->emitByte(0x60);
        a->emitByte(0x08); // jmp qword ptr [rax+8]
        a->jmp(assembler::JumpDestination::fromStart(epilog));
#endif
    }

    void emitJump(CFGBlock* b) {
        assert(stack == 0);
        a->mov(assembler::Immediate((void*)b), assembler::RAX);
        a->emitByte(0xFF);
        a->emitByte(0x60);
        a->emitByte(0x08); // jmp qword ptr [rax+8]
    }

    void emitReturn() {
        assert(stack == 1);
        a->mov(assembler::Immediate(0ul), assembler::RAX);
        a->pop(assembler::RDX);
        a->jmp(assembler::JumpDestination::fromStart(epilog));
        --stack;
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
        eh_frame_addr = malloc(EH_FRAME_SIZE);
        writeTrivialEhFrame(eh_frame_addr, func_addr, func_size);
        // (EH_FRAME_SIZE - 4) to omit the 4-byte null terminator, otherwise we trip an assert in parseEhFrame.
        // TODO: can we omit the terminator in general?
        registerDynamicEhFrame((uint64_t)func_addr, func_size, (uint64_t)eh_frame_addr, EH_FRAME_SIZE - 4);
        registerEHFrames((uint8_t*)eh_frame_addr, (uint64_t)eh_frame_addr, EH_FRAME_SIZE);
    }


    void compile() {
        assert(stack == 0);
        if (a->hasFailed()) {
            interp->abortTracing();
            interp = 0;
            return;
        }
        interp = 0;
        RELEASE_ASSERT(!a->hasFailed(), "asm failed");
        a->jmp(assembler::JumpDestination::fromStart(entry));
        // printf("wrote %d\n", (int)a->bytesWritten());
        // printf("start: %p\n", buf);
        a->fillWithNopsExcept(epilog_size);
        llvm::sys::Memory::InvalidateInstructionCache(buf, a->bytesWritten());

        // generate eh frame... :-(
        EHwriteAndRegister((void*)buf, code_size);

        for (auto&& i : cfg_code) {
            if (!i.first->code) {
                assert(!i.first->code);
                i.first->code = i.second;
            }
        }

        finished = true;
        static int s = 0;
        ++s;
        printf("SUCCESS\n");
    }
};

void ASTInterpreter::abortTracing() {
    if (istracing) {
        istracing = false;
        // tracers.erase(tracer->start);

        tracers_aborted.insert(tracer->start);

        tracer = 0;
        edgecount = 0;
        printf("FAILED\n");
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

RegisterHelper::RegisterHelper(ASTInterpreter* interpreter, void* frame_addr)
    : frame_addr(frame_addr), interpreter(interpreter) {
    interpreter->frame_addr = frame_addr;
    s_interpreterMap[frame_addr] = interpreter;
}

RegisterHelper::~RegisterHelper() {
    interpreter->frame_addr = nullptr;
    deregister(frame_addr);
}

Value ASTInterpreter::execute(ASTInterpreter& interpreter, CFGBlock* start_block, AST_stmt* start_at) {
    STAT_TIMER(t0, "us_timer_astinterpreter_execute");

    void* frame_addr = __builtin_frame_address(0);
    RegisterHelper frame_registerer(&interpreter, frame_addr);

    Value v;

    bool trace = false;
    bool from_start = start_block == NULL && start_at == NULL;

    assert((start_block == NULL) == (start_at == NULL));
    if (start_block == NULL) {
        start_block = interpreter.source_info->cfg->getStartingBlock();
        start_at = start_block->body[0];

        if (ENABLE_TRACING_FUNC && interpreter.compiled_func->times_called == 25) {
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
        interpreter.istracing = true;
        interpreter.tracer = new Tracer(start_block, &interpreter);
        interpreter.tracer->abort_on_backage = true;
    } else if (ENABLE_TRACING_FUNC && ENABLE_TRACING && from_start && tracers.count(start_block)) {
        started = true;
        assert(tracers[start_block]->interp == 0);
        typedef std::pair<CFGBlock*, Box*>(*Foo)(ASTInterpreter*);

        std::pair<CFGBlock*, Box*> rtn = ((Foo)(tracers[start_block]->buf))(&interpreter);
        interpreter.next_block = rtn.first;
        if (!interpreter.next_block)
            return rtn.second;
    }

    if (!started) {
        for (auto s : start_block->body) {
            if (!started) {
                if (s != start_at)
                    continue;
                started = true;
            }

            interpreter.current_inst = s;
            v = interpreter.visit_stmt(s);
        }
    }

    bool was_tracing = false;

    int num_aborts = 0;
    static StatCounter num_trace_aborts("num_trace_aborts");

    while (interpreter.next_block) {
        interpreter.current_block = interpreter.next_block;
        interpreter.next_block = 0;

        if (ENABLE_TRACING && !interpreter.istracing) {
            auto it = tracers.find(interpreter.current_block);
            if (it != tracers.end()) {
                was_tracing = true;
                assert(it->second->finished);
                assert(it->second->interp == 0);
                typedef std::pair<CFGBlock*, Box*>(*Foo)(ASTInterpreter*);
                std::pair<CFGBlock*, Box*> rtn = ((Foo)(it->second->buf))(&interpreter);
                interpreter.next_block = rtn.first;
                if (!interpreter.next_block) {
                    v = rtn.second;
                    continue;
                }
                ++num_aborts;
                if (num_aborts >= 5) {
                    interpreter.edgecount = 0;
                    num_aborts = 0;
                    num_trace_aborts.log(5);
                }
                continue;
            }
        }
/*
        if (interpreter.istracing && interpreter.current_block->code) {
            interpreter.tracer->compile();
            interpreter.istracing = 0;
            interpreter.tracer = 0;
        } else if (interpreter.istracing && interpreter.current_block != interpreter.tracer->start) {
            if (interpreter.istracing && interpreter.tracer->have_block_traced(interpreter.current_block)) {
                interpreter.tracer->emitJump(interpreter.current_block);
                interpreter.tracer->compile();
                interpreter.istracing = 0;
                interpreter.tracer = 0;
            } else
                interpreter.tracer->setCfgBlockEntry(interpreter.current_block);
        } else if (was_tracing && !interpreter.istracing) {
            interpreter.istracing = true;
            interpreter.tracer = new Tracer(interpreter.current_block, &interpreter);
        }
*/
#if ENABLE_TRACING
        if (interpreter.istracing
            && (interpreter.current_block != interpreter.tracer->start || interpreter.current_block->code)) {
            if (interpreter.istracing && interpreter.tracer->have_block_traced(interpreter.current_block)) {
                assert(0);
            } else
                interpreter.tracer->setCfgBlockEntry(interpreter.current_block);
        } else if (was_tracing && !interpreter.istracing && tracers_aborted.count(interpreter.current_block) == 0) {
            interpreter.istracing = true;
            interpreter.tracer = new Tracer(interpreter.current_block, &interpreter);
        }
#endif
        for (AST_stmt* s : interpreter.current_block->body) {
            interpreter.current_inst = s;
            v = interpreter.visit_stmt(s);
        }
    }
    return v;
}

Value ASTInterpreter::doBinOp(Box* left, Box* right, int op, BinExpType exp_type) {
    switch (exp_type) {
        case BinExpType::AugBinOp:
            if (istracing)
                tracer->emitAugbinop(op);
            return augbinop(left, right, op);
        case BinExpType::BinOp:
            if (istracing)
                tracer->emitBinop(op);
            return binop(left, right, op);
        case BinExpType::Compare:
            if (istracing)
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
        if (istracing)
            tracer->emitSetGlobal(globals, name.getBox());
        setGlobal(globals, name.getBox(), value.o);
    } else if (vst == ScopeInfo::VarScopeType::NAME) {
        if (istracing)
            tracer->emitSetItemName(name.getBox());

        assert(frame_info.boxedLocals != NULL);
        // TODO should probably pre-box the names when it's a scope that usesNameLookup
        setitem(frame_info.boxedLocals, name.getBox(), value.o);
    } else {
        if (istracing)
            tracer->emitSetLocal(name);

        sym_table[name] = value.o;
        if (vst == ScopeInfo::VarScopeType::CLOSURE) {
            abortTracing();
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
        if (istracing)
            tracer->emitSetAttr(attr->attr.getBox());
        pyston::setattr(o.o, attr->attr.getBox(), value.o);
    } else if (node->type == AST_TYPE::Tuple) {
        AST_Tuple* tuple = (AST_Tuple*)node;
        Box** array = unpackIntoArray(value.o, tuple->elts.size());

        if (istracing)
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

        if (istracing)
            tracer->emitSetItem();

        setitem(target.o, slice.o, value.o);
    } else {
        RELEASE_ASSERT(0, "not implemented");
    }
}

Value ASTInterpreter::visit_unaryop(AST_UnaryOp* node) {
    Value operand = visit_expr(node->operand);
    if (node->op_type == AST_TYPE::Not) {
        if (istracing)
            tracer->emitNotNonzero();

        return boxBool(!nonzero(operand.o));
    } else {
        abortTracing();
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
    if (istracing && !node->lower)
        tracer->emitPush((uint64_t)None);
    Value upper = node->upper ? visit_expr(node->upper) : None;
    if (istracing && !node->upper)
        tracer->emitPush((uint64_t)None);
    Value step = node->step ? visit_expr(node->step) : None;
    if (istracing && !node->step)
        tracer->emitPush((uint64_t)None);

    if (istracing)
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

    if (istracing)
        tracer->addGuard(v.o == True, v.o == True ? node->iffalse : node->iftrue);
    if (v.o == True)
        next_block = node->iftrue;
    else
        next_block = node->iffalse;


    if (tracer && istracing && next_block->code) {
        tracer->emitJump(next_block);
        tracer->compile();
        if (istracing)
            tracers[tracer->start] = tracer;
        istracing = false;
        tracer = 0;
    }

    return Value();
}

Value ASTInterpreter::visit_jump(AST_Jump* node) {
    bool backedge = node->target->idx < current_block->idx && compiled_func;
    if (backedge)
        threading::allowGLReadPreemption();

    if (tracer && istracing && node->target == tracer->start) {
        // assert(tracers.count(std::make_pair(node->target, current_block)) == 0);
        tracer->compile();
        if (istracing)
            tracers[node->target] = tracer;
        istracing = false;
        tracer = 0;
    }

    if (tracer && istracing && node->target->code) {
        tracer->emitJump(node->target);
        tracer->compile();
        if (istracing)
            tracers[tracer->start] = tracer;
        istracing = false;
        tracer = 0;
    }

    if (backedge)
        ++edgecount;

    if (backedge && tracer && istracing && tracer->abort_on_backage)
        abortTracing();

    if (ENABLE_TRACING && backedge && edgecount == 5 && !tracer && tracers_aborted.count(node->target) == 0) {
        auto t = tracers.find(node->target);
        if (t == tracers.end()) {
            tracer = new Tracer(node->target, this);
            istracing = true;
        }
    }

    if (0 && ENABLE_OSR && backedge && edgecount == OSR_THRESHOLD_INTERPRETER) {
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
    abortTracing();
    Value v;
    try {
        v = visit_stmt(node->stmt);
        next_block = node->normal_dest;
    } catch (ExcInfo e) {
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
        if (istracing)
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
        if (istracing)
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
        if (istracing)
            tracer->emitUncacheExcInfo();

        getFrameInfo()->exc = ExcInfo(NULL, NULL, NULL);
        v = None;
    } else if (node->opcode == AST_LangPrimitive::HASNEXT) {
        assert(node->args.size() == 1);
        Value obj = visit_expr(node->args[0]);

        if (istracing)
            tracer->emitHasnext();

        v = boxBool(hasnext(obj.o));
    } else
        RELEASE_ASSERT(0, "unknown opcode %d", node->opcode);
    return v;
}

Value ASTInterpreter::visit_yield(AST_Yield* node) {
    Value value = node->value ? visit_expr(node->value) : None;
    assert(generator && generator->cls == generator_cls);

    if (istracing)
        tracer->emitYield(node->value != 0);

    return yield(generator, value.o);
}

Value ASTInterpreter::visit_stmt(AST_stmt* node) {
#if ENABLE_SAMPLING_PROFILER
    threading::allowGLReadPreemption();
#endif

    if (0 && istracing) {
        printf("%20s % 2d ", source_info->getName().c_str(), current_block->idx);
        print_ast(node);
        printf("\n");
    }

    if (istracing)
        tracer->emitSetCurrentInst(node);

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

    if (istracing && !node->value)
        tracer->emitPush((uint64_t)None);

    if (istracing) {
        tracer->emitReturn();
        if (istracing) {
            tracer->compile();
            if (istracing)
                tracers[tracer->start] = tracer;
            istracing = false;
        }
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

    std::vector<BoxedString*> keywords;
    for (AST_keyword* k : node->keywords) {
        keywords.push_back(k->arg.getBox());
        args.push_back(visit_expr(k->value).o);
    }

    if (node->starargs)
        args.push_back(visit_expr(node->starargs).o);

    if (node->kwargs)
        args.push_back(visit_expr(node->kwargs).o);

    ArgPassSpec argspec(node->args.size(), node->keywords.size(), node->starargs, node->kwargs);

    if (is_callattr) {
        if (argspec.has_starargs || argspec.num_keywords || argspec.has_kwargs)
            abortTracing();

        if (args.size() > 2)
            abortTracing();


        CallattrFlags flags{.cls_only = callattr_clsonly, .null_on_nonexistent = false };

        if (istracing)
            tracer->emitCallattr(attr.getBox(), flags, argspec);

        return callattr(func.o, attr.getBox(),
                        CallattrFlags({.cls_only = callattr_clsonly, .null_on_nonexistent = false }), argspec,
                        args.size() > 0 ? args[0] : 0, args.size() > 1 ? args[1] : 0, args.size() > 2 ? args[2] : 0,
                        args.size() > 3 ? &args[3] : 0, &keywords);
    } else {
        if (argspec.num_keywords || argspec.has_starargs)
            abortTracing();

        if (args.size() > 3)
            abortTracing();

        if (istracing)
            tracer->emitRuntimeCall(argspec);

        return runtimeCall(func.o, argspec, args.size() > 0 ? args[0] : 0, args.size() > 1 ? args[1] : 0,
                           args.size() > 2 ? args[2] : 0, args.size() > 3 ? &args[3] : 0, &keywords);
    }
}


Value ASTInterpreter::visit_expr(AST_Expr* node) {
    Value rtn = visit_expr(node->value);
    if (istracing)
        tracer->emitPop();
    return rtn;
}

Value ASTInterpreter::visit_num(AST_Num* node) {
    if (node->num_type == AST_Num::INT) {
        if (istracing)
            tracer->emitInt(node->n_int);
        return boxInt(node->n_int);
    } else if (node->num_type == AST_Num::FLOAT) {
        if (istracing)
            tracer->emitFloat(node->n_float);
        return boxFloat(node->n_float);
    } else if (node->num_type == AST_Num::LONG) {
        if (istracing)
            tracer->emitPush((uint64_t)createLong(node->n_long));
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
    if (istracing) {
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
        if (istracing)
            tracer->emitPush((uint64_t)source_info->parent_module->getStringConstant(node->str_data));
        return source_info->parent_module->getStringConstant(node->str_data);
    } else if (node->str_type == AST_Str::UNICODE) {
        if (istracing)
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
            if (istracing)
                tracer->emitGetGlobal(globals, node->id.getBox());

            return getGlobal(globals, node->id.getBox());
        case ScopeInfo::VarScopeType::DEREF: {

            if (istracing)
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
            if (istracing)
                tracer->emitGetLocal(node->id);

            SymMap::iterator it = sym_table.find(node->id);
            if (it != sym_table.end())
                return sym_table.getMapped(it->second);

            assertNameDefined(0, node->id.c_str(), UnboundLocalError, true);
            return Value();
        }
        case ScopeInfo::VarScopeType::NAME: {
            if (istracing)
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

    if (istracing)
        tracer->emitGetItem();

    return getitem(value.o, slice.o);
}

Value ASTInterpreter::visit_list(AST_List* node) {
    if (istracing) {
        if (node->elts.empty())
            tracer->emitCreateList();
        else
            abortTracing();
    }

    BoxedList* list = new BoxedList;
    list->ensure(node->elts.size());
    for (AST_expr* e : node->elts)
        listAppendInternal(list, visit_expr(e).o);
    return list;
}

Value ASTInterpreter::visit_tuple(AST_Tuple* node) {
    BoxedTuple* rtn = BoxedTuple::create(node->elts.size());
    int rtn_idx = 0;
    for (AST_expr* e : node->elts)
        rtn->elts[rtn_idx++] = visit_expr(e).o;

    if (node->elts.size() > 5)
        abortTracing();

    if (istracing)
        tracer->emitCreateTuple(node->elts.size());

    return rtn;
}

Value ASTInterpreter::visit_attribute(AST_Attribute* node) {
    Value v = visit_expr(node->value);
    if (istracing)
        tracer->emitGetAttr(node->attr.getBox());
    return pyston::getattr(v.o, node->attr.getBox());
}
}

const void* interpreter_instr_addr = (void*)&ASTInterpreter::execute;

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
