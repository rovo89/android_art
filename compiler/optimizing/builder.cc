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

#include "builder.h"

#include "art_field-inl.h"
#include "base/logging.h"
#include "class_linker.h"
#include "dex/verified_method.h"
#include "dex_file-inl.h"
#include "dex_instruction-inl.h"
#include "dex/verified_method.h"
#include "driver/compiler_driver-inl.h"
#include "driver/compiler_options.h"
#include "mirror/class_loader.h"
#include "mirror/dex_cache.h"
#include "nodes.h"
#include "primitive.h"
#include "scoped_thread_state_change.h"
#include "thread.h"
#include "utils/dex_cache_arrays_layout-inl.h"

namespace art {

/**
 * Helper class to add HTemporary instructions. This class is used when
 * converting a DEX instruction to multiple HInstruction, and where those
 * instructions do not die at the following instruction, but instead spans
 * multiple instructions.
 */
class Temporaries : public ValueObject {
 public:
  explicit Temporaries(HGraph* graph) : graph_(graph), index_(0) {}

  void Add(HInstruction* instruction) {
    HInstruction* temp = new (graph_->GetArena()) HTemporary(index_, instruction->GetDexPc());
    instruction->GetBlock()->AddInstruction(temp);

    DCHECK(temp->GetPrevious() == instruction);

    size_t offset;
    if (instruction->GetType() == Primitive::kPrimLong
        || instruction->GetType() == Primitive::kPrimDouble) {
      offset = 2;
    } else {
      offset = 1;
    }
    index_ += offset;

    graph_->UpdateTemporariesVRegSlots(index_);
  }

 private:
  HGraph* const graph_;

  // Current index in the temporary stack, updated by `Add`.
  size_t index_;
};

class SwitchTable : public ValueObject {
 public:
  SwitchTable(const Instruction& instruction, uint32_t dex_pc, bool sparse)
      : instruction_(instruction), dex_pc_(dex_pc), sparse_(sparse) {
    int32_t table_offset = instruction.VRegB_31t();
    const uint16_t* table = reinterpret_cast<const uint16_t*>(&instruction) + table_offset;
    if (sparse) {
      CHECK_EQ(table[0], static_cast<uint16_t>(Instruction::kSparseSwitchSignature));
    } else {
      CHECK_EQ(table[0], static_cast<uint16_t>(Instruction::kPackedSwitchSignature));
    }
    num_entries_ = table[1];
    values_ = reinterpret_cast<const int32_t*>(&table[2]);
  }

  uint16_t GetNumEntries() const {
    return num_entries_;
  }

  void CheckIndex(size_t index) const {
    if (sparse_) {
      // In a sparse table, we have num_entries_ keys and num_entries_ values, in that order.
      DCHECK_LT(index, 2 * static_cast<size_t>(num_entries_));
    } else {
      // In a packed table, we have the starting key and num_entries_ values.
      DCHECK_LT(index, 1 + static_cast<size_t>(num_entries_));
    }
  }

  int32_t GetEntryAt(size_t index) const {
    CheckIndex(index);
    return values_[index];
  }

  uint32_t GetDexPcForIndex(size_t index) const {
    CheckIndex(index);
    return dex_pc_ +
        (reinterpret_cast<const int16_t*>(values_ + index) -
         reinterpret_cast<const int16_t*>(&instruction_));
  }

  // Index of the first value in the table.
  size_t GetFirstValueIndex() const {
    if (sparse_) {
      // In a sparse table, we have num_entries_ keys and num_entries_ values, in that order.
      return num_entries_;
    } else {
      // In a packed table, we have the starting key and num_entries_ values.
      return 1;
    }
  }

 private:
  const Instruction& instruction_;
  const uint32_t dex_pc_;

  // Whether this is a sparse-switch table (or a packed-switch one).
  const bool sparse_;

  // This can't be const as it needs to be computed off of the given instruction, and complicated
  // expressions in the initializer list seemed very ugly.
  uint16_t num_entries_;

  const int32_t* values_;

