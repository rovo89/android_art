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
#include "code_generator_utils.h"
#include "compiled_method.h"
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
using helpers::ArtVixlRegCodeCoherentForRegSet;

static constexpr int kCurrentMethodStackOffset = 0;
// The compare/jump sequence will generate about (2 * num_entries + 1) instructions. While jump
// table version generates 7 instructions and num_entries literals. Compare/jump sequence will
// generates less code/data with a small num_entries.
static constexpr uint32_t kPackedSwitchJumpTableThreshold = 6;

inline Condition ARM64Condition(IfCondition cond) {
  switch (cond) {
    case kCondEQ: return eq;
    case kCondNE: return ne;
    case kCondLT: return lt;
    case kCondLE: return le;
    case kCondGT: return gt;
    case kCondGE: return ge;
    case kCondB:  return lo;
    case kCondBE: return ls;
    case kCondA:  return hi;
    case kCondAE: return hs;
  }
  LOG(FATAL) << "Unreachable";
  UNREACHABLE();
}

Location ARM64ReturnLocation(Primitive::Type return_type) {
  // Note that in practice, `LocationFrom(x0)` and `LocationFrom(w0)` create the
  // same Location object, and so do `LocationFrom(d0)` and `LocationFrom(s0)`,
  // but we use the exact registers for clarity.
  if (return_type == Primitive::kPrimFloat) {
    return LocationFrom(s0);
  } else if (return_type == Primitive::kPrimDouble) {
    return LocationFrom(d0);
  } else if (return_type == Primitive::kPrimLong) {
    return LocationFrom(x0);
  } else if (return_type == Primitive::kPrimVoid) {
    return Location::NoLocation();
  } else {
    return LocationFrom(w0);
  }
}

Location InvokeRuntimeCallingConvention::GetReturnLocation(Primitive::Type return_type) {
  return ARM64ReturnLocation(return_type);
}

#define __ down_cast<CodeGeneratorARM64*>(codegen)->GetVIXLAssembler()->
#define QUICK_ENTRY_POINT(x) QUICK_ENTRYPOINT_OFFSET(kArm64WordSize, x).Int32Value()

// Calculate memory accessing operand for save/restore live registers.
static void SaveRestoreLiveRegistersHelper(CodeGenerator* codegen,
                                           RegisterSet* register_set,
                                           int64_t spill_offset,
                                           bool is_save) {
  DCHECK(ArtVixlRegCodeCoherentForRegSet(register_set->GetCoreRegisters(),
                                         codegen->GetNumberOfCoreRegisters(),
                                         register_set->GetFloatingPointRegisters(),
                                         codegen->GetNumberOfFloatingPointRegisters()));

  CPURegList core_list = CPURegList(CPURegister::kRegister, kXRegSize,
      register_set->GetCoreRegisters() & (~callee_saved_core_registers.list()));
  CPURegList fp_list = CPURegList(CPURegister::kFPRegister, kDRegSize,
      register_set->GetFloatingPointRegisters() & (~callee_saved_fp_registers.list()));

  MacroAssembler* masm = down_cast<CodeGeneratorARM64*>(codegen)->GetVIXLAssembler();
  UseScratchRegisterScope temps(masm);

  Register base = masm->StackPointer();
  int64_t core_spill_size = core_list.TotalSizeInBytes();
  int64_t fp_spill_size = fp_list.TotalSizeInBytes();
  int64_t reg_size = kXRegSizeInBytes;
  int64_t max_ls_pair_offset = spill_offset + core_spill_size + fp_spill_size - 2 * reg_size;
  uint32_t ls_access_size = WhichPowerOf2(reg_size);
  if (((core_list.Count() > 1) || (fp_list.Count() > 1)) &&
      !masm->IsImmLSPair(max_ls_pair_offset, ls_access_size)) {
    // If the offset does not fit in the instruction's immediate field, use an alternate register
    // to compute the base address(float point registers spill base address).
    Register new_base = temps.AcquireSameSizeAs(base);
    __ Add(new_base, base, Operand(spill_offset + core_spill_size));
    base = new_base;
    spill_offset = -core_spill_size;
    int64_t new_max_ls_pair_offset = fp_spill_size - 2 * reg_size;
    DCHECK(masm->IsImmLSPair(spill_offset, ls_access_size));
    DCHECK(masm->IsImmLSPair(new_max_ls_pair_offset, ls_access_size));
  }

  if (is_save) {
    __ StoreCPURegList(core_list, MemOperand(base, spill_offset));
    __ StoreCPURegList(fp_list, MemOperand(base, spill_offset + core_spill_size));
  } else {
    __ LoadCPURegList(core_list, MemOperand(base, spill_offset));
    __ LoadCPURegList(fp_list, MemOperand(base, spill_offset + core_spill_size));
  }
}

void SlowPathCodeARM64::SaveLiveRegisters(CodeGenerator* codegen, LocationSummary* locations) {
  RegisterSet* register_set = locations->GetLiveRegisters();
  size_t stack_offset = codegen->GetFirstRegisterSlotInSlowPath();
  for (size_t i = 0, e = codegen->GetNumberOfCoreRegisters(); i < e; ++i) {
    if (!codegen->IsCoreCalleeSaveRegister(i) && register_set->ContainsCoreRegister(i)) {
      // If the register holds an object, update the stack mask.
      if (locations->RegisterContainsObject(i)) {
        locations->SetStackBit(stack_offset / kVRegSize);
      }
      DCHECK_LT(stack_offset, codegen->GetFrameSize() - codegen->FrameEntrySpillSize());
      DCHECK_LT(i, kMaximumNumberOfExpectedRegisters);
      saved_core_stack_offsets_[i] = stack_offset;
      stack_offset += kXRegSizeInBytes;
    }
  }

  for (size_t i = 0, e = codegen->GetNumberOfFloatingPointRegisters(); i < e; ++i) {
    if (!codegen->IsFloatingPointCalleeSaveRegister(i) &&
        register_set->ContainsFloatingPointRegister(i)) {
      DCHECK_LT(stack_offset, codegen->GetFrameSize() - codegen->FrameEntrySpillSize());
      DCHECK_LT(i, kMaximumNumberOfExpectedRegisters);
      saved_fpu_stack_offsets_[i] = stack_offset;
      stack_offset += kDRegSizeInBytes;
    }
  }

  SaveRestoreLiveRegistersHelper(codegen, register_set,
                                 codegen->GetFirstRegisterSlotInSlowPath(), true /* is_save */);
}

void SlowPathCodeARM64::RestoreLiveRegisters(CodeGenerator* codegen, LocationSummary* locations) {
  RegisterSet* register_set = locations->GetLiveRegisters();
  SaveRestoreLiveRegistersHelper(codegen, register_set,
                                 codegen->GetFirstRegisterSlotInSlowPath(), false /* is_save */);
}

class BoundsCheckSlowPathARM64 : public SlowPathCodeARM64 {
 public:
  explicit BoundsCheckSlowPathARM64(HBoundsCheck* instruction) : instruction_(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    LocationSummary* locations = instruction_->GetLocations();
    CodeGeneratorARM64* arm64_codegen = down_cast<CodeGeneratorARM64*>(codegen);

    __ Bind(GetEntryLabel());
    if (instruction_->CanThrowIntoCatchBlock()) {
      // Live registers will be restored in the catch block if caught.
      SaveLiveRegisters(codegen, instruction_->GetLocations());
    }
    // We're moving two locations to locations that could overlap, so we need a parallel
    // move resolver.
    InvokeRuntimeCallingConvention calling_convention;
    codegen->EmitParallelMoves(
        locations->InAt(0), LocationFrom(calling_convention.GetRegisterAt(0)), Primitive::kPrimInt,
        locations->InAt(1), LocationFrom(calling_convention.GetRegisterAt(1)), Primitive::kPrimInt);
    arm64_codegen->InvokeRuntime(
        QUICK_ENTRY_POINT(pThrowArrayBounds), instruction_, instruction_->GetDexPc(), this);
    CheckEntrypointTypes<kQuickThrowArrayBounds, void, int32_t, int32_t>();
  }

  bool IsFatal() const OVERRIDE { return true; }

  const char* GetDescription() const OVERRIDE { return "BoundsCheckSlowPathARM64"; }

 private:
  HBoundsCheck* const instruction_;

  DISALLOW_COPY_AND_ASSIGN(BoundsCheckSlowPathARM64);
};

class DivZeroCheckSlowPathARM64 : public SlowPathCodeARM64 {
 public:
  explicit DivZeroCheckSlowPathARM64(HDivZeroCheck* instruction) : instruction_(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    CodeGeneratorARM64* arm64_codegen = down_cast<CodeGeneratorARM64*>(codegen);
    __ Bind(GetEntryLabel());
    if (instruction_->CanThrowIntoCatchBlock()) {
      // Live registers will be restored in the catch block if caught.
      SaveLiveRegisters(codegen, instruction_->GetLocations());
    }
    arm64_codegen->InvokeRuntime(
        QUICK_ENTRY_POINT(pThrowDivZero), instruction_, instruction_->GetDexPc(), this);
    CheckEntrypointTypes<kQuickThrowDivZero, void, void>();
  }

  bool IsFatal() const OVERRIDE { return true; }

  const char* GetDescription() const OVERRIDE { return "DivZeroCheckSlowPathARM64"; }

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

  const char* GetDescription() const OVERRIDE { return "LoadClassSlowPathARM64"; }

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

  const char* GetDescription() const OVERRIDE { return "LoadStringSlowPathARM64"; }

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
    if (instruction_->CanThrowIntoCatchBlock()) {
      // Live registers will be restored in the catch block if caught.
      SaveLiveRegisters(codegen, instruction_->GetLocations());
    }
    arm64_codegen->InvokeRuntime(
        QUICK_ENTRY_POINT(pThrowNullPointer), instruction_, instruction_->GetDexPc(), this);
    CheckEntrypointTypes<kQuickThrowNullPointer, void, void>();
  }

  bool IsFatal() const OVERRIDE { return true; }

  const char* GetDescription() const OVERRIDE { return "NullCheckSlowPathARM64"; }

 private:
  HNullCheck* const instruction_;

  DISALLOW_COPY_AND_ASSIGN(NullCheckSlowPathARM64);
};

class SuspendCheckSlowPathARM64 : public SlowPathCodeARM64 {
 public:
  SuspendCheckSlowPathARM64(HSuspendCheck* instruction, HBasicBlock* successor)
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

  const char* GetDescription() const OVERRIDE { return "SuspendCheckSlowPathARM64"; }

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
  TypeCheckSlowPathARM64(HInstruction* instruction, bool is_fatal)
      : instruction_(instruction), is_fatal_(is_fatal) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    LocationSummary* locations = instruction_->GetLocations();
    Location class_to_check = locations->InAt(1);
    Location object_class = instruction_->IsCheckCast() ? locations->GetTemp(0)
                                                        : locations->Out();
    DCHECK(instruction_->IsCheckCast()
           || !locations->GetLiveRegisters()->ContainsCoreRegister(locations->Out().reg()));
    CodeGeneratorARM64* arm64_codegen = down_cast<CodeGeneratorARM64*>(codegen);
    uint32_t dex_pc = instruction_->GetDexPc();

    __ Bind(GetEntryLabel());

    if (instruction_->IsCheckCast()) {
      // The codegen for the instruction overwrites `temp`, so put it back in place.
      Register obj = InputRegisterAt(instruction_, 0);
      Register temp = WRegisterFrom(locations->GetTemp(0));
      uint32_t class_offset = mirror::Object::ClassOffset().Int32Value();
      __ Ldr(temp, HeapOperand(obj, class_offset));
      arm64_codegen->GetAssembler()->MaybeUnpoisonHeapReference(temp);
    }

    if (!is_fatal_) {
      SaveLiveRegisters(codegen, locations);
    }

    // We're moving two locations to locations that could overlap, so we need a parallel
    // move resolver.
    InvokeRuntimeCallingConvention calling_convention;
    codegen->EmitParallelMoves(
        class_to_check, LocationFrom(calling_convention.GetRegisterAt(0)), Primitive::kPrimNot,
        object_class, LocationFrom(calling_convention.GetRegisterAt(1)), Primitive::kPrimNot);

