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

#ifndef PYSTON_CORE_BST_H
#define PYSTON_CORE_BST_H

#include <cassert>
#include <cstdlib>
#include <map>
#include <stdint.h>
#include <string>
#include <vector>

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include "analysis/scoping_analysis.h"
#include "core/ast.h"
#include "core/common.h"
#include "core/stringpool.h"

namespace pyston {

namespace BST_TYPE {

#define FOREACH_TYPE(X)                                                                                                \
    X(Assert, 1)                                                                                                       \
    X(AugBinOp, 2)                                                                                                     \
    X(BinOp, 3)                                                                                                        \
    X(Branch, 4)                                                                                                       \
    X(CallAttr, 5)                                                                                                     \
    X(CallClsAttr, 6)                                                                                                  \
    X(CallFunc, 7)                                                                                                     \
    X(CheckExcMatch, 8)                                                                                                \
    X(ClassDef, 9)                                                                                                     \
    X(Compare, 10)                                                                                                     \
    X(CopyVReg, 11)                                                                                                    \
    X(DeleteAttr, 12)                                                                                                  \
    X(DeleteName, 13)                                                                                                  \
    X(DeleteSub, 14)                                                                                                   \
    X(DeleteSubSlice, 15)                                                                                              \
    X(Dict, 16)                                                                                                        \
    X(Ellipsis, 17)                                                                                                    \
    X(Exec, 18)                                                                                                        \
    X(FunctionDef, 19)                                                                                                 \
    X(GetIter, 20)                                                                                                     \
    X(HasNext, 21)                                                                                                     \
    X(ImportFrom, 22)                                                                                                  \
    X(ImportName, 23)                                                                                                  \
    X(ImportStar, 24)                                                                                                  \
    X(Invoke, 25)                                                                                                      \
    X(Jump, 26)                                                                                                        \
    X(Landingpad, 27)                                                                                                  \
    X(List, 28)                                                                                                        \
    X(LoadAttr, 29)                                                                                                    \
    X(LoadName, 30)                                                                                                    \
    X(LoadSub, 31)                                                                                                     \
    X(LoadSubSlice, 32)                                                                                                \
    X(Locals, 33)                                                                                                      \
    X(MakeClass, 34)                                                                                                   \
    X(MakeFunction, 35)                                                                                                \
    X(MakeSlice, 36)                                                                                                   \
    X(Nonzero, 37)                                                                                                     \
    X(Print, 38)                                                                                                       \
    X(PrintExpr, 39)                                                                                                   \
    X(Raise, 40)                                                                                                       \
    X(Repr, 41)                                                                                                        \
    X(Return, 42)                                                                                                      \
    X(Set, 43)                                                                                                         \
    X(SetExcInfo, 44)                                                                                                  \
    X(StoreAttr, 45)                                                                                                   \
    X(StoreName, 46)                                                                                                   \
    X(StoreSub, 47)                                                                                                    \
    X(StoreSubSlice, 48)                                                                                               \
    X(Tuple, 49)                                                                                                       \
    X(UnaryOp, 50)                                                                                                     \
    X(UncacheExcInfo, 51)                                                                                              \
    X(UnpackIntoArray, 52)                                                                                             \
    X(Yield, 53)

#define GENERATE_ENUM(ENUM, N) ENUM = N,
#define GENERATE_STRING(STRING, N) m[N] = #STRING;

enum BST_TYPE : unsigned char { FOREACH_TYPE(GENERATE_ENUM) };

static const char* stringify(int n) {
    static std::map<int, const char*> m;
    FOREACH_TYPE(GENERATE_STRING)
    return m[n];
}

#undef FOREACH_TYPE
#undef GENERATE_ENUM
#undef GENERATE_STRING
};

class BSTVisitor;
class ExprVisitor;
class StmtVisitor;

static constexpr int VREG_UNDEFINED = std::numeric_limits<int>::min();

class BST {
public:
    virtual ~BST() {}

    const BST_TYPE::BST_TYPE type;
    uint32_t lineno;

    virtual void accept(BSTVisitor* v) = 0;

// #define DEBUG_LINE_NUMBERS 1
#ifdef DEBUG_LINE_NUMBERS
private:
    // Initialize lineno to something unique, so that if we see something ridiculous
    // appear in the traceback, we can isolate the allocation which created it.
    static int next_lineno;

public:
    BST(BST_TYPE::BST_TYPE type);
#else
    BST(BST_TYPE::BST_TYPE type) : type(type), lineno(0) {}
#endif
    BST(BST_TYPE::BST_TYPE type, uint32_t lineno) : type(type), lineno(lineno) {}

    virtual bool isBST_dst() const { return false; }
};

class BST_stmt : public BST {
public:
    virtual void accept_stmt(StmtVisitor* v) = 0;

    int cxx_exception_count = 0;

