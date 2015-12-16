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

#include <algorithm>
#include <array>
#include <type_traits>

#include "base/arena_bit_vector.h"
#include "base/arena_containers.h"
#include "base/arena_object.h"
#include "base/stl_util.h"
#include "dex/compiler_enums.h"
#include "entrypoints/quick/quick_entrypoints_enum.h"
#include "handle.h"
#include "handle_scope.h"
#include "invoke_type.h"
#include "locations.h"
#include "method_reference.h"
#include "mirror/class.h"
#include "offsets.h"
#include "primitive.h"
#include "utils/array_ref.h"

namespace art {

class GraphChecker;
class HBasicBlock;
class HCurrentMethod;
class HDoubleConstant;
class HEnvironment;
class HFakeString;
class HFloatConstant;
class HGraphBuilder;
class HGraphVisitor;
class HInstruction;
class HIntConstant;
class HInvoke;
class HLongConstant;
class HNullConstant;
class HPhi;
class HSuspendCheck;
class HTryBoundary;
class LiveInterval;
class LocationSummary;
class SlowPathCode;
class SsaBuilder;

namespace mirror {
class DexCache;
}  // namespace mirror

static const int kDefaultNumberOfBlocks = 8;
static const int kDefaultNumberOfSuccessors = 2;
static const int kDefaultNumberOfPredecessors = 2;
static const int kDefaultNumberOfExceptionalPredecessors = 0;
static const int kDefaultNumberOfDominatedBlocks = 1;
static const int kDefaultNumberOfBackEdges = 1;

static constexpr uint32_t kMaxIntShiftValue = 0x1f;
static constexpr uint64_t kMaxLongShiftValue = 0x3f;

static constexpr uint32_t kUnknownFieldIndex = static_cast<uint32_t>(-1);
static constexpr uint16_t kUnknownClassDefIndex = static_cast<uint16_t>(-1);

static constexpr InvokeType kInvalidInvokeType = static_cast<InvokeType>(-1);

static constexpr uint32_t kNoDexPc = -1;

enum IfCondition {
  // All types.
  kCondEQ,  // ==
  kCondNE,  // !=
  // Signed integers and floating-point numbers.
  kCondLT,  // <
  kCondLE,  // <=
  kCondGT,  // >
  kCondGE,  // >=
  // Unsigned integers.
  kCondB,   // <
  kCondBE,  // <=
  kCondA,   // >
  kCondAE,  // >=
};

enum BuildSsaResult {
  kBuildSsaFailNonNaturalLoop,
  kBuildSsaFailThrowCatchLoop,
  kBuildSsaFailAmbiguousArrayGet,
  kBuildSsaSuccess,
};

class HInstructionList : public ValueObject {
 public:
  HInstructionList() : first_instruction_(nullptr), last_instruction_(nullptr) {}

  void AddInstruction(HInstruction* instruction);
  void RemoveInstruction(HInstruction* instruction);

  // Insert `instruction` before/after an existing instruction `cursor`.
  void InsertInstructionBefore(HInstruction* instruction, HInstruction* cursor);
  void InsertInstructionAfter(HInstruction* instruction, HInstruction* cursor);

  // Return true if this list contains `instruction`.
  bool Contains(HInstruction* instruction) const;

  // Return true if `instruction1` is found before `instruction2` in
  // this instruction list and false otherwise.  Abort if none
  // of these instructions is found.
  bool FoundBefore(const HInstruction* instruction1,
                   const HInstruction* instruction2) const;

  bool IsEmpty() const { return first_instruction_ == nullptr; }
  void Clear() { first_instruction_ = last_instruction_ = nullptr; }

  // Update the block of all instructions to be `block`.
  void SetBlockOfInstructions(HBasicBlock* block) const;

  void AddAfter(HInstruction* cursor, const HInstructionList& instruction_list);
  void Add(const HInstructionList& instruction_list);

  // Return the number of instructions in the list. This is an expensive operation.
  size_t CountSize() const;

 private:
  HInstruction* first_instruction_;
  HInstruction* last_instruction_;

  friend class HBasicBlock;
  friend class HGraph;
  friend class HInstruction;
  friend class HInstructionIterator;
  friend class HBackwardInstructionIterator;

  DISALLOW_COPY_AND_ASSIGN(HInstructionList);
};

class ReferenceTypeInfo : ValueObject {
 public:
  typedef Handle<mirror::Class> TypeHandle;

  static ReferenceTypeInfo Create(TypeHandle type_handle, bool is_exact) {
    // The constructor will check that the type_handle is valid.
    return ReferenceTypeInfo(type_handle, is_exact);
  }

  static ReferenceTypeInfo CreateInvalid() { return ReferenceTypeInfo(); }

  static bool IsValidHandle(TypeHandle handle) SHARED_REQUIRES(Locks::mutator_lock_) {
    return handle.GetReference() != nullptr;
  }

  bool IsValid() const SHARED_REQUIRES(Locks::mutator_lock_) {
    return IsValidHandle(type_handle_);
  }

  bool IsExact() const { return is_exact_; }

  bool IsObjectClass() const SHARED_REQUIRES(Locks::mutator_lock_) {
    DCHECK(IsValid());
    return GetTypeHandle()->IsObjectClass();
  }

  bool IsStringClass() const SHARED_REQUIRES(Locks::mutator_lock_) {
    DCHECK(IsValid());
    return GetTypeHandle()->IsStringClass();
  }

  bool IsObjectArray() const SHARED_REQUIRES(Locks::mutator_lock_) {
    DCHECK(IsValid());
    return IsArrayClass() && GetTypeHandle()->GetComponentType()->IsObjectClass();
  }

  bool IsInterface() const SHARED_REQUIRES(Locks::mutator_lock_) {
    DCHECK(IsValid());
    return GetTypeHandle()->IsInterface();
  }

  bool IsArrayClass() const SHARED_REQUIRES(Locks::mutator_lock_) {
    DCHECK(IsValid());
    return GetTypeHandle()->IsArrayClass();
  }

  bool IsPrimitiveArrayClass() const SHARED_REQUIRES(Locks::mutator_lock_) {
    DCHECK(IsValid());
    return GetTypeHandle()->IsPrimitiveArray();
  }

  bool IsNonPrimitiveArrayClass() const SHARED_REQUIRES(Locks::mutator_lock_) {
    DCHECK(IsValid());
    return GetTypeHandle()->IsArrayClass() && !GetTypeHandle()->IsPrimitiveArray();
  }

  bool CanArrayHold(ReferenceTypeInfo rti)  const SHARED_REQUIRES(Locks::mutator_lock_) {
    DCHECK(IsValid());
    if (!IsExact()) return false;
    if (!IsArrayClass()) return false;
    return GetTypeHandle()->GetComponentType()->IsAssignableFrom(rti.GetTypeHandle().Get());
  }

  bool CanArrayHoldValuesOf(ReferenceTypeInfo rti)  const SHARED_REQUIRES(Locks::mutator_lock_) {
    DCHECK(IsValid());
    if (!IsExact()) return false;
    if (!IsArrayClass()) return false;
    if (!rti.IsArrayClass()) return false;
    return GetTypeHandle()->GetComponentType()->IsAssignableFrom(
        rti.GetTypeHandle()->GetComponentType());
  }

  Handle<mirror::Class> GetTypeHandle() const { return type_handle_; }

  bool IsSupertypeOf(ReferenceTypeInfo rti) const SHARED_REQUIRES(Locks::mutator_lock_) {
    DCHECK(IsValid());
    DCHECK(rti.IsValid());
    return GetTypeHandle()->IsAssignableFrom(rti.GetTypeHandle().Get());
  }

  bool IsStrictSupertypeOf(ReferenceTypeInfo rti) const SHARED_REQUIRES(Locks::mutator_lock_) {
    DCHECK(IsValid());
    DCHECK(rti.IsValid());
    return GetTypeHandle().Get() != rti.GetTypeHandle().Get() &&
        GetTypeHandle()->IsAssignableFrom(rti.GetTypeHandle().Get());
  }

  // Returns true if the type information provide the same amount of details.
  // Note that it does not mean that the instructions have the same actual type
  // (because the type can be the result of a merge).
  bool IsEqual(ReferenceTypeInfo rti) SHARED_REQUIRES(Locks::mutator_lock_) {
    if (!IsValid() && !rti.IsValid()) {
      // Invalid types are equal.
      return true;
    }
    if (!IsValid() || !rti.IsValid()) {
      // One is valid, the other not.
      return false;
    }
    return IsExact() == rti.IsExact()
        && GetTypeHandle().Get() == rti.GetTypeHandle().Get();
  }

 private:
  ReferenceTypeInfo();
  ReferenceTypeInfo(TypeHandle type_handle, bool is_exact);

  // The class of the object.
  TypeHandle type_handle_;
  // Whether or not the type is exact or a superclass of the actual type.
  // Whether or not we have any information about this type.
  bool is_exact_;
};

std::ostream& operator<<(std::ostream& os, const ReferenceTypeInfo& rhs);

// Control-flow graph of a method. Contains a list of basic blocks.
class HGraph : public ArenaObject<kArenaAllocGraph> {
 public:
  HGraph(ArenaAllocator* arena,
         const DexFile& dex_file,
         uint32_t method_idx,
         bool should_generate_constructor_barrier,
         InstructionSet instruction_set,
         InvokeType invoke_type = kInvalidInvokeType,
         bool debuggable = false,
         int start_instruction_id = 0)
      : arena_(arena),
        blocks_(arena->Adapter(kArenaAllocBlockList)),
        reverse_post_order_(arena->Adapter(kArenaAllocReversePostOrder)),
        linear_order_(arena->Adapter(kArenaAllocLinearOrder)),
        entry_block_(nullptr),
        exit_block_(nullptr),
        maximum_number_of_out_vregs_(0),
        number_of_vregs_(0),
        number_of_in_vregs_(0),
        temporaries_vreg_slots_(0),
        has_bounds_checks_(false),
        has_try_catch_(false),
        debuggable_(debuggable),
        current_instruction_id_(start_instruction_id),
        dex_file_(dex_file),
        method_idx_(method_idx),
        invoke_type_(invoke_type),
        in_ssa_form_(false),
        should_generate_constructor_barrier_(should_generate_constructor_barrier),
        instruction_set_(instruction_set),
        cached_null_constant_(nullptr),
        cached_int_constants_(std::less<int32_t>(), arena->Adapter(kArenaAllocConstantsMap)),
        cached_float_constants_(std::less<int32_t>(), arena->Adapter(kArenaAllocConstantsMap)),
        cached_long_constants_(std::less<int64_t>(), arena->Adapter(kArenaAllocConstantsMap)),
        cached_double_constants_(std::less<int64_t>(), arena->Adapter(kArenaAllocConstantsMap)),
        cached_current_method_(nullptr),
        inexact_object_rti_(ReferenceTypeInfo::CreateInvalid()) {
    blocks_.reserve(kDefaultNumberOfBlocks);
  }

  ArenaAllocator* GetArena() const { return arena_; }
  const ArenaVector<HBasicBlock*>& GetBlocks() const { return blocks_; }

  bool IsInSsaForm() const { return in_ssa_form_; }

  HBasicBlock* GetEntryBlock() const { return entry_block_; }
  HBasicBlock* GetExitBlock() const { return exit_block_; }
  bool HasExitBlock() const { return exit_block_ != nullptr; }

  void SetEntryBlock(HBasicBlock* block) { entry_block_ = block; }
  void SetExitBlock(HBasicBlock* block) { exit_block_ = block; }

  void AddBlock(HBasicBlock* block);

  // Try building the SSA form of this graph, with dominance computation and
  // loop recognition. Returns a code specifying that it was successful or the
  // reason for failure.
  BuildSsaResult TryBuildingSsa(StackHandleScopeCollection* handles);

  void ComputeDominanceInformation();
  void ClearDominanceInformation();

  void BuildDominatorTree();
  void SimplifyCFG();
  void SimplifyCatchBlocks();

  // Analyze all natural loops in this graph. Returns a code specifying that it
  // was successful or the reason for failure. The method will fail if a loop
  // is not natural, that is the header does not dominate a back edge, or if it
  // is a throw-catch loop, i.e. the header is a catch block.
  BuildSsaResult AnalyzeNaturalLoops() const;

  // Iterate over blocks to compute try block membership. Needs reverse post
  // order and loop information.
  void ComputeTryBlockInformation();

  // Inline this graph in `outer_graph`, replacing the given `invoke` instruction.
  // Returns the instruction used to replace the invoke expression or null if the
  // invoke is for a void method.
  HInstruction* InlineInto(HGraph* outer_graph, HInvoke* invoke);

  // Need to add a couple of blocks to test if the loop body is entered and
  // put deoptimization instructions, etc.
  void TransformLoopHeaderForBCE(HBasicBlock* header);

  // Removes `block` from the graph. Assumes `block` has been disconnected from
  // other blocks and has no instructions or phis.
  void DeleteDeadEmptyBlock(HBasicBlock* block);

  // Splits the edge between `block` and `successor` while preserving the
  // indices in the predecessor/successor lists. If there are multiple edges
  // between the blocks, the lowest indices are used.
  // Returns the new block which is empty and has the same dex pc as `successor`.
  HBasicBlock* SplitEdge(HBasicBlock* block, HBasicBlock* successor);

  void SplitCriticalEdge(HBasicBlock* block, HBasicBlock* successor);
  void SimplifyLoop(HBasicBlock* header);

  int32_t GetNextInstructionId() {
    DCHECK_NE(current_instruction_id_, INT32_MAX);
    return current_instruction_id_++;
  }

  int32_t GetCurrentInstructionId() const {
    return current_instruction_id_;
  }

  void SetCurrentInstructionId(int32_t id) {
    current_instruction_id_ = id;
  }

  uint16_t GetMaximumNumberOfOutVRegs() const {
    return maximum_number_of_out_vregs_;
  }

  void SetMaximumNumberOfOutVRegs(uint16_t new_value) {
    maximum_number_of_out_vregs_ = new_value;
  }

  void UpdateMaximumNumberOfOutVRegs(uint16_t other_value) {
    maximum_number_of_out_vregs_ = std::max(maximum_number_of_out_vregs_, other_value);
  }

  void UpdateTemporariesVRegSlots(size_t slots) {
    temporaries_vreg_slots_ = std::max(slots, temporaries_vreg_slots_);
  }

  size_t GetTemporariesVRegSlots() const {
    DCHECK(!in_ssa_form_);
    return temporaries_vreg_slots_;
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

  uint16_t GetNumberOfLocalVRegs() const {
    DCHECK(!in_ssa_form_);
    return number_of_vregs_ - number_of_in_vregs_;
  }

  const ArenaVector<HBasicBlock*>& GetReversePostOrder() const {
    return reverse_post_order_;
  }

  const ArenaVector<HBasicBlock*>& GetLinearOrder() const {
    return linear_order_;
  }

  bool HasBoundsChecks() const {
    return has_bounds_checks_;
  }

  void SetHasBoundsChecks(bool value) {
    has_bounds_checks_ = value;
  }

  bool ShouldGenerateConstructorBarrier() const {
    return should_generate_constructor_barrier_;
  }

  bool IsDebuggable() const { return debuggable_; }

  // Returns a constant of the given type and value. If it does not exist
  // already, it is created and inserted into the graph. This method is only for
  // integral types.
  HConstant* GetConstant(Primitive::Type type, int64_t value, uint32_t dex_pc = kNoDexPc);

  // TODO: This is problematic for the consistency of reference type propagation
  // because it can be created anytime after the pass and thus it will be left
  // with an invalid type.
  HNullConstant* GetNullConstant(uint32_t dex_pc = kNoDexPc);

  HIntConstant* GetIntConstant(int32_t value, uint32_t dex_pc = kNoDexPc) {
    return CreateConstant(value, &cached_int_constants_, dex_pc);
  }
  HLongConstant* GetLongConstant(int64_t value, uint32_t dex_pc = kNoDexPc) {
    return CreateConstant(value, &cached_long_constants_, dex_pc);
  }
  HFloatConstant* GetFloatConstant(float value, uint32_t dex_pc = kNoDexPc) {
    return CreateConstant(bit_cast<int32_t, float>(value), &cached_float_constants_, dex_pc);
  }
  HDoubleConstant* GetDoubleConstant(double value, uint32_t dex_pc = kNoDexPc) {
    return CreateConstant(bit_cast<int64_t, double>(value), &cached_double_constants_, dex_pc);
  }

  HCurrentMethod* GetCurrentMethod();

  const DexFile& GetDexFile() const {
    return dex_file_;
  }

  uint32_t GetMethodIdx() const {
    return method_idx_;
  }

  InvokeType GetInvokeType() const {
    return invoke_type_;
  }

  InstructionSet GetInstructionSet() const {
    return instruction_set_;
  }

  bool HasTryCatch() const { return has_try_catch_; }
  void SetHasTryCatch(bool value) { has_try_catch_ = value; }

  ArtMethod* GetArtMethod() const { return art_method_; }
  void SetArtMethod(ArtMethod* method) { art_method_ = method; }

  // Returns an instruction with the opposite boolean value from 'cond'.
  // The instruction has been inserted into the graph, either as a constant, or
  // before cursor.
  HInstruction* InsertOppositeCondition(HInstruction* cond, HInstruction* cursor);

 private:
  void FindBackEdges(ArenaBitVector* visited);
  void RemoveInstructionsAsUsersFromDeadBlocks(const ArenaBitVector& visited) const;
  void RemoveDeadBlocks(const ArenaBitVector& visited);

  template <class InstructionType, typename ValueType>
  InstructionType* CreateConstant(ValueType value,
                                  ArenaSafeMap<ValueType, InstructionType*>* cache,
                                  uint32_t dex_pc = kNoDexPc) {
    // Try to find an existing constant of the given value.
    InstructionType* constant = nullptr;
    auto cached_constant = cache->find(value);
    if (cached_constant != cache->end()) {
      constant = cached_constant->second;
    }

    // If not found or previously deleted, create and cache a new instruction.
    // Don't bother reviving a previously deleted instruction, for simplicity.
    if (constant == nullptr || constant->GetBlock() == nullptr) {
      constant = new (arena_) InstructionType(value, dex_pc);
      cache->Overwrite(value, constant);
      InsertConstant(constant);
    }
    return constant;
  }

  void InsertConstant(HConstant* instruction);

  // Cache a float constant into the graph. This method should only be
  // called by the SsaBuilder when creating "equivalent" instructions.
  void CacheFloatConstant(HFloatConstant* constant);

  // See CacheFloatConstant comment.
  void CacheDoubleConstant(HDoubleConstant* constant);

  ArenaAllocator* const arena_;

  // List of blocks in insertion order.
  ArenaVector<HBasicBlock*> blocks_;

  // List of blocks to perform a reverse post order tree traversal.
  ArenaVector<HBasicBlock*> reverse_post_order_;

  // List of blocks to perform a linear order tree traversal.
  ArenaVector<HBasicBlock*> linear_order_;

  HBasicBlock* entry_block_;
  HBasicBlock* exit_block_;

  // The maximum number of virtual registers arguments passed to a HInvoke in this graph.
  uint16_t maximum_number_of_out_vregs_;

  // The number of virtual registers in this method. Contains the parameters.
  uint16_t number_of_vregs_;

  // The number of virtual registers used by parameters of this method.
  uint16_t number_of_in_vregs_;

  // Number of vreg size slots that the temporaries use (used in baseline compiler).
  size_t temporaries_vreg_slots_;

  // Has bounds checks. We can totally skip BCE if it's false.
  bool has_bounds_checks_;

  // Flag whether there are any try/catch blocks in the graph. We will skip
  // try/catch-related passes if false.
  bool has_try_catch_;

  // Indicates whether the graph should be compiled in a way that
  // ensures full debuggability. If false, we can apply more
  // aggressive optimizations that may limit the level of debugging.
  const bool debuggable_;

  // The current id to assign to a newly added instruction. See HInstruction.id_.
  int32_t current_instruction_id_;

  // The dex file from which the method is from.
  const DexFile& dex_file_;

  // The method index in the dex file.
  const uint32_t method_idx_;

  // If inlined, this encodes how the callee is being invoked.
  const InvokeType invoke_type_;

  // Whether the graph has been transformed to SSA form. Only used
  // in debug mode to ensure we are not using properties only valid
  // for non-SSA form (like the number of temporaries).
  bool in_ssa_form_;

  const bool should_generate_constructor_barrier_;

  const InstructionSet instruction_set_;

  // Cached constants.
  HNullConstant* cached_null_constant_;
  ArenaSafeMap<int32_t, HIntConstant*> cached_int_constants_;
  ArenaSafeMap<int32_t, HFloatConstant*> cached_float_constants_;
  ArenaSafeMap<int64_t, HLongConstant*> cached_long_constants_;
  ArenaSafeMap<int64_t, HDoubleConstant*> cached_double_constants_;

  HCurrentMethod* cached_current_method_;

  // The ArtMethod this graph is for. Note that for AOT, it may be null,
  // for example for methods whose declaring class could not be resolved
  // (such as when the superclass could not be found).
  ArtMethod* art_method_;

  // Keep the RTI of inexact Object to avoid having to pass stack handle
  // collection pointer to passes which may create NullConstant.
  ReferenceTypeInfo inexact_object_rti_;

  friend class SsaBuilder;           // For caching constants.
  friend class SsaLivenessAnalysis;  // For the linear order.
  ART_FRIEND_TEST(GraphTest, IfSuccessorSimpleJoinBlock1);
  DISALLOW_COPY_AND_ASSIGN(HGraph);
};

class HLoopInformation : public ArenaObject<kArenaAllocLoopInfo> {
 public:
  HLoopInformation(HBasicBlock* header, HGraph* graph)
      : header_(header),
        suspend_check_(nullptr),
        back_edges_(graph->GetArena()->Adapter(kArenaAllocLoopInfoBackEdges)),
        // Make bit vector growable, as the number of blocks may change.
        blocks_(graph->GetArena(), graph->GetBlocks().size(), true) {
    back_edges_.reserve(kDefaultNumberOfBackEdges);
  }

  HBasicBlock* GetHeader() const {
    return header_;
  }

  void SetHeader(HBasicBlock* block) {
    header_ = block;
  }

  HSuspendCheck* GetSuspendCheck() const { return suspend_check_; }
  void SetSuspendCheck(HSuspendCheck* check) { suspend_check_ = check; }
  bool HasSuspendCheck() const { return suspend_check_ != nullptr; }

  void AddBackEdge(HBasicBlock* back_edge) {
    back_edges_.push_back(back_edge);
  }

  void RemoveBackEdge(HBasicBlock* back_edge) {
    RemoveElement(back_edges_, back_edge);
  }

  bool IsBackEdge(const HBasicBlock& block) const {
    return ContainsElement(back_edges_, &block);
  }

  size_t NumberOfBackEdges() const {
    return back_edges_.size();
  }

  HBasicBlock* GetPreHeader() const;

  const ArenaVector<HBasicBlock*>& GetBackEdges() const {
    return back_edges_;
  }

  // Returns the lifetime position of the back edge that has the
  // greatest lifetime position.
  size_t GetLifetimeEnd() const;

  void ReplaceBackEdge(HBasicBlock* existing, HBasicBlock* new_back_edge) {
    ReplaceElement(back_edges_, existing, new_back_edge);
  }

  // Finds blocks that are part of this loop. Returns whether the loop is a natural loop,
  // that is the header dominates the back edge.
  bool Populate();

  // Reanalyzes the loop by removing loop info from its blocks and re-running
  // Populate(). If there are no back edges left, the loop info is completely
  // removed as well as its SuspendCheck instruction. It must be run on nested
  // inner loops first.
  void Update();

  // Returns whether this loop information contains `block`.
  // Note that this loop information *must* be populated before entering this function.
  bool Contains(const HBasicBlock& block) const;

  // Returns whether this loop information is an inner loop of `other`.
  // Note that `other` *must* be populated before entering this function.
  bool IsIn(const HLoopInformation& other) const;

  // Returns true if instruction is not defined within this loop.
  bool IsDefinedOutOfTheLoop(HInstruction* instruction) const;

  const ArenaBitVector& GetBlocks() const { return blocks_; }

  void Add(HBasicBlock* block);
  void Remove(HBasicBlock* block);

 private:
  // Internal recursive implementation of `Populate`.
  void PopulateRecursive(HBasicBlock* block);

  HBasicBlock* header_;
  HSuspendCheck* suspend_check_;
  ArenaVector<HBasicBlock*> back_edges_;
  ArenaBitVector blocks_;

  DISALLOW_COPY_AND_ASSIGN(HLoopInformation);
};

// Stores try/catch information for basic blocks.
// Note that HGraph is constructed so that catch blocks cannot simultaneously
// be try blocks.
class TryCatchInformation : public ArenaObject<kArenaAllocTryCatchInfo> {
 public:
  // Try block information constructor.
  explicit TryCatchInformation(const HTryBoundary& try_entry)
      : try_entry_(&try_entry),
        catch_dex_file_(nullptr),
        catch_type_index_(DexFile::kDexNoIndex16) {
    DCHECK(try_entry_ != nullptr);
  }

  // Catch block information constructor.
  TryCatchInformation(uint16_t catch_type_index, const DexFile& dex_file)
      : try_entry_(nullptr),
        catch_dex_file_(&dex_file),
        catch_type_index_(catch_type_index) {}

  bool IsTryBlock() const { return try_entry_ != nullptr; }

  const HTryBoundary& GetTryEntry() const {
    DCHECK(IsTryBlock());
    return *try_entry_;
  }

  bool IsCatchBlock() const { return catch_dex_file_ != nullptr; }

  bool IsCatchAllTypeIndex() const {
    DCHECK(IsCatchBlock());
    return catch_type_index_ == DexFile::kDexNoIndex16;
  }

  uint16_t GetCatchTypeIndex() const {
    DCHECK(IsCatchBlock());
    return catch_type_index_;
  }

