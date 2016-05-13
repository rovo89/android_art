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

#include "art_method.h"
#include "code_generator_utils.h"
#include "entrypoints/quick/quick_entrypoints.h"
#include "entrypoints/quick/quick_entrypoints_enum.h"
#include "gc/accounting/card_table.h"
#include "intrinsics.h"
#include "intrinsics_mips64.h"
#include "mirror/array-inl.h"
#include "mirror/class-inl.h"
#include "offsets.h"
#include "thread.h"
#include "utils/assembler.h"
#include "utils/mips64/assembler_mips64.h"
#include "utils/stack_checks.h"

namespace art {
namespace mips64 {

static constexpr int kCurrentMethodStackOffset = 0;
static constexpr GpuRegister kMethodRegisterArgument = A0;

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

Location InvokeDexCallingConventionVisitorMIPS64::GetMethodLocation() const {
  return Location::RegisterLocation(kMethodRegisterArgument);
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
#define QUICK_ENTRY_POINT(x) QUICK_ENTRYPOINT_OFFSET(kMips64DoublewordSize, x).Int32Value()

class BoundsCheckSlowPathMIPS64 : public SlowPathCodeMIPS64 {
 public:
  explicit BoundsCheckSlowPathMIPS64(HBoundsCheck* instruction) : SlowPathCodeMIPS64(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    LocationSummary* locations = instruction_->GetLocations();
    CodeGeneratorMIPS64* mips64_codegen = down_cast<CodeGeneratorMIPS64*>(codegen);
    __ Bind(GetEntryLabel());
    if (instruction_->CanThrowIntoCatchBlock()) {
      // Live registers will be restored in the catch block if caught.
      SaveLiveRegisters(codegen, instruction_->GetLocations());
    }
    // We're moving two locations to locations that could overlap, so we need a parallel
    // move resolver.
    InvokeRuntimeCallingConvention calling_convention;
    codegen->EmitParallelMoves(locations->InAt(0),
                               Location::RegisterLocation(calling_convention.GetRegisterAt(0)),
                               Primitive::kPrimInt,
                               locations->InAt(1),
                               Location::RegisterLocation(calling_convention.GetRegisterAt(1)),
                               Primitive::kPrimInt);
    mips64_codegen->InvokeRuntime(QUICK_ENTRY_POINT(pThrowArrayBounds),
                                  instruction_,
                                  instruction_->GetDexPc(),
                                  this);
    CheckEntrypointTypes<kQuickThrowArrayBounds, void, int32_t, int32_t>();
  }

  bool IsFatal() const OVERRIDE { return true; }

  const char* GetDescription() const OVERRIDE { return "BoundsCheckSlowPathMIPS64"; }

 private:
  DISALLOW_COPY_AND_ASSIGN(BoundsCheckSlowPathMIPS64);
};

class DivZeroCheckSlowPathMIPS64 : public SlowPathCodeMIPS64 {
 public:
  explicit DivZeroCheckSlowPathMIPS64(HDivZeroCheck* instruction) : SlowPathCodeMIPS64(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    CodeGeneratorMIPS64* mips64_codegen = down_cast<CodeGeneratorMIPS64*>(codegen);
    __ Bind(GetEntryLabel());
    if (instruction_->CanThrowIntoCatchBlock()) {
      // Live registers will be restored in the catch block if caught.
      SaveLiveRegisters(codegen, instruction_->GetLocations());
    }
    mips64_codegen->InvokeRuntime(QUICK_ENTRY_POINT(pThrowDivZero),
                                  instruction_,
                                  instruction_->GetDexPc(),
                                  this);
    CheckEntrypointTypes<kQuickThrowDivZero, void, void>();
  }

  bool IsFatal() const OVERRIDE { return true; }

  const char* GetDescription() const OVERRIDE { return "DivZeroCheckSlowPathMIPS64"; }

 private:
  DISALLOW_COPY_AND_ASSIGN(DivZeroCheckSlowPathMIPS64);
};

class LoadClassSlowPathMIPS64 : public SlowPathCodeMIPS64 {
 public:
  LoadClassSlowPathMIPS64(HLoadClass* cls,
                          HInstruction* at,
                          uint32_t dex_pc,
                          bool do_clinit)
      : SlowPathCodeMIPS64(at), cls_(cls), at_(at), dex_pc_(dex_pc), do_clinit_(do_clinit) {
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
    __ Bc(GetExitLabel());
  }

  const char* GetDescription() const OVERRIDE { return "LoadClassSlowPathMIPS64"; }

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
  explicit LoadStringSlowPathMIPS64(HLoadString* instruction) : SlowPathCodeMIPS64(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    LocationSummary* locations = instruction_->GetLocations();
    DCHECK(!locations->GetLiveRegisters()->ContainsCoreRegister(locations->Out().reg()));
    CodeGeneratorMIPS64* mips64_codegen = down_cast<CodeGeneratorMIPS64*>(codegen);

    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, locations);

    InvokeRuntimeCallingConvention calling_convention;
    const uint32_t string_index = instruction_->AsLoadString()->GetStringIndex();
    __ LoadConst32(calling_convention.GetRegisterAt(0), string_index);
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
    __ Bc(GetExitLabel());
  }

  const char* GetDescription() const OVERRIDE { return "LoadStringSlowPathMIPS64"; }

 private:
  DISALLOW_COPY_AND_ASSIGN(LoadStringSlowPathMIPS64);
};

class NullCheckSlowPathMIPS64 : public SlowPathCodeMIPS64 {
 public:
  explicit NullCheckSlowPathMIPS64(HNullCheck* instr) : SlowPathCodeMIPS64(instr) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    CodeGeneratorMIPS64* mips64_codegen = down_cast<CodeGeneratorMIPS64*>(codegen);
    __ Bind(GetEntryLabel());
    if (instruction_->CanThrowIntoCatchBlock()) {
      // Live registers will be restored in the catch block if caught.
      SaveLiveRegisters(codegen, instruction_->GetLocations());
    }
    mips64_codegen->InvokeRuntime(QUICK_ENTRY_POINT(pThrowNullPointer),
                                  instruction_,
                                  instruction_->GetDexPc(),
                                  this);
    CheckEntrypointTypes<kQuickThrowNullPointer, void, void>();
  }

  bool IsFatal() const OVERRIDE { return true; }

  const char* GetDescription() const OVERRIDE { return "NullCheckSlowPathMIPS64"; }

 private:
  DISALLOW_COPY_AND_ASSIGN(NullCheckSlowPathMIPS64);
};

class SuspendCheckSlowPathMIPS64 : public SlowPathCodeMIPS64 {
 public:
  SuspendCheckSlowPathMIPS64(HSuspendCheck* instruction, HBasicBlock* successor)
      : SlowPathCodeMIPS64(instruction), successor_(successor) {}

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
      __ Bc(GetReturnLabel());
    } else {
      __ Bc(mips64_codegen->GetLabelOf(successor_));
    }
  }

  Mips64Label* GetReturnLabel() {
    DCHECK(successor_ == nullptr);
    return &return_label_;
  }

  const char* GetDescription() const OVERRIDE { return "SuspendCheckSlowPathMIPS64"; }

 private:
  // If not null, the block to branch to after the suspend check.
  HBasicBlock* const successor_;

  // If `successor_` is null, the label to branch to after the suspend check.
  Mips64Label return_label_;

  DISALLOW_COPY_AND_ASSIGN(SuspendCheckSlowPathMIPS64);
};

class TypeCheckSlowPathMIPS64 : public SlowPathCodeMIPS64 {
 public:
  explicit TypeCheckSlowPathMIPS64(HInstruction* instruction) : SlowPathCodeMIPS64(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    LocationSummary* locations = instruction_->GetLocations();
    Location object_class = instruction_->IsCheckCast() ? locations->GetTemp(0) : locations->Out();
    uint32_t dex_pc = instruction_->GetDexPc();
    DCHECK(instruction_->IsCheckCast()
           || !locations->GetLiveRegisters()->ContainsCoreRegister(locations->Out().reg()));
    CodeGeneratorMIPS64* mips64_codegen = down_cast<CodeGeneratorMIPS64*>(codegen);

    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, locations);

    // We're moving two locations to locations that could overlap, so we need a parallel
    // move resolver.
    InvokeRuntimeCallingConvention calling_convention;
    codegen->EmitParallelMoves(locations->InAt(1),
                               Location::RegisterLocation(calling_convention.GetRegisterAt(0)),
                               Primitive::kPrimNot,
                               object_class,
                               Location::RegisterLocation(calling_convention.GetRegisterAt(1)),
                               Primitive::kPrimNot);

    if (instruction_->IsInstanceOf()) {
      mips64_codegen->InvokeRuntime(QUICK_ENTRY_POINT(pInstanceofNonTrivial),
                                    instruction_,
                                    dex_pc,
                                    this);
      CheckEntrypointTypes<
          kQuickInstanceofNonTrivial, uint32_t, const mirror::Class*, const mirror::Class*>();
      Primitive::Type ret_type = instruction_->GetType();
      Location ret_loc = calling_convention.GetReturnLocation(ret_type);
      mips64_codegen->MoveLocation(locations->Out(), ret_loc, ret_type);
    } else {
      DCHECK(instruction_->IsCheckCast());
      mips64_codegen->InvokeRuntime(QUICK_ENTRY_POINT(pCheckCast), instruction_, dex_pc, this);
      CheckEntrypointTypes<kQuickCheckCast, void, const mirror::Class*, const mirror::Class*>();
    }

    RestoreLiveRegisters(codegen, locations);
    __ Bc(GetExitLabel());
  }

  const char* GetDescription() const OVERRIDE { return "TypeCheckSlowPathMIPS64"; }

 private:
  DISALLOW_COPY_AND_ASSIGN(TypeCheckSlowPathMIPS64);
};

class DeoptimizationSlowPathMIPS64 : public SlowPathCodeMIPS64 {
 public:
  explicit DeoptimizationSlowPathMIPS64(HDeoptimize* instruction)
    : SlowPathCodeMIPS64(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    CodeGeneratorMIPS64* mips64_codegen = down_cast<CodeGeneratorMIPS64*>(codegen);
    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, instruction_->GetLocations());
    mips64_codegen->InvokeRuntime(QUICK_ENTRY_POINT(pDeoptimize),
                                  instruction_,
                                  instruction_->GetDexPc(),
                                  this);
    CheckEntrypointTypes<kQuickDeoptimize, void, void>();
  }

  const char* GetDescription() const OVERRIDE { return "DeoptimizationSlowPathMIPS64"; }

 private:
  DISALLOW_COPY_AND_ASSIGN(DeoptimizationSlowPathMIPS64);
};

CodeGeneratorMIPS64::CodeGeneratorMIPS64(HGraph* graph,
                                         const Mips64InstructionSetFeatures& isa_features,
                                         const CompilerOptions& compiler_options,
                                         OptimizingCompilerStats* stats)
    : CodeGenerator(graph,
                    kNumberOfGpuRegisters,
                    kNumberOfFpuRegisters,
                    /* number_of_register_pairs */ 0,
                    ComputeRegisterMask(reinterpret_cast<const int*>(kCoreCalleeSaves),
                                        arraysize(kCoreCalleeSaves)),
                    ComputeRegisterMask(reinterpret_cast<const int*>(kFpuCalleeSaves),
                                        arraysize(kFpuCalleeSaves)),
                    compiler_options,
                    stats),
      block_labels_(nullptr),
      location_builder_(graph, this),
      instruction_visitor_(graph, this),
      move_resolver_(graph->GetArena(), this),
      assembler_(graph->GetArena()),
      isa_features_(isa_features) {
  // Save RA (containing the return address) to mimic Quick.
  AddAllocatedRegister(Location::RegisterLocation(RA));
}

#undef __
#define __ down_cast<Mips64Assembler*>(GetAssembler())->
#define QUICK_ENTRY_POINT(x) QUICK_ENTRYPOINT_OFFSET(kMips64DoublewordSize, x).Int32Value()

void CodeGeneratorMIPS64::Finalize(CodeAllocator* allocator) {
  // Ensure that we fix up branches.
  __ FinalizeCode();

  // Adjust native pc offsets in stack maps.
  for (size_t i = 0, num = stack_map_stream_.GetNumberOfStackMaps(); i != num; ++i) {
    uint32_t old_position = stack_map_stream_.GetStackMap(i).native_pc_offset;
    uint32_t new_position = __ GetAdjustedPosition(old_position);
    DCHECK_GE(new_position, old_position);
    stack_map_stream_.SetStackMapNativePcOffset(i, new_position);
  }

  // Adjust pc offsets for the disassembly information.
  if (disasm_info_ != nullptr) {
    GeneratedCodeInterval* frame_entry_interval = disasm_info_->GetFrameEntryInterval();
    frame_entry_interval->start = __ GetAdjustedPosition(frame_entry_interval->start);
    frame_entry_interval->end = __ GetAdjustedPosition(frame_entry_interval->end);
    for (auto& it : *disasm_info_->GetInstructionIntervals()) {
      it.second.start = __ GetAdjustedPosition(it.second.start);
      it.second.end = __ GetAdjustedPosition(it.second.end);
    }
    for (auto& it : *disasm_info_->GetSlowPathIntervals()) {
      it.code_interval.start = __ GetAdjustedPosition(it.code_interval.start);
      it.code_interval.end = __ GetAdjustedPosition(it.code_interval.end);
    }
  }

  CodeGenerator::Finalize(allocator);
}

