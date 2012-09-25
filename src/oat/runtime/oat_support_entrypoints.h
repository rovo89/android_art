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

#ifndef ART_SRC_OAT_RUNTIME_OAT_SUPPORT_ENTRYPOINTS_H_
#define ART_SRC_OAT_RUNTIME_OAT_SUPPORT_ENTRYPOINTS_H_

#include "runtime.h"

#define ENTRYPOINT_OFFSET(x) \
    (static_cast<uintptr_t>(OFFSETOF_MEMBER(Thread, entrypoints_)) + \
        static_cast<uintptr_t>(OFFSETOF_MEMBER(EntryPoints, x)))

namespace art {

class Class;
class DvmDex;
class AbstractMethod;
class Thread;

struct PACKED EntryPoints {
  // Alloc
  void* (*pAllocArrayFromCode)(uint32_t, void*, int32_t);
  void* (*pAllocArrayFromCodeWithAccessCheck)(uint32_t, void*, int32_t);
  void* (*pAllocObjectFromCode)(uint32_t, void*);
  void* (*pAllocObjectFromCodeWithAccessCheck)(uint32_t, void*);
  void* (*pCheckAndAllocArrayFromCode)(uint32_t, void*, int32_t);
  void* (*pCheckAndAllocArrayFromCodeWithAccessCheck)(uint32_t, void*, int32_t);

  // Cast
  uint32_t (*pInstanceofNonTrivialFromCode)(const Class*, const Class*);
  void (*pCanPutArrayElementFromCode)(void*, void*);
  void (*pCheckCastFromCode)(void*, void*);

  // Debug
  void (*pDebugMe)(AbstractMethod*, uint32_t);
  void (*pUpdateDebuggerFromCode)(void*, void*, int32_t, void*);

  // DexCache
  void* (*pInitializeStaticStorage)(uint32_t, void*);
  void* (*pInitializeTypeAndVerifyAccessFromCode)(uint32_t, void*);
  void* (*pInitializeTypeFromCode)(uint32_t, void*);
  void* (*pResolveStringFromCode)(void*, uint32_t);

  // Exceptions
  void* (*pGetAndClearException)(Thread*);

  // Field
  int (*pSet32Instance)(uint32_t, void*, int32_t);  // field_idx, obj, src
  int (*pSet32Static)(uint32_t, int32_t);
  int (*pSet64Instance)(uint32_t, void*, int64_t);
  int (*pSet64Static)(uint32_t, int64_t);
  int (*pSetObjInstance)(uint32_t, void*, void*);
  int (*pSetObjStatic)(uint32_t, void*);
  int32_t (*pGet32Instance)(uint32_t, void*);
  int32_t (*pGet32Static)(uint32_t);
  int64_t (*pGet64Instance)(uint32_t, void*);
  int64_t (*pGet64Static)(uint32_t);
  void* (*pGetObjInstance)(uint32_t, void*);
  void* (*pGetObjStatic)(uint32_t);

  // FillArray
  void (*pHandleFillArrayDataFromCode)(void*, void*);

  // JNI
  void* (*pFindNativeMethod)(Thread* thread);
  uint32_t (*pJniMethodStart)(Thread*);
  uint32_t (*pJniMethodStartSynchronized)(jobject to_lock, Thread* self);
  void (*pJniMethodEnd)(uint32_t cookie, Thread* self);
  void (*pJniMethodEndSynchronized)(uint32_t cookie, jobject locked, Thread* self);
  Object* (*pJniMethodEndWithReference)(jobject result, uint32_t cookie, Thread* self);
  Object* (*pJniMethodEndWithReferenceSynchronized)(jobject result, uint32_t cookie,
                                                    jobject locked, Thread* self);

  // Locks
  void (*pLockObjectFromCode)(void*);
  void (*pUnlockObjectFromCode)(void*);

  // Math
  int32_t (*pCmpgDouble)(double, double);
  int32_t (*pCmpgFloat)(float, float);
  int32_t (*pCmplDouble)(double, double);
  int32_t (*pCmplFloat)(float, float);
  double (*pDadd)(double, double);
  double (*pDdiv)(double, double);
  double (*pDmul)(double, double);
  double (*pDsub)(double, double);
  double (*pF2d)(float);
  double (*pFmod)(double, double);
  double (*pSqrt)(double);
  double (*pI2d)(int);
  double (*pL2d)(int64_t);
  float (*pD2f)(double);
  float (*pFadd)(float, float);
  float (*pFdiv)(float, float);
  float (*pFmodf)(float, float);
  float (*pFmul)(float, float);
  float (*pFsub)(float, float);
  float (*pI2f)(int32_t);
  float (*pL2f)(int64_t);
  int32_t (*pD2iz)(double);
  int32_t (*pF2iz)(float);
  int32_t (*pIdivmod)(int32_t, int32_t);
  int64_t (*pD2l)(double);
  int64_t (*pF2l)(float);
  int64_t (*pLdiv)(int64_t, int64_t);
  int64_t (*pLdivmod)(int64_t, int64_t);
  int64_t (*pLmul)(int64_t, int64_t);
  uint64_t (*pShlLong)(uint64_t, uint32_t);
  uint64_t (*pShrLong)(uint64_t, uint32_t);
  uint64_t (*pUshrLong)(uint64_t, uint32_t);

  // Intrinsics
  int32_t (*pIndexOf)(void*, uint32_t, uint32_t, uint32_t);
  int32_t (*pMemcmp16)(void*, void*, int32_t);
  int32_t (*pStringCompareTo)(void*, void*);
  void* (*pMemcpy)(void*, const void*, size_t);

  // Invocation
  const void* (*pUnresolvedDirectMethodTrampolineFromCode)(AbstractMethod*, AbstractMethod**, Thread*,
                                                           Runtime::TrampolineType);
  void (*pInvokeDirectTrampolineWithAccessCheck)(uint32_t, void*);
  void (*pInvokeInterfaceTrampoline)(uint32_t, void*);
  void (*pInvokeInterfaceTrampolineWithAccessCheck)(uint32_t, void*);
  void (*pInvokeStaticTrampolineWithAccessCheck)(uint32_t, void*);
  void (*pInvokeSuperTrampolineWithAccessCheck)(uint32_t, void*);
  void (*pInvokeVirtualTrampolineWithAccessCheck)(uint32_t, void*);

  // Thread
  void (*pCheckSuspendFromCode)(Thread*);  // Stub that is called when the suspend count is non-zero
  void (*pTestSuspendFromCode)();  // Stub that is periodically called to test the suspend count

  // Throws
  void (*pDeliverException)(void*);
  void (*pThrowAbstractMethodErrorFromCode)(AbstractMethod* m, Thread* thread, AbstractMethod** sp);
  void (*pThrowArrayBoundsFromCode)(int32_t, int32_t);
  void (*pThrowDivZeroFromCode)();
  void (*pThrowNoSuchMethodFromCode)(int32_t);
  void (*pThrowNullPointerFromCode)();
  void (*pThrowStackOverflowFromCode)(void*);
};

// Initialize an entry point data structure.
void InitEntryPoints(EntryPoints* points);

// Change the debugger entry point in the data structure.
void ChangeDebuggerEntryPoint(EntryPoints* points, bool enabled);

// Is the given return_pc the trace exit return pc?
bool IsTraceExitPc(uintptr_t pc);

// Return address of stub that logs method entries.
void* GetLogTraceEntryPoint();

}  // namespace art

#endif  // ART_SRC_OAT_RUNTIME_OAT_SUPPORT_ENTRYPOINTS_H_
