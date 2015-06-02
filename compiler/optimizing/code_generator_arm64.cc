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

#include "code_generator_arm64.h"

#include "arch/arm64/instruction_set_features_arm64.h"
#include "art_method.h"
#include "common_arm64.h"
#include "entrypoints/quick/quick_entrypoints.h"
#include "entrypoints/quick/quick_entrypoints_enum.h"
#include "gc/accounting/card_table.h"
#include "intrinsics.h"
#include "intrinsics_arm64.h"
#include "mirror/array-inl.h"
#include "mirror/class-inl.h"
#include "offsets.h"
#include "thread.h"
#include "utils/arm64/assembler_arm64.h"
#include "utils/assembler.h"
#include "utils/stack_checks.h"


using namespace vixl;   // NOLINT(build/namespaces)

#ifdef __
#error "ARM64 Codegen VIXL macro-assembler macro already defined."
#endif

namespace art {

namespace arm64 {

using helpers::CPURegisterFrom;
using helpers::DRegisterFrom;
using helpers::FPRegisterFrom;
using helpers::HeapOperand;
using helpers::HeapOperandFrom;
using helpers::InputCPURegisterAt;
using helpers::InputFPRegisterAt;
using helpers::InputRegisterAt;
using helpers::InputOperandAt;
using helpers::Int64ConstantFrom;
using helpers::LocationFrom;
using helpers::OperandFromMemOperand;
using helpers::OutputCPURegister;
using helpers::OutputFPRegister;
using helpers::OutputRegister;
using helpers::RegisterFrom;
using helpers::StackOperandFrom;
using helpers::VIXLRegCodeFromART;
using helpers::WRegisterFrom;
using helpers::XRegisterFrom;
using helpers::ARM64EncodableConstantOrRegister;

static constexpr int kCurrentMethodStackOffset = 0;

inline Condition ARM64Condition(IfCondition cond) {
  switch (cond) {
    case kCondEQ: return eq;
    case kCondNE: return ne;
    case kCondLT: return lt;
    case kCondLE: return le;
    case kCondGT: return gt;
    case kCondGE: return ge;
    default:
      LOG(FATAL) << "Unknown if condition";
  }
  return nv;  // Unreachable.
}

Location ARM64ReturnLocation(Primitive::Type return_type) {
  DCHECK_NE(return_type, Primitive::kPrimVoid);
  // Note that in practice, `LocationFrom(x0)` and `LocationFrom(w0)` create the
  // same Location object, and so do `LocationFrom(d0)` and `LocationFrom(s0)`,
  // but we use the exact registers for clarity.
  if (return_type == Primitive::kPrimFloat) {
    return LocationFrom(s0);
  } else if (return_type == Primitive::kPrimDouble) {
    return LocationFrom(d0);
  } else if (return_type == Primitive::kPrimLong) {
    return LocationFrom(x0);
  } else {
    return LocationFrom(w0);
  }
}

Location InvokeRuntimeCallingConvention::GetReturnLocation(Primitive::Type return_type) {
  return ARM64ReturnLocation(return_type);
}

#define __ down_cast<CodeGeneratorARM64*>(codegen)->GetVIXLAssembler()->
#define QUICK_ENTRY_POINT(x) QUICK_ENTRYPOINT_OFFSET(kArm64WordSize, x).Int32Value()

class BoundsCheckSlowPathARM64 : public SlowPathCodeARM64 {
 public:
  BoundsCheckSlowPathARM64(HBoundsCheck* instruction,
                           Location index_location,
                           Location length_location)
      : instruction_(instruction),
        index_location_(index_location),
        length_location_(length_location) {}


  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    CodeGeneratorARM64* arm64_codegen = down_cast<CodeGeneratorARM64*>(codegen);
    __ Bind(GetEntryLabel());
    // We're moving two locations to locations that could overlap, so we need a parallel
    // move resolver.
    InvokeRuntimeCallingConvention calling_convention;
    codegen->EmitParallelMoves(
        index_location_, LocationFrom(calling_convention.GetRegisterAt(0)), Primitive::kPrimInt,
        length_location_, LocationFrom(calling_convention.GetRegisterAt(1)), Primitive::kPrimInt);
    arm64_codegen->InvokeRuntime(
        QUICK_ENTRY_POINT(pThrowArrayBounds), instruction_, instruction_->GetDexPc(), this);
    CheckEntrypointTypes<kQuickThrowArrayBounds, void, int32_t, int32_t>();
  }

 private:
  HBoundsCheck* const instruction_;
  const Location index_location_;
  const Location length_location_;

  DISALLOW_COPY_AND_ASSIGN(BoundsCheckSlowPathARM64);
};

class DivZeroCheckSlowPathARM64 : public SlowPathCodeARM64 {
 public:
  explicit DivZeroCheckSlowPathARM64(HDivZeroCheck* instruction) : instruction_(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    CodeGeneratorARM64* arm64_codegen = down_cast<CodeGeneratorARM64*>(codegen);
    __ Bind(GetEntryLabel());
    arm64_codegen->InvokeRuntime(
        QUICK_ENTRY_POINT(pThrowDivZero), instruction_, instruction_->GetDexPc(), this);
    CheckEntrypointTypes<kQuickThrowDivZero, void, void>();
  }

 private:
  HDivZeroCheck* const instruction_;
  DISALLOW_COPY_AND_ASSIGN(DivZeroCheckSlowPathARM64);
};

class LoadClassSlowPathARM64 : public SlowPathCodeARM64 {
 public:
  LoadClassSlowPathARM64(HLoadClass* cls,
                         HInstruction* at,
                         uint32_t dex_pc,
                         bool do_clinit)
      : cls_(cls), at_(at), dex_pc_(dex_pc), do_clinit_(do_clinit) {
    DCHECK(at->IsLoadClass() || at->IsClinitCheck());
  }

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    LocationSummary* locations = at_->GetLocations();
    CodeGeneratorARM64* arm64_codegen = down_cast<CodeGeneratorARM64*>(codegen);

    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, locations);

    InvokeRuntimeCallingConvention calling_convention;
    __ Mov(calling_convention.GetRegisterAt(0).W(), cls_->GetTypeIndex());
    int32_t entry_point_offset = do_clinit_ ? QUICK_ENTRY_POINT(pInitializeStaticStorage)
                                            : QUICK_ENTRY_POINT(pInitializeType);
    arm64_codegen->InvokeRuntime(entry_point_offset, at_, dex_pc_, this);
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
      arm64_codegen->MoveLocation(out, calling_convention.GetReturnLocation(type), type);
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

  DISALLOW_COPY_AND_ASSIGN(LoadClassSlowPathARM64);
};

class LoadStringSlowPathARM64 : public SlowPathCodeARM64 {
 public:
  explicit LoadStringSlowPathARM64(HLoadString* instruction) : instruction_(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    LocationSummary* locations = instruction_->GetLocations();
    DCHECK(!locations->GetLiveRegisters()->ContainsCoreRegister(locations->Out().reg()));
    CodeGeneratorARM64* arm64_codegen = down_cast<CodeGeneratorARM64*>(codegen);

    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, locations);

    InvokeRuntimeCallingConvention calling_convention;
    __ Mov(calling_convention.GetRegisterAt(0).W(), instruction_->GetStringIndex());
    arm64_codegen->InvokeRuntime(
        QUICK_ENTRY_POINT(pResolveString), instruction_, instruction_->GetDexPc(), this);
    CheckEntrypointTypes<kQuickResolveString, void*, uint32_t>();
    Primitive::Type type = instruction_->GetType();
    arm64_codegen->MoveLocation(locations->Out(), calling_convention.GetReturnLocation(type), type);

    RestoreLiveRegisters(codegen, locations);
    __ B(GetExitLabel());
  }

 private:
  HLoadString* const instruction_;

  DISALLOW_COPY_AND_ASSIGN(LoadStringSlowPathARM64);
};

class NullCheckSlowPathARM64 : public SlowPathCodeARM64 {
 public:
  explicit NullCheckSlowPathARM64(HNullCheck* instr) : instruction_(instr) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    CodeGeneratorARM64* arm64_codegen = down_cast<CodeGeneratorARM64*>(codegen);
    __ Bind(GetEntryLabel());
    arm64_codegen->InvokeRuntime(
        QUICK_ENTRY_POINT(pThrowNullPointer), instruction_, instruction_->GetDexPc(), this);
    CheckEntrypointTypes<kQuickThrowNullPointer, void, void>();
  }

 private:
  HNullCheck* const instruction_;

  DISALLOW_COPY_AND_ASSIGN(NullCheckSlowPathARM64);
};

class SuspendCheckSlowPathARM64 : public SlowPathCodeARM64 {
 public:
  explicit SuspendCheckSlowPathARM64(HSuspendCheck* instruction,
                                     HBasicBlock* successor)
      : instruction_(instruction), successor_(successor) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    CodeGeneratorARM64* arm64_codegen = down_cast<CodeGeneratorARM64*>(codegen);
    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, instruction_->GetLocations());
    arm64_codegen->InvokeRuntime(
        QUICK_ENTRY_POINT(pTestSuspend), instruction_, instruction_->GetDexPc(), this);
    CheckEntrypointTypes<kQuickTestSuspend, void, void>();
    RestoreLiveRegisters(codegen, instruction_->GetLocations());
    if (successor_ == nullptr) {
      __ B(GetReturnLabel());
    } else {
      __ B(arm64_codegen->GetLabelOf(successor_));
    }
  }

  vixl::Label* GetReturnLabel() {
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
  vixl::Label return_label_;

  DISALLOW_COPY_AND_ASSIGN(SuspendCheckSlowPathARM64);
};

class TypeCheckSlowPathARM64 : public SlowPathCodeARM64 {
 public:
  TypeCheckSlowPathARM64(HInstruction* instruction,
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
    CodeGeneratorARM64* arm64_codegen = down_cast<CodeGeneratorARM64*>(codegen);

    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, locations);

    // We're moving two locations to locations that could overlap, so we need a parallel
    // move resolver.
    InvokeRuntimeCallingConvention calling_convention;
    codegen->EmitParallelMoves(
        class_to_check_, LocationFrom(calling_convention.GetRegisterAt(0)), Primitive::kPrimNot,
        object_class_, LocationFrom(calling_convention.GetRegisterAt(1)), Primitive::kPrimNot);

    if (instruction_->IsInstanceOf()) {
      arm64_codegen->InvokeRuntime(
          QUICK_ENTRY_POINT(pInstanceofNonTrivial), instruction_, dex_pc_, this);
      Primitive::Type ret_type = instruction_->GetType();
      Location ret_loc = calling_convention.GetReturnLocation(ret_type);
      arm64_codegen->MoveLocation(locations->Out(), ret_loc, ret_type);
      CheckEntrypointTypes<kQuickInstanceofNonTrivial, uint32_t,
                           const mirror::Class*, const mirror::Class*>();
    } else {
      DCHECK(instruction_->IsCheckCast());
      arm64_codegen->InvokeRuntime(QUICK_ENTRY_POINT(pCheckCast), instruction_, dex_pc_, this);
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

  DISALLOW_COPY_AND_ASSIGN(TypeCheckSlowPathARM64);
};

class DeoptimizationSlowPathARM64 : public SlowPathCodeARM64 {
 public:
  explicit DeoptimizationSlowPathARM64(HInstruction* instruction)
    : instruction_(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, instruction_->GetLocations());
    DCHECK(instruction_->IsDeoptimize());
    HDeoptimize* deoptimize = instruction_->AsDeoptimize();
    uint32_t dex_pc = deoptimize->GetDexPc();
    CodeGeneratorARM64* arm64_codegen = down_cast<CodeGeneratorARM64*>(codegen);
    arm64_codegen->InvokeRuntime(QUICK_ENTRY_POINT(pDeoptimize), instruction_, dex_pc, this);
  }

 private:
  HInstruction* const instruction_;
  DISALLOW_COPY_AND_ASSIGN(DeoptimizationSlowPathARM64);
};

