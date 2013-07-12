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

#include "sea.h"

#include "file_output_stream.h"

#define MAX_REACHING_DEF_ITERERATIONS (10)

namespace sea_ir {

SeaGraph SeaGraph::graph_;
int SeaNode::current_max_node_id_ = 0;


SeaGraph* SeaGraph::GetCurrentGraph() {
  return &sea_ir::SeaGraph::graph_;
}

void SeaGraph::DumpSea(std::string filename) const {
  std::string result;
  result += "digraph seaOfNodes {\n";
  for (std::vector<Region*>::const_iterator cit = regions_.begin(); cit != regions_.end(); cit++) {
    (*cit)->ToDot(result);
  }
  result += "}\n";
  art::File* file = art::OS::OpenFile(filename.c_str(), true, true);
  art::FileOutputStream fos(file);
  fos.WriteFully(result.c_str(), result.size());
  LOG(INFO) << "Written SEA string to file.";
}

void SeaGraph::AddEdge(Region* src, Region* dst) const {
  src->AddSuccessor(dst);
  dst->AddPredecessor(src);
}

void SeaGraph::ComputeDownExposedDefs() {
  for (std::vector<Region*>::iterator region_it = regions_.begin();
        region_it != regions_.end(); region_it++) {
      (*region_it)->ComputeDownExposedDefs();
    }
}

void SeaGraph::ComputeReachingDefs() {
  // Iterate until the reaching definitions set doesn't change anymore.
  // (See Cooper & Torczon, "Engineering a Compiler", second edition, page 487)
  bool changed = true;
  int iteration = 0;
  while (changed && (iteration < MAX_REACHING_DEF_ITERERATIONS)) {
    iteration++;
    changed = false;
    // TODO: optimize the ordering if this becomes performance bottleneck.
    for (std::vector<Region*>::iterator regions_it = regions_.begin();
        regions_it != regions_.end();
        regions_it++) {
      changed |= (*regions_it)->UpdateReachingDefs();
    }
  }
  DCHECK(!changed) << "Reaching definitions computation did not reach a fixed point.";
}


void SeaGraph::CompileMethod(const art::DexFile::CodeItem* code_item,
  uint32_t class_def_idx, uint32_t method_idx, const art::DexFile& dex_file) {
  const uint16_t* code = code_item->insns_;
  const size_t size_in_code_units = code_item->insns_size_in_code_units_;

  Region* r = NULL;
  // This maps  target instruction pointers to their corresponding region objects.
  std::map<const uint16_t*, Region*> target_regions;
  size_t i = 0;

  // Pass: Find the start instruction of basic blocks
  //         by locating targets and flow-though instructions of branches.
  while (i < size_in_code_units) {
    const art::Instruction* inst = art::Instruction::At(&code[i]);
    if (inst->IsBranch()||inst->IsUnconditional()) {
      int32_t offset = inst->GetTargetOffset();
      if (target_regions.end() == target_regions.find(&code[i+offset])) {
        Region* region = GetNewRegion();
        target_regions.insert(std::pair<const uint16_t*, Region*>(&code[i+offset], region));
      }
      if (inst->CanFlowThrough() &&
          (target_regions.end() == target_regions.find(&code[i+inst->SizeInCodeUnits()]))) {
        Region* region = GetNewRegion();
        target_regions.insert(std::pair<const uint16_t*, Region*>(&code[i+inst->SizeInCodeUnits()], region));
      }
    }
    i += inst->SizeInCodeUnits();
  }

  // Pass: Assign instructions to region nodes and
  //         assign branches their control flow successors.
  i = 0;
  r = GetNewRegion();
  sea_ir::InstructionNode* last_node = NULL;
  sea_ir::InstructionNode* node = NULL;
  while (i < size_in_code_units) {
    const art::Instruction* inst = art::Instruction::At(&code[i]); //TODO: find workaround for this
    last_node = node;
    node = new sea_ir::InstructionNode(inst);

    if (inst->IsBranch() || inst->IsUnconditional()) {
      int32_t offset = inst->GetTargetOffset();
      std::map<const uint16_t*, Region*>::iterator it = target_regions.find(&code[i+offset]);
      DCHECK(it != target_regions.end());
      AddEdge(r, it->second); // Add edge to branch target.
    }

    std::map<const uint16_t*, Region*>::iterator it = target_regions.find(&code[i]);
    if (target_regions.end() != it) {
      // Get the already created region because this is a branch target.
      Region* nextRegion = it->second;
      if (last_node->GetInstruction()->IsBranch() && last_node->GetInstruction()->CanFlowThrough()) {
        AddEdge(r, it->second); // Add flow-through edge.
      }
      r = nextRegion;
    }
    bool definesRegister = (0 !=
            InstructionTools::instruction_attributes_[inst->Opcode()] && (1 << kDA));
    LOG(INFO) << inst->GetDexPc(code) << "*** " << inst->DumpString(&dex_file)
            << " region:" <<r->StringId() << "Definition?" << definesRegister << std::endl;
    r->AddChild(node);
    i += inst->SizeInCodeUnits();
  }

  // Pass: compute downward-exposed definitions.
  ComputeDownExposedDefs();

  // Multiple Passes: Compute reaching definitions (iterative fixed-point algorithm)
  ComputeReachingDefs();
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

void Region::AddChild(sea_ir::InstructionNode* instruction) {
  DCHECK(instruction) << "Tried to add NULL instruction to region node.";
  instructions_.push_back(instruction);
}

SeaNode* Region::GetLastChild() const {
  if (instructions_.size() > 0) {
    return instructions_.back();
  }
  return NULL;
}

void InstructionNode::ToDot(std::string& result) const {
  result += "// Instruction: \n" + StringId() +
      " [label=\"" + instruction_->DumpString(NULL) + "\"";
  if (de_def_) {
    result += "style=bold";
  }
  result += "];\n";
}

int InstructionNode::GetResultRegister() const {
  if (!InstructionTools::IsDefinition(instruction_)) {
    return NO_REGISTER;
  }
  return instruction_->VRegA();
}

void InstructionNode::MarkAsDEDef() {
  de_def_ = true;
}

void Region::ToDot(std::string& result) const {
  result += "\n// Region: \n" + StringId() + " [label=\"region " + StringId() + "\"];";
  // Save instruction nodes that belong to this region.
  for (std::vector<InstructionNode*>::const_iterator cit = instructions_.begin();
      cit != instructions_.end(); cit++) {
    (*cit)->ToDot(result);
    result += StringId() + " -> " + (*cit)->StringId() + ";\n";
  }

  for (std::vector<Region*>::const_iterator cit = successors_.begin(); cit != successors_.end();
      cit++) {
    DCHECK(NULL != *cit) << "Null successor found for SeaNode" << GetLastChild()->StringId() << ".";
    result += GetLastChild()->StringId() + " -> " + (*cit)->StringId() + ";\n\n";
  }

  // Save reaching definitions.
  for (std::map<int, std::set<sea_ir::InstructionNode*>* >::const_iterator cit =
      reaching_defs_.begin();
      cit != reaching_defs_.end(); cit++) {
    for (std::set<sea_ir::InstructionNode*>::const_iterator
        reaching_set_it = (*cit).second->begin();
        reaching_set_it != (*cit).second->end();
        reaching_set_it++) {
      result += (*reaching_set_it)->StringId() +
         " -> " + StringId() +
         " [style=dotted]; // Reaching def.\n";
    }
  }

  result += "// End Region.\n";
}


void Region::ComputeDownExposedDefs() {
  for (std::vector<InstructionNode*>::const_iterator inst_it = instructions_.begin();
      inst_it != instructions_.end(); inst_it++) {
    int reg_no = (*inst_it)->GetResultRegister();
    std::map<int, InstructionNode*>::iterator res = de_defs_.find(reg_no);
    if ((reg_no != NO_REGISTER) && (res == de_defs_.end())) {
      de_defs_.insert(std::pair<int, InstructionNode*>(reg_no, *inst_it));
    } else {
      res->second = *inst_it;
    }
  }

  for (std::map<int, sea_ir::InstructionNode*>::const_iterator cit = de_defs_.begin();
      cit != de_defs_.end(); cit++) {
    (*cit).second->MarkAsDEDef();
  }
}


const std::map<int, sea_ir::InstructionNode*>* Region::GetDownExposedDefs() const {
  return &de_defs_;
}

std::map<int, std::set<sea_ir::InstructionNode*>* >* Region::GetReachingDefs() {
  return &reaching_defs_;
}

bool Region::UpdateReachingDefs() {
  std::map<int, std::set<sea_ir::InstructionNode*>* > new_reaching;
  for (std::vector<Region*>::const_iterator pred_it = predecessors_.begin();
      pred_it != predecessors_.end(); pred_it++) {
    // The reaching_defs variable will contain reaching defs __for current predecessor only__
    std::map<int, std::set<sea_ir::InstructionNode*>* > reaching_defs;
    std::map<int, std::set<sea_ir::InstructionNode*>* >* pred_reaching = (*pred_it)->GetReachingDefs();
    const std::map<int, InstructionNode*>* de_defs = (*pred_it)->GetDownExposedDefs();

    // The definitions from the reaching set of the predecessor
    // may be shadowed by downward exposed definitions from the predecessor,
    // otherwise the defs from the reaching set are still good.
    for (std::map<int, InstructionNode*>::const_iterator de_def = de_defs->begin();
        de_def != de_defs->end(); de_def++) {
      std::set<InstructionNode*>* solo_def;
      solo_def = new std::set<InstructionNode*>();
      solo_def->insert(de_def->second);
      reaching_defs.insert(
          std::pair<int const, std::set<InstructionNode*>*>(de_def->first, solo_def));
    }
    LOG(INFO) << "Adding to " <<StringId() << "reaching set of " << (*pred_it)->StringId();
    reaching_defs.insert(pred_reaching->begin(), pred_reaching->end());

    // Now we combine the reaching map coming from the current predecessor (reaching_defs)
    // with the accumulated set from all predecessors so far (from new_reaching).
    std::map<int, std::set<sea_ir::InstructionNode*>*>::iterator reaching_it = reaching_defs.begin();
    for (; reaching_it != reaching_defs.end(); reaching_it++) {
      std::map<int, std::set<sea_ir::InstructionNode*>*>::iterator crt_entry =
          new_reaching.find(reaching_it->first);
      if (new_reaching.end() != crt_entry) {
        crt_entry->second->insert(reaching_it->second->begin(), reaching_it->second->end());
      } else {
        new_reaching.insert(
            std::pair<int, std::set<sea_ir::InstructionNode*>*>(
                reaching_it->first,
                reaching_it->second) );
      }
    }
  }
  bool changed = false;
  // Because the sets are monotonically increasing,
  // we can compare sizes instead of using set comparison.
  // TODO: Find formal proof.
  int old_size = 0;
  if (-1 == reaching_defs_size_) {
    std::map<int, std::set<sea_ir::InstructionNode*>*>::iterator reaching_it = reaching_defs_.begin();
    for (; reaching_it != reaching_defs_.end(); reaching_it++) {
      old_size += (*reaching_it).second->size();
    }
  } else {
    old_size = reaching_defs_size_;
  }
  int new_size = 0;
  std::map<int, std::set<sea_ir::InstructionNode*>*>::iterator reaching_it = new_reaching.begin();
  for (; reaching_it != new_reaching.end(); reaching_it++) {
    new_size += (*reaching_it).second->size();
  }
  if (old_size != new_size) {
    changed = true;
  }
  if (changed) {
    reaching_defs_ = new_reaching;
    reaching_defs_size_ = new_size;
  }
  return changed;
}

void SeaNode::AddSuccessor(Region* successor) {
  DCHECK(successor) << "Tried to add NULL successor to SEA node.";
  successors_.push_back(successor);
  return;
}

void SeaNode::AddPredecessor(Region* predecessor) {
  DCHECK(predecessor) << "Tried to add NULL predecessor to SEA node.";
  predecessors_.push_back(predecessor);
}

} // end namespace sea_ir
