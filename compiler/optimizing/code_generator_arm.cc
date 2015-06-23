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

#include "code_generator_arm.h"

#include "arch/arm/instruction_set_features_arm.h"
#include "art_method.h"
#include "entrypoints/quick/quick_entrypoints.h"
#include "gc/accounting/card_table.h"
#include "intrinsics.h"
#include "intrinsics_arm.h"
#include "mirror/array-inl.h"
#include "mirror/class-inl.h"
#include "thread.h"
#include "utils/arm/assembler_arm.h"
#include "utils/arm/managed_register_arm.h"
#include "utils/assembler.h"
#include "utils/stack_checks.h"

namespace art {

namespace arm {

static bool ExpectedPairLayout(Location location) {
  // We expected this for both core and fpu register pairs.
  return ((location.low() & 1) == 0) && (location.low() + 1 == location.high());
}

static constexpr int kCurrentMethodStackOffset = 0;

// We unconditionally allocate R5 to ensure we can do long operations
// with baseline.
static constexpr Register kCoreSavedRegisterForBaseline = R5;
static constexpr Register kCoreCalleeSaves[] =
    { R5, R6, R7, R8, R10, R11, PC };
static constexpr SRegister kFpuCalleeSaves[] =
    { S16, S17, S18, S19, S20, S21, S22, S23, S24, S25, S26, S27, S28, S29, S30, S31 };

// D31 cannot be split into two S registers, and the register allocator only works on
// S registers. Therefore there is no need to block it.
static constexpr DRegister DTMP = D31;

#define __ reinterpret_cast<ArmAssembler*>(codegen->GetAssembler())->
#define QUICK_ENTRY_POINT(x) QUICK_ENTRYPOINT_OFFSET(kArmWordSize, x).Int32Value()

class NullCheckSlowPathARM : public SlowPathCodeARM {
 public:
  explicit NullCheckSlowPathARM(HNullCheck* instruction) : instruction_(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    CodeGeneratorARM* arm_codegen = down_cast<CodeGeneratorARM*>(codegen);
    __ Bind(GetEntryLabel());
    arm_codegen->InvokeRuntime(
        QUICK_ENTRY_POINT(pThrowNullPointer), instruction_, instruction_->GetDexPc(), this);
  }

 private:
  HNullCheck* const instruction_;
  DISALLOW_COPY_AND_ASSIGN(NullCheckSlowPathARM);
};

class DivZeroCheckSlowPathARM : public SlowPathCodeARM {
 public:
  explicit DivZeroCheckSlowPathARM(HDivZeroCheck* instruction) : instruction_(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    CodeGeneratorARM* arm_codegen = down_cast<CodeGeneratorARM*>(codegen);
    __ Bind(GetEntryLabel());
    arm_codegen->InvokeRuntime(
        QUICK_ENTRY_POINT(pThrowDivZero), instruction_, instruction_->GetDexPc(), this);
  }

 private:
  HDivZeroCheck* const instruction_;
  DISALLOW_COPY_AND_ASSIGN(DivZeroCheckSlowPathARM);
};

class SuspendCheckSlowPathARM : public SlowPathCodeARM {
 public:
  SuspendCheckSlowPathARM(HSuspendCheck* instruction, HBasicBlock* successor)
      : instruction_(instruction), successor_(successor) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    CodeGeneratorARM* arm_codegen = down_cast<CodeGeneratorARM*>(codegen);
    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, instruction_->GetLocations());
    arm_codegen->InvokeRuntime(
        QUICK_ENTRY_POINT(pTestSuspend), instruction_, instruction_->GetDexPc(), this);
    RestoreLiveRegisters(codegen, instruction_->GetLocations());
    if (successor_ == nullptr) {
      __ b(GetReturnLabel());
    } else {
      __ b(arm_codegen->GetLabelOf(successor_));
    }
  }

  Label* GetReturnLabel() {
    DCHECK(successor_ == nullptr);
    return &return_label_;
  }

  HBasicBlock* GetSuccessor() const {
    return successor_;
  }

 private:
  HSuspendCheck* const instruction_;
  // If not null, the block to branch to after the suspend check.
  HBasicBlock* const successor_;

  // If `successor_` is null, the label to branch to after the suspend check.
  Label return_label_;

  DISALLOW_COPY_AND_ASSIGN(SuspendCheckSlowPathARM);
};

class BoundsCheckSlowPathARM : public SlowPathCodeARM {
 public:
  BoundsCheckSlowPathARM(HBoundsCheck* instruction,
                         Location index_location,
                         Location length_location)
      : instruction_(instruction),
        index_location_(index_location),
        length_location_(length_location) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    CodeGeneratorARM* arm_codegen = down_cast<CodeGeneratorARM*>(codegen);
    __ Bind(GetEntryLabel());
    // We're moving two locations to locations that could overlap, so we need a parallel
    // move resolver.
    InvokeRuntimeCallingConvention calling_convention;
    codegen->EmitParallelMoves(
        index_location_,
        Location::RegisterLocation(calling_convention.GetRegisterAt(0)),
        Primitive::kPrimInt,
        length_location_,
        Location::RegisterLocation(calling_convention.GetRegisterAt(1)),
        Primitive::kPrimInt);
    arm_codegen->InvokeRuntime(
        QUICK_ENTRY_POINT(pThrowArrayBounds), instruction_, instruction_->GetDexPc(), this);
  }

 private:
  HBoundsCheck* const instruction_;
  const Location index_location_;
  const Location length_location_;

  DISALLOW_COPY_AND_ASSIGN(BoundsCheckSlowPathARM);
};

class LoadClassSlowPathARM : public SlowPathCodeARM {
 public:
  LoadClassSlowPathARM(HLoadClass* cls,
                       HInstruction* at,
                       uint32_t dex_pc,
                       bool do_clinit)
      : cls_(cls), at_(at), dex_pc_(dex_pc), do_clinit_(do_clinit) {
    DCHECK(at->IsLoadClass() || at->IsClinitCheck());
  }

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    LocationSummary* locations = at_->GetLocations();

    CodeGeneratorARM* arm_codegen = down_cast<CodeGeneratorARM*>(codegen);
    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, locations);

    InvokeRuntimeCallingConvention calling_convention;
    __ LoadImmediate(calling_convention.GetRegisterAt(0), cls_->GetTypeIndex());
    int32_t entry_point_offset = do_clinit_
        ? QUICK_ENTRY_POINT(pInitializeStaticStorage)
        : QUICK_ENTRY_POINT(pInitializeType);
    arm_codegen->InvokeRuntime(entry_point_offset, at_, dex_pc_, this);

    // Move the class to the desired location.
    Location out = locations->Out();
    if (out.IsValid()) {
      DCHECK(out.IsRegister() && !locations->GetLiveRegisters()->ContainsCoreRegister(out.reg()));
      arm_codegen->Move32(locations->Out(), Location::RegisterLocation(R0));
    }
    RestoreLiveRegisters(codegen, locations);
    __ b(GetExitLabel());
  }

 private:
  // The class this slow path will load.
  HLoadClass* const cls_;

  // The instruction where this slow path is happening.
  // (Might be the load class or an initialization check).
  HInstruction* const at_;

  // The dex PC of `at_`.
  const uint32_t dex_pc_;

  // Whether to initialize the class.
  const bool do_clinit_;

  DISALLOW_COPY_AND_ASSIGN(LoadClassSlowPathARM);
};

class LoadStringSlowPathARM : public SlowPathCodeARM {
 public:
  explicit LoadStringSlowPathARM(HLoadString* instruction) : instruction_(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    LocationSummary* locations = instruction_->GetLocations();
    DCHECK(!locations->GetLiveRegisters()->ContainsCoreRegister(locations->Out().reg()));

    CodeGeneratorARM* arm_codegen = down_cast<CodeGeneratorARM*>(codegen);
    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, locations);

    InvokeRuntimeCallingConvention calling_convention;
    __ LoadImmediate(calling_convention.GetRegisterAt(0), instruction_->GetStringIndex());
    arm_codegen->InvokeRuntime(
        QUICK_ENTRY_POINT(pResolveString), instruction_, instruction_->GetDexPc(), this);
    arm_codegen->Move32(locations->Out(), Location::RegisterLocation(R0));

    RestoreLiveRegisters(codegen, locations);
    __ b(GetExitLabel());
  }

 private:
  HLoadString* const instruction_;

  DISALLOW_COPY_AND_ASSIGN(LoadStringSlowPathARM);
};

class TypeCheckSlowPathARM : public SlowPathCodeARM {
 public:
  TypeCheckSlowPathARM(HInstruction* instruction,
                       Location class_to_check,
                       Location object_class,
                       uint32_t dex_pc)
      : instruction_(instruction),
        class_to_check_(class_to_check),
        object_class_(object_class),
        dex_pc_(dex_pc) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    LocationSummary* locations = instruction_->GetLocations();
    DCHECK(instruction_->IsCheckCast()
           || !locations->GetLiveRegisters()->ContainsCoreRegister(locations->Out().reg()));

    CodeGeneratorARM* arm_codegen = down_cast<CodeGeneratorARM*>(codegen);
    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, locations);

    // We're moving two locations to locations that could overlap, so we need a parallel
    // move resolver.
    InvokeRuntimeCallingConvention calling_convention;
    codegen->EmitParallelMoves(
        class_to_check_,
        Location::RegisterLocation(calling_convention.GetRegisterAt(0)),
        Primitive::kPrimNot,
        object_class_,
        Location::RegisterLocation(calling_convention.GetRegisterAt(1)),
        Primitive::kPrimNot);

    if (instruction_->IsInstanceOf()) {
      arm_codegen->InvokeRuntime(
          QUICK_ENTRY_POINT(pInstanceofNonTrivial), instruction_, dex_pc_, this);
      arm_codegen->Move32(locations->Out(), Location::RegisterLocation(R0));
    } else {
      DCHECK(instruction_->IsCheckCast());
      arm_codegen->InvokeRuntime(QUICK_ENTRY_POINT(pCheckCast), instruction_, dex_pc_, this);
    }

    RestoreLiveRegisters(codegen, locations);
    __ b(GetExitLabel());
  }

 private:
  HInstruction* const instruction_;
  const Location class_to_check_;
  const Location object_class_;
  uint32_t dex_pc_;

  DISALLOW_COPY_AND_ASSIGN(TypeCheckSlowPathARM);
};

class DeoptimizationSlowPathARM : public SlowPathCodeARM {
 public:
  explicit DeoptimizationSlowPathARM(HInstruction* instruction)
    : instruction_(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, instruction_->GetLocations());
    DCHECK(instruction_->IsDeoptimize());
    HDeoptimize* deoptimize = instruction_->AsDeoptimize();
    uint32_t dex_pc = deoptimize->GetDexPc();
    CodeGeneratorARM* arm_codegen = down_cast<CodeGeneratorARM*>(codegen);
    arm_codegen->InvokeRuntime(QUICK_ENTRY_POINT(pDeoptimize), instruction_, dex_pc, this);
  }

 private:
  HInstruction* const instruction_;
  DISALLOW_COPY_AND_ASSIGN(DeoptimizationSlowPathARM);
};

#undef __

#undef __
#define __ reinterpret_cast<ArmAssembler*>(GetAssembler())->

inline Condition ARMCondition(IfCondition cond) {
  switch (cond) {
    case kCondEQ: return EQ;
    case kCondNE: return NE;
    case kCondLT: return LT;
    case kCondLE: return LE;
    case kCondGT: return GT;
    case kCondGE: return GE;
    default:
      LOG(FATAL) << "Unknown if condition";
  }
  return EQ;        // Unreachable.
}

inline Condition ARMOppositeCondition(IfCondition cond) {
  switch (cond) {
    case kCondEQ: return NE;
    case kCondNE: return EQ;
    case kCondLT: return GE;
    case kCondLE: return GT;
    case kCondGT: return LE;
    case kCondGE: return LT;
    default:
      LOG(FATAL) << "Unknown if condition";
  }
  return EQ;        // Unreachable.
}

void CodeGeneratorARM::DumpCoreRegister(std::ostream& stream, int reg) const {
  stream << ArmManagedRegister::FromCoreRegister(Register(reg));
}

void CodeGeneratorARM::DumpFloatingPointRegister(std::ostream& stream, int reg) const {
  stream << ArmManagedRegister::FromSRegister(SRegister(reg));
}

size_t CodeGeneratorARM::SaveCoreRegister(size_t stack_index, uint32_t reg_id) {
  __ StoreToOffset(kStoreWord, static_cast<Register>(reg_id), SP, stack_index);
  return kArmWordSize;
}

size_t CodeGeneratorARM::RestoreCoreRegister(size_t stack_index, uint32_t reg_id) {
  __ LoadFromOffset(kLoadWord, static_cast<Register>(reg_id), SP, stack_index);
  return kArmWordSize;
}

size_t CodeGeneratorARM::SaveFloatingPointRegister(size_t stack_index, uint32_t reg_id) {
  __ StoreSToOffset(static_cast<SRegister>(reg_id), SP, stack_index);
  return kArmWordSize;
}

size_t CodeGeneratorARM::RestoreFloatingPointRegister(size_t stack_index, uint32_t reg_id) {
  __ LoadSFromOffset(static_cast<SRegister>(reg_id), SP, stack_index);
  return kArmWordSize;
}

CodeGeneratorARM::CodeGeneratorARM(HGraph* graph,
                                   const ArmInstructionSetFeatures& isa_features,
                                   const CompilerOptions& compiler_options)
    : CodeGenerator(graph,
                    kNumberOfCoreRegisters,
                    kNumberOfSRegisters,
                    kNumberOfRegisterPairs,
                    ComputeRegisterMask(reinterpret_cast<const int*>(kCoreCalleeSaves),
                                        arraysize(kCoreCalleeSaves)),
                    ComputeRegisterMask(reinterpret_cast<const int*>(kFpuCalleeSaves),
                                        arraysize(kFpuCalleeSaves)),
                    compiler_options),
      block_labels_(graph->GetArena(), 0),
      location_builder_(graph, this),
      instruction_visitor_(graph, this),
      move_resolver_(graph->GetArena(), this),
      assembler_(true),
      isa_features_(isa_features) {
  // Save the PC register to mimic Quick.
  AddAllocatedRegister(Location::RegisterLocation(PC));
}

Location CodeGeneratorARM::AllocateFreeRegister(Primitive::Type type) const {
  switch (type) {
    case Primitive::kPrimLong: {
      size_t reg = FindFreeEntry(blocked_register_pairs_, kNumberOfRegisterPairs);
      ArmManagedRegister pair =
          ArmManagedRegister::FromRegisterPair(static_cast<RegisterPair>(reg));
      DCHECK(!blocked_core_registers_[pair.AsRegisterPairLow()]);
      DCHECK(!blocked_core_registers_[pair.AsRegisterPairHigh()]);

      blocked_core_registers_[pair.AsRegisterPairLow()] = true;
      blocked_core_registers_[pair.AsRegisterPairHigh()] = true;
      UpdateBlockedPairRegisters();
      return Location::RegisterPairLocation(pair.AsRegisterPairLow(), pair.AsRegisterPairHigh());
    }

    case Primitive::kPrimByte:
    case Primitive::kPrimBoolean:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      int reg = FindFreeEntry(blocked_core_registers_, kNumberOfCoreRegisters);
      // Block all register pairs that contain `reg`.
      for (int i = 0; i < kNumberOfRegisterPairs; i++) {
        ArmManagedRegister current =
            ArmManagedRegister::FromRegisterPair(static_cast<RegisterPair>(i));
        if (current.AsRegisterPairLow() == reg || current.AsRegisterPairHigh() == reg) {
          blocked_register_pairs_[i] = true;
        }
      }
      return Location::RegisterLocation(reg);
    }

    case Primitive::kPrimFloat: {
      int reg = FindFreeEntry(blocked_fpu_registers_, kNumberOfSRegisters);
      return Location::FpuRegisterLocation(reg);
    }

    case Primitive::kPrimDouble: {
      int reg = FindTwoFreeConsecutiveAlignedEntries(blocked_fpu_registers_, kNumberOfSRegisters);
      DCHECK_EQ(reg % 2, 0);
      return Location::FpuRegisterPairLocation(reg, reg + 1);
    }

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << type;
  }

  return Location();
}

void CodeGeneratorARM::SetupBlockedRegisters(bool is_baseline) const {
  // Don't allocate the dalvik style register pair passing.
  blocked_register_pairs_[R1_R2] = true;

  // Stack register, LR and PC are always reserved.
  blocked_core_registers_[SP] = true;
  blocked_core_registers_[LR] = true;
  blocked_core_registers_[PC] = true;

  // Reserve thread register.
  blocked_core_registers_[TR] = true;

  // Reserve temp register.
  blocked_core_registers_[IP] = true;

  if (is_baseline) {
    for (size_t i = 0; i < arraysize(kCoreCalleeSaves); ++i) {
      blocked_core_registers_[kCoreCalleeSaves[i]] = true;
    }

    blocked_core_registers_[kCoreSavedRegisterForBaseline] = false;

    for (size_t i = 0; i < arraysize(kFpuCalleeSaves); ++i) {
      blocked_fpu_registers_[kFpuCalleeSaves[i]] = true;
    }
  }

  UpdateBlockedPairRegisters();
}

void CodeGeneratorARM::UpdateBlockedPairRegisters() const {
  for (int i = 0; i < kNumberOfRegisterPairs; i++) {
    ArmManagedRegister current =
        ArmManagedRegister::FromRegisterPair(static_cast<RegisterPair>(i));
    if (blocked_core_registers_[current.AsRegisterPairLow()]
        || blocked_core_registers_[current.AsRegisterPairHigh()]) {
      blocked_register_pairs_[i] = true;
    }
  }
}

InstructionCodeGeneratorARM::InstructionCodeGeneratorARM(HGraph* graph, CodeGeneratorARM* codegen)
      : HGraphVisitor(graph),
        assembler_(codegen->GetAssembler()),
        codegen_(codegen) {}

void CodeGeneratorARM::ComputeSpillMask() {
  core_spill_mask_ = allocated_registers_.GetCoreRegisters() & core_callee_save_mask_;
  // Save one extra register for baseline. Note that on thumb2, there is no easy
  // instruction to restore just the PC, so this actually helps both baseline
  // and non-baseline to save and restore at least two registers at entry and exit.
  core_spill_mask_ |= (1 << kCoreSavedRegisterForBaseline);
  DCHECK_NE(core_spill_mask_, 0u) << "At least the return address register must be saved";
  fpu_spill_mask_ = allocated_registers_.GetFloatingPointRegisters() & fpu_callee_save_mask_;
  // We use vpush and vpop for saving and restoring floating point registers, which take
  // a SRegister and the number of registers to save/restore after that SRegister. We
  // therefore update the `fpu_spill_mask_` to also contain those registers not allocated,
  // but in the range.
  if (fpu_spill_mask_ != 0) {
    uint32_t least_significant_bit = LeastSignificantBit(fpu_spill_mask_);
    uint32_t most_significant_bit = MostSignificantBit(fpu_spill_mask_);
    for (uint32_t i = least_significant_bit + 1 ; i < most_significant_bit; ++i) {
      fpu_spill_mask_ |= (1 << i);
    }
  }
}

static dwarf::Reg DWARFReg(Register reg) {
  return dwarf::Reg::ArmCore(static_cast<int>(reg));
}

static dwarf::Reg DWARFReg(SRegister reg) {
  return dwarf::Reg::ArmFp(static_cast<int>(reg));
}

