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

#include "code_generator_x86.h"

#include "art_method.h"
#include "code_generator_utils.h"
#include "entrypoints/quick/quick_entrypoints.h"
#include "entrypoints/quick/quick_entrypoints_enum.h"
#include "gc/accounting/card_table.h"
#include "intrinsics.h"
#include "intrinsics_x86.h"
#include "mirror/array-inl.h"
#include "mirror/class-inl.h"
#include "thread.h"
#include "utils/assembler.h"
#include "utils/stack_checks.h"
#include "utils/x86/assembler_x86.h"
#include "utils/x86/managed_register_x86.h"

namespace art {

namespace x86 {

static constexpr int kCurrentMethodStackOffset = 0;

static constexpr Register kCoreCalleeSaves[] = { EBP, ESI, EDI };

static constexpr int kC2ConditionMask = 0x400;

static constexpr int kFakeReturnRegister = Register(8);

#define __ reinterpret_cast<X86Assembler*>(codegen->GetAssembler())->

class NullCheckSlowPathX86 : public SlowPathCodeX86 {
 public:
  explicit NullCheckSlowPathX86(HNullCheck* instruction) : instruction_(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    __ Bind(GetEntryLabel());
    __ fs()->call(Address::Absolute(QUICK_ENTRYPOINT_OFFSET(kX86WordSize, pThrowNullPointer)));
    RecordPcInfo(codegen, instruction_, instruction_->GetDexPc());
  }

 private:
  HNullCheck* const instruction_;
  DISALLOW_COPY_AND_ASSIGN(NullCheckSlowPathX86);
};

class DivZeroCheckSlowPathX86 : public SlowPathCodeX86 {
 public:
  explicit DivZeroCheckSlowPathX86(HDivZeroCheck* instruction) : instruction_(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    __ Bind(GetEntryLabel());
    __ fs()->call(Address::Absolute(QUICK_ENTRYPOINT_OFFSET(kX86WordSize, pThrowDivZero)));
    RecordPcInfo(codegen, instruction_, instruction_->GetDexPc());
  }

 private:
  HDivZeroCheck* const instruction_;
  DISALLOW_COPY_AND_ASSIGN(DivZeroCheckSlowPathX86);
};

class DivRemMinusOneSlowPathX86 : public SlowPathCodeX86 {
 public:
  explicit DivRemMinusOneSlowPathX86(Register reg, bool is_div) : reg_(reg), is_div_(is_div) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    __ Bind(GetEntryLabel());
    if (is_div_) {
      __ negl(reg_);
    } else {
      __ movl(reg_, Immediate(0));
    }
    __ jmp(GetExitLabel());
  }

 private:
  Register reg_;
  bool is_div_;
  DISALLOW_COPY_AND_ASSIGN(DivRemMinusOneSlowPathX86);
};

class BoundsCheckSlowPathX86 : public SlowPathCodeX86 {
 public:
  BoundsCheckSlowPathX86(HBoundsCheck* instruction,
                         Location index_location,
                         Location length_location)
      : instruction_(instruction),
        index_location_(index_location),
        length_location_(length_location) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    CodeGeneratorX86* x86_codegen = down_cast<CodeGeneratorX86*>(codegen);
    __ Bind(GetEntryLabel());
    // We're moving two locations to locations that could overlap, so we need a parallel
    // move resolver.
    InvokeRuntimeCallingConvention calling_convention;
    x86_codegen->EmitParallelMoves(
        index_location_,
        Location::RegisterLocation(calling_convention.GetRegisterAt(0)),
        Primitive::kPrimInt,
        length_location_,
        Location::RegisterLocation(calling_convention.GetRegisterAt(1)),
        Primitive::kPrimInt);
    __ fs()->call(Address::Absolute(QUICK_ENTRYPOINT_OFFSET(kX86WordSize, pThrowArrayBounds)));
    RecordPcInfo(codegen, instruction_, instruction_->GetDexPc());
  }

 private:
  HBoundsCheck* const instruction_;
  const Location index_location_;
  const Location length_location_;

  DISALLOW_COPY_AND_ASSIGN(BoundsCheckSlowPathX86);
};

class SuspendCheckSlowPathX86 : public SlowPathCodeX86 {
 public:
  SuspendCheckSlowPathX86(HSuspendCheck* instruction, HBasicBlock* successor)
      : instruction_(instruction), successor_(successor) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    CodeGeneratorX86* x86_codegen = down_cast<CodeGeneratorX86*>(codegen);
    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, instruction_->GetLocations());
    __ fs()->call(Address::Absolute(QUICK_ENTRYPOINT_OFFSET(kX86WordSize, pTestSuspend)));
    RecordPcInfo(codegen, instruction_, instruction_->GetDexPc());
    RestoreLiveRegisters(codegen, instruction_->GetLocations());
    if (successor_ == nullptr) {
      __ jmp(GetReturnLabel());
    } else {
      __ jmp(x86_codegen->GetLabelOf(successor_));
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
  HBasicBlock* const successor_;
  Label return_label_;

  DISALLOW_COPY_AND_ASSIGN(SuspendCheckSlowPathX86);
};

class LoadStringSlowPathX86 : public SlowPathCodeX86 {
 public:
  explicit LoadStringSlowPathX86(HLoadString* instruction) : instruction_(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    LocationSummary* locations = instruction_->GetLocations();
    DCHECK(!locations->GetLiveRegisters()->ContainsCoreRegister(locations->Out().reg()));

    CodeGeneratorX86* x86_codegen = down_cast<CodeGeneratorX86*>(codegen);
    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, locations);

    InvokeRuntimeCallingConvention calling_convention;
    __ movl(calling_convention.GetRegisterAt(0), Immediate(instruction_->GetStringIndex()));
    __ fs()->call(Address::Absolute(QUICK_ENTRYPOINT_OFFSET(kX86WordSize, pResolveString)));
    RecordPcInfo(codegen, instruction_, instruction_->GetDexPc());
    x86_codegen->Move32(locations->Out(), Location::RegisterLocation(EAX));
    RestoreLiveRegisters(codegen, locations);

    __ jmp(GetExitLabel());
  }

 private:
  HLoadString* const instruction_;

  DISALLOW_COPY_AND_ASSIGN(LoadStringSlowPathX86);
};

class LoadClassSlowPathX86 : public SlowPathCodeX86 {
 public:
  LoadClassSlowPathX86(HLoadClass* cls,
                       HInstruction* at,
                       uint32_t dex_pc,
                       bool do_clinit)
      : cls_(cls), at_(at), dex_pc_(dex_pc), do_clinit_(do_clinit) {
    DCHECK(at->IsLoadClass() || at->IsClinitCheck());
  }

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    LocationSummary* locations = at_->GetLocations();
    CodeGeneratorX86* x86_codegen = down_cast<CodeGeneratorX86*>(codegen);
    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, locations);

    InvokeRuntimeCallingConvention calling_convention;
    __ movl(calling_convention.GetRegisterAt(0), Immediate(cls_->GetTypeIndex()));
    __ fs()->call(Address::Absolute(do_clinit_
        ? QUICK_ENTRYPOINT_OFFSET(kX86WordSize, pInitializeStaticStorage)
        : QUICK_ENTRYPOINT_OFFSET(kX86WordSize, pInitializeType)));
    RecordPcInfo(codegen, at_, dex_pc_);

    // Move the class to the desired location.
    Location out = locations->Out();
    if (out.IsValid()) {
      DCHECK(out.IsRegister() && !locations->GetLiveRegisters()->ContainsCoreRegister(out.reg()));
      x86_codegen->Move32(out, Location::RegisterLocation(EAX));
    }

    RestoreLiveRegisters(codegen, locations);
    __ jmp(GetExitLabel());
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

  DISALLOW_COPY_AND_ASSIGN(LoadClassSlowPathX86);
};

class TypeCheckSlowPathX86 : public SlowPathCodeX86 {
 public:
  TypeCheckSlowPathX86(HInstruction* instruction,
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

    CodeGeneratorX86* x86_codegen = down_cast<CodeGeneratorX86*>(codegen);
    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, locations);

    // We're moving two locations to locations that could overlap, so we need a parallel
    // move resolver.
    InvokeRuntimeCallingConvention calling_convention;
    x86_codegen->EmitParallelMoves(
        class_to_check_,
        Location::RegisterLocation(calling_convention.GetRegisterAt(0)),
        Primitive::kPrimNot,
        object_class_,
        Location::RegisterLocation(calling_convention.GetRegisterAt(1)),
        Primitive::kPrimNot);

    if (instruction_->IsInstanceOf()) {
      __ fs()->call(Address::Absolute(QUICK_ENTRYPOINT_OFFSET(kX86WordSize,
                                                              pInstanceofNonTrivial)));
    } else {
      DCHECK(instruction_->IsCheckCast());
      __ fs()->call(Address::Absolute(QUICK_ENTRYPOINT_OFFSET(kX86WordSize, pCheckCast)));
    }

    RecordPcInfo(codegen, instruction_, dex_pc_);
    if (instruction_->IsInstanceOf()) {
      x86_codegen->Move32(locations->Out(), Location::RegisterLocation(EAX));
    }
    RestoreLiveRegisters(codegen, locations);

    __ jmp(GetExitLabel());
  }

 private:
  HInstruction* const instruction_;
  const Location class_to_check_;
  const Location object_class_;
  const uint32_t dex_pc_;

  DISALLOW_COPY_AND_ASSIGN(TypeCheckSlowPathX86);
};

class DeoptimizationSlowPathX86 : public SlowPathCodeX86 {
 public:
  explicit DeoptimizationSlowPathX86(HInstruction* instruction)
    : instruction_(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, instruction_->GetLocations());
    __ fs()->call(Address::Absolute(QUICK_ENTRYPOINT_OFFSET(kX86WordSize, pDeoptimize)));
    // No need to restore live registers.
    DCHECK(instruction_->IsDeoptimize());
    HDeoptimize* deoptimize = instruction_->AsDeoptimize();
    uint32_t dex_pc = deoptimize->GetDexPc();
    codegen->RecordPcInfo(instruction_, dex_pc, this);
  }

 private:
  HInstruction* const instruction_;
  DISALLOW_COPY_AND_ASSIGN(DeoptimizationSlowPathX86);
};

#undef __
#define __ reinterpret_cast<X86Assembler*>(GetAssembler())->

inline Condition X86Condition(IfCondition cond) {
  switch (cond) {
    case kCondEQ: return kEqual;
    case kCondNE: return kNotEqual;
    case kCondLT: return kLess;
    case kCondLE: return kLessEqual;
    case kCondGT: return kGreater;
    case kCondGE: return kGreaterEqual;
    default:
      LOG(FATAL) << "Unknown if condition";
  }
  return kEqual;
}

void CodeGeneratorX86::DumpCoreRegister(std::ostream& stream, int reg) const {
  stream << X86ManagedRegister::FromCpuRegister(Register(reg));
}

void CodeGeneratorX86::DumpFloatingPointRegister(std::ostream& stream, int reg) const {
  stream << X86ManagedRegister::FromXmmRegister(XmmRegister(reg));
}

size_t CodeGeneratorX86::SaveCoreRegister(size_t stack_index, uint32_t reg_id) {
  __ movl(Address(ESP, stack_index), static_cast<Register>(reg_id));
  return kX86WordSize;
}

size_t CodeGeneratorX86::RestoreCoreRegister(size_t stack_index, uint32_t reg_id) {
  __ movl(static_cast<Register>(reg_id), Address(ESP, stack_index));
  return kX86WordSize;
}

size_t CodeGeneratorX86::SaveFloatingPointRegister(size_t stack_index, uint32_t reg_id) {
  __ movsd(Address(ESP, stack_index), XmmRegister(reg_id));
  return GetFloatingPointSpillSlotSize();
}

size_t CodeGeneratorX86::RestoreFloatingPointRegister(size_t stack_index, uint32_t reg_id) {
  __ movsd(XmmRegister(reg_id), Address(ESP, stack_index));
  return GetFloatingPointSpillSlotSize();
}

CodeGeneratorX86::CodeGeneratorX86(HGraph* graph,
                   const X86InstructionSetFeatures& isa_features,
                   const CompilerOptions& compiler_options)
    : CodeGenerator(graph,
                    kNumberOfCpuRegisters,
                    kNumberOfXmmRegisters,
                    kNumberOfRegisterPairs,
                    ComputeRegisterMask(reinterpret_cast<const int*>(kCoreCalleeSaves),
                                        arraysize(kCoreCalleeSaves))
                        | (1 << kFakeReturnRegister),
                        0,
                        compiler_options),
      block_labels_(graph->GetArena(), 0),
      location_builder_(graph, this),
      instruction_visitor_(graph, this),
      move_resolver_(graph->GetArena(), this),
      isa_features_(isa_features) {
  // Use a fake return address register to mimic Quick.
  AddAllocatedRegister(Location::RegisterLocation(kFakeReturnRegister));
}

Location CodeGeneratorX86::AllocateFreeRegister(Primitive::Type type) const {
  switch (type) {
    case Primitive::kPrimLong: {
      size_t reg = FindFreeEntry(blocked_register_pairs_, kNumberOfRegisterPairs);
      X86ManagedRegister pair =
          X86ManagedRegister::FromRegisterPair(static_cast<RegisterPair>(reg));
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
      Register reg = static_cast<Register>(
          FindFreeEntry(blocked_core_registers_, kNumberOfCpuRegisters));
      // Block all register pairs that contain `reg`.
      for (int i = 0; i < kNumberOfRegisterPairs; i++) {
        X86ManagedRegister current =
            X86ManagedRegister::FromRegisterPair(static_cast<RegisterPair>(i));
        if (current.AsRegisterPairLow() == reg || current.AsRegisterPairHigh() == reg) {
          blocked_register_pairs_[i] = true;
        }
      }
      return Location::RegisterLocation(reg);
    }

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      return Location::FpuRegisterLocation(
          FindFreeEntry(blocked_fpu_registers_, kNumberOfXmmRegisters));
    }

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << type;
  }

  return Location();
}

void CodeGeneratorX86::SetupBlockedRegisters(bool is_baseline) const {
  // Don't allocate the dalvik style register pair passing.
  blocked_register_pairs_[ECX_EDX] = true;

  // Stack register is always reserved.
  blocked_core_registers_[ESP] = true;

  if (is_baseline) {
    blocked_core_registers_[EBP] = true;
    blocked_core_registers_[ESI] = true;
    blocked_core_registers_[EDI] = true;
  }

  UpdateBlockedPairRegisters();
}

void CodeGeneratorX86::UpdateBlockedPairRegisters() const {
  for (int i = 0; i < kNumberOfRegisterPairs; i++) {
    X86ManagedRegister current =
        X86ManagedRegister::FromRegisterPair(static_cast<RegisterPair>(i));
    if (blocked_core_registers_[current.AsRegisterPairLow()]
        || blocked_core_registers_[current.AsRegisterPairHigh()]) {
      blocked_register_pairs_[i] = true;
    }
  }
}

InstructionCodeGeneratorX86::InstructionCodeGeneratorX86(HGraph* graph, CodeGeneratorX86* codegen)
      : HGraphVisitor(graph),
        assembler_(codegen->GetAssembler()),
        codegen_(codegen) {}

static dwarf::Reg DWARFReg(Register reg) {
  return dwarf::Reg::X86Core(static_cast<int>(reg));
}

void CodeGeneratorX86::GenerateFrameEntry() {
  __ cfi().SetCurrentCFAOffset(kX86WordSize);  // return address
  __ Bind(&frame_entry_label_);
  bool skip_overflow_check =
      IsLeafMethod() && !FrameNeedsStackCheck(GetFrameSize(), InstructionSet::kX86);
  DCHECK(GetCompilerOptions().GetImplicitStackOverflowChecks());

  if (!skip_overflow_check) {
    __ testl(EAX, Address(ESP, -static_cast<int32_t>(GetStackOverflowReservedBytes(kX86))));
    RecordPcInfo(nullptr, 0);
  }

  if (HasEmptyFrame()) {
    return;
  }

  for (int i = arraysize(kCoreCalleeSaves) - 1; i >= 0; --i) {
    Register reg = kCoreCalleeSaves[i];
    if (allocated_registers_.ContainsCoreRegister(reg)) {
      __ pushl(reg);
      __ cfi().AdjustCFAOffset(kX86WordSize);
      __ cfi().RelOffset(DWARFReg(reg), 0);
    }
  }

  int adjust = GetFrameSize() - FrameEntrySpillSize();
  __ subl(ESP, Immediate(adjust));
  __ cfi().AdjustCFAOffset(adjust);
  __ movl(Address(ESP, kCurrentMethodStackOffset), EAX);
}

void CodeGeneratorX86::GenerateFrameExit() {
  __ cfi().RememberState();
  if (!HasEmptyFrame()) {
    int adjust = GetFrameSize() - FrameEntrySpillSize();
    __ addl(ESP, Immediate(adjust));
    __ cfi().AdjustCFAOffset(-adjust);

    for (size_t i = 0; i < arraysize(kCoreCalleeSaves); ++i) {
      Register reg = kCoreCalleeSaves[i];
      if (allocated_registers_.ContainsCoreRegister(reg)) {
        __ popl(reg);
        __ cfi().AdjustCFAOffset(-static_cast<int>(kX86WordSize));
        __ cfi().Restore(DWARFReg(reg));
      }
    }
  }
  __ ret();
  __ cfi().RestoreState();
  __ cfi().DefCFAOffset(GetFrameSize());
}

void CodeGeneratorX86::Bind(HBasicBlock* block) {
  __ Bind(GetLabelOf(block));
}

void CodeGeneratorX86::LoadCurrentMethod(Register reg) {
  DCHECK(RequiresCurrentMethod());
  __ movl(reg, Address(ESP, kCurrentMethodStackOffset));
}

Location CodeGeneratorX86::GetStackLocation(HLoadLocal* load) const {
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

Location InvokeDexCallingConventionVisitorX86::GetNextLocation(Primitive::Type type) {
  switch (type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      uint32_t index = gp_index_++;
      stack_index_++;
      if (index < calling_convention.GetNumberOfRegisters()) {
        return Location::RegisterLocation(calling_convention.GetRegisterAt(index));
      } else {
        return Location::StackSlot(calling_convention.GetStackOffsetOf(stack_index_ - 1));
      }
    }

    case Primitive::kPrimLong: {
      uint32_t index = gp_index_;
      gp_index_ += 2;
      stack_index_ += 2;
      if (index + 1 < calling_convention.GetNumberOfRegisters()) {
        X86ManagedRegister pair = X86ManagedRegister::FromRegisterPair(
            calling_convention.GetRegisterPairAt(index));
        return Location::RegisterPairLocation(pair.AsRegisterPairLow(), pair.AsRegisterPairHigh());
      } else {
        return Location::DoubleStackSlot(calling_convention.GetStackOffsetOf(stack_index_ - 2));
      }
    }

    case Primitive::kPrimFloat: {
      uint32_t index = float_index_++;
      stack_index_++;
      if (index < calling_convention.GetNumberOfFpuRegisters()) {
        return Location::FpuRegisterLocation(calling_convention.GetFpuRegisterAt(index));
      } else {
        return Location::StackSlot(calling_convention.GetStackOffsetOf(stack_index_ - 1));
      }
    }

    case Primitive::kPrimDouble: {
      uint32_t index = float_index_++;
      stack_index_ += 2;
      if (index < calling_convention.GetNumberOfFpuRegisters()) {
        return Location::FpuRegisterLocation(calling_convention.GetFpuRegisterAt(index));
      } else {
        return Location::DoubleStackSlot(calling_convention.GetStackOffsetOf(stack_index_ - 2));
      }
    }

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unexpected parameter type " << type;
      break;
  }
  return Location();
}

