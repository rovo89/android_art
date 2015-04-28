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

#include "dex/mir_field_info.h"
#include "global_value_numbering.h"
#include "local_value_numbering.h"
#include "gtest/gtest.h"

namespace art {

class LocalValueNumberingTest : public testing::Test {
 protected:
  struct IFieldDef {
    uint16_t field_idx;
    uintptr_t declaring_dex_file;
    uint16_t declaring_field_idx;
    bool is_volatile;
    DexMemAccessType type;
  };

  struct SFieldDef {
    uint16_t field_idx;
    uintptr_t declaring_dex_file;
    uint16_t declaring_field_idx;
    bool is_volatile;
    DexMemAccessType type;
  };

  struct MIRDef {
    static constexpr size_t kMaxSsaDefs = 2;
    static constexpr size_t kMaxSsaUses = 4;

    Instruction::Code opcode;
    int64_t value;
    uint32_t field_info;
    size_t num_uses;
    int32_t uses[kMaxSsaUses];
    size_t num_defs;
    int32_t defs[kMaxSsaDefs];
  };

#define DEF_CONST(opcode, reg, value) \
    { opcode, value, 0u, 0, { }, 1, { reg } }
#define DEF_CONST_WIDE(opcode, reg, value) \
    { opcode, value, 0u, 0, { }, 2, { reg, reg + 1 } }
#define DEF_CONST_STRING(opcode, reg, index) \
    { opcode, index, 0u, 0, { }, 1, { reg } }
#define DEF_IGET(opcode, reg, obj, field_info) \
    { opcode, 0u, field_info, 1, { obj }, 1, { reg } }
#define DEF_IGET_WIDE(opcode, reg, obj, field_info) \
    { opcode, 0u, field_info, 1, { obj }, 2, { reg, reg + 1 } }
#define DEF_IPUT(opcode, reg, obj, field_info) \
    { opcode, 0u, field_info, 2, { reg, obj }, 0, { } }
#define DEF_IPUT_WIDE(opcode, reg, obj, field_info) \
    { opcode, 0u, field_info, 3, { reg, reg + 1, obj }, 0, { } }
#define DEF_SGET(opcode, reg, field_info) \
    { opcode, 0u, field_info, 0, { }, 1, { reg } }
#define DEF_SGET_WIDE(opcode, reg, field_info) \
    { opcode, 0u, field_info, 0, { }, 2, { reg, reg + 1 } }
#define DEF_SPUT(opcode, reg, field_info) \
    { opcode, 0u, field_info, 1, { reg }, 0, { } }
#define DEF_SPUT_WIDE(opcode, reg, field_info) \
    { opcode, 0u, field_info, 2, { reg, reg + 1 }, 0, { } }
#define DEF_AGET(opcode, reg, obj, idx) \
    { opcode, 0u, 0u, 2, { obj, idx }, 1, { reg } }
#define DEF_AGET_WIDE(opcode, reg, obj, idx) \
    { opcode, 0u, 0u, 2, { obj, idx }, 2, { reg, reg + 1 } }
#define DEF_APUT(opcode, reg, obj, idx) \
    { opcode, 0u, 0u, 3, { reg, obj, idx }, 0, { } }
#define DEF_APUT_WIDE(opcode, reg, obj, idx) \
    { opcode, 0u, 0u, 4, { reg, reg + 1, obj, idx }, 0, { } }
#define DEF_INVOKE1(opcode, reg) \
    { opcode, 0u, 0u, 1, { reg }, 0, { } }
#define DEF_UNIQUE_REF(opcode, reg) \
    { opcode, 0u, 0u, 0, { }, 1, { reg } }  // CONST_CLASS, CONST_STRING, NEW_ARRAY, ...
#define DEF_DIV_REM(opcode, result, dividend, divisor) \
    { opcode, 0u, 0u, 2, { dividend, divisor }, 1, { result } }
#define DEF_DIV_REM_WIDE(opcode, result, dividend, divisor) \
    { opcode, 0u, 0u, 4, { dividend, dividend + 1, divisor, divisor + 1 }, 2, { result, result + 1 } }

  void DoPrepareIFields(const IFieldDef* defs, size_t count) {
    cu_.mir_graph->ifield_lowering_infos_.clear();
    cu_.mir_graph->ifield_lowering_infos_.reserve(count);
    for (size_t i = 0u; i != count; ++i) {
      const IFieldDef* def = &defs[i];
      MirIFieldLoweringInfo field_info(def->field_idx, def->type, false);
      if (def->declaring_dex_file != 0u) {
        field_info.declaring_dex_file_ = reinterpret_cast<const DexFile*>(def->declaring_dex_file);
        field_info.declaring_field_idx_ = def->declaring_field_idx;
        field_info.flags_ &= ~(def->is_volatile ? 0u : MirSFieldLoweringInfo::kFlagIsVolatile);
      }
      cu_.mir_graph->ifield_lowering_infos_.push_back(field_info);
    }
  }

  template <size_t count>
  void PrepareIFields(const IFieldDef (&defs)[count]) {
    DoPrepareIFields(defs, count);
  }

  void DoPrepareSFields(const SFieldDef* defs, size_t count) {
    cu_.mir_graph->sfield_lowering_infos_.clear();
    cu_.mir_graph->sfield_lowering_infos_.reserve(count);
    for (size_t i = 0u; i != count; ++i) {
      const SFieldDef* def = &defs[i];
      MirSFieldLoweringInfo field_info(def->field_idx, def->type);
      // Mark even unresolved fields as initialized.
      field_info.flags_ |= MirSFieldLoweringInfo::kFlagClassIsInitialized;
      // NOTE: MirSFieldLoweringInfo::kFlagClassIsInDexCache isn't used by LVN.
      if (def->declaring_dex_file != 0u) {
        field_info.declaring_dex_file_ = reinterpret_cast<const DexFile*>(def->declaring_dex_file);
        field_info.declaring_field_idx_ = def->declaring_field_idx;
        field_info.flags_ &= ~(def->is_volatile ? 0u : MirSFieldLoweringInfo::kFlagIsVolatile);
      }
      cu_.mir_graph->sfield_lowering_infos_.push_back(field_info);
    }
  }

  template <size_t count>
  void PrepareSFields(const SFieldDef (&defs)[count]) {
    DoPrepareSFields(defs, count);
  }

