/*
 * Copyright (C) 2013 The Android Open Source Project
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

#include <algorithm>
#include <utility>
#include "thread.h"
#include "thread-inl.h"
#include "base/mutex.h"
#include "base/mutex-inl.h"
#include "base/logging.h"
#include "driver/compiler_driver.h"
#include "dex/quick/arm/arm_dex_file_method_inliner.h"
#include "dex/quick/mips/mips_dex_file_method_inliner.h"
#include "dex/quick/x86/x86_dex_file_method_inliner.h"

#include "dex_file_to_method_inliner_map.h"

namespace art {

DexFileToMethodInlinerMap::DexFileToMethodInlinerMap(const CompilerDriver* compiler)
    : compiler_(compiler),
      mutex_("inline_helper_mutex")
{
}

DexFileToMethodInlinerMap::~DexFileToMethodInlinerMap() {
  for (auto& entry : inliners_) {
    delete entry.second;
  }
}

const DexFileMethodInliner& DexFileToMethodInlinerMap::GetMethodInliner(const DexFile* dex_file) {
  Thread* self = Thread::Current();
  {
    ReaderMutexLock lock(self, mutex_);
    auto it = inliners_.find(dex_file);
    if (it != inliners_.end()) {
      return *it->second;
    }
  }

  WriterMutexLock lock(self, mutex_);
  DexFileMethodInliner** inliner = &inliners_[dex_file];  // inserts new entry if not found
  if (*inliner) {
    return **inliner;
  }
  switch (compiler_->GetInstructionSet()) {
    case kThumb2:
      *inliner = new ArmDexFileMethodInliner;
      break;
    case kX86:
      *inliner = new X86DexFileMethodInliner;
      break;
    case kMips:
      *inliner = new MipsDexFileMethodInliner;
      break;
    default:
      LOG(FATAL) << "Unexpected instruction set: " << compiler_->GetInstructionSet();
  }
  DCHECK(*inliner != nullptr);
  // TODO: per-dex file locking for the intrinsics container filling.
  (*inliner)->FindIntrinsics(dex_file);
  return **inliner;
}

}  // namespace art
