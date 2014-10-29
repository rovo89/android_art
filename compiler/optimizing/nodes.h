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

#ifndef ART_COMPILER_OPTIMIZING_NODES_H_
#define ART_COMPILER_OPTIMIZING_NODES_H_

#include "locations.h"
#include "offsets.h"
#include "primitive.h"
#include "utils/arena_object.h"
#include "utils/arena_bit_vector.h"
#include "utils/growable_array.h"

namespace art {

class HBasicBlock;
class HEnvironment;
class HInstruction;
class HIntConstant;
class HGraphVisitor;
class HPhi;
class HSuspendCheck;
class LiveInterval;
class LocationSummary;

static const int kDefaultNumberOfBlocks = 8;
static const int kDefaultNumberOfSuccessors = 2;
static const int kDefaultNumberOfPredecessors = 2;
static const int kDefaultNumberOfDominatedBlocks = 1;
static const int kDefaultNumberOfBackEdges = 1;

enum IfCondition {
  kCondEQ,
  kCondNE,
  kCondLT,
  kCondLE,
  kCondGT,
  kCondGE,
};

class HInstructionList {
 public:
  HInstructionList() : first_instruction_(nullptr), last_instruction_(nullptr) {}

  void AddInstruction(HInstruction* instruction);
  void RemoveInstruction(HInstruction* instruction);

  // Return true if this list contains `instruction`.
  bool Contains(HInstruction* instruction) const;

  // Return true if `instruction1` is found before `instruction2` in
  // this instruction list and false otherwise.  Abort if none
  // of these instructions is found.
  bool FoundBefore(const HInstruction* instruction1,
                   const HInstruction* instruction2) const;

 private:
  HInstruction* first_instruction_;
  HInstruction* last_instruction_;

  friend class HBasicBlock;
  friend class HInstructionIterator;
  friend class HBackwardInstructionIterator;

  DISALLOW_COPY_AND_ASSIGN(HInstructionList);
};

// Control-flow graph of a method. Contains a list of basic blocks.
class HGraph : public ArenaObject {
 public:
  explicit HGraph(ArenaAllocator* arena)
      : arena_(arena),
        blocks_(arena, kDefaultNumberOfBlocks),
        reverse_post_order_(arena, kDefaultNumberOfBlocks),
        maximum_number_of_out_vregs_(0),
        number_of_vregs_(0),
        number_of_in_vregs_(0),
        number_of_temporaries_(0),
        current_instruction_id_(0) {}

  ArenaAllocator* GetArena() const { return arena_; }
  const GrowableArray<HBasicBlock*>& GetBlocks() const { return blocks_; }
  HBasicBlock* GetBlock(size_t id) const { return blocks_.Get(id); }

  HBasicBlock* GetEntryBlock() const { return entry_block_; }
  HBasicBlock* GetExitBlock() const { return exit_block_; }

  void SetEntryBlock(HBasicBlock* block) { entry_block_ = block; }
  void SetExitBlock(HBasicBlock* block) { exit_block_ = block; }

  void AddBlock(HBasicBlock* block);

  void BuildDominatorTree();
  void TransformToSSA();
  void SimplifyCFG();

  // Find all natural loops in this graph. Aborts computation and returns false
  // if one loop is not natural, that is the header does not dominate the back
  // edge.
  bool FindNaturalLoops() const;

  void SplitCriticalEdge(HBasicBlock* block, HBasicBlock* successor);
  void SimplifyLoop(HBasicBlock* header);

  int GetNextInstructionId() {
    return current_instruction_id_++;
  }

  uint16_t GetMaximumNumberOfOutVRegs() const {
    return maximum_number_of_out_vregs_;
  }

  void UpdateMaximumNumberOfOutVRegs(uint16_t new_value) {
    maximum_number_of_out_vregs_ = std::max(new_value, maximum_number_of_out_vregs_);
  }

  void UpdateNumberOfTemporaries(size_t count) {
    number_of_temporaries_ = std::max(count, number_of_temporaries_);
  }

  size_t GetNumberOfTemporaries() const {
    return number_of_temporaries_;
  }

  void SetNumberOfVRegs(uint16_t number_of_vregs) {
    number_of_vregs_ = number_of_vregs;
  }

  uint16_t GetNumberOfVRegs() const {
    return number_of_vregs_;
  }

  void SetNumberOfInVRegs(uint16_t value) {
    number_of_in_vregs_ = value;
  }

  uint16_t GetNumberOfInVRegs() const {
    return number_of_in_vregs_;
  }

  uint16_t GetNumberOfLocalVRegs() const {
    return number_of_vregs_ - number_of_in_vregs_;
  }

  const GrowableArray<HBasicBlock*>& GetReversePostOrder() const {
    return reverse_post_order_;
  }

 private:
  HBasicBlock* FindCommonDominator(HBasicBlock* first, HBasicBlock* second) const;
  void VisitBlockForDominatorTree(HBasicBlock* block,
                                  HBasicBlock* predecessor,
                                  GrowableArray<size_t>* visits);
  void FindBackEdges(ArenaBitVector* visited);
  void VisitBlockForBackEdges(HBasicBlock* block,
                              ArenaBitVector* visited,
                              ArenaBitVector* visiting);
  void RemoveDeadBlocks(const ArenaBitVector& visited) const;

  ArenaAllocator* const arena_;

  // List of blocks in insertion order.
  GrowableArray<HBasicBlock*> blocks_;

  // List of blocks to perform a reverse post order tree traversal.
  GrowableArray<HBasicBlock*> reverse_post_order_;

  HBasicBlock* entry_block_;
  HBasicBlock* exit_block_;

  // The maximum number of virtual registers arguments passed to a HInvoke in this graph.
  uint16_t maximum_number_of_out_vregs_;

  // The number of virtual registers in this method. Contains the parameters.
  uint16_t number_of_vregs_;

  // The number of virtual registers used by parameters of this method.
  uint16_t number_of_in_vregs_;

  // The number of temporaries that will be needed for the baseline compiler.
  size_t number_of_temporaries_;

  // The current id to assign to a newly added instruction. See HInstruction.id_.
  int current_instruction_id_;

  DISALLOW_COPY_AND_ASSIGN(HGraph);
};

class HLoopInformation : public ArenaObject {
 public:
  HLoopInformation(HBasicBlock* header, HGraph* graph)
      : header_(header),
        suspend_check_(nullptr),
        back_edges_(graph->GetArena(), kDefaultNumberOfBackEdges),
        // Make bit vector growable, as the number of blocks may change.
        blocks_(graph->GetArena(), graph->GetBlocks().Size(), true) {}

  HBasicBlock* GetHeader() const {
    return header_;
  }

  HSuspendCheck* GetSuspendCheck() const { return suspend_check_; }
  void SetSuspendCheck(HSuspendCheck* check) { suspend_check_ = check; }
  bool HasSuspendCheck() const { return suspend_check_ != nullptr; }

  void AddBackEdge(HBasicBlock* back_edge) {
    back_edges_.Add(back_edge);
  }

  void RemoveBackEdge(HBasicBlock* back_edge) {
    back_edges_.Delete(back_edge);
  }

  bool IsBackEdge(HBasicBlock* block) {
    for (size_t i = 0, e = back_edges_.Size(); i < e; ++i) {
      if (back_edges_.Get(i) == block) return true;
    }
    return false;
  }

  int NumberOfBackEdges() const {
    return back_edges_.Size();
  }

  HBasicBlock* GetPreHeader() const;

  const GrowableArray<HBasicBlock*>& GetBackEdges() const {
    return back_edges_;
  }

  void ClearBackEdges() {
    back_edges_.Reset();
  }

  // Find blocks that are part of this loop. Returns whether the loop is a natural loop,
  // that is the header dominates the back edge.
  bool Populate();

  // Returns whether this loop information contains `block`.
  // Note that this loop information *must* be populated before entering this function.
  bool Contains(const HBasicBlock& block) const;

  // Returns whether this loop information is an inner loop of `other`.
  // Note that `other` *must* be populated before entering this function.
  bool IsIn(const HLoopInformation& other) const;

  const ArenaBitVector& GetBlocks() const { return blocks_; }

 private:
  // Internal recursive implementation of `Populate`.
  void PopulateRecursive(HBasicBlock* block);

  HBasicBlock* header_;
  HSuspendCheck* suspend_check_;
  GrowableArray<HBasicBlock*> back_edges_;
  ArenaBitVector blocks_;

  DISALLOW_COPY_AND_ASSIGN(HLoopInformation);
};

static constexpr size_t kNoLifetime = -1;
static constexpr uint32_t kNoDexPc = -1;

// A block in a method. Contains the list of instructions represented
// as a double linked list. Each block knows its predecessors and
// successors.

class HBasicBlock : public ArenaObject {
 public:
  explicit HBasicBlock(HGraph* graph, uint32_t dex_pc = kNoDexPc)
      : graph_(graph),
        predecessors_(graph->GetArena(), kDefaultNumberOfPredecessors),
        successors_(graph->GetArena(), kDefaultNumberOfSuccessors),
        loop_information_(nullptr),
        dominator_(nullptr),
        dominated_blocks_(graph->GetArena(), kDefaultNumberOfDominatedBlocks),
        block_id_(-1),
        dex_pc_(dex_pc),
        lifetime_start_(kNoLifetime),
        lifetime_end_(kNoLifetime) {}

