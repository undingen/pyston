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


#include "codegen/bc_printer.h"

#include "codegen/bc_generator.h"
#include "codegen/bc_instructions.h"
#include "core/ast.h"

namespace pyston {

namespace {

class BCPrinter {
public:
    BCPrinter(const BCFunction& bc_function) : bc_function(bc_function) {}

    void print();

private:
    std::string printReg(VRegIndex reg);
    std::string printConstPoolIndex(ConstPoolIndex index);
    std::string printConst(ConstPoolIndex index);
    std::string printRegName(VRegIndex reg);

    const BCFunction& bc_function;
};



std::string BCPrinter::printReg(VRegIndex reg) {
    char tmp[30];
    if (reg != (uint16_t)-1)
        sprintf(tmp, "%%%u", reg);
    else
        sprintf(tmp, "%%undef");
    return tmp;
}

std::string BCPrinter::printRegName(VRegIndex reg) {
    char tmp[50] = { 0 };
    if (reg != (uint16_t)-1) {
        for (auto&& i : bc_function.reg_map) {
            if (i.second == reg) {
                if (i.first[0] != '#')
                    sprintf(tmp, "%s=%s", printReg(reg).c_str(), i.first.c_str());
                return tmp;
            }
        }
    } else
        sprintf(tmp, "%%undef");
    return tmp;
}

std::string BCPrinter::printConstPoolIndex(ConstPoolIndex index) {
    char tmp[30];
    sprintf(tmp, "#%u", index);
    return tmp;
}

std::string BCPrinter::printConst(ConstPoolIndex index) {
    char tmp[100];
    Constant c = bc_function.const_pool[index];
    if (c.getType() == Constant::Type::Num) {
        AST_Num* node = c.num_value;
        if (node->num_type == AST_Num::INT) {
            sprintf(tmp, "int %ld", node->n_int);
        } else if (node->num_type == AST_Num::LONG) {
            sprintf(tmp, "long %sL", node->n_long.c_str());
        } else if (node->num_type == AST_Num::FLOAT) {
            sprintf(tmp, "float %f", node->n_float);
        } else if (node->num_type == AST_Num::COMPLEX) {
            sprintf(tmp, "complex %fj", node->n_float);
        } else {
            RELEASE_ASSERT(0, "");
        }
    } else if (c.getType() == Constant::Type::String) {
        sprintf(tmp, "string '%s'", c.string_value.c_str());
    } else if (c.getType() == Constant::Type::FunctionDef) {
        sprintf(tmp, "<code object %s at %p>", c.functionDef_value->name.c_str(), c.functionDef_value);
    } else {
        RELEASE_ASSERT(0, "");
    }

    return tmp;
}


void BCPrinter::print() {
    printf("; num args: %u num regs: %u num consts: %u\n", bc_function.num_args,
           bc_function.num_regs - bc_function.num_args, (unsigned)bc_function.const_pool.size());

    const unsigned char* bytecode_pc = &bc_function.bytecode[0];
    while (bytecode_pc != &bc_function.bytecode[bc_function.bytecode.size()]) {
        Instruction* _inst = (Instruction*)bytecode_pc;

        switch (_inst->op) {
            case BCOp::LoadConst: {
                InstructionRC* inst = (InstructionRC*)_inst;
                printf("%s = loadConst %s ; %s\n", printReg(inst->reg_dst).c_str(),
                       printConstPoolIndex(inst->const_pool_index).c_str(), printConst(inst->const_pool_index).c_str());
                bytecode_pc += inst->sizeInBytes();
                break;
            }
            case BCOp::Store: {
                InstructionRR* inst = (InstructionRR*)_inst;
                printf("store %s, %s ; %s %s\n", printReg(inst->reg_dst).c_str(), printReg(inst->reg_src).c_str(),
                       printRegName(inst->reg_dst).c_str(), printRegName(inst->reg_src).c_str());
                bytecode_pc += inst->sizeInBytes();
                break;
            }
            case BCOp::BinOp: {
                InstructionO8RRR* inst = (InstructionO8RRR*)_inst;
                printf("%s = %s %s %s\n", printReg(inst->reg_dst).c_str(), printReg(inst->reg_src1).c_str(),
                       getOpName(inst->other).c_str(), printReg(inst->reg_src2).c_str());
                bytecode_pc += inst->sizeInBytes();
                break;
            }
            case BCOp::Print: {
                InstructionV* inst = (InstructionV*)_inst;
                printf("print nl=%u dst: %s", inst->reg[0],
                       inst->reg[1] == (uint16_t)-1 ? "stdout" : printReg(inst->reg[1]).c_str());
                for (int i = 2; i < inst->num_args; ++i)
                    printf(" %s", printReg(inst->reg[i]).c_str());
                printf("\n");
                bytecode_pc += inst->sizeInBytes();
                break;
            }
            case BCOp::Return: {
                InstructionR* inst = (InstructionR*)_inst;
                printf("ret %s\n", printReg(inst->reg).c_str());
                bytecode_pc += inst->sizeInBytes();
                break;
            }
            case BCOp::ReturnNone: {
                Instruction* inst = (Instruction*)_inst;
                printf("ret None\n");
                bytecode_pc += inst->sizeInBytes();
                break;
            }

            case BCOp::SetAttrParent: {
                InstructionRC* inst = (InstructionRC*)_inst;
                printf("setAttrParent %s, %s ; %s\n", printConstPoolIndex(inst->const_pool_index).c_str(),
                       printReg(inst->reg_dst).c_str(), printConst(inst->const_pool_index).c_str());
                bytecode_pc += inst->sizeInBytes();
                break;
            }
            case BCOp::GetGlobalParent: {
                InstructionRC* inst = (InstructionRC*)_inst;
                printf("%s = getGlobalParent %s ; %s\n", printReg(inst->reg_dst).c_str(),
                       printConstPoolIndex(inst->const_pool_index).c_str(), printConst(inst->const_pool_index).c_str());
                bytecode_pc += inst->sizeInBytes();
                break;
            }
            case BCOp::CreateFunction: {
                InstructionRC* inst = (InstructionRC*)_inst;
                printf("%s = createFunction %s ; %s\n", printReg(inst->reg_dst).c_str(),
                       printConstPoolIndex(inst->const_pool_index).c_str(), printConst(inst->const_pool_index).c_str());
                bytecode_pc += inst->sizeInBytes();
                break;
            }
            case BCOp::RuntimeCall: {
                InstructionV* inst = (InstructionV*)_inst;
                printf("%s = runtimeCall %s(", printReg(inst->reg[0]).c_str(), printReg(inst->reg[1]).c_str());
                for (int i = 2; i < inst->num_args; ++i) {
                    if (i != 2)
                        printf(", ");
                    printf("%s", printReg(inst->reg[i]).c_str());
                }
                printf(")\n");
                bytecode_pc += inst->sizeInBytes();
                break;
            }
            default:
                RELEASE_ASSERT(0, "not implemented");
                break;
        }
    }
    printf("\n");
}
}

void printBC(std::shared_ptr<BCFunction> bc_function) {
    BCPrinter printer(*bc_function);
    printer.print();
}
}
