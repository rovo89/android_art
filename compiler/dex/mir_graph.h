/*
 * Copyright (C) 2013 The Android Open Source Project
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

#ifndef ART_COMPILER_DEX_MIR_GRAPH_H_
#define ART_COMPILER_DEX_MIR_GRAPH_H_

#include <stdint.h>

#include "base/arena_containers.h"
#include "base/bit_utils.h"
#include "base/scoped_arena_containers.h"
#include "dex_file.h"
#include "dex_instruction.h"
#include "dex_types.h"
#include "invoke_type.h"
#include "mir_field_info.h"
#include "mir_method_info.h"
#include "reg_location.h"
#include "reg_storage.h"
#include "utils/arena_bit_vector.h"

namespace art {

struct CompilationUnit;
class DexCompilationUnit;
class DexFileMethodInliner;
class GlobalValueNumbering;
class GvnDeadCodeElimination;
class PassManager;
class TypeInference;

// Forward declaration.
class MIRGraph;

enum DataFlowAttributePos {
  kUA = 0,
  kUB,
  kUC,
  kAWide,
  kBWide,
  kCWide,
  kDA,
  kIsMove,
  kSetsConst,
  kFormat35c,
  kFormat3rc,
  kFormatExtended,       // Extended format for extended MIRs.
  kNullCheckA,           // Null check of A.
  kNullCheckB,           // Null check of B.
  kNullCheckOut0,        // Null check out outgoing arg0.
  kDstNonNull,           // May assume dst is non-null.
  kRetNonNull,           // May assume retval is non-null.
  kNullTransferSrc0,     // Object copy src[0] -> dst.
  kNullTransferSrcN,     // Phi null check state transfer.
  kRangeCheckC,          // Range check of C.
  kCheckCastA,           // Check cast of A.
  kFPA,
  kFPB,
  kFPC,
  kCoreA,
  kCoreB,
  kCoreC,
  kRefA,
  kRefB,
  kRefC,
  kSameTypeAB,           // A and B have the same type but it can be core/ref/fp (IF_cc).
  kUsesMethodStar,       // Implicit use of Method*.
  kUsesIField,           // Accesses an instance field (IGET/IPUT).
  kUsesSField,           // Accesses a static field (SGET/SPUT).
  kCanInitializeClass,   // Can trigger class initialization (SGET/SPUT/INVOKE_STATIC).
  kDoLVN,                // Worth computing local value numbers.
};

#define DF_NOP                  UINT64_C(0)
#define DF_UA                   (UINT64_C(1) << kUA)
#define DF_UB                   (UINT64_C(1) << kUB)
#define DF_UC                   (UINT64_C(1) << kUC)
#define DF_A_WIDE               (UINT64_C(1) << kAWide)
#define DF_B_WIDE               (UINT64_C(1) << kBWide)
#define DF_C_WIDE               (UINT64_C(1) << kCWide)
#define DF_DA                   (UINT64_C(1) << kDA)
#define DF_IS_MOVE              (UINT64_C(1) << kIsMove)
#define DF_SETS_CONST           (UINT64_C(1) << kSetsConst)
#define DF_FORMAT_35C           (UINT64_C(1) << kFormat35c)
#define DF_FORMAT_3RC           (UINT64_C(1) << kFormat3rc)
#define DF_FORMAT_EXTENDED      (UINT64_C(1) << kFormatExtended)
#define DF_NULL_CHK_A           (UINT64_C(1) << kNullCheckA)
#define DF_NULL_CHK_B           (UINT64_C(1) << kNullCheckB)
#define DF_NULL_CHK_OUT0        (UINT64_C(1) << kNullCheckOut0)
#define DF_NON_NULL_DST         (UINT64_C(1) << kDstNonNull)
#define DF_NON_NULL_RET         (UINT64_C(1) << kRetNonNull)
#define DF_NULL_TRANSFER_0      (UINT64_C(1) << kNullTransferSrc0)
#define DF_NULL_TRANSFER_N      (UINT64_C(1) << kNullTransferSrcN)
#define DF_RANGE_CHK_C          (UINT64_C(1) << kRangeCheckC)
#define DF_CHK_CAST             (UINT64_C(1) << kCheckCastA)
#define DF_FP_A                 (UINT64_C(1) << kFPA)
#define DF_FP_B                 (UINT64_C(1) << kFPB)
#define DF_FP_C                 (UINT64_C(1) << kFPC)
#define DF_CORE_A               (UINT64_C(1) << kCoreA)
#define DF_CORE_B               (UINT64_C(1) << kCoreB)
#define DF_CORE_C               (UINT64_C(1) << kCoreC)
#define DF_REF_A                (UINT64_C(1) << kRefA)
#define DF_REF_B                (UINT64_C(1) << kRefB)
#define DF_REF_C                (UINT64_C(1) << kRefC)
#define DF_SAME_TYPE_AB         (UINT64_C(1) << kSameTypeAB)
#define DF_UMS                  (UINT64_C(1) << kUsesMethodStar)
#define DF_IFIELD               (UINT64_C(1) << kUsesIField)
#define DF_SFIELD               (UINT64_C(1) << kUsesSField)
#define DF_CLINIT               (UINT64_C(1) << kCanInitializeClass)
#define DF_LVN                  (UINT64_C(1) << kDoLVN)

#define DF_HAS_USES             (DF_UA | DF_UB | DF_UC)

#define DF_HAS_DEFS             (DF_DA)

#define DF_HAS_NULL_CHKS        (DF_NULL_CHK_A | \
                                 DF_NULL_CHK_B | \
                                 DF_NULL_CHK_OUT0)

#define DF_HAS_RANGE_CHKS       (DF_RANGE_CHK_C)

#define DF_HAS_NR_CHKS          (DF_HAS_NULL_CHKS | \
                                 DF_HAS_RANGE_CHKS)

#define DF_A_IS_REG             (DF_UA | DF_DA)
#define DF_B_IS_REG             (DF_UB)
#define DF_C_IS_REG             (DF_UC)
#define DF_USES_FP              (DF_FP_A | DF_FP_B | DF_FP_C)
#define DF_NULL_TRANSFER        (DF_NULL_TRANSFER_0 | DF_NULL_TRANSFER_N)
#define DF_IS_INVOKE            (DF_FORMAT_35C | DF_FORMAT_3RC)

enum OatMethodAttributes {
  kIsLeaf,            // Method is leaf.
};

#define METHOD_IS_LEAF          (1 << kIsLeaf)

// Minimum field size to contain Dalvik v_reg number.
#define VREG_NUM_WIDTH 16

#define INVALID_VREG (0xFFFFU)
#define INVALID_OFFSET (0xDEADF00FU)

#define MIR_IGNORE_NULL_CHECK           (1 << kMIRIgnoreNullCheck)
#define MIR_IGNORE_RANGE_CHECK          (1 << kMIRIgnoreRangeCheck)
#define MIR_IGNORE_CHECK_CAST           (1 << kMIRIgnoreCheckCast)
#define MIR_STORE_NON_NULL_VALUE        (1 << kMIRStoreNonNullValue)
#define MIR_CLASS_IS_INITIALIZED        (1 << kMIRClassIsInitialized)
#define MIR_CLASS_IS_IN_DEX_CACHE       (1 << kMIRClassIsInDexCache)
#define MIR_IGNORE_DIV_ZERO_CHECK       (1 << kMirIgnoreDivZeroCheck)
#define MIR_INLINED                     (1 << kMIRInlined)
#define MIR_INLINED_PRED                (1 << kMIRInlinedPred)
#define MIR_CALLEE                      (1 << kMIRCallee)
#define MIR_IGNORE_SUSPEND_CHECK        (1 << kMIRIgnoreSuspendCheck)
#define MIR_DUP                         (1 << kMIRDup)
#define MIR_MARK                        (1 << kMIRMark)
#define MIR_STORE_NON_TEMPORAL          (1 << kMIRStoreNonTemporal)

#define BLOCK_NAME_LEN 80

typedef uint16_t BasicBlockId;
static const BasicBlockId NullBasicBlockId = 0;

// Leaf optimization is basically the removal of suspend checks from leaf methods.
// This is incompatible with SuspendCheckElimination (SCE) which eliminates suspend
// checks from loops that call any non-intrinsic method, since a loop that calls
// only a leaf method would end up without any suspend checks at all. So turning
// this on automatically disables the SCE in MIRGraph::EliminateSuspendChecksGate().
//
// Since the Optimizing compiler is actually applying the same optimization, Quick
// must not run SCE anyway, so we enable this optimization as a way to disable SCE
// while keeping a consistent behavior across the backends, b/22657404.
static constexpr bool kLeafOptimization = true;

/*
 * In general, vreg/sreg describe Dalvik registers that originated with dx.  However,
 * it is useful to have compiler-generated temporary registers and have them treated
 * in the same manner as dx-generated virtual registers.  This struct records the SSA
 * name of compiler-introduced temporaries.
 */
