/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include "unstarted_runtime.h"

#include <cmath>
#include <unordered_map>

#include "ScopedLocalRef.h"

#include "base/logging.h"
#include "base/macros.h"
#include "class_linker.h"
#include "common_throws.h"
#include "entrypoints/entrypoint_utils-inl.h"
#include "handle_scope-inl.h"
#include "interpreter/interpreter_common.h"
#include "mirror/array-inl.h"
#include "mirror/art_method-inl.h"
#include "mirror/class.h"
#include "mirror/field-inl.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/string-inl.h"
#include "nth_caller_visitor.h"
#include "thread.h"
#include "transaction.h"
#include "well_known_classes.h"
#include "zip_archive.h"

namespace art {
namespace interpreter {

static void AbortTransactionOrFail(Thread* self, const char* fmt, ...)
    __attribute__((__format__(__printf__, 2, 3)))
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

static void AbortTransactionOrFail(Thread* self, const char* fmt, ...) {
  va_list args;
  if (Runtime::Current()->IsActiveTransaction()) {
    va_start(args, fmt);
    AbortTransactionV(self, fmt, args);
    va_end(args);
  } else {
    va_start(args, fmt);
    std::string msg;
    StringAppendV(&msg, fmt, args);
    va_end(args);
    LOG(FATAL) << "Trying to abort, but not in transaction mode: " << msg;
    UNREACHABLE();
  }
}

// Helper function to deal with class loading in an unstarted runtime.
static void UnstartedRuntimeFindClass(Thread* self, Handle<mirror::String> className,
                                      Handle<mirror::ClassLoader> class_loader, JValue* result,
                                      const std::string& method_name, bool initialize_class,
                                      bool abort_if_not_found)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  CHECK(className.Get() != nullptr);
  std::string descriptor(DotToDescriptor(className->ToModifiedUtf8().c_str()));
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();

  mirror::Class* found = class_linker->FindClass(self, descriptor.c_str(), class_loader);
  if (found == nullptr && abort_if_not_found) {
    if (!self->IsExceptionPending()) {
      AbortTransactionOrFail(self, "%s failed in un-started runtime for class: %s",
                             method_name.c_str(), PrettyDescriptor(descriptor.c_str()).c_str());
    }
    return;
  }
  if (found != nullptr && initialize_class) {
    StackHandleScope<1> hs(self);
    Handle<mirror::Class> h_class(hs.NewHandle(found));
    if (!class_linker->EnsureInitialized(self, h_class, true, true)) {
      CHECK(self->IsExceptionPending());
      return;
    }
  }
  result->SetL(found);
}

// Common helper for class-loading cutouts in an unstarted runtime. We call Runtime methods that
// rely on Java code to wrap errors in the correct exception class (i.e., NoClassDefFoundError into
// ClassNotFoundException), so need to do the same. The only exception is if the exception is
// actually the transaction abort exception. This must not be wrapped, as it signals an
// initialization abort.
static void CheckExceptionGenerateClassNotFound(Thread* self)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  if (self->IsExceptionPending()) {
    // If it is not the transaction abort exception, wrap it.
    std::string type(PrettyTypeOf(self->GetException()));
    if (type != Transaction::kAbortExceptionDescriptor) {
      self->ThrowNewWrappedException("Ljava/lang/ClassNotFoundException;",
                                     "ClassNotFoundException");
    }
  }
}

static mirror::String* GetClassName(Thread* self, ShadowFrame* shadow_frame, size_t arg_offset)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::Object* param = shadow_frame->GetVRegReference(arg_offset);
  if (param == nullptr) {
    AbortTransactionOrFail(self, "Null-pointer in Class.forName.");
    return nullptr;
  }
  return param->AsString();
}

static void UnstartedClassForName(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::String* class_name = GetClassName(self, shadow_frame, arg_offset);
  if (class_name == nullptr) {
    return;
  }
  StackHandleScope<1> hs(self);
  Handle<mirror::String> h_class_name(hs.NewHandle(class_name));
  UnstartedRuntimeFindClass(self, h_class_name, NullHandle<mirror::ClassLoader>(), result,
                            "Class.forName", true, false);
  CheckExceptionGenerateClassNotFound(self);
}

static void UnstartedClassForNameLong(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::String* class_name = GetClassName(self, shadow_frame, arg_offset);
  if (class_name == nullptr) {
    return;
  }
  bool initialize_class = shadow_frame->GetVReg(arg_offset + 1) != 0;
  mirror::ClassLoader* class_loader =
      down_cast<mirror::ClassLoader*>(shadow_frame->GetVRegReference(arg_offset + 2));
  StackHandleScope<2> hs(self);
  Handle<mirror::String> h_class_name(hs.NewHandle(class_name));
  Handle<mirror::ClassLoader> h_class_loader(hs.NewHandle(class_loader));
  UnstartedRuntimeFindClass(self, h_class_name, h_class_loader, result, "Class.forName",
                            initialize_class, false);
  CheckExceptionGenerateClassNotFound(self);
}

static void UnstartedClassClassForName(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::String* class_name = GetClassName(self, shadow_frame, arg_offset);
  if (class_name == nullptr) {
    return;
  }
  bool initialize_class = shadow_frame->GetVReg(arg_offset + 1) != 0;
  mirror::ClassLoader* class_loader =
      down_cast<mirror::ClassLoader*>(shadow_frame->GetVRegReference(arg_offset + 2));
  StackHandleScope<2> hs(self);
  Handle<mirror::String> h_class_name(hs.NewHandle(class_name));
  Handle<mirror::ClassLoader> h_class_loader(hs.NewHandle(class_loader));
  UnstartedRuntimeFindClass(self, h_class_name, h_class_loader, result, "Class.classForName",
                            initialize_class, false);
  CheckExceptionGenerateClassNotFound(self);
}

