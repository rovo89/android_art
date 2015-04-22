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

#include "base/casts.h"
#include "base/logging.h"
#include "compiler_ir.h"
#include "dex_flags.h"
#include "mir_graph.h"
#include "pass_me.h"

namespace art {

/**
 * @class PassMEMirSsaRep
 * @brief Convenience class for passes that check MIRGraph::MirSsaRepUpToDate().
 */
class PassMEMirSsaRep : public PassME {
 public:
  PassMEMirSsaRep(const char* name, DataFlowAnalysisMode type = kAllNodes)
      : PassME(name, type) {
  }

  bool Gate(const PassDataHolder* data) const OVERRIDE {
    DCHECK(data != nullptr);
    CompilationUnit* c_unit = down_cast<const PassMEDataHolder*>(data)->c_unit;
    DCHECK(c_unit != nullptr);
    return !c_unit->mir_graph->MirSsaRepUpToDate();
  }
};

/**
 * @class InitializeSSATransformation
 * @brief There is some data that needs to be initialized before performing
 * the post optimization passes.
 */
class InitializeSSATransformation : public PassMEMirSsaRep {
 public:
  InitializeSSATransformation() : PassMEMirSsaRep("InitializeSSATransformation", kNoNodes) {
  }

  void Start(PassDataHolder* data) const {
    // New blocks may have been inserted so the first thing we do is ensure that
    // the c_unit's number of blocks matches the actual count of basic blocks.
    DCHECK(data != nullptr);
    CompilationUnit* c_unit = down_cast<PassMEDataHolder*>(data)->c_unit;
    DCHECK(c_unit != nullptr);
    c_unit->mir_graph->SSATransformationStart();
    c_unit->mir_graph->CompilerInitializeSSAConversion();
  }
};

/**
 * @class ClearPhiInformation
 * @brief Clear the PHI nodes from the CFG.
 */
class ClearPhiInstructions : public PassMEMirSsaRep {
 public:
  ClearPhiInstructions() : PassMEMirSsaRep("ClearPhiInstructions") {
  }

  bool Worker(PassDataHolder* data) const;
};

/**
 * @class CalculatePredecessors
 * @brief Calculate the predecessor BitVector of each Basicblock.
 */
class CalculatePredecessors : public PassME {
 public:
  CalculatePredecessors() : PassME("CalculatePredecessors", kNoNodes) {
  }

  void Start(PassDataHolder* data) const;
};

/**
 * @class DFSOrders
 * @brief Compute the DFS order of the MIR graph
 */
class DFSOrders : public PassME {
 public:
  DFSOrders() : PassME("DFSOrders", kNoNodes) {
  }