    BST_stmt(BST_TYPE::BST_TYPE type) : BST(type) {}
    BST_stmt(BST_TYPE::BST_TYPE type, uint32_t lineno) : BST(type, lineno) {}
};

// base class of all nodes which have a single destination vreg
class BST_dst : public BST_stmt {
public:
    int vreg_dst = VREG_UNDEFINED;
    BST_dst(BST_TYPE::BST_TYPE type) : BST_stmt(type) {}
    BST_dst(BST_TYPE::BST_TYPE type, int lineno) : BST_stmt(type, lineno) {}

    bool isBST_dst() const override { return true; }
};



class BST_Assert : public BST_stmt {
public:
    int vreg_msg = VREG_UNDEFINED;

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_Assert() : BST_stmt(BST_TYPE::Assert) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::Assert;
};

class BST_UnpackIntoArray : public BST_stmt {
public:
    int vreg_src = VREG_UNDEFINED;
    const int num_elts;
    int vreg_dst[1];

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    static BST_UnpackIntoArray* create(int num_elts) {
        BST_UnpackIntoArray* o
            = (BST_UnpackIntoArray*)new char[offsetof(BST_UnpackIntoArray, vreg_dst) + num_elts * sizeof(int)];
        new (o) BST_UnpackIntoArray(num_elts);
        return o;
    }

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::UnpackIntoArray;

private:
    BST_UnpackIntoArray(int num_elts) : BST_stmt(BST_TYPE::UnpackIntoArray), num_elts(num_elts) {
        for (int i = 0; i < num_elts; ++i) {
            vreg_dst[i] = VREG_UNDEFINED;
        }
    }
};

class BST_CopyVReg : public BST_dst {
public:
    int vreg_src = VREG_UNDEFINED; // this vreg will not get killed!

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_CopyVReg() : BST_dst(BST_TYPE::CopyVReg) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::CopyVReg;
};


class BST_StoreName : public BST_stmt {
public:
    int vreg_value = VREG_UNDEFINED;

    InternedString id;
    ScopeInfo::VarScopeType lookup_type;
    int vreg = VREG_UNDEFINED;

    // Only valid for lookup_type == DEREF:
    DerefInfo deref_info = DerefInfo({ INT_MAX, INT_MAX });
    // Only valid for lookup_type == CLOSURE:
    int closure_offset = -1;



    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_StoreName() : BST_stmt(BST_TYPE::StoreName) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::StoreName;
};

class BST_StoreAttr : public BST_stmt {
public:
    InternedString attr;
    int vreg_target = VREG_UNDEFINED;
    int vreg_value = VREG_UNDEFINED;

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_StoreAttr() : BST_stmt(BST_TYPE::StoreAttr) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::StoreAttr;
};

class BST_StoreSub : public BST_stmt {
public:
    int vreg_target = VREG_UNDEFINED;
    int vreg_slice = VREG_UNDEFINED;
    int vreg_value = VREG_UNDEFINED;

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_StoreSub() : BST_stmt(BST_TYPE::StoreSub) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::StoreSub;
};

class BST_StoreSubSlice : public BST_stmt {
public:
    int vreg_target = VREG_UNDEFINED;
    int vreg_lower = VREG_UNDEFINED, vreg_upper = VREG_UNDEFINED;
    int vreg_value = VREG_UNDEFINED;

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_StoreSubSlice() : BST_stmt(BST_TYPE::StoreSubSlice) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::StoreSubSlice;
};

class BST_LoadName : public BST_dst {
public:
    InternedString id;
    ScopeInfo::VarScopeType lookup_type;
    // LoadName does not kill this vreg
    int vreg = VREG_UNDEFINED;

    // Only valid for lookup_type == DEREF:
    DerefInfo deref_info = DerefInfo({ INT_MAX, INT_MAX });
    // Only valid for lookup_type == CLOSURE:
    int closure_offset = -1;

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_LoadName() : BST_dst(BST_TYPE::LoadName) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::LoadName;
};

class BST_LoadAttr : public BST_dst {
public:
    InternedString attr;
    int vreg_value = VREG_UNDEFINED;
    bool clsonly = false;

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_LoadAttr() : BST_dst(BST_TYPE::LoadAttr) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::LoadAttr;
};

class BST_LoadSub : public BST_dst {
public:
    int vreg_value = VREG_UNDEFINED;
    int vreg_slice = VREG_UNDEFINED;

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_LoadSub() : BST_dst(BST_TYPE::LoadSub) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::LoadSub;
};

class BST_LoadSubSlice : public BST_dst {
public:
    int vreg_value = VREG_UNDEFINED;
    int vreg_lower = VREG_UNDEFINED, vreg_upper = VREG_UNDEFINED;

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_LoadSubSlice() : BST_dst(BST_TYPE::LoadSubSlice) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::LoadSubSlice;
};

class BST_AugBinOp : public BST_dst {
public:
    AST_TYPE::AST_TYPE op_type;
    int vreg_left = VREG_UNDEFINED, vreg_right = VREG_UNDEFINED;

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_AugBinOp() : BST_dst(BST_TYPE::AugBinOp) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::AugBinOp;
};

class BST_BinOp : public BST_dst {
public:
    AST_TYPE::AST_TYPE op_type;
    int vreg_left = VREG_UNDEFINED, vreg_right = VREG_UNDEFINED;

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_BinOp() : BST_dst(BST_TYPE::BinOp) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::BinOp;
};

class BST_Call : public BST_dst {
public:
    int vreg_starargs = VREG_UNDEFINED, vreg_kwargs = VREG_UNDEFINED;
    const int num_args;
    const int num_keywords;

