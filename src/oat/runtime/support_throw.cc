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
#include "dex_verifier.h"
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
  // Remove extra entry pushed onto second stack during method tracing
  if (Runtime::Current()->IsMethodTracingActive()) {
    TraceMethodUnwindFromCode(thread);
  }
  thread->SetStackEndForStackOverflow();  // Allow space on the stack for constructor to execute
  thread->ThrowNewExceptionF("Ljava/lang/StackOverflowError;",
      "stack size %zdkb; default stack size: %zdkb",
      thread->GetStackSize() / KB, Runtime::Current()->GetDefaultStackSize() / KB);
  thread->ResetDefaultStackEnd();  // Return to default stack size
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

static std::string ClassNameFromIndex(Method* method, uint32_t ref,
                                      verifier::VerifyErrorRefType ref_type, bool access) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  const DexFile& dex_file = class_linker->FindDexFile(method->GetDeclaringClass()->GetDexCache());

  uint16_t type_idx = 0;
  if (ref_type == verifier::VERIFY_ERROR_REF_FIELD) {
    const DexFile::FieldId& id = dex_file.GetFieldId(ref);
    type_idx = id.class_idx_;
  } else if (ref_type == verifier::VERIFY_ERROR_REF_METHOD) {
    const DexFile::MethodId& id = dex_file.GetMethodId(ref);
    type_idx = id.class_idx_;
  } else if (ref_type == verifier::VERIFY_ERROR_REF_CLASS) {
    type_idx = ref;
  } else {
    CHECK(false) << static_cast<int>(ref_type);
  }

  std::string class_name(PrettyDescriptor(dex_file.StringByTypeIdx(type_idx)));
  if (!access) {
    return class_name;
  }

  std::string result;
  result += "tried to access class ";
  result += class_name;
  result += " from class ";
  result += PrettyDescriptor(method->GetDeclaringClass());
  return result;
}

extern "C" void artThrowVerificationErrorFromCode(int32_t kind, int32_t ref, Thread* self, Method** sp) {
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kSaveAll);
  Frame frame = self->GetTopOfStack();  // We need the calling method as context to interpret 'ref'
  frame.Next();
  Method* method = frame.GetMethod();

  verifier::VerifyErrorRefType ref_type =
      static_cast<verifier::VerifyErrorRefType>(kind >> verifier::kVerifyErrorRefTypeShift);

  const char* exception_class = "Ljava/lang/VerifyError;";
  std::string msg;

  switch (static_cast<verifier::VerifyError>(kind & ~(0xff << verifier::kVerifyErrorRefTypeShift))) {
  case verifier::VERIFY_ERROR_NO_CLASS:
    exception_class = "Ljava/lang/NoClassDefFoundError;";
    msg = ClassNameFromIndex(method, ref, ref_type, false);
    break;
  case verifier::VERIFY_ERROR_NO_FIELD:
    exception_class = "Ljava/lang/NoSuchFieldError;";
    msg = FieldNameFromIndex(method, ref, ref_type, false);
    break;
  case verifier::VERIFY_ERROR_NO_METHOD:
    exception_class = "Ljava/lang/NoSuchMethodError;";
    msg = MethodNameFromIndex(method, ref, ref_type, false);
    break;
  case verifier::VERIFY_ERROR_ACCESS_CLASS:
    exception_class = "Ljava/lang/IllegalAccessError;";
    msg = ClassNameFromIndex(method, ref, ref_type, true);
    break;
  case verifier::VERIFY_ERROR_ACCESS_FIELD:
    exception_class = "Ljava/lang/IllegalAccessError;";
    msg = FieldNameFromIndex(method, ref, ref_type, true);
    break;
  case verifier::VERIFY_ERROR_ACCESS_METHOD:
    exception_class = "Ljava/lang/IllegalAccessError;";
    msg = MethodNameFromIndex(method, ref, ref_type, true);
    break;
  case verifier::VERIFY_ERROR_CLASS_CHANGE:
    exception_class = "Ljava/lang/IncompatibleClassChangeError;";
    msg = ClassNameFromIndex(method, ref, ref_type, false);
    break;
  case verifier::VERIFY_ERROR_INSTANTIATION:
    exception_class = "Ljava/lang/InstantiationError;";
    msg = ClassNameFromIndex(method, ref, ref_type, false);
    break;
  case verifier::VERIFY_ERROR_BAD_CLASS_SOFT:
  case verifier::VERIFY_ERROR_BAD_CLASS_HARD:
    // Generic VerifyError; use default exception, no message.
    break;
  case verifier::VERIFY_ERROR_NONE:
    CHECK(false);
    break;
  }
  self->ThrowNewException(exception_class, msg.c_str());
  self->DeliverException();
}

}  // namespace art
