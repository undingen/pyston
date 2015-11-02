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

#include "Python.h"
#include "pythread.h"

#include "codegen/ast_interpreter.h"
#include "codegen/unwinding.h"
#include "core/ast.h"
#include "runtime/code.h"
#include "runtime/types.h"

namespace pyston {

BoxedClass* frame_cls;

// Issues:
// - breaks gdb backtraces
// - breaks c++ exceptions
// - we never free the trampolines
class BoxedFrame : public Box {
private:
    // Call boxFrame to get a BoxedFrame object.
    BoxedFrame() __attribute__((visibility("default")))
    : thread_id(PyThread_get_thread_ident()), vregs(NULL), _back(NULL), _locals(NULL), _stmt(NULL) {}

public:
    long thread_id;

    Box* _globals;
    Box* _code;
    Box** vregs;
    BoxedFrame* _back;
    Box* _locals;
    AST_stmt* _stmt;

    // cpython frame objects have the following attributes

    // read-only attributes
    //
    // f_back[*]       : previous stack frame (toward caller)
    // f_code          : code object being executed in this frame
    // f_locals        : dictionary used to look up local variables in this frame
    // f_globals       : dictionary used to look up global variables in this frame
    // f_builtins[*]   : dictionary to look up built-in (intrinsic) names
    // f_restricted[*] : whether this function is executing in restricted execution mode
    // f_lasti[*]      : precise instruction (this is an index into the bytecode string of the code object)

    // writable attributes
    //
    // f_trace[*]         : if not None, is a function called at the start of each source code line (used by debugger)
    // f_exc_type[*],     : represent the last exception raised in the parent frame provided another exception was
    // f_exc_value[*],    : ever raised in the current frame (in all other cases they are None).
    // f_exc_traceback[*] :
    // f_lineno[**]       : the current line number of the frame -- writing to this from within a trace function jumps
    // to
    //                    : the given line (only for the bottom-most frame).  A debugger can implement a Jump command
    //                    (aka
    //                    : Set Next Statement) by writing to f_lineno
    //
    // * = unsupported in Pyston
    // ** = getter supported, but setter unsupported

    static void gchandler(GCVisitor* v, Box* b) {
        Box::gcHandler(v, b);

        auto f = static_cast<BoxedFrame*>(b);

        v->visit(&f->_code);
        v->visit(&f->_globals);
        v->visit(&f->vregs);
        v->visit(&f->_back);
        v->visit(&f->_locals);
    }

    static void simpleDestructor(Box* b) {
        auto f = static_cast<BoxedFrame*>(b);

        // f->it.~PythonFrameIterator();
    }

    static Box* code(Box* obj, void*) {
        auto f = static_cast<BoxedFrame*>(obj);
        return f->_code;
    }

    static Box* locals(Box* obj, void*) {
        auto f = static_cast<BoxedFrame*>(obj);

        if (f->vregs) {
            CLFunction* clfunc = ((BoxedCode*)f->_code)->f;

            if (clfunc->source->scope_info->areLocalsFromModule())
                return clfunc->source->parent_module->getAttrWrapper();

            return localsForInterpretedFrame(f->vregs, clfunc->source->cfg, true);
        }
        return new BoxedDict;
    }

    static Box* globals(Box* obj, void*) {
        auto f = static_cast<BoxedFrame*>(obj);
        return f->_globals;
    }

    static Box* back(Box* obj, void*) {
        auto f = static_cast<BoxedFrame*>(obj);

        if (!f->_back)
            return None;
        return f->_back;
    }

    static Box* lineno(Box* obj, void*) {
        auto f = static_cast<BoxedFrame*>(obj);
        return boxInt(f->_stmt->lineno);
    }

    DEFAULT_CLASS(frame_cls);

    static Box* boxFrame(BoxedCode* code, Box** vregs, Box* next_frame, Box* globals) {
        auto frame = new BoxedFrame;
        frame->_code = (Box*)code;
        frame->vregs = vregs;
        frame->_back = (BoxedFrame*)next_frame;
        frame->_globals = globals;
        return frame;
    }
};

Box* getFrame(int depth) {
    BoxedFrame* f = (BoxedFrame*)PyThreadState_GET()->frame;
    while (depth > 0 && f != NULL) {
        f = f->_back;
        --depth;
    }
    RELEASE_ASSERT(f, "");
    return f;
}

Box* createFrame(BoxedCode* code, Box** vregs, Box* next_frame, Box* globals) {
    return BoxedFrame::boxFrame(code, vregs, next_frame, globals);
}

Box* backFrame(Box* frame) {
    if (!frame)
        return NULL;
    return ((BoxedFrame*)frame)->_back;
}

int countFrames(Box* frame) {
    if (!frame)
        return 0;
    return countFrames(((BoxedFrame*)frame)->_back) + 1;
}

void setupFrame() {
    frame_cls
        = BoxedClass::create(type_cls, object_cls, &BoxedFrame::gchandler, 0, 0, sizeof(BoxedFrame), false, "frame");
    frame_cls->tp_dealloc = BoxedFrame::simpleDestructor;
    frame_cls->has_safe_tp_dealloc = true;

    frame_cls->giveAttr("f_code", new (pyston_getset_cls) BoxedGetsetDescriptor(BoxedFrame::code, NULL, NULL));
    frame_cls->giveAttr("f_locals", new (pyston_getset_cls) BoxedGetsetDescriptor(BoxedFrame::locals, NULL, NULL));
    frame_cls->giveAttr("f_lineno", new (pyston_getset_cls) BoxedGetsetDescriptor(BoxedFrame::lineno, NULL, NULL));

    frame_cls->giveAttr("f_globals", new (pyston_getset_cls) BoxedGetsetDescriptor(BoxedFrame::globals, NULL, NULL));
    frame_cls->giveAttr("f_back", new (pyston_getset_cls) BoxedGetsetDescriptor(BoxedFrame::back, NULL, NULL));

    frame_cls->freeze();
}
}
