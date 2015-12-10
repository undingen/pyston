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

#include "codegen/unwinding.h"
#include "core/ast.h"
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
    BoxedFrame(FrameInfo* frame_info) __attribute__((visibility("default")))
    : frame_info(frame_info), _globals(NULL), _code(NULL), _locals(NULL), _back(NULL), _stmt(NULL) {
        assert(frame_info);
    }

public:
    FrameInfo* frame_info;

    Box* _globals;
    Box* _code;
    Box* _locals;
    Box* _back;
    AST_stmt* _stmt;


    bool hasExited() const { return frame_info == NULL; }


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
        v->visit(&f->_locals);
        v->visit(&f->_back);
    }

    static Box* code(Box* obj, void*) {
        auto f = static_cast<BoxedFrame*>(obj);

        if (!f->_code) {
            f->_code = (Box*)f->frame_info->md->getCode();
        }
        return f->_code;
    }

    static Box* locals(Box* obj, void*) {
        auto f = static_cast<BoxedFrame*>(obj);

        if (f->hasExited()) {
            return f->_locals;
        }

        return f->frame_info->getBoxedLocals();
    }

    static Box* globals(Box* obj, void*) {
        auto f = static_cast<BoxedFrame*>(obj);
        return f->_globals;
    }

    static Box* back(Box* obj, void*) {
        auto f = static_cast<BoxedFrame*>(obj);

        if (f->_back)
            return f->_back;

        if (!f->frame_info->back)
            f->_back = None;
        else
            f->_back = BoxedFrame::boxFrame(f->frame_info->back);
        return f->_back;
    }

    static Box* lineno(Box* obj, void*) {
        auto f = static_cast<BoxedFrame*>(obj);

        if (f->hasExited())
            return boxInt(f->_stmt->lineno);

        AST_stmt* stmt = f->frame_info->stmt;
        return boxInt(stmt->lineno);
    }

    DEFAULT_CLASS(frame_cls);


    static Box* boxFrame(FrameInfo* fi) {
        if (fi->frame_obj == NULL) {
            // auto md = it.getMD();
            Box* globals = fi->globals;
            if (globals && PyModule_Check(globals))
                globals = globals->getAttrWrapper();
            BoxedFrame* f = fi->frame_obj = new BoxedFrame(fi);
            f->_globals = globals;
        }
        assert(fi->frame_obj->cls == frame_cls);

        return fi->frame_obj;
    }

    void handleExit() {
        if (hasExited())
            return;

        _locals = frame_info->getVRegs();
        back(this, NULL);
        _stmt = frame_info->stmt;

        frame_info = NULL;
    }
};


Box* getFrame(FrameInfo* frame_info) {
    UNAVOIDABLE_STAT_TIMER(t0, "us_timer__getFrame_frame_info");
    return BoxedFrame::boxFrame(frame_info);
}

Box* getFrame(int depth) {
    UNAVOIDABLE_STAT_TIMER(t0, "us_timer__getFrame_depth");

    FrameInfo* frame_info = getPythonFrame(depth);
    if (!frame_info)
        return NULL;

    // printf("getFrame %i %p\n", depth, frame_info);

    BoxedFrame* rtn = (BoxedFrame*)BoxedFrame::boxFrame(frame_info);
    return rtn;
}

void updateFrameForDeopt(BoxedFrame* frame) {
    // assert(frame->cls == frame_cls);
    // RELEASE_ASSERT(!frame->exited, "");
    // frame->frame_info = getPythonFrame(0)->frame_info;
}

__thread int level = 0;


extern "C" void initFrame(FrameInfo* frame_info) {
    // UNAVOIDABLE_STAT_TIMER(t0, "us_timer__initFrame");

    // printf("initFrame %p %i\n", frame_info, ++level);
    // printf("initFrame %i\n", ++level);
    frame_info->back = (FrameInfo*)(cur_thread_state.frame_info);
    cur_thread_state.frame_info = frame_info;
}

void handleExit(BoxedFrame*);

extern "C" void deinitFrame(FrameInfo* frame_info) {
    // UNAVOIDABLE_STAT_TIMER(t0, "us_timer__deinitFrame");

    // printf("deinitFrame %p %i\n", frame_info, --level);
    // printf("deinitFrame %i\n", --level);
    cur_thread_state.frame_info = (FrameInfo*)frame_info->back;
    BoxedFrame* frame = frame_info->frame_obj;
    if (frame) {
        handleExit(frame);
    }
}

void handleExit(BoxedFrame* frame) {
    UNAVOIDABLE_STAT_TIMER(t0, "us_timer__handleExit");
    frame->handleExit();
}

void setupFrame() {
    frame_cls
        = BoxedClass::create(type_cls, object_cls, &BoxedFrame::gchandler, 0, 0, sizeof(BoxedFrame), false, "frame");
    frame_cls->has_safe_tp_dealloc = true;

    frame_cls->giveAttr("f_code", new (pyston_getset_cls) BoxedGetsetDescriptor(BoxedFrame::code, NULL, NULL));
    frame_cls->giveAttr("f_locals", new (pyston_getset_cls) BoxedGetsetDescriptor(BoxedFrame::locals, NULL, NULL));
    frame_cls->giveAttr("f_lineno", new (pyston_getset_cls) BoxedGetsetDescriptor(BoxedFrame::lineno, NULL, NULL));

    frame_cls->giveAttr("f_globals", new (pyston_getset_cls) BoxedGetsetDescriptor(BoxedFrame::globals, NULL, NULL));
    frame_cls->giveAttr("f_back", new (pyston_getset_cls) BoxedGetsetDescriptor(BoxedFrame::back, NULL, NULL));

    frame_cls->freeze();
}
}
