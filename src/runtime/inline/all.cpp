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

#define I1_IS_I64
#include "runtime/objmodel.h"
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

extern "C" Box* noneNonzero(Box* v) {
    return False;
}

// BoxedInt::BoxedInt(int64_t n) : Box(int_cls), n(n) {}
}
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

#include "runtime/objmodel.h"
#include <cstring>

#include "runtime/dict.h"
#include "runtime/objmodel.h"

namespace pyston {

BoxedDictIterator::BoxedDictIterator(BoxedDict* d, IteratorType type)
    : d(d), it(d->d.begin()), itEnd(d->d.end()), type(type) {
}

Box* dictIterKeys(Box* s) {
    assert(PyDict_Check(s));
    BoxedDict* self = static_cast<BoxedDict*>(s);
    return new BoxedDictIterator(self, BoxedDictIterator::KeyIterator);
}

Box* dictIterValues(Box* s) {
    assert(PyDict_Check(s));
    BoxedDict* self = static_cast<BoxedDict*>(s);
    return new BoxedDictIterator(self, BoxedDictIterator::ValueIterator);
}

Box* dictIterItems(Box* s) {
    assert(PyDict_Check(s));
    BoxedDict* self = static_cast<BoxedDict*>(s);
    return new BoxedDictIterator(self, BoxedDictIterator::ItemIterator);
}

Box* dictIterIter(Box* s) {
    return s;
}

i64 dictIterHasnextUnboxed(Box* s) {
    assert(s->cls == dict_iterator_cls);
    BoxedDictIterator* self = static_cast<BoxedDictIterator*>(s);

    return self->it != self->itEnd;
}

Box* dictIterHasnext(Box* s) {
    return boxBool(dictIterHasnextUnboxed(s));
}

Box* dictIterNext(Box* s) {
    assert(s->cls == dict_iterator_cls);
    BoxedDictIterator* self = static_cast<BoxedDictIterator*>(s);

    if (self->it == self->itEnd)
        raiseExcHelper(StopIteration, "");

    Box* rtn = nullptr;
    if (self->type == BoxedDictIterator::KeyIterator) {
        rtn = self->it->first.value;
    } else if (self->type == BoxedDictIterator::ValueIterator) {
        rtn = self->it->second;
    } else if (self->type == BoxedDictIterator::ItemIterator) {
        rtn = BoxedTuple::create({ self->it->first.value, self->it->second });
    }
    ++self->it;
    return rtn;
}

BoxedDictView::BoxedDictView(BoxedDict* d) : d(d) {
}

Box* dictViewKeysIter(Box* s) {
    assert(s->cls == dict_keys_cls);
    BoxedDictView* self = static_cast<BoxedDictView*>(s);
    return dictIterKeys(self->d);
}

Box* dictViewValuesIter(Box* s) {
    assert(s->cls == dict_values_cls);
    BoxedDictView* self = static_cast<BoxedDictView*>(s);
    return dictIterValues(self->d);
}

Box* dictViewItemsIter(Box* s) {
    assert(s->cls == dict_items_cls);
    BoxedDictView* self = static_cast<BoxedDictView*>(s);
    return dictIterItems(self->d);
}
}
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

// This file is for forcing the inclusion of function declarations into the stdlib.
// This is so that the types of the functions are available to the compiler.

#include "runtime/objmodel.h"
#include "codegen/ast_interpreter.h"
#include "codegen/irgen/hooks.h"
#include "core/ast.h"
#include "core/threading.h"
#include "core/types.h"
#include "gc/heap.h"
#include "runtime/complex.h"
#include "runtime/float.h"
#include "runtime/generator.h"
#include "runtime/import.h"
#include "runtime/inline/boxing.h"
#include "runtime/inline/list.h"
#include "runtime/int.h"
#include "runtime/list.h"
#include "runtime/long.h"
#include "runtime/objmodel.h"
#include "runtime/set.h"
#include "runtime/types.h"
#include "runtime/util.h"

