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
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/string-inl.h"
#include "nth_caller_visitor.h"
#include "thread.h"
#include "well_known_classes.h"

namespace art {
namespace interpreter {

static void AbortTransactionOrFail(Thread* self, const char* fmt, ...)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  va_list args;
  va_start(args, fmt);
  if (Runtime::Current()->IsActiveTransaction()) {
    AbortTransaction(self, fmt, args);
    va_end(args);
  } else {
    LOG(FATAL) << "Trying to abort, but not in transaction mode: " << StringPrintf(fmt, args);
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
// actually InternalError. This must not be wrapped, as it signals an initialization abort.
static void CheckExceptionGenerateClassNotFound(Thread* self)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  if (self->IsExceptionPending()) {
    // If it is not an InternalError, wrap it.
    std::string type(PrettyTypeOf(self->GetException()));
    if (type != "java.lang.InternalError") {
      self->ThrowNewWrappedException("Ljava/lang/ClassNotFoundException;",
                                     "ClassNotFoundException");
    }
  }
}

static void UnstartedClassForName(Thread* self, ShadowFrame* shadow_frame, JValue* result,
                                  size_t arg_offset)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::String* class_name = shadow_frame->GetVRegReference(arg_offset)->AsString();
  StackHandleScope<1> hs(self);
  Handle<mirror::String> h_class_name(hs.NewHandle(class_name));
  UnstartedRuntimeFindClass(self, h_class_name, NullHandle<mirror::ClassLoader>(), result,
                            "Class.forName", true, false);
  CheckExceptionGenerateClassNotFound(self);
}

static void UnstartedClassForNameLong(Thread* self, ShadowFrame* shadow_frame, JValue* result,
                                      size_t arg_offset)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::String* class_name = shadow_frame->GetVRegReference(arg_offset)->AsString();
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

static void UnstartedClassClassForName(Thread* self, ShadowFrame* shadow_frame, JValue* result,
                                       size_t arg_offset)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::String* class_name = shadow_frame->GetVRegReference(arg_offset)->AsString();
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

static void UnstartedClassNewInstance(Thread* self, ShadowFrame* shadow_frame, JValue* result,
                                      size_t arg_offset)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  StackHandleScope<3> hs(self);  // Class, constructor, object.
  mirror::Class* klass = shadow_frame->GetVRegReference(arg_offset)->AsClass();
  Handle<mirror::Class> h_klass(hs.NewHandle(klass));

  // Check that it's not null.
  if (h_klass.Get() == nullptr) {
    AbortTransactionOrFail(self, "Class reference is null for newInstance");
    return;
  }

