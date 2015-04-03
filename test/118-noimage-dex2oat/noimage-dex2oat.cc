/*
 * Copyright (C) 2014 The Android Open Source Project
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
#include "dex_file-inl.h"
#include "mirror/class-inl.h"
#include "scoped_thread_state_change.h"
#include "thread.h"

namespace art {

class NoDex2OatTest {
 public:
  static bool hasOat(jclass cls) {
    ScopedObjectAccess soa(Thread::Current());
    mirror::Class* klass = soa.Decode<mirror::Class*>(cls);
    const DexFile& dex_file = klass->GetDexFile();
    const OatFile::OatDexFile* oat_dex_file = dex_file.GetOatDexFile();
    return oat_dex_file != nullptr;
  }
};

extern "C" JNIEXPORT jboolean JNICALL Java_Main_hasImage(JNIEnv*, jclass) {
  return Runtime::Current()->GetHeap()->HasImageSpace();
}

extern "C" JNIEXPORT jboolean JNICALL Java_Main_isImageDex2OatEnabled(JNIEnv*, jclass) {
  return Runtime::Current()->IsImageDex2OatEnabled();
}

}  // namespace art