static void UnstartedClassNewInstance(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  StackHandleScope<3> hs(self);  // Class, constructor, object.
  mirror::Object* param = shadow_frame->GetVRegReference(arg_offset);
  if (param == nullptr) {
    AbortTransactionOrFail(self, "Null-pointer in Class.newInstance.");
    return;
  }
  mirror::Class* klass = param->AsClass();
  Handle<mirror::Class> h_klass(hs.NewHandle(klass));

  // Check that it's not null.
  if (h_klass.Get() == nullptr) {
    AbortTransactionOrFail(self, "Class reference is null for newInstance");
    return;
  }

  // If we're in a transaction, class must not be finalizable (it or a superclass has a finalizer).
  if (Runtime::Current()->IsActiveTransaction()) {
    if (h_klass.Get()->IsFinalizable()) {
      AbortTransactionF(self, "Class for newInstance is finalizable: '%s'",
                        PrettyClass(h_klass.Get()).c_str());
      return;
    }
  }

  // There are two situations in which we'll abort this run.
  //  1) If the class isn't yet initialized and initialization fails.
  //  2) If we can't find the default constructor. We'll postpone the exception to runtime.
  // Note that 2) could likely be handled here, but for safety abort the transaction.
  bool ok = false;
  if (Runtime::Current()->GetClassLinker()->EnsureInitialized(self, h_klass, true, true)) {
    Handle<mirror::ArtMethod> h_cons(hs.NewHandle(
        h_klass->FindDeclaredDirectMethod("<init>", "()V")));
    if (h_cons.Get() != nullptr) {
      Handle<mirror::Object> h_obj(hs.NewHandle(klass->AllocObject(self)));
      CHECK(h_obj.Get() != nullptr);  // We don't expect OOM at compile-time.
      EnterInterpreterFromInvoke(self, h_cons.Get(), h_obj.Get(), nullptr, nullptr);
      if (!self->IsExceptionPending()) {
        result->SetL(h_obj.Get());
        ok = true;
      }
    } else {
      self->ThrowNewExceptionF("Ljava/lang/InternalError;",
                               "Could not find default constructor for '%s'",
                               PrettyClass(h_klass.Get()).c_str());
    }
  }
  if (!ok) {
    AbortTransactionOrFail(self, "Failed in Class.newInstance for '%s' with %s",
                           PrettyClass(h_klass.Get()).c_str(),
                           PrettyTypeOf(self->GetException()).c_str());
  }
}

static void UnstartedClassGetDeclaredField(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  // Special managed code cut-out to allow field lookup in a un-started runtime that'd fail
  // going the reflective Dex way.
  mirror::Class* klass = shadow_frame->GetVRegReference(arg_offset)->AsClass();
  mirror::String* name2 = shadow_frame->GetVRegReference(arg_offset + 1)->AsString();
  ArtField* found = nullptr;
  ArtField* fields = klass->GetIFields();
  for (int32_t i = 0, count = klass->NumInstanceFields(); i < count; ++i) {
    ArtField* f = &fields[i];
    if (name2->Equals(f->GetName())) {
      found = f;
      break;
    }
  }
  if (found == nullptr) {
    fields = klass->GetSFields();
    for (int32_t i = 0, count = klass->NumStaticFields(); i < count; ++i) {
      ArtField* f = &fields[i];
      if (name2->Equals(f->GetName())) {
        found = f;
        break;
      }
    }
  }
  if (found == nullptr) {
    AbortTransactionOrFail(self, "Failed to find field in Class.getDeclaredField in un-started "
                           " runtime. name=%s class=%s", name2->ToModifiedUtf8().c_str(),
                           PrettyDescriptor(klass).c_str());
    return;
  }
  if (Runtime::Current()->IsActiveTransaction()) {
    result->SetL(mirror::Field::CreateFromArtField<true>(self, found, true));
  } else {
    result->SetL(mirror::Field::CreateFromArtField<false>(self, found, true));
  }
}

static void UnstartedVmClassLoaderFindLoadedClass(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::String* class_name = shadow_frame->GetVRegReference(arg_offset + 1)->AsString();
  mirror::ClassLoader* class_loader =
      down_cast<mirror::ClassLoader*>(shadow_frame->GetVRegReference(arg_offset));
  StackHandleScope<2> hs(self);
  Handle<mirror::String> h_class_name(hs.NewHandle(class_name));
  Handle<mirror::ClassLoader> h_class_loader(hs.NewHandle(class_loader));
  UnstartedRuntimeFindClass(self, h_class_name, h_class_loader, result,
                            "VMClassLoader.findLoadedClass", false, false);
  // This might have an error pending. But semantics are to just return null.
  if (self->IsExceptionPending()) {
    // If it is an InternalError, keep it. See CheckExceptionGenerateClassNotFound.
    std::string type(PrettyTypeOf(self->GetException()));
    if (type != "java.lang.InternalError") {
      self->ClearException();
    }
  }
}

static void UnstartedVoidLookupType(Thread* self ATTRIBUTE_UNUSED,
                                    ShadowFrame* shadow_frame ATTRIBUTE_UNUSED,
                                    JValue* result,
                                    size_t arg_offset ATTRIBUTE_UNUSED)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  result->SetL(Runtime::Current()->GetClassLinker()->FindPrimitiveClass('V'));
}

// Arraycopy emulation.
// Note: we can't use any fast copy functions, as they are not available under transaction.

template <typename T>
static void PrimitiveArrayCopy(Thread* self,
                               mirror::Array* src_array, int32_t src_pos,
                               mirror::Array* dst_array, int32_t dst_pos,
                               int32_t length)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  if (src_array->GetClass()->GetComponentType() != dst_array->GetClass()->GetComponentType()) {
    AbortTransactionOrFail(self, "Types mismatched in arraycopy: %s vs %s.",
                           PrettyDescriptor(src_array->GetClass()->GetComponentType()).c_str(),
                           PrettyDescriptor(dst_array->GetClass()->GetComponentType()).c_str());
    return;
  }
  mirror::PrimitiveArray<T>* src = down_cast<mirror::PrimitiveArray<T>*>(src_array);
  mirror::PrimitiveArray<T>* dst = down_cast<mirror::PrimitiveArray<T>*>(dst_array);
  const bool copy_forward = (dst_pos < src_pos) || (dst_pos - src_pos >= length);
  if (copy_forward) {
    for (int32_t i = 0; i < length; ++i) {
      dst->Set(dst_pos + i, src->Get(src_pos + i));
    }
  } else {
    for (int32_t i = 1; i <= length; ++i) {
      dst->Set(dst_pos + length - i, src->Get(src_pos + length - i));
    }
  }
}

