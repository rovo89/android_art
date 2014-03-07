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

#include <vector>

#include "local_value_numbering.h"
#include "compiler_internals.h"
#include "gtest/gtest.h"

namespace art {

class LocalValueNumberingTest : public testing::Test {
 protected:
  struct IFieldDef {
    uint16_t field_idx;
    uintptr_t declaring_dex_file;
    uint16_t declaring_field_idx;
    bool is_volatile;
  };

  struct SFieldDef {
    uint16_t field_idx;
    uintptr_t declaring_dex_file;
    uint16_t declaring_field_idx;
    bool is_volatile;
  };

  struct MIRDef {
    static constexpr size_t kMaxSsaDefs = 2;
    static constexpr size_t kMaxSsaUses = 3;

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
#define DEF_INVOKE1(opcode, reg) \
    { opcode, 0u, 0u, 1, { reg }, 0, { } }
#define DEF_UNIQUE_REF(opcode, reg) \
    { opcode, 0u, 0u, 0, { }, 1, { reg } }  // CONST_CLASS, CONST_STRING, NEW_ARRAY, ...

  void DoPrepareIFields(const IFieldDef* defs, size_t count) {
    cu_.mir_graph->ifield_lowering_infos_.Reset();
    cu_.mir_graph->ifield_lowering_infos_.Resize(count);
    for (size_t i = 0u; i != count; ++i) {
      const IFieldDef* def = &defs[i];
      MirIFieldLoweringInfo field_info(def->field_idx);
      if (def->declaring_dex_file != 0u) {
        field_info.declaring_dex_file_ = reinterpret_cast<const DexFile*>(def->declaring_dex_file);
        field_info.declaring_field_idx_ = def->declaring_field_idx;
        field_info.flags_ = 0u |  // Without kFlagIsStatic.
            (def->is_volatile ? MirIFieldLoweringInfo::kFlagIsVolatile : 0u);
      }
      cu_.mir_graph->ifield_lowering_infos_.Insert(field_info);
    }
  }

  template <size_t count>
  void PrepareIFields(const IFieldDef (&defs)[count]) {
    DoPrepareIFields(defs, count);
  }

  void DoPrepareSFields(const SFieldDef* defs, size_t count) {
    cu_.mir_graph->sfield_lowering_infos_.Reset();
    cu_.mir_graph->sfield_lowering_infos_.Resize(count);
    for (size_t i = 0u; i != count; ++i) {
      const SFieldDef* def = &defs[i];
      MirSFieldLoweringInfo field_info(def->field_idx);
      if (def->declaring_dex_file != 0u) {
        field_info.declaring_dex_file_ = reinterpret_cast<const DexFile*>(def->declaring_dex_file);
        field_info.declaring_field_idx_ = def->declaring_field_idx;
        field_info.flags_ = MirSFieldLoweringInfo::kFlagIsStatic |
            (def->is_volatile ? MirSFieldLoweringInfo::kFlagIsVolatile : 0u);
      }
      cu_.mir_graph->sfield_lowering_infos_.Insert(field_info);
    }
  }

  template <size_t count>
  void PrepareSFields(const SFieldDef (&defs)[count]) {
    DoPrepareSFields(defs, count);
  }

