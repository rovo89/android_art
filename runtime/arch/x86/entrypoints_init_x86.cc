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

#include "entrypoints/portable/portable_entrypoints.h"
#include "entrypoints/quick/quick_entrypoints.h"
#include "entrypoints/entrypoint_utils.h"

namespace art {

// Alloc entrypoints.
extern "C" void* art_quick_alloc_array_from_code(uint32_t, void*, int32_t);
extern "C" void* art_quick_alloc_array_from_code_with_access_check(uint32_t, void*, int32_t);
extern "C" void* art_quick_alloc_object_from_code(uint32_t type_idx, void* method);
extern "C" void* art_quick_alloc_object_from_code_with_access_check(uint32_t type_idx, void* method);
extern "C" void* art_quick_check_and_alloc_array_from_code(uint32_t, void*, int32_t);
extern "C" void* art_quick_check_and_alloc_array_from_code_with_access_check(uint32_t, void*, int32_t);

// Cast entrypoints.
extern "C" uint32_t art_quick_is_assignable_from_code(const mirror::Class* klass,
                                                const mirror::Class* ref_class);
extern "C" void art_quick_can_put_array_element_from_code(void*, void*);
extern "C" void art_quick_check_cast_from_code(void*, void*);

// DexCache entrypoints.
extern "C" void* art_quick_initialize_static_storage_from_code(uint32_t, void*);
extern "C" void* art_quick_initialize_type_from_code(uint32_t, void*);
extern "C" void* art_quick_initialize_type_and_verify_access_from_code(uint32_t, void*);
extern "C" void* art_quick_resolve_string_from_code(void*, uint32_t);

// Field entrypoints.
extern "C" int art_quick_set32_instance_from_code(uint32_t, void*, int32_t);
extern "C" int art_quick_set32_static_from_code(uint32_t, int32_t);
extern "C" int art_quick_set64_instance_from_code(uint32_t, void*, int64_t);
extern "C" int art_quick_set64_static_from_code(uint32_t, int64_t);
extern "C" int art_quick_set_obj_instance_from_code(uint32_t, void*, void*);
extern "C" int art_quick_set_obj_static_from_code(uint32_t, void*);
extern "C" int32_t art_quick_get32_instance_from_code(uint32_t, void*);
extern "C" int32_t art_quick_get32_static_from_code(uint32_t);
extern "C" int64_t art_quick_get64_instance_from_code(uint32_t, void*);
extern "C" int64_t art_quick_get64_static_from_code(uint32_t);
extern "C" void* art_quick_get_obj_instance_from_code(uint32_t, void*);
extern "C" void* art_quick_get_obj_static_from_code(uint32_t);

// FillArray entrypoint.
extern "C" void art_quick_handle_fill_data_from_code(void*, void*);

// Lock entrypoints.
extern "C" void art_quick_lock_object_from_code(void*);
extern "C" void art_quick_unlock_object_from_code(void*);

// Math entrypoints.
extern "C" double art_quick_fmod_from_code(double, double);
extern "C" float art_quick_fmodf_from_code(float, float);
extern "C" double art_quick_l2d_from_code(int64_t);
extern "C" float art_quick_l2f_from_code(int64_t);
extern "C" int64_t art_quick_d2l_from_code(double);
extern "C" int64_t art_quick_f2l_from_code(float);
extern "C" int32_t art_quick_idivmod_from_code(int32_t, int32_t);
extern "C" int64_t art_quick_ldiv_from_code(int64_t, int64_t);
extern "C" int64_t art_quick_ldivmod_from_code(int64_t, int64_t);
extern "C" int64_t art_quick_lmul_from_code(int64_t, int64_t);
extern "C" uint64_t art_quick_lshl_from_code(uint64_t, uint32_t);
extern "C" uint64_t art_quick_lshr_from_code(uint64_t, uint32_t);
extern "C" uint64_t art_quick_lushr_from_code(uint64_t, uint32_t);

// Interpreter entrypoints.
extern "C" void artInterpreterToInterpreterEntry(Thread* self, MethodHelper& mh,
                                                 const DexFile::CodeItem* code_item,
                                                 ShadowFrame* shadow_frame, JValue* result);
extern "C" void artInterpreterToQuickEntry(Thread* self, MethodHelper& mh,
                                           const DexFile::CodeItem* code_item,
                                           ShadowFrame* shadow_frame, JValue* result);

// Intrinsic entrypoints.
extern "C" int32_t art_quick_memcmp16(void*, void*, int32_t);
extern "C" int32_t art_quick_indexof(void*, uint32_t, uint32_t, uint32_t);
extern "C" int32_t art_quick_string_compareto(void*, void*);
extern "C" void* art_quick_memcpy(void*, const void*, size_t);

// Invoke entrypoints.
extern "C" const void* artPortableResolutionTrampoline(mirror::AbstractMethod* called,
                                                       mirror::Object* receiver,
                                                       mirror::AbstractMethod** sp, Thread* thread);
extern "C" const void* artQuickResolutionTrampoline(mirror::AbstractMethod* called,
                                                    mirror::Object* receiver,
                                                    mirror::AbstractMethod** sp, Thread* thread);
extern "C" void art_quick_invoke_direct_trampoline_with_access_check(uint32_t, void*);
extern "C" void art_quick_invoke_interface_trampoline(uint32_t, void*);
extern "C" void art_quick_invoke_interface_trampoline_with_access_check(uint32_t, void*);
extern "C" void art_quick_invoke_static_trampoline_with_access_check(uint32_t, void*);
extern "C" void art_quick_invoke_super_trampoline_with_access_check(uint32_t, void*);
extern "C" void art_quick_invoke_virtual_trampoline_with_access_check(uint32_t, void*);

// Thread entrypoints.
extern void CheckSuspendFromCode(Thread* thread);
extern "C" void art_quick_test_suspend();

// Throw entrypoints.
extern "C" void art_quick_deliver_exception_from_code(void*);
extern "C" void art_quick_throw_array_bounds_from_code(int32_t index, int32_t limit);
extern "C" void art_quick_throw_div_zero_from_code();
extern "C" void art_quick_throw_no_such_method_from_code(int32_t method_idx);
extern "C" void art_quick_throw_null_pointer_exception_from_code();
extern "C" void art_quick_throw_stack_overflow_from_code(void*);

void InitEntryPoints(QuickEntryPoints* qpoints, PortableEntryPoints* ppoints) {
  // Alloc
  qpoints->pAllocArrayFromCode = art_quick_alloc_array_from_code;
  qpoints->pAllocArrayFromCodeWithAccessCheck = art_quick_alloc_array_from_code_with_access_check;
  qpoints->pAllocObjectFromCode = art_quick_alloc_object_from_code;
  qpoints->pAllocObjectFromCodeWithAccessCheck = art_quick_alloc_object_from_code_with_access_check;
  qpoints->pCheckAndAllocArrayFromCode = art_quick_check_and_alloc_array_from_code;
  qpoints->pCheckAndAllocArrayFromCodeWithAccessCheck = art_quick_check_and_alloc_array_from_code_with_access_check;

  // Cast
  qpoints->pInstanceofNonTrivialFromCode = art_quick_is_assignable_from_code;
  qpoints->pCanPutArrayElementFromCode = art_quick_can_put_array_element_from_code;
  qpoints->pCheckCastFromCode = art_quick_check_cast_from_code;

  // DexCache
  qpoints->pInitializeStaticStorage = art_quick_initialize_static_storage_from_code;
  qpoints->pInitializeTypeAndVerifyAccessFromCode = art_quick_initialize_type_and_verify_access_from_code;
  qpoints->pInitializeTypeFromCode = art_quick_initialize_type_from_code;
  qpoints->pResolveStringFromCode = art_quick_resolve_string_from_code;

  // Field
  qpoints->pSet32Instance = art_quick_set32_instance_from_code;
  qpoints->pSet32Static = art_quick_set32_static_from_code;
  qpoints->pSet64Instance = art_quick_set64_instance_from_code;
  qpoints->pSet64Static = art_quick_set64_static_from_code;
  qpoints->pSetObjInstance = art_quick_set_obj_instance_from_code;
  qpoints->pSetObjStatic = art_quick_set_obj_static_from_code;
  qpoints->pGet32Instance = art_quick_get32_instance_from_code;
  qpoints->pGet64Instance = art_quick_get64_instance_from_code;
  qpoints->pGetObjInstance = art_quick_get_obj_instance_from_code;
  qpoints->pGet32Static = art_quick_get32_static_from_code;
  qpoints->pGet64Static = art_quick_get64_static_from_code;
  qpoints->pGetObjStatic = art_quick_get_obj_static_from_code;

  // FillArray
  qpoints->pHandleFillArrayDataFromCode = art_quick_handle_fill_data_from_code;

  // JNI
  qpoints->pJniMethodStart = JniMethodStart;
  qpoints->pJniMethodStartSynchronized = JniMethodStartSynchronized;
  qpoints->pJniMethodEnd = JniMethodEnd;
  qpoints->pJniMethodEndSynchronized = JniMethodEndSynchronized;
  qpoints->pJniMethodEndWithReference = JniMethodEndWithReference;
  qpoints->pJniMethodEndWithReferenceSynchronized = JniMethodEndWithReferenceSynchronized;

  // Locks
  qpoints->pLockObjectFromCode = art_quick_lock_object_from_code;
  qpoints->pUnlockObjectFromCode = art_quick_unlock_object_from_code;

  // Math
  // points->pCmpgDouble = NULL;  // Not needed on x86.
  // points->pCmpgFloat = NULL;  // Not needed on x86.
  // points->pCmplDouble = NULL;  // Not needed on x86.
  // points->pCmplFloat = NULL;  // Not needed on x86.
  qpoints->pFmod = art_quick_fmod_from_code;
  qpoints->pL2d = art_quick_l2d_from_code;
  qpoints->pFmodf = art_quick_fmodf_from_code;
  qpoints->pL2f = art_quick_l2f_from_code;
  // points->pD2iz = NULL;  // Not needed on x86.
  // points->pF2iz = NULL;  // Not needed on x86.
  qpoints->pIdivmod = art_quick_idivmod_from_code;
  qpoints->pD2l = art_quick_d2l_from_code;
  qpoints->pF2l = art_quick_f2l_from_code;
  qpoints->pLdiv = art_quick_ldiv_from_code;
  qpoints->pLdivmod = art_quick_ldivmod_from_code;
  qpoints->pLmul = art_quick_lmul_from_code;
  qpoints->pShlLong = art_quick_lshl_from_code;
  qpoints->pShrLong = art_quick_lshr_from_code;
  qpoints->pUshrLong = art_quick_lushr_from_code;

  // Interpreter
  qpoints->pInterpreterToInterpreterEntry = artInterpreterToInterpreterEntry;
  qpoints->pInterpreterToQuickEntry = artInterpreterToQuickEntry;

  // Intrinsics
  qpoints->pIndexOf = art_quick_indexof;
  qpoints->pMemcmp16 = art_quick_memcmp16;
  qpoints->pStringCompareTo = art_quick_string_compareto;
  qpoints->pMemcpy = art_quick_memcpy;

  // Invocation
  qpoints->pQuickResolutionTrampolineFromCode = artQuickResolutionTrampoline;
  qpoints->pInvokeDirectTrampolineWithAccessCheck = art_quick_invoke_direct_trampoline_with_access_check;
  qpoints->pInvokeInterfaceTrampoline = art_quick_invoke_interface_trampoline;
  qpoints->pInvokeInterfaceTrampolineWithAccessCheck = art_quick_invoke_interface_trampoline_with_access_check;
  qpoints->pInvokeStaticTrampolineWithAccessCheck = art_quick_invoke_static_trampoline_with_access_check;
  qpoints->pInvokeSuperTrampolineWithAccessCheck = art_quick_invoke_super_trampoline_with_access_check;
  qpoints->pInvokeVirtualTrampolineWithAccessCheck = art_quick_invoke_virtual_trampoline_with_access_check;

  // Thread
  qpoints->pCheckSuspendFromCode = CheckSuspendFromCode;
  qpoints->pTestSuspendFromCode = art_quick_test_suspend;

  // Throws
  qpoints->pDeliverException = art_quick_deliver_exception_from_code;
  qpoints->pThrowArrayBoundsFromCode = art_quick_throw_array_bounds_from_code;
  qpoints->pThrowDivZeroFromCode = art_quick_throw_div_zero_from_code;
  qpoints->pThrowNoSuchMethodFromCode = art_quick_throw_no_such_method_from_code;
  qpoints->pThrowNullPointerFromCode = art_quick_throw_null_pointer_exception_from_code;
  qpoints->pThrowStackOverflowFromCode = art_quick_throw_stack_overflow_from_code;

  // Portable
  ppoints->pPortableResolutionTrampolineFromCode = artPortableResolutionTrampoline;
};

}  // namespace art
