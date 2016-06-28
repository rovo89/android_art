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

#include "code_generator_x86_64.h"

#include "art_method.h"
#include "code_generator_utils.h"
#include "compiled_method.h"
#include "entrypoints/quick/quick_entrypoints.h"
#include "gc/accounting/card_table.h"
#include "intrinsics.h"
#include "intrinsics_x86_64.h"
#include "mirror/array-inl.h"
#include "mirror/class-inl.h"
#include "mirror/object_reference.h"
#include "thread.h"
#include "utils/assembler.h"
#include "utils/stack_checks.h"
#include "utils/x86_64/assembler_x86_64.h"
#include "utils/x86_64/managed_register_x86_64.h"

namespace art {

template<class MirrorType>
class GcRoot;

namespace x86_64 {

static constexpr int kCurrentMethodStackOffset = 0;
static constexpr Register kMethodRegisterArgument = RDI;
// The compare/jump sequence will generate about (1.5 * num_entries) instructions. A jump
// table version generates 7 instructions and num_entries literals. Compare/jump sequence will
// generates less code/data with a small num_entries.
static constexpr uint32_t kPackedSwitchJumpTableThreshold = 5;

static constexpr Register kCoreCalleeSaves[] = { RBX, RBP, R12, R13, R14, R15 };
static constexpr FloatRegister kFpuCalleeSaves[] = { XMM12, XMM13, XMM14, XMM15 };

static constexpr int kC2ConditionMask = 0x400;

#define __ down_cast<X86_64Assembler*>(codegen->GetAssembler())->
#define QUICK_ENTRY_POINT(x) QUICK_ENTRYPOINT_OFFSET(kX86_64WordSize, x).Int32Value()

class NullCheckSlowPathX86_64 : public SlowPathCode {
 public:
  explicit NullCheckSlowPathX86_64(HNullCheck* instruction) : SlowPathCode(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    CodeGeneratorX86_64* x86_64_codegen = down_cast<CodeGeneratorX86_64*>(codegen);
    __ Bind(GetEntryLabel());
    if (instruction_->CanThrowIntoCatchBlock()) {
      // Live registers will be restored in the catch block if caught.
      SaveLiveRegisters(codegen, instruction_->GetLocations());
    }
    x86_64_codegen->InvokeRuntime(QUICK_ENTRY_POINT(pThrowNullPointer),
                                  instruction_,
                                  instruction_->GetDexPc(),
                                  this);
    CheckEntrypointTypes<kQuickThrowNullPointer, void, void>();
  }

  bool IsFatal() const OVERRIDE { return true; }

  const char* GetDescription() const OVERRIDE { return "NullCheckSlowPathX86_64"; }

 private:
  DISALLOW_COPY_AND_ASSIGN(NullCheckSlowPathX86_64);
};

class DivZeroCheckSlowPathX86_64 : public SlowPathCode {
 public:
  explicit DivZeroCheckSlowPathX86_64(HDivZeroCheck* instruction) : SlowPathCode(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    CodeGeneratorX86_64* x86_64_codegen = down_cast<CodeGeneratorX86_64*>(codegen);
    __ Bind(GetEntryLabel());
    if (instruction_->CanThrowIntoCatchBlock()) {
      // Live registers will be restored in the catch block if caught.
      SaveLiveRegisters(codegen, instruction_->GetLocations());
    }
    x86_64_codegen->InvokeRuntime(QUICK_ENTRY_POINT(pThrowDivZero),
                                  instruction_,
                                  instruction_->GetDexPc(),
                                  this);
    CheckEntrypointTypes<kQuickThrowDivZero, void, void>();
  }

  bool IsFatal() const OVERRIDE { return true; }

  const char* GetDescription() const OVERRIDE { return "DivZeroCheckSlowPathX86_64"; }

 private:
  DISALLOW_COPY_AND_ASSIGN(DivZeroCheckSlowPathX86_64);
};

class DivRemMinusOneSlowPathX86_64 : public SlowPathCode {
 public:
  DivRemMinusOneSlowPathX86_64(HInstruction* at, Register reg, Primitive::Type type, bool is_div)
      : SlowPathCode(at), cpu_reg_(CpuRegister(reg)), type_(type), is_div_(is_div) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    __ Bind(GetEntryLabel());
    if (type_ == Primitive::kPrimInt) {
      if (is_div_) {
        __ negl(cpu_reg_);
      } else {
        __ xorl(cpu_reg_, cpu_reg_);
      }

    } else {
      DCHECK_EQ(Primitive::kPrimLong, type_);
      if (is_div_) {
        __ negq(cpu_reg_);
      } else {
        __ xorl(cpu_reg_, cpu_reg_);
      }
    }
    __ jmp(GetExitLabel());
  }

  const char* GetDescription() const OVERRIDE { return "DivRemMinusOneSlowPathX86_64"; }

 private:
  const CpuRegister cpu_reg_;
  const Primitive::Type type_;
  const bool is_div_;
  DISALLOW_COPY_AND_ASSIGN(DivRemMinusOneSlowPathX86_64);
};

class SuspendCheckSlowPathX86_64 : public SlowPathCode {
 public:
  SuspendCheckSlowPathX86_64(HSuspendCheck* instruction, HBasicBlock* successor)
      : SlowPathCode(instruction), successor_(successor) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    CodeGeneratorX86_64* x86_64_codegen = down_cast<CodeGeneratorX86_64*>(codegen);
    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, instruction_->GetLocations());
    x86_64_codegen->InvokeRuntime(QUICK_ENTRY_POINT(pTestSuspend),
                                  instruction_,
                                  instruction_->GetDexPc(),
                                  this);
    CheckEntrypointTypes<kQuickTestSuspend, void, void>();
    RestoreLiveRegisters(codegen, instruction_->GetLocations());
    if (successor_ == nullptr) {
      __ jmp(GetReturnLabel());
    } else {
      __ jmp(x86_64_codegen->GetLabelOf(successor_));
    }
  }

  Label* GetReturnLabel() {
    DCHECK(successor_ == nullptr);
    return &return_label_;
  }

  HBasicBlock* GetSuccessor() const {
    return successor_;
  }

  const char* GetDescription() const OVERRIDE { return "SuspendCheckSlowPathX86_64"; }

 private:
  HBasicBlock* const successor_;
  Label return_label_;

  DISALLOW_COPY_AND_ASSIGN(SuspendCheckSlowPathX86_64);
};

class BoundsCheckSlowPathX86_64 : public SlowPathCode {
 public:
  explicit BoundsCheckSlowPathX86_64(HBoundsCheck* instruction)
    : SlowPathCode(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    LocationSummary* locations = instruction_->GetLocations();
    CodeGeneratorX86_64* x86_64_codegen = down_cast<CodeGeneratorX86_64*>(codegen);
    __ Bind(GetEntryLabel());
    if (instruction_->CanThrowIntoCatchBlock()) {
      // Live registers will be restored in the catch block if caught.
      SaveLiveRegisters(codegen, instruction_->GetLocations());
    }
    // We're moving two locations to locations that could overlap, so we need a parallel
    // move resolver.
    InvokeRuntimeCallingConvention calling_convention;
    codegen->EmitParallelMoves(
        locations->InAt(0),
        Location::RegisterLocation(calling_convention.GetRegisterAt(0)),
        Primitive::kPrimInt,
        locations->InAt(1),
        Location::RegisterLocation(calling_convention.GetRegisterAt(1)),
        Primitive::kPrimInt);
    x86_64_codegen->InvokeRuntime(QUICK_ENTRY_POINT(pThrowArrayBounds),
                                  instruction_,
                                  instruction_->GetDexPc(),
                                  this);
    CheckEntrypointTypes<kQuickThrowArrayBounds, void, int32_t, int32_t>();
  }

  bool IsFatal() const OVERRIDE { return true; }

  const char* GetDescription() const OVERRIDE { return "BoundsCheckSlowPathX86_64"; }

 private:
  DISALLOW_COPY_AND_ASSIGN(BoundsCheckSlowPathX86_64);
};

class LoadClassSlowPathX86_64 : public SlowPathCode {
 public:
  LoadClassSlowPathX86_64(HLoadClass* cls,
                          HInstruction* at,
                          uint32_t dex_pc,
                          bool do_clinit)
      : SlowPathCode(at), cls_(cls), at_(at), dex_pc_(dex_pc), do_clinit_(do_clinit) {
    DCHECK(at->IsLoadClass() || at->IsClinitCheck());
  }

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    LocationSummary* locations = at_->GetLocations();
    CodeGeneratorX86_64* x86_64_codegen = down_cast<CodeGeneratorX86_64*>(codegen);
    __ Bind(GetEntryLabel());

    SaveLiveRegisters(codegen, locations);

    InvokeRuntimeCallingConvention calling_convention;
    __ movl(CpuRegister(calling_convention.GetRegisterAt(0)), Immediate(cls_->GetTypeIndex()));
    x86_64_codegen->InvokeRuntime(do_clinit_ ?
                                      QUICK_ENTRY_POINT(pInitializeStaticStorage) :
                                      QUICK_ENTRY_POINT(pInitializeType),
                                  at_,
                                  dex_pc_,
                                  this);
    if (do_clinit_) {
      CheckEntrypointTypes<kQuickInitializeStaticStorage, void*, uint32_t>();
    } else {
      CheckEntrypointTypes<kQuickInitializeType, void*, uint32_t>();
    }

    Location out = locations->Out();
    // Move the class to the desired location.
    if (out.IsValid()) {
      DCHECK(out.IsRegister() && !locations->GetLiveRegisters()->ContainsCoreRegister(out.reg()));
      x86_64_codegen->Move(out, Location::RegisterLocation(RAX));
    }

    RestoreLiveRegisters(codegen, locations);
    __ jmp(GetExitLabel());
  }

  const char* GetDescription() const OVERRIDE { return "LoadClassSlowPathX86_64"; }

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

  DISALLOW_COPY_AND_ASSIGN(LoadClassSlowPathX86_64);
};

class LoadStringSlowPathX86_64 : public SlowPathCode {
 public:
  explicit LoadStringSlowPathX86_64(HLoadString* instruction) : SlowPathCode(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    LocationSummary* locations = instruction_->GetLocations();
    DCHECK(!locations->GetLiveRegisters()->ContainsCoreRegister(locations->Out().reg()));

    CodeGeneratorX86_64* x86_64_codegen = down_cast<CodeGeneratorX86_64*>(codegen);
    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, locations);

    InvokeRuntimeCallingConvention calling_convention;
    const uint32_t string_index = instruction_->AsLoadString()->GetStringIndex();
    __ movl(CpuRegister(calling_convention.GetRegisterAt(0)), Immediate(string_index));
    x86_64_codegen->InvokeRuntime(QUICK_ENTRY_POINT(pResolveString),
                                  instruction_,
                                  instruction_->GetDexPc(),
                                  this);
    CheckEntrypointTypes<kQuickResolveString, void*, uint32_t>();
    x86_64_codegen->Move(locations->Out(), Location::RegisterLocation(RAX));
    RestoreLiveRegisters(codegen, locations);
    __ jmp(GetExitLabel());
  }

  const char* GetDescription() const OVERRIDE { return "LoadStringSlowPathX86_64"; }

 private:
  DISALLOW_COPY_AND_ASSIGN(LoadStringSlowPathX86_64);
};

class TypeCheckSlowPathX86_64 : public SlowPathCode {
 public:
  TypeCheckSlowPathX86_64(HInstruction* instruction, bool is_fatal)
      : SlowPathCode(instruction), is_fatal_(is_fatal) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    LocationSummary* locations = instruction_->GetLocations();
    Location object_class = instruction_->IsCheckCast() ? locations->GetTemp(0)
                                                        : locations->Out();
    uint32_t dex_pc = instruction_->GetDexPc();
    DCHECK(instruction_->IsCheckCast()
           || !locations->GetLiveRegisters()->ContainsCoreRegister(locations->Out().reg()));

    CodeGeneratorX86_64* x86_64_codegen = down_cast<CodeGeneratorX86_64*>(codegen);
    __ Bind(GetEntryLabel());

    if (!is_fatal_) {
      SaveLiveRegisters(codegen, locations);
    }

    // We're moving two locations to locations that could overlap, so we need a parallel
    // move resolver.
    InvokeRuntimeCallingConvention calling_convention;
    codegen->EmitParallelMoves(
        locations->InAt(1),
        Location::RegisterLocation(calling_convention.GetRegisterAt(0)),
        Primitive::kPrimNot,
        object_class,
        Location::RegisterLocation(calling_convention.GetRegisterAt(1)),
        Primitive::kPrimNot);

    if (instruction_->IsInstanceOf()) {
      x86_64_codegen->InvokeRuntime(QUICK_ENTRY_POINT(pInstanceofNonTrivial),
                                    instruction_,
                                    dex_pc,
                                    this);
      CheckEntrypointTypes<
          kQuickInstanceofNonTrivial, uint32_t, const mirror::Class*, const mirror::Class*>();
    } else {
      DCHECK(instruction_->IsCheckCast());
      x86_64_codegen->InvokeRuntime(QUICK_ENTRY_POINT(pCheckCast),
                                    instruction_,
                                    dex_pc,
                                    this);
      CheckEntrypointTypes<kQuickCheckCast, void, const mirror::Class*, const mirror::Class*>();
    }

    if (!is_fatal_) {
      if (instruction_->IsInstanceOf()) {
        x86_64_codegen->Move(locations->Out(), Location::RegisterLocation(RAX));
      }

      RestoreLiveRegisters(codegen, locations);
      __ jmp(GetExitLabel());
    }
  }

  const char* GetDescription() const OVERRIDE { return "TypeCheckSlowPathX86_64"; }

  bool IsFatal() const OVERRIDE { return is_fatal_; }

 private:
  const bool is_fatal_;

  DISALLOW_COPY_AND_ASSIGN(TypeCheckSlowPathX86_64);
};

class DeoptimizationSlowPathX86_64 : public SlowPathCode {
 public:
  explicit DeoptimizationSlowPathX86_64(HDeoptimize* instruction)
      : SlowPathCode(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    CodeGeneratorX86_64* x86_64_codegen = down_cast<CodeGeneratorX86_64*>(codegen);
    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, instruction_->GetLocations());
    x86_64_codegen->InvokeRuntime(QUICK_ENTRY_POINT(pDeoptimize),
                                  instruction_,
                                  instruction_->GetDexPc(),
                                  this);
    CheckEntrypointTypes<kQuickDeoptimize, void, void>();
  }

  const char* GetDescription() const OVERRIDE { return "DeoptimizationSlowPathX86_64"; }

 private:
  DISALLOW_COPY_AND_ASSIGN(DeoptimizationSlowPathX86_64);
};

class ArraySetSlowPathX86_64 : public SlowPathCode {
 public:
  explicit ArraySetSlowPathX86_64(HInstruction* instruction) : SlowPathCode(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    LocationSummary* locations = instruction_->GetLocations();
    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, locations);

    InvokeRuntimeCallingConvention calling_convention;
    HParallelMove parallel_move(codegen->GetGraph()->GetArena());
    parallel_move.AddMove(
        locations->InAt(0),
        Location::RegisterLocation(calling_convention.GetRegisterAt(0)),
        Primitive::kPrimNot,
        nullptr);
    parallel_move.AddMove(
        locations->InAt(1),
        Location::RegisterLocation(calling_convention.GetRegisterAt(1)),
        Primitive::kPrimInt,
        nullptr);
    parallel_move.AddMove(
        locations->InAt(2),
        Location::RegisterLocation(calling_convention.GetRegisterAt(2)),
        Primitive::kPrimNot,
        nullptr);
    codegen->GetMoveResolver()->EmitNativeCode(&parallel_move);

    CodeGeneratorX86_64* x86_64_codegen = down_cast<CodeGeneratorX86_64*>(codegen);
    x86_64_codegen->InvokeRuntime(QUICK_ENTRY_POINT(pAputObject),
                                  instruction_,
                                  instruction_->GetDexPc(),
                                  this);
    CheckEntrypointTypes<kQuickAputObject, void, mirror::Array*, int32_t, mirror::Object*>();
    RestoreLiveRegisters(codegen, locations);
    __ jmp(GetExitLabel());
  }

  const char* GetDescription() const OVERRIDE { return "ArraySetSlowPathX86_64"; }

 private:
  DISALLOW_COPY_AND_ASSIGN(ArraySetSlowPathX86_64);
};

// Slow path marking an object during a read barrier.
class ReadBarrierMarkSlowPathX86_64 : public SlowPathCode {
 public:
  ReadBarrierMarkSlowPathX86_64(HInstruction* instruction, Location out, Location obj)
      : SlowPathCode(instruction), out_(out), obj_(obj) {
    DCHECK(kEmitCompilerReadBarrier);
  }

  const char* GetDescription() const OVERRIDE { return "ReadBarrierMarkSlowPathX86_64"; }

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    LocationSummary* locations = instruction_->GetLocations();
    Register reg_out = out_.AsRegister<Register>();
    DCHECK(locations->CanCall());
    DCHECK(!locations->GetLiveRegisters()->ContainsCoreRegister(reg_out));
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
    CodeGeneratorX86_64* x86_64_codegen = down_cast<CodeGeneratorX86_64*>(codegen);
    x86_64_codegen->Move(Location::RegisterLocation(calling_convention.GetRegisterAt(0)), obj_);
    x86_64_codegen->InvokeRuntime(QUICK_ENTRY_POINT(pReadBarrierMark),
                               instruction_,
                               instruction_->GetDexPc(),
                               this);
    CheckEntrypointTypes<kQuickReadBarrierMark, mirror::Object*, mirror::Object*>();
    x86_64_codegen->Move(out_, Location::RegisterLocation(RAX));

    RestoreLiveRegisters(codegen, locations);
    __ jmp(GetExitLabel());
  }

 private:
  const Location out_;
  const Location obj_;

  DISALLOW_COPY_AND_ASSIGN(ReadBarrierMarkSlowPathX86_64);
};

// Slow path generating a read barrier for a heap reference.
class ReadBarrierForHeapReferenceSlowPathX86_64 : public SlowPathCode {
 public:
  ReadBarrierForHeapReferenceSlowPathX86_64(HInstruction* instruction,
                                            Location out,
                                            Location ref,
                                            Location obj,
                                            uint32_t offset,
                                            Location index)
      : SlowPathCode(instruction),
        out_(out),
        ref_(ref),
        obj_(obj),
        offset_(offset),
        index_(index) {
    DCHECK(kEmitCompilerReadBarrier);
    // If `obj` is equal to `out` or `ref`, it means the initial
    // object has been overwritten by (or after) the heap object
    // reference load to be instrumented, e.g.:
    //
    //   __ movl(out, Address(out, offset));
    //   codegen_->GenerateReadBarrierSlow(instruction, out_loc, out_loc, out_loc, offset);
    //
    // In that case, we have lost the information about the original
    // object, and the emitted read barrier cannot work properly.
    DCHECK(!obj.Equals(out)) << "obj=" << obj << " out=" << out;
    DCHECK(!obj.Equals(ref)) << "obj=" << obj << " ref=" << ref;
}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    CodeGeneratorX86_64* x86_64_codegen = down_cast<CodeGeneratorX86_64*>(codegen);
    LocationSummary* locations = instruction_->GetLocations();
    CpuRegister reg_out = out_.AsRegister<CpuRegister>();
    DCHECK(locations->CanCall());
    DCHECK(!locations->GetLiveRegisters()->ContainsCoreRegister(reg_out.AsRegister())) << out_;
    DCHECK(!instruction_->IsInvoke() ||
           (instruction_->IsInvokeStaticOrDirect() &&
            instruction_->GetLocations()->Intrinsified()))
        << "Unexpected instruction in read barrier for heap reference slow path: "
        << instruction_->DebugName();

    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, locations);

    // We may have to change the index's value, but as `index_` is a
    // constant member (like other "inputs" of this slow path),
    // introduce a copy of it, `index`.
    Location index = index_;
    if (index_.IsValid()) {
      // Handle `index_` for HArrayGet and intrinsic UnsafeGetObject.
      if (instruction_->IsArrayGet()) {
        // Compute real offset and store it in index_.
        Register index_reg = index_.AsRegister<CpuRegister>().AsRegister();
        DCHECK(locations->GetLiveRegisters()->ContainsCoreRegister(index_reg));
        if (codegen->IsCoreCalleeSaveRegister(index_reg)) {
          // We are about to change the value of `index_reg` (see the
          // calls to art::x86_64::X86_64Assembler::shll and
          // art::x86_64::X86_64Assembler::AddImmediate below), but it
          // has not been saved by the previous call to
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
          Register free_reg = FindAvailableCallerSaveRegister(codegen).AsRegister();
          __ movl(CpuRegister(free_reg), CpuRegister(index_reg));
          index_reg = free_reg;
          index = Location::RegisterLocation(index_reg);
        } else {
          // The initial register stored in `index_` has already been
          // saved in the call to art::SlowPathCode::SaveLiveRegisters
          // (as it is not a callee-save register), so we can freely
          // use it.
        }
        // Shifting the index value contained in `index_reg` by the
        // scale factor (2) cannot overflow in practice, as the
        // runtime is unable to allocate object arrays with a size
        // larger than 2^26 - 1 (that is, 2^28 - 4 bytes).
        __ shll(CpuRegister(index_reg), Immediate(TIMES_4));
        static_assert(
            sizeof(mirror::HeapReference<mirror::Object>) == sizeof(int32_t),
            "art::mirror::HeapReference<art::mirror::Object> and int32_t have different sizes.");
        __ AddImmediate(CpuRegister(index_reg), Immediate(offset_));
      } else {
        DCHECK(instruction_->IsInvoke());
        DCHECK(instruction_->GetLocations()->Intrinsified());
        DCHECK((instruction_->AsInvoke()->GetIntrinsic() == Intrinsics::kUnsafeGetObject) ||
               (instruction_->AsInvoke()->GetIntrinsic() == Intrinsics::kUnsafeGetObjectVolatile))
            << instruction_->AsInvoke()->GetIntrinsic();
        DCHECK_EQ(offset_, 0U);
        DCHECK(index_.IsRegister());
      }
    }

    // We're moving two or three locations to locations that could
    // overlap, so we need a parallel move resolver.
    InvokeRuntimeCallingConvention calling_convention;
    HParallelMove parallel_move(codegen->GetGraph()->GetArena());
    parallel_move.AddMove(ref_,
                          Location::RegisterLocation(calling_convention.GetRegisterAt(0)),
                          Primitive::kPrimNot,
                          nullptr);
    parallel_move.AddMove(obj_,
                          Location::RegisterLocation(calling_convention.GetRegisterAt(1)),
                          Primitive::kPrimNot,
                          nullptr);
    if (index.IsValid()) {
      parallel_move.AddMove(index,
                            Location::RegisterLocation(calling_convention.GetRegisterAt(2)),
                            Primitive::kPrimInt,
                            nullptr);
      codegen->GetMoveResolver()->EmitNativeCode(&parallel_move);
    } else {
      codegen->GetMoveResolver()->EmitNativeCode(&parallel_move);
      __ movl(CpuRegister(calling_convention.GetRegisterAt(2)), Immediate(offset_));
    }
    x86_64_codegen->InvokeRuntime(QUICK_ENTRY_POINT(pReadBarrierSlow),
                                  instruction_,
                                  instruction_->GetDexPc(),
                                  this);
    CheckEntrypointTypes<
        kQuickReadBarrierSlow, mirror::Object*, mirror::Object*, mirror::Object*, uint32_t>();
    x86_64_codegen->Move(out_, Location::RegisterLocation(RAX));

    RestoreLiveRegisters(codegen, locations);
    __ jmp(GetExitLabel());
  }

  const char* GetDescription() const OVERRIDE {
    return "ReadBarrierForHeapReferenceSlowPathX86_64";
  }

 private:
  CpuRegister FindAvailableCallerSaveRegister(CodeGenerator* codegen) {
    size_t ref = static_cast<int>(ref_.AsRegister<CpuRegister>().AsRegister());
    size_t obj = static_cast<int>(obj_.AsRegister<CpuRegister>().AsRegister());
    for (size_t i = 0, e = codegen->GetNumberOfCoreRegisters(); i < e; ++i) {
      if (i != ref && i != obj && !codegen->IsCoreCalleeSaveRegister(i)) {
        return static_cast<CpuRegister>(i);
      }
    }
    // We shall never fail to find a free caller-save register, as
    // there are more than two core caller-save registers on x86-64
    // (meaning it is possible to find one which is different from
    // `ref` and `obj`).
    DCHECK_GT(codegen->GetNumberOfCoreCallerSaveRegisters(), 2u);
    LOG(FATAL) << "Could not find a free caller-save register";
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

  DISALLOW_COPY_AND_ASSIGN(ReadBarrierForHeapReferenceSlowPathX86_64);
};

// Slow path generating a read barrier for a GC root.
class ReadBarrierForRootSlowPathX86_64 : public SlowPathCode {
 public:
  ReadBarrierForRootSlowPathX86_64(HInstruction* instruction, Location out, Location root)
      : SlowPathCode(instruction), out_(out), root_(root) {
    DCHECK(kEmitCompilerReadBarrier);
  }

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    LocationSummary* locations = instruction_->GetLocations();
    DCHECK(locations->CanCall());
    DCHECK(!locations->GetLiveRegisters()->ContainsCoreRegister(out_.reg()));
    DCHECK(instruction_->IsLoadClass() || instruction_->IsLoadString())
        << "Unexpected instruction in read barrier for GC root slow path: "
        << instruction_->DebugName();

    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, locations);

    InvokeRuntimeCallingConvention calling_convention;
    CodeGeneratorX86_64* x86_64_codegen = down_cast<CodeGeneratorX86_64*>(codegen);
    x86_64_codegen->Move(Location::RegisterLocation(calling_convention.GetRegisterAt(0)), root_);
    x86_64_codegen->InvokeRuntime(QUICK_ENTRY_POINT(pReadBarrierForRootSlow),
                                  instruction_,
                                  instruction_->GetDexPc(),
                                  this);
    CheckEntrypointTypes<kQuickReadBarrierForRootSlow, mirror::Object*, GcRoot<mirror::Object>*>();
    x86_64_codegen->Move(out_, Location::RegisterLocation(RAX));

    RestoreLiveRegisters(codegen, locations);
    __ jmp(GetExitLabel());
  }

  const char* GetDescription() const OVERRIDE { return "ReadBarrierForRootSlowPathX86_64"; }

 private:
  const Location out_;
  const Location root_;

  DISALLOW_COPY_AND_ASSIGN(ReadBarrierForRootSlowPathX86_64);
};

#undef __
#define __ down_cast<X86_64Assembler*>(GetAssembler())->

inline Condition X86_64IntegerCondition(IfCondition cond) {
  switch (cond) {
    case kCondEQ: return kEqual;
    case kCondNE: return kNotEqual;
    case kCondLT: return kLess;
    case kCondLE: return kLessEqual;
    case kCondGT: return kGreater;
    case kCondGE: return kGreaterEqual;
    case kCondB:  return kBelow;
    case kCondBE: return kBelowEqual;
    case kCondA:  return kAbove;
    case kCondAE: return kAboveEqual;
  }
  LOG(FATAL) << "Unreachable";
  UNREACHABLE();
}

// Maps FP condition to x86_64 name.
inline Condition X86_64FPCondition(IfCondition cond) {
  switch (cond) {
    case kCondEQ: return kEqual;
    case kCondNE: return kNotEqual;
    case kCondLT: return kBelow;
    case kCondLE: return kBelowEqual;
    case kCondGT: return kAbove;
    case kCondGE: return kAboveEqual;
    default:      break;  // should not happen
  };
  LOG(FATAL) << "Unreachable";
  UNREACHABLE();
}

