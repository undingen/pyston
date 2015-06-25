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
#include "asm_writing/icinfo.h"
#include "asm_writing/rewriter.h"
#include "codegen/baseline_jit.h"
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

#define BASELINEJIT_THR 20

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
    Value doBinOp(Value left, Value right, int op, BinExpType exp_type);
    void doStore(AST_expr* node, Value value);
    void doStore(InternedString name, Value value);
    Box* doOSR(AST_Jump* node);
    Value getNone();

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
    friend struct pyston::ASTInterpreterJitInterface;

    std::unique_ptr<JitFragment> tracer;

    void abortTracing();
    void startTracing(CFGBlock* block, int jump_offset = 0);

    LivenessAnalysis* getLiveness() {
        if (!source_info->liveness_info)
            source_info->liveness_info = computeLivenessInfo(source_info->cfg);
        return source_info->liveness_info.get();
    }
};

void ASTInterpreter::abortTracing() {
    if (tracer) {
        tracer->abortTrace();
        tracer.reset();
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
      globals(0),
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
        doStore(source_info->getInternedStrings().get(name), Value(argsArray[i++], 0));
    }

    if (!param_names.vararg.str().empty()) {
        doStore(source_info->getInternedStrings().get(param_names.vararg), Value(argsArray[i++], 0));
    }

    if (!param_names.kwarg.str().empty()) {
        doStore(source_info->getInternedStrings().get(param_names.kwarg), Value(argsArray[i++], 0));
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

void ASTInterpreter::startTracing(CFGBlock* block, int jump_offset) {
#if ENABLE_TRACING
    if (!compiled_func->jitted_code)
        compiled_func->jitted_code = (void*)(new JitedCode(source_info->getName()));
    assert(!tracer);
    JitedCode* jitted_code = (JitedCode*)compiled_func->jitted_code;
    tracer = jitted_code->newFragment(block, jump_offset);
#endif
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

        if (ENABLE_TRACING_FUNC && interpreter.compiled_func->times_called == REOPT_THRESHOLD_INTERPRETER
            && !start_block->code)
            trace = true;
    }

    // Important that this happens after RegisterHelper:
    interpreter.current_inst = start_at;
    threading::allowGLReadPreemption();
    interpreter.current_inst = NULL;

    interpreter.current_block = start_block;

    bool started = false;
    if (trace && from_start)
        interpreter.startTracing(start_block);

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
    } else
        interpreter.next_block = interpreter.current_block;

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
                        return Value(rtn.second, 0);
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
        if (was_tracing && !interpreter.tracer) {
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
    UNAVOIDABLE_STAT_TIMER(t0, "us_timer_in_interpreter");

    RegisterHelper frame_registerer;
    return executeInner(interpreter, start_block, start_at, &frame_registerer);
}

Value ASTInterpreter::doBinOp(Value left, Value right, int op, BinExpType exp_type) {
    switch (exp_type) {
        case BinExpType::AugBinOp:
            return Value(augbinop(left.o, right.o, op), tracer ? tracer->emitAugbinop(left, right, op) : NULL);
        case BinExpType::BinOp:
            return Value(binop(left.o, right.o, op), tracer ? tracer->emitBinop(left, right, op) : NULL);
        case BinExpType::Compare:
            return Value(compare(left.o, right.o, op), tracer ? tracer->emitCompare(left, right, op) : NULL);
        default:
            RELEASE_ASSERT(0, "not implemented");
    }
    return Value();
}

void ASTInterpreter::doStore(InternedString name, Value value) {
    ScopeInfo::VarScopeType vst = scope_info->getScopeTypeOfName(name);
    if (vst == ScopeInfo::VarScopeType::GLOBAL) {
        if (tracer)
            tracer->emitSetGlobal(globals, name.getBox(), value);
        setGlobal(globals, name.getBox(), value.o);
    } else if (vst == ScopeInfo::VarScopeType::NAME) {
        if (tracer)
            tracer->emitSetItemName(name.getBox(), value);

        assert(frame_info.boxedLocals != NULL);
        // TODO should probably pre-box the names when it's a scope that usesNameLookup
        setitem(frame_info.boxedLocals, name.getBox(), value.o);
    } else {
        bool closure = vst == ScopeInfo::VarScopeType::CLOSURE;
        if (tracer) {
            if (!closure) {
                bool is_live = getLiveness()->isLiveAtEnd(name, current_block);
                if (is_live)
                    tracer->emitSetLocal(name, closure, value);
                else
                    tracer->emitSetDeadLocal(name, value);
            } else
                tracer->emitSetLocal(name, closure, value);
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
            tracer->emitSetAttr(o, attr->attr.getBox(), value);
        pyston::setattr(o.o, attr->attr.getBox(), value.o);
    } else if (node->type == AST_TYPE::Tuple) {
        AST_Tuple* tuple = (AST_Tuple*)node;
        Box** array = unpackIntoArray(value.o, tuple->elts.size());

        RewriterVar* array_var = nullptr;
        if (tracer)
            array_var = tracer->emitUnpackIntoArray(value, tuple->elts.size());

        unsigned i = 0;
        for (AST_expr* e : tuple->elts) {
            doStore(e, Value(array[i], tracer ? array_var->getAttr(i * 8) : NULL));
            ++i;
        }
    } else if (node->type == AST_TYPE::List) {
        AST_List* list = (AST_List*)node;
        Box** array = unpackIntoArray(value.o, list->elts.size());

        RewriterVar* array_var = nullptr;
        if (tracer)
            array_var = tracer->emitUnpackIntoArray(value, list->elts.size());

        unsigned i = 0;
        for (AST_expr* e : list->elts) {
            doStore(e, Value(array[i], tracer ? array_var->getAttr(i * 8) : NULL));
            ++i;
        }
    } else if (node->type == AST_TYPE::Subscript) {
        AST_Subscript* subscript = (AST_Subscript*)node;

        Value target = visit_expr(subscript->value);
        Value slice = visit_expr(subscript->slice);

        if (tracer)
            tracer->emitSetItem(target, slice, value);

        setitem(target.o, slice.o, value.o);
    } else {
        RELEASE_ASSERT(0, "not implemented");
    }
}

Value ASTInterpreter::getNone() {
    return Value(None, tracer ? tracer->Imm((void*)None) : NULL);
}

Value ASTInterpreter::visit_unaryop(AST_UnaryOp* node) {
    Value operand = visit_expr(node->operand);
    if (node->op_type == AST_TYPE::Not) {
        Value v;
        if (tracer)
            v.var = tracer->emitNotNonzero(operand);

        v.o = boxBool(!nonzero(operand.o));
        return v;
    } else {
        Value v;
        if (tracer)
            v.var = tracer->emitUnaryop(operand, node->op_type);

        v.o = unaryop(operand.o, node->op_type);
        return v;
    }
}

Value ASTInterpreter::visit_binop(AST_BinOp* node) {
    Value left = visit_expr(node->left);
    Value right = visit_expr(node->right);
    return doBinOp(left, right, node->op_type, BinExpType::BinOp);
}

Value ASTInterpreter::visit_slice(AST_Slice* node) {
    Value lower = node->lower ? visit_expr(node->lower) : getNone();
    Value upper = node->upper ? visit_expr(node->upper) : getNone();
    Value step = node->step ? visit_expr(node->step) : getNone();

    Value v;
    if (tracer)
        v.var = tracer->emitCreateSlice(lower, upper, step);

    v.o = createSlice(lower.o, upper.o, step.o);
    return v;
}

Value ASTInterpreter::visit_extslice(AST_ExtSlice* node) {
    abortTracing();
    int num_slices = node->dims.size();
    BoxedTuple* rtn = BoxedTuple::create(num_slices);
    for (int i = 0; i < num_slices; ++i)
        rtn->elts[i] = visit_expr(node->dims[i]).o;
    return Value(rtn, 0);
}

Value ASTInterpreter::visit_branch(AST_Branch* node) {
    Value v = visit_expr(node->test);
    ASSERT(v.o == True || v.o == False, "Should have called NONZERO before this branch");

    if (tracer)
        tracer->addGuard(v, v.o == True ? node->iffalse : node->iftrue);
    if (v.o == True)
        next_block = node->iftrue;
    else
        next_block = node->iffalse;


    if (tracer) {
        tracer->emitJump(next_block);
        int jump_offset = tracer->compile();
        tracer.reset();
        if (!next_block->code)
            startTracing(next_block, jump_offset);
    }

    return Value();
}

Value ASTInterpreter::visit_jump(AST_Jump* node) {
    bool backedge = node->target->idx < current_block->idx && compiled_func;
    if (backedge) {
        threading::allowGLReadPreemption();

        if (tracer)
            tracer->call(false, (void*)threading::allowGLReadPreemption);
    }

    if (tracer) {
        if (backedge)
            tracer->emitOSRPoint(node);
        tracer->emitJump(node->target);
        int jump_offset = tracer->compile();
        tracer.reset();
        if (!node->target->code)
            startTracing(node->target, jump_offset);
    }

    if (backedge)
        ++edgecount;

    if (ENABLE_TRACING && backedge && edgecount == OSR_THRESHOLD_INTERPRETER && !tracer && !node->target->code)
        startTracing(node->target);

    if (backedge && edgecount == OSR_THRESHOLD_BASELINE) {
        Box* rtn = doOSR(node);
        if (rtn)
            return Value(rtn, nullptr);
    }

    next_block = node->target;
    return Value();
}

Box* ASTInterpreter::doOSR(AST_Jump* node) {
    bool can_osr = ENABLE_OSR && !FORCE_INTERPRETER && source_info->scoping->areGlobalsFromModule();
    if (!can_osr)
        return nullptr;

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
        return nullptr;
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

    UNAVOIDABLE_STAT_TIMER(t0, "us_timer_in_jitted_code");
    CompiledFunction* partial_func = compilePartialFuncInternal(&exit);
    auto arg_tuple = getTupleFromArgsArray(&arg_array[0], arg_array.size());
    Box* r = partial_func->call(std::get<0>(arg_tuple), std::get<1>(arg_tuple), std::get<2>(arg_tuple),
                                std::get<3>(arg_tuple));

    // This is one of the few times that we are allowed to have an invalid value in a Box* Value.
    // Check for it, and return as an int so that we don't trigger a potential assert when
    // creating the Value.
    if (compiled_func->getReturnType() != VOID)
        assert(r);

    return r ? r : None;
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
    Value obj = visit_expr(node->value);
    BoxedString* attr = node->attr.getBox();
    return Value(getclsattr(obj.o, attr), tracer ? tracer->emitGetClsAttr(obj, attr) : nullptr);
}

Value ASTInterpreter::visit_augBinOp(AST_AugBinOp* node) {
    assert(node->op_type != AST_TYPE::Is && node->op_type != AST_TYPE::IsNot && "not tested yet");

    Value left = visit_expr(node->left);
    Value right = visit_expr(node->right);
    return doBinOp(left, right, node->op_type, BinExpType::AugBinOp);
}

Value ASTInterpreter::visit_langPrimitive(AST_LangPrimitive* node) {
    Value v;
    if (node->opcode == AST_LangPrimitive::GET_ITER) {
        assert(node->args.size() == 1);

        Value val = visit_expr(node->args[0]);
        v = Value(getPystonIter(val.o), tracer ? tracer->emitGetPystonIter(val) : NULL);
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
        v.o = importFrom(module.o, boxString(name));
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
        v.o = import(level, froms.o, module_name);
    } else if (node->opcode == AST_LangPrimitive::IMPORT_STAR) {
        abortTracing();
        assert(node->args.size() == 1);
        assert(node->args[0]->type == AST_TYPE::Name);

        RELEASE_ASSERT(source_info->ast->type == AST_TYPE::Module || source_info->ast->type == AST_TYPE::Suite,
                       "import * not supported in functions");

        Value module = visit_expr(node->args[0]);

        v.o = importStar(module.o, globals);
    } else if (node->opcode == AST_LangPrimitive::NONE) {
        v = getNone();
    } else if (node->opcode == AST_LangPrimitive::LANDINGPAD) {
        assert(last_exception.type);
        Box* type = last_exception.type;
        Box* value = last_exception.value ? last_exception.value : None;
        Box* traceback = last_exception.traceback ? last_exception.traceback : None;
        v = Value(BoxedTuple::create({ type, value, traceback }), tracer ? tracer->emitLandingpad() : nullptr);
        last_exception = ExcInfo(NULL, NULL, NULL);
    } else if (node->opcode == AST_LangPrimitive::CHECK_EXC_MATCH) {
        assert(node->args.size() == 2);
        Value obj = visit_expr(node->args[0]);
        Value cls = visit_expr(node->args[1]);
        v = Value(boxBool(exceptionMatches(obj.o, cls.o)), tracer ? tracer->emitExceptionMatches(obj, cls) : nullptr);
    } else if (node->opcode == AST_LangPrimitive::LOCALS) {
        abortTracing();
        assert(frame_info.boxedLocals != NULL);
        v.o = frame_info.boxedLocals;
    } else if (node->opcode == AST_LangPrimitive::NONZERO) {
        assert(node->args.size() == 1);
        Value obj = visit_expr(node->args[0]);
        v = Value(boxBool(nonzero(obj.o)), tracer ? tracer->emitNonzero(obj) : NULL);
    } else if (node->opcode == AST_LangPrimitive::SET_EXC_INFO) {
        assert(node->args.size() == 3);

        Value type = visit_expr(node->args[0]);
        assert(type.o);
        Value value = visit_expr(node->args[1]);
        assert(value.o);
        Value traceback = visit_expr(node->args[2]);
        assert(traceback.o);

        if (tracer)
            tracer->emitSetExcInfo(type, value, traceback);

        getFrameInfo()->exc = ExcInfo(type.o, value.o, traceback.o);
        v = getNone();
    } else if (node->opcode == AST_LangPrimitive::UNCACHE_EXC_INFO) {
        assert(node->args.empty());
        if (tracer)
            tracer->emitUncacheExcInfo();

        getFrameInfo()->exc = ExcInfo(NULL, NULL, NULL);
        v = getNone();
    } else if (node->opcode == AST_LangPrimitive::HASNEXT) {
        assert(node->args.size() == 1);
        Value obj = visit_expr(node->args[0]);
        v = Value(boxBool(hasnext(obj.o)), tracer ? tracer->emitHasnext(obj) : NULL);
    } else
        RELEASE_ASSERT(0, "unknown opcode %d", node->opcode);
    return v;
}

Value ASTInterpreter::visit_yield(AST_Yield* node) {
    Value value = node->value ? visit_expr(node->value) : getNone();
    assert(generator && generator->cls == generator_cls);

    return Value(yield(generator, value.o), tracer ? tracer->emitYield(value) : NULL);
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
    Value s = node->value ? visit_expr(node->value) : getNone();

    if (tracer) {
        tracer->emitReturn(s);
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

    return Value(func, NULL);
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

    return Value(classobj, NULL);
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
    Value left = visit_expr(node->left);
    Value right = visit_expr(node->comparators[0]);
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


    std::vector<Value, StlCompatAllocator<Value>> argsValues;
    for (AST_expr* e : node->args)
        argsValues.push_back(visit_expr(e));

    std::vector<BoxedString*>* keyword_names = NULL;
    if (node->keywords.size())
        keyword_names = getKeywordNameStorage(node);

    for (AST_keyword* k : node->keywords)
        argsValues.push_back(visit_expr(k->value));

    if (node->starargs)
        argsValues.push_back(visit_expr(node->starargs));

    if (node->kwargs)
        argsValues.push_back(visit_expr(node->kwargs));

    std::vector<Box*, StlCompatAllocator<Box*>> args;
    for (Value& v : argsValues)
        args.push_back(v.o);

    ArgPassSpec argspec(node->args.size(), node->keywords.size(), node->starargs, node->kwargs);

    if (is_callattr) {
        CallattrFlags flags{.cls_only = callattr_clsonly, .null_on_nonexistent = false };

        if (tracer)
            v.var = tracer->emitCallattr(func, attr.getBox(), flags, argspec, argsValues, keyword_names);

        v.o = callattr(func.o, attr.getBox(),
                       CallattrFlags({.cls_only = callattr_clsonly, .null_on_nonexistent = false }), argspec,
                       args.size() > 0 ? args[0] : 0, args.size() > 1 ? args[1] : 0, args.size() > 2 ? args[2] : 0,
                       args.size() > 3 ? &args[3] : 0, keyword_names);
        return v;
    } else {
        Value v;

        if (tracer)
            v.var = tracer->emitRuntimeCall(func, argspec, argsValues, keyword_names);

        v.o = runtimeCall(func.o, argspec, args.size() > 0 ? args[0] : 0, args.size() > 1 ? args[1] : 0,
                          args.size() > 2 ? args[2] : 0, args.size() > 3 ? &args[3] : 0, keyword_names);
        return v;
    }
}


Value ASTInterpreter::visit_expr(AST_Expr* node) {
    Value rtn = visit_expr(node->value);
    return rtn;
}

Value ASTInterpreter::visit_num(AST_Num* node) {
    if (node->num_type == AST_Num::INT) {
        return Value(boxInt(node->n_int), tracer ? tracer->emitInt(node->n_int) : NULL);
    } else if (node->num_type == AST_Num::FLOAT) {
        return Value(boxFloat(node->n_float), tracer ? tracer->emitFloat(node->n_float) : NULL);
    } else if (node->num_type == AST_Num::LONG) {
        return Value(createLong(node->n_long), tracer ? tracer->emitLong(node->n_long) : NULL);
    } else if (node->num_type == AST_Num::COMPLEX) {
        abortTracing();
        return Value(boxComplex(0.0, node->n_float), NULL);
    }
    RELEASE_ASSERT(0, "not implemented");
    return Value();
}

Value ASTInterpreter::visit_index(AST_Index* node) {
    return visit_expr(node->value);
}

Value ASTInterpreter::visit_repr(AST_Repr* node) {
    abortTracing();
    return Value(repr(visit_expr(node->value).o), 0);
}

Value ASTInterpreter::visit_lambda(AST_Lambda* node) {
    abortTracing();
    AST_Return* expr = new AST_Return();
    expr->value = node->body;

    std::vector<AST_stmt*> body = { expr };
    return Value(createFunction(node, node->args, body), NULL);
}

Value ASTInterpreter::visit_dict(AST_Dict* node) {
    RELEASE_ASSERT(node->keys.size() == node->values.size(), "not implemented");
    Value v;
    if (tracer) {
        if (node->keys.size())
            abortTracing();
        else
            v.var = tracer->emitCreateDict();
    }

    BoxedDict* dict = new BoxedDict();
    for (size_t i = 0; i < node->keys.size(); ++i) {
        Box* v = visit_expr(node->values[i]).o;
        Box* k = visit_expr(node->keys[i]).o;
        dict->d[k] = v;
    }
    v.o = dict;
    return v;
}

Value ASTInterpreter::visit_set(AST_Set* node) {
    abortTracing();
    BoxedSet::Set set;
    for (AST_expr* e : node->elts)
        set.insert(visit_expr(e).o);

    return Value(new BoxedSet(std::move(set)), NULL);
}

Value ASTInterpreter::visit_str(AST_Str* node) {
    if (node->str_type == AST_Str::STR) {
        // if (tracer)
        //    tracer->emitPush((uint64_t)source_info->parent_module->getStringConstant(node->str_data));
        Value v;
        v.o = source_info->parent_module->getStringConstant(node->str_data);
        if (tracer)
            v.var = tracer->Imm((void*)v.o);
        return v;
    } else if (node->str_type == AST_Str::UNICODE) {
        return Value(decodeUTF8StringPtr(node->str_data), tracer ? tracer->emitUnicodeStr(node->str_data) : NULL);
    } else {
        RELEASE_ASSERT(0, "%d", node->str_type);
    }
}

Value ASTInterpreter::visit_name(AST_Name* node) {
    if (node->lookup_type == ScopeInfo::VarScopeType::UNKNOWN) {
        node->lookup_type = scope_info->getScopeTypeOfName(node->id);
    }

    switch (node->lookup_type) {
        case ScopeInfo::VarScopeType::GLOBAL: {
            Value v;
            if (tracer)
                v.var = tracer->emitGetGlobal(globals, node->id.getBox());

            v.o = getGlobal(globals, node->id.getBox());
            return v;
        }
        case ScopeInfo::VarScopeType::DEREF: {
            Value v;
            if (tracer)
                v.var = tracer->emitDeref(node->id);

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
            v.o = val;
            return v;
        }
        case ScopeInfo::VarScopeType::FAST:
        case ScopeInfo::VarScopeType::CLOSURE: {
            Value v;
            if (tracer) {
                bool dead = false;
                if (node->lookup_type == ScopeInfo::VarScopeType::FAST)
                    dead = !getLiveness()->isLiveAtEnd(node->id, current_block);

                if (dead)
                    v.var = tracer->emitGetDeadLocal(node->id);
                else
                    v.var = tracer->emitGetLocal(node->id);
            }

            SymMap::iterator it = sym_table.find(node->id);
            if (it != sym_table.end()) {
                v.o = sym_table.getMapped(it->second);
                return v;
            }

            assertNameDefined(0, node->id.c_str(), UnboundLocalError, true);
            RELEASE_ASSERT(0, "unreachable");
            return Value();
        }
        case ScopeInfo::VarScopeType::NAME: {
            Value v;
            if (tracer)
                v.var = tracer->emitBoxedLocalsGet(node->id.getBox());
            v.o = boxedLocalsGet(frame_info.boxedLocals, node->id.getBox(), globals);
            return v;
        }
        default:
            abort();
    }
}

Value ASTInterpreter::visit_subscript(AST_Subscript* node) {
    Value value = visit_expr(node->value);
    Value slice = visit_expr(node->slice);

    return Value(getitem(value.o, slice.o), tracer ? tracer->emitGetItem(value, slice) : NULL);
}

Value ASTInterpreter::visit_list(AST_List* node) {
    llvm::SmallVector<Value, 8> items;

    BoxedList* list = new BoxedList;
    list->ensure(node->elts.size());
    for (AST_expr* e : node->elts) {
        Value v = visit_expr(e);
        items.push_back(v);
        listAppendInternal(list, v.o);
    }

    return Value(list, tracer ? tracer->emitCreateList(items) : NULL);
}

Value ASTInterpreter::visit_tuple(AST_Tuple* node) {
    llvm::SmallVector<Value, 8> items;

    BoxedTuple* rtn = BoxedTuple::create(node->elts.size());
    int rtn_idx = 0;
    for (AST_expr* e : node->elts) {
        Value v = visit_expr(e);
        rtn->elts[rtn_idx++] = v.o;
        items.push_back(v);
    }

    return Value(rtn, tracer ? tracer->emitCreateTuple(items) : NULL);
}

Value ASTInterpreter::visit_attribute(AST_Attribute* node) {
    Value v = visit_expr(node->value);
    return Value(pyston::getattr(v.o, node->attr.getBox()),
                 tracer ? tracer->emitGetAttr(v, node->attr.getBox()) : NULL);
}
}

int ASTInterpreterJitInterface::getCurrentInstOffset() {
    return offsetof(ASTInterpreter, current_inst);
}

int ASTInterpreterJitInterface::getCurrentBlockOffset() {
    return offsetof(ASTInterpreter, current_block);
}

Box* ASTInterpreterJitInterface::doOSRHelper(void* _interpreter, AST_Jump* node) {
    ASTInterpreter* interpreter = (ASTInterpreter*)_interpreter;
    ++interpreter->edgecount;
    if (interpreter->edgecount >= OSR_THRESHOLD_BASELINE)
        return interpreter->doOSR(node);
    return nullptr;
}

Box* ASTInterpreterJitInterface::tracerHelperGetLocal(void* _interpreter, InternedString id) {
    ASTInterpreter* interpreter = (ASTInterpreter*)_interpreter;

    auto it = interpreter->sym_table.find(id);
    if (it != interpreter->sym_table.end()) {
        Box* v = interpreter->sym_table.getMapped(it->second);
        assert(gc::isValidGCObject(v));
        return v;
    }

    assertNameDefined(0, id.c_str(), UnboundLocalError, true);
    return 0;
}

void ASTInterpreterJitInterface::tracerHelperSetLocal(void* _interpreter, InternedString id, Box* v, bool set_closure) {
    ASTInterpreter* interpreter = (ASTInterpreter*)_interpreter;

    assert(gc::isValidGCObject(v));
    interpreter->sym_table[id] = v;

    if (set_closure) {
        interpreter->created_closure->elts[interpreter->scope_info->getClosureOffset(id)] = v;
    }
}

Box* ASTInterpreterJitInterface::boxedLocalsGetHelper(void* _interpreter, BoxedString* s) {
    ASTInterpreter* interpreter = (ASTInterpreter*)_interpreter;
    return boxedLocalsGet(interpreter->frame_info.boxedLocals, s, interpreter->globals);
}

void ASTInterpreterJitInterface::setItemNameHelper(void* _interpreter, Box* str, Box* val) {
    ASTInterpreter* interpreter = (ASTInterpreter*)_interpreter;
    assert(interpreter->frame_info.boxedLocals != NULL);
    setitem(interpreter->frame_info.boxedLocals, str, val);
}

Box* ASTInterpreterJitInterface::derefHelper(void* _interpreter, InternedString s) {
    ASTInterpreter* interpreter = (ASTInterpreter*)_interpreter;
    DerefInfo deref_info = interpreter->scope_info->getDerefInfo(s);
    assert(interpreter->passed_closure);
    BoxedClosure* closure = interpreter->passed_closure;
    for (int i = 0; i < deref_info.num_parents_from_passed_closure; i++) {
        closure = closure->parent;
    }
    Box* val = closure->elts[deref_info.offset];
    if (val == NULL) {
        raiseExcHelper(NameError, "free variable '%s' referenced before assignment in enclosing scope", s.c_str());
    }
    return val;
}

Box* ASTInterpreterJitInterface::yieldHelper(void* _interpreter, Box* val) {
    ASTInterpreter* interpreter = (ASTInterpreter*)_interpreter;
    return yield(interpreter->generator, val);
}

Box* ASTInterpreterJitInterface::uncacheExcInfoHelper(void* _interpreter) {
    ASTInterpreter* interpreter = (ASTInterpreter*)_interpreter;
    interpreter->getFrameInfo()->exc = ExcInfo(NULL, NULL, NULL);
    return None;
}

Box* ASTInterpreterJitInterface::setExcInfoHelper(void* _interpreter, Box* type, Box* value, Box* traceback) {
    ASTInterpreter* interpreter = (ASTInterpreter*)_interpreter;
    interpreter->getFrameInfo()->exc = ExcInfo(type, value, traceback);
    return None;
}

Box* ASTInterpreterJitInterface::landingpadHelper(void* _interpreter) {
    ASTInterpreter* interpreter = (ASTInterpreter*)_interpreter;
    auto& last_exception = interpreter->last_exception;
    Box* type = last_exception.type;
    Box* value = last_exception.value ? last_exception.value : None;
    Box* traceback = last_exception.traceback ? last_exception.traceback : None;
    Box* rtn = BoxedTuple::create({ type, value, traceback });
    last_exception = ExcInfo(NULL, NULL, NULL);
    return rtn;
}

const void* interpreter_instr_addr = (void*)&ASTInterpreter::executeInner;

Box* astInterpretFunction(CompiledFunction* cf, int nargs, Box* closure, Box* generator, Box* globals, Box* arg1,
                          Box* arg2, Box* arg3, Box** args) {
    UNAVOIDABLE_STAT_TIMER(t0, "us_timer_in_interpreter");

    assert((!globals) == cf->clfunc->source->scoping->areGlobalsFromModule());
    bool can_reopt = ENABLE_REOPT && !FORCE_INTERPRETER && (globals == NULL);
    int num_blocks = cf->clfunc->source->cfg->blocks.size();
    int threshold = num_blocks <= 20 ? (REOPT_THRESHOLD_BASELINE / 3) : REOPT_THRESHOLD_BASELINE;
    if (unlikely(can_reopt && cf->times_called > threshold)) {
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
