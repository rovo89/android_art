/*
 * Copyright (C) 2012 The Android Open Source Project
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

#include "register_allocator.h"
#include "lir_function.h"
#include "lir_frame_info.h"
#include "lir_reg.h"

#include "target_register_info.h"

#include "logging.h"
#include "stl_util.h"

namespace art {
namespace greenland {

void RegisterAllocator::BuildKillInfo(LIRFunction& lir_func) {
  //TODO: Build Register KillInfo
}

static LIRBasicBlock::iterator
GetSourceIterator(LIRBasicBlock& sourceBB, LIRBasicBlock& targetBB) {
  LIRBasicBlock::iterator it;
  for (it = sourceBB.back(); it != sourceBB.begin(); --it) {
    if (it->IsBranch() && (it->GetOperand(0).GetLabelTarget() == &targetBB))
      break;
  }
  return it;
}

void RegisterAllocator::PHIElimination(LIRFunction& lir_func) {
  for (LIRFunction::iterator bb = lir_func.begin(); bb != lir_func.end(); ++bb) {
    if (bb->IsEmpty() || !bb->front().IsPHI())
      continue;

    LIRBasicBlock::iterator it; // current LIR
    LIRBasicBlock::iterator next_it; // next LIR
    for(it = bb->begin(); it != bb->end(); it = next_it) {
      next_it = it; ++next_it;
      if (!it->IsPHI())
        break;

      for (unsigned i = 1; i < it->GetNumOperands(); i+=2) {
        const LIROperand& dst = it->GetOperand(0);
        const LIROperand& src = it->GetOperand(i);
        LIRBasicBlock* sourceBB =
            const_cast<LIRBasicBlock*> (it->GetOperand(i + 1).GetLabelTarget());

        LIR* lir = reg_info_.CreateCopy(lir_func, dst.GetReg(), src);
        LOG(INFO) << "PHIElimination: Insert COPY into BB: " << sourceBB->GetName();
        sourceBB->insert(GetSourceIterator(*sourceBB, *bb), lir);
      }

      bb->erase(it);
    }
  }
}

RegisterAllocator::Storage
RegisterAllocator::AllocateStorage(LIRFunction &lir_func, unsigned vreg_idx) {
  // The virtual register is allocated, return it.
  if (allocated_map_.find(vreg_idx) != allocated_map_.end()) {
    return allocated_map_[vreg_idx];
  }

  Storage s;
  // Have free register(s).
  if (!allocatable_list_.empty()) {
    s.regTag = kPhyRegType;
    s.index = allocatable_list_.front();
    allocatable_list_.pop_front();
  }
  else {
    s.regTag = kFrameType;
    if (stackstorage_list_.empty()) {
      // FIXME: Get register subword
      s.index = lir_func.GetFrameInfo().AllocateStackObject(4);
    }
    else {
      s.index = stackstorage_list_.front();
      stackstorage_list_.pop_front();
    }
  }
  allocated_map_[vreg_idx] = s;
  return s;
}

void RegisterAllocator::KeepStorage(const LIR& lir) {
  unsigned reg_value = lir.GetOperand(0).GetReg();
  int frame_idx =  lir.GetOperand(1).GetFrameIndex();

  if (!LIRReg::IsVirtualReg(reg_value))
    return;

  unsigned vreg_idx = LIRReg::GetRegNo(reg_value);

  LOG(INFO) << "VIRTUAL REG " << vreg_idx << " Keep in Stack Frame [" <<  frame_idx << "];";

  Storage s;
  s.regTag = kInStackType;
  s.index = frame_idx;
  allocated_map_[vreg_idx] = s;
}

void RegisterAllocator::FreeStorage(unsigned vreg_idx) {
  //TODO: Check vreg_idx must be allocated
  Storage s = allocated_map_[vreg_idx];

  if (s.regTag == kInStackType)
    return;

  if (s.regTag == kPhyRegType)
    allocatable_list_.push_front(s.index);
  else
    stackstorage_list_.push_front(s.index);

  allocated_map_.erase(vreg_idx);
}

void RegisterAllocator::InitializeAllocation(LIRFunction &lir_func) {
  allocatable_list_ = reg_info_.GetAllocatableList();
  stackstorage_list_.clear();
  allocated_map_.clear();
}

void RegisterAllocator::PreRegisterAllocation(LIRFunction &lir_func) {
  PHIElimination(lir_func);
}

void
RegisterAllocator::HandleInsnCopy(LIRBasicBlock &bb,
                                  LIRBasicBlock::iterator it,
                                  LIRBasicBlock::iterator next_it) {
  unsigned reg_dst = it->GetOperand(0).GetReg();
  unsigned reg_src = it->GetOperand(1).GetReg();

  Storage stor_dst = {-1, kNoneType};
  Storage stor_src = {-1, kNoneType};
  unsigned vidx_dst = 0, vidx_src = 0;

  if (LIRReg::IsVirtualReg(reg_dst)) {
    vidx_dst = LIRReg::GetRegNo(reg_dst);
    LOG(INFO) << "VIRTUAL REG " << vidx_dst << " in COPY need allocated !!";
    stor_dst = AllocateStorage(bb.GetParent(), vidx_dst);
  }

  if (LIRReg::IsVirtualReg(reg_src)) {
    vidx_src = LIRReg::GetRegNo(reg_src);
    LOG(INFO) << "VIRTUAL REG " << vidx_src << " in COPY need allocated !!";
    stor_src = AllocateStorage(bb.GetParent(), vidx_src);
  }

  if ((LIRReg::IsPhysicalReg(reg_dst) || (stor_dst.regTag == kPhyRegType))
      && (LIRReg::IsPhysicalReg(reg_src) || (stor_src.regTag == kPhyRegType))) {
    // MovRR
    unsigned idx_dst = (LIRReg::IsPhysicalReg(reg_dst)) ?
        LIRReg::GetRegNo(reg_dst) : stor_dst.index;
    unsigned idx_src = (LIRReg::IsPhysicalReg(reg_src)) ?
        LIRReg::GetRegNo(reg_src) : stor_src.index;
    LOG(INFO) << "\t [COPY] create MOVE: move " << idx_dst << ", "<< idx_src;
    LIR* lir = reg_info_.CreateMoveReg(bb.GetParent(), idx_dst, idx_src);
    bb.insert(next_it, lir);
    bb.erase(it);

    if(LIRReg::IsVirtualReg(reg_src)) {
      FreeStorage(vidx_src);
    }
    return;
  }

  if ((stor_dst.regTag == stor_src.regTag) && (stor_dst.index == stor_src.index)) {
    // Redundant move
    FreeStorage(vidx_dst);
    FreeStorage(vidx_src);
    bb.erase(it);
    return;
  }

  if (stor_dst.regTag != kPhyRegType && stor_src.regTag != kPhyRegType) {
    unsigned idx = reg_info_.GetTempRegsiter(0);
    LIR* lir = reg_info_.CreateLoadStack(bb.GetParent(), idx, stor_src.index);
    bb.insert(it, lir);
    lir = reg_info_.CreateStoreStack(bb.GetParent(), idx, stor_dst.index);
    bb.insert(next_it, lir);
    bb.erase(it);
    return;
  }

  if (stor_dst.regTag != kPhyRegType) {
    unsigned idx = (LIRReg::IsPhysicalReg(reg_src)) ?
        LIRReg::GetRegNo(reg_src) : stor_src.index;
    LOG(INFO) << "\t [COPY] create StoreStack: move " << idx << ", "<< stor_dst.index;
    LIR* lir = reg_info_.CreateStoreStack(bb.GetParent(), idx, stor_dst.index);
    bb.insert(next_it, lir);
    bb.erase(it);
    return;
  }

  if (stor_src.regTag != kPhyRegType) {
    unsigned idx = (LIRReg::IsPhysicalReg(reg_dst)) ?
        LIRReg::GetRegNo(reg_dst) : stor_dst.index;
    LOG(INFO) << "\t [COPY] create LoadStack: move " << idx << ", "<< stor_src.index;
    LIR* lir = reg_info_.CreateLoadStack(bb.GetParent(), idx, stor_src.index);
    bb.insert(next_it, lir);
    bb.erase(it);
    return;
  }
}

void RegisterAllocator::FunctionRegisterAllocation(LIRFunction &lir_func) {
  for (LIRFunction::iterator bb =  lir_func.begin(); bb != lir_func.end(); ++bb) {
    LIRBasicBlock::iterator it; // current LIR
    LIRBasicBlock::iterator next_it; // next LIR
    for (LIRBasicBlock::iterator it = bb->begin(); it != bb->end(); it = next_it) {
      next_it = it; ++next_it;

      // Handle Incoming Args
      if (reg_info_.IsLoadIncomingArgs(it)) {
        KeepStorage(*it);
        bb->erase(it);
        continue;
      }

      // Handle Copy
      if (it->GetOpcode() == opcode::kCOPY) {
        HandleInsnCopy(*bb, *it, *next_it);
        continue;
      }

      std::vector<unsigned> free_list;
      //for (LIR::op_iterator opi = i->operands_begin(); opi != i->operands_end(); ++opi) {
      for (unsigned i = 0 ; i < it->GetNumOperands(); i++) {
        LIROperand& lir_opd = it->GetOperand(i);
        unsigned temp_count = 0;

        if (!lir_opd.IsReg())
          continue;

        unsigned reg_value = lir_opd.GetReg();
        if (!LIRReg::IsVirtualReg(reg_value))
          continue;
        unsigned vreg_idx = LIRReg::GetRegNo(reg_value);
        LOG(INFO) << "\t" << "VIRTUAL REG " << vreg_idx << " need allocated !!";
        Storage s = AllocateStorage(lir_func, vreg_idx);
        if (!s.regTag) {
          LOG(INFO) << "\t" << "VIRTUAL REG " << vreg_idx << " is map to REG[" << s.index << "]!!";
          lir_opd.SetReg(s.index);
        } else {
          unsigned temp_idx = reg_info_.GetTempRegsiter(temp_count++);
          if ( lir_opd.IsDef() ) {
            LIR* lir = reg_info_.CreateStoreStack(lir_func, temp_idx, s.index);
            bb->insert(next_it, lir);
            lir_opd.SetReg(temp_idx);
          } else {
            LIR* lir = reg_info_.CreateLoadStack(lir_func, temp_idx, s.index);
            bb->insert(it, lir);
            lir_opd.SetReg(temp_idx);
          }

          LOG(INFO) << "\t" << "VIRTUAL REG " << vreg_idx << " is map to Frame[" << s.index << "]!!";
        }

        if (lir_opd.IsKill()) {
          free_list.push_back(vreg_idx);
          LOG(INFO) << "\t" << "VIRTUAL REG " << vreg_idx << " is mark kill!!";
        }
      }

      for (size_t i = 0 ; i < free_list.size(); i++) {
        LOG(INFO) << "\t" << "VIRTUAL REG " << free_list[i] << " is kill!!";
        FreeStorage(free_list[i]);
      }
    }
  }
}

} // namespace greenland
} // namespace art
