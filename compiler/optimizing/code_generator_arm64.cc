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

template<class MirrorType>
class GcRoot;

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
// The compare/jump sequence will generate about (1.5 * num_entries + 3) instructions. While jump
// table version generates 7 instructions and num_entries literals. Compare/jump sequence will
// generates less code/data with a small num_entries.
static constexpr uint32_t kPackedSwitchCompareJumpThreshold = 7;

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

inline Condition ARM64FPCondition(IfCondition cond, bool gt_bias) {
  // The ARM64 condition codes can express all the necessary branches, see the
  // "Meaning (floating-point)" column in the table C1-1 in the ARMv8 reference manual.
  // There is no dex instruction or HIR that would need the missing conditions
  // "equal or unordered" or "not equal".
  switch (cond) {
    case kCondEQ: return eq;
    case kCondNE: return ne /* unordered */;
    case kCondLT: return gt_bias ? cc : lt /* unordered */;
    case kCondLE: return gt_bias ? ls : le /* unordered */;
    case kCondGT: return gt_bias ? hi /* unordered */ : gt;
    case kCondGE: return gt_bias ? cs /* unordered */ : ge;
    default:
      LOG(FATAL) << "UNREACHABLE";
      UNREACHABLE();
  }
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
  explicit BoundsCheckSlowPathARM64(HBoundsCheck* instruction) : SlowPathCodeARM64(instruction) {}

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
  DISALLOW_COPY_AND_ASSIGN(BoundsCheckSlowPathARM64);
};

class DivZeroCheckSlowPathARM64 : public SlowPathCodeARM64 {
 public:
  explicit DivZeroCheckSlowPathARM64(HDivZeroCheck* instruction) : SlowPathCodeARM64(instruction) {}

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
  DISALLOW_COPY_AND_ASSIGN(DivZeroCheckSlowPathARM64);
};

class LoadClassSlowPathARM64 : public SlowPathCodeARM64 {
 public:
  LoadClassSlowPathARM64(HLoadClass* cls,
                         HInstruction* at,
                         uint32_t dex_pc,
                         bool do_clinit)
      : SlowPathCodeARM64(at), cls_(cls), at_(at), dex_pc_(dex_pc), do_clinit_(do_clinit) {
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
  explicit LoadStringSlowPathARM64(HLoadString* instruction) : SlowPathCodeARM64(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    LocationSummary* locations = instruction_->GetLocations();
    DCHECK(!locations->GetLiveRegisters()->ContainsCoreRegister(locations->Out().reg()));
    CodeGeneratorARM64* arm64_codegen = down_cast<CodeGeneratorARM64*>(codegen);

    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, locations);

    InvokeRuntimeCallingConvention calling_convention;
    const uint32_t string_index = instruction_->AsLoadString()->GetStringIndex();
    __ Mov(calling_convention.GetRegisterAt(0).W(), string_index);
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
  DISALLOW_COPY_AND_ASSIGN(LoadStringSlowPathARM64);
};

class NullCheckSlowPathARM64 : public SlowPathCodeARM64 {
 public:
  explicit NullCheckSlowPathARM64(HNullCheck* instr) : SlowPathCodeARM64(instr) {}

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
  DISALLOW_COPY_AND_ASSIGN(NullCheckSlowPathARM64);
};

class SuspendCheckSlowPathARM64 : public SlowPathCodeARM64 {
 public:
  SuspendCheckSlowPathARM64(HSuspendCheck* instruction, HBasicBlock* successor)
      : SlowPathCodeARM64(instruction), successor_(successor) {}

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
  // If not null, the block to branch to after the suspend check.
  HBasicBlock* const successor_;

  // If `successor_` is null, the label to branch to after the suspend check.
  vixl::Label return_label_;

  DISALLOW_COPY_AND_ASSIGN(SuspendCheckSlowPathARM64);
};

class TypeCheckSlowPathARM64 : public SlowPathCodeARM64 {
 public:
  TypeCheckSlowPathARM64(HInstruction* instruction, bool is_fatal)
      : SlowPathCodeARM64(instruction), is_fatal_(is_fatal) {}

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
  const bool is_fatal_;

  DISALLOW_COPY_AND_ASSIGN(TypeCheckSlowPathARM64);
};

class DeoptimizationSlowPathARM64 : public SlowPathCodeARM64 {
 public:
  explicit DeoptimizationSlowPathARM64(HDeoptimize* instruction)
      : SlowPathCodeARM64(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    CodeGeneratorARM64* arm64_codegen = down_cast<CodeGeneratorARM64*>(codegen);
    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, instruction_->GetLocations());
    arm64_codegen->InvokeRuntime(QUICK_ENTRY_POINT(pDeoptimize),
                                 instruction_,
                                 instruction_->GetDexPc(),
                                 this);
    CheckEntrypointTypes<kQuickDeoptimize, void, void>();
  }

  const char* GetDescription() const OVERRIDE { return "DeoptimizationSlowPathARM64"; }

 private:
  DISALLOW_COPY_AND_ASSIGN(DeoptimizationSlowPathARM64);
};

class ArraySetSlowPathARM64 : public SlowPathCodeARM64 {
 public:
  explicit ArraySetSlowPathARM64(HInstruction* instruction) : SlowPathCodeARM64(instruction) {}

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
  DISALLOW_COPY_AND_ASSIGN(ArraySetSlowPathARM64);
};

void JumpTableARM64::EmitTable(CodeGeneratorARM64* codegen) {
  uint32_t num_entries = switch_instr_->GetNumEntries();
  DCHECK_GE(num_entries, kPackedSwitchCompareJumpThreshold);

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

// Slow path marking an object during a read barrier.
class ReadBarrierMarkSlowPathARM64 : public SlowPathCodeARM64 {
 public:
  ReadBarrierMarkSlowPathARM64(HInstruction* instruction, Location out, Location obj)
      : SlowPathCodeARM64(instruction), out_(out), obj_(obj) {
    DCHECK(kEmitCompilerReadBarrier);
  }

  const char* GetDescription() const OVERRIDE { return "ReadBarrierMarkSlowPathARM64"; }

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    LocationSummary* locations = instruction_->GetLocations();
    Primitive::Type type = Primitive::kPrimNot;
    DCHECK(locations->CanCall());
    DCHECK(!locations->GetLiveRegisters()->ContainsCoreRegister(out_.reg()));
    DCHECK(instruction_->IsInstanceFieldGet() ||
           instruction_->IsStaticFieldGet() ||
           instruction_->IsArrayGet() ||
           instruction_->IsLoadClass() ||
           instruction_->IsLoadString() ||
           instruction_->IsInstanceOf() ||
           instruction_->IsCheckCast())
        << "Unexpected instruction in read barrier marking slow path: "
        << instruction_->DebugName();

    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, locations);

    InvokeRuntimeCallingConvention calling_convention;
    CodeGeneratorARM64* arm64_codegen = down_cast<CodeGeneratorARM64*>(codegen);
    arm64_codegen->MoveLocation(LocationFrom(calling_convention.GetRegisterAt(0)), obj_, type);
    arm64_codegen->InvokeRuntime(QUICK_ENTRY_POINT(pReadBarrierMark),
                                 instruction_,
                                 instruction_->GetDexPc(),
                                 this);
    CheckEntrypointTypes<kQuickReadBarrierMark, mirror::Object*, mirror::Object*>();
    arm64_codegen->MoveLocation(out_, calling_convention.GetReturnLocation(type), type);

    RestoreLiveRegisters(codegen, locations);
    __ B(GetExitLabel());
  }

 private:
  const Location out_;
  const Location obj_;

  DISALLOW_COPY_AND_ASSIGN(ReadBarrierMarkSlowPathARM64);
};

// Slow path generating a read barrier for a heap reference.
class ReadBarrierForHeapReferenceSlowPathARM64 : public SlowPathCodeARM64 {
 public:
  ReadBarrierForHeapReferenceSlowPathARM64(HInstruction* instruction,
                                           Location out,
                                           Location ref,
                                           Location obj,
                                           uint32_t offset,
                                           Location index)
      : SlowPathCodeARM64(instruction),
        out_(out),
        ref_(ref),
        obj_(obj),
        offset_(offset),
        index_(index) {
    DCHECK(kEmitCompilerReadBarrier);
    // If `obj` is equal to `out` or `ref`, it means the initial object
    // has been overwritten by (or after) the heap object reference load
    // to be instrumented, e.g.:
    //
    //   __ Ldr(out, HeapOperand(out, class_offset);
    //   codegen_->GenerateReadBarrierSlow(instruction, out_loc, out_loc, out_loc, offset);
    //
    // In that case, we have lost the information about the original
    // object, and the emitted read barrier cannot work properly.
    DCHECK(!obj.Equals(out)) << "obj=" << obj << " out=" << out;
    DCHECK(!obj.Equals(ref)) << "obj=" << obj << " ref=" << ref;
  }

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    CodeGeneratorARM64* arm64_codegen = down_cast<CodeGeneratorARM64*>(codegen);
    LocationSummary* locations = instruction_->GetLocations();
    Primitive::Type type = Primitive::kPrimNot;
    DCHECK(locations->CanCall());
    DCHECK(!locations->GetLiveRegisters()->ContainsCoreRegister(out_.reg()));
    DCHECK(!instruction_->IsInvoke() ||
           (instruction_->IsInvokeStaticOrDirect() &&
            instruction_->GetLocations()->Intrinsified()))
        << "Unexpected instruction in read barrier for heap reference slow path: "
        << instruction_->DebugName();
    // The read barrier instrumentation does not support the
    // HArm64IntermediateAddress instruction yet.
    DCHECK(!(instruction_->IsArrayGet() &&
             instruction_->AsArrayGet()->GetArray()->IsArm64IntermediateAddress()));

    __ Bind(GetEntryLabel());

    SaveLiveRegisters(codegen, locations);

    // We may have to change the index's value, but as `index_` is a
    // constant member (like other "inputs" of this slow path),
    // introduce a copy of it, `index`.
    Location index = index_;
    if (index_.IsValid()) {
      // Handle `index_` for HArrayGet and intrinsic UnsafeGetObject.
      if (instruction_->IsArrayGet()) {
        // Compute the actual memory offset and store it in `index`.
        Register index_reg = RegisterFrom(index_, Primitive::kPrimInt);
        DCHECK(locations->GetLiveRegisters()->ContainsCoreRegister(index_.reg()));
        if (codegen->IsCoreCalleeSaveRegister(index_.reg())) {
          // We are about to change the value of `index_reg` (see the
          // calls to vixl::MacroAssembler::Lsl and
          // vixl::MacroAssembler::Mov below), but it has
          // not been saved by the previous call to
          // art::SlowPathCode::SaveLiveRegisters, as it is a
          // callee-save register --
          // art::SlowPathCode::SaveLiveRegisters does not consider
          // callee-save registers, as it has been designed with the
          // assumption that callee-save registers are supposed to be
          // handled by the called function.  So, as a callee-save
          // register, `index_reg` _would_ eventually be saved onto
          // the stack, but it would be too late: we would have
          // changed its value earlier.  Therefore, we manually save
          // it here into another freely available register,
          // `free_reg`, chosen of course among the caller-save
          // registers (as a callee-save `free_reg` register would
          // exhibit the same problem).
          //
          // Note we could have requested a temporary register from
          // the register allocator instead; but we prefer not to, as
          // this is a slow path, and we know we can find a
          // caller-save register that is available.
          Register free_reg = FindAvailableCallerSaveRegister(codegen);
          __ Mov(free_reg.W(), index_reg);
          index_reg = free_reg;
          index = LocationFrom(index_reg);
        } else {
          // The initial register stored in `index_` has already been
          // saved in the call to art::SlowPathCode::SaveLiveRegisters
          // (as it is not a callee-save register), so we can freely
          // use it.
        }
        // Shifting the index value contained in `index_reg` by the scale
        // factor (2) cannot overflow in practice, as the runtime is
        // unable to allocate object arrays with a size larger than
        // 2^26 - 1 (that is, 2^28 - 4 bytes).
        __ Lsl(index_reg, index_reg, Primitive::ComponentSizeShift(type));
        static_assert(
            sizeof(mirror::HeapReference<mirror::Object>) == sizeof(int32_t),
            "art::mirror::HeapReference<art::mirror::Object> and int32_t have different sizes.");
        __ Add(index_reg, index_reg, Operand(offset_));
      } else {
        DCHECK(instruction_->IsInvoke());
        DCHECK(instruction_->GetLocations()->Intrinsified());
        DCHECK((instruction_->AsInvoke()->GetIntrinsic() == Intrinsics::kUnsafeGetObject) ||
               (instruction_->AsInvoke()->GetIntrinsic() == Intrinsics::kUnsafeGetObjectVolatile))
            << instruction_->AsInvoke()->GetIntrinsic();
        DCHECK_EQ(offset_, 0U);
        DCHECK(index_.IsRegisterPair());
        // UnsafeGet's offset location is a register pair, the low
        // part contains the correct offset.
        index = index_.ToLow();
      }
    }