  void DoPrepareMIRs(const MIRDef* defs, size_t count) {
    mir_count_ = count;
    mirs_ = cu_.arena.AllocArray<MIR>(count, kArenaAllocMIR);
    ssa_reps_.resize(count);
    for (size_t i = 0u; i != count; ++i) {
      const MIRDef* def = &defs[i];
      MIR* mir = &mirs_[i];
      mir->dalvikInsn.opcode = def->opcode;
      mir->dalvikInsn.vB = static_cast<int32_t>(def->value);
      mir->dalvikInsn.vB_wide = def->value;
      if (IsInstructionIGetOrIPut(def->opcode)) {
        ASSERT_LT(def->field_info, cu_.mir_graph->ifield_lowering_infos_.size());
        mir->meta.ifield_lowering_info = def->field_info;
        ASSERT_EQ(cu_.mir_graph->ifield_lowering_infos_[def->field_info].MemAccessType(),
                  IGetOrIPutMemAccessType(def->opcode));
      } else if (IsInstructionSGetOrSPut(def->opcode)) {
        ASSERT_LT(def->field_info, cu_.mir_graph->sfield_lowering_infos_.size());
        mir->meta.sfield_lowering_info = def->field_info;
        ASSERT_EQ(cu_.mir_graph->sfield_lowering_infos_[def->field_info].MemAccessType(),
                  SGetOrSPutMemAccessType(def->opcode));
      }
      mir->ssa_rep = &ssa_reps_[i];
      mir->ssa_rep->num_uses = def->num_uses;
      mir->ssa_rep->uses = const_cast<int32_t*>(def->uses);  // Not modified by LVN.
      mir->ssa_rep->num_defs = def->num_defs;
      mir->ssa_rep->defs = const_cast<int32_t*>(def->defs);  // Not modified by LVN.
      mir->dalvikInsn.opcode = def->opcode;
      mir->offset = i;  // LVN uses offset only for debug output
      mir->optimization_flags = 0u;

      if (i != 0u) {
        mirs_[i - 1u].next = mir;
      }
    }
    mirs_[count - 1u].next = nullptr;
  }

  template <size_t count>
  void PrepareMIRs(const MIRDef (&defs)[count]) {
    DoPrepareMIRs(defs, count);
  }

  void MakeSFieldUninitialized(uint32_t sfield_index) {
    CHECK_LT(sfield_index, cu_.mir_graph->sfield_lowering_infos_.size());
    cu_.mir_graph->sfield_lowering_infos_[sfield_index].flags_ &=
        ~MirSFieldLoweringInfo::kFlagClassIsInitialized;
  }

  template <size_t count>
  void MarkAsWideSRegs(const int32_t (&sregs)[count]) {
    for (int32_t sreg : sregs) {
      cu_.mir_graph->reg_location_[sreg].wide = true;
      cu_.mir_graph->reg_location_[sreg + 1].wide = true;
      cu_.mir_graph->reg_location_[sreg + 1].high_word = true;
    }
  }

  void PerformLVN() {
    cu_.mir_graph->temp_.gvn.ifield_ids =  GlobalValueNumbering::PrepareGvnFieldIds(
        allocator_.get(), cu_.mir_graph->ifield_lowering_infos_);
    cu_.mir_graph->temp_.gvn.sfield_ids =  GlobalValueNumbering::PrepareGvnFieldIds(
        allocator_.get(), cu_.mir_graph->sfield_lowering_infos_);
    gvn_.reset(new (allocator_.get()) GlobalValueNumbering(&cu_, allocator_.get(),
                                                           GlobalValueNumbering::kModeLvn));
    lvn_.reset(new (allocator_.get()) LocalValueNumbering(gvn_.get(), 0u, allocator_.get()));
    value_names_.resize(mir_count_);
    for (size_t i = 0; i != mir_count_; ++i) {
      value_names_[i] =  lvn_->GetValueNumber(&mirs_[i]);
    }
    EXPECT_TRUE(gvn_->Good());
  }

  LocalValueNumberingTest()
      : pool_(),
        cu_(&pool_, kRuntimeISA, nullptr, nullptr),
        mir_count_(0u),
        mirs_(nullptr),
        ssa_reps_(),
        allocator_(),
        gvn_(),
        lvn_(),
        value_names_() {
    cu_.mir_graph.reset(new MIRGraph(&cu_, &cu_.arena));
    allocator_.reset(ScopedArenaAllocator::Create(&cu_.arena_stack));
    // By default, the zero-initialized reg_location_[.] with ref == false tells LVN that
    // 0 constants are integral, not references, and the values are all narrow.
    // Nothing else is used by LVN/GVN. Tests can override the default values as needed.
    cu_.mir_graph->reg_location_ = static_cast<RegLocation*>(cu_.arena.Alloc(
        kMaxSsaRegs * sizeof(cu_.mir_graph->reg_location_[0]), kArenaAllocRegAlloc));
    cu_.mir_graph->num_ssa_regs_ = kMaxSsaRegs;
  }

  static constexpr size_t kMaxSsaRegs = 16384u;