static void UnstartedSystemArraycopy(
    Thread* self, ShadowFrame* shadow_frame, JValue* result ATTRIBUTE_UNUSED, size_t arg_offset)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  // Special case array copying without initializing System.
  jint src_pos = shadow_frame->GetVReg(arg_offset + 1);
  jint dst_pos = shadow_frame->GetVReg(arg_offset + 3);
  jint length = shadow_frame->GetVReg(arg_offset + 4);
  mirror::Array* src_array = shadow_frame->GetVRegReference(arg_offset)->AsArray();
  mirror::Array* dst_array = shadow_frame->GetVRegReference(arg_offset + 2)->AsArray();

  // Null checking.
  if (src_array == nullptr) {
    AbortTransactionOrFail(self, "src is null in arraycopy.");
    return;
  }
  if (dst_array == nullptr) {
    AbortTransactionOrFail(self, "dst is null in arraycopy.");
    return;
  }

  // Bounds checking.
  if (UNLIKELY(src_pos < 0) || UNLIKELY(dst_pos < 0) || UNLIKELY(length < 0) ||
      UNLIKELY(src_pos > src_array->GetLength() - length) ||
      UNLIKELY(dst_pos > dst_array->GetLength() - length)) {
    self->ThrowNewExceptionF("Ljava/lang/ArrayIndexOutOfBoundsException;",
                             "src.length=%d srcPos=%d dst.length=%d dstPos=%d length=%d",
                             src_array->GetLength(), src_pos, dst_array->GetLength(), dst_pos,
                             length);
    AbortTransactionOrFail(self, "Index out of bounds.");
    return;
  }

  // Type checking.
  mirror::Class* src_type = shadow_frame->GetVRegReference(arg_offset)->GetClass()->
      GetComponentType();

  if (!src_type->IsPrimitive()) {
    // Check that the second type is not primitive.
    mirror::Class* trg_type = shadow_frame->GetVRegReference(arg_offset + 2)->GetClass()->
        GetComponentType();
    if (trg_type->IsPrimitiveInt()) {
      AbortTransactionOrFail(self, "Type mismatch in arraycopy: %s vs %s",
                             PrettyDescriptor(src_array->GetClass()->GetComponentType()).c_str(),
                             PrettyDescriptor(dst_array->GetClass()->GetComponentType()).c_str());
      return;
    }

    // For simplicity only do this if the component types are the same. Otherwise we have to copy
    // even more code from the object-array functions.
    if (src_type != trg_type) {
      AbortTransactionOrFail(self, "Types not the same in arraycopy: %s vs %s",
                             PrettyDescriptor(src_array->GetClass()->GetComponentType()).c_str(),
                             PrettyDescriptor(dst_array->GetClass()->GetComponentType()).c_str());
      return;
    }

    mirror::ObjectArray<mirror::Object>* src = src_array->AsObjectArray<mirror::Object>();
    mirror::ObjectArray<mirror::Object>* dst = dst_array->AsObjectArray<mirror::Object>();
    if (src == dst) {
      // Can overlap, but not have type mismatches.
      const bool copy_forward = (dst_pos < src_pos) || (dst_pos - src_pos >= length);
      if (copy_forward) {
        for (int32_t i = 0; i < length; ++i) {
          dst->Set(dst_pos + i, src->Get(src_pos + i));
        }
      } else {
        for (int32_t i = 1; i <= length; ++i) {
          dst->Set(dst_pos + length - i, src->Get(src_pos + length - i));
        }
      }
    } else {
      // Can't overlap. Would need type checks, but we abort above.
      for (int32_t i = 0; i < length; ++i) {
        dst->Set(dst_pos + i, src->Get(src_pos + i));
      }
    }
  } else if (src_type->IsPrimitiveChar()) {
    PrimitiveArrayCopy<uint16_t>(self, src_array, src_pos, dst_array, dst_pos, length);
  } else if (src_type->IsPrimitiveInt()) {
    PrimitiveArrayCopy<int32_t>(self, src_array, src_pos, dst_array, dst_pos, length);
  } else {
    AbortTransactionOrFail(self, "Unimplemented System.arraycopy for type '%s'",
                           PrettyDescriptor(src_type).c_str());
  }
}

static void UnstartedThreadLocalGet(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset ATTRIBUTE_UNUSED)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  std::string caller(PrettyMethod(shadow_frame->GetLink()->GetMethod()));
  bool ok = false;
  if (caller == "java.lang.String java.lang.IntegralToString.convertInt"
                "(java.lang.AbstractStringBuilder, int)") {
    // Allocate non-threadlocal buffer.
    result->SetL(mirror::CharArray::Alloc(self, 11));
    ok = true;
  } else if (caller == "java.lang.RealToString java.lang.RealToString.getInstance()") {
    // Note: RealToString is implemented and used in a different fashion than IntegralToString.
    // Conversion is done over an actual object of RealToString (the conversion method is an
    // instance method). This means it is not as clear whether it is correct to return a new
    // object each time. The caller needs to be inspected by hand to see whether it (incorrectly)
    // stores the object for later use.
    // See also b/19548084 for a possible rewrite and bringing it in line with IntegralToString.
    if (shadow_frame->GetLink()->GetLink() != nullptr) {
      std::string caller2(PrettyMethod(shadow_frame->GetLink()->GetLink()->GetMethod()));
      if (caller2 == "java.lang.String java.lang.Double.toString(double)") {
        // Allocate new object.
        StackHandleScope<2> hs(self);
        Handle<mirror::Class> h_real_to_string_class(hs.NewHandle(
            shadow_frame->GetLink()->GetMethod()->GetDeclaringClass()));
        Handle<mirror::Object> h_real_to_string_obj(hs.NewHandle(
            h_real_to_string_class->AllocObject(self)));
        if (h_real_to_string_obj.Get() != nullptr) {
          mirror::ArtMethod* init_method =
              h_real_to_string_class->FindDirectMethod("<init>", "()V");
          if (init_method == nullptr) {
            h_real_to_string_class->DumpClass(LOG(FATAL), mirror::Class::kDumpClassFullDetail);
          } else {
            JValue invoke_result;
            EnterInterpreterFromInvoke(self, init_method, h_real_to_string_obj.Get(), nullptr,
                                       nullptr);
            if (!self->IsExceptionPending()) {
              result->SetL(h_real_to_string_obj.Get());
              ok = true;
            }
          }
        }
      }
    }
  }

  if (!ok) {
    AbortTransactionOrFail(self, "Could not create RealToString object");
  }
}

static void UnstartedMathCeil(
    Thread* self ATTRIBUTE_UNUSED, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  double in = shadow_frame->GetVRegDouble(arg_offset);
  double out;
  // Special cases:
  // 1) NaN, infinity, +0, -0 -> out := in. All are guaranteed by cmath.
  // -1 < in < 0 -> out := -0.
  if (-1.0 < in && in < 0) {
    out = -0.0;
  } else {
    out = ceil(in);
  }
  result->SetD(out);
}

