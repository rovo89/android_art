/*
 * Copyright (C) 2012 The Android Open Source Project
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

#ifndef ART_SRC_COMMON_THROWS__H_
#define ART_SRC_COMMON_THROWS_H_

#include "base/mutex.h"
#include "invoke_type.h"

namespace art {
namespace mirror {
class AbstractMethod;
class Class;
class Field;
class Object;
}  // namespace mirror
class StringPiece;

// NullPointerException

void ThrowNullPointerExceptionForFieldAccess(mirror::Field* field, bool is_read)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

void ThrowNullPointerExceptionForMethodAccess(mirror::AbstractMethod* caller, uint32_t method_idx,
                                              InvokeType type)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

void ThrowNullPointerExceptionFromDexPC(mirror::AbstractMethod* throw_method, uint32_t dex_pc)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

// IllegalAccessError

void ThrowIllegalAccessErrorClass(mirror::Class* referrer, mirror::Class* accessed)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

void ThrowIllegalAccessErrorClassForMethodDispatch(mirror::Class* referrer, mirror::Class* accessed,
                                                   const mirror::AbstractMethod* caller,
                                                   const mirror::AbstractMethod* called,
                                                   InvokeType type)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

void ThrowIllegalAccessErrorMethod(mirror::Class* referrer, mirror::AbstractMethod* accessed)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

void ThrowIllegalAccessErrorField(mirror::Class* referrer, mirror::Field* accessed)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

void ThrowIllegalAccessErrorFinalField(const mirror::AbstractMethod* referrer,
                                       mirror::Field* accessed)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

// IncompatibleClassChangeError

void ThrowIncompatibleClassChangeError(InvokeType expected_type, InvokeType found_type,
                                       mirror::AbstractMethod* method,
                                       const mirror::AbstractMethod* referrer)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

void ThrowIncompatibleClassChangeErrorClassForInterfaceDispatch(const mirror::AbstractMethod* interface_method,
                                                                mirror::Object* this_object,
                                                                const mirror::AbstractMethod* referrer)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

void ThrowIncompatibleClassChangeErrorField(const mirror::Field* resolved_field, bool is_static,
                                            const mirror::AbstractMethod* referrer)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

// NoSuchMethodError

void ThrowNoSuchMethodError(InvokeType type, mirror::Class* c, const StringPiece& name,
                            const StringPiece& signature, const mirror::AbstractMethod* referrer)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

void ThrowNoSuchMethodError(uint32_t method_idx, const mirror::AbstractMethod* referrer)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

}  // namespace art

#endif  // ART_SRC_COMMON_THROWS_H_
