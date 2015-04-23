/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include "base/logging.h"
#include "compiler_ir.h"
#include "dataflow_iterator-inl.h"
#include "dex_flags.h"
#include "dex/mir_field_info.h"
#include "dex/mir_graph.h"
#include "driver/dex_compilation_unit.h"
#include "gtest/gtest.h"
#include "type_inference.h"
#include "utils/test_dex_file_builder.h"

namespace art {

class TypeInferenceTest : public testing::Test {
 protected:
  struct TypeDef {
    const char* descriptor;
  };

  struct FieldDef {
    const char* class_descriptor;
    const char* type;
    const char* name;
  };

  struct MethodDef {
    const char* class_descriptor;
    const char* signature;
    const char* name;
    InvokeType type;
  };

  struct BBDef {
    static constexpr size_t kMaxSuccessors = 4;
    static constexpr size_t kMaxPredecessors = 4;

    BBType type;
    size_t num_successors;
    BasicBlockId successors[kMaxPredecessors];
    size_t num_predecessors;
    BasicBlockId predecessors[kMaxPredecessors];
  };

  struct MIRDef {
    static constexpr size_t kMaxSsaDefs = 2;
    static constexpr size_t kMaxSsaUses = 4;

    BasicBlockId bbid;
    Instruction::Code opcode;
    int64_t value;
    uint32_t metadata;
    size_t num_uses;
    int32_t uses[kMaxSsaUses];
    size_t num_defs;
    int32_t defs[kMaxSsaDefs];
  };

#define DEF_SUCC0() \
    0u, { }
#define DEF_SUCC1(s1) \
    1u, { s1 }
#define DEF_SUCC2(s1, s2) \
    2u, { s1, s2 }
#define DEF_SUCC3(s1, s2, s3) \
    3u, { s1, s2, s3 }
#define DEF_SUCC4(s1, s2, s3, s4) \
    4u, { s1, s2, s3, s4 }
#define DEF_PRED0() \
    0u, { }
#define DEF_PRED1(p1) \
    1u, { p1 }
#define DEF_PRED2(p1, p2) \
    2u, { p1, p2 }
#define DEF_PRED3(p1, p2, p3) \
    3u, { p1, p2, p3 }
#define DEF_PRED4(p1, p2, p3, p4) \
    4u, { p1, p2, p3, p4 }
#define DEF_BB(type, succ, pred) \
    { type, succ, pred }

#define DEF_CONST(bb, opcode, reg, value) \
    { bb, opcode, value, 0u, 0, { }, 1, { reg } }
#define DEF_CONST_WIDE(bb, opcode, reg, value) \
    { bb, opcode, value, 0u, 0, { }, 2, { reg, reg + 1 } }
#define DEF_CONST_STRING(bb, opcode, reg, index) \
    { bb, opcode, index, 0u, 0, { }, 1, { reg } }
#define DEF_IGET(bb, opcode, reg, obj, field_info) \
    { bb, opcode, 0u, field_info, 1, { obj }, 1, { reg } }
#define DEF_IGET_WIDE(bb, opcode, reg, obj, field_info) \
    { bb, opcode, 0u, field_info, 1, { obj }, 2, { reg, reg + 1 } }
#define DEF_IPUT(bb, opcode, reg, obj, field_info) \
    { bb, opcode, 0u, field_info, 2, { reg, obj }, 0, { } }
#define DEF_IPUT_WIDE(bb, opcode, reg, obj, field_info) \
    { bb, opcode, 0u, field_info, 3, { reg, reg + 1, obj }, 0, { } }
#define DEF_SGET(bb, opcode, reg, field_info) \
    { bb, opcode, 0u, field_info, 0, { }, 1, { reg } }
#define DEF_SGET_WIDE(bb, opcode, reg, field_info) \
    { bb, opcode, 0u, field_info, 0, { }, 2, { reg, reg + 1 } }
#define DEF_SPUT(bb, opcode, reg, field_info) \
    { bb, opcode, 0u, field_info, 1, { reg }, 0, { } }
#define DEF_SPUT_WIDE(bb, opcode, reg, field_info) \
    { bb, opcode, 0u, field_info, 2, { reg, reg + 1 }, 0, { } }
#define DEF_AGET(bb, opcode, reg, obj, idx) \
    { bb, opcode, 0u, 0u, 2, { obj, idx }, 1, { reg } }
#define DEF_AGET_WIDE(bb, opcode, reg, obj, idx) \
    { bb, opcode, 0u, 0u, 2, { obj, idx }, 2, { reg, reg + 1 } }
#define DEF_APUT(bb, opcode, reg, obj, idx) \
    { bb, opcode, 0u, 0u, 3, { reg, obj, idx }, 0, { } }
#define DEF_APUT_WIDE(bb, opcode, reg, obj, idx) \
    { bb, opcode, 0u, 0u, 4, { reg, reg + 1, obj, idx }, 0, { } }
#define DEF_INVOKE0(bb, opcode, method_idx) \
    { bb, opcode, 0u, method_idx, 0, { }, 0, { } }
#define DEF_INVOKE1(bb, opcode, reg, method_idx) \
    { bb, opcode, 0u, method_idx, 1, { reg }, 0, { } }
#define DEF_INVOKE2(bb, opcode, reg1, reg2, method_idx) \
    { bb, opcode, 0u, method_idx, 2, { reg1, reg2 }, 0, { } }
#define DEF_IFZ(bb, opcode, reg) \
    { bb, opcode, 0u, 0u, 1, { reg }, 0, { } }
#define DEF_MOVE(bb, opcode, reg, src) \
    { bb, opcode, 0u, 0u, 1, { src }, 1, { reg } }
#define DEF_MOVE_WIDE(bb, opcode, reg, src) \
    { bb, opcode, 0u, 0u, 2, { src, src + 1 }, 2, { reg, reg + 1 } }
#define DEF_PHI2(bb, reg, src1, src2) \
    { bb, static_cast<Instruction::Code>(kMirOpPhi), 0, 0u, 2u, { src1, src2 }, 1, { reg } }
#define DEF_BINOP(bb, opcode, result, src1, src2) \
    { bb, opcode, 0u, 0u, 2, { src1, src2 }, 1, { result } }
#define DEF_UNOP(bb, opcode, result, src) DEF_MOVE(bb, opcode, result, src)
#define DEF_NULOP(bb, opcode, result) DEF_CONST(bb, opcode, result, 0)
#define DEF_NULOP_WIDE(bb, opcode, result) DEF_CONST_WIDE(bb, opcode, result, 0)
#define DEF_CHECK_CAST(bb, opcode, reg, type) \
    { bb, opcode, 0, type, 1, { reg }, 0, { } }
#define DEF_NEW_ARRAY(bb, opcode, reg, length, type) \
    { bb, opcode, 0, type, 1, { length }, 1, { reg } }

  void AddTypes(const TypeDef* defs, size_t count) {
    for (size_t i = 0; i != count; ++i) {
      const TypeDef* def = &defs[i];
      dex_file_builder_.AddType(def->descriptor);
    }
  }

  template <size_t count>
  void PrepareTypes(const TypeDef (&defs)[count]) {
    type_defs_ = defs;
    type_count_ = count;
    AddTypes(defs, count);
  }

  void AddFields(const FieldDef* defs, size_t count) {
    for (size_t i = 0; i != count; ++i) {
      const FieldDef* def = &defs[i];
      dex_file_builder_.AddField(def->class_descriptor, def->type, def->name);
    }
  }

  template <size_t count>
  void PrepareIFields(const FieldDef (&defs)[count]) {
    ifield_defs_ = defs;
    ifield_count_ = count;
    AddFields(defs, count);
  }

  template <size_t count>
  void PrepareSFields(const FieldDef (&defs)[count]) {
    sfield_defs_ = defs;
    sfield_count_ = count;
    AddFields(defs, count);
  }

  void AddMethods(const MethodDef* defs, size_t count) {
    for (size_t i = 0; i != count; ++i) {
      const MethodDef* def = &defs[i];
      dex_file_builder_.AddMethod(def->class_descriptor, def->signature, def->name);
    }
  }

  template <size_t count>
  void PrepareMethods(const MethodDef (&defs)[count]) {
    method_defs_ = defs;
    method_count_ = count;
    AddMethods(defs, count);
  }

  DexMemAccessType AccessTypeForDescriptor(const char* descriptor) {
    switch (descriptor[0]) {
      case 'I':
      case 'F':
        return kDexMemAccessWord;
      case 'J':
      case 'D':
        return kDexMemAccessWide;
      case '[':
      case 'L':
        return kDexMemAccessObject;
      case 'Z':
        return kDexMemAccessBoolean;
      case 'B':
        return kDexMemAccessByte;
      case 'C':
        return kDexMemAccessChar;
      case 'S':
        return kDexMemAccessShort;
      default:
        LOG(FATAL) << "Bad descriptor: " << descriptor;
        UNREACHABLE();
    }
  }

  size_t CountIns(const std::string& test_method_signature, bool is_static) {
    const char* sig = test_method_signature.c_str();
    CHECK_EQ(sig[0], '(');
    ++sig;
    size_t result = is_static ? 0u : 1u;
    while (*sig != ')') {
      result += (AccessTypeForDescriptor(sig) == kDexMemAccessWide) ? 2u : 1u;
      while (*sig == '[') {
        ++sig;
      }
      if (*sig == 'L') {
        do {
          ++sig;
          CHECK(*sig != '\0' && *sig != ')');
        } while (*sig != ';');
      }
      ++sig;
    }
    return result;
  }

  void BuildDexFile(const std::string& test_method_signature, bool is_static) {
    dex_file_builder_.AddMethod(kClassName, test_method_signature, kMethodName);
    dex_file_ = dex_file_builder_.Build(kDexLocation);
    cu_.dex_file = dex_file_.get();
    cu_.method_idx = dex_file_builder_.GetMethodIdx(kClassName, test_method_signature, kMethodName);
    cu_.access_flags = is_static ? kAccStatic : 0u;
    cu_.mir_graph->m_units_.push_back(new (cu_.mir_graph->arena_) DexCompilationUnit(
        &cu_, cu_.class_loader, cu_.class_linker, *cu_.dex_file, nullptr /* code_item not used */,
        0u /* class_def_idx not used */, 0u /* method_index not used */,
        cu_.access_flags, nullptr /* verified_method not used */));
    cu_.mir_graph->current_method_ = 0u;
    code_item_ = static_cast<DexFile::CodeItem*>(
        cu_.arena.Alloc(sizeof(DexFile::CodeItem), kArenaAllocMisc));

    code_item_->ins_size_ = CountIns(test_method_signature, is_static);
    code_item_->registers_size_ = kLocalVRs + code_item_->ins_size_;
    cu_.mir_graph->current_code_item_ = code_item_;
    cu_.mir_graph->num_ssa_regs_ = kMaxSsaRegs;

    cu_.mir_graph->ifield_lowering_infos_.clear();
    cu_.mir_graph->ifield_lowering_infos_.reserve(ifield_count_);
    for (size_t i = 0u; i != ifield_count_; ++i) {
      const FieldDef* def = &ifield_defs_[i];
      uint32_t field_idx =
          dex_file_builder_.GetFieldIdx(def->class_descriptor, def->type, def->name);
      MirIFieldLoweringInfo field_info(field_idx, AccessTypeForDescriptor(def->type), false);
      field_info.declaring_dex_file_ = cu_.dex_file;
      field_info.declaring_field_idx_ = field_idx;
      cu_.mir_graph->ifield_lowering_infos_.push_back(field_info);
    }

    cu_.mir_graph->sfield_lowering_infos_.clear();
    cu_.mir_graph->sfield_lowering_infos_.reserve(sfield_count_);
    for (size_t i = 0u; i != sfield_count_; ++i) {
      const FieldDef* def = &sfield_defs_[i];
      uint32_t field_idx =
          dex_file_builder_.GetFieldIdx(def->class_descriptor, def->type, def->name);
      MirSFieldLoweringInfo field_info(field_idx, AccessTypeForDescriptor(def->type));
      field_info.declaring_dex_file_ = cu_.dex_file;
      field_info.declaring_field_idx_ = field_idx;
      cu_.mir_graph->sfield_lowering_infos_.push_back(field_info);
    }

    cu_.mir_graph->method_lowering_infos_.clear();
    cu_.mir_graph->method_lowering_infos_.reserve(ifield_count_);
    for (size_t i = 0u; i != method_count_; ++i) {
      const MethodDef* def = &method_defs_[i];
      uint32_t method_idx =
          dex_file_builder_.GetMethodIdx(def->class_descriptor, def->signature, def->name);
      MirMethodLoweringInfo method_info(method_idx, def->type, false);
      method_info.declaring_dex_file_ = cu_.dex_file;
      method_info.declaring_method_idx_ = method_idx;
      cu_.mir_graph->method_lowering_infos_.push_back(method_info);
    }
  }

