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
#include "entrypoints/math_entrypoints.h"

namespace art {

// Alloc entrypoints.
extern "C" void* art_quick_alloc_array_from_code(uint32_t, void*, int32_t);
extern "C" void* art_quick_alloc_array_from_code_with_access_check(uint32_t, void*, int32_t);
extern "C" void* art_quick_alloc_object_from_code(uint32_t type_idx, void* method);
extern "C" void* art_quick_alloc_object_from_code_with_access_check(uint32_t type_idx, void* method);
extern "C" void* art_quick_check_and_alloc_array_from_code(uint32_t, void*, int32_t);
extern "C" void* art_quick_check_and_alloc_array_from_code_with_access_check(uint32_t, void*, int32_t);

// Cast entrypoints.
extern "C" uint32_t artIsAssignableFromCode(const mirror::Class* klass,
                                            const mirror::Class* ref_class);
extern "C" void art_quick_can_put_array_element_from_code(void*, void*);
extern "C" void art_quick_check_cast_from_code(void*, void*);

// DexCache entrypoints.
extern "C" void* art_quick_initialize_static_storage_from_code(uint32_t, void*);
extern "C" void* art_quick_initialize_type_from_code(uint32_t, void*);
extern "C" void* art_quick_initialize_type_and_verify_access_from_code(uint32_t, void*);
extern "C" void* art_quick_resolve_string_from_code(void*, uint32_t);

// Exception entrypoints.
extern "C" void* GetAndClearException(Thread*);

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
extern int32_t CmpgDouble(double a, double b);
extern int32_t CmplDouble(double a, double b);
extern int32_t CmpgFloat(float a, float b);
extern int32_t CmplFloat(float a, float b);

// Math conversions.
extern "C" int32_t __aeabi_f2iz(float op1);        // FLOAT_TO_INT
extern "C" int32_t __aeabi_d2iz(double op1);       // DOUBLE_TO_INT
extern "C" float __aeabi_l2f(int64_t op1);         // LONG_TO_FLOAT
extern "C" double __aeabi_l2d(int64_t op1);        // LONG_TO_DOUBLE

// Single-precision FP arithmetics.
extern "C" float fmodf(float a, float b);          // REM_FLOAT[_2ADDR]

// Double-precision FP arithmetics.
extern "C" double fmod(double a, double b);         // REM_DOUBLE[_2ADDR]

// Integer arithmetics.
extern "C" int __aeabi_idivmod(int32_t, int32_t);  // [DIV|REM]_INT[_2ADDR|_LIT8|_LIT16]

// Long long arithmetics - REM_LONG[_2ADDR] and DIV_LONG[_2ADDR]
extern "C" int64_t __aeabi_ldivmod(int64_t, int64_t);
extern "C" int64_t art_quick_mul_long(int64_t, int64_t);
extern "C" uint64_t art_quick_shl_long(uint64_t, uint32_t);
extern "C" uint64_t art_quick_shr_long(uint64_t, uint32_t);
extern "C" uint64_t art_quick_ushr_long(uint64_t, uint32_t);

// Interpreter entrypoints.
extern "C" void artInterpreterToInterpreterEntry(Thread* self, MethodHelper& mh,
                                                 const DexFile::CodeItem* code_item,
                                                 ShadowFrame* shadow_frame, JValue* result);
extern "C" void artInterpreterToQuickEntry(Thread* self, MethodHelper& mh,
                                           const DexFile::CodeItem* code_item,
                                           ShadowFrame* shadow_frame, JValue* result);

// Intrinsic entrypoints.
extern "C" int32_t __memcmp16(void*, void*, int32_t);
extern "C" int32_t art_quick_indexof(void*, uint32_t, uint32_t, uint32_t);
extern "C" int32_t art_quick_string_compareto(void*, void*);

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
  qpoints->pInstanceofNonTrivialFromCode = artIsAssignableFromCode;
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
  qpoints->pCmpgDouble = CmpgDouble;
  qpoints->pCmpgFloat = CmpgFloat;
  qpoints->pCmplDouble = CmplDouble;
  qpoints->pCmplFloat = CmplFloat;
  qpoints->pFmod = fmod;
  qpoints->pSqrt = sqrt;
  qpoints->pL2d = __aeabi_l2d;
  qpoints->pFmodf = fmodf;
  qpoints->pL2f = __aeabi_l2f;
  qpoints->pD2iz = __aeabi_d2iz;
  qpoints->pF2iz = __aeabi_f2iz;
  qpoints->pIdivmod = __aeabi_idivmod;
  qpoints->pD2l = art_d2l;
  qpoints->pF2l = art_f2l;
  qpoints->pLdiv = __aeabi_ldivmod;
  qpoints->pLdivmod = __aeabi_ldivmod;  // result returned in r2:r3
  qpoints->pLmul = art_quick_mul_long;
  qpoints->pShlLong = art_quick_shl_long;
  qpoints->pShrLong = art_quick_shr_long;
  qpoints->pUshrLong = art_quick_ushr_long;

  // Interpreter
  qpoints->pInterpreterToInterpreterEntry = artInterpreterToInterpreterEntry;
  qpoints->pInterpreterToQuickEntry = artInterpreterToQuickEntry;

  // Intrinsics
  qpoints->pIndexOf = art_quick_indexof;
  qpoints->pMemcmp16 = __memcmp16;
  qpoints->pStringCompareTo = art_quick_string_compareto;
  qpoints->pMemcpy = memcpy;

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
