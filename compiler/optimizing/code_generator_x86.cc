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

#include "entrypoints/quick/quick_entrypoints.h"
#include "gc/accounting/card_table.h"
#include "mirror/array-inl.h"
#include "mirror/art_method.h"
#include "mirror/class.h"
#include "thread.h"
#include "utils/assembler.h"
#include "utils/stack_checks.h"
#include "utils/x86/assembler_x86.h"
#include "utils/x86/managed_register_x86.h"

namespace art {

namespace x86 {

static constexpr bool kExplicitStackOverflowCheck = false;

static constexpr int kNumberOfPushedRegistersAtEntry = 1;
static constexpr int kCurrentMethodStackOffset = 0;

static constexpr Register kRuntimeParameterCoreRegisters[] = { EAX, ECX, EDX, EBX };
static constexpr size_t kRuntimeParameterCoreRegistersLength =
    arraysize(kRuntimeParameterCoreRegisters);
static constexpr XmmRegister kRuntimeParameterFpuRegisters[] = { };
static constexpr size_t kRuntimeParameterFpuRegistersLength = 0;

// Marker for places that can be updated once we don't follow the quick ABI.
static constexpr bool kFollowsQuickABI = true;

class InvokeRuntimeCallingConvention : public CallingConvention<Register, XmmRegister> {
 public:
  InvokeRuntimeCallingConvention()
      : CallingConvention(kRuntimeParameterCoreRegisters,
                          kRuntimeParameterCoreRegistersLength,
                          kRuntimeParameterFpuRegisters,
                          kRuntimeParameterFpuRegistersLength) {}

 private:
  DISALLOW_COPY_AND_ASSIGN(InvokeRuntimeCallingConvention);
};

#define __ reinterpret_cast<X86Assembler*>(codegen->GetAssembler())->

class SlowPathCodeX86 : public SlowPathCode {
 public:
  SlowPathCodeX86() : entry_label_(), exit_label_() {}

  Label* GetEntryLabel() { return &entry_label_; }
  Label* GetExitLabel() { return &exit_label_; }

 private:
  Label entry_label_;
  Label exit_label_;

  DISALLOW_COPY_AND_ASSIGN(SlowPathCodeX86);
};

class NullCheckSlowPathX86 : public SlowPathCodeX86 {
 public:
  explicit NullCheckSlowPathX86(HNullCheck* instruction) : instruction_(instruction) {}

  virtual void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    __ Bind(GetEntryLabel());
    __ fs()->call(Address::Absolute(QUICK_ENTRYPOINT_OFFSET(kX86WordSize, pThrowNullPointer)));
    codegen->RecordPcInfo(instruction_, instruction_->GetDexPc());
  }

 private:
  HNullCheck* const instruction_;
  DISALLOW_COPY_AND_ASSIGN(NullCheckSlowPathX86);
};

class DivZeroCheckSlowPathX86 : public SlowPathCodeX86 {
 public:
  explicit DivZeroCheckSlowPathX86(HDivZeroCheck* instruction) : instruction_(instruction) {}

  virtual void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    __ Bind(GetEntryLabel());
    __ fs()->call(Address::Absolute(QUICK_ENTRYPOINT_OFFSET(kX86WordSize, pThrowDivZero)));
    codegen->RecordPcInfo(instruction_, instruction_->GetDexPc());
  }

 private:
  HDivZeroCheck* const instruction_;
  DISALLOW_COPY_AND_ASSIGN(DivZeroCheckSlowPathX86);
};

class DivRemMinusOneSlowPathX86 : public SlowPathCodeX86 {
 public:
  explicit DivRemMinusOneSlowPathX86(Register reg, bool is_div) : reg_(reg), is_div_(is_div) {}

  virtual void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
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

class StackOverflowCheckSlowPathX86 : public SlowPathCodeX86 {
 public:
  StackOverflowCheckSlowPathX86() {}

  virtual void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    __ Bind(GetEntryLabel());
    __ addl(ESP,
            Immediate(codegen->GetFrameSize() - kNumberOfPushedRegistersAtEntry * kX86WordSize));
    __ fs()->jmp(Address::Absolute(QUICK_ENTRYPOINT_OFFSET(kX86WordSize, pThrowStackOverflow)));
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(StackOverflowCheckSlowPathX86);
};

class BoundsCheckSlowPathX86 : public SlowPathCodeX86 {
 public:
  BoundsCheckSlowPathX86(HBoundsCheck* instruction,
                         Location index_location,
                         Location length_location)
      : instruction_(instruction),
        index_location_(index_location),
        length_location_(length_location) {}

  virtual void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    CodeGeneratorX86* x86_codegen = down_cast<CodeGeneratorX86*>(codegen);
    __ Bind(GetEntryLabel());
    // We're moving two locations to locations that could overlap, so we need a parallel
    // move resolver.
    InvokeRuntimeCallingConvention calling_convention;
    x86_codegen->EmitParallelMoves(
        index_location_,
        Location::RegisterLocation(calling_convention.GetRegisterAt(0)),
        length_location_,
        Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
    __ fs()->call(Address::Absolute(QUICK_ENTRYPOINT_OFFSET(kX86WordSize, pThrowArrayBounds)));
    codegen->RecordPcInfo(instruction_, instruction_->GetDexPc());
  }

 private:
  HBoundsCheck* const instruction_;
  const Location index_location_;
  const Location length_location_;

  DISALLOW_COPY_AND_ASSIGN(BoundsCheckSlowPathX86);
};

class SuspendCheckSlowPathX86 : public SlowPathCodeX86 {
 public:
  explicit SuspendCheckSlowPathX86(HSuspendCheck* instruction, HBasicBlock* successor)
      : instruction_(instruction), successor_(successor) {}

  virtual void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    CodeGeneratorX86* x86_codegen = down_cast<CodeGeneratorX86*>(codegen);
    __ Bind(GetEntryLabel());
    codegen->SaveLiveRegisters(instruction_->GetLocations());
    __ fs()->call(Address::Absolute(QUICK_ENTRYPOINT_OFFSET(kX86WordSize, pTestSuspend)));
    codegen->RecordPcInfo(instruction_, instruction_->GetDexPc());
    codegen->RestoreLiveRegisters(instruction_->GetLocations());
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

 private:
  HSuspendCheck* const instruction_;
  HBasicBlock* const successor_;
  Label return_label_;

  DISALLOW_COPY_AND_ASSIGN(SuspendCheckSlowPathX86);
};

class LoadStringSlowPathX86 : public SlowPathCodeX86 {
 public:
  explicit LoadStringSlowPathX86(HLoadString* instruction) : instruction_(instruction) {}

  virtual void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    LocationSummary* locations = instruction_->GetLocations();
    DCHECK(!locations->GetLiveRegisters()->ContainsCoreRegister(locations->Out().reg()));

    CodeGeneratorX86* x86_codegen = down_cast<CodeGeneratorX86*>(codegen);
    __ Bind(GetEntryLabel());
    codegen->SaveLiveRegisters(locations);

    InvokeRuntimeCallingConvention calling_convention;
    x86_codegen->LoadCurrentMethod(calling_convention.GetRegisterAt(0));
    __ movl(calling_convention.GetRegisterAt(1), Immediate(instruction_->GetStringIndex()));
    __ fs()->call(Address::Absolute(QUICK_ENTRYPOINT_OFFSET(kX86WordSize, pResolveString)));
    codegen->RecordPcInfo(instruction_, instruction_->GetDexPc());
    x86_codegen->Move32(locations->Out(), Location::RegisterLocation(EAX));
    codegen->RestoreLiveRegisters(locations);

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