void CodeGeneratorX86::Move32(Location destination, Location source) {
  if (source.Equals(destination)) {
    return;
  }
  if (destination.IsRegister()) {
    if (source.IsRegister()) {
      __ movl(destination.AsRegister<Register>(), source.AsRegister<Register>());
    } else if (source.IsFpuRegister()) {
      __ movd(destination.AsRegister<Register>(), source.AsFpuRegister<XmmRegister>());
    } else {
      DCHECK(source.IsStackSlot());
      __ movl(destination.AsRegister<Register>(), Address(ESP, source.GetStackIndex()));
    }
  } else if (destination.IsFpuRegister()) {
    if (source.IsRegister()) {
      __ movd(destination.AsFpuRegister<XmmRegister>(), source.AsRegister<Register>());
    } else if (source.IsFpuRegister()) {
      __ movaps(destination.AsFpuRegister<XmmRegister>(), source.AsFpuRegister<XmmRegister>());
    } else {
      DCHECK(source.IsStackSlot());
      __ movss(destination.AsFpuRegister<XmmRegister>(), Address(ESP, source.GetStackIndex()));
    }
  } else {
    DCHECK(destination.IsStackSlot()) << destination;
    if (source.IsRegister()) {
      __ movl(Address(ESP, destination.GetStackIndex()), source.AsRegister<Register>());
    } else if (source.IsFpuRegister()) {
      __ movss(Address(ESP, destination.GetStackIndex()), source.AsFpuRegister<XmmRegister>());
    } else if (source.IsConstant()) {
      HConstant* constant = source.GetConstant();
      int32_t value = GetInt32ValueOf(constant);
      __ movl(Address(ESP, destination.GetStackIndex()), Immediate(value));
    } else {
      DCHECK(source.IsStackSlot());
      __ pushl(Address(ESP, source.GetStackIndex()));
      __ popl(Address(ESP, destination.GetStackIndex()));
    }
  }
}

void CodeGeneratorX86::Move64(Location destination, Location source) {
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
      LOG(FATAL) << "Unimplemented";
    } else {
      // No conflict possible, so just do the moves.
      DCHECK(source.IsDoubleStackSlot());
      __ movl(destination.AsRegisterPairLow<Register>(), Address(ESP, source.GetStackIndex()));
      __ movl(destination.AsRegisterPairHigh<Register>(),
              Address(ESP, source.GetHighStackIndex(kX86WordSize)));
    }
  } else if (destination.IsFpuRegister()) {
    if (source.IsFpuRegister()) {
      __ movaps(destination.AsFpuRegister<XmmRegister>(), source.AsFpuRegister<XmmRegister>());
    } else if (source.IsDoubleStackSlot()) {
      __ movsd(destination.AsFpuRegister<XmmRegister>(), Address(ESP, source.GetStackIndex()));
    } else {
      LOG(FATAL) << "Unimplemented";
    }
  } else {
    DCHECK(destination.IsDoubleStackSlot()) << destination;
    if (source.IsRegisterPair()) {
      // No conflict possible, so just do the moves.
      __ movl(Address(ESP, destination.GetStackIndex()), source.AsRegisterPairLow<Register>());
      __ movl(Address(ESP, destination.GetHighStackIndex(kX86WordSize)),
              source.AsRegisterPairHigh<Register>());
    } else if (source.IsFpuRegister()) {
      __ movsd(Address(ESP, destination.GetStackIndex()), source.AsFpuRegister<XmmRegister>());
    } else if (source.IsConstant()) {
      HConstant* constant = source.GetConstant();
      int64_t value;
      if (constant->IsLongConstant()) {
        value = constant->AsLongConstant()->GetValue();
      } else {
        DCHECK(constant->IsDoubleConstant());
        value = bit_cast<int64_t, double>(constant->AsDoubleConstant()->GetValue());
      }
      __ movl(Address(ESP, destination.GetStackIndex()), Immediate(Low32Bits(value)));
      __ movl(Address(ESP, destination.GetHighStackIndex(kX86WordSize)), Immediate(High32Bits(value)));
    } else {
      DCHECK(source.IsDoubleStackSlot()) << source;
      EmitParallelMoves(
          Location::StackSlot(source.GetStackIndex()),
          Location::StackSlot(destination.GetStackIndex()),
          Primitive::kPrimInt,
          Location::StackSlot(source.GetHighStackIndex(kX86WordSize)),
          Location::StackSlot(destination.GetHighStackIndex(kX86WordSize)),
          Primitive::kPrimInt);
    }
  }
}

void CodeGeneratorX86::Move(HInstruction* instruction, Location location, HInstruction* move_for) {
  LocationSummary* locations = instruction->GetLocations();
  if (locations != nullptr && locations->Out().Equals(location)) {
    return;
  }

  if (locations != nullptr && locations->Out().IsConstant()) {
    HConstant* const_to_move = locations->Out().GetConstant();
    if (const_to_move->IsIntConstant() || const_to_move->IsNullConstant()) {
      Immediate imm(GetInt32ValueOf(const_to_move));
      if (location.IsRegister()) {
        __ movl(location.AsRegister<Register>(), imm);
      } else if (location.IsStackSlot()) {
        __ movl(Address(ESP, location.GetStackIndex()), imm);
      } else {
        DCHECK(location.IsConstant());
        DCHECK_EQ(location.GetConstant(), const_to_move);
      }
    } else if (const_to_move->IsLongConstant()) {
      int64_t value = const_to_move->AsLongConstant()->GetValue();
      if (location.IsRegisterPair()) {
        __ movl(location.AsRegisterPairLow<Register>(), Immediate(Low32Bits(value)));
        __ movl(location.AsRegisterPairHigh<Register>(), Immediate(High32Bits(value)));
      } else if (location.IsDoubleStackSlot()) {
        __ movl(Address(ESP, location.GetStackIndex()), Immediate(Low32Bits(value)));
        __ movl(Address(ESP, location.GetHighStackIndex(kX86WordSize)),
                Immediate(High32Bits(value)));
      } else {
        DCHECK(location.IsConstant());
        DCHECK_EQ(location.GetConstant(), instruction);
      }
    }
  } else if (instruction->IsTemporary()) {
    Location temp_location = GetTemporaryLocation(instruction->AsTemporary());
    if (temp_location.IsStackSlot()) {
      Move32(location, temp_location);
    } else {
      DCHECK(temp_location.IsDoubleStackSlot());
      Move64(location, temp_location);
    }
  } else if (instruction->IsLoadLocal()) {
    int slot = GetStackSlot(instruction->AsLoadLocal()->GetLocal());
    switch (instruction->GetType()) {
      case Primitive::kPrimBoolean:
      case Primitive::kPrimByte:
      case Primitive::kPrimChar:
      case Primitive::kPrimShort:
      case Primitive::kPrimInt:
      case Primitive::kPrimNot:
      case Primitive::kPrimFloat:
        Move32(location, Location::StackSlot(slot));
        break;

      case Primitive::kPrimLong:
      case Primitive::kPrimDouble:
        Move64(location, Location::DoubleStackSlot(slot));
        break;

      default:
        LOG(FATAL) << "Unimplemented local type " << instruction->GetType();
    }
  } else {
    DCHECK((instruction->GetNext() == move_for) || instruction->GetNext()->IsTemporary());
    switch (instruction->GetType()) {
      case Primitive::kPrimBoolean:
      case Primitive::kPrimByte:
      case Primitive::kPrimChar:
      case Primitive::kPrimShort:
      case Primitive::kPrimInt:
      case Primitive::kPrimNot:
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

void LocationsBuilderX86::VisitGoto(HGoto* got) {
  got->SetLocations(nullptr);
}

void InstructionCodeGeneratorX86::VisitGoto(HGoto* got) {
  HBasicBlock* successor = got->GetSuccessor();
  DCHECK(!successor->IsExitBlock());

  HBasicBlock* block = got->GetBlock();
  HInstruction* previous = got->GetPrevious();

  HLoopInformation* info = block->GetLoopInformation();
  if (info != nullptr && info->IsBackEdge(*block) && info->HasSuspendCheck()) {
    GenerateSuspendCheck(info->GetSuspendCheck(), successor);
    return;
  }

  if (block->IsEntryBlock() && (previous != nullptr) && previous->IsSuspendCheck()) {
    GenerateSuspendCheck(previous->AsSuspendCheck(), nullptr);
  }
  if (!codegen_->GoesToNextBlock(got->GetBlock(), successor)) {
    __ jmp(codegen_->GetLabelOf(successor));
  }
}

void LocationsBuilderX86::VisitExit(HExit* exit) {
  exit->SetLocations(nullptr);
}

void InstructionCodeGeneratorX86::VisitExit(HExit* exit) {
  UNUSED(exit);
}

void InstructionCodeGeneratorX86::GenerateTestAndBranch(HInstruction* instruction,
                                                        Label* true_target,
                                                        Label* false_target,
                                                        Label* always_true_target) {
  HInstruction* cond = instruction->InputAt(0);
  if (cond->IsIntConstant()) {
    // Constant condition, statically compared against 1.
    int32_t cond_value = cond->AsIntConstant()->GetValue();
    if (cond_value == 1) {
      if (always_true_target != nullptr) {
        __ jmp(always_true_target);
      }
      return;
    } else {
      DCHECK_EQ(cond_value, 0);
    }
  } else {
    bool materialized =
        !cond->IsCondition() || cond->AsCondition()->NeedsMaterialization();
    // Moves do not affect the eflags register, so if the condition is
    // evaluated just before the if, we don't need to evaluate it
    // again.
    bool eflags_set = cond->IsCondition()
        && cond->AsCondition()->IsBeforeWhenDisregardMoves(instruction);
    if (materialized) {
      if (!eflags_set) {
        // Materialized condition, compare against 0.
        Location lhs = instruction->GetLocations()->InAt(0);
        if (lhs.IsRegister()) {
          __ testl(lhs.AsRegister<Register>(), lhs.AsRegister<Register>());
        } else {
          __ cmpl(Address(ESP, lhs.GetStackIndex()), Immediate(0));
        }
        __ j(kNotEqual, true_target);
      } else {
        __ j(X86Condition(cond->AsCondition()->GetCondition()), true_target);
      }
    } else {
      Location lhs = cond->GetLocations()->InAt(0);
      Location rhs = cond->GetLocations()->InAt(1);
      // LHS is guaranteed to be in a register (see
      // LocationsBuilderX86::VisitCondition).
      if (rhs.IsRegister()) {
        __ cmpl(lhs.AsRegister<Register>(), rhs.AsRegister<Register>());
      } else if (rhs.IsConstant()) {
        int32_t constant = CodeGenerator::GetInt32ValueOf(rhs.GetConstant());
        if (constant == 0) {
          __ testl(lhs.AsRegister<Register>(), lhs.AsRegister<Register>());
        } else {
          __ cmpl(lhs.AsRegister<Register>(), Immediate(constant));
        }
      } else {
        __ cmpl(lhs.AsRegister<Register>(), Address(ESP, rhs.GetStackIndex()));
      }
      __ j(X86Condition(cond->AsCondition()->GetCondition()), true_target);
    }
  }
  if (false_target != nullptr) {
    __ jmp(false_target);
  }
}

void LocationsBuilderX86::VisitIf(HIf* if_instr) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(if_instr, LocationSummary::kNoCall);
  HInstruction* cond = if_instr->InputAt(0);
  if (!cond->IsCondition() || cond->AsCondition()->NeedsMaterialization()) {
    locations->SetInAt(0, Location::Any());
  }
}

void InstructionCodeGeneratorX86::VisitIf(HIf* if_instr) {
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

void LocationsBuilderX86::VisitDeoptimize(HDeoptimize* deoptimize) {
  LocationSummary* locations = new (GetGraph()->GetArena())
      LocationSummary(deoptimize, LocationSummary::kCallOnSlowPath);
  HInstruction* cond = deoptimize->InputAt(0);
  DCHECK(cond->IsCondition());
  if (cond->AsCondition()->NeedsMaterialization()) {
    locations->SetInAt(0, Location::Any());
  }
}

void InstructionCodeGeneratorX86::VisitDeoptimize(HDeoptimize* deoptimize) {
  SlowPathCodeX86* slow_path = new (GetGraph()->GetArena())
      DeoptimizationSlowPathX86(deoptimize);
  codegen_->AddSlowPath(slow_path);
  Label* slow_path_entry = slow_path->GetEntryLabel();
  GenerateTestAndBranch(deoptimize, slow_path_entry, nullptr, slow_path_entry);
}

void LocationsBuilderX86::VisitLocal(HLocal* local) {
  local->SetLocations(nullptr);
}

void InstructionCodeGeneratorX86::VisitLocal(HLocal* local) {
  DCHECK_EQ(local->GetBlock(), GetGraph()->GetEntryBlock());
}

void LocationsBuilderX86::VisitLoadLocal(HLoadLocal* local) {
  local->SetLocations(nullptr);
}

void InstructionCodeGeneratorX86::VisitLoadLocal(HLoadLocal* load) {
  // Nothing to do, this is driven by the code generator.
  UNUSED(load);
}

void LocationsBuilderX86::VisitStoreLocal(HStoreLocal* store) {
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
      LOG(FATAL) << "Unknown local type " << store->InputAt(1)->GetType();
  }
  store->SetLocations(locations);
}

void InstructionCodeGeneratorX86::VisitStoreLocal(HStoreLocal* store) {
  UNUSED(store);
}

void LocationsBuilderX86::VisitCondition(HCondition* comp) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(comp, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::Any());
  if (comp->NeedsMaterialization()) {
    // We need a byte register.
    locations->SetOut(Location::RegisterLocation(ECX));
  }
}

void InstructionCodeGeneratorX86::VisitCondition(HCondition* comp) {
  if (comp->NeedsMaterialization()) {
    LocationSummary* locations = comp->GetLocations();
    Register reg = locations->Out().AsRegister<Register>();
    // Clear register: setcc only sets the low byte.
    __ xorl(reg, reg);
    Location lhs = locations->InAt(0);
    Location rhs = locations->InAt(1);
    if (rhs.IsRegister()) {
      __ cmpl(lhs.AsRegister<Register>(), rhs.AsRegister<Register>());
    } else if (rhs.IsConstant()) {
      int32_t constant = CodeGenerator::GetInt32ValueOf(rhs.GetConstant());
      if (constant == 0) {
        __ testl(lhs.AsRegister<Register>(), lhs.AsRegister<Register>());
      } else {
      __ cmpl(lhs.AsRegister<Register>(), Immediate(constant));
      }
    } else {
      __ cmpl(lhs.AsRegister<Register>(), Address(ESP, rhs.GetStackIndex()));
    }
    __ setb(X86Condition(comp->GetCondition()), reg);
  }
}

void LocationsBuilderX86::VisitEqual(HEqual* comp) {
  VisitCondition(comp);
}

void InstructionCodeGeneratorX86::VisitEqual(HEqual* comp) {
  VisitCondition(comp);
}

void LocationsBuilderX86::VisitNotEqual(HNotEqual* comp) {
  VisitCondition(comp);
}

void InstructionCodeGeneratorX86::VisitNotEqual(HNotEqual* comp) {
  VisitCondition(comp);
}

void LocationsBuilderX86::VisitLessThan(HLessThan* comp) {
  VisitCondition(comp);
}

void InstructionCodeGeneratorX86::VisitLessThan(HLessThan* comp) {
  VisitCondition(comp);
}

void LocationsBuilderX86::VisitLessThanOrEqual(HLessThanOrEqual* comp) {
  VisitCondition(comp);
}

void InstructionCodeGeneratorX86::VisitLessThanOrEqual(HLessThanOrEqual* comp) {
  VisitCondition(comp);
}

void LocationsBuilderX86::VisitGreaterThan(HGreaterThan* comp) {
  VisitCondition(comp);
}

void InstructionCodeGeneratorX86::VisitGreaterThan(HGreaterThan* comp) {
  VisitCondition(comp);
}

void LocationsBuilderX86::VisitGreaterThanOrEqual(HGreaterThanOrEqual* comp) {
  VisitCondition(comp);
}

void InstructionCodeGeneratorX86::VisitGreaterThanOrEqual(HGreaterThanOrEqual* comp) {
  VisitCondition(comp);
}

void LocationsBuilderX86::VisitIntConstant(HIntConstant* constant) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(constant, LocationSummary::kNoCall);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorX86::VisitIntConstant(HIntConstant* constant) {
  // Will be generated at use site.
  UNUSED(constant);
}

void LocationsBuilderX86::VisitNullConstant(HNullConstant* constant) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(constant, LocationSummary::kNoCall);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorX86::VisitNullConstant(HNullConstant* constant) {
  // Will be generated at use site.
  UNUSED(constant);
}

void LocationsBuilderX86::VisitLongConstant(HLongConstant* constant) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(constant, LocationSummary::kNoCall);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorX86::VisitLongConstant(HLongConstant* constant) {
  // Will be generated at use site.
  UNUSED(constant);
}

void LocationsBuilderX86::VisitFloatConstant(HFloatConstant* constant) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(constant, LocationSummary::kNoCall);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorX86::VisitFloatConstant(HFloatConstant* constant) {
  // Will be generated at use site.
  UNUSED(constant);
}

void LocationsBuilderX86::VisitDoubleConstant(HDoubleConstant* constant) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(constant, LocationSummary::kNoCall);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorX86::VisitDoubleConstant(HDoubleConstant* constant) {
  // Will be generated at use site.
  UNUSED(constant);
}

void LocationsBuilderX86::VisitMemoryBarrier(HMemoryBarrier* memory_barrier) {
  memory_barrier->SetLocations(nullptr);
}

void InstructionCodeGeneratorX86::VisitMemoryBarrier(HMemoryBarrier* memory_barrier) {
  GenerateMemoryBarrier(memory_barrier->GetBarrierKind());
}

void LocationsBuilderX86::VisitReturnVoid(HReturnVoid* ret) {
  ret->SetLocations(nullptr);
}

void InstructionCodeGeneratorX86::VisitReturnVoid(HReturnVoid* ret) {
  UNUSED(ret);
  codegen_->GenerateFrameExit();
}

void LocationsBuilderX86::VisitReturn(HReturn* ret) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(ret, LocationSummary::kNoCall);
  switch (ret->InputAt(0)->GetType()) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimInt:
    case Primitive::kPrimNot:
      locations->SetInAt(0, Location::RegisterLocation(EAX));
      break;

    case Primitive::kPrimLong:
      locations->SetInAt(
          0, Location::RegisterPairLocation(EAX, EDX));
      break;

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      locations->SetInAt(
          0, Location::FpuRegisterLocation(XMM0));
      break;

    default:
      LOG(FATAL) << "Unknown return type " << ret->InputAt(0)->GetType();
  }
}

void InstructionCodeGeneratorX86::VisitReturn(HReturn* ret) {
  if (kIsDebugBuild) {
    switch (ret->InputAt(0)->GetType()) {
      case Primitive::kPrimBoolean:
      case Primitive::kPrimByte:
      case Primitive::kPrimChar:
      case Primitive::kPrimShort:
      case Primitive::kPrimInt:
      case Primitive::kPrimNot:
        DCHECK_EQ(ret->GetLocations()->InAt(0).AsRegister<Register>(), EAX);
        break;

      case Primitive::kPrimLong:
        DCHECK_EQ(ret->GetLocations()->InAt(0).AsRegisterPairLow<Register>(), EAX);
        DCHECK_EQ(ret->GetLocations()->InAt(0).AsRegisterPairHigh<Register>(), EDX);
        break;

      case Primitive::kPrimFloat:
      case Primitive::kPrimDouble:
        DCHECK_EQ(ret->GetLocations()->InAt(0).AsFpuRegister<XmmRegister>(), XMM0);
        break;

      default:
        LOG(FATAL) << "Unknown return type " << ret->InputAt(0)->GetType();
    }
  }
  codegen_->GenerateFrameExit();
}

void LocationsBuilderX86::VisitInvokeStaticOrDirect(HInvokeStaticOrDirect* invoke) {
  // When we do not run baseline, explicit clinit checks triggered by static
  // invokes must have been pruned by art::PrepareForRegisterAllocation.
  DCHECK(codegen_->IsBaseline() || !invoke->IsStaticWithExplicitClinitCheck());

  IntrinsicLocationsBuilderX86 intrinsic(codegen_);
  if (intrinsic.TryDispatch(invoke)) {
    return;
  }

  HandleInvoke(invoke);
}