void CodeGeneratorARM::GenerateFrameEntry() {
  bool skip_overflow_check =
      IsLeafMethod() && !FrameNeedsStackCheck(GetFrameSize(), InstructionSet::kArm);
  DCHECK(GetCompilerOptions().GetImplicitStackOverflowChecks());
  __ Bind(&frame_entry_label_);

  if (HasEmptyFrame()) {
    return;
  }

  if (!skip_overflow_check) {
    __ AddConstant(IP, SP, -static_cast<int32_t>(GetStackOverflowReservedBytes(kArm)));
    __ LoadFromOffset(kLoadWord, IP, IP, 0);
    RecordPcInfo(nullptr, 0);
  }

  // PC is in the list of callee-save to mimic Quick, but we need to push
  // LR at entry instead.
  uint32_t push_mask = (core_spill_mask_ & (~(1 << PC))) | 1 << LR;
  __ PushList(push_mask);
  __ cfi().AdjustCFAOffset(kArmWordSize * POPCOUNT(push_mask));
  __ cfi().RelOffsetForMany(DWARFReg(R0), 0, push_mask, kArmWordSize);
  if (fpu_spill_mask_ != 0) {
    SRegister start_register = SRegister(LeastSignificantBit(fpu_spill_mask_));
    __ vpushs(start_register, POPCOUNT(fpu_spill_mask_));
    __ cfi().AdjustCFAOffset(kArmWordSize * POPCOUNT(fpu_spill_mask_));
    __ cfi().RelOffsetForMany(DWARFReg(S0), 0, fpu_spill_mask_, kArmWordSize);
  }
  int adjust = GetFrameSize() - FrameEntrySpillSize();
  __ AddConstant(SP, -adjust);
  __ cfi().AdjustCFAOffset(adjust);
  __ StoreToOffset(kStoreWord, R0, SP, 0);
}

void CodeGeneratorARM::GenerateFrameExit() {
  if (HasEmptyFrame()) {
    __ bx(LR);
    return;
  }
  __ cfi().RememberState();
  int adjust = GetFrameSize() - FrameEntrySpillSize();
  __ AddConstant(SP, adjust);
  __ cfi().AdjustCFAOffset(-adjust);
  if (fpu_spill_mask_ != 0) {
    SRegister start_register = SRegister(LeastSignificantBit(fpu_spill_mask_));
    __ vpops(start_register, POPCOUNT(fpu_spill_mask_));
    __ cfi().AdjustCFAOffset(-kArmPointerSize * POPCOUNT(fpu_spill_mask_));
    __ cfi().RestoreMany(DWARFReg(SRegister(0)), fpu_spill_mask_);
  }
  __ PopList(core_spill_mask_);
  __ cfi().RestoreState();
  __ cfi().DefCFAOffset(GetFrameSize());
}

void CodeGeneratorARM::Bind(HBasicBlock* block) {
  __ Bind(GetLabelOf(block));
}

Location CodeGeneratorARM::GetStackLocation(HLoadLocal* load) const {
  switch (load->GetType()) {
    case Primitive::kPrimLong:
    case Primitive::kPrimDouble:
      return Location::DoubleStackSlot(GetStackSlot(load->GetLocal()));

    case Primitive::kPrimInt:
    case Primitive::kPrimNot:
    case Primitive::kPrimFloat:
      return Location::StackSlot(GetStackSlot(load->GetLocal()));

    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unexpected type " << load->GetType();
      UNREACHABLE();
  }

  LOG(FATAL) << "Unreachable";
  UNREACHABLE();
}

Location InvokeDexCallingConventionVisitorARM::GetNextLocation(Primitive::Type type) {
  switch (type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      uint32_t index = gp_index_++;
      uint32_t stack_index = stack_index_++;
      if (index < calling_convention.GetNumberOfRegisters()) {
        return Location::RegisterLocation(calling_convention.GetRegisterAt(index));
      } else {
        return Location::StackSlot(calling_convention.GetStackOffsetOf(stack_index));
      }
    }

    case Primitive::kPrimLong: {
      uint32_t index = gp_index_;
      uint32_t stack_index = stack_index_;
      gp_index_ += 2;
      stack_index_ += 2;
      if (index + 1 < calling_convention.GetNumberOfRegisters()) {
        if (calling_convention.GetRegisterAt(index) == R1) {
          // Skip R1, and use R2_R3 instead.
          gp_index_++;
          index++;
        }
      }
      if (index + 1 < calling_convention.GetNumberOfRegisters()) {
        DCHECK_EQ(calling_convention.GetRegisterAt(index) + 1,
                  calling_convention.GetRegisterAt(index + 1));
        return Location::RegisterPairLocation(calling_convention.GetRegisterAt(index),
                                              calling_convention.GetRegisterAt(index + 1));
      } else {
        return Location::DoubleStackSlot(calling_convention.GetStackOffsetOf(stack_index));
      }
    }

    case Primitive::kPrimFloat: {
      uint32_t stack_index = stack_index_++;
      if (float_index_ % 2 == 0) {
        float_index_ = std::max(double_index_, float_index_);
      }
      if (float_index_ < calling_convention.GetNumberOfFpuRegisters()) {
        return Location::FpuRegisterLocation(calling_convention.GetFpuRegisterAt(float_index_++));
      } else {
        return Location::StackSlot(calling_convention.GetStackOffsetOf(stack_index));
      }
    }

    case Primitive::kPrimDouble: {
      double_index_ = std::max(double_index_, RoundUp(float_index_, 2));
      uint32_t stack_index = stack_index_;
      stack_index_ += 2;
      if (double_index_ + 1 < calling_convention.GetNumberOfFpuRegisters()) {
        uint32_t index = double_index_;
        double_index_ += 2;
        Location result = Location::FpuRegisterPairLocation(
          calling_convention.GetFpuRegisterAt(index),
          calling_convention.GetFpuRegisterAt(index + 1));
        DCHECK(ExpectedPairLayout(result));
        return result;
      } else {
        return Location::DoubleStackSlot(calling_convention.GetStackOffsetOf(stack_index));
      }
    }

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unexpected parameter type " << type;
      break;
  }
  return Location();
}

Location InvokeDexCallingConventionVisitorARM::GetReturnLocation(Primitive::Type type) {
  switch (type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      return Location::RegisterLocation(R0);
    }

    case Primitive::kPrimFloat: {
      return Location::FpuRegisterLocation(S0);
    }

    case Primitive::kPrimLong: {
      return Location::RegisterPairLocation(R0, R1);
    }

    case Primitive::kPrimDouble: {
      return Location::FpuRegisterPairLocation(S0, S1);
    }

    case Primitive::kPrimVoid:
      return Location();
  }
  UNREACHABLE();
}

void CodeGeneratorARM::Move32(Location destination, Location source) {
  if (source.Equals(destination)) {
    return;
  }
  if (destination.IsRegister()) {
    if (source.IsRegister()) {
      __ Mov(destination.AsRegister<Register>(), source.AsRegister<Register>());
    } else if (source.IsFpuRegister()) {
      __ vmovrs(destination.AsRegister<Register>(), source.AsFpuRegister<SRegister>());
    } else {
      __ LoadFromOffset(kLoadWord, destination.AsRegister<Register>(), SP, source.GetStackIndex());
    }
  } else if (destination.IsFpuRegister()) {
    if (source.IsRegister()) {
      __ vmovsr(destination.AsFpuRegister<SRegister>(), source.AsRegister<Register>());
    } else if (source.IsFpuRegister()) {
      __ vmovs(destination.AsFpuRegister<SRegister>(), source.AsFpuRegister<SRegister>());
    } else {
      __ LoadSFromOffset(destination.AsFpuRegister<SRegister>(), SP, source.GetStackIndex());
    }
  } else {
    DCHECK(destination.IsStackSlot()) << destination;
    if (source.IsRegister()) {
      __ StoreToOffset(kStoreWord, source.AsRegister<Register>(), SP, destination.GetStackIndex());
    } else if (source.IsFpuRegister()) {
      __ StoreSToOffset(source.AsFpuRegister<SRegister>(), SP, destination.GetStackIndex());
    } else {
      DCHECK(source.IsStackSlot()) << source;
      __ LoadFromOffset(kLoadWord, IP, SP, source.GetStackIndex());
      __ StoreToOffset(kStoreWord, IP, SP, destination.GetStackIndex());
    }
  }
}

void CodeGeneratorARM::Move64(Location destination, Location source) {
  if (source.Equals(destination)) {
    return;
  }
  if (destination.IsRegisterPair()) {
    if (source.IsRegisterPair()) {
      EmitParallelMoves(
          Location::RegisterLocation(source.AsRegisterPairHigh<Register>()),
          Location::RegisterLocation(destination.AsRegisterPairHigh<Register>()),
          Primitive::kPrimInt,
          Location::RegisterLocation(source.AsRegisterPairLow<Register>()),
          Location::RegisterLocation(destination.AsRegisterPairLow<Register>()),
          Primitive::kPrimInt);
    } else if (source.IsFpuRegister()) {
      UNIMPLEMENTED(FATAL);
    } else {
      DCHECK(source.IsDoubleStackSlot());
      DCHECK(ExpectedPairLayout(destination));
      __ LoadFromOffset(kLoadWordPair, destination.AsRegisterPairLow<Register>(),
                        SP, source.GetStackIndex());
    }
  } else if (destination.IsFpuRegisterPair()) {
    if (source.IsDoubleStackSlot()) {
      __ LoadDFromOffset(FromLowSToD(destination.AsFpuRegisterPairLow<SRegister>()),
                         SP,
                         source.GetStackIndex());
    } else {
      UNIMPLEMENTED(FATAL);
    }
  } else {
    DCHECK(destination.IsDoubleStackSlot());
    if (source.IsRegisterPair()) {
      // No conflict possible, so just do the moves.
      if (source.AsRegisterPairLow<Register>() == R1) {
        DCHECK_EQ(source.AsRegisterPairHigh<Register>(), R2);
        __ StoreToOffset(kStoreWord, R1, SP, destination.GetStackIndex());
        __ StoreToOffset(kStoreWord, R2, SP, destination.GetHighStackIndex(kArmWordSize));
      } else {
        __ StoreToOffset(kStoreWordPair, source.AsRegisterPairLow<Register>(),
                         SP, destination.GetStackIndex());
      }
    } else if (source.IsFpuRegisterPair()) {
      __ StoreDToOffset(FromLowSToD(source.AsFpuRegisterPairLow<SRegister>()),
                        SP,
                        destination.GetStackIndex());
    } else {
      DCHECK(source.IsDoubleStackSlot());
      EmitParallelMoves(
          Location::StackSlot(source.GetStackIndex()),
          Location::StackSlot(destination.GetStackIndex()),
          Primitive::kPrimInt,
          Location::StackSlot(source.GetHighStackIndex(kArmWordSize)),
          Location::StackSlot(destination.GetHighStackIndex(kArmWordSize)),
          Primitive::kPrimInt);
    }
  }
}

void CodeGeneratorARM::Move(HInstruction* instruction, Location location, HInstruction* move_for) {
  LocationSummary* locations = instruction->GetLocations();
  if (locations != nullptr && locations->Out().Equals(location)) {
    return;
  }

  if (locations != nullptr && locations->Out().IsConstant()) {
    HConstant* const_to_move = locations->Out().GetConstant();
    if (const_to_move->IsIntConstant() || const_to_move->IsNullConstant()) {
      int32_t value = GetInt32ValueOf(const_to_move);
      if (location.IsRegister()) {
        __ LoadImmediate(location.AsRegister<Register>(), value);
      } else {
        DCHECK(location.IsStackSlot());
        __ LoadImmediate(IP, value);
        __ StoreToOffset(kStoreWord, IP, SP, location.GetStackIndex());
      }
    } else {
      DCHECK(const_to_move->IsLongConstant()) << const_to_move->DebugName();
      int64_t value = const_to_move->AsLongConstant()->GetValue();
      if (location.IsRegisterPair()) {
        __ LoadImmediate(location.AsRegisterPairLow<Register>(), Low32Bits(value));
        __ LoadImmediate(location.AsRegisterPairHigh<Register>(), High32Bits(value));
      } else {
        DCHECK(location.IsDoubleStackSlot());
        __ LoadImmediate(IP, Low32Bits(value));
        __ StoreToOffset(kStoreWord, IP, SP, location.GetStackIndex());
        __ LoadImmediate(IP, High32Bits(value));
        __ StoreToOffset(kStoreWord, IP, SP, location.GetHighStackIndex(kArmWordSize));
      }
    }
  } else if (instruction->IsLoadLocal()) {
    uint32_t stack_slot = GetStackSlot(instruction->AsLoadLocal()->GetLocal());
    switch (instruction->GetType()) {
      case Primitive::kPrimBoolean:
      case Primitive::kPrimByte:
      case Primitive::kPrimChar:
      case Primitive::kPrimShort:
      case Primitive::kPrimInt:
      case Primitive::kPrimNot:
      case Primitive::kPrimFloat:
        Move32(location, Location::StackSlot(stack_slot));
        break;

      case Primitive::kPrimLong:
      case Primitive::kPrimDouble:
        Move64(location, Location::DoubleStackSlot(stack_slot));
        break;

      default:
        LOG(FATAL) << "Unexpected type " << instruction->GetType();
    }
  } else if (instruction->IsTemporary()) {
    Location temp_location = GetTemporaryLocation(instruction->AsTemporary());
    if (temp_location.IsStackSlot()) {
      Move32(location, temp_location);
    } else {
      DCHECK(temp_location.IsDoubleStackSlot());
      Move64(location, temp_location);
    }
  } else {
    DCHECK((instruction->GetNext() == move_for) || instruction->GetNext()->IsTemporary());
    switch (instruction->GetType()) {
      case Primitive::kPrimBoolean:
      case Primitive::kPrimByte:
      case Primitive::kPrimChar:
      case Primitive::kPrimShort:
      case Primitive::kPrimNot:
      case Primitive::kPrimInt:
      case Primitive::kPrimFloat:
        Move32(location, locations->Out());
        break;

      case Primitive::kPrimLong:
      case Primitive::kPrimDouble:
        Move64(location, locations->Out());
        break;

      default:
        LOG(FATAL) << "Unexpected type " << instruction->GetType();
    }
  }
}

void CodeGeneratorARM::InvokeRuntime(int32_t entry_point_offset,
                                     HInstruction* instruction,
                                     uint32_t dex_pc,
                                     SlowPathCode* slow_path) {
  __ LoadFromOffset(kLoadWord, LR, TR, entry_point_offset);
  __ blx(LR);
  RecordPcInfo(instruction, dex_pc, slow_path);
  DCHECK(instruction->IsSuspendCheck()
      || instruction->IsBoundsCheck()
      || instruction->IsNullCheck()
      || instruction->IsDivZeroCheck()
      || instruction->GetLocations()->CanCall()
      || !IsLeafMethod());
}

void LocationsBuilderARM::VisitGoto(HGoto* got) {
  got->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM::VisitGoto(HGoto* got) {
  HBasicBlock* successor = got->GetSuccessor();
  DCHECK(!successor->IsExitBlock());

  HBasicBlock* block = got->GetBlock();
  HInstruction* previous = got->GetPrevious();

  HLoopInformation* info = block->GetLoopInformation();
  if (info != nullptr && info->IsBackEdge(*block) && info->HasSuspendCheck()) {
    codegen_->ClearSpillSlotsFromLoopPhisInStackMap(info->GetSuspendCheck());
    GenerateSuspendCheck(info->GetSuspendCheck(), successor);
    return;
  }

  if (block->IsEntryBlock() && (previous != nullptr) && previous->IsSuspendCheck()) {
    GenerateSuspendCheck(previous->AsSuspendCheck(), nullptr);
  }
  if (!codegen_->GoesToNextBlock(got->GetBlock(), successor)) {
    __ b(codegen_->GetLabelOf(successor));
  }
}

void LocationsBuilderARM::VisitExit(HExit* exit) {
  exit->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM::VisitExit(HExit* exit) {
  UNUSED(exit);
}

void InstructionCodeGeneratorARM::GenerateTestAndBranch(HInstruction* instruction,
                                                        Label* true_target,
                                                        Label* false_target,
                                                        Label* always_true_target) {
  HInstruction* cond = instruction->InputAt(0);
  if (cond->IsIntConstant()) {
    // Constant condition, statically compared against 1.
    int32_t cond_value = cond->AsIntConstant()->GetValue();
    if (cond_value == 1) {
      if (always_true_target != nullptr) {
        __ b(always_true_target);
      }
      return;
    } else {
      DCHECK_EQ(cond_value, 0);
    }
  } else {
    if (!cond->IsCondition() || cond->AsCondition()->NeedsMaterialization()) {
      // Condition has been materialized, compare the output to 0
      DCHECK(instruction->GetLocations()->InAt(0).IsRegister());
      __ cmp(instruction->GetLocations()->InAt(0).AsRegister<Register>(),
             ShifterOperand(0));
      __ b(true_target, NE);
    } else {
      // Condition has not been materialized, use its inputs as the
      // comparison and its condition as the branch condition.
      LocationSummary* locations = cond->GetLocations();
      DCHECK(locations->InAt(0).IsRegister()) << locations->InAt(0);
      Register left = locations->InAt(0).AsRegister<Register>();
      if (locations->InAt(1).IsRegister()) {
        __ cmp(left, ShifterOperand(locations->InAt(1).AsRegister<Register>()));
      } else {
        DCHECK(locations->InAt(1).IsConstant());
        HConstant* constant = locations->InAt(1).GetConstant();
        int32_t value = CodeGenerator::GetInt32ValueOf(constant);
        ShifterOperand operand;
        if (GetAssembler()->ShifterOperandCanHold(R0, left, CMP, value, &operand)) {
          __ cmp(left, operand);
        } else {
          Register temp = IP;
          __ LoadImmediate(temp, value);
          __ cmp(left, ShifterOperand(temp));
        }
      }
      __ b(true_target, ARMCondition(cond->AsCondition()->GetCondition()));
    }
  }
  if (false_target != nullptr) {
    __ b(false_target);
  }
}

void LocationsBuilderARM::VisitIf(HIf* if_instr) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(if_instr, LocationSummary::kNoCall);
  HInstruction* cond = if_instr->InputAt(0);
  if (!cond->IsCondition() || cond->AsCondition()->NeedsMaterialization()) {
    locations->SetInAt(0, Location::RequiresRegister());
  }
}

void InstructionCodeGeneratorARM::VisitIf(HIf* if_instr) {
  Label* true_target = codegen_->GetLabelOf(if_instr->IfTrueSuccessor());
  Label* false_target = codegen_->GetLabelOf(if_instr->IfFalseSuccessor());
  Label* always_true_target = true_target;
  if (codegen_->GoesToNextBlock(if_instr->GetBlock(),
                                if_instr->IfTrueSuccessor())) {
    always_true_target = nullptr;
  }
  if (codegen_->GoesToNextBlock(if_instr->GetBlock(),
                                if_instr->IfFalseSuccessor())) {
    false_target = nullptr;
  }
  GenerateTestAndBranch(if_instr, true_target, false_target, always_true_target);
}

void LocationsBuilderARM::VisitDeoptimize(HDeoptimize* deoptimize) {
  LocationSummary* locations = new (GetGraph()->GetArena())
      LocationSummary(deoptimize, LocationSummary::kCallOnSlowPath);
  HInstruction* cond = deoptimize->InputAt(0);
  DCHECK(cond->IsCondition());
  if (cond->AsCondition()->NeedsMaterialization()) {
    locations->SetInAt(0, Location::RequiresRegister());
  }
}

void InstructionCodeGeneratorARM::VisitDeoptimize(HDeoptimize* deoptimize) {
  SlowPathCodeARM* slow_path = new (GetGraph()->GetArena())
      DeoptimizationSlowPathARM(deoptimize);
  codegen_->AddSlowPath(slow_path);
  Label* slow_path_entry = slow_path->GetEntryLabel();
  GenerateTestAndBranch(deoptimize, slow_path_entry, nullptr, slow_path_entry);
}

void LocationsBuilderARM::VisitCondition(HCondition* comp) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(comp, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RegisterOrConstant(comp->InputAt(1)));
  if (comp->NeedsMaterialization()) {
    locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
  }
}

