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


#ifndef ART_COMPILER_SEA_IR_SEA_H_
#define ART_COMPILER_SEA_IR_SEA_H_

#include <set>
#include <map>

#include "dex_file.h"
#include "dex_instruction.h"
#include "sea_ir/instruction_tools.h"
#include "utils/scoped_hashtable.h"


namespace sea_ir {

#define NO_REGISTER             (-1)

// Reverse post-order numbering constants
enum RegionNumbering {
  NOT_VISITED = -1,
  VISITING = -2
};

class Region;
class InstructionNode;
class PhiInstructionNode;

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

  std::vector<sea_ir::Region*>* GetSuccessors() {
    return &successors_;
  }
  std::vector<sea_ir::Region*>* GetPredecessors() {
    return &predecessors_;
  }
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
  explicit InstructionNode(const art::Instruction* in):
    SeaNode(), instruction_(in), de_def_(false) {}
  // Returns the Dalvik instruction around which this InstructionNode is wrapped.
  const art::Instruction* GetInstruction() const {
    DCHECK(NULL != instruction_) << "Tried to access NULL instruction in an InstructionNode.";
    return instruction_;
  }
  // Returns the register that is defined by the current instruction, or NO_REGISTER otherwise.
  virtual int GetResultRegister() const;
  // Returns the set of registers defined by the current instruction.
  virtual std::vector<int> GetDefinitions() const;
  // Returns the set of register numbers that are used by the instruction.
  virtual std::vector<int> GetUses();
  // Appends to @result the .dot string representation of the instruction.
  void ToDot(std::string& result) const;
  // Mark the current instruction as a dowward exposed definition.
  void MarkAsDEDef();
  // Rename the use of @reg_no to refer to the instruction @definition,
  // essentially creating SSA form.
  void RenameToSSA(int reg_no, InstructionNode* definition) {
    definition_edges_.insert(std::pair<int, InstructionNode*>(reg_no, definition));
  }

 private:
  const art::Instruction* const instruction_;
  std::map<int, InstructionNode* > definition_edges_;
  bool de_def_;
};

class SignatureNode: public InstructionNode {
 public:
  explicit SignatureNode(unsigned int start_register, unsigned int count):
      InstructionNode(NULL), defined_regs_() {
    for (unsigned int crt_offset = 0; crt_offset < count; crt_offset++) {
      defined_regs_.push_back(start_register - crt_offset);
    }
  }

  void ToDot(std::string& result) const {
    result += StringId() +"[label=\"signature:";
    std::stringstream vector_printer;
    if (!defined_regs_.empty()) {
      for (unsigned int crt_el = 0; crt_el < defined_regs_.size()-1; crt_el++) {
        vector_printer << defined_regs_[crt_el] <<",";
      }
      vector_printer << defined_regs_[defined_regs_.size()-1] <<";";
    }
    result += "\"] // signature node\n";
  }

  std::vector<int> GetDefinitions() const {
    return defined_regs_;
  }

  int GetResultRegister() const {
    return NO_REGISTER;
  }

  std::vector<int> GetUses() {
    return std::vector<int>();
  }

 private:
  std::vector<int> defined_regs_;
};


class PhiInstructionNode: public InstructionNode {
 public:
  explicit PhiInstructionNode(int register_no):
    InstructionNode(NULL), register_no_(register_no), definition_edges_() {}
  // Appends to @result the .dot string representation of the instruction.
  void ToDot(std::string& result) const;
  // Returns the register on which this phi-function is used.
  int GetRegisterNumber() {
    return register_no_;
  }

  // Rename the use of @reg_no to refer to the instruction @definition.
  // Phi-functions are different than normal instructions in that they
  // have multiple predecessor regions; this is why RenameToSSA has
  // the additional parameter specifying that @parameter_id is the incoming
  // edge for @definition, essentially creating SSA form.
  void RenameToSSA(int reg_no, InstructionNode* definition, unsigned int predecessor_id) {
    DCHECK(NULL != definition) << "Tried to rename to SSA using a NULL definition for "
        << StringId() << " register " << reg_no;
    if (definition_edges_.size() < predecessor_id+1) {
      definition_edges_.resize(predecessor_id+1, NULL);
    }

    if (NULL == definition_edges_.at(predecessor_id)) {
      definition_edges_[predecessor_id] = new std::map<int, InstructionNode*>();
    }
    definition_edges_[predecessor_id]->insert(std::pair<int, InstructionNode*>(reg_no, definition));
  }

 private:
  int register_no_;
  std::vector<std::map<int, InstructionNode*>*> definition_edges_;
};

class Region : public SeaNode {
 public:
  explicit Region():
    SeaNode(), reaching_defs_size_(0), rpo_(NOT_VISITED), idom_(NULL),
    idominated_set_(), df_(), phi_set_() {}

  // Adds @instruction as an instruction node child in the current region.
  void AddChild(sea_ir::InstructionNode* insttruction);