#undef __

Location InvokeDexCallingConventionVisitorARM64::GetNextLocation(Primitive::Type type) {
  Location next_location;
  if (type == Primitive::kPrimVoid) {
    LOG(FATAL) << "Unreachable type " << type;
  }

  if (Primitive::IsFloatingPointType(type) &&
      (float_index_ < calling_convention.GetNumberOfFpuRegisters())) {
    next_location = LocationFrom(calling_convention.GetFpuRegisterAt(float_index_++));
  } else if (!Primitive::IsFloatingPointType(type) &&
             (gp_index_ < calling_convention.GetNumberOfRegisters())) {
    next_location = LocationFrom(calling_convention.GetRegisterAt(gp_index_++));
  } else {
    size_t stack_offset = calling_convention.GetStackOffsetOf(stack_index_);
    next_location = Primitive::Is64BitType(type) ? Location::DoubleStackSlot(stack_offset)
                                                 : Location::StackSlot(stack_offset);
  }

  // Space on the stack is reserved for all arguments.
  stack_index_ += Primitive::Is64BitType(type) ? 2 : 1;
  return next_location;
}

CodeGeneratorARM64::CodeGeneratorARM64(HGraph* graph,
                                       const Arm64InstructionSetFeatures& isa_features,
                                       const CompilerOptions& compiler_options)
    : CodeGenerator(graph,
                    kNumberOfAllocatableRegisters,
                    kNumberOfAllocatableFPRegisters,
                    kNumberOfAllocatableRegisterPairs,
                    callee_saved_core_registers.list(),
                    callee_saved_fp_registers.list(),
                    compiler_options),
      block_labels_(nullptr),
      location_builder_(graph, this),
      instruction_visitor_(graph, this),
      move_resolver_(graph->GetArena(), this),
      isa_features_(isa_features) {
  // Save the link register (containing the return address) to mimic Quick.
  AddAllocatedRegister(LocationFrom(lr));
}

#undef __
#define __ GetVIXLAssembler()->

void CodeGeneratorARM64::Finalize(CodeAllocator* allocator) {
  // Ensure we emit the literal pool.
  __ FinalizeCode();
  CodeGenerator::Finalize(allocator);
}

void ParallelMoveResolverARM64::PrepareForEmitNativeCode() {
  // Note: There are 6 kinds of moves:
  // 1. constant -> GPR/FPR (non-cycle)
  // 2. constant -> stack (non-cycle)
  // 3. GPR/FPR -> GPR/FPR
  // 4. GPR/FPR -> stack
  // 5. stack -> GPR/FPR
  // 6. stack -> stack (non-cycle)
  // Case 1, 2 and 6 should never be included in a dependency cycle on ARM64. For case 3, 4, and 5
  // VIXL uses at most 1 GPR. VIXL has 2 GPR and 1 FPR temps, and there should be no intersecting
  // cycles on ARM64, so we always have 1 GPR and 1 FPR available VIXL temps to resolve the
  // dependency.
  vixl_temps_.Open(GetVIXLAssembler());
}

void ParallelMoveResolverARM64::FinishEmitNativeCode() {
  vixl_temps_.Close();
}

Location ParallelMoveResolverARM64::AllocateScratchLocationFor(Location::Kind kind) {
  DCHECK(kind == Location::kRegister || kind == Location::kFpuRegister ||
         kind == Location::kStackSlot || kind == Location::kDoubleStackSlot);
  kind = (kind == Location::kFpuRegister) ? Location::kFpuRegister : Location::kRegister;
  Location scratch = GetScratchLocation(kind);
  if (!scratch.Equals(Location::NoLocation())) {
    return scratch;
  }
  // Allocate from VIXL temp registers.
  if (kind == Location::kRegister) {
    scratch = LocationFrom(vixl_temps_.AcquireX());
  } else {
    DCHECK(kind == Location::kFpuRegister);
    scratch = LocationFrom(vixl_temps_.AcquireD());
  }
  AddScratchLocation(scratch);
  return scratch;
}

void ParallelMoveResolverARM64::FreeScratchLocation(Location loc) {
  if (loc.IsRegister()) {
    vixl_temps_.Release(XRegisterFrom(loc));
  } else {
    DCHECK(loc.IsFpuRegister());
    vixl_temps_.Release(DRegisterFrom(loc));
  }
  RemoveScratchLocation(loc);
}

void ParallelMoveResolverARM64::EmitMove(size_t index) {
  MoveOperands* move = moves_.Get(index);
  codegen_->MoveLocation(move->GetDestination(), move->GetSource());
}

void CodeGeneratorARM64::GenerateFrameEntry() {
  MacroAssembler* masm = GetVIXLAssembler();
  BlockPoolsScope block_pools(masm);
  __ Bind(&frame_entry_label_);

  bool do_overflow_check = FrameNeedsStackCheck(GetFrameSize(), kArm64) || !IsLeafMethod();
  if (do_overflow_check) {
    UseScratchRegisterScope temps(masm);
    Register temp = temps.AcquireX();
    DCHECK(GetCompilerOptions().GetImplicitStackOverflowChecks());
    __ Sub(temp, sp, static_cast<int32_t>(GetStackOverflowReservedBytes(kArm64)));
    __ Ldr(wzr, MemOperand(temp, 0));
    RecordPcInfo(nullptr, 0);
  }

  if (!HasEmptyFrame()) {
    int frame_size = GetFrameSize();
    // Stack layout:
    //      sp[frame_size - 8]        : lr.
    //      ...                       : other preserved core registers.
    //      ...                       : other preserved fp registers.
    //      ...                       : reserved frame space.
    //      sp[0]                     : current method.
    __ Str(kArtMethodRegister, MemOperand(sp, -frame_size, PreIndex));
    GetAssembler()->cfi().AdjustCFAOffset(frame_size);
    GetAssembler()->SpillRegisters(GetFramePreservedCoreRegisters(),
        frame_size - GetCoreSpillSize());
    GetAssembler()->SpillRegisters(GetFramePreservedFPRegisters(),
        frame_size - FrameEntrySpillSize());
  }
}

void CodeGeneratorARM64::GenerateFrameExit() {
  BlockPoolsScope block_pools(GetVIXLAssembler());
  GetAssembler()->cfi().RememberState();
  if (!HasEmptyFrame()) {
    int frame_size = GetFrameSize();
    GetAssembler()->UnspillRegisters(GetFramePreservedFPRegisters(),
        frame_size - FrameEntrySpillSize());
    GetAssembler()->UnspillRegisters(GetFramePreservedCoreRegisters(),
        frame_size - GetCoreSpillSize());
    __ Drop(frame_size);
    GetAssembler()->cfi().AdjustCFAOffset(-frame_size);
  }
  __ Ret();
  GetAssembler()->cfi().RestoreState();
  GetAssembler()->cfi().DefCFAOffset(GetFrameSize());
}

void CodeGeneratorARM64::Bind(HBasicBlock* block) {
  __ Bind(GetLabelOf(block));
}