struct CompilerTemp {
  int32_t v_reg;      // Virtual register number for temporary.
  int32_t s_reg_low;  // SSA name for low Dalvik word.
};

enum CompilerTempType {
  kCompilerTempVR,                // A virtual register temporary.
  kCompilerTempSpecialMethodPtr,  // Temporary that keeps track of current method pointer.
  kCompilerTempBackend,           // Temporary that is used by backend.
};

// When debug option enabled, records effectiveness of null and range check elimination.
struct Checkstats {
  int32_t null_checks;
  int32_t null_checks_eliminated;
  int32_t range_checks;
  int32_t range_checks_eliminated;
};

// Dataflow attributes of a basic block.
struct BasicBlockDataFlow {
  ArenaBitVector* use_v;
  ArenaBitVector* def_v;
  ArenaBitVector* live_in_v;
  int32_t* vreg_to_ssa_map_exit;
};

/*
 * Normalized use/def for a MIR operation using SSA names rather than vregs.  Note that
 * uses/defs retain the Dalvik convention that long operations operate on a pair of 32-bit
 * vregs.  For example, "ADD_LONG v0, v2, v3" would have 2 defs (v0/v1) and 4 uses (v2/v3, v4/v5).
 * Following SSA renaming, this is the primary struct used by code generators to locate
 * operand and result registers.  This is a somewhat confusing and unhelpful convention that
 * we may want to revisit in the future.
 *
 * TODO:
 *  1. Add accessors for uses/defs and make data private
 *  2. Change fp_use/fp_def to a bit array (could help memory usage)
 *  3. Combine array storage into internal array and handled via accessors from 1.
 */
struct SSARepresentation {
  int32_t* uses;
  int32_t* defs;
  uint16_t num_uses_allocated;
  uint16_t num_defs_allocated;
  uint16_t num_uses;
  uint16_t num_defs;

  static uint32_t GetStartUseIndex(Instruction::Code opcode);
};

/*
 * The Midlevel Intermediate Representation node, which may be largely considered a
 * wrapper around a Dalvik byte code.
 */
class MIR : public ArenaObject<kArenaAllocMIR> {
 public:
  /*
   * TODO: remove embedded DecodedInstruction to save space, keeping only opcode.  Recover
   * additional fields on as-needed basis.  Question: how to support MIR Pseudo-ops; probably
   * need to carry aux data pointer.
   */
  struct DecodedInstruction {
    uint32_t vA;
    uint32_t vB;
    uint64_t vB_wide;        /* for k51l */
    uint32_t vC;
    uint32_t arg[5];         /* vC/D/E/F/G in invoke or filled-new-array */
    Instruction::Code opcode;

    explicit DecodedInstruction():vA(0), vB(0), vB_wide(0), vC(0), opcode(Instruction::NOP) {
    }

    /*
     * Given a decoded instruction representing a const bytecode, it updates
     * the out arguments with proper values as dictated by the constant bytecode.
     */
    bool GetConstant(int64_t* ptr_value, bool* wide) const;

    static bool IsPseudoMirOp(Instruction::Code opcode) {
      return static_cast<int>(opcode) >= static_cast<int>(kMirOpFirst);
    }

    static bool IsPseudoMirOp(int opcode) {
      return opcode >= static_cast<int>(kMirOpFirst);
    }

    bool IsInvoke() const {
      return ((FlagsOf() & Instruction::kInvoke) == Instruction::kInvoke);
    }

    bool IsStore() const {
      return ((FlagsOf() & Instruction::kStore) == Instruction::kStore);
    }

    bool IsLoad() const {
      return ((FlagsOf() & Instruction::kLoad) == Instruction::kLoad);
    }

    bool IsConditionalBranch() const {
      return (FlagsOf() == (Instruction::kContinue | Instruction::kBranch));
    }

    /**
     * @brief Is the register C component of the decoded instruction a constant?
     */
    bool IsCFieldOrConstant() const {
      return ((FlagsOf() & Instruction::kRegCFieldOrConstant) == Instruction::kRegCFieldOrConstant);
    }

    /**
     * @brief Is the register C component of the decoded instruction a constant?
     */
    bool IsBFieldOrConstant() const {
      return ((FlagsOf() & Instruction::kRegBFieldOrConstant) == Instruction::kRegBFieldOrConstant);
    }

    bool IsCast() const {
      return ((FlagsOf() & Instruction::kCast) == Instruction::kCast);
    }

    /**
     * @brief Does the instruction clobber memory?
     * @details Clobber means that the instruction changes the memory not in a punctual way.
     *          Therefore any supposition on memory aliasing or memory contents should be disregarded
     *            when crossing such an instruction.
     */
    bool Clobbers() const {
      return ((FlagsOf() & Instruction::kClobber) == Instruction::kClobber);
    }

    bool IsLinear() const {
      return (FlagsOf() & (Instruction::kAdd | Instruction::kSubtract)) != 0;
    }

    int FlagsOf() const;
  } dalvikInsn;

  NarrowDexOffset offset;         // Offset of the instruction in code units.
  uint16_t optimization_flags;
  int16_t m_unit_index;           // From which method was this MIR included
  BasicBlockId bb;
  MIR* next;
  SSARepresentation* ssa_rep;
  union {
    // Incoming edges for phi node.
    BasicBlockId* phi_incoming;
    // Establish link from check instruction (kMirOpCheck) to the actual throwing instruction.
    MIR* throw_insn;
    // Branch condition for fused cmp or select.
    ConditionCode ccode;
    // IGET/IPUT lowering info index, points to MIRGraph::ifield_lowering_infos_. Due to limit on
    // the number of code points (64K) and size of IGET/IPUT insn (2), this will never exceed 32K.
    uint32_t ifield_lowering_info;
    // SGET/SPUT lowering info index, points to MIRGraph::sfield_lowering_infos_. Due to limit on
    // the number of code points (64K) and size of SGET/SPUT insn (2), this will never exceed 32K.
    uint32_t sfield_lowering_info;
    // INVOKE data index, points to MIRGraph::method_lowering_infos_. Also used for inlined
    // CONST and MOVE insn (with MIR_CALLEE) to remember the invoke for type inference.
    uint32_t method_lowering_info;
  } meta;

