/*
 * Copyright 2011 Google Inc. All Rights Reserved.
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

#include "runtime_support.h"

namespace art {

// Temporary debugging hook for compiler.
extern void DebugMe(Method* method, uint32_t info) {
  LOG(INFO) << "DebugMe";
  if (method != NULL) {
    LOG(INFO) << PrettyMethod(method);
  }
  LOG(INFO) << "Info: " << info;
}

// Return value helper for jobject return types
extern Object* DecodeJObjectInThread(Thread* thread, jobject obj) {
  return thread->DecodeJObject(obj);
}

extern void* FindNativeMethod(Thread* thread) {
  DCHECK(Thread::Current() == thread);

  Method* method = const_cast<Method*>(thread->GetCurrentMethod());
  DCHECK(method != NULL);

  // Lookup symbol address for method, on failure we'll return NULL with an
  // exception set, otherwise we return the address of the method we found.
  void* native_code = thread->GetJniEnv()->vm->FindCodeForNativeMethod(method);
  if (native_code == NULL) {
    DCHECK(thread->IsExceptionPending());
    return NULL;
  } else {
    // Register so that future calls don't come here
    method->RegisterNative(native_code);
    return native_code;
  }
}

// Called by generated call to throw an exception
extern "C" void artDeliverExceptionFromCode(Throwable* exception, Thread* thread, Method** sp) {
  /*
   * exception may be NULL, in which case this routine should
   * throw NPE.  NOTE: this is a convenience for generated code,
   * which previously did the null check inline and constructed
   * and threw a NPE if NULL.  This routine responsible for setting
   * exception_ in thread and delivering the exception.
   */
  // Place a special frame at the TOS that will save all callee saves
  *sp = Runtime::Current()->GetCalleeSaveMethod();
  thread->SetTopOfStack(sp, 0);
  if (exception == NULL) {
    thread->ThrowNewException("Ljava/lang/NullPointerException;", "throw with null exception");
  } else {
    thread->SetException(exception);
  }
  thread->DeliverException();
}

// Deliver an exception that's pending on thread helping set up a callee save frame on the way
extern "C" void artDeliverPendingExceptionFromCode(Thread* thread, Method** sp) {
  *sp = Runtime::Current()->GetCalleeSaveMethod();
  thread->SetTopOfStack(sp, 0);
  thread->DeliverException();
}

// Called by generated call to throw a NPE exception
extern "C" void artThrowNullPointerExceptionFromCode(Thread* thread, Method** sp) {
  // Place a special frame at the TOS that will save all callee saves
  *sp = Runtime::Current()->GetCalleeSaveMethod();
  thread->SetTopOfStack(sp, 0);
  thread->ThrowNewException("Ljava/lang/NullPointerException;", NULL);
  thread->DeliverException();
}

// Called by generated call to throw an arithmetic divide by zero exception
extern "C" void artThrowDivZeroFromCode(Thread* thread, Method** sp) {
  // Place a special frame at the TOS that will save all callee saves
  *sp = Runtime::Current()->GetCalleeSaveMethod();
  thread->SetTopOfStack(sp, 0);
  thread->ThrowNewException("Ljava/lang/ArithmeticException;", "divide by zero");
  thread->DeliverException();
}

// Called by generated call to throw an arithmetic divide by zero exception
extern "C" void artThrowArrayBoundsFromCode(int index, int limit, Thread* thread, Method** sp) {
  // Place a special frame at the TOS that will save all callee saves
  *sp = Runtime::Current()->GetCalleeSaveMethod();
  thread->SetTopOfStack(sp, 0);
  thread->ThrowNewExceptionF("Ljava/lang/ArrayIndexOutOfBoundsException;", "length=%d; index=%d",
      limit, index);
  thread->DeliverException();
}

// Called by the AbstractMethodError stub (not runtime support)
extern void ThrowAbstractMethodErrorFromCode(Method* method, Thread* thread, Method** sp) {
  *sp = Runtime::Current()->GetCalleeSaveMethod();
  thread->SetTopOfStack(sp, 0);
  thread->ThrowNewExceptionF("Ljava/lang/AbstractMethodError;", "abstract method \"%s\"",
      PrettyMethod(method).c_str());
  thread->DeliverException();
}

