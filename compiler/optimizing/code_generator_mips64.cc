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

#include "code_generator_mips64.h"

#include "entrypoints/quick/quick_entrypoints.h"
#include "entrypoints/quick/quick_entrypoints_enum.h"
#include "gc/accounting/card_table.h"
#include "intrinsics.h"
#include "art_method.h"
#include "mirror/array-inl.h"
#include "mirror/class-inl.h"
#include "offsets.h"
#include "thread.h"
#include "utils/mips64/assembler_mips64.h"
#include "utils/assembler.h"
#include "utils/stack_checks.h"

namespace art {
namespace mips64 {

static constexpr int kCurrentMethodStackOffset = 0;
static constexpr GpuRegister kMethodRegisterArgument = A0;

// We need extra temporary/scratch registers (in addition to AT) in some cases.
static constexpr GpuRegister TMP = T8;
static constexpr FpuRegister FTMP = F8;

// ART Thread Register.
static constexpr GpuRegister TR = S1;

Location Mips64ReturnLocation(Primitive::Type return_type) {
  switch (return_type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimInt:
    case Primitive::kPrimNot:
    case Primitive::kPrimLong:
      return Location::RegisterLocation(V0);

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      return Location::FpuRegisterLocation(F0);

    case Primitive::kPrimVoid:
      return Location();
  }
  UNREACHABLE();
}

Location InvokeDexCallingConventionVisitorMIPS64::GetReturnLocation(Primitive::Type type) const {
  return Mips64ReturnLocation(type);
}

Location InvokeDexCallingConventionVisitorMIPS64::GetNextLocation(Primitive::Type type) {
  Location next_location;
  if (type == Primitive::kPrimVoid) {
    LOG(FATAL) << "Unexpected parameter type " << type;
  }

  if (Primitive::IsFloatingPointType(type) &&
      (float_index_ < calling_convention.GetNumberOfFpuRegisters())) {
    next_location = Location::FpuRegisterLocation(
        calling_convention.GetFpuRegisterAt(float_index_++));
    gp_index_++;
  } else if (!Primitive::IsFloatingPointType(type) &&
             (gp_index_ < calling_convention.GetNumberOfRegisters())) {
    next_location = Location::RegisterLocation(calling_convention.GetRegisterAt(gp_index_++));
    float_index_++;
  } else {
    size_t stack_offset = calling_convention.GetStackOffsetOf(stack_index_);
    next_location = Primitive::Is64BitType(type) ? Location::DoubleStackSlot(stack_offset)
                                                 : Location::StackSlot(stack_offset);
  }

  // Space on the stack is reserved for all arguments.
  stack_index_ += Primitive::Is64BitType(type) ? 2 : 1;

  // TODO: review

  // TODO: shouldn't we use a whole machine word per argument on the stack?
  // Implicit 4-byte method pointer (and such) will cause misalignment.

  return next_location;
}

Location InvokeRuntimeCallingConvention::GetReturnLocation(Primitive::Type type) {
  return Mips64ReturnLocation(type);
}

#define __ down_cast<CodeGeneratorMIPS64*>(codegen)->GetAssembler()->
#define QUICK_ENTRY_POINT(x) QUICK_ENTRYPOINT_OFFSET(kMips64WordSize, x).Int32Value()

class BoundsCheckSlowPathMIPS64 : public SlowPathCodeMIPS64 {
 public:
  BoundsCheckSlowPathMIPS64(HBoundsCheck* instruction,
                            Location index_location,
                            Location length_location)
      : instruction_(instruction),
        index_location_(index_location),
        length_location_(length_location) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    CodeGeneratorMIPS64* mips64_codegen = down_cast<CodeGeneratorMIPS64*>(codegen);
    __ Bind(GetEntryLabel());
    // We're moving two locations to locations that could overlap, so we need a parallel
    // move resolver.
    InvokeRuntimeCallingConvention calling_convention;
    codegen->EmitParallelMoves(index_location_,
                               Location::RegisterLocation(calling_convention.GetRegisterAt(0)),
                               Primitive::kPrimInt,
                               length_location_,
                               Location::RegisterLocation(calling_convention.GetRegisterAt(1)),
                               Primitive::kPrimInt);
    mips64_codegen->InvokeRuntime(QUICK_ENTRY_POINT(pThrowArrayBounds),
                                  instruction_,
                                  instruction_->GetDexPc(),
                                  this);
    CheckEntrypointTypes<kQuickThrowArrayBounds, void, int32_t, int32_t>();
  }

 private:
  HBoundsCheck* const instruction_;
  const Location index_location_;
  const Location length_location_;

  DISALLOW_COPY_AND_ASSIGN(BoundsCheckSlowPathMIPS64);
};

class DivZeroCheckSlowPathMIPS64 : public SlowPathCodeMIPS64 {
 public:
  explicit DivZeroCheckSlowPathMIPS64(HDivZeroCheck* instruction) : instruction_(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    CodeGeneratorMIPS64* mips64_codegen = down_cast<CodeGeneratorMIPS64*>(codegen);
    __ Bind(GetEntryLabel());
    mips64_codegen->InvokeRuntime(QUICK_ENTRY_POINT(pThrowDivZero),
                                  instruction_,
                                  instruction_->GetDexPc(),
                                  this);
    CheckEntrypointTypes<kQuickThrowDivZero, void, void>();
  }

 private:
  HDivZeroCheck* const instruction_;
  DISALLOW_COPY_AND_ASSIGN(DivZeroCheckSlowPathMIPS64);
};

class LoadClassSlowPathMIPS64 : public SlowPathCodeMIPS64 {
 public:
  LoadClassSlowPathMIPS64(HLoadClass* cls,
                          HInstruction* at,
                          uint32_t dex_pc,
                          bool do_clinit)
      : cls_(cls), at_(at), dex_pc_(dex_pc), do_clinit_(do_clinit) {
    DCHECK(at->IsLoadClass() || at->IsClinitCheck());
  }

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    LocationSummary* locations = at_->GetLocations();
    CodeGeneratorMIPS64* mips64_codegen = down_cast<CodeGeneratorMIPS64*>(codegen);

    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, locations);

    InvokeRuntimeCallingConvention calling_convention;
    __ LoadConst32(calling_convention.GetRegisterAt(0), cls_->GetTypeIndex());
    int32_t entry_point_offset = do_clinit_ ? QUICK_ENTRY_POINT(pInitializeStaticStorage)
                                            : QUICK_ENTRY_POINT(pInitializeType);
    mips64_codegen->InvokeRuntime(entry_point_offset, at_, dex_pc_, this);
    if (do_clinit_) {
      CheckEntrypointTypes<kQuickInitializeStaticStorage, void*, uint32_t>();
    } else {
      CheckEntrypointTypes<kQuickInitializeType, void*, uint32_t>();
    }

    // Move the class to the desired location.
    Location out = locations->Out();
    if (out.IsValid()) {
      DCHECK(out.IsRegister() && !locations->GetLiveRegisters()->ContainsCoreRegister(out.reg()));
      Primitive::Type type = at_->GetType();
      mips64_codegen->MoveLocation(out, calling_convention.GetReturnLocation(type), type);
    }

    RestoreLiveRegisters(codegen, locations);
    __ B(GetExitLabel());
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

  DISALLOW_COPY_AND_ASSIGN(LoadClassSlowPathMIPS64);
};

class LoadStringSlowPathMIPS64 : public SlowPathCodeMIPS64 {
 public:
  explicit LoadStringSlowPathMIPS64(HLoadString* instruction) : instruction_(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    LocationSummary* locations = instruction_->GetLocations();
    DCHECK(!locations->GetLiveRegisters()->ContainsCoreRegister(locations->Out().reg()));
    CodeGeneratorMIPS64* mips64_codegen = down_cast<CodeGeneratorMIPS64*>(codegen);

    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, locations);

    InvokeRuntimeCallingConvention calling_convention;
    __ LoadConst32(calling_convention.GetRegisterAt(0), instruction_->GetStringIndex());
    mips64_codegen->InvokeRuntime(QUICK_ENTRY_POINT(pResolveString),
                                  instruction_,
                                  instruction_->GetDexPc(),
                                  this);
    CheckEntrypointTypes<kQuickResolveString, void*, uint32_t>();
    Primitive::Type type = instruction_->GetType();
    mips64_codegen->MoveLocation(locations->Out(),
                                 calling_convention.GetReturnLocation(type),
                                 type);

    RestoreLiveRegisters(codegen, locations);
    __ B(GetExitLabel());
  }

 private:
  HLoadString* const instruction_;

  DISALLOW_COPY_AND_ASSIGN(LoadStringSlowPathMIPS64);
};

class NullCheckSlowPathMIPS64 : public SlowPathCodeMIPS64 {
 public:
  explicit NullCheckSlowPathMIPS64(HNullCheck* instr) : instruction_(instr) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    CodeGeneratorMIPS64* mips64_codegen = down_cast<CodeGeneratorMIPS64*>(codegen);
    __ Bind(GetEntryLabel());
    mips64_codegen->InvokeRuntime(QUICK_ENTRY_POINT(pThrowNullPointer),
                                  instruction_,
                                  instruction_->GetDexPc(),
                                  this);
    CheckEntrypointTypes<kQuickThrowNullPointer, void, void>();
  }

 private:
  HNullCheck* const instruction_;

  DISALLOW_COPY_AND_ASSIGN(NullCheckSlowPathMIPS64);
};

class SuspendCheckSlowPathMIPS64 : public SlowPathCodeMIPS64 {
 public:
  explicit SuspendCheckSlowPathMIPS64(HSuspendCheck* instruction,
                                      HBasicBlock* successor)
      : instruction_(instruction), successor_(successor) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    CodeGeneratorMIPS64* mips64_codegen = down_cast<CodeGeneratorMIPS64*>(codegen);
    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, instruction_->GetLocations());
    mips64_codegen->InvokeRuntime(QUICK_ENTRY_POINT(pTestSuspend),
                                  instruction_,
                                  instruction_->GetDexPc(),
                                  this);
    CheckEntrypointTypes<kQuickTestSuspend, void, void>();
    RestoreLiveRegisters(codegen, instruction_->GetLocations());
    if (successor_ == nullptr) {
      __ B(GetReturnLabel());
    } else {
      __ B(mips64_codegen->GetLabelOf(successor_));
    }
  }

  Label* GetReturnLabel() {
    DCHECK(successor_ == nullptr);
    return &return_label_;
  }

 private:
  HSuspendCheck* const instruction_;
  // If not null, the block to branch to after the suspend check.
  HBasicBlock* const successor_;

  // If `successor_` is null, the label to branch to after the suspend check.
  Label return_label_;

  DISALLOW_COPY_AND_ASSIGN(SuspendCheckSlowPathMIPS64);
};

class TypeCheckSlowPathMIPS64 : public SlowPathCodeMIPS64 {
 public:
  TypeCheckSlowPathMIPS64(HInstruction* instruction,
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
    CodeGeneratorMIPS64* mips64_codegen = down_cast<CodeGeneratorMIPS64*>(codegen);

    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, locations);

    // We're moving two locations to locations that could overlap, so we need a parallel
    // move resolver.
    InvokeRuntimeCallingConvention calling_convention;
    codegen->EmitParallelMoves(class_to_check_,
                               Location::RegisterLocation(calling_convention.GetRegisterAt(0)),
                               Primitive::kPrimNot,
                               object_class_,
                               Location::RegisterLocation(calling_convention.GetRegisterAt(1)),
                               Primitive::kPrimNot);

    if (instruction_->IsInstanceOf()) {
      mips64_codegen->InvokeRuntime(QUICK_ENTRY_POINT(pInstanceofNonTrivial),
                                    instruction_,
                                    dex_pc_,
                                    this);
      Primitive::Type ret_type = instruction_->GetType();
      Location ret_loc = calling_convention.GetReturnLocation(ret_type);
      mips64_codegen->MoveLocation(locations->Out(), ret_loc, ret_type);
      CheckEntrypointTypes<kQuickInstanceofNonTrivial,
                           uint32_t,
                           const mirror::Class*,
                           const mirror::Class*>();
    } else {
      DCHECK(instruction_->IsCheckCast());
      mips64_codegen->InvokeRuntime(QUICK_ENTRY_POINT(pCheckCast), instruction_, dex_pc_, this);
      CheckEntrypointTypes<kQuickCheckCast, void, const mirror::Class*, const mirror::Class*>();
    }

    RestoreLiveRegisters(codegen, locations);
    __ B(GetExitLabel());
  }

 private:
  HInstruction* const instruction_;
  const Location class_to_check_;
  const Location object_class_;
  uint32_t dex_pc_;

  DISALLOW_COPY_AND_ASSIGN(TypeCheckSlowPathMIPS64);
};

class DeoptimizationSlowPathMIPS64 : public SlowPathCodeMIPS64 {
 public:
  explicit DeoptimizationSlowPathMIPS64(HInstruction* instruction)
    : instruction_(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, instruction_->GetLocations());
    DCHECK(instruction_->IsDeoptimize());
    HDeoptimize* deoptimize = instruction_->AsDeoptimize();
    uint32_t dex_pc = deoptimize->GetDexPc();
    CodeGeneratorMIPS64* mips64_codegen = down_cast<CodeGeneratorMIPS64*>(codegen);
    mips64_codegen->InvokeRuntime(QUICK_ENTRY_POINT(pDeoptimize), instruction_, dex_pc, this);
  }

 private:
  HInstruction* const instruction_;
  DISALLOW_COPY_AND_ASSIGN(DeoptimizationSlowPathMIPS64);
};

CodeGeneratorMIPS64::CodeGeneratorMIPS64(HGraph* graph,
                                         const Mips64InstructionSetFeatures& isa_features,
                                         const CompilerOptions& compiler_options)
    : CodeGenerator(graph,
                    kNumberOfGpuRegisters,
                    kNumberOfFpuRegisters,
                    0,  // kNumberOfRegisterPairs
                    ComputeRegisterMask(reinterpret_cast<const int*>(kCoreCalleeSaves),
                                        arraysize(kCoreCalleeSaves)),
                    ComputeRegisterMask(reinterpret_cast<const int*>(kFpuCalleeSaves),
                                        arraysize(kFpuCalleeSaves)),
                    compiler_options),
      block_labels_(graph->GetArena(), 0),
      location_builder_(graph, this),
      instruction_visitor_(graph, this),
      move_resolver_(graph->GetArena(), this),
      isa_features_(isa_features) {
  // Save RA (containing the return address) to mimic Quick.
  AddAllocatedRegister(Location::RegisterLocation(RA));
}

#undef __
#define __ down_cast<Mips64Assembler*>(GetAssembler())->
#define QUICK_ENTRY_POINT(x) QUICK_ENTRYPOINT_OFFSET(kMips64WordSize, x).Int32Value()

void CodeGeneratorMIPS64::Finalize(CodeAllocator* allocator) {
  CodeGenerator::Finalize(allocator);
}

Mips64Assembler* ParallelMoveResolverMIPS64::GetAssembler() const {
  return codegen_->GetAssembler();
}

