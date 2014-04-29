/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ART_RUNTIME_MIRROR_PROXY_H_
#define ART_RUNTIME_MIRROR_PROXY_H_

#include "object.h"

namespace art {

struct ProxyOffsets;

namespace mirror {

// All proxy objects have a class which is a synthesized proxy class. The synthesized proxy class
// has the static fields used to implement reflection on proxy objects.
class MANAGED SynthesizedProxyClass : public Class {
 public:
  ObjectArray<Class>* GetInterfaces() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return GetFieldObject<ObjectArray<Class> >(OFFSET_OF_OBJECT_MEMBER(SynthesizedProxyClass,
                                                                       interfaces_));
  }

  ObjectArray<ObjectArray<Class> >* GetThrows()  SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return GetFieldObject<ObjectArray<ObjectArray<Class> > >(OFFSET_OF_OBJECT_MEMBER(SynthesizedProxyClass,
                                                                                     throws_));
  }

 private:
  HeapReference<ObjectArray<Class> > interfaces_;
  HeapReference<ObjectArray<ObjectArray<Class> > > throws_;
  DISALLOW_IMPLICIT_CONSTRUCTORS(SynthesizedProxyClass);
};

// C++ mirror of java.lang.reflect.Proxy.
class MANAGED Proxy : public Object {
 private:
  HeapReference<Object> h_;

  friend struct art::ProxyOffsets;  // for verifying offset information
  DISALLOW_IMPLICIT_CONSTRUCTORS(Proxy);
};

}  // namespace mirror
}  // namespace art

#endif  // ART_RUNTIME_MIRROR_PROXY_H_
