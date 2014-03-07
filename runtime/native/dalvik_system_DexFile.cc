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

#include "base/logging.h"
#include "class_linker.h"
#include "common_throws.h"
#include "dex_file-inl.h"
#include "gc/space/image_space.h"
#include "gc/space/space-inl.h"
#include "image.h"
#include "jni_internal.h"
#include "mirror/class_loader.h"
#include "mirror/object-inl.h"
#include "mirror/string.h"
#include "oat.h"
#include "os.h"
#include "runtime.h"
#include "scoped_thread_state_change.h"
#include "ScopedLocalRef.h"
#include "ScopedUtfChars.h"
#include "toStringArray.h"
#include "zip_archive.h"

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

static jlong DexFile_openDexFileNative(JNIEnv* env, jclass, jstring javaSourceName, jstring javaOutputName, jint) {
  ScopedUtfChars sourceName(env, javaSourceName);
  if (sourceName.c_str() == NULL) {
    return 0;
  }
  NullableScopedUtfChars outputName(env, javaOutputName);
  if (env->ExceptionCheck()) {
    return 0;
  }

  uint32_t dex_location_checksum;
  uint32_t* dex_location_checksum_pointer = &dex_location_checksum;
  std::string error_msg;
  if (!DexFile::GetChecksum(sourceName.c_str(), dex_location_checksum_pointer, &error_msg)) {
    dex_location_checksum_pointer = NULL;
  }

  ClassLinker* linker = Runtime::Current()->GetClassLinker();
  const DexFile* dex_file;
  if (outputName.c_str() == nullptr) {
    // FindOrCreateOatFileForDexLocation can tolerate a missing dex_location_checksum
    error_msg.clear();
    dex_file = linker->FindDexFileInOatFileFromDexLocation(sourceName.c_str(),
                                                           dex_location_checksum_pointer, &error_msg);
  } else {
    // FindOrCreateOatFileForDexLocation requires the dex_location_checksum
    if (dex_location_checksum_pointer == NULL) {
      ScopedObjectAccess soa(env);
      DCHECK(!error_msg.empty());
      ThrowIOException("%s", error_msg.c_str());
      return 0;
    }
    dex_file = linker->FindOrCreateOatFileForDexLocation(sourceName.c_str(), dex_location_checksum,
                                                         outputName.c_str(), &error_msg);
  }
  if (dex_file == nullptr) {
    ScopedObjectAccess soa(env);
    CHECK(!error_msg.empty());
    ThrowIOException("%s", error_msg.c_str());
    return 0;
  }
  return static_cast<jlong>(reinterpret_cast<uintptr_t>(dex_file));
}

static const DexFile* toDexFile(jlong dex_file_address, JNIEnv* env) {
  const DexFile* dex_file = reinterpret_cast<const DexFile*>(static_cast<uintptr_t>(dex_file_address));
  if (UNLIKELY(dex_file == nullptr)) {
    ScopedObjectAccess soa(env);
    ThrowNullPointerException(NULL, "dex_file == null");
  }
  return dex_file;
}

static void DexFile_closeDexFile(JNIEnv* env, jclass, jlong cookie) {
  const DexFile* dex_file;
  dex_file = toDexFile(cookie, env);
  if (dex_file == nullptr) {
    return;
  }
  if (Runtime::Current()->GetClassLinker()->IsDexFileRegistered(*dex_file)) {
    return;
  }
  delete dex_file;
}