extern "C" void artThrowStackOverflowFromCode(Method* method, Thread* thread, Method** sp) {
  // Place a special frame at the TOS that will save all callee saves
  Runtime* runtime = Runtime::Current();
  *sp = runtime->GetCalleeSaveMethod();
  thread->SetTopOfStack(sp, 0);
  thread->SetStackEndForStackOverflow();  // Allow space on the stack for constructor to execute
  thread->ThrowNewExceptionF("Ljava/lang/StackOverflowError;",
      "stack size %zdkb; default stack size: %zdkb",
      thread->GetStackSize() / KB, runtime->GetDefaultStackSize() / KB);
  thread->ResetDefaultStackEnd();  // Return to default stack size
  thread->DeliverException();
}

extern "C" void artThrowVerificationErrorFromCode(int32_t src1, int32_t ref, Thread* thread, Method** sp) {
  // Place a special frame at the TOS that will save all callee saves
  Runtime* runtime = Runtime::Current();
  *sp = runtime->GetCalleeSaveMethod();
  thread->SetTopOfStack(sp, 0);
  LOG(WARNING) << "TODO: verifcation error detail message. src1=" << src1 << " ref=" << ref;
  thread->ThrowNewExceptionF("Ljava/lang/VerifyError;",
      "TODO: verification error detail message. src1=%d; ref=%d", src1, ref);
  thread->DeliverException();
}

extern "C" void artThrowInternalErrorFromCode(int32_t errnum, Thread* thread, Method** sp) {
  // Place a special frame at the TOS that will save all callee saves
  Runtime* runtime = Runtime::Current();
  *sp = runtime->GetCalleeSaveMethod();
  thread->SetTopOfStack(sp, 0);
  LOG(WARNING) << "TODO: internal error detail message. errnum=" << errnum;
  thread->ThrowNewExceptionF("Ljava/lang/InternalError;", "errnum=%d", errnum);
  thread->DeliverException();
}

extern "C" void artThrowRuntimeExceptionFromCode(int32_t errnum, Thread* thread, Method** sp) {
  // Place a special frame at the TOS that will save all callee saves
  Runtime* runtime = Runtime::Current();
  *sp = runtime->GetCalleeSaveMethod();
  thread->SetTopOfStack(sp, 0);
  LOG(WARNING) << "TODO: runtime exception detail message. errnum=" << errnum;
  thread->ThrowNewExceptionF("Ljava/lang/RuntimeException;", "errnum=%d", errnum);
  thread->DeliverException();
}

extern "C" void artThrowNoSuchMethodFromCode(int32_t method_idx, Thread* thread, Method** sp) {
  // Place a special frame at the TOS that will save all callee saves
  Runtime* runtime = Runtime::Current();
  *sp = runtime->GetCalleeSaveMethod();
  thread->SetTopOfStack(sp, 0);
  LOG(WARNING) << "TODO: no such method exception detail message. method_idx=" << method_idx;
  thread->ThrowNewExceptionF("Ljava/lang/NoSuchMethodError;", "method_idx=%d", method_idx);
  thread->DeliverException();
}

extern "C" void artThrowNegArraySizeFromCode(int32_t size, Thread* thread, Method** sp) {
  LOG(WARNING) << "UNTESTED artThrowNegArraySizeFromCode";
  // Place a special frame at the TOS that will save all callee saves
  Runtime* runtime = Runtime::Current();
  *sp = runtime->GetCalleeSaveMethod();
  thread->SetTopOfStack(sp, 0);
  thread->ThrowNewExceptionF("Ljava/lang/NegativeArraySizeException;", "%d", size);
  thread->DeliverException();
}

// TODO: placeholder.  Helper function to type
Class* InitializeTypeFromCode(uint32_t type_idx, Method* method) {
  /*
   * Should initialize & fix up method->dex_cache_resolved_types_[].
   * Returns initialized type.  Does not return normally if an exception
   * is thrown, but instead initiates the catch.  Should be similar to
   * ClassLinker::InitializeStaticStorageFromCode.
   */
  UNIMPLEMENTED(FATAL);
  return NULL;
}

// TODO: placeholder.  Helper function to resolve virtual method
void ResolveMethodFromCode(Method* method, uint32_t method_idx) {
    /*
     * Slow-path handler on invoke virtual method path in which
     * base method is unresolved at compile-time.  Doesn't need to
     * return anything - just either ensure that
     * method->dex_cache_resolved_methods_(method_idx) != NULL or
     * throw and unwind.  The caller will restart call sequence
     * from the beginning.
     */
}

