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
class LocationSummary;

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
        maximum_number_of_out_vregs_(0),
        number_of_vregs_(0),
        number_of_in_vregs_(0),
        current_instruction_id_(0) { }

  ArenaAllocator* GetArena() const { return arena_; }
  const GrowableArray<HBasicBlock*>* GetBlocks() const { return &blocks_; }

  HBasicBlock* GetEntryBlock() const { return entry_block_; }
  HBasicBlock* GetExitBlock() const { return exit_block_; }

  void SetEntryBlock(HBasicBlock* block) { entry_block_ = block; }
  void SetExitBlock(HBasicBlock* block) { exit_block_ = block; }

  void AddBlock(HBasicBlock* block);
  void BuildDominatorTree();

  int GetNextInstructionId() {
    return current_instruction_id_++;
  }

  uint16_t GetMaximumNumberOfOutVRegs() const {
    return maximum_number_of_out_vregs_;
  }

  void UpdateMaximumNumberOfOutVRegs(uint16_t new_value) {
    maximum_number_of_out_vregs_ = std::max(new_value, maximum_number_of_out_vregs_);
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

  // The maximum number of virtual registers arguments passed to a HInvoke in this graph.
  uint16_t maximum_number_of_out_vregs_;

  // The number of virtual registers in this method. Contains the parameters.
  uint16_t number_of_vregs_;

  // The number of virtual registers used by parameters of this method.
  uint16_t number_of_in_vregs_;

  // The current id to assign to a newly added instruction. See HInstruction.id_.
  int current_instruction_id_;

  DISALLOW_COPY_AND_ASSIGN(HGraph);
};

class HLoopInformation : public ArenaObject {
 public:
  HLoopInformation(HBasicBlock* header, HGraph* graph)
      : header_(header),
        back_edges_(graph->GetArena(), kDefaultNumberOfBackEdges) { }

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
        predecessors_(graph->GetArena(), kDefaultNumberOfPredecessors),
        successors_(graph->GetArena(), kDefaultNumberOfSuccessors),
        first_instruction_(nullptr),
        last_instruction_(nullptr),
        loop_information_(nullptr),
        dominator_(nullptr),
        block_id_(-1) { }

  const GrowableArray<HBasicBlock*>* GetPredecessors() const {
    return &predecessors_;
  }

  const GrowableArray<HBasicBlock*>* GetSuccessors() const {
    return &successors_;
  }

  void AddBackEdge(HBasicBlock* back_edge) {
    if (loop_information_ == nullptr) {
      loop_information_ = new (graph_->GetArena()) HLoopInformation(this, graph_);
    }
    loop_information_->AddBackEdge(back_edge);
  }

  HGraph* GetGraph() const { return graph_; }

  int GetBlockId() const { return block_id_; }
  void SetBlockId(int id) { block_id_ = id; }

  HBasicBlock* GetDominator() const { return dominator_; }
  void SetDominator(HBasicBlock* dominator) { dominator_ = dominator; }

  int NumberOfBackEdges() const {
    return loop_information_ == nullptr
        ? 0
        : loop_information_->NumberOfBackEdges();
  }