  explicit MIR() : offset(0), optimization_flags(0), m_unit_index(0), bb(NullBasicBlockId),
                 next(nullptr), ssa_rep(nullptr) {
    memset(&meta, 0, sizeof(meta));
  }

  uint32_t GetStartUseIndex() const {
    return SSARepresentation::GetStartUseIndex(dalvikInsn.opcode);
  }

  MIR* Copy(CompilationUnit *c_unit);
  MIR* Copy(MIRGraph* mir_Graph);
};

struct SuccessorBlockInfo;

class BasicBlock : public DeletableArenaObject<kArenaAllocBB> {
 public:
  BasicBlock(BasicBlockId block_id, BBType type, ArenaAllocator* allocator)
      : id(block_id),
        dfs_id(), start_offset(), fall_through(), taken(), i_dom(), nesting_depth(),
        block_type(type),
        successor_block_list_type(kNotUsed),
        visited(), hidden(), catch_entry(), explicit_throw(), conditional_branch(),
        terminated_by_return(), dominates_return(), use_lvn(), first_mir_insn(),
        last_mir_insn(), data_flow_info(), dominators(), i_dominated(), dom_frontier(),
        predecessors(allocator->Adapter(kArenaAllocBBPredecessors)),
        successor_blocks(allocator->Adapter(kArenaAllocSuccessor)) {
  }
  BasicBlockId id;
  BasicBlockId dfs_id;
  NarrowDexOffset start_offset;     // Offset in code units.
  BasicBlockId fall_through;
  BasicBlockId taken;
  BasicBlockId i_dom;               // Immediate dominator.
  uint16_t nesting_depth;
  BBType block_type:4;
  BlockListType successor_block_list_type:4;
  bool visited:1;
  bool hidden:1;
  bool catch_entry:1;
  bool explicit_throw:1;
  bool conditional_branch:1;
  bool terminated_by_return:1;  // Block ends with a Dalvik return opcode.
  bool dominates_return:1;      // Is a member of return extended basic block.
  bool use_lvn:1;               // Run local value numbering on this block.
  MIR* first_mir_insn;
  MIR* last_mir_insn;
  BasicBlockDataFlow* data_flow_info;
  ArenaBitVector* dominators;
  ArenaBitVector* i_dominated;      // Set nodes being immediately dominated.
  ArenaBitVector* dom_frontier;     // Dominance frontier.
  ArenaVector<BasicBlockId> predecessors;
  ArenaVector<SuccessorBlockInfo*> successor_blocks;

  void AppendMIR(MIR* mir);
  void AppendMIRList(MIR* first_list_mir, MIR* last_list_mir);
  void AppendMIRList(const std::vector<MIR*>& insns);
  void PrependMIR(MIR* mir);
  void PrependMIRList(MIR* first_list_mir, MIR* last_list_mir);
  void PrependMIRList(const std::vector<MIR*>& to_add);
  void InsertMIRAfter(MIR* current_mir, MIR* new_mir);
  void InsertMIRListAfter(MIR* insert_after, MIR* first_list_mir, MIR* last_list_mir);
  MIR* FindPreviousMIR(MIR* mir);
  void InsertMIRBefore(MIR* insert_before, MIR* list);
  void InsertMIRListBefore(MIR* insert_before, MIR* first_list_mir, MIR* last_list_mir);
  bool RemoveMIR(MIR* mir);
  bool RemoveMIRList(MIR* first_list_mir, MIR* last_list_mir);

  BasicBlock* Copy(CompilationUnit* c_unit);
  BasicBlock* Copy(MIRGraph* mir_graph);

  /**
   * @brief Reset the optimization_flags field of each MIR.
   */
  void ResetOptimizationFlags(uint16_t reset_flags);

  /**
   * @brief Kill the BasicBlock.
   * @details Unlink predecessors and successors, remove all MIRs, set the block type to kDead
   *          and set hidden to true.
   */
  void Kill(MIRGraph* mir_graph);

  /**
   * @brief Is ssa_reg the last SSA definition of that VR in the block?
   */
  bool IsSSALiveOut(const CompilationUnit* c_unit, int ssa_reg);

  /**
   * @brief Replace the edge going to old_bb to now go towards new_bb.
   */
  bool ReplaceChild(BasicBlockId old_bb, BasicBlockId new_bb);

  /**
   * @brief Erase the predecessor old_pred.
   */
  void ErasePredecessor(BasicBlockId old_pred);

  /**
   * @brief Update the predecessor array from old_pred to new_pred.
   */
  void UpdatePredecessor(BasicBlockId old_pred, BasicBlockId new_pred);

  /**
   * @brief Return first non-Phi insn.
   */
  MIR* GetFirstNonPhiInsn();

  /**
   * @brief Checks whether the block ends with if-nez or if-eqz that branches to
   *        the given successor only if the register in not zero.
   */
  bool BranchesToSuccessorOnlyIfNotZero(BasicBlockId succ_id) const {
    if (last_mir_insn == nullptr) {
      return false;
    }
    Instruction::Code last_opcode = last_mir_insn->dalvikInsn.opcode;
    return ((last_opcode == Instruction::IF_EQZ && fall_through == succ_id) ||
        (last_opcode == Instruction::IF_NEZ && taken == succ_id)) &&
        // Make sure the other successor isn't the same (empty if), b/21614284.
        (fall_through != taken);
  }

  /**
   * @brief Used to obtain the next MIR that follows unconditionally.
   * @details The implementation does not guarantee that a MIR does not
   * follow even if this method returns nullptr.
   * @param mir_graph the MIRGraph.
   * @param current The MIR for which to find an unconditional follower.
   * @return Returns the following MIR if one can be found.
   */
  MIR* GetNextUnconditionalMir(MIRGraph* mir_graph, MIR* current);
  bool IsExceptionBlock() const;

 private:
  DISALLOW_COPY_AND_ASSIGN(BasicBlock);
};

/*
 * The "blocks" field in "successor_block_list" points to an array of elements with the type
 * "SuccessorBlockInfo".  For catch blocks, key is type index for the exception.  For switch
 * blocks, key is the case value.
 */
struct SuccessorBlockInfo {
  BasicBlockId block;
  int key;
};

/**
 * @class ChildBlockIterator
 * @brief Enable an easy iteration of the children.
 */
class ChildBlockIterator {
 public:
  /**
   * @brief Constructs a child iterator.
   * @param bb The basic whose children we need to iterate through.
   * @param mir_graph The MIRGraph used to get the basic block during iteration.
   */
  ChildBlockIterator(BasicBlock* bb, MIRGraph* mir_graph);
  BasicBlock* Next();

 private:
  BasicBlock* basic_block_;
  MIRGraph* mir_graph_;
  bool visited_fallthrough_;
  bool visited_taken_;
  bool have_successors_;
  ArenaVector<SuccessorBlockInfo*>::const_iterator successor_iter_;
};

/*
 * Collection of information describing an invoke, and the destination of
 * the subsequent MOVE_RESULT (if applicable).  Collected as a unit to enable
 * more efficient invoke code generation.
 */
struct CallInfo {
  size_t num_arg_words;   // Note: word count, not arg count.
  RegLocation* args;      // One for each word of arguments.
  RegLocation result;     // Eventual target of MOVE_RESULT.
  int opt_flags;
  InvokeType type;
  uint32_t dex_idx;
  MethodReference method_ref;
  uint32_t index;         // Method idx for invokes, type idx for FilledNewArray.
  uintptr_t direct_code;
  uintptr_t direct_method;
  RegLocation target;     // Target of following move_result.
  bool skip_this;
  bool is_range;
  DexOffset offset;       // Offset in code units.
  MIR* mir;
  int32_t string_init_offset;
};


