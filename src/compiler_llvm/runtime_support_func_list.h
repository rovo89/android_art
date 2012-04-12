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

#define RUNTIME_SUPPORT_FUNC_LIST(V) \
  V(LockObject, art_lock_object_from_code) \
  V(UnlockObject, art_unlock_object_from_code) \
  V(GetCurrentThread, art_get_current_thread_from_code) \
  V(SetCurrentThread, art_set_current_thread_from_code) \
  V(PushShadowFrame, art_push_shadow_frame_from_code) \
  V(PopShadowFrame, art_pop_shadow_frame_from_code) \
  V(TestSuspend, art_test_suspend_from_code) \
  V(ThrowException, art_throw_exception_from_code) \
  V(ThrowStackOverflowException, art_throw_stack_overflow_from_code) \
  V(ThrowNullPointerException, art_throw_null_pointer_exception_from_code) \
  V(ThrowDivZeroException, art_throw_div_zero_from_code) \
  V(ThrowIndexOutOfBounds, art_throw_array_bounds_from_code) \
  V(ThrowVerificationError, art_throw_verification_error_from_code) \
  V(InitializeTypeAndVerifyAccess, art_initialize_type_and_verify_access_from_code) \
  V(InitializeType, art_initialize_type_from_code) \
  V(IsAssignable, art_is_assignable_from_code) \
  V(CheckCast, art_check_cast_from_code) \
  V(CheckPutArrayElement, art_check_put_array_element_from_code) \
  V(AllocObject, art_alloc_object_from_code) \
  V(AllocObjectWithAccessCheck, art_alloc_object_from_code_with_access_check) \
  V(AllocArray, art_alloc_array_from_code) \
  V(AllocArrayWithAccessCheck, art_alloc_array_from_code_with_access_check) \
  V(CheckAndAllocArray, art_check_and_alloc_array_from_code) \
  V(CheckAndAllocArrayWithAccessCheck, art_check_and_alloc_array_from_code_with_access_check) \
  V(FindStaticMethodWithAccessCheck, art_find_static_method_from_code_with_access_check) \
  V(FindDirectMethodWithAccessCheck, art_find_direct_method_from_code_with_access_check) \
  V(FindVirtualMethodWithAccessCheck, art_find_virtual_method_from_code_with_access_check) \
  V(FindSuperMethodWithAccessCheck, art_find_super_method_from_code_with_access_check) \
  V(FindInterfaceMethodWithAccessCheck, art_find_interface_method_from_code_with_access_check) \
  V(FindInterfaceMethod, art_find_interface_method_from_code) \
  V(ResolveString, art_resolve_string_from_code) \
  V(Set32Static, art_set32_static_from_code) \
  V(Set64Static, art_set64_static_from_code) \
  V(SetObjectStatic, art_set_obj_static_from_code) \
  V(Get32Static, art_get32_static_from_code) \
  V(Get64Static, art_get64_static_from_code) \
  V(GetObjectStatic, art_get_obj_static_from_code) \
  V(Set32Instance, art_set32_instance_from_code) \
  V(Set64Instance, art_set64_instance_from_code) \
  V(SetObjectInstance, art_set_obj_instance_from_code) \
  V(Get32Instance, art_get32_instance_from_code) \
  V(Get64Instance, art_get64_instance_from_code) \
  V(GetObjectInstance, art_get_obj_instance_from_code) \
  V(InitializeStaticStorage, art_initialize_static_storage_from_code) \
  V(IsExceptionPending, art_is_exception_pending_from_code) \
  V(FindCatchBlock, art_find_catch_block_from_code) \
  V(EnsureResolved, art_ensure_resolved_from_code) \
  V(FixStub, art_fix_stub_from_code) \
  V(ProxyInvokeHandler, art_proxy_invoke_handler_from_code) \
  V(DecodeJObjectInThread, art_decode_jobject_in_thread) \
  V(D2L, D2L) \
  V(D2I, D2I) \
  V(F2L, F2L) \
  V(F2I, F2I)