void CodeGeneratorARM64::Move(HInstruction* instruction,
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
    int64_t value = GetInt64ValueOf(instruction->AsConstant());
    if (location.IsRegister()) {
      Register dst = RegisterFrom(location, type);
      DCHECK(((instruction->IsIntConstant() || instruction->IsNullConstant()) && dst.Is32Bits()) ||
             (instruction->IsLongConstant() && dst.Is64Bits()));
      __ Mov(dst, value);
    } else {
      DCHECK(location.IsStackSlot() || location.IsDoubleStackSlot());
      UseScratchRegisterScope temps(GetVIXLAssembler());
      Register temp = (instruction->IsIntConstant() || instruction->IsNullConstant())
          ? temps.AcquireW()
          : temps.AcquireX();
      __ Mov(temp, value);
      __ Str(temp, StackOperandFrom(location));
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

Location CodeGeneratorARM64::GetStackLocation(HLoadLocal* load) const {
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

void CodeGeneratorARM64::MarkGCCard(Register object, Register value) {
  UseScratchRegisterScope temps(GetVIXLAssembler());
  Register card = temps.AcquireX();
  Register temp = temps.AcquireW();   // Index within the CardTable - 32bit.
  vixl::Label done;
  __ Cbz(value, &done);
  __ Ldr(card, MemOperand(tr, Thread::CardTableOffset<kArm64WordSize>().Int32Value()));
  __ Lsr(temp, object, gc::accounting::CardTable::kCardShift);
  __ Strb(card, MemOperand(card, temp.X()));
  __ Bind(&done);
}

void CodeGeneratorARM64::SetupBlockedRegisters(bool is_baseline) const {
  // Blocked core registers:
  //      lr        : Runtime reserved.
  //      tr        : Runtime reserved.
  //      xSuspend  : Runtime reserved. TODO: Unblock this when the runtime stops using it.
  //      ip1       : VIXL core temp.
  //      ip0       : VIXL core temp.
  //
  // Blocked fp registers:
  //      d31       : VIXL fp temp.
  CPURegList reserved_core_registers = vixl_reserved_core_registers;
  reserved_core_registers.Combine(runtime_reserved_core_registers);
  while (!reserved_core_registers.IsEmpty()) {
    blocked_core_registers_[reserved_core_registers.PopLowestIndex().code()] = true;
  }

  CPURegList reserved_fp_registers = vixl_reserved_fp_registers;
  while (!reserved_fp_registers.IsEmpty()) {
    blocked_fpu_registers_[reserved_fp_registers.PopLowestIndex().code()] = true;
  }

  if (is_baseline) {
    CPURegList reserved_core_baseline_registers = callee_saved_core_registers;
    while (!reserved_core_baseline_registers.IsEmpty()) {
      blocked_core_registers_[reserved_core_baseline_registers.PopLowestIndex().code()] = true;
    }

    CPURegList reserved_fp_baseline_registers = callee_saved_fp_registers;
    while (!reserved_fp_baseline_registers.IsEmpty()) {
      blocked_fpu_registers_[reserved_fp_baseline_registers.PopLowestIndex().code()] = true;
    }
  }
}

Location CodeGeneratorARM64::AllocateFreeRegister(Primitive::Type type) const {
  if (type == Primitive::kPrimVoid) {
    LOG(FATAL) << "Unreachable type " << type;
  }

  if (Primitive::IsFloatingPointType(type)) {
    ssize_t reg = FindFreeEntry(blocked_fpu_registers_, kNumberOfAllocatableFPRegisters);
    DCHECK_NE(reg, -1);
    return Location::FpuRegisterLocation(reg);
  } else {
    ssize_t reg = FindFreeEntry(blocked_core_registers_, kNumberOfAllocatableRegisters);
    DCHECK_NE(reg, -1);
    return Location::RegisterLocation(reg);
  }
}

size_t CodeGeneratorARM64::SaveCoreRegister(size_t stack_index, uint32_t reg_id) {
  Register reg = Register(VIXLRegCodeFromART(reg_id), kXRegSize);
  __ Str(reg, MemOperand(sp, stack_index));
  return kArm64WordSize;
}

size_t CodeGeneratorARM64::RestoreCoreRegister(size_t stack_index, uint32_t reg_id) {
  Register reg = Register(VIXLRegCodeFromART(reg_id), kXRegSize);
  __ Ldr(reg, MemOperand(sp, stack_index));
  return kArm64WordSize;
}

size_t CodeGeneratorARM64::SaveFloatingPointRegister(size_t stack_index, uint32_t reg_id) {
  FPRegister reg = FPRegister(reg_id, kDRegSize);
  __ Str(reg, MemOperand(sp, stack_index));
  return kArm64WordSize;
}

size_t CodeGeneratorARM64::RestoreFloatingPointRegister(size_t stack_index, uint32_t reg_id) {
  FPRegister reg = FPRegister(reg_id, kDRegSize);
  __ Ldr(reg, MemOperand(sp, stack_index));
  return kArm64WordSize;
}

void CodeGeneratorARM64::DumpCoreRegister(std::ostream& stream, int reg) const {
  stream << Arm64ManagedRegister::FromXRegister(XRegister(reg));
}

void CodeGeneratorARM64::DumpFloatingPointRegister(std::ostream& stream, int reg) const {
  stream << Arm64ManagedRegister::FromDRegister(DRegister(reg));
}

void CodeGeneratorARM64::MoveConstant(CPURegister destination, HConstant* constant) {
  if (constant->IsIntConstant()) {
    __ Mov(Register(destination), constant->AsIntConstant()->GetValue());
  } else if (constant->IsLongConstant()) {
    __ Mov(Register(destination), constant->AsLongConstant()->GetValue());
  } else if (constant->IsNullConstant()) {
    __ Mov(Register(destination), 0);
  } else if (constant->IsFloatConstant()) {
    __ Fmov(FPRegister(destination), constant->AsFloatConstant()->GetValue());
  } else {
    DCHECK(constant->IsDoubleConstant());
    __ Fmov(FPRegister(destination), constant->AsDoubleConstant()->GetValue());
  }
}


static bool CoherentConstantAndType(Location constant, Primitive::Type type) {
  DCHECK(constant.IsConstant());
  HConstant* cst = constant.GetConstant();
  return (cst->IsIntConstant() && type == Primitive::kPrimInt) ||
         // Null is mapped to a core W register, which we associate with kPrimInt.
         (cst->IsNullConstant() && type == Primitive::kPrimInt) ||
         (cst->IsLongConstant() && type == Primitive::kPrimLong) ||
         (cst->IsFloatConstant() && type == Primitive::kPrimFloat) ||
         (cst->IsDoubleConstant() && type == Primitive::kPrimDouble);
}

void CodeGeneratorARM64::MoveLocation(Location destination, Location source, Primitive::Type type) {
  if (source.Equals(destination)) {
    return;
  }

  // A valid move can always be inferred from the destination and source
  // locations. When moving from and to a register, the argument type can be
  // used to generate 32bit instead of 64bit moves. In debug mode we also
  // checks the coherency of the locations and the type.
  bool unspecified_type = (type == Primitive::kPrimVoid);

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
    CPURegister dst = CPURegisterFrom(destination, type);
    if (source.IsStackSlot() || source.IsDoubleStackSlot()) {
      DCHECK(dst.Is64Bits() == source.IsDoubleStackSlot());
      __ Ldr(dst, StackOperandFrom(source));
    } else if (source.IsConstant()) {
      DCHECK(CoherentConstantAndType(source, type));
      MoveConstant(dst, source.GetConstant());
    } else {
      if (destination.IsRegister()) {
        __ Mov(Register(dst), RegisterFrom(source, type));
      } else {
        DCHECK(destination.IsFpuRegister());
        __ Fmov(FPRegister(dst), FPRegisterFrom(source, type));
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
      __ Str(CPURegisterFrom(source, type), StackOperandFrom(destination));
    } else if (source.IsConstant()) {
      DCHECK(unspecified_type || CoherentConstantAndType(source, type));
      UseScratchRegisterScope temps(GetVIXLAssembler());
      HConstant* src_cst = source.GetConstant();
      CPURegister temp;
      if (src_cst->IsIntConstant() || src_cst->IsNullConstant()) {
        temp = temps.AcquireW();
      } else if (src_cst->IsLongConstant()) {
        temp = temps.AcquireX();
      } else if (src_cst->IsFloatConstant()) {
        temp = temps.AcquireS();
      } else {
        DCHECK(src_cst->IsDoubleConstant());
        temp = temps.AcquireD();
      }
      MoveConstant(temp, src_cst);
      __ Str(temp, StackOperandFrom(destination));
    } else {
      DCHECK(source.IsStackSlot() || source.IsDoubleStackSlot());
      DCHECK(source.IsDoubleStackSlot() == destination.IsDoubleStackSlot());
      UseScratchRegisterScope temps(GetVIXLAssembler());
      // There is generally less pressure on FP registers.
      FPRegister temp = destination.IsDoubleStackSlot() ? temps.AcquireD() : temps.AcquireS();
      __ Ldr(temp, StackOperandFrom(source));
      __ Str(temp, StackOperandFrom(destination));
    }
  }
}

void CodeGeneratorARM64::Load(Primitive::Type type,
                              CPURegister dst,
                              const MemOperand& src) {
  switch (type) {
    case Primitive::kPrimBoolean:
      __ Ldrb(Register(dst), src);
      break;
    case Primitive::kPrimByte:
      __ Ldrsb(Register(dst), src);
      break;
    case Primitive::kPrimShort:
      __ Ldrsh(Register(dst), src);
      break;
    case Primitive::kPrimChar:
      __ Ldrh(Register(dst), src);
      break;
    case Primitive::kPrimInt:
    case Primitive::kPrimNot:
    case Primitive::kPrimLong:
    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      DCHECK_EQ(dst.Is64Bits(), Primitive::Is64BitType(type));
      __ Ldr(dst, src);
      break;
    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << type;
  }
}

void CodeGeneratorARM64::LoadAcquire(HInstruction* instruction,
                                     CPURegister dst,
                                     const MemOperand& src) {
  MacroAssembler* masm = GetVIXLAssembler();
  BlockPoolsScope block_pools(masm);
  UseScratchRegisterScope temps(masm);
  Register temp_base = temps.AcquireX();
  Primitive::Type type = instruction->GetType();

  DCHECK(!src.IsPreIndex());
  DCHECK(!src.IsPostIndex());

  // TODO(vixl): Let the MacroAssembler handle MemOperand.
  __ Add(temp_base, src.base(), OperandFromMemOperand(src));
  MemOperand base = MemOperand(temp_base);
  switch (type) {
    case Primitive::kPrimBoolean:
      __ Ldarb(Register(dst), base);
      MaybeRecordImplicitNullCheck(instruction);
      break;
    case Primitive::kPrimByte:
      __ Ldarb(Register(dst), base);
      MaybeRecordImplicitNullCheck(instruction);
      __ Sbfx(Register(dst), Register(dst), 0, Primitive::ComponentSize(type) * kBitsPerByte);
      break;
    case Primitive::kPrimChar:
      __ Ldarh(Register(dst), base);
      MaybeRecordImplicitNullCheck(instruction);
      break;
    case Primitive::kPrimShort:
      __ Ldarh(Register(dst), base);
      MaybeRecordImplicitNullCheck(instruction);
      __ Sbfx(Register(dst), Register(dst), 0, Primitive::ComponentSize(type) * kBitsPerByte);
      break;
    case Primitive::kPrimInt:
    case Primitive::kPrimNot:
    case Primitive::kPrimLong:
      DCHECK_EQ(dst.Is64Bits(), Primitive::Is64BitType(type));
      __ Ldar(Register(dst), base);
      MaybeRecordImplicitNullCheck(instruction);
      break;
    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      DCHECK(dst.IsFPRegister());
      DCHECK_EQ(dst.Is64Bits(), Primitive::Is64BitType(type));

      Register temp = dst.Is64Bits() ? temps.AcquireX() : temps.AcquireW();
      __ Ldar(temp, base);
      MaybeRecordImplicitNullCheck(instruction);
      __ Fmov(FPRegister(dst), temp);
      break;
    }
    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << type;
  }
}

void CodeGeneratorARM64::Store(Primitive::Type type,
                               CPURegister src,
                               const MemOperand& dst) {
  switch (type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
      __ Strb(Register(src), dst);
      break;
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
      __ Strh(Register(src), dst);
      break;
    case Primitive::kPrimInt:
    case Primitive::kPrimNot:
    case Primitive::kPrimLong:
    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      DCHECK_EQ(src.Is64Bits(), Primitive::Is64BitType(type));
      __ Str(src, dst);
      break;
    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << type;
  }
}

void CodeGeneratorARM64::StoreRelease(Primitive::Type type,
                                      CPURegister src,
                                      const MemOperand& dst) {
  UseScratchRegisterScope temps(GetVIXLAssembler());
  Register temp_base = temps.AcquireX();

  DCHECK(!dst.IsPreIndex());
  DCHECK(!dst.IsPostIndex());

  // TODO(vixl): Let the MacroAssembler handle this.
  Operand op = OperandFromMemOperand(dst);
  __ Add(temp_base, dst.base(), op);
  MemOperand base = MemOperand(temp_base);
  switch (type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
      __ Stlrb(Register(src), base);
      break;
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
      __ Stlrh(Register(src), base);
      break;
    case Primitive::kPrimInt:
    case Primitive::kPrimNot:
    case Primitive::kPrimLong:
      DCHECK_EQ(src.Is64Bits(), Primitive::Is64BitType(type));
      __ Stlr(Register(src), base);
      break;
    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      DCHECK(src.IsFPRegister());
      DCHECK_EQ(src.Is64Bits(), Primitive::Is64BitType(type));

      Register temp = src.Is64Bits() ? temps.AcquireX() : temps.AcquireW();
      __ Fmov(temp, FPRegister(src));
      __ Stlr(temp, base);
      break;
    }
    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << type;
  }
}

void CodeGeneratorARM64::LoadCurrentMethod(vixl::Register current_method) {
  DCHECK(RequiresCurrentMethod());
  CHECK(current_method.IsX());
  __ Ldr(current_method, MemOperand(sp, kCurrentMethodStackOffset));
}

void CodeGeneratorARM64::InvokeRuntime(int32_t entry_point_offset,
                                       HInstruction* instruction,
                                       uint32_t dex_pc,
                                       SlowPathCode* slow_path) {
  BlockPoolsScope block_pools(GetVIXLAssembler());
  __ Ldr(lr, MemOperand(tr, entry_point_offset));
  __ Blr(lr);
  if (instruction != nullptr) {
    RecordPcInfo(instruction, dex_pc, slow_path);
    DCHECK(instruction->IsSuspendCheck()
        || instruction->IsBoundsCheck()
        || instruction->IsNullCheck()
        || instruction->IsDivZeroCheck()
        || !IsLeafMethod());
    }
}

void InstructionCodeGeneratorARM64::GenerateClassInitializationCheck(SlowPathCodeARM64* slow_path,
                                                                     vixl::Register class_reg) {
  UseScratchRegisterScope temps(GetVIXLAssembler());
  Register temp = temps.AcquireW();
  size_t status_offset = mirror::Class::StatusOffset().SizeValue();
  bool use_acquire_release = codegen_->GetInstructionSetFeatures().PreferAcquireRelease();

  // Even if the initialized flag is set, we need to ensure consistent memory ordering.
  if (use_acquire_release) {
    // TODO(vixl): Let the MacroAssembler handle MemOperand.
    __ Add(temp, class_reg, status_offset);
    __ Ldar(temp, HeapOperand(temp));
    __ Cmp(temp, mirror::Class::kStatusInitialized);
    __ B(lt, slow_path->GetEntryLabel());
  } else {
    __ Ldr(temp, HeapOperand(class_reg, status_offset));
    __ Cmp(temp, mirror::Class::kStatusInitialized);
    __ B(lt, slow_path->GetEntryLabel());
    __ Dmb(InnerShareable, BarrierReads);
  }
  __ Bind(slow_path->GetExitLabel());
}

void InstructionCodeGeneratorARM64::GenerateMemoryBarrier(MemBarrierKind kind) {
  BarrierType type = BarrierAll;

  switch (kind) {
    case MemBarrierKind::kAnyAny:
    case MemBarrierKind::kAnyStore: {
      type = BarrierAll;
      break;
    }
    case MemBarrierKind::kLoadAny: {
      type = BarrierReads;
      break;
    }
    case MemBarrierKind::kStoreStore: {
      type = BarrierWrites;
      break;
    }
    default:
      LOG(FATAL) << "Unexpected memory barrier " << kind;
  }
  __ Dmb(InnerShareable, type);
}

void InstructionCodeGeneratorARM64::GenerateSuspendCheck(HSuspendCheck* instruction,
                                                         HBasicBlock* successor) {
  SuspendCheckSlowPathARM64* slow_path =
      down_cast<SuspendCheckSlowPathARM64*>(instruction->GetSlowPath());
  if (slow_path == nullptr) {
    slow_path = new (GetGraph()->GetArena()) SuspendCheckSlowPathARM64(instruction, successor);
    instruction->SetSlowPath(slow_path);
    codegen_->AddSlowPath(slow_path);
    if (successor != nullptr) {
      DCHECK(successor->IsLoopHeader());
      codegen_->ClearSpillSlotsFromLoopPhisInStackMap(instruction);
    }
  } else {
    DCHECK_EQ(slow_path->GetSuccessor(), successor);
  }

  UseScratchRegisterScope temps(codegen_->GetVIXLAssembler());
  Register temp = temps.AcquireW();

  __ Ldrh(temp, MemOperand(tr, Thread::ThreadFlagsOffset<kArm64WordSize>().SizeValue()));
  if (successor == nullptr) {
    __ Cbnz(temp, slow_path->GetEntryLabel());
    __ Bind(slow_path->GetReturnLabel());
  } else {
    __ Cbz(temp, codegen_->GetLabelOf(successor));
    __ B(slow_path->GetEntryLabel());
    // slow_path will return to GetLabelOf(successor).
  }
}

InstructionCodeGeneratorARM64::InstructionCodeGeneratorARM64(HGraph* graph,
                                                             CodeGeneratorARM64* codegen)
      : HGraphVisitor(graph),
        assembler_(codegen->GetAssembler()),
        codegen_(codegen) {}

#define FOR_EACH_UNIMPLEMENTED_INSTRUCTION(M)              \
  /* No unimplemented IR. */

#define UNIMPLEMENTED_INSTRUCTION_BREAK_CODE(name) name##UnimplementedInstructionBreakCode

enum UnimplementedInstructionBreakCode {
  // Using a base helps identify when we hit such breakpoints.
  UnimplementedInstructionBreakCodeBaseCode = 0x900,
#define ENUM_UNIMPLEMENTED_INSTRUCTION(name) UNIMPLEMENTED_INSTRUCTION_BREAK_CODE(name),
  FOR_EACH_UNIMPLEMENTED_INSTRUCTION(ENUM_UNIMPLEMENTED_INSTRUCTION)
#undef ENUM_UNIMPLEMENTED_INSTRUCTION
};

#define DEFINE_UNIMPLEMENTED_INSTRUCTION_VISITORS(name)                               \
  void InstructionCodeGeneratorARM64::Visit##name(H##name* instr) {                   \
    UNUSED(instr);                                                                    \
    __ Brk(UNIMPLEMENTED_INSTRUCTION_BREAK_CODE(name));                               \
  }                                                                                   \
  void LocationsBuilderARM64::Visit##name(H##name* instr) {                           \
    LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instr); \
    locations->SetOut(Location::Any());                                               \
  }
  FOR_EACH_UNIMPLEMENTED_INSTRUCTION(DEFINE_UNIMPLEMENTED_INSTRUCTION_VISITORS)