  DISALLOW_COPY_AND_ASSIGN(SwitchTable);
};

void HGraphBuilder::InitializeLocals(uint16_t count) {
  graph_->SetNumberOfVRegs(count);
  locals_.resize(count);
  for (int i = 0; i < count; i++) {
    HLocal* local = new (arena_) HLocal(i);
    entry_block_->AddInstruction(local);
    locals_[i] = local;
  }
}

void HGraphBuilder::InitializeParameters(uint16_t number_of_parameters) {
  // dex_compilation_unit_ is null only when unit testing.
  if (dex_compilation_unit_ == nullptr) {
    return;
  }

  graph_->SetNumberOfInVRegs(number_of_parameters);
  const char* shorty = dex_compilation_unit_->GetShorty();
  int locals_index = locals_.size() - number_of_parameters;
  int parameter_index = 0;

  const DexFile::MethodId& referrer_method_id =
      dex_file_->GetMethodId(dex_compilation_unit_->GetDexMethodIndex());
  if (!dex_compilation_unit_->IsStatic()) {
    // Add the implicit 'this' argument, not expressed in the signature.
    HParameterValue* parameter = new (arena_) HParameterValue(*dex_file_,
                                                              referrer_method_id.class_idx_,
                                                              parameter_index++,
                                                              Primitive::kPrimNot,
                                                              true);
    entry_block_->AddInstruction(parameter);
    HLocal* local = GetLocalAt(locals_index++);
    entry_block_->AddInstruction(new (arena_) HStoreLocal(local, parameter, local->GetDexPc()));
    number_of_parameters--;
  }

  const DexFile::ProtoId& proto = dex_file_->GetMethodPrototype(referrer_method_id);
  const DexFile::TypeList* arg_types = dex_file_->GetProtoParameters(proto);
  for (int i = 0, shorty_pos = 1; i < number_of_parameters; i++) {
    HParameterValue* parameter = new (arena_) HParameterValue(
        *dex_file_,
        arg_types->GetTypeItem(shorty_pos - 1).type_idx_,
        parameter_index++,
        Primitive::GetType(shorty[shorty_pos]),
        false);
    ++shorty_pos;
    entry_block_->AddInstruction(parameter);
    HLocal* local = GetLocalAt(locals_index++);
    // Store the parameter value in the local that the dex code will use
    // to reference that parameter.
    entry_block_->AddInstruction(new (arena_) HStoreLocal(local, parameter, local->GetDexPc()));
    bool is_wide = (parameter->GetType() == Primitive::kPrimLong)
        || (parameter->GetType() == Primitive::kPrimDouble);
    if (is_wide) {
      i++;
      locals_index++;
      parameter_index++;
    }
  }
}

template<typename T>
void HGraphBuilder::If_22t(const Instruction& instruction, uint32_t dex_pc) {
  int32_t target_offset = instruction.GetTargetOffset();
  HBasicBlock* branch_target = FindBlockStartingAt(dex_pc + target_offset);
  HBasicBlock* fallthrough_target = FindBlockStartingAt(dex_pc + instruction.SizeInCodeUnits());
  DCHECK(branch_target != nullptr);
  DCHECK(fallthrough_target != nullptr);
  PotentiallyAddSuspendCheck(branch_target, dex_pc);
  HInstruction* first = LoadLocal(instruction.VRegA(), Primitive::kPrimInt, dex_pc);
  HInstruction* second = LoadLocal(instruction.VRegB(), Primitive::kPrimInt, dex_pc);
  T* comparison = new (arena_) T(first, second, dex_pc);
  current_block_->AddInstruction(comparison);
  HInstruction* ifinst = new (arena_) HIf(comparison, dex_pc);
  current_block_->AddInstruction(ifinst);
  current_block_->AddSuccessor(branch_target);
  current_block_->AddSuccessor(fallthrough_target);
  current_block_ = nullptr;
}

template<typename T>
void HGraphBuilder::If_21t(const Instruction& instruction, uint32_t dex_pc) {
  int32_t target_offset = instruction.GetTargetOffset();
  HBasicBlock* branch_target = FindBlockStartingAt(dex_pc + target_offset);
  HBasicBlock* fallthrough_target = FindBlockStartingAt(dex_pc + instruction.SizeInCodeUnits());
  DCHECK(branch_target != nullptr);
  DCHECK(fallthrough_target != nullptr);
  PotentiallyAddSuspendCheck(branch_target, dex_pc);
  HInstruction* value = LoadLocal(instruction.VRegA(), Primitive::kPrimInt, dex_pc);
  T* comparison = new (arena_) T(value, graph_->GetIntConstant(0, dex_pc), dex_pc);
  current_block_->AddInstruction(comparison);
  HInstruction* ifinst = new (arena_) HIf(comparison, dex_pc);
  current_block_->AddInstruction(ifinst);
  current_block_->AddSuccessor(branch_target);
  current_block_->AddSuccessor(fallthrough_target);
  current_block_ = nullptr;
}

void HGraphBuilder::MaybeRecordStat(MethodCompilationStat compilation_stat) {
  if (compilation_stats_ != nullptr) {
    compilation_stats_->RecordStat(compilation_stat);
  }
}

bool HGraphBuilder::SkipCompilation(const DexFile::CodeItem& code_item,
                                    size_t number_of_branches) {
  const CompilerOptions& compiler_options = compiler_driver_->GetCompilerOptions();
  CompilerOptions::CompilerFilter compiler_filter = compiler_options.GetCompilerFilter();
  if (compiler_filter == CompilerOptions::kEverything) {
    return false;
  }

  if (compiler_options.IsHugeMethod(code_item.insns_size_in_code_units_)) {
    VLOG(compiler) << "Skip compilation of huge method "
                   << PrettyMethod(dex_compilation_unit_->GetDexMethodIndex(), *dex_file_)
                   << ": " << code_item.insns_size_in_code_units_ << " code units";
    MaybeRecordStat(MethodCompilationStat::kNotCompiledHugeMethod);
    return true;
  }

  // If it's large and contains no branches, it's likely to be machine generated initialization.
  if (compiler_options.IsLargeMethod(code_item.insns_size_in_code_units_)
      && (number_of_branches == 0)) {
    VLOG(compiler) << "Skip compilation of large method with no branch "
                   << PrettyMethod(dex_compilation_unit_->GetDexMethodIndex(), *dex_file_)
                   << ": " << code_item.insns_size_in_code_units_ << " code units";
    MaybeRecordStat(MethodCompilationStat::kNotCompiledLargeMethodNoBranches);
    return true;
  }

  return false;
}

void HGraphBuilder::CreateBlocksForTryCatch(const DexFile::CodeItem& code_item) {
  if (code_item.tries_size_ == 0) {
    return;
  }

  // Create branch targets at the start/end of the TryItem range. These are
  // places where the program might fall through into/out of the a block and
  // where TryBoundary instructions will be inserted later. Other edges which
  // enter/exit the try blocks are a result of branches/switches.
  for (size_t idx = 0; idx < code_item.tries_size_; ++idx) {
    const DexFile::TryItem* try_item = DexFile::GetTryItems(code_item, idx);
    uint32_t dex_pc_start = try_item->start_addr_;
    uint32_t dex_pc_end = dex_pc_start + try_item->insn_count_;
    FindOrCreateBlockStartingAt(dex_pc_start);
    if (dex_pc_end < code_item.insns_size_in_code_units_) {
      // TODO: Do not create block if the last instruction cannot fall through.
      FindOrCreateBlockStartingAt(dex_pc_end);
    } else {
      // The TryItem spans until the very end of the CodeItem (or beyond if
      // invalid) and therefore cannot have any code afterwards.
    }
  }

  // Create branch targets for exception handlers.
  const uint8_t* handlers_ptr = DexFile::GetCatchHandlerData(code_item, 0);
  uint32_t handlers_size = DecodeUnsignedLeb128(&handlers_ptr);
  for (uint32_t idx = 0; idx < handlers_size; ++idx) {
    CatchHandlerIterator iterator(handlers_ptr);
    for (; iterator.HasNext(); iterator.Next()) {
      uint32_t address = iterator.GetHandlerAddress();
      HBasicBlock* block = FindOrCreateBlockStartingAt(address);
      block->SetTryCatchInformation(
        new (arena_) TryCatchInformation(iterator.GetHandlerTypeIndex(), *dex_file_));
    }
    handlers_ptr = iterator.EndDataPointer();
  }
}

// Returns the TryItem stored for `block` or nullptr if there is no info for it.
static const DexFile::TryItem* GetTryItem(
    HBasicBlock* block,
    const ArenaSafeMap<uint32_t, const DexFile::TryItem*>& try_block_info) {
  auto iterator = try_block_info.find(block->GetBlockId());
  return (iterator == try_block_info.end()) ? nullptr : iterator->second;
}

void HGraphBuilder::LinkToCatchBlocks(HTryBoundary* try_boundary,
                                      const DexFile::CodeItem& code_item,
                                      const DexFile::TryItem* try_item) {
  for (CatchHandlerIterator it(code_item, *try_item); it.HasNext(); it.Next()) {
    try_boundary->AddExceptionHandler(FindBlockStartingAt(it.GetHandlerAddress()));
  }
}

void HGraphBuilder::InsertTryBoundaryBlocks(const DexFile::CodeItem& code_item) {
  if (code_item.tries_size_ == 0) {
    return;
  }

  // Keep a map of all try blocks and their respective TryItems. We do not use
  // the block's pointer but rather its id to ensure deterministic iteration.
  ArenaSafeMap<uint32_t, const DexFile::TryItem*> try_block_info(
      std::less<uint32_t>(), arena_->Adapter(kArenaAllocGraphBuilder));

  // Obtain TryItem information for blocks with throwing instructions, and split
  // blocks which are both try & catch to simplify the graph.
  // NOTE: We are appending new blocks inside the loop, so we need to use index
  // because iterators can be invalidated. We remember the initial size to avoid
  // iterating over the new blocks which cannot throw.
  for (size_t i = 0, e = graph_->GetBlocks().size(); i < e; ++i) {
    HBasicBlock* block = graph_->GetBlocks()[i];

    // Do not bother creating exceptional edges for try blocks which have no
    // throwing instructions. In that case we simply assume that the block is
    // not covered by a TryItem. This prevents us from creating a throw-catch
    // loop for synchronized blocks.
    if (block->HasThrowingInstructions()) {
      // Try to find a TryItem covering the block.
      DCHECK_NE(block->GetDexPc(), kNoDexPc) << "Block must have a dec_pc to find its TryItem.";
      const int32_t try_item_idx = DexFile::FindTryItem(code_item, block->GetDexPc());
      if (try_item_idx != -1) {
        // Block throwing and in a TryItem. Store the try block information.
        HBasicBlock* throwing_block = block;
        if (block->IsCatchBlock()) {
          // Simplify blocks which are both try and catch, otherwise we would
          // need a strategy for splitting exceptional edges. We split the block
          // after the move-exception (if present) and mark the first part not
          // throwing. The normal-flow edge between them will be split later.
          throwing_block = block->SplitCatchBlockAfterMoveException();
          // Move-exception does not throw and the block has throwing insructions
          // so it must have been possible to split it.
          DCHECK(throwing_block != nullptr);
        }

        try_block_info.Put(throwing_block->GetBlockId(),
                           DexFile::GetTryItems(code_item, try_item_idx));
      }
    }
  }

  // Do a pass over the try blocks and insert entering TryBoundaries where at
  // least one predecessor is not covered by the same TryItem as the try block.
  // We do not split each edge separately, but rather create one boundary block
  // that all predecessors are relinked to. This preserves loop headers (b/23895756).
  for (auto entry : try_block_info) {
    HBasicBlock* try_block = graph_->GetBlocks()[entry.first];
    for (HBasicBlock* predecessor : try_block->GetPredecessors()) {
      if (GetTryItem(predecessor, try_block_info) != entry.second) {
        // Found a predecessor not covered by the same TryItem. Insert entering
        // boundary block.
        HTryBoundary* try_entry =
            new (arena_) HTryBoundary(HTryBoundary::kEntry, try_block->GetDexPc());
        try_block->CreateImmediateDominator()->AddInstruction(try_entry);
        LinkToCatchBlocks(try_entry, code_item, entry.second);
        break;
      }
    }
  }

  // Do a second pass over the try blocks and insert exit TryBoundaries where
  // the successor is not in the same TryItem.
  for (auto entry : try_block_info) {
    HBasicBlock* try_block = graph_->GetBlocks()[entry.first];
    // NOTE: Do not use iterators because SplitEdge would invalidate them.
    for (size_t i = 0, e = try_block->GetSuccessors().size(); i < e; ++i) {
      HBasicBlock* successor = try_block->GetSuccessors()[i];

      // If the successor is a try block, all of its predecessors must be
      // covered by the same TryItem. Otherwise the previous pass would have
      // created a non-throwing boundary block.
      if (GetTryItem(successor, try_block_info) != nullptr) {
        DCHECK_EQ(entry.second, GetTryItem(successor, try_block_info));
        continue;
      }

      // Preserve the invariant that Return(Void) always jumps to Exit by moving
      // it outside the try block if necessary.
      HInstruction* last_instruction = try_block->GetLastInstruction();
      if (last_instruction->IsReturn() || last_instruction->IsReturnVoid()) {
        DCHECK_EQ(successor, exit_block_);
        successor = try_block->SplitBefore(last_instruction);
      }

      // Insert TryBoundary and link to catch blocks.
      HTryBoundary* try_exit =
          new (arena_) HTryBoundary(HTryBoundary::kExit, successor->GetDexPc());
      graph_->SplitEdge(try_block, successor)->AddInstruction(try_exit);
      LinkToCatchBlocks(try_exit, code_item, entry.second);
    }
  }
}

bool HGraphBuilder::BuildGraph(const DexFile::CodeItem& code_item) {
  DCHECK(graph_->GetBlocks().empty());

  const uint16_t* code_ptr = code_item.insns_;
  const uint16_t* code_end = code_item.insns_ + code_item.insns_size_in_code_units_;
  code_start_ = code_ptr;

  // Setup the graph with the entry block and exit block.
  entry_block_ = new (arena_) HBasicBlock(graph_, 0);
  graph_->AddBlock(entry_block_);
  exit_block_ = new (arena_) HBasicBlock(graph_, kNoDexPc);
  graph_->SetEntryBlock(entry_block_);
  graph_->SetExitBlock(exit_block_);

  graph_->SetHasTryCatch(code_item.tries_size_ != 0);

  InitializeLocals(code_item.registers_size_);
  graph_->SetMaximumNumberOfOutVRegs(code_item.outs_size_);

  // Compute the number of dex instructions, blocks, and branches. We will
  // check these values against limits given to the compiler.
  size_t number_of_branches = 0;

  // To avoid splitting blocks, we compute ahead of time the instructions that
  // start a new block, and create these blocks.
  if (!ComputeBranchTargets(code_ptr, code_end, &number_of_branches)) {
    MaybeRecordStat(MethodCompilationStat::kNotCompiledBranchOutsideMethodCode);
    return false;
  }

  // Note that the compiler driver is null when unit testing.
  if ((compiler_driver_ != nullptr) && SkipCompilation(code_item, number_of_branches)) {
    return false;
  }

  CreateBlocksForTryCatch(code_item);

  InitializeParameters(code_item.ins_size_);

  size_t dex_pc = 0;
  while (code_ptr < code_end) {
    // Update the current block if dex_pc starts a new block.
    MaybeUpdateCurrentBlock(dex_pc);
    const Instruction& instruction = *Instruction::At(code_ptr);
    if (!AnalyzeDexInstruction(instruction, dex_pc)) {
      return false;
    }
    dex_pc += instruction.SizeInCodeUnits();
    code_ptr += instruction.SizeInCodeUnits();
  }

  // Add Exit to the exit block.
  exit_block_->AddInstruction(new (arena_) HExit());
  // Add the suspend check to the entry block.
  entry_block_->AddInstruction(new (arena_) HSuspendCheck(0));
  entry_block_->AddInstruction(new (arena_) HGoto());
  // Add the exit block at the end.
  graph_->AddBlock(exit_block_);

  // Iterate over blocks covered by TryItems and insert TryBoundaries at entry
  // and exit points. This requires all control-flow instructions and
  // non-exceptional edges to have been created.
  InsertTryBoundaryBlocks(code_item);

  return true;
}

void HGraphBuilder::MaybeUpdateCurrentBlock(size_t dex_pc) {
  HBasicBlock* block = FindBlockStartingAt(dex_pc);
  if (block == nullptr) {
    return;
  }

  if (current_block_ != nullptr) {
    // Branching instructions clear current_block, so we know
    // the last instruction of the current block is not a branching
    // instruction. We add an unconditional goto to the found block.
    current_block_->AddInstruction(new (arena_) HGoto(dex_pc));
    current_block_->AddSuccessor(block);
  }
  graph_->AddBlock(block);
  current_block_ = block;
}

bool HGraphBuilder::ComputeBranchTargets(const uint16_t* code_ptr,
                                         const uint16_t* code_end,
                                         size_t* number_of_branches) {
  branch_targets_.resize(code_end - code_ptr, nullptr);

  // Create the first block for the dex instructions, single successor of the entry block.
  HBasicBlock* block = new (arena_) HBasicBlock(graph_, 0);
  branch_targets_[0] = block;
  entry_block_->AddSuccessor(block);

  // Iterate over all instructions and find branching instructions. Create blocks for
  // the locations these instructions branch to.
  uint32_t dex_pc = 0;
  while (code_ptr < code_end) {
    const Instruction& instruction = *Instruction::At(code_ptr);
    if (instruction.IsBranch()) {
      (*number_of_branches)++;
      int32_t target = instruction.GetTargetOffset() + dex_pc;
      // Create a block for the target instruction.
      FindOrCreateBlockStartingAt(target);

      dex_pc += instruction.SizeInCodeUnits();
      code_ptr += instruction.SizeInCodeUnits();

      if (instruction.CanFlowThrough()) {
        if (code_ptr >= code_end) {
          // In the normal case we should never hit this but someone can artificially forge a dex
          // file to fall-through out the method code. In this case we bail out compilation.
          return false;
        } else {
          FindOrCreateBlockStartingAt(dex_pc);
        }
      }
    } else if (instruction.IsSwitch()) {
      SwitchTable table(instruction, dex_pc, instruction.Opcode() == Instruction::SPARSE_SWITCH);

      uint16_t num_entries = table.GetNumEntries();

      // In a packed-switch, the entry at index 0 is the starting key. In a sparse-switch, the
      // entry at index 0 is the first key, and values are after *all* keys.
      size_t offset = table.GetFirstValueIndex();

      // Use a larger loop counter type to avoid overflow issues.
      for (size_t i = 0; i < num_entries; ++i) {
        // The target of the case.
        uint32_t target = dex_pc + table.GetEntryAt(i + offset);
        FindOrCreateBlockStartingAt(target);

        // Create a block for the switch-case logic. The block gets the dex_pc
        // of the SWITCH instruction because it is part of its semantics.
        block = new (arena_) HBasicBlock(graph_, dex_pc);
        branch_targets_[table.GetDexPcForIndex(i)] = block;
      }

      // Fall-through. Add a block if there is more code afterwards.
      dex_pc += instruction.SizeInCodeUnits();
      code_ptr += instruction.SizeInCodeUnits();
      if (code_ptr >= code_end) {
        // In the normal case we should never hit this but someone can artificially forge a dex
        // file to fall-through out the method code. In this case we bail out compilation.
        // (A switch can fall-through so we don't need to check CanFlowThrough().)
        return false;
      } else {
        FindOrCreateBlockStartingAt(dex_pc);
      }
    } else {
      code_ptr += instruction.SizeInCodeUnits();
      dex_pc += instruction.SizeInCodeUnits();
    }
  }
  return true;
}

HBasicBlock* HGraphBuilder::FindBlockStartingAt(int32_t dex_pc) const {
  DCHECK_GE(dex_pc, 0);
  return branch_targets_[dex_pc];
}

HBasicBlock* HGraphBuilder::FindOrCreateBlockStartingAt(int32_t dex_pc) {
  HBasicBlock* block = FindBlockStartingAt(dex_pc);
  if (block == nullptr) {
    block = new (arena_) HBasicBlock(graph_, dex_pc);
    branch_targets_[dex_pc] = block;
  }
  return block;
}

template<typename T>
void HGraphBuilder::Unop_12x(const Instruction& instruction,
                             Primitive::Type type,
                             uint32_t dex_pc) {
  HInstruction* first = LoadLocal(instruction.VRegB(), type, dex_pc);
  current_block_->AddInstruction(new (arena_) T(type, first, dex_pc));
  UpdateLocal(instruction.VRegA(), current_block_->GetLastInstruction(), dex_pc);
}

void HGraphBuilder::Conversion_12x(const Instruction& instruction,
                                   Primitive::Type input_type,
                                   Primitive::Type result_type,
                                   uint32_t dex_pc) {
  HInstruction* first = LoadLocal(instruction.VRegB(), input_type, dex_pc);
  current_block_->AddInstruction(new (arena_) HTypeConversion(result_type, first, dex_pc));
  UpdateLocal(instruction.VRegA(), current_block_->GetLastInstruction(), dex_pc);
}

template<typename T>
void HGraphBuilder::Binop_23x(const Instruction& instruction,
                              Primitive::Type type,
                              uint32_t dex_pc) {
  HInstruction* first = LoadLocal(instruction.VRegB(), type, dex_pc);
  HInstruction* second = LoadLocal(instruction.VRegC(), type, dex_pc);
  current_block_->AddInstruction(new (arena_) T(type, first, second, dex_pc));
  UpdateLocal(instruction.VRegA(), current_block_->GetLastInstruction(), dex_pc);
}

template<typename T>
void HGraphBuilder::Binop_23x_shift(const Instruction& instruction,
                                    Primitive::Type type,
                                    uint32_t dex_pc) {
  HInstruction* first = LoadLocal(instruction.VRegB(), type, dex_pc);
  HInstruction* second = LoadLocal(instruction.VRegC(), Primitive::kPrimInt, dex_pc);
  current_block_->AddInstruction(new (arena_) T(type, first, second, dex_pc));
  UpdateLocal(instruction.VRegA(), current_block_->GetLastInstruction(), dex_pc);
}

void HGraphBuilder::Binop_23x_cmp(const Instruction& instruction,
                                  Primitive::Type type,
                                  ComparisonBias bias,
                                  uint32_t dex_pc) {
  HInstruction* first = LoadLocal(instruction.VRegB(), type, dex_pc);
  HInstruction* second = LoadLocal(instruction.VRegC(), type, dex_pc);
  current_block_->AddInstruction(new (arena_) HCompare(type, first, second, bias, dex_pc));
  UpdateLocal(instruction.VRegA(), current_block_->GetLastInstruction(), dex_pc);
}

template<typename T>
void HGraphBuilder::Binop_12x_shift(const Instruction& instruction, Primitive::Type type,
                                    uint32_t dex_pc) {
  HInstruction* first = LoadLocal(instruction.VRegA(), type, dex_pc);
  HInstruction* second = LoadLocal(instruction.VRegB(), Primitive::kPrimInt, dex_pc);
  current_block_->AddInstruction(new (arena_) T(type, first, second, dex_pc));
  UpdateLocal(instruction.VRegA(), current_block_->GetLastInstruction(), dex_pc);
}

template<typename T>
void HGraphBuilder::Binop_12x(const Instruction& instruction,
                              Primitive::Type type,
                              uint32_t dex_pc) {
  HInstruction* first = LoadLocal(instruction.VRegA(), type, dex_pc);
  HInstruction* second = LoadLocal(instruction.VRegB(), type, dex_pc);
  current_block_->AddInstruction(new (arena_) T(type, first, second, dex_pc));
  UpdateLocal(instruction.VRegA(), current_block_->GetLastInstruction(), dex_pc);
}

template<typename T>
void HGraphBuilder::Binop_22s(const Instruction& instruction, bool reverse, uint32_t dex_pc) {
  HInstruction* first = LoadLocal(instruction.VRegB(), Primitive::kPrimInt, dex_pc);
  HInstruction* second = graph_->GetIntConstant(instruction.VRegC_22s(), dex_pc);
  if (reverse) {
    std::swap(first, second);
  }
  current_block_->AddInstruction(new (arena_) T(Primitive::kPrimInt, first, second, dex_pc));
  UpdateLocal(instruction.VRegA(), current_block_->GetLastInstruction(), dex_pc);
}

template<typename T>
void HGraphBuilder::Binop_22b(const Instruction& instruction, bool reverse, uint32_t dex_pc) {
  HInstruction* first = LoadLocal(instruction.VRegB(), Primitive::kPrimInt, dex_pc);
  HInstruction* second = graph_->GetIntConstant(instruction.VRegC_22b(), dex_pc);
  if (reverse) {
    std::swap(first, second);
  }
  current_block_->AddInstruction(new (arena_) T(Primitive::kPrimInt, first, second, dex_pc));
  UpdateLocal(instruction.VRegA(), current_block_->GetLastInstruction(), dex_pc);
}

static bool RequiresConstructorBarrier(const DexCompilationUnit* cu, const CompilerDriver& driver) {
  Thread* self = Thread::Current();
  return cu->IsConstructor()
      && driver.RequiresConstructorBarrier(self, cu->GetDexFile(), cu->GetClassDefIndex());
}

void HGraphBuilder::BuildReturn(const Instruction& instruction,
                                Primitive::Type type,
                                uint32_t dex_pc) {
  if (type == Primitive::kPrimVoid) {
    if (graph_->ShouldGenerateConstructorBarrier()) {
      // The compilation unit is null during testing.
      if (dex_compilation_unit_ != nullptr) {
        DCHECK(RequiresConstructorBarrier(dex_compilation_unit_, *compiler_driver_))
          << "Inconsistent use of ShouldGenerateConstructorBarrier. Should not generate a barrier.";
      }
      current_block_->AddInstruction(new (arena_) HMemoryBarrier(kStoreStore, dex_pc));
    }
    current_block_->AddInstruction(new (arena_) HReturnVoid(dex_pc));
  } else {
    HInstruction* value = LoadLocal(instruction.VRegA(), type, dex_pc);
    current_block_->AddInstruction(new (arena_) HReturn(value, dex_pc));
  }
  current_block_->AddSuccessor(exit_block_);
  current_block_ = nullptr;
}

static InvokeType GetInvokeTypeFromOpCode(Instruction::Code opcode) {
  switch (opcode) {
    case Instruction::INVOKE_STATIC:
    case Instruction::INVOKE_STATIC_RANGE:
      return kStatic;
    case Instruction::INVOKE_DIRECT:
    case Instruction::INVOKE_DIRECT_RANGE:
      return kDirect;
    case Instruction::INVOKE_VIRTUAL:
    case Instruction::INVOKE_VIRTUAL_QUICK:
    case Instruction::INVOKE_VIRTUAL_RANGE:
    case Instruction::INVOKE_VIRTUAL_RANGE_QUICK:
      return kVirtual;
    case Instruction::INVOKE_INTERFACE:
    case Instruction::INVOKE_INTERFACE_RANGE:
      return kInterface;
    case Instruction::INVOKE_SUPER_RANGE:
    case Instruction::INVOKE_SUPER:
      return kSuper;
    default:
      LOG(FATAL) << "Unexpected invoke opcode: " << opcode;
      UNREACHABLE();
  }
}

bool HGraphBuilder::BuildInvoke(const Instruction& instruction,
                                uint32_t dex_pc,
                                uint32_t method_idx,
                                uint32_t number_of_vreg_arguments,
                                bool is_range,
                                uint32_t* args,
                                uint32_t register_index) {
  InvokeType original_invoke_type = GetInvokeTypeFromOpCode(instruction.Opcode());
  InvokeType optimized_invoke_type = original_invoke_type;
  const char* descriptor = dex_file_->GetMethodShorty(method_idx);
  Primitive::Type return_type = Primitive::GetType(descriptor[0]);

  // Remove the return type from the 'proto'.
  size_t number_of_arguments = strlen(descriptor) - 1;
  if (original_invoke_type != kStatic) {  // instance call
    // One extra argument for 'this'.
    number_of_arguments++;
  }

  MethodReference target_method(dex_file_, method_idx);
  int32_t table_index = 0;
  uintptr_t direct_code = 0;
  uintptr_t direct_method = 0;

  // Special handling for string init.
  int32_t string_init_offset = 0;
  bool is_string_init = compiler_driver_->IsStringInit(method_idx,
                                                       dex_file_,
                                                       &string_init_offset);
  // Replace calls to String.<init> with StringFactory.
  if (is_string_init) {
    HInvokeStaticOrDirect::DispatchInfo dispatch_info = {
        HInvokeStaticOrDirect::MethodLoadKind::kStringInit,
        HInvokeStaticOrDirect::CodePtrLocation::kCallArtMethod,
        dchecked_integral_cast<uint64_t>(string_init_offset),
        0U
    };
    HInvoke* invoke = new (arena_) HInvokeStaticOrDirect(
        arena_,
        number_of_arguments - 1,
        Primitive::kPrimNot /*return_type */,
        dex_pc,
        method_idx,
        target_method,
        dispatch_info,
        original_invoke_type,
        kStatic /* optimized_invoke_type */,
        HInvokeStaticOrDirect::ClinitCheckRequirement::kImplicit);
    return HandleStringInit(invoke,
                            number_of_vreg_arguments,
                            args,
                            register_index,
                            is_range,
                            descriptor);
  }

  // Handle unresolved methods.
  if (!compiler_driver_->ComputeInvokeInfo(dex_compilation_unit_,
                                           dex_pc,
                                           true /* update_stats */,
                                           true /* enable_devirtualization */,
                                           &optimized_invoke_type,
                                           &target_method,
                                           &table_index,
                                           &direct_code,
                                           &direct_method)) {
    MaybeRecordStat(MethodCompilationStat::kUnresolvedMethod);
    HInvoke* invoke = new (arena_) HInvokeUnresolved(arena_,
                                                     number_of_arguments,
                                                     return_type,
                                                     dex_pc,
                                                     method_idx,
                                                     original_invoke_type);
    return HandleInvoke(invoke,
                        number_of_vreg_arguments,
                        args,
                        register_index,
                        is_range,
                        descriptor,
                        nullptr /* clinit_check */);
  }

  // Handle resolved methods (non string init).

  DCHECK(optimized_invoke_type != kSuper);

  // Potential class initialization check, in the case of a static method call.
  HClinitCheck* clinit_check = nullptr;
  HInvoke* invoke = nullptr;

  if (optimized_invoke_type == kDirect || optimized_invoke_type == kStatic) {
    // By default, consider that the called method implicitly requires
    // an initialization check of its declaring method.
    HInvokeStaticOrDirect::ClinitCheckRequirement clinit_check_requirement
        = HInvokeStaticOrDirect::ClinitCheckRequirement::kImplicit;
    if (optimized_invoke_type == kStatic) {
      clinit_check = ProcessClinitCheckForInvoke(dex_pc, method_idx, &clinit_check_requirement);
    }

    HInvokeStaticOrDirect::DispatchInfo dispatch_info = {
        HInvokeStaticOrDirect::MethodLoadKind::kDexCacheViaMethod,
        HInvokeStaticOrDirect::CodePtrLocation::kCallArtMethod,
        0u,
        0U
    };
    invoke = new (arena_) HInvokeStaticOrDirect(arena_,
                                                number_of_arguments,
                                                return_type,
                                                dex_pc,
                                                method_idx,
                                                target_method,
                                                dispatch_info,
                                                original_invoke_type,
                                                optimized_invoke_type,
                                                clinit_check_requirement);
  } else if (optimized_invoke_type == kVirtual) {
    invoke = new (arena_) HInvokeVirtual(arena_,
                                         number_of_arguments,
                                         return_type,
                                         dex_pc,
                                         method_idx,
                                         table_index);
  } else {
    DCHECK_EQ(optimized_invoke_type, kInterface);
    invoke = new (arena_) HInvokeInterface(arena_,
                                           number_of_arguments,
                                           return_type,
                                           dex_pc,
                                           method_idx,
                                           table_index);
  }

  return HandleInvoke(invoke,
                      number_of_vreg_arguments,
                      args,
                      register_index,
                      is_range,
                      descriptor,
                      clinit_check);
}

bool HGraphBuilder::BuildNewInstance(uint16_t type_index, uint32_t dex_pc) {
  bool finalizable;
  bool can_throw = NeedsAccessCheck(type_index, &finalizable);

  // Only the non-resolved entrypoint handles the finalizable class case. If we
  // need access checks, then we haven't resolved the method and the class may
  // again be finalizable.
  QuickEntrypointEnum entrypoint = (finalizable || can_throw)
      ? kQuickAllocObject
      : kQuickAllocObjectInitialized;

  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<3> hs(soa.Self());
  Handle<mirror::DexCache> dex_cache(hs.NewHandle(
      dex_compilation_unit_->GetClassLinker()->FindDexCache(
          soa.Self(), *dex_compilation_unit_->GetDexFile())));
  Handle<mirror::Class> resolved_class(hs.NewHandle(dex_cache->GetResolvedType(type_index)));
  const DexFile& outer_dex_file = *outer_compilation_unit_->GetDexFile();
  Handle<mirror::DexCache> outer_dex_cache(hs.NewHandle(
      outer_compilation_unit_->GetClassLinker()->FindDexCache(soa.Self(), outer_dex_file)));

  if (outer_dex_cache.Get() != dex_cache.Get()) {
    // We currently do not support inlining allocations across dex files.
    return false;
  }

  HLoadClass* load_class = new (arena_) HLoadClass(
      graph_->GetCurrentMethod(),
      type_index,
      outer_dex_file,
      IsOutermostCompilingClass(type_index),
      dex_pc,
      /*needs_access_check*/ can_throw,
      compiler_driver_->CanAssumeTypeIsPresentInDexCache(outer_dex_file, type_index));

  current_block_->AddInstruction(load_class);
  HInstruction* cls = load_class;
  if (!IsInitialized(resolved_class)) {
    cls = new (arena_) HClinitCheck(load_class, dex_pc);
    current_block_->AddInstruction(cls);
  }

  current_block_->AddInstruction(new (arena_) HNewInstance(
      cls,
      graph_->GetCurrentMethod(),
      dex_pc,
      type_index,
      *dex_compilation_unit_->GetDexFile(),
      can_throw,
      finalizable,
      entrypoint));
  return true;
}

static bool IsSubClass(mirror::Class* to_test, mirror::Class* super_class)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  return to_test != nullptr && !to_test->IsInterface() && to_test->IsSubClass(super_class);
}

