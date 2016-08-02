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

#include "runtime/inline/boxing.h"

#include "llvm/ADT/SmallString.h"

#include "runtime/int.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

namespace pyston {

extern "C" Box* createDict() {
    return new BoxedDict();
}

extern "C" Box* createList() {
    return new BoxedList();
}

BoxedString* boxStringTwine(const llvm::Twine& t) {
    llvm::SmallString<256> Vec;
    return boxString(t.toStringRef(Vec));
}


extern "C" double unboxFloat(Box* b) {
    ASSERT(b->cls == float_cls, "%s", getTypeName(b));
    BoxedFloat* f = (BoxedFloat*)b;
    return f->d;
}

i64 unboxInt(Box* b) {
    ASSERT(b->cls == int_cls, "%s", getTypeName(b));
    return ((BoxedInt*)b)->n;
}

extern "C" bool hasnext(Box* o) {
    return o->cls->tpp_hasnext(o);
}

extern "C" Py_ssize_t str_length(Box* a) noexcept {
    return Py_SIZE(a);
}

extern "C" PyObject* int_richcompare(PyObject* v, PyObject* w, int op) noexcept {
    if (!PyInt_Check(v) || !PyInt_Check(w)) {
        Py_INCREF(Py_NotImplemented);
        return Py_NotImplemented;
    }

    int64_t lhs = static_cast<BoxedInt*>(v)->n;
    int64_t rhs = static_cast<BoxedInt*>(w)->n;

    switch (op) {
        case Py_EQ:
            return boxBool(lhs == rhs);
        case Py_NE:
            return boxBool(lhs != rhs);
        case Py_LT:
            return boxBool(lhs < rhs);
        case Py_LE:
            return boxBool(lhs <= rhs);
        case Py_GT:
            return boxBool(lhs > rhs);
        case Py_GE:
            return boxBool(lhs >= rhs);
        default:
            RELEASE_ASSERT(0, "%d", op);
    }
}

static inline BORROWED(Box*) listGetitemUnboxed(BoxedList* self, int64_t n) {
    assert(PyList_Check(self));
    if (n < 0)
        n = self->size + n;

    if (n < 0 || n >= self->size) {
        raiseExcHelper(IndexError, "list index out of range");
    }
    Box* rtn = self->elts->elts[n];
    return rtn;
}

extern "C" Box* listGetitemInt(BoxedList* self, BoxedInt* slice) {
    assert(PyInt_Check(slice));
    return incref(listGetitemUnboxed(self, slice->n));
}

// BoxedInt::BoxedInt(int64_t n) : Box(int_cls), n(n) {}
}