#undef DEFINE_UNIMPLEMENTED_INSTRUCTION_VISITORS

#undef UNIMPLEMENTED_INSTRUCTION_BREAK_CODE
#undef FOR_EACH_UNIMPLEMENTED_INSTRUCTION

void LocationsBuilderARM64::HandleBinaryOp(HBinaryOperation* instr) {
  DCHECK_EQ(instr->InputCount(), 2U);
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instr);
  Primitive::Type type = instr->GetResultType();
  switch (type) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong:
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, ARM64EncodableConstantOrRegister(instr->InputAt(1), instr));
      locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      break;

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetInAt(1, Location::RequiresFpuRegister());
      locations->SetOut(Location::RequiresFpuRegister(), Location::kNoOutputOverlap);
      break;

    default:
      LOG(FATAL) << "Unexpected " << instr->DebugName() << " type " << type;
  }
}

void LocationsBuilderARM64::HandleFieldGet(HInstruction* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  if (Primitive::IsFloatingPointType(instruction->GetType())) {
    locations->SetOut(Location::RequiresFpuRegister());
  } else {
    locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
  }
}

void InstructionCodeGeneratorARM64::HandleFieldGet(HInstruction* instruction,
                                                   const FieldInfo& field_info) {
  DCHECK(instruction->IsInstanceFieldGet() || instruction->IsStaticFieldGet());
  BlockPoolsScope block_pools(GetVIXLAssembler());

  MemOperand field = HeapOperand(InputRegisterAt(instruction, 0), field_info.GetFieldOffset());
  bool use_acquire_release = codegen_->GetInstructionSetFeatures().PreferAcquireRelease();

  if (field_info.IsVolatile()) {
    if (use_acquire_release) {
      // NB: LoadAcquire will record the pc info if needed.
      codegen_->LoadAcquire(instruction, OutputCPURegister(instruction), field);
    } else {
      codegen_->Load(field_info.GetFieldType(), OutputCPURegister(instruction), field);
      codegen_->MaybeRecordImplicitNullCheck(instruction);
      // For IRIW sequential consistency kLoadAny is not sufficient.
      GenerateMemoryBarrier(MemBarrierKind::kAnyAny);
    }
  } else {
    codegen_->Load(field_info.GetFieldType(), OutputCPURegister(instruction), field);
    codegen_->MaybeRecordImplicitNullCheck(instruction);
  }
}

void LocationsBuilderARM64::HandleFieldSet(HInstruction* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  if (Primitive::IsFloatingPointType(instruction->InputAt(1)->GetType())) {
    locations->SetInAt(1, Location::RequiresFpuRegister());
  } else {
    locations->SetInAt(1, Location::RequiresRegister());
  }
}

void InstructionCodeGeneratorARM64::HandleFieldSet(HInstruction* instruction,
                                                   const FieldInfo& field_info) {
  DCHECK(instruction->IsInstanceFieldSet() || instruction->IsStaticFieldSet());
  BlockPoolsScope block_pools(GetVIXLAssembler());

  Register obj = InputRegisterAt(instruction, 0);
  CPURegister value = InputCPURegisterAt(instruction, 1);
  Offset offset = field_info.GetFieldOffset();
  Primitive::Type field_type = field_info.GetFieldType();
  bool use_acquire_release = codegen_->GetInstructionSetFeatures().PreferAcquireRelease();

  if (field_info.IsVolatile()) {
    if (use_acquire_release) {
      codegen_->StoreRelease(field_type, value, HeapOperand(obj, offset));
      codegen_->MaybeRecordImplicitNullCheck(instruction);
    } else {
      GenerateMemoryBarrier(MemBarrierKind::kAnyStore);
      codegen_->Store(field_type, value, HeapOperand(obj, offset));
      codegen_->MaybeRecordImplicitNullCheck(instruction);
      GenerateMemoryBarrier(MemBarrierKind::kAnyAny);
    }
  } else {
    codegen_->Store(field_type, value, HeapOperand(obj, offset));
    codegen_->MaybeRecordImplicitNullCheck(instruction);
  }

  if (CodeGenerator::StoreNeedsWriteBarrier(field_type, instruction->InputAt(1))) {
    codegen_->MarkGCCard(obj, Register(value));
  }
}

void InstructionCodeGeneratorARM64::HandleBinaryOp(HBinaryOperation* instr) {
  Primitive::Type type = instr->GetType();

  switch (type) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong: {
      Register dst = OutputRegister(instr);
      Register lhs = InputRegisterAt(instr, 0);
      Operand rhs = InputOperandAt(instr, 1);
      if (instr->IsAdd()) {
        __ Add(dst, lhs, rhs);
      } else if (instr->IsAnd()) {
        __ And(dst, lhs, rhs);
      } else if (instr->IsOr()) {
        __ Orr(dst, lhs, rhs);
      } else if (instr->IsSub()) {
        __ Sub(dst, lhs, rhs);
      } else {
        DCHECK(instr->IsXor());
        __ Eor(dst, lhs, rhs);
      }
      break;
    }
    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      FPRegister dst = OutputFPRegister(instr);
      FPRegister lhs = InputFPRegisterAt(instr, 0);
      FPRegister rhs = InputFPRegisterAt(instr, 1);
      if (instr->IsAdd()) {
        __ Fadd(dst, lhs, rhs);
      } else if (instr->IsSub()) {
        __ Fsub(dst, lhs, rhs);
      } else {
        LOG(FATAL) << "Unexpected floating-point binary operation";
      }
      break;
    }
    default:
      LOG(FATAL) << "Unexpected binary operation type " << type;
  }
}

void LocationsBuilderARM64::HandleShift(HBinaryOperation* instr) {
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

void InstructionCodeGeneratorARM64::HandleShift(HBinaryOperation* instr) {
  DCHECK(instr->IsShl() || instr->IsShr() || instr->IsUShr());

  Primitive::Type type = instr->GetType();
  switch (type) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong: {
      Register dst = OutputRegister(instr);
      Register lhs = InputRegisterAt(instr, 0);
      Operand rhs = InputOperandAt(instr, 1);
      if (rhs.IsImmediate()) {
        uint32_t shift_value = (type == Primitive::kPrimInt)
          ? static_cast<uint32_t>(rhs.immediate() & kMaxIntShiftValue)
          : static_cast<uint32_t>(rhs.immediate() & kMaxLongShiftValue);
        if (instr->IsShl()) {
          __ Lsl(dst, lhs, shift_value);
        } else if (instr->IsShr()) {
          __ Asr(dst, lhs, shift_value);
        } else {
          __ Lsr(dst, lhs, shift_value);
        }
      } else {
        Register rhs_reg = dst.IsX() ? rhs.reg().X() : rhs.reg().W();

        if (instr->IsShl()) {
          __ Lsl(dst, lhs, rhs_reg);
        } else if (instr->IsShr()) {
          __ Asr(dst, lhs, rhs_reg);
        } else {
          __ Lsr(dst, lhs, rhs_reg);
        }
      }
      break;
    }
    default:
      LOG(FATAL) << "Unexpected shift operation type " << type;
  }
}

void LocationsBuilderARM64::VisitAdd(HAdd* instruction) {
  HandleBinaryOp(instruction);
}

void InstructionCodeGeneratorARM64::VisitAdd(HAdd* instruction) {
  HandleBinaryOp(instruction);
}

void LocationsBuilderARM64::VisitAnd(HAnd* instruction) {
  HandleBinaryOp(instruction);
}

void InstructionCodeGeneratorARM64::VisitAnd(HAnd* instruction) {
  HandleBinaryOp(instruction);
}