Mips64Assembler* ParallelMoveResolverMIPS64::GetAssembler() const {
  return codegen_->GetAssembler();
}

void ParallelMoveResolverMIPS64::EmitMove(size_t index) {
  MoveOperands* move = moves_[index];
  codegen_->MoveLocation(move->GetDestination(), move->GetSource(), move->GetType());
}

void ParallelMoveResolverMIPS64::EmitSwap(size_t index) {
  MoveOperands* move = moves_[index];
  codegen_->SwapLocations(move->GetDestination(), move->GetSource(), move->GetType());
}

void ParallelMoveResolverMIPS64::RestoreScratch(int reg) {
  // Pop reg
  __ Ld(GpuRegister(reg), SP, 0);
  __ DecreaseFrameSize(kMips64DoublewordSize);
}

void ParallelMoveResolverMIPS64::SpillScratch(int reg) {
  // Push reg
  __ IncreaseFrameSize(kMips64DoublewordSize);
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
  int stack_offset = ensure_scratch.IsSpilled() ? kMips64DoublewordSize : 0;
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

static dwarf::Reg DWARFReg(FpuRegister reg) {
  return dwarf::Reg::Mips64Fp(static_cast<int>(reg));
}

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
      ofs -= kMips64DoublewordSize;
      __ Sd(reg, SP, ofs);
      __ cfi().RelOffset(DWARFReg(reg), ofs);
    }
  }

  for (int i = arraysize(kFpuCalleeSaves) - 1; i >= 0; --i) {
    FpuRegister reg = kFpuCalleeSaves[i];
    if (allocated_registers_.ContainsFloatingPointRegister(reg)) {
      ofs -= kMips64DoublewordSize;
      __ Sdc1(reg, SP, ofs);
      __ cfi().RelOffset(DWARFReg(reg), ofs);
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
        ofs += kMips64DoublewordSize;
        __ cfi().Restore(DWARFReg(reg));
      }
    }

    for (size_t i = 0; i < arraysize(kCoreCalleeSaves); ++i) {
      GpuRegister reg = kCoreCalleeSaves[i];
      if (allocated_registers_.ContainsCoreRegister(reg)) {
        __ Ld(reg, SP, ofs);
        ofs += kMips64DoublewordSize;
        __ cfi().Restore(DWARFReg(reg));
      }
    }

    DCHECK_EQ(ofs, FrameEntrySpillSize());
    __ DecreaseFrameSize(ofs);
  }

  __ Jr(RA);
  __ Nop();

  __ cfi().RestoreState();
  __ cfi().DefCFAOffset(GetFrameSize());
}

void CodeGeneratorMIPS64::Bind(HBasicBlock* block) {
  __ Bind(GetLabelOf(block));
}

void CodeGeneratorMIPS64::MoveLocation(Location destination,
                                       Location source,
                                       Primitive::Type dst_type) {
  if (source.Equals(destination)) {
    return;
  }

  // A valid move can always be inferred from the destination and source
  // locations. When moving from and to a register, the argument type can be
  // used to generate 32bit instead of 64bit moves.
  bool unspecified_type = (dst_type == Primitive::kPrimVoid);
  DCHECK_EQ(unspecified_type, false);

  if (destination.IsRegister() || destination.IsFpuRegister()) {
    if (unspecified_type) {
      HConstant* src_cst = source.IsConstant() ? source.GetConstant() : nullptr;
      if (source.IsStackSlot() ||
          (src_cst != nullptr && (src_cst->IsIntConstant()
                                  || src_cst->IsFloatConstant()
                                  || src_cst->IsNullConstant()))) {
        // For stack slots and 32bit constants, a 64bit type is appropriate.
        dst_type = destination.IsRegister() ? Primitive::kPrimInt : Primitive::kPrimFloat;
      } else {
        // If the source is a double stack slot or a 64bit constant, a 64bit
        // type is appropriate. Else the source is a register, and since the
        // type has not been specified, we chose a 64bit type to force a 64bit
        // move.
        dst_type = destination.IsRegister() ? Primitive::kPrimLong : Primitive::kPrimDouble;
      }
    }
    DCHECK((destination.IsFpuRegister() && Primitive::IsFloatingPointType(dst_type)) ||
           (destination.IsRegister() && !Primitive::IsFloatingPointType(dst_type)));
    if (source.IsStackSlot() || source.IsDoubleStackSlot()) {
      // Move to GPR/FPR from stack
      LoadOperandType load_type = source.IsStackSlot() ? kLoadWord : kLoadDoubleword;
      if (Primitive::IsFloatingPointType(dst_type)) {
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
      if (!Primitive::IsFloatingPointType(dst_type)) {
        gpr = destination.AsRegister<GpuRegister>();
      }
      if (dst_type == Primitive::kPrimInt || dst_type == Primitive::kPrimFloat) {
        int32_t value = GetInt32ValueOf(source.GetConstant()->AsConstant());
        if (Primitive::IsFloatingPointType(dst_type) && value == 0) {
          gpr = ZERO;
        } else {
          __ LoadConst32(gpr, value);
        }
      } else {
        int64_t value = GetInt64ValueOf(source.GetConstant()->AsConstant());
        if (Primitive::IsFloatingPointType(dst_type) && value == 0) {
          gpr = ZERO;
        } else {
          __ LoadConst64(gpr, value);
        }
      }
      if (dst_type == Primitive::kPrimFloat) {
        __ Mtc1(gpr, destination.AsFpuRegister<FpuRegister>());
      } else if (dst_type == Primitive::kPrimDouble) {
        __ Dmtc1(gpr, destination.AsFpuRegister<FpuRegister>());
      }
    } else if (source.IsRegister()) {
      if (destination.IsRegister()) {
        // Move to GPR from GPR
        __ Move(destination.AsRegister<GpuRegister>(), source.AsRegister<GpuRegister>());
      } else {
        DCHECK(destination.IsFpuRegister());
        if (Primitive::Is64BitType(dst_type)) {
          __ Dmtc1(source.AsRegister<GpuRegister>(), destination.AsFpuRegister<FpuRegister>());
        } else {
          __ Mtc1(source.AsRegister<GpuRegister>(), destination.AsFpuRegister<FpuRegister>());
        }
      }
    } else if (source.IsFpuRegister()) {
      if (destination.IsFpuRegister()) {
        // Move to FPR from FPR
        if (dst_type == Primitive::kPrimFloat) {
          __ MovS(destination.AsFpuRegister<FpuRegister>(), source.AsFpuRegister<FpuRegister>());
        } else {
          DCHECK_EQ(dst_type, Primitive::kPrimDouble);
          __ MovD(destination.AsFpuRegister<FpuRegister>(), source.AsFpuRegister<FpuRegister>());
        }
      } else {
        DCHECK(destination.IsRegister());
        if (Primitive::Is64BitType(dst_type)) {
          __ Dmfc1(destination.AsRegister<GpuRegister>(), source.AsFpuRegister<FpuRegister>());
        } else {
          __ Mfc1(destination.AsRegister<GpuRegister>(), source.AsFpuRegister<FpuRegister>());
        }
      }
    }
  } else {  // The destination is not a register. It must be a stack slot.
    DCHECK(destination.IsStackSlot() || destination.IsDoubleStackSlot());
    if (source.IsRegister() || source.IsFpuRegister()) {
      if (unspecified_type) {
        if (source.IsRegister()) {
          dst_type = destination.IsStackSlot() ? Primitive::kPrimInt : Primitive::kPrimLong;
        } else {
          dst_type = destination.IsStackSlot() ? Primitive::kPrimFloat : Primitive::kPrimDouble;
        }
      }
      DCHECK((destination.IsDoubleStackSlot() == Primitive::Is64BitType(dst_type)) &&
             (source.IsFpuRegister() == Primitive::IsFloatingPointType(dst_type)));
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
      GpuRegister gpr = ZERO;
      if (destination.IsStackSlot()) {
        int32_t value = GetInt32ValueOf(src_cst->AsConstant());
        if (value != 0) {
          gpr = TMP;
          __ LoadConst32(gpr, value);
        }
      } else {
        DCHECK(destination.IsDoubleStackSlot());
        int64_t value = GetInt64ValueOf(src_cst->AsConstant());
        if (value != 0) {
          gpr = TMP;
          __ LoadConst64(gpr, value);
        }
      }
      __ StoreToOffset(store_type, gpr, SP, destination.GetStackIndex());
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

void CodeGeneratorMIPS64::SwapLocations(Location loc1, Location loc2, Primitive::Type type) {
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
    if (type == Primitive::kPrimFloat) {
      __ MovS(FTMP, r1);
      __ MovS(r1, r2);
      __ MovS(r2, FTMP);
    } else {
      DCHECK_EQ(type, Primitive::kPrimDouble);
      __ MovD(FTMP, r1);
      __ MovD(r1, r2);
      __ MovD(r2, FTMP);
    }
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

void CodeGeneratorMIPS64::MoveConstant(Location location, int32_t value) {
  DCHECK(location.IsRegister());
  __ LoadConst32(location.AsRegister<GpuRegister>(), value);
}

void CodeGeneratorMIPS64::AddLocationAsTemp(Location location, LocationSummary* locations) {
  if (location.IsRegister()) {
    locations->AddTemp(location);
  } else {
    UNIMPLEMENTED(FATAL) << "AddLocationAsTemp not implemented for location " << location;
  }
}

void CodeGeneratorMIPS64::MarkGCCard(GpuRegister object,
                                     GpuRegister value,
                                     bool value_can_be_null) {
  Mips64Label done;
  GpuRegister card = AT;
  GpuRegister temp = TMP;
  if (value_can_be_null) {
    __ Beqzc(value, &done);
  }
  __ LoadFromOffset(kLoadDoubleword,
                    card,
                    TR,
                    Thread::CardTableOffset<kMips64DoublewordSize>().Int32Value());
  __ Dsrl(temp, object, gc::accounting::CardTable::kCardShift);
  __ Daddu(temp, card, temp);
  __ Sb(card, temp, 0);
  if (value_can_be_null) {
    __ Bind(&done);
  }
}

void CodeGeneratorMIPS64::SetupBlockedRegisters() const {
  // ZERO, K0, K1, GP, SP, RA are always reserved and can't be allocated.
  blocked_core_registers_[ZERO] = true;
  blocked_core_registers_[K0] = true;
  blocked_core_registers_[K1] = true;
  blocked_core_registers_[GP] = true;
  blocked_core_registers_[SP] = true;
  blocked_core_registers_[RA] = true;

  // AT, TMP(T8) and TMP2(T3) are used as temporary/scratch
  // registers (similar to how AT is used by MIPS assemblers).
  blocked_core_registers_[AT] = true;
  blocked_core_registers_[TMP] = true;
  blocked_core_registers_[TMP2] = true;
  blocked_fpu_registers_[FTMP] = true;

  // Reserve suspend and thread registers.
  blocked_core_registers_[S0] = true;
  blocked_core_registers_[TR] = true;

  // Reserve T9 for function calls
  blocked_core_registers_[T9] = true;

  // TODO: review; anything else?

  // TODO: remove once all the issues with register saving/restoring are sorted out.
  for (size_t i = 0; i < arraysize(kCoreCalleeSaves); ++i) {
    blocked_core_registers_[kCoreCalleeSaves[i]] = true;
  }

  for (size_t i = 0; i < arraysize(kFpuCalleeSaves); ++i) {
    blocked_fpu_registers_[kFpuCalleeSaves[i]] = true;
  }
}

size_t CodeGeneratorMIPS64::SaveCoreRegister(size_t stack_index, uint32_t reg_id) {
  __ StoreToOffset(kStoreDoubleword, GpuRegister(reg_id), SP, stack_index);
  return kMips64DoublewordSize;
}

size_t CodeGeneratorMIPS64::RestoreCoreRegister(size_t stack_index, uint32_t reg_id) {
  __ LoadFromOffset(kLoadDoubleword, GpuRegister(reg_id), SP, stack_index);
  return kMips64DoublewordSize;
}

size_t CodeGeneratorMIPS64::SaveFloatingPointRegister(size_t stack_index, uint32_t reg_id) {
  __ StoreFpuToOffset(kStoreDoubleword, FpuRegister(reg_id), SP, stack_index);
  return kMips64DoublewordSize;
}

size_t CodeGeneratorMIPS64::RestoreFloatingPointRegister(size_t stack_index, uint32_t reg_id) {
  __ LoadFpuFromOffset(kLoadDoubleword, FpuRegister(reg_id), SP, stack_index);
  return kMips64DoublewordSize;
}

void CodeGeneratorMIPS64::DumpCoreRegister(std::ostream& stream, int reg) const {
  stream << GpuRegister(reg);
}

void CodeGeneratorMIPS64::DumpFloatingPointRegister(std::ostream& stream, int reg) const {
  stream << FpuRegister(reg);
}

void CodeGeneratorMIPS64::InvokeRuntime(QuickEntrypointEnum entrypoint,
                                     HInstruction* instruction,
                                     uint32_t dex_pc,
                                     SlowPathCode* slow_path) {
  InvokeRuntime(GetThreadOffset<kMips64DoublewordSize>(entrypoint).Int32Value(),
                instruction,
                dex_pc,
                slow_path);
}

void CodeGeneratorMIPS64::InvokeRuntime(int32_t entry_point_offset,
                                        HInstruction* instruction,
                                        uint32_t dex_pc,
                                        SlowPathCode* slow_path) {
  ValidateInvokeRuntime(instruction, slow_path);
  // TODO: anything related to T9/GP/GOT/PIC/.so's?
  __ LoadFromOffset(kLoadDoubleword, T9, TR, entry_point_offset);
  __ Jalr(T9);
  __ Nop();
  RecordPcInfo(instruction, dex_pc, slow_path);
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
                    Thread::ThreadFlagsOffset<kMips64DoublewordSize>().Int32Value());
  if (successor == nullptr) {
    __ Bnezc(TMP, slow_path->GetEntryLabel());
    __ Bind(slow_path->GetReturnLabel());
  } else {
    __ Beqzc(TMP, codegen_->GetLabelOf(successor));
    __ Bc(slow_path->GetEntryLabel());
    // slow_path will return to GetLabelOf(successor).
  }
}

InstructionCodeGeneratorMIPS64::InstructionCodeGeneratorMIPS64(HGraph* graph,
                                                               CodeGeneratorMIPS64* codegen)
      : InstructionCodeGenerator(graph, codegen),
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
  DCHECK(instr->IsShl() || instr->IsShr() || instr->IsUShr() || instr->IsRor());

  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instr);
  Primitive::Type type = instr->GetResultType();
  switch (type) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong: {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RegisterOrConstant(instr->InputAt(1)));
      locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      break;
    }
    default:
      LOG(FATAL) << "Unexpected shift type " << type;
  }
}

