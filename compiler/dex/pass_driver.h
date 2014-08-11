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
#include "pass.h"
#include "safe_map.h"

#ifdef QC_STRONG
#define QC_WEAK
#else
#define QC_WEAK __attribute__((weak))
#endif

// Forward Declarations.
class Pass;
class PassDriver;
namespace art {
/**
 * @brief Helper function to create a single instance of a given Pass and can be shared across
 * the threads.
 */
template <typename PassType>
const Pass* GetPassInstance() {
  static const PassType pass;
  return &pass;
}

const Pass* GetMorePassInstance() QC_WEAK;

// Empty holder for the constructor.
class PassDriverDataHolder {
};

/**
 * @class PassDriver
 * @brief PassDriver is the wrapper around all Pass instances in order to execute them
 */
template <typename PassDriverType>
class PassDriver {
 public:
  explicit PassDriver() {
    InitializePasses();
  }

  virtual ~PassDriver() {
  }

  /**
   * @brief Insert a Pass: can warn if multiple passes have the same name.
   */
  void InsertPass(const Pass* new_pass) {
    DCHECK(new_pass != nullptr);
    DCHECK(new_pass->GetName() != nullptr && new_pass->GetName()[0] != 0);

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
    // Paranoid: c_unit cannot be nullptr and we need a pass name.
    DCHECK(pass_name != nullptr && pass_name[0] != 0);

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

  static void CreateDefaultPassList(const std::string& disable_passes) {
    // Insert each pass from g_passes into g_default_pass_list.
    PassDriverType::g_default_pass_list.clear();
    PassDriverType::g_default_pass_list.reserve(PassDriver<PassDriverType>::g_passes_size);
    for (uint16_t i = 0; i < PassDriver<PassDriverType>::g_passes_size; ++i) {
      const Pass* pass = PassDriver<PassDriverType>::g_passes[i];
      // Check if we should disable this pass.
      if (disable_passes.find(pass->GetName()) != std::string::npos) {
        LOG(INFO) << "Skipping " << pass->GetName();
      } else {
        PassDriver<PassDriverType>::g_default_pass_list.push_back(pass);
      }
    }
  }

  /**
   * @brief Run a pass using the Pass itself.
   * @param time_split do we want a time split request(default: false)?
   * @return whether the pass was applied.
   */
  virtual bool RunPass(const Pass* pass, bool time_split = false) = 0;

  /**
   * @brief Print the pass names of all the passes available.
   */
  static void PrintPassNames() {
    LOG(INFO) << "Loop Passes are:";

    for (const Pass* cur_pass : PassDriver<PassDriverType>::g_default_pass_list) {
      LOG(INFO) << "\t-" << cur_pass->GetName();
    }
  }

  /**
   * @brief Gets the list of passes currently schedule to execute.
   * @return pass_list_
   */
  std::vector<const Pass*>& GetPasses() {
    return pass_list_;
  }

  static void SetPrintAllPasses() {
    default_print_passes_ = true;
  }

  static void SetDumpPassList(const std::string& list) {
    dump_pass_list_ = list;
  }

  static void SetPrintPassList(const std::string& list) {
    print_pass_list_ = list;
  }

  void SetDefaultPasses() {
    pass_list_ = PassDriver<PassDriverType>::g_default_pass_list;
  }

 protected:
  virtual void InitializePasses() {
    SetDefaultPasses();
  }

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

  /** @brief List of passes: provides the order to execute the passes. */
  std::vector<const Pass*> pass_list_;

  /** @brief The number of passes within g_passes.  */
  static const uint16_t g_passes_size;

  /** @brief The number of passes within g_passes.  */
  static const Pass* const g_passes[];

  /** @brief The default pass list is used to initialize pass_list_. */
  static std::vector<const Pass*> g_default_pass_list;

  /** @brief Do we, by default, want to be printing the log messages? */
  static bool default_print_passes_;

  /** @brief What are the passes we want to be printing the log messages? */
  static std::string print_pass_list_;

  /** @brief What are the passes we want to be dumping the CFG? */
  static std::string dump_pass_list_;
};

}  // namespace art
#endif  // ART_COMPILER_DEX_PASS_DRIVER_H_