void ParallelMoveResolverMIPS64::EmitMove(size_t index) {
  MoveOperands* move = moves_.Get(index);
  codegen_->MoveLocation(move->GetDestination(), move->GetSource(), move->GetType());
}

void ParallelMoveResolverMIPS64::EmitSwap(size_t index) {
  MoveOperands* move = moves_.Get(index);
  codegen_->SwapLocations(move->GetDestination(), move->GetSource(), move->GetType());
}

void ParallelMoveResolverMIPS64::RestoreScratch(int reg) {
  // Pop reg
  __ Ld(GpuRegister(reg), SP, 0);
  __ DecreaseFrameSize(kMips64WordSize);
}

void ParallelMoveResolverMIPS64::SpillScratch(int reg) {
  // Push reg
  __ IncreaseFrameSize(kMips64WordSize);
  __ Sd(GpuRegister(reg), SP, 0);
}

void ParallelMoveResolverMIPS64::Exchange(int index1, int index2, bool double_slot) {
  LoadOperandType load_type = double_slot ? kLoadDoubleword : kLoadWord;
  StoreOperandType store_type = double_slot ? kStoreDoubleword : kStoreWord;
  // Allocate a scratch register other than TMP, if available.
  // Else, spill V0 (arbitrary choice) and use it as a scratch register (it will be
  // automatically unspilled when the scratch scope object is destroyed).
  ScratchRegisterScope ensure_scratch(this, TMP, V0, codegen_->GetNumberOfCoreRegisters());
  // If V0 spills onto the stack, SP-relative offsets need to be adjusted.
  int stack_offset = ensure_scratch.IsSpilled() ? kMips64WordSize : 0;
  __ LoadFromOffset(load_type,
                    GpuRegister(ensure_scratch.GetRegister()),
                    SP,
                    index1 + stack_offset);
  __ LoadFromOffset(load_type,
                    TMP,
                    SP,
                    index2 + stack_offset);
  __ StoreToOffset(store_type,
                   GpuRegister(ensure_scratch.GetRegister()),
                   SP,
                   index2 + stack_offset);
  __ StoreToOffset(store_type, TMP, SP, index1 + stack_offset);
}

static dwarf::Reg DWARFReg(GpuRegister reg) {
  return dwarf::Reg::Mips64Core(static_cast<int>(reg));
}

// TODO: mapping of floating-point registers to DWARF

void CodeGeneratorMIPS64::GenerateFrameEntry() {
  __ Bind(&frame_entry_label_);

  bool do_overflow_check = FrameNeedsStackCheck(GetFrameSize(), kMips64) || !IsLeafMethod();

  if (do_overflow_check) {
    __ LoadFromOffset(kLoadWord,
                      ZERO,
                      SP,
                      -static_cast<int32_t>(GetStackOverflowReservedBytes(kMips64)));
    RecordPcInfo(nullptr, 0);
  }

  // TODO: anything related to T9/GP/GOT/PIC/.so's?

  if (HasEmptyFrame()) {
    return;
  }

  // Make sure the frame size isn't unreasonably large. Per the various APIs
  // it looks like it should always be less than 2GB in size, which allows
  // us using 32-bit signed offsets from the stack pointer.
  if (GetFrameSize() > 0x7FFFFFFF)
    LOG(FATAL) << "Stack frame larger than 2GB";

  // Spill callee-saved registers.
  // Note that their cumulative size is small and they can be indexed using
  // 16-bit offsets.

  // TODO: increment/decrement SP in one step instead of two or remove this comment.

  uint32_t ofs = FrameEntrySpillSize();
  __ IncreaseFrameSize(ofs);

  for (int i = arraysize(kCoreCalleeSaves) - 1; i >= 0; --i) {
    GpuRegister reg = kCoreCalleeSaves[i];
    if (allocated_registers_.ContainsCoreRegister(reg)) {
      ofs -= kMips64WordSize;
      __ Sd(reg, SP, ofs);
      __ cfi().RelOffset(DWARFReg(reg), ofs);
    }
  }

  for (int i = arraysize(kFpuCalleeSaves) - 1; i >= 0; --i) {
    FpuRegister reg = kFpuCalleeSaves[i];
    if (allocated_registers_.ContainsFloatingPointRegister(reg)) {
      ofs -= kMips64WordSize;
      __ Sdc1(reg, SP, ofs);
      // TODO: __ cfi().RelOffset(DWARFReg(reg), ofs);
    }
  }

  // Allocate the rest of the frame and store the current method pointer
  // at its end.

  __ IncreaseFrameSize(GetFrameSize() - FrameEntrySpillSize());

  static_assert(IsInt<16>(kCurrentMethodStackOffset),
                "kCurrentMethodStackOffset must fit into int16_t");
  __ Sd(kMethodRegisterArgument, SP, kCurrentMethodStackOffset);
}

void CodeGeneratorMIPS64::GenerateFrameExit() {
  __ cfi().RememberState();

  // TODO: anything related to T9/GP/GOT/PIC/.so's?

  if (!HasEmptyFrame()) {
    // Deallocate the rest of the frame.

    __ DecreaseFrameSize(GetFrameSize() - FrameEntrySpillSize());

    // Restore callee-saved registers.
    // Note that their cumulative size is small and they can be indexed using
    // 16-bit offsets.

    // TODO: increment/decrement SP in one step instead of two or remove this comment.

    uint32_t ofs = 0;

    for (size_t i = 0; i < arraysize(kFpuCalleeSaves); ++i) {
      FpuRegister reg = kFpuCalleeSaves[i];
      if (allocated_registers_.ContainsFloatingPointRegister(reg)) {
        __ Ldc1(reg, SP, ofs);
        ofs += kMips64WordSize;
        // TODO: __ cfi().Restore(DWARFReg(reg));
      }
    }

    for (size_t i = 0; i < arraysize(kCoreCalleeSaves); ++i) {
      GpuRegister reg = kCoreCalleeSaves[i];
      if (allocated_registers_.ContainsCoreRegister(reg)) {
        __ Ld(reg, SP, ofs);
        ofs += kMips64WordSize;
        __ cfi().Restore(DWARFReg(reg));
      }
    }

    DCHECK_EQ(ofs, FrameEntrySpillSize());
    __ DecreaseFrameSize(ofs);
  }

  __ Jr(RA);

  __ cfi().RestoreState();
  __ cfi().DefCFAOffset(GetFrameSize());
}

void CodeGeneratorMIPS64::Bind(HBasicBlock* block) {
  __ Bind(GetLabelOf(block));
}

void CodeGeneratorMIPS64::MoveLocation(Location destination,
                                       Location source,
                                       Primitive::Type type) {
  if (source.Equals(destination)) {
    return;
  }

  // A valid move can always be inferred from the destination and source
  // locations. When moving from and to a register, the argument type can be
  // used to generate 32bit instead of 64bit moves.
  bool unspecified_type = (type == Primitive::kPrimVoid);
  DCHECK_EQ(unspecified_type, false);

  if (destination.IsRegister() || destination.IsFpuRegister()) {
    if (unspecified_type) {
      HConstant* src_cst = source.IsConstant() ? source.GetConstant() : nullptr;
      if (source.IsStackSlot() ||
          (src_cst != nullptr && (src_cst->IsIntConstant()
                                  || src_cst->IsFloatConstant()
                                  || src_cst->IsNullConstant()))) {
        // For stack slots and 32bit constants, a 64bit type is appropriate.
        type = destination.IsRegister() ? Primitive::kPrimInt : Primitive::kPrimFloat;
      } else {
        // If the source is a double stack slot or a 64bit constant, a 64bit
        // type is appropriate. Else the source is a register, and since the
        // type has not been specified, we chose a 64bit type to force a 64bit
        // move.
        type = destination.IsRegister() ? Primitive::kPrimLong : Primitive::kPrimDouble;
      }
    }
    DCHECK((destination.IsFpuRegister() && Primitive::IsFloatingPointType(type)) ||
           (destination.IsRegister() && !Primitive::IsFloatingPointType(type)));
    if (source.IsStackSlot() || source.IsDoubleStackSlot()) {
      // Move to GPR/FPR from stack
      LoadOperandType load_type = source.IsStackSlot() ? kLoadWord : kLoadDoubleword;
      if (Primitive::IsFloatingPointType(type)) {
        __ LoadFpuFromOffset(load_type,
                             destination.AsFpuRegister<FpuRegister>(),
                             SP,
                             source.GetStackIndex());
      } else {
        // TODO: use load_type = kLoadUnsignedWord when type == Primitive::kPrimNot.
        __ LoadFromOffset(load_type,
                          destination.AsRegister<GpuRegister>(),
                          SP,
                          source.GetStackIndex());
      }
    } else if (source.IsConstant()) {
      // Move to GPR/FPR from constant
      GpuRegister gpr = AT;
      if (!Primitive::IsFloatingPointType(type)) {
        gpr = destination.AsRegister<GpuRegister>();
      }
      if (type == Primitive::kPrimInt || type == Primitive::kPrimFloat) {
        __ LoadConst32(gpr, GetInt32ValueOf(source.GetConstant()->AsConstant()));
      } else {
        __ LoadConst64(gpr, GetInt64ValueOf(source.GetConstant()->AsConstant()));
      }
      if (type == Primitive::kPrimFloat) {
        __ Mtc1(gpr, destination.AsFpuRegister<FpuRegister>());
      } else if (type == Primitive::kPrimDouble) {
        __ Dmtc1(gpr, destination.AsFpuRegister<FpuRegister>());
      }
    } else {
      if (destination.IsRegister()) {
        // Move to GPR from GPR
        __ Move(destination.AsRegister<GpuRegister>(), source.AsRegister<GpuRegister>());
      } else {
        // Move to FPR from FPR
        if (type == Primitive::kPrimFloat) {
          __ MovS(destination.AsFpuRegister<FpuRegister>(), source.AsFpuRegister<FpuRegister>());
        } else {
          DCHECK_EQ(type, Primitive::kPrimDouble);
          __ MovD(destination.AsFpuRegister<FpuRegister>(), source.AsFpuRegister<FpuRegister>());
        }
      }
    }
  } else {  // The destination is not a register. It must be a stack slot.
    DCHECK(destination.IsStackSlot() || destination.IsDoubleStackSlot());
    if (source.IsRegister() || source.IsFpuRegister()) {
      if (unspecified_type) {
        if (source.IsRegister()) {
          type = destination.IsStackSlot() ? Primitive::kPrimInt : Primitive::kPrimLong;
        } else {
          type = destination.IsStackSlot() ? Primitive::kPrimFloat : Primitive::kPrimDouble;
        }
      }
      DCHECK((destination.IsDoubleStackSlot() == Primitive::Is64BitType(type)) &&
             (source.IsFpuRegister() == Primitive::IsFloatingPointType(type)));
      // Move to stack from GPR/FPR
      StoreOperandType store_type = destination.IsStackSlot() ? kStoreWord : kStoreDoubleword;
      if (source.IsRegister()) {
        __ StoreToOffset(store_type,
                         source.AsRegister<GpuRegister>(),
                         SP,
                         destination.GetStackIndex());
      } else {
        __ StoreFpuToOffset(store_type,
                            source.AsFpuRegister<FpuRegister>(),
                            SP,
                            destination.GetStackIndex());
      }
    } else if (source.IsConstant()) {
      // Move to stack from constant
      HConstant* src_cst = source.GetConstant();
      StoreOperandType store_type = destination.IsStackSlot() ? kStoreWord : kStoreDoubleword;
      if (destination.IsStackSlot()) {
        __ LoadConst32(TMP, GetInt32ValueOf(src_cst->AsConstant()));
      } else {
        __ LoadConst64(TMP, GetInt64ValueOf(src_cst->AsConstant()));
      }
      __ StoreToOffset(store_type, TMP, SP, destination.GetStackIndex());
    } else {
      DCHECK(source.IsStackSlot() || source.IsDoubleStackSlot());
      DCHECK_EQ(source.IsDoubleStackSlot(), destination.IsDoubleStackSlot());
      // Move to stack from stack
      if (destination.IsStackSlot()) {
        __ LoadFromOffset(kLoadWord, TMP, SP, source.GetStackIndex());
        __ StoreToOffset(kStoreWord, TMP, SP, destination.GetStackIndex());
      } else {
        __ LoadFromOffset(kLoadDoubleword, TMP, SP, source.GetStackIndex());
        __ StoreToOffset(kStoreDoubleword, TMP, SP, destination.GetStackIndex());
      }
    }
  }
}

void CodeGeneratorMIPS64::SwapLocations(Location loc1,
                                        Location loc2,
                                        Primitive::Type type ATTRIBUTE_UNUSED) {
  DCHECK(!loc1.IsConstant());
  DCHECK(!loc2.IsConstant());

  if (loc1.Equals(loc2)) {
    return;
  }

  bool is_slot1 = loc1.IsStackSlot() || loc1.IsDoubleStackSlot();
  bool is_slot2 = loc2.IsStackSlot() || loc2.IsDoubleStackSlot();
  bool is_fp_reg1 = loc1.IsFpuRegister();
  bool is_fp_reg2 = loc2.IsFpuRegister();

  if (loc2.IsRegister() && loc1.IsRegister()) {
    // Swap 2 GPRs
    GpuRegister r1 = loc1.AsRegister<GpuRegister>();
    GpuRegister r2 = loc2.AsRegister<GpuRegister>();
    __ Move(TMP, r2);
    __ Move(r2, r1);
    __ Move(r1, TMP);
  } else if (is_fp_reg2 && is_fp_reg1) {
    // Swap 2 FPRs
    FpuRegister r1 = loc1.AsFpuRegister<FpuRegister>();
    FpuRegister r2 = loc2.AsFpuRegister<FpuRegister>();
    // TODO: Can MOV.S/MOV.D be used here to save one instruction?
    // Need to distinguish float from double, right?
    __ Dmfc1(TMP, r2);
    __ Dmfc1(AT, r1);
    __ Dmtc1(TMP, r1);
    __ Dmtc1(AT, r2);
  } else if (is_slot1 != is_slot2) {
    // Swap GPR/FPR and stack slot
    Location reg_loc = is_slot1 ? loc2 : loc1;
    Location mem_loc = is_slot1 ? loc1 : loc2;
    LoadOperandType load_type = mem_loc.IsStackSlot() ? kLoadWord : kLoadDoubleword;
    StoreOperandType store_type = mem_loc.IsStackSlot() ? kStoreWord : kStoreDoubleword;
    // TODO: use load_type = kLoadUnsignedWord when type == Primitive::kPrimNot.
    __ LoadFromOffset(load_type, TMP, SP, mem_loc.GetStackIndex());
    if (reg_loc.IsFpuRegister()) {
      __ StoreFpuToOffset(store_type,
                          reg_loc.AsFpuRegister<FpuRegister>(),
                          SP,
                          mem_loc.GetStackIndex());
      // TODO: review this MTC1/DMTC1 move
      if (mem_loc.IsStackSlot()) {
        __ Mtc1(TMP, reg_loc.AsFpuRegister<FpuRegister>());
      } else {
        DCHECK(mem_loc.IsDoubleStackSlot());
        __ Dmtc1(TMP, reg_loc.AsFpuRegister<FpuRegister>());
      }
    } else {
      __ StoreToOffset(store_type, reg_loc.AsRegister<GpuRegister>(), SP, mem_loc.GetStackIndex());
      __ Move(reg_loc.AsRegister<GpuRegister>(), TMP);
    }
  } else if (is_slot1 && is_slot2) {
    move_resolver_.Exchange(loc1.GetStackIndex(),
                            loc2.GetStackIndex(),
                            loc1.IsDoubleStackSlot());
  } else {
    LOG(FATAL) << "Unimplemented swap between locations " << loc1 << " and " << loc2;
  }
}

