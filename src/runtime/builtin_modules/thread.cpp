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

#include <pthread.h>
#include <stddef.h>

#include "Python.h"
#include "pythread.h"

#include "capi/typeobject.h"
#include "core/threading.h"
#include "core/types.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

using namespace pyston::threading;

extern "C" void initthread();


static int initialized;
static void PyThread__init_thread(void); /* Forward */

extern "C" void PyThread_init_thread(void) noexcept {
#ifdef Py_DEBUG
    char* p = Py_GETENV("PYTHONTHREADDEBUG");

    if (p) {
        if (*p)
            thread_debug = atoi(p);
        else
            thread_debug = 1;
    }
#endif /* Py_DEBUG */
    if (initialized)
        return;
    initialized = 1;
    PyThread__init_thread();
}

/* Support for runtime thread stack size tuning.
   A value of 0 means using the platform's default stack size
   or the size specified by the THREAD_STACK_SIZE macro. */
static size_t _pythread_stacksize = 0;

#include "thread_pthread.h"

extern "C" size_t PyThread_get_stacksize(void) noexcept {
    return _pythread_stacksize;
}

/* Only platforms defining a THREAD_SET_STACKSIZE() macro
   in thread_<platform>.h support changing the stack size.
   Return 0 if stack size is valid,
      -1 if stack size value is invalid,
      -2 if setting stack size is not supported. */
extern "C" int PyThread_set_stacksize(size_t size) noexcept {
    // Pyston change: don't allow stack size change
    return -2;

#if defined(THREAD_SET_STACKSIZE)
    return THREAD_SET_STACKSIZE(size);
#else
    return -2;
#endif
}

namespace pyston {
void setupThread() {
    initthread();
    RELEASE_ASSERT(!PyErr_Occurred(), "");
    return;
}
}
