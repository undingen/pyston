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
#include "runtime/objmodel.h"
#include "runtime/types.h"

namespace pyston {

extern "C" Box* createDict() {
    return new BoxedDict();
}

extern "C" Box* createList() {
    return new BoxedList();
}

extern "C" Box* createTuple1(Box* elt1) {
    return BoxedTuple::create1(elt1);
}
extern "C" Box* createTuple2(Box* elt1, Box* elt2) {
    return BoxedTuple::create2(elt1, elt2);
}
extern "C" Box* createTuple3(Box* elt1, Box* elt2, Box* elt3) {
    return BoxedTuple::create3(elt1, elt2, elt3);
}
extern "C" Box* createTuple5(Box* elt1, Box* elt2, Box* elt3, Box* elt4, Box* elt5) {
    return BoxedTuple::create5(elt1, elt2, elt3, elt4, elt5);
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

// BoxedInt::BoxedInt(int64_t n) : Box(int_cls), n(n) {}
}
