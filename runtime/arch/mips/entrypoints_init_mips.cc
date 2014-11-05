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

#include "atomic.h"
#include "entrypoints/interpreter/interpreter_entrypoints.h"
#include "entrypoints/jni/jni_entrypoints.h"
#include "entrypoints/portable/portable_entrypoints.h"
#include "entrypoints/quick/quick_alloc_entrypoints.h"
#include "entrypoints/quick/quick_default_externs.h"
#include "entrypoints/quick/quick_entrypoints.h"
#include "entrypoints/entrypoint_utils.h"
#include "entrypoints/math_entrypoints.h"
#include "entrypoints/runtime_asm_entrypoints.h"
#include "interpreter/interpreter.h"

namespace art {

// Cast entrypoints.
extern "C" uint32_t artIsAssignableFromCode(const mirror::Class* klass,
                                            const mirror::Class* ref_class);

// Math entrypoints.
extern int32_t CmpgDouble(double a, double b);
extern int32_t CmplDouble(double a, double b);
extern int32_t CmpgFloat(float a, float b);
extern int32_t CmplFloat(float a, float b);
extern "C" int64_t artLmul(int64_t a, int64_t b);
extern "C" int64_t artLdiv(int64_t a, int64_t b);
extern "C" int64_t artLmod(int64_t a, int64_t b);

// Math conversions.
extern "C" int32_t __fixsfsi(float op1);      // FLOAT_TO_INT
extern "C" int32_t __fixdfsi(double op1);     // DOUBLE_TO_INT
extern "C" float __floatdisf(int64_t op1);    // LONG_TO_FLOAT
extern "C" double __floatdidf(int64_t op1);   // LONG_TO_DOUBLE
extern "C" int64_t __fixsfdi(float op1);      // FLOAT_TO_LONG
extern "C" int64_t __fixdfdi(double op1);     // DOUBLE_TO_LONG

// Single-precision FP arithmetics.
extern "C" float fmodf(float a, float b);      // REM_FLOAT[_2ADDR]

// Double-precision FP arithmetics.
extern "C" double fmod(double a, double b);     // REM_DOUBLE[_2ADDR]

// Long long arithmetics - REM_LONG[_2ADDR] and DIV_LONG[_2ADDR]
extern "C" int64_t __divdi3(int64_t, int64_t);
extern "C" int64_t __moddi3(int64_t, int64_t);

void InitEntryPoints(InterpreterEntryPoints* ipoints, JniEntryPoints* jpoints,
                     PortableEntryPoints* ppoints, QuickEntryPoints* qpoints) {
  // Interpreter
  ipoints->pInterpreterToInterpreterBridge = artInterpreterToInterpreterBridge;
  ipoints->pInterpreterToCompiledCodeBridge = artInterpreterToCompiledCodeBridge;

  // JNI
  jpoints->pDlsymLookup = art_jni_dlsym_lookup_stub;

  // Portable
  ppoints->pPortableResolutionTrampoline = art_portable_resolution_trampoline;
  ppoints->pPortableToInterpreterBridge = art_portable_to_interpreter_bridge;

  // Alloc
  ResetQuickAllocEntryPoints(qpoints);

  // Cast
  qpoints->pInstanceofNonTrivial = artIsAssignableFromCode;
  qpoints->pCheckCast = art_quick_check_cast;

  // DexCache
  qpoints->pInitializeStaticStorage = art_quick_initialize_static_storage;
  qpoints->pInitializeTypeAndVerifyAccess = art_quick_initialize_type_and_verify_access;
  qpoints->pInitializeType = art_quick_initialize_type;
  qpoints->pResolveString = art_quick_resolve_string;

  // Field
  qpoints->pSet8Instance = art_quick_set8_instance;
  qpoints->pSet8Static = art_quick_set8_static;
  qpoints->pSet16Instance = art_quick_set16_instance;
  qpoints->pSet16Static = art_quick_set16_static;
  qpoints->pSet32Instance = art_quick_set32_instance;
  qpoints->pSet32Static = art_quick_set32_static;
  qpoints->pSet64Instance = art_quick_set64_instance;
  qpoints->pSet64Static = art_quick_set64_static;
  qpoints->pSetObjInstance = art_quick_set_obj_instance;
  qpoints->pSetObjStatic = art_quick_set_obj_static;
  qpoints->pGetBooleanInstance = art_quick_get_boolean_instance;
  qpoints->pGetByteInstance = art_quick_get_byte_instance;
  qpoints->pGetCharInstance = art_quick_get_char_instance;
  qpoints->pGetShortInstance = art_quick_get_short_instance;
  qpoints->pGet32Instance = art_quick_get32_instance;
  qpoints->pGet64Instance = art_quick_get64_instance;
  qpoints->pGetObjInstance = art_quick_get_obj_instance;
  qpoints->pGetBooleanStatic = art_quick_get_boolean_static;
  qpoints->pGetByteStatic = art_quick_get_byte_static;
  qpoints->pGetCharStatic = art_quick_get_char_static;
  qpoints->pGetShortStatic = art_quick_get_short_static;
  qpoints->pGet32Static = art_quick_get32_static;
  qpoints->pGet64Static = art_quick_get64_static;
  qpoints->pGetObjStatic = art_quick_get_obj_static;

  // Array
  qpoints->pAputObjectWithNullAndBoundCheck = art_quick_aput_obj_with_null_and_bound_check;
  qpoints->pAputObjectWithBoundCheck = art_quick_aput_obj_with_bound_check;
  qpoints->pAputObject = art_quick_aput_obj;
  qpoints->pHandleFillArrayData = art_quick_handle_fill_data;

  // JNI
  qpoints->pJniMethodStart = JniMethodStart;
  qpoints->pJniMethodStartSynchronized = JniMethodStartSynchronized;
  qpoints->pJniMethodEnd = JniMethodEnd;
  qpoints->pJniMethodEndSynchronized = JniMethodEndSynchronized;
  qpoints->pJniMethodEndWithReference = JniMethodEndWithReference;
  qpoints->pJniMethodEndWithReferenceSynchronized = JniMethodEndWithReferenceSynchronized;
  qpoints->pQuickGenericJniTrampoline = art_quick_generic_jni_trampoline;

  // Locks
  qpoints->pLockObject = art_quick_lock_object;
  qpoints->pUnlockObject = art_quick_unlock_object;

  // Math
  qpoints->pCmpgDouble = CmpgDouble;
  qpoints->pCmpgFloat = CmpgFloat;
  qpoints->pCmplDouble = CmplDouble;
  qpoints->pCmplFloat = CmplFloat;
  qpoints->pFmod = fmod;
  qpoints->pL2d = art_l2d;
  qpoints->pFmodf = fmodf;
  qpoints->pL2f = art_l2f;
  qpoints->pD2iz = art_d2i;
  qpoints->pF2iz = art_f2i;
  qpoints->pIdivmod = NULL;
  qpoints->pD2l = art_d2l;
  qpoints->pF2l = art_f2l;
  qpoints->pLdiv = artLdiv;
  qpoints->pLmod = artLmod;
  qpoints->pLmul = artLmul;
  qpoints->pShlLong = art_quick_shl_long;
  qpoints->pShrLong = art_quick_shr_long;
  qpoints->pUshrLong = art_quick_ushr_long;

  // Intrinsics
  qpoints->pIndexOf = art_quick_indexof;
  qpoints->pStringCompareTo = art_quick_string_compareto;
  qpoints->pMemcpy = memcpy;

  // Invocation
  qpoints->pQuickImtConflictTrampoline = art_quick_imt_conflict_trampoline;
  qpoints->pQuickResolutionTrampoline = art_quick_resolution_trampoline;
  qpoints->pQuickToInterpreterBridge = art_quick_to_interpreter_bridge;
  qpoints->pInvokeDirectTrampolineWithAccessCheck = art_quick_invoke_direct_trampoline_with_access_check;
  qpoints->pInvokeInterfaceTrampolineWithAccessCheck = art_quick_invoke_interface_trampoline_with_access_check;
  qpoints->pInvokeStaticTrampolineWithAccessCheck = art_quick_invoke_static_trampoline_with_access_check;
  qpoints->pInvokeSuperTrampolineWithAccessCheck = art_quick_invoke_super_trampoline_with_access_check;
  qpoints->pInvokeVirtualTrampolineWithAccessCheck = art_quick_invoke_virtual_trampoline_with_access_check;

  // Thread
  qpoints->pTestSuspend = art_quick_test_suspend;

  // Throws
  qpoints->pDeliverException = art_quick_deliver_exception;
  qpoints->pThrowArrayBounds = art_quick_throw_array_bounds;
  qpoints->pThrowDivZero = art_quick_throw_div_zero;
  qpoints->pThrowNoSuchMethod = art_quick_throw_no_such_method;
  qpoints->pThrowNullPointer = art_quick_throw_null_pointer_exception;
  qpoints->pThrowStackOverflow = art_quick_throw_stack_overflow;

  // Atomic 64-bit load/store
  qpoints->pA64Load = QuasiAtomic::Read64;
  qpoints->pA64Store = QuasiAtomic::Write64;
};

}  // namespace art