void InstructionCodeGeneratorMIPS64::HandleShift(HBinaryOperation* instr) {
  DCHECK(instr->IsShl() || instr->IsShr() || instr->IsUShr() || instr->IsRor());
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
        uint32_t shift_value = rhs_imm &
            (type == Primitive::kPrimInt ? kMaxIntShiftDistance : kMaxLongShiftDistance);

        if (shift_value == 0) {
          if (dst != lhs) {
            __ Move(dst, lhs);
          }
        } else if (type == Primitive::kPrimInt) {
          if (instr->IsShl()) {
            __ Sll(dst, lhs, shift_value);
          } else if (instr->IsShr()) {
            __ Sra(dst, lhs, shift_value);
          } else if (instr->IsUShr()) {
            __ Srl(dst, lhs, shift_value);
          } else {
            __ Rotr(dst, lhs, shift_value);
          }
        } else {
          if (shift_value < 32) {
            if (instr->IsShl()) {
              __ Dsll(dst, lhs, shift_value);
            } else if (instr->IsShr()) {
              __ Dsra(dst, lhs, shift_value);
            } else if (instr->IsUShr()) {
              __ Dsrl(dst, lhs, shift_value);
            } else {
              __ Drotr(dst, lhs, shift_value);
            }
          } else {
            shift_value -= 32;
            if (instr->IsShl()) {
              __ Dsll32(dst, lhs, shift_value);
            } else if (instr->IsShr()) {
              __ Dsra32(dst, lhs, shift_value);
            } else if (instr->IsUShr()) {
              __ Dsrl32(dst, lhs, shift_value);
            } else {
              __ Drotr32(dst, lhs, shift_value);
            }
          }
        }
      } else {
        if (type == Primitive::kPrimInt) {
          if (instr->IsShl()) {
            __ Sllv(dst, lhs, rhs_reg);
          } else if (instr->IsShr()) {
            __ Srav(dst, lhs, rhs_reg);
          } else if (instr->IsUShr()) {
            __ Srlv(dst, lhs, rhs_reg);
          } else {
            __ Rotrv(dst, lhs, rhs_reg);
          }
        } else {
          if (instr->IsShl()) {
            __ Dsllv(dst, lhs, rhs_reg);
          } else if (instr->IsShr()) {
            __ Dsrav(dst, lhs, rhs_reg);
          } else if (instr->IsUShr()) {
            __ Dsrlv(dst, lhs, rhs_reg);
          } else {
            __ Drotrv(dst, lhs, rhs_reg);
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
          codegen_->MarkGCCard(obj, value, instruction->GetValueCanBeNull());
        }
      } else {
        DCHECK_EQ(value_type, Primitive::kPrimNot);
        codegen_->InvokeRuntime(QUICK_ENTRY_POINT(pAputObject),
                                instruction,
                                instruction->GetDexPc(),
                                nullptr);
        CheckEntrypointTypes<kQuickAputObject, void, mirror::Array*, int32_t, mirror::Object*>();
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
  LocationSummary::CallKind call_kind = instruction->CanThrowIntoCatchBlock()
      ? LocationSummary::kCallOnSlowPath
      : LocationSummary::kNoCall;
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction, call_kind);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  if (instruction->HasUses()) {
    locations->SetOut(Location::SameAsFirstInput());
  }
}

void InstructionCodeGeneratorMIPS64::VisitBoundsCheck(HBoundsCheck* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  BoundsCheckSlowPathMIPS64* slow_path =
      new (GetGraph()->GetArena()) BoundsCheckSlowPathMIPS64(instruction);
  codegen_->AddSlowPath(slow_path);

  GpuRegister index = locations->InAt(0).AsRegister<GpuRegister>();
  GpuRegister length = locations->InAt(1).AsRegister<GpuRegister>();

  // length is limited by the maximum positive signed 32-bit integer.
  // Unsigned comparison of length and index checks for index < 0
  // and for length <= index simultaneously.
  __ Bgeuc(index, length, slow_path->GetEntryLabel());
}

void LocationsBuilderMIPS64::VisitCheckCast(HCheckCast* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(
      instruction,
      LocationSummary::kCallOnSlowPath);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  // Note that TypeCheckSlowPathMIPS64 uses this register too.
  locations->AddTemp(Location::RequiresRegister());
}

void InstructionCodeGeneratorMIPS64::VisitCheckCast(HCheckCast* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  GpuRegister obj = locations->InAt(0).AsRegister<GpuRegister>();
  GpuRegister cls = locations->InAt(1).AsRegister<GpuRegister>();
  GpuRegister obj_cls = locations->GetTemp(0).AsRegister<GpuRegister>();

  SlowPathCodeMIPS64* slow_path =
      new (GetGraph()->GetArena()) TypeCheckSlowPathMIPS64(instruction);
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

  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(compare);

  switch (in_type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimShort:
    case Primitive::kPrimChar:
    case Primitive::kPrimInt:
    case Primitive::kPrimLong:
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RegisterOrConstant(compare->InputAt(1)));
      locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      break;

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetInAt(1, Location::RequiresFpuRegister());
      locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      break;

    default:
      LOG(FATAL) << "Unexpected type for compare operation " << in_type;
  }
}

void InstructionCodeGeneratorMIPS64::VisitCompare(HCompare* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  GpuRegister res = locations->Out().AsRegister<GpuRegister>();
  Primitive::Type in_type = instruction->InputAt(0)->GetType();

  //  0 if: left == right
  //  1 if: left  > right
  // -1 if: left  < right
  switch (in_type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimShort:
    case Primitive::kPrimChar:
    case Primitive::kPrimInt:
    case Primitive::kPrimLong: {
      GpuRegister lhs = locations->InAt(0).AsRegister<GpuRegister>();
      Location rhs_location = locations->InAt(1);
      bool use_imm = rhs_location.IsConstant();
      GpuRegister rhs = ZERO;
      if (use_imm) {
        if (in_type == Primitive::kPrimLong) {
          int64_t value = CodeGenerator::GetInt64ValueOf(rhs_location.GetConstant()->AsConstant());
          if (value != 0) {
            rhs = AT;
            __ LoadConst64(rhs, value);
          }
        } else {
          int32_t value = CodeGenerator::GetInt32ValueOf(rhs_location.GetConstant()->AsConstant());
          if (value != 0) {
            rhs = AT;
            __ LoadConst32(rhs, value);
          }
        }
      } else {
        rhs = rhs_location.AsRegister<GpuRegister>();
      }
      __ Slt(TMP, lhs, rhs);
      __ Slt(res, rhs, lhs);
      __ Subu(res, res, TMP);
      break;
    }

    case Primitive::kPrimFloat: {
      FpuRegister lhs = locations->InAt(0).AsFpuRegister<FpuRegister>();
      FpuRegister rhs = locations->InAt(1).AsFpuRegister<FpuRegister>();
      Mips64Label done;
      __ CmpEqS(FTMP, lhs, rhs);
      __ LoadConst32(res, 0);
      __ Bc1nez(FTMP, &done);
      if (instruction->IsGtBias()) {
        __ CmpLtS(FTMP, lhs, rhs);
        __ LoadConst32(res, -1);
        __ Bc1nez(FTMP, &done);
        __ LoadConst32(res, 1);
      } else {
        __ CmpLtS(FTMP, rhs, lhs);
        __ LoadConst32(res, 1);
        __ Bc1nez(FTMP, &done);
        __ LoadConst32(res, -1);
      }
      __ Bind(&done);
      break;
    }

    case Primitive::kPrimDouble: {
      FpuRegister lhs = locations->InAt(0).AsFpuRegister<FpuRegister>();
      FpuRegister rhs = locations->InAt(1).AsFpuRegister<FpuRegister>();
      Mips64Label done;
      __ CmpEqD(FTMP, lhs, rhs);
      __ LoadConst32(res, 0);
      __ Bc1nez(FTMP, &done);
      if (instruction->IsGtBias()) {
        __ CmpLtD(FTMP, lhs, rhs);
        __ LoadConst32(res, -1);
        __ Bc1nez(FTMP, &done);
        __ LoadConst32(res, 1);
      } else {
        __ CmpLtD(FTMP, rhs, lhs);
        __ LoadConst32(res, 1);
        __ Bc1nez(FTMP, &done);
        __ LoadConst32(res, -1);
      }
      __ Bind(&done);
      break;
    }

    default:
      LOG(FATAL) << "Unimplemented compare type " << in_type;
  }
}

void LocationsBuilderMIPS64::HandleCondition(HCondition* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  switch (instruction->InputAt(0)->GetType()) {
    default:
    case Primitive::kPrimLong:
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RegisterOrConstant(instruction->InputAt(1)));
      break;

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetInAt(1, Location::RequiresFpuRegister());
      break;
  }
  if (!instruction->IsEmittedAtUseSite()) {
    locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
  }
}

void InstructionCodeGeneratorMIPS64::HandleCondition(HCondition* instruction) {
  if (instruction->IsEmittedAtUseSite()) {
    return;
  }

  Primitive::Type type = instruction->InputAt(0)->GetType();
  LocationSummary* locations = instruction->GetLocations();
  GpuRegister dst = locations->Out().AsRegister<GpuRegister>();
  Mips64Label true_label;

  switch (type) {
    default:
      // Integer case.
      GenerateIntLongCompare(instruction->GetCondition(), /* is64bit */ false, locations);
      return;
    case Primitive::kPrimLong:
      GenerateIntLongCompare(instruction->GetCondition(), /* is64bit */ true, locations);
      return;

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      // TODO: don't use branches.
      GenerateFpCompareAndBranch(instruction->GetCondition(),
                                 instruction->IsGtBias(),
                                 type,
                                 locations,
                                 &true_label);
      break;
  }

  // Convert the branches into the result.
  Mips64Label done;

  // False case: result = 0.
  __ LoadConst32(dst, 0);
  __ Bc(&done);

  // True case: result = 1.
  __ Bind(&true_label);
  __ LoadConst32(dst, 1);
  __ Bind(&done);
}

void InstructionCodeGeneratorMIPS64::DivRemOneOrMinusOne(HBinaryOperation* instruction) {
  DCHECK(instruction->IsDiv() || instruction->IsRem());
  Primitive::Type type = instruction->GetResultType();

  LocationSummary* locations = instruction->GetLocations();
  Location second = locations->InAt(1);
  DCHECK(second.IsConstant());

  GpuRegister out = locations->Out().AsRegister<GpuRegister>();
  GpuRegister dividend = locations->InAt(0).AsRegister<GpuRegister>();
  int64_t imm = Int64FromConstant(second.GetConstant());
  DCHECK(imm == 1 || imm == -1);

  if (instruction->IsRem()) {
    __ Move(out, ZERO);
  } else {
    if (imm == -1) {
      if (type == Primitive::kPrimInt) {
        __ Subu(out, ZERO, dividend);
      } else {
        DCHECK_EQ(type, Primitive::kPrimLong);
        __ Dsubu(out, ZERO, dividend);
      }
    } else if (out != dividend) {
      __ Move(out, dividend);
    }
  }
}

