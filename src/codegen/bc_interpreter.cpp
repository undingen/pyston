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

#include "codegen/bc_interpreter.h"

#include "analysis/scoping_analysis.h"
#include "codegen/bc_generator.h"
#include "codegen/irgen/irgenerator.h"
#include "core/ast.h"
#include "runtime/inline/boxing.h"
#include "runtime/long.h"
#include "runtime/types.h"


namespace pyston {

namespace {

class BCInterpreter {
public:
    BCInterpreter(const BCFunction& bc_function, CompiledFunction* cf);

    ~BCInterpreter() { delete vregs; }

    void initArguments(int nargs, BoxedClosure* closure, BoxedGenerator* generator, Box* arg1, Box* arg2, Box* arg3,
                       Box** args);

    Box* run();

private:
    Box* createConst(ConstPoolIndex index);
    std::string getStrConst(ConstPoolIndex index);

    void execute_binop(InstructionO8RRR* inst);
    void execute_createFunction(InstructionRC* inst);
    void execute_runtimeCall(InstructionV* inst);
    void execute_print(InstructionV* inst);

    void dump_vregs();

private:
    const BCFunction& bc_function;
    Box** vregs;
    SourceInfo* source_info;
};


BCInterpreter::BCInterpreter(const BCFunction& bc_function, CompiledFunction* cf)
    : bc_function(bc_function), vregs(new Box* [bc_function.num_regs]), source_info(cf->clfunc->source) {
    memset(vregs, 0, sizeof(Box*) * bc_function.num_regs);
}

void BCInterpreter::initArguments(int nargs, BoxedClosure* closure, BoxedGenerator* generator, Box* arg1, Box* arg2,
                                  Box* arg3, Box** args) {
    if (nargs >= 1) {
        vregs[0] = arg1;
        if (nargs >= 2) {
            vregs[1] = arg2;
            if (nargs >= 3)
                vregs[2] = arg3;
        }
    }
}

void BCInterpreter::dump_vregs() {
    printf("reg dump:\n");
    for (int i = 0; i < bc_function.num_regs; ++i) {
        printf("  %d %p ", i, vregs[i]);
        if (vregs[i])
            printf("%s", repr(vregs[i])->s.c_str());
        printf("\n");
    }
    printf("\n");
}

Box* BCInterpreter::createConst(ConstPoolIndex index) {
    Constant c = bc_function.const_pool[index];
    if (c.getType() == Constant::Type::Num) {
        AST_Num* node = c.num_value;
        if (node->num_type == AST_Num::INT)
            return boxInt(node->n_int);
        else if (node->num_type == AST_Num::FLOAT)
            return boxFloat(node->n_float);
        else if (node->num_type == AST_Num::LONG)
            return createLong(&node->n_long);
        else if (node->num_type == AST_Num::COMPLEX)
            return boxComplex(0.0, node->n_float);
        RELEASE_ASSERT(0, "not implemented");
    } else if (c.getType() == Constant::Type::String) {
        return boxString(c.string_value);
    }

    RELEASE_ASSERT(0, "not implemented");
}

std::string BCInterpreter::getStrConst(ConstPoolIndex index) {
    Constant c = bc_function.const_pool[index];
    RELEASE_ASSERT(c.getType() == Constant::Type::String, "not implemented");
    return c.string_value;
}

void BCInterpreter::execute_binop(InstructionO8RRR* inst) {
    vregs[inst->reg_dst] = binop(vregs[inst->reg_src1], vregs[inst->reg_src2], inst->other);
}

void BCInterpreter::execute_createFunction(InstructionRC* inst) {
    Constant c = bc_function.const_pool[inst->const_pool_index];
    RELEASE_ASSERT(c.getType() == Constant::Type::FunctionDef, "error");

    AST_FunctionDef* node = c.functionDef_value;
    AST_arguments* args = node->args;
    const std::vector<AST_stmt*>& body = node->body;


    // copy from ASTInterpreter::createFunction
    CLFunction* cl = wrapFunction(node, args, body, source_info);

    std::vector<Box*> defaults;
    for (AST_expr* d : args->defaults) {
        RELEASE_ASSERT(0, "not implemented");
        // defaults.push_back(visit_expr(d).o);
    }
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

    bool is_generator = cl->source->is_generator;

    BoxedClosure* closure = 0;
    if (takes_closure) {
        RELEASE_ASSERT(0, "not implemented");
        /*
        if (scope_info->createsClosure()) {
            closure = created_closure;
        } else {
            assert(scope_info->passesThroughClosure());
            closure = passed_closure;
        }
        assert(closure);*/
    }

    vregs[inst->reg_dst] = boxCLFunction(cl, closure, is_generator, u.il);
}

void BCInterpreter::execute_print(InstructionV* inst) {
    static const std::string write_str("write");
    static const std::string newline_str("\n");
    static const std::string space_str(" ");

    int nl = inst->reg[0];
    Box* dest = (uint16_t)inst->reg[1] != (uint16_t)-1 ? vregs[inst->reg[1]] : getSysStdout();
    int nvals = inst->num_args - 2;
    for (int i = 0; i < nvals; i++) {
        Box* var = vregs[inst->reg[i + 2]];

        // begin code for handling of softspace
        bool new_softspace = (i < nvals - 1) || (!nl);
        if (softspace(dest, new_softspace)) {
            callattrInternal(dest, &write_str, CLASS_OR_INST, 0, ArgPassSpec(1), boxString(space_str), 0, 0, 0, 0);
        }
        callattrInternal(dest, &write_str, CLASS_OR_INST, 0, ArgPassSpec(1), str(var), 0, 0, 0, 0);
    }

    if (nl) {
        callattrInternal(dest, &write_str, CLASS_OR_INST, 0, ArgPassSpec(1), boxString(newline_str), 0, 0, 0, 0);
        if (nvals == 0) {
            softspace(dest, false);
        }
    }
}

void BCInterpreter::execute_runtimeCall(InstructionV* inst) {
    int num_args = inst->num_args - 2;
    Box* obj = vregs[inst->reg[1]];
    ArgPassSpec arg_pass_spec(num_args);

    Box* arg1 = num_args > 0 ? vregs[inst->reg[2 + 0]] : nullptr;
    Box* arg2 = num_args > 1 ? vregs[inst->reg[2 + 1]] : nullptr;
    Box* arg3 = num_args > 2 ? vregs[inst->reg[2 + 2]] : nullptr;
    Box** args = nullptr;
    RELEASE_ASSERT(num_args <= 2, "not implmented");

    vregs[inst->reg[0]] = runtimeCall(obj, arg_pass_spec, arg1, arg2, arg3, args, {});
}

Box* BCInterpreter::run() {
    const unsigned char* bytecode_pc = &bc_function.bytecode[0];
    BoxedModule* parent_module = source_info->parent_module;

    Box** reg = vregs;

    while (true) {
        Instruction* _inst = (Instruction*)bytecode_pc;

        switch (_inst->op) {
            case BCOp::LoadConst: {
                InstructionRC* inst = (InstructionRC*)_inst;
                reg[inst->reg_dst] = createConst(inst->const_pool_index);
                bytecode_pc += inst->sizeInBytes();
                break;
            }
            case BCOp::Store: {
                InstructionRR* inst = (InstructionRR*)_inst;
                reg[inst->reg_dst] = reg[inst->reg_src];
                bytecode_pc += inst->sizeInBytes();
                break;
            }
            case BCOp::BinOp: {
                InstructionO8RRR* inst = (InstructionO8RRR*)_inst;
                execute_binop(inst);
                bytecode_pc += inst->sizeInBytes();
                break;
            }
            case BCOp::Print: {
                InstructionV* inst = (InstructionV*)_inst;
                execute_print(inst);
                bytecode_pc += inst->sizeInBytes();
                break;
            }
            case BCOp::Return: {
                InstructionR* inst = (InstructionR*)_inst;
                return reg[inst->reg];
                bytecode_pc += inst->sizeInBytes();
                break;
            }
            case BCOp::ReturnNone: {
                Instruction* inst = (Instruction*)_inst;
                return None;
                bytecode_pc += inst->sizeInBytes();
                break;
            }
            case BCOp::SetAttrParent: {
                InstructionRC* inst = (InstructionRC*)_inst;
                setattr(parent_module, getStrConst(inst->const_pool_index).c_str(), vregs[inst->reg_dst]);
                bytecode_pc += inst->sizeInBytes();
                break;
            }
            case BCOp::GetGlobalParent: {
                InstructionRC* inst = (InstructionRC*)_inst;
                std::string name = getStrConst(inst->const_pool_index);
                reg[inst->reg_dst] = getGlobal(parent_module, &name);
                bytecode_pc += inst->sizeInBytes();
                break;
            }
            case BCOp::CreateFunction: {
                InstructionRC* inst = (InstructionRC*)_inst;
                execute_createFunction(inst);
                bytecode_pc += inst->sizeInBytes();
                break;
            }
            case BCOp::RuntimeCall: {
                InstructionV* inst = (InstructionV*)_inst;
                execute_runtimeCall(inst);
                bytecode_pc += inst->sizeInBytes();
                break;
            }
            default:
                RELEASE_ASSERT(0, "not implemented");
                break;
        }
    }
}
}

Box* bcInterpretFunction(CompiledFunction* f, int nargs, Box* closure, Box* generator, Box* arg1, Box* arg2, Box* arg3,
                         Box** args) {
    std::shared_ptr<BCFunction> bc_function = generateBC(f);

    BCInterpreter interpreter(*bc_function, f);
    interpreter.initArguments(nargs, (BoxedClosure*)closure, (BoxedGenerator*)generator, arg1, arg2, arg3, args);
    return interpreter.run();
}
}
