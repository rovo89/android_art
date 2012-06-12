/*
 * Copyright (C) 2012 The Android Open Source Project
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

#ifndef ART_SRC_GREENLAND_GBC_CONTEXT_H_
#define ART_SRC_GREENLAND_GBC_CONTEXT_H_

#include "dex_lang.h"

#include "macros.h"

#include <llvm/LLVMContext.h>

namespace llvm {
  class Module;
} // namespace llvm

namespace art {
namespace greenland {

class IntrinsicHelper;

class GBCContext {
 private:
  llvm::LLVMContext context_;
  llvm::Module* module_;
  DexLang::Context* dex_lang_ctx_;

  mutable volatile int32_t ref_count_;
  volatile int32_t mem_usage_;

  ~GBCContext();

 public:
  GBCContext();

  inline llvm::LLVMContext& GetLLVMContext()
  { return context_; }
  inline llvm::Module& GetOutputModule()
  { return *module_; }

  inline IntrinsicHelper& GetIntrinsicHelper()
  { return dex_lang_ctx_->GetIntrinsicHelper(); }
  inline const IntrinsicHelper& GetIntrinsicHelper() const
  { return dex_lang_ctx_->GetIntrinsicHelper(); }

  inline DexLang::Context& GetDexLangContext()
  { return *dex_lang_ctx_; }
  inline const DexLang::Context& GetDexLangContext() const
  { return *dex_lang_ctx_; }

  GBCContext& IncRef();
  const GBCContext& IncRef() const;
  void DecRef() const;

  void AddMemUsageApproximation(size_t usage);
  inline bool IsMemUsageThresholdReached() const {
    return (mem_usage_ > (30 << 20)); // (threshold: 30MiB)
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(GBCContext);
};

} // namespace greenland
} // namespace art

#endif // ART_SRC_GREENLAND_GBC_CONTEXT_H_