namespace pyston {

static void forceLink(void* x) {
    printf("%p\n", x);
}

namespace _force {

// Create dummy objects of these types to make sure the types make it into the stdlib:
FrameInfo* _frameinfo_forcer;
AST_stmt* _asttmt_forcer;

#define FORCE(name) forceLink((void*)name)
void force() {
    FORCE(softspace);
    FORCE(my_assert);

    FORCE(boxInt);
    FORCE(unboxInt);
    FORCE(boxFloat);
    FORCE(unboxFloat);
    FORCE(boxCLFunction);
    FORCE(unboxCLFunction);
    FORCE(boxInstanceMethod);
    FORCE(boxBool);
    FORCE(boxBoolNegated);
    FORCE(unboxBool);
    FORCE(createTuple);
    FORCE(createDict);
    FORCE(createList);
    FORCE(createSlice);
    FORCE(createUserClass);
    FORCE(createClosure);
    FORCE(createGenerator);
    FORCE(createLong);
    FORCE(createPureImaginary);
    FORCE(createSet);
    FORCE(decodeUTF8StringPtr);

    FORCE(getattr);
    FORCE(getattr_capi);
    FORCE(setattr);
    FORCE(delattr);
    FORCE(nonzero);
    FORCE(binop);
    FORCE(compare);
    FORCE(augbinop);
    FORCE(unboxedLen);
    FORCE(getitem);
    FORCE(getitem_capi);
    FORCE(getclsattr);
    FORCE(getGlobal);
    FORCE(delGlobal);
    FORCE(setitem);
    FORCE(delitem);
    FORCE(unaryop);
    FORCE(import);
    FORCE(importFrom);
    FORCE(importStar);
    FORCE(repr);
    FORCE(str);
    FORCE(exceptionMatches);
    FORCE(yield);
    FORCE(getiterHelper);
    FORCE(hasnext);

    FORCE(unpackIntoArray);
    FORCE(raiseAttributeError);
    FORCE(raiseAttributeErrorStr);
    FORCE(raiseAttributeErrorCapi);
    FORCE(raiseAttributeErrorStrCapi);
    FORCE(raiseIndexErrorStr);
    FORCE(raiseIndexErrorStrCapi);
    FORCE(raiseNotIterableError);
    FORCE(assertNameDefined);
    FORCE(assertFailDerefNameDefined);
    FORCE(assertFail);
    FORCE(printExprHelper);

    FORCE(strOrUnicode);
    FORCE(printFloat);
    FORCE(listAppendInternal);
    FORCE(getSysStdout);

    FORCE(runtimeCall);
    FORCE(runtimeCallCapi);
    FORCE(callattr);
    FORCE(callattrCapi);

    FORCE(raise0);
    FORCE(raise0_capi);
    FORCE(raise3);
    FORCE(raise3_capi);
    FORCE(PyErr_Fetch);
    FORCE(PyErr_NormalizeException);
    FORCE(PyErr_Restore);
    FORCE(caughtCapiException);
    FORCE(reraiseCapiExcAsCxx);
    FORCE(deopt);

    FORCE(div_i64_i64);
    FORCE(mod_i64_i64);
    FORCE(pow_i64_i64);

    FORCE(div_float_float);
    FORCE(floordiv_float_float);
    FORCE(mod_float_float);
    FORCE(pow_float_float);

    FORCE(exec);

    FORCE(dump);

    FORCE(boxFloat);

    FORCE(createModule);

    FORCE(gc::sizes);

    FORCE(boxedLocalsSet);
    FORCE(boxedLocalsGet);
    FORCE(boxedLocalsDel);

    FORCE(threading::allowGLReadPreemption);

    // FORCE(listIter);
}
}
}

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

#include "runtime/objmodel.h"
#include "runtime/inline/list.h"

#include <cstring>

#include "runtime/list.h"
#include "runtime/objmodel.h"