  // Returns the last instruction node child of the current region.
  // This child has the CFG successors pointing to the new regions.
  SeaNode* GetLastChild() const;
  // Returns all the child instructions of this region, in program order.
  std::vector<InstructionNode*>* GetInstructions() {
    return &instructions_;
  }
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

  void SetRPO(int rpo) {
    rpo_ = rpo;
  }

  int GetRPO() {
    return rpo_;
  }

  void SetIDominator(Region* dom) {
    idom_ = dom;
  }

  Region* GetIDominator() const {
    return idom_;
  }

  void AddToIDominatedSet(Region* dominated) {
    idominated_set_.insert(dominated);
  }

  const std::set<Region*>* GetIDominatedSet() {
    return &idominated_set_;
  }

  // Adds @df_reg to the dominance frontier of the current region.
  void AddToDominanceFrontier(Region* df_reg) {
    df_.insert(df_reg);
  }
  // Returns the dominance frontier of the current region.
  // Preconditions: SeaGraph.ComputeDominanceFrontier()
  std::set<Region*>* GetDominanceFrontier() {
    return &df_;
  }
  // Returns true if the region contains a phi function for @reg_no.
  bool ContainsPhiFor(int reg_no) {
    return (phi_set_.end() != phi_set_.find(reg_no));
  }
  // Returns the phi-functions from the region.
  std::vector<PhiInstructionNode*>* GetPhiNodes() {
    return &phi_instructions_;
  }
  // Adds a phi-function for @reg_no to this region.
  // Note: The insertion order does not matter, as phi-functions
  //       are conceptually executed at the same time.
  bool InsertPhiFor(int reg_no);
  // Sets the phi-function uses to be as defined in @scoped_table for predecessor @@predecessor.
  void SetPhiDefinitionsForUses(const utils::ScopedHashtable<int, InstructionNode*>* scoped_table,
      Region* predecessor);

 private:
  std::vector<sea_ir::InstructionNode*> instructions_;
  std::map<int, sea_ir::InstructionNode*> de_defs_;
  std::map<int, std::set<sea_ir::InstructionNode*>* > reaching_defs_;
  int reaching_defs_size_;
  int rpo_;
  // Immediate dominator node.
  Region* idom_;
  // The set of nodes immediately dominated by the region.
  std::set<Region*> idominated_set_;
  // Records the dominance frontier.
  std::set<Region*> df_;
  // Records the set of register numbers that have phi nodes in this region.
  std::set<int> phi_set_;
  std::vector<PhiInstructionNode*> phi_instructions_;
};

class SeaGraph {
 public:
  static SeaGraph* GetCurrentGraph();

  void CompileMethod(const art::DexFile::CodeItem* code_item,
      uint32_t class_def_idx, uint32_t method_idx, const art::DexFile& dex_file);
  // Returns a string representation of the region and its Instruction children.
  void DumpSea(std::string filename) const;
  // Recursively computes the reverse postorder value for @crt_bb and successors.
  static void ComputeRPO(Region* crt_bb, int& crt_rpo);
  // Returns the "lowest common ancestor" of @i and @j in the dominator tree.
  static Region* Intersect(Region* i, Region* j);

 private:
  // Registers @childReg as a region belonging to the SeaGraph instance.
  void AddRegion(Region* childReg);
  // Returns new region and registers it with the  SeaGraph instance.
  Region* GetNewRegion();
  // Adds a CFG edge from @src node to @dst node.
  void AddEdge(Region* src, Region* dst) const;
  // Builds the non-SSA sea-ir representation of the function @code_item from @dex_file.
  void BuildMethodSeaGraph(const art::DexFile::CodeItem* code_item, const art::DexFile& dex_file);
  // Computes immediate dominators for each region.
  // Precondition: ComputeMethodSeaGraph()
  void ComputeIDominators();
  // Computes Downward Exposed Definitions for all regions in the graph.
  void ComputeDownExposedDefs();
  // Computes the reaching definitions set following the equations from
  // Cooper & Torczon, "Engineering a Compiler", second edition, page 491.
  // Precondition: ComputeDEDefs()
  void ComputeReachingDefs();
  // Computes the reverse-postorder numbering for the region nodes.
  // Precondition: ComputeDEDefs()
  void ComputeRPO();
  // Computes the dominance frontier for all regions in the graph,
  // following the algorithm from
  // Cooper & Torczon, "Engineering a Compiler", second edition, page 499.
  // Precondition: ComputeIDominators()
  void ComputeDominanceFrontier();

  void ConvertToSSA();
  // Identifies the definitions corresponding to uses for region @node
  // by using the scoped hashtable of names @ scoped_table.
  void RenameAsSSA(Region* node, utils::ScopedHashtable<int, InstructionNode*>* scoped_table);
  void RenameAsSSA();

  static SeaGraph graph_;
  std::vector<Region*> regions_;
};
} // end namespace sea_ir
#endif  // ART_COMPILER_SEA_IR_SEA_H_