const RegLocation bad_loc = {kLocDalvikFrame, 0, 0, 0, 0, 0, 0, 0, 0, RegStorage(), INVALID_SREG,
                             INVALID_SREG};

class MIRGraph {
 public:
  MIRGraph(CompilationUnit* cu, ArenaAllocator* arena);
  virtual ~MIRGraph();

  /*
   * Examine the graph to determine whether it's worthwile to spend the time compiling
   * this method.
   */
  bool SkipCompilation(std::string* skip_message);

  /*
   * Should we skip the compilation of this method based on its name?
   */
  bool SkipCompilationByName(const std::string& methodname);

  /*
   * Parse dex method and add MIR at current insert point.  Returns id (which is
   * actually the index of the method in the m_units_ array).
   */
  void InlineMethod(const DexFile::CodeItem* code_item, uint32_t access_flags,
                    InvokeType invoke_type, uint16_t class_def_idx,
                    uint32_t method_idx, jobject class_loader, const DexFile& dex_file);

  /* Find existing block */
  BasicBlock* FindBlock(DexOffset code_offset,
                        ScopedArenaVector<uint16_t>* dex_pc_to_block_map) {
    return FindBlock(code_offset, false, nullptr, dex_pc_to_block_map);
  }

  const uint16_t* GetCurrentInsns() const {
    return current_code_item_->insns_;
  }

  /**
   * @brief Used to obtain the raw dex bytecode instruction pointer.
   * @param m_unit_index The method index in MIRGraph (caused by having multiple methods).
   * This is guaranteed to contain index 0 which is the base method being compiled.
   * @return Returns the raw instruction pointer.
   */
  const uint16_t* GetInsns(int m_unit_index) const;

  /**
   * @brief Used to obtain the raw data table.
   * @param mir sparse switch, packed switch, of fill-array-data
   * @param table_offset The table offset from start of method.
   * @return Returns the raw table pointer.
   */
  const uint16_t* GetTable(MIR* mir, uint32_t table_offset) const {
    return GetInsns(mir->m_unit_index) + mir->offset + static_cast<int32_t>(table_offset);
  }

  unsigned int GetNumBlocks() const {
    return block_list_.size();
  }

  /**
   * @brief Provides the total size in code units of all instructions in MIRGraph.
   * @details Includes the sizes of all methods in compilation unit.
   * @return Returns the cumulative sum of all insn sizes (in code units).
   */
  size_t GetNumDalvikInsns() const;

  ArenaBitVector* GetTryBlockAddr() const {
    return try_block_addr_;
  }

  BasicBlock* GetEntryBlock() const {
    return entry_block_;
  }

  BasicBlock* GetExitBlock() const {
    return exit_block_;
  }

  BasicBlock* GetBasicBlock(unsigned int block_id) const {
    DCHECK_LT(block_id, block_list_.size());  // NOTE: NullBasicBlockId is 0.
    return (block_id == NullBasicBlockId) ? nullptr : block_list_[block_id];
  }

  size_t GetBasicBlockListCount() const {
    return block_list_.size();
  }

  const ArenaVector<BasicBlock*>& GetBlockList() {
    return block_list_;
  }

  const ArenaVector<BasicBlockId>& GetDfsOrder() {
    return dfs_order_;
  }

  const ArenaVector<BasicBlockId>& GetDfsPostOrder() {
    return dfs_post_order_;
  }

  const ArenaVector<BasicBlockId>& GetDomPostOrder() {
    return dom_post_order_traversal_;
  }

  int GetDefCount() const {
    return def_count_;
  }

  ArenaAllocator* GetArena() const {
    return arena_;
  }

  void EnableOpcodeCounting() {
    opcode_count_ = arena_->AllocArray<int>(kNumPackedOpcodes, kArenaAllocMisc);
  }

  void ShowOpcodeStats();

  DexCompilationUnit* GetCurrentDexCompilationUnit() const {
    return m_units_[current_method_];
  }

  /**
   * @brief Dump a CFG into a dot file format.
   * @param dir_prefix the directory the file will be created in.
   * @param all_blocks does the dumper use all the basic blocks or use the reachable blocks.
   * @param suffix does the filename require a suffix or not (default = nullptr).
   */
  void DumpCFG(const char* dir_prefix, bool all_blocks, const char* suffix = nullptr);

  bool HasCheckCast() const {
    return (merged_df_flags_ & DF_CHK_CAST) != 0u;
  }

  bool HasFieldAccess() const {
    return (merged_df_flags_ & (DF_IFIELD | DF_SFIELD)) != 0u;
  }

  bool HasStaticFieldAccess() const {
    return (merged_df_flags_ & DF_SFIELD) != 0u;
  }

  bool HasInvokes() const {
    // NOTE: These formats include the rare filled-new-array/range.
    return (merged_df_flags_ & (DF_FORMAT_35C | DF_FORMAT_3RC)) != 0u;
  }

  void DoCacheFieldLoweringInfo();

  const MirIFieldLoweringInfo& GetIFieldLoweringInfo(MIR* mir) const {
    return GetIFieldLoweringInfo(mir->meta.ifield_lowering_info);
  }

  const MirIFieldLoweringInfo& GetIFieldLoweringInfo(uint32_t lowering_info) const {
    DCHECK_LT(lowering_info, ifield_lowering_infos_.size());
    return ifield_lowering_infos_[lowering_info];
  }

  size_t GetIFieldLoweringInfoCount() const {
    return ifield_lowering_infos_.size();
  }

  const MirSFieldLoweringInfo& GetSFieldLoweringInfo(MIR* mir) const {
    return GetSFieldLoweringInfo(mir->meta.sfield_lowering_info);
  }

  const MirSFieldLoweringInfo& GetSFieldLoweringInfo(uint32_t lowering_info) const {
    DCHECK_LT(lowering_info, sfield_lowering_infos_.size());
    return sfield_lowering_infos_[lowering_info];
  }

  size_t GetSFieldLoweringInfoCount() const {
    return sfield_lowering_infos_.size();
  }

  void DoCacheMethodLoweringInfo();

  const MirMethodLoweringInfo& GetMethodLoweringInfo(MIR* mir) const {
    return GetMethodLoweringInfo(mir->meta.method_lowering_info);
  }

  const MirMethodLoweringInfo& GetMethodLoweringInfo(uint32_t lowering_info) const {
    DCHECK_LT(lowering_info, method_lowering_infos_.size());
    return method_lowering_infos_[lowering_info];
  }

  size_t GetMethodLoweringInfoCount() const {
    return method_lowering_infos_.size();
  }

  void ComputeInlineIFieldLoweringInfo(uint16_t field_idx, MIR* invoke, MIR* iget_or_iput);

  void InitRegLocations();

  void RemapRegLocations();

  void DumpRegLocTable(RegLocation* table, int count);

  void BasicBlockOptimizationStart();
  void BasicBlockOptimization();
  void BasicBlockOptimizationEnd();

  void StringChange();

  const ArenaVector<BasicBlockId>& GetTopologicalSortOrder() {
    DCHECK(!topological_order_.empty());
    return topological_order_;
  }

  const ArenaVector<BasicBlockId>& GetTopologicalSortOrderLoopEnds() {
    DCHECK(!topological_order_loop_ends_.empty());
    return topological_order_loop_ends_;
  }

  const ArenaVector<BasicBlockId>& GetTopologicalSortOrderIndexes() {
    DCHECK(!topological_order_indexes_.empty());
    return topological_order_indexes_;
  }

