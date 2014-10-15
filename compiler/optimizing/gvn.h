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

#ifndef ART_COMPILER_OPTIMIZING_GVN_H_
#define ART_COMPILER_OPTIMIZING_GVN_H_

#include "nodes.h"

namespace art {

/**
 * A node in the collision list of a ValueSet. Encodes the instruction,
 * the hash code, and the next node in the collision list.
 */
class ValueSetNode : public ArenaObject {
 public:
  ValueSetNode(HInstruction* instruction, size_t hash_code, ValueSetNode* next)
      : instruction_(instruction), hash_code_(hash_code), next_(next) {}

  size_t GetHashCode() const { return hash_code_; }
  HInstruction* GetInstruction() const { return instruction_; }
  ValueSetNode* GetNext() const { return next_; }
  void SetNext(ValueSetNode* node) { next_ = node; }

 private:
  HInstruction* const instruction_;
  const size_t hash_code_;
  ValueSetNode* next_;

  DISALLOW_COPY_AND_ASSIGN(ValueSetNode);
};

/**
 * A ValueSet holds instructions that can replace other instructions. It is updated
 * through the `Add` method, and the `Kill` method. The `Kill` method removes
 * instructions that are affected by the given side effect.
 *
 * The `Lookup` method returns an equivalent instruction to the given instruction
 * if there is one in the set. In GVN, we would say those instructions have the
 * same "number".
 */
class ValueSet : public ArenaObject {
 public:
  explicit ValueSet(ArenaAllocator* allocator)
      : allocator_(allocator), number_of_entries_(0), collisions_(nullptr) {
    for (size_t i = 0; i < kDefaultNumberOfEntries; ++i) {
      table_[i] = nullptr;
    }
  }

  // Adds an instruction in the set.
  void Add(HInstruction* instruction) {
    DCHECK(Lookup(instruction) == nullptr);
    size_t hash_code = instruction->ComputeHashCode();
    size_t index = hash_code % kDefaultNumberOfEntries;
    if (table_[index] == nullptr) {
      table_[index] = instruction;
    } else {
      collisions_ = new (allocator_) ValueSetNode(instruction, hash_code, collisions_);
    }
    ++number_of_entries_;
  }

  // If in the set, returns an equivalent instruction to the given instruction. Returns
  // null otherwise.
  HInstruction* Lookup(HInstruction* instruction) const {
    size_t hash_code = instruction->ComputeHashCode();
    size_t index = hash_code % kDefaultNumberOfEntries;
    HInstruction* existing = table_[index];
    if (existing != nullptr && existing->Equals(instruction)) {
      return existing;
    }

    for (ValueSetNode* node = collisions_; node != nullptr; node = node->GetNext()) {
      if (node->GetHashCode() == hash_code) {
        existing = node->GetInstruction();
        if (existing->Equals(instruction)) {
          return existing;
        }
      }
    }
    return nullptr;
  }

  // Removes all instructions in the set that are affected by the given side effects.
  void Kill(SideEffects side_effects) {
    for (size_t i = 0; i < kDefaultNumberOfEntries; ++i) {
      HInstruction* instruction = table_[i];
      if (instruction != nullptr && instruction->GetSideEffects().DependsOn(side_effects)) {
        table_[i] = nullptr;
        --number_of_entries_;
      }
    }

    ValueSetNode* current = collisions_;
    ValueSetNode* previous = nullptr;
    while (current != nullptr) {
      HInstruction* instruction = current->GetInstruction();
      if (instruction->GetSideEffects().DependsOn(side_effects)) {
        if (previous == nullptr) {
          collisions_ = current->GetNext();
        } else {
          previous->SetNext(current->GetNext());
        }
        --number_of_entries_;
      } else {
        previous = current;
      }
      current = current->GetNext();
    }
  }

  // Returns a copy of this set.
  ValueSet* Copy() const {
    ValueSet* copy = new (allocator_) ValueSet(allocator_);

    for (size_t i = 0; i < kDefaultNumberOfEntries; ++i) {
      copy->table_[i] = table_[i];
    }

    // Note that the order will be inverted in the copy. This is fine, as the order is not
    // relevant for a ValueSet.
    for (ValueSetNode* node = collisions_; node != nullptr; node = node->GetNext()) {
      copy->collisions_ = new (allocator_) ValueSetNode(
          node->GetInstruction(), node->GetHashCode(), copy->collisions_);
    }

    copy->number_of_entries_ = number_of_entries_;
    return copy;
  }

  bool IsEmpty() const { return number_of_entries_ == 0; }
  size_t GetNumberOfEntries() const { return number_of_entries_; }

 private:
  static constexpr size_t kDefaultNumberOfEntries = 8;

  ArenaAllocator* const allocator_;

  // The number of entries in the set.
  size_t number_of_entries_;

  // The internal implementation of the set. It uses a combination of a hash code based
  // fixed-size list, and a linked list to handle hash code collisions.
  // TODO: Tune the fixed size list original size, and support growing it.
  ValueSetNode* collisions_;
  HInstruction* table_[kDefaultNumberOfEntries];

  DISALLOW_COPY_AND_ASSIGN(ValueSet);
};

/**
 * Optimization phase that removes redundant instruction.
 */
class GlobalValueNumberer : public ValueObject {
 public:
  GlobalValueNumberer(ArenaAllocator* allocator, HGraph* graph)
      : allocator_(allocator),
        graph_(graph),
        block_effects_(allocator, graph->GetBlocks().Size()),
        loop_effects_(allocator, graph->GetBlocks().Size()),
        sets_(allocator, graph->GetBlocks().Size()),
        visited_(allocator, graph->GetBlocks().Size()) {
    size_t number_of_blocks = graph->GetBlocks().Size();
    block_effects_.SetSize(number_of_blocks);
    loop_effects_.SetSize(number_of_blocks);
    sets_.SetSize(number_of_blocks);
    visited_.SetSize(number_of_blocks);

    for (size_t i = 0; i < number_of_blocks; ++i) {
      block_effects_.Put(i, SideEffects::None());
      loop_effects_.Put(i, SideEffects::None());
    }
  }

  void Run();

 private:
  // Per-block GVN. Will also update the ValueSet of the dominated and
  // successor blocks.
  void VisitBasicBlock(HBasicBlock* block);

  // Compute side effects of individual blocks and loops. The GVN algorithm
  // will use these side effects to update the ValueSet of individual blocks.
  void ComputeSideEffects();

  void UpdateLoopEffects(HLoopInformation* info, SideEffects effects);
  SideEffects GetLoopEffects(HBasicBlock* block) const;
  SideEffects GetBlockEffects(HBasicBlock* block) const;

  ArenaAllocator* const allocator_;
  HGraph* const graph_;

  // Side effects of individual blocks, that is the union of the side effects
  // of the instructions in the block.
  GrowableArray<SideEffects> block_effects_;

  // Side effects of loops, that is the union of the side effects of the
  // blocks contained in that loop.
  GrowableArray<SideEffects> loop_effects_;

  // ValueSet for blocks. Initially null, but for an individual block they
  // are allocated and populated by the dominator, and updated by all blocks
  // in the path from the dominator to the block.
  GrowableArray<ValueSet*> sets_;

  // Mark visisted blocks. Only used for debugging.
  GrowableArray<bool> visited_;

  ART_FRIEND_TEST(GVNTest, LoopSideEffects);
  DISALLOW_COPY_AND_ASSIGN(GlobalValueNumberer);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_GVN_H_