void InstructionCodeGeneratorARM::VisitCondition(HCondition* comp) {
  if (!comp->NeedsMaterialization()) return;
  LocationSummary* locations = comp->GetLocations();
  Register left = locations->InAt(0).AsRegister<Register>();

  if (locations->InAt(1).IsRegister()) {
    __ cmp(left, ShifterOperand(locations->InAt(1).AsRegister<Register>()));
  } else {
    DCHECK(locations->InAt(1).IsConstant());
    int32_t value = CodeGenerator::GetInt32ValueOf(locations->InAt(1).GetConstant());
    ShifterOperand operand;
    if (GetAssembler()->ShifterOperandCanHold(R0, left, CMP, value, &operand)) {
      __ cmp(left, operand);
    } else {
      Register temp = IP;
      __ LoadImmediate(temp, value);
      __ cmp(left, ShifterOperand(temp));
    }
  }
  __ it(ARMCondition(comp->GetCondition()), kItElse);
  __ mov(locations->Out().AsRegister<Register>(), ShifterOperand(1),
         ARMCondition(comp->GetCondition()));
  __ mov(locations->Out().AsRegister<Register>(), ShifterOperand(0),
         ARMOppositeCondition(comp->GetCondition()));
}

void LocationsBuilderARM::VisitEqual(HEqual* comp) {
  VisitCondition(comp);
}

void InstructionCodeGeneratorARM::VisitEqual(HEqual* comp) {
  VisitCondition(comp);
}

void LocationsBuilderARM::VisitNotEqual(HNotEqual* comp) {
  VisitCondition(comp);
}

void InstructionCodeGeneratorARM::VisitNotEqual(HNotEqual* comp) {
  VisitCondition(comp);
}

void LocationsBuilderARM::VisitLessThan(HLessThan* comp) {
  VisitCondition(comp);
}

void InstructionCodeGeneratorARM::VisitLessThan(HLessThan* comp) {
  VisitCondition(comp);
}

void LocationsBuilderARM::VisitLessThanOrEqual(HLessThanOrEqual* comp) {
  VisitCondition(comp);
}

void InstructionCodeGeneratorARM::VisitLessThanOrEqual(HLessThanOrEqual* comp) {
  VisitCondition(comp);
}

void LocationsBuilderARM::VisitGreaterThan(HGreaterThan* comp) {
  VisitCondition(comp);
}

void InstructionCodeGeneratorARM::VisitGreaterThan(HGreaterThan* comp) {
  VisitCondition(comp);
}

void LocationsBuilderARM::VisitGreaterThanOrEqual(HGreaterThanOrEqual* comp) {
  VisitCondition(comp);
}

void InstructionCodeGeneratorARM::VisitGreaterThanOrEqual(HGreaterThanOrEqual* comp) {
  VisitCondition(comp);
}

void LocationsBuilderARM::VisitLocal(HLocal* local) {
  local->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM::VisitLocal(HLocal* local) {
  DCHECK_EQ(local->GetBlock(), GetGraph()->GetEntryBlock());
}

void LocationsBuilderARM::VisitLoadLocal(HLoadLocal* load) {
  load->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM::VisitLoadLocal(HLoadLocal* load) {
  // Nothing to do, this is driven by the code generator.
  UNUSED(load);
}

void LocationsBuilderARM::VisitStoreLocal(HStoreLocal* store) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(store, LocationSummary::kNoCall);
  switch (store->InputAt(1)->GetType()) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimInt:
    case Primitive::kPrimNot:
    case Primitive::kPrimFloat:
      locations->SetInAt(1, Location::StackSlot(codegen_->GetStackSlot(store->GetLocal())));
      break;

    case Primitive::kPrimLong:
    case Primitive::kPrimDouble:
      locations->SetInAt(1, Location::DoubleStackSlot(codegen_->GetStackSlot(store->GetLocal())));
      break;

    default:
      LOG(FATAL) << "Unexpected local type " << store->InputAt(1)->GetType();
  }
}

void InstructionCodeGeneratorARM::VisitStoreLocal(HStoreLocal* store) {
  UNUSED(store);
}

void LocationsBuilderARM::VisitIntConstant(HIntConstant* constant) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(constant, LocationSummary::kNoCall);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorARM::VisitIntConstant(HIntConstant* constant) {
  // Will be generated at use site.
  UNUSED(constant);
}

void LocationsBuilderARM::VisitNullConstant(HNullConstant* constant) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(constant, LocationSummary::kNoCall);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorARM::VisitNullConstant(HNullConstant* constant) {
  // Will be generated at use site.
  UNUSED(constant);
}

void LocationsBuilderARM::VisitLongConstant(HLongConstant* constant) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(constant, LocationSummary::kNoCall);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorARM::VisitLongConstant(HLongConstant* constant) {
  // Will be generated at use site.
  UNUSED(constant);
}

void LocationsBuilderARM::VisitFloatConstant(HFloatConstant* constant) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(constant, LocationSummary::kNoCall);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorARM::VisitFloatConstant(HFloatConstant* constant) {
  // Will be generated at use site.
  UNUSED(constant);
}

void LocationsBuilderARM::VisitDoubleConstant(HDoubleConstant* constant) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(constant, LocationSummary::kNoCall);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorARM::VisitDoubleConstant(HDoubleConstant* constant) {
  // Will be generated at use site.
  UNUSED(constant);
}

void LocationsBuilderARM::VisitMemoryBarrier(HMemoryBarrier* memory_barrier) {
  memory_barrier->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM::VisitMemoryBarrier(HMemoryBarrier* memory_barrier) {
  GenerateMemoryBarrier(memory_barrier->GetBarrierKind());
}

void LocationsBuilderARM::VisitReturnVoid(HReturnVoid* ret) {
  ret->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM::VisitReturnVoid(HReturnVoid* ret) {
  UNUSED(ret);
  codegen_->GenerateFrameExit();
}

void LocationsBuilderARM::VisitReturn(HReturn* ret) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(ret, LocationSummary::kNoCall);
  locations->SetInAt(0, parameter_visitor_.GetReturnLocation(ret->InputAt(0)->GetType()));
}

void InstructionCodeGeneratorARM::VisitReturn(HReturn* ret) {
  UNUSED(ret);
  codegen_->GenerateFrameExit();
}

void LocationsBuilderARM::VisitInvokeStaticOrDirect(HInvokeStaticOrDirect* invoke) {
  // When we do not run baseline, explicit clinit checks triggered by static
  // invokes must have been pruned by art::PrepareForRegisterAllocation.
  DCHECK(codegen_->IsBaseline() || !invoke->IsStaticWithExplicitClinitCheck());

  IntrinsicLocationsBuilderARM intrinsic(GetGraph()->GetArena(),
                                         codegen_->GetInstructionSetFeatures());
  if (intrinsic.TryDispatch(invoke)) {
    return;
  }

  HandleInvoke(invoke);
}

void CodeGeneratorARM::LoadCurrentMethod(Register reg) {
  DCHECK(RequiresCurrentMethod());
  __ LoadFromOffset(kLoadWord, reg, SP, kCurrentMethodStackOffset);
}

static bool TryGenerateIntrinsicCode(HInvoke* invoke, CodeGeneratorARM* codegen) {
  if (invoke->GetLocations()->Intrinsified()) {
    IntrinsicCodeGeneratorARM intrinsic(codegen);
    intrinsic.Dispatch(invoke);
    return true;
  }
  return false;
}

void InstructionCodeGeneratorARM::VisitInvokeStaticOrDirect(HInvokeStaticOrDirect* invoke) {
  // When we do not run baseline, explicit clinit checks triggered by static
  // invokes must have been pruned by art::PrepareForRegisterAllocation.
  DCHECK(codegen_->IsBaseline() || !invoke->IsStaticWithExplicitClinitCheck());

  if (TryGenerateIntrinsicCode(invoke, codegen_)) {
    return;
  }

  Register temp = invoke->GetLocations()->GetTemp(0).AsRegister<Register>();

  codegen_->GenerateStaticOrDirectCall(invoke, temp);
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
}

void LocationsBuilderARM::HandleInvoke(HInvoke* invoke) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(invoke, LocationSummary::kCall);
  locations->AddTemp(Location::RegisterLocation(R0));

  InvokeDexCallingConventionVisitorARM calling_convention_visitor;
  for (size_t i = 0; i < invoke->GetNumberOfArguments(); i++) {
    HInstruction* input = invoke->InputAt(i);
    locations->SetInAt(i, calling_convention_visitor.GetNextLocation(input->GetType()));
  }

  locations->SetOut(calling_convention_visitor.GetReturnLocation(invoke->GetType()));
}

void LocationsBuilderARM::VisitInvokeVirtual(HInvokeVirtual* invoke) {
  IntrinsicLocationsBuilderARM intrinsic(GetGraph()->GetArena(),
                                         codegen_->GetInstructionSetFeatures());
  if (intrinsic.TryDispatch(invoke)) {
    return;
  }

  HandleInvoke(invoke);
}

void InstructionCodeGeneratorARM::VisitInvokeVirtual(HInvokeVirtual* invoke) {
  if (TryGenerateIntrinsicCode(invoke, codegen_)) {
    return;
  }

  Register temp = invoke->GetLocations()->GetTemp(0).AsRegister<Register>();
  uint32_t method_offset = mirror::Class::EmbeddedVTableEntryOffset(
      invoke->GetVTableIndex(), kArmPointerSize).Uint32Value();
  LocationSummary* locations = invoke->GetLocations();
  Location receiver = locations->InAt(0);
  uint32_t class_offset = mirror::Object::ClassOffset().Int32Value();
  // temp = object->GetClass();
  if (receiver.IsStackSlot()) {
    __ LoadFromOffset(kLoadWord, temp, SP, receiver.GetStackIndex());
    __ LoadFromOffset(kLoadWord, temp, temp, class_offset);
  } else {
    __ LoadFromOffset(kLoadWord, temp, receiver.AsRegister<Register>(), class_offset);
  }
  codegen_->MaybeRecordImplicitNullCheck(invoke);
  // temp = temp->GetMethodAt(method_offset);
  uint32_t entry_point = ArtMethod::EntryPointFromQuickCompiledCodeOffset(
      kArmWordSize).Int32Value();
  __ LoadFromOffset(kLoadWord, temp, temp, method_offset);
  // LR = temp->GetEntryPoint();
  __ LoadFromOffset(kLoadWord, LR, temp, entry_point);
  // LR();
  __ blx(LR);
  DCHECK(!codegen_->IsLeafMethod());
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
}

void LocationsBuilderARM::VisitInvokeInterface(HInvokeInterface* invoke) {
  HandleInvoke(invoke);
  // Add the hidden argument.
  invoke->GetLocations()->AddTemp(Location::RegisterLocation(R12));
}

void InstructionCodeGeneratorARM::VisitInvokeInterface(HInvokeInterface* invoke) {
  // TODO: b/18116999, our IMTs can miss an IncompatibleClassChangeError.
  Register temp = invoke->GetLocations()->GetTemp(0).AsRegister<Register>();
  uint32_t method_offset = mirror::Class::EmbeddedImTableEntryOffset(
      invoke->GetImtIndex() % mirror::Class::kImtSize, kArmPointerSize).Uint32Value();
  LocationSummary* locations = invoke->GetLocations();
  Location receiver = locations->InAt(0);
  uint32_t class_offset = mirror::Object::ClassOffset().Int32Value();

  // Set the hidden argument.
  __ LoadImmediate(invoke->GetLocations()->GetTemp(1).AsRegister<Register>(),
                   invoke->GetDexMethodIndex());

  // temp = object->GetClass();
  if (receiver.IsStackSlot()) {
    __ LoadFromOffset(kLoadWord, temp, SP, receiver.GetStackIndex());
    __ LoadFromOffset(kLoadWord, temp, temp, class_offset);
  } else {
    __ LoadFromOffset(kLoadWord, temp, receiver.AsRegister<Register>(), class_offset);
  }
  codegen_->MaybeRecordImplicitNullCheck(invoke);
  // temp = temp->GetImtEntryAt(method_offset);
  uint32_t entry_point = ArtMethod::EntryPointFromQuickCompiledCodeOffset(
      kArmWordSize).Int32Value();
  __ LoadFromOffset(kLoadWord, temp, temp, method_offset);
  // LR = temp->GetEntryPoint();
  __ LoadFromOffset(kLoadWord, LR, temp, entry_point);
  // LR();
  __ blx(LR);
  DCHECK(!codegen_->IsLeafMethod());
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
}

void LocationsBuilderARM::VisitNeg(HNeg* neg) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(neg, LocationSummary::kNoCall);
  switch (neg->GetResultType()) {
    case Primitive::kPrimInt: {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      break;
    }
    case Primitive::kPrimLong: {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetOut(Location::RequiresRegister(), Location::kOutputOverlap);
      break;
    }

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetOut(Location::RequiresFpuRegister(), Location::kNoOutputOverlap);
      break;

    default:
      LOG(FATAL) << "Unexpected neg type " << neg->GetResultType();
  }
}

void InstructionCodeGeneratorARM::VisitNeg(HNeg* neg) {
  LocationSummary* locations = neg->GetLocations();
  Location out = locations->Out();
  Location in = locations->InAt(0);
  switch (neg->GetResultType()) {
    case Primitive::kPrimInt:
      DCHECK(in.IsRegister());
      __ rsb(out.AsRegister<Register>(), in.AsRegister<Register>(), ShifterOperand(0));
      break;

    case Primitive::kPrimLong:
      DCHECK(in.IsRegisterPair());
      // out.lo = 0 - in.lo (and update the carry/borrow (C) flag)
      __ rsbs(out.AsRegisterPairLow<Register>(),
              in.AsRegisterPairLow<Register>(),
              ShifterOperand(0));
      // We cannot emit an RSC (Reverse Subtract with Carry)
      // instruction here, as it does not exist in the Thumb-2
      // instruction set.  We use the following approach
      // using SBC and SUB instead.
      //
      // out.hi = -C
      __ sbc(out.AsRegisterPairHigh<Register>(),
             out.AsRegisterPairHigh<Register>(),
             ShifterOperand(out.AsRegisterPairHigh<Register>()));
      // out.hi = out.hi - in.hi
      __ sub(out.AsRegisterPairHigh<Register>(),
             out.AsRegisterPairHigh<Register>(),
             ShifterOperand(in.AsRegisterPairHigh<Register>()));
      break;

    case Primitive::kPrimFloat:
      DCHECK(in.IsFpuRegister());
      __ vnegs(out.AsFpuRegister<SRegister>(), in.AsFpuRegister<SRegister>());
      break;

    case Primitive::kPrimDouble:
      DCHECK(in.IsFpuRegisterPair());
      __ vnegd(FromLowSToD(out.AsFpuRegisterPairLow<SRegister>()),
               FromLowSToD(in.AsFpuRegisterPairLow<SRegister>()));
      break;

    default:
      LOG(FATAL) << "Unexpected neg type " << neg->GetResultType();
  }
}

void LocationsBuilderARM::VisitTypeConversion(HTypeConversion* conversion) {
  Primitive::Type result_type = conversion->GetResultType();
  Primitive::Type input_type = conversion->GetInputType();
  DCHECK_NE(result_type, input_type);

  // The float-to-long and double-to-long type conversions rely on a
  // call to the runtime.
  LocationSummary::CallKind call_kind =
      ((input_type == Primitive::kPrimFloat || input_type == Primitive::kPrimDouble)
       && result_type == Primitive::kPrimLong)
      ? LocationSummary::kCall
      : LocationSummary::kNoCall;
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(conversion, call_kind);

  // The Java language does not allow treating boolean as an integral type but
  // our bit representation makes it safe.

  switch (result_type) {
    case Primitive::kPrimByte:
      switch (input_type) {
        case Primitive::kPrimBoolean:
          // Boolean input is a result of code transformations.
        case Primitive::kPrimShort:
        case Primitive::kPrimInt:
        case Primitive::kPrimChar:
          // Processing a Dex `int-to-byte' instruction.
          locations->SetInAt(0, Location::RequiresRegister());
          locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      }
      break;

    case Primitive::kPrimShort:
      switch (input_type) {
        case Primitive::kPrimBoolean:
          // Boolean input is a result of code transformations.
        case Primitive::kPrimByte:
        case Primitive::kPrimInt:
        case Primitive::kPrimChar:
          // Processing a Dex `int-to-short' instruction.
          locations->SetInAt(0, Location::RequiresRegister());
          locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      }
      break;

    case Primitive::kPrimInt:
      switch (input_type) {
        case Primitive::kPrimLong:
          // Processing a Dex `long-to-int' instruction.
          locations->SetInAt(0, Location::Any());
          locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
          break;

        case Primitive::kPrimFloat:
          // Processing a Dex `float-to-int' instruction.
          locations->SetInAt(0, Location::RequiresFpuRegister());
          locations->SetOut(Location::RequiresRegister());
          locations->AddTemp(Location::RequiresFpuRegister());
          break;

        case Primitive::kPrimDouble:
          // Processing a Dex `double-to-int' instruction.
          locations->SetInAt(0, Location::RequiresFpuRegister());
          locations->SetOut(Location::RequiresRegister());
          locations->AddTemp(Location::RequiresFpuRegister());
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      }
      break;

    case Primitive::kPrimLong:
      switch (input_type) {
        case Primitive::kPrimBoolean:
          // Boolean input is a result of code transformations.
        case Primitive::kPrimByte:
        case Primitive::kPrimShort:
        case Primitive::kPrimInt:
        case Primitive::kPrimChar:
          // Processing a Dex `int-to-long' instruction.
          locations->SetInAt(0, Location::RequiresRegister());
          locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
          break;

        case Primitive::kPrimFloat: {
          // Processing a Dex `float-to-long' instruction.
          InvokeRuntimeCallingConvention calling_convention;
          locations->SetInAt(0, Location::FpuRegisterLocation(
              calling_convention.GetFpuRegisterAt(0)));
          locations->SetOut(Location::RegisterPairLocation(R0, R1));
          break;
        }

        case Primitive::kPrimDouble: {
          // Processing a Dex `double-to-long' instruction.
          InvokeRuntimeCallingConvention calling_convention;
          locations->SetInAt(0, Location::FpuRegisterPairLocation(
              calling_convention.GetFpuRegisterAt(0),
              calling_convention.GetFpuRegisterAt(1)));
          locations->SetOut(Location::RegisterPairLocation(R0, R1));
          break;
        }

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      }
      break;

    case Primitive::kPrimChar:
      switch (input_type) {
        case Primitive::kPrimBoolean:
          // Boolean input is a result of code transformations.
        case Primitive::kPrimByte:
        case Primitive::kPrimShort:
        case Primitive::kPrimInt:
          // Processing a Dex `int-to-char' instruction.
          locations->SetInAt(0, Location::RequiresRegister());
          locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      }
      break;

    case Primitive::kPrimFloat:
      switch (input_type) {
        case Primitive::kPrimBoolean:
          // Boolean input is a result of code transformations.
        case Primitive::kPrimByte:
        case Primitive::kPrimShort:
        case Primitive::kPrimInt:
        case Primitive::kPrimChar:
          // Processing a Dex `int-to-float' instruction.
          locations->SetInAt(0, Location::RequiresRegister());
          locations->SetOut(Location::RequiresFpuRegister());
          break;

        case Primitive::kPrimLong:
          // Processing a Dex `long-to-float' instruction.
          locations->SetInAt(0, Location::RequiresRegister());
          locations->SetOut(Location::RequiresFpuRegister());
          locations->AddTemp(Location::RequiresRegister());
          locations->AddTemp(Location::RequiresRegister());
          locations->AddTemp(Location::RequiresFpuRegister());
          locations->AddTemp(Location::RequiresFpuRegister());
          break;

        case Primitive::kPrimDouble:
          // Processing a Dex `double-to-float' instruction.
          locations->SetInAt(0, Location::RequiresFpuRegister());
          locations->SetOut(Location::RequiresFpuRegister(), Location::kNoOutputOverlap);
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      };
      break;

    case Primitive::kPrimDouble:
      switch (input_type) {
        case Primitive::kPrimBoolean:
          // Boolean input is a result of code transformations.
        case Primitive::kPrimByte:
        case Primitive::kPrimShort:
        case Primitive::kPrimInt:
        case Primitive::kPrimChar:
          // Processing a Dex `int-to-double' instruction.
          locations->SetInAt(0, Location::RequiresRegister());
          locations->SetOut(Location::RequiresFpuRegister());
          break;

        case Primitive::kPrimLong:
          // Processing a Dex `long-to-double' instruction.
          locations->SetInAt(0, Location::RequiresRegister());
          locations->SetOut(Location::RequiresFpuRegister());
          locations->AddTemp(Location::RequiresRegister());
          locations->AddTemp(Location::RequiresRegister());
          locations->AddTemp(Location::RequiresFpuRegister());
          break;

        case Primitive::kPrimFloat:
          // Processing a Dex `float-to-double' instruction.
          locations->SetInAt(0, Location::RequiresFpuRegister());
          locations->SetOut(Location::RequiresFpuRegister(), Location::kNoOutputOverlap);
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      };
      break;

    default:
      LOG(FATAL) << "Unexpected type conversion from " << input_type
                 << " to " << result_type;
  }
}

