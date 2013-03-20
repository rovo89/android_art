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

#ifndef ART_SRC_COMPILER_DEX_MIRGRAPH_H_
#define ART_SRC_COMPILER_DEX_MIRGRAPH_H_

#include "dex_file.h"
#include "dex_instruction.h"
#include "compiler_ir.h"

namespace art {

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
  kNullCheckSrc0,        // Null check of uses[0].
  kNullCheckSrc1,        // Null check of uses[1].
  kNullCheckSrc2,        // Null check of uses[2].
  kNullCheckOut0,        // Null check out outgoing arg0.
  kDstNonNull,           // May assume dst is non-null.
  kRetNonNull,           // May assume retval is non-null.
  kNullTransferSrc0,     // Object copy src[0] -> dst.
  kNullTransferSrcN,     // Phi null check state transfer.
  kRangeCheckSrc1,       // Range check of uses[1].
  kRangeCheckSrc2,       // Range check of uses[2].
  kRangeCheckSrc3,       // Range check of uses[3].
  kFPA,
  kFPB,
  kFPC,
  kCoreA,
  kCoreB,
  kCoreC,
  kRefA,
  kRefB,
  kRefC,
  kUsesMethodStar,       // Implicit use of Method*.
};

#define DF_NOP                  0
#define DF_UA                   (1 << kUA)
#define DF_UB                   (1 << kUB)
#define DF_UC                   (1 << kUC)
#define DF_A_WIDE               (1 << kAWide)
#define DF_B_WIDE               (1 << kBWide)
#define DF_C_WIDE               (1 << kCWide)
#define DF_DA                   (1 << kDA)
#define DF_IS_MOVE              (1 << kIsMove)
#define DF_SETS_CONST           (1 << kSetsConst)
#define DF_FORMAT_35C           (1 << kFormat35c)
#define DF_FORMAT_3RC           (1 << kFormat3rc)
#define DF_NULL_CHK_0           (1 << kNullCheckSrc0)
#define DF_NULL_CHK_1           (1 << kNullCheckSrc1)
#define DF_NULL_CHK_2           (1 << kNullCheckSrc2)
#define DF_NULL_CHK_OUT0        (1 << kNullCheckOut0)
#define DF_NON_NULL_DST         (1 << kDstNonNull)
#define DF_NON_NULL_RET         (1 << kRetNonNull)
#define DF_NULL_TRANSFER_0      (1 << kNullTransferSrc0)
#define DF_NULL_TRANSFER_N      (1 << kNullTransferSrcN)
#define DF_RANGE_CHK_1          (1 << kRangeCheckSrc1)
#define DF_RANGE_CHK_2          (1 << kRangeCheckSrc2)
#define DF_RANGE_CHK_3          (1 << kRangeCheckSrc3)
#define DF_FP_A                 (1 << kFPA)
#define DF_FP_B                 (1 << kFPB)
#define DF_FP_C                 (1 << kFPC)
#define DF_CORE_A               (1 << kCoreA)
#define DF_CORE_B               (1 << kCoreB)
#define DF_CORE_C               (1 << kCoreC)
#define DF_REF_A                (1 << kRefA)
#define DF_REF_B                (1 << kRefB)
#define DF_REF_C                (1 << kRefC)
#define DF_UMS                  (1 << kUsesMethodStar)

#define DF_HAS_USES             (DF_UA | DF_UB | DF_UC)

#define DF_HAS_DEFS             (DF_DA)

#define DF_HAS_NULL_CHKS        (DF_NULL_CHK_0 | \
                                 DF_NULL_CHK_1 | \
                                 DF_NULL_CHK_2 | \
                                 DF_NULL_CHK_OUT0)

#define DF_HAS_RANGE_CHKS       (DF_RANGE_CHK_1 | \
                                 DF_RANGE_CHK_2 | \
                                 DF_RANGE_CHK_3)

#define DF_HAS_NR_CHKS          (DF_HAS_NULL_CHKS | \
                                 DF_HAS_RANGE_CHKS)