  const DexFile& GetCatchDexFile() const {
    DCHECK(IsCatchBlock());
    return *catch_dex_file_;
  }

 private:
  // One of possibly several TryBoundary instructions entering the block's try.
  // Only set for try blocks.
  const HTryBoundary* try_entry_;

  // Exception type information. Only set for catch blocks.
  const DexFile* catch_dex_file_;
  const uint16_t catch_type_index_;
};

static constexpr size_t kNoLifetime = -1;
static constexpr uint32_t kInvalidBlockId = static_cast<uint32_t>(-1);

// A block in a method. Contains the list of instructions represented
// as a double linked list. Each block knows its predecessors and
// successors.

class HBasicBlock : public ArenaObject<kArenaAllocBasicBlock> {
 public:
  HBasicBlock(HGraph* graph, uint32_t dex_pc = kNoDexPc)
      : graph_(graph),
        predecessors_(graph->GetArena()->Adapter(kArenaAllocPredecessors)),
        successors_(graph->GetArena()->Adapter(kArenaAllocSuccessors)),
        loop_information_(nullptr),
        dominator_(nullptr),
        dominated_blocks_(graph->GetArena()->Adapter(kArenaAllocDominated)),
        block_id_(kInvalidBlockId),
        dex_pc_(dex_pc),
        lifetime_start_(kNoLifetime),
        lifetime_end_(kNoLifetime),
        try_catch_information_(nullptr) {
    predecessors_.reserve(kDefaultNumberOfPredecessors);
    successors_.reserve(kDefaultNumberOfSuccessors);
    dominated_blocks_.reserve(kDefaultNumberOfDominatedBlocks);
  }

  const ArenaVector<HBasicBlock*>& GetPredecessors() const {
    return predecessors_;
  }

  const ArenaVector<HBasicBlock*>& GetSuccessors() const {
    return successors_;
  }

  ArrayRef<HBasicBlock* const> GetNormalSuccessors() const;
  ArrayRef<HBasicBlock* const> GetExceptionalSuccessors() const;

  bool HasSuccessor(const HBasicBlock* block, size_t start_from = 0u) {
    return ContainsElement(successors_, block, start_from);
  }

  const ArenaVector<HBasicBlock*>& GetDominatedBlocks() const {
    return dominated_blocks_;
  }

  bool IsEntryBlock() const {
    return graph_->GetEntryBlock() == this;
  }

  bool IsExitBlock() const {
    return graph_->GetExitBlock() == this;
  }

  bool IsSingleGoto() const;
  bool IsSingleTryBoundary() const;

  // Returns true if this block emits nothing but a jump.
  bool IsSingleJump() const {
    HLoopInformation* loop_info = GetLoopInformation();
    return (IsSingleGoto() || IsSingleTryBoundary())
           // Back edges generate a suspend check.
           && (loop_info == nullptr || !loop_info->IsBackEdge(*this));
  }

  void AddBackEdge(HBasicBlock* back_edge) {
    if (loop_information_ == nullptr) {
      loop_information_ = new (graph_->GetArena()) HLoopInformation(this, graph_);
    }
    DCHECK_EQ(loop_information_->GetHeader(), this);
    loop_information_->AddBackEdge(back_edge);
  }

  HGraph* GetGraph() const { return graph_; }
  void SetGraph(HGraph* graph) { graph_ = graph; }

  uint32_t GetBlockId() const { return block_id_; }
  void SetBlockId(int id) { block_id_ = id; }
  uint32_t GetDexPc() const { return dex_pc_; }

  HBasicBlock* GetDominator() const { return dominator_; }
  void SetDominator(HBasicBlock* dominator) { dominator_ = dominator; }
  void AddDominatedBlock(HBasicBlock* block) { dominated_blocks_.push_back(block); }

  void RemoveDominatedBlock(HBasicBlock* block) {
    RemoveElement(dominated_blocks_, block);
  }

  void ReplaceDominatedBlock(HBasicBlock* existing, HBasicBlock* new_block) {
    ReplaceElement(dominated_blocks_, existing, new_block);
  }

  void ClearDominanceInformation();

  int NumberOfBackEdges() const {
    return IsLoopHeader() ? loop_information_->NumberOfBackEdges() : 0;
  }

  HInstruction* GetFirstInstruction() const { return instructions_.first_instruction_; }
  HInstruction* GetLastInstruction() const { return instructions_.last_instruction_; }
  const HInstructionList& GetInstructions() const { return instructions_; }
  HInstruction* GetFirstPhi() const { return phis_.first_instruction_; }
  HInstruction* GetLastPhi() const { return phis_.last_instruction_; }
  const HInstructionList& GetPhis() const { return phis_; }

  void AddSuccessor(HBasicBlock* block) {
    successors_.push_back(block);
    block->predecessors_.push_back(this);
  }

  void ReplaceSuccessor(HBasicBlock* existing, HBasicBlock* new_block) {
    size_t successor_index = GetSuccessorIndexOf(existing);
    existing->RemovePredecessor(this);
    new_block->predecessors_.push_back(this);
    successors_[successor_index] = new_block;
  }

  void ReplacePredecessor(HBasicBlock* existing, HBasicBlock* new_block) {
    size_t predecessor_index = GetPredecessorIndexOf(existing);
    existing->RemoveSuccessor(this);
    new_block->successors_.push_back(this);
    predecessors_[predecessor_index] = new_block;
  }

  // Insert `this` between `predecessor` and `successor. This method
  // preserves the indicies, and will update the first edge found between
  // `predecessor` and `successor`.
  void InsertBetween(HBasicBlock* predecessor, HBasicBlock* successor) {
    size_t predecessor_index = successor->GetPredecessorIndexOf(predecessor);
    size_t successor_index = predecessor->GetSuccessorIndexOf(successor);
    successor->predecessors_[predecessor_index] = this;
    predecessor->successors_[successor_index] = this;
    successors_.push_back(successor);
    predecessors_.push_back(predecessor);
  }

  void RemovePredecessor(HBasicBlock* block) {
    predecessors_.erase(predecessors_.begin() + GetPredecessorIndexOf(block));
  }

  void RemoveSuccessor(HBasicBlock* block) {
    successors_.erase(successors_.begin() + GetSuccessorIndexOf(block));
  }

  void ClearAllPredecessors() {
    predecessors_.clear();
  }

  void AddPredecessor(HBasicBlock* block) {
    predecessors_.push_back(block);
    block->successors_.push_back(this);
  }

  void SwapPredecessors() {
    DCHECK_EQ(predecessors_.size(), 2u);
    std::swap(predecessors_[0], predecessors_[1]);
  }

  void SwapSuccessors() {
    DCHECK_EQ(successors_.size(), 2u);
    std::swap(successors_[0], successors_[1]);
  }

  size_t GetPredecessorIndexOf(HBasicBlock* predecessor) const {
    return IndexOfElement(predecessors_, predecessor);
  }

  size_t GetSuccessorIndexOf(HBasicBlock* successor) const {
    return IndexOfElement(successors_, successor);
  }

  HBasicBlock* GetSinglePredecessor() const {
    DCHECK_EQ(GetPredecessors().size(), 1u);
    return GetPredecessors()[0];
  }

  HBasicBlock* GetSingleSuccessor() const {
    DCHECK_EQ(GetSuccessors().size(), 1u);
    return GetSuccessors()[0];
  }

  // Returns whether the first occurrence of `predecessor` in the list of
  // predecessors is at index `idx`.
  bool IsFirstIndexOfPredecessor(HBasicBlock* predecessor, size_t idx) const {
    DCHECK_EQ(GetPredecessors()[idx], predecessor);
    return GetPredecessorIndexOf(predecessor) == idx;
  }

  // Create a new block between this block and its predecessors. The new block
  // is added to the graph, all predecessor edges are relinked to it and an edge
  // is created to `this`. Returns the new empty block. Reverse post order or
  // loop and try/catch information are not updated.
  HBasicBlock* CreateImmediateDominator();

  // Split the block into two blocks just before `cursor`. Returns the newly
  // created, latter block. Note that this method will add the block to the
  // graph, create a Goto at the end of the former block and will create an edge
  // between the blocks. It will not, however, update the reverse post order or
  // loop and try/catch information.
  HBasicBlock* SplitBefore(HInstruction* cursor);

  // Split the block into two blocks just after `cursor`. Returns the newly
  // created block. Note that this method just updates raw block information,
  // like predecessors, successors, dominators, and instruction list. It does not
  // update the graph, reverse post order, loop information, nor make sure the
  // blocks are consistent (for example ending with a control flow instruction).
  HBasicBlock* SplitAfter(HInstruction* cursor);

  // Split catch block into two blocks after the original move-exception bytecode
  // instruction, or at the beginning if not present. Returns the newly created,
  // latter block, or nullptr if such block could not be created (must be dead
  // in that case). Note that this method just updates raw block information,
  // like predecessors, successors, dominators, and instruction list. It does not
  // update the graph, reverse post order, loop information, nor make sure the
  // blocks are consistent (for example ending with a control flow instruction).
  HBasicBlock* SplitCatchBlockAfterMoveException();

  // Merge `other` at the end of `this`. Successors and dominated blocks of
  // `other` are changed to be successors and dominated blocks of `this`. Note
  // that this method does not update the graph, reverse post order, loop
  // information, nor make sure the blocks are consistent (for example ending
  // with a control flow instruction).
  void MergeWithInlined(HBasicBlock* other);

  // Replace `this` with `other`. Predecessors, successors, and dominated blocks
  // of `this` are moved to `other`.
  // Note that this method does not update the graph, reverse post order, loop
  // information, nor make sure the blocks are consistent (for example ending
  // with a control flow instruction).
  void ReplaceWith(HBasicBlock* other);

  // Merge `other` at the end of `this`. This method updates loops, reverse post
  // order, links to predecessors, successors, dominators and deletes the block
  // from the graph. The two blocks must be successive, i.e. `this` the only
  // predecessor of `other` and vice versa.
  void MergeWith(HBasicBlock* other);

  // Disconnects `this` from all its predecessors, successors and dominator,
  // removes it from all loops it is included in and eventually from the graph.
  // The block must not dominate any other block. Predecessors and successors
  // are safely updated.
  void DisconnectAndDelete();

  void AddInstruction(HInstruction* instruction);
  // Insert `instruction` before/after an existing instruction `cursor`.
  void InsertInstructionBefore(HInstruction* instruction, HInstruction* cursor);
  void InsertInstructionAfter(HInstruction* instruction, HInstruction* cursor);
  // Replace instruction `initial` with `replacement` within this block.
  void ReplaceAndRemoveInstructionWith(HInstruction* initial,
                                       HInstruction* replacement);
  void AddPhi(HPhi* phi);
  void InsertPhiAfter(HPhi* instruction, HPhi* cursor);
  // RemoveInstruction and RemovePhi delete a given instruction from the respective
  // instruction list. With 'ensure_safety' set to true, it verifies that the
  // instruction is not in use and removes it from the use lists of its inputs.
  void RemoveInstruction(HInstruction* instruction, bool ensure_safety = true);
  void RemovePhi(HPhi* phi, bool ensure_safety = true);
  void RemoveInstructionOrPhi(HInstruction* instruction, bool ensure_safety = true);

  bool IsLoopHeader() const {
    return IsInLoop() && (loop_information_->GetHeader() == this);
  }

  bool IsLoopPreHeaderFirstPredecessor() const {
    DCHECK(IsLoopHeader());
    return GetPredecessors()[0] == GetLoopInformation()->GetPreHeader();
  }

  HLoopInformation* GetLoopInformation() const {
    return loop_information_;
  }