static bool TryGenerateIntrinsicCode(HInvoke* invoke, CodeGeneratorX86* codegen) {
  if (invoke->GetLocations()->Intrinsified()) {
    IntrinsicCodeGeneratorX86 intrinsic(codegen);
    intrinsic.Dispatch(invoke);
    return true;
  }
  return false;
}

void InstructionCodeGeneratorX86::VisitInvokeStaticOrDirect(HInvokeStaticOrDirect* invoke) {
  // When we do not run baseline, explicit clinit checks triggered by static
  // invokes must have been pruned by art::PrepareForRegisterAllocation.
  DCHECK(codegen_->IsBaseline() || !invoke->IsStaticWithExplicitClinitCheck());

  if (TryGenerateIntrinsicCode(invoke, codegen_)) {
    return;
  }

  codegen_->GenerateStaticOrDirectCall(
      invoke, invoke->GetLocations()->GetTemp(0).AsRegister<Register>());
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
}

void LocationsBuilderX86::VisitInvokeVirtual(HInvokeVirtual* invoke) {
  HandleInvoke(invoke);
}

void LocationsBuilderX86::HandleInvoke(HInvoke* invoke) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(invoke, LocationSummary::kCall);
  locations->AddTemp(Location::RegisterLocation(EAX));

  InvokeDexCallingConventionVisitorX86 calling_convention_visitor;
  for (size_t i = 0; i < invoke->GetNumberOfArguments(); i++) {
    HInstruction* input = invoke->InputAt(i);
    locations->SetInAt(i, calling_convention_visitor.GetNextLocation(input->GetType()));
  }

  switch (invoke->GetType()) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimInt:
    case Primitive::kPrimNot:
      locations->SetOut(Location::RegisterLocation(EAX));
      break;

    case Primitive::kPrimLong:
      locations->SetOut(Location::RegisterPairLocation(EAX, EDX));
      break;

    case Primitive::kPrimVoid:
      break;

    case Primitive::kPrimDouble:
    case Primitive::kPrimFloat:
      locations->SetOut(Location::FpuRegisterLocation(XMM0));
      break;
  }

  invoke->SetLocations(locations);
}

void InstructionCodeGeneratorX86::VisitInvokeVirtual(HInvokeVirtual* invoke) {
  Register temp = invoke->GetLocations()->GetTemp(0).AsRegister<Register>();
  uint32_t method_offset = mirror::Class::EmbeddedVTableEntryOffset(
      invoke->GetVTableIndex(), kX86PointerSize).Uint32Value();
  LocationSummary* locations = invoke->GetLocations();
  Location receiver = locations->InAt(0);
  uint32_t class_offset = mirror::Object::ClassOffset().Int32Value();
  // temp = object->GetClass();
  if (receiver.IsStackSlot()) {
    __ movl(temp, Address(ESP, receiver.GetStackIndex()));
    __ movl(temp, Address(temp, class_offset));
  } else {
    __ movl(temp, Address(receiver.AsRegister<Register>(), class_offset));
  }
  codegen_->MaybeRecordImplicitNullCheck(invoke);
  // temp = temp->GetMethodAt(method_offset);
  __ movl(temp, Address(temp, method_offset));
  // call temp->GetEntryPoint();
  __ call(Address(
      temp, ArtMethod::EntryPointFromQuickCompiledCodeOffset(kX86WordSize).Int32Value()));

  DCHECK(!codegen_->IsLeafMethod());
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
}

void LocationsBuilderX86::VisitInvokeInterface(HInvokeInterface* invoke) {
  HandleInvoke(invoke);
  // Add the hidden argument.
  invoke->GetLocations()->AddTemp(Location::FpuRegisterLocation(XMM7));
}

void InstructionCodeGeneratorX86::VisitInvokeInterface(HInvokeInterface* invoke) {
  // TODO: b/18116999, our IMTs can miss an IncompatibleClassChangeError.
  Register temp = invoke->GetLocations()->GetTemp(0).AsRegister<Register>();
  uint32_t method_offset = mirror::Class::EmbeddedImTableEntryOffset(
      invoke->GetImtIndex() % mirror::Class::kImtSize, kX86PointerSize).Uint32Value();
  LocationSummary* locations = invoke->GetLocations();
  Location receiver = locations->InAt(0);
  uint32_t class_offset = mirror::Object::ClassOffset().Int32Value();

  // Set the hidden argument.
  __ movl(temp, Immediate(invoke->GetDexMethodIndex()));
  __ movd(invoke->GetLocations()->GetTemp(1).AsFpuRegister<XmmRegister>(), temp);

  // temp = object->GetClass();
  if (receiver.IsStackSlot()) {
    __ movl(temp, Address(ESP, receiver.GetStackIndex()));
    __ movl(temp, Address(temp, class_offset));
  } else {
    __ movl(temp, Address(receiver.AsRegister<Register>(), class_offset));
  }
    codegen_->MaybeRecordImplicitNullCheck(invoke);
  // temp = temp->GetImtEntryAt(method_offset);
  __ movl(temp, Address(temp, method_offset));
  // call temp->GetEntryPoint();
  __ call(Address(temp, ArtMethod::EntryPointFromQuickCompiledCodeOffset(
      kX86WordSize).Int32Value()));

  DCHECK(!codegen_->IsLeafMethod());
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
}

void LocationsBuilderX86::VisitNeg(HNeg* neg) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(neg, LocationSummary::kNoCall);
  switch (neg->GetResultType()) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong:
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetOut(Location::SameAsFirstInput());
      break;

    case Primitive::kPrimFloat:
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetOut(Location::SameAsFirstInput());
      locations->AddTemp(Location::RequiresRegister());
      locations->AddTemp(Location::RequiresFpuRegister());
      break;

    case Primitive::kPrimDouble:
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetOut(Location::SameAsFirstInput());
      locations->AddTemp(Location::RequiresFpuRegister());
      break;

    default:
      LOG(FATAL) << "Unexpected neg type " << neg->GetResultType();
  }
}

void InstructionCodeGeneratorX86::VisitNeg(HNeg* neg) {
  LocationSummary* locations = neg->GetLocations();
  Location out = locations->Out();
  Location in = locations->InAt(0);
  switch (neg->GetResultType()) {
    case Primitive::kPrimInt:
      DCHECK(in.IsRegister());
      DCHECK(in.Equals(out));
      __ negl(out.AsRegister<Register>());
      break;

    case Primitive::kPrimLong:
      DCHECK(in.IsRegisterPair());
      DCHECK(in.Equals(out));
      __ negl(out.AsRegisterPairLow<Register>());
      // Negation is similar to subtraction from zero.  The least
      // significant byte triggers a borrow when it is different from
      // zero; to take it into account, add 1 to the most significant
      // byte if the carry flag (CF) is set to 1 after the first NEGL
      // operation.
      __ adcl(out.AsRegisterPairHigh<Register>(), Immediate(0));
      __ negl(out.AsRegisterPairHigh<Register>());
      break;

    case Primitive::kPrimFloat: {
      DCHECK(in.Equals(out));
      Register constant = locations->GetTemp(0).AsRegister<Register>();
      XmmRegister mask = locations->GetTemp(1).AsFpuRegister<XmmRegister>();
      // Implement float negation with an exclusive or with value
      // 0x80000000 (mask for bit 31, representing the sign of a
      // single-precision floating-point number).
      __ movl(constant, Immediate(INT32_C(0x80000000)));
      __ movd(mask, constant);
      __ xorps(out.AsFpuRegister<XmmRegister>(), mask);
      break;
    }

    case Primitive::kPrimDouble: {
      DCHECK(in.Equals(out));
      XmmRegister mask = locations->GetTemp(0).AsFpuRegister<XmmRegister>();
      // Implement double negation with an exclusive or with value
      // 0x8000000000000000 (mask for bit 63, representing the sign of
      // a double-precision floating-point number).
      __ LoadLongConstant(mask, INT64_C(0x8000000000000000));
      __ xorpd(out.AsFpuRegister<XmmRegister>(), mask);
      break;
    }

    default:
      LOG(FATAL) << "Unexpected neg type " << neg->GetResultType();
  }
}

void LocationsBuilderX86::VisitTypeConversion(HTypeConversion* conversion) {
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
          locations->SetInAt(0, Location::ByteRegisterOrConstant(ECX, conversion->InputAt(0)));
          // Make the output overlap to please the register allocator. This greatly simplifies
          // the validation of the linear scan implementation
          locations->SetOut(Location::RequiresRegister(), Location::kOutputOverlap);
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
          locations->SetInAt(0, Location::Any());
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
          locations->SetInAt(0, Location::RegisterLocation(EAX));
          locations->SetOut(Location::RegisterPairLocation(EAX, EDX));
          break;

        case Primitive::kPrimFloat:
        case Primitive::kPrimDouble: {
          // Processing a Dex `float-to-long' or 'double-to-long' instruction.
          InvokeRuntimeCallingConvention calling_convention;
          XmmRegister parameter = calling_convention.GetFpuRegisterAt(0);
          locations->SetInAt(0, Location::FpuRegisterLocation(parameter));

          // The runtime helper puts the result in EAX, EDX.
          locations->SetOut(Location::RegisterPairLocation(EAX, EDX));
        }
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
          locations->SetInAt(0, Location::Any());
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
          locations->SetInAt(0, Location::Any());
          locations->SetOut(Location::Any());
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
          locations->SetInAt(0, Location::Any());
          locations->SetOut(Location::Any());
          break;

        case Primitive::kPrimFloat:
          // Processing a Dex `float-to-double' instruction.
          locations->SetInAt(0, Location::RequiresFpuRegister());
          locations->SetOut(Location::RequiresFpuRegister(), Location::kNoOutputOverlap);
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      }
      break;

    default:
      LOG(FATAL) << "Unexpected type conversion from " << input_type
                 << " to " << result_type;
  }
}

void InstructionCodeGeneratorX86::VisitTypeConversion(HTypeConversion* conversion) {
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
          if (in.IsRegister()) {
            __ movsxb(out.AsRegister<Register>(), in.AsRegister<ByteRegister>());
          } else {
            DCHECK(in.GetConstant()->IsIntConstant());
            int32_t value = in.GetConstant()->AsIntConstant()->GetValue();
            __ movl(out.AsRegister<Register>(), Immediate(static_cast<int8_t>(value)));
          }
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
          if (in.IsRegister()) {
            __ movsxw(out.AsRegister<Register>(), in.AsRegister<Register>());
          } else if (in.IsStackSlot()) {
            __ movsxw(out.AsRegister<Register>(), Address(ESP, in.GetStackIndex()));
          } else {
            DCHECK(in.GetConstant()->IsIntConstant());
            int32_t value = in.GetConstant()->AsIntConstant()->GetValue();
            __ movl(out.AsRegister<Register>(), Immediate(static_cast<int16_t>(value)));
          }
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
          if (in.IsRegisterPair()) {
            __ movl(out.AsRegister<Register>(), in.AsRegisterPairLow<Register>());
          } else if (in.IsDoubleStackSlot()) {
            __ movl(out.AsRegister<Register>(), Address(ESP, in.GetStackIndex()));
          } else {
            DCHECK(in.IsConstant());
            DCHECK(in.GetConstant()->IsLongConstant());
            int64_t value = in.GetConstant()->AsLongConstant()->GetValue();
            __ movl(out.AsRegister<Register>(), Immediate(static_cast<int32_t>(value)));
          }
          break;

        case Primitive::kPrimFloat: {
          // Processing a Dex `float-to-int' instruction.
          XmmRegister input = in.AsFpuRegister<XmmRegister>();
          Register output = out.AsRegister<Register>();
          XmmRegister temp = locations->GetTemp(0).AsFpuRegister<XmmRegister>();
          Label done, nan;

          __ movl(output, Immediate(kPrimIntMax));
          // temp = int-to-float(output)
          __ cvtsi2ss(temp, output);
          // if input >= temp goto done
          __ comiss(input, temp);
          __ j(kAboveEqual, &done);
          // if input == NaN goto nan
          __ j(kUnordered, &nan);
          // output = float-to-int-truncate(input)
          __ cvttss2si(output, input);
          __ jmp(&done);
          __ Bind(&nan);
          //  output = 0
          __ xorl(output, output);
          __ Bind(&done);
          break;
        }

        case Primitive::kPrimDouble: {
          // Processing a Dex `double-to-int' instruction.
          XmmRegister input = in.AsFpuRegister<XmmRegister>();
          Register output = out.AsRegister<Register>();
          XmmRegister temp = locations->GetTemp(0).AsFpuRegister<XmmRegister>();
          Label done, nan;

          __ movl(output, Immediate(kPrimIntMax));
          // temp = int-to-double(output)
          __ cvtsi2sd(temp, output);
          // if input >= temp goto done
          __ comisd(input, temp);
          __ j(kAboveEqual, &done);
          // if input == NaN goto nan
          __ j(kUnordered, &nan);
          // output = double-to-int-truncate(input)
          __ cvttsd2si(output, input);
          __ jmp(&done);
          __ Bind(&nan);
          //  output = 0
          __ xorl(output, output);
          __ Bind(&done);
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
          DCHECK_EQ(out.AsRegisterPairLow<Register>(), EAX);
          DCHECK_EQ(out.AsRegisterPairHigh<Register>(), EDX);
          DCHECK_EQ(in.AsRegister<Register>(), EAX);
          __ cdq();
          break;

        case Primitive::kPrimFloat:
          // Processing a Dex `float-to-long' instruction.
          __ fs()->call(Address::Absolute(QUICK_ENTRYPOINT_OFFSET(kX86WordSize, pF2l)));
          codegen_->RecordPcInfo(conversion, conversion->GetDexPc());
          break;

        case Primitive::kPrimDouble:
          // Processing a Dex `double-to-long' instruction.
          __ fs()->call(Address::Absolute(QUICK_ENTRYPOINT_OFFSET(kX86WordSize, pD2l)));
          codegen_->RecordPcInfo(conversion, conversion->GetDexPc());
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
          // Processing a Dex `Process a Dex `int-to-char'' instruction.
          if (in.IsRegister()) {
            __ movzxw(out.AsRegister<Register>(), in.AsRegister<Register>());
          } else if (in.IsStackSlot()) {
            __ movzxw(out.AsRegister<Register>(), Address(ESP, in.GetStackIndex()));
          } else {
            DCHECK(in.GetConstant()->IsIntConstant());
            int32_t value = in.GetConstant()->AsIntConstant()->GetValue();
            __ movl(out.AsRegister<Register>(), Immediate(static_cast<uint16_t>(value)));
          }
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
          __ cvtsi2ss(out.AsFpuRegister<XmmRegister>(), in.AsRegister<Register>());
          break;

        case Primitive::kPrimLong: {
          // Processing a Dex `long-to-float' instruction.
          size_t adjustment = 0;

          // Create stack space for the call to
          // InstructionCodeGeneratorX86::PushOntoFPStack and/or X86Assembler::fstps below.
          // TODO: enhance register allocator to ask for stack temporaries.
          if (!in.IsDoubleStackSlot() || !out.IsStackSlot()) {
            adjustment = Primitive::ComponentSize(Primitive::kPrimLong);
            __ subl(ESP, Immediate(adjustment));
          }

          // Load the value to the FP stack, using temporaries if needed.
          PushOntoFPStack(in, 0, adjustment, false, true);

          if (out.IsStackSlot()) {
            __ fstps(Address(ESP, out.GetStackIndex() + adjustment));
          } else {
            __ fstps(Address(ESP, 0));
            Location stack_temp = Location::StackSlot(0);
            codegen_->Move32(out, stack_temp);
          }

          // Remove the temporary stack space we allocated.
          if (adjustment != 0) {
            __ addl(ESP, Immediate(adjustment));
          }
          break;
        }

        case Primitive::kPrimDouble:
          // Processing a Dex `double-to-float' instruction.
          __ cvtsd2ss(out.AsFpuRegister<XmmRegister>(), in.AsFpuRegister<XmmRegister>());
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
          __ cvtsi2sd(out.AsFpuRegister<XmmRegister>(), in.AsRegister<Register>());
          break;

        case Primitive::kPrimLong: {
          // Processing a Dex `long-to-double' instruction.
          size_t adjustment = 0;

          // Create stack space for the call to
          // InstructionCodeGeneratorX86::PushOntoFPStack and/or X86Assembler::fstpl below.
          // TODO: enhance register allocator to ask for stack temporaries.
          if (!in.IsDoubleStackSlot() || !out.IsDoubleStackSlot()) {
            adjustment = Primitive::ComponentSize(Primitive::kPrimLong);
            __ subl(ESP, Immediate(adjustment));
          }

          // Load the value to the FP stack, using temporaries if needed.
          PushOntoFPStack(in, 0, adjustment, false, true);

          if (out.IsDoubleStackSlot()) {
            __ fstpl(Address(ESP, out.GetStackIndex() + adjustment));
          } else {
            __ fstpl(Address(ESP, 0));
            Location stack_temp = Location::DoubleStackSlot(0);
            codegen_->Move64(out, stack_temp);
          }

          // Remove the temporary stack space we allocated.
          if (adjustment != 0) {
            __ addl(ESP, Immediate(adjustment));
          }
          break;
        }

        case Primitive::kPrimFloat:
          // Processing a Dex `float-to-double' instruction.
          __ cvtss2sd(out.AsFpuRegister<XmmRegister>(), in.AsFpuRegister<XmmRegister>());
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

void LocationsBuilderX86::VisitAdd(HAdd* add) {
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
      locations->SetInAt(1, Location::Any());
      locations->SetOut(Location::SameAsFirstInput());
      break;
    }

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetInAt(1, Location::RequiresFpuRegister());
      locations->SetOut(Location::SameAsFirstInput());
      break;
    }

    default:
      LOG(FATAL) << "Unexpected add type " << add->GetResultType();
      break;
  }
}

void InstructionCodeGeneratorX86::VisitAdd(HAdd* add) {
  LocationSummary* locations = add->GetLocations();
  Location first = locations->InAt(0);
  Location second = locations->InAt(1);
  Location out = locations->Out();

  switch (add->GetResultType()) {
    case Primitive::kPrimInt: {
      if (second.IsRegister()) {
        if (out.AsRegister<Register>() == first.AsRegister<Register>()) {
          __ addl(out.AsRegister<Register>(), second.AsRegister<Register>());
        } else {
          __ leal(out.AsRegister<Register>(), Address(
              first.AsRegister<Register>(), second.AsRegister<Register>(), TIMES_1, 0));
          }
      } else if (second.IsConstant()) {
        int32_t value = second.GetConstant()->AsIntConstant()->GetValue();
        if (out.AsRegister<Register>() == first.AsRegister<Register>()) {
          __ addl(out.AsRegister<Register>(), Immediate(value));
        } else {
          __ leal(out.AsRegister<Register>(), Address(first.AsRegister<Register>(), value));
        }
      } else {
        DCHECK(first.Equals(locations->Out()));
        __ addl(first.AsRegister<Register>(), Address(ESP, second.GetStackIndex()));
      }
      break;
    }

    case Primitive::kPrimLong: {
      if (second.IsRegisterPair()) {
        __ addl(first.AsRegisterPairLow<Register>(), second.AsRegisterPairLow<Register>());
        __ adcl(first.AsRegisterPairHigh<Register>(), second.AsRegisterPairHigh<Register>());
      } else if (second.IsDoubleStackSlot()) {
        __ addl(first.AsRegisterPairLow<Register>(), Address(ESP, second.GetStackIndex()));
        __ adcl(first.AsRegisterPairHigh<Register>(),
                Address(ESP, second.GetHighStackIndex(kX86WordSize)));
      } else {
        DCHECK(second.IsConstant()) << second;
        int64_t value = second.GetConstant()->AsLongConstant()->GetValue();
        __ addl(first.AsRegisterPairLow<Register>(), Immediate(Low32Bits(value)));
        __ adcl(first.AsRegisterPairHigh<Register>(), Immediate(High32Bits(value)));
      }
      break;
    }

    case Primitive::kPrimFloat: {
      if (second.IsFpuRegister()) {
        __ addss(first.AsFpuRegister<XmmRegister>(), second.AsFpuRegister<XmmRegister>());
      }
      break;
    }

    case Primitive::kPrimDouble: {
      if (second.IsFpuRegister()) {
        __ addsd(first.AsFpuRegister<XmmRegister>(), second.AsFpuRegister<XmmRegister>());
      }
      break;
    }

    default:
      LOG(FATAL) << "Unexpected add type " << add->GetResultType();
  }
}