void LocationsBuilderARM64::VisitArrayGet(HArrayGet* instruction) {
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

void InstructionCodeGeneratorARM64::VisitArrayGet(HArrayGet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Primitive::Type type = instruction->GetType();
  Register obj = InputRegisterAt(instruction, 0);
  Location index = locations->InAt(1);
  size_t offset = mirror::Array::DataOffset(Primitive::ComponentSize(type)).Uint32Value();
  MemOperand source = HeapOperand(obj);
  MacroAssembler* masm = GetVIXLAssembler();
  UseScratchRegisterScope temps(masm);
  BlockPoolsScope block_pools(masm);

  if (index.IsConstant()) {
    offset += Int64ConstantFrom(index) << Primitive::ComponentSizeShift(type);
    source = HeapOperand(obj, offset);
  } else {
    Register temp = temps.AcquireSameSizeAs(obj);
    Register index_reg = RegisterFrom(index, Primitive::kPrimInt);
    __ Add(temp, obj, Operand(index_reg, LSL, Primitive::ComponentSizeShift(type)));
    source = HeapOperand(temp, offset);
  }

  codegen_->Load(type, OutputCPURegister(instruction), source);
  codegen_->MaybeRecordImplicitNullCheck(instruction);
}

void LocationsBuilderARM64::VisitArrayLength(HArrayLength* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

void InstructionCodeGeneratorARM64::VisitArrayLength(HArrayLength* instruction) {
  BlockPoolsScope block_pools(GetVIXLAssembler());
  __ Ldr(OutputRegister(instruction),
         HeapOperand(InputRegisterAt(instruction, 0), mirror::Array::LengthOffset()));
  codegen_->MaybeRecordImplicitNullCheck(instruction);
}

void LocationsBuilderARM64::VisitArraySet(HArraySet* instruction) {
  if (instruction->NeedsTypeCheck()) {
    LocationSummary* locations =
        new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCall);
    InvokeRuntimeCallingConvention calling_convention;
    locations->SetInAt(0, LocationFrom(calling_convention.GetRegisterAt(0)));
    locations->SetInAt(1, LocationFrom(calling_convention.GetRegisterAt(1)));
    locations->SetInAt(2, LocationFrom(calling_convention.GetRegisterAt(2)));
  } else {
    LocationSummary* locations =
        new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
    locations->SetInAt(0, Location::RequiresRegister());
    locations->SetInAt(1, Location::RegisterOrConstant(instruction->InputAt(1)));
    if (Primitive::IsFloatingPointType(instruction->InputAt(2)->GetType())) {
      locations->SetInAt(2, Location::RequiresFpuRegister());
    } else {
      locations->SetInAt(2, Location::RequiresRegister());
    }
  }
}

void InstructionCodeGeneratorARM64::VisitArraySet(HArraySet* instruction) {
  Primitive::Type value_type = instruction->GetComponentType();
  LocationSummary* locations = instruction->GetLocations();
  bool needs_runtime_call = locations->WillCall();

  if (needs_runtime_call) {
    codegen_->InvokeRuntime(
        QUICK_ENTRY_POINT(pAputObject), instruction, instruction->GetDexPc(), nullptr);
    CheckEntrypointTypes<kQuickAputObject, void, mirror::Array*, int32_t, mirror::Object*>();
  } else {
    Register obj = InputRegisterAt(instruction, 0);
    CPURegister value = InputCPURegisterAt(instruction, 2);
    Location index = locations->InAt(1);
    size_t offset = mirror::Array::DataOffset(Primitive::ComponentSize(value_type)).Uint32Value();
    MemOperand destination = HeapOperand(obj);
    MacroAssembler* masm = GetVIXLAssembler();
    BlockPoolsScope block_pools(masm);
    {
      // We use a block to end the scratch scope before the write barrier, thus
      // freeing the temporary registers so they can be used in `MarkGCCard`.
      UseScratchRegisterScope temps(masm);

      if (index.IsConstant()) {
        offset += Int64ConstantFrom(index) << Primitive::ComponentSizeShift(value_type);
        destination = HeapOperand(obj, offset);
      } else {
        Register temp = temps.AcquireSameSizeAs(obj);
        Register index_reg = InputRegisterAt(instruction, 1);
        __ Add(temp, obj, Operand(index_reg, LSL, Primitive::ComponentSizeShift(value_type)));
        destination = HeapOperand(temp, offset);
      }

      codegen_->Store(value_type, value, destination);
      codegen_->MaybeRecordImplicitNullCheck(instruction);
    }
    if (CodeGenerator::StoreNeedsWriteBarrier(value_type, instruction->GetValue())) {
      codegen_->MarkGCCard(obj, value.W());
    }
  }
}

void LocationsBuilderARM64::VisitBoundsCheck(HBoundsCheck* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, ARM64EncodableConstantOrRegister(instruction->InputAt(1), instruction));
  if (instruction->HasUses()) {
    locations->SetOut(Location::SameAsFirstInput());
  }
}

void InstructionCodeGeneratorARM64::VisitBoundsCheck(HBoundsCheck* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  BoundsCheckSlowPathARM64* slow_path = new (GetGraph()->GetArena()) BoundsCheckSlowPathARM64(
      instruction, locations->InAt(0), locations->InAt(1));
  codegen_->AddSlowPath(slow_path);

  __ Cmp(InputRegisterAt(instruction, 0), InputOperandAt(instruction, 1));
  __ B(slow_path->GetEntryLabel(), hs);
}

void LocationsBuilderARM64::VisitCheckCast(HCheckCast* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(
      instruction, LocationSummary::kCallOnSlowPath);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  locations->AddTemp(Location::RequiresRegister());
}

void InstructionCodeGeneratorARM64::VisitCheckCast(HCheckCast* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Register obj = InputRegisterAt(instruction, 0);;
  Register cls = InputRegisterAt(instruction, 1);;
  Register obj_cls = WRegisterFrom(instruction->GetLocations()->GetTemp(0));

  SlowPathCodeARM64* slow_path = new (GetGraph()->GetArena()) TypeCheckSlowPathARM64(
      instruction, locations->InAt(1), LocationFrom(obj_cls), instruction->GetDexPc());
  codegen_->AddSlowPath(slow_path);

  // Avoid null check if we know obj is not null.
  if (instruction->MustDoNullCheck()) {
    __ Cbz(obj, slow_path->GetExitLabel());
  }
  // Compare the class of `obj` with `cls`.
  __ Ldr(obj_cls, HeapOperand(obj, mirror::Object::ClassOffset()));
  __ Cmp(obj_cls, cls);
  __ B(ne, slow_path->GetEntryLabel());
  __ Bind(slow_path->GetExitLabel());
}

void LocationsBuilderARM64::VisitClinitCheck(HClinitCheck* check) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(check, LocationSummary::kCallOnSlowPath);
  locations->SetInAt(0, Location::RequiresRegister());
  if (check->HasUses()) {
    locations->SetOut(Location::SameAsFirstInput());
  }
}

void InstructionCodeGeneratorARM64::VisitClinitCheck(HClinitCheck* check) {
  // We assume the class is not null.
  SlowPathCodeARM64* slow_path = new (GetGraph()->GetArena()) LoadClassSlowPathARM64(
      check->GetLoadClass(), check, check->GetDexPc(), true);
  codegen_->AddSlowPath(slow_path);
  GenerateClassInitializationCheck(slow_path, InputRegisterAt(check, 0));
}

void LocationsBuilderARM64::VisitCompare(HCompare* compare) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(compare, LocationSummary::kNoCall);
  Primitive::Type in_type = compare->InputAt(0)->GetType();
  switch (in_type) {
    case Primitive::kPrimLong: {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, ARM64EncodableConstantOrRegister(compare->InputAt(1), compare));
      locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      break;
    }
    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      locations->SetInAt(0, Location::RequiresFpuRegister());
      HInstruction* right = compare->InputAt(1);
      if ((right->IsFloatConstant() && (right->AsFloatConstant()->GetValue() == 0.0f)) ||
          (right->IsDoubleConstant() && (right->AsDoubleConstant()->GetValue() == 0.0))) {
        locations->SetInAt(1, Location::ConstantLocation(right->AsConstant()));
      } else {
        locations->SetInAt(1, Location::RequiresFpuRegister());
      }
      locations->SetOut(Location::RequiresRegister());
      break;
    }
    default:
      LOG(FATAL) << "Unexpected type for compare operation " << in_type;
  }
}

void InstructionCodeGeneratorARM64::VisitCompare(HCompare* compare) {
  Primitive::Type in_type = compare->InputAt(0)->GetType();

  //  0 if: left == right
  //  1 if: left  > right
  // -1 if: left  < right
  switch (in_type) {
    case Primitive::kPrimLong: {
      Register result = OutputRegister(compare);
      Register left = InputRegisterAt(compare, 0);
      Operand right = InputOperandAt(compare, 1);

      __ Cmp(left, right);
      __ Cset(result, ne);
      __ Cneg(result, result, lt);
      break;
    }
    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      Register result = OutputRegister(compare);
      FPRegister left = InputFPRegisterAt(compare, 0);
      if (compare->GetLocations()->InAt(1).IsConstant()) {
        if (kIsDebugBuild) {
          HInstruction* right = compare->GetLocations()->InAt(1).GetConstant();
          DCHECK((right->IsFloatConstant() && (right->AsFloatConstant()->GetValue() == 0.0f)) ||
                  (right->IsDoubleConstant() && (right->AsDoubleConstant()->GetValue() == 0.0)));
        }
        // 0.0 is the only immediate that can be encoded directly in a FCMP instruction.
        __ Fcmp(left, 0.0);
      } else {
        __ Fcmp(left, InputFPRegisterAt(compare, 1));
      }
      if (compare->IsGtBias()) {
        __ Cset(result, ne);
      } else {
        __ Csetm(result, ne);
      }
      __ Cneg(result, result, compare->IsGtBias() ? mi : gt);
      break;
    }
    default:
      LOG(FATAL) << "Unimplemented compare type " << in_type;
  }
}

void LocationsBuilderARM64::VisitCondition(HCondition* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, ARM64EncodableConstantOrRegister(instruction->InputAt(1), instruction));
  if (instruction->NeedsMaterialization()) {
    locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
  }
}

void InstructionCodeGeneratorARM64::VisitCondition(HCondition* instruction) {
  if (!instruction->NeedsMaterialization()) {
    return;
  }

  LocationSummary* locations = instruction->GetLocations();
  Register lhs = InputRegisterAt(instruction, 0);
  Operand rhs = InputOperandAt(instruction, 1);
  Register res = RegisterFrom(locations->Out(), instruction->GetType());
  Condition cond = ARM64Condition(instruction->GetCondition());

  __ Cmp(lhs, rhs);
  __ Cset(res, cond);
}

#define FOR_EACH_CONDITION_INSTRUCTION(M)                                                \
  M(Equal)                                                                               \
  M(NotEqual)                                                                            \
  M(LessThan)                                                                            \
  M(LessThanOrEqual)                                                                     \
  M(GreaterThan)                                                                         \
  M(GreaterThanOrEqual)
#define DEFINE_CONDITION_VISITORS(Name)                                                  \
void LocationsBuilderARM64::Visit##Name(H##Name* comp) { VisitCondition(comp); }         \
void InstructionCodeGeneratorARM64::Visit##Name(H##Name* comp) { VisitCondition(comp); }
FOR_EACH_CONDITION_INSTRUCTION(DEFINE_CONDITION_VISITORS)
#undef DEFINE_CONDITION_VISITORS
#undef FOR_EACH_CONDITION_INSTRUCTION

void LocationsBuilderARM64::VisitDiv(HDiv* div) {
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

void InstructionCodeGeneratorARM64::VisitDiv(HDiv* div) {
  Primitive::Type type = div->GetResultType();
  switch (type) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong:
      __ Sdiv(OutputRegister(div), InputRegisterAt(div, 0), InputRegisterAt(div, 1));
      break;

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      __ Fdiv(OutputFPRegister(div), InputFPRegisterAt(div, 0), InputFPRegisterAt(div, 1));
      break;

    default:
      LOG(FATAL) << "Unexpected div type " << type;
  }
}

void LocationsBuilderARM64::VisitDivZeroCheck(HDivZeroCheck* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RegisterOrConstant(instruction->InputAt(0)));
  if (instruction->HasUses()) {
    locations->SetOut(Location::SameAsFirstInput());
  }
}

void InstructionCodeGeneratorARM64::VisitDivZeroCheck(HDivZeroCheck* instruction) {
  SlowPathCodeARM64* slow_path =
      new (GetGraph()->GetArena()) DivZeroCheckSlowPathARM64(instruction);
  codegen_->AddSlowPath(slow_path);
  Location value = instruction->GetLocations()->InAt(0);

  Primitive::Type type = instruction->GetType();

  if ((type != Primitive::kPrimInt) && (type != Primitive::kPrimLong)) {
      LOG(FATAL) << "Unexpected type " << type << "for DivZeroCheck.";
    return;
  }

  if (value.IsConstant()) {
    int64_t divisor = Int64ConstantFrom(value);
    if (divisor == 0) {
      __ B(slow_path->GetEntryLabel());
    } else {
      // A division by a non-null constant is valid. We don't need to perform
      // any check, so simply fall through.
    }
  } else {
    __ Cbz(InputRegisterAt(instruction, 0), slow_path->GetEntryLabel());
  }
}