static void UnstartedArtMethodGetMethodName(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::ArtMethod* method = shadow_frame->GetVRegReference(arg_offset)->AsArtMethod();
  result->SetL(method->GetNameAsString(self));
}

static void UnstartedObjectHashCode(
    Thread* self ATTRIBUTE_UNUSED, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::Object* obj = shadow_frame->GetVRegReference(arg_offset);
  result->SetI(obj->IdentityHashCode());
}

static void UnstartedDoubleDoubleToRawLongBits(
    Thread* self ATTRIBUTE_UNUSED, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  double in = shadow_frame->GetVRegDouble(arg_offset);
  result->SetJ(bit_cast<int64_t, double>(in));
}

static mirror::Object* GetDexFromDexCache(Thread* self, mirror::DexCache* dex_cache)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  const DexFile* dex_file = dex_cache->GetDexFile();
  if (dex_file == nullptr) {
    return nullptr;
  }

  // Create the direct byte buffer.
  JNIEnv* env = self->GetJniEnv();
  DCHECK(env != nullptr);
  void* address = const_cast<void*>(reinterpret_cast<const void*>(dex_file->Begin()));
  ScopedLocalRef<jobject> byte_buffer(env, env->NewDirectByteBuffer(address, dex_file->Size()));
  if (byte_buffer.get() == nullptr) {
    DCHECK(self->IsExceptionPending());
    return nullptr;
  }

  jvalue args[1];
  args[0].l = byte_buffer.get();

  ScopedLocalRef<jobject> dex(env, env->CallStaticObjectMethodA(
      WellKnownClasses::com_android_dex_Dex,
      WellKnownClasses::com_android_dex_Dex_create,
      args));

  return self->DecodeJObject(dex.get());
}

static void UnstartedDexCacheGetDexNative(
    Thread* self, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  // We will create the Dex object, but the image writer will release it before creating the
  // art file.
  mirror::Object* src = shadow_frame->GetVRegReference(arg_offset);
  bool have_dex = false;
  if (src != nullptr) {
    mirror::Object* dex = GetDexFromDexCache(self, reinterpret_cast<mirror::DexCache*>(src));
    if (dex != nullptr) {
      have_dex = true;
      result->SetL(dex);
    }
  }
  if (!have_dex) {
    self->ClearException();
    Runtime::Current()->AbortTransactionAndThrowAbortError(self, "Could not create Dex object");
  }
}

static void UnstartedMemoryPeek(
    Primitive::Type type, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  int64_t address = shadow_frame->GetVRegLong(arg_offset);
  // TODO: Check that this is in the heap somewhere. Otherwise we will segfault instead of
  //       aborting the transaction.

  switch (type) {
    case Primitive::kPrimByte: {
      result->SetB(*reinterpret_cast<int8_t*>(static_cast<intptr_t>(address)));
      return;
    }

    case Primitive::kPrimShort: {
      result->SetS(*reinterpret_cast<int16_t*>(static_cast<intptr_t>(address)));
      return;
    }

    case Primitive::kPrimInt: {
      result->SetI(*reinterpret_cast<int32_t*>(static_cast<intptr_t>(address)));
      return;
    }

    case Primitive::kPrimLong: {
      result->SetJ(*reinterpret_cast<int64_t*>(static_cast<intptr_t>(address)));
      return;
    }

    case Primitive::kPrimBoolean:
    case Primitive::kPrimChar:
    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
    case Primitive::kPrimVoid:
    case Primitive::kPrimNot:
      LOG(FATAL) << "Not in the Memory API: " << type;
      UNREACHABLE();
  }
  LOG(FATAL) << "Should not reach here";
  UNREACHABLE();
}

static void UnstartedMemoryPeekEntry(
    Thread* self ATTRIBUTE_UNUSED, ShadowFrame* shadow_frame, JValue* result, size_t arg_offset)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  std::string name(PrettyMethod(shadow_frame->GetMethod()));
  if (name == "byte libcore.io.Memory.peekByte(long)") {
    UnstartedMemoryPeek(Primitive::kPrimByte, shadow_frame, result, arg_offset);
  } else if (name == "short libcore.io.Memory.peekShortNative(long)") {
    UnstartedMemoryPeek(Primitive::kPrimShort, shadow_frame, result, arg_offset);
  } else if (name == "int libcore.io.Memory.peekIntNative(long)") {
    UnstartedMemoryPeek(Primitive::kPrimInt, shadow_frame, result, arg_offset);
  } else if (name == "long libcore.io.Memory.peekLongNative(long)") {
    UnstartedMemoryPeek(Primitive::kPrimLong, shadow_frame, result, arg_offset);
  } else {
    LOG(FATAL) << "Unsupported Memory.peek entry: " << name;
    UNREACHABLE();
  }
}

static void UnstartedMemoryPeekArray(
    Primitive::Type type, Thread* self, ShadowFrame* shadow_frame, size_t arg_offset)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  int64_t address_long = shadow_frame->GetVRegLong(arg_offset);
  mirror::Object* obj = shadow_frame->GetVRegReference(arg_offset + 2);
  if (obj == nullptr) {
    Runtime::Current()->AbortTransactionAndThrowAbortError(self, "Null pointer in peekArray");
    return;
  }
  mirror::Array* array = obj->AsArray();

  int offset = shadow_frame->GetVReg(arg_offset + 3);
  int count = shadow_frame->GetVReg(arg_offset + 4);
  if (offset < 0 || offset + count > array->GetLength()) {
    std::string error_msg(StringPrintf("Array out of bounds in peekArray: %d/%d vs %d",
                                       offset, count, array->GetLength()));
    Runtime::Current()->AbortTransactionAndThrowAbortError(self, error_msg.c_str());
    return;
  }

  switch (type) {
    case Primitive::kPrimByte: {
      int8_t* address = reinterpret_cast<int8_t*>(static_cast<intptr_t>(address_long));
      mirror::ByteArray* byte_array = array->AsByteArray();
      for (int32_t i = 0; i < count; ++i, ++address) {
        byte_array->SetWithoutChecks<true>(i + offset, *address);
      }
      return;
    }

    case Primitive::kPrimShort:
    case Primitive::kPrimInt:
    case Primitive::kPrimLong:
      LOG(FATAL) << "Type unimplemented for Memory Array API, should not reach here: " << type;
      UNREACHABLE();

    case Primitive::kPrimBoolean:
    case Primitive::kPrimChar:
    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
    case Primitive::kPrimVoid:
    case Primitive::kPrimNot:
      LOG(FATAL) << "Not in the Memory API: " << type;
      UNREACHABLE();
  }
  LOG(FATAL) << "Should not reach here";
  UNREACHABLE();
}

