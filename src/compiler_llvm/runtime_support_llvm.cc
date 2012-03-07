// Copyright 2012 Google Inc. All Rights Reserved.

#include "class_linker.h"
#include "dex_verifier.h"
#include "object.h"
#include "object_utils.h"
#include "runtime_support_llvm.h"
#include "thread.h"
#include "thread_list.h"

#include <stdint.h>

namespace art {

//----------------------------------------------------------------------------
// Thread
//----------------------------------------------------------------------------

Thread* art_get_current_thread_from_code() {
  return Thread::Current();
}

void art_set_current_thread_from_code(void* thread_object_addr) {
  // TODO: LLVM IR generating something like "r9 = thread_object_addr"
  UNIMPLEMENTED(WARNING);
}

void art_lock_object_from_code(Object* obj) {
  Thread* thread = Thread::Current();
  DCHECK(obj != NULL);        // Assumed to have been checked before entry
  obj->MonitorEnter(thread);  // May block
  DCHECK(thread->HoldsLock(obj));
  // Only possible exception is NPE and is handled before entry
  DCHECK(!thread->IsExceptionPending());
}

void art_unlock_object_from_code(Object* obj) {
  Thread* thread = Thread::Current();
  DCHECK(obj != NULL);  // Assumed to have been checked before entry
  // MonitorExit may throw exception
  obj->MonitorExit(thread);
}

void art_test_suspend_from_code() {
  Thread* thread = Thread::Current();
  Runtime::Current()->GetThreadList()->FullSuspendCheck(thread);
}

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


//----------------------------------------------------------------------------
// Object Space
//----------------------------------------------------------------------------

Object* art_alloc_object_from_code(uint32_t type_idx, Object* referrer) {
  UNIMPLEMENTED(WARNING);
  return NULL;
}

Object* art_alloc_object_from_code_with_access_check(uint32_t type_idx,
                                                     Object* referrer) {
  UNIMPLEMENTED(WARNING);
  return NULL;
}

Object* art_alloc_array_from_code(uint32_t type_idx,
                                  Object* referrer,
                                  uint32_t length) {
  UNIMPLEMENTED(WARNING);
  return NULL;
}

Object* art_alloc_array_from_code_with_access_check(uint32_t type_idx,
                                                    Object* referrer,
                                                    uint32_t length) {
  UNIMPLEMENTED(WARNING);
  return NULL;
}

Object* art_check_and_alloc_array_from_code(uint32_t type_idx,
                                            Object* referrer,
                                            uint32_t length) {
  UNIMPLEMENTED(WARNING);
  return NULL;
}

Object* art_check_and_alloc_array_from_code_with_access_check(uint32_t type_idx,
                                                              Object* referrer,
                                                              uint32_t length) {
  UNIMPLEMENTED(WARNING);
  return NULL;
}

void art_find_instance_field_from_code(uint32_t field_idx, Object* referrer) {
  UNIMPLEMENTED(WARNING);
}

void art_find_static_field_from_code(uint32_t field_idx, Object* referrer) {
  UNIMPLEMENTED(WARNING);
}

Object* art_find_interface_method_from_code(uint32_t method_idx,
                                            Object* referrer) {
  UNIMPLEMENTED(WARNING);
  return NULL;
}

Object* art_initialize_static_storage_from_code(uint32_t type_idx,
                                                Object* referrer) {
  UNIMPLEMENTED(WARNING);
  return NULL;
}

Object* art_initialize_type_from_code(uint32_t type_idx, Object* referrer) {
  UNIMPLEMENTED(WARNING);
  return NULL;
}

Object* art_initialize_type_and_verify_access_from_code(uint32_t type_idx,
                                                        Object* referrer) {
  UNIMPLEMENTED(WARNING);
  return NULL;
}

Object* art_resolve_string_from_code(Object* referrer, uint32_t string_idx) {
  UNIMPLEMENTED(WARNING);
  return NULL;
}

int32_t art_set32_static_from_code(uint32_t field_idx,
                                   Object* referrer,
                                   int32_t new_value) {
  UNIMPLEMENTED(WARNING);
  return -1;
}

int32_t art_set64_static_from_code(uint32_t field_idx,
                                   Object* referrer,
                                   int64_t new_value) {
  UNIMPLEMENTED(WARNING);
  return -1;
}

int32_t art_set_obj_static_from_code(uint32_t field_idx,
                                     Object* referrer,
                                     Object* new_value) {
  UNIMPLEMENTED(WARNING);
  return -1;
}

int32_t art_get32_static_from_code(uint32_t field_idx, Object* referrer) {
  UNIMPLEMENTED(WARNING);
  return 0;
}

int64_t art_get64_static_from_code(uint32_t field_idx, Object* referrer) {
  UNIMPLEMENTED(WARNING);
  return 0;
}

Object* art_get_obj_static_from_code(uint32_t field_idx, Object* referrer) {
  UNIMPLEMENTED(WARNING);
  return NULL;
}

int32_t art_set32_instance_from_code(uint32_t field_idx,
                                     Object* referrer,
                                     Object* object,
                                     uint32_t new_value) {
  UNIMPLEMENTED(WARNING);
  return -1;
}

int32_t art_set64_instance_from_code(uint32_t field_idx,
                                     Object* referrer,
                                     Object* object,
                                     int64_t new_value) {
  UNIMPLEMENTED(WARNING);
  return -1;
}

int32_t art_set_obj_instance_from_code(uint32_t field_idx,
                                       Object* referrer,
                                       Object* object,
                                       Object* new_value) {
  UNIMPLEMENTED(WARNING);
  return -1;
}

int32_t art_get32_instance_from_code(uint32_t field_idx,
                                     Object* referrer,
                                     Object* object) {
  UNIMPLEMENTED(WARNING);
  return 0;
}

int64_t art_get64_instance_from_code(uint32_t field_idx,
                                     Object* referrer,
                                     Object* object) {
  UNIMPLEMENTED(WARNING);
  return 0;
}

Object* art_get_obj_instance_from_code(uint32_t field_idx,
                                       Object* referrer,
                                       Object* object) {
  UNIMPLEMENTED(WARNING);
  return NULL;
}


//----------------------------------------------------------------------------
// RTTI
//----------------------------------------------------------------------------

int32_t art_is_assignable_from_code(Object* dest_type, Object* src_type) {
  return 0;
}

void art_check_cast_from_code(Object* dest_type, Object* src_type) {
}


//----------------------------------------------------------------------------
// Runtime Support Function Lookup Callback
//----------------------------------------------------------------------------

void* art_find_runtime_support_func(void* context, char const* name) {
  struct func_entry_t {
    char const* name;
    size_t name_len;
    void* addr;
  };

  static struct func_entry_t const tab[] = {
#define DEFINE_ENTRY(ID, NAME) \
    { #NAME, sizeof(#NAME) - 1, reinterpret_cast<void*>(NAME) },

#include "runtime_support_func_list.h"
    RUNTIME_SUPPORT_FUNC_LIST(DEFINE_ENTRY)
#undef RUNTIME_SUPPORT_FUNC_LIST
#undef DEFINE_ENTRY
  };

  static size_t const tab_size = sizeof(tab) / sizeof(struct func_entry_t);

  // Note: Since our table is small, we are using trivial O(n) searching
  // function.  For bigger table, it will be better to use binary
  // search or hash function.
  size_t i;
  size_t name_len = strlen(name);
  for (i = 0; i < tab_size; ++i) {
    if (name_len == tab[i].name_len && strcmp(name, tab[i].name) == 0) {
      return tab[i].addr;
    }
  }

  LOG(FATAL) << "Error: Can't find symbol " << name;
  return 0;
}

}  // namespace art