void InstructionCodeGeneratorARM::VisitTypeConversion(HTypeConversion* conversion) {
  LocationSummary* locations = conversion->GetLocations();
  Location out = locations->Out();
  Location in = locations->InAt(0);
  Primitive::Type result_type = conversion->GetResultType();
  Primitive::Type input_type = conversion->GetInputType();
  DCHECK_NE(result_type, input_type);
  switch (result_type) {
    case Primitive::kPrimByte:
      switch (input_type) {
        case Primitive::kPrimBoolean:
          // Boolean input is a result of code transformations.
        case Primitive::kPrimShort:
        case Primitive::kPrimInt:
        case Primitive::kPrimChar:
          // Processing a Dex `int-to-byte' instruction.
          __ sbfx(out.AsRegister<Register>(), in.AsRegister<Register>(), 0, 8);
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      }
      break;

    case Primitive::kPrimShort:
      switch (input_type) {
        case Primitive::kPrimBoolean:
          // Boolean input is a result of code transformations.
        case Primitive::kPrimByte:
        case Primitive::kPrimInt:
        case Primitive::kPrimChar:
          // Processing a Dex `int-to-short' instruction.
          __ sbfx(out.AsRegister<Register>(), in.AsRegister<Register>(), 0, 16);
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      }
      break;

    case Primitive::kPrimInt:
      switch (input_type) {
        case Primitive::kPrimLong:
          // Processing a Dex `long-to-int' instruction.
          DCHECK(out.IsRegister());
          if (in.IsRegisterPair()) {
            __ Mov(out.AsRegister<Register>(), in.AsRegisterPairLow<Register>());
          } else if (in.IsDoubleStackSlot()) {
            __ LoadFromOffset(kLoadWord, out.AsRegister<Register>(), SP, in.GetStackIndex());
          } else {
            DCHECK(in.IsConstant());
            DCHECK(in.GetConstant()->IsLongConstant());
            int64_t value = in.GetConstant()->AsLongConstant()->GetValue();
            __ LoadImmediate(out.AsRegister<Register>(), static_cast<int32_t>(value));
          }
          break;

        case Primitive::kPrimFloat: {
          // Processing a Dex `float-to-int' instruction.
          SRegister temp = locations->GetTemp(0).AsFpuRegisterPairLow<SRegister>();
          __ vmovs(temp, in.AsFpuRegister<SRegister>());
          __ vcvtis(temp, temp);
          __ vmovrs(out.AsRegister<Register>(), temp);
          break;
        }

        case Primitive::kPrimDouble: {
          // Processing a Dex `double-to-int' instruction.
          SRegister temp_s = locations->GetTemp(0).AsFpuRegisterPairLow<SRegister>();
          DRegister temp_d = FromLowSToD(temp_s);
          __ vmovd(temp_d, FromLowSToD(in.AsFpuRegisterPairLow<SRegister>()));
          __ vcvtid(temp_s, temp_d);
          __ vmovrs(out.AsRegister<Register>(), temp_s);
          break;
        }

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      }
      break;

    case Primitive::kPrimLong:
      switch (input_type) {
        case Primitive::kPrimBoolean:
          // Boolean input is a result of code transformations.
        case Primitive::kPrimByte:
        case Primitive::kPrimShort:
        case Primitive::kPrimInt:
        case Primitive::kPrimChar:
          // Processing a Dex `int-to-long' instruction.
          DCHECK(out.IsRegisterPair());
          DCHECK(in.IsRegister());
          __ Mov(out.AsRegisterPairLow<Register>(), in.AsRegister<Register>());
          // Sign extension.
          __ Asr(out.AsRegisterPairHigh<Register>(),
                 out.AsRegisterPairLow<Register>(),
                 31);
          break;

        case Primitive::kPrimFloat:
          // Processing a Dex `float-to-long' instruction.
          codegen_->InvokeRuntime(QUICK_ENTRY_POINT(pF2l),
                                  conversion,
                                  conversion->GetDexPc(),
                                  nullptr);
          break;

        case Primitive::kPrimDouble:
          // Processing a Dex `double-to-long' instruction.
          codegen_->InvokeRuntime(QUICK_ENTRY_POINT(pD2l),
                                  conversion,
                                  conversion->GetDexPc(),
                                  nullptr);
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      }
      break;

    case Primitive::kPrimChar:
      switch (input_type) {
        case Primitive::kPrimBoolean:
          // Boolean input is a result of code transformations.
        case Primitive::kPrimByte:
        case Primitive::kPrimShort:
        case Primitive::kPrimInt:
          // Processing a Dex `int-to-char' instruction.
          __ ubfx(out.AsRegister<Register>(), in.AsRegister<Register>(), 0, 16);
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      }
      break;

    case Primitive::kPrimFloat:
      switch (input_type) {
        case Primitive::kPrimBoolean:
          // Boolean input is a result of code transformations.
        case Primitive::kPrimByte:
        case Primitive::kPrimShort:
        case Primitive::kPrimInt:
        case Primitive::kPrimChar: {
          // Processing a Dex `int-to-float' instruction.
          __ vmovsr(out.AsFpuRegister<SRegister>(), in.AsRegister<Register>());
          __ vcvtsi(out.AsFpuRegister<SRegister>(), out.AsFpuRegister<SRegister>());
          break;
        }

        case Primitive::kPrimLong: {
          // Processing a Dex `long-to-float' instruction.
          Register low = in.AsRegisterPairLow<Register>();
          Register high = in.AsRegisterPairHigh<Register>();
          SRegister output = out.AsFpuRegister<SRegister>();
          Register constant_low = locations->GetTemp(0).AsRegister<Register>();
          Register constant_high = locations->GetTemp(1).AsRegister<Register>();
          SRegister temp1_s = locations->GetTemp(2).AsFpuRegisterPairLow<SRegister>();
          DRegister temp1_d = FromLowSToD(temp1_s);
          SRegister temp2_s = locations->GetTemp(3).AsFpuRegisterPairLow<SRegister>();
          DRegister temp2_d = FromLowSToD(temp2_s);

          // Operations use doubles for precision reasons (each 32-bit
          // half of a long fits in the 53-bit mantissa of a double,
          // but not in the 24-bit mantissa of a float).  This is
          // especially important for the low bits.  The result is
          // eventually converted to float.

          // temp1_d = int-to-double(high)
          __ vmovsr(temp1_s, high);
          __ vcvtdi(temp1_d, temp1_s);
          // Using vmovd to load the `k2Pow32EncodingForDouble` constant
          // as an immediate value into `temp2_d` does not work, as
          // this instruction only transfers 8 significant bits of its
          // immediate operand.  Instead, use two 32-bit core
          // registers to load `k2Pow32EncodingForDouble` into
          // `temp2_d`.
          __ LoadImmediate(constant_low, Low32Bits(k2Pow32EncodingForDouble));
          __ LoadImmediate(constant_high, High32Bits(k2Pow32EncodingForDouble));
          __ vmovdrr(temp2_d, constant_low, constant_high);
          // temp1_d = temp1_d * 2^32
          __ vmuld(temp1_d, temp1_d, temp2_d);
          // temp2_d = unsigned-to-double(low)
          __ vmovsr(temp2_s, low);
          __ vcvtdu(temp2_d, temp2_s);
          // temp1_d = temp1_d + temp2_d
          __ vaddd(temp1_d, temp1_d, temp2_d);
          // output = double-to-float(temp1_d);
          __ vcvtsd(output, temp1_d);
          break;
        }

        case Primitive::kPrimDouble:
          // Processing a Dex `double-to-float' instruction.
          __ vcvtsd(out.AsFpuRegister<SRegister>(),
                    FromLowSToD(in.AsFpuRegisterPairLow<SRegister>()));
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      };
      break;

    case Primitive::kPrimDouble:
      switch (input_type) {
        case Primitive::kPrimBoolean:
          // Boolean input is a result of code transformations.
        case Primitive::kPrimByte:
        case Primitive::kPrimShort:
        case Primitive::kPrimInt:
        case Primitive::kPrimChar: {
          // Processing a Dex `int-to-double' instruction.
          __ vmovsr(out.AsFpuRegisterPairLow<SRegister>(), in.AsRegister<Register>());
          __ vcvtdi(FromLowSToD(out.AsFpuRegisterPairLow<SRegister>()),
                    out.AsFpuRegisterPairLow<SRegister>());
          break;
        }

        case Primitive::kPrimLong: {
          // Processing a Dex `long-to-double' instruction.
          Register low = in.AsRegisterPairLow<Register>();
          Register high = in.AsRegisterPairHigh<Register>();
          SRegister out_s = out.AsFpuRegisterPairLow<SRegister>();
          DRegister out_d = FromLowSToD(out_s);
          Register constant_low = locations->GetTemp(0).AsRegister<Register>();
          Register constant_high = locations->GetTemp(1).AsRegister<Register>();
          SRegister temp_s = locations->GetTemp(2).AsFpuRegisterPairLow<SRegister>();
          DRegister temp_d = FromLowSToD(temp_s);

          // out_d = int-to-double(high)
          __ vmovsr(out_s, high);
          __ vcvtdi(out_d, out_s);
          // Using vmovd to load the `k2Pow32EncodingForDouble` constant
          // as an immediate value into `temp_d` does not work, as
          // this instruction only transfers 8 significant bits of its
          // immediate operand.  Instead, use two 32-bit core
          // registers to load `k2Pow32EncodingForDouble` into `temp_d`.
          __ LoadImmediate(constant_low, Low32Bits(k2Pow32EncodingForDouble));
          __ LoadImmediate(constant_high, High32Bits(k2Pow32EncodingForDouble));
          __ vmovdrr(temp_d, constant_low, constant_high);
          // out_d = out_d * 2^32
          __ vmuld(out_d, out_d, temp_d);
          // temp_d = unsigned-to-double(low)
          __ vmovsr(temp_s, low);
          __ vcvtdu(temp_d, temp_s);
          // out_d = out_d + temp_d
          __ vaddd(out_d, out_d, temp_d);
          break;
        }

        case Primitive::kPrimFloat:
          // Processing a Dex `float-to-double' instruction.
          __ vcvtds(FromLowSToD(out.AsFpuRegisterPairLow<SRegister>()),
                    in.AsFpuRegister<SRegister>());
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      };
      break;

    default:
      LOG(FATAL) << "Unexpected type conversion from " << input_type
                 << " to " << result_type;
  }
}

void LocationsBuilderARM::VisitAdd(HAdd* add) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(add, LocationSummary::kNoCall);
  switch (add->GetResultType()) {
    case Primitive::kPrimInt: {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RegisterOrConstant(add->InputAt(1)));
      locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      break;
    }

    case Primitive::kPrimLong: {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RequiresRegister());
      locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      break;
    }

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetInAt(1, Location::RequiresFpuRegister());
      locations->SetOut(Location::RequiresFpuRegister(), Location::kNoOutputOverlap);
      break;
    }

    default:
      LOG(FATAL) << "Unexpected add type " << add->GetResultType();
  }
}

void InstructionCodeGeneratorARM::VisitAdd(HAdd* add) {
  LocationSummary* locations = add->GetLocations();
  Location out = locations->Out();
  Location first = locations->InAt(0);
  Location second = locations->InAt(1);
  switch (add->GetResultType()) {
    case Primitive::kPrimInt:
      if (second.IsRegister()) {
        __ add(out.AsRegister<Register>(),
               first.AsRegister<Register>(),
               ShifterOperand(second.AsRegister<Register>()));
      } else {
        __ AddConstant(out.AsRegister<Register>(),
                       first.AsRegister<Register>(),
                       second.GetConstant()->AsIntConstant()->GetValue());
      }
      break;

    case Primitive::kPrimLong: {
      DCHECK(second.IsRegisterPair());
      __ adds(out.AsRegisterPairLow<Register>(),
              first.AsRegisterPairLow<Register>(),
              ShifterOperand(second.AsRegisterPairLow<Register>()));
      __ adc(out.AsRegisterPairHigh<Register>(),
             first.AsRegisterPairHigh<Register>(),
             ShifterOperand(second.AsRegisterPairHigh<Register>()));
      break;
    }

    case Primitive::kPrimFloat:
      __ vadds(out.AsFpuRegister<SRegister>(),
               first.AsFpuRegister<SRegister>(),
               second.AsFpuRegister<SRegister>());
      break;

    case Primitive::kPrimDouble:
      __ vaddd(FromLowSToD(out.AsFpuRegisterPairLow<SRegister>()),
               FromLowSToD(first.AsFpuRegisterPairLow<SRegister>()),
               FromLowSToD(second.AsFpuRegisterPairLow<SRegister>()));
      break;

    default:
      LOG(FATAL) << "Unexpected add type " << add->GetResultType();
  }
}

void LocationsBuilderARM::VisitSub(HSub* sub) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(sub, LocationSummary::kNoCall);
  switch (sub->GetResultType()) {
    case Primitive::kPrimInt: {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RegisterOrConstant(sub->InputAt(1)));
      locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      break;
    }

    case Primitive::kPrimLong: {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RequiresRegister());
      locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      break;
    }
    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetInAt(1, Location::RequiresFpuRegister());
      locations->SetOut(Location::RequiresFpuRegister(), Location::kNoOutputOverlap);
      break;
    }
    default:
      LOG(FATAL) << "Unexpected sub type " << sub->GetResultType();
  }
}

void InstructionCodeGeneratorARM::VisitSub(HSub* sub) {
  LocationSummary* locations = sub->GetLocations();
  Location out = locations->Out();
  Location first = locations->InAt(0);
  Location second = locations->InAt(1);
  switch (sub->GetResultType()) {
    case Primitive::kPrimInt: {
      if (second.IsRegister()) {
        __ sub(out.AsRegister<Register>(),
               first.AsRegister<Register>(),
               ShifterOperand(second.AsRegister<Register>()));
      } else {
        __ AddConstant(out.AsRegister<Register>(),
                       first.AsRegister<Register>(),
                       -second.GetConstant()->AsIntConstant()->GetValue());
      }
      break;
    }

    case Primitive::kPrimLong: {
      DCHECK(second.IsRegisterPair());
      __ subs(out.AsRegisterPairLow<Register>(),
              first.AsRegisterPairLow<Register>(),
              ShifterOperand(second.AsRegisterPairLow<Register>()));
      __ sbc(out.AsRegisterPairHigh<Register>(),
             first.AsRegisterPairHigh<Register>(),
             ShifterOperand(second.AsRegisterPairHigh<Register>()));
      break;
    }

    case Primitive::kPrimFloat: {
      __ vsubs(out.AsFpuRegister<SRegister>(),
               first.AsFpuRegister<SRegister>(),
               second.AsFpuRegister<SRegister>());
      break;
    }

    case Primitive::kPrimDouble: {
      __ vsubd(FromLowSToD(out.AsFpuRegisterPairLow<SRegister>()),
               FromLowSToD(first.AsFpuRegisterPairLow<SRegister>()),
               FromLowSToD(second.AsFpuRegisterPairLow<SRegister>()));
      break;
    }


    default:
      LOG(FATAL) << "Unexpected sub type " << sub->GetResultType();
  }
}

void LocationsBuilderARM::VisitMul(HMul* mul) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(mul, LocationSummary::kNoCall);
  switch (mul->GetResultType()) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong:  {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RequiresRegister());
      locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      break;
    }

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetInAt(1, Location::RequiresFpuRegister());
      locations->SetOut(Location::RequiresFpuRegister(), Location::kNoOutputOverlap);
      break;
    }

    default:
      LOG(FATAL) << "Unexpected mul type " << mul->GetResultType();
  }
}

void InstructionCodeGeneratorARM::VisitMul(HMul* mul) {
  LocationSummary* locations = mul->GetLocations();
  Location out = locations->Out();
  Location first = locations->InAt(0);
  Location second = locations->InAt(1);
  switch (mul->GetResultType()) {
    case Primitive::kPrimInt: {
      __ mul(out.AsRegister<Register>(),
             first.AsRegister<Register>(),
             second.AsRegister<Register>());
      break;
    }
    case Primitive::kPrimLong: {
      Register out_hi = out.AsRegisterPairHigh<Register>();
      Register out_lo = out.AsRegisterPairLow<Register>();
      Register in1_hi = first.AsRegisterPairHigh<Register>();
      Register in1_lo = first.AsRegisterPairLow<Register>();
      Register in2_hi = second.AsRegisterPairHigh<Register>();
      Register in2_lo = second.AsRegisterPairLow<Register>();

      // Extra checks to protect caused by the existence of R1_R2.
      // The algorithm is wrong if out.hi is either in1.lo or in2.lo:
      // (e.g. in1=r0_r1, in2=r2_r3 and out=r1_r2);
      DCHECK_NE(out_hi, in1_lo);
      DCHECK_NE(out_hi, in2_lo);

      // input: in1 - 64 bits, in2 - 64 bits
      // output: out
      // formula: out.hi : out.lo = (in1.lo * in2.hi + in1.hi * in2.lo)* 2^32 + in1.lo * in2.lo
      // parts: out.hi = in1.lo * in2.hi + in1.hi * in2.lo + (in1.lo * in2.lo)[63:32]
      // parts: out.lo = (in1.lo * in2.lo)[31:0]

      // IP <- in1.lo * in2.hi
      __ mul(IP, in1_lo, in2_hi);
      // out.hi <- in1.lo * in2.hi + in1.hi * in2.lo
      __ mla(out_hi, in1_hi, in2_lo, IP);
      // out.lo <- (in1.lo * in2.lo)[31:0];
      __ umull(out_lo, IP, in1_lo, in2_lo);
      // out.hi <- in2.hi * in1.lo +  in2.lo * in1.hi + (in1.lo * in2.lo)[63:32]
      __ add(out_hi, out_hi, ShifterOperand(IP));
      break;
    }

    case Primitive::kPrimFloat: {
      __ vmuls(out.AsFpuRegister<SRegister>(),
               first.AsFpuRegister<SRegister>(),
               second.AsFpuRegister<SRegister>());
      break;
    }

    case Primitive::kPrimDouble: {
      __ vmuld(FromLowSToD(out.AsFpuRegisterPairLow<SRegister>()),
               FromLowSToD(first.AsFpuRegisterPairLow<SRegister>()),
               FromLowSToD(second.AsFpuRegisterPairLow<SRegister>()));
      break;
    }

    default:
      LOG(FATAL) << "Unexpected mul type " << mul->GetResultType();
  }
}

