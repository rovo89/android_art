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
    JNIEnv* env, jclass, jstring filename, jclass cls_from_primary, jclass cls_from_secondary) {
  // Note:
  // Ideally we would get the dex list from the primary oat file.
  // e.g.
  //   oat_file = Runtime::Current()->GetOatFileManager().GetPrimaryOatFile();
  //   dex_files = OatFileAssistant::LoadDexFiles(*oat_file, dex_location.c_str());
  // However the ownership of the pointers is complicated since the primary file
  // already exists and the test crashed sporadically because some data changes under
  // our feet.
  // To simplify things get the dex files from the classes passed as arguments.
  const DexFile* dex_primary;
  const DexFile* dex_secondary;
  {
    ScopedObjectAccess soa(Thread::Current());
    dex_primary = soa.Decode<mirror::Class*>(cls_from_primary)->GetDexCache()->GetDexFile();
    dex_secondary = soa.Decode<mirror::Class*>(cls_from_secondary)->GetDexCache()->GetDexFile();
  }

  std::vector<const DexFile*> dex_files;
  dex_files.push_back(dex_primary);
  dex_files.push_back(dex_secondary);

  const char* filename_chars = env->GetStringUTFChars(filename, nullptr);
  ProfileCompilationInfo info(filename_chars);
  const char* result = info.Load(dex_files)
      ? info.DumpInfo(/*print_full_dex_location*/false).c_str()
      : nullptr;
  env->ReleaseStringUTFChars(filename, filename_chars);
  return env->NewStringUTF(result);
}

}  // namespace
}  // namespace art