    // used during execution stores all keyword names
    std::unique_ptr<std::vector<BoxedString*>> keywords_names;

    BST_Call(BST_TYPE::BST_TYPE type, int num_args, int num_keywords)
        : BST_dst(type), num_args(num_args), num_keywords(num_keywords) {}
};

class BST_CallFunc : public BST_Call {
public:
    int vreg_func = VREG_UNDEFINED;
    int elts[1];

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    static BST_CallFunc* create(int num_args, int num_keywords) {
        BST_CallFunc* o
            = (BST_CallFunc*)new char[offsetof(BST_CallFunc, elts) + (num_args + num_keywords) * sizeof(int)];
        new (o) BST_CallFunc(num_args, num_keywords);
        return o;
    }

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::CallFunc;

private:
    BST_CallFunc(int num_args, int num_keywords) : BST_Call(BST_TYPE::CallFunc, num_args, num_keywords) {
        for (int i = 0; i < num_args + num_keywords; ++i) {
            elts[i] = VREG_UNDEFINED;
        }
    }
};

class BST_CallAttr : public BST_Call {
public:
    int vreg_value = VREG_UNDEFINED;
    InternedString attr;
    int elts[1];

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    static BST_CallAttr* create(int num_args, int num_keywords) {
        BST_CallAttr* o
            = (BST_CallAttr*)new char[offsetof(BST_CallAttr, elts) + (num_args + num_keywords) * sizeof(int)];
        new (o) BST_CallAttr(num_args, num_keywords);
        return o;
    }

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::CallAttr;

private:
    BST_CallAttr(int num_args, int num_keywords) : BST_Call(BST_TYPE::CallAttr, num_args, num_keywords) {
        for (int i = 0; i < num_args + num_keywords; ++i) {
            elts[i] = VREG_UNDEFINED;
        }
    }
};

class BST_CallClsAttr : public BST_Call {
public:
    int vreg_value = VREG_UNDEFINED;
    InternedString attr;
    int elts[1];

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    static BST_CallClsAttr* create(int num_args, int num_keywords) {
        BST_CallClsAttr* o
            = (BST_CallClsAttr*)new char[offsetof(BST_CallClsAttr, elts) + (num_args + num_keywords) * sizeof(int)];
        new (o) BST_CallClsAttr(num_args, num_keywords);
        return o;
    }

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::CallClsAttr;

private:
    BST_CallClsAttr(int num_args, int num_keywords) : BST_Call(BST_TYPE::CallClsAttr, num_args, num_keywords) {
        for (int i = 0; i < num_args + num_keywords; ++i) {
            elts[i] = VREG_UNDEFINED;
        }
    }
};


class BST_Compare : public BST_dst {
public:
    AST_TYPE::AST_TYPE op;
    int vreg_comparator = VREG_UNDEFINED;
    int vreg_left = VREG_UNDEFINED;

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_Compare() : BST_dst(BST_TYPE::Compare) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::Compare;
};

class BST_ClassDef : public BST_stmt {
public:
    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BoxedCode* code;

    InternedString name;
    int vreg_bases_tuple;
    const int num_decorator;
    int decorator[1];



    static BST_ClassDef* create(int num_decorator) {
        BST_ClassDef* o = (BST_ClassDef*)new char[offsetof(BST_ClassDef, decorator) + (num_decorator) * sizeof(int)];
        new (o) BST_ClassDef(num_decorator);
        return o;
    }

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::ClassDef;


private:
    BST_ClassDef(int num_decorator) : BST_stmt(BST_TYPE::ClassDef), num_decorator(num_decorator) {
        for (int i = 0; i < num_decorator; ++i) {
            decorator[i] = VREG_UNDEFINED;
        }
    }
};

class BST_Dict : public BST_dst {
public:
    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_Dict() : BST_dst(BST_TYPE::Dict) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::Dict;
};

class BST_DeleteAttr : public BST_stmt {
public:
    int vreg_value = VREG_UNDEFINED;
    InternedString attr;

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_DeleteAttr() : BST_stmt(BST_TYPE::DeleteAttr) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::DeleteAttr;
};

class BST_DeleteName : public BST_stmt {
public:
    InternedString id;
    ScopeInfo::VarScopeType lookup_type;
    int vreg = VREG_UNDEFINED;