void LocationsBuilderARM::VisitDiv(HDiv* div) {
  LocationSummary::CallKind call_kind = LocationSummary::kNoCall;
  if (div->GetResultType() == Primitive::kPrimLong) {
    // pLdiv runtime call.
    call_kind = LocationSummary::kCall;
  } else if (div->GetResultType() == Primitive::kPrimInt &&
             !codegen_->GetInstructionSetFeatures().HasDivideInstruction()) {
    // pIdivmod runtime call.
    call_kind = LocationSummary::kCall;
  }

  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(div, call_kind);

  switch (div->GetResultType()) {
    case Primitive::kPrimInt: {
      if (codegen_->GetInstructionSetFeatures().HasDivideInstruction()) {
        locations->SetInAt(0, Location::RequiresRegister());
        locations->SetInAt(1, Location::RequiresRegister());
        locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      } else {
        InvokeRuntimeCallingConvention calling_convention;
        locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
        locations->SetInAt(1, Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
        // Note: divrem will compute both the quotient and the remainder as the pair R0 and R1, but
        //       we only need the former.
        locations->SetOut(Location::RegisterLocation(R0));
      }
      break;
    }
    case Primitive::kPrimLong: {
      InvokeRuntimeCallingConvention calling_convention;
      locations->SetInAt(0, Location::RegisterPairLocation(
          calling_convention.GetRegisterAt(0), calling_convention.GetRegisterAt(1)));
      locations->SetInAt(1, Location::RegisterPairLocation(
          calling_convention.GetRegisterAt(2), calling_convention.GetRegisterAt(3)));
      locations->SetOut(Location::RegisterPairLocation(R0, R1));
      break;
    }
    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetInAt(1, Location::RequiresFpuRegister());
      locations->SetOut(Location::RequiresFpuRegister(), Location::kNoOutputOverlap);
      break;
    }

    default:
      LOG(FATAL) << "Unexpected div type " << div->GetResultType();
  }
}

void InstructionCodeGeneratorARM::VisitDiv(HDiv* div) {
  LocationSummary* locations = div->GetLocations();
  Location out = locations->Out();
  Location first = locations->InAt(0);
  Location second = locations->InAt(1);

  switch (div->GetResultType()) {
    case Primitive::kPrimInt: {
      if (codegen_->GetInstructionSetFeatures().HasDivideInstruction()) {
        __ sdiv(out.AsRegister<Register>(),
                first.AsRegister<Register>(),
                second.AsRegister<Register>());
      } else {
        InvokeRuntimeCallingConvention calling_convention;
        DCHECK_EQ(calling_convention.GetRegisterAt(0), first.AsRegister<Register>());
        DCHECK_EQ(calling_convention.GetRegisterAt(1), second.AsRegister<Register>());
        DCHECK_EQ(R0, out.AsRegister<Register>());

        codegen_->InvokeRuntime(QUICK_ENTRY_POINT(pIdivmod), div, div->GetDexPc(), nullptr);
      }
      break;
    }

    case Primitive::kPrimLong: {
      InvokeRuntimeCallingConvention calling_convention;
      DCHECK_EQ(calling_convention.GetRegisterAt(0), first.AsRegisterPairLow<Register>());
      DCHECK_EQ(calling_convention.GetRegisterAt(1), first.AsRegisterPairHigh<Register>());
      DCHECK_EQ(calling_convention.GetRegisterAt(2), second.AsRegisterPairLow<Register>());
      DCHECK_EQ(calling_convention.GetRegisterAt(3), second.AsRegisterPairHigh<Register>());
      DCHECK_EQ(R0, out.AsRegisterPairLow<Register>());
      DCHECK_EQ(R1, out.AsRegisterPairHigh<Register>());

      codegen_->InvokeRuntime(QUICK_ENTRY_POINT(pLdiv), div, div->GetDexPc(), nullptr);
      break;
    }

    case Primitive::kPrimFloat: {
      __ vdivs(out.AsFpuRegister<SRegister>(),
               first.AsFpuRegister<SRegister>(),
               second.AsFpuRegister<SRegister>());
      break;
    }

    case Primitive::kPrimDouble: {
      __ vdivd(FromLowSToD(out.AsFpuRegisterPairLow<SRegister>()),
               FromLowSToD(first.AsFpuRegisterPairLow<SRegister>()),
               FromLowSToD(second.AsFpuRegisterPairLow<SRegister>()));
      break;
    }

    default:
      LOG(FATAL) << "Unexpected div type " << div->GetResultType();
  }
}

void LocationsBuilderARM::VisitRem(HRem* rem) {
  Primitive::Type type = rem->GetResultType();

  // Most remainders are implemented in the runtime.
  LocationSummary::CallKind call_kind = LocationSummary::kCall;
  if (rem->GetResultType() == Primitive::kPrimInt &&
      codegen_->GetInstructionSetFeatures().HasDivideInstruction()) {
    // Have hardware divide instruction for int, do it with three instructions.
    call_kind = LocationSummary::kNoCall;
  }

  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(rem, call_kind);

  switch (type) {
    case Primitive::kPrimInt: {
      if (codegen_->GetInstructionSetFeatures().HasDivideInstruction()) {
        locations->SetInAt(0, Location::RequiresRegister());
        locations->SetInAt(1, Location::RequiresRegister());
        locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
        locations->AddTemp(Location::RequiresRegister());
      } else {
        InvokeRuntimeCallingConvention calling_convention;
        locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
        locations->SetInAt(1, Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
        // Note: divrem will compute both the quotient and the remainder as the pair R0 and R1, but
        //       we only need the latter.
        locations->SetOut(Location::RegisterLocation(R1));
      }
      break;
    }
    case Primitive::kPrimLong: {
      InvokeRuntimeCallingConvention calling_convention;
      locations->SetInAt(0, Location::RegisterPairLocation(
          calling_convention.GetRegisterAt(0), calling_convention.GetRegisterAt(1)));
      locations->SetInAt(1, Location::RegisterPairLocation(
          calling_convention.GetRegisterAt(2), calling_convention.GetRegisterAt(3)));
      // The runtime helper puts the output in R2,R3.
      locations->SetOut(Location::RegisterPairLocation(R2, R3));
      break;
    }
    case Primitive::kPrimFloat: {
      InvokeRuntimeCallingConvention calling_convention;
      locations->SetInAt(0, Location::FpuRegisterLocation(calling_convention.GetFpuRegisterAt(0)));
      locations->SetInAt(1, Location::FpuRegisterLocation(calling_convention.GetFpuRegisterAt(1)));
      locations->SetOut(Location::FpuRegisterLocation(S0));
      break;
    }

    case Primitive::kPrimDouble: {
      InvokeRuntimeCallingConvention calling_convention;
      locations->SetInAt(0, Location::FpuRegisterPairLocation(
          calling_convention.GetFpuRegisterAt(0), calling_convention.GetFpuRegisterAt(1)));
      locations->SetInAt(1, Location::FpuRegisterPairLocation(
          calling_convention.GetFpuRegisterAt(2), calling_convention.GetFpuRegisterAt(3)));
      locations->SetOut(Location::Location::FpuRegisterPairLocation(S0, S1));
      break;
    }

    default:
      LOG(FATAL) << "Unexpected rem type " << type;
  }
}

void InstructionCodeGeneratorARM::VisitRem(HRem* rem) {
  LocationSummary* locations = rem->GetLocations();
  Location out = locations->Out();
  Location first = locations->InAt(0);
  Location second = locations->InAt(1);

  Primitive::Type type = rem->GetResultType();
  switch (type) {
    case Primitive::kPrimInt: {
      if (codegen_->GetInstructionSetFeatures().HasDivideInstruction()) {
        Register reg1 = first.AsRegister<Register>();
        Register reg2 = second.AsRegister<Register>();
        Register temp = locations->GetTemp(0).AsRegister<Register>();

        // temp = reg1 / reg2  (integer division)
        // temp = temp * reg2
        // dest = reg1 - temp
        __ sdiv(temp, reg1, reg2);
        __ mul(temp, temp, reg2);
        __ sub(out.AsRegister<Register>(), reg1, ShifterOperand(temp));
      } else {
        InvokeRuntimeCallingConvention calling_convention;
        DCHECK_EQ(calling_convention.GetRegisterAt(0), first.AsRegister<Register>());
        DCHECK_EQ(calling_convention.GetRegisterAt(1), second.AsRegister<Register>());
        DCHECK_EQ(R1, out.AsRegister<Register>());

        codegen_->InvokeRuntime(QUICK_ENTRY_POINT(pIdivmod), rem, rem->GetDexPc(), nullptr);
      }
      break;
    }

    case Primitive::kPrimLong: {
      codegen_->InvokeRuntime(QUICK_ENTRY_POINT(pLmod), rem, rem->GetDexPc(), nullptr);
      break;
    }

    case Primitive::kPrimFloat: {
      codegen_->InvokeRuntime(QUICK_ENTRY_POINT(pFmodf), rem, rem->GetDexPc(), nullptr);
      break;
    }

    case Primitive::kPrimDouble: {
      codegen_->InvokeRuntime(QUICK_ENTRY_POINT(pFmod), rem, rem->GetDexPc(), nullptr);
      break;
    }

    default:
      LOG(FATAL) << "Unexpected rem type " << type;
  }
}

void LocationsBuilderARM::VisitDivZeroCheck(HDivZeroCheck* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RegisterOrConstant(instruction->InputAt(0)));
  if (instruction->HasUses()) {
    locations->SetOut(Location::SameAsFirstInput());
  }
}

void InstructionCodeGeneratorARM::VisitDivZeroCheck(HDivZeroCheck* instruction) {
  SlowPathCodeARM* slow_path = new (GetGraph()->GetArena()) DivZeroCheckSlowPathARM(instruction);
  codegen_->AddSlowPath(slow_path);

  LocationSummary* locations = instruction->GetLocations();
  Location value = locations->InAt(0);

  switch (instruction->GetType()) {
    case Primitive::kPrimInt: {
      if (value.IsRegister()) {
        __ cmp(value.AsRegister<Register>(), ShifterOperand(0));
        __ b(slow_path->GetEntryLabel(), EQ);
      } else {
        DCHECK(value.IsConstant()) << value;
        if (value.GetConstant()->AsIntConstant()->GetValue() == 0) {
          __ b(slow_path->GetEntryLabel());
        }
      }
      break;
    }
    case Primitive::kPrimLong: {
      if (value.IsRegisterPair()) {
        __ orrs(IP,
                value.AsRegisterPairLow<Register>(),
                ShifterOperand(value.AsRegisterPairHigh<Register>()));
        __ b(slow_path->GetEntryLabel(), EQ);
      } else {
        DCHECK(value.IsConstant()) << value;
        if (value.GetConstant()->AsLongConstant()->GetValue() == 0) {
          __ b(slow_path->GetEntryLabel());
        }
      }
      break;
    default:
      LOG(FATAL) << "Unexpected type for HDivZeroCheck " << instruction->GetType();
    }
  }
}

void LocationsBuilderARM::HandleShift(HBinaryOperation* op) {
  DCHECK(op->IsShl() || op->IsShr() || op->IsUShr());

  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(op, LocationSummary::kNoCall);

  switch (op->GetResultType()) {
    case Primitive::kPrimInt: {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RegisterOrConstant(op->InputAt(1)));
      // Make the output overlap, as it will be used to hold the masked
      // second input.
      locations->SetOut(Location::RequiresRegister(), Location::kOutputOverlap);
      break;
    }
    case Primitive::kPrimLong: {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RequiresRegister());
      locations->AddTemp(Location::RequiresRegister());
      locations->SetOut(Location::RequiresRegister());
      break;
    }
    default:
      LOG(FATAL) << "Unexpected operation type " << op->GetResultType();
  }
}

void InstructionCodeGeneratorARM::HandleShift(HBinaryOperation* op) {
  DCHECK(op->IsShl() || op->IsShr() || op->IsUShr());

  LocationSummary* locations = op->GetLocations();
  Location out = locations->Out();
  Location first = locations->InAt(0);
  Location second = locations->InAt(1);

  Primitive::Type type = op->GetResultType();
  switch (type) {
    case Primitive::kPrimInt: {
      Register out_reg = out.AsRegister<Register>();
      Register first_reg = first.AsRegister<Register>();
      // Arm doesn't mask the shift count so we need to do it ourselves.
      if (second.IsRegister()) {
        Register second_reg = second.AsRegister<Register>();
        __ and_(out_reg, second_reg, ShifterOperand(kMaxIntShiftValue));
        if (op->IsShl()) {
          __ Lsl(out_reg, first_reg, out_reg);
        } else if (op->IsShr()) {
          __ Asr(out_reg, first_reg, out_reg);
        } else {
          __ Lsr(out_reg, first_reg, out_reg);
        }
      } else {
        int32_t cst = second.GetConstant()->AsIntConstant()->GetValue();
        uint32_t shift_value = static_cast<uint32_t>(cst & kMaxIntShiftValue);
        if (shift_value == 0) {  // arm does not support shifting with 0 immediate.
          __ Mov(out_reg, first_reg);
        } else if (op->IsShl()) {
          __ Lsl(out_reg, first_reg, shift_value);
        } else if (op->IsShr()) {
          __ Asr(out_reg, first_reg, shift_value);
        } else {
          __ Lsr(out_reg, first_reg, shift_value);
        }
      }
      break;
    }
    case Primitive::kPrimLong: {
      Register o_h = out.AsRegisterPairHigh<Register>();
      Register o_l = out.AsRegisterPairLow<Register>();

      Register temp = locations->GetTemp(0).AsRegister<Register>();

      Register high = first.AsRegisterPairHigh<Register>();
      Register low = first.AsRegisterPairLow<Register>();

      Register second_reg = second.AsRegister<Register>();

      if (op->IsShl()) {
        __ and_(o_l, second_reg, ShifterOperand(kMaxLongShiftValue));
        // Shift the high part
        __ Lsl(o_h, high, o_l);
        // Shift the low part and `or` what overflew on the high part
        __ rsb(temp, o_l, ShifterOperand(kArmBitsPerWord));
        __ Lsr(temp, low, temp);
        __ orr(o_h, o_h, ShifterOperand(temp));
        // If the shift is > 32 bits, override the high part
        __ subs(temp, o_l, ShifterOperand(kArmBitsPerWord));
        __ it(PL);
        __ Lsl(o_h, low, temp, false, PL);
        // Shift the low part
        __ Lsl(o_l, low, o_l);
      } else if (op->IsShr()) {
        __ and_(o_h, second_reg, ShifterOperand(kMaxLongShiftValue));
        // Shift the low part
        __ Lsr(o_l, low, o_h);
        // Shift the high part and `or` what underflew on the low part
        __ rsb(temp, o_h, ShifterOperand(kArmBitsPerWord));
        __ Lsl(temp, high, temp);
        __ orr(o_l, o_l, ShifterOperand(temp));
        // If the shift is > 32 bits, override the low part
        __ subs(temp, o_h, ShifterOperand(kArmBitsPerWord));
        __ it(PL);
        __ Asr(o_l, high, temp, false, PL);
        // Shift the high part
        __ Asr(o_h, high, o_h);
      } else {
        __ and_(o_h, second_reg, ShifterOperand(kMaxLongShiftValue));
        // same as Shr except we use `Lsr`s and not `Asr`s
        __ Lsr(o_l, low, o_h);
        __ rsb(temp, o_h, ShifterOperand(kArmBitsPerWord));
        __ Lsl(temp, high, temp);
        __ orr(o_l, o_l, ShifterOperand(temp));
        __ subs(temp, o_h, ShifterOperand(kArmBitsPerWord));
        __ it(PL);
        __ Lsr(o_l, high, temp, false, PL);
        __ Lsr(o_h, high, o_h);
      }
      break;
    }
    default:
      LOG(FATAL) << "Unexpected operation type " << type;
  }
}

void LocationsBuilderARM::VisitShl(HShl* shl) {
  HandleShift(shl);
}

void InstructionCodeGeneratorARM::VisitShl(HShl* shl) {
  HandleShift(shl);
}

void LocationsBuilderARM::VisitShr(HShr* shr) {
  HandleShift(shr);
}

void InstructionCodeGeneratorARM::VisitShr(HShr* shr) {
  HandleShift(shr);
}

void LocationsBuilderARM::VisitUShr(HUShr* ushr) {
  HandleShift(ushr);
}

void InstructionCodeGeneratorARM::VisitUShr(HUShr* ushr) {
  HandleShift(ushr);
}

void LocationsBuilderARM::VisitNewInstance(HNewInstance* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCall);
  InvokeRuntimeCallingConvention calling_convention;
  locations->AddTemp(Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
  locations->AddTemp(Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
  locations->SetOut(Location::RegisterLocation(R0));
}

void InstructionCodeGeneratorARM::VisitNewInstance(HNewInstance* instruction) {
  InvokeRuntimeCallingConvention calling_convention;
  codegen_->LoadCurrentMethod(calling_convention.GetRegisterAt(1));
  __ LoadImmediate(calling_convention.GetRegisterAt(0), instruction->GetTypeIndex());
  codegen_->InvokeRuntime(GetThreadOffset<kArmWordSize>(instruction->GetEntrypoint()).Int32Value(),
                          instruction,
                          instruction->GetDexPc(),
                          nullptr);
}

void LocationsBuilderARM::VisitNewArray(HNewArray* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCall);
  InvokeRuntimeCallingConvention calling_convention;
  locations->AddTemp(Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
  locations->AddTemp(Location::RegisterLocation(calling_convention.GetRegisterAt(2)));
  locations->SetOut(Location::RegisterLocation(R0));
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
}

void InstructionCodeGeneratorARM::VisitNewArray(HNewArray* instruction) {
  InvokeRuntimeCallingConvention calling_convention;
  codegen_->LoadCurrentMethod(calling_convention.GetRegisterAt(2));
  __ LoadImmediate(calling_convention.GetRegisterAt(0), instruction->GetTypeIndex());
  codegen_->InvokeRuntime(GetThreadOffset<kArmWordSize>(instruction->GetEntrypoint()).Int32Value(),
                          instruction,
                          instruction->GetDexPc(),
                          nullptr);
}

void LocationsBuilderARM::VisitParameterValue(HParameterValue* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  Location location = parameter_visitor_.GetNextLocation(instruction->GetType());
  if (location.IsStackSlot()) {
    location = Location::StackSlot(location.GetStackIndex() + codegen_->GetFrameSize());
  } else if (location.IsDoubleStackSlot()) {
    location = Location::DoubleStackSlot(location.GetStackIndex() + codegen_->GetFrameSize());
  }
  locations->SetOut(location);
}

void InstructionCodeGeneratorARM::VisitParameterValue(HParameterValue* instruction) {
  // Nothing to do, the parameter is already at its location.
  UNUSED(instruction);
}

void LocationsBuilderARM::VisitNot(HNot* not_) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(not_, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

void InstructionCodeGeneratorARM::VisitNot(HNot* not_) {
  LocationSummary* locations = not_->GetLocations();
  Location out = locations->Out();
  Location in = locations->InAt(0);
  switch (not_->GetResultType()) {
    case Primitive::kPrimInt:
      __ mvn(out.AsRegister<Register>(), ShifterOperand(in.AsRegister<Register>()));
      break;

    case Primitive::kPrimLong:
      __ mvn(out.AsRegisterPairLow<Register>(),
             ShifterOperand(in.AsRegisterPairLow<Register>()));
      __ mvn(out.AsRegisterPairHigh<Register>(),
             ShifterOperand(in.AsRegisterPairHigh<Register>()));
      break;

    default:
      LOG(FATAL) << "Unimplemented type for not operation " << not_->GetResultType();
  }
}

void LocationsBuilderARM::VisitBooleanNot(HBooleanNot* bool_not) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(bool_not, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

void InstructionCodeGeneratorARM::VisitBooleanNot(HBooleanNot* bool_not) {
  LocationSummary* locations = bool_not->GetLocations();
  Location out = locations->Out();
  Location in = locations->InAt(0);
  __ eor(out.AsRegister<Register>(), in.AsRegister<Register>(), ShifterOperand(1));
}

void LocationsBuilderARM::VisitCompare(HCompare* compare) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(compare, LocationSummary::kNoCall);
  switch (compare->InputAt(0)->GetType()) {
    case Primitive::kPrimLong: {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RequiresRegister());
      // Output overlaps because it is written before doing the low comparison.
      locations->SetOut(Location::RequiresRegister(), Location::kOutputOverlap);
      break;
    }
    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetInAt(1, Location::RequiresFpuRegister());
      locations->SetOut(Location::RequiresRegister());
      break;
    }
    default:
      LOG(FATAL) << "Unexpected type for compare operation " << compare->InputAt(0)->GetType();
  }
}

