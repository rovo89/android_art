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

// Forward Declarations.
class CompilationUnit;
class Pass;

namespace art {

/**
 * @class PassDriver
 * @brief PassDriver is the wrapper around all Pass instances in order to execute them from the Middle-End
 */
class PassDriver {
 public:
  explicit PassDriver(CompilationUnit* cu, bool create_default_passes = true);

  ~PassDriver();

  /**
   * @brief Insert a Pass: can warn if multiple passes have the same name.
   * @param new_pass the new Pass to insert in the map and list.
   * @param warn_override warn if the name of the Pass is already used.
   */
  void InsertPass(const Pass* new_pass);

  /**
   * @brief Run a pass using the name as key.
   * @param c_unit the considered CompilationUnit.
   * @param pass_name the Pass name.
   * @return whether the pass was applied.
   */
  bool RunPass(CompilationUnit* c_unit, const char* pass_name);

  /**
   * @brief Run a pass using the Pass itself.
   * @param time_split do we want a time split request(default: false)?
   * @return whether the pass was applied.
   */
  bool RunPass(CompilationUnit* c_unit, const Pass* pass, bool time_split = false);

  void Launch();

  void HandlePassFlag(CompilationUnit* c_unit, const Pass* pass);

  /**
   * @brief Apply a patch: perform start/work/end functions.
   */
  void ApplyPass(CompilationUnit* c_unit, const Pass* pass);

  /**
   * @brief Dispatch a patch: walk the BasicBlocks depending on the traversal mode
   */
  void DispatchPass(CompilationUnit* c_unit, const Pass* pass);

  static void PrintPassNames();
  static void CreateDefaultPassList(const std::string& disable_passes);

  const Pass* GetPass(const char* name) const;

  const char* GetDumpCFGFolder() const {
    return dump_cfg_folder_;
  }

 protected:
  void CreatePasses();

  /** @brief List of passes: provides the order to execute the passes. */
  std::vector<const Pass*> pass_list_;

  /** @brief The CompilationUnit on which to execute the passes on. */
  CompilationUnit* const cu_;

  /** @brief Dump CFG base folder: where is the base folder for dumping CFGs. */
  const char* dump_cfg_folder_;
};

}  // namespace art
#endif  // ART_COMPILER_DEX_PASS_DRIVER_H_