void LocationsBuilderX86::VisitSub(HSub* sub) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(sub, LocationSummary::kNoCall);
  switch (sub->GetResultType()) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong: {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::Any());
      locations->SetOut(Location::SameAsFirstInput());
      break;
    }
    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetInAt(1, Location::RequiresFpuRegister());
      locations->SetOut(Location::SameAsFirstInput());
      break;
    }

    default:
      LOG(FATAL) << "Unexpected sub type " << sub->GetResultType();
  }
}

void InstructionCodeGeneratorX86::VisitSub(HSub* sub) {
  LocationSummary* locations = sub->GetLocations();
  Location first = locations->InAt(0);
  Location second = locations->InAt(1);
  DCHECK(first.Equals(locations->Out()));
  switch (sub->GetResultType()) {
    case Primitive::kPrimInt: {
      if (second.IsRegister()) {
        __ subl(first.AsRegister<Register>(), second.AsRegister<Register>());
      } else if (second.IsConstant()) {
        __ subl(first.AsRegister<Register>(),
                Immediate(second.GetConstant()->AsIntConstant()->GetValue()));
      } else {
        __ subl(first.AsRegister<Register>(), Address(ESP, second.GetStackIndex()));
      }
      break;
    }

    case Primitive::kPrimLong: {
      if (second.IsRegisterPair()) {
        __ subl(first.AsRegisterPairLow<Register>(), second.AsRegisterPairLow<Register>());
        __ sbbl(first.AsRegisterPairHigh<Register>(), second.AsRegisterPairHigh<Register>());
      } else if (second.IsDoubleStackSlot()) {
        __ subl(first.AsRegisterPairLow<Register>(), Address(ESP, second.GetStackIndex()));
        __ sbbl(first.AsRegisterPairHigh<Register>(),
                Address(ESP, second.GetHighStackIndex(kX86WordSize)));
      } else {
        DCHECK(second.IsConstant()) << second;
        int64_t value = second.GetConstant()->AsLongConstant()->GetValue();
        __ subl(first.AsRegisterPairLow<Register>(), Immediate(Low32Bits(value)));
        __ sbbl(first.AsRegisterPairHigh<Register>(), Immediate(High32Bits(value)));
      }
      break;
    }

    case Primitive::kPrimFloat: {
      __ subss(first.AsFpuRegister<XmmRegister>(), second.AsFpuRegister<XmmRegister>());
      break;
    }

    case Primitive::kPrimDouble: {
      __ subsd(first.AsFpuRegister<XmmRegister>(), second.AsFpuRegister<XmmRegister>());
      break;
    }

    default:
      LOG(FATAL) << "Unexpected sub type " << sub->GetResultType();
  }
}

void LocationsBuilderX86::VisitMul(HMul* mul) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(mul, LocationSummary::kNoCall);
  switch (mul->GetResultType()) {
    case Primitive::kPrimInt:
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::Any());
      locations->SetOut(Location::SameAsFirstInput());
      break;
    case Primitive::kPrimLong: {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::Any());
      locations->SetOut(Location::SameAsFirstInput());
      // Needed for imul on 32bits with 64bits output.
      locations->AddTemp(Location::RegisterLocation(EAX));
      locations->AddTemp(Location::RegisterLocation(EDX));
      break;
    }
    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetInAt(1, Location::RequiresFpuRegister());
      locations->SetOut(Location::SameAsFirstInput());
      break;
    }

    default:
      LOG(FATAL) << "Unexpected mul type " << mul->GetResultType();
  }
}

void InstructionCodeGeneratorX86::VisitMul(HMul* mul) {
  LocationSummary* locations = mul->GetLocations();
  Location first = locations->InAt(0);
  Location second = locations->InAt(1);
  DCHECK(first.Equals(locations->Out()));

  switch (mul->GetResultType()) {
    case Primitive::kPrimInt: {
      if (second.IsRegister()) {
        __ imull(first.AsRegister<Register>(), second.AsRegister<Register>());
      } else if (second.IsConstant()) {
        Immediate imm(second.GetConstant()->AsIntConstant()->GetValue());
        __ imull(first.AsRegister<Register>(), imm);
      } else {
        DCHECK(second.IsStackSlot());
        __ imull(first.AsRegister<Register>(), Address(ESP, second.GetStackIndex()));
      }
      break;
    }

    case Primitive::kPrimLong: {
      Register in1_hi = first.AsRegisterPairHigh<Register>();
      Register in1_lo = first.AsRegisterPairLow<Register>();
      Register eax = locations->GetTemp(0).AsRegister<Register>();
      Register edx = locations->GetTemp(1).AsRegister<Register>();

      DCHECK_EQ(EAX, eax);
      DCHECK_EQ(EDX, edx);

      // input: in1 - 64 bits, in2 - 64 bits.
      // output: in1
      // formula: in1.hi : in1.lo = (in1.lo * in2.hi + in1.hi * in2.lo)* 2^32 + in1.lo * in2.lo
      // parts: in1.hi = in1.lo * in2.hi + in1.hi * in2.lo + (in1.lo * in2.lo)[63:32]
      // parts: in1.lo = (in1.lo * in2.lo)[31:0]
      if (second.IsConstant()) {
        DCHECK(second.GetConstant()->IsLongConstant());

        int64_t value = second.GetConstant()->AsLongConstant()->GetValue();
        int32_t low_value = Low32Bits(value);
        int32_t high_value = High32Bits(value);
        Immediate low(low_value);
        Immediate high(high_value);

        __ movl(eax, high);
        // eax <- in1.lo * in2.hi
        __ imull(eax, in1_lo);
        // in1.hi <- in1.hi * in2.lo
        __ imull(in1_hi, low);
        // in1.hi <- in1.lo * in2.hi + in1.hi * in2.lo
        __ addl(in1_hi, eax);
        // move in2_lo to eax to prepare for double precision
        __ movl(eax, low);
        // edx:eax <- in1.lo * in2.lo
        __ mull(in1_lo);
        // in1.hi <- in2.hi * in1.lo +  in2.lo * in1.hi + (in1.lo * in2.lo)[63:32]
        __ addl(in1_hi, edx);
        // in1.lo <- (in1.lo * in2.lo)[31:0];
        __ movl(in1_lo, eax);
      } else if (second.IsRegisterPair()) {
        Register in2_hi = second.AsRegisterPairHigh<Register>();
        Register in2_lo = second.AsRegisterPairLow<Register>();

        __ movl(eax, in2_hi);
        // eax <- in1.lo * in2.hi
        __ imull(eax, in1_lo);
        // in1.hi <- in1.hi * in2.lo
        __ imull(in1_hi, in2_lo);
        // in1.hi <- in1.lo * in2.hi + in1.hi * in2.lo
        __ addl(in1_hi, eax);
        // move in1_lo to eax to prepare for double precision
        __ movl(eax, in1_lo);
        // edx:eax <- in1.lo * in2.lo
        __ mull(in2_lo);
        // in1.hi <- in2.hi * in1.lo +  in2.lo * in1.hi + (in1.lo * in2.lo)[63:32]
        __ addl(in1_hi, edx);
        // in1.lo <- (in1.lo * in2.lo)[31:0];
        __ movl(in1_lo, eax);
      } else {
        DCHECK(second.IsDoubleStackSlot()) << second;
        Address in2_hi(ESP, second.GetHighStackIndex(kX86WordSize));
        Address in2_lo(ESP, second.GetStackIndex());

        __ movl(eax, in2_hi);
        // eax <- in1.lo * in2.hi
        __ imull(eax, in1_lo);
        // in1.hi <- in1.hi * in2.lo
        __ imull(in1_hi, in2_lo);
        // in1.hi <- in1.lo * in2.hi + in1.hi * in2.lo
        __ addl(in1_hi, eax);
        // move in1_lo to eax to prepare for double precision
        __ movl(eax, in1_lo);
        // edx:eax <- in1.lo * in2.lo
        __ mull(in2_lo);
        // in1.hi <- in2.hi * in1.lo +  in2.lo * in1.hi + (in1.lo * in2.lo)[63:32]
        __ addl(in1_hi, edx);
        // in1.lo <- (in1.lo * in2.lo)[31:0];
        __ movl(in1_lo, eax);
      }

      break;
    }

    case Primitive::kPrimFloat: {
      __ mulss(first.AsFpuRegister<XmmRegister>(), second.AsFpuRegister<XmmRegister>());
      break;
    }

    case Primitive::kPrimDouble: {
      __ mulsd(first.AsFpuRegister<XmmRegister>(), second.AsFpuRegister<XmmRegister>());
      break;
    }

    default:
      LOG(FATAL) << "Unexpected mul type " << mul->GetResultType();
  }
}

void InstructionCodeGeneratorX86::PushOntoFPStack(Location source,
                                                  uint32_t temp_offset,
                                                  uint32_t stack_adjustment,
                                                  bool is_fp,
                                                  bool is_wide) {
  if (source.IsStackSlot()) {
    DCHECK(!is_wide);
    if (is_fp) {
      __ flds(Address(ESP, source.GetStackIndex() + stack_adjustment));
    } else {
      __ filds(Address(ESP, source.GetStackIndex() + stack_adjustment));
    }
  } else if (source.IsDoubleStackSlot()) {
    DCHECK(is_wide);
    if (is_fp) {
      __ fldl(Address(ESP, source.GetStackIndex() + stack_adjustment));
    } else {
      __ fildl(Address(ESP, source.GetStackIndex() + stack_adjustment));
    }
  } else {
    // Write the value to the temporary location on the stack and load to FP stack.
    if (!is_wide) {
      Location stack_temp = Location::StackSlot(temp_offset);
      codegen_->Move32(stack_temp, source);
      if (is_fp) {
        __ flds(Address(ESP, temp_offset));
      } else {
        __ filds(Address(ESP, temp_offset));
      }
    } else {
      Location stack_temp = Location::DoubleStackSlot(temp_offset);
      codegen_->Move64(stack_temp, source);
      if (is_fp) {
        __ fldl(Address(ESP, temp_offset));
      } else {
        __ fildl(Address(ESP, temp_offset));
      }
    }
  }
}

void InstructionCodeGeneratorX86::GenerateRemFP(HRem *rem) {
  Primitive::Type type = rem->GetResultType();
  bool is_float = type == Primitive::kPrimFloat;
  size_t elem_size = Primitive::ComponentSize(type);
  LocationSummary* locations = rem->GetLocations();
  Location first = locations->InAt(0);
  Location second = locations->InAt(1);
  Location out = locations->Out();

  // Create stack space for 2 elements.
  // TODO: enhance register allocator to ask for stack temporaries.
  __ subl(ESP, Immediate(2 * elem_size));

  // Load the values to the FP stack in reverse order, using temporaries if needed.
  const bool is_wide = !is_float;
  PushOntoFPStack(second, elem_size, 2 * elem_size, /* is_fp */ true, is_wide);
  PushOntoFPStack(first, 0, 2 * elem_size, /* is_fp */ true, is_wide);

  // Loop doing FPREM until we stabilize.
  Label retry;
  __ Bind(&retry);
  __ fprem();

  // Move FP status to AX.
  __ fstsw();

  // And see if the argument reduction is complete. This is signaled by the
  // C2 FPU flag bit set to 0.
  __ andl(EAX, Immediate(kC2ConditionMask));
  __ j(kNotEqual, &retry);

  // We have settled on the final value. Retrieve it into an XMM register.
  // Store FP top of stack to real stack.
  if (is_float) {
    __ fsts(Address(ESP, 0));
  } else {
    __ fstl(Address(ESP, 0));
  }

  // Pop the 2 items from the FP stack.
  __ fucompp();

  // Load the value from the stack into an XMM register.
  DCHECK(out.IsFpuRegister()) << out;
  if (is_float) {
    __ movss(out.AsFpuRegister<XmmRegister>(), Address(ESP, 0));
  } else {
    __ movsd(out.AsFpuRegister<XmmRegister>(), Address(ESP, 0));
  }

  // And remove the temporary stack space we allocated.
  __ addl(ESP, Immediate(2 * elem_size));
}


void InstructionCodeGeneratorX86::DivRemOneOrMinusOne(HBinaryOperation* instruction) {
  DCHECK(instruction->IsDiv() || instruction->IsRem());

  LocationSummary* locations = instruction->GetLocations();
  DCHECK(locations->InAt(1).IsConstant());
  DCHECK(locations->InAt(1).GetConstant()->IsIntConstant());

  Register out_register = locations->Out().AsRegister<Register>();
  Register input_register = locations->InAt(0).AsRegister<Register>();
  int32_t imm = locations->InAt(1).GetConstant()->AsIntConstant()->GetValue();

  DCHECK(imm == 1 || imm == -1);

  if (instruction->IsRem()) {
    __ xorl(out_register, out_register);
  } else {
    __ movl(out_register, input_register);
    if (imm == -1) {
      __ negl(out_register);
    }
  }
}


void InstructionCodeGeneratorX86::DivByPowerOfTwo(HDiv* instruction) {
  LocationSummary* locations = instruction->GetLocations();

  Register out_register = locations->Out().AsRegister<Register>();
  Register input_register = locations->InAt(0).AsRegister<Register>();
  int32_t imm = locations->InAt(1).GetConstant()->AsIntConstant()->GetValue();

  DCHECK(IsPowerOfTwo(std::abs(imm)));
  Register num = locations->GetTemp(0).AsRegister<Register>();

  __ leal(num, Address(input_register, std::abs(imm) - 1));
  __ testl(input_register, input_register);
  __ cmovl(kGreaterEqual, num, input_register);
  int shift = CTZ(imm);
  __ sarl(num, Immediate(shift));

  if (imm < 0) {
    __ negl(num);
  }

  __ movl(out_register, num);
}

void InstructionCodeGeneratorX86::GenerateDivRemWithAnyConstant(HBinaryOperation* instruction) {
  DCHECK(instruction->IsDiv() || instruction->IsRem());

  LocationSummary* locations = instruction->GetLocations();
  int imm = locations->InAt(1).GetConstant()->AsIntConstant()->GetValue();

  Register eax = locations->InAt(0).AsRegister<Register>();
  Register out = locations->Out().AsRegister<Register>();
  Register num;
  Register edx;

  if (instruction->IsDiv()) {
    edx = locations->GetTemp(0).AsRegister<Register>();
    num = locations->GetTemp(1).AsRegister<Register>();
  } else {
    edx = locations->Out().AsRegister<Register>();
    num = locations->GetTemp(0).AsRegister<Register>();
  }

  DCHECK_EQ(EAX, eax);
  DCHECK_EQ(EDX, edx);
  if (instruction->IsDiv()) {
    DCHECK_EQ(EAX, out);
  } else {
    DCHECK_EQ(EDX, out);
  }

  int64_t magic;
  int shift;
  CalculateMagicAndShiftForDivRem(imm, false /* is_long */, &magic, &shift);

  Label ndiv;
  Label end;
  // If numerator is 0, the result is 0, no computation needed.
  __ testl(eax, eax);
  __ j(kNotEqual, &ndiv);

  __ xorl(out, out);
  __ jmp(&end);

  __ Bind(&ndiv);

  // Save the numerator.
  __ movl(num, eax);

  // EAX = magic
  __ movl(eax, Immediate(magic));

  // EDX:EAX = magic * numerator
  __ imull(num);

  if (imm > 0 && magic < 0) {
    // EDX += num
    __ addl(edx, num);
  } else if (imm < 0 && magic > 0) {
    __ subl(edx, num);
  }

  // Shift if needed.
  if (shift != 0) {
    __ sarl(edx, Immediate(shift));
  }

  // EDX += 1 if EDX < 0
  __ movl(eax, edx);
  __ shrl(edx, Immediate(31));
  __ addl(edx, eax);

  if (instruction->IsRem()) {
    __ movl(eax, num);
    __ imull(edx, Immediate(imm));
    __ subl(eax, edx);
    __ movl(edx, eax);
  } else {
    __ movl(eax, edx);
  }
  __ Bind(&end);
}

void InstructionCodeGeneratorX86::GenerateDivRemIntegral(HBinaryOperation* instruction) {
  DCHECK(instruction->IsDiv() || instruction->IsRem());

  LocationSummary* locations = instruction->GetLocations();
  Location out = locations->Out();
  Location first = locations->InAt(0);
  Location second = locations->InAt(1);
  bool is_div = instruction->IsDiv();

  switch (instruction->GetResultType()) {
    case Primitive::kPrimInt: {
      DCHECK_EQ(EAX, first.AsRegister<Register>());
      DCHECK_EQ(is_div ? EAX : EDX, out.AsRegister<Register>());

      if (instruction->InputAt(1)->IsIntConstant()) {
        int32_t imm = second.GetConstant()->AsIntConstant()->GetValue();

        if (imm == 0) {
          // Do not generate anything for 0. DivZeroCheck would forbid any generated code.
        } else if (imm == 1 || imm == -1) {
          DivRemOneOrMinusOne(instruction);
        } else if (is_div && IsPowerOfTwo(std::abs(imm))) {
          DivByPowerOfTwo(instruction->AsDiv());
        } else {
          DCHECK(imm <= -2 || imm >= 2);
          GenerateDivRemWithAnyConstant(instruction);
        }
      } else {
        SlowPathCodeX86* slow_path =
          new (GetGraph()->GetArena()) DivRemMinusOneSlowPathX86(out.AsRegister<Register>(),
              is_div);
        codegen_->AddSlowPath(slow_path);

        Register second_reg = second.AsRegister<Register>();
        // 0x80000000/-1 triggers an arithmetic exception!
        // Dividing by -1 is actually negation and -0x800000000 = 0x80000000 so
        // it's safe to just use negl instead of more complex comparisons.

        __ cmpl(second_reg, Immediate(-1));
        __ j(kEqual, slow_path->GetEntryLabel());

        // edx:eax <- sign-extended of eax
        __ cdq();
        // eax = quotient, edx = remainder
        __ idivl(second_reg);
        __ Bind(slow_path->GetExitLabel());
      }
      break;
    }

    case Primitive::kPrimLong: {
      InvokeRuntimeCallingConvention calling_convention;
      DCHECK_EQ(calling_convention.GetRegisterAt(0), first.AsRegisterPairLow<Register>());
      DCHECK_EQ(calling_convention.GetRegisterAt(1), first.AsRegisterPairHigh<Register>());
      DCHECK_EQ(calling_convention.GetRegisterAt(2), second.AsRegisterPairLow<Register>());
      DCHECK_EQ(calling_convention.GetRegisterAt(3), second.AsRegisterPairHigh<Register>());
      DCHECK_EQ(EAX, out.AsRegisterPairLow<Register>());
      DCHECK_EQ(EDX, out.AsRegisterPairHigh<Register>());

      if (is_div) {
        __ fs()->call(Address::Absolute(QUICK_ENTRYPOINT_OFFSET(kX86WordSize, pLdiv)));
      } else {
        __ fs()->call(Address::Absolute(QUICK_ENTRYPOINT_OFFSET(kX86WordSize, pLmod)));
      }
      uint32_t dex_pc = is_div
          ? instruction->AsDiv()->GetDexPc()
          : instruction->AsRem()->GetDexPc();
      codegen_->RecordPcInfo(instruction, dex_pc);

      break;
    }

    default:
      LOG(FATAL) << "Unexpected type for GenerateDivRemIntegral " << instruction->GetResultType();
  }
}