  ArenaVector<std::pair<uint16_t, bool>>* GetTopologicalSortOrderLoopHeadStack() {
    DCHECK(!topological_order_.empty());  // Checking the main array, not the stack.
    return &topological_order_loop_head_stack_;
  }

  size_t GetMaxNestedLoops() const {
    return max_nested_loops_;
  }

  bool IsLoopHead(BasicBlockId bb_id) {
    return topological_order_loop_ends_[topological_order_indexes_[bb_id]] != 0u;
  }

  bool IsConst(int32_t s_reg) const {
    return is_constant_v_->IsBitSet(s_reg);
  }

  bool IsConst(RegLocation loc) const {
    return loc.orig_sreg < 0 ? false : IsConst(loc.orig_sreg);
  }

  int32_t ConstantValue(RegLocation loc) const {
    DCHECK(IsConst(loc));
    return constant_values_[loc.orig_sreg];
  }

  int32_t ConstantValue(int32_t s_reg) const {
    DCHECK(IsConst(s_reg));
    return constant_values_[s_reg];
  }

  /**
   * @brief Used to obtain 64-bit value of a pair of ssa registers.
   * @param s_reg_low The ssa register representing the low bits.
   * @param s_reg_high The ssa register representing the high bits.
   * @return Retusn the 64-bit constant value.
   */
  int64_t ConstantValueWide(int32_t s_reg_low, int32_t s_reg_high) const {
    DCHECK(IsConst(s_reg_low));
    DCHECK(IsConst(s_reg_high));
    return (static_cast<int64_t>(constant_values_[s_reg_high]) << 32) |
        Low32Bits(static_cast<int64_t>(constant_values_[s_reg_low]));
  }

  int64_t ConstantValueWide(RegLocation loc) const {
    DCHECK(IsConst(loc));
    DCHECK(!loc.high_word);  // Do not allow asking for the high partner.
    DCHECK_LT(loc.orig_sreg + 1, GetNumSSARegs());
    return (static_cast<int64_t>(constant_values_[loc.orig_sreg + 1]) << 32) |
        Low32Bits(static_cast<int64_t>(constant_values_[loc.orig_sreg]));
  }

  /**
   * @brief Used to mark ssa register as being constant.
   * @param ssa_reg The ssa register.
   * @param value The constant value of ssa register.
   */
  void SetConstant(int32_t ssa_reg, int32_t value);

  /**
   * @brief Used to mark ssa register and its wide counter-part as being constant.
   * @param ssa_reg The ssa register.
   * @param value The 64-bit constant value of ssa register and its pair.
   */
  void SetConstantWide(int32_t ssa_reg, int64_t value);

  bool IsConstantNullRef(RegLocation loc) const {
    return loc.ref && loc.is_const && (ConstantValue(loc) == 0);
  }

  int GetNumSSARegs() const {
    return num_ssa_regs_;
  }

  void SetNumSSARegs(int new_num) {
     /*
      * TODO: It's theoretically possible to exceed 32767, though any cases which did
      * would be filtered out with current settings.  When orig_sreg field is removed
      * from RegLocation, expand s_reg_low to handle all possible cases and remove DCHECK().
      */
    CHECK_EQ(new_num, static_cast<int16_t>(new_num));
    num_ssa_regs_ = new_num;
  }

  unsigned int GetNumReachableBlocks() const {
    return num_reachable_blocks_;
  }

  uint32_t GetUseCount(int sreg) const {
    DCHECK_LT(static_cast<size_t>(sreg), use_counts_.size());
    return use_counts_[sreg];
  }

  uint32_t GetRawUseCount(int sreg) const {
    DCHECK_LT(static_cast<size_t>(sreg), raw_use_counts_.size());
    return raw_use_counts_[sreg];
  }

  int GetSSASubscript(int ssa_reg) const {
    DCHECK_LT(static_cast<size_t>(ssa_reg), ssa_subscripts_.size());
    return ssa_subscripts_[ssa_reg];
  }

  RegLocation GetRawSrc(MIR* mir, int num) {
    DCHECK(num < mir->ssa_rep->num_uses);
    RegLocation res = reg_location_[mir->ssa_rep->uses[num]];
    return res;
  }

  RegLocation GetRawDest(MIR* mir) {
    DCHECK_GT(mir->ssa_rep->num_defs, 0);
    RegLocation res = reg_location_[mir->ssa_rep->defs[0]];
    return res;
  }

  RegLocation GetDest(MIR* mir) {
    RegLocation res = GetRawDest(mir);
    DCHECK(!res.wide);
    return res;
  }

  RegLocation GetSrc(MIR* mir, int num) {
    RegLocation res = GetRawSrc(mir, num);
    DCHECK(!res.wide);
    return res;
  }

  RegLocation GetDestWide(MIR* mir) {
    RegLocation res = GetRawDest(mir);
    DCHECK(res.wide);
    return res;
  }

  RegLocation GetSrcWide(MIR* mir, int low) {
    RegLocation res = GetRawSrc(mir, low);
    DCHECK(res.wide);
    return res;
  }

  RegLocation GetBadLoc() {
    return bad_loc;
  }

  int GetMethodSReg() const {
    return method_sreg_;
  }

  /**
   * @brief Used to obtain the number of compiler temporaries being used.
   * @return Returns the number of compiler temporaries.
   */
  size_t GetNumUsedCompilerTemps() const {
    // Assume that the special temps will always be used.
    return GetNumNonSpecialCompilerTemps() + max_available_special_compiler_temps_;
  }

  /**
   * @brief Used to obtain number of bytes needed for special temps.
   * @details This space is always needed because temps have special location on stack.
   * @return Returns number of bytes for the special temps.
   */
  size_t GetNumBytesForSpecialTemps() const;

  /**
   * @brief Used by backend as a hint for maximum number of bytes for non-special temps.
   * @details Returns 4 bytes for each temp because that is the maximum amount needed
   * for storing each temp. The BE could be smarter though and allocate a smaller
   * spill region.
   * @return Returns the maximum number of bytes needed for non-special temps.
   */
  size_t GetMaximumBytesForNonSpecialTemps() const {
    return GetNumNonSpecialCompilerTemps() * sizeof(uint32_t);
  }

  /**
   * @brief Used to obtain the number of non-special compiler temporaries being used.
   * @return Returns the number of non-special compiler temporaries.
   */
  size_t GetNumNonSpecialCompilerTemps() const {
    return num_non_special_compiler_temps_;
  }

  /**
   * @brief Used to set the total number of available non-special compiler temporaries.
   * @details Can fail setting the new max if there are more temps being used than the new_max.
   * @param new_max The new maximum number of non-special compiler temporaries.
   * @return Returns true if the max was set and false if failed to set.
   */
  bool SetMaxAvailableNonSpecialCompilerTemps(size_t new_max) {
    // Make sure that enough temps still exist for backend and also that the
    // new max can still keep around all of the already requested temps.
    if (new_max < (GetNumNonSpecialCompilerTemps() + reserved_temps_for_backend_)) {
      return false;
    } else {
      max_available_non_special_compiler_temps_ = new_max;
      return true;
    }
  }

  /**
   * @brief Provides the number of non-special compiler temps available for use by ME.
   * @details Even if this returns zero, special compiler temps are guaranteed to be available.
   * Additionally, this makes sure to not use any temps reserved for BE only.
   * @return Returns the number of available temps.
   */
  size_t GetNumAvailableVRTemps();