void InstructionCodeGeneratorMIPS64::DivRemByPowerOfTwo(HBinaryOperation* instruction) {
  DCHECK(instruction->IsDiv() || instruction->IsRem());
  Primitive::Type type = instruction->GetResultType();

  LocationSummary* locations = instruction->GetLocations();
  Location second = locations->InAt(1);
  DCHECK(second.IsConstant());

  GpuRegister out = locations->Out().AsRegister<GpuRegister>();
  GpuRegister dividend = locations->InAt(0).AsRegister<GpuRegister>();
  int64_t imm = Int64FromConstant(second.GetConstant());
  uint64_t abs_imm = static_cast<uint64_t>(AbsOrMin(imm));
  int ctz_imm = CTZ(abs_imm);

  if (instruction->IsDiv()) {
    if (type == Primitive::kPrimInt) {
      if (ctz_imm == 1) {
        // Fast path for division by +/-2, which is very common.
        __ Srl(TMP, dividend, 31);
      } else {
        __ Sra(TMP, dividend, 31);
        __ Srl(TMP, TMP, 32 - ctz_imm);
      }
      __ Addu(out, dividend, TMP);
      __ Sra(out, out, ctz_imm);
      if (imm < 0) {
        __ Subu(out, ZERO, out);
      }
    } else {
      DCHECK_EQ(type, Primitive::kPrimLong);
      if (ctz_imm == 1) {
        // Fast path for division by +/-2, which is very common.
        __ Dsrl32(TMP, dividend, 31);
      } else {
        __ Dsra32(TMP, dividend, 31);
        if (ctz_imm > 32) {
          __ Dsrl(TMP, TMP, 64 - ctz_imm);
        } else {
          __ Dsrl32(TMP, TMP, 32 - ctz_imm);
        }
      }
      __ Daddu(out, dividend, TMP);
      if (ctz_imm < 32) {
        __ Dsra(out, out, ctz_imm);
      } else {
        __ Dsra32(out, out, ctz_imm - 32);
      }
      if (imm < 0) {
        __ Dsubu(out, ZERO, out);
      }
    }
  } else {
    if (type == Primitive::kPrimInt) {
      if (ctz_imm == 1) {
        // Fast path for modulo +/-2, which is very common.
        __ Sra(TMP, dividend, 31);
        __ Subu(out, dividend, TMP);
        __ Andi(out, out, 1);
        __ Addu(out, out, TMP);
      } else {
        __ Sra(TMP, dividend, 31);
        __ Srl(TMP, TMP, 32 - ctz_imm);
        __ Addu(out, dividend, TMP);
        if (IsUint<16>(abs_imm - 1)) {
          __ Andi(out, out, abs_imm - 1);
        } else {
          __ Sll(out, out, 32 - ctz_imm);
          __ Srl(out, out, 32 - ctz_imm);
        }
        __ Subu(out, out, TMP);
      }
    } else {
      DCHECK_EQ(type, Primitive::kPrimLong);
      if (ctz_imm == 1) {
        // Fast path for modulo +/-2, which is very common.
        __ Dsra32(TMP, dividend, 31);
        __ Dsubu(out, dividend, TMP);
        __ Andi(out, out, 1);
        __ Daddu(out, out, TMP);
      } else {
        __ Dsra32(TMP, dividend, 31);
        if (ctz_imm > 32) {
          __ Dsrl(TMP, TMP, 64 - ctz_imm);
        } else {
          __ Dsrl32(TMP, TMP, 32 - ctz_imm);
        }
        __ Daddu(out, dividend, TMP);
        if (IsUint<16>(abs_imm - 1)) {
          __ Andi(out, out, abs_imm - 1);
        } else {
          if (ctz_imm > 32) {
            __ Dsll(out, out, 64 - ctz_imm);
            __ Dsrl(out, out, 64 - ctz_imm);
          } else {
            __ Dsll32(out, out, 32 - ctz_imm);
            __ Dsrl32(out, out, 32 - ctz_imm);
          }
        }
        __ Dsubu(out, out, TMP);
      }
    }
  }
}

void InstructionCodeGeneratorMIPS64::GenerateDivRemWithAnyConstant(HBinaryOperation* instruction) {
  DCHECK(instruction->IsDiv() || instruction->IsRem());

  LocationSummary* locations = instruction->GetLocations();
  Location second = locations->InAt(1);
  DCHECK(second.IsConstant());

  GpuRegister out = locations->Out().AsRegister<GpuRegister>();
  GpuRegister dividend = locations->InAt(0).AsRegister<GpuRegister>();
  int64_t imm = Int64FromConstant(second.GetConstant());

  Primitive::Type type = instruction->GetResultType();
  DCHECK(type == Primitive::kPrimInt || type == Primitive::kPrimLong) << type;

  int64_t magic;
  int shift;
  CalculateMagicAndShiftForDivRem(imm,
                                  (type == Primitive::kPrimLong),
                                  &magic,
                                  &shift);

  if (type == Primitive::kPrimInt) {
    __ LoadConst32(TMP, magic);
    __ MuhR6(TMP, dividend, TMP);

    if (imm > 0 && magic < 0) {
      __ Addu(TMP, TMP, dividend);
    } else if (imm < 0 && magic > 0) {
      __ Subu(TMP, TMP, dividend);
    }

    if (shift != 0) {
      __ Sra(TMP, TMP, shift);
    }

    if (instruction->IsDiv()) {
      __ Sra(out, TMP, 31);
      __ Subu(out, TMP, out);
    } else {
      __ Sra(AT, TMP, 31);
      __ Subu(AT, TMP, AT);
      __ LoadConst32(TMP, imm);
      __ MulR6(TMP, AT, TMP);
      __ Subu(out, dividend, TMP);
    }
  } else {
    __ LoadConst64(TMP, magic);
    __ Dmuh(TMP, dividend, TMP);

    if (imm > 0 && magic < 0) {
      __ Daddu(TMP, TMP, dividend);
    } else if (imm < 0 && magic > 0) {
      __ Dsubu(TMP, TMP, dividend);
    }

    if (shift >= 32) {
      __ Dsra32(TMP, TMP, shift - 32);
    } else if (shift > 0) {
      __ Dsra(TMP, TMP, shift);
    }

    if (instruction->IsDiv()) {
      __ Dsra32(out, TMP, 31);
      __ Dsubu(out, TMP, out);
    } else {
      __ Dsra32(AT, TMP, 31);
      __ Dsubu(AT, TMP, AT);
      __ LoadConst64(TMP, imm);
      __ Dmul(TMP, AT, TMP);
      __ Dsubu(out, dividend, TMP);
    }
  }
}

void InstructionCodeGeneratorMIPS64::GenerateDivRemIntegral(HBinaryOperation* instruction) {
  DCHECK(instruction->IsDiv() || instruction->IsRem());
  Primitive::Type type = instruction->GetResultType();
  DCHECK(type == Primitive::kPrimInt || type == Primitive::kPrimLong) << type;

  LocationSummary* locations = instruction->GetLocations();
  GpuRegister out = locations->Out().AsRegister<GpuRegister>();
  Location second = locations->InAt(1);

  if (second.IsConstant()) {
    int64_t imm = Int64FromConstant(second.GetConstant());
    if (imm == 0) {
      // Do not generate anything. DivZeroCheck would prevent any code to be executed.
    } else if (imm == 1 || imm == -1) {
      DivRemOneOrMinusOne(instruction);
    } else if (IsPowerOfTwo(AbsOrMin(imm))) {
      DivRemByPowerOfTwo(instruction);
    } else {
      DCHECK(imm <= -2 || imm >= 2);
      GenerateDivRemWithAnyConstant(instruction);
    }
  } else {
    GpuRegister dividend = locations->InAt(0).AsRegister<GpuRegister>();
    GpuRegister divisor = second.AsRegister<GpuRegister>();
    if (instruction->IsDiv()) {
      if (type == Primitive::kPrimInt)
        __ DivR6(out, dividend, divisor);
      else
        __ Ddiv(out, dividend, divisor);
    } else {
      if (type == Primitive::kPrimInt)
        __ ModR6(out, dividend, divisor);
      else
        __ Dmod(out, dividend, divisor);
    }
  }
}

void LocationsBuilderMIPS64::VisitDiv(HDiv* div) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(div, LocationSummary::kNoCall);
  switch (div->GetResultType()) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong:
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RegisterOrConstant(div->InputAt(1)));
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
    case Primitive::kPrimLong:
      GenerateDivRemIntegral(instruction);
      break;
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
  LocationSummary::CallKind call_kind = instruction->CanThrowIntoCatchBlock()
      ? LocationSummary::kCallOnSlowPath
      : LocationSummary::kNoCall;
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction, call_kind);
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

  if (!Primitive::IsIntegralType(type)) {
    LOG(FATAL) << "Unexpected type " << type << " for DivZeroCheck.";
    return;
  }

  if (value.IsConstant()) {
    int64_t divisor = codegen_->GetInt64ValueOf(value.GetConstant()->AsConstant());
    if (divisor == 0) {
      __ Bc(slow_path->GetEntryLabel());
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

void InstructionCodeGeneratorMIPS64::HandleGoto(HInstruction* got, HBasicBlock* successor) {
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
    __ Bc(codegen_->GetLabelOf(successor));
  }
}

void LocationsBuilderMIPS64::VisitGoto(HGoto* got) {
  got->SetLocations(nullptr);
}

void InstructionCodeGeneratorMIPS64::VisitGoto(HGoto* got) {
  HandleGoto(got, got->GetSuccessor());
}

void LocationsBuilderMIPS64::VisitTryBoundary(HTryBoundary* try_boundary) {
  try_boundary->SetLocations(nullptr);
}

void InstructionCodeGeneratorMIPS64::VisitTryBoundary(HTryBoundary* try_boundary) {
  HBasicBlock* successor = try_boundary->GetNormalFlowSuccessor();
  if (!successor->IsExitBlock()) {
    HandleGoto(try_boundary, successor);
  }
}

void InstructionCodeGeneratorMIPS64::GenerateIntLongCompare(IfCondition cond,
                                                            bool is64bit,
                                                            LocationSummary* locations) {
  GpuRegister dst = locations->Out().AsRegister<GpuRegister>();
  GpuRegister lhs = locations->InAt(0).AsRegister<GpuRegister>();
  Location rhs_location = locations->InAt(1);
  GpuRegister rhs_reg = ZERO;
  int64_t rhs_imm = 0;
  bool use_imm = rhs_location.IsConstant();
  if (use_imm) {
    if (is64bit) {
      rhs_imm = CodeGenerator::GetInt64ValueOf(rhs_location.GetConstant());
    } else {
      rhs_imm = CodeGenerator::GetInt32ValueOf(rhs_location.GetConstant());
    }
  } else {
    rhs_reg = rhs_location.AsRegister<GpuRegister>();
  }
  int64_t rhs_imm_plus_one = rhs_imm + UINT64_C(1);

  switch (cond) {
    case kCondEQ:
    case kCondNE:
      if (use_imm && IsUint<16>(rhs_imm)) {
        __ Xori(dst, lhs, rhs_imm);
      } else {
        if (use_imm) {
          rhs_reg = TMP;
          __ LoadConst64(rhs_reg, rhs_imm);
        }
        __ Xor(dst, lhs, rhs_reg);
      }
      if (cond == kCondEQ) {
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
          __ LoadConst64(rhs_reg, rhs_imm);
        }
        __ Slt(dst, lhs, rhs_reg);
      }
      if (cond == kCondGE) {
        // Simulate lhs >= rhs via !(lhs < rhs) since there's
        // only the slt instruction but no sge.
        __ Xori(dst, dst, 1);
      }
      break;

    case kCondLE:
    case kCondGT:
      if (use_imm && IsInt<16>(rhs_imm_plus_one)) {
        // Simulate lhs <= rhs via lhs < rhs + 1.
        __ Slti(dst, lhs, rhs_imm_plus_one);
        if (cond == kCondGT) {
          // Simulate lhs > rhs via !(lhs <= rhs) since there's
          // only the slti instruction but no sgti.
          __ Xori(dst, dst, 1);
        }
      } else {
        if (use_imm) {
          rhs_reg = TMP;
          __ LoadConst64(rhs_reg, rhs_imm);
        }
        __ Slt(dst, rhs_reg, lhs);
        if (cond == kCondLE) {
          // Simulate lhs <= rhs via !(rhs < lhs) since there's
          // only the slt instruction but no sle.
          __ Xori(dst, dst, 1);
        }
      }
      break;

    case kCondB:
    case kCondAE:
      if (use_imm && IsInt<16>(rhs_imm)) {
        // Sltiu sign-extends its 16-bit immediate operand before
        // the comparison and thus lets us compare directly with
        // unsigned values in the ranges [0, 0x7fff] and
        // [0x[ffffffff]ffff8000, 0x[ffffffff]ffffffff].
        __ Sltiu(dst, lhs, rhs_imm);
      } else {
        if (use_imm) {
          rhs_reg = TMP;
          __ LoadConst64(rhs_reg, rhs_imm);
        }
        __ Sltu(dst, lhs, rhs_reg);
      }
      if (cond == kCondAE) {
        // Simulate lhs >= rhs via !(lhs < rhs) since there's
        // only the sltu instruction but no sgeu.
        __ Xori(dst, dst, 1);
      }
      break;

    case kCondBE:
    case kCondA:
      if (use_imm && (rhs_imm_plus_one != 0) && IsInt<16>(rhs_imm_plus_one)) {
        // Simulate lhs <= rhs via lhs < rhs + 1.
        // Note that this only works if rhs + 1 does not overflow
        // to 0, hence the check above.
        // Sltiu sign-extends its 16-bit immediate operand before
        // the comparison and thus lets us compare directly with
        // unsigned values in the ranges [0, 0x7fff] and
        // [0x[ffffffff]ffff8000, 0x[ffffffff]ffffffff].
        __ Sltiu(dst, lhs, rhs_imm_plus_one);
        if (cond == kCondA) {
          // Simulate lhs > rhs via !(lhs <= rhs) since there's
          // only the sltiu instruction but no sgtiu.
          __ Xori(dst, dst, 1);
        }
      } else {
        if (use_imm) {
          rhs_reg = TMP;
          __ LoadConst64(rhs_reg, rhs_imm);
        }
        __ Sltu(dst, rhs_reg, lhs);
        if (cond == kCondBE) {
          // Simulate lhs <= rhs via !(rhs < lhs) since there's
          // only the sltu instruction but no sleu.
          __ Xori(dst, dst, 1);
        }
      }
      break;
  }
}