bool HGraphBuilder::IsInitialized(Handle<mirror::Class> cls) const {
  if (cls.Get() == nullptr) {
    return false;
  }

  // `CanAssumeClassIsLoaded` will return true if we're JITting, or will
  // check whether the class is in an image for the AOT compilation.
  if (cls->IsInitialized() &&
      compiler_driver_->CanAssumeClassIsLoaded(cls.Get())) {
    return true;
  }

  if (IsSubClass(GetOutermostCompilingClass(), cls.Get())) {
    return true;
  }

  // TODO: We should walk over the inlined methods, but we don't pass
  //       that information to the builder.
  if (IsSubClass(GetCompilingClass(), cls.Get())) {
    return true;
  }

  return false;
}

HClinitCheck* HGraphBuilder::ProcessClinitCheckForInvoke(
      uint32_t dex_pc,
      uint32_t method_idx,
      HInvokeStaticOrDirect::ClinitCheckRequirement* clinit_check_requirement) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<5> hs(soa.Self());
  Handle<mirror::DexCache> dex_cache(hs.NewHandle(
      dex_compilation_unit_->GetClassLinker()->FindDexCache(
          soa.Self(), *dex_compilation_unit_->GetDexFile())));
  Handle<mirror::ClassLoader> class_loader(hs.NewHandle(
      soa.Decode<mirror::ClassLoader*>(dex_compilation_unit_->GetClassLoader())));
  ArtMethod* resolved_method = compiler_driver_->ResolveMethod(
      soa, dex_cache, class_loader, dex_compilation_unit_, method_idx, InvokeType::kStatic);

  DCHECK(resolved_method != nullptr);

  const DexFile& outer_dex_file = *outer_compilation_unit_->GetDexFile();
  Handle<mirror::DexCache> outer_dex_cache(hs.NewHandle(
      outer_compilation_unit_->GetClassLinker()->FindDexCache(soa.Self(), outer_dex_file)));
  Handle<mirror::Class> outer_class(hs.NewHandle(GetOutermostCompilingClass()));
  Handle<mirror::Class> resolved_method_class(hs.NewHandle(resolved_method->GetDeclaringClass()));

  // The index at which the method's class is stored in the DexCache's type array.
  uint32_t storage_index = DexFile::kDexNoIndex;
  bool is_outer_class = (resolved_method->GetDeclaringClass() == outer_class.Get());
  if (is_outer_class) {
    storage_index = outer_class->GetDexTypeIndex();
  } else if (outer_dex_cache.Get() == dex_cache.Get()) {
    // Get `storage_index` from IsClassOfStaticMethodAvailableToReferrer.
    compiler_driver_->IsClassOfStaticMethodAvailableToReferrer(outer_dex_cache.Get(),
                                                               GetCompilingClass(),
                                                               resolved_method,
                                                               method_idx,
                                                               &storage_index);
  }

  HClinitCheck* clinit_check = nullptr;

  if (IsInitialized(resolved_method_class)) {
    *clinit_check_requirement = HInvokeStaticOrDirect::ClinitCheckRequirement::kNone;
  } else if (storage_index != DexFile::kDexNoIndex) {
    *clinit_check_requirement = HInvokeStaticOrDirect::ClinitCheckRequirement::kExplicit;
    HLoadClass* load_class = new (arena_) HLoadClass(
        graph_->GetCurrentMethod(),
        storage_index,
        outer_dex_file,
        is_outer_class,
        dex_pc,
        /*needs_access_check*/ false,
        compiler_driver_->CanAssumeTypeIsPresentInDexCache(outer_dex_file, storage_index));
    current_block_->AddInstruction(load_class);
    clinit_check = new (arena_) HClinitCheck(load_class, dex_pc);
    current_block_->AddInstruction(clinit_check);
  }
  return clinit_check;
}