static void UnstartedMemoryPeekArrayEntry(
    Thread* self, ShadowFrame* shadow_frame, JValue* result ATTRIBUTE_UNUSED, size_t arg_offset)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  std::string name(PrettyMethod(shadow_frame->GetMethod()));
  if (name == "void libcore.io.Memory.peekByteArray(long, byte[], int, int)") {
    UnstartedMemoryPeekArray(Primitive::kPrimByte, self, shadow_frame, arg_offset);
  } else {
    LOG(FATAL) << "Unsupported Memory.peekArray entry: " << name;
    UNREACHABLE();
  }
}

// This allows reading security.properties in an unstarted runtime and initialize Security.
static void UnstartedSecurityGetSecurityPropertiesReader(
    Thread* self,
    ShadowFrame* shadow_frame ATTRIBUTE_UNUSED,
    JValue* result,
    size_t arg_offset ATTRIBUTE_UNUSED)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  Runtime* runtime = Runtime::Current();
  const std::vector<const DexFile*>& path = runtime->GetClassLinker()->GetBootClassPath();
  std::string canonical(DexFile::GetDexCanonicalLocation(path[0]->GetLocation().c_str()));
  mirror::String* string_data;

  // Use a block to enclose the I/O and MemMap code so buffers are released early.
  {
    std::string error_msg;
    std::unique_ptr<ZipArchive> zip_archive(ZipArchive::Open(canonical.c_str(), &error_msg));
    if (zip_archive.get() == nullptr) {
      AbortTransactionOrFail(self, "Could not open zip file %s: %s", canonical.c_str(),
                             error_msg.c_str());
      return;
    }
    std::unique_ptr<ZipEntry> zip_entry(zip_archive->Find("java/security/security.properties",
                                                          &error_msg));
    if (zip_entry.get() == nullptr) {
      AbortTransactionOrFail(self, "Could not find security.properties file in %s: %s",
                             canonical.c_str(), error_msg.c_str());
      return;
    }
    std::unique_ptr<MemMap> map(zip_entry->ExtractToMemMap(canonical.c_str(),
                                                           "java/security/security.properties",
                                                           &error_msg));
    if (map.get() == nullptr) {
      AbortTransactionOrFail(self, "Could not unzip security.properties file in %s: %s",
                             canonical.c_str(), error_msg.c_str());
      return;
    }

    uint32_t length = zip_entry->GetUncompressedLength();
    std::unique_ptr<char[]> tmp(new char[length + 1]);
    memcpy(tmp.get(), map->Begin(), length);
    tmp.get()[length] = 0;  // null terminator

    string_data = mirror::String::AllocFromModifiedUtf8(self, tmp.get());
  }

  if (string_data == nullptr) {
    AbortTransactionOrFail(self, "Could not create string from file content of %s",
                           canonical.c_str());
    return;
  }

  // Create a StringReader.
  StackHandleScope<3> hs(self);
  Handle<mirror::String> h_string(hs.NewHandle(string_data));

  Handle<mirror::Class> h_class(hs.NewHandle(
      runtime->GetClassLinker()->FindClass(self,
                                           "Ljava/io/StringReader;",
                                           NullHandle<mirror::ClassLoader>())));
  if (h_class.Get() == nullptr) {
    AbortTransactionOrFail(self, "Could not find StringReader class");
    return;
  }

  if (!runtime->GetClassLinker()->EnsureInitialized(self, h_class, true, true)) {
    AbortTransactionOrFail(self, "Could not initialize StringReader class");
    return;
  }

  Handle<mirror::Object> h_obj(hs.NewHandle(h_class->AllocObject(self)));
  if (h_obj.Get() == nullptr) {
    AbortTransactionOrFail(self, "Could not allocate StringReader object");
    return;
  }

  mirror::ArtMethod* constructor = h_class->FindDeclaredDirectMethod("<init>",
                                                                     "(Ljava/lang/String;)V");
  if (constructor == nullptr) {
    AbortTransactionOrFail(self, "Could not find StringReader constructor");
    return;
  }

  uint32_t args[1];
  args[0] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(h_string.Get()));
  EnterInterpreterFromInvoke(self, constructor, h_obj.Get(), args, nullptr);

  if (self->IsExceptionPending()) {
    AbortTransactionOrFail(self, "Could not run StringReader constructor");
    return;
  }

  result->SetL(h_obj.Get());
}

static void UnstartedJNIVMRuntimeNewUnpaddedArray(Thread* self,
                                                  mirror::ArtMethod* method ATTRIBUTE_UNUSED,
                                                  mirror::Object* receiver ATTRIBUTE_UNUSED,
                                                  uint32_t* args,
                                                  JValue* result)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  int32_t length = args[1];
  DCHECK_GE(length, 0);
  mirror::Class* element_class = reinterpret_cast<mirror::Object*>(args[0])->AsClass();
  Runtime* runtime = Runtime::Current();
  mirror::Class* array_class = runtime->GetClassLinker()->FindArrayClass(self, &element_class);
  DCHECK(array_class != nullptr);
  gc::AllocatorType allocator = runtime->GetHeap()->GetCurrentAllocator();
  result->SetL(mirror::Array::Alloc<true, true>(self, array_class, length,
                                                array_class->GetComponentSizeShift(), allocator));
}

static void UnstartedJNIVMStackGetCallingClassLoader(Thread* self ATTRIBUTE_UNUSED,
                                                     mirror::ArtMethod* method ATTRIBUTE_UNUSED,
                                                     mirror::Object* receiver ATTRIBUTE_UNUSED,
                                                     uint32_t* args ATTRIBUTE_UNUSED,
                                                     JValue* result) {
  result->SetL(nullptr);
}

static void UnstartedJNIVMStackGetStackClass2(Thread* self,
                                              mirror::ArtMethod* method ATTRIBUTE_UNUSED,
                                              mirror::Object* receiver ATTRIBUTE_UNUSED,
                                              uint32_t* args ATTRIBUTE_UNUSED,
                                              JValue* result)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  NthCallerVisitor visitor(self, 3);
  visitor.WalkStack();
  if (visitor.caller != nullptr) {
    result->SetL(visitor.caller->GetDeclaringClass());
  }
}