  HInstruction* GetFirstInstruction() const { return first_instruction_; }
  HInstruction* GetLastInstruction() const { return last_instruction_; }

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
  M(Add)                                                   \
  M(Equal)                                                 \
  M(Exit)                                                  \
  M(Goto)                                                  \
  M(If)                                                    \
  M(IntConstant)                                           \
  M(InvokeStatic)                                          \
  M(LoadLocal)                                             \
  M(Local)                                                 \
  M(NewInstance)                                           \
  M(ParameterValue)                                        \
  M(PushArgument)                                          \
  M(Return)                                                \
  M(ReturnVoid)                                            \
  M(StoreLocal)                                            \
  M(Sub)                                                   \

#define FORWARD_DECLARATION(type) class H##type;
FOR_EACH_INSTRUCTION(FORWARD_DECLARATION)
#undef FORWARD_DECLARATION

#define DECLARE_INSTRUCTION(type)                          \
  virtual void Accept(HGraphVisitor* visitor);             \
  virtual const char* DebugName() const { return #type; }  \
  virtual H##type* As##type() { return this; }             \

class HUseListNode : public ArenaObject {
 public:
  HUseListNode(HInstruction* instruction, HUseListNode* tail)
      : instruction_(instruction), tail_(tail) { }

  HUseListNode* GetTail() const { return tail_; }
  HInstruction* GetInstruction() const { return instruction_; }

 private:
  HInstruction* const instruction_;
  HUseListNode* const tail_;

  DISALLOW_COPY_AND_ASSIGN(HUseListNode);
};

class HInstruction : public ArenaObject {
 public:
  HInstruction()
      : previous_(nullptr),
        next_(nullptr),
        block_(nullptr),
        id_(-1),
        uses_(nullptr),
        locations_(nullptr) { }

  virtual ~HInstruction() { }

  HInstruction* GetNext() const { return next_; }
  HInstruction* GetPrevious() const { return previous_; }

  HBasicBlock* GetBlock() const { return block_; }
  void SetBlock(HBasicBlock* block) { block_ = block; }

  virtual intptr_t InputCount() const  = 0;
  virtual HInstruction* InputAt(intptr_t i) const = 0;

  virtual void Accept(HGraphVisitor* visitor) = 0;
  virtual const char* DebugName() const = 0;

  void AddUse(HInstruction* user) {
    uses_ = new (block_->GetGraph()->GetArena()) HUseListNode(user, uses_);
  }

  HUseListNode* GetUses() const { return uses_; }

  bool HasUses() const { return uses_ != nullptr; }

  int GetId() const { return id_; }
  void SetId(int id) { id_ = id; }

  LocationSummary* GetLocations() const { return locations_; }
  void SetLocations(LocationSummary* locations) { locations_ = locations; }

#define INSTRUCTION_TYPE_CHECK(type)                                           \
  virtual H##type* As##type() { return nullptr; }

  FOR_EACH_INSTRUCTION(INSTRUCTION_TYPE_CHECK)
#undef INSTRUCTION_TYPE_CHECK

 private:
  HInstruction* previous_;
  HInstruction* next_;
  HBasicBlock* block_;

  // An instruction gets an id when it is added to the graph.
  // It reflects creation order. A negative id means the instruction
  // has not beed added to the graph.
  int id_;

  HUseListNode* uses_;

  // Set by the code generator.
  LocationSummary* locations_;

  friend class HBasicBlock;

  DISALLOW_COPY_AND_ASSIGN(HInstruction);
};

class HUseIterator : public ValueObject {
 public:
  explicit HUseIterator(HInstruction* instruction) : current_(instruction->GetUses()) { }

  bool Done() const { return current_ == nullptr; }

  void Advance() {
    DCHECK(!Done());
    current_ = current_->GetTail();
  }

  HInstruction* Current() const {
    DCHECK(!Done());
    return current_->GetInstruction();
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
      : instruction_(block->GetFirstInstruction()) {
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
};

// An embedded container with N elements of type T.  Used (with partial
// specialization for N=0) because embedded arrays cannot have size 0.
template<typename T, intptr_t N>
class EmbeddedArray {
 public:
  EmbeddedArray() : elements_() { }

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

// Represents dex's RETURN opcodes. A HReturn is a control flow
// instruction that branches to the exit block.
class HReturn : public HTemplateInstruction<1> {
 public:
  explicit HReturn(HInstruction* value) {
    SetRawInputAt(0, value);
  }

  DECLARE_INSTRUCTION(Return)

 private:
  DISALLOW_COPY_AND_ASSIGN(HReturn);
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
    return GetBlock()->GetSuccessors()->Get(0);
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

  HBasicBlock* IfTrueSuccessor() const {
    return GetBlock()->GetSuccessors()->Get(0);
  }

  HBasicBlock* IfFalseSuccessor() const {
    return GetBlock()->GetSuccessors()->Get(1);
  }

  DECLARE_INSTRUCTION(If)

 private:
  DISALLOW_COPY_AND_ASSIGN(HIf);
};

class HBinaryOperation : public HTemplateInstruction<2> {
 public:
  HBinaryOperation(Primitive::Type result_type,
                   HInstruction* left,
                   HInstruction* right) : result_type_(result_type) {
    SetRawInputAt(0, left);
    SetRawInputAt(1, right);
  }

  HInstruction* GetLeft() const { return InputAt(0); }
  HInstruction* GetRight() const { return InputAt(1); }
  Primitive::Type GetResultType() const { return result_type_; }

  virtual bool IsCommutative() { return false; }

 private:
  const Primitive::Type result_type_;

  DISALLOW_COPY_AND_ASSIGN(HBinaryOperation);
};


// Instruction to check if two inputs are equal to each other.
class HEqual : public HBinaryOperation {
 public:
  HEqual(HInstruction* first, HInstruction* second)
      : HBinaryOperation(Primitive::kPrimBoolean, first, second) {}

  virtual bool IsCommutative() { return true; }

  DECLARE_INSTRUCTION(Equal)

 private:
  DISALLOW_COPY_AND_ASSIGN(HEqual);
};

// A local in the graph. Corresponds to a Dex register.
class HLocal : public HTemplateInstruction<0> {
 public:
  explicit HLocal(uint16_t reg_number) : reg_number_(reg_number) { }

  DECLARE_INSTRUCTION(Local)

  uint16_t GetRegNumber() const { return reg_number_; }

 private:
  // The Dex register number.
  const uint16_t reg_number_;

  DISALLOW_COPY_AND_ASSIGN(HLocal);
};

// Load a given local. The local is an input of this instruction.
class HLoadLocal : public HTemplateInstruction<1> {
 public:
  explicit HLoadLocal(HLocal* local) {
    SetRawInputAt(0, local);
  }

  HLocal* GetLocal() const { return reinterpret_cast<HLocal*>(InputAt(0)); }

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

  HLocal* GetLocal() const { return reinterpret_cast<HLocal*>(InputAt(0)); }

  DECLARE_INSTRUCTION(StoreLocal)

 private:
  DISALLOW_COPY_AND_ASSIGN(HStoreLocal);
};

// Constants of the type int. Those can be from Dex instructions, or
// synthesized (for example with the if-eqz instruction).
class HIntConstant : public HTemplateInstruction<0> {
 public:
  explicit HIntConstant(int32_t value) : value_(value) { }

  int32_t GetValue() const { return value_; }

  DECLARE_INSTRUCTION(IntConstant)

 private:
  const int32_t value_;

  DISALLOW_COPY_AND_ASSIGN(HIntConstant);
};

class HInvoke : public HInstruction {
 public:
  HInvoke(ArenaAllocator* arena, uint32_t number_of_arguments, uint32_t dex_pc)
    : inputs_(arena, number_of_arguments),
      dex_pc_(dex_pc) {
    inputs_.SetSize(number_of_arguments);
  }

  virtual intptr_t InputCount() const { return inputs_.Size(); }
  virtual HInstruction* InputAt(intptr_t i) const { return inputs_.Get(i); }

  void SetArgumentAt(size_t index, HInstruction* argument) {
    inputs_.Put(index, argument);
  }

  uint32_t GetDexPc() const { return dex_pc_; }

 protected:
  GrowableArray<HInstruction*> inputs_;
  const uint32_t dex_pc_;

 private:
  DISALLOW_COPY_AND_ASSIGN(HInvoke);
};

class HInvokeStatic : public HInvoke {
 public:
  HInvokeStatic(ArenaAllocator* arena,
                uint32_t number_of_arguments,
                uint32_t dex_pc,
                uint32_t index_in_dex_cache)
      : HInvoke(arena, number_of_arguments, dex_pc), index_in_dex_cache_(index_in_dex_cache) {}

  uint32_t GetIndexInDexCache() const { return index_in_dex_cache_; }

  DECLARE_INSTRUCTION(InvokeStatic)

 private:
  const uint32_t index_in_dex_cache_;

  DISALLOW_COPY_AND_ASSIGN(HInvokeStatic);
};

class HNewInstance : public HTemplateInstruction<0> {
 public:
  HNewInstance(uint32_t dex_pc, uint16_t type_index) : dex_pc_(dex_pc), type_index_(type_index) {}

  uint32_t GetDexPc() const { return dex_pc_; }
  uint16_t GetTypeIndex() const { return type_index_; }

  DECLARE_INSTRUCTION(NewInstance)

 private:
  const uint32_t dex_pc_;
  const uint16_t type_index_;

  DISALLOW_COPY_AND_ASSIGN(HNewInstance);
};

// HPushArgument nodes are inserted after the evaluation of an argument
// of a call. Their mere purpose is to ease the code generator's work.
class HPushArgument : public HTemplateInstruction<1> {
 public:
  HPushArgument(HInstruction* argument, uint8_t argument_index) : argument_index_(argument_index) {
    SetRawInputAt(0, argument);
  }

  uint8_t GetArgumentIndex() const { return argument_index_; }

  DECLARE_INSTRUCTION(PushArgument)

 private:
  const uint8_t argument_index_;

  DISALLOW_COPY_AND_ASSIGN(HPushArgument);
};

class HAdd : public HBinaryOperation {
 public:
  HAdd(Primitive::Type result_type, HInstruction* left, HInstruction* right)
      : HBinaryOperation(result_type, left, right) {}

  virtual bool IsCommutative() { return true; }

  DECLARE_INSTRUCTION(Add);

 private:
  DISALLOW_COPY_AND_ASSIGN(HAdd);
};

class HSub : public HBinaryOperation {
 public:
  HSub(Primitive::Type result_type, HInstruction* left, HInstruction* right)
      : HBinaryOperation(result_type, left, right) {}

  virtual bool IsCommutative() { return false; }

  DECLARE_INSTRUCTION(Sub);

 private:
  DISALLOW_COPY_AND_ASSIGN(HSub);
};

// The value of a parameter in this method. Its location depends on
// the calling convention.
class HParameterValue : public HTemplateInstruction<0> {
 public:
  explicit HParameterValue(uint8_t index) : index_(index) {}

  uint8_t GetIndex() const { return index_; }

  DECLARE_INSTRUCTION(ParameterValue);

 private:
  // The index of this parameter in the parameters list. Must be less
  // than HGraph::number_of_in_vregs_;
  const uint8_t index_;

  DISALLOW_COPY_AND_ASSIGN(HParameterValue);
};

class HGraphVisitor : public ValueObject {
 public:
  explicit HGraphVisitor(HGraph* graph) : graph_(graph) { }
  virtual ~HGraphVisitor() { }

  virtual void VisitInstruction(HInstruction* instruction) { }
  virtual void VisitBasicBlock(HBasicBlock* block);

  void VisitInsertionOrder();

  HGraph* GetGraph() const { return graph_; }

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