bool HGraphBuilder::SetupInvokeArguments(HInvoke* invoke,
                                         uint32_t number_of_vreg_arguments,
                                         uint32_t* args,
                                         uint32_t register_index,
                                         bool is_range,
                                         const char* descriptor,
                                         size_t start_index,
                                         size_t* argument_index) {
  uint32_t descriptor_index = 1;  // Skip the return type.
  uint32_t dex_pc = invoke->GetDexPc();

  for (size_t i = start_index;
       // Make sure we don't go over the expected arguments or over the number of
       // dex registers given. If the instruction was seen as dead by the verifier,
       // it hasn't been properly checked.
       (i < number_of_vreg_arguments) && (*argument_index < invoke->GetNumberOfArguments());
       i++, (*argument_index)++) {
    Primitive::Type type = Primitive::GetType(descriptor[descriptor_index++]);
    bool is_wide = (type == Primitive::kPrimLong) || (type == Primitive::kPrimDouble);
    if (!is_range
        && is_wide
        && ((i + 1 == number_of_vreg_arguments) || (args[i] + 1 != args[i + 1]))) {
      // Longs and doubles should be in pairs, that is, sequential registers. The verifier should
      // reject any class where this is violated. However, the verifier only does these checks
      // on non trivially dead instructions, so we just bailout the compilation.
      VLOG(compiler) << "Did not compile "
                     << PrettyMethod(dex_compilation_unit_->GetDexMethodIndex(), *dex_file_)
                     << " because of non-sequential dex register pair in wide argument";
      MaybeRecordStat(MethodCompilationStat::kNotCompiledMalformedOpcode);
      return false;
    }
    HInstruction* arg = LoadLocal(is_range ? register_index + i : args[i], type, dex_pc);
    invoke->SetArgumentAt(*argument_index, arg);
    if (is_wide) {
      i++;
    }
  }

  if (*argument_index != invoke->GetNumberOfArguments()) {
    VLOG(compiler) << "Did not compile "
                   << PrettyMethod(dex_compilation_unit_->GetDexMethodIndex(), *dex_file_)
                   << " because of wrong number of arguments in invoke instruction";
    MaybeRecordStat(MethodCompilationStat::kNotCompiledMalformedOpcode);
    return false;
  }

  if (invoke->IsInvokeStaticOrDirect() &&
      HInvokeStaticOrDirect::NeedsCurrentMethodInput(
          invoke->AsInvokeStaticOrDirect()->GetMethodLoadKind())) {
    invoke->SetArgumentAt(*argument_index, graph_->GetCurrentMethod());
    (*argument_index)++;
  }

  return true;
}

bool HGraphBuilder::HandleInvoke(HInvoke* invoke,
                                 uint32_t number_of_vreg_arguments,
                                 uint32_t* args,
                                 uint32_t register_index,
                                 bool is_range,
                                 const char* descriptor,
                                 HClinitCheck* clinit_check) {
  DCHECK(!invoke->IsInvokeStaticOrDirect() || !invoke->AsInvokeStaticOrDirect()->IsStringInit());

  size_t start_index = 0;
  size_t argument_index = 0;
  if (invoke->GetOriginalInvokeType() != InvokeType::kStatic) {  // Instance call.
    Temporaries temps(graph_);
    HInstruction* arg = LoadLocal(
        is_range ? register_index : args[0], Primitive::kPrimNot, invoke->GetDexPc());
    HNullCheck* null_check = new (arena_) HNullCheck(arg, invoke->GetDexPc());
    current_block_->AddInstruction(null_check);
    temps.Add(null_check);
    invoke->SetArgumentAt(0, null_check);
    start_index = 1;
    argument_index = 1;
  }

  if (!SetupInvokeArguments(invoke,
                            number_of_vreg_arguments,
                            args,
                            register_index,
                            is_range,
                            descriptor,
                            start_index,
                            &argument_index)) {
    return false;
  }

  if (clinit_check != nullptr) {
    // Add the class initialization check as last input of `invoke`.
    DCHECK(invoke->IsInvokeStaticOrDirect());
    DCHECK(invoke->AsInvokeStaticOrDirect()->GetClinitCheckRequirement()
        == HInvokeStaticOrDirect::ClinitCheckRequirement::kExplicit);
    invoke->SetArgumentAt(argument_index, clinit_check);
    argument_index++;
  }

  current_block_->AddInstruction(invoke);
  latest_result_ = invoke;

  return true;
}

bool HGraphBuilder::HandleStringInit(HInvoke* invoke,
                                     uint32_t number_of_vreg_arguments,
                                     uint32_t* args,
                                     uint32_t register_index,
                                     bool is_range,
                                     const char* descriptor) {
  DCHECK(invoke->IsInvokeStaticOrDirect());
  DCHECK(invoke->AsInvokeStaticOrDirect()->IsStringInit());

  size_t start_index = 1;
  size_t argument_index = 0;
  if (!SetupInvokeArguments(invoke,
                            number_of_vreg_arguments,
                            args,
                            register_index,
                            is_range,
                            descriptor,
                            start_index,
                            &argument_index)) {
    return false;
  }

  // Add move-result for StringFactory method.
  uint32_t orig_this_reg = is_range ? register_index : args[0];
  HInstruction* fake_string = LoadLocal(orig_this_reg, Primitive::kPrimNot, invoke->GetDexPc());
  invoke->SetArgumentAt(argument_index, fake_string);
  current_block_->AddInstruction(invoke);
  PotentiallySimplifyFakeString(orig_this_reg, invoke->GetDexPc(), invoke);

  latest_result_ = invoke;

  return true;
}

void HGraphBuilder::PotentiallySimplifyFakeString(uint16_t original_dex_register,
                                                  uint32_t dex_pc,
                                                  HInvoke* actual_string) {
  if (!graph_->IsDebuggable()) {
    // Notify that we cannot compile with baseline. The dex registers aliasing
    // with `original_dex_register` will be handled when we optimize
    // (see HInstructionSimplifer::VisitFakeString).
    can_use_baseline_for_string_init_ = false;
    return;
  }
  const VerifiedMethod* verified_method =
      compiler_driver_->GetVerifiedMethod(dex_file_, dex_compilation_unit_->GetDexMethodIndex());
  if (verified_method != nullptr) {
    UpdateLocal(original_dex_register, actual_string, dex_pc);
    const SafeMap<uint32_t, std::set<uint32_t>>& string_init_map =
        verified_method->GetStringInitPcRegMap();
    auto map_it = string_init_map.find(dex_pc);
    if (map_it != string_init_map.end()) {
      for (uint32_t reg : map_it->second) {
        HInstruction* load_local = LoadLocal(original_dex_register, Primitive::kPrimNot, dex_pc);
        UpdateLocal(reg, load_local, dex_pc);
      }
    }
  } else {
    can_use_baseline_for_string_init_ = false;
  }
}

static Primitive::Type GetFieldAccessType(const DexFile& dex_file, uint16_t field_index) {
  const DexFile::FieldId& field_id = dex_file.GetFieldId(field_index);
  const char* type = dex_file.GetFieldTypeDescriptor(field_id);
  return Primitive::GetType(type[0]);
}

bool HGraphBuilder::BuildInstanceFieldAccess(const Instruction& instruction,
                                             uint32_t dex_pc,
                                             bool is_put) {
  uint32_t source_or_dest_reg = instruction.VRegA_22c();
  uint32_t obj_reg = instruction.VRegB_22c();
  uint16_t field_index;
  if (instruction.IsQuickened()) {
    if (!CanDecodeQuickenedInfo()) {
      return false;
    }
    field_index = LookupQuickenedInfo(dex_pc);
  } else {
    field_index = instruction.VRegC_22c();
  }

  ScopedObjectAccess soa(Thread::Current());
  ArtField* resolved_field =
      compiler_driver_->ComputeInstanceFieldInfo(field_index, dex_compilation_unit_, is_put, soa);


  HInstruction* object = LoadLocal(obj_reg, Primitive::kPrimNot, dex_pc);
  HInstruction* null_check = new (arena_) HNullCheck(object, dex_pc);
  current_block_->AddInstruction(null_check);

  Primitive::Type field_type = (resolved_field == nullptr)
      ? GetFieldAccessType(*dex_file_, field_index)
      : resolved_field->GetTypeAsPrimitiveType();
  if (is_put) {
    Temporaries temps(graph_);
    // We need one temporary for the null check.
    temps.Add(null_check);
    HInstruction* value = LoadLocal(source_or_dest_reg, field_type, dex_pc);
    HInstruction* field_set = nullptr;
    if (resolved_field == nullptr) {
      MaybeRecordStat(MethodCompilationStat::kUnresolvedField);
      field_set = new (arena_) HUnresolvedInstanceFieldSet(null_check,
                                                           value,
                                                           field_type,
                                                           field_index,
                                                           dex_pc);
    } else {
      uint16_t class_def_index = resolved_field->GetDeclaringClass()->GetDexClassDefIndex();
      field_set = new (arena_) HInstanceFieldSet(null_check,
                                                 value,
                                                 field_type,
                                                 resolved_field->GetOffset(),
                                                 resolved_field->IsVolatile(),
                                                 field_index,
                                                 class_def_index,
                                                 *dex_file_,
                                                 dex_compilation_unit_->GetDexCache(),
                                                 dex_pc);
    }
    current_block_->AddInstruction(field_set);
  } else {
    HInstruction* field_get = nullptr;
    if (resolved_field == nullptr) {
      MaybeRecordStat(MethodCompilationStat::kUnresolvedField);
      field_get = new (arena_) HUnresolvedInstanceFieldGet(null_check,
                                                           field_type,
                                                           field_index,
                                                           dex_pc);
    } else {
      uint16_t class_def_index = resolved_field->GetDeclaringClass()->GetDexClassDefIndex();
      field_get = new (arena_) HInstanceFieldGet(null_check,
                                                 field_type,
                                                 resolved_field->GetOffset(),
                                                 resolved_field->IsVolatile(),
                                                 field_index,
                                                 class_def_index,
                                                 *dex_file_,
                                                 dex_compilation_unit_->GetDexCache(),
                                                 dex_pc);
    }
    current_block_->AddInstruction(field_get);
    UpdateLocal(source_or_dest_reg, field_get, dex_pc);
  }

  return true;
}

static mirror::Class* GetClassFrom(CompilerDriver* driver,
                                   const DexCompilationUnit& compilation_unit) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<2> hs(soa.Self());
  const DexFile& dex_file = *compilation_unit.GetDexFile();
  Handle<mirror::ClassLoader> class_loader(hs.NewHandle(
      soa.Decode<mirror::ClassLoader*>(compilation_unit.GetClassLoader())));
  Handle<mirror::DexCache> dex_cache(hs.NewHandle(
      compilation_unit.GetClassLinker()->FindDexCache(soa.Self(), dex_file)));

  return driver->ResolveCompilingMethodsClass(soa, dex_cache, class_loader, &compilation_unit);
}

mirror::Class* HGraphBuilder::GetOutermostCompilingClass() const {
  return GetClassFrom(compiler_driver_, *outer_compilation_unit_);
}

mirror::Class* HGraphBuilder::GetCompilingClass() const {
  return GetClassFrom(compiler_driver_, *dex_compilation_unit_);
}

bool HGraphBuilder::IsOutermostCompilingClass(uint16_t type_index) const {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<4> hs(soa.Self());
  Handle<mirror::DexCache> dex_cache(hs.NewHandle(
      dex_compilation_unit_->GetClassLinker()->FindDexCache(
          soa.Self(), *dex_compilation_unit_->GetDexFile())));
  Handle<mirror::ClassLoader> class_loader(hs.NewHandle(
      soa.Decode<mirror::ClassLoader*>(dex_compilation_unit_->GetClassLoader())));
  Handle<mirror::Class> cls(hs.NewHandle(compiler_driver_->ResolveClass(
      soa, dex_cache, class_loader, type_index, dex_compilation_unit_)));
  Handle<mirror::Class> outer_class(hs.NewHandle(GetOutermostCompilingClass()));

  // GetOutermostCompilingClass returns null when the class is unresolved
  // (e.g. if it derives from an unresolved class). This is bogus knowing that
  // we are compiling it.
  // When this happens we cannot establish a direct relation between the current
  // class and the outer class, so we return false.
  // (Note that this is only used for optimizing invokes and field accesses)
  return (cls.Get() != nullptr) && (outer_class.Get() == cls.Get());
}

void HGraphBuilder::BuildUnresolvedStaticFieldAccess(const Instruction& instruction,
                                                     uint32_t dex_pc,
                                                     bool is_put,
                                                     Primitive::Type field_type) {
  uint32_t source_or_dest_reg = instruction.VRegA_21c();
  uint16_t field_index = instruction.VRegB_21c();

  if (is_put) {
    HInstruction* value = LoadLocal(source_or_dest_reg, field_type, dex_pc);
    current_block_->AddInstruction(
        new (arena_) HUnresolvedStaticFieldSet(value, field_type, field_index, dex_pc));
  } else {
    current_block_->AddInstruction(
        new (arena_) HUnresolvedStaticFieldGet(field_type, field_index, dex_pc));
    UpdateLocal(source_or_dest_reg, current_block_->GetLastInstruction(), dex_pc);
  }
}
bool HGraphBuilder::BuildStaticFieldAccess(const Instruction& instruction,
                                           uint32_t dex_pc,
                                           bool is_put) {
  uint32_t source_or_dest_reg = instruction.VRegA_21c();
  uint16_t field_index = instruction.VRegB_21c();

  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<5> hs(soa.Self());
  Handle<mirror::DexCache> dex_cache(hs.NewHandle(
      dex_compilation_unit_->GetClassLinker()->FindDexCache(
          soa.Self(), *dex_compilation_unit_->GetDexFile())));
  Handle<mirror::ClassLoader> class_loader(hs.NewHandle(
      soa.Decode<mirror::ClassLoader*>(dex_compilation_unit_->GetClassLoader())));
  ArtField* resolved_field = compiler_driver_->ResolveField(
      soa, dex_cache, class_loader, dex_compilation_unit_, field_index, true);

  if (resolved_field == nullptr) {
    MaybeRecordStat(MethodCompilationStat::kUnresolvedField);
    Primitive::Type field_type = GetFieldAccessType(*dex_file_, field_index);
    BuildUnresolvedStaticFieldAccess(instruction, dex_pc, is_put, field_type);
    return true;
  }

  Primitive::Type field_type = resolved_field->GetTypeAsPrimitiveType();
  const DexFile& outer_dex_file = *outer_compilation_unit_->GetDexFile();
  Handle<mirror::DexCache> outer_dex_cache(hs.NewHandle(
      outer_compilation_unit_->GetClassLinker()->FindDexCache(soa.Self(), outer_dex_file)));
  Handle<mirror::Class> outer_class(hs.NewHandle(GetOutermostCompilingClass()));

  // The index at which the field's class is stored in the DexCache's type array.
  uint32_t storage_index;
  bool is_outer_class = (outer_class.Get() == resolved_field->GetDeclaringClass());
  if (is_outer_class) {
    storage_index = outer_class->GetDexTypeIndex();
  } else if (outer_dex_cache.Get() != dex_cache.Get()) {
    // The compiler driver cannot currently understand multiple dex caches involved. Just bailout.
    return false;
  } else {
    // TODO: This is rather expensive. Perf it and cache the results if needed.
    std::pair<bool, bool> pair = compiler_driver_->IsFastStaticField(
        outer_dex_cache.Get(),
        GetCompilingClass(),
        resolved_field,
        field_index,
        &storage_index);
    bool can_easily_access = is_put ? pair.second : pair.first;
    if (!can_easily_access) {
      MaybeRecordStat(MethodCompilationStat::kUnresolvedFieldNotAFastAccess);
      BuildUnresolvedStaticFieldAccess(instruction, dex_pc, is_put, field_type);
      return true;
    }
  }

  bool is_in_cache =
      compiler_driver_->CanAssumeTypeIsPresentInDexCache(outer_dex_file, storage_index);
  HLoadClass* constant = new (arena_) HLoadClass(graph_->GetCurrentMethod(),
                                                 storage_index,
                                                 outer_dex_file,
                                                 is_outer_class,
                                                 dex_pc,
                                                 /*needs_access_check*/ false,
                                                 is_in_cache);
  current_block_->AddInstruction(constant);

  HInstruction* cls = constant;

  Handle<mirror::Class> klass(hs.NewHandle(resolved_field->GetDeclaringClass()));
  if (!IsInitialized(klass)) {
    cls = new (arena_) HClinitCheck(constant, dex_pc);
    current_block_->AddInstruction(cls);
  }

  uint16_t class_def_index = klass->GetDexClassDefIndex();
  if (is_put) {
    // We need to keep the class alive before loading the value.
    Temporaries temps(graph_);
    temps.Add(cls);
    HInstruction* value = LoadLocal(source_or_dest_reg, field_type, dex_pc);
    DCHECK_EQ(value->GetType(), field_type);
    current_block_->AddInstruction(new (arena_) HStaticFieldSet(cls,
                                                                value,
                                                                field_type,
                                                                resolved_field->GetOffset(),
                                                                resolved_field->IsVolatile(),
                                                                field_index,
                                                                class_def_index,
                                                                *dex_file_,
                                                                dex_cache_,
                                                                dex_pc));
  } else {
    current_block_->AddInstruction(new (arena_) HStaticFieldGet(cls,
                                                                field_type,
                                                                resolved_field->GetOffset(),
                                                                resolved_field->IsVolatile(),
                                                                field_index,
                                                                class_def_index,
                                                                *dex_file_,
                                                                dex_cache_,
                                                                dex_pc));
    UpdateLocal(source_or_dest_reg, current_block_->GetLastInstruction(), dex_pc);
  }
  return true;
}