  /**
   * @brief Used to obtain the maximum number of compiler temporaries that can be requested.
   * @return Returns the maximum number of compiler temporaries, whether used or not.
   */
  size_t GetMaxPossibleCompilerTemps() const {
    return max_available_special_compiler_temps_ + max_available_non_special_compiler_temps_;
  }

  /**
   * @brief Used to signal that the compiler temps have been committed.
   * @details This should be used once the number of temps can no longer change,
   * such as after frame size is committed and cannot be changed.
   */
  void CommitCompilerTemps() {
    compiler_temps_committed_ = true;
  }

  /**
   * @brief Used to obtain a new unique compiler temporary.
   * @details Two things are done for convenience when allocating a new compiler
   * temporary. The ssa register is automatically requested and the information
   * about reg location is filled. This helps when the temp is requested post
   * ssa initialization, such as when temps are requested by the backend.
   * @warning If the temp requested will be used for ME and have multiple versions,
   * the sreg provided by the temp will be invalidated on next ssa recalculation.
   * @param ct_type Type of compiler temporary requested.
   * @param wide Whether we should allocate a wide temporary.
   * @return Returns the newly created compiler temporary.
   */
  CompilerTemp* GetNewCompilerTemp(CompilerTempType ct_type, bool wide);

  /**
   * @brief Used to remove last created compiler temporary when it's not needed.
   * @param temp the temporary to remove.
   */
  void RemoveLastCompilerTemp(CompilerTempType ct_type, bool wide, CompilerTemp* temp);

  bool MethodIsLeaf() {
    return attributes_ & METHOD_IS_LEAF;
  }

  RegLocation GetRegLocation(int index) {
    DCHECK((index >= 0) && (index < num_ssa_regs_));
    return reg_location_[index];
  }

  RegLocation GetMethodLoc() {
    return reg_location_[method_sreg_];
  }

  bool IsBackEdge(BasicBlock* branch_bb, BasicBlockId target_bb_id) {
    DCHECK_NE(target_bb_id, NullBasicBlockId);
    DCHECK_LT(target_bb_id, topological_order_indexes_.size());
    DCHECK_LT(branch_bb->id, topological_order_indexes_.size());
    return topological_order_indexes_[target_bb_id] <= topological_order_indexes_[branch_bb->id];
  }

  bool IsSuspendCheckEdge(BasicBlock* branch_bb, BasicBlockId target_bb_id) {
    if (!IsBackEdge(branch_bb, target_bb_id)) {
      return false;
    }
    if (suspend_checks_in_loops_ == nullptr) {
      // We didn't run suspend check elimination.
      return true;
    }
    uint16_t target_depth = GetBasicBlock(target_bb_id)->nesting_depth;
    return (suspend_checks_in_loops_[branch_bb->id] & (1u << (target_depth - 1u))) == 0;
  }

  void CountBranch(DexOffset target_offset) {
    if (target_offset <= current_offset_) {
      backward_branches_++;
    } else {
      forward_branches_++;
    }
  }

  int GetBranchCount() {
    return backward_branches_ + forward_branches_;
  }

  // Is this vreg in the in set?
  bool IsInVReg(uint32_t vreg) {
    return (vreg >= GetFirstInVR()) && (vreg < GetFirstTempVR());
  }

  uint32_t GetNumOfCodeVRs() const {
    return current_code_item_->registers_size_;
  }

  uint32_t GetNumOfCodeAndTempVRs() const {
    // Include all of the possible temps so that no structures overflow when initialized.
    return GetNumOfCodeVRs() + GetMaxPossibleCompilerTemps();
  }

  uint32_t GetNumOfLocalCodeVRs() const {
    // This also refers to the first "in" VR.
    return GetNumOfCodeVRs() - current_code_item_->ins_size_;
  }

  uint32_t GetNumOfInVRs() const {
    return current_code_item_->ins_size_;
  }

  uint32_t GetNumOfOutVRs() const {
    return current_code_item_->outs_size_;
  }

  uint32_t GetFirstInVR() const {
    return GetNumOfLocalCodeVRs();
  }

  uint32_t GetFirstTempVR() const {
    // Temp VRs immediately follow code VRs.
    return GetNumOfCodeVRs();
  }

  uint32_t GetFirstSpecialTempVR() const {
    // Special temps appear first in the ordering before non special temps.
    return GetFirstTempVR();
  }

  uint32_t GetFirstNonSpecialTempVR() const {
    // We always leave space for all the special temps before the non-special ones.
    return GetFirstSpecialTempVR() + max_available_special_compiler_temps_;
  }

  bool HasTryCatchBlocks() const {
    return current_code_item_->tries_size_ != 0;
  }

  void DumpCheckStats();
  MIR* FindMoveResult(BasicBlock* bb, MIR* mir);

  /* Return the base virtual register for a SSA name */
  int SRegToVReg(int ssa_reg) const {
    return ssa_base_vregs_[ssa_reg];
  }

  void VerifyDataflow();
  void CheckForDominanceFrontier(BasicBlock* dom_bb, const BasicBlock* succ_bb);
  bool EliminateNullChecksGate();
  bool EliminateNullChecks(BasicBlock* bb);
  void EliminateNullChecksEnd();
  void InferTypesStart();
  bool InferTypes(BasicBlock* bb);
  void InferTypesEnd();
  bool EliminateClassInitChecksGate();
  bool EliminateClassInitChecks(BasicBlock* bb);
  void EliminateClassInitChecksEnd();
  bool ApplyGlobalValueNumberingGate();
  bool ApplyGlobalValueNumbering(BasicBlock* bb);
  void ApplyGlobalValueNumberingEnd();
  bool EliminateDeadCodeGate();
  bool EliminateDeadCode(BasicBlock* bb);
  void EliminateDeadCodeEnd();
  void GlobalValueNumberingCleanup();
  bool EliminateSuspendChecksGate();
  bool EliminateSuspendChecks(BasicBlock* bb);

  uint16_t GetGvnIFieldId(MIR* mir) const {
    DCHECK(IsInstructionIGetOrIPut(mir->dalvikInsn.opcode));
    DCHECK_LT(mir->meta.ifield_lowering_info, ifield_lowering_infos_.size());
    DCHECK(temp_.gvn.ifield_ids != nullptr);
    return temp_.gvn.ifield_ids[mir->meta.ifield_lowering_info];
  }

  uint16_t GetGvnSFieldId(MIR* mir) const {
    DCHECK(IsInstructionSGetOrSPut(mir->dalvikInsn.opcode));
    DCHECK_LT(mir->meta.sfield_lowering_info, sfield_lowering_infos_.size());
    DCHECK(temp_.gvn.sfield_ids != nullptr);
    return temp_.gvn.sfield_ids[mir->meta.sfield_lowering_info];
  }

  bool PuntToInterpreter() {
    return punt_to_interpreter_;
  }

  void SetPuntToInterpreter(bool val);

  void DisassembleExtendedInstr(const MIR* mir, std::string* decoded_mir);
  char* GetDalvikDisassembly(const MIR* mir);
  void ReplaceSpecialChars(std::string& str);
  std::string GetSSAName(int ssa_reg);
  std::string GetSSANameWithConst(int ssa_reg, bool singles_only);
  void GetBlockName(BasicBlock* bb, char* name);
  const char* GetShortyFromMethodReference(const MethodReference& target_method);
  void DumpMIRGraph();
  CallInfo* NewMemCallInfo(BasicBlock* bb, MIR* mir, InvokeType type, bool is_range);
  BasicBlock* NewMemBB(BBType block_type, int block_id);
  MIR* NewMIR();
  MIR* AdvanceMIR(BasicBlock** p_bb, MIR* mir);
  BasicBlock* NextDominatedBlock(BasicBlock* bb);
  bool LayoutBlocks(BasicBlock* bb);
  void ComputeTopologicalSortOrder();
  BasicBlock* CreateNewBB(BBType block_type);