#define DF_A_IS_REG             (DF_UA | DF_DA)
#define DF_B_IS_REG             (DF_UB)
#define DF_C_IS_REG             (DF_UC)
#define DF_IS_GETTER_OR_SETTER  (DF_IS_GETTER | DF_IS_SETTER)
#define DF_USES_FP              (DF_FP_A | DF_FP_B | DF_FP_C)

extern const int oat_data_flow_attributes[kMirOpLast];

class MIRGraph {
 public:
  MIRGraph(CompilationUnit* cu);
  ~MIRGraph() {}

  /*
   * Parse dex method and add MIR at current insert point.  Returns id (which is
   * actually the index of the method in the m_units_ array).
   */
  void InlineMethod(const DexFile::CodeItem* code_item, uint32_t access_flags,
                    InvokeType invoke_type, uint32_t class_def_idx,
                    uint32_t method_idx, jobject class_loader, const DexFile& dex_file);

  /* Find existing block */
  BasicBlock* FindBlock(unsigned int code_offset) {
    return FindBlock(code_offset, false, false, NULL);
  }

  const uint16_t* GetCurrentInsns() const {
    return current_code_item_->insns_;
  }

  const uint16_t* GetInsns(int m_unit_index) const {
    return m_units_[m_unit_index]->GetCodeItem()->insns_;
  }

  int GetNumBlocks() const {
    return num_blocks_;
  }

  ArenaBitVector* GetTryBlockAddr() const {
    return try_block_addr_;
  }

  BasicBlock* GetEntryBlock() const {
    return entry_block_;
  }

  BasicBlock* GetExitBlock() const {
    return exit_block_;
  }

  GrowableListIterator GetBasicBlockIterator() {
    GrowableListIterator iterator;
    GrowableListIteratorInit(&block_list_, &iterator);
    return iterator;
  }

  BasicBlock* GetBasicBlock(int block_id) const {
    return reinterpret_cast<BasicBlock*>(GrowableListGetElement(&block_list_, block_id));
  }

  size_t GetBasicBlockListCount() const {
    return block_list_.num_used;
  }

  GrowableList* GetBlockList() {
    return &block_list_;
  }

  GrowableList* GetDfsOrder() {
    return &dfs_order_;
  }

  GrowableList* GetDfsPostOrder() {
    return &dfs_post_order_;
  }

  GrowableList* GetDomPostOrder() {
    return &dom_post_order_traversal_;
  }

  GrowableList* GetSSASubscripts() {
    return ssa_subscripts_;
  }

  int GetDefCount() const {
    return def_count_;
  }

  void EnableOpcodeCounting() {
    opcode_count_ = static_cast<int*>(NewMem(cu_, kNumPackedOpcodes * sizeof(int), true,
                                             kAllocMisc));
  }

  void ShowOpcodeStats();

  DexCompilationUnit* GetCurrentDexCompilationUnit() const {
    return m_units_[current_method_];
  }

  void DumpCFG(const char* dir_prefix, bool all_blocks);

  void BuildRegLocations();

  void DumpRegLocTable(RegLocation* table, int count);

  int ComputeFrameSize();

  void BasicBlockOptimization();

  bool IsConst(int32_t s_reg) const {
    return (IsBitSet(is_constant_v_, s_reg));
  }

  bool IsConst(RegLocation loc) const {
    return (IsConst(loc.orig_sreg));
  }

  int32_t ConstantValue(RegLocation loc) const {
    DCHECK(IsConst(loc));
    return constant_values_[loc.orig_sreg];
  }

  int32_t ConstantValue(int32_t s_reg) const {
    DCHECK(IsConst(s_reg));
    return constant_values_[s_reg];
  }

  int64_t ConstantValueWide(RegLocation loc) const {
    DCHECK(IsConst(loc));
    return (static_cast<int64_t>(constant_values_[loc.orig_sreg + 1]) << 32) |
        Low32Bits(static_cast<int64_t>(constant_values_[loc.orig_sreg]));
  }