HInvokeStaticOrDirect::DispatchInfo CodeGeneratorX86_64::GetSupportedInvokeStaticOrDirectDispatch(
      const HInvokeStaticOrDirect::DispatchInfo& desired_dispatch_info,
      MethodReference target_method ATTRIBUTE_UNUSED) {
  switch (desired_dispatch_info.code_ptr_location) {
    case HInvokeStaticOrDirect::CodePtrLocation::kCallDirectWithFixup:
    case HInvokeStaticOrDirect::CodePtrLocation::kCallDirect:
      // For direct code, we actually prefer to call via the code pointer from ArtMethod*.
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

void CodeGeneratorX86_64::GenerateStaticOrDirectCall(HInvokeStaticOrDirect* invoke,
                                                     Location temp) {
  // All registers are assumed to be correctly set up.

  Location callee_method = temp;  // For all kinds except kRecursive, callee will be in temp.
  switch (invoke->GetMethodLoadKind()) {
    case HInvokeStaticOrDirect::MethodLoadKind::kStringInit:
      // temp = thread->string_init_entrypoint
      __ gs()->movq(temp.AsRegister<CpuRegister>(),
                    Address::Absolute(invoke->GetStringInitOffset(), /* no_rip */ true));
      break;
    case HInvokeStaticOrDirect::MethodLoadKind::kRecursive:
      callee_method = invoke->GetLocations()->InAt(invoke->GetSpecialInputIndex());
      break;
    case HInvokeStaticOrDirect::MethodLoadKind::kDirectAddress:
      __ movq(temp.AsRegister<CpuRegister>(), Immediate(invoke->GetMethodAddress()));
      break;
    case HInvokeStaticOrDirect::MethodLoadKind::kDirectAddressWithFixup:
      __ movl(temp.AsRegister<CpuRegister>(), Immediate(0));  // Placeholder.
      method_patches_.emplace_back(invoke->GetTargetMethod());
      __ Bind(&method_patches_.back().label);  // Bind the label at the end of the "movl" insn.
      break;
    case HInvokeStaticOrDirect::MethodLoadKind::kDexCachePcRelative: {
      __ movq(temp.AsRegister<CpuRegister>(),
              Address::Absolute(kDummy32BitOffset, /* no_rip */ false));
      // Bind a new fixup label at the end of the "movl" insn.
      uint32_t offset = invoke->GetDexCacheArrayOffset();
      __ Bind(NewPcRelativeDexCacheArrayPatch(*invoke->GetTargetMethod().dex_file, offset));
      break;
    }
    case HInvokeStaticOrDirect::MethodLoadKind::kDexCacheViaMethod: {
      Location current_method = invoke->GetLocations()->InAt(invoke->GetSpecialInputIndex());
      Register method_reg;
      CpuRegister reg = temp.AsRegister<CpuRegister>();
      if (current_method.IsRegister()) {
        method_reg = current_method.AsRegister<Register>();
      } else {
        DCHECK(invoke->GetLocations()->Intrinsified());
        DCHECK(!current_method.IsValid());
        method_reg = reg.AsRegister();
        __ movq(reg, Address(CpuRegister(RSP), kCurrentMethodStackOffset));
      }
      // /* ArtMethod*[] */ temp = temp.ptr_sized_fields_->dex_cache_resolved_methods_;
      __ movq(reg,
              Address(CpuRegister(method_reg),
                      ArtMethod::DexCacheResolvedMethodsOffset(kX86_64PointerSize).SizeValue()));
      // temp = temp[index_in_cache];
      // Note: Don't use invoke->GetTargetMethod() as it may point to a different dex file.
      uint32_t index_in_cache = invoke->GetDexMethodIndex();
      __ movq(reg, Address(reg, CodeGenerator::GetCachePointerOffset(index_in_cache)));
      break;
    }
  }

  switch (invoke->GetCodePtrLocation()) {
    case HInvokeStaticOrDirect::CodePtrLocation::kCallSelf:
      __ call(&frame_entry_label_);
      break;
    case HInvokeStaticOrDirect::CodePtrLocation::kCallPCRelative: {
      relative_call_patches_.emplace_back(invoke->GetTargetMethod());
      Label* label = &relative_call_patches_.back().label;
      __ call(label);  // Bind to the patch label, override at link time.
      __ Bind(label);  // Bind the label at the end of the "call" insn.
      break;
    }
    case HInvokeStaticOrDirect::CodePtrLocation::kCallDirectWithFixup:
    case HInvokeStaticOrDirect::CodePtrLocation::kCallDirect:
      // Filtered out by GetSupportedInvokeStaticOrDirectDispatch().
      LOG(FATAL) << "Unsupported";
      UNREACHABLE();
    case HInvokeStaticOrDirect::CodePtrLocation::kCallArtMethod:
      // (callee_method + offset_of_quick_compiled_code)()
      __ call(Address(callee_method.AsRegister<CpuRegister>(),
                      ArtMethod::EntryPointFromQuickCompiledCodeOffset(
                          kX86_64WordSize).SizeValue()));
      break;
  }

  DCHECK(!IsLeafMethod());
}

void CodeGeneratorX86_64::GenerateVirtualCall(HInvokeVirtual* invoke, Location temp_in) {
  CpuRegister temp = temp_in.AsRegister<CpuRegister>();
  size_t method_offset = mirror::Class::EmbeddedVTableEntryOffset(
      invoke->GetVTableIndex(), kX86_64PointerSize).SizeValue();

  // Use the calling convention instead of the location of the receiver, as
  // intrinsics may have put the receiver in a different register. In the intrinsics
  // slow path, the arguments have been moved to the right place, so here we are
  // guaranteed that the receiver is the first register of the calling convention.
  InvokeDexCallingConvention calling_convention;
  Register receiver = calling_convention.GetRegisterAt(0);

  size_t class_offset = mirror::Object::ClassOffset().SizeValue();
  // /* HeapReference<Class> */ temp = receiver->klass_
  __ movl(temp, Address(CpuRegister(receiver), class_offset));
  MaybeRecordImplicitNullCheck(invoke);
  // Instead of simply (possibly) unpoisoning `temp` here, we should
  // emit a read barrier for the previous class reference load.
  // However this is not required in practice, as this is an
  // intermediate/temporary reference and because the current
  // concurrent copying collector keeps the from-space memory
  // intact/accessible until the end of the marking phase (the
  // concurrent copying collector may not in the future).
  __ MaybeUnpoisonHeapReference(temp);
  // temp = temp->GetMethodAt(method_offset);
  __ movq(temp, Address(temp, method_offset));
  // call temp->GetEntryPoint();
  __ call(Address(temp, ArtMethod::EntryPointFromQuickCompiledCodeOffset(
      kX86_64WordSize).SizeValue()));
}

void CodeGeneratorX86_64::RecordSimplePatch() {
  if (GetCompilerOptions().GetIncludePatchInformation()) {
    simple_patches_.emplace_back();
    __ Bind(&simple_patches_.back());
  }
}

void CodeGeneratorX86_64::RecordStringPatch(HLoadString* load_string) {
  string_patches_.emplace_back(load_string->GetDexFile(), load_string->GetStringIndex());
  __ Bind(&string_patches_.back().label);
}

Label* CodeGeneratorX86_64::NewPcRelativeDexCacheArrayPatch(const DexFile& dex_file,
                                                            uint32_t element_offset) {
  // Add a patch entry and return the label.
  pc_relative_dex_cache_patches_.emplace_back(dex_file, element_offset);
  return &pc_relative_dex_cache_patches_.back().label;
}

void CodeGeneratorX86_64::EmitLinkerPatches(ArenaVector<LinkerPatch>* linker_patches) {
  DCHECK(linker_patches->empty());
  size_t size =
      method_patches_.size() +
      relative_call_patches_.size() +
      pc_relative_dex_cache_patches_.size() +
      simple_patches_.size() +
      string_patches_.size();
  linker_patches->reserve(size);
  // The label points to the end of the "movl" insn but the literal offset for method
  // patch needs to point to the embedded constant which occupies the last 4 bytes.
  constexpr uint32_t kLabelPositionToLiteralOffsetAdjustment = 4u;
  for (const MethodPatchInfo<Label>& info : method_patches_) {
    uint32_t literal_offset = info.label.Position() - kLabelPositionToLiteralOffsetAdjustment;
    linker_patches->push_back(LinkerPatch::MethodPatch(literal_offset,
                                                       info.target_method.dex_file,
                                                       info.target_method.dex_method_index));
  }
  for (const MethodPatchInfo<Label>& info : relative_call_patches_) {
    uint32_t literal_offset = info.label.Position() - kLabelPositionToLiteralOffsetAdjustment;
    linker_patches->push_back(LinkerPatch::RelativeCodePatch(literal_offset,
                                                             info.target_method.dex_file,
                                                             info.target_method.dex_method_index));
  }
  for (const PcRelativeDexCacheAccessInfo& info : pc_relative_dex_cache_patches_) {
    uint32_t literal_offset = info.label.Position() - kLabelPositionToLiteralOffsetAdjustment;
    linker_patches->push_back(LinkerPatch::DexCacheArrayPatch(literal_offset,
                                                              &info.target_dex_file,
                                                              info.label.Position(),
                                                              info.element_offset));
  }
  for (const Label& label : simple_patches_) {
    uint32_t literal_offset = label.Position() - kLabelPositionToLiteralOffsetAdjustment;
    linker_patches->push_back(LinkerPatch::RecordPosition(literal_offset));
  }
  for (const StringPatchInfo<Label>& info : string_patches_) {
    // These are always PC-relative, see GetSupportedLoadStringKind().
    uint32_t literal_offset = info.label.Position() - kLabelPositionToLiteralOffsetAdjustment;
    linker_patches->push_back(LinkerPatch::RelativeStringPatch(literal_offset,
                                                               &info.dex_file,
                                                               info.label.Position(),
                                                               info.string_index));
  }
}

void CodeGeneratorX86_64::DumpCoreRegister(std::ostream& stream, int reg) const {
  stream << Register(reg);
}

void CodeGeneratorX86_64::DumpFloatingPointRegister(std::ostream& stream, int reg) const {
  stream << FloatRegister(reg);
}

size_t CodeGeneratorX86_64::SaveCoreRegister(size_t stack_index, uint32_t reg_id) {
  __ movq(Address(CpuRegister(RSP), stack_index), CpuRegister(reg_id));
  return kX86_64WordSize;
}

size_t CodeGeneratorX86_64::RestoreCoreRegister(size_t stack_index, uint32_t reg_id) {
  __ movq(CpuRegister(reg_id), Address(CpuRegister(RSP), stack_index));
  return kX86_64WordSize;
}

size_t CodeGeneratorX86_64::SaveFloatingPointRegister(size_t stack_index, uint32_t reg_id) {
  __ movsd(Address(CpuRegister(RSP), stack_index), XmmRegister(reg_id));
  return kX86_64WordSize;
}

size_t CodeGeneratorX86_64::RestoreFloatingPointRegister(size_t stack_index, uint32_t reg_id) {
  __ movsd(XmmRegister(reg_id), Address(CpuRegister(RSP), stack_index));
  return kX86_64WordSize;
}

void CodeGeneratorX86_64::InvokeRuntime(QuickEntrypointEnum entrypoint,
                                        HInstruction* instruction,
                                        uint32_t dex_pc,
                                        SlowPathCode* slow_path) {
  InvokeRuntime(GetThreadOffset<kX86_64WordSize>(entrypoint).Int32Value(),
                instruction,
                dex_pc,
                slow_path);
}

void CodeGeneratorX86_64::InvokeRuntime(int32_t entry_point_offset,
                                        HInstruction* instruction,
                                        uint32_t dex_pc,
                                        SlowPathCode* slow_path) {
  ValidateInvokeRuntime(instruction, slow_path);
  __ gs()->call(Address::Absolute(entry_point_offset, /* no_rip */ true));
  RecordPcInfo(instruction, dex_pc, slow_path);
}

static constexpr int kNumberOfCpuRegisterPairs = 0;
// Use a fake return address register to mimic Quick.
static constexpr Register kFakeReturnRegister = Register(kLastCpuRegister + 1);
CodeGeneratorX86_64::CodeGeneratorX86_64(HGraph* graph,
                                         const X86_64InstructionSetFeatures& isa_features,
                                         const CompilerOptions& compiler_options,
                                         OptimizingCompilerStats* stats)
      : CodeGenerator(graph,
                      kNumberOfCpuRegisters,
                      kNumberOfFloatRegisters,
                      kNumberOfCpuRegisterPairs,
                      ComputeRegisterMask(reinterpret_cast<const int*>(kCoreCalleeSaves),
                                          arraysize(kCoreCalleeSaves))
                          | (1 << kFakeReturnRegister),
                      ComputeRegisterMask(reinterpret_cast<const int*>(kFpuCalleeSaves),
                                          arraysize(kFpuCalleeSaves)),
                      compiler_options,
                      stats),
        block_labels_(nullptr),
        location_builder_(graph, this),
        instruction_visitor_(graph, this),
        move_resolver_(graph->GetArena(), this),
        assembler_(graph->GetArena()),
        isa_features_(isa_features),
        constant_area_start_(0),
        method_patches_(graph->GetArena()->Adapter(kArenaAllocCodeGenerator)),
        relative_call_patches_(graph->GetArena()->Adapter(kArenaAllocCodeGenerator)),
        pc_relative_dex_cache_patches_(graph->GetArena()->Adapter(kArenaAllocCodeGenerator)),
        simple_patches_(graph->GetArena()->Adapter(kArenaAllocCodeGenerator)),
        string_patches_(graph->GetArena()->Adapter(kArenaAllocCodeGenerator)),
        fixups_to_jump_tables_(graph->GetArena()->Adapter(kArenaAllocCodeGenerator)) {
  AddAllocatedRegister(Location::RegisterLocation(kFakeReturnRegister));
}

InstructionCodeGeneratorX86_64::InstructionCodeGeneratorX86_64(HGraph* graph,
                                                               CodeGeneratorX86_64* codegen)
      : InstructionCodeGenerator(graph, codegen),
        assembler_(codegen->GetAssembler()),
        codegen_(codegen) {}

void CodeGeneratorX86_64::SetupBlockedRegisters() const {
  // Stack register is always reserved.
  blocked_core_registers_[RSP] = true;

  // Block the register used as TMP.
  blocked_core_registers_[TMP] = true;
}

static dwarf::Reg DWARFReg(Register reg) {
  return dwarf::Reg::X86_64Core(static_cast<int>(reg));
}

static dwarf::Reg DWARFReg(FloatRegister reg) {
  return dwarf::Reg::X86_64Fp(static_cast<int>(reg));
}

void CodeGeneratorX86_64::GenerateFrameEntry() {
  __ cfi().SetCurrentCFAOffset(kX86_64WordSize);  // return address
  __ Bind(&frame_entry_label_);
  bool skip_overflow_check = IsLeafMethod()
      && !FrameNeedsStackCheck(GetFrameSize(), InstructionSet::kX86_64);
  DCHECK(GetCompilerOptions().GetImplicitStackOverflowChecks());

  if (!skip_overflow_check) {
    __ testq(CpuRegister(RAX), Address(
        CpuRegister(RSP), -static_cast<int32_t>(GetStackOverflowReservedBytes(kX86_64))));
    RecordPcInfo(nullptr, 0);
  }

  if (HasEmptyFrame()) {
    return;
  }

  for (int i = arraysize(kCoreCalleeSaves) - 1; i >= 0; --i) {
    Register reg = kCoreCalleeSaves[i];
    if (allocated_registers_.ContainsCoreRegister(reg)) {
      __ pushq(CpuRegister(reg));
      __ cfi().AdjustCFAOffset(kX86_64WordSize);
      __ cfi().RelOffset(DWARFReg(reg), 0);
    }
  }

  int adjust = GetFrameSize() - GetCoreSpillSize();
  __ subq(CpuRegister(RSP), Immediate(adjust));
  __ cfi().AdjustCFAOffset(adjust);
  uint32_t xmm_spill_location = GetFpuSpillStart();
  size_t xmm_spill_slot_size = GetFloatingPointSpillSlotSize();

  for (int i = arraysize(kFpuCalleeSaves) - 1; i >= 0; --i) {
    if (allocated_registers_.ContainsFloatingPointRegister(kFpuCalleeSaves[i])) {
      int offset = xmm_spill_location + (xmm_spill_slot_size * i);
      __ movsd(Address(CpuRegister(RSP), offset), XmmRegister(kFpuCalleeSaves[i]));
      __ cfi().RelOffset(DWARFReg(kFpuCalleeSaves[i]), offset);
    }
  }

  __ movq(Address(CpuRegister(RSP), kCurrentMethodStackOffset),
          CpuRegister(kMethodRegisterArgument));
}

void CodeGeneratorX86_64::GenerateFrameExit() {
  __ cfi().RememberState();
  if (!HasEmptyFrame()) {
    uint32_t xmm_spill_location = GetFpuSpillStart();
    size_t xmm_spill_slot_size = GetFloatingPointSpillSlotSize();
    for (size_t i = 0; i < arraysize(kFpuCalleeSaves); ++i) {
      if (allocated_registers_.ContainsFloatingPointRegister(kFpuCalleeSaves[i])) {
        int offset = xmm_spill_location + (xmm_spill_slot_size * i);
        __ movsd(XmmRegister(kFpuCalleeSaves[i]), Address(CpuRegister(RSP), offset));
        __ cfi().Restore(DWARFReg(kFpuCalleeSaves[i]));
      }
    }

    int adjust = GetFrameSize() - GetCoreSpillSize();
    __ addq(CpuRegister(RSP), Immediate(adjust));
    __ cfi().AdjustCFAOffset(-adjust);

    for (size_t i = 0; i < arraysize(kCoreCalleeSaves); ++i) {
      Register reg = kCoreCalleeSaves[i];
      if (allocated_registers_.ContainsCoreRegister(reg)) {
        __ popq(CpuRegister(reg));
        __ cfi().AdjustCFAOffset(-static_cast<int>(kX86_64WordSize));
        __ cfi().Restore(DWARFReg(reg));
      }
    }
  }
  __ ret();
  __ cfi().RestoreState();
  __ cfi().DefCFAOffset(GetFrameSize());
}

void CodeGeneratorX86_64::Bind(HBasicBlock* block) {
  __ Bind(GetLabelOf(block));
}

void CodeGeneratorX86_64::Move(Location destination, Location source) {
  if (source.Equals(destination)) {
    return;
  }
  if (destination.IsRegister()) {
    CpuRegister dest = destination.AsRegister<CpuRegister>();
    if (source.IsRegister()) {
      __ movq(dest, source.AsRegister<CpuRegister>());
    } else if (source.IsFpuRegister()) {
      __ movd(dest, source.AsFpuRegister<XmmRegister>());
    } else if (source.IsStackSlot()) {
      __ movl(dest, Address(CpuRegister(RSP), source.GetStackIndex()));
    } else if (source.IsConstant()) {
      HConstant* constant = source.GetConstant();
      if (constant->IsLongConstant()) {
        Load64BitValue(dest, constant->AsLongConstant()->GetValue());
      } else {
        Load32BitValue(dest, GetInt32ValueOf(constant));
      }
    } else {
      DCHECK(source.IsDoubleStackSlot());
      __ movq(dest, Address(CpuRegister(RSP), source.GetStackIndex()));
    }
  } else if (destination.IsFpuRegister()) {
    XmmRegister dest = destination.AsFpuRegister<XmmRegister>();
    if (source.IsRegister()) {
      __ movd(dest, source.AsRegister<CpuRegister>());
    } else if (source.IsFpuRegister()) {
      __ movaps(dest, source.AsFpuRegister<XmmRegister>());
    } else if (source.IsConstant()) {
      HConstant* constant = source.GetConstant();
      int64_t value = CodeGenerator::GetInt64ValueOf(constant);
      if (constant->IsFloatConstant()) {
        Load32BitValue(dest, static_cast<int32_t>(value));
      } else {
        Load64BitValue(dest, value);
      }
    } else if (source.IsStackSlot()) {
      __ movss(dest, Address(CpuRegister(RSP), source.GetStackIndex()));
    } else {
      DCHECK(source.IsDoubleStackSlot());
      __ movsd(dest, Address(CpuRegister(RSP), source.GetStackIndex()));
    }
  } else if (destination.IsStackSlot()) {
    if (source.IsRegister()) {
      __ movl(Address(CpuRegister(RSP), destination.GetStackIndex()),
              source.AsRegister<CpuRegister>());
    } else if (source.IsFpuRegister()) {
      __ movss(Address(CpuRegister(RSP), destination.GetStackIndex()),
               source.AsFpuRegister<XmmRegister>());
    } else if (source.IsConstant()) {
      HConstant* constant = source.GetConstant();
      int32_t value = GetInt32ValueOf(constant);
      __ movl(Address(CpuRegister(RSP), destination.GetStackIndex()), Immediate(value));
    } else {
      DCHECK(source.IsStackSlot()) << source;
      __ movl(CpuRegister(TMP), Address(CpuRegister(RSP), source.GetStackIndex()));
      __ movl(Address(CpuRegister(RSP), destination.GetStackIndex()), CpuRegister(TMP));
    }
  } else {
    DCHECK(destination.IsDoubleStackSlot());
    if (source.IsRegister()) {
      __ movq(Address(CpuRegister(RSP), destination.GetStackIndex()),
              source.AsRegister<CpuRegister>());
    } else if (source.IsFpuRegister()) {
      __ movsd(Address(CpuRegister(RSP), destination.GetStackIndex()),
               source.AsFpuRegister<XmmRegister>());
    } else if (source.IsConstant()) {
      HConstant* constant = source.GetConstant();
      int64_t value;
      if (constant->IsDoubleConstant()) {
        value = bit_cast<int64_t, double>(constant->AsDoubleConstant()->GetValue());
      } else {
        DCHECK(constant->IsLongConstant());
        value = constant->AsLongConstant()->GetValue();
      }
      Store64BitValueToStack(destination, value);
    } else {
      DCHECK(source.IsDoubleStackSlot());
      __ movq(CpuRegister(TMP), Address(CpuRegister(RSP), source.GetStackIndex()));
      __ movq(Address(CpuRegister(RSP), destination.GetStackIndex()), CpuRegister(TMP));
    }
  }
}

void CodeGeneratorX86_64::MoveConstant(Location location, int32_t value) {
  DCHECK(location.IsRegister());
  Load64BitValue(location.AsRegister<CpuRegister>(), static_cast<int64_t>(value));
}

void CodeGeneratorX86_64::MoveLocation(
    Location dst, Location src, Primitive::Type dst_type ATTRIBUTE_UNUSED) {
  Move(dst, src);
}

void CodeGeneratorX86_64::AddLocationAsTemp(Location location, LocationSummary* locations) {
  if (location.IsRegister()) {
    locations->AddTemp(location);
  } else {
    UNIMPLEMENTED(FATAL) << "AddLocationAsTemp not implemented for location " << location;
  }
}

void InstructionCodeGeneratorX86_64::HandleGoto(HInstruction* got, HBasicBlock* successor) {
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

void LocationsBuilderX86_64::VisitGoto(HGoto* got) {
  got->SetLocations(nullptr);
}

void InstructionCodeGeneratorX86_64::VisitGoto(HGoto* got) {
  HandleGoto(got, got->GetSuccessor());
}

void LocationsBuilderX86_64::VisitTryBoundary(HTryBoundary* try_boundary) {
  try_boundary->SetLocations(nullptr);
}

void InstructionCodeGeneratorX86_64::VisitTryBoundary(HTryBoundary* try_boundary) {
  HBasicBlock* successor = try_boundary->GetNormalFlowSuccessor();
  if (!successor->IsExitBlock()) {
    HandleGoto(try_boundary, successor);
  }
}

void LocationsBuilderX86_64::VisitExit(HExit* exit) {
  exit->SetLocations(nullptr);
}

void InstructionCodeGeneratorX86_64::VisitExit(HExit* exit ATTRIBUTE_UNUSED) {
}

template<class LabelType>
void InstructionCodeGeneratorX86_64::GenerateFPJumps(HCondition* cond,
                                                     LabelType* true_label,
                                                     LabelType* false_label) {
  if (cond->IsFPConditionTrueIfNaN()) {
    __ j(kUnordered, true_label);
  } else if (cond->IsFPConditionFalseIfNaN()) {
    __ j(kUnordered, false_label);
  }
  __ j(X86_64FPCondition(cond->GetCondition()), true_label);
}

void InstructionCodeGeneratorX86_64::GenerateCompareTest(HCondition* condition) {
  LocationSummary* locations = condition->GetLocations();

  Location left = locations->InAt(0);
  Location right = locations->InAt(1);
  Primitive::Type type = condition->InputAt(0)->GetType();
  switch (type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      CpuRegister left_reg = left.AsRegister<CpuRegister>();
      if (right.IsConstant()) {
        int32_t value = CodeGenerator::GetInt32ValueOf(right.GetConstant());
        if (value == 0) {
          __ testl(left_reg, left_reg);
        } else {
          __ cmpl(left_reg, Immediate(value));
        }
      } else if (right.IsStackSlot()) {
        __ cmpl(left_reg, Address(CpuRegister(RSP), right.GetStackIndex()));
      } else {
        __ cmpl(left_reg, right.AsRegister<CpuRegister>());
      }
      break;
    }
    case Primitive::kPrimLong: {
      CpuRegister left_reg = left.AsRegister<CpuRegister>();
      if (right.IsConstant()) {
        int64_t value = right.GetConstant()->AsLongConstant()->GetValue();
        codegen_->Compare64BitValue(left_reg, value);
      } else if (right.IsDoubleStackSlot()) {
        __ cmpq(left_reg, Address(CpuRegister(RSP), right.GetStackIndex()));
      } else {
        __ cmpq(left_reg, right.AsRegister<CpuRegister>());
      }
      break;
    }
    case Primitive::kPrimFloat: {
      if (right.IsFpuRegister()) {
        __ ucomiss(left.AsFpuRegister<XmmRegister>(), right.AsFpuRegister<XmmRegister>());
      } else if (right.IsConstant()) {
        __ ucomiss(left.AsFpuRegister<XmmRegister>(),
                   codegen_->LiteralFloatAddress(
                     right.GetConstant()->AsFloatConstant()->GetValue()));
      } else {
        DCHECK(right.IsStackSlot());
        __ ucomiss(left.AsFpuRegister<XmmRegister>(),
                   Address(CpuRegister(RSP), right.GetStackIndex()));
      }
      break;
    }
    case Primitive::kPrimDouble: {
      if (right.IsFpuRegister()) {
        __ ucomisd(left.AsFpuRegister<XmmRegister>(), right.AsFpuRegister<XmmRegister>());
      } else if (right.IsConstant()) {
        __ ucomisd(left.AsFpuRegister<XmmRegister>(),
                   codegen_->LiteralDoubleAddress(
                     right.GetConstant()->AsDoubleConstant()->GetValue()));
      } else {
        DCHECK(right.IsDoubleStackSlot());
        __ ucomisd(left.AsFpuRegister<XmmRegister>(),
                   Address(CpuRegister(RSP), right.GetStackIndex()));
      }
      break;
    }
    default:
      LOG(FATAL) << "Unexpected condition type " << type;
  }
}

template<class LabelType>
void InstructionCodeGeneratorX86_64::GenerateCompareTestAndBranch(HCondition* condition,
                                                                  LabelType* true_target_in,
                                                                  LabelType* false_target_in) {
  // Generated branching requires both targets to be explicit. If either of the
  // targets is nullptr (fallthrough) use and bind `fallthrough_target` instead.
  LabelType fallthrough_target;
  LabelType* true_target = true_target_in == nullptr ? &fallthrough_target : true_target_in;
  LabelType* false_target = false_target_in == nullptr ? &fallthrough_target : false_target_in;

  // Generate the comparison to set the CC.
  GenerateCompareTest(condition);

  // Now generate the correct jump(s).
  Primitive::Type type = condition->InputAt(0)->GetType();
  switch (type) {
    case Primitive::kPrimLong: {
      __ j(X86_64IntegerCondition(condition->GetCondition()), true_target);
      break;
    }
    case Primitive::kPrimFloat: {
      GenerateFPJumps(condition, true_target, false_target);
      break;
    }
    case Primitive::kPrimDouble: {
      GenerateFPJumps(condition, true_target, false_target);
      break;
    }
    default:
      LOG(FATAL) << "Unexpected condition type " << type;
  }

  if (false_target != &fallthrough_target) {
    __ jmp(false_target);
  }

  if (fallthrough_target.IsLinked()) {
    __ Bind(&fallthrough_target);
  }
}

static bool AreEflagsSetFrom(HInstruction* cond, HInstruction* branch) {
  // Moves may affect the eflags register (move zero uses xorl), so the EFLAGS
  // are set only strictly before `branch`. We can't use the eflags on long
  // conditions if they are materialized due to the complex branching.
  return cond->IsCondition() &&
         cond->GetNext() == branch &&
         !Primitive::IsFloatingPointType(cond->InputAt(0)->GetType());
}

template<class LabelType>
void InstructionCodeGeneratorX86_64::GenerateTestAndBranch(HInstruction* instruction,
                                                           size_t condition_input_index,
                                                           LabelType* true_target,
                                                           LabelType* false_target) {
  HInstruction* cond = instruction->InputAt(condition_input_index);

  if (true_target == nullptr && false_target == nullptr) {
    // Nothing to do. The code always falls through.
    return;
  } else if (cond->IsIntConstant()) {
    // Constant condition, statically compared against "true" (integer value 1).
    if (cond->AsIntConstant()->IsTrue()) {
      if (true_target != nullptr) {
        __ jmp(true_target);
      }
    } else {
      DCHECK(cond->AsIntConstant()->IsFalse()) << cond->AsIntConstant()->GetValue();
      if (false_target != nullptr) {
        __ jmp(false_target);
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
    if (AreEflagsSetFrom(cond, instruction)) {
      if (true_target == nullptr) {
        __ j(X86_64IntegerCondition(cond->AsCondition()->GetOppositeCondition()), false_target);
      } else {
        __ j(X86_64IntegerCondition(cond->AsCondition()->GetCondition()), true_target);
      }
    } else {
      // Materialized condition, compare against 0.
      Location lhs = instruction->GetLocations()->InAt(condition_input_index);
      if (lhs.IsRegister()) {
        __ testl(lhs.AsRegister<CpuRegister>(), lhs.AsRegister<CpuRegister>());
      } else {
        __ cmpl(Address(CpuRegister(RSP), lhs.GetStackIndex()), Immediate(0));
      }
      if (true_target == nullptr) {
        __ j(kEqual, false_target);
      } else {
        __ j(kNotEqual, true_target);
      }
    }
  } else {
    // Condition has not been materialized, use its inputs as the
    // comparison and its condition as the branch condition.
    HCondition* condition = cond->AsCondition();

    // If this is a long or FP comparison that has been folded into
    // the HCondition, generate the comparison directly.
    Primitive::Type type = condition->InputAt(0)->GetType();
    if (type == Primitive::kPrimLong || Primitive::IsFloatingPointType(type)) {
      GenerateCompareTestAndBranch(condition, true_target, false_target);
      return;
    }

    Location lhs = condition->GetLocations()->InAt(0);
    Location rhs = condition->GetLocations()->InAt(1);
    if (rhs.IsRegister()) {
      __ cmpl(lhs.AsRegister<CpuRegister>(), rhs.AsRegister<CpuRegister>());
    } else if (rhs.IsConstant()) {
      int32_t constant = CodeGenerator::GetInt32ValueOf(rhs.GetConstant());
      codegen_->Compare32BitValue(lhs.AsRegister<CpuRegister>(), constant);
    } else {
      __ cmpl(lhs.AsRegister<CpuRegister>(),
              Address(CpuRegister(RSP), rhs.GetStackIndex()));
    }
      if (true_target == nullptr) {
      __ j(X86_64IntegerCondition(condition->GetOppositeCondition()), false_target);
    } else {
      __ j(X86_64IntegerCondition(condition->GetCondition()), true_target);
    }
  }

  // If neither branch falls through (case 3), the conditional branch to `true_target`
  // was already emitted (case 2) and we need to emit a jump to `false_target`.
  if (true_target != nullptr && false_target != nullptr) {
    __ jmp(false_target);
  }
}

void LocationsBuilderX86_64::VisitIf(HIf* if_instr) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(if_instr);
  if (IsBooleanValueOrMaterializedCondition(if_instr->InputAt(0))) {
    locations->SetInAt(0, Location::Any());
  }
}

void InstructionCodeGeneratorX86_64::VisitIf(HIf* if_instr) {
  HBasicBlock* true_successor = if_instr->IfTrueSuccessor();
  HBasicBlock* false_successor = if_instr->IfFalseSuccessor();
  Label* true_target = codegen_->GoesToNextBlock(if_instr->GetBlock(), true_successor) ?
      nullptr : codegen_->GetLabelOf(true_successor);
  Label* false_target = codegen_->GoesToNextBlock(if_instr->GetBlock(), false_successor) ?
      nullptr : codegen_->GetLabelOf(false_successor);
  GenerateTestAndBranch(if_instr, /* condition_input_index */ 0, true_target, false_target);
}

void LocationsBuilderX86_64::VisitDeoptimize(HDeoptimize* deoptimize) {
  LocationSummary* locations = new (GetGraph()->GetArena())
      LocationSummary(deoptimize, LocationSummary::kCallOnSlowPath);
  if (IsBooleanValueOrMaterializedCondition(deoptimize->InputAt(0))) {
    locations->SetInAt(0, Location::Any());
  }
}

void InstructionCodeGeneratorX86_64::VisitDeoptimize(HDeoptimize* deoptimize) {
  SlowPathCode* slow_path = deopt_slow_paths_.NewSlowPath<DeoptimizationSlowPathX86_64>(deoptimize);
  GenerateTestAndBranch<Label>(deoptimize,
                               /* condition_input_index */ 0,
                               slow_path->GetEntryLabel(),
                               /* false_target */ nullptr);
}

static bool SelectCanUseCMOV(HSelect* select) {
  // There are no conditional move instructions for XMMs.
  if (Primitive::IsFloatingPointType(select->GetType())) {
    return false;
  }

  // A FP condition doesn't generate the single CC that we need.
  HInstruction* condition = select->GetCondition();
  if (condition->IsCondition() &&
      Primitive::IsFloatingPointType(condition->InputAt(0)->GetType())) {
    return false;
  }

  // We can generate a CMOV for this Select.
  return true;
}

void LocationsBuilderX86_64::VisitSelect(HSelect* select) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(select);
  if (Primitive::IsFloatingPointType(select->GetType())) {
    locations->SetInAt(0, Location::RequiresFpuRegister());
    locations->SetInAt(1, Location::Any());
  } else {
    locations->SetInAt(0, Location::RequiresRegister());
    if (SelectCanUseCMOV(select)) {
      if (select->InputAt(1)->IsConstant()) {
        locations->SetInAt(1, Location::RequiresRegister());
      } else {
        locations->SetInAt(1, Location::Any());
      }
    } else {
      locations->SetInAt(1, Location::Any());
    }
  }
  if (IsBooleanValueOrMaterializedCondition(select->GetCondition())) {
    locations->SetInAt(2, Location::RequiresRegister());
  }
  locations->SetOut(Location::SameAsFirstInput());
}

void InstructionCodeGeneratorX86_64::VisitSelect(HSelect* select) {
  LocationSummary* locations = select->GetLocations();
  if (SelectCanUseCMOV(select)) {
    // If both the condition and the source types are integer, we can generate
    // a CMOV to implement Select.
    CpuRegister value_false = locations->InAt(0).AsRegister<CpuRegister>();
    Location value_true_loc = locations->InAt(1);
    DCHECK(locations->InAt(0).Equals(locations->Out()));

    HInstruction* select_condition = select->GetCondition();
    Condition cond = kNotEqual;

    // Figure out how to test the 'condition'.
    if (select_condition->IsCondition()) {
      HCondition* condition = select_condition->AsCondition();
      if (!condition->IsEmittedAtUseSite()) {
        // This was a previously materialized condition.
        // Can we use the existing condition code?
        if (AreEflagsSetFrom(condition, select)) {
          // Materialization was the previous instruction.  Condition codes are right.
          cond = X86_64IntegerCondition(condition->GetCondition());
        } else {
          // No, we have to recreate the condition code.
          CpuRegister cond_reg = locations->InAt(2).AsRegister<CpuRegister>();
          __ testl(cond_reg, cond_reg);
        }
      } else {
        GenerateCompareTest(condition);
        cond = X86_64IntegerCondition(condition->GetCondition());
      }
    } else {
      // Must be a boolean condition, which needs to be compared to 0.
      CpuRegister cond_reg = locations->InAt(2).AsRegister<CpuRegister>();
      __ testl(cond_reg, cond_reg);
    }

    // If the condition is true, overwrite the output, which already contains false.
    // Generate the correct sized CMOV.
    bool is_64_bit = Primitive::Is64BitType(select->GetType());
    if (value_true_loc.IsRegister()) {
      __ cmov(cond, value_false, value_true_loc.AsRegister<CpuRegister>(), is_64_bit);
    } else {
      __ cmov(cond,
              value_false,
              Address(CpuRegister(RSP), value_true_loc.GetStackIndex()), is_64_bit);
    }
  } else {
    NearLabel false_target;
    GenerateTestAndBranch<NearLabel>(select,
                                     /* condition_input_index */ 2,
                                     /* true_target */ nullptr,
                                     &false_target);
    codegen_->MoveLocation(locations->Out(), locations->InAt(1), select->GetType());
    __ Bind(&false_target);
  }
}

void LocationsBuilderX86_64::VisitNativeDebugInfo(HNativeDebugInfo* info) {
  new (GetGraph()->GetArena()) LocationSummary(info);
}

void InstructionCodeGeneratorX86_64::VisitNativeDebugInfo(HNativeDebugInfo*) {
  // MaybeRecordNativeDebugInfo is already called implicitly in CodeGenerator::Compile.
}

void CodeGeneratorX86_64::GenerateNop() {
  __ nop();
}

void LocationsBuilderX86_64::HandleCondition(HCondition* cond) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(cond, LocationSummary::kNoCall);
  // Handle the long/FP comparisons made in instruction simplification.
  switch (cond->InputAt(0)->GetType()) {
    case Primitive::kPrimLong:
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::Any());
      break;
    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetInAt(1, Location::Any());
      break;
    default:
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::Any());
      break;
  }
  if (!cond->IsEmittedAtUseSite()) {
    locations->SetOut(Location::RequiresRegister());
  }
}

