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

#include <stdarg.h>
#include <string.h>

#include "Python.h"

#include "capi/typeobject.h"
#include "capi/types.h"
#include "core/ast.h"
#include "core/threading.h"
#include "core/types.h"
#include "runtime/classobj.h"
#include "runtime/import.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

namespace pyston {

static PyObject* abstract_get_bases(PyObject* cls) noexcept {
    PyObject* bases;

    static PyObject* __bases__ = NULL;
    if (__bases__ == NULL) {
        __bases__ = getStaticString("__bases__");
        if (__bases__ == NULL)
            return NULL;
    }

    bases = PyObject_GetAttr(cls, __bases__);
    if (bases == NULL) {
        if (PyErr_ExceptionMatches(PyExc_AttributeError))
            PyErr_Clear();
        return NULL;
    }
    if (!PyTuple_Check(bases)) {
        Py_DECREF(bases);
        return NULL;
    }
    return bases;
}

static int abstract_issubclass(PyObject* derived, PyObject* cls) noexcept {
    PyObject* bases = NULL;
    Py_ssize_t i, n;
    int r = 0;

    while (1) {
        if (derived == cls)
            return 1;
        bases = abstract_get_bases(derived);
        if (bases == NULL) {
            if (PyErr_Occurred())
                return -1;
            return 0;
        }
        n = PyTuple_GET_SIZE(bases);
        if (n == 0) {
            Py_DECREF(bases);
            return 0;
        }
        /* Avoid recursivity in the single inheritance case */
        if (n == 1) {
            derived = PyTuple_GET_ITEM(bases, 0);
            Py_DECREF(bases);
            continue;
        }
        for (i = 0; i < n; i++) {
            r = abstract_issubclass(PyTuple_GET_ITEM(bases, i), cls);
            if (r != 0)
                break;
        }
        Py_DECREF(bases);
        return r;
    }
}

static int check_class(PyObject* cls, const char* error) noexcept {
    PyObject* bases = abstract_get_bases(cls);
    if (bases == NULL) {
        /* Do not mask errors. */
        if (!PyErr_Occurred())
            PyErr_SetString(PyExc_TypeError, error);
        return 0;
    }
    Py_DECREF(bases);
    return -1;
}

static int recursive_isinstance(PyObject* inst, PyObject* cls) noexcept {
    PyObject* icls;
    int retval = 0;

    static PyObject* __class__ = NULL;
    if (__class__ == NULL) {
        __class__ = getStaticString("__class__");
        if (__class__ == NULL)
            return -1;
    }

    if (PyClass_Check(cls) && PyInstance_Check(inst)) {
        PyObject* inclass = static_cast<BoxedInstance*>(inst)->inst_cls;
        retval = PyClass_IsSubclass(inclass, cls);
    } else if (PyType_Check(cls)) {
        retval = PyObject_TypeCheck(inst, (PyTypeObject*)cls);
        if (retval == 0) {
            PyObject* c = NULL;

            if (!inst->cls->has_getattribute) {
                assert(inst->cls->tp_getattr == object_cls->tp_getattr);
                assert(inst->cls->tp_getattro == object_cls->tp_getattro
                       || inst->cls->tp_getattro == slot_tp_getattr_hook);
            }
            // We don't need to worry about __getattr__, since the default __class__ will always resolve.
            bool has_custom_class = inst->cls->has___class__ || inst->cls->has_getattribute;
            if (!has_custom_class) {
                assert(autoDecref(PyObject_GetAttr(inst, __class__)) == inst->cls);
            } else {
                c = PyObject_GetAttr(inst, __class__);
                if (!c)
                    PyErr_Clear();
            }

            if (c) {
                if (c != (PyObject*)(inst->cls) && PyType_Check(c))
                    retval = PyType_IsSubtype((PyTypeObject*)c, (PyTypeObject*)cls);
                Py_DECREF(c);
            }
        }
    } else {
        if (!check_class(cls, "isinstance() arg 2 must be a class, type,"
                              " or tuple of classes and types"))
            return -1;
        icls = PyObject_GetAttr(inst, __class__);
        if (icls == NULL) {
            PyErr_Clear();
            retval = 0;
        } else {
            retval = abstract_issubclass(icls, cls);
            Py_DECREF(icls);
        }
    }

    return retval;
}

extern "C" int PyObject_IsInstance(PyObject* inst, PyObject* cls) noexcept {
    STAT_TIMER(t0, "us_timer_pyobject_isinstance", 20);

    static PyObject* name = NULL;

    /* Quick test for an exact match */
    if (Py_TYPE(inst) == (PyTypeObject*)cls)
        return 1;

    if (PyTuple_Check(cls)) {
        Py_ssize_t i;
        Py_ssize_t n;
        int r = 0;

        if (Py_EnterRecursiveCall(" in __instancecheck__"))
            return -1;
        n = PyTuple_GET_SIZE(cls);
        for (i = 0; i < n; ++i) {
            PyObject* item = PyTuple_GET_ITEM(cls, i);
            r = PyObject_IsInstance(inst, item);
            if (r != 0)
                /* either found it, or got an error */
                break;
        }
        Py_LeaveRecursiveCall();
        return r;
    }

    if (!(PyClass_Check(cls) || PyInstance_Check(cls))) {
        PyObject* checker = NULL;
        if (cls->cls->has_instancecheck) {
            checker = _PyObject_LookupSpecial(cls, "__instancecheck__", &name);
            if (!checker && PyErr_Occurred())
                return -1;

            assert(checker);
        }

        if (checker != NULL) {
            PyObject* res;
            int ok = -1;
            if (Py_EnterRecursiveCall(" in __instancecheck__")) {
                Py_DECREF(checker);
                return ok;
            }
            res = PyObject_CallFunctionObjArgs(checker, inst, NULL);
            Py_LeaveRecursiveCall();
            Py_DECREF(checker);
            if (res != NULL) {
                ok = PyObject_IsTrue(res);
                Py_DECREF(res);
            }
            return ok;
        }
    }
    return recursive_isinstance(inst, cls);
}

static int recursive_issubclass(PyObject* derived, PyObject* cls) noexcept {
    int retval;

    if (PyType_Check(cls) && PyType_Check(derived)) {
        /* Fast path (non-recursive) */
        return PyType_IsSubtype((PyTypeObject*)derived, (PyTypeObject*)cls);
    }
    if (!PyClass_Check(derived) || !PyClass_Check(cls)) {
        if (!check_class(derived, "issubclass() arg 1 must be a class"))
            return -1;

        if (!check_class(cls, "issubclass() arg 2 must be a class"
                              " or tuple of classes"))
            return -1;
        retval = abstract_issubclass(derived, cls);
    } else {
        /* shortcut */
        if (!(retval = (derived == cls)))
            retval = PyClass_IsSubclass(derived, cls);
    }

    return retval;
}

extern "C" int PyObject_IsSubclass(PyObject* derived, PyObject* cls) noexcept {
    static PyObject* name = NULL;

    if (PyTuple_Check(cls)) {
        Py_ssize_t i;
        Py_ssize_t n;
        int r = 0;

        if (Py_EnterRecursiveCall(" in __subclasscheck__"))
            return -1;
        n = PyTuple_GET_SIZE(cls);
        for (i = 0; i < n; ++i) {
            PyObject* item = PyTuple_GET_ITEM(cls, i);
            r = PyObject_IsSubclass(derived, item);
            if (r != 0)
                /* either found it, or got an error */
                break;
        }
        Py_LeaveRecursiveCall();
        return r;
    }
    if (!(PyClass_Check(cls) || PyInstance_Check(cls))) {
        PyObject* checker = NULL;
        if (cls->cls->has_subclasscheck) {
            checker = _PyObject_LookupSpecial(cls, "__subclasscheck__", &name);
            if (!checker && PyErr_Occurred())
                return -1;

            assert(checker);
        }

        if (checker != NULL) {
            PyObject* res;
            int ok = -1;
            if (Py_EnterRecursiveCall(" in __subclasscheck__")) {
                Py_DECREF(checker);
                return ok;
            }
            res = PyObject_CallFunctionObjArgs(checker, derived, NULL);
            Py_LeaveRecursiveCall();
            Py_DECREF(checker);
            if (res != NULL) {
                ok = PyObject_IsTrue(res);
                Py_DECREF(res);
            }
            return ok;
        } else if (PyErr_Occurred()) {
            return -1;
        }
    }
    return recursive_issubclass(derived, cls);
}

extern "C" Py_ssize_t PyObject_Size(PyObject* o) noexcept {
    BoxedInt* r = lenInternal<ExceptionStyle::CAPI, NOT_REWRITABLE>(o, NULL);
    if (!r)
        return -1;
    AUTO_DECREF(r);
    return r->n;
}

extern "C" PyObject* PyObject_GetIter(PyObject* o) noexcept {
    try {
        return getiter(o);
    } catch (ExcInfo e) {
        setCAPIException(e);
        return NULL;
    }
}
}