void CodeGeneratorMIPS64::Move(HInstruction* instruction,
                               Location location,
                               HInstruction* move_for) {
  LocationSummary* locations = instruction->GetLocations();
  if (locations != nullptr && locations->Out().Equals(location)) {
    return;
  }

  Primitive::Type type = instruction->GetType();
  DCHECK_NE(type, Primitive::kPrimVoid);

  if (instruction->IsIntConstant()
      || instruction->IsLongConstant()
      || instruction->IsNullConstant()) {
    if (location.IsRegister()) {
      // Move to GPR from constant
      GpuRegister dst = location.AsRegister<GpuRegister>();
      if (instruction->IsNullConstant() || instruction->IsIntConstant()) {
        __ LoadConst32(dst, GetInt32ValueOf(instruction->AsConstant()));
      } else {
        __ LoadConst64(dst, instruction->AsLongConstant()->GetValue());
      }
    } else {
      DCHECK(location.IsStackSlot() || location.IsDoubleStackSlot());
      // Move to stack from constant
      if (location.IsStackSlot()) {
        __ LoadConst32(TMP, GetInt32ValueOf(instruction->AsConstant()));
        __ StoreToOffset(kStoreWord, TMP, SP, location.GetStackIndex());
      } else {
        __ LoadConst64(TMP, instruction->AsLongConstant()->GetValue());
        __ StoreToOffset(kStoreDoubleword, TMP, SP, location.GetStackIndex());
      }
    }
  } else if (instruction->IsTemporary()) {
    Location temp_location = GetTemporaryLocation(instruction->AsTemporary());
    MoveLocation(location, temp_location, type);
  } else if (instruction->IsLoadLocal()) {
    uint32_t stack_slot = GetStackSlot(instruction->AsLoadLocal()->GetLocal());
    if (Primitive::Is64BitType(type)) {
      MoveLocation(location, Location::DoubleStackSlot(stack_slot), type);
    } else {
      MoveLocation(location, Location::StackSlot(stack_slot), type);
    }
  } else {
    DCHECK((instruction->GetNext() == move_for) || instruction->GetNext()->IsTemporary());
    MoveLocation(location, locations->Out(), type);
  }
}

Location CodeGeneratorMIPS64::GetStackLocation(HLoadLocal* load) const {
  Primitive::Type type = load->GetType();

  switch (type) {
    case Primitive::kPrimNot:
    case Primitive::kPrimInt:
    case Primitive::kPrimFloat:
      return Location::StackSlot(GetStackSlot(load->GetLocal()));

    case Primitive::kPrimLong:
    case Primitive::kPrimDouble:
      return Location::DoubleStackSlot(GetStackSlot(load->GetLocal()));

    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unexpected type " << type;
  }

  LOG(FATAL) << "Unreachable";
  return Location::NoLocation();
}

void CodeGeneratorMIPS64::MarkGCCard(GpuRegister object, GpuRegister value) {
  Label done;
  GpuRegister card = AT;
  GpuRegister temp = TMP;
  __ Beqzc(value, &done);
  __ LoadFromOffset(kLoadDoubleword,
                    card,
                    TR,
                    Thread::CardTableOffset<kMips64WordSize>().Int32Value());
  __ Dsrl(temp, object, gc::accounting::CardTable::kCardShift);
  __ Daddu(temp, card, temp);
  __ Sb(card, temp, 0);
  __ Bind(&done);
}

void CodeGeneratorMIPS64::SetupBlockedRegisters(bool is_baseline ATTRIBUTE_UNUSED) const {
  // ZERO, K0, K1, GP, SP, RA are always reserved and can't be allocated.
  blocked_core_registers_[ZERO] = true;
  blocked_core_registers_[K0] = true;
  blocked_core_registers_[K1] = true;
  blocked_core_registers_[GP] = true;
  blocked_core_registers_[SP] = true;
  blocked_core_registers_[RA] = true;

  // AT and TMP(T8) are used as temporary/scratch registers
  // (similar to how AT is used by MIPS assemblers).
  blocked_core_registers_[AT] = true;
  blocked_core_registers_[TMP] = true;
  blocked_fpu_registers_[FTMP] = true;

  // Reserve suspend and thread registers.
  blocked_core_registers_[S0] = true;
  blocked_core_registers_[TR] = true;

  // Reserve T9 for function calls
  blocked_core_registers_[T9] = true;

  // TODO: review; anything else?

  // TODO: make these two for's conditional on is_baseline once
  // all the issues with register saving/restoring are sorted out.
  for (size_t i = 0; i < arraysize(kCoreCalleeSaves); ++i) {
    blocked_core_registers_[kCoreCalleeSaves[i]] = true;
  }

  for (size_t i = 0; i < arraysize(kFpuCalleeSaves); ++i) {
    blocked_fpu_registers_[kFpuCalleeSaves[i]] = true;
  }
}

Location CodeGeneratorMIPS64::AllocateFreeRegister(Primitive::Type type) const {
  if (type == Primitive::kPrimVoid) {
    LOG(FATAL) << "Unreachable type " << type;
  }

  if (Primitive::IsFloatingPointType(type)) {
    size_t reg = FindFreeEntry(blocked_fpu_registers_, kNumberOfFpuRegisters);
    return Location::FpuRegisterLocation(reg);
  } else {
    size_t reg = FindFreeEntry(blocked_core_registers_, kNumberOfGpuRegisters);
    return Location::RegisterLocation(reg);
  }
}

size_t CodeGeneratorMIPS64::SaveCoreRegister(size_t stack_index, uint32_t reg_id) {
  __ StoreToOffset(kStoreDoubleword, GpuRegister(reg_id), SP, stack_index);
  return kMips64WordSize;
}

size_t CodeGeneratorMIPS64::RestoreCoreRegister(size_t stack_index, uint32_t reg_id) {
  __ LoadFromOffset(kLoadDoubleword, GpuRegister(reg_id), SP, stack_index);
  return kMips64WordSize;
}

size_t CodeGeneratorMIPS64::SaveFloatingPointRegister(size_t stack_index, uint32_t reg_id) {
  __ StoreFpuToOffset(kStoreDoubleword, FpuRegister(reg_id), SP, stack_index);
  return kMips64WordSize;
}

size_t CodeGeneratorMIPS64::RestoreFloatingPointRegister(size_t stack_index, uint32_t reg_id) {
  __ LoadFpuFromOffset(kLoadDoubleword, FpuRegister(reg_id), SP, stack_index);
  return kMips64WordSize;
}

void CodeGeneratorMIPS64::DumpCoreRegister(std::ostream& stream, int reg) const {
  stream << Mips64ManagedRegister::FromGpuRegister(GpuRegister(reg));
}

void CodeGeneratorMIPS64::DumpFloatingPointRegister(std::ostream& stream, int reg) const {
  stream << Mips64ManagedRegister::FromFpuRegister(FpuRegister(reg));
}

void CodeGeneratorMIPS64::LoadCurrentMethod(GpuRegister current_method) {
  DCHECK(RequiresCurrentMethod());
  __ Ld(current_method, SP, kCurrentMethodStackOffset);
}

void CodeGeneratorMIPS64::InvokeRuntime(int32_t entry_point_offset,
                                        HInstruction* instruction,
                                        uint32_t dex_pc,
                                        SlowPathCode* slow_path) {
  // TODO: anything related to T9/GP/GOT/PIC/.so's?
  __ LoadFromOffset(kLoadDoubleword, T9, TR, entry_point_offset);
  __ Jalr(T9);
  RecordPcInfo(instruction, dex_pc, slow_path);
  DCHECK(instruction->IsSuspendCheck()
      || instruction->IsBoundsCheck()
      || instruction->IsNullCheck()
      || instruction->IsDivZeroCheck()
      || !IsLeafMethod());
}

void InstructionCodeGeneratorMIPS64::GenerateClassInitializationCheck(SlowPathCodeMIPS64* slow_path,
                                                                      GpuRegister class_reg) {
  __ LoadFromOffset(kLoadWord, TMP, class_reg, mirror::Class::StatusOffset().Int32Value());
  __ LoadConst32(AT, mirror::Class::kStatusInitialized);
  __ Bltc(TMP, AT, slow_path->GetEntryLabel());
  // TODO: barrier needed?
  __ Bind(slow_path->GetExitLabel());
}

void InstructionCodeGeneratorMIPS64::GenerateMemoryBarrier(MemBarrierKind kind ATTRIBUTE_UNUSED) {
  __ Sync(0);  // only stype 0 is supported
}

void InstructionCodeGeneratorMIPS64::GenerateSuspendCheck(HSuspendCheck* instruction,
                                                          HBasicBlock* successor) {
  SuspendCheckSlowPathMIPS64* slow_path =
    new (GetGraph()->GetArena()) SuspendCheckSlowPathMIPS64(instruction, successor);
  codegen_->AddSlowPath(slow_path);

  __ LoadFromOffset(kLoadUnsignedHalfword,
                    TMP,
                    TR,
                    Thread::ThreadFlagsOffset<kMips64WordSize>().Int32Value());
  if (successor == nullptr) {
    __ Bnezc(TMP, slow_path->GetEntryLabel());
    __ Bind(slow_path->GetReturnLabel());
  } else {
    __ Beqzc(TMP, codegen_->GetLabelOf(successor));
    __ B(slow_path->GetEntryLabel());
    // slow_path will return to GetLabelOf(successor).
  }
}

InstructionCodeGeneratorMIPS64::InstructionCodeGeneratorMIPS64(HGraph* graph,
                                                               CodeGeneratorMIPS64* codegen)
      : HGraphVisitor(graph),
        assembler_(codegen->GetAssembler()),
        codegen_(codegen) {}

void LocationsBuilderMIPS64::HandleBinaryOp(HBinaryOperation* instruction) {
  DCHECK_EQ(instruction->InputCount(), 2U);
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  Primitive::Type type = instruction->GetResultType();
  switch (type) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong: {
      locations->SetInAt(0, Location::RequiresRegister());
      HInstruction* right = instruction->InputAt(1);
      bool can_use_imm = false;
      if (right->IsConstant()) {
        int64_t imm = CodeGenerator::GetInt64ValueOf(right->AsConstant());
        if (instruction->IsAnd() || instruction->IsOr() || instruction->IsXor()) {
          can_use_imm = IsUint<16>(imm);
        } else if (instruction->IsAdd()) {
          can_use_imm = IsInt<16>(imm);
        } else {
          DCHECK(instruction->IsSub());
          can_use_imm = IsInt<16>(-imm);
        }
      }
      if (can_use_imm)
        locations->SetInAt(1, Location::ConstantLocation(right->AsConstant()));
      else
        locations->SetInAt(1, Location::RequiresRegister());
      locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      }
      break;

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetInAt(1, Location::RequiresFpuRegister());
      locations->SetOut(Location::RequiresFpuRegister(), Location::kNoOutputOverlap);
      break;

    default:
      LOG(FATAL) << "Unexpected " << instruction->DebugName() << " type " << type;
  }
}

void InstructionCodeGeneratorMIPS64::HandleBinaryOp(HBinaryOperation* instruction) {
  Primitive::Type type = instruction->GetType();
  LocationSummary* locations = instruction->GetLocations();

  switch (type) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong: {
      GpuRegister dst = locations->Out().AsRegister<GpuRegister>();
      GpuRegister lhs = locations->InAt(0).AsRegister<GpuRegister>();
      Location rhs_location = locations->InAt(1);

      GpuRegister rhs_reg = ZERO;
      int64_t rhs_imm = 0;
      bool use_imm = rhs_location.IsConstant();
      if (use_imm) {
        rhs_imm = CodeGenerator::GetInt64ValueOf(rhs_location.GetConstant());
      } else {
        rhs_reg = rhs_location.AsRegister<GpuRegister>();
      }

      if (instruction->IsAnd()) {
        if (use_imm)
          __ Andi(dst, lhs, rhs_imm);
        else
          __ And(dst, lhs, rhs_reg);
      } else if (instruction->IsOr()) {
        if (use_imm)
          __ Ori(dst, lhs, rhs_imm);
        else
          __ Or(dst, lhs, rhs_reg);
      } else if (instruction->IsXor()) {
        if (use_imm)
          __ Xori(dst, lhs, rhs_imm);
        else
          __ Xor(dst, lhs, rhs_reg);
      } else if (instruction->IsAdd()) {
        if (type == Primitive::kPrimInt) {
          if (use_imm)
            __ Addiu(dst, lhs, rhs_imm);
          else
            __ Addu(dst, lhs, rhs_reg);
        } else {
          if (use_imm)
            __ Daddiu(dst, lhs, rhs_imm);
          else
            __ Daddu(dst, lhs, rhs_reg);
        }
      } else {
        DCHECK(instruction->IsSub());
        if (type == Primitive::kPrimInt) {
          if (use_imm)
            __ Addiu(dst, lhs, -rhs_imm);
          else
            __ Subu(dst, lhs, rhs_reg);
        } else {
          if (use_imm)
            __ Daddiu(dst, lhs, -rhs_imm);
          else
            __ Dsubu(dst, lhs, rhs_reg);
        }
      }
      break;
    }
    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      FpuRegister dst = locations->Out().AsFpuRegister<FpuRegister>();
      FpuRegister lhs = locations->InAt(0).AsFpuRegister<FpuRegister>();
      FpuRegister rhs = locations->InAt(1).AsFpuRegister<FpuRegister>();
      if (instruction->IsAdd()) {
        if (type == Primitive::kPrimFloat)
          __ AddS(dst, lhs, rhs);
        else
          __ AddD(dst, lhs, rhs);
      } else if (instruction->IsSub()) {
        if (type == Primitive::kPrimFloat)
          __ SubS(dst, lhs, rhs);
        else
          __ SubD(dst, lhs, rhs);
      } else {
        LOG(FATAL) << "Unexpected floating-point binary operation";
      }
      break;
    }
    default:
      LOG(FATAL) << "Unexpected binary operation type " << type;
  }
}

void LocationsBuilderMIPS64::HandleShift(HBinaryOperation* instr) {
  DCHECK(instr->IsShl() || instr->IsShr() || instr->IsUShr());

  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instr);
  Primitive::Type type = instr->GetResultType();
  switch (type) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong: {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RegisterOrConstant(instr->InputAt(1)));
      locations->SetOut(Location::RequiresRegister());
      break;
    }
    default:
      LOG(FATAL) << "Unexpected shift type " << type;
  }
}

