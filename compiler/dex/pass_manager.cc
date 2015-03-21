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

#include "pass_manager.h"

#include "base/stl_util.h"
#include "pass_me.h"

namespace art {

PassManager::PassManager(const PassManagerOptions& options) : options_(options) {
}

PassManager::~PassManager() {
  STLDeleteElements(&passes_);
}

void PassManager::CreateDefaultPassList() {
  default_pass_list_.clear();
  // Add each pass which isn't disabled into default_pass_list_.
  for (const auto* pass : passes_) {
    if (options_.GetDisablePassList().find(pass->GetName()) != std::string::npos) {
      VLOG(compiler) << "Skipping disabled pass " << pass->GetName();
    } else {
      default_pass_list_.push_back(pass);
    }
  }
}

void PassManager::PrintPassNames() const {
  LOG(INFO) << "Loop Passes are:";
  for (const Pass* cur_pass : default_pass_list_) {
    LOG(INFO) << "\t-" << cur_pass->GetName();
  }
}

}  // namespace art
