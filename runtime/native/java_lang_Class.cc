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

#include "class_linker.h"
#include "common_throws.h"
#include "dex_file-inl.h"
#include "jni_internal.h"
#include "nth_caller_visitor.h"
#include "mirror/art_field-inl.h"
#include "mirror/class-inl.h"
#include "mirror/class_loader.h"
#include "mirror/field-inl.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/string-inl.h"
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
  DCHECK(c != NULL);
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
  StackHandleScope<3> hs(self);
  auto h_ifields = hs.NewHandle(klass->GetIFields());
  auto h_sfields = hs.NewHandle(klass->GetSFields());
  const int32_t num_ifields = h_ifields.Get() != nullptr ? h_ifields->GetLength() : 0;
  const int32_t num_sfields = h_sfields.Get() != nullptr ? h_sfields->GetLength() : 0;
  int32_t array_size = num_ifields + num_sfields;
  if (public_only) {
    // Lets go subtract all the non public fields.
    for (int32_t i = 0; i < num_ifields; ++i) {
      if (!h_ifields->GetWithoutChecks(i)->IsPublic()) {
        --array_size;
      }
    }
    for (int32_t i = 0; i < num_sfields; ++i) {
      if (!h_sfields->GetWithoutChecks(i)->IsPublic()) {
        --array_size;
      }
    }
  }
  int32_t array_idx = 0;
  auto object_array = hs.NewHandle(mirror::ObjectArray<mirror::Field>::Alloc(
      self, mirror::Field::ArrayClass(), array_size));
  if (object_array.Get() == nullptr) {
    return nullptr;
  }
  for (int32_t i = 0; i < num_ifields; ++i) {
    auto* art_field = h_ifields->GetWithoutChecks(i);
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
  for (int32_t i = 0; i < num_sfields; ++i) {
    auto* art_field = h_sfields->GetWithoutChecks(i);
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
ALWAYS_INLINE static inline mirror::ArtField* FindFieldByName(
    Thread* self ATTRIBUTE_UNUSED, mirror::String* name,
    mirror::ObjectArray<mirror::ArtField>* fields)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  uint32_t low = 0;
  uint32_t high = fields->GetLength();
  const uint16_t* const data = name->GetCharArray()->GetData() + name->GetOffset();
  const size_t length = name->GetLength();
  while (low < high) {
    auto mid = (low + high) / 2;
    mirror::ArtField* const field = fields->GetWithoutChecks(mid);
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
    for (int32_t i = 0; i < fields->GetLength(); ++i) {
      CHECK_NE(fields->GetWithoutChecks(i)->GetName(), name->ToModifiedUtf8());
    }
  }
  return nullptr;
}

ALWAYS_INLINE static inline mirror::Field* GetDeclaredField(
    Thread* self, mirror::Class* c, mirror::String* name)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  auto* instance_fields = c->GetIFields();
  if (instance_fields != nullptr) {
    auto* art_field = FindFieldByName(self, name, instance_fields);
    if (art_field != nullptr) {
      return mirror::Field::CreateFromArtField(self, art_field, true);
    }
  }
  auto* static_fields = c->GetSFields();
  if (static_fields != nullptr) {
    auto* art_field = FindFieldByName(self, name, static_fields);
    if (art_field != nullptr) {
      return mirror::Field::CreateFromArtField(self, art_field, true);
    }
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
  if (name == nullptr) {
    ThrowNullPointerException("name == null");
    return nullptr;
  }
  auto* klass = DecodeClass(soa, javaThis);
  mirror::Field* result = GetDeclaredField(soa.Self(), klass, name_string);
  if (result == nullptr) {
    std::string name_str = name_string->ToModifiedUtf8();
    // We may have a pending exception if we failed to resolve.
    if (!soa.Self()->IsExceptionPending()) {
      soa.Self()->ThrowNewException("Ljava/lang/NoSuchFieldException;", name_str.c_str());
    }
    return nullptr;
  }
  return soa.AddLocalReference<jobject>(result);
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(Class, classForName, "!(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;"),
  NATIVE_METHOD(Class, getNameNative, "!()Ljava/lang/String;"),
  NATIVE_METHOD(Class, getProxyInterfaces, "!()[Ljava/lang/Class;"),
  NATIVE_METHOD(Class, getDeclaredFields, "!()[Ljava/lang/reflect/Field;"),
  NATIVE_METHOD(Class, getPublicDeclaredFields, "!()[Ljava/lang/reflect/Field;"),
  NATIVE_METHOD(Class, getDeclaredFieldsUnchecked, "!(Z)[Ljava/lang/reflect/Field;"),
  NATIVE_METHOD(Class, getDeclaredFieldInternal, "!(Ljava/lang/String;)Ljava/lang/reflect/Field;"),
  NATIVE_METHOD(Class, getDeclaredField, "!(Ljava/lang/String;)Ljava/lang/reflect/Field;"),
};

void register_java_lang_Class(JNIEnv* env) {
  REGISTER_NATIVE_METHODS("java/lang/Class");
}

}  // namespace art