void InstructionCodeGeneratorX86_64::HandleCondition(HCondition* cond) {
  if (cond->IsEmittedAtUseSite()) {
    return;
  }

  LocationSummary* locations = cond->GetLocations();
  Location lhs = locations->InAt(0);
  Location rhs = locations->InAt(1);
  CpuRegister reg = locations->Out().AsRegister<CpuRegister>();
  NearLabel true_label, false_label;

  switch (cond->InputAt(0)->GetType()) {
    default:
      // Integer case.

      // Clear output register: setcc only sets the low byte.
      __ xorl(reg, reg);

      if (rhs.IsRegister()) {
        __ cmpl(lhs.AsRegister<CpuRegister>(), rhs.AsRegister<CpuRegister>());
      } else if (rhs.IsConstant()) {
        int32_t constant = CodeGenerator::GetInt32ValueOf(rhs.GetConstant());
        codegen_->Compare32BitValue(lhs.AsRegister<CpuRegister>(), constant);
      } else {
        __ cmpl(lhs.AsRegister<CpuRegister>(), Address(CpuRegister(RSP), rhs.GetStackIndex()));
      }
      __ setcc(X86_64IntegerCondition(cond->GetCondition()), reg);
      return;
    case Primitive::kPrimLong:
      // Clear output register: setcc only sets the low byte.
      __ xorl(reg, reg);

      if (rhs.IsRegister()) {
        __ cmpq(lhs.AsRegister<CpuRegister>(), rhs.AsRegister<CpuRegister>());
      } else if (rhs.IsConstant()) {
        int64_t value = rhs.GetConstant()->AsLongConstant()->GetValue();
        codegen_->Compare64BitValue(lhs.AsRegister<CpuRegister>(), value);
      } else {
        __ cmpq(lhs.AsRegister<CpuRegister>(), Address(CpuRegister(RSP), rhs.GetStackIndex()));
      }
      __ setcc(X86_64IntegerCondition(cond->GetCondition()), reg);
      return;
    case Primitive::kPrimFloat: {
      XmmRegister lhs_reg = lhs.AsFpuRegister<XmmRegister>();
      if (rhs.IsConstant()) {
        float value = rhs.GetConstant()->AsFloatConstant()->GetValue();
        __ ucomiss(lhs_reg, codegen_->LiteralFloatAddress(value));
      } else if (rhs.IsStackSlot()) {
        __ ucomiss(lhs_reg, Address(CpuRegister(RSP), rhs.GetStackIndex()));
      } else {
        __ ucomiss(lhs_reg, rhs.AsFpuRegister<XmmRegister>());
      }
      GenerateFPJumps(cond, &true_label, &false_label);
      break;
    }
    case Primitive::kPrimDouble: {
      XmmRegister lhs_reg = lhs.AsFpuRegister<XmmRegister>();
      if (rhs.IsConstant()) {
        double value = rhs.GetConstant()->AsDoubleConstant()->GetValue();
        __ ucomisd(lhs_reg, codegen_->LiteralDoubleAddress(value));
      } else if (rhs.IsDoubleStackSlot()) {
        __ ucomisd(lhs_reg, Address(CpuRegister(RSP), rhs.GetStackIndex()));
      } else {
        __ ucomisd(lhs_reg, rhs.AsFpuRegister<XmmRegister>());
      }
      GenerateFPJumps(cond, &true_label, &false_label);
      break;
    }
  }

  // Convert the jumps into the result.
  NearLabel done_label;

  // False case: result = 0.
  __ Bind(&false_label);
  __ xorl(reg, reg);
  __ jmp(&done_label);

  // True case: result = 1.
  __ Bind(&true_label);
  __ movl(reg, Immediate(1));
  __ Bind(&done_label);
}

void LocationsBuilderX86_64::VisitEqual(HEqual* comp) {
  HandleCondition(comp);
}

void InstructionCodeGeneratorX86_64::VisitEqual(HEqual* comp) {
  HandleCondition(comp);
}

void LocationsBuilderX86_64::VisitNotEqual(HNotEqual* comp) {
  HandleCondition(comp);
}

void InstructionCodeGeneratorX86_64::VisitNotEqual(HNotEqual* comp) {
  HandleCondition(comp);
}

void LocationsBuilderX86_64::VisitLessThan(HLessThan* comp) {
  HandleCondition(comp);
}

void InstructionCodeGeneratorX86_64::VisitLessThan(HLessThan* comp) {
  HandleCondition(comp);
}

void LocationsBuilderX86_64::VisitLessThanOrEqual(HLessThanOrEqual* comp) {
  HandleCondition(comp);
}

void InstructionCodeGeneratorX86_64::VisitLessThanOrEqual(HLessThanOrEqual* comp) {
  HandleCondition(comp);
}

void LocationsBuilderX86_64::VisitGreaterThan(HGreaterThan* comp) {
  HandleCondition(comp);
}

void InstructionCodeGeneratorX86_64::VisitGreaterThan(HGreaterThan* comp) {
  HandleCondition(comp);
}

void LocationsBuilderX86_64::VisitGreaterThanOrEqual(HGreaterThanOrEqual* comp) {
  HandleCondition(comp);
}

void InstructionCodeGeneratorX86_64::VisitGreaterThanOrEqual(HGreaterThanOrEqual* comp) {
  HandleCondition(comp);
}

void LocationsBuilderX86_64::VisitBelow(HBelow* comp) {
  HandleCondition(comp);
}

void InstructionCodeGeneratorX86_64::VisitBelow(HBelow* comp) {
  HandleCondition(comp);
}

void LocationsBuilderX86_64::VisitBelowOrEqual(HBelowOrEqual* comp) {
  HandleCondition(comp);
}

void InstructionCodeGeneratorX86_64::VisitBelowOrEqual(HBelowOrEqual* comp) {
  HandleCondition(comp);
}

void LocationsBuilderX86_64::VisitAbove(HAbove* comp) {
  HandleCondition(comp);
}

void InstructionCodeGeneratorX86_64::VisitAbove(HAbove* comp) {
  HandleCondition(comp);
}

void LocationsBuilderX86_64::VisitAboveOrEqual(HAboveOrEqual* comp) {
  HandleCondition(comp);
}

void InstructionCodeGeneratorX86_64::VisitAboveOrEqual(HAboveOrEqual* comp) {
  HandleCondition(comp);
}

void LocationsBuilderX86_64::VisitCompare(HCompare* compare) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(compare, LocationSummary::kNoCall);
  switch (compare->InputAt(0)->GetType()) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimShort:
    case Primitive::kPrimChar:
    case Primitive::kPrimInt:
    case Primitive::kPrimLong: {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::Any());
      locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      break;
    }
    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetInAt(1, Location::Any());
      locations->SetOut(Location::RequiresRegister());
      break;
    }
    default:
      LOG(FATAL) << "Unexpected type for compare operation " << compare->InputAt(0)->GetType();
  }
}

void InstructionCodeGeneratorX86_64::VisitCompare(HCompare* compare) {
  LocationSummary* locations = compare->GetLocations();
  CpuRegister out = locations->Out().AsRegister<CpuRegister>();
  Location left = locations->InAt(0);
  Location right = locations->InAt(1);

  NearLabel less, greater, done;
  Primitive::Type type = compare->InputAt(0)->GetType();
  Condition less_cond = kLess;

  switch (type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimShort:
    case Primitive::kPrimChar:
    case Primitive::kPrimInt: {
      CpuRegister left_reg = left.AsRegister<CpuRegister>();
      if (right.IsConstant()) {
        int32_t value = right.GetConstant()->AsIntConstant()->GetValue();
        codegen_->Compare32BitValue(left_reg, value);
      } else if (right.IsStackSlot()) {
        __ cmpl(left_reg, Address(CpuRegister(RSP), right.GetStackIndex()));
      } else {
        __ cmpl(left_reg, right.AsRegister<CpuRegister>());
      }
      break;
    }
    case Primitive::kPrimLong: {
      CpuRegister left_reg = left.AsRegister<CpuRegister>();
      if (right.IsConstant()) {
        int64_t value = right.GetConstant()->AsLongConstant()->GetValue();
        codegen_->Compare64BitValue(left_reg, value);
      } else if (right.IsDoubleStackSlot()) {
        __ cmpq(left_reg, Address(CpuRegister(RSP), right.GetStackIndex()));
      } else {
        __ cmpq(left_reg, right.AsRegister<CpuRegister>());
      }
      break;
    }
    case Primitive::kPrimFloat: {
      XmmRegister left_reg = left.AsFpuRegister<XmmRegister>();
      if (right.IsConstant()) {
        float value = right.GetConstant()->AsFloatConstant()->GetValue();
        __ ucomiss(left_reg, codegen_->LiteralFloatAddress(value));
      } else if (right.IsStackSlot()) {
        __ ucomiss(left_reg, Address(CpuRegister(RSP), right.GetStackIndex()));
      } else {
        __ ucomiss(left_reg, right.AsFpuRegister<XmmRegister>());
      }
      __ j(kUnordered, compare->IsGtBias() ? &greater : &less);
      less_cond = kBelow;  //  ucomis{s,d} sets CF
      break;
    }
    case Primitive::kPrimDouble: {
      XmmRegister left_reg = left.AsFpuRegister<XmmRegister>();
      if (right.IsConstant()) {
        double value = right.GetConstant()->AsDoubleConstant()->GetValue();
        __ ucomisd(left_reg, codegen_->LiteralDoubleAddress(value));
      } else if (right.IsDoubleStackSlot()) {
        __ ucomisd(left_reg, Address(CpuRegister(RSP), right.GetStackIndex()));
      } else {
        __ ucomisd(left_reg, right.AsFpuRegister<XmmRegister>());
      }
      __ j(kUnordered, compare->IsGtBias() ? &greater : &less);
      less_cond = kBelow;  //  ucomis{s,d} sets CF
      break;
    }
    default:
      LOG(FATAL) << "Unexpected compare type " << type;
  }

  __ movl(out, Immediate(0));
  __ j(kEqual, &done);
  __ j(less_cond, &less);

  __ Bind(&greater);
  __ movl(out, Immediate(1));
  __ jmp(&done);

  __ Bind(&less);
  __ movl(out, Immediate(-1));

  __ Bind(&done);
}

void LocationsBuilderX86_64::VisitIntConstant(HIntConstant* constant) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(constant, LocationSummary::kNoCall);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorX86_64::VisitIntConstant(HIntConstant* constant ATTRIBUTE_UNUSED) {
  // Will be generated at use site.
}

void LocationsBuilderX86_64::VisitNullConstant(HNullConstant* constant) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(constant, LocationSummary::kNoCall);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorX86_64::VisitNullConstant(HNullConstant* constant ATTRIBUTE_UNUSED) {
  // Will be generated at use site.
}

void LocationsBuilderX86_64::VisitLongConstant(HLongConstant* constant) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(constant, LocationSummary::kNoCall);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorX86_64::VisitLongConstant(HLongConstant* constant ATTRIBUTE_UNUSED) {
  // Will be generated at use site.
}

void LocationsBuilderX86_64::VisitFloatConstant(HFloatConstant* constant) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(constant, LocationSummary::kNoCall);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorX86_64::VisitFloatConstant(HFloatConstant* constant ATTRIBUTE_UNUSED) {
  // Will be generated at use site.
}

void LocationsBuilderX86_64::VisitDoubleConstant(HDoubleConstant* constant) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(constant, LocationSummary::kNoCall);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorX86_64::VisitDoubleConstant(
    HDoubleConstant* constant ATTRIBUTE_UNUSED) {
  // Will be generated at use site.
}

void LocationsBuilderX86_64::VisitMemoryBarrier(HMemoryBarrier* memory_barrier) {
  memory_barrier->SetLocations(nullptr);
}

void InstructionCodeGeneratorX86_64::VisitMemoryBarrier(HMemoryBarrier* memory_barrier) {
  codegen_->GenerateMemoryBarrier(memory_barrier->GetBarrierKind());
}

void LocationsBuilderX86_64::VisitReturnVoid(HReturnVoid* ret) {
  ret->SetLocations(nullptr);
}

void InstructionCodeGeneratorX86_64::VisitReturnVoid(HReturnVoid* ret ATTRIBUTE_UNUSED) {
  codegen_->GenerateFrameExit();
}

void LocationsBuilderX86_64::VisitReturn(HReturn* ret) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(ret, LocationSummary::kNoCall);
  switch (ret->InputAt(0)->GetType()) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimInt:
    case Primitive::kPrimNot:
    case Primitive::kPrimLong:
      locations->SetInAt(0, Location::RegisterLocation(RAX));
      break;

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      locations->SetInAt(0, Location::FpuRegisterLocation(XMM0));
      break;

    default:
      LOG(FATAL) << "Unexpected return type " << ret->InputAt(0)->GetType();
  }
}

void InstructionCodeGeneratorX86_64::VisitReturn(HReturn* ret) {
  if (kIsDebugBuild) {
    switch (ret->InputAt(0)->GetType()) {
      case Primitive::kPrimBoolean:
      case Primitive::kPrimByte:
      case Primitive::kPrimChar:
      case Primitive::kPrimShort:
      case Primitive::kPrimInt:
      case Primitive::kPrimNot:
      case Primitive::kPrimLong:
        DCHECK_EQ(ret->GetLocations()->InAt(0).AsRegister<CpuRegister>().AsRegister(), RAX);
        break;

      case Primitive::kPrimFloat:
      case Primitive::kPrimDouble:
        DCHECK_EQ(ret->GetLocations()->InAt(0).AsFpuRegister<XmmRegister>().AsFloatRegister(),
                  XMM0);
        break;

      default:
        LOG(FATAL) << "Unexpected return type " << ret->InputAt(0)->GetType();
    }
  }
  codegen_->GenerateFrameExit();
}

Location InvokeDexCallingConventionVisitorX86_64::GetReturnLocation(Primitive::Type type) const {
  switch (type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimInt:
    case Primitive::kPrimNot:
    case Primitive::kPrimLong:
      return Location::RegisterLocation(RAX);

    case Primitive::kPrimVoid:
      return Location::NoLocation();

    case Primitive::kPrimDouble:
    case Primitive::kPrimFloat:
      return Location::FpuRegisterLocation(XMM0);
  }

  UNREACHABLE();
}

Location InvokeDexCallingConventionVisitorX86_64::GetMethodLocation() const {
  return Location::RegisterLocation(kMethodRegisterArgument);
}

Location InvokeDexCallingConventionVisitorX86_64::GetNextLocation(Primitive::Type type) {
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
      stack_index_ += 2;
      if (index < calling_convention.GetNumberOfRegisters()) {
        gp_index_ += 1;
        return Location::RegisterLocation(calling_convention.GetRegisterAt(index));
      } else {
        gp_index_ += 2;
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
  return Location::NoLocation();
}

void LocationsBuilderX86_64::VisitInvokeUnresolved(HInvokeUnresolved* invoke) {
  // The trampoline uses the same calling convention as dex calling conventions,
  // except instead of loading arg0/r0 with the target Method*, arg0/r0 will contain
  // the method_idx.
  HandleInvoke(invoke);
}

void InstructionCodeGeneratorX86_64::VisitInvokeUnresolved(HInvokeUnresolved* invoke) {
  codegen_->GenerateInvokeUnresolvedRuntimeCall(invoke);
}

void LocationsBuilderX86_64::VisitInvokeStaticOrDirect(HInvokeStaticOrDirect* invoke) {
  // Explicit clinit checks triggered by static invokes must have been pruned by
  // art::PrepareForRegisterAllocation.
  DCHECK(!invoke->IsStaticWithExplicitClinitCheck());

  IntrinsicLocationsBuilderX86_64 intrinsic(codegen_);
  if (intrinsic.TryDispatch(invoke)) {
    return;
  }

  HandleInvoke(invoke);
}

static bool TryGenerateIntrinsicCode(HInvoke* invoke, CodeGeneratorX86_64* codegen) {
  if (invoke->GetLocations()->Intrinsified()) {
    IntrinsicCodeGeneratorX86_64 intrinsic(codegen);
    intrinsic.Dispatch(invoke);
    return true;
  }
  return false;
}

void InstructionCodeGeneratorX86_64::VisitInvokeStaticOrDirect(HInvokeStaticOrDirect* invoke) {
  // Explicit clinit checks triggered by static invokes must have been pruned by
  // art::PrepareForRegisterAllocation.
  DCHECK(!invoke->IsStaticWithExplicitClinitCheck());

  if (TryGenerateIntrinsicCode(invoke, codegen_)) {
    return;
  }

  LocationSummary* locations = invoke->GetLocations();
  codegen_->GenerateStaticOrDirectCall(
      invoke, locations->HasTemps() ? locations->GetTemp(0) : Location::NoLocation());
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
}

void LocationsBuilderX86_64::HandleInvoke(HInvoke* invoke) {
  InvokeDexCallingConventionVisitorX86_64 calling_convention_visitor;
  CodeGenerator::CreateCommonInvokeLocationSummary(invoke, &calling_convention_visitor);
}

void LocationsBuilderX86_64::VisitInvokeVirtual(HInvokeVirtual* invoke) {
  IntrinsicLocationsBuilderX86_64 intrinsic(codegen_);
  if (intrinsic.TryDispatch(invoke)) {
    return;
  }

  HandleInvoke(invoke);
}

void InstructionCodeGeneratorX86_64::VisitInvokeVirtual(HInvokeVirtual* invoke) {
  if (TryGenerateIntrinsicCode(invoke, codegen_)) {
    return;
  }

  codegen_->GenerateVirtualCall(invoke, invoke->GetLocations()->GetTemp(0));
  DCHECK(!codegen_->IsLeafMethod());
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
}

void LocationsBuilderX86_64::VisitInvokeInterface(HInvokeInterface* invoke) {
  HandleInvoke(invoke);
  // Add the hidden argument.
  invoke->GetLocations()->AddTemp(Location::RegisterLocation(RAX));
}

void InstructionCodeGeneratorX86_64::VisitInvokeInterface(HInvokeInterface* invoke) {
  // TODO: b/18116999, our IMTs can miss an IncompatibleClassChangeError.
  LocationSummary* locations = invoke->GetLocations();
  CpuRegister temp = locations->GetTemp(0).AsRegister<CpuRegister>();
  CpuRegister hidden_reg = locations->GetTemp(1).AsRegister<CpuRegister>();
  uint32_t method_offset = mirror::Class::EmbeddedImTableEntryOffset(
      invoke->GetImtIndex() % mirror::Class::kImtSize, kX86_64PointerSize).Uint32Value();
  Location receiver = locations->InAt(0);
  size_t class_offset = mirror::Object::ClassOffset().SizeValue();

  // Set the hidden argument. This is safe to do this here, as RAX
  // won't be modified thereafter, before the `call` instruction.
  DCHECK_EQ(RAX, hidden_reg.AsRegister());
  codegen_->Load64BitValue(hidden_reg, invoke->GetDexMethodIndex());

  if (receiver.IsStackSlot()) {
    __ movl(temp, Address(CpuRegister(RSP), receiver.GetStackIndex()));
    // /* HeapReference<Class> */ temp = temp->klass_
    __ movl(temp, Address(temp, class_offset));
  } else {
    // /* HeapReference<Class> */ temp = receiver->klass_
    __ movl(temp, Address(receiver.AsRegister<CpuRegister>(), class_offset));
  }
  codegen_->MaybeRecordImplicitNullCheck(invoke);
  // Instead of simply (possibly) unpoisoning `temp` here, we should
  // emit a read barrier for the previous class reference load.
  // However this is not required in practice, as this is an
  // intermediate/temporary reference and because the current
  // concurrent copying collector keeps the from-space memory
  // intact/accessible until the end of the marking phase (the
  // concurrent copying collector may not in the future).
  __ MaybeUnpoisonHeapReference(temp);
  // temp = temp->GetImtEntryAt(method_offset);
  __ movq(temp, Address(temp, method_offset));
  // call temp->GetEntryPoint();
  __ call(Address(temp,
                  ArtMethod::EntryPointFromQuickCompiledCodeOffset(kX86_64WordSize).SizeValue()));

  DCHECK(!codegen_->IsLeafMethod());
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
}

void LocationsBuilderX86_64::VisitNeg(HNeg* neg) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(neg, LocationSummary::kNoCall);
  switch (neg->GetResultType()) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong:
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetOut(Location::SameAsFirstInput());
      break;

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetOut(Location::SameAsFirstInput());
      locations->AddTemp(Location::RequiresFpuRegister());
      break;

    default:
      LOG(FATAL) << "Unexpected neg type " << neg->GetResultType();
  }
}

void InstructionCodeGeneratorX86_64::VisitNeg(HNeg* neg) {
  LocationSummary* locations = neg->GetLocations();
  Location out = locations->Out();
  Location in = locations->InAt(0);
  switch (neg->GetResultType()) {
    case Primitive::kPrimInt:
      DCHECK(in.IsRegister());
      DCHECK(in.Equals(out));
      __ negl(out.AsRegister<CpuRegister>());
      break;

    case Primitive::kPrimLong:
      DCHECK(in.IsRegister());
      DCHECK(in.Equals(out));
      __ negq(out.AsRegister<CpuRegister>());
      break;

    case Primitive::kPrimFloat: {
      DCHECK(in.Equals(out));
      XmmRegister mask = locations->GetTemp(0).AsFpuRegister<XmmRegister>();
      // Implement float negation with an exclusive or with value
      // 0x80000000 (mask for bit 31, representing the sign of a
      // single-precision floating-point number).
      __ movss(mask, codegen_->LiteralInt32Address(0x80000000));
      __ xorps(out.AsFpuRegister<XmmRegister>(), mask);
      break;
    }

    case Primitive::kPrimDouble: {
      DCHECK(in.Equals(out));
      XmmRegister mask = locations->GetTemp(0).AsFpuRegister<XmmRegister>();
      // Implement double negation with an exclusive or with value
      // 0x8000000000000000 (mask for bit 63, representing the sign of
      // a double-precision floating-point number).
      __ movsd(mask, codegen_->LiteralInt64Address(INT64_C(0x8000000000000000)));
      __ xorpd(out.AsFpuRegister<XmmRegister>(), mask);
      break;
    }

    default:
      LOG(FATAL) << "Unexpected neg type " << neg->GetResultType();
  }
}

