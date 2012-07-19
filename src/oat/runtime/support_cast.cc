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

#include "callee_save_frame.h"
#include "runtime_support.h"

namespace art {

// Assignable test for code, won't throw.  Null and equality tests already performed
extern "C" uint32_t artIsAssignableFromCode(const Class* klass, const Class* ref_class)
    SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_) {
  DCHECK(klass != NULL);
  DCHECK(ref_class != NULL);
  return klass->IsAssignableFrom(ref_class) ? 1 : 0;
}

// Check whether it is safe to cast one class to the other, throw exception and return -1 on failure
extern "C" int artCheckCastFromCode(const Class* a, const Class* b, Thread* self, Method** sp)
    SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_) {
  DCHECK(a->IsClass()) << PrettyClass(a);
  DCHECK(b->IsClass()) << PrettyClass(b);
  if (LIKELY(b->IsAssignableFrom(a))) {
    return 0;  // Success
  } else {
    FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
    Thread::Current()->ThrowNewExceptionF("Ljava/lang/ClassCastException;",
        "%s cannot be cast to %s",
        PrettyDescriptor(a).c_str(),
        PrettyDescriptor(b).c_str());
    return -1;  // Failure
  }
}

// Tests whether 'element' can be assigned into an array of type 'array_class'.
// Returns 0 on success and -1 if an exception is pending.
extern "C" int artCanPutArrayElementFromCode(const Object* element, const Class* array_class,
                                             Thread* self, Method** sp)
    SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_) {
  DCHECK(array_class != NULL);
  // element can't be NULL as we catch this is screened in runtime_support
  Class* element_class = element->GetClass();
  Class* component_type = array_class->GetComponentType();
  if (LIKELY(component_type->IsAssignableFrom(element_class))) {
    return 0;  // Success
  } else {
    FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsOnly);
    Thread::Current()->ThrowNewExceptionF("Ljava/lang/ArrayStoreException;",
        "%s cannot be stored in an array of type %s",
        PrettyDescriptor(element_class).c_str(),
        PrettyDescriptor(array_class).c_str());
    return -1;  // Failure
  }
}

}  // namespace art