    if (instruction_->IsInstanceOf()) {
      arm64_codegen->InvokeRuntime(
          QUICK_ENTRY_POINT(pInstanceofNonTrivial), instruction_, dex_pc, this);
      CheckEntrypointTypes<kQuickInstanceofNonTrivial, uint32_t,
                           const mirror::Class*, const mirror::Class*>();
      Primitive::Type ret_type = instruction_->GetType();
      Location ret_loc = calling_convention.GetReturnLocation(ret_type);
      arm64_codegen->MoveLocation(locations->Out(), ret_loc, ret_type);
    } else {
      DCHECK(instruction_->IsCheckCast());
      arm64_codegen->InvokeRuntime(QUICK_ENTRY_POINT(pCheckCast), instruction_, dex_pc, this);
      CheckEntrypointTypes<kQuickCheckCast, void, const mirror::Class*, const mirror::Class*>();
    }

    if (!is_fatal_) {
      RestoreLiveRegisters(codegen, locations);
      __ B(GetExitLabel());
    }
  }

  const char* GetDescription() const OVERRIDE { return "TypeCheckSlowPathARM64"; }
  bool IsFatal() const { return is_fatal_; }

 private:
  HInstruction* const instruction_;
  const bool is_fatal_;

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
    CheckEntrypointTypes<kQuickDeoptimize, void, void>();
  }

  const char* GetDescription() const OVERRIDE { return "DeoptimizationSlowPathARM64"; }

 private:
  HInstruction* const instruction_;
  DISALLOW_COPY_AND_ASSIGN(DeoptimizationSlowPathARM64);
};

class ArraySetSlowPathARM64 : public SlowPathCodeARM64 {
 public:
  explicit ArraySetSlowPathARM64(HInstruction* instruction) : instruction_(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    LocationSummary* locations = instruction_->GetLocations();
    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, locations);

    InvokeRuntimeCallingConvention calling_convention;
    HParallelMove parallel_move(codegen->GetGraph()->GetArena());
    parallel_move.AddMove(
        locations->InAt(0),
        LocationFrom(calling_convention.GetRegisterAt(0)),
        Primitive::kPrimNot,
        nullptr);
    parallel_move.AddMove(
        locations->InAt(1),
        LocationFrom(calling_convention.GetRegisterAt(1)),
        Primitive::kPrimInt,
        nullptr);
    parallel_move.AddMove(
        locations->InAt(2),
        LocationFrom(calling_convention.GetRegisterAt(2)),
        Primitive::kPrimNot,
        nullptr);
    codegen->GetMoveResolver()->EmitNativeCode(&parallel_move);

    CodeGeneratorARM64* arm64_codegen = down_cast<CodeGeneratorARM64*>(codegen);
    arm64_codegen->InvokeRuntime(QUICK_ENTRY_POINT(pAputObject),
                                 instruction_,
                                 instruction_->GetDexPc(),
                                 this);
    CheckEntrypointTypes<kQuickAputObject, void, mirror::Array*, int32_t, mirror::Object*>();
    RestoreLiveRegisters(codegen, locations);
    __ B(GetExitLabel());
  }

  const char* GetDescription() const OVERRIDE { return "ArraySetSlowPathARM64"; }

 private:
  HInstruction* const instruction_;

  DISALLOW_COPY_AND_ASSIGN(ArraySetSlowPathARM64);
};

void JumpTableARM64::EmitTable(CodeGeneratorARM64* codegen) {
  uint32_t num_entries = switch_instr_->GetNumEntries();
  DCHECK_GE(num_entries, kPackedSwitchJumpTableThreshold);

  // We are about to use the assembler to place literals directly. Make sure we have enough
  // underlying code buffer and we have generated the jump table with right size.
  CodeBufferCheckScope scope(codegen->GetVIXLAssembler(), num_entries * sizeof(int32_t),
                             CodeBufferCheckScope::kCheck, CodeBufferCheckScope::kExactSize);

  __ Bind(&table_start_);
  const ArenaVector<HBasicBlock*>& successors = switch_instr_->GetBlock()->GetSuccessors();
  for (uint32_t i = 0; i < num_entries; i++) {
    vixl::Label* target_label = codegen->GetLabelOf(successors[i]);
    DCHECK(target_label->IsBound());
    ptrdiff_t jump_offset = target_label->location() - table_start_.location();
    DCHECK_GT(jump_offset, std::numeric_limits<int32_t>::min());
    DCHECK_LE(jump_offset, std::numeric_limits<int32_t>::max());
    Literal<int32_t> literal(jump_offset);
    __ place(&literal);
  }
}

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

Location InvokeDexCallingConventionVisitorARM64::GetMethodLocation() const {
  return LocationFrom(kArtMethodRegister);
}

CodeGeneratorARM64::CodeGeneratorARM64(HGraph* graph,
                                       const Arm64InstructionSetFeatures& isa_features,
                                       const CompilerOptions& compiler_options,
                                       OptimizingCompilerStats* stats)
    : CodeGenerator(graph,
                    kNumberOfAllocatableRegisters,
                    kNumberOfAllocatableFPRegisters,
                    kNumberOfAllocatableRegisterPairs,
                    callee_saved_core_registers.list(),
                    callee_saved_fp_registers.list(),
                    compiler_options,
                    stats),
      block_labels_(nullptr),
      jump_tables_(graph->GetArena()->Adapter(kArenaAllocCodeGenerator)),
      location_builder_(graph, this),
      instruction_visitor_(graph, this),
      move_resolver_(graph->GetArena(), this),
      isa_features_(isa_features),
      uint64_literals_(std::less<uint64_t>(),
                       graph->GetArena()->Adapter(kArenaAllocCodeGenerator)),
      method_patches_(MethodReferenceComparator(),
                      graph->GetArena()->Adapter(kArenaAllocCodeGenerator)),
      call_patches_(MethodReferenceComparator(),
                    graph->GetArena()->Adapter(kArenaAllocCodeGenerator)),
      relative_call_patches_(graph->GetArena()->Adapter(kArenaAllocCodeGenerator)),
      pc_relative_dex_cache_patches_(graph->GetArena()->Adapter(kArenaAllocCodeGenerator)) {
  // Save the link register (containing the return address) to mimic Quick.
  AddAllocatedRegister(LocationFrom(lr));
}

#define __ GetVIXLAssembler()->

void CodeGeneratorARM64::EmitJumpTables() {
  for (auto jump_table : jump_tables_) {
    jump_table->EmitTable(this);
  }
}

