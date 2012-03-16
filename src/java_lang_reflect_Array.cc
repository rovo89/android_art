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
#include "object_utils.h"

#include "JniConstants.h" // Last to avoid problems with LOG redefinition.

namespace art {

// Recursively create an array with multiple dimensions.  Elements may be
// Objects or primitive types.
static Array* CreateMultiArray(Class* array_class, int current_dimension, IntArray* dimensions) {
  int32_t array_length = dimensions->Get(current_dimension++);
  SirtRef<Array> new_array(Array::Alloc(array_class, array_length));
  if (new_array.get() == NULL) {
    CHECK(Thread::Current()->IsExceptionPending());
    return NULL;
  }
  if (current_dimension == dimensions->GetLength()) {
    return new_array.get();
  }

  if (!array_class->GetComponentType()->IsArrayClass()) {
    // TODO: throw an exception, not relying on class_linker->FindClass to throw.
    // old code assumed this but if you recurse from "[Foo" to "Foo" to "oo",
    // you shouldn't assume there isn't a class "oo".
  }
  std::string sub_array_descriptor(ClassHelper(array_class).GetDescriptor() + 1);
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  Class* sub_array_class = class_linker->FindClass(sub_array_descriptor.c_str(),
                                                   array_class->GetClassLoader());
  if (sub_array_class == NULL) {
    CHECK(Thread::Current()->IsExceptionPending());
    return NULL;
  }
  DCHECK(sub_array_class->IsArrayClass());
  // Create a new sub-array in every element of the array.
  SirtRef<ObjectArray<Array> > object_array(new_array->AsObjectArray<Array>());
  for (int32_t i = 0; i < array_length; i++) {
    SirtRef<Array> sub_array(CreateMultiArray(sub_array_class, current_dimension, dimensions));
    if (sub_array.get() == NULL) {
      CHECK(Thread::Current()->IsExceptionPending());
      return NULL;
    }
    object_array->Set(i, sub_array.get());
  }
  return new_array.get();
}

// Create a multi-dimensional array of Objects or primitive types.
//
// We have to generate the names for X[], X[][], X[][][], and so on.  The
// easiest way to deal with that is to create the full name once and then
// subtract pieces off.  Besides, we want to start with the outermost
// piece and work our way in.
static jobject Array_createMultiArray(JNIEnv* env, jclass, jclass javaElementClass, jobject javaDimArray) {
  ScopedThreadStateChange tsc(Thread::Current(), Thread::kRunnable);
  DCHECK(javaElementClass != NULL);
  Class* element_class = Decode<Class*>(env, javaElementClass);
  DCHECK(element_class->IsClass());
  DCHECK(javaDimArray != NULL);
  Object* dimensions_obj = Decode<Object*>(env, javaDimArray);
  DCHECK(dimensions_obj->IsArrayInstance());
  DCHECK_STREQ(ClassHelper(dimensions_obj->GetClass()).GetDescriptor(), "[I");
  IntArray* dimensions_array = down_cast<IntArray*>(dimensions_obj);

  // Verify dimensions.
  //
  // The caller is responsible for verifying that "dimArray" is non-null
  // and has a length > 0 and <= 255.
  int num_dimensions = dimensions_array->GetLength();
  DCHECK_GT(num_dimensions, 0);
  DCHECK_LE(num_dimensions, 255);

  for (int i = 0; i < num_dimensions; i++) {
    int dimension = dimensions_array->Get(i);
    if (dimension < 0) {
      Thread::Current()->ThrowNewExceptionF("Ljava/lang/NegativeArraySizeException;",
          "Dimension %d: %d", i, dimension);
      return NULL;
    }
  }

  // Generate the full name of the array class.
  std::string descriptor(num_dimensions, '[');
  descriptor += ClassHelper(element_class).GetDescriptor();

  // Find/generate the array class.
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  Class* array_class = class_linker->FindClass(descriptor.c_str(), element_class->GetClassLoader());
  if (array_class == NULL) {
    CHECK(Thread::Current()->IsExceptionPending());
    return NULL;
  }
  // create the array
  Array* new_array = CreateMultiArray(array_class, 0, dimensions_array);
  if (new_array == NULL) {
    CHECK(Thread::Current()->IsExceptionPending());
    return NULL;
  }
  return AddLocalReference<jobject>(env, new_array);
}

static jobject Array_createObjectArray(JNIEnv* env, jclass, jclass javaElementClass, jint length) {
  ScopedThreadStateChange tsc(Thread::Current(), Thread::kRunnable);
  DCHECK(javaElementClass != NULL);
  Class* element_class = Decode<Class*>(env, javaElementClass);
  if (length < 0) {
    Thread::Current()->ThrowNewExceptionF("Ljava/lang/NegativeArraySizeException;", "%d", length);
    return NULL;
  }
  std::string descriptor;
  descriptor += '[';
  descriptor += ClassHelper(element_class).GetDescriptor();

  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  Class* array_class = class_linker->FindClass(descriptor.c_str(), element_class->GetClassLoader());
  if (array_class == NULL) {
    CHECK(Thread::Current()->IsExceptionPending());
    return NULL;
  }
  DCHECK(array_class->IsArrayClass());
  Array* new_array = Array::Alloc(array_class, length);
  if (new_array == NULL) {
    CHECK(Thread::Current()->IsExceptionPending());
    return NULL;
  }
  return AddLocalReference<jobject>(env, new_array);
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(Array, createMultiArray, "(Ljava/lang/Class;[I)Ljava/lang/Object;"),
  NATIVE_METHOD(Array, createObjectArray, "(Ljava/lang/Class;I)Ljava/lang/Object;"),
};

void register_java_lang_reflect_Array(JNIEnv* env) {
  jniRegisterNativeMethods(env, "java/lang/reflect/Array", gMethods, NELEM(gMethods));
}

}  // namespace art
