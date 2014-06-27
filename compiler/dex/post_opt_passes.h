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

#ifndef ART_COMPILER_DEX_POST_OPT_PASSES_H_
#define ART_COMPILER_DEX_POST_OPT_PASSES_H_

#include "compiler_internals.h"
#include "pass_me.h"

namespace art {

/**
 * @class InitializeData
 * @brief There is some data that needs to be initialized before performing
 * the post optimization passes.
 */
class InitializeData : public PassME {
 public:
  InitializeData() : PassME("InitializeData") {
  }

  void Start(PassDataHolder* data) const {
    // New blocks may have been inserted so the first thing we do is ensure that
    // the c_unit's number of blocks matches the actual count of basic blocks.
    DCHECK(data != nullptr);
    CompilationUnit* c_unit = down_cast<PassMEDataHolder*>(data)->c_unit;
    DCHECK(c_unit != nullptr);
    c_unit->mir_graph.get()->InitializeBasicBlockData();
    c_unit->mir_graph.get()->SSATransformationStart();
  }
};

/**
 * @class MethodUseCount
 * @brief Count the register uses of the method
 */
class MethodUseCount : public PassME {
 public:
  MethodUseCount() : PassME("UseCount") {
  }

  bool Worker(const PassDataHolder* data) const;

  bool Gate(const PassDataHolder* data) const;
};

/**
 * @class ClearPhiInformation
 * @brief Clear the PHI nodes from the CFG.
 */
class ClearPhiInstructions : public PassME {
 public:
  ClearPhiInstructions() : PassME("ClearPhiInstructions") {
  }

  bool Worker(const PassDataHolder* data) const;
};

/**
 * @class CalculatePredecessors
 * @brief Calculate the predecessor BitVector of each Basicblock.
 */
class CalculatePredecessors : public PassME {
 public:
  CalculatePredecessors() : PassME("CalculatePredecessors") {
  }

  void Start(PassDataHolder* data) const;
};

/**
 * @class DFSOrders
 * @brief Compute the DFS order of the MIR graph
 */
class DFSOrders : public PassME {
 public:
  DFSOrders() : PassME("DFSOrders") {
  }

  void Start(PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* c_unit = down_cast<PassMEDataHolder*>(data)->c_unit;
    DCHECK(c_unit != nullptr);
    c_unit->mir_graph.get()->ComputeDFSOrders();
  }
};

/**
 * @class BuildDomination
 * @brief Build the domination information of the MIR Graph
 */
class BuildDomination : public PassME {
 public:
  BuildDomination() : PassME("BuildDomination") {
  }

  void Start(PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* c_unit = down_cast<PassMEDataHolder*>(data)->c_unit;
    DCHECK(c_unit != nullptr);
    c_unit->mir_graph.get()->ComputeDominators();
    c_unit->mir_graph.get()->CompilerInitializeSSAConversion();
  }

  void End(PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* c_unit = down_cast<PassMEDataHolder*>(data)->c_unit;
    DCHECK(c_unit != nullptr);
    // Verify the dataflow information after the pass.
    if (c_unit->enable_debug & (1 << kDebugVerifyDataflow)) {
      c_unit->mir_graph->VerifyDataflow();
    }
  }
};

/**
 * @class TopologicalSortOrders
 * @brief Compute the topological sort order of the MIR graph
 */
class TopologicalSortOrders : public PassME {
 public:
  TopologicalSortOrders() : PassME("TopologicalSortOrders") {
  }

  void Start(PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* c_unit = down_cast<PassMEDataHolder*>(data)->c_unit;
    DCHECK(c_unit != nullptr);
    c_unit->mir_graph.get()->ComputeTopologicalSortOrder();
  }
};

/**
 * @class DefBlockMatrix
 * @brief Calculate the matrix of definition per basic block
 */
class DefBlockMatrix : public PassME {
 public:
  DefBlockMatrix() : PassME("DefBlockMatrix") {
  }