void HGraphBuilder::BuildCheckedDivRem(uint16_t out_vreg,
                                       uint16_t first_vreg,
                                       int64_t second_vreg_or_constant,
                                       uint32_t dex_pc,
                                       Primitive::Type type,
                                       bool second_is_constant,
                                       bool isDiv) {
  DCHECK(type == Primitive::kPrimInt || type == Primitive::kPrimLong);

  HInstruction* first = LoadLocal(first_vreg, type, dex_pc);
  HInstruction* second = nullptr;
  if (second_is_constant) {
    if (type == Primitive::kPrimInt) {
      second = graph_->GetIntConstant(second_vreg_or_constant, dex_pc);
    } else {
      second = graph_->GetLongConstant(second_vreg_or_constant, dex_pc);
    }
  } else {
    second = LoadLocal(second_vreg_or_constant, type, dex_pc);
  }

  if (!second_is_constant
      || (type == Primitive::kPrimInt && second->AsIntConstant()->GetValue() == 0)
      || (type == Primitive::kPrimLong && second->AsLongConstant()->GetValue() == 0)) {
    second = new (arena_) HDivZeroCheck(second, dex_pc);
    Temporaries temps(graph_);
    current_block_->AddInstruction(second);
    temps.Add(current_block_->GetLastInstruction());
  }

  if (isDiv) {
    current_block_->AddInstruction(new (arena_) HDiv(type, first, second, dex_pc));
  } else {
    current_block_->AddInstruction(new (arena_) HRem(type, first, second, dex_pc));
  }
  UpdateLocal(out_vreg, current_block_->GetLastInstruction(), dex_pc);
}

void HGraphBuilder::BuildArrayAccess(const Instruction& instruction,
                                     uint32_t dex_pc,
                                     bool is_put,
                                     Primitive::Type anticipated_type) {
  uint8_t source_or_dest_reg = instruction.VRegA_23x();
  uint8_t array_reg = instruction.VRegB_23x();
  uint8_t index_reg = instruction.VRegC_23x();

  // We need one temporary for the null check, one for the index, and one for the length.
  Temporaries temps(graph_);

  HInstruction* object = LoadLocal(array_reg, Primitive::kPrimNot, dex_pc);
  object = new (arena_) HNullCheck(object, dex_pc);
  current_block_->AddInstruction(object);
  temps.Add(object);

  HInstruction* length = new (arena_) HArrayLength(object, dex_pc);
  current_block_->AddInstruction(length);
  temps.Add(length);
  HInstruction* index = LoadLocal(index_reg, Primitive::kPrimInt, dex_pc);
  index = new (arena_) HBoundsCheck(index, length, dex_pc);
  current_block_->AddInstruction(index);
  temps.Add(index);
  if (is_put) {
    HInstruction* value = LoadLocal(source_or_dest_reg, anticipated_type, dex_pc);
    // TODO: Insert a type check node if the type is Object.
    current_block_->AddInstruction(new (arena_) HArraySet(
        object, index, value, anticipated_type, dex_pc));
  } else {
    current_block_->AddInstruction(new (arena_) HArrayGet(object, index, anticipated_type, dex_pc));
    UpdateLocal(source_or_dest_reg, current_block_->GetLastInstruction(), dex_pc);
  }
  graph_->SetHasBoundsChecks(true);
}

void HGraphBuilder::BuildFilledNewArray(uint32_t dex_pc,
                                        uint32_t type_index,
                                        uint32_t number_of_vreg_arguments,
                                        bool is_range,
                                        uint32_t* args,
                                        uint32_t register_index) {
  HInstruction* length = graph_->GetIntConstant(number_of_vreg_arguments, dex_pc);
  bool finalizable;
  QuickEntrypointEnum entrypoint = NeedsAccessCheck(type_index, &finalizable)
      ? kQuickAllocArrayWithAccessCheck
      : kQuickAllocArray;
  HInstruction* object = new (arena_) HNewArray(length,
                                                graph_->GetCurrentMethod(),
                                                dex_pc,
                                                type_index,
                                                *dex_compilation_unit_->GetDexFile(),
                                                entrypoint);
  current_block_->AddInstruction(object);

  const char* descriptor = dex_file_->StringByTypeIdx(type_index);
  DCHECK_EQ(descriptor[0], '[') << descriptor;
  char primitive = descriptor[1];
  DCHECK(primitive == 'I'
      || primitive == 'L'
      || primitive == '[') << descriptor;
  bool is_reference_array = (primitive == 'L') || (primitive == '[');
  Primitive::Type type = is_reference_array ? Primitive::kPrimNot : Primitive::kPrimInt;

  Temporaries temps(graph_);
  temps.Add(object);
  for (size_t i = 0; i < number_of_vreg_arguments; ++i) {
    HInstruction* value = LoadLocal(is_range ? register_index + i : args[i], type, dex_pc);
    HInstruction* index = graph_->GetIntConstant(i, dex_pc);
    current_block_->AddInstruction(
        new (arena_) HArraySet(object, index, value, type, dex_pc));
  }
  latest_result_ = object;
}

template <typename T>
void HGraphBuilder::BuildFillArrayData(HInstruction* object,
                                       const T* data,
                                       uint32_t element_count,
                                       Primitive::Type anticipated_type,
                                       uint32_t dex_pc) {
  for (uint32_t i = 0; i < element_count; ++i) {
    HInstruction* index = graph_->GetIntConstant(i, dex_pc);
    HInstruction* value = graph_->GetIntConstant(data[i], dex_pc);
    current_block_->AddInstruction(new (arena_) HArraySet(
      object, index, value, anticipated_type, dex_pc));
  }
}

void HGraphBuilder::BuildFillArrayData(const Instruction& instruction, uint32_t dex_pc) {
  Temporaries temps(graph_);
  HInstruction* array = LoadLocal(instruction.VRegA_31t(), Primitive::kPrimNot, dex_pc);
  HNullCheck* null_check = new (arena_) HNullCheck(array, dex_pc);
  current_block_->AddInstruction(null_check);
  temps.Add(null_check);

  HInstruction* length = new (arena_) HArrayLength(null_check, dex_pc);
  current_block_->AddInstruction(length);

  int32_t payload_offset = instruction.VRegB_31t() + dex_pc;
  const Instruction::ArrayDataPayload* payload =
      reinterpret_cast<const Instruction::ArrayDataPayload*>(code_start_ + payload_offset);
  const uint8_t* data = payload->data;
  uint32_t element_count = payload->element_count;

  // Implementation of this DEX instruction seems to be that the bounds check is
  // done before doing any stores.
  HInstruction* last_index = graph_->GetIntConstant(payload->element_count - 1, dex_pc);
  current_block_->AddInstruction(new (arena_) HBoundsCheck(last_index, length, dex_pc));

  switch (payload->element_width) {
    case 1:
      BuildFillArrayData(null_check,
                         reinterpret_cast<const int8_t*>(data),
                         element_count,
                         Primitive::kPrimByte,
                         dex_pc);
      break;
    case 2:
      BuildFillArrayData(null_check,
                         reinterpret_cast<const int16_t*>(data),
                         element_count,
                         Primitive::kPrimShort,
                         dex_pc);
      break;
    case 4:
      BuildFillArrayData(null_check,
                         reinterpret_cast<const int32_t*>(data),
                         element_count,
                         Primitive::kPrimInt,
                         dex_pc);
      break;
    case 8:
      BuildFillWideArrayData(null_check,
                             reinterpret_cast<const int64_t*>(data),
                             element_count,
                             dex_pc);
      break;
    default:
      LOG(FATAL) << "Unknown element width for " << payload->element_width;
  }
  graph_->SetHasBoundsChecks(true);
}

void HGraphBuilder::BuildFillWideArrayData(HInstruction* object,
                                           const int64_t* data,
                                           uint32_t element_count,
                                           uint32_t dex_pc) {
  for (uint32_t i = 0; i < element_count; ++i) {
    HInstruction* index = graph_->GetIntConstant(i, dex_pc);
    HInstruction* value = graph_->GetLongConstant(data[i], dex_pc);
    current_block_->AddInstruction(new (arena_) HArraySet(
      object, index, value, Primitive::kPrimLong, dex_pc));
  }
}

static TypeCheckKind ComputeTypeCheckKind(Handle<mirror::Class> cls)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  if (cls.Get() == nullptr) {
    return TypeCheckKind::kUnresolvedCheck;
  } else if (cls->IsInterface()) {
    return TypeCheckKind::kInterfaceCheck;
  } else if (cls->IsArrayClass()) {
    if (cls->GetComponentType()->IsObjectClass()) {
      return TypeCheckKind::kArrayObjectCheck;
    } else if (cls->CannotBeAssignedFromOtherTypes()) {
      return TypeCheckKind::kExactCheck;
    } else {
      return TypeCheckKind::kArrayCheck;
    }
  } else if (cls->IsFinal()) {
    return TypeCheckKind::kExactCheck;
  } else if (cls->IsAbstract()) {
    return TypeCheckKind::kAbstractClassCheck;
  } else {
    return TypeCheckKind::kClassHierarchyCheck;
  }
}

void HGraphBuilder::BuildTypeCheck(const Instruction& instruction,
                                   uint8_t destination,
                                   uint8_t reference,
                                   uint16_t type_index,
                                   uint32_t dex_pc) {
  bool type_known_final, type_known_abstract, use_declaring_class;
  bool can_access = compiler_driver_->CanAccessTypeWithoutChecks(
      dex_compilation_unit_->GetDexMethodIndex(),
      *dex_compilation_unit_->GetDexFile(),
      type_index,
      &type_known_final,
      &type_known_abstract,
      &use_declaring_class);

  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<2> hs(soa.Self());
  const DexFile& dex_file = *dex_compilation_unit_->GetDexFile();
  Handle<mirror::DexCache> dex_cache(hs.NewHandle(
      dex_compilation_unit_->GetClassLinker()->FindDexCache(soa.Self(), dex_file)));
  Handle<mirror::Class> resolved_class(hs.NewHandle(dex_cache->GetResolvedType(type_index)));

  HInstruction* object = LoadLocal(reference, Primitive::kPrimNot, dex_pc);
  HLoadClass* cls = new (arena_) HLoadClass(
      graph_->GetCurrentMethod(),
      type_index,
      dex_file,
      IsOutermostCompilingClass(type_index),
      dex_pc,
      !can_access,
      compiler_driver_->CanAssumeTypeIsPresentInDexCache(dex_file, type_index));
  current_block_->AddInstruction(cls);

  // The class needs a temporary before being used by the type check.
  Temporaries temps(graph_);
  temps.Add(cls);

  TypeCheckKind check_kind = ComputeTypeCheckKind(resolved_class);
  if (instruction.Opcode() == Instruction::INSTANCE_OF) {
    current_block_->AddInstruction(new (arena_) HInstanceOf(object, cls, check_kind, dex_pc));
    UpdateLocal(destination, current_block_->GetLastInstruction(), dex_pc);
  } else {
    DCHECK_EQ(instruction.Opcode(), Instruction::CHECK_CAST);
    current_block_->AddInstruction(new (arena_) HCheckCast(object, cls, check_kind, dex_pc));
  }
}

bool HGraphBuilder::NeedsAccessCheck(uint32_t type_index, bool* finalizable) const {
  return !compiler_driver_->CanAccessInstantiableTypeWithoutChecks(
      dex_compilation_unit_->GetDexMethodIndex(), *dex_file_, type_index, finalizable);
}

void HGraphBuilder::BuildSwitchJumpTable(const SwitchTable& table,
                                         const Instruction& instruction,
                                         HInstruction* value,
                                         uint32_t dex_pc) {
  // Add the successor blocks to the current block.
  uint16_t num_entries = table.GetNumEntries();
  for (size_t i = 1; i <= num_entries; i++) {
    int32_t target_offset = table.GetEntryAt(i);
    HBasicBlock* case_target = FindBlockStartingAt(dex_pc + target_offset);
    DCHECK(case_target != nullptr);

    // Add the target block as a successor.
    current_block_->AddSuccessor(case_target);
  }

  // Add the default target block as the last successor.
  HBasicBlock* default_target = FindBlockStartingAt(dex_pc + instruction.SizeInCodeUnits());
  DCHECK(default_target != nullptr);
  current_block_->AddSuccessor(default_target);

  // Now add the Switch instruction.
  int32_t starting_key = table.GetEntryAt(0);
  current_block_->AddInstruction(
      new (arena_) HPackedSwitch(starting_key, num_entries, value, dex_pc));
  // This block ends with control flow.
  current_block_ = nullptr;
}