  virtual void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    LocationSummary* locations = at_->GetLocations();
    CodeGeneratorX86* x86_codegen = down_cast<CodeGeneratorX86*>(codegen);
    __ Bind(GetEntryLabel());
    codegen->SaveLiveRegisters(locations);

    InvokeRuntimeCallingConvention calling_convention;
    __ movl(calling_convention.GetRegisterAt(0), Immediate(cls_->GetTypeIndex()));
    x86_codegen->LoadCurrentMethod(calling_convention.GetRegisterAt(1));
    __ fs()->call(Address::Absolute(do_clinit_
        ? QUICK_ENTRYPOINT_OFFSET(kX86WordSize, pInitializeStaticStorage)
        : QUICK_ENTRYPOINT_OFFSET(kX86WordSize, pInitializeType)));
    codegen->RecordPcInfo(at_, dex_pc_);

    // Move the class to the desired location.
    Location out = locations->Out();
    if (out.IsValid()) {
      DCHECK(out.IsRegister() && !locations->GetLiveRegisters()->ContainsCoreRegister(out.reg()));
      x86_codegen->Move32(out, Location::RegisterLocation(EAX));
    }

    codegen->RestoreLiveRegisters(locations);
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

  virtual void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    LocationSummary* locations = instruction_->GetLocations();
    DCHECK(instruction_->IsCheckCast()
           || !locations->GetLiveRegisters()->ContainsCoreRegister(locations->Out().reg()));

    CodeGeneratorX86* x86_codegen = down_cast<CodeGeneratorX86*>(codegen);
    __ Bind(GetEntryLabel());
    codegen->SaveLiveRegisters(locations);

    // We're moving two locations to locations that could overlap, so we need a parallel
    // move resolver.
    InvokeRuntimeCallingConvention calling_convention;
    x86_codegen->EmitParallelMoves(
        class_to_check_,
        Location::RegisterLocation(calling_convention.GetRegisterAt(0)),
        object_class_,
        Location::RegisterLocation(calling_convention.GetRegisterAt(1)));

    if (instruction_->IsInstanceOf()) {
      __ fs()->call(Address::Absolute(QUICK_ENTRYPOINT_OFFSET(kX86WordSize,
                                                              pInstanceofNonTrivial)));
    } else {
      DCHECK(instruction_->IsCheckCast());
      __ fs()->call(Address::Absolute(QUICK_ENTRYPOINT_OFFSET(kX86WordSize, pCheckCast)));
    }

    codegen->RecordPcInfo(instruction_, dex_pc_);
    if (instruction_->IsInstanceOf()) {
      x86_codegen->Move32(locations->Out(), Location::RegisterLocation(EAX));
    }
    codegen->RestoreLiveRegisters(locations);

    __ jmp(GetExitLabel());
  }

 private:
  HInstruction* const instruction_;
  const Location class_to_check_;
  const Location object_class_;
  const uint32_t dex_pc_;