void LocationsBuilderARM64::VisitDoubleConstant(HDoubleConstant* constant) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(constant, LocationSummary::kNoCall);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorARM64::VisitDoubleConstant(HDoubleConstant* constant) {
  UNUSED(constant);
  // Will be generated at use site.
}

void LocationsBuilderARM64::VisitExit(HExit* exit) {
  exit->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM64::VisitExit(HExit* exit) {
  UNUSED(exit);
}

void LocationsBuilderARM64::VisitFloatConstant(HFloatConstant* constant) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(constant, LocationSummary::kNoCall);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorARM64::VisitFloatConstant(HFloatConstant* constant) {
  UNUSED(constant);
  // Will be generated at use site.
}

void LocationsBuilderARM64::VisitGoto(HGoto* got) {
  got->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM64::VisitGoto(HGoto* got) {
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

void InstructionCodeGeneratorARM64::GenerateTestAndBranch(HInstruction* instruction,
                                                          vixl::Label* true_target,
                                                          vixl::Label* false_target,
                                                          vixl::Label* always_true_target) {
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
    __ Cbnz(InputRegisterAt(instruction, 0), true_target);
  } else {
    // The condition instruction has not been materialized, use its inputs as
    // the comparison and its condition as the branch condition.
    Register lhs = InputRegisterAt(condition, 0);
    Operand rhs = InputOperandAt(condition, 1);
    Condition arm64_cond = ARM64Condition(condition->GetCondition());
    if ((arm64_cond != gt && arm64_cond != le) && rhs.IsImmediate() && (rhs.immediate() == 0)) {
      switch (arm64_cond) {
        case eq:
          __ Cbz(lhs, true_target);
          break;
        case ne:
          __ Cbnz(lhs, true_target);
          break;
        case lt:
          // Test the sign bit and branch accordingly.
          __ Tbnz(lhs, (lhs.IsX() ? kXRegSize : kWRegSize) - 1, true_target);
          break;
        case ge:
          // Test the sign bit and branch accordingly.
          __ Tbz(lhs, (lhs.IsX() ? kXRegSize : kWRegSize) - 1, true_target);
          break;
        default:
          // Without the `static_cast` the compiler throws an error for
          // `-Werror=sign-promo`.
          LOG(FATAL) << "Unexpected condition: " << static_cast<int>(arm64_cond);
      }
    } else {
      __ Cmp(lhs, rhs);
      __ B(arm64_cond, true_target);
    }
  }
  if (false_target != nullptr) {
    __ B(false_target);
  }
}

void LocationsBuilderARM64::VisitIf(HIf* if_instr) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(if_instr);
  HInstruction* cond = if_instr->InputAt(0);
  if (!cond->IsCondition() || cond->AsCondition()->NeedsMaterialization()) {
    locations->SetInAt(0, Location::RequiresRegister());
  }
}

void InstructionCodeGeneratorARM64::VisitIf(HIf* if_instr) {
  vixl::Label* true_target = codegen_->GetLabelOf(if_instr->IfTrueSuccessor());
  vixl::Label* false_target = codegen_->GetLabelOf(if_instr->IfFalseSuccessor());
  vixl::Label* always_true_target = true_target;
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

void LocationsBuilderARM64::VisitDeoptimize(HDeoptimize* deoptimize) {
  LocationSummary* locations = new (GetGraph()->GetArena())
      LocationSummary(deoptimize, LocationSummary::kCallOnSlowPath);
  HInstruction* cond = deoptimize->InputAt(0);
  DCHECK(cond->IsCondition());
  if (cond->AsCondition()->NeedsMaterialization()) {
    locations->SetInAt(0, Location::RequiresRegister());
  }
}

void InstructionCodeGeneratorARM64::VisitDeoptimize(HDeoptimize* deoptimize) {
  SlowPathCodeARM64* slow_path = new (GetGraph()->GetArena())
      DeoptimizationSlowPathARM64(deoptimize);
  codegen_->AddSlowPath(slow_path);
  vixl::Label* slow_path_entry = slow_path->GetEntryLabel();
  GenerateTestAndBranch(deoptimize, slow_path_entry, nullptr, slow_path_entry);
}

void LocationsBuilderARM64::VisitInstanceFieldGet(HInstanceFieldGet* instruction) {
  HandleFieldGet(instruction);
}

void InstructionCodeGeneratorARM64::VisitInstanceFieldGet(HInstanceFieldGet* instruction) {
  HandleFieldGet(instruction, instruction->GetFieldInfo());
}

void LocationsBuilderARM64::VisitInstanceFieldSet(HInstanceFieldSet* instruction) {
  HandleFieldSet(instruction);
}

void InstructionCodeGeneratorARM64::VisitInstanceFieldSet(HInstanceFieldSet* instruction) {
  HandleFieldSet(instruction, instruction->GetFieldInfo());
}

void LocationsBuilderARM64::VisitInstanceOf(HInstanceOf* instruction) {
  LocationSummary::CallKind call_kind =
      instruction->IsClassFinal() ? LocationSummary::kNoCall : LocationSummary::kCallOnSlowPath;
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction, call_kind);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  // The output does overlap inputs.
  locations->SetOut(Location::RequiresRegister(), Location::kOutputOverlap);
}

void InstructionCodeGeneratorARM64::VisitInstanceOf(HInstanceOf* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Register obj = InputRegisterAt(instruction, 0);;
  Register cls = InputRegisterAt(instruction, 1);;
  Register out = OutputRegister(instruction);

  vixl::Label done;

  // Return 0 if `obj` is null.
  // Avoid null check if we know `obj` is not null.
  if (instruction->MustDoNullCheck()) {
    __ Mov(out, 0);
    __ Cbz(obj, &done);
  }

  // Compare the class of `obj` with `cls`.
  __ Ldr(out, HeapOperand(obj, mirror::Object::ClassOffset()));
  __ Cmp(out, cls);
  if (instruction->IsClassFinal()) {
    // Classes must be equal for the instanceof to succeed.
    __ Cset(out, eq);
  } else {
    // If the classes are not equal, we go into a slow path.
    DCHECK(locations->OnlyCallsOnSlowPath());
    SlowPathCodeARM64* slow_path =
        new (GetGraph()->GetArena()) TypeCheckSlowPathARM64(
        instruction, locations->InAt(1), locations->Out(), instruction->GetDexPc());
    codegen_->AddSlowPath(slow_path);
    __ B(ne, slow_path->GetEntryLabel());
    __ Mov(out, 1);
    __ Bind(slow_path->GetExitLabel());
  }

  __ Bind(&done);
}

void LocationsBuilderARM64::VisitIntConstant(HIntConstant* constant) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(constant);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorARM64::VisitIntConstant(HIntConstant* constant) {
  // Will be generated at use site.
  UNUSED(constant);
}

void LocationsBuilderARM64::VisitNullConstant(HNullConstant* constant) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(constant);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorARM64::VisitNullConstant(HNullConstant* constant) {
  // Will be generated at use site.
  UNUSED(constant);
}

void LocationsBuilderARM64::HandleInvoke(HInvoke* invoke) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(invoke, LocationSummary::kCall);
  locations->AddTemp(LocationFrom(x0));

  InvokeDexCallingConventionVisitorARM64 calling_convention_visitor;
  for (size_t i = 0; i < invoke->GetNumberOfArguments(); i++) {
    HInstruction* input = invoke->InputAt(i);
    locations->SetInAt(i, calling_convention_visitor.GetNextLocation(input->GetType()));
  }

  Primitive::Type return_type = invoke->GetType();
  if (return_type != Primitive::kPrimVoid) {
    locations->SetOut(calling_convention_visitor.GetReturnLocation(return_type));
  }
}

void LocationsBuilderARM64::VisitInvokeInterface(HInvokeInterface* invoke) {
  HandleInvoke(invoke);
}

void InstructionCodeGeneratorARM64::VisitInvokeInterface(HInvokeInterface* invoke) {
  // TODO: b/18116999, our IMTs can miss an IncompatibleClassChangeError.
  Register temp = XRegisterFrom(invoke->GetLocations()->GetTemp(0));
  uint32_t method_offset = mirror::Class::EmbeddedImTableEntryOffset(
      invoke->GetImtIndex() % mirror::Class::kImtSize, kArm64PointerSize).Uint32Value();
  Location receiver = invoke->GetLocations()->InAt(0);
  Offset class_offset = mirror::Object::ClassOffset();
  Offset entry_point = ArtMethod::EntryPointFromQuickCompiledCodeOffset(kArm64WordSize);

  // The register ip1 is required to be used for the hidden argument in
  // art_quick_imt_conflict_trampoline, so prevent VIXL from using it.
  MacroAssembler* masm = GetVIXLAssembler();
  UseScratchRegisterScope scratch_scope(masm);
  BlockPoolsScope block_pools(masm);
  scratch_scope.Exclude(ip1);
  __ Mov(ip1, invoke->GetDexMethodIndex());

  // temp = object->GetClass();
  if (receiver.IsStackSlot()) {
    __ Ldr(temp.W(), StackOperandFrom(receiver));
    __ Ldr(temp.W(), HeapOperand(temp.W(), class_offset));
  } else {
    __ Ldr(temp.W(), HeapOperandFrom(receiver, class_offset));
  }
  codegen_->MaybeRecordImplicitNullCheck(invoke);
  // temp = temp->GetImtEntryAt(method_offset);
  __ Ldr(temp, MemOperand(temp, method_offset));
  // lr = temp->GetEntryPoint();
  __ Ldr(lr, MemOperand(temp, entry_point.Int32Value()));
  // lr();
  __ Blr(lr);
  DCHECK(!codegen_->IsLeafMethod());
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
}

void LocationsBuilderARM64::VisitInvokeVirtual(HInvokeVirtual* invoke) {
  IntrinsicLocationsBuilderARM64 intrinsic(GetGraph()->GetArena());
  if (intrinsic.TryDispatch(invoke)) {
    return;
  }

  HandleInvoke(invoke);
}

void LocationsBuilderARM64::VisitInvokeStaticOrDirect(HInvokeStaticOrDirect* invoke) {
  // When we do not run baseline, explicit clinit checks triggered by static
  // invokes must have been pruned by art::PrepareForRegisterAllocation.
  DCHECK(codegen_->IsBaseline() || !invoke->IsStaticWithExplicitClinitCheck());

  IntrinsicLocationsBuilderARM64 intrinsic(GetGraph()->GetArena());
  if (intrinsic.TryDispatch(invoke)) {
    return;
  }

  HandleInvoke(invoke);
}

static bool TryGenerateIntrinsicCode(HInvoke* invoke, CodeGeneratorARM64* codegen) {
  if (invoke->GetLocations()->Intrinsified()) {
    IntrinsicCodeGeneratorARM64 intrinsic(codegen);
    intrinsic.Dispatch(invoke);
    return true;
  }
  return false;
}

void CodeGeneratorARM64::GenerateStaticOrDirectCall(HInvokeStaticOrDirect* invoke, Register temp) {
  // Make sure that ArtMethod* is passed in kArtMethodRegister as per the calling convention.
  DCHECK(temp.Is(kArtMethodRegister));
  size_t index_in_cache = GetCachePointerOffset(invoke->GetDexMethodIndex());

  // TODO: Implement all kinds of calls:
  // 1) boot -> boot
  // 2) app -> boot
  // 3) app -> app
  //
  // Currently we implement the app -> app logic, which looks up in the resolve cache.

  if (invoke->IsStringInit()) {
    // temp = thread->string_init_entrypoint
    __ Ldr(temp.X(), MemOperand(tr, invoke->GetStringInitOffset()));
    // LR = temp->entry_point_from_quick_compiled_code_;
    __ Ldr(lr, MemOperand(
        temp, ArtMethod::EntryPointFromQuickCompiledCodeOffset(kArm64WordSize).Int32Value()));
    // lr()
    __ Blr(lr);
  } else {
    // temp = method;
    LoadCurrentMethod(temp.X());
    if (!invoke->IsRecursive()) {
      // temp = temp->dex_cache_resolved_methods_;
      __ Ldr(temp.W(), MemOperand(temp.X(),
                                  ArtMethod::DexCacheResolvedMethodsOffset().Int32Value()));
      // temp = temp[index_in_cache];
      __ Ldr(temp.X(), MemOperand(temp, index_in_cache));
      // lr = temp->entry_point_from_quick_compiled_code_;
      __ Ldr(lr, MemOperand(temp.X(), ArtMethod::EntryPointFromQuickCompiledCodeOffset(
          kArm64WordSize).Int32Value()));
      // lr();
      __ Blr(lr);
    } else {
      __ Bl(&frame_entry_label_);
    }
  }

  DCHECK(!IsLeafMethod());
}