    // Only valid for lookup_type == DEREF:
    DerefInfo deref_info = DerefInfo({ INT_MAX, INT_MAX });
    // Only valid for lookup_type == CLOSURE:
    int closure_offset = -1;


    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_DeleteName() : BST_stmt(BST_TYPE::DeleteName) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::DeleteName;
};

class BST_DeleteSub : public BST_stmt {
public:
    int vreg_value = VREG_UNDEFINED;
    int vreg_slice = VREG_UNDEFINED;

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_DeleteSub() : BST_stmt(BST_TYPE::DeleteSub) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::DeleteSub;
};

class BST_DeleteSubSlice : public BST_stmt {
public:
    int vreg_value = VREG_UNDEFINED;
    int vreg_lower = VREG_UNDEFINED;
    int vreg_upper = VREG_UNDEFINED;

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_DeleteSubSlice() : BST_stmt(BST_TYPE::DeleteSubSlice) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::DeleteSubSlice;
};

class BST_Ellipsis : public BST_dst {
public:
    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_Ellipsis() : BST_dst(BST_TYPE::Ellipsis) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::Ellipsis;
};

class BST_Exec : public BST_stmt {
public:
    int vreg_body = VREG_UNDEFINED;
    int vreg_globals = VREG_UNDEFINED;
    int vreg_locals = VREG_UNDEFINED;

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_Exec() : BST_stmt(BST_TYPE::Exec) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::Exec;
};

class BST_FunctionDef : public BST_stmt {
public:
    InternedString name; // if the name is not set this is a lambda

    BoxedCode* code;

    const int num_decorator;
    const int num_defaults;

    int elts[1]; // decorators followed by defaults

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    static BST_FunctionDef* create(int num_decorator, int num_defaults) {
        BST_FunctionDef* o = (BST_FunctionDef*)new char[offsetof(BST_FunctionDef, elts)
                                                        + (num_decorator + num_defaults) * sizeof(int)];
        new (o) BST_FunctionDef(num_decorator, num_defaults);
        return o;
    }

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::FunctionDef;

private:
    BST_FunctionDef(int num_decorator, int num_defaults)
        : BST_stmt(BST_TYPE::FunctionDef), num_decorator(num_decorator), num_defaults(num_defaults) {
        for (int i = 0; i < num_decorator + num_defaults; ++i) {
            elts[i] = VREG_UNDEFINED;
        }
    }
};

class BST_List : public BST_dst {
public:
    const int num_elts;
    int elts[1];

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    static BST_List* create(int num_elts) {
        BST_List* o = (BST_List*)new char[offsetof(BST_List, elts) + num_elts * sizeof(int)];
        new (o) BST_List(num_elts);
        return o;
    }

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::List;

private:
    BST_List(int num_elts) : BST_dst(BST_TYPE::List), num_elts(num_elts) {
        for (int i = 0; i < num_elts; ++i) {
            elts[i] = VREG_UNDEFINED;
        }
    }
};

class BST_Repr : public BST_dst {
public:
    int vreg_value = VREG_UNDEFINED;

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_Repr() : BST_dst(BST_TYPE::Repr) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::Repr;
};

class BST_Print : public BST_stmt {
public:
    int vreg_dest = VREG_UNDEFINED;
    bool nl;
    int vreg_value = VREG_UNDEFINED;

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_Print() : BST_stmt(BST_TYPE::Print) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::Print;
};

class BST_Raise : public BST_stmt {
public:
    // In the python ast module, these are called "type", "inst", and "tback", respectively.
    // Renaming to arg{0..2} since I find that confusing, since they are filled in
    // sequentially rather than semantically.
    // Ie "raise Exception()" will have type==Exception(), inst==None, tback==None
    int vreg_arg0 = VREG_UNDEFINED, vreg_arg1 = VREG_UNDEFINED, vreg_arg2 = VREG_UNDEFINED;

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_Raise() : BST_stmt(BST_TYPE::Raise) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::Raise;
};

class BST_Return : public BST_stmt {
public:
    int vreg_value = VREG_UNDEFINED;

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_Return() : BST_stmt(BST_TYPE::Return) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::Return;
};

class BST_Set : public BST_dst {
public:
    const int num_elts;
    int elts[1];

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    static BST_Set* create(int num_elts) {
        BST_Set* o = (BST_Set*)new char[offsetof(BST_Set, elts) + num_elts * sizeof(int)];
        new (o) BST_Set(num_elts);
        return o;
    }

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::Set;

private:
    BST_Set(int num_elts) : BST_dst(BST_TYPE::Set), num_elts(num_elts) {
        for (int i = 0; i < num_elts; ++i) {
            elts[i] = VREG_UNDEFINED;
        }
    }
};

class BST_MakeSlice : public BST_dst {
public:
    int vreg_lower = VREG_UNDEFINED, vreg_upper = VREG_UNDEFINED, vreg_step = VREG_UNDEFINED;

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_MakeSlice() : BST_dst(BST_TYPE::MakeSlice) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::MakeSlice;
};

class BST_Tuple : public BST_dst {
public:
    const int num_elts;
    int elts[1];

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    static BST_Tuple* create(int num_elts) {
        BST_Tuple* o = (BST_Tuple*)new char[offsetof(BST_Tuple, elts) + num_elts * sizeof(int)];
        new (o) BST_Tuple(num_elts);
        return o;
    }

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::Tuple;

private:
    BST_Tuple(int num_elts) : BST_dst(BST_TYPE::Tuple), num_elts(num_elts) {
        for (int i = 0; i < num_elts; ++i) {
            elts[i] = VREG_UNDEFINED;
        }
    }
};

class BST_UnaryOp : public BST_dst {
public:
    int vreg_operand = VREG_UNDEFINED;
    AST_TYPE::AST_TYPE op_type;

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_UnaryOp() : BST_dst(BST_TYPE::UnaryOp) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::UnaryOp;
};

class BST_Yield : public BST_dst {
public:
    int vreg_value = VREG_UNDEFINED;

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_Yield() : BST_dst(BST_TYPE::Yield) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::Yield;
};

class BST_MakeFunction : public BST_dst {
public:
    BST_FunctionDef* function_def;

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_MakeFunction(BST_FunctionDef* fd) : BST_dst(BST_TYPE::MakeFunction, fd->lineno), function_def(fd) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::MakeFunction;
};

class BST_MakeClass : public BST_dst {
public:
    BST_ClassDef* class_def;

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_MakeClass(BST_ClassDef* cd) : BST_dst(BST_TYPE::MakeClass, cd->lineno), class_def(cd) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::MakeClass;
};


// BST pseudo-nodes that will get added during CFG-construction.  These don't exist in the input BST, but adding them in
// lets us avoid creating a completely new IR for this phase

class CFGBlock;

class BST_Branch : public BST_stmt {
public:
    int vreg_test = VREG_UNDEFINED;
    CFGBlock* iftrue, *iffalse;

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_Branch() : BST_stmt(BST_TYPE::Branch) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::Branch;
};

class BST_Jump : public BST_stmt {
public:
    CFGBlock* target;

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_Jump() : BST_stmt(BST_TYPE::Jump) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::Jump;
};

class BST_Invoke : public BST_stmt {
public:
    BST_stmt* stmt;