void LocationsBuilderX86::VisitDiv(HDiv* div) {
  LocationSummary::CallKind call_kind = (div->GetResultType() == Primitive::kPrimLong)
      ? LocationSummary::kCall
      : LocationSummary::kNoCall;
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(div, call_kind);

  switch (div->GetResultType()) {
    case Primitive::kPrimInt: {
      locations->SetInAt(0, Location::RegisterLocation(EAX));
      locations->SetInAt(1, Location::RegisterOrConstant(div->InputAt(1)));
      locations->SetOut(Location::SameAsFirstInput());
      // Intel uses edx:eax as the dividend.
      locations->AddTemp(Location::RegisterLocation(EDX));
      // We need to save the numerator while we tweak eax and edx. As we are using imul in a way
      // which enforces results to be in EAX and EDX, things are simpler if we use EAX also as
      // output and request another temp.
      if (div->InputAt(1)->IsIntConstant()) {
        locations->AddTemp(Location::RequiresRegister());
      }
      break;
    }
    case Primitive::kPrimLong: {
      InvokeRuntimeCallingConvention calling_convention;
      locations->SetInAt(0, Location::RegisterPairLocation(
          calling_convention.GetRegisterAt(0), calling_convention.GetRegisterAt(1)));
      locations->SetInAt(1, Location::RegisterPairLocation(
          calling_convention.GetRegisterAt(2), calling_convention.GetRegisterAt(3)));
      // Runtime helper puts the result in EAX, EDX.
      locations->SetOut(Location::RegisterPairLocation(EAX, EDX));
      break;
    }
    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetInAt(1, Location::RequiresFpuRegister());
      locations->SetOut(Location::SameAsFirstInput());
      break;
    }

    default:
      LOG(FATAL) << "Unexpected div type " << div->GetResultType();
  }
}

void InstructionCodeGeneratorX86::VisitDiv(HDiv* div) {
  LocationSummary* locations = div->GetLocations();
  Location out = locations->Out();
  Location first = locations->InAt(0);
  Location second = locations->InAt(1);

  switch (div->GetResultType()) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong: {
      GenerateDivRemIntegral(div);
      break;
    }

    case Primitive::kPrimFloat: {
      DCHECK(first.Equals(out));
      __ divss(first.AsFpuRegister<XmmRegister>(), second.AsFpuRegister<XmmRegister>());
      break;
    }

    case Primitive::kPrimDouble: {
      DCHECK(first.Equals(out));
      __ divsd(first.AsFpuRegister<XmmRegister>(), second.AsFpuRegister<XmmRegister>());
      break;
    }

    default:
      LOG(FATAL) << "Unexpected div type " << div->GetResultType();
  }
}

void LocationsBuilderX86::VisitRem(HRem* rem) {
  Primitive::Type type = rem->GetResultType();

  LocationSummary::CallKind call_kind = (rem->GetResultType() == Primitive::kPrimLong)
      ? LocationSummary::kCall
      : LocationSummary::kNoCall;
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(rem, call_kind);

  switch (type) {
    case Primitive::kPrimInt: {
      locations->SetInAt(0, Location::RegisterLocation(EAX));
      locations->SetInAt(1, Location::RegisterOrConstant(rem->InputAt(1)));
      locations->SetOut(Location::RegisterLocation(EDX));
      // We need to save the numerator while we tweak eax and edx. As we are using imul in a way
      // which enforces results to be in EAX and EDX, things are simpler if we use EDX also as
      // output and request another temp.
      if (rem->InputAt(1)->IsIntConstant()) {
        locations->AddTemp(Location::RequiresRegister());
      }
      break;
    }
    case Primitive::kPrimLong: {
      InvokeRuntimeCallingConvention calling_convention;
      locations->SetInAt(0, Location::RegisterPairLocation(
          calling_convention.GetRegisterAt(0), calling_convention.GetRegisterAt(1)));
      locations->SetInAt(1, Location::RegisterPairLocation(
          calling_convention.GetRegisterAt(2), calling_convention.GetRegisterAt(3)));
      // Runtime helper puts the result in EAX, EDX.
      locations->SetOut(Location::RegisterPairLocation(EAX, EDX));
      break;
    }
    case Primitive::kPrimDouble:
    case Primitive::kPrimFloat: {
      locations->SetInAt(0, Location::Any());
      locations->SetInAt(1, Location::Any());
      locations->SetOut(Location::RequiresFpuRegister());
      locations->AddTemp(Location::RegisterLocation(EAX));
      break;
    }

    default:
      LOG(FATAL) << "Unexpected rem type " << type;
  }
}

void InstructionCodeGeneratorX86::VisitRem(HRem* rem) {
  Primitive::Type type = rem->GetResultType();
  switch (type) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong: {
      GenerateDivRemIntegral(rem);
      break;
    }
    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      GenerateRemFP(rem);
      break;
    }
    default:
      LOG(FATAL) << "Unexpected rem type " << type;
  }
}

void LocationsBuilderX86::VisitDivZeroCheck(HDivZeroCheck* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  switch (instruction->GetType()) {
    case Primitive::kPrimInt: {
      locations->SetInAt(0, Location::Any());
      break;
    }
    case Primitive::kPrimLong: {
      locations->SetInAt(0, Location::RegisterOrConstant(instruction->InputAt(0)));
      if (!instruction->IsConstant()) {
        locations->AddTemp(Location::RequiresRegister());
      }
      break;
    }
    default:
      LOG(FATAL) << "Unexpected type for HDivZeroCheck " << instruction->GetType();
  }
  if (instruction->HasUses()) {
    locations->SetOut(Location::SameAsFirstInput());
  }
}

void InstructionCodeGeneratorX86::VisitDivZeroCheck(HDivZeroCheck* instruction) {
  SlowPathCodeX86* slow_path = new (GetGraph()->GetArena()) DivZeroCheckSlowPathX86(instruction);
  codegen_->AddSlowPath(slow_path);

  LocationSummary* locations = instruction->GetLocations();
  Location value = locations->InAt(0);

  switch (instruction->GetType()) {
    case Primitive::kPrimInt: {
      if (value.IsRegister()) {
        __ testl(value.AsRegister<Register>(), value.AsRegister<Register>());
        __ j(kEqual, slow_path->GetEntryLabel());
      } else if (value.IsStackSlot()) {
        __ cmpl(Address(ESP, value.GetStackIndex()), Immediate(0));
        __ j(kEqual, slow_path->GetEntryLabel());
      } else {
        DCHECK(value.IsConstant()) << value;
        if (value.GetConstant()->AsIntConstant()->GetValue() == 0) {
        __ jmp(slow_path->GetEntryLabel());
        }
      }
      break;
    }
    case Primitive::kPrimLong: {
      if (value.IsRegisterPair()) {
        Register temp = locations->GetTemp(0).AsRegister<Register>();
        __ movl(temp, value.AsRegisterPairLow<Register>());
        __ orl(temp, value.AsRegisterPairHigh<Register>());
        __ j(kEqual, slow_path->GetEntryLabel());
      } else {
        DCHECK(value.IsConstant()) << value;
        if (value.GetConstant()->AsLongConstant()->GetValue() == 0) {
          __ jmp(slow_path->GetEntryLabel());
        }
      }
      break;
    }
    default:
      LOG(FATAL) << "Unexpected type for HDivZeroCheck" << instruction->GetType();
  }
}

void LocationsBuilderX86::HandleShift(HBinaryOperation* op) {
  DCHECK(op->IsShl() || op->IsShr() || op->IsUShr());

  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(op, LocationSummary::kNoCall);

  switch (op->GetResultType()) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong: {
      // Can't have Location::Any() and output SameAsFirstInput()
      locations->SetInAt(0, Location::RequiresRegister());
      // The shift count needs to be in CL or a constant.
      locations->SetInAt(1, Location::ByteRegisterOrConstant(ECX, op->InputAt(1)));
      locations->SetOut(Location::SameAsFirstInput());
      break;
    }
    default:
      LOG(FATAL) << "Unexpected op type " << op->GetResultType();
  }
}

void InstructionCodeGeneratorX86::HandleShift(HBinaryOperation* op) {
  DCHECK(op->IsShl() || op->IsShr() || op->IsUShr());

  LocationSummary* locations = op->GetLocations();
  Location first = locations->InAt(0);
  Location second = locations->InAt(1);
  DCHECK(first.Equals(locations->Out()));

  switch (op->GetResultType()) {
    case Primitive::kPrimInt: {
      DCHECK(first.IsRegister());
      Register first_reg = first.AsRegister<Register>();
      if (second.IsRegister()) {
        Register second_reg = second.AsRegister<Register>();
        DCHECK_EQ(ECX, second_reg);
        if (op->IsShl()) {
          __ shll(first_reg, second_reg);
        } else if (op->IsShr()) {
          __ sarl(first_reg, second_reg);
        } else {
          __ shrl(first_reg, second_reg);
        }
      } else {
        int32_t shift = second.GetConstant()->AsIntConstant()->GetValue() & kMaxIntShiftValue;
        if (shift == 0) {
          return;
        }
        Immediate imm(shift);
        if (op->IsShl()) {
          __ shll(first_reg, imm);
        } else if (op->IsShr()) {
          __ sarl(first_reg, imm);
        } else {
          __ shrl(first_reg, imm);
        }
      }
      break;
    }
    case Primitive::kPrimLong: {
      if (second.IsRegister()) {
        Register second_reg = second.AsRegister<Register>();
        DCHECK_EQ(ECX, second_reg);
        if (op->IsShl()) {
          GenerateShlLong(first, second_reg);
        } else if (op->IsShr()) {
          GenerateShrLong(first, second_reg);
        } else {
          GenerateUShrLong(first, second_reg);
        }
      } else {
        // Shift by a constant.
        int shift = second.GetConstant()->AsIntConstant()->GetValue() & kMaxLongShiftValue;
        // Nothing to do if the shift is 0, as the input is already the output.
        if (shift != 0) {
          if (op->IsShl()) {
            GenerateShlLong(first, shift);
          } else if (op->IsShr()) {
            GenerateShrLong(first, shift);
          } else {
            GenerateUShrLong(first, shift);
          }
        }
      }
      break;
    }
    default:
      LOG(FATAL) << "Unexpected op type " << op->GetResultType();
  }
}

void InstructionCodeGeneratorX86::GenerateShlLong(const Location& loc, int shift) {
  Register low = loc.AsRegisterPairLow<Register>();
  Register high = loc.AsRegisterPairHigh<Register>();
  if (shift == 1) {
    // This is just an addition.
    __ addl(low, low);
    __ adcl(high, high);
  } else if (shift == 32) {
    // Shift by 32 is easy. High gets low, and low gets 0.
    codegen_->EmitParallelMoves(
        loc.ToLow(),
        loc.ToHigh(),
        Primitive::kPrimInt,
        Location::ConstantLocation(GetGraph()->GetIntConstant(0)),
        loc.ToLow(),
        Primitive::kPrimInt);
  } else if (shift > 32) {
    // Low part becomes 0.  High part is low part << (shift-32).
    __ movl(high, low);
    __ shll(high, Immediate(shift - 32));
    __ xorl(low, low);
  } else {
    // Between 1 and 31.
    __ shld(high, low, Immediate(shift));
    __ shll(low, Immediate(shift));
  }
}

void InstructionCodeGeneratorX86::GenerateShlLong(const Location& loc, Register shifter) {
  Label done;
  __ shld(loc.AsRegisterPairHigh<Register>(), loc.AsRegisterPairLow<Register>(), shifter);
  __ shll(loc.AsRegisterPairLow<Register>(), shifter);
  __ testl(shifter, Immediate(32));
  __ j(kEqual, &done);
  __ movl(loc.AsRegisterPairHigh<Register>(), loc.AsRegisterPairLow<Register>());
  __ movl(loc.AsRegisterPairLow<Register>(), Immediate(0));
  __ Bind(&done);
}

void InstructionCodeGeneratorX86::GenerateShrLong(const Location& loc, int shift) {
  Register low = loc.AsRegisterPairLow<Register>();
  Register high = loc.AsRegisterPairHigh<Register>();
  if (shift == 32) {
    // Need to copy the sign.
    DCHECK_NE(low, high);
    __ movl(low, high);
    __ sarl(high, Immediate(31));
  } else if (shift > 32) {
    DCHECK_NE(low, high);
    // High part becomes sign. Low part is shifted by shift - 32.
    __ movl(low, high);
    __ sarl(high, Immediate(31));
    __ sarl(low, Immediate(shift - 32));
  } else {
    // Between 1 and 31.
    __ shrd(low, high, Immediate(shift));
    __ sarl(high, Immediate(shift));
  }
}

void InstructionCodeGeneratorX86::GenerateShrLong(const Location& loc, Register shifter) {
  Label done;
  __ shrd(loc.AsRegisterPairLow<Register>(), loc.AsRegisterPairHigh<Register>(), shifter);
  __ sarl(loc.AsRegisterPairHigh<Register>(), shifter);
  __ testl(shifter, Immediate(32));
  __ j(kEqual, &done);
  __ movl(loc.AsRegisterPairLow<Register>(), loc.AsRegisterPairHigh<Register>());
  __ sarl(loc.AsRegisterPairHigh<Register>(), Immediate(31));
  __ Bind(&done);
}

void InstructionCodeGeneratorX86::GenerateUShrLong(const Location& loc, int shift) {
  Register low = loc.AsRegisterPairLow<Register>();
  Register high = loc.AsRegisterPairHigh<Register>();
  if (shift == 32) {
    // Shift by 32 is easy. Low gets high, and high gets 0.
    codegen_->EmitParallelMoves(
        loc.ToHigh(),
        loc.ToLow(),
        Primitive::kPrimInt,
        Location::ConstantLocation(GetGraph()->GetIntConstant(0)),
        loc.ToHigh(),
        Primitive::kPrimInt);
  } else if (shift > 32) {
    // Low part is high >> (shift - 32). High part becomes 0.
    __ movl(low, high);
    __ shrl(low, Immediate(shift - 32));
    __ xorl(high, high);
  } else {
    // Between 1 and 31.
    __ shrd(low, high, Immediate(shift));
    __ shrl(high, Immediate(shift));
  }
}

void InstructionCodeGeneratorX86::GenerateUShrLong(const Location& loc, Register shifter) {
  Label done;
  __ shrd(loc.AsRegisterPairLow<Register>(), loc.AsRegisterPairHigh<Register>(), shifter);
  __ shrl(loc.AsRegisterPairHigh<Register>(), shifter);
  __ testl(shifter, Immediate(32));
  __ j(kEqual, &done);
  __ movl(loc.AsRegisterPairLow<Register>(), loc.AsRegisterPairHigh<Register>());
  __ movl(loc.AsRegisterPairHigh<Register>(), Immediate(0));
  __ Bind(&done);
}

void LocationsBuilderX86::VisitShl(HShl* shl) {
  HandleShift(shl);
}

void InstructionCodeGeneratorX86::VisitShl(HShl* shl) {
  HandleShift(shl);
}

void LocationsBuilderX86::VisitShr(HShr* shr) {
  HandleShift(shr);
}

void InstructionCodeGeneratorX86::VisitShr(HShr* shr) {
  HandleShift(shr);
}

void LocationsBuilderX86::VisitUShr(HUShr* ushr) {
  HandleShift(ushr);
}

void InstructionCodeGeneratorX86::VisitUShr(HUShr* ushr) {
  HandleShift(ushr);
}

void LocationsBuilderX86::VisitNewInstance(HNewInstance* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCall);
  locations->SetOut(Location::RegisterLocation(EAX));
  InvokeRuntimeCallingConvention calling_convention;
  locations->AddTemp(Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
  locations->AddTemp(Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
}

void InstructionCodeGeneratorX86::VisitNewInstance(HNewInstance* instruction) {
  InvokeRuntimeCallingConvention calling_convention;
  codegen_->LoadCurrentMethod(calling_convention.GetRegisterAt(1));
  __ movl(calling_convention.GetRegisterAt(0), Immediate(instruction->GetTypeIndex()));

  __ fs()->call(Address::Absolute(GetThreadOffset<kX86WordSize>(instruction->GetEntrypoint())));

  codegen_->RecordPcInfo(instruction, instruction->GetDexPc());
  DCHECK(!codegen_->IsLeafMethod());
}

void LocationsBuilderX86::VisitNewArray(HNewArray* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCall);
  locations->SetOut(Location::RegisterLocation(EAX));
  InvokeRuntimeCallingConvention calling_convention;
  locations->AddTemp(Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
  locations->AddTemp(Location::RegisterLocation(calling_convention.GetRegisterAt(2)));
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
}

void InstructionCodeGeneratorX86::VisitNewArray(HNewArray* instruction) {
  InvokeRuntimeCallingConvention calling_convention;
  codegen_->LoadCurrentMethod(calling_convention.GetRegisterAt(2));
  __ movl(calling_convention.GetRegisterAt(0), Immediate(instruction->GetTypeIndex()));

  __ fs()->call(Address::Absolute(GetThreadOffset<kX86WordSize>(instruction->GetEntrypoint())));

  codegen_->RecordPcInfo(instruction, instruction->GetDexPc());
  DCHECK(!codegen_->IsLeafMethod());
}

void LocationsBuilderX86::VisitParameterValue(HParameterValue* instruction) {
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

void InstructionCodeGeneratorX86::VisitParameterValue(HParameterValue* instruction) {
  UNUSED(instruction);
}

void LocationsBuilderX86::VisitNot(HNot* not_) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(not_, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::SameAsFirstInput());
}

void InstructionCodeGeneratorX86::VisitNot(HNot* not_) {
  LocationSummary* locations = not_->GetLocations();
  Location in = locations->InAt(0);
  Location out = locations->Out();
  DCHECK(in.Equals(out));
  switch (not_->GetResultType()) {
    case Primitive::kPrimInt:
      __ notl(out.AsRegister<Register>());
      break;

    case Primitive::kPrimLong:
      __ notl(out.AsRegisterPairLow<Register>());
      __ notl(out.AsRegisterPairHigh<Register>());
      break;

    default:
      LOG(FATAL) << "Unimplemented type for not operation " << not_->GetResultType();
  }
}

void LocationsBuilderX86::VisitBooleanNot(HBooleanNot* bool_not) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(bool_not, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::SameAsFirstInput());
}

void InstructionCodeGeneratorX86::VisitBooleanNot(HBooleanNot* bool_not) {
  LocationSummary* locations = bool_not->GetLocations();
  Location in = locations->InAt(0);
  Location out = locations->Out();
  DCHECK(in.Equals(out));
  __ xorl(out.AsRegister<Register>(), Immediate(1));
}