void InstructionCodeGeneratorARM64::VisitInvokeStaticOrDirect(HInvokeStaticOrDirect* invoke) {
  // When we do not run baseline, explicit clinit checks triggered by static
  // invokes must have been pruned by art::PrepareForRegisterAllocation.
  DCHECK(codegen_->IsBaseline() || !invoke->IsStaticWithExplicitClinitCheck());

  if (TryGenerateIntrinsicCode(invoke, codegen_)) {
    return;
  }

  BlockPoolsScope block_pools(GetVIXLAssembler());
  Register temp = XRegisterFrom(invoke->GetLocations()->GetTemp(0));
  codegen_->GenerateStaticOrDirectCall(invoke, temp);
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
}

void InstructionCodeGeneratorARM64::VisitInvokeVirtual(HInvokeVirtual* invoke) {
  if (TryGenerateIntrinsicCode(invoke, codegen_)) {
    return;
  }

  LocationSummary* locations = invoke->GetLocations();
  Location receiver = locations->InAt(0);
  Register temp = XRegisterFrom(invoke->GetLocations()->GetTemp(0));
  size_t method_offset = mirror::Class::EmbeddedVTableEntryOffset(
      invoke->GetVTableIndex(), kArm64PointerSize).SizeValue();
  Offset class_offset = mirror::Object::ClassOffset();
  Offset entry_point = ArtMethod::EntryPointFromQuickCompiledCodeOffset(kArm64WordSize);

  BlockPoolsScope block_pools(GetVIXLAssembler());

  // temp = object->GetClass();
  if (receiver.IsStackSlot()) {
    __ Ldr(temp.W(), MemOperand(sp, receiver.GetStackIndex()));
    __ Ldr(temp.W(), HeapOperand(temp.W(), class_offset));
  } else {
    DCHECK(receiver.IsRegister());
    __ Ldr(temp.W(), HeapOperandFrom(receiver, class_offset));
  }
  codegen_->MaybeRecordImplicitNullCheck(invoke);
  // temp = temp->GetMethodAt(method_offset);
  __ Ldr(temp, MemOperand(temp, method_offset));
  // lr = temp->GetEntryPoint();
  __ Ldr(lr, MemOperand(temp, entry_point.SizeValue()));
  // lr();
  __ Blr(lr);
  DCHECK(!codegen_->IsLeafMethod());
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
}

void LocationsBuilderARM64::VisitLoadClass(HLoadClass* cls) {
  LocationSummary::CallKind call_kind = cls->CanCallRuntime() ? LocationSummary::kCallOnSlowPath
                                                              : LocationSummary::kNoCall;
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(cls, call_kind);
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorARM64::VisitLoadClass(HLoadClass* cls) {
  Register out = OutputRegister(cls);
  if (cls->IsReferrersClass()) {
    DCHECK(!cls->CanCallRuntime());
    DCHECK(!cls->MustGenerateClinitCheck());
    codegen_->LoadCurrentMethod(out.X());
    __ Ldr(out, MemOperand(out.X(), ArtMethod::DeclaringClassOffset().Int32Value()));
  } else {
    DCHECK(cls->CanCallRuntime());
    codegen_->LoadCurrentMethod(out.X());
    __ Ldr(out, MemOperand(out.X(), ArtMethod::DexCacheResolvedTypesOffset().Int32Value()));
    __ Ldr(out, HeapOperand(out, CodeGenerator::GetCacheOffset(cls->GetTypeIndex())));

    SlowPathCodeARM64* slow_path = new (GetGraph()->GetArena()) LoadClassSlowPathARM64(
        cls, cls, cls->GetDexPc(), cls->MustGenerateClinitCheck());
    codegen_->AddSlowPath(slow_path);
    __ Cbz(out, slow_path->GetEntryLabel());
    if (cls->MustGenerateClinitCheck()) {
      GenerateClassInitializationCheck(slow_path, out);
    } else {
      __ Bind(slow_path->GetExitLabel());
    }
  }
}

void LocationsBuilderARM64::VisitLoadException(HLoadException* load) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(load, LocationSummary::kNoCall);
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorARM64::VisitLoadException(HLoadException* instruction) {
  MemOperand exception = MemOperand(tr, Thread::ExceptionOffset<kArm64WordSize>().Int32Value());
  __ Ldr(OutputRegister(instruction), exception);
  __ Str(wzr, exception);
}

void LocationsBuilderARM64::VisitLoadLocal(HLoadLocal* load) {
  load->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM64::VisitLoadLocal(HLoadLocal* load) {
  // Nothing to do, this is driven by the code generator.
  UNUSED(load);
}

void LocationsBuilderARM64::VisitLoadString(HLoadString* load) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(load, LocationSummary::kCallOnSlowPath);
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorARM64::VisitLoadString(HLoadString* load) {
  SlowPathCodeARM64* slow_path = new (GetGraph()->GetArena()) LoadStringSlowPathARM64(load);
  codegen_->AddSlowPath(slow_path);

  Register out = OutputRegister(load);
  codegen_->LoadCurrentMethod(out.X());
  __ Ldr(out, MemOperand(out.X(), ArtMethod::DeclaringClassOffset().Int32Value()));
  __ Ldr(out, HeapOperand(out, mirror::Class::DexCacheStringsOffset()));
  __ Ldr(out, HeapOperand(out, CodeGenerator::GetCacheOffset(load->GetStringIndex())));
  __ Cbz(out, slow_path->GetEntryLabel());
  __ Bind(slow_path->GetExitLabel());
}

void LocationsBuilderARM64::VisitLocal(HLocal* local) {
  local->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM64::VisitLocal(HLocal* local) {
  DCHECK_EQ(local->GetBlock(), GetGraph()->GetEntryBlock());
}

void LocationsBuilderARM64::VisitLongConstant(HLongConstant* constant) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(constant);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorARM64::VisitLongConstant(HLongConstant* constant) {
  // Will be generated at use site.
  UNUSED(constant);
}

void LocationsBuilderARM64::VisitMonitorOperation(HMonitorOperation* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCall);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, LocationFrom(calling_convention.GetRegisterAt(0)));
}

void InstructionCodeGeneratorARM64::VisitMonitorOperation(HMonitorOperation* instruction) {
  codegen_->InvokeRuntime(instruction->IsEnter()
        ? QUICK_ENTRY_POINT(pLockObject) : QUICK_ENTRY_POINT(pUnlockObject),
      instruction,
      instruction->GetDexPc(),
      nullptr);
  CheckEntrypointTypes<kQuickLockObject, void, mirror::Object*>();
}

void LocationsBuilderARM64::VisitMul(HMul* mul) {
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

void InstructionCodeGeneratorARM64::VisitMul(HMul* mul) {
  switch (mul->GetResultType()) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong:
      __ Mul(OutputRegister(mul), InputRegisterAt(mul, 0), InputRegisterAt(mul, 1));
      break;

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      __ Fmul(OutputFPRegister(mul), InputFPRegisterAt(mul, 0), InputFPRegisterAt(mul, 1));
      break;

    default:
      LOG(FATAL) << "Unexpected mul type " << mul->GetResultType();
  }
}

void LocationsBuilderARM64::VisitNeg(HNeg* neg) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(neg, LocationSummary::kNoCall);
  switch (neg->GetResultType()) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong:
      locations->SetInAt(0, ARM64EncodableConstantOrRegister(neg->InputAt(0), neg));
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

void InstructionCodeGeneratorARM64::VisitNeg(HNeg* neg) {
  switch (neg->GetResultType()) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong:
      __ Neg(OutputRegister(neg), InputOperandAt(neg, 0));
      break;

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      __ Fneg(OutputFPRegister(neg), InputFPRegisterAt(neg, 0));
      break;

    default:
      LOG(FATAL) << "Unexpected neg type " << neg->GetResultType();
  }
}

void LocationsBuilderARM64::VisitNewArray(HNewArray* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCall);
  InvokeRuntimeCallingConvention calling_convention;
  locations->AddTemp(LocationFrom(calling_convention.GetRegisterAt(0)));
  locations->AddTemp(LocationFrom(calling_convention.GetRegisterAt(2)));
  locations->SetOut(LocationFrom(x0));
  locations->SetInAt(0, LocationFrom(calling_convention.GetRegisterAt(1)));
  CheckEntrypointTypes<kQuickAllocArrayWithAccessCheck,
                       void*, uint32_t, int32_t, ArtMethod*>();
}

void InstructionCodeGeneratorARM64::VisitNewArray(HNewArray* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  InvokeRuntimeCallingConvention calling_convention;
  Register type_index = RegisterFrom(locations->GetTemp(0), Primitive::kPrimInt);
  DCHECK(type_index.Is(w0));
  Register current_method = RegisterFrom(locations->GetTemp(1), Primitive::kPrimLong);
  DCHECK(current_method.Is(x2));
  codegen_->LoadCurrentMethod(current_method.X());
  __ Mov(type_index, instruction->GetTypeIndex());
  codegen_->InvokeRuntime(
      GetThreadOffset<kArm64WordSize>(instruction->GetEntrypoint()).Int32Value(),
      instruction,
      instruction->GetDexPc(),
      nullptr);
  CheckEntrypointTypes<kQuickAllocArrayWithAccessCheck, void*, uint32_t, int32_t, ArtMethod*>();
}

void LocationsBuilderARM64::VisitNewInstance(HNewInstance* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCall);
  InvokeRuntimeCallingConvention calling_convention;
  locations->AddTemp(LocationFrom(calling_convention.GetRegisterAt(0)));
  locations->AddTemp(LocationFrom(calling_convention.GetRegisterAt(1)));
  locations->SetOut(calling_convention.GetReturnLocation(Primitive::kPrimNot));
  CheckEntrypointTypes<kQuickAllocObjectWithAccessCheck, void*, uint32_t, ArtMethod*>();
}

void InstructionCodeGeneratorARM64::VisitNewInstance(HNewInstance* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Register type_index = RegisterFrom(locations->GetTemp(0), Primitive::kPrimInt);
  DCHECK(type_index.Is(w0));
  Register current_method = RegisterFrom(locations->GetTemp(1), Primitive::kPrimNot);
  DCHECK(current_method.Is(w1));
  codegen_->LoadCurrentMethod(current_method.X());
  __ Mov(type_index, instruction->GetTypeIndex());
  codegen_->InvokeRuntime(
      GetThreadOffset<kArm64WordSize>(instruction->GetEntrypoint()).Int32Value(),
      instruction,
      instruction->GetDexPc(),
      nullptr);
  CheckEntrypointTypes<kQuickAllocObjectWithAccessCheck, void*, uint32_t, ArtMethod*>();
}

void LocationsBuilderARM64::VisitNot(HNot* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

void InstructionCodeGeneratorARM64::VisitNot(HNot* instruction) {
  switch (instruction->GetResultType()) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong:
      __ Mvn(OutputRegister(instruction), InputOperandAt(instruction, 0));
      break;

    default:
      LOG(FATAL) << "Unexpected type for not operation " << instruction->GetResultType();
  }
}

