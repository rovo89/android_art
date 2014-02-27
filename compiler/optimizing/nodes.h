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
        dominator_order_(arena, kDefaultNumberOfBlocks) { }

  ArenaAllocator* arena() const { return arena_; }
  const GrowableArray<HBasicBlock*>* blocks() const { return &blocks_; }

  void AddBlock(HBasicBlock* block);
  void BuildDominatorTree();

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

  HBasicBlock* GetEntryBlock() const { return blocks_.Get(0); }

  ArenaAllocator* const arena_;

  // List of blocks in insertion order.
  GrowableArray<HBasicBlock*> blocks_;

  // List of blocks to perform a pre-order dominator tree traversal.
  GrowableArray<HBasicBlock*> dominator_order_;

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
  M(Exit)                                                  \
  M(Goto)                                                  \
  M(If)                                                    \
  M(ReturnVoid)                                            \

#define DECLARE_INSTRUCTION(type)                          \
  virtual void Accept(HGraphVisitor* visitor);             \
  virtual const char* DebugName() const { return #type; }  \

class HInstruction : public ArenaObject {
 public:
  HInstruction() : previous_(nullptr), next_(nullptr) { }
  virtual ~HInstruction() { }

  HInstruction* next() const { return next_; }
  HInstruction* previous() const { return previous_; }

  virtual intptr_t InputCount() const  = 0;
  virtual HInstruction* InputAt(intptr_t i) const = 0;

  virtual void Accept(HGraphVisitor* visitor) = 0;
  virtual const char* DebugName() const = 0;

 private:
  HInstruction* previous_;
  HInstruction* next_;

  friend class HBasicBlock;

  DISALLOW_COPY_AND_ASSIGN(HInstruction);
};

class HInstructionIterator : public ValueObject {
 public:
  explicit HInstructionIterator(HBasicBlock* block)
      : instruction_(block->first_instruction()) {
    next_ = Done() ? nullptr : instruction_->next();
  }

  inline bool Done() const { return instruction_ == nullptr; }
  inline HInstruction* Current() { return instruction_; }
  inline void Advance() {
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
    ASSERT(i < length());
    return elements_[i];
  }

  T& operator[](intptr_t i) {
    ASSERT(i < length());
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

  DECLARE_INSTRUCTION(Goto)

 private:
  DISALLOW_COPY_AND_ASSIGN(HGoto);
};

// Conditional branch. A block ending with an HIf instruction must have
// two successors.
// TODO: Make it take an input.
class HIf : public HTemplateInstruction<0> {
 public:
  HIf() { }

  DECLARE_INSTRUCTION(If)

 private:
  DISALLOW_COPY_AND_ASSIGN(HIf);
};

class HGraphVisitor : public ValueObject {
 public:
  explicit HGraphVisitor(HGraph* graph) : graph_(graph) { }
  virtual ~HGraphVisitor() { }

  virtual void VisitInstruction(HInstruction* instruction) { }
  virtual void VisitBasicBlock(HBasicBlock* block);

  void VisitInsertionOrder();

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