void LocationsBuilderX86_64::VisitTypeConversion(HTypeConversion* conversion) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(conversion, LocationSummary::kNoCall);
  Primitive::Type result_type = conversion->GetResultType();
  Primitive::Type input_type = conversion->GetInputType();
  DCHECK_NE(result_type, input_type);

  // The Java language does not allow treating boolean as an integral type but
  // our bit representation makes it safe.

  switch (result_type) {
    case Primitive::kPrimByte:
      switch (input_type) {
        case Primitive::kPrimLong:
          // Type conversion from long to byte is a result of code transformations.
        case Primitive::kPrimBoolean:
          // Boolean input is a result of code transformations.
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
        case Primitive::kPrimLong:
          // Type conversion from long to short is a result of code transformations.
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
          break;

        case Primitive::kPrimDouble:
          // Processing a Dex `double-to-int' instruction.
          locations->SetInAt(0, Location::RequiresFpuRegister());
          locations->SetOut(Location::RequiresRegister());
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
          // TODO: We would benefit from a (to-be-implemented)
          // Location::RegisterOrStackSlot requirement for this input.
          locations->SetInAt(0, Location::RequiresRegister());
          locations->SetOut(Location::RequiresRegister());
          break;

        case Primitive::kPrimFloat:
          // Processing a Dex `float-to-long' instruction.
          locations->SetInAt(0, Location::RequiresFpuRegister());
          locations->SetOut(Location::RequiresRegister());
          break;

        case Primitive::kPrimDouble:
          // Processing a Dex `double-to-long' instruction.
          locations->SetInAt(0, Location::RequiresFpuRegister());
          locations->SetOut(Location::RequiresRegister());
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      }
      break;

    case Primitive::kPrimChar:
      switch (input_type) {
        case Primitive::kPrimLong:
          // Type conversion from long to char is a result of code transformations.
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
          locations->SetInAt(0, Location::Any());
          locations->SetOut(Location::RequiresFpuRegister());
          break;

        case Primitive::kPrimLong:
          // Processing a Dex `long-to-float' instruction.
          locations->SetInAt(0, Location::Any());
          locations->SetOut(Location::RequiresFpuRegister());
          break;

        case Primitive::kPrimDouble:
          // Processing a Dex `double-to-float' instruction.
          locations->SetInAt(0, Location::Any());
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
          locations->SetInAt(0, Location::Any());
          locations->SetOut(Location::RequiresFpuRegister());
          break;

        case Primitive::kPrimLong:
          // Processing a Dex `long-to-double' instruction.
          locations->SetInAt(0, Location::Any());
          locations->SetOut(Location::RequiresFpuRegister());
          break;

        case Primitive::kPrimFloat:
          // Processing a Dex `float-to-double' instruction.
          locations->SetInAt(0, Location::Any());
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

void InstructionCodeGeneratorX86_64::VisitTypeConversion(HTypeConversion* conversion) {
  LocationSummary* locations = conversion->GetLocations();
  Location out = locations->Out();
  Location in = locations->InAt(0);
  Primitive::Type result_type = conversion->GetResultType();
  Primitive::Type input_type = conversion->GetInputType();
  DCHECK_NE(result_type, input_type);
  switch (result_type) {
    case Primitive::kPrimByte:
      switch (input_type) {
        case Primitive::kPrimLong:
          // Type conversion from long to byte is a result of code transformations.
        case Primitive::kPrimBoolean:
          // Boolean input is a result of code transformations.
        case Primitive::kPrimShort:
        case Primitive::kPrimInt:
        case Primitive::kPrimChar:
          // Processing a Dex `int-to-byte' instruction.
          if (in.IsRegister()) {
            __ movsxb(out.AsRegister<CpuRegister>(), in.AsRegister<CpuRegister>());
          } else if (in.IsStackSlot() || in.IsDoubleStackSlot()) {
            __ movsxb(out.AsRegister<CpuRegister>(),
                      Address(CpuRegister(RSP), in.GetStackIndex()));
          } else {
            __ movl(out.AsRegister<CpuRegister>(),
                    Immediate(static_cast<int8_t>(Int64FromConstant(in.GetConstant()))));
          }
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      }
      break;

    case Primitive::kPrimShort:
      switch (input_type) {
        case Primitive::kPrimLong:
          // Type conversion from long to short is a result of code transformations.
        case Primitive::kPrimBoolean:
          // Boolean input is a result of code transformations.
        case Primitive::kPrimByte:
        case Primitive::kPrimInt:
        case Primitive::kPrimChar:
          // Processing a Dex `int-to-short' instruction.
          if (in.IsRegister()) {
            __ movsxw(out.AsRegister<CpuRegister>(), in.AsRegister<CpuRegister>());
          } else if (in.IsStackSlot() || in.IsDoubleStackSlot()) {
            __ movsxw(out.AsRegister<CpuRegister>(),
                      Address(CpuRegister(RSP), in.GetStackIndex()));
          } else {
            __ movl(out.AsRegister<CpuRegister>(),
                    Immediate(static_cast<int16_t>(Int64FromConstant(in.GetConstant()))));
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
          if (in.IsRegister()) {
            __ movl(out.AsRegister<CpuRegister>(), in.AsRegister<CpuRegister>());
          } else if (in.IsDoubleStackSlot()) {
            __ movl(out.AsRegister<CpuRegister>(),
                    Address(CpuRegister(RSP), in.GetStackIndex()));
          } else {
            DCHECK(in.IsConstant());
            DCHECK(in.GetConstant()->IsLongConstant());
            int64_t value = in.GetConstant()->AsLongConstant()->GetValue();
            __ movl(out.AsRegister<CpuRegister>(), Immediate(static_cast<int32_t>(value)));
          }
          break;

        case Primitive::kPrimFloat: {
          // Processing a Dex `float-to-int' instruction.
          XmmRegister input = in.AsFpuRegister<XmmRegister>();
          CpuRegister output = out.AsRegister<CpuRegister>();
          NearLabel done, nan;

          __ movl(output, Immediate(kPrimIntMax));
          // if input >= (float)INT_MAX goto done
          __ comiss(input, codegen_->LiteralFloatAddress(kPrimIntMax));
          __ j(kAboveEqual, &done);
          // if input == NaN goto nan
          __ j(kUnordered, &nan);
          // output = float-to-int-truncate(input)
          __ cvttss2si(output, input, false);
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
          CpuRegister output = out.AsRegister<CpuRegister>();
          NearLabel done, nan;

          __ movl(output, Immediate(kPrimIntMax));
          // if input >= (double)INT_MAX goto done
          __ comisd(input, codegen_->LiteralDoubleAddress(kPrimIntMax));
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
        DCHECK(out.IsRegister());
        case Primitive::kPrimBoolean:
          // Boolean input is a result of code transformations.
        case Primitive::kPrimByte:
        case Primitive::kPrimShort:
        case Primitive::kPrimInt:
        case Primitive::kPrimChar:
          // Processing a Dex `int-to-long' instruction.
          DCHECK(in.IsRegister());
          __ movsxd(out.AsRegister<CpuRegister>(), in.AsRegister<CpuRegister>());
          break;

        case Primitive::kPrimFloat: {
          // Processing a Dex `float-to-long' instruction.
          XmmRegister input = in.AsFpuRegister<XmmRegister>();
          CpuRegister output = out.AsRegister<CpuRegister>();
          NearLabel done, nan;

          codegen_->Load64BitValue(output, kPrimLongMax);
          // if input >= (float)LONG_MAX goto done
          __ comiss(input, codegen_->LiteralFloatAddress(kPrimLongMax));
          __ j(kAboveEqual, &done);
          // if input == NaN goto nan
          __ j(kUnordered, &nan);
          // output = float-to-long-truncate(input)
          __ cvttss2si(output, input, true);
          __ jmp(&done);
          __ Bind(&nan);
          //  output = 0
          __ xorl(output, output);
          __ Bind(&done);
          break;
        }

        case Primitive::kPrimDouble: {
          // Processing a Dex `double-to-long' instruction.
          XmmRegister input = in.AsFpuRegister<XmmRegister>();
          CpuRegister output = out.AsRegister<CpuRegister>();
          NearLabel done, nan;

          codegen_->Load64BitValue(output, kPrimLongMax);
          // if input >= (double)LONG_MAX goto done
          __ comisd(input, codegen_->LiteralDoubleAddress(kPrimLongMax));
          __ j(kAboveEqual, &done);
          // if input == NaN goto nan
          __ j(kUnordered, &nan);
          // output = double-to-long-truncate(input)
          __ cvttsd2si(output, input, true);
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

    case Primitive::kPrimChar:
      switch (input_type) {
        case Primitive::kPrimLong:
          // Type conversion from long to char is a result of code transformations.
        case Primitive::kPrimBoolean:
          // Boolean input is a result of code transformations.
        case Primitive::kPrimByte:
        case Primitive::kPrimShort:
        case Primitive::kPrimInt:
          // Processing a Dex `int-to-char' instruction.
          if (in.IsRegister()) {
            __ movzxw(out.AsRegister<CpuRegister>(), in.AsRegister<CpuRegister>());
          } else if (in.IsStackSlot() || in.IsDoubleStackSlot()) {
            __ movzxw(out.AsRegister<CpuRegister>(),
                      Address(CpuRegister(RSP), in.GetStackIndex()));
          } else {
            __ movl(out.AsRegister<CpuRegister>(),
                    Immediate(static_cast<uint16_t>(Int64FromConstant(in.GetConstant()))));
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
          if (in.IsRegister()) {
            __ cvtsi2ss(out.AsFpuRegister<XmmRegister>(), in.AsRegister<CpuRegister>(), false);
          } else if (in.IsConstant()) {
            int32_t v = in.GetConstant()->AsIntConstant()->GetValue();
            XmmRegister dest = out.AsFpuRegister<XmmRegister>();
            codegen_->Load32BitValue(dest, static_cast<float>(v));
          } else {
            __ cvtsi2ss(out.AsFpuRegister<XmmRegister>(),
                        Address(CpuRegister(RSP), in.GetStackIndex()), false);
          }
          break;

        case Primitive::kPrimLong:
          // Processing a Dex `long-to-float' instruction.
          if (in.IsRegister()) {
            __ cvtsi2ss(out.AsFpuRegister<XmmRegister>(), in.AsRegister<CpuRegister>(), true);
          } else if (in.IsConstant()) {
            int64_t v = in.GetConstant()->AsLongConstant()->GetValue();
            XmmRegister dest = out.AsFpuRegister<XmmRegister>();
            codegen_->Load32BitValue(dest, static_cast<float>(v));
          } else {
            __ cvtsi2ss(out.AsFpuRegister<XmmRegister>(),
                        Address(CpuRegister(RSP), in.GetStackIndex()), true);
          }
          break;

        case Primitive::kPrimDouble:
          // Processing a Dex `double-to-float' instruction.
          if (in.IsFpuRegister()) {
            __ cvtsd2ss(out.AsFpuRegister<XmmRegister>(), in.AsFpuRegister<XmmRegister>());
          } else if (in.IsConstant()) {
            double v = in.GetConstant()->AsDoubleConstant()->GetValue();
            XmmRegister dest = out.AsFpuRegister<XmmRegister>();
            codegen_->Load32BitValue(dest, static_cast<float>(v));
          } else {
            __ cvtsd2ss(out.AsFpuRegister<XmmRegister>(),
                        Address(CpuRegister(RSP), in.GetStackIndex()));
          }
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
          if (in.IsRegister()) {
            __ cvtsi2sd(out.AsFpuRegister<XmmRegister>(), in.AsRegister<CpuRegister>(), false);
          } else if (in.IsConstant()) {
            int32_t v = in.GetConstant()->AsIntConstant()->GetValue();
            XmmRegister dest = out.AsFpuRegister<XmmRegister>();
            codegen_->Load64BitValue(dest, static_cast<double>(v));
          } else {
            __ cvtsi2sd(out.AsFpuRegister<XmmRegister>(),
                        Address(CpuRegister(RSP), in.GetStackIndex()), false);
          }
          break;

        case Primitive::kPrimLong:
          // Processing a Dex `long-to-double' instruction.
          if (in.IsRegister()) {
            __ cvtsi2sd(out.AsFpuRegister<XmmRegister>(), in.AsRegister<CpuRegister>(), true);
          } else if (in.IsConstant()) {
            int64_t v = in.GetConstant()->AsLongConstant()->GetValue();
            XmmRegister dest = out.AsFpuRegister<XmmRegister>();
            codegen_->Load64BitValue(dest, static_cast<double>(v));
          } else {
            __ cvtsi2sd(out.AsFpuRegister<XmmRegister>(),
                        Address(CpuRegister(RSP), in.GetStackIndex()), true);
          }
          break;

        case Primitive::kPrimFloat:
          // Processing a Dex `float-to-double' instruction.
          if (in.IsFpuRegister()) {
            __ cvtss2sd(out.AsFpuRegister<XmmRegister>(), in.AsFpuRegister<XmmRegister>());
          } else if (in.IsConstant()) {
            float v = in.GetConstant()->AsFloatConstant()->GetValue();
            XmmRegister dest = out.AsFpuRegister<XmmRegister>();
            codegen_->Load64BitValue(dest, static_cast<double>(v));
          } else {
            __ cvtss2sd(out.AsFpuRegister<XmmRegister>(),
                        Address(CpuRegister(RSP), in.GetStackIndex()));
          }
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

void LocationsBuilderX86_64::VisitAdd(HAdd* add) {
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
      // We can use a leaq or addq if the constant can fit in an immediate.
      locations->SetInAt(1, Location::RegisterOrInt32Constant(add->InputAt(1)));
      locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      break;
    }

    case Primitive::kPrimDouble:
    case Primitive::kPrimFloat: {
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetInAt(1, Location::Any());
      locations->SetOut(Location::SameAsFirstInput());
      break;
    }

    default:
      LOG(FATAL) << "Unexpected add type " << add->GetResultType();
  }
}

void InstructionCodeGeneratorX86_64::VisitAdd(HAdd* add) {
  LocationSummary* locations = add->GetLocations();
  Location first = locations->InAt(0);
  Location second = locations->InAt(1);
  Location out = locations->Out();

  switch (add->GetResultType()) {
    case Primitive::kPrimInt: {
      if (second.IsRegister()) {
        if (out.AsRegister<Register>() == first.AsRegister<Register>()) {
          __ addl(out.AsRegister<CpuRegister>(), second.AsRegister<CpuRegister>());
        } else if (out.AsRegister<Register>() == second.AsRegister<Register>()) {
          __ addl(out.AsRegister<CpuRegister>(), first.AsRegister<CpuRegister>());
        } else {
          __ leal(out.AsRegister<CpuRegister>(), Address(
              first.AsRegister<CpuRegister>(), second.AsRegister<CpuRegister>(), TIMES_1, 0));
        }
      } else if (second.IsConstant()) {
        if (out.AsRegister<Register>() == first.AsRegister<Register>()) {
          __ addl(out.AsRegister<CpuRegister>(),
                  Immediate(second.GetConstant()->AsIntConstant()->GetValue()));
        } else {
          __ leal(out.AsRegister<CpuRegister>(), Address(
              first.AsRegister<CpuRegister>(), second.GetConstant()->AsIntConstant()->GetValue()));
        }
      } else {
        DCHECK(first.Equals(locations->Out()));
        __ addl(first.AsRegister<CpuRegister>(), Address(CpuRegister(RSP), second.GetStackIndex()));
      }
      break;
    }

    case Primitive::kPrimLong: {
      if (second.IsRegister()) {
        if (out.AsRegister<Register>() == first.AsRegister<Register>()) {
          __ addq(out.AsRegister<CpuRegister>(), second.AsRegister<CpuRegister>());
        } else if (out.AsRegister<Register>() == second.AsRegister<Register>()) {
          __ addq(out.AsRegister<CpuRegister>(), first.AsRegister<CpuRegister>());
        } else {
          __ leaq(out.AsRegister<CpuRegister>(), Address(
              first.AsRegister<CpuRegister>(), second.AsRegister<CpuRegister>(), TIMES_1, 0));
        }
      } else {
        DCHECK(second.IsConstant());
        int64_t value = second.GetConstant()->AsLongConstant()->GetValue();
        int32_t int32_value = Low32Bits(value);
        DCHECK_EQ(int32_value, value);
        if (out.AsRegister<Register>() == first.AsRegister<Register>()) {
          __ addq(out.AsRegister<CpuRegister>(), Immediate(int32_value));
        } else {
          __ leaq(out.AsRegister<CpuRegister>(), Address(
              first.AsRegister<CpuRegister>(), int32_value));
        }
      }
      break;
    }

    case Primitive::kPrimFloat: {
      if (second.IsFpuRegister()) {
        __ addss(first.AsFpuRegister<XmmRegister>(), second.AsFpuRegister<XmmRegister>());
      } else if (second.IsConstant()) {
        __ addss(first.AsFpuRegister<XmmRegister>(),
                 codegen_->LiteralFloatAddress(
                     second.GetConstant()->AsFloatConstant()->GetValue()));
      } else {
        DCHECK(second.IsStackSlot());
        __ addss(first.AsFpuRegister<XmmRegister>(),
                 Address(CpuRegister(RSP), second.GetStackIndex()));
      }
      break;
    }

    case Primitive::kPrimDouble: {
      if (second.IsFpuRegister()) {
        __ addsd(first.AsFpuRegister<XmmRegister>(), second.AsFpuRegister<XmmRegister>());
      } else if (second.IsConstant()) {
        __ addsd(first.AsFpuRegister<XmmRegister>(),
                 codegen_->LiteralDoubleAddress(
                     second.GetConstant()->AsDoubleConstant()->GetValue()));
      } else {
        DCHECK(second.IsDoubleStackSlot());
        __ addsd(first.AsFpuRegister<XmmRegister>(),
                 Address(CpuRegister(RSP), second.GetStackIndex()));
      }
      break;
    }

    default:
      LOG(FATAL) << "Unexpected add type " << add->GetResultType();
  }
}

void LocationsBuilderX86_64::VisitSub(HSub* sub) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(sub, LocationSummary::kNoCall);
  switch (sub->GetResultType()) {
    case Primitive::kPrimInt: {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::Any());
      locations->SetOut(Location::SameAsFirstInput());
      break;
    }
    case Primitive::kPrimLong: {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RegisterOrInt32Constant(sub->InputAt(1)));
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
      LOG(FATAL) << "Unexpected sub type " << sub->GetResultType();
  }
}

void InstructionCodeGeneratorX86_64::VisitSub(HSub* sub) {
  LocationSummary* locations = sub->GetLocations();
  Location first = locations->InAt(0);
  Location second = locations->InAt(1);
  DCHECK(first.Equals(locations->Out()));
  switch (sub->GetResultType()) {
    case Primitive::kPrimInt: {
      if (second.IsRegister()) {
        __ subl(first.AsRegister<CpuRegister>(), second.AsRegister<CpuRegister>());
      } else if (second.IsConstant()) {
        Immediate imm(second.GetConstant()->AsIntConstant()->GetValue());
        __ subl(first.AsRegister<CpuRegister>(), imm);
      } else {
        __ subl(first.AsRegister<CpuRegister>(), Address(CpuRegister(RSP), second.GetStackIndex()));
      }
      break;
    }
    case Primitive::kPrimLong: {
      if (second.IsConstant()) {
        int64_t value = second.GetConstant()->AsLongConstant()->GetValue();
        DCHECK(IsInt<32>(value));
        __ subq(first.AsRegister<CpuRegister>(), Immediate(static_cast<int32_t>(value)));
      } else {
        __ subq(first.AsRegister<CpuRegister>(), second.AsRegister<CpuRegister>());
      }
      break;
    }

    case Primitive::kPrimFloat: {
      if (second.IsFpuRegister()) {
        __ subss(first.AsFpuRegister<XmmRegister>(), second.AsFpuRegister<XmmRegister>());
      } else if (second.IsConstant()) {
        __ subss(first.AsFpuRegister<XmmRegister>(),
                 codegen_->LiteralFloatAddress(
                     second.GetConstant()->AsFloatConstant()->GetValue()));
      } else {
        DCHECK(second.IsStackSlot());
        __ subss(first.AsFpuRegister<XmmRegister>(),
                 Address(CpuRegister(RSP), second.GetStackIndex()));
      }
      break;
    }

    case Primitive::kPrimDouble: {
      if (second.IsFpuRegister()) {
        __ subsd(first.AsFpuRegister<XmmRegister>(), second.AsFpuRegister<XmmRegister>());
      } else if (second.IsConstant()) {
        __ subsd(first.AsFpuRegister<XmmRegister>(),
                 codegen_->LiteralDoubleAddress(
                     second.GetConstant()->AsDoubleConstant()->GetValue()));
      } else {
        DCHECK(second.IsDoubleStackSlot());
        __ subsd(first.AsFpuRegister<XmmRegister>(),
                 Address(CpuRegister(RSP), second.GetStackIndex()));
      }
      break;
    }

    default:
      LOG(FATAL) << "Unexpected sub type " << sub->GetResultType();
  }
}

void LocationsBuilderX86_64::VisitMul(HMul* mul) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(mul, LocationSummary::kNoCall);
  switch (mul->GetResultType()) {
    case Primitive::kPrimInt: {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::Any());
      if (mul->InputAt(1)->IsIntConstant()) {
        // Can use 3 operand multiply.
        locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      } else {
        locations->SetOut(Location::SameAsFirstInput());
      }
      break;
    }
    case Primitive::kPrimLong: {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::Any());
      if (mul->InputAt(1)->IsLongConstant() &&
          IsInt<32>(mul->InputAt(1)->AsLongConstant()->GetValue())) {
        // Can use 3 operand multiply.
        locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      } else {
        locations->SetOut(Location::SameAsFirstInput());
      }
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
      LOG(FATAL) << "Unexpected mul type " << mul->GetResultType();
  }
}

void InstructionCodeGeneratorX86_64::VisitMul(HMul* mul) {
  LocationSummary* locations = mul->GetLocations();
  Location first = locations->InAt(0);
  Location second = locations->InAt(1);
  Location out = locations->Out();
  switch (mul->GetResultType()) {
    case Primitive::kPrimInt:
      // The constant may have ended up in a register, so test explicitly to avoid
      // problems where the output may not be the same as the first operand.
      if (mul->InputAt(1)->IsIntConstant()) {
        Immediate imm(mul->InputAt(1)->AsIntConstant()->GetValue());
        __ imull(out.AsRegister<CpuRegister>(), first.AsRegister<CpuRegister>(), imm);
      } else if (second.IsRegister()) {
        DCHECK(first.Equals(out));
        __ imull(first.AsRegister<CpuRegister>(), second.AsRegister<CpuRegister>());
      } else {
        DCHECK(first.Equals(out));
        DCHECK(second.IsStackSlot());
        __ imull(first.AsRegister<CpuRegister>(),
                 Address(CpuRegister(RSP), second.GetStackIndex()));
      }
      break;
    case Primitive::kPrimLong: {
      // The constant may have ended up in a register, so test explicitly to avoid
      // problems where the output may not be the same as the first operand.
      if (mul->InputAt(1)->IsLongConstant()) {
        int64_t value = mul->InputAt(1)->AsLongConstant()->GetValue();
        if (IsInt<32>(value)) {
          __ imulq(out.AsRegister<CpuRegister>(), first.AsRegister<CpuRegister>(),
                   Immediate(static_cast<int32_t>(value)));
        } else {
          // Have to use the constant area.
          DCHECK(first.Equals(out));
          __ imulq(first.AsRegister<CpuRegister>(), codegen_->LiteralInt64Address(value));
        }
      } else if (second.IsRegister()) {
        DCHECK(first.Equals(out));
        __ imulq(first.AsRegister<CpuRegister>(), second.AsRegister<CpuRegister>());
      } else {
        DCHECK(second.IsDoubleStackSlot());
        DCHECK(first.Equals(out));
        __ imulq(first.AsRegister<CpuRegister>(),
                 Address(CpuRegister(RSP), second.GetStackIndex()));
      }
      break;
    }

    case Primitive::kPrimFloat: {
      DCHECK(first.Equals(out));
      if (second.IsFpuRegister()) {
        __ mulss(first.AsFpuRegister<XmmRegister>(), second.AsFpuRegister<XmmRegister>());
      } else if (second.IsConstant()) {
        __ mulss(first.AsFpuRegister<XmmRegister>(),
                 codegen_->LiteralFloatAddress(
                     second.GetConstant()->AsFloatConstant()->GetValue()));
      } else {
        DCHECK(second.IsStackSlot());
        __ mulss(first.AsFpuRegister<XmmRegister>(),
                 Address(CpuRegister(RSP), second.GetStackIndex()));
      }
      break;
    }

    case Primitive::kPrimDouble: {
      DCHECK(first.Equals(out));
      if (second.IsFpuRegister()) {
        __ mulsd(first.AsFpuRegister<XmmRegister>(), second.AsFpuRegister<XmmRegister>());
      } else if (second.IsConstant()) {
        __ mulsd(first.AsFpuRegister<XmmRegister>(),
                 codegen_->LiteralDoubleAddress(
                     second.GetConstant()->AsDoubleConstant()->GetValue()));
      } else {
        DCHECK(second.IsDoubleStackSlot());
        __ mulsd(first.AsFpuRegister<XmmRegister>(),
                 Address(CpuRegister(RSP), second.GetStackIndex()));
      }
      break;
    }

    default:
      LOG(FATAL) << "Unexpected mul type " << mul->GetResultType();
  }
}

void InstructionCodeGeneratorX86_64::PushOntoFPStack(Location source, uint32_t temp_offset,
                                                     uint32_t stack_adjustment, bool is_float) {
  if (source.IsStackSlot()) {
    DCHECK(is_float);
    __ flds(Address(CpuRegister(RSP), source.GetStackIndex() + stack_adjustment));
  } else if (source.IsDoubleStackSlot()) {
    DCHECK(!is_float);
    __ fldl(Address(CpuRegister(RSP), source.GetStackIndex() + stack_adjustment));
  } else {
    // Write the value to the temporary location on the stack and load to FP stack.
    if (is_float) {
      Location stack_temp = Location::StackSlot(temp_offset);
      codegen_->Move(stack_temp, source);
      __ flds(Address(CpuRegister(RSP), temp_offset));
    } else {
      Location stack_temp = Location::DoubleStackSlot(temp_offset);
      codegen_->Move(stack_temp, source);
      __ fldl(Address(CpuRegister(RSP), temp_offset));
    }
  }
}

void InstructionCodeGeneratorX86_64::GenerateRemFP(HRem *rem) {
  Primitive::Type type = rem->GetResultType();
  bool is_float = type == Primitive::kPrimFloat;
  size_t elem_size = Primitive::ComponentSize(type);
  LocationSummary* locations = rem->GetLocations();
  Location first = locations->InAt(0);
  Location second = locations->InAt(1);
  Location out = locations->Out();

  // Create stack space for 2 elements.
  // TODO: enhance register allocator to ask for stack temporaries.
  __ subq(CpuRegister(RSP), Immediate(2 * elem_size));

  // Load the values to the FP stack in reverse order, using temporaries if needed.
  PushOntoFPStack(second, elem_size, 2 * elem_size, is_float);
  PushOntoFPStack(first, 0, 2 * elem_size, is_float);

  // Loop doing FPREM until we stabilize.
  NearLabel retry;
  __ Bind(&retry);
  __ fprem();

  // Move FP status to AX.
  __ fstsw();

  // And see if the argument reduction is complete. This is signaled by the
  // C2 FPU flag bit set to 0.
  __ andl(CpuRegister(RAX), Immediate(kC2ConditionMask));
  __ j(kNotEqual, &retry);

  // We have settled on the final value. Retrieve it into an XMM register.
  // Store FP top of stack to real stack.
  if (is_float) {
    __ fsts(Address(CpuRegister(RSP), 0));
  } else {
    __ fstl(Address(CpuRegister(RSP), 0));
  }

  // Pop the 2 items from the FP stack.
  __ fucompp();

  // Load the value from the stack into an XMM register.
  DCHECK(out.IsFpuRegister()) << out;
  if (is_float) {
    __ movss(out.AsFpuRegister<XmmRegister>(), Address(CpuRegister(RSP), 0));
  } else {
    __ movsd(out.AsFpuRegister<XmmRegister>(), Address(CpuRegister(RSP), 0));
  }

  // And remove the temporary stack space we allocated.
  __ addq(CpuRegister(RSP), Immediate(2 * elem_size));
}

void InstructionCodeGeneratorX86_64::DivRemOneOrMinusOne(HBinaryOperation* instruction) {
  DCHECK(instruction->IsDiv() || instruction->IsRem());

  LocationSummary* locations = instruction->GetLocations();
  Location second = locations->InAt(1);
  DCHECK(second.IsConstant());

  CpuRegister output_register = locations->Out().AsRegister<CpuRegister>();
  CpuRegister input_register = locations->InAt(0).AsRegister<CpuRegister>();
  int64_t imm = Int64FromConstant(second.GetConstant());

  DCHECK(imm == 1 || imm == -1);

  switch (instruction->GetResultType()) {
    case Primitive::kPrimInt: {
      if (instruction->IsRem()) {
        __ xorl(output_register, output_register);
      } else {
        __ movl(output_register, input_register);
        if (imm == -1) {
          __ negl(output_register);
        }
      }
      break;
    }

    case Primitive::kPrimLong: {
      if (instruction->IsRem()) {
        __ xorl(output_register, output_register);
      } else {
        __ movq(output_register, input_register);
        if (imm == -1) {
          __ negq(output_register);
        }
      }
      break;
    }

    default:
      LOG(FATAL) << "Unexpected type for div by (-)1 " << instruction->GetResultType();
  }
}

void InstructionCodeGeneratorX86_64::DivByPowerOfTwo(HDiv* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Location second = locations->InAt(1);

  CpuRegister output_register = locations->Out().AsRegister<CpuRegister>();
  CpuRegister numerator = locations->InAt(0).AsRegister<CpuRegister>();

  int64_t imm = Int64FromConstant(second.GetConstant());
  DCHECK(IsPowerOfTwo(AbsOrMin(imm)));
  uint64_t abs_imm = AbsOrMin(imm);

  CpuRegister tmp = locations->GetTemp(0).AsRegister<CpuRegister>();

  if (instruction->GetResultType() == Primitive::kPrimInt) {
    __ leal(tmp, Address(numerator, abs_imm - 1));
    __ testl(numerator, numerator);
    __ cmov(kGreaterEqual, tmp, numerator);
    int shift = CTZ(imm);
    __ sarl(tmp, Immediate(shift));

    if (imm < 0) {
      __ negl(tmp);
    }

    __ movl(output_register, tmp);
  } else {
    DCHECK_EQ(instruction->GetResultType(), Primitive::kPrimLong);
    CpuRegister rdx = locations->GetTemp(0).AsRegister<CpuRegister>();

    codegen_->Load64BitValue(rdx, abs_imm - 1);
    __ addq(rdx, numerator);
    __ testq(numerator, numerator);
    __ cmov(kGreaterEqual, rdx, numerator);
    int shift = CTZ(imm);
    __ sarq(rdx, Immediate(shift));

    if (imm < 0) {
      __ negq(rdx);
    }

    __ movq(output_register, rdx);
  }
}

void InstructionCodeGeneratorX86_64::GenerateDivRemWithAnyConstant(HBinaryOperation* instruction) {
  DCHECK(instruction->IsDiv() || instruction->IsRem());

  LocationSummary* locations = instruction->GetLocations();
  Location second = locations->InAt(1);

  CpuRegister numerator = instruction->IsDiv() ? locations->GetTemp(1).AsRegister<CpuRegister>()
      : locations->GetTemp(0).AsRegister<CpuRegister>();
  CpuRegister eax = locations->InAt(0).AsRegister<CpuRegister>();
  CpuRegister edx = instruction->IsDiv() ? locations->GetTemp(0).AsRegister<CpuRegister>()
      : locations->Out().AsRegister<CpuRegister>();
  CpuRegister out = locations->Out().AsRegister<CpuRegister>();

  DCHECK_EQ(RAX, eax.AsRegister());
  DCHECK_EQ(RDX, edx.AsRegister());
  if (instruction->IsDiv()) {
    DCHECK_EQ(RAX, out.AsRegister());
  } else {
    DCHECK_EQ(RDX, out.AsRegister());
  }

  int64_t magic;
  int shift;

  // TODO: can these branches be written as one?
  if (instruction->GetResultType() == Primitive::kPrimInt) {
    int imm = second.GetConstant()->AsIntConstant()->GetValue();

    CalculateMagicAndShiftForDivRem(imm, false /* is_long */, &magic, &shift);

    __ movl(numerator, eax);

    NearLabel no_div;
    NearLabel end;
    __ testl(eax, eax);
    __ j(kNotEqual, &no_div);

    __ xorl(out, out);
    __ jmp(&end);

    __ Bind(&no_div);

    __ movl(eax, Immediate(magic));
    __ imull(numerator);

    if (imm > 0 && magic < 0) {
      __ addl(edx, numerator);
    } else if (imm < 0 && magic > 0) {
      __ subl(edx, numerator);
    }

    if (shift != 0) {
      __ sarl(edx, Immediate(shift));
    }

    __ movl(eax, edx);
    __ shrl(edx, Immediate(31));
    __ addl(edx, eax);

    if (instruction->IsRem()) {
      __ movl(eax, numerator);
      __ imull(edx, Immediate(imm));
      __ subl(eax, edx);
      __ movl(edx, eax);
    } else {
      __ movl(eax, edx);
    }
    __ Bind(&end);
  } else {
    int64_t imm = second.GetConstant()->AsLongConstant()->GetValue();

    DCHECK_EQ(instruction->GetResultType(), Primitive::kPrimLong);

    CpuRegister rax = eax;
    CpuRegister rdx = edx;

    CalculateMagicAndShiftForDivRem(imm, true /* is_long */, &magic, &shift);

    // Save the numerator.
    __ movq(numerator, rax);

    // RAX = magic
    codegen_->Load64BitValue(rax, magic);

    // RDX:RAX = magic * numerator
    __ imulq(numerator);

    if (imm > 0 && magic < 0) {
      // RDX += numerator
      __ addq(rdx, numerator);
    } else if (imm < 0 && magic > 0) {
      // RDX -= numerator
      __ subq(rdx, numerator);
    }

    // Shift if needed.
    if (shift != 0) {
      __ sarq(rdx, Immediate(shift));
    }

    // RDX += 1 if RDX < 0
    __ movq(rax, rdx);
    __ shrq(rdx, Immediate(63));
    __ addq(rdx, rax);

    if (instruction->IsRem()) {
      __ movq(rax, numerator);

      if (IsInt<32>(imm)) {
        __ imulq(rdx, Immediate(static_cast<int32_t>(imm)));
      } else {
        __ imulq(rdx, codegen_->LiteralInt64Address(imm));
      }

      __ subq(rax, rdx);
      __ movq(rdx, rax);
    } else {
      __ movq(rax, rdx);
    }
  }
}

void InstructionCodeGeneratorX86_64::GenerateDivRemIntegral(HBinaryOperation* instruction) {
  DCHECK(instruction->IsDiv() || instruction->IsRem());
  Primitive::Type type = instruction->GetResultType();
  DCHECK(type == Primitive::kPrimInt || Primitive::kPrimLong);

  bool is_div = instruction->IsDiv();
  LocationSummary* locations = instruction->GetLocations();

  CpuRegister out = locations->Out().AsRegister<CpuRegister>();
  Location second = locations->InAt(1);

  DCHECK_EQ(RAX, locations->InAt(0).AsRegister<CpuRegister>().AsRegister());
  DCHECK_EQ(is_div ? RAX : RDX, out.AsRegister());

  if (second.IsConstant()) {
    int64_t imm = Int64FromConstant(second.GetConstant());

    if (imm == 0) {
      // Do not generate anything. DivZeroCheck would prevent any code to be executed.
    } else if (imm == 1 || imm == -1) {
      DivRemOneOrMinusOne(instruction);
    } else if (instruction->IsDiv() && IsPowerOfTwo(AbsOrMin(imm))) {
      DivByPowerOfTwo(instruction->AsDiv());
    } else {
      DCHECK(imm <= -2 || imm >= 2);
      GenerateDivRemWithAnyConstant(instruction);
    }
  } else {
    SlowPathCode* slow_path =
        new (GetGraph()->GetArena()) DivRemMinusOneSlowPathX86_64(
            instruction, out.AsRegister(), type, is_div);
    codegen_->AddSlowPath(slow_path);

    CpuRegister second_reg = second.AsRegister<CpuRegister>();
    // 0x80000000(00000000)/-1 triggers an arithmetic exception!
    // Dividing by -1 is actually negation and -0x800000000(00000000) = 0x80000000(00000000)
    // so it's safe to just use negl instead of more complex comparisons.
    if (type == Primitive::kPrimInt) {
      __ cmpl(second_reg, Immediate(-1));
      __ j(kEqual, slow_path->GetEntryLabel());
      // edx:eax <- sign-extended of eax
      __ cdq();
      // eax = quotient, edx = remainder
      __ idivl(second_reg);
    } else {
      __ cmpq(second_reg, Immediate(-1));
      __ j(kEqual, slow_path->GetEntryLabel());
      // rdx:rax <- sign-extended of rax
      __ cqo();
      // rax = quotient, rdx = remainder
      __ idivq(second_reg);
    }
    __ Bind(slow_path->GetExitLabel());
  }
}

void LocationsBuilderX86_64::VisitDiv(HDiv* div) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(div, LocationSummary::kNoCall);
  switch (div->GetResultType()) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong: {
      locations->SetInAt(0, Location::RegisterLocation(RAX));
      locations->SetInAt(1, Location::RegisterOrConstant(div->InputAt(1)));
      locations->SetOut(Location::SameAsFirstInput());
      // Intel uses edx:eax as the dividend.
      locations->AddTemp(Location::RegisterLocation(RDX));
      // We need to save the numerator while we tweak rax and rdx. As we are using imul in a way
      // which enforces results to be in RAX and RDX, things are simpler if we use RDX also as
      // output and request another temp.
      if (div->InputAt(1)->IsConstant()) {
        locations->AddTemp(Location::RequiresRegister());
      }
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
      LOG(FATAL) << "Unexpected div type " << div->GetResultType();
  }
}

void InstructionCodeGeneratorX86_64::VisitDiv(HDiv* div) {
  LocationSummary* locations = div->GetLocations();
  Location first = locations->InAt(0);
  Location second = locations->InAt(1);
  DCHECK(first.Equals(locations->Out()));

  Primitive::Type type = div->GetResultType();
  switch (type) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong: {
      GenerateDivRemIntegral(div);
      break;
    }

    case Primitive::kPrimFloat: {
      if (second.IsFpuRegister()) {
        __ divss(first.AsFpuRegister<XmmRegister>(), second.AsFpuRegister<XmmRegister>());
      } else if (second.IsConstant()) {
        __ divss(first.AsFpuRegister<XmmRegister>(),
                 codegen_->LiteralFloatAddress(
                     second.GetConstant()->AsFloatConstant()->GetValue()));
      } else {
        DCHECK(second.IsStackSlot());
        __ divss(first.AsFpuRegister<XmmRegister>(),
                 Address(CpuRegister(RSP), second.GetStackIndex()));
      }
      break;
    }

    case Primitive::kPrimDouble: {
      if (second.IsFpuRegister()) {
        __ divsd(first.AsFpuRegister<XmmRegister>(), second.AsFpuRegister<XmmRegister>());
      } else if (second.IsConstant()) {
        __ divsd(first.AsFpuRegister<XmmRegister>(),
                 codegen_->LiteralDoubleAddress(
                     second.GetConstant()->AsDoubleConstant()->GetValue()));
      } else {
        DCHECK(second.IsDoubleStackSlot());
        __ divsd(first.AsFpuRegister<XmmRegister>(),
                 Address(CpuRegister(RSP), second.GetStackIndex()));
      }
      break;
    }

    default:
      LOG(FATAL) << "Unexpected div type " << div->GetResultType();
  }
}

void LocationsBuilderX86_64::VisitRem(HRem* rem) {
  Primitive::Type type = rem->GetResultType();
  LocationSummary* locations =
    new (GetGraph()->GetArena()) LocationSummary(rem, LocationSummary::kNoCall);

  switch (type) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong: {
      locations->SetInAt(0, Location::RegisterLocation(RAX));
      locations->SetInAt(1, Location::RegisterOrConstant(rem->InputAt(1)));
      // Intel uses rdx:rax as the dividend and puts the remainder in rdx
      locations->SetOut(Location::RegisterLocation(RDX));
      // We need to save the numerator while we tweak eax and edx. As we are using imul in a way
      // which enforces results to be in RAX and RDX, things are simpler if we use EAX also as
      // output and request another temp.
      if (rem->InputAt(1)->IsConstant()) {
        locations->AddTemp(Location::RequiresRegister());
      }
      break;
    }

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      locations->SetInAt(0, Location::Any());
      locations->SetInAt(1, Location::Any());
      locations->SetOut(Location::RequiresFpuRegister());
      locations->AddTemp(Location::RegisterLocation(RAX));
      break;
    }

    default:
      LOG(FATAL) << "Unexpected rem type " << type;
  }
}