namespace pyston {

BoxedListIterator::BoxedListIterator(BoxedList* l, int start) : l(l), pos(start) {
}

Box* listIterIter(Box* s) {
    return s;
}

Box* listIter(Box* s) noexcept {
    assert(PyList_Check(s));
    BoxedList* self = static_cast<BoxedList*>(s);
    return new BoxedListIterator(self, 0);
}

Box* listiterHasnext(Box* s) {
    assert(s->cls == list_iterator_cls);
    BoxedListIterator* self = static_cast<BoxedListIterator*>(s);

    if (!self->l) {
        return False;
    }

    bool ans = (self->pos < self->l->size);
    if (!ans) {
        self->l = NULL;
    }
    return boxBool(ans);
}

i64 listiterHasnextUnboxed(Box* s) {
    assert(s->cls == list_iterator_cls);
    BoxedListIterator* self = static_cast<BoxedListIterator*>(s);

    if (!self->l) {
        return false;
    }

    bool ans = (self->pos < self->l->size);
    if (!ans) {
        self->l = NULL;
    }
    return ans;
}

template <ExceptionStyle S> Box* listiterNext(Box* s) noexcept(S == CAPI) {
    assert(s->cls == list_iterator_cls);
    BoxedListIterator* self = static_cast<BoxedListIterator*>(s);

    if (!self->l) {
        if (S == CAPI) {
            PyErr_SetObject(StopIteration, None);
            return NULL;
        } else
            raiseExcHelper(StopIteration, "");
    }

    if (!(self->pos >= 0 && self->pos < self->l->size)) {
        self->l = NULL;
        if (S == CAPI) {
            PyErr_SetObject(StopIteration, None);
            return NULL;
        } else
            raiseExcHelper(StopIteration, "");
    }

    Box* rtn = self->l->elts->elts[self->pos];
    self->pos++;
    return rtn;
}
// force instantiation:
template Box* listiterNext<CAPI>(Box*);
template Box* listiterNext<CXX>(Box*);

Box* listReversed(Box* s) {
    assert(PyList_Check(s));
    BoxedList* self = static_cast<BoxedList*>(s);
    return new (list_reverse_iterator_cls) BoxedListIterator(self, self->size - 1);
}

Box* listreviterHasnext(Box* s) {
    assert(s->cls == list_reverse_iterator_cls);
    BoxedListIterator* self = static_cast<BoxedListIterator*>(s);

    return boxBool(self->pos >= 0);
}

i64 listreviterHasnextUnboxed(Box* s) {
    assert(s->cls == list_reverse_iterator_cls);
    BoxedListIterator* self = static_cast<BoxedListIterator*>(s);

    return self->pos >= 0;
}

Box* listreviterNext(Box* s) {
    assert(s->cls == list_reverse_iterator_cls);
    BoxedListIterator* self = static_cast<BoxedListIterator*>(s);

    if (!(self->pos >= 0 && self->pos < self->l->size)) {
        raiseExcHelper(StopIteration, "");
    }

    Box* rtn = self->l->elts->elts[self->pos];
    self->pos--;
    return rtn;
}


const int BoxedList::INITIAL_CAPACITY = 8;
// TODO the inliner doesn't want to inline these; is there any point to having them in the inline section?
void BoxedList::shrink() {
    // TODO more attention to the shrink condition to avoid frequent shrink and alloc
    if (capacity > size * 3) {
        int new_capacity = std::max(static_cast<int64_t>(INITIAL_CAPACITY), capacity / 2);
        if (size > 0) {
            elts = GCdArray::realloc(elts, new_capacity);
            capacity = new_capacity;
        } else if (size == 0) {
            delete elts;
            capacity = 0;
        }
    }
}


extern "C" void listAppendArrayInternal(Box* s, Box** v, int nelts) {
    assert(PyList_Check(s));
    BoxedList* self = static_cast<BoxedList*>(s);

    assert(self->size <= self->capacity);
    self->ensure(nelts);

    assert(self->size <= self->capacity);
    memcpy(&self->elts->elts[self->size], &v[0], nelts * sizeof(Box*));

    self->size += nelts;
}

// TODO the inliner doesn't want to inline these; is there any point to having them in the inline section?
extern "C" Box* listAppend(Box* s, Box* v) {
    assert(PyList_Check(s));
    BoxedList* self = static_cast<BoxedList*>(s);

    listAppendInternal(self, v);

    return None;
}
}
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

#include "runtime/objmodel.h"
#include <cstring>

#include "runtime/objmodel.h"
#include "runtime/tuple.h"

namespace pyston {

BoxedTupleIterator::BoxedTupleIterator(BoxedTuple* t) : t(t), pos(0) {
}

Box* tupleIterIter(Box* s) {
    return s;
}

Box* tupleIter(Box* s) noexcept {
    assert(PyTuple_Check(s));
    BoxedTuple* self = static_cast<BoxedTuple*>(s);
    return new BoxedTupleIterator(self);
}

Box* tupleiterHasnext(Box* s) {
    return boxBool(tupleiterHasnextUnboxed(s));
}

i64 tupleiterHasnextUnboxed(Box* s) {
    assert(s->cls == tuple_iterator_cls);
    BoxedTupleIterator* self = static_cast<BoxedTupleIterator*>(s);

    return self->pos < self->t->size();
}

Box* tupleiterNext(Box* s) {
    assert(s->cls == tuple_iterator_cls);
    BoxedTupleIterator* self = static_cast<BoxedTupleIterator*>(s);

    if (!(self->pos >= 0 && self->pos < self->t->size())) {
        raiseExcHelper(StopIteration, "");
    }

    Box* rtn = self->t->elts[self->pos];
    self->pos++;
    return rtn;
}
}
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