static void UnstartedJNIMathLog(Thread* self ATTRIBUTE_UNUSED,
                                mirror::ArtMethod* method ATTRIBUTE_UNUSED,
                                mirror::Object* receiver ATTRIBUTE_UNUSED,
                                uint32_t* args,
                                JValue* result) {
  JValue value;
  value.SetJ((static_cast<uint64_t>(args[1]) << 32) | args[0]);
  result->SetD(log(value.GetD()));
}

static void UnstartedJNIMathExp(Thread* self ATTRIBUTE_UNUSED,
                                mirror::ArtMethod* method ATTRIBUTE_UNUSED,
                                mirror::Object* receiver ATTRIBUTE_UNUSED,
                                uint32_t* args,
                                JValue* result) {
  JValue value;
  value.SetJ((static_cast<uint64_t>(args[1]) << 32) | args[0]);
  result->SetD(exp(value.GetD()));
}

static void UnstartedJNIClassGetNameNative(Thread* self,
                                           mirror::ArtMethod* method ATTRIBUTE_UNUSED,
                                           mirror::Object* receiver,
                                           uint32_t* args ATTRIBUTE_UNUSED,
                                           JValue* result)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  StackHandleScope<1> hs(self);
  result->SetL(mirror::Class::ComputeName(hs.NewHandle(receiver->AsClass())));
}

static void UnstartedJNIFloatFloatToRawIntBits(Thread* self ATTRIBUTE_UNUSED,
                                               mirror::ArtMethod* method ATTRIBUTE_UNUSED,
                                               mirror::Object* receiver ATTRIBUTE_UNUSED,
                                               uint32_t* args,
                                               JValue* result) {
  result->SetI(args[0]);
}

static void UnstartedJNIFloatIntBitsToFloat(Thread* self ATTRIBUTE_UNUSED,
                                            mirror::ArtMethod* method ATTRIBUTE_UNUSED,
                                            mirror::Object* receiver ATTRIBUTE_UNUSED,
                                            uint32_t* args,
                                            JValue* result) {
  result->SetI(args[0]);
}

static void UnstartedJNIObjectInternalClone(Thread* self,
                                            mirror::ArtMethod* method ATTRIBUTE_UNUSED,
                                            mirror::Object* receiver,
                                            uint32_t* args ATTRIBUTE_UNUSED,
                                            JValue* result)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  result->SetL(receiver->Clone(self));
}

static void UnstartedJNIObjectNotifyAll(Thread* self,
                                        mirror::ArtMethod* method ATTRIBUTE_UNUSED,
                                        mirror::Object* receiver,
                                        uint32_t* args ATTRIBUTE_UNUSED,
                                        JValue* result ATTRIBUTE_UNUSED)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  receiver->NotifyAll(self);
}

static void UnstartedJNIStringCompareTo(Thread* self,
                                        mirror::ArtMethod* method ATTRIBUTE_UNUSED,
                                        mirror::Object* receiver,
                                        uint32_t* args,
                                        JValue* result)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::String* rhs = reinterpret_cast<mirror::Object*>(args[0])->AsString();
  if (rhs == nullptr) {
    AbortTransactionOrFail(self, "String.compareTo with null object");
  }
  result->SetI(receiver->AsString()->CompareTo(rhs));
}

static void UnstartedJNIStringIntern(Thread* self ATTRIBUTE_UNUSED,
                                     mirror::ArtMethod* method ATTRIBUTE_UNUSED,
                                     mirror::Object* receiver,
                                     uint32_t* args ATTRIBUTE_UNUSED,
                                     JValue* result)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  result->SetL(receiver->AsString()->Intern());
}

static void UnstartedJNIStringFastIndexOf(Thread* self ATTRIBUTE_UNUSED,
                                          mirror::ArtMethod* method ATTRIBUTE_UNUSED,
                                          mirror::Object* receiver,
                                          uint32_t* args,
                                          JValue* result)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  result->SetI(receiver->AsString()->FastIndexOf(args[0], args[1]));
}

static void UnstartedJNIArrayCreateMultiArray(Thread* self,
                                              mirror::ArtMethod* method ATTRIBUTE_UNUSED,
                                              mirror::Object* receiver ATTRIBUTE_UNUSED,
                                              uint32_t* args,
                                              JValue* result)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  StackHandleScope<2> hs(self);
  auto h_class(hs.NewHandle(reinterpret_cast<mirror::Class*>(args[0])->AsClass()));
  auto h_dimensions(hs.NewHandle(reinterpret_cast<mirror::IntArray*>(args[1])->AsIntArray()));
  result->SetL(mirror::Array::CreateMultiArray(self, h_class, h_dimensions));
}

static void UnstartedJNIArrayCreateObjectArray(Thread* self,
                                               mirror::ArtMethod* method ATTRIBUTE_UNUSED,
                                               mirror::Object* receiver ATTRIBUTE_UNUSED,
                                               uint32_t* args,
                                               JValue* result)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  int32_t length = static_cast<int32_t>(args[1]);
  if (length < 0) {
    ThrowNegativeArraySizeException(length);
    return;
  }
  mirror::Class* element_class = reinterpret_cast<mirror::Class*>(args[0])->AsClass();
  Runtime* runtime = Runtime::Current();
  ClassLinker* class_linker = runtime->GetClassLinker();
  mirror::Class* array_class = class_linker->FindArrayClass(self, &element_class);
  if (UNLIKELY(array_class == nullptr)) {
    CHECK(self->IsExceptionPending());
    return;
  }
  DCHECK(array_class->IsObjectArrayClass());
  mirror::Array* new_array = mirror::ObjectArray<mirror::Object*>::Alloc(
      self, array_class, length, runtime->GetHeap()->GetCurrentAllocator());
  result->SetL(new_array);
}

static void UnstartedJNIThrowableNativeFillInStackTrace(Thread* self,
                                                        mirror::ArtMethod* method ATTRIBUTE_UNUSED,
                                                        mirror::Object* receiver ATTRIBUTE_UNUSED,
                                                        uint32_t* args ATTRIBUTE_UNUSED,
                                                        JValue* result)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  ScopedObjectAccessUnchecked soa(self);
  if (Runtime::Current()->IsActiveTransaction()) {
    result->SetL(soa.Decode<mirror::Object*>(self->CreateInternalStackTrace<true>(soa)));
  } else {
    result->SetL(soa.Decode<mirror::Object*>(self->CreateInternalStackTrace<false>(soa)));
  }
}