  const GrowableArray<HBasicBlock*>& GetPredecessors() const {
    return predecessors_;
  }

  const GrowableArray<HBasicBlock*>& GetSuccessors() const {
    return successors_;
  }

  const GrowableArray<HBasicBlock*>& GetDominatedBlocks() const {
    return dominated_blocks_;
  }

  bool IsEntryBlock() const {
    return graph_->GetEntryBlock() == this;
  }

  bool IsExitBlock() const {
    return graph_->GetExitBlock() == this;
  }

  void AddBackEdge(HBasicBlock* back_edge) {
    if (loop_information_ == nullptr) {
      loop_information_ = new (graph_->GetArena()) HLoopInformation(this, graph_);
    }
    DCHECK_EQ(loop_information_->GetHeader(), this);
    loop_information_->AddBackEdge(back_edge);
  }

  HGraph* GetGraph() const { return graph_; }

  int GetBlockId() const { return block_id_; }
  void SetBlockId(int id) { block_id_ = id; }

  HBasicBlock* GetDominator() const { return dominator_; }
  void SetDominator(HBasicBlock* dominator) { dominator_ = dominator; }
  void AddDominatedBlock(HBasicBlock* block) { dominated_blocks_.Add(block); }

  int NumberOfBackEdges() const {
    return loop_information_ == nullptr
        ? 0
        : loop_information_->NumberOfBackEdges();
  }

  HInstruction* GetFirstInstruction() const { return instructions_.first_instruction_; }
  HInstruction* GetLastInstruction() const { return instructions_.last_instruction_; }
  const HInstructionList& GetInstructions() const { return instructions_; }
  const HInstructionList& GetPhis() const { return phis_; }
  HInstruction* GetFirstPhi() const { return phis_.first_instruction_; }

  void AddSuccessor(HBasicBlock* block) {
    successors_.Add(block);
    block->predecessors_.Add(this);
  }

  void ReplaceSuccessor(HBasicBlock* existing, HBasicBlock* new_block) {
    size_t successor_index = GetSuccessorIndexOf(existing);
    DCHECK_NE(successor_index, static_cast<size_t>(-1));
    existing->RemovePredecessor(this);
    new_block->predecessors_.Add(this);
    successors_.Put(successor_index, new_block);
  }

  void RemovePredecessor(HBasicBlock* block) {
    predecessors_.Delete(block);
  }

  void ClearAllPredecessors() {
    predecessors_.Reset();
  }

  void AddPredecessor(HBasicBlock* block) {
    predecessors_.Add(block);
    block->successors_.Add(this);
  }

  void SwapPredecessors() {
    DCHECK_EQ(predecessors_.Size(), 2u);
    HBasicBlock* temp = predecessors_.Get(0);
    predecessors_.Put(0, predecessors_.Get(1));
    predecessors_.Put(1, temp);
  }

  size_t GetPredecessorIndexOf(HBasicBlock* predecessor) {
    for (size_t i = 0, e = predecessors_.Size(); i < e; ++i) {
      if (predecessors_.Get(i) == predecessor) {
        return i;
      }
    }
    return -1;
  }

  size_t GetSuccessorIndexOf(HBasicBlock* successor) {
    for (size_t i = 0, e = successors_.Size(); i < e; ++i) {
      if (successors_.Get(i) == successor) {
        return i;
      }
    }
    return -1;
  }

  void AddInstruction(HInstruction* instruction);
  void RemoveInstruction(HInstruction* instruction);
  void InsertInstructionBefore(HInstruction* instruction, HInstruction* cursor);
  // Replace instruction `initial` with `replacement` within this block.
  void ReplaceAndRemoveInstructionWith(HInstruction* initial,
                                       HInstruction* replacement);
  void AddPhi(HPhi* phi);
  void InsertPhiAfter(HPhi* instruction, HPhi* cursor);
  void RemovePhi(HPhi* phi);

  bool IsLoopHeader() const {
    return (loop_information_ != nullptr) && (loop_information_->GetHeader() == this);
  }

  bool IsLoopPreHeaderFirstPredecessor() const {
    DCHECK(IsLoopHeader());
    DCHECK(!GetPredecessors().IsEmpty());
    return GetPredecessors().Get(0) == GetLoopInformation()->GetPreHeader();
  }

  HLoopInformation* GetLoopInformation() const {
    return loop_information_;
  }

  // Set the loop_information_ on this block. This method overrides the current
  // loop_information if it is an outer loop of the passed loop information.
  void SetInLoop(HLoopInformation* info) {
    if (IsLoopHeader()) {
      // Nothing to do. This just means `info` is an outer loop.
    } else if (loop_information_ == nullptr) {
      loop_information_ = info;
    } else if (loop_information_->Contains(*info->GetHeader())) {
      // Block is currently part of an outer loop. Make it part of this inner loop.
      // Note that a non loop header having a loop information means this loop information
      // has already been populated
      loop_information_ = info;
    } else {
      // Block is part of an inner loop. Do not update the loop information.
      // Note that we cannot do the check `info->Contains(loop_information_)->GetHeader()`
      // at this point, because this method is being called while populating `info`.
    }
  }

  bool IsInLoop() const { return loop_information_ != nullptr; }

  // Returns wheter this block dominates the blocked passed as parameter.
  bool Dominates(HBasicBlock* block) const;

  size_t GetLifetimeStart() const { return lifetime_start_; }
  size_t GetLifetimeEnd() const { return lifetime_end_; }

  void SetLifetimeStart(size_t start) { lifetime_start_ = start; }
  void SetLifetimeEnd(size_t end) { lifetime_end_ = end; }

  uint32_t GetDexPc() const { return dex_pc_; }

 private:
  HGraph* const graph_;
  GrowableArray<HBasicBlock*> predecessors_;
  GrowableArray<HBasicBlock*> successors_;
  HInstructionList instructions_;
  HInstructionList phis_;
  HLoopInformation* loop_information_;
  HBasicBlock* dominator_;
  GrowableArray<HBasicBlock*> dominated_blocks_;
  int block_id_;
  // The dex program counter of the first instruction of this block.
  const uint32_t dex_pc_;
  size_t lifetime_start_;
  size_t lifetime_end_;

