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
#include "utils.h"

#include "utils/arena_bit_vector.h"
#include "base/bit_vector-inl.h"

namespace art {

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
  // Constructs an empty ValueSet which owns all its buckets.
  explicit ValueSet(ArenaAllocator* allocator)
      : allocator_(allocator),
        num_buckets_(kMinimumNumberOfBuckets),
        buckets_(allocator->AllocArray<Node*>(num_buckets_)),
        buckets_owned_(allocator, num_buckets_, false),
        num_entries_(0) {
    // ArenaAllocator returns zeroed memory, so no need to set buckets to null.
    DCHECK(IsPowerOfTwo(num_buckets_));
    buckets_owned_.SetInitialBits(num_buckets_);
  }

  // Copy constructor. Depending on the load factor, it will either make a deep
  // copy (all buckets owned) or a shallow one (buckets pointing to the parent).
  ValueSet(ArenaAllocator* allocator, const ValueSet& to_copy)
      : allocator_(allocator),
        num_buckets_(to_copy.IdealBucketCount()),
        buckets_(allocator->AllocArray<Node*>(num_buckets_)),
        buckets_owned_(allocator, num_buckets_, false),
        num_entries_(to_copy.num_entries_) {
    // ArenaAllocator returns zeroed memory, so entries of buckets_ and
    // buckets_owned_ are initialized to null and false, respectively.
    DCHECK(IsPowerOfTwo(num_buckets_));
    if (num_buckets_ == to_copy.num_buckets_) {
      // Hash table remains the same size. We copy the bucket pointers and leave
      // all buckets_owned_ bits false.
      memcpy(buckets_, to_copy.buckets_, num_buckets_ * sizeof(Node*));
    } else {
      // Hash table size changes. We copy and rehash all entries, and set all
      // buckets_owned_ bits to true.
      for (size_t i = 0; i < to_copy.num_buckets_; ++i) {
        for (Node* node = to_copy.buckets_[i]; node != nullptr; node = node->GetNext()) {
          size_t new_index = BucketIndex(node->GetHashCode());
          buckets_[new_index] = node->Dup(allocator_, buckets_[new_index]);
        }
      }
      buckets_owned_.SetInitialBits(num_buckets_);
    }
  }

  // Adds an instruction in the set.
  void Add(HInstruction* instruction) {
    DCHECK(Lookup(instruction) == nullptr);
    size_t hash_code = HashCode(instruction);
    size_t index = BucketIndex(hash_code);

    if (!buckets_owned_.IsBitSet(index)) {
      CloneBucket(index);
    }
    buckets_[index] = new (allocator_) Node(instruction, hash_code, buckets_[index]);
    ++num_entries_;
  }

  // If in the set, returns an equivalent instruction to the given instruction.
  // Returns null otherwise.
  HInstruction* Lookup(HInstruction* instruction) const {
    size_t hash_code = HashCode(instruction);
    size_t index = BucketIndex(hash_code);

    for (Node* node = buckets_[index]; node != nullptr; node = node->GetNext()) {
      if (node->GetHashCode() == hash_code) {
        HInstruction* existing = node->GetInstruction();
        if (existing->Equals(instruction)) {
          return existing;
        }
      }
    }
    return nullptr;
  }

  // Returns whether instruction is in the set.
  bool Contains(HInstruction* instruction) const {
    size_t hash_code = HashCode(instruction);
    size_t index = BucketIndex(hash_code);

    for (Node* node = buckets_[index]; node != nullptr; node = node->GetNext()) {
      if (node->GetInstruction() == instruction) {
        return true;
      }
    }
    return false;
  }

  // Removes all instructions in the set affected by the given side effects.
  void Kill(SideEffects side_effects) {
    DeleteAllImpureWhich([side_effects](Node* node) {
      return node->GetInstruction()->GetSideEffects().DependsOn(side_effects);
    });
  }

  // Updates this set by intersecting with instructions in a predecessor's set.
  void IntersectWith(ValueSet* predecessor) {
    if (IsEmpty()) {
      return;
    } else if (predecessor->IsEmpty()) {
      Clear();
    } else {
      // Pure instructions do not need to be tested because only impure
      // instructions can be killed.
      DeleteAllImpureWhich([predecessor](Node* node) {
        return !predecessor->Contains(node->GetInstruction());
      });
    }
  }

  bool IsEmpty() const { return num_entries_ == 0; }
  size_t GetNumberOfEntries() const { return num_entries_; }

 private:
  class Node : public ArenaObject<kArenaAllocMisc> {
   public:
    Node(HInstruction* instruction, size_t hash_code, Node* next)
        : instruction_(instruction), hash_code_(hash_code), next_(next) {}

    size_t GetHashCode() const { return hash_code_; }
    HInstruction* GetInstruction() const { return instruction_; }
    Node* GetNext() const { return next_; }
    void SetNext(Node* node) { next_ = node; }

    Node* Dup(ArenaAllocator* allocator, Node* new_next = nullptr) {
      return new (allocator) Node(instruction_, hash_code_, new_next);
    }

   private:
    HInstruction* const instruction_;
    const size_t hash_code_;
    Node* next_;

    DISALLOW_COPY_AND_ASSIGN(Node);
  };

