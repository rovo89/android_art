/*
 * Copyright (C) 2008 The Android Open Source Project
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

#include "java_lang_Class.h"

#include "art_field-inl.h"
#include "class_linker.h"
#include "common_throws.h"
#include "dex_file-inl.h"
#include "jni_internal.h"
#include "nth_caller_visitor.h"
#include "mirror/class-inl.h"
#include "mirror/class_loader.h"
#include "mirror/field-inl.h"
#include "mirror/method.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/string-inl.h"
#include "reflection.h"
#include "scoped_thread_state_change.h"
#include "scoped_fast_native_object_access.h"
#include "ScopedLocalRef.h"
#include "ScopedUtfChars.h"
#include "utf.h"
#include "well_known_classes.h"

namespace art {

ALWAYS_INLINE static inline mirror::Class* DecodeClass(
    const ScopedFastNativeObjectAccess& soa, jobject java_class)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::Class* c = soa.Decode<mirror::Class*>(java_class);
  DCHECK(c != nullptr);
  DCHECK(c->IsClass());
  // TODO: we could EnsureInitialized here, rather than on every reflective get/set or invoke .
  // For now, we conservatively preserve the old dalvik behavior. A quick "IsInitialized" check
  // every time probably doesn't make much difference to reflection performance anyway.
  return c;
}

// "name" is in "binary name" format, e.g. "dalvik.system.Debug$1".
static jclass Class_classForName(JNIEnv* env, jclass, jstring javaName, jboolean initialize,
                                 jobject javaLoader) {
  ScopedFastNativeObjectAccess soa(env);
  ScopedUtfChars name(env, javaName);
  if (name.c_str() == nullptr) {
    return nullptr;
  }

  // We need to validate and convert the name (from x.y.z to x/y/z).  This
  // is especially handy for array types, since we want to avoid
  // auto-generating bogus array classes.
  if (!IsValidBinaryClassName(name.c_str())) {
    soa.Self()->ThrowNewExceptionF("Ljava/lang/ClassNotFoundException;",
                                   "Invalid name: %s", name.c_str());
    return nullptr;
  }

  std::string descriptor(DotToDescriptor(name.c_str()));
  StackHandleScope<2> hs(soa.Self());
  Handle<mirror::ClassLoader> class_loader(hs.NewHandle(soa.Decode<mirror::ClassLoader*>(javaLoader)));
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  Handle<mirror::Class> c(
      hs.NewHandle(class_linker->FindClass(soa.Self(), descriptor.c_str(), class_loader)));
  if (c.Get() == nullptr) {
    ScopedLocalRef<jthrowable> cause(env, env->ExceptionOccurred());
    env->ExceptionClear();
    jthrowable cnfe = reinterpret_cast<jthrowable>(env->NewObject(WellKnownClasses::java_lang_ClassNotFoundException,
                                                                  WellKnownClasses::java_lang_ClassNotFoundException_init,
                                                                  javaName, cause.get()));
    if (cnfe != nullptr) {
      // Make sure allocation didn't fail with an OOME.
      env->Throw(cnfe);
    }
    return nullptr;
  }
  if (initialize) {
    class_linker->EnsureInitialized(soa.Self(), c, true, true);
  }
  return soa.AddLocalReference<jclass>(c.Get());
}

static jstring Class_getNameNative(JNIEnv* env, jobject javaThis) {
  ScopedFastNativeObjectAccess soa(env);
  StackHandleScope<1> hs(soa.Self());
  mirror::Class* const c = DecodeClass(soa, javaThis);
  return soa.AddLocalReference<jstring>(mirror::Class::ComputeName(hs.NewHandle(c)));
}

static jobjectArray Class_getProxyInterfaces(JNIEnv* env, jobject javaThis) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Class* c = DecodeClass(soa, javaThis);
  return soa.AddLocalReference<jobjectArray>(c->GetInterfaces()->Clone(soa.Self()));
}

static mirror::ObjectArray<mirror::Field>* GetDeclaredFields(
    Thread* self, mirror::Class* klass, bool public_only, bool force_resolve)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  StackHandleScope<1> hs(self);
  auto* ifields = klass->GetIFields();
  auto* sfields = klass->GetSFields();
  const auto num_ifields = klass->NumInstanceFields();
  const auto num_sfields = klass->NumStaticFields();
  size_t array_size = num_ifields + num_sfields;
  if (public_only) {
    // Lets go subtract all the non public fields.
    for (size_t i = 0; i < num_ifields; ++i) {
      if (!ifields[i].IsPublic()) {
        --array_size;
      }
    }
    for (size_t i = 0; i < num_sfields; ++i) {
      if (!sfields[i].IsPublic()) {
        --array_size;
      }
    }
  }
  size_t array_idx = 0;
  auto object_array = hs.NewHandle(mirror::ObjectArray<mirror::Field>::Alloc(
      self, mirror::Field::ArrayClass(), array_size));
  if (object_array.Get() == nullptr) {
    return nullptr;
  }
  for (size_t i = 0; i < num_ifields; ++i) {
    auto* art_field = &ifields[i];
    if (!public_only || art_field->IsPublic()) {
      auto* field = mirror::Field::CreateFromArtField(self, art_field, force_resolve);
      if (field == nullptr) {
        if (kIsDebugBuild) {
          self->AssertPendingException();
        }
        // Maybe null due to OOME or type resolving exception.
        return nullptr;
      }
      object_array->SetWithoutChecks<false>(array_idx++, field);
    }
  }
  for (size_t i = 0; i < num_sfields; ++i) {
    auto* art_field = &sfields[i];
    if (!public_only || art_field->IsPublic()) {
      auto* field = mirror::Field::CreateFromArtField(self, art_field, force_resolve);
      if (field == nullptr) {
        if (kIsDebugBuild) {
          self->AssertPendingException();
        }
        return nullptr;
      }
      object_array->SetWithoutChecks<false>(array_idx++, field);
    }
  }
  CHECK_EQ(array_idx, array_size);
  return object_array.Get();
}

static jobjectArray Class_getDeclaredFieldsUnchecked(JNIEnv* env, jobject javaThis,
                                                     jboolean publicOnly) {
  ScopedFastNativeObjectAccess soa(env);
  return soa.AddLocalReference<jobjectArray>(
      GetDeclaredFields(soa.Self(), DecodeClass(soa, javaThis), publicOnly != JNI_FALSE, false));
}

static jobjectArray Class_getDeclaredFields(JNIEnv* env, jobject javaThis) {
  ScopedFastNativeObjectAccess soa(env);
  return soa.AddLocalReference<jobjectArray>(
      GetDeclaredFields(soa.Self(), DecodeClass(soa, javaThis), false, true));
}

static jobjectArray Class_getPublicDeclaredFields(JNIEnv* env, jobject javaThis) {
  ScopedFastNativeObjectAccess soa(env);
  return soa.AddLocalReference<jobjectArray>(
      GetDeclaredFields(soa.Self(), DecodeClass(soa, javaThis), true, true));
}

// Performs a binary search through an array of fields, TODO: Is this fast enough if we don't use
// the dex cache for lookups? I think CompareModifiedUtf8ToUtf16AsCodePointValues should be fairly
// fast.
ALWAYS_INLINE static inline ArtField* FindFieldByName(
    Thread* self ATTRIBUTE_UNUSED, mirror::String* name, ArtField* fields, size_t num_fields)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  size_t low = 0;
  size_t high = num_fields;
  const uint16_t* const data = name->GetValue();
  const size_t length = name->GetLength();
  while (low < high) {
    auto mid = (low + high) / 2;
    ArtField* const field = &fields[mid];
    int result = CompareModifiedUtf8ToUtf16AsCodePointValues(field->GetName(), data, length);
    // Alternate approach, only a few % faster at the cost of more allocations.
    // int result = field->GetStringName(self, true)->CompareTo(name);
    if (result < 0) {
      low = mid + 1;
    } else if (result > 0) {
      high = mid;
    } else {
      return field;
    }
  }
  if (kIsDebugBuild) {
    for (size_t i = 0; i < num_fields; ++i) {
      CHECK_NE(fields[i].GetName(), name->ToModifiedUtf8());
    }
  }
  return nullptr;
}

ALWAYS_INLINE static inline mirror::Field* GetDeclaredField(
    Thread* self, mirror::Class* c, mirror::String* name)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  auto* instance_fields = c->GetIFields();
  auto* art_field = FindFieldByName(self, name, instance_fields, c->NumInstanceFields());
  if (art_field != nullptr) {
    return mirror::Field::CreateFromArtField(self, art_field, true);
  }
  auto* static_fields = c->GetSFields();
  art_field = FindFieldByName(self, name, static_fields, c->NumStaticFields());
  if (art_field != nullptr) {
    return mirror::Field::CreateFromArtField(self, art_field, true);
  }
  return nullptr;
}

static jobject Class_getDeclaredFieldInternal(JNIEnv* env, jobject javaThis, jstring name) {
  ScopedFastNativeObjectAccess soa(env);
  auto* name_string = soa.Decode<mirror::String*>(name);
  return soa.AddLocalReference<jobject>(
      GetDeclaredField(soa.Self(), DecodeClass(soa, javaThis), name_string));
}

static jobject Class_getDeclaredField(JNIEnv* env, jobject javaThis, jstring name) {
  ScopedFastNativeObjectAccess soa(env);
  auto* name_string = soa.Decode<mirror::String*>(name);
  if (name_string == nullptr) {
    ThrowNullPointerException("name == null");
    return nullptr;
  }
  auto* klass = DecodeClass(soa, javaThis);
  mirror::Field* result = GetDeclaredField(soa.Self(), klass, name_string);
  if (result == nullptr) {
    std::string name_str = name_string->ToModifiedUtf8();
    // We may have a pending exception if we failed to resolve.
    if (!soa.Self()->IsExceptionPending()) {
      ThrowNoSuchFieldException(DecodeClass(soa, javaThis), name_str.c_str());
    }
    return nullptr;
  }
  return soa.AddLocalReference<jobject>(result);
}

static jobject Class_getDeclaredConstructorInternal(
    JNIEnv* env, jobject javaThis, jobjectArray args) {
  ScopedFastNativeObjectAccess soa(env);
  auto* klass = DecodeClass(soa, javaThis);
  auto* params = soa.Decode<mirror::ObjectArray<mirror::Class>*>(args);
  StackHandleScope<1> hs(soa.Self());
  auto* declared_constructor = klass->GetDeclaredConstructor(soa.Self(), hs.NewHandle(params));
  if (declared_constructor != nullptr) {
    return soa.AddLocalReference<jobject>(
        mirror::Constructor::CreateFromArtMethod(soa.Self(), declared_constructor));
  }
  return nullptr;
}

static ALWAYS_INLINE inline bool MethodMatchesConstructor(ArtMethod* m, bool public_only)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  DCHECK(m != nullptr);
  return (!public_only || m->IsPublic()) && !m->IsStatic() && m->IsConstructor();
}

static jobjectArray Class_getDeclaredConstructorsInternal(
    JNIEnv* env, jobject javaThis, jboolean publicOnly) {
  ScopedFastNativeObjectAccess soa(env);
  StackHandleScope<2> hs(soa.Self());
  Handle<mirror::Class> h_klass = hs.NewHandle(DecodeClass(soa, javaThis));
  size_t constructor_count = 0;
  // Two pass approach for speed.
  for (auto& m : h_klass->GetDirectMethods(sizeof(void*))) {
    constructor_count += MethodMatchesConstructor(&m, publicOnly != JNI_FALSE) ? 1u : 0u;
  }
  auto h_constructors = hs.NewHandle(mirror::ObjectArray<mirror::Constructor>::Alloc(
      soa.Self(), mirror::Constructor::ArrayClass(), constructor_count));
  if (UNLIKELY(h_constructors.Get() == nullptr)) {
    soa.Self()->AssertPendingException();
    return nullptr;
  }
  constructor_count = 0;
  for (auto& m : h_klass->GetDirectMethods(sizeof(void*))) {
    if (MethodMatchesConstructor(&m, publicOnly != JNI_FALSE)) {
      auto* constructor = mirror::Constructor::CreateFromArtMethod(soa.Self(), &m);
      if (UNLIKELY(constructor == nullptr)) {
        soa.Self()->AssertPendingOOMException();
        return nullptr;
      }
      h_constructors->SetWithoutChecks<false>(constructor_count++, constructor);
    }
  }
  return soa.AddLocalReference<jobjectArray>(h_constructors.Get());
}

static jobject Class_getDeclaredMethodInternal(JNIEnv* env, jobject javaThis,
                                               jobject name, jobjectArray args) {
  // Covariant return types permit the class to define multiple
  // methods with the same name and parameter types. Prefer to
  // return a non-synthetic method in such situations. We may
  // still return a synthetic method to handle situations like
  // escalated visibility. We never return miranda methods that
  // were synthesized by the runtime.
  constexpr uint32_t kSkipModifiers = kAccMiranda | kAccSynthetic;
  ScopedFastNativeObjectAccess soa(env);
  StackHandleScope<3> hs(soa.Self());
  auto h_method_name = hs.NewHandle(soa.Decode<mirror::String*>(name));
  if (UNLIKELY(h_method_name.Get() == nullptr)) {
    ThrowNullPointerException("name == null");
    return nullptr;
  }
  auto h_args = hs.NewHandle(soa.Decode<mirror::ObjectArray<mirror::Class>*>(args));
  Handle<mirror::Class> h_klass = hs.NewHandle(DecodeClass(soa, javaThis));
  ArtMethod* result = nullptr;
  for (auto& m : h_klass->GetVirtualMethods(sizeof(void*))) {
    auto* np_method = m.GetInterfaceMethodIfProxy(sizeof(void*));
    // May cause thread suspension.
    mirror::String* np_name = np_method->GetNameAsString(soa.Self());
    if (!np_name->Equals(h_method_name.Get()) || !np_method->EqualParameters(h_args)) {
      if (UNLIKELY(soa.Self()->IsExceptionPending())) {
        return nullptr;
      }
      continue;
    }
    auto modifiers = m.GetAccessFlags();
    if ((modifiers & kSkipModifiers) == 0) {
      return soa.AddLocalReference<jobject>(mirror::Method::CreateFromArtMethod(soa.Self(), &m));
    }
    if ((modifiers & kAccMiranda) == 0) {
      result = &m;  // Remember as potential result if it's not a miranda method.
    }
  }
  if (result == nullptr) {
    for (auto& m : h_klass->GetDirectMethods(sizeof(void*))) {
      auto modifiers = m.GetAccessFlags();
      if ((modifiers & kAccConstructor) != 0) {
        continue;
      }
      auto* np_method = m.GetInterfaceMethodIfProxy(sizeof(void*));
      // May cause thread suspension.
      mirror::String* np_name = np_method->GetNameAsString(soa.Self());
      if (np_name == nullptr) {
        soa.Self()->AssertPendingException();
        return nullptr;
      }
      if (!np_name->Equals(h_method_name.Get()) || !np_method->EqualParameters(h_args)) {
        if (UNLIKELY(soa.Self()->IsExceptionPending())) {
          return nullptr;
        }
        continue;
      }
      if ((modifiers & kSkipModifiers) == 0) {
        return soa.AddLocalReference<jobject>(mirror::Method::CreateFromArtMethod(soa.Self(), &m));
      }
      // Direct methods cannot be miranda methods, so this potential result must be synthetic.
      result = &m;
    }
  }
  return result != nullptr ?
      soa.AddLocalReference<jobject>(mirror::Method::CreateFromArtMethod(soa.Self(), result)) :
      nullptr;
}

static jobjectArray Class_getDeclaredMethodsUnchecked(JNIEnv* env, jobject javaThis,
                                                      jboolean publicOnly) {
  ScopedFastNativeObjectAccess soa(env);
  StackHandleScope<2> hs(soa.Self());
  Handle<mirror::Class> klass = hs.NewHandle(DecodeClass(soa, javaThis));
  size_t num_methods = 0;
  for (auto& m : klass->GetVirtualMethods(sizeof(void*))) {
    auto modifiers = m.GetAccessFlags();
    if ((publicOnly == JNI_FALSE || (modifiers & kAccPublic) != 0) &&
        (modifiers & kAccMiranda) == 0) {
      ++num_methods;
    }
  }
  for (auto& m : klass->GetDirectMethods(sizeof(void*))) {
    auto modifiers = m.GetAccessFlags();
    // Add non-constructor direct/static methods.
    if ((publicOnly == JNI_FALSE || (modifiers & kAccPublic) != 0) &&
        (modifiers & kAccConstructor) == 0) {
      ++num_methods;
    }
  }
  auto ret = hs.NewHandle(mirror::ObjectArray<mirror::Method>::Alloc(
      soa.Self(), mirror::Method::ArrayClass(), num_methods));
  num_methods = 0;
  for (auto& m : klass->GetVirtualMethods(sizeof(void*))) {
    auto modifiers = m.GetAccessFlags();
    if ((publicOnly == JNI_FALSE || (modifiers & kAccPublic) != 0) &&
        (modifiers & kAccMiranda) == 0) {
      auto* method = mirror::Method::CreateFromArtMethod(soa.Self(), &m);
      if (method == nullptr) {
        soa.Self()->AssertPendingException();
        return nullptr;
      }
      ret->SetWithoutChecks<false>(num_methods++, method);
    }
  }
  for (auto& m : klass->GetDirectMethods(sizeof(void*))) {
    auto modifiers = m.GetAccessFlags();
    // Add non-constructor direct/static methods.
    if ((publicOnly == JNI_FALSE || (modifiers & kAccPublic) != 0) &&
        (modifiers & kAccConstructor) == 0) {
      auto* method = mirror::Method::CreateFromArtMethod(soa.Self(), &m);
      if (method == nullptr) {
        soa.Self()->AssertPendingException();
        return nullptr;
      }
      ret->SetWithoutChecks<false>(num_methods++, method);
    }
  }
  return soa.AddLocalReference<jobjectArray>(ret.Get());
}

static jobject Class_newInstance(JNIEnv* env, jobject javaThis) {
  ScopedFastNativeObjectAccess soa(env);
  StackHandleScope<4> hs(soa.Self());
  Handle<mirror::Class> klass = hs.NewHandle(DecodeClass(soa, javaThis));
  if (UNLIKELY(klass->GetPrimitiveType() != 0 || klass->IsInterface() || klass->IsArrayClass() ||
               klass->IsAbstract())) {
    soa.Self()->ThrowNewExceptionF("Ljava/lang/InstantiationException;",
                                   "%s cannot be instantiated", PrettyClass(klass.Get()).c_str());
    return nullptr;
  }
  auto caller = hs.NewHandle<mirror::Class>(nullptr);
  // Verify that we can access the class.
  if (!klass->IsPublic()) {
    caller.Assign(GetCallingClass(soa.Self(), 1));
    if (caller.Get() != nullptr && !caller->CanAccess(klass.Get())) {
      soa.Self()->ThrowNewExceptionF(
          "Ljava/lang/IllegalAccessException;", "%s is not accessible from %s",
          PrettyClass(klass.Get()).c_str(), PrettyClass(caller.Get()).c_str());
      return nullptr;
    }
  }
  auto* constructor = klass->GetDeclaredConstructor(
      soa.Self(), NullHandle<mirror::ObjectArray<mirror::Class>>());
  if (UNLIKELY(constructor == nullptr)) {
    soa.Self()->ThrowNewExceptionF("Ljava/lang/InstantiationException;",
                                   "%s has no zero argument constructor",
                                   PrettyClass(klass.Get()).c_str());
    return nullptr;
  }
  // Invoke the string allocator to return an empty string for the string class.
  if (klass->IsStringClass()) {
    gc::AllocatorType allocator_type = Runtime::Current()->GetHeap()->GetCurrentAllocator();
    mirror::SetStringCountVisitor visitor(0);
    mirror::Object* obj = mirror::String::Alloc<true>(soa.Self(), 0, allocator_type, visitor);
    if (UNLIKELY(soa.Self()->IsExceptionPending())) {
      return nullptr;
    } else {
      return soa.AddLocalReference<jobject>(obj);
    }
  }
  auto receiver = hs.NewHandle(klass->AllocObject(soa.Self()));
  if (UNLIKELY(receiver.Get() == nullptr)) {
    soa.Self()->AssertPendingOOMException();
    return nullptr;
  }
  // Verify that we can access the constructor.
  auto* declaring_class = constructor->GetDeclaringClass();
  if (!constructor->IsPublic()) {
    if (caller.Get() == nullptr) {
      caller.Assign(GetCallingClass(soa.Self(), 1));
    }
    if (UNLIKELY(caller.Get() != nullptr && !VerifyAccess(
        soa.Self(), receiver.Get(), declaring_class, constructor->GetAccessFlags(),
        caller.Get()))) {
      soa.Self()->ThrowNewExceptionF(
          "Ljava/lang/IllegalAccessException;", "%s is not accessible from %s",
          PrettyMethod(constructor).c_str(), PrettyClass(caller.Get()).c_str());
      return nullptr;
    }
  }
  // Ensure that we are initialized.
  if (UNLIKELY(!declaring_class->IsInitialized())) {
    if (!Runtime::Current()->GetClassLinker()->EnsureInitialized(
        soa.Self(), hs.NewHandle(declaring_class), true, true)) {
      soa.Self()->AssertPendingException();
      return nullptr;
    }
  }
  // Invoke the constructor.
  JValue result;
  uint32_t args[1] = { static_cast<uint32_t>(reinterpret_cast<uintptr_t>(receiver.Get())) };
  constructor->Invoke(soa.Self(), args, sizeof(args), &result, "V");
  if (UNLIKELY(soa.Self()->IsExceptionPending())) {
    return nullptr;
  }
  // Constructors are ()V methods, so we shouldn't touch the result of InvokeMethod.
  return soa.AddLocalReference<jobject>(receiver.Get());
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(Class, classForName,
                "!(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;"),
  NATIVE_METHOD(Class, getDeclaredConstructorInternal,
                "!([Ljava/lang/Class;)Ljava/lang/reflect/Constructor;"),
  NATIVE_METHOD(Class, getDeclaredConstructorsInternal, "!(Z)[Ljava/lang/reflect/Constructor;"),
  NATIVE_METHOD(Class, getDeclaredField, "!(Ljava/lang/String;)Ljava/lang/reflect/Field;"),
  NATIVE_METHOD(Class, getDeclaredFieldInternal, "!(Ljava/lang/String;)Ljava/lang/reflect/Field;"),
  NATIVE_METHOD(Class, getDeclaredFields, "!()[Ljava/lang/reflect/Field;"),
  NATIVE_METHOD(Class, getDeclaredFieldsUnchecked, "!(Z)[Ljava/lang/reflect/Field;"),
  NATIVE_METHOD(Class, getDeclaredMethodInternal,
                "!(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;"),
  NATIVE_METHOD(Class, getDeclaredMethodsUnchecked,
                  "!(Z)[Ljava/lang/reflect/Method;"),
  NATIVE_METHOD(Class, getNameNative, "!()Ljava/lang/String;"),
  NATIVE_METHOD(Class, getProxyInterfaces, "!()[Ljava/lang/Class;"),
  NATIVE_METHOD(Class, getPublicDeclaredFields, "!()[Ljava/lang/reflect/Field;"),
  NATIVE_METHOD(Class, newInstance, "!()Ljava/lang/Object;"),
};

void register_java_lang_Class(JNIEnv* env) {
  REGISTER_NATIVE_METHODS("java/lang/Class");
}

}  // namespace art
