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

#include "oat/runtime/oat_support_entrypoints.h"
#include "runtime_support.h"

namespace art {

// Alloc entrypoints.
extern "C" void* art_alloc_array_from_code(uint32_t, void*, int32_t);
extern "C" void* art_alloc_array_from_code_with_access_check(uint32_t, void*, int32_t);
extern "C" void* art_alloc_object_from_code(uint32_t type_idx, void* method);
extern "C" void* art_alloc_object_from_code_with_access_check(uint32_t type_idx, void* method);
extern "C" void* art_check_and_alloc_array_from_code(uint32_t, void*, int32_t);
extern "C" void* art_check_and_alloc_array_from_code_with_access_check(uint32_t, void*, int32_t);

// Cast entrypoints.
extern "C" uint32_t art_is_assignable_from_code(const Class* klass, const Class* ref_class);
extern "C" void art_can_put_array_element_from_code(void*, void*);
extern "C" void art_check_cast_from_code(void*, void*);

// Debug entrypoints.
extern void DebugMe(Method* method, uint32_t info);

// DexCache entrypoints.
extern "C" void* art_initialize_static_storage_from_code(uint32_t, void*);
extern "C" void* art_initialize_type_from_code(uint32_t, void*);
extern "C" void* art_initialize_type_and_verify_access_from_code(uint32_t, void*);
extern "C" void* art_resolve_string_from_code(void*, uint32_t);

// Field entrypoints.
extern "C" int art_set32_instance_from_code(uint32_t, void*, int32_t);
extern "C" int art_set32_static_from_code(uint32_t, int32_t);
extern "C" int art_set64_instance_from_code(uint32_t, void*, int64_t);
extern "C" int art_set64_static_from_code(uint32_t, int64_t);
extern "C" int art_set_obj_instance_from_code(uint32_t, void*, void*);
extern "C" int art_set_obj_static_from_code(uint32_t, void*);
extern "C" int32_t art_get32_instance_from_code(uint32_t, void*);
extern "C" int32_t art_get32_static_from_code(uint32_t);
extern "C" int64_t art_get64_instance_from_code(uint32_t, void*);
extern "C" int64_t art_get64_static_from_code(uint32_t);
extern "C" void* art_get_obj_instance_from_code(uint32_t, void*);
extern "C" void* art_get_obj_static_from_code(uint32_t);

// FillArray entrypoint.
extern "C" void art_handle_fill_data_from_code(void*, void*);

// JNI entrypoints.
extern void* FindNativeMethod(Thread* thread);
extern uint32_t JniMethodStart(Thread* self);
extern uint32_t JniMethodStartSynchronized(jobject to_lock, Thread* self);
extern void JniMethodEnd(uint32_t saved_local_ref_cookie, Thread* self);
extern void JniMethodEndSynchronized(uint32_t saved_local_ref_cookie, jobject locked,
                                     Thread* self);
extern Object* JniMethodEndWithReference(jobject result, uint32_t saved_local_ref_cookie,
                                         Thread* self);
extern Object* JniMethodEndWithReferenceSynchronized(jobject result,
                                                     uint32_t saved_local_ref_cookie,
                                                     jobject locked, Thread* self);

// Lock entrypoints.
extern "C" void art_lock_object_from_code(void*);
extern "C" void art_unlock_object_from_code(void*);

// Math entrypoints.
extern "C" double art_fmod_from_code(double, double);
extern "C" float art_fmodf_from_code(float, float);
extern "C" double art_l2d_from_code(int64_t);
extern "C" float art_l2f_from_code(int64_t);
extern "C" int64_t art_d2l_from_code(double);
extern "C" int64_t art_f2l_from_code(float);
extern "C" int32_t art_idivmod_from_code(int32_t, int32_t);
extern "C" int64_t art_ldiv_from_code(int64_t, int64_t);
extern "C" int64_t art_ldivmod_from_code(int64_t, int64_t);
extern "C" int64_t art_lmul_from_code(int64_t, int64_t);
extern "C" uint64_t art_lshl_from_code(uint64_t, uint32_t);
extern "C" uint64_t art_lshr_from_code(uint64_t, uint32_t);
extern "C" uint64_t art_lushr_from_code(uint64_t, uint32_t);

// Intrinsic entrypoints.
extern "C" int32_t art_memcmp16(void*, void*, int32_t);
extern "C" int32_t art_indexof(void*, uint32_t, uint32_t, uint32_t);
extern "C" int32_t art_string_compareto(void*, void*);
extern "C" void* art_memcpy(void*, const void*, size_t);

// Invoke entrypoints.
const void* UnresolvedDirectMethodTrampolineFromCode(Method*, Method**, Thread*,
                                                     Runtime::TrampolineType);
extern "C" void art_invoke_direct_trampoline_with_access_check(uint32_t, void*);
extern "C" void art_invoke_interface_trampoline(uint32_t, void*);
extern "C" void art_invoke_interface_trampoline_with_access_check(uint32_t, void*);
extern "C" void art_invoke_static_trampoline_with_access_check(uint32_t, void*);
extern "C" void art_invoke_super_trampoline_with_access_check(uint32_t, void*);
extern "C" void art_invoke_virtual_trampoline_with_access_check(uint32_t, void*);

// Thread entrypoints.
extern void CheckSuspendFromCode(Thread* thread);
extern "C" void art_test_suspend();

// Throw entrypoints.
extern void ThrowAbstractMethodErrorFromCode(Method* method, Thread* thread, Method** sp);
extern "C" void art_deliver_exception_from_code(void*);
extern "C" void art_throw_array_bounds_from_code(int32_t index, int32_t limit);
extern "C" void art_throw_div_zero_from_code();
extern "C" void art_throw_no_such_method_from_code(int32_t method_idx);
extern "C" void art_throw_null_pointer_exception_from_code();
extern "C" void art_throw_stack_overflow_from_code(void*);
extern "C" void art_throw_verification_error_from_code(int32_t src1, int32_t ref);

void InitEntryPoints(EntryPoints* points) {
  // Alloc
  points->pAllocArrayFromCode = art_alloc_array_from_code;
  points->pAllocArrayFromCodeWithAccessCheck = art_alloc_array_from_code_with_access_check;
  points->pAllocObjectFromCode = art_alloc_object_from_code;
  points->pAllocObjectFromCodeWithAccessCheck = art_alloc_object_from_code_with_access_check;
  points->pCheckAndAllocArrayFromCode = art_check_and_alloc_array_from_code;
  points->pCheckAndAllocArrayFromCodeWithAccessCheck = art_check_and_alloc_array_from_code_with_access_check;

  // Cast
  points->pInstanceofNonTrivialFromCode = art_is_assignable_from_code;
  points->pCanPutArrayElementFromCode = art_can_put_array_element_from_code;
  points->pCheckCastFromCode = art_check_cast_from_code;

  // Debug
  points->pDebugMe = DebugMe;
  points->pUpdateDebuggerFromCode = NULL; // Controlled by SetDebuggerUpdatesEnabled.

  // DexCache
  points->pInitializeStaticStorage = art_initialize_static_storage_from_code;
  points->pInitializeTypeAndVerifyAccessFromCode = art_initialize_type_and_verify_access_from_code;
  points->pInitializeTypeFromCode = art_initialize_type_from_code;
  points->pResolveStringFromCode = art_resolve_string_from_code;

  // Field
  points->pSet32Instance = art_set32_instance_from_code;
  points->pSet32Static = art_set32_static_from_code;
  points->pSet64Instance = art_set64_instance_from_code;
  points->pSet64Static = art_set64_static_from_code;
  points->pSetObjInstance = art_set_obj_instance_from_code;
  points->pSetObjStatic = art_set_obj_static_from_code;
  points->pGet32Instance = art_get32_instance_from_code;
  points->pGet64Instance = art_get64_instance_from_code;
  points->pGetObjInstance = art_get_obj_instance_from_code;
  points->pGet32Static = art_get32_static_from_code;
  points->pGet64Static = art_get64_static_from_code;
  points->pGetObjStatic = art_get_obj_static_from_code;

  // FillArray
  points->pHandleFillArrayDataFromCode = art_handle_fill_data_from_code;

  // JNI
  points->pFindNativeMethod = FindNativeMethod;
  points->pJniMethodStart = JniMethodStart;
  points->pJniMethodStartSynchronized = JniMethodStartSynchronized;
  points->pJniMethodEnd = JniMethodEnd;
  points->pJniMethodEndSynchronized = JniMethodEndSynchronized;
  points->pJniMethodEndWithReference = JniMethodEndWithReference;
  points->pJniMethodEndWithReferenceSynchronized = JniMethodEndWithReferenceSynchronized;

  // Locks
  points->pLockObjectFromCode = art_lock_object_from_code;
  points->pUnlockObjectFromCode = art_unlock_object_from_code;

  // Math
  //points->pCmpgDouble = NULL; // Not needed on x86.
  //points->pCmpgFloat = NULL; // Not needed on x86.
  //points->pCmplDouble = NULL; // Not needed on x86.
  //points->pCmplFloat = NULL; // Not needed on x86.
  //points->pDadd = NULL; // Not needed on x86.
  //points->pDdiv = NULL; // Not needed on x86.
  //points->pDmul = NULL; // Not needed on x86.
  //points->pDsub = NULL; // Not needed on x86.
  //points->pF2d = NULL;
  points->pFmod = art_fmod_from_code;
  //points->pI2d = NULL;
  points->pL2d = art_l2d_from_code;
  //points->pD2f = NULL;
  //points->pFadd = NULL; // Not needed on x86.
  //points->pFdiv = NULL; // Not needed on x86.
  points->pFmodf = art_fmodf_from_code;
  //points->pFmul = NULL; // Not needed on x86.
  //points->pFsub = NULL; // Not needed on x86.
  //points->pI2f = NULL;
  points->pL2f = art_l2f_from_code;
  //points->pD2iz = NULL; // Not needed on x86.
  //points->pF2iz = NULL; // Not needed on x86.
  points->pIdivmod = art_idivmod_from_code;
  points->pD2l = art_d2l_from_code;
  points->pF2l = art_f2l_from_code;
  points->pLdiv = art_ldiv_from_code;
  points->pLdivmod = art_ldivmod_from_code;
  points->pLmul = art_lmul_from_code;
  points->pShlLong = art_lshl_from_code;
  points->pShrLong = art_lshr_from_code;
  points->pUshrLong = art_lushr_from_code;

  // Intrinsics
  points->pIndexOf = art_indexof;
  points->pMemcmp16 = art_memcmp16;
  points->pStringCompareTo = art_string_compareto;
  points->pMemcpy = art_memcpy;

  // Invocation
  points->pUnresolvedDirectMethodTrampolineFromCode = UnresolvedDirectMethodTrampolineFromCode;
  points->pInvokeDirectTrampolineWithAccessCheck = art_invoke_direct_trampoline_with_access_check;
  points->pInvokeInterfaceTrampoline = art_invoke_interface_trampoline;
  points->pInvokeInterfaceTrampolineWithAccessCheck = art_invoke_interface_trampoline_with_access_check;
  points->pInvokeStaticTrampolineWithAccessCheck = art_invoke_static_trampoline_with_access_check;
  points->pInvokeSuperTrampolineWithAccessCheck = art_invoke_super_trampoline_with_access_check;
  points->pInvokeVirtualTrampolineWithAccessCheck = art_invoke_virtual_trampoline_with_access_check;

  // Thread
  points->pCheckSuspendFromCode = CheckSuspendFromCode;
  points->pTestSuspendFromCode = art_test_suspend;

  // Throws
  points->pDeliverException = art_deliver_exception_from_code;
  points->pThrowAbstractMethodErrorFromCode = ThrowAbstractMethodErrorFromCode;
  points->pThrowArrayBoundsFromCode = art_throw_array_bounds_from_code;
  points->pThrowDivZeroFromCode = art_throw_div_zero_from_code;
  points->pThrowNoSuchMethodFromCode = art_throw_no_such_method_from_code;
  points->pThrowNullPointerFromCode = art_throw_null_pointer_exception_from_code;
  points->pThrowStackOverflowFromCode = art_throw_stack_overflow_from_code;
  points->pThrowVerificationErrorFromCode = art_throw_verification_error_from_code;
};

void ChangeDebuggerEntryPoint(EntryPoints*, bool) {
  UNIMPLEMENTED(FATAL);
}

bool IsTraceExitPc(uintptr_t) {
  return false;
}

void* GetLogTraceEntryPoint() {
  return NULL;
}

}  // namespace art
