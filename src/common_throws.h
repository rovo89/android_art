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

#include "mutex.h"
#include "object.h"

namespace art {

// NullPointerException

void ThrowNullPointerExceptionForFieldAccess(Field* field, bool is_read)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

void ThrowNullPointerExceptionForMethodAccess(AbstractMethod* caller, uint32_t method_idx, InvokeType type)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

void ThrowNullPointerExceptionFromDexPC(AbstractMethod* throw_method, uint32_t dex_pc)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

// IllegalAccessError

void ThrowIllegalAccessErrorClass(Class* referrer, Class* accessed)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

void ThrowIllegalAccessErrorClassForMethodDispatch(Class* referrer, Class* accessed,
                                                   const AbstractMethod* caller, const AbstractMethod* called,
                                                   InvokeType type)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

void ThrowIllegalAccessErrorMethod(Class* referrer, AbstractMethod* accessed)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

void ThrowIllegalAccessErrorField(Class* referrer, Field* accessed)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

void ThrowIllegalAccessErrorFinalField(const AbstractMethod* referrer, Field* accessed)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

// IncompatibleClassChangeError

void ThrowIncompatibleClassChangeError(InvokeType expected_type, InvokeType found_type,
                                       AbstractMethod* method, const AbstractMethod* referrer)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

void ThrowIncompatibleClassChangeErrorClassForInterfaceDispatch(const AbstractMethod* interface_method,
                                                                Object* this_object,
                                                                const AbstractMethod* referrer)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

void ThrowIncompatibleClassChangeErrorField(const Field* resolved_field, bool is_static,
                                            const AbstractMethod* referrer)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

// NoSuchMethodError

void ThrowNoSuchMethodError(InvokeType type, Class* c, const StringPiece& name,
                            const StringPiece& signature, const AbstractMethod* referrer)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

void ThrowNoSuchMethodError(uint32_t method_idx, const AbstractMethod* referrer)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

}  // namespace art

#endif  // ART_SRC_COMMON_THROWS_H_
