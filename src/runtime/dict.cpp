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

#include "runtime/dict.h"

#include "capi/typeobject.h"
#include "capi/types.h"
#include "core/ast.h"
#include "core/common.h"
#include "core/stats.h"
#include "core/types.h"
#include "runtime/hiddenclass.h"
#include "runtime/ics.h"
#include "runtime/inline/list.h"
#include "runtime/objmodel.h"
#include "runtime/set.h"
#include "runtime/types.h"
#include "runtime/util.h"

namespace pyston {

extern "C" {
BoxedClass* dictiterkey_cls = NULL;
BoxedClass* dictitervalue_cls = NULL;
BoxedClass* dictiteritem_cls = NULL;
}

Box* dictRepr(BoxedDict* self) {
    RELEASE_ASSERT(!self->getHCAttrs(), "");

    std::vector<char> chars;
    int status = Py_ReprEnter((PyObject*)self);
    if (status != 0) {
        if (status < 0)
            throwCAPIException();

        chars.push_back('{');
        chars.push_back('.');
        chars.push_back('.');
        chars.push_back('.');
        chars.push_back('}');
        return boxString(llvm::StringRef(&chars[0], chars.size()));
    }

    try {
        chars.push_back('{');
        bool first = true;
        for (const auto& p : *self) {
            if (!first) {
                chars.push_back(',');
                chars.push_back(' ');
            }
            first = false;
            BoxedString* k = static_cast<BoxedString*>(repr(p.first));
            BoxedString* v = static_cast<BoxedString*>(repr(p.second));
            chars.insert(chars.end(), k->s().begin(), k->s().end());
            chars.push_back(':');
            chars.push_back(' ');
            chars.insert(chars.end(), v->s().begin(), v->s().end());
        }
        chars.push_back('}');
    } catch (ExcInfo e) {
        Py_ReprLeave((PyObject*)self);
        throw e;
    }
    Py_ReprLeave((PyObject*)self);
    return boxString(llvm::StringRef(&chars[0], chars.size()));
}

Box* dictClear(BoxedDict* self) {
    if (!PyDict_Check(self))
        raiseExcHelper(TypeError, "descriptor 'clear' requires a 'dict' object but received a '%s'", getTypeName(self));

    PyDict_Clear(self);
    return None;
}

Box* dictCopy(BoxedDict* self) {
    if (!PyDict_Check(self))
        raiseExcHelper(TypeError, "descriptor 'copy' requires a 'dict' object but received a '%s'", getTypeName(self));

    HCAttrs* attrs = self->getHCAttrs();
    if (attrs) {
        BoxedDict* rtn = new BoxedDict();
        RELEASE_ASSERT(attrs->hcls->type == HiddenClass::NORMAL || attrs->hcls->type == HiddenClass::SINGLETON, "");
        if (attrs->hcls->type == HiddenClass::SINGLETON) {
            rtn->hcattrs = new HCAttrs(attrs->hcls);
            int numattrs = attrs->hcls->attributeArraySize();
            int new_size = sizeof(HCAttrs::AttrList) + sizeof(Box*) * (numattrs);
            rtn->hcattrs->attr_list = (HCAttrs::AttrList*)gc_alloc(new_size, gc::GCKind::PRECISE);
            memcpy(rtn->hcattrs->attr_list, attrs->attr_list->attrs, new_size);
        } else {
            for (const auto& p : attrs->hcls->getStrAttrOffsets()) {
                PyDict_SetItem(rtn, p.first, attrs->attr_list->attrs[p.second]);
            }
        }
        return rtn;
    }

    BoxedDict* r = new BoxedDict();
    if (self->d) {
        r->d = new BoxedDict::DictMap;
        *r->d = *self->d;
    }
    return r;
}

Box* dictItems(BoxedDict* self) {
    BoxedList* rtn = new BoxedList();

    HCAttrs* attrs = self->getHCAttrs();
    if (attrs) {
        RELEASE_ASSERT(attrs->hcls->type == HiddenClass::NORMAL || attrs->hcls->type == HiddenClass::SINGLETON, "");
        rtn->ensure(attrs->hcls->getStrAttrOffsets().size());
        for (const auto& p : attrs->hcls->getStrAttrOffsets()) {
            BoxedTuple* t = BoxedTuple::create({ p.first, attrs->attr_list->attrs[p.second] });
            listAppend(rtn, t);
        }
        return rtn;
    }

    if (!self->d)
        return rtn;

    rtn->ensure(self->d->size());
    for (const auto& p : *self) {
        BoxedTuple* t = BoxedTuple::create({ p.first, p.second });
        listAppendInternal(rtn, t);
    }

    return rtn;
}

Box* dictValues(BoxedDict* self) {
    HCAttrs* attrs = self->getHCAttrs();
    if (attrs) {
        BoxedList* rtn = new BoxedList();
        RELEASE_ASSERT(attrs->hcls->type == HiddenClass::NORMAL || attrs->hcls->type == HiddenClass::SINGLETON, "");
        rtn->ensure(attrs->hcls->getStrAttrOffsets().size());
        for (const auto& p : attrs->hcls->getStrAttrOffsets()) {
            listAppend(rtn, attrs->attr_list->attrs[p.second]);
        }
        return rtn;
    }

    BoxedList* rtn = new BoxedList();
    if (!self->d)
        return rtn;

    rtn->ensure(self->d->size());
    for (const auto& p : *self) {
        listAppendInternal(rtn, p.second);
    }
    return rtn;
}

Box* dictKeys(BoxedDict* self) {
    RELEASE_ASSERT(PyDict_Check(self), "");

    HCAttrs* attrs = self->getHCAttrs();
    if (attrs) {
        BoxedList* rtn = new BoxedList();
        RELEASE_ASSERT(attrs->hcls->type == HiddenClass::NORMAL || attrs->hcls->type == HiddenClass::SINGLETON, "");
        rtn->ensure(attrs->hcls->getStrAttrOffsets().size());
        for (const auto& p : attrs->hcls->getStrAttrOffsets()) {
            listAppend(rtn, p.first);
        }
        return rtn;
    }

    BoxedList* rtn = new BoxedList();
    if (!self->d)
        return rtn;

    rtn->ensure(self->d->size());
    for (const auto& p : *self) {
        listAppendInternal(rtn, p.first);
    }
    return rtn;
}

static PyObject* dict_helper(PyObject* mp, std::function<Box*(BoxedDict*)> f) noexcept {
    if (mp == NULL || !PyDict_Check(mp)) {
        PyErr_BadInternalCall();
        return NULL;
    }

    try {
        return f(static_cast<BoxedDict*>(mp));
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

extern "C" PyObject* PyDict_Keys(PyObject* mp) noexcept {
    return dict_helper(mp, dictKeys);
}

extern "C" PyObject* PyDict_Values(PyObject* mp) noexcept {
    return dict_helper(mp, dictValues);
}

extern "C" PyObject* PyDict_Items(PyObject* mp) noexcept {
    return dict_helper(mp, dictItems);
}

// Analoguous to CPython's, used for sq_ slots.
static Py_ssize_t dict_length(PyDictObject* mp) {
    return PyDict_Size((PyObject*)mp);
}

Box* dictLen(BoxedDict* self) {
    if (!PyDict_Check(self))
        raiseExcHelper(TypeError, "descriptor '__len__' requires a 'dict' object but received a '%s'",
                       getTypeName(self));

    return boxInt(PyDict_Size(self));
}

extern "C" Py_ssize_t PyDict_Size(PyObject* op) noexcept {
    if (op->cls == attrwrapper_cls)
        return PyObject_Size(op);

    RELEASE_ASSERT(PyDict_Check(op), "");
    BoxedDict* self = (BoxedDict*)op;

    HCAttrs* attrs = self->getHCAttrs();
    if (attrs) {
        RELEASE_ASSERT(attrs->hcls->type == HiddenClass::NORMAL || attrs->hcls->type == HiddenClass::SINGLETON, "");
        return attrs->hcls->getStrAttrOffsets().size();
    }
    if (!self->d)
        return 0;
    return self->d->size();
}

extern "C" void PyDict_Clear(PyObject* op) noexcept {
    RELEASE_ASSERT(PyDict_Check(op), "");
    BoxedDict* self = (BoxedDict*)op;

    HCAttrs* attrs = self->getHCAttrs();
    if (attrs) {
        if (self->b) {
            RELEASE_ASSERT(attrs->hcls->type == HiddenClass::NORMAL || attrs->hcls->type == HiddenClass::SINGLETON, "");
            // Clear the attrs array:
            new ((void*)attrs) HCAttrs(root_hcls);
            // Add the existing attrwrapper object (ie self) back as the attrwrapper:
            attrs->appendNewHCAttr(self);
            attrs->hcls = attrs->hcls->getAttrwrapperChild();
            return;
        }
        self->hcattrs = NULL;
    }
    if (self->d)
        self->d->clear();
}

extern "C" PyObject* PyDict_Copy(PyObject* o) noexcept {
    RELEASE_ASSERT(PyDict_Check(o) || o->cls == attrwrapper_cls, "");
    try {
        if (o->cls == attrwrapper_cls)
            return attrwrapperToDict(o);

        return dictCopy(static_cast<BoxedDict*>(o));
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

extern "C" int PyDict_Update(PyObject* a, PyObject* b) noexcept {
    return PyDict_Merge(a, b, 1);
}

template <enum ExceptionStyle S> Box* dictGetitem(BoxedDict* self, Box* k) noexcept(S == CAPI) {
    if (!PyDict_Check(self)) {
        if (S == CAPI) {
            PyErr_Format(TypeError, "descriptor '__getitem__' requires a 'dict' object but received a '%s'",
                         getTypeName(self));
            return NULL;
        } else {
            raiseExcHelper(TypeError, "descriptor '__getitem__' requires a 'dict' object but received a '%s'",
                           getTypeName(self));
        }
    }

    try {
        Box* rtn = self->getOrNull(k);
        if (rtn)
            return rtn;
    } catch (ExcInfo e) {
        if (S == CAPI) {
            setCAPIException(e);
            return NULL;
        } else {
            throw e;
        }
    }

    // Try calling __missing__ if this is a subclass
    if (self->cls != dict_cls) {
        // Special-case defaultdict, assuming that it's the main time we will actually hit this.
        // We could just use a single runtime IC here, or have a small cache that maps type->runtimeic.
        // Or use a polymorphic runtime ic.
        static BoxedClass* defaultdict_cls = NULL;
        static CallattrIC defaultdict_ic;
        if (defaultdict_cls == NULL && strcmp(self->cls->tp_name, "collections.defaultdict") == 0) {
            defaultdict_cls = self->cls;
        }

        static BoxedString* missing_str = internStringImmortal("__missing__");
        CallattrFlags callattr_flags{.cls_only = true, .null_on_nonexistent = true, .argspec = ArgPassSpec(1) };
        Box* r;
        try {
            if (self->cls == defaultdict_cls) {
                r = defaultdict_ic.call(self, missing_str, callattr_flags, k, NULL, NULL, NULL, NULL);
            } else {
                r = callattr(self, missing_str, callattr_flags, k, NULL, NULL, NULL, NULL);
            }
        } catch (ExcInfo e) {
            if (S == CAPI) {
                setCAPIException(e);
                return NULL;
            } else
                throw e;
        }
        if (r)
            return r;
    }

    if (S == CAPI) {
        PyErr_SetObject(KeyError, BoxedTuple::create1(k));
        return NULL;
    } else
        raiseExcHelper(KeyError, k);
}

extern "C" PyObject* PyDict_New() noexcept {
    return new BoxedDict();
}



Box* dictSetitem(BoxedDict* self, Box* k, Box* v);
// We don't assume that dicts passed to PyDict are necessarily dicts, since there are a couple places
// that we provide dict-like objects instead of proper dicts.
// The performance should hopefully be comparable to the CPython fast case, since we can use
// runtimeICs.
extern "C" int PyDict_SetItem(PyObject* mp, PyObject* _key, PyObject* _item) noexcept {
    ASSERT(PyDict_Check(mp) || mp->cls == attrwrapper_cls, "%s", getTypeName(mp));

    assert(mp);
    BoxedDict* b = static_cast<BoxedDict*>(mp);
    Box* key = static_cast<Box*>(_key);
    Box* item = static_cast<Box*>(_item);

    assert(key);
    assert(item);

    try {
        dictSetitem(b, key, item);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return -1;
    }
    return 0;
}

extern "C" int PyDict_SetItemString(PyObject* mp, const char* key, PyObject* item) noexcept {
    Box* key_s;
    try {
        key_s = boxString(key);
    } catch (ExcInfo e) {
        abort();
    }

    return PyDict_SetItem(mp, key_s, item);
}

extern "C" PyObject* PyDict_GetItem(PyObject* dict, PyObject* key) noexcept {
    ASSERT(PyDict_Check(dict) || dict->cls == attrwrapper_cls, "%s", getTypeName(dict));
    if (PyDict_Check(dict)) {
        BoxedDict* d = static_cast<BoxedDict*>(dict);
        return d->getOrNull(key);
    }

    auto&& tstate = _PyThreadState_Current;
    if (tstate != NULL && tstate->curexc_type != NULL) {
        /* preserve the existing exception */
        PyObject* err_type, *err_value, *err_tb;
        PyErr_Fetch(&err_type, &err_value, &err_tb);
        Box* b = getitemInternal<CAPI>(dict, key);
        /* ignore errors */
        PyErr_Restore(err_type, err_value, err_tb);
        return b;
    } else {
        Box* b = getitemInternal<CAPI>(dict, key);
        if (b == NULL)
            PyErr_Clear();
        return b;
    }
}

extern "C" int PyDict_Next(PyObject* op, Py_ssize_t* ppos, PyObject** pkey, PyObject** pvalue) noexcept {
    assert(PyDict_Check(op));
    BoxedDict* self = static_cast<BoxedDict*>(op);

    // Callers of PyDict_New() provide a pointer to some storage for this function to use, in
    // the form of a Py_ssize_t* -- ie they allocate a Py_ssize_t on their stack, and let us use
    // it.
    //
    // We want to store an unordered_map::iterator in that.  In my glibc it would fit, but to keep
    // things a little bit more portable, allocate separate storage for the iterator, and store the
    // pointer to this storage in the Py_ssize_t slot.
    //
    // Results in lots of indirection unfortunately.  If it becomes an issue we can try to switch
    // to storing the iterator directly in the stack slot.

    typedef BoxedDict::iterator iterator;

    static_assert(sizeof(Py_ssize_t) == sizeof(iterator*), "");
    iterator** it_ptr = reinterpret_cast<iterator**>(ppos);

    // Clients are supposed to zero-initialize *ppos:
    if (*it_ptr == NULL) {
        *it_ptr = (iterator*)malloc(sizeof(iterator));
        **it_ptr = self->begin();
    }

    iterator* it = *it_ptr;

    if (*it == self->end()) {
        free(it);
        return 0;
    }

    *pkey = it->first();
    *pvalue = it->second();

    ++(*it);

    return 1;
}

Box* BoxedDict::getOrNull(Box* k) {
    HCAttrs* attrs = getHCAttrs();
    Box* _key = NULL;
    if (attrs) {
        _key = coerceUnicodeToStr<CAPI>(k);
        if (!_key || _key->cls != str_cls) {
            PyErr_Clear();
            convertToDict();
            attrs = NULL;
        }
    }

    if (attrs) {
        RELEASE_ASSERT(_key->cls == str_cls, "");
        BoxedString* key = static_cast<BoxedString*>(_key);
        internStringMortalInplace(key);

        HiddenClass* hcls = attrs->hcls;
        assert(hcls->type == HiddenClass::NORMAL || hcls->type == HiddenClass::SINGLETON);

        int offset = hcls->getOffset(key);
        if (offset == -1)
            return NULL;
        return attrs->attr_list->attrs[offset];
    }
    if (!d)
        return NULL;

    const auto& p = d->find(BoxAndHash(k));
    if (p != d->end())
        return p->second;
    return NULL;
}

extern "C" PyObject* PyDict_GetItemString(PyObject* dict, const char* key) noexcept {
    if (dict->cls == attrwrapper_cls)
        return unwrapAttrWrapper(dict)->getattr(internStringMortal(key));

    Box* key_s;
    try {
        key_s = boxString(key);
    } catch (ExcInfo e) {
        abort();
    }
    return PyDict_GetItem(dict, key_s);
}

void BoxedDict::convertToDict() {
    HCAttrs* attrs = getHCAttrs();
    RELEASE_ASSERT(!d, "");
    RELEASE_ASSERT(attrs->hcls->type == HiddenClass::NORMAL || attrs->hcls->type == HiddenClass::SINGLETON, "");
    d = new DictMap;
    d->grow(attrs->hcls->getStrAttrOffsets().size());
    for (const auto& p : attrs->hcls->getStrAttrOffsets()) {
        (*d)[p.first] = attrs->attr_list->attrs[p.second];
    }
    b = NULL;
    hcattrs = NULL;
}

Box* dictSetitem(BoxedDict* self, Box* k, Box* v) {
    HCAttrs* attrs = self->getHCAttrs();
    if (!self->d && !attrs) {
        if (k->cls == str_cls)
            attrs = self->hcattrs = new HCAttrs;
        else
            self->d = new BoxedDict::DictMap;
    }

    if (attrs) {
        if (k->cls == str_cls) {
            RELEASE_ASSERT(k->cls == str_cls, "");
            BoxedString* key = static_cast<BoxedString*>(k);
            internStringMortalInplace(key);

            HiddenClass* hcls = attrs->hcls;
            assert(hcls->type == HiddenClass::NORMAL || hcls->type == HiddenClass::SINGLETON);

            int offset = hcls->getOffset(key);
            if (offset >= 0) {
                assert(offset < hcls->attributeArraySize());
                Box* prev = attrs->attr_list->attrs[offset];
                attrs->attr_list->attrs[offset] = v;
                return None;
            }

            assert(offset == -1);

            if (hcls->type == HiddenClass::NORMAL) {
                HiddenClass* new_hcls = hcls->getOrMakeChild(key);
                // make sure we don't need to rearrange the attributes
                assert(new_hcls->getStrAttrOffsets().lookup(key) == hcls->attributeArraySize());

                attrs->appendNewHCAttr(v);
                attrs->hcls = new_hcls;
            } else {
                assert(hcls->type == HiddenClass::SINGLETON);
                attrs->appendNewHCAttr(v);
                hcls->appendAttribute(key);
            }
            return None;
        }

        self->convertToDict();
    }
    (*self->d)[k] = v;
    return None;
}

Box* dictDelitem(BoxedDict* self, Box* k) {
    if (!PyDict_Check(self))
        raiseExcHelper(TypeError, "descriptor '__delitem__' requires a 'dict' object but received a '%s'",
                       getTypeName(self));

    if (!self->getOrNull(k)) {
        raiseExcHelper(KeyError, k);
    }

    HCAttrs* attrs = self->getHCAttrs();
    if (attrs) {
        HiddenClass* hcls = attrs->hcls;
        assert(hcls->type == HiddenClass::NORMAL || hcls->type == HiddenClass::SINGLETON);

        Box* _key = coerceUnicodeToStr<CAPI>(k);
        RELEASE_ASSERT(_key->cls == str_cls, "");
        BoxedString* key = static_cast<BoxedString*>(_key);
        internStringMortalInplace(key);

        // The order of attributes is pertained as delAttrToMakeHC constructs
        // the new HiddenClass by invoking getOrMakeChild in the prevous order
        // of remaining attributes
        int num_attrs = hcls->attributeArraySize();
        int offset = hcls->getOffset(key);
        assert(offset >= 0);
        Box** start = attrs->attr_list->attrs;
        memmove(start + offset, start + offset + 1, (num_attrs - offset - 1) * sizeof(Box*));

        if (hcls->type == HiddenClass::NORMAL) {
            HiddenClass* new_hcls = hcls->delAttrToMakeHC(key);
            attrs->hcls = new_hcls;
        } else {
            assert(hcls->type == HiddenClass::SINGLETON);
            hcls->delAttribute(key);
        }

        // guarantee the size of the attr_list equals the number of attrs
        int new_size = sizeof(HCAttrs::AttrList) + sizeof(Box*) * (num_attrs - 1);
        attrs->attr_list = (HCAttrs::AttrList*)gc::gc_realloc(attrs->attr_list, new_size);
        return None;
    }
    self->d->erase(k);
    return None;
}

// Analoguous to CPython's, used for sq_ slots.
static int dict_ass_sub(PyDictObject* mp, PyObject* v, PyObject* w) noexcept {
    try {
        Box* res;
        if (w == NULL) {
            res = dictDelitem((BoxedDict*)mp, v);
        } else {
            res = dictSetitem((BoxedDict*)mp, v, w);
        }
        assert(res == None);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return -1;
    }
    return 0;
}

extern "C" int PyDict_DelItem(PyObject* op, PyObject* key) noexcept {
    ASSERT(PyDict_Check(op) || op->cls == attrwrapper_cls, "%s", getTypeName(op));
    try {
        delitem(op, key);
        return 0;
    } catch (ExcInfo e) {
        setCAPIException(e);
        return -1;
    }
}

extern "C" int PyDict_DelItemString(PyObject* v, const char* key) noexcept {
    PyObject* kv;
    int err;
    kv = PyString_FromString(key);
    if (kv == NULL)
        return -1;
    err = PyDict_DelItem(v, kv);
    Py_DECREF(kv);
    return err;
}

Box* dictPop(BoxedDict* self, Box* k, Box* d) {
    if (!PyDict_Check(self))
        raiseExcHelper(TypeError, "descriptor 'pop' requires a 'dict' object but received a '%s'", getTypeName(self));

    Box* rtn = self->getOrNull(k);
    if (!rtn) {
        if (d)
            return d;
        raiseExcHelper(KeyError, k);
    }
    dictDelitem(self, k);
    return rtn;
}

Box* dictPopitem(BoxedDict* self) {
    if (!PyDict_Check(self))
        raiseExcHelper(TypeError, "descriptor 'popitem' requires a 'dict' object but received a '%s'",
                       getTypeName(self));

    RELEASE_ASSERT(!self->getHCAttrs(), "");

    auto it = self->d->begin();
    if (it == self->d->end()) {
        raiseExcHelper(KeyError, "popitem(): dictionary is empty");
    }

    Box* key = it->first.value;
    Box* value = it->second;
    self->d->erase(it);

    auto rtn = BoxedTuple::create({ key, value });
    return rtn;
}

Box* dictGet(BoxedDict* self, Box* k, Box* d) {
    if (!PyDict_Check(self))
        raiseExcHelper(TypeError, "descriptor 'get' requires a 'dict' object but received a '%s'", getTypeName(self));

    Box* rtn = self->getOrNull(k);
    if (!rtn)
        return d;
    return rtn;
}

Box* dictSetdefault(BoxedDict* self, Box* k, Box* v) {
    if (!PyDict_Check(self))
        raiseExcHelper(TypeError, "descriptor 'setdefault' requires a 'dict' object but received a '%s'",
                       getTypeName(self));

    Box* rtn = self->getOrNull(k);
    if (rtn)
        return rtn;

    dictSetitem(self, k, v);
    return v;
}

Box* dictContains(BoxedDict* self, Box* k) {
    if (!PyDict_Check(self))
        raiseExcHelper(TypeError, "descriptor '__contains__' requires a 'dict' object but received a '%s'",
                       getTypeName(self));

    return boxBool(self->getOrNull(k) != NULL);
}

/* Return 1 if `key` is in dict `op`, 0 if not, and -1 on error. */
extern "C" int PyDict_Contains(PyObject* op, PyObject* key) noexcept {

    try {
        if (op->cls == attrwrapper_cls) {
            if (key->cls == str_cls) {
                BoxedString* key_str = (BoxedString*)key;
                internStringMortalInplace(key_str);
                return unwrapAttrWrapper(op)->hasattr(key_str);
            }

            Box* rtn = PyObject_CallMethod(op, "__contains__", "O", key);
            if (!rtn)
                return -1;
            return rtn == True;
        }

        BoxedDict* mp = (BoxedDict*)op;
        assert(PyDict_Check(mp));
        return mp->getOrNull(key) ? 1 : 0;
    } catch (ExcInfo e) {
        setCAPIException(e);
        return -1;
    }
}


Box* dictNonzero(BoxedDict* self) {
    return boxBool(PyDict_Size(self) != 0);
}

Box* dictFromkeys(Box* cls, Box* iterable, Box* default_value) {
    auto rtn = new BoxedDict();
    if (PyAnySet_Check(iterable)) {
        for (auto&& elt : ((BoxedSet*)iterable)->s) {
            dictSetitem(rtn, elt.value, default_value);
        }
    } else {
        for (Box* e : iterable->pyElements()) {
            dictSetitem(rtn, e, default_value);
        }
    }

    return rtn;
}

Box* dictEq(BoxedDict* self, Box* _rhs) {
    if (!PyDict_Check(self))
        raiseExcHelper(TypeError, "descriptor '__eq__' requires a 'dict' object but received a '%s'",
                       getTypeName(self));

    if (_rhs->cls == attrwrapper_cls)
        _rhs = attrwrapperToDict(_rhs);

    if (!PyDict_Check(_rhs))
        return NotImplemented;

    BoxedDict* rhs = static_cast<BoxedDict*>(_rhs);

    if (PyDict_Size(self) != PyDict_Size(rhs))
        return False;

    for (auto&& p : (*self)) {
        Box* val = rhs->getOrNull(p.first);
        if (!val)
            return False;
        if (!PyEq()(p.second, val))
            return False;
    }

    return True;
}

Box* dictNe(BoxedDict* self, Box* _rhs) {
    Box* eq = dictEq(self, _rhs);
    if (eq == NotImplemented)
        return eq;
    if (eq == True)
        return False;
    return True;
}


extern "C" Box* dictNew(Box* _cls, BoxedTuple* args, BoxedDict* kwargs) {
    if (!PyType_Check(_cls))
        raiseExcHelper(TypeError, "dict.__new__(X): X is not a type object (%s)", getTypeName(_cls));

    BoxedClass* cls = static_cast<BoxedClass*>(_cls);
    if (!isSubclass(cls, dict_cls))
        raiseExcHelper(TypeError, "dict.__new__(%s): %s is not a subtype of dict", getNameOfClass(cls),
                       getNameOfClass(cls));

    return new (cls) BoxedDict();
}

void dictMerge(BoxedDict* self, Box* other) {
    if (PyDict_Check(other)) {
        BoxedDict* other_dict = (BoxedDict*)other;
        HCAttrs* attrs = other_dict->getHCAttrs();
        if (attrs) {
            RELEASE_ASSERT(attrs->hcls->type == HiddenClass::NORMAL || attrs->hcls->type == HiddenClass::SINGLETON, "");
            for (const auto& p : attrs->hcls->getStrAttrOffsets())
                dictSetitem(self, p.first, attrs->attr_list->attrs[p.second]);
        } else {
            if (other_dict->d) {
                for (const auto& p : *other_dict->d)
                    dictSetitem(self, p.first.value, p.second);
            }
        }
        return;
    }

    Box* keys;
    if (other->cls == attrwrapper_cls) {
        keys = attrwrapperKeys(other);
    } else {
        static BoxedString* keys_str = internStringImmortal("keys");
        CallattrFlags callattr_flags{.cls_only = false, .null_on_nonexistent = true, .argspec = ArgPassSpec(0) };
        keys = callattr(other, keys_str, callattr_flags, NULL, NULL, NULL, NULL, NULL);
    }
    assert(keys);

    for (Box* k : keys->pyElements()) {
        Box* v = getitemInternal<CXX>(other, k);
        dictSetitem(self, k, v);
    }
}

void dictMergeFromSeq2(BoxedDict* self, Box* other) {
    int idx = 0;

    // raises if not iterable
    for (const auto& element : other->pyElements()) {

        // should this check subclasses? anyway to check if something is iterable...
        if (element->cls == list_cls) {
            BoxedList* list = static_cast<BoxedList*>(element);
            if (list->size != 2)
                raiseExcHelper(ValueError, "dictionary update sequence element #%d has length %ld; 2 is required", idx,
                               list->size);

            dictSetitem(self, list->elts->elts[0], list->elts->elts[1]);
        } else if (element->cls == tuple_cls) {
            BoxedTuple* tuple = static_cast<BoxedTuple*>(element);
            if (tuple->size() != 2)
                raiseExcHelper(ValueError, "dictionary update sequence element #%d has length %ld; 2 is required", idx,
                               tuple->size());

            dictSetitem(self, tuple->elts[0], tuple->elts[1]);
        } else
            raiseExcHelper(TypeError, "cannot convert dictionary update sequence element #%d to a sequence", idx);

        idx++;
    }
}

extern "C" int PyDict_Merge(PyObject* a, PyObject* b, int override_) noexcept {
    try {
        if (a == NULL || !PyDict_Check(a) || b == NULL) {
            if (a && b && a->cls == attrwrapper_cls) {
                RELEASE_ASSERT(PyDict_Check(b) && override_ == 1, "");
                for (auto&& item : *(BoxedDict*)b) {
                    setitem(a, item.first, item.second);
                }
                return 0;
            }

            PyErr_BadInternalCall();
            return -1;
        }

        if (override_ != 1)
            Py_FatalError("unimplemented");

        dictMerge(static_cast<BoxedDict*>(a), b);
        return 0;
    } catch (ExcInfo e) {
        setCAPIException(e);
        return -1;
    }
}

Box* dictUpdate(BoxedDict* self, BoxedTuple* args, BoxedDict* kwargs) {
    assert(args->cls == tuple_cls);
    assert(!kwargs || kwargs->cls == dict_cls);

    RELEASE_ASSERT(args->size() <= 1, ""); // should throw a TypeError
    if (args->size()) {
        Box* arg = args->elts[0];
        static BoxedString* keys_str = internStringImmortal("keys");
        if (getattrInternal<ExceptionStyle::CXX>(arg, keys_str)) {
            dictMerge(self, arg);
        } else {
            dictMergeFromSeq2(self, arg);
        }
    }

    if (kwargs && PyDict_Size(kwargs))
        dictMerge(self, kwargs);

    return None;
}

extern "C" Box* dictInit(BoxedDict* self, BoxedTuple* args, BoxedDict* kwargs) {
    int args_sz = args->size();
    int kwargs_sz = kwargs ? PyDict_Size(kwargs) : 0;

    // CPython accepts a single positional and keyword arguments, in any combination
    if (args_sz > 1)
        raiseExcHelper(TypeError, "dict expected at most 1 arguments, got %d", args_sz);

    dictUpdate(self, args, kwargs);

    if (kwargs) {
        // handle keyword arguments by merging (possibly over positional entries per CPy)
        assert(kwargs->cls == dict_cls);

        for (const auto& p : *kwargs)
            dictSetitem(self, p.first, p.second);
    }

    return None;
}

BoxedDict::iterator BoxedDict::begin() {
    HCAttrs* attrs = getHCAttrs();
    if (attrs) {
        RELEASE_ASSERT(attrs->hcls->type == HiddenClass::NORMAL || attrs->hcls->type == HiddenClass::SINGLETON, "");
        return BoxedDict::iterator(this, attrs->hcls->getStrAttrOffsets().begin());
    }
    if (!d)
        return BoxedDict::iterator(this, BoxedDict::DictMap::iterator());
    return BoxedDict::iterator(this, d->begin());
}
BoxedDict::iterator BoxedDict::end() {
    HCAttrs* attrs = getHCAttrs();
    if (attrs) {
        RELEASE_ASSERT(attrs->hcls->type == HiddenClass::NORMAL || attrs->hcls->type == HiddenClass::SINGLETON, "");
        return iterator(this, attrs->hcls->getStrAttrOffsets().end());
    }
    if (!d)
        return BoxedDict::iterator(this, BoxedDict::DictMap::iterator());
    return BoxedDict::iterator(this, d->end());
}

void BoxedDict::gcHandler(GCVisitor* v, Box* b) {
    assert(PyDict_Check(b));

    Box::gcHandler(v, b);

    BoxedDict* d = (BoxedDict*)b;

    HCAttrs* attrs = d->getHCAttrs();
    if (attrs) {
        v->visit(&attrs->hcls);
        if (attrs->attr_list)
            v->visit(&attrs->attr_list);
        return;
    }

    for (auto p : *d) {
        v->visit(&p.first);
        v->visit(&p.second);
    }
}

void BoxedDictIterator::gcHandler(GCVisitor* v, Box* b) {
    Box::gcHandler(v, b);

    BoxedDictIterator* it = static_cast<BoxedDictIterator*>(b);
    v->visit(&it->d);
}

static int dict_init(PyObject* self, PyObject* args, PyObject* kwds) noexcept {
    assert(PyDict_Check(self));
    try {
        dictInit(static_cast<BoxedDict*>(self), static_cast<BoxedTuple*>(args), static_cast<BoxedDict*>(kwds));
    } catch (ExcInfo e) {
        setCAPIException(e);
        return -1;
    }
    return 0;
}

static Box* dict_repr(PyObject* self) noexcept {
    assert(PyDict_Check(self));
    try {
        return dictRepr(static_cast<BoxedDict*>(self));
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}

static int dict_print(PyObject* mp, FILE* fp, int flags) noexcept {
    Py_ssize_t any;
    int status;

    status = Py_ReprEnter((PyObject*)mp);
    if (status != 0) {
        if (status < 0)
            return status;
        Py_BEGIN_ALLOW_THREADS fprintf(fp, "{...}");
        Py_END_ALLOW_THREADS return 0;
    }

    Py_BEGIN_ALLOW_THREADS fprintf(fp, "{");
    Py_END_ALLOW_THREADS any = 0;
    for (auto&& entry : *(BoxedDict*)mp) {
        PyObject* pvalue = entry.second;
        if (pvalue != NULL) {
            /* Prevent PyObject_Repr from deleting value during
               key format */
            Py_INCREF(pvalue);
            if (any++ > 0) {
                Py_BEGIN_ALLOW_THREADS fprintf(fp, ", ");
                Py_END_ALLOW_THREADS
            }
            if (PyObject_Print((PyObject*)entry.first, fp, 0) != 0) {
                Py_DECREF(pvalue);
                Py_ReprLeave((PyObject*)mp);
                return -1;
            }
            Py_BEGIN_ALLOW_THREADS fprintf(fp, ": ");
            Py_END_ALLOW_THREADS if (PyObject_Print(pvalue, fp, 0) != 0) {
                Py_DECREF(pvalue);
                Py_ReprLeave((PyObject*)mp);
                return -1;
            }
            Py_DECREF(pvalue);
        }
    }
    Py_BEGIN_ALLOW_THREADS fprintf(fp, "}");
    Py_END_ALLOW_THREADS Py_ReprLeave((PyObject*)mp);
    return 0;
}

void BoxedDict::dealloc(Box* b) noexcept {
    assert(PyDict_Check(b));
    BoxedDict* d = (BoxedDict*)b;
    if (d->d)
        d->d->freeAllMemory();
}

// We use cpythons dictview implementation from dictobject.c
extern "C" PyObject* dictview_new(PyObject* dict, PyTypeObject* type) noexcept;
Box* dictViewKeys(BoxedDict* d) {
    Box* rtn = dictview_new(d, &PyDictKeys_Type);
    if (!rtn)
        throwCAPIException();
    return rtn;
}
Box* dictViewValues(BoxedDict* d) {
    Box* rtn = dictview_new(d, &PyDictValues_Type);
    if (!rtn)
        throwCAPIException();
    return rtn;
}
Box* dictViewItems(BoxedDict* d) {
    Box* rtn = dictview_new(d, &PyDictItems_Type);
    if (!rtn)
        throwCAPIException();
    return rtn;
}


// This function gets called from dictobject.c
extern "C" PyObject* dictiter_new(PyDictObject* dict, PyTypeObject* iter_type) noexcept {
    return new (iter_type) BoxedDictIterator((BoxedDict*)dict);
}


void setupDict() {
    static PyMappingMethods dict_as_mapping;
    dict_cls->tp_as_mapping = &dict_as_mapping;
    static PySequenceMethods dict_as_sequence;
    dict_cls->tp_as_sequence = &dict_as_sequence;

    dictiterkey_cls = BoxedClass::create(type_cls, object_cls, &BoxedDictIterator::gcHandler, 0, 0,
                                         sizeof(BoxedDictIterator), false, "dictionary-keyiterator");
    dictitervalue_cls = BoxedClass::create(type_cls, object_cls, &BoxedDictIterator::gcHandler, 0, 0,
                                           sizeof(BoxedDictIterator), false, "dictionary-valueiterator");
    dictiteritem_cls = BoxedClass::create(type_cls, object_cls, &BoxedDictIterator::gcHandler, 0, 0,
                                          sizeof(BoxedDictIterator), false, "dictionary-itemiterator");

    dictiterkey_cls->instances_are_nonzero = dictitervalue_cls->instances_are_nonzero
        = dictiteritem_cls->instances_are_nonzero = true;

    dict_cls->tp_dealloc = &BoxedDict::dealloc;
    dict_cls->tp_hash = PyObject_HashNotImplemented;
    dict_cls->has_safe_tp_dealloc = true;

    dict_cls->giveAttr("__len__", new BoxedFunction(FunctionMetadata::create((void*)dictLen, BOXED_INT, 1)));
    dict_cls->giveAttr("__new__", new BoxedFunction(FunctionMetadata::create((void*)dictNew, UNKNOWN, 1, true, true)));
    dict_cls->giveAttr("__init__", new BoxedFunction(FunctionMetadata::create((void*)dictInit, NONE, 1, true, true)));
    dict_cls->giveAttr("__repr__", new BoxedFunction(FunctionMetadata::create((void*)dictRepr, STR, 1)));

    dict_cls->giveAttr("__eq__", new BoxedFunction(FunctionMetadata::create((void*)dictEq, UNKNOWN, 2)));
    dict_cls->giveAttr("__ne__", new BoxedFunction(FunctionMetadata::create((void*)dictNe, UNKNOWN, 2)));
    dict_cls->giveAttr("__hash__", None);
    dict_cls->giveAttr("__iter__", new BoxedFunction(FunctionMetadata::create((void*)dictIterKeys,
                                                                              typeFromClass(dictiterkey_cls), 1)));

    dict_cls->giveAttr("update", new BoxedFunction(FunctionMetadata::create((void*)dictUpdate, NONE, 1, true, true)));

    dict_cls->giveAttr("clear", new BoxedFunction(FunctionMetadata::create((void*)dictClear, NONE, 1)));
    dict_cls->giveAttr("copy", new BoxedFunction(FunctionMetadata::create((void*)dictCopy, DICT, 1)));

    dict_cls->giveAttr("has_key", new BoxedFunction(FunctionMetadata::create((void*)dictContains, BOXED_BOOL, 2)));
    dict_cls->giveAttr("items", new BoxedFunction(FunctionMetadata::create((void*)dictItems, LIST, 1)));
    dict_cls->giveAttr("iteritems", new BoxedFunction(FunctionMetadata::create((void*)dictIterItems,
                                                                               typeFromClass(dictiteritem_cls), 1)));

    dict_cls->giveAttr("values", new BoxedFunction(FunctionMetadata::create((void*)dictValues, LIST, 1)));
    dict_cls->giveAttr("itervalues", new BoxedFunction(FunctionMetadata::create((void*)dictIterValues,
                                                                                typeFromClass(dictitervalue_cls), 1)));

    dict_cls->giveAttr("keys", new BoxedFunction(FunctionMetadata::create((void*)dictKeys, LIST, 1)));
    dict_cls->giveAttr("iterkeys", dict_cls->getattr(internStringMortal("__iter__")));

    dict_cls->giveAttr("pop",
                       new BoxedFunction(FunctionMetadata::create((void*)dictPop, UNKNOWN, 3, false, false), { NULL }));
    dict_cls->giveAttr("popitem", new BoxedFunction(FunctionMetadata::create((void*)dictPopitem, BOXED_TUPLE, 1)));

    auto* fromkeys_func
        = new BoxedFunction(FunctionMetadata::create((void*)dictFromkeys, DICT, 3, false, false), { None });
    dict_cls->giveAttr("fromkeys", boxInstanceMethod(dict_cls, fromkeys_func, dict_cls));

    dict_cls->giveAttr("viewkeys", new BoxedFunction(FunctionMetadata::create((void*)dictViewKeys, UNKNOWN, 1)));
    dict_cls->giveAttr("viewvalues", new BoxedFunction(FunctionMetadata::create((void*)dictViewValues, UNKNOWN, 1)));
    dict_cls->giveAttr("viewitems", new BoxedFunction(FunctionMetadata::create((void*)dictViewItems, UNKNOWN, 1)));

    dict_cls->giveAttr("get",
                       new BoxedFunction(FunctionMetadata::create((void*)dictGet, UNKNOWN, 3, false, false), { None }));

    dict_cls->giveAttr(
        "setdefault",
        new BoxedFunction(FunctionMetadata::create((void*)dictSetdefault, UNKNOWN, 3, false, false), { None }));

    auto dict_getitem = FunctionMetadata::create((void*)dictGetitem<CXX>, UNKNOWN, 2, ParamNames::empty(), CXX);
    dict_getitem->addVersion((void*)dictGetitem<CAPI>, UNKNOWN, CAPI);
    dict_cls->giveAttr("__getitem__", new BoxedFunction(dict_getitem));
    dict_cls->giveAttr("__setitem__", new BoxedFunction(FunctionMetadata::create((void*)dictSetitem, NONE, 3)));
    dict_cls->giveAttr("__delitem__", new BoxedFunction(FunctionMetadata::create((void*)dictDelitem, UNKNOWN, 2)));
    dict_cls->giveAttr("__contains__", new BoxedFunction(FunctionMetadata::create((void*)dictContains, BOXED_BOOL, 2)));

    dict_cls->giveAttr("__nonzero__", new BoxedFunction(FunctionMetadata::create((void*)dictNonzero, BOXED_BOOL, 1)));

    add_operators(dict_cls);
    dict_cls->freeze();

    // create the dictonary iterator types
    for (BoxedClass* iter_type : { dictiterkey_cls, dictitervalue_cls, dictiteritem_cls }) {
        FunctionMetadata* hasnext = FunctionMetadata::create((void*)dictIterHasnextUnboxed, BOOL, 1);
        hasnext->addVersion((void*)dictIterHasnext, BOXED_BOOL);
        iter_type->giveAttr("__hasnext__", new BoxedFunction(hasnext));
        iter_type->giveAttr(
            "__iter__", new BoxedFunction(FunctionMetadata::create((void*)dictIterIter, typeFromClass(iter_type), 1)));
        iter_type->giveAttr("next", new BoxedFunction(FunctionMetadata::create((void*)dictIterNext, UNKNOWN, 1)));
        iter_type->freeze();
        iter_type->tp_iter = PyObject_SelfIter;
        iter_type->tp_iternext = dictiter_next;
        iter_type->tp_flags &= ~Py_TPFLAGS_BASETYPE; // subclassing is not allowed
    }


    // Manually set some tp_* slots *after* calling freeze() -> fixup_slot_dispatchers().
    // fixup_slot_dispatchers will insert a wrapper like slot_tp_init into tp_init, which calls the python-level
    // __init__ function.  This is all well and good, until a C extension tries to subclass from dict and then
    // creates a new tp_init function which calls Py_DictType.tp_init().  That tp_init is slot_tp_init, which calls
    // self.__init__, which is the *subclasses* init function not dict's.
    //
    // This seems to happen pretty rarely, and only with dict, so for now let's just work around it by manually
    // setting the couple functions that get used.
    //
    // I'm not sure if CPython has a better mechanism for this, since I assume they allow having extension classes
    // subclass Python classes.
    dict_cls->tp_init = dict_init;
    dict_cls->tp_repr = dict_repr;
    dict_cls->tp_print = dict_print;
    dict_cls->tp_iter = dict_iter;

    dict_cls->tp_as_mapping->mp_length = (lenfunc)dict_length;
    dict_cls->tp_as_mapping->mp_subscript = (binaryfunc)dictGetitem<CAPI>;
    dict_cls->tp_as_mapping->mp_ass_subscript = (objobjargproc)dict_ass_sub;

    dict_cls->tp_as_sequence->sq_contains = (objobjproc)PyDict_Contains;

    PyType_Ready(&PyDictKeys_Type);
    PyType_Ready(&PyDictValues_Type);
    PyType_Ready(&PyDictItems_Type);
}

void teardownDict() {
}
}