void InstructionCodeGeneratorARM::VisitCompare(HCompare* compare) {
  LocationSummary* locations = compare->GetLocations();
  Register out = locations->Out().AsRegister<Register>();
  Location left = locations->InAt(0);
  Location right = locations->InAt(1);

  Label less, greater, done;
  Primitive::Type type = compare->InputAt(0)->GetType();
  switch (type) {
    case Primitive::kPrimLong: {
      __ cmp(left.AsRegisterPairHigh<Register>(),
             ShifterOperand(right.AsRegisterPairHigh<Register>()));  // Signed compare.
      __ b(&less, LT);
      __ b(&greater, GT);
      // Do LoadImmediate before any `cmp`, as LoadImmediate might affect the status flags.
      __ LoadImmediate(out, 0);
      __ cmp(left.AsRegisterPairLow<Register>(),
             ShifterOperand(right.AsRegisterPairLow<Register>()));  // Unsigned compare.
      break;
    }
    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      __ LoadImmediate(out, 0);
      if (type == Primitive::kPrimFloat) {
        __ vcmps(left.AsFpuRegister<SRegister>(), right.AsFpuRegister<SRegister>());
      } else {
        __ vcmpd(FromLowSToD(left.AsFpuRegisterPairLow<SRegister>()),
                 FromLowSToD(right.AsFpuRegisterPairLow<SRegister>()));
      }
      __ vmstat();  // transfer FP status register to ARM APSR.
      __ b(compare->IsGtBias() ? &greater : &less, VS);  // VS for unordered.
      break;
    }
    default:
      LOG(FATAL) << "Unexpected compare type " << type;
  }
  __ b(&done, EQ);
  __ b(&less, CC);  // CC is for both: unsigned compare for longs and 'less than' for floats.

  __ Bind(&greater);
  __ LoadImmediate(out, 1);
  __ b(&done);

  __ Bind(&less);
  __ LoadImmediate(out, -1);

  __ Bind(&done);
}

void LocationsBuilderARM::VisitPhi(HPhi* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  for (size_t i = 0, e = instruction->InputCount(); i < e; ++i) {
    locations->SetInAt(i, Location::Any());
  }
  locations->SetOut(Location::Any());
}

void InstructionCodeGeneratorARM::VisitPhi(HPhi* instruction) {
  UNUSED(instruction);
  LOG(FATAL) << "Unreachable";
}

void InstructionCodeGeneratorARM::GenerateMemoryBarrier(MemBarrierKind kind) {
  // TODO (ported from quick): revisit Arm barrier kinds
  DmbOptions flavour = DmbOptions::ISH;  // quiet c++ warnings
  switch (kind) {
    case MemBarrierKind::kAnyStore:
    case MemBarrierKind::kLoadAny:
    case MemBarrierKind::kAnyAny: {
      flavour = DmbOptions::ISH;
      break;
    }
    case MemBarrierKind::kStoreStore: {
      flavour = DmbOptions::ISHST;
      break;
    }
    default:
      LOG(FATAL) << "Unexpected memory barrier " << kind;
  }
  __ dmb(flavour);
}

void InstructionCodeGeneratorARM::GenerateWideAtomicLoad(Register addr,
                                                         uint32_t offset,
                                                         Register out_lo,
                                                         Register out_hi) {
  if (offset != 0) {
    __ LoadImmediate(out_lo, offset);
    __ add(IP, addr, ShifterOperand(out_lo));
    addr = IP;
  }
  __ ldrexd(out_lo, out_hi, addr);
}

void InstructionCodeGeneratorARM::GenerateWideAtomicStore(Register addr,
                                                          uint32_t offset,
                                                          Register value_lo,
                                                          Register value_hi,
                                                          Register temp1,
                                                          Register temp2,
                                                          HInstruction* instruction) {
  Label fail;
  if (offset != 0) {
    __ LoadImmediate(temp1, offset);
    __ add(IP, addr, ShifterOperand(temp1));
    addr = IP;
  }
  __ Bind(&fail);
  // We need a load followed by store. (The address used in a STREX instruction must
  // be the same as the address in the most recently executed LDREX instruction.)
  __ ldrexd(temp1, temp2, addr);
  codegen_->MaybeRecordImplicitNullCheck(instruction);
  __ strexd(temp1, value_lo, value_hi, addr);
  __ cmp(temp1, ShifterOperand(0));
  __ b(&fail, NE);
}

void LocationsBuilderARM::HandleFieldSet(HInstruction* instruction, const FieldInfo& field_info) {
  DCHECK(instruction->IsInstanceFieldSet() || instruction->IsStaticFieldSet());

  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());

  Primitive::Type field_type = field_info.GetFieldType();
  if (Primitive::IsFloatingPointType(field_type)) {
    locations->SetInAt(1, Location::RequiresFpuRegister());
  } else {
    locations->SetInAt(1, Location::RequiresRegister());
  }

  bool is_wide = field_type == Primitive::kPrimLong || field_type == Primitive::kPrimDouble;
  bool generate_volatile = field_info.IsVolatile()
      && is_wide
      && !codegen_->GetInstructionSetFeatures().HasAtomicLdrdAndStrd();
  // Temporary registers for the write barrier.
  // TODO: consider renaming StoreNeedsWriteBarrier to StoreNeedsGCMark.
  if (CodeGenerator::StoreNeedsWriteBarrier(field_type, instruction->InputAt(1))) {
    locations->AddTemp(Location::RequiresRegister());
    locations->AddTemp(Location::RequiresRegister());
  } else if (generate_volatile) {
    // Arm encoding have some additional constraints for ldrexd/strexd:
    // - registers need to be consecutive
    // - the first register should be even but not R14.
    // We don't test for Arm yet, and the assertion makes sure that we revisit this if we ever
    // enable Arm encoding.
    DCHECK_EQ(InstructionSet::kThumb2, codegen_->GetInstructionSet());

    locations->AddTemp(Location::RequiresRegister());
    locations->AddTemp(Location::RequiresRegister());
    if (field_type == Primitive::kPrimDouble) {
      // For doubles we need two more registers to copy the value.
      locations->AddTemp(Location::RegisterLocation(R2));
      locations->AddTemp(Location::RegisterLocation(R3));
    }
  }
}

void InstructionCodeGeneratorARM::HandleFieldSet(HInstruction* instruction,
                                                 const FieldInfo& field_info) {
  DCHECK(instruction->IsInstanceFieldSet() || instruction->IsStaticFieldSet());

  LocationSummary* locations = instruction->GetLocations();
  Register base = locations->InAt(0).AsRegister<Register>();
  Location value = locations->InAt(1);

  bool is_volatile = field_info.IsVolatile();
  bool atomic_ldrd_strd = codegen_->GetInstructionSetFeatures().HasAtomicLdrdAndStrd();
  Primitive::Type field_type = field_info.GetFieldType();
  uint32_t offset = field_info.GetFieldOffset().Uint32Value();

  if (is_volatile) {
    GenerateMemoryBarrier(MemBarrierKind::kAnyStore);
  }

  switch (field_type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte: {
      __ StoreToOffset(kStoreByte, value.AsRegister<Register>(), base, offset);
      break;
    }

    case Primitive::kPrimShort:
    case Primitive::kPrimChar: {
      __ StoreToOffset(kStoreHalfword, value.AsRegister<Register>(), base, offset);
      break;
    }

    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      __ StoreToOffset(kStoreWord, value.AsRegister<Register>(), base, offset);
      break;
    }

    case Primitive::kPrimLong: {
      if (is_volatile && !atomic_ldrd_strd) {
        GenerateWideAtomicStore(base, offset,
                                value.AsRegisterPairLow<Register>(),
                                value.AsRegisterPairHigh<Register>(),
                                locations->GetTemp(0).AsRegister<Register>(),
                                locations->GetTemp(1).AsRegister<Register>(),
                                instruction);
      } else {
        __ StoreToOffset(kStoreWordPair, value.AsRegisterPairLow<Register>(), base, offset);
        codegen_->MaybeRecordImplicitNullCheck(instruction);
      }
      break;
    }

    case Primitive::kPrimFloat: {
      __ StoreSToOffset(value.AsFpuRegister<SRegister>(), base, offset);
      break;
    }

    case Primitive::kPrimDouble: {
      DRegister value_reg = FromLowSToD(value.AsFpuRegisterPairLow<SRegister>());
      if (is_volatile && !atomic_ldrd_strd) {
        Register value_reg_lo = locations->GetTemp(0).AsRegister<Register>();
        Register value_reg_hi = locations->GetTemp(1).AsRegister<Register>();

        __ vmovrrd(value_reg_lo, value_reg_hi, value_reg);

        GenerateWideAtomicStore(base, offset,
                                value_reg_lo,
                                value_reg_hi,
                                locations->GetTemp(2).AsRegister<Register>(),
                                locations->GetTemp(3).AsRegister<Register>(),
                                instruction);
      } else {
        __ StoreDToOffset(value_reg, base, offset);
        codegen_->MaybeRecordImplicitNullCheck(instruction);
      }
      break;
    }

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << field_type;
      UNREACHABLE();
  }

  // Longs and doubles are handled in the switch.
  if (field_type != Primitive::kPrimLong && field_type != Primitive::kPrimDouble) {
    codegen_->MaybeRecordImplicitNullCheck(instruction);
  }

  if (CodeGenerator::StoreNeedsWriteBarrier(field_type, instruction->InputAt(1))) {
    Register temp = locations->GetTemp(0).AsRegister<Register>();
    Register card = locations->GetTemp(1).AsRegister<Register>();
    codegen_->MarkGCCard(temp, card, base, value.AsRegister<Register>());
  }

  if (is_volatile) {
    GenerateMemoryBarrier(MemBarrierKind::kAnyAny);
  }
}

void LocationsBuilderARM::HandleFieldGet(HInstruction* instruction, const FieldInfo& field_info) {
  DCHECK(instruction->IsInstanceFieldGet() || instruction->IsStaticFieldGet());
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());

  bool volatile_for_double = field_info.IsVolatile()
      && (field_info.GetFieldType() == Primitive::kPrimDouble)
      && !codegen_->GetInstructionSetFeatures().HasAtomicLdrdAndStrd();
  bool overlap = field_info.IsVolatile() && (field_info.GetFieldType() == Primitive::kPrimLong);

  if (Primitive::IsFloatingPointType(instruction->GetType())) {
    locations->SetOut(Location::RequiresFpuRegister());
  } else {
    locations->SetOut(Location::RequiresRegister(),
                      (overlap ? Location::kOutputOverlap : Location::kNoOutputOverlap));
  }
  if (volatile_for_double) {
    // Arm encoding have some additional constraints for ldrexd/strexd:
    // - registers need to be consecutive
    // - the first register should be even but not R14.
    // We don't test for Arm yet, and the assertion makes sure that we revisit this if we ever
    // enable Arm encoding.
    DCHECK_EQ(InstructionSet::kThumb2, codegen_->GetInstructionSet());
    locations->AddTemp(Location::RequiresRegister());
    locations->AddTemp(Location::RequiresRegister());
  }
}

void InstructionCodeGeneratorARM::HandleFieldGet(HInstruction* instruction,
                                                 const FieldInfo& field_info) {
  DCHECK(instruction->IsInstanceFieldGet() || instruction->IsStaticFieldGet());

  LocationSummary* locations = instruction->GetLocations();
  Register base = locations->InAt(0).AsRegister<Register>();
  Location out = locations->Out();
  bool is_volatile = field_info.IsVolatile();
  bool atomic_ldrd_strd = codegen_->GetInstructionSetFeatures().HasAtomicLdrdAndStrd();
  Primitive::Type field_type = field_info.GetFieldType();
  uint32_t offset = field_info.GetFieldOffset().Uint32Value();

  switch (field_type) {
    case Primitive::kPrimBoolean: {
      __ LoadFromOffset(kLoadUnsignedByte, out.AsRegister<Register>(), base, offset);
      break;
    }

    case Primitive::kPrimByte: {
      __ LoadFromOffset(kLoadSignedByte, out.AsRegister<Register>(), base, offset);
      break;
    }

    case Primitive::kPrimShort: {
      __ LoadFromOffset(kLoadSignedHalfword, out.AsRegister<Register>(), base, offset);
      break;
    }

    case Primitive::kPrimChar: {
      __ LoadFromOffset(kLoadUnsignedHalfword, out.AsRegister<Register>(), base, offset);
      break;
    }

    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      __ LoadFromOffset(kLoadWord, out.AsRegister<Register>(), base, offset);
      break;
    }

    case Primitive::kPrimLong: {
      if (is_volatile && !atomic_ldrd_strd) {
        GenerateWideAtomicLoad(base, offset,
                               out.AsRegisterPairLow<Register>(),
                               out.AsRegisterPairHigh<Register>());
      } else {
        __ LoadFromOffset(kLoadWordPair, out.AsRegisterPairLow<Register>(), base, offset);
      }
      break;
    }

    case Primitive::kPrimFloat: {
      __ LoadSFromOffset(out.AsFpuRegister<SRegister>(), base, offset);
      break;
    }

    case Primitive::kPrimDouble: {
      DRegister out_reg = FromLowSToD(out.AsFpuRegisterPairLow<SRegister>());
      if (is_volatile && !atomic_ldrd_strd) {
        Register lo = locations->GetTemp(0).AsRegister<Register>();
        Register hi = locations->GetTemp(1).AsRegister<Register>();
        GenerateWideAtomicLoad(base, offset, lo, hi);
        codegen_->MaybeRecordImplicitNullCheck(instruction);
        __ vmovdrr(out_reg, lo, hi);
      } else {
        __ LoadDFromOffset(out_reg, base, offset);
        codegen_->MaybeRecordImplicitNullCheck(instruction);
      }
      break;
    }

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << field_type;
      UNREACHABLE();
  }

  // Doubles are handled in the switch.
  if (field_type != Primitive::kPrimDouble) {
    codegen_->MaybeRecordImplicitNullCheck(instruction);
  }

  if (is_volatile) {
    GenerateMemoryBarrier(MemBarrierKind::kLoadAny);
  }
}

void LocationsBuilderARM::VisitInstanceFieldSet(HInstanceFieldSet* instruction) {
  HandleFieldSet(instruction, instruction->GetFieldInfo());
}

void InstructionCodeGeneratorARM::VisitInstanceFieldSet(HInstanceFieldSet* instruction) {
  HandleFieldSet(instruction, instruction->GetFieldInfo());
}

void LocationsBuilderARM::VisitInstanceFieldGet(HInstanceFieldGet* instruction) {
  HandleFieldGet(instruction, instruction->GetFieldInfo());
}

void InstructionCodeGeneratorARM::VisitInstanceFieldGet(HInstanceFieldGet* instruction) {
  HandleFieldGet(instruction, instruction->GetFieldInfo());
}

void LocationsBuilderARM::VisitStaticFieldGet(HStaticFieldGet* instruction) {
  HandleFieldGet(instruction, instruction->GetFieldInfo());
}

void InstructionCodeGeneratorARM::VisitStaticFieldGet(HStaticFieldGet* instruction) {
  HandleFieldGet(instruction, instruction->GetFieldInfo());
}

void LocationsBuilderARM::VisitStaticFieldSet(HStaticFieldSet* instruction) {
  HandleFieldSet(instruction, instruction->GetFieldInfo());
}

void InstructionCodeGeneratorARM::VisitStaticFieldSet(HStaticFieldSet* instruction) {
  HandleFieldSet(instruction, instruction->GetFieldInfo());
}

void LocationsBuilderARM::VisitNullCheck(HNullCheck* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  if (instruction->HasUses()) {
    locations->SetOut(Location::SameAsFirstInput());
  }
}

void InstructionCodeGeneratorARM::GenerateImplicitNullCheck(HNullCheck* instruction) {
  if (codegen_->CanMoveNullCheckToUser(instruction)) {
    return;
  }
  Location obj = instruction->GetLocations()->InAt(0);

  __ LoadFromOffset(kLoadWord, IP, obj.AsRegister<Register>(), 0);
  codegen_->RecordPcInfo(instruction, instruction->GetDexPc());
}

void InstructionCodeGeneratorARM::GenerateExplicitNullCheck(HNullCheck* instruction) {
  SlowPathCodeARM* slow_path = new (GetGraph()->GetArena()) NullCheckSlowPathARM(instruction);
  codegen_->AddSlowPath(slow_path);

  LocationSummary* locations = instruction->GetLocations();
  Location obj = locations->InAt(0);

  __ cmp(obj.AsRegister<Register>(), ShifterOperand(0));
  __ b(slow_path->GetEntryLabel(), EQ);
}

void InstructionCodeGeneratorARM::VisitNullCheck(HNullCheck* instruction) {
  if (codegen_->GetCompilerOptions().GetImplicitNullChecks()) {
    GenerateImplicitNullCheck(instruction);
  } else {
    GenerateExplicitNullCheck(instruction);
  }
}

void LocationsBuilderARM::VisitArrayGet(HArrayGet* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RegisterOrConstant(instruction->InputAt(1)));
  if (Primitive::IsFloatingPointType(instruction->GetType())) {
    locations->SetOut(Location::RequiresFpuRegister(), Location::kNoOutputOverlap);
  } else {
    locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
  }
}