  DISALLOW_COPY_AND_ASSIGN(TypeCheckSlowPathX86);
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

CodeGeneratorX86::CodeGeneratorX86(HGraph* graph)
    : CodeGenerator(graph, kNumberOfCpuRegisters, kNumberOfXmmRegisters, kNumberOfRegisterPairs),
      block_labels_(graph->GetArena(), 0),
      location_builder_(graph, this),
      instruction_visitor_(graph, this),
      move_resolver_(graph->GetArena(), this) {}

size_t CodeGeneratorX86::FrameEntrySpillSize() const {
  return kNumberOfPushedRegistersAtEntry * kX86WordSize;
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

void CodeGeneratorX86::SetupBlockedRegisters() const {
  // Don't allocate the dalvik style register pair passing.
  blocked_register_pairs_[ECX_EDX] = true;

  // Stack register is always reserved.
  blocked_core_registers_[ESP] = true;

  // TODO: We currently don't use Quick's callee saved registers.
  DCHECK(kFollowsQuickABI);
  blocked_core_registers_[EBP] = true;
  blocked_core_registers_[ESI] = true;
  blocked_core_registers_[EDI] = true;

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

void CodeGeneratorX86::GenerateFrameEntry() {
  // Create a fake register to mimic Quick.
  static const int kFakeReturnRegister = 8;
  core_spill_mask_ |= (1 << kFakeReturnRegister);

  bool skip_overflow_check =
      IsLeafMethod() && !FrameNeedsStackCheck(GetFrameSize(), InstructionSet::kX86);
  if (!skip_overflow_check && !kExplicitStackOverflowCheck) {
    __ testl(EAX, Address(ESP, -static_cast<int32_t>(GetStackOverflowReservedBytes(kX86))));
    RecordPcInfo(nullptr, 0);
  }

  // The return PC has already been pushed on the stack.
  __ subl(ESP, Immediate(GetFrameSize() - kNumberOfPushedRegistersAtEntry * kX86WordSize));

  if (!skip_overflow_check && kExplicitStackOverflowCheck) {
    SlowPathCodeX86* slow_path = new (GetGraph()->GetArena()) StackOverflowCheckSlowPathX86();
    AddSlowPath(slow_path);

    __ fs()->cmpl(ESP, Address::Absolute(Thread::StackEndOffset<kX86WordSize>()));
    __ j(kLess, slow_path->GetEntryLabel());
  }

  __ movl(Address(ESP, kCurrentMethodStackOffset), EAX);
}

void CodeGeneratorX86::GenerateFrameExit() {
  __ addl(ESP, Immediate(GetFrameSize() - kNumberOfPushedRegistersAtEntry * kX86WordSize));
}

void CodeGeneratorX86::Bind(HBasicBlock* block) {
  __ Bind(GetLabelOf(block));
}

void CodeGeneratorX86::LoadCurrentMethod(Register reg) {
  __ movl(reg, Address(ESP, kCurrentMethodStackOffset));
}

Location CodeGeneratorX86::GetStackLocation(HLoadLocal* load) const {
  switch (load->GetType()) {
    case Primitive::kPrimLong:
    case Primitive::kPrimDouble:
      return Location::DoubleStackSlot(GetStackSlot(load->GetLocal()));
      break;

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
  }

  LOG(FATAL) << "Unreachable";
  return Location();
}

Location InvokeDexCallingConventionVisitor::GetNextLocation(Primitive::Type type) {
  switch (type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimInt:
    case Primitive::kPrimFloat:
    case Primitive::kPrimNot: {
      uint32_t index = gp_index_++;
      if (index < calling_convention.GetNumberOfRegisters()) {
        return Location::RegisterLocation(calling_convention.GetRegisterAt(index));
      } else {
        return Location::StackSlot(calling_convention.GetStackOffsetOf(index));
      }
    }

    case Primitive::kPrimLong:
    case Primitive::kPrimDouble: {
      uint32_t index = gp_index_;
      gp_index_ += 2;
      if (index + 1 < calling_convention.GetNumberOfRegisters()) {
        X86ManagedRegister pair = X86ManagedRegister::FromRegisterPair(
            calling_convention.GetRegisterPairAt(index));
        return Location::RegisterPairLocation(pair.AsRegisterPairLow(), pair.AsRegisterPairHigh());
      } else if (index + 1 == calling_convention.GetNumberOfRegisters()) {
        // On X86, the register index and stack index of a quick parameter is the same, since
        // we are passing floating pointer values in core registers.
        return Location::QuickParameter(index, index);
      } else {
        return Location::DoubleStackSlot(calling_convention.GetStackOffsetOf(index));
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
          Location::RegisterLocation(source.AsRegisterPairLow<Register>()),
          Location::RegisterLocation(destination.AsRegisterPairLow<Register>()));
    } else if (source.IsFpuRegister()) {
      LOG(FATAL) << "Unimplemented";
    } else if (source.IsQuickParameter()) {
      uint16_t register_index = source.GetQuickParameterRegisterIndex();
      uint16_t stack_index = source.GetQuickParameterStackIndex();
      InvokeDexCallingConvention calling_convention;
      EmitParallelMoves(
          Location::RegisterLocation(calling_convention.GetRegisterAt(register_index)),
          Location::RegisterLocation(destination.AsRegisterPairLow<Register>()),
          Location::StackSlot(
              calling_convention.GetStackOffsetOf(stack_index + 1) + GetFrameSize()),
          Location::RegisterLocation(destination.AsRegisterPairHigh<Register>()));
    } else {
      // No conflict possible, so just do the moves.
      DCHECK(source.IsDoubleStackSlot());
      __ movl(destination.AsRegisterPairLow<Register>(), Address(ESP, source.GetStackIndex()));
      __ movl(destination.AsRegisterPairHigh<Register>(),
              Address(ESP, source.GetHighStackIndex(kX86WordSize)));
    }
  } else if (destination.IsQuickParameter()) {
    InvokeDexCallingConvention calling_convention;
    uint16_t register_index = destination.GetQuickParameterRegisterIndex();
    uint16_t stack_index = destination.GetQuickParameterStackIndex();
    if (source.IsRegisterPair()) {
      LOG(FATAL) << "Unimplemented";
    } else if (source.IsFpuRegister()) {
      LOG(FATAL) << "Unimplemented";
    } else {
      DCHECK(source.IsDoubleStackSlot());
      EmitParallelMoves(
          Location::StackSlot(source.GetStackIndex()),
          Location::RegisterLocation(calling_convention.GetRegisterAt(register_index)),
          Location::StackSlot(source.GetHighStackIndex(kX86WordSize)),
          Location::StackSlot(calling_convention.GetStackOffsetOf(stack_index + 1)));
    }
  } else if (destination.IsFpuRegister()) {
    if (source.IsDoubleStackSlot()) {
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
    } else if (source.IsQuickParameter()) {
      // No conflict possible, so just do the move.
      InvokeDexCallingConvention calling_convention;
      uint16_t register_index = source.GetQuickParameterRegisterIndex();
      uint16_t stack_index = source.GetQuickParameterStackIndex();
      // Just move the low part. The only time a source is a quick parameter is
      // when moving the parameter to its stack locations. And the (Java) caller
      // of this method has already done that.
      __ movl(Address(ESP, destination.GetStackIndex()),
              calling_convention.GetRegisterAt(register_index));
      DCHECK_EQ(calling_convention.GetStackOffsetOf(stack_index + 1) + GetFrameSize(),
                static_cast<size_t>(destination.GetHighStackIndex(kX86WordSize)));
    } else if (source.IsFpuRegister()) {
      __ movsd(Address(ESP, destination.GetStackIndex()), source.AsFpuRegister<XmmRegister>());
    } else {
      DCHECK(source.IsDoubleStackSlot());
      EmitParallelMoves(
          Location::StackSlot(source.GetStackIndex()),
          Location::StackSlot(destination.GetStackIndex()),
          Location::StackSlot(source.GetHighStackIndex(kX86WordSize)),
          Location::StackSlot(destination.GetHighStackIndex(kX86WordSize)));
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
    if (const_to_move->IsIntConstant()) {
      Immediate imm(const_to_move->AsIntConstant()->GetValue());
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
  if (info != nullptr && info->IsBackEdge(block) && info->HasSuspendCheck()) {
    codegen_->ClearSpillSlotsFromLoopPhisInStackMap(info->GetSuspendCheck());
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
  if (kIsDebugBuild) {
    __ Comment("Unreachable");
    __ int3();
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
  HInstruction* cond = if_instr->InputAt(0);
  if (cond->IsIntConstant()) {
    // Constant condition, statically compared against 1.
    int32_t cond_value = cond->AsIntConstant()->GetValue();
    if (cond_value == 1) {
      if (!codegen_->GoesToNextBlock(if_instr->GetBlock(),
                                     if_instr->IfTrueSuccessor())) {
        __ jmp(codegen_->GetLabelOf(if_instr->IfTrueSuccessor()));
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
        && cond->AsCondition()->IsBeforeWhenDisregardMoves(if_instr);
    if (materialized) {
      if (!eflags_set) {
        // Materialized condition, compare against 0.
        Location lhs = if_instr->GetLocations()->InAt(0);
        if (lhs.IsRegister()) {
          __ cmpl(lhs.AsRegister<Register>(), Immediate(0));
        } else {
          __ cmpl(Address(ESP, lhs.GetStackIndex()), Immediate(0));
        }
        __ j(kNotEqual,  codegen_->GetLabelOf(if_instr->IfTrueSuccessor()));
      } else {
        __ j(X86Condition(cond->AsCondition()->GetCondition()),
             codegen_->GetLabelOf(if_instr->IfTrueSuccessor()));
      }
    } else {
      Location lhs = cond->GetLocations()->InAt(0);
      Location rhs = cond->GetLocations()->InAt(1);
      // LHS is guaranteed to be in a register (see
      // LocationsBuilderX86::VisitCondition).
      if (rhs.IsRegister()) {
        __ cmpl(lhs.AsRegister<Register>(), rhs.AsRegister<Register>());
      } else if (rhs.IsConstant()) {
        HIntConstant* instruction = rhs.GetConstant()->AsIntConstant();
        Immediate imm(instruction->AsIntConstant()->GetValue());
        __ cmpl(lhs.AsRegister<Register>(), imm);
      } else {
        __ cmpl(lhs.AsRegister<Register>(), Address(ESP, rhs.GetStackIndex()));
      }
      __ j(X86Condition(cond->AsCondition()->GetCondition()),
           codegen_->GetLabelOf(if_instr->IfTrueSuccessor()));
    }
  }
  if (!codegen_->GoesToNextBlock(if_instr->GetBlock(),
                                 if_instr->IfFalseSuccessor())) {
    __ jmp(codegen_->GetLabelOf(if_instr->IfFalseSuccessor()));
  }
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
    locations->SetOut(Location::RequiresRegister());
  }
}

void InstructionCodeGeneratorX86::VisitCondition(HCondition* comp) {
  if (comp->NeedsMaterialization()) {
    LocationSummary* locations = comp->GetLocations();
    Register reg = locations->Out().AsRegister<Register>();
    // Clear register: setcc only sets the low byte.
    __ xorl(reg, reg);
    if (locations->InAt(1).IsRegister()) {
      __ cmpl(locations->InAt(0).AsRegister<Register>(),
              locations->InAt(1).AsRegister<Register>());
    } else if (locations->InAt(1).IsConstant()) {
      HConstant* instruction = locations->InAt(1).GetConstant();
      Immediate imm(instruction->AsIntConstant()->GetValue());
      __ cmpl(locations->InAt(0).AsRegister<Register>(), imm);
    } else {
      __ cmpl(locations->InAt(0).AsRegister<Register>(),
              Address(ESP, locations->InAt(1).GetStackIndex()));
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

void LocationsBuilderX86::VisitReturnVoid(HReturnVoid* ret) {
  ret->SetLocations(nullptr);
}

void InstructionCodeGeneratorX86::VisitReturnVoid(HReturnVoid* ret) {
  UNUSED(ret);
  codegen_->GenerateFrameExit();
  __ ret();
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
  __ ret();
}

void LocationsBuilderX86::VisitInvokeStatic(HInvokeStatic* invoke) {
  HandleInvoke(invoke);
}

void InstructionCodeGeneratorX86::VisitInvokeStatic(HInvokeStatic* invoke) {
  Register temp = invoke->GetLocations()->GetTemp(0).AsRegister<Register>();

  // TODO: Implement all kinds of calls:
  // 1) boot -> boot
  // 2) app -> boot
  // 3) app -> app
  //
  // Currently we implement the app -> app logic, which looks up in the resolve cache.

  // temp = method;
  codegen_->LoadCurrentMethod(temp);
  // temp = temp->dex_cache_resolved_methods_;
  __ movl(temp, Address(temp, mirror::ArtMethod::DexCacheResolvedMethodsOffset().Int32Value()));
  // temp = temp[index_in_cache]
  __ movl(temp, Address(temp, CodeGenerator::GetCacheOffset(invoke->GetIndexInDexCache())));
  // (temp + offset_of_quick_compiled_code)()
  __ call(Address(
      temp, mirror::ArtMethod::EntryPointFromQuickCompiledCodeOffset(kX86WordSize).Int32Value()));

  DCHECK(!codegen_->IsLeafMethod());
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
}

void LocationsBuilderX86::VisitInvokeVirtual(HInvokeVirtual* invoke) {
  HandleInvoke(invoke);
}

void LocationsBuilderX86::HandleInvoke(HInvoke* invoke) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(invoke, LocationSummary::kCall);
  locations->AddTemp(Location::RegisterLocation(EAX));

  InvokeDexCallingConventionVisitor calling_convention_visitor;
  for (size_t i = 0; i < invoke->InputCount(); i++) {
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
  uint32_t method_offset = mirror::Class::EmbeddedVTableOffset().Uint32Value() +
          invoke->GetVTableIndex() * sizeof(mirror::Class::VTableEntry);
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
  // temp = temp->GetMethodAt(method_offset);
  __ movl(temp, Address(temp, method_offset));
  // call temp->GetEntryPoint();
  __ call(Address(
      temp, mirror::ArtMethod::EntryPointFromQuickCompiledCodeOffset(kX86WordSize).Int32Value()));

  DCHECK(!codegen_->IsLeafMethod());
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
}

void LocationsBuilderX86::VisitInvokeInterface(HInvokeInterface* invoke) {
  HandleInvoke(invoke);
  // Add the hidden argument.
  invoke->GetLocations()->AddTemp(Location::FpuRegisterLocation(XMM0));
}

void InstructionCodeGeneratorX86::VisitInvokeInterface(HInvokeInterface* invoke) {
  // TODO: b/18116999, our IMTs can miss an IncompatibleClassChangeError.
  Register temp = invoke->GetLocations()->GetTemp(0).AsRegister<Register>();
  uint32_t method_offset = mirror::Class::EmbeddedImTableOffset().Uint32Value() +
          (invoke->GetImtIndex() % mirror::Class::kImtSize) * sizeof(mirror::Class::ImTableEntry);
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
  // temp = temp->GetImtEntryAt(method_offset);
  __ movl(temp, Address(temp, method_offset));
  // call temp->GetEntryPoint();
  __ call(Address(temp, mirror::ArtMethod::EntryPointFromQuickCompiledCodeOffset(
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

  // Float-to-long conversions invoke the runtime.
  LocationSummary::CallKind call_kind =
      (input_type == Primitive::kPrimFloat && result_type == Primitive::kPrimLong)
      ? LocationSummary::kCall
      : LocationSummary::kNoCall;
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(conversion, call_kind);

  switch (result_type) {
    case Primitive::kPrimByte:
      switch (input_type) {
        case Primitive::kPrimShort:
        case Primitive::kPrimInt:
        case Primitive::kPrimChar:
          // Processing a Dex `int-to-byte' instruction.
          locations->SetInAt(0, Location::Any());
          locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      }
      break;

    case Primitive::kPrimShort:
      switch (input_type) {
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
          LOG(FATAL) << "Type conversion from " << input_type
                     << " to " << result_type << " not yet implemented";
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      }
      break;

    case Primitive::kPrimLong:
      switch (input_type) {
        case Primitive::kPrimByte:
        case Primitive::kPrimShort:
        case Primitive::kPrimInt:
        case Primitive::kPrimChar:
          // Processing a Dex `int-to-long' instruction.
          locations->SetInAt(0, Location::RegisterLocation(EAX));
          locations->SetOut(Location::RegisterPairLocation(EAX, EDX));
          break;

        case Primitive::kPrimFloat: {
          // Processing a Dex `float-to-long' instruction.
          InvokeRuntimeCallingConvention calling_convention;
          locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
          // The runtime helper puts the result in EAX, EDX.
          locations->SetOut(Location::RegisterPairLocation(EAX, EDX));
          break;
        }

        case Primitive::kPrimDouble:
          LOG(FATAL) << "Type conversion from " << input_type << " to "
                     << result_type << " not yet implemented";
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      }
      break;

    case Primitive::kPrimChar:
      switch (input_type) {
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
          locations->AddTemp(Location::RequiresFpuRegister());
          locations->AddTemp(Location::RequiresFpuRegister());
          break;

        case Primitive::kPrimDouble:
          LOG(FATAL) << "Type conversion from " << input_type
                     << " to " << result_type << " not yet implemented";
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      };
      break;

    case Primitive::kPrimDouble:
      switch (input_type) {
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
          locations->AddTemp(Location::RequiresFpuRegister());
          locations->AddTemp(Location::RequiresFpuRegister());
          break;

        case Primitive::kPrimFloat:
          LOG(FATAL) << "Type conversion from " << input_type
                     << " to " << result_type << " not yet implemented";
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
        case Primitive::kPrimShort:
        case Primitive::kPrimInt:
        case Primitive::kPrimChar:
          // Processing a Dex `int-to-byte' instruction.
          if (in.IsRegister()) {
            __ movsxb(out.AsRegister<Register>(), in.AsRegister<ByteRegister>());
          } else if (in.IsStackSlot()) {
            __ movsxb(out.AsRegister<Register>(), Address(ESP, in.GetStackIndex()));
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

        case Primitive::kPrimDouble:
          LOG(FATAL) << "Type conversion from " << input_type
                     << " to " << result_type << " not yet implemented";
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      }
      break;

    case Primitive::kPrimLong:
      switch (input_type) {
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
          // This call does not actually record PC information.
          codegen_->RecordPcInfo(conversion, conversion->GetDexPc());
          break;

        case Primitive::kPrimDouble:
          LOG(FATAL) << "Type conversion from " << input_type << " to "
                     << result_type << " not yet implemented";
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      }
      break;

    case Primitive::kPrimChar:
      switch (input_type) {
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
        case Primitive::kPrimByte:
        case Primitive::kPrimShort:
        case Primitive::kPrimInt:
        case Primitive::kPrimChar:
          // Processing a Dex `int-to-float' instruction.
          __ cvtsi2ss(out.AsFpuRegister<XmmRegister>(), in.AsRegister<Register>());
          break;

        case Primitive::kPrimLong: {
          // Processing a Dex `long-to-float' instruction.
          Register low = in.AsRegisterPairLow<Register>();
          Register high = in.AsRegisterPairHigh<Register>();
          XmmRegister result = out.AsFpuRegister<XmmRegister>();
          XmmRegister temp = locations->GetTemp(0).AsFpuRegister<XmmRegister>();
          XmmRegister constant = locations->GetTemp(1).AsFpuRegister<XmmRegister>();

          // Operations use doubles for precision reasons (each 32-bit
          // half of a long fits in the 53-bit mantissa of a double,
          // but not in the 24-bit mantissa of a float).  This is
          // especially important for the low bits.  The result is
          // eventually converted to float.

          // low = low - 2^31 (to prevent bit 31 of `low` to be
          // interpreted as a sign bit)
          __ subl(low, Immediate(0x80000000));
          // temp = int-to-double(high)
          __ cvtsi2sd(temp, high);
          // temp = temp * 2^32
          __ LoadLongConstant(constant, k2Pow32EncodingForDouble);
          __ mulsd(temp, constant);
          // result = int-to-double(low)
          __ cvtsi2sd(result, low);
          // result = result + 2^31 (restore the original value of `low`)
          __ LoadLongConstant(constant, k2Pow31EncodingForDouble);
          __ addsd(result, constant);
          // result = result + temp
          __ addsd(result, temp);
          // result = double-to-float(result)
          __ cvtsd2ss(result, result);
          break;
        }

        case Primitive::kPrimDouble:
          LOG(FATAL) << "Type conversion from " << input_type
                     << " to " << result_type << " not yet implemented";
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      };
      break;

    case Primitive::kPrimDouble:
      switch (input_type) {
        case Primitive::kPrimByte:
        case Primitive::kPrimShort:
        case Primitive::kPrimInt:
        case Primitive::kPrimChar:
          // Processing a Dex `int-to-double' instruction.
          __ cvtsi2sd(out.AsFpuRegister<XmmRegister>(), in.AsRegister<Register>());
          break;

        case Primitive::kPrimLong: {
          // Processing a Dex `long-to-double' instruction.
          Register low = in.AsRegisterPairLow<Register>();
          Register high = in.AsRegisterPairHigh<Register>();
          XmmRegister result = out.AsFpuRegister<XmmRegister>();
          XmmRegister temp = locations->GetTemp(0).AsFpuRegister<XmmRegister>();
          XmmRegister constant = locations->GetTemp(1).AsFpuRegister<XmmRegister>();

          // low = low - 2^31 (to prevent bit 31 of `low` to be
          // interpreted as a sign bit)
          __ subl(low, Immediate(0x80000000));
          // temp = int-to-double(high)
          __ cvtsi2sd(temp, high);
          // temp = temp * 2^32
          __ LoadLongConstant(constant, k2Pow32EncodingForDouble);
          __ mulsd(temp, constant);
          // result = int-to-double(low)
          __ cvtsi2sd(result, low);
          // result = result + 2^31 (restore the original value of `low`)
          __ LoadLongConstant(constant, k2Pow31EncodingForDouble);
          __ addsd(result, constant);
          // result = result + temp
          __ addsd(result, temp);
          break;
        }

        case Primitive::kPrimFloat:
          LOG(FATAL) << "Type conversion from " << input_type
                     << " to " << result_type << " not yet implemented";
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
      locations->SetInAt(1, Location::Any());
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
  DCHECK(first.Equals(locations->Out()));
  switch (add->GetResultType()) {
    case Primitive::kPrimInt: {
      if (second.IsRegister()) {
        __ addl(first.AsRegister<Register>(), second.AsRegister<Register>());
      } else if (second.IsConstant()) {
        __ addl(first.AsRegister<Register>(),
                Immediate(second.GetConstant()->AsIntConstant()->GetValue()));
      } else {
        __ addl(first.AsRegister<Register>(), Address(ESP, second.GetStackIndex()));
      }
      break;
    }

    case Primitive::kPrimLong: {
      if (second.IsRegisterPair()) {
        __ addl(first.AsRegisterPairLow<Register>(), second.AsRegisterPairLow<Register>());
        __ adcl(first.AsRegisterPairHigh<Register>(), second.AsRegisterPairHigh<Register>());
      } else {
        __ addl(first.AsRegisterPairLow<Register>(), Address(ESP, second.GetStackIndex()));
        __ adcl(first.AsRegisterPairHigh<Register>(),
                Address(ESP, second.GetHighStackIndex(kX86WordSize)));
      }
      break;
    }

    case Primitive::kPrimFloat: {
      if (second.IsFpuRegister()) {
        __ addss(first.AsFpuRegister<XmmRegister>(), second.AsFpuRegister<XmmRegister>());
      } else {
        __ addss(first.AsFpuRegister<XmmRegister>(), Address(ESP, second.GetStackIndex()));
      }
      break;
    }

    case Primitive::kPrimDouble: {
      if (second.IsFpuRegister()) {
        __ addsd(first.AsFpuRegister<XmmRegister>(), second.AsFpuRegister<XmmRegister>());
      } else {
        __ addsd(first.AsFpuRegister<XmmRegister>(), Address(ESP, second.GetStackIndex()));
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
      } else {
        __ subl(first.AsRegisterPairLow<Register>(), Address(ESP, second.GetStackIndex()));
        __ sbbl(first.AsRegisterPairHigh<Register>(),
                Address(ESP, second.GetHighStackIndex(kX86WordSize)));
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
      // TODO: Currently this handles only stack operands:
      // - we don't have enough registers because we currently use Quick ABI.
      // - by the time we have a working register allocator we will probably change the ABI
      // and fix the above.
      // - we don't have a way yet to request operands on stack but the base line compiler
      // will leave the operands on the stack with Any().
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
      DCHECK(second.IsDoubleStackSlot());

      Register in1_hi = first.AsRegisterPairHigh<Register>();
      Register in1_lo = first.AsRegisterPairLow<Register>();
      Address in2_hi(ESP, second.GetHighStackIndex(kX86WordSize));
      Address in2_lo(ESP, second.GetStackIndex());
      Register eax = locations->GetTemp(0).AsRegister<Register>();
      Register edx = locations->GetTemp(1).AsRegister<Register>();

      DCHECK_EQ(EAX, eax);
      DCHECK_EQ(EDX, edx);

      // input: in1 - 64 bits, in2 - 64 bits
      // output: in1
      // formula: in1.hi : in1.lo = (in1.lo * in2.hi + in1.hi * in2.lo)* 2^32 + in1.lo * in2.lo
      // parts: in1.hi = in1.lo * in2.hi + in1.hi * in2.lo + (in1.lo * in2.lo)[63:32]
      // parts: in1.lo = (in1.lo * in2.lo)[31:0]

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

void InstructionCodeGeneratorX86::GenerateDivRemIntegral(HBinaryOperation* instruction) {
  DCHECK(instruction->IsDiv() || instruction->IsRem());

  LocationSummary* locations = instruction->GetLocations();
  Location out = locations->Out();
  Location first = locations->InAt(0);
  Location second = locations->InAt(1);
  bool is_div = instruction->IsDiv();

  switch (instruction->GetResultType()) {
    case Primitive::kPrimInt: {
      Register second_reg = second.AsRegister<Register>();
      DCHECK_EQ(EAX, first.AsRegister<Register>());
      DCHECK_EQ(is_div ? EAX : EDX, out.AsRegister<Register>());

      SlowPathCodeX86* slow_path =
          new (GetGraph()->GetArena()) DivRemMinusOneSlowPathX86(out.AsRegister<Register>(),
                                                                 is_div);
      codegen_->AddSlowPath(slow_path);

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
  LocationSummary::CallKind call_kind = div->GetResultType() == Primitive::kPrimLong
      ? LocationSummary::kCall
      : LocationSummary::kNoCall;
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(div, call_kind);

  switch (div->GetResultType()) {
    case Primitive::kPrimInt: {
      locations->SetInAt(0, Location::RegisterLocation(EAX));
      locations->SetInAt(1, Location::RequiresRegister());
      locations->SetOut(Location::SameAsFirstInput());
      // Intel uses edx:eax as the dividend.
      locations->AddTemp(Location::RegisterLocation(EDX));
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
  LocationSummary::CallKind call_kind = rem->GetResultType() == Primitive::kPrimLong
      ? LocationSummary::kCall
      : LocationSummary::kNoCall;
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(rem, call_kind);

  switch (rem->GetResultType()) {
    case Primitive::kPrimInt: {
      locations->SetInAt(0, Location::RegisterLocation(EAX));
      locations->SetInAt(1, Location::RequiresRegister());
      locations->SetOut(Location::RegisterLocation(EDX));
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
      LOG(FATAL) << "Unimplemented rem type " << rem->GetResultType();
      break;
    }

    default:
      LOG(FATAL) << "Unexpected rem type " << rem->GetResultType();
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
      LOG(FATAL) << "Unimplemented rem type " << type;
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
    case Primitive::kPrimInt: {
      locations->SetInAt(0, Location::RequiresRegister());
      // The shift count needs to be in CL.
      locations->SetInAt(1, Location::ByteRegisterOrConstant(ECX, op->InputAt(1)));
      locations->SetOut(Location::SameAsFirstInput());
      break;
    }
    case Primitive::kPrimLong: {
      locations->SetInAt(0, Location::RequiresRegister());
      // The shift count needs to be in CL.
      locations->SetInAt(1, Location::RegisterLocation(ECX));
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
        Immediate imm(second.GetConstant()->AsIntConstant()->GetValue());
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
      Register second_reg = second.AsRegister<Register>();
      DCHECK_EQ(ECX, second_reg);
      if (op->IsShl()) {
        GenerateShlLong(first, second_reg);
      } else if (op->IsShr()) {
        GenerateShrLong(first, second_reg);
      } else {
        GenerateUShrLong(first, second_reg);
      }
      break;
    }
    default:
      LOG(FATAL) << "Unexpected op type " << op->GetResultType();
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

  __ fs()->call(
      Address::Absolute(QUICK_ENTRYPOINT_OFFSET(kX86WordSize, pAllocObjectWithAccessCheck)));

  codegen_->RecordPcInfo(instruction, instruction->GetDexPc());
  DCHECK(!codegen_->IsLeafMethod());
}

void LocationsBuilderX86::VisitNewArray(HNewArray* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCall);
  locations->SetOut(Location::RegisterLocation(EAX));
  InvokeRuntimeCallingConvention calling_convention;
  locations->AddTemp(Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
  locations->AddTemp(Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(2)));
}

void InstructionCodeGeneratorX86::VisitNewArray(HNewArray* instruction) {
  InvokeRuntimeCallingConvention calling_convention;
  codegen_->LoadCurrentMethod(calling_convention.GetRegisterAt(1));
  __ movl(calling_convention.GetRegisterAt(0), Immediate(instruction->GetTypeIndex()));

  __ fs()->call(
      Address::Absolute(QUICK_ENTRYPOINT_OFFSET(kX86WordSize, pAllocArrayWithAccessCheck)));

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
  switch (not_->InputAt(0)->GetType()) {
    case Primitive::kPrimBoolean:
      __ xorl(out.AsRegister<Register>(), Immediate(1));
      break;

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

void LocationsBuilderX86::VisitCompare(HCompare* compare) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(compare, LocationSummary::kNoCall);
  switch (compare->InputAt(0)->GetType()) {
    case Primitive::kPrimLong: {
      locations->SetInAt(0, Location::RequiresRegister());
      // TODO: we set any here but we don't handle constants
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
      if (right.IsRegisterPair()) {
        __ cmpl(left.AsRegisterPairHigh<Register>(), right.AsRegisterPairHigh<Register>());
      } else {
        DCHECK(right.IsDoubleStackSlot());
        __ cmpl(left.AsRegisterPairHigh<Register>(),
                Address(ESP, right.GetHighStackIndex(kX86WordSize)));
      }
      __ j(kLess, &less);  // Signed compare.
      __ j(kGreater, &greater);  // Signed compare.
      if (right.IsRegisterPair()) {
        __ cmpl(left.AsRegisterPairLow<Register>(), right.AsRegisterPairLow<Register>());
      } else {
        DCHECK(right.IsDoubleStackSlot());
        __ cmpl(left.AsRegisterPairLow<Register>(), Address(ESP, right.GetStackIndex()));
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

void LocationsBuilderX86::VisitInstanceFieldSet(HInstanceFieldSet* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  Primitive::Type field_type = instruction->GetFieldType();
  bool needs_write_barrier =
    CodeGenerator::StoreNeedsWriteBarrier(field_type, instruction->InputAt(1));

  bool is_byte_type = (field_type == Primitive::kPrimBoolean)
      || (field_type == Primitive::kPrimByte);
  // The register allocator does not support multiple
  // inputs that die at entry with one in a specific register.
  if (is_byte_type) {
    // Ensure the value is in a byte register.
    locations->SetInAt(1, Location::RegisterLocation(EAX));
  } else {
    locations->SetInAt(1, Location::RequiresRegister());
  }
  // Temporary registers for the write barrier.
  if (needs_write_barrier) {
    locations->AddTemp(Location::RequiresRegister());
    // Ensure the card is in a byte register.
    locations->AddTemp(Location::RegisterLocation(ECX));
  }
}

void InstructionCodeGeneratorX86::VisitInstanceFieldSet(HInstanceFieldSet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Register obj = locations->InAt(0).AsRegister<Register>();
  uint32_t offset = instruction->GetFieldOffset().Uint32Value();
  Primitive::Type field_type = instruction->GetFieldType();

  switch (field_type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte: {
      ByteRegister value = locations->InAt(1).AsRegister<ByteRegister>();
      __ movb(Address(obj, offset), value);
      break;
    }

    case Primitive::kPrimShort:
    case Primitive::kPrimChar: {
      Register value = locations->InAt(1).AsRegister<Register>();
      __ movw(Address(obj, offset), value);
      break;
    }

    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      Register value = locations->InAt(1).AsRegister<Register>();
      __ movl(Address(obj, offset), value);

      if (CodeGenerator::StoreNeedsWriteBarrier(field_type, instruction->InputAt(1))) {
        Register temp = locations->GetTemp(0).AsRegister<Register>();
        Register card = locations->GetTemp(1).AsRegister<Register>();
        codegen_->MarkGCCard(temp, card, obj, value);
      }
      break;
    }

    case Primitive::kPrimLong: {
      Location value = locations->InAt(1);
      __ movl(Address(obj, offset), value.AsRegisterPairLow<Register>());
      __ movl(Address(obj, kX86WordSize + offset), value.AsRegisterPairHigh<Register>());
      break;
    }

    case Primitive::kPrimFloat: {
      XmmRegister value = locations->InAt(1).AsFpuRegister<XmmRegister>();
      __ movss(Address(obj, offset), value);
      break;
    }

    case Primitive::kPrimDouble: {
      XmmRegister value = locations->InAt(1).AsFpuRegister<XmmRegister>();
      __ movsd(Address(obj, offset), value);
      break;
    }

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << field_type;
      UNREACHABLE();
  }
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

void LocationsBuilderX86::VisitInstanceFieldGet(HInstanceFieldGet* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

void InstructionCodeGeneratorX86::VisitInstanceFieldGet(HInstanceFieldGet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Register obj = locations->InAt(0).AsRegister<Register>();
  uint32_t offset = instruction->GetFieldOffset().Uint32Value();

  switch (instruction->GetType()) {
    case Primitive::kPrimBoolean: {
      Register out = locations->Out().AsRegister<Register>();
      __ movzxb(out, Address(obj, offset));
      break;
    }

    case Primitive::kPrimByte: {
      Register out = locations->Out().AsRegister<Register>();
      __ movsxb(out, Address(obj, offset));
      break;
    }

    case Primitive::kPrimShort: {
      Register out = locations->Out().AsRegister<Register>();
      __ movsxw(out, Address(obj, offset));
      break;
    }

    case Primitive::kPrimChar: {
      Register out = locations->Out().AsRegister<Register>();
      __ movzxw(out, Address(obj, offset));
      break;
    }

    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      Register out = locations->Out().AsRegister<Register>();
      __ movl(out, Address(obj, offset));
      break;
    }

    case Primitive::kPrimLong: {
      // TODO: support volatile.
      __ movl(locations->Out().AsRegisterPairLow<Register>(), Address(obj, offset));
      __ movl(locations->Out().AsRegisterPairHigh<Register>(), Address(obj, kX86WordSize + offset));
      break;
    }

    case Primitive::kPrimFloat: {
      XmmRegister out = locations->Out().AsFpuRegister<XmmRegister>();
      __ movss(out, Address(obj, offset));
      break;
    }

    case Primitive::kPrimDouble: {
      XmmRegister out = locations->Out().AsFpuRegister<XmmRegister>();
      __ movsd(out, Address(obj, offset));
      break;
    }

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << instruction->GetType();
      UNREACHABLE();
  }
}

void LocationsBuilderX86::VisitNullCheck(HNullCheck* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::Any());
  if (instruction->HasUses()) {
    locations->SetOut(Location::SameAsFirstInput());
  }
}

void InstructionCodeGeneratorX86::VisitNullCheck(HNullCheck* instruction) {
  SlowPathCodeX86* slow_path = new (GetGraph()->GetArena()) NullCheckSlowPathX86(instruction);
  codegen_->AddSlowPath(slow_path);

  LocationSummary* locations = instruction->GetLocations();
  Location obj = locations->InAt(0);

  if (obj.IsRegister()) {
    __ cmpl(obj.AsRegister<Register>(), Immediate(0));
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

void LocationsBuilderX86::VisitArrayGet(HArrayGet* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RegisterOrConstant(instruction->InputAt(1)));
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

void InstructionCodeGeneratorX86::VisitArrayGet(HArrayGet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Register obj = locations->InAt(0).AsRegister<Register>();
  Location index = locations->InAt(1);

  switch (instruction->GetType()) {
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
      if (index.IsConstant()) {
        size_t offset = (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_8) + data_offset;
        __ movl(out.AsRegisterPairLow<Register>(), Address(obj, offset));
        __ movl(out.AsRegisterPairHigh<Register>(), Address(obj, offset + kX86WordSize));
      } else {
        __ movl(out.AsRegisterPairLow<Register>(),
                Address(obj, index.AsRegister<Register>(), TIMES_8, data_offset));
        __ movl(out.AsRegisterPairHigh<Register>(),
                Address(obj, index.AsRegister<Register>(), TIMES_8, data_offset + kX86WordSize));
      }
      break;
    }

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      LOG(FATAL) << "Unimplemented register type " << instruction->GetType();
      UNREACHABLE();
    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << instruction->GetType();
      UNREACHABLE();
  }
}

void LocationsBuilderX86::VisitArraySet(HArraySet* instruction) {
  Primitive::Type value_type = instruction->GetComponentType();
  bool needs_write_barrier =
      CodeGenerator::StoreNeedsWriteBarrier(value_type, instruction->GetValue());

  DCHECK(kFollowsQuickABI);
  bool not_enough_registers = needs_write_barrier
      && !instruction->GetValue()->IsConstant()
      && !instruction->GetIndex()->IsConstant();
  bool needs_runtime_call = instruction->NeedsTypeCheck() || not_enough_registers;

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
          __ movl(Address(obj, offset + kX86WordSize), value.AsRegisterPairHigh<Register>());
        } else {
          DCHECK(value.IsConstant());
          int64_t val = value.GetConstant()->AsLongConstant()->GetValue();
          __ movl(Address(obj, offset), Immediate(Low32Bits(val)));
          __ movl(Address(obj, offset + kX86WordSize), Immediate(High32Bits(val)));
        }
      } else {
        if (value.IsRegisterPair()) {
          __ movl(Address(obj, index.AsRegister<Register>(), TIMES_8, data_offset),
                  value.AsRegisterPairLow<Register>());
          __ movl(Address(obj, index.AsRegister<Register>(), TIMES_8, data_offset + kX86WordSize),
                  value.AsRegisterPairHigh<Register>());
        } else {
          DCHECK(value.IsConstant());
          int64_t val = value.GetConstant()->AsLongConstant()->GetValue();
          __ movl(Address(obj, index.AsRegister<Register>(), TIMES_8, data_offset),
                  Immediate(Low32Bits(val)));
          __ movl(Address(obj, index.AsRegister<Register>(), TIMES_8, data_offset + kX86WordSize),
                  Immediate(High32Bits(val)));
        }
      }
      break;
    }

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      LOG(FATAL) << "Unimplemented register type " << instruction->GetType();
      UNREACHABLE();
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
}

void LocationsBuilderX86::VisitBoundsCheck(HBoundsCheck* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  if (instruction->HasUses()) {
    locations->SetOut(Location::SameAsFirstInput());
  }
}

void InstructionCodeGeneratorX86::VisitBoundsCheck(HBoundsCheck* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  SlowPathCodeX86* slow_path = new (GetGraph()->GetArena()) BoundsCheckSlowPathX86(
      instruction, locations->InAt(0), locations->InAt(1));
  codegen_->AddSlowPath(slow_path);

  Register index = locations->InAt(0).AsRegister<Register>();
  Register length = locations->InAt(1).AsRegister<Register>();

  __ cmpl(index, length);
  __ j(kAboveEqual, slow_path->GetEntryLabel());
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
      new (GetGraph()->GetArena()) SuspendCheckSlowPathX86(instruction, successor);
  codegen_->AddSlowPath(slow_path);
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

void ParallelMoveResolverX86::MoveMemoryToMemory(int dst, int src) {
  ScratchRegisterScope ensure_scratch(
      this, kNoRegister, EAX, codegen_->GetNumberOfCoreRegisters());
  int stack_offset = ensure_scratch.IsSpilled() ? kX86WordSize : 0;
  __ movl(static_cast<Register>(ensure_scratch.GetRegister()), Address(ESP, src + stack_offset));
  __ movl(Address(ESP, dst + stack_offset), static_cast<Register>(ensure_scratch.GetRegister()));
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
  } else if (source.IsStackSlot()) {
    if (destination.IsRegister()) {
      __ movl(destination.AsRegister<Register>(), Address(ESP, source.GetStackIndex()));
    } else {
      DCHECK(destination.IsStackSlot());
      MoveMemoryToMemory(destination.GetStackIndex(),
                         source.GetStackIndex());
    }
  } else if (source.IsConstant()) {
    HIntConstant* instruction = source.GetConstant()->AsIntConstant();
    Immediate imm(instruction->AsIntConstant()->GetValue());
    if (destination.IsRegister()) {
      __ movl(destination.AsRegister<Register>(), imm);
    } else {
      __ movl(Address(ESP, destination.GetStackIndex()), imm);
    }
  } else {
    LOG(FATAL) << "Unimplemented";
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
  } else {
    LOG(FATAL) << "Unimplemented";
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
    __ movl(out, Address(out, mirror::ArtMethod::DeclaringClassOffset().Int32Value()));
  } else {
    DCHECK(cls->CanCallRuntime());
    codegen_->LoadCurrentMethod(out);
    __ movl(out, Address(out, mirror::ArtMethod::DexCacheResolvedTypesOffset().Int32Value()));
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

void LocationsBuilderX86::VisitStaticFieldGet(HStaticFieldGet* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

void InstructionCodeGeneratorX86::VisitStaticFieldGet(HStaticFieldGet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Register cls = locations->InAt(0).AsRegister<Register>();
  uint32_t offset = instruction->GetFieldOffset().Uint32Value();

  switch (instruction->GetType()) {
    case Primitive::kPrimBoolean: {
      Register out = locations->Out().AsRegister<Register>();
      __ movzxb(out, Address(cls, offset));
      break;
    }

    case Primitive::kPrimByte: {
      Register out = locations->Out().AsRegister<Register>();
      __ movsxb(out, Address(cls, offset));
      break;
    }

    case Primitive::kPrimShort: {
      Register out = locations->Out().AsRegister<Register>();
      __ movsxw(out, Address(cls, offset));
      break;
    }

    case Primitive::kPrimChar: {
      Register out = locations->Out().AsRegister<Register>();
      __ movzxw(out, Address(cls, offset));
      break;
    }

    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      Register out = locations->Out().AsRegister<Register>();
      __ movl(out, Address(cls, offset));
      break;
    }

    case Primitive::kPrimLong: {
      // TODO: support volatile.
      __ movl(locations->Out().AsRegisterPairLow<Register>(), Address(cls, offset));
      __ movl(locations->Out().AsRegisterPairHigh<Register>(), Address(cls, kX86WordSize + offset));
      break;
    }

    case Primitive::kPrimFloat: {
      XmmRegister out = locations->Out().AsFpuRegister<XmmRegister>();
      __ movss(out, Address(cls, offset));
      break;
    }

    case Primitive::kPrimDouble: {
      XmmRegister out = locations->Out().AsFpuRegister<XmmRegister>();
      __ movsd(out, Address(cls, offset));
      break;
    }

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << instruction->GetType();
      UNREACHABLE();
  }
}

void LocationsBuilderX86::VisitStaticFieldSet(HStaticFieldSet* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  Primitive::Type field_type = instruction->GetFieldType();
  bool needs_write_barrier =
      CodeGenerator::StoreNeedsWriteBarrier(field_type, instruction->InputAt(1));
  bool is_byte_type = (field_type == Primitive::kPrimBoolean)
      || (field_type == Primitive::kPrimByte);
  // The register allocator does not support multiple
  // inputs that die at entry with one in a specific register.
  if (is_byte_type) {
    // Ensure the value is in a byte register.
    locations->SetInAt(1, Location::RegisterLocation(EAX));
  } else {
    locations->SetInAt(1, Location::RequiresRegister());
  }
  // Temporary registers for the write barrier.
  if (needs_write_barrier) {
    locations->AddTemp(Location::RequiresRegister());
    // Ensure the card is in a byte register.
    locations->AddTemp(Location::RegisterLocation(ECX));
  }
}

void InstructionCodeGeneratorX86::VisitStaticFieldSet(HStaticFieldSet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Register cls = locations->InAt(0).AsRegister<Register>();
  uint32_t offset = instruction->GetFieldOffset().Uint32Value();
  Primitive::Type field_type = instruction->GetFieldType();

  switch (field_type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte: {
      ByteRegister value = locations->InAt(1).AsRegister<ByteRegister>();
      __ movb(Address(cls, offset), value);
      break;
    }

    case Primitive::kPrimShort:
    case Primitive::kPrimChar: {
      Register value = locations->InAt(1).AsRegister<Register>();
      __ movw(Address(cls, offset), value);
      break;
    }

    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      Register value = locations->InAt(1).AsRegister<Register>();
      __ movl(Address(cls, offset), value);

      if (CodeGenerator::StoreNeedsWriteBarrier(field_type, instruction->InputAt(1))) {
        Register temp = locations->GetTemp(0).AsRegister<Register>();
        Register card = locations->GetTemp(1).AsRegister<Register>();
        codegen_->MarkGCCard(temp, card, cls, value);
      }
      break;
    }

    case Primitive::kPrimLong: {
      Location value = locations->InAt(1);
      __ movl(Address(cls, offset), value.AsRegisterPairLow<Register>());
      __ movl(Address(cls, kX86WordSize + offset), value.AsRegisterPairHigh<Register>());
      break;
    }

    case Primitive::kPrimFloat: {
      XmmRegister value = locations->InAt(1).AsFpuRegister<XmmRegister>();
      __ movss(Address(cls, offset), value);
      break;
    }

    case Primitive::kPrimDouble: {
      XmmRegister value = locations->InAt(1).AsFpuRegister<XmmRegister>();
      __ movsd(Address(cls, offset), value);
      break;
    }

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << field_type;
      UNREACHABLE();
  }
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
  __ movl(out, Address(out, mirror::ArtMethod::DeclaringClassOffset().Int32Value()));
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
  // TODO: avoid this check if we know obj is not null.
  __ testl(obj, obj);
  __ j(kEqual, &zero);
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
  __ Bind(&zero);
  __ movl(out, Immediate(0));
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

  // TODO: avoid this check if we know obj is not null.
  __ testl(obj, obj);
  __ j(kEqual, slow_path->GetExitLabel());
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
    } else {
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
    }
  }
}

}  // namespace x86
}  // namespace art