    // We're moving two or three locations to locations that could
    // overlap, so we need a parallel move resolver.
    InvokeRuntimeCallingConvention calling_convention;
    HParallelMove parallel_move(codegen->GetGraph()->GetArena());
    parallel_move.AddMove(ref_,
                          LocationFrom(calling_convention.GetRegisterAt(0)),
                          type,
                          nullptr);
    parallel_move.AddMove(obj_,
                          LocationFrom(calling_convention.GetRegisterAt(1)),
                          type,
                          nullptr);
    if (index.IsValid()) {
      parallel_move.AddMove(index,
                            LocationFrom(calling_convention.GetRegisterAt(2)),
                            Primitive::kPrimInt,
                            nullptr);
      codegen->GetMoveResolver()->EmitNativeCode(&parallel_move);
    } else {
      codegen->GetMoveResolver()->EmitNativeCode(&parallel_move);
      arm64_codegen->MoveConstant(LocationFrom(calling_convention.GetRegisterAt(2)), offset_);
    }
    arm64_codegen->InvokeRuntime(QUICK_ENTRY_POINT(pReadBarrierSlow),
                                 instruction_,
                                 instruction_->GetDexPc(),
                                 this);
    CheckEntrypointTypes<
        kQuickReadBarrierSlow, mirror::Object*, mirror::Object*, mirror::Object*, uint32_t>();
    arm64_codegen->MoveLocation(out_, calling_convention.GetReturnLocation(type), type);

    RestoreLiveRegisters(codegen, locations);

    __ B(GetExitLabel());
  }

  const char* GetDescription() const OVERRIDE { return "ReadBarrierForHeapReferenceSlowPathARM64"; }

 private:
  Register FindAvailableCallerSaveRegister(CodeGenerator* codegen) {
    size_t ref = static_cast<int>(XRegisterFrom(ref_).code());
    size_t obj = static_cast<int>(XRegisterFrom(obj_).code());
    for (size_t i = 0, e = codegen->GetNumberOfCoreRegisters(); i < e; ++i) {
      if (i != ref && i != obj && !codegen->IsCoreCalleeSaveRegister(i)) {
        return Register(VIXLRegCodeFromART(i), kXRegSize);
      }
    }
    // We shall never fail to find a free caller-save register, as
    // there are more than two core caller-save registers on ARM64
    // (meaning it is possible to find one which is different from
    // `ref` and `obj`).
    DCHECK_GT(codegen->GetNumberOfCoreCallerSaveRegisters(), 2u);
    LOG(FATAL) << "Could not find a free register";
    UNREACHABLE();
  }

  const Location out_;
  const Location ref_;
  const Location obj_;
  const uint32_t offset_;
  // An additional location containing an index to an array.
  // Only used for HArrayGet and the UnsafeGetObject &
  // UnsafeGetObjectVolatile intrinsics.
  const Location index_;

  DISALLOW_COPY_AND_ASSIGN(ReadBarrierForHeapReferenceSlowPathARM64);
};

// Slow path generating a read barrier for a GC root.
class ReadBarrierForRootSlowPathARM64 : public SlowPathCodeARM64 {
 public:
  ReadBarrierForRootSlowPathARM64(HInstruction* instruction, Location out, Location root)
      : SlowPathCodeARM64(instruction), out_(out), root_(root) {
    DCHECK(kEmitCompilerReadBarrier);
  }

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    LocationSummary* locations = instruction_->GetLocations();
    Primitive::Type type = Primitive::kPrimNot;
    DCHECK(locations->CanCall());
    DCHECK(!locations->GetLiveRegisters()->ContainsCoreRegister(out_.reg()));
    DCHECK(instruction_->IsLoadClass() || instruction_->IsLoadString())
        << "Unexpected instruction in read barrier for GC root slow path: "
        << instruction_->DebugName();

    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, locations);

    InvokeRuntimeCallingConvention calling_convention;
    CodeGeneratorARM64* arm64_codegen = down_cast<CodeGeneratorARM64*>(codegen);
    // The argument of the ReadBarrierForRootSlow is not a managed
    // reference (`mirror::Object*`), but a `GcRoot<mirror::Object>*`;
    // thus we need a 64-bit move here, and we cannot use
    //
    //   arm64_codegen->MoveLocation(
    //       LocationFrom(calling_convention.GetRegisterAt(0)),
    //       root_,
    //       type);
    //
    // which would emit a 32-bit move, as `type` is a (32-bit wide)
    // reference type (`Primitive::kPrimNot`).
    __ Mov(calling_convention.GetRegisterAt(0), XRegisterFrom(out_));
    arm64_codegen->InvokeRuntime(QUICK_ENTRY_POINT(pReadBarrierForRootSlow),
                                 instruction_,
                                 instruction_->GetDexPc(),
                                 this);
    CheckEntrypointTypes<kQuickReadBarrierForRootSlow, mirror::Object*, GcRoot<mirror::Object>*>();
    arm64_codegen->MoveLocation(out_, calling_convention.GetReturnLocation(type), type);

    RestoreLiveRegisters(codegen, locations);
    __ B(GetExitLabel());
  }

  const char* GetDescription() const OVERRIDE { return "ReadBarrierForRootSlowPathARM64"; }

 private:
  const Location out_;
  const Location root_;

  DISALLOW_COPY_AND_ASSIGN(ReadBarrierForRootSlowPathARM64);
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
      block_labels_(graph->GetArena()->Adapter(kArenaAllocCodeGenerator)),
      jump_tables_(graph->GetArena()->Adapter(kArenaAllocCodeGenerator)),
      location_builder_(graph, this),
      instruction_visitor_(graph, this),
      move_resolver_(graph->GetArena(), this),
      assembler_(graph->GetArena()),
      isa_features_(isa_features),
      uint32_literals_(std::less<uint32_t>(),
                       graph->GetArena()->Adapter(kArenaAllocCodeGenerator)),
      uint64_literals_(std::less<uint64_t>(),
                       graph->GetArena()->Adapter(kArenaAllocCodeGenerator)),
      method_patches_(MethodReferenceComparator(),
                      graph->GetArena()->Adapter(kArenaAllocCodeGenerator)),
      call_patches_(MethodReferenceComparator(),
                    graph->GetArena()->Adapter(kArenaAllocCodeGenerator)),
      relative_call_patches_(graph->GetArena()->Adapter(kArenaAllocCodeGenerator)),
      pc_relative_dex_cache_patches_(graph->GetArena()->Adapter(kArenaAllocCodeGenerator)),
      boot_image_string_patches_(StringReferenceValueComparator(),
                                 graph->GetArena()->Adapter(kArenaAllocCodeGenerator)),
      pc_relative_string_patches_(graph->GetArena()->Adapter(kArenaAllocCodeGenerator)),
      boot_image_address_patches_(std::less<uint32_t>(),
                                  graph->GetArena()->Adapter(kArenaAllocCodeGenerator)) {
  // Save the link register (containing the return address) to mimic Quick.
  AddAllocatedRegister(LocationFrom(lr));
}

#define __ GetVIXLAssembler()->

