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

#include "gvn.h"
#include "side_effects_analysis.h"

namespace art {

/**
 * A node in the collision list of a ValueSet. Encodes the instruction,
 * the hash code, and the next node in the collision list.
 */
class ValueSetNode : public ArenaObject<kArenaAllocMisc> {
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
class ValueSet : public ArenaObject<kArenaAllocMisc> {
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

  // Returns whether `instruction` is in the set.
  HInstruction* IdentityLookup(HInstruction* instruction) const {
    size_t hash_code = instruction->ComputeHashCode();
    size_t index = hash_code % kDefaultNumberOfEntries;
    HInstruction* existing = table_[index];
    if (existing != nullptr && existing == instruction) {
      return existing;
    }

    for (ValueSetNode* node = collisions_; node != nullptr; node = node->GetNext()) {
      if (node->GetHashCode() == hash_code) {
        existing = node->GetInstruction();
        if (existing == instruction) {
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

    for (ValueSetNode* current = collisions_, *previous = nullptr;
         current != nullptr;
         current = current->GetNext()) {
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

  void Clear() {
    number_of_entries_ = 0;
    collisions_ = nullptr;
    for (size_t i = 0; i < kDefaultNumberOfEntries; ++i) {
      table_[i] = nullptr;
    }
  }

  // Update this `ValueSet` by intersecting with instructions in `other`.
  void IntersectionWith(ValueSet* other) {
    if (IsEmpty()) {
      return;
    } else if (other->IsEmpty()) {
      Clear();
    } else {
      for (size_t i = 0; i < kDefaultNumberOfEntries; ++i) {
        if (table_[i] != nullptr && other->IdentityLookup(table_[i]) == nullptr) {
          --number_of_entries_;
          table_[i] = nullptr;
        }
      }
      for (ValueSetNode* current = collisions_, *previous = nullptr;
           current != nullptr;
           current = current->GetNext()) {
        if (other->IdentityLookup(current->GetInstruction()) == nullptr) {
          if (previous == nullptr) {
            collisions_ = current->GetNext();
          } else {
            previous->SetNext(current->GetNext());
          }
          --number_of_entries_;
        } else {
          previous = current;
        }
      }
    }
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
  GlobalValueNumberer(ArenaAllocator* allocator,
                      HGraph* graph,
                      const SideEffectsAnalysis& side_effects)
      : graph_(graph),
        allocator_(allocator),
        side_effects_(side_effects),
        sets_(allocator, graph->GetBlocks().Size(), nullptr) {}

  void Run();

 private:
  // Per-block GVN. Will also update the ValueSet of the dominated and
  // successor blocks.
  void VisitBasicBlock(HBasicBlock* block);

  HGraph* graph_;
  ArenaAllocator* const allocator_;
  const SideEffectsAnalysis& side_effects_;

  // ValueSet for blocks. Initially null, but for an individual block they
  // are allocated and populated by the dominator, and updated by all blocks
  // in the path from the dominator to the block.
  GrowableArray<ValueSet*> sets_;

  DISALLOW_COPY_AND_ASSIGN(GlobalValueNumberer);
};

void GlobalValueNumberer::Run() {
  DCHECK(side_effects_.HasRun());
  sets_.Put(graph_->GetEntryBlock()->GetBlockId(), new (allocator_) ValueSet(allocator_));

  // Use the reverse post order to ensure the non back-edge predecessors of a block are
  // visited before the block itself.
  for (HReversePostOrderIterator it(*graph_); !it.Done(); it.Advance()) {
    VisitBasicBlock(it.Current());
  }
}

void GlobalValueNumberer::VisitBasicBlock(HBasicBlock* block) {
  ValueSet* set = nullptr;
  const GrowableArray<HBasicBlock*>& predecessors = block->GetPredecessors();
  if (predecessors.Size() == 0 || predecessors.Get(0)->IsEntryBlock()) {
    // The entry block should only accumulate constant instructions, and
    // the builder puts constants only in the entry block.
    // Therefore, there is no need to propagate the value set to the next block.
    set = new (allocator_) ValueSet(allocator_);
  } else {
    HBasicBlock* dominator = block->GetDominator();
    set = sets_.Get(dominator->GetBlockId())->Copy();
    if (dominator->GetSuccessors().Size() != 1 || dominator->GetSuccessors().Get(0) != block) {
      // We have to copy if the dominator has other successors, or `block` is not a successor
      // of the dominator.
      set = set->Copy();
    }
    if (!set->IsEmpty()) {
      if (block->IsLoopHeader()) {
        DCHECK_EQ(block->GetDominator(), block->GetLoopInformation()->GetPreHeader());
        set->Kill(side_effects_.GetLoopEffects(block));
      } else if (predecessors.Size() > 1) {
        for (size_t i = 0, e = predecessors.Size(); i < e; ++i) {
          set->IntersectionWith(sets_.Get(predecessors.Get(i)->GetBlockId()));
          if (set->IsEmpty()) {
            break;
          }
        }
      }
    }
  }

  sets_.Put(block->GetBlockId(), set);

  HInstruction* current = block->GetFirstInstruction();
  while (current != nullptr) {
    set->Kill(current->GetSideEffects());
    // Save the next instruction in case `current` is removed from the graph.
    HInstruction* next = current->GetNext();
    if (current->CanBeMoved()) {
      HInstruction* existing = set->Lookup(current);
      if (existing != nullptr) {
        current->ReplaceWith(existing);
        current->GetBlock()->RemoveInstruction(current);
      } else {
        set->Add(current);
      }
    }
    current = next;
  }
}

void GVNOptimization::Run() {
  GlobalValueNumberer gvn(graph_->GetArena(), graph_, side_effects_);
  gvn.Run();
}

}  // namespace art