void InstructionCodeGeneratorMIPS64::HandleShift(HBinaryOperation* instr) {
  DCHECK(instr->IsShl() || instr->IsShr() || instr->IsUShr());
  LocationSummary* locations = instr->GetLocations();
  Primitive::Type type = instr->GetType();

  switch (type) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong: {
      GpuRegister dst = locations->Out().AsRegister<GpuRegister>();
      GpuRegister lhs = locations->InAt(0).AsRegister<GpuRegister>();
      Location rhs_location = locations->InAt(1);

      GpuRegister rhs_reg = ZERO;
      int64_t rhs_imm = 0;
      bool use_imm = rhs_location.IsConstant();
      if (use_imm) {
        rhs_imm = CodeGenerator::GetInt64ValueOf(rhs_location.GetConstant());
      } else {
        rhs_reg = rhs_location.AsRegister<GpuRegister>();
      }

      if (use_imm) {
        uint32_t shift_value = (type == Primitive::kPrimInt)
          ? static_cast<uint32_t>(rhs_imm & kMaxIntShiftValue)
          : static_cast<uint32_t>(rhs_imm & kMaxLongShiftValue);

        if (type == Primitive::kPrimInt) {
          if (instr->IsShl()) {
            __ Sll(dst, lhs, shift_value);
          } else if (instr->IsShr()) {
            __ Sra(dst, lhs, shift_value);
          } else {
            __ Srl(dst, lhs, shift_value);
          }
        } else {
          if (shift_value < 32) {
            if (instr->IsShl()) {
              __ Dsll(dst, lhs, shift_value);
            } else if (instr->IsShr()) {
              __ Dsra(dst, lhs, shift_value);
            } else {
              __ Dsrl(dst, lhs, shift_value);
            }
          } else {
            shift_value -= 32;
            if (instr->IsShl()) {
              __ Dsll32(dst, lhs, shift_value);
            } else if (instr->IsShr()) {
              __ Dsra32(dst, lhs, shift_value);
            } else {
              __ Dsrl32(dst, lhs, shift_value);
            }
          }
        }
      } else {
        if (type == Primitive::kPrimInt) {
          if (instr->IsShl()) {
            __ Sllv(dst, lhs, rhs_reg);
          } else if (instr->IsShr()) {
            __ Srav(dst, lhs, rhs_reg);
          } else {
            __ Srlv(dst, lhs, rhs_reg);
          }
        } else {
          if (instr->IsShl()) {
            __ Dsllv(dst, lhs, rhs_reg);
          } else if (instr->IsShr()) {
            __ Dsrav(dst, lhs, rhs_reg);
          } else {
            __ Dsrlv(dst, lhs, rhs_reg);
          }
        }
      }
      break;
    }
    default:
      LOG(FATAL) << "Unexpected shift operation type " << type;
  }
}

void LocationsBuilderMIPS64::VisitAdd(HAdd* instruction) {
  HandleBinaryOp(instruction);
}

void InstructionCodeGeneratorMIPS64::VisitAdd(HAdd* instruction) {
  HandleBinaryOp(instruction);
}

void LocationsBuilderMIPS64::VisitAnd(HAnd* instruction) {
  HandleBinaryOp(instruction);
}

void InstructionCodeGeneratorMIPS64::VisitAnd(HAnd* instruction) {
  HandleBinaryOp(instruction);
}

void LocationsBuilderMIPS64::VisitArrayGet(HArrayGet* instruction) {
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

void InstructionCodeGeneratorMIPS64::VisitArrayGet(HArrayGet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  GpuRegister obj = locations->InAt(0).AsRegister<GpuRegister>();
  Location index = locations->InAt(1);
  Primitive::Type type = instruction->GetType();

  switch (type) {
    case Primitive::kPrimBoolean: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(uint8_t)).Uint32Value();
      GpuRegister out = locations->Out().AsRegister<GpuRegister>();
      if (index.IsConstant()) {
        size_t offset =
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_1) + data_offset;
        __ LoadFromOffset(kLoadUnsignedByte, out, obj, offset);
      } else {
        __ Daddu(TMP, obj, index.AsRegister<GpuRegister>());
        __ LoadFromOffset(kLoadUnsignedByte, out, TMP, data_offset);
      }
      break;
    }

    case Primitive::kPrimByte: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(int8_t)).Uint32Value();
      GpuRegister out = locations->Out().AsRegister<GpuRegister>();
      if (index.IsConstant()) {
        size_t offset =
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_1) + data_offset;
        __ LoadFromOffset(kLoadSignedByte, out, obj, offset);
      } else {
        __ Daddu(TMP, obj, index.AsRegister<GpuRegister>());
        __ LoadFromOffset(kLoadSignedByte, out, TMP, data_offset);
      }
      break;
    }

    case Primitive::kPrimShort: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(int16_t)).Uint32Value();
      GpuRegister out = locations->Out().AsRegister<GpuRegister>();
      if (index.IsConstant()) {
        size_t offset =
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_2) + data_offset;
        __ LoadFromOffset(kLoadSignedHalfword, out, obj, offset);
      } else {
        __ Dsll(TMP, index.AsRegister<GpuRegister>(), TIMES_2);
        __ Daddu(TMP, obj, TMP);
        __ LoadFromOffset(kLoadSignedHalfword, out, TMP, data_offset);
      }
      break;
    }

    case Primitive::kPrimChar: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(uint16_t)).Uint32Value();
      GpuRegister out = locations->Out().AsRegister<GpuRegister>();
      if (index.IsConstant()) {
        size_t offset =
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_2) + data_offset;
        __ LoadFromOffset(kLoadUnsignedHalfword, out, obj, offset);
      } else {
        __ Dsll(TMP, index.AsRegister<GpuRegister>(), TIMES_2);
        __ Daddu(TMP, obj, TMP);
        __ LoadFromOffset(kLoadUnsignedHalfword, out, TMP, data_offset);
      }
      break;
    }

    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      DCHECK_EQ(sizeof(mirror::HeapReference<mirror::Object>), sizeof(int32_t));
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(int32_t)).Uint32Value();
      GpuRegister out = locations->Out().AsRegister<GpuRegister>();
      LoadOperandType load_type = (type == Primitive::kPrimNot) ? kLoadUnsignedWord : kLoadWord;
      if (index.IsConstant()) {
        size_t offset =
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_4) + data_offset;
        __ LoadFromOffset(load_type, out, obj, offset);
      } else {
        __ Dsll(TMP, index.AsRegister<GpuRegister>(), TIMES_4);
        __ Daddu(TMP, obj, TMP);
        __ LoadFromOffset(load_type, out, TMP, data_offset);
      }
      break;
    }

    case Primitive::kPrimLong: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(int64_t)).Uint32Value();
      GpuRegister out = locations->Out().AsRegister<GpuRegister>();
      if (index.IsConstant()) {
        size_t offset =
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_8) + data_offset;
        __ LoadFromOffset(kLoadDoubleword, out, obj, offset);
      } else {
        __ Dsll(TMP, index.AsRegister<GpuRegister>(), TIMES_8);
        __ Daddu(TMP, obj, TMP);
        __ LoadFromOffset(kLoadDoubleword, out, TMP, data_offset);
      }
      break;
    }

    case Primitive::kPrimFloat: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(float)).Uint32Value();
      FpuRegister out = locations->Out().AsFpuRegister<FpuRegister>();
      if (index.IsConstant()) {
        size_t offset =
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_4) + data_offset;
        __ LoadFpuFromOffset(kLoadWord, out, obj, offset);
      } else {
        __ Dsll(TMP, index.AsRegister<GpuRegister>(), TIMES_4);
        __ Daddu(TMP, obj, TMP);
        __ LoadFpuFromOffset(kLoadWord, out, TMP, data_offset);
      }
      break;
    }

    case Primitive::kPrimDouble: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(double)).Uint32Value();
      FpuRegister out = locations->Out().AsFpuRegister<FpuRegister>();
      if (index.IsConstant()) {
        size_t offset =
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_8) + data_offset;
        __ LoadFpuFromOffset(kLoadDoubleword, out, obj, offset);
      } else {
        __ Dsll(TMP, index.AsRegister<GpuRegister>(), TIMES_8);
        __ Daddu(TMP, obj, TMP);
        __ LoadFpuFromOffset(kLoadDoubleword, out, TMP, data_offset);
      }
      break;
    }

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << instruction->GetType();
      UNREACHABLE();
  }
  codegen_->MaybeRecordImplicitNullCheck(instruction);
}

void LocationsBuilderMIPS64::VisitArrayLength(HArrayLength* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

void InstructionCodeGeneratorMIPS64::VisitArrayLength(HArrayLength* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  uint32_t offset = mirror::Array::LengthOffset().Uint32Value();
  GpuRegister obj = locations->InAt(0).AsRegister<GpuRegister>();
  GpuRegister out = locations->Out().AsRegister<GpuRegister>();
  __ LoadFromOffset(kLoadWord, out, obj, offset);
  codegen_->MaybeRecordImplicitNullCheck(instruction);
}

void LocationsBuilderMIPS64::VisitArraySet(HArraySet* instruction) {
  Primitive::Type value_type = instruction->GetComponentType();
  bool is_object = value_type == Primitive::kPrimNot;
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(
      instruction,
      is_object ? LocationSummary::kCall : LocationSummary::kNoCall);
  if (is_object) {
    InvokeRuntimeCallingConvention calling_convention;
    locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
    locations->SetInAt(1, Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
    locations->SetInAt(2, Location::RegisterLocation(calling_convention.GetRegisterAt(2)));
  } else {
    locations->SetInAt(0, Location::RequiresRegister());
    locations->SetInAt(1, Location::RegisterOrConstant(instruction->InputAt(1)));
    if (Primitive::IsFloatingPointType(instruction->InputAt(2)->GetType())) {
      locations->SetInAt(2, Location::RequiresFpuRegister());
    } else {
      locations->SetInAt(2, Location::RequiresRegister());
    }
  }
}

void InstructionCodeGeneratorMIPS64::VisitArraySet(HArraySet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  GpuRegister obj = locations->InAt(0).AsRegister<GpuRegister>();
  Location index = locations->InAt(1);
  Primitive::Type value_type = instruction->GetComponentType();
  bool needs_runtime_call = locations->WillCall();
  bool needs_write_barrier =
      CodeGenerator::StoreNeedsWriteBarrier(value_type, instruction->GetValue());

  switch (value_type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(uint8_t)).Uint32Value();
      GpuRegister value = locations->InAt(2).AsRegister<GpuRegister>();
      if (index.IsConstant()) {
        size_t offset =
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_1) + data_offset;
        __ StoreToOffset(kStoreByte, value, obj, offset);
      } else {
        __ Daddu(TMP, obj, index.AsRegister<GpuRegister>());
        __ StoreToOffset(kStoreByte, value, TMP, data_offset);
      }
      break;
    }

    case Primitive::kPrimShort:
    case Primitive::kPrimChar: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(uint16_t)).Uint32Value();
      GpuRegister value = locations->InAt(2).AsRegister<GpuRegister>();
      if (index.IsConstant()) {
        size_t offset =
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_2) + data_offset;
        __ StoreToOffset(kStoreHalfword, value, obj, offset);
      } else {
        __ Dsll(TMP, index.AsRegister<GpuRegister>(), TIMES_2);
        __ Daddu(TMP, obj, TMP);
        __ StoreToOffset(kStoreHalfword, value, TMP, data_offset);
      }
      break;
    }

    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      if (!needs_runtime_call) {
        uint32_t data_offset = mirror::Array::DataOffset(sizeof(int32_t)).Uint32Value();
        GpuRegister value = locations->InAt(2).AsRegister<GpuRegister>();
        if (index.IsConstant()) {
          size_t offset =
              (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_4) + data_offset;
          __ StoreToOffset(kStoreWord, value, obj, offset);
        } else {
          DCHECK(index.IsRegister()) << index;
          __ Dsll(TMP, index.AsRegister<GpuRegister>(), TIMES_4);
          __ Daddu(TMP, obj, TMP);
          __ StoreToOffset(kStoreWord, value, TMP, data_offset);
        }
        codegen_->MaybeRecordImplicitNullCheck(instruction);
        if (needs_write_barrier) {
          DCHECK_EQ(value_type, Primitive::kPrimNot);
          codegen_->MarkGCCard(obj, value);
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
      GpuRegister value = locations->InAt(2).AsRegister<GpuRegister>();
      if (index.IsConstant()) {
        size_t offset =
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_8) + data_offset;
        __ StoreToOffset(kStoreDoubleword, value, obj, offset);
      } else {
        __ Dsll(TMP, index.AsRegister<GpuRegister>(), TIMES_8);
        __ Daddu(TMP, obj, TMP);
        __ StoreToOffset(kStoreDoubleword, value, TMP, data_offset);
      }
      break;
    }

    case Primitive::kPrimFloat: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(float)).Uint32Value();
      FpuRegister value = locations->InAt(2).AsFpuRegister<FpuRegister>();
      DCHECK(locations->InAt(2).IsFpuRegister());
      if (index.IsConstant()) {
        size_t offset =
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_4) + data_offset;
        __ StoreFpuToOffset(kStoreWord, value, obj, offset);
      } else {
        __ Dsll(TMP, index.AsRegister<GpuRegister>(), TIMES_4);
        __ Daddu(TMP, obj, TMP);
        __ StoreFpuToOffset(kStoreWord, value, TMP, data_offset);
      }
      break;
    }

    case Primitive::kPrimDouble: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(double)).Uint32Value();
      FpuRegister value = locations->InAt(2).AsFpuRegister<FpuRegister>();
      DCHECK(locations->InAt(2).IsFpuRegister());
      if (index.IsConstant()) {
        size_t offset =
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_8) + data_offset;
        __ StoreFpuToOffset(kStoreDoubleword, value, obj, offset);
      } else {
        __ Dsll(TMP, index.AsRegister<GpuRegister>(), TIMES_8);
        __ Daddu(TMP, obj, TMP);
        __ StoreFpuToOffset(kStoreDoubleword, value, TMP, data_offset);
      }
      break;
    }

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << instruction->GetType();
      UNREACHABLE();
  }

  // Ints and objects are handled in the switch.
  if (value_type != Primitive::kPrimInt && value_type != Primitive::kPrimNot) {
    codegen_->MaybeRecordImplicitNullCheck(instruction);
  }
}

void LocationsBuilderMIPS64::VisitBoundsCheck(HBoundsCheck* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  if (instruction->HasUses()) {
    locations->SetOut(Location::SameAsFirstInput());
  }
}

void InstructionCodeGeneratorMIPS64::VisitBoundsCheck(HBoundsCheck* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  BoundsCheckSlowPathMIPS64* slow_path = new (GetGraph()->GetArena()) BoundsCheckSlowPathMIPS64(
      instruction,
      locations->InAt(0),
      locations->InAt(1));
  codegen_->AddSlowPath(slow_path);

  GpuRegister index = locations->InAt(0).AsRegister<GpuRegister>();
  GpuRegister length = locations->InAt(1).AsRegister<GpuRegister>();

  // length is limited by the maximum positive signed 32-bit integer.
  // Unsigned comparison of length and index checks for index < 0
  // and for length <= index simultaneously.
  // Mips R6 requires lhs != rhs for compact branches.
  if (index == length) {
    __ B(slow_path->GetEntryLabel());
  } else {
    __ Bgeuc(index, length, slow_path->GetEntryLabel());
  }
}