#include "core/types.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

namespace pyston {

BoxedClass* xrange_cls, *xrange_iterator_cls;

class BoxedXrangeIterator;
class BoxedXrange : public Box {
public:
    const int64_t start, stop, step;
    int64_t len;

    // from cpython
    /* Return number of items in range (lo, hi, step).  step != 0
     * required.  The result always fits in an unsigned long.
     */
    static int64_t get_len_of_range(int64_t lo, int64_t hi, int64_t step) {
        /* -------------------------------------------------------------
        If step > 0 and lo >= hi, or step < 0 and lo <= hi, the range is empty.
        Else for step > 0, if n values are in the range, the last one is
        lo + (n-1)*step, which must be <= hi-1.  Rearranging,
        n <= (hi - lo - 1)/step + 1, so taking the floor of the RHS gives
        the proper value.  Since lo < hi in this case, hi-lo-1 >= 0, so
        the RHS is non-negative and so truncation is the same as the
        floor.  Letting M be the largest positive long, the worst case
        for the RHS numerator is hi=M, lo=-M-1, and then
        hi-lo-1 = M-(-M-1)-1 = 2*M.  Therefore unsigned long has enough
        precision to compute the RHS exactly.  The analysis for step < 0
        is similar.
        ---------------------------------------------------------------*/
        assert(step != 0LL);
        if (step > 0LL && lo < hi)
            return 1LL + (hi - 1LL - lo) / step;
        else if (step < 0 && lo > hi)
            return 1LL + (lo - 1LL - hi) / (0LL - step);
        else
            return 0LL;
    }

    BoxedXrange(int64_t start, int64_t stop, int64_t step) : start(start), stop(stop), step(step) {
        len = get_len_of_range(start, stop, step);
    }

    friend class BoxedXrangeIterator;

    DEFAULT_CLASS(xrange_cls);
};

class BoxedXrangeIterator : public Box {
private:
    BoxedXrange* const xrange;
    int64_t cur;
    int64_t stop, step;

public:
    BoxedXrangeIterator(BoxedXrange* xrange, bool reversed) : xrange(xrange) {
        int64_t start = xrange->start;
        int64_t len = xrange->len;

        stop = xrange->stop;
        step = xrange->step;

        if (reversed) {
            stop = xrange->start - step;
            start = xrange->start + (len - 1) * step;
            step = -step;
        }

        cur = start;
    }

    DEFAULT_CLASS(xrange_iterator_cls);

    static bool xrangeIteratorHasnextUnboxed(Box* s) __attribute__((visibility("default"))) {
        assert(s->cls == xrange_iterator_cls);
        BoxedXrangeIterator* self = static_cast<BoxedXrangeIterator*>(s);

        if (self->step > 0) {
            return self->cur < self->stop;
        } else {
            return self->cur > self->stop;
        }
    }

    static Box* xrangeIteratorHasnext(Box* s) __attribute__((visibility("default"))) {
        return boxBool(xrangeIteratorHasnextUnboxed(s));
    }

    static i64 xrangeIteratorNextUnboxed(Box* s) __attribute__((visibility("default"))) {
        assert(s->cls == xrange_iterator_cls);
        BoxedXrangeIterator* self = static_cast<BoxedXrangeIterator*>(s);

        if (!xrangeIteratorHasnextUnboxed(s))
            raiseExcHelper(StopIteration, "");

        i64 rtn = self->cur;
        self->cur += self->step;
        return rtn;
    }

    static Box* xrangeIteratorNext(Box* s) __attribute__((visibility("default"))) {
        return boxInt(xrangeIteratorNextUnboxed(s));
    }