void HGraphBuilder::BuildPackedSwitch(const Instruction& instruction, uint32_t dex_pc) {
  // Verifier guarantees that the payload for PackedSwitch contains:
  //   (a) number of entries (may be zero)
  //   (b) first and lowest switch case value (entry 0, always present)
  //   (c) list of target pcs (entries 1 <= i <= N)
  SwitchTable table(instruction, dex_pc, false);

  // Value to test against.
  HInstruction* value = LoadLocal(instruction.VRegA(), Primitive::kPrimInt, dex_pc);

  // Starting key value.
  int32_t starting_key = table.GetEntryAt(0);

  // Retrieve number of entries.
  uint16_t num_entries = table.GetNumEntries();
  if (num_entries == 0) {
    return;
  }

  // Don't use a packed switch if there are very few entries.
  if (num_entries > kSmallSwitchThreshold) {
    BuildSwitchJumpTable(table, instruction, value, dex_pc);
  } else {
    // Chained cmp-and-branch, starting from starting_key.
    for (size_t i = 1; i <= num_entries; i++) {
      BuildSwitchCaseHelper(instruction,
                            i,
                            i == num_entries,
                            table,
                            value,
                            starting_key + i - 1,
                            table.GetEntryAt(i),
                            dex_pc);
    }
  }
}

void HGraphBuilder::BuildSparseSwitch(const Instruction& instruction, uint32_t dex_pc) {
  // Verifier guarantees that the payload for SparseSwitch contains:
  //   (a) number of entries (may be zero)
  //   (b) sorted key values (entries 0 <= i < N)
  //   (c) target pcs corresponding to the switch values (entries N <= i < 2*N)
  SwitchTable table(instruction, dex_pc, true);

  // Value to test against.
  HInstruction* value = LoadLocal(instruction.VRegA(), Primitive::kPrimInt, dex_pc);

  uint16_t num_entries = table.GetNumEntries();

  for (size_t i = 0; i < num_entries; i++) {
    BuildSwitchCaseHelper(instruction, i, i == static_cast<size_t>(num_entries) - 1, table, value,
                          table.GetEntryAt(i), table.GetEntryAt(i + num_entries), dex_pc);
  }
}

void HGraphBuilder::BuildSwitchCaseHelper(const Instruction& instruction, size_t index,
                                          bool is_last_case, const SwitchTable& table,
                                          HInstruction* value, int32_t case_value_int,
                                          int32_t target_offset, uint32_t dex_pc) {
  HBasicBlock* case_target = FindBlockStartingAt(dex_pc + target_offset);
  DCHECK(case_target != nullptr);
  PotentiallyAddSuspendCheck(case_target, dex_pc);

  // The current case's value.
  HInstruction* this_case_value = graph_->GetIntConstant(case_value_int, dex_pc);

  // Compare value and this_case_value.
  HEqual* comparison = new (arena_) HEqual(value, this_case_value, dex_pc);
  current_block_->AddInstruction(comparison);
  HInstruction* ifinst = new (arena_) HIf(comparison, dex_pc);
  current_block_->AddInstruction(ifinst);

  // Case hit: use the target offset to determine where to go.
  current_block_->AddSuccessor(case_target);

  // Case miss: go to the next case (or default fall-through).
  // When there is a next case, we use the block stored with the table offset representing this
  // case (that is where we registered them in ComputeBranchTargets).
  // When there is no next case, we use the following instruction.
  // TODO: Find a good way to peel the last iteration to avoid conditional, but still have re-use.
  if (!is_last_case) {
    HBasicBlock* next_case_target = FindBlockStartingAt(table.GetDexPcForIndex(index));
    DCHECK(next_case_target != nullptr);
    current_block_->AddSuccessor(next_case_target);

    // Need to manually add the block, as there is no dex-pc transition for the cases.
    graph_->AddBlock(next_case_target);

    current_block_ = next_case_target;
  } else {
    HBasicBlock* default_target = FindBlockStartingAt(dex_pc + instruction.SizeInCodeUnits());
    DCHECK(default_target != nullptr);
    current_block_->AddSuccessor(default_target);
    current_block_ = nullptr;
  }
}

void HGraphBuilder::PotentiallyAddSuspendCheck(HBasicBlock* target, uint32_t dex_pc) {
  int32_t target_offset = target->GetDexPc() - dex_pc;
  if (target_offset <= 0) {
    // DX generates back edges to the first encountered return. We can save
    // time of later passes by not adding redundant suspend checks.
    HInstruction* last_in_target = target->GetLastInstruction();
    if (last_in_target != nullptr &&
        (last_in_target->IsReturn() || last_in_target->IsReturnVoid())) {
      return;
    }

    // Add a suspend check to backward branches which may potentially loop. We
    // can remove them after we recognize loops in the graph.
    current_block_->AddInstruction(new (arena_) HSuspendCheck(dex_pc));
  }
}

bool HGraphBuilder::CanDecodeQuickenedInfo() const {
  return interpreter_metadata_ != nullptr;
}

uint16_t HGraphBuilder::LookupQuickenedInfo(uint32_t dex_pc) {
  DCHECK(interpreter_metadata_ != nullptr);
  uint32_t dex_pc_in_map = DecodeUnsignedLeb128(&interpreter_metadata_);
  DCHECK_EQ(dex_pc, dex_pc_in_map);
  return DecodeUnsignedLeb128(&interpreter_metadata_);
}

