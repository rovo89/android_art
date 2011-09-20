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
#include "jni_internal.h"
#include "ScopedUtfChars.h"
#include "zip_archive.h"

#include "JniConstants.h" // Last to avoid problems with LOG redefinition.

namespace art {

namespace {

// Turn "java.lang.String" into "Ljava/lang/String;".
std::string ToDescriptor(const char* class_name) {
  std::string descriptor(class_name);
  std::replace(descriptor.begin(), descriptor.end(), '.', '/');
  if (descriptor.length() > 0 && descriptor[0] != '[') {
    descriptor = "L" + descriptor + ";";
  }
  return descriptor;
}

jclass VMClassLoader_findLoadedClass(JNIEnv* env, jclass, jobject javaLoader, jstring javaName) {
  ClassLoader* loader = Decode<ClassLoader*>(env, javaLoader);
  ScopedUtfChars name(env, javaName);
  if (name.c_str() == NULL) {
    return NULL;
  }

  std::string descriptor(ToDescriptor(name.c_str()));
  Class* c = Runtime::Current()->GetClassLinker()->LookupClass(descriptor.c_str(), loader);
  return AddLocalReference<jclass>(env, c);
}

jint VMClassLoader_getBootClassPathSize(JNIEnv* env, jclass) {
  return Runtime::Current()->GetClassLinker()->GetBootClassPath().size();
}

/*
 * Returns a string URL for a resource with the specified 'javaName' in
 * entry 'index' of the boot class path.
 *
 * We return a newly-allocated String in the following form:
 *
 *   jar:file://path!/name
 *
 * Where "path" is the bootstrap class path entry and "name" is the string
 * passed into this method.  "path" needs to be an absolute path (starting
 * with '/'); if it's not we'd need to make it absolute as part of forming
 * the URL string.
 */
jstring VMClassLoader_getBootClassPathResource(JNIEnv* env, jclass, jstring javaName, jint index) {
  ScopedUtfChars name(env, javaName);
  if (name.c_str() == NULL) {
    return NULL;
  }

  const std::vector<const DexFile*>& path = Runtime::Current()->GetClassLinker()->GetBootClassPath();
  if (index < 0 || size_t(index) >= path.size()) {
    return NULL;
  }
  const DexFile* dex_file = path[index];
  const std::string& location(dex_file->GetLocation());
  UniquePtr<ZipArchive> zip_archive(ZipArchive::Open(location));
  if (zip_archive.get() == NULL) {
    return NULL;
  }
  UniquePtr<ZipEntry> zip_entry(zip_archive->Find(name.c_str()));
  if (zip_entry.get() == NULL) {
    return NULL;
  }

  std::string url;
  StringAppendF(&url, "jar:file://%s!/%s", location.c_str(), name.c_str());
  return env->NewStringUTF(url.c_str());
}

/*
 * static Class loadClass(String name, boolean resolve)
 *     throws ClassNotFoundException
 *
 * Load class using bootstrap class loader.
 *
 * Return the Class object associated with the class or interface with
 * the specified name.
 *
 * "name" is in "binary name" format, e.g. "dalvik.system.Debug$1".
 */
jclass VMClassLoader_loadClass(JNIEnv* env, jclass, jstring javaName, jboolean resolve) {
  ScopedUtfChars name(env, javaName);
  if (name.c_str() == NULL) {
    return NULL;
  }

  /*
   * We need to validate and convert the name (from x.y.z to x/y/z).  This
   * is especially handy for array types, since we want to avoid
   * auto-generating bogus array classes.
   */
  if (!IsValidClassName(name.c_str(), true, true)) {
    Thread::Current()->ThrowNewException("Ljava/lang/ClassNotFoundException;",
        "Invalid name: %s", name.c_str());
    return NULL;
  }

  std::string descriptor(ToDescriptor(name.c_str()));
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  Class* c = class_linker->FindClass(descriptor.c_str(), NULL);
  if (resolve) {
    class_linker->EnsureInitialized(c, true);
  }
  return AddLocalReference<jclass>(env, c);
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(VMClassLoader, findLoadedClass, "(Ljava/lang/ClassLoader;Ljava/lang/String;)Ljava/lang/Class;"),
  NATIVE_METHOD(VMClassLoader, getBootClassPathResource, "(Ljava/lang/String;I)Ljava/lang/String;"),
  NATIVE_METHOD(VMClassLoader, getBootClassPathSize, "()I"),
  NATIVE_METHOD(VMClassLoader, loadClass, "(Ljava/lang/String;Z)Ljava/lang/Class;"),
};

}  // namespace

void register_java_lang_VMClassLoader(JNIEnv* env) {
  jniRegisterNativeMethods(env, "java/lang/VMClassLoader", gMethods, NELEM(gMethods));
}

}  // namespace art
