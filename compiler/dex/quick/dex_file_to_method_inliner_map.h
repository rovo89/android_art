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

#ifndef ART_COMPILER_DEX_QUICK_DEX_FILE_TO_METHOD_INLINER_MAP_H_
#define ART_COMPILER_DEX_QUICK_DEX_FILE_TO_METHOD_INLINER_MAP_H_

#include <map>
#include <vector>
#include "base/macros.h"
#include "base/mutex.h"

#include "dex/quick/dex_file_method_inliner.h"

namespace art {

class CompilerDriver;
class DexFile;

/**
 * Map each DexFile to its DexFileMethodInliner.
 *
 * The method inliner is created and initialized the first time it's requested
 * for a particular DexFile.
 */
class DexFileToMethodInlinerMap {
  public:
    explicit DexFileToMethodInlinerMap(const CompilerDriver* compiler);
    ~DexFileToMethodInlinerMap();

    const DexFileMethodInliner& GetMethodInliner(const DexFile* dex_file) LOCKS_EXCLUDED(mutex_);

  private:
    const CompilerDriver* const compiler_;
    ReaderWriterMutex mutex_;
    std::map<const DexFile*, DexFileMethodInliner*> inliners_ GUARDED_BY(mutex_);
};

}  // namespace art

#endif  // ART_COMPILER_DEX_QUICK_DEX_FILE_TO_METHOD_INLINER_MAP_H_
