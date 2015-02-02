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

#include "codegen/bc_generator.h"

#include <llvm/ADT/StringMap.h>
#include <unordered_map>

#include "analysis/function_analysis.h"
#include "analysis/scoping_analysis.h"
#include "codegen/bc_instructions.h"
#include "codegen/bc_printer.h"
#include "codegen/codegen.h"
#include "codegen/compvars.h"
#include "codegen/irgen.h"
#include "codegen/irgen/hooks.h"
#include "codegen/irgen/irgenerator.h"
#include "codegen/irgen/util.h"
#include "codegen/osrentry.h"
#include "core/ast.h"
#include "core/cfg.h"
#include "core/common.h"
#include "core/stats.h"
#include "core/thread_utils.h"
#include "core/util.h"
#include "runtime/generator.h"
#include "runtime/import.h"
#include "runtime/inline/boxing.h"
#include "runtime/long.h"
#include "runtime/objmodel.h"
#include "runtime/set.h"
#include "runtime/types.h"

namespace pyston {


class VReg {
public:
    VReg(uint32_t number = -1, bool is_unused = true) : number(number), is_unused(is_unused) {}

    uint32_t num() const { return number; }
    void setNumer(uint32_t reg_number) { number = reg_number; }

    bool isUnused() const { return is_unused; }
    void setUsed() { is_unused = false; }

    operator uint16_t() const { return num(); }

    static VReg* undefReg() {
        static VReg undef_reg(-1, false);
        return &undef_reg;
    }

private:
    uint32_t number;
    bool is_unused;
};

class GenerateBC {
public:
    typedef llvm::StringMap<Box*> SymMap;

    GenerateBC(CompiledFunction* compiled_function);

private:
    /*
    Box* createFunction(AST* node, AST_arguments* args, const std::vector<AST_stmt*>& body);
    Value doBinOp(Box* left, Box* right, int op, BinExpType exp_type);
    void doStore(AST_expr* node, Value value);
    void doStore(const std::string& name, Value value);
    void eraseDeadSymbols();

    Value visit_assert(AST_Assert* node);
    Value visit_assign(AST_Assign* node);
    Value visit_binop(AST_BinOp* node);
    Value visit_call(AST_Call* node);
    Value visit_classDef(AST_ClassDef* node);
    Value visit_compare(AST_Compare* node);
    Value visit_delete(AST_Delete* node);
    Value visit_functionDef(AST_FunctionDef* node);
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


    // pseudo
    Value visit_augBinOp(AST_AugBinOp* node);
    Value visit_branch(AST_Branch* node);
    Value visit_clsAttribute(AST_ClsAttribute* node);
    Value visit_invoke(AST_Invoke* node);
    Value visit_jump(AST_Jump* node);
    Value visit_langPrimitive(AST_LangPrimitive* node);

    CompiledFunction* compiled_func;*/
    SourceInfo* source_info;
    ScopeInfo* scope_info;

    void visit_stmt(AST_stmt* node);
    void visit_assign(AST_Assign* node);
    void visit_print(AST_Print* node);
    void visit_return(AST_Return* node);
    void visit_functionDef(AST_FunctionDef* node);

    VReg* visit_name(AST_Name* node);
    VReg* visit_num(AST_Num* node);
    VReg* visit_str(AST_Str* node);
    VReg* visit_binop(AST_BinOp* node);
    VReg* visit_call(AST_Call* node);

    template <class Inst> void addInstuction(const Inst& inst) {
        const uint8_t* begin = (const uint8_t*)&inst;
        const uint8_t* end = begin + inst.sizeInBytes();
        bytecode.insert(bytecode.end(), begin, end);
    }

    void addInstuctionV(BCOp op, const std::vector<uint16_t>& regs) {
        std::vector<uint8_t> buf(InstructionV(op, regs.size()).sizeInBytes(), 0);
        InstructionV* inst = (InstructionV*)&buf[0];
        *inst = InstructionV(op, regs.size());
        for (int i = 0; i < regs.size(); ++i)
            inst->reg[i] = regs[i];

        bytecode.insert(bytecode.end(), buf.begin(), buf.end());
    }


    void processBB(CFGBlock* bb);
    VReg* getInReg(AST_expr* node);
    VReg* allocReg();

    ConstPoolIndex addConst(Constant constant);


    void doStore(AST_expr* node, VReg* value);
    void doStore(const std::string& name, VReg* value);


    std::string printConst(ConstPoolIndex index);
    std::string printRegName(VReg reg);

private:
    std::unordered_map<std::string, VReg*> reg_map;
    unsigned num_regs;
    unsigned num_args;
    std::vector<Constant> const_pool;
    std::vector<unsigned char> bytecode;

public:
    /*
    AST_stmt* getCurrentStatement() {
        assert(current_inst);
        return current_inst;
    }

    CompiledFunction* getCF() { return compiled_func; }
    const SymMap& getSymbolTable() { return sym_table; }
    void gcVisit(GCVisitor* visitor);*/

