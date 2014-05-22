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

#ifndef ART_COMPILER_DEX_PASS_DRIVER_ME_H_
#define ART_COMPILER_DEX_PASS_DRIVER_ME_H_

#include "bb_optimizations.h"
#include "pass_driver.h"
#include "pass_me.h"

namespace art {

class PassDriverME: public PassDriver<PassDriverME> {
 public:
  explicit PassDriverME(CompilationUnit* cu);
  ~PassDriverME();
  /**
   * @brief Dispatch a patch: walk the BasicBlocks depending on the traversal mode
   */
  void DispatchPass(const Pass* pass);
  bool RunPass(const Pass* pass, bool time_split = false);
  const char* GetDumpCFGFolder() const;
 protected:
  /** @brief The data holder that contains data needed for the PassDriverME. */
  PassMEDataHolder pass_me_data_holder_;

  /** @brief Dump CFG base folder: where is the base folder for dumping CFGs. */
  const char* dump_cfg_folder_;
};

}  // namespace art
#endif  // ART_COMPILER_DEX_PASS_DRIVER_ME_H_
