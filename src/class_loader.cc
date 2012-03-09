/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include "class_loader.h"

#include "class_linker.h"
#include "runtime.h"

namespace art {

// TODO: get global references for these
Class* PathClassLoader::dalvik_system_PathClassLoader_ = NULL;

PathClassLoader* PathClassLoader::AllocCompileTime(std::vector<const DexFile*>& dex_files) {
  CHECK(!Runtime::Current()->IsStarted());
  DCHECK(dalvik_system_PathClassLoader_ != NULL);
  SirtRef<PathClassLoader> p(down_cast<PathClassLoader*>(dalvik_system_PathClassLoader_->AllocObject()));
  Runtime::Current()->SetCompileTimeClassPath(p.get(), dex_files);
  return p.get();
}

void PathClassLoader::SetClass(Class* dalvik_system_PathClassLoader) {
  CHECK(dalvik_system_PathClassLoader_ == NULL);
  CHECK(dalvik_system_PathClassLoader != NULL);
  dalvik_system_PathClassLoader_ = dalvik_system_PathClassLoader;
}

void PathClassLoader::ResetClass() {
  CHECK(dalvik_system_PathClassLoader_ != NULL);
  dalvik_system_PathClassLoader_ = NULL;
}

}  // namespace art
