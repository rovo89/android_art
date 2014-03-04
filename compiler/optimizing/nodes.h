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

#include "utils/allocation.h"
#include "utils/arena_bit_vector.h"
#include "utils/growable_array.h"

namespace art {

class HBasicBlock;
class HInstruction;
class HIntConstant;
class HGraphVisitor;

static const int kDefaultNumberOfBlocks = 8;
static const int kDefaultNumberOfSuccessors = 2;
static const int kDefaultNumberOfPredecessors = 2;
static const int kDefaultNumberOfBackEdges = 1;

// Control-flow graph of a method. Contains a list of basic blocks.
class HGraph : public ArenaObject {
 public:
  explicit HGraph(ArenaAllocator* arena)
      : arena_(arena),
        blocks_(arena, kDefaultNumberOfBlocks),
        dominator_order_(arena, kDefaultNumberOfBlocks),
        current_instruction_id_(0) { }

  ArenaAllocator* arena() const { return arena_; }
  const GrowableArray<HBasicBlock*>* blocks() const { return &blocks_; }

  HBasicBlock* entry_block() const { return entry_block_; }
  HBasicBlock* exit_block() const { return exit_block_; }

  void set_entry_block(HBasicBlock* block) { entry_block_ = block; }
  void set_exit_block(HBasicBlock* block) { exit_block_ = block; }

  void AddBlock(HBasicBlock* block);
  void BuildDominatorTree();

  int GetNextInstructionId() {
    return current_instruction_id_++;
  }

 private:
  HBasicBlock* FindCommonDominator(HBasicBlock* first, HBasicBlock* second) const;
  void VisitBlockForDominatorTree(HBasicBlock* block,
                                  HBasicBlock* predecessor,
                                  GrowableArray<size_t>* visits);
  void FindBackEdges(ArenaBitVector* visited) const;
  void VisitBlockForBackEdges(HBasicBlock* block,
                              ArenaBitVector* visited,
                              ArenaBitVector* visiting) const;
  void RemoveDeadBlocks(const ArenaBitVector& visited) const;

  ArenaAllocator* const arena_;

  // List of blocks in insertion order.
  GrowableArray<HBasicBlock*> blocks_;

  // List of blocks to perform a pre-order dominator tree traversal.
  GrowableArray<HBasicBlock*> dominator_order_;

  HBasicBlock* entry_block_;
  HBasicBlock* exit_block_;

  // The current id to assign to a newly added instruction. See HInstruction.id_.
  int current_instruction_id_;

  DISALLOW_COPY_AND_ASSIGN(HGraph);
};

class HLoopInformation : public ArenaObject {
 public:
  HLoopInformation(HBasicBlock* header, HGraph* graph)
      : header_(header),
        back_edges_(graph->arena(), kDefaultNumberOfBackEdges) { }

  void AddBackEdge(HBasicBlock* back_edge) {
    back_edges_.Add(back_edge);
  }

  int NumberOfBackEdges() const {
    return back_edges_.Size();
  }

 private:
  HBasicBlock* header_;
  GrowableArray<HBasicBlock*> back_edges_;

  DISALLOW_COPY_AND_ASSIGN(HLoopInformation);
};

// A block in a method. Contains the list of instructions represented
// as a double linked list. Each block knows its predecessors and
// successors.
class HBasicBlock : public ArenaObject {
 public:
  explicit HBasicBlock(HGraph* graph)
      : graph_(graph),
        predecessors_(graph->arena(), kDefaultNumberOfPredecessors),
        successors_(graph->arena(), kDefaultNumberOfSuccessors),
        first_instruction_(nullptr),
        last_instruction_(nullptr),
        loop_information_(nullptr),
        dominator_(nullptr),
        block_id_(-1) { }

  const GrowableArray<HBasicBlock*>* predecessors() const {
    return &predecessors_;
  }

  const GrowableArray<HBasicBlock*>* successors() const {
    return &successors_;
  }

  void AddBackEdge(HBasicBlock* back_edge) {
    if (loop_information_ == nullptr) {
      loop_information_ = new (graph_->arena()) HLoopInformation(this, graph_);
    }
    loop_information_->AddBackEdge(back_edge);
  }

  HGraph* graph() const { return graph_; }

  int block_id() const { return block_id_; }
  void set_block_id(int id) { block_id_ = id; }

  HBasicBlock* dominator() const { return dominator_; }
  void set_dominator(HBasicBlock* dominator) { dominator_ = dominator; }

  int NumberOfBackEdges() const {
    return loop_information_ == nullptr
        ? 0
        : loop_information_->NumberOfBackEdges();
  }

  HInstruction* first_instruction() const { return first_instruction_; }
  HInstruction* last_instruction() const { return last_instruction_; }

  void AddSuccessor(HBasicBlock* block) {
    successors_.Add(block);
    block->predecessors_.Add(this);
  }