void InstructionCodeGeneratorMIPS64::GenerateIntLongCompareAndBranch(IfCondition cond,
                                                                     bool is64bit,
                                                                     LocationSummary* locations,
                                                                     Mips64Label* label) {
  GpuRegister lhs = locations->InAt(0).AsRegister<GpuRegister>();
  Location rhs_location = locations->InAt(1);
  GpuRegister rhs_reg = ZERO;
  int64_t rhs_imm = 0;
  bool use_imm = rhs_location.IsConstant();
  if (use_imm) {
    if (is64bit) {
      rhs_imm = CodeGenerator::GetInt64ValueOf(rhs_location.GetConstant());
    } else {
      rhs_imm = CodeGenerator::GetInt32ValueOf(rhs_location.GetConstant());
    }
  } else {
    rhs_reg = rhs_location.AsRegister<GpuRegister>();
  }

  if (use_imm && rhs_imm == 0) {
    switch (cond) {
      case kCondEQ:
      case kCondBE:  // <= 0 if zero
        __ Beqzc(lhs, label);
        break;
      case kCondNE:
      case kCondA:  // > 0 if non-zero
        __ Bnezc(lhs, label);
        break;
      case kCondLT:
        __ Bltzc(lhs, label);
        break;
      case kCondGE:
        __ Bgezc(lhs, label);
        break;
      case kCondLE:
        __ Blezc(lhs, label);
        break;
      case kCondGT:
        __ Bgtzc(lhs, label);
        break;
      case kCondB:  // always false
        break;
      case kCondAE:  // always true
        __ Bc(label);
        break;
    }
  } else {
    if (use_imm) {
      rhs_reg = TMP;
      __ LoadConst64(rhs_reg, rhs_imm);
    }
    switch (cond) {
      case kCondEQ:
        __ Beqc(lhs, rhs_reg, label);
        break;
      case kCondNE:
        __ Bnec(lhs, rhs_reg, label);
        break;
      case kCondLT:
        __ Bltc(lhs, rhs_reg, label);
        break;
      case kCondGE:
        __ Bgec(lhs, rhs_reg, label);
        break;
      case kCondLE:
        __ Bgec(rhs_reg, lhs, label);
        break;
      case kCondGT:
        __ Bltc(rhs_reg, lhs, label);
        break;
      case kCondB:
        __ Bltuc(lhs, rhs_reg, label);
        break;
      case kCondAE:
        __ Bgeuc(lhs, rhs_reg, label);
        break;
      case kCondBE:
        __ Bgeuc(rhs_reg, lhs, label);
        break;
      case kCondA:
        __ Bltuc(rhs_reg, lhs, label);
        break;
    }
  }
}

void InstructionCodeGeneratorMIPS64::GenerateFpCompareAndBranch(IfCondition cond,
                                                                bool gt_bias,
                                                                Primitive::Type type,
                                                                LocationSummary* locations,
                                                                Mips64Label* label) {
  FpuRegister lhs = locations->InAt(0).AsFpuRegister<FpuRegister>();
  FpuRegister rhs = locations->InAt(1).AsFpuRegister<FpuRegister>();
  if (type == Primitive::kPrimFloat) {
    switch (cond) {
      case kCondEQ:
        __ CmpEqS(FTMP, lhs, rhs);
        __ Bc1nez(FTMP, label);
        break;
      case kCondNE:
        __ CmpEqS(FTMP, lhs, rhs);
        __ Bc1eqz(FTMP, label);
        break;
      case kCondLT:
        if (gt_bias) {
          __ CmpLtS(FTMP, lhs, rhs);
        } else {
          __ CmpUltS(FTMP, lhs, rhs);
        }
        __ Bc1nez(FTMP, label);
        break;
      case kCondLE:
        if (gt_bias) {
          __ CmpLeS(FTMP, lhs, rhs);
        } else {
          __ CmpUleS(FTMP, lhs, rhs);
        }
        __ Bc1nez(FTMP, label);
        break;
      case kCondGT:
        if (gt_bias) {
          __ CmpUltS(FTMP, rhs, lhs);
        } else {
          __ CmpLtS(FTMP, rhs, lhs);
        }
        __ Bc1nez(FTMP, label);
        break;
      case kCondGE:
        if (gt_bias) {
          __ CmpUleS(FTMP, rhs, lhs);
        } else {
          __ CmpLeS(FTMP, rhs, lhs);
        }
        __ Bc1nez(FTMP, label);
        break;
      default:
        LOG(FATAL) << "Unexpected non-floating-point condition";
    }
  } else {
    DCHECK_EQ(type, Primitive::kPrimDouble);
    switch (cond) {
      case kCondEQ:
        __ CmpEqD(FTMP, lhs, rhs);
        __ Bc1nez(FTMP, label);
        break;
      case kCondNE:
        __ CmpEqD(FTMP, lhs, rhs);
        __ Bc1eqz(FTMP, label);
        break;
      case kCondLT:
        if (gt_bias) {
          __ CmpLtD(FTMP, lhs, rhs);
        } else {
          __ CmpUltD(FTMP, lhs, rhs);
        }
        __ Bc1nez(FTMP, label);
        break;
      case kCondLE:
        if (gt_bias) {
          __ CmpLeD(FTMP, lhs, rhs);
        } else {
          __ CmpUleD(FTMP, lhs, rhs);
        }
        __ Bc1nez(FTMP, label);
        break;
      case kCondGT:
        if (gt_bias) {
          __ CmpUltD(FTMP, rhs, lhs);
        } else {
          __ CmpLtD(FTMP, rhs, lhs);
        }
        __ Bc1nez(FTMP, label);
        break;
      case kCondGE:
        if (gt_bias) {
          __ CmpUleD(FTMP, rhs, lhs);
        } else {
          __ CmpLeD(FTMP, rhs, lhs);
        }
        __ Bc1nez(FTMP, label);
        break;
      default:
        LOG(FATAL) << "Unexpected non-floating-point condition";
    }
  }
}

void InstructionCodeGeneratorMIPS64::GenerateTestAndBranch(HInstruction* instruction,
                                                           size_t condition_input_index,
                                                           Mips64Label* true_target,
                                                           Mips64Label* false_target) {
  HInstruction* cond = instruction->InputAt(condition_input_index);

  if (true_target == nullptr && false_target == nullptr) {
    // Nothing to do. The code always falls through.
    return;
  } else if (cond->IsIntConstant()) {
    // Constant condition, statically compared against "true" (integer value 1).
    if (cond->AsIntConstant()->IsTrue()) {
      if (true_target != nullptr) {
        __ Bc(true_target);
      }
    } else {
      DCHECK(cond->AsIntConstant()->IsFalse()) << cond->AsIntConstant()->GetValue();
      if (false_target != nullptr) {
        __ Bc(false_target);
      }
    }
    return;
  }

  // The following code generates these patterns:
  //  (1) true_target == nullptr && false_target != nullptr
  //        - opposite condition true => branch to false_target
  //  (2) true_target != nullptr && false_target == nullptr
  //        - condition true => branch to true_target
  //  (3) true_target != nullptr && false_target != nullptr
  //        - condition true => branch to true_target
  //        - branch to false_target
  if (IsBooleanValueOrMaterializedCondition(cond)) {
    // The condition instruction has been materialized, compare the output to 0.
    Location cond_val = instruction->GetLocations()->InAt(condition_input_index);
    DCHECK(cond_val.IsRegister());
    if (true_target == nullptr) {
      __ Beqzc(cond_val.AsRegister<GpuRegister>(), false_target);
    } else {
      __ Bnezc(cond_val.AsRegister<GpuRegister>(), true_target);
    }
  } else {
    // The condition instruction has not been materialized, use its inputs as
    // the comparison and its condition as the branch condition.
    HCondition* condition = cond->AsCondition();
    Primitive::Type type = condition->InputAt(0)->GetType();
    LocationSummary* locations = cond->GetLocations();
    IfCondition if_cond = condition->GetCondition();
    Mips64Label* branch_target = true_target;

    if (true_target == nullptr) {
      if_cond = condition->GetOppositeCondition();
      branch_target = false_target;
    }

    switch (type) {
      default:
        GenerateIntLongCompareAndBranch(if_cond, /* is64bit */ false, locations, branch_target);
        break;
      case Primitive::kPrimLong:
        GenerateIntLongCompareAndBranch(if_cond, /* is64bit */ true, locations, branch_target);
        break;
      case Primitive::kPrimFloat:
      case Primitive::kPrimDouble:
        GenerateFpCompareAndBranch(if_cond, condition->IsGtBias(), type, locations, branch_target);
        break;
    }
  }

  // If neither branch falls through (case 3), the conditional branch to `true_target`
  // was already emitted (case 2) and we need to emit a jump to `false_target`.
  if (true_target != nullptr && false_target != nullptr) {
    __ Bc(false_target);
  }
}

void LocationsBuilderMIPS64::VisitIf(HIf* if_instr) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(if_instr);
  if (IsBooleanValueOrMaterializedCondition(if_instr->InputAt(0))) {
    locations->SetInAt(0, Location::RequiresRegister());
  }
}

void InstructionCodeGeneratorMIPS64::VisitIf(HIf* if_instr) {
  HBasicBlock* true_successor = if_instr->IfTrueSuccessor();
  HBasicBlock* false_successor = if_instr->IfFalseSuccessor();
  Mips64Label* true_target = codegen_->GoesToNextBlock(if_instr->GetBlock(), true_successor) ?
      nullptr : codegen_->GetLabelOf(true_successor);
  Mips64Label* false_target = codegen_->GoesToNextBlock(if_instr->GetBlock(), false_successor) ?
      nullptr : codegen_->GetLabelOf(false_successor);
  GenerateTestAndBranch(if_instr, /* condition_input_index */ 0, true_target, false_target);
}

void LocationsBuilderMIPS64::VisitDeoptimize(HDeoptimize* deoptimize) {
  LocationSummary* locations = new (GetGraph()->GetArena())
      LocationSummary(deoptimize, LocationSummary::kCallOnSlowPath);
  if (IsBooleanValueOrMaterializedCondition(deoptimize->InputAt(0))) {
    locations->SetInAt(0, Location::RequiresRegister());
  }
}

void InstructionCodeGeneratorMIPS64::VisitDeoptimize(HDeoptimize* deoptimize) {
  SlowPathCodeMIPS64* slow_path =
      deopt_slow_paths_.NewSlowPath<DeoptimizationSlowPathMIPS64>(deoptimize);
  GenerateTestAndBranch(deoptimize,
                        /* condition_input_index */ 0,
                        slow_path->GetEntryLabel(),
                        /* false_target */ nullptr);
}

void LocationsBuilderMIPS64::VisitSelect(HSelect* select) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(select);
  if (Primitive::IsFloatingPointType(select->GetType())) {
    locations->SetInAt(0, Location::RequiresFpuRegister());
    locations->SetInAt(1, Location::RequiresFpuRegister());
  } else {
    locations->SetInAt(0, Location::RequiresRegister());
    locations->SetInAt(1, Location::RequiresRegister());
  }
  if (IsBooleanValueOrMaterializedCondition(select->GetCondition())) {
    locations->SetInAt(2, Location::RequiresRegister());
  }
  locations->SetOut(Location::SameAsFirstInput());
}