  bool IsConstantNullRef(RegLocation loc) const {
    return loc.ref && loc.is_const && (ConstantValue(loc) == 0);
  }

  int GetNumSSARegs() const {
    return num_ssa_regs_;
  }

  void SetNumSSARegs(int new_num) {
    num_ssa_regs_ = new_num;
  }

  int GetNumReachableBlocks() const {
    return num_reachable_blocks_;
  }

  int GetUseCount(int vreg) const {
    return GrowableListGetElement(&use_counts_, vreg);
  }

  int GetRawUseCount(int vreg) const {
    return GrowableListGetElement(&raw_use_counts_, vreg);
  }

  int GetSSASubscript(int ssa_reg) const {
    return GrowableListGetElement(ssa_subscripts_, ssa_reg);
  }

  const char* GetSSAString(int ssa_reg) const {
    return GET_ELEM_N(ssa_strings_, char*, ssa_reg);
  }

  void BasicBlockCombine();
  void CodeLayout();
  void DumpCheckStats();
  void PropagateConstants();
  MIR* FindMoveResult(BasicBlock* bb, MIR* mir);
  int SRegToVReg(int ssa_reg) const;
  void VerifyDataflow();
  void MethodUseCount();
  void SSATransformation();
  void CheckForDominanceFrontier(BasicBlock* dom_bb, const BasicBlock* succ_bb);
  void NullCheckElimination();

  /*
   * IsDebugBuild sanity check: keep track of the Dex PCs for catch entries so that later on
   * we can verify that all catch entries have native PC entries.
   */
   std::set<uint32_t> catches_;

 private:

   int FindCommonParent(int block1, int block2);
   void ComputeSuccLineIn(ArenaBitVector* dest, const ArenaBitVector* src1,
                          const ArenaBitVector* src2);
   void HandleLiveInUse(ArenaBitVector* use_v, ArenaBitVector* def_v,
                        ArenaBitVector* live_in_v, int dalvik_reg_id);
   void HandleDef(ArenaBitVector* def_v, int dalvik_reg_id);
   void CompilerInitializeSSAConversion();
   bool DoSSAConversion(BasicBlock* bb);
   bool InvokeUsesMethodStar(MIR* mir);
   int ParseInsn(const uint16_t* code_ptr, DecodedInstruction* decoded_instruction);
   bool ContentIsInsn(const uint16_t* code_ptr);
   BasicBlock* SplitBlock(unsigned int code_offset, BasicBlock* orig_block,
                          BasicBlock** immed_pred_block_p);
   BasicBlock* FindBlock(unsigned int code_offset, bool split, bool create,
                         BasicBlock** immed_pred_block_p);
   void ProcessTryCatchBlocks();
   BasicBlock* ProcessCanBranch(BasicBlock* cur_block, MIR* insn, int cur_offset, int width,
                                int flags, const uint16_t* code_ptr, const uint16_t* code_end);
   void ProcessCanSwitch(BasicBlock* cur_block, MIR* insn, int cur_offset, int width, int flags);
   BasicBlock* ProcessCanThrow(BasicBlock* cur_block, MIR* insn, int cur_offset, int width,
                               int flags, ArenaBitVector* try_block_addr, const uint16_t* code_ptr,
                               const uint16_t* code_end);
   int AddNewSReg(int v_reg);
   void HandleSSAUse(int* uses, int dalvik_reg, int reg_index);
   void HandleSSADef(int* defs, int dalvik_reg, int reg_index);
   void DataFlowSSAFormat35C(MIR* mir);
   void DataFlowSSAFormat3RC(MIR* mir);
   bool FindLocalLiveIn(BasicBlock* bb);
   bool ClearVisitedFlag(struct BasicBlock* bb);
   bool CountUses(struct BasicBlock* bb);
   bool InferTypeAndSize(BasicBlock* bb);
   bool VerifyPredInfo(BasicBlock* bb);
   BasicBlock* NeedsVisit(BasicBlock* bb);
   BasicBlock* NextUnvisitedSuccessor(BasicBlock* bb);
   void MarkPreOrder(BasicBlock* bb);
   void RecordDFSOrders(BasicBlock* bb);
   void ComputeDFSOrders();
   void ComputeDefBlockMatrix();
   void ComputeDomPostOrderTraversal(BasicBlock* bb);
   void ComputeDominators();
   void InsertPhiNodes();
   void DoDFSPreOrderSSARename(BasicBlock* block);
   void SetConstant(int32_t ssa_reg, int value);
   void SetConstantWide(int ssa_reg, int64_t value);
   int GetSSAUseCount(int s_reg);
   bool BasicBlockOpt(BasicBlock* bb);
   bool EliminateNullChecks(BasicBlock* bb);
   bool NullCheckEliminationInit(BasicBlock* bb);
   bool BuildExtendedBBList(struct BasicBlock* bb);
   bool FillDefBlockMatrix(BasicBlock* bb);
   bool InitializeDominationInfo(BasicBlock* bb);
   bool ComputeblockIDom(BasicBlock* bb);
   bool ComputeBlockDominators(BasicBlock* bb);
   bool SetDominators(BasicBlock* bb);
   bool ComputeBlockLiveIns(BasicBlock* bb);
   bool InsertPhiNodeOperands(BasicBlock* bb);
   bool ComputeDominanceFrontier(BasicBlock* bb);
   bool DoConstantPropogation(BasicBlock* bb);
   bool CountChecks(BasicBlock* bb);
   bool CombineBlocks(BasicBlock* bb);