static void UnstartedJNISystemIdentityHashCode(Thread* self ATTRIBUTE_UNUSED,
                                               mirror::ArtMethod* method ATTRIBUTE_UNUSED,
                                               mirror::Object* receiver ATTRIBUTE_UNUSED,
                                               uint32_t* args,
                                               JValue* result)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::Object* obj = reinterpret_cast<mirror::Object*>(args[0]);
  result->SetI((obj != nullptr) ? obj->IdentityHashCode() : 0);
}

static void UnstartedJNIByteOrderIsLittleEndian(Thread* self ATTRIBUTE_UNUSED,
                                                mirror::ArtMethod* method ATTRIBUTE_UNUSED,
                                                mirror::Object* receiver ATTRIBUTE_UNUSED,
                                                uint32_t* args ATTRIBUTE_UNUSED,
                                                JValue* result) {
  result->SetZ(JNI_TRUE);
}

static void UnstartedJNIUnsafeCompareAndSwapInt(Thread* self ATTRIBUTE_UNUSED,
                                                mirror::ArtMethod* method ATTRIBUTE_UNUSED,
                                                mirror::Object* receiver ATTRIBUTE_UNUSED,
                                                uint32_t* args,
                                                JValue* result)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::Object* obj = reinterpret_cast<mirror::Object*>(args[0]);
  jlong offset = (static_cast<uint64_t>(args[2]) << 32) | args[1];
  jint expectedValue = args[3];
  jint newValue = args[4];
  bool success;
  if (Runtime::Current()->IsActiveTransaction()) {
    success = obj->CasFieldStrongSequentiallyConsistent32<true>(MemberOffset(offset),
                                                                expectedValue, newValue);
  } else {
    success = obj->CasFieldStrongSequentiallyConsistent32<false>(MemberOffset(offset),
                                                                 expectedValue, newValue);
  }
  result->SetZ(success ? JNI_TRUE : JNI_FALSE);
}

static void UnstartedJNIUnsafePutObject(Thread* self ATTRIBUTE_UNUSED,
                                        mirror::ArtMethod* method ATTRIBUTE_UNUSED,
                                        mirror::Object* receiver ATTRIBUTE_UNUSED,
                                        uint32_t* args,
                                        JValue* result ATTRIBUTE_UNUSED)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::Object* obj = reinterpret_cast<mirror::Object*>(args[0]);
  jlong offset = (static_cast<uint64_t>(args[2]) << 32) | args[1];
  mirror::Object* newValue = reinterpret_cast<mirror::Object*>(args[3]);
  if (Runtime::Current()->IsActiveTransaction()) {
    obj->SetFieldObject<true>(MemberOffset(offset), newValue);
  } else {
    obj->SetFieldObject<false>(MemberOffset(offset), newValue);
  }
}

static void UnstartedJNIUnsafeGetArrayBaseOffsetForComponentType(
    Thread* self ATTRIBUTE_UNUSED,
    mirror::ArtMethod* method ATTRIBUTE_UNUSED,
    mirror::Object* receiver ATTRIBUTE_UNUSED,
    uint32_t* args,
    JValue* result)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::Class* component = reinterpret_cast<mirror::Object*>(args[0])->AsClass();
  Primitive::Type primitive_type = component->GetPrimitiveType();
  result->SetI(mirror::Array::DataOffset(Primitive::ComponentSize(primitive_type)).Int32Value());
}

static void UnstartedJNIUnsafeGetArrayIndexScaleForComponentType(
    Thread* self ATTRIBUTE_UNUSED,
    mirror::ArtMethod* method ATTRIBUTE_UNUSED,
    mirror::Object* receiver ATTRIBUTE_UNUSED,
    uint32_t* args,
    JValue* result)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::Class* component = reinterpret_cast<mirror::Object*>(args[0])->AsClass();
  Primitive::Type primitive_type = component->GetPrimitiveType();
  result->SetI(Primitive::ComponentSize(primitive_type));
}

typedef void (*InvokeHandler)(Thread* self, ShadowFrame* shadow_frame, JValue* result,
    size_t arg_size);

typedef void (*JNIHandler)(Thread* self, mirror::ArtMethod* method, mirror::Object* receiver,
    uint32_t* args, JValue* result);

static bool tables_initialized_ = false;
static std::unordered_map<std::string, InvokeHandler> invoke_handlers_;
static std::unordered_map<std::string, JNIHandler> jni_handlers_;

static void UnstartedRuntimeInitializeInvokeHandlers() {
  struct InvokeHandlerDef {
    std::string name;
    InvokeHandler function;
  };

  InvokeHandlerDef defs[] {
      { "java.lang.Class java.lang.Class.forName(java.lang.String)",
          &UnstartedClassForName },
      { "java.lang.Class java.lang.Class.forName(java.lang.String, boolean, java.lang.ClassLoader)",
          &UnstartedClassForNameLong },
      { "java.lang.Class java.lang.Class.classForName(java.lang.String, boolean, java.lang.ClassLoader)",
          &UnstartedClassClassForName },
      { "java.lang.Class java.lang.VMClassLoader.findLoadedClass(java.lang.ClassLoader, java.lang.String)",
          &UnstartedVmClassLoaderFindLoadedClass },
      { "java.lang.Class java.lang.Void.lookupType()",
          &UnstartedVoidLookupType },
      { "java.lang.Object java.lang.Class.newInstance()",
          &UnstartedClassNewInstance },
      { "java.lang.reflect.Field java.lang.Class.getDeclaredField(java.lang.String)",
          &UnstartedClassGetDeclaredField },
      { "int java.lang.Object.hashCode()",
          &UnstartedObjectHashCode },
      { "java.lang.String java.lang.reflect.ArtMethod.getMethodName(java.lang.reflect.ArtMethod)",
          &UnstartedArtMethodGetMethodName },
      { "void java.lang.System.arraycopy(java.lang.Object, int, java.lang.Object, int, int)",
          &UnstartedSystemArraycopy},
      { "void java.lang.System.arraycopy(char[], int, char[], int, int)",
          &UnstartedSystemArraycopy },
      { "void java.lang.System.arraycopy(int[], int, int[], int, int)",
          &UnstartedSystemArraycopy },
      { "long java.lang.Double.doubleToRawLongBits(double)",
          &UnstartedDoubleDoubleToRawLongBits },
      { "double java.lang.Math.ceil(double)",
          &UnstartedMathCeil },
      { "java.lang.Object java.lang.ThreadLocal.get()",
          &UnstartedThreadLocalGet },
      { "com.android.dex.Dex java.lang.DexCache.getDexNative()",
          &UnstartedDexCacheGetDexNative },
      { "byte libcore.io.Memory.peekByte(long)",
          &UnstartedMemoryPeekEntry },
      { "short libcore.io.Memory.peekShortNative(long)",
          &UnstartedMemoryPeekEntry },
      { "int libcore.io.Memory.peekIntNative(long)",
          &UnstartedMemoryPeekEntry },
      { "long libcore.io.Memory.peekLongNative(long)",
          &UnstartedMemoryPeekEntry },
      { "void libcore.io.Memory.peekByteArray(long, byte[], int, int)",
          &UnstartedMemoryPeekArrayEntry },
      { "java.io.Reader java.security.Security.getSecurityPropertiesReader()",
          &UnstartedSecurityGetSecurityPropertiesReader },
  };

  for (auto& def : defs) {
    invoke_handlers_.insert(std::make_pair(def.name, def.function));
  }
}

