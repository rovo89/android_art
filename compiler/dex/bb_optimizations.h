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

#ifndef ART_COMPILER_DEX_BB_OPTIMIZATIONS_H_
#define ART_COMPILER_DEX_BB_OPTIMIZATIONS_H_

#include "compiler_internals.h"
#include "pass_me.h"

namespace art {

/**
 * @class CacheFieldLoweringInfo
 * @brief Cache the lowering info for fields used by IGET/IPUT/SGET/SPUT insns.
 */
class CacheFieldLoweringInfo : public PassME {
 public:
  CacheFieldLoweringInfo() : PassME("CacheFieldLoweringInfo", kNoNodes) {
  }

  void Start(PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* cUnit = down_cast<PassMEDataHolder*>(data)->c_unit;
    DCHECK(cUnit != nullptr);
    cUnit->mir_graph->DoCacheFieldLoweringInfo();
  }

  bool Gate(const PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* cUnit = down_cast<const PassMEDataHolder*>(data)->c_unit;
    DCHECK(cUnit != nullptr);
    return cUnit->mir_graph->HasFieldAccess();
  }
};

/**
 * @class CacheMethodLoweringInfo
 * @brief Cache the lowering info for methods called by INVOKEs.
 */
class CacheMethodLoweringInfo : public PassME {
 public:
  CacheMethodLoweringInfo() : PassME("CacheMethodLoweringInfo", kNoNodes) {
  }

  void Start(PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* cUnit = down_cast<PassMEDataHolder*>(data)->c_unit;
    DCHECK(cUnit != nullptr);
    cUnit->mir_graph->DoCacheMethodLoweringInfo();
  }

  bool Gate(const PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* cUnit = down_cast<const PassMEDataHolder*>(data)->c_unit;
    DCHECK(cUnit != nullptr);
    return cUnit->mir_graph->HasInvokes();
  }
};

/**
 * @class SpecialMethodInliner
 * @brief Performs method inlining pass on special kinds of methods.
 * @details Special methods are methods that fall in one of the following categories:
 * empty, instance getter, instance setter, argument return, and constant return.
 */
class SpecialMethodInliner : public PassME {
 public:
  SpecialMethodInliner() : PassME("SpecialMethodInliner") {
  }

  bool Gate(const PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* cUnit = down_cast<const PassMEDataHolder*>(data)->c_unit;
    DCHECK(cUnit != nullptr);
    return cUnit->mir_graph->InlineSpecialMethodsGate();
  }

  void Start(PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* cUnit = down_cast<PassMEDataHolder*>(data)->c_unit;
    DCHECK(cUnit != nullptr);
    cUnit->mir_graph->InlineSpecialMethodsStart();
  }

  bool Worker(const PassDataHolder* data) const {
    DCHECK(data != nullptr);
    const PassMEDataHolder* pass_me_data_holder = down_cast<const PassMEDataHolder*>(data);
    CompilationUnit* cUnit = pass_me_data_holder->c_unit;
    DCHECK(cUnit != nullptr);
    BasicBlock* bb = pass_me_data_holder->bb;
    DCHECK(bb != nullptr);
    cUnit->mir_graph->InlineSpecialMethods(bb);
    // No need of repeating, so just return false.
    return false;
  }

  void End(PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* cUnit = down_cast<PassMEDataHolder*>(data)->c_unit;
    DCHECK(cUnit != nullptr);
    cUnit->mir_graph->InlineSpecialMethodsEnd();
  }
};

/**
 * @class CodeLayout
 * @brief Perform the code layout pass.
 */
class CodeLayout : public PassME {
 public:
  CodeLayout() : PassME("CodeLayout", kAllNodes, kOptimizationBasicBlockChange, "2_post_layout_cfg") {
  }

  void Start(PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* cUnit = down_cast<PassMEDataHolder*>(data)->c_unit;
    DCHECK(cUnit != nullptr);
    cUnit->mir_graph->VerifyDataflow();
  }

  bool Worker(const PassDataHolder* data) const;
};

/**
 * @class NullCheckEliminationAndTypeInference
 * @brief Null check elimination and type inference.
 */
class NullCheckEliminationAndTypeInference : public PassME {
 public:
  NullCheckEliminationAndTypeInference()
    : PassME("NCE_TypeInference", kRepeatingTopologicalSortTraversal, "4_post_nce_cfg") {
  }

  void Start(PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* cUnit = down_cast<PassMEDataHolder*>(data)->c_unit;
    DCHECK(cUnit != nullptr);
    cUnit->mir_graph->EliminateNullChecksAndInferTypesStart();
  }

