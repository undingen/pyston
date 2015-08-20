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

#include "runtime/inline/boxing.h"

#include "llvm/ADT/SmallString.h"

#include "runtime/int.h"
#include "runtime/long.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

namespace pyston {

extern "C" Box* createDict() {
    return new BoxedDict();
}

extern "C" Box* createList() {
    return new BoxedList();
}

extern "C" Box* add_i64_i64(i64 lhs, i64 rhs) {
    i64 result;
    if (!__builtin_saddl_overflow(lhs, rhs, &result))
        return boxInt(result);
    return longAdd(boxLong(lhs), boxLong(rhs));
}

extern "C" Box* intAddIntFallback(i64 lhs, i64 rhs) {
    return longAdd(boxLong(lhs), boxLong(rhs));
}

extern "C" __attribute__((always_inline)) Box* intAddInt(BoxedInt* lhs, BoxedInt* rhs) {
    assert(PyInt_Check(lhs));
    assert(PyInt_Check(rhs));
    i64 result;
    if (!__builtin_saddl_overflow(lhs->n, rhs->n, &result))
        return boxInt(result);
    return intAddIntFallback(lhs->n, rhs->n);
}

extern "C" Box* intSubIntFallback(i64 lhs, i64 rhs) {
    return longSub(boxLong(lhs), boxLong(rhs));
}

extern "C" Box* __attribute__((always_inline)) intSubInt(BoxedInt* lhs, BoxedInt* rhs) {
    assert(PyInt_Check(lhs));
    assert(PyInt_Check(rhs));
    i64 result;
    if (!__builtin_ssubl_overflow(lhs->n, rhs->n, &result))
        return boxInt(result);
    return intSubIntFallback(lhs->n, rhs->n);
}

extern "C" Box* intSubFloat(BoxedInt* lhs, BoxedFloat* rhs) {
    assert(PyInt_Check(lhs));
    assert(rhs->cls == float_cls);
    return boxFloat(lhs->n - rhs->d);
}


extern "C" Box* intAddFloat(BoxedInt* lhs, BoxedFloat* rhs) {
    assert(PyInt_Check(lhs));
    assert(rhs->cls == float_cls);
    return boxFloat(lhs->n + rhs->d);
}

extern "C" Box* intMulInt(BoxedInt* lhs, BoxedInt* rhs) {
    assert(PyInt_Check(lhs));
    assert(PyInt_Check(rhs));
    return mul_i64_i64(lhs->n, rhs->n);
}
extern "C" Box* intMulFloat(BoxedInt* lhs, BoxedFloat* rhs) {
    assert(PyInt_Check(lhs));
    assert(rhs->cls == float_cls);
    return boxFloat(lhs->n * rhs->d);
}

PyObject* int_richcompare(PyObject* v, PyObject* w, int op) noexcept {
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

// BoxedInt::BoxedInt(int64_t n) : Box(int_cls), n(n) {}
}