void LocationsBuilderMIPS64::VisitCheckCast(HCheckCast* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(
      instruction,
      LocationSummary::kCallOnSlowPath);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  locations->AddTemp(Location::RequiresRegister());
}

void InstructionCodeGeneratorMIPS64::VisitCheckCast(HCheckCast* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  GpuRegister obj = locations->InAt(0).AsRegister<GpuRegister>();
  GpuRegister cls = locations->InAt(1).AsRegister<GpuRegister>();
  GpuRegister obj_cls = locations->GetTemp(0).AsRegister<GpuRegister>();

  SlowPathCodeMIPS64* slow_path = new (GetGraph()->GetArena()) TypeCheckSlowPathMIPS64(
      instruction,
      locations->InAt(1),
      Location::RegisterLocation(obj_cls),
      instruction->GetDexPc());
  codegen_->AddSlowPath(slow_path);

  // TODO: avoid this check if we know obj is not null.
  __ Beqzc(obj, slow_path->GetExitLabel());
  // Compare the class of `obj` with `cls`.
  __ LoadFromOffset(kLoadUnsignedWord, obj_cls, obj, mirror::Object::ClassOffset().Int32Value());
  __ Bnec(obj_cls, cls, slow_path->GetEntryLabel());
  __ Bind(slow_path->GetExitLabel());
}

void LocationsBuilderMIPS64::VisitClinitCheck(HClinitCheck* check) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(check, LocationSummary::kCallOnSlowPath);
  locations->SetInAt(0, Location::RequiresRegister());
  if (check->HasUses()) {
    locations->SetOut(Location::SameAsFirstInput());
  }
}

void InstructionCodeGeneratorMIPS64::VisitClinitCheck(HClinitCheck* check) {
  // We assume the class is not null.
  SlowPathCodeMIPS64* slow_path = new (GetGraph()->GetArena()) LoadClassSlowPathMIPS64(
      check->GetLoadClass(),
      check,
      check->GetDexPc(),
      true);
  codegen_->AddSlowPath(slow_path);
  GenerateClassInitializationCheck(slow_path,
                                   check->GetLocations()->InAt(0).AsRegister<GpuRegister>());
}

void LocationsBuilderMIPS64::VisitCompare(HCompare* compare) {
  Primitive::Type in_type = compare->InputAt(0)->GetType();

  LocationSummary::CallKind call_kind = Primitive::IsFloatingPointType(in_type)
      ? LocationSummary::kCall
      : LocationSummary::kNoCall;

  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(compare, call_kind);

  switch (in_type) {
    case Primitive::kPrimLong:
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RequiresRegister());
      locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      break;

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      InvokeRuntimeCallingConvention calling_convention;
      locations->SetInAt(0, Location::FpuRegisterLocation(calling_convention.GetFpuRegisterAt(0)));
      locations->SetInAt(1, Location::FpuRegisterLocation(calling_convention.GetFpuRegisterAt(1)));
      locations->SetOut(calling_convention.GetReturnLocation(Primitive::kPrimInt));
      break;
    }

    default:
      LOG(FATAL) << "Unexpected type for compare operation " << in_type;
  }
}

void InstructionCodeGeneratorMIPS64::VisitCompare(HCompare* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Primitive::Type in_type = instruction->InputAt(0)->GetType();

  //  0 if: left == right
  //  1 if: left  > right
  // -1 if: left  < right
  switch (in_type) {
    case Primitive::kPrimLong: {
      GpuRegister dst = locations->Out().AsRegister<GpuRegister>();
      GpuRegister lhs = locations->InAt(0).AsRegister<GpuRegister>();
      GpuRegister rhs = locations->InAt(1).AsRegister<GpuRegister>();
      // TODO: more efficient (direct) comparison with a constant
      __ Slt(TMP, lhs, rhs);
      __ Slt(dst, rhs, lhs);
      __ Subu(dst, dst, TMP);
      break;
    }

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      int32_t entry_point_offset;
      if (in_type == Primitive::kPrimFloat) {
        entry_point_offset = instruction->IsGtBias() ? QUICK_ENTRY_POINT(pCmpgFloat)
                                                     : QUICK_ENTRY_POINT(pCmplFloat);
      } else {
        entry_point_offset = instruction->IsGtBias() ? QUICK_ENTRY_POINT(pCmpgDouble)
                                                     : QUICK_ENTRY_POINT(pCmplDouble);
      }
      codegen_->InvokeRuntime(entry_point_offset, instruction, instruction->GetDexPc(), nullptr);
      break;
    }

    default:
      LOG(FATAL) << "Unimplemented compare type " << in_type;
  }
}

void LocationsBuilderMIPS64::VisitCondition(HCondition* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RegisterOrConstant(instruction->InputAt(1)));
  if (instruction->NeedsMaterialization()) {
    locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
  }
}

void InstructionCodeGeneratorMIPS64::VisitCondition(HCondition* instruction) {
  if (!instruction->NeedsMaterialization()) {
    return;
  }

  LocationSummary* locations = instruction->GetLocations();

  GpuRegister dst = locations->Out().AsRegister<GpuRegister>();
  GpuRegister lhs = locations->InAt(0).AsRegister<GpuRegister>();
  Location rhs_location = locations->InAt(1);

  GpuRegister rhs_reg = ZERO;
  int64_t rhs_imm = 0;
  bool use_imm = rhs_location.IsConstant();
  if (use_imm) {
    rhs_imm = CodeGenerator::GetInt32ValueOf(rhs_location.GetConstant());
  } else {
    rhs_reg = rhs_location.AsRegister<GpuRegister>();
  }

  IfCondition if_cond = instruction->GetCondition();

  switch (if_cond) {
    case kCondEQ:
    case kCondNE:
      if (use_imm && IsUint<16>(rhs_imm)) {
        __ Xori(dst, lhs, rhs_imm);
      } else {
        if (use_imm) {
          rhs_reg = TMP;
          __ LoadConst32(rhs_reg, rhs_imm);
        }
        __ Xor(dst, lhs, rhs_reg);
      }
      if (if_cond == kCondEQ) {
        __ Sltiu(dst, dst, 1);
      } else {
        __ Sltu(dst, ZERO, dst);
      }
      break;

    case kCondLT:
    case kCondGE:
      if (use_imm && IsInt<16>(rhs_imm)) {
        __ Slti(dst, lhs, rhs_imm);
      } else {
        if (use_imm) {
          rhs_reg = TMP;
          __ LoadConst32(rhs_reg, rhs_imm);
        }
        __ Slt(dst, lhs, rhs_reg);
      }
      if (if_cond == kCondGE) {
        // Simulate lhs >= rhs via !(lhs < rhs) since there's
        // only the slt instruction but no sge.
        __ Xori(dst, dst, 1);
      }
      break;

    case kCondLE:
    case kCondGT:
      if (use_imm && IsInt<16>(rhs_imm + 1)) {
        // Simulate lhs <= rhs via lhs < rhs + 1.
        __ Slti(dst, lhs, rhs_imm + 1);
        if (if_cond == kCondGT) {
          // Simulate lhs > rhs via !(lhs <= rhs) since there's
          // only the slti instruction but no sgti.
          __ Xori(dst, dst, 1);
        }
      } else {
        if (use_imm) {
          rhs_reg = TMP;
          __ LoadConst32(rhs_reg, rhs_imm);
        }
        __ Slt(dst, rhs_reg, lhs);
        if (if_cond == kCondLE) {
          // Simulate lhs <= rhs via !(rhs < lhs) since there's
          // only the slt instruction but no sle.
          __ Xori(dst, dst, 1);
        }
      }
      break;
  }
}

void LocationsBuilderMIPS64::VisitDiv(HDiv* div) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(div, LocationSummary::kNoCall);
  switch (div->GetResultType()) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong:
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RequiresRegister());
      locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      break;

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetInAt(1, Location::RequiresFpuRegister());
      locations->SetOut(Location::RequiresFpuRegister(), Location::kNoOutputOverlap);
      break;

    default:
      LOG(FATAL) << "Unexpected div type " << div->GetResultType();
  }
}

void InstructionCodeGeneratorMIPS64::VisitDiv(HDiv* instruction) {
  Primitive::Type type = instruction->GetType();
  LocationSummary* locations = instruction->GetLocations();

  switch (type) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong: {
      GpuRegister dst = locations->Out().AsRegister<GpuRegister>();
      GpuRegister lhs = locations->InAt(0).AsRegister<GpuRegister>();
      GpuRegister rhs = locations->InAt(1).AsRegister<GpuRegister>();
      if (type == Primitive::kPrimInt)
        __ DivR6(dst, lhs, rhs);
      else
        __ Ddiv(dst, lhs, rhs);
      break;
    }
    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      FpuRegister dst = locations->Out().AsFpuRegister<FpuRegister>();
      FpuRegister lhs = locations->InAt(0).AsFpuRegister<FpuRegister>();
      FpuRegister rhs = locations->InAt(1).AsFpuRegister<FpuRegister>();
      if (type == Primitive::kPrimFloat)
        __ DivS(dst, lhs, rhs);
      else
        __ DivD(dst, lhs, rhs);
      break;
    }
    default:
      LOG(FATAL) << "Unexpected div type " << type;
  }
}

void LocationsBuilderMIPS64::VisitDivZeroCheck(HDivZeroCheck* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RegisterOrConstant(instruction->InputAt(0)));
  if (instruction->HasUses()) {
    locations->SetOut(Location::SameAsFirstInput());
  }
}

void InstructionCodeGeneratorMIPS64::VisitDivZeroCheck(HDivZeroCheck* instruction) {
  SlowPathCodeMIPS64* slow_path =
      new (GetGraph()->GetArena()) DivZeroCheckSlowPathMIPS64(instruction);
  codegen_->AddSlowPath(slow_path);
  Location value = instruction->GetLocations()->InAt(0);

  Primitive::Type type = instruction->GetType();

  if ((type != Primitive::kPrimInt) && (type != Primitive::kPrimLong)) {
      LOG(FATAL) << "Unexpected type " << type << " for DivZeroCheck.";
  }

  if (value.IsConstant()) {
    int64_t divisor = codegen_->GetInt64ValueOf(value.GetConstant()->AsConstant());
    if (divisor == 0) {
      __ B(slow_path->GetEntryLabel());
    } else {
      // A division by a non-null constant is valid. We don't need to perform
      // any check, so simply fall through.
    }
  } else {
    __ Beqzc(value.AsRegister<GpuRegister>(), slow_path->GetEntryLabel());
  }
}

void LocationsBuilderMIPS64::VisitDoubleConstant(HDoubleConstant* constant) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(constant, LocationSummary::kNoCall);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorMIPS64::VisitDoubleConstant(HDoubleConstant* cst ATTRIBUTE_UNUSED) {
  // Will be generated at use site.
}

void LocationsBuilderMIPS64::VisitExit(HExit* exit) {
  exit->SetLocations(nullptr);
}

void InstructionCodeGeneratorMIPS64::VisitExit(HExit* exit ATTRIBUTE_UNUSED) {
}

void LocationsBuilderMIPS64::VisitFloatConstant(HFloatConstant* constant) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(constant, LocationSummary::kNoCall);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorMIPS64::VisitFloatConstant(HFloatConstant* constant ATTRIBUTE_UNUSED) {
  // Will be generated at use site.
}

void LocationsBuilderMIPS64::VisitGoto(HGoto* got) {
  got->SetLocations(nullptr);
}

void InstructionCodeGeneratorMIPS64::VisitGoto(HGoto* got) {
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
  if (!codegen_->GoesToNextBlock(block, successor)) {
    __ B(codegen_->GetLabelOf(successor));
  }
}

void InstructionCodeGeneratorMIPS64::GenerateTestAndBranch(HInstruction* instruction,
                                                           Label* true_target,
                                                           Label* false_target,
                                                           Label* always_true_target) {
  HInstruction* cond = instruction->InputAt(0);
  HCondition* condition = cond->AsCondition();

  if (cond->IsIntConstant()) {
    int32_t cond_value = cond->AsIntConstant()->GetValue();
    if (cond_value == 1) {
      if (always_true_target != nullptr) {
        __ B(always_true_target);
      }
      return;
    } else {
      DCHECK_EQ(cond_value, 0);
    }
  } else if (!cond->IsCondition() || condition->NeedsMaterialization()) {
    // The condition instruction has been materialized, compare the output to 0.
    Location cond_val = instruction->GetLocations()->InAt(0);
    DCHECK(cond_val.IsRegister());
    __ Bnezc(cond_val.AsRegister<GpuRegister>(), true_target);
  } else {
    // The condition instruction has not been materialized, use its inputs as
    // the comparison and its condition as the branch condition.
    GpuRegister lhs = condition->GetLocations()->InAt(0).AsRegister<GpuRegister>();
    Location rhs_location = condition->GetLocations()->InAt(1);
    GpuRegister rhs_reg = ZERO;
    int32_t rhs_imm = 0;
    bool use_imm = rhs_location.IsConstant();
    if (use_imm) {
      rhs_imm = CodeGenerator::GetInt32ValueOf(rhs_location.GetConstant());
    } else {
      rhs_reg = rhs_location.AsRegister<GpuRegister>();
    }

    IfCondition if_cond = condition->GetCondition();
    if (use_imm && rhs_imm == 0) {
      switch (if_cond) {
        case kCondEQ:
          __ Beqzc(lhs, true_target);
          break;
        case kCondNE:
          __ Bnezc(lhs, true_target);
          break;
        case kCondLT:
          __ Bltzc(lhs, true_target);
          break;
        case kCondGE:
          __ Bgezc(lhs, true_target);
          break;
        case kCondLE:
          __ Blezc(lhs, true_target);
          break;
        case kCondGT:
          __ Bgtzc(lhs, true_target);
          break;
      }
    } else {
      if (use_imm) {
        rhs_reg = TMP;
        __ LoadConst32(rhs_reg, rhs_imm);
      }
      // It looks like we can get here with lhs == rhs. Should that be possible at all?
      // Mips R6 requires lhs != rhs for compact branches.
      if (lhs == rhs_reg) {
        DCHECK(!use_imm);
        switch (if_cond) {
          case kCondEQ:
          case kCondGE:
          case kCondLE:
            // if lhs == rhs for a positive condition, then it is a branch
            __ B(true_target);
            break;
          case kCondNE:
          case kCondLT:
          case kCondGT:
            // if lhs == rhs for a negative condition, then it is a NOP
            break;
        }
      } else {
        switch (if_cond) {
          case kCondEQ:
            __ Beqc(lhs, rhs_reg, true_target);
            break;
          case kCondNE:
            __ Bnec(lhs, rhs_reg, true_target);
            break;
          case kCondLT:
            __ Bltc(lhs, rhs_reg, true_target);
            break;
          case kCondGE:
            __ Bgec(lhs, rhs_reg, true_target);
            break;
          case kCondLE:
            __ Bgec(rhs_reg, lhs, true_target);
            break;
          case kCondGT:
            __ Bltc(rhs_reg, lhs, true_target);
            break;
        }
      }
    }
  }
  if (false_target != nullptr) {
    __ B(false_target);
  }
}