bool HGraphBuilder::AnalyzeDexInstruction(const Instruction& instruction, uint32_t dex_pc) {
  if (current_block_ == nullptr) {
    return true;  // Dead code
  }

  switch (instruction.Opcode()) {
    case Instruction::CONST_4: {
      int32_t register_index = instruction.VRegA();
      HIntConstant* constant = graph_->GetIntConstant(instruction.VRegB_11n(), dex_pc);
      UpdateLocal(register_index, constant, dex_pc);
      break;
    }

    case Instruction::CONST_16: {
      int32_t register_index = instruction.VRegA();
      HIntConstant* constant = graph_->GetIntConstant(instruction.VRegB_21s(), dex_pc);
      UpdateLocal(register_index, constant, dex_pc);
      break;
    }

    case Instruction::CONST: {
      int32_t register_index = instruction.VRegA();
      HIntConstant* constant = graph_->GetIntConstant(instruction.VRegB_31i(), dex_pc);
      UpdateLocal(register_index, constant, dex_pc);
      break;
    }

    case Instruction::CONST_HIGH16: {
      int32_t register_index = instruction.VRegA();
      HIntConstant* constant = graph_->GetIntConstant(instruction.VRegB_21h() << 16, dex_pc);
      UpdateLocal(register_index, constant, dex_pc);
      break;
    }

    case Instruction::CONST_WIDE_16: {
      int32_t register_index = instruction.VRegA();
      // Get 16 bits of constant value, sign extended to 64 bits.
      int64_t value = instruction.VRegB_21s();
      value <<= 48;
      value >>= 48;
      HLongConstant* constant = graph_->GetLongConstant(value, dex_pc);
      UpdateLocal(register_index, constant, dex_pc);
      break;
    }

    case Instruction::CONST_WIDE_32: {
      int32_t register_index = instruction.VRegA();
      // Get 32 bits of constant value, sign extended to 64 bits.
      int64_t value = instruction.VRegB_31i();
      value <<= 32;
      value >>= 32;
      HLongConstant* constant = graph_->GetLongConstant(value, dex_pc);
      UpdateLocal(register_index, constant, dex_pc);
      break;
    }

    case Instruction::CONST_WIDE: {
      int32_t register_index = instruction.VRegA();
      HLongConstant* constant = graph_->GetLongConstant(instruction.VRegB_51l(), dex_pc);
      UpdateLocal(register_index, constant, dex_pc);
      break;
    }

    case Instruction::CONST_WIDE_HIGH16: {
      int32_t register_index = instruction.VRegA();
      int64_t value = static_cast<int64_t>(instruction.VRegB_21h()) << 48;
      HLongConstant* constant = graph_->GetLongConstant(value, dex_pc);
      UpdateLocal(register_index, constant, dex_pc);
      break;
    }

    // Note that the SSA building will refine the types.
    case Instruction::MOVE:
    case Instruction::MOVE_FROM16:
    case Instruction::MOVE_16: {
      HInstruction* value = LoadLocal(instruction.VRegB(), Primitive::kPrimInt, dex_pc);
      UpdateLocal(instruction.VRegA(), value, dex_pc);
      break;
    }

    // Note that the SSA building will refine the types.
    case Instruction::MOVE_WIDE:
    case Instruction::MOVE_WIDE_FROM16:
    case Instruction::MOVE_WIDE_16: {
      HInstruction* value = LoadLocal(instruction.VRegB(), Primitive::kPrimLong, dex_pc);
      UpdateLocal(instruction.VRegA(), value, dex_pc);
      break;
    }

    case Instruction::MOVE_OBJECT:
    case Instruction::MOVE_OBJECT_16:
    case Instruction::MOVE_OBJECT_FROM16: {
      HInstruction* value = LoadLocal(instruction.VRegB(), Primitive::kPrimNot, dex_pc);
      UpdateLocal(instruction.VRegA(), value, dex_pc);
      break;
    }

    case Instruction::RETURN_VOID_NO_BARRIER:
    case Instruction::RETURN_VOID: {
      BuildReturn(instruction, Primitive::kPrimVoid, dex_pc);
      break;
    }

#define IF_XX(comparison, cond) \
    case Instruction::IF_##cond: If_22t<comparison>(instruction, dex_pc); break; \
    case Instruction::IF_##cond##Z: If_21t<comparison>(instruction, dex_pc); break

    IF_XX(HEqual, EQ);
    IF_XX(HNotEqual, NE);
    IF_XX(HLessThan, LT);
    IF_XX(HLessThanOrEqual, LE);
    IF_XX(HGreaterThan, GT);
    IF_XX(HGreaterThanOrEqual, GE);

    case Instruction::GOTO:
    case Instruction::GOTO_16:
    case Instruction::GOTO_32: {
      int32_t offset = instruction.GetTargetOffset();
      HBasicBlock* target = FindBlockStartingAt(offset + dex_pc);
      DCHECK(target != nullptr);
      PotentiallyAddSuspendCheck(target, dex_pc);
      current_block_->AddInstruction(new (arena_) HGoto(dex_pc));
      current_block_->AddSuccessor(target);
      current_block_ = nullptr;
      break;
    }

    case Instruction::RETURN: {
      BuildReturn(instruction, return_type_, dex_pc);
      break;
    }

    case Instruction::RETURN_OBJECT: {
      BuildReturn(instruction, return_type_, dex_pc);
      break;
    }

    case Instruction::RETURN_WIDE: {
      BuildReturn(instruction, return_type_, dex_pc);
      break;
    }

    case Instruction::INVOKE_DIRECT:
    case Instruction::INVOKE_INTERFACE:
    case Instruction::INVOKE_STATIC:
    case Instruction::INVOKE_SUPER:
    case Instruction::INVOKE_VIRTUAL:
    case Instruction::INVOKE_VIRTUAL_QUICK: {
      uint16_t method_idx;
      if (instruction.Opcode() == Instruction::INVOKE_VIRTUAL_QUICK) {
        if (!CanDecodeQuickenedInfo()) {
          return false;
        }
        method_idx = LookupQuickenedInfo(dex_pc);
      } else {
        method_idx = instruction.VRegB_35c();
      }
      uint32_t number_of_vreg_arguments = instruction.VRegA_35c();
      uint32_t args[5];
      instruction.GetVarArgs(args);
      if (!BuildInvoke(instruction, dex_pc, method_idx,
                       number_of_vreg_arguments, false, args, -1)) {
        return false;
      }
      break;
    }

    case Instruction::INVOKE_DIRECT_RANGE:
    case Instruction::INVOKE_INTERFACE_RANGE:
    case Instruction::INVOKE_STATIC_RANGE:
    case Instruction::INVOKE_SUPER_RANGE:
    case Instruction::INVOKE_VIRTUAL_RANGE:
    case Instruction::INVOKE_VIRTUAL_RANGE_QUICK: {
      uint16_t method_idx;
      if (instruction.Opcode() == Instruction::INVOKE_VIRTUAL_RANGE_QUICK) {
        if (!CanDecodeQuickenedInfo()) {
          return false;
        }
        method_idx = LookupQuickenedInfo(dex_pc);
      } else {
        method_idx = instruction.VRegB_3rc();
      }
      uint32_t number_of_vreg_arguments = instruction.VRegA_3rc();
      uint32_t register_index = instruction.VRegC();
      if (!BuildInvoke(instruction, dex_pc, method_idx,
                       number_of_vreg_arguments, true, nullptr, register_index)) {
        return false;
      }
      break;
    }

    case Instruction::NEG_INT: {
      Unop_12x<HNeg>(instruction, Primitive::kPrimInt, dex_pc);
      break;
    }

    case Instruction::NEG_LONG: {
      Unop_12x<HNeg>(instruction, Primitive::kPrimLong, dex_pc);
      break;
    }

    case Instruction::NEG_FLOAT: {
      Unop_12x<HNeg>(instruction, Primitive::kPrimFloat, dex_pc);
      break;
    }

    case Instruction::NEG_DOUBLE: {
      Unop_12x<HNeg>(instruction, Primitive::kPrimDouble, dex_pc);
      break;
    }

    case Instruction::NOT_INT: {
      Unop_12x<HNot>(instruction, Primitive::kPrimInt, dex_pc);
      break;
    }

    case Instruction::NOT_LONG: {
      Unop_12x<HNot>(instruction, Primitive::kPrimLong, dex_pc);
      break;
    }

    case Instruction::INT_TO_LONG: {
      Conversion_12x(instruction, Primitive::kPrimInt, Primitive::kPrimLong, dex_pc);
      break;
    }

    case Instruction::INT_TO_FLOAT: {
      Conversion_12x(instruction, Primitive::kPrimInt, Primitive::kPrimFloat, dex_pc);
      break;
    }

    case Instruction::INT_TO_DOUBLE: {
      Conversion_12x(instruction, Primitive::kPrimInt, Primitive::kPrimDouble, dex_pc);
      break;
    }

    case Instruction::LONG_TO_INT: {
      Conversion_12x(instruction, Primitive::kPrimLong, Primitive::kPrimInt, dex_pc);
      break;
    }

    case Instruction::LONG_TO_FLOAT: {
      Conversion_12x(instruction, Primitive::kPrimLong, Primitive::kPrimFloat, dex_pc);
      break;
    }

    case Instruction::LONG_TO_DOUBLE: {
      Conversion_12x(instruction, Primitive::kPrimLong, Primitive::kPrimDouble, dex_pc);
      break;
    }

    case Instruction::FLOAT_TO_INT: {
      Conversion_12x(instruction, Primitive::kPrimFloat, Primitive::kPrimInt, dex_pc);
      break;
    }

    case Instruction::FLOAT_TO_LONG: {
      Conversion_12x(instruction, Primitive::kPrimFloat, Primitive::kPrimLong, dex_pc);
      break;
    }

    case Instruction::FLOAT_TO_DOUBLE: {
      Conversion_12x(instruction, Primitive::kPrimFloat, Primitive::kPrimDouble, dex_pc);
      break;
    }

    case Instruction::DOUBLE_TO_INT: {
      Conversion_12x(instruction, Primitive::kPrimDouble, Primitive::kPrimInt, dex_pc);
      break;
    }

    case Instruction::DOUBLE_TO_LONG: {
      Conversion_12x(instruction, Primitive::kPrimDouble, Primitive::kPrimLong, dex_pc);
      break;
    }

    case Instruction::DOUBLE_TO_FLOAT: {
      Conversion_12x(instruction, Primitive::kPrimDouble, Primitive::kPrimFloat, dex_pc);
      break;
    }

    case Instruction::INT_TO_BYTE: {
      Conversion_12x(instruction, Primitive::kPrimInt, Primitive::kPrimByte, dex_pc);
      break;
    }

    case Instruction::INT_TO_SHORT: {
      Conversion_12x(instruction, Primitive::kPrimInt, Primitive::kPrimShort, dex_pc);
      break;
    }

    case Instruction::INT_TO_CHAR: {
      Conversion_12x(instruction, Primitive::kPrimInt, Primitive::kPrimChar, dex_pc);
      break;
    }

    case Instruction::ADD_INT: {
      Binop_23x<HAdd>(instruction, Primitive::kPrimInt, dex_pc);
      break;
    }

    case Instruction::ADD_LONG: {
      Binop_23x<HAdd>(instruction, Primitive::kPrimLong, dex_pc);
      break;
    }

    case Instruction::ADD_DOUBLE: {
      Binop_23x<HAdd>(instruction, Primitive::kPrimDouble, dex_pc);
      break;
    }

    case Instruction::ADD_FLOAT: {
      Binop_23x<HAdd>(instruction, Primitive::kPrimFloat, dex_pc);
      break;
    }

    case Instruction::SUB_INT: {
      Binop_23x<HSub>(instruction, Primitive::kPrimInt, dex_pc);
      break;
    }

    case Instruction::SUB_LONG: {
      Binop_23x<HSub>(instruction, Primitive::kPrimLong, dex_pc);
      break;
    }

    case Instruction::SUB_FLOAT: {
      Binop_23x<HSub>(instruction, Primitive::kPrimFloat, dex_pc);
      break;
    }

    case Instruction::SUB_DOUBLE: {
      Binop_23x<HSub>(instruction, Primitive::kPrimDouble, dex_pc);
      break;
    }

    case Instruction::ADD_INT_2ADDR: {
      Binop_12x<HAdd>(instruction, Primitive::kPrimInt, dex_pc);
      break;
    }

    case Instruction::MUL_INT: {
      Binop_23x<HMul>(instruction, Primitive::kPrimInt, dex_pc);
      break;
    }

    case Instruction::MUL_LONG: {
      Binop_23x<HMul>(instruction, Primitive::kPrimLong, dex_pc);
      break;
    }

    case Instruction::MUL_FLOAT: {
      Binop_23x<HMul>(instruction, Primitive::kPrimFloat, dex_pc);
      break;
    }

    case Instruction::MUL_DOUBLE: {
      Binop_23x<HMul>(instruction, Primitive::kPrimDouble, dex_pc);
      break;
    }

    case Instruction::DIV_INT: {
      BuildCheckedDivRem(instruction.VRegA(), instruction.VRegB(), instruction.VRegC(),
                         dex_pc, Primitive::kPrimInt, false, true);
      break;
    }

    case Instruction::DIV_LONG: {
      BuildCheckedDivRem(instruction.VRegA(), instruction.VRegB(), instruction.VRegC(),
                         dex_pc, Primitive::kPrimLong, false, true);
      break;
    }

    case Instruction::DIV_FLOAT: {
      Binop_23x<HDiv>(instruction, Primitive::kPrimFloat, dex_pc);
      break;
    }

    case Instruction::DIV_DOUBLE: {
      Binop_23x<HDiv>(instruction, Primitive::kPrimDouble, dex_pc);
      break;
    }

    case Instruction::REM_INT: {
      BuildCheckedDivRem(instruction.VRegA(), instruction.VRegB(), instruction.VRegC(),
                         dex_pc, Primitive::kPrimInt, false, false);
      break;
    }

    case Instruction::REM_LONG: {
      BuildCheckedDivRem(instruction.VRegA(), instruction.VRegB(), instruction.VRegC(),
                         dex_pc, Primitive::kPrimLong, false, false);
      break;
    }

    case Instruction::REM_FLOAT: {
      Binop_23x<HRem>(instruction, Primitive::kPrimFloat, dex_pc);
      break;
    }

    case Instruction::REM_DOUBLE: {
      Binop_23x<HRem>(instruction, Primitive::kPrimDouble, dex_pc);
      break;
    }

    case Instruction::AND_INT: {
      Binop_23x<HAnd>(instruction, Primitive::kPrimInt, dex_pc);
      break;
    }

    case Instruction::AND_LONG: {
      Binop_23x<HAnd>(instruction, Primitive::kPrimLong, dex_pc);
      break;
    }

    case Instruction::SHL_INT: {
      Binop_23x_shift<HShl>(instruction, Primitive::kPrimInt, dex_pc);
      break;
    }

    case Instruction::SHL_LONG: {
      Binop_23x_shift<HShl>(instruction, Primitive::kPrimLong, dex_pc);
      break;
    }

    case Instruction::SHR_INT: {
      Binop_23x_shift<HShr>(instruction, Primitive::kPrimInt, dex_pc);
      break;
    }

    case Instruction::SHR_LONG: {
      Binop_23x_shift<HShr>(instruction, Primitive::kPrimLong, dex_pc);
      break;
    }

    case Instruction::USHR_INT: {
      Binop_23x_shift<HUShr>(instruction, Primitive::kPrimInt, dex_pc);
      break;
    }

    case Instruction::USHR_LONG: {
      Binop_23x_shift<HUShr>(instruction, Primitive::kPrimLong, dex_pc);
      break;
    }

    case Instruction::OR_INT: {
      Binop_23x<HOr>(instruction, Primitive::kPrimInt, dex_pc);
      break;
    }

    case Instruction::OR_LONG: {
      Binop_23x<HOr>(instruction, Primitive::kPrimLong, dex_pc);
      break;
    }

    case Instruction::XOR_INT: {
      Binop_23x<HXor>(instruction, Primitive::kPrimInt, dex_pc);
      break;
    }

    case Instruction::XOR_LONG: {
      Binop_23x<HXor>(instruction, Primitive::kPrimLong, dex_pc);
      break;
    }

    case Instruction::ADD_LONG_2ADDR: {
      Binop_12x<HAdd>(instruction, Primitive::kPrimLong, dex_pc);
      break;
    }

    case Instruction::ADD_DOUBLE_2ADDR: {
      Binop_12x<HAdd>(instruction, Primitive::kPrimDouble, dex_pc);
      break;
    }

    case Instruction::ADD_FLOAT_2ADDR: {
      Binop_12x<HAdd>(instruction, Primitive::kPrimFloat, dex_pc);
      break;
    }

    case Instruction::SUB_INT_2ADDR: {
      Binop_12x<HSub>(instruction, Primitive::kPrimInt, dex_pc);
      break;
    }

    case Instruction::SUB_LONG_2ADDR: {
      Binop_12x<HSub>(instruction, Primitive::kPrimLong, dex_pc);
      break;
    }

    case Instruction::SUB_FLOAT_2ADDR: {
      Binop_12x<HSub>(instruction, Primitive::kPrimFloat, dex_pc);
      break;
    }

    case Instruction::SUB_DOUBLE_2ADDR: {
      Binop_12x<HSub>(instruction, Primitive::kPrimDouble, dex_pc);
      break;
    }

    case Instruction::MUL_INT_2ADDR: {
      Binop_12x<HMul>(instruction, Primitive::kPrimInt, dex_pc);
      break;
    }

    case Instruction::MUL_LONG_2ADDR: {
      Binop_12x<HMul>(instruction, Primitive::kPrimLong, dex_pc);
      break;
    }

    case Instruction::MUL_FLOAT_2ADDR: {
      Binop_12x<HMul>(instruction, Primitive::kPrimFloat, dex_pc);
      break;
    }

    case Instruction::MUL_DOUBLE_2ADDR: {
      Binop_12x<HMul>(instruction, Primitive::kPrimDouble, dex_pc);
      break;
    }

    case Instruction::DIV_INT_2ADDR: {
      BuildCheckedDivRem(instruction.VRegA(), instruction.VRegA(), instruction.VRegB(),
                         dex_pc, Primitive::kPrimInt, false, true);
      break;
    }

    case Instruction::DIV_LONG_2ADDR: {
      BuildCheckedDivRem(instruction.VRegA(), instruction.VRegA(), instruction.VRegB(),
                         dex_pc, Primitive::kPrimLong, false, true);
      break;
    }

    case Instruction::REM_INT_2ADDR: {
      BuildCheckedDivRem(instruction.VRegA(), instruction.VRegA(), instruction.VRegB(),
                         dex_pc, Primitive::kPrimInt, false, false);
      break;
    }

    case Instruction::REM_LONG_2ADDR: {
      BuildCheckedDivRem(instruction.VRegA(), instruction.VRegA(), instruction.VRegB(),
                         dex_pc, Primitive::kPrimLong, false, false);
      break;
    }

    case Instruction::REM_FLOAT_2ADDR: {
      Binop_12x<HRem>(instruction, Primitive::kPrimFloat, dex_pc);
      break;
    }

    case Instruction::REM_DOUBLE_2ADDR: {
      Binop_12x<HRem>(instruction, Primitive::kPrimDouble, dex_pc);
      break;
    }

    case Instruction::SHL_INT_2ADDR: {
      Binop_12x_shift<HShl>(instruction, Primitive::kPrimInt, dex_pc);
      break;
    }

    case Instruction::SHL_LONG_2ADDR: {
      Binop_12x_shift<HShl>(instruction, Primitive::kPrimLong, dex_pc);
      break;
    }

    case Instruction::SHR_INT_2ADDR: {
      Binop_12x_shift<HShr>(instruction, Primitive::kPrimInt, dex_pc);
      break;
    }

    case Instruction::SHR_LONG_2ADDR: {
      Binop_12x_shift<HShr>(instruction, Primitive::kPrimLong, dex_pc);
      break;
    }

    case Instruction::USHR_INT_2ADDR: {
      Binop_12x_shift<HUShr>(instruction, Primitive::kPrimInt, dex_pc);
      break;
    }

    case Instruction::USHR_LONG_2ADDR: {
      Binop_12x_shift<HUShr>(instruction, Primitive::kPrimLong, dex_pc);
      break;
    }

    case Instruction::DIV_FLOAT_2ADDR: {
      Binop_12x<HDiv>(instruction, Primitive::kPrimFloat, dex_pc);
      break;
    }

    case Instruction::DIV_DOUBLE_2ADDR: {
      Binop_12x<HDiv>(instruction, Primitive::kPrimDouble, dex_pc);
      break;
    }

    case Instruction::AND_INT_2ADDR: {
      Binop_12x<HAnd>(instruction, Primitive::kPrimInt, dex_pc);
      break;
    }

    case Instruction::AND_LONG_2ADDR: {
      Binop_12x<HAnd>(instruction, Primitive::kPrimLong, dex_pc);
      break;
    }

    case Instruction::OR_INT_2ADDR: {
      Binop_12x<HOr>(instruction, Primitive::kPrimInt, dex_pc);
      break;
    }

    case Instruction::OR_LONG_2ADDR: {
      Binop_12x<HOr>(instruction, Primitive::kPrimLong, dex_pc);
      break;
    }

    case Instruction::XOR_INT_2ADDR: {
      Binop_12x<HXor>(instruction, Primitive::kPrimInt, dex_pc);
      break;
    }

    case Instruction::XOR_LONG_2ADDR: {
      Binop_12x<HXor>(instruction, Primitive::kPrimLong, dex_pc);
      break;
    }

    case Instruction::ADD_INT_LIT16: {
      Binop_22s<HAdd>(instruction, false, dex_pc);
      break;
    }

    case Instruction::AND_INT_LIT16: {
      Binop_22s<HAnd>(instruction, false, dex_pc);
      break;
    }

    case Instruction::OR_INT_LIT16: {
      Binop_22s<HOr>(instruction, false, dex_pc);
      break;
    }

    case Instruction::XOR_INT_LIT16: {
      Binop_22s<HXor>(instruction, false, dex_pc);
      break;
    }

    case Instruction::RSUB_INT: {
      Binop_22s<HSub>(instruction, true, dex_pc);
      break;
    }

    case Instruction::MUL_INT_LIT16: {
      Binop_22s<HMul>(instruction, false, dex_pc);
      break;
    }

    case Instruction::ADD_INT_LIT8: {
      Binop_22b<HAdd>(instruction, false, dex_pc);
      break;
    }

    case Instruction::AND_INT_LIT8: {
      Binop_22b<HAnd>(instruction, false, dex_pc);
      break;
    }

    case Instruction::OR_INT_LIT8: {
      Binop_22b<HOr>(instruction, false, dex_pc);
      break;
    }

    case Instruction::XOR_INT_LIT8: {
      Binop_22b<HXor>(instruction, false, dex_pc);
      break;
    }

    case Instruction::RSUB_INT_LIT8: {
      Binop_22b<HSub>(instruction, true, dex_pc);
      break;
    }

    case Instruction::MUL_INT_LIT8: {
      Binop_22b<HMul>(instruction, false, dex_pc);
      break;
    }

    case Instruction::DIV_INT_LIT16:
    case Instruction::DIV_INT_LIT8: {
      BuildCheckedDivRem(instruction.VRegA(), instruction.VRegB(), instruction.VRegC(),
                         dex_pc, Primitive::kPrimInt, true, true);
      break;
    }

    case Instruction::REM_INT_LIT16:
    case Instruction::REM_INT_LIT8: {
      BuildCheckedDivRem(instruction.VRegA(), instruction.VRegB(), instruction.VRegC(),
                         dex_pc, Primitive::kPrimInt, true, false);
      break;
    }

    case Instruction::SHL_INT_LIT8: {
      Binop_22b<HShl>(instruction, false, dex_pc);
      break;
    }

    case Instruction::SHR_INT_LIT8: {
      Binop_22b<HShr>(instruction, false, dex_pc);
      break;
    }

    case Instruction::USHR_INT_LIT8: {
      Binop_22b<HUShr>(instruction, false, dex_pc);
      break;
    }

    case Instruction::NEW_INSTANCE: {
      uint16_t type_index = instruction.VRegB_21c();
      if (compiler_driver_->IsStringTypeIndex(type_index, dex_file_)) {
        int32_t register_index = instruction.VRegA();
        HFakeString* fake_string = new (arena_) HFakeString(dex_pc);
        current_block_->AddInstruction(fake_string);
        UpdateLocal(register_index, fake_string, dex_pc);
      } else {
        if (!BuildNewInstance(type_index, dex_pc)) {
          return false;
        }
        UpdateLocal(instruction.VRegA(), current_block_->GetLastInstruction(), dex_pc);
      }
      break;
    }

    case Instruction::NEW_ARRAY: {
      uint16_t type_index = instruction.VRegC_22c();
      HInstruction* length = LoadLocal(instruction.VRegB_22c(), Primitive::kPrimInt, dex_pc);
      bool finalizable;
      QuickEntrypointEnum entrypoint = NeedsAccessCheck(type_index, &finalizable)
          ? kQuickAllocArrayWithAccessCheck
          : kQuickAllocArray;
      current_block_->AddInstruction(new (arena_) HNewArray(length,
                                                            graph_->GetCurrentMethod(),
                                                            dex_pc,
                                                            type_index,
                                                            *dex_compilation_unit_->GetDexFile(),
                                                            entrypoint));
      UpdateLocal(instruction.VRegA_22c(), current_block_->GetLastInstruction(), dex_pc);
      break;
    }

    case Instruction::FILLED_NEW_ARRAY: {
      uint32_t number_of_vreg_arguments = instruction.VRegA_35c();
      uint32_t type_index = instruction.VRegB_35c();
      uint32_t args[5];
      instruction.GetVarArgs(args);
      BuildFilledNewArray(dex_pc, type_index, number_of_vreg_arguments, false, args, 0);
      break;
    }

    case Instruction::FILLED_NEW_ARRAY_RANGE: {
      uint32_t number_of_vreg_arguments = instruction.VRegA_3rc();
      uint32_t type_index = instruction.VRegB_3rc();
      uint32_t register_index = instruction.VRegC_3rc();
      BuildFilledNewArray(
          dex_pc, type_index, number_of_vreg_arguments, true, nullptr, register_index);
      break;
    }

    case Instruction::FILL_ARRAY_DATA: {
      BuildFillArrayData(instruction, dex_pc);
      break;
    }

    case Instruction::MOVE_RESULT:
    case Instruction::MOVE_RESULT_WIDE:
    case Instruction::MOVE_RESULT_OBJECT: {
      if (latest_result_ == nullptr) {
        // Only dead code can lead to this situation, where the verifier
        // does not reject the method.
      } else {
        // An Invoke/FilledNewArray and its MoveResult could have landed in
        // different blocks if there was a try/catch block boundary between
        // them. For Invoke, we insert a StoreLocal after the instruction. For
        // FilledNewArray, the local needs to be updated after the array was
        // filled, otherwise we might overwrite an input vreg.
        HStoreLocal* update_local =
            new (arena_) HStoreLocal(GetLocalAt(instruction.VRegA()), latest_result_, dex_pc);
        HBasicBlock* block = latest_result_->GetBlock();
        if (block == current_block_) {
          // MoveResult and the previous instruction are in the same block.
          current_block_->AddInstruction(update_local);
        } else {
          // The two instructions are in different blocks. Insert the MoveResult
          // before the final control-flow instruction of the previous block.
          DCHECK(block->EndsWithControlFlowInstruction());
          DCHECK(current_block_->GetInstructions().IsEmpty());
          block->InsertInstructionBefore(update_local, block->GetLastInstruction());
        }
        latest_result_ = nullptr;
      }
      break;
    }

    case Instruction::CMP_LONG: {
      Binop_23x_cmp(instruction, Primitive::kPrimLong, ComparisonBias::kNoBias, dex_pc);
      break;
    }

    case Instruction::CMPG_FLOAT: {
      Binop_23x_cmp(instruction, Primitive::kPrimFloat, ComparisonBias::kGtBias, dex_pc);
      break;
    }

    case Instruction::CMPG_DOUBLE: {
      Binop_23x_cmp(instruction, Primitive::kPrimDouble, ComparisonBias::kGtBias, dex_pc);
      break;
    }

    case Instruction::CMPL_FLOAT: {
      Binop_23x_cmp(instruction, Primitive::kPrimFloat, ComparisonBias::kLtBias, dex_pc);
      break;
    }

    case Instruction::CMPL_DOUBLE: {
      Binop_23x_cmp(instruction, Primitive::kPrimDouble, ComparisonBias::kLtBias, dex_pc);
      break;
    }

    case Instruction::NOP:
      break;

    case Instruction::IGET:
    case Instruction::IGET_QUICK:
    case Instruction::IGET_WIDE:
    case Instruction::IGET_WIDE_QUICK:
    case Instruction::IGET_OBJECT:
    case Instruction::IGET_OBJECT_QUICK:
    case Instruction::IGET_BOOLEAN:
    case Instruction::IGET_BOOLEAN_QUICK:
    case Instruction::IGET_BYTE:
    case Instruction::IGET_BYTE_QUICK:
    case Instruction::IGET_CHAR:
    case Instruction::IGET_CHAR_QUICK:
    case Instruction::IGET_SHORT:
    case Instruction::IGET_SHORT_QUICK: {
      if (!BuildInstanceFieldAccess(instruction, dex_pc, false)) {
        return false;
      }
      break;
    }

    case Instruction::IPUT:
    case Instruction::IPUT_QUICK:
    case Instruction::IPUT_WIDE:
    case Instruction::IPUT_WIDE_QUICK:
    case Instruction::IPUT_OBJECT:
    case Instruction::IPUT_OBJECT_QUICK:
    case Instruction::IPUT_BOOLEAN:
    case Instruction::IPUT_BOOLEAN_QUICK:
    case Instruction::IPUT_BYTE:
    case Instruction::IPUT_BYTE_QUICK:
    case Instruction::IPUT_CHAR:
    case Instruction::IPUT_CHAR_QUICK:
    case Instruction::IPUT_SHORT:
    case Instruction::IPUT_SHORT_QUICK: {
      if (!BuildInstanceFieldAccess(instruction, dex_pc, true)) {
        return false;
      }
      break;
    }

    case Instruction::SGET:
    case Instruction::SGET_WIDE:
    case Instruction::SGET_OBJECT:
    case Instruction::SGET_BOOLEAN:
    case Instruction::SGET_BYTE:
    case Instruction::SGET_CHAR:
    case Instruction::SGET_SHORT: {
      if (!BuildStaticFieldAccess(instruction, dex_pc, false)) {
        return false;
      }
      break;
    }

    case Instruction::SPUT:
    case Instruction::SPUT_WIDE:
    case Instruction::SPUT_OBJECT:
    case Instruction::SPUT_BOOLEAN:
    case Instruction::SPUT_BYTE:
    case Instruction::SPUT_CHAR:
    case Instruction::SPUT_SHORT: {
      if (!BuildStaticFieldAccess(instruction, dex_pc, true)) {
        return false;
      }
      break;
    }

#define ARRAY_XX(kind, anticipated_type)                                          \
    case Instruction::AGET##kind: {                                               \
      BuildArrayAccess(instruction, dex_pc, false, anticipated_type);         \
      break;                                                                      \
    }                                                                             \
    case Instruction::APUT##kind: {                                               \
      BuildArrayAccess(instruction, dex_pc, true, anticipated_type);          \
      break;                                                                      \
    }

    ARRAY_XX(, Primitive::kPrimInt);
    ARRAY_XX(_WIDE, Primitive::kPrimLong);
    ARRAY_XX(_OBJECT, Primitive::kPrimNot);
    ARRAY_XX(_BOOLEAN, Primitive::kPrimBoolean);
    ARRAY_XX(_BYTE, Primitive::kPrimByte);
    ARRAY_XX(_CHAR, Primitive::kPrimChar);
    ARRAY_XX(_SHORT, Primitive::kPrimShort);

    case Instruction::ARRAY_LENGTH: {
      HInstruction* object = LoadLocal(instruction.VRegB_12x(), Primitive::kPrimNot, dex_pc);
      // No need for a temporary for the null check, it is the only input of the following
      // instruction.
      object = new (arena_) HNullCheck(object, dex_pc);
      current_block_->AddInstruction(object);
      current_block_->AddInstruction(new (arena_) HArrayLength(object, dex_pc));
      UpdateLocal(instruction.VRegA_12x(), current_block_->GetLastInstruction(), dex_pc);
      break;
    }

    case Instruction::CONST_STRING: {
      current_block_->AddInstruction(
          new (arena_) HLoadString(graph_->GetCurrentMethod(), instruction.VRegB_21c(), dex_pc));
      UpdateLocal(instruction.VRegA_21c(), current_block_->GetLastInstruction(), dex_pc);
      break;
    }

    case Instruction::CONST_STRING_JUMBO: {
      current_block_->AddInstruction(
          new (arena_) HLoadString(graph_->GetCurrentMethod(), instruction.VRegB_31c(), dex_pc));
      UpdateLocal(instruction.VRegA_31c(), current_block_->GetLastInstruction(), dex_pc);
      break;
    }

    case Instruction::CONST_CLASS: {
      uint16_t type_index = instruction.VRegB_21c();
      bool type_known_final;
      bool type_known_abstract;
      bool dont_use_is_referrers_class;
      // `CanAccessTypeWithoutChecks` will tell whether the method being
      // built is trying to access its own class, so that the generated
      // code can optimize for this case. However, the optimization does not
      // work for inlining, so we use `IsOutermostCompilingClass` instead.
      bool can_access = compiler_driver_->CanAccessTypeWithoutChecks(
          dex_compilation_unit_->GetDexMethodIndex(), *dex_file_, type_index,
          &type_known_final, &type_known_abstract, &dont_use_is_referrers_class);
      current_block_->AddInstruction(new (arena_) HLoadClass(
          graph_->GetCurrentMethod(),
          type_index,
          *dex_file_,
          IsOutermostCompilingClass(type_index),
          dex_pc,
          !can_access,
          compiler_driver_->CanAssumeTypeIsPresentInDexCache(*dex_file_, type_index)));
      UpdateLocal(instruction.VRegA_21c(), current_block_->GetLastInstruction(), dex_pc);
      break;
    }

    case Instruction::MOVE_EXCEPTION: {
      current_block_->AddInstruction(new (arena_) HLoadException(dex_pc));
      UpdateLocal(instruction.VRegA_11x(), current_block_->GetLastInstruction(), dex_pc);
      current_block_->AddInstruction(new (arena_) HClearException(dex_pc));
      break;
    }

    case Instruction::THROW: {
      HInstruction* exception = LoadLocal(instruction.VRegA_11x(), Primitive::kPrimNot, dex_pc);
      current_block_->AddInstruction(new (arena_) HThrow(exception, dex_pc));
      // A throw instruction must branch to the exit block.
      current_block_->AddSuccessor(exit_block_);
      // We finished building this block. Set the current block to null to avoid
      // adding dead instructions to it.
      current_block_ = nullptr;
      break;
    }

    case Instruction::INSTANCE_OF: {
      uint8_t destination = instruction.VRegA_22c();
      uint8_t reference = instruction.VRegB_22c();
      uint16_t type_index = instruction.VRegC_22c();
      BuildTypeCheck(instruction, destination, reference, type_index, dex_pc);
      break;
    }

    case Instruction::CHECK_CAST: {
      uint8_t reference = instruction.VRegA_21c();
      uint16_t type_index = instruction.VRegB_21c();
      BuildTypeCheck(instruction, -1, reference, type_index, dex_pc);
      break;
    }

    case Instruction::MONITOR_ENTER: {
      current_block_->AddInstruction(new (arena_) HMonitorOperation(
          LoadLocal(instruction.VRegA_11x(), Primitive::kPrimNot, dex_pc),
          HMonitorOperation::kEnter,
          dex_pc));
      break;
    }

    case Instruction::MONITOR_EXIT: {
      current_block_->AddInstruction(new (arena_) HMonitorOperation(
          LoadLocal(instruction.VRegA_11x(), Primitive::kPrimNot, dex_pc),
          HMonitorOperation::kExit,
          dex_pc));
      break;
    }

    case Instruction::PACKED_SWITCH: {
      BuildPackedSwitch(instruction, dex_pc);
      break;
    }

    case Instruction::SPARSE_SWITCH: {
      BuildSparseSwitch(instruction, dex_pc);
      break;
    }

    default:
      VLOG(compiler) << "Did not compile "
                     << PrettyMethod(dex_compilation_unit_->GetDexMethodIndex(), *dex_file_)
                     << " because of unhandled instruction "
                     << instruction.Name();
      MaybeRecordStat(MethodCompilationStat::kNotCompiledUnhandledInstruction);
      return false;
  }
  return true;
}  // NOLINT(readability/fn_size)

HLocal* HGraphBuilder::GetLocalAt(uint32_t register_index) const {
  return locals_[register_index];
}

void HGraphBuilder::UpdateLocal(uint32_t register_index,
                                HInstruction* instruction,
                                uint32_t dex_pc) const {
  HLocal* local = GetLocalAt(register_index);
  current_block_->AddInstruction(new (arena_) HStoreLocal(local, instruction, dex_pc));
}

HInstruction* HGraphBuilder::LoadLocal(uint32_t register_index,
                                       Primitive::Type type,
                                       uint32_t dex_pc) const {
  HLocal* local = GetLocalAt(register_index);
  current_block_->AddInstruction(new (arena_) HLoadLocal(local, type, dex_pc));
  return current_block_->GetLastInstruction();
}

}  // namespace art