  bool Gate(const PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* c_unit = down_cast<const PassMEDataHolder*>(data)->c_unit;
    DCHECK(c_unit != nullptr);
    return !c_unit->mir_graph->DfsOrdersUpToDate();
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
  BuildDomination() : PassME("BuildDomination", kNoNodes) {
  }

  bool Gate(const PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* c_unit = down_cast<const PassMEDataHolder*>(data)->c_unit;
    DCHECK(c_unit != nullptr);
    return !c_unit->mir_graph->DominationUpToDate();
  }

  void Start(PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* c_unit = down_cast<PassMEDataHolder*>(data)->c_unit;
    DCHECK(c_unit != nullptr);
    c_unit->mir_graph->ComputeDominators();
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
  TopologicalSortOrders() : PassME("TopologicalSortOrders", kNoNodes) {
  }

  bool Gate(const PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* c_unit = down_cast<const PassMEDataHolder*>(data)->c_unit;
    DCHECK(c_unit != nullptr);
    return !c_unit->mir_graph->TopologicalOrderUpToDate();
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
class DefBlockMatrix : public PassMEMirSsaRep {
 public:
  DefBlockMatrix() : PassMEMirSsaRep("DefBlockMatrix", kNoNodes) {
  }

  void Start(PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* c_unit = down_cast<PassMEDataHolder*>(data)->c_unit;
    DCHECK(c_unit != nullptr);
    c_unit->mir_graph.get()->ComputeDefBlockMatrix();
  }
};

/**
 * @class FindPhiNodeBlocksPass
 * @brief Pass to find out where we need to insert the phi nodes for the SSA conversion.
 */
class FindPhiNodeBlocksPass : public PassMEMirSsaRep {
 public:
  FindPhiNodeBlocksPass() : PassMEMirSsaRep("FindPhiNodeBlocks", kNoNodes) {
  }

  void Start(PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* c_unit = down_cast<PassMEDataHolder*>(data)->c_unit;
    DCHECK(c_unit != nullptr);
    c_unit->mir_graph.get()->FindPhiNodeBlocks();
  }
};

/**
 * @class SSAConversion
 * @brief Pass for SSA conversion of MIRs
 */
class SSAConversion : public PassMEMirSsaRep {
 public:
  SSAConversion() : PassMEMirSsaRep("SSAConversion", kNoNodes) {
  }

  void Start(PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* c_unit = down_cast<PassMEDataHolder*>(data)->c_unit;
    DCHECK(c_unit != nullptr);
    MIRGraph *mir_graph = c_unit->mir_graph.get();
    mir_graph->ClearAllVisitedFlags();
    mir_graph->DoDFSPreOrderSSARename(mir_graph->GetEntryBlock());
  }
};

/**
 * @class PhiNodeOperands
 * @brief Pass to insert the Phi node operands to basic blocks
 */
class PhiNodeOperands : public PassMEMirSsaRep {
 public:
  PhiNodeOperands() : PassMEMirSsaRep("PhiNodeOperands", kPreOrderDFSTraversal) {
  }

  bool Worker(PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* c_unit = down_cast<PassMEDataHolder*>(data)->c_unit;
    DCHECK(c_unit != nullptr);
    BasicBlock* bb = down_cast<PassMEDataHolder*>(data)->bb;
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
class PerformInitRegLocations : public PassMEMirSsaRep {
 public:
  PerformInitRegLocations() : PassMEMirSsaRep("PerformInitRegLocation", kNoNodes) {
  }

  void Start(PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* c_unit = down_cast<PassMEDataHolder*>(data)->c_unit;
    DCHECK(c_unit != nullptr);
    c_unit->mir_graph->InitRegLocations();
  }
};

/**
 * @class TypeInferencePass
 * @brief Type inference pass.
 */
class TypeInferencePass : public PassMEMirSsaRep {
 public:
  TypeInferencePass() : PassMEMirSsaRep("TypeInference", kRepeatingPreOrderDFSTraversal) {
  }

  void Start(PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* c_unit = down_cast<PassMEDataHolder*>(data)->c_unit;
    DCHECK(c_unit != nullptr);
    c_unit->mir_graph->InferTypesStart();
  }

  bool Worker(PassDataHolder* data) const {
    DCHECK(data != nullptr);
    PassMEDataHolder* pass_me_data_holder = down_cast<PassMEDataHolder*>(data);
    CompilationUnit* c_unit = pass_me_data_holder->c_unit;
    DCHECK(c_unit != nullptr);
    BasicBlock* bb = pass_me_data_holder->bb;
    DCHECK(bb != nullptr);
    return c_unit->mir_graph->InferTypes(bb);
  }

  void End(PassDataHolder* data) const {
    DCHECK(data != nullptr);
    CompilationUnit* c_unit = down_cast<PassMEDataHolder*>(data)->c_unit;
    DCHECK(c_unit != nullptr);
    c_unit->mir_graph.get()->InferTypesEnd();
  }
};

/**
 * @class FinishSSATransformation
 * @brief There is some data that needs to be freed after performing the post optimization passes.
 */
class FinishSSATransformation : public PassMEMirSsaRep {
 public:
  FinishSSATransformation() : PassMEMirSsaRep("FinishSSATransformation", kNoNodes) {
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
