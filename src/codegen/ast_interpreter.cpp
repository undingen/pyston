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
#include "codegen/memmgr.h"
#include "codegen/unwinding.h"
#include "codegen/irgen.h"
#include "codegen/irgen/hooks.h"
#include "codegen/irgen/irgenerator.h"
#include "codegen/irgen/util.h"
#include "codegen/osrentry.h"
#include "core/ast.h"
#include "core/cfg.h"
#include "core/common.h"
#include "core/contiguous_map.h"
#include "core/stats.h"
#include "core/thread_utils.h"
#include "core/util.h"
#include "runtime/capi.h"
#include "runtime/generator.h"
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

namespace {

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

class ASTInterpreter {
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


    llvm::DenseMap<CFGBlock*, int> freq;

public:
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

    void gcVisit(GCVisitor* visitor);

    friend class Tracer;

    class Tracer* tracer = 0;
    bool istracing = 0;

    void abortTracing() {
        if (istracing) {
            istracing = false;
            tracer = 0;
            printf("FAILED\n");
        }
    }

    static Box* tracerHelperGetLocal(ASTInterpreter* i, InternedString id) {
        SymMap::iterator it = i->sym_table.find(id);
        if (it != i->sym_table.end())
            return i->sym_table.getMapped(it->second);

        assertNameDefined(0, id.c_str(), UnboundLocalError, true);
        return 0;
    }

    static void tracerHelperSetLocal(ASTInterpreter* i, InternedString id, Box* v) {
        i->sym_table[id] = v;
    }

};

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

    const int code_size = 4096;
    const int epilog_size = 2;//3+1+1;

    //assembler::Assembler* a;

    Tracer(CFGBlock* start, ASTInterpreter* interp) : start(start), failed(false), finished(false), interp(interp), entry(0), epilog(0), stack(0) {
        buf = (unsigned char*)malloc(code_size+16);
        buf = (unsigned char*)((((uint64_t)buf) + 15) & ~(15ul)) ;
        assert(((uint64_t)buf%16) == 0);
        a = std::unique_ptr<assembler::Assembler>(new assembler::Assembler(buf, code_size));
        //a->trap();
        a->push(assembler::RBP);
        a->mov(assembler::RSP, assembler::RBP);
        a->push(assembler::RDI); // interp
        entry = a->bytesWritten();

        epilog = code_size-(epilog_size);
        assembler::Assembler endAsm(buf+epilog, epilog_size);
        //endAsm.mov(assembler::RBP, assembler::RSP);
        //endAsm.pop(assembler::RBP);
        endAsm.leave();
        endAsm.retq();
        RELEASE_ASSERT(!endAsm.hasFailed(), "");
    }

    void emitCall(void* addr) {
        a->emitCall(addr, assembler::Register(11));
    }

    void emitCall(void* addr, uint64_t arg1) {
        a->mov(assembler::Immediate(arg1), assembler::RDI);
        a->emitCall(addr, assembler::Register(11));
    }

    void emitCall(void* addr, uint64_t arg1, uint64_t arg2) {
        a->mov(assembler::Immediate(arg1), assembler::RDI);
        a->mov(assembler::Immediate(arg2), assembler::RSI);
        a->emitCall(addr, assembler::Register(11));
    }

    void emitCall(void* addr, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
        a->mov(assembler::Immediate(arg1), assembler::RDI);
        a->mov(assembler::Immediate(arg2), assembler::RSI);
        a->mov(assembler::Immediate(arg3), assembler::RDX);
        a->emitCall(addr, assembler::Register(11));
    }

    void emitCall(void* addr, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4) {
        a->mov(assembler::Immediate(arg1), assembler::RDI);
        a->mov(assembler::Immediate(arg2), assembler::RSI);
        a->mov(assembler::Immediate(arg3), assembler::RDX);
        a->mov(assembler::Immediate(arg4), assembler::RCX);
        a->emitCall(addr, assembler::Register(11));
    }

    void emitCall(void* addr, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5) {
        a->mov(assembler::Immediate(arg1), assembler::RDI);
        a->mov(assembler::Immediate(arg2), assembler::RSI);
        a->mov(assembler::Immediate(arg3), assembler::RDX);
        a->mov(assembler::Immediate(arg4), assembler::RCX);
        a->mov(assembler::Immediate(arg5), assembler::R8);
        a->emitCall(addr, assembler::Register(11));
    }