static jclass DexFile_defineClassNative(JNIEnv* env, jclass, jstring javaName, jobject javaLoader,
                                        jlong cookie) {
  const DexFile* dex_file = toDexFile(cookie, env);
  if (dex_file == NULL) {
    VLOG(class_linker) << "Failed to find dex_file";
    return NULL;
  }
  ScopedUtfChars class_name(env, javaName);
  if (class_name.c_str() == NULL) {
    VLOG(class_linker) << "Failed to find class_name";
    return NULL;
  }
  const std::string descriptor(DotToDescriptor(class_name.c_str()));
  const DexFile::ClassDef* dex_class_def = dex_file->FindClassDef(descriptor.c_str());
  if (dex_class_def == NULL) {
    VLOG(class_linker) << "Failed to find dex_class_def";
    return NULL;
  }
  ScopedObjectAccess soa(env);
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  class_linker->RegisterDexFile(*dex_file);
  SirtRef<mirror::ClassLoader> class_loader(soa.Self(), soa.Decode<mirror::ClassLoader*>(javaLoader));
  mirror::Class* result = class_linker->DefineClass(descriptor.c_str(), class_loader, *dex_file,
                                                    *dex_class_def);
  VLOG(class_linker) << "DexFile_defineClassNative returning " << result;
  return soa.AddLocalReference<jclass>(result);
}

