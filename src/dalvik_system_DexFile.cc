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

#include <unistd.h>

#include "class_loader.h"
#include "class_linker.h"
#include "dex_file.h"
#include "image.h"
#include "logging.h"
#include "os.h"
#include "runtime.h"
#include "space.h"
#include "zip_archive.h"
#include "toStringArray.h"
#include "ScopedLocalRef.h"
#include "ScopedUtfChars.h"

#include "JniConstants.h" // Last to avoid problems with LOG redefinition.

namespace art {

// A smart pointer that provides read-only access to a Java string's UTF chars.
// Unlike libcore's NullableScopedUtfChars, this will *not* throw NullPointerException if
// passed a null jstring. The correct idiom is:
//
//   NullableScopedUtfChars name(env, javaName);
//   if (env->ExceptionCheck()) {
//       return NULL;
//   }
//   // ... use name.c_str()
//
// TODO: rewrite to get rid of this, or change ScopedUtfChars to offer this option.
class NullableScopedUtfChars {
 public:
  NullableScopedUtfChars(JNIEnv* env, jstring s) : mEnv(env), mString(s) {
    mUtfChars = (s != NULL) ? env->GetStringUTFChars(s, NULL) : NULL;
  }

  ~NullableScopedUtfChars() {
    if (mUtfChars) {
      mEnv->ReleaseStringUTFChars(mString, mUtfChars);
    }
  }

  const char* c_str() const {
    return mUtfChars;
  }

  size_t size() const {
    return strlen(mUtfChars);
  }

  // Element access.
  const char& operator[](size_t n) const {
    return mUtfChars[n];
  }

 private:
  JNIEnv* mEnv;
  jstring mString;
  const char* mUtfChars;