  // Creates our own copy of a bucket that is currently pointing to a parent.
  // This algorithm can be called while iterating over the bucket because it
  // preserves the order of entries in the bucket and will return the clone of
  // the given 'iterator'.
  Node* CloneBucket(size_t index, Node* iterator = nullptr) {
    DCHECK(!buckets_owned_.IsBitSet(index));
    Node* clone_current = nullptr;
    Node* clone_previous = nullptr;
    Node* clone_iterator = nullptr;
    for (Node* node = buckets_[index]; node != nullptr; node = node->GetNext()) {
      clone_current = node->Dup(allocator_, nullptr);
      if (node == iterator) {
        clone_iterator = clone_current;
      }
      if (clone_previous == nullptr) {
        buckets_[index] = clone_current;
      } else {
        clone_previous->SetNext(clone_current);
      }
      clone_previous = clone_current;
    }
    buckets_owned_.SetBit(index);
    return clone_iterator;
  }

  void Clear() {
    num_entries_ = 0;
    for (size_t i = 0; i < num_buckets_; ++i) {
      buckets_[i] = nullptr;
    }
    buckets_owned_.SetInitialBits(num_buckets_);
  }

  // Iterates over buckets with impure instructions (even indices) and deletes
  // the ones on which 'cond' returns true.
  template<typename Functor>
  void DeleteAllImpureWhich(Functor cond) {
    for (size_t i = 0; i < num_buckets_; i += 2) {
      Node* node = buckets_[i];
      Node* previous = nullptr;

      if (node == nullptr) {
        continue;
      }

      if (!buckets_owned_.IsBitSet(i)) {
        // Bucket is not owned but maybe we won't need to change it at all.
        // Iterate as long as the entries don't satisfy 'cond'.
        while (node != nullptr) {
          if (cond(node)) {
            // We do need to delete an entry but we do not own the bucket.
            // Clone the bucket, make sure 'previous' and 'node' point to
            // the cloned entries and break.
            previous = CloneBucket(i, previous);
            node = (previous == nullptr) ? buckets_[i] : previous->GetNext();
            break;
          }
          previous = node;
          node = node->GetNext();
        }
      }

      // By this point we either own the bucket and can start deleting entries,
      // or we do not own it but no entries matched 'cond'.
      DCHECK(buckets_owned_.IsBitSet(i) || node == nullptr);

      // We iterate over the remainder of entries and delete those that match
      // the given condition.
      while (node != nullptr) {
        Node* next = node->GetNext();
        if (cond(node)) {
          if (previous == nullptr) {
            buckets_[i] = next;
          } else {
            previous->SetNext(next);
          }
        } else {
          previous = node;
        }
        node = next;
      }
    }
  }

  // Computes a bucket count such that the load factor is reasonable.
  // This is estimated as (num_entries_ * 1.5) and rounded up to nearest pow2.
  size_t IdealBucketCount() const {
    size_t bucket_count = RoundUpToPowerOfTwo(num_entries_ + (num_entries_ >> 1));
    if (bucket_count > kMinimumNumberOfBuckets) {
      return bucket_count;
    } else {
      return kMinimumNumberOfBuckets;
    }
  }

  // Generates a hash code for an instruction. Pure instructions are put into
  // odd buckets to speed up deletion.
  size_t HashCode(HInstruction* instruction) const {
    size_t hash_code = instruction->ComputeHashCode();
    if (instruction->GetSideEffects().HasDependencies()) {
      return (hash_code << 1) | 0;
    } else {
      return (hash_code << 1) | 1;
    }
  }

  // Converts a hash code to a bucket index.
  size_t BucketIndex(size_t hash_code) const {
    return hash_code & (num_buckets_ - 1);
  }

  ArenaAllocator* const allocator_;

  // The internal bucket implementation of the set.
  size_t const num_buckets_;
  Node** const buckets_;

  // Flags specifying which buckets were copied into the set from its parent.
  // If a flag is not set, the corresponding bucket points to entries in the
  // parent and must be cloned prior to making changes.
  ArenaBitVector buckets_owned_;

  // The number of entries in the set.
  size_t num_entries_;

  static constexpr size_t kMinimumNumberOfBuckets = 8;

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
    ValueSet* dominator_set = sets_.Get(dominator->GetBlockId());
    if (dominator->GetSuccessors().Size() == 1) {
      DCHECK_EQ(dominator->GetSuccessors().Get(0), block);
      set = dominator_set;
    } else {
      // We have to copy if the dominator has other successors, or `block` is not a successor
      // of the dominator.
      set = new (allocator_) ValueSet(allocator_, *dominator_set);
    }
    if (!set->IsEmpty()) {
      if (block->IsLoopHeader()) {
        DCHECK_EQ(block->GetDominator(), block->GetLoopInformation()->GetPreHeader());
        set->Kill(side_effects_.GetLoopEffects(block));
      } else if (predecessors.Size() > 1) {
        for (size_t i = 0, e = predecessors.Size(); i < e; ++i) {
          set->IntersectWith(sets_.Get(predecessors.Get(i)->GetBlockId()));
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
      if (current->IsBinaryOperation() && current->AsBinaryOperation()->IsCommutative()) {
        // For commutative ops, (x op y) will be treated the same as (y op x)
        // after fixed ordering.
        current->AsBinaryOperation()->OrderInputs();
      }
      HInstruction* existing = set->Lookup(current);
      if (existing != nullptr) {
        // This replacement doesn't make more OrderInputs() necessary since
        // current is either used by an instruction that it dominates,
        // which hasn't been visited yet due to the order we visit instructions.
        // Or current is used by a phi, and we don't do OrderInputs() on a phi anyway.
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
