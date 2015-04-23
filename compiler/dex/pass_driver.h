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

#ifndef ART_COMPILER_DEX_PASS_DRIVER_H_
#define ART_COMPILER_DEX_PASS_DRIVER_H_

#include <vector>

#include "base/logging.h"
#include "pass.h"
#include "pass_manager.h"

namespace art {

class Pass;
class PassDataHolder;
class PassDriver;
class PassManager;

// Empty holder for the constructor.
class PassDriverDataHolder {
};

/**
 * @class PassDriver
 * @brief PassDriver is the wrapper around all Pass instances in order to execute them
 */
class PassDriver {
 public:
  explicit PassDriver(const PassManager* const pass_manager) : pass_manager_(pass_manager) {
    pass_list_ = *pass_manager_->GetDefaultPassList();
    DCHECK(!pass_list_.empty());
  }

  virtual ~PassDriver() {
  }

  /**
   * @brief Insert a Pass: can warn if multiple passes have the same name.
   */
  void InsertPass(const Pass* new_pass) {
    DCHECK(new_pass != nullptr);
    DCHECK(new_pass->GetName() != nullptr);
    DCHECK_NE(new_pass->GetName()[0], 0);

    // It is an error to override an existing pass.
    DCHECK(GetPass(new_pass->GetName()) == nullptr)
        << "Pass name " << new_pass->GetName() << " already used.";
    // Now add to the list.
    pass_list_.push_back(new_pass);
  }

  /**
   * @brief Run a pass using the name as key.
   * @return whether the pass was applied.
   */
  virtual bool RunPass(const char* pass_name) {
    // Paranoid: c_unit cannot be null and we need a pass name.
    DCHECK(pass_name != nullptr);
    DCHECK_NE(pass_name[0], 0);

    const Pass* cur_pass = GetPass(pass_name);

    if (cur_pass != nullptr) {
      return RunPass(cur_pass);
    }

    // Return false, we did not find the pass.
    return false;
  }

  /**
   * @brief Runs all the passes with the pass_list_.
   */
  void Launch() {
    for (const Pass* cur_pass : pass_list_) {
      RunPass(cur_pass);
    }
  }

  /**
   * @brief Searches for a particular pass.
   * @param the name of the pass to be searched for.
   */
  const Pass* GetPass(const char* name) const {
    for (const Pass* cur_pass : pass_list_) {
      if (strcmp(name, cur_pass->GetName()) == 0) {
        return cur_pass;
      }
    }
    return nullptr;
  }

  /**
   * @brief Run a pass using the Pass itself.
   * @param time_split do we want a time split request(default: false)?
   * @return whether the pass was applied.
   */
  virtual bool RunPass(const Pass* pass, bool time_split = false) = 0;

 protected:
  /**
   * @brief Apply a patch: perform start/work/end functions.
   */
  virtual void ApplyPass(PassDataHolder* data, const Pass* pass) {
    pass->Start(data);
    DispatchPass(pass);
    pass->End(data);
  }

  /**
   * @brief Dispatch a patch.
   * Gives the ability to add logic when running the patch.
   */
  virtual void DispatchPass(const Pass* pass) {
    UNUSED(pass);
  }

  /** @brief List of passes: provides the order to execute the passes.
   *  Passes are owned by pass_manager_. */
  std::vector<const Pass*> pass_list_;

  const PassManager* const pass_manager_;
};

}  // namespace art
#endif  // ART_COMPILER_DEX_PASS_DRIVER_H_