void CodeGeneratorARM64::EmitJumpTables() {
  for (auto&& jump_table : jump_tables_) {
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

void CodeGeneratorARM64::SetupBlockedRegisters() const {
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

  if (GetGraph()->IsDebuggable()) {
    // Stubs do not save callee-save floating point registers. If the graph
    // is debuggable, we need to deal with these registers differently. For
    // now, just block them.
    CPURegList reserved_fp_registers_debuggable = callee_saved_fp_registers;
    while (!reserved_fp_registers_debuggable.IsEmpty()) {
      blocked_fpu_registers_[reserved_fp_registers_debuggable.PopLowestIndex().code()] = true;
    }
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
                                     const MemOperand& src,
                                     bool needs_null_check) {
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
      if (needs_null_check) {
        MaybeRecordImplicitNullCheck(instruction);
      }
      break;
    case Primitive::kPrimByte:
      __ Ldarb(Register(dst), base);
      if (needs_null_check) {
        MaybeRecordImplicitNullCheck(instruction);
      }
      __ Sbfx(Register(dst), Register(dst), 0, Primitive::ComponentSize(type) * kBitsPerByte);
      break;
    case Primitive::kPrimChar:
      __ Ldarh(Register(dst), base);
      if (needs_null_check) {
        MaybeRecordImplicitNullCheck(instruction);
      }
      break;
    case Primitive::kPrimShort:
      __ Ldarh(Register(dst), base);
      if (needs_null_check) {
        MaybeRecordImplicitNullCheck(instruction);
      }
      __ Sbfx(Register(dst), Register(dst), 0, Primitive::ComponentSize(type) * kBitsPerByte);
      break;
    case Primitive::kPrimInt:
    case Primitive::kPrimNot:
    case Primitive::kPrimLong:
      DCHECK_EQ(dst.Is64Bits(), Primitive::Is64BitType(type));
      __ Ldar(Register(dst), base);
      if (needs_null_check) {
        MaybeRecordImplicitNullCheck(instruction);
      }
      break;
    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      DCHECK(dst.IsFPRegister());
      DCHECK_EQ(dst.Is64Bits(), Primitive::Is64BitType(type));

      Register temp = dst.Is64Bits() ? temps.AcquireX() : temps.AcquireW();
      __ Ldar(temp, base);
      if (needs_null_check) {
        MaybeRecordImplicitNullCheck(instruction);
      }
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

  // Even if the initialized flag is set, we need to ensure consistent memory ordering.
  // TODO(vixl): Let the MacroAssembler handle MemOperand.
  __ Add(temp, class_reg, status_offset);
  __ Ldar(temp, HeapOperand(temp));
  __ Cmp(temp, mirror::Class::kStatusInitialized);
  __ B(lt, slow_path->GetEntryLabel());
  __ Bind(slow_path->GetExitLabel());
}

void CodeGeneratorARM64::GenerateMemoryBarrier(MemBarrierKind kind) {
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
      : InstructionCodeGenerator(graph, codegen),
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
  DCHECK(instruction->IsInstanceFieldGet() || instruction->IsStaticFieldGet());

  bool object_field_get_with_read_barrier =
      kEmitCompilerReadBarrier && (instruction->GetType() == Primitive::kPrimNot);
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction,
                                                   object_field_get_with_read_barrier ?
                                                       LocationSummary::kCallOnSlowPath :
                                                       LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  if (Primitive::IsFloatingPointType(instruction->GetType())) {
    locations->SetOut(Location::RequiresFpuRegister());
  } else {
    // The output overlaps for an object field get when read barriers
    // are enabled: we do not want the load to overwrite the object's
    // location, as we need it to emit the read barrier.
    locations->SetOut(
        Location::RequiresRegister(),
        object_field_get_with_read_barrier ? Location::kOutputOverlap : Location::kNoOutputOverlap);
  }
}

void InstructionCodeGeneratorARM64::HandleFieldGet(HInstruction* instruction,
                                                   const FieldInfo& field_info) {
  DCHECK(instruction->IsInstanceFieldGet() || instruction->IsStaticFieldGet());
  LocationSummary* locations = instruction->GetLocations();
  Location base_loc = locations->InAt(0);
  Location out = locations->Out();
  uint32_t offset = field_info.GetFieldOffset().Uint32Value();
  Primitive::Type field_type = field_info.GetFieldType();
  BlockPoolsScope block_pools(GetVIXLAssembler());
  MemOperand field = HeapOperand(InputRegisterAt(instruction, 0), field_info.GetFieldOffset());

  if (field_type == Primitive::kPrimNot && kEmitCompilerReadBarrier && kUseBakerReadBarrier) {
    // Object FieldGet with Baker's read barrier case.
    MacroAssembler* masm = GetVIXLAssembler();
    UseScratchRegisterScope temps(masm);
    // /* HeapReference<Object> */ out = *(base + offset)
    Register base = RegisterFrom(base_loc, Primitive::kPrimNot);
    Register temp = temps.AcquireW();
    // Note that potential implicit null checks are handled in this
    // CodeGeneratorARM64::GenerateFieldLoadWithBakerReadBarrier call.
    codegen_->GenerateFieldLoadWithBakerReadBarrier(
        instruction,
        out,
        base,
        offset,
        temp,
        /* needs_null_check */ true,
        field_info.IsVolatile());
  } else {
    // General case.
    if (field_info.IsVolatile()) {
      // Note that a potential implicit null check is handled in this
      // CodeGeneratorARM64::LoadAcquire call.
      // NB: LoadAcquire will record the pc info if needed.
      codegen_->LoadAcquire(
          instruction, OutputCPURegister(instruction), field, /* needs_null_check */ true);
    } else {
      codegen_->Load(field_type, OutputCPURegister(instruction), field);
      codegen_->MaybeRecordImplicitNullCheck(instruction);
    }
    if (field_type == Primitive::kPrimNot) {
      // If read barriers are enabled, emit read barriers other than
      // Baker's using a slow path (and also unpoison the loaded
      // reference, if heap poisoning is enabled).
      codegen_->MaybeGenerateReadBarrierSlow(instruction, out, out, base_loc, offset);
    }
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
      codegen_->StoreRelease(field_type, source, HeapOperand(obj, offset));
      codegen_->MaybeRecordImplicitNullCheck(instruction);
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
      } else if (instr->IsRor()) {
        if (rhs.IsImmediate()) {
          uint32_t shift = rhs.immediate() & (lhs.SizeInBits() - 1);
          __ Ror(dst, lhs, shift);
        } else {
          // Ensure shift distance is in the same size register as the result. If
          // we are rotating a long and the shift comes in a w register originally,
          // we don't need to sxtw for use as an x since the shift distances are
          // all & reg_bits - 1.
          __ Ror(dst, lhs, RegisterFrom(instr->GetLocations()->InAt(1), type));
        }
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
        uint32_t shift_value = rhs.immediate() &
            (type == Primitive::kPrimInt ? kMaxIntShiftDistance : kMaxLongShiftDistance);
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

void LocationsBuilderARM64::VisitBitwiseNegatedRight(HBitwiseNegatedRight* instr) {
  DCHECK(Primitive::IsIntegralType(instr->GetType())) << instr->GetType();
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instr);
  locations->SetInAt(0, Location::RequiresRegister());
  // There is no immediate variant of negated bitwise instructions in AArch64.
  locations->SetInAt(1, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

void InstructionCodeGeneratorARM64::VisitBitwiseNegatedRight(HBitwiseNegatedRight* instr) {
  Register dst = OutputRegister(instr);
  Register lhs = InputRegisterAt(instr, 0);
  Register rhs = InputRegisterAt(instr, 1);

  switch (instr->GetOpKind()) {
    case HInstruction::kAnd:
      __ Bic(dst, lhs, rhs);
      break;
    case HInstruction::kOr:
      __ Orn(dst, lhs, rhs);
      break;
    case HInstruction::kXor:
      __ Eon(dst, lhs, rhs);
      break;
    default:
      LOG(FATAL) << "Unreachable";
  }
}

void LocationsBuilderARM64::VisitArm64DataProcWithShifterOp(
    HArm64DataProcWithShifterOp* instruction) {
  DCHECK(instruction->GetType() == Primitive::kPrimInt ||
         instruction->GetType() == Primitive::kPrimLong);
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  if (instruction->GetInstrKind() == HInstruction::kNeg) {
    locations->SetInAt(0, Location::ConstantLocation(instruction->InputAt(0)->AsConstant()));
  } else {
    locations->SetInAt(0, Location::RequiresRegister());
  }
  locations->SetInAt(1, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

void InstructionCodeGeneratorARM64::VisitArm64DataProcWithShifterOp(
    HArm64DataProcWithShifterOp* instruction) {
  Primitive::Type type = instruction->GetType();
  HInstruction::InstructionKind kind = instruction->GetInstrKind();
  DCHECK(type == Primitive::kPrimInt || type == Primitive::kPrimLong);
  Register out = OutputRegister(instruction);
  Register left;
  if (kind != HInstruction::kNeg) {
    left = InputRegisterAt(instruction, 0);
  }
  // If this `HArm64DataProcWithShifterOp` was created by merging a type conversion as the
  // shifter operand operation, the IR generating `right_reg` (input to the type
  // conversion) can have a different type from the current instruction's type,
  // so we manually indicate the type.
  Register right_reg = RegisterFrom(instruction->GetLocations()->InAt(1), type);
  int64_t shift_amount = instruction->GetShiftAmount() &
      (type == Primitive::kPrimInt ? kMaxIntShiftDistance : kMaxLongShiftDistance);

  Operand right_operand(0);

  HArm64DataProcWithShifterOp::OpKind op_kind = instruction->GetOpKind();
  if (HArm64DataProcWithShifterOp::IsExtensionOp(op_kind)) {
    right_operand = Operand(right_reg, helpers::ExtendFromOpKind(op_kind));
  } else {
    right_operand = Operand(right_reg, helpers::ShiftFromOpKind(op_kind), shift_amount);
  }

  // Logical binary operations do not support extension operations in the
  // operand. Note that VIXL would still manage if it was passed by generating
  // the extension as a separate instruction.
  // `HNeg` also does not support extension. See comments in `ShifterOperandSupportsExtension()`.
  DCHECK(!right_operand.IsExtendedRegister() ||
         (kind != HInstruction::kAnd && kind != HInstruction::kOr && kind != HInstruction::kXor &&
          kind != HInstruction::kNeg));
  switch (kind) {
    case HInstruction::kAdd:
      __ Add(out, left, right_operand);
      break;
    case HInstruction::kAnd:
      __ And(out, left, right_operand);
      break;
    case HInstruction::kNeg:
      DCHECK(instruction->InputAt(0)->AsConstant()->IsArithmeticZero());
      __ Neg(out, right_operand);
      break;
    case HInstruction::kOr:
      __ Orr(out, left, right_operand);
      break;
    case HInstruction::kSub:
      __ Sub(out, left, right_operand);
      break;
    case HInstruction::kXor:
      __ Eor(out, left, right_operand);
      break;
    default:
      LOG(FATAL) << "Unexpected operation kind: " << kind;
      UNREACHABLE();
  }
}

void LocationsBuilderARM64::VisitArm64IntermediateAddress(HArm64IntermediateAddress* instruction) {
  // The read barrier instrumentation does not support the
  // HArm64IntermediateAddress instruction yet.
  DCHECK(!kEmitCompilerReadBarrier);
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, ARM64EncodableConstantOrRegister(instruction->GetOffset(), instruction));
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorARM64::VisitArm64IntermediateAddress(
    HArm64IntermediateAddress* instruction) {
  // The read barrier instrumentation does not support the
  // HArm64IntermediateAddress instruction yet.
  DCHECK(!kEmitCompilerReadBarrier);
  __ Add(OutputRegister(instruction),
         InputRegisterAt(instruction, 0),
         Operand(InputOperandAt(instruction, 1)));
}

void LocationsBuilderARM64::VisitMultiplyAccumulate(HMultiplyAccumulate* instr) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instr, LocationSummary::kNoCall);
  HInstruction* accumulator = instr->InputAt(HMultiplyAccumulate::kInputAccumulatorIndex);
  if (instr->GetOpKind() == HInstruction::kSub &&
      accumulator->IsConstant() &&
      accumulator->AsConstant()->IsArithmeticZero()) {
    // Don't allocate register for Mneg instruction.
  } else {
    locations->SetInAt(HMultiplyAccumulate::kInputAccumulatorIndex,
                       Location::RequiresRegister());
  }
  locations->SetInAt(HMultiplyAccumulate::kInputMulLeftIndex, Location::RequiresRegister());
  locations->SetInAt(HMultiplyAccumulate::kInputMulRightIndex, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

void InstructionCodeGeneratorARM64::VisitMultiplyAccumulate(HMultiplyAccumulate* instr) {
  Register res = OutputRegister(instr);
  Register mul_left = InputRegisterAt(instr, HMultiplyAccumulate::kInputMulLeftIndex);
  Register mul_right = InputRegisterAt(instr, HMultiplyAccumulate::kInputMulRightIndex);

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
    Register accumulator = InputRegisterAt(instr, HMultiplyAccumulate::kInputAccumulatorIndex);
    __ Madd(res, mul_left, mul_right, accumulator);
  } else {
    DCHECK(instr->GetOpKind() == HInstruction::kSub);
    HInstruction* accum_instr = instr->InputAt(HMultiplyAccumulate::kInputAccumulatorIndex);
    if (accum_instr->IsConstant() && accum_instr->AsConstant()->IsArithmeticZero()) {
      __ Mneg(res, mul_left, mul_right);
    } else {
      Register accumulator = InputRegisterAt(instr, HMultiplyAccumulate::kInputAccumulatorIndex);
      __ Msub(res, mul_left, mul_right, accumulator);
    }
  }
}

void LocationsBuilderARM64::VisitArrayGet(HArrayGet* instruction) {
  bool object_array_get_with_read_barrier =
      kEmitCompilerReadBarrier && (instruction->GetType() == Primitive::kPrimNot);
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction,
                                                   object_array_get_with_read_barrier ?
                                                       LocationSummary::kCallOnSlowPath :
                                                       LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RegisterOrConstant(instruction->InputAt(1)));
  if (Primitive::IsFloatingPointType(instruction->GetType())) {
    locations->SetOut(Location::RequiresFpuRegister(), Location::kNoOutputOverlap);
  } else {
    // The output overlaps in the case of an object array get with
    // read barriers enabled: we do not want the move to overwrite the
    // array's location, as we need it to emit the read barrier.
    locations->SetOut(
        Location::RequiresRegister(),
        object_array_get_with_read_barrier ? Location::kOutputOverlap : Location::kNoOutputOverlap);
  }
}

void InstructionCodeGeneratorARM64::VisitArrayGet(HArrayGet* instruction) {
  Primitive::Type type = instruction->GetType();
  Register obj = InputRegisterAt(instruction, 0);
  LocationSummary* locations = instruction->GetLocations();
  Location index = locations->InAt(1);
  uint32_t offset = mirror::Array::DataOffset(Primitive::ComponentSize(type)).Uint32Value();
  Location out = locations->Out();

  MacroAssembler* masm = GetVIXLAssembler();
  UseScratchRegisterScope temps(masm);
  // Block pools between `Load` and `MaybeRecordImplicitNullCheck`.
  BlockPoolsScope block_pools(masm);

  if (type == Primitive::kPrimNot && kEmitCompilerReadBarrier && kUseBakerReadBarrier) {
    // Object ArrayGet with Baker's read barrier case.
    Register temp = temps.AcquireW();
    // The read barrier instrumentation does not support the
    // HArm64IntermediateAddress instruction yet.
    DCHECK(!instruction->GetArray()->IsArm64IntermediateAddress());
    // Note that a potential implicit null check is handled in the
    // CodeGeneratorARM64::GenerateArrayLoadWithBakerReadBarrier call.
    codegen_->GenerateArrayLoadWithBakerReadBarrier(
        instruction, out, obj.W(), offset, index, temp, /* needs_null_check */ true);
  } else {
    // General case.
    MemOperand source = HeapOperand(obj);
    if (index.IsConstant()) {
      offset += Int64ConstantFrom(index) << Primitive::ComponentSizeShift(type);
      source = HeapOperand(obj, offset);
    } else {
      Register temp = temps.AcquireSameSizeAs(obj);
      if (instruction->GetArray()->IsArm64IntermediateAddress()) {
        // The read barrier instrumentation does not support the
        // HArm64IntermediateAddress instruction yet.
        DCHECK(!kEmitCompilerReadBarrier);
        // We do not need to compute the intermediate address from the array: the
        // input instruction has done it already. See the comment in
        // `InstructionSimplifierArm64::TryExtractArrayAccessAddress()`.
        if (kIsDebugBuild) {
          HArm64IntermediateAddress* tmp = instruction->GetArray()->AsArm64IntermediateAddress();
          DCHECK_EQ(tmp->GetOffset()->AsIntConstant()->GetValueAsUint64(), offset);
        }
        temp = obj;
      } else {
        __ Add(temp, obj, offset);
      }
      source = HeapOperand(temp, XRegisterFrom(index), LSL, Primitive::ComponentSizeShift(type));
    }

    codegen_->Load(type, OutputCPURegister(instruction), source);
    codegen_->MaybeRecordImplicitNullCheck(instruction);

    if (type == Primitive::kPrimNot) {
      static_assert(
          sizeof(mirror::HeapReference<mirror::Object>) == sizeof(int32_t),
          "art::mirror::HeapReference<art::mirror::Object> and int32_t have different sizes.");
      Location obj_loc = locations->InAt(0);
      if (index.IsConstant()) {
        codegen_->MaybeGenerateReadBarrierSlow(instruction, out, out, obj_loc, offset);
      } else {
        codegen_->MaybeGenerateReadBarrierSlow(instruction, out, out, obj_loc, offset, index);
      }
    }
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
  Primitive::Type value_type = instruction->GetComponentType();

  bool may_need_runtime_call_for_type_check = instruction->NeedsTypeCheck();
  bool object_array_set_with_read_barrier =
      kEmitCompilerReadBarrier && (value_type == Primitive::kPrimNot);
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(
      instruction,
      (may_need_runtime_call_for_type_check  || object_array_set_with_read_barrier) ?
          LocationSummary::kCallOnSlowPath :
          LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RegisterOrConstant(instruction->InputAt(1)));
  if (Primitive::IsFloatingPointType(value_type)) {
    locations->SetInAt(2, Location::RequiresFpuRegister());
  } else {
    locations->SetInAt(2, Location::RequiresRegister());
  }
}

void InstructionCodeGeneratorARM64::VisitArraySet(HArraySet* instruction) {
  Primitive::Type value_type = instruction->GetComponentType();
  LocationSummary* locations = instruction->GetLocations();
  bool may_need_runtime_call_for_type_check = instruction->NeedsTypeCheck();
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
    DCHECK(!may_need_runtime_call_for_type_check);
    if (index.IsConstant()) {
      offset += Int64ConstantFrom(index) << Primitive::ComponentSizeShift(value_type);
      destination = HeapOperand(array, offset);
    } else {
      UseScratchRegisterScope temps(masm);
      Register temp = temps.AcquireSameSizeAs(array);
      if (instruction->GetArray()->IsArm64IntermediateAddress()) {
        // The read barrier instrumentation does not support the
        // HArm64IntermediateAddress instruction yet.
        DCHECK(!kEmitCompilerReadBarrier);
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

      if (may_need_runtime_call_for_type_check) {
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

        if (kEmitCompilerReadBarrier) {
          // When read barriers are enabled, the type checking
          // instrumentation requires two read barriers:
          //
          //   __ Mov(temp2, temp);
          //   // /* HeapReference<Class> */ temp = temp->component_type_
          //   __ Ldr(temp, HeapOperand(temp, component_offset));
          //   codegen_->GenerateReadBarrierSlow(
          //       instruction, temp_loc, temp_loc, temp2_loc, component_offset);
          //
          //   // /* HeapReference<Class> */ temp2 = value->klass_
          //   __ Ldr(temp2, HeapOperand(Register(value), class_offset));
          //   codegen_->GenerateReadBarrierSlow(
          //       instruction, temp2_loc, temp2_loc, value_loc, class_offset, temp_loc);
          //
          //   __ Cmp(temp, temp2);
          //
          // However, the second read barrier may trash `temp`, as it
          // is a temporary register, and as such would not be saved
          // along with live registers before calling the runtime (nor
          // restored afterwards).  So in this case, we bail out and
          // delegate the work to the array set slow path.
          //
          // TODO: Extend the register allocator to support a new
          // "(locally) live temp" location so as to avoid always
          // going into the slow path when read barriers are enabled.
          __ B(slow_path->GetEntryLabel());
        } else {
          Register temp2 = temps.AcquireSameSizeAs(array);
          // /* HeapReference<Class> */ temp = array->klass_
          __ Ldr(temp, HeapOperand(array, class_offset));
          codegen_->MaybeRecordImplicitNullCheck(instruction);
          GetAssembler()->MaybeUnpoisonHeapReference(temp);

          // /* HeapReference<Class> */ temp = temp->component_type_
          __ Ldr(temp, HeapOperand(temp, component_offset));
          // /* HeapReference<Class> */ temp2 = value->klass_
          __ Ldr(temp2, HeapOperand(Register(value), class_offset));
          // If heap poisoning is enabled, no need to unpoison `temp`
          // nor `temp2`, as we are comparing two poisoned references.
          __ Cmp(temp, temp2);

          if (instruction->StaticTypeOfArrayIsObjectArray()) {
            vixl::Label do_put;
            __ B(eq, &do_put);
            // If heap poisoning is enabled, the `temp` reference has
            // not been unpoisoned yet; unpoison it now.
            GetAssembler()->MaybeUnpoisonHeapReference(temp);

            // /* HeapReference<Class> */ temp = temp->super_class_
            __ Ldr(temp, HeapOperand(temp, super_offset));
            // If heap poisoning is enabled, no need to unpoison
            // `temp`, as we are comparing against null below.
            __ Cbnz(temp, slow_path->GetEntryLabel());
            __ Bind(&do_put);
          } else {
            __ B(ne, slow_path->GetEntryLabel());
          }
          temps.Release(temp2);
        }
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

      if (!may_need_runtime_call_for_type_check) {
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

static bool IsFloatingPointZeroConstant(HInstruction* inst) {
  return (inst->IsFloatConstant() && (inst->AsFloatConstant()->IsArithmeticZero()))
      || (inst->IsDoubleConstant() && (inst->AsDoubleConstant()->IsArithmeticZero()));
}

void InstructionCodeGeneratorARM64::GenerateFcmp(HInstruction* instruction) {
  FPRegister lhs_reg = InputFPRegisterAt(instruction, 0);
  Location rhs_loc = instruction->GetLocations()->InAt(1);
  if (rhs_loc.IsConstant()) {
    // 0.0 is the only immediate that can be encoded directly in
    // an FCMP instruction.
    //
    // Both the JLS (section 15.20.1) and the JVMS (section 6.5)
    // specify that in a floating-point comparison, positive zero
    // and negative zero are considered equal, so we can use the
    // literal 0.0 for both cases here.
    //
    // Note however that some methods (Float.equal, Float.compare,
    // Float.compareTo, Double.equal, Double.compare,
    // Double.compareTo, Math.max, Math.min, StrictMath.max,
    // StrictMath.min) consider 0.0 to be (strictly) greater than
    // -0.0. So if we ever translate calls to these methods into a
    // HCompare instruction, we must handle the -0.0 case with
    // care here.
    DCHECK(IsFloatingPointZeroConstant(rhs_loc.GetConstant()));
    __ Fcmp(lhs_reg, 0.0);
  } else {
    __ Fcmp(lhs_reg, InputFPRegisterAt(instruction, 1));
  }
}

void LocationsBuilderARM64::VisitCompare(HCompare* compare) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(compare, LocationSummary::kNoCall);
  Primitive::Type in_type = compare->InputAt(0)->GetType();
  switch (in_type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimShort:
    case Primitive::kPrimChar:
    case Primitive::kPrimInt:
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
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimShort:
    case Primitive::kPrimChar:
    case Primitive::kPrimInt:
    case Primitive::kPrimLong: {
      Register result = OutputRegister(compare);
      Register left = InputRegisterAt(compare, 0);
      Operand right = InputOperandAt(compare, 1);
      __ Cmp(left, right);
      __ Cset(result, ne);          // result == +1 if NE or 0 otherwise
      __ Cneg(result, result, lt);  // result == -1 if LT or unchanged otherwise
      break;
    }
    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      Register result = OutputRegister(compare);
      GenerateFcmp(compare);
      __ Cset(result, ne);
      __ Cneg(result, result, ARM64FPCondition(kCondLT, compare->IsGtBias()));
      break;
    }
    default:
      LOG(FATAL) << "Unimplemented compare type " << in_type;
  }
}

void LocationsBuilderARM64::HandleCondition(HCondition* instruction) {
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

  if (!instruction->IsEmittedAtUseSite()) {
    locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
  }
}

void InstructionCodeGeneratorARM64::HandleCondition(HCondition* instruction) {
  if (instruction->IsEmittedAtUseSite()) {
    return;
  }

  LocationSummary* locations = instruction->GetLocations();
  Register res = RegisterFrom(locations->Out(), instruction->GetType());
  IfCondition if_cond = instruction->GetCondition();

  if (Primitive::IsFloatingPointType(instruction->InputAt(0)->GetType())) {
    GenerateFcmp(instruction);
    __ Cset(res, ARM64FPCondition(if_cond, instruction->IsGtBias()));
  } else {
    // Integer cases.
    Register lhs = InputRegisterAt(instruction, 0);
    Operand rhs = InputOperandAt(instruction, 1);
    __ Cmp(lhs, rhs);
    __ Cset(res, ARM64Condition(if_cond));
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
void LocationsBuilderARM64::Visit##Name(H##Name* comp) { HandleCondition(comp); }         \
void InstructionCodeGeneratorARM64::Visit##Name(H##Name* comp) { HandleCondition(comp); }
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
  uint64_t abs_imm = static_cast<uint64_t>(AbsOrMin(imm));
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
    } else if (IsPowerOfTwo(AbsOrMin(imm))) {
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

  if (!Primitive::IsIntegralType(type)) {
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
    // Constant condition, statically compared against "true" (integer value 1).
    if (cond->AsIntConstant()->IsTrue()) {
      if (true_target != nullptr) {
        __ B(true_target);
      }
    } else {
      DCHECK(cond->AsIntConstant()->IsFalse()) << cond->AsIntConstant()->GetValue();
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
      GenerateFcmp(condition);
      if (true_target == nullptr) {
        IfCondition opposite_condition = condition->GetOppositeCondition();
        __ B(ARM64FPCondition(opposite_condition, condition->IsGtBias()), false_target);
      } else {
        __ B(ARM64FPCondition(condition->GetCondition(), condition->IsGtBias()), true_target);
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

      if ((arm64_cond == eq || arm64_cond == ne || arm64_cond == lt || arm64_cond == ge) &&
          rhs.IsImmediate() && (rhs.immediate() == 0)) {
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
  SlowPathCodeARM64* slow_path =
      deopt_slow_paths_.NewSlowPath<DeoptimizationSlowPathARM64>(deoptimize);
  GenerateTestAndBranch(deoptimize,
                        /* condition_input_index */ 0,
                        slow_path->GetEntryLabel(),
                        /* false_target */ nullptr);
}

enum SelectVariant {
  kCsel,
  kCselFalseConst,
  kCselTrueConst,
  kFcsel,
};

static inline bool IsConditionOnFloatingPointValues(HInstruction* condition) {
  return condition->IsCondition() &&
         Primitive::IsFloatingPointType(condition->InputAt(0)->GetType());
}

static inline bool IsRecognizedCselConstant(HInstruction* constant) {
  if (constant->IsConstant()) {
    int64_t value = Int64FromConstant(constant->AsConstant());
    if ((value == -1) || (value == 0) || (value == 1)) {
      return true;
    }
  }
  return false;
}

static inline SelectVariant GetSelectVariant(HSelect* select) {
  if (Primitive::IsFloatingPointType(select->GetType())) {
    return kFcsel;
  } else if (IsRecognizedCselConstant(select->GetFalseValue())) {
    return kCselFalseConst;
  } else if (IsRecognizedCselConstant(select->GetTrueValue())) {
    return kCselTrueConst;
  } else {
    return kCsel;
  }
}

static inline bool HasSwappedInputs(SelectVariant variant) {
  return variant == kCselTrueConst;
}

static inline Condition GetConditionForSelect(HCondition* condition, SelectVariant variant) {
  IfCondition cond = HasSwappedInputs(variant) ? condition->GetOppositeCondition()
                                               : condition->GetCondition();
  return IsConditionOnFloatingPointValues(condition) ? ARM64FPCondition(cond, condition->IsGtBias())
                                                     : ARM64Condition(cond);
}

void LocationsBuilderARM64::VisitSelect(HSelect* select) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(select);
  switch (GetSelectVariant(select)) {
    case kCsel:
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RequiresRegister());
      locations->SetOut(Location::RequiresRegister());
      break;
    case kCselFalseConst:
      locations->SetInAt(0, Location::ConstantLocation(select->InputAt(0)->AsConstant()));
      locations->SetInAt(1, Location::RequiresRegister());
      locations->SetOut(Location::RequiresRegister());
      break;
    case kCselTrueConst:
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::ConstantLocation(select->InputAt(1)->AsConstant()));
      locations->SetOut(Location::RequiresRegister());
      break;
    case kFcsel:
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetInAt(1, Location::RequiresFpuRegister());
      locations->SetOut(Location::RequiresFpuRegister());
      break;
  }
  if (IsBooleanValueOrMaterializedCondition(select->GetCondition())) {
    locations->SetInAt(2, Location::RequiresRegister());
  }
}

void InstructionCodeGeneratorARM64::VisitSelect(HSelect* select) {
  HInstruction* cond = select->GetCondition();
  SelectVariant variant = GetSelectVariant(select);
  Condition csel_cond;

  if (IsBooleanValueOrMaterializedCondition(cond)) {
    if (cond->IsCondition() && cond->GetNext() == select) {
      // Condition codes set from previous instruction.
      csel_cond = GetConditionForSelect(cond->AsCondition(), variant);
    } else {
      __ Cmp(InputRegisterAt(select, 2), 0);
      csel_cond = HasSwappedInputs(variant) ? eq : ne;
    }
  } else if (IsConditionOnFloatingPointValues(cond)) {
    GenerateFcmp(cond);
    csel_cond = GetConditionForSelect(cond->AsCondition(), variant);
  } else {
    __ Cmp(InputRegisterAt(cond, 0), InputOperandAt(cond, 1));
    csel_cond = GetConditionForSelect(cond->AsCondition(), variant);
  }

  switch (variant) {
    case kCsel:
    case kCselFalseConst:
      __ Csel(OutputRegister(select),
              InputRegisterAt(select, 1),
              InputOperandAt(select, 0),
              csel_cond);
      break;
    case kCselTrueConst:
      __ Csel(OutputRegister(select),
              InputRegisterAt(select, 0),
              InputOperandAt(select, 1),
              csel_cond);
      break;
    case kFcsel:
      __ Fcsel(OutputFPRegister(select),
               InputFPRegisterAt(select, 1),
               InputFPRegisterAt(select, 0),
               csel_cond);
      break;
  }
}

void LocationsBuilderARM64::VisitNativeDebugInfo(HNativeDebugInfo* info) {
  new (GetGraph()->GetArena()) LocationSummary(info);
}

void InstructionCodeGeneratorARM64::VisitNativeDebugInfo(HNativeDebugInfo*) {
  // MaybeRecordNativeDebugInfo is already called implicitly in CodeGenerator::Compile.
}

void CodeGeneratorARM64::GenerateNop() {
  __ Nop();
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

static bool TypeCheckNeedsATemporary(TypeCheckKind type_check_kind) {
  return kEmitCompilerReadBarrier &&
      (kUseBakerReadBarrier ||
       type_check_kind == TypeCheckKind::kAbstractClassCheck ||
       type_check_kind == TypeCheckKind::kClassHierarchyCheck ||
       type_check_kind == TypeCheckKind::kArrayObjectCheck);
}

void LocationsBuilderARM64::VisitInstanceOf(HInstanceOf* instruction) {
  LocationSummary::CallKind call_kind = LocationSummary::kNoCall;
  TypeCheckKind type_check_kind = instruction->GetTypeCheckKind();
  switch (type_check_kind) {
    case TypeCheckKind::kExactCheck:
    case TypeCheckKind::kAbstractClassCheck:
    case TypeCheckKind::kClassHierarchyCheck:
    case TypeCheckKind::kArrayObjectCheck:
      call_kind =
          kEmitCompilerReadBarrier ? LocationSummary::kCallOnSlowPath : LocationSummary::kNoCall;
      break;
    case TypeCheckKind::kArrayCheck:
    case TypeCheckKind::kUnresolvedCheck:
    case TypeCheckKind::kInterfaceCheck:
      call_kind = LocationSummary::kCallOnSlowPath;
      break;
  }

  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction, call_kind);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  // The "out" register is used as a temporary, so it overlaps with the inputs.
  // Note that TypeCheckSlowPathARM64 uses this register too.
  locations->SetOut(Location::RequiresRegister(), Location::kOutputOverlap);
  // When read barriers are enabled, we need a temporary register for
  // some cases.
  if (TypeCheckNeedsATemporary(type_check_kind)) {
    locations->AddTemp(Location::RequiresRegister());
  }
}

void InstructionCodeGeneratorARM64::VisitInstanceOf(HInstanceOf* instruction) {
  TypeCheckKind type_check_kind = instruction->GetTypeCheckKind();
  LocationSummary* locations = instruction->GetLocations();
  Location obj_loc = locations->InAt(0);
  Register obj = InputRegisterAt(instruction, 0);
  Register cls = InputRegisterAt(instruction, 1);
  Location out_loc = locations->Out();
  Register out = OutputRegister(instruction);
  Location maybe_temp_loc = TypeCheckNeedsATemporary(type_check_kind) ?
      locations->GetTemp(0) :
      Location::NoLocation();
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

  // /* HeapReference<Class> */ out = obj->klass_
  GenerateReferenceLoadTwoRegisters(instruction, out_loc, obj_loc, class_offset, maybe_temp_loc);

  switch (type_check_kind) {
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
      // /* HeapReference<Class> */ out = out->super_class_
      GenerateReferenceLoadOneRegister(instruction, out_loc, super_offset, maybe_temp_loc);
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
      // /* HeapReference<Class> */ out = out->super_class_
      GenerateReferenceLoadOneRegister(instruction, out_loc, super_offset, maybe_temp_loc);
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
      // Otherwise, we need to check that the object's class is a non-primitive array.
      // /* HeapReference<Class> */ out = out->component_type_
      GenerateReferenceLoadOneRegister(instruction, out_loc, component_offset, maybe_temp_loc);
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
      slow_path = new (GetGraph()->GetArena()) TypeCheckSlowPathARM64(instruction,
                                                                      /* is_fatal */ false);
      codegen_->AddSlowPath(slow_path);
      __ B(ne, slow_path->GetEntryLabel());
      __ Mov(out, 1);
      if (zero.IsLinked()) {
        __ B(&done);
      }
      break;
    }

    case TypeCheckKind::kUnresolvedCheck:
    case TypeCheckKind::kInterfaceCheck: {
      // Note that we indeed only call on slow path, but we always go
      // into the slow path for the unresolved and interface check
      // cases.
      //
      // We cannot directly call the InstanceofNonTrivial runtime
      // entry point without resorting to a type checking slow path
      // here (i.e. by calling InvokeRuntime directly), as it would
      // require to assign fixed registers for the inputs of this
      // HInstanceOf instruction (following the runtime calling
      // convention), which might be cluttered by the potential first
      // read barrier emission at the beginning of this method.
      //
      // TODO: Introduce a new runtime entry point taking the object
      // to test (instead of its class) as argument, and let it deal
      // with the read barrier issues. This will let us refactor this
      // case of the `switch` code as it was previously (with a direct
      // call to the runtime not using a type checking slow path).
      // This should also be beneficial for the other cases above.
      DCHECK(locations->OnlyCallsOnSlowPath());
      slow_path = new (GetGraph()->GetArena()) TypeCheckSlowPathARM64(instruction,
                                                                      /* is_fatal */ false);
      codegen_->AddSlowPath(slow_path);
      __ B(slow_path->GetEntryLabel());
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

  TypeCheckKind type_check_kind = instruction->GetTypeCheckKind();
  switch (type_check_kind) {
    case TypeCheckKind::kExactCheck:
    case TypeCheckKind::kAbstractClassCheck:
    case TypeCheckKind::kClassHierarchyCheck:
    case TypeCheckKind::kArrayObjectCheck:
      call_kind = (throws_into_catch || kEmitCompilerReadBarrier) ?
          LocationSummary::kCallOnSlowPath :
          LocationSummary::kNoCall;  // In fact, call on a fatal (non-returning) slow path.
      break;
    case TypeCheckKind::kArrayCheck:
    case TypeCheckKind::kUnresolvedCheck:
    case TypeCheckKind::kInterfaceCheck:
      call_kind = LocationSummary::kCallOnSlowPath;
      break;
  }

  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction, call_kind);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  // Note that TypeCheckSlowPathARM64 uses this "temp" register too.
  locations->AddTemp(Location::RequiresRegister());
  // When read barriers are enabled, we need an additional temporary
  // register for some cases.
  if (TypeCheckNeedsATemporary(type_check_kind)) {
    locations->AddTemp(Location::RequiresRegister());
  }
}

void InstructionCodeGeneratorARM64::VisitCheckCast(HCheckCast* instruction) {
  TypeCheckKind type_check_kind = instruction->GetTypeCheckKind();
  LocationSummary* locations = instruction->GetLocations();
  Location obj_loc = locations->InAt(0);
  Register obj = InputRegisterAt(instruction, 0);
  Register cls = InputRegisterAt(instruction, 1);
  Location temp_loc = locations->GetTemp(0);
  Location maybe_temp2_loc = TypeCheckNeedsATemporary(type_check_kind) ?
      locations->GetTemp(1) :
      Location::NoLocation();
  Register temp = WRegisterFrom(temp_loc);
  uint32_t class_offset = mirror::Object::ClassOffset().Int32Value();
  uint32_t super_offset = mirror::Class::SuperClassOffset().Int32Value();
  uint32_t component_offset = mirror::Class::ComponentTypeOffset().Int32Value();
  uint32_t primitive_offset = mirror::Class::PrimitiveTypeOffset().Int32Value();

  bool is_type_check_slow_path_fatal =
      (type_check_kind == TypeCheckKind::kExactCheck ||
       type_check_kind == TypeCheckKind::kAbstractClassCheck ||
       type_check_kind == TypeCheckKind::kClassHierarchyCheck ||
       type_check_kind == TypeCheckKind::kArrayObjectCheck) &&
      !instruction->CanThrowIntoCatchBlock();
  SlowPathCodeARM64* type_check_slow_path =
      new (GetGraph()->GetArena()) TypeCheckSlowPathARM64(instruction,
                                                          is_type_check_slow_path_fatal);
  codegen_->AddSlowPath(type_check_slow_path);

  vixl::Label done;
  // Avoid null check if we know obj is not null.
  if (instruction->MustDoNullCheck()) {
    __ Cbz(obj, &done);
  }

  // /* HeapReference<Class> */ temp = obj->klass_
  GenerateReferenceLoadTwoRegisters(instruction, temp_loc, obj_loc, class_offset, maybe_temp2_loc);

  switch (type_check_kind) {
    case TypeCheckKind::kExactCheck:
    case TypeCheckKind::kArrayCheck: {
      __ Cmp(temp, cls);
      // Jump to slow path for throwing the exception or doing a
      // more involved array check.
      __ B(ne, type_check_slow_path->GetEntryLabel());
      break;
    }

    case TypeCheckKind::kAbstractClassCheck: {
      // If the class is abstract, we eagerly fetch the super class of the
      // object to avoid doing a comparison we know will fail.
      vixl::Label loop, compare_classes;
      __ Bind(&loop);
      // /* HeapReference<Class> */ temp = temp->super_class_
      GenerateReferenceLoadOneRegister(instruction, temp_loc, super_offset, maybe_temp2_loc);

      // If the class reference currently in `temp` is not null, jump
      // to the `compare_classes` label to compare it with the checked
      // class.
      __ Cbnz(temp, &compare_classes);
      // Otherwise, jump to the slow path to throw the exception.
      //
      // But before, move back the object's class into `temp` before
      // going into the slow path, as it has been overwritten in the
      // meantime.
      // /* HeapReference<Class> */ temp = obj->klass_
      GenerateReferenceLoadTwoRegisters(
          instruction, temp_loc, obj_loc, class_offset, maybe_temp2_loc);
      __ B(type_check_slow_path->GetEntryLabel());

      __ Bind(&compare_classes);
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

      // /* HeapReference<Class> */ temp = temp->super_class_
      GenerateReferenceLoadOneRegister(instruction, temp_loc, super_offset, maybe_temp2_loc);

      // If the class reference currently in `temp` is not null, jump
      // back at the beginning of the loop.
      __ Cbnz(temp, &loop);
      // Otherwise, jump to the slow path to throw the exception.
      //
      // But before, move back the object's class into `temp` before
      // going into the slow path, as it has been overwritten in the
      // meantime.
      // /* HeapReference<Class> */ temp = obj->klass_
      GenerateReferenceLoadTwoRegisters(
          instruction, temp_loc, obj_loc, class_offset, maybe_temp2_loc);
      __ B(type_check_slow_path->GetEntryLabel());
      break;
    }

    case TypeCheckKind::kArrayObjectCheck: {
      // Do an exact check.
      vixl::Label check_non_primitive_component_type;
      __ Cmp(temp, cls);
      __ B(eq, &done);

      // Otherwise, we need to check that the object's class is a non-primitive array.
      // /* HeapReference<Class> */ temp = temp->component_type_
      GenerateReferenceLoadOneRegister(instruction, temp_loc, component_offset, maybe_temp2_loc);

      // If the component type is not null (i.e. the object is indeed
      // an array), jump to label `check_non_primitive_component_type`
      // to further check that this component type is not a primitive
      // type.
      __ Cbnz(temp, &check_non_primitive_component_type);
      // Otherwise, jump to the slow path to throw the exception.
      //
      // But before, move back the object's class into `temp` before
      // going into the slow path, as it has been overwritten in the
      // meantime.
      // /* HeapReference<Class> */ temp = obj->klass_
      GenerateReferenceLoadTwoRegisters(
          instruction, temp_loc, obj_loc, class_offset, maybe_temp2_loc);
      __ B(type_check_slow_path->GetEntryLabel());

      __ Bind(&check_non_primitive_component_type);
      __ Ldrh(temp, HeapOperand(temp, primitive_offset));
      static_assert(Primitive::kPrimNot == 0, "Expected 0 for kPrimNot");
      __ Cbz(temp, &done);
      // Same comment as above regarding `temp` and the slow path.
      // /* HeapReference<Class> */ temp = obj->klass_
      GenerateReferenceLoadTwoRegisters(
          instruction, temp_loc, obj_loc, class_offset, maybe_temp2_loc);
      __ B(type_check_slow_path->GetEntryLabel());
      break;
    }

    case TypeCheckKind::kUnresolvedCheck:
    case TypeCheckKind::kInterfaceCheck:
      // We always go into the type check slow path for the unresolved
      // and interface check cases.
      //
      // We cannot directly call the CheckCast runtime entry point
      // without resorting to a type checking slow path here (i.e. by
      // calling InvokeRuntime directly), as it would require to
      // assign fixed registers for the inputs of this HInstanceOf
      // instruction (following the runtime calling convention), which
      // might be cluttered by the potential first read barrier
      // emission at the beginning of this method.
      //
      // TODO: Introduce a new runtime entry point taking the object
      // to test (instead of its class) as argument, and let it deal
      // with the read barrier issues. This will let us refactor this
      // case of the `switch` code as it was previously (with a direct
      // call to the runtime not using a type checking slow path).
      // This should also be beneficial for the other cases above.
      __ B(type_check_slow_path->GetEntryLabel());
      break;
  }
  __ Bind(&done);

  __ Bind(type_check_slow_path->GetExitLabel());
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
  LocationSummary* locations = invoke->GetLocations();
  Register temp = XRegisterFrom(locations->GetTemp(0));
  Location receiver = locations->InAt(0);
  Offset class_offset = mirror::Object::ClassOffset();
  Offset entry_point = ArtMethod::EntryPointFromQuickCompiledCodeOffset(kArm64WordSize);

  // The register ip1 is required to be used for the hidden argument in
  // art_quick_imt_conflict_trampoline, so prevent VIXL from using it.
  MacroAssembler* masm = GetVIXLAssembler();
  UseScratchRegisterScope scratch_scope(masm);
  BlockPoolsScope block_pools(masm);
  scratch_scope.Exclude(ip1);
  __ Mov(ip1, invoke->GetDexMethodIndex());

  if (receiver.IsStackSlot()) {
    __ Ldr(temp.W(), StackOperandFrom(receiver));
    // /* HeapReference<Class> */ temp = temp->klass_
    __ Ldr(temp.W(), HeapOperand(temp.W(), class_offset));
  } else {
    // /* HeapReference<Class> */ temp = receiver->klass_
    __ Ldr(temp.W(), HeapOperandFrom(receiver, class_offset));
  }
  codegen_->MaybeRecordImplicitNullCheck(invoke);
  // Instead of simply (possibly) unpoisoning `temp` here, we should
  // emit a read barrier for the previous class reference load.
  // However this is not required in practice, as this is an
  // intermediate/temporary reference and because the current
  // concurrent copying collector keeps the from-space memory
  // intact/accessible until the end of the marking phase (the
  // concurrent copying collector may not in the future).
  GetAssembler()->MaybeUnpoisonHeapReference(temp.W());
  __ Ldr(temp,
      MemOperand(temp, mirror::Class::ImtPtrOffset(kArm64PointerSize).Uint32Value()));
  uint32_t method_offset = static_cast<uint32_t>(ImTable::OffsetOfElement(
      invoke->GetImtIndex() % ImTable::kSize, kArm64PointerSize));
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
  // Explicit clinit checks triggered by static invokes must have been pruned by
  // art::PrepareForRegisterAllocation.
  DCHECK(!invoke->IsStaticWithExplicitClinitCheck());

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
  // On ARM64 we support all dispatch types.
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
      const DexFile& dex_file = *invoke->GetTargetMethod().dex_file;
      uint32_t element_offset = invoke->GetDexCacheArrayOffset();
      vixl::Label* adrp_label = NewPcRelativeDexCacheArrayPatch(dex_file, element_offset);
      {
        vixl::SingleEmissionCheckScope guard(GetVIXLAssembler());
        __ Bind(adrp_label);
        __ adrp(XRegisterFrom(temp), /* offset placeholder */ 0);
      }
      // Add LDR with its PC-relative DexCache access patch.
      vixl::Label* ldr_label =
          NewPcRelativeDexCacheArrayPatch(dex_file, element_offset, adrp_label);
      {
        vixl::SingleEmissionCheckScope guard(GetVIXLAssembler());
        __ Bind(ldr_label);
        __ ldr(XRegisterFrom(temp), MemOperand(XRegisterFrom(temp), /* offset placeholder */ 0));
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

      // /* ArtMethod*[] */ temp = temp.ptr_sized_fields_->dex_cache_resolved_methods_;
      __ Ldr(reg.X(),
             MemOperand(method_reg.X(),
                        ArtMethod::DexCacheResolvedMethodsOffset(kArm64WordSize).Int32Value()));
      // temp = temp[index_in_cache];
      // Note: Don't use invoke->GetTargetMethod() as it may point to a different dex file.
      uint32_t index_in_cache = invoke->GetDexMethodIndex();
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
  // Use the calling convention instead of the location of the receiver, as
  // intrinsics may have put the receiver in a different register. In the intrinsics
  // slow path, the arguments have been moved to the right place, so here we are
  // guaranteed that the receiver is the first register of the calling convention.
  InvokeDexCallingConvention calling_convention;
  Register receiver = calling_convention.GetRegisterAt(0);
  Register temp = XRegisterFrom(temp_in);
  size_t method_offset = mirror::Class::EmbeddedVTableEntryOffset(
      invoke->GetVTableIndex(), kArm64PointerSize).SizeValue();
  Offset class_offset = mirror::Object::ClassOffset();
  Offset entry_point = ArtMethod::EntryPointFromQuickCompiledCodeOffset(kArm64WordSize);

  BlockPoolsScope block_pools(GetVIXLAssembler());

  DCHECK(receiver.IsRegister());
  // /* HeapReference<Class> */ temp = receiver->klass_
  __ Ldr(temp.W(), HeapOperandFrom(LocationFrom(receiver), class_offset));
  MaybeRecordImplicitNullCheck(invoke);
  // Instead of simply (possibly) unpoisoning `temp` here, we should
  // emit a read barrier for the previous class reference load.
  // intermediate/temporary reference and because the current
  // concurrent copying collector keeps the from-space memory
  // intact/accessible until the end of the marking phase (the
  // concurrent copying collector may not in the future).
  GetAssembler()->MaybeUnpoisonHeapReference(temp.W());
  // temp = temp->GetMethodAt(method_offset);
  __ Ldr(temp, MemOperand(temp, method_offset));
  // lr = temp->GetEntryPoint();
  __ Ldr(lr, MemOperand(temp, entry_point.SizeValue()));
  // lr();
  __ Blr(lr);
}

vixl::Label* CodeGeneratorARM64::NewPcRelativeStringPatch(const DexFile& dex_file,
                                                          uint32_t string_index,
                                                          vixl::Label* adrp_label) {
  return NewPcRelativePatch(dex_file, string_index, adrp_label, &pc_relative_string_patches_);
}

vixl::Label* CodeGeneratorARM64::NewPcRelativeDexCacheArrayPatch(const DexFile& dex_file,
                                                                 uint32_t element_offset,
                                                                 vixl::Label* adrp_label) {
  return NewPcRelativePatch(dex_file, element_offset, adrp_label, &pc_relative_dex_cache_patches_);
}

vixl::Label* CodeGeneratorARM64::NewPcRelativePatch(const DexFile& dex_file,
                                                    uint32_t offset_or_index,
                                                    vixl::Label* adrp_label,
                                                    ArenaDeque<PcRelativePatchInfo>* patches) {
  // Add a patch entry and return the label.
  patches->emplace_back(dex_file, offset_or_index);
  PcRelativePatchInfo* info = &patches->back();
  vixl::Label* label = &info->label;
  // If adrp_label is null, this is the ADRP patch and needs to point to its own label.
  info->pc_insn_label = (adrp_label != nullptr) ? adrp_label : label;
  return label;
}

vixl::Literal<uint32_t>* CodeGeneratorARM64::DeduplicateBootImageStringLiteral(
    const DexFile& dex_file, uint32_t string_index) {
  return boot_image_string_patches_.GetOrCreate(
      StringReference(&dex_file, string_index),
      [this]() { return __ CreateLiteralDestroyedWithPool<uint32_t>(/* placeholder */ 0u); });
}

vixl::Literal<uint32_t>* CodeGeneratorARM64::DeduplicateBootImageAddressLiteral(uint64_t address) {
  bool needs_patch = GetCompilerOptions().GetIncludePatchInformation();
  Uint32ToLiteralMap* map = needs_patch ? &boot_image_address_patches_ : &uint32_literals_;
  return DeduplicateUint32Literal(dchecked_integral_cast<uint32_t>(address), map);
}

vixl::Literal<uint64_t>* CodeGeneratorARM64::DeduplicateDexCacheAddressLiteral(uint64_t address) {
  return DeduplicateUint64Literal(address);
}

void CodeGeneratorARM64::EmitLinkerPatches(ArenaVector<LinkerPatch>* linker_patches) {
  DCHECK(linker_patches->empty());
  size_t size =
      method_patches_.size() +
      call_patches_.size() +
      relative_call_patches_.size() +
      pc_relative_dex_cache_patches_.size() +
      boot_image_string_patches_.size() +
      pc_relative_string_patches_.size() +
      boot_image_address_patches_.size();
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
  for (const PcRelativePatchInfo& info : pc_relative_dex_cache_patches_) {
    linker_patches->push_back(LinkerPatch::DexCacheArrayPatch(info.label.location(),
                                                              &info.target_dex_file,
                                                              info.pc_insn_label->location(),
                                                              info.offset_or_index));
  }
  for (const auto& entry : boot_image_string_patches_) {
    const StringReference& target_string = entry.first;
    vixl::Literal<uint32_t>* literal = entry.second;
    linker_patches->push_back(LinkerPatch::StringPatch(literal->offset(),
                                                       target_string.dex_file,
                                                       target_string.string_index));
  }
  for (const PcRelativePatchInfo& info : pc_relative_string_patches_) {
    linker_patches->push_back(LinkerPatch::RelativeStringPatch(info.label.location(),
                                                               &info.target_dex_file,
                                                               info.pc_insn_label->location(),
                                                               info.offset_or_index));
  }
  for (const auto& entry : boot_image_address_patches_) {
    DCHECK(GetCompilerOptions().GetIncludePatchInformation());
    vixl::Literal<uint32_t>* literal = entry.second;
    linker_patches->push_back(LinkerPatch::RecordPosition(literal->offset()));
  }
}

vixl::Literal<uint32_t>* CodeGeneratorARM64::DeduplicateUint32Literal(uint32_t value,
                                                                      Uint32ToLiteralMap* map) {
  return map->GetOrCreate(
      value,
      [this, value]() { return __ CreateLiteralDestroyedWithPool<uint32_t>(value); });
}

vixl::Literal<uint64_t>* CodeGeneratorARM64::DeduplicateUint64Literal(uint64_t value) {
  return uint64_literals_.GetOrCreate(
      value,
      [this, value]() { return __ CreateLiteralDestroyedWithPool<uint64_t>(value); });
}

vixl::Literal<uint64_t>* CodeGeneratorARM64::DeduplicateMethodLiteral(
    MethodReference target_method,
    MethodToLiteralMap* map) {
  return map->GetOrCreate(
      target_method,
      [this]() { return __ CreateLiteralDestroyedWithPool<uint64_t>(/* placeholder */ 0u); });
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
  // Explicit clinit checks triggered by static invokes must have been pruned by
  // art::PrepareForRegisterAllocation.
  DCHECK(!invoke->IsStaticWithExplicitClinitCheck());

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
      LocationFrom(vixl::x0),
      /* code_generator_supports_read_barrier */ true);
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

  Location out_loc = cls->GetLocations()->Out();
  Register out = OutputRegister(cls);
  Register current_method = InputRegisterAt(cls, 0);
  if (cls->IsReferrersClass()) {
    DCHECK(!cls->CanCallRuntime());
    DCHECK(!cls->MustGenerateClinitCheck());
    // /* GcRoot<mirror::Class> */ out = current_method->declaring_class_
    GenerateGcRootFieldLoad(
        cls, out_loc, current_method, ArtMethod::DeclaringClassOffset().Int32Value());
  } else {
    MemberOffset resolved_types_offset = ArtMethod::DexCacheResolvedTypesOffset(kArm64PointerSize);
    // /* GcRoot<mirror::Class>[] */ out =
    //        current_method.ptr_sized_fields_->dex_cache_resolved_types_
    __ Ldr(out.X(), MemOperand(current_method, resolved_types_offset.Int32Value()));
    // /* GcRoot<mirror::Class> */ out = out[type_index]
    GenerateGcRootFieldLoad(
        cls, out_loc, out.X(), CodeGenerator::GetCacheOffset(cls->GetTypeIndex()));

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

HLoadString::LoadKind CodeGeneratorARM64::GetSupportedLoadStringKind(
    HLoadString::LoadKind desired_string_load_kind) {
  if (kEmitCompilerReadBarrier) {
    switch (desired_string_load_kind) {
      case HLoadString::LoadKind::kBootImageLinkTimeAddress:
      case HLoadString::LoadKind::kBootImageLinkTimePcRelative:
      case HLoadString::LoadKind::kBootImageAddress:
        // TODO: Implement for read barrier.
        return HLoadString::LoadKind::kDexCacheViaMethod;
      default:
        break;
    }
  }
  switch (desired_string_load_kind) {
    case HLoadString::LoadKind::kBootImageLinkTimeAddress:
      DCHECK(!GetCompilerOptions().GetCompilePic());
      break;
    case HLoadString::LoadKind::kBootImageLinkTimePcRelative:
      DCHECK(GetCompilerOptions().GetCompilePic());
      break;
    case HLoadString::LoadKind::kBootImageAddress:
      break;
    case HLoadString::LoadKind::kDexCacheAddress:
      DCHECK(Runtime::Current()->UseJitCompilation());
      break;
    case HLoadString::LoadKind::kDexCachePcRelative:
      DCHECK(!Runtime::Current()->UseJitCompilation());
      break;
    case HLoadString::LoadKind::kDexCacheViaMethod:
      break;
  }
  return desired_string_load_kind;
}

void LocationsBuilderARM64::VisitLoadString(HLoadString* load) {
  LocationSummary::CallKind call_kind = (load->NeedsEnvironment() || kEmitCompilerReadBarrier)
      ? LocationSummary::kCallOnSlowPath
      : LocationSummary::kNoCall;
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(load, call_kind);
  if (load->GetLoadKind() == HLoadString::LoadKind::kDexCacheViaMethod) {
    locations->SetInAt(0, Location::RequiresRegister());
  }
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorARM64::VisitLoadString(HLoadString* load) {
  Location out_loc = load->GetLocations()->Out();
  Register out = OutputRegister(load);

  switch (load->GetLoadKind()) {
    case HLoadString::LoadKind::kBootImageLinkTimeAddress:
      DCHECK(!kEmitCompilerReadBarrier);
      __ Ldr(out, codegen_->DeduplicateBootImageStringLiteral(load->GetDexFile(),
                                                              load->GetStringIndex()));
      return;  // No dex cache slow path.
    case HLoadString::LoadKind::kBootImageLinkTimePcRelative: {
      DCHECK(!kEmitCompilerReadBarrier);
      // Add ADRP with its PC-relative String patch.
      const DexFile& dex_file = load->GetDexFile();
      uint32_t string_index = load->GetStringIndex();
      vixl::Label* adrp_label = codegen_->NewPcRelativeStringPatch(dex_file, string_index);
      {
        vixl::SingleEmissionCheckScope guard(GetVIXLAssembler());
        __ Bind(adrp_label);
        __ adrp(out.X(), /* offset placeholder */ 0);
      }
      // Add ADD with its PC-relative String patch.
      vixl::Label* add_label =
          codegen_->NewPcRelativeStringPatch(dex_file, string_index, adrp_label);
      {
        vixl::SingleEmissionCheckScope guard(GetVIXLAssembler());
        __ Bind(add_label);
        __ add(out.X(), out.X(), Operand(/* offset placeholder */ 0));
      }
      return;  // No dex cache slow path.
    }
    case HLoadString::LoadKind::kBootImageAddress: {
      DCHECK(!kEmitCompilerReadBarrier);
      DCHECK(load->GetAddress() != 0u && IsUint<32>(load->GetAddress()));
      __ Ldr(out.W(), codegen_->DeduplicateBootImageAddressLiteral(load->GetAddress()));
      return;  // No dex cache slow path.
    }
    case HLoadString::LoadKind::kDexCacheAddress: {
      DCHECK_NE(load->GetAddress(), 0u);
      // LDR immediate has a 12-bit offset multiplied by the size and for 32-bit loads
      // that gives a 16KiB range. To try and reduce the number of literals if we load
      // multiple strings, simply split the dex cache address to a 16KiB aligned base
      // loaded from a literal and the remaining offset embedded in the load.
      static_assert(sizeof(GcRoot<mirror::String>) == 4u, "Expected GC root to be 4 bytes.");
      DCHECK_ALIGNED(load->GetAddress(), 4u);
      constexpr size_t offset_bits = /* encoded bits */ 12 + /* scale */ 2;
      uint64_t base_address = load->GetAddress() & ~MaxInt<uint64_t>(offset_bits);
      uint32_t offset = load->GetAddress() & MaxInt<uint64_t>(offset_bits);
      __ Ldr(out.X(), codegen_->DeduplicateDexCacheAddressLiteral(base_address));
      GenerateGcRootFieldLoad(load, out_loc, out.X(), offset);
      break;
    }
    case HLoadString::LoadKind::kDexCachePcRelative: {
      // Add ADRP with its PC-relative DexCache access patch.
      const DexFile& dex_file = load->GetDexFile();
      uint32_t element_offset = load->GetDexCacheElementOffset();
      vixl::Label* adrp_label = codegen_->NewPcRelativeDexCacheArrayPatch(dex_file, element_offset);
      {
        vixl::SingleEmissionCheckScope guard(GetVIXLAssembler());
        __ Bind(adrp_label);
        __ adrp(out.X(), /* offset placeholder */ 0);
      }
      // Add LDR with its PC-relative DexCache access patch.
      vixl::Label* ldr_label =
          codegen_->NewPcRelativeDexCacheArrayPatch(dex_file, element_offset, adrp_label);
      GenerateGcRootFieldLoad(load, out_loc, out.X(), /* offset placeholder */ 0, ldr_label);
      break;
    }
    case HLoadString::LoadKind::kDexCacheViaMethod: {
      Register current_method = InputRegisterAt(load, 0);
      // /* GcRoot<mirror::Class> */ out = current_method->declaring_class_
      GenerateGcRootFieldLoad(
          load, out_loc, current_method, ArtMethod::DeclaringClassOffset().Int32Value());
      // /* GcRoot<mirror::String>[] */ out = out->dex_cache_strings_
      __ Ldr(out.X(), HeapOperand(out, mirror::Class::DexCacheStringsOffset().Uint32Value()));
      // /* GcRoot<mirror::String> */ out = out[string_index]
      GenerateGcRootFieldLoad(
          load, out_loc, out.X(), CodeGenerator::GetCacheOffset(load->GetStringIndex()));
      break;
    }
    default:
      LOG(FATAL) << "Unexpected load kind: " << load->GetLoadKind();
      UNREACHABLE();
  }

  if (!load->IsInDexCache()) {
    SlowPathCodeARM64* slow_path = new (GetGraph()->GetArena()) LoadStringSlowPathARM64(load);
    codegen_->AddSlowPath(slow_path);
    __ Cbz(out, slow_path->GetEntryLabel());
    __ Bind(slow_path->GetExitLabel());
  }
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
  if (instruction->IsStringAlloc()) {
    locations->AddTemp(LocationFrom(kArtMethodRegister));
  } else {
    locations->SetInAt(0, LocationFrom(calling_convention.GetRegisterAt(0)));
    locations->SetInAt(1, LocationFrom(calling_convention.GetRegisterAt(1)));
  }
  locations->SetOut(calling_convention.GetReturnLocation(Primitive::kPrimNot));
}

void InstructionCodeGeneratorARM64::VisitNewInstance(HNewInstance* instruction) {
  // Note: if heap poisoning is enabled, the entry point takes cares
  // of poisoning the reference.
  if (instruction->IsStringAlloc()) {
    // String is allocated through StringFactory. Call NewEmptyString entry point.
    Location temp = instruction->GetLocations()->GetTemp(0);
    MemberOffset code_offset = ArtMethod::EntryPointFromQuickCompiledCodeOffset(kArm64WordSize);
    __ Ldr(XRegisterFrom(temp), MemOperand(tr, QUICK_ENTRY_POINT(pNewEmptyString)));
    __ Ldr(lr, MemOperand(XRegisterFrom(temp), code_offset.Int32Value()));
    __ Blr(lr);
    codegen_->RecordPcInfo(instruction, instruction->GetDexPc());
  } else {
    codegen_->InvokeRuntime(instruction->GetEntrypoint(),
                            instruction,
                            instruction->GetDexPc(),
                            nullptr);
    CheckEntrypointTypes<kQuickAllocObjectWithAccessCheck, void*, uint32_t, ArtMethod*>();
  }
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

void CodeGeneratorARM64::GenerateImplicitNullCheck(HNullCheck* instruction) {
  if (CanMoveNullCheckToUser(instruction)) {
    return;
  }

  BlockPoolsScope block_pools(GetVIXLAssembler());
  Location obj = instruction->GetLocations()->InAt(0);
  __ Ldr(wzr, HeapOperandFrom(obj, Offset(0)));
  RecordPcInfo(instruction, instruction->GetDexPc());
}

void CodeGeneratorARM64::GenerateExplicitNullCheck(HNullCheck* instruction) {
  SlowPathCodeARM64* slow_path = new (GetGraph()->GetArena()) NullCheckSlowPathARM64(instruction);
  AddSlowPath(slow_path);

  LocationSummary* locations = instruction->GetLocations();
  Location obj = locations->InAt(0);

  __ Cbz(RegisterFrom(obj, instruction->InputAt(0)->GetType()), slow_path->GetEntryLabel());
}

void InstructionCodeGeneratorARM64::VisitNullCheck(HNullCheck* instruction) {
  codegen_->GenerateNullCheck(instruction);
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
      UNREACHABLE();
  }
}

void LocationsBuilderARM64::VisitMemoryBarrier(HMemoryBarrier* memory_barrier) {
  memory_barrier->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM64::VisitMemoryBarrier(HMemoryBarrier* memory_barrier) {
  codegen_->GenerateMemoryBarrier(memory_barrier->GetBarrierKind());
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

void LocationsBuilderARM64::VisitRor(HRor* ror) {
  HandleBinaryOp(ror);
}

void InstructionCodeGeneratorARM64::VisitRor(HRor* ror) {
  HandleBinaryOp(ror);
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
    if (result_type == Primitive::kPrimInt && input_type == Primitive::kPrimLong) {
      // 'int' values are used directly as W registers, discarding the top
      // bits, so we don't need to sign-extend and can just perform a move.
      // We do not pass the `kDiscardForSameWReg` argument to force clearing the
      // top 32 bits of the target register. We theoretically could leave those
      // bits unchanged, but we would have to make sure that no code uses a
      // 32bit input value as a 64bit value assuming that the top 32 bits are
      // zero.
      __ Mov(output.W(), source.W());
    } else if (result_type == Primitive::kPrimChar ||
               (input_type == Primitive::kPrimChar && input_size < result_size)) {
      __ Ubfx(output,
              output.IsX() ? source.X() : source.W(),
              0, Primitive::ComponentSize(Primitive::kPrimChar) * kBitsPerByte);
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

  if (num_entries <= kPackedSwitchCompareJumpThreshold ||
      // Current instruction id is an upper bound of the number of HIRs in the graph.
      GetGraph()->GetCurrentInstructionId() > kJumpTableInstructionThreshold) {
    // Create a series of compare/jumps.
    UseScratchRegisterScope temps(codegen_->GetVIXLAssembler());
    Register temp = temps.AcquireW();
    __ Subs(temp, value_reg, Operand(lower_bound));

    const ArenaVector<HBasicBlock*>& successors = switch_instr->GetBlock()->GetSuccessors();
    // Jump to successors[0] if value == lower_bound.
    __ B(eq, codegen_->GetLabelOf(successors[0]));
    int32_t last_index = 0;
    for (; num_entries - last_index > 2; last_index += 2) {
      __ Subs(temp, temp, Operand(2));
      // Jump to successors[last_index + 1] if value < case_value[last_index + 2].
      __ B(lo, codegen_->GetLabelOf(successors[last_index + 1]));
      // Jump to successors[last_index + 2] if value == case_value[last_index + 2].
      __ B(eq, codegen_->GetLabelOf(successors[last_index + 2]));
    }
    if (num_entries - last_index == 2) {
      // The last missing case_value.
      __ Cmp(temp, Operand(1));
      __ B(eq, codegen_->GetLabelOf(successors[last_index + 1]));
    }

    // And the default for any other value.
    if (!codegen_->GoesToNextBlock(switch_instr->GetBlock(), default_block)) {
      __ B(codegen_->GetLabelOf(default_block));
    }
  } else {
    JumpTableARM64* jump_table = codegen_->CreateJumpTable(switch_instr);

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

void InstructionCodeGeneratorARM64::GenerateReferenceLoadOneRegister(HInstruction* instruction,
                                                                     Location out,
                                                                     uint32_t offset,
                                                                     Location maybe_temp) {
  Primitive::Type type = Primitive::kPrimNot;
  Register out_reg = RegisterFrom(out, type);
  if (kEmitCompilerReadBarrier) {
    Register temp_reg = RegisterFrom(maybe_temp, type);
    if (kUseBakerReadBarrier) {
      // Load with fast path based Baker's read barrier.
      // /* HeapReference<Object> */ out = *(out + offset)
      codegen_->GenerateFieldLoadWithBakerReadBarrier(instruction,
                                                      out,
                                                      out_reg,
                                                      offset,
                                                      temp_reg,
                                                      /* needs_null_check */ false,
                                                      /* use_load_acquire */ false);
    } else {
      // Load with slow path based read barrier.
      // Save the value of `out` into `maybe_temp` before overwriting it
      // in the following move operation, as we will need it for the
      // read barrier below.
      __ Mov(temp_reg, out_reg);
      // /* HeapReference<Object> */ out = *(out + offset)
      __ Ldr(out_reg, HeapOperand(out_reg, offset));
      codegen_->GenerateReadBarrierSlow(instruction, out, out, maybe_temp, offset);
    }
  } else {
    // Plain load with no read barrier.
    // /* HeapReference<Object> */ out = *(out + offset)
    __ Ldr(out_reg, HeapOperand(out_reg, offset));
    GetAssembler()->MaybeUnpoisonHeapReference(out_reg);
  }
}

void InstructionCodeGeneratorARM64::GenerateReferenceLoadTwoRegisters(HInstruction* instruction,
                                                                      Location out,
                                                                      Location obj,
                                                                      uint32_t offset,
                                                                      Location maybe_temp) {
  Primitive::Type type = Primitive::kPrimNot;
  Register out_reg = RegisterFrom(out, type);
  Register obj_reg = RegisterFrom(obj, type);
  if (kEmitCompilerReadBarrier) {
    if (kUseBakerReadBarrier) {
      // Load with fast path based Baker's read barrier.
      Register temp_reg = RegisterFrom(maybe_temp, type);
      // /* HeapReference<Object> */ out = *(obj + offset)
      codegen_->GenerateFieldLoadWithBakerReadBarrier(instruction,
                                                      out,
                                                      obj_reg,
                                                      offset,
                                                      temp_reg,
                                                      /* needs_null_check */ false,
                                                      /* use_load_acquire */ false);
    } else {
      // Load with slow path based read barrier.
      // /* HeapReference<Object> */ out = *(obj + offset)
      __ Ldr(out_reg, HeapOperand(obj_reg, offset));
      codegen_->GenerateReadBarrierSlow(instruction, out, out, obj, offset);
    }
  } else {
    // Plain load with no read barrier.
    // /* HeapReference<Object> */ out = *(obj + offset)
    __ Ldr(out_reg, HeapOperand(obj_reg, offset));
    GetAssembler()->MaybeUnpoisonHeapReference(out_reg);
  }
}

void InstructionCodeGeneratorARM64::GenerateGcRootFieldLoad(HInstruction* instruction,
                                                            Location root,
                                                            vixl::Register obj,
                                                            uint32_t offset,
                                                            vixl::Label* fixup_label) {
  Register root_reg = RegisterFrom(root, Primitive::kPrimNot);
  if (kEmitCompilerReadBarrier) {
    if (kUseBakerReadBarrier) {
      // Fast path implementation of art::ReadBarrier::BarrierForRoot when
      // Baker's read barrier are used:
      //
      //   root = obj.field;
      //   if (Thread::Current()->GetIsGcMarking()) {
      //     root = ReadBarrier::Mark(root)
      //   }

      // /* GcRoot<mirror::Object> */ root = *(obj + offset)
      if (fixup_label == nullptr) {
        __ Ldr(root_reg, MemOperand(obj, offset));
      } else {
        vixl::SingleEmissionCheckScope guard(GetVIXLAssembler());
        __ Bind(fixup_label);
        __ ldr(root_reg, MemOperand(obj, offset));
      }
      static_assert(
          sizeof(mirror::CompressedReference<mirror::Object>) == sizeof(GcRoot<mirror::Object>),
          "art::mirror::CompressedReference<mirror::Object> and art::GcRoot<mirror::Object> "
          "have different sizes.");
      static_assert(sizeof(mirror::CompressedReference<mirror::Object>) == sizeof(int32_t),
                    "art::mirror::CompressedReference<mirror::Object> and int32_t "
                    "have different sizes.");

      // Slow path used to mark the GC root `root`.
      SlowPathCodeARM64* slow_path =
          new (GetGraph()->GetArena()) ReadBarrierMarkSlowPathARM64(instruction, root, root);
      codegen_->AddSlowPath(slow_path);

      MacroAssembler* masm = GetVIXLAssembler();
      UseScratchRegisterScope temps(masm);
      Register temp = temps.AcquireW();
      // temp = Thread::Current()->GetIsGcMarking()
      __ Ldr(temp, MemOperand(tr, Thread::IsGcMarkingOffset<kArm64WordSize>().Int32Value()));
      __ Cbnz(temp, slow_path->GetEntryLabel());
      __ Bind(slow_path->GetExitLabel());
    } else {
      // GC root loaded through a slow path for read barriers other
      // than Baker's.
      // /* GcRoot<mirror::Object>* */ root = obj + offset
      if (fixup_label == nullptr) {
        __ Add(root_reg.X(), obj.X(), offset);
      } else {
        vixl::SingleEmissionCheckScope guard(GetVIXLAssembler());
        __ Bind(fixup_label);
        __ add(root_reg.X(), obj.X(), offset);
      }
      // /* mirror::Object* */ root = root->Read()
      codegen_->GenerateReadBarrierForRootSlow(instruction, root, root);
    }
  } else {
    // Plain GC root load with no read barrier.
    // /* GcRoot<mirror::Object> */ root = *(obj + offset)
    if (fixup_label == nullptr) {
      __ Ldr(root_reg, MemOperand(obj, offset));
    } else {
      vixl::SingleEmissionCheckScope guard(GetVIXLAssembler());
      __ Bind(fixup_label);
      __ ldr(root_reg, MemOperand(obj, offset));
    }
    // Note that GC roots are not affected by heap poisoning, thus we
    // do not have to unpoison `root_reg` here.
  }
}

void CodeGeneratorARM64::GenerateFieldLoadWithBakerReadBarrier(HInstruction* instruction,
                                                               Location ref,
                                                               vixl::Register obj,
                                                               uint32_t offset,
                                                               Register temp,
                                                               bool needs_null_check,
                                                               bool use_load_acquire) {
  DCHECK(kEmitCompilerReadBarrier);
  DCHECK(kUseBakerReadBarrier);

  // /* HeapReference<Object> */ ref = *(obj + offset)
  Location no_index = Location::NoLocation();
  GenerateReferenceLoadWithBakerReadBarrier(
      instruction, ref, obj, offset, no_index, temp, needs_null_check, use_load_acquire);
}

void CodeGeneratorARM64::GenerateArrayLoadWithBakerReadBarrier(HInstruction* instruction,
                                                               Location ref,
                                                               vixl::Register obj,
                                                               uint32_t data_offset,
                                                               Location index,
                                                               Register temp,
                                                               bool needs_null_check) {
  DCHECK(kEmitCompilerReadBarrier);
  DCHECK(kUseBakerReadBarrier);

  // Array cells are never volatile variables, therefore array loads
  // never use Load-Acquire instructions on ARM64.
  const bool use_load_acquire = false;

  // /* HeapReference<Object> */ ref =
  //     *(obj + data_offset + index * sizeof(HeapReference<Object>))
  GenerateReferenceLoadWithBakerReadBarrier(
      instruction, ref, obj, data_offset, index, temp, needs_null_check, use_load_acquire);
}

void CodeGeneratorARM64::GenerateReferenceLoadWithBakerReadBarrier(HInstruction* instruction,
                                                                   Location ref,
                                                                   vixl::Register obj,
                                                                   uint32_t offset,
                                                                   Location index,
                                                                   Register temp,
                                                                   bool needs_null_check,
                                                                   bool use_load_acquire) {
  DCHECK(kEmitCompilerReadBarrier);
  DCHECK(kUseBakerReadBarrier);
  // If `index` is a valid location, then we are emitting an array
  // load, so we shouldn't be using a Load Acquire instruction.
  // In other words: `index.IsValid()` => `!use_load_acquire`.
  DCHECK(!index.IsValid() || !use_load_acquire);

  MacroAssembler* masm = GetVIXLAssembler();
  UseScratchRegisterScope temps(masm);

  // In slow path based read barriers, the read barrier call is
  // inserted after the original load. However, in fast path based
  // Baker's read barriers, we need to perform the load of
  // mirror::Object::monitor_ *before* the original reference load.
  // This load-load ordering is required by the read barrier.
  // The fast path/slow path (for Baker's algorithm) should look like:
  //
  //   uint32_t rb_state = Lockword(obj->monitor_).ReadBarrierState();
  //   lfence;  // Load fence or artificial data dependency to prevent load-load reordering
  //   HeapReference<Object> ref = *src;  // Original reference load.
  //   bool is_gray = (rb_state == ReadBarrier::gray_ptr_);
  //   if (is_gray) {
  //     ref = ReadBarrier::Mark(ref);  // Performed by runtime entrypoint slow path.
  //   }
  //
  // Note: the original implementation in ReadBarrier::Barrier is
  // slightly more complex as it performs additional checks that we do
  // not do here for performance reasons.

  Primitive::Type type = Primitive::kPrimNot;
  Register ref_reg = RegisterFrom(ref, type);
  DCHECK(obj.IsW());
  uint32_t monitor_offset = mirror::Object::MonitorOffset().Int32Value();

  // /* int32_t */ monitor = obj->monitor_
  __ Ldr(temp, HeapOperand(obj, monitor_offset));
  if (needs_null_check) {
    MaybeRecordImplicitNullCheck(instruction);
  }
  // /* LockWord */ lock_word = LockWord(monitor)
  static_assert(sizeof(LockWord) == sizeof(int32_t),
                "art::LockWord and int32_t have different sizes.");
  // /* uint32_t */ rb_state = lock_word.ReadBarrierState()
  __ Lsr(temp, temp, LockWord::kReadBarrierStateShift);
  __ And(temp, temp, Operand(LockWord::kReadBarrierStateMask));
  static_assert(
      LockWord::kReadBarrierStateMask == ReadBarrier::rb_ptr_mask_,
      "art::LockWord::kReadBarrierStateMask is not equal to art::ReadBarrier::rb_ptr_mask_.");

  // Introduce a dependency on the high bits of rb_state, which shall
  // be all zeroes, to prevent load-load reordering, and without using
  // a memory barrier (which would be more expensive).
  // temp2 = rb_state & ~LockWord::kReadBarrierStateMask = 0
  Register temp2 = temps.AcquireW();
  __ Bic(temp2, temp, Operand(LockWord::kReadBarrierStateMask));
  // obj is unchanged by this operation, but its value now depends on
  // temp2, which depends on temp.
  __ Add(obj, obj, Operand(temp2));
  temps.Release(temp2);

  // The actual reference load.
  if (index.IsValid()) {
    static_assert(
        sizeof(mirror::HeapReference<mirror::Object>) == sizeof(int32_t),
        "art::mirror::HeapReference<art::mirror::Object> and int32_t have different sizes.");
    // /* HeapReference<Object> */ ref =
    //     *(obj + offset + index * sizeof(HeapReference<Object>))
    const size_t shift_amount = Primitive::ComponentSizeShift(type);
    if (index.IsConstant()) {
      uint32_t computed_offset = offset + (Int64ConstantFrom(index) << shift_amount);
      Load(type, ref_reg, HeapOperand(obj, computed_offset));
    } else {
      temp2 = temps.AcquireW();
      __ Add(temp2, obj, offset);
      Load(type, ref_reg, HeapOperand(temp2, XRegisterFrom(index), LSL, shift_amount));
      temps.Release(temp2);
    }
  } else {
    // /* HeapReference<Object> */ ref = *(obj + offset)
    MemOperand field = HeapOperand(obj, offset);
    if (use_load_acquire) {
      LoadAcquire(instruction, ref_reg, field, /* needs_null_check */ false);
    } else {
      Load(type, ref_reg, field);
    }
  }

  // Object* ref = ref_addr->AsMirrorPtr()
  GetAssembler()->MaybeUnpoisonHeapReference(ref_reg);

  // Slow path used to mark the object `ref` when it is gray.
  SlowPathCodeARM64* slow_path =
      new (GetGraph()->GetArena()) ReadBarrierMarkSlowPathARM64(instruction, ref, ref);
  AddSlowPath(slow_path);

  // if (rb_state == ReadBarrier::gray_ptr_)
  //   ref = ReadBarrier::Mark(ref);
  __ Cmp(temp, ReadBarrier::gray_ptr_);
  __ B(eq, slow_path->GetEntryLabel());
  __ Bind(slow_path->GetExitLabel());
}

void CodeGeneratorARM64::GenerateReadBarrierSlow(HInstruction* instruction,
                                                 Location out,
                                                 Location ref,
                                                 Location obj,
                                                 uint32_t offset,
                                                 Location index) {
  DCHECK(kEmitCompilerReadBarrier);

  // Insert a slow path based read barrier *after* the reference load.
  //
  // If heap poisoning is enabled, the unpoisoning of the loaded
  // reference will be carried out by the runtime within the slow
  // path.
  //
  // Note that `ref` currently does not get unpoisoned (when heap
  // poisoning is enabled), which is alright as the `ref` argument is
  // not used by the artReadBarrierSlow entry point.
  //
  // TODO: Unpoison `ref` when it is used by artReadBarrierSlow.
  SlowPathCodeARM64* slow_path = new (GetGraph()->GetArena())
      ReadBarrierForHeapReferenceSlowPathARM64(instruction, out, ref, obj, offset, index);
  AddSlowPath(slow_path);

  __ B(slow_path->GetEntryLabel());
  __ Bind(slow_path->GetExitLabel());
}

void CodeGeneratorARM64::MaybeGenerateReadBarrierSlow(HInstruction* instruction,
                                                      Location out,
                                                      Location ref,
                                                      Location obj,
                                                      uint32_t offset,
                                                      Location index) {
  if (kEmitCompilerReadBarrier) {
    // Baker's read barriers shall be handled by the fast path
    // (CodeGeneratorARM64::GenerateReferenceLoadWithBakerReadBarrier).
    DCHECK(!kUseBakerReadBarrier);
    // If heap poisoning is enabled, unpoisoning will be taken care of
    // by the runtime within the slow path.
    GenerateReadBarrierSlow(instruction, out, ref, obj, offset, index);
  } else if (kPoisonHeapReferences) {
    GetAssembler()->UnpoisonHeapReference(WRegisterFrom(out));
  }
}

void CodeGeneratorARM64::GenerateReadBarrierForRootSlow(HInstruction* instruction,
                                                        Location out,
                                                        Location root) {
  DCHECK(kEmitCompilerReadBarrier);

  // Insert a slow path based read barrier *after* the GC root load.
  //
  // Note that GC roots are not affected by heap poisoning, so we do
  // not need to do anything special for this here.
  SlowPathCodeARM64* slow_path =
      new (GetGraph()->GetArena()) ReadBarrierForRootSlowPathARM64(instruction, out, root);
  AddSlowPath(slow_path);

  __ B(slow_path->GetEntryLabel());
  __ Bind(slow_path->GetExitLabel());
}

void LocationsBuilderARM64::VisitClassTableGet(HClassTableGet* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorARM64::VisitClassTableGet(HClassTableGet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  uint32_t method_offset = 0;
  if (instruction->GetTableKind() == HClassTableGet::TableKind::kVTable) {
    method_offset = mirror::Class::EmbeddedVTableEntryOffset(
        instruction->GetIndex(), kArm64PointerSize).SizeValue();
  } else {
    __ Ldr(XRegisterFrom(locations->Out()), MemOperand(XRegisterFrom(locations->InAt(0)),
        mirror::Class::ImtPtrOffset(kArm64PointerSize).Uint32Value()));
    method_offset = static_cast<uint32_t>(ImTable::OffsetOfElement(
        instruction->GetIndex() % ImTable::kSize, kArm64PointerSize));
  }
  __ Ldr(XRegisterFrom(locations->Out()),
         MemOperand(XRegisterFrom(locations->InAt(0)), method_offset));
}



#undef __
#undef QUICK_ENTRY_POINT

}  // namespace arm64
}  // namespace art