  bool InlineSpecialMethodsGate();
  void InlineSpecialMethodsStart();
  void InlineSpecialMethods(BasicBlock* bb);
  void InlineSpecialMethodsEnd();

  /**
   * @brief Perform the initial preparation for the Method Uses.
   */
  void InitializeMethodUses();

  /**
   * @brief Perform the initial preparation for the Constant Propagation.
   */
  void InitializeConstantPropagation();

  /**
   * @brief Perform the initial preparation for the SSA Transformation.
   */
  void SSATransformationStart();

  /**
   * @brief Insert a the operands for the Phi nodes.
   * @param bb the considered BasicBlock.
   * @return true
   */
  bool InsertPhiNodeOperands(BasicBlock* bb);

  /**
   * @brief Perform the cleanup after the SSA Transformation.
   */
  void SSATransformationEnd();

  /**
   * @brief Perform constant propagation on a BasicBlock.
   * @param bb the considered BasicBlock.
   */
  void DoConstantPropagation(BasicBlock* bb);

  /**
   * @brief Get use count weight for a given block.
   * @param bb the BasicBlock.
   */
  uint32_t GetUseCountWeight(BasicBlock* bb) const;

  /**
   * @brief Count the uses in the BasicBlock
   * @param bb the BasicBlock
   */
  void CountUses(BasicBlock* bb);

  static uint64_t GetDataFlowAttributes(Instruction::Code opcode);
  static uint64_t GetDataFlowAttributes(MIR* mir);

  /**
   * @brief Combine BasicBlocks
   * @param the BasicBlock we are considering
   */
  void CombineBlocks(BasicBlock* bb);

  void ClearAllVisitedFlags();

  void AllocateSSAUseData(MIR *mir, int num_uses);
  void AllocateSSADefData(MIR *mir, int num_defs);
  void CalculateBasicBlockInformation(const PassManager* const post_opt);
  void ComputeDFSOrders();
  void ComputeDefBlockMatrix();
  void ComputeDominators();
  void CompilerInitializeSSAConversion();
  virtual void InitializeBasicBlockDataFlow();
  void FindPhiNodeBlocks();
  void DoDFSPreOrderSSARename(BasicBlock* block);

  bool DfsOrdersUpToDate() const {
    return dfs_orders_up_to_date_;
  }

  bool DominationUpToDate() const {
    return domination_up_to_date_;
  }

  bool MirSsaRepUpToDate() const {
    return mir_ssa_rep_up_to_date_;
  }

  bool TopologicalOrderUpToDate() const {
    return topological_order_up_to_date_;
  }

  /*
   * IsDebugBuild sanity check: keep track of the Dex PCs for catch entries so that later on
   * we can verify that all catch entries have native PC entries.
   */
  std::set<uint32_t> catches_;

  // TODO: make these private.
  RegLocation* reg_location_;                               // Map SSA names to location.
  ArenaSafeMap<unsigned int, unsigned int> block_id_map_;   // Block collapse lookup cache.

  static const char* extended_mir_op_names_[kMirOpLast - kMirOpFirst];

  void HandleSSADef(int* defs, int dalvik_reg, int reg_index);

 protected:
  int FindCommonParent(int block1, int block2);
  void ComputeSuccLineIn(ArenaBitVector* dest, const ArenaBitVector* src1,
                         const ArenaBitVector* src2);
  void HandleLiveInUse(ArenaBitVector* use_v, ArenaBitVector* def_v,
                       ArenaBitVector* live_in_v, int dalvik_reg_id);
  void HandleDef(ArenaBitVector* def_v, int dalvik_reg_id);
  void HandleExtended(ArenaBitVector* use_v, ArenaBitVector* def_v,
                      ArenaBitVector* live_in_v,
                      const MIR::DecodedInstruction& d_insn);
  bool DoSSAConversion(BasicBlock* bb);
  int ParseInsn(const uint16_t* code_ptr, MIR::DecodedInstruction* decoded_instruction);
  bool ContentIsInsn(const uint16_t* code_ptr);
  BasicBlock* SplitBlock(DexOffset code_offset, BasicBlock* orig_block,
                         BasicBlock** immed_pred_block_p);
  BasicBlock* FindBlock(DexOffset code_offset, bool create, BasicBlock** immed_pred_block_p,
                        ScopedArenaVector<uint16_t>* dex_pc_to_block_map);
  void ProcessTryCatchBlocks(ScopedArenaVector<uint16_t>* dex_pc_to_block_map);
  bool IsBadMonitorExitCatch(NarrowDexOffset monitor_exit_offset, NarrowDexOffset catch_offset);
  BasicBlock* ProcessCanBranch(BasicBlock* cur_block, MIR* insn, DexOffset cur_offset, int width,
                               int flags, const uint16_t* code_ptr, const uint16_t* code_end,
                               ScopedArenaVector<uint16_t>* dex_pc_to_block_map);
  BasicBlock* ProcessCanSwitch(BasicBlock* cur_block, MIR* insn, DexOffset cur_offset, int width,
                               int flags,
                               ScopedArenaVector<uint16_t>* dex_pc_to_block_map);
  BasicBlock* ProcessCanThrow(BasicBlock* cur_block, MIR* insn, DexOffset cur_offset, int width,
                              int flags, ArenaBitVector* try_block_addr, const uint16_t* code_ptr,
                              const uint16_t* code_end,
                              ScopedArenaVector<uint16_t>* dex_pc_to_block_map);
  int AddNewSReg(int v_reg);
  void HandleSSAUse(int* uses, int dalvik_reg, int reg_index);
  void DataFlowSSAFormat35C(MIR* mir);
  void DataFlowSSAFormat3RC(MIR* mir);
  void DataFlowSSAFormatExtended(MIR* mir);
  bool FindLocalLiveIn(BasicBlock* bb);
  bool VerifyPredInfo(BasicBlock* bb);
  BasicBlock* NeedsVisit(BasicBlock* bb);
  BasicBlock* NextUnvisitedSuccessor(BasicBlock* bb);
  void MarkPreOrder(BasicBlock* bb);
  void RecordDFSOrders(BasicBlock* bb);
  void ComputeDomPostOrderTraversal(BasicBlock* bb);
  int GetSSAUseCount(int s_reg);
  bool BasicBlockOpt(BasicBlock* bb);
  void MultiplyAddOpt(BasicBlock* bb);

  /**
   * @brief Check whether the given MIR is possible to throw an exception.
   * @param mir The mir to check.
   * @return Returns 'true' if the given MIR might throw an exception.
   */
  bool CanThrow(MIR* mir) const;

  /**
   * @brief Combine multiply and add/sub MIRs into corresponding extended MAC MIR.
   * @param mul_mir The multiply MIR to be combined.
   * @param add_mir The add/sub MIR to be combined.
   * @param mul_is_first_addend 'true' if multiply product is the first addend of add operation.
   * @param is_wide 'true' if the operations are long type.
   * @param is_sub 'true' if it is a multiply-subtract operation.
   */
  void CombineMultiplyAdd(MIR* mul_mir, MIR* add_mir, bool mul_is_first_addend,
                          bool is_wide, bool is_sub);
  /*
   * @brief Check whether the first MIR anti-depends on the second MIR.
   * @details To check whether one of first MIR's uses of vregs is redefined by the second MIR,
   * i.e. there is a write-after-read dependency.
   * @param first The first MIR.
   * @param second The second MIR.
   * @param Returns true if there is a write-after-read dependency.
   */
  bool HasAntiDependency(MIR* first, MIR* second);