  void RemovePredecessor(HBasicBlock* block) {
    predecessors_.Delete(block);
  }

  void AddInstruction(HInstruction* instruction);

 private:
  HGraph* const graph_;
  GrowableArray<HBasicBlock*> predecessors_;
  GrowableArray<HBasicBlock*> successors_;
  HInstruction* first_instruction_;
  HInstruction* last_instruction_;
  HLoopInformation* loop_information_;
  HBasicBlock* dominator_;
  int block_id_;

  DISALLOW_COPY_AND_ASSIGN(HBasicBlock);
};

#define FOR_EACH_INSTRUCTION(M)                            \
  M(Equal)                                                 \
  M(Exit)                                                  \
  M(Goto)                                                  \
  M(If)                                                    \
  M(IntConstant)                                           \
  M(LoadLocal)                                             \
  M(Local)                                                 \
  M(ReturnVoid)                                            \
  M(StoreLocal)                                            \

#define DECLARE_INSTRUCTION(type)                          \
  virtual void Accept(HGraphVisitor* visitor);             \
  virtual const char* DebugName() const { return #type; }  \

class HUseListNode : public ArenaObject {
 public:
  HUseListNode(HInstruction* instruction, HUseListNode* tail)
      : instruction_(instruction), tail_(tail) { }

  HUseListNode* tail() const { return tail_; }
  HInstruction* instruction() const { return instruction_; }

 private:
  HInstruction* const instruction_;
  HUseListNode* const tail_;

  DISALLOW_COPY_AND_ASSIGN(HUseListNode);
};

class HInstruction : public ArenaObject {
 public:
  HInstruction() : previous_(nullptr), next_(nullptr), block_(nullptr), id_(-1), uses_(nullptr) { }
  virtual ~HInstruction() { }

  HInstruction* next() const { return next_; }
  HInstruction* previous() const { return previous_; }

  HBasicBlock* block() const { return block_; }
  void set_block(HBasicBlock* block) { block_ = block; }

  virtual intptr_t InputCount() const  = 0;
  virtual HInstruction* InputAt(intptr_t i) const = 0;

  virtual void Accept(HGraphVisitor* visitor) = 0;
  virtual const char* DebugName() const = 0;

  void AddUse(HInstruction* user) {
    uses_ = new (block_->graph()->arena()) HUseListNode(user, uses_);
  }

  HUseListNode* uses() const { return uses_; }

  bool HasUses() const { return uses_ != nullptr; }

  int id() const { return id_; }
  void set_id(int id) { id_ = id; }

 private:
  HInstruction* previous_;
  HInstruction* next_;
  HBasicBlock* block_;

  // An instruction gets an id when it is added to the graph.
  // It reflects creation order. A negative id means the instruction
  // has not beed added to the graph.
  int id_;

  HUseListNode* uses_;

  friend class HBasicBlock;

  DISALLOW_COPY_AND_ASSIGN(HInstruction);
};

class HUseIterator : public ValueObject {
 public:
  explicit HUseIterator(HInstruction* instruction) : current_(instruction->uses()) { }

  bool Done() const { return current_ == nullptr; }

  void Advance() {
    DCHECK(!Done());
    current_ = current_->tail();
  }

  HInstruction* Current() const {
    DCHECK(!Done());
    return current_->instruction();
  }

 private:
  HUseListNode* current_;

  friend class HValue;
};

class HInputIterator : public ValueObject {
 public:
  explicit HInputIterator(HInstruction* instruction) : instruction_(instruction), index_(0) { }

  bool Done() const { return index_ == instruction_->InputCount(); }
  HInstruction* Current() const { return instruction_->InputAt(index_); }
  void Advance() { index_++; }

 private:
  HInstruction* instruction_;
  int index_;

  DISALLOW_COPY_AND_ASSIGN(HInputIterator);
};

class HInstructionIterator : public ValueObject {
 public:
  explicit HInstructionIterator(HBasicBlock* block)
      : instruction_(block->first_instruction()) {
    next_ = Done() ? nullptr : instruction_->next();
  }

  bool Done() const { return instruction_ == nullptr; }
  HInstruction* Current() const { return instruction_; }
  void Advance() {
    instruction_ = next_;
    next_ = Done() ? nullptr : instruction_->next();
  }

 private:
  HInstruction* instruction_;
  HInstruction* next_;
};

// An embedded container with N elements of type T.  Used (with partial
// specialization for N=0) because embedded arrays cannot have size 0.
template<typename T, intptr_t N>
class EmbeddedArray {
 public:
  EmbeddedArray() : elements_() { }

  intptr_t length() const { return N; }

  const T& operator[](intptr_t i) const {
    DCHECK_LT(i, length());
    return elements_[i];
  }

  T& operator[](intptr_t i) {
    DCHECK_LT(i, length());
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
  HTemplateInstruction<N>() : inputs_() { }
  virtual ~HTemplateInstruction() { }

  virtual intptr_t InputCount() const { return N; }
  virtual HInstruction* InputAt(intptr_t i) const { return inputs_[i]; }

 protected:
  void SetRawInputAt(intptr_t i, HInstruction* instruction) {
    inputs_[i] = instruction;
  }

 private:
  EmbeddedArray<HInstruction*, N> inputs_;
};

// Represents dex's RETURN_VOID opcode. A HReturnVoid is a control flow
// instruction that branches to the exit block.
class HReturnVoid : public HTemplateInstruction<0> {
 public:
  HReturnVoid() { }

  DECLARE_INSTRUCTION(ReturnVoid)

 private:
  DISALLOW_COPY_AND_ASSIGN(HReturnVoid);
};

// The exit instruction is the only instruction of the exit block.
// Instructions aborting the method (HTrow and HReturn) must branch to the
// exit block.
class HExit : public HTemplateInstruction<0> {
 public:
  HExit() { }

  DECLARE_INSTRUCTION(Exit)

 private:
  DISALLOW_COPY_AND_ASSIGN(HExit);
};

// Jumps from one block to another.
class HGoto : public HTemplateInstruction<0> {
 public:
  HGoto() { }

  HBasicBlock* GetSuccessor() const {
    return block()->successors()->Get(0);
  }

  DECLARE_INSTRUCTION(Goto)

 private:
  DISALLOW_COPY_AND_ASSIGN(HGoto);
};

// Conditional branch. A block ending with an HIf instruction must have
// two successors.
class HIf : public HTemplateInstruction<1> {
 public:
  explicit HIf(HInstruction* input) {
    SetRawInputAt(0, input);
  }

  DECLARE_INSTRUCTION(If)

 private:
  DISALLOW_COPY_AND_ASSIGN(HIf);
};

// Instruction to check if two inputs are equal to each other.
class HEqual : public HTemplateInstruction<2> {
 public:
  HEqual(HInstruction* first, HInstruction* second) {
    SetRawInputAt(0, first);
    SetRawInputAt(1, second);
  }

  DECLARE_INSTRUCTION(Equal)

 private:
  DISALLOW_COPY_AND_ASSIGN(HEqual);
};

// A local in the graph. Corresponds to a Dex register.
class HLocal : public HTemplateInstruction<0> {
 public:
  explicit HLocal(uint16_t reg_number) : reg_number_(reg_number) { }

  DECLARE_INSTRUCTION(Local)

 private:
  // The register number in Dex.
  uint16_t reg_number_;

  DISALLOW_COPY_AND_ASSIGN(HLocal);
};

// Load a given local. The local is an input of this instruction.
class HLoadLocal : public HTemplateInstruction<1> {
 public:
  explicit HLoadLocal(HLocal* local) {
    SetRawInputAt(0, local);
  }

  DECLARE_INSTRUCTION(LoadLocal)

 private:
  DISALLOW_COPY_AND_ASSIGN(HLoadLocal);
};

// Store a value in a given local. This instruction has two inputs: the value
// and the local.
class HStoreLocal : public HTemplateInstruction<2> {
 public:
  HStoreLocal(HLocal* local, HInstruction* value) {
    SetRawInputAt(0, local);
    SetRawInputAt(1, value);
  }

  DECLARE_INSTRUCTION(StoreLocal)

 private:
  DISALLOW_COPY_AND_ASSIGN(HStoreLocal);
};

// Constants of the type int. Those can be from Dex instructions, or
// synthesized (for example with the if-eqz instruction).
class HIntConstant : public HTemplateInstruction<0> {
 public:
  explicit HIntConstant(int32_t value) : value_(value) { }

  DECLARE_INSTRUCTION(IntConstant)

 private:
  const int32_t value_;

  DISALLOW_COPY_AND_ASSIGN(HIntConstant);
};

class HGraphVisitor : public ValueObject {
 public:
  explicit HGraphVisitor(HGraph* graph) : graph_(graph) { }
  virtual ~HGraphVisitor() { }

  virtual void VisitInstruction(HInstruction* instruction) { }
  virtual void VisitBasicBlock(HBasicBlock* block);

  void VisitInsertionOrder();

  HGraph* graph() const { return graph_; }

  // Visit functions for instruction classes.
#define DECLARE_VISIT_INSTRUCTION(name)                                        \
  virtual void Visit##name(H##name* instr) { VisitInstruction(instr); }

  FOR_EACH_INSTRUCTION(DECLARE_VISIT_INSTRUCTION)

#undef DECLARE_VISIT_INSTRUCTION

 private:
  HGraph* graph_;

  DISALLOW_COPY_AND_ASSIGN(HGraphVisitor);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_NODES_H_
