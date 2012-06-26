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

#include "class_linker.h"
#include "class_loader.h"
#include "jni_internal.h"
#include "nth_caller_visitor.h"
#include "object.h"
#include "object_utils.h"
#include "scoped_jni_thread_state.h"
#include "ScopedLocalRef.h"
#include "ScopedUtfChars.h"
#include "well_known_classes.h"

namespace art {

static Class* DecodeClass(const ScopedJniThreadState& ts, jobject java_class) {
  Class* c = ts.Decode<Class*>(java_class);
  DCHECK(c != NULL);
  DCHECK(c->IsClass());
  // TODO: we could EnsureInitialized here, rather than on every reflective get/set or invoke .
  // For now, we conservatively preserve the old dalvik behavior. A quick "IsInitialized" check
  // every time probably doesn't make much difference to reflection performance anyway.
  return c;
}

// "name" is in "binary name" format, e.g. "dalvik.system.Debug$1".
static jclass Class_classForName(JNIEnv* env, jclass, jstring javaName, jboolean initialize, jobject javaLoader) {
  ScopedJniThreadState ts(env);
  ScopedUtfChars name(env, javaName);
  if (name.c_str() == NULL) {
    return NULL;
  }

  // We need to validate and convert the name (from x.y.z to x/y/z).  This
  // is especially handy for array types, since we want to avoid
  // auto-generating bogus array classes.
  if (!IsValidBinaryClassName(name.c_str())) {
    Thread::Current()->ThrowNewExceptionF("Ljava/lang/ClassNotFoundException;",
        "Invalid name: %s", name.c_str());
    return NULL;
  }

  std::string descriptor(DotToDescriptor(name.c_str()));
  ClassLoader* class_loader = ts.Decode<ClassLoader*>(javaLoader);
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  Class* c = class_linker->FindClass(descriptor.c_str(), class_loader);
  if (c == NULL) {
    ScopedLocalRef<jthrowable> cause(env, env->ExceptionOccurred());
    env->ExceptionClear();
    jthrowable cnfe = reinterpret_cast<jthrowable>(env->NewObject(WellKnownClasses::java_lang_ClassNotFoundException,
                                                                  WellKnownClasses::java_lang_ClassNotFoundException_init,
                                                                  javaName, cause.get()));
    env->Throw(cnfe);
    return NULL;
  }
  if (initialize) {
    class_linker->EnsureInitialized(c, true, true);
  }
  return ts.AddLocalReference<jclass>(c);
}

static jint Class_getAnnotationDirectoryOffset(JNIEnv* env, jclass javaClass) {
  ScopedJniThreadState ts(env);
  Class* c = DecodeClass(ts, javaClass);
  if (c->IsPrimitive() || c->IsArrayClass() || c->IsProxyClass()) {
    return 0;  // primitive, array and proxy classes don't have class definitions
  }
  const DexFile::ClassDef* class_def = ClassHelper(c).GetClassDef();
  if (class_def == NULL) {
    return 0;  // not found
  } else {
    return class_def->annotations_off_;
  }
}

template<typename T>
static jobjectArray ToArray(const ScopedJniThreadState& ts, const char* array_class_name,
                            const std::vector<T*>& objects) {
  ScopedLocalRef<jclass> array_class(ts.Env(), ts.Env()->FindClass(array_class_name));
  jobjectArray result = ts.Env()->NewObjectArray(objects.size(), array_class.get(), NULL);
  for (size_t i = 0; i < objects.size(); ++i) {
    ScopedLocalRef<jobject> object(ts.Env(), ts.AddLocalReference<jobject>(objects[i]));
    ts.Env()->SetObjectArrayElement(result, i, object.get());
  }
  return result;
}

static bool IsVisibleConstructor(Method* m, bool public_only) {
  if (public_only && !m->IsPublic()) {
    return false;
  }
  if (m->IsStatic()) {
    return false;
  }
  return m->IsConstructor();
}

static jobjectArray Class_getDeclaredConstructors(JNIEnv* env, jclass javaClass, jboolean publicOnly) {
  ScopedJniThreadState ts(env);
  Class* c = DecodeClass(ts, javaClass);
  std::vector<Method*> constructors;
  for (size_t i = 0; i < c->NumDirectMethods(); ++i) {
    Method* m = c->GetDirectMethod(i);
    if (IsVisibleConstructor(m, publicOnly)) {
      constructors.push_back(m);
    }
  }

  return ToArray(ts, "java/lang/reflect/Constructor", constructors);
}

static bool IsVisibleField(Field* f, bool public_only) {
  if (public_only && !f->IsPublic()) {
    return false;
  }
  return true;
}

static jobjectArray Class_getDeclaredFields(JNIEnv* env, jclass javaClass, jboolean publicOnly) {
  ScopedJniThreadState ts(env);
  Class* c = DecodeClass(ts, javaClass);
  std::vector<Field*> fields;
  FieldHelper fh;
  for (size_t i = 0; i < c->NumInstanceFields(); ++i) {
    Field* f = c->GetInstanceField(i);
    fh.ChangeField(f);
    if (IsVisibleField(f, publicOnly)) {
      if (fh.GetType() == NULL) {
        DCHECK(env->ExceptionOccurred());
        return NULL;
      }
      fields.push_back(f);
    }
    if (env->ExceptionOccurred()) {
      return NULL;
    }
  }
  for (size_t i = 0; i < c->NumStaticFields(); ++i) {
    Field* f = c->GetStaticField(i);
    fh.ChangeField(f);
    if (IsVisibleField(f, publicOnly)) {
      if (fh.GetType() == NULL) {
        DCHECK(env->ExceptionOccurred());
        return NULL;
      }
      fields.push_back(f);
    }
    if (env->ExceptionOccurred()) {
      return NULL;
    }
  }

  return ToArray(ts, "java/lang/reflect/Field", fields);
}

static bool IsVisibleMethod(Method* m, bool public_only) {
  if (public_only && !m->IsPublic()) {
    return false;
  }
  if (m->IsConstructor()) {
    return false;
  }
  if (m->IsMiranda()) {
    return false;
  }
  return true;
}

static jobjectArray Class_getDeclaredMethods(JNIEnv* env, jclass javaClass, jboolean publicOnly) {
  ScopedJniThreadState ts(env);
  Class* c = DecodeClass(ts, javaClass);
  if (c == NULL) {
    return NULL;
  }

  std::vector<Method*> methods;
  MethodHelper mh;
  for (size_t i = 0; i < c->NumVirtualMethods(); ++i) {
    Method* m = c->GetVirtualMethod(i);
    mh.ChangeMethod(m);
    if (IsVisibleMethod(m, publicOnly)) {
      if (mh.GetReturnType() == NULL || mh.GetParameterTypes() == NULL) {
        DCHECK(env->ExceptionOccurred());
        return NULL;
      }
      methods.push_back(m);
    }
    if (env->ExceptionOccurred()) {
      return NULL;
    }
  }
  for (size_t i = 0; i < c->NumDirectMethods(); ++i) {
    Method* m = c->GetDirectMethod(i);
    mh.ChangeMethod(m);
    if (IsVisibleMethod(m, publicOnly)) {
      if (mh.GetReturnType() == NULL || mh.GetParameterTypes() == NULL) {
        DCHECK(env->ExceptionOccurred());
        return NULL;
      }
      methods.push_back(m);
    }
    if (env->ExceptionOccurred()) {
      return NULL;
    }
  }

  return ToArray(ts, "java/lang/reflect/Method", methods);
}

static jobject Class_getDex(JNIEnv* env, jobject javaClass) {
  ScopedJniThreadState ts(env);
  Class* c = DecodeClass(ts, javaClass);

  DexCache* dex_cache = c->GetDexCache();
  if (dex_cache == NULL) {
    return NULL;
  }

  return Runtime::Current()->GetClassLinker()->FindDexFile(dex_cache).GetDexObject(env);
}

static bool MethodMatches(MethodHelper* mh, const std::string& name, ObjectArray<Class>* arg_array) {
  if (name != mh->GetName()) {
    return false;
  }
  const DexFile::TypeList* m_type_list = mh->GetParameterTypeList();
  uint32_t m_type_list_size = m_type_list == NULL ? 0 : m_type_list->Size();
  uint32_t sig_length = arg_array->GetLength();

  if (m_type_list_size != sig_length) {
    return false;
  }

  for (uint32_t i = 0; i < sig_length; i++) {
    if (mh->GetClassFromTypeIdx(m_type_list->GetTypeItem(i).type_idx_) != arg_array->Get(i)) {
      return false;
    }
  }
  return true;
}

static Method* FindConstructorOrMethodInArray(ObjectArray<Method>* methods, const std::string& name,
                                              ObjectArray<Class>* arg_array) {
  if (methods == NULL) {
    return NULL;
  }
  Method* result = NULL;
  MethodHelper mh;
  for (int32_t i = 0; i < methods->GetLength(); ++i) {
    Method* method = methods->Get(i);
    mh.ChangeMethod(method);
    if (method->IsMiranda() || !MethodMatches(&mh, name, arg_array)) {
      continue;
    }

    result = method;

    // Covariant return types permit the class to define multiple
    // methods with the same name and parameter types. Prefer to return
    // a non-synthetic method in such situations. We may still return
    // a synthetic method to handle situations like escalated visibility.
    if (!method->IsSynthetic()) {
        break;
    }
  }
  return result;
}

static jobject Class_getDeclaredConstructorOrMethod(JNIEnv* env, jclass javaClass, jstring javaName,
                                                    jobjectArray javaArgs) {
  ScopedJniThreadState ts(env);
  Class* c = DecodeClass(ts, javaClass);
  std::string name(ts.Decode<String*>(javaName)->ToModifiedUtf8());
  ObjectArray<Class>* arg_array = ts.Decode<ObjectArray<Class>*>(javaArgs);

  Method* m = FindConstructorOrMethodInArray(c->GetDirectMethods(), name, arg_array);
  if (m == NULL) {
    m = FindConstructorOrMethodInArray(c->GetVirtualMethods(), name, arg_array);
  }

  if (m != NULL) {
    return ts.AddLocalReference<jobject>(m);
  } else {
    return NULL;
  }
}

static jobject Class_getDeclaredFieldNative(JNIEnv* env, jclass java_class, jobject jname) {
  ScopedJniThreadState ts(env);
  Class* c = DecodeClass(ts, java_class);
  String* name = ts.Decode<String*>(jname);
  DCHECK(name->GetClass()->IsStringClass());

  FieldHelper fh;
  for (size_t i = 0; i < c->NumInstanceFields(); ++i) {
    Field* f = c->GetInstanceField(i);
    fh.ChangeField(f);
    if (name->Equals(fh.GetName())) {
      if (fh.GetType() == NULL) {
        DCHECK(env->ExceptionOccurred());
        return NULL;
      }
      return ts.AddLocalReference<jclass>(f);
    }
  }
  for (size_t i = 0; i < c->NumStaticFields(); ++i) {
    Field* f = c->GetStaticField(i);
    fh.ChangeField(f);
    if (name->Equals(fh.GetName())) {
      if (fh.GetType() == NULL) {
        DCHECK(env->ExceptionOccurred());
        return NULL;
      }
      return ts.AddLocalReference<jclass>(f);
    }
  }
  return NULL;
}

static jstring Class_getNameNative(JNIEnv* env, jobject javaThis) {
  ScopedJniThreadState ts(env);
  Class* c = DecodeClass(ts, javaThis);
  return ts.AddLocalReference<jstring>(c->ComputeName());
}

static jobjectArray Class_getProxyInterfaces(JNIEnv* env, jobject javaThis) {
  ScopedJniThreadState ts(env);
  SynthesizedProxyClass* c = down_cast<SynthesizedProxyClass*>(DecodeClass(ts, javaThis));
  return ts.AddLocalReference<jobjectArray>(c->GetInterfaces()->Clone());
}

static jboolean Class_isAssignableFrom(JNIEnv* env, jobject javaLhs, jclass javaRhs) {
  ScopedJniThreadState ts(env);
  Class* lhs = DecodeClass(ts, javaLhs);
  Class* rhs = ts.Decode<Class*>(javaRhs); // Can be null.
  if (rhs == NULL) {
    ts.Self()->ThrowNewException("Ljava/lang/NullPointerException;", "class == null");
    return JNI_FALSE;
  }
  return lhs->IsAssignableFrom(rhs) ? JNI_TRUE : JNI_FALSE;
}

// Validate method/field access.
static bool CheckMemberAccess(const Class* access_from, Class* access_to, uint32_t member_flags) {
  // quick accept for public access */
  if (member_flags & kAccPublic) {
    return true;
  }

  // quick accept for access from same class
  if (access_from == access_to) {
    return true;
  }

  // quick reject for private access from another class
  if (member_flags & kAccPrivate) {
    return false;
  }

  // Semi-quick test for protected access from a sub-class, which may or
  // may not be in the same package.
  if (member_flags & kAccProtected) {
    if (access_from->IsSubClass(access_to)) {
        return true;
    }
  }

  // Allow protected and private access from other classes in the same
  return access_from->IsInSamePackage(access_to);
}

static jobject Class_newInstanceImpl(JNIEnv* env, jobject javaThis) {
  ScopedJniThreadState ts(env);
  Class* c = DecodeClass(ts, javaThis);
  if (c->IsPrimitive() || c->IsInterface() || c->IsArrayClass() || c->IsAbstract()) {
    ts.Self()->ThrowNewExceptionF("Ljava/lang/InstantiationException;",
        "Class %s can not be instantiated", PrettyDescriptor(ClassHelper(c).GetDescriptor()).c_str());
    return NULL;
  }

  if (!Runtime::Current()->GetClassLinker()->EnsureInitialized(c, true, true)) {
    return NULL;
  }

  Method* init = c->FindDeclaredDirectMethod("<init>", "()V");
  if (init == NULL) {
    ts.Self()->ThrowNewExceptionF("Ljava/lang/InstantiationException;",
        "Class %s has no default <init>()V constructor", PrettyDescriptor(ClassHelper(c).GetDescriptor()).c_str());
    return NULL;
  }

  // Verify access from the call site.
  //
  // First, make sure the method invoking Class.newInstance() has permission
  // to access the class.
  //
  // Second, make sure it has permission to invoke the constructor.  The
  // constructor must be public or, if the caller is in the same package,
  // have package scope.

  NthCallerVisitor visitor(ts.Self()->GetManagedStack(), ts.Self()->GetTraceStack(), 2);
  visitor.WalkStack();
  Class* caller_class = visitor.caller->GetDeclaringClass();

  ClassHelper caller_ch(caller_class);
  if (!caller_class->CanAccess(c)) {
    ts.Self()->ThrowNewExceptionF("Ljava/lang/IllegalAccessException;",
        "Class %s is not accessible from class %s",
        PrettyDescriptor(ClassHelper(c).GetDescriptor()).c_str(),
        PrettyDescriptor(caller_ch.GetDescriptor()).c_str());
    return NULL;
  }
  if (!CheckMemberAccess(caller_class, init->GetDeclaringClass(), init->GetAccessFlags())) {
    ts.Self()->ThrowNewExceptionF("Ljava/lang/IllegalAccessException;",
        "%s is not accessible from class %s",
        PrettyMethod(init).c_str(),
        PrettyDescriptor(caller_ch.GetDescriptor()).c_str());
    return NULL;
  }

  Object* new_obj = c->AllocObject();
  if (new_obj == NULL) {
    DCHECK(ts.Self()->IsExceptionPending());
    return NULL;
  }

  // invoke constructor; unlike reflection calls, we don't wrap exceptions
  jclass java_class = ts.AddLocalReference<jclass>(c);
  jmethodID mid = ts.EncodeMethod(init);
  return env->NewObject(java_class, mid);
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(Class, classForName, "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;"),
  NATIVE_METHOD(Class, getAnnotationDirectoryOffset, "()I"),
  NATIVE_METHOD(Class, getDeclaredConstructorOrMethod, "(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Member;"),
  NATIVE_METHOD(Class, getDeclaredConstructors, "(Z)[Ljava/lang/reflect/Constructor;"),
  NATIVE_METHOD(Class, getDeclaredFieldNative, "(Ljava/lang/String;)Ljava/lang/reflect/Field;"),
  NATIVE_METHOD(Class, getDeclaredFields, "(Z)[Ljava/lang/reflect/Field;"),
  NATIVE_METHOD(Class, getDeclaredMethods, "(Z)[Ljava/lang/reflect/Method;"),
  NATIVE_METHOD(Class, getDex, "()Lcom/android/dex/Dex;"),
  NATIVE_METHOD(Class, getNameNative, "()Ljava/lang/String;"),
  NATIVE_METHOD(Class, getProxyInterfaces, "()[Ljava/lang/Class;"),
  NATIVE_METHOD(Class, isAssignableFrom, "(Ljava/lang/Class;)Z"),
  NATIVE_METHOD(Class, newInstanceImpl, "()Ljava/lang/Object;"),
};

void register_java_lang_Class(JNIEnv* env) {
  REGISTER_NATIVE_METHODS("java/lang/Class");
}

}  // namespace art