void LocationsBuilderMIPS64::VisitIf(HIf* if_instr) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(if_instr);
  HInstruction* cond = if_instr->InputAt(0);
  if (!cond->IsCondition() || cond->AsCondition()->NeedsMaterialization()) {
    locations->SetInAt(0, Location::RequiresRegister());
  }
}

void InstructionCodeGeneratorMIPS64::VisitIf(HIf* if_instr) {
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

void LocationsBuilderMIPS64::VisitDeoptimize(HDeoptimize* deoptimize) {
  LocationSummary* locations = new (GetGraph()->GetArena())
      LocationSummary(deoptimize, LocationSummary::kCallOnSlowPath);
  HInstruction* cond = deoptimize->InputAt(0);
  DCHECK(cond->IsCondition());
  if (cond->AsCondition()->NeedsMaterialization()) {
    locations->SetInAt(0, Location::RequiresRegister());
  }
}

void InstructionCodeGeneratorMIPS64::VisitDeoptimize(HDeoptimize* deoptimize) {
  SlowPathCodeMIPS64* slow_path = new (GetGraph()->GetArena())
      DeoptimizationSlowPathMIPS64(deoptimize);
  codegen_->AddSlowPath(slow_path);
  Label* slow_path_entry = slow_path->GetEntryLabel();
  GenerateTestAndBranch(deoptimize, slow_path_entry, nullptr, slow_path_entry);
}

void LocationsBuilderMIPS64::HandleFieldGet(HInstruction* instruction,
                                            const FieldInfo& field_info ATTRIBUTE_UNUSED) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  if (Primitive::IsFloatingPointType(instruction->GetType())) {
    locations->SetOut(Location::RequiresFpuRegister());
  } else {
    locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
  }
}

void InstructionCodeGeneratorMIPS64::HandleFieldGet(HInstruction* instruction,
                                                    const FieldInfo& field_info) {
  Primitive::Type type = field_info.GetFieldType();
  LocationSummary* locations = instruction->GetLocations();
  GpuRegister obj = locations->InAt(0).AsRegister<GpuRegister>();
  LoadOperandType load_type = kLoadUnsignedByte;
  switch (type) {
    case Primitive::kPrimBoolean:
      load_type = kLoadUnsignedByte;
      break;
    case Primitive::kPrimByte:
      load_type = kLoadSignedByte;
      break;
    case Primitive::kPrimShort:
      load_type = kLoadSignedHalfword;
      break;
    case Primitive::kPrimChar:
      load_type = kLoadUnsignedHalfword;
      break;
    case Primitive::kPrimInt:
    case Primitive::kPrimFloat:
      load_type = kLoadWord;
      break;
    case Primitive::kPrimLong:
    case Primitive::kPrimDouble:
      load_type = kLoadDoubleword;
      break;
    case Primitive::kPrimNot:
      load_type = kLoadUnsignedWord;
      break;
    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << type;
      UNREACHABLE();
  }
  if (!Primitive::IsFloatingPointType(type)) {
    DCHECK(locations->Out().IsRegister());
    GpuRegister dst = locations->Out().AsRegister<GpuRegister>();
    __ LoadFromOffset(load_type, dst, obj, field_info.GetFieldOffset().Uint32Value());
  } else {
    DCHECK(locations->Out().IsFpuRegister());
    FpuRegister dst = locations->Out().AsFpuRegister<FpuRegister>();
    __ LoadFpuFromOffset(load_type, dst, obj, field_info.GetFieldOffset().Uint32Value());
  }

  codegen_->MaybeRecordImplicitNullCheck(instruction);
  // TODO: memory barrier?
}

void LocationsBuilderMIPS64::HandleFieldSet(HInstruction* instruction,
                                            const FieldInfo& field_info ATTRIBUTE_UNUSED) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  if (Primitive::IsFloatingPointType(instruction->InputAt(1)->GetType())) {
    locations->SetInAt(1, Location::RequiresFpuRegister());
  } else {
    locations->SetInAt(1, Location::RequiresRegister());
  }
}

void InstructionCodeGeneratorMIPS64::HandleFieldSet(HInstruction* instruction,
                                                    const FieldInfo& field_info) {
  Primitive::Type type = field_info.GetFieldType();
  LocationSummary* locations = instruction->GetLocations();
  GpuRegister obj = locations->InAt(0).AsRegister<GpuRegister>();
  StoreOperandType store_type = kStoreByte;
  switch (type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
      store_type = kStoreByte;
      break;
    case Primitive::kPrimShort:
    case Primitive::kPrimChar:
      store_type = kStoreHalfword;
      break;
    case Primitive::kPrimInt:
    case Primitive::kPrimFloat:
    case Primitive::kPrimNot:
      store_type = kStoreWord;
      break;
    case Primitive::kPrimLong:
    case Primitive::kPrimDouble:
      store_type = kStoreDoubleword;
      break;
    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << type;
      UNREACHABLE();
  }
  if (!Primitive::IsFloatingPointType(type)) {
    DCHECK(locations->InAt(1).IsRegister());
    GpuRegister src = locations->InAt(1).AsRegister<GpuRegister>();
    __ StoreToOffset(store_type, src, obj, field_info.GetFieldOffset().Uint32Value());
  } else {
    DCHECK(locations->InAt(1).IsFpuRegister());
    FpuRegister src = locations->InAt(1).AsFpuRegister<FpuRegister>();
    __ StoreFpuToOffset(store_type, src, obj, field_info.GetFieldOffset().Uint32Value());
  }

  codegen_->MaybeRecordImplicitNullCheck(instruction);
  // TODO: memory barriers?
  if (CodeGenerator::StoreNeedsWriteBarrier(type, instruction->InputAt(1))) {
    DCHECK(locations->InAt(1).IsRegister());
    GpuRegister src = locations->InAt(1).AsRegister<GpuRegister>();
    codegen_->MarkGCCard(obj, src);
  }
}

void LocationsBuilderMIPS64::VisitInstanceFieldGet(HInstanceFieldGet* instruction) {
  HandleFieldGet(instruction, instruction->GetFieldInfo());
}

void InstructionCodeGeneratorMIPS64::VisitInstanceFieldGet(HInstanceFieldGet* instruction) {
  HandleFieldGet(instruction, instruction->GetFieldInfo());
}

void LocationsBuilderMIPS64::VisitInstanceFieldSet(HInstanceFieldSet* instruction) {
  HandleFieldSet(instruction, instruction->GetFieldInfo());
}

void InstructionCodeGeneratorMIPS64::VisitInstanceFieldSet(HInstanceFieldSet* instruction) {
  HandleFieldSet(instruction, instruction->GetFieldInfo());
}

void LocationsBuilderMIPS64::VisitInstanceOf(HInstanceOf* instruction) {
  LocationSummary::CallKind call_kind =
      instruction->IsClassFinal() ? LocationSummary::kNoCall : LocationSummary::kCallOnSlowPath;
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction, call_kind);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  // The output does overlap inputs.
  locations->SetOut(Location::RequiresRegister(), Location::kOutputOverlap);
}

void InstructionCodeGeneratorMIPS64::VisitInstanceOf(HInstanceOf* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  GpuRegister obj = locations->InAt(0).AsRegister<GpuRegister>();
  GpuRegister cls = locations->InAt(1).AsRegister<GpuRegister>();
  GpuRegister out = locations->Out().AsRegister<GpuRegister>();

  Label done;

  // Return 0 if `obj` is null.
  // TODO: Avoid this check if we know `obj` is not null.
  __ Move(out, ZERO);
  __ Beqzc(obj, &done);

  // Compare the class of `obj` with `cls`.
  __ LoadFromOffset(kLoadUnsignedWord, out, obj, mirror::Object::ClassOffset().Int32Value());
  if (instruction->IsClassFinal()) {
    // Classes must be equal for the instanceof to succeed.
    __ Xor(out, out, cls);
    __ Sltiu(out, out, 1);
  } else {
    // If the classes are not equal, we go into a slow path.
    DCHECK(locations->OnlyCallsOnSlowPath());
    SlowPathCodeMIPS64* slow_path =
        new (GetGraph()->GetArena()) TypeCheckSlowPathMIPS64(instruction,
                                                             locations->InAt(1),
                                                             locations->Out(),
                                                             instruction->GetDexPc());
    codegen_->AddSlowPath(slow_path);
    __ Bnec(out, cls, slow_path->GetEntryLabel());
    __ LoadConst32(out, 1);
    __ Bind(slow_path->GetExitLabel());
  }

  __ Bind(&done);
}

void LocationsBuilderMIPS64::VisitIntConstant(HIntConstant* constant) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(constant);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorMIPS64::VisitIntConstant(HIntConstant* constant ATTRIBUTE_UNUSED) {
  // Will be generated at use site.
}

void LocationsBuilderMIPS64::VisitNullConstant(HNullConstant* constant) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(constant);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorMIPS64::VisitNullConstant(HNullConstant* constant ATTRIBUTE_UNUSED) {
  // Will be generated at use site.
}

void LocationsBuilderMIPS64::HandleInvoke(HInvoke* invoke) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(invoke, LocationSummary::kCall);
  locations->AddTemp(Location::RegisterLocation(kMethodRegisterArgument));

  InvokeDexCallingConventionVisitorMIPS64 calling_convention_visitor;
  for (size_t i = 0; i < invoke->GetNumberOfArguments(); i++) {
    HInstruction* input = invoke->InputAt(i);
    locations->SetInAt(i, calling_convention_visitor.GetNextLocation(input->GetType()));
  }

  Primitive::Type return_type = invoke->GetType();
  if (return_type != Primitive::kPrimVoid) {
    locations->SetOut(calling_convention_visitor.GetReturnLocation(return_type));
  }
}

void LocationsBuilderMIPS64::VisitInvokeInterface(HInvokeInterface* invoke) {
  HandleInvoke(invoke);
  // The register T0 is required to be used for the hidden argument in
  // art_quick_imt_conflict_trampoline, so add the hidden argument.
  invoke->GetLocations()->AddTemp(Location::RegisterLocation(T0));
}

void InstructionCodeGeneratorMIPS64::VisitInvokeInterface(HInvokeInterface* invoke) {
  // TODO: b/18116999, our IMTs can miss an IncompatibleClassChangeError.
  GpuRegister temp = invoke->GetLocations()->GetTemp(0).AsRegister<GpuRegister>();
  uint32_t method_offset = mirror::Class::EmbeddedImTableEntryOffset(
      invoke->GetImtIndex() % mirror::Class::kImtSize, kMips64PointerSize).Uint32Value();
  Location receiver = invoke->GetLocations()->InAt(0);
  uint32_t class_offset = mirror::Object::ClassOffset().Int32Value();
  Offset entry_point = ArtMethod::EntryPointFromQuickCompiledCodeOffset(kMips64WordSize);

  // Set the hidden argument.
  __ LoadConst32(invoke->GetLocations()->GetTemp(1).AsRegister<GpuRegister>(),
                 invoke->GetDexMethodIndex());

  // temp = object->GetClass();
  if (receiver.IsStackSlot()) {
    __ LoadFromOffset(kLoadUnsignedWord, temp, SP, receiver.GetStackIndex());
    __ LoadFromOffset(kLoadUnsignedWord, temp, temp, class_offset);
  } else {
    __ LoadFromOffset(kLoadUnsignedWord, temp, receiver.AsRegister<GpuRegister>(), class_offset);
  }
  codegen_->MaybeRecordImplicitNullCheck(invoke);
  // temp = temp->GetImtEntryAt(method_offset);
  __ LoadFromOffset(kLoadDoubleword, temp, temp, method_offset);
  // T9 = temp->GetEntryPoint();
  __ LoadFromOffset(kLoadDoubleword, T9, temp, entry_point.Int32Value());
  // T9();
  __ Jalr(T9);
  DCHECK(!codegen_->IsLeafMethod());
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
}

void LocationsBuilderMIPS64::VisitInvokeVirtual(HInvokeVirtual* invoke) {
  // TODO intrinsic function
  HandleInvoke(invoke);
}

void LocationsBuilderMIPS64::VisitInvokeStaticOrDirect(HInvokeStaticOrDirect* invoke) {
  // When we do not run baseline, explicit clinit checks triggered by static
  // invokes must have been pruned by art::PrepareForRegisterAllocation.
  DCHECK(codegen_->IsBaseline() || !invoke->IsStaticWithExplicitClinitCheck());

  // TODO - intrinsic function
  HandleInvoke(invoke);
}

static bool TryGenerateIntrinsicCode(HInvoke* invoke,
                                     CodeGeneratorMIPS64* codegen ATTRIBUTE_UNUSED) {
  if (invoke->GetLocations()->Intrinsified()) {
    // TODO - intrinsic function
    return true;
  }
  return false;
}

void CodeGeneratorMIPS64::GenerateStaticOrDirectCall(HInvokeStaticOrDirect* invoke,
                                                     GpuRegister temp) {
  // All registers are assumed to be correctly set up per the calling convention.

  // TODO: Implement all kinds of calls:
  // 1) boot -> boot
  // 2) app -> boot
  // 3) app -> app
  //
  // Currently we implement the app -> app logic, which looks up in the resolve cache.

  if (invoke->IsStringInit()) {
    // temp = thread->string_init_entrypoint
    __ LoadFromOffset(kLoadDoubleword,
                      temp,
                      TR,
                      invoke->GetStringInitOffset());
    // T9 = temp->entry_point_from_quick_compiled_code_;
    __ LoadFromOffset(kLoadDoubleword,
                      T9,
                      temp,
                      ArtMethod::EntryPointFromQuickCompiledCodeOffset(
                          kMips64WordSize).Int32Value());
    // T9()
    __ Jalr(T9);
  } else {
    // temp = method;
    LoadCurrentMethod(temp);
    if (!invoke->IsRecursive()) {
      // temp = temp->dex_cache_resolved_methods_;
      __ LoadFromOffset(kLoadUnsignedWord,
                        temp,
                        temp,
                        ArtMethod::DexCacheResolvedMethodsOffset().Int32Value());
      // temp = temp[index_in_cache]
      __ LoadFromOffset(kLoadDoubleword,
                        temp,
                        temp,
                        CodeGenerator::GetCachePointerOffset(invoke->GetDexMethodIndex()));
      // T9 = temp[offset_of_quick_compiled_code]
      __ LoadFromOffset(kLoadDoubleword,
                        T9,
                        temp,
                        ArtMethod::EntryPointFromQuickCompiledCodeOffset(
                            kMips64WordSize).Int32Value());
      // T9()
      __ Jalr(T9);
    } else {
      __ Jalr(&frame_entry_label_, T9);
    }
  }

  DCHECK(!IsLeafMethod());
}