  // Disallow copy and assignment.
  NullableScopedUtfChars(const NullableScopedUtfChars&);
  void operator=(const NullableScopedUtfChars&);
};

static jint DexFile_openDexFile(JNIEnv* env, jclass, jstring javaSourceName, jstring javaOutputName, jint) {
  ScopedUtfChars sourceName(env, javaSourceName);
  if (sourceName.c_str() == NULL) {
    return 0;
  }
  std::string source(sourceName.c_str());
  NullableScopedUtfChars outputName(env, javaOutputName);
  if (env->ExceptionCheck()) {
    return 0;
  }
  const DexFile* dex_file;
  if (outputName.c_str() == NULL) {
    dex_file = Runtime::Current()->GetClassLinker()->FindDexFileInOatFileFromDexLocation(source);
  } else {
    std::string output(outputName.c_str());
    dex_file = Runtime::Current()->GetClassLinker()->FindOrCreateOatFileForDexLocation(source, output);
  }
  if (dex_file == NULL) {
    LOG(WARNING) << "Failed to open dex file: " << source;
    jniThrowExceptionFmt(env, "java/io/IOException", "unable to open dex file: %s",
                         source.c_str());
    return 0;
  }
  return static_cast<jint>(reinterpret_cast<uintptr_t>(dex_file));
}

static const DexFile* toDexFile(JNIEnv* env, int dex_file_address) {
  const DexFile* dex_file = reinterpret_cast<const DexFile*>(static_cast<uintptr_t>(dex_file_address));
  if (dex_file == NULL) {
    jniThrowNullPointerException(env, "dex_file == null");
  }
  return dex_file;
}

static void DexFile_closeDexFile(JNIEnv* env, jclass, jint cookie) {
  const DexFile* dex_file = toDexFile(env, cookie);
  if (dex_file == NULL) {
    return;
  }
  if (Runtime::Current()->GetClassLinker()->IsDexFileRegistered(*dex_file)) {
    return;
  }
  delete dex_file;
}

static jclass DexFile_defineClassNative(JNIEnv* env, jclass, jstring javaName, jobject javaLoader,
                                        jint cookie) {
  ScopedThreadStateChange tsc(Thread::Current(), Thread::kRunnable);
  const DexFile* dex_file = toDexFile(env, cookie);
  if (dex_file == NULL) {
    return NULL;
  }
  ScopedUtfChars class_name(env, javaName);
  if (class_name.c_str() == NULL) {
    return NULL;
  }
  const std::string descriptor(DotToDescriptor(class_name.c_str()));
  const DexFile::ClassDef* dex_class_def = dex_file->FindClassDef(descriptor);
  if (dex_class_def == NULL) {
    return NULL;
  }

  Object* class_loader_object = Decode<Object*>(env, javaLoader);
  ClassLoader* class_loader = down_cast<ClassLoader*>(class_loader_object);
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  class_linker->RegisterDexFile(*dex_file);
  Class* result = class_linker->DefineClass(descriptor, class_loader, *dex_file, *dex_class_def);
  return AddLocalReference<jclass>(env, result);
}

static jobjectArray DexFile_getClassNameList(JNIEnv* env, jclass, jint cookie) {
  const DexFile* dex_file = toDexFile(env, cookie);
  if (dex_file == NULL) {
    return NULL;
  }

  std::vector<std::string> class_names;
  for (size_t i = 0; i < dex_file->NumClassDefs(); ++i) {
    const DexFile::ClassDef& class_def = dex_file->GetClassDef(i);
    const char* descriptor = dex_file->GetClassDescriptor(class_def);
    class_names.push_back(DescriptorToDot(descriptor));
  }
  return toStringArray(env, class_names);
}

static jboolean DexFile_isDexOptNeeded(JNIEnv* env, jclass, jstring javaFilename) {
  bool debug_logging = false;

  ScopedUtfChars filename(env, javaFilename);
  if (filename.c_str() == NULL) {
    LOG(ERROR) << "DexFile_isDexOptNeeded null filename";
    return JNI_TRUE;
  }

  if (!OS::FileExists(filename.c_str())) {
    LOG(ERROR) << "DexFile_isDexOptNeeded file '" << filename.c_str() << "' does not exist";
    jniThrowExceptionFmt(env, "java/io/FileNotFoundException", "%s", filename.c_str());
    return JNI_TRUE;
  }

  // Always treat elements of the bootclasspath as up-to-date.  The
  // fact that code is running at all means that this should be true.
  Runtime* runtime = Runtime::Current();
  ClassLinker* class_linker = runtime->GetClassLinker();
  const std::vector<const DexFile*>& boot_class_path = class_linker->GetBootClassPath();
  for (size_t i = 0; i < boot_class_path.size(); i++) {
    if (boot_class_path[i]->GetLocation() == filename.c_str()) {
      if (debug_logging) {
        LOG(INFO) << "DexFile_isDexOptNeeded ignoring boot class path file: " << filename.c_str();
      }
      return JNI_FALSE;
    }
  }

  // If we have an oat file next to the dex file, assume up-to-date.
  // A user build looks like this, and it will have no classes.dex in
  // the input for checksum validation.
  std::string oat_filename(OatFile::DexFilenameToOatFilename(filename.c_str()));
  UniquePtr<const OatFile> oat_file(OatFile::Open(oat_filename, oat_filename, NULL));
  if (oat_file.get() != NULL && oat_file->GetOatDexFile(filename.c_str()) != NULL) {
    if (debug_logging) {
      LOG(INFO) << "DexFile_isDexOptNeeded ignoring precompiled file: " << filename.c_str();
    }
    return JNI_FALSE;
  }

  // Check if we have an oat file in the cache
  std::string cache_location(GetArtCacheFilenameOrDie(oat_filename));
  oat_file.reset(OatFile::Open(cache_location, oat_filename, NULL));
  if (oat_file.get() == NULL) {
    LOG(INFO) << "DexFile_isDexOptNeeded cache file " << cache_location
              << " does not exist for " << filename.c_str();
    return JNI_TRUE;
  }

  const ImageHeader& image_header = runtime->GetHeap()->GetImageSpace()->GetImageHeader();
  if (oat_file->GetOatHeader().GetImageFileLocationChecksum() != image_header.GetOatChecksum()) {
    LOG(INFO) << "DexFile_isDexOptNeeded cache file " << cache_location
              << " has out-of-date checksum compared to "
              << image_header.GetImageRoot(ImageHeader::kOatLocation)->AsString()->ToModifiedUtf8();
    return JNI_TRUE;
  }

  const OatFile::OatDexFile* oat_dex_file = oat_file->GetOatDexFile(filename.c_str());
  if (oat_dex_file == NULL) {
    LOG(ERROR) << "DexFile_isDexOptNeeded cache file " << cache_location
               << " does not contain contents for " << filename.c_str();
    std::vector<const OatFile::OatDexFile*> oat_dex_files = oat_file->GetOatDexFiles();
    for (size_t i = 0; i < oat_dex_files.size(); i++) {
      const OatFile::OatDexFile* oat_dex_file = oat_dex_files[i];
      LOG(ERROR) << "DexFile_isDexOptNeeded cache file " << cache_location
                 << " contains contents for " << oat_dex_file->GetDexFileLocation();
    }
    return JNI_TRUE;
  }

  uint32_t location_checksum;
  if (!DexFile::GetChecksum(filename.c_str(), location_checksum)) {
    LOG(ERROR) << "DexFile_isDexOptNeeded failed to compute checksum of " << filename.c_str();
    return JNI_TRUE;
  }

  if (location_checksum != oat_dex_file->GetDexFileLocationChecksum()) {
    LOG(INFO) << "DexFile_isDexOptNeeded cache file " << cache_location
              << " has out-of-date checksum compared to " << filename.c_str();
    return JNI_TRUE;
  }

  if (debug_logging) {
    LOG(INFO) << "DexFile_isDexOptNeeded cache file " << cache_location
              << " is up-to-date for " << filename.c_str();
  }
  return JNI_FALSE;
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(DexFile, closeDexFile, "(I)V"),
  NATIVE_METHOD(DexFile, defineClassNative, "(Ljava/lang/String;Ljava/lang/ClassLoader;I)Ljava/lang/Class;"),
  NATIVE_METHOD(DexFile, getClassNameList, "(I)[Ljava/lang/String;"),
  NATIVE_METHOD(DexFile, isDexOptNeeded, "(Ljava/lang/String;)Z"),
  NATIVE_METHOD(DexFile, openDexFile, "(Ljava/lang/String;Ljava/lang/String;I)I"),
};

void register_dalvik_system_DexFile(JNIEnv* env) {
  jniRegisterNativeMethods(env, "dalvik/system/DexFile", gMethods, NELEM(gMethods));
}

}  // namespace art