void LocationsBuilderX86::VisitCompare(HCompare* compare) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(compare, LocationSummary::kNoCall);
  switch (compare->InputAt(0)->GetType()) {
    case Primitive::kPrimLong: {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::Any());
      locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
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

void InstructionCodeGeneratorX86::VisitCompare(HCompare* compare) {
  LocationSummary* locations = compare->GetLocations();
  Register out = locations->Out().AsRegister<Register>();
  Location left = locations->InAt(0);
  Location right = locations->InAt(1);

  Label less, greater, done;
  switch (compare->InputAt(0)->GetType()) {
    case Primitive::kPrimLong: {
      Register left_low = left.AsRegisterPairLow<Register>();
      Register left_high = left.AsRegisterPairHigh<Register>();
      int32_t val_low = 0;
      int32_t val_high = 0;
      bool right_is_const = false;

      if (right.IsConstant()) {
        DCHECK(right.GetConstant()->IsLongConstant());
        right_is_const = true;
        int64_t val = right.GetConstant()->AsLongConstant()->GetValue();
        val_low = Low32Bits(val);
        val_high = High32Bits(val);
      }

      if (right.IsRegisterPair()) {
        __ cmpl(left_high, right.AsRegisterPairHigh<Register>());
      } else if (right.IsDoubleStackSlot()) {
        __ cmpl(left_high, Address(ESP, right.GetHighStackIndex(kX86WordSize)));
      } else {
        DCHECK(right_is_const) << right;
        if (val_high == 0) {
          __ testl(left_high, left_high);
        } else {
          __ cmpl(left_high, Immediate(val_high));
        }
      }
      __ j(kLess, &less);  // Signed compare.
      __ j(kGreater, &greater);  // Signed compare.
      if (right.IsRegisterPair()) {
        __ cmpl(left_low, right.AsRegisterPairLow<Register>());
      } else if (right.IsDoubleStackSlot()) {
        __ cmpl(left_low, Address(ESP, right.GetStackIndex()));
      } else {
        DCHECK(right_is_const) << right;
        if (val_low == 0) {
          __ testl(left_low, left_low);
        } else {
          __ cmpl(left_low, Immediate(val_low));
        }
      }
      break;
    }
    case Primitive::kPrimFloat: {
      __ ucomiss(left.AsFpuRegister<XmmRegister>(), right.AsFpuRegister<XmmRegister>());
      __ j(kUnordered, compare->IsGtBias() ? &greater : &less);
      break;
    }
    case Primitive::kPrimDouble: {
      __ ucomisd(left.AsFpuRegister<XmmRegister>(), right.AsFpuRegister<XmmRegister>());
      __ j(kUnordered, compare->IsGtBias() ? &greater : &less);
      break;
    }
    default:
      LOG(FATAL) << "Unexpected type for compare operation " << compare->InputAt(0)->GetType();
  }
  __ movl(out, Immediate(0));
  __ j(kEqual, &done);
  __ j(kBelow, &less);  // kBelow is for CF (unsigned & floats).

  __ Bind(&greater);
  __ movl(out, Immediate(1));
  __ jmp(&done);

  __ Bind(&less);
  __ movl(out, Immediate(-1));

  __ Bind(&done);
}

void LocationsBuilderX86::VisitPhi(HPhi* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  for (size_t i = 0, e = instruction->InputCount(); i < e; ++i) {
    locations->SetInAt(i, Location::Any());
  }
  locations->SetOut(Location::Any());
}

void InstructionCodeGeneratorX86::VisitPhi(HPhi* instruction) {
  UNUSED(instruction);
  LOG(FATAL) << "Unreachable";
}

void InstructionCodeGeneratorX86::GenerateMemoryBarrier(MemBarrierKind kind) {
  /*
   * According to the JSR-133 Cookbook, for x86 only StoreLoad/AnyAny barriers need memory fence.
   * All other barriers (LoadAny, AnyStore, StoreStore) are nops due to the x86 memory model.
   * For those cases, all we need to ensure is that there is a scheduling barrier in place.
   */
  switch (kind) {
    case MemBarrierKind::kAnyAny: {
      __ mfence();
      break;
    }
    case MemBarrierKind::kAnyStore:
    case MemBarrierKind::kLoadAny:
    case MemBarrierKind::kStoreStore: {
      // nop
      break;
    }
    default:
      LOG(FATAL) << "Unexpected memory barrier " << kind;
  }
}


void CodeGeneratorX86::GenerateStaticOrDirectCall(HInvokeStaticOrDirect* invoke,
                                                  Register temp) {
  // TODO: Implement all kinds of calls:
  // 1) boot -> boot
  // 2) app -> boot
  // 3) app -> app
  //
  // Currently we implement the app -> app logic, which looks up in the resolve cache.

  if (invoke->IsStringInit()) {
    // temp = thread->string_init_entrypoint
    __ fs()->movl(temp, Address::Absolute(invoke->GetStringInitOffset()));
    // (temp + offset_of_quick_compiled_code)()
    __ call(Address(
        temp, ArtMethod::EntryPointFromQuickCompiledCodeOffset(kX86WordSize).Int32Value()));
  } else {
    // temp = method;
    LoadCurrentMethod(temp);
    if (!invoke->IsRecursive()) {
      // temp = temp->dex_cache_resolved_methods_;
      __ movl(temp, Address(temp, ArtMethod::DexCacheResolvedMethodsOffset().Int32Value()));
      // temp = temp[index_in_cache]
      __ movl(temp, Address(temp,
                            CodeGenerator::GetCachePointerOffset(invoke->GetDexMethodIndex())));
      // (temp + offset_of_quick_compiled_code)()
      __ call(Address(temp,
          ArtMethod::EntryPointFromQuickCompiledCodeOffset(kX86WordSize).Int32Value()));
    } else {
      __ call(GetFrameEntryLabel());
    }
  }

  DCHECK(!IsLeafMethod());
}

void CodeGeneratorX86::MarkGCCard(Register temp, Register card, Register object, Register value) {
  Label is_null;
  __ testl(value, value);
  __ j(kEqual, &is_null);
  __ fs()->movl(card, Address::Absolute(Thread::CardTableOffset<kX86WordSize>().Int32Value()));
  __ movl(temp, object);
  __ shrl(temp, Immediate(gc::accounting::CardTable::kCardShift));
  __ movb(Address(temp, card, TIMES_1, 0),
          X86ManagedRegister::FromCpuRegister(card).AsByteRegister());
  __ Bind(&is_null);
}

void LocationsBuilderX86::HandleFieldGet(HInstruction* instruction, const FieldInfo& field_info) {
  DCHECK(instruction->IsInstanceFieldGet() || instruction->IsStaticFieldGet());
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());

  if (Primitive::IsFloatingPointType(instruction->GetType())) {
    locations->SetOut(Location::RequiresFpuRegister());
  } else {
    // The output overlaps in case of long: we don't want the low move to overwrite
    // the object's location.
    locations->SetOut(Location::RequiresRegister(),
        (instruction->GetType() == Primitive::kPrimLong) ? Location::kOutputOverlap
                                                         : Location::kNoOutputOverlap);
  }

  if (field_info.IsVolatile() && (field_info.GetFieldType() == Primitive::kPrimLong)) {
    // Long values can be loaded atomically into an XMM using movsd.
    // So we use an XMM register as a temp to achieve atomicity (first load the temp into the XMM
    // and then copy the XMM into the output 32bits at a time).
    locations->AddTemp(Location::RequiresFpuRegister());
  }
}

void InstructionCodeGeneratorX86::HandleFieldGet(HInstruction* instruction,
                                                 const FieldInfo& field_info) {
  DCHECK(instruction->IsInstanceFieldGet() || instruction->IsStaticFieldGet());

  LocationSummary* locations = instruction->GetLocations();
  Register base = locations->InAt(0).AsRegister<Register>();
  Location out = locations->Out();
  bool is_volatile = field_info.IsVolatile();
  Primitive::Type field_type = field_info.GetFieldType();
  uint32_t offset = field_info.GetFieldOffset().Uint32Value();

  switch (field_type) {
    case Primitive::kPrimBoolean: {
      __ movzxb(out.AsRegister<Register>(), Address(base, offset));
      break;
    }

    case Primitive::kPrimByte: {
      __ movsxb(out.AsRegister<Register>(), Address(base, offset));
      break;
    }

    case Primitive::kPrimShort: {
      __ movsxw(out.AsRegister<Register>(), Address(base, offset));
      break;
    }

    case Primitive::kPrimChar: {
      __ movzxw(out.AsRegister<Register>(), Address(base, offset));
      break;
    }

    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      __ movl(out.AsRegister<Register>(), Address(base, offset));
      break;
    }

    case Primitive::kPrimLong: {
      if (is_volatile) {
        XmmRegister temp = locations->GetTemp(0).AsFpuRegister<XmmRegister>();
        __ movsd(temp, Address(base, offset));
        codegen_->MaybeRecordImplicitNullCheck(instruction);
        __ movd(out.AsRegisterPairLow<Register>(), temp);
        __ psrlq(temp, Immediate(32));
        __ movd(out.AsRegisterPairHigh<Register>(), temp);
      } else {
        DCHECK_NE(base, out.AsRegisterPairLow<Register>());
        __ movl(out.AsRegisterPairLow<Register>(), Address(base, offset));
        codegen_->MaybeRecordImplicitNullCheck(instruction);
        __ movl(out.AsRegisterPairHigh<Register>(), Address(base, kX86WordSize + offset));
      }
      break;
    }

    case Primitive::kPrimFloat: {
      __ movss(out.AsFpuRegister<XmmRegister>(), Address(base, offset));
      break;
    }

    case Primitive::kPrimDouble: {
      __ movsd(out.AsFpuRegister<XmmRegister>(), Address(base, offset));
      break;
    }

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << field_type;
      UNREACHABLE();
  }

  // Longs are handled in the switch.
  if (field_type != Primitive::kPrimLong) {
    codegen_->MaybeRecordImplicitNullCheck(instruction);
  }

  if (is_volatile) {
    GenerateMemoryBarrier(MemBarrierKind::kLoadAny);
  }
}

void LocationsBuilderX86::HandleFieldSet(HInstruction* instruction, const FieldInfo& field_info) {
  DCHECK(instruction->IsInstanceFieldSet() || instruction->IsStaticFieldSet());

  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  bool is_volatile = field_info.IsVolatile();
  Primitive::Type field_type = field_info.GetFieldType();
  bool is_byte_type = (field_type == Primitive::kPrimBoolean)
    || (field_type == Primitive::kPrimByte);

  // The register allocator does not support multiple
  // inputs that die at entry with one in a specific register.
  if (is_byte_type) {
    // Ensure the value is in a byte register.
    locations->SetInAt(1, Location::RegisterLocation(EAX));
  } else if (Primitive::IsFloatingPointType(field_type)) {
    locations->SetInAt(1, Location::RequiresFpuRegister());
  } else {
    locations->SetInAt(1, Location::RequiresRegister());
  }
  // Temporary registers for the write barrier.
  if (CodeGenerator::StoreNeedsWriteBarrier(field_type, instruction->InputAt(1))) {
    locations->AddTemp(Location::RequiresRegister());
    // Ensure the card is in a byte register.
    locations->AddTemp(Location::RegisterLocation(ECX));
  } else if (is_volatile && (field_type == Primitive::kPrimLong)) {
    // 64bits value can be atomically written to an address with movsd and an XMM register.
    // We need two XMM registers because there's no easier way to (bit) copy a register pair
    // into a single XMM register (we copy each pair part into the XMMs and then interleave them).
    // NB: We could make the register allocator understand fp_reg <-> core_reg moves but given the
    // isolated cases when we need this it isn't worth adding the extra complexity.
    locations->AddTemp(Location::RequiresFpuRegister());
    locations->AddTemp(Location::RequiresFpuRegister());
  }
}

void InstructionCodeGeneratorX86::HandleFieldSet(HInstruction* instruction,
                                                 const FieldInfo& field_info) {
  DCHECK(instruction->IsInstanceFieldSet() || instruction->IsStaticFieldSet());

  LocationSummary* locations = instruction->GetLocations();
  Register base = locations->InAt(0).AsRegister<Register>();
  Location value = locations->InAt(1);
  bool is_volatile = field_info.IsVolatile();
  Primitive::Type field_type = field_info.GetFieldType();
  uint32_t offset = field_info.GetFieldOffset().Uint32Value();

  if (is_volatile) {
    GenerateMemoryBarrier(MemBarrierKind::kAnyStore);
  }

  switch (field_type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte: {
      __ movb(Address(base, offset), value.AsRegister<ByteRegister>());
      break;
    }

    case Primitive::kPrimShort:
    case Primitive::kPrimChar: {
      __ movw(Address(base, offset), value.AsRegister<Register>());
      break;
    }

    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      __ movl(Address(base, offset), value.AsRegister<Register>());
      break;
    }

    case Primitive::kPrimLong: {
      if (is_volatile) {
        XmmRegister temp1 = locations->GetTemp(0).AsFpuRegister<XmmRegister>();
        XmmRegister temp2 = locations->GetTemp(1).AsFpuRegister<XmmRegister>();
        __ movd(temp1, value.AsRegisterPairLow<Register>());
        __ movd(temp2, value.AsRegisterPairHigh<Register>());
        __ punpckldq(temp1, temp2);
        __ movsd(Address(base, offset), temp1);
        codegen_->MaybeRecordImplicitNullCheck(instruction);
      } else {
        __ movl(Address(base, offset), value.AsRegisterPairLow<Register>());
        codegen_->MaybeRecordImplicitNullCheck(instruction);
        __ movl(Address(base, kX86WordSize + offset), value.AsRegisterPairHigh<Register>());
      }
      break;
    }

    case Primitive::kPrimFloat: {
      __ movss(Address(base, offset), value.AsFpuRegister<XmmRegister>());
      break;
    }

    case Primitive::kPrimDouble: {
      __ movsd(Address(base, offset), value.AsFpuRegister<XmmRegister>());
      break;
    }

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << field_type;
      UNREACHABLE();
  }

  // Longs are handled in the switch.
  if (field_type != Primitive::kPrimLong) {
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

void LocationsBuilderX86::VisitStaticFieldGet(HStaticFieldGet* instruction) {
  HandleFieldGet(instruction, instruction->GetFieldInfo());
}

void InstructionCodeGeneratorX86::VisitStaticFieldGet(HStaticFieldGet* instruction) {
  HandleFieldGet(instruction, instruction->GetFieldInfo());
}

void LocationsBuilderX86::VisitStaticFieldSet(HStaticFieldSet* instruction) {
  HandleFieldSet(instruction, instruction->GetFieldInfo());
}

void InstructionCodeGeneratorX86::VisitStaticFieldSet(HStaticFieldSet* instruction) {
  HandleFieldSet(instruction, instruction->GetFieldInfo());
}

void LocationsBuilderX86::VisitInstanceFieldSet(HInstanceFieldSet* instruction) {
  HandleFieldSet(instruction, instruction->GetFieldInfo());
}

void InstructionCodeGeneratorX86::VisitInstanceFieldSet(HInstanceFieldSet* instruction) {
  HandleFieldSet(instruction, instruction->GetFieldInfo());
}

void LocationsBuilderX86::VisitInstanceFieldGet(HInstanceFieldGet* instruction) {
  HandleFieldGet(instruction, instruction->GetFieldInfo());
}

void InstructionCodeGeneratorX86::VisitInstanceFieldGet(HInstanceFieldGet* instruction) {
  HandleFieldGet(instruction, instruction->GetFieldInfo());
}

void LocationsBuilderX86::VisitNullCheck(HNullCheck* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  Location loc = codegen_->GetCompilerOptions().GetImplicitNullChecks()
      ? Location::RequiresRegister()
      : Location::Any();
  locations->SetInAt(0, loc);
  if (instruction->HasUses()) {
    locations->SetOut(Location::SameAsFirstInput());
  }
}

void InstructionCodeGeneratorX86::GenerateImplicitNullCheck(HNullCheck* instruction) {
  if (codegen_->CanMoveNullCheckToUser(instruction)) {
    return;
  }
  LocationSummary* locations = instruction->GetLocations();
  Location obj = locations->InAt(0);

  __ testl(EAX, Address(obj.AsRegister<Register>(), 0));
  codegen_->RecordPcInfo(instruction, instruction->GetDexPc());
}

void InstructionCodeGeneratorX86::GenerateExplicitNullCheck(HNullCheck* instruction) {
  SlowPathCodeX86* slow_path = new (GetGraph()->GetArena()) NullCheckSlowPathX86(instruction);
  codegen_->AddSlowPath(slow_path);

  LocationSummary* locations = instruction->GetLocations();
  Location obj = locations->InAt(0);

  if (obj.IsRegister()) {
    __ testl(obj.AsRegister<Register>(), obj.AsRegister<Register>());
  } else if (obj.IsStackSlot()) {
    __ cmpl(Address(ESP, obj.GetStackIndex()), Immediate(0));
  } else {
    DCHECK(obj.IsConstant()) << obj;
    DCHECK_EQ(obj.GetConstant()->AsIntConstant()->GetValue(), 0);
    __ jmp(slow_path->GetEntryLabel());
    return;
  }
  __ j(kEqual, slow_path->GetEntryLabel());
}

void InstructionCodeGeneratorX86::VisitNullCheck(HNullCheck* instruction) {
  if (codegen_->GetCompilerOptions().GetImplicitNullChecks()) {
    GenerateImplicitNullCheck(instruction);
  } else {
    GenerateExplicitNullCheck(instruction);
  }
}

void LocationsBuilderX86::VisitArrayGet(HArrayGet* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RegisterOrConstant(instruction->InputAt(1)));
  if (Primitive::IsFloatingPointType(instruction->GetType())) {
    locations->SetOut(Location::RequiresFpuRegister(), Location::kNoOutputOverlap);
  } else {
    // The output overlaps in case of long: we don't want the low move to overwrite
    // the array's location.
    locations->SetOut(Location::RequiresRegister(),
        (instruction->GetType() == Primitive::kPrimLong) ? Location::kOutputOverlap
                                                         : Location::kNoOutputOverlap);
  }
}

void InstructionCodeGeneratorX86::VisitArrayGet(HArrayGet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Register obj = locations->InAt(0).AsRegister<Register>();
  Location index = locations->InAt(1);

  Primitive::Type type = instruction->GetType();
  switch (type) {
    case Primitive::kPrimBoolean: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(uint8_t)).Uint32Value();
      Register out = locations->Out().AsRegister<Register>();
      if (index.IsConstant()) {
        __ movzxb(out, Address(obj,
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_1) + data_offset));
      } else {
        __ movzxb(out, Address(obj, index.AsRegister<Register>(), TIMES_1, data_offset));
      }
      break;
    }

    case Primitive::kPrimByte: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(int8_t)).Uint32Value();
      Register out = locations->Out().AsRegister<Register>();
      if (index.IsConstant()) {
        __ movsxb(out, Address(obj,
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_1) + data_offset));
      } else {
        __ movsxb(out, Address(obj, index.AsRegister<Register>(), TIMES_1, data_offset));
      }
      break;
    }

    case Primitive::kPrimShort: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(int16_t)).Uint32Value();
      Register out = locations->Out().AsRegister<Register>();
      if (index.IsConstant()) {
        __ movsxw(out, Address(obj,
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_2) + data_offset));
      } else {
        __ movsxw(out, Address(obj, index.AsRegister<Register>(), TIMES_2, data_offset));
      }
      break;
    }

    case Primitive::kPrimChar: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(uint16_t)).Uint32Value();
      Register out = locations->Out().AsRegister<Register>();
      if (index.IsConstant()) {
        __ movzxw(out, Address(obj,
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_2) + data_offset));
      } else {
        __ movzxw(out, Address(obj, index.AsRegister<Register>(), TIMES_2, data_offset));
      }
      break;
    }

    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(int32_t)).Uint32Value();
      Register out = locations->Out().AsRegister<Register>();
      if (index.IsConstant()) {
        __ movl(out, Address(obj,
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_4) + data_offset));
      } else {
        __ movl(out, Address(obj, index.AsRegister<Register>(), TIMES_4, data_offset));
      }
      break;
    }

    case Primitive::kPrimLong: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(int64_t)).Uint32Value();
      Location out = locations->Out();
      DCHECK_NE(obj, out.AsRegisterPairLow<Register>());
      if (index.IsConstant()) {
        size_t offset = (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_8) + data_offset;
        __ movl(out.AsRegisterPairLow<Register>(), Address(obj, offset));
        codegen_->MaybeRecordImplicitNullCheck(instruction);
        __ movl(out.AsRegisterPairHigh<Register>(), Address(obj, offset + kX86WordSize));
      } else {
        __ movl(out.AsRegisterPairLow<Register>(),
                Address(obj, index.AsRegister<Register>(), TIMES_8, data_offset));
        codegen_->MaybeRecordImplicitNullCheck(instruction);
        __ movl(out.AsRegisterPairHigh<Register>(),
                Address(obj, index.AsRegister<Register>(), TIMES_8, data_offset + kX86WordSize));
      }
      break;
    }

    case Primitive::kPrimFloat: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(float)).Uint32Value();
      XmmRegister out = locations->Out().AsFpuRegister<XmmRegister>();
      if (index.IsConstant()) {
        __ movss(out, Address(obj,
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_4) + data_offset));
      } else {
        __ movss(out, Address(obj, index.AsRegister<Register>(), TIMES_4, data_offset));
      }
      break;
    }

    case Primitive::kPrimDouble: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(double)).Uint32Value();
      XmmRegister out = locations->Out().AsFpuRegister<XmmRegister>();
      if (index.IsConstant()) {
        __ movsd(out, Address(obj,
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_8) + data_offset));
      } else {
        __ movsd(out, Address(obj, index.AsRegister<Register>(), TIMES_8, data_offset));
      }
      break;
    }

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << type;
      UNREACHABLE();
  }

  if (type != Primitive::kPrimLong) {
    codegen_->MaybeRecordImplicitNullCheck(instruction);
  }
}