void InstructionCodeGeneratorMIPS64::VisitSelect(HSelect* select) {
  LocationSummary* locations = select->GetLocations();
  Mips64Label false_target;
  GenerateTestAndBranch(select,
                        /* condition_input_index */ 2,
                        /* true_target */ nullptr,
                        &false_target);
  codegen_->MoveLocation(locations->Out(), locations->InAt(1), select->GetType());
  __ Bind(&false_target);
}

void LocationsBuilderMIPS64::VisitNativeDebugInfo(HNativeDebugInfo* info) {
  new (GetGraph()->GetArena()) LocationSummary(info);
}

void InstructionCodeGeneratorMIPS64::VisitNativeDebugInfo(HNativeDebugInfo*) {
  // MaybeRecordNativeDebugInfo is already called implicitly in CodeGenerator::Compile.
}

void CodeGeneratorMIPS64::GenerateNop() {
  __ Nop();
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
                                                    const FieldInfo& field_info,
                                                    bool value_can_be_null) {
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
    codegen_->MarkGCCard(obj, src, value_can_be_null);
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
  HandleFieldSet(instruction, instruction->GetFieldInfo(), instruction->GetValueCanBeNull());
}

void LocationsBuilderMIPS64::VisitInstanceOf(HInstanceOf* instruction) {
  LocationSummary::CallKind call_kind =
      instruction->IsExactCheck() ? LocationSummary::kNoCall : LocationSummary::kCallOnSlowPath;
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction, call_kind);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  // The output does overlap inputs.
  // Note that TypeCheckSlowPathMIPS64 uses this register too.
  locations->SetOut(Location::RequiresRegister(), Location::kOutputOverlap);
}

