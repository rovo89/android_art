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
#include "object.h"

#include "JniConstants.h" // Last to avoid problems with LOG redefinition.

namespace art {

namespace {

jboolean Class_desiredAssertionStatus(JNIEnv* env, jobject javaThis) {
    return JNI_FALSE;
}

jobject Class_getClassLoader(JNIEnv* env, jclass, jobject javaClass) {
  Class* c = Decode<Class*>(env, javaClass);
  Object* result = reinterpret_cast<Object*>(const_cast<ClassLoader*>(c->GetClassLoader()));
  return AddLocalReference<jobject>(env, result);
}

jclass Class_getComponentType(JNIEnv* env, jobject javaThis) {
  return AddLocalReference<jclass>(env, Decode<Class*>(env, javaThis)->GetComponentType());
}

jobjectArray Class_getDeclaredClasses(JNIEnv* env, jclass java_lang_Class_class, jclass c, jboolean publicOnly) {
  UNIMPLEMENTED(WARNING) << "needs annotations";
  return env->NewObjectArray(0, java_lang_Class_class, NULL);
}

jobject Class_getDeclaredConstructorOrMethod(JNIEnv* env, jclass,
                                             jclass jklass, jstring jname, jobjectArray jsignature) {
  Class* klass = Decode<Class*>(env, jklass);
  DCHECK(klass->IsClass());
  String* name = Decode<String*>(env, jname);
  DCHECK(name->IsString());
  Object* signature_obj = Decode<Object*>(env, jsignature);
  DCHECK(signature_obj->IsArrayInstance());
  // check that this is a Class[] by checkin that component type is Class
  // foo->GetClass()->GetClass() is an idiom for getting java.lang.Class from an arbitrary object
  DCHECK(signature_obj->GetClass()->GetComponentType() == signature_obj->GetClass()->GetClass());
  ObjectArray<Class>* signature = down_cast<ObjectArray<Class>*>(signature_obj);

  std::string name_string = name->ToModifiedUtf8();
  std::string signature_string;
  signature_string += "(";
  for (int i = 0; i < signature->GetLength(); i++) {
    Class* argument_class = signature->Get(0);
    if (argument_class == NULL) {
      UNIMPLEMENTED(FATAL) << "throw null pointer exception?";
    }
    signature_string += argument_class->GetDescriptor()->ToModifiedUtf8();
  }
  signature_string += ")";

  for (size_t i = 0; i < klass->NumVirtualMethods(); ++i) {
    Method* method = klass->GetVirtualMethod(i);
    if (!method->GetName()->Equals(name)) {
      continue;
    }
    std::string method_signature = method->GetSignature()->ToModifiedUtf8();
    if (!StringPiece(method_signature).starts_with(signature_string)) {
      continue;
    }
    return AddLocalReference<jobject>(env, method);
  }

  for (size_t i = 0; i < klass->NumVirtualMethods(); ++i) {
    Method* method = klass->GetVirtualMethod(i);
    if (!method->GetName()->Equals(name)) {
      continue;
    }
    std::string method_signature = method->GetSignature()->ToModifiedUtf8();
    if (!StringPiece(method_signature).starts_with(signature_string)) {
      continue;
    }
    return AddLocalReference<jobject>(env, method);
  }

  return NULL;
}

jobject Class_getDeclaredField(JNIEnv* env, jclass, jclass jklass, jobject jname) {
  Class* klass = Decode<Class*>(env, jklass);
  DCHECK(klass->IsClass());
  String* name = Decode<String*>(env, jname);
  DCHECK(name->IsString());

  for (size_t i = 0; i < klass->NumVirtualMethods(); ++i) {
    Field* f = klass->GetInstanceField(i);
    if (f->GetName()->Equals(name)) {
      return AddLocalReference<jclass>(env, f);
    }
  }
  for (size_t i = 0; i < klass->NumStaticFields(); ++i) {
    Field* f = klass->GetStaticField(i);
    if (f->GetName()->Equals(name)) {
      return AddLocalReference<jclass>(env, f);
    }
  }
  return NULL;
}

jclass Class_getDeclaringClass(JNIEnv* env, jobject javaThis) {
  UNIMPLEMENTED(WARNING) << "needs annotations";
  return NULL;
}

jobject Class_getEnclosingConstructor(JNIEnv* env, jobject javaThis) {
  UNIMPLEMENTED(WARNING) << "needs annotations";
  return NULL;
}

jobject Class_getEnclosingMethod(JNIEnv* env, jobject javaThis) {
  UNIMPLEMENTED(WARNING) << "needs annotations";
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

jboolean Class_isAnonymousClass(JNIEnv* env, jobject javaThis) {
  UNIMPLEMENTED(WARNING) << "needs annotations";
  return JNI_FALSE;
}

jboolean Class_isInterface(JNIEnv* env, jobject javaThis) {
  Class* c = Decode<Class*>(env, javaThis);
  return c->IsInterface();
}

jboolean Class_isPrimitive(JNIEnv* env, jobject javaThis) {
  Class* c = Decode<Class*>(env, javaThis);
  return c->IsPrimitive();
}

static JNINativeMethod gMethods[] = {
  //NATIVE_METHOD(Class, classForName, "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;"),
  NATIVE_METHOD(Class, desiredAssertionStatus, "()Z"),
  NATIVE_METHOD(Class, getClassLoader, "(Ljava/lang/Class;)Ljava/lang/ClassLoader;"),
  NATIVE_METHOD(Class, getComponentType, "()Ljava/lang/Class;"),
  //NATIVE_METHOD(Class, getDeclaredAnnotation, "(Ljava/lang/Class;)Ljava/lang/annotation/Annotation;"),
  //NATIVE_METHOD(Class, getDeclaredAnnotations, "()[Ljava/lang/annotation/Annotation;"),
  NATIVE_METHOD(Class, getDeclaredClasses, "(Ljava/lang/Class;Z)[Ljava/lang/Class;"),
  NATIVE_METHOD(Class, getDeclaredConstructorOrMethod, "(Ljava/lang/Class;Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Member;"),
  //NATIVE_METHOD(Class, getDeclaredConstructors, "(Ljava/lang/Class;Z)[Ljava/lang/reflect/Constructor;"),
  NATIVE_METHOD(Class, getDeclaredField, "(Ljava/lang/Class;Ljava/lang/String;)Ljava/lang/reflect/Field;"),
  //NATIVE_METHOD(Class, getDeclaredFields, "(Ljava/lang/Class;Z)[Ljava/lang/reflect/Field;"),
  //NATIVE_METHOD(Class, getDeclaredMethods, "(Ljava/lang/Class;Z)[Ljava/lang/reflect/Method;"),
  NATIVE_METHOD(Class, getDeclaringClass, "()Ljava/lang/Class;"),
  //NATIVE_METHOD(Class, getEnclosingClass, "()Ljava/lang/Class;"),
  NATIVE_METHOD(Class, getEnclosingConstructor, "()Ljava/lang/reflect/Constructor;"),
  NATIVE_METHOD(Class, getEnclosingMethod, "()Ljava/lang/reflect/Method;"),
  //NATIVE_METHOD(Class, getInnerClassName, "()Ljava/lang/String;"),
  //NATIVE_METHOD(Class, getInterfaces, "()[Ljava/lang/Class;"),
  //NATIVE_METHOD(Class, getModifiers, "(Ljava/lang/Class;Z)I"),
  NATIVE_METHOD(Class, getNameNative, "()Ljava/lang/String;"),
  //NATIVE_METHOD(Class, getSignatureAnnotation, "()[Ljava/lang/Object;"),
  NATIVE_METHOD(Class, getSuperclass, "()Ljava/lang/Class;"),
  NATIVE_METHOD(Class, isAnonymousClass, "()Z"),
  //NATIVE_METHOD(Class, isAssignableFrom, "(Ljava/lang/Class;)Z"),
  //NATIVE_METHOD(Class, isDeclaredAnnotationPresent, "(Ljava/lang/Class;)Z"),
  //NATIVE_METHOD(Class, isInstance, "(Ljava/lang/Object;)Z"),
  NATIVE_METHOD(Class, isInterface, "()Z"),
  NATIVE_METHOD(Class, isPrimitive, "()Z"),
  //NATIVE_METHOD(Class, newInstanceImpl, "()Ljava/lang/Object;"),
};

}  // namespace

void register_java_lang_Class(JNIEnv* env) {
  jniRegisterNativeMethods(env, "java/lang/Class", gMethods, NELEM(gMethods));
}

}  // namespace art