void LocationsBuilderX86::VisitArraySet(HArraySet* instruction) {
  // This location builder might end up asking to up to four registers, which is
  // not currently possible for baseline. The situation in which we need four
  // registers cannot be met by baseline though, because it has not run any
  // optimization.

  Primitive::Type value_type = instruction->GetComponentType();
  bool needs_write_barrier =
      CodeGenerator::StoreNeedsWriteBarrier(value_type, instruction->GetValue());

  bool needs_runtime_call = instruction->NeedsTypeCheck();

  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(
      instruction,
      needs_runtime_call ? LocationSummary::kCall : LocationSummary::kNoCall);

  if (needs_runtime_call) {
    InvokeRuntimeCallingConvention calling_convention;
    locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
    locations->SetInAt(1, Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
    locations->SetInAt(2, Location::RegisterLocation(calling_convention.GetRegisterAt(2)));
  } else {
    bool is_byte_type = (value_type == Primitive::kPrimBoolean)
        || (value_type == Primitive::kPrimByte);
    // We need the inputs to be different than the output in case of long operation.
    // In case of a byte operation, the register allocator does not support multiple
    // inputs that die at entry with one in a specific register.
    locations->SetInAt(0, Location::RequiresRegister());
    locations->SetInAt(1, Location::RegisterOrConstant(instruction->InputAt(1)));
    if (is_byte_type) {
      // Ensure the value is in a byte register.
      locations->SetInAt(2, Location::ByteRegisterOrConstant(EAX, instruction->InputAt(2)));
    } else if (Primitive::IsFloatingPointType(value_type)) {
      locations->SetInAt(2, Location::RequiresFpuRegister());
    } else {
      locations->SetInAt(2, Location::RegisterOrConstant(instruction->InputAt(2)));
    }
    // Temporary registers for the write barrier.
    if (needs_write_barrier) {
      locations->AddTemp(Location::RequiresRegister());
      // Ensure the card is in a byte register.
      locations->AddTemp(Location::RegisterLocation(ECX));
    }
  }
}

void InstructionCodeGeneratorX86::VisitArraySet(HArraySet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Register obj = locations->InAt(0).AsRegister<Register>();
  Location index = locations->InAt(1);
  Location value = locations->InAt(2);
  Primitive::Type value_type = instruction->GetComponentType();
  bool needs_runtime_call = locations->WillCall();
  bool needs_write_barrier =
      CodeGenerator::StoreNeedsWriteBarrier(value_type, instruction->GetValue());

  switch (value_type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(uint8_t)).Uint32Value();
      if (index.IsConstant()) {
        size_t offset = (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_1) + data_offset;
        if (value.IsRegister()) {
          __ movb(Address(obj, offset), value.AsRegister<ByteRegister>());
        } else {
          __ movb(Address(obj, offset),
                  Immediate(value.GetConstant()->AsIntConstant()->GetValue()));
        }
      } else {
        if (value.IsRegister()) {
          __ movb(Address(obj, index.AsRegister<Register>(), TIMES_1, data_offset),
                  value.AsRegister<ByteRegister>());
        } else {
          __ movb(Address(obj, index.AsRegister<Register>(), TIMES_1, data_offset),
                  Immediate(value.GetConstant()->AsIntConstant()->GetValue()));
        }
      }
      codegen_->MaybeRecordImplicitNullCheck(instruction);
      break;
    }

    case Primitive::kPrimShort:
    case Primitive::kPrimChar: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(uint16_t)).Uint32Value();
      if (index.IsConstant()) {
        size_t offset = (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_2) + data_offset;
        if (value.IsRegister()) {
          __ movw(Address(obj, offset), value.AsRegister<Register>());
        } else {
          __ movw(Address(obj, offset),
                  Immediate(value.GetConstant()->AsIntConstant()->GetValue()));
        }
      } else {
        if (value.IsRegister()) {
          __ movw(Address(obj, index.AsRegister<Register>(), TIMES_2, data_offset),
                  value.AsRegister<Register>());
        } else {
          __ movw(Address(obj, index.AsRegister<Register>(), TIMES_2, data_offset),
                  Immediate(value.GetConstant()->AsIntConstant()->GetValue()));
        }
      }
      codegen_->MaybeRecordImplicitNullCheck(instruction);
      break;
    }

    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      if (!needs_runtime_call) {
        uint32_t data_offset = mirror::Array::DataOffset(sizeof(int32_t)).Uint32Value();
        if (index.IsConstant()) {
          size_t offset =
              (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_4) + data_offset;
          if (value.IsRegister()) {
            __ movl(Address(obj, offset), value.AsRegister<Register>());
          } else {
            DCHECK(value.IsConstant()) << value;
            __ movl(Address(obj, offset),
                    Immediate(value.GetConstant()->AsIntConstant()->GetValue()));
          }
        } else {
          DCHECK(index.IsRegister()) << index;
          if (value.IsRegister()) {
            __ movl(Address(obj, index.AsRegister<Register>(), TIMES_4, data_offset),
                    value.AsRegister<Register>());
          } else {
            DCHECK(value.IsConstant()) << value;
            __ movl(Address(obj, index.AsRegister<Register>(), TIMES_4, data_offset),
                    Immediate(value.GetConstant()->AsIntConstant()->GetValue()));
          }
        }
        codegen_->MaybeRecordImplicitNullCheck(instruction);

        if (needs_write_barrier) {
          Register temp = locations->GetTemp(0).AsRegister<Register>();
          Register card = locations->GetTemp(1).AsRegister<Register>();
          codegen_->MarkGCCard(temp, card, obj, value.AsRegister<Register>());
        }
      } else {
        DCHECK_EQ(value_type, Primitive::kPrimNot);
        DCHECK(!codegen_->IsLeafMethod());
        __ fs()->call(Address::Absolute(QUICK_ENTRYPOINT_OFFSET(kX86WordSize, pAputObject)));
        codegen_->RecordPcInfo(instruction, instruction->GetDexPc());
      }
      break;
    }

    case Primitive::kPrimLong: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(int64_t)).Uint32Value();
      if (index.IsConstant()) {
        size_t offset = (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_8) + data_offset;
        if (value.IsRegisterPair()) {
          __ movl(Address(obj, offset), value.AsRegisterPairLow<Register>());
          codegen_->MaybeRecordImplicitNullCheck(instruction);
          __ movl(Address(obj, offset + kX86WordSize), value.AsRegisterPairHigh<Register>());
        } else {
          DCHECK(value.IsConstant());
          int64_t val = value.GetConstant()->AsLongConstant()->GetValue();
          __ movl(Address(obj, offset), Immediate(Low32Bits(val)));
          codegen_->MaybeRecordImplicitNullCheck(instruction);
          __ movl(Address(obj, offset + kX86WordSize), Immediate(High32Bits(val)));
        }
      } else {
        if (value.IsRegisterPair()) {
          __ movl(Address(obj, index.AsRegister<Register>(), TIMES_8, data_offset),
                  value.AsRegisterPairLow<Register>());
          codegen_->MaybeRecordImplicitNullCheck(instruction);
          __ movl(Address(obj, index.AsRegister<Register>(), TIMES_8, data_offset + kX86WordSize),
                  value.AsRegisterPairHigh<Register>());
        } else {
          DCHECK(value.IsConstant());
          int64_t val = value.GetConstant()->AsLongConstant()->GetValue();
          __ movl(Address(obj, index.AsRegister<Register>(), TIMES_8, data_offset),
                  Immediate(Low32Bits(val)));
          codegen_->MaybeRecordImplicitNullCheck(instruction);
          __ movl(Address(obj, index.AsRegister<Register>(), TIMES_8, data_offset + kX86WordSize),
                  Immediate(High32Bits(val)));
        }
      }
      break;
    }

    case Primitive::kPrimFloat: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(float)).Uint32Value();
      DCHECK(value.IsFpuRegister());
      if (index.IsConstant()) {
        size_t offset = (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_4) + data_offset;
        __ movss(Address(obj, offset), value.AsFpuRegister<XmmRegister>());
      } else {
        __ movss(Address(obj, index.AsRegister<Register>(), TIMES_4, data_offset),
                value.AsFpuRegister<XmmRegister>());
      }
      break;
    }

    case Primitive::kPrimDouble: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(double)).Uint32Value();
      DCHECK(value.IsFpuRegister());
      if (index.IsConstant()) {
        size_t offset = (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_8) + data_offset;
        __ movsd(Address(obj, offset), value.AsFpuRegister<XmmRegister>());
      } else {
        __ movsd(Address(obj, index.AsRegister<Register>(), TIMES_8, data_offset),
                value.AsFpuRegister<XmmRegister>());
      }
      break;
    }

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << instruction->GetType();
      UNREACHABLE();
  }
}

void LocationsBuilderX86::VisitArrayLength(HArrayLength* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
  instruction->SetLocations(locations);
}

void InstructionCodeGeneratorX86::VisitArrayLength(HArrayLength* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  uint32_t offset = mirror::Array::LengthOffset().Uint32Value();
  Register obj = locations->InAt(0).AsRegister<Register>();
  Register out = locations->Out().AsRegister<Register>();
  __ movl(out, Address(obj, offset));
  codegen_->MaybeRecordImplicitNullCheck(instruction);
}

void LocationsBuilderX86::VisitBoundsCheck(HBoundsCheck* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RegisterOrConstant(instruction->InputAt(0)));
  locations->SetInAt(1, Location::RegisterOrConstant(instruction->InputAt(1)));
  if (instruction->HasUses()) {
    locations->SetOut(Location::SameAsFirstInput());
  }
}

void InstructionCodeGeneratorX86::VisitBoundsCheck(HBoundsCheck* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Location index_loc = locations->InAt(0);
  Location length_loc = locations->InAt(1);
  SlowPathCodeX86* slow_path =
    new (GetGraph()->GetArena()) BoundsCheckSlowPathX86(instruction, index_loc, length_loc);

  if (length_loc.IsConstant()) {
    int32_t length = CodeGenerator::GetInt32ValueOf(length_loc.GetConstant());
    if (index_loc.IsConstant()) {
      // BCE will remove the bounds check if we are guarenteed to pass.
      int32_t index = CodeGenerator::GetInt32ValueOf(index_loc.GetConstant());
      if (index < 0 || index >= length) {
        codegen_->AddSlowPath(slow_path);
        __ jmp(slow_path->GetEntryLabel());
      } else {
        // Some optimization after BCE may have generated this, and we should not
        // generate a bounds check if it is a valid range.
      }
      return;
    }

    // We have to reverse the jump condition because the length is the constant.
    Register index_reg = index_loc.AsRegister<Register>();
    __ cmpl(index_reg, Immediate(length));
    codegen_->AddSlowPath(slow_path);
    __ j(kAboveEqual, slow_path->GetEntryLabel());
  } else {
    Register length = length_loc.AsRegister<Register>();
    if (index_loc.IsConstant()) {
      int32_t value = CodeGenerator::GetInt32ValueOf(index_loc.GetConstant());
      __ cmpl(length, Immediate(value));
    } else {
      __ cmpl(length, index_loc.AsRegister<Register>());
    }
    codegen_->AddSlowPath(slow_path);
    __ j(kBelowEqual, slow_path->GetEntryLabel());
  }
}

void LocationsBuilderX86::VisitTemporary(HTemporary* temp) {
  temp->SetLocations(nullptr);
}

void InstructionCodeGeneratorX86::VisitTemporary(HTemporary* temp) {
  // Nothing to do, this is driven by the code generator.
  UNUSED(temp);
}

void LocationsBuilderX86::VisitParallelMove(HParallelMove* instruction) {
  UNUSED(instruction);
  LOG(FATAL) << "Unreachable";
}

void InstructionCodeGeneratorX86::VisitParallelMove(HParallelMove* instruction) {
  codegen_->GetMoveResolver()->EmitNativeCode(instruction);
}

void LocationsBuilderX86::VisitSuspendCheck(HSuspendCheck* instruction) {
  new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCallOnSlowPath);
}