    CFGBlock* normal_dest, *exc_dest;

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_Invoke(BST_stmt* stmt) : BST_stmt(BST_TYPE::Invoke), stmt(stmt) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::Invoke;
};


// grabs the info about the last raised exception
class BST_Landingpad : public BST_dst {
public:
    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_Landingpad() : BST_dst(BST_TYPE::Landingpad) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::Landingpad;
};

class BST_Locals : public BST_dst {
public:
    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_Locals() : BST_dst(BST_TYPE::Locals) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::Locals;
};

class BST_GetIter : public BST_dst {
public:
    int vreg_value = VREG_UNDEFINED;

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_GetIter() : BST_dst(BST_TYPE::GetIter) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::GetIter;
};

class BST_ImportFrom : public BST_dst {
public:
    int vreg_module = VREG_UNDEFINED;
    int vreg_name = VREG_UNDEFINED;

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_ImportFrom() : BST_dst(BST_TYPE::ImportFrom) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::ImportFrom;
};

class BST_ImportName : public BST_dst {
public:
    int vreg_from = VREG_UNDEFINED;
    int level = VREG_UNDEFINED;
    int vreg_name = VREG_UNDEFINED;


    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_ImportName() : BST_dst(BST_TYPE::ImportName) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::ImportName;
};

class BST_ImportStar : public BST_dst {
public:
    int vreg_name = VREG_UNDEFINED;

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_ImportStar() : BST_dst(BST_TYPE::ImportStar) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::ImportStar;
};

// determines whether something is "true" for purposes of `if' and so forth
class BST_Nonzero : public BST_dst {
public:
    int vreg_value = VREG_UNDEFINED;

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_Nonzero() : BST_dst(BST_TYPE::Nonzero) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::Nonzero;
};

class BST_CheckExcMatch : public BST_dst {
public:
    int vreg_value = VREG_UNDEFINED;
    int vreg_cls = VREG_UNDEFINED;

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_CheckExcMatch() : BST_dst(BST_TYPE::CheckExcMatch) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::CheckExcMatch;
};

class BST_SetExcInfo : public BST_stmt {
public:
    int vreg_type = VREG_UNDEFINED;
    int vreg_value = VREG_UNDEFINED;
    int vreg_traceback = VREG_UNDEFINED;

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_SetExcInfo() : BST_stmt(BST_TYPE::SetExcInfo) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::SetExcInfo;
};

class BST_UncacheExcInfo : public BST_stmt {
public:
    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_UncacheExcInfo() : BST_stmt(BST_TYPE::UncacheExcInfo) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::UncacheExcInfo;
};

class BST_HasNext : public BST_dst {
public:
    int vreg_value = VREG_UNDEFINED;

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_HasNext() : BST_dst(BST_TYPE::HasNext) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::HasNext;
};

class BST_PrintExpr : public BST_stmt {
public:
    int vreg_value = VREG_UNDEFINED;

    virtual void accept(BSTVisitor* v);
    virtual void accept_stmt(StmtVisitor* v);