void InstructionCodeGeneratorX86_64::VisitRem(HRem* rem) {
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
      LOG(FATAL) << "Unexpected rem type " << rem->GetResultType();
  }
}

void LocationsBuilderX86_64::VisitDivZeroCheck(HDivZeroCheck* instruction) {
  LocationSummary::CallKind call_kind = instruction->CanThrowIntoCatchBlock()
      ? LocationSummary::kCallOnSlowPath
      : LocationSummary::kNoCall;
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction, call_kind);
  locations->SetInAt(0, Location::Any());
  if (instruction->HasUses()) {
    locations->SetOut(Location::SameAsFirstInput());
  }
}

void InstructionCodeGeneratorX86_64::VisitDivZeroCheck(HDivZeroCheck* instruction) {
  SlowPathCode* slow_path =
      new (GetGraph()->GetArena()) DivZeroCheckSlowPathX86_64(instruction);
  codegen_->AddSlowPath(slow_path);

  LocationSummary* locations = instruction->GetLocations();
  Location value = locations->InAt(0);

  switch (instruction->GetType()) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimInt: {
      if (value.IsRegister()) {
        __ testl(value.AsRegister<CpuRegister>(), value.AsRegister<CpuRegister>());
        __ j(kEqual, slow_path->GetEntryLabel());
      } else if (value.IsStackSlot()) {
        __ cmpl(Address(CpuRegister(RSP), value.GetStackIndex()), Immediate(0));
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
      if (value.IsRegister()) {
        __ testq(value.AsRegister<CpuRegister>(), value.AsRegister<CpuRegister>());
        __ j(kEqual, slow_path->GetEntryLabel());
      } else if (value.IsDoubleStackSlot()) {
        __ cmpq(Address(CpuRegister(RSP), value.GetStackIndex()), Immediate(0));
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
      LOG(FATAL) << "Unexpected type for HDivZeroCheck " << instruction->GetType();
  }
}

void LocationsBuilderX86_64::HandleShift(HBinaryOperation* op) {
  DCHECK(op->IsShl() || op->IsShr() || op->IsUShr());

  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(op, LocationSummary::kNoCall);

  switch (op->GetResultType()) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong: {
      locations->SetInAt(0, Location::RequiresRegister());
      // The shift count needs to be in CL.
      locations->SetInAt(1, Location::ByteRegisterOrConstant(RCX, op->InputAt(1)));
      locations->SetOut(Location::SameAsFirstInput());
      break;
    }
    default:
      LOG(FATAL) << "Unexpected operation type " << op->GetResultType();
  }
}

void InstructionCodeGeneratorX86_64::HandleShift(HBinaryOperation* op) {
  DCHECK(op->IsShl() || op->IsShr() || op->IsUShr());

  LocationSummary* locations = op->GetLocations();
  CpuRegister first_reg = locations->InAt(0).AsRegister<CpuRegister>();
  Location second = locations->InAt(1);

  switch (op->GetResultType()) {
    case Primitive::kPrimInt: {
      if (second.IsRegister()) {
        CpuRegister second_reg = second.AsRegister<CpuRegister>();
        if (op->IsShl()) {
          __ shll(first_reg, second_reg);
        } else if (op->IsShr()) {
          __ sarl(first_reg, second_reg);
        } else {
          __ shrl(first_reg, second_reg);
        }
      } else {
        Immediate imm(second.GetConstant()->AsIntConstant()->GetValue() & kMaxIntShiftDistance);
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
        CpuRegister second_reg = second.AsRegister<CpuRegister>();
        if (op->IsShl()) {
          __ shlq(first_reg, second_reg);
        } else if (op->IsShr()) {
          __ sarq(first_reg, second_reg);
        } else {
          __ shrq(first_reg, second_reg);
        }
      } else {
        Immediate imm(second.GetConstant()->AsIntConstant()->GetValue() & kMaxLongShiftDistance);
        if (op->IsShl()) {
          __ shlq(first_reg, imm);
        } else if (op->IsShr()) {
          __ sarq(first_reg, imm);
        } else {
          __ shrq(first_reg, imm);
        }
      }
      break;
    }
    default:
      LOG(FATAL) << "Unexpected operation type " << op->GetResultType();
      UNREACHABLE();
  }
}

void LocationsBuilderX86_64::VisitRor(HRor* ror) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(ror, LocationSummary::kNoCall);

  switch (ror->GetResultType()) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong: {
      locations->SetInAt(0, Location::RequiresRegister());
      // The shift count needs to be in CL (unless it is a constant).
      locations->SetInAt(1, Location::ByteRegisterOrConstant(RCX, ror->InputAt(1)));
      locations->SetOut(Location::SameAsFirstInput());
      break;
    }
    default:
      LOG(FATAL) << "Unexpected operation type " << ror->GetResultType();
      UNREACHABLE();
  }
}

void InstructionCodeGeneratorX86_64::VisitRor(HRor* ror) {
  LocationSummary* locations = ror->GetLocations();
  CpuRegister first_reg = locations->InAt(0).AsRegister<CpuRegister>();
  Location second = locations->InAt(1);

  switch (ror->GetResultType()) {
    case Primitive::kPrimInt:
      if (second.IsRegister()) {
        CpuRegister second_reg = second.AsRegister<CpuRegister>();
        __ rorl(first_reg, second_reg);
      } else {
        Immediate imm(second.GetConstant()->AsIntConstant()->GetValue() & kMaxIntShiftDistance);
        __ rorl(first_reg, imm);
      }
      break;
    case Primitive::kPrimLong:
      if (second.IsRegister()) {
        CpuRegister second_reg = second.AsRegister<CpuRegister>();
        __ rorq(first_reg, second_reg);
      } else {
        Immediate imm(second.GetConstant()->AsIntConstant()->GetValue() & kMaxLongShiftDistance);
        __ rorq(first_reg, imm);
      }
      break;
    default:
      LOG(FATAL) << "Unexpected operation type " << ror->GetResultType();
      UNREACHABLE();
  }
}

void LocationsBuilderX86_64::VisitShl(HShl* shl) {
  HandleShift(shl);
}

void InstructionCodeGeneratorX86_64::VisitShl(HShl* shl) {
  HandleShift(shl);
}

void LocationsBuilderX86_64::VisitShr(HShr* shr) {
  HandleShift(shr);
}

void InstructionCodeGeneratorX86_64::VisitShr(HShr* shr) {
  HandleShift(shr);
}

void LocationsBuilderX86_64::VisitUShr(HUShr* ushr) {
  HandleShift(ushr);
}

void InstructionCodeGeneratorX86_64::VisitUShr(HUShr* ushr) {
  HandleShift(ushr);
}

void LocationsBuilderX86_64::VisitNewInstance(HNewInstance* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCall);
  InvokeRuntimeCallingConvention calling_convention;
  if (instruction->IsStringAlloc()) {
    locations->AddTemp(Location::RegisterLocation(kMethodRegisterArgument));
  } else {
    locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
    locations->SetInAt(1, Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
  }
  locations->SetOut(Location::RegisterLocation(RAX));
}

void InstructionCodeGeneratorX86_64::VisitNewInstance(HNewInstance* instruction) {
  // Note: if heap poisoning is enabled, the entry point takes cares
  // of poisoning the reference.
  if (instruction->IsStringAlloc()) {
    // String is allocated through StringFactory. Call NewEmptyString entry point.
    CpuRegister temp = instruction->GetLocations()->GetTemp(0).AsRegister<CpuRegister>();
    MemberOffset code_offset = ArtMethod::EntryPointFromQuickCompiledCodeOffset(kX86_64WordSize);
    __ gs()->movq(temp, Address::Absolute(QUICK_ENTRY_POINT(pNewEmptyString), /* no_rip */ true));
    __ call(Address(temp, code_offset.SizeValue()));
    codegen_->RecordPcInfo(instruction, instruction->GetDexPc());
  } else {
    codegen_->InvokeRuntime(instruction->GetEntrypoint(),
                            instruction,
                            instruction->GetDexPc(),
                            nullptr);
    CheckEntrypointTypes<kQuickAllocObjectWithAccessCheck, void*, uint32_t, ArtMethod*>();
    DCHECK(!codegen_->IsLeafMethod());
  }
}

void LocationsBuilderX86_64::VisitNewArray(HNewArray* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCall);
  InvokeRuntimeCallingConvention calling_convention;
  locations->AddTemp(Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
  locations->SetOut(Location::RegisterLocation(RAX));
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
  locations->SetInAt(1, Location::RegisterLocation(calling_convention.GetRegisterAt(2)));
}

void InstructionCodeGeneratorX86_64::VisitNewArray(HNewArray* instruction) {
  InvokeRuntimeCallingConvention calling_convention;
  codegen_->Load64BitValue(CpuRegister(calling_convention.GetRegisterAt(0)),
                           instruction->GetTypeIndex());
  // Note: if heap poisoning is enabled, the entry point takes cares
  // of poisoning the reference.
  codegen_->InvokeRuntime(instruction->GetEntrypoint(),
                          instruction,
                          instruction->GetDexPc(),
                          nullptr);
  CheckEntrypointTypes<kQuickAllocArrayWithAccessCheck, void*, uint32_t, int32_t, ArtMethod*>();

  DCHECK(!codegen_->IsLeafMethod());
}

void LocationsBuilderX86_64::VisitParameterValue(HParameterValue* instruction) {
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

void InstructionCodeGeneratorX86_64::VisitParameterValue(
    HParameterValue* instruction ATTRIBUTE_UNUSED) {
  // Nothing to do, the parameter is already at its location.
}

void LocationsBuilderX86_64::VisitCurrentMethod(HCurrentMethod* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetOut(Location::RegisterLocation(kMethodRegisterArgument));
}

void InstructionCodeGeneratorX86_64::VisitCurrentMethod(
    HCurrentMethod* instruction ATTRIBUTE_UNUSED) {
  // Nothing to do, the method is already at its location.
}

void LocationsBuilderX86_64::VisitClassTableGet(HClassTableGet* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorX86_64::VisitClassTableGet(HClassTableGet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  uint32_t method_offset = 0;
  if (instruction->GetTableKind() == HClassTableGet::TableKind::kVTable) {
    method_offset = mirror::Class::EmbeddedVTableEntryOffset(
        instruction->GetIndex(), kX86_64PointerSize).SizeValue();
  } else {
    method_offset = mirror::Class::EmbeddedImTableEntryOffset(
        instruction->GetIndex() % mirror::Class::kImtSize, kX86_64PointerSize).Uint32Value();
  }
  __ movq(locations->Out().AsRegister<CpuRegister>(),
          Address(locations->InAt(0).AsRegister<CpuRegister>(), method_offset));
}

void LocationsBuilderX86_64::VisitNot(HNot* not_) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(not_, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::SameAsFirstInput());
}

void InstructionCodeGeneratorX86_64::VisitNot(HNot* not_) {
  LocationSummary* locations = not_->GetLocations();
  DCHECK_EQ(locations->InAt(0).AsRegister<CpuRegister>().AsRegister(),
            locations->Out().AsRegister<CpuRegister>().AsRegister());
  Location out = locations->Out();
  switch (not_->GetResultType()) {
    case Primitive::kPrimInt:
      __ notl(out.AsRegister<CpuRegister>());
      break;

    case Primitive::kPrimLong:
      __ notq(out.AsRegister<CpuRegister>());
      break;

    default:
      LOG(FATAL) << "Unimplemented type for not operation " << not_->GetResultType();
  }
}

void LocationsBuilderX86_64::VisitBooleanNot(HBooleanNot* bool_not) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(bool_not, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::SameAsFirstInput());
}

void InstructionCodeGeneratorX86_64::VisitBooleanNot(HBooleanNot* bool_not) {
  LocationSummary* locations = bool_not->GetLocations();
  DCHECK_EQ(locations->InAt(0).AsRegister<CpuRegister>().AsRegister(),
            locations->Out().AsRegister<CpuRegister>().AsRegister());
  Location out = locations->Out();
  __ xorl(out.AsRegister<CpuRegister>(), Immediate(1));
}

void LocationsBuilderX86_64::VisitPhi(HPhi* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  for (size_t i = 0, e = instruction->InputCount(); i < e; ++i) {
    locations->SetInAt(i, Location::Any());
  }
  locations->SetOut(Location::Any());
}

void InstructionCodeGeneratorX86_64::VisitPhi(HPhi* instruction ATTRIBUTE_UNUSED) {
  LOG(FATAL) << "Unimplemented";
}

void CodeGeneratorX86_64::GenerateMemoryBarrier(MemBarrierKind kind) {
  /*
   * According to the JSR-133 Cookbook, for x86 only StoreLoad/AnyAny barriers need memory fence.
   * All other barriers (LoadAny, AnyStore, StoreStore) are nops due to the x86-64 memory model.
   * For those cases, all we need to ensure is that there is a scheduling barrier in place.
   */
  switch (kind) {
    case MemBarrierKind::kAnyAny: {
      MemoryFence();
      break;
    }
    case MemBarrierKind::kAnyStore:
    case MemBarrierKind::kLoadAny:
    case MemBarrierKind::kStoreStore: {
      // nop
      break;
    }
    default:
      LOG(FATAL) << "Unexpected memory barier " << kind;
  }
}

void LocationsBuilderX86_64::HandleFieldGet(HInstruction* instruction) {
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
    // are enabled: we do not want the move to overwrite the object's
    // location, as we need it to emit the read barrier.
    locations->SetOut(
        Location::RequiresRegister(),
        object_field_get_with_read_barrier ? Location::kOutputOverlap : Location::kNoOutputOverlap);
  }
  if (object_field_get_with_read_barrier && kUseBakerReadBarrier) {
    // We need a temporary register for the read barrier marking slow
    // path in CodeGeneratorX86_64::GenerateFieldLoadWithBakerReadBarrier.
    locations->AddTemp(Location::RequiresRegister());
  }
}

void InstructionCodeGeneratorX86_64::HandleFieldGet(HInstruction* instruction,
                                                    const FieldInfo& field_info) {
  DCHECK(instruction->IsInstanceFieldGet() || instruction->IsStaticFieldGet());

  LocationSummary* locations = instruction->GetLocations();
  Location base_loc = locations->InAt(0);
  CpuRegister base = base_loc.AsRegister<CpuRegister>();
  Location out = locations->Out();
  bool is_volatile = field_info.IsVolatile();
  Primitive::Type field_type = field_info.GetFieldType();
  uint32_t offset = field_info.GetFieldOffset().Uint32Value();

  switch (field_type) {
    case Primitive::kPrimBoolean: {
      __ movzxb(out.AsRegister<CpuRegister>(), Address(base, offset));
      break;
    }

    case Primitive::kPrimByte: {
      __ movsxb(out.AsRegister<CpuRegister>(), Address(base, offset));
      break;
    }

    case Primitive::kPrimShort: {
      __ movsxw(out.AsRegister<CpuRegister>(), Address(base, offset));
      break;
    }

    case Primitive::kPrimChar: {
      __ movzxw(out.AsRegister<CpuRegister>(), Address(base, offset));
      break;
    }

    case Primitive::kPrimInt: {
      __ movl(out.AsRegister<CpuRegister>(), Address(base, offset));
      break;
    }

    case Primitive::kPrimNot: {
      // /* HeapReference<Object> */ out = *(base + offset)
      if (kEmitCompilerReadBarrier && kUseBakerReadBarrier) {
        Location temp_loc = locations->GetTemp(0);
        // Note that a potential implicit null check is handled in this
        // CodeGeneratorX86::GenerateFieldLoadWithBakerReadBarrier call.
        codegen_->GenerateFieldLoadWithBakerReadBarrier(
            instruction, out, base, offset, temp_loc, /* needs_null_check */ true);
        if (is_volatile) {
          codegen_->GenerateMemoryBarrier(MemBarrierKind::kLoadAny);
        }
      } else {
        __ movl(out.AsRegister<CpuRegister>(), Address(base, offset));
        codegen_->MaybeRecordImplicitNullCheck(instruction);
        if (is_volatile) {
          codegen_->GenerateMemoryBarrier(MemBarrierKind::kLoadAny);
        }
        // If read barriers are enabled, emit read barriers other than
        // Baker's using a slow path (and also unpoison the loaded
        // reference, if heap poisoning is enabled).
        codegen_->MaybeGenerateReadBarrierSlow(instruction, out, out, base_loc, offset);
      }
      break;
    }

    case Primitive::kPrimLong: {
      __ movq(out.AsRegister<CpuRegister>(), Address(base, offset));
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

  if (field_type == Primitive::kPrimNot) {
    // Potential implicit null checks, in the case of reference
    // fields, are handled in the previous switch statement.
  } else {
    codegen_->MaybeRecordImplicitNullCheck(instruction);
  }

  if (is_volatile) {
    if (field_type == Primitive::kPrimNot) {
      // Memory barriers, in the case of references, are also handled
      // in the previous switch statement.
    } else {
      codegen_->GenerateMemoryBarrier(MemBarrierKind::kLoadAny);
    }
  }
}

void LocationsBuilderX86_64::HandleFieldSet(HInstruction* instruction,
                                            const FieldInfo& field_info) {
  DCHECK(instruction->IsInstanceFieldSet() || instruction->IsStaticFieldSet());

  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  Primitive::Type field_type = field_info.GetFieldType();
  bool is_volatile = field_info.IsVolatile();
  bool needs_write_barrier =
      CodeGenerator::StoreNeedsWriteBarrier(field_type, instruction->InputAt(1));

  locations->SetInAt(0, Location::RequiresRegister());
  if (Primitive::IsFloatingPointType(instruction->InputAt(1)->GetType())) {
    if (is_volatile) {
      // In order to satisfy the semantics of volatile, this must be a single instruction store.
      locations->SetInAt(1, Location::FpuRegisterOrInt32Constant(instruction->InputAt(1)));
    } else {
      locations->SetInAt(1, Location::FpuRegisterOrConstant(instruction->InputAt(1)));
    }
  } else {
    if (is_volatile) {
      // In order to satisfy the semantics of volatile, this must be a single instruction store.
      locations->SetInAt(1, Location::RegisterOrInt32Constant(instruction->InputAt(1)));
    } else {
      locations->SetInAt(1, Location::RegisterOrConstant(instruction->InputAt(1)));
    }
  }
  if (needs_write_barrier) {
    // Temporary registers for the write barrier.
    locations->AddTemp(Location::RequiresRegister());  // Possibly used for reference poisoning too.
    locations->AddTemp(Location::RequiresRegister());
  } else if (kPoisonHeapReferences && field_type == Primitive::kPrimNot) {
    // Temporary register for the reference poisoning.
    locations->AddTemp(Location::RequiresRegister());
  }
}

void InstructionCodeGeneratorX86_64::HandleFieldSet(HInstruction* instruction,
                                                    const FieldInfo& field_info,
                                                    bool value_can_be_null) {
  DCHECK(instruction->IsInstanceFieldSet() || instruction->IsStaticFieldSet());

  LocationSummary* locations = instruction->GetLocations();
  CpuRegister base = locations->InAt(0).AsRegister<CpuRegister>();
  Location value = locations->InAt(1);
  bool is_volatile = field_info.IsVolatile();
  Primitive::Type field_type = field_info.GetFieldType();
  uint32_t offset = field_info.GetFieldOffset().Uint32Value();

  if (is_volatile) {
    codegen_->GenerateMemoryBarrier(MemBarrierKind::kAnyStore);
  }

  bool maybe_record_implicit_null_check_done = false;

  switch (field_type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte: {
      if (value.IsConstant()) {
        int8_t v = CodeGenerator::GetInt32ValueOf(value.GetConstant());
        __ movb(Address(base, offset), Immediate(v));
      } else {
        __ movb(Address(base, offset), value.AsRegister<CpuRegister>());
      }
      break;
    }

    case Primitive::kPrimShort:
    case Primitive::kPrimChar: {
      if (value.IsConstant()) {
        int16_t v = CodeGenerator::GetInt32ValueOf(value.GetConstant());
        __ movw(Address(base, offset), Immediate(v));
      } else {
        __ movw(Address(base, offset), value.AsRegister<CpuRegister>());
      }
      break;
    }

    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      if (value.IsConstant()) {
        int32_t v = CodeGenerator::GetInt32ValueOf(value.GetConstant());
        // `field_type == Primitive::kPrimNot` implies `v == 0`.
        DCHECK((field_type != Primitive::kPrimNot) || (v == 0));
        // Note: if heap poisoning is enabled, no need to poison
        // (negate) `v` if it is a reference, as it would be null.
        __ movl(Address(base, offset), Immediate(v));
      } else {
        if (kPoisonHeapReferences && field_type == Primitive::kPrimNot) {
          CpuRegister temp = locations->GetTemp(0).AsRegister<CpuRegister>();
          __ movl(temp, value.AsRegister<CpuRegister>());
          __ PoisonHeapReference(temp);
          __ movl(Address(base, offset), temp);
        } else {
          __ movl(Address(base, offset), value.AsRegister<CpuRegister>());
        }
      }
      break;
    }

    case Primitive::kPrimLong: {
      if (value.IsConstant()) {
        int64_t v = value.GetConstant()->AsLongConstant()->GetValue();
        codegen_->MoveInt64ToAddress(Address(base, offset),
                                     Address(base, offset + sizeof(int32_t)),
                                     v,
                                     instruction);
        maybe_record_implicit_null_check_done = true;
      } else {
        __ movq(Address(base, offset), value.AsRegister<CpuRegister>());
      }
      break;
    }

    case Primitive::kPrimFloat: {
      if (value.IsConstant()) {
        int32_t v =
            bit_cast<int32_t, float>(value.GetConstant()->AsFloatConstant()->GetValue());
        __ movl(Address(base, offset), Immediate(v));
      } else {
        __ movss(Address(base, offset), value.AsFpuRegister<XmmRegister>());
      }
      break;
    }

    case Primitive::kPrimDouble: {
      if (value.IsConstant()) {
        int64_t v =
            bit_cast<int64_t, double>(value.GetConstant()->AsDoubleConstant()->GetValue());
        codegen_->MoveInt64ToAddress(Address(base, offset),
                                     Address(base, offset + sizeof(int32_t)),
                                     v,
                                     instruction);
        maybe_record_implicit_null_check_done = true;
      } else {
        __ movsd(Address(base, offset), value.AsFpuRegister<XmmRegister>());
      }
      break;
    }

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << field_type;
      UNREACHABLE();
  }

  if (!maybe_record_implicit_null_check_done) {
    codegen_->MaybeRecordImplicitNullCheck(instruction);
  }

  if (CodeGenerator::StoreNeedsWriteBarrier(field_type, instruction->InputAt(1))) {
    CpuRegister temp = locations->GetTemp(0).AsRegister<CpuRegister>();
    CpuRegister card = locations->GetTemp(1).AsRegister<CpuRegister>();
    codegen_->MarkGCCard(temp, card, base, value.AsRegister<CpuRegister>(), value_can_be_null);
  }

  if (is_volatile) {
    codegen_->GenerateMemoryBarrier(MemBarrierKind::kAnyAny);
  }
}

void LocationsBuilderX86_64::VisitInstanceFieldSet(HInstanceFieldSet* instruction) {
  HandleFieldSet(instruction, instruction->GetFieldInfo());
}

void InstructionCodeGeneratorX86_64::VisitInstanceFieldSet(HInstanceFieldSet* instruction) {
  HandleFieldSet(instruction, instruction->GetFieldInfo(), instruction->GetValueCanBeNull());
}

void LocationsBuilderX86_64::VisitInstanceFieldGet(HInstanceFieldGet* instruction) {
  HandleFieldGet(instruction);
}

void InstructionCodeGeneratorX86_64::VisitInstanceFieldGet(HInstanceFieldGet* instruction) {
  HandleFieldGet(instruction, instruction->GetFieldInfo());
}

void LocationsBuilderX86_64::VisitStaticFieldGet(HStaticFieldGet* instruction) {
  HandleFieldGet(instruction);
}

void InstructionCodeGeneratorX86_64::VisitStaticFieldGet(HStaticFieldGet* instruction) {
  HandleFieldGet(instruction, instruction->GetFieldInfo());
}

void LocationsBuilderX86_64::VisitStaticFieldSet(HStaticFieldSet* instruction) {
  HandleFieldSet(instruction, instruction->GetFieldInfo());
}

void InstructionCodeGeneratorX86_64::VisitStaticFieldSet(HStaticFieldSet* instruction) {
  HandleFieldSet(instruction, instruction->GetFieldInfo(), instruction->GetValueCanBeNull());
}

void LocationsBuilderX86_64::VisitUnresolvedInstanceFieldGet(
    HUnresolvedInstanceFieldGet* instruction) {
  FieldAccessCallingConventionX86_64 calling_convention;
  codegen_->CreateUnresolvedFieldLocationSummary(
      instruction, instruction->GetFieldType(), calling_convention);
}

void InstructionCodeGeneratorX86_64::VisitUnresolvedInstanceFieldGet(
    HUnresolvedInstanceFieldGet* instruction) {
  FieldAccessCallingConventionX86_64 calling_convention;
  codegen_->GenerateUnresolvedFieldAccess(instruction,
                                          instruction->GetFieldType(),
                                          instruction->GetFieldIndex(),
                                          instruction->GetDexPc(),
                                          calling_convention);
}

void LocationsBuilderX86_64::VisitUnresolvedInstanceFieldSet(
    HUnresolvedInstanceFieldSet* instruction) {
  FieldAccessCallingConventionX86_64 calling_convention;
  codegen_->CreateUnresolvedFieldLocationSummary(
      instruction, instruction->GetFieldType(), calling_convention);
}

void InstructionCodeGeneratorX86_64::VisitUnresolvedInstanceFieldSet(
    HUnresolvedInstanceFieldSet* instruction) {
  FieldAccessCallingConventionX86_64 calling_convention;
  codegen_->GenerateUnresolvedFieldAccess(instruction,
                                          instruction->GetFieldType(),
                                          instruction->GetFieldIndex(),
                                          instruction->GetDexPc(),
                                          calling_convention);
}

void LocationsBuilderX86_64::VisitUnresolvedStaticFieldGet(
    HUnresolvedStaticFieldGet* instruction) {
  FieldAccessCallingConventionX86_64 calling_convention;
  codegen_->CreateUnresolvedFieldLocationSummary(
      instruction, instruction->GetFieldType(), calling_convention);
}

void InstructionCodeGeneratorX86_64::VisitUnresolvedStaticFieldGet(
    HUnresolvedStaticFieldGet* instruction) {
  FieldAccessCallingConventionX86_64 calling_convention;
  codegen_->GenerateUnresolvedFieldAccess(instruction,
                                          instruction->GetFieldType(),
                                          instruction->GetFieldIndex(),
                                          instruction->GetDexPc(),
                                          calling_convention);
}

void LocationsBuilderX86_64::VisitUnresolvedStaticFieldSet(
    HUnresolvedStaticFieldSet* instruction) {
  FieldAccessCallingConventionX86_64 calling_convention;
  codegen_->CreateUnresolvedFieldLocationSummary(
      instruction, instruction->GetFieldType(), calling_convention);
}

void InstructionCodeGeneratorX86_64::VisitUnresolvedStaticFieldSet(
    HUnresolvedStaticFieldSet* instruction) {
  FieldAccessCallingConventionX86_64 calling_convention;
  codegen_->GenerateUnresolvedFieldAccess(instruction,
                                          instruction->GetFieldType(),
                                          instruction->GetFieldIndex(),
                                          instruction->GetDexPc(),
                                          calling_convention);
}

void LocationsBuilderX86_64::VisitNullCheck(HNullCheck* instruction) {
  LocationSummary::CallKind call_kind = instruction->CanThrowIntoCatchBlock()
      ? LocationSummary::kCallOnSlowPath
      : LocationSummary::kNoCall;
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction, call_kind);
  Location loc = codegen_->IsImplicitNullCheckAllowed(instruction)
      ? Location::RequiresRegister()
      : Location::Any();
  locations->SetInAt(0, loc);
  if (instruction->HasUses()) {
    locations->SetOut(Location::SameAsFirstInput());
  }
}