    static void gcHandler(GCVisitor* v, Box* b) {
        Box::gcHandler(v, b);

        BoxedXrangeIterator* it = (BoxedXrangeIterator*)b;
        v->visit(it->xrange);
    }
};

Box* xrange(Box* cls, Box* start, Box* stop, Box** args) {
    assert(cls == xrange_cls);

    Box* step = args[0];

    if (stop == NULL) {
        i64 istop = PyLong_AsLong(start);
        checkAndThrowCAPIException();
        return new BoxedXrange(0, istop, 1);
    } else if (step == NULL) {
        i64 istart = PyLong_AsLong(start);
        checkAndThrowCAPIException();
        i64 istop = PyLong_AsLong(stop);
        checkAndThrowCAPIException();
        return new BoxedXrange(istart, istop, 1);
    } else {
        i64 istart = PyLong_AsLong(start);
        checkAndThrowCAPIException();
        i64 istop = PyLong_AsLong(stop);
        checkAndThrowCAPIException();
        i64 istep = PyLong_AsLong(step);
        checkAndThrowCAPIException();
        RELEASE_ASSERT(istep != 0, "step can't be 0");
        return new BoxedXrange(istart, istop, istep);
    }
}

Box* xrangeIterIter(Box* self) {
    assert(self->cls == xrange_iterator_cls);
    return self;
}

Box* xrangeIter(Box* self) noexcept {
    assert(self->cls == xrange_cls);

    Box* rtn = new BoxedXrangeIterator(static_cast<BoxedXrange*>(self), false);
    return rtn;
}

Box* xrangeReversed(Box* self) {
    assert(self->cls == xrange_cls);

    Box* rtn = new BoxedXrangeIterator(static_cast<BoxedXrange*>(self), true);
    return rtn;
}

Box* xrangeGetitem(Box* self, Box* slice) {
    assert(isSubclass(self->cls, xrange_cls));
    BoxedXrange* r = static_cast<BoxedXrange*>(self);
    if (PyIndex_Check(slice)) {
        Py_ssize_t i = PyNumber_AsSsize_t(slice, PyExc_IndexError);
        if (i < 0 || i >= r->len) {
            raiseExcHelper(IndexError, "xrange object index out of range");
        }
        /* do calculation entirely using unsigned longs, to avoid
           undefined behaviour due to signed overflow. */
        return PyInt_FromLong((long)(r->start + (unsigned long)i * r->step));
    } else {
        RELEASE_ASSERT(false, "unimplemented");
    }
}

Box* xrangeLen(Box* self) {
    assert(isSubclass(self->cls, xrange_cls));
    return boxInt(static_cast<BoxedXrange*>(self)->len);
}

void setupXrange() {
    xrange_cls = BoxedHeapClass::create(type_cls, object_cls, NULL, 0, 0, sizeof(BoxedXrange), false, "xrange");
    xrange_iterator_cls = BoxedHeapClass::create(type_cls, object_cls, &BoxedXrangeIterator::gcHandler, 0, 0,
                                                 sizeof(BoxedXrangeIterator), false, "rangeiterator");

    xrange_cls->giveAttr(
        "__new__",
        new BoxedFunction(boxRTFunction((void*)xrange, typeFromClass(xrange_cls), 4, 2, false, false), { NULL, NULL }));
    xrange_cls->giveAttr("__iter__",
                         new BoxedFunction(boxRTFunction((void*)xrangeIter, typeFromClass(xrange_iterator_cls), 1)));
    xrange_cls->giveAttr(
        "__reversed__", new BoxedFunction(boxRTFunction((void*)xrangeReversed, typeFromClass(xrange_iterator_cls), 1)));

    xrange_cls->giveAttr("__getitem__", new BoxedFunction(boxRTFunction((void*)xrangeGetitem, BOXED_INT, 2)));

    xrange_cls->giveAttr("__len__", new BoxedFunction(boxRTFunction((void*)xrangeLen, BOXED_INT, 1)));

    CLFunction* hasnext = boxRTFunction((void*)BoxedXrangeIterator::xrangeIteratorHasnextUnboxed, BOOL, 1);
    addRTFunction(hasnext, (void*)BoxedXrangeIterator::xrangeIteratorHasnext, BOXED_BOOL);
    xrange_iterator_cls->giveAttr(
        "__iter__", new BoxedFunction(boxRTFunction((void*)xrangeIterIter, typeFromClass(xrange_iterator_cls), 1)));
    xrange_iterator_cls->giveAttr("__hasnext__", new BoxedFunction(hasnext));

    CLFunction* next = boxRTFunction((void*)BoxedXrangeIterator::xrangeIteratorNextUnboxed, INT, 1);
    addRTFunction(next, (void*)BoxedXrangeIterator::xrangeIteratorNext, BOXED_INT);
    xrange_iterator_cls->giveAttr("next", new BoxedFunction(next));

    // TODO this is pretty hacky, but stuff the iterator cls into xrange to make sure it gets decref'd at the end
    xrange_cls->giveAttr("__iterator_cls__", xrange_iterator_cls);

    xrange_cls->freeze();
    xrange_cls->tp_iter = xrangeIter;

    xrange_iterator_cls->freeze();
    xrange_iterator_cls->tpp_hasnext = BoxedXrangeIterator::xrangeIteratorHasnextUnboxed;
}
}