static jobjectArray DexFile_getClassNameList(JNIEnv* env, jclass, jlong cookie) {
  const DexFile* dex_file;
  dex_file = toDexFile(cookie, env);
  if (dex_file == nullptr) {
    return nullptr;
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
  const bool kVerboseLogging = false;  // Spammy logging.
  const bool kDebugLogging = true;  // Logging useful for debugging.

  ScopedUtfChars filename(env, javaFilename);

  if ((filename.c_str() == nullptr) || !OS::FileExists(filename.c_str())) {
    LOG(ERROR) << "DexFile_isDexOptNeeded file '" << filename.c_str() << "' does not exist";
    ScopedLocalRef<jclass> fnfe(env, env->FindClass("java/io/FileNotFoundException"));
    const char* message = (filename.c_str() == nullptr) ? "<empty file name>" : filename.c_str();
    env->ThrowNew(fnfe.get(), message);
    return JNI_FALSE;
  }

  // Always treat elements of the bootclasspath as up-to-date.  The
  // fact that code is running at all means that this should be true.
  Runtime* runtime = Runtime::Current();
  ClassLinker* class_linker = runtime->GetClassLinker();
  const std::vector<const DexFile*>& boot_class_path = class_linker->GetBootClassPath();
  for (size_t i = 0; i < boot_class_path.size(); i++) {
    if (boot_class_path[i]->GetLocation() == filename.c_str()) {
      if (kVerboseLogging) {
        LOG(INFO) << "DexFile_isDexOptNeeded ignoring boot class path file: " << filename.c_str();
      }
      return JNI_FALSE;
    }
  }

  // Check if we have an odex file next to the dex file.
  std::string odex_filename(OatFile::DexFilenameToOdexFilename(filename.c_str()));
  std::string error_msg;
  UniquePtr<const OatFile> oat_file(OatFile::Open(odex_filename, odex_filename, NULL, false,
                                                  &error_msg));
  if (oat_file.get() == nullptr) {
    if (kVerboseLogging) {
      LOG(INFO) << "DexFile_isDexOptNeeded failed to open oat file '" << filename.c_str()
          << "': " << error_msg;
    }
    error_msg.clear();
  } else {
    const art::OatFile::OatDexFile* oat_dex_file = oat_file->GetOatDexFile(filename.c_str(), NULL,
                                                                           kDebugLogging);
    if (oat_dex_file != nullptr) {
      uint32_t location_checksum;
      // If its not possible to read the classes.dex assume up-to-date as we won't be able to
      // compile it anyway.
      if (!DexFile::GetChecksum(filename.c_str(), &location_checksum, &error_msg)) {
        if (kVerboseLogging) {
          LOG(INFO) << "DexFile_isDexOptNeeded ignoring precompiled stripped file: "
              << filename.c_str() << ": " << error_msg;
        }
        return JNI_FALSE;
      }
      if (ClassLinker::VerifyOatFileChecksums(oat_file.get(), filename.c_str(), location_checksum,
                                              &error_msg)) {
        if (kVerboseLogging) {
          LOG(INFO) << "DexFile_isDexOptNeeded precompiled file " << odex_filename
              << " has an up-to-date checksum compared to " << filename.c_str();
        }
        return JNI_FALSE;
      } else {
        if (kVerboseLogging) {
          LOG(INFO) << "DexFile_isDexOptNeeded found precompiled file " << odex_filename
              << " with an out-of-date checksum compared to " << filename.c_str()
              << ": " << error_msg;
        }
        error_msg.clear();
      }
    }
  }

  // Check if we have an oat file in the cache
  std::string cache_location(GetDalvikCacheFilenameOrDie(filename.c_str()));
  oat_file.reset(OatFile::Open(cache_location, filename.c_str(), NULL, false, &error_msg));
  if (oat_file.get() == nullptr) {
    if (kDebugLogging) {
      LOG(INFO) << "DexFile_isDexOptNeeded cache file " << cache_location
          << " does not exist for " << filename.c_str() << ": " << error_msg;
    }
    return JNI_TRUE;
  }

  for (const auto& space : runtime->GetHeap()->GetContinuousSpaces()) {
    if (space->IsImageSpace()) {
      // TODO: Ensure this works with multiple image spaces.
      const ImageHeader& image_header = space->AsImageSpace()->GetImageHeader();
      if (oat_file->GetOatHeader().GetImageFileLocationOatChecksum() !=
          image_header.GetOatChecksum()) {
        if (kDebugLogging) {
          ScopedObjectAccess soa(env);
          LOG(INFO) << "DexFile_isDexOptNeeded cache file " << cache_location
              << " has out-of-date oat checksum compared to "
              << image_header.GetImageRoot(ImageHeader::kOatLocation)->AsString()->ToModifiedUtf8();
        }
        return JNI_TRUE;
      }
      if (oat_file->GetOatHeader().GetImageFileLocationOatDataBegin()
          != reinterpret_cast<uintptr_t>(image_header.GetOatDataBegin())) {
        if (kDebugLogging) {
          ScopedObjectAccess soa(env);
          LOG(INFO) << "DexFile_isDexOptNeeded cache file " << cache_location
              << " has out-of-date oat begin compared to "
              << image_header.GetImageRoot(ImageHeader::kOatLocation)->AsString()->ToModifiedUtf8();
        }
        return JNI_TRUE;
      }
    }
  }

  uint32_t location_checksum;
  if (!DexFile::GetChecksum(filename.c_str(), &location_checksum, &error_msg)) {
    if (kDebugLogging) {
      LOG(ERROR) << "DexFile_isDexOptNeeded failed to compute checksum of " << filename.c_str()
            << " (error " << error_msg << ")";
    }
    return JNI_TRUE;
  }

  if (!ClassLinker::VerifyOatFileChecksums(oat_file.get(), filename.c_str(), location_checksum,
                                           &error_msg)) {
    if (kDebugLogging) {
      LOG(INFO) << "DexFile_isDexOptNeeded cache file " << cache_location
          << " has out-of-date checksum compared to " << filename.c_str()
          << " (error " << error_msg << ")";
    }
    return JNI_TRUE;
  }

  if (kVerboseLogging) {
    LOG(INFO) << "DexFile_isDexOptNeeded cache file " << cache_location
              << " is up-to-date for " << filename.c_str();
  }
  CHECK(error_msg.empty()) << error_msg;
  return JNI_FALSE;
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(DexFile, closeDexFile, "(J)V"),
  NATIVE_METHOD(DexFile, defineClassNative, "(Ljava/lang/String;Ljava/lang/ClassLoader;J)Ljava/lang/Class;"),
  NATIVE_METHOD(DexFile, getClassNameList, "(J)[Ljava/lang/String;"),
  NATIVE_METHOD(DexFile, isDexOptNeeded, "(Ljava/lang/String;)Z"),
  NATIVE_METHOD(DexFile, openDexFileNative, "(Ljava/lang/String;Ljava/lang/String;I)J"),
};

void register_dalvik_system_DexFile(JNIEnv* env) {
  REGISTER_NATIVE_METHODS("dalvik/system/DexFile");
}

}  // namespace art
