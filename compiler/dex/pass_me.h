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

#ifndef ART_COMPILER_DEX_PASS_ME_H_
#define ART_COMPILER_DEX_PASS_ME_H_

#include <string>
#include "pass.h"

namespace art {

// Forward declarations.
struct BasicBlock;
struct CompilationUnit;
class Pass;

/**
 * @brief OptimizationFlag is an enumeration to perform certain tasks for a given pass.
 * @details Each enum should be a power of 2 to be correctly used.
 */
enum OptimizationFlag {
  kOptimizationBasicBlockChange = 1,  /**< @brief Has there been a change to a BasicBlock? */
  kOptimizationDefUsesChange = 2,     /**< @brief Has there been a change to a def-use? */
  kLoopStructureChange = 4,           /**< @brief Has there been a loop structural change? */
};

// Data holder class.
class PassMEDataHolder: public PassDataHolder {
  public:
    CompilationUnit* c_unit;
    BasicBlock* bb;
    void* data;               /**< @brief Any data the pass wants to use */
    bool dirty;               /**< @brief Has the pass rendered the CFG dirty, requiring post-opt? */
};

enum DataFlowAnalysisMode {
  kAllNodes = 0,                           /**< @brief All nodes. */
  kPreOrderDFSTraversal,                   /**< @brief Depth-First-Search / Pre-Order. */
  kRepeatingPreOrderDFSTraversal,          /**< @brief Depth-First-Search / Repeating Pre-Order. */
  kReversePostOrderDFSTraversal,           /**< @brief Depth-First-Search / Reverse Post-Order. */
  kRepeatingPostOrderDFSTraversal,         /**< @brief Depth-First-Search / Repeating Post-Order. */
  kRepeatingReversePostOrderDFSTraversal,  /**< @brief Depth-First-Search / Repeating Reverse Post-Order. */
  kPostOrderDOMTraversal,                  /**< @brief Dominator tree / Post-Order. */
  kTopologicalSortTraversal,               /**< @brief Topological Order traversal. */
  kLoopRepeatingTopologicalSortTraversal,  /**< @brief Loop-repeating Topological Order traversal. */
  kNoNodes,                                /**< @brief Skip BasicBlock traversal. */
};

/**
 * @class Pass
 * @brief Pass is the Pass structure for the optimizations.
 * @details The following structure has the different optimization passes that we are going to do.
 */
class PassME: public Pass {
 public:
  explicit PassME(const char* name, DataFlowAnalysisMode type = kAllNodes,
          unsigned int flags = 0u, const char* dump = "")
    : Pass(name), traversal_type_(type), flags_(flags), dump_cfg_folder_(dump) {
  }

  PassME(const char* name, DataFlowAnalysisMode type, const char* dump)
    : Pass(name), traversal_type_(type), flags_(0), dump_cfg_folder_(dump) {
  }

  PassME(const char* name, const char* dump)
    : Pass(name), traversal_type_(kAllNodes), flags_(0), dump_cfg_folder_(dump) {
  }

  ~PassME() {
    default_options_.clear();
  }

  virtual DataFlowAnalysisMode GetTraversal() const {
    return traversal_type_;
  }

  /**
   * @return Returns whether the pass has any configurable options.
   */
  bool HasOptions() const {
    return default_options_.size() != 0;
  }

  /**
   * @brief Prints the pass options along with default settings if there are any.
   * @details The printing is done using LOG(INFO).
   */
  void PrintPassDefaultOptions() const {
    for (auto option_it = default_options_.begin(); option_it != default_options_.end(); option_it++) {
      LOG(INFO) << "\t" << option_it->first << ":" << std::dec << option_it->second;
    }
  }

  /**
   * @brief Prints the pass options along with either default or overridden setting.
   * @param overridden_options The overridden settings for this pass.
   */
  void PrintPassOptions(SafeMap<const std::string, int>& overridden_options) const {
    // We walk through the default options only to get the pass names. We use GetPassOption to
    // also consider the overridden ones.
    for (auto option_it = default_options_.begin(); option_it != default_options_.end(); option_it++) {
      LOG(INFO) << "\t" << option_it->first << ":" << std::dec << GetPassOption(option_it->first, overridden_options);
    }
  }

  /**
   * @brief Used to obtain the option for a pass.
   * @details Will return the overridden option if it exists or default one.
   * @param option_name The name of option whose setting to look for.
   * @param c_unit The compilation unit currently being handled.
   * @return Returns the setting for the pass option.
   */
  int GetPassOption(const char* option_name, CompilationUnit* c_unit) const {
    return GetPassOption(option_name, c_unit->overridden_pass_options);
  }

  const char* GetDumpCFGFolder() const {
    return dump_cfg_folder_;
  }

  bool GetFlag(OptimizationFlag flag) const {
    return (flags_ & flag);
  }

 protected:
  int GetPassOption(const char* option_name, const SafeMap<const std::string, int>& overridden_options) const {
    // First check if there are any overridden settings.
    auto overridden_it = overridden_options.find(std::string(option_name));
    if (overridden_it != overridden_options.end()) {
      return overridden_it->second;
    }

    // Next check the default options.
    auto default_it = default_options_.find(option_name);

    if (default_it == default_options_.end()) {
      // An invalid option is being requested.
      DCHECK(false);
      return 0;
    }

    return default_it->second;
  }

  /** @brief Type of traversal: determines the order to execute the pass on the BasicBlocks. */
  const DataFlowAnalysisMode traversal_type_;

  /** @brief Flags for additional directives: used to determine if a particular post-optimization pass is necessary. */
  const unsigned int flags_;

  /** @brief CFG Dump Folder: what sub-folder to use for dumping the CFGs post pass. */
  const char* const dump_cfg_folder_;

  /**
   * @brief Contains a map of options with the default settings.
   * @details The constructor of the specific pass instance should fill this
   * with default options.
   * */
  SafeMap<const char*, int> default_options_;
};
}  // namespace art
#endif  // ART_COMPILER_DEX_PASS_ME_H_
