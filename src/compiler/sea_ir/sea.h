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

#include "dex_file.h"
#include "dex_instruction.h"

#ifndef SEA_IR_H_
#define SEA_IR_H_

#include <set>
#include <map>

namespace sea_ir {


class SeaNode {
 public:
  explicit SeaNode(const art::Instruction* in):id_(GetNewId()), instruction_(in), successors_() {};
  explicit SeaNode():id_(GetNewId()), instruction_(NULL) {};
  void AddSuccessor(SeaNode* successor);
  const art::Instruction* GetInstruction() {
    DCHECK(NULL != instruction_);
    return instruction_;
  }
  std::string StringId() const;
  // Returns a dot language formatted string representing the node and
  //    (by convention) outgoing edges, so that the composition of theToDot() of all nodes
  //    builds a complete dot graph (without prolog and epilog though).
  virtual std::string ToDot() const;
  virtual ~SeaNode(){};

 protected:
  // Returns the id of the current block as string

  static int GetNewId() {
    return current_max_node_id_++;
  }


 private:
  const int id_;
  const art::Instruction* const instruction_;
  std::vector<sea_ir::SeaNode*> successors_;
  static int current_max_node_id_;
};



class Region : public SeaNode {
 public:
  explicit Region():SeaNode() {}
  void AddChild(sea_ir::SeaNode* instruction);
  SeaNode* GetLastChild() const;

  // Returns a dot language formatted string representing the node and
  //    (by convention) outgoing edges, so that the composition of theToDot() of all nodes
  //    builds a complete dot graph (without prolog and epilog though).
  virtual std::string ToDot() const;

 private:
  std::vector<sea_ir::SeaNode*> instructions_;
};



class SeaGraph {
 public:
  static SeaGraph* GetCurrentGraph();
  void CompileMethod(const art::DexFile::CodeItem* code_item,
      uint32_t class_def_idx, uint32_t method_idx, const art::DexFile& dex_file);
  // Returns a string representation of the region and its Instruction children
  void DumpSea(std::string filename) const;
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
