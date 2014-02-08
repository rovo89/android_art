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

#include "entrypoints/entrypoint_utils.h"
#include "mirror/art_method-inl.h"
#include "mirror/dex_cache-inl.h"
#include "mirror/object-inl.h"

namespace art {

template<InvokeType type, bool access_check>
mirror::ArtMethod* FindMethodHelper(uint32_t method_idx, mirror::Object* this_object,
                                    mirror::ArtMethod* caller_method, Thread* thread) {
  mirror::ArtMethod* method = FindMethodFast(method_idx, this_object, caller_method,
                                             access_check, type);
  if (UNLIKELY(method == NULL)) {
    method = FindMethodFromCode<type, access_check>(method_idx, this_object, caller_method, thread);
    if (UNLIKELY(method == NULL)) {
      CHECK(thread->IsExceptionPending());
      return 0;  // failure
    }
  }
  DCHECK(!thread->IsExceptionPending());
  const void* code = method->GetEntryPointFromPortableCompiledCode();

  // When we return, the caller will branch to this address, so it had better not be 0!
  if (UNLIKELY(code == NULL)) {
      MethodHelper mh(method);
      LOG(FATAL) << "Code was NULL in method: " << PrettyMethod(method)
                 << " location: " << mh.GetDexFile().GetLocation();
  }
  return method;
}

// Explicit template declarations of FindMethodHelper for all invoke types.
#define EXPLICIT_FIND_METHOD_HELPER_TEMPLATE_DECL(_type, _access_check)                        \
  template SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)                                         \
  mirror::ArtMethod* FindMethodHelper<_type, _access_check>(uint32_t method_idx,               \
                                                            mirror::Object* this_object,       \
                                                            mirror::ArtMethod* caller_method,  \
                                                            Thread* thread)
#define EXPLICIT_FIND_METHOD_HELPER_TYPED_TEMPLATE_DECL(_type) \
    EXPLICIT_FIND_METHOD_HELPER_TEMPLATE_DECL(_type, false);   \
    EXPLICIT_FIND_METHOD_HELPER_TEMPLATE_DECL(_type, true)

EXPLICIT_FIND_METHOD_HELPER_TYPED_TEMPLATE_DECL(kStatic);
EXPLICIT_FIND_METHOD_HELPER_TYPED_TEMPLATE_DECL(kDirect);
EXPLICIT_FIND_METHOD_HELPER_TYPED_TEMPLATE_DECL(kVirtual);
EXPLICIT_FIND_METHOD_HELPER_TYPED_TEMPLATE_DECL(kSuper);
EXPLICIT_FIND_METHOD_HELPER_TYPED_TEMPLATE_DECL(kInterface);

#undef EXPLICIT_FIND_METHOD_HELPER_TYPED_TEMPLATE_DECL
#undef EXPLICIT_FIND_METHOD_HELPER_TEMPLATE_DECL

extern "C" mirror::Object* art_portable_find_static_method_from_code_with_access_check(uint32_t method_idx,
                                                                                       mirror::Object* this_object,
                                                                                       mirror::ArtMethod* referrer,
                                                                                       Thread* thread)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return FindMethodHelper<kStatic, true>(method_idx, this_object, referrer, thread);
}

extern "C" mirror::Object* art_portable_find_direct_method_from_code_with_access_check(uint32_t method_idx,
                                                                                       mirror::Object* this_object,
                                                                                       mirror::ArtMethod* referrer,
                                                                                       Thread* thread)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return FindMethodHelper<kDirect, true>(method_idx, this_object, referrer, thread);
}

extern "C" mirror::Object* art_portable_find_virtual_method_from_code_with_access_check(uint32_t method_idx,
                                                                                        mirror::Object* this_object,
                                                                                        mirror::ArtMethod* referrer,
                                                                                        Thread* thread)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return FindMethodHelper<kVirtual, true>(method_idx, this_object, referrer, thread);
}

extern "C" mirror::Object* art_portable_find_super_method_from_code_with_access_check(uint32_t method_idx,
                                                                                      mirror::Object* this_object,
                                                                                      mirror::ArtMethod* referrer,
                                                                                      Thread* thread)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return FindMethodHelper<kSuper, true>(method_idx, this_object, referrer, thread);
}

extern "C" mirror::Object* art_portable_find_interface_method_from_code_with_access_check(uint32_t method_idx,
                                                                                          mirror::Object* this_object,
                                                                                          mirror::ArtMethod* referrer,
                                                                                          Thread* thread)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return FindMethodHelper<kInterface, true>(method_idx, this_object, referrer, thread);
}

extern "C" mirror::Object* art_portable_find_interface_method_from_code(uint32_t method_idx,
                                                                        mirror::Object* this_object,
                                                                        mirror::ArtMethod* referrer,
                                                                        Thread* thread)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return FindMethodHelper<kInterface, false>(method_idx, this_object, referrer, thread);
}

}  // namespace art