   CompilationUnit* const cu_;
   GrowableList* ssa_base_vregs_;
   GrowableList* ssa_subscripts_;
   GrowableList* ssa_strings_;
   // Map original Dalvik virtual reg i to the current SSA name.
   int* vreg_to_ssa_map_;            // length == method->registers_size
   int* ssa_last_defs_;              // length == method->registers_size
   ArenaBitVector* is_constant_v_;   // length == num_ssa_reg
   int* constant_values_;            // length == num_ssa_reg
   // Use counts of ssa names.
   GrowableList use_counts_;         // Weighted by nesting depth
   GrowableList raw_use_counts_;     // Not weighted
   int num_reachable_blocks_;
   GrowableList dfs_order_;
   GrowableList dfs_post_order_;
   GrowableList dom_post_order_traversal_;
   int* i_dom_list_;
   ArenaBitVector** def_block_matrix_;    // num_dalvik_register x num_blocks.
   ArenaBitVector* temp_block_v_;
   ArenaBitVector* temp_dalvik_register_v_;
   ArenaBitVector* temp_ssa_register_v_;  // num_ssa_regs.
   static const int kInvalidEntry = -1;
   GrowableList block_list_;
   ArenaBitVector* try_block_addr_;
   BasicBlock* entry_block_;
   BasicBlock* exit_block_;
   BasicBlock* cur_block_;
   int num_blocks_;
   const DexFile::CodeItem* current_code_item_;
   SafeMap<unsigned int, BasicBlock*> block_map_; // FindBlock lookup cache.
   std::vector<DexCompilationUnit*> m_units_;     // List of methods included in this graph
   typedef std::pair<int, int> MIRLocation;       // Insert point, (m_unit_ index, offset)
   std::vector<MIRLocation> method_stack_;        // Include stack
   int current_method_;
   int current_offset_;
   int def_count_;                                // Used to estimate size of ssa name storage.
   int* opcode_count_;                            // Dex opcode coverage stats.
   int num_ssa_regs_;                             // Number of names following SSA transformation.
   std::vector<BasicBlock*> extended_basic_blocks_; // Heads of block "traces".
};

}  // namespace art

#endif // ART_SRC_COMPILER_DEX_MIRGRAPH_H_
