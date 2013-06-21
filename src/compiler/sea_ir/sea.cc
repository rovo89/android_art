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

#include "compiler/sea_ir/sea.h"
#include "file_output_stream.h"



namespace sea_ir {


SeaGraph SeaGraph::graph_;
int SeaNode::current_max_node_id_ = 0;

SeaGraph* SeaGraph::GetCurrentGraph() {
  return &sea_ir::SeaGraph::graph_;
}

void SeaGraph::DumpSea(std::string filename) const {
  std::string result;
  result += "digraph seaOfNodes {\n";
  for(std::vector<Region*>::const_iterator cit = regions_.begin(); cit != regions_.end(); cit++) {
    result += (*cit)->ToDot();
  }
  result += "}\n";
  art::File* file = art::OS::OpenFile(filename.c_str(), true, true);
  art::FileOutputStream fos(file);
  fos.WriteFully(result.c_str(), result.size());
  LOG(INFO) << "Written SEA string to file...";
}

void SeaGraph::CompileMethod(const art::DexFile::CodeItem* code_item,
  uint32_t class_def_idx, uint32_t method_idx, const art::DexFile& dex_file) {
  const uint16_t* code = code_item->insns_;
  const size_t size_in_code_units = code_item->insns_size_in_code_units_;

  Region* r = NULL;
  // This maps  target instruction pointers to their corresponding region objects.
  std::map<const uint16_t*, Region*> target_regions;
  size_t i = 0;

  // Pass 1: Find the start instruction of basic blocks, as targets and flow-though of branches.
  while (i < size_in_code_units) {
    const art::Instruction* inst = art::Instruction::At(&code[i]);
    if (inst->IsBranch()||inst->IsUnconditional()) {
      int32_t offset = inst->GetTargetOffset();
      if (target_regions.end() == target_regions.find(&code[i+offset])) {
        Region* region = GetNewRegion();
        target_regions.insert(std::pair<const uint16_t*, Region*>(&code[i+offset], region));
      }
      if (inst->IsFlowthrough() &&
          (target_regions.end() == target_regions.find(&code[i+inst->SizeInCodeUnits()]))) {
        Region* region = GetNewRegion();
        target_regions.insert(std::pair<const uint16_t*, Region*>(&code[i+inst->SizeInCodeUnits()], region));
      }
    }
    i += inst->SizeInCodeUnits();
  }


  // Pass 2: Assign instructions to region nodes and
  //         assign branches their control flow successors.
  i = 0;
  r = GetNewRegion();
  sea_ir::SeaNode* last_node = NULL;
  sea_ir::SeaNode* node = NULL;
  while (i < size_in_code_units) {
    const art::Instruction* inst = art::Instruction::At(&code[i]); //TODO: find workaround for this
    last_node = node;
    node = new sea_ir::SeaNode(inst);

    if (inst->IsBranch() || inst->IsUnconditional()) {
      int32_t offset = inst->GetTargetOffset();
      std::map<const uint16_t*, Region*>::iterator it = target_regions.find(&code[i+offset]);
      DCHECK(it != target_regions.end());
      node->AddSuccessor(it->second);
    }

    std::map<const uint16_t*, Region*>::iterator it = target_regions.find(&code[i]);
    if (target_regions.end() != it) {
      // Get the already created region because this is a branch target.
      Region* nextRegion = it->second;
      if (last_node->GetInstruction()->IsBranch() && last_node->GetInstruction()->IsFlowthrough()) {
        last_node->AddSuccessor(nextRegion);

      }
      r = nextRegion;
    }

    LOG(INFO) << inst->GetDexPc(code) << "*** " << inst->DumpString(&dex_file)
            << " region:" <<r->StringId() << std::endl;
    r->AddChild(node);
    i += inst->SizeInCodeUnits();
  }

}


Region* SeaGraph::GetNewRegion() {
  Region* new_region = new Region();
  AddRegion(new_region);
  return new_region;
}

void SeaGraph::AddRegion(Region* r) {
  DCHECK(r) << "Tried to add NULL region to SEA graph.";
  regions_.push_back(r);
}
void Region::AddChild(sea_ir::SeaNode* instruction) {
  DCHECK(inst) << "Tried to add NULL instruction to region node.";
  instructions_.push_back(instruction);
}

SeaNode* Region::GetLastChild() const {
  if (instructions_.size()>0) {
    return instructions_.back();
  }
  return NULL;
}

std::string SeaNode::ToDot() const {
  std::string node = "// Instruction: \n" + StringId() +
      " [label=\"" + instruction_->DumpString(NULL) + "\"];\n";

  for(std::vector<SeaNode*>::const_iterator cit = successors_.begin();
      cit != successors_.end(); cit++) {
    DCHECK(NULL != *cit) << "Null successor found for SeaNode" << StringId() << ".";
    node += StringId() + " -> " + (*cit)->StringId() + ";\n\n";
  }
  return node;
}

std::string SeaNode::StringId() const {
  std::stringstream ss;
  ss << id_;
  return ss.str();
}

std::string Region::ToDot() const {
  std::string result = "// Region: \n" +
      StringId() + " [label=\"region " + StringId() + "\"];";

  for(std::vector<SeaNode*>::const_iterator cit = instructions_.begin();
      cit != instructions_.end(); cit++) {
    result += (*cit)->ToDot();
    result += StringId() + " -> " + (*cit)->StringId() + ";\n";
  }

  result += "// End Region.\n";
  return result;
}

void SeaNode::AddSuccessor(SeaNode* successor) {
  DCHECK(successor) << "Tried to add NULL successor to SEA node.";
  successors_.push_back(successor);
  return;
}

} // end namespace