  DISALLOW_COPY_AND_ASSIGN(HBasicBlock);
};

#define FOR_EACH_CONCRETE_INSTRUCTION(M)                                \
  M(Add, BinaryOperation)                                               \
  M(Condition, BinaryOperation)                                         \
  M(Equal, Condition)                                                   \
  M(NotEqual, Condition)                                                \
  M(LessThan, Condition)                                                \
  M(LessThanOrEqual, Condition)                                         \
  M(GreaterThan, Condition)                                             \
  M(GreaterThanOrEqual, Condition)                                      \
  M(Exit, Instruction)                                                  \
  M(Goto, Instruction)                                                  \
  M(If, Instruction)                                                    \
  M(IntConstant, Constant)                                              \
  M(InvokeStatic, Invoke)                                               \
  M(InvokeVirtual, Invoke)                                              \
  M(LoadLocal, Instruction)                                             \
  M(Local, Instruction)                                                 \
  M(LongConstant, Constant)                                             \
  M(NewInstance, Instruction)                                           \
  M(Not, UnaryOperation)                                                \
  M(ParameterValue, Instruction)                                        \
  M(ParallelMove, Instruction)                                          \
  M(Phi, Instruction)                                                   \
  M(Return, Instruction)                                                \
  M(ReturnVoid, Instruction)                                            \
  M(StoreLocal, Instruction)                                            \
  M(Sub, BinaryOperation)                                               \
  M(Compare, BinaryOperation)                                           \
  M(InstanceFieldGet, Instruction)                                      \
  M(InstanceFieldSet, Instruction)                                      \
  M(ArrayGet, Instruction)                                              \
  M(ArraySet, Instruction)                                              \
  M(ArrayLength, Instruction)                                           \
  M(BoundsCheck, Instruction)                                           \
  M(NullCheck, Instruction)                                             \
  M(Temporary, Instruction)                                             \
  M(SuspendCheck, Instruction)                                          \
  M(Mul, BinaryOperation)                                               \
  M(Neg, UnaryOperation)                                                \
  M(FloatConstant, Constant)                                            \
  M(DoubleConstant, Constant)                                           \
  M(NewArray, Instruction)                                              \

#define FOR_EACH_INSTRUCTION(M)                                         \
  FOR_EACH_CONCRETE_INSTRUCTION(M)                                      \
  M(Constant, Instruction)                                              \
  M(UnaryOperation, Instruction)                                        \
  M(BinaryOperation, Instruction)                                       \
  M(Invoke, Instruction)

#define FORWARD_DECLARATION(type, super) class H##type;
FOR_EACH_INSTRUCTION(FORWARD_DECLARATION)
#undef FORWARD_DECLARATION

#define DECLARE_INSTRUCTION(type)                                       \
  virtual InstructionKind GetKind() const { return k##type; }           \
  virtual const char* DebugName() const { return #type; }               \
  virtual const H##type* As##type() const OVERRIDE { return this; }     \
  virtual H##type* As##type() OVERRIDE { return this; }                 \
  virtual bool InstructionTypeEquals(HInstruction* other) const {       \
    return other->Is##type();                                           \
  }                                                                     \
  virtual void Accept(HGraphVisitor* visitor)

template <typename T>
class HUseListNode : public ArenaObject {
 public:
  HUseListNode(T* user, size_t index, HUseListNode* tail)
      : user_(user), index_(index), tail_(tail) {}

  HUseListNode* GetTail() const { return tail_; }
  T* GetUser() const { return user_; }
  size_t GetIndex() const { return index_; }

  void SetTail(HUseListNode<T>* node) { tail_ = node; }

 private:
  T* const user_;
  const size_t index_;
  HUseListNode<T>* tail_;

  DISALLOW_COPY_AND_ASSIGN(HUseListNode);
};

// Represents the side effects an instruction may have.
class SideEffects : public ValueObject {
 public:
  SideEffects() : flags_(0) {}

  static SideEffects None() {
    return SideEffects(0);
  }

  static SideEffects All() {
    return SideEffects(ChangesSomething().flags_ | DependsOnSomething().flags_);
  }

  static SideEffects ChangesSomething() {
    return SideEffects((1 << kFlagChangesCount) - 1);
  }

  static SideEffects DependsOnSomething() {
    int count = kFlagDependsOnCount - kFlagChangesCount;
    return SideEffects(((1 << count) - 1) << kFlagChangesCount);
  }

  SideEffects Union(SideEffects other) const {
    return SideEffects(flags_ | other.flags_);
  }

  bool HasSideEffects() const {
    size_t all_bits_set = (1 << kFlagChangesCount) - 1;
    return (flags_ & all_bits_set) != 0;
  }

  bool HasAllSideEffects() const {
    size_t all_bits_set = (1 << kFlagChangesCount) - 1;
    return all_bits_set == (flags_ & all_bits_set);
  }

  bool DependsOn(SideEffects other) const {
    size_t depends_flags = other.ComputeDependsFlags();
    return (flags_ & depends_flags) != 0;
  }

  bool HasDependencies() const {
    int count = kFlagDependsOnCount - kFlagChangesCount;
    size_t all_bits_set = (1 << count) - 1;
    return ((flags_ >> kFlagChangesCount) & all_bits_set) != 0;
  }

 private:
  static constexpr int kFlagChangesSomething = 0;
  static constexpr int kFlagChangesCount = kFlagChangesSomething + 1;

  static constexpr int kFlagDependsOnSomething = kFlagChangesCount;
  static constexpr int kFlagDependsOnCount = kFlagDependsOnSomething + 1;

  explicit SideEffects(size_t flags) : flags_(flags) {}

  size_t ComputeDependsFlags() const {
    return flags_ << kFlagChangesCount;
  }

  size_t flags_;
};

class HInstruction : public ArenaObject {
 public:
  explicit HInstruction(SideEffects side_effects)
      : previous_(nullptr),
        next_(nullptr),
        block_(nullptr),
        id_(-1),
        ssa_index_(-1),
        uses_(nullptr),
        env_uses_(nullptr),
        environment_(nullptr),
        locations_(nullptr),
        live_interval_(nullptr),
        lifetime_position_(kNoLifetime),
        side_effects_(side_effects) {}

  virtual ~HInstruction() {}

#define DECLARE_KIND(type, super) k##type,
  enum InstructionKind {
    FOR_EACH_INSTRUCTION(DECLARE_KIND)
  };
#undef DECLARE_KIND

  HInstruction* GetNext() const { return next_; }
  HInstruction* GetPrevious() const { return previous_; }

  HBasicBlock* GetBlock() const { return block_; }
  void SetBlock(HBasicBlock* block) { block_ = block; }
  bool IsInBlock() const { return block_ != nullptr; }
  bool IsInLoop() const { return block_->IsInLoop(); }
  bool IsLoopHeaderPhi() { return IsPhi() && block_->IsLoopHeader(); }

  virtual size_t InputCount() const = 0;
  virtual HInstruction* InputAt(size_t i) const = 0;

  virtual void Accept(HGraphVisitor* visitor) = 0;
  virtual const char* DebugName() const = 0;

  virtual Primitive::Type GetType() const { return Primitive::kPrimVoid; }
  virtual void SetRawInputAt(size_t index, HInstruction* input) = 0;

  virtual bool NeedsEnvironment() const { return false; }
  virtual bool IsControlFlow() const { return false; }
  virtual bool CanThrow() const { return false; }
  bool HasSideEffects() const { return side_effects_.HasSideEffects(); }

  void AddUseAt(HInstruction* user, size_t index) {
    uses_ = new (block_->GetGraph()->GetArena()) HUseListNode<HInstruction>(user, index, uses_);
  }

  void AddEnvUseAt(HEnvironment* user, size_t index) {
    DCHECK(user != nullptr);
    env_uses_ = new (block_->GetGraph()->GetArena()) HUseListNode<HEnvironment>(
        user, index, env_uses_);
  }

  void RemoveUser(HInstruction* user, size_t index);
  void RemoveEnvironmentUser(HEnvironment* user, size_t index);

  HUseListNode<HInstruction>* GetUses() const { return uses_; }
  HUseListNode<HEnvironment>* GetEnvUses() const { return env_uses_; }

  bool HasUses() const { return uses_ != nullptr || env_uses_ != nullptr; }
  bool HasEnvironmentUses() const { return env_uses_ != nullptr; }

  size_t NumberOfUses() const {
    // TODO: Optimize this method if it is used outside of the HGraphVisualizer.
    size_t result = 0;
    HUseListNode<HInstruction>* current = uses_;
    while (current != nullptr) {
      current = current->GetTail();
      ++result;
    }
    return result;
  }

  // Does this instruction strictly dominate `other_instruction`?
  // Returns false if this instruction and `other_instruction` are the same.
  // Aborts if this instruction and `other_instruction` are both phis.
  bool StrictlyDominates(HInstruction* other_instruction) const;

  int GetId() const { return id_; }
  void SetId(int id) { id_ = id; }

  int GetSsaIndex() const { return ssa_index_; }
  void SetSsaIndex(int ssa_index) { ssa_index_ = ssa_index; }
  bool HasSsaIndex() const { return ssa_index_ != -1; }

  bool HasEnvironment() const { return environment_ != nullptr; }
  HEnvironment* GetEnvironment() const { return environment_; }
  void SetEnvironment(HEnvironment* environment) { environment_ = environment; }

  // Returns the number of entries in the environment. Typically, that is the
  // number of dex registers in a method. It could be more in case of inlining.
  size_t EnvironmentSize() const;

  LocationSummary* GetLocations() const { return locations_; }
  void SetLocations(LocationSummary* locations) { locations_ = locations; }

  void ReplaceWith(HInstruction* instruction);
  void ReplaceInput(HInstruction* replacement, size_t index);

  bool HasOnlyOneUse() const {
    return uses_ != nullptr && uses_->GetTail() == nullptr;
  }

#define INSTRUCTION_TYPE_CHECK(type, super)                                    \
  bool Is##type() const { return (As##type() != nullptr); }                    \
  virtual const H##type* As##type() const { return nullptr; }                  \
  virtual H##type* As##type() { return nullptr; }

  FOR_EACH_INSTRUCTION(INSTRUCTION_TYPE_CHECK)
#undef INSTRUCTION_TYPE_CHECK

  // Returns whether the instruction can be moved within the graph.
  virtual bool CanBeMoved() const { return false; }

  // Returns whether the two instructions are of the same kind.
  virtual bool InstructionTypeEquals(HInstruction* other) const { return false; }

  // Returns whether any data encoded in the two instructions is equal.
  // This method does not look at the inputs. Both instructions must be
  // of the same type, otherwise the method has undefined behavior.
  virtual bool InstructionDataEquals(HInstruction* other) const { return false; }

  // Returns whether two instructions are equal, that is:
  // 1) They have the same type and contain the same data,
  // 2) Their inputs are identical.
  bool Equals(HInstruction* other) const;

  virtual InstructionKind GetKind() const = 0;

  virtual size_t ComputeHashCode() const {
    size_t result = GetKind();
    for (size_t i = 0, e = InputCount(); i < e; ++i) {
      result = (result * 31) + InputAt(i)->GetId();
    }
    return result;
  }

  SideEffects GetSideEffects() const { return side_effects_; }

  size_t GetLifetimePosition() const { return lifetime_position_; }
  void SetLifetimePosition(size_t position) { lifetime_position_ = position; }
  LiveInterval* GetLiveInterval() const { return live_interval_; }
  void SetLiveInterval(LiveInterval* interval) { live_interval_ = interval; }
  bool HasLiveInterval() const { return live_interval_ != nullptr; }

 private:
  HInstruction* previous_;
  HInstruction* next_;
  HBasicBlock* block_;

  // An instruction gets an id when it is added to the graph.
  // It reflects creation order. A negative id means the instruction
  // has not been added to the graph.
  int id_;

  // When doing liveness analysis, instructions that have uses get an SSA index.
  int ssa_index_;

  // List of instructions that have this instruction as input.
  HUseListNode<HInstruction>* uses_;

  // List of environments that contain this instruction.
  HUseListNode<HEnvironment>* env_uses_;

  // The environment associated with this instruction. Not null if the instruction
  // might jump out of the method.
  HEnvironment* environment_;

  // Set by the code generator.
  LocationSummary* locations_;

  // Set by the liveness analysis.
  LiveInterval* live_interval_;

  // Set by the liveness analysis, this is the position in a linear
  // order of blocks where this instruction's live interval start.
  size_t lifetime_position_;

  const SideEffects side_effects_;

  friend class HBasicBlock;
  friend class HInstructionList;

  DISALLOW_COPY_AND_ASSIGN(HInstruction);
};

template<typename T>
class HUseIterator : public ValueObject {
 public:
  explicit HUseIterator(HUseListNode<T>* uses) : current_(uses) {}

  bool Done() const { return current_ == nullptr; }

  void Advance() {
    DCHECK(!Done());
    current_ = current_->GetTail();
  }

  HUseListNode<T>* Current() const {
    DCHECK(!Done());
    return current_;
  }

 private:
  HUseListNode<T>* current_;

  friend class HValue;
};

// A HEnvironment object contains the values of virtual registers at a given location.
class HEnvironment : public ArenaObject {
 public:
  HEnvironment(ArenaAllocator* arena, size_t number_of_vregs) : vregs_(arena, number_of_vregs) {
    vregs_.SetSize(number_of_vregs);
    for (size_t i = 0; i < number_of_vregs; i++) {
      vregs_.Put(i, nullptr);
    }
  }

  void Populate(const GrowableArray<HInstruction*>& env) {
    for (size_t i = 0; i < env.Size(); i++) {
      HInstruction* instruction = env.Get(i);
      vregs_.Put(i, instruction);
      if (instruction != nullptr) {
        instruction->AddEnvUseAt(this, i);
      }
    }
  }

  void SetRawEnvAt(size_t index, HInstruction* instruction) {
    vregs_.Put(index, instruction);
  }

  HInstruction* GetInstructionAt(size_t index) const {
    return vregs_.Get(index);
  }

  GrowableArray<HInstruction*>* GetVRegs() {
    return &vregs_;
  }

  size_t Size() const { return vregs_.Size(); }

 private:
  GrowableArray<HInstruction*> vregs_;

  DISALLOW_COPY_AND_ASSIGN(HEnvironment);
};

class HInputIterator : public ValueObject {
 public:
  explicit HInputIterator(HInstruction* instruction) : instruction_(instruction), index_(0) {}

  bool Done() const { return index_ == instruction_->InputCount(); }
  HInstruction* Current() const { return instruction_->InputAt(index_); }
  void Advance() { index_++; }

 private:
  HInstruction* instruction_;
  size_t index_;

  DISALLOW_COPY_AND_ASSIGN(HInputIterator);
};

class HInstructionIterator : public ValueObject {
 public:
  explicit HInstructionIterator(const HInstructionList& instructions)
      : instruction_(instructions.first_instruction_) {
    next_ = Done() ? nullptr : instruction_->GetNext();
  }

  bool Done() const { return instruction_ == nullptr; }
  HInstruction* Current() const { return instruction_; }
  void Advance() {
    instruction_ = next_;
    next_ = Done() ? nullptr : instruction_->GetNext();
  }

 private:
  HInstruction* instruction_;
  HInstruction* next_;

  DISALLOW_COPY_AND_ASSIGN(HInstructionIterator);
};

class HBackwardInstructionIterator : public ValueObject {
 public:
  explicit HBackwardInstructionIterator(const HInstructionList& instructions)
      : instruction_(instructions.last_instruction_) {
    next_ = Done() ? nullptr : instruction_->GetPrevious();
  }

  bool Done() const { return instruction_ == nullptr; }
  HInstruction* Current() const { return instruction_; }
  void Advance() {
    instruction_ = next_;
    next_ = Done() ? nullptr : instruction_->GetPrevious();
  }

 private:
  HInstruction* instruction_;
  HInstruction* next_;

  DISALLOW_COPY_AND_ASSIGN(HBackwardInstructionIterator);
};

// An embedded container with N elements of type T.  Used (with partial
// specialization for N=0) because embedded arrays cannot have size 0.
template<typename T, intptr_t N>
class EmbeddedArray {
 public:
  EmbeddedArray() : elements_() {}

  intptr_t GetLength() const { return N; }

  const T& operator[](intptr_t i) const {
    DCHECK_LT(i, GetLength());
    return elements_[i];
  }

  T& operator[](intptr_t i) {
    DCHECK_LT(i, GetLength());
    return elements_[i];
  }

  const T& At(intptr_t i) const {
    return (*this)[i];
  }

  void SetAt(intptr_t i, const T& val) {
    (*this)[i] = val;
  }

 private:
  T elements_[N];
};

template<typename T>
class EmbeddedArray<T, 0> {
 public:
  intptr_t length() const { return 0; }
  const T& operator[](intptr_t i) const {
    LOG(FATAL) << "Unreachable";
    static T sentinel = 0;
    return sentinel;
  }
  T& operator[](intptr_t i) {
    LOG(FATAL) << "Unreachable";
    static T sentinel = 0;
    return sentinel;
  }
};

template<intptr_t N>
class HTemplateInstruction: public HInstruction {
 public:
  HTemplateInstruction<N>(SideEffects side_effects)
      : HInstruction(side_effects), inputs_() {}
  virtual ~HTemplateInstruction() {}

  virtual size_t InputCount() const { return N; }
  virtual HInstruction* InputAt(size_t i) const { return inputs_[i]; }

 protected:
  virtual void SetRawInputAt(size_t i, HInstruction* instruction) {
    inputs_[i] = instruction;
  }

 private:
  EmbeddedArray<HInstruction*, N> inputs_;

  friend class SsaBuilder;
};

template<intptr_t N>
class HExpression : public HTemplateInstruction<N> {
 public:
  HExpression<N>(Primitive::Type type, SideEffects side_effects)
      : HTemplateInstruction<N>(side_effects), type_(type) {}
  virtual ~HExpression() {}

  virtual Primitive::Type GetType() const { return type_; }

 protected:
  Primitive::Type type_;
};

// Represents dex's RETURN_VOID opcode. A HReturnVoid is a control flow
// instruction that branches to the exit block.
class HReturnVoid : public HTemplateInstruction<0> {
 public:
  HReturnVoid() : HTemplateInstruction(SideEffects::None()) {}

  virtual bool IsControlFlow() const { return true; }

  DECLARE_INSTRUCTION(ReturnVoid);

 private:
  DISALLOW_COPY_AND_ASSIGN(HReturnVoid);
};

// Represents dex's RETURN opcodes. A HReturn is a control flow
// instruction that branches to the exit block.
class HReturn : public HTemplateInstruction<1> {
 public:
  explicit HReturn(HInstruction* value) : HTemplateInstruction(SideEffects::None()) {
    SetRawInputAt(0, value);
  }

  virtual bool IsControlFlow() const { return true; }

  DECLARE_INSTRUCTION(Return);

 private:
  DISALLOW_COPY_AND_ASSIGN(HReturn);
};

// The exit instruction is the only instruction of the exit block.
// Instructions aborting the method (HTrow and HReturn) must branch to the
// exit block.
class HExit : public HTemplateInstruction<0> {
 public:
  HExit() : HTemplateInstruction(SideEffects::None()) {}

  virtual bool IsControlFlow() const { return true; }

  DECLARE_INSTRUCTION(Exit);

 private:
  DISALLOW_COPY_AND_ASSIGN(HExit);
};

// Jumps from one block to another.
class HGoto : public HTemplateInstruction<0> {
 public:
  HGoto() : HTemplateInstruction(SideEffects::None()) {}

  virtual bool IsControlFlow() const { return true; }

  HBasicBlock* GetSuccessor() const {
    return GetBlock()->GetSuccessors().Get(0);
  }

  DECLARE_INSTRUCTION(Goto);

 private:
  DISALLOW_COPY_AND_ASSIGN(HGoto);
};


// Conditional branch. A block ending with an HIf instruction must have
// two successors.
class HIf : public HTemplateInstruction<1> {
 public:
  explicit HIf(HInstruction* input) : HTemplateInstruction(SideEffects::None()) {
    SetRawInputAt(0, input);
  }

  virtual bool IsControlFlow() const { return true; }

  HBasicBlock* IfTrueSuccessor() const {
    return GetBlock()->GetSuccessors().Get(0);
  }

  HBasicBlock* IfFalseSuccessor() const {
    return GetBlock()->GetSuccessors().Get(1);
  }

  DECLARE_INSTRUCTION(If);

  virtual bool IsIfInstruction() const { return true; }

 private:
  DISALLOW_COPY_AND_ASSIGN(HIf);
};

class HUnaryOperation : public HExpression<1> {
 public:
  HUnaryOperation(Primitive::Type result_type, HInstruction* input)
      : HExpression(result_type, SideEffects::None()) {
    SetRawInputAt(0, input);
  }

  HInstruction* GetInput() const { return InputAt(0); }
  Primitive::Type GetResultType() const { return GetType(); }

  virtual bool CanBeMoved() const { return true; }
  virtual bool InstructionDataEquals(HInstruction* other) const { return true; }

  // Try to statically evaluate `operation` and return a HConstant
  // containing the result of this evaluation.  If `operation` cannot
  // be evaluated as a constant, return nullptr.
  HConstant* TryStaticEvaluation() const;

  // Apply this operation to `x`.
  virtual int32_t Evaluate(int32_t x) const = 0;
  virtual int64_t Evaluate(int64_t x) const = 0;

  DECLARE_INSTRUCTION(UnaryOperation);

 private:
  DISALLOW_COPY_AND_ASSIGN(HUnaryOperation);
};

class HBinaryOperation : public HExpression<2> {
 public:
  HBinaryOperation(Primitive::Type result_type,
                   HInstruction* left,
                   HInstruction* right) : HExpression(result_type, SideEffects::None()) {
    SetRawInputAt(0, left);
    SetRawInputAt(1, right);
  }

  HInstruction* GetLeft() const { return InputAt(0); }
  HInstruction* GetRight() const { return InputAt(1); }
  Primitive::Type GetResultType() const { return GetType(); }

  virtual bool IsCommutative() { return false; }

  virtual bool CanBeMoved() const { return true; }
  virtual bool InstructionDataEquals(HInstruction* other) const { return true; }

  // Try to statically evaluate `operation` and return a HConstant
  // containing the result of this evaluation.  If `operation` cannot
  // be evaluated as a constant, return nullptr.
  HConstant* TryStaticEvaluation() const;

  // Apply this operation to `x` and `y`.
  virtual int32_t Evaluate(int32_t x, int32_t y) const = 0;
  virtual int64_t Evaluate(int64_t x, int64_t y) const = 0;

  DECLARE_INSTRUCTION(BinaryOperation);

 private:
  DISALLOW_COPY_AND_ASSIGN(HBinaryOperation);
};

class HCondition : public HBinaryOperation {
 public:
  HCondition(HInstruction* first, HInstruction* second)
      : HBinaryOperation(Primitive::kPrimBoolean, first, second),
        needs_materialization_(true) {}

  virtual bool IsCommutative() { return true; }

  bool NeedsMaterialization() const { return needs_materialization_; }
  void ClearNeedsMaterialization() { needs_materialization_ = false; }

  // For code generation purposes, returns whether this instruction is just before
  // `if_`, and disregard moves in between.
  bool IsBeforeWhenDisregardMoves(HIf* if_) const;

  DECLARE_INSTRUCTION(Condition);

  virtual IfCondition GetCondition() const = 0;

 private:
  // For register allocation purposes, returns whether this instruction needs to be
  // materialized (that is, not just be in the processor flags).
  bool needs_materialization_;

  DISALLOW_COPY_AND_ASSIGN(HCondition);
};

// Instruction to check if two inputs are equal to each other.
class HEqual : public HCondition {
 public:
  HEqual(HInstruction* first, HInstruction* second)
      : HCondition(first, second) {}

  virtual int32_t Evaluate(int32_t x, int32_t y) const OVERRIDE {
    return x == y ? 1 : 0;
  }
  virtual int64_t Evaluate(int64_t x, int64_t y) const OVERRIDE {
    return x == y ? 1 : 0;
  }

  DECLARE_INSTRUCTION(Equal);

  virtual IfCondition GetCondition() const {
    return kCondEQ;
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(HEqual);
};

class HNotEqual : public HCondition {
 public:
  HNotEqual(HInstruction* first, HInstruction* second)
      : HCondition(first, second) {}

  virtual int32_t Evaluate(int32_t x, int32_t y) const OVERRIDE {
    return x != y ? 1 : 0;
  }
  virtual int64_t Evaluate(int64_t x, int64_t y) const OVERRIDE {
    return x != y ? 1 : 0;
  }

  DECLARE_INSTRUCTION(NotEqual);

  virtual IfCondition GetCondition() const {
    return kCondNE;
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(HNotEqual);
};

class HLessThan : public HCondition {
 public:
  HLessThan(HInstruction* first, HInstruction* second)
      : HCondition(first, second) {}

  virtual int32_t Evaluate(int32_t x, int32_t y) const OVERRIDE {
    return x < y ? 1 : 0;
  }
  virtual int64_t Evaluate(int64_t x, int64_t y) const OVERRIDE {
    return x < y ? 1 : 0;
  }

  DECLARE_INSTRUCTION(LessThan);

  virtual IfCondition GetCondition() const {
    return kCondLT;
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(HLessThan);
};

class HLessThanOrEqual : public HCondition {
 public:
  HLessThanOrEqual(HInstruction* first, HInstruction* second)
      : HCondition(first, second) {}

  virtual int32_t Evaluate(int32_t x, int32_t y) const OVERRIDE {
    return x <= y ? 1 : 0;
  }
  virtual int64_t Evaluate(int64_t x, int64_t y) const OVERRIDE {
    return x <= y ? 1 : 0;
  }

  DECLARE_INSTRUCTION(LessThanOrEqual);

  virtual IfCondition GetCondition() const {
    return kCondLE;
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(HLessThanOrEqual);
};

class HGreaterThan : public HCondition {
 public:
  HGreaterThan(HInstruction* first, HInstruction* second)
      : HCondition(first, second) {}

  virtual int32_t Evaluate(int32_t x, int32_t y) const OVERRIDE {
    return x > y ? 1 : 0;
  }
  virtual int64_t Evaluate(int64_t x, int64_t y) const OVERRIDE {
    return x > y ? 1 : 0;
  }

  DECLARE_INSTRUCTION(GreaterThan);

  virtual IfCondition GetCondition() const {
    return kCondGT;
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(HGreaterThan);
};

class HGreaterThanOrEqual : public HCondition {
 public:
  HGreaterThanOrEqual(HInstruction* first, HInstruction* second)
      : HCondition(first, second) {}

  virtual int32_t Evaluate(int32_t x, int32_t y) const OVERRIDE {
    return x >= y ? 1 : 0;
  }
  virtual int64_t Evaluate(int64_t x, int64_t y) const OVERRIDE {
    return x >= y ? 1 : 0;
  }

  DECLARE_INSTRUCTION(GreaterThanOrEqual);

  virtual IfCondition GetCondition() const {
    return kCondGE;
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(HGreaterThanOrEqual);
};


// Instruction to check how two inputs compare to each other.
// Result is 0 if input0 == input1, 1 if input0 > input1, or -1 if input0 < input1.
class HCompare : public HBinaryOperation {
 public:
  HCompare(Primitive::Type type, HInstruction* first, HInstruction* second)
      : HBinaryOperation(Primitive::kPrimInt, first, second) {
    DCHECK_EQ(type, first->GetType());
    DCHECK_EQ(type, second->GetType());
  }

  virtual int32_t Evaluate(int32_t x, int32_t y) const OVERRIDE {
    return
      x == y ? 0 :
      x > y ? 1 :
      -1;
  }
  virtual int64_t Evaluate(int64_t x, int64_t y) const OVERRIDE {
    return
      x == y ? 0 :
      x > y ? 1 :
      -1;
  }

  DECLARE_INSTRUCTION(Compare);

 private:
  DISALLOW_COPY_AND_ASSIGN(HCompare);
};

// A local in the graph. Corresponds to a Dex register.
class HLocal : public HTemplateInstruction<0> {
 public:
  explicit HLocal(uint16_t reg_number)
      : HTemplateInstruction(SideEffects::None()), reg_number_(reg_number) {}

  DECLARE_INSTRUCTION(Local);

  uint16_t GetRegNumber() const { return reg_number_; }

 private:
  // The Dex register number.
  const uint16_t reg_number_;

  DISALLOW_COPY_AND_ASSIGN(HLocal);
};

// Load a given local. The local is an input of this instruction.
class HLoadLocal : public HExpression<1> {
 public:
  HLoadLocal(HLocal* local, Primitive::Type type)
      : HExpression(type, SideEffects::None()) {
    SetRawInputAt(0, local);
  }

  HLocal* GetLocal() const { return reinterpret_cast<HLocal*>(InputAt(0)); }

  DECLARE_INSTRUCTION(LoadLocal);

 private:
  DISALLOW_COPY_AND_ASSIGN(HLoadLocal);
};

// Store a value in a given local. This instruction has two inputs: the value
// and the local.
class HStoreLocal : public HTemplateInstruction<2> {
 public:
  HStoreLocal(HLocal* local, HInstruction* value) : HTemplateInstruction(SideEffects::None()) {
    SetRawInputAt(0, local);
    SetRawInputAt(1, value);
  }

  HLocal* GetLocal() const { return reinterpret_cast<HLocal*>(InputAt(0)); }

  DECLARE_INSTRUCTION(StoreLocal);

 private:
  DISALLOW_COPY_AND_ASSIGN(HStoreLocal);
};

class HConstant : public HExpression<0> {
 public:
  explicit HConstant(Primitive::Type type) : HExpression(type, SideEffects::None()) {}

  virtual bool CanBeMoved() const { return true; }

  DECLARE_INSTRUCTION(Constant);

 private:
  DISALLOW_COPY_AND_ASSIGN(HConstant);
};

class HFloatConstant : public HConstant {
 public:
  explicit HFloatConstant(float value) : HConstant(Primitive::kPrimFloat), value_(value) {}

  float GetValue() const { return value_; }

  virtual bool InstructionDataEquals(HInstruction* other) const {
    return bit_cast<float, int32_t>(other->AsFloatConstant()->value_) ==
        bit_cast<float, int32_t>(value_);
  }

  virtual size_t ComputeHashCode() const { return static_cast<size_t>(GetValue()); }

  DECLARE_INSTRUCTION(FloatConstant);

 private:
  const float value_;

  DISALLOW_COPY_AND_ASSIGN(HFloatConstant);
};

class HDoubleConstant : public HConstant {
 public:
  explicit HDoubleConstant(double value) : HConstant(Primitive::kPrimDouble), value_(value) {}

  double GetValue() const { return value_; }

  virtual bool InstructionDataEquals(HInstruction* other) const {
    return bit_cast<double, int64_t>(other->AsDoubleConstant()->value_) ==
        bit_cast<double, int64_t>(value_);
  }

  virtual size_t ComputeHashCode() const { return static_cast<size_t>(GetValue()); }

  DECLARE_INSTRUCTION(DoubleConstant);

 private:
  const double value_;

  DISALLOW_COPY_AND_ASSIGN(HDoubleConstant);
};

// Constants of the type int. Those can be from Dex instructions, or
// synthesized (for example with the if-eqz instruction).
class HIntConstant : public HConstant {
 public:
  explicit HIntConstant(int32_t value) : HConstant(Primitive::kPrimInt), value_(value) {}

  int32_t GetValue() const { return value_; }

  virtual bool InstructionDataEquals(HInstruction* other) const {
    return other->AsIntConstant()->value_ == value_;
  }

  virtual size_t ComputeHashCode() const { return GetValue(); }

  DECLARE_INSTRUCTION(IntConstant);

 private:
  const int32_t value_;

  DISALLOW_COPY_AND_ASSIGN(HIntConstant);
};

class HLongConstant : public HConstant {
 public:
  explicit HLongConstant(int64_t value) : HConstant(Primitive::kPrimLong), value_(value) {}

  int64_t GetValue() const { return value_; }

  virtual bool InstructionDataEquals(HInstruction* other) const {
    return other->AsLongConstant()->value_ == value_;
  }

  virtual size_t ComputeHashCode() const { return static_cast<size_t>(GetValue()); }

  DECLARE_INSTRUCTION(LongConstant);

 private:
  const int64_t value_;

  DISALLOW_COPY_AND_ASSIGN(HLongConstant);
};

class HInvoke : public HInstruction {
 public:
  HInvoke(ArenaAllocator* arena,
          uint32_t number_of_arguments,
          Primitive::Type return_type,
          uint32_t dex_pc)
    : HInstruction(SideEffects::All()),
      inputs_(arena, number_of_arguments),
      return_type_(return_type),
      dex_pc_(dex_pc) {
    inputs_.SetSize(number_of_arguments);
  }

  virtual size_t InputCount() const { return inputs_.Size(); }
  virtual HInstruction* InputAt(size_t i) const { return inputs_.Get(i); }

  // Runtime needs to walk the stack, so Dex -> Dex calls need to
  // know their environment.
  virtual bool NeedsEnvironment() const { return true; }

  void SetArgumentAt(size_t index, HInstruction* argument) {
    SetRawInputAt(index, argument);
  }

  virtual void SetRawInputAt(size_t index, HInstruction* input) {
    inputs_.Put(index, input);
  }

  virtual Primitive::Type GetType() const { return return_type_; }

  uint32_t GetDexPc() const { return dex_pc_; }

  DECLARE_INSTRUCTION(Invoke);

 protected:
  GrowableArray<HInstruction*> inputs_;
  const Primitive::Type return_type_;
  const uint32_t dex_pc_;

 private:
  DISALLOW_COPY_AND_ASSIGN(HInvoke);
};

class HInvokeStatic : public HInvoke {
 public:
  HInvokeStatic(ArenaAllocator* arena,
                uint32_t number_of_arguments,
                Primitive::Type return_type,
                uint32_t dex_pc,
                uint32_t index_in_dex_cache)
      : HInvoke(arena, number_of_arguments, return_type, dex_pc),
        index_in_dex_cache_(index_in_dex_cache) {}

  uint32_t GetIndexInDexCache() const { return index_in_dex_cache_; }

  DECLARE_INSTRUCTION(InvokeStatic);

 private:
  const uint32_t index_in_dex_cache_;

  DISALLOW_COPY_AND_ASSIGN(HInvokeStatic);
};

class HInvokeVirtual : public HInvoke {
 public:
  HInvokeVirtual(ArenaAllocator* arena,
                 uint32_t number_of_arguments,
                 Primitive::Type return_type,
                 uint32_t dex_pc,
                 uint32_t vtable_index)
      : HInvoke(arena, number_of_arguments, return_type, dex_pc),
        vtable_index_(vtable_index) {}

  uint32_t GetVTableIndex() const { return vtable_index_; }

  DECLARE_INSTRUCTION(InvokeVirtual);

 private:
  const uint32_t vtable_index_;

  DISALLOW_COPY_AND_ASSIGN(HInvokeVirtual);
};

class HNewInstance : public HExpression<0> {
 public:
  HNewInstance(uint32_t dex_pc, uint16_t type_index)
      : HExpression(Primitive::kPrimNot, SideEffects::None()),
        dex_pc_(dex_pc),
        type_index_(type_index) {}

  uint32_t GetDexPc() const { return dex_pc_; }
  uint16_t GetTypeIndex() const { return type_index_; }

  // Calls runtime so needs an environment.
  virtual bool NeedsEnvironment() const { return true; }

  DECLARE_INSTRUCTION(NewInstance);

 private:
  const uint32_t dex_pc_;
  const uint16_t type_index_;

  DISALLOW_COPY_AND_ASSIGN(HNewInstance);
};

class HNeg : public HUnaryOperation {
 public:
  explicit HNeg(Primitive::Type result_type, HInstruction* input)
      : HUnaryOperation(result_type, input) {}

  virtual int32_t Evaluate(int32_t x) const OVERRIDE { return -x; }
  virtual int64_t Evaluate(int64_t x) const OVERRIDE { return -x; }

  DECLARE_INSTRUCTION(Neg);

 private:
  DISALLOW_COPY_AND_ASSIGN(HNeg);
};

class HNewArray : public HExpression<1> {
 public:
  HNewArray(HInstruction* length, uint32_t dex_pc, uint16_t type_index)
      : HExpression(Primitive::kPrimNot, SideEffects::None()),
        dex_pc_(dex_pc),
        type_index_(type_index) {
    SetRawInputAt(0, length);
  }

  uint32_t GetDexPc() const { return dex_pc_; }
  uint16_t GetTypeIndex() const { return type_index_; }

  // Calls runtime so needs an environment.
  virtual bool NeedsEnvironment() const { return true; }

  DECLARE_INSTRUCTION(NewArray);

 private:
  const uint32_t dex_pc_;
  const uint16_t type_index_;

  DISALLOW_COPY_AND_ASSIGN(HNewArray);
};

class HAdd : public HBinaryOperation {
 public:
  HAdd(Primitive::Type result_type, HInstruction* left, HInstruction* right)
      : HBinaryOperation(result_type, left, right) {}

  virtual bool IsCommutative() { return true; }

  virtual int32_t Evaluate(int32_t x, int32_t y) const OVERRIDE {
    return x + y;
  }
  virtual int64_t Evaluate(int64_t x, int64_t y) const OVERRIDE {
    return x + y;
  }

  DECLARE_INSTRUCTION(Add);

 private:
  DISALLOW_COPY_AND_ASSIGN(HAdd);
};

class HSub : public HBinaryOperation {
 public:
  HSub(Primitive::Type result_type, HInstruction* left, HInstruction* right)
      : HBinaryOperation(result_type, left, right) {}

  virtual bool IsCommutative() { return false; }

  virtual int32_t Evaluate(int32_t x, int32_t y) const OVERRIDE {
    return x - y;
  }
  virtual int64_t Evaluate(int64_t x, int64_t y) const OVERRIDE {
    return x - y;
  }

  DECLARE_INSTRUCTION(Sub);

 private:
  DISALLOW_COPY_AND_ASSIGN(HSub);
};

class HMul : public HBinaryOperation {
 public:
  HMul(Primitive::Type result_type, HInstruction* left, HInstruction* right)
      : HBinaryOperation(result_type, left, right) {}

  virtual bool IsCommutative() { return true; }

  virtual int32_t Evaluate(int32_t x, int32_t y) const { return x * y; }
  virtual int64_t Evaluate(int64_t x, int64_t y) const { return x * y; }

  DECLARE_INSTRUCTION(Mul);

 private:
  DISALLOW_COPY_AND_ASSIGN(HMul);
};

// The value of a parameter in this method. Its location depends on
// the calling convention.
class HParameterValue : public HExpression<0> {
 public:
  HParameterValue(uint8_t index, Primitive::Type parameter_type)
      : HExpression(parameter_type, SideEffects::None()), index_(index) {}

  uint8_t GetIndex() const { return index_; }

  DECLARE_INSTRUCTION(ParameterValue);

 private:
  // The index of this parameter in the parameters list. Must be less
  // than HGraph::number_of_in_vregs_;
  const uint8_t index_;

  DISALLOW_COPY_AND_ASSIGN(HParameterValue);
};

class HNot : public HUnaryOperation {
 public:
  explicit HNot(Primitive::Type result_type, HInstruction* input)
      : HUnaryOperation(result_type, input) {}

  virtual bool CanBeMoved() const { return true; }
  virtual bool InstructionDataEquals(HInstruction* other) const { return true; }

  virtual int32_t Evaluate(int32_t x) const OVERRIDE { return ~x; }
  virtual int64_t Evaluate(int64_t x) const OVERRIDE { return ~x; }

  DECLARE_INSTRUCTION(Not);

 private:
  DISALLOW_COPY_AND_ASSIGN(HNot);
};

class HPhi : public HInstruction {
 public:
  HPhi(ArenaAllocator* arena, uint32_t reg_number, size_t number_of_inputs, Primitive::Type type)
      : HInstruction(SideEffects::None()),
        inputs_(arena, number_of_inputs),
        reg_number_(reg_number),
        type_(type),
        is_live_(false) {
    inputs_.SetSize(number_of_inputs);
  }

  virtual size_t InputCount() const { return inputs_.Size(); }
  virtual HInstruction* InputAt(size_t i) const { return inputs_.Get(i); }

  virtual void SetRawInputAt(size_t index, HInstruction* input) {
    inputs_.Put(index, input);
  }

  void AddInput(HInstruction* input);

  virtual Primitive::Type GetType() const { return type_; }
  void SetType(Primitive::Type type) { type_ = type; }

  uint32_t GetRegNumber() const { return reg_number_; }

  void SetDead() { is_live_ = false; }
  void SetLive() { is_live_ = true; }
  bool IsDead() const { return !is_live_; }
  bool IsLive() const { return is_live_; }

  DECLARE_INSTRUCTION(Phi);

 private:
  GrowableArray<HInstruction*> inputs_;
  const uint32_t reg_number_;
  Primitive::Type type_;
  bool is_live_;

  DISALLOW_COPY_AND_ASSIGN(HPhi);
};

class HNullCheck : public HExpression<1> {
 public:
  HNullCheck(HInstruction* value, uint32_t dex_pc)
      : HExpression(value->GetType(), SideEffects::None()), dex_pc_(dex_pc) {
    SetRawInputAt(0, value);
  }

  virtual bool CanBeMoved() const { return true; }
  virtual bool InstructionDataEquals(HInstruction* other) const { return true; }

  virtual bool NeedsEnvironment() const { return true; }

  virtual bool CanThrow() const { return true; }

  uint32_t GetDexPc() const { return dex_pc_; }

  DECLARE_INSTRUCTION(NullCheck);

 private:
  const uint32_t dex_pc_;

  DISALLOW_COPY_AND_ASSIGN(HNullCheck);
};

class FieldInfo : public ValueObject {
 public:
  FieldInfo(MemberOffset field_offset, Primitive::Type field_type)
      : field_offset_(field_offset), field_type_(field_type) {}

  MemberOffset GetFieldOffset() const { return field_offset_; }
  Primitive::Type GetFieldType() const { return field_type_; }

 private:
  const MemberOffset field_offset_;
  const Primitive::Type field_type_;
};

class HInstanceFieldGet : public HExpression<1> {
 public:
  HInstanceFieldGet(HInstruction* value,
                    Primitive::Type field_type,
                    MemberOffset field_offset)
      : HExpression(field_type, SideEffects::DependsOnSomething()),
        field_info_(field_offset, field_type) {
    SetRawInputAt(0, value);
  }

  virtual bool CanBeMoved() const { return true; }
  virtual bool InstructionDataEquals(HInstruction* other) const {
    size_t other_offset = other->AsInstanceFieldGet()->GetFieldOffset().SizeValue();
    return other_offset == GetFieldOffset().SizeValue();
  }

  virtual size_t ComputeHashCode() const {
    return (HInstruction::ComputeHashCode() << 7) | GetFieldOffset().SizeValue();
  }

  MemberOffset GetFieldOffset() const { return field_info_.GetFieldOffset(); }
  Primitive::Type GetFieldType() const { return field_info_.GetFieldType(); }

  DECLARE_INSTRUCTION(InstanceFieldGet);

 private:
  const FieldInfo field_info_;

  DISALLOW_COPY_AND_ASSIGN(HInstanceFieldGet);
};

class HInstanceFieldSet : public HTemplateInstruction<2> {
 public:
  HInstanceFieldSet(HInstruction* object,
                    HInstruction* value,
                    Primitive::Type field_type,
                    MemberOffset field_offset)
      : HTemplateInstruction(SideEffects::ChangesSomething()),
        field_info_(field_offset, field_type) {
    SetRawInputAt(0, object);
    SetRawInputAt(1, value);
  }

  MemberOffset GetFieldOffset() const { return field_info_.GetFieldOffset(); }
  Primitive::Type GetFieldType() const { return field_info_.GetFieldType(); }

  DECLARE_INSTRUCTION(InstanceFieldSet);

 private:
  const FieldInfo field_info_;

  DISALLOW_COPY_AND_ASSIGN(HInstanceFieldSet);
};

class HArrayGet : public HExpression<2> {
 public:
  HArrayGet(HInstruction* array, HInstruction* index, Primitive::Type type)
      : HExpression(type, SideEffects::DependsOnSomething()) {
    SetRawInputAt(0, array);
    SetRawInputAt(1, index);
  }

  virtual bool CanBeMoved() const { return true; }
  virtual bool InstructionDataEquals(HInstruction* other) const { return true; }
  void SetType(Primitive::Type type) { type_ = type; }

  DECLARE_INSTRUCTION(ArrayGet);

 private:
  DISALLOW_COPY_AND_ASSIGN(HArrayGet);
};

class HArraySet : public HTemplateInstruction<3> {
 public:
  HArraySet(HInstruction* array,
            HInstruction* index,
            HInstruction* value,
            Primitive::Type expected_component_type,
            uint32_t dex_pc)
      : HTemplateInstruction(SideEffects::ChangesSomething()),
        dex_pc_(dex_pc),
        expected_component_type_(expected_component_type) {
    SetRawInputAt(0, array);
    SetRawInputAt(1, index);
    SetRawInputAt(2, value);
  }

  virtual bool NeedsEnvironment() const {
    // We currently always call a runtime method to catch array store
    // exceptions.
    return InputAt(2)->GetType() == Primitive::kPrimNot;
  }

  uint32_t GetDexPc() const { return dex_pc_; }

  HInstruction* GetValue() const { return InputAt(2); }

  Primitive::Type GetComponentType() const {
    // The Dex format does not type floating point index operations. Since the
    // `expected_component_type_` is set during building and can therefore not
    // be correct, we also check what is the value type. If it is a floating
    // point type, we must use that type.
    Primitive::Type value_type = GetValue()->GetType();
    return ((value_type == Primitive::kPrimFloat) || (value_type == Primitive::kPrimDouble))
        ? value_type
        : expected_component_type_;
  }

  DECLARE_INSTRUCTION(ArraySet);

 private:
  const uint32_t dex_pc_;
  const Primitive::Type expected_component_type_;

  DISALLOW_COPY_AND_ASSIGN(HArraySet);
};

class HArrayLength : public HExpression<1> {
 public:
  explicit HArrayLength(HInstruction* array)
      : HExpression(Primitive::kPrimInt, SideEffects::None()) {
    // Note that arrays do not change length, so the instruction does not
    // depend on any write.
    SetRawInputAt(0, array);
  }

  virtual bool CanBeMoved() const { return true; }
  virtual bool InstructionDataEquals(HInstruction* other) const { return true; }

  DECLARE_INSTRUCTION(ArrayLength);

 private:
  DISALLOW_COPY_AND_ASSIGN(HArrayLength);
};

class HBoundsCheck : public HExpression<2> {
 public:
  HBoundsCheck(HInstruction* index, HInstruction* length, uint32_t dex_pc)
      : HExpression(index->GetType(), SideEffects::None()), dex_pc_(dex_pc) {
    DCHECK(index->GetType() == Primitive::kPrimInt);
    SetRawInputAt(0, index);
    SetRawInputAt(1, length);
  }

  virtual bool CanBeMoved() const { return true; }
  virtual bool InstructionDataEquals(HInstruction* other) const { return true; }

  virtual bool NeedsEnvironment() const { return true; }

  virtual bool CanThrow() const { return true; }

  uint32_t GetDexPc() const { return dex_pc_; }

  DECLARE_INSTRUCTION(BoundsCheck);

 private:
  const uint32_t dex_pc_;

  DISALLOW_COPY_AND_ASSIGN(HBoundsCheck);
};

/**
 * Some DEX instructions are folded into multiple HInstructions that need
 * to stay live until the last HInstruction. This class
 * is used as a marker for the baseline compiler to ensure its preceding
 * HInstruction stays live. `index` is the temporary number that is used
 * for knowing the stack offset where to store the instruction.
 */
class HTemporary : public HTemplateInstruction<0> {
 public:
  explicit HTemporary(size_t index) : HTemplateInstruction(SideEffects::None()), index_(index) {}

  size_t GetIndex() const { return index_; }

  DECLARE_INSTRUCTION(Temporary);

 private:
  const size_t index_;

  DISALLOW_COPY_AND_ASSIGN(HTemporary);
};

class HSuspendCheck : public HTemplateInstruction<0> {
 public:
  explicit HSuspendCheck(uint32_t dex_pc)
      : HTemplateInstruction(SideEffects::None()), dex_pc_(dex_pc) {}

  virtual bool NeedsEnvironment() const {
    return true;
  }

  uint32_t GetDexPc() const { return dex_pc_; }

  DECLARE_INSTRUCTION(SuspendCheck);

 private:
  const uint32_t dex_pc_;

  DISALLOW_COPY_AND_ASSIGN(HSuspendCheck);
};

class MoveOperands : public ArenaObject {
 public:
  MoveOperands(Location source, Location destination, HInstruction* instruction)
      : source_(source), destination_(destination), instruction_(instruction) {}

  Location GetSource() const { return source_; }
  Location GetDestination() const { return destination_; }

  void SetSource(Location value) { source_ = value; }
  void SetDestination(Location value) { destination_ = value; }

  // The parallel move resolver marks moves as "in-progress" by clearing the
  // destination (but not the source).
  Location MarkPending() {
    DCHECK(!IsPending());
    Location dest = destination_;
    destination_ = Location::NoLocation();
    return dest;
  }

  void ClearPending(Location dest) {
    DCHECK(IsPending());
    destination_ = dest;
  }

  bool IsPending() const {
    DCHECK(!source_.IsInvalid() || destination_.IsInvalid());
    return destination_.IsInvalid() && !source_.IsInvalid();
  }

  // True if this blocks a move from the given location.
  bool Blocks(Location loc) const {
    return !IsEliminated() && source_.Equals(loc);
  }

  // A move is redundant if it's been eliminated, if its source and
  // destination are the same, or if its destination is unneeded.
  bool IsRedundant() const {
    return IsEliminated() || destination_.IsInvalid() || source_.Equals(destination_);
  }

  // We clear both operands to indicate move that's been eliminated.
  void Eliminate() {
    source_ = destination_ = Location::NoLocation();
  }

  bool IsEliminated() const {
    DCHECK(!source_.IsInvalid() || destination_.IsInvalid());
    return source_.IsInvalid();
  }

  HInstruction* GetInstruction() const { return instruction_; }

 private:
  Location source_;
  Location destination_;
  // The instruction this move is assocatied with. Null when this move is
  // for moving an input in the expected locations of user (including a phi user).
  // This is only used in debug mode, to ensure we do not connect interval siblings
  // in the same parallel move.
  HInstruction* instruction_;

  DISALLOW_COPY_AND_ASSIGN(MoveOperands);
};

static constexpr size_t kDefaultNumberOfMoves = 4;

class HParallelMove : public HTemplateInstruction<0> {
 public:
  explicit HParallelMove(ArenaAllocator* arena)
      : HTemplateInstruction(SideEffects::None()), moves_(arena, kDefaultNumberOfMoves) {}

  void AddMove(MoveOperands* move) {
    if (kIsDebugBuild && move->GetInstruction() != nullptr) {
      for (size_t i = 0, e = moves_.Size(); i < e; ++i) {
        DCHECK_NE(moves_.Get(i)->GetInstruction(), move->GetInstruction())
          << "Doing parallel moves for the same instruction.";
      }
    }
    moves_.Add(move);
  }

  MoveOperands* MoveOperandsAt(size_t index) const {
    return moves_.Get(index);
  }

  size_t NumMoves() const { return moves_.Size(); }

  DECLARE_INSTRUCTION(ParallelMove);

 private:
  GrowableArray<MoveOperands*> moves_;

  DISALLOW_COPY_AND_ASSIGN(HParallelMove);
};

class HGraphVisitor : public ValueObject {
 public:
  explicit HGraphVisitor(HGraph* graph) : graph_(graph) {}
  virtual ~HGraphVisitor() {}

  virtual void VisitInstruction(HInstruction* instruction) {}
  virtual void VisitBasicBlock(HBasicBlock* block);

  // Visit the graph following basic block insertion order.
  void VisitInsertionOrder();

  // Visit the graph following dominator tree reverse post-order.
  void VisitReversePostOrder();

  HGraph* GetGraph() const { return graph_; }

  // Visit functions for instruction classes.
#define DECLARE_VISIT_INSTRUCTION(name, super)                                        \
  virtual void Visit##name(H##name* instr) { VisitInstruction(instr); }

  FOR_EACH_INSTRUCTION(DECLARE_VISIT_INSTRUCTION)

#undef DECLARE_VISIT_INSTRUCTION

 private:
  HGraph* const graph_;

  DISALLOW_COPY_AND_ASSIGN(HGraphVisitor);
};

class HGraphDelegateVisitor : public HGraphVisitor {
 public:
  explicit HGraphDelegateVisitor(HGraph* graph) : HGraphVisitor(graph) {}
  virtual ~HGraphDelegateVisitor() {}

  // Visit functions that delegate to to super class.
#define DECLARE_VISIT_INSTRUCTION(name, super)                                        \
  virtual void Visit##name(H##name* instr) OVERRIDE { Visit##super(instr); }

  FOR_EACH_INSTRUCTION(DECLARE_VISIT_INSTRUCTION)

#undef DECLARE_VISIT_INSTRUCTION

 private:
  DISALLOW_COPY_AND_ASSIGN(HGraphDelegateVisitor);
};

class HInsertionOrderIterator : public ValueObject {
 public:
  explicit HInsertionOrderIterator(const HGraph& graph) : graph_(graph), index_(0) {}

  bool Done() const { return index_ == graph_.GetBlocks().Size(); }
  HBasicBlock* Current() const { return graph_.GetBlocks().Get(index_); }
  void Advance() { ++index_; }

 private:
  const HGraph& graph_;
  size_t index_;

  DISALLOW_COPY_AND_ASSIGN(HInsertionOrderIterator);
};

class HReversePostOrderIterator : public ValueObject {
 public:
  explicit HReversePostOrderIterator(const HGraph& graph) : graph_(graph), index_(0) {}

  bool Done() const { return index_ == graph_.GetReversePostOrder().Size(); }
  HBasicBlock* Current() const { return graph_.GetReversePostOrder().Get(index_); }
  void Advance() { ++index_; }

 private:
  const HGraph& graph_;
  size_t index_;

  DISALLOW_COPY_AND_ASSIGN(HReversePostOrderIterator);
};

class HPostOrderIterator : public ValueObject {
 public:
  explicit HPostOrderIterator(const HGraph& graph)
      : graph_(graph), index_(graph_.GetReversePostOrder().Size()) {}

  bool Done() const { return index_ == 0; }
  HBasicBlock* Current() const { return graph_.GetReversePostOrder().Get(index_ - 1); }
  void Advance() { --index_; }

 private:
  const HGraph& graph_;
  size_t index_;

  DISALLOW_COPY_AND_ASSIGN(HPostOrderIterator);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_NODES_H_