void CodeGeneratorX86_64::GenerateImplicitNullCheck(HNullCheck* instruction) {
  if (CanMoveNullCheckToUser(instruction)) {
    return;
  }
  LocationSummary* locations = instruction->GetLocations();
  Location obj = locations->InAt(0);

  __ testl(CpuRegister(RAX), Address(obj.AsRegister<CpuRegister>(), 0));
  RecordPcInfo(instruction, instruction->GetDexPc());
}

void CodeGeneratorX86_64::GenerateExplicitNullCheck(HNullCheck* instruction) {
  SlowPathCode* slow_path = new (GetGraph()->GetArena()) NullCheckSlowPathX86_64(instruction);
  AddSlowPath(slow_path);

  LocationSummary* locations = instruction->GetLocations();
  Location obj = locations->InAt(0);

  if (obj.IsRegister()) {
    __ testl(obj.AsRegister<CpuRegister>(), obj.AsRegister<CpuRegister>());
  } else if (obj.IsStackSlot()) {
    __ cmpl(Address(CpuRegister(RSP), obj.GetStackIndex()), Immediate(0));
  } else {
    DCHECK(obj.IsConstant()) << obj;
    DCHECK(obj.GetConstant()->IsNullConstant());
    __ jmp(slow_path->GetEntryLabel());
    return;
  }
  __ j(kEqual, slow_path->GetEntryLabel());
}

void InstructionCodeGeneratorX86_64::VisitNullCheck(HNullCheck* instruction) {
  codegen_->GenerateNullCheck(instruction);
}

void LocationsBuilderX86_64::VisitArrayGet(HArrayGet* instruction) {
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
    // The output overlaps for an object array get when read barriers
    // are enabled: we do not want the move to overwrite the array's
    // location, as we need it to emit the read barrier.
    locations->SetOut(
        Location::RequiresRegister(),
        object_array_get_with_read_barrier ? Location::kOutputOverlap : Location::kNoOutputOverlap);
  }
  // We need a temporary register for the read barrier marking slow
  // path in CodeGeneratorX86_64::GenerateArrayLoadWithBakerReadBarrier.
  if (object_array_get_with_read_barrier && kUseBakerReadBarrier) {
    locations->AddTemp(Location::RequiresRegister());
  }
}

void InstructionCodeGeneratorX86_64::VisitArrayGet(HArrayGet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Location obj_loc = locations->InAt(0);
  CpuRegister obj = obj_loc.AsRegister<CpuRegister>();
  Location index = locations->InAt(1);
  Location out_loc = locations->Out();

  Primitive::Type type = instruction->GetType();
  switch (type) {
    case Primitive::kPrimBoolean: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(uint8_t)).Uint32Value();
      CpuRegister out = out_loc.AsRegister<CpuRegister>();
      if (index.IsConstant()) {
        __ movzxb(out, Address(obj,
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_1) + data_offset));
      } else {
        __ movzxb(out, Address(obj, index.AsRegister<CpuRegister>(), TIMES_1, data_offset));
      }
      break;
    }

    case Primitive::kPrimByte: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(int8_t)).Uint32Value();
      CpuRegister out = out_loc.AsRegister<CpuRegister>();
      if (index.IsConstant()) {
        __ movsxb(out, Address(obj,
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_1) + data_offset));
      } else {
        __ movsxb(out, Address(obj, index.AsRegister<CpuRegister>(), TIMES_1, data_offset));
      }
      break;
    }

    case Primitive::kPrimShort: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(int16_t)).Uint32Value();
      CpuRegister out = out_loc.AsRegister<CpuRegister>();
      if (index.IsConstant()) {
        __ movsxw(out, Address(obj,
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_2) + data_offset));
      } else {
        __ movsxw(out, Address(obj, index.AsRegister<CpuRegister>(), TIMES_2, data_offset));
      }
      break;
    }

    case Primitive::kPrimChar: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(uint16_t)).Uint32Value();
      CpuRegister out = out_loc.AsRegister<CpuRegister>();
      if (index.IsConstant()) {
        __ movzxw(out, Address(obj,
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_2) + data_offset));
      } else {
        __ movzxw(out, Address(obj, index.AsRegister<CpuRegister>(), TIMES_2, data_offset));
      }
      break;
    }

    case Primitive::kPrimInt: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(int32_t)).Uint32Value();
      CpuRegister out = out_loc.AsRegister<CpuRegister>();
      if (index.IsConstant()) {
        __ movl(out, Address(obj,
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_4) + data_offset));
      } else {
        __ movl(out, Address(obj, index.AsRegister<CpuRegister>(), TIMES_4, data_offset));
      }
      break;
    }

    case Primitive::kPrimNot: {
      static_assert(
          sizeof(mirror::HeapReference<mirror::Object>) == sizeof(int32_t),
          "art::mirror::HeapReference<art::mirror::Object> and int32_t have different sizes.");
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(int32_t)).Uint32Value();
      // /* HeapReference<Object> */ out =
      //     *(obj + data_offset + index * sizeof(HeapReference<Object>))
      if (kEmitCompilerReadBarrier && kUseBakerReadBarrier) {
        Location temp = locations->GetTemp(0);
        // Note that a potential implicit null check is handled in this
        // CodeGeneratorX86::GenerateArrayLoadWithBakerReadBarrier call.
        codegen_->GenerateArrayLoadWithBakerReadBarrier(
            instruction, out_loc, obj, data_offset, index, temp, /* needs_null_check */ true);
      } else {
        CpuRegister out = out_loc.AsRegister<CpuRegister>();
        if (index.IsConstant()) {
          uint32_t offset =
              (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_4) + data_offset;
          __ movl(out, Address(obj, offset));
          codegen_->MaybeRecordImplicitNullCheck(instruction);
          // If read barriers are enabled, emit read barriers other than
          // Baker's using a slow path (and also unpoison the loaded
          // reference, if heap poisoning is enabled).
          codegen_->MaybeGenerateReadBarrierSlow(instruction, out_loc, out_loc, obj_loc, offset);
        } else {
          __ movl(out, Address(obj, index.AsRegister<CpuRegister>(), TIMES_4, data_offset));
          codegen_->MaybeRecordImplicitNullCheck(instruction);
          // If read barriers are enabled, emit read barriers other than
          // Baker's using a slow path (and also unpoison the loaded
          // reference, if heap poisoning is enabled).
          codegen_->MaybeGenerateReadBarrierSlow(
              instruction, out_loc, out_loc, obj_loc, data_offset, index);
        }
      }
      break;
    }

    case Primitive::kPrimLong: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(int64_t)).Uint32Value();
      CpuRegister out = out_loc.AsRegister<CpuRegister>();
      if (index.IsConstant()) {
        __ movq(out, Address(obj,
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_8) + data_offset));
      } else {
        __ movq(out, Address(obj, index.AsRegister<CpuRegister>(), TIMES_8, data_offset));
      }
      break;
    }

    case Primitive::kPrimFloat: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(float)).Uint32Value();
      XmmRegister out = out_loc.AsFpuRegister<XmmRegister>();
      if (index.IsConstant()) {
        __ movss(out, Address(obj,
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_4) + data_offset));
      } else {
        __ movss(out, Address(obj, index.AsRegister<CpuRegister>(), TIMES_4, data_offset));
      }
      break;
    }

    case Primitive::kPrimDouble: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(double)).Uint32Value();
      XmmRegister out = out_loc.AsFpuRegister<XmmRegister>();
      if (index.IsConstant()) {
        __ movsd(out, Address(obj,
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_8) + data_offset));
      } else {
        __ movsd(out, Address(obj, index.AsRegister<CpuRegister>(), TIMES_8, data_offset));
      }
      break;
    }

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << type;
      UNREACHABLE();
  }

  if (type == Primitive::kPrimNot) {
    // Potential implicit null checks, in the case of reference
    // arrays, are handled in the previous switch statement.
  } else {
    codegen_->MaybeRecordImplicitNullCheck(instruction);
  }
}

void LocationsBuilderX86_64::VisitArraySet(HArraySet* instruction) {
  Primitive::Type value_type = instruction->GetComponentType();

  bool needs_write_barrier =
      CodeGenerator::StoreNeedsWriteBarrier(value_type, instruction->GetValue());
  bool may_need_runtime_call_for_type_check = instruction->NeedsTypeCheck();
  bool object_array_set_with_read_barrier =
      kEmitCompilerReadBarrier && (value_type == Primitive::kPrimNot);

  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(
      instruction,
      (may_need_runtime_call_for_type_check || object_array_set_with_read_barrier) ?
          LocationSummary::kCallOnSlowPath :
          LocationSummary::kNoCall);

  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RegisterOrConstant(instruction->InputAt(1)));
  if (Primitive::IsFloatingPointType(value_type)) {
    locations->SetInAt(2, Location::FpuRegisterOrConstant(instruction->InputAt(2)));
  } else {
    locations->SetInAt(2, Location::RegisterOrConstant(instruction->InputAt(2)));
  }

  if (needs_write_barrier) {
    // Temporary registers for the write barrier.

    // This first temporary register is possibly used for heap
    // reference poisoning and/or read barrier emission too.
    locations->AddTemp(Location::RequiresRegister());
    locations->AddTemp(Location::RequiresRegister());
  }
}

void InstructionCodeGeneratorX86_64::VisitArraySet(HArraySet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Location array_loc = locations->InAt(0);
  CpuRegister array = array_loc.AsRegister<CpuRegister>();
  Location index = locations->InAt(1);
  Location value = locations->InAt(2);
  Primitive::Type value_type = instruction->GetComponentType();
  bool may_need_runtime_call_for_type_check = instruction->NeedsTypeCheck();
  bool needs_write_barrier =
      CodeGenerator::StoreNeedsWriteBarrier(value_type, instruction->GetValue());
  uint32_t class_offset = mirror::Object::ClassOffset().Int32Value();
  uint32_t super_offset = mirror::Class::SuperClassOffset().Int32Value();
  uint32_t component_offset = mirror::Class::ComponentTypeOffset().Int32Value();

  switch (value_type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte: {
      uint32_t offset = mirror::Array::DataOffset(sizeof(uint8_t)).Uint32Value();
      Address address = index.IsConstant()
          ? Address(array, (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_1) + offset)
          : Address(array, index.AsRegister<CpuRegister>(), TIMES_1, offset);
      if (value.IsRegister()) {
        __ movb(address, value.AsRegister<CpuRegister>());
      } else {
        __ movb(address, Immediate(value.GetConstant()->AsIntConstant()->GetValue()));
      }
      codegen_->MaybeRecordImplicitNullCheck(instruction);
      break;
    }

    case Primitive::kPrimShort:
    case Primitive::kPrimChar: {
      uint32_t offset = mirror::Array::DataOffset(sizeof(uint16_t)).Uint32Value();
      Address address = index.IsConstant()
          ? Address(array, (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_2) + offset)
          : Address(array, index.AsRegister<CpuRegister>(), TIMES_2, offset);
      if (value.IsRegister()) {
        __ movw(address, value.AsRegister<CpuRegister>());
      } else {
        DCHECK(value.IsConstant()) << value;
        __ movw(address, Immediate(value.GetConstant()->AsIntConstant()->GetValue()));
      }
      codegen_->MaybeRecordImplicitNullCheck(instruction);
      break;
    }

    case Primitive::kPrimNot: {
      uint32_t offset = mirror::Array::DataOffset(sizeof(int32_t)).Uint32Value();
      Address address = index.IsConstant()
          ? Address(array, (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_4) + offset)
          : Address(array, index.AsRegister<CpuRegister>(), TIMES_4, offset);

      if (!value.IsRegister()) {
        // Just setting null.
        DCHECK(instruction->InputAt(2)->IsNullConstant());
        DCHECK(value.IsConstant()) << value;
        __ movl(address, Immediate(0));
        codegen_->MaybeRecordImplicitNullCheck(instruction);
        DCHECK(!needs_write_barrier);
        DCHECK(!may_need_runtime_call_for_type_check);
        break;
      }

      DCHECK(needs_write_barrier);
      CpuRegister register_value = value.AsRegister<CpuRegister>();
      NearLabel done, not_null, do_put;
      SlowPathCode* slow_path = nullptr;
      CpuRegister temp = locations->GetTemp(0).AsRegister<CpuRegister>();
      if (may_need_runtime_call_for_type_check) {
        slow_path = new (GetGraph()->GetArena()) ArraySetSlowPathX86_64(instruction);
        codegen_->AddSlowPath(slow_path);
        if (instruction->GetValueCanBeNull()) {
          __ testl(register_value, register_value);
          __ j(kNotEqual, &not_null);
          __ movl(address, Immediate(0));
          codegen_->MaybeRecordImplicitNullCheck(instruction);
          __ jmp(&done);
          __ Bind(&not_null);
        }

        if (kEmitCompilerReadBarrier) {
          // When read barriers are enabled, the type checking
          // instrumentation requires two read barriers:
          //
          //   __ movl(temp2, temp);
          //   // /* HeapReference<Class> */ temp = temp->component_type_
          //   __ movl(temp, Address(temp, component_offset));
          //   codegen_->GenerateReadBarrierSlow(
          //       instruction, temp_loc, temp_loc, temp2_loc, component_offset);
          //
          //   // /* HeapReference<Class> */ temp2 = register_value->klass_
          //   __ movl(temp2, Address(register_value, class_offset));
          //   codegen_->GenerateReadBarrierSlow(
          //       instruction, temp2_loc, temp2_loc, value, class_offset, temp_loc);
          //
          //   __ cmpl(temp, temp2);
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
          __ jmp(slow_path->GetEntryLabel());
        } else {
          // /* HeapReference<Class> */ temp = array->klass_
          __ movl(temp, Address(array, class_offset));
          codegen_->MaybeRecordImplicitNullCheck(instruction);
          __ MaybeUnpoisonHeapReference(temp);

          // /* HeapReference<Class> */ temp = temp->component_type_
          __ movl(temp, Address(temp, component_offset));
          // If heap poisoning is enabled, no need to unpoison `temp`
          // nor the object reference in `register_value->klass`, as
          // we are comparing two poisoned references.
          __ cmpl(temp, Address(register_value, class_offset));

          if (instruction->StaticTypeOfArrayIsObjectArray()) {
            __ j(kEqual, &do_put);
            // If heap poisoning is enabled, the `temp` reference has
            // not been unpoisoned yet; unpoison it now.
            __ MaybeUnpoisonHeapReference(temp);

            // /* HeapReference<Class> */ temp = temp->super_class_
            __ movl(temp, Address(temp, super_offset));
            // If heap poisoning is enabled, no need to unpoison
            // `temp`, as we are comparing against null below.
            __ testl(temp, temp);
            __ j(kNotEqual, slow_path->GetEntryLabel());
            __ Bind(&do_put);
          } else {
            __ j(kNotEqual, slow_path->GetEntryLabel());
          }
        }
      }

      if (kPoisonHeapReferences) {
        __ movl(temp, register_value);
        __ PoisonHeapReference(temp);
        __ movl(address, temp);
      } else {
        __ movl(address, register_value);
      }
      if (!may_need_runtime_call_for_type_check) {
        codegen_->MaybeRecordImplicitNullCheck(instruction);
      }

      CpuRegister card = locations->GetTemp(1).AsRegister<CpuRegister>();
      codegen_->MarkGCCard(
          temp, card, array, value.AsRegister<CpuRegister>(), instruction->GetValueCanBeNull());
      __ Bind(&done);

      if (slow_path != nullptr) {
        __ Bind(slow_path->GetExitLabel());
      }

      break;
    }

    case Primitive::kPrimInt: {
      uint32_t offset = mirror::Array::DataOffset(sizeof(int32_t)).Uint32Value();
      Address address = index.IsConstant()
          ? Address(array, (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_4) + offset)
          : Address(array, index.AsRegister<CpuRegister>(), TIMES_4, offset);
      if (value.IsRegister()) {
        __ movl(address, value.AsRegister<CpuRegister>());
      } else {
        DCHECK(value.IsConstant()) << value;
        int32_t v = CodeGenerator::GetInt32ValueOf(value.GetConstant());
        __ movl(address, Immediate(v));
      }
      codegen_->MaybeRecordImplicitNullCheck(instruction);
      break;
    }

    case Primitive::kPrimLong: {
      uint32_t offset = mirror::Array::DataOffset(sizeof(int64_t)).Uint32Value();
      Address address = index.IsConstant()
          ? Address(array, (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_8) + offset)
          : Address(array, index.AsRegister<CpuRegister>(), TIMES_8, offset);
      if (value.IsRegister()) {
        __ movq(address, value.AsRegister<CpuRegister>());
        codegen_->MaybeRecordImplicitNullCheck(instruction);
      } else {
        int64_t v = value.GetConstant()->AsLongConstant()->GetValue();
        Address address_high = index.IsConstant()
            ? Address(array, (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_8) +
                offset + sizeof(int32_t))
            : Address(array, index.AsRegister<CpuRegister>(), TIMES_8, offset + sizeof(int32_t));
        codegen_->MoveInt64ToAddress(address, address_high, v, instruction);
      }
      break;
    }

    case Primitive::kPrimFloat: {
      uint32_t offset = mirror::Array::DataOffset(sizeof(float)).Uint32Value();
      Address address = index.IsConstant()
          ? Address(array, (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_4) + offset)
          : Address(array, index.AsRegister<CpuRegister>(), TIMES_4, offset);
      if (value.IsFpuRegister()) {
        __ movss(address, value.AsFpuRegister<XmmRegister>());
      } else {
        DCHECK(value.IsConstant());
        int32_t v =
            bit_cast<int32_t, float>(value.GetConstant()->AsFloatConstant()->GetValue());
        __ movl(address, Immediate(v));
      }
      codegen_->MaybeRecordImplicitNullCheck(instruction);
      break;
    }

    case Primitive::kPrimDouble: {
      uint32_t offset = mirror::Array::DataOffset(sizeof(double)).Uint32Value();
      Address address = index.IsConstant()
          ? Address(array, (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_8) + offset)
          : Address(array, index.AsRegister<CpuRegister>(), TIMES_8, offset);
      if (value.IsFpuRegister()) {
        __ movsd(address, value.AsFpuRegister<XmmRegister>());
        codegen_->MaybeRecordImplicitNullCheck(instruction);
      } else {
        int64_t v =
            bit_cast<int64_t, double>(value.GetConstant()->AsDoubleConstant()->GetValue());
        Address address_high = index.IsConstant()
            ? Address(array, (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_8) +
                offset + sizeof(int32_t))
            : Address(array, index.AsRegister<CpuRegister>(), TIMES_8, offset + sizeof(int32_t));
        codegen_->MoveInt64ToAddress(address, address_high, v, instruction);
      }
      break;
    }

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << instruction->GetType();
      UNREACHABLE();
  }
}

void LocationsBuilderX86_64::VisitArrayLength(HArrayLength* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

void InstructionCodeGeneratorX86_64::VisitArrayLength(HArrayLength* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  uint32_t offset = mirror::Array::LengthOffset().Uint32Value();
  CpuRegister obj = locations->InAt(0).AsRegister<CpuRegister>();
  CpuRegister out = locations->Out().AsRegister<CpuRegister>();
  __ movl(out, Address(obj, offset));
  codegen_->MaybeRecordImplicitNullCheck(instruction);
}

void LocationsBuilderX86_64::VisitBoundsCheck(HBoundsCheck* instruction) {
  LocationSummary::CallKind call_kind = instruction->CanThrowIntoCatchBlock()
      ? LocationSummary::kCallOnSlowPath
      : LocationSummary::kNoCall;
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction, call_kind);
  locations->SetInAt(0, Location::RegisterOrConstant(instruction->InputAt(0)));
  locations->SetInAt(1, Location::RegisterOrConstant(instruction->InputAt(1)));
  if (instruction->HasUses()) {
    locations->SetOut(Location::SameAsFirstInput());
  }
}

void InstructionCodeGeneratorX86_64::VisitBoundsCheck(HBoundsCheck* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Location index_loc = locations->InAt(0);
  Location length_loc = locations->InAt(1);
  SlowPathCode* slow_path =
      new (GetGraph()->GetArena()) BoundsCheckSlowPathX86_64(instruction);

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
    CpuRegister index_reg = index_loc.AsRegister<CpuRegister>();
    __ cmpl(index_reg, Immediate(length));
    codegen_->AddSlowPath(slow_path);
    __ j(kAboveEqual, slow_path->GetEntryLabel());
  } else {
    CpuRegister length = length_loc.AsRegister<CpuRegister>();
    if (index_loc.IsConstant()) {
      int32_t value = CodeGenerator::GetInt32ValueOf(index_loc.GetConstant());
      __ cmpl(length, Immediate(value));
    } else {
      __ cmpl(length, index_loc.AsRegister<CpuRegister>());
    }
    codegen_->AddSlowPath(slow_path);
    __ j(kBelowEqual, slow_path->GetEntryLabel());
  }
}

void CodeGeneratorX86_64::MarkGCCard(CpuRegister temp,
                                     CpuRegister card,
                                     CpuRegister object,
                                     CpuRegister value,
                                     bool value_can_be_null) {
  NearLabel is_null;
  if (value_can_be_null) {
    __ testl(value, value);
    __ j(kEqual, &is_null);
  }
  __ gs()->movq(card, Address::Absolute(Thread::CardTableOffset<kX86_64WordSize>().Int32Value(),
                                        /* no_rip */ true));
  __ movq(temp, object);
  __ shrq(temp, Immediate(gc::accounting::CardTable::kCardShift));
  __ movb(Address(temp, card, TIMES_1, 0), card);
  if (value_can_be_null) {
    __ Bind(&is_null);
  }
}

void LocationsBuilderX86_64::VisitParallelMove(HParallelMove* instruction ATTRIBUTE_UNUSED) {
  LOG(FATAL) << "Unimplemented";
}

void InstructionCodeGeneratorX86_64::VisitParallelMove(HParallelMove* instruction) {
  codegen_->GetMoveResolver()->EmitNativeCode(instruction);
}

void LocationsBuilderX86_64::VisitSuspendCheck(HSuspendCheck* instruction) {
  new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCallOnSlowPath);
}

