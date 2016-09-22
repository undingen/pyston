﻿// Copyright (c) 2014-2016 Dropbox, Inc.
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

#include "core/bst.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdint.h>

#include "Python.h"

#include "core/cfg.h"
#include "runtime/types.h"

namespace pyston {

#ifdef DEBUG_LINE_NUMBERS
int BST::next_lineno = 100000;

BST::BST(BST_TYPE::BST_TYPE type) : type(type), lineno(++next_lineno) {
    // if (lineno == 100644)
    // raise(SIGTRAP);
}

#endif

template <class T> static void visitVector(const std::vector<T*>& vec, BSTVisitor* v) {
    for (int i = 0; i < vec.size(); i++) {
        vec[i]->accept(v);
    }
}

static void visitCFG(CFG* cfg, BSTVisitor* v) {
    for (auto bb : cfg->blocks)
        for (auto e : bb->body)
            e->accept(v);
}

void BST_Assert::accept(BSTVisitor* v) {
    bool skip = v->visit_assert(this);
    if (skip)
        return;

    // if (vreg_msg != VREG_UNDEFINED)
    v->visit_vreg(&vreg_msg);
}

void BST_Assert::accept_stmt(StmtVisitor* v) {
    v->visit_assert(this);
}

void BST_Assign::accept(BSTVisitor* v) {
    bool skip = v->visit_assign(this);
    if (skip)
        return;

    value->accept(v);
    target->accept(v);
}

void BST_Assign::accept_stmt(StmtVisitor* v) {
    v->visit_assign(this);
}

void BST_AssignVRegVReg::accept(BSTVisitor* v) {
    bool skip = v->visit_assignvregvreg(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_dst, true);
    v->visit_vreg(&vreg_src);
}

void BST_AssignVRegVReg::accept_stmt(StmtVisitor* v) {
    v->visit_assignvregvreg(this);
}

void BST_AugBinOp::accept(BSTVisitor* v) {
    bool skip = v->visit_augbinop(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_dst, true);
    v->visit_vreg(&vreg_left);
    v->visit_vreg(&vreg_right);
}

void BST_AugBinOp::accept_stmt(StmtVisitor* v) {
    return v->visit_augbinop(this);
}

void BST_BinOp::accept(BSTVisitor* v) {
    bool skip = v->visit_binop(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_dst, true);
    v->visit_vreg(&vreg_left);
    v->visit_vreg(&vreg_right);
}

void BST_BinOp::accept_stmt(StmtVisitor* v) {
    return v->visit_binop(this);
}

void BST_CallFunc::accept(BSTVisitor* v) {
    bool skip = v->visit_callfunc(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_dst, true);
    v->visit_vreg(&vreg_func);
    for (int i = 0; i < num_args + num_keywords; ++i) {
        v->visit_vreg(&elts[i]);
    }
    v->visit_vreg(&vreg_starargs);
    v->visit_vreg(&vreg_kwargs);
}

void BST_CallFunc::accept_stmt(StmtVisitor* v) {
    return v->visit_callfunc(this);
}

void BST_CallAttr::accept(BSTVisitor* v) {
    bool skip = v->visit_callattr(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_dst, true);
    v->visit_vreg(&vreg_value);
    for (int i = 0; i < num_args + num_keywords; ++i) {
        v->visit_vreg(&elts[i]);
    }
    v->visit_vreg(&vreg_starargs);
    v->visit_vreg(&vreg_kwargs);
}

void BST_CallAttr::accept_stmt(StmtVisitor* v) {
    return v->visit_callattr(this);
}

void BST_CallClsAttr::accept(BSTVisitor* v) {
    bool skip = v->visit_callclsattr(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_dst, true);
    v->visit_vreg(&vreg_value);
    for (int i = 0; i < num_args + num_keywords; ++i) {
        v->visit_vreg(&elts[i]);
    }
    v->visit_vreg(&vreg_starargs);
    v->visit_vreg(&vreg_kwargs);
}

void BST_CallClsAttr::accept_stmt(StmtVisitor* v) {
    return v->visit_callclsattr(this);
}

void BST_Compare::accept(BSTVisitor* v) {
    bool skip = v->visit_compare(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_dst, true);
    v->visit_vreg(&vreg_left);
    v->visit_vreg(&vreg_comparator);
}

void BST_Compare::accept_stmt(StmtVisitor* v) {
    return v->visit_compare(this);
}

void BST_ClassDef::accept(BSTVisitor* v) {
    bool skip = v->visit_classdef(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_bases_tuple);
    for (int i = 0; i < num_decorator; ++i) {
        v->visit_vreg(&decorator[i]);
    }
    visitCFG(this->code->source->cfg, v);
}

void BST_ClassDef::accept_stmt(StmtVisitor* v) {
    v->visit_classdef(this);
}

void BST_DeleteAttr::accept(BSTVisitor* v) {
    bool skip = v->visit_deleteattr(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_value);
}

void BST_DeleteAttr::accept_stmt(StmtVisitor* v) {
    v->visit_deleteattr(this);
}

void BST_DeleteSub::accept(BSTVisitor* v) {
    bool skip = v->visit_deletesub(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_value);
    v->visit_vreg(&vreg_slice);
}

void BST_DeleteSub::accept_stmt(StmtVisitor* v) {
    v->visit_deletesub(this);
}

void BST_DeleteSubSlice::accept(BSTVisitor* v) {
    bool skip = v->visit_deletesubslice(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_value);
    v->visit_vreg(&vreg_lower);
    v->visit_vreg(&vreg_upper);
}

void BST_DeleteSubSlice::accept_stmt(StmtVisitor* v) {
    v->visit_deletesubslice(this);
}


void BST_DeleteName::accept(BSTVisitor* v) {
    bool skip = v->visit_deletename(this);
    if (skip)
        return;
    v->visit_vreg(&vreg);
}

void BST_DeleteName::accept_stmt(StmtVisitor* v) {
    v->visit_deletename(this);
}

void BST_Dict::accept(BSTVisitor* v) {
    bool skip = v->visit_dict(this);
    if (skip)
        return;
    v->visit_vreg(&vreg_dst, true);
}

void BST_Dict::accept_stmt(StmtVisitor* v) {
    return v->visit_dict(this);
}

void BST_Ellipsis::accept(BSTVisitor* v) {
    bool skip = v->visit_ellipsis(this);
    if (skip)
        return;
    v->visit_vreg(&vreg_dst, true);
}

void BST_Ellipsis::accept_stmt(StmtVisitor* v) {
    return v->visit_ellipsis(this);
}

void BST_Exec::accept(BSTVisitor* v) {
    bool skip = v->visit_exec(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_body);
    v->visit_vreg(&vreg_globals);
    v->visit_vreg(&vreg_locals);
}

void BST_Exec::accept_stmt(StmtVisitor* v) {
    v->visit_exec(this);
}

void BST_FunctionDef::accept(BSTVisitor* v) {
    bool skip = v->visit_functiondef(this);
    if (skip)
        return;

    for (int i = 0; i < num_decorator + num_defaults; ++i) {
        v->visit_vreg(&elts[i]);
    }
    visitCFG(code->source->cfg, v);
}

void BST_FunctionDef::accept_stmt(StmtVisitor* v) {
    v->visit_functiondef(this);
}

void BST_Invoke::accept(BSTVisitor* v) {
    bool skip = v->visit_invoke(this);
    if (skip)
        return;

    this->stmt->accept(v);
}

void BST_Invoke::accept_stmt(StmtVisitor* v) {
    return v->visit_invoke(this);
}

void BST_Landingpad::accept(BSTVisitor* v) {
    bool skip = v->visit_landingpad(this);
    if (skip)
        return;
    v->visit_vreg(&vreg_dst, true);
}

void BST_Landingpad::accept_stmt(StmtVisitor* v) {
    return v->visit_landingpad(this);
}

void BST_Locals::accept(BSTVisitor* v) {
    bool skip = v->visit_locals(this);
    if (skip)
        return;
    v->visit_vreg(&vreg_dst, true);
}

void BST_Locals::accept_stmt(StmtVisitor* v) {
    return v->visit_locals(this);
}

void BST_GetIter::accept(BSTVisitor* v) {
    bool skip = v->visit_getiter(this);
    if (skip)
        return;
    v->visit_vreg(&vreg_dst, true);
    v->visit_vreg(&vreg_value);
}

void BST_GetIter::accept_stmt(StmtVisitor* v) {
    return v->visit_getiter(this);
}

void BST_ImportFrom::accept(BSTVisitor* v) {
    bool skip = v->visit_importfrom(this);
    if (skip)
        return;
    v->visit_vreg(&vreg_dst, true);
    v->visit_vreg(&vreg_module);
    v->visit_vreg(&vreg_name);
}

void BST_ImportFrom::accept_stmt(StmtVisitor* v) {
    return v->visit_importfrom(this);
}

void BST_ImportName::accept(BSTVisitor* v) {
    bool skip = v->visit_importname(this);
    if (skip)
        return;
    v->visit_vreg(&vreg_dst, true);
    v->visit_vreg(&vreg_from);
    v->visit_vreg(&vreg_name);
}

void BST_ImportName::accept_stmt(StmtVisitor* v) {
    return v->visit_importname(this);
}

void BST_ImportStar::accept(BSTVisitor* v) {
    bool skip = v->visit_importstar(this);
    if (skip)
        return;
    v->visit_vreg(&vreg_dst, true);
    v->visit_vreg(&vreg_name);
}

void BST_ImportStar::accept_stmt(StmtVisitor* v) {
    return v->visit_importstar(this);
}

void BST_None::accept(BSTVisitor* v) {
    bool skip = v->visit_none(this);
    if (skip)
        return;
}

void* BST_None::accept_expr(ExprVisitor* v) {
    return v->visit_none(this);
}

void BST_Nonzero::accept(BSTVisitor* v) {
    bool skip = v->visit_nonzero(this);
    if (skip)
        return;
    v->visit_vreg(&vreg_dst, true);
    v->visit_vreg(&vreg_value);
}

void BST_Nonzero::accept_stmt(StmtVisitor* v) {
    return v->visit_nonzero(this);
}

void BST_CheckExcMatch::accept(BSTVisitor* v) {
    bool skip = v->visit_checkexcmatch(this);
    if (skip)
        return;
    v->visit_vreg(&vreg_dst, true);
    v->visit_vreg(&vreg_value);
    v->visit_vreg(&vreg_cls);
}

void BST_CheckExcMatch::accept_stmt(StmtVisitor* v) {
    return v->visit_checkexcmatch(this);
}

void BST_SetExcInfo::accept(BSTVisitor* v) {
    bool skip = v->visit_setexcinfo(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_type);
    v->visit_vreg(&vreg_value);
    v->visit_vreg(&vreg_traceback);
}

void BST_SetExcInfo::accept_stmt(StmtVisitor* v) {
    return v->visit_setexcinfo(this);
}

void BST_UncacheExcInfo::accept(BSTVisitor* v) {
    bool skip = v->visit_uncacheexcinfo(this);
    if (skip)
        return;
}

void BST_UncacheExcInfo::accept_stmt(StmtVisitor* v) {
    return v->visit_uncacheexcinfo(this);
}

void BST_HasNext::accept(BSTVisitor* v) {
    bool skip = v->visit_hasnext(this);
    if (skip)
        return;
    v->visit_vreg(&vreg_dst, true);
    v->visit_vreg(&vreg_value);
}

void BST_HasNext::accept_stmt(StmtVisitor* v) {
    return v->visit_hasnext(this);
}

void BST_PrintExpr::accept(BSTVisitor* v) {
    bool skip = v->visit_printexpr(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_value);
}

void BST_PrintExpr::accept_stmt(StmtVisitor* v) {
    return v->visit_printexpr(this);
}

void BST_List::accept(BSTVisitor* v) {
    bool skip = v->visit_list(this);
    if (skip)
        return;
    v->visit_vreg(&vreg_dst, true);
    for (int i = 0; i < num_elts; ++i)
        v->visit_vreg(&elts[i]);
}

void BST_List::accept_stmt(StmtVisitor* v) {
    return v->visit_list(this);
}

void BST_LoadName::accept(BSTVisitor* v) {
    bool skip = v->visit_loadname(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_dst, true);
    v->visit_vreg(&vreg);
}


void BST_LoadName::accept_stmt(StmtVisitor* v) {
    v->visit_loadname(this);
}

void BST_LoadAttr::accept(BSTVisitor* v) {
    bool skip = v->visit_loadattr(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_dst, true);
    v->visit_vreg(&vreg_value);
}


void BST_LoadAttr::accept_stmt(StmtVisitor* v) {
    v->visit_loadattr(this);
}


void BST_LoadSub::accept(BSTVisitor* v) {
    bool skip = v->visit_loadsub(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_dst, true);
    v->visit_vreg(&vreg_value);
    v->visit_vreg(&vreg_slice);
}


void BST_LoadSub::accept_stmt(StmtVisitor* v) {
    v->visit_loadsub(this);
}

void BST_LoadSubSlice::accept(BSTVisitor* v) {
    bool skip = v->visit_loadsubslice(this);
    if (skip)
        return;
    v->visit_vreg(&vreg_dst, true);
    v->visit_vreg(&vreg_value);
    v->visit_vreg(&vreg_lower);
    v->visit_vreg(&vreg_upper);
}

void BST_LoadSubSlice::accept_stmt(StmtVisitor* v) {
    v->visit_loadsubslice(this);
}


void BST_StoreName::accept(BSTVisitor* v) {
    bool skip = v->visit_storename(this);
    if (skip)
        return;

    v->visit_vreg(&vreg);
    v->visit_vreg(&vreg_value);
}


void BST_StoreName::accept_stmt(StmtVisitor* v) {
    v->visit_storename(this);
}

void BST_StoreAttr::accept(BSTVisitor* v) {
    bool skip = v->visit_storeattr(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_target);
    v->visit_vreg(&vreg_value);
}


void BST_StoreAttr::accept_stmt(StmtVisitor* v) {
    v->visit_storeattr(this);
}

void BST_StoreSub::accept(BSTVisitor* v) {
    bool skip = v->visit_storesub(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_target);
    v->visit_vreg(&vreg_slice);
    v->visit_vreg(&vreg_value);
}


void BST_StoreSub::accept_stmt(StmtVisitor* v) {
    v->visit_storesub(this);
}

void BST_StoreSubSlice::accept(BSTVisitor* v) {
    bool skip = v->visit_storesubslice(this);
    if (skip)
        return;
    v->visit_vreg(&vreg_target);
    v->visit_vreg(&vreg_lower);
    v->visit_vreg(&vreg_upper);
    v->visit_vreg(&vreg_value);
}

void BST_StoreSubSlice::accept_stmt(StmtVisitor* v) {
    v->visit_storesubslice(this);
}

void BST_Name::accept(BSTVisitor* v) {
    bool skip = v->visit_name(this);
}

void* BST_Name::accept_expr(ExprVisitor* v) {
    return v->visit_name(this);
}

void BST_Num::accept(BSTVisitor* v) {
    bool skip = v->visit_num(this);
}

void* BST_Num::accept_expr(ExprVisitor* v) {
    return v->visit_num(this);
}

void BST_Print::accept(BSTVisitor* v) {
    bool skip = v->visit_print(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_dest);
    v->visit_vreg(&vreg_value);
}

void BST_Print::accept_stmt(StmtVisitor* v) {
    v->visit_print(this);
}

void BST_Raise::accept(BSTVisitor* v) {
    bool skip = v->visit_raise(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_arg0);
    v->visit_vreg(&vreg_arg1);
    v->visit_vreg(&vreg_arg2);
}

void BST_Raise::accept_stmt(StmtVisitor* v) {
    v->visit_raise(this);
}

void BST_Repr::accept(BSTVisitor* v) {
    bool skip = v->visit_repr(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_dst, true);
    v->visit_vreg(&vreg_value);
}

void BST_Repr::accept_stmt(StmtVisitor* v) {
    return v->visit_repr(this);
}

void BST_Return::accept(BSTVisitor* v) {
    bool skip = v->visit_return(this);
    if (skip)
        return;

    // if (vreg_value != VREG_UNDEFINED)
    v->visit_vreg(&vreg_value);
}

void BST_Return::accept_stmt(StmtVisitor* v) {
    v->visit_return(this);
}

void BST_Set::accept(BSTVisitor* v) {
    bool skip = v->visit_set(this);
    if (skip)
        return;
    v->visit_vreg(&vreg_dst, true);
    for (int i = 0; i < num_elts; ++i) {
        v->visit_vreg(&elts[i]);
    }
}

void BST_Set::accept_stmt(StmtVisitor* v) {
    return v->visit_set(this);
}

void BST_Str::accept(BSTVisitor* v) {
    bool skip = v->visit_str(this);
    if (skip)
        return;
}

void* BST_Str::accept_expr(ExprVisitor* v) {
    return v->visit_str(this);
}

void BST_Tuple::accept(BSTVisitor* v) {
    bool skip = v->visit_tuple(this);
    if (skip)
        return;
    v->visit_vreg(&vreg_dst, true);
    for (int i = 0; i < num_elts; ++i) {
        v->visit_vreg(&elts[i]);
    }
}

void BST_Tuple::accept_stmt(StmtVisitor* v) {
    return v->visit_tuple(this);
}

void BST_UnaryOp::accept(BSTVisitor* v) {
    bool skip = v->visit_unaryop(this);
    if (skip)
        return;
    v->visit_vreg(&vreg_dst, true);
    v->visit_vreg(&vreg_operand);
}

void BST_UnaryOp::accept_stmt(StmtVisitor* v) {
    return v->visit_unaryop(this);
}

void BST_UnpackIntoArray::accept(BSTVisitor* v) {
    bool skip = v->visit_unpackintoarray(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_src);
    for (int i = 0; i < num_elts; ++i) {
        v->visit_vreg(&vreg_dst[i], true);
    }
}

void BST_UnpackIntoArray::accept_stmt(StmtVisitor* v) {
    return v->visit_unpackintoarray(this);
}

void BST_Yield::accept(BSTVisitor* v) {
    bool skip = v->visit_yield(this);
    if (skip)
        return;
    v->visit_vreg(&vreg_dst, true);
    v->visit_vreg(&vreg_value);
}

void BST_Yield::accept_stmt(StmtVisitor* v) {
    return v->visit_yield(this);
}

void BST_Branch::accept(BSTVisitor* v) {
    bool skip = v->visit_branch(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_test);
}

void BST_Branch::accept_stmt(StmtVisitor* v) {
    v->visit_branch(this);
}

void BST_Jump::accept(BSTVisitor* v) {
    bool skip = v->visit_jump(this);
    if (skip)
        return;
}

void BST_Jump::accept_stmt(StmtVisitor* v) {
    v->visit_jump(this);
}

void BST_MakeFunction::accept(BSTVisitor* v) {
    bool skip = v->visit_makefunction(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_dst, true);
    function_def->accept(v);
}

void BST_MakeFunction::accept_stmt(StmtVisitor* v) {
    return v->visit_makefunction(this);
}

void BST_MakeClass::accept(BSTVisitor* v) {
    bool skip = v->visit_makeclass(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_dst, true);
    class_def->accept(v);
}

void BST_MakeClass::accept_stmt(StmtVisitor* v) {
    return v->visit_makeclass(this);
}

void BST_MakeSlice::accept(BSTVisitor* v) {
    bool skip = v->visit_makeslice(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_dst, true);
    v->visit_vreg(&vreg_lower);
    v->visit_vreg(&vreg_upper);
    v->visit_vreg(&vreg_step);
}

void BST_MakeSlice::accept_stmt(StmtVisitor* v) {
    return v->visit_makeslice(this);
}

void print_bst(BST* bst) {
    PrintVisitor v(0, llvm::outs(), NULL);
    bst->accept(&v);
    v.flush();
}

void PrintVisitor::printIndent() {
    for (int i = 0; i < indent; i++) {
        stream << ' ';
    }
}

extern "C" BoxedString* repr(Box* obj);
bool PrintVisitor::visit_vreg(int* vreg, bool is_dst) {
    if (*vreg != VREG_UNDEFINED)
        stream << "@" << *vreg;
    else
        stream << "@undef";
    if (mod && *vreg < 0 && *vreg != VREG_UNDEFINED)
        stream << "(" << autoDecref(repr(mod->constants[-*vreg - 1]))->s() << ")";

    if (is_dst)
        stream << " =";

    return true;
}

bool PrintVisitor::visit_assert(BST_Assert* node) {
    stream << "assert 0";
    if (node->vreg_msg != VREG_UNDEFINED) {
        stream << ", ";
        visit_vreg(&node->vreg_msg);
    }
    return true;
}

bool PrintVisitor::visit_assign(BST_Assign* node) {
    node->target->accept(this);
    stream << " = ";
    node->value->accept(this);
    return true;
}

bool PrintVisitor::visit_assignvregvreg(BST_AssignVRegVReg* node) {
    visit_vreg(&node->vreg_dst, true);
    if (!node->kill_src)
        stream << "nokill ";
    visit_vreg(&node->vreg_src);
    return true;
}

void PrintVisitor::printOp(AST_TYPE::AST_TYPE op_type) {
    switch (op_type) {
        case BST_TYPE::Add:
            stream << '+';
            break;
        case BST_TYPE::BitAnd:
            stream << '&';
            break;
        case BST_TYPE::BitOr:
            stream << '|';
            break;
        case BST_TYPE::BitXor:
            stream << '^';
            break;
        case BST_TYPE::Div:
            stream << '/';
            break;
        case BST_TYPE::LShift:
            stream << "<<";
            break;
        case BST_TYPE::RShift:
            stream << ">>";
            break;
        case BST_TYPE::Pow:
            stream << "**";
            break;
        case BST_TYPE::Mod:
            stream << '%';
            break;
        case BST_TYPE::Mult:
            stream << '*';
            break;
        case BST_TYPE::Sub:
            stream << '-';
            break;
        default:
            stream << "<" << (int)op_type << ">";
            break;
    }
}

bool PrintVisitor::visit_augbinop(BST_AugBinOp* node) {
    visit_vreg(&node->vreg_dst, true);
    visit_vreg(&node->vreg_left);
    stream << '=';
    printOp(node->op_type);
    visit_vreg(&node->vreg_right);
    return true;
}

bool PrintVisitor::visit_binop(BST_BinOp* node) {
    visit_vreg(&node->vreg_dst, true);
    visit_vreg(&node->vreg_left);
    printOp(node->op_type);
    visit_vreg(&node->vreg_right);
    return true;
}

bool PrintVisitor::visit_callfunc(BST_CallFunc* node) {
    visit_vreg(&node->vreg_dst, true);
    visit_vreg(&node->vreg_func);
    stream << "(";

    bool prevarg = false;
    for (int i = 0; i < node->num_args + node->num_keywords; ++i) {
        if (prevarg)
            stream << ", ";
        visit_vreg(&node->elts[i]);
        prevarg = true;
    }
    if (node->vreg_starargs != VREG_UNDEFINED) {
        if (prevarg)
            stream << ", ";
        visit_vreg(&node->vreg_starargs);
        prevarg = true;
    }
    if (node->vreg_kwargs != VREG_UNDEFINED) {
        if (prevarg)
            stream << ", ";
        visit_vreg(&node->vreg_kwargs);
        prevarg = true;
    }
    stream << ")";
    return true;
}

bool PrintVisitor::visit_callattr(BST_CallAttr* node) {
    visit_vreg(&node->vreg_dst, true);
    stream << "@" << node->vreg_value << ".";
    stream << node->attr.s();
    stream << "(";

    bool prevarg = false;
    for (int i = 0; i < node->num_args + node->num_keywords; ++i) {
        if (prevarg)
            stream << ", ";
        visit_vreg(&node->elts[i]);
        prevarg = true;
    }
    if (node->vreg_starargs != VREG_UNDEFINED) {
        if (prevarg)
            stream << ", ";
        visit_vreg(&node->vreg_starargs);
        prevarg = true;
    }
    if (node->vreg_kwargs != VREG_UNDEFINED) {
        if (prevarg)
            stream << ", ";
        visit_vreg(&node->vreg_kwargs);
        prevarg = true;
    }
    stream << ")";
    return true;
}

bool PrintVisitor::visit_callclsattr(BST_CallClsAttr* node) {
    visit_vreg(&node->vreg_dst, true);
    stream << "@" << node->vreg_value << ":";
    stream << node->attr.s();
    stream << "(";

    bool prevarg = false;
    for (int i = 0; i < node->num_args + node->num_keywords; ++i) {
        if (prevarg)
            stream << ", ";
        visit_vreg(&node->elts[i]);
        prevarg = true;
    }
    if (node->vreg_starargs != VREG_UNDEFINED) {
        if (prevarg)
            stream << ", ";
        visit_vreg(&node->vreg_starargs);
        prevarg = true;
    }
    if (node->vreg_kwargs != VREG_UNDEFINED) {
        if (prevarg)
            stream << ", ";
        visit_vreg(&node->vreg_kwargs);
        prevarg = true;
    }
    stream << ")";
    return true;
}

bool PrintVisitor::visit_compare(BST_Compare* node) {
    visit_vreg(&node->vreg_dst, true);
    visit_vreg(&node->vreg_left);
    stream << " " << getOpSymbol(node->op) << " ";
    visit_vreg(&node->vreg_comparator);

    return true;
}

bool PrintVisitor::visit_classdef(BST_ClassDef* node) {
    for (int i = 0, n = node->num_decorator; i < n; i++) {
        stream << "@";
        visit_vreg(&node->decorator[i]);
        stream << "\n";
        printIndent();
    }
    stream << "class " << node->name.s() << "(";
    visit_vreg(&node->vreg_bases_tuple);
    stream << ")";

    indent += 4;
    stream << '\n';
    printIndent();
    stream << "...";
#if 0
    for (int i = 0, n = node->body.size(); i < n; i++) {
        stream << "\n";
        printIndent();
        node->body[i]->accept(this);
    }
#endif
    indent -= 4;

    return true;
}

bool PrintVisitor::visit_deletesub(BST_DeleteSub* node) {
    stream << "del ";
    visit_vreg(&node->vreg_value);
    stream << "[";
    visit_vreg(&node->vreg_slice);
    stream << "]";
    return true;
}
bool PrintVisitor::visit_deletesubslice(BST_DeleteSubSlice* node) {
    stream << "del ";
    visit_vreg(&node->vreg_value);
    stream << "[";
    visit_vreg(&node->vreg_lower);
    stream << ":";
    visit_vreg(&node->vreg_upper);
    stream << "]";
    return true;
}
bool PrintVisitor::visit_deleteattr(BST_DeleteAttr* node) {
    stream << "del ";
    visit_vreg(&node->vreg_value);
    stream << '.';
    stream << node->attr.s();
    return true;
}
bool PrintVisitor::visit_deletename(BST_DeleteName* node) {
    stream << "del ";
    stream << node->id.s();
    stream << "(#" << node->vreg << ")";
    return true;
}

bool PrintVisitor::visit_dict(BST_Dict* node) {
    visit_vreg(&node->vreg_dst, true);
    stream << "{}";
    return true;
}

bool PrintVisitor::visit_ellipsis(BST_Ellipsis* node) {
    visit_vreg(&node->vreg_dst, true);
    stream << "...";
    return true;
}

bool PrintVisitor::visit_exec(BST_Exec* node) {
    stream << "exec ";

    visit_vreg(&node->vreg_body);
    if (node->vreg_globals != VREG_UNDEFINED) {
        stream << " in ";
        visit_vreg(&node->vreg_globals);

        if (node->vreg_locals != VREG_UNDEFINED) {
            stream << ", ";
            visit_vreg(&node->vreg_locals);
        }
    }
    stream << "\n";
    return true;
}

bool PrintVisitor::visit_functiondef(BST_FunctionDef* node) {
    for (int i = 0; i < node->num_decorator; ++i) {
        stream << "@";
        visit_vreg(&node->elts[i]);
        stream << "\n";
        printIndent();
    }

    stream << "def ";
    if (node->name != InternedString())
        stream << node->name.s();
    else
        stream << "<lambda>";
    stream << "(";

    for (int i = 0; i < node->num_defaults; ++i) {
        if (i > 0)
            stream << ", ";

        stream << "<default " << i << ">=";
        visit_vreg(&node->elts[node->num_decorator + i]);
    }

    stream << ")";

    indent += 4;
    stream << '\n';
    printIndent();
    stream << "...";
#if 0
    for (int i = 0; i < node->body.size(); i++) {
        stream << "\n";
        printIndent();
        node->body[i]->accept(this);
    }
#endif
    indent -= 4;
    return true;
}

bool PrintVisitor::visit_invoke(BST_Invoke* node) {
    stream << "invoke " << node->normal_dest->idx << " " << node->exc_dest->idx << ": ";
    node->stmt->accept(this);
    return true;
}
/*
bool PrintVisitor::visit_langprimitive(BST_LangPrimitive* node) {
    stream << ":";
    switch (node->opcode) {
        case BST_LangPrimitive::CHECK_EXC_MATCH:
            stream << "CHECK_EXC_MATCH";
            break;
        case BST_LangPrimitive::LANDINGPAD:
            stream << "LANDINGPAD";
            break;
        case BST_LangPrimitive::LOCALS:
            stream << "LOCALS";
            break;
        case BST_LangPrimitive::GET_ITER:
            stream << "GET_ITER";
            break;
        case BST_LangPrimitive::IMPORT_FROM:
            stream << "IMPORT_FROM";
            break;
        case BST_LangPrimitive::IMPORT_NAME:
            stream << "IMPORT_NAME";
            break;
        case BST_LangPrimitive::IMPORT_STAR:
            stream << "IMPORT_STAR";
            break;
        case BST_LangPrimitive::NONE:
            stream << "NONE";
            break;
        case BST_LangPrimitive::NONZERO:
            stream << "NONZERO";
            break;
        case BST_LangPrimitive::SET_EXC_INFO:
            stream << "SET_EXC_INFO";
            break;
        case BST_LangPrimitive::UNCACHE_EXC_INFO:
            stream << "UNCACHE_EXC_INFO";
            break;
        case BST_LangPrimitive::HASNEXT:
            stream << "HASNEXT";
            break;
        case BST_LangPrimitive::PRINT_EXPR:
            stream << "PRINT_EXPR";
            break;
        default:
            RELEASE_ASSERT(0, "%d", node->opcode);
    }
    stream << "(";
    for (int i = 0, n = node->args.size(); i < n; ++i) {
        if (i > 0)
            stream << ", ";
        node->args[i]->accept(this);
    }
    stream << ")";
    return true;
}
*/
bool PrintVisitor::visit_landingpad(BST_Landingpad* node) {
    visit_vreg(&node->vreg_dst, true);
    stream << ":LANDINGPAD()";
    return true;
}
bool PrintVisitor::visit_locals(BST_Locals* node) {
    visit_vreg(&node->vreg_dst, true);
    stream << ":LOCALS()";
    return true;
}
bool PrintVisitor::visit_getiter(BST_GetIter* node) {
    visit_vreg(&node->vreg_dst, true);
    stream << ":GET_ITER(";
    visit_vreg(&node->vreg_value);
    stream << ")";
    return true;
}
bool PrintVisitor::visit_importfrom(BST_ImportFrom* node) {
    visit_vreg(&node->vreg_dst, true);
    stream << ":IMPORT_FROM(";
    visit_vreg(&node->vreg_module);
    stream << ", ";
    visit_vreg(&node->vreg_name);
    stream << ")";
    return true;
}
bool PrintVisitor::visit_importname(BST_ImportName* node) {
    visit_vreg(&node->vreg_dst, true);
    stream << ":IMPORT_NAME(";
    visit_vreg(&node->vreg_from);
    stream << ", ";
    visit_vreg(&node->vreg_name);
    stream << ", " << node->level << ")";
    return true;
}
bool PrintVisitor::visit_importstar(BST_ImportStar* node) {
    visit_vreg(&node->vreg_dst, true);
    stream << ":IMPORT_STAR(";
    visit_vreg(&node->vreg_name);
    stream << ")";
    return true;
}
bool PrintVisitor::visit_none(BST_None* node) {
    stream << ":NONE()";
    return true;
}
bool PrintVisitor::visit_nonzero(BST_Nonzero* node) {
    visit_vreg(&node->vreg_dst, true);
    stream << ":NONZERO(";
    visit_vreg(&node->vreg_value);
    stream << ")";
    return true;
}
bool PrintVisitor::visit_checkexcmatch(BST_CheckExcMatch* node) {
    visit_vreg(&node->vreg_dst, true);
    stream << ":CHECK_EXC_MATCH(";
    visit_vreg(&node->vreg_value);
    stream << ", ";
    visit_vreg(&node->vreg_cls);
    stream << ")";
    return true;
}
bool PrintVisitor::visit_setexcinfo(BST_SetExcInfo* node) {
    stream << ":SET_EXC_INFO(";
    visit_vreg(&node->vreg_value);
    stream << ", ";
    visit_vreg(&node->vreg_type);
    stream << ", ";
    visit_vreg(&node->vreg_traceback);
    stream << ")";
    return true;
}
bool PrintVisitor::visit_uncacheexcinfo(BST_UncacheExcInfo* node) {
    stream << ":UNCACHE_EXC_INFO()";
    return true;
}
bool PrintVisitor::visit_hasnext(BST_HasNext* node) {
    visit_vreg(&node->vreg_dst, true);
    stream << ":HAS_NEXT(";
    visit_vreg(&node->vreg_value);
    stream << ")";
    return true;
}
bool PrintVisitor::visit_printexpr(BST_PrintExpr* node) {
    stream << ":PRINT_EXPR(";
    visit_vreg(&node->vreg_value);
    stream << ")";
    return true;
}

bool PrintVisitor::visit_list(BST_List* node) {
    visit_vreg(&node->vreg_dst, true);
    stream << "[";
    for (int i = 0, n = node->num_elts; i < n; ++i) {
        if (i > 0)
            stream << ", ";
        visit_vreg(&node->elts[i]);
    }
    stream << "]";
    return true;
}

bool PrintVisitor::visit_name(BST_Name* node) {
    stream << node->id.s();
    stream << "(vreg" << node->vreg << ")";
#if 0
    if (node->lookup_type == ScopeInfo::VarScopeType::UNKNOWN)
        stream << "<U>";
    else if (node->lookup_type == ScopeInfo::VarScopeType::FAST)
        stream << "<F>";
    else if (node->lookup_type == ScopeInfo::VarScopeType::DEREF)
        stream << "<D>";
    else if (node->lookup_type == ScopeInfo::VarScopeType::CLOSURE)
        stream << "<C>";
    else if (node->lookup_type == ScopeInfo::VarScopeType::GLOBAL)
        stream << "<G>";
    else
        stream << "<?>";
#endif

#if 0
    if (node->is_kill) stream << "<k>";
#endif
    return false;
}



bool PrintVisitor::visit_num(BST_Num* node) {
    if (node->num_type == AST_Num::INT) {
        stream << node->n_int;
    } else if (node->num_type == AST_Num::LONG) {
        stream << node->n_long << "L";
    } else if (node->num_type == AST_Num::FLOAT) {
        stream << node->n_float;
    } else if (node->num_type == AST_Num::COMPLEX) {
        stream << node->n_float << "j";
    } else {
        RELEASE_ASSERT(0, "");
    }
    return false;
}

bool PrintVisitor::visit_print(BST_Print* node) {
    stream << "print ";
    if (node->vreg_dest != VREG_UNDEFINED) {
        stream << ">>";
        visit_vreg(&node->vreg_dest);
        stream << ", ";
    }
    if (node->vreg_value != VREG_UNDEFINED)
        visit_vreg(&node->vreg_value);
    if (!node->nl)
        stream << ",";
    return true;
}

bool PrintVisitor::visit_raise(BST_Raise* node) {
    stream << "raise";
    if (node->vreg_arg0 != VREG_UNDEFINED) {
        stream << " ";
        visit_vreg(&node->vreg_arg0);
    }
    if (node->vreg_arg1 != VREG_UNDEFINED) {
        stream << ", ";
        visit_vreg(&node->vreg_arg1);
    }
    if (node->vreg_arg2 != VREG_UNDEFINED) {
        stream << ", ";
        visit_vreg(&node->vreg_arg2);
    }
    return true;
}

bool PrintVisitor::visit_repr(BST_Repr* node) {
    visit_vreg(&node->vreg_dst, true);
    stream << "`";
    visit_vreg(&node->vreg_value);
    stream << "`";
    return true;
}

bool PrintVisitor::visit_return(BST_Return* node) {
    stream << "return ";
    if (node->vreg_value != VREG_UNDEFINED)
        visit_vreg(&node->vreg_value);
    return true;
}

bool PrintVisitor::visit_set(BST_Set* node) {
    visit_vreg(&node->vreg_dst, true);
    // An empty set literal is not writeable in Python (it's a dictionary),
    // but we sometimes generate it (ex in set comprehension lowering).
    // Just to make it clear when printing, print empty set literals as "SET{}".
    if (!node->num_elts)
        stream << "SET";

    stream << "{";

    bool first = true;
    for (int i = 0; i < node->num_elts; ++i) {
        if (!first)
            stream << ", ";
        first = false;

        visit_vreg(&node->num_elts[&i]);
    }

    stream << "}";
    return true;
}

bool PrintVisitor::visit_makeslice(BST_MakeSlice* node) {
    visit_vreg(&node->vreg_dst, true);
    stream << "<slice>(";
    if (node->vreg_lower != VREG_UNDEFINED)
        visit_vreg(&node->vreg_lower);
    if (node->vreg_upper != VREG_UNDEFINED || node->vreg_step != VREG_UNDEFINED)
        stream << ':';
    if (node->vreg_upper != VREG_UNDEFINED)
        visit_vreg(&node->vreg_upper);
    if (node->vreg_step != VREG_UNDEFINED) {
        stream << ':';
        visit_vreg(&node->vreg_step);
    }
    stream << ")";
    return true;
}

bool PrintVisitor::visit_loadname(BST_LoadName* node) {
    visit_vreg(&node->vreg_dst, true);
    stream << node->id.s() << "(vreg" << node->vreg << ")";
    return true;
}

bool PrintVisitor::visit_loadattr(BST_LoadAttr* node) {
    visit_vreg(&node->vreg_dst, true);
    visit_vreg(&node->vreg_value);
    stream << (node->clsonly ? ':' : '.') << node->attr.s();
    return true;
}

bool PrintVisitor::visit_loadsub(BST_LoadSub* node) {
    visit_vreg(&node->vreg_dst, true);
    visit_vreg(&node->vreg_value);
    stream << "<sub[";
    visit_vreg(&node->vreg_slice);
    stream << "]";
    return true;
}

bool PrintVisitor::visit_loadsubslice(BST_LoadSubSlice* node) {
    visit_vreg(&node->vreg_dst, true);
    visit_vreg(&node->vreg_value);
    stream << "<subslice[[";
    // TODO improve this
    visit_vreg(&node->vreg_lower);
    visit_vreg(&node->vreg_upper);
    stream << "]";
    return true;
}

bool PrintVisitor::visit_storename(BST_StoreName* node) {
    stream << node->id.s() << "(vreg" << node->vreg << ")" << " = ";
    visit_vreg(&node->vreg_value);
    return true;
}

bool PrintVisitor::visit_storeattr(BST_StoreAttr* node) {
    visit_vreg(&node->vreg_target);
    stream << "." << node->attr.s() << " = ";
    visit_vreg(&node->vreg_value);
    return true;
}

bool PrintVisitor::visit_storesub(BST_StoreSub* node) {
    visit_vreg(&node->vreg_target);
    stream << "<sub[";
    visit_vreg(&node->vreg_slice);
    stream << "] =";
    visit_vreg(&node->vreg_value);
    return true;
}

bool PrintVisitor::visit_storesubslice(BST_StoreSubSlice* node) {
    visit_vreg(&node->vreg_target);
    stream << "<subslice[";
    // TODO improve this
    visit_vreg(&node->vreg_lower);
    visit_vreg(&node->vreg_upper);
    stream << "] =";
    visit_vreg(&node->vreg_value);
    return true;
}

bool PrintVisitor::visit_str(BST_Str* node) {
    if (node->str_type == AST_Str::STR) {
        stream << "\"" << node->str_data << "\"";
    } else if (node->str_type == AST_Str::UNICODE) {
        stream << "<unicode value>";
    } else {
        RELEASE_ASSERT(0, "%d", node->str_type);
    }
    return false;
}
/*
bool PrintVisitor::visit_subscript(BST_Subscript* node) {
    node->value->accept(this);
    stream << "[";
    node->slice->accept(this);
    stream << "]";
    return true;
}
*/
bool PrintVisitor::visit_tuple(BST_Tuple* node) {
    visit_vreg(&node->vreg_dst, true);
    stream << "(";
    int n = node->num_elts;
    for (int i = 0; i < n; i++) {
        if (i)
            stream << ", ";
        visit_vreg(&node->elts[i]);
    }
    if (n == 1)
        stream << ",";
    stream << ")";
    return true;
}

bool PrintVisitor::visit_unaryop(BST_UnaryOp* node) {
    visit_vreg(&node->vreg_dst, true);
    switch (node->op_type) {
        case BST_TYPE::Invert:
            stream << "~";
            break;
        case BST_TYPE::Not:
            stream << "not ";
            break;
        case BST_TYPE::UAdd:
            stream << "+";
            break;
        case BST_TYPE::USub:
            stream << "-";
            break;
        default:
            RELEASE_ASSERT(0, "%s", getOpName(node->op_type)->c_str());
            break;
    }
    stream << "(";
    // node->operand->accept(this);
    visit_vreg(&node->vreg_operand);
    stream << ")";
    return true;
}

bool PrintVisitor::visit_unpackintoarray(BST_UnpackIntoArray* node) {
    stream << "(";
    for (int i = 0; i < node->num_elts; ++i) {
        visit_vreg(&node->vreg_dst[i]);
        if (i + 1 < node->num_elts || i == 0)
            stream << ", ";
    }
    stream << ") = ";

    visit_vreg(&node->vreg_src);
    return true;
}

bool PrintVisitor::visit_yield(BST_Yield* node) {
    visit_vreg(&node->vreg_dst, true);
    stream << "yield ";
    if (node->vreg_value != VREG_UNDEFINED)
        visit_vreg(&node->vreg_value);
    return true;
}

bool PrintVisitor::visit_branch(BST_Branch* node) {
    stream << "if ";
    visit_vreg(&node->vreg_test);
    stream << " goto " << node->iftrue->idx << " else goto " << node->iffalse->idx;
    return true;
}

bool PrintVisitor::visit_jump(BST_Jump* node) {
    stream << "goto " << node->target->idx;
    return true;
}

bool PrintVisitor::visit_makefunction(BST_MakeFunction* node) {
    visit_vreg(&node->vreg_dst, true);
    stream << "make_";
    return false;
}

bool PrintVisitor::visit_makeclass(BST_MakeClass* node) {
    visit_vreg(&node->vreg_dst, true);
    stream << "make_";
    return false;
}

namespace {
class FlattenVisitor : public BSTVisitor {
private:
    std::vector<BST*>* output;
    bool expand_scopes;

public:
    FlattenVisitor(std::vector<BST*>* output, bool expand_scopes) : output(output), expand_scopes(expand_scopes) {
        assert(expand_scopes && "not sure if this works properly");
    }