  ArenaPool pool_;
  CompilationUnit cu_;
  size_t mir_count_;
  MIR* mirs_;
  std::vector<SSARepresentation> ssa_reps_;
  std::unique_ptr<ScopedArenaAllocator> allocator_;
  std::unique_ptr<GlobalValueNumbering> gvn_;
  std::unique_ptr<LocalValueNumbering> lvn_;
  std::vector<uint16_t> value_names_;
};

TEST_F(LocalValueNumberingTest, IGetIGetInvokeIGet) {
  static const IFieldDef ifields[] = {
      { 1u, 1u, 1u, false, kDexMemAccessWord },
  };
  static const MIRDef mirs[] = {
      DEF_IGET(Instruction::IGET, 0u, 10u, 0u),
      DEF_IGET(Instruction::IGET, 1u, 10u, 0u),
      DEF_INVOKE1(Instruction::INVOKE_VIRTUAL, 11u),
      DEF_IGET(Instruction::IGET, 2u, 10u, 0u),
  };

  PrepareIFields(ifields);
  PrepareMIRs(mirs);
  PerformLVN();
  ASSERT_EQ(value_names_.size(), 4u);
  EXPECT_EQ(value_names_[0], value_names_[1]);
  EXPECT_NE(value_names_[0], value_names_[3]);
  EXPECT_EQ(mirs_[0].optimization_flags, 0u);
  EXPECT_EQ(mirs_[1].optimization_flags, MIR_IGNORE_NULL_CHECK);
  EXPECT_EQ(mirs_[2].optimization_flags, 0u);
  EXPECT_EQ(mirs_[3].optimization_flags, MIR_IGNORE_NULL_CHECK);
}

TEST_F(LocalValueNumberingTest, IGetIPutIGetIGetIGet) {
  static const IFieldDef ifields[] = {
      { 1u, 1u, 1u, false, kDexMemAccessObject },
      { 2u, 1u, 2u, false, kDexMemAccessWord },
  };
  static const MIRDef mirs[] = {
      DEF_IGET(Instruction::IGET_OBJECT, 0u, 10u, 0u),
      DEF_IPUT(Instruction::IPUT_OBJECT, 1u, 11u, 0u),  // May alias.
      DEF_IGET(Instruction::IGET_OBJECT, 2u, 10u, 0u),
      DEF_IGET(Instruction::IGET, 3u,  0u, 1u),
      DEF_IGET(Instruction::IGET, 4u,  2u, 1u),
  };

  PrepareIFields(ifields);
  PrepareMIRs(mirs);
  PerformLVN();
  ASSERT_EQ(value_names_.size(), 5u);
  EXPECT_NE(value_names_[0], value_names_[2]);
  EXPECT_NE(value_names_[3], value_names_[4]);
  for (size_t i = 0; i != arraysize(mirs); ++i) {
    EXPECT_EQ((i == 2u) ? MIR_IGNORE_NULL_CHECK : 0,
              mirs_[i].optimization_flags) << i;
  }
}

TEST_F(LocalValueNumberingTest, UniquePreserve1) {
  static const IFieldDef ifields[] = {
      { 1u, 1u, 1u, false, kDexMemAccessWord },
  };
  static const MIRDef mirs[] = {
      DEF_UNIQUE_REF(Instruction::NEW_INSTANCE, 10u),
      DEF_IGET(Instruction::IGET, 0u, 10u, 0u),
      DEF_IPUT(Instruction::IPUT, 1u, 11u, 0u),  // No aliasing since 10u is unique.
      DEF_IGET(Instruction::IGET, 2u, 10u, 0u),
  };

  PrepareIFields(ifields);
  PrepareMIRs(mirs);
  PerformLVN();
  ASSERT_EQ(value_names_.size(), 4u);
  EXPECT_EQ(value_names_[1], value_names_[3]);
  for (size_t i = 0; i != arraysize(mirs); ++i) {
    EXPECT_EQ((i == 1u || i == 3u) ? MIR_IGNORE_NULL_CHECK : 0,
              mirs_[i].optimization_flags) << i;
  }
}

TEST_F(LocalValueNumberingTest, UniquePreserve2) {
  static const IFieldDef ifields[] = {
      { 1u, 1u, 1u, false, kDexMemAccessWord },
  };
  static const MIRDef mirs[] = {
      DEF_UNIQUE_REF(Instruction::NEW_INSTANCE, 11u),
      DEF_IGET(Instruction::IGET, 0u, 10u, 0u),
      DEF_IPUT(Instruction::IPUT, 1u, 11u, 0u),  // No aliasing since 11u is unique.
      DEF_IGET(Instruction::IGET, 2u, 10u, 0u),
  };

  PrepareIFields(ifields);
  PrepareMIRs(mirs);
  PerformLVN();
  ASSERT_EQ(value_names_.size(), 4u);
  EXPECT_EQ(value_names_[1], value_names_[3]);
  for (size_t i = 0; i != arraysize(mirs); ++i) {
    EXPECT_EQ((i == 2u || i == 3u) ? MIR_IGNORE_NULL_CHECK : 0,
              mirs_[i].optimization_flags) << i;
  }
}

TEST_F(LocalValueNumberingTest, UniquePreserveAndEscape) {
  static const IFieldDef ifields[] = {
      { 1u, 1u, 1u, false, kDexMemAccessWord },
  };
  static const MIRDef mirs[] = {
      DEF_UNIQUE_REF(Instruction::NEW_INSTANCE, 10u),
      DEF_IGET(Instruction::IGET, 0u, 10u, 0u),
      DEF_INVOKE1(Instruction::INVOKE_VIRTUAL, 11u),  // 10u still unique.
      DEF_IGET(Instruction::IGET, 2u, 10u, 0u),
      DEF_INVOKE1(Instruction::INVOKE_VIRTUAL, 10u),  // 10u not unique anymore.
      DEF_IGET(Instruction::IGET, 3u, 10u, 0u),
  };

  PrepareIFields(ifields);
  PrepareMIRs(mirs);
  PerformLVN();
  ASSERT_EQ(value_names_.size(), 6u);
  EXPECT_EQ(value_names_[1], value_names_[3]);
  EXPECT_NE(value_names_[1], value_names_[5]);
  for (size_t i = 0; i != arraysize(mirs); ++i) {
    EXPECT_EQ((i == 1u || i == 3u || i == 4u || i == 5u) ? MIR_IGNORE_NULL_CHECK : 0,
              mirs_[i].optimization_flags) << i;
  }
}

TEST_F(LocalValueNumberingTest, Volatile) {
  static const IFieldDef ifields[] = {
      { 1u, 1u, 1u, false, kDexMemAccessWord },
      { 2u, 1u, 2u, true, kDexMemAccessWord },
  };
  static const MIRDef mirs[] = {
      DEF_IGET(Instruction::IGET, 0u, 10u, 1u),  // Volatile.
      DEF_IGET(Instruction::IGET, 1u,  0u, 0u),  // Non-volatile.
      DEF_IGET(Instruction::IGET, 2u, 10u, 1u),  // Volatile.
      DEF_IGET(Instruction::IGET, 3u,  2u, 1u),  // Non-volatile.
      DEF_IGET(Instruction::IGET, 4u,  0u, 0u),  // Non-volatile.
  };

  PrepareIFields(ifields);
  PrepareMIRs(mirs);
  PerformLVN();
  ASSERT_EQ(value_names_.size(), 5u);
  EXPECT_NE(value_names_[0], value_names_[2]);  // Volatile has always different value name.
  EXPECT_NE(value_names_[1], value_names_[3]);  // Used different base because of volatile.
  EXPECT_NE(value_names_[1], value_names_[4]);  // Not guaranteed to be the same after "acquire".

  for (size_t i = 0; i != arraysize(mirs); ++i) {
    EXPECT_EQ((i == 2u || i == 4u) ? MIR_IGNORE_NULL_CHECK : 0,
              mirs_[i].optimization_flags) << i;
  }
}

TEST_F(LocalValueNumberingTest, UnresolvedIField) {
  static const IFieldDef ifields[] = {
      { 1u, 1u, 1u, false, kDexMemAccessWord },  // Resolved field #1.
      { 2u, 1u, 2u, false, kDexMemAccessWide },  // Resolved field #2.
      { 3u, 0u, 0u, false, kDexMemAccessWord },  // Unresolved field.
  };
  static const MIRDef mirs[] = {
      DEF_UNIQUE_REF(Instruction::NEW_INSTANCE, 30u),
      DEF_IGET(Instruction::IGET, 1u, 30u, 0u),             // Resolved field #1, unique object.
      DEF_IGET(Instruction::IGET, 2u, 31u, 0u),             // Resolved field #1.
      DEF_IGET_WIDE(Instruction::IGET_WIDE, 3u, 31u, 1u),   // Resolved field #2.
      DEF_IGET(Instruction::IGET, 5u, 32u, 2u),             // Unresolved IGET can be "acquire".
      DEF_IGET(Instruction::IGET, 6u, 30u, 0u),             // Resolved field #1, unique object.
      DEF_IGET(Instruction::IGET, 7u, 31u, 0u),             // Resolved field #1.
      DEF_IGET_WIDE(Instruction::IGET_WIDE, 8u, 31u, 1u),   // Resolved field #2.
      DEF_IPUT(Instruction::IPUT, 10u, 32u, 2u),            // IPUT clobbers field #1 (#2 is wide).
      DEF_IGET(Instruction::IGET, 11u, 30u, 0u),            // Resolved field #1, unique object.
      DEF_IGET(Instruction::IGET, 12u, 31u, 0u),            // Resolved field #1, new value name.
      DEF_IGET_WIDE(Instruction::IGET_WIDE, 13u, 31u, 1u),  // Resolved field #2.
      DEF_IGET_WIDE(Instruction::IGET_WIDE, 15u, 30u, 1u),  // Resolved field #2, unique object.
      DEF_IPUT(Instruction::IPUT, 17u, 30u, 2u),            // IPUT clobbers field #1 (#2 is wide).
      DEF_IGET(Instruction::IGET, 18u, 30u, 0u),            // Resolved field #1, unique object.
      DEF_IGET_WIDE(Instruction::IGET_WIDE, 19u, 30u, 1u),  // Resolved field #2, unique object.
  };

  PrepareIFields(ifields);
  PrepareMIRs(mirs);
  static const int32_t wide_sregs[] = { 3, 8, 13, 15, 19 };
  MarkAsWideSRegs(wide_sregs);
  PerformLVN();
  ASSERT_EQ(value_names_.size(), 16u);
  // Unresolved field is potentially volatile, so we need to adhere to the volatile semantics.
  EXPECT_EQ(value_names_[1], value_names_[5]);    // Unique object.
  EXPECT_NE(value_names_[2], value_names_[6]);    // Not guaranteed to be the same after "acquire".
  EXPECT_NE(value_names_[3], value_names_[7]);    // Not guaranteed to be the same after "acquire".
  EXPECT_EQ(value_names_[1], value_names_[9]);    // Unique object.
  EXPECT_NE(value_names_[6], value_names_[10]);   // This aliased with unresolved IPUT.
  EXPECT_EQ(value_names_[7], value_names_[11]);   // Still the same after "release".
  EXPECT_EQ(value_names_[12], value_names_[15]);  // Still the same after "release".
  EXPECT_NE(value_names_[1], value_names_[14]);   // This aliased with unresolved IPUT.
  EXPECT_EQ(mirs_[0].optimization_flags, 0u);
  EXPECT_EQ(mirs_[1].optimization_flags, MIR_IGNORE_NULL_CHECK);
  EXPECT_EQ(mirs_[2].optimization_flags, 0u);
  EXPECT_EQ(mirs_[3].optimization_flags, MIR_IGNORE_NULL_CHECK);
  EXPECT_EQ(mirs_[4].optimization_flags, 0u);
  for (size_t i = 5u; i != mir_count_; ++i) {
    EXPECT_EQ((i == 1u || i == 3u || i >=5u) ? MIR_IGNORE_NULL_CHECK : 0,
              mirs_[i].optimization_flags) << i;
  }
}

TEST_F(LocalValueNumberingTest, UnresolvedSField) {
  static const SFieldDef sfields[] = {
      { 1u, 1u, 1u, false, kDexMemAccessWord },  // Resolved field #1.
      { 2u, 1u, 2u, false, kDexMemAccessWide },  // Resolved field #2.
      { 3u, 0u, 0u, false, kDexMemAccessWord },  // Unresolved field.
  };
  static const MIRDef mirs[] = {
      DEF_SGET(Instruction::SGET, 0u, 0u),            // Resolved field #1.
      DEF_SGET_WIDE(Instruction::SGET_WIDE, 1u, 1u),  // Resolved field #2.
      DEF_SGET(Instruction::SGET, 3u, 2u),            // Unresolved SGET can be "acquire".
      DEF_SGET(Instruction::SGET, 4u, 0u),            // Resolved field #1.
      DEF_SGET_WIDE(Instruction::SGET_WIDE, 5u, 1u),  // Resolved field #2.
      DEF_SPUT(Instruction::SPUT, 7u, 2u),            // SPUT clobbers field #1 (#2 is wide).
      DEF_SGET(Instruction::SGET, 8u, 0u),            // Resolved field #1.
      DEF_SGET_WIDE(Instruction::SGET_WIDE, 9u, 1u),  // Resolved field #2.
  };

  PrepareSFields(sfields);
  PrepareMIRs(mirs);
  static const int32_t wide_sregs[] = { 1, 5, 9 };
  MarkAsWideSRegs(wide_sregs);
  PerformLVN();
  ASSERT_EQ(value_names_.size(), 8u);
  // Unresolved field is potentially volatile, so we need to adhere to the volatile semantics.
  EXPECT_NE(value_names_[0], value_names_[3]);  // Not guaranteed to be the same after "acquire".
  EXPECT_NE(value_names_[1], value_names_[4]);  // Not guaranteed to be the same after "acquire".
  EXPECT_NE(value_names_[3], value_names_[6]);  // This aliased with unresolved IPUT.
  EXPECT_EQ(value_names_[4], value_names_[7]);  // Still the same after "release".
  for (size_t i = 0u; i != mir_count_; ++i) {
    EXPECT_EQ(0, mirs_[i].optimization_flags) << i;
  }
}

TEST_F(LocalValueNumberingTest, UninitializedSField) {
  static const IFieldDef ifields[] = {
      { 1u, 1u, 1u, false, kDexMemAccessWord },  // Resolved field #1.
  };
  static const SFieldDef sfields[] = {
      { 1u, 1u, 1u, false, kDexMemAccessWord },  // Resolved field #1.
      { 2u, 1u, 2u, false, kDexMemAccessWord },  // Resolved field #2; uninitialized.
  };
  static const MIRDef mirs[] = {
      DEF_UNIQUE_REF(Instruction::NEW_INSTANCE, 200u),
      DEF_IGET(Instruction::IGET, 1u, 100u, 0u),
      DEF_IGET(Instruction::IGET, 2u, 200u, 0u),
      DEF_SGET(Instruction::SGET, 3u, 0u),
      DEF_SGET(Instruction::SGET, 4u, 1u),            // Can call <clinit>().
      DEF_IGET(Instruction::IGET, 5u, 100u, 0u),      // Differs from 1u.
      DEF_IGET(Instruction::IGET, 6u, 200u, 0u),      // Same as 2u.
      DEF_SGET(Instruction::SGET, 7u, 0u),            // Differs from 3u.
  };

  PrepareIFields(ifields);
  PrepareSFields(sfields);
  MakeSFieldUninitialized(1u);
  PrepareMIRs(mirs);
  PerformLVN();
  ASSERT_EQ(value_names_.size(), 8u);
  EXPECT_NE(value_names_[1], value_names_[5]);
  EXPECT_EQ(value_names_[2], value_names_[6]);
  EXPECT_NE(value_names_[3], value_names_[7]);
}

TEST_F(LocalValueNumberingTest, ConstString) {
  static const MIRDef mirs[] = {
      DEF_CONST_STRING(Instruction::CONST_STRING, 0u, 0u),
      DEF_CONST_STRING(Instruction::CONST_STRING, 1u, 0u),
      DEF_CONST_STRING(Instruction::CONST_STRING, 2u, 2u),
      DEF_CONST_STRING(Instruction::CONST_STRING, 3u, 0u),
      DEF_INVOKE1(Instruction::INVOKE_DIRECT, 2u),
      DEF_CONST_STRING(Instruction::CONST_STRING, 4u, 2u),
  };

  PrepareMIRs(mirs);
  PerformLVN();
  ASSERT_EQ(value_names_.size(), 6u);
  EXPECT_EQ(value_names_[1], value_names_[0]);
  EXPECT_NE(value_names_[2], value_names_[0]);
  EXPECT_EQ(value_names_[3], value_names_[0]);
  EXPECT_EQ(value_names_[5], value_names_[2]);
}

TEST_F(LocalValueNumberingTest, SameValueInDifferentMemoryLocations) {
  static const IFieldDef ifields[] = {
      { 1u, 1u, 1u, false, kDexMemAccessWord },
      { 2u, 1u, 2u, false, kDexMemAccessWord },
  };
  static const SFieldDef sfields[] = {
      { 3u, 1u, 3u, false, kDexMemAccessWord },
  };
  static const MIRDef mirs[] = {
      DEF_UNIQUE_REF(Instruction::NEW_ARRAY, 201u),
      DEF_IGET(Instruction::IGET, 0u, 100u, 0u),
      DEF_IPUT(Instruction::IPUT, 0u, 100u, 1u),
      DEF_IPUT(Instruction::IPUT, 0u, 101u, 1u),
      DEF_APUT(Instruction::APUT, 0u, 200u, 300u),
      DEF_APUT(Instruction::APUT, 0u, 200u, 301u),
      DEF_APUT(Instruction::APUT, 0u, 201u, 300u),
      DEF_APUT(Instruction::APUT, 0u, 201u, 301u),
      DEF_SPUT(Instruction::SPUT, 0u, 0u),
      DEF_IGET(Instruction::IGET, 9u, 100u, 0u),
      DEF_IGET(Instruction::IGET, 10u, 100u, 1u),
      DEF_IGET(Instruction::IGET, 11u, 101u, 1u),
      DEF_AGET(Instruction::AGET, 12u, 200u, 300u),
      DEF_AGET(Instruction::AGET, 13u, 200u, 301u),
      DEF_AGET(Instruction::AGET, 14u, 201u, 300u),
      DEF_AGET(Instruction::AGET, 15u, 201u, 301u),
      DEF_SGET(Instruction::SGET, 16u, 0u),
  };

  PrepareIFields(ifields);
  PrepareSFields(sfields);
  PrepareMIRs(mirs);
  PerformLVN();
  ASSERT_EQ(value_names_.size(), 17u);
  for (size_t i = 9; i != arraysize(mirs); ++i) {
    EXPECT_EQ(value_names_[1], value_names_[i]) << i;
  }
  for (size_t i = 0; i != arraysize(mirs); ++i) {
    int expected_flags =
        ((i == 2u || (i >= 5u && i <= 7u) || (i >= 9u && i <= 15u)) ? MIR_IGNORE_NULL_CHECK : 0) |
        ((i >= 12u && i <= 15u) ? MIR_IGNORE_RANGE_CHECK : 0);
    EXPECT_EQ(expected_flags, mirs_[i].optimization_flags) << i;
  }
}

TEST_F(LocalValueNumberingTest, UniqueArrayAliasing) {
  static const MIRDef mirs[] = {
      DEF_UNIQUE_REF(Instruction::NEW_ARRAY, 20u),
      DEF_AGET(Instruction::AGET, 1u, 20u, 40u),
      DEF_APUT(Instruction::APUT, 2u, 20u, 41u),  // May alias with index for sreg 40u.
      DEF_AGET(Instruction::AGET, 3u, 20u, 40u),
  };

  PrepareMIRs(mirs);
  PerformLVN();
  ASSERT_EQ(value_names_.size(), 4u);
  EXPECT_NE(value_names_[1], value_names_[3]);
  for (size_t i = 0; i != arraysize(mirs); ++i) {
    int expected_flags =
        ((i >= 1u) ? MIR_IGNORE_NULL_CHECK : 0) |
        ((i == 3u) ? MIR_IGNORE_RANGE_CHECK : 0);
    EXPECT_EQ(expected_flags, mirs_[i].optimization_flags) << i;
  }
}

TEST_F(LocalValueNumberingTest, EscapingRefs) {
  static const IFieldDef ifields[] = {
      { 1u, 1u, 1u, false, kDexMemAccessWord },    // Field #1.
      { 2u, 1u, 2u, false, kDexMemAccessWord },    // Field #2.
      { 3u, 1u, 3u, false, kDexMemAccessObject },  // For storing escaping refs.
      { 4u, 1u, 4u, false, kDexMemAccessWide },    // Wide.
      { 5u, 0u, 0u, false, kDexMemAccessWord },    // Unresolved field, int.
      { 6u, 0u, 0u, false, kDexMemAccessWide },    // Unresolved field, wide.
  };
  static const MIRDef mirs[] = {
      DEF_UNIQUE_REF(Instruction::NEW_INSTANCE, 20u),
      DEF_IGET(Instruction::IGET, 1u, 20u, 0u),
      DEF_IGET(Instruction::IGET, 2u, 20u, 1u),
      DEF_IPUT(Instruction::IPUT_OBJECT, 20u, 30u, 2u),      // Ref escapes.
      DEF_IGET(Instruction::IGET, 4u, 20u, 0u),
      DEF_IGET(Instruction::IGET, 5u, 20u, 1u),
      DEF_IPUT(Instruction::IPUT, 6u, 31u, 0u),              // May alias with field #1.
      DEF_IGET(Instruction::IGET, 7u, 20u, 0u),              // New value.
      DEF_IGET(Instruction::IGET, 8u, 20u, 1u),              // Still the same.
      DEF_IPUT_WIDE(Instruction::IPUT_WIDE, 9u, 31u, 3u),    // No aliasing, different type.
      DEF_IGET(Instruction::IGET, 11u, 20u, 0u),
      DEF_IGET(Instruction::IGET, 12u, 20u, 1u),
      DEF_IPUT_WIDE(Instruction::IPUT_WIDE, 13u, 31u, 5u),   // No aliasing, different type.
      DEF_IGET(Instruction::IGET, 15u, 20u, 0u),
      DEF_IGET(Instruction::IGET, 16u, 20u, 1u),
      DEF_IPUT(Instruction::IPUT, 17u, 31u, 4u),             // Aliasing, same type.
      DEF_IGET(Instruction::IGET, 18u, 20u, 0u),
      DEF_IGET(Instruction::IGET, 19u, 20u, 1u),
  };

  PrepareIFields(ifields);
  PrepareMIRs(mirs);
  static const int32_t wide_sregs[] = { 9, 13 };
  MarkAsWideSRegs(wide_sregs);
  PerformLVN();
  ASSERT_EQ(value_names_.size(), 18u);
  EXPECT_EQ(value_names_[1], value_names_[4]);
  EXPECT_EQ(value_names_[2], value_names_[5]);
  EXPECT_NE(value_names_[4], value_names_[7]);  // New value.
  EXPECT_EQ(value_names_[5], value_names_[8]);
  EXPECT_EQ(value_names_[7], value_names_[10]);
  EXPECT_EQ(value_names_[8], value_names_[11]);
  EXPECT_EQ(value_names_[10], value_names_[13]);
  EXPECT_EQ(value_names_[11], value_names_[14]);
  EXPECT_NE(value_names_[13], value_names_[16]);  // New value.
  EXPECT_NE(value_names_[14], value_names_[17]);  // New value.
  for (size_t i = 0u; i != mir_count_; ++i) {
    int expected =
        ((i != 0u && i != 3u && i != 6u) ? MIR_IGNORE_NULL_CHECK : 0) |
        ((i == 3u) ? MIR_STORE_NON_NULL_VALUE: 0);
    EXPECT_EQ(expected, mirs_[i].optimization_flags) << i;
  }
}

TEST_F(LocalValueNumberingTest, EscapingArrayRefs) {
  static const MIRDef mirs[] = {
      DEF_UNIQUE_REF(Instruction::NEW_ARRAY, 20u),
      DEF_AGET(Instruction::AGET, 1u, 20u, 40u),
      DEF_AGET(Instruction::AGET, 2u, 20u, 41u),
      DEF_APUT(Instruction::APUT_OBJECT, 20u, 30u, 42u),    // Array ref escapes.
      DEF_AGET(Instruction::AGET, 4u, 20u, 40u),
      DEF_AGET(Instruction::AGET, 5u, 20u, 41u),
      DEF_APUT_WIDE(Instruction::APUT_WIDE, 6u, 31u, 43u),  // No aliasing, different type.
      DEF_AGET(Instruction::AGET, 8u, 20u, 40u),
      DEF_AGET(Instruction::AGET, 9u, 20u, 41u),
      DEF_APUT(Instruction::APUT, 10u, 32u, 40u),           // May alias with all elements.
      DEF_AGET(Instruction::AGET, 11u, 20u, 40u),           // New value (same index name).
      DEF_AGET(Instruction::AGET, 12u, 20u, 41u),           // New value (different index name).
  };

  PrepareMIRs(mirs);
  static const int32_t wide_sregs[] = { 6 };
  MarkAsWideSRegs(wide_sregs);
  PerformLVN();
  ASSERT_EQ(value_names_.size(), 12u);
  EXPECT_EQ(value_names_[1], value_names_[4]);
  EXPECT_EQ(value_names_[2], value_names_[5]);
  EXPECT_EQ(value_names_[4], value_names_[7]);
  EXPECT_EQ(value_names_[5], value_names_[8]);
  EXPECT_NE(value_names_[7], value_names_[10]);  // New value.
  EXPECT_NE(value_names_[8], value_names_[11]);  // New value.
  for (size_t i = 0u; i != mir_count_; ++i) {
    int expected =
        ((i != 0u && i != 3u && i != 6u && i != 9u) ? MIR_IGNORE_NULL_CHECK : 0u) |
        ((i >= 4 && i != 6u && i != 9u) ? MIR_IGNORE_RANGE_CHECK : 0u) |
        ((i == 3u) ? MIR_STORE_NON_NULL_VALUE: 0);
    EXPECT_EQ(expected, mirs_[i].optimization_flags) << i;
  }
}

TEST_F(LocalValueNumberingTest, StoringSameValueKeepsMemoryVersion) {
  static const IFieldDef ifields[] = {
      { 1u, 1u, 1u, false, kDexMemAccessWord },
      { 2u, 1u, 2u, false, kDexMemAccessWord },
  };
  static const SFieldDef sfields[] = {
      { 2u, 1u, 2u, false, kDexMemAccessWord },
  };
  static const MIRDef mirs[] = {
      DEF_IGET(Instruction::IGET, 0u, 30u, 0u),
      DEF_IGET(Instruction::IGET, 1u, 31u, 0u),
      DEF_IPUT(Instruction::IPUT, 1u, 31u, 0u),            // Store the same value.
      DEF_IGET(Instruction::IGET, 3u, 30u, 0u),
      DEF_AGET(Instruction::AGET, 4u, 32u, 40u),
      DEF_AGET(Instruction::AGET, 5u, 33u, 40u),
      DEF_APUT(Instruction::APUT, 5u, 33u, 40u),           // Store the same value.
      DEF_AGET(Instruction::AGET, 7u, 32u, 40u),
      DEF_SGET(Instruction::SGET, 8u, 0u),
      DEF_SPUT(Instruction::SPUT, 8u, 0u),                 // Store the same value.
      DEF_SGET(Instruction::SGET, 10u, 0u),
      DEF_UNIQUE_REF(Instruction::NEW_INSTANCE, 50u),      // Test with unique references.
      { Instruction::FILLED_NEW_ARRAY, 0, 0u, 2, { 12u, 13u }, 0, { } },
      DEF_UNIQUE_REF(Instruction::MOVE_RESULT_OBJECT, 51u),
      DEF_IGET(Instruction::IGET, 14u, 50u, 0u),
      DEF_IGET(Instruction::IGET, 15u, 50u, 1u),
      DEF_IPUT(Instruction::IPUT, 15u, 50u, 1u),           // Store the same value.
      DEF_IGET(Instruction::IGET, 17u, 50u, 0u),
      DEF_AGET(Instruction::AGET, 18u, 51u, 40u),
      DEF_AGET(Instruction::AGET, 19u, 51u, 41u),
      DEF_APUT(Instruction::APUT, 19u, 51u, 41u),          // Store the same value.
      DEF_AGET(Instruction::AGET, 21u, 51u, 40u),
  };

  PrepareIFields(ifields);
  PrepareSFields(sfields);
  PrepareMIRs(mirs);
  PerformLVN();
  ASSERT_EQ(value_names_.size(), 22u);
  EXPECT_NE(value_names_[0], value_names_[1]);
  EXPECT_EQ(value_names_[0], value_names_[3]);
  EXPECT_NE(value_names_[4], value_names_[5]);
  EXPECT_EQ(value_names_[4], value_names_[7]);
  EXPECT_EQ(value_names_[8], value_names_[10]);
  EXPECT_NE(value_names_[14], value_names_[15]);
  EXPECT_EQ(value_names_[14], value_names_[17]);
  EXPECT_NE(value_names_[18], value_names_[19]);
  EXPECT_EQ(value_names_[18], value_names_[21]);
  for (size_t i = 0u; i != mir_count_; ++i) {
    int expected =
        ((i == 2u || i == 3u || i == 6u || i == 7u || (i >= 14u)) ? MIR_IGNORE_NULL_CHECK : 0u) |
        ((i == 6u || i == 7u || i >= 20u) ? MIR_IGNORE_RANGE_CHECK : 0u);
    EXPECT_EQ(expected, mirs_[i].optimization_flags) << i;
  }
}

TEST_F(LocalValueNumberingTest, FilledNewArrayTracking) {
  if (!kLocalValueNumberingEnableFilledNewArrayTracking) {
    // Feature disabled.
    return;
  }
  static const MIRDef mirs[] = {
      DEF_CONST(Instruction::CONST, 0u, 100),
      DEF_CONST(Instruction::CONST, 1u, 200),
      { Instruction::FILLED_NEW_ARRAY, 0, 0u, 2, { 0u, 1u }, 0, { } },
      DEF_UNIQUE_REF(Instruction::MOVE_RESULT_OBJECT, 10u),
      DEF_CONST(Instruction::CONST, 20u, 0),
      DEF_CONST(Instruction::CONST, 21u, 1),
      DEF_AGET(Instruction::AGET, 6u, 10u, 20u),
      DEF_AGET(Instruction::AGET, 7u, 10u, 21u),
  };

  PrepareMIRs(mirs);
  PerformLVN();
  ASSERT_EQ(value_names_.size(), 8u);
  EXPECT_EQ(value_names_[0], value_names_[6]);
  EXPECT_EQ(value_names_[1], value_names_[7]);
  for (size_t i = 0u; i != mir_count_; ++i) {
    int expected = (i == 6u || i == 7u) ? (MIR_IGNORE_NULL_CHECK | MIR_IGNORE_RANGE_CHECK) : 0u;
    EXPECT_EQ(expected, mirs_[i].optimization_flags) << i;
  }
}

TEST_F(LocalValueNumberingTest, ClInitOnSget) {
  static const SFieldDef sfields[] = {
      { 0u, 1u, 0u, false, kDexMemAccessObject },
      { 1u, 2u, 1u, false, kDexMemAccessObject },
  };
  static const MIRDef mirs[] = {
      DEF_SGET(Instruction::SGET_OBJECT, 0u, 0u),
      DEF_AGET(Instruction::AGET, 1u, 0u, 100u),
      DEF_SGET(Instruction::SGET_OBJECT, 2u, 1u),
      DEF_SGET(Instruction::SGET_OBJECT, 3u, 0u),
      DEF_AGET(Instruction::AGET, 4u, 3u, 100u),
  };

  PrepareSFields(sfields);
  MakeSFieldUninitialized(1u);
  PrepareMIRs(mirs);
  PerformLVN();
  ASSERT_EQ(value_names_.size(), 5u);
  EXPECT_NE(value_names_[0], value_names_[3]);
}

TEST_F(LocalValueNumberingTest, DivZeroCheck) {
  static const MIRDef mirs[] = {
      DEF_DIV_REM(Instruction::DIV_INT, 1u, 10u, 20u),
      DEF_DIV_REM(Instruction::DIV_INT, 2u, 20u, 20u),
      DEF_DIV_REM(Instruction::DIV_INT_2ADDR, 3u, 10u, 1u),
      DEF_DIV_REM(Instruction::REM_INT, 4u, 30u, 20u),
      DEF_DIV_REM_WIDE(Instruction::REM_LONG, 5u, 12u, 14u),
      DEF_DIV_REM_WIDE(Instruction::DIV_LONG_2ADDR, 7u, 16u, 14u),
  };

  static const bool expected_ignore_div_zero_check[] = {
      false, true, false, true, false, true,
  };

  PrepareMIRs(mirs);
  static const int32_t wide_sregs[] = { 5, 7, 12, 14, 16 };
  MarkAsWideSRegs(wide_sregs);
  PerformLVN();
  for (size_t i = 0u; i != mir_count_; ++i) {
    int expected = expected_ignore_div_zero_check[i] ? MIR_IGNORE_DIV_ZERO_CHECK : 0u;
    EXPECT_EQ(expected, mirs_[i].optimization_flags) << i;
  }
}

TEST_F(LocalValueNumberingTest, ConstWide) {
  static const MIRDef mirs[] = {
      // Core reg constants.
      DEF_CONST(Instruction::CONST_WIDE_16, 0u, 0),
      DEF_CONST(Instruction::CONST_WIDE_16, 2u, 1),
      DEF_CONST(Instruction::CONST_WIDE_16, 4u, -1),
      DEF_CONST(Instruction::CONST_WIDE_32, 6u, 1 << 16),
      DEF_CONST(Instruction::CONST_WIDE_32, 8u, -1 << 16),
      DEF_CONST(Instruction::CONST_WIDE_32, 10u, (1 << 16) + 1),
      DEF_CONST(Instruction::CONST_WIDE_32, 12u, (1 << 16) - 1),
      DEF_CONST(Instruction::CONST_WIDE_32, 14u, -(1 << 16) + 1),
      DEF_CONST(Instruction::CONST_WIDE_32, 16u, -(1 << 16) - 1),
      DEF_CONST(Instruction::CONST_WIDE, 18u, INT64_C(1) << 32),
      DEF_CONST(Instruction::CONST_WIDE, 20u, INT64_C(-1) << 32),
      DEF_CONST(Instruction::CONST_WIDE, 22u, (INT64_C(1) << 32) + 1),
      DEF_CONST(Instruction::CONST_WIDE, 24u, (INT64_C(1) << 32) - 1),
      DEF_CONST(Instruction::CONST_WIDE, 26u, (INT64_C(-1) << 32) + 1),
      DEF_CONST(Instruction::CONST_WIDE, 28u, (INT64_C(-1) << 32) - 1),
      DEF_CONST(Instruction::CONST_WIDE_HIGH16, 30u, 1),       // Effectively 1 << 48.
      DEF_CONST(Instruction::CONST_WIDE_HIGH16, 32u, 0xffff),  // Effectively -1 << 48.
      DEF_CONST(Instruction::CONST_WIDE, 34u, (INT64_C(1) << 48) + 1),
      DEF_CONST(Instruction::CONST_WIDE, 36u, (INT64_C(1) << 48) - 1),
      DEF_CONST(Instruction::CONST_WIDE, 38u, (INT64_C(-1) << 48) + 1),
      DEF_CONST(Instruction::CONST_WIDE, 40u, (INT64_C(-1) << 48) - 1),
      // FP reg constants.
      DEF_CONST(Instruction::CONST_WIDE_16, 42u, 0),
      DEF_CONST(Instruction::CONST_WIDE_16, 44u, 1),
      DEF_CONST(Instruction::CONST_WIDE_16, 46u, -1),
      DEF_CONST(Instruction::CONST_WIDE_32, 48u, 1 << 16),
      DEF_CONST(Instruction::CONST_WIDE_32, 50u, -1 << 16),
      DEF_CONST(Instruction::CONST_WIDE_32, 52u, (1 << 16) + 1),
      DEF_CONST(Instruction::CONST_WIDE_32, 54u, (1 << 16) - 1),
      DEF_CONST(Instruction::CONST_WIDE_32, 56u, -(1 << 16) + 1),
      DEF_CONST(Instruction::CONST_WIDE_32, 58u, -(1 << 16) - 1),
      DEF_CONST(Instruction::CONST_WIDE, 60u, INT64_C(1) << 32),
      DEF_CONST(Instruction::CONST_WIDE, 62u, INT64_C(-1) << 32),
      DEF_CONST(Instruction::CONST_WIDE, 64u, (INT64_C(1) << 32) + 1),
      DEF_CONST(Instruction::CONST_WIDE, 66u, (INT64_C(1) << 32) - 1),
      DEF_CONST(Instruction::CONST_WIDE, 68u, (INT64_C(-1) << 32) + 1),
      DEF_CONST(Instruction::CONST_WIDE, 70u, (INT64_C(-1) << 32) - 1),
      DEF_CONST(Instruction::CONST_WIDE_HIGH16, 72u, 1),       // Effectively 1 << 48.
      DEF_CONST(Instruction::CONST_WIDE_HIGH16, 74u, 0xffff),  // Effectively -1 << 48.
      DEF_CONST(Instruction::CONST_WIDE, 76u, (INT64_C(1) << 48) + 1),
      DEF_CONST(Instruction::CONST_WIDE, 78u, (INT64_C(1) << 48) - 1),
      DEF_CONST(Instruction::CONST_WIDE, 80u, (INT64_C(-1) << 48) + 1),
      DEF_CONST(Instruction::CONST_WIDE, 82u, (INT64_C(-1) << 48) - 1),
  };

  PrepareMIRs(mirs);
  for (size_t i = 0; i != arraysize(mirs); ++i) {
    const int32_t wide_sregs[] = { mirs_[i].ssa_rep->defs[0] };
    MarkAsWideSRegs(wide_sregs);
  }
  for (size_t i = arraysize(mirs) / 2u; i != arraysize(mirs); ++i) {
    cu_.mir_graph->reg_location_[mirs_[i].ssa_rep->defs[0]].fp = true;
  }
  PerformLVN();
  for (size_t i = 0u; i != mir_count_; ++i) {
    for (size_t j = i + 1u; j != mir_count_; ++j) {
      EXPECT_NE(value_names_[i], value_names_[j]) << i << " " << j;
    }
  }
}

TEST_F(LocalValueNumberingTest, Const) {
  static const MIRDef mirs[] = {
      // Core reg constants.
      DEF_CONST(Instruction::CONST_4, 0u, 0),
      DEF_CONST(Instruction::CONST_4, 1u, 1),
      DEF_CONST(Instruction::CONST_4, 2u, -1),
      DEF_CONST(Instruction::CONST_16, 3u, 1 << 4),
      DEF_CONST(Instruction::CONST_16, 4u, -1 << 4),
      DEF_CONST(Instruction::CONST_16, 5u, (1 << 4) + 1),
      DEF_CONST(Instruction::CONST_16, 6u, (1 << 4) - 1),
      DEF_CONST(Instruction::CONST_16, 7u, -(1 << 4) + 1),
      DEF_CONST(Instruction::CONST_16, 8u, -(1 << 4) - 1),
      DEF_CONST(Instruction::CONST_HIGH16, 9u, 1),       // Effectively 1 << 16.
      DEF_CONST(Instruction::CONST_HIGH16, 10u, 0xffff),  // Effectively -1 << 16.
      DEF_CONST(Instruction::CONST, 11u, (1 << 16) + 1),
      DEF_CONST(Instruction::CONST, 12u, (1 << 16) - 1),
      DEF_CONST(Instruction::CONST, 13u, (-1 << 16) + 1),
      DEF_CONST(Instruction::CONST, 14u, (-1 << 16) - 1),
      // FP reg constants.
      DEF_CONST(Instruction::CONST_4, 15u, 0),
      DEF_CONST(Instruction::CONST_4, 16u, 1),
      DEF_CONST(Instruction::CONST_4, 17u, -1),
      DEF_CONST(Instruction::CONST_16, 18u, 1 << 4),
      DEF_CONST(Instruction::CONST_16, 19u, -1 << 4),
      DEF_CONST(Instruction::CONST_16, 20u, (1 << 4) + 1),
      DEF_CONST(Instruction::CONST_16, 21u, (1 << 4) - 1),
      DEF_CONST(Instruction::CONST_16, 22u, -(1 << 4) + 1),
      DEF_CONST(Instruction::CONST_16, 23u, -(1 << 4) - 1),
      DEF_CONST(Instruction::CONST_HIGH16, 24u, 1),       // Effectively 1 << 16.
      DEF_CONST(Instruction::CONST_HIGH16, 25u, 0xffff),  // Effectively -1 << 16.
      DEF_CONST(Instruction::CONST, 26u, (1 << 16) + 1),
      DEF_CONST(Instruction::CONST, 27u, (1 << 16) - 1),
      DEF_CONST(Instruction::CONST, 28u, (-1 << 16) + 1),
      DEF_CONST(Instruction::CONST, 29u, (-1 << 16) - 1),
      // null reference constant.
      DEF_CONST(Instruction::CONST_4, 30u, 0),
  };

  PrepareMIRs(mirs);
  static_assert((arraysize(mirs) & 1) != 0, "missing null or unmatched fp/core");
  cu_.mir_graph->reg_location_[arraysize(mirs) - 1].ref = true;
  for (size_t i = arraysize(mirs) / 2u; i != arraysize(mirs) - 1; ++i) {
    cu_.mir_graph->reg_location_[mirs_[i].ssa_rep->defs[0]].fp = true;
  }
  PerformLVN();
  for (size_t i = 0u; i != mir_count_; ++i) {
    for (size_t j = i + 1u; j != mir_count_; ++j) {
      EXPECT_NE(value_names_[i], value_names_[j]) << i << " " << j;
    }
  }
}

}  // namespace art
