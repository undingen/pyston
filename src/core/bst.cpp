// Copyright (c) 2014-2016 Dropbox, Inc.
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

void BST_arguments::accept(BSTVisitor* v) {
    bool skip = v->visit_arguments(this);
    if (skip)
        return;

    visitVector(defaults, v);
}

void BST_Assert::accept(BSTVisitor* v) {
    bool skip = v->visit_assert(this);
    if (skip)
        return;

    test->accept(v);
    if (msg)
        msg->accept(v);
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

void BST_AugBinOp::accept(BSTVisitor* v) {
    bool skip = v->visit_augbinop(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_left);
    v->visit_vreg(&vreg_right);
}

void* BST_AugBinOp::accept_expr(ExprVisitor* v) {
    return v->visit_augbinop(this);
}

void BST_Attribute::accept(BSTVisitor* v) {
    bool skip = v->visit_attribute(this);
    if (skip)
        return;

    value->accept(v);
}

void* BST_Attribute::accept_expr(ExprVisitor* v) {
    return v->visit_attribute(this);
}

void BST_BinOp::accept(BSTVisitor* v) {
    bool skip = v->visit_binop(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_left);
    v->visit_vreg(&vreg_right);
}

void* BST_BinOp::accept_expr(ExprVisitor* v) {
    return v->visit_binop(this);
}

void BST_Call::accept(BSTVisitor* v) {
    bool skip = v->visit_call(this);
    if (skip)
        return;

    func->accept(v);
    visitVector(args, v);
    visitVector(keywords, v);
    if (starargs)
        starargs->accept(v);
    if (kwargs)
        kwargs->accept(v);
}

void* BST_Call::accept_expr(ExprVisitor* v) {
    return v->visit_call(this);
}

void BST_Compare::accept(BSTVisitor* v) {
    bool skip = v->visit_compare(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_left);
    v->visit_vreg(&vreg_comparator);
}

void* BST_Compare::accept_expr(ExprVisitor* v) {
    return v->visit_compare(this);
}

void BST_ClassDef::accept(BSTVisitor* v) {
    bool skip = v->visit_classdef(this);
    if (skip)
        return;

    visitVector(this->bases, v);
    visitVector(this->decorator_list, v);
    visitCFG(this->code->source->cfg, v);
}

void BST_ClassDef::accept_stmt(StmtVisitor* v) {
    v->visit_classdef(this);
}

void BST_Delete::accept(BSTVisitor* v) {
    bool skip = v->visit_delete(this);
    if (skip)
        return;

    target->accept(v);
}

void BST_Delete::accept_stmt(StmtVisitor* v) {
    v->visit_delete(this);
}

void BST_Dict::accept(BSTVisitor* v) {
    bool skip = v->visit_dict(this);
    if (skip)
        return;

    for (int i = 0; i < keys.size(); i++) {
        keys[i]->accept(v);
        values[i]->accept(v);
    }
}

void* BST_Dict::accept_expr(ExprVisitor* v) {
    return v->visit_dict(this);
}

void BST_Ellipsis::accept(BSTVisitor* v) {
    bool skip = v->visit_ellipsis(this);
    if (skip)
        return;
}

void* BST_Ellipsis::accept_slice(SliceVisitor* v) {
    return v->visit_ellipsis(this);
}

void BST_Exec::accept(BSTVisitor* v) {
    bool skip = v->visit_exec(this);
    if (skip)
        return;

    if (body)
        body->accept(v);
    if (globals)
        globals->accept(v);
    if (locals)
        locals->accept(v);
}

void BST_Exec::accept_stmt(StmtVisitor* v) {
    v->visit_exec(this);
}

void BST_Expr::accept(BSTVisitor* v) {
    bool skip = v->visit_expr(this);
    if (skip)
        return;

    value->accept(v);
}

void BST_Expr::accept_stmt(StmtVisitor* v) {
    v->visit_expr(this);
}


void BST_ExtSlice::accept(BSTVisitor* v) {
    bool skip = v->visit_extslice(this);
    if (skip)
        return;
    visitVector(dims, v);
}

void* BST_ExtSlice::accept_slice(SliceVisitor* v) {
    return v->visit_extslice(this);
}

void BST_FunctionDef::accept(BSTVisitor* v) {
    bool skip = v->visit_functiondef(this);
    if (skip)
        return;

    visitVector(decorator_list, v);
    args->accept(v);
    visitCFG(code->source->cfg, v);
}

void BST_FunctionDef::accept_stmt(StmtVisitor* v) {
    v->visit_functiondef(this);
}

void BST_Index::accept(BSTVisitor* v) {
    bool skip = v->visit_index(this);
    if (skip)
        return;

    this->value->accept(v);
}

void* BST_Index::accept_slice(SliceVisitor* v) {
    return v->visit_index(this);
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

void BST_keyword::accept(BSTVisitor* v) {
    bool skip = v->visit_keyword(this);
    if (skip)
        return;

    value->accept(v);
}

void BST_LangPrimitive::accept(BSTVisitor* v) {
    bool skip = v->visit_langprimitive(this);
    if (skip)
        return;

    visitVector(args, v);
}

void* BST_LangPrimitive::accept_expr(ExprVisitor* v) {
    return v->visit_langprimitive(this);
}

void BST_List::accept(BSTVisitor* v) {
    bool skip = v->visit_list(this);
    if (skip)
        return;

    visitVector(elts, v);
}

void* BST_List::accept_expr(ExprVisitor* v) {
    return v->visit_list(this);
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

    if (dest)
        dest->accept(v);

    if (value)
        value->accept(v);
}

void BST_Print::accept_stmt(StmtVisitor* v) {
    v->visit_print(this);
}

void BST_Raise::accept(BSTVisitor* v) {
    bool skip = v->visit_raise(this);
    if (skip)
        return;

    if (arg0)
        arg0->accept(v);
    if (arg1)
        arg1->accept(v);
    if (arg2)
        arg2->accept(v);
}

void BST_Raise::accept_stmt(StmtVisitor* v) {
    v->visit_raise(this);
}

void BST_Repr::accept(BSTVisitor* v) {
    bool skip = v->visit_repr(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_value);
}

void* BST_Repr::accept_expr(ExprVisitor* v) {
    return v->visit_repr(this);
}

void BST_Return::accept(BSTVisitor* v) {
    bool skip = v->visit_return(this);
    if (skip)
        return;

    if (value)
        value->accept(v);
}

void BST_Return::accept_stmt(StmtVisitor* v) {
    v->visit_return(this);
}

void BST_Set::accept(BSTVisitor* v) {
    bool skip = v->visit_set(this);
    if (skip)
        return;

    visitVector(elts, v);
}

void* BST_Set::accept_expr(ExprVisitor* v) {
    return v->visit_set(this);
}

void BST_Slice::accept(BSTVisitor* v) {
    bool skip = v->visit_slice(this);
    if (skip)
        return;

    if (lower)
        lower->accept(v);
    if (upper)
        upper->accept(v);
    if (step)
        step->accept(v);
}

void* BST_Slice::accept_slice(SliceVisitor* v) {
    return v->visit_slice(this);
}

void BST_Str::accept(BSTVisitor* v) {
    bool skip = v->visit_str(this);
    if (skip)
        return;
}

void* BST_Str::accept_expr(ExprVisitor* v) {
    return v->visit_str(this);
}

void BST_Subscript::accept(BSTVisitor* v) {
    bool skip = v->visit_subscript(this);
    if (skip)
        return;

    this->value->accept(v);
    this->slice->accept(v);
}

void* BST_Subscript::accept_expr(ExprVisitor* v) {
    return v->visit_subscript(this);
}

void BST_Tuple::accept(BSTVisitor* v) {
    bool skip = v->visit_tuple(this);
    if (skip)
        return;

    visitVector(elts, v);
}

void* BST_Tuple::accept_expr(ExprVisitor* v) {
    return v->visit_tuple(this);
}

void BST_UnaryOp::accept(BSTVisitor* v) {
    bool skip = v->visit_unaryop(this);
    if (skip)
        return;

    v->visit_vreg(&vreg_operand);
}

void* BST_UnaryOp::accept_expr(ExprVisitor* v) {
    return v->visit_unaryop(this);
}

void BST_Yield::accept(BSTVisitor* v) {
    bool skip = v->visit_yield(this);
    if (skip)
        return;

    if (value)
        value->accept(v);
}

void* BST_Yield::accept_expr(ExprVisitor* v) {
    return v->visit_yield(this);
}

void BST_Branch::accept(BSTVisitor* v) {
    bool skip = v->visit_branch(this);
    if (skip)
        return;

    test->accept(v);
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

void BST_ClsAttribute::accept(BSTVisitor* v) {
    bool skip = v->visit_clsattribute(this);
    if (skip)
        return;

    value->accept(v);
}

void* BST_ClsAttribute::accept_expr(ExprVisitor* v) {
    return v->visit_clsattribute(this);
}

void BST_MakeFunction::accept(BSTVisitor* v) {
    bool skip = v->visit_makefunction(this);
    if (skip)
        return;

    function_def->accept(v);
}

void* BST_MakeFunction::accept_expr(ExprVisitor* v) {
    return v->visit_makefunction(this);
}

void BST_MakeClass::accept(BSTVisitor* v) {
    bool skip = v->visit_makeclass(this);
    if (skip)
        return;

    class_def->accept(v);
}

void* BST_MakeClass::accept_expr(ExprVisitor* v) {
    return v->visit_makeclass(this);
}

void print_bst(BST* bst) {
    PrintVisitor v;
    bst->accept(&v);
    v.flush();
}

void PrintVisitor::printIndent() {
    for (int i = 0; i < indent; i++) {
        stream << ' ';
    }
}

bool PrintVisitor::visit_arguments(BST_arguments* node) {
    int ndefault = node->defaults.size();
    for (int i = 0; i < ndefault; i++) {
        if (i > 0)
            stream << ", ";

        stream << "<default " << i << ">=";
        node->defaults[i]->accept(this);
    }
    return true;
}

bool PrintVisitor::visit_assert(BST_Assert* node) {
    stream << "assert ";
    node->test->accept(this);
    if (node->msg) {
        stream << ", ";
        node->msg->accept(this);
    }
    return true;
}

bool PrintVisitor::visit_assign(BST_Assign* node) {
    node->target->accept(this);
    stream << " = ";
    node->value->accept(this);
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
    stream << "#" << node->vreg_left;
    stream << '=';
    printOp(node->op_type);
    stream << "#" << node->vreg_right;
    return true;
}

bool PrintVisitor::visit_attribute(BST_Attribute* node) {
    node->value->accept(this);
    stream << '.';
    stream << node->attr.s();
    return true;
}

bool PrintVisitor::visit_binop(BST_BinOp* node) {
    stream << '#' << node->vreg_left;
    printOp(node->op_type);
    stream << '#' << node->vreg_right;
    return true;
}

bool PrintVisitor::visit_call(BST_Call* node) {
    node->func->accept(this);
    stream << "(";

    bool prevarg = false;
    for (int i = 0; i < node->args.size(); i++) {
        if (prevarg)
            stream << ", ";
        node->args[i]->accept(this);
        prevarg = true;
    }
    for (int i = 0; i < node->keywords.size(); i++) {
        if (prevarg)
            stream << ", ";
        node->keywords[i]->accept(this);
        prevarg = true;
    }
    if (node->starargs) {
        if (prevarg)
            stream << ", ";
        node->starargs->accept(this);
        prevarg = true;
    }
    if (node->kwargs) {
        if (prevarg)
            stream << ", ";
        node->kwargs->accept(this);
        prevarg = true;
    }
    stream << ")";
    return true;
}

bool PrintVisitor::visit_compare(BST_Compare* node) {
    stream << "#" << node->vreg_left;
    stream << " " << getOpSymbol(node->op) << " ";
    stream << "#" << node->vreg_comparator;

    return true;
}

bool PrintVisitor::visit_classdef(BST_ClassDef* node) {
    for (int i = 0, n = node->decorator_list.size(); i < n; i++) {
        stream << "@";
        node->decorator_list[i]->accept(this);
        stream << "\n";
        printIndent();
    }
    stream << "class " << node->name.s() << "(";
    for (int i = 0, n = node->bases.size(); i < n; i++) {
        if (i)
            stream << ", ";
        node->bases[i]->accept(this);
    }
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

bool PrintVisitor::visit_delete(BST_Delete* node) {
    stream << "del ";
    node->target->accept(this);
    return true;
}

bool PrintVisitor::visit_dict(BST_Dict* node) {
    stream << "{";
    for (int i = 0; i < node->keys.size(); i++) {
        if (i > 0)
            stream << ", ";
        node->keys[i]->accept(this);
        stream << ":";
        node->values[i]->accept(this);
    }
    stream << "}";
    return true;
}

bool PrintVisitor::visit_ellipsis(BST_Ellipsis*) {
    stream << "...";
    return true;
}

bool PrintVisitor::visit_exec(BST_Exec* node) {
    stream << "exec ";

    node->body->accept(this);
    if (node->globals) {
        stream << " in ";
        node->globals->accept(this);

        if (node->locals) {
            stream << ", ";
            node->locals->accept(this);
        }
    }
    stream << "\n";
    return true;
}

bool PrintVisitor::visit_expr(BST_Expr* node) {
    return false;
}

bool PrintVisitor::visit_extslice(BST_ExtSlice* node) {
    for (int i = 0; i < node->dims.size(); ++i) {
        if (i > 0)
            stream << ", ";
        node->dims[i]->accept(this);
    }
    return true;
}

bool PrintVisitor::visit_functiondef(BST_FunctionDef* node) {
    for (auto d : node->decorator_list) {
        stream << "@";
        d->accept(this);
        stream << "\n";
        printIndent();
    }

    stream << "def ";
    if (node->name != InternedString())
        stream << node->name.s();
    else
        stream << "<lambda>";
    stream << "(";
    node->args->accept(this);
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

bool PrintVisitor::visit_index(BST_Index* node) {
    return false;
}

bool PrintVisitor::visit_invoke(BST_Invoke* node) {
    stream << "invoke " << node->normal_dest->idx << " " << node->exc_dest->idx << ": ";
    node->stmt->accept(this);
    return true;
}

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

bool PrintVisitor::visit_list(BST_List* node) {
    stream << "[";
    for (int i = 0, n = node->elts.size(); i < n; ++i) {
        if (i > 0)
            stream << ", ";
        node->elts[i]->accept(this);
    }
    stream << "]";
    return true;
}

bool PrintVisitor::visit_keyword(BST_keyword* node) {
    stream << node->arg.s() << "=";
    node->value->accept(this);
    return true;
}

bool PrintVisitor::visit_name(BST_Name* node) {
    stream << node->id.s();
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
    if (node->dest) {
        stream << ">>";
        node->dest->accept(this);
        stream << ", ";
    }
    if (node->value)
        node->value->accept(this);
    if (!node->nl)
        stream << ",";
    return true;
}

bool PrintVisitor::visit_raise(BST_Raise* node) {
    stream << "raise";
    if (node->arg0) {
        stream << " ";
        node->arg0->accept(this);
    }
    if (node->arg1) {
        stream << ", ";
        node->arg1->accept(this);
    }
    if (node->arg2) {
        stream << ", ";
        node->arg2->accept(this);
    }
    return true;
}

bool PrintVisitor::visit_repr(BST_Repr* node) {
    stream << "`";
    stream << "#" << node->vreg_value;
    stream << "`";
    return true;
}

bool PrintVisitor::visit_return(BST_Return* node) {
    stream << "return ";
    return false;
}

bool PrintVisitor::visit_set(BST_Set* node) {
    // An empty set literal is not writeable in Python (it's a dictionary),
    // but we sometimes generate it (ex in set comprehension lowering).
    // Just to make it clear when printing, print empty set literals as "SET{}".
    if (!node->elts.size())
        stream << "SET";

    stream << "{";

    bool first = true;
    for (auto e : node->elts) {
        if (!first)
            stream << ", ";
        first = false;

        e->accept(this);
    }

    stream << "}";
    return true;
}

bool PrintVisitor::visit_slice(BST_Slice* node) {
    stream << "<slice>(";
    if (node->lower)
        node->lower->accept(this);
    if (node->upper || node->step)
        stream << ':';
    if (node->upper)
        node->upper->accept(this);
    if (node->step) {
        stream << ':';
        node->step->accept(this);
    }
    stream << ")";
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

bool PrintVisitor::visit_subscript(BST_Subscript* node) {
    node->value->accept(this);
    stream << "[";
    node->slice->accept(this);
    stream << "]";
    return true;
}

bool PrintVisitor::visit_tuple(BST_Tuple* node) {
    stream << "(";
    int n = node->elts.size();
    for (int i = 0; i < n; i++) {
        if (i)
            stream << ", ";
        node->elts[i]->accept(this);
    }
    if (n == 1)
        stream << ",";
    stream << ")";
    return true;
}

bool PrintVisitor::visit_unaryop(BST_UnaryOp* node) {
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
    stream << "#" << node->vreg_operand;
    stream << ")";
    return true;
}

bool PrintVisitor::visit_yield(BST_Yield* node) {
    stream << "yield ";
    if (node->value)
        node->value->accept(this);
    return true;
}

bool PrintVisitor::visit_branch(BST_Branch* node) {
    stream << "if ";
    node->test->accept(this);
    stream << " goto " << node->iftrue->idx << " else goto " << node->iffalse->idx;
    return true;
}

bool PrintVisitor::visit_jump(BST_Jump* node) {
    stream << "goto " << node->target->idx;
    return true;
}

bool PrintVisitor::visit_clsattribute(BST_ClsAttribute* node) {
    // printf("getclsattr(");
    // node->value->accept(this);
    // printf(", '%s')", node->attr.c_str());
    node->value->accept(this);
    stream << ":" << node->attr.s();
    return true;
}

bool PrintVisitor::visit_makefunction(BST_MakeFunction* node) {
    stream << "make_";
    return false;
}

bool PrintVisitor::visit_makeclass(BST_MakeClass* node) {
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

    virtual bool visit_vreg(int* vreg) { return false; }


    virtual bool visit_arguments(BST_arguments* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_assert(BST_Assert* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_assign(BST_Assign* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_augbinop(BST_AugBinOp* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_attribute(BST_Attribute* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_binop(BST_BinOp* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_call(BST_Call* node) {
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
    virtual bool visit_delete(BST_Delete* node) {
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
    virtual bool visit_expr(BST_Expr* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_extslice(BST_ExtSlice* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_functiondef(BST_FunctionDef* node) {
        output->push_back(node);
        return !expand_scopes;
    }
    virtual bool visit_index(BST_Index* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_invoke(BST_Invoke* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_keyword(BST_keyword* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_langprimitive(BST_LangPrimitive* node) {
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
    virtual bool visit_slice(BST_Slice* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_str(BST_Str* node) {
        output->push_back(node);
        return false;
    }
    virtual bool visit_subscript(BST_Subscript* node) {
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
    virtual bool visit_clsattribute(BST_ClsAttribute* node) {
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