  bool BuildExtendedBBList(class BasicBlock* bb);
  bool FillDefBlockMatrix(BasicBlock* bb);
  void InitializeDominationInfo(BasicBlock* bb);
  bool ComputeblockIDom(BasicBlock* bb);
  bool ComputeBlockDominators(BasicBlock* bb);
  bool SetDominators(BasicBlock* bb);
  bool ComputeBlockLiveIns(BasicBlock* bb);
  bool ComputeDominanceFrontier(BasicBlock* bb);

  void CountChecks(BasicBlock* bb);
  void AnalyzeBlock(BasicBlock* bb, struct MethodStats* stats);
  bool ComputeSkipCompilation(struct MethodStats* stats, bool skip_default,
                              std::string* skip_message);

  CompilationUnit* const cu_;
  ArenaVector<int> ssa_base_vregs_;
  ArenaVector<int> ssa_subscripts_;
  // Map original Dalvik virtual reg i to the current SSA name.
  int32_t* vreg_to_ssa_map_;        // length == method->registers_size
  int* ssa_last_defs_;              // length == method->registers_size
  ArenaBitVector* is_constant_v_;   // length == num_ssa_reg
  int* constant_values_;            // length == num_ssa_reg
  // Use counts of ssa names.
  ArenaVector<uint32_t> use_counts_;      // Weighted by nesting depth
  ArenaVector<uint32_t> raw_use_counts_;  // Not weighted
  unsigned int num_reachable_blocks_;
  unsigned int max_num_reachable_blocks_;
  bool dfs_orders_up_to_date_;
  bool domination_up_to_date_;
  bool mir_ssa_rep_up_to_date_;
  bool topological_order_up_to_date_;
  ArenaVector<BasicBlockId> dfs_order_;
  ArenaVector<BasicBlockId> dfs_post_order_;
  ArenaVector<BasicBlockId> dom_post_order_traversal_;
  ArenaVector<BasicBlockId> topological_order_;
  // Indexes in topological_order_ need to be only as big as the BasicBlockId.
  static_assert(sizeof(BasicBlockId) == sizeof(uint16_t), "Assuming 16 bit BasicBlockId");
  // For each loop head, remember the past-the-end index of the end of the loop. 0 if not loop head.
  ArenaVector<uint16_t> topological_order_loop_ends_;
  // Map BB ids to topological_order_ indexes. 0xffff if not included (hidden or null block).
  ArenaVector<uint16_t> topological_order_indexes_;
  // Stack of the loop head indexes and recalculation flags for RepeatingTopologicalSortIterator.
  ArenaVector<std::pair<uint16_t, bool>> topological_order_loop_head_stack_;
  size_t max_nested_loops_;
  int* i_dom_list_;
  std::unique_ptr<ScopedArenaAllocator> temp_scoped_alloc_;
  // Union of temporaries used by different passes.
  union {
    // Class init check elimination.
    struct {
      size_t num_class_bits;  // 2 bits per class: class initialized and class in dex cache.
      ArenaBitVector* work_classes_to_check;
      ArenaBitVector** ending_classes_to_check_matrix;  // num_blocks_ x num_class_bits.
      uint16_t* indexes;
    } cice;
    // Null check elimination.
    struct {
      size_t num_vregs;
      ArenaBitVector* work_vregs_to_check;
      ArenaBitVector** ending_vregs_to_check_matrix;  // num_blocks_ x num_vregs.
    } nce;
    // Special method inlining.
    struct {
      size_t num_indexes;
      ArenaBitVector* processed_indexes;
      uint16_t* lowering_infos;
    } smi;
    // SSA transformation.
    struct {
      size_t num_vregs;
      ArenaBitVector* work_live_vregs;
      ArenaBitVector** def_block_matrix;  // num_vregs x num_blocks_.
      ArenaBitVector** phi_node_blocks;  // num_vregs x num_blocks_.
      TypeInference* ti;
    } ssa;
    // Global value numbering.
    struct {
      GlobalValueNumbering* gvn;
      uint16_t* ifield_ids;  // Part of GVN/LVN but cached here for LVN to avoid recalculation.
      uint16_t* sfield_ids;  // Ditto.
      GvnDeadCodeElimination* dce;
    } gvn;
  } temp_;
  static const int kInvalidEntry = -1;
  ArenaVector<BasicBlock*> block_list_;
  ArenaBitVector* try_block_addr_;
  BasicBlock* entry_block_;
  BasicBlock* exit_block_;
  const DexFile::CodeItem* current_code_item_;
  ArenaVector<DexCompilationUnit*> m_units_;     // List of methods included in this graph
  typedef std::pair<int, int> MIRLocation;       // Insert point, (m_unit_ index, offset)
  ArenaVector<MIRLocation> method_stack_;        // Include stack
  int current_method_;
  DexOffset current_offset_;                     // Offset in code units
  int def_count_;                                // Used to estimate size of ssa name storage.
  int* opcode_count_;                            // Dex opcode coverage stats.
  int num_ssa_regs_;                             // Number of names following SSA transformation.
  ArenaVector<BasicBlockId> extended_basic_blocks_;  // Heads of block "traces".
  int method_sreg_;
  unsigned int attributes_;
  Checkstats* checkstats_;
  ArenaAllocator* const arena_;
  int backward_branches_;
  int forward_branches_;
  size_t num_non_special_compiler_temps_;  // Keeps track of allocated non-special compiler temps. These are VRs that are in compiler temp region on stack.
  size_t max_available_non_special_compiler_temps_;  // Keeps track of maximum available non-special temps.
  size_t max_available_special_compiler_temps_;      // Keeps track of maximum available special temps.
  bool requested_backend_temp_;            // Keeps track whether BE temps have been requested.
  size_t reserved_temps_for_backend_;      // Keeps track of the remaining temps that are reserved for BE.
  bool compiler_temps_committed_;          // Keeps track whether number of temps has been frozen (for example post frame size calculation).
  bool punt_to_interpreter_;               // Difficult or not worthwhile - just interpret.
  uint64_t merged_df_flags_;
  ArenaVector<MirIFieldLoweringInfo> ifield_lowering_infos_;
  ArenaVector<MirSFieldLoweringInfo> sfield_lowering_infos_;
  ArenaVector<MirMethodLoweringInfo> method_lowering_infos_;

  // In the suspend check elimination pass we determine for each basic block and enclosing
  // loop whether there's guaranteed to be a suspend check on the path from the loop head
  // to this block. If so, we can eliminate the back-edge suspend check.
  // The bb->id is index into suspend_checks_in_loops_ and the loop head's depth is bit index
  // in a suspend_checks_in_loops_[bb->id].
  uint32_t* suspend_checks_in_loops_;

  static const uint64_t oat_data_flow_attributes_[kMirOpLast];

  friend class MirOptimizationTest;
  friend class ClassInitCheckEliminationTest;
  friend class SuspendCheckEliminationTest;
  friend class NullCheckEliminationTest;
  friend class GlobalValueNumberingTest;
  friend class GvnDeadCodeEliminationTest;
  friend class LocalValueNumberingTest;
  friend class TopologicalSortOrderTest;
  friend class TypeInferenceTest;
  friend class QuickCFITest;
  friend class QuickAssembleX86TestBase;
};

}  // namespace art

#endif  // ART_COMPILER_DEX_MIR_GRAPH_H_