    virtual bool visit_vreg(int* vreg, bool is_dst = false) { return false; }


    virtual bool visit_assert(BST_Assert* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_assign(BST_Assign* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_assignvregvreg(BST_AssignVRegVReg* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_augbinop(BST_AugBinOp* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_binop(BST_BinOp* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_callfunc(BST_CallFunc* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_callattr(BST_CallAttr* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_callclsattr(BST_CallClsAttr* node) {
        output->push_back(node);
        return false;
    }

    virtual bool visit_classdef(BST_ClassDef* node) {
        output->push_back(node);
        return !expand_scopes;
    }
    virtual bool visit_compare(BST_Compare* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_deletesub(BST_DeleteSub* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_deletesubslice(BST_DeleteSubSlice* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_deleteattr(BST_DeleteAttr* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_deletename(BST_DeleteName* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_dict(BST_Dict* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_ellipsis(BST_Ellipsis* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_exec(BST_Exec* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_functiondef(BST_FunctionDef* node) {
        output->push_back(node);
        return !expand_scopes;
    }
    virtual bool visit_invoke(BST_Invoke* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_list(BST_List* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_name(BST_Name* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_num(BST_Num* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_print(BST_Print* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_raise(BST_Raise* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_repr(BST_Repr* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_return(BST_Return* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_set(BST_Set* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_str(BST_Str* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_tuple(BST_Tuple* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_unaryop(BST_UnaryOp* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_unpackintoarray(BST_UnpackIntoArray* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_yield(BST_Yield* node) {
        output->push_back(node);
        return false;
    }

    virtual bool visit_branch(BST_Branch* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_jump(BST_Jump* node) {
        output->push_back(node);
        return false;
    }

    virtual bool visit_makeclass(BST_MakeClass* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_makefunction(BST_MakeFunction* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_makeslice(BST_MakeSlice* node) {
        output->push_back(node);
        return false;
    }

    virtual bool visit_landingpad(BST_Landingpad* node) override {
        output->push_back(node);
        return false;
    }
    virtual bool visit_locals(BST_Locals* node) override {
        output->push_back(node);
        return false;
    }
    virtual bool visit_getiter(BST_GetIter* node) override {
        output->push_back(node);
        return false;
    }
    virtual bool visit_importfrom(BST_ImportFrom* node) override {
        output->push_back(node);
        return false;
    }
    virtual bool visit_importname(BST_ImportName* node) override {
        output->push_back(node);
        return false;
    }
    virtual bool visit_importstar(BST_ImportStar* node) override {
        output->push_back(node);
        return false;
    }
    virtual bool visit_none(BST_None* node) override {
        output->push_back(node);
        return false;
    }
    virtual bool visit_nonzero(BST_Nonzero* node) override {
        output->push_back(node);
        return false;
    }
    virtual bool visit_checkexcmatch(BST_CheckExcMatch* node) override {
        output->push_back(node);
        return false;
    }
    virtual bool visit_setexcinfo(BST_SetExcInfo* node) override {
        output->push_back(node);
        return false;
    }
    virtual bool visit_uncacheexcinfo(BST_UncacheExcInfo* node) override {
        output->push_back(node);
        return false;
    }
    virtual bool visit_hasnext(BST_HasNext* node) override {
        output->push_back(node);
        return false;
    }
    virtual bool visit_printexpr(BST_PrintExpr* node) override {
        output->push_back(node);
        return false;
    }
    virtual bool visit_loadname(BST_LoadName* node) override {
        output->push_back(node);
        return false;
    }
    virtual bool visit_loadattr(BST_LoadAttr* node) override {
        output->push_back(node);
        return false;
    }
    virtual bool visit_loadsub(BST_LoadSub* node) override {
        output->push_back(node);
        return false;
    }
    virtual bool visit_loadsubslice(BST_LoadSubSlice* node) override {
        output->push_back(node);
        return false;
    }
    virtual bool visit_storename(BST_StoreName* node) override {
        output->push_back(node);
        return false;
    }
    virtual bool visit_storesub(BST_StoreSub* node) override {
        output->push_back(node);
        return false;
    }
    virtual bool visit_storesubslice(BST_StoreSubSlice* node) override {
        output->push_back(node);
        return false;
    }
    virtual bool visit_storeattr(BST_StoreAttr* node) override {
        output->push_back(node);
        return false;
    }
};
}

void flatten(llvm::ArrayRef<BST_stmt*> roots, std::vector<BST*>& output, bool expand_scopes) {
    FlattenVisitor visitor(&output, expand_scopes);

    for (int i = 0; i < roots.size(); i++) {
        roots[i]->accept(&visitor);
    }
}

void flatten(BST_expr* root, std::vector<BST*>& output, bool expand_scopes) {
    FlattenVisitor visitor(&output, expand_scopes);

    root->accept(&visitor);
}
}