static void UnstartedRuntimeInitializeJNIHandlers() {
  struct JNIHandlerDef {
    std::string name;
    JNIHandler function;
  };

  JNIHandlerDef defs[] {
      { "java.lang.Object dalvik.system.VMRuntime.newUnpaddedArray(java.lang.Class, int)",
          &UnstartedJNIVMRuntimeNewUnpaddedArray },
      { "java.lang.ClassLoader dalvik.system.VMStack.getCallingClassLoader()",
          &UnstartedJNIVMStackGetCallingClassLoader },
      { "java.lang.Class dalvik.system.VMStack.getStackClass2()",
          &UnstartedJNIVMStackGetStackClass2 },
      { "double java.lang.Math.log(double)",
          &UnstartedJNIMathLog },
      { "java.lang.String java.lang.Class.getNameNative()",
          &UnstartedJNIClassGetNameNative },
      { "int java.lang.Float.floatToRawIntBits(float)",
          &UnstartedJNIFloatFloatToRawIntBits },
      { "float java.lang.Float.intBitsToFloat(int)",
          &UnstartedJNIFloatIntBitsToFloat },
      { "double java.lang.Math.exp(double)",
          &UnstartedJNIMathExp },
      { "java.lang.Object java.lang.Object.internalClone()",
          &UnstartedJNIObjectInternalClone },
      { "void java.lang.Object.notifyAll()",
          &UnstartedJNIObjectNotifyAll},
      { "int java.lang.String.compareTo(java.lang.String)",
          &UnstartedJNIStringCompareTo },
      { "java.lang.String java.lang.String.intern()",
          &UnstartedJNIStringIntern },
      { "int java.lang.String.fastIndexOf(int, int)",
          &UnstartedJNIStringFastIndexOf },
      { "java.lang.Object java.lang.reflect.Array.createMultiArray(java.lang.Class, int[])",
          &UnstartedJNIArrayCreateMultiArray },
      { "java.lang.Object java.lang.reflect.Array.createObjectArray(java.lang.Class, int)",
          &UnstartedJNIArrayCreateObjectArray },
      { "java.lang.Object java.lang.Throwable.nativeFillInStackTrace()",
          &UnstartedJNIThrowableNativeFillInStackTrace },
      { "int java.lang.System.identityHashCode(java.lang.Object)",
          &UnstartedJNISystemIdentityHashCode },
      { "boolean java.nio.ByteOrder.isLittleEndian()",
          &UnstartedJNIByteOrderIsLittleEndian },
      { "boolean sun.misc.Unsafe.compareAndSwapInt(java.lang.Object, long, int, int)",
          &UnstartedJNIUnsafeCompareAndSwapInt },
      { "void sun.misc.Unsafe.putObject(java.lang.Object, long, java.lang.Object)",
          &UnstartedJNIUnsafePutObject },
      { "int sun.misc.Unsafe.getArrayBaseOffsetForComponentType(java.lang.Class)",
          &UnstartedJNIUnsafeGetArrayBaseOffsetForComponentType },
      { "int sun.misc.Unsafe.getArrayIndexScaleForComponentType(java.lang.Class)",
          &UnstartedJNIUnsafeGetArrayIndexScaleForComponentType },
  };

  for (auto& def : defs) {
    jni_handlers_.insert(std::make_pair(def.name, def.function));
  }
}

void UnstartedRuntimeInitialize() {
  CHECK(!tables_initialized_);

  UnstartedRuntimeInitializeInvokeHandlers();
  UnstartedRuntimeInitializeJNIHandlers();

  tables_initialized_ = true;
}

void UnstartedRuntimeInvoke(Thread* self, const DexFile::CodeItem* code_item,
                            ShadowFrame* shadow_frame, JValue* result, size_t arg_offset) {
  // In a runtime that's not started we intercept certain methods to avoid complicated dependency
  // problems in core libraries.
  CHECK(tables_initialized_);

  std::string name(PrettyMethod(shadow_frame->GetMethod()));
  const auto& iter = invoke_handlers_.find(name);
  if (iter != invoke_handlers_.end()) {
    (*iter->second)(self, shadow_frame, result, arg_offset);
  } else {
    // Not special, continue with regular interpreter execution.
    artInterpreterToInterpreterBridge(self, code_item, shadow_frame, result);
  }
}

// Hand select a number of methods to be run in a not yet started runtime without using JNI.
void UnstartedRuntimeJni(Thread* self, mirror::ArtMethod* method, mirror::Object* receiver,
                         uint32_t* args, JValue* result) {
  std::string name(PrettyMethod(method));
  const auto& iter = jni_handlers_.find(name);
  if (iter != jni_handlers_.end()) {
    (*iter->second)(self, method, receiver, args, result);
  } else if (Runtime::Current()->IsActiveTransaction()) {
    AbortTransactionF(self, "Attempt to invoke native method in non-started runtime: %s",
                      name.c_str());
  } else {
    LOG(FATAL) << "Calling native method " << PrettyMethod(method) << " in an unstarted "
        "non-transactional runtime";
  }
}

}  // namespace interpreter
}  // namespace art