void InstructionCodeGeneratorARM::VisitArrayGet(HArrayGet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Register obj = locations->InAt(0).AsRegister<Register>();
  Location index = locations->InAt(1);

  switch (instruction->GetType()) {
    case Primitive::kPrimBoolean: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(uint8_t)).Uint32Value();
      Register out = locations->Out().AsRegister<Register>();
      if (index.IsConstant()) {
        size_t offset =
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_1) + data_offset;
        __ LoadFromOffset(kLoadUnsignedByte, out, obj, offset);
      } else {
        __ add(IP, obj, ShifterOperand(index.AsRegister<Register>()));
        __ LoadFromOffset(kLoadUnsignedByte, out, IP, data_offset);
      }
      break;
    }

    case Primitive::kPrimByte: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(int8_t)).Uint32Value();
      Register out = locations->Out().AsRegister<Register>();
      if (index.IsConstant()) {
        size_t offset =
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_1) + data_offset;
        __ LoadFromOffset(kLoadSignedByte, out, obj, offset);
      } else {
        __ add(IP, obj, ShifterOperand(index.AsRegister<Register>()));
        __ LoadFromOffset(kLoadSignedByte, out, IP, data_offset);
      }
      break;
    }

    case Primitive::kPrimShort: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(int16_t)).Uint32Value();
      Register out = locations->Out().AsRegister<Register>();
      if (index.IsConstant()) {
        size_t offset =
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_2) + data_offset;
        __ LoadFromOffset(kLoadSignedHalfword, out, obj, offset);
      } else {
        __ add(IP, obj, ShifterOperand(index.AsRegister<Register>(), LSL, TIMES_2));
        __ LoadFromOffset(kLoadSignedHalfword, out, IP, data_offset);
      }
      break;
    }

    case Primitive::kPrimChar: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(uint16_t)).Uint32Value();
      Register out = locations->Out().AsRegister<Register>();
      if (index.IsConstant()) {
        size_t offset =
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_2) + data_offset;
        __ LoadFromOffset(kLoadUnsignedHalfword, out, obj, offset);
      } else {
        __ add(IP, obj, ShifterOperand(index.AsRegister<Register>(), LSL, TIMES_2));
        __ LoadFromOffset(kLoadUnsignedHalfword, out, IP, data_offset);
      }
      break;
    }

    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      DCHECK_EQ(sizeof(mirror::HeapReference<mirror::Object>), sizeof(int32_t));
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(int32_t)).Uint32Value();
      Register out = locations->Out().AsRegister<Register>();
      if (index.IsConstant()) {
        size_t offset =
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_4) + data_offset;
        __ LoadFromOffset(kLoadWord, out, obj, offset);
      } else {
        __ add(IP, obj, ShifterOperand(index.AsRegister<Register>(), LSL, TIMES_4));
        __ LoadFromOffset(kLoadWord, out, IP, data_offset);
      }
      break;
    }

    case Primitive::kPrimLong: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(int64_t)).Uint32Value();
      Location out = locations->Out();
      if (index.IsConstant()) {
        size_t offset =
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_8) + data_offset;
        __ LoadFromOffset(kLoadWordPair, out.AsRegisterPairLow<Register>(), obj, offset);
      } else {
        __ add(IP, obj, ShifterOperand(index.AsRegister<Register>(), LSL, TIMES_8));
        __ LoadFromOffset(kLoadWordPair, out.AsRegisterPairLow<Register>(), IP, data_offset);
      }
      break;
    }

    case Primitive::kPrimFloat: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(float)).Uint32Value();
      Location out = locations->Out();
      DCHECK(out.IsFpuRegister());
      if (index.IsConstant()) {
        size_t offset = (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_4) + data_offset;
        __ LoadSFromOffset(out.AsFpuRegister<SRegister>(), obj, offset);
      } else {
        __ add(IP, obj, ShifterOperand(index.AsRegister<Register>(), LSL, TIMES_4));
        __ LoadSFromOffset(out.AsFpuRegister<SRegister>(), IP, data_offset);
      }
      break;
    }

    case Primitive::kPrimDouble: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(double)).Uint32Value();
      Location out = locations->Out();
      DCHECK(out.IsFpuRegisterPair());
      if (index.IsConstant()) {
        size_t offset = (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_8) + data_offset;
        __ LoadDFromOffset(FromLowSToD(out.AsFpuRegisterPairLow<SRegister>()), obj, offset);
      } else {
        __ add(IP, obj, ShifterOperand(index.AsRegister<Register>(), LSL, TIMES_8));
        __ LoadDFromOffset(FromLowSToD(out.AsFpuRegisterPairLow<SRegister>()), IP, data_offset);
      }
      break;
    }

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << instruction->GetType();
      UNREACHABLE();
  }
  codegen_->MaybeRecordImplicitNullCheck(instruction);
}

void LocationsBuilderARM::VisitArraySet(HArraySet* instruction) {
  Primitive::Type value_type = instruction->GetComponentType();

  bool needs_write_barrier =
      CodeGenerator::StoreNeedsWriteBarrier(value_type, instruction->GetValue());
  bool needs_runtime_call = instruction->NeedsTypeCheck();

  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(
      instruction, needs_runtime_call ? LocationSummary::kCall : LocationSummary::kNoCall);
  if (needs_runtime_call) {
    InvokeRuntimeCallingConvention calling_convention;
    locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
    locations->SetInAt(1, Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
    locations->SetInAt(2, Location::RegisterLocation(calling_convention.GetRegisterAt(2)));
  } else {
    locations->SetInAt(0, Location::RequiresRegister());
    locations->SetInAt(1, Location::RegisterOrConstant(instruction->InputAt(1)));
    if (Primitive::IsFloatingPointType(value_type)) {
      locations->SetInAt(2, Location::RequiresFpuRegister());
    } else {
      locations->SetInAt(2, Location::RequiresRegister());
    }

    if (needs_write_barrier) {
      // Temporary registers for the write barrier.
      locations->AddTemp(Location::RequiresRegister());
      locations->AddTemp(Location::RequiresRegister());
    }
  }
}

void InstructionCodeGeneratorARM::VisitArraySet(HArraySet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Register obj = locations->InAt(0).AsRegister<Register>();
  Location index = locations->InAt(1);
  Primitive::Type value_type = instruction->GetComponentType();
  bool needs_runtime_call = locations->WillCall();
  bool needs_write_barrier =
      CodeGenerator::StoreNeedsWriteBarrier(value_type, instruction->GetValue());

  switch (value_type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(uint8_t)).Uint32Value();
      Register value = locations->InAt(2).AsRegister<Register>();
      if (index.IsConstant()) {
        size_t offset =
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_1) + data_offset;
        __ StoreToOffset(kStoreByte, value, obj, offset);
      } else {
        __ add(IP, obj, ShifterOperand(index.AsRegister<Register>()));
        __ StoreToOffset(kStoreByte, value, IP, data_offset);
      }
      break;
    }

    case Primitive::kPrimShort:
    case Primitive::kPrimChar: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(uint16_t)).Uint32Value();
      Register value = locations->InAt(2).AsRegister<Register>();
      if (index.IsConstant()) {
        size_t offset =
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_2) + data_offset;
        __ StoreToOffset(kStoreHalfword, value, obj, offset);
      } else {
        __ add(IP, obj, ShifterOperand(index.AsRegister<Register>(), LSL, TIMES_2));
        __ StoreToOffset(kStoreHalfword, value, IP, data_offset);
      }
      break;
    }

    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      if (!needs_runtime_call) {
        uint32_t data_offset = mirror::Array::DataOffset(sizeof(int32_t)).Uint32Value();
        Register value = locations->InAt(2).AsRegister<Register>();
        if (index.IsConstant()) {
          size_t offset =
              (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_4) + data_offset;
          __ StoreToOffset(kStoreWord, value, obj, offset);
        } else {
          DCHECK(index.IsRegister()) << index;
          __ add(IP, obj, ShifterOperand(index.AsRegister<Register>(), LSL, TIMES_4));
          __ StoreToOffset(kStoreWord, value, IP, data_offset);
        }
        codegen_->MaybeRecordImplicitNullCheck(instruction);
        if (needs_write_barrier) {
          DCHECK_EQ(value_type, Primitive::kPrimNot);
          Register temp = locations->GetTemp(0).AsRegister<Register>();
          Register card = locations->GetTemp(1).AsRegister<Register>();
          codegen_->MarkGCCard(temp, card, obj, value);
        }
      } else {
        DCHECK_EQ(value_type, Primitive::kPrimNot);
        codegen_->InvokeRuntime(QUICK_ENTRY_POINT(pAputObject),
                                instruction,
                                instruction->GetDexPc(),
                                nullptr);
      }
      break;
    }

    case Primitive::kPrimLong: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(int64_t)).Uint32Value();
      Location value = locations->InAt(2);
      if (index.IsConstant()) {
        size_t offset =
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_8) + data_offset;
        __ StoreToOffset(kStoreWordPair, value.AsRegisterPairLow<Register>(), obj, offset);
      } else {
        __ add(IP, obj, ShifterOperand(index.AsRegister<Register>(), LSL, TIMES_8));
        __ StoreToOffset(kStoreWordPair, value.AsRegisterPairLow<Register>(), IP, data_offset);
      }
      break;
    }

    case Primitive::kPrimFloat: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(float)).Uint32Value();
      Location value = locations->InAt(2);
      DCHECK(value.IsFpuRegister());
      if (index.IsConstant()) {
        size_t offset = (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_4) + data_offset;
        __ StoreSToOffset(value.AsFpuRegister<SRegister>(), obj, offset);
      } else {
        __ add(IP, obj, ShifterOperand(index.AsRegister<Register>(), LSL, TIMES_4));
        __ StoreSToOffset(value.AsFpuRegister<SRegister>(), IP, data_offset);
      }
      break;
    }

    case Primitive::kPrimDouble: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(double)).Uint32Value();
      Location value = locations->InAt(2);
      DCHECK(value.IsFpuRegisterPair());
      if (index.IsConstant()) {
        size_t offset = (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_8) + data_offset;
        __ StoreDToOffset(FromLowSToD(value.AsFpuRegisterPairLow<SRegister>()), obj, offset);
      } else {
        __ add(IP, obj, ShifterOperand(index.AsRegister<Register>(), LSL, TIMES_8));
        __ StoreDToOffset(FromLowSToD(value.AsFpuRegisterPairLow<SRegister>()), IP, data_offset);
      }

      break;
    }

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << value_type;
      UNREACHABLE();
  }

  // Ints and objects are handled in the switch.
  if (value_type != Primitive::kPrimInt && value_type != Primitive::kPrimNot) {
    codegen_->MaybeRecordImplicitNullCheck(instruction);
  }
}

void LocationsBuilderARM::VisitArrayLength(HArrayLength* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

void InstructionCodeGeneratorARM::VisitArrayLength(HArrayLength* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  uint32_t offset = mirror::Array::LengthOffset().Uint32Value();
  Register obj = locations->InAt(0).AsRegister<Register>();
  Register out = locations->Out().AsRegister<Register>();
  __ LoadFromOffset(kLoadWord, out, obj, offset);
  codegen_->MaybeRecordImplicitNullCheck(instruction);
}

void LocationsBuilderARM::VisitBoundsCheck(HBoundsCheck* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  if (instruction->HasUses()) {
    locations->SetOut(Location::SameAsFirstInput());
  }
}

void InstructionCodeGeneratorARM::VisitBoundsCheck(HBoundsCheck* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  SlowPathCodeARM* slow_path = new (GetGraph()->GetArena()) BoundsCheckSlowPathARM(
      instruction, locations->InAt(0), locations->InAt(1));
  codegen_->AddSlowPath(slow_path);

  Register index = locations->InAt(0).AsRegister<Register>();
  Register length = locations->InAt(1).AsRegister<Register>();

  __ cmp(index, ShifterOperand(length));
  __ b(slow_path->GetEntryLabel(), CS);
}

void CodeGeneratorARM::MarkGCCard(Register temp, Register card, Register object, Register value) {
  Label is_null;
  __ CompareAndBranchIfZero(value, &is_null);
  __ LoadFromOffset(kLoadWord, card, TR, Thread::CardTableOffset<kArmWordSize>().Int32Value());
  __ Lsr(temp, object, gc::accounting::CardTable::kCardShift);
  __ strb(card, Address(card, temp));
  __ Bind(&is_null);
}

void LocationsBuilderARM::VisitTemporary(HTemporary* temp) {
  temp->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM::VisitTemporary(HTemporary* temp) {
  // Nothing to do, this is driven by the code generator.
  UNUSED(temp);
}

void LocationsBuilderARM::VisitParallelMove(HParallelMove* instruction) {
  UNUSED(instruction);
  LOG(FATAL) << "Unreachable";
}

void InstructionCodeGeneratorARM::VisitParallelMove(HParallelMove* instruction) {
  codegen_->GetMoveResolver()->EmitNativeCode(instruction);
}

void LocationsBuilderARM::VisitSuspendCheck(HSuspendCheck* instruction) {
  new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCallOnSlowPath);
}

void InstructionCodeGeneratorARM::VisitSuspendCheck(HSuspendCheck* instruction) {
  HBasicBlock* block = instruction->GetBlock();
  if (block->GetLoopInformation() != nullptr) {
    DCHECK(block->GetLoopInformation()->GetSuspendCheck() == instruction);
    // The back edge will generate the suspend check.
    return;
  }
  if (block->IsEntryBlock() && instruction->GetNext()->IsGoto()) {
    // The goto will generate the suspend check.
    return;
  }
  GenerateSuspendCheck(instruction, nullptr);
}

void InstructionCodeGeneratorARM::GenerateSuspendCheck(HSuspendCheck* instruction,
                                                       HBasicBlock* successor) {
  SuspendCheckSlowPathARM* slow_path =
      down_cast<SuspendCheckSlowPathARM*>(instruction->GetSlowPath());
  if (slow_path == nullptr) {
    slow_path = new (GetGraph()->GetArena()) SuspendCheckSlowPathARM(instruction, successor);
    instruction->SetSlowPath(slow_path);
    codegen_->AddSlowPath(slow_path);
    if (successor != nullptr) {
      DCHECK(successor->IsLoopHeader());
      codegen_->ClearSpillSlotsFromLoopPhisInStackMap(instruction);
    }
  } else {
    DCHECK_EQ(slow_path->GetSuccessor(), successor);
  }

  __ LoadFromOffset(
      kLoadUnsignedHalfword, IP, TR, Thread::ThreadFlagsOffset<kArmWordSize>().Int32Value());
  __ cmp(IP, ShifterOperand(0));
  // TODO: Figure out the branch offsets and use cbz/cbnz.
  if (successor == nullptr) {
    __ b(slow_path->GetEntryLabel(), NE);
    __ Bind(slow_path->GetReturnLabel());
  } else {
    __ b(codegen_->GetLabelOf(successor), EQ);
    __ b(slow_path->GetEntryLabel());
  }
}

ArmAssembler* ParallelMoveResolverARM::GetAssembler() const {
  return codegen_->GetAssembler();
}

void ParallelMoveResolverARM::EmitMove(size_t index) {
  MoveOperands* move = moves_.Get(index);
  Location source = move->GetSource();
  Location destination = move->GetDestination();

  if (source.IsRegister()) {
    if (destination.IsRegister()) {
      __ Mov(destination.AsRegister<Register>(), source.AsRegister<Register>());
    } else {
      DCHECK(destination.IsStackSlot());
      __ StoreToOffset(kStoreWord, source.AsRegister<Register>(),
                       SP, destination.GetStackIndex());
    }
  } else if (source.IsStackSlot()) {
    if (destination.IsRegister()) {
      __ LoadFromOffset(kLoadWord, destination.AsRegister<Register>(),
                        SP, source.GetStackIndex());
    } else if (destination.IsFpuRegister()) {
      __ LoadSFromOffset(destination.AsFpuRegister<SRegister>(), SP, source.GetStackIndex());
    } else {
      DCHECK(destination.IsStackSlot());
      __ LoadFromOffset(kLoadWord, IP, SP, source.GetStackIndex());
      __ StoreToOffset(kStoreWord, IP, SP, destination.GetStackIndex());
    }
  } else if (source.IsFpuRegister()) {
    if (destination.IsFpuRegister()) {
      __ vmovs(destination.AsFpuRegister<SRegister>(), source.AsFpuRegister<SRegister>());
    } else {
      DCHECK(destination.IsStackSlot());
      __ StoreSToOffset(source.AsFpuRegister<SRegister>(), SP, destination.GetStackIndex());
    }
  } else if (source.IsDoubleStackSlot()) {
    if (destination.IsDoubleStackSlot()) {
      __ LoadDFromOffset(DTMP, SP, source.GetStackIndex());
      __ StoreDToOffset(DTMP, SP, destination.GetStackIndex());
    } else if (destination.IsRegisterPair()) {
      DCHECK(ExpectedPairLayout(destination));
      __ LoadFromOffset(
          kLoadWordPair, destination.AsRegisterPairLow<Register>(), SP, source.GetStackIndex());
    } else {
      DCHECK(destination.IsFpuRegisterPair()) << destination;
      __ LoadDFromOffset(FromLowSToD(destination.AsFpuRegisterPairLow<SRegister>()),
                         SP,
                         source.GetStackIndex());
    }
  } else if (source.IsRegisterPair()) {
    if (destination.IsRegisterPair()) {
      __ Mov(destination.AsRegisterPairLow<Register>(), source.AsRegisterPairLow<Register>());
      __ Mov(destination.AsRegisterPairHigh<Register>(), source.AsRegisterPairHigh<Register>());
    } else {
      DCHECK(destination.IsDoubleStackSlot()) << destination;
      DCHECK(ExpectedPairLayout(source));
      __ StoreToOffset(
          kStoreWordPair, source.AsRegisterPairLow<Register>(), SP, destination.GetStackIndex());
    }
  } else if (source.IsFpuRegisterPair()) {
    if (destination.IsFpuRegisterPair()) {
      __ vmovd(FromLowSToD(destination.AsFpuRegisterPairLow<SRegister>()),
               FromLowSToD(source.AsFpuRegisterPairLow<SRegister>()));
    } else {
      DCHECK(destination.IsDoubleStackSlot()) << destination;
      __ StoreDToOffset(FromLowSToD(source.AsFpuRegisterPairLow<SRegister>()),
                        SP,
                        destination.GetStackIndex());
    }
  } else {
    DCHECK(source.IsConstant()) << source;
    HConstant* constant = source.GetConstant();
    if (constant->IsIntConstant() || constant->IsNullConstant()) {
      int32_t value = CodeGenerator::GetInt32ValueOf(constant);
      if (destination.IsRegister()) {
        __ LoadImmediate(destination.AsRegister<Register>(), value);
      } else {
        DCHECK(destination.IsStackSlot());
        __ LoadImmediate(IP, value);
        __ StoreToOffset(kStoreWord, IP, SP, destination.GetStackIndex());
      }
    } else if (constant->IsLongConstant()) {
      int64_t value = constant->AsLongConstant()->GetValue();
      if (destination.IsRegisterPair()) {
        __ LoadImmediate(destination.AsRegisterPairLow<Register>(), Low32Bits(value));
        __ LoadImmediate(destination.AsRegisterPairHigh<Register>(), High32Bits(value));
      } else {
        DCHECK(destination.IsDoubleStackSlot()) << destination;
        __ LoadImmediate(IP, Low32Bits(value));
        __ StoreToOffset(kStoreWord, IP, SP, destination.GetStackIndex());
        __ LoadImmediate(IP, High32Bits(value));
        __ StoreToOffset(kStoreWord, IP, SP, destination.GetHighStackIndex(kArmWordSize));
      }
    } else if (constant->IsDoubleConstant()) {
      double value = constant->AsDoubleConstant()->GetValue();
      if (destination.IsFpuRegisterPair()) {
        __ LoadDImmediate(FromLowSToD(destination.AsFpuRegisterPairLow<SRegister>()), value);
      } else {
        DCHECK(destination.IsDoubleStackSlot()) << destination;
        uint64_t int_value = bit_cast<uint64_t, double>(value);
        __ LoadImmediate(IP, Low32Bits(int_value));
        __ StoreToOffset(kStoreWord, IP, SP, destination.GetStackIndex());
        __ LoadImmediate(IP, High32Bits(int_value));
        __ StoreToOffset(kStoreWord, IP, SP, destination.GetHighStackIndex(kArmWordSize));
      }
    } else {
      DCHECK(constant->IsFloatConstant()) << constant->DebugName();
      float value = constant->AsFloatConstant()->GetValue();
      if (destination.IsFpuRegister()) {
        __ LoadSImmediate(destination.AsFpuRegister<SRegister>(), value);
      } else {
        DCHECK(destination.IsStackSlot());
        __ LoadImmediate(IP, bit_cast<int32_t, float>(value));
        __ StoreToOffset(kStoreWord, IP, SP, destination.GetStackIndex());
      }
    }
  }
}

void ParallelMoveResolverARM::Exchange(Register reg, int mem) {
  __ Mov(IP, reg);
  __ LoadFromOffset(kLoadWord, reg, SP, mem);
  __ StoreToOffset(kStoreWord, IP, SP, mem);
}

void ParallelMoveResolverARM::Exchange(int mem1, int mem2) {
  ScratchRegisterScope ensure_scratch(this, IP, R0, codegen_->GetNumberOfCoreRegisters());
  int stack_offset = ensure_scratch.IsSpilled() ? kArmWordSize : 0;
  __ LoadFromOffset(kLoadWord, static_cast<Register>(ensure_scratch.GetRegister()),
                    SP, mem1 + stack_offset);
  __ LoadFromOffset(kLoadWord, IP, SP, mem2 + stack_offset);
  __ StoreToOffset(kStoreWord, static_cast<Register>(ensure_scratch.GetRegister()),
                   SP, mem2 + stack_offset);
  __ StoreToOffset(kStoreWord, IP, SP, mem1 + stack_offset);
}

void ParallelMoveResolverARM::EmitSwap(size_t index) {
  MoveOperands* move = moves_.Get(index);
  Location source = move->GetSource();
  Location destination = move->GetDestination();

  if (source.IsRegister() && destination.IsRegister()) {
    DCHECK_NE(source.AsRegister<Register>(), IP);
    DCHECK_NE(destination.AsRegister<Register>(), IP);
    __ Mov(IP, source.AsRegister<Register>());
    __ Mov(source.AsRegister<Register>(), destination.AsRegister<Register>());
    __ Mov(destination.AsRegister<Register>(), IP);
  } else if (source.IsRegister() && destination.IsStackSlot()) {
    Exchange(source.AsRegister<Register>(), destination.GetStackIndex());
  } else if (source.IsStackSlot() && destination.IsRegister()) {
    Exchange(destination.AsRegister<Register>(), source.GetStackIndex());
  } else if (source.IsStackSlot() && destination.IsStackSlot()) {
    Exchange(source.GetStackIndex(), destination.GetStackIndex());
  } else if (source.IsFpuRegister() && destination.IsFpuRegister()) {
    __ vmovrs(IP, source.AsFpuRegister<SRegister>());
    __ vmovs(source.AsFpuRegister<SRegister>(), destination.AsFpuRegister<SRegister>());
    __ vmovsr(destination.AsFpuRegister<SRegister>(), IP);
  } else if (source.IsRegisterPair() && destination.IsRegisterPair()) {
    __ vmovdrr(DTMP, source.AsRegisterPairLow<Register>(), source.AsRegisterPairHigh<Register>());
    __ Mov(source.AsRegisterPairLow<Register>(), destination.AsRegisterPairLow<Register>());
    __ Mov(source.AsRegisterPairHigh<Register>(), destination.AsRegisterPairHigh<Register>());
    __ vmovrrd(destination.AsRegisterPairLow<Register>(),
               destination.AsRegisterPairHigh<Register>(),
               DTMP);
  } else if (source.IsRegisterPair() || destination.IsRegisterPair()) {
    Register low_reg = source.IsRegisterPair()
        ? source.AsRegisterPairLow<Register>()
        : destination.AsRegisterPairLow<Register>();
    int mem = source.IsRegisterPair()
        ? destination.GetStackIndex()
        : source.GetStackIndex();
    DCHECK(ExpectedPairLayout(source.IsRegisterPair() ? source : destination));
    __ vmovdrr(DTMP, low_reg, static_cast<Register>(low_reg + 1));
    __ LoadFromOffset(kLoadWordPair, low_reg, SP, mem);
    __ StoreDToOffset(DTMP, SP, mem);
  } else if (source.IsFpuRegisterPair() && destination.IsFpuRegisterPair()) {
    DRegister first = FromLowSToD(source.AsFpuRegisterPairLow<SRegister>());
    DRegister second = FromLowSToD(destination.AsFpuRegisterPairLow<SRegister>());
    __ vmovd(DTMP, first);
    __ vmovd(first, second);
    __ vmovd(second, DTMP);
  } else if (source.IsFpuRegisterPair() || destination.IsFpuRegisterPair()) {
    DRegister reg = source.IsFpuRegisterPair()
        ? FromLowSToD(source.AsFpuRegisterPairLow<SRegister>())
        : FromLowSToD(destination.AsFpuRegisterPairLow<SRegister>());
    int mem = source.IsFpuRegisterPair()
        ? destination.GetStackIndex()
        : source.GetStackIndex();
    __ vmovd(DTMP, reg);
    __ LoadDFromOffset(reg, SP, mem);
    __ StoreDToOffset(DTMP, SP, mem);
  } else if (source.IsFpuRegister() || destination.IsFpuRegister()) {
    SRegister reg = source.IsFpuRegister() ? source.AsFpuRegister<SRegister>()
                                           : destination.AsFpuRegister<SRegister>();
    int mem = source.IsFpuRegister()
        ? destination.GetStackIndex()
        : source.GetStackIndex();

    __ vmovrs(IP, reg);
    __ LoadSFromOffset(reg, SP, mem);
    __ StoreToOffset(kStoreWord, IP, SP, mem);
  } else if (source.IsDoubleStackSlot() && destination.IsDoubleStackSlot()) {
    Exchange(source.GetStackIndex(), destination.GetStackIndex());
    Exchange(source.GetHighStackIndex(kArmWordSize), destination.GetHighStackIndex(kArmWordSize));
  } else {
    LOG(FATAL) << "Unimplemented" << source << " <-> " << destination;
  }
}

void ParallelMoveResolverARM::SpillScratch(int reg) {
  __ Push(static_cast<Register>(reg));
}

void ParallelMoveResolverARM::RestoreScratch(int reg) {
  __ Pop(static_cast<Register>(reg));
}

void LocationsBuilderARM::VisitLoadClass(HLoadClass* cls) {
  LocationSummary::CallKind call_kind = cls->CanCallRuntime()
      ? LocationSummary::kCallOnSlowPath
      : LocationSummary::kNoCall;
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(cls, call_kind);
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorARM::VisitLoadClass(HLoadClass* cls) {
  Register out = cls->GetLocations()->Out().AsRegister<Register>();
  if (cls->IsReferrersClass()) {
    DCHECK(!cls->CanCallRuntime());
    DCHECK(!cls->MustGenerateClinitCheck());
    codegen_->LoadCurrentMethod(out);
    __ LoadFromOffset(kLoadWord, out, out, ArtMethod::DeclaringClassOffset().Int32Value());
  } else {
    DCHECK(cls->CanCallRuntime());
    codegen_->LoadCurrentMethod(out);
    __ LoadFromOffset(
        kLoadWord, out, out, ArtMethod::DexCacheResolvedTypesOffset().Int32Value());
    __ LoadFromOffset(kLoadWord, out, out, CodeGenerator::GetCacheOffset(cls->GetTypeIndex()));

    SlowPathCodeARM* slow_path = new (GetGraph()->GetArena()) LoadClassSlowPathARM(
        cls, cls, cls->GetDexPc(), cls->MustGenerateClinitCheck());
    codegen_->AddSlowPath(slow_path);
    __ cmp(out, ShifterOperand(0));
    __ b(slow_path->GetEntryLabel(), EQ);
    if (cls->MustGenerateClinitCheck()) {
      GenerateClassInitializationCheck(slow_path, out);
    } else {
      __ Bind(slow_path->GetExitLabel());
    }
  }
}

void LocationsBuilderARM::VisitClinitCheck(HClinitCheck* check) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(check, LocationSummary::kCallOnSlowPath);
  locations->SetInAt(0, Location::RequiresRegister());
  if (check->HasUses()) {
    locations->SetOut(Location::SameAsFirstInput());
  }
}

void InstructionCodeGeneratorARM::VisitClinitCheck(HClinitCheck* check) {
  // We assume the class is not null.
  SlowPathCodeARM* slow_path = new (GetGraph()->GetArena()) LoadClassSlowPathARM(
      check->GetLoadClass(), check, check->GetDexPc(), true);
  codegen_->AddSlowPath(slow_path);
  GenerateClassInitializationCheck(slow_path,
                                   check->GetLocations()->InAt(0).AsRegister<Register>());
}

void InstructionCodeGeneratorARM::GenerateClassInitializationCheck(
    SlowPathCodeARM* slow_path, Register class_reg) {
  __ LoadFromOffset(kLoadWord, IP, class_reg, mirror::Class::StatusOffset().Int32Value());
  __ cmp(IP, ShifterOperand(mirror::Class::kStatusInitialized));
  __ b(slow_path->GetEntryLabel(), LT);
  // Even if the initialized flag is set, we may be in a situation where caches are not synced
  // properly. Therefore, we do a memory fence.
  __ dmb(ISH);
  __ Bind(slow_path->GetExitLabel());
}

void LocationsBuilderARM::VisitLoadString(HLoadString* load) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(load, LocationSummary::kCallOnSlowPath);
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorARM::VisitLoadString(HLoadString* load) {
  SlowPathCodeARM* slow_path = new (GetGraph()->GetArena()) LoadStringSlowPathARM(load);
  codegen_->AddSlowPath(slow_path);

  Register out = load->GetLocations()->Out().AsRegister<Register>();
  codegen_->LoadCurrentMethod(out);
  __ LoadFromOffset(kLoadWord, out, out, ArtMethod::DeclaringClassOffset().Int32Value());
  __ LoadFromOffset(kLoadWord, out, out, mirror::Class::DexCacheStringsOffset().Int32Value());
  __ LoadFromOffset(kLoadWord, out, out, CodeGenerator::GetCacheOffset(load->GetStringIndex()));
  __ cmp(out, ShifterOperand(0));
  __ b(slow_path->GetEntryLabel(), EQ);
  __ Bind(slow_path->GetExitLabel());
}

void LocationsBuilderARM::VisitLoadException(HLoadException* load) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(load, LocationSummary::kNoCall);
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorARM::VisitLoadException(HLoadException* load) {
  Register out = load->GetLocations()->Out().AsRegister<Register>();
  int32_t offset = Thread::ExceptionOffset<kArmWordSize>().Int32Value();
  __ LoadFromOffset(kLoadWord, out, TR, offset);
  __ LoadImmediate(IP, 0);
  __ StoreToOffset(kStoreWord, IP, TR, offset);
}

void LocationsBuilderARM::VisitThrow(HThrow* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCall);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
}

