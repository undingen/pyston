// This file is originally from CPython 2.7, with modifications for Pyston

/* Class object interface */

/* Revealing some structures (not for general use) */

#ifndef Py_CLASSOBJECT_H
#define Py_CLASSOBJECT_H
#ifdef __cplusplus
extern "C" {
#endif

//#include "Python.h"

// Pyston change:
struct _hcattrs2 {
    char _data[16];
};

typedef struct {
    PyObject_HEAD
    // Pyston change:
    struct _hcattrs2 hcattrs;
    PyObject	*cl_bases;	/* A tuple of class objects */
    // Pyston change:
    // PyObject	*cl_dict;	/* A dictionary */
    PyObject	*cl_name;	/* A string */
    /* The following three are functions or NULL */
    PyObject	*cl_getattr;
    PyObject	*cl_setattr;
    PyObject	*cl_delattr;
    PyObject    *cl_weakreflist; /* List of weak references */
} PyClassObject;

typedef struct {
    PyObject_HEAD
    // Pyston change:
    struct _hcattrs2 hcattrs;
    PyClassObject *in_class;	/* The class object */
    // Pyston change:
    // PyObject	  *in_dict;	/* A dictionary */
    PyObject	  *in_weakreflist; /* List of weak references */
} PyInstanceObject;

typedef struct {
    PyObject_HEAD
    PyObject *im_func;   /* The callable object implementing the method */
    PyObject *im_self;   /* The instance it is bound to, or NULL */
    PyObject *im_class;  /* The class that asked for the method */
    PyObject *im_weakreflist; /* List of weak references */
} PyMethodObject;

PyAPI_DATA(PyTypeObject) PyClass_Type, PyInstance_Type, PyMethod_Type;

// Pyston change:
// #define PyClass_Check(op) ((op)->ob_type == &PyClass_Type)
// #define PyInstance_Check(op) ((op)->ob_type == &PyInstance_Type)
// #define PyMethod_Check(op) ((op)->ob_type == &PyMethod_Type)

#define PyClass_Check(op) (Py_TYPE(op) == &PyClass_Type)
#define PyInstance_Check(op) (Py_TYPE(op) == &PyInstance_Type)
#define PyMethod_Check(op) (Py_TYPE(op) == &PyMethod_Type)

PyAPI_FUNC(PyObject *) PyClass_New(PyObject *, PyObject *, PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyInstance_New(PyObject *, PyObject *,
                                            PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyInstance_NewRaw(PyObject *, PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyMethod_New(PyObject *, PyObject *, PyObject *) PYSTON_NOEXCEPT;

PyAPI_FUNC(PyObject *) PyMethod_Function(PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyMethod_Self(PyObject *) PYSTON_NOEXCEPT;
PyAPI_FUNC(PyObject *) PyMethod_Class(PyObject *) PYSTON_NOEXCEPT;

/* Look up attribute with name (a string) on instance object pinst, using
 * only the instance and base class dicts.  If a descriptor is found in
 * a class dict, the descriptor is returned without calling it.
 * Returns NULL if nothing found, else a borrowed reference to the
 * value associated with name in the dict in which name was found.
 * The point of this routine is that it never calls arbitrary Python
 * code, so is always "safe":  all it does is dict lookups.  The function
 * can't fail, never sets an exception, and NULL is not an error (it just
 * means "not found").
 */
PyAPI_FUNC(PyObject *) _PyInstance_Lookup(PyObject *pinst, PyObject *name) PYSTON_NOEXCEPT;

/* Macros for direct access to these values. Type checks are *not*
   done, so use with care. */
#define PyMethod_GET_FUNCTION(meth) \
        (((PyMethodObject *)meth) -> im_func)
#define PyMethod_GET_SELF(meth) \
    (((PyMethodObject *)meth) -> im_self)
#define PyMethod_GET_CLASS(meth) \
    (((PyMethodObject *)meth) -> im_class)

PyAPI_FUNC(int) PyClass_IsSubclass(PyObject *, PyObject *) PYSTON_NOEXCEPT;

PyAPI_FUNC(int) PyMethod_ClearFreeList(void) PYSTON_NOEXCEPT;

#ifdef __cplusplus
}
#endif
#endif /* !Py_CLASSOBJECT_H */