  // If we're in a transaction, class must not be finalizable (it or a superclass has a finalizer).
  if (Runtime::Current()->IsActiveTransaction()) {
    if (h_klass.Get()->IsFinalizable()) {
      AbortTransaction(self, "Class for newInstance is finalizable: '%s'",
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

static void UnstartedClassGetDeclaredField(Thread* self, ShadowFrame* shadow_frame, JValue* result,
                                           size_t arg_offset)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  // Special managed code cut-out to allow field lookup in a un-started runtime that'd fail
  // going the reflective Dex way.
  mirror::Class* klass = shadow_frame->GetVRegReference(arg_offset)->AsClass();
  mirror::String* name2 = shadow_frame->GetVRegReference(arg_offset + 1)->AsString();
  mirror::ArtField* found = nullptr;
  mirror::ObjectArray<mirror::ArtField>* fields = klass->GetIFields();
  for (int32_t i = 0; i < fields->GetLength() && found == nullptr; ++i) {
    mirror::ArtField* f = fields->Get(i);
    if (name2->Equals(f->GetName())) {
      found = f;
    }
  }
  if (found == nullptr) {
    fields = klass->GetSFields();
    for (int32_t i = 0; i < fields->GetLength() && found == nullptr; ++i) {
      mirror::ArtField* f = fields->Get(i);
      if (name2->Equals(f->GetName())) {
        found = f;
      }
    }
  }
  if (found == nullptr) {
    AbortTransactionOrFail(self, "Failed to find field in Class.getDeclaredField in un-started "
                           " runtime. name=%s class=%s", name2->ToModifiedUtf8().c_str(),
                           PrettyDescriptor(klass).c_str());
    return;
  }
  // TODO: getDeclaredField calls GetType once the field is found to ensure a
  //       NoClassDefFoundError is thrown if the field's type cannot be resolved.
  mirror::Class* jlr_Field = self->DecodeJObject(
      WellKnownClasses::java_lang_reflect_Field)->AsClass();
  StackHandleScope<1> hs(self);
  Handle<mirror::Object> field(hs.NewHandle(jlr_Field->AllocNonMovableObject(self)));
  CHECK(field.Get() != nullptr);
  mirror::ArtMethod* c = jlr_Field->FindDeclaredDirectMethod("<init>",
                                                             "(Ljava/lang/reflect/ArtField;)V");
  uint32_t args[1];
  args[0] = StackReference<mirror::Object>::FromMirrorPtr(found).AsVRegValue();
  EnterInterpreterFromInvoke(self, c, field.Get(), args, nullptr);
  result->SetL(field.Get());
}

static void UnstartedVmClassLoaderFindLoadedClass(Thread* self, ShadowFrame* shadow_frame,
                                                  JValue* result, size_t arg_offset)
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

static void UnstartedSystemArraycopy(Thread* self, ShadowFrame* shadow_frame,
                                     JValue* result ATTRIBUTE_UNUSED, size_t arg_offset)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  // Special case array copying without initializing System.
  mirror::Class* ctype = shadow_frame->GetVRegReference(arg_offset)->GetClass()->GetComponentType();
  jint srcPos = shadow_frame->GetVReg(arg_offset + 1);
  jint dstPos = shadow_frame->GetVReg(arg_offset + 3);
  jint length = shadow_frame->GetVReg(arg_offset + 4);
  if (!ctype->IsPrimitive()) {
    mirror::ObjectArray<mirror::Object>* src = shadow_frame->GetVRegReference(arg_offset)->
        AsObjectArray<mirror::Object>();
    mirror::ObjectArray<mirror::Object>* dst = shadow_frame->GetVRegReference(arg_offset + 2)->
        AsObjectArray<mirror::Object>();
    for (jint i = 0; i < length; ++i) {
      dst->Set(dstPos + i, src->Get(srcPos + i));
    }
  } else if (ctype->IsPrimitiveChar()) {
    mirror::CharArray* src = shadow_frame->GetVRegReference(arg_offset)->AsCharArray();
    mirror::CharArray* dst = shadow_frame->GetVRegReference(arg_offset + 2)->AsCharArray();
    for (jint i = 0; i < length; ++i) {
      dst->Set(dstPos + i, src->Get(srcPos + i));
    }
  } else if (ctype->IsPrimitiveInt()) {
    mirror::IntArray* src = shadow_frame->GetVRegReference(arg_offset)->AsIntArray();
    mirror::IntArray* dst = shadow_frame->GetVRegReference(arg_offset + 2)->AsIntArray();
    for (jint i = 0; i < length; ++i) {
      dst->Set(dstPos + i, src->Get(srcPos + i));
    }
  } else {
    AbortTransactionOrFail(self, "Unimplemented System.arraycopy for type '%s'",
                           PrettyDescriptor(ctype).c_str());
  }
}

static void UnstartedThreadLocalGet(Thread* self, ShadowFrame* shadow_frame, JValue* result,
                                    size_t arg_offset ATTRIBUTE_UNUSED)
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

static void UnstartedMathCeil(Thread* self ATTRIBUTE_UNUSED, ShadowFrame* shadow_frame,
                              JValue* result, size_t arg_offset) {
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

static void UnstartedArtMethodGetMethodName(Thread* self, ShadowFrame* shadow_frame,
                                            JValue* result, size_t arg_offset)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::ArtMethod* method = shadow_frame->GetVRegReference(arg_offset)->AsArtMethod();
  result->SetL(method->GetNameAsString(self));
}

static void UnstartedObjectHashCode(Thread* self ATTRIBUTE_UNUSED, ShadowFrame* shadow_frame,
                                    JValue* result, size_t arg_offset)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::Object* obj = shadow_frame->GetVRegReference(arg_offset);
  result->SetI(obj->IdentityHashCode());
}

static void UnstartedDoubleDoubleToRawLongBits(Thread* self ATTRIBUTE_UNUSED,
                                               ShadowFrame* shadow_frame, JValue* result,
                                               size_t arg_offset) {
  double in = shadow_frame->GetVRegDouble(arg_offset);
  result->SetJ(bit_cast<int64_t>(in));
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

static void UnstartedJNIObjectInternalClone(Thread* self ATTRIBUTE_UNUSED,
                                            mirror::ArtMethod* method ATTRIBUTE_UNUSED,
                                            mirror::Object* receiver,
                                            uint32_t* args ATTRIBUTE_UNUSED,
                                            JValue* result)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  result->SetL(receiver->Clone(self));
}

static void UnstartedJNIObjectNotifyAll(Thread* self ATTRIBUTE_UNUSED,
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

typedef void(*InvokeHandler)(Thread* self, ShadowFrame* shadow_frame, JValue* result,
    size_t arg_size);

typedef void(*JNIHandler)(Thread* self, mirror::ArtMethod* method, mirror::Object* receiver,
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
    AbortTransaction(self, "Attempt to invoke native method in non-started runtime: %s",
                     name.c_str());
  } else {
    LOG(FATAL) << "Calling native method " << PrettyMethod(method) << " in an unstarted "
        "non-transactional runtime";
  }
}

}  // namespace interpreter
}  // namespace art