  void DoPrepareMIRs(const MIRDef* defs, size_t count) {
    mir_count_ = count;
    mirs_ = reinterpret_cast<MIR*>(cu_.arena.Alloc(sizeof(MIR) * count, kArenaAllocMIR));
    ssa_reps_.resize(count);
    for (size_t i = 0u; i != count; ++i) {
      const MIRDef* def = &defs[i];
      MIR* mir = &mirs_[i];
      mir->dalvikInsn.opcode = def->opcode;
      mir->dalvikInsn.vB = static_cast<int32_t>(def->value);
      mir->dalvikInsn.vB_wide = def->value;
      if (def->opcode >= Instruction::IGET && def->opcode <= Instruction::IPUT_SHORT) {
        ASSERT_LT(def->field_info, cu_.mir_graph->ifield_lowering_infos_.Size());
        mir->meta.ifield_lowering_info = def->field_info;
      } else if (def->opcode >= Instruction::SGET && def->opcode <= Instruction::SPUT_SHORT) {
        ASSERT_LT(def->field_info, cu_.mir_graph->sfield_lowering_infos_.Size());
        mir->meta.sfield_lowering_info = def->field_info;
      }
      mir->ssa_rep = &ssa_reps_[i];
      mir->ssa_rep->num_uses = def->num_uses;
      mir->ssa_rep->uses = const_cast<int32_t*>(def->uses);  // Not modified by LVN.
      mir->ssa_rep->fp_use = nullptr;  // Not used by LVN.
      mir->ssa_rep->num_defs = def->num_defs;
      mir->ssa_rep->defs = const_cast<int32_t*>(def->defs);  // Not modified by LVN.
      mir->ssa_rep->fp_def = nullptr;  // Not used by LVN.
      mir->dalvikInsn.opcode = def->opcode;
      mir->offset = i;  // LVN uses offset only for debug output
      mir->width = 1u;  // Not used by LVN.
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

  void PerformLVN() {
    value_names_.resize(mir_count_);
    for (size_t i = 0; i != mir_count_; ++i) {
      value_names_[i] =  lvn_->GetValueNumber(&mirs_[i]);
    }
  }

  LocalValueNumberingTest()
      : pool_(),
        cu_(&pool_),
        mir_count_(0u),
        mirs_(nullptr),
        lvn_(LocalValueNumbering::Create(&cu_)) {
    cu_.mir_graph.reset(new MIRGraph(&cu_, &cu_.arena));
  }

  ArenaPool pool_;
  CompilationUnit cu_;
  size_t mir_count_;
  MIR* mirs_;
  std::vector<SSARepresentation> ssa_reps_;
  std::vector<uint16_t> value_names_;
  UniquePtr<LocalValueNumbering> lvn_;
};

TEST_F(LocalValueNumberingTest, TestIGetIGetInvokeIGet) {
  static const IFieldDef ifields[] = {
      { 1u, 1u, 1u, false }
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

TEST_F(LocalValueNumberingTest, TestIGetIPutIGetIGetIGet) {
  static const IFieldDef ifields[] = {
      { 1u, 1u, 1u, false },
      { 2u, 1u, 2u, false },
  };
  static const MIRDef mirs[] = {
      DEF_IGET(Instruction::IGET, 0u, 10u, 0u),
      DEF_IPUT(Instruction::IPUT, 1u, 11u, 0u),  // May alias.
      DEF_IGET(Instruction::IGET, 2u, 10u, 0u),
      DEF_IGET(Instruction::IGET, 3u,  0u, 1u),
      DEF_IGET(Instruction::IGET, 4u,  2u, 1u),
  };

  PrepareIFields(ifields);
  PrepareMIRs(mirs);
  PerformLVN();
  ASSERT_EQ(value_names_.size(), 5u);
  EXPECT_NE(value_names_[0], value_names_[2]);
  EXPECT_NE(value_names_[3], value_names_[4]);
  EXPECT_EQ(mirs_[0].optimization_flags, 0u);
  EXPECT_EQ(mirs_[1].optimization_flags, 0u);
  EXPECT_EQ(mirs_[2].optimization_flags, MIR_IGNORE_NULL_CHECK);
  EXPECT_EQ(mirs_[3].optimization_flags, 0u);
  EXPECT_EQ(mirs_[4].optimization_flags, 0u);
}

TEST_F(LocalValueNumberingTest, TestUniquePreserve1) {
  static const IFieldDef ifields[] = {
      { 1u, 1u, 1u, false },
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
  EXPECT_EQ(mirs_[1].optimization_flags, MIR_IGNORE_NULL_CHECK);
  EXPECT_EQ(mirs_[2].optimization_flags, 0u);
  EXPECT_EQ(mirs_[3].optimization_flags, MIR_IGNORE_NULL_CHECK);
}

TEST_F(LocalValueNumberingTest, TestUniquePreserve2) {
  static const IFieldDef ifields[] = {
      { 1u, 1u, 1u, false },
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
  EXPECT_EQ(mirs_[1].optimization_flags, 0u);
  EXPECT_EQ(mirs_[2].optimization_flags, MIR_IGNORE_NULL_CHECK);
  EXPECT_EQ(mirs_[3].optimization_flags, MIR_IGNORE_NULL_CHECK);
}

TEST_F(LocalValueNumberingTest, TestUniquePreserveAndEscape) {
  static const IFieldDef ifields[] = {
      { 1u, 1u, 1u, false },
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
  EXPECT_EQ(mirs_[1].optimization_flags, MIR_IGNORE_NULL_CHECK);
  EXPECT_EQ(mirs_[3].optimization_flags, MIR_IGNORE_NULL_CHECK);
  EXPECT_EQ(mirs_[5].optimization_flags, MIR_IGNORE_NULL_CHECK);
}

TEST_F(LocalValueNumberingTest, TestVolatile) {
  static const IFieldDef ifields[] = {
      { 1u, 1u, 1u, false },
      { 2u, 1u, 2u, true },
  };
  static const MIRDef mirs[] = {
      DEF_IGET(Instruction::IGET, 0u, 10u, 1u),  // Volatile.
      DEF_IGET(Instruction::IGET, 1u,  0u, 0u),  // Non-volatile.
      DEF_IGET(Instruction::IGET, 2u, 10u, 1u),  // Volatile.
      DEF_IGET(Instruction::IGET, 3u,  2u, 1u),  // Non-volatile.
  };

  PrepareIFields(ifields);
  PrepareMIRs(mirs);
  PerformLVN();
  ASSERT_EQ(value_names_.size(), 4u);
  EXPECT_NE(value_names_[0], value_names_[2]);  // Volatile has always different value name.
  EXPECT_NE(value_names_[1], value_names_[3]);  // Used different base because of volatile.
  EXPECT_EQ(mirs_[0].optimization_flags, 0u);
  EXPECT_EQ(mirs_[1].optimization_flags, 0u);
  EXPECT_EQ(mirs_[2].optimization_flags, MIR_IGNORE_NULL_CHECK);
  EXPECT_EQ(mirs_[3].optimization_flags, 0u);
}

}  // namespace art
