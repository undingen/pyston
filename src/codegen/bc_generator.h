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

#ifndef PYSTON_CODEGEN_BC_GENERATOR_H
#define PYSTON_CODEGEN_BC_GENERATOR_H

#include "codegen/bc_instructions.h"

namespace pyston {

class AST_Num;
class AST_FunctionDef;

class Constant {
public:
    Constant(AST_Num* node) : type(Type::Num), num_value(node) {}
    Constant(AST_FunctionDef* functionDef) : type(Type::FunctionDef), functionDef_value(functionDef) {}
    Constant(const std::string& str) : type(Type::String), string_value(str) {}

    enum class Type { Num, String, FunctionDef } type;

    Type getType() const { return type; }

    std::string string_value;
    union {
        AST_Num* num_value;
        AST_FunctionDef* functionDef_value;
    };
};

struct BCFunction {
    BCFunction(std::unordered_map<std::string, VRegIndex> reg_map, unsigned num_regs, unsigned num_args,
               std::vector<Constant> const_pool, std::vector<unsigned char> bytecode)
        : reg_map(reg_map), num_regs(num_regs), num_args(num_args), const_pool(const_pool), bytecode(bytecode) {}


    std::unordered_map<std::string, VRegIndex> reg_map;
    unsigned num_regs;
    unsigned num_args;
    std::vector<Constant> const_pool;
    std::vector<unsigned char> bytecode;
};

std::shared_ptr<BCFunction> generateBC(CompiledFunction* f);
}

#endif
