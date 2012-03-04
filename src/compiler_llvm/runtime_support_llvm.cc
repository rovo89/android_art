// Copyright 2012 Google Inc. All Rights Reserved.

#include "class_linker.h"
#include "dex_verifier.h"
#include "object.h"
#include "object_utils.h"
#include "runtime_support_llvm.h"
#include "thread.h"

#include <stdint.h>

namespace art {

//----------------------------------------------------------------------------
// Thread
//----------------------------------------------------------------------------

void art_push_shadow_frame_from_code(void* new_shadow_frame) {
  Thread* thread = Thread::Current();
  thread->PushSirt(
      static_cast<StackIndirectReferenceTable*>(new_shadow_frame)
                   );
}

void art_pop_shadow_frame_from_code() {
  Thread* thread = Thread::Current();
  thread->PopSirt();
}


//----------------------------------------------------------------------------
// Exception
//----------------------------------------------------------------------------

static std::string MethodNameFromIndex(const Method* method,
                                       uint32_t ref,
                                       verifier::VerifyErrorRefType ref_type,
                                       bool access) {
  CHECK_EQ(static_cast<int>(ref_type),
           static_cast<int>(verifier::VERIFY_ERROR_REF_METHOD));

  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  const DexFile& dex_file =
      class_linker->FindDexFile(method->GetDeclaringClass()->GetDexCache());

  const DexFile::MethodId& id = dex_file.GetMethodId(ref);
  std::string class_name(
      PrettyDescriptor(dex_file.GetMethodDeclaringClassDescriptor(id))
                         );
  const char* method_name = dex_file.StringDataByIdx(id.name_idx_);
  if (!access) {
    return class_name + "." + method_name;
  }

  std::string result;
  result += "tried to access method ";
  result += class_name + "." + method_name + ":" +
      dex_file.CreateMethodSignature(id.proto_idx_, NULL);
  result += " from class ";
  result += PrettyDescriptor(method->GetDeclaringClass());
  return result;
}

bool art_is_exception_pending_from_code() {
  return Thread::Current()->IsExceptionPending();
}

void art_throw_div_zero_from_code() {
  Thread* thread = Thread::Current();
  thread->ThrowNewException("Ljava/lang/ArithmeticException;",
                            "divide by zero");
}

void art_throw_array_bounds_from_code(int32_t length, int32_t index) {
  Thread* thread = Thread::Current();
  thread->ThrowNewExceptionF("Ljava/lang/ArrayIndexOutOfBoundsException;",
                             "length=%d; index=%d", length, index);
}

void art_throw_no_such_method_from_code(int32_t method_idx) {
  Thread* thread = Thread::Current();
  // We need the calling method as context for the method_idx
  Frame frame = thread->GetTopOfStack();
  frame.Next();
  Method* method = frame.GetMethod();
  thread->ThrowNewException("Ljava/lang/NoSuchMethodError;",
                            MethodNameFromIndex(method,
                                                method_idx,
                                                verifier::VERIFY_ERROR_REF_METHOD,
                                                false).c_str());
}

void art_throw_null_pointer_exception_from_code() {
  Thread* thread = Thread::Current();
  thread->ThrowNewException("Ljava/lang/NullPointerException;", NULL);
}

void art_throw_stack_overflow_from_code(void*) {
  Thread* thread = Thread::Current();
  thread->ThrowNewExceptionF("Ljava/lang/StackOverflowError;",
      "stack size %zdkb; default stack size: %zdkb",
      thread->GetStackSize() / KB,
      Runtime::Current()->GetDefaultStackSize() / KB);
}

void art_throw_exception_from_code(Object* exception) {
  Thread* thread = Thread::Current();
  thread->SetException(static_cast<Throwable*>(exception));
}

int32_t art_find_catch_block_from_code(Object* exception, int32_t dex_pc) {
  Class* exception_type = exception->GetClass();
  Method* current_method = Thread::Current()->GetCurrentMethod();
  MethodHelper mh(current_method);
  const DexFile::CodeItem* code_item = mh.GetCodeItem();
  int iter_index = 0;
  // Iterate over the catch handlers associated with dex_pc
  for (CatchHandlerIterator it(*code_item, dex_pc); it.HasNext(); it.Next()) {
    uint16_t iter_type_idx = it.GetHandlerTypeIndex();
    // Catch all case
    if (iter_type_idx == DexFile::kDexNoIndex16) {
      return iter_index;
    }
    // Does this catch exception type apply?
    Class* iter_exception_type = mh.GetDexCacheResolvedType(iter_type_idx);
    if (iter_exception_type == NULL) {
      // The verifier should take care of resolving all exception classes early
      LOG(WARNING) << "Unresolved exception class when finding catch block: "
          << mh.GetTypeDescriptorFromTypeIdx(iter_type_idx);
    } else if (iter_exception_type->IsAssignableFrom(exception_type)) {
      return iter_index;
    }
    ++iter_index;
  }
  // Handler not found
  return -1;
}

void art_test_suspend_from_code() {
}

void art_set_current_thread_from_code(void* thread_object_addr) {
}

}  // namespace art