// Given the context of a calling Method, use its DexCache to resolve a type to a Class. If it
// cannot be resolved, throw an error. If it can, use it to create an instance.
extern "C" Object* artAllocObjectFromCode(uint32_t type_idx, Method* method) {
  Class* klass = method->GetDexCacheResolvedTypes()->Get(type_idx);
  if (klass == NULL) {
    klass = Runtime::Current()->GetClassLinker()->ResolveType(type_idx, method);
    if (klass == NULL) {
      DCHECK(Thread::Current()->IsExceptionPending());
      return NULL;  // Failure
    }
  }
  if (!Runtime::Current()->GetClassLinker()->EnsureInitialized(klass, true)) {
    DCHECK(Thread::Current()->IsExceptionPending());
    return NULL;  // Failure
  }
  return klass->AllocObject();
}

// Helper function to alloc array for OP_FILLED_NEW_ARRAY
extern "C" Array* artCheckAndArrayAllocFromCode(uint32_t type_idx, Method* method,
                                                int32_t component_count) {
  if (component_count < 0) {
    Thread::Current()->ThrowNewExceptionF("Ljava/lang/NegativeArraySizeException;", "%d",
                                         component_count);
    return NULL;  // Failure
  }
  Class* klass = method->GetDexCacheResolvedTypes()->Get(type_idx);
  if (klass == NULL) {  // Not in dex cache so try to resolve
    klass = Runtime::Current()->GetClassLinker()->ResolveType(type_idx, method);
    if (klass == NULL) {  // Error
      DCHECK(Thread::Current()->IsExceptionPending());
      return NULL;  // Failure
    }
  }
  if (klass->IsPrimitive() && !klass->IsPrimitiveInt()) {
    if (klass->IsPrimitiveLong() || klass->IsPrimitiveDouble()) {
      Thread::Current()->ThrowNewExceptionF("Ljava/lang/RuntimeException;",
          "Bad filled array request for type %s",
          PrettyDescriptor(klass->GetDescriptor()).c_str());
    } else {
      Thread::Current()->ThrowNewExceptionF("Ljava/lang/InternalError;",
          "Found type %s; filled-new-array not implemented for anything but \'int\'",
          PrettyDescriptor(klass->GetDescriptor()).c_str());
    }
    return NULL;  // Failure
  } else {
    CHECK(klass->IsArrayClass()) << PrettyClass(klass);
    return Array::Alloc(klass, component_count);
  }
}

// Given the context of a calling Method, use its DexCache to resolve a type to an array Class. If
// it cannot be resolved, throw an error. If it can, use it to create an array.
extern "C" Array* artArrayAllocFromCode(uint32_t type_idx, Method* method, int32_t component_count) {
  if (component_count < 0) {
    Thread::Current()->ThrowNewExceptionF("Ljava/lang/NegativeArraySizeException;", "%d",
                                         component_count);
    return NULL;  // Failure
  }
  Class* klass = method->GetDexCacheResolvedTypes()->Get(type_idx);
  if (klass == NULL) {  // Not in dex cache so try to resolve
    klass = Runtime::Current()->GetClassLinker()->ResolveType(type_idx, method);
    if (klass == NULL) {  // Error
      DCHECK(Thread::Current()->IsExceptionPending());
      return NULL;  // Failure
    }
    CHECK(klass->IsArrayClass()) << PrettyClass(klass);
  }
  return Array::Alloc(klass, component_count);
}

// Check whether it is safe to cast one class to the other, throw exception and return -1 on failure
extern "C" int artCheckCastFromCode(const Class* a, const Class* b) {
  DCHECK(a->IsClass()) << PrettyClass(a);
  DCHECK(b->IsClass()) << PrettyClass(b);
  if (b->IsAssignableFrom(a)) {
    return 0;  // Success
  } else {
    Thread::Current()->ThrowNewExceptionF("Ljava/lang/ClassCastException;",
        "%s cannot be cast to %s",
        PrettyDescriptor(a->GetDescriptor()).c_str(),
        PrettyDescriptor(b->GetDescriptor()).c_str());
    return -1;  // Failure
  }
}

// Tests whether 'element' can be assigned into an array of type 'array_class'.
// Returns 0 on success and -1 if an exception is pending.
extern "C" int artCanPutArrayElementFromCode(const Object* element, const Class* array_class) {
  DCHECK(array_class != NULL);
  // element can't be NULL as we catch this is screened in runtime_support
  Class* element_class = element->GetClass();
  Class* component_type = array_class->GetComponentType();
  if (component_type->IsAssignableFrom(element_class)) {
    return 0;  // Success
  } else {
    Thread::Current()->ThrowNewExceptionF("Ljava/lang/ArrayStoreException;",
        "Cannot store an object of type %s in to an array of type %s",
        PrettyDescriptor(element_class->GetDescriptor()).c_str(),
        PrettyDescriptor(array_class->GetDescriptor()).c_str());
    return -1;  // Failure
  }
}

