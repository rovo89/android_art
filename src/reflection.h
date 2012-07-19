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

#ifndef ART_SRC_REFLECTION_H_
#define ART_SRC_REFLECTION_H_

#include "jni.h"
#include "primitive.h"

namespace art {

class Class;
class Field;
union JValue;
class Method;
class Object;
class ScopedObjectAccess;

void BoxPrimitive(Primitive::Type src_class, JValue& value)
    SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_);
bool UnboxPrimitiveForArgument(Object* o, Class* dst_class, JValue& unboxed_value, Method* m,
                               size_t index)
    SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_);
bool UnboxPrimitiveForField(Object* o, Class* dst_class, JValue& unboxed_value, Field* f)
    SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_);
bool UnboxPrimitiveForResult(Object* o, Class* dst_class, JValue& unboxed_value)
    SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_);

bool ConvertPrimitiveValue(Primitive::Type src_class, Primitive::Type dst_class, const JValue& src,
                           JValue& dst)
    SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_);

jobject InvokeMethod(const ScopedObjectAccess& soa, jobject method, jobject receiver, jobject args)
    SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_);

bool VerifyObjectInClass(Object* o, Class* c)
    SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_);

}  // namespace art

#endif  // ART_SRC_REFLECTION_H_