    std::shared_ptr<BCFunction> generate();
};

VReg* GenerateBC::allocReg() {
    ++num_regs;
    return new VReg(num_regs - 1);
}

ConstPoolIndex GenerateBC::addConst(Constant constant) {
    const_pool.push_back(constant);
    return const_pool.size() - 1;
}

void GenerateBC::doStore(const std::string& name, VReg* value) {
    if (scope_info->refersToGlobal(name)) {
        addInstuction(InstructionRC(BCOp::SetAttrParent, value->num(), addConst(name)));
    } else {
        if (!reg_map.count(name)) {
            if (value->isUnused()) {
                // don't have to store the value...
                reg_map[name] = value;
                value->setUsed();
                return;
            }
            reg_map[name] = allocReg();
        }
        value->setUsed();
        addInstuction(InstructionRR(BCOp::Store, reg_map[name]->num(), value->num()));
        reg_map[name]->setUsed();
    }
}

void GenerateBC::doStore(AST_expr* node, VReg* value) {
    if (node->type == AST_TYPE::Name) {
        AST_Name* name = (AST_Name*)node;
        doStore(name->id, value);
    } else
        RELEASE_ASSERT(0, "not implemented");
}

GenerateBC::GenerateBC(CompiledFunction* compiled_function)
    : source_info(compiled_function->clfunc->source), scope_info(0), num_regs(0), num_args(0) {
    CLFunction* f = compiled_function->clfunc;
    if (!source_info->cfg)
        source_info->cfg = computeCFG(f->source, f->source->body);

    scope_info = source_info->getScopeInfo();

    if (source_info->arg_names.args) {
        for (AST_expr* e : *source_info->arg_names.args) {
            RELEASE_ASSERT(e->type == AST_TYPE::Name, "not implemented");
            AST_Name* name = (AST_Name*)e;
            VReg* reg = allocReg();
            reg_map[name->id] = reg;
            reg->setUsed();
            ++num_args;
        }
    }
}

std::shared_ptr<BCFunction> GenerateBC::generate() {
    CFG* cfg = source_info->cfg;
    for (auto* bb : cfg->blocks)
        processBB(bb);

    decltype(BCFunction::reg_map) newRegMap;
    for (auto&& r : reg_map)
        newRegMap[r.first] = r.second->num();

    return std::make_shared<BCFunction>(std::move(newRegMap), num_regs, num_args, const_pool, bytecode);
}

void GenerateBC::processBB(CFGBlock* bb) {
    for (auto* stmt : bb->body)
        visit_stmt(stmt);
}

void GenerateBC::visit_stmt(AST_stmt* node) {
    if (node->type == AST_TYPE::Assign)
        visit_assign((AST_Assign*)node);
    else if (node->type == AST_TYPE::Print)
        visit_print((AST_Print*)node);
    else if (node->type == AST_TYPE::Return)
        visit_return((AST_Return*)node);
    else if (node->type == AST_TYPE::FunctionDef)
        visit_functionDef((AST_FunctionDef*)node);
    else if (node->type == AST_TYPE::Expr)
        getInReg(((AST_Expr*)node)->value);
    else
        RELEASE_ASSERT(0, "not implemented");
}

void GenerateBC::visit_assign(AST_Assign* node) {
    VReg* vreg = getInReg(node->value);
    for (AST_expr* e : node->targets)
        doStore(e, vreg);
}

void GenerateBC::visit_print(AST_Print* node) {
    std::vector<uint16_t> reg_values;
    reg_values.push_back(node->nl);
    reg_values.push_back(node->dest ? getInReg(node->dest)->num() : (uint16_t)-1);
    for (auto* v : node->values) {
        VReg* v_reg = getInReg(v);
        v_reg->setUsed();
        reg_values.push_back(v_reg->num());
    }

    addInstuctionV(BCOp::Print, reg_values);
}

void GenerateBC::visit_return(AST_Return* node) {
    if (node->value) {
        VReg* src = getInReg(node->value);
        addInstuction(InstructionR(BCOp::Return, src->num()));
    } else {
        addInstuction(Instruction(BCOp::ReturnNone));
    }
}

void GenerateBC::visit_functionDef(AST_FunctionDef* node) {
    RELEASE_ASSERT(node->decorator_list.empty(), "not implemented");
    VReg* reg = allocReg();
    addInstuction(InstructionRC(BCOp::CreateFunction, reg->num(), addConst(node)));
    doStore(source_info->mangleName(node->name), reg);
}

VReg* GenerateBC::getInReg(AST_expr* node) {
    if (node->type == AST_TYPE::Name)
        return visit_name((AST_Name*)node);
    else if (node->type == AST_TYPE::Num)
        return visit_num((AST_Num*)node);
    else if (node->type == AST_TYPE::Str)
        return visit_str((AST_Str*)node);
    else if (node->type == AST_TYPE::BinOp)
        return visit_binop((AST_BinOp*)node);
    else if (node->type == AST_TYPE::Call)
        return visit_call((AST_Call*)node);
    RELEASE_ASSERT(0, "not implemented");
}

VReg* GenerateBC::visit_name(AST_Name* node) {
    if (scope_info->refersToGlobal(node->id)) {
        VReg* reg = allocReg();
        addInstuction(InstructionRC(BCOp::GetGlobalParent, reg->num(), addConst(node->id)));
        return reg;
    } else if (scope_info->refersToClosure(node->id)) {
        RELEASE_ASSERT(0, "not implemented");
    } else {
        auto it = reg_map.find(node->id);
        if (it != reg_map.end())
            return it->second;
    }

    RELEASE_ASSERT(0, "not implemented");
}

VReg* GenerateBC::visit_num(AST_Num* node) {
    VReg* reg = allocReg();
    ConstPoolIndex const_index = addConst(node);
    addInstuction(InstructionRC(BCOp::LoadConst, reg->num(), const_index));
    return reg;
}

VReg* GenerateBC::visit_str(AST_Str* node) {
    VReg* reg = allocReg();
    ConstPoolIndex const_index = addConst(node->s);
    addInstuction(InstructionRC(BCOp::LoadConst, reg->num(), const_index));
    return reg;
}

VReg* GenerateBC::visit_binop(AST_BinOp* node) {
    VReg* src1 = getInReg(node->left);
    VReg* src2 = getInReg(node->right);
    src1->setUsed();
    src2->setUsed();
    VReg* dst = allocReg();
    addInstuction(InstructionO8RRR(BCOp::BinOp, node->op_type, dst->num(), src1->num(), src2->num()));
    return dst;
}

VReg* GenerateBC::visit_call(AST_Call* node) {
    VReg* reg_dst = allocReg();
    VReg* func = VReg::undefReg();

    std::string* attr = nullptr;

    bool is_callattr = false;
    bool callattr_clsonly = false;
    if (node->func->type == AST_TYPE::Attribute) {
        is_callattr = true;
        callattr_clsonly = false;
        AST_Attribute* attr_ast = ast_cast<AST_Attribute>(node->func);
        func = getInReg(attr_ast->value);
        attr = &attr_ast->attr;
    } else if (node->func->type == AST_TYPE::ClsAttribute) {
        is_callattr = true;
        callattr_clsonly = true;
        AST_ClsAttribute* attr_ast = ast_cast<AST_ClsAttribute>(node->func);
        func = getInReg(attr_ast->value);
        attr = &attr_ast->attr;
    } else
        func = getInReg(node->func);
    func->setUsed();

    std::vector<VReg*> args;
    for (AST_expr* e : node->args)
        args.push_back(getInReg(e));

    std::vector<const std::string*> keywords;
    for (AST_keyword* k : node->keywords) {
        keywords.push_back(&k->arg);
        args.push_back(getInReg(k->value));
    }

    if (node->starargs)
        args.push_back(getInReg(node->starargs));

    if (node->kwargs)
        args.push_back(getInReg(node->kwargs));

    ArgPassSpec argspec(node->args.size(), node->keywords.size(), node->starargs, node->kwargs);

    if (is_callattr) {
        RELEASE_ASSERT(0, "not implemented");
        /*
                return callattr(func.o, attr, CallattrFlags({.cls_only = callattr_clsonly, .null_on_nonexistent = false
           }),
                                argspec, args.size() > 0 ? args[0] : 0, args.size() > 1 ? args[1] : 0,
                                args.size() > 2 ? args[2] : 0, args.size() > 3 ? &args[3] : 0, &keywords);*/
    } else {

        std::vector<uint16_t> regs = { (uint16_t)reg_dst->num(), (uint16_t)func->num() };
        for (VReg* r : args) {
            regs.push_back(r->num());
            r->setUsed();
        }
        addInstuctionV(BCOp::RuntimeCall, regs);
        /*
        return runtimeCall(func.o, argspec, args.size() > 0 ? args[0] : 0, args.size() > 1 ? args[1] : 0,
                           args.size() > 2 ? args[2] : 0, args.size() > 3 ? &args[3] : 0, &keywords);*/
    }
    return reg_dst;
}

std::string printReg(VReg reg) {
    char tmp[30];
    if (reg != (uint16_t)-1)
        sprintf(tmp, "%%%u", reg.num());
    else
        sprintf(tmp, "%%undef");
    return tmp;
}

std::string GenerateBC::printRegName(VReg reg) {
    char tmp[50] = { 0 };
    if (reg != (uint16_t)-1) {
        for (auto&& i : reg_map) {
            if (i.second->num() == reg.num()) {
                if (i.first[0] != '#')
                    sprintf(tmp, "%s=%s", printReg(reg).c_str(), i.first.c_str());
                return tmp;
            }
        }
    } else
        sprintf(tmp, "%%undef");
    return tmp;
}

std::string printConstPoolIndex(ConstPoolIndex index) {
    char tmp[30];
    sprintf(tmp, "#%u", index);
    return tmp;
}

std::string GenerateBC::printConst(ConstPoolIndex index) {
    char tmp[100];
    Constant c = const_pool[index];
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

std::shared_ptr<BCFunction> generateBC(CompiledFunction* f) {
    GenerateBC generate(f);
    std::shared_ptr<BCFunction> bc_function = generate.generate();
    printBC(bc_function);
    return bc_function;
}
}