    void emitCall(void* addr, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t arg6) {
        a->mov(assembler::Immediate(arg1), assembler::RDI);
        a->mov(assembler::Immediate(arg2), assembler::RSI);
        a->mov(assembler::Immediate(arg3), assembler::RDX);
        a->mov(assembler::Immediate(arg4), assembler::RCX);
        a->mov(assembler::Immediate(arg5), assembler::R8);
        a->mov(assembler::Immediate(arg6), assembler::R9);
        a->emitCall(addr, assembler::Register(11));
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

    void setGlobal(Box* global, llvm::StringRef s) {
        a->mov(assembler::Immediate((uint64_t)global), assembler::RDI);
        a->mov(assembler::Immediate((uint64_t)s.data()), assembler::RSI);
        a->mov(assembler::Immediate((uint64_t)s.size()), assembler::RDX);
        a->pop(assembler::RCX);
        a->emitCall((void*)pyston::setGlobal, assembler::Register(11));
        stack--;
    }

    void getGlobal(Box* global, const std::string* s) {
        emitCall((void*)pyston::getGlobal, (uint64_t)global, (uint64_t)s);
        a->push(assembler::RAX);
        stack++;
    }

    void setAttr(const char* s) {
        a->pop(assembler::RDI);
        a->mov(assembler::Immediate((uint64_t)s), assembler::RSI);
        a->pop(assembler::RDX);
        a->emitCall((void*)pyston::setattr, assembler::Register(11));
        stack -= 2;
    }

    void getAttr(const char* s) {
        a->pop(assembler::RDI);
        a->mov(assembler::Immediate((uint64_t)s), assembler::RSI);
        a->emitCall((void*)pyston::getattr, assembler::Register(11));
        a->push(assembler::RAX);
    }

    assembler::Indirect getInterp() {
        return assembler::Indirect(assembler::RBP, -8);
    }

    void getLocal(InternedString s) {
        a->mov(getInterp(), assembler::RDI);
        a->mov(assembler::Immediate((uint64_t)toArg(s)), assembler::RSI);
        a->emitCall((void*)ASTInterpreter::tracerHelperGetLocal, assembler::Register(11));
        a->push(assembler::RAX);
        stack++;
    }

    void storeLocal(InternedString s) {
        a->mov(getInterp(), assembler::RDI);
        a->mov(assembler::Immediate(toArg(s)), assembler::RSI);
        a->pop(assembler::RDX);
        a->emitCall((void*)ASTInterpreter::tracerHelperSetLocal, assembler::Register(11));
        stack--;
    }

    void setItem() {
        a->pop(assembler::RSI);
        a->pop(assembler::RDI);
        a->pop(assembler::RDX);
        a->emitCall((void*)pyston::setitem, assembler::Register(11));
        stack -= 3;
    }

    static long nonzeroHelper(Box* b) {
        bool result = pyston::nonzero(b);
        if (result == true)
            return 1;
        return 0;
    }

    void nonzero() {
        a->pop(assembler::RDI);
        emitCall((void*)nonzeroHelper);
        a->push(assembler::RAX);
    }


    void addGuard(bool t, CFGBlock* cont) {
        a->pop(assembler::RSI);
        stack--;
        a->test(assembler::RSI, assembler::RSI);
        if (t)
            a->jne(assembler::JumpDestination::fromStart(a->bytesWritten()+17));
        else
            a->je(assembler::JumpDestination::fromStart(a->bytesWritten()+17));
        assert(stack == 0);
        a->mov(assembler::Immediate(cont), assembler::RAX);
        a->jmp(assembler::JumpDestination::fromStart(epilog));
    }

    void emitReturn() {
        assert(stack == 0);
        a->mov(assembler::Immediate(0ul), assembler::RAX);
        a->jmp(assembler::JumpDestination::fromStart(epilog));
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
        RELEASE_ASSERT(!a->hasFailed(), "asm failed");
        a->jmp(assembler::JumpDestination::fromStart(entry));
        printf("wrote %d\n", (int)a->bytesWritten());
        a->fillWithNopsExcept(epilog_size);
        llvm::sys::Memory::InvalidateInstructionCache(buf, a->bytesWritten());

        // generate eh frame... :-(
        EHwriteAndRegister((void*)buf, code_size);


        finished = true;
    }

    void compare(int op) {
        a->pop(assembler::RSI);
        a->pop(assembler::RDI);
        a->mov(assembler::Immediate(op), assembler::RDX);
        a->emitCall((void*)pyston::compare, assembler::Register(11));
        a->push(assembler::RAX);
        stack--;
    }

    void augbinop(int op) {
        a->pop(assembler::RSI);
        a->pop(assembler::RDI);
        a->mov(assembler::Immediate(op), assembler::RDX);
        a->emitCall((void*)pyston::augbinop, assembler::Register(11));
        a->push(assembler::RAX);
        stack--;
    }

    void binop(int op) {
        a->pop(assembler::RSI);
        a->pop(assembler::RDI);
        a->mov(assembler::Immediate(op), assembler::RDX);
        a->emitCall((void*)pyston::binop, assembler::Register(11));
        a->push(assembler::RAX);
        stack--;
    }

    void yield(Box* generator, bool hasvalue) {
        a->mov(assembler::Immediate((void*)generator), assembler::RDI);
        if (hasvalue) {
            a->pop(assembler::RSI);
            stack--;
        } else
            a->mov(assembler::Immediate((void*)None), assembler::RSI);
        a->emitCall((void*)pyston::yield, assembler::Register(11));
        a->push(assembler::RAX);
        stack++;
    }

    static Box* uncacheExcInfoHelper(ASTInterpreter* inter) {
        inter->getFrameInfo()->exc = ExcInfo(NULL, NULL, NULL);
        return None;
    }

    void uncacheExcInfo() {
        a->mov(getInterp(), assembler::RDI); // interp pointer
        emitCall((void*)uncacheExcInfoHelper);
        a->push(assembler::RAX);
        stack++;
    }

    void numInt(long n) {
        emitCall((void*)pyston::boxInt, n);
        a->push(assembler::RAX);
        stack++;
    }

    void numFloat(double n) {
        push((uint64_t)boxFloat(n));
    }

    static Box* callattrHelper(Box* b, const std::string* attr, CallattrFlags flags, ArgPassSpec args, Box* arg1, Box* arg2) {
        return pyston::callattr(b, attr, flags, args, arg1, arg2, NULL, NULL, NULL);
    }

    void callattr(const std::string* attr, CallattrFlags flags, ArgPassSpec args) {
        //a->trap();
        RELEASE_ASSERT(args.num_args <= 2, "");
        if (args.num_args > 1) {
            a->pop(assembler::R9);
            stack--;
        }
        if (args.num_args > 0) {
            a->pop(assembler::R8);
            stack--;
        }

        a->pop(assembler::RDI);
        a->mov(assembler::Immediate((void*)attr), assembler::RSI);
        a->mov(assembler::Immediate(flags.asInt()), assembler::RDX);
        a->mov(assembler::Immediate(args.asInt()), assembler::RCX);
        a->emitCall((void*)callattrHelper, assembler::Register(11));
        a->push(assembler::RAX);
    }

    static Box* runtimeCallHelper(Box* obj, ArgPassSpec argspec, Box* arg1, Box* arg2, Box* arg3) {
        return pyston::runtimeCall(obj, argspec, arg1, arg2, arg3, NULL, NULL);
    }

    void runtimeCall(ArgPassSpec args) {
        //a->trap();
        RELEASE_ASSERT(args.num_args <= 3, "");
        if (args.num_args > 2) {
            a->pop(assembler::R8);
            stack--;
        }

        if (args.num_args > 1) {
            a->pop(assembler::RCX);
            stack--;
        }
        if (args.num_args > 0) {
            a->pop(assembler::RDX);
            stack--;
        }

        a->pop(assembler::RDI);
        a->mov(assembler::Immediate(args.asInt()), assembler::RSI);
        a->emitCall((void*)runtimeCallHelper, assembler::Register(11));
        a->push(assembler::RAX);
    }

    void push(uint64_t val) {
        a->mov(assembler::Immediate(val), assembler::RSI);
        a->push(assembler::RSI);
        stack++;
    }

    void pop() {
        a->pop(assembler::RAX);
        stack--;
    }

    static Box* hasnextHelper(Box* b) {
        return boxBool(pyston::hasnext(b));
    }

    void hasnext() {
        a->pop(assembler::RDI);
        a->emitCall((void*)hasnextHelper, assembler::Register(11));
        a->push(assembler::RAX);
    }

    static void setCurrentInstHelper(ASTInterpreter* interp, AST_stmt* node) {
        interp->current_inst = node;
    }

    void setCurrentInst(AST_stmt* node) {
        a->mov(getInterp(), assembler::RDI);
        a->mov(assembler::Immediate((uint64_t)node), assembler::RSI);
        a->emitCall((void*)setCurrentInstHelper, assembler::Register(11));
    }
};


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

void ASTInterpreter::gcVisit(GCVisitor* visitor) {
    visitor->visitRange((void* const*)&sym_table.vector()[0], (void* const*)&sym_table.vector()[sym_table.size()]);
    if (passed_closure)
        visitor->visit(passed_closure);
    if (created_closure)
        visitor->visit(created_closure);
    if (generator)
        visitor->visit(generator);
    if (frame_info.boxedLocals)
        visitor->visit(frame_info.boxedLocals);
    visitor->visit(globals);
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
      frame_info(ExcInfo(NULL, NULL, NULL)) {

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

// Map from stack frame pointers for frames corresponding to ASTInterpreter::execute() to the ASTInterpreter handling
// them. Used to look up information about that frame. This is used for getting tracebacks, for CPython introspection
// (sys._getframe & co), and for GC scanning.
static std::unordered_map<void*, ASTInterpreter*> s_interpreterMap;
static_assert(THREADING_USE_GIL, "have to make the interpreter map thread safe!");

class RegisterHelper {
private:
    void* frame_addr;

public:
    RegisterHelper(ASTInterpreter* interpreter, void* frame_addr) : frame_addr(frame_addr) {
        s_interpreterMap[frame_addr] = interpreter;
    }
    ~RegisterHelper() {
        assert(s_interpreterMap.count(frame_addr));
        s_interpreterMap.erase(frame_addr);
    }
};

Value ASTInterpreter::execute(ASTInterpreter& interpreter, CFGBlock* start_block, AST_stmt* start_at) {
    STAT_TIMER(t0, "us_timer_astinterpreter_execute");

    void* frame_addr = __builtin_frame_address(0);
    RegisterHelper frame_registerer(&interpreter, frame_addr);

    Value v;

    assert((start_block == NULL) == (start_at == NULL));
    if (start_block == NULL) {
        start_block = interpreter.source_info->cfg->getStartingBlock();
        start_at = start_block->body[0];
    }

    // Important that this happens after RegisterHelper:
    interpreter.current_inst = start_at;
    threading::allowGLReadPreemption();
    interpreter.current_inst = NULL;

    interpreter.current_block = start_block;
    bool started = false;
    for (auto s : start_block->body) {
        if (!started) {
            if (s != start_at)
                continue;
            started = true;
        }

        interpreter.current_inst = s;
        v = interpreter.visit_stmt(s);
    }

    while (interpreter.next_block) {
        interpreter.current_block = interpreter.next_block;
        interpreter.next_block = 0;

        if (interpreter.tracer && interpreter.tracer->start == interpreter.current_block) {
            if (interpreter.tracer->finished) {
                typedef CFGBlock* (*Foo)(ASTInterpreter*);
                interpreter.next_block = ((Foo)(interpreter.tracer->buf))(&interpreter);
                continue;
            }
        }

        for (AST_stmt* s : interpreter.current_block->body) {
            interpreter.current_inst = s;
            v = interpreter.visit_stmt(s);
        }
    }
    return v;
}

Value ASTInterpreter::doBinOp(Box* left, Box* right, int op, BinExpType exp_type) {
    if (op == AST_TYPE::Div && (source_info->parent_module->future_flags & FF_DIVISION)) {
        op = AST_TYPE::TrueDiv;
    }
    switch (exp_type) {
        case BinExpType::AugBinOp:
            if (istracing) {
                tracer->augbinop(op);
            }
            return augbinop(left, right, op);
        case BinExpType::BinOp:
            if (istracing) {
                tracer->binop(op);
            }
            return binop(left, right, op);
        case BinExpType::Compare:
            if (istracing) {
                tracer->compare(op);
            }
            return compare(left, right, op);
        default:
            RELEASE_ASSERT(0, "not implemented");
    }
    return Value();
}

void ASTInterpreter::doStore(InternedString name, Value value) {
    ScopeInfo::VarScopeType vst = scope_info->getScopeTypeOfName(name);
    if (vst == ScopeInfo::VarScopeType::GLOBAL) {
        if (istracing) {
            tracer->setGlobal(globals, name);
        }
        setGlobal(globals, name, value.o);
    } else if (vst == ScopeInfo::VarScopeType::NAME) {
        abortTracing();
        assert(frame_info.boxedLocals != NULL);
        // TODO should probably pre-box the names when it's a scope that usesNameLookup
        setitem(frame_info.boxedLocals, boxString(name.str()), value.o);
    } else {
        if (tracer && istracing) {
            tracer->storeLocal(name);
        }


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
            tracer->setAttr(attr->attr.c_str());
        setattr(o.o, attr->attr.c_str(), value.o);
    } else if (node->type == AST_TYPE::Tuple) {
        abortTracing();
        AST_Tuple* tuple = (AST_Tuple*)node;
        Box** array = unpackIntoArray(value.o, tuple->elts.size());
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
            tracer->setItem();

        setitem(target.o, slice.o, value.o);
    } else {
        RELEASE_ASSERT(0, "not implemented");
    }
}

Value ASTInterpreter::visit_unaryop(AST_UnaryOp* node) {
    abortTracing();
    Value operand = visit_expr(node->operand);
    if (node->op_type == AST_TYPE::Not)
        return boxBool(!nonzero(operand.o));
    else
        return unaryop(operand.o, node->op_type);
}

Value ASTInterpreter::visit_binop(AST_BinOp* node) {
    Value left = visit_expr(node->left);
    Value right = visit_expr(node->right);
    return doBinOp(left.o, right.o, node->op_type, BinExpType::BinOp);
}

Value ASTInterpreter::visit_slice(AST_Slice* node) {
    abortTracing();
    Value lower = node->lower ? visit_expr(node->lower) : None;
    Value upper = node->upper ? visit_expr(node->upper) : None;
    Value step = node->step ? visit_expr(node->step) : None;
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
    return Value();
}

Value ASTInterpreter::visit_jump(AST_Jump* node) {
    bool backedge = node->target->idx < current_block->idx && compiled_func;
    if (backedge)
        threading::allowGLReadPreemption();

    if (tracer && istracing && node->target == tracer->start) {
        tracer->compile();
        istracing = false;
    }

    ++edgecount;
    if (1 && backedge && edgecount == 5 && !tracer) {
        Tracer*& t = tracers[node->target];
        if (!t) {
            tracer = new Tracer(node->target, this);
            t = tracer;
            istracing = true;
        } else {
            printf("reusing\n");
            tracer = t;
            t->interp = this;
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
                    if (isIsDefinedName(it.first.str()))
                        entry->args[it.first] = BOOL;
                    else if (it.first.str() == PASSED_GENERATOR_NAME)
                        entry->args[it.first] = GENERATOR;
                    else if (it.first.str() == PASSED_CLOSURE_NAME || it.first.str() == CREATED_CLOSURE_NAME)
                        entry->args[it.first] = CLOSURE;
                    else if (it.first.str() == FRAME_INFO_PTR_NAME)
                        entry->args[it.first] = FRAME_INFO;
                    else {
                        assert(it.first.str()[0] != '!');
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
    return getclsattr(visit_expr(node->value).o, node->attr.c_str());
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
        abortTracing();
        assert(node->args.size() == 1);
        v = getPystonIter(visit_expr(node->args[0]).o);
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
        v = importFrom(module.o, &name);
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
        v = import(level, froms.o, &module_name);
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
        if (tracer && istracing) {
            tracer->nonzero();
        }
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
            tracer->uncacheExcInfo();

        getFrameInfo()->exc = ExcInfo(NULL, NULL, NULL);
        v = None;
    } else if (node->opcode == AST_LangPrimitive::HASNEXT) {
        assert(node->args.size() == 1);
        Value obj = visit_expr(node->args[0]);

        if (istracing)
            tracer->hasnext();

        v = boxBool(hasnext(obj.o));
    } else
        RELEASE_ASSERT(0, "unknown opcode %d", node->opcode);
    return v;
}

Value ASTInterpreter::visit_yield(AST_Yield* node) {
    Value value = node->value ? visit_expr(node->value) : None;
    assert(generator && generator->cls == generator_cls);

    if (istracing) {
        tracer->yield(generator, node->value != 0);
    }

    return yield(generator, value.o);
}

Value ASTInterpreter::visit_stmt(AST_stmt* node) {
#if ENABLE_SAMPLING_PROFILER
    threading::allowGLReadPreemption();
#endif

    if (1 && istracing) {
        printf("%20s % 2d ", source_info->getName().c_str(), current_block->idx);
        print_ast(node);
        printf("\n");
    }

    if (istracing)
        tracer->setCurrentInst(node);

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

    if (node->value)
        abortTracing();

    if (istracing)
        tracer->emitReturn();

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

    Box* classobj = createUserClass(&node->name.str(), basesTuple, attrDict);

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

    static std::string AssertionError_str("AssertionError");
    Box* assertion_type = getGlobal(globals, &AssertionError_str);
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
                delattr(visit_expr(attr->value).o, attr->attr.c_str());
                break;
            }
            case AST_TYPE::Name: {
                AST_Name* target = (AST_Name*)target_;

                ScopeInfo::VarScopeType vst = scope_info->getScopeTypeOfName(target->id);
                if (vst == ScopeInfo::VarScopeType::GLOBAL) {
                    delGlobal(globals, &target->id.str());
                    continue;
                } else if (vst == ScopeInfo::VarScopeType::NAME) {
                    assert(frame_info.boxedLocals != NULL);
                    if (frame_info.boxedLocals->cls == dict_cls) {
                        auto& d = static_cast<BoxedDict*>(frame_info.boxedLocals)->d;
                        auto it = d.find(boxString(target->id.str()));
                        if (it == d.end()) {
                            assertNameDefined(0, target->id.c_str(), NameError, false /* local_var_msg */);
                        }
                        d.erase(it);
                    } else if (frame_info.boxedLocals->cls == attrwrapper_cls) {
                        attrwrapperDel(frame_info.boxedLocals, target->id.str());
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
    static const std::string write_str("write");
    static const std::string newline_str("\n");
    static const std::string space_str(" ");

    STAT_TIMER(t0, "us_timer_visit_print");

    Box* dest = node->dest ? visit_expr(node->dest).o : getSysStdout();
    int nvals = node->values.size();
    assert(nvals <= 1 && "cfg should have lowered it to 0 or 1 values");
    for (int i = 0; i < nvals; i++) {
        Box* var = visit_expr(node->values[i]).o;

        // begin code for handling of softspace
        bool new_softspace = (i < nvals - 1) || (!node->nl);
        if (softspace(dest, new_softspace)) {
            callattrInternal(dest, write_str, CLASS_OR_INST, 0, ArgPassSpec(1), boxString(space_str), 0, 0, 0, 0);
        }

        Box* str_or_unicode_var = (var->cls == unicode_cls) ? var : str(var);
        callattrInternal(dest, write_str, CLASS_OR_INST, 0, ArgPassSpec(1), str_or_unicode_var, 0, 0, 0, 0);
    }

    if (node->nl) {
        callattrInternal(dest, write_str, CLASS_OR_INST, 0, ArgPassSpec(1), boxString(newline_str), 0, 0, 0, 0);
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

    exec(code, globals, locals);

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

    std::vector<const std::string*> keywords;
    for (AST_keyword* k : node->keywords) {
        keywords.push_back(&k->arg.str());
        args.push_back(visit_expr(k->value).o);
    }

    if (node->starargs)
        args.push_back(visit_expr(node->starargs).o);

    if (node->kwargs)
        args.push_back(visit_expr(node->kwargs).o);

    ArgPassSpec argspec(node->args.size(), node->keywords.size(), node->starargs, node->kwargs);



    if (is_callattr) {
        if (argspec != ArgPassSpec(0) && argspec != ArgPassSpec(1) && argspec != ArgPassSpec(2))
            abortTracing();

        if (args.size() > 2)
            abortTracing();


        CallattrFlags flags{.cls_only = callattr_clsonly, .null_on_nonexistent = false };

        if (istracing) {
            tracer->callattr(&attr.str(), flags, argspec);
        }

        return callattr(func.o, &attr.str(), flags, argspec,
                        args.size() > 0 ? args[0] : 0, args.size() > 1 ? args[1] : 0, args.size() > 2 ? args[2] : 0,
                        args.size() > 3 ? &args[3] : 0, &keywords);
    } else {
        if (argspec != ArgPassSpec(0) && argspec != ArgPassSpec(1) && argspec != ArgPassSpec(2) && argspec != ArgPassSpec(3))
            abortTracing();

        if (args.size() > 3)
            abortTracing();

        if (istracing) {
            tracer->runtimeCall(argspec);
        }
        return runtimeCall(func.o, argspec, args.size() > 0 ? args[0] : 0, args.size() > 1 ? args[1] : 0,
                           args.size() > 2 ? args[2] : 0, args.size() > 3 ? &args[3] : 0, &keywords);
    }
}


Value ASTInterpreter::visit_expr(AST_Expr* node) {
    Value rtn = visit_expr(node->value);
    if (istracing)
        tracer->pop();
    return rtn;
}

Value ASTInterpreter::visit_num(AST_Num* node) {
    if (node->num_type == AST_Num::INT) {
        if (istracing) {
            tracer->numInt(node->n_int);
        }
        return boxInt(node->n_int);
    } else if (node->num_type == AST_Num::FLOAT) {
        if (istracing) {
            tracer->numFloat(node->n_float);
        }
        return boxFloat(node->n_float);
}    else if (node->num_type == AST_Num::LONG) {
        abortTracing();
        return createLong(&node->n_long);
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
    abortTracing();
    RELEASE_ASSERT(node->keys.size() == node->values.size(), "not implemented");
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
            tracer->push((uint64_t)source_info->parent_module->getStringConstant(node->str_data));
        return source_info->parent_module->getStringConstant(node->str_data);
    } else if (node->str_type == AST_Str::UNICODE) {
        abortTracing();
        return decodeUTF8StringPtr(&node->str_data);
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
            if (tracer && istracing) {
                tracer->getGlobal(globals, &node->id.str());
            }

            return getGlobal(globals, &node->id.str());
        case ScopeInfo::VarScopeType::DEREF: {
            abortTracing();
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
            if (tracer && istracing) {
                tracer->getLocal(node->id);
            }

            SymMap::iterator it = sym_table.find(node->id);
            if (it != sym_table.end())
                return sym_table.getMapped(it->second);

            assertNameDefined(0, node->id.c_str(), UnboundLocalError, true);
            return Value();
        }
        case ScopeInfo::VarScopeType::NAME: {
            abortTracing();
            return boxedLocalsGet(frame_info.boxedLocals, node->id.c_str(), globals);
        }
        default:
            abort();
    }
}


Value ASTInterpreter::visit_subscript(AST_Subscript* node) {
    abortTracing();
    Value value = visit_expr(node->value);
    Value slice = visit_expr(node->slice);
    return getitem(value.o, slice.o);
}

Value ASTInterpreter::visit_list(AST_List* node) {
    abortTracing();
    BoxedList* list = new BoxedList;
    list->ensure(node->elts.size());
    for (AST_expr* e : node->elts)
        listAppendInternal(list, visit_expr(e).o);
    return list;
}

Value ASTInterpreter::visit_tuple(AST_Tuple* node) {
    abortTracing();
    BoxedTuple* rtn = BoxedTuple::create(node->elts.size());
    int rtn_idx = 0;
    for (AST_expr* e : node->elts)
        rtn->elts[rtn_idx++] = visit_expr(e).o;
    return rtn;
}

Value ASTInterpreter::visit_attribute(AST_Attribute* node) {
    Value val = visit_expr(node->value);
    if (istracing)
        tracer->getAttr(node->attr.c_str());
    return getattr(val.o, node->attr.c_str());
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
    ASTInterpreter interpreter(cf);

    ScopeInfo* scope_info = cf->clfunc->source->getScopeInfo();
    SourceInfo* source_info = cf->clfunc->source.get();
    if (unlikely(scope_info->usesNameLookup())) {
        interpreter.setBoxedLocals(new BoxedDict());
    }

    assert((!globals) == cf->clfunc->source->scoping->areGlobalsFromModule());
    if (globals) {
        interpreter.setGlobals(globals);
    } else {
        interpreter.setGlobals(source_info->parent_module);
    }

    interpreter.initArguments(nargs, (BoxedClosure*)closure, (BoxedGenerator*)generator, arg1, arg2, arg3, args);
    Value v = ASTInterpreter::execute(interpreter);

    return v.o ? v.o : None;
}

Box* astInterpretFunctionEval(CompiledFunction* cf, Box* globals, Box* boxedLocals) {
    ++cf->times_called;

    ASTInterpreter interpreter(cf);
    interpreter.initArguments(0, NULL, NULL, NULL, NULL, NULL, NULL);
    interpreter.setBoxedLocals(boxedLocals);

    ScopeInfo* scope_info = cf->clfunc->source->getScopeInfo();
    SourceInfo* source_info = cf->clfunc->source.get();

    assert(!cf->clfunc->source->scoping->areGlobalsFromModule());
    assert(globals);
    interpreter.setGlobals(globals);

    Value v = ASTInterpreter::execute(interpreter);

    return v.o ? v.o : None;
}

Box* astInterpretFrom(CompiledFunction* cf, AST_expr* after_expr, AST_stmt* enclosing_stmt, Box* expr_val,
                      FrameStackState frame_state) {
    assert(cf);
    assert(enclosing_stmt);
    assert(frame_state.locals);
    assert(after_expr);
    assert(expr_val);

    ASTInterpreter interpreter(cf);

    ScopeInfo* scope_info = cf->clfunc->source->getScopeInfo();
    SourceInfo* source_info = cf->clfunc->source.get();
    assert(cf->clfunc->source->scoping->areGlobalsFromModule());
    interpreter.setGlobals(source_info->parent_module);

    for (const auto& p : frame_state.locals->d) {
        assert(p.first->cls == str_cls);
        auto name = static_cast<BoxedString*>(p.first)->s();
        if (name == PASSED_GENERATOR_NAME) {
            interpreter.setGenerator(p.second);
        } else if (name == PASSED_CLOSURE_NAME) {
            interpreter.setPassedClosure(p.second);
        } else if (name == CREATED_CLOSURE_NAME) {
            interpreter.setCreatedClosure(p.second);
        } else {
            InternedString interned = cf->clfunc->source->getInternedStrings().get(name);
            interpreter.addSymbol(interned, p.second, false);
        }
    }

    interpreter.setFrameInfo(frame_state.frame_info);

    CFGBlock* start_block = NULL;
    AST_stmt* starting_statement = NULL;
    while (true) {
        if (enclosing_stmt->type == AST_TYPE::Assign) {
            auto asgn = ast_cast<AST_Assign>(enclosing_stmt);
            assert(asgn->value == after_expr);
            assert(asgn->targets.size() == 1);
            assert(asgn->targets[0]->type == AST_TYPE::Name);
            auto name = ast_cast<AST_Name>(asgn->targets[0]);
            assert(name->id.str()[0] == '#');
            interpreter.addSymbol(name->id, expr_val, true);
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

    Value v = ASTInterpreter::execute(interpreter, start_block, starting_statement);

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
        if (only_user_visible && (l.first.str()[0] == '!' || l.first.str()[0] == '#'))
            continue;

        rtn->d[boxString(l.first.str())] = interpreter->getSymbolTable().getMapped(l.second);
    }

    return rtn;
}

BoxedClosure* passedClosureForInterpretedFrame(void* frame_ptr) {
    ASTInterpreter* interpreter = s_interpreterMap[frame_ptr];
    assert(interpreter);
    return interpreter->getPassedClosure();
}

void gatherInterpreterRoots(GCVisitor* visitor) {
    for (const auto& p : s_interpreterMap) {
        p.second->gcVisit(visitor);
    }
}
}