void InstructionCodeGeneratorMIPS64::VisitInstanceOf(HInstanceOf* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  GpuRegister obj = locations->InAt(0).AsRegister<GpuRegister>();
  GpuRegister cls = locations->InAt(1).AsRegister<GpuRegister>();
  GpuRegister out = locations->Out().AsRegister<GpuRegister>();

  Mips64Label done;

  // Return 0 if `obj` is null.
  // TODO: Avoid this check if we know `obj` is not null.
  __ Move(out, ZERO);
  __ Beqzc(obj, &done);

  // Compare the class of `obj` with `cls`.
  __ LoadFromOffset(kLoadUnsignedWord, out, obj, mirror::Object::ClassOffset().Int32Value());
  if (instruction->IsExactCheck()) {
    // Classes must be equal for the instanceof to succeed.
    __ Xor(out, out, cls);
    __ Sltiu(out, out, 1);
  } else {
    // If the classes are not equal, we go into a slow path.
    DCHECK(locations->OnlyCallsOnSlowPath());
    SlowPathCodeMIPS64* slow_path =
        new (GetGraph()->GetArena()) TypeCheckSlowPathMIPS64(instruction);
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

void LocationsBuilderMIPS64::VisitInvokeUnresolved(HInvokeUnresolved* invoke) {
  // The trampoline uses the same calling convention as dex calling conventions,
  // except instead of loading arg0/r0 with the target Method*, arg0/r0 will contain
  // the method_idx.
  HandleInvoke(invoke);
}

void InstructionCodeGeneratorMIPS64::VisitInvokeUnresolved(HInvokeUnresolved* invoke) {
  codegen_->GenerateInvokeUnresolvedRuntimeCall(invoke);
}

void LocationsBuilderMIPS64::HandleInvoke(HInvoke* invoke) {
  InvokeDexCallingConventionVisitorMIPS64 calling_convention_visitor;
  CodeGenerator::CreateCommonInvokeLocationSummary(invoke, &calling_convention_visitor);
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
  Location receiver = invoke->GetLocations()->InAt(0);
  uint32_t class_offset = mirror::Object::ClassOffset().Int32Value();
  Offset entry_point = ArtMethod::EntryPointFromQuickCompiledCodeOffset(kMips64DoublewordSize);

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
  __ LoadFromOffset(kLoadDoubleword, temp, temp,
      mirror::Class::ImtPtrOffset(kMips64PointerSize).Uint32Value());
  uint32_t method_offset = static_cast<uint32_t>(ImTable::OffsetOfElement(
      invoke->GetImtIndex() % ImTable::kSize, kMips64PointerSize));
  // temp = temp->GetImtEntryAt(method_offset);
  __ LoadFromOffset(kLoadDoubleword, temp, temp, method_offset);
  // T9 = temp->GetEntryPoint();
  __ LoadFromOffset(kLoadDoubleword, T9, temp, entry_point.Int32Value());
  // T9();
  __ Jalr(T9);
  __ Nop();
  DCHECK(!codegen_->IsLeafMethod());
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
}

void LocationsBuilderMIPS64::VisitInvokeVirtual(HInvokeVirtual* invoke) {
  IntrinsicLocationsBuilderMIPS64 intrinsic(codegen_);
  if (intrinsic.TryDispatch(invoke)) {
    return;
  }

  HandleInvoke(invoke);
}

void LocationsBuilderMIPS64::VisitInvokeStaticOrDirect(HInvokeStaticOrDirect* invoke) {
  // Explicit clinit checks triggered by static invokes must have been pruned by
  // art::PrepareForRegisterAllocation.
  DCHECK(!invoke->IsStaticWithExplicitClinitCheck());

  IntrinsicLocationsBuilderMIPS64 intrinsic(codegen_);
  if (intrinsic.TryDispatch(invoke)) {
    return;
  }

  HandleInvoke(invoke);

  // While SetupBlockedRegisters() blocks registers S2-S8 due to their
  // clobbering somewhere else, reduce further register pressure by avoiding
  // allocation of a register for the current method pointer like on x86 baseline.
  // TODO: remove this once all the issues with register saving/restoring are
  // sorted out.
  if (invoke->HasCurrentMethodInput()) {
    LocationSummary* locations = invoke->GetLocations();
    Location location = locations->InAt(invoke->GetSpecialInputIndex());
    if (location.IsUnallocated() && location.GetPolicy() == Location::kRequiresRegister) {
      locations->SetInAt(invoke->GetSpecialInputIndex(), Location::NoLocation());
    }
  }
}

static bool TryGenerateIntrinsicCode(HInvoke* invoke, CodeGeneratorMIPS64* codegen) {
  if (invoke->GetLocations()->Intrinsified()) {
    IntrinsicCodeGeneratorMIPS64 intrinsic(codegen);
    intrinsic.Dispatch(invoke);
    return true;
  }
  return false;
}

HLoadString::LoadKind CodeGeneratorMIPS64::GetSupportedLoadStringKind(
    HLoadString::LoadKind desired_string_load_kind ATTRIBUTE_UNUSED) {
  // TODO: Implement other kinds.
  return HLoadString::LoadKind::kDexCacheViaMethod;
}

HInvokeStaticOrDirect::DispatchInfo CodeGeneratorMIPS64::GetSupportedInvokeStaticOrDirectDispatch(
      const HInvokeStaticOrDirect::DispatchInfo& desired_dispatch_info,
      MethodReference target_method ATTRIBUTE_UNUSED) {
  switch (desired_dispatch_info.method_load_kind) {
    case HInvokeStaticOrDirect::MethodLoadKind::kDirectAddressWithFixup:
    case HInvokeStaticOrDirect::MethodLoadKind::kDexCachePcRelative:
      // TODO: Implement these types. For the moment, we fall back to kDexCacheViaMethod.
      return HInvokeStaticOrDirect::DispatchInfo {
        HInvokeStaticOrDirect::MethodLoadKind::kDexCacheViaMethod,
        HInvokeStaticOrDirect::CodePtrLocation::kCallArtMethod,
        0u,
        0u
      };
    default:
      break;
  }
  switch (desired_dispatch_info.code_ptr_location) {
    case HInvokeStaticOrDirect::CodePtrLocation::kCallDirectWithFixup:
    case HInvokeStaticOrDirect::CodePtrLocation::kCallPCRelative:
      // TODO: Implement these types. For the moment, we fall back to kCallArtMethod.
      return HInvokeStaticOrDirect::DispatchInfo {
        desired_dispatch_info.method_load_kind,
        HInvokeStaticOrDirect::CodePtrLocation::kCallArtMethod,
        desired_dispatch_info.method_load_data,
        0u
      };
    default:
      return desired_dispatch_info;
  }
}

void CodeGeneratorMIPS64::GenerateStaticOrDirectCall(HInvokeStaticOrDirect* invoke, Location temp) {
  // All registers are assumed to be correctly set up per the calling convention.

  Location callee_method = temp;  // For all kinds except kRecursive, callee will be in temp.
  switch (invoke->GetMethodLoadKind()) {
    case HInvokeStaticOrDirect::MethodLoadKind::kStringInit:
      // temp = thread->string_init_entrypoint
      __ LoadFromOffset(kLoadDoubleword,
                        temp.AsRegister<GpuRegister>(),
                        TR,
                        invoke->GetStringInitOffset());
      break;
    case HInvokeStaticOrDirect::MethodLoadKind::kRecursive:
      callee_method = invoke->GetLocations()->InAt(invoke->GetSpecialInputIndex());
      break;
    case HInvokeStaticOrDirect::MethodLoadKind::kDirectAddress:
      __ LoadConst64(temp.AsRegister<GpuRegister>(), invoke->GetMethodAddress());
      break;
    case HInvokeStaticOrDirect::MethodLoadKind::kDirectAddressWithFixup:
    case HInvokeStaticOrDirect::MethodLoadKind::kDexCachePcRelative:
      // TODO: Implement these types.
      // Currently filtered out by GetSupportedInvokeStaticOrDirectDispatch().
      LOG(FATAL) << "Unsupported";
      UNREACHABLE();
    case HInvokeStaticOrDirect::MethodLoadKind::kDexCacheViaMethod: {
      Location current_method = invoke->GetLocations()->InAt(invoke->GetSpecialInputIndex());
      GpuRegister reg = temp.AsRegister<GpuRegister>();
      GpuRegister method_reg;
      if (current_method.IsRegister()) {
        method_reg = current_method.AsRegister<GpuRegister>();
      } else {
        // TODO: use the appropriate DCHECK() here if possible.
        // DCHECK(invoke->GetLocations()->Intrinsified());
        DCHECK(!current_method.IsValid());
        method_reg = reg;
        __ Ld(reg, SP, kCurrentMethodStackOffset);
      }

      // temp = temp->dex_cache_resolved_methods_;
      __ LoadFromOffset(kLoadDoubleword,
                        reg,
                        method_reg,
                        ArtMethod::DexCacheResolvedMethodsOffset(kMips64PointerSize).Int32Value());
      // temp = temp[index_in_cache];
      // Note: Don't use invoke->GetTargetMethod() as it may point to a different dex file.
      uint32_t index_in_cache = invoke->GetDexMethodIndex();
      __ LoadFromOffset(kLoadDoubleword,
                        reg,
                        reg,
                        CodeGenerator::GetCachePointerOffset(index_in_cache));
      break;
    }
  }

  switch (invoke->GetCodePtrLocation()) {
    case HInvokeStaticOrDirect::CodePtrLocation::kCallSelf:
      __ Jialc(&frame_entry_label_, T9);
      break;
    case HInvokeStaticOrDirect::CodePtrLocation::kCallDirect:
      // LR = invoke->GetDirectCodePtr();
      __ LoadConst64(T9, invoke->GetDirectCodePtr());
      // LR()
      __ Jalr(T9);
      __ Nop();
      break;
    case HInvokeStaticOrDirect::CodePtrLocation::kCallDirectWithFixup:
    case HInvokeStaticOrDirect::CodePtrLocation::kCallPCRelative:
      // TODO: Implement these types.
      // Currently filtered out by GetSupportedInvokeStaticOrDirectDispatch().
      LOG(FATAL) << "Unsupported";
      UNREACHABLE();
    case HInvokeStaticOrDirect::CodePtrLocation::kCallArtMethod:
      // T9 = callee_method->entry_point_from_quick_compiled_code_;
      __ LoadFromOffset(kLoadDoubleword,
                        T9,
                        callee_method.AsRegister<GpuRegister>(),
                        ArtMethod::EntryPointFromQuickCompiledCodeOffset(
                            kMips64DoublewordSize).Int32Value());
      // T9()
      __ Jalr(T9);
      __ Nop();
      break;
  }
  DCHECK(!IsLeafMethod());
}

void InstructionCodeGeneratorMIPS64::VisitInvokeStaticOrDirect(HInvokeStaticOrDirect* invoke) {
  // Explicit clinit checks triggered by static invokes must have been pruned by
  // art::PrepareForRegisterAllocation.
  DCHECK(!invoke->IsStaticWithExplicitClinitCheck());

  if (TryGenerateIntrinsicCode(invoke, codegen_)) {
    return;
  }

  LocationSummary* locations = invoke->GetLocations();
  codegen_->GenerateStaticOrDirectCall(invoke,
                                       locations->HasTemps()
                                           ? locations->GetTemp(0)
                                           : Location::NoLocation());
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
}

void CodeGeneratorMIPS64::GenerateVirtualCall(HInvokeVirtual* invoke, Location temp_location) {
  // Use the calling convention instead of the location of the receiver, as
  // intrinsics may have put the receiver in a different register. In the intrinsics
  // slow path, the arguments have been moved to the right place, so here we are
  // guaranteed that the receiver is the first register of the calling convention.
  InvokeDexCallingConvention calling_convention;
  GpuRegister receiver = calling_convention.GetRegisterAt(0);

  GpuRegister temp = temp_location.AsRegister<GpuRegister>();
  size_t method_offset = mirror::Class::EmbeddedVTableEntryOffset(
      invoke->GetVTableIndex(), kMips64PointerSize).SizeValue();
  uint32_t class_offset = mirror::Object::ClassOffset().Int32Value();
  Offset entry_point = ArtMethod::EntryPointFromQuickCompiledCodeOffset(kMips64DoublewordSize);

  // temp = object->GetClass();
  __ LoadFromOffset(kLoadUnsignedWord, temp, receiver, class_offset);
  MaybeRecordImplicitNullCheck(invoke);
  // temp = temp->GetMethodAt(method_offset);
  __ LoadFromOffset(kLoadDoubleword, temp, temp, method_offset);
  // T9 = temp->GetEntryPoint();
  __ LoadFromOffset(kLoadDoubleword, T9, temp, entry_point.Int32Value());
  // T9();
  __ Jalr(T9);
  __ Nop();
}

void InstructionCodeGeneratorMIPS64::VisitInvokeVirtual(HInvokeVirtual* invoke) {
  if (TryGenerateIntrinsicCode(invoke, codegen_)) {
    return;
  }

  codegen_->GenerateVirtualCall(invoke, invoke->GetLocations()->GetTemp(0));
  DCHECK(!codegen_->IsLeafMethod());
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
}

void LocationsBuilderMIPS64::VisitLoadClass(HLoadClass* cls) {
  InvokeRuntimeCallingConvention calling_convention;
  CodeGenerator::CreateLoadClassLocationSummary(
      cls,
      Location::RegisterLocation(calling_convention.GetRegisterAt(0)),
      calling_convention.GetReturnLocation(cls->GetType()));
}

void InstructionCodeGeneratorMIPS64::VisitLoadClass(HLoadClass* cls) {
  LocationSummary* locations = cls->GetLocations();
  if (cls->NeedsAccessCheck()) {
    codegen_->MoveConstant(locations->GetTemp(0), cls->GetTypeIndex());
    codegen_->InvokeRuntime(QUICK_ENTRY_POINT(pInitializeTypeAndVerifyAccess),
                            cls,
                            cls->GetDexPc(),
                            nullptr);
    CheckEntrypointTypes<kQuickInitializeTypeAndVerifyAccess, void*, uint32_t>();
    return;
  }

  GpuRegister out = locations->Out().AsRegister<GpuRegister>();
  GpuRegister current_method = locations->InAt(0).AsRegister<GpuRegister>();
  if (cls->IsReferrersClass()) {
    DCHECK(!cls->CanCallRuntime());
    DCHECK(!cls->MustGenerateClinitCheck());
    __ LoadFromOffset(kLoadUnsignedWord, out, current_method,
                      ArtMethod::DeclaringClassOffset().Int32Value());
  } else {
    __ LoadFromOffset(kLoadDoubleword, out, current_method,
                      ArtMethod::DexCacheResolvedTypesOffset(kMips64PointerSize).Int32Value());
    __ LoadFromOffset(
        kLoadUnsignedWord, out, out, CodeGenerator::GetCacheOffset(cls->GetTypeIndex()));
    // TODO: We will need a read barrier here.
    if (!cls->IsInDexCache() || cls->MustGenerateClinitCheck()) {
      DCHECK(cls->CanCallRuntime());
      SlowPathCodeMIPS64* slow_path = new (GetGraph()->GetArena()) LoadClassSlowPathMIPS64(
          cls,
          cls,
          cls->GetDexPc(),
          cls->MustGenerateClinitCheck());
      codegen_->AddSlowPath(slow_path);
      if (!cls->IsInDexCache()) {
        __ Beqzc(out, slow_path->GetEntryLabel());
      }
      if (cls->MustGenerateClinitCheck()) {
        GenerateClassInitializationCheck(slow_path, out);
      } else {
        __ Bind(slow_path->GetExitLabel());
      }
    }
  }
}

static int32_t GetExceptionTlsOffset() {
  return Thread::ExceptionOffset<kMips64DoublewordSize>().Int32Value();
}

void LocationsBuilderMIPS64::VisitLoadException(HLoadException* load) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(load, LocationSummary::kNoCall);
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorMIPS64::VisitLoadException(HLoadException* load) {
  GpuRegister out = load->GetLocations()->Out().AsRegister<GpuRegister>();
  __ LoadFromOffset(kLoadUnsignedWord, out, TR, GetExceptionTlsOffset());
}

void LocationsBuilderMIPS64::VisitClearException(HClearException* clear) {
  new (GetGraph()->GetArena()) LocationSummary(clear, LocationSummary::kNoCall);
}

void InstructionCodeGeneratorMIPS64::VisitClearException(HClearException* clear ATTRIBUTE_UNUSED) {
  __ StoreToOffset(kStoreWord, ZERO, TR, GetExceptionTlsOffset());
}

void LocationsBuilderMIPS64::VisitLoadString(HLoadString* load) {
  LocationSummary::CallKind call_kind = load->NeedsEnvironment()
      ? LocationSummary::kCallOnSlowPath
      : LocationSummary::kNoCall;
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(load, call_kind);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorMIPS64::VisitLoadString(HLoadString* load) {
  LocationSummary* locations = load->GetLocations();
  GpuRegister out = locations->Out().AsRegister<GpuRegister>();
  GpuRegister current_method = locations->InAt(0).AsRegister<GpuRegister>();
  __ LoadFromOffset(kLoadUnsignedWord, out, current_method,
                    ArtMethod::DeclaringClassOffset().Int32Value());
  __ LoadFromOffset(kLoadDoubleword, out, out, mirror::Class::DexCacheStringsOffset().Int32Value());
  __ LoadFromOffset(
      kLoadUnsignedWord, out, out, CodeGenerator::GetCacheOffset(load->GetStringIndex()));
  // TODO: We will need a read barrier here.

  if (!load->IsInDexCache()) {
    SlowPathCodeMIPS64* slow_path = new (GetGraph()->GetArena()) LoadStringSlowPathMIPS64(load);
    codegen_->AddSlowPath(slow_path);
    __ Beqzc(out, slow_path->GetEntryLabel());
    __ Bind(slow_path->GetExitLabel());
  }
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
  if (instruction->IsEnter()) {
    CheckEntrypointTypes<kQuickLockObject, void, mirror::Object*>();
  } else {
    CheckEntrypointTypes<kQuickUnlockObject, void, mirror::Object*>();
  }
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
  locations->SetOut(calling_convention.GetReturnLocation(Primitive::kPrimNot));
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
  locations->SetInAt(1, Location::RegisterLocation(calling_convention.GetRegisterAt(2)));
}

void InstructionCodeGeneratorMIPS64::VisitNewArray(HNewArray* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  // Move an uint16_t value to a register.
  __ LoadConst32(locations->GetTemp(0).AsRegister<GpuRegister>(), instruction->GetTypeIndex());
  codegen_->InvokeRuntime(instruction->GetEntrypoint(),
                          instruction,
                          instruction->GetDexPc(),
                          nullptr);
  CheckEntrypointTypes<kQuickAllocArrayWithAccessCheck, void*, uint32_t, int32_t, ArtMethod*>();
}

void LocationsBuilderMIPS64::VisitNewInstance(HNewInstance* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCall);
  InvokeRuntimeCallingConvention calling_convention;
  if (instruction->IsStringAlloc()) {
    locations->AddTemp(Location::RegisterLocation(kMethodRegisterArgument));
  } else {
    locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
    locations->SetInAt(1, Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
  }
  locations->SetOut(calling_convention.GetReturnLocation(Primitive::kPrimNot));
}

void InstructionCodeGeneratorMIPS64::VisitNewInstance(HNewInstance* instruction) {
  if (instruction->IsStringAlloc()) {
    // String is allocated through StringFactory. Call NewEmptyString entry point.
    GpuRegister temp = instruction->GetLocations()->GetTemp(0).AsRegister<GpuRegister>();
    MemberOffset code_offset =
        ArtMethod::EntryPointFromQuickCompiledCodeOffset(kMips64DoublewordSize);
    __ LoadFromOffset(kLoadDoubleword, temp, TR, QUICK_ENTRY_POINT(pNewEmptyString));
    __ LoadFromOffset(kLoadDoubleword, T9, temp, code_offset.Int32Value());
    __ Jalr(T9);
    __ Nop();
    codegen_->RecordPcInfo(instruction, instruction->GetDexPc());
  } else {
    codegen_->InvokeRuntime(instruction->GetEntrypoint(),
                            instruction,
                            instruction->GetDexPc(),
                            nullptr);
    CheckEntrypointTypes<kQuickAllocObjectWithAccessCheck, void*, uint32_t, ArtMethod*>();
  }
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
  LocationSummary::CallKind call_kind = instruction->CanThrowIntoCatchBlock()
      ? LocationSummary::kCallOnSlowPath
      : LocationSummary::kNoCall;
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction, call_kind);
  locations->SetInAt(0, Location::RequiresRegister());
  if (instruction->HasUses()) {
    locations->SetOut(Location::SameAsFirstInput());
  }
}

void CodeGeneratorMIPS64::GenerateImplicitNullCheck(HNullCheck* instruction) {
  if (CanMoveNullCheckToUser(instruction)) {
    return;
  }
  Location obj = instruction->GetLocations()->InAt(0);

  __ Lw(ZERO, obj.AsRegister<GpuRegister>(), 0);
  RecordPcInfo(instruction, instruction->GetDexPc());
}

void CodeGeneratorMIPS64::GenerateExplicitNullCheck(HNullCheck* instruction) {
  SlowPathCodeMIPS64* slow_path = new (GetGraph()->GetArena()) NullCheckSlowPathMIPS64(instruction);
  AddSlowPath(slow_path);

  Location obj = instruction->GetLocations()->InAt(0);

  __ Beqzc(obj.AsRegister<GpuRegister>(), slow_path->GetEntryLabel());
}

void InstructionCodeGeneratorMIPS64::VisitNullCheck(HNullCheck* instruction) {
  codegen_->GenerateNullCheck(instruction);
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

void LocationsBuilderMIPS64::VisitCurrentMethod(HCurrentMethod* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetOut(Location::RegisterLocation(kMethodRegisterArgument));
}

void InstructionCodeGeneratorMIPS64::VisitCurrentMethod(HCurrentMethod* instruction
                                                        ATTRIBUTE_UNUSED) {
  // Nothing to do, the method is already at its location.
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
      locations->SetInAt(1, Location::RegisterOrConstant(rem->InputAt(1)));
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

  switch (type) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong:
      GenerateDivRemIntegral(instruction);
      break;

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      int32_t entry_offset = (type == Primitive::kPrimFloat) ? QUICK_ENTRY_POINT(pFmodf)
                                                             : QUICK_ENTRY_POINT(pFmod);
      codegen_->InvokeRuntime(entry_offset, instruction, instruction->GetDexPc(), nullptr);
      if (type == Primitive::kPrimFloat) {
        CheckEntrypointTypes<kQuickFmodf, float, float, float>();
      } else {
        CheckEntrypointTypes<kQuickFmod, double, double, double>();
      }
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

void LocationsBuilderMIPS64::VisitRor(HRor* ror) {
  HandleShift(ror);
}

void InstructionCodeGeneratorMIPS64::VisitRor(HRor* ror) {
  HandleShift(ror);
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
  HandleFieldSet(instruction, instruction->GetFieldInfo(), instruction->GetValueCanBeNull());
}

void LocationsBuilderMIPS64::VisitUnresolvedInstanceFieldGet(
    HUnresolvedInstanceFieldGet* instruction) {
  FieldAccessCallingConventionMIPS64 calling_convention;
  codegen_->CreateUnresolvedFieldLocationSummary(
      instruction, instruction->GetFieldType(), calling_convention);
}

void InstructionCodeGeneratorMIPS64::VisitUnresolvedInstanceFieldGet(
    HUnresolvedInstanceFieldGet* instruction) {
  FieldAccessCallingConventionMIPS64 calling_convention;
  codegen_->GenerateUnresolvedFieldAccess(instruction,
                                          instruction->GetFieldType(),
                                          instruction->GetFieldIndex(),
                                          instruction->GetDexPc(),
                                          calling_convention);
}

void LocationsBuilderMIPS64::VisitUnresolvedInstanceFieldSet(
    HUnresolvedInstanceFieldSet* instruction) {
  FieldAccessCallingConventionMIPS64 calling_convention;
  codegen_->CreateUnresolvedFieldLocationSummary(
      instruction, instruction->GetFieldType(), calling_convention);
}

void InstructionCodeGeneratorMIPS64::VisitUnresolvedInstanceFieldSet(
    HUnresolvedInstanceFieldSet* instruction) {
  FieldAccessCallingConventionMIPS64 calling_convention;
  codegen_->GenerateUnresolvedFieldAccess(instruction,
                                          instruction->GetFieldType(),
                                          instruction->GetFieldIndex(),
                                          instruction->GetDexPc(),
                                          calling_convention);
}

void LocationsBuilderMIPS64::VisitUnresolvedStaticFieldGet(
    HUnresolvedStaticFieldGet* instruction) {
  FieldAccessCallingConventionMIPS64 calling_convention;
  codegen_->CreateUnresolvedFieldLocationSummary(
      instruction, instruction->GetFieldType(), calling_convention);
}

void InstructionCodeGeneratorMIPS64::VisitUnresolvedStaticFieldGet(
    HUnresolvedStaticFieldGet* instruction) {
  FieldAccessCallingConventionMIPS64 calling_convention;
  codegen_->GenerateUnresolvedFieldAccess(instruction,
                                          instruction->GetFieldType(),
                                          instruction->GetFieldIndex(),
                                          instruction->GetDexPc(),
                                          calling_convention);
}

void LocationsBuilderMIPS64::VisitUnresolvedStaticFieldSet(
    HUnresolvedStaticFieldSet* instruction) {
  FieldAccessCallingConventionMIPS64 calling_convention;
  codegen_->CreateUnresolvedFieldLocationSummary(
      instruction, instruction->GetFieldType(), calling_convention);
}

void InstructionCodeGeneratorMIPS64::VisitUnresolvedStaticFieldSet(
    HUnresolvedStaticFieldSet* instruction) {
  FieldAccessCallingConventionMIPS64 calling_convention;
  codegen_->GenerateUnresolvedFieldAccess(instruction,
                                          instruction->GetFieldType(),
                                          instruction->GetFieldIndex(),
                                          instruction->GetDexPc(),
                                          calling_convention);
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

  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(conversion);

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
        if (input_type == Primitive::kPrimLong) {
          // Type conversion from long to types narrower than int is a result of code
          // transformations. To avoid unpredictable results for SEB and SEH, we first
          // need to sign-extend the low 32-bit value into bits 32 through 63.
          __ Sll(dst, src, 0);
          __ Seb(dst, dst);
        } else {
          __ Seb(dst, src);
        }
        break;
      case Primitive::kPrimShort:
        if (input_type == Primitive::kPrimLong) {
          // Type conversion from long to types narrower than int is a result of code
          // transformations. To avoid unpredictable results for SEB and SEH, we first
          // need to sign-extend the low 32-bit value into bits 32 through 63.
          __ Sll(dst, src, 0);
          __ Seh(dst, dst);
        } else {
          __ Seh(dst, src);
        }
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
    FpuRegister dst = locations->Out().AsFpuRegister<FpuRegister>();
    GpuRegister src = locations->InAt(0).AsRegister<GpuRegister>();
    if (input_type == Primitive::kPrimLong) {
      __ Dmtc1(src, FTMP);
      if (result_type == Primitive::kPrimFloat) {
        __ Cvtsl(dst, FTMP);
      } else {
        __ Cvtdl(dst, FTMP);
      }
    } else {
      __ Mtc1(src, FTMP);
      if (result_type == Primitive::kPrimFloat) {
        __ Cvtsw(dst, FTMP);
      } else {
        __ Cvtdw(dst, FTMP);
      }
    }
  } else if (Primitive::IsIntegralType(result_type) && Primitive::IsFloatingPointType(input_type)) {
    CHECK(result_type == Primitive::kPrimInt || result_type == Primitive::kPrimLong);
    GpuRegister dst = locations->Out().AsRegister<GpuRegister>();
    FpuRegister src = locations->InAt(0).AsFpuRegister<FpuRegister>();
    Mips64Label truncate;
    Mips64Label done;

    // When NAN2008=0 (R2 and before), the truncate instruction produces the maximum positive
    // value when the input is either a NaN or is outside of the range of the output type
    // after the truncation. IOW, the three special cases (NaN, too small, too big) produce
    // the same result.
    //
    // When NAN2008=1 (R6), the truncate instruction caps the output at the minimum/maximum
    // value of the output type if the input is outside of the range after the truncation or
    // produces 0 when the input is a NaN. IOW, the three special cases produce three distinct
    // results. This matches the desired float/double-to-int/long conversion exactly.
    //
    // So, NAN2008 affects handling of negative values and NaNs by the truncate instruction.
    //
    // The following code supports both NAN2008=0 and NAN2008=1 behaviors of the truncate
    // instruction, the reason being that the emulator implements NAN2008=0 on MIPS64R6,
    // even though it must be NAN2008=1 on R6.
    //
    // The code takes care of the different behaviors by first comparing the input to the
    // minimum output value (-2**-63 for truncating to long, -2**-31 for truncating to int).
    // If the input is greater than or equal to the minimum, it procedes to the truncate
    // instruction, which will handle such an input the same way irrespective of NAN2008.
    // Otherwise the input is compared to itself to determine whether it is a NaN or not
    // in order to return either zero or the minimum value.
    //
    // TODO: simplify this when the emulator correctly implements NAN2008=1 behavior of the
    // truncate instruction for MIPS64R6.
    if (input_type == Primitive::kPrimFloat) {
      uint32_t min_val = (result_type == Primitive::kPrimLong)
          ? bit_cast<uint32_t, float>(std::numeric_limits<int64_t>::min())
          : bit_cast<uint32_t, float>(std::numeric_limits<int32_t>::min());
      __ LoadConst32(TMP, min_val);
      __ Mtc1(TMP, FTMP);
      __ CmpLeS(FTMP, FTMP, src);
    } else {
      uint64_t min_val = (result_type == Primitive::kPrimLong)
          ? bit_cast<uint64_t, double>(std::numeric_limits<int64_t>::min())
          : bit_cast<uint64_t, double>(std::numeric_limits<int32_t>::min());
      __ LoadConst64(TMP, min_val);
      __ Dmtc1(TMP, FTMP);
      __ CmpLeD(FTMP, FTMP, src);
    }

    __ Bc1nez(FTMP, &truncate);

    if (input_type == Primitive::kPrimFloat) {
      __ CmpEqS(FTMP, src, src);
    } else {
      __ CmpEqD(FTMP, src, src);
    }
    if (result_type == Primitive::kPrimLong) {
      __ LoadConst64(dst, std::numeric_limits<int64_t>::min());
    } else {
      __ LoadConst32(dst, std::numeric_limits<int32_t>::min());
    }
    __ Mfc1(TMP, FTMP);
    __ And(dst, dst, TMP);

    __ Bc(&done);

    __ Bind(&truncate);

    if (result_type == Primitive::kPrimLong) {
      if (input_type == Primitive::kPrimFloat) {
        __ TruncLS(FTMP, src);
      } else {
        __ TruncLD(FTMP, src);
      }
      __ Dmfc1(dst, FTMP);
    } else {
      if (input_type == Primitive::kPrimFloat) {
        __ TruncWS(FTMP, src);
      } else {
        __ TruncWD(FTMP, src);
      }
      __ Mfc1(dst, FTMP);
    }

    __ Bind(&done);
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
  HandleCondition(comp);
}

void InstructionCodeGeneratorMIPS64::VisitEqual(HEqual* comp) {
  HandleCondition(comp);
}

void LocationsBuilderMIPS64::VisitNotEqual(HNotEqual* comp) {
  HandleCondition(comp);
}

void InstructionCodeGeneratorMIPS64::VisitNotEqual(HNotEqual* comp) {
  HandleCondition(comp);
}

void LocationsBuilderMIPS64::VisitLessThan(HLessThan* comp) {
  HandleCondition(comp);
}

void InstructionCodeGeneratorMIPS64::VisitLessThan(HLessThan* comp) {
  HandleCondition(comp);
}

void LocationsBuilderMIPS64::VisitLessThanOrEqual(HLessThanOrEqual* comp) {
  HandleCondition(comp);
}

void InstructionCodeGeneratorMIPS64::VisitLessThanOrEqual(HLessThanOrEqual* comp) {
  HandleCondition(comp);
}

void LocationsBuilderMIPS64::VisitGreaterThan(HGreaterThan* comp) {
  HandleCondition(comp);
}

void InstructionCodeGeneratorMIPS64::VisitGreaterThan(HGreaterThan* comp) {
  HandleCondition(comp);
}

void LocationsBuilderMIPS64::VisitGreaterThanOrEqual(HGreaterThanOrEqual* comp) {
  HandleCondition(comp);
}

void InstructionCodeGeneratorMIPS64::VisitGreaterThanOrEqual(HGreaterThanOrEqual* comp) {
  HandleCondition(comp);
}

void LocationsBuilderMIPS64::VisitBelow(HBelow* comp) {
  HandleCondition(comp);
}

void InstructionCodeGeneratorMIPS64::VisitBelow(HBelow* comp) {
  HandleCondition(comp);
}

void LocationsBuilderMIPS64::VisitBelowOrEqual(HBelowOrEqual* comp) {
  HandleCondition(comp);
}

void InstructionCodeGeneratorMIPS64::VisitBelowOrEqual(HBelowOrEqual* comp) {
  HandleCondition(comp);
}

void LocationsBuilderMIPS64::VisitAbove(HAbove* comp) {
  HandleCondition(comp);
}

void InstructionCodeGeneratorMIPS64::VisitAbove(HAbove* comp) {
  HandleCondition(comp);
}

void LocationsBuilderMIPS64::VisitAboveOrEqual(HAboveOrEqual* comp) {
  HandleCondition(comp);
}

void InstructionCodeGeneratorMIPS64::VisitAboveOrEqual(HAboveOrEqual* comp) {
  HandleCondition(comp);
}

// Simple implementation of packed switch - generate cascaded compare/jumps.
void LocationsBuilderMIPS64::VisitPackedSwitch(HPackedSwitch* switch_instr) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(switch_instr, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
}

void InstructionCodeGeneratorMIPS64::VisitPackedSwitch(HPackedSwitch* switch_instr) {
  int32_t lower_bound = switch_instr->GetStartValue();
  int32_t num_entries = switch_instr->GetNumEntries();
  LocationSummary* locations = switch_instr->GetLocations();
  GpuRegister value_reg = locations->InAt(0).AsRegister<GpuRegister>();
  HBasicBlock* default_block = switch_instr->GetDefaultBlock();

  // Create a set of compare/jumps.
  GpuRegister temp_reg = TMP;
  if (IsInt<16>(-lower_bound)) {
    __ Addiu(temp_reg, value_reg, -lower_bound);
  } else {
    __ LoadConst32(AT, -lower_bound);
    __ Addu(temp_reg, value_reg, AT);
  }
  // Jump to default if index is negative
  // Note: We don't check the case that index is positive while value < lower_bound, because in
  // this case, index >= num_entries must be true. So that we can save one branch instruction.
  __ Bltzc(temp_reg, codegen_->GetLabelOf(default_block));

  const ArenaVector<HBasicBlock*>& successors = switch_instr->GetBlock()->GetSuccessors();
  // Jump to successors[0] if value == lower_bound.
  __ Beqzc(temp_reg, codegen_->GetLabelOf(successors[0]));
  int32_t last_index = 0;
  for (; num_entries - last_index > 2; last_index += 2) {
    __ Addiu(temp_reg, temp_reg, -2);
    // Jump to successors[last_index + 1] if value < case_value[last_index + 2].
    __ Bltzc(temp_reg, codegen_->GetLabelOf(successors[last_index + 1]));
    // Jump to successors[last_index + 2] if value == case_value[last_index + 2].
    __ Beqzc(temp_reg, codegen_->GetLabelOf(successors[last_index + 2]));
  }
  if (num_entries - last_index == 2) {
    // The last missing case_value.
    __ Addiu(temp_reg, temp_reg, -1);
    __ Beqzc(temp_reg, codegen_->GetLabelOf(successors[last_index + 1]));
  }

  // And the default for any other value.
  if (!codegen_->GoesToNextBlock(switch_instr->GetBlock(), default_block)) {
    __ Bc(codegen_->GetLabelOf(default_block));
  }
}

void LocationsBuilderMIPS64::VisitClassTableGet(HClassTableGet*) {
  UNIMPLEMENTED(FATAL) << "ClassTableGet is unimplemented on mips64";
}

void InstructionCodeGeneratorMIPS64::VisitClassTableGet(HClassTableGet*) {
  UNIMPLEMENTED(FATAL) << "ClassTableGet is unimplemented on mips64";
}

}  // namespace mips64
}  // namespace art