  bool Worker(const PassDataHolder* data) const {
    DCHECK(data != nullptr);
    const PassMEDataHolder* pass_me_data_holder = down_cast<const PassMEDataHolder*>(data);
    CompilationUnit* cUnit = pass_me_data_holder->c_unit;
    DCHECK(cUnit != nullptr);
    BasicBlock* bb = pass_me_data_holder->bb;
    DCHECK(bb != nullptr);
    return cUnit->mir_graph->EliminateNullChecksAndInferTypes(bb);
  }

  void End(PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* cUnit = down_cast<PassMEDataHolder*>(data)->c_unit;
    DCHECK(cUnit != nullptr);
    cUnit->mir_graph->EliminateNullChecksAndInferTypesEnd();
  }
};

class ClassInitCheckElimination : public PassME {
 public:
  ClassInitCheckElimination()
    : PassME("ClInitCheckElimination", kLoopRepeatingTopologicalSortTraversal) {
  }

  bool Gate(const PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* cUnit = down_cast<const PassMEDataHolder*>(data)->c_unit;
    DCHECK(cUnit != nullptr);
    return cUnit->mir_graph->EliminateClassInitChecksGate();
  }

  bool Worker(const PassDataHolder* data) const {
    DCHECK(data != nullptr);
    const PassMEDataHolder* pass_me_data_holder = down_cast<const PassMEDataHolder*>(data);
    CompilationUnit* cUnit = pass_me_data_holder->c_unit;
    DCHECK(cUnit != nullptr);
    BasicBlock* bb = pass_me_data_holder->bb;
    DCHECK(bb != nullptr);
    return cUnit->mir_graph->EliminateClassInitChecks(bb);
  }

  void End(PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* cUnit = down_cast<PassMEDataHolder*>(data)->c_unit;
    DCHECK(cUnit != nullptr);
    cUnit->mir_graph->EliminateClassInitChecksEnd();
  }
};

/**
 * @class GlobalValueNumberingPass
 * @brief Performs the global value numbering pass.
 */
class GlobalValueNumberingPass : public PassME {
 public:
  GlobalValueNumberingPass()
    : PassME("GVN", kLoopRepeatingTopologicalSortTraversal, "4_post_gvn_cfg") {
  }

  bool Gate(const PassDataHolder* data) const OVERRIDE {
    DCHECK(data != nullptr);
    CompilationUnit* cUnit = down_cast<const PassMEDataHolder*>(data)->c_unit;
    DCHECK(cUnit != nullptr);
    return cUnit->mir_graph->ApplyGlobalValueNumberingGate();
  }

  bool Worker(const PassDataHolder* data) const OVERRIDE {
    DCHECK(data != nullptr);
    const PassMEDataHolder* pass_me_data_holder = down_cast<const PassMEDataHolder*>(data);
    CompilationUnit* cUnit = pass_me_data_holder->c_unit;
    DCHECK(cUnit != nullptr);
    BasicBlock* bb = pass_me_data_holder->bb;
    DCHECK(bb != nullptr);
    return cUnit->mir_graph->ApplyGlobalValueNumbering(bb);
  }

  void End(PassDataHolder* data) const OVERRIDE {
    DCHECK(data != nullptr);
    CompilationUnit* cUnit = down_cast<PassMEDataHolder*>(data)->c_unit;
    DCHECK(cUnit != nullptr);
    cUnit->mir_graph->ApplyGlobalValueNumberingEnd();
  }
};

/**
 * @class BBCombine
 * @brief Perform the basic block combination pass.
 */
class BBCombine : public PassME {
 public:
  BBCombine() : PassME("BBCombine", kPreOrderDFSTraversal, "5_post_bbcombine_cfg") {
  }

  bool Gate(const PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* cUnit = down_cast<const PassMEDataHolder*>(data)->c_unit;
    DCHECK(cUnit != nullptr);
    return ((cUnit->disable_opt & (1 << kSuppressExceptionEdges)) != 0);
  }

  bool Worker(const PassDataHolder* data) const;
};

/**
 * @class BasicBlock Optimizations
 * @brief Any simple BasicBlock optimization can be put here.
 */
class BBOptimizations : public PassME {
 public:
  BBOptimizations() : PassME("BBOptimizations", kNoNodes, "5_post_bbo_cfg") {
  }

  bool Gate(const PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* cUnit = down_cast<const PassMEDataHolder*>(data)->c_unit;
    DCHECK(cUnit != nullptr);
    return ((cUnit->disable_opt & (1 << kBBOpt)) == 0);
  }

  void Start(PassDataHolder* data) const;
};

// dummy pass, for placeholder only
class DummyPass : public PassME {
    public:
     DummyPass() : PassME("DummyPass", kNoNodes, "") {
     }

     bool Gate(const PassDataHolder* data) const {
        return false;
     }
};

}  // namespace art

#endif  // ART_COMPILER_DEX_BB_OPTIMIZATIONS_H_