void InstructionCodeGeneratorX86::VisitSuspendCheck(HSuspendCheck* instruction) {
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

void InstructionCodeGeneratorX86::GenerateSuspendCheck(HSuspendCheck* instruction,
                                                       HBasicBlock* successor) {
  SuspendCheckSlowPathX86* slow_path =
      down_cast<SuspendCheckSlowPathX86*>(instruction->GetSlowPath());
  if (slow_path == nullptr) {
    slow_path = new (GetGraph()->GetArena()) SuspendCheckSlowPathX86(instruction, successor);
    instruction->SetSlowPath(slow_path);
    codegen_->AddSlowPath(slow_path);
    if (successor != nullptr) {
      DCHECK(successor->IsLoopHeader());
      codegen_->ClearSpillSlotsFromLoopPhisInStackMap(instruction);
    }
  } else {
    DCHECK_EQ(slow_path->GetSuccessor(), successor);
  }

  __ fs()->cmpw(Address::Absolute(
      Thread::ThreadFlagsOffset<kX86WordSize>().Int32Value()), Immediate(0));
  if (successor == nullptr) {
    __ j(kNotEqual, slow_path->GetEntryLabel());
    __ Bind(slow_path->GetReturnLabel());
  } else {
    __ j(kEqual, codegen_->GetLabelOf(successor));
    __ jmp(slow_path->GetEntryLabel());
  }
}

X86Assembler* ParallelMoveResolverX86::GetAssembler() const {
  return codegen_->GetAssembler();
}

void ParallelMoveResolverX86::MoveMemoryToMemory32(int dst, int src) {
  ScratchRegisterScope ensure_scratch(
      this, kNoRegister, EAX, codegen_->GetNumberOfCoreRegisters());
  Register temp_reg = static_cast<Register>(ensure_scratch.GetRegister());
  int stack_offset = ensure_scratch.IsSpilled() ? kX86WordSize : 0;
  __ movl(temp_reg, Address(ESP, src + stack_offset));
  __ movl(Address(ESP, dst + stack_offset), temp_reg);
}

void ParallelMoveResolverX86::MoveMemoryToMemory64(int dst, int src) {
  ScratchRegisterScope ensure_scratch(
      this, kNoRegister, EAX, codegen_->GetNumberOfCoreRegisters());
  Register temp_reg = static_cast<Register>(ensure_scratch.GetRegister());
  int stack_offset = ensure_scratch.IsSpilled() ? kX86WordSize : 0;
  __ movl(temp_reg, Address(ESP, src + stack_offset));
  __ movl(Address(ESP, dst + stack_offset), temp_reg);
  __ movl(temp_reg, Address(ESP, src + stack_offset + kX86WordSize));
  __ movl(Address(ESP, dst + stack_offset + kX86WordSize), temp_reg);
}

void ParallelMoveResolverX86::EmitMove(size_t index) {
  MoveOperands* move = moves_.Get(index);
  Location source = move->GetSource();
  Location destination = move->GetDestination();

  if (source.IsRegister()) {
    if (destination.IsRegister()) {
      __ movl(destination.AsRegister<Register>(), source.AsRegister<Register>());
    } else {
      DCHECK(destination.IsStackSlot());
      __ movl(Address(ESP, destination.GetStackIndex()), source.AsRegister<Register>());
    }
  } else if (source.IsFpuRegister()) {
    if (destination.IsFpuRegister()) {
      __ movaps(destination.AsFpuRegister<XmmRegister>(), source.AsFpuRegister<XmmRegister>());
    } else if (destination.IsStackSlot()) {
      __ movss(Address(ESP, destination.GetStackIndex()), source.AsFpuRegister<XmmRegister>());
    } else {
      DCHECK(destination.IsDoubleStackSlot());
      __ movsd(Address(ESP, destination.GetStackIndex()), source.AsFpuRegister<XmmRegister>());
    }
  } else if (source.IsStackSlot()) {
    if (destination.IsRegister()) {
      __ movl(destination.AsRegister<Register>(), Address(ESP, source.GetStackIndex()));
    } else if (destination.IsFpuRegister()) {
      __ movss(destination.AsFpuRegister<XmmRegister>(), Address(ESP, source.GetStackIndex()));
    } else {
      DCHECK(destination.IsStackSlot());
      MoveMemoryToMemory32(destination.GetStackIndex(), source.GetStackIndex());
    }
  } else if (source.IsDoubleStackSlot()) {
    if (destination.IsFpuRegister()) {
      __ movsd(destination.AsFpuRegister<XmmRegister>(), Address(ESP, source.GetStackIndex()));
    } else {
      DCHECK(destination.IsDoubleStackSlot()) << destination;
      MoveMemoryToMemory64(destination.GetStackIndex(), source.GetStackIndex());
    }
  } else if (source.IsConstant()) {
    HConstant* constant = source.GetConstant();
    if (constant->IsIntConstant() || constant->IsNullConstant()) {
      int32_t value = CodeGenerator::GetInt32ValueOf(constant);
      if (destination.IsRegister()) {
        if (value == 0) {
          __ xorl(destination.AsRegister<Register>(), destination.AsRegister<Register>());
        } else {
          __ movl(destination.AsRegister<Register>(), Immediate(value));
        }
      } else {
        DCHECK(destination.IsStackSlot()) << destination;
        __ movl(Address(ESP, destination.GetStackIndex()), Immediate(value));
      }
    } else if (constant->IsFloatConstant()) {
      float fp_value = constant->AsFloatConstant()->GetValue();
      int32_t value = bit_cast<int32_t, float>(fp_value);
      Immediate imm(value);
      if (destination.IsFpuRegister()) {
        XmmRegister dest = destination.AsFpuRegister<XmmRegister>();
        if (value == 0) {
          // Easy handling of 0.0.
          __ xorps(dest, dest);
        } else {
          ScratchRegisterScope ensure_scratch(
              this, kNoRegister, EAX, codegen_->GetNumberOfCoreRegisters());
          Register temp = static_cast<Register>(ensure_scratch.GetRegister());
          __ movl(temp, Immediate(value));
          __ movd(dest, temp);
        }
      } else {
        DCHECK(destination.IsStackSlot()) << destination;
        __ movl(Address(ESP, destination.GetStackIndex()), imm);
      }
    } else if (constant->IsLongConstant()) {
      int64_t value = constant->AsLongConstant()->GetValue();
      int32_t low_value = Low32Bits(value);
      int32_t high_value = High32Bits(value);
      Immediate low(low_value);
      Immediate high(high_value);
      if (destination.IsDoubleStackSlot()) {
        __ movl(Address(ESP, destination.GetStackIndex()), low);
        __ movl(Address(ESP, destination.GetHighStackIndex(kX86WordSize)), high);
      } else {
        __ movl(destination.AsRegisterPairLow<Register>(), low);
        __ movl(destination.AsRegisterPairHigh<Register>(), high);
      }
    } else {
      DCHECK(constant->IsDoubleConstant());
      double dbl_value = constant->AsDoubleConstant()->GetValue();
      int64_t value = bit_cast<int64_t, double>(dbl_value);
      int32_t low_value = Low32Bits(value);
      int32_t high_value = High32Bits(value);
      Immediate low(low_value);
      Immediate high(high_value);
      if (destination.IsFpuRegister()) {
        XmmRegister dest = destination.AsFpuRegister<XmmRegister>();
        if (value == 0) {
          // Easy handling of 0.0.
          __ xorpd(dest, dest);
        } else {
          __ pushl(high);
          __ pushl(low);
          __ movsd(dest, Address(ESP, 0));
          __ addl(ESP, Immediate(8));
        }
      } else {
        DCHECK(destination.IsDoubleStackSlot()) << destination;
        __ movl(Address(ESP, destination.GetStackIndex()), low);
        __ movl(Address(ESP, destination.GetHighStackIndex(kX86WordSize)), high);
      }
    }
  } else {
    LOG(FATAL) << "Unimplemented move: " << destination << " <- " << source;
  }
}

void ParallelMoveResolverX86::Exchange(Register reg, int mem) {
  Register suggested_scratch = reg == EAX ? EBX : EAX;
  ScratchRegisterScope ensure_scratch(
      this, reg, suggested_scratch, codegen_->GetNumberOfCoreRegisters());

  int stack_offset = ensure_scratch.IsSpilled() ? kX86WordSize : 0;
  __ movl(static_cast<Register>(ensure_scratch.GetRegister()), Address(ESP, mem + stack_offset));
  __ movl(Address(ESP, mem + stack_offset), reg);
  __ movl(reg, static_cast<Register>(ensure_scratch.GetRegister()));
}

void ParallelMoveResolverX86::Exchange32(XmmRegister reg, int mem) {
  ScratchRegisterScope ensure_scratch(
      this, kNoRegister, EAX, codegen_->GetNumberOfCoreRegisters());

  Register temp_reg = static_cast<Register>(ensure_scratch.GetRegister());
  int stack_offset = ensure_scratch.IsSpilled() ? kX86WordSize : 0;
  __ movl(temp_reg, Address(ESP, mem + stack_offset));
  __ movss(Address(ESP, mem + stack_offset), reg);
  __ movd(reg, temp_reg);
}

void ParallelMoveResolverX86::Exchange(int mem1, int mem2) {
  ScratchRegisterScope ensure_scratch1(
      this, kNoRegister, EAX, codegen_->GetNumberOfCoreRegisters());

  Register suggested_scratch = ensure_scratch1.GetRegister() == EAX ? EBX : EAX;
  ScratchRegisterScope ensure_scratch2(
      this, ensure_scratch1.GetRegister(), suggested_scratch, codegen_->GetNumberOfCoreRegisters());

  int stack_offset = ensure_scratch1.IsSpilled() ? kX86WordSize : 0;
  stack_offset += ensure_scratch2.IsSpilled() ? kX86WordSize : 0;
  __ movl(static_cast<Register>(ensure_scratch1.GetRegister()), Address(ESP, mem1 + stack_offset));
  __ movl(static_cast<Register>(ensure_scratch2.GetRegister()), Address(ESP, mem2 + stack_offset));
  __ movl(Address(ESP, mem2 + stack_offset), static_cast<Register>(ensure_scratch1.GetRegister()));
  __ movl(Address(ESP, mem1 + stack_offset), static_cast<Register>(ensure_scratch2.GetRegister()));
}

void ParallelMoveResolverX86::EmitSwap(size_t index) {
  MoveOperands* move = moves_.Get(index);
  Location source = move->GetSource();
  Location destination = move->GetDestination();

  if (source.IsRegister() && destination.IsRegister()) {
    __ xchgl(destination.AsRegister<Register>(), source.AsRegister<Register>());
  } else if (source.IsRegister() && destination.IsStackSlot()) {
    Exchange(source.AsRegister<Register>(), destination.GetStackIndex());
  } else if (source.IsStackSlot() && destination.IsRegister()) {
    Exchange(destination.AsRegister<Register>(), source.GetStackIndex());
  } else if (source.IsStackSlot() && destination.IsStackSlot()) {
    Exchange(destination.GetStackIndex(), source.GetStackIndex());
  } else if (source.IsFpuRegister() && destination.IsFpuRegister()) {
    // Use XOR Swap algorithm to avoid a temporary.
    DCHECK_NE(source.reg(), destination.reg());
    __ xorpd(destination.AsFpuRegister<XmmRegister>(), source.AsFpuRegister<XmmRegister>());
    __ xorpd(source.AsFpuRegister<XmmRegister>(), destination.AsFpuRegister<XmmRegister>());
    __ xorpd(destination.AsFpuRegister<XmmRegister>(), source.AsFpuRegister<XmmRegister>());
  } else if (source.IsFpuRegister() && destination.IsStackSlot()) {
    Exchange32(source.AsFpuRegister<XmmRegister>(), destination.GetStackIndex());
  } else if (destination.IsFpuRegister() && source.IsStackSlot()) {
    Exchange32(destination.AsFpuRegister<XmmRegister>(), source.GetStackIndex());
  } else if (source.IsFpuRegister() && destination.IsDoubleStackSlot()) {
    // Take advantage of the 16 bytes in the XMM register.
    XmmRegister reg = source.AsFpuRegister<XmmRegister>();
    Address stack(ESP, destination.GetStackIndex());
    // Load the double into the high doubleword.
    __ movhpd(reg, stack);

    // Store the low double into the destination.
    __ movsd(stack, reg);

    // Move the high double to the low double.
    __ psrldq(reg, Immediate(8));
  } else if (destination.IsFpuRegister() && source.IsDoubleStackSlot()) {
    // Take advantage of the 16 bytes in the XMM register.
    XmmRegister reg = destination.AsFpuRegister<XmmRegister>();
    Address stack(ESP, source.GetStackIndex());
    // Load the double into the high doubleword.
    __ movhpd(reg, stack);

    // Store the low double into the destination.
    __ movsd(stack, reg);

    // Move the high double to the low double.
    __ psrldq(reg, Immediate(8));
  } else if (destination.IsDoubleStackSlot() && source.IsDoubleStackSlot()) {
    Exchange(destination.GetStackIndex(), source.GetStackIndex());
    Exchange(destination.GetHighStackIndex(kX86WordSize), source.GetHighStackIndex(kX86WordSize));
  } else {
    LOG(FATAL) << "Unimplemented: source: " << source << ", destination: " << destination;
  }
}

void ParallelMoveResolverX86::SpillScratch(int reg) {
  __ pushl(static_cast<Register>(reg));
}

void ParallelMoveResolverX86::RestoreScratch(int reg) {
  __ popl(static_cast<Register>(reg));
}

void LocationsBuilderX86::VisitLoadClass(HLoadClass* cls) {
  LocationSummary::CallKind call_kind = cls->CanCallRuntime()
      ? LocationSummary::kCallOnSlowPath
      : LocationSummary::kNoCall;
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(cls, call_kind);
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorX86::VisitLoadClass(HLoadClass* cls) {
  Register out = cls->GetLocations()->Out().AsRegister<Register>();
  if (cls->IsReferrersClass()) {
    DCHECK(!cls->CanCallRuntime());
    DCHECK(!cls->MustGenerateClinitCheck());
    codegen_->LoadCurrentMethod(out);
    __ movl(out, Address(out, ArtMethod::DeclaringClassOffset().Int32Value()));
  } else {
    DCHECK(cls->CanCallRuntime());
    codegen_->LoadCurrentMethod(out);
    __ movl(out, Address(out, ArtMethod::DexCacheResolvedTypesOffset().Int32Value()));
    __ movl(out, Address(out, CodeGenerator::GetCacheOffset(cls->GetTypeIndex())));

    SlowPathCodeX86* slow_path = new (GetGraph()->GetArena()) LoadClassSlowPathX86(
        cls, cls, cls->GetDexPc(), cls->MustGenerateClinitCheck());
    codegen_->AddSlowPath(slow_path);
    __ testl(out, out);
    __ j(kEqual, slow_path->GetEntryLabel());
    if (cls->MustGenerateClinitCheck()) {
      GenerateClassInitializationCheck(slow_path, out);
    } else {
      __ Bind(slow_path->GetExitLabel());
    }
  }
}

void LocationsBuilderX86::VisitClinitCheck(HClinitCheck* check) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(check, LocationSummary::kCallOnSlowPath);
  locations->SetInAt(0, Location::RequiresRegister());
  if (check->HasUses()) {
    locations->SetOut(Location::SameAsFirstInput());
  }
}

void InstructionCodeGeneratorX86::VisitClinitCheck(HClinitCheck* check) {
  // We assume the class to not be null.
  SlowPathCodeX86* slow_path = new (GetGraph()->GetArena()) LoadClassSlowPathX86(
      check->GetLoadClass(), check, check->GetDexPc(), true);
  codegen_->AddSlowPath(slow_path);
  GenerateClassInitializationCheck(slow_path,
                                   check->GetLocations()->InAt(0).AsRegister<Register>());
}

void InstructionCodeGeneratorX86::GenerateClassInitializationCheck(
    SlowPathCodeX86* slow_path, Register class_reg) {
  __ cmpl(Address(class_reg,  mirror::Class::StatusOffset().Int32Value()),
          Immediate(mirror::Class::kStatusInitialized));
  __ j(kLess, slow_path->GetEntryLabel());
  __ Bind(slow_path->GetExitLabel());
  // No need for memory fence, thanks to the X86 memory model.
}

void LocationsBuilderX86::VisitLoadString(HLoadString* load) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(load, LocationSummary::kCallOnSlowPath);
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorX86::VisitLoadString(HLoadString* load) {
  SlowPathCodeX86* slow_path = new (GetGraph()->GetArena()) LoadStringSlowPathX86(load);
  codegen_->AddSlowPath(slow_path);

  Register out = load->GetLocations()->Out().AsRegister<Register>();
  codegen_->LoadCurrentMethod(out);
  __ movl(out, Address(out, ArtMethod::DeclaringClassOffset().Int32Value()));
  __ movl(out, Address(out, mirror::Class::DexCacheStringsOffset().Int32Value()));
  __ movl(out, Address(out, CodeGenerator::GetCacheOffset(load->GetStringIndex())));
  __ testl(out, out);
  __ j(kEqual, slow_path->GetEntryLabel());
  __ Bind(slow_path->GetExitLabel());
}

void LocationsBuilderX86::VisitLoadException(HLoadException* load) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(load, LocationSummary::kNoCall);
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorX86::VisitLoadException(HLoadException* load) {
  Address address = Address::Absolute(Thread::ExceptionOffset<kX86WordSize>().Int32Value());
  __ fs()->movl(load->GetLocations()->Out().AsRegister<Register>(), address);
  __ fs()->movl(address, Immediate(0));
}

void LocationsBuilderX86::VisitThrow(HThrow* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCall);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
}

void InstructionCodeGeneratorX86::VisitThrow(HThrow* instruction) {
  __ fs()->call(Address::Absolute(QUICK_ENTRYPOINT_OFFSET(kX86WordSize, pDeliverException)));
  codegen_->RecordPcInfo(instruction, instruction->GetDexPc());
}

void LocationsBuilderX86::VisitInstanceOf(HInstanceOf* instruction) {
  LocationSummary::CallKind call_kind = instruction->IsClassFinal()
      ? LocationSummary::kNoCall
      : LocationSummary::kCallOnSlowPath;
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction, call_kind);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::Any());
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorX86::VisitInstanceOf(HInstanceOf* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Register obj = locations->InAt(0).AsRegister<Register>();
  Location cls = locations->InAt(1);
  Register out = locations->Out().AsRegister<Register>();
  uint32_t class_offset = mirror::Object::ClassOffset().Int32Value();
  Label done, zero;
  SlowPathCodeX86* slow_path = nullptr;

  // Return 0 if `obj` is null.
  // Avoid null check if we know obj is not null.
  if (instruction->MustDoNullCheck()) {
    __ testl(obj, obj);
    __ j(kEqual, &zero);
  }
  __ movl(out, Address(obj, class_offset));
  // Compare the class of `obj` with `cls`.
  if (cls.IsRegister()) {
    __ cmpl(out, cls.AsRegister<Register>());
  } else {
    DCHECK(cls.IsStackSlot()) << cls;
    __ cmpl(out, Address(ESP, cls.GetStackIndex()));
  }

  if (instruction->IsClassFinal()) {
    // Classes must be equal for the instanceof to succeed.
    __ j(kNotEqual, &zero);
    __ movl(out, Immediate(1));
    __ jmp(&done);
  } else {
    // If the classes are not equal, we go into a slow path.
    DCHECK(locations->OnlyCallsOnSlowPath());
    slow_path = new (GetGraph()->GetArena()) TypeCheckSlowPathX86(
        instruction, locations->InAt(1), locations->Out(), instruction->GetDexPc());
    codegen_->AddSlowPath(slow_path);
    __ j(kNotEqual, slow_path->GetEntryLabel());
    __ movl(out, Immediate(1));
    __ jmp(&done);
  }

  if (instruction->MustDoNullCheck() || instruction->IsClassFinal()) {
    __ Bind(&zero);
    __ movl(out, Immediate(0));
  }

  if (slow_path != nullptr) {
    __ Bind(slow_path->GetExitLabel());
  }
  __ Bind(&done);
}

void LocationsBuilderX86::VisitCheckCast(HCheckCast* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(
      instruction, LocationSummary::kCallOnSlowPath);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::Any());
  locations->AddTemp(Location::RequiresRegister());
}

void InstructionCodeGeneratorX86::VisitCheckCast(HCheckCast* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Register obj = locations->InAt(0).AsRegister<Register>();
  Location cls = locations->InAt(1);
  Register temp = locations->GetTemp(0).AsRegister<Register>();
  uint32_t class_offset = mirror::Object::ClassOffset().Int32Value();
  SlowPathCodeX86* slow_path = new (GetGraph()->GetArena()) TypeCheckSlowPathX86(
      instruction, locations->InAt(1), locations->GetTemp(0), instruction->GetDexPc());
  codegen_->AddSlowPath(slow_path);

  // Avoid null check if we know obj is not null.
  if (instruction->MustDoNullCheck()) {
    __ testl(obj, obj);
    __ j(kEqual, slow_path->GetExitLabel());
  }

  __ movl(temp, Address(obj, class_offset));
  // Compare the class of `obj` with `cls`.
  if (cls.IsRegister()) {
    __ cmpl(temp, cls.AsRegister<Register>());
  } else {
    DCHECK(cls.IsStackSlot()) << cls;
    __ cmpl(temp, Address(ESP, cls.GetStackIndex()));
  }

  __ j(kNotEqual, slow_path->GetEntryLabel());
  __ Bind(slow_path->GetExitLabel());
}

void LocationsBuilderX86::VisitMonitorOperation(HMonitorOperation* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCall);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
}

void InstructionCodeGeneratorX86::VisitMonitorOperation(HMonitorOperation* instruction) {
  __ fs()->call(Address::Absolute(instruction->IsEnter()
        ? QUICK_ENTRYPOINT_OFFSET(kX86WordSize, pLockObject)
        : QUICK_ENTRYPOINT_OFFSET(kX86WordSize, pUnlockObject)));
  codegen_->RecordPcInfo(instruction, instruction->GetDexPc());
}

void LocationsBuilderX86::VisitAnd(HAnd* instruction) { HandleBitwiseOperation(instruction); }
void LocationsBuilderX86::VisitOr(HOr* instruction) { HandleBitwiseOperation(instruction); }
void LocationsBuilderX86::VisitXor(HXor* instruction) { HandleBitwiseOperation(instruction); }

void LocationsBuilderX86::HandleBitwiseOperation(HBinaryOperation* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  DCHECK(instruction->GetResultType() == Primitive::kPrimInt
         || instruction->GetResultType() == Primitive::kPrimLong);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::Any());
  locations->SetOut(Location::SameAsFirstInput());
}

void InstructionCodeGeneratorX86::VisitAnd(HAnd* instruction) {
  HandleBitwiseOperation(instruction);
}

void InstructionCodeGeneratorX86::VisitOr(HOr* instruction) {
  HandleBitwiseOperation(instruction);
}

void InstructionCodeGeneratorX86::VisitXor(HXor* instruction) {
  HandleBitwiseOperation(instruction);
}

void InstructionCodeGeneratorX86::HandleBitwiseOperation(HBinaryOperation* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Location first = locations->InAt(0);
  Location second = locations->InAt(1);
  DCHECK(first.Equals(locations->Out()));

  if (instruction->GetResultType() == Primitive::kPrimInt) {
    if (second.IsRegister()) {
      if (instruction->IsAnd()) {
        __ andl(first.AsRegister<Register>(), second.AsRegister<Register>());
      } else if (instruction->IsOr()) {
        __ orl(first.AsRegister<Register>(), second.AsRegister<Register>());
      } else {
        DCHECK(instruction->IsXor());
        __ xorl(first.AsRegister<Register>(), second.AsRegister<Register>());
      }
    } else if (second.IsConstant()) {
      if (instruction->IsAnd()) {
        __ andl(first.AsRegister<Register>(),
                Immediate(second.GetConstant()->AsIntConstant()->GetValue()));
      } else if (instruction->IsOr()) {
        __ orl(first.AsRegister<Register>(),
               Immediate(second.GetConstant()->AsIntConstant()->GetValue()));
      } else {
        DCHECK(instruction->IsXor());
        __ xorl(first.AsRegister<Register>(),
                Immediate(second.GetConstant()->AsIntConstant()->GetValue()));
      }
    } else {
      if (instruction->IsAnd()) {
        __ andl(first.AsRegister<Register>(), Address(ESP, second.GetStackIndex()));
      } else if (instruction->IsOr()) {
        __ orl(first.AsRegister<Register>(), Address(ESP, second.GetStackIndex()));
      } else {
        DCHECK(instruction->IsXor());
        __ xorl(first.AsRegister<Register>(), Address(ESP, second.GetStackIndex()));
      }
    }
  } else {
    DCHECK_EQ(instruction->GetResultType(), Primitive::kPrimLong);
    if (second.IsRegisterPair()) {
      if (instruction->IsAnd()) {
        __ andl(first.AsRegisterPairLow<Register>(), second.AsRegisterPairLow<Register>());
        __ andl(first.AsRegisterPairHigh<Register>(), second.AsRegisterPairHigh<Register>());
      } else if (instruction->IsOr()) {
        __ orl(first.AsRegisterPairLow<Register>(), second.AsRegisterPairLow<Register>());
        __ orl(first.AsRegisterPairHigh<Register>(), second.AsRegisterPairHigh<Register>());
      } else {
        DCHECK(instruction->IsXor());
        __ xorl(first.AsRegisterPairLow<Register>(), second.AsRegisterPairLow<Register>());
        __ xorl(first.AsRegisterPairHigh<Register>(), second.AsRegisterPairHigh<Register>());
      }
    } else if (second.IsDoubleStackSlot()) {
      if (instruction->IsAnd()) {
        __ andl(first.AsRegisterPairLow<Register>(), Address(ESP, second.GetStackIndex()));
        __ andl(first.AsRegisterPairHigh<Register>(),
                Address(ESP, second.GetHighStackIndex(kX86WordSize)));
      } else if (instruction->IsOr()) {
        __ orl(first.AsRegisterPairLow<Register>(), Address(ESP, second.GetStackIndex()));
        __ orl(first.AsRegisterPairHigh<Register>(),
                Address(ESP, second.GetHighStackIndex(kX86WordSize)));
      } else {
        DCHECK(instruction->IsXor());
        __ xorl(first.AsRegisterPairLow<Register>(), Address(ESP, second.GetStackIndex()));
        __ xorl(first.AsRegisterPairHigh<Register>(),
                Address(ESP, second.GetHighStackIndex(kX86WordSize)));
      }
    } else {
      DCHECK(second.IsConstant()) << second;
      int64_t value = second.GetConstant()->AsLongConstant()->GetValue();
      int32_t low_value = Low32Bits(value);
      int32_t high_value = High32Bits(value);
      Immediate low(low_value);
      Immediate high(high_value);
      Register first_low = first.AsRegisterPairLow<Register>();
      Register first_high = first.AsRegisterPairHigh<Register>();
      if (instruction->IsAnd()) {
        if (low_value == 0) {
          __ xorl(first_low, first_low);
        } else if (low_value != -1) {
          __ andl(first_low, low);
        }
        if (high_value == 0) {
          __ xorl(first_high, first_high);
        } else if (high_value != -1) {
          __ andl(first_high, high);
        }
      } else if (instruction->IsOr()) {
        if (low_value != 0) {
          __ orl(first_low, low);
        }
        if (high_value != 0) {
          __ orl(first_high, high);
        }
      } else {
        DCHECK(instruction->IsXor());
        if (low_value != 0) {
          __ xorl(first_low, low);
        }
        if (high_value != 0) {
          __ xorl(first_high, high);
        }
      }
    }
  }
}

void LocationsBuilderX86::VisitBoundType(HBoundType* instruction) {
  // Nothing to do, this should be removed during prepare for register allocator.
  UNUSED(instruction);
  LOG(FATAL) << "Unreachable";
}

void InstructionCodeGeneratorX86::VisitBoundType(HBoundType* instruction) {
  // Nothing to do, this should be removed during prepare for register allocator.
  UNUSED(instruction);
  LOG(FATAL) << "Unreachable";
}

}  // namespace x86
}  // namespace art