    BST_PrintExpr() : BST_stmt(BST_TYPE::PrintExpr) {}

    static const BST_TYPE::BST_TYPE TYPE = BST_TYPE::PrintExpr;
};


// this is not a real bytecode it's only used to initalize arguments
class BST_Name {
public:
    InternedString id;

    // The resolved scope of this name.  Kind of hacky to be storing it in the BST node;
    // in CPython it ends up getting "cached" by being translated into one of a number of
    // different bytecodes.
    ScopeInfo::VarScopeType lookup_type;

    // These are only valid for lookup_type == FAST or CLOSURE
    // The interpreter and baseline JIT store variables with FAST and CLOSURE scopes in an array (vregs) this specifies
    // the zero based index of this variable inside the vregs array. If uninitialized it's value is VREG_UNDEFINED.
    int vreg;

    // Only valid for lookup_type == CLOSURE:
    int closure_offset = -1;

    BST_Name(InternedString id, int lineno)
        : id(id), lookup_type(ScopeInfo::VarScopeType::UNKNOWN), vreg(VREG_UNDEFINED) {}
};

template <typename T> T* bst_cast(BST* node) {
    ASSERT(!node || node->type == T::TYPE, "%d", node ? node->type : 0);
    return static_cast<T*>(node);
}


class BSTVisitor {
public:
    const bool skip_visit_child_cfg;
    // if skip_visit_child_cfg is set function and class defs will not visit their body nodes.
    BSTVisitor(bool skip_visit_child_cfg) : skip_visit_child_cfg(skip_visit_child_cfg) {}
    virtual ~BSTVisitor() {}

    // pseudo
    virtual bool visit_vreg(int* vreg, bool is_dst = false) { RELEASE_ASSERT(0, ""); }