extern "C" int artUnlockObjectFromCode(Thread* thread, Object* obj) {
  DCHECK(obj != NULL);  // Assumed to have been checked before entry
  return obj->MonitorExit(thread) ? 0 /* Success */ : -1 /* Failure */;
}

void LockObjectFromCode(Thread* thread, Object* obj) {
  DCHECK(obj != NULL);  // Assumed to have been checked before entry
  obj->MonitorEnter(thread);
  DCHECK(thread->HoldsLock(obj));
  // Only possible exception is NPE and is handled before entry
  DCHECK(!thread->IsExceptionPending());
}

extern "C" void artCheckSuspendFromCode(Thread* thread) {
  Runtime::Current()->GetThreadList()->FullSuspendCheck(thread);
}

/*
 * Fill the array with predefined constant values, throwing exceptions if the array is null or
 * not of sufficient length.
 *
 * NOTE: When dealing with a raw dex file, the data to be copied uses
 * little-endian ordering.  Require that oat2dex do any required swapping
 * so this routine can get by with a memcpy().
 *
 * Format of the data:
 *  ushort ident = 0x0300   magic value
 *  ushort width            width of each element in the table
 *  uint   size             number of elements in the table
 *  ubyte  data[size*width] table of data values (may contain a single-byte
 *                          padding at the end)
 */
extern "C" int artHandleFillArrayDataFromCode(Array* array, const uint16_t* table) {
  DCHECK_EQ(table[0], 0x0300);
  if (array == NULL) {
    Thread::Current()->ThrowNewExceptionF("Ljava/lang/NullPointerException;",
        "null array in fill array");
    return -1;  // Error
  }
  DCHECK(array->IsArrayInstance() && !array->IsObjectArray());
  uint32_t size = (uint32_t)table[2] | (((uint32_t)table[3]) << 16);
  if (static_cast<int32_t>(size) > array->GetLength()) {
    Thread::Current()->ThrowNewExceptionF("Ljava/lang/ArrayIndexOutOfBoundsException;",
        "failed array fill. length=%d; index=%d", array->GetLength(), size);
    return -1;  // Error
  }
  uint16_t width = table[1];
  uint32_t size_in_bytes = size * width;
  memcpy((char*)array + Array::DataOffset().Int32Value(), (char*)&table[4], size_in_bytes);
  return 0;  // Success
}

// See comments in runtime_support_asm.S
extern "C" uint64_t artFindInterfaceMethodInCacheFromCode(uint32_t method_idx,
                                                          Object* this_object ,
                                                          Method* caller_method) {
  Thread* thread = Thread::Current();
  if (this_object == NULL) {
    thread->ThrowNewExceptionF("Ljava/lang/NullPointerException;",
        "null receiver during interface dispatch");
    return 0;
  }
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  Method* interface_method = class_linker->ResolveMethod(method_idx, caller_method, false);
  if (interface_method == NULL) {
    // Could not resolve interface method. Throw error and unwind
    CHECK(thread->IsExceptionPending());
    return 0;
  }
  Method* method = this_object->GetClass()->FindVirtualMethodForInterface(interface_method);
  if (method == NULL) {
    CHECK(thread->IsExceptionPending());
    return 0;
  }
  const void* code = method->GetCode();

  uint32_t method_uint = reinterpret_cast<uint32_t>(method);
  uint64_t code_uint = reinterpret_cast<uint32_t>(code);
  uint64_t result = ((code_uint << 32) | method_uint);
  return result;
}

/*
 * Float/double conversion requires clamping to min and max of integer form.  If
 * target doesn't support this normally, use these.
 */
int64_t D2L(double d) {
    static const double kMaxLong = (double)(int64_t)0x7fffffffffffffffULL;
    static const double kMinLong = (double)(int64_t)0x8000000000000000ULL;
    if (d >= kMaxLong)
        return (int64_t)0x7fffffffffffffffULL;
    else if (d <= kMinLong)
        return (int64_t)0x8000000000000000ULL;
    else if (d != d) // NaN case
        return 0;
    else
        return (int64_t)d;
}

int64_t F2L(float f) {
    static const float kMaxLong = (float)(int64_t)0x7fffffffffffffffULL;
    static const float kMinLong = (float)(int64_t)0x8000000000000000ULL;
    if (f >= kMaxLong)
        return (int64_t)0x7fffffffffffffffULL;
    else if (f <= kMinLong)
        return (int64_t)0x8000000000000000ULL;
    else if (f != f) // NaN case
        return 0;
    else
        return (int64_t)f;
}

}  // namespace art
