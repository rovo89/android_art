/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include "dex_file.h"

#include "jit/offline_profiling_info.h"
#include "jni.h"
#include "mirror/class-inl.h"
#include "oat_file_assistant.h"
#include "oat_file_manager.h"
#include "scoped_thread_state_change.h"
#include "thread.h"

namespace art {
namespace {

extern "C" JNIEXPORT jstring JNICALL Java_Main_getProfileInfoDump(
      JNIEnv* env, jclass cls, jstring filename) {
  std::string dex_location;
  {
    ScopedObjectAccess soa(Thread::Current());
    dex_location = soa.Decode<mirror::Class*>(cls)->GetDexCache()->GetDexFile()->GetLocation();
  }
  const OatFile* oat_file = Runtime::Current()->GetOatFileManager().GetPrimaryOatFile();
  std::vector<std::unique_ptr<const DexFile>> dex_files =
      OatFileAssistant::LoadDexFiles(*oat_file, dex_location.c_str());
  const char* filename_chars = env->GetStringUTFChars(filename, nullptr);

  std::vector<const DexFile*> dex_files_raw;
  for (size_t i = 0; i < dex_files.size(); i++) {
    dex_files_raw.push_back(dex_files[i].get());
  }

  ProfileCompilationInfo info(filename_chars);

  std::string result = info.Load(dex_files_raw)
      ? info.DumpInfo(/*print_full_dex_location*/false)
      : "Could not load profile info";

  env->ReleaseStringUTFChars(filename, filename_chars);
  // Return the dump of the profile info. It will be compared against a golden value.
  return env->NewStringUTF(result.c_str());
}

}  // namespace
}  // namespace art