void LocationsBuilderARM64::VisitBooleanNot(HBooleanNot* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

void InstructionCodeGeneratorARM64::VisitBooleanNot(HBooleanNot* instruction) {
  __ Eor(OutputRegister(instruction), InputRegisterAt(instruction, 0), vixl::Operand(1));
}

void LocationsBuilderARM64::VisitNullCheck(HNullCheck* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  if (instruction->HasUses()) {
    locations->SetOut(Location::SameAsFirstInput());
  }
}

void InstructionCodeGeneratorARM64::GenerateImplicitNullCheck(HNullCheck* instruction) {
  if (codegen_->CanMoveNullCheckToUser(instruction)) {
    return;
  }

  BlockPoolsScope block_pools(GetVIXLAssembler());
  Location obj = instruction->GetLocations()->InAt(0);
  __ Ldr(wzr, HeapOperandFrom(obj, Offset(0)));
  codegen_->RecordPcInfo(instruction, instruction->GetDexPc());
}

void InstructionCodeGeneratorARM64::GenerateExplicitNullCheck(HNullCheck* instruction) {
  SlowPathCodeARM64* slow_path = new (GetGraph()->GetArena()) NullCheckSlowPathARM64(instruction);
  codegen_->AddSlowPath(slow_path);

  LocationSummary* locations = instruction->GetLocations();
  Location obj = locations->InAt(0);

  __ Cbz(RegisterFrom(obj, instruction->InputAt(0)->GetType()), slow_path->GetEntryLabel());
}

void InstructionCodeGeneratorARM64::VisitNullCheck(HNullCheck* instruction) {
  if (codegen_->GetCompilerOptions().GetImplicitNullChecks()) {
    GenerateImplicitNullCheck(instruction);
  } else {
    GenerateExplicitNullCheck(instruction);
  }
}

void LocationsBuilderARM64::VisitOr(HOr* instruction) {
  HandleBinaryOp(instruction);
}

void InstructionCodeGeneratorARM64::VisitOr(HOr* instruction) {
  HandleBinaryOp(instruction);
}

void LocationsBuilderARM64::VisitParallelMove(HParallelMove* instruction ATTRIBUTE_UNUSED) {
  LOG(FATAL) << "Unreachable";
}

void InstructionCodeGeneratorARM64::VisitParallelMove(HParallelMove* instruction) {
  codegen_->GetMoveResolver()->EmitNativeCode(instruction);
}

void LocationsBuilderARM64::VisitParameterValue(HParameterValue* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  Location location = parameter_visitor_.GetNextLocation(instruction->GetType());
  if (location.IsStackSlot()) {
    location = Location::StackSlot(location.GetStackIndex() + codegen_->GetFrameSize());
  } else if (location.IsDoubleStackSlot()) {
    location = Location::DoubleStackSlot(location.GetStackIndex() + codegen_->GetFrameSize());
  }
  locations->SetOut(location);
}

void InstructionCodeGeneratorARM64::VisitParameterValue(HParameterValue* instruction) {
  // Nothing to do, the parameter is already at its location.
  UNUSED(instruction);
}

void LocationsBuilderARM64::VisitPhi(HPhi* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  for (size_t i = 0, e = instruction->InputCount(); i < e; ++i) {
    locations->SetInAt(i, Location::Any());
  }
  locations->SetOut(Location::Any());
}

void InstructionCodeGeneratorARM64::VisitPhi(HPhi* instruction) {
  UNUSED(instruction);
  LOG(FATAL) << "Unreachable";
}

void LocationsBuilderARM64::VisitRem(HRem* rem) {
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
      locations->SetInAt(0, LocationFrom(calling_convention.GetFpuRegisterAt(0)));
      locations->SetInAt(1, LocationFrom(calling_convention.GetFpuRegisterAt(1)));
      locations->SetOut(calling_convention.GetReturnLocation(type));

      break;
    }

    default:
      LOG(FATAL) << "Unexpected rem type " << type;
  }
}

void InstructionCodeGeneratorARM64::VisitRem(HRem* rem) {
  Primitive::Type type = rem->GetResultType();

  switch (type) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong: {
      UseScratchRegisterScope temps(GetVIXLAssembler());
      Register dividend = InputRegisterAt(rem, 0);
      Register divisor = InputRegisterAt(rem, 1);
      Register output = OutputRegister(rem);
      Register temp = temps.AcquireSameSizeAs(output);

      __ Sdiv(temp, dividend, divisor);
      __ Msub(output, temp, divisor, dividend);
      break;
    }

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      int32_t entry_offset = (type == Primitive::kPrimFloat) ? QUICK_ENTRY_POINT(pFmodf)
                                                             : QUICK_ENTRY_POINT(pFmod);
      codegen_->InvokeRuntime(entry_offset, rem, rem->GetDexPc(), nullptr);
      break;
    }

    default:
      LOG(FATAL) << "Unexpected rem type " << type;
  }
}

void LocationsBuilderARM64::VisitMemoryBarrier(HMemoryBarrier* memory_barrier) {
  memory_barrier->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM64::VisitMemoryBarrier(HMemoryBarrier* memory_barrier) {
  GenerateMemoryBarrier(memory_barrier->GetBarrierKind());
}

void LocationsBuilderARM64::VisitReturn(HReturn* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  Primitive::Type return_type = instruction->InputAt(0)->GetType();
  locations->SetInAt(0, ARM64ReturnLocation(return_type));
}

void InstructionCodeGeneratorARM64::VisitReturn(HReturn* instruction) {
  UNUSED(instruction);
  codegen_->GenerateFrameExit();
}

void LocationsBuilderARM64::VisitReturnVoid(HReturnVoid* instruction) {
  instruction->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM64::VisitReturnVoid(HReturnVoid* instruction) {
  UNUSED(instruction);
  codegen_->GenerateFrameExit();
}

void LocationsBuilderARM64::VisitShl(HShl* shl) {
  HandleShift(shl);
}

void InstructionCodeGeneratorARM64::VisitShl(HShl* shl) {
  HandleShift(shl);
}

void LocationsBuilderARM64::VisitShr(HShr* shr) {
  HandleShift(shr);
}

void InstructionCodeGeneratorARM64::VisitShr(HShr* shr) {
  HandleShift(shr);
}

void LocationsBuilderARM64::VisitStoreLocal(HStoreLocal* store) {
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

void InstructionCodeGeneratorARM64::VisitStoreLocal(HStoreLocal* store) {
  UNUSED(store);
}

void LocationsBuilderARM64::VisitSub(HSub* instruction) {
  HandleBinaryOp(instruction);
}

void InstructionCodeGeneratorARM64::VisitSub(HSub* instruction) {
  HandleBinaryOp(instruction);
}

void LocationsBuilderARM64::VisitStaticFieldGet(HStaticFieldGet* instruction) {
  HandleFieldGet(instruction);
}

void InstructionCodeGeneratorARM64::VisitStaticFieldGet(HStaticFieldGet* instruction) {
  HandleFieldGet(instruction, instruction->GetFieldInfo());
}

void LocationsBuilderARM64::VisitStaticFieldSet(HStaticFieldSet* instruction) {
  HandleFieldSet(instruction);
}

void InstructionCodeGeneratorARM64::VisitStaticFieldSet(HStaticFieldSet* instruction) {
  HandleFieldSet(instruction, instruction->GetFieldInfo());
}

void LocationsBuilderARM64::VisitSuspendCheck(HSuspendCheck* instruction) {
  new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCallOnSlowPath);
}

void InstructionCodeGeneratorARM64::VisitSuspendCheck(HSuspendCheck* instruction) {
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

void LocationsBuilderARM64::VisitTemporary(HTemporary* temp) {
  temp->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM64::VisitTemporary(HTemporary* temp) {
  // Nothing to do, this is driven by the code generator.
  UNUSED(temp);
}

void LocationsBuilderARM64::VisitThrow(HThrow* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCall);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, LocationFrom(calling_convention.GetRegisterAt(0)));
}

void InstructionCodeGeneratorARM64::VisitThrow(HThrow* instruction) {
  codegen_->InvokeRuntime(
      QUICK_ENTRY_POINT(pDeliverException), instruction, instruction->GetDexPc(), nullptr);
  CheckEntrypointTypes<kQuickDeliverException, void, mirror::Object*>();
}

void LocationsBuilderARM64::VisitTypeConversion(HTypeConversion* conversion) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(conversion, LocationSummary::kNoCall);
  Primitive::Type input_type = conversion->GetInputType();
  Primitive::Type result_type = conversion->GetResultType();
  DCHECK_NE(input_type, result_type);
  if ((input_type == Primitive::kPrimNot) || (input_type == Primitive::kPrimVoid) ||
      (result_type == Primitive::kPrimNot) || (result_type == Primitive::kPrimVoid)) {
    LOG(FATAL) << "Unexpected type conversion from " << input_type << " to " << result_type;
  }

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

void InstructionCodeGeneratorARM64::VisitTypeConversion(HTypeConversion* conversion) {
  Primitive::Type result_type = conversion->GetResultType();
  Primitive::Type input_type = conversion->GetInputType();

  DCHECK_NE(input_type, result_type);

  if (Primitive::IsIntegralType(result_type) && Primitive::IsIntegralType(input_type)) {
    int result_size = Primitive::ComponentSize(result_type);
    int input_size = Primitive::ComponentSize(input_type);
    int min_size = std::min(result_size, input_size);
    Register output = OutputRegister(conversion);
    Register source = InputRegisterAt(conversion, 0);
    if ((result_type == Primitive::kPrimChar) && (input_size < result_size)) {
      __ Ubfx(output, source, 0, result_size * kBitsPerByte);
    } else if ((result_type == Primitive::kPrimChar) ||
               ((input_type == Primitive::kPrimChar) && (result_size > input_size))) {
      __ Ubfx(output, output.IsX() ? source.X() : source.W(), 0, min_size * kBitsPerByte);
    } else {
      __ Sbfx(output, output.IsX() ? source.X() : source.W(), 0, min_size * kBitsPerByte);
    }
  } else if (Primitive::IsFloatingPointType(result_type) && Primitive::IsIntegralType(input_type)) {
    __ Scvtf(OutputFPRegister(conversion), InputRegisterAt(conversion, 0));
  } else if (Primitive::IsIntegralType(result_type) && Primitive::IsFloatingPointType(input_type)) {
    CHECK(result_type == Primitive::kPrimInt || result_type == Primitive::kPrimLong);
    __ Fcvtzs(OutputRegister(conversion), InputFPRegisterAt(conversion, 0));
  } else if (Primitive::IsFloatingPointType(result_type) &&
             Primitive::IsFloatingPointType(input_type)) {
    __ Fcvt(OutputFPRegister(conversion), InputFPRegisterAt(conversion, 0));
  } else {
    LOG(FATAL) << "Unexpected or unimplemented type conversion from " << input_type
                << " to " << result_type;
  }
}

void LocationsBuilderARM64::VisitUShr(HUShr* ushr) {
  HandleShift(ushr);
}

void InstructionCodeGeneratorARM64::VisitUShr(HUShr* ushr) {
  HandleShift(ushr);
}

void LocationsBuilderARM64::VisitXor(HXor* instruction) {
  HandleBinaryOp(instruction);
}

void InstructionCodeGeneratorARM64::VisitXor(HXor* instruction) {
  HandleBinaryOp(instruction);
}

void LocationsBuilderARM64::VisitBoundType(HBoundType* instruction) {
  // Nothing to do, this should be removed during prepare for register allocator.
  UNUSED(instruction);
  LOG(FATAL) << "Unreachable";
}

void InstructionCodeGeneratorARM64::VisitBoundType(HBoundType* instruction) {
  // Nothing to do, this should be removed during prepare for register allocator.
  UNUSED(instruction);
  LOG(FATAL) << "Unreachable";
}

#undef __
#undef QUICK_ENTRY_POINT

}  // namespace arm64
}  // namespace art