void CodeGeneratorARM64::Finalize(CodeAllocator* allocator) {
  EmitJumpTables();
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
  MoveOperands* move = moves_[index];
  codegen_->MoveLocation(move->GetDestination(), move->GetSource(), Primitive::kPrimVoid);
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

vixl::CPURegList CodeGeneratorARM64::GetFramePreservedCoreRegisters() const {
  DCHECK(ArtVixlRegCodeCoherentForRegSet(core_spill_mask_, GetNumberOfCoreRegisters(), 0, 0));
  return vixl::CPURegList(vixl::CPURegister::kRegister, vixl::kXRegSize,
                          core_spill_mask_);
}

vixl::CPURegList CodeGeneratorARM64::GetFramePreservedFPRegisters() const {
  DCHECK(ArtVixlRegCodeCoherentForRegSet(0, 0, fpu_spill_mask_,
                                         GetNumberOfFloatingPointRegisters()));
  return vixl::CPURegList(vixl::CPURegister::kFPRegister, vixl::kDRegSize,
                          fpu_spill_mask_);
}

void CodeGeneratorARM64::Bind(HBasicBlock* block) {
  __ Bind(GetLabelOf(block));
}

void CodeGeneratorARM64::Move(HInstruction* instruction,
                              Location location,
                              HInstruction* move_for) {
  LocationSummary* locations = instruction->GetLocations();
  Primitive::Type type = instruction->GetType();
  DCHECK_NE(type, Primitive::kPrimVoid);

  if (instruction->IsFakeString()) {
    // The fake string is an alias for null.
    DCHECK(IsBaseline());
    instruction = locations->Out().GetConstant();
    DCHECK(instruction->IsNullConstant()) << instruction->DebugName();
  }

  if (instruction->IsCurrentMethod()) {
    MoveLocation(location,
                 Location::DoubleStackSlot(kCurrentMethodStackOffset),
                 Primitive::kPrimVoid);
  } else if (locations != nullptr && locations->Out().Equals(location)) {
    return;
  } else if (instruction->IsIntConstant()
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

void CodeGeneratorARM64::MoveConstant(Location location, int32_t value) {
  DCHECK(location.IsRegister());
  __ Mov(RegisterFrom(location, Primitive::kPrimInt), value);
}

void CodeGeneratorARM64::AddLocationAsTemp(Location location, LocationSummary* locations) {
  if (location.IsRegister()) {
    locations->AddTemp(location);
  } else {
    UNIMPLEMENTED(FATAL) << "AddLocationAsTemp not implemented for location " << location;
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

void CodeGeneratorARM64::MarkGCCard(Register object, Register value, bool value_can_be_null) {
  UseScratchRegisterScope temps(GetVIXLAssembler());
  Register card = temps.AcquireX();
  Register temp = temps.AcquireW();   // Index within the CardTable - 32bit.
  vixl::Label done;
  if (value_can_be_null) {
    __ Cbz(value, &done);
  }
  __ Ldr(card, MemOperand(tr, Thread::CardTableOffset<kArm64WordSize>().Int32Value()));
  __ Lsr(temp, object, gc::accounting::CardTable::kCardShift);
  __ Strb(card, MemOperand(card, temp.X()));
  if (value_can_be_null) {
    __ Bind(&done);
  }
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
  }

  if (is_baseline || GetGraph()->IsDebuggable()) {
    // Stubs do not save callee-save floating point registers. If the graph
    // is debuggable, we need to deal with these registers differently. For
    // now, just block them.
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
  stream << XRegister(reg);
}

void CodeGeneratorARM64::DumpFloatingPointRegister(std::ostream& stream, int reg) const {
  stream << DRegister(reg);
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

void CodeGeneratorARM64::MoveLocation(Location destination,
                                      Location source,
                                      Primitive::Type dst_type) {
  if (source.Equals(destination)) {
    return;
  }

  // A valid move can always be inferred from the destination and source
  // locations. When moving from and to a register, the argument type can be
  // used to generate 32bit instead of 64bit moves. In debug mode we also
  // checks the coherency of the locations and the type.
  bool unspecified_type = (dst_type == Primitive::kPrimVoid);

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
    CPURegister dst = CPURegisterFrom(destination, dst_type);
    if (source.IsStackSlot() || source.IsDoubleStackSlot()) {
      DCHECK(dst.Is64Bits() == source.IsDoubleStackSlot());
      __ Ldr(dst, StackOperandFrom(source));
    } else if (source.IsConstant()) {
      DCHECK(CoherentConstantAndType(source, dst_type));
      MoveConstant(dst, source.GetConstant());
    } else if (source.IsRegister()) {
      if (destination.IsRegister()) {
        __ Mov(Register(dst), RegisterFrom(source, dst_type));
      } else {
        DCHECK(destination.IsFpuRegister());
        Primitive::Type source_type = Primitive::Is64BitType(dst_type)
            ? Primitive::kPrimLong
            : Primitive::kPrimInt;
        __ Fmov(FPRegisterFrom(destination, dst_type), RegisterFrom(source, source_type));
      }
    } else {
      DCHECK(source.IsFpuRegister());
      if (destination.IsRegister()) {
        Primitive::Type source_type = Primitive::Is64BitType(dst_type)
            ? Primitive::kPrimDouble
            : Primitive::kPrimFloat;
        __ Fmov(RegisterFrom(destination, dst_type), FPRegisterFrom(source, source_type));
      } else {
        DCHECK(destination.IsFpuRegister());
        __ Fmov(FPRegister(dst), FPRegisterFrom(source, dst_type));
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
      __ Str(CPURegisterFrom(source, dst_type), StackOperandFrom(destination));
    } else if (source.IsConstant()) {
      DCHECK(unspecified_type || CoherentConstantAndType(source, dst_type))
          << source << " " << dst_type;
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

void CodeGeneratorARM64::InvokeRuntime(QuickEntrypointEnum entrypoint,
                                       HInstruction* instruction,
                                       uint32_t dex_pc,
                                       SlowPathCode* slow_path) {
  InvokeRuntime(GetThreadOffset<kArm64WordSize>(entrypoint).Int32Value(),
                instruction,
                dex_pc,
                slow_path);
}

void CodeGeneratorARM64::InvokeRuntime(int32_t entry_point_offset,
                                       HInstruction* instruction,
                                       uint32_t dex_pc,
                                       SlowPathCode* slow_path) {
  ValidateInvokeRuntime(instruction, slow_path);
  BlockPoolsScope block_pools(GetVIXLAssembler());
  __ Ldr(lr, MemOperand(tr, entry_point_offset));
  __ Blr(lr);
  RecordPcInfo(instruction, dex_pc, slow_path);
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
  void InstructionCodeGeneratorARM64::Visit##name(H##name* instr ATTRIBUTE_UNUSED) {  \
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
  Primitive::Type field_type = field_info.GetFieldType();
  BlockPoolsScope block_pools(GetVIXLAssembler());

  MemOperand field = HeapOperand(InputRegisterAt(instruction, 0), field_info.GetFieldOffset());
  bool use_acquire_release = codegen_->GetInstructionSetFeatures().PreferAcquireRelease();

  if (field_info.IsVolatile()) {
    if (use_acquire_release) {
      // NB: LoadAcquire will record the pc info if needed.
      codegen_->LoadAcquire(instruction, OutputCPURegister(instruction), field);
    } else {
      codegen_->Load(field_type, OutputCPURegister(instruction), field);
      codegen_->MaybeRecordImplicitNullCheck(instruction);
      // For IRIW sequential consistency kLoadAny is not sufficient.
      GenerateMemoryBarrier(MemBarrierKind::kAnyAny);
    }
  } else {
    codegen_->Load(field_type, OutputCPURegister(instruction), field);
    codegen_->MaybeRecordImplicitNullCheck(instruction);
  }

  if (field_type == Primitive::kPrimNot) {
    GetAssembler()->MaybeUnpoisonHeapReference(OutputCPURegister(instruction).W());
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
                                                   const FieldInfo& field_info,
                                                   bool value_can_be_null) {
  DCHECK(instruction->IsInstanceFieldSet() || instruction->IsStaticFieldSet());
  BlockPoolsScope block_pools(GetVIXLAssembler());

  Register obj = InputRegisterAt(instruction, 0);
  CPURegister value = InputCPURegisterAt(instruction, 1);
  CPURegister source = value;
  Offset offset = field_info.GetFieldOffset();
  Primitive::Type field_type = field_info.GetFieldType();
  bool use_acquire_release = codegen_->GetInstructionSetFeatures().PreferAcquireRelease();

  {
    // We use a block to end the scratch scope before the write barrier, thus
    // freeing the temporary registers so they can be used in `MarkGCCard`.
    UseScratchRegisterScope temps(GetVIXLAssembler());

    if (kPoisonHeapReferences && field_type == Primitive::kPrimNot) {
      DCHECK(value.IsW());
      Register temp = temps.AcquireW();
      __ Mov(temp, value.W());
      GetAssembler()->PoisonHeapReference(temp.W());
      source = temp;
    }

    if (field_info.IsVolatile()) {
      if (use_acquire_release) {
        codegen_->StoreRelease(field_type, source, HeapOperand(obj, offset));
        codegen_->MaybeRecordImplicitNullCheck(instruction);
      } else {
        GenerateMemoryBarrier(MemBarrierKind::kAnyStore);
        codegen_->Store(field_type, source, HeapOperand(obj, offset));
        codegen_->MaybeRecordImplicitNullCheck(instruction);
        GenerateMemoryBarrier(MemBarrierKind::kAnyAny);
      }
    } else {
      codegen_->Store(field_type, source, HeapOperand(obj, offset));
      codegen_->MaybeRecordImplicitNullCheck(instruction);
    }
  }

  if (CodeGenerator::StoreNeedsWriteBarrier(field_type, instruction->InputAt(1))) {
    codegen_->MarkGCCard(obj, Register(value), value_can_be_null);
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

void LocationsBuilderARM64::VisitArm64IntermediateAddress(HArm64IntermediateAddress* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, ARM64EncodableConstantOrRegister(instruction->GetOffset(), instruction));
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorARM64::VisitArm64IntermediateAddress(
    HArm64IntermediateAddress* instruction) {
  __ Add(OutputRegister(instruction),
         InputRegisterAt(instruction, 0),
         Operand(InputOperandAt(instruction, 1)));
}

void LocationsBuilderARM64::VisitArm64MultiplyAccumulate(HArm64MultiplyAccumulate* instr) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instr, LocationSummary::kNoCall);
  locations->SetInAt(HArm64MultiplyAccumulate::kInputAccumulatorIndex,
                     Location::RequiresRegister());
  locations->SetInAt(HArm64MultiplyAccumulate::kInputMulLeftIndex, Location::RequiresRegister());
  locations->SetInAt(HArm64MultiplyAccumulate::kInputMulRightIndex, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

void InstructionCodeGeneratorARM64::VisitArm64MultiplyAccumulate(HArm64MultiplyAccumulate* instr) {
  Register res = OutputRegister(instr);
  Register accumulator = InputRegisterAt(instr, HArm64MultiplyAccumulate::kInputAccumulatorIndex);
  Register mul_left = InputRegisterAt(instr, HArm64MultiplyAccumulate::kInputMulLeftIndex);
  Register mul_right = InputRegisterAt(instr, HArm64MultiplyAccumulate::kInputMulRightIndex);

  // Avoid emitting code that could trigger Cortex A53's erratum 835769.
  // This fixup should be carried out for all multiply-accumulate instructions:
  // madd, msub, smaddl, smsubl, umaddl and umsubl.
  if (instr->GetType() == Primitive::kPrimLong &&
      codegen_->GetInstructionSetFeatures().NeedFixCortexA53_835769()) {
    MacroAssembler* masm = down_cast<CodeGeneratorARM64*>(codegen_)->GetVIXLAssembler();
    vixl::Instruction* prev = masm->GetCursorAddress<vixl::Instruction*>() - vixl::kInstructionSize;
    if (prev->IsLoadOrStore()) {
      // Make sure we emit only exactly one nop.
      vixl::CodeBufferCheckScope scope(masm,
                                       vixl::kInstructionSize,
                                       vixl::CodeBufferCheckScope::kCheck,
                                       vixl::CodeBufferCheckScope::kExactSize);
      __ nop();
    }
  }

  if (instr->GetOpKind() == HInstruction::kAdd) {
    __ Madd(res, mul_left, mul_right, accumulator);
  } else {
    DCHECK(instr->GetOpKind() == HInstruction::kSub);
    __ Msub(res, mul_left, mul_right, accumulator);
  }
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
  Primitive::Type type = instruction->GetType();
  Register obj = InputRegisterAt(instruction, 0);
  Location index = instruction->GetLocations()->InAt(1);
  size_t offset = mirror::Array::DataOffset(Primitive::ComponentSize(type)).Uint32Value();
  MemOperand source = HeapOperand(obj);
  CPURegister dest = OutputCPURegister(instruction);

  MacroAssembler* masm = GetVIXLAssembler();
  UseScratchRegisterScope temps(masm);
  // Block pools between `Load` and `MaybeRecordImplicitNullCheck`.
  BlockPoolsScope block_pools(masm);

  if (index.IsConstant()) {
    offset += Int64ConstantFrom(index) << Primitive::ComponentSizeShift(type);
    source = HeapOperand(obj, offset);
  } else {
    Register temp = temps.AcquireSameSizeAs(obj);
    if (instruction->GetArray()->IsArm64IntermediateAddress()) {
      // We do not need to compute the intermediate address from the array: the
      // input instruction has done it already. See the comment in
      // `InstructionSimplifierArm64::TryExtractArrayAccessAddress()`.
      if (kIsDebugBuild) {
        HArm64IntermediateAddress* tmp = instruction->GetArray()->AsArm64IntermediateAddress();
        DCHECK(tmp->GetOffset()->AsIntConstant()->GetValueAsUint64() == offset);
      }
      temp = obj;
    } else {
      __ Add(temp, obj, offset);
    }
    source = HeapOperand(temp, XRegisterFrom(index), LSL, Primitive::ComponentSizeShift(type));
  }

  codegen_->Load(type, dest, source);
  codegen_->MaybeRecordImplicitNullCheck(instruction);

  if (instruction->GetType() == Primitive::kPrimNot) {
    GetAssembler()->MaybeUnpoisonHeapReference(dest.W());
  }
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
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(
      instruction,
      instruction->NeedsTypeCheck() ? LocationSummary::kCallOnSlowPath : LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RegisterOrConstant(instruction->InputAt(1)));
  if (Primitive::IsFloatingPointType(instruction->InputAt(2)->GetType())) {
    locations->SetInAt(2, Location::RequiresFpuRegister());
  } else {
    locations->SetInAt(2, Location::RequiresRegister());
  }
}

void InstructionCodeGeneratorARM64::VisitArraySet(HArraySet* instruction) {
  Primitive::Type value_type = instruction->GetComponentType();
  LocationSummary* locations = instruction->GetLocations();
  bool may_need_runtime_call = locations->CanCall();
  bool needs_write_barrier =
      CodeGenerator::StoreNeedsWriteBarrier(value_type, instruction->GetValue());

  Register array = InputRegisterAt(instruction, 0);
  CPURegister value = InputCPURegisterAt(instruction, 2);
  CPURegister source = value;
  Location index = locations->InAt(1);
  size_t offset = mirror::Array::DataOffset(Primitive::ComponentSize(value_type)).Uint32Value();
  MemOperand destination = HeapOperand(array);
  MacroAssembler* masm = GetVIXLAssembler();
  BlockPoolsScope block_pools(masm);

  if (!needs_write_barrier) {
    DCHECK(!may_need_runtime_call);
    if (index.IsConstant()) {
      offset += Int64ConstantFrom(index) << Primitive::ComponentSizeShift(value_type);
      destination = HeapOperand(array, offset);
    } else {
      UseScratchRegisterScope temps(masm);
      Register temp = temps.AcquireSameSizeAs(array);
      if (instruction->GetArray()->IsArm64IntermediateAddress()) {
        // We do not need to compute the intermediate address from the array: the
        // input instruction has done it already. See the comment in
        // `InstructionSimplifierArm64::TryExtractArrayAccessAddress()`.
        if (kIsDebugBuild) {
          HArm64IntermediateAddress* tmp = instruction->GetArray()->AsArm64IntermediateAddress();
          DCHECK(tmp->GetOffset()->AsIntConstant()->GetValueAsUint64() == offset);
        }
        temp = array;
      } else {
        __ Add(temp, array, offset);
      }
      destination = HeapOperand(temp,
                                XRegisterFrom(index),
                                LSL,
                                Primitive::ComponentSizeShift(value_type));
    }
    codegen_->Store(value_type, value, destination);
    codegen_->MaybeRecordImplicitNullCheck(instruction);
  } else {
    DCHECK(needs_write_barrier);
    DCHECK(!instruction->GetArray()->IsArm64IntermediateAddress());
    vixl::Label done;
    SlowPathCodeARM64* slow_path = nullptr;
    {
      // We use a block to end the scratch scope before the write barrier, thus
      // freeing the temporary registers so they can be used in `MarkGCCard`.
      UseScratchRegisterScope temps(masm);
      Register temp = temps.AcquireSameSizeAs(array);
      if (index.IsConstant()) {
        offset += Int64ConstantFrom(index) << Primitive::ComponentSizeShift(value_type);
        destination = HeapOperand(array, offset);
      } else {
        destination = HeapOperand(temp,
                                  XRegisterFrom(index),
                                  LSL,
                                  Primitive::ComponentSizeShift(value_type));
      }

      uint32_t class_offset = mirror::Object::ClassOffset().Int32Value();
      uint32_t super_offset = mirror::Class::SuperClassOffset().Int32Value();
      uint32_t component_offset = mirror::Class::ComponentTypeOffset().Int32Value();

      if (may_need_runtime_call) {
        slow_path = new (GetGraph()->GetArena()) ArraySetSlowPathARM64(instruction);
        codegen_->AddSlowPath(slow_path);
        if (instruction->GetValueCanBeNull()) {
          vixl::Label non_zero;
          __ Cbnz(Register(value), &non_zero);
          if (!index.IsConstant()) {
            __ Add(temp, array, offset);
          }
          __ Str(wzr, destination);
          codegen_->MaybeRecordImplicitNullCheck(instruction);
          __ B(&done);
          __ Bind(&non_zero);
        }

        Register temp2 = temps.AcquireSameSizeAs(array);
        __ Ldr(temp, HeapOperand(array, class_offset));
        codegen_->MaybeRecordImplicitNullCheck(instruction);
        GetAssembler()->MaybeUnpoisonHeapReference(temp);
        __ Ldr(temp, HeapOperand(temp, component_offset));
        __ Ldr(temp2, HeapOperand(Register(value), class_offset));
        // No need to poison/unpoison, we're comparing two poisoned references.
        __ Cmp(temp, temp2);
        if (instruction->StaticTypeOfArrayIsObjectArray()) {
          vixl::Label do_put;
          __ B(eq, &do_put);
          GetAssembler()->MaybeUnpoisonHeapReference(temp);
          __ Ldr(temp, HeapOperand(temp, super_offset));
          // No need to unpoison, we're comparing against null.
          __ Cbnz(temp, slow_path->GetEntryLabel());
          __ Bind(&do_put);
        } else {
          __ B(ne, slow_path->GetEntryLabel());
        }
        temps.Release(temp2);
      }

      if (kPoisonHeapReferences) {
        Register temp2 = temps.AcquireSameSizeAs(array);
          DCHECK(value.IsW());
        __ Mov(temp2, value.W());
        GetAssembler()->PoisonHeapReference(temp2);
        source = temp2;
      }

      if (!index.IsConstant()) {
        __ Add(temp, array, offset);
      }
      __ Str(source, destination);

      if (!may_need_runtime_call) {
        codegen_->MaybeRecordImplicitNullCheck(instruction);
      }
    }

    codegen_->MarkGCCard(array, value.W(), instruction->GetValueCanBeNull());

    if (done.IsLinked()) {
      __ Bind(&done);
    }

    if (slow_path != nullptr) {
      __ Bind(slow_path->GetExitLabel());
    }
  }
}

void LocationsBuilderARM64::VisitBoundsCheck(HBoundsCheck* instruction) {
  LocationSummary::CallKind call_kind = instruction->CanThrowIntoCatchBlock()
      ? LocationSummary::kCallOnSlowPath
      : LocationSummary::kNoCall;
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction, call_kind);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, ARM64EncodableConstantOrRegister(instruction->InputAt(1), instruction));
  if (instruction->HasUses()) {
    locations->SetOut(Location::SameAsFirstInput());
  }
}

void InstructionCodeGeneratorARM64::VisitBoundsCheck(HBoundsCheck* instruction) {
  BoundsCheckSlowPathARM64* slow_path =
      new (GetGraph()->GetArena()) BoundsCheckSlowPathARM64(instruction);
  codegen_->AddSlowPath(slow_path);

  __ Cmp(InputRegisterAt(instruction, 0), InputOperandAt(instruction, 1));
  __ B(slow_path->GetEntryLabel(), hs);
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

static bool IsFloatingPointZeroConstant(HInstruction* instruction) {
  return (instruction->IsFloatConstant() && (instruction->AsFloatConstant()->GetValue() == 0.0f))
      || (instruction->IsDoubleConstant() && (instruction->AsDoubleConstant()->GetValue() == 0.0));
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
      locations->SetInAt(1,
                         IsFloatingPointZeroConstant(compare->InputAt(1))
                             ? Location::ConstantLocation(compare->InputAt(1)->AsConstant())
                             : Location::RequiresFpuRegister());
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
        DCHECK(IsFloatingPointZeroConstant(compare->GetLocations()->InAt(1).GetConstant()));
        // 0.0 is the only immediate that can be encoded directly in an FCMP instruction.
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

  if (Primitive::IsFloatingPointType(instruction->InputAt(0)->GetType())) {
    locations->SetInAt(0, Location::RequiresFpuRegister());
    locations->SetInAt(1,
                       IsFloatingPointZeroConstant(instruction->InputAt(1))
                           ? Location::ConstantLocation(instruction->InputAt(1)->AsConstant())
                           : Location::RequiresFpuRegister());
  } else {
    // Integer cases.
    locations->SetInAt(0, Location::RequiresRegister());
    locations->SetInAt(1, ARM64EncodableConstantOrRegister(instruction->InputAt(1), instruction));
  }

  if (instruction->NeedsMaterialization()) {
    locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
  }
}

void InstructionCodeGeneratorARM64::VisitCondition(HCondition* instruction) {
  if (!instruction->NeedsMaterialization()) {
    return;
  }

  LocationSummary* locations = instruction->GetLocations();
  Register res = RegisterFrom(locations->Out(), instruction->GetType());
  IfCondition if_cond = instruction->GetCondition();
  Condition arm64_cond = ARM64Condition(if_cond);

  if (Primitive::IsFloatingPointType(instruction->InputAt(0)->GetType())) {
    FPRegister lhs = InputFPRegisterAt(instruction, 0);
    if (locations->InAt(1).IsConstant()) {
      DCHECK(IsFloatingPointZeroConstant(locations->InAt(1).GetConstant()));
      // 0.0 is the only immediate that can be encoded directly in an FCMP instruction.
      __ Fcmp(lhs, 0.0);
    } else {
      __ Fcmp(lhs, InputFPRegisterAt(instruction, 1));
    }
    __ Cset(res, arm64_cond);
    if (instruction->IsFPConditionTrueIfNaN()) {
      // res = IsUnordered(arm64_cond) ? 1 : res  <=>  res = IsNotUnordered(arm64_cond) ? res : 1
      __ Csel(res, res, Operand(1), vc);  // VC for "not unordered".
    } else if (instruction->IsFPConditionFalseIfNaN()) {
      // res = IsUnordered(arm64_cond) ? 0 : res  <=>  res = IsNotUnordered(arm64_cond) ? res : 0
      __ Csel(res, res, Operand(0), vc);  // VC for "not unordered".
    }
  } else {
    // Integer cases.
    Register lhs = InputRegisterAt(instruction, 0);
    Operand rhs = InputOperandAt(instruction, 1);
    __ Cmp(lhs, rhs);
    __ Cset(res, arm64_cond);
  }
}

#define FOR_EACH_CONDITION_INSTRUCTION(M)                                                \
  M(Equal)                                                                               \
  M(NotEqual)                                                                            \
  M(LessThan)                                                                            \
  M(LessThanOrEqual)                                                                     \
  M(GreaterThan)                                                                         \
  M(GreaterThanOrEqual)                                                                  \
  M(Below)                                                                               \
  M(BelowOrEqual)                                                                        \
  M(Above)                                                                               \
  M(AboveOrEqual)
#define DEFINE_CONDITION_VISITORS(Name)                                                  \
void LocationsBuilderARM64::Visit##Name(H##Name* comp) { VisitCondition(comp); }         \
void InstructionCodeGeneratorARM64::Visit##Name(H##Name* comp) { VisitCondition(comp); }
FOR_EACH_CONDITION_INSTRUCTION(DEFINE_CONDITION_VISITORS)
#undef DEFINE_CONDITION_VISITORS
#undef FOR_EACH_CONDITION_INSTRUCTION

void InstructionCodeGeneratorARM64::DivRemOneOrMinusOne(HBinaryOperation* instruction) {
  DCHECK(instruction->IsDiv() || instruction->IsRem());

  LocationSummary* locations = instruction->GetLocations();
  Location second = locations->InAt(1);
  DCHECK(second.IsConstant());

  Register out = OutputRegister(instruction);
  Register dividend = InputRegisterAt(instruction, 0);
  int64_t imm = Int64FromConstant(second.GetConstant());
  DCHECK(imm == 1 || imm == -1);

  if (instruction->IsRem()) {
    __ Mov(out, 0);
  } else {
    if (imm == 1) {
      __ Mov(out, dividend);
    } else {
      __ Neg(out, dividend);
    }
  }
}

void InstructionCodeGeneratorARM64::DivRemByPowerOfTwo(HBinaryOperation* instruction) {
  DCHECK(instruction->IsDiv() || instruction->IsRem());

  LocationSummary* locations = instruction->GetLocations();
  Location second = locations->InAt(1);
  DCHECK(second.IsConstant());

  Register out = OutputRegister(instruction);
  Register dividend = InputRegisterAt(instruction, 0);
  int64_t imm = Int64FromConstant(second.GetConstant());
  uint64_t abs_imm = static_cast<uint64_t>(std::abs(imm));
  DCHECK(IsPowerOfTwo(abs_imm));
  int ctz_imm = CTZ(abs_imm);

  UseScratchRegisterScope temps(GetVIXLAssembler());
  Register temp = temps.AcquireSameSizeAs(out);

  if (instruction->IsDiv()) {
    __ Add(temp, dividend, abs_imm - 1);
    __ Cmp(dividend, 0);
    __ Csel(out, temp, dividend, lt);
    if (imm > 0) {
      __ Asr(out, out, ctz_imm);
    } else {
      __ Neg(out, Operand(out, ASR, ctz_imm));
    }
  } else {
    int bits = instruction->GetResultType() == Primitive::kPrimInt ? 32 : 64;
    __ Asr(temp, dividend, bits - 1);
    __ Lsr(temp, temp, bits - ctz_imm);
    __ Add(out, dividend, temp);
    __ And(out, out, abs_imm - 1);
    __ Sub(out, out, temp);
  }
}

void InstructionCodeGeneratorARM64::GenerateDivRemWithAnyConstant(HBinaryOperation* instruction) {
  DCHECK(instruction->IsDiv() || instruction->IsRem());

  LocationSummary* locations = instruction->GetLocations();
  Location second = locations->InAt(1);
  DCHECK(second.IsConstant());

  Register out = OutputRegister(instruction);
  Register dividend = InputRegisterAt(instruction, 0);
  int64_t imm = Int64FromConstant(second.GetConstant());

  Primitive::Type type = instruction->GetResultType();
  DCHECK(type == Primitive::kPrimInt || type == Primitive::kPrimLong);

  int64_t magic;
  int shift;
  CalculateMagicAndShiftForDivRem(imm, type == Primitive::kPrimLong /* is_long */, &magic, &shift);

  UseScratchRegisterScope temps(GetVIXLAssembler());
  Register temp = temps.AcquireSameSizeAs(out);

  // temp = get_high(dividend * magic)
  __ Mov(temp, magic);
  if (type == Primitive::kPrimLong) {
    __ Smulh(temp, dividend, temp);
  } else {
    __ Smull(temp.X(), dividend, temp);
    __ Lsr(temp.X(), temp.X(), 32);
  }

  if (imm > 0 && magic < 0) {
    __ Add(temp, temp, dividend);
  } else if (imm < 0 && magic > 0) {
    __ Sub(temp, temp, dividend);
  }

  if (shift != 0) {
    __ Asr(temp, temp, shift);
  }

  if (instruction->IsDiv()) {
    __ Sub(out, temp, Operand(temp, ASR, type == Primitive::kPrimLong ? 63 : 31));
  } else {
    __ Sub(temp, temp, Operand(temp, ASR, type == Primitive::kPrimLong ? 63 : 31));
    // TODO: Strength reduction for msub.
    Register temp_imm = temps.AcquireSameSizeAs(out);
    __ Mov(temp_imm, imm);
    __ Msub(out, temp, temp_imm, dividend);
  }
}

void InstructionCodeGeneratorARM64::GenerateDivRemIntegral(HBinaryOperation* instruction) {
  DCHECK(instruction->IsDiv() || instruction->IsRem());
  Primitive::Type type = instruction->GetResultType();
  DCHECK(type == Primitive::kPrimInt || Primitive::kPrimLong);

  LocationSummary* locations = instruction->GetLocations();
  Register out = OutputRegister(instruction);
  Location second = locations->InAt(1);

  if (second.IsConstant()) {
    int64_t imm = Int64FromConstant(second.GetConstant());

    if (imm == 0) {
      // Do not generate anything. DivZeroCheck would prevent any code to be executed.
    } else if (imm == 1 || imm == -1) {
      DivRemOneOrMinusOne(instruction);
    } else if (IsPowerOfTwo(std::abs(imm))) {
      DivRemByPowerOfTwo(instruction);
    } else {
      DCHECK(imm <= -2 || imm >= 2);
      GenerateDivRemWithAnyConstant(instruction);
    }
  } else {
    Register dividend = InputRegisterAt(instruction, 0);
    Register divisor = InputRegisterAt(instruction, 1);
    if (instruction->IsDiv()) {
      __ Sdiv(out, dividend, divisor);
    } else {
      UseScratchRegisterScope temps(GetVIXLAssembler());
      Register temp = temps.AcquireSameSizeAs(out);
      __ Sdiv(temp, dividend, divisor);
      __ Msub(out, temp, divisor, dividend);
    }
  }
}

void LocationsBuilderARM64::VisitDiv(HDiv* div) {
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

void InstructionCodeGeneratorARM64::VisitDiv(HDiv* div) {
  Primitive::Type type = div->GetResultType();
  switch (type) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong:
      GenerateDivRemIntegral(div);
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
  LocationSummary::CallKind call_kind = instruction->CanThrowIntoCatchBlock()
      ? LocationSummary::kCallOnSlowPath
      : LocationSummary::kNoCall;
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction, call_kind);
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

  if ((type == Primitive::kPrimBoolean) || !Primitive::IsIntegralType(type)) {
      LOG(FATAL) << "Unexpected type " << type << " for DivZeroCheck.";
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

void InstructionCodeGeneratorARM64::VisitDoubleConstant(
    HDoubleConstant* constant ATTRIBUTE_UNUSED) {
  // Will be generated at use site.
}

void LocationsBuilderARM64::VisitExit(HExit* exit) {
  exit->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM64::VisitExit(HExit* exit ATTRIBUTE_UNUSED) {
}

void LocationsBuilderARM64::VisitFloatConstant(HFloatConstant* constant) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(constant, LocationSummary::kNoCall);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorARM64::VisitFloatConstant(HFloatConstant* constant ATTRIBUTE_UNUSED) {
  // Will be generated at use site.
}

void InstructionCodeGeneratorARM64::HandleGoto(HInstruction* got, HBasicBlock* successor) {
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

void LocationsBuilderARM64::VisitGoto(HGoto* got) {
  got->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM64::VisitGoto(HGoto* got) {
  HandleGoto(got, got->GetSuccessor());
}

void LocationsBuilderARM64::VisitTryBoundary(HTryBoundary* try_boundary) {
  try_boundary->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM64::VisitTryBoundary(HTryBoundary* try_boundary) {
  HBasicBlock* successor = try_boundary->GetNormalFlowSuccessor();
  if (!successor->IsExitBlock()) {
    HandleGoto(try_boundary, successor);
  }
}

void InstructionCodeGeneratorARM64::GenerateTestAndBranch(HInstruction* instruction,
                                                          size_t condition_input_index,
                                                          vixl::Label* true_target,
                                                          vixl::Label* false_target) {
  // FP branching requires both targets to be explicit. If either of the targets
  // is nullptr (fallthrough) use and bind `fallthrough_target` instead.
  vixl::Label fallthrough_target;
  HInstruction* cond = instruction->InputAt(condition_input_index);

  if (true_target == nullptr && false_target == nullptr) {
    // Nothing to do. The code always falls through.
    return;
  } else if (cond->IsIntConstant()) {
    // Constant condition, statically compared against 1.
    if (cond->AsIntConstant()->IsOne()) {
      if (true_target != nullptr) {
        __ B(true_target);
      }
    } else {
      DCHECK(cond->AsIntConstant()->IsZero());
      if (false_target != nullptr) {
        __ B(false_target);
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
      __ Cbz(InputRegisterAt(instruction, condition_input_index), false_target);
    } else {
      __ Cbnz(InputRegisterAt(instruction, condition_input_index), true_target);
    }
  } else {
    // The condition instruction has not been materialized, use its inputs as
    // the comparison and its condition as the branch condition.
    HCondition* condition = cond->AsCondition();

    Primitive::Type type = condition->InputAt(0)->GetType();
    if (Primitive::IsFloatingPointType(type)) {
      FPRegister lhs = InputFPRegisterAt(condition, 0);
      if (condition->GetLocations()->InAt(1).IsConstant()) {
        DCHECK(IsFloatingPointZeroConstant(condition->GetLocations()->InAt(1).GetConstant()));
        // 0.0 is the only immediate that can be encoded directly in an FCMP instruction.
        __ Fcmp(lhs, 0.0);
      } else {
        __ Fcmp(lhs, InputFPRegisterAt(condition, 1));
      }
      if (condition->IsFPConditionTrueIfNaN()) {
        __ B(vs, true_target == nullptr ? &fallthrough_target : true_target);
      } else if (condition->IsFPConditionFalseIfNaN()) {
        __ B(vs, false_target == nullptr ? &fallthrough_target : false_target);
      }
      if (true_target == nullptr) {
        __ B(ARM64Condition(condition->GetOppositeCondition()), false_target);
      } else {
        __ B(ARM64Condition(condition->GetCondition()), true_target);
      }
    } else {
      // Integer cases.
      Register lhs = InputRegisterAt(condition, 0);
      Operand rhs = InputOperandAt(condition, 1);

      Condition arm64_cond;
      vixl::Label* non_fallthrough_target;
      if (true_target == nullptr) {
        arm64_cond = ARM64Condition(condition->GetOppositeCondition());
        non_fallthrough_target = false_target;
      } else {
        arm64_cond = ARM64Condition(condition->GetCondition());
        non_fallthrough_target = true_target;
      }

      if ((arm64_cond != gt && arm64_cond != le) && rhs.IsImmediate() && (rhs.immediate() == 0)) {
        switch (arm64_cond) {
          case eq:
            __ Cbz(lhs, non_fallthrough_target);
            break;
          case ne:
            __ Cbnz(lhs, non_fallthrough_target);
            break;
          case lt:
            // Test the sign bit and branch accordingly.
            __ Tbnz(lhs, (lhs.IsX() ? kXRegSize : kWRegSize) - 1, non_fallthrough_target);
            break;
          case ge:
            // Test the sign bit and branch accordingly.
            __ Tbz(lhs, (lhs.IsX() ? kXRegSize : kWRegSize) - 1, non_fallthrough_target);
            break;
          default:
            // Without the `static_cast` the compiler throws an error for
            // `-Werror=sign-promo`.
            LOG(FATAL) << "Unexpected condition: " << static_cast<int>(arm64_cond);
        }
      } else {
        __ Cmp(lhs, rhs);
        __ B(arm64_cond, non_fallthrough_target);
      }
    }
  }

  // If neither branch falls through (case 3), the conditional branch to `true_target`
  // was already emitted (case 2) and we need to emit a jump to `false_target`.
  if (true_target != nullptr && false_target != nullptr) {
    __ B(false_target);
  }

  if (fallthrough_target.IsLinked()) {
    __ Bind(&fallthrough_target);
  }
}

void LocationsBuilderARM64::VisitIf(HIf* if_instr) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(if_instr);
  if (IsBooleanValueOrMaterializedCondition(if_instr->InputAt(0))) {
    locations->SetInAt(0, Location::RequiresRegister());
  }
}

void InstructionCodeGeneratorARM64::VisitIf(HIf* if_instr) {
  HBasicBlock* true_successor = if_instr->IfTrueSuccessor();
  HBasicBlock* false_successor = if_instr->IfFalseSuccessor();
  vixl::Label* true_target = codegen_->GoesToNextBlock(if_instr->GetBlock(), true_successor) ?
      nullptr : codegen_->GetLabelOf(true_successor);
  vixl::Label* false_target = codegen_->GoesToNextBlock(if_instr->GetBlock(), false_successor) ?
      nullptr : codegen_->GetLabelOf(false_successor);
  GenerateTestAndBranch(if_instr, /* condition_input_index */ 0, true_target, false_target);
}

void LocationsBuilderARM64::VisitDeoptimize(HDeoptimize* deoptimize) {
  LocationSummary* locations = new (GetGraph()->GetArena())
      LocationSummary(deoptimize, LocationSummary::kCallOnSlowPath);
  if (IsBooleanValueOrMaterializedCondition(deoptimize->InputAt(0))) {
    locations->SetInAt(0, Location::RequiresRegister());
  }
}

void InstructionCodeGeneratorARM64::VisitDeoptimize(HDeoptimize* deoptimize) {
  SlowPathCodeARM64* slow_path = new (GetGraph()->GetArena())
      DeoptimizationSlowPathARM64(deoptimize);
  codegen_->AddSlowPath(slow_path);
  GenerateTestAndBranch(deoptimize,
                        /* condition_input_index */ 0,
                        slow_path->GetEntryLabel(),
                        /* false_target */ nullptr);
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
  HandleFieldSet(instruction, instruction->GetFieldInfo(), instruction->GetValueCanBeNull());
}

void LocationsBuilderARM64::VisitInstanceOf(HInstanceOf* instruction) {
  LocationSummary::CallKind call_kind = LocationSummary::kNoCall;
  switch (instruction->GetTypeCheckKind()) {
    case TypeCheckKind::kExactCheck:
    case TypeCheckKind::kAbstractClassCheck:
    case TypeCheckKind::kClassHierarchyCheck:
    case TypeCheckKind::kArrayObjectCheck:
      call_kind = LocationSummary::kNoCall;
      break;
    case TypeCheckKind::kUnresolvedCheck:
    case TypeCheckKind::kInterfaceCheck:
      call_kind = LocationSummary::kCall;
      break;
    case TypeCheckKind::kArrayCheck:
      call_kind = LocationSummary::kCallOnSlowPath;
      break;
  }
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction, call_kind);
  if (call_kind != LocationSummary::kCall) {
    locations->SetInAt(0, Location::RequiresRegister());
    locations->SetInAt(1, Location::RequiresRegister());
    // The out register is used as a temporary, so it overlaps with the inputs.
    // Note that TypeCheckSlowPathARM64 uses this register too.
    locations->SetOut(Location::RequiresRegister(), Location::kOutputOverlap);
  } else {
    InvokeRuntimeCallingConvention calling_convention;
    locations->SetInAt(1, LocationFrom(calling_convention.GetRegisterAt(0)));
    locations->SetInAt(0, LocationFrom(calling_convention.GetRegisterAt(1)));
    locations->SetOut(calling_convention.GetReturnLocation(Primitive::kPrimInt));
  }
}

void InstructionCodeGeneratorARM64::VisitInstanceOf(HInstanceOf* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Register obj = InputRegisterAt(instruction, 0);
  Register cls = InputRegisterAt(instruction, 1);
  Register out = OutputRegister(instruction);
  uint32_t class_offset = mirror::Object::ClassOffset().Int32Value();
  uint32_t super_offset = mirror::Class::SuperClassOffset().Int32Value();
  uint32_t component_offset = mirror::Class::ComponentTypeOffset().Int32Value();
  uint32_t primitive_offset = mirror::Class::PrimitiveTypeOffset().Int32Value();

  vixl::Label done, zero;
  SlowPathCodeARM64* slow_path = nullptr;

  // Return 0 if `obj` is null.
  // Avoid null check if we know `obj` is not null.
  if (instruction->MustDoNullCheck()) {
    __ Cbz(obj, &zero);
  }

  // In case of an interface/unresolved check, we put the object class into the object register.
  // This is safe, as the register is caller-save, and the object must be in another
  // register if it survives the runtime call.
  Register target = (instruction->GetTypeCheckKind() == TypeCheckKind::kInterfaceCheck) ||
      (instruction->GetTypeCheckKind() == TypeCheckKind::kUnresolvedCheck)
      ? obj
      : out;
  __ Ldr(target, HeapOperand(obj.W(), class_offset));
  GetAssembler()->MaybeUnpoisonHeapReference(target);

  switch (instruction->GetTypeCheckKind()) {
    case TypeCheckKind::kExactCheck: {
      __ Cmp(out, cls);
      __ Cset(out, eq);
      if (zero.IsLinked()) {
        __ B(&done);
      }
      break;
    }
    case TypeCheckKind::kAbstractClassCheck: {
      // If the class is abstract, we eagerly fetch the super class of the
      // object to avoid doing a comparison we know will fail.
      vixl::Label loop, success;
      __ Bind(&loop);
      __ Ldr(out, HeapOperand(out, super_offset));
      GetAssembler()->MaybeUnpoisonHeapReference(out);
      // If `out` is null, we use it for the result, and jump to `done`.
      __ Cbz(out, &done);
      __ Cmp(out, cls);
      __ B(ne, &loop);
      __ Mov(out, 1);
      if (zero.IsLinked()) {
        __ B(&done);
      }
      break;
    }
    case TypeCheckKind::kClassHierarchyCheck: {
      // Walk over the class hierarchy to find a match.
      vixl::Label loop, success;
      __ Bind(&loop);
      __ Cmp(out, cls);
      __ B(eq, &success);
      __ Ldr(out, HeapOperand(out, super_offset));
      GetAssembler()->MaybeUnpoisonHeapReference(out);
      __ Cbnz(out, &loop);
      // If `out` is null, we use it for the result, and jump to `done`.
      __ B(&done);
      __ Bind(&success);
      __ Mov(out, 1);
      if (zero.IsLinked()) {
        __ B(&done);
      }
      break;
    }
    case TypeCheckKind::kArrayObjectCheck: {
      // Do an exact check.
      vixl::Label exact_check;
      __ Cmp(out, cls);
      __ B(eq, &exact_check);
      // Otherwise, we need to check that the object's class is a non primitive array.
      __ Ldr(out, HeapOperand(out, component_offset));
      GetAssembler()->MaybeUnpoisonHeapReference(out);
      // If `out` is null, we use it for the result, and jump to `done`.
      __ Cbz(out, &done);
      __ Ldrh(out, HeapOperand(out, primitive_offset));
      static_assert(Primitive::kPrimNot == 0, "Expected 0 for kPrimNot");
      __ Cbnz(out, &zero);
      __ Bind(&exact_check);
      __ Mov(out, 1);
      __ B(&done);
      break;
    }
    case TypeCheckKind::kArrayCheck: {
      __ Cmp(out, cls);
      DCHECK(locations->OnlyCallsOnSlowPath());
      slow_path = new (GetGraph()->GetArena()) TypeCheckSlowPathARM64(
          instruction, /* is_fatal */ false);
      codegen_->AddSlowPath(slow_path);
      __ B(ne, slow_path->GetEntryLabel());
      __ Mov(out, 1);
      if (zero.IsLinked()) {
        __ B(&done);
      }
      break;
    }
    case TypeCheckKind::kUnresolvedCheck:
    case TypeCheckKind::kInterfaceCheck:
    default: {
      codegen_->InvokeRuntime(QUICK_ENTRY_POINT(pInstanceofNonTrivial),
                              instruction,
                              instruction->GetDexPc(),
                              nullptr);
      if (zero.IsLinked()) {
        __ B(&done);
      }
      break;
    }
  }

  if (zero.IsLinked()) {
    __ Bind(&zero);
    __ Mov(out, 0);
  }

  if (done.IsLinked()) {
    __ Bind(&done);
  }

  if (slow_path != nullptr) {
    __ Bind(slow_path->GetExitLabel());
  }
}

void LocationsBuilderARM64::VisitCheckCast(HCheckCast* instruction) {
  LocationSummary::CallKind call_kind = LocationSummary::kNoCall;
  bool throws_into_catch = instruction->CanThrowIntoCatchBlock();

  switch (instruction->GetTypeCheckKind()) {
    case TypeCheckKind::kExactCheck:
    case TypeCheckKind::kAbstractClassCheck:
    case TypeCheckKind::kClassHierarchyCheck:
    case TypeCheckKind::kArrayObjectCheck:
      call_kind = throws_into_catch
          ? LocationSummary::kCallOnSlowPath
          : LocationSummary::kNoCall;
      break;
    case TypeCheckKind::kUnresolvedCheck:
    case TypeCheckKind::kInterfaceCheck:
      call_kind = LocationSummary::kCall;
      break;
    case TypeCheckKind::kArrayCheck:
      call_kind = LocationSummary::kCallOnSlowPath;
      break;
  }

  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(
      instruction, call_kind);
  if (call_kind != LocationSummary::kCall) {
    locations->SetInAt(0, Location::RequiresRegister());
    locations->SetInAt(1, Location::RequiresRegister());
    // Note that TypeCheckSlowPathARM64 uses this register too.
    locations->AddTemp(Location::RequiresRegister());
  } else {
    InvokeRuntimeCallingConvention calling_convention;
    locations->SetInAt(1, LocationFrom(calling_convention.GetRegisterAt(0)));
    locations->SetInAt(0, LocationFrom(calling_convention.GetRegisterAt(1)));
  }
}

void InstructionCodeGeneratorARM64::VisitCheckCast(HCheckCast* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Register obj = InputRegisterAt(instruction, 0);
  Register cls = InputRegisterAt(instruction, 1);
  Register temp;
  if (!locations->WillCall()) {
    temp = WRegisterFrom(instruction->GetLocations()->GetTemp(0));
  }

  uint32_t class_offset = mirror::Object::ClassOffset().Int32Value();
  uint32_t super_offset = mirror::Class::SuperClassOffset().Int32Value();
  uint32_t component_offset = mirror::Class::ComponentTypeOffset().Int32Value();
  uint32_t primitive_offset = mirror::Class::PrimitiveTypeOffset().Int32Value();
  SlowPathCodeARM64* slow_path = nullptr;

  if (!locations->WillCall()) {
    slow_path = new (GetGraph()->GetArena()) TypeCheckSlowPathARM64(
        instruction, !locations->CanCall());
    codegen_->AddSlowPath(slow_path);
  }

  vixl::Label done;
  // Avoid null check if we know obj is not null.
  if (instruction->MustDoNullCheck()) {
    __ Cbz(obj, &done);
  }

  if (locations->WillCall()) {
    __ Ldr(obj, HeapOperand(obj, class_offset));
    GetAssembler()->MaybeUnpoisonHeapReference(obj);
  } else {
    __ Ldr(temp, HeapOperand(obj, class_offset));
    GetAssembler()->MaybeUnpoisonHeapReference(temp);
  }

  switch (instruction->GetTypeCheckKind()) {
    case TypeCheckKind::kExactCheck:
    case TypeCheckKind::kArrayCheck: {
      __ Cmp(temp, cls);
      // Jump to slow path for throwing the exception or doing a
      // more involved array check.
      __ B(ne, slow_path->GetEntryLabel());
      break;
    }
    case TypeCheckKind::kAbstractClassCheck: {
      // If the class is abstract, we eagerly fetch the super class of the
      // object to avoid doing a comparison we know will fail.
      vixl::Label loop;
      __ Bind(&loop);
      __ Ldr(temp, HeapOperand(temp, super_offset));
      GetAssembler()->MaybeUnpoisonHeapReference(temp);
      // Jump to the slow path to throw the exception.
      __ Cbz(temp, slow_path->GetEntryLabel());
      __ Cmp(temp, cls);
      __ B(ne, &loop);
      break;
    }
    case TypeCheckKind::kClassHierarchyCheck: {
      // Walk over the class hierarchy to find a match.
      vixl::Label loop;
      __ Bind(&loop);
      __ Cmp(temp, cls);
      __ B(eq, &done);
      __ Ldr(temp, HeapOperand(temp, super_offset));
      GetAssembler()->MaybeUnpoisonHeapReference(temp);
      __ Cbnz(temp, &loop);
      // Jump to the slow path to throw the exception.
      __ B(slow_path->GetEntryLabel());
      break;
    }
    case TypeCheckKind::kArrayObjectCheck: {
      // Do an exact check.
      __ Cmp(temp, cls);
      __ B(eq, &done);
      // Otherwise, we need to check that the object's class is a non primitive array.
      __ Ldr(temp, HeapOperand(temp, component_offset));
      GetAssembler()->MaybeUnpoisonHeapReference(temp);
      __ Cbz(temp, slow_path->GetEntryLabel());
      __ Ldrh(temp, HeapOperand(temp, primitive_offset));
      static_assert(Primitive::kPrimNot == 0, "Expected 0 for kPrimNot");
      __ Cbnz(temp, slow_path->GetEntryLabel());
      break;
    }
    case TypeCheckKind::kUnresolvedCheck:
    case TypeCheckKind::kInterfaceCheck:
    default:
      codegen_->InvokeRuntime(QUICK_ENTRY_POINT(pCheckCast),
                              instruction,
                              instruction->GetDexPc(),
                              nullptr);
      break;
  }
  __ Bind(&done);

  if (slow_path != nullptr) {
    __ Bind(slow_path->GetExitLabel());
  }
}

void LocationsBuilderARM64::VisitIntConstant(HIntConstant* constant) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(constant);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorARM64::VisitIntConstant(HIntConstant* constant ATTRIBUTE_UNUSED) {
  // Will be generated at use site.
}

void LocationsBuilderARM64::VisitNullConstant(HNullConstant* constant) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(constant);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorARM64::VisitNullConstant(HNullConstant* constant ATTRIBUTE_UNUSED) {
  // Will be generated at use site.
}

void LocationsBuilderARM64::VisitInvokeUnresolved(HInvokeUnresolved* invoke) {
  // The trampoline uses the same calling convention as dex calling conventions,
  // except instead of loading arg0/r0 with the target Method*, arg0/r0 will contain
  // the method_idx.
  HandleInvoke(invoke);
}

void InstructionCodeGeneratorARM64::VisitInvokeUnresolved(HInvokeUnresolved* invoke) {
  codegen_->GenerateInvokeUnresolvedRuntimeCall(invoke);
}

void LocationsBuilderARM64::HandleInvoke(HInvoke* invoke) {
  InvokeDexCallingConventionVisitorARM64 calling_convention_visitor;
  CodeGenerator::CreateCommonInvokeLocationSummary(invoke, &calling_convention_visitor);
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
  GetAssembler()->MaybeUnpoisonHeapReference(temp.W());
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

HInvokeStaticOrDirect::DispatchInfo CodeGeneratorARM64::GetSupportedInvokeStaticOrDirectDispatch(
      const HInvokeStaticOrDirect::DispatchInfo& desired_dispatch_info,
      MethodReference target_method ATTRIBUTE_UNUSED) {
  // On arm64 we support all dispatch types.
  return desired_dispatch_info;
}

void CodeGeneratorARM64::GenerateStaticOrDirectCall(HInvokeStaticOrDirect* invoke, Location temp) {
  // For better instruction scheduling we load the direct code pointer before the method pointer.
  bool direct_code_loaded = false;
  switch (invoke->GetCodePtrLocation()) {
    case HInvokeStaticOrDirect::CodePtrLocation::kCallDirectWithFixup:
      // LR = code address from literal pool with link-time patch.
      __ Ldr(lr, DeduplicateMethodCodeLiteral(invoke->GetTargetMethod()));
      direct_code_loaded = true;
      break;
    case HInvokeStaticOrDirect::CodePtrLocation::kCallDirect:
      // LR = invoke->GetDirectCodePtr();
      __ Ldr(lr, DeduplicateUint64Literal(invoke->GetDirectCodePtr()));
      direct_code_loaded = true;
      break;
    default:
      break;
  }

  // Make sure that ArtMethod* is passed in kArtMethodRegister as per the calling convention.
  Location callee_method = temp;  // For all kinds except kRecursive, callee will be in temp.
  switch (invoke->GetMethodLoadKind()) {
    case HInvokeStaticOrDirect::MethodLoadKind::kStringInit:
      // temp = thread->string_init_entrypoint
      __ Ldr(XRegisterFrom(temp), MemOperand(tr, invoke->GetStringInitOffset()));
      break;
    case HInvokeStaticOrDirect::MethodLoadKind::kRecursive:
      callee_method = invoke->GetLocations()->InAt(invoke->GetSpecialInputIndex());
      break;
    case HInvokeStaticOrDirect::MethodLoadKind::kDirectAddress:
      // Load method address from literal pool.
      __ Ldr(XRegisterFrom(temp), DeduplicateUint64Literal(invoke->GetMethodAddress()));
      break;
    case HInvokeStaticOrDirect::MethodLoadKind::kDirectAddressWithFixup:
      // Load method address from literal pool with a link-time patch.
      __ Ldr(XRegisterFrom(temp),
             DeduplicateMethodAddressLiteral(invoke->GetTargetMethod()));
      break;
    case HInvokeStaticOrDirect::MethodLoadKind::kDexCachePcRelative: {
      // Add ADRP with its PC-relative DexCache access patch.
      pc_relative_dex_cache_patches_.emplace_back(*invoke->GetTargetMethod().dex_file,
                                                  invoke->GetDexCacheArrayOffset());
      vixl::Label* pc_insn_label = &pc_relative_dex_cache_patches_.back().label;
      {
        vixl::SingleEmissionCheckScope guard(GetVIXLAssembler());
        __ Bind(pc_insn_label);
        __ adrp(XRegisterFrom(temp), 0);
      }
      pc_relative_dex_cache_patches_.back().pc_insn_label = pc_insn_label;
      // Add LDR with its PC-relative DexCache access patch.
      pc_relative_dex_cache_patches_.emplace_back(*invoke->GetTargetMethod().dex_file,
                                                  invoke->GetDexCacheArrayOffset());
      {
        vixl::SingleEmissionCheckScope guard(GetVIXLAssembler());
        __ Bind(&pc_relative_dex_cache_patches_.back().label);
        __ ldr(XRegisterFrom(temp), MemOperand(XRegisterFrom(temp), 0));
        pc_relative_dex_cache_patches_.back().pc_insn_label = pc_insn_label;
      }
      break;
    }
    case HInvokeStaticOrDirect::MethodLoadKind::kDexCacheViaMethod: {
      Location current_method = invoke->GetLocations()->InAt(invoke->GetSpecialInputIndex());
      Register reg = XRegisterFrom(temp);
      Register method_reg;
      if (current_method.IsRegister()) {
        method_reg = XRegisterFrom(current_method);
      } else {
        DCHECK(invoke->GetLocations()->Intrinsified());
        DCHECK(!current_method.IsValid());
        method_reg = reg;
        __ Ldr(reg.X(), MemOperand(sp, kCurrentMethodStackOffset));
      }

      // temp = current_method->dex_cache_resolved_methods_;
      __ Ldr(reg.X(),
             MemOperand(method_reg.X(),
                        ArtMethod::DexCacheResolvedMethodsOffset(kArm64WordSize).Int32Value()));
      // temp = temp[index_in_cache];
      uint32_t index_in_cache = invoke->GetTargetMethod().dex_method_index;
    __ Ldr(reg.X(), MemOperand(reg.X(), GetCachePointerOffset(index_in_cache)));
      break;
    }
  }

  switch (invoke->GetCodePtrLocation()) {
    case HInvokeStaticOrDirect::CodePtrLocation::kCallSelf:
      __ Bl(&frame_entry_label_);
      break;
    case HInvokeStaticOrDirect::CodePtrLocation::kCallPCRelative: {
      relative_call_patches_.emplace_back(invoke->GetTargetMethod());
      vixl::Label* label = &relative_call_patches_.back().label;
      vixl::SingleEmissionCheckScope guard(GetVIXLAssembler());
      __ Bind(label);
      __ bl(0);  // Branch and link to itself. This will be overriden at link time.
      break;
    }
    case HInvokeStaticOrDirect::CodePtrLocation::kCallDirectWithFixup:
    case HInvokeStaticOrDirect::CodePtrLocation::kCallDirect:
      // LR prepared above for better instruction scheduling.
      DCHECK(direct_code_loaded);
      // lr()
      __ Blr(lr);
      break;
    case HInvokeStaticOrDirect::CodePtrLocation::kCallArtMethod:
      // LR = callee_method->entry_point_from_quick_compiled_code_;
      __ Ldr(lr, MemOperand(
          XRegisterFrom(callee_method),
          ArtMethod::EntryPointFromQuickCompiledCodeOffset(kArm64WordSize).Int32Value()));
      // lr()
      __ Blr(lr);
      break;
  }

  DCHECK(!IsLeafMethod());
}

void CodeGeneratorARM64::GenerateVirtualCall(HInvokeVirtual* invoke, Location temp_in) {
  LocationSummary* locations = invoke->GetLocations();
  Location receiver = locations->InAt(0);
  Register temp = XRegisterFrom(temp_in);
  size_t method_offset = mirror::Class::EmbeddedVTableEntryOffset(
      invoke->GetVTableIndex(), kArm64PointerSize).SizeValue();
  Offset class_offset = mirror::Object::ClassOffset();
  Offset entry_point = ArtMethod::EntryPointFromQuickCompiledCodeOffset(kArm64WordSize);

  BlockPoolsScope block_pools(GetVIXLAssembler());

  DCHECK(receiver.IsRegister());
  __ Ldr(temp.W(), HeapOperandFrom(receiver, class_offset));
  MaybeRecordImplicitNullCheck(invoke);
  GetAssembler()->MaybeUnpoisonHeapReference(temp.W());
  // temp = temp->GetMethodAt(method_offset);
  __ Ldr(temp, MemOperand(temp, method_offset));
  // lr = temp->GetEntryPoint();
  __ Ldr(lr, MemOperand(temp, entry_point.SizeValue()));
  // lr();
  __ Blr(lr);
}

void CodeGeneratorARM64::EmitLinkerPatches(ArenaVector<LinkerPatch>* linker_patches) {
  DCHECK(linker_patches->empty());
  size_t size =
      method_patches_.size() +
      call_patches_.size() +
      relative_call_patches_.size() +
      pc_relative_dex_cache_patches_.size();
  linker_patches->reserve(size);
  for (const auto& entry : method_patches_) {
    const MethodReference& target_method = entry.first;
    vixl::Literal<uint64_t>* literal = entry.second;
    linker_patches->push_back(LinkerPatch::MethodPatch(literal->offset(),
                                                       target_method.dex_file,
                                                       target_method.dex_method_index));
  }
  for (const auto& entry : call_patches_) {
    const MethodReference& target_method = entry.first;
    vixl::Literal<uint64_t>* literal = entry.second;
    linker_patches->push_back(LinkerPatch::CodePatch(literal->offset(),
                                                     target_method.dex_file,
                                                     target_method.dex_method_index));
  }
  for (const MethodPatchInfo<vixl::Label>& info : relative_call_patches_) {
    linker_patches->push_back(LinkerPatch::RelativeCodePatch(info.label.location(),
                                                             info.target_method.dex_file,
                                                             info.target_method.dex_method_index));
  }
  for (const PcRelativeDexCacheAccessInfo& info : pc_relative_dex_cache_patches_) {
    linker_patches->push_back(LinkerPatch::DexCacheArrayPatch(info.label.location(),
                                                              &info.target_dex_file,
                                                              info.pc_insn_label->location(),
                                                              info.element_offset));
  }
}

vixl::Literal<uint64_t>* CodeGeneratorARM64::DeduplicateUint64Literal(uint64_t value) {
  // Look up the literal for value.
  auto lb = uint64_literals_.lower_bound(value);
  if (lb != uint64_literals_.end() && !uint64_literals_.key_comp()(value, lb->first)) {
    return lb->second;
  }
  // We don't have a literal for this value, insert a new one.
  vixl::Literal<uint64_t>* literal = __ CreateLiteralDestroyedWithPool<uint64_t>(value);
  uint64_literals_.PutBefore(lb, value, literal);
  return literal;
}

vixl::Literal<uint64_t>* CodeGeneratorARM64::DeduplicateMethodLiteral(
    MethodReference target_method,
    MethodToLiteralMap* map) {
  // Look up the literal for target_method.
  auto lb = map->lower_bound(target_method);
  if (lb != map->end() && !map->key_comp()(target_method, lb->first)) {
    return lb->second;
  }
  // We don't have a literal for this method yet, insert a new one.
  vixl::Literal<uint64_t>* literal = __ CreateLiteralDestroyedWithPool<uint64_t>(0u);
  map->PutBefore(lb, target_method, literal);
  return literal;
}

vixl::Literal<uint64_t>* CodeGeneratorARM64::DeduplicateMethodAddressLiteral(
    MethodReference target_method) {
  return DeduplicateMethodLiteral(target_method, &method_patches_);
}

vixl::Literal<uint64_t>* CodeGeneratorARM64::DeduplicateMethodCodeLiteral(
    MethodReference target_method) {
  return DeduplicateMethodLiteral(target_method, &call_patches_);
}


void InstructionCodeGeneratorARM64::VisitInvokeStaticOrDirect(HInvokeStaticOrDirect* invoke) {
  // When we do not run baseline, explicit clinit checks triggered by static
  // invokes must have been pruned by art::PrepareForRegisterAllocation.
  DCHECK(codegen_->IsBaseline() || !invoke->IsStaticWithExplicitClinitCheck());

  if (TryGenerateIntrinsicCode(invoke, codegen_)) {
    return;
  }

  BlockPoolsScope block_pools(GetVIXLAssembler());
  LocationSummary* locations = invoke->GetLocations();
  codegen_->GenerateStaticOrDirectCall(
      invoke, locations->HasTemps() ? locations->GetTemp(0) : Location::NoLocation());
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
}

void InstructionCodeGeneratorARM64::VisitInvokeVirtual(HInvokeVirtual* invoke) {
  if (TryGenerateIntrinsicCode(invoke, codegen_)) {
    return;
  }

  codegen_->GenerateVirtualCall(invoke, invoke->GetLocations()->GetTemp(0));
  DCHECK(!codegen_->IsLeafMethod());
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
}

void LocationsBuilderARM64::VisitLoadClass(HLoadClass* cls) {
  InvokeRuntimeCallingConvention calling_convention;
  CodeGenerator::CreateLoadClassLocationSummary(
      cls,
      LocationFrom(calling_convention.GetRegisterAt(0)),
      LocationFrom(vixl::x0));
}

void InstructionCodeGeneratorARM64::VisitLoadClass(HLoadClass* cls) {
  if (cls->NeedsAccessCheck()) {
    codegen_->MoveConstant(cls->GetLocations()->GetTemp(0), cls->GetTypeIndex());
    codegen_->InvokeRuntime(QUICK_ENTRY_POINT(pInitializeTypeAndVerifyAccess),
                            cls,
                            cls->GetDexPc(),
                            nullptr);
    CheckEntrypointTypes<kQuickInitializeTypeAndVerifyAccess, void*, uint32_t>();
    return;
  }

  Register out = OutputRegister(cls);
  Register current_method = InputRegisterAt(cls, 0);
  if (cls->IsReferrersClass()) {
    DCHECK(!cls->CanCallRuntime());
    DCHECK(!cls->MustGenerateClinitCheck());
    __ Ldr(out, MemOperand(current_method, ArtMethod::DeclaringClassOffset().Int32Value()));
  } else {
    MemberOffset resolved_types_offset = ArtMethod::DexCacheResolvedTypesOffset(kArm64PointerSize);
    __ Ldr(out.X(), MemOperand(current_method, resolved_types_offset.Int32Value()));
    __ Ldr(out, MemOperand(out.X(), CodeGenerator::GetCacheOffset(cls->GetTypeIndex())));
    // TODO: We will need a read barrier here.

    if (!cls->IsInDexCache() || cls->MustGenerateClinitCheck()) {
      DCHECK(cls->CanCallRuntime());
      SlowPathCodeARM64* slow_path = new (GetGraph()->GetArena()) LoadClassSlowPathARM64(
          cls, cls, cls->GetDexPc(), cls->MustGenerateClinitCheck());
      codegen_->AddSlowPath(slow_path);
      if (!cls->IsInDexCache()) {
        __ Cbz(out, slow_path->GetEntryLabel());
      }
      if (cls->MustGenerateClinitCheck()) {
        GenerateClassInitializationCheck(slow_path, out);
      } else {
        __ Bind(slow_path->GetExitLabel());
      }
    }
  }
}

static MemOperand GetExceptionTlsAddress() {
  return MemOperand(tr, Thread::ExceptionOffset<kArm64WordSize>().Int32Value());
}

void LocationsBuilderARM64::VisitLoadException(HLoadException* load) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(load, LocationSummary::kNoCall);
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorARM64::VisitLoadException(HLoadException* instruction) {
  __ Ldr(OutputRegister(instruction), GetExceptionTlsAddress());
}

void LocationsBuilderARM64::VisitClearException(HClearException* clear) {
  new (GetGraph()->GetArena()) LocationSummary(clear, LocationSummary::kNoCall);
}

void InstructionCodeGeneratorARM64::VisitClearException(HClearException* clear ATTRIBUTE_UNUSED) {
  __ Str(wzr, GetExceptionTlsAddress());
}

void LocationsBuilderARM64::VisitLoadLocal(HLoadLocal* load) {
  load->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM64::VisitLoadLocal(HLoadLocal* load ATTRIBUTE_UNUSED) {
  // Nothing to do, this is driven by the code generator.
}

void LocationsBuilderARM64::VisitLoadString(HLoadString* load) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(load, LocationSummary::kCallOnSlowPath);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorARM64::VisitLoadString(HLoadString* load) {
  SlowPathCodeARM64* slow_path = new (GetGraph()->GetArena()) LoadStringSlowPathARM64(load);
  codegen_->AddSlowPath(slow_path);

  Register out = OutputRegister(load);
  Register current_method = InputRegisterAt(load, 0);
  __ Ldr(out, MemOperand(current_method, ArtMethod::DeclaringClassOffset().Int32Value()));
  __ Ldr(out.X(), HeapOperand(out, mirror::Class::DexCacheStringsOffset()));
  __ Ldr(out, MemOperand(out.X(), CodeGenerator::GetCacheOffset(load->GetStringIndex())));
  // TODO: We will need a read barrier here.
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

void InstructionCodeGeneratorARM64::VisitLongConstant(HLongConstant* constant ATTRIBUTE_UNUSED) {
  // Will be generated at use site.
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
  if (instruction->IsEnter()) {
    CheckEntrypointTypes<kQuickLockObject, void, mirror::Object*>();
  } else {
    CheckEntrypointTypes<kQuickUnlockObject, void, mirror::Object*>();
  }
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
  locations->SetOut(LocationFrom(x0));
  locations->SetInAt(0, LocationFrom(calling_convention.GetRegisterAt(1)));
  locations->SetInAt(1, LocationFrom(calling_convention.GetRegisterAt(2)));
}

void InstructionCodeGeneratorARM64::VisitNewArray(HNewArray* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  InvokeRuntimeCallingConvention calling_convention;
  Register type_index = RegisterFrom(locations->GetTemp(0), Primitive::kPrimInt);
  DCHECK(type_index.Is(w0));
  __ Mov(type_index, instruction->GetTypeIndex());
  // Note: if heap poisoning is enabled, the entry point takes cares
  // of poisoning the reference.
  codegen_->InvokeRuntime(instruction->GetEntrypoint(),
                          instruction,
                          instruction->GetDexPc(),
                          nullptr);
  CheckEntrypointTypes<kQuickAllocArrayWithAccessCheck, void*, uint32_t, int32_t, ArtMethod*>();
}

void LocationsBuilderARM64::VisitNewInstance(HNewInstance* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCall);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, LocationFrom(calling_convention.GetRegisterAt(0)));
  locations->SetInAt(1, LocationFrom(calling_convention.GetRegisterAt(1)));
  locations->SetOut(calling_convention.GetReturnLocation(Primitive::kPrimNot));
}

void InstructionCodeGeneratorARM64::VisitNewInstance(HNewInstance* instruction) {
  // Note: if heap poisoning is enabled, the entry point takes cares
  // of poisoning the reference.
  codegen_->InvokeRuntime(instruction->GetEntrypoint(),
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
  LocationSummary::CallKind call_kind = instruction->CanThrowIntoCatchBlock()
      ? LocationSummary::kCallOnSlowPath
      : LocationSummary::kNoCall;
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction, call_kind);
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
  if (codegen_->IsImplicitNullCheckAllowed(instruction)) {
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

void InstructionCodeGeneratorARM64::VisitParameterValue(
    HParameterValue* instruction ATTRIBUTE_UNUSED) {
  // Nothing to do, the parameter is already at its location.
}

void LocationsBuilderARM64::VisitCurrentMethod(HCurrentMethod* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetOut(LocationFrom(kArtMethodRegister));
}

void InstructionCodeGeneratorARM64::VisitCurrentMethod(
    HCurrentMethod* instruction ATTRIBUTE_UNUSED) {
  // Nothing to do, the method is already at its location.
}

void LocationsBuilderARM64::VisitPhi(HPhi* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  for (size_t i = 0, e = instruction->InputCount(); i < e; ++i) {
    locations->SetInAt(i, Location::Any());
  }
  locations->SetOut(Location::Any());
}

void InstructionCodeGeneratorARM64::VisitPhi(HPhi* instruction ATTRIBUTE_UNUSED) {
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
      locations->SetInAt(1, Location::RegisterOrConstant(rem->InputAt(1)));
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
      GenerateDivRemIntegral(rem);
      break;
    }

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      int32_t entry_offset = (type == Primitive::kPrimFloat) ? QUICK_ENTRY_POINT(pFmodf)
                                                             : QUICK_ENTRY_POINT(pFmod);
      codegen_->InvokeRuntime(entry_offset, rem, rem->GetDexPc(), nullptr);
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

void InstructionCodeGeneratorARM64::VisitReturn(HReturn* instruction ATTRIBUTE_UNUSED) {
  codegen_->GenerateFrameExit();
}

void LocationsBuilderARM64::VisitReturnVoid(HReturnVoid* instruction) {
  instruction->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM64::VisitReturnVoid(HReturnVoid* instruction ATTRIBUTE_UNUSED) {
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

void InstructionCodeGeneratorARM64::VisitStoreLocal(HStoreLocal* store ATTRIBUTE_UNUSED) {
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
  HandleFieldSet(instruction, instruction->GetFieldInfo(), instruction->GetValueCanBeNull());
}

void LocationsBuilderARM64::VisitUnresolvedInstanceFieldGet(
    HUnresolvedInstanceFieldGet* instruction) {
  FieldAccessCallingConventionARM64 calling_convention;
  codegen_->CreateUnresolvedFieldLocationSummary(
      instruction, instruction->GetFieldType(), calling_convention);
}

void InstructionCodeGeneratorARM64::VisitUnresolvedInstanceFieldGet(
    HUnresolvedInstanceFieldGet* instruction) {
  FieldAccessCallingConventionARM64 calling_convention;
  codegen_->GenerateUnresolvedFieldAccess(instruction,
                                          instruction->GetFieldType(),
                                          instruction->GetFieldIndex(),
                                          instruction->GetDexPc(),
                                          calling_convention);
}

void LocationsBuilderARM64::VisitUnresolvedInstanceFieldSet(
    HUnresolvedInstanceFieldSet* instruction) {
  FieldAccessCallingConventionARM64 calling_convention;
  codegen_->CreateUnresolvedFieldLocationSummary(
      instruction, instruction->GetFieldType(), calling_convention);
}

void InstructionCodeGeneratorARM64::VisitUnresolvedInstanceFieldSet(
    HUnresolvedInstanceFieldSet* instruction) {
  FieldAccessCallingConventionARM64 calling_convention;
  codegen_->GenerateUnresolvedFieldAccess(instruction,
                                          instruction->GetFieldType(),
                                          instruction->GetFieldIndex(),
                                          instruction->GetDexPc(),
                                          calling_convention);
}

void LocationsBuilderARM64::VisitUnresolvedStaticFieldGet(
    HUnresolvedStaticFieldGet* instruction) {
  FieldAccessCallingConventionARM64 calling_convention;
  codegen_->CreateUnresolvedFieldLocationSummary(
      instruction, instruction->GetFieldType(), calling_convention);
}

void InstructionCodeGeneratorARM64::VisitUnresolvedStaticFieldGet(
    HUnresolvedStaticFieldGet* instruction) {
  FieldAccessCallingConventionARM64 calling_convention;
  codegen_->GenerateUnresolvedFieldAccess(instruction,
                                          instruction->GetFieldType(),
                                          instruction->GetFieldIndex(),
                                          instruction->GetDexPc(),
                                          calling_convention);
}

void LocationsBuilderARM64::VisitUnresolvedStaticFieldSet(
    HUnresolvedStaticFieldSet* instruction) {
  FieldAccessCallingConventionARM64 calling_convention;
  codegen_->CreateUnresolvedFieldLocationSummary(
      instruction, instruction->GetFieldType(), calling_convention);
}

void InstructionCodeGeneratorARM64::VisitUnresolvedStaticFieldSet(
    HUnresolvedStaticFieldSet* instruction) {
  FieldAccessCallingConventionARM64 calling_convention;
  codegen_->GenerateUnresolvedFieldAccess(instruction,
                                          instruction->GetFieldType(),
                                          instruction->GetFieldIndex(),
                                          instruction->GetDexPc(),
                                          calling_convention);
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

void InstructionCodeGeneratorARM64::VisitTemporary(HTemporary* temp ATTRIBUTE_UNUSED) {
  // Nothing to do, this is driven by the code generator.
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
    } else if (result_type == Primitive::kPrimInt && input_type == Primitive::kPrimLong) {
      // 'int' values are used directly as W registers, discarding the top
      // bits, so we don't need to sign-extend and can just perform a move.
      // We do not pass the `kDiscardForSameWReg` argument to force clearing the
      // top 32 bits of the target register. We theoretically could leave those
      // bits unchanged, but we would have to make sure that no code uses a
      // 32bit input value as a 64bit value assuming that the top 32 bits are
      // zero.
      __ Mov(output.W(), source.W());
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

void LocationsBuilderARM64::VisitBoundType(HBoundType* instruction ATTRIBUTE_UNUSED) {
  // Nothing to do, this should be removed during prepare for register allocator.
  LOG(FATAL) << "Unreachable";
}

void InstructionCodeGeneratorARM64::VisitBoundType(HBoundType* instruction ATTRIBUTE_UNUSED) {
  // Nothing to do, this should be removed during prepare for register allocator.
  LOG(FATAL) << "Unreachable";
}

void LocationsBuilderARM64::VisitFakeString(HFakeString* instruction) {
  DCHECK(codegen_->IsBaseline());
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetOut(Location::ConstantLocation(GetGraph()->GetNullConstant()));
}

void InstructionCodeGeneratorARM64::VisitFakeString(HFakeString* instruction ATTRIBUTE_UNUSED) {
  DCHECK(codegen_->IsBaseline());
  // Will be generated at use site.
}

// Simple implementation of packed switch - generate cascaded compare/jumps.
void LocationsBuilderARM64::VisitPackedSwitch(HPackedSwitch* switch_instr) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(switch_instr, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
}

void InstructionCodeGeneratorARM64::VisitPackedSwitch(HPackedSwitch* switch_instr) {
  int32_t lower_bound = switch_instr->GetStartValue();
  uint32_t num_entries = switch_instr->GetNumEntries();
  Register value_reg = InputRegisterAt(switch_instr, 0);
  HBasicBlock* default_block = switch_instr->GetDefaultBlock();

  // Roughly set 16 as max average assemblies generated per HIR in a graph.
  static constexpr int32_t kMaxExpectedSizePerHInstruction = 16 * vixl::kInstructionSize;
  // ADR has a limited range(+/-1MB), so we set a threshold for the number of HIRs in the graph to
  // make sure we don't emit it if the target may run out of range.
  // TODO: Instead of emitting all jump tables at the end of the code, we could keep track of ADR
  // ranges and emit the tables only as required.
  static constexpr int32_t kJumpTableInstructionThreshold = 1* MB / kMaxExpectedSizePerHInstruction;

  if (num_entries < kPackedSwitchJumpTableThreshold ||
      // Current instruction id is an upper bound of the number of HIRs in the graph.
      GetGraph()->GetCurrentInstructionId() > kJumpTableInstructionThreshold) {
    // Create a series of compare/jumps.
    const ArenaVector<HBasicBlock*>& successors = switch_instr->GetBlock()->GetSuccessors();
    for (uint32_t i = 0; i < num_entries; i++) {
      int32_t case_value = lower_bound + i;
      vixl::Label* succ = codegen_->GetLabelOf(successors[i]);
      if (case_value == 0) {
        __ Cbz(value_reg, succ);
      } else {
        __ Cmp(value_reg, Operand(case_value));
        __ B(eq, succ);
      }
    }

    // And the default for any other value.
    if (!codegen_->GoesToNextBlock(switch_instr->GetBlock(), default_block)) {
      __ B(codegen_->GetLabelOf(default_block));
    }
  } else {
    JumpTableARM64* jump_table = new (GetGraph()->GetArena()) JumpTableARM64(switch_instr);
    codegen_->AddJumpTable(jump_table);

    UseScratchRegisterScope temps(codegen_->GetVIXLAssembler());

    // Below instructions should use at most one blocked register. Since there are two blocked
    // registers, we are free to block one.
    Register temp_w = temps.AcquireW();
    Register index;
    // Remove the bias.
    if (lower_bound != 0) {
      index = temp_w;
      __ Sub(index, value_reg, Operand(lower_bound));
    } else {
      index = value_reg;
    }

    // Jump to default block if index is out of the range.
    __ Cmp(index, Operand(num_entries));
    __ B(hs, codegen_->GetLabelOf(default_block));

    // In current VIXL implementation, it won't require any blocked registers to encode the
    // immediate value for Adr. So we are free to use both VIXL blocked registers to reduce the
    // register pressure.
    Register table_base = temps.AcquireX();
    // Load jump offset from the table.
    __ Adr(table_base, jump_table->GetTableStartLabel());
    Register jump_offset = temp_w;
    __ Ldr(jump_offset, MemOperand(table_base, index, UXTW, 2));

    // Jump to target block by branching to table_base(pc related) + offset.
    Register target_address = table_base;
    __ Add(target_address, table_base, Operand(jump_offset, SXTW));
    __ Br(target_address);
  }
}

#undef __
#undef QUICK_ENTRY_POINT

}  // namespace arm64
}  // namespace art
