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

#include "jni_internal.h"
#include "class_linker.h"
#include "class_loader.h"
#include "object.h"
#include "ScopedLocalRef.h"
#include "ScopedUtfChars.h"

#include "JniConstants.h" // Last to avoid problems with LOG redefinition.

namespace art {

namespace {

// "name" is in "binary name" format, e.g. "dalvik.system.Debug$1".
jclass Class_classForName(JNIEnv* env, jclass, jstring javaName, jboolean initialize, jobject javaLoader) {
  ScopedThreadStateChange tsc(Thread::Current(), Thread::kRunnable);
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
  Object* loader = Decode<Object*>(env, javaLoader);
  ClassLoader* class_loader = down_cast<ClassLoader*>(loader);
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  Class* c = class_linker->FindClass(descriptor.c_str(), class_loader);
  if (c == NULL) {
    // Convert NoClassDefFoundError to ClassNotFoundException
    // TODO: chain exceptions?
    DCHECK(env->ExceptionCheck());
    env->ExceptionClear();
    Thread::Current()->ThrowNewException("Ljava/lang/ClassNotFoundException;", name.c_str());
    return NULL;
  }
  if (initialize) {
    class_linker->EnsureInitialized(c, true);
  }
  return AddLocalReference<jclass>(env, c);
}

template<typename T>
jobjectArray ToArray(JNIEnv* env, const char* array_class_name, const std::vector<T*>& objects) {
  jclass array_class = env->FindClass(array_class_name);
  jobjectArray result = env->NewObjectArray(objects.size(), array_class, NULL);
  for (size_t i = 0; i < objects.size(); ++i) {
    ScopedLocalRef<jobject> object(env, AddLocalReference<jobject>(env, objects[i]));
    env->SetObjectArrayElement(result, i, object.get());
  }
  return result;
}

bool IsVisibleConstructor(Method* m, bool public_only) {
  if (public_only && !m->IsPublic()) {
    return false;
  }
  if (m->IsStatic()) {
    return false;
  }
  if (m->GetName()->CharAt(0) != '<') {
    return false;
  }
  m->InitJavaFields();
  return true;
}

jobjectArray Class_getDeclaredConstructors(JNIEnv* env, jclass, jclass javaClass, jboolean publicOnly) {
  Class* c = Decode<Class*>(env, javaClass);

  std::vector<Method*> constructors;
  for (size_t i = 0; i < c->NumDirectMethods(); ++i) {
    Method* m = c->GetDirectMethod(i);
    if (IsVisibleConstructor(m, publicOnly)) {
      constructors.push_back(m);
    }
  }

  return ToArray(env, "java/lang/reflect/Constructor", constructors);
}

bool IsVisibleField(Field* f, bool public_only) {
  if (public_only && !f->IsPublic()) {
    return false;
  }
  f->InitJavaFields();
  return true;
}

jobjectArray Class_getDeclaredFields(JNIEnv* env, jclass, jclass javaClass, jboolean publicOnly) {
  Class* c = Decode<Class*>(env, javaClass);

  std::vector<Field*> fields;
  for (size_t i = 0; i < c->NumInstanceFields(); ++i) {
    Field* f = c->GetInstanceField(i);
    if (IsVisibleField(f, publicOnly)) {
      fields.push_back(f);
    }
  }
  for (size_t i = 0; i < c->NumStaticFields(); ++i) {
    Field* f = c->GetStaticField(i);
    if (IsVisibleField(f, publicOnly)) {
      fields.push_back(f);
    }
  }

  return ToArray(env, "java/lang/reflect/Field", fields);
}

bool IsVisibleMethod(Method* m, bool public_only) {
  if (public_only && !m->IsPublic()) {
    return false;
  }
  if (m->GetName()->CharAt(0) == '<') {
    return false;
  }
  m->InitJavaFields();
  return true;
}

jobjectArray Class_getDeclaredMethods(JNIEnv* env, jclass, jclass javaClass, jboolean publicOnly) {
  Class* c = Decode<Class*>(env, javaClass);

  std::vector<Method*> methods;
  for (size_t i = 0; i < c->NumVirtualMethods(); ++i) {
    Method* m = c->GetVirtualMethod(i);
    if (IsVisibleMethod(m, publicOnly)) {
      methods.push_back(m);
    }
  }
  for (size_t i = 0; i < c->NumDirectMethods(); ++i) {
    Method* m = c->GetDirectMethod(i);
    if (IsVisibleMethod(m, publicOnly)) {
      methods.push_back(m);
    }
  }

  return ToArray(env, "java/lang/reflect/Method", methods);
}

jboolean Class_desiredAssertionStatus(JNIEnv* env, jobject javaThis) {
    return JNI_FALSE;
}

jobject Class_getDex(JNIEnv* env, jobject javaClass) {
  Class* c = Decode<Class*>(env, javaClass);

  DexCache* dex_cache = c->GetDexCache();
  if (dex_cache == NULL) {
    return NULL;
  }

  return Runtime::Current()->GetClassLinker()->FindDexFile(dex_cache).GetDexObject(env);
}

jint Class_getNonInnerClassModifiers(JNIEnv* env, jclass, jclass javaClass) {
  Class* c = Decode<Class*>(env, javaClass);
  return c->GetAccessFlags() & kAccJavaFlagsMask;
}

jobject Class_getClassLoader(JNIEnv* env, jclass, jobject javaClass) {
  Class* c = Decode<Class*>(env, javaClass);
  Object* result = reinterpret_cast<Object*>(const_cast<ClassLoader*>(c->GetClassLoader()));
  return AddLocalReference<jobject>(env, result);
}

jclass Class_getComponentType(JNIEnv* env, jobject javaThis) {
  return AddLocalReference<jclass>(env, Decode<Class*>(env, javaThis)->GetComponentType());
}

bool MethodMatches(Method* m, String* name, const std::string& signature) {
  if (!m->GetName()->Equals(name)) {
    return false;
  }
  std::string method_signature = m->GetSignature()->ToModifiedUtf8();
  if (!StringPiece(method_signature).starts_with(signature)) {
    return false;
  }
  m->InitJavaFields();
  return true;
}

jobject Class_getDeclaredConstructorOrMethod(JNIEnv* env, jclass,
    jclass javaClass, jstring javaName, jobjectArray javaSignature) {
  Class* c = Decode<Class*>(env, javaClass);
  String* name = Decode<String*>(env, javaName);
  ObjectArray<Class>* signature_array = Decode<ObjectArray<Class>*>(env, javaSignature);

  std::string signature;
  signature += "(";
  for (int i = 0; i < signature_array->GetLength(); i++) {
    signature += signature_array->Get(i)->GetDescriptor()->ToModifiedUtf8();
  }
  signature += ")";

  for (size_t i = 0; i < c->NumVirtualMethods(); ++i) {
    Method* m = c->GetVirtualMethod(i);
    if (MethodMatches(m, name, signature)) {
      return AddLocalReference<jobject>(env, m);
    }
  }

  for (size_t i = 0; i < c->NumDirectMethods(); ++i) {
    Method* m = c->GetDirectMethod(i);
    if (MethodMatches(m, name, signature)) {
      return AddLocalReference<jobject>(env, m);
    }
  }

  return NULL;
}

jobject Class_getDeclaredField(JNIEnv* env, jclass, jclass jklass, jobject jname) {
  Class* klass = Decode<Class*>(env, jklass);
  DCHECK(klass->IsClass());
  String* name = Decode<String*>(env, jname);
  DCHECK(name->GetClass()->IsStringClass());

  for (size_t i = 0; i < klass->NumInstanceFields(); ++i) {
    Field* f = klass->GetInstanceField(i);
    if (f->GetName()->Equals(name)) {
      f->InitJavaFields();
      return AddLocalReference<jclass>(env, f);
    }
  }
  for (size_t i = 0; i < klass->NumStaticFields(); ++i) {
    Field* f = klass->GetStaticField(i);
    if (f->GetName()->Equals(name)) {
      f->InitJavaFields();
      return AddLocalReference<jclass>(env, f);
    }
  }
  return NULL;
}

/*
 * private native String getNameNative()
 *
 * Return the class' name. The exact format is bizarre, but it's the specified
 * behavior: keywords for primitive types, regular "[I" form for primitive
 * arrays (so "int" but "[I"), and arrays of reference types written
 * between "L" and ";" but with dots rather than slashes (so "java.lang.String"
 * but "[Ljava.lang.String;"). Madness.
 */
jstring Class_getNameNative(JNIEnv* env, jobject javaThis) {
  Class* c = Decode<Class*>(env, javaThis);
  std::string descriptor(c->GetDescriptor()->ToModifiedUtf8());
  if ((descriptor[0] != 'L') && (descriptor[0] != '[')) {
    // The descriptor indicates that this is the class for
    // a primitive type; special-case the return value.
    const char* name = NULL;
    switch (descriptor[0]) {
    case 'Z': name = "boolean"; break;
    case 'B': name = "byte";    break;
    case 'C': name = "char";    break;
    case 'S': name = "short";   break;
    case 'I': name = "int";     break;
    case 'J': name = "long";    break;
    case 'F': name = "float";   break;
    case 'D': name = "double";  break;
    case 'V': name = "void";    break;
    default:
      LOG(FATAL) << "Unknown primitive type: " << PrintableChar(descriptor[0]);
    }
    return env->NewStringUTF(name);
  }

  // Convert the UTF-8 name to a java.lang.String. The
  // name must use '.' to separate package components.
  if (descriptor.size() > 2 && descriptor[0] == 'L' && descriptor[descriptor.size() - 1] == ';') {
    descriptor.erase(0, 1);
    descriptor.erase(descriptor.size() - 1);
  }
  std::replace(descriptor.begin(), descriptor.end(), '/', '.');
  return env->NewStringUTF(descriptor.c_str());
}

jclass Class_getSuperclass(JNIEnv* env, jobject javaThis) {
  Class* c = Decode<Class*>(env, javaThis);
  Class* result = c->GetSuperClass();
  return AddLocalReference<jclass>(env, result);
}

jboolean Class_isAssignableFrom(JNIEnv* env, jobject javaLhs, jclass javaRhs) {
  ScopedThreadStateChange tsc(Thread::Current(), Thread::kRunnable);
  Class* lhs = Decode<Class*>(env, javaLhs);
  Class* rhs = Decode<Class*>(env, javaRhs);
  if (rhs == NULL) {
    Thread::Current()->ThrowNewException("Ljava/lang/NullPointerException;", "class == null");
    return JNI_FALSE;
  }
  return lhs->IsAssignableFrom(rhs) ? JNI_TRUE : JNI_FALSE;
}

jboolean Class_isInstance(JNIEnv* env, jobject javaClass, jobject javaObject) {
  Class* c = Decode<Class*>(env, javaClass);
  Object* o = Decode<Object*>(env, javaObject);
  if (o == NULL) {
    return JNI_FALSE;
  }
  return o->InstanceOf(c) ? JNI_TRUE : JNI_FALSE;
}

jboolean Class_isInterface(JNIEnv* env, jobject javaThis) {
  Class* c = Decode<Class*>(env, javaThis);
  return c->IsInterface();
}

jboolean Class_isPrimitive(JNIEnv* env, jobject javaThis) {
  Class* c = Decode<Class*>(env, javaThis);
  return c->IsPrimitive();
}

// Validate method/field access.
bool CheckMemberAccess(const Class* access_from, const Class* access_to, uint32_t member_flags) {
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

jobject Class_newInstanceImpl(JNIEnv* env, jobject javaThis) {
  ScopedThreadStateChange tsc(Thread::Current(), Thread::kRunnable);
  Class* c = Decode<Class*>(env, javaThis);
  if (c->IsPrimitive() || c->IsInterface() || c->IsArrayClass() || c->IsAbstract()) {
    Thread::Current()->ThrowNewExceptionF("Ljava/lang/InstantiationException;",
        "Class %s can not be instantiated", PrettyDescriptor(c->GetDescriptor()).c_str());
    return NULL;
  }

  if (!Runtime::Current()->GetClassLinker()->EnsureInitialized(c, true)) {
    return NULL;
  }

  Method* init = c->FindDirectMethod("<init>", "()V");
  if (init == NULL) {
    Thread::Current()->ThrowNewExceptionF("Ljava/lang/InstantiationException;",
        "Class %s has no default <init>()V constructor", PrettyDescriptor(c->GetDescriptor()).c_str());
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
  // TODO: need SmartFrame (Thread::WalkStack-like iterator).
  Frame frame = Thread::Current()->GetTopOfStack();
  frame.Next();
  frame.Next();
  Method* caller_caller = frame.GetMethod();
  Class* caller_class = caller_caller->GetDeclaringClass();

  if (!caller_class->CanAccess(c)) {
    Thread::Current()->ThrowNewExceptionF("Ljava/lang/IllegalAccessException;",
        "Class %s is not accessible from class %s",
        PrettyDescriptor(c->GetDescriptor()).c_str(),
        PrettyDescriptor(caller_class->GetDescriptor()).c_str());
    return NULL;
  }
  if (!CheckMemberAccess(caller_class, init->GetDeclaringClass(), init->GetAccessFlags())) {
    Thread::Current()->ThrowNewExceptionF("Ljava/lang/IllegalAccessException;",
        "%s is not accessible from class %s",
        PrettyMethod(init).c_str(),
        PrettyDescriptor(caller_class->GetDescriptor()).c_str());
    return NULL;
  }

  Object* new_obj = c->AllocObject();
  if (new_obj == NULL) {
    DCHECK(Thread::Current()->IsExceptionPending());
    return NULL;
  }

  // invoke constructor; unlike reflection calls, we don't wrap exceptions
  jclass jklass = AddLocalReference<jclass>(env, c);
  jmethodID mid = EncodeMethod(init);
  return env->NewObject(jklass, mid);
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(Class, classForName, "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;"),
  NATIVE_METHOD(Class, desiredAssertionStatus, "()Z"),
  NATIVE_METHOD(Class, getClassLoader, "(Ljava/lang/Class;)Ljava/lang/ClassLoader;"),
  NATIVE_METHOD(Class, getComponentType, "()Ljava/lang/Class;"),
  NATIVE_METHOD(Class, getDeclaredConstructorOrMethod, "(Ljava/lang/Class;Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Member;"),
  NATIVE_METHOD(Class, getDeclaredConstructors, "(Ljava/lang/Class;Z)[Ljava/lang/reflect/Constructor;"),
  NATIVE_METHOD(Class, getDeclaredField, "(Ljava/lang/Class;Ljava/lang/String;)Ljava/lang/reflect/Field;"),
  NATIVE_METHOD(Class, getDeclaredFields, "(Ljava/lang/Class;Z)[Ljava/lang/reflect/Field;"),
  NATIVE_METHOD(Class, getDeclaredMethods, "(Ljava/lang/Class;Z)[Ljava/lang/reflect/Method;"),
  NATIVE_METHOD(Class, getDex, "()Lcom/android/dex/Dex;"),
  NATIVE_METHOD(Class, getNonInnerClassModifiers, "(Ljava/lang/Class;)I"),
  NATIVE_METHOD(Class, getNameNative, "()Ljava/lang/String;"),
  NATIVE_METHOD(Class, getSuperclass, "()Ljava/lang/Class;"),
  NATIVE_METHOD(Class, isAssignableFrom, "(Ljava/lang/Class;)Z"),
  NATIVE_METHOD(Class, isInstance, "(Ljava/lang/Object;)Z"),
  NATIVE_METHOD(Class, isInterface, "()Z"),
  NATIVE_METHOD(Class, isPrimitive, "()Z"),
  NATIVE_METHOD(Class, newInstanceImpl, "()Ljava/lang/Object;"),
};

}  // namespace

void register_java_lang_Class(JNIEnv* env) {
  jniRegisterNativeMethods(env, "java/lang/Class", gMethods, NELEM(gMethods));
}

}  // namespace art