  void DoPrepareBasicBlocks(const BBDef* defs, size_t count) {
    cu_.mir_graph->block_id_map_.clear();
    cu_.mir_graph->block_list_.clear();
    ASSERT_LT(3u, count);  // null, entry, exit and at least one bytecode block.
    ASSERT_EQ(kNullBlock, defs[0].type);
    ASSERT_EQ(kEntryBlock, defs[1].type);
    ASSERT_EQ(kExitBlock, defs[2].type);
    for (size_t i = 0u; i != count; ++i) {
      const BBDef* def = &defs[i];
      BasicBlock* bb = cu_.mir_graph->CreateNewBB(def->type);
      if (def->num_successors <= 2) {
        bb->successor_block_list_type = kNotUsed;
        bb->fall_through = (def->num_successors >= 1) ? def->successors[0] : 0u;
        bb->taken = (def->num_successors >= 2) ? def->successors[1] : 0u;
      } else {
        bb->successor_block_list_type = kPackedSwitch;
        bb->fall_through = 0u;
        bb->taken = 0u;
        bb->successor_blocks.reserve(def->num_successors);
        for (size_t j = 0u; j != def->num_successors; ++j) {
          SuccessorBlockInfo* successor_block_info =
              static_cast<SuccessorBlockInfo*>(cu_.arena.Alloc(sizeof(SuccessorBlockInfo),
                                                               kArenaAllocSuccessor));
          successor_block_info->block = j;
          successor_block_info->key = 0u;  // Not used by class init check elimination.
          bb->successor_blocks.push_back(successor_block_info);
        }
      }
      bb->predecessors.assign(def->predecessors, def->predecessors + def->num_predecessors);
      if (def->type == kDalvikByteCode || def->type == kEntryBlock || def->type == kExitBlock) {
        bb->data_flow_info = static_cast<BasicBlockDataFlow*>(
            cu_.arena.Alloc(sizeof(BasicBlockDataFlow), kArenaAllocDFInfo));
        bb->data_flow_info->live_in_v = live_in_v_;
      }
    }
    ASSERT_EQ(count, cu_.mir_graph->block_list_.size());
    cu_.mir_graph->entry_block_ = cu_.mir_graph->block_list_[1];
    ASSERT_EQ(kEntryBlock, cu_.mir_graph->entry_block_->block_type);
    cu_.mir_graph->exit_block_ = cu_.mir_graph->block_list_[2];
    ASSERT_EQ(kExitBlock, cu_.mir_graph->exit_block_->block_type);
  }

  template <size_t count>
  void PrepareBasicBlocks(const BBDef (&defs)[count]) {
    DoPrepareBasicBlocks(defs, count);
  }

  void PrepareSingleBlock() {
    static const BBDef bbs[] = {
        DEF_BB(kNullBlock, DEF_SUCC0(), DEF_PRED0()),
        DEF_BB(kEntryBlock, DEF_SUCC1(3), DEF_PRED0()),
        DEF_BB(kExitBlock, DEF_SUCC0(), DEF_PRED1(3)),
        DEF_BB(kDalvikByteCode, DEF_SUCC1(2), DEF_PRED1(1)),
    };
    PrepareBasicBlocks(bbs);
  }

  void PrepareDiamond() {
    static const BBDef bbs[] = {
        DEF_BB(kNullBlock, DEF_SUCC0(), DEF_PRED0()),
        DEF_BB(kEntryBlock, DEF_SUCC1(3), DEF_PRED0()),
        DEF_BB(kExitBlock, DEF_SUCC0(), DEF_PRED1(6)),
        DEF_BB(kDalvikByteCode, DEF_SUCC2(4, 5), DEF_PRED1(1)),
        DEF_BB(kDalvikByteCode, DEF_SUCC1(6), DEF_PRED1(3)),
        DEF_BB(kDalvikByteCode, DEF_SUCC1(6), DEF_PRED1(3)),
        DEF_BB(kDalvikByteCode, DEF_SUCC1(2), DEF_PRED2(4, 5)),
    };
    PrepareBasicBlocks(bbs);
  }

  void PrepareLoop() {
    static const BBDef bbs[] = {
        DEF_BB(kNullBlock, DEF_SUCC0(), DEF_PRED0()),
        DEF_BB(kEntryBlock, DEF_SUCC1(3), DEF_PRED0()),
        DEF_BB(kExitBlock, DEF_SUCC0(), DEF_PRED1(5)),
        DEF_BB(kDalvikByteCode, DEF_SUCC1(4), DEF_PRED1(1)),
        DEF_BB(kDalvikByteCode, DEF_SUCC2(5, 4), DEF_PRED2(3, 4)),  // "taken" loops to self.
        DEF_BB(kDalvikByteCode, DEF_SUCC1(2), DEF_PRED1(4)),
    };
    PrepareBasicBlocks(bbs);
  }

  void DoPrepareMIRs(const MIRDef* defs, size_t count) {
    mir_count_ = count;
    mirs_ = cu_.arena.AllocArray<MIR>(count, kArenaAllocMIR);
    ssa_reps_.resize(count);
    for (size_t i = 0u; i != count; ++i) {
      const MIRDef* def = &defs[i];
      MIR* mir = &mirs_[i];
      ASSERT_LT(def->bbid, cu_.mir_graph->block_list_.size());
      BasicBlock* bb = cu_.mir_graph->block_list_[def->bbid];
      bb->AppendMIR(mir);
      mir->dalvikInsn.opcode = def->opcode;
      mir->dalvikInsn.vB = static_cast<int32_t>(def->value);
      mir->dalvikInsn.vB_wide = def->value;
      if (IsInstructionIGetOrIPut(def->opcode)) {
        ASSERT_LT(def->metadata, cu_.mir_graph->ifield_lowering_infos_.size());
        mir->meta.ifield_lowering_info = def->metadata;
        ASSERT_EQ(cu_.mir_graph->ifield_lowering_infos_[def->metadata].MemAccessType(),
                  IGetOrIPutMemAccessType(def->opcode));
        cu_.mir_graph->merged_df_flags_ |= DF_IFIELD;
      } else if (IsInstructionSGetOrSPut(def->opcode)) {
        ASSERT_LT(def->metadata, cu_.mir_graph->sfield_lowering_infos_.size());
        mir->meta.sfield_lowering_info = def->metadata;
        ASSERT_EQ(cu_.mir_graph->sfield_lowering_infos_[def->metadata].MemAccessType(),
                  SGetOrSPutMemAccessType(def->opcode));
        cu_.mir_graph->merged_df_flags_ |= DF_SFIELD;
      } else if (IsInstructionInvoke(def->opcode)) {
        ASSERT_LT(def->metadata, cu_.mir_graph->method_lowering_infos_.size());
        mir->meta.method_lowering_info = def->metadata;
        mir->dalvikInsn.vA = def->num_uses;
        cu_.mir_graph->merged_df_flags_ |= DF_FORMAT_35C;
      } else if (def->opcode == static_cast<Instruction::Code>(kMirOpPhi)) {
        mir->meta.phi_incoming =
            allocator_->AllocArray<BasicBlockId>(def->num_uses, kArenaAllocDFInfo);
        ASSERT_EQ(def->num_uses, bb->predecessors.size());
        std::copy(bb->predecessors.begin(), bb->predecessors.end(), mir->meta.phi_incoming);
      } else if (def->opcode == Instruction::CHECK_CAST) {
        ASSERT_LT(def->metadata, type_count_);
        mir->dalvikInsn.vB = dex_file_builder_.GetTypeIdx(type_defs_[def->metadata].descriptor);
        cu_.mir_graph->merged_df_flags_ |= DF_CHK_CAST;
      } else if (def->opcode == Instruction::NEW_ARRAY) {
        ASSERT_LT(def->metadata, type_count_);
        mir->dalvikInsn.vC = dex_file_builder_.GetTypeIdx(type_defs_[def->metadata].descriptor);
      }
      mir->ssa_rep = &ssa_reps_[i];
      mir->ssa_rep->num_uses = def->num_uses;
      mir->ssa_rep->uses = const_cast<int32_t*>(def->uses);  // Not modified by LVN.
      mir->ssa_rep->num_defs = def->num_defs;
      mir->ssa_rep->defs = const_cast<int32_t*>(def->defs);  // Not modified by LVN.
      mir->dalvikInsn.opcode = def->opcode;
      mir->offset = i;  // LVN uses offset only for debug output
      mir->optimization_flags = 0u;
    }
    code_item_->insns_size_in_code_units_ = 2u * count;
  }

  template <size_t count>
  void PrepareMIRs(const MIRDef (&defs)[count]) {
    DoPrepareMIRs(defs, count);
  }

  // BasicBlockDataFlow::vreg_to_ssa_map_exit is used only for check-casts.
  void AllocEndingVRegToSRegMaps() {
    AllNodesIterator iterator(cu_.mir_graph.get());
    for (BasicBlock* bb = iterator.Next(); bb != nullptr; bb = iterator.Next()) {
      if (bb->data_flow_info != nullptr) {
        if (bb->data_flow_info->vreg_to_ssa_map_exit == nullptr) {
          size_t num_vregs = code_item_->registers_size_;
          bb->data_flow_info->vreg_to_ssa_map_exit = static_cast<int32_t*>(
              cu_.arena.AllocArray<int32_t>(num_vregs, kArenaAllocDFInfo));
          std::fill_n(bb->data_flow_info->vreg_to_ssa_map_exit, num_vregs, INVALID_SREG);
        }
      }
    }
  }

  template <size_t count>
  void MapVRegToSReg(int vreg, int32_t sreg, const BasicBlockId (&bb_ids)[count]) {
    AllocEndingVRegToSRegMaps();
    for (BasicBlockId bb_id : bb_ids) {
      BasicBlock* bb = cu_.mir_graph->GetBasicBlock(bb_id);
      CHECK(bb != nullptr);
      CHECK(bb->data_flow_info != nullptr);
      CHECK(bb->data_flow_info->vreg_to_ssa_map_exit != nullptr);
      bb->data_flow_info->vreg_to_ssa_map_exit[vreg] = sreg;
    }
  }

  void PerformTypeInference() {
    cu_.mir_graph->SSATransformationStart();
    cu_.mir_graph->ComputeDFSOrders();
    cu_.mir_graph->ComputeDominators();
    cu_.mir_graph->ComputeTopologicalSortOrder();
    cu_.mir_graph->SSATransformationEnd();
    ASSERT_TRUE(type_inference_ == nullptr);
    type_inference_.reset(new (allocator_.get()) TypeInference(cu_.mir_graph.get(),
                                                               allocator_.get()));
    RepeatingPreOrderDfsIterator iter(cu_.mir_graph.get());
    bool changed = false;
    for (BasicBlock* bb = iter.Next(changed); bb != nullptr; bb = iter.Next(changed)) {
      changed = type_inference_->Apply(bb);
    }
    type_inference_->Finish();
  }

  TypeInferenceTest()
      : pool_(),
        cu_(&pool_, kRuntimeISA, nullptr, nullptr),
        mir_count_(0u),
        mirs_(nullptr),
        code_item_(nullptr),
        ssa_reps_(),
        allocator_(),
        live_in_v_(new (&cu_.arena) ArenaBitVector(&cu_.arena, kMaxSsaRegs, false, kBitMapMisc)),
        type_defs_(nullptr),
        type_count_(0u),
        ifield_defs_(nullptr),
        ifield_count_(0u),
        sfield_defs_(nullptr),
        sfield_count_(0u),
        method_defs_(nullptr),
        method_count_(0u),
        dex_file_builder_(),
        dex_file_(nullptr) {
    cu_.mir_graph.reset(new MIRGraph(&cu_, &cu_.arena));
    allocator_.reset(ScopedArenaAllocator::Create(&cu_.arena_stack));
    // Bind all possible sregs to live vregs for test purposes.
    live_in_v_->SetInitialBits(kMaxSsaRegs);
    cu_.mir_graph->reg_location_ = static_cast<RegLocation*>(cu_.arena.Alloc(
        kMaxSsaRegs * sizeof(cu_.mir_graph->reg_location_[0]), kArenaAllocRegAlloc));
    cu_.mir_graph->method_sreg_ = kMaxSsaRegs - 1u;
    cu_.mir_graph->reg_location_[cu_.mir_graph->GetMethodSReg()].location = kLocCompilerTemp;
    // Bind all possible sregs to live vregs for test purposes.
    live_in_v_->SetInitialBits(kMaxSsaRegs);
    cu_.mir_graph->ssa_base_vregs_.reserve(kMaxSsaRegs);
    cu_.mir_graph->ssa_subscripts_.reserve(kMaxSsaRegs);
    for (unsigned int i = 0; i < kMaxSsaRegs; i++) {
      cu_.mir_graph->ssa_base_vregs_.push_back(i);
      cu_.mir_graph->ssa_subscripts_.push_back(0);
    }
  }

  enum ExpectFlags : uint32_t {
    kExpectWide         = 0x0001u,
    kExpectNarrow       = 0x0002u,
    kExpectFp           = 0x0004u,
    kExpectCore         = 0x0008u,
    kExpectRef          = 0x0010u,
    kExpectArrayWide    = 0x0020u,
    kExpectArrayNarrow  = 0x0040u,
    kExpectArrayFp      = 0x0080u,
    kExpectArrayCore    = 0x0100u,
    kExpectArrayRef     = 0x0200u,
    kExpectNull         = 0x0400u,
    kExpectHigh         = 0x0800u,  // Reserved for ExpectSRegType().
  };

  struct SRegExpectation {
    uint32_t array_depth;
    uint32_t flags;
  };