  void Start(PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* c_unit = down_cast<PassMEDataHolder*>(data)->c_unit;
    DCHECK(c_unit != nullptr);
    c_unit->mir_graph.get()->ComputeDefBlockMatrix();
  }
};

/**
 * @class CreatePhiNodes
 * @brief Pass to create the phi nodes after SSA calculation
 */
class CreatePhiNodes : public PassME {
 public:
  CreatePhiNodes() : PassME("CreatePhiNodes") {
  }

  void Start(PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* c_unit = down_cast<PassMEDataHolder*>(data)->c_unit;
    DCHECK(c_unit != nullptr);
    c_unit->mir_graph.get()->InsertPhiNodes();
  }
};

/**
 * @class ClearVisitedFlag
 * @brief Pass to clear the visited flag for all basic blocks.
 */

class ClearVisitedFlag : public PassME {
 public:
  ClearVisitedFlag() : PassME("ClearVisitedFlag") {
  }

  void Start(PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* c_unit = down_cast<PassMEDataHolder*>(data)->c_unit;
    DCHECK(c_unit != nullptr);
    c_unit->mir_graph.get()->ClearAllVisitedFlags();
  }
};

/**
 * @class SSAConversion
 * @brief Pass for SSA conversion of MIRs
 */
class SSAConversion : public PassME {
 public:
  SSAConversion() : PassME("SSAConversion") {
  }

  void Start(PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* c_unit = down_cast<PassMEDataHolder*>(data)->c_unit;
    DCHECK(c_unit != nullptr);
    MIRGraph *mir_graph = c_unit->mir_graph.get();
    mir_graph->DoDFSPreOrderSSARename(mir_graph->GetEntryBlock());
  }
};

/**
 * @class PhiNodeOperands
 * @brief Pass to insert the Phi node operands to basic blocks
 */
class PhiNodeOperands : public PassME {
 public:
  PhiNodeOperands() : PassME("PhiNodeOperands", kPreOrderDFSTraversal) {
  }

  bool Worker(const PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* c_unit = down_cast<const PassMEDataHolder*>(data)->c_unit;
    DCHECK(c_unit != nullptr);
    BasicBlock* bb = down_cast<const PassMEDataHolder*>(data)->bb;
    DCHECK(bb != nullptr);
    c_unit->mir_graph->InsertPhiNodeOperands(bb);
    // No need of repeating, so just return false.
    return false;
  }
};

/**
 * @class InitRegLocations
 * @brief Initialize Register Locations.
 */
class PerformInitRegLocations : public PassME {
 public:
  PerformInitRegLocations() : PassME("PerformInitRegLocation") {
  }

  void Start(PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* c_unit = down_cast<PassMEDataHolder*>(data)->c_unit;
    DCHECK(c_unit != nullptr);
    c_unit->mir_graph->InitRegLocations();
  }
};

/**
 * @class ConstantPropagation
 * @brief Perform a constant propagation pass.
 */
class ConstantPropagation : public PassME {
 public:
  ConstantPropagation() : PassME("ConstantPropagation") {
  }

  bool Worker(const PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* c_unit = down_cast<const PassMEDataHolder*>(data)->c_unit;
    DCHECK(c_unit != nullptr);
    BasicBlock* bb = down_cast<const PassMEDataHolder*>(data)->bb;
    DCHECK(bb != nullptr);
    c_unit->mir_graph->DoConstantPropagation(bb);
    // No need of repeating, so just return false.
    return false;
  }

  void Start(PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* c_unit = down_cast<PassMEDataHolder*>(data)->c_unit;
    DCHECK(c_unit != nullptr);
    c_unit->mir_graph->InitializeConstantPropagation();
  }
};

/**
 * @class FreeData
 * @brief There is some data that needs to be freed after performing the post optimization passes.
 */
class FreeData : public PassME {
 public:
  FreeData() : PassME("FreeData") {
  }

  void End(PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* c_unit = down_cast<PassMEDataHolder*>(data)->c_unit;
    DCHECK(c_unit != nullptr);
    c_unit->mir_graph.get()->SSATransformationEnd();
  }
};

}  // namespace art

#endif  // ART_COMPILER_DEX_POST_OPT_PASSES_H_