    virtual bool visit_assert(BST_Assert* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_augbinop(BST_AugBinOp* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_binop(BST_BinOp* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_branch(BST_Branch* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_callattr(BST_CallAttr* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_callclsattr(BST_CallClsAttr* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_callfunc(BST_CallFunc* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_checkexcmatch(BST_CheckExcMatch* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_classdef(BST_ClassDef* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_compare(BST_Compare* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_copyvreg(BST_CopyVReg* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_deleteattr(BST_DeleteAttr* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_deletename(BST_DeleteName* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_deletesub(BST_DeleteSub* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_deletesubslice(BST_DeleteSubSlice* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_dict(BST_Dict* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_ellipsis(BST_Ellipsis* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_exec(BST_Exec* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_functiondef(BST_FunctionDef* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_getiter(BST_GetIter* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_hasnext(BST_HasNext* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_importfrom(BST_ImportFrom* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_importname(BST_ImportName* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_importstar(BST_ImportStar* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_invoke(BST_Invoke* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_jump(BST_Jump* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_landingpad(BST_Landingpad* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_list(BST_List* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_loadattr(BST_LoadAttr* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_loadname(BST_LoadName* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_loadsub(BST_LoadSub* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_loadsubslice(BST_LoadSubSlice* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_locals(BST_Locals* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_makeclass(BST_MakeClass* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_makefunction(BST_MakeFunction* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_makeslice(BST_MakeSlice* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_nonzero(BST_Nonzero* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_print(BST_Print* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_printexpr(BST_PrintExpr* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_raise(BST_Raise* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_repr(BST_Repr* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_return(BST_Return* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_set(BST_Set* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_setexcinfo(BST_SetExcInfo* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_storeattr(BST_StoreAttr* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_storename(BST_StoreName* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_storesub(BST_StoreSub* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_storesubslice(BST_StoreSubSlice* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_tuple(BST_Tuple* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_unaryop(BST_UnaryOp* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_uncacheexcinfo(BST_UncacheExcInfo* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_unpackintoarray(BST_UnpackIntoArray* node) { RELEASE_ASSERT(0, ""); }
    virtual bool visit_yield(BST_Yield* node) { RELEASE_ASSERT(0, ""); }
};

class NoopBSTVisitor : public BSTVisitor {
protected:
public:
    NoopBSTVisitor(bool skip_visit_child_cfg) : BSTVisitor(skip_visit_child_cfg) {}
    virtual ~NoopBSTVisitor() {}

    virtual bool visit_assert(BST_Assert* node) { return false; }
    virtual bool visit_augbinop(BST_AugBinOp* node) { return false; }
    virtual bool visit_binop(BST_BinOp* node) { return false; }
    virtual bool visit_branch(BST_Branch* node) { return false; }
    virtual bool visit_callattr(BST_CallAttr* node) { return false; }
    virtual bool visit_callclsattr(BST_CallClsAttr* node) { return false; }
    virtual bool visit_callfunc(BST_CallFunc* node) { return false; }
    virtual bool visit_checkexcmatch(BST_CheckExcMatch* node) override { return false; }
    virtual bool visit_classdef(BST_ClassDef* node) { return false; }
    virtual bool visit_compare(BST_Compare* node) { return false; }
    virtual bool visit_copyvreg(BST_CopyVReg* node) { return false; }
    virtual bool visit_deleteattr(BST_DeleteAttr* node) { return false; }
    virtual bool visit_deletename(BST_DeleteName* node) { return false; }
    virtual bool visit_deletesub(BST_DeleteSub* node) { return false; }
    virtual bool visit_deletesubslice(BST_DeleteSubSlice* node) { return false; }
    virtual bool visit_dict(BST_Dict* node) { return false; }
    virtual bool visit_ellipsis(BST_Ellipsis* node) { return false; }
    virtual bool visit_exec(BST_Exec* node) { return false; }
    virtual bool visit_functiondef(BST_FunctionDef* node) { return false; }
    virtual bool visit_getiter(BST_GetIter* node) override { return false; }
    virtual bool visit_hasnext(BST_HasNext* node) override { return false; }
    virtual bool visit_importfrom(BST_ImportFrom* node) override { return false; }
    virtual bool visit_importname(BST_ImportName* node) override { return false; }
    virtual bool visit_importstar(BST_ImportStar* node) override { return false; }
    virtual bool visit_invoke(BST_Invoke* node) { return false; }
    virtual bool visit_jump(BST_Jump* node) { return false; }
    virtual bool visit_landingpad(BST_Landingpad* node) override { return false; }
    virtual bool visit_list(BST_List* node) { return false; }
    virtual bool visit_loadattr(BST_LoadAttr* node) override { return false; }
    virtual bool visit_loadname(BST_LoadName* node) override { return false; }
    virtual bool visit_loadsub(BST_LoadSub* node) override { return false; }
    virtual bool visit_loadsubslice(BST_LoadSubSlice* node) override { return false; }
    virtual bool visit_locals(BST_Locals* node) override { return false; }
    virtual bool visit_makeclass(BST_MakeClass* node) { return false; }
    virtual bool visit_makefunction(BST_MakeFunction* node) { return false; }
    virtual bool visit_makeslice(BST_MakeSlice* node) { return false; }
    virtual bool visit_nonzero(BST_Nonzero* node) override { return false; }
    virtual bool visit_print(BST_Print* node) { return false; }
    virtual bool visit_printexpr(BST_PrintExpr* node) override { return false; }
    virtual bool visit_raise(BST_Raise* node) { return false; }
    virtual bool visit_repr(BST_Repr* node) { return false; }
    virtual bool visit_return(BST_Return* node) { return false; }
    virtual bool visit_set(BST_Set* node) { return false; }
    virtual bool visit_setexcinfo(BST_SetExcInfo* node) override { return false; }
    virtual bool visit_storeattr(BST_StoreAttr* node) { return false; }
    virtual bool visit_storename(BST_StoreName* node) { return false; }
    virtual bool visit_storesub(BST_StoreSub* node) override { return false; }
    virtual bool visit_storesubslice(BST_StoreSubSlice* node) override { return false; }
    virtual bool visit_tuple(BST_Tuple* node) { return false; }
    virtual bool visit_unaryop(BST_UnaryOp* node) { return false; }
    virtual bool visit_uncacheexcinfo(BST_UncacheExcInfo* node) override { return false; }
    virtual bool visit_unpackintoarray(BST_UnpackIntoArray* node) { return false; }
    virtual bool visit_yield(BST_Yield* node) { return false; }
};

class StmtVisitor {
protected:
public:
    virtual ~StmtVisitor() {}

    virtual void visit_assert(BST_Assert* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_augbinop(BST_AugBinOp* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_binop(BST_BinOp* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_branch(BST_Branch* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_callattr(BST_CallAttr* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_callclsattr(BST_CallClsAttr* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_callfunc(BST_CallFunc* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_checkexcmatch(BST_CheckExcMatch* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_classdef(BST_ClassDef* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_compare(BST_Compare* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_copyvreg(BST_CopyVReg* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_deleteattr(BST_DeleteAttr* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_deletename(BST_DeleteName* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_deletesub(BST_DeleteSub* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_deletesubslice(BST_DeleteSubSlice* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_dict(BST_Dict* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_ellipsis(BST_Ellipsis* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_exec(BST_Exec* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_functiondef(BST_FunctionDef* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_getiter(BST_GetIter* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_hasnext(BST_HasNext* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_importfrom(BST_ImportFrom* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_importname(BST_ImportName* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_importstar(BST_ImportStar* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_invoke(BST_Invoke* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_jump(BST_Jump* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_landingpad(BST_Landingpad* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_list(BST_List* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_loadattr(BST_LoadAttr* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_loadname(BST_LoadName* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_loadsub(BST_LoadSub* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_loadsubslice(BST_LoadSubSlice* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_locals(BST_Locals* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_makeclass(BST_MakeClass* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_makefunction(BST_MakeFunction* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_makeslice(BST_MakeSlice* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_nonzero(BST_Nonzero* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_print(BST_Print* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_printexpr(BST_PrintExpr* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_raise(BST_Raise* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_repr(BST_Repr* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_return(BST_Return* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_set(BST_Set* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_setexcinfo(BST_SetExcInfo* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_storeattr(BST_StoreAttr* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_storename(BST_StoreName* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_storesub(BST_StoreSub* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_storesubslice(BST_StoreSubSlice* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_tuple(BST_Tuple* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_unaryop(BST_UnaryOp* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_uncacheexcinfo(BST_UncacheExcInfo* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_unpackintoarray(BST_UnpackIntoArray* node) { RELEASE_ASSERT(0, ""); }
    virtual void visit_yield(BST_Yield* node) { RELEASE_ASSERT(0, ""); }
};

void print_bst(BST* bst);
class PrintVisitor : public BSTVisitor {
private:
    llvm::raw_ostream& stream;
    int indent;
    BoxedModule* mod;
    void printIndent();
    void printOp(AST_TYPE::AST_TYPE op_type);

public:
    PrintVisitor(int indent, llvm::raw_ostream& stream, BoxedModule* mod)
        : BSTVisitor(false /* visit child CFG */), stream(stream), indent(indent), mod(mod) {}
    virtual ~PrintVisitor() {}
    void flush() { stream.flush(); }

    virtual bool visit_vreg(int* vreg, bool is_dst = false);

    virtual bool visit_assert(BST_Assert* node);
    virtual bool visit_augbinop(BST_AugBinOp* node);
    virtual bool visit_binop(BST_BinOp* node);
    virtual bool visit_branch(BST_Branch* node);
    virtual bool visit_callattr(BST_CallAttr* node);
    virtual bool visit_callclsattr(BST_CallClsAttr* node);
    virtual bool visit_callfunc(BST_CallFunc* node);
    virtual bool visit_checkexcmatch(BST_CheckExcMatch* node);
    virtual bool visit_classdef(BST_ClassDef* node);
    virtual bool visit_compare(BST_Compare* node);
    virtual bool visit_copyvreg(BST_CopyVReg* node);
    virtual bool visit_deleteattr(BST_DeleteAttr* node);
    virtual bool visit_deletename(BST_DeleteName* node);
    virtual bool visit_deletesub(BST_DeleteSub* node);
    virtual bool visit_deletesubslice(BST_DeleteSubSlice* node);
    virtual bool visit_dict(BST_Dict* node);
    virtual bool visit_ellipsis(BST_Ellipsis* node);
    virtual bool visit_exec(BST_Exec* node);
    virtual bool visit_functiondef(BST_FunctionDef* node);
    virtual bool visit_getiter(BST_GetIter* node);
    virtual bool visit_hasnext(BST_HasNext* node);
    virtual bool visit_importfrom(BST_ImportFrom* node);
    virtual bool visit_importname(BST_ImportName* node);
    virtual bool visit_importstar(BST_ImportStar* node);
    virtual bool visit_invoke(BST_Invoke* node);
    virtual bool visit_jump(BST_Jump* node);
    virtual bool visit_landingpad(BST_Landingpad* node);
    virtual bool visit_list(BST_List* node);
    virtual bool visit_loadattr(BST_LoadAttr* node);
    virtual bool visit_loadname(BST_LoadName* node);
    virtual bool visit_loadsub(BST_LoadSub* node);
    virtual bool visit_loadsubslice(BST_LoadSubSlice* node);
    virtual bool visit_locals(BST_Locals* node);
    virtual bool visit_makeclass(BST_MakeClass* node);
    virtual bool visit_makefunction(BST_MakeFunction* node);
    virtual bool visit_makeslice(BST_MakeSlice* node);
    virtual bool visit_nonzero(BST_Nonzero* node);
    virtual bool visit_print(BST_Print* node);
    virtual bool visit_printexpr(BST_PrintExpr* node);
    virtual bool visit_raise(BST_Raise* node);
    virtual bool visit_repr(BST_Repr* node);
    virtual bool visit_return(BST_Return* node);
    virtual bool visit_set(BST_Set* node);
    virtual bool visit_setexcinfo(BST_SetExcInfo* node);
    virtual bool visit_storeattr(BST_StoreAttr* node);
    virtual bool visit_storename(BST_StoreName* node);
    virtual bool visit_storesub(BST_StoreSub* node);
    virtual bool visit_storesubslice(BST_StoreSubSlice* node);
    virtual bool visit_tuple(BST_Tuple* node);
    virtual bool visit_unaryop(BST_UnaryOp* node);
    virtual bool visit_uncacheexcinfo(BST_UncacheExcInfo* node);
    virtual bool visit_unpackintoarray(BST_UnpackIntoArray* node);
    virtual bool visit_yield(BST_Yield* node);
};

// Given an BST node, return a vector of the node plus all its descendents.
// This is useful for analyses that care more about the constituent nodes than the
// exact tree structure; ex, finding all "global" directives.
void flatten(llvm::ArrayRef<BST_stmt*> roots, std::vector<BST*>& output, bool expand_scopes);
};

#endif