  void ExpectSRegType(int s_reg, const SRegExpectation& expectation, bool check_loc = true) {
    uint32_t flags = expectation.flags;
    uint32_t array_depth = expectation.array_depth;
    TypeInference::Type type = type_inference_->sregs_[s_reg];

    if (check_loc) {
      RegLocation loc = cu_.mir_graph->reg_location_[s_reg];
      EXPECT_EQ((flags & kExpectWide) != 0u, loc.wide) << s_reg;
      EXPECT_EQ((flags & kExpectFp) != 0u, loc.fp) << s_reg;
      EXPECT_EQ((flags & kExpectCore) != 0u, loc.core) << s_reg;
      EXPECT_EQ((flags & kExpectRef) != 0u, loc.ref) << s_reg;
      EXPECT_EQ((flags & kExpectHigh) != 0u, loc.high_word) << s_reg;
    }

    EXPECT_EQ((flags & kExpectWide) != 0u, type.Wide()) << s_reg;
    EXPECT_EQ((flags & kExpectNarrow) != 0u, type.Narrow()) << s_reg;
    EXPECT_EQ((flags & kExpectFp) != 0u, type.Fp()) << s_reg;
    EXPECT_EQ((flags & kExpectCore) != 0u, type.Core()) << s_reg;
    EXPECT_EQ((flags & kExpectRef) != 0u, type.Ref()) << s_reg;
    EXPECT_EQ((flags & kExpectHigh) == 0u, type.LowWord()) << s_reg;
    EXPECT_EQ((flags & kExpectHigh) != 0u, type.HighWord()) << s_reg;

    if ((flags & kExpectRef) != 0u) {
      EXPECT_EQ((flags & kExpectNull) != 0u, !type.NonNull()) << s_reg;
    } else {
      // Null should be checked only for references.
      ASSERT_EQ((flags & kExpectNull), 0u);
    }

    ASSERT_EQ(array_depth, type.ArrayDepth()) << s_reg;
    if (array_depth != 0u) {
      ASSERT_NE((flags & kExpectRef), 0u);
      TypeInference::Type nested_type = type.NestedType();
      EXPECT_EQ((flags & kExpectArrayWide) != 0u, nested_type.Wide()) << s_reg;
      EXPECT_EQ((flags & kExpectArrayNarrow) != 0u, nested_type.Narrow()) << s_reg;
      EXPECT_EQ((flags & kExpectArrayFp) != 0u, nested_type.Fp()) << s_reg;
      EXPECT_EQ((flags & kExpectArrayCore) != 0u, nested_type.Core()) << s_reg;
      EXPECT_EQ((flags & kExpectArrayRef) != 0u, nested_type.Ref()) << s_reg;
    }
    if (!type.Narrow() && type.LowWord() &&
        (expectation.flags & (kExpectWide | kExpectNarrow | kExpectHigh)) == kExpectWide) {
      SRegExpectation high_expectation = { array_depth, flags | kExpectHigh };
      ExpectSRegType(s_reg + 1, high_expectation);
    }
  }

  void ExpectCore(int s_reg, bool core) {
    EXPECT_EQ(core, type_inference_->sregs_[s_reg].Core());
  }

  void ExpectRef(int s_reg, bool ref) {
    EXPECT_EQ(ref, type_inference_->sregs_[s_reg].Ref());
  }

  void ExpectArrayDepth(int s_reg, uint32_t array_depth) {
    EXPECT_EQ(array_depth, type_inference_->sregs_[s_reg].ArrayDepth());
  }

  static constexpr size_t kMaxSsaRegs = 16384u;
  static constexpr uint16_t kLocalVRs = 1000u;

  static constexpr const char* kDexLocation = "TypeInferenceDexFile;";
  static constexpr const char* kClassName = "LTypeInferenceTest;";
  static constexpr const char* kMethodName = "test";

  ArenaPool pool_;
  CompilationUnit cu_;
  size_t mir_count_;
  MIR* mirs_;
  DexFile::CodeItem* code_item_;
  std::vector<SSARepresentation> ssa_reps_;
  std::unique_ptr<ScopedArenaAllocator> allocator_;
  std::unique_ptr<TypeInference> type_inference_;
  ArenaBitVector* live_in_v_;

  const TypeDef* type_defs_;
  size_t type_count_;
  const FieldDef* ifield_defs_;
  size_t ifield_count_;
  const FieldDef* sfield_defs_;
  size_t sfield_count_;
  const MethodDef* method_defs_;
  size_t method_count_;