void InstructionCodeGeneratorARM::VisitThrow(HThrow* instruction) {
  codegen_->InvokeRuntime(
      QUICK_ENTRY_POINT(pDeliverException), instruction, instruction->GetDexPc(), nullptr);
}

void LocationsBuilderARM::VisitInstanceOf(HInstanceOf* instruction) {
  LocationSummary::CallKind call_kind = instruction->IsClassFinal()
      ? LocationSummary::kNoCall
      : LocationSummary::kCallOnSlowPath;
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction, call_kind);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  // The out register is used as a temporary, so it overlaps with the inputs.
  locations->SetOut(Location::RequiresRegister(), Location::kOutputOverlap);
}

void InstructionCodeGeneratorARM::VisitInstanceOf(HInstanceOf* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Register obj = locations->InAt(0).AsRegister<Register>();
  Register cls = locations->InAt(1).AsRegister<Register>();
  Register out = locations->Out().AsRegister<Register>();
  uint32_t class_offset = mirror::Object::ClassOffset().Int32Value();
  Label done, zero;
  SlowPathCodeARM* slow_path = nullptr;

  // Return 0 if `obj` is null.
  // avoid null check if we know obj is not null.
  if (instruction->MustDoNullCheck()) {
    __ cmp(obj, ShifterOperand(0));
    __ b(&zero, EQ);
  }
  // Compare the class of `obj` with `cls`.
  __ LoadFromOffset(kLoadWord, out, obj, class_offset);
  __ cmp(out, ShifterOperand(cls));
  if (instruction->IsClassFinal()) {
    // Classes must be equal for the instanceof to succeed.
    __ b(&zero, NE);
    __ LoadImmediate(out, 1);
    __ b(&done);
  } else {
    // If the classes are not equal, we go into a slow path.
    DCHECK(locations->OnlyCallsOnSlowPath());
    slow_path = new (GetGraph()->GetArena()) TypeCheckSlowPathARM(
        instruction, locations->InAt(1), locations->Out(), instruction->GetDexPc());
    codegen_->AddSlowPath(slow_path);
    __ b(slow_path->GetEntryLabel(), NE);
    __ LoadImmediate(out, 1);
    __ b(&done);
  }

  if (instruction->MustDoNullCheck() || instruction->IsClassFinal()) {
    __ Bind(&zero);
    __ LoadImmediate(out, 0);
  }

  if (slow_path != nullptr) {
    __ Bind(slow_path->GetExitLabel());
  }
  __ Bind(&done);
}

void LocationsBuilderARM::VisitCheckCast(HCheckCast* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(
      instruction, LocationSummary::kCallOnSlowPath);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  locations->AddTemp(Location::RequiresRegister());
}

void InstructionCodeGeneratorARM::VisitCheckCast(HCheckCast* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Register obj = locations->InAt(0).AsRegister<Register>();
  Register cls = locations->InAt(1).AsRegister<Register>();
  Register temp = locations->GetTemp(0).AsRegister<Register>();
  uint32_t class_offset = mirror::Object::ClassOffset().Int32Value();

  SlowPathCodeARM* slow_path = new (GetGraph()->GetArena()) TypeCheckSlowPathARM(
      instruction, locations->InAt(1), locations->GetTemp(0), instruction->GetDexPc());
  codegen_->AddSlowPath(slow_path);

  // avoid null check if we know obj is not null.
  if (instruction->MustDoNullCheck()) {
    __ cmp(obj, ShifterOperand(0));
    __ b(slow_path->GetExitLabel(), EQ);
  }
  // Compare the class of `obj` with `cls`.
  __ LoadFromOffset(kLoadWord, temp, obj, class_offset);
  __ cmp(temp, ShifterOperand(cls));
  __ b(slow_path->GetEntryLabel(), NE);
  __ Bind(slow_path->GetExitLabel());
}

void LocationsBuilderARM::VisitMonitorOperation(HMonitorOperation* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCall);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
}

void InstructionCodeGeneratorARM::VisitMonitorOperation(HMonitorOperation* instruction) {
  codegen_->InvokeRuntime(instruction->IsEnter()
        ? QUICK_ENTRY_POINT(pLockObject) : QUICK_ENTRY_POINT(pUnlockObject),
      instruction,
      instruction->GetDexPc(),
      nullptr);
}

void LocationsBuilderARM::VisitAnd(HAnd* instruction) { HandleBitwiseOperation(instruction); }
void LocationsBuilderARM::VisitOr(HOr* instruction) { HandleBitwiseOperation(instruction); }
void LocationsBuilderARM::VisitXor(HXor* instruction) { HandleBitwiseOperation(instruction); }

void LocationsBuilderARM::HandleBitwiseOperation(HBinaryOperation* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  DCHECK(instruction->GetResultType() == Primitive::kPrimInt
         || instruction->GetResultType() == Primitive::kPrimLong);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

void InstructionCodeGeneratorARM::VisitAnd(HAnd* instruction) {
  HandleBitwiseOperation(instruction);
}

void InstructionCodeGeneratorARM::VisitOr(HOr* instruction) {
  HandleBitwiseOperation(instruction);
}

void InstructionCodeGeneratorARM::VisitXor(HXor* instruction) {
  HandleBitwiseOperation(instruction);
}

void InstructionCodeGeneratorARM::HandleBitwiseOperation(HBinaryOperation* instruction) {
  LocationSummary* locations = instruction->GetLocations();

  if (instruction->GetResultType() == Primitive::kPrimInt) {
    Register first = locations->InAt(0).AsRegister<Register>();
    Register second = locations->InAt(1).AsRegister<Register>();
    Register out = locations->Out().AsRegister<Register>();
    if (instruction->IsAnd()) {
      __ and_(out, first, ShifterOperand(second));
    } else if (instruction->IsOr()) {
      __ orr(out, first, ShifterOperand(second));
    } else {
      DCHECK(instruction->IsXor());
      __ eor(out, first, ShifterOperand(second));
    }
  } else {
    DCHECK_EQ(instruction->GetResultType(), Primitive::kPrimLong);
    Location first = locations->InAt(0);
    Location second = locations->InAt(1);
    Location out = locations->Out();
    if (instruction->IsAnd()) {
      __ and_(out.AsRegisterPairLow<Register>(),
              first.AsRegisterPairLow<Register>(),
              ShifterOperand(second.AsRegisterPairLow<Register>()));
      __ and_(out.AsRegisterPairHigh<Register>(),
              first.AsRegisterPairHigh<Register>(),
              ShifterOperand(second.AsRegisterPairHigh<Register>()));
    } else if (instruction->IsOr()) {
      __ orr(out.AsRegisterPairLow<Register>(),
             first.AsRegisterPairLow<Register>(),
             ShifterOperand(second.AsRegisterPairLow<Register>()));
      __ orr(out.AsRegisterPairHigh<Register>(),
             first.AsRegisterPairHigh<Register>(),
             ShifterOperand(second.AsRegisterPairHigh<Register>()));
    } else {
      DCHECK(instruction->IsXor());
      __ eor(out.AsRegisterPairLow<Register>(),
             first.AsRegisterPairLow<Register>(),
             ShifterOperand(second.AsRegisterPairLow<Register>()));
      __ eor(out.AsRegisterPairHigh<Register>(),
             first.AsRegisterPairHigh<Register>(),
             ShifterOperand(second.AsRegisterPairHigh<Register>()));
    }
  }
}

void CodeGeneratorARM::GenerateStaticOrDirectCall(HInvokeStaticOrDirect* invoke, Register temp) {
  DCHECK_EQ(temp, kArtMethodRegister);

  // TODO: Implement all kinds of calls:
  // 1) boot -> boot
  // 2) app -> boot
  // 3) app -> app
  //
  // Currently we implement the app -> app logic, which looks up in the resolve cache.

  if (invoke->IsStringInit()) {
    // temp = thread->string_init_entrypoint
    __ LoadFromOffset(kLoadWord, temp, TR, invoke->GetStringInitOffset());
    // LR = temp[offset_of_quick_compiled_code]
    __ LoadFromOffset(kLoadWord, LR, temp,
                      ArtMethod::EntryPointFromQuickCompiledCodeOffset(
                          kArmWordSize).Int32Value());
    // LR()
    __ blx(LR);
  } else {
    // temp = method;
    LoadCurrentMethod(temp);
    if (!invoke->IsRecursive()) {
      // temp = temp->dex_cache_resolved_methods_;
      __ LoadFromOffset(
          kLoadWord, temp, temp, ArtMethod::DexCacheResolvedMethodsOffset().Int32Value());
      // temp = temp[index_in_cache]
      __ LoadFromOffset(
          kLoadWord, temp, temp, CodeGenerator::GetCacheOffset(invoke->GetDexMethodIndex()));
      // LR = temp[offset_of_quick_compiled_code]
      __ LoadFromOffset(kLoadWord, LR, temp, ArtMethod::EntryPointFromQuickCompiledCodeOffset(
          kArmWordSize).Int32Value());
      // LR()
      __ blx(LR);
    } else {
      __ bl(GetFrameEntryLabel());
    }
  }

  DCHECK(!IsLeafMethod());
}

void LocationsBuilderARM::VisitBoundType(HBoundType* instruction) {
  // Nothing to do, this should be removed during prepare for register allocator.
  UNUSED(instruction);
  LOG(FATAL) << "Unreachable";
}

void InstructionCodeGeneratorARM::VisitBoundType(HBoundType* instruction) {
  // Nothing to do, this should be removed during prepare for register allocator.
  UNUSED(instruction);
  LOG(FATAL) << "Unreachable";
}

}  // namespace arm
}  // namespace art
