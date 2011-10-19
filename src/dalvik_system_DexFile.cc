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
#include "file.h"
#include "logging.h"
#include "os.h"
#include "runtime.h"
#include "space.h"
#include "zip_archive.h"
#include "toStringArray.h"
#include "ScopedUtfChars.h"

#include "JniConstants.h" // Last to avoid problems with LOG redefinition.

namespace art {

namespace {

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
    NullableScopedUtfChars(JNIEnv* env, jstring s)
    : mEnv(env), mString(s)
    {
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
  NullableScopedUtfChars outputName(env, javaOutputName);
  if (env->ExceptionCheck()) {
    return 0;
  }
  if (outputName.c_str() != NULL) {
    // Check that output name ends in .dex.
    if (!StringPiece(outputName.c_str()).ends_with(".dex")) {
      LOG(ERROR) << "Output filename '" << outputName.c_str() << "' does not end with .dex";
      return 0;
    }

    // Extract the dex file.
    UniquePtr<ZipArchive> zip_archive(ZipArchive::Open(sourceName.c_str()));
    if (zip_archive.get() == NULL) {
      LOG(ERROR) << "Failed to open " << sourceName.c_str() << " when looking for classes.dex";
      return 0;
    }

    UniquePtr<ZipEntry> zip_entry(zip_archive->Find(DexFile::kClassesDex));
    if (zip_entry.get() == NULL) {
      LOG(ERROR) << "Failed to find classes.dex within " << sourceName.c_str();
      return 0;
    }

    UniquePtr<File> file(OS::OpenFile(outputName.c_str(), true));
    bool success = zip_entry->Extract(*file);
    if (!success) {
      LOG(ERROR) << "Failed to extract classes.dex from " << sourceName.c_str();
      return 0;
    }

    // Fork and exec dex2oat.
    pid_t pid = fork();
    if (pid == 0) {
      std::string boot_image_option("--boot-image=");
      boot_image_option += Heap::GetSpaces()[0]->GetImageFilename();

      std::string dex_file_option("--dex-file=");
      dex_file_option += outputName.c_str();

      std::string oat_file_option("--oat=");
      oat_file_option += outputName.c_str();
      oat_file_option.erase(oat_file_option.size() - 3);
      oat_file_option += "oat";

      execl("/system/bin/dex2oatd",
            "/system/bin/dex2oatd",
            "-Xms64m",
            "-Xmx64m",
            boot_image_option.c_str(),
            dex_file_option.c_str(),
            oat_file_option.c_str(),
            NULL);

      PLOG(FATAL) << "execl(dex2oatd) failed";
      return 0;
    } else {
      // Wait for dex2oat to finish.
      int status;
      pid_t got_pid = TEMP_FAILURE_RETRY(waitpid(pid, &status, 0));
      if (got_pid != pid) {
        PLOG(ERROR) << "waitpid failed: wanted " << pid << ", got " << got_pid;
        return 0;
      }
      if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        LOG(ERROR) << "dex2oatd failed with dex-file=" << outputName.c_str();
        return 0;
      }
    }
  }

  const DexFile* dex_file;
  if (outputName.c_str() == NULL) {
    dex_file = DexFile::Open(sourceName.c_str(), "");
  } else {
    dex_file = DexFile::Open(outputName.c_str(), "");
  }
  if (dex_file == NULL) {
    jniThrowExceptionFmt(env, "java/io/IOException", "unable to open DEX file: %s",
                         sourceName.c_str());
    return 0;
  }
  return static_cast<jint>(reinterpret_cast<uintptr_t>(dex_file));
}

static const DexFile* toDexFile(JNIEnv* env, int dex_file_address) {
  const DexFile* dex_file = reinterpret_cast<const DexFile*>(static_cast<uintptr_t>(dex_file_address));
  if ((dex_file == NULL)) {
    jniThrowNullPointerException(env, "dex_file == null");
  }
  return dex_file;
}

void DexFile_closeDexFile(JNIEnv* env, jclass, jint cookie) {
  const DexFile* dex_file = toDexFile(env, cookie);
  if (dex_file == NULL) {
    return;
  }
  if (Runtime::Current()->GetClassLinker()->IsDexFileRegistered(*dex_file)) {
    return;
  }
  delete dex_file;
}

jclass DexFile_defineClass(JNIEnv* env, jclass, jstring javaName, jobject javaLoader, jint cookie) {
  const DexFile* dex_file = toDexFile(env, cookie);
  if (dex_file == NULL) {
    return NULL;
  }
  ScopedUtfChars class_name(env, javaName);
  if (class_name.c_str() == NULL) {
    return NULL;
  }
  const std::string descriptor = DotToDescriptor(class_name.c_str());
  const DexFile::ClassDef* dex_class_def = dex_file->FindClassDef(descriptor);
  if (dex_class_def == NULL) {
    return NULL;
  }

  Object* class_loader_object = Decode<Object*>(env, javaLoader);
  ClassLoader* class_loader = down_cast<ClassLoader*>(class_loader_object);
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  class_linker->RegisterDexFile(*dex_file);
  Class* result = class_linker->DefineClass(descriptor, class_loader, *dex_file, *dex_class_def);
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    return NULL;
  }
  return AddLocalReference<jclass>(env, result);
}

jobjectArray DexFile_getClassNameList(JNIEnv* env, jclass, jint cookie) {
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

jboolean DexFile_isDexOptNeeded(JNIEnv* env, jclass, jstring javaFilename) {
  ScopedUtfChars filename(env, javaFilename);
  if (filename.c_str() == NULL) {
    return JNI_TRUE;
  }

  if (!OS::FileExists(filename.c_str())) {
    jniThrowExceptionFmt(env, "java/io/FileNotFoundException", "%s", filename.c_str());
    return JNI_TRUE;
  }

  // Always treat elements of the bootclasspath as up-to-date.  The
  // fact that code is running at all means that this should be true.
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  const std::vector<const DexFile*>& boot_class_path = class_linker->GetBootClassPath();
  for (size_t i = 0; i < boot_class_path.size(); i++) {
    if (boot_class_path[i]->GetLocation() == filename.c_str()) {
      return JNI_FALSE;
    }
  }

  UniquePtr<const DexFile> dex_file(DexFile::Open(filename.c_str(), ""));
  if (dex_file.get() == NULL) {
    return JNI_TRUE;
  }

  const OatFile* oat_file = class_linker->FindOatFile(*dex_file.get());
  if (oat_file == NULL) {
    return JNI_TRUE;
  }
  const OatFile::OatDexFile* oat_dex_file = oat_file->GetOatDexFile(dex_file->GetLocation());
  if (oat_dex_file == NULL) {
    return JNI_TRUE;
  }
  if (oat_dex_file->GetDexFileChecksum() != dex_file->GetHeader().checksum_) {
    return JNI_TRUE;
  }
  return JNI_FALSE;
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(DexFile, closeDexFile, "(I)V"),
  NATIVE_METHOD(DexFile, defineClass, "(Ljava/lang/String;Ljava/lang/ClassLoader;I)Ljava/lang/Class;"),
  NATIVE_METHOD(DexFile, getClassNameList, "(I)[Ljava/lang/String;"),
  NATIVE_METHOD(DexFile, isDexOptNeeded, "(Ljava/lang/String;)Z"),
  NATIVE_METHOD(DexFile, openDexFile, "(Ljava/lang/String;Ljava/lang/String;I)I"),
};

}  // namespace

void register_dalvik_system_DexFile(JNIEnv* env) {
  jniRegisterNativeMethods(env, "dalvik/system/DexFile", gMethods, NELEM(gMethods));
}

}  // namespace art