  TestDexFileBuilder dex_file_builder_;
  std::unique_ptr<const DexFile> dex_file_;
};

TEST_F(TypeInferenceTest, IGet) {
  static const FieldDef ifields[] = {
      { kClassName, "B", "byteField" },
      { kClassName, "C", "charField" },
      { kClassName, "D", "doubleField" },
      { kClassName, "F", "floatField" },
      { kClassName, "I", "intField" },
      { kClassName, "J", "longField" },
      { kClassName, "S", "shortField" },
      { kClassName, "Z", "booleanField" },
      { kClassName, "Ljava/lang/Object;", "objectField" },
      { kClassName, "[Ljava/lang/Object;", "objectArrayField" },
  };
  constexpr uint32_t thiz = kLocalVRs;
  static const MIRDef mirs[] = {
      DEF_IGET(3u, Instruction::IGET_BYTE, 0u, thiz, 0u),
      DEF_IGET(3u, Instruction::IGET_CHAR, 1u, thiz, 1u),
      DEF_IGET_WIDE(3u, Instruction::IGET_WIDE, 2u, thiz, 2u),
      DEF_IGET(3u, Instruction::IGET, 4u, thiz, 3u),
      DEF_IGET(3u, Instruction::IGET, 5u, thiz, 4u),
      DEF_IGET_WIDE(3u, Instruction::IGET_WIDE, 6u, thiz, 5u),
      DEF_IGET(3u, Instruction::IGET_SHORT, 8u, thiz, 6u),
      DEF_IGET(3u, Instruction::IGET_BOOLEAN, 9u, thiz, 7u),
      DEF_IGET(3u, Instruction::IGET_OBJECT, 10u, thiz, 8u),
      DEF_IGET(3u, Instruction::IGET_OBJECT, 11u, thiz, 9u),
  };

  PrepareIFields(ifields);
  BuildDexFile("()V", false);
  PrepareSingleBlock();
  PrepareMIRs(mirs);
  PerformTypeInference();

  ASSERT_EQ(arraysize(mirs), mir_count_);
  static const SRegExpectation expectations[] = {
      { 0u, kExpectCore | kExpectNarrow },
      { 0u, kExpectCore | kExpectNarrow },
      { 0u, kExpectFp | kExpectWide },
      { 0u, kExpectFp | kExpectNarrow },
      { 0u, kExpectCore | kExpectNarrow },
      { 0u, kExpectCore | kExpectWide },
      { 0u, kExpectCore | kExpectNarrow },
      { 0u, kExpectCore | kExpectNarrow },
      { 0u, kExpectRef | kExpectNarrow },
      { 1u, kExpectRef | kExpectNarrow | kExpectArrayRef | kExpectArrayNarrow },
  };
  static_assert(arraysize(expectations) == arraysize(mirs), "array size mismatch");
  for (size_t i = 0; i != arraysize(expectations); ++i) {
    EXPECT_EQ(mirs[i].opcode, mirs_[i].dalvikInsn.opcode);
    ASSERT_LE(1u, mirs_[i].ssa_rep->num_defs);
    ExpectSRegType(mirs_[i].ssa_rep->defs[0], expectations[i]);
  }
  EXPECT_EQ(cu_.disable_opt & (1u << kPromoteRegs), 0u);
  EXPECT_FALSE(cu_.mir_graph->PuntToInterpreter());
}

TEST_F(TypeInferenceTest, SGet) {
  static const FieldDef sfields[] = {
      { kClassName, "B", "staticByteField" },
      { kClassName, "C", "staticCharField" },
      { kClassName, "D", "staticDoubleField" },
      { kClassName, "F", "staticFloatField" },
      { kClassName, "I", "staticIntField" },
      { kClassName, "J", "staticLongField" },
      { kClassName, "S", "staticShortField" },
      { kClassName, "Z", "staticBooleanField" },
      { kClassName, "Ljava/lang/Object;", "staticObjectField" },
      { kClassName, "[Ljava/lang/Object;", "staticObjectArrayField" },
  };
  static const MIRDef mirs[] = {
      DEF_SGET(3u, Instruction::SGET_BYTE, 0u, 0u),
      DEF_SGET(3u, Instruction::SGET_CHAR, 1u, 1u),
      DEF_SGET_WIDE(3u, Instruction::SGET_WIDE, 2u, 2u),
      DEF_SGET(3u, Instruction::SGET, 4u, 3u),
      DEF_SGET(3u, Instruction::SGET, 5u, 4u),
      DEF_SGET_WIDE(3u, Instruction::SGET_WIDE, 6u, 5u),
      DEF_SGET(3u, Instruction::SGET_SHORT, 8u, 6u),
      DEF_SGET(3u, Instruction::SGET_BOOLEAN, 9u, 7u),
      DEF_SGET(3u, Instruction::SGET_OBJECT, 10u, 8u),
      DEF_SGET(3u, Instruction::SGET_OBJECT, 11u, 9u),
  };

  PrepareSFields(sfields);
  BuildDexFile("()V", true);
  PrepareSingleBlock();
  PrepareMIRs(mirs);
  PerformTypeInference();

  ASSERT_EQ(arraysize(mirs), mir_count_);
  static const SRegExpectation expectations[] = {
      { 0u, kExpectCore | kExpectNarrow },
      { 0u, kExpectCore | kExpectNarrow },
      { 0u, kExpectFp | kExpectWide },
      { 0u, kExpectFp | kExpectNarrow },
      { 0u, kExpectCore | kExpectNarrow },
      { 0u, kExpectCore | kExpectWide },
      { 0u, kExpectCore | kExpectNarrow },
      { 0u, kExpectCore | kExpectNarrow },
      { 0u, kExpectRef | kExpectNarrow },
      { 1u, kExpectRef | kExpectNarrow | kExpectArrayRef | kExpectArrayNarrow },
  };
  static_assert(arraysize(expectations) == arraysize(mirs), "array size mismatch");
  for (size_t i = 0; i != arraysize(expectations); ++i) {
    EXPECT_EQ(mirs[i].opcode, mirs_[i].dalvikInsn.opcode);
    ASSERT_LE(1u, mirs_[i].ssa_rep->num_defs);
    ExpectSRegType(mirs_[i].ssa_rep->defs[0], expectations[i]);
  }
  EXPECT_EQ(cu_.disable_opt & (1u << kPromoteRegs), 0u);
  EXPECT_FALSE(cu_.mir_graph->PuntToInterpreter());
}

TEST_F(TypeInferenceTest, IPut) {
  static const FieldDef ifields[] = {
      { kClassName, "B", "byteField" },
      { kClassName, "C", "charField" },
      { kClassName, "D", "doubleField" },
      { kClassName, "F", "floatField" },
      { kClassName, "I", "intField" },
      { kClassName, "J", "longField" },
      { kClassName, "S", "shortField" },
      { kClassName, "Z", "booleanField" },
      { kClassName, "Ljava/lang/Object;", "objectField" },
      { kClassName, "[Ljava/lang/Object;", "objectArrayField" },
  };
  constexpr uint32_t thiz = kLocalVRs;
  static const MIRDef mirs[] = {
      DEF_CONST(3u, Instruction::CONST, 0u, 0),
      DEF_IPUT(3u, Instruction::IPUT_BYTE, 0u, thiz, 0u),
      DEF_CONST(3u, Instruction::CONST, 1u, 0),
      DEF_IPUT(3u, Instruction::IPUT_CHAR, 1u, thiz, 1u),
      DEF_CONST_WIDE(3u, Instruction::CONST_WIDE, 2u, 0),
      DEF_IPUT_WIDE(3u, Instruction::IPUT_WIDE, 2u, thiz, 2u),
      DEF_CONST(3u, Instruction::CONST, 4u, 0),
      DEF_IPUT(3u, Instruction::IPUT, 4u, thiz, 3u),
      DEF_CONST(3u, Instruction::CONST, 5u, 0),
      DEF_IPUT(3u, Instruction::IPUT, 5u, thiz, 4u),
      DEF_CONST_WIDE(3u, Instruction::CONST_WIDE, 6u, 0),
      DEF_IPUT_WIDE(3u, Instruction::IPUT_WIDE, 6u, thiz, 5u),
      DEF_CONST(3u, Instruction::CONST, 8u, 0),
      DEF_IPUT(3u, Instruction::IPUT_SHORT, 8u, thiz, 6u),
      DEF_CONST(3u, Instruction::CONST, 9u, 0),
      DEF_IPUT(3u, Instruction::IPUT_BOOLEAN, 9u, thiz, 7u),
      DEF_CONST(3u, Instruction::CONST, 10u, 0),
      DEF_IPUT(3u, Instruction::IPUT_OBJECT, 10u, thiz, 8u),
      DEF_CONST(3u, Instruction::CONST, 11u, 0),
      DEF_IPUT(3u, Instruction::IPUT_OBJECT, 11u, thiz, 9u),
  };

  PrepareIFields(ifields);
  BuildDexFile("()V", false);
  PrepareSingleBlock();
  PrepareMIRs(mirs);
  PerformTypeInference();

  ASSERT_EQ(arraysize(mirs), mir_count_);
  static const SRegExpectation expectations[] = {
      // One expectation for every 2 MIRs.
      { 0u, kExpectCore | kExpectNarrow },
      { 0u, kExpectCore | kExpectNarrow },
      { 0u, kExpectFp | kExpectWide },
      { 0u, kExpectFp | kExpectNarrow },
      { 0u, kExpectCore | kExpectNarrow },
      { 0u, kExpectCore | kExpectWide },
      { 0u, kExpectCore | kExpectNarrow },
      { 0u, kExpectCore | kExpectNarrow },
      { 0u, kExpectRef | kExpectNarrow | kExpectNull },
      { 1u, kExpectRef | kExpectNarrow | kExpectNull | kExpectArrayRef | kExpectArrayNarrow },
  };
  static_assert(2 * arraysize(expectations) == arraysize(mirs), "array size mismatch");
  for (size_t i = 0; i != arraysize(expectations); ++i) {
    EXPECT_EQ(mirs[2 * i].opcode, mirs_[2 * i].dalvikInsn.opcode);
    EXPECT_EQ(mirs[2 * i + 1].opcode, mirs_[2 * i + 1].dalvikInsn.opcode);
    ASSERT_LE(1u, mirs_[2 * i].ssa_rep->num_defs);
    ExpectSRegType(mirs_[2 * i].ssa_rep->defs[0], expectations[i]);
  }
  EXPECT_EQ(cu_.disable_opt & (1u << kPromoteRegs), 0u);
  EXPECT_FALSE(cu_.mir_graph->PuntToInterpreter());
}

TEST_F(TypeInferenceTest, SPut) {
  static const FieldDef sfields[] = {
      { kClassName, "B", "staticByteField" },
      { kClassName, "C", "staticCharField" },
      { kClassName, "D", "staticDoubleField" },
      { kClassName, "F", "staticFloatField" },
      { kClassName, "I", "staticIntField" },
      { kClassName, "J", "staticLongField" },
      { kClassName, "S", "staticShortField" },
      { kClassName, "Z", "staticBooleanField" },
      { kClassName, "Ljava/lang/Object;", "staticObjectField" },
      { kClassName, "[Ljava/lang/Object;", "staticObjectArrayField" },
  };
  static const MIRDef mirs[] = {
      DEF_CONST(3u, Instruction::CONST, 0u, 0),
      DEF_SPUT(3u, Instruction::SPUT_BYTE, 0u, 0u),
      DEF_CONST(3u, Instruction::CONST, 1u, 0),
      DEF_SPUT(3u, Instruction::SPUT_CHAR, 1u, 1u),
      DEF_CONST_WIDE(3u, Instruction::CONST_WIDE, 2u, 0),
      DEF_SPUT_WIDE(3u, Instruction::SPUT_WIDE, 2u, 2u),
      DEF_CONST(3u, Instruction::CONST, 4u, 0),
      DEF_SPUT(3u, Instruction::SPUT, 4u, 3u),
      DEF_CONST(3u, Instruction::CONST, 5u, 0),
      DEF_SPUT(3u, Instruction::SPUT, 5u, 4u),
      DEF_CONST_WIDE(3u, Instruction::CONST_WIDE, 6u, 0),
      DEF_SPUT_WIDE(3u, Instruction::SPUT_WIDE, 6u, 5u),
      DEF_CONST(3u, Instruction::CONST, 8u, 0),
      DEF_SPUT(3u, Instruction::SPUT_SHORT, 8u, 6u),
      DEF_CONST(3u, Instruction::CONST, 9u, 0),
      DEF_SPUT(3u, Instruction::SPUT_BOOLEAN, 9u, 7u),
      DEF_CONST(3u, Instruction::CONST, 10u, 0),
      DEF_SPUT(3u, Instruction::SPUT_OBJECT, 10u, 8u),
      DEF_CONST(3u, Instruction::CONST, 11u, 0),
      DEF_SPUT(3u, Instruction::SPUT_OBJECT, 11u, 9u),
  };

  PrepareSFields(sfields);
  BuildDexFile("()V", true);
  PrepareSingleBlock();
  PrepareMIRs(mirs);
  PerformTypeInference();

  ASSERT_EQ(arraysize(mirs), mir_count_);
  static const SRegExpectation expectations[] = {
      // One expectation for every 2 MIRs.
      { 0u, kExpectCore | kExpectNarrow },
      { 0u, kExpectCore | kExpectNarrow },
      { 0u, kExpectFp | kExpectWide },
      { 0u, kExpectFp | kExpectNarrow },
      { 0u, kExpectCore | kExpectNarrow },
      { 0u, kExpectCore | kExpectWide },
      { 0u, kExpectCore | kExpectNarrow },
      { 0u, kExpectCore | kExpectNarrow },
      { 0u, kExpectRef | kExpectNarrow | kExpectNull },
      { 1u, kExpectRef | kExpectNarrow | kExpectNull | kExpectArrayRef | kExpectArrayNarrow },
  };
  static_assert(2 * arraysize(expectations) == arraysize(mirs), "array size mismatch");
  for (size_t i = 0; i != arraysize(expectations); ++i) {
    EXPECT_EQ(mirs[2 * i].opcode, mirs_[2 * i].dalvikInsn.opcode);
    EXPECT_EQ(mirs[2 * i + 1].opcode, mirs_[2 * i + 1].dalvikInsn.opcode);
    ASSERT_LE(1u, mirs_[2 * i].ssa_rep->num_defs);
    ExpectSRegType(mirs_[2 * i].ssa_rep->defs[0], expectations[i]);
  }
  EXPECT_EQ(cu_.disable_opt & (1u << kPromoteRegs), 0u);
  EXPECT_FALSE(cu_.mir_graph->PuntToInterpreter());
}

TEST_F(TypeInferenceTest, MethodReturnType) {
  static const MethodDef methods[] = {
      { kClassName, "()B", "byteFoo", kStatic },
      { kClassName, "()C", "charFoo", kStatic },
      { kClassName, "()D", "doubleFoo", kStatic },
      { kClassName, "()F", "floatFoo", kStatic },
      { kClassName, "()I", "intFoo", kStatic },
      { kClassName, "()J", "longFoo", kStatic },
      { kClassName, "()S", "shortFoo", kStatic },
      { kClassName, "()Z", "booleanFoo", kStatic },
      { kClassName, "()Ljava/lang/Object;", "objectFoo", kStatic },
      { kClassName, "()[Ljava/lang/Object;", "objectArrayFoo", kStatic },
  };
  static const MIRDef mirs[] = {
      DEF_INVOKE0(3u, Instruction::INVOKE_STATIC, 0u),
      DEF_NULOP(3u, Instruction::MOVE_RESULT, 0u),
      DEF_INVOKE0(3u, Instruction::INVOKE_STATIC, 1u),
      DEF_NULOP(3u, Instruction::MOVE_RESULT, 1u),
      DEF_INVOKE0(3u, Instruction::INVOKE_STATIC, 2u),
      DEF_NULOP_WIDE(3u, Instruction::MOVE_RESULT_WIDE, 2u),
      DEF_INVOKE0(3u, Instruction::INVOKE_STATIC, 3u),
      DEF_NULOP(3u, Instruction::MOVE_RESULT, 4u),
      DEF_INVOKE0(3u, Instruction::INVOKE_STATIC, 4u),
      DEF_NULOP(3u, Instruction::MOVE_RESULT, 5u),
      DEF_INVOKE0(3u, Instruction::INVOKE_STATIC, 5u),
      DEF_NULOP_WIDE(3u, Instruction::MOVE_RESULT_WIDE, 6u),
      DEF_INVOKE0(3u, Instruction::INVOKE_STATIC, 6u),
      DEF_NULOP(3u, Instruction::MOVE_RESULT, 8u),
      DEF_INVOKE0(3u, Instruction::INVOKE_STATIC, 7u),
      DEF_NULOP(3u, Instruction::MOVE_RESULT, 9u),
      DEF_INVOKE0(3u, Instruction::INVOKE_STATIC, 8u),
      DEF_NULOP(3u, Instruction::MOVE_RESULT_OBJECT, 10u),
      DEF_INVOKE0(3u, Instruction::INVOKE_STATIC, 9u),
      DEF_NULOP(3u, Instruction::MOVE_RESULT_OBJECT, 11u),
  };

  PrepareMethods(methods);
  BuildDexFile("()V", true);
  PrepareSingleBlock();
  PrepareMIRs(mirs);
  PerformTypeInference();

  ASSERT_EQ(arraysize(mirs), mir_count_);
  static const SRegExpectation expectations[] = {
      // One expectation for every 2 MIRs.
      { 0u, kExpectCore | kExpectNarrow },
      { 0u, kExpectCore | kExpectNarrow },
      { 0u, kExpectFp | kExpectWide },
      { 0u, kExpectFp | kExpectNarrow },
      { 0u, kExpectCore | kExpectNarrow },
      { 0u, kExpectCore | kExpectWide },
      { 0u, kExpectCore | kExpectNarrow },
      { 0u, kExpectCore | kExpectNarrow },
      { 0u, kExpectRef | kExpectNarrow },
      { 1u, kExpectRef | kExpectNarrow | kExpectArrayRef | kExpectArrayNarrow },
  };
  static_assert(2 * arraysize(expectations) == arraysize(mirs), "array size mismatch");
  for (size_t i = 0; i != arraysize(expectations); ++i) {
    EXPECT_EQ(mirs[2 * i].opcode, mirs_[2 * i].dalvikInsn.opcode);
    EXPECT_EQ(mirs[2 * i + 1].opcode, mirs_[2 * i + 1].dalvikInsn.opcode);
    ASSERT_LE(1u, mirs_[2 * i + 1].ssa_rep->num_defs);
    ExpectSRegType(mirs_[2 * i + 1].ssa_rep->defs[0], expectations[i]);
  }
  EXPECT_EQ(cu_.disable_opt & (1u << kPromoteRegs), 0u);
  EXPECT_FALSE(cu_.mir_graph->PuntToInterpreter());
}

TEST_F(TypeInferenceTest, MethodArgType) {
  static const MethodDef methods[] = {
      { kClassName, "(B)V", "fooByte", kStatic },
      { kClassName, "(C)V", "fooChar", kStatic },
      { kClassName, "(D)V", "fooDouble", kStatic },
      { kClassName, "(F)V", "fooFloat", kStatic },
      { kClassName, "(I)V", "fooInt", kStatic },
      { kClassName, "(J)V", "fooLong", kStatic },
      { kClassName, "(S)V", "fooShort", kStatic },
      { kClassName, "(Z)V", "fooBoolean", kStatic },
      { kClassName, "(Ljava/lang/Object;)V", "fooObject", kStatic },
      { kClassName, "([Ljava/lang/Object;)V", "fooObjectArray", kStatic },
  };
  static const MIRDef mirs[] = {
      DEF_CONST(3u, Instruction::CONST, 0u, 0),
      DEF_INVOKE1(3u, Instruction::INVOKE_STATIC, 0u, 0u),
      DEF_CONST(3u, Instruction::CONST, 1u, 0),
      DEF_INVOKE1(3u, Instruction::INVOKE_STATIC, 1u, 1u),
      DEF_CONST_WIDE(3u, Instruction::CONST_WIDE, 2u, 0),
      DEF_INVOKE2(3u, Instruction::INVOKE_STATIC, 2u, 3u, 2u),
      DEF_CONST(3u, Instruction::CONST, 4u, 0),
      DEF_INVOKE1(3u, Instruction::INVOKE_STATIC, 4u, 3u),
      DEF_CONST(3u, Instruction::CONST, 5u, 0),
      DEF_INVOKE1(3u, Instruction::INVOKE_STATIC, 5u, 4u),
      DEF_CONST_WIDE(3u, Instruction::CONST_WIDE, 6u, 0),
      DEF_INVOKE2(3u, Instruction::INVOKE_STATIC, 6u, 7u, 5u),
      DEF_CONST(3u, Instruction::CONST, 8u, 0),
      DEF_INVOKE1(3u, Instruction::INVOKE_STATIC, 8u, 6u),
      DEF_CONST(3u, Instruction::CONST, 9u, 0),
      DEF_INVOKE1(3u, Instruction::INVOKE_STATIC, 9u, 7u),
      DEF_CONST(3u, Instruction::CONST, 10u, 0),
      DEF_INVOKE1(3u, Instruction::INVOKE_STATIC, 10u, 8u),
      DEF_CONST(3u, Instruction::CONST, 11u, 0),
      DEF_INVOKE1(3u, Instruction::INVOKE_STATIC, 11u, 9u),
  };

  PrepareMethods(methods);
  BuildDexFile("()V", true);
  PrepareSingleBlock();
  PrepareMIRs(mirs);
  PerformTypeInference();

  ASSERT_EQ(arraysize(mirs), mir_count_);
  static const SRegExpectation expectations[] = {
      // One expectation for every 2 MIRs.
      { 0u, kExpectCore | kExpectNarrow },
      { 0u, kExpectCore | kExpectNarrow },
      { 0u, kExpectFp | kExpectWide },
      { 0u, kExpectFp | kExpectNarrow },
      { 0u, kExpectCore | kExpectNarrow },
      { 0u, kExpectCore | kExpectWide },
      { 0u, kExpectCore | kExpectNarrow },
      { 0u, kExpectCore | kExpectNarrow },
      { 0u, kExpectRef | kExpectNarrow | kExpectNull },
      { 1u, kExpectRef | kExpectNarrow | kExpectNull | kExpectArrayRef | kExpectArrayNarrow },
  };
  static_assert(2 * arraysize(expectations) == arraysize(mirs), "array size mismatch");
  for (size_t i = 0; i != arraysize(expectations); ++i) {
    EXPECT_EQ(mirs[2 * i].opcode, mirs_[2 * i].dalvikInsn.opcode);
    EXPECT_EQ(mirs[2 * i + 1].opcode, mirs_[2 * i + 1].dalvikInsn.opcode);
    ASSERT_LE(1u, mirs_[2 * i].ssa_rep->num_defs);
    ExpectSRegType(mirs_[2 * i].ssa_rep->defs[0], expectations[i]);
  }
  EXPECT_EQ(cu_.disable_opt & (1u << kPromoteRegs), 0u);
  EXPECT_FALSE(cu_.mir_graph->PuntToInterpreter());
}

TEST_F(TypeInferenceTest, APut1) {
  static const MIRDef mirs[] = {
      DEF_CONST(3u, Instruction::CONST, 0u, 0),  // Object[] array
      DEF_CONST(3u, Instruction::CONST, 1u, 0),  // value; can't even determine whether core or fp.
      DEF_CONST(3u, Instruction::CONST, 2u, 0),  // index
      DEF_APUT(3u, Instruction::APUT, 1u, 0u, 2u),
  };

  BuildDexFile("()V", true);
  PrepareSingleBlock();
  PrepareMIRs(mirs);
  PerformTypeInference();

  ASSERT_EQ(arraysize(mirs), mir_count_);
  static const SRegExpectation expectations[] = {
      { 1u, kExpectRef | kExpectNarrow | kExpectNull | kExpectArrayNarrow },
      { 0u, kExpectNarrow },
      { 0u, kExpectCore | kExpectNarrow },
  };
  for (int32_t sreg = 0; sreg != arraysize(expectations); ++sreg) {
    ExpectSRegType(sreg, expectations[sreg]);
  }
  EXPECT_EQ(cu_.disable_opt & (1u << kPromoteRegs), 0u);
  EXPECT_FALSE(cu_.mir_graph->PuntToInterpreter());
}

TEST_F(TypeInferenceTest, APut2) {
  static const MIRDef mirs[] = {
      DEF_CONST(3u, Instruction::CONST, 0u, 0),  // Object[] array
      DEF_CONST(3u, Instruction::CONST, 1u, 0),  // Object[] value
      DEF_CONST(3u, Instruction::CONST, 2u, 0),  // index
      DEF_APUT(3u, Instruction::APUT_OBJECT, 1u, 0u, 2u),
  };

  BuildDexFile("()V", true);
  PrepareSingleBlock();
  PrepareMIRs(mirs);
  PerformTypeInference();

  ASSERT_EQ(arraysize(mirs), mir_count_);
  static const SRegExpectation expectations[] = {
      { 1u, kExpectRef | kExpectNarrow | kExpectNull | kExpectArrayRef | kExpectArrayNarrow },
      { 0u, kExpectRef | kExpectNarrow | kExpectNull },
      { 0u, kExpectCore | kExpectNarrow },
  };
  for (int32_t sreg = 0; sreg != arraysize(expectations); ++sreg) {
    ExpectSRegType(sreg, expectations[sreg]);
  }
  EXPECT_EQ(cu_.disable_opt & (1u << kPromoteRegs), 0u);
  EXPECT_FALSE(cu_.mir_graph->PuntToInterpreter());
}

TEST_F(TypeInferenceTest, APut3) {
  static const MIRDef mirs[] = {
      // Either array1 or array2 could be Object[][] but there is no way to tell from the bytecode.
      DEF_CONST(3u, Instruction::CONST, 0u, 0),  // Object[] array1
      DEF_CONST(3u, Instruction::CONST, 1u, 0),  // Object[] array2
      DEF_CONST(3u, Instruction::CONST, 2u, 0),  // index
      DEF_APUT(3u, Instruction::APUT_OBJECT, 0u, 1u, 2u),
      DEF_APUT(3u, Instruction::APUT_OBJECT, 1u, 0u, 2u),
  };

  BuildDexFile("()V", true);
  PrepareSingleBlock();
  PrepareMIRs(mirs);
  PerformTypeInference();

  ASSERT_EQ(arraysize(mirs), mir_count_);
  static const SRegExpectation expectations[] = {
      { 1u, kExpectRef | kExpectNarrow | kExpectNull | kExpectArrayRef | kExpectArrayNarrow },
      { 1u, kExpectRef | kExpectNarrow | kExpectNull | kExpectArrayRef | kExpectArrayNarrow },
      { 0u, kExpectCore | kExpectNarrow },
  };
  for (int32_t sreg = 0; sreg != arraysize(expectations); ++sreg) {
    ExpectSRegType(sreg, expectations[sreg]);
  }
  EXPECT_EQ(cu_.disable_opt & (1u << kPromoteRegs), 0u);
  EXPECT_FALSE(cu_.mir_graph->PuntToInterpreter());
}

TEST_F(TypeInferenceTest, APut4) {
  static const MIRDef mirs[] = {
      DEF_CONST(3u, Instruction::CONST, 0u, 0),
      DEF_CONST(3u, Instruction::CONST, 1u, 0),  // index
      DEF_AGET(3u, Instruction::AGET_OBJECT, 2u, 0u, 1u),  // Object[] array
      DEF_CONST(3u, Instruction::CONST, 3u, 0),  // value; can't even determine whether core or fp.
      DEF_APUT(3u, Instruction::APUT, 3u, 2u, 1u),
  };

  BuildDexFile("()V", true);
  PrepareSingleBlock();
  PrepareMIRs(mirs);
  PerformTypeInference();

  ASSERT_EQ(arraysize(mirs), mir_count_);
  static const SRegExpectation expectations[] = {
      { 1u, kExpectRef | kExpectNarrow | kExpectNull | kExpectArrayRef | kExpectArrayNarrow },
      { 0u, kExpectCore | kExpectNarrow },
      { 1u, kExpectRef | kExpectNarrow | kExpectArrayNarrow },
      { 0u, kExpectNarrow },
  };
  for (int32_t sreg = 0; sreg != arraysize(expectations); ++sreg) {
    ExpectSRegType(sreg, expectations[sreg]);
  }
  EXPECT_EQ(cu_.disable_opt & (1u << kPromoteRegs), 0u);
  EXPECT_FALSE(cu_.mir_graph->PuntToInterpreter());
}

TEST_F(TypeInferenceTest, APut5) {
  static const MIRDef mirs[] = {
      DEF_CONST(3u, Instruction::CONST, 0u, 0),
      DEF_CONST(3u, Instruction::CONST, 1u, 0),  // index
      DEF_AGET(3u, Instruction::AGET_OBJECT, 2u, 0u, 1u),  // Object[] array
      DEF_CONST(3u, Instruction::CONST, 3u, 0),  // Object[] value
      DEF_APUT(3u, Instruction::APUT_OBJECT, 3u, 2u, 1u),
  };

  BuildDexFile("()V", true);
  PrepareSingleBlock();
  PrepareMIRs(mirs);
  PerformTypeInference();

  ASSERT_EQ(arraysize(mirs), mir_count_);
  static const SRegExpectation expectations[] = {
      { 1u, kExpectRef | kExpectNarrow | kExpectNull | kExpectArrayRef | kExpectArrayNarrow },
      { 0u, kExpectCore | kExpectNarrow },
      { 1u, kExpectRef | kExpectNarrow | kExpectArrayRef | kExpectArrayNarrow },
      { 0u, kExpectRef | kExpectNarrow | kExpectNull },
  };
  for (int32_t sreg = 0; sreg != arraysize(expectations); ++sreg) {
    ExpectSRegType(sreg, expectations[sreg]);
  }
  EXPECT_EQ(cu_.disable_opt & (1u << kPromoteRegs), 0u);
  EXPECT_FALSE(cu_.mir_graph->PuntToInterpreter());
}

TEST_F(TypeInferenceTest, APut6) {
  static const MIRDef mirs[] = {
      DEF_CONST(3u, Instruction::CONST, 0u, 0),
      DEF_CONST(3u, Instruction::CONST, 1u, 0),  // index
      // Either array1 or array2 could be Object[][] but there is no way to tell from the bytecode.
      DEF_AGET(3u, Instruction::AGET_OBJECT, 2u, 0u, 1u),  // Object[] array1
      DEF_AGET(3u, Instruction::AGET_OBJECT, 3u, 0u, 1u),  // Object[] array2
      DEF_APUT(3u, Instruction::APUT_OBJECT, 2u, 3u, 1u),
      DEF_APUT(3u, Instruction::APUT_OBJECT, 3u, 2u, 1u),
  };

  BuildDexFile("()V", true);
  PrepareSingleBlock();
  PrepareMIRs(mirs);
  PerformTypeInference();

  ASSERT_EQ(arraysize(mirs), mir_count_);
  static const SRegExpectation expectations[] = {
      { 1u, kExpectRef | kExpectNarrow | kExpectNull | kExpectArrayRef | kExpectArrayNarrow },
      { 0u, kExpectCore | kExpectNarrow },
      { 1u, kExpectRef | kExpectNarrow | kExpectArrayRef | kExpectArrayNarrow },
      { 1u, kExpectRef | kExpectNarrow | kExpectArrayRef | kExpectArrayNarrow },
  };
  for (int32_t sreg = 0; sreg != arraysize(expectations); ++sreg) {
    ExpectSRegType(sreg, expectations[sreg]);
  }
  EXPECT_EQ(cu_.disable_opt & (1u << kPromoteRegs), 0u);
  EXPECT_FALSE(cu_.mir_graph->PuntToInterpreter());
}

TEST_F(TypeInferenceTest, TwoNullObjectArraysInLoop) {
  static const MIRDef mirs[] = {
      // void foo() {
      //   Object[] array1 = ((Object[])null)[0];
      //   Object[] array2 = ((Object[])null)[0];
      //   for (int i = 0; i != 3; ++i) {
      //     Object[] a1 = null;  // One of these could be Object[][] but not both.
      //     Object[] a2 = null;  // But they will be deduced as Object[].
      //     try { a1[0] = a2; } catch (Throwable ignored) { }
      //     try { a2[0] = a1; } catch (Throwable ignored) { }
      //     array1 = a1;
      //     array2 = a2;
      //   }
      // }
      //
      // Omitting the try-catch:
      DEF_CONST(3u, Instruction::CONST, 0u, 0),            // null
      DEF_CONST(3u, Instruction::CONST, 1u, 0),            // index
      DEF_AGET(3u, Instruction::AGET_OBJECT, 2u, 0u, 1u),  // array1
      DEF_AGET(3u, Instruction::AGET_OBJECT, 3u, 0u, 1u),  // array2
      DEF_PHI2(4u, 4u, 2u, 8u),  // ? + [L -> [? gives [L (see array-length below)
      DEF_PHI2(4u, 5u, 3u, 9u),  // ? + [L -> ? gives ?
      DEF_AGET(4u, Instruction::AGET_OBJECT, 6u, 0u, 1u),  // a1
      DEF_AGET(4u, Instruction::AGET_OBJECT, 7u, 0u, 1u),  // a2
      DEF_APUT(4u, Instruction::APUT_OBJECT, 6u, 7u, 1u),
      DEF_APUT(4u, Instruction::APUT_OBJECT, 7u, 6u, 1u),
      DEF_MOVE(4u, Instruction::MOVE_OBJECT, 8u, 6u),
      DEF_MOVE(4u, Instruction::MOVE_OBJECT, 9u, 7u),
      DEF_UNOP(5u, Instruction::ARRAY_LENGTH, 10u, 4u),
  };

  BuildDexFile("()V", true);
  PrepareLoop();
  PrepareMIRs(mirs);
  PerformTypeInference();

  ASSERT_EQ(arraysize(mirs), mir_count_);
  static const SRegExpectation expectations[] = {
      { 1u, kExpectRef | kExpectNarrow | kExpectNull | kExpectArrayRef | kExpectArrayNarrow },
      { 0u, kExpectCore | kExpectNarrow },
      { 1u, kExpectRef | kExpectNarrow | kExpectArrayRef | kExpectArrayNarrow },
      { 0u, kExpectRef | kExpectNarrow },
      { 1u, kExpectRef | kExpectNarrow | kExpectArrayRef | kExpectArrayNarrow },
      { 0u, kExpectRef | kExpectNarrow },
      { 1u, kExpectRef | kExpectNarrow | kExpectArrayRef | kExpectArrayNarrow },
      { 1u, kExpectRef | kExpectNarrow | kExpectArrayRef | kExpectArrayNarrow },
      { 1u, kExpectRef | kExpectNarrow | kExpectArrayRef | kExpectArrayNarrow },
      { 1u, kExpectRef | kExpectNarrow | kExpectArrayRef | kExpectArrayNarrow },
      { 0u, kExpectCore | kExpectNarrow },
  };
  for (int32_t sreg = 0; sreg != arraysize(expectations); ++sreg) {
    ExpectSRegType(sreg, expectations[sreg]);
  }
  EXPECT_EQ(cu_.disable_opt & (1u << kPromoteRegs), 0u);
  EXPECT_FALSE(cu_.mir_graph->PuntToInterpreter());
}

TEST_F(TypeInferenceTest, ArrayArrayFloat) {
  static const MethodDef methods[] = {
      { kClassName, "(F)V", "fooFloat", kStatic },
  };
  static const MIRDef mirs[] = {
      // void foo() {
      //   try {
      //     float[][][] aaaf = null;
      //     float[][] array = aaaf[0];  // Make sure array is treated as properly typed.
      //     array[0][0] = 0.0f;      // const + aget-object[1] + aput
      //     fooFloat(array[0][0]);   // aget-object[2] + aget + invoke
      //     // invoke: signature => input is F.
      //     // aget: output is F => base is [F (precise)
      //     // aget-object[2]: output is [F => base is [[F (precise)
      //     // aput: unknown input type => base is [?
      //     // aget-object[1]: base is [[F => result is L or [F, merge with [? => result is [F
      //     // aput (again): base is [F => result is F
      //     // const: F determined by the aput reprocessing.
      //   } catch (Throwable ignored) {
      //   }
      // }
      //
      // Omitting the try-catch:
      DEF_CONST(3u, Instruction::CONST, 0u, 0),             // 0
      DEF_CONST(3u, Instruction::CONST, 1u, 0),             // aaaf
      DEF_AGET(3u, Instruction::AGET_OBJECT, 2u, 1u, 0u),   // array = aaaf[0]
      DEF_CONST(3u, Instruction::CONST, 3u, 0),             // 0.0f
      DEF_AGET(3u, Instruction::AGET_OBJECT, 4u, 2u, 0u),   // array[0]
      DEF_APUT(3u, Instruction::APUT, 3u, 4u, 0u),          // array[0][0] = 0.0f
      DEF_AGET(3u, Instruction::AGET_OBJECT, 5u, 2u, 0u),   // array[0]
      DEF_AGET(3u, Instruction::AGET, 6u, 5u, 0u),          // array[0][0]
      DEF_INVOKE1(3u, Instruction::INVOKE_STATIC, 6u, 0u),  // fooFloat(array[0][0])
  };

  PrepareMethods(methods);
  BuildDexFile("()V", true);
  PrepareSingleBlock();
  PrepareMIRs(mirs);
  PerformTypeInference();

  ASSERT_EQ(arraysize(mirs), mir_count_);
  static const SRegExpectation expectations[] = {
      { 0u, kExpectCore | kExpectNarrow },
      { 1u, kExpectRef | kExpectNarrow | kExpectNull | kExpectArrayRef | kExpectArrayNarrow },
      { 2u, kExpectRef | kExpectNarrow | kExpectArrayFp | kExpectArrayNarrow },
      { 0u, kExpectFp | kExpectNarrow },
      { 1u, kExpectRef | kExpectNarrow | kExpectArrayFp | kExpectArrayNarrow },
      { 1u, kExpectRef | kExpectNarrow | kExpectArrayFp | kExpectArrayNarrow },
      { 0u, kExpectFp | kExpectNarrow },
  };
  for (int32_t sreg = 0; sreg != arraysize(expectations); ++sreg) {
    ExpectSRegType(sreg, expectations[sreg]);
  }
  EXPECT_EQ(cu_.disable_opt & (1u << kPromoteRegs), 0u);
  EXPECT_FALSE(cu_.mir_graph->PuntToInterpreter());
}

TEST_F(TypeInferenceTest, CheckCast1) {
  static const TypeDef types[] = {
      { "[I" },
  };
  static const MIRDef mirs[] = {
      DEF_CONST(3u, Instruction::CONST, 0u, 0),
      DEF_CONST(3u, Instruction::CONST, 1u, 0),
      DEF_AGET(3u, Instruction::AGET_OBJECT, 2u, 0u, 1u),
      DEF_CHECK_CAST(4u, Instruction::CHECK_CAST, 2u, 0u),
      DEF_CHECK_CAST(5u, Instruction::CHECK_CAST, 2u, 0u),
      // Pseudo-phi from [I and [I into L infers only L but not [.
      DEF_MOVE(6u, Instruction::MOVE_OBJECT, 3u, 2u),
  };
  PrepareTypes(types);
  BuildDexFile("()V", true);
  PrepareDiamond();
  PrepareMIRs(mirs);
  static const BasicBlockId v0_def_blocks[] = { 3u, 4u, 5u, 6u };
  MapVRegToSReg(2, 2, v0_def_blocks);
  PerformTypeInference();

  ASSERT_EQ(arraysize(mirs), mir_count_);
  static const SRegExpectation expectations[] = {
      { 1u, kExpectRef | kExpectNarrow | kExpectNull | kExpectArrayRef | kExpectArrayNarrow },
      { 0u, kExpectCore | kExpectNarrow },
      { 0u, kExpectRef | kExpectNarrow },
      { 0u, kExpectRef | kExpectNarrow },
  };
  for (int32_t sreg = 0; sreg != arraysize(expectations); ++sreg) {
    ExpectSRegType(sreg, expectations[sreg]);
  }
  EXPECT_EQ(cu_.disable_opt & (1u << kPromoteRegs), 0u);
  EXPECT_FALSE(cu_.mir_graph->PuntToInterpreter());
}

TEST_F(TypeInferenceTest, CheckCast2) {
  static const TypeDef types[] = {
      { "[I" },
  };
  static const MIRDef mirs[] = {
      DEF_CONST(3u, Instruction::CONST, 0u, 0),
      DEF_CONST(3u, Instruction::CONST, 1u, 0),
      DEF_AGET(3u, Instruction::AGET_OBJECT, 2u, 0u, 1u),
      DEF_CHECK_CAST(4u, Instruction::CHECK_CAST, 2u, 0u),
      DEF_CHECK_CAST(5u, Instruction::CHECK_CAST, 2u, 0u),
      // Pseudo-phi from [I and [I into [? infers [I.
      DEF_MOVE(6u, Instruction::MOVE_OBJECT, 3u, 2u),
      DEF_UNOP(6u, Instruction::ARRAY_LENGTH, 4u, 2u),
  };
  PrepareTypes(types);
  BuildDexFile("()V", true);
  PrepareDiamond();
  PrepareMIRs(mirs);
  static const BasicBlockId v0_def_blocks[] = { 3u, 4u, 5u, 6u };
  MapVRegToSReg(2, 2, v0_def_blocks);
  PerformTypeInference();

  ASSERT_EQ(arraysize(mirs), mir_count_);
  static const SRegExpectation expectations[] = {
      { 1u, kExpectRef | kExpectNarrow | kExpectNull | kExpectArrayRef | kExpectArrayNarrow },
      { 0u, kExpectCore | kExpectNarrow },
      { 0u, kExpectRef | kExpectNarrow },
      { 1u, kExpectRef | kExpectNarrow | kExpectArrayCore | kExpectArrayNarrow },
      { 0u, kExpectCore | kExpectNarrow },
  };
  for (int32_t sreg = 0; sreg != arraysize(expectations); ++sreg) {
    ExpectSRegType(sreg, expectations[sreg]);
  }
  EXPECT_EQ(cu_.disable_opt & (1u << kPromoteRegs), 0u);
  EXPECT_FALSE(cu_.mir_graph->PuntToInterpreter());
}

TEST_F(TypeInferenceTest, CheckCast3) {
  static const TypeDef types[] = {
      { "[I" },
      { "[F" },
  };
  static const MIRDef mirs[] = {
      DEF_CONST(3u, Instruction::CONST, 0u, 0),
      DEF_CONST(3u, Instruction::CONST, 1u, 0),
      DEF_AGET(3u, Instruction::AGET_OBJECT, 2u, 0u, 1u),
      DEF_CHECK_CAST(4u, Instruction::CHECK_CAST, 2u, 0u),
      DEF_CHECK_CAST(5u, Instruction::CHECK_CAST, 2u, 1u),
      // Pseudo-phi from [I and [F into L correctly leaves it as L.
      DEF_MOVE(6u, Instruction::MOVE_OBJECT, 3u, 2u),
  };
  PrepareTypes(types);
  BuildDexFile("()V", true);
  PrepareDiamond();
  PrepareMIRs(mirs);
  static const BasicBlockId v0_def_blocks[] = { 3u, 4u, 5u, 6u };
  MapVRegToSReg(2, 2, v0_def_blocks);
  PerformTypeInference();

  ASSERT_EQ(arraysize(mirs), mir_count_);
  static const SRegExpectation expectations[] = {
      { 1u, kExpectRef | kExpectNarrow | kExpectNull | kExpectArrayRef | kExpectArrayNarrow },
      { 0u, kExpectCore | kExpectNarrow },
      { 0u, kExpectRef | kExpectNarrow },
      { 0u, kExpectRef | kExpectNarrow },
  };
  for (int32_t sreg = 0; sreg != arraysize(expectations); ++sreg) {
    ExpectSRegType(sreg, expectations[sreg]);
  }
  EXPECT_EQ(cu_.disable_opt & (1u << kPromoteRegs), 0u);
  EXPECT_FALSE(cu_.mir_graph->PuntToInterpreter());
}

TEST_F(TypeInferenceTest, CheckCastConflict1) {
  static const TypeDef types[] = {
      { "[I" },
      { "[F" },
  };
  static const MIRDef mirs[] = {
      DEF_CONST(3u, Instruction::CONST, 0u, 0),
      DEF_CONST(3u, Instruction::CONST, 1u, 0),
      DEF_AGET(3u, Instruction::AGET_OBJECT, 2u, 0u, 1u),
      DEF_CHECK_CAST(4u, Instruction::CHECK_CAST, 2u, 0u),
      DEF_CHECK_CAST(5u, Instruction::CHECK_CAST, 2u, 1u),
      // Pseudo-phi from [I and [F into [? infers conflict [I/[F.
      DEF_MOVE(6u, Instruction::MOVE_OBJECT, 3u, 2u),
      DEF_UNOP(6u, Instruction::ARRAY_LENGTH, 4u, 2u),
  };
  PrepareTypes(types);
  BuildDexFile("()V", true);
  PrepareDiamond();
  PrepareMIRs(mirs);
  static const BasicBlockId v0_def_blocks[] = { 3u, 4u, 5u, 6u };
  MapVRegToSReg(2, 2, v0_def_blocks);
  PerformTypeInference();

  ASSERT_EQ(arraysize(mirs), mir_count_);
  static const SRegExpectation expectations[] = {
      { 1u, kExpectRef | kExpectNarrow | kExpectNull | kExpectArrayRef | kExpectArrayNarrow },
      { 0u, kExpectCore | kExpectNarrow },
      { 0u, kExpectRef | kExpectNarrow },
      { 1u, kExpectRef | kExpectNarrow | kExpectArrayCore | kExpectArrayFp | kExpectArrayNarrow },
      { 0u, kExpectCore | kExpectNarrow },
  };
  for (int32_t sreg = 0; sreg != arraysize(expectations); ++sreg) {
    ExpectSRegType(sreg, expectations[sreg], false);
  }
  // The type conflict in array element wasn't propagated to an SSA reg.
  EXPECT_EQ(cu_.disable_opt & (1u << kPromoteRegs), 0u);
  EXPECT_FALSE(cu_.mir_graph->PuntToInterpreter());
}

TEST_F(TypeInferenceTest, CheckCastConflict2) {
  static const TypeDef types[] = {
      { "[I" },
      { "[F" },
  };
  static const MIRDef mirs[] = {
      DEF_CONST(3u, Instruction::CONST, 0u, 0),
      DEF_CONST(3u, Instruction::CONST, 1u, 0),
      DEF_AGET(3u, Instruction::AGET_OBJECT, 2u, 0u, 1u),
      DEF_CHECK_CAST(4u, Instruction::CHECK_CAST, 2u, 0u),
      DEF_CHECK_CAST(5u, Instruction::CHECK_CAST, 2u, 1u),
      // Pseudo-phi from [I and [F into [? infers conflict [I/[F.
      DEF_MOVE(6u, Instruction::MOVE_OBJECT, 3u, 2u),
      DEF_AGET(6u, Instruction::AGET, 4u, 2u, 1u),
  };
  PrepareTypes(types);
  BuildDexFile("()V", true);
  PrepareDiamond();
  PrepareMIRs(mirs);
  static const BasicBlockId v0_def_blocks[] = { 3u, 4u, 5u, 6u };
  MapVRegToSReg(2, 2, v0_def_blocks);
  PerformTypeInference();

  ASSERT_EQ(arraysize(mirs), mir_count_);
  static const SRegExpectation expectations[] = {
      { 1u, kExpectRef | kExpectNarrow | kExpectNull | kExpectArrayRef | kExpectArrayNarrow },
      { 0u, kExpectCore | kExpectNarrow },
      { 0u, kExpectRef | kExpectNarrow },
      { 1u, kExpectRef | kExpectNarrow | kExpectArrayCore | kExpectArrayFp | kExpectArrayNarrow },
      { 0u, kExpectCore | kExpectFp | kExpectNarrow },
  };
  for (int32_t sreg = 0; sreg != arraysize(expectations); ++sreg) {
    ExpectSRegType(sreg, expectations[sreg], false);
  }
  // Type conflict in an SSA reg, register promotion disabled.
  EXPECT_NE(cu_.disable_opt & (1u << kPromoteRegs), 0u);
  EXPECT_FALSE(cu_.mir_graph->PuntToInterpreter());
}

TEST_F(TypeInferenceTest, Phi1) {
  static const TypeDef types[] = {
      { "[I" },
  };
  static const MIRDef mirs[] = {
      DEF_CONST(3u, Instruction::CONST, 0u, 100),
      DEF_NEW_ARRAY(4u, Instruction::NEW_ARRAY, 1u, 0u, 0u),
      DEF_NEW_ARRAY(5u, Instruction::NEW_ARRAY, 2u, 0u, 0u),
      // Phi from [I and [I infers only L but not [.
      DEF_PHI2(6u, 3u, 1u, 2u),
  };
  PrepareTypes(types);
  BuildDexFile("()V", true);
  PrepareDiamond();
  PrepareMIRs(mirs);
  PerformTypeInference();

  ASSERT_EQ(arraysize(mirs), mir_count_);
  static const SRegExpectation expectations[] = {
      { 0u, kExpectCore | kExpectNarrow },
      { 1u, kExpectRef | kExpectNarrow | kExpectArrayCore | kExpectArrayNarrow },
      { 1u, kExpectRef | kExpectNarrow | kExpectArrayCore | kExpectArrayNarrow },
      { 0u, kExpectRef | kExpectNarrow },
  };
  for (int32_t sreg = 0; sreg != arraysize(expectations); ++sreg) {
    ExpectSRegType(sreg, expectations[sreg]);
  }
  EXPECT_EQ(cu_.disable_opt & (1u << kPromoteRegs), 0u);
  EXPECT_FALSE(cu_.mir_graph->PuntToInterpreter());
}

TEST_F(TypeInferenceTest, Phi2) {
  static const TypeDef types[] = {
      { "[F" },
  };
  static const MIRDef mirs[] = {
      DEF_CONST(3u, Instruction::CONST, 0u, 100),
      DEF_NEW_ARRAY(4u, Instruction::NEW_ARRAY, 1u, 0u, 0u),
      DEF_NEW_ARRAY(5u, Instruction::NEW_ARRAY, 2u, 0u, 0u),
      // Phi from [F and [F into [? infers [F.
      DEF_PHI2(6u, 3u, 1u, 2u),
      DEF_UNOP(6u, Instruction::ARRAY_LENGTH, 4u, 3u),
  };
  PrepareTypes(types);
  BuildDexFile("()V", true);
  PrepareDiamond();
  PrepareMIRs(mirs);
  PerformTypeInference();

  ASSERT_EQ(arraysize(mirs), mir_count_);
  static const SRegExpectation expectations[] = {
      { 0u, kExpectCore | kExpectNarrow },
      { 1u, kExpectRef | kExpectNarrow | kExpectArrayFp | kExpectArrayNarrow },
      { 1u, kExpectRef | kExpectNarrow | kExpectArrayFp | kExpectArrayNarrow },
      { 1u, kExpectRef | kExpectNarrow | kExpectArrayFp | kExpectArrayNarrow },
      { 0u, kExpectCore | kExpectNarrow },
  };
  for (int32_t sreg = 0; sreg != arraysize(expectations); ++sreg) {
    ExpectSRegType(sreg, expectations[sreg]);
  }
  EXPECT_EQ(cu_.disable_opt & (1u << kPromoteRegs), 0u);
  EXPECT_FALSE(cu_.mir_graph->PuntToInterpreter());
}

TEST_F(TypeInferenceTest, Phi3) {
  static const TypeDef types[] = {
      { "[I" },
      { "[F" },
  };
  static const MIRDef mirs[] = {
      DEF_CONST(3u, Instruction::CONST, 0u, 100),
      DEF_NEW_ARRAY(4u, Instruction::NEW_ARRAY, 1u, 0u, 0u),
      DEF_NEW_ARRAY(5u, Instruction::NEW_ARRAY, 2u, 0u, 1u),
      // Phi from [I and [F infers L.
      DEF_PHI2(6u, 3u, 1u, 2u),
  };
  PrepareTypes(types);
  BuildDexFile("()V", true);
  PrepareDiamond();
  PrepareMIRs(mirs);
  PerformTypeInference();

  ASSERT_EQ(arraysize(mirs), mir_count_);
  static const SRegExpectation expectations[] = {
      { 0u, kExpectCore | kExpectNarrow },
      { 1u, kExpectRef | kExpectNarrow | kExpectArrayCore | kExpectArrayNarrow },
      { 1u, kExpectRef | kExpectNarrow | kExpectArrayFp | kExpectArrayNarrow },
      { 0u, kExpectRef | kExpectNarrow },
  };
  for (int32_t sreg = 0; sreg != arraysize(expectations); ++sreg) {
    ExpectSRegType(sreg, expectations[sreg]);
  }
  EXPECT_EQ(cu_.disable_opt & (1u << kPromoteRegs), 0u);
  EXPECT_FALSE(cu_.mir_graph->PuntToInterpreter());
}

TEST_F(TypeInferenceTest, Phi4) {
  static const TypeDef types[] = {
      { "[I" },
  };
  static const MIRDef mirs[] = {
      DEF_CONST(3u, Instruction::CONST, 0u, 100),
      DEF_NEW_ARRAY(4u, Instruction::NEW_ARRAY, 1u, 0u, 0u),
      DEF_CONST(5u, Instruction::CONST, 2u, 0),
      // Pseudo-phi from [I and null infers L.
      DEF_PHI2(6u, 3u, 1u, 2u),
  };
  PrepareTypes(types);
  BuildDexFile("()V", true);
  PrepareDiamond();
  PrepareMIRs(mirs);
  PerformTypeInference();

  ASSERT_EQ(arraysize(mirs), mir_count_);
  static const SRegExpectation expectations[] = {
      { 0u, kExpectCore | kExpectNarrow },
      { 1u, kExpectRef | kExpectNarrow | kExpectArrayCore | kExpectArrayNarrow },
      { 0u, kExpectRef | kExpectNarrow | kExpectNull },
      { 0u, kExpectRef | kExpectNarrow },
  };
  for (int32_t sreg = 0; sreg != arraysize(expectations); ++sreg) {
    ExpectSRegType(sreg, expectations[sreg]);
  }
  EXPECT_EQ(cu_.disable_opt & (1u << kPromoteRegs), 0u);
  EXPECT_FALSE(cu_.mir_graph->PuntToInterpreter());
}

TEST_F(TypeInferenceTest, PhiConflict1) {
  static const TypeDef types[] = {
      { "[I" },
      { "[F" },
  };
  static const MIRDef mirs[] = {
      DEF_CONST(3u, Instruction::CONST, 0u, 100),
      DEF_NEW_ARRAY(4u, Instruction::NEW_ARRAY, 1u, 0u, 0u),
      DEF_NEW_ARRAY(5u, Instruction::NEW_ARRAY, 2u, 0u, 1u),
      // Pseudo-phi from [I and [F into [? infers conflict [I/[F (then propagated upwards).
      DEF_PHI2(6u, 3u, 1u, 2u),
      DEF_UNOP(6u, Instruction::ARRAY_LENGTH, 4u, 3u),
  };
  PrepareTypes(types);
  BuildDexFile("()V", true);
  PrepareDiamond();
  PrepareMIRs(mirs);
  PerformTypeInference();

  ASSERT_EQ(arraysize(mirs), mir_count_);
  static const SRegExpectation expectations[] = {
      { 0u, kExpectCore | kExpectNarrow },
      { 1u, kExpectRef | kExpectNarrow | kExpectArrayCore | kExpectArrayFp | kExpectArrayNarrow },
      { 1u, kExpectRef | kExpectNarrow | kExpectArrayCore | kExpectArrayFp | kExpectArrayNarrow },
      { 1u, kExpectRef | kExpectNarrow | kExpectArrayCore | kExpectArrayFp | kExpectArrayNarrow },
      { 0u, kExpectCore | kExpectNarrow },
  };
  for (int32_t sreg = 0; sreg != arraysize(expectations); ++sreg) {
    ExpectSRegType(sreg, expectations[sreg], false);
  }
  // The type conflict in array element wasn't propagated to an SSA reg.
  EXPECT_EQ(cu_.disable_opt & (1u << kPromoteRegs), 0u);
  EXPECT_FALSE(cu_.mir_graph->PuntToInterpreter());
}

TEST_F(TypeInferenceTest, PhiConflict2) {
  static const TypeDef types[] = {
      { "[I" },
      { "[F" },
  };
  static const MIRDef mirs[] = {
      DEF_CONST(3u, Instruction::CONST, 0u, 100),
      DEF_NEW_ARRAY(4u, Instruction::NEW_ARRAY, 1u, 0u, 0u),
      DEF_NEW_ARRAY(5u, Instruction::NEW_ARRAY, 2u, 0u, 1u),
      // Pseudo-phi from [I and [F into [? infers conflict [I/[F (then propagated upwards).
      DEF_PHI2(6u, 3u, 1u, 2u),
      DEF_AGET(6u, Instruction::AGET, 4u, 3u, 0u),
  };
  PrepareTypes(types);
  BuildDexFile("()V", true);
  PrepareDiamond();
  PrepareMIRs(mirs);
  PerformTypeInference();

  ASSERT_EQ(arraysize(mirs), mir_count_);
  static const SRegExpectation expectations[] = {
      { 0u, kExpectCore | kExpectNarrow },
      { 1u, kExpectRef | kExpectNarrow | kExpectArrayCore | kExpectArrayFp | kExpectArrayNarrow },
      { 1u, kExpectRef | kExpectNarrow | kExpectArrayCore | kExpectArrayFp | kExpectArrayNarrow },
      { 1u, kExpectRef | kExpectNarrow | kExpectArrayCore | kExpectArrayFp | kExpectArrayNarrow },
      { 0u, kExpectCore | kExpectFp | kExpectNarrow },
  };
  for (int32_t sreg = 0; sreg != arraysize(expectations); ++sreg) {
    ExpectSRegType(sreg, expectations[sreg], false);
  }
  // Type conflict in an SSA reg, register promotion disabled.
  EXPECT_NE(cu_.disable_opt & (1u << kPromoteRegs), 0u);
  EXPECT_FALSE(cu_.mir_graph->PuntToInterpreter());
}

TEST_F(TypeInferenceTest, Wide1) {
  static const MIRDef mirs[] = {
      DEF_CONST(3u, Instruction::CONST, 0u, 0),
      DEF_CONST(3u, Instruction::CONST, 1u, 0),  // index
      DEF_AGET(3u, Instruction::AGET_OBJECT, 2u, 0u, 1u),  // long[]
      DEF_CONST_WIDE(3u, Instruction::CONST_WIDE, 3u, 0),  // long
      DEF_APUT_WIDE(3u, Instruction::APUT_WIDE, 3u, 2u, 1u),
      { 3u, Instruction::RETURN_OBJECT, 0, 0u, 1u, { 2u }, 0u, { } },
  };

  BuildDexFile("()[J", true);
  PrepareSingleBlock();
  PrepareMIRs(mirs);
  PerformTypeInference();

  ASSERT_EQ(arraysize(mirs), mir_count_);
  static const SRegExpectation expectations[] = {
      { 1u, kExpectRef | kExpectNarrow | kExpectNull | kExpectArrayRef | kExpectArrayNarrow },
      { 0u, kExpectCore | kExpectNarrow },
      { 1u, kExpectRef | kExpectNarrow | kExpectArrayCore | kExpectArrayWide },
      { 0u, kExpectCore | kExpectWide },
      // NOTE: High word checked implicitly for sreg = 3.
  };
  for (int32_t sreg = 0; sreg != arraysize(expectations); ++sreg) {
    ExpectSRegType(sreg, expectations[sreg], false);
  }
  EXPECT_EQ(cu_.disable_opt & (1u << kPromoteRegs), 0u);
  EXPECT_FALSE(cu_.mir_graph->PuntToInterpreter());
}

TEST_F(TypeInferenceTest, WideSizeConflict1) {
  static const MIRDef mirs[] = {
      DEF_CONST_WIDE(3u, Instruction::CONST_WIDE, 0u, 0),
      DEF_MOVE(3u, Instruction::MOVE, 2u, 0u),
  };

  BuildDexFile("()V", true);
  PrepareSingleBlock();
  PrepareMIRs(mirs);
  PerformTypeInference();

  ASSERT_EQ(arraysize(mirs), mir_count_);
  static const SRegExpectation expectations[] = {
      { 0u, kExpectNarrow | kExpectWide },
      { 0u, kExpectNarrow | kExpectWide },
  };
  ExpectSRegType(0u, expectations[0], false);
  ExpectSRegType(2u, expectations[1], false);
  EXPECT_TRUE(cu_.mir_graph->PuntToInterpreter());
}

TEST_F(TypeInferenceTest, ArrayLongLength) {
  static const FieldDef sfields[] = {
      { kClassName, "[J", "arrayLongField" },
  };
  static const MIRDef mirs[] = {
      DEF_CONST(4u, Instruction::CONST, 0u, 0),
      DEF_SGET(5u, Instruction::SGET_OBJECT, 1u, 0u),
      DEF_PHI2(6u, 2u, 0u, 1u),
      DEF_UNOP(6u, Instruction::ARRAY_LENGTH, 3u, 2u),
      DEF_SGET(6u, Instruction::SGET_OBJECT, 4u, 0u),
      DEF_UNOP(6u, Instruction::ARRAY_LENGTH, 5u, 4u),
  };

  PrepareSFields(sfields);
  BuildDexFile("()V", true);
  PrepareDiamond();
  PrepareMIRs(mirs);
  PerformTypeInference();

  ASSERT_EQ(arraysize(mirs), mir_count_);
  static const SRegExpectation expectations[] = {
      { 1u, kExpectRef | kExpectNarrow | kExpectNull | kExpectArrayCore | kExpectArrayWide },
      { 1u, kExpectRef | kExpectNarrow | kExpectArrayCore | kExpectArrayWide },
      { 1u, kExpectRef | kExpectNarrow | kExpectArrayCore | kExpectArrayWide },
      { 0u, kExpectCore | kExpectNarrow },
      { 1u, kExpectRef | kExpectNarrow | kExpectArrayCore | kExpectArrayWide },
      { 0u, kExpectCore | kExpectNarrow },
  };
  for (int32_t sreg = 0; sreg != arraysize(expectations); ++sreg) {
    ExpectSRegType(sreg, expectations[sreg]);
  }
  EXPECT_EQ(cu_.disable_opt & (1u << kPromoteRegs), 0u);
  EXPECT_FALSE(cu_.mir_graph->PuntToInterpreter());
}

TEST_F(TypeInferenceTest, ArrayArrayObjectLength) {
  static const FieldDef sfields[] = {
      { kClassName, "[[Ljava/lang/Object;", "arrayLongField" },
  };
  static const MIRDef mirs[] = {
      DEF_CONST(4u, Instruction::CONST, 0u, 0),
      DEF_SGET(5u, Instruction::SGET_OBJECT, 1u, 0u),
      DEF_PHI2(6u, 2u, 0u, 1u),
      DEF_UNOP(6u, Instruction::ARRAY_LENGTH, 3u, 2u),
      DEF_SGET(6u, Instruction::SGET_OBJECT, 4u, 0u),
      DEF_UNOP(6u, Instruction::ARRAY_LENGTH, 5u, 4u),
  };

  PrepareSFields(sfields);
  BuildDexFile("()V", true);
  PrepareDiamond();
  PrepareMIRs(mirs);
  PerformTypeInference();

  ASSERT_EQ(arraysize(mirs), mir_count_);
  static const SRegExpectation expectations[] = {
      { 1u, kExpectRef | kExpectNarrow | kExpectNull | kExpectArrayRef | kExpectArrayNarrow },
      { 2u, kExpectRef | kExpectNarrow | kExpectArrayRef | kExpectArrayNarrow },
      { 1u, kExpectRef | kExpectNarrow | kExpectArrayRef | kExpectArrayNarrow },
      { 0u, kExpectCore | kExpectNarrow },
      { 2u, kExpectRef | kExpectNarrow | kExpectArrayRef | kExpectArrayNarrow },
      { 0u, kExpectCore | kExpectNarrow },
  };
  for (int32_t sreg = 0; sreg != arraysize(expectations); ++sreg) {
    ExpectSRegType(sreg, expectations[sreg]);
  }
  EXPECT_EQ(cu_.disable_opt & (1u << kPromoteRegs), 0u);
  EXPECT_FALSE(cu_.mir_graph->PuntToInterpreter());
}

TEST_F(TypeInferenceTest, SGetAdd0SPut) {
  static const FieldDef sfields[] = {
      { kClassName, "I", "staticIntField" },
  };
  static const MIRDef mirs[] = {
      DEF_SGET(3u, Instruction::SGET, 0u, 0u),
      DEF_UNOP(3u, Instruction::ADD_INT_LIT8, 1u, 0u),  // +0
      DEF_SPUT(3u, Instruction::SPUT, 1u, 0u),
  };

  PrepareSFields(sfields);
  BuildDexFile("()V", true);
  PrepareSingleBlock();
  PrepareMIRs(mirs);
  PerformTypeInference();

  ASSERT_EQ(arraysize(mirs), mir_count_);
  static const SRegExpectation expectations[] = {
      { 0u, kExpectCore | kExpectNarrow },
      { 0u, kExpectCore | kExpectNarrow },
  };
  for (int32_t sreg = 0; sreg != arraysize(expectations); ++sreg) {
    ExpectSRegType(sreg, expectations[sreg]);
  }
  EXPECT_EQ(cu_.disable_opt & (1u << kPromoteRegs), 0u);
  EXPECT_FALSE(cu_.mir_graph->PuntToInterpreter());
}

TEST_F(TypeInferenceTest, MoveObjectNull) {
  static const MethodDef methods[] = {
      { kClassName, "([I[D)V", "foo", kStatic },
  };
  static const MIRDef mirs[] = {
      DEF_CONST(3u, Instruction::CONST, 0u, 0),
      DEF_MOVE(3u, Instruction::MOVE_OBJECT, 1u, 0u),
      DEF_INVOKE2(3u, Instruction::INVOKE_STATIC, 0u, 1u, 0u),
  };

  PrepareMethods(methods);
  BuildDexFile("()V", true);
  PrepareSingleBlock();
  PrepareMIRs(mirs);
  PerformTypeInference();

  ASSERT_EQ(arraysize(mirs), mir_count_);
  static const SRegExpectation expectation = {
      1u,
      kExpectRef | kExpectNarrow | kExpectNull |
      kExpectArrayCore | kExpectArrayFp | kExpectArrayNarrow | kExpectArrayWide
  };
  ExpectSRegType(0u, expectation);
  ExpectSRegType(1u, expectation);
  EXPECT_EQ(cu_.disable_opt & (1u << kPromoteRegs), 0u);
  EXPECT_FALSE(cu_.mir_graph->PuntToInterpreter());
}

TEST_F(TypeInferenceTest, MoveNull1) {
  static const MethodDef methods[] = {
      { kClassName, "([I[D)V", "foo", kStatic },
  };
  static const MIRDef mirs[] = {
      DEF_CONST(3u, Instruction::CONST, 0u, 0),
      DEF_MOVE(3u, Instruction::MOVE, 1u, 0u),
      DEF_INVOKE2(3u, Instruction::INVOKE_STATIC, 0u, 1u, 0u),
  };

  PrepareMethods(methods);
  BuildDexFile("()V", true);
  PrepareSingleBlock();
  PrepareMIRs(mirs);
  PerformTypeInference();

  ASSERT_EQ(arraysize(mirs), mir_count_);
  static const SRegExpectation expectation = {
      1u,
      kExpectCore | kExpectRef | kExpectFp | kExpectNarrow | kExpectNull |
      kExpectArrayCore | kExpectArrayFp | kExpectArrayNarrow | kExpectArrayWide
  };
  ExpectSRegType(0u, expectation);
  ExpectSRegType(1u, expectation);
  // Type conflict using move instead of move-object for null, register promotion disabled.
  EXPECT_NE(cu_.disable_opt & (1u << kPromoteRegs), 0u);
  EXPECT_FALSE(cu_.mir_graph->PuntToInterpreter());
}

TEST_F(TypeInferenceTest, MoveNull2) {
  static const FieldDef sfields[] = {
      { kClassName, "[F", "staticArrayArrayFloatField" },
      { kClassName, "[I", "staticArrayIntField" },
      { kClassName, "[[I", "staticArrayArrayIntField" },
  };
  static const MIRDef mirs[] = {
      DEF_CONST(4u, Instruction::CONST, 0u, 0),
      DEF_MOVE(4u, Instruction::MOVE_OBJECT, 1u, 0u),
      DEF_MOVE(4u, Instruction::MOVE_OBJECT, 2u, 1u),
      DEF_SGET(5u, Instruction::SGET_OBJECT, 3u, 0u),
      DEF_SGET(5u, Instruction::SGET_OBJECT, 4u, 1u),
      DEF_SGET(5u, Instruction::SGET_OBJECT, 5u, 2u),
      DEF_PHI2(6u, 6u, 0u, 3u),
      DEF_PHI2(6u, 7u, 1u, 4u),
      DEF_PHI2(6u, 8u, 2u, 5u),
      DEF_UNOP(6u, Instruction::ARRAY_LENGTH, 9u, 6u),
      DEF_UNOP(6u, Instruction::ARRAY_LENGTH, 10u, 7u),
      DEF_UNOP(6u, Instruction::ARRAY_LENGTH, 11u, 8u),
      { 6u, Instruction::RETURN_OBJECT, 0, 0u, 1u, { 8u }, 0u, { } },
  };

  PrepareSFields(sfields);
  BuildDexFile("()[[I", true);
  PrepareDiamond();
  PrepareMIRs(mirs);
  PerformTypeInference();

  ASSERT_EQ(arraysize(mirs), mir_count_);
  static const SRegExpectation expectations[] = {
      { 1u, kExpectRef | kExpectNarrow | kExpectNull |
          kExpectArrayCore | kExpectArrayFp | kExpectArrayRef | kExpectArrayNarrow },
      { 1u, kExpectRef | kExpectNarrow | kExpectNull |
          kExpectArrayCore | kExpectArrayFp | kExpectArrayRef | kExpectArrayNarrow},
      { 1u, kExpectRef | kExpectNarrow | kExpectNull |
          kExpectArrayCore | kExpectArrayFp | kExpectArrayRef | kExpectArrayNarrow},
      { 1u, kExpectRef | kExpectNarrow | kExpectArrayFp | kExpectArrayNarrow },
      { 1u, kExpectRef | kExpectNarrow | kExpectArrayCore | kExpectArrayNarrow },
      { 2u, kExpectRef | kExpectNarrow | kExpectArrayCore | kExpectArrayNarrow },
      { 1u, kExpectRef | kExpectNarrow | kExpectArrayFp | kExpectArrayNarrow },
      { 1u, kExpectRef | kExpectNarrow | kExpectArrayCore | kExpectArrayNarrow },
      { 2u, kExpectRef | kExpectNarrow | kExpectArrayCore | kExpectArrayNarrow },
      { 0u, kExpectCore | kExpectNarrow },
      { 0u, kExpectCore | kExpectNarrow },
      { 0u, kExpectCore | kExpectNarrow },
  };
  for (int32_t sreg = 0; sreg != arraysize(expectations); ++sreg) {
    ExpectSRegType(sreg, expectations[sreg]);
  }
  // Type conflict in array type not propagated to actual register.
  EXPECT_EQ(cu_.disable_opt & (1u << kPromoteRegs), 0u);
  EXPECT_FALSE(cu_.mir_graph->PuntToInterpreter());
}

TEST_F(TypeInferenceTest, ReuseNull1) {
  static const FieldDef sfields[] = {
      { kClassName, "[I", "staticArrayLongField" },
      { kClassName, "[[F", "staticArrayArrayFloatField" },
  };
  static const MIRDef mirs[] = {
      DEF_CONST(3u, Instruction::CONST, 0u, 0),
      DEF_SPUT(3u, Instruction::SPUT_OBJECT, 0u, 0u),
      DEF_SPUT(3u, Instruction::SPUT_OBJECT, 0u, 1u),
  };

  PrepareSFields(sfields);
  BuildDexFile("()V", true);
  PrepareSingleBlock();
  PrepareMIRs(mirs);
  PerformTypeInference();

  ASSERT_EQ(arraysize(mirs), mir_count_);
  static const SRegExpectation expectation = {
      1u,
      kExpectRef | kExpectNarrow | kExpectNull |
      kExpectArrayCore | kExpectArrayRef | kExpectArrayFp | kExpectArrayNarrow
  };
  ExpectSRegType(0u, expectation);
  // Type conflict in array type not propagated to actual register.
  EXPECT_EQ(cu_.disable_opt & (1u << kPromoteRegs), 0u);
  EXPECT_FALSE(cu_.mir_graph->PuntToInterpreter());
}

TEST_F(TypeInferenceTest, ReuseNull2) {
  static const FieldDef sfields[] = {
      { kClassName, "[J", "staticArrayLongField" },
      { kClassName, "[[F", "staticArrayArrayFloatField" },
  };
  static const MIRDef mirs[] = {
      DEF_CONST(3u, Instruction::CONST, 0u, 0),
      DEF_SPUT(3u, Instruction::SPUT_OBJECT, 0u, 0u),
      DEF_SPUT(3u, Instruction::SPUT_OBJECT, 0u, 1u),
  };

  PrepareSFields(sfields);
  BuildDexFile("()V", true);
  PrepareSingleBlock();
  PrepareMIRs(mirs);
  PerformTypeInference();

  ASSERT_EQ(arraysize(mirs), mir_count_);
  static const SRegExpectation expectation = {
      1u,
      kExpectRef | kExpectNarrow | kExpectNull |
      kExpectArrayCore | kExpectArrayRef | kExpectArrayFp | kExpectArrayNarrow | kExpectArrayWide
  };
  ExpectSRegType(0u, expectation);
  // Type conflict in array type not propagated to actual register.
  EXPECT_EQ(cu_.disable_opt & (1u << kPromoteRegs), 0u);
  EXPECT_FALSE(cu_.mir_graph->PuntToInterpreter());
}

TEST_F(TypeInferenceTest, ArgIsNonNull) {
  constexpr uint32_t thiz = kLocalVRs;
  static const MIRDef mirs[] = {
      DEF_MOVE(3u, Instruction::MOVE_OBJECT, 0u, thiz),
  };

  BuildDexFile("(Ljava/lang/Object;)V", true);
  PrepareSingleBlock();
  PrepareMIRs(mirs);
  PerformTypeInference();

  ASSERT_EQ(arraysize(mirs), mir_count_);
  static const SRegExpectation expectation = {
      0u,
      kExpectRef | kExpectNarrow
  };
  ExpectSRegType(0u, expectation);
  // Type conflict in array type not propagated to actual register.
  EXPECT_EQ(cu_.disable_opt & (1u << kPromoteRegs), 0u);
  EXPECT_FALSE(cu_.mir_graph->PuntToInterpreter());
}

TEST_F(TypeInferenceTest, IfCc) {
  static const FieldDef sfields[] = {
      { kClassName, "I", "intField" },
  };
  static const MIRDef mirs[] = {
      DEF_SGET(3u, Instruction::SGET, 0u, 0u),
      DEF_CONST(3u, Instruction::CONST, 1u, 0u),
      { 3u, Instruction::IF_EQ, 0, 0u, 2, { 0u, 1u }, 0, { } },
  };

  PrepareSFields(sfields);
  BuildDexFile("()V", false);
  PrepareDiamond();
  PrepareMIRs(mirs);
  PerformTypeInference();

  ASSERT_EQ(arraysize(mirs), mir_count_);
  static const SRegExpectation expectations[] = {
      { 0u, kExpectCore | kExpectNarrow },
      { 0u, kExpectCore | kExpectNarrow },
  };
  for (int32_t sreg = 0; sreg != arraysize(expectations); ++sreg) {
    ExpectSRegType(sreg, expectations[sreg]);
  }
  EXPECT_EQ(cu_.disable_opt & (1u << kPromoteRegs), 0u);
  EXPECT_FALSE(cu_.mir_graph->PuntToInterpreter());
}

}  // namespace art
