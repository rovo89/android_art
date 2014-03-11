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

#ifndef ART_COMPILER_DEX_PASS_H_
#define ART_COMPILER_DEX_PASS_H_

#include <string>

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
};

enum DataFlowAnalysisMode {
  kAllNodes = 0,                           /**< @brief All nodes. */
  kPreOrderDFSTraversal,                   /**< @brief Depth-First-Search / Pre-Order. */
  kRepeatingPreOrderDFSTraversal,          /**< @brief Depth-First-Search / Repeating Pre-Order. */
  kReversePostOrderDFSTraversal,           /**< @brief Depth-First-Search / Reverse Post-Order. */
  kRepeatingPostOrderDFSTraversal,         /**< @brief Depth-First-Search / Repeating Post-Order. */
  kRepeatingReversePostOrderDFSTraversal,  /**< @brief Depth-First-Search / Repeating Reverse Post-Order. */
  kPostOrderDOMTraversal,                  /**< @brief Dominator tree / Post-Order. */
  kNoNodes,                                /**< @brief Skip BasicBlock traversal. */
};

/**
 * @class Pass
 * @brief Pass is the Pass structure for the optimizations.
 * @details The following structure has the different optimization passes that we are going to do.
 */
class Pass {
 public:
  explicit Pass(const char* name, DataFlowAnalysisMode type = kAllNodes,
                unsigned int flags = 0u, const char* dump = "")
    : pass_name_(name), traversal_type_(type), flags_(flags), dump_cfg_folder_(dump) {
  }

  Pass(const char* name, DataFlowAnalysisMode type, const char* dump)
    : pass_name_(name), traversal_type_(type), flags_(0), dump_cfg_folder_(dump) {
  }

  Pass(const char* name, const char* dump)
    : pass_name_(name), traversal_type_(kAllNodes), flags_(0), dump_cfg_folder_(dump) {
  }

  virtual ~Pass() {
  }

  virtual const char* GetName() const {
    return pass_name_;
  }

  virtual DataFlowAnalysisMode GetTraversal() const {
    return traversal_type_;
  }

  virtual bool GetFlag(OptimizationFlag flag) const {
    return (flags_ & flag);
  }

  const char* GetDumpCFGFolder() const {
    return dump_cfg_folder_;
  }

  /**
   * @brief Gate for the pass: determines whether to execute the pass or not considering a CompilationUnit
   * @param c_unit the CompilationUnit.
   * @return whether or not to execute the pass
   */
  virtual bool Gate(const CompilationUnit* c_unit) const {
    // Unused parameter.
    UNUSED(c_unit);

    // Base class says yes.
    return true;
  }

  /**
   * @brief Start of the pass: called before the WalkBasicBlocks function
   * @param c_unit the considered CompilationUnit.
   */
  virtual void Start(CompilationUnit* c_unit) const {
    // Unused parameter.
    UNUSED(c_unit);
  }

  /**
   * @brief End of the pass: called after the WalkBasicBlocks function
   * @param c_unit the considered CompilationUnit.
   */
  virtual void End(CompilationUnit* c_unit) const {
    // Unused parameter.
    UNUSED(c_unit);
  }

  /**
   * @brief Actually walk the BasicBlocks following a particular traversal type.
   * @param c_unit the CompilationUnit.
   * @param bb the BasicBlock.
   * @return whether or not there is a change when walking the BasicBlock
   */
  virtual bool WalkBasicBlocks(CompilationUnit* c_unit, BasicBlock* bb) const {
    // Unused parameters.
    UNUSED(c_unit);
    UNUSED(bb);

    // BasicBlock did not change.
    return false;
  }

 protected:
  /** @brief The pass name: used for searching for a pass when running a particular pass or debugging. */
  const char* const pass_name_;

  /** @brief Type of traversal: determines the order to execute the pass on the BasicBlocks. */
  const DataFlowAnalysisMode traversal_type_;

  /** @brief Flags for additional directives: used to determine if a particular clean-up is necessary post pass. */
  const unsigned int flags_;

  /** @brief CFG Dump Folder: what sub-folder to use for dumping the CFGs post pass. */
  const char* const dump_cfg_folder_;

 private:
  // In order to make the all passes not copy-friendly.
  DISALLOW_COPY_AND_ASSIGN(Pass);
};
}  // namespace art
#endif  // ART_COMPILER_DEX_PASS_H_