void InstructionCodeGeneratorMIPS64::VisitInvokeStaticOrDirect(HInvokeStaticOrDirect* invoke) {
  // When we do not run baseline, explicit clinit checks triggered by static
  // invokes must have been pruned by art::PrepareForRegisterAllocation.
  DCHECK(codegen_->IsBaseline() || !invoke->IsStaticWithExplicitClinitCheck());

  if (TryGenerateIntrinsicCode(invoke, codegen_)) {
    return;
  }

  GpuRegister temp = invoke->GetLocations()->GetTemp(0).AsRegister<GpuRegister>();

  codegen_->GenerateStaticOrDirectCall(invoke, temp);
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
}

void InstructionCodeGeneratorMIPS64::VisitInvokeVirtual(HInvokeVirtual* invoke) {
  // TODO: Try to generate intrinsics code.
  LocationSummary* locations = invoke->GetLocations();
  Location receiver = locations->InAt(0);
  GpuRegister temp = invoke->GetLocations()->GetTemp(0).AsRegister<GpuRegister>();
  size_t method_offset = mirror::Class::EmbeddedVTableEntryOffset(
      invoke->GetVTableIndex(), kMips64PointerSize).SizeValue();
  uint32_t class_offset = mirror::Object::ClassOffset().Int32Value();
  Offset entry_point = ArtMethod::EntryPointFromQuickCompiledCodeOffset(kMips64WordSize);

  // temp = object->GetClass();
  if (receiver.IsStackSlot()) {
    __ LoadFromOffset(kLoadUnsignedWord, temp, SP, receiver.GetStackIndex());
    __ LoadFromOffset(kLoadUnsignedWord, temp, temp, class_offset);
  } else {
    DCHECK(receiver.IsRegister());
    __ LoadFromOffset(kLoadUnsignedWord, temp, receiver.AsRegister<GpuRegister>(), class_offset);
  }
  codegen_->MaybeRecordImplicitNullCheck(invoke);
  // temp = temp->GetMethodAt(method_offset);
  __ LoadFromOffset(kLoadDoubleword, temp, temp, method_offset);
  // T9 = temp->GetEntryPoint();
  __ LoadFromOffset(kLoadDoubleword, T9, temp, entry_point.Int32Value());
  // T9();
  __ Jalr(T9);
  DCHECK(!codegen_->IsLeafMethod());
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
}

void LocationsBuilderMIPS64::VisitLoadClass(HLoadClass* cls) {
  LocationSummary::CallKind call_kind = cls->CanCallRuntime() ? LocationSummary::kCallOnSlowPath
                                                              : LocationSummary::kNoCall;
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(cls, call_kind);
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorMIPS64::VisitLoadClass(HLoadClass* cls) {
  GpuRegister out = cls->GetLocations()->Out().AsRegister<GpuRegister>();
  if (cls->IsReferrersClass()) {
    DCHECK(!cls->CanCallRuntime());
    DCHECK(!cls->MustGenerateClinitCheck());
    codegen_->LoadCurrentMethod(out);
    __ LoadFromOffset(
        kLoadUnsignedWord, out, out, ArtMethod::DeclaringClassOffset().Int32Value());
  } else {
    DCHECK(cls->CanCallRuntime());
    codegen_->LoadCurrentMethod(out);
    __ LoadFromOffset(
        kLoadUnsignedWord, out, out, ArtMethod::DexCacheResolvedTypesOffset().Int32Value());
    __ LoadFromOffset(kLoadUnsignedWord, out, out, CodeGenerator::GetCacheOffset(cls->GetTypeIndex()));
    SlowPathCodeMIPS64* slow_path = new (GetGraph()->GetArena()) LoadClassSlowPathMIPS64(
        cls,
        cls,
        cls->GetDexPc(),
        cls->MustGenerateClinitCheck());
    codegen_->AddSlowPath(slow_path);
    __ Beqzc(out, slow_path->GetEntryLabel());
    if (cls->MustGenerateClinitCheck()) {
      GenerateClassInitializationCheck(slow_path, out);
    } else {
      __ Bind(slow_path->GetExitLabel());
    }
  }
}

void LocationsBuilderMIPS64::VisitLoadException(HLoadException* load) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(load, LocationSummary::kNoCall);
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorMIPS64::VisitLoadException(HLoadException* load) {
  GpuRegister out = load->GetLocations()->Out().AsRegister<GpuRegister>();
  __ LoadFromOffset(kLoadUnsignedWord, out, TR, Thread::ExceptionOffset<kMips64WordSize>().Int32Value());
  __ StoreToOffset(kStoreWord, ZERO, TR, Thread::ExceptionOffset<kMips64WordSize>().Int32Value());
}

void LocationsBuilderMIPS64::VisitLoadLocal(HLoadLocal* load) {
  load->SetLocations(nullptr);
}

void InstructionCodeGeneratorMIPS64::VisitLoadLocal(HLoadLocal* load ATTRIBUTE_UNUSED) {
  // Nothing to do, this is driven by the code generator.
}

void LocationsBuilderMIPS64::VisitLoadString(HLoadString* load) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(load, LocationSummary::kCallOnSlowPath);
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorMIPS64::VisitLoadString(HLoadString* load) {
  SlowPathCodeMIPS64* slow_path = new (GetGraph()->GetArena()) LoadStringSlowPathMIPS64(load);
  codegen_->AddSlowPath(slow_path);

  GpuRegister out = load->GetLocations()->Out().AsRegister<GpuRegister>();
  codegen_->LoadCurrentMethod(out);
  __ LoadFromOffset(
      kLoadUnsignedWord, out, out, ArtMethod::DeclaringClassOffset().Int32Value());
  __ LoadFromOffset(kLoadUnsignedWord, out, out, mirror::Class::DexCacheStringsOffset().Int32Value());
  __ LoadFromOffset(kLoadUnsignedWord, out, out, CodeGenerator::GetCacheOffset(load->GetStringIndex()));
  __ Beqzc(out, slow_path->GetEntryLabel());
  __ Bind(slow_path->GetExitLabel());
}

void LocationsBuilderMIPS64::VisitLocal(HLocal* local) {
  local->SetLocations(nullptr);
}

void InstructionCodeGeneratorMIPS64::VisitLocal(HLocal* local) {
  DCHECK_EQ(local->GetBlock(), GetGraph()->GetEntryBlock());
}

void LocationsBuilderMIPS64::VisitLongConstant(HLongConstant* constant) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(constant);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorMIPS64::VisitLongConstant(HLongConstant* constant ATTRIBUTE_UNUSED) {
  // Will be generated at use site.
}

void LocationsBuilderMIPS64::VisitMonitorOperation(HMonitorOperation* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCall);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
}

void InstructionCodeGeneratorMIPS64::VisitMonitorOperation(HMonitorOperation* instruction) {
  codegen_->InvokeRuntime(instruction->IsEnter()
                              ? QUICK_ENTRY_POINT(pLockObject)
                              : QUICK_ENTRY_POINT(pUnlockObject),
                          instruction,
                          instruction->GetDexPc(),
                          nullptr);
  CheckEntrypointTypes<kQuickLockObject, void, mirror::Object*>();
}

void LocationsBuilderMIPS64::VisitMul(HMul* mul) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(mul, LocationSummary::kNoCall);
  switch (mul->GetResultType()) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong:
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RequiresRegister());
      locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      break;

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetInAt(1, Location::RequiresFpuRegister());
      locations->SetOut(Location::RequiresFpuRegister(), Location::kNoOutputOverlap);
      break;

    default:
      LOG(FATAL) << "Unexpected mul type " << mul->GetResultType();
  }
}

void InstructionCodeGeneratorMIPS64::VisitMul(HMul* instruction) {
  Primitive::Type type = instruction->GetType();
  LocationSummary* locations = instruction->GetLocations();

  switch (type) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong: {
      GpuRegister dst = locations->Out().AsRegister<GpuRegister>();
      GpuRegister lhs = locations->InAt(0).AsRegister<GpuRegister>();
      GpuRegister rhs = locations->InAt(1).AsRegister<GpuRegister>();
      if (type == Primitive::kPrimInt)
        __ MulR6(dst, lhs, rhs);
      else
        __ Dmul(dst, lhs, rhs);
      break;
    }
    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      FpuRegister dst = locations->Out().AsFpuRegister<FpuRegister>();
      FpuRegister lhs = locations->InAt(0).AsFpuRegister<FpuRegister>();
      FpuRegister rhs = locations->InAt(1).AsFpuRegister<FpuRegister>();
      if (type == Primitive::kPrimFloat)
        __ MulS(dst, lhs, rhs);
      else
        __ MulD(dst, lhs, rhs);
      break;
    }
    default:
      LOG(FATAL) << "Unexpected mul type " << type;
  }
}

void LocationsBuilderMIPS64::VisitNeg(HNeg* neg) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(neg, LocationSummary::kNoCall);
  switch (neg->GetResultType()) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong:
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      break;

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetOut(Location::RequiresFpuRegister(), Location::kNoOutputOverlap);
      break;

    default:
      LOG(FATAL) << "Unexpected neg type " << neg->GetResultType();
  }
}

void InstructionCodeGeneratorMIPS64::VisitNeg(HNeg* instruction) {
  Primitive::Type type = instruction->GetType();
  LocationSummary* locations = instruction->GetLocations();

  switch (type) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong: {
      GpuRegister dst = locations->Out().AsRegister<GpuRegister>();
      GpuRegister src = locations->InAt(0).AsRegister<GpuRegister>();
      if (type == Primitive::kPrimInt)
        __ Subu(dst, ZERO, src);
      else
        __ Dsubu(dst, ZERO, src);
      break;
    }
    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      FpuRegister dst = locations->Out().AsFpuRegister<FpuRegister>();
      FpuRegister src = locations->InAt(0).AsFpuRegister<FpuRegister>();
      if (type == Primitive::kPrimFloat)
        __ NegS(dst, src);
      else
        __ NegD(dst, src);
      break;
    }
    default:
      LOG(FATAL) << "Unexpected neg type " << type;
  }
}

void LocationsBuilderMIPS64::VisitNewArray(HNewArray* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCall);
  InvokeRuntimeCallingConvention calling_convention;
  locations->AddTemp(Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
  locations->AddTemp(Location::RegisterLocation(calling_convention.GetRegisterAt(2)));
  locations->SetOut(calling_convention.GetReturnLocation(Primitive::kPrimNot));
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
}

void InstructionCodeGeneratorMIPS64::VisitNewArray(HNewArray* instruction) {
  InvokeRuntimeCallingConvention calling_convention;
  codegen_->LoadCurrentMethod(calling_convention.GetRegisterAt(2));
  // Move an uint16_t value to a register.
  __ LoadConst32(calling_convention.GetRegisterAt(0), instruction->GetTypeIndex());
  codegen_->InvokeRuntime(
      GetThreadOffset<kMips64WordSize>(instruction->GetEntrypoint()).Int32Value(),
      instruction,
      instruction->GetDexPc(),
      nullptr);
  CheckEntrypointTypes<kQuickAllocArrayWithAccessCheck, void*, uint32_t, int32_t, ArtMethod*>();
}

void LocationsBuilderMIPS64::VisitNewInstance(HNewInstance* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCall);
  InvokeRuntimeCallingConvention calling_convention;
  locations->AddTemp(Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
  locations->AddTemp(Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
  locations->SetOut(calling_convention.GetReturnLocation(Primitive::kPrimNot));
}

void InstructionCodeGeneratorMIPS64::VisitNewInstance(HNewInstance* instruction) {
  InvokeRuntimeCallingConvention calling_convention;
  codegen_->LoadCurrentMethod(calling_convention.GetRegisterAt(1));
  // Move an uint16_t value to a register.
  __ LoadConst32(calling_convention.GetRegisterAt(0), instruction->GetTypeIndex());
  codegen_->InvokeRuntime(
      GetThreadOffset<kMips64WordSize>(instruction->GetEntrypoint()).Int32Value(),
      instruction,
      instruction->GetDexPc(),
      nullptr);
  CheckEntrypointTypes<kQuickAllocObjectWithAccessCheck, void*, uint32_t, ArtMethod*>();
}

void LocationsBuilderMIPS64::VisitNot(HNot* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

void InstructionCodeGeneratorMIPS64::VisitNot(HNot* instruction) {
  Primitive::Type type = instruction->GetType();
  LocationSummary* locations = instruction->GetLocations();

  switch (type) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong: {
      GpuRegister dst = locations->Out().AsRegister<GpuRegister>();
      GpuRegister src = locations->InAt(0).AsRegister<GpuRegister>();
      __ Nor(dst, src, ZERO);
      break;
    }

    default:
      LOG(FATAL) << "Unexpected type for not operation " << instruction->GetResultType();
  }
}

void LocationsBuilderMIPS64::VisitBooleanNot(HBooleanNot* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

void InstructionCodeGeneratorMIPS64::VisitBooleanNot(HBooleanNot* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  __ Xori(locations->Out().AsRegister<GpuRegister>(),
          locations->InAt(0).AsRegister<GpuRegister>(),
          1);
}

void LocationsBuilderMIPS64::VisitNullCheck(HNullCheck* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  if (instruction->HasUses()) {
    locations->SetOut(Location::SameAsFirstInput());
  }
}

void InstructionCodeGeneratorMIPS64::GenerateImplicitNullCheck(HNullCheck* instruction) {
  if (codegen_->CanMoveNullCheckToUser(instruction)) {
    return;
  }
  Location obj = instruction->GetLocations()->InAt(0);

  __ Lw(ZERO, obj.AsRegister<GpuRegister>(), 0);
  codegen_->RecordPcInfo(instruction, instruction->GetDexPc());
}

void InstructionCodeGeneratorMIPS64::GenerateExplicitNullCheck(HNullCheck* instruction) {
  SlowPathCodeMIPS64* slow_path = new (GetGraph()->GetArena()) NullCheckSlowPathMIPS64(instruction);
  codegen_->AddSlowPath(slow_path);

  Location obj = instruction->GetLocations()->InAt(0);

  __ Beqzc(obj.AsRegister<GpuRegister>(), slow_path->GetEntryLabel());
}

void InstructionCodeGeneratorMIPS64::VisitNullCheck(HNullCheck* instruction) {
  if (codegen_->GetCompilerOptions().GetImplicitNullChecks()) {
    GenerateImplicitNullCheck(instruction);
  } else {
    GenerateExplicitNullCheck(instruction);
  }
}

void LocationsBuilderMIPS64::VisitOr(HOr* instruction) {
  HandleBinaryOp(instruction);
}

void InstructionCodeGeneratorMIPS64::VisitOr(HOr* instruction) {
  HandleBinaryOp(instruction);
}

void LocationsBuilderMIPS64::VisitParallelMove(HParallelMove* instruction ATTRIBUTE_UNUSED) {
  LOG(FATAL) << "Unreachable";
}

void InstructionCodeGeneratorMIPS64::VisitParallelMove(HParallelMove* instruction) {
  codegen_->GetMoveResolver()->EmitNativeCode(instruction);
}

void LocationsBuilderMIPS64::VisitParameterValue(HParameterValue* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  Location location = parameter_visitor_.GetNextLocation(instruction->GetType());
  if (location.IsStackSlot()) {
    location = Location::StackSlot(location.GetStackIndex() + codegen_->GetFrameSize());
  } else if (location.IsDoubleStackSlot()) {
    location = Location::DoubleStackSlot(location.GetStackIndex() + codegen_->GetFrameSize());
  }
  locations->SetOut(location);
}

void InstructionCodeGeneratorMIPS64::VisitParameterValue(HParameterValue* instruction
                                                         ATTRIBUTE_UNUSED) {
  // Nothing to do, the parameter is already at its location.
}

void LocationsBuilderMIPS64::VisitPhi(HPhi* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  for (size_t i = 0, e = instruction->InputCount(); i < e; ++i) {
    locations->SetInAt(i, Location::Any());
  }
  locations->SetOut(Location::Any());
}

void InstructionCodeGeneratorMIPS64::VisitPhi(HPhi* instruction ATTRIBUTE_UNUSED) {
  LOG(FATAL) << "Unreachable";
}

void LocationsBuilderMIPS64::VisitRem(HRem* rem) {
  Primitive::Type type = rem->GetResultType();
  LocationSummary::CallKind call_kind =
      Primitive::IsFloatingPointType(type) ? LocationSummary::kCall : LocationSummary::kNoCall;
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(rem, call_kind);

  switch (type) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong:
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RequiresRegister());
      locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      break;

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      InvokeRuntimeCallingConvention calling_convention;
      locations->SetInAt(0, Location::FpuRegisterLocation(calling_convention.GetFpuRegisterAt(0)));
      locations->SetInAt(1, Location::FpuRegisterLocation(calling_convention.GetFpuRegisterAt(1)));
      locations->SetOut(calling_convention.GetReturnLocation(type));
      break;
    }

    default:
      LOG(FATAL) << "Unexpected rem type " << type;
  }
}

