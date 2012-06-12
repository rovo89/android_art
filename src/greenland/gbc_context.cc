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

#include "gbc_context.h"

#include "atomic.h"

namespace art {
namespace greenland {

GBCContext::GBCContext()
    : context_(), module_(NULL), ref_count_(1), mem_usage_(0) {
  module_ = new llvm::Module("art", context_);

  // Initialize the contents of an empty module
  // Type of "JavaObject"
  llvm::StructType::create(context_, "JavaObject");
  // Type of "Method"
  llvm::StructType::create(context_, "Method");
  // Type of "Thread"
  llvm::StructType::create(context_, "Thread");

  dex_lang_ctx_ = new DexLang::Context(*module_);
  return;
}

GBCContext::~GBCContext() {
  delete dex_lang_ctx_;
  return;
}

GBCContext& GBCContext::IncRef() {
  android_atomic_inc(&ref_count_);
  return *this;
}

const GBCContext& GBCContext::IncRef() const {
  android_atomic_inc(&ref_count_);
  return *this;
}

void GBCContext::DecRef() const {
  int32_t old_ref_count = android_atomic_dec(&ref_count_);
  if (old_ref_count <= 1) {
    delete this;
  }
  return;
}

void GBCContext::AddMemUsageApproximation(size_t usage) {
  android_atomic_add(static_cast<int32_t>(usage), &mem_usage_);
  return;
}

} // namespace greenland
} // namespace art