  // Set the loop_information_ on this block. Overrides the current
  // loop_information if it is an outer loop of the passed loop information.
  // Note that this method is called while creating the loop information.
  void SetInLoop(HLoopInformation* info) {
    if (IsLoopHeader()) {
      // Nothing to do. This just means `info` is an outer loop.
    } else if (!IsInLoop()) {
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

  // Raw update of the loop information.
  void SetLoopInformation(HLoopInformation* info) {
    loop_information_ = info;
  }

  bool IsInLoop() const { return loop_information_ != nullptr; }

  TryCatchInformation* GetTryCatchInformation() const { return try_catch_information_; }

  void SetTryCatchInformation(TryCatchInformation* try_catch_information) {
    try_catch_information_ = try_catch_information;
  }

  bool IsTryBlock() const {
    return try_catch_information_ != nullptr && try_catch_information_->IsTryBlock();
  }

  bool IsCatchBlock() const {
    return try_catch_information_ != nullptr && try_catch_information_->IsCatchBlock();
  }

  // Returns the try entry that this block's successors should have. They will
  // be in the same try, unless the block ends in a try boundary. In that case,
  // the appropriate try entry will be returned.
  const HTryBoundary* ComputeTryEntryOfSuccessors() const;

  bool HasThrowingInstructions() const;

  // Returns whether this block dominates the blocked passed as parameter.
  bool Dominates(HBasicBlock* block) const;

  size_t GetLifetimeStart() const { return lifetime_start_; }
  size_t GetLifetimeEnd() const { return lifetime_end_; }

  void SetLifetimeStart(size_t start) { lifetime_start_ = start; }
  void SetLifetimeEnd(size_t end) { lifetime_end_ = end; }

  bool EndsWithControlFlowInstruction() const;
  bool EndsWithIf() const;
  bool EndsWithTryBoundary() const;
  bool HasSinglePhi() const;

 private:
  HGraph* graph_;
  ArenaVector<HBasicBlock*> predecessors_;
  ArenaVector<HBasicBlock*> successors_;
  HInstructionList instructions_;
  HInstructionList phis_;
  HLoopInformation* loop_information_;
  HBasicBlock* dominator_;
  ArenaVector<HBasicBlock*> dominated_blocks_;
  uint32_t block_id_;
  // The dex program counter of the first instruction of this block.
  const uint32_t dex_pc_;
  size_t lifetime_start_;
  size_t lifetime_end_;
  TryCatchInformation* try_catch_information_;

  friend class HGraph;
  friend class HInstruction;

  DISALLOW_COPY_AND_ASSIGN(HBasicBlock);
};

// Iterates over the LoopInformation of all loops which contain 'block'
// from the innermost to the outermost.
class HLoopInformationOutwardIterator : public ValueObject {
 public:
  explicit HLoopInformationOutwardIterator(const HBasicBlock& block)
      : current_(block.GetLoopInformation()) {}

  bool Done() const { return current_ == nullptr; }

  void Advance() {
    DCHECK(!Done());
    current_ = current_->GetPreHeader()->GetLoopInformation();
  }

  HLoopInformation* Current() const {
    DCHECK(!Done());
    return current_;
  }

 private:
  HLoopInformation* current_;

  DISALLOW_COPY_AND_ASSIGN(HLoopInformationOutwardIterator);
};

#define FOR_EACH_CONCRETE_INSTRUCTION_COMMON(M)                         \
  M(Above, Condition)                                                   \
  M(AboveOrEqual, Condition)                                            \
  M(Add, BinaryOperation)                                               \
  M(And, BinaryOperation)                                               \
  M(ArrayGet, Instruction)                                              \
  M(ArrayLength, Instruction)                                           \
  M(ArraySet, Instruction)                                              \
  M(Below, Condition)                                                   \
  M(BelowOrEqual, Condition)                                            \
  M(BooleanNot, UnaryOperation)                                         \
  M(BoundsCheck, Instruction)                                           \
  M(BoundType, Instruction)                                             \
  M(CheckCast, Instruction)                                             \
  M(ClearException, Instruction)                                        \
  M(ClinitCheck, Instruction)                                           \
  M(Compare, BinaryOperation)                                           \
  M(CurrentMethod, Instruction)                                         \
  M(Deoptimize, Instruction)                                            \
  M(Div, BinaryOperation)                                               \
  M(DivZeroCheck, Instruction)                                          \
  M(DoubleConstant, Constant)                                           \
  M(Equal, Condition)                                                   \
  M(Exit, Instruction)                                                  \
  M(FakeString, Instruction)                                            \
  M(FloatConstant, Constant)                                            \
  M(Goto, Instruction)                                                  \
  M(GreaterThan, Condition)                                             \
  M(GreaterThanOrEqual, Condition)                                      \
  M(If, Instruction)                                                    \
  M(InstanceFieldGet, Instruction)                                      \
  M(InstanceFieldSet, Instruction)                                      \
  M(InstanceOf, Instruction)                                            \
  M(IntConstant, Constant)                                              \
  M(InvokeUnresolved, Invoke)                                           \
  M(InvokeInterface, Invoke)                                            \
  M(InvokeStaticOrDirect, Invoke)                                       \
  M(InvokeVirtual, Invoke)                                              \
  M(LessThan, Condition)                                                \
  M(LessThanOrEqual, Condition)                                         \
  M(LoadClass, Instruction)                                             \
  M(LoadException, Instruction)                                         \
  M(LoadLocal, Instruction)                                             \
  M(LoadString, Instruction)                                            \
  M(Local, Instruction)                                                 \
  M(LongConstant, Constant)                                             \
  M(MemoryBarrier, Instruction)                                         \
  M(MonitorOperation, Instruction)                                      \
  M(Mul, BinaryOperation)                                               \
  M(NativeDebugInfo, Instruction)                                       \
  M(Neg, UnaryOperation)                                                \
  M(NewArray, Instruction)                                              \
  M(NewInstance, Instruction)                                           \
  M(Not, UnaryOperation)                                                \
  M(NotEqual, Condition)                                                \
  M(NullConstant, Instruction)                                          \
  M(NullCheck, Instruction)                                             \
  M(Or, BinaryOperation)                                                \
  M(PackedSwitch, Instruction)                                          \
  M(ParallelMove, Instruction)                                          \
  M(ParameterValue, Instruction)                                        \
  M(Phi, Instruction)                                                   \
  M(Rem, BinaryOperation)                                               \
  M(Return, Instruction)                                                \
  M(ReturnVoid, Instruction)                                            \
  M(Ror, BinaryOperation)                                               \
  M(Shl, BinaryOperation)                                               \
  M(Shr, BinaryOperation)                                               \
  M(StaticFieldGet, Instruction)                                        \
  M(StaticFieldSet, Instruction)                                        \
  M(UnresolvedInstanceFieldGet, Instruction)                            \
  M(UnresolvedInstanceFieldSet, Instruction)                            \
  M(UnresolvedStaticFieldGet, Instruction)                              \
  M(UnresolvedStaticFieldSet, Instruction)                              \
  M(StoreLocal, Instruction)                                            \
  M(Sub, BinaryOperation)                                               \
  M(SuspendCheck, Instruction)                                          \
  M(Temporary, Instruction)                                             \
  M(Throw, Instruction)                                                 \
  M(TryBoundary, Instruction)                                           \
  M(TypeConversion, Instruction)                                        \
  M(UShr, BinaryOperation)                                              \
  M(Xor, BinaryOperation)                                               \

#ifndef ART_ENABLE_CODEGEN_arm
#define FOR_EACH_CONCRETE_INSTRUCTION_ARM(M)
#else
#define FOR_EACH_CONCRETE_INSTRUCTION_ARM(M)                            \
  M(ArmDexCacheArraysBase, Instruction)
#endif

#ifndef ART_ENABLE_CODEGEN_arm64
#define FOR_EACH_CONCRETE_INSTRUCTION_ARM64(M)
#else
#define FOR_EACH_CONCRETE_INSTRUCTION_ARM64(M)                          \
  M(Arm64DataProcWithShifterOp, Instruction)                            \
  M(Arm64IntermediateAddress, Instruction)                              \
  M(Arm64MultiplyAccumulate, Instruction)
#endif

#define FOR_EACH_CONCRETE_INSTRUCTION_MIPS(M)

#define FOR_EACH_CONCRETE_INSTRUCTION_MIPS64(M)

#ifndef ART_ENABLE_CODEGEN_x86
#define FOR_EACH_CONCRETE_INSTRUCTION_X86(M)
#else
#define FOR_EACH_CONCRETE_INSTRUCTION_X86(M)                            \
  M(X86ComputeBaseMethodAddress, Instruction)                           \
  M(X86LoadFromConstantTable, Instruction)                              \
  M(X86PackedSwitch, Instruction)
#endif

#define FOR_EACH_CONCRETE_INSTRUCTION_X86_64(M)

#define FOR_EACH_CONCRETE_INSTRUCTION(M)                                \
  FOR_EACH_CONCRETE_INSTRUCTION_COMMON(M)                               \
  FOR_EACH_CONCRETE_INSTRUCTION_ARM(M)                                  \
  FOR_EACH_CONCRETE_INSTRUCTION_ARM64(M)                                \
  FOR_EACH_CONCRETE_INSTRUCTION_MIPS(M)                                 \
  FOR_EACH_CONCRETE_INSTRUCTION_MIPS64(M)                               \
  FOR_EACH_CONCRETE_INSTRUCTION_X86(M)                                  \
  FOR_EACH_CONCRETE_INSTRUCTION_X86_64(M)

#define FOR_EACH_ABSTRACT_INSTRUCTION(M)                                \
  M(Condition, BinaryOperation)                                         \
  M(Constant, Instruction)                                              \
  M(UnaryOperation, Instruction)                                        \
  M(BinaryOperation, Instruction)                                       \
  M(Invoke, Instruction)

#define FOR_EACH_INSTRUCTION(M)                                         \
  FOR_EACH_CONCRETE_INSTRUCTION(M)                                      \
  FOR_EACH_ABSTRACT_INSTRUCTION(M)

#define FORWARD_DECLARATION(type, super) class H##type;
FOR_EACH_INSTRUCTION(FORWARD_DECLARATION)
#undef FORWARD_DECLARATION

#define DECLARE_INSTRUCTION(type)                                       \
  InstructionKind GetKindInternal() const OVERRIDE { return k##type; }  \
  const char* DebugName() const OVERRIDE { return #type; }              \
  bool InstructionTypeEquals(HInstruction* other) const OVERRIDE {      \
    return other->Is##type();                                           \
  }                                                                     \
  void Accept(HGraphVisitor* visitor) OVERRIDE

#define DECLARE_ABSTRACT_INSTRUCTION(type)                              \
  bool Is##type() const { return As##type() != nullptr; }               \
  const H##type* As##type() const { return this; }                      \
  H##type* As##type() { return this; }

template <typename T> class HUseList;

template <typename T>
class HUseListNode : public ArenaObject<kArenaAllocUseListNode> {
 public:
  HUseListNode* GetPrevious() const { return prev_; }
  HUseListNode* GetNext() const { return next_; }
  T GetUser() const { return user_; }
  size_t GetIndex() const { return index_; }
  void SetIndex(size_t index) { index_ = index; }

 private:
  HUseListNode(T user, size_t index)
      : user_(user), index_(index), prev_(nullptr), next_(nullptr) {}

  T const user_;
  size_t index_;
  HUseListNode<T>* prev_;
  HUseListNode<T>* next_;

  friend class HUseList<T>;

  DISALLOW_COPY_AND_ASSIGN(HUseListNode);
};

template <typename T>
class HUseList : public ValueObject {
 public:
  HUseList() : first_(nullptr) {}

  void Clear() {
    first_ = nullptr;
  }

  // Adds a new entry at the beginning of the use list and returns
  // the newly created node.
  HUseListNode<T>* AddUse(T user, size_t index, ArenaAllocator* arena) {
    HUseListNode<T>* new_node = new (arena) HUseListNode<T>(user, index);
    if (IsEmpty()) {
      first_ = new_node;
    } else {
      first_->prev_ = new_node;
      new_node->next_ = first_;
      first_ = new_node;
    }
    return new_node;
  }

  HUseListNode<T>* GetFirst() const {
    return first_;
  }

  void Remove(HUseListNode<T>* node) {
    DCHECK(node != nullptr);
    DCHECK(Contains(node));

    if (node->prev_ != nullptr) {
      node->prev_->next_ = node->next_;
    }
    if (node->next_ != nullptr) {
      node->next_->prev_ = node->prev_;
    }
    if (node == first_) {
      first_ = node->next_;
    }
  }

  bool Contains(const HUseListNode<T>* node) const {
    if (node == nullptr) {
      return false;
    }
    for (HUseListNode<T>* current = first_; current != nullptr; current = current->GetNext()) {
      if (current == node) {
        return true;
      }
    }
    return false;
  }

  bool IsEmpty() const {
    return first_ == nullptr;
  }

  bool HasOnlyOneUse() const {
    return first_ != nullptr && first_->next_ == nullptr;
  }

  size_t SizeSlow() const {
    size_t count = 0;
    for (HUseListNode<T>* current = first_; current != nullptr; current = current->GetNext()) {
      ++count;
    }
    return count;
  }

 private:
  HUseListNode<T>* first_;
};

template<typename T>
class HUseIterator : public ValueObject {
 public:
  explicit HUseIterator(const HUseList<T>& uses) : current_(uses.GetFirst()) {}

  bool Done() const { return current_ == nullptr; }

  void Advance() {
    DCHECK(!Done());
    current_ = current_->GetNext();
  }

  HUseListNode<T>* Current() const {
    DCHECK(!Done());
    return current_;
  }

 private:
  HUseListNode<T>* current_;

  friend class HValue;
};

// This class is used by HEnvironment and HInstruction classes to record the
// instructions they use and pointers to the corresponding HUseListNodes kept
// by the used instructions.
template <typename T>
class HUserRecord : public ValueObject {
 public:
  HUserRecord() : instruction_(nullptr), use_node_(nullptr) {}
  explicit HUserRecord(HInstruction* instruction) : instruction_(instruction), use_node_(nullptr) {}

  HUserRecord(const HUserRecord<T>& old_record, HUseListNode<T>* use_node)
    : instruction_(old_record.instruction_), use_node_(use_node) {
    DCHECK(instruction_ != nullptr);
    DCHECK(use_node_ != nullptr);
    DCHECK(old_record.use_node_ == nullptr);
  }

  HInstruction* GetInstruction() const { return instruction_; }
  HUseListNode<T>* GetUseNode() const { return use_node_; }

 private:
  // Instruction used by the user.
  HInstruction* instruction_;

  // Corresponding entry in the use list kept by 'instruction_'.
  HUseListNode<T>* use_node_;
};

/**
 * Side-effects representation.
 *
 * For write/read dependences on fields/arrays, the dependence analysis uses
 * type disambiguation (e.g. a float field write cannot modify the value of an
 * integer field read) and the access type (e.g.  a reference array write cannot
 * modify the value of a reference field read [although it may modify the
 * reference fetch prior to reading the field, which is represented by its own
 * write/read dependence]). The analysis makes conservative points-to
 * assumptions on reference types (e.g. two same typed arrays are assumed to be
 * the same, and any reference read depends on any reference read without
 * further regard of its type).
 *
 * The internal representation uses 38-bit and is described in the table below.
 * The first line indicates the side effect, and for field/array accesses the
 * second line indicates the type of the access (in the order of the
 * Primitive::Type enum).
 * The two numbered lines below indicate the bit position in the bitfield (read
 * vertically).
 *
 *   |Depends on GC|ARRAY-R  |FIELD-R  |Can trigger GC|ARRAY-W  |FIELD-W  |
 *   +-------------+---------+---------+--------------+---------+---------+
 *   |             |DFJISCBZL|DFJISCBZL|              |DFJISCBZL|DFJISCBZL|
 *   |      3      |333333322|222222221|       1      |111111110|000000000|
 *   |      7      |654321098|765432109|       8      |765432109|876543210|
 *
 * Note that, to ease the implementation, 'changes' bits are least significant
 * bits, while 'dependency' bits are most significant bits.
 */
class SideEffects : public ValueObject {
 public:
  SideEffects() : flags_(0) {}

  static SideEffects None() {
    return SideEffects(0);
  }

  static SideEffects All() {
    return SideEffects(kAllChangeBits | kAllDependOnBits);
  }

  static SideEffects AllChanges() {
    return SideEffects(kAllChangeBits);
  }

  static SideEffects AllDependencies() {
    return SideEffects(kAllDependOnBits);
  }

  static SideEffects AllExceptGCDependency() {
    return AllWritesAndReads().Union(SideEffects::CanTriggerGC());
  }

  static SideEffects AllWritesAndReads() {
    return SideEffects(kAllWrites | kAllReads);
  }

  static SideEffects AllWrites() {
    return SideEffects(kAllWrites);
  }

  static SideEffects AllReads() {
    return SideEffects(kAllReads);
  }

  static SideEffects FieldWriteOfType(Primitive::Type type, bool is_volatile) {
    return is_volatile
        ? AllWritesAndReads()
        : SideEffects(TypeFlagWithAlias(type, kFieldWriteOffset));
  }

  static SideEffects ArrayWriteOfType(Primitive::Type type) {
    return SideEffects(TypeFlagWithAlias(type, kArrayWriteOffset));
  }

  static SideEffects FieldReadOfType(Primitive::Type type, bool is_volatile) {
    return is_volatile
        ? AllWritesAndReads()
        : SideEffects(TypeFlagWithAlias(type, kFieldReadOffset));
  }

  static SideEffects ArrayReadOfType(Primitive::Type type) {
    return SideEffects(TypeFlagWithAlias(type, kArrayReadOffset));
  }

  static SideEffects CanTriggerGC() {
    return SideEffects(1ULL << kCanTriggerGCBit);
  }

  static SideEffects DependsOnGC() {
    return SideEffects(1ULL << kDependsOnGCBit);
  }

  // Combines the side-effects of this and the other.
  SideEffects Union(SideEffects other) const {
    return SideEffects(flags_ | other.flags_);
  }

  SideEffects Exclusion(SideEffects other) const {
    return SideEffects(flags_ & ~other.flags_);
  }

  void Add(SideEffects other) {
    flags_ |= other.flags_;
  }

  bool Includes(SideEffects other) const {
    return (other.flags_ & flags_) == other.flags_;
  }

  bool HasSideEffects() const {
    return (flags_ & kAllChangeBits);
  }

  bool HasDependencies() const {
    return (flags_ & kAllDependOnBits);
  }

  // Returns true if there are no side effects or dependencies.
  bool DoesNothing() const {
    return flags_ == 0;
  }

  // Returns true if something is written.
  bool DoesAnyWrite() const {
    return (flags_ & kAllWrites);
  }

  // Returns true if something is read.
  bool DoesAnyRead() const {
    return (flags_ & kAllReads);
  }

  // Returns true if potentially everything is written and read
  // (every type and every kind of access).
  bool DoesAllReadWrite() const {
    return (flags_ & (kAllWrites | kAllReads)) == (kAllWrites | kAllReads);
  }

  bool DoesAll() const {
    return flags_ == (kAllChangeBits | kAllDependOnBits);
  }

  // Returns true if `this` may read something written by `other`.
  bool MayDependOn(SideEffects other) const {
    const uint64_t depends_on_flags = (flags_ & kAllDependOnBits) >> kChangeBits;
    return (other.flags_ & depends_on_flags);
  }

  // Returns string representation of flags (for debugging only).
  // Format: |x|DFJISCBZL|DFJISCBZL|y|DFJISCBZL|DFJISCBZL|
  std::string ToString() const {
    std::string flags = "|";
    for (int s = kLastBit; s >= 0; s--) {
      bool current_bit_is_set = ((flags_ >> s) & 1) != 0;
      if ((s == kDependsOnGCBit) || (s == kCanTriggerGCBit)) {
        // This is a bit for the GC side effect.
        if (current_bit_is_set) {
          flags += "GC";
        }
        flags += "|";
      } else {
        // This is a bit for the array/field analysis.
        // The underscore character stands for the 'can trigger GC' bit.
        static const char *kDebug = "LZBCSIJFDLZBCSIJFD_LZBCSIJFDLZBCSIJFD";
        if (current_bit_is_set) {
          flags += kDebug[s];
        }
        if ((s == kFieldWriteOffset) || (s == kArrayWriteOffset) ||
            (s == kFieldReadOffset) || (s == kArrayReadOffset)) {
          flags += "|";
        }
      }
    }
    return flags;
  }

  bool Equals(const SideEffects& other) const { return flags_ == other.flags_; }

 private:
  static constexpr int kFieldArrayAnalysisBits = 9;

  static constexpr int kFieldWriteOffset = 0;
  static constexpr int kArrayWriteOffset = kFieldWriteOffset + kFieldArrayAnalysisBits;
  static constexpr int kLastBitForWrites = kArrayWriteOffset + kFieldArrayAnalysisBits - 1;
  static constexpr int kCanTriggerGCBit = kLastBitForWrites + 1;

  static constexpr int kChangeBits = kCanTriggerGCBit + 1;

  static constexpr int kFieldReadOffset = kCanTriggerGCBit + 1;
  static constexpr int kArrayReadOffset = kFieldReadOffset + kFieldArrayAnalysisBits;
  static constexpr int kLastBitForReads = kArrayReadOffset + kFieldArrayAnalysisBits - 1;
  static constexpr int kDependsOnGCBit = kLastBitForReads + 1;

  static constexpr int kLastBit = kDependsOnGCBit;
  static constexpr int kDependOnBits = kLastBit + 1 - kChangeBits;

  // Aliases.

  static_assert(kChangeBits == kDependOnBits,
                "the 'change' bits should match the 'depend on' bits.");

  static constexpr uint64_t kAllChangeBits = ((1ULL << kChangeBits) - 1);
  static constexpr uint64_t kAllDependOnBits = ((1ULL << kDependOnBits) - 1) << kChangeBits;
  static constexpr uint64_t kAllWrites =
      ((1ULL << (kLastBitForWrites + 1 - kFieldWriteOffset)) - 1) << kFieldWriteOffset;
  static constexpr uint64_t kAllReads =
      ((1ULL << (kLastBitForReads + 1 - kFieldReadOffset)) - 1) << kFieldReadOffset;

  // Work around the fact that HIR aliases I/F and J/D.
  // TODO: remove this interceptor once HIR types are clean
  static uint64_t TypeFlagWithAlias(Primitive::Type type, int offset) {
    switch (type) {
      case Primitive::kPrimInt:
      case Primitive::kPrimFloat:
        return TypeFlag(Primitive::kPrimInt, offset) |
               TypeFlag(Primitive::kPrimFloat, offset);
      case Primitive::kPrimLong:
      case Primitive::kPrimDouble:
        return TypeFlag(Primitive::kPrimLong, offset) |
               TypeFlag(Primitive::kPrimDouble, offset);
      default:
        return TypeFlag(type, offset);
    }
  }

  // Translates type to bit flag.
  static uint64_t TypeFlag(Primitive::Type type, int offset) {
    CHECK_NE(type, Primitive::kPrimVoid);
    const uint64_t one = 1;
    const int shift = type;  // 0-based consecutive enum
    DCHECK_LE(kFieldWriteOffset, shift);
    DCHECK_LT(shift, kArrayWriteOffset);
    return one << (type + offset);
  }

  // Private constructor on direct flags value.
  explicit SideEffects(uint64_t flags) : flags_(flags) {}

  uint64_t flags_;
};

// A HEnvironment object contains the values of virtual registers at a given location.
class HEnvironment : public ArenaObject<kArenaAllocEnvironment> {
 public:
  HEnvironment(ArenaAllocator* arena,
               size_t number_of_vregs,
               const DexFile& dex_file,
               uint32_t method_idx,
               uint32_t dex_pc,
               InvokeType invoke_type,
               HInstruction* holder)
     : vregs_(number_of_vregs, arena->Adapter(kArenaAllocEnvironmentVRegs)),
       locations_(number_of_vregs, arena->Adapter(kArenaAllocEnvironmentLocations)),
       parent_(nullptr),
       dex_file_(dex_file),
       method_idx_(method_idx),
       dex_pc_(dex_pc),
       invoke_type_(invoke_type),
       holder_(holder) {
  }

  HEnvironment(ArenaAllocator* arena, const HEnvironment& to_copy, HInstruction* holder)
      : HEnvironment(arena,
                     to_copy.Size(),
                     to_copy.GetDexFile(),
                     to_copy.GetMethodIdx(),
                     to_copy.GetDexPc(),
                     to_copy.GetInvokeType(),
                     holder) {}

  void SetAndCopyParentChain(ArenaAllocator* allocator, HEnvironment* parent) {
    if (parent_ != nullptr) {
      parent_->SetAndCopyParentChain(allocator, parent);
    } else {
      parent_ = new (allocator) HEnvironment(allocator, *parent, holder_);
      parent_->CopyFrom(parent);
      if (parent->GetParent() != nullptr) {
        parent_->SetAndCopyParentChain(allocator, parent->GetParent());
      }
    }
  }

  void CopyFrom(const ArenaVector<HInstruction*>& locals);
  void CopyFrom(HEnvironment* environment);

  // Copy from `env`. If it's a loop phi for `loop_header`, copy the first
  // input to the loop phi instead. This is for inserting instructions that
  // require an environment (like HDeoptimization) in the loop pre-header.
  void CopyFromWithLoopPhiAdjustment(HEnvironment* env, HBasicBlock* loop_header);

  void SetRawEnvAt(size_t index, HInstruction* instruction) {
    vregs_[index] = HUserRecord<HEnvironment*>(instruction);
  }

  HInstruction* GetInstructionAt(size_t index) const {
    return vregs_[index].GetInstruction();
  }

  void RemoveAsUserOfInput(size_t index) const;

  size_t Size() const { return vregs_.size(); }

  HEnvironment* GetParent() const { return parent_; }

  void SetLocationAt(size_t index, Location location) {
    locations_[index] = location;
  }

  Location GetLocationAt(size_t index) const {
    return locations_[index];
  }

  uint32_t GetDexPc() const {
    return dex_pc_;
  }

  uint32_t GetMethodIdx() const {
    return method_idx_;
  }

  InvokeType GetInvokeType() const {
    return invoke_type_;
  }

  const DexFile& GetDexFile() const {
    return dex_file_;
  }

  HInstruction* GetHolder() const {
    return holder_;
  }


  bool IsFromInlinedInvoke() const {
    return GetParent() != nullptr;
  }

 private:
  // Record instructions' use entries of this environment for constant-time removal.
  // It should only be called by HInstruction when a new environment use is added.
  void RecordEnvUse(HUseListNode<HEnvironment*>* env_use) {
    DCHECK(env_use->GetUser() == this);
    size_t index = env_use->GetIndex();
    vregs_[index] = HUserRecord<HEnvironment*>(vregs_[index], env_use);
  }

  ArenaVector<HUserRecord<HEnvironment*>> vregs_;
  ArenaVector<Location> locations_;
  HEnvironment* parent_;
  const DexFile& dex_file_;
  const uint32_t method_idx_;
  const uint32_t dex_pc_;
  const InvokeType invoke_type_;

  // The instruction that holds this environment.
  HInstruction* const holder_;

  friend class HInstruction;

  DISALLOW_COPY_AND_ASSIGN(HEnvironment);
};

class HInstruction : public ArenaObject<kArenaAllocInstruction> {
 public:
  HInstruction(SideEffects side_effects, uint32_t dex_pc)
      : previous_(nullptr),
        next_(nullptr),
        block_(nullptr),
        dex_pc_(dex_pc),
        id_(-1),
        ssa_index_(-1),
        environment_(nullptr),
        locations_(nullptr),
        live_interval_(nullptr),
        lifetime_position_(kNoLifetime),
        side_effects_(side_effects),
        reference_type_info_(ReferenceTypeInfo::CreateInvalid()) {}

  virtual ~HInstruction() {}

#define DECLARE_KIND(type, super) k##type,
  enum InstructionKind {
    FOR_EACH_INSTRUCTION(DECLARE_KIND)
  };
#undef DECLARE_KIND

  HInstruction* GetNext() const { return next_; }
  HInstruction* GetPrevious() const { return previous_; }

  HInstruction* GetNextDisregardingMoves() const;
  HInstruction* GetPreviousDisregardingMoves() const;

  HBasicBlock* GetBlock() const { return block_; }
  ArenaAllocator* GetArena() const { return block_->GetGraph()->GetArena(); }
  void SetBlock(HBasicBlock* block) { block_ = block; }
  bool IsInBlock() const { return block_ != nullptr; }
  bool IsInLoop() const { return block_->IsInLoop(); }
  bool IsLoopHeaderPhi() { return IsPhi() && block_->IsLoopHeader(); }

  virtual size_t InputCount() const = 0;
  HInstruction* InputAt(size_t i) const { return InputRecordAt(i).GetInstruction(); }

  virtual void Accept(HGraphVisitor* visitor) = 0;
  virtual const char* DebugName() const = 0;

  virtual Primitive::Type GetType() const { return Primitive::kPrimVoid; }
  void SetRawInputAt(size_t index, HInstruction* input) {
    SetRawInputRecordAt(index, HUserRecord<HInstruction*>(input));
  }

  virtual bool NeedsEnvironment() const { return false; }

  uint32_t GetDexPc() const { return dex_pc_; }

  virtual bool IsControlFlow() const { return false; }

  virtual bool CanThrow() const { return false; }
  bool CanThrowIntoCatchBlock() const { return CanThrow() && block_->IsTryBlock(); }

  bool HasSideEffects() const { return side_effects_.HasSideEffects(); }
  bool DoesAnyWrite() const { return side_effects_.DoesAnyWrite(); }

  // Does not apply for all instructions, but having this at top level greatly
  // simplifies the null check elimination.
  // TODO: Consider merging can_be_null into ReferenceTypeInfo.
  virtual bool CanBeNull() const {
    DCHECK_EQ(GetType(), Primitive::kPrimNot) << "CanBeNull only applies to reference types";
    return true;
  }

  virtual bool CanDoImplicitNullCheckOn(HInstruction* obj ATTRIBUTE_UNUSED) const {
    return false;
  }

  void SetReferenceTypeInfo(ReferenceTypeInfo rti);

  ReferenceTypeInfo GetReferenceTypeInfo() const {
    DCHECK_EQ(GetType(), Primitive::kPrimNot);
    return reference_type_info_;
  }

  void AddUseAt(HInstruction* user, size_t index) {
    DCHECK(user != nullptr);
    HUseListNode<HInstruction*>* use =
        uses_.AddUse(user, index, GetBlock()->GetGraph()->GetArena());
    user->SetRawInputRecordAt(index, HUserRecord<HInstruction*>(user->InputRecordAt(index), use));
  }

  void AddEnvUseAt(HEnvironment* user, size_t index) {
    DCHECK(user != nullptr);
    HUseListNode<HEnvironment*>* env_use =
        env_uses_.AddUse(user, index, GetBlock()->GetGraph()->GetArena());
    user->RecordEnvUse(env_use);
  }

  void RemoveAsUserOfInput(size_t input) {
    HUserRecord<HInstruction*> input_use = InputRecordAt(input);
    input_use.GetInstruction()->uses_.Remove(input_use.GetUseNode());
  }

  const HUseList<HInstruction*>& GetUses() const { return uses_; }
  const HUseList<HEnvironment*>& GetEnvUses() const { return env_uses_; }

  bool HasUses() const { return !uses_.IsEmpty() || !env_uses_.IsEmpty(); }
  bool HasEnvironmentUses() const { return !env_uses_.IsEmpty(); }
  bool HasNonEnvironmentUses() const { return !uses_.IsEmpty(); }
  bool HasOnlyOneNonEnvironmentUse() const {
    return !HasEnvironmentUses() && GetUses().HasOnlyOneUse();
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
  // Set the `environment_` field. Raw because this method does not
  // update the uses lists.
  void SetRawEnvironment(HEnvironment* environment) {
    DCHECK(environment_ == nullptr);
    DCHECK_EQ(environment->GetHolder(), this);
    environment_ = environment;
  }

  // Set the environment of this instruction, copying it from `environment`. While
  // copying, the uses lists are being updated.
  void CopyEnvironmentFrom(HEnvironment* environment) {
    DCHECK(environment_ == nullptr);
    ArenaAllocator* allocator = GetBlock()->GetGraph()->GetArena();
    environment_ = new (allocator) HEnvironment(allocator, *environment, this);
    environment_->CopyFrom(environment);
    if (environment->GetParent() != nullptr) {
      environment_->SetAndCopyParentChain(allocator, environment->GetParent());
    }
  }

  void CopyEnvironmentFromWithLoopPhiAdjustment(HEnvironment* environment,
                                                HBasicBlock* block) {
    DCHECK(environment_ == nullptr);
    ArenaAllocator* allocator = GetBlock()->GetGraph()->GetArena();
    environment_ = new (allocator) HEnvironment(allocator, *environment, this);
    environment_->CopyFromWithLoopPhiAdjustment(environment, block);
    if (environment->GetParent() != nullptr) {
      environment_->SetAndCopyParentChain(allocator, environment->GetParent());
    }
  }

  // Returns the number of entries in the environment. Typically, that is the
  // number of dex registers in a method. It could be more in case of inlining.
  size_t EnvironmentSize() const;

  LocationSummary* GetLocations() const { return locations_; }
  void SetLocations(LocationSummary* locations) { locations_ = locations; }

  void ReplaceWith(HInstruction* instruction);
  void ReplaceInput(HInstruction* replacement, size_t index);

  // This is almost the same as doing `ReplaceWith()`. But in this helper, the
  // uses of this instruction by `other` are *not* updated.
  void ReplaceWithExceptInReplacementAtIndex(HInstruction* other, size_t use_index) {
    ReplaceWith(other);
    other->ReplaceInput(this, use_index);
  }

  // Move `this` instruction before `cursor`.
  void MoveBefore(HInstruction* cursor);

  // Move `this` before its first user and out of any loops. If there is no
  // out-of-loop user that dominates all other users, move the instruction
  // to the end of the out-of-loop common dominator of the user's blocks.
  //
  // This can be used only on non-throwing instructions with no side effects that
  // have at least one use but no environment uses.
  void MoveBeforeFirstUserAndOutOfLoops();

#define INSTRUCTION_TYPE_CHECK(type, super)                                    \
  bool Is##type() const;                                                       \
  const H##type* As##type() const;                                             \
  H##type* As##type();

  FOR_EACH_CONCRETE_INSTRUCTION(INSTRUCTION_TYPE_CHECK)
#undef INSTRUCTION_TYPE_CHECK

#define INSTRUCTION_TYPE_CHECK(type, super)                                    \
  bool Is##type() const { return (As##type() != nullptr); }                    \
  virtual const H##type* As##type() const { return nullptr; }                  \
  virtual H##type* As##type() { return nullptr; }
  FOR_EACH_ABSTRACT_INSTRUCTION(INSTRUCTION_TYPE_CHECK)
#undef INSTRUCTION_TYPE_CHECK

  // Returns whether the instruction can be moved within the graph.
  virtual bool CanBeMoved() const { return false; }

  // Returns whether the two instructions are of the same kind.
  virtual bool InstructionTypeEquals(HInstruction* other ATTRIBUTE_UNUSED) const {
    return false;
  }

  // Returns whether any data encoded in the two instructions is equal.
  // This method does not look at the inputs. Both instructions must be
  // of the same type, otherwise the method has undefined behavior.
  virtual bool InstructionDataEquals(HInstruction* other ATTRIBUTE_UNUSED) const {
    return false;
  }

  // Returns whether two instructions are equal, that is:
  // 1) They have the same type and contain the same data (InstructionDataEquals).
  // 2) Their inputs are identical.
  bool Equals(HInstruction* other) const;

  // TODO: Remove this indirection when the [[pure]] attribute proposal (n3744)
  // is adopted and implemented by our C++ compiler(s). Fow now, we need to hide
  // the virtual function because the __attribute__((__pure__)) doesn't really
  // apply the strong requirement for virtual functions, preventing optimizations.
  InstructionKind GetKind() const PURE;
  virtual InstructionKind GetKindInternal() const = 0;

  virtual size_t ComputeHashCode() const {
    size_t result = GetKind();
    for (size_t i = 0, e = InputCount(); i < e; ++i) {
      result = (result * 31) + InputAt(i)->GetId();
    }
    return result;
  }

  SideEffects GetSideEffects() const { return side_effects_; }
  void AddSideEffects(SideEffects other) { side_effects_.Add(other); }

  size_t GetLifetimePosition() const { return lifetime_position_; }
  void SetLifetimePosition(size_t position) { lifetime_position_ = position; }
  LiveInterval* GetLiveInterval() const { return live_interval_; }
  void SetLiveInterval(LiveInterval* interval) { live_interval_ = interval; }
  bool HasLiveInterval() const { return live_interval_ != nullptr; }

  bool IsSuspendCheckEntry() const { return IsSuspendCheck() && GetBlock()->IsEntryBlock(); }

  // Returns whether the code generation of the instruction will require to have access
  // to the current method. Such instructions are:
  // (1): Instructions that require an environment, as calling the runtime requires
  //      to walk the stack and have the current method stored at a specific stack address.
  // (2): Object literals like classes and strings, that are loaded from the dex cache
  //      fields of the current method.
  bool NeedsCurrentMethod() const {
    return NeedsEnvironment() || IsLoadClass() || IsLoadString();
  }

  // Returns whether the code generation of the instruction will require to have access
  // to the dex cache of the current method's declaring class via the current method.
  virtual bool NeedsDexCacheOfDeclaringClass() const { return false; }

  // Does this instruction have any use in an environment before
  // control flow hits 'other'?
  bool HasAnyEnvironmentUseBefore(HInstruction* other);

  // Remove all references to environment uses of this instruction.
  // The caller must ensure that this is safe to do.
  void RemoveEnvironmentUsers();

 protected:
  virtual const HUserRecord<HInstruction*> InputRecordAt(size_t i) const = 0;
  virtual void SetRawInputRecordAt(size_t index, const HUserRecord<HInstruction*>& input) = 0;
  void SetSideEffects(SideEffects other) { side_effects_ = other; }

 private:
  void RemoveEnvironmentUser(HUseListNode<HEnvironment*>* use_node) { env_uses_.Remove(use_node); }

  HInstruction* previous_;
  HInstruction* next_;
  HBasicBlock* block_;
  const uint32_t dex_pc_;

  // An instruction gets an id when it is added to the graph.
  // It reflects creation order. A negative id means the instruction
  // has not been added to the graph.
  int id_;

  // When doing liveness analysis, instructions that have uses get an SSA index.
  int ssa_index_;

  // List of instructions that have this instruction as input.
  HUseList<HInstruction*> uses_;

  // List of environments that contain this instruction.
  HUseList<HEnvironment*> env_uses_;

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

  SideEffects side_effects_;

  // TODO: for primitive types this should be marked as invalid.
  ReferenceTypeInfo reference_type_info_;

  friend class GraphChecker;
  friend class HBasicBlock;
  friend class HEnvironment;
  friend class HGraph;
  friend class HInstructionList;

  DISALLOW_COPY_AND_ASSIGN(HInstruction);
};
std::ostream& operator<<(std::ostream& os, const HInstruction::InstructionKind& rhs);

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

template<size_t N>
class HTemplateInstruction: public HInstruction {
 public:
  HTemplateInstruction<N>(SideEffects side_effects, uint32_t dex_pc)
      : HInstruction(side_effects, dex_pc), inputs_() {}
  virtual ~HTemplateInstruction() {}

  size_t InputCount() const OVERRIDE { return N; }

 protected:
  const HUserRecord<HInstruction*> InputRecordAt(size_t i) const OVERRIDE {
    DCHECK_LT(i, N);
    return inputs_[i];
  }

  void SetRawInputRecordAt(size_t i, const HUserRecord<HInstruction*>& input) OVERRIDE {
    DCHECK_LT(i, N);
    inputs_[i] = input;
  }

 private:
  std::array<HUserRecord<HInstruction*>, N> inputs_;

  friend class SsaBuilder;
};

// HTemplateInstruction specialization for N=0.
template<>
class HTemplateInstruction<0>: public HInstruction {
 public:
  explicit HTemplateInstruction<0>(SideEffects side_effects, uint32_t dex_pc)
      : HInstruction(side_effects, dex_pc) {}

  virtual ~HTemplateInstruction() {}

  size_t InputCount() const OVERRIDE { return 0; }

 protected:
  const HUserRecord<HInstruction*> InputRecordAt(size_t i ATTRIBUTE_UNUSED) const OVERRIDE {
    LOG(FATAL) << "Unreachable";
    UNREACHABLE();
  }

  void SetRawInputRecordAt(size_t i ATTRIBUTE_UNUSED,
                           const HUserRecord<HInstruction*>& input ATTRIBUTE_UNUSED) OVERRIDE {
    LOG(FATAL) << "Unreachable";
    UNREACHABLE();
  }

 private:
  friend class SsaBuilder;
};

template<intptr_t N>
class HExpression : public HTemplateInstruction<N> {
 public:
  HExpression<N>(Primitive::Type type, SideEffects side_effects, uint32_t dex_pc)
      : HTemplateInstruction<N>(side_effects, dex_pc), type_(type) {}
  virtual ~HExpression() {}

  Primitive::Type GetType() const OVERRIDE { return type_; }

 protected:
  Primitive::Type type_;
};

// Represents dex's RETURN_VOID opcode. A HReturnVoid is a control flow
// instruction that branches to the exit block.
class HReturnVoid : public HTemplateInstruction<0> {
 public:
  explicit HReturnVoid(uint32_t dex_pc = kNoDexPc)
      : HTemplateInstruction(SideEffects::None(), dex_pc) {}

  bool IsControlFlow() const OVERRIDE { return true; }

  DECLARE_INSTRUCTION(ReturnVoid);

 private:
  DISALLOW_COPY_AND_ASSIGN(HReturnVoid);
};

// Represents dex's RETURN opcodes. A HReturn is a control flow
// instruction that branches to the exit block.
class HReturn : public HTemplateInstruction<1> {
 public:
  explicit HReturn(HInstruction* value, uint32_t dex_pc = kNoDexPc)
      : HTemplateInstruction(SideEffects::None(), dex_pc) {
    SetRawInputAt(0, value);
  }

  bool IsControlFlow() const OVERRIDE { return true; }

  DECLARE_INSTRUCTION(Return);

 private:
  DISALLOW_COPY_AND_ASSIGN(HReturn);
};

// The exit instruction is the only instruction of the exit block.
// Instructions aborting the method (HThrow and HReturn) must branch to the
// exit block.
class HExit : public HTemplateInstruction<0> {
 public:
  explicit HExit(uint32_t dex_pc = kNoDexPc) : HTemplateInstruction(SideEffects::None(), dex_pc) {}

  bool IsControlFlow() const OVERRIDE { return true; }

  DECLARE_INSTRUCTION(Exit);

 private:
  DISALLOW_COPY_AND_ASSIGN(HExit);
};

// Jumps from one block to another.
class HGoto : public HTemplateInstruction<0> {
 public:
  explicit HGoto(uint32_t dex_pc = kNoDexPc) : HTemplateInstruction(SideEffects::None(), dex_pc) {}

  bool IsControlFlow() const OVERRIDE { return true; }

  HBasicBlock* GetSuccessor() const {
    return GetBlock()->GetSingleSuccessor();
  }

  DECLARE_INSTRUCTION(Goto);

 private:
  DISALLOW_COPY_AND_ASSIGN(HGoto);
};

class HConstant : public HExpression<0> {
 public:
  explicit HConstant(Primitive::Type type, uint32_t dex_pc = kNoDexPc)
      : HExpression(type, SideEffects::None(), dex_pc) {}

  bool CanBeMoved() const OVERRIDE { return true; }

  virtual bool IsMinusOne() const { return false; }
  virtual bool IsZero() const { return false; }
  virtual bool IsOne() const { return false; }

  virtual uint64_t GetValueAsUint64() const = 0;

  DECLARE_ABSTRACT_INSTRUCTION(Constant);

 private:
  DISALLOW_COPY_AND_ASSIGN(HConstant);
};

class HNullConstant : public HConstant {
 public:
  bool InstructionDataEquals(HInstruction* other ATTRIBUTE_UNUSED) const OVERRIDE {
    return true;
  }

  uint64_t GetValueAsUint64() const OVERRIDE { return 0; }

  size_t ComputeHashCode() const OVERRIDE { return 0; }

  DECLARE_INSTRUCTION(NullConstant);

 private:
  explicit HNullConstant(uint32_t dex_pc = kNoDexPc) : HConstant(Primitive::kPrimNot, dex_pc) {}

  friend class HGraph;
  DISALLOW_COPY_AND_ASSIGN(HNullConstant);
};

// Constants of the type int. Those can be from Dex instructions, or
// synthesized (for example with the if-eqz instruction).
class HIntConstant : public HConstant {
 public:
  int32_t GetValue() const { return value_; }

  uint64_t GetValueAsUint64() const OVERRIDE {
    return static_cast<uint64_t>(static_cast<uint32_t>(value_));
  }

  bool InstructionDataEquals(HInstruction* other) const OVERRIDE {
    DCHECK(other->IsIntConstant());
    return other->AsIntConstant()->value_ == value_;
  }

  size_t ComputeHashCode() const OVERRIDE { return GetValue(); }

  bool IsMinusOne() const OVERRIDE { return GetValue() == -1; }
  bool IsZero() const OVERRIDE { return GetValue() == 0; }
  bool IsOne() const OVERRIDE { return GetValue() == 1; }

  DECLARE_INSTRUCTION(IntConstant);

 private:
  explicit HIntConstant(int32_t value, uint32_t dex_pc = kNoDexPc)
      : HConstant(Primitive::kPrimInt, dex_pc), value_(value) {}
  explicit HIntConstant(bool value, uint32_t dex_pc = kNoDexPc)
      : HConstant(Primitive::kPrimInt, dex_pc), value_(value ? 1 : 0) {}

  const int32_t value_;

  friend class HGraph;
  ART_FRIEND_TEST(GraphTest, InsertInstructionBefore);
  ART_FRIEND_TYPED_TEST(ParallelMoveTest, ConstantLast);
  DISALLOW_COPY_AND_ASSIGN(HIntConstant);
};

class HLongConstant : public HConstant {
 public:
  int64_t GetValue() const { return value_; }

  uint64_t GetValueAsUint64() const OVERRIDE { return value_; }

  bool InstructionDataEquals(HInstruction* other) const OVERRIDE {
    DCHECK(other->IsLongConstant());
    return other->AsLongConstant()->value_ == value_;
  }

  size_t ComputeHashCode() const OVERRIDE { return static_cast<size_t>(GetValue()); }

  bool IsMinusOne() const OVERRIDE { return GetValue() == -1; }
  bool IsZero() const OVERRIDE { return GetValue() == 0; }
  bool IsOne() const OVERRIDE { return GetValue() == 1; }

  DECLARE_INSTRUCTION(LongConstant);

 private:
  explicit HLongConstant(int64_t value, uint32_t dex_pc = kNoDexPc)
      : HConstant(Primitive::kPrimLong, dex_pc), value_(value) {}

  const int64_t value_;

  friend class HGraph;
  DISALLOW_COPY_AND_ASSIGN(HLongConstant);
};

// Conditional branch. A block ending with an HIf instruction must have
// two successors.
class HIf : public HTemplateInstruction<1> {
 public:
  explicit HIf(HInstruction* input, uint32_t dex_pc = kNoDexPc)
      : HTemplateInstruction(SideEffects::None(), dex_pc) {
    SetRawInputAt(0, input);
  }

  bool IsControlFlow() const OVERRIDE { return true; }

  HBasicBlock* IfTrueSuccessor() const {
    return GetBlock()->GetSuccessors()[0];
  }

  HBasicBlock* IfFalseSuccessor() const {
    return GetBlock()->GetSuccessors()[1];
  }

  DECLARE_INSTRUCTION(If);

 private:
  DISALLOW_COPY_AND_ASSIGN(HIf);
};


// Abstract instruction which marks the beginning and/or end of a try block and
// links it to the respective exception handlers. Behaves the same as a Goto in
// non-exceptional control flow.
// Normal-flow successor is stored at index zero, exception handlers under
// higher indices in no particular order.
class HTryBoundary : public HTemplateInstruction<0> {
 public:
  enum BoundaryKind {
    kEntry,
    kExit,
  };

  explicit HTryBoundary(BoundaryKind kind, uint32_t dex_pc = kNoDexPc)
      : HTemplateInstruction(SideEffects::None(), dex_pc), kind_(kind) {}

  bool IsControlFlow() const OVERRIDE { return true; }

  // Returns the block's non-exceptional successor (index zero).
  HBasicBlock* GetNormalFlowSuccessor() const { return GetBlock()->GetSuccessors()[0]; }

  ArrayRef<HBasicBlock* const> GetExceptionHandlers() const {
    return ArrayRef<HBasicBlock* const>(GetBlock()->GetSuccessors()).SubArray(1u);
  }

  // Returns whether `handler` is among its exception handlers (non-zero index
  // successors).
  bool HasExceptionHandler(const HBasicBlock& handler) const {
    DCHECK(handler.IsCatchBlock());
    return GetBlock()->HasSuccessor(&handler, 1u /* Skip first successor. */);
  }

  // If not present already, adds `handler` to its block's list of exception
  // handlers.
  void AddExceptionHandler(HBasicBlock* handler) {
    if (!HasExceptionHandler(*handler)) {
      GetBlock()->AddSuccessor(handler);
    }
  }

  bool IsEntry() const { return kind_ == BoundaryKind::kEntry; }

  bool HasSameExceptionHandlersAs(const HTryBoundary& other) const;

  DECLARE_INSTRUCTION(TryBoundary);

 private:
  const BoundaryKind kind_;

  DISALLOW_COPY_AND_ASSIGN(HTryBoundary);
};

// Deoptimize to interpreter, upon checking a condition.
class HDeoptimize : public HTemplateInstruction<1> {
 public:
  HDeoptimize(HInstruction* cond, uint32_t dex_pc)
      : HTemplateInstruction(SideEffects::None(), dex_pc) {
    SetRawInputAt(0, cond);
  }

  bool CanBeMoved() const OVERRIDE { return true; }
  bool InstructionDataEquals(HInstruction* other ATTRIBUTE_UNUSED) const OVERRIDE {
    return true;
  }
  bool NeedsEnvironment() const OVERRIDE { return true; }
  bool CanThrow() const OVERRIDE { return true; }

  DECLARE_INSTRUCTION(Deoptimize);

 private:
  DISALLOW_COPY_AND_ASSIGN(HDeoptimize);
};

// Represents the ArtMethod that was passed as a first argument to
// the method. It is used by instructions that depend on it, like
// instructions that work with the dex cache.
class HCurrentMethod : public HExpression<0> {
 public:
  explicit HCurrentMethod(Primitive::Type type, uint32_t dex_pc = kNoDexPc)
      : HExpression(type, SideEffects::None(), dex_pc) {}

  DECLARE_INSTRUCTION(CurrentMethod);

 private:
  DISALLOW_COPY_AND_ASSIGN(HCurrentMethod);
};

// PackedSwitch (jump table). A block ending with a PackedSwitch instruction will
// have one successor for each entry in the switch table, and the final successor
// will be the block containing the next Dex opcode.
class HPackedSwitch : public HTemplateInstruction<1> {
 public:
  HPackedSwitch(int32_t start_value,
                uint32_t num_entries,
                HInstruction* input,
                uint32_t dex_pc = kNoDexPc)
    : HTemplateInstruction(SideEffects::None(), dex_pc),
      start_value_(start_value),
      num_entries_(num_entries) {
    SetRawInputAt(0, input);
  }

  bool IsControlFlow() const OVERRIDE { return true; }

  int32_t GetStartValue() const { return start_value_; }

  uint32_t GetNumEntries() const { return num_entries_; }

  HBasicBlock* GetDefaultBlock() const {
    // Last entry is the default block.
    return GetBlock()->GetSuccessors()[num_entries_];
  }
  DECLARE_INSTRUCTION(PackedSwitch);

 private:
  const int32_t start_value_;
  const uint32_t num_entries_;

  DISALLOW_COPY_AND_ASSIGN(HPackedSwitch);
};

class HUnaryOperation : public HExpression<1> {
 public:
  HUnaryOperation(Primitive::Type result_type, HInstruction* input, uint32_t dex_pc = kNoDexPc)
      : HExpression(result_type, SideEffects::None(), dex_pc) {
    SetRawInputAt(0, input);
  }

  HInstruction* GetInput() const { return InputAt(0); }
  Primitive::Type GetResultType() const { return GetType(); }

  bool CanBeMoved() const OVERRIDE { return true; }
  bool InstructionDataEquals(HInstruction* other ATTRIBUTE_UNUSED) const OVERRIDE {
    return true;
  }

  // Try to statically evaluate `operation` and return a HConstant
  // containing the result of this evaluation.  If `operation` cannot
  // be evaluated as a constant, return null.
  HConstant* TryStaticEvaluation() const;

  // Apply this operation to `x`.
  virtual HConstant* Evaluate(HIntConstant* x) const = 0;
  virtual HConstant* Evaluate(HLongConstant* x) const = 0;

  DECLARE_ABSTRACT_INSTRUCTION(UnaryOperation);

 private:
  DISALLOW_COPY_AND_ASSIGN(HUnaryOperation);
};

class HBinaryOperation : public HExpression<2> {
 public:
  HBinaryOperation(Primitive::Type result_type,
                   HInstruction* left,
                   HInstruction* right,
                   SideEffects side_effects = SideEffects::None(),
                   uint32_t dex_pc = kNoDexPc)
      : HExpression(result_type, side_effects, dex_pc) {
    SetRawInputAt(0, left);
    SetRawInputAt(1, right);
  }

  HInstruction* GetLeft() const { return InputAt(0); }
  HInstruction* GetRight() const { return InputAt(1); }
  Primitive::Type GetResultType() const { return GetType(); }

  virtual bool IsCommutative() const { return false; }

  // Put constant on the right.
  // Returns whether order is changed.
  bool OrderInputsWithConstantOnTheRight() {
    HInstruction* left = InputAt(0);
    HInstruction* right = InputAt(1);
    if (left->IsConstant() && !right->IsConstant()) {
      ReplaceInput(right, 0);
      ReplaceInput(left, 1);
      return true;
    }
    return false;
  }

  // Order inputs by instruction id, but favor constant on the right side.
  // This helps GVN for commutative ops.
  void OrderInputs() {
    DCHECK(IsCommutative());
    HInstruction* left = InputAt(0);
    HInstruction* right = InputAt(1);
    if (left == right || (!left->IsConstant() && right->IsConstant())) {
      return;
    }
    if (OrderInputsWithConstantOnTheRight()) {
      return;
    }
    // Order according to instruction id.
    if (left->GetId() > right->GetId()) {
      ReplaceInput(right, 0);
      ReplaceInput(left, 1);
    }
  }

  bool CanBeMoved() const OVERRIDE { return true; }
  bool InstructionDataEquals(HInstruction* other ATTRIBUTE_UNUSED) const OVERRIDE {
    return true;
  }

  // Try to statically evaluate `operation` and return a HConstant
  // containing the result of this evaluation.  If `operation` cannot
  // be evaluated as a constant, return null.
  HConstant* TryStaticEvaluation() const;

  // Apply this operation to `x` and `y`.
  virtual HConstant* Evaluate(HIntConstant* x, HIntConstant* y) const = 0;
  virtual HConstant* Evaluate(HLongConstant* x, HLongConstant* y) const = 0;
  virtual HConstant* Evaluate(HIntConstant* x ATTRIBUTE_UNUSED,
                              HLongConstant* y ATTRIBUTE_UNUSED) const {
    VLOG(compiler) << DebugName() << " is not defined for the (int, long) case.";
    return nullptr;
  }
  virtual HConstant* Evaluate(HLongConstant* x ATTRIBUTE_UNUSED,
                              HIntConstant* y ATTRIBUTE_UNUSED) const {
    VLOG(compiler) << DebugName() << " is not defined for the (long, int) case.";
    return nullptr;
  }
  virtual HConstant* Evaluate(HNullConstant* x ATTRIBUTE_UNUSED,
                              HNullConstant* y ATTRIBUTE_UNUSED) const {
    VLOG(compiler) << DebugName() << " is not defined for the (null, null) case.";
    return nullptr;
  }

  // Returns an input that can legally be used as the right input and is
  // constant, or null.
  HConstant* GetConstantRight() const;

  // If `GetConstantRight()` returns one of the input, this returns the other
  // one. Otherwise it returns null.
  HInstruction* GetLeastConstantLeft() const;

  DECLARE_ABSTRACT_INSTRUCTION(BinaryOperation);

 private:
  DISALLOW_COPY_AND_ASSIGN(HBinaryOperation);
};

// The comparison bias applies for floating point operations and indicates how NaN
// comparisons are treated:
enum class ComparisonBias {
  kNoBias,  // bias is not applicable (i.e. for long operation)
  kGtBias,  // return 1 for NaN comparisons
  kLtBias,  // return -1 for NaN comparisons
};

class HCondition : public HBinaryOperation {
 public:
  HCondition(HInstruction* first, HInstruction* second, uint32_t dex_pc = kNoDexPc)
      : HBinaryOperation(Primitive::kPrimBoolean, first, second, SideEffects::None(), dex_pc),
        needs_materialization_(true),
        bias_(ComparisonBias::kNoBias) {}

  bool NeedsMaterialization() const { return needs_materialization_; }
  void ClearNeedsMaterialization() { needs_materialization_ = false; }

  // For code generation purposes, returns whether this instruction is just before
  // `instruction`, and disregard moves in between.
  bool IsBeforeWhenDisregardMoves(HInstruction* instruction) const;

  DECLARE_ABSTRACT_INSTRUCTION(Condition);

  virtual IfCondition GetCondition() const = 0;

  virtual IfCondition GetOppositeCondition() const = 0;

  bool IsGtBias() const { return bias_ == ComparisonBias::kGtBias; }

  void SetBias(ComparisonBias bias) { bias_ = bias; }

  bool InstructionDataEquals(HInstruction* other) const OVERRIDE {
    return bias_ == other->AsCondition()->bias_;
  }

  bool IsFPConditionTrueIfNaN() const {
    DCHECK(Primitive::IsFloatingPointType(InputAt(0)->GetType()));
    IfCondition if_cond = GetCondition();
    return IsGtBias() ? ((if_cond == kCondGT) || (if_cond == kCondGE)) : (if_cond == kCondNE);
  }

  bool IsFPConditionFalseIfNaN() const {
    DCHECK(Primitive::IsFloatingPointType(InputAt(0)->GetType()));
    IfCondition if_cond = GetCondition();
    return IsGtBias() ? ((if_cond == kCondLT) || (if_cond == kCondLE)) : (if_cond == kCondEQ);
  }

 private:
  // For register allocation purposes, returns whether this instruction needs to be
  // materialized (that is, not just be in the processor flags).
  bool needs_materialization_;

  // Needed if we merge a HCompare into a HCondition.
  ComparisonBias bias_;

  DISALLOW_COPY_AND_ASSIGN(HCondition);
};

// Instruction to check if two inputs are equal to each other.
class HEqual : public HCondition {
 public:
  HEqual(HInstruction* first, HInstruction* second, uint32_t dex_pc = kNoDexPc)
      : HCondition(first, second, dex_pc) {}

  bool IsCommutative() const OVERRIDE { return true; }

  HConstant* Evaluate(HIntConstant* x, HIntConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetIntConstant(
        Compute(x->GetValue(), y->GetValue()), GetDexPc());
  }
  HConstant* Evaluate(HLongConstant* x, HLongConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetIntConstant(
        Compute(x->GetValue(), y->GetValue()), GetDexPc());
  }
  HConstant* Evaluate(HNullConstant* x ATTRIBUTE_UNUSED,
                      HNullConstant* y ATTRIBUTE_UNUSED) const OVERRIDE {
    return GetBlock()->GetGraph()->GetIntConstant(1);
  }

  DECLARE_INSTRUCTION(Equal);

  IfCondition GetCondition() const OVERRIDE {
    return kCondEQ;
  }

  IfCondition GetOppositeCondition() const OVERRIDE {
    return kCondNE;
  }

 private:
  template <typename T> bool Compute(T x, T y) const { return x == y; }

  DISALLOW_COPY_AND_ASSIGN(HEqual);
};

class HNotEqual : public HCondition {
 public:
  HNotEqual(HInstruction* first, HInstruction* second, uint32_t dex_pc = kNoDexPc)
      : HCondition(first, second, dex_pc) {}

  bool IsCommutative() const OVERRIDE { return true; }

  HConstant* Evaluate(HIntConstant* x, HIntConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetIntConstant(
        Compute(x->GetValue(), y->GetValue()), GetDexPc());
  }
  HConstant* Evaluate(HLongConstant* x, HLongConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetIntConstant(
        Compute(x->GetValue(), y->GetValue()), GetDexPc());
  }
  HConstant* Evaluate(HNullConstant* x ATTRIBUTE_UNUSED,
                      HNullConstant* y ATTRIBUTE_UNUSED) const OVERRIDE {
    return GetBlock()->GetGraph()->GetIntConstant(0);
  }

  DECLARE_INSTRUCTION(NotEqual);

  IfCondition GetCondition() const OVERRIDE {
    return kCondNE;
  }

  IfCondition GetOppositeCondition() const OVERRIDE {
    return kCondEQ;
  }

 private:
  template <typename T> bool Compute(T x, T y) const { return x != y; }

  DISALLOW_COPY_AND_ASSIGN(HNotEqual);
};

class HLessThan : public HCondition {
 public:
  HLessThan(HInstruction* first, HInstruction* second, uint32_t dex_pc = kNoDexPc)
      : HCondition(first, second, dex_pc) {}

  HConstant* Evaluate(HIntConstant* x, HIntConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetIntConstant(
        Compute(x->GetValue(), y->GetValue()), GetDexPc());
  }
  HConstant* Evaluate(HLongConstant* x, HLongConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetIntConstant(
        Compute(x->GetValue(), y->GetValue()), GetDexPc());
  }

  DECLARE_INSTRUCTION(LessThan);

  IfCondition GetCondition() const OVERRIDE {
    return kCondLT;
  }

  IfCondition GetOppositeCondition() const OVERRIDE {
    return kCondGE;
  }

 private:
  template <typename T> bool Compute(T x, T y) const { return x < y; }

  DISALLOW_COPY_AND_ASSIGN(HLessThan);
};

class HLessThanOrEqual : public HCondition {
 public:
  HLessThanOrEqual(HInstruction* first, HInstruction* second, uint32_t dex_pc = kNoDexPc)
      : HCondition(first, second, dex_pc) {}

  HConstant* Evaluate(HIntConstant* x, HIntConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetIntConstant(
        Compute(x->GetValue(), y->GetValue()), GetDexPc());
  }
  HConstant* Evaluate(HLongConstant* x, HLongConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetIntConstant(
        Compute(x->GetValue(), y->GetValue()), GetDexPc());
  }

  DECLARE_INSTRUCTION(LessThanOrEqual);

  IfCondition GetCondition() const OVERRIDE {
    return kCondLE;
  }

  IfCondition GetOppositeCondition() const OVERRIDE {
    return kCondGT;
  }

 private:
  template <typename T> bool Compute(T x, T y) const { return x <= y; }

  DISALLOW_COPY_AND_ASSIGN(HLessThanOrEqual);
};

class HGreaterThan : public HCondition {
 public:
  HGreaterThan(HInstruction* first, HInstruction* second, uint32_t dex_pc = kNoDexPc)
      : HCondition(first, second, dex_pc) {}

  HConstant* Evaluate(HIntConstant* x, HIntConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetIntConstant(
        Compute(x->GetValue(), y->GetValue()), GetDexPc());
  }
  HConstant* Evaluate(HLongConstant* x, HLongConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetIntConstant(
        Compute(x->GetValue(), y->GetValue()), GetDexPc());
  }

  DECLARE_INSTRUCTION(GreaterThan);

  IfCondition GetCondition() const OVERRIDE {
    return kCondGT;
  }

  IfCondition GetOppositeCondition() const OVERRIDE {
    return kCondLE;
  }

 private:
  template <typename T> bool Compute(T x, T y) const { return x > y; }

  DISALLOW_COPY_AND_ASSIGN(HGreaterThan);
};

class HGreaterThanOrEqual : public HCondition {
 public:
  HGreaterThanOrEqual(HInstruction* first, HInstruction* second, uint32_t dex_pc = kNoDexPc)
      : HCondition(first, second, dex_pc) {}

  HConstant* Evaluate(HIntConstant* x, HIntConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetIntConstant(
        Compute(x->GetValue(), y->GetValue()), GetDexPc());
  }
  HConstant* Evaluate(HLongConstant* x, HLongConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetIntConstant(
        Compute(x->GetValue(), y->GetValue()), GetDexPc());
  }

  DECLARE_INSTRUCTION(GreaterThanOrEqual);

  IfCondition GetCondition() const OVERRIDE {
    return kCondGE;
  }

  IfCondition GetOppositeCondition() const OVERRIDE {
    return kCondLT;
  }

 private:
  template <typename T> bool Compute(T x, T y) const { return x >= y; }

  DISALLOW_COPY_AND_ASSIGN(HGreaterThanOrEqual);
};

class HBelow : public HCondition {
 public:
  HBelow(HInstruction* first, HInstruction* second, uint32_t dex_pc = kNoDexPc)
      : HCondition(first, second, dex_pc) {}

  HConstant* Evaluate(HIntConstant* x, HIntConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetIntConstant(
        Compute(static_cast<uint32_t>(x->GetValue()),
                static_cast<uint32_t>(y->GetValue())), GetDexPc());
  }
  HConstant* Evaluate(HLongConstant* x, HLongConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetIntConstant(
        Compute(static_cast<uint64_t>(x->GetValue()),
                static_cast<uint64_t>(y->GetValue())), GetDexPc());
  }

  DECLARE_INSTRUCTION(Below);

  IfCondition GetCondition() const OVERRIDE {
    return kCondB;
  }

  IfCondition GetOppositeCondition() const OVERRIDE {
    return kCondAE;
  }

 private:
  template <typename T> bool Compute(T x, T y) const { return x < y; }

  DISALLOW_COPY_AND_ASSIGN(HBelow);
};

class HBelowOrEqual : public HCondition {
 public:
  HBelowOrEqual(HInstruction* first, HInstruction* second, uint32_t dex_pc = kNoDexPc)
      : HCondition(first, second, dex_pc) {}

  HConstant* Evaluate(HIntConstant* x, HIntConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetIntConstant(
        Compute(static_cast<uint32_t>(x->GetValue()),
                static_cast<uint32_t>(y->GetValue())), GetDexPc());
  }
  HConstant* Evaluate(HLongConstant* x, HLongConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetIntConstant(
        Compute(static_cast<uint64_t>(x->GetValue()),
                static_cast<uint64_t>(y->GetValue())), GetDexPc());
  }

  DECLARE_INSTRUCTION(BelowOrEqual);

  IfCondition GetCondition() const OVERRIDE {
    return kCondBE;
  }

  IfCondition GetOppositeCondition() const OVERRIDE {
    return kCondA;
  }

 private:
  template <typename T> bool Compute(T x, T y) const { return x <= y; }

  DISALLOW_COPY_AND_ASSIGN(HBelowOrEqual);
};

class HAbove : public HCondition {
 public:
  HAbove(HInstruction* first, HInstruction* second, uint32_t dex_pc = kNoDexPc)
      : HCondition(first, second, dex_pc) {}

  HConstant* Evaluate(HIntConstant* x, HIntConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetIntConstant(
        Compute(static_cast<uint32_t>(x->GetValue()),
                static_cast<uint32_t>(y->GetValue())), GetDexPc());
  }
  HConstant* Evaluate(HLongConstant* x, HLongConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetIntConstant(
        Compute(static_cast<uint64_t>(x->GetValue()),
                static_cast<uint64_t>(y->GetValue())), GetDexPc());
  }

  DECLARE_INSTRUCTION(Above);

  IfCondition GetCondition() const OVERRIDE {
    return kCondA;
  }

  IfCondition GetOppositeCondition() const OVERRIDE {
    return kCondBE;
  }

 private:
  template <typename T> bool Compute(T x, T y) const { return x > y; }

  DISALLOW_COPY_AND_ASSIGN(HAbove);
};

class HAboveOrEqual : public HCondition {
 public:
  HAboveOrEqual(HInstruction* first, HInstruction* second, uint32_t dex_pc = kNoDexPc)
      : HCondition(first, second, dex_pc) {}

  HConstant* Evaluate(HIntConstant* x, HIntConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetIntConstant(
        Compute(static_cast<uint32_t>(x->GetValue()),
                static_cast<uint32_t>(y->GetValue())), GetDexPc());
  }
  HConstant* Evaluate(HLongConstant* x, HLongConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetIntConstant(
        Compute(static_cast<uint64_t>(x->GetValue()),
                static_cast<uint64_t>(y->GetValue())), GetDexPc());
  }

  DECLARE_INSTRUCTION(AboveOrEqual);

  IfCondition GetCondition() const OVERRIDE {
    return kCondAE;
  }

  IfCondition GetOppositeCondition() const OVERRIDE {
    return kCondB;
  }

 private:
  template <typename T> bool Compute(T x, T y) const { return x >= y; }

  DISALLOW_COPY_AND_ASSIGN(HAboveOrEqual);
};

// Instruction to check how two inputs compare to each other.
// Result is 0 if input0 == input1, 1 if input0 > input1, or -1 if input0 < input1.
class HCompare : public HBinaryOperation {
 public:
  HCompare(Primitive::Type type,
           HInstruction* first,
           HInstruction* second,
           ComparisonBias bias,
           uint32_t dex_pc)
      : HBinaryOperation(Primitive::kPrimInt,
                         first,
                         second,
                         SideEffectsForArchRuntimeCalls(type),
                         dex_pc),
        bias_(bias) {
    DCHECK_EQ(type, first->GetType());
    DCHECK_EQ(type, second->GetType());
  }

  template <typename T>
  int32_t Compute(T x, T y) const { return x == y ? 0 : x > y ? 1 : -1; }

  HConstant* Evaluate(HIntConstant* x, HIntConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetIntConstant(
        Compute(x->GetValue(), y->GetValue()), GetDexPc());
  }
  HConstant* Evaluate(HLongConstant* x, HLongConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetIntConstant(
        Compute(x->GetValue(), y->GetValue()), GetDexPc());
  }

  bool InstructionDataEquals(HInstruction* other) const OVERRIDE {
    return bias_ == other->AsCompare()->bias_;
  }

  ComparisonBias GetBias() const { return bias_; }

  bool IsGtBias() { return bias_ == ComparisonBias::kGtBias; }


  static SideEffects SideEffectsForArchRuntimeCalls(Primitive::Type type) {
    // MIPS64 uses a runtime call for FP comparisons.
    return Primitive::IsFloatingPointType(type) ? SideEffects::CanTriggerGC() : SideEffects::None();
  }

  DECLARE_INSTRUCTION(Compare);

 private:
  const ComparisonBias bias_;

  DISALLOW_COPY_AND_ASSIGN(HCompare);
};

// A local in the graph. Corresponds to a Dex register.
class HLocal : public HTemplateInstruction<0> {
 public:
  explicit HLocal(uint16_t reg_number)
      : HTemplateInstruction(SideEffects::None(), kNoDexPc), reg_number_(reg_number) {}

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
  HLoadLocal(HLocal* local, Primitive::Type type, uint32_t dex_pc = kNoDexPc)
      : HExpression(type, SideEffects::None(), dex_pc) {
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
  HStoreLocal(HLocal* local, HInstruction* value, uint32_t dex_pc = kNoDexPc)
      : HTemplateInstruction(SideEffects::None(), dex_pc) {
    SetRawInputAt(0, local);
    SetRawInputAt(1, value);
  }

  HLocal* GetLocal() const { return reinterpret_cast<HLocal*>(InputAt(0)); }

  DECLARE_INSTRUCTION(StoreLocal);

 private:
  DISALLOW_COPY_AND_ASSIGN(HStoreLocal);
};

class HFloatConstant : public HConstant {
 public:
  float GetValue() const { return value_; }

  uint64_t GetValueAsUint64() const OVERRIDE {
    return static_cast<uint64_t>(bit_cast<uint32_t, float>(value_));
  }

  bool InstructionDataEquals(HInstruction* other) const OVERRIDE {
    DCHECK(other->IsFloatConstant());
    return other->AsFloatConstant()->GetValueAsUint64() == GetValueAsUint64();
  }

  size_t ComputeHashCode() const OVERRIDE { return static_cast<size_t>(GetValue()); }

  bool IsMinusOne() const OVERRIDE {
    return bit_cast<uint32_t, float>(value_) == bit_cast<uint32_t, float>((-1.0f));
  }
  bool IsZero() const OVERRIDE {
    return value_ == 0.0f;
  }
  bool IsOne() const OVERRIDE {
    return bit_cast<uint32_t, float>(value_) == bit_cast<uint32_t, float>(1.0f);
  }
  bool IsNaN() const {
    return std::isnan(value_);
  }

  DECLARE_INSTRUCTION(FloatConstant);

 private:
  explicit HFloatConstant(float value, uint32_t dex_pc = kNoDexPc)
      : HConstant(Primitive::kPrimFloat, dex_pc), value_(value) {}
  explicit HFloatConstant(int32_t value, uint32_t dex_pc = kNoDexPc)
      : HConstant(Primitive::kPrimFloat, dex_pc), value_(bit_cast<float, int32_t>(value)) {}

  const float value_;

  // Only the SsaBuilder and HGraph can create floating-point constants.
  friend class SsaBuilder;
  friend class HGraph;
  DISALLOW_COPY_AND_ASSIGN(HFloatConstant);
};

class HDoubleConstant : public HConstant {
 public:
  double GetValue() const { return value_; }

  uint64_t GetValueAsUint64() const OVERRIDE { return bit_cast<uint64_t, double>(value_); }

  bool InstructionDataEquals(HInstruction* other) const OVERRIDE {
    DCHECK(other->IsDoubleConstant());
    return other->AsDoubleConstant()->GetValueAsUint64() == GetValueAsUint64();
  }

  size_t ComputeHashCode() const OVERRIDE { return static_cast<size_t>(GetValue()); }

  bool IsMinusOne() const OVERRIDE {
    return bit_cast<uint64_t, double>(value_) == bit_cast<uint64_t, double>((-1.0));
  }
  bool IsZero() const OVERRIDE {
    return value_ == 0.0;
  }
  bool IsOne() const OVERRIDE {
    return bit_cast<uint64_t, double>(value_) == bit_cast<uint64_t, double>(1.0);
  }
  bool IsNaN() const {
    return std::isnan(value_);
  }

  DECLARE_INSTRUCTION(DoubleConstant);

 private:
  explicit HDoubleConstant(double value, uint32_t dex_pc = kNoDexPc)
      : HConstant(Primitive::kPrimDouble, dex_pc), value_(value) {}
  explicit HDoubleConstant(int64_t value, uint32_t dex_pc = kNoDexPc)
      : HConstant(Primitive::kPrimDouble, dex_pc), value_(bit_cast<double, int64_t>(value)) {}

  const double value_;

  // Only the SsaBuilder and HGraph can create floating-point constants.
  friend class SsaBuilder;
  friend class HGraph;
  DISALLOW_COPY_AND_ASSIGN(HDoubleConstant);
};

enum class Intrinsics {
#define OPTIMIZING_INTRINSICS(Name, IsStatic, NeedsEnvironmentOrCache, SideEffects, Exceptions) \
  k ## Name,
#include "intrinsics_list.h"
  kNone,
  INTRINSICS_LIST(OPTIMIZING_INTRINSICS)
#undef INTRINSICS_LIST
#undef OPTIMIZING_INTRINSICS
};
std::ostream& operator<<(std::ostream& os, const Intrinsics& intrinsic);

enum IntrinsicNeedsEnvironmentOrCache {
  kNoEnvironmentOrCache,        // Intrinsic does not require an environment or dex cache.
  kNeedsEnvironmentOrCache      // Intrinsic requires an environment or requires a dex cache.
};

enum IntrinsicSideEffects {
  kNoSideEffects,     // Intrinsic does not have any heap memory side effects.
  kReadSideEffects,   // Intrinsic may read heap memory.
  kWriteSideEffects,  // Intrinsic may write heap memory.
  kAllSideEffects     // Intrinsic may read or write heap memory, or trigger GC.
};

enum IntrinsicExceptions {
  kNoThrow,  // Intrinsic does not throw any exceptions.
  kCanThrow  // Intrinsic may throw exceptions.
};

class HInvoke : public HInstruction {
 public:
  size_t InputCount() const OVERRIDE { return inputs_.size(); }

  bool NeedsEnvironment() const OVERRIDE;

  void SetArgumentAt(size_t index, HInstruction* argument) {
    SetRawInputAt(index, argument);
  }

  // Return the number of arguments.  This number can be lower than
  // the number of inputs returned by InputCount(), as some invoke
  // instructions (e.g. HInvokeStaticOrDirect) can have non-argument
  // inputs at the end of their list of inputs.
  uint32_t GetNumberOfArguments() const { return number_of_arguments_; }

  Primitive::Type GetType() const OVERRIDE { return return_type_; }

  uint32_t GetDexMethodIndex() const { return dex_method_index_; }
  const DexFile& GetDexFile() const { return GetEnvironment()->GetDexFile(); }

  InvokeType GetOriginalInvokeType() const { return original_invoke_type_; }

  Intrinsics GetIntrinsic() const {
    return intrinsic_;
  }

  void SetIntrinsic(Intrinsics intrinsic,
                    IntrinsicNeedsEnvironmentOrCache needs_env_or_cache,
                    IntrinsicSideEffects side_effects,
                    IntrinsicExceptions exceptions);

  bool IsFromInlinedInvoke() const {
    return GetEnvironment()->IsFromInlinedInvoke();
  }

  bool CanThrow() const OVERRIDE { return can_throw_; }

  bool CanBeMoved() const OVERRIDE { return IsIntrinsic(); }

  bool InstructionDataEquals(HInstruction* other) const OVERRIDE {
    return intrinsic_ != Intrinsics::kNone && intrinsic_ == other->AsInvoke()->intrinsic_;
  }

  uint32_t* GetIntrinsicOptimizations() {
    return &intrinsic_optimizations_;
  }

  const uint32_t* GetIntrinsicOptimizations() const {
    return &intrinsic_optimizations_;
  }

  bool IsIntrinsic() const { return intrinsic_ != Intrinsics::kNone; }

  DECLARE_ABSTRACT_INSTRUCTION(Invoke);

 protected:
  HInvoke(ArenaAllocator* arena,
          uint32_t number_of_arguments,
          uint32_t number_of_other_inputs,
          Primitive::Type return_type,
          uint32_t dex_pc,
          uint32_t dex_method_index,
          InvokeType original_invoke_type)
    : HInstruction(
          SideEffects::AllExceptGCDependency(), dex_pc),  // Assume write/read on all fields/arrays.
      number_of_arguments_(number_of_arguments),
      inputs_(number_of_arguments + number_of_other_inputs,
              arena->Adapter(kArenaAllocInvokeInputs)),
      return_type_(return_type),
      dex_method_index_(dex_method_index),
      original_invoke_type_(original_invoke_type),
      can_throw_(true),
      intrinsic_(Intrinsics::kNone),
      intrinsic_optimizations_(0) {
  }

  const HUserRecord<HInstruction*> InputRecordAt(size_t index) const OVERRIDE {
    return inputs_[index];
  }

  void SetRawInputRecordAt(size_t index, const HUserRecord<HInstruction*>& input) OVERRIDE {
    inputs_[index] = input;
  }

  void SetCanThrow(bool can_throw) { can_throw_ = can_throw; }

  uint32_t number_of_arguments_;
  ArenaVector<HUserRecord<HInstruction*>> inputs_;
  const Primitive::Type return_type_;
  const uint32_t dex_method_index_;
  const InvokeType original_invoke_type_;
  bool can_throw_;
  Intrinsics intrinsic_;

  // A magic word holding optimizations for intrinsics. See intrinsics.h.
  uint32_t intrinsic_optimizations_;

 private:
  DISALLOW_COPY_AND_ASSIGN(HInvoke);
};

class HInvokeUnresolved : public HInvoke {
 public:
  HInvokeUnresolved(ArenaAllocator* arena,
                    uint32_t number_of_arguments,
                    Primitive::Type return_type,
                    uint32_t dex_pc,
                    uint32_t dex_method_index,
                    InvokeType invoke_type)
      : HInvoke(arena,
                number_of_arguments,
                0u /* number_of_other_inputs */,
                return_type,
                dex_pc,
                dex_method_index,
                invoke_type) {
  }

  DECLARE_INSTRUCTION(InvokeUnresolved);

 private:
  DISALLOW_COPY_AND_ASSIGN(HInvokeUnresolved);
};

class HInvokeStaticOrDirect : public HInvoke {
 public:
  // Requirements of this method call regarding the class
  // initialization (clinit) check of its declaring class.
  enum class ClinitCheckRequirement {
    kNone,      // Class already initialized.
    kExplicit,  // Static call having explicit clinit check as last input.
    kImplicit,  // Static call implicitly requiring a clinit check.
  };

  // Determines how to load the target ArtMethod*.
  enum class MethodLoadKind {
    // Use a String init ArtMethod* loaded from Thread entrypoints.
    kStringInit,

    // Use the method's own ArtMethod* loaded by the register allocator.
    kRecursive,

    // Use ArtMethod* at a known address, embed the direct address in the code.
    // Used for app->boot calls with non-relocatable image and for JIT-compiled calls.
    kDirectAddress,

    // Use ArtMethod* at an address that will be known at link time, embed the direct
    // address in the code. If the image is relocatable, emit .patch_oat entry.
    // Used for app->boot calls with relocatable image and boot->boot calls, whether
    // the image relocatable or not.
    kDirectAddressWithFixup,

    // Load from resoved methods array in the dex cache using a PC-relative load.
    // Used when we need to use the dex cache, for example for invoke-static that
    // may cause class initialization (the entry may point to a resolution method),
    // and we know that we can access the dex cache arrays using a PC-relative load.
    kDexCachePcRelative,

    // Use ArtMethod* from the resolved methods of the compiled method's own ArtMethod*.
    // Used for JIT when we need to use the dex cache. This is also the last-resort-kind
    // used when other kinds are unavailable (say, dex cache arrays are not PC-relative)
    // or unimplemented or impractical (i.e. slow) on a particular architecture.
    kDexCacheViaMethod,
  };

  // Determines the location of the code pointer.
  enum class CodePtrLocation {
    // Recursive call, use local PC-relative call instruction.
    kCallSelf,

    // Use PC-relative call instruction patched at link time.
    // Used for calls within an oat file, boot->boot or app->app.
    kCallPCRelative,

    // Call to a known target address, embed the direct address in code.
    // Used for app->boot call with non-relocatable image and for JIT-compiled calls.
    kCallDirect,

    // Call to a target address that will be known at link time, embed the direct
    // address in code. If the image is relocatable, emit .patch_oat entry.
    // Used for app->boot calls with relocatable image and boot->boot calls, whether
    // the image relocatable or not.
    kCallDirectWithFixup,

    // Use code pointer from the ArtMethod*.
    // Used when we don't know the target code. This is also the last-resort-kind used when
    // other kinds are unimplemented or impractical (i.e. slow) on a particular architecture.
    kCallArtMethod,
  };

  struct DispatchInfo {
    MethodLoadKind method_load_kind;
    CodePtrLocation code_ptr_location;
    // The method load data holds
    //   - thread entrypoint offset for kStringInit method if this is a string init invoke.
    //     Note that there are multiple string init methods, each having its own offset.
    //   - the method address for kDirectAddress
    //   - the dex cache arrays offset for kDexCachePcRel.
    uint64_t method_load_data;
    uint64_t direct_code_ptr;
  };

  HInvokeStaticOrDirect(ArenaAllocator* arena,
                        uint32_t number_of_arguments,
                        Primitive::Type return_type,
                        uint32_t dex_pc,
                        uint32_t method_index,
                        MethodReference target_method,
                        DispatchInfo dispatch_info,
                        InvokeType original_invoke_type,
                        InvokeType optimized_invoke_type,
                        ClinitCheckRequirement clinit_check_requirement)
      : HInvoke(arena,
                number_of_arguments,
                // There is potentially one extra argument for the HCurrentMethod node, and
                // potentially one other if the clinit check is explicit, and potentially
                // one other if the method is a string factory.
                (NeedsCurrentMethodInput(dispatch_info.method_load_kind) ? 1u : 0u) +
                    (clinit_check_requirement == ClinitCheckRequirement::kExplicit ? 1u : 0u) +
                    (dispatch_info.method_load_kind == MethodLoadKind::kStringInit ? 1u : 0u),
                return_type,
                dex_pc,
                method_index,
                original_invoke_type),
        optimized_invoke_type_(optimized_invoke_type),
        clinit_check_requirement_(clinit_check_requirement),
        target_method_(target_method),
        dispatch_info_(dispatch_info) { }

  void SetDispatchInfo(const DispatchInfo& dispatch_info) {
    bool had_current_method_input = HasCurrentMethodInput();
    bool needs_current_method_input = NeedsCurrentMethodInput(dispatch_info.method_load_kind);

    // Using the current method is the default and once we find a better
    // method load kind, we should not go back to using the current method.
    DCHECK(had_current_method_input || !needs_current_method_input);

    if (had_current_method_input && !needs_current_method_input) {
      DCHECK_EQ(InputAt(GetSpecialInputIndex()), GetBlock()->GetGraph()->GetCurrentMethod());
      RemoveInputAt(GetSpecialInputIndex());
    }
    dispatch_info_ = dispatch_info;
  }

  void AddSpecialInput(HInstruction* input) {
    // We allow only one special input.
    DCHECK(!IsStringInit() && !HasCurrentMethodInput());
    DCHECK(InputCount() == GetSpecialInputIndex() ||
           (InputCount() == GetSpecialInputIndex() + 1 && IsStaticWithExplicitClinitCheck()));
    InsertInputAt(GetSpecialInputIndex(), input);
  }

  bool CanDoImplicitNullCheckOn(HInstruction* obj ATTRIBUTE_UNUSED) const OVERRIDE {
    // We access the method via the dex cache so we can't do an implicit null check.
    // TODO: for intrinsics we can generate implicit null checks.
    return false;
  }

  bool CanBeNull() const OVERRIDE {
    return return_type_ == Primitive::kPrimNot && !IsStringInit();
  }

  // Get the index of the special input, if any.
  //
  // If the invoke IsStringInit(), it initially has a HFakeString special argument
  // which is removed by the instruction simplifier; if the invoke HasCurrentMethodInput(),
  // the "special input" is the current method pointer; otherwise there may be one
  // platform-specific special input, such as PC-relative addressing base.
  uint32_t GetSpecialInputIndex() const { return GetNumberOfArguments(); }

  InvokeType GetOptimizedInvokeType() const { return optimized_invoke_type_; }
  void SetOptimizedInvokeType(InvokeType invoke_type) {
    optimized_invoke_type_ = invoke_type;
  }

  MethodLoadKind GetMethodLoadKind() const { return dispatch_info_.method_load_kind; }
  CodePtrLocation GetCodePtrLocation() const { return dispatch_info_.code_ptr_location; }
  bool IsRecursive() const { return GetMethodLoadKind() == MethodLoadKind::kRecursive; }
  bool NeedsDexCacheOfDeclaringClass() const OVERRIDE;
  bool IsStringInit() const { return GetMethodLoadKind() == MethodLoadKind::kStringInit; }
  bool HasMethodAddress() const { return GetMethodLoadKind() == MethodLoadKind::kDirectAddress; }
  bool HasPcRelativeDexCache() const {
    return GetMethodLoadKind() == MethodLoadKind::kDexCachePcRelative;
  }
  bool HasCurrentMethodInput() const {
    // This function can be called only after the invoke has been fully initialized by the builder.
    if (NeedsCurrentMethodInput(GetMethodLoadKind())) {
      DCHECK(InputAt(GetSpecialInputIndex())->IsCurrentMethod());
      return true;
    } else {
      DCHECK(InputCount() == GetSpecialInputIndex() ||
             !InputAt(GetSpecialInputIndex())->IsCurrentMethod());
      return false;
    }
  }
  bool HasDirectCodePtr() const { return GetCodePtrLocation() == CodePtrLocation::kCallDirect; }
  MethodReference GetTargetMethod() const { return target_method_; }
  void SetTargetMethod(MethodReference method) { target_method_ = method; }

  int32_t GetStringInitOffset() const {
    DCHECK(IsStringInit());
    return dispatch_info_.method_load_data;
  }

  uint64_t GetMethodAddress() const {
    DCHECK(HasMethodAddress());
    return dispatch_info_.method_load_data;
  }

  uint32_t GetDexCacheArrayOffset() const {
    DCHECK(HasPcRelativeDexCache());
    return dispatch_info_.method_load_data;
  }

  uint64_t GetDirectCodePtr() const {
    DCHECK(HasDirectCodePtr());
    return dispatch_info_.direct_code_ptr;
  }

  ClinitCheckRequirement GetClinitCheckRequirement() const { return clinit_check_requirement_; }

  // Is this instruction a call to a static method?
  bool IsStatic() const {
    return GetOriginalInvokeType() == kStatic;
  }

  // Remove the HClinitCheck or the replacement HLoadClass (set as last input by
  // PrepareForRegisterAllocation::VisitClinitCheck() in lieu of the initial HClinitCheck)
  // instruction; only relevant for static calls with explicit clinit check.
  void RemoveExplicitClinitCheck(ClinitCheckRequirement new_requirement) {
    DCHECK(IsStaticWithExplicitClinitCheck());
    size_t last_input_index = InputCount() - 1;
    HInstruction* last_input = InputAt(last_input_index);
    DCHECK(last_input != nullptr);
    DCHECK(last_input->IsLoadClass() || last_input->IsClinitCheck()) << last_input->DebugName();
    RemoveAsUserOfInput(last_input_index);
    inputs_.pop_back();
    clinit_check_requirement_ = new_requirement;
    DCHECK(!IsStaticWithExplicitClinitCheck());
  }

  bool IsStringFactoryFor(HFakeString* str) const {
    if (!IsStringInit()) return false;
    DCHECK(!HasCurrentMethodInput());
    if (InputCount() == (number_of_arguments_)) return false;
    return InputAt(InputCount() - 1)->AsFakeString() == str;
  }

  void RemoveFakeStringArgumentAsLastInput() {
    DCHECK(IsStringInit());
    size_t last_input_index = InputCount() - 1;
    HInstruction* last_input = InputAt(last_input_index);
    DCHECK(last_input != nullptr);
    DCHECK(last_input->IsFakeString()) << last_input->DebugName();
    RemoveAsUserOfInput(last_input_index);
    inputs_.pop_back();
  }

  // Is this a call to a static method whose declaring class has an
  // explicit initialization check in the graph?
  bool IsStaticWithExplicitClinitCheck() const {
    return IsStatic() && (clinit_check_requirement_ == ClinitCheckRequirement::kExplicit);
  }

  // Is this a call to a static method whose declaring class has an
  // implicit intialization check requirement?
  bool IsStaticWithImplicitClinitCheck() const {
    return IsStatic() && (clinit_check_requirement_ == ClinitCheckRequirement::kImplicit);
  }

  // Does this method load kind need the current method as an input?
  static bool NeedsCurrentMethodInput(MethodLoadKind kind) {
    return kind == MethodLoadKind::kRecursive || kind == MethodLoadKind::kDexCacheViaMethod;
  }

  DECLARE_INSTRUCTION(InvokeStaticOrDirect);

 protected:
  const HUserRecord<HInstruction*> InputRecordAt(size_t i) const OVERRIDE {
    const HUserRecord<HInstruction*> input_record = HInvoke::InputRecordAt(i);
    if (kIsDebugBuild && IsStaticWithExplicitClinitCheck() && (i == InputCount() - 1)) {
      HInstruction* input = input_record.GetInstruction();
      // `input` is the last input of a static invoke marked as having
      // an explicit clinit check. It must either be:
      // - an art::HClinitCheck instruction, set by art::HGraphBuilder; or
      // - an art::HLoadClass instruction, set by art::PrepareForRegisterAllocation.
      DCHECK(input != nullptr);
      DCHECK(input->IsClinitCheck() || input->IsLoadClass()) << input->DebugName();
    }
    return input_record;
  }

  void InsertInputAt(size_t index, HInstruction* input);
  void RemoveInputAt(size_t index);

 private:
  InvokeType optimized_invoke_type_;
  ClinitCheckRequirement clinit_check_requirement_;
  // The target method may refer to different dex file or method index than the original
  // invoke. This happens for sharpened calls and for calls where a method was redeclared
  // in derived class to increase visibility.
  MethodReference target_method_;
  DispatchInfo dispatch_info_;

  DISALLOW_COPY_AND_ASSIGN(HInvokeStaticOrDirect);
};
std::ostream& operator<<(std::ostream& os, HInvokeStaticOrDirect::MethodLoadKind rhs);
std::ostream& operator<<(std::ostream& os, HInvokeStaticOrDirect::ClinitCheckRequirement rhs);

class HInvokeVirtual : public HInvoke {
 public:
  HInvokeVirtual(ArenaAllocator* arena,
                 uint32_t number_of_arguments,
                 Primitive::Type return_type,
                 uint32_t dex_pc,
                 uint32_t dex_method_index,
                 uint32_t vtable_index)
      : HInvoke(arena, number_of_arguments, 0u, return_type, dex_pc, dex_method_index, kVirtual),
        vtable_index_(vtable_index) {}

  bool CanDoImplicitNullCheckOn(HInstruction* obj) const OVERRIDE {
    // TODO: Add implicit null checks in intrinsics.
    return (obj == InputAt(0)) && !GetLocations()->Intrinsified();
  }

  uint32_t GetVTableIndex() const { return vtable_index_; }

  DECLARE_INSTRUCTION(InvokeVirtual);

 private:
  const uint32_t vtable_index_;

  DISALLOW_COPY_AND_ASSIGN(HInvokeVirtual);
};

class HInvokeInterface : public HInvoke {
 public:
  HInvokeInterface(ArenaAllocator* arena,
                   uint32_t number_of_arguments,
                   Primitive::Type return_type,
                   uint32_t dex_pc,
                   uint32_t dex_method_index,
                   uint32_t imt_index)
      : HInvoke(arena, number_of_arguments, 0u, return_type, dex_pc, dex_method_index, kInterface),
        imt_index_(imt_index) {}

  bool CanDoImplicitNullCheckOn(HInstruction* obj) const OVERRIDE {
    // TODO: Add implicit null checks in intrinsics.
    return (obj == InputAt(0)) && !GetLocations()->Intrinsified();
  }

  uint32_t GetImtIndex() const { return imt_index_; }
  uint32_t GetDexMethodIndex() const { return dex_method_index_; }

  DECLARE_INSTRUCTION(InvokeInterface);

 private:
  const uint32_t imt_index_;

  DISALLOW_COPY_AND_ASSIGN(HInvokeInterface);
};

class HNewInstance : public HExpression<2> {
 public:
  HNewInstance(HInstruction* cls,
               HCurrentMethod* current_method,
               uint32_t dex_pc,
               uint16_t type_index,
               const DexFile& dex_file,
               bool can_throw,
               bool finalizable,
               QuickEntrypointEnum entrypoint)
      : HExpression(Primitive::kPrimNot, SideEffects::CanTriggerGC(), dex_pc),
        type_index_(type_index),
        dex_file_(dex_file),
        can_throw_(can_throw),
        finalizable_(finalizable),
        entrypoint_(entrypoint) {
    SetRawInputAt(0, cls);
    SetRawInputAt(1, current_method);
  }

  uint16_t GetTypeIndex() const { return type_index_; }
  const DexFile& GetDexFile() const { return dex_file_; }

  // Calls runtime so needs an environment.
  bool NeedsEnvironment() const OVERRIDE { return true; }

  // It may throw when called on type that's not instantiable/accessible.
  // It can throw OOME.
  // TODO: distinguish between the two cases so we can for example allow allocation elimination.
  bool CanThrow() const OVERRIDE { return can_throw_ || true; }

  bool IsFinalizable() const { return finalizable_; }

  bool CanBeNull() const OVERRIDE { return false; }

  QuickEntrypointEnum GetEntrypoint() const { return entrypoint_; }

  void SetEntrypoint(QuickEntrypointEnum entrypoint) {
    entrypoint_ = entrypoint;
  }

  DECLARE_INSTRUCTION(NewInstance);

 private:
  const uint16_t type_index_;
  const DexFile& dex_file_;
  const bool can_throw_;
  const bool finalizable_;
  QuickEntrypointEnum entrypoint_;

  DISALLOW_COPY_AND_ASSIGN(HNewInstance);
};

class HNeg : public HUnaryOperation {
 public:
  HNeg(Primitive::Type result_type, HInstruction* input, uint32_t dex_pc = kNoDexPc)
      : HUnaryOperation(result_type, input, dex_pc) {}

  template <typename T> T Compute(T x) const { return -x; }

  HConstant* Evaluate(HIntConstant* x) const OVERRIDE {
    return GetBlock()->GetGraph()->GetIntConstant(Compute(x->GetValue()), GetDexPc());
  }
  HConstant* Evaluate(HLongConstant* x) const OVERRIDE {
    return GetBlock()->GetGraph()->GetLongConstant(Compute(x->GetValue()), GetDexPc());
  }

  DECLARE_INSTRUCTION(Neg);

 private:
  DISALLOW_COPY_AND_ASSIGN(HNeg);
};

class HNewArray : public HExpression<2> {
 public:
  HNewArray(HInstruction* length,
            HCurrentMethod* current_method,
            uint32_t dex_pc,
            uint16_t type_index,
            const DexFile& dex_file,
            QuickEntrypointEnum entrypoint)
      : HExpression(Primitive::kPrimNot, SideEffects::CanTriggerGC(), dex_pc),
        type_index_(type_index),
        dex_file_(dex_file),
        entrypoint_(entrypoint) {
    SetRawInputAt(0, length);
    SetRawInputAt(1, current_method);
  }

  uint16_t GetTypeIndex() const { return type_index_; }
  const DexFile& GetDexFile() const { return dex_file_; }

  // Calls runtime so needs an environment.
  bool NeedsEnvironment() const OVERRIDE { return true; }

  // May throw NegativeArraySizeException, OutOfMemoryError, etc.
  bool CanThrow() const OVERRIDE { return true; }

  bool CanBeNull() const OVERRIDE { return false; }

  QuickEntrypointEnum GetEntrypoint() const { return entrypoint_; }

  DECLARE_INSTRUCTION(NewArray);

 private:
  const uint16_t type_index_;
  const DexFile& dex_file_;
  const QuickEntrypointEnum entrypoint_;

  DISALLOW_COPY_AND_ASSIGN(HNewArray);
};

class HAdd : public HBinaryOperation {
 public:
  HAdd(Primitive::Type result_type,
       HInstruction* left,
       HInstruction* right,
       uint32_t dex_pc = kNoDexPc)
      : HBinaryOperation(result_type, left, right, SideEffects::None(), dex_pc) {}

  bool IsCommutative() const OVERRIDE { return true; }

  template <typename T> T Compute(T x, T y) const { return x + y; }

  HConstant* Evaluate(HIntConstant* x, HIntConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetIntConstant(
        Compute(x->GetValue(), y->GetValue()), GetDexPc());
  }
  HConstant* Evaluate(HLongConstant* x, HLongConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetLongConstant(
        Compute(x->GetValue(), y->GetValue()), GetDexPc());
  }

  DECLARE_INSTRUCTION(Add);

 private:
  DISALLOW_COPY_AND_ASSIGN(HAdd);
};

class HSub : public HBinaryOperation {
 public:
  HSub(Primitive::Type result_type,
       HInstruction* left,
       HInstruction* right,
       uint32_t dex_pc = kNoDexPc)
      : HBinaryOperation(result_type, left, right, SideEffects::None(), dex_pc) {}

  template <typename T> T Compute(T x, T y) const { return x - y; }

  HConstant* Evaluate(HIntConstant* x, HIntConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetIntConstant(
        Compute(x->GetValue(), y->GetValue()), GetDexPc());
  }
  HConstant* Evaluate(HLongConstant* x, HLongConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetLongConstant(
        Compute(x->GetValue(), y->GetValue()), GetDexPc());
  }

  DECLARE_INSTRUCTION(Sub);

 private:
  DISALLOW_COPY_AND_ASSIGN(HSub);
};

class HMul : public HBinaryOperation {
 public:
  HMul(Primitive::Type result_type,
       HInstruction* left,
       HInstruction* right,
       uint32_t dex_pc = kNoDexPc)
      : HBinaryOperation(result_type, left, right, SideEffects::None(), dex_pc) {}

  bool IsCommutative() const OVERRIDE { return true; }

  template <typename T> T Compute(T x, T y) const { return x * y; }

  HConstant* Evaluate(HIntConstant* x, HIntConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetIntConstant(
        Compute(x->GetValue(), y->GetValue()), GetDexPc());
  }
  HConstant* Evaluate(HLongConstant* x, HLongConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetLongConstant(
        Compute(x->GetValue(), y->GetValue()), GetDexPc());
  }

  DECLARE_INSTRUCTION(Mul);

 private:
  DISALLOW_COPY_AND_ASSIGN(HMul);
};

class HDiv : public HBinaryOperation {
 public:
  HDiv(Primitive::Type result_type,
       HInstruction* left,
       HInstruction* right,
       uint32_t dex_pc)
      : HBinaryOperation(result_type, left, right, SideEffectsForArchRuntimeCalls(), dex_pc) {}

  template <typename T>
  T Compute(T x, T y) const {
    // Our graph structure ensures we never have 0 for `y` during
    // constant folding.
    DCHECK_NE(y, 0);
    // Special case -1 to avoid getting a SIGFPE on x86(_64).
    return (y == -1) ? -x : x / y;
  }

  HConstant* Evaluate(HIntConstant* x, HIntConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetIntConstant(
        Compute(x->GetValue(), y->GetValue()), GetDexPc());
  }
  HConstant* Evaluate(HLongConstant* x, HLongConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetLongConstant(
        Compute(x->GetValue(), y->GetValue()), GetDexPc());
  }

  static SideEffects SideEffectsForArchRuntimeCalls() {
    // The generated code can use a runtime call.
    return SideEffects::CanTriggerGC();
  }

  DECLARE_INSTRUCTION(Div);

 private:
  DISALLOW_COPY_AND_ASSIGN(HDiv);
};

class HRem : public HBinaryOperation {
 public:
  HRem(Primitive::Type result_type,
       HInstruction* left,
       HInstruction* right,
       uint32_t dex_pc)
      : HBinaryOperation(result_type, left, right, SideEffectsForArchRuntimeCalls(), dex_pc) {}

  template <typename T>
  T Compute(T x, T y) const {
    // Our graph structure ensures we never have 0 for `y` during
    // constant folding.
    DCHECK_NE(y, 0);
    // Special case -1 to avoid getting a SIGFPE on x86(_64).
    return (y == -1) ? 0 : x % y;
  }

  HConstant* Evaluate(HIntConstant* x, HIntConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetIntConstant(
        Compute(x->GetValue(), y->GetValue()), GetDexPc());
  }
  HConstant* Evaluate(HLongConstant* x, HLongConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetLongConstant(
        Compute(x->GetValue(), y->GetValue()), GetDexPc());
  }


  static SideEffects SideEffectsForArchRuntimeCalls() {
    return SideEffects::CanTriggerGC();
  }

  DECLARE_INSTRUCTION(Rem);

 private:
  DISALLOW_COPY_AND_ASSIGN(HRem);
};

class HDivZeroCheck : public HExpression<1> {
 public:
  HDivZeroCheck(HInstruction* value, uint32_t dex_pc)
      : HExpression(value->GetType(), SideEffects::None(), dex_pc) {
    SetRawInputAt(0, value);
  }

  Primitive::Type GetType() const OVERRIDE { return InputAt(0)->GetType(); }

  bool CanBeMoved() const OVERRIDE { return true; }

  bool InstructionDataEquals(HInstruction* other ATTRIBUTE_UNUSED) const OVERRIDE {
    return true;
  }

  bool NeedsEnvironment() const OVERRIDE { return true; }
  bool CanThrow() const OVERRIDE { return true; }

  DECLARE_INSTRUCTION(DivZeroCheck);

 private:
  DISALLOW_COPY_AND_ASSIGN(HDivZeroCheck);
};

class HShl : public HBinaryOperation {
 public:
  HShl(Primitive::Type result_type,
       HInstruction* left,
       HInstruction* right,
       uint32_t dex_pc = kNoDexPc)
      : HBinaryOperation(result_type, left, right, SideEffects::None(), dex_pc) {}

  template <typename T, typename U, typename V>
  T Compute(T x, U y, V max_shift_value) const {
    static_assert(std::is_same<V, typename std::make_unsigned<T>::type>::value,
                  "V is not the unsigned integer type corresponding to T");
    return x << (y & max_shift_value);
  }

  HConstant* Evaluate(HIntConstant* x, HIntConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetIntConstant(
        Compute(x->GetValue(), y->GetValue(), kMaxIntShiftValue), GetDexPc());
  }
  // There is no `Evaluate(HIntConstant* x, HLongConstant* y)`, as this
  // case is handled as `x << static_cast<int>(y)`.
  HConstant* Evaluate(HLongConstant* x, HIntConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetLongConstant(
        Compute(x->GetValue(), y->GetValue(), kMaxLongShiftValue), GetDexPc());
  }
  HConstant* Evaluate(HLongConstant* x, HLongConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetLongConstant(
        Compute(x->GetValue(), y->GetValue(), kMaxLongShiftValue), GetDexPc());
  }

  DECLARE_INSTRUCTION(Shl);

 private:
  DISALLOW_COPY_AND_ASSIGN(HShl);
};

class HShr : public HBinaryOperation {
 public:
  HShr(Primitive::Type result_type,
       HInstruction* left,
       HInstruction* right,
       uint32_t dex_pc = kNoDexPc)
      : HBinaryOperation(result_type, left, right, SideEffects::None(), dex_pc) {}

  template <typename T, typename U, typename V>
  T Compute(T x, U y, V max_shift_value) const {
    static_assert(std::is_same<V, typename std::make_unsigned<T>::type>::value,
                  "V is not the unsigned integer type corresponding to T");
    return x >> (y & max_shift_value);
  }

  HConstant* Evaluate(HIntConstant* x, HIntConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetIntConstant(
        Compute(x->GetValue(), y->GetValue(), kMaxIntShiftValue), GetDexPc());
  }
  // There is no `Evaluate(HIntConstant* x, HLongConstant* y)`, as this
  // case is handled as `x >> static_cast<int>(y)`.
  HConstant* Evaluate(HLongConstant* x, HIntConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetLongConstant(
        Compute(x->GetValue(), y->GetValue(), kMaxLongShiftValue), GetDexPc());
  }
  HConstant* Evaluate(HLongConstant* x, HLongConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetLongConstant(
        Compute(x->GetValue(), y->GetValue(), kMaxLongShiftValue), GetDexPc());
  }

  DECLARE_INSTRUCTION(Shr);

 private:
  DISALLOW_COPY_AND_ASSIGN(HShr);
};

class HUShr : public HBinaryOperation {
 public:
  HUShr(Primitive::Type result_type,
        HInstruction* left,
        HInstruction* right,
        uint32_t dex_pc = kNoDexPc)
      : HBinaryOperation(result_type, left, right, SideEffects::None(), dex_pc) {}

  template <typename T, typename U, typename V>
  T Compute(T x, U y, V max_shift_value) const {
    static_assert(std::is_same<V, typename std::make_unsigned<T>::type>::value,
                  "V is not the unsigned integer type corresponding to T");
    V ux = static_cast<V>(x);
    return static_cast<T>(ux >> (y & max_shift_value));
  }

  HConstant* Evaluate(HIntConstant* x, HIntConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetIntConstant(
        Compute(x->GetValue(), y->GetValue(), kMaxIntShiftValue), GetDexPc());
  }
  // There is no `Evaluate(HIntConstant* x, HLongConstant* y)`, as this
  // case is handled as `x >>> static_cast<int>(y)`.
  HConstant* Evaluate(HLongConstant* x, HIntConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetLongConstant(
        Compute(x->GetValue(), y->GetValue(), kMaxLongShiftValue), GetDexPc());
  }
  HConstant* Evaluate(HLongConstant* x, HLongConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetLongConstant(
        Compute(x->GetValue(), y->GetValue(), kMaxLongShiftValue), GetDexPc());
  }

  DECLARE_INSTRUCTION(UShr);

 private:
  DISALLOW_COPY_AND_ASSIGN(HUShr);
};

class HAnd : public HBinaryOperation {
 public:
  HAnd(Primitive::Type result_type,
       HInstruction* left,
       HInstruction* right,
       uint32_t dex_pc = kNoDexPc)
      : HBinaryOperation(result_type, left, right, SideEffects::None(), dex_pc) {}

  bool IsCommutative() const OVERRIDE { return true; }

  template <typename T, typename U>
  auto Compute(T x, U y) const -> decltype(x & y) { return x & y; }

  HConstant* Evaluate(HIntConstant* x, HIntConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetIntConstant(
        Compute(x->GetValue(), y->GetValue()), GetDexPc());
  }
  HConstant* Evaluate(HIntConstant* x, HLongConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetLongConstant(
        Compute(x->GetValue(), y->GetValue()), GetDexPc());
  }
  HConstant* Evaluate(HLongConstant* x, HIntConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetLongConstant(
        Compute(x->GetValue(), y->GetValue()), GetDexPc());
  }
  HConstant* Evaluate(HLongConstant* x, HLongConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetLongConstant(
        Compute(x->GetValue(), y->GetValue()), GetDexPc());
  }

  DECLARE_INSTRUCTION(And);

 private:
  DISALLOW_COPY_AND_ASSIGN(HAnd);
};

class HOr : public HBinaryOperation {
 public:
  HOr(Primitive::Type result_type,
      HInstruction* left,
      HInstruction* right,
      uint32_t dex_pc = kNoDexPc)
      : HBinaryOperation(result_type, left, right, SideEffects::None(), dex_pc) {}

  bool IsCommutative() const OVERRIDE { return true; }

  template <typename T, typename U>
  auto Compute(T x, U y) const -> decltype(x | y) { return x | y; }

  HConstant* Evaluate(HIntConstant* x, HIntConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetIntConstant(
        Compute(x->GetValue(), y->GetValue()), GetDexPc());
  }
  HConstant* Evaluate(HIntConstant* x, HLongConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetLongConstant(
        Compute(x->GetValue(), y->GetValue()), GetDexPc());
  }
  HConstant* Evaluate(HLongConstant* x, HIntConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetLongConstant(
        Compute(x->GetValue(), y->GetValue()), GetDexPc());
  }
  HConstant* Evaluate(HLongConstant* x, HLongConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetLongConstant(
        Compute(x->GetValue(), y->GetValue()), GetDexPc());
  }

  DECLARE_INSTRUCTION(Or);

 private:
  DISALLOW_COPY_AND_ASSIGN(HOr);
};

class HXor : public HBinaryOperation {
 public:
  HXor(Primitive::Type result_type,
       HInstruction* left,
       HInstruction* right,
       uint32_t dex_pc = kNoDexPc)
      : HBinaryOperation(result_type, left, right, SideEffects::None(), dex_pc) {}

  bool IsCommutative() const OVERRIDE { return true; }

  template <typename T, typename U>
  auto Compute(T x, U y) const -> decltype(x ^ y) { return x ^ y; }

  HConstant* Evaluate(HIntConstant* x, HIntConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetIntConstant(
        Compute(x->GetValue(), y->GetValue()), GetDexPc());
  }
  HConstant* Evaluate(HIntConstant* x, HLongConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetLongConstant(
        Compute(x->GetValue(), y->GetValue()), GetDexPc());
  }
  HConstant* Evaluate(HLongConstant* x, HIntConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetLongConstant(
        Compute(x->GetValue(), y->GetValue()), GetDexPc());
  }
  HConstant* Evaluate(HLongConstant* x, HLongConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetLongConstant(
        Compute(x->GetValue(), y->GetValue()), GetDexPc());
  }

  DECLARE_INSTRUCTION(Xor);

 private:
  DISALLOW_COPY_AND_ASSIGN(HXor);
};

class HRor : public HBinaryOperation {
 public:
  HRor(Primitive::Type result_type, HInstruction* value, HInstruction* distance)
    : HBinaryOperation(result_type, value, distance) {}

  template <typename T, typename U, typename V>
  T Compute(T x, U y, V max_shift_value) const {
    static_assert(std::is_same<V, typename std::make_unsigned<T>::type>::value,
                  "V is not the unsigned integer type corresponding to T");
    V ux = static_cast<V>(x);
    if ((y & max_shift_value) == 0) {
      return static_cast<T>(ux);
    } else {
      const V reg_bits = sizeof(T) * 8;
      return static_cast<T>(ux >> (y & max_shift_value)) |
                           (x << (reg_bits - (y & max_shift_value)));
    }
  }

  HConstant* Evaluate(HIntConstant* x, HIntConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetIntConstant(
        Compute(x->GetValue(), y->GetValue(), kMaxIntShiftValue), GetDexPc());
  }
  HConstant* Evaluate(HLongConstant* x, HIntConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetLongConstant(
        Compute(x->GetValue(), y->GetValue(), kMaxLongShiftValue), GetDexPc());
  }
  HConstant* Evaluate(HLongConstant* x, HLongConstant* y) const OVERRIDE {
    return GetBlock()->GetGraph()->GetLongConstant(
        Compute(x->GetValue(), y->GetValue(), kMaxLongShiftValue), GetDexPc());
  }

  DECLARE_INSTRUCTION(Ror);

 private:
  DISALLOW_COPY_AND_ASSIGN(HRor);
};

// The value of a parameter in this method. Its location depends on
// the calling convention.
class HParameterValue : public HExpression<0> {
 public:
  HParameterValue(const DexFile& dex_file,
                  uint16_t type_index,
                  uint8_t index,
                  Primitive::Type parameter_type,
                  bool is_this = false)
      : HExpression(parameter_type, SideEffects::None(), kNoDexPc),
        dex_file_(dex_file),
        type_index_(type_index),
        index_(index),
        is_this_(is_this),
        can_be_null_(!is_this) {}

  const DexFile& GetDexFile() const { return dex_file_; }
  uint16_t GetTypeIndex() const { return type_index_; }
  uint8_t GetIndex() const { return index_; }
  bool IsThis() const { return is_this_; }

  bool CanBeNull() const OVERRIDE { return can_be_null_; }
  void SetCanBeNull(bool can_be_null) { can_be_null_ = can_be_null; }

  DECLARE_INSTRUCTION(ParameterValue);

 private:
  const DexFile& dex_file_;
  const uint16_t type_index_;
  // The index of this parameter in the parameters list. Must be less
  // than HGraph::number_of_in_vregs_.
  const uint8_t index_;

  // Whether or not the parameter value corresponds to 'this' argument.
  const bool is_this_;

  bool can_be_null_;

  DISALLOW_COPY_AND_ASSIGN(HParameterValue);
};

class HNot : public HUnaryOperation {
 public:
  HNot(Primitive::Type result_type, HInstruction* input, uint32_t dex_pc = kNoDexPc)
      : HUnaryOperation(result_type, input, dex_pc) {}

  bool CanBeMoved() const OVERRIDE { return true; }
  bool InstructionDataEquals(HInstruction* other ATTRIBUTE_UNUSED) const OVERRIDE {
    return true;
  }

  template <typename T> T Compute(T x) const { return ~x; }

  HConstant* Evaluate(HIntConstant* x) const OVERRIDE {
    return GetBlock()->GetGraph()->GetIntConstant(Compute(x->GetValue()), GetDexPc());
  }
  HConstant* Evaluate(HLongConstant* x) const OVERRIDE {
    return GetBlock()->GetGraph()->GetLongConstant(Compute(x->GetValue()), GetDexPc());
  }

  DECLARE_INSTRUCTION(Not);

 private:
  DISALLOW_COPY_AND_ASSIGN(HNot);
};

class HBooleanNot : public HUnaryOperation {
 public:
  explicit HBooleanNot(HInstruction* input, uint32_t dex_pc = kNoDexPc)
      : HUnaryOperation(Primitive::Type::kPrimBoolean, input, dex_pc) {}

  bool CanBeMoved() const OVERRIDE { return true; }
  bool InstructionDataEquals(HInstruction* other ATTRIBUTE_UNUSED) const OVERRIDE {
    return true;
  }

  template <typename T> bool Compute(T x) const {
    DCHECK(IsUint<1>(x));
    return !x;
  }

  HConstant* Evaluate(HIntConstant* x) const OVERRIDE {
    return GetBlock()->GetGraph()->GetIntConstant(Compute(x->GetValue()), GetDexPc());
  }
  HConstant* Evaluate(HLongConstant* x ATTRIBUTE_UNUSED) const OVERRIDE {
    LOG(FATAL) << DebugName() << " is not defined for long values";
    UNREACHABLE();
  }

  DECLARE_INSTRUCTION(BooleanNot);

 private:
  DISALLOW_COPY_AND_ASSIGN(HBooleanNot);
};

class HTypeConversion : public HExpression<1> {
 public:
  // Instantiate a type conversion of `input` to `result_type`.
  HTypeConversion(Primitive::Type result_type, HInstruction* input, uint32_t dex_pc)
      : HExpression(result_type,
                    SideEffectsForArchRuntimeCalls(input->GetType(), result_type),
                    dex_pc) {
    SetRawInputAt(0, input);
    DCHECK_NE(input->GetType(), result_type);
  }

  HInstruction* GetInput() const { return InputAt(0); }
  Primitive::Type GetInputType() const { return GetInput()->GetType(); }
  Primitive::Type GetResultType() const { return GetType(); }

  // Required by the x86, ARM, MIPS and MIPS64 code generators when producing calls
  // to the runtime.

  bool CanBeMoved() const OVERRIDE { return true; }
  bool InstructionDataEquals(HInstruction* other ATTRIBUTE_UNUSED) const OVERRIDE { return true; }

  // Try to statically evaluate the conversion and return a HConstant
  // containing the result.  If the input cannot be converted, return nullptr.
  HConstant* TryStaticEvaluation() const;

  static SideEffects SideEffectsForArchRuntimeCalls(Primitive::Type input_type,
                                                    Primitive::Type result_type) {
    // Some architectures may not require the 'GC' side effects, but at this point
    // in the compilation process we do not know what architecture we will
    // generate code for, so we must be conservative.
    if ((Primitive::IsFloatingPointType(input_type) && Primitive::IsIntegralType(result_type))
        || (input_type == Primitive::kPrimLong && Primitive::IsFloatingPointType(result_type))) {
      return SideEffects::CanTriggerGC();
    }
    return SideEffects::None();
  }

  DECLARE_INSTRUCTION(TypeConversion);

 private:
  DISALLOW_COPY_AND_ASSIGN(HTypeConversion);
};

static constexpr uint32_t kNoRegNumber = -1;

class HPhi : public HInstruction {
 public:
  HPhi(ArenaAllocator* arena,
       uint32_t reg_number,
       size_t number_of_inputs,
       Primitive::Type type,
       uint32_t dex_pc = kNoDexPc)
      : HInstruction(SideEffects::None(), dex_pc),
        inputs_(number_of_inputs, arena->Adapter(kArenaAllocPhiInputs)),
        reg_number_(reg_number),
        type_(ToPhiType(type)),
        // Phis are constructed live and marked dead if conflicting or unused.
        // Individual steps of SsaBuilder should assume that if a phi has been
        // marked dead, it can be ignored and will be removed by SsaPhiElimination.
        is_live_(true),
        can_be_null_(true) {
    DCHECK_NE(type_, Primitive::kPrimVoid);
  }

  // Returns a type equivalent to the given `type`, but that a `HPhi` can hold.
  static Primitive::Type ToPhiType(Primitive::Type type) {
    switch (type) {
      case Primitive::kPrimBoolean:
      case Primitive::kPrimByte:
      case Primitive::kPrimShort:
      case Primitive::kPrimChar:
        return Primitive::kPrimInt;
      default:
        return type;
    }
  }

  bool IsCatchPhi() const { return GetBlock()->IsCatchBlock(); }

  size_t InputCount() const OVERRIDE { return inputs_.size(); }

  void AddInput(HInstruction* input);
  void RemoveInputAt(size_t index);

  Primitive::Type GetType() const OVERRIDE { return type_; }
  void SetType(Primitive::Type new_type) {
    // Make sure that only valid type changes occur. The following are allowed:
    //  (1) int  -> float/ref (primitive type propagation),
    //  (2) long -> double (primitive type propagation).
    DCHECK(type_ == new_type ||
           (type_ == Primitive::kPrimInt && new_type == Primitive::kPrimFloat) ||
           (type_ == Primitive::kPrimInt && new_type == Primitive::kPrimNot) ||
           (type_ == Primitive::kPrimLong && new_type == Primitive::kPrimDouble));
    type_ = new_type;
  }

  bool CanBeNull() const OVERRIDE { return can_be_null_; }
  void SetCanBeNull(bool can_be_null) { can_be_null_ = can_be_null; }

  uint32_t GetRegNumber() const { return reg_number_; }

  void SetDead() { is_live_ = false; }
  void SetLive() { is_live_ = true; }
  bool IsDead() const { return !is_live_; }
  bool IsLive() const { return is_live_; }

  bool IsVRegEquivalentOf(HInstruction* other) const {
    return other != nullptr
        && other->IsPhi()
        && other->AsPhi()->GetBlock() == GetBlock()
        && other->AsPhi()->GetRegNumber() == GetRegNumber();
  }

  // Returns the next equivalent phi (starting from the current one) or null if there is none.
  // An equivalent phi is a phi having the same dex register and type.
  // It assumes that phis with the same dex register are adjacent.
  HPhi* GetNextEquivalentPhiWithSameType() {
    HInstruction* next = GetNext();
    while (next != nullptr && next->AsPhi()->GetRegNumber() == reg_number_) {
      if (next->GetType() == GetType()) {
        return next->AsPhi();
      }
      next = next->GetNext();
    }
    return nullptr;
  }

  DECLARE_INSTRUCTION(Phi);

 protected:
  const HUserRecord<HInstruction*> InputRecordAt(size_t index) const OVERRIDE {
    return inputs_[index];
  }

  void SetRawInputRecordAt(size_t index, const HUserRecord<HInstruction*>& input) OVERRIDE {
    inputs_[index] = input;
  }

 private:
  ArenaVector<HUserRecord<HInstruction*> > inputs_;
  const uint32_t reg_number_;
  Primitive::Type type_;
  bool is_live_;
  bool can_be_null_;

  DISALLOW_COPY_AND_ASSIGN(HPhi);
};

class HNullCheck : public HExpression<1> {
 public:
  HNullCheck(HInstruction* value, uint32_t dex_pc)
      : HExpression(value->GetType(), SideEffects::None(), dex_pc) {
    SetRawInputAt(0, value);
  }

  bool CanBeMoved() const OVERRIDE { return true; }
  bool InstructionDataEquals(HInstruction* other ATTRIBUTE_UNUSED) const OVERRIDE {
    return true;
  }

  bool NeedsEnvironment() const OVERRIDE { return true; }

  bool CanThrow() const OVERRIDE { return true; }

  bool CanBeNull() const OVERRIDE { return false; }


  DECLARE_INSTRUCTION(NullCheck);

 private:
  DISALLOW_COPY_AND_ASSIGN(HNullCheck);
};

class FieldInfo : public ValueObject {
 public:
  FieldInfo(MemberOffset field_offset,
            Primitive::Type field_type,
            bool is_volatile,
            uint32_t index,
            uint16_t declaring_class_def_index,
            const DexFile& dex_file,
            Handle<mirror::DexCache> dex_cache)
      : field_offset_(field_offset),
        field_type_(field_type),
        is_volatile_(is_volatile),
        index_(index),
        declaring_class_def_index_(declaring_class_def_index),
        dex_file_(dex_file),
        dex_cache_(dex_cache) {}

  MemberOffset GetFieldOffset() const { return field_offset_; }
  Primitive::Type GetFieldType() const { return field_type_; }
  uint32_t GetFieldIndex() const { return index_; }
  uint16_t GetDeclaringClassDefIndex() const { return declaring_class_def_index_;}
  const DexFile& GetDexFile() const { return dex_file_; }
  bool IsVolatile() const { return is_volatile_; }
  Handle<mirror::DexCache> GetDexCache() const { return dex_cache_; }

 private:
  const MemberOffset field_offset_;
  const Primitive::Type field_type_;
  const bool is_volatile_;
  const uint32_t index_;
  const uint16_t declaring_class_def_index_;
  const DexFile& dex_file_;
  const Handle<mirror::DexCache> dex_cache_;
};

class HInstanceFieldGet : public HExpression<1> {
 public:
  HInstanceFieldGet(HInstruction* value,
                    Primitive::Type field_type,
                    MemberOffset field_offset,
                    bool is_volatile,
                    uint32_t field_idx,
                    uint16_t declaring_class_def_index,
                    const DexFile& dex_file,
                    Handle<mirror::DexCache> dex_cache,
                    uint32_t dex_pc)
      : HExpression(field_type,
                    SideEffects::FieldReadOfType(field_type, is_volatile),
                    dex_pc),
        field_info_(field_offset,
                    field_type,
                    is_volatile,
                    field_idx,
                    declaring_class_def_index,
                    dex_file,
                    dex_cache) {
    SetRawInputAt(0, value);
  }

  bool CanBeMoved() const OVERRIDE { return !IsVolatile(); }

  bool InstructionDataEquals(HInstruction* other) const OVERRIDE {
    HInstanceFieldGet* other_get = other->AsInstanceFieldGet();
    return GetFieldOffset().SizeValue() == other_get->GetFieldOffset().SizeValue();
  }

  bool CanDoImplicitNullCheckOn(HInstruction* obj) const OVERRIDE {
    return (obj == InputAt(0)) && GetFieldOffset().Uint32Value() < kPageSize;
  }

  size_t ComputeHashCode() const OVERRIDE {
    return (HInstruction::ComputeHashCode() << 7) | GetFieldOffset().SizeValue();
  }

  const FieldInfo& GetFieldInfo() const { return field_info_; }
  MemberOffset GetFieldOffset() const { return field_info_.GetFieldOffset(); }
  Primitive::Type GetFieldType() const { return field_info_.GetFieldType(); }
  bool IsVolatile() const { return field_info_.IsVolatile(); }

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
                    MemberOffset field_offset,
                    bool is_volatile,
                    uint32_t field_idx,
                    uint16_t declaring_class_def_index,
                    const DexFile& dex_file,
                    Handle<mirror::DexCache> dex_cache,
                    uint32_t dex_pc)
      : HTemplateInstruction(SideEffects::FieldWriteOfType(field_type, is_volatile),
                             dex_pc),
        field_info_(field_offset,
                    field_type,
                    is_volatile,
                    field_idx,
                    declaring_class_def_index,
                    dex_file,
                    dex_cache),
        value_can_be_null_(true) {
    SetRawInputAt(0, object);
    SetRawInputAt(1, value);
  }

  bool CanDoImplicitNullCheckOn(HInstruction* obj) const OVERRIDE {
    return (obj == InputAt(0)) && GetFieldOffset().Uint32Value() < kPageSize;
  }

  const FieldInfo& GetFieldInfo() const { return field_info_; }
  MemberOffset GetFieldOffset() const { return field_info_.GetFieldOffset(); }
  Primitive::Type GetFieldType() const { return field_info_.GetFieldType(); }
  bool IsVolatile() const { return field_info_.IsVolatile(); }
  HInstruction* GetValue() const { return InputAt(1); }
  bool GetValueCanBeNull() const { return value_can_be_null_; }
  void ClearValueCanBeNull() { value_can_be_null_ = false; }

  DECLARE_INSTRUCTION(InstanceFieldSet);

 private:
  const FieldInfo field_info_;
  bool value_can_be_null_;

  DISALLOW_COPY_AND_ASSIGN(HInstanceFieldSet);
};

class HArrayGet : public HExpression<2> {
 public:
  HArrayGet(HInstruction* array,
            HInstruction* index,
            Primitive::Type type,
            uint32_t dex_pc,
            SideEffects additional_side_effects = SideEffects::None())
      : HExpression(type,
                    SideEffects::ArrayReadOfType(type).Union(additional_side_effects),
                    dex_pc) {
    SetRawInputAt(0, array);
    SetRawInputAt(1, index);
  }

  bool CanBeMoved() const OVERRIDE { return true; }
  bool InstructionDataEquals(HInstruction* other ATTRIBUTE_UNUSED) const OVERRIDE {
    return true;
  }
  bool CanDoImplicitNullCheckOn(HInstruction* obj ATTRIBUTE_UNUSED) const OVERRIDE {
    // TODO: We can be smarter here.
    // Currently, the array access is always preceded by an ArrayLength or a NullCheck
    // which generates the implicit null check. There are cases when these can be removed
    // to produce better code. If we ever add optimizations to do so we should allow an
    // implicit check here (as long as the address falls in the first page).
    return false;
  }

  bool IsEquivalentOf(HArrayGet* other) const {
    bool result = (GetDexPc() == other->GetDexPc());
    if (kIsDebugBuild && result) {
      DCHECK_EQ(GetBlock(), other->GetBlock());
      DCHECK_EQ(GetArray(), other->GetArray());
      DCHECK_EQ(GetIndex(), other->GetIndex());
      if (Primitive::IsIntOrLongType(GetType())) {
        DCHECK(Primitive::IsFloatingPointType(other->GetType()));
      } else {
        DCHECK(Primitive::IsFloatingPointType(GetType()));
        DCHECK(Primitive::IsIntOrLongType(other->GetType()));
      }
    }
    return result;
  }

  HInstruction* GetArray() const { return InputAt(0); }
  HInstruction* GetIndex() const { return InputAt(1); }

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
            uint32_t dex_pc,
            SideEffects additional_side_effects = SideEffects::None())
      : HTemplateInstruction(
            SideEffects::ArrayWriteOfType(expected_component_type).Union(
                SideEffectsForArchRuntimeCalls(value->GetType())).Union(
                    additional_side_effects),
            dex_pc),
        expected_component_type_(expected_component_type),
        needs_type_check_(value->GetType() == Primitive::kPrimNot),
        value_can_be_null_(true),
        static_type_of_array_is_object_array_(false) {
    SetRawInputAt(0, array);
    SetRawInputAt(1, index);
    SetRawInputAt(2, value);
  }

  bool NeedsEnvironment() const OVERRIDE {
    // We currently always call a runtime method to catch array store
    // exceptions.
    return needs_type_check_;
  }

  // Can throw ArrayStoreException.
  bool CanThrow() const OVERRIDE { return needs_type_check_; }

  bool CanDoImplicitNullCheckOn(HInstruction* obj ATTRIBUTE_UNUSED) const OVERRIDE {
    // TODO: Same as for ArrayGet.
    return false;
  }

  void ClearNeedsTypeCheck() {
    needs_type_check_ = false;
  }

  void ClearValueCanBeNull() {
    value_can_be_null_ = false;
  }

  void SetStaticTypeOfArrayIsObjectArray() {
    static_type_of_array_is_object_array_ = true;
  }

  bool GetValueCanBeNull() const { return value_can_be_null_; }
  bool NeedsTypeCheck() const { return needs_type_check_; }
  bool StaticTypeOfArrayIsObjectArray() const { return static_type_of_array_is_object_array_; }

  HInstruction* GetArray() const { return InputAt(0); }
  HInstruction* GetIndex() const { return InputAt(1); }
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

  Primitive::Type GetRawExpectedComponentType() const {
    return expected_component_type_;
  }

  static SideEffects SideEffectsForArchRuntimeCalls(Primitive::Type value_type) {
    return (value_type == Primitive::kPrimNot) ? SideEffects::CanTriggerGC() : SideEffects::None();
  }

  DECLARE_INSTRUCTION(ArraySet);

 private:
  const Primitive::Type expected_component_type_;
  bool needs_type_check_;
  bool value_can_be_null_;
  // Cached information for the reference_type_info_ so that codegen
  // does not need to inspect the static type.
  bool static_type_of_array_is_object_array_;

  DISALLOW_COPY_AND_ASSIGN(HArraySet);
};

class HArrayLength : public HExpression<1> {
 public:
  HArrayLength(HInstruction* array, uint32_t dex_pc)
      : HExpression(Primitive::kPrimInt, SideEffects::None(), dex_pc) {
    // Note that arrays do not change length, so the instruction does not
    // depend on any write.
    SetRawInputAt(0, array);
  }

  bool CanBeMoved() const OVERRIDE { return true; }
  bool InstructionDataEquals(HInstruction* other ATTRIBUTE_UNUSED) const OVERRIDE {
    return true;
  }
  bool CanDoImplicitNullCheckOn(HInstruction* obj) const OVERRIDE {
    return obj == InputAt(0);
  }

  DECLARE_INSTRUCTION(ArrayLength);

 private:
  DISALLOW_COPY_AND_ASSIGN(HArrayLength);
};

class HBoundsCheck : public HExpression<2> {
 public:
  HBoundsCheck(HInstruction* index, HInstruction* length, uint32_t dex_pc)
      : HExpression(index->GetType(), SideEffects::None(), dex_pc) {
    DCHECK(index->GetType() == Primitive::kPrimInt);
    SetRawInputAt(0, index);
    SetRawInputAt(1, length);
  }

  bool CanBeMoved() const OVERRIDE { return true; }
  bool InstructionDataEquals(HInstruction* other ATTRIBUTE_UNUSED) const OVERRIDE {
    return true;
  }

  bool NeedsEnvironment() const OVERRIDE { return true; }

  bool CanThrow() const OVERRIDE { return true; }

  HInstruction* GetIndex() const { return InputAt(0); }

  DECLARE_INSTRUCTION(BoundsCheck);

 private:
  DISALLOW_COPY_AND_ASSIGN(HBoundsCheck);
};

/**
 * Some DEX instructions are folded into multiple HInstructions that need
 * to stay live until the last HInstruction. This class
 * is used as a marker for the baseline compiler to ensure its preceding
 * HInstruction stays live. `index` represents the stack location index of the
 * instruction (the actual offset is computed as index * vreg_size).
 */
class HTemporary : public HTemplateInstruction<0> {
 public:
  explicit HTemporary(size_t index, uint32_t dex_pc = kNoDexPc)
      : HTemplateInstruction(SideEffects::None(), dex_pc), index_(index) {}

  size_t GetIndex() const { return index_; }

  Primitive::Type GetType() const OVERRIDE {
    // The previous instruction is the one that will be stored in the temporary location.
    DCHECK(GetPrevious() != nullptr);
    return GetPrevious()->GetType();
  }

  DECLARE_INSTRUCTION(Temporary);

 private:
  const size_t index_;
  DISALLOW_COPY_AND_ASSIGN(HTemporary);
};

class HSuspendCheck : public HTemplateInstruction<0> {
 public:
  explicit HSuspendCheck(uint32_t dex_pc)
      : HTemplateInstruction(SideEffects::CanTriggerGC(), dex_pc), slow_path_(nullptr) {}

  bool NeedsEnvironment() const OVERRIDE {
    return true;
  }

  void SetSlowPath(SlowPathCode* slow_path) { slow_path_ = slow_path; }
  SlowPathCode* GetSlowPath() const { return slow_path_; }

  DECLARE_INSTRUCTION(SuspendCheck);

 private:
  // Only used for code generation, in order to share the same slow path between back edges
  // of a same loop.
  SlowPathCode* slow_path_;

  DISALLOW_COPY_AND_ASSIGN(HSuspendCheck);
};

// Pseudo-instruction which provides the native debugger with mapping information.
// It ensures that we can generate line number and local variables at this point.
class HNativeDebugInfo : public HTemplateInstruction<0> {
 public:
  explicit HNativeDebugInfo(uint32_t dex_pc)
      : HTemplateInstruction<0>(SideEffects::None(), dex_pc) {}

  bool NeedsEnvironment() const OVERRIDE {
    return true;
  }

  DECLARE_INSTRUCTION(NativeDebugInfo);

 private:
  DISALLOW_COPY_AND_ASSIGN(HNativeDebugInfo);
};

/**
 * Instruction to load a Class object.
 */
class HLoadClass : public HExpression<1> {
 public:
  HLoadClass(HCurrentMethod* current_method,
             uint16_t type_index,
             const DexFile& dex_file,
             bool is_referrers_class,
             uint32_t dex_pc,
             bool needs_access_check,
             bool is_in_dex_cache)
      : HExpression(Primitive::kPrimNot, SideEffectsForArchRuntimeCalls(), dex_pc),
        type_index_(type_index),
        dex_file_(dex_file),
        is_referrers_class_(is_referrers_class),
        generate_clinit_check_(false),
        needs_access_check_(needs_access_check),
        is_in_dex_cache_(is_in_dex_cache),
        loaded_class_rti_(ReferenceTypeInfo::CreateInvalid()) {
    // Referrers class should not need access check. We never inline unverified
    // methods so we can't possibly end up in this situation.
    DCHECK(!is_referrers_class_ || !needs_access_check_);
    SetRawInputAt(0, current_method);
  }

  bool CanBeMoved() const OVERRIDE { return true; }

  bool InstructionDataEquals(HInstruction* other) const OVERRIDE {
    // Note that we don't need to test for generate_clinit_check_.
    // Whether or not we need to generate the clinit check is processed in
    // prepare_for_register_allocator based on existing HInvokes and HClinitChecks.
    return other->AsLoadClass()->type_index_ == type_index_ &&
        other->AsLoadClass()->needs_access_check_ == needs_access_check_;
  }

  size_t ComputeHashCode() const OVERRIDE { return type_index_; }

  uint16_t GetTypeIndex() const { return type_index_; }
  bool IsReferrersClass() const { return is_referrers_class_; }
  bool CanBeNull() const OVERRIDE { return false; }

  bool NeedsEnvironment() const OVERRIDE {
    return CanCallRuntime();
  }

  bool MustGenerateClinitCheck() const {
    return generate_clinit_check_;
  }

  void SetMustGenerateClinitCheck(bool generate_clinit_check) {
    // The entrypoint the code generator is going to call does not do
    // clinit of the class.
    DCHECK(!NeedsAccessCheck());
    generate_clinit_check_ = generate_clinit_check;
  }

  bool CanCallRuntime() const {
    return MustGenerateClinitCheck() ||
           (!is_referrers_class_ && !is_in_dex_cache_) ||
           needs_access_check_;
  }

  bool NeedsAccessCheck() const {
    return needs_access_check_;
  }

  bool CanThrow() const OVERRIDE {
    return CanCallRuntime();
  }

  ReferenceTypeInfo GetLoadedClassRTI() {
    return loaded_class_rti_;
  }

  void SetLoadedClassRTI(ReferenceTypeInfo rti) {
    // Make sure we only set exact types (the loaded class should never be merged).
    DCHECK(rti.IsExact());
    loaded_class_rti_ = rti;
  }

  const DexFile& GetDexFile() { return dex_file_; }

  bool NeedsDexCacheOfDeclaringClass() const OVERRIDE { return !is_referrers_class_; }

  static SideEffects SideEffectsForArchRuntimeCalls() {
    return SideEffects::CanTriggerGC();
  }

  bool IsInDexCache() const { return is_in_dex_cache_; }

  DECLARE_INSTRUCTION(LoadClass);

 private:
  const uint16_t type_index_;
  const DexFile& dex_file_;
  const bool is_referrers_class_;
  // Whether this instruction must generate the initialization check.
  // Used for code generation.
  bool generate_clinit_check_;
  const bool needs_access_check_;
  const bool is_in_dex_cache_;

  ReferenceTypeInfo loaded_class_rti_;

  DISALLOW_COPY_AND_ASSIGN(HLoadClass);
};

class HLoadString : public HExpression<1> {
 public:
  HLoadString(HCurrentMethod* current_method,
              uint32_t string_index,
              uint32_t dex_pc,
              bool is_in_dex_cache)
      : HExpression(Primitive::kPrimNot, SideEffectsForArchRuntimeCalls(), dex_pc),
        string_index_(string_index),
        is_in_dex_cache_(is_in_dex_cache) {
    SetRawInputAt(0, current_method);
  }

  bool CanBeMoved() const OVERRIDE { return true; }

  bool InstructionDataEquals(HInstruction* other) const OVERRIDE {
    return other->AsLoadString()->string_index_ == string_index_;
  }

  size_t ComputeHashCode() const OVERRIDE { return string_index_; }

  uint32_t GetStringIndex() const { return string_index_; }

  // TODO: Can we deopt or debug when we resolve a string?
  bool NeedsEnvironment() const OVERRIDE { return false; }
  bool NeedsDexCacheOfDeclaringClass() const OVERRIDE { return true; }
  bool CanBeNull() const OVERRIDE { return false; }
  bool IsInDexCache() const { return is_in_dex_cache_; }

  static SideEffects SideEffectsForArchRuntimeCalls() {
    return SideEffects::CanTriggerGC();
  }

  DECLARE_INSTRUCTION(LoadString);

 private:
  const uint32_t string_index_;
  const bool is_in_dex_cache_;

  DISALLOW_COPY_AND_ASSIGN(HLoadString);
};

/**
 * Performs an initialization check on its Class object input.
 */
class HClinitCheck : public HExpression<1> {
 public:
  HClinitCheck(HLoadClass* constant, uint32_t dex_pc)
      : HExpression(
            Primitive::kPrimNot,
            SideEffects::AllChanges(),  // Assume write/read on all fields/arrays.
            dex_pc) {
    SetRawInputAt(0, constant);
  }

  bool CanBeMoved() const OVERRIDE { return true; }
  bool InstructionDataEquals(HInstruction* other ATTRIBUTE_UNUSED) const OVERRIDE {
    return true;
  }

  bool NeedsEnvironment() const OVERRIDE {
    // May call runtime to initialize the class.
    return true;
  }

  bool CanThrow() const OVERRIDE { return true; }

  HLoadClass* GetLoadClass() const { return InputAt(0)->AsLoadClass(); }

  DECLARE_INSTRUCTION(ClinitCheck);

 private:
  DISALLOW_COPY_AND_ASSIGN(HClinitCheck);
};

class HStaticFieldGet : public HExpression<1> {
 public:
  HStaticFieldGet(HInstruction* cls,
                  Primitive::Type field_type,
                  MemberOffset field_offset,
                  bool is_volatile,
                  uint32_t field_idx,
                  uint16_t declaring_class_def_index,
                  const DexFile& dex_file,
                  Handle<mirror::DexCache> dex_cache,
                  uint32_t dex_pc)
      : HExpression(field_type,
                    SideEffects::FieldReadOfType(field_type, is_volatile),
                    dex_pc),
        field_info_(field_offset,
                    field_type,
                    is_volatile,
                    field_idx,
                    declaring_class_def_index,
                    dex_file,
                    dex_cache) {
    SetRawInputAt(0, cls);
  }


  bool CanBeMoved() const OVERRIDE { return !IsVolatile(); }

  bool InstructionDataEquals(HInstruction* other) const OVERRIDE {
    HStaticFieldGet* other_get = other->AsStaticFieldGet();
    return GetFieldOffset().SizeValue() == other_get->GetFieldOffset().SizeValue();
  }

  size_t ComputeHashCode() const OVERRIDE {
    return (HInstruction::ComputeHashCode() << 7) | GetFieldOffset().SizeValue();
  }

  const FieldInfo& GetFieldInfo() const { return field_info_; }
  MemberOffset GetFieldOffset() const { return field_info_.GetFieldOffset(); }
  Primitive::Type GetFieldType() const { return field_info_.GetFieldType(); }
  bool IsVolatile() const { return field_info_.IsVolatile(); }

  DECLARE_INSTRUCTION(StaticFieldGet);

 private:
  const FieldInfo field_info_;

  DISALLOW_COPY_AND_ASSIGN(HStaticFieldGet);
};

class HStaticFieldSet : public HTemplateInstruction<2> {
 public:
  HStaticFieldSet(HInstruction* cls,
                  HInstruction* value,
                  Primitive::Type field_type,
                  MemberOffset field_offset,
                  bool is_volatile,
                  uint32_t field_idx,
                  uint16_t declaring_class_def_index,
                  const DexFile& dex_file,
                  Handle<mirror::DexCache> dex_cache,
                  uint32_t dex_pc)
      : HTemplateInstruction(SideEffects::FieldWriteOfType(field_type, is_volatile),
                             dex_pc),
        field_info_(field_offset,
                    field_type,
                    is_volatile,
                    field_idx,
                    declaring_class_def_index,
                    dex_file,
                    dex_cache),
        value_can_be_null_(true) {
    SetRawInputAt(0, cls);
    SetRawInputAt(1, value);
  }

  const FieldInfo& GetFieldInfo() const { return field_info_; }
  MemberOffset GetFieldOffset() const { return field_info_.GetFieldOffset(); }
  Primitive::Type GetFieldType() const { return field_info_.GetFieldType(); }
  bool IsVolatile() const { return field_info_.IsVolatile(); }

  HInstruction* GetValue() const { return InputAt(1); }
  bool GetValueCanBeNull() const { return value_can_be_null_; }
  void ClearValueCanBeNull() { value_can_be_null_ = false; }

  DECLARE_INSTRUCTION(StaticFieldSet);

 private:
  const FieldInfo field_info_;
  bool value_can_be_null_;

  DISALLOW_COPY_AND_ASSIGN(HStaticFieldSet);
};

class HUnresolvedInstanceFieldGet : public HExpression<1> {
 public:
  HUnresolvedInstanceFieldGet(HInstruction* obj,
                              Primitive::Type field_type,
                              uint32_t field_index,
                              uint32_t dex_pc)
      : HExpression(field_type, SideEffects::AllExceptGCDependency(), dex_pc),
        field_index_(field_index) {
    SetRawInputAt(0, obj);
  }

  bool NeedsEnvironment() const OVERRIDE { return true; }
  bool CanThrow() const OVERRIDE { return true; }

  Primitive::Type GetFieldType() const { return GetType(); }
  uint32_t GetFieldIndex() const { return field_index_; }

  DECLARE_INSTRUCTION(UnresolvedInstanceFieldGet);

 private:
  const uint32_t field_index_;

  DISALLOW_COPY_AND_ASSIGN(HUnresolvedInstanceFieldGet);
};

class HUnresolvedInstanceFieldSet : public HTemplateInstruction<2> {
 public:
  HUnresolvedInstanceFieldSet(HInstruction* obj,
                              HInstruction* value,
                              Primitive::Type field_type,
                              uint32_t field_index,
                              uint32_t dex_pc)
      : HTemplateInstruction(SideEffects::AllExceptGCDependency(), dex_pc),
        field_type_(field_type),
        field_index_(field_index) {
    DCHECK_EQ(field_type, value->GetType());
    SetRawInputAt(0, obj);
    SetRawInputAt(1, value);
  }

  bool NeedsEnvironment() const OVERRIDE { return true; }
  bool CanThrow() const OVERRIDE { return true; }

  Primitive::Type GetFieldType() const { return field_type_; }
  uint32_t GetFieldIndex() const { return field_index_; }

  DECLARE_INSTRUCTION(UnresolvedInstanceFieldSet);

 private:
  const Primitive::Type field_type_;
  const uint32_t field_index_;

  DISALLOW_COPY_AND_ASSIGN(HUnresolvedInstanceFieldSet);
};

class HUnresolvedStaticFieldGet : public HExpression<0> {
 public:
  HUnresolvedStaticFieldGet(Primitive::Type field_type,
                            uint32_t field_index,
                            uint32_t dex_pc)
      : HExpression(field_type, SideEffects::AllExceptGCDependency(), dex_pc),
        field_index_(field_index) {
  }

  bool NeedsEnvironment() const OVERRIDE { return true; }
  bool CanThrow() const OVERRIDE { return true; }

  Primitive::Type GetFieldType() const { return GetType(); }
  uint32_t GetFieldIndex() const { return field_index_; }

  DECLARE_INSTRUCTION(UnresolvedStaticFieldGet);

 private:
  const uint32_t field_index_;

  DISALLOW_COPY_AND_ASSIGN(HUnresolvedStaticFieldGet);
};

class HUnresolvedStaticFieldSet : public HTemplateInstruction<1> {
 public:
  HUnresolvedStaticFieldSet(HInstruction* value,
                            Primitive::Type field_type,
                            uint32_t field_index,
                            uint32_t dex_pc)
      : HTemplateInstruction(SideEffects::AllExceptGCDependency(), dex_pc),
        field_type_(field_type),
        field_index_(field_index) {
    DCHECK_EQ(field_type, value->GetType());
    SetRawInputAt(0, value);
  }

  bool NeedsEnvironment() const OVERRIDE { return true; }
  bool CanThrow() const OVERRIDE { return true; }

  Primitive::Type GetFieldType() const { return field_type_; }
  uint32_t GetFieldIndex() const { return field_index_; }

  DECLARE_INSTRUCTION(UnresolvedStaticFieldSet);

 private:
  const Primitive::Type field_type_;
  const uint32_t field_index_;

  DISALLOW_COPY_AND_ASSIGN(HUnresolvedStaticFieldSet);
};

// Implement the move-exception DEX instruction.
class HLoadException : public HExpression<0> {
 public:
  explicit HLoadException(uint32_t dex_pc = kNoDexPc)
      : HExpression(Primitive::kPrimNot, SideEffects::None(), dex_pc) {}

  bool CanBeNull() const OVERRIDE { return false; }

  DECLARE_INSTRUCTION(LoadException);

 private:
  DISALLOW_COPY_AND_ASSIGN(HLoadException);
};

// Implicit part of move-exception which clears thread-local exception storage.
// Must not be removed because the runtime expects the TLS to get cleared.
class HClearException : public HTemplateInstruction<0> {
 public:
  explicit HClearException(uint32_t dex_pc = kNoDexPc)
      : HTemplateInstruction(SideEffects::AllWrites(), dex_pc) {}

  DECLARE_INSTRUCTION(ClearException);

 private:
  DISALLOW_COPY_AND_ASSIGN(HClearException);
};

class HThrow : public HTemplateInstruction<1> {
 public:
  HThrow(HInstruction* exception, uint32_t dex_pc)
      : HTemplateInstruction(SideEffects::CanTriggerGC(), dex_pc) {
    SetRawInputAt(0, exception);
  }

  bool IsControlFlow() const OVERRIDE { return true; }

  bool NeedsEnvironment() const OVERRIDE { return true; }

  bool CanThrow() const OVERRIDE { return true; }


  DECLARE_INSTRUCTION(Throw);

 private:
  DISALLOW_COPY_AND_ASSIGN(HThrow);
};

/**
 * Implementation strategies for the code generator of a HInstanceOf
 * or `HCheckCast`.
 */
enum class TypeCheckKind {
  kUnresolvedCheck,       // Check against an unresolved type.
  kExactCheck,            // Can do a single class compare.
  kClassHierarchyCheck,   // Can just walk the super class chain.
  kAbstractClassCheck,    // Can just walk the super class chain, starting one up.
  kInterfaceCheck,        // No optimization yet when checking against an interface.
  kArrayObjectCheck,      // Can just check if the array is not primitive.
  kArrayCheck             // No optimization yet when checking against a generic array.
};

class HInstanceOf : public HExpression<2> {
 public:
  HInstanceOf(HInstruction* object,
              HLoadClass* constant,
              TypeCheckKind check_kind,
              uint32_t dex_pc)
      : HExpression(Primitive::kPrimBoolean,
                    SideEffectsForArchRuntimeCalls(check_kind),
                    dex_pc),
        check_kind_(check_kind),
        must_do_null_check_(true) {
    SetRawInputAt(0, object);
    SetRawInputAt(1, constant);
  }

  bool CanBeMoved() const OVERRIDE { return true; }

  bool InstructionDataEquals(HInstruction* other ATTRIBUTE_UNUSED) const OVERRIDE {
    return true;
  }

  bool NeedsEnvironment() const OVERRIDE {
    return false;
  }

  bool IsExactCheck() const { return check_kind_ == TypeCheckKind::kExactCheck; }

  TypeCheckKind GetTypeCheckKind() const { return check_kind_; }

  // Used only in code generation.
  bool MustDoNullCheck() const { return must_do_null_check_; }
  void ClearMustDoNullCheck() { must_do_null_check_ = false; }

  static SideEffects SideEffectsForArchRuntimeCalls(TypeCheckKind check_kind) {
    return (check_kind == TypeCheckKind::kExactCheck)
        ? SideEffects::None()
        // Mips currently does runtime calls for any other checks.
        : SideEffects::CanTriggerGC();
  }

  DECLARE_INSTRUCTION(InstanceOf);

 private:
  const TypeCheckKind check_kind_;
  bool must_do_null_check_;

  DISALLOW_COPY_AND_ASSIGN(HInstanceOf);
};

class HBoundType : public HExpression<1> {
 public:
  // Constructs an HBoundType with the given upper_bound.
  // Ensures that the upper_bound is valid.
  HBoundType(HInstruction* input,
             ReferenceTypeInfo upper_bound,
             bool upper_can_be_null,
             uint32_t dex_pc = kNoDexPc)
      : HExpression(Primitive::kPrimNot, SideEffects::None(), dex_pc),
        upper_bound_(upper_bound),
        upper_can_be_null_(upper_can_be_null),
        can_be_null_(upper_can_be_null) {
    DCHECK_EQ(input->GetType(), Primitive::kPrimNot);
    SetRawInputAt(0, input);
    SetReferenceTypeInfo(upper_bound_);
  }

  // GetUpper* should only be used in reference type propagation.
  const ReferenceTypeInfo& GetUpperBound() const { return upper_bound_; }
  bool GetUpperCanBeNull() const { return upper_can_be_null_; }

  void SetCanBeNull(bool can_be_null) {
    DCHECK(upper_can_be_null_ || !can_be_null);
    can_be_null_ = can_be_null;
  }

  bool CanBeNull() const OVERRIDE { return can_be_null_; }

  DECLARE_INSTRUCTION(BoundType);

 private:
  // Encodes the most upper class that this instruction can have. In other words
  // it is always the case that GetUpperBound().IsSupertypeOf(GetReferenceType()).
  // It is used to bound the type in cases like:
  //   if (x instanceof ClassX) {
  //     // uper_bound_ will be ClassX
  //   }
  const ReferenceTypeInfo upper_bound_;
  // Represents the top constraint that can_be_null_ cannot exceed (i.e. if this
  // is false then can_be_null_ cannot be true).
  const bool upper_can_be_null_;
  bool can_be_null_;

  DISALLOW_COPY_AND_ASSIGN(HBoundType);
};

class HCheckCast : public HTemplateInstruction<2> {
 public:
  HCheckCast(HInstruction* object,
             HLoadClass* constant,
             TypeCheckKind check_kind,
             uint32_t dex_pc)
      : HTemplateInstruction(SideEffects::CanTriggerGC(), dex_pc),
        check_kind_(check_kind),
        must_do_null_check_(true) {
    SetRawInputAt(0, object);
    SetRawInputAt(1, constant);
  }

  bool CanBeMoved() const OVERRIDE { return true; }

  bool InstructionDataEquals(HInstruction* other ATTRIBUTE_UNUSED) const OVERRIDE {
    return true;
  }

  bool NeedsEnvironment() const OVERRIDE {
    // Instruction may throw a CheckCastError.
    return true;
  }

  bool CanThrow() const OVERRIDE { return true; }

  bool MustDoNullCheck() const { return must_do_null_check_; }
  void ClearMustDoNullCheck() { must_do_null_check_ = false; }
  TypeCheckKind GetTypeCheckKind() const { return check_kind_; }

  bool IsExactCheck() const { return check_kind_ == TypeCheckKind::kExactCheck; }

  DECLARE_INSTRUCTION(CheckCast);

 private:
  const TypeCheckKind check_kind_;
  bool must_do_null_check_;

  DISALLOW_COPY_AND_ASSIGN(HCheckCast);
};

class HMemoryBarrier : public HTemplateInstruction<0> {
 public:
  explicit HMemoryBarrier(MemBarrierKind barrier_kind, uint32_t dex_pc = kNoDexPc)
      : HTemplateInstruction(
            SideEffects::AllWritesAndReads(), dex_pc),  // Assume write/read on all fields/arrays.
        barrier_kind_(barrier_kind) {}

  MemBarrierKind GetBarrierKind() { return barrier_kind_; }

  DECLARE_INSTRUCTION(MemoryBarrier);

 private:
  const MemBarrierKind barrier_kind_;

  DISALLOW_COPY_AND_ASSIGN(HMemoryBarrier);
};

class HMonitorOperation : public HTemplateInstruction<1> {
 public:
  enum OperationKind {
    kEnter,
    kExit,
  };

  HMonitorOperation(HInstruction* object, OperationKind kind, uint32_t dex_pc)
    : HTemplateInstruction(
          SideEffects::AllExceptGCDependency(), dex_pc),  // Assume write/read on all fields/arrays.
      kind_(kind) {
    SetRawInputAt(0, object);
  }

  // Instruction may throw a Java exception, so we need an environment.
  bool NeedsEnvironment() const OVERRIDE { return CanThrow(); }

  bool CanThrow() const OVERRIDE {
    // Verifier guarantees that monitor-exit cannot throw.
    // This is important because it allows the HGraphBuilder to remove
    // a dead throw-catch loop generated for `synchronized` blocks/methods.
    return IsEnter();
  }


  bool IsEnter() const { return kind_ == kEnter; }

  DECLARE_INSTRUCTION(MonitorOperation);

 private:
  const OperationKind kind_;

 private:
  DISALLOW_COPY_AND_ASSIGN(HMonitorOperation);
};

/**
 * A HInstruction used as a marker for the replacement of new + <init>
 * of a String to a call to a StringFactory. Only baseline will see
 * the node at code generation, where it will be be treated as null.
 * When compiling non-baseline, `HFakeString` instructions are being removed
 * in the instruction simplifier.
 */
class HFakeString : public HTemplateInstruction<0> {
 public:
  explicit HFakeString(uint32_t dex_pc = kNoDexPc)
      : HTemplateInstruction(SideEffects::None(), dex_pc) {}

  Primitive::Type GetType() const OVERRIDE { return Primitive::kPrimNot; }

  DECLARE_INSTRUCTION(FakeString);

 private:
  DISALLOW_COPY_AND_ASSIGN(HFakeString);
};

class MoveOperands : public ArenaObject<kArenaAllocMoveOperands> {
 public:
  MoveOperands(Location source,
               Location destination,
               Primitive::Type type,
               HInstruction* instruction)
      : source_(source), destination_(destination), type_(type), instruction_(instruction) {}

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
    return !IsEliminated() && source_.OverlapsWith(loc);
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

  Primitive::Type GetType() const { return type_; }

  bool Is64BitMove() const {
    return Primitive::Is64BitType(type_);
  }

  HInstruction* GetInstruction() const { return instruction_; }

 private:
  Location source_;
  Location destination_;
  // The type this move is for.
  Primitive::Type type_;
  // The instruction this move is assocatied with. Null when this move is
  // for moving an input in the expected locations of user (including a phi user).
  // This is only used in debug mode, to ensure we do not connect interval siblings
  // in the same parallel move.
  HInstruction* instruction_;
};

static constexpr size_t kDefaultNumberOfMoves = 4;

class HParallelMove : public HTemplateInstruction<0> {
 public:
  explicit HParallelMove(ArenaAllocator* arena, uint32_t dex_pc = kNoDexPc)
      : HTemplateInstruction(SideEffects::None(), dex_pc),
        moves_(arena->Adapter(kArenaAllocMoveOperands)) {
    moves_.reserve(kDefaultNumberOfMoves);
  }

  void AddMove(Location source,
               Location destination,
               Primitive::Type type,
               HInstruction* instruction) {
    DCHECK(source.IsValid());
    DCHECK(destination.IsValid());
    if (kIsDebugBuild) {
      if (instruction != nullptr) {
        for (const MoveOperands& move : moves_) {
          if (move.GetInstruction() == instruction) {
            // Special case the situation where the move is for the spill slot
            // of the instruction.
            if ((GetPrevious() == instruction)
                || ((GetPrevious() == nullptr)
                    && instruction->IsPhi()
                    && instruction->GetBlock() == GetBlock())) {
              DCHECK_NE(destination.GetKind(), move.GetDestination().GetKind())
                  << "Doing parallel moves for the same instruction.";
            } else {
              DCHECK(false) << "Doing parallel moves for the same instruction.";
            }
          }
        }
      }
      for (const MoveOperands& move : moves_) {
        DCHECK(!destination.OverlapsWith(move.GetDestination()))
            << "Overlapped destination for two moves in a parallel move: "
            << move.GetSource() << " ==> " << move.GetDestination() << " and "
            << source << " ==> " << destination;
      }
    }
    moves_.emplace_back(source, destination, type, instruction);
  }

  MoveOperands* MoveOperandsAt(size_t index) {
    return &moves_[index];
  }

  size_t NumMoves() const { return moves_.size(); }

  DECLARE_INSTRUCTION(ParallelMove);

 private:
  ArenaVector<MoveOperands> moves_;

  DISALLOW_COPY_AND_ASSIGN(HParallelMove);
};

}  // namespace art

#ifdef ART_ENABLE_CODEGEN_arm
#include "nodes_arm.h"
#endif
#ifdef ART_ENABLE_CODEGEN_arm64
#include "nodes_arm64.h"
#endif
#ifdef ART_ENABLE_CODEGEN_x86
#include "nodes_x86.h"
#endif

namespace art {

class HGraphVisitor : public ValueObject {
 public:
  explicit HGraphVisitor(HGraph* graph) : graph_(graph) {}
  virtual ~HGraphVisitor() {}

  virtual void VisitInstruction(HInstruction* instruction ATTRIBUTE_UNUSED) {}
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
  void Visit##name(H##name* instr) OVERRIDE { Visit##super(instr); }

  FOR_EACH_INSTRUCTION(DECLARE_VISIT_INSTRUCTION)

#undef DECLARE_VISIT_INSTRUCTION

 private:
  DISALLOW_COPY_AND_ASSIGN(HGraphDelegateVisitor);
};

class HInsertionOrderIterator : public ValueObject {
 public:
  explicit HInsertionOrderIterator(const HGraph& graph) : graph_(graph), index_(0) {}

  bool Done() const { return index_ == graph_.GetBlocks().size(); }
  HBasicBlock* Current() const { return graph_.GetBlocks()[index_]; }
  void Advance() { ++index_; }

 private:
  const HGraph& graph_;
  size_t index_;

  DISALLOW_COPY_AND_ASSIGN(HInsertionOrderIterator);
};

class HReversePostOrderIterator : public ValueObject {
 public:
  explicit HReversePostOrderIterator(const HGraph& graph) : graph_(graph), index_(0) {
    // Check that reverse post order of the graph has been built.
    DCHECK(!graph.GetReversePostOrder().empty());
  }

  bool Done() const { return index_ == graph_.GetReversePostOrder().size(); }
  HBasicBlock* Current() const { return graph_.GetReversePostOrder()[index_]; }
  void Advance() { ++index_; }

 private:
  const HGraph& graph_;
  size_t index_;

  DISALLOW_COPY_AND_ASSIGN(HReversePostOrderIterator);
};

class HPostOrderIterator : public ValueObject {
 public:
  explicit HPostOrderIterator(const HGraph& graph)
      : graph_(graph), index_(graph_.GetReversePostOrder().size()) {
    // Check that reverse post order of the graph has been built.
    DCHECK(!graph.GetReversePostOrder().empty());
  }

  bool Done() const { return index_ == 0; }
  HBasicBlock* Current() const { return graph_.GetReversePostOrder()[index_ - 1u]; }
  void Advance() { --index_; }

 private:
  const HGraph& graph_;
  size_t index_;

  DISALLOW_COPY_AND_ASSIGN(HPostOrderIterator);
};

class HLinearPostOrderIterator : public ValueObject {
 public:
  explicit HLinearPostOrderIterator(const HGraph& graph)
      : order_(graph.GetLinearOrder()), index_(graph.GetLinearOrder().size()) {}

  bool Done() const { return index_ == 0; }

  HBasicBlock* Current() const { return order_[index_ - 1u]; }

  void Advance() {
    --index_;
    DCHECK_GE(index_, 0U);
  }

 private:
  const ArenaVector<HBasicBlock*>& order_;
  size_t index_;

  DISALLOW_COPY_AND_ASSIGN(HLinearPostOrderIterator);
};

class HLinearOrderIterator : public ValueObject {
 public:
  explicit HLinearOrderIterator(const HGraph& graph)
      : order_(graph.GetLinearOrder()), index_(0) {}

  bool Done() const { return index_ == order_.size(); }
  HBasicBlock* Current() const { return order_[index_]; }
  void Advance() { ++index_; }

 private:
  const ArenaVector<HBasicBlock*>& order_;
  size_t index_;

  DISALLOW_COPY_AND_ASSIGN(HLinearOrderIterator);
};

// Iterator over the blocks that art part of the loop. Includes blocks part
// of an inner loop. The order in which the blocks are iterated is on their
// block id.
class HBlocksInLoopIterator : public ValueObject {
 public:
  explicit HBlocksInLoopIterator(const HLoopInformation& info)
      : blocks_in_loop_(info.GetBlocks()),
        blocks_(info.GetHeader()->GetGraph()->GetBlocks()),
        index_(0) {
    if (!blocks_in_loop_.IsBitSet(index_)) {
      Advance();
    }
  }

  bool Done() const { return index_ == blocks_.size(); }
  HBasicBlock* Current() const { return blocks_[index_]; }
  void Advance() {
    ++index_;
    for (size_t e = blocks_.size(); index_ < e; ++index_) {
      if (blocks_in_loop_.IsBitSet(index_)) {
        break;
      }
    }
  }

 private:
  const BitVector& blocks_in_loop_;
  const ArenaVector<HBasicBlock*>& blocks_;
  size_t index_;

  DISALLOW_COPY_AND_ASSIGN(HBlocksInLoopIterator);
};

// Iterator over the blocks that art part of the loop. Includes blocks part
// of an inner loop. The order in which the blocks are iterated is reverse
// post order.
class HBlocksInLoopReversePostOrderIterator : public ValueObject {
 public:
  explicit HBlocksInLoopReversePostOrderIterator(const HLoopInformation& info)
      : blocks_in_loop_(info.GetBlocks()),
        blocks_(info.GetHeader()->GetGraph()->GetReversePostOrder()),
        index_(0) {
    if (!blocks_in_loop_.IsBitSet(blocks_[index_]->GetBlockId())) {
      Advance();
    }
  }

  bool Done() const { return index_ == blocks_.size(); }
  HBasicBlock* Current() const { return blocks_[index_]; }
  void Advance() {
    ++index_;
    for (size_t e = blocks_.size(); index_ < e; ++index_) {
      if (blocks_in_loop_.IsBitSet(blocks_[index_]->GetBlockId())) {
        break;
      }
    }
  }

 private:
  const BitVector& blocks_in_loop_;
  const ArenaVector<HBasicBlock*>& blocks_;
  size_t index_;

  DISALLOW_COPY_AND_ASSIGN(HBlocksInLoopReversePostOrderIterator);
};

inline int64_t Int64FromConstant(HConstant* constant) {
  DCHECK(constant->IsIntConstant() || constant->IsLongConstant());
  return constant->IsIntConstant() ? constant->AsIntConstant()->GetValue()
                                   : constant->AsLongConstant()->GetValue();
}

inline bool IsSameDexFile(const DexFile& lhs, const DexFile& rhs) {
  // For the purposes of the compiler, the dex files must actually be the same object
  // if we want to safely treat them as the same. This is especially important for JIT
  // as custom class loaders can open the same underlying file (or memory) multiple
  // times and provide different class resolution but no two class loaders should ever
  // use the same DexFile object - doing so is an unsupported hack that can lead to
  // all sorts of weird failures.
  return &lhs == &rhs;
}

#define INSTRUCTION_TYPE_CHECK(type, super)                                    \
  inline bool HInstruction::Is##type() const { return GetKind() == k##type; }  \
  inline const H##type* HInstruction::As##type() const {                       \
    return Is##type() ? down_cast<const H##type*>(this) : nullptr;             \
  }                                                                            \
  inline H##type* HInstruction::As##type() {                                   \
    return Is##type() ? static_cast<H##type*>(this) : nullptr;                 \
  }

  FOR_EACH_CONCRETE_INSTRUCTION(INSTRUCTION_TYPE_CHECK)
#undef INSTRUCTION_TYPE_CHECK

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_NODES_H_
