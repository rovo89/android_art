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
#include "object.h"
#include "object_utils.h"
#include "runtime_support.h"
#include "thread.h"

namespace art {

// Deliver an exception that's pending on thread helping set up a callee save frame on the way.
extern "C" void artDeliverPendingExceptionFromCode(Thread* thread, Method** sp) {
  FinishCalleeSaveFrameSetup(thread, sp, Runtime::kSaveAll);
  thread->DeliverException();
}

// Called by generated call to throw an exception.
extern "C" void artDeliverExceptionFromCode(Throwable* exception, Thread* thread, Method** sp) {
  /*
   * exception may be NULL, in which case this routine should
   * throw NPE.  NOTE: this is a convenience for generated code,
   * which previously did the null check inline and constructed
   * and threw a NPE if NULL.  This routine responsible for setting
   * exception_ in thread and delivering the exception.
   */
  FinishCalleeSaveFrameSetup(thread, sp, Runtime::kSaveAll);
  if (exception == NULL) {
    thread->ThrowNewException("Ljava/lang/NullPointerException;", "throw with null exception");
  } else {
    thread->SetException(exception);
  }
  thread->DeliverException();
}

// Called by generated call to throw a NPE exception.
extern "C" void artThrowNullPointerExceptionFromCode(Thread* self, Method** sp) {
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kSaveAll);
  Frame frame = self->GetTopOfStack();
  uintptr_t throw_native_pc = frame.GetReturnPC();
  frame.Next();
  Method* throw_method = frame.GetMethod();
  uint32_t dex_pc = throw_method->ToDexPC(throw_native_pc - 2);
  ThrowNullPointerExceptionFromDexPC(self, throw_method, dex_pc);
  self->DeliverException();
}

// Called by generated call to throw an arithmetic divide by zero exception.
extern "C" void artThrowDivZeroFromCode(Thread* thread, Method** sp) {
  FinishCalleeSaveFrameSetup(thread, sp, Runtime::kSaveAll);
  thread->ThrowNewException("Ljava/lang/ArithmeticException;", "divide by zero");
  thread->DeliverException();
}

// Called by generated call to throw an array index out of bounds exception.
extern "C" void artThrowArrayBoundsFromCode(int index, int limit, Thread* thread, Method** sp) {
  FinishCalleeSaveFrameSetup(thread, sp, Runtime::kSaveAll);
  thread->ThrowNewExceptionF("Ljava/lang/ArrayIndexOutOfBoundsException;",
                             "length=%d; index=%d", limit, index);
  thread->DeliverException();
}

extern "C" void artThrowStackOverflowFromCode(Thread* thread, Method** sp) {
  FinishCalleeSaveFrameSetup(thread, sp, Runtime::kSaveAll);
  // Remove extra entry pushed onto second stack during method tracing.
  if (Runtime::Current()->IsMethodTracingActive()) {
    TraceMethodUnwindFromCode(thread);
  }
  thread->SetStackEndForStackOverflow();  // Allow space on the stack for constructor to execute.
  thread->ThrowNewExceptionF("Ljava/lang/StackOverflowError;", "stack size %s",
                             PrettySize(thread->GetStackSize()).c_str());
  thread->ResetDefaultStackEnd();  // Return to default stack size.
  thread->DeliverException();
}

extern "C" void artThrowNoSuchMethodFromCode(int32_t method_idx, Thread* self, Method** sp) {
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kSaveAll);
  Frame frame = self->GetTopOfStack();  // We need the calling method as context for the method_idx
  frame.Next();
  Method* method = frame.GetMethod();
  self->ThrowNewException("Ljava/lang/NoSuchMethodError;",
      MethodNameFromIndex(method, method_idx, verifier::VERIFY_ERROR_REF_METHOD, false).c_str());
  self->DeliverException();
}

extern "C" void artThrowVerificationErrorFromCode(int32_t kind, int32_t ref, Thread* self, Method** sp) {
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kSaveAll);
  Frame frame = self->GetTopOfStack();  // We need the calling method as context to interpret 'ref'
  frame.Next();
  Method* method = frame.GetMethod();
  ThrowVerificationError(self, method, kind, ref);
  self->DeliverException();
}

}  // namespace art