void InstructionCodeGeneratorX86_64::VisitSuspendCheck(HSuspendCheck* instruction) {
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

void InstructionCodeGeneratorX86_64::GenerateSuspendCheck(HSuspendCheck* instruction,
                                                          HBasicBlock* successor) {
  SuspendCheckSlowPathX86_64* slow_path =
      down_cast<SuspendCheckSlowPathX86_64*>(instruction->GetSlowPath());
  if (slow_path == nullptr) {
    slow_path = new (GetGraph()->GetArena()) SuspendCheckSlowPathX86_64(instruction, successor);
    instruction->SetSlowPath(slow_path);
    codegen_->AddSlowPath(slow_path);
    if (successor != nullptr) {
      DCHECK(successor->IsLoopHeader());
      codegen_->ClearSpillSlotsFromLoopPhisInStackMap(instruction);
    }
  } else {
    DCHECK_EQ(slow_path->GetSuccessor(), successor);
  }

  __ gs()->cmpw(Address::Absolute(Thread::ThreadFlagsOffset<kX86_64WordSize>().Int32Value(),
                                  /* no_rip */ true),
                Immediate(0));
  if (successor == nullptr) {
    __ j(kNotEqual, slow_path->GetEntryLabel());
    __ Bind(slow_path->GetReturnLabel());
  } else {
    __ j(kEqual, codegen_->GetLabelOf(successor));
    __ jmp(slow_path->GetEntryLabel());
  }
}

X86_64Assembler* ParallelMoveResolverX86_64::GetAssembler() const {
  return codegen_->GetAssembler();
}

void ParallelMoveResolverX86_64::EmitMove(size_t index) {
  MoveOperands* move = moves_[index];
  Location source = move->GetSource();
  Location destination = move->GetDestination();

  if (source.IsRegister()) {
    if (destination.IsRegister()) {
      __ movq(destination.AsRegister<CpuRegister>(), source.AsRegister<CpuRegister>());
    } else if (destination.IsStackSlot()) {
      __ movl(Address(CpuRegister(RSP), destination.GetStackIndex()),
              source.AsRegister<CpuRegister>());
    } else {
      DCHECK(destination.IsDoubleStackSlot());
      __ movq(Address(CpuRegister(RSP), destination.GetStackIndex()),
              source.AsRegister<CpuRegister>());
    }
  } else if (source.IsStackSlot()) {
    if (destination.IsRegister()) {
      __ movl(destination.AsRegister<CpuRegister>(),
              Address(CpuRegister(RSP), source.GetStackIndex()));
    } else if (destination.IsFpuRegister()) {
      __ movss(destination.AsFpuRegister<XmmRegister>(),
              Address(CpuRegister(RSP), source.GetStackIndex()));
    } else {
      DCHECK(destination.IsStackSlot());
      __ movl(CpuRegister(TMP), Address(CpuRegister(RSP), source.GetStackIndex()));
      __ movl(Address(CpuRegister(RSP), destination.GetStackIndex()), CpuRegister(TMP));
    }
  } else if (source.IsDoubleStackSlot()) {
    if (destination.IsRegister()) {
      __ movq(destination.AsRegister<CpuRegister>(),
              Address(CpuRegister(RSP), source.GetStackIndex()));
    } else if (destination.IsFpuRegister()) {
      __ movsd(destination.AsFpuRegister<XmmRegister>(),
               Address(CpuRegister(RSP), source.GetStackIndex()));
    } else {
      DCHECK(destination.IsDoubleStackSlot()) << destination;
      __ movq(CpuRegister(TMP), Address(CpuRegister(RSP), source.GetStackIndex()));
      __ movq(Address(CpuRegister(RSP), destination.GetStackIndex()), CpuRegister(TMP));
    }
  } else if (source.IsConstant()) {
    HConstant* constant = source.GetConstant();
    if (constant->IsIntConstant() || constant->IsNullConstant()) {
      int32_t value = CodeGenerator::GetInt32ValueOf(constant);
      if (destination.IsRegister()) {
        if (value == 0) {
          __ xorl(destination.AsRegister<CpuRegister>(), destination.AsRegister<CpuRegister>());
        } else {
          __ movl(destination.AsRegister<CpuRegister>(), Immediate(value));
        }
      } else {
        DCHECK(destination.IsStackSlot()) << destination;
        __ movl(Address(CpuRegister(RSP), destination.GetStackIndex()), Immediate(value));
      }
    } else if (constant->IsLongConstant()) {
      int64_t value = constant->AsLongConstant()->GetValue();
      if (destination.IsRegister()) {
        codegen_->Load64BitValue(destination.AsRegister<CpuRegister>(), value);
      } else {
        DCHECK(destination.IsDoubleStackSlot()) << destination;
        codegen_->Store64BitValueToStack(destination, value);
      }
    } else if (constant->IsFloatConstant()) {
      float fp_value = constant->AsFloatConstant()->GetValue();
      if (destination.IsFpuRegister()) {
        XmmRegister dest = destination.AsFpuRegister<XmmRegister>();
        codegen_->Load32BitValue(dest, fp_value);
      } else {
        DCHECK(destination.IsStackSlot()) << destination;
        Immediate imm(bit_cast<int32_t, float>(fp_value));
        __ movl(Address(CpuRegister(RSP), destination.GetStackIndex()), imm);
      }
    } else {
      DCHECK(constant->IsDoubleConstant()) << constant->DebugName();
      double fp_value =  constant->AsDoubleConstant()->GetValue();
      int64_t value = bit_cast<int64_t, double>(fp_value);
      if (destination.IsFpuRegister()) {
        XmmRegister dest = destination.AsFpuRegister<XmmRegister>();
        codegen_->Load64BitValue(dest, fp_value);
      } else {
        DCHECK(destination.IsDoubleStackSlot()) << destination;
        codegen_->Store64BitValueToStack(destination, value);
      }
    }
  } else if (source.IsFpuRegister()) {
    if (destination.IsFpuRegister()) {
      __ movaps(destination.AsFpuRegister<XmmRegister>(), source.AsFpuRegister<XmmRegister>());
    } else if (destination.IsStackSlot()) {
      __ movss(Address(CpuRegister(RSP), destination.GetStackIndex()),
               source.AsFpuRegister<XmmRegister>());
    } else {
      DCHECK(destination.IsDoubleStackSlot()) << destination;
      __ movsd(Address(CpuRegister(RSP), destination.GetStackIndex()),
               source.AsFpuRegister<XmmRegister>());
    }
  }
}

void ParallelMoveResolverX86_64::Exchange32(CpuRegister reg, int mem) {
  __ movl(CpuRegister(TMP), Address(CpuRegister(RSP), mem));
  __ movl(Address(CpuRegister(RSP), mem), reg);
  __ movl(reg, CpuRegister(TMP));
}

void ParallelMoveResolverX86_64::Exchange32(int mem1, int mem2) {
  ScratchRegisterScope ensure_scratch(
      this, TMP, RAX, codegen_->GetNumberOfCoreRegisters());

  int stack_offset = ensure_scratch.IsSpilled() ? kX86_64WordSize : 0;
  __ movl(CpuRegister(TMP), Address(CpuRegister(RSP), mem1 + stack_offset));
  __ movl(CpuRegister(ensure_scratch.GetRegister()),
          Address(CpuRegister(RSP), mem2 + stack_offset));
  __ movl(Address(CpuRegister(RSP), mem2 + stack_offset), CpuRegister(TMP));
  __ movl(Address(CpuRegister(RSP), mem1 + stack_offset),
          CpuRegister(ensure_scratch.GetRegister()));
}

void ParallelMoveResolverX86_64::Exchange64(CpuRegister reg1, CpuRegister reg2) {
  __ movq(CpuRegister(TMP), reg1);
  __ movq(reg1, reg2);
  __ movq(reg2, CpuRegister(TMP));
}

void ParallelMoveResolverX86_64::Exchange64(CpuRegister reg, int mem) {
  __ movq(CpuRegister(TMP), Address(CpuRegister(RSP), mem));
  __ movq(Address(CpuRegister(RSP), mem), reg);
  __ movq(reg, CpuRegister(TMP));
}

void ParallelMoveResolverX86_64::Exchange64(int mem1, int mem2) {
  ScratchRegisterScope ensure_scratch(
      this, TMP, RAX, codegen_->GetNumberOfCoreRegisters());

  int stack_offset = ensure_scratch.IsSpilled() ? kX86_64WordSize : 0;
  __ movq(CpuRegister(TMP), Address(CpuRegister(RSP), mem1 + stack_offset));
  __ movq(CpuRegister(ensure_scratch.GetRegister()),
          Address(CpuRegister(RSP), mem2 + stack_offset));
  __ movq(Address(CpuRegister(RSP), mem2 + stack_offset), CpuRegister(TMP));
  __ movq(Address(CpuRegister(RSP), mem1 + stack_offset),
          CpuRegister(ensure_scratch.GetRegister()));
}

void ParallelMoveResolverX86_64::Exchange32(XmmRegister reg, int mem) {
  __ movl(CpuRegister(TMP), Address(CpuRegister(RSP), mem));
  __ movss(Address(CpuRegister(RSP), mem), reg);
  __ movd(reg, CpuRegister(TMP));
}

void ParallelMoveResolverX86_64::Exchange64(XmmRegister reg, int mem) {
  __ movq(CpuRegister(TMP), Address(CpuRegister(RSP), mem));
  __ movsd(Address(CpuRegister(RSP), mem), reg);
  __ movd(reg, CpuRegister(TMP));
}

void ParallelMoveResolverX86_64::EmitSwap(size_t index) {
  MoveOperands* move = moves_[index];
  Location source = move->GetSource();
  Location destination = move->GetDestination();

  if (source.IsRegister() && destination.IsRegister()) {
    Exchange64(source.AsRegister<CpuRegister>(), destination.AsRegister<CpuRegister>());
  } else if (source.IsRegister() && destination.IsStackSlot()) {
    Exchange32(source.AsRegister<CpuRegister>(), destination.GetStackIndex());
  } else if (source.IsStackSlot() && destination.IsRegister()) {
    Exchange32(destination.AsRegister<CpuRegister>(), source.GetStackIndex());
  } else if (source.IsStackSlot() && destination.IsStackSlot()) {
    Exchange32(destination.GetStackIndex(), source.GetStackIndex());
  } else if (source.IsRegister() && destination.IsDoubleStackSlot()) {
    Exchange64(source.AsRegister<CpuRegister>(), destination.GetStackIndex());
  } else if (source.IsDoubleStackSlot() && destination.IsRegister()) {
    Exchange64(destination.AsRegister<CpuRegister>(), source.GetStackIndex());
  } else if (source.IsDoubleStackSlot() && destination.IsDoubleStackSlot()) {
    Exchange64(destination.GetStackIndex(), source.GetStackIndex());
  } else if (source.IsFpuRegister() && destination.IsFpuRegister()) {
    __ movd(CpuRegister(TMP), source.AsFpuRegister<XmmRegister>());
    __ movaps(source.AsFpuRegister<XmmRegister>(), destination.AsFpuRegister<XmmRegister>());
    __ movd(destination.AsFpuRegister<XmmRegister>(), CpuRegister(TMP));
  } else if (source.IsFpuRegister() && destination.IsStackSlot()) {
    Exchange32(source.AsFpuRegister<XmmRegister>(), destination.GetStackIndex());
  } else if (source.IsStackSlot() && destination.IsFpuRegister()) {
    Exchange32(destination.AsFpuRegister<XmmRegister>(), source.GetStackIndex());
  } else if (source.IsFpuRegister() && destination.IsDoubleStackSlot()) {
    Exchange64(source.AsFpuRegister<XmmRegister>(), destination.GetStackIndex());
  } else if (source.IsDoubleStackSlot() && destination.IsFpuRegister()) {
    Exchange64(destination.AsFpuRegister<XmmRegister>(), source.GetStackIndex());
  } else {
    LOG(FATAL) << "Unimplemented swap between " << source << " and " << destination;
  }
}


void ParallelMoveResolverX86_64::SpillScratch(int reg) {
  __ pushq(CpuRegister(reg));
}


void ParallelMoveResolverX86_64::RestoreScratch(int reg) {
  __ popq(CpuRegister(reg));
}

void InstructionCodeGeneratorX86_64::GenerateClassInitializationCheck(
    SlowPathCode* slow_path, CpuRegister class_reg) {
  __ cmpl(Address(class_reg,  mirror::Class::StatusOffset().Int32Value()),
          Immediate(mirror::Class::kStatusInitialized));
  __ j(kLess, slow_path->GetEntryLabel());
  __ Bind(slow_path->GetExitLabel());
  // No need for memory fence, thanks to the x86-64 memory model.
}

void LocationsBuilderX86_64::VisitLoadClass(HLoadClass* cls) {
  InvokeRuntimeCallingConvention calling_convention;
  CodeGenerator::CreateLoadClassLocationSummary(
      cls,
      Location::RegisterLocation(calling_convention.GetRegisterAt(0)),
      Location::RegisterLocation(RAX),
      /* code_generator_supports_read_barrier */ true);
}

void InstructionCodeGeneratorX86_64::VisitLoadClass(HLoadClass* cls) {
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

  Location out_loc = locations->Out();
  CpuRegister out = out_loc.AsRegister<CpuRegister>();
  CpuRegister current_method = locations->InAt(0).AsRegister<CpuRegister>();

  if (cls->IsReferrersClass()) {
    DCHECK(!cls->CanCallRuntime());
    DCHECK(!cls->MustGenerateClinitCheck());
    // /* GcRoot<mirror::Class> */ out = current_method->declaring_class_
    GenerateGcRootFieldLoad(
        cls, out_loc, Address(current_method, ArtMethod::DeclaringClassOffset().Int32Value()));
  } else {
    // /* GcRoot<mirror::Class>[] */ out =
    //        current_method.ptr_sized_fields_->dex_cache_resolved_types_
    __ movq(out, Address(current_method,
                         ArtMethod::DexCacheResolvedTypesOffset(kX86_64PointerSize).Int32Value()));
    // /* GcRoot<mirror::Class> */ out = out[type_index]
    GenerateGcRootFieldLoad(
        cls, out_loc, Address(out, CodeGenerator::GetCacheOffset(cls->GetTypeIndex())));

    if (!cls->IsInDexCache() || cls->MustGenerateClinitCheck()) {
      DCHECK(cls->CanCallRuntime());
      SlowPathCode* slow_path = new (GetGraph()->GetArena()) LoadClassSlowPathX86_64(
          cls, cls, cls->GetDexPc(), cls->MustGenerateClinitCheck());
      codegen_->AddSlowPath(slow_path);
      if (!cls->IsInDexCache()) {
        __ testl(out, out);
        __ j(kEqual, slow_path->GetEntryLabel());
      }
      if (cls->MustGenerateClinitCheck()) {
        GenerateClassInitializationCheck(slow_path, out);
      } else {
        __ Bind(slow_path->GetExitLabel());
      }
    }
  }
}

void LocationsBuilderX86_64::VisitClinitCheck(HClinitCheck* check) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(check, LocationSummary::kCallOnSlowPath);
  locations->SetInAt(0, Location::RequiresRegister());
  if (check->HasUses()) {
    locations->SetOut(Location::SameAsFirstInput());
  }
}

void InstructionCodeGeneratorX86_64::VisitClinitCheck(HClinitCheck* check) {
  // We assume the class to not be null.
  SlowPathCode* slow_path = new (GetGraph()->GetArena()) LoadClassSlowPathX86_64(
      check->GetLoadClass(), check, check->GetDexPc(), true);
  codegen_->AddSlowPath(slow_path);
  GenerateClassInitializationCheck(slow_path,
                                   check->GetLocations()->InAt(0).AsRegister<CpuRegister>());
}