void InstructionCodeGeneratorMIPS64::VisitRem(HRem* instruction) {
  Primitive::Type type = instruction->GetType();
  LocationSummary* locations = instruction->GetLocations();

  switch (type) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong: {
      GpuRegister dst = locations->Out().AsRegister<GpuRegister>();
      GpuRegister lhs = locations->InAt(0).AsRegister<GpuRegister>();
      GpuRegister rhs = locations->InAt(1).AsRegister<GpuRegister>();
      if (type == Primitive::kPrimInt)
        __ ModR6(dst, lhs, rhs);
      else
        __ Dmod(dst, lhs, rhs);
      break;
    }

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      int32_t entry_offset = (type == Primitive::kPrimFloat) ? QUICK_ENTRY_POINT(pFmodf)
                                                             : QUICK_ENTRY_POINT(pFmod);
      codegen_->InvokeRuntime(entry_offset, instruction, instruction->GetDexPc(), nullptr);
      break;
    }
    default:
      LOG(FATAL) << "Unexpected rem type " << type;
  }
}

void LocationsBuilderMIPS64::VisitMemoryBarrier(HMemoryBarrier* memory_barrier) {
  memory_barrier->SetLocations(nullptr);
}

void InstructionCodeGeneratorMIPS64::VisitMemoryBarrier(HMemoryBarrier* memory_barrier) {
  GenerateMemoryBarrier(memory_barrier->GetBarrierKind());
}

void LocationsBuilderMIPS64::VisitReturn(HReturn* ret) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(ret);
  Primitive::Type return_type = ret->InputAt(0)->GetType();
  locations->SetInAt(0, Mips64ReturnLocation(return_type));
}

void InstructionCodeGeneratorMIPS64::VisitReturn(HReturn* ret ATTRIBUTE_UNUSED) {
  codegen_->GenerateFrameExit();
}

void LocationsBuilderMIPS64::VisitReturnVoid(HReturnVoid* ret) {
  ret->SetLocations(nullptr);
}

void InstructionCodeGeneratorMIPS64::VisitReturnVoid(HReturnVoid* ret ATTRIBUTE_UNUSED) {
  codegen_->GenerateFrameExit();
}

void LocationsBuilderMIPS64::VisitShl(HShl* shl) {
  HandleShift(shl);
}

void InstructionCodeGeneratorMIPS64::VisitShl(HShl* shl) {
  HandleShift(shl);
}

void LocationsBuilderMIPS64::VisitShr(HShr* shr) {
  HandleShift(shr);
}

void InstructionCodeGeneratorMIPS64::VisitShr(HShr* shr) {
  HandleShift(shr);
}

void LocationsBuilderMIPS64::VisitStoreLocal(HStoreLocal* store) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(store);
  Primitive::Type field_type = store->InputAt(1)->GetType();
  switch (field_type) {
    case Primitive::kPrimNot:
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimInt:
    case Primitive::kPrimFloat:
      locations->SetInAt(1, Location::StackSlot(codegen_->GetStackSlot(store->GetLocal())));
      break;

    case Primitive::kPrimLong:
    case Primitive::kPrimDouble:
      locations->SetInAt(1, Location::DoubleStackSlot(codegen_->GetStackSlot(store->GetLocal())));
      break;

    default:
      LOG(FATAL) << "Unimplemented local type " << field_type;
  }
}

void InstructionCodeGeneratorMIPS64::VisitStoreLocal(HStoreLocal* store ATTRIBUTE_UNUSED) {
}

void LocationsBuilderMIPS64::VisitSub(HSub* instruction) {
  HandleBinaryOp(instruction);
}

void InstructionCodeGeneratorMIPS64::VisitSub(HSub* instruction) {
  HandleBinaryOp(instruction);
}

void LocationsBuilderMIPS64::VisitStaticFieldGet(HStaticFieldGet* instruction) {
  HandleFieldGet(instruction, instruction->GetFieldInfo());
}

void InstructionCodeGeneratorMIPS64::VisitStaticFieldGet(HStaticFieldGet* instruction) {
  HandleFieldGet(instruction, instruction->GetFieldInfo());
}

void LocationsBuilderMIPS64::VisitStaticFieldSet(HStaticFieldSet* instruction) {
  HandleFieldSet(instruction, instruction->GetFieldInfo());
}

void InstructionCodeGeneratorMIPS64::VisitStaticFieldSet(HStaticFieldSet* instruction) {
  HandleFieldSet(instruction, instruction->GetFieldInfo());
}

void LocationsBuilderMIPS64::VisitSuspendCheck(HSuspendCheck* instruction) {
  new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCallOnSlowPath);
}

void InstructionCodeGeneratorMIPS64::VisitSuspendCheck(HSuspendCheck* instruction) {
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

void LocationsBuilderMIPS64::VisitTemporary(HTemporary* temp) {
  temp->SetLocations(nullptr);
}

void InstructionCodeGeneratorMIPS64::VisitTemporary(HTemporary* temp ATTRIBUTE_UNUSED) {
  // Nothing to do, this is driven by the code generator.
}

void LocationsBuilderMIPS64::VisitThrow(HThrow* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCall);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
}

void InstructionCodeGeneratorMIPS64::VisitThrow(HThrow* instruction) {
  codegen_->InvokeRuntime(QUICK_ENTRY_POINT(pDeliverException),
                          instruction,
                          instruction->GetDexPc(),
                          nullptr);
  CheckEntrypointTypes<kQuickDeliverException, void, mirror::Object*>();
}

void LocationsBuilderMIPS64::VisitTypeConversion(HTypeConversion* conversion) {
  Primitive::Type input_type = conversion->GetInputType();
  Primitive::Type result_type = conversion->GetResultType();
  DCHECK_NE(input_type, result_type);

  if ((input_type == Primitive::kPrimNot) || (input_type == Primitive::kPrimVoid) ||
      (result_type == Primitive::kPrimNot) || (result_type == Primitive::kPrimVoid)) {
    LOG(FATAL) << "Unexpected type conversion from " << input_type << " to " << result_type;
  }

  LocationSummary::CallKind call_kind = LocationSummary::kNoCall;
  if ((Primitive::IsFloatingPointType(result_type) && input_type == Primitive::kPrimLong) ||
      (Primitive::IsIntegralType(result_type) && Primitive::IsFloatingPointType(input_type))) {
    call_kind = LocationSummary::kCall;
  }

  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(conversion, call_kind);

  if (call_kind == LocationSummary::kNoCall) {
    if (Primitive::IsFloatingPointType(input_type)) {
      locations->SetInAt(0, Location::RequiresFpuRegister());
    } else {
      locations->SetInAt(0, Location::RequiresRegister());
    }

    if (Primitive::IsFloatingPointType(result_type)) {
      locations->SetOut(Location::RequiresFpuRegister(), Location::kNoOutputOverlap);
    } else {
      locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
    }
  } else {
    InvokeRuntimeCallingConvention calling_convention;

    if (Primitive::IsFloatingPointType(input_type)) {
      locations->SetInAt(0, Location::FpuRegisterLocation(calling_convention.GetFpuRegisterAt(0)));
    } else {
      locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
    }

    locations->SetOut(calling_convention.GetReturnLocation(result_type));
  }
}

void InstructionCodeGeneratorMIPS64::VisitTypeConversion(HTypeConversion* conversion) {
  LocationSummary* locations = conversion->GetLocations();
  Primitive::Type result_type = conversion->GetResultType();
  Primitive::Type input_type = conversion->GetInputType();

  DCHECK_NE(input_type, result_type);

  if (Primitive::IsIntegralType(result_type) && Primitive::IsIntegralType(input_type)) {
    GpuRegister dst = locations->Out().AsRegister<GpuRegister>();
    GpuRegister src = locations->InAt(0).AsRegister<GpuRegister>();

    switch (result_type) {
      case Primitive::kPrimChar:
        __ Andi(dst, src, 0xFFFF);
        break;
      case Primitive::kPrimByte:
        // long is never converted into types narrower than int directly,
        // so SEB and SEH can be used without ever causing unpredictable results
        // on 64-bit inputs
        DCHECK(input_type != Primitive::kPrimLong);
        __ Seb(dst, src);
        break;
      case Primitive::kPrimShort:
        // long is never converted into types narrower than int directly,
        // so SEB and SEH can be used without ever causing unpredictable results
        // on 64-bit inputs
        DCHECK(input_type != Primitive::kPrimLong);
        __ Seh(dst, src);
        break;
      case Primitive::kPrimInt:
      case Primitive::kPrimLong:
        // Sign-extend 32-bit int into bits 32 through 63 for
        // int-to-long and long-to-int conversions
        __ Sll(dst, src, 0);
        break;

      default:
        LOG(FATAL) << "Unexpected type conversion from " << input_type
                   << " to " << result_type;
    }
  } else if (Primitive::IsFloatingPointType(result_type) && Primitive::IsIntegralType(input_type)) {
    if (input_type != Primitive::kPrimLong) {
      FpuRegister dst = locations->Out().AsFpuRegister<FpuRegister>();
      GpuRegister src = locations->InAt(0).AsRegister<GpuRegister>();
      __ Mtc1(src, FTMP);
      if (result_type == Primitive::kPrimFloat) {
        __ Cvtsw(dst, FTMP);
      } else {
        __ Cvtdw(dst, FTMP);
      }
    } else {
      int32_t entry_offset = (result_type == Primitive::kPrimFloat) ? QUICK_ENTRY_POINT(pL2f)
                                                                    : QUICK_ENTRY_POINT(pL2d);
      codegen_->InvokeRuntime(entry_offset,
                              conversion,
                              conversion->GetDexPc(),
                              nullptr);
    }
  } else if (Primitive::IsIntegralType(result_type) && Primitive::IsFloatingPointType(input_type)) {
    CHECK(result_type == Primitive::kPrimInt || result_type == Primitive::kPrimLong);
    int32_t entry_offset;
    if (result_type != Primitive::kPrimLong) {
      entry_offset = (input_type == Primitive::kPrimFloat) ? QUICK_ENTRY_POINT(pF2iz)
                                                           : QUICK_ENTRY_POINT(pD2iz);
    } else {
      entry_offset = (input_type == Primitive::kPrimFloat) ? QUICK_ENTRY_POINT(pF2l)
                                                           : QUICK_ENTRY_POINT(pD2l);
    }
    codegen_->InvokeRuntime(entry_offset,
                            conversion,
                            conversion->GetDexPc(),
                            nullptr);
  } else if (Primitive::IsFloatingPointType(result_type) &&
             Primitive::IsFloatingPointType(input_type)) {
    FpuRegister dst = locations->Out().AsFpuRegister<FpuRegister>();
    FpuRegister src = locations->InAt(0).AsFpuRegister<FpuRegister>();
    if (result_type == Primitive::kPrimFloat) {
      __ Cvtsd(dst, src);
    } else {
      __ Cvtds(dst, src);
    }
  } else {
    LOG(FATAL) << "Unexpected or unimplemented type conversion from " << input_type
                << " to " << result_type;
  }
}

void LocationsBuilderMIPS64::VisitUShr(HUShr* ushr) {
  HandleShift(ushr);
}

void InstructionCodeGeneratorMIPS64::VisitUShr(HUShr* ushr) {
  HandleShift(ushr);
}

void LocationsBuilderMIPS64::VisitXor(HXor* instruction) {
  HandleBinaryOp(instruction);
}

void InstructionCodeGeneratorMIPS64::VisitXor(HXor* instruction) {
  HandleBinaryOp(instruction);
}

void LocationsBuilderMIPS64::VisitBoundType(HBoundType* instruction ATTRIBUTE_UNUSED) {
  // Nothing to do, this should be removed during prepare for register allocator.
  LOG(FATAL) << "Unreachable";
}

void InstructionCodeGeneratorMIPS64::VisitBoundType(HBoundType* instruction ATTRIBUTE_UNUSED) {
  // Nothing to do, this should be removed during prepare for register allocator.
  LOG(FATAL) << "Unreachable";
}

void LocationsBuilderMIPS64::VisitEqual(HEqual* comp) {
  VisitCondition(comp);
}

void InstructionCodeGeneratorMIPS64::VisitEqual(HEqual* comp) {
  VisitCondition(comp);
}

void LocationsBuilderMIPS64::VisitNotEqual(HNotEqual* comp) {
  VisitCondition(comp);
}

void InstructionCodeGeneratorMIPS64::VisitNotEqual(HNotEqual* comp) {
  VisitCondition(comp);
}

void LocationsBuilderMIPS64::VisitLessThan(HLessThan* comp) {
  VisitCondition(comp);
}

void InstructionCodeGeneratorMIPS64::VisitLessThan(HLessThan* comp) {
  VisitCondition(comp);
}

void LocationsBuilderMIPS64::VisitLessThanOrEqual(HLessThanOrEqual* comp) {
  VisitCondition(comp);
}

void InstructionCodeGeneratorMIPS64::VisitLessThanOrEqual(HLessThanOrEqual* comp) {
  VisitCondition(comp);
}

void LocationsBuilderMIPS64::VisitGreaterThan(HGreaterThan* comp) {
  VisitCondition(comp);
}

void InstructionCodeGeneratorMIPS64::VisitGreaterThan(HGreaterThan* comp) {
  VisitCondition(comp);
}

void LocationsBuilderMIPS64::VisitGreaterThanOrEqual(HGreaterThanOrEqual* comp) {
  VisitCondition(comp);
}

void InstructionCodeGeneratorMIPS64::VisitGreaterThanOrEqual(HGreaterThanOrEqual* comp) {
  VisitCondition(comp);
}

}  // namespace mips64
}  // namespace art
