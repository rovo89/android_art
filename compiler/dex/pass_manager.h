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

#ifndef ART_COMPILER_DEX_PASS_MANAGER_H_
#define ART_COMPILER_DEX_PASS_MANAGER_H_

#include <string>
#include <vector>

#include "base/logging.h"

namespace art {

class Pass;

class PassManagerOptions {
 public:
  PassManagerOptions()
     : default_print_passes_(false),
       print_pass_names_(false),
       print_pass_options_(false) {
  }
  explicit PassManagerOptions(const PassManagerOptions&) = default;

  void SetPrintPassNames(bool b) {
    print_pass_names_ = b;
  }

  void SetPrintAllPasses() {
    default_print_passes_ = true;
  }
  bool GetPrintAllPasses() const {
    return default_print_passes_;
  }

  void SetDisablePassList(const std::string& list) {
    disable_pass_list_ = list;
  }
  const std::string& GetDisablePassList() const {
    return disable_pass_list_;
  }

  void SetPrintPassList(const std::string& list) {
    print_pass_list_ = list;
  }
  const std::string& GetPrintPassList() const {
    return print_pass_list_;
  }

  void SetDumpPassList(const std::string& list) {
    dump_pass_list_ = list;
  }
  const std::string& GetDumpPassList() const {
    return dump_pass_list_;
  }

  /**
   * @brief Used to set a string that contains the overridden pass options.
   * @details An overridden pass option means that the pass uses this option
   * instead of using its default option.
   * @param s The string passed by user with overridden options. The string is in format
   * Pass1Name:Pass1Option:Pass1Setting,Pass2Name:Pass2Option::Pass2Setting
   */
  void SetOverriddenPassOptions(const std::string& list) {
    overridden_pass_options_list_ = list;
  }
  const std::string& GetOverriddenPassOptions() const {
    return overridden_pass_options_list_;
  }

  void SetPrintPassOptions(bool b) {
    print_pass_options_ = b;
  }
  bool GetPrintPassOptions() const {
    return print_pass_options_;
  }

 private:
  /** @brief Do we, by default, want to be printing the log messages? */
  bool default_print_passes_;

  /** @brief What are the passes we want to be printing the log messages? */
  std::string print_pass_list_;

  /** @brief What are the passes we want to be dumping the CFG? */
  std::string dump_pass_list_;

  /** @brief String of all options that should be overridden for selected passes */
  std::string overridden_pass_options_list_;

  /** @brief String of all options that should be overridden for selected passes */
  std::string disable_pass_list_;

  /** @brief Whether or not we print all the passes when we create the pass manager */
  bool print_pass_names_;

  /** @brief Whether or not we print all the pass options when we create the pass manager */
  bool print_pass_options_;
};

/**
 * @class PassManager
 * @brief Owns passes
 */
class PassManager {
 public:
  explicit PassManager(const PassManagerOptions& options);
  virtual ~PassManager();
  void CreateDefaultPassList();
  void AddPass(const Pass* pass) {
    passes_.push_back(pass);
  }
  /**
   * @brief Print the pass names of all the passes available.
   */
  void PrintPassNames() const;
  const std::vector<const Pass*>* GetDefaultPassList() const {
    return &default_pass_list_;
  }
  const PassManagerOptions& GetOptions() const {
    return options_;
  }

 private:
  /** @brief The set of possible passes.  */
  std::vector<const Pass*> passes_;

  /** @brief The default pass list is used to initialize pass_list_. */
  std::vector<const Pass*> default_pass_list_;

  /** @brief Pass manager options. */
  PassManagerOptions options_;

  DISALLOW_COPY_AND_ASSIGN(PassManager);
};
}  // namespace art
#endif  // ART_COMPILER_DEX_PASS_MANAGER_H_
