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

#ifndef ART_COMPILER_DEX_QUICK_X86_X86_DEX_FILE_METHOD_INLINER_H_
#define ART_COMPILER_DEX_QUICK_X86_X86_DEX_FILE_METHOD_INLINER_H_

#include "dex/quick/dex_file_method_inliner.h"

namespace art {

class X86DexFileMethodInliner : public DexFileMethodInliner {
  public:
    X86DexFileMethodInliner();
    ~X86DexFileMethodInliner();

    void FindIntrinsics(const DexFile* dex_file);

  private:
    static const IntrinsicDef kIntrinsicMethods[];
};

}  // namespace art

#endif  // ART_COMPILER_DEX_QUICK_X86_X86_DEX_FILE_METHOD_INLINER_H_
