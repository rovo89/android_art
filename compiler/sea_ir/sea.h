/*
 * Copyright (C) 2013 The Android Open Source Project
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


#ifndef SEA_IR_H_
#define SEA_IR_H_

#include <set>
#include <map>

#include "dex_file.h"
#include "dex_instruction.h"
#include "sea_ir/instruction_tools.h"

#define NO_REGISTER       (-1)

namespace sea_ir {
class Region;

class SeaNode {
 public:
  explicit SeaNode():id_(GetNewId()), string_id_(), successors_(), predecessors_() {
    std::stringstream ss;
    ss << id_;
    string_id_.append(ss.str());
  }

  // Adds CFG predecessors and successors to each block.
  void AddSuccessor(Region* successor);
  void AddPredecessor(Region* predecesor);

  // Returns the id of the current block as string
  const std::string& StringId() const {
    return string_id_;
  }

  // Appends to @result a dot language formatted string representing the node and
  //    (by convention) outgoing edges, so that the composition of theToDot() of all nodes
  //    builds a complete dot graph, but without prolog ("digraph {") and epilog ("}").
  virtual void ToDot(std::string& result) const = 0;

  virtual ~SeaNode() {}

 protected:
  static int GetNewId() {
    return current_max_node_id_++;
  }

  const int id_;
  std::string string_id_;
  std::vector<sea_ir::Region*> successors_;    // CFG successor nodes (regions)
  std::vector<sea_ir::Region*> predecessors_;  // CFG predecessor nodes (instructions/regions)

 private:
  static int current_max_node_id_;
};

class InstructionNode: public SeaNode {
 public:
  explicit InstructionNode(const art::Instruction* in):SeaNode(), instruction_(in), de_def_(false) {}

  const art::Instruction* GetInstruction() const {
    DCHECK(NULL != instruction_) << "Tried to access NULL instruction in an InstructionNode.";
    return instruction_;
  }
  // Returns the register that is defined by the current instruction, or NO_REGISTER otherwise.
  int GetResultRegister() const;
  void ToDot(std::string& result) const;
  void MarkAsDEDef();

 private:
  const art::Instruction* const instruction_;
  bool de_def_;
};



class Region : public SeaNode {
 public:
  explicit Region():SeaNode(), reaching_defs_size_(-1) {}

  // Adds @inst as an instruction node child in the current region.
  void AddChild(sea_ir::InstructionNode* inst);

  // Returns the last instruction node child of the current region.
  // This child has the CFG successors pointing to the new regions.
  SeaNode* GetLastChild() const;

  // Appends to @result a dot language formatted string representing the node and
  //    (by convention) outgoing edges, so that the composition of theToDot() of all nodes
  //    builds a complete dot graph (without prolog and epilog though).
  virtual void ToDot(std::string& result) const;

  // Computes Downward Exposed Definitions for the current node.
  void ComputeDownExposedDefs();
  const std::map<int, sea_ir::InstructionNode*>* GetDownExposedDefs() const;

  // Performs one iteration of the reaching definitions algorithm
  // and returns true if the reaching definitions set changed.
  bool UpdateReachingDefs();

  // Returns the set of reaching definitions for the current region.
  std::map<int, std::set<sea_ir::InstructionNode*>* >* GetReachingDefs();

 private:
  std::vector<sea_ir::InstructionNode*> instructions_;
  std::map<int, sea_ir::InstructionNode*> de_defs_;
  std::map<int, std::set<sea_ir::InstructionNode*>* > reaching_defs_;
  int reaching_defs_size_;
};



class SeaGraph {
 public:
  static SeaGraph* GetCurrentGraph();
  void CompileMethod(const art::DexFile::CodeItem* code_item,
      uint32_t class_def_idx, uint32_t method_idx, const art::DexFile& dex_file);

  // Returns a string representation of the region and its Instruction children
  void DumpSea(std::string filename) const;

  // Adds a CFG edge from @src node to @dst node.
  void AddEdge(Region* src, Region* dst) const;

  // Computes Downward Exposed Definitions for all regions in the graph.
  void ComputeDownExposedDefs();

  // Computes the reaching definitions set following the equations from
  // Cooper & Torczon, "Engineering a Compiler", second edition, page 491
  void ComputeReachingDefs();

  /*** Static helper functions follow: ***/
  static int ParseInstruction(const uint16_t* code_ptr,
      art::DecodedInstruction* decoded_instruction);
  static bool IsInstruction(const uint16_t* code_ptr);

 private:
  // Registers the parameter as a child region of the SeaGraph instance
  void AddRegion(Region* r);
  // Returns new region and registers it with the  SeaGraph instance
  Region* GetNewRegion();
  static SeaGraph graph_;
  std::vector<Region*> regions_;
};


} // end namespace sea_ir
#endif