HLoadString::LoadKind CodeGeneratorX86_64::GetSupportedLoadStringKind(
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
      // We prefer the always-available RIP-relative address for the x86-64 boot image.
      return HLoadString::LoadKind::kBootImageLinkTimePcRelative;
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

void LocationsBuilderX86_64::VisitLoadString(HLoadString* load) {
  LocationSummary::CallKind call_kind = (load->NeedsEnvironment() || kEmitCompilerReadBarrier)
      ? LocationSummary::kCallOnSlowPath
      : LocationSummary::kNoCall;
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(load, call_kind);
  if (load->GetLoadKind() == HLoadString::LoadKind::kDexCacheViaMethod) {
    locations->SetInAt(0, Location::RequiresRegister());
  }
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorX86_64::VisitLoadString(HLoadString* load) {
  LocationSummary* locations = load->GetLocations();
  Location out_loc = locations->Out();
  CpuRegister out = out_loc.AsRegister<CpuRegister>();

  switch (load->GetLoadKind()) {
    case HLoadString::LoadKind::kBootImageLinkTimePcRelative: {
      DCHECK(!kEmitCompilerReadBarrier);
      __ leal(out, Address::Absolute(CodeGeneratorX86_64::kDummy32BitOffset, /* no_rip */ false));
      codegen_->RecordStringPatch(load);
      return;  // No dex cache slow path.
    }
    case HLoadString::LoadKind::kBootImageAddress: {
      DCHECK(!kEmitCompilerReadBarrier);
      DCHECK_NE(load->GetAddress(), 0u);
      uint32_t address = dchecked_integral_cast<uint32_t>(load->GetAddress());
      __ movl(out, Immediate(address));  // Zero-extended.
      codegen_->RecordSimplePatch();
      return;  // No dex cache slow path.
    }
    case HLoadString::LoadKind::kDexCacheAddress: {
      DCHECK_NE(load->GetAddress(), 0u);
      if (IsUint<32>(load->GetAddress())) {
        Address address = Address::Absolute(load->GetAddress(), /* no_rip */ true);
        GenerateGcRootFieldLoad(load, out_loc, address);
      } else {
        // TODO: Consider using opcode A1, i.e. movl eax, moff32 (with 64-bit address).
        __ movq(out, Immediate(load->GetAddress()));
        GenerateGcRootFieldLoad(load, out_loc, Address(out, 0));
      }
      break;
    }
    case HLoadString::LoadKind::kDexCachePcRelative: {
      uint32_t offset = load->GetDexCacheElementOffset();
      Label* fixup_label = codegen_->NewPcRelativeDexCacheArrayPatch(load->GetDexFile(), offset);
      Address address = Address::Absolute(CodeGeneratorX86_64::kDummy32BitOffset,
                                          /* no_rip */ false);
      GenerateGcRootFieldLoad(load, out_loc, address, fixup_label);
      break;
    }
    case HLoadString::LoadKind::kDexCacheViaMethod: {
      CpuRegister current_method = locations->InAt(0).AsRegister<CpuRegister>();

      // /* GcRoot<mirror::Class> */ out = current_method->declaring_class_
      GenerateGcRootFieldLoad(
          load, out_loc, Address(current_method, ArtMethod::DeclaringClassOffset().Int32Value()));
      // /* GcRoot<mirror::String>[] */ out = out->dex_cache_strings_
      __ movq(out, Address(out, mirror::Class::DexCacheStringsOffset().Uint32Value()));
      // /* GcRoot<mirror::String> */ out = out[string_index]
      GenerateGcRootFieldLoad(
          load, out_loc, Address(out, CodeGenerator::GetCacheOffset(load->GetStringIndex())));
      break;
    }
    default:
      LOG(FATAL) << "Unexpected load kind: " << load->GetLoadKind();
      UNREACHABLE();
  }

  if (!load->IsInDexCache()) {
    SlowPathCode* slow_path = new (GetGraph()->GetArena()) LoadStringSlowPathX86_64(load);
    codegen_->AddSlowPath(slow_path);
    __ testl(out, out);
    __ j(kEqual, slow_path->GetEntryLabel());
    __ Bind(slow_path->GetExitLabel());
  }
}

static Address GetExceptionTlsAddress() {
  return Address::Absolute(Thread::ExceptionOffset<kX86_64WordSize>().Int32Value(),
                           /* no_rip */ true);
}

void LocationsBuilderX86_64::VisitLoadException(HLoadException* load) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(load, LocationSummary::kNoCall);
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorX86_64::VisitLoadException(HLoadException* load) {
  __ gs()->movl(load->GetLocations()->Out().AsRegister<CpuRegister>(), GetExceptionTlsAddress());
}

void LocationsBuilderX86_64::VisitClearException(HClearException* clear) {
  new (GetGraph()->GetArena()) LocationSummary(clear, LocationSummary::kNoCall);
}

void InstructionCodeGeneratorX86_64::VisitClearException(HClearException* clear ATTRIBUTE_UNUSED) {
  __ gs()->movl(GetExceptionTlsAddress(), Immediate(0));
}

void LocationsBuilderX86_64::VisitThrow(HThrow* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCall);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
}

void InstructionCodeGeneratorX86_64::VisitThrow(HThrow* instruction) {
  codegen_->InvokeRuntime(QUICK_ENTRY_POINT(pDeliverException),
                          instruction,
                          instruction->GetDexPc(),
                          nullptr);
  CheckEntrypointTypes<kQuickDeliverException, void, mirror::Object*>();
}

static bool TypeCheckNeedsATemporary(TypeCheckKind type_check_kind) {
  return kEmitCompilerReadBarrier &&
      (kUseBakerReadBarrier ||
       type_check_kind == TypeCheckKind::kAbstractClassCheck ||
       type_check_kind == TypeCheckKind::kClassHierarchyCheck ||
       type_check_kind == TypeCheckKind::kArrayObjectCheck);
}

void LocationsBuilderX86_64::VisitInstanceOf(HInstanceOf* instruction) {
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
  locations->SetInAt(1, Location::Any());
  // Note that TypeCheckSlowPathX86_64 uses this "out" register too.
  locations->SetOut(Location::RequiresRegister());
  // When read barriers are enabled, we need a temporary register for
  // some cases.
  if (TypeCheckNeedsATemporary(type_check_kind)) {
    locations->AddTemp(Location::RequiresRegister());
  }
}

void InstructionCodeGeneratorX86_64::VisitInstanceOf(HInstanceOf* instruction) {
  TypeCheckKind type_check_kind = instruction->GetTypeCheckKind();
  LocationSummary* locations = instruction->GetLocations();
  Location obj_loc = locations->InAt(0);
  CpuRegister obj = obj_loc.AsRegister<CpuRegister>();
  Location cls = locations->InAt(1);
  Location out_loc =  locations->Out();
  CpuRegister out = out_loc.AsRegister<CpuRegister>();
  Location maybe_temp_loc = TypeCheckNeedsATemporary(type_check_kind) ?
      locations->GetTemp(0) :
      Location::NoLocation();
  uint32_t class_offset = mirror::Object::ClassOffset().Int32Value();
  uint32_t super_offset = mirror::Class::SuperClassOffset().Int32Value();
  uint32_t component_offset = mirror::Class::ComponentTypeOffset().Int32Value();
  uint32_t primitive_offset = mirror::Class::PrimitiveTypeOffset().Int32Value();
  SlowPathCode* slow_path = nullptr;
  NearLabel done, zero;

  // Return 0 if `obj` is null.
  // Avoid null check if we know obj is not null.
  if (instruction->MustDoNullCheck()) {
    __ testl(obj, obj);
    __ j(kEqual, &zero);
  }

  // /* HeapReference<Class> */ out = obj->klass_
  GenerateReferenceLoadTwoRegisters(instruction, out_loc, obj_loc, class_offset, maybe_temp_loc);

  switch (type_check_kind) {
    case TypeCheckKind::kExactCheck: {
      if (cls.IsRegister()) {
        __ cmpl(out, cls.AsRegister<CpuRegister>());
      } else {
        DCHECK(cls.IsStackSlot()) << cls;
        __ cmpl(out, Address(CpuRegister(RSP), cls.GetStackIndex()));
      }
      if (zero.IsLinked()) {
        // Classes must be equal for the instanceof to succeed.
        __ j(kNotEqual, &zero);
        __ movl(out, Immediate(1));
        __ jmp(&done);
      } else {
        __ setcc(kEqual, out);
        // setcc only sets the low byte.
        __ andl(out, Immediate(1));
      }
      break;
    }

    case TypeCheckKind::kAbstractClassCheck: {
      // If the class is abstract, we eagerly fetch the super class of the
      // object to avoid doing a comparison we know will fail.
      NearLabel loop, success;
      __ Bind(&loop);
      // /* HeapReference<Class> */ out = out->super_class_
      GenerateReferenceLoadOneRegister(instruction, out_loc, super_offset, maybe_temp_loc);
      __ testl(out, out);
      // If `out` is null, we use it for the result, and jump to `done`.
      __ j(kEqual, &done);
      if (cls.IsRegister()) {
        __ cmpl(out, cls.AsRegister<CpuRegister>());
      } else {
        DCHECK(cls.IsStackSlot()) << cls;
        __ cmpl(out, Address(CpuRegister(RSP), cls.GetStackIndex()));
      }
      __ j(kNotEqual, &loop);
      __ movl(out, Immediate(1));
      if (zero.IsLinked()) {
        __ jmp(&done);
      }
      break;
    }

    case TypeCheckKind::kClassHierarchyCheck: {
      // Walk over the class hierarchy to find a match.
      NearLabel loop, success;
      __ Bind(&loop);
      if (cls.IsRegister()) {
        __ cmpl(out, cls.AsRegister<CpuRegister>());
      } else {
        DCHECK(cls.IsStackSlot()) << cls;
        __ cmpl(out, Address(CpuRegister(RSP), cls.GetStackIndex()));
      }
      __ j(kEqual, &success);
      // /* HeapReference<Class> */ out = out->super_class_
      GenerateReferenceLoadOneRegister(instruction, out_loc, super_offset, maybe_temp_loc);
      __ testl(out, out);
      __ j(kNotEqual, &loop);
      // If `out` is null, we use it for the result, and jump to `done`.
      __ jmp(&done);
      __ Bind(&success);
      __ movl(out, Immediate(1));
      if (zero.IsLinked()) {
        __ jmp(&done);
      }
      break;
    }

    case TypeCheckKind::kArrayObjectCheck: {
      // Do an exact check.
      NearLabel exact_check;
      if (cls.IsRegister()) {
        __ cmpl(out, cls.AsRegister<CpuRegister>());
      } else {
        DCHECK(cls.IsStackSlot()) << cls;
        __ cmpl(out, Address(CpuRegister(RSP), cls.GetStackIndex()));
      }
      __ j(kEqual, &exact_check);
      // Otherwise, we need to check that the object's class is a non-primitive array.
      // /* HeapReference<Class> */ out = out->component_type_
      GenerateReferenceLoadOneRegister(instruction, out_loc, component_offset, maybe_temp_loc);
      __ testl(out, out);
      // If `out` is null, we use it for the result, and jump to `done`.
      __ j(kEqual, &done);
      __ cmpw(Address(out, primitive_offset), Immediate(Primitive::kPrimNot));
      __ j(kNotEqual, &zero);
      __ Bind(&exact_check);
      __ movl(out, Immediate(1));
      __ jmp(&done);
      break;
    }

    case TypeCheckKind::kArrayCheck: {
      if (cls.IsRegister()) {
        __ cmpl(out, cls.AsRegister<CpuRegister>());
      } else {
        DCHECK(cls.IsStackSlot()) << cls;
        __ cmpl(out, Address(CpuRegister(RSP), cls.GetStackIndex()));
      }
      DCHECK(locations->OnlyCallsOnSlowPath());
      slow_path = new (GetGraph()->GetArena()) TypeCheckSlowPathX86_64(instruction,
                                                                       /* is_fatal */ false);
      codegen_->AddSlowPath(slow_path);
      __ j(kNotEqual, slow_path->GetEntryLabel());
      __ movl(out, Immediate(1));
      if (zero.IsLinked()) {
        __ jmp(&done);
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
      slow_path = new (GetGraph()->GetArena()) TypeCheckSlowPathX86_64(instruction,
                                                                       /* is_fatal */ false);
      codegen_->AddSlowPath(slow_path);
      __ jmp(slow_path->GetEntryLabel());
      if (zero.IsLinked()) {
        __ jmp(&done);
      }
      break;
    }
  }

  if (zero.IsLinked()) {
    __ Bind(&zero);
    __ xorl(out, out);
  }

  if (done.IsLinked()) {
    __ Bind(&done);
  }

  if (slow_path != nullptr) {
    __ Bind(slow_path->GetExitLabel());
  }
}

void LocationsBuilderX86_64::VisitCheckCast(HCheckCast* instruction) {
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
  locations->SetInAt(1, Location::Any());
  // Note that TypeCheckSlowPathX86_64 uses this "temp" register too.
  locations->AddTemp(Location::RequiresRegister());
  // When read barriers are enabled, we need an additional temporary
  // register for some cases.
  if (TypeCheckNeedsATemporary(type_check_kind)) {
    locations->AddTemp(Location::RequiresRegister());
  }
}

void InstructionCodeGeneratorX86_64::VisitCheckCast(HCheckCast* instruction) {
  TypeCheckKind type_check_kind = instruction->GetTypeCheckKind();
  LocationSummary* locations = instruction->GetLocations();
  Location obj_loc = locations->InAt(0);
  CpuRegister obj = obj_loc.AsRegister<CpuRegister>();
  Location cls = locations->InAt(1);
  Location temp_loc = locations->GetTemp(0);
  CpuRegister temp = temp_loc.AsRegister<CpuRegister>();
  Location maybe_temp2_loc = TypeCheckNeedsATemporary(type_check_kind) ?
      locations->GetTemp(1) :
      Location::NoLocation();
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
  SlowPathCode* type_check_slow_path =
      new (GetGraph()->GetArena()) TypeCheckSlowPathX86_64(instruction,
                                                           is_type_check_slow_path_fatal);
  codegen_->AddSlowPath(type_check_slow_path);

  switch (type_check_kind) {
    case TypeCheckKind::kExactCheck:
    case TypeCheckKind::kArrayCheck: {
      NearLabel done;
      // Avoid null check if we know obj is not null.
      if (instruction->MustDoNullCheck()) {
        __ testl(obj, obj);
        __ j(kEqual, &done);
      }

      // /* HeapReference<Class> */ temp = obj->klass_
      GenerateReferenceLoadTwoRegisters(
          instruction, temp_loc, obj_loc, class_offset, maybe_temp2_loc);

      if (cls.IsRegister()) {
        __ cmpl(temp, cls.AsRegister<CpuRegister>());
      } else {
        DCHECK(cls.IsStackSlot()) << cls;
        __ cmpl(temp, Address(CpuRegister(RSP), cls.GetStackIndex()));
      }
      // Jump to slow path for throwing the exception or doing a
      // more involved array check.
      __ j(kNotEqual, type_check_slow_path->GetEntryLabel());
      __ Bind(&done);
      break;
    }

    case TypeCheckKind::kAbstractClassCheck: {
      NearLabel done;
      // Avoid null check if we know obj is not null.
      if (instruction->MustDoNullCheck()) {
        __ testl(obj, obj);
        __ j(kEqual, &done);
      }

      // /* HeapReference<Class> */ temp = obj->klass_
      GenerateReferenceLoadTwoRegisters(
          instruction, temp_loc, obj_loc, class_offset, maybe_temp2_loc);

      // If the class is abstract, we eagerly fetch the super class of the
      // object to avoid doing a comparison we know will fail.
      NearLabel loop, compare_classes;
      __ Bind(&loop);
      // /* HeapReference<Class> */ temp = temp->super_class_
      GenerateReferenceLoadOneRegister(instruction, temp_loc, super_offset, maybe_temp2_loc);

      // If the class reference currently in `temp` is not null, jump
      // to the `compare_classes` label to compare it with the checked
      // class.
      __ testl(temp, temp);
      __ j(kNotEqual, &compare_classes);
      // Otherwise, jump to the slow path to throw the exception.
      //
      // But before, move back the object's class into `temp` before
      // going into the slow path, as it has been overwritten in the
      // meantime.
      // /* HeapReference<Class> */ temp = obj->klass_
      GenerateReferenceLoadTwoRegisters(
          instruction, temp_loc, obj_loc, class_offset, maybe_temp2_loc);
      __ jmp(type_check_slow_path->GetEntryLabel());

      __ Bind(&compare_classes);
      if (cls.IsRegister()) {
        __ cmpl(temp, cls.AsRegister<CpuRegister>());
      } else {
        DCHECK(cls.IsStackSlot()) << cls;
        __ cmpl(temp, Address(CpuRegister(RSP), cls.GetStackIndex()));
      }
      __ j(kNotEqual, &loop);
      __ Bind(&done);
      break;
    }

    case TypeCheckKind::kClassHierarchyCheck: {
      NearLabel done;
      // Avoid null check if we know obj is not null.
      if (instruction->MustDoNullCheck()) {
        __ testl(obj, obj);
        __ j(kEqual, &done);
      }

      // /* HeapReference<Class> */ temp = obj->klass_
      GenerateReferenceLoadTwoRegisters(
          instruction, temp_loc, obj_loc, class_offset, maybe_temp2_loc);

      // Walk over the class hierarchy to find a match.
      NearLabel loop;
      __ Bind(&loop);
      if (cls.IsRegister()) {
        __ cmpl(temp, cls.AsRegister<CpuRegister>());
      } else {
        DCHECK(cls.IsStackSlot()) << cls;
        __ cmpl(temp, Address(CpuRegister(RSP), cls.GetStackIndex()));
      }
      __ j(kEqual, &done);

      // /* HeapReference<Class> */ temp = temp->super_class_
      GenerateReferenceLoadOneRegister(instruction, temp_loc, super_offset, maybe_temp2_loc);

      // If the class reference currently in `temp` is not null, jump
      // back at the beginning of the loop.
      __ testl(temp, temp);
      __ j(kNotEqual, &loop);
      // Otherwise, jump to the slow path to throw the exception.
      //
      // But before, move back the object's class into `temp` before
      // going into the slow path, as it has been overwritten in the
      // meantime.
      // /* HeapReference<Class> */ temp = obj->klass_
      GenerateReferenceLoadTwoRegisters(
          instruction, temp_loc, obj_loc, class_offset, maybe_temp2_loc);
      __ jmp(type_check_slow_path->GetEntryLabel());
      __ Bind(&done);
      break;
    }

    case TypeCheckKind::kArrayObjectCheck: {
      // We cannot use a NearLabel here, as its range might be too
      // short in some cases when read barriers are enabled.  This has
      // been observed for instance when the code emitted for this
      // case uses high x86-64 registers (R8-R15).
      Label done;
      // Avoid null check if we know obj is not null.
      if (instruction->MustDoNullCheck()) {
        __ testl(obj, obj);
        __ j(kEqual, &done);
      }

      // /* HeapReference<Class> */ temp = obj->klass_
      GenerateReferenceLoadTwoRegisters(
          instruction, temp_loc, obj_loc, class_offset, maybe_temp2_loc);

      // Do an exact check.
      NearLabel check_non_primitive_component_type;
      if (cls.IsRegister()) {
        __ cmpl(temp, cls.AsRegister<CpuRegister>());
      } else {
        DCHECK(cls.IsStackSlot()) << cls;
        __ cmpl(temp, Address(CpuRegister(RSP), cls.GetStackIndex()));
      }
      __ j(kEqual, &done);

      // Otherwise, we need to check that the object's class is a non-primitive array.
      // /* HeapReference<Class> */ temp = temp->component_type_
      GenerateReferenceLoadOneRegister(instruction, temp_loc, component_offset, maybe_temp2_loc);

      // If the component type is not null (i.e. the object is indeed
      // an array), jump to label `check_non_primitive_component_type`
      // to further check that this component type is not a primitive
      // type.
      __ testl(temp, temp);
      __ j(kNotEqual, &check_non_primitive_component_type);
      // Otherwise, jump to the slow path to throw the exception.
      //
      // But before, move back the object's class into `temp` before
      // going into the slow path, as it has been overwritten in the
      // meantime.
      // /* HeapReference<Class> */ temp = obj->klass_
      GenerateReferenceLoadTwoRegisters(
          instruction, temp_loc, obj_loc, class_offset, maybe_temp2_loc);
      __ jmp(type_check_slow_path->GetEntryLabel());

      __ Bind(&check_non_primitive_component_type);
      __ cmpw(Address(temp, primitive_offset), Immediate(Primitive::kPrimNot));
      __ j(kEqual, &done);
      // Same comment as above regarding `temp` and the slow path.
      // /* HeapReference<Class> */ temp = obj->klass_
      GenerateReferenceLoadTwoRegisters(
          instruction, temp_loc, obj_loc, class_offset, maybe_temp2_loc);
      __ jmp(type_check_slow_path->GetEntryLabel());
      __ Bind(&done);
      break;
    }

    case TypeCheckKind::kUnresolvedCheck:
    case TypeCheckKind::kInterfaceCheck:
      NearLabel done;
      // Avoid null check if we know obj is not null.
      if (instruction->MustDoNullCheck()) {
        __ testl(obj, obj);
        __ j(kEqual, &done);
      }

      // /* HeapReference<Class> */ temp = obj->klass_
      GenerateReferenceLoadTwoRegisters(
          instruction, temp_loc, obj_loc, class_offset, maybe_temp2_loc);

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
      __ jmp(type_check_slow_path->GetEntryLabel());
      __ Bind(&done);
      break;
  }

  __ Bind(type_check_slow_path->GetExitLabel());
}

void LocationsBuilderX86_64::VisitMonitorOperation(HMonitorOperation* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCall);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
}

void InstructionCodeGeneratorX86_64::VisitMonitorOperation(HMonitorOperation* instruction) {
  codegen_->InvokeRuntime(instruction->IsEnter() ? QUICK_ENTRY_POINT(pLockObject)
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

void LocationsBuilderX86_64::VisitAnd(HAnd* instruction) { HandleBitwiseOperation(instruction); }
void LocationsBuilderX86_64::VisitOr(HOr* instruction) { HandleBitwiseOperation(instruction); }
void LocationsBuilderX86_64::VisitXor(HXor* instruction) { HandleBitwiseOperation(instruction); }

void LocationsBuilderX86_64::HandleBitwiseOperation(HBinaryOperation* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  DCHECK(instruction->GetResultType() == Primitive::kPrimInt
         || instruction->GetResultType() == Primitive::kPrimLong);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::Any());
  locations->SetOut(Location::SameAsFirstInput());
}

void InstructionCodeGeneratorX86_64::VisitAnd(HAnd* instruction) {
  HandleBitwiseOperation(instruction);
}

void InstructionCodeGeneratorX86_64::VisitOr(HOr* instruction) {
  HandleBitwiseOperation(instruction);
}

void InstructionCodeGeneratorX86_64::VisitXor(HXor* instruction) {
  HandleBitwiseOperation(instruction);
}

void InstructionCodeGeneratorX86_64::HandleBitwiseOperation(HBinaryOperation* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Location first = locations->InAt(0);
  Location second = locations->InAt(1);
  DCHECK(first.Equals(locations->Out()));

  if (instruction->GetResultType() == Primitive::kPrimInt) {
    if (second.IsRegister()) {
      if (instruction->IsAnd()) {
        __ andl(first.AsRegister<CpuRegister>(), second.AsRegister<CpuRegister>());
      } else if (instruction->IsOr()) {
        __ orl(first.AsRegister<CpuRegister>(), second.AsRegister<CpuRegister>());
      } else {
        DCHECK(instruction->IsXor());
        __ xorl(first.AsRegister<CpuRegister>(), second.AsRegister<CpuRegister>());
      }
    } else if (second.IsConstant()) {
      Immediate imm(second.GetConstant()->AsIntConstant()->GetValue());
      if (instruction->IsAnd()) {
        __ andl(first.AsRegister<CpuRegister>(), imm);
      } else if (instruction->IsOr()) {
        __ orl(first.AsRegister<CpuRegister>(), imm);
      } else {
        DCHECK(instruction->IsXor());
        __ xorl(first.AsRegister<CpuRegister>(), imm);
      }
    } else {
      Address address(CpuRegister(RSP), second.GetStackIndex());
      if (instruction->IsAnd()) {
        __ andl(first.AsRegister<CpuRegister>(), address);
      } else if (instruction->IsOr()) {
        __ orl(first.AsRegister<CpuRegister>(), address);
      } else {
        DCHECK(instruction->IsXor());
        __ xorl(first.AsRegister<CpuRegister>(), address);
      }
    }
  } else {
    DCHECK_EQ(instruction->GetResultType(), Primitive::kPrimLong);
    CpuRegister first_reg = first.AsRegister<CpuRegister>();
    bool second_is_constant = false;
    int64_t value = 0;
    if (second.IsConstant()) {
      second_is_constant = true;
      value = second.GetConstant()->AsLongConstant()->GetValue();
    }
    bool is_int32_value = IsInt<32>(value);

    if (instruction->IsAnd()) {
      if (second_is_constant) {
        if (is_int32_value) {
          __ andq(first_reg, Immediate(static_cast<int32_t>(value)));
        } else {
          __ andq(first_reg, codegen_->LiteralInt64Address(value));
        }
      } else if (second.IsDoubleStackSlot()) {
        __ andq(first_reg, Address(CpuRegister(RSP), second.GetStackIndex()));
      } else {
        __ andq(first_reg, second.AsRegister<CpuRegister>());
      }
    } else if (instruction->IsOr()) {
      if (second_is_constant) {
        if (is_int32_value) {
          __ orq(first_reg, Immediate(static_cast<int32_t>(value)));
        } else {
          __ orq(first_reg, codegen_->LiteralInt64Address(value));
        }
      } else if (second.IsDoubleStackSlot()) {
        __ orq(first_reg, Address(CpuRegister(RSP), second.GetStackIndex()));
      } else {
        __ orq(first_reg, second.AsRegister<CpuRegister>());
      }
    } else {
      DCHECK(instruction->IsXor());
      if (second_is_constant) {
        if (is_int32_value) {
          __ xorq(first_reg, Immediate(static_cast<int32_t>(value)));
        } else {
          __ xorq(first_reg, codegen_->LiteralInt64Address(value));
        }
      } else if (second.IsDoubleStackSlot()) {
        __ xorq(first_reg, Address(CpuRegister(RSP), second.GetStackIndex()));
      } else {
        __ xorq(first_reg, second.AsRegister<CpuRegister>());
      }
    }
  }
}

void InstructionCodeGeneratorX86_64::GenerateReferenceLoadOneRegister(HInstruction* instruction,
                                                                      Location out,
                                                                      uint32_t offset,
                                                                      Location maybe_temp) {
  CpuRegister out_reg = out.AsRegister<CpuRegister>();
  if (kEmitCompilerReadBarrier) {
    DCHECK(maybe_temp.IsRegister()) << maybe_temp;
    if (kUseBakerReadBarrier) {
      // Load with fast path based Baker's read barrier.
      // /* HeapReference<Object> */ out = *(out + offset)
      codegen_->GenerateFieldLoadWithBakerReadBarrier(
          instruction, out, out_reg, offset, maybe_temp, /* needs_null_check */ false);
    } else {
      // Load with slow path based read barrier.
      // Save the value of `out` into `maybe_temp` before overwriting it
      // in the following move operation, as we will need it for the
      // read barrier below.
      __ movl(maybe_temp.AsRegister<CpuRegister>(), out_reg);
      // /* HeapReference<Object> */ out = *(out + offset)
      __ movl(out_reg, Address(out_reg, offset));
      codegen_->GenerateReadBarrierSlow(instruction, out, out, maybe_temp, offset);
    }
  } else {
    // Plain load with no read barrier.
    // /* HeapReference<Object> */ out = *(out + offset)
    __ movl(out_reg, Address(out_reg, offset));
    __ MaybeUnpoisonHeapReference(out_reg);
  }
}

void InstructionCodeGeneratorX86_64::GenerateReferenceLoadTwoRegisters(HInstruction* instruction,
                                                                       Location out,
                                                                       Location obj,
                                                                       uint32_t offset,
                                                                       Location maybe_temp) {
  CpuRegister out_reg = out.AsRegister<CpuRegister>();
  CpuRegister obj_reg = obj.AsRegister<CpuRegister>();
  if (kEmitCompilerReadBarrier) {
    if (kUseBakerReadBarrier) {
      DCHECK(maybe_temp.IsRegister()) << maybe_temp;
      // Load with fast path based Baker's read barrier.
      // /* HeapReference<Object> */ out = *(obj + offset)
      codegen_->GenerateFieldLoadWithBakerReadBarrier(
          instruction, out, obj_reg, offset, maybe_temp, /* needs_null_check */ false);
    } else {
      // Load with slow path based read barrier.
      // /* HeapReference<Object> */ out = *(obj + offset)
      __ movl(out_reg, Address(obj_reg, offset));
      codegen_->GenerateReadBarrierSlow(instruction, out, out, obj, offset);
    }
  } else {
    // Plain load with no read barrier.
    // /* HeapReference<Object> */ out = *(obj + offset)
    __ movl(out_reg, Address(obj_reg, offset));
    __ MaybeUnpoisonHeapReference(out_reg);
  }
}

void InstructionCodeGeneratorX86_64::GenerateGcRootFieldLoad(HInstruction* instruction,
                                                             Location root,
                                                             const Address& address,
                                                             Label* fixup_label) {
  CpuRegister root_reg = root.AsRegister<CpuRegister>();
  if (kEmitCompilerReadBarrier) {
    if (kUseBakerReadBarrier) {
      // Fast path implementation of art::ReadBarrier::BarrierForRoot when
      // Baker's read barrier are used:
      //
      //   root = *address;
      //   if (Thread::Current()->GetIsGcMarking()) {
      //     root = ReadBarrier::Mark(root)
      //   }

      // /* GcRoot<mirror::Object> */ root = *address
      __ movl(root_reg, address);
      if (fixup_label != nullptr) {
        __ Bind(fixup_label);
      }
      static_assert(
          sizeof(mirror::CompressedReference<mirror::Object>) == sizeof(GcRoot<mirror::Object>),
          "art::mirror::CompressedReference<mirror::Object> and art::GcRoot<mirror::Object> "
          "have different sizes.");
      static_assert(sizeof(mirror::CompressedReference<mirror::Object>) == sizeof(int32_t),
                    "art::mirror::CompressedReference<mirror::Object> and int32_t "
                    "have different sizes.");

      // Slow path used to mark the GC root `root`.
      SlowPathCode* slow_path =
          new (GetGraph()->GetArena()) ReadBarrierMarkSlowPathX86_64(instruction, root, root);
      codegen_->AddSlowPath(slow_path);

      __ gs()->cmpl(Address::Absolute(Thread::IsGcMarkingOffset<kX86_64WordSize>().Int32Value(),
                                      /* no_rip */ true),
                    Immediate(0));
      __ j(kNotEqual, slow_path->GetEntryLabel());
      __ Bind(slow_path->GetExitLabel());
    } else {
      // GC root loaded through a slow path for read barriers other
      // than Baker's.
      // /* GcRoot<mirror::Object>* */ root = address
      __ leaq(root_reg, address);
      if (fixup_label != nullptr) {
        __ Bind(fixup_label);
      }
      // /* mirror::Object* */ root = root->Read()
      codegen_->GenerateReadBarrierForRootSlow(instruction, root, root);
    }
  } else {
    // Plain GC root load with no read barrier.
    // /* GcRoot<mirror::Object> */ root = *address
    __ movl(root_reg, address);
    if (fixup_label != nullptr) {
      __ Bind(fixup_label);
    }
    // Note that GC roots are not affected by heap poisoning, thus we
    // do not have to unpoison `root_reg` here.
  }
}

void CodeGeneratorX86_64::GenerateFieldLoadWithBakerReadBarrier(HInstruction* instruction,
                                                                Location ref,
                                                                CpuRegister obj,
                                                                uint32_t offset,
                                                                Location temp,
                                                                bool needs_null_check) {
  DCHECK(kEmitCompilerReadBarrier);
  DCHECK(kUseBakerReadBarrier);

  // /* HeapReference<Object> */ ref = *(obj + offset)
  Address src(obj, offset);
  GenerateReferenceLoadWithBakerReadBarrier(instruction, ref, obj, src, temp, needs_null_check);
}

void CodeGeneratorX86_64::GenerateArrayLoadWithBakerReadBarrier(HInstruction* instruction,
                                                                Location ref,
                                                                CpuRegister obj,
                                                                uint32_t data_offset,
                                                                Location index,
                                                                Location temp,
                                                                bool needs_null_check) {
  DCHECK(kEmitCompilerReadBarrier);
  DCHECK(kUseBakerReadBarrier);

  // /* HeapReference<Object> */ ref =
  //     *(obj + data_offset + index * sizeof(HeapReference<Object>))
  Address src = index.IsConstant() ?
      Address(obj, (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_4) + data_offset) :
      Address(obj, index.AsRegister<CpuRegister>(), TIMES_4, data_offset);
  GenerateReferenceLoadWithBakerReadBarrier(instruction, ref, obj, src, temp, needs_null_check);
}

void CodeGeneratorX86_64::GenerateReferenceLoadWithBakerReadBarrier(HInstruction* instruction,
                                                                    Location ref,
                                                                    CpuRegister obj,
                                                                    const Address& src,
                                                                    Location temp,
                                                                    bool needs_null_check) {
  DCHECK(kEmitCompilerReadBarrier);
  DCHECK(kUseBakerReadBarrier);

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
  // slightly more complex as:
  // - it implements the load-load fence using a data dependency on
  //   the high-bits of rb_state, which are expected to be all zeroes
  //   (we use CodeGeneratorX86_64::GenerateMemoryBarrier instead
  //   here, which is a no-op thanks to the x86-64 memory model);
  // - it performs additional checks that we do not do here for
  //   performance reasons.

  CpuRegister ref_reg = ref.AsRegister<CpuRegister>();
  CpuRegister temp_reg = temp.AsRegister<CpuRegister>();
  uint32_t monitor_offset = mirror::Object::MonitorOffset().Int32Value();

  // /* int32_t */ monitor = obj->monitor_
  __ movl(temp_reg, Address(obj, monitor_offset));
  if (needs_null_check) {
    MaybeRecordImplicitNullCheck(instruction);
  }
  // /* LockWord */ lock_word = LockWord(monitor)
  static_assert(sizeof(LockWord) == sizeof(int32_t),
                "art::LockWord and int32_t have different sizes.");
  // /* uint32_t */ rb_state = lock_word.ReadBarrierState()
  __ shrl(temp_reg, Immediate(LockWord::kReadBarrierStateShift));
  __ andl(temp_reg, Immediate(LockWord::kReadBarrierStateMask));
  static_assert(
      LockWord::kReadBarrierStateMask == ReadBarrier::rb_ptr_mask_,
      "art::LockWord::kReadBarrierStateMask is not equal to art::ReadBarrier::rb_ptr_mask_.");

  // Load fence to prevent load-load reordering.
  // Note that this is a no-op, thanks to the x86-64 memory model.
  GenerateMemoryBarrier(MemBarrierKind::kLoadAny);

  // The actual reference load.
  // /* HeapReference<Object> */ ref = *src
  __ movl(ref_reg, src);

  // Object* ref = ref_addr->AsMirrorPtr()
  __ MaybeUnpoisonHeapReference(ref_reg);

  // Slow path used to mark the object `ref` when it is gray.
  SlowPathCode* slow_path =
      new (GetGraph()->GetArena()) ReadBarrierMarkSlowPathX86_64(instruction, ref, ref);
  AddSlowPath(slow_path);

  // if (rb_state == ReadBarrier::gray_ptr_)
  //   ref = ReadBarrier::Mark(ref);
  __ cmpl(temp_reg, Immediate(ReadBarrier::gray_ptr_));
  __ j(kEqual, slow_path->GetEntryLabel());
  __ Bind(slow_path->GetExitLabel());
}

void CodeGeneratorX86_64::GenerateReadBarrierSlow(HInstruction* instruction,
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
  SlowPathCode* slow_path = new (GetGraph()->GetArena())
      ReadBarrierForHeapReferenceSlowPathX86_64(instruction, out, ref, obj, offset, index);
  AddSlowPath(slow_path);

  __ jmp(slow_path->GetEntryLabel());
  __ Bind(slow_path->GetExitLabel());
}

void CodeGeneratorX86_64::MaybeGenerateReadBarrierSlow(HInstruction* instruction,
                                                       Location out,
                                                       Location ref,
                                                       Location obj,
                                                       uint32_t offset,
                                                       Location index) {
  if (kEmitCompilerReadBarrier) {
    // Baker's read barriers shall be handled by the fast path
    // (CodeGeneratorX86_64::GenerateReferenceLoadWithBakerReadBarrier).
    DCHECK(!kUseBakerReadBarrier);
    // If heap poisoning is enabled, unpoisoning will be taken care of
    // by the runtime within the slow path.
    GenerateReadBarrierSlow(instruction, out, ref, obj, offset, index);
  } else if (kPoisonHeapReferences) {
    __ UnpoisonHeapReference(out.AsRegister<CpuRegister>());
  }
}

void CodeGeneratorX86_64::GenerateReadBarrierForRootSlow(HInstruction* instruction,
                                                         Location out,
                                                         Location root) {
  DCHECK(kEmitCompilerReadBarrier);

  // Insert a slow path based read barrier *after* the GC root load.
  //
  // Note that GC roots are not affected by heap poisoning, so we do
  // not need to do anything special for this here.
  SlowPathCode* slow_path =
      new (GetGraph()->GetArena()) ReadBarrierForRootSlowPathX86_64(instruction, out, root);
  AddSlowPath(slow_path);

  __ jmp(slow_path->GetEntryLabel());
  __ Bind(slow_path->GetExitLabel());
}

void LocationsBuilderX86_64::VisitBoundType(HBoundType* instruction ATTRIBUTE_UNUSED) {
  // Nothing to do, this should be removed during prepare for register allocator.
  LOG(FATAL) << "Unreachable";
}

void InstructionCodeGeneratorX86_64::VisitBoundType(HBoundType* instruction ATTRIBUTE_UNUSED) {
  // Nothing to do, this should be removed during prepare for register allocator.
  LOG(FATAL) << "Unreachable";
}

// Simple implementation of packed switch - generate cascaded compare/jumps.
void LocationsBuilderX86_64::VisitPackedSwitch(HPackedSwitch* switch_instr) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(switch_instr, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->AddTemp(Location::RequiresRegister());
  locations->AddTemp(Location::RequiresRegister());
}

void InstructionCodeGeneratorX86_64::VisitPackedSwitch(HPackedSwitch* switch_instr) {
  int32_t lower_bound = switch_instr->GetStartValue();
  uint32_t num_entries = switch_instr->GetNumEntries();
  LocationSummary* locations = switch_instr->GetLocations();
  CpuRegister value_reg_in = locations->InAt(0).AsRegister<CpuRegister>();
  CpuRegister temp_reg = locations->GetTemp(0).AsRegister<CpuRegister>();
  CpuRegister base_reg = locations->GetTemp(1).AsRegister<CpuRegister>();
  HBasicBlock* default_block = switch_instr->GetDefaultBlock();

  // Should we generate smaller inline compare/jumps?
  if (num_entries <= kPackedSwitchJumpTableThreshold) {
    // Figure out the correct compare values and jump conditions.
    // Handle the first compare/branch as a special case because it might
    // jump to the default case.
    DCHECK_GT(num_entries, 2u);
    Condition first_condition;
    uint32_t index;
    const ArenaVector<HBasicBlock*>& successors = switch_instr->GetBlock()->GetSuccessors();
    if (lower_bound != 0) {
      first_condition = kLess;
      __ cmpl(value_reg_in, Immediate(lower_bound));
      __ j(first_condition, codegen_->GetLabelOf(default_block));
      __ j(kEqual, codegen_->GetLabelOf(successors[0]));

      index = 1;
    } else {
      // Handle all the compare/jumps below.
      first_condition = kBelow;
      index = 0;
    }

    // Handle the rest of the compare/jumps.
    for (; index + 1 < num_entries; index += 2) {
      int32_t compare_to_value = lower_bound + index + 1;
      __ cmpl(value_reg_in, Immediate(compare_to_value));
      // Jump to successors[index] if value < case_value[index].
      __ j(first_condition, codegen_->GetLabelOf(successors[index]));
      // Jump to successors[index + 1] if value == case_value[index + 1].
      __ j(kEqual, codegen_->GetLabelOf(successors[index + 1]));
    }

    if (index != num_entries) {
      // There are an odd number of entries. Handle the last one.
      DCHECK_EQ(index + 1, num_entries);
      __ cmpl(value_reg_in, Immediate(static_cast<int32_t>(lower_bound + index)));
      __ j(kEqual, codegen_->GetLabelOf(successors[index]));
    }

    // And the default for any other value.
    if (!codegen_->GoesToNextBlock(switch_instr->GetBlock(), default_block)) {
      __ jmp(codegen_->GetLabelOf(default_block));
    }
    return;
  }

  // Remove the bias, if needed.
  Register value_reg_out = value_reg_in.AsRegister();
  if (lower_bound != 0) {
    __ leal(temp_reg, Address(value_reg_in, -lower_bound));
    value_reg_out = temp_reg.AsRegister();
  }
  CpuRegister value_reg(value_reg_out);

  // Is the value in range?
  __ cmpl(value_reg, Immediate(num_entries - 1));
  __ j(kAbove, codegen_->GetLabelOf(default_block));

  // We are in the range of the table.
  // Load the address of the jump table in the constant area.
  __ leaq(base_reg, codegen_->LiteralCaseTable(switch_instr));

  // Load the (signed) offset from the jump table.
  __ movsxd(temp_reg, Address(base_reg, value_reg, TIMES_4, 0));

  // Add the offset to the address of the table base.
  __ addq(temp_reg, base_reg);

  // And jump.
  __ jmp(temp_reg);
}

void CodeGeneratorX86_64::Load32BitValue(CpuRegister dest, int32_t value) {
  if (value == 0) {
    __ xorl(dest, dest);
  } else {
    __ movl(dest, Immediate(value));
  }
}

void CodeGeneratorX86_64::Load64BitValue(CpuRegister dest, int64_t value) {
  if (value == 0) {
    // Clears upper bits too.
    __ xorl(dest, dest);
  } else if (IsUint<32>(value)) {
    // We can use a 32 bit move, as it will zero-extend and is shorter.
    __ movl(dest, Immediate(static_cast<int32_t>(value)));
  } else {
    __ movq(dest, Immediate(value));
  }
}

void CodeGeneratorX86_64::Load32BitValue(XmmRegister dest, int32_t value) {
  if (value == 0) {
    __ xorps(dest, dest);
  } else {
    __ movss(dest, LiteralInt32Address(value));
  }
}

void CodeGeneratorX86_64::Load64BitValue(XmmRegister dest, int64_t value) {
  if (value == 0) {
    __ xorpd(dest, dest);
  } else {
    __ movsd(dest, LiteralInt64Address(value));
  }
}

void CodeGeneratorX86_64::Load32BitValue(XmmRegister dest, float value) {
  Load32BitValue(dest, bit_cast<int32_t, float>(value));
}

void CodeGeneratorX86_64::Load64BitValue(XmmRegister dest, double value) {
  Load64BitValue(dest, bit_cast<int64_t, double>(value));
}

void CodeGeneratorX86_64::Compare32BitValue(CpuRegister dest, int32_t value) {
  if (value == 0) {
    __ testl(dest, dest);
  } else {
    __ cmpl(dest, Immediate(value));
  }
}

void CodeGeneratorX86_64::Compare64BitValue(CpuRegister dest, int64_t value) {
  if (IsInt<32>(value)) {
    if (value == 0) {
      __ testq(dest, dest);
    } else {
      __ cmpq(dest, Immediate(static_cast<int32_t>(value)));
    }
  } else {
    // Value won't fit in an int.
    __ cmpq(dest, LiteralInt64Address(value));
  }
}

void CodeGeneratorX86_64::Store64BitValueToStack(Location dest, int64_t value) {
  DCHECK(dest.IsDoubleStackSlot());
  if (IsInt<32>(value)) {
    // Can move directly as an int32 constant.
    __ movq(Address(CpuRegister(RSP), dest.GetStackIndex()),
            Immediate(static_cast<int32_t>(value)));
  } else {
    Load64BitValue(CpuRegister(TMP), value);
    __ movq(Address(CpuRegister(RSP), dest.GetStackIndex()), CpuRegister(TMP));
  }
}

/**
 * Class to handle late fixup of offsets into constant area.
 */
class RIPFixup : public AssemblerFixup, public ArenaObject<kArenaAllocCodeGenerator> {
 public:
  RIPFixup(CodeGeneratorX86_64& codegen, size_t offset)
      : codegen_(&codegen), offset_into_constant_area_(offset) {}

 protected:
  void SetOffset(size_t offset) { offset_into_constant_area_ = offset; }

  CodeGeneratorX86_64* codegen_;

 private:
  void Process(const MemoryRegion& region, int pos) OVERRIDE {
    // Patch the correct offset for the instruction.  We use the address of the
    // 'next' instruction, which is 'pos' (patch the 4 bytes before).
    int32_t constant_offset = codegen_->ConstantAreaStart() + offset_into_constant_area_;
    int32_t relative_position = constant_offset - pos;

    // Patch in the right value.
    region.StoreUnaligned<int32_t>(pos - 4, relative_position);
  }

  // Location in constant area that the fixup refers to.
  size_t offset_into_constant_area_;
};

/**
 t * Class to handle late fixup of offsets to a jump table that will be created in the
 * constant area.
 */
class JumpTableRIPFixup : public RIPFixup {
 public:
  JumpTableRIPFixup(CodeGeneratorX86_64& codegen, HPackedSwitch* switch_instr)
      : RIPFixup(codegen, -1), switch_instr_(switch_instr) {}

  void CreateJumpTable() {
    X86_64Assembler* assembler = codegen_->GetAssembler();

    // Ensure that the reference to the jump table has the correct offset.
    const int32_t offset_in_constant_table = assembler->ConstantAreaSize();
    SetOffset(offset_in_constant_table);

    // Compute the offset from the start of the function to this jump table.
    const int32_t current_table_offset = assembler->CodeSize() + offset_in_constant_table;

    // Populate the jump table with the correct values for the jump table.
    int32_t num_entries = switch_instr_->GetNumEntries();
    HBasicBlock* block = switch_instr_->GetBlock();
    const ArenaVector<HBasicBlock*>& successors = block->GetSuccessors();
    // The value that we want is the target offset - the position of the table.
    for (int32_t i = 0; i < num_entries; i++) {
      HBasicBlock* b = successors[i];
      Label* l = codegen_->GetLabelOf(b);
      DCHECK(l->IsBound());
      int32_t offset_to_block = l->Position() - current_table_offset;
      assembler->AppendInt32(offset_to_block);
    }
  }

 private:
  const HPackedSwitch* switch_instr_;
};

void CodeGeneratorX86_64::Finalize(CodeAllocator* allocator) {
  // Generate the constant area if needed.
  X86_64Assembler* assembler = GetAssembler();
  if (!assembler->IsConstantAreaEmpty() || !fixups_to_jump_tables_.empty()) {
    // Align to 4 byte boundary to reduce cache misses, as the data is 4 and 8 byte values.
    assembler->Align(4, 0);
    constant_area_start_ = assembler->CodeSize();

    // Populate any jump tables.
    for (auto jump_table : fixups_to_jump_tables_) {
      jump_table->CreateJumpTable();
    }

    // And now add the constant area to the generated code.
    assembler->AddConstantArea();
  }

  // And finish up.
  CodeGenerator::Finalize(allocator);
}

Address CodeGeneratorX86_64::LiteralDoubleAddress(double v) {
  AssemblerFixup* fixup = new (GetGraph()->GetArena()) RIPFixup(*this, __ AddDouble(v));
  return Address::RIP(fixup);
}

Address CodeGeneratorX86_64::LiteralFloatAddress(float v) {
  AssemblerFixup* fixup = new (GetGraph()->GetArena()) RIPFixup(*this, __ AddFloat(v));
  return Address::RIP(fixup);
}

Address CodeGeneratorX86_64::LiteralInt32Address(int32_t v) {
  AssemblerFixup* fixup = new (GetGraph()->GetArena()) RIPFixup(*this, __ AddInt32(v));
  return Address::RIP(fixup);
}

Address CodeGeneratorX86_64::LiteralInt64Address(int64_t v) {
  AssemblerFixup* fixup = new (GetGraph()->GetArena()) RIPFixup(*this, __ AddInt64(v));
  return Address::RIP(fixup);
}

// TODO: trg as memory.
void CodeGeneratorX86_64::MoveFromReturnRegister(Location trg, Primitive::Type type) {
  if (!trg.IsValid()) {
    DCHECK_EQ(type, Primitive::kPrimVoid);
    return;
  }

  DCHECK_NE(type, Primitive::kPrimVoid);

  Location return_loc = InvokeDexCallingConventionVisitorX86_64().GetReturnLocation(type);
  if (trg.Equals(return_loc)) {
    return;
  }

  // Let the parallel move resolver take care of all of this.
  HParallelMove parallel_move(GetGraph()->GetArena());
  parallel_move.AddMove(return_loc, trg, type, nullptr);
  GetMoveResolver()->EmitNativeCode(&parallel_move);
}

Address CodeGeneratorX86_64::LiteralCaseTable(HPackedSwitch* switch_instr) {
  // Create a fixup to be used to create and address the jump table.
  JumpTableRIPFixup* table_fixup =
      new (GetGraph()->GetArena()) JumpTableRIPFixup(*this, switch_instr);

  // We have to populate the jump tables.
  fixups_to_jump_tables_.push_back(table_fixup);
  return Address::RIP(table_fixup);
}

void CodeGeneratorX86_64::MoveInt64ToAddress(const Address& addr_low,
                                             const Address& addr_high,
                                             int64_t v,
                                             HInstruction* instruction) {
  if (IsInt<32>(v)) {
    int32_t v_32 = v;
    __ movq(addr_low, Immediate(v_32));
    MaybeRecordImplicitNullCheck(instruction);
  } else {
    // Didn't fit in a register.  Do it in pieces.
    int32_t low_v = Low32Bits(v);
    int32_t high_v = High32Bits(v);
    __ movl(addr_low, Immediate(low_v));
    MaybeRecordImplicitNullCheck(instruction);
    __ movl(addr_high, Immediate(high_v));
  }
}

#undef __

}  // namespace x86_64
}  // namespace art
