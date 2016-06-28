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
#include "compiled_method.h"
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

template<class MirrorType>
class GcRoot;

namespace x86 {

static constexpr int kCurrentMethodStackOffset = 0;
static constexpr Register kMethodRegisterArgument = EAX;
static constexpr Register kCoreCalleeSaves[] = { EBP, ESI, EDI };

static constexpr int kC2ConditionMask = 0x400;

static constexpr int kFakeReturnRegister = Register(8);

#define __ down_cast<X86Assembler*>(codegen->GetAssembler())->
#define QUICK_ENTRY_POINT(x) QUICK_ENTRYPOINT_OFFSET(kX86WordSize, x).Int32Value()

class NullCheckSlowPathX86 : public SlowPathCode {
 public:
  explicit NullCheckSlowPathX86(HNullCheck* instruction) : SlowPathCode(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    CodeGeneratorX86* x86_codegen = down_cast<CodeGeneratorX86*>(codegen);
    __ Bind(GetEntryLabel());
    if (instruction_->CanThrowIntoCatchBlock()) {
      // Live registers will be restored in the catch block if caught.
      SaveLiveRegisters(codegen, instruction_->GetLocations());
    }
    x86_codegen->InvokeRuntime(QUICK_ENTRY_POINT(pThrowNullPointer),
                               instruction_,
                               instruction_->GetDexPc(),
                               this);
    CheckEntrypointTypes<kQuickThrowNullPointer, void, void>();
  }

  bool IsFatal() const OVERRIDE { return true; }

  const char* GetDescription() const OVERRIDE { return "NullCheckSlowPathX86"; }

 private:
  DISALLOW_COPY_AND_ASSIGN(NullCheckSlowPathX86);
};

class DivZeroCheckSlowPathX86 : public SlowPathCode {
 public:
  explicit DivZeroCheckSlowPathX86(HDivZeroCheck* instruction) : SlowPathCode(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    CodeGeneratorX86* x86_codegen = down_cast<CodeGeneratorX86*>(codegen);
    __ Bind(GetEntryLabel());
    if (instruction_->CanThrowIntoCatchBlock()) {
      // Live registers will be restored in the catch block if caught.
      SaveLiveRegisters(codegen, instruction_->GetLocations());
    }
    x86_codegen->InvokeRuntime(QUICK_ENTRY_POINT(pThrowDivZero),
                               instruction_,
                               instruction_->GetDexPc(),
                               this);
    CheckEntrypointTypes<kQuickThrowDivZero, void, void>();
  }

  bool IsFatal() const OVERRIDE { return true; }

  const char* GetDescription() const OVERRIDE { return "DivZeroCheckSlowPathX86"; }

 private:
  DISALLOW_COPY_AND_ASSIGN(DivZeroCheckSlowPathX86);
};

class DivRemMinusOneSlowPathX86 : public SlowPathCode {
 public:
  DivRemMinusOneSlowPathX86(HInstruction* instruction, Register reg, bool is_div)
      : SlowPathCode(instruction), reg_(reg), is_div_(is_div) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    __ Bind(GetEntryLabel());
    if (is_div_) {
      __ negl(reg_);
    } else {
      __ movl(reg_, Immediate(0));
    }
    __ jmp(GetExitLabel());
  }

  const char* GetDescription() const OVERRIDE { return "DivRemMinusOneSlowPathX86"; }

 private:
  Register reg_;
  bool is_div_;
  DISALLOW_COPY_AND_ASSIGN(DivRemMinusOneSlowPathX86);
};

class BoundsCheckSlowPathX86 : public SlowPathCode {
 public:
  explicit BoundsCheckSlowPathX86(HBoundsCheck* instruction) : SlowPathCode(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    LocationSummary* locations = instruction_->GetLocations();
    CodeGeneratorX86* x86_codegen = down_cast<CodeGeneratorX86*>(codegen);
    __ Bind(GetEntryLabel());
    // We're moving two locations to locations that could overlap, so we need a parallel
    // move resolver.
    if (instruction_->CanThrowIntoCatchBlock()) {
      // Live registers will be restored in the catch block if caught.
      SaveLiveRegisters(codegen, instruction_->GetLocations());
    }
    InvokeRuntimeCallingConvention calling_convention;
    x86_codegen->EmitParallelMoves(
        locations->InAt(0),
        Location::RegisterLocation(calling_convention.GetRegisterAt(0)),
        Primitive::kPrimInt,
        locations->InAt(1),
        Location::RegisterLocation(calling_convention.GetRegisterAt(1)),
        Primitive::kPrimInt);
    x86_codegen->InvokeRuntime(QUICK_ENTRY_POINT(pThrowArrayBounds),
                               instruction_,
                               instruction_->GetDexPc(),
                               this);
    CheckEntrypointTypes<kQuickThrowArrayBounds, void, int32_t, int32_t>();
  }

  bool IsFatal() const OVERRIDE { return true; }

  const char* GetDescription() const OVERRIDE { return "BoundsCheckSlowPathX86"; }

 private:
  DISALLOW_COPY_AND_ASSIGN(BoundsCheckSlowPathX86);
};

class SuspendCheckSlowPathX86 : public SlowPathCode {
 public:
  SuspendCheckSlowPathX86(HSuspendCheck* instruction, HBasicBlock* successor)
      : SlowPathCode(instruction), successor_(successor) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    CodeGeneratorX86* x86_codegen = down_cast<CodeGeneratorX86*>(codegen);
    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, instruction_->GetLocations());
    x86_codegen->InvokeRuntime(QUICK_ENTRY_POINT(pTestSuspend),
                               instruction_,
                               instruction_->GetDexPc(),
                               this);
    CheckEntrypointTypes<kQuickTestSuspend, void, void>();
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

  const char* GetDescription() const OVERRIDE { return "SuspendCheckSlowPathX86"; }

 private:
  HBasicBlock* const successor_;
  Label return_label_;

  DISALLOW_COPY_AND_ASSIGN(SuspendCheckSlowPathX86);
};

class LoadStringSlowPathX86 : public SlowPathCode {
 public:
  explicit LoadStringSlowPathX86(HLoadString* instruction): SlowPathCode(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    LocationSummary* locations = instruction_->GetLocations();
    DCHECK(!locations->GetLiveRegisters()->ContainsCoreRegister(locations->Out().reg()));

    CodeGeneratorX86* x86_codegen = down_cast<CodeGeneratorX86*>(codegen);
    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, locations);

    InvokeRuntimeCallingConvention calling_convention;
    const uint32_t string_index = instruction_->AsLoadString()->GetStringIndex();
    __ movl(calling_convention.GetRegisterAt(0), Immediate(string_index));
    x86_codegen->InvokeRuntime(QUICK_ENTRY_POINT(pResolveString),
                               instruction_,
                               instruction_->GetDexPc(),
                               this);
    CheckEntrypointTypes<kQuickResolveString, void*, uint32_t>();
    x86_codegen->Move32(locations->Out(), Location::RegisterLocation(EAX));
    RestoreLiveRegisters(codegen, locations);

    __ jmp(GetExitLabel());
  }

  const char* GetDescription() const OVERRIDE { return "LoadStringSlowPathX86"; }

 private:
  DISALLOW_COPY_AND_ASSIGN(LoadStringSlowPathX86);
};

class LoadClassSlowPathX86 : public SlowPathCode {
 public:
  LoadClassSlowPathX86(HLoadClass* cls,
                       HInstruction* at,
                       uint32_t dex_pc,
                       bool do_clinit)
      : SlowPathCode(at), cls_(cls), at_(at), dex_pc_(dex_pc), do_clinit_(do_clinit) {
    DCHECK(at->IsLoadClass() || at->IsClinitCheck());
  }

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    LocationSummary* locations = at_->GetLocations();
    CodeGeneratorX86* x86_codegen = down_cast<CodeGeneratorX86*>(codegen);
    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, locations);

    InvokeRuntimeCallingConvention calling_convention;
    __ movl(calling_convention.GetRegisterAt(0), Immediate(cls_->GetTypeIndex()));
    x86_codegen->InvokeRuntime(do_clinit_ ? QUICK_ENTRY_POINT(pInitializeStaticStorage)
                                          : QUICK_ENTRY_POINT(pInitializeType),
                               at_, dex_pc_, this);
    if (do_clinit_) {
      CheckEntrypointTypes<kQuickInitializeStaticStorage, void*, uint32_t>();
    } else {
      CheckEntrypointTypes<kQuickInitializeType, void*, uint32_t>();
    }

    // Move the class to the desired location.
    Location out = locations->Out();
    if (out.IsValid()) {
      DCHECK(out.IsRegister() && !locations->GetLiveRegisters()->ContainsCoreRegister(out.reg()));
      x86_codegen->Move32(out, Location::RegisterLocation(EAX));
    }

    RestoreLiveRegisters(codegen, locations);
    __ jmp(GetExitLabel());
  }

  const char* GetDescription() const OVERRIDE { return "LoadClassSlowPathX86"; }

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

class TypeCheckSlowPathX86 : public SlowPathCode {
 public:
  TypeCheckSlowPathX86(HInstruction* instruction, bool is_fatal)
      : SlowPathCode(instruction), is_fatal_(is_fatal) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    LocationSummary* locations = instruction_->GetLocations();
    Location object_class = instruction_->IsCheckCast() ? locations->GetTemp(0)
                                                        : locations->Out();
    DCHECK(instruction_->IsCheckCast()
           || !locations->GetLiveRegisters()->ContainsCoreRegister(locations->Out().reg()));

    CodeGeneratorX86* x86_codegen = down_cast<CodeGeneratorX86*>(codegen);
    __ Bind(GetEntryLabel());

    if (!is_fatal_) {
      SaveLiveRegisters(codegen, locations);
    }

    // We're moving two locations to locations that could overlap, so we need a parallel
    // move resolver.
    InvokeRuntimeCallingConvention calling_convention;
    x86_codegen->EmitParallelMoves(
        locations->InAt(1),
        Location::RegisterLocation(calling_convention.GetRegisterAt(0)),
        Primitive::kPrimNot,
        object_class,
        Location::RegisterLocation(calling_convention.GetRegisterAt(1)),
        Primitive::kPrimNot);

    if (instruction_->IsInstanceOf()) {
      x86_codegen->InvokeRuntime(QUICK_ENTRY_POINT(pInstanceofNonTrivial),
                                 instruction_,
                                 instruction_->GetDexPc(),
                                 this);
      CheckEntrypointTypes<
          kQuickInstanceofNonTrivial, uint32_t, const mirror::Class*, const mirror::Class*>();
    } else {
      DCHECK(instruction_->IsCheckCast());
      x86_codegen->InvokeRuntime(QUICK_ENTRY_POINT(pCheckCast),
                                 instruction_,
                                 instruction_->GetDexPc(),
                                 this);
      CheckEntrypointTypes<kQuickCheckCast, void, const mirror::Class*, const mirror::Class*>();
    }

    if (!is_fatal_) {
      if (instruction_->IsInstanceOf()) {
        x86_codegen->Move32(locations->Out(), Location::RegisterLocation(EAX));
      }
      RestoreLiveRegisters(codegen, locations);

      __ jmp(GetExitLabel());
    }
  }

  const char* GetDescription() const OVERRIDE { return "TypeCheckSlowPathX86"; }
  bool IsFatal() const OVERRIDE { return is_fatal_; }

 private:
  const bool is_fatal_;

  DISALLOW_COPY_AND_ASSIGN(TypeCheckSlowPathX86);
};

class DeoptimizationSlowPathX86 : public SlowPathCode {
 public:
  explicit DeoptimizationSlowPathX86(HDeoptimize* instruction)
    : SlowPathCode(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    CodeGeneratorX86* x86_codegen = down_cast<CodeGeneratorX86*>(codegen);
    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, instruction_->GetLocations());
    x86_codegen->InvokeRuntime(QUICK_ENTRY_POINT(pDeoptimize),
                               instruction_,
                               instruction_->GetDexPc(),
                               this);
    CheckEntrypointTypes<kQuickDeoptimize, void, void>();
  }

  const char* GetDescription() const OVERRIDE { return "DeoptimizationSlowPathX86"; }

 private:
  DISALLOW_COPY_AND_ASSIGN(DeoptimizationSlowPathX86);
};

class ArraySetSlowPathX86 : public SlowPathCode {
 public:
  explicit ArraySetSlowPathX86(HInstruction* instruction) : SlowPathCode(instruction) {}

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

    CodeGeneratorX86* x86_codegen = down_cast<CodeGeneratorX86*>(codegen);
    x86_codegen->InvokeRuntime(QUICK_ENTRY_POINT(pAputObject),
                               instruction_,
                               instruction_->GetDexPc(),
                               this);
    CheckEntrypointTypes<kQuickAputObject, void, mirror::Array*, int32_t, mirror::Object*>();
    RestoreLiveRegisters(codegen, locations);
    __ jmp(GetExitLabel());
  }

  const char* GetDescription() const OVERRIDE { return "ArraySetSlowPathX86"; }

 private:
  DISALLOW_COPY_AND_ASSIGN(ArraySetSlowPathX86);
};

// Slow path marking an object during a read barrier.
class ReadBarrierMarkSlowPathX86 : public SlowPathCode {
 public:
  ReadBarrierMarkSlowPathX86(HInstruction* instruction, Location out, Location obj)
      : SlowPathCode(instruction), out_(out), obj_(obj) {
    DCHECK(kEmitCompilerReadBarrier);
  }

  const char* GetDescription() const OVERRIDE { return "ReadBarrierMarkSlowPathX86"; }

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
    CodeGeneratorX86* x86_codegen = down_cast<CodeGeneratorX86*>(codegen);
    x86_codegen->Move32(Location::RegisterLocation(calling_convention.GetRegisterAt(0)), obj_);
    x86_codegen->InvokeRuntime(QUICK_ENTRY_POINT(pReadBarrierMark),
                               instruction_,
                               instruction_->GetDexPc(),
                               this);
    CheckEntrypointTypes<kQuickReadBarrierMark, mirror::Object*, mirror::Object*>();
    x86_codegen->Move32(out_, Location::RegisterLocation(EAX));

    RestoreLiveRegisters(codegen, locations);
    __ jmp(GetExitLabel());
  }

 private:
  const Location out_;
  const Location obj_;

  DISALLOW_COPY_AND_ASSIGN(ReadBarrierMarkSlowPathX86);
};

// Slow path generating a read barrier for a heap reference.
class ReadBarrierForHeapReferenceSlowPathX86 : public SlowPathCode {
 public:
  ReadBarrierForHeapReferenceSlowPathX86(HInstruction* instruction,
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
    // If `obj` is equal to `out` or `ref`, it means the initial object
    // has been overwritten by (or after) the heap object reference load
    // to be instrumented, e.g.:
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
    CodeGeneratorX86* x86_codegen = down_cast<CodeGeneratorX86*>(codegen);
    LocationSummary* locations = instruction_->GetLocations();
    Register reg_out = out_.AsRegister<Register>();
    DCHECK(locations->CanCall());
    DCHECK(!locations->GetLiveRegisters()->ContainsCoreRegister(reg_out));
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
        // Compute the actual memory offset and store it in `index`.
        Register index_reg = index_.AsRegister<Register>();
        DCHECK(locations->GetLiveRegisters()->ContainsCoreRegister(index_reg));
        if (codegen->IsCoreCalleeSaveRegister(index_reg)) {
          // We are about to change the value of `index_reg` (see the
          // calls to art::x86::X86Assembler::shll and
          // art::x86::X86Assembler::AddImmediate below), but it has
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
          __ movl(free_reg, index_reg);
          index_reg = free_reg;
          index = Location::RegisterLocation(index_reg);
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
        __ shll(index_reg, Immediate(TIMES_4));
        static_assert(
            sizeof(mirror::HeapReference<mirror::Object>) == sizeof(int32_t),
            "art::mirror::HeapReference<art::mirror::Object> and int32_t have different sizes.");
        __ AddImmediate(index_reg, Immediate(offset_));
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
      __ movl(calling_convention.GetRegisterAt(2), Immediate(offset_));
    }
    x86_codegen->InvokeRuntime(QUICK_ENTRY_POINT(pReadBarrierSlow),
                               instruction_,
                               instruction_->GetDexPc(),
                               this);
    CheckEntrypointTypes<
        kQuickReadBarrierSlow, mirror::Object*, mirror::Object*, mirror::Object*, uint32_t>();
    x86_codegen->Move32(out_, Location::RegisterLocation(EAX));

    RestoreLiveRegisters(codegen, locations);
    __ jmp(GetExitLabel());
  }

  const char* GetDescription() const OVERRIDE { return "ReadBarrierForHeapReferenceSlowPathX86"; }

 private:
  Register FindAvailableCallerSaveRegister(CodeGenerator* codegen) {
    size_t ref = static_cast<int>(ref_.AsRegister<Register>());
    size_t obj = static_cast<int>(obj_.AsRegister<Register>());
    for (size_t i = 0, e = codegen->GetNumberOfCoreRegisters(); i < e; ++i) {
      if (i != ref && i != obj && !codegen->IsCoreCalleeSaveRegister(i)) {
        return static_cast<Register>(i);
      }
    }
    // We shall never fail to find a free caller-save register, as
    // there are more than two core caller-save registers on x86
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

  DISALLOW_COPY_AND_ASSIGN(ReadBarrierForHeapReferenceSlowPathX86);
};

// Slow path generating a read barrier for a GC root.
class ReadBarrierForRootSlowPathX86 : public SlowPathCode {
 public:
  ReadBarrierForRootSlowPathX86(HInstruction* instruction, Location out, Location root)
      : SlowPathCode(instruction), out_(out), root_(root) {
    DCHECK(kEmitCompilerReadBarrier);
  }

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    LocationSummary* locations = instruction_->GetLocations();
    Register reg_out = out_.AsRegister<Register>();
    DCHECK(locations->CanCall());
    DCHECK(!locations->GetLiveRegisters()->ContainsCoreRegister(reg_out));
    DCHECK(instruction_->IsLoadClass() || instruction_->IsLoadString())
        << "Unexpected instruction in read barrier for GC root slow path: "
        << instruction_->DebugName();

    __ Bind(GetEntryLabel());
    SaveLiveRegisters(codegen, locations);

    InvokeRuntimeCallingConvention calling_convention;
    CodeGeneratorX86* x86_codegen = down_cast<CodeGeneratorX86*>(codegen);
    x86_codegen->Move32(Location::RegisterLocation(calling_convention.GetRegisterAt(0)), root_);
    x86_codegen->InvokeRuntime(QUICK_ENTRY_POINT(pReadBarrierForRootSlow),
                               instruction_,
                               instruction_->GetDexPc(),
                               this);
    CheckEntrypointTypes<kQuickReadBarrierForRootSlow, mirror::Object*, GcRoot<mirror::Object>*>();
    x86_codegen->Move32(out_, Location::RegisterLocation(EAX));

    RestoreLiveRegisters(codegen, locations);
    __ jmp(GetExitLabel());
  }

  const char* GetDescription() const OVERRIDE { return "ReadBarrierForRootSlowPathX86"; }

 private:
  const Location out_;
  const Location root_;

  DISALLOW_COPY_AND_ASSIGN(ReadBarrierForRootSlowPathX86);
};

#undef __
#define __ down_cast<X86Assembler*>(GetAssembler())->

inline Condition X86Condition(IfCondition cond) {
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

// Maps signed condition to unsigned condition and FP condition to x86 name.
inline Condition X86UnsignedOrFPCondition(IfCondition cond) {
  switch (cond) {
    case kCondEQ: return kEqual;
    case kCondNE: return kNotEqual;
    // Signed to unsigned, and FP to x86 name.
    case kCondLT: return kBelow;
    case kCondLE: return kBelowEqual;
    case kCondGT: return kAbove;
    case kCondGE: return kAboveEqual;
    // Unsigned remain unchanged.
    case kCondB:  return kBelow;
    case kCondBE: return kBelowEqual;
    case kCondA:  return kAbove;
    case kCondAE: return kAboveEqual;
  }
  LOG(FATAL) << "Unreachable";
  UNREACHABLE();
}

void CodeGeneratorX86::DumpCoreRegister(std::ostream& stream, int reg) const {
  stream << Register(reg);
}

void CodeGeneratorX86::DumpFloatingPointRegister(std::ostream& stream, int reg) const {
  stream << XmmRegister(reg);
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

void CodeGeneratorX86::InvokeRuntime(QuickEntrypointEnum entrypoint,
                                     HInstruction* instruction,
                                     uint32_t dex_pc,
                                     SlowPathCode* slow_path) {
  InvokeRuntime(GetThreadOffset<kX86WordSize>(entrypoint).Int32Value(),
                instruction,
                dex_pc,
                slow_path);
}

void CodeGeneratorX86::InvokeRuntime(int32_t entry_point_offset,
                                     HInstruction* instruction,
                                     uint32_t dex_pc,
                                     SlowPathCode* slow_path) {
  ValidateInvokeRuntime(instruction, slow_path);
  __ fs()->call(Address::Absolute(entry_point_offset));
  RecordPcInfo(instruction, dex_pc, slow_path);
}

CodeGeneratorX86::CodeGeneratorX86(HGraph* graph,
                                   const X86InstructionSetFeatures& isa_features,
                                   const CompilerOptions& compiler_options,
                                   OptimizingCompilerStats* stats)
    : CodeGenerator(graph,
                    kNumberOfCpuRegisters,
                    kNumberOfXmmRegisters,
                    kNumberOfRegisterPairs,
                    ComputeRegisterMask(reinterpret_cast<const int*>(kCoreCalleeSaves),
                                        arraysize(kCoreCalleeSaves))
                        | (1 << kFakeReturnRegister),
                    0,
                    compiler_options,
                    stats),
      block_labels_(nullptr),
      location_builder_(graph, this),
      instruction_visitor_(graph, this),
      move_resolver_(graph->GetArena(), this),
      assembler_(graph->GetArena()),
      isa_features_(isa_features),
      method_patches_(graph->GetArena()->Adapter(kArenaAllocCodeGenerator)),
      relative_call_patches_(graph->GetArena()->Adapter(kArenaAllocCodeGenerator)),
      pc_relative_dex_cache_patches_(graph->GetArena()->Adapter(kArenaAllocCodeGenerator)),
      simple_patches_(graph->GetArena()->Adapter(kArenaAllocCodeGenerator)),
      string_patches_(graph->GetArena()->Adapter(kArenaAllocCodeGenerator)),
      constant_area_start_(-1),
      fixups_to_jump_tables_(graph->GetArena()->Adapter(kArenaAllocCodeGenerator)),
      method_address_offset_(-1) {
  // Use a fake return address register to mimic Quick.
  AddAllocatedRegister(Location::RegisterLocation(kFakeReturnRegister));
}

void CodeGeneratorX86::SetupBlockedRegisters() const {
  // Don't allocate the dalvik style register pair passing.
  blocked_register_pairs_[ECX_EDX] = true;

  // Stack register is always reserved.
  blocked_core_registers_[ESP] = true;

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
      : InstructionCodeGenerator(graph, codegen),
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
  __ movl(Address(ESP, kCurrentMethodStackOffset), kMethodRegisterArgument);
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

Location InvokeDexCallingConventionVisitorX86::GetReturnLocation(Primitive::Type type) const {
  switch (type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimInt:
    case Primitive::kPrimNot:
      return Location::RegisterLocation(EAX);

    case Primitive::kPrimLong:
      return Location::RegisterPairLocation(EAX, EDX);

    case Primitive::kPrimVoid:
      return Location::NoLocation();

    case Primitive::kPrimDouble:
    case Primitive::kPrimFloat:
      return Location::FpuRegisterLocation(XMM0);
  }

  UNREACHABLE();
}

Location InvokeDexCallingConventionVisitorX86::GetMethodLocation() const {
  return Location::RegisterLocation(kMethodRegisterArgument);
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
  return Location::NoLocation();
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
      XmmRegister src_reg = source.AsFpuRegister<XmmRegister>();
      __ movd(destination.AsRegisterPairLow<Register>(), src_reg);
      __ psrlq(src_reg, Immediate(32));
      __ movd(destination.AsRegisterPairHigh<Register>(), src_reg);
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
    } else if (source.IsRegisterPair()) {
      size_t elem_size = Primitive::ComponentSize(Primitive::kPrimInt);
      // Create stack space for 2 elements.
      __ subl(ESP, Immediate(2 * elem_size));
      __ movl(Address(ESP, 0), source.AsRegisterPairLow<Register>());
      __ movl(Address(ESP, elem_size), source.AsRegisterPairHigh<Register>());
      __ movsd(destination.AsFpuRegister<XmmRegister>(), Address(ESP, 0));
      // And remove the temporary stack space we allocated.
      __ addl(ESP, Immediate(2 * elem_size));
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

void CodeGeneratorX86::MoveConstant(Location location, int32_t value) {
  DCHECK(location.IsRegister());
  __ movl(location.AsRegister<Register>(), Immediate(value));
}

void CodeGeneratorX86::MoveLocation(Location dst, Location src, Primitive::Type dst_type) {
  HParallelMove move(GetGraph()->GetArena());
  if (dst_type == Primitive::kPrimLong && !src.IsConstant() && !src.IsFpuRegister()) {
    move.AddMove(src.ToLow(), dst.ToLow(), Primitive::kPrimInt, nullptr);
    move.AddMove(src.ToHigh(), dst.ToHigh(), Primitive::kPrimInt, nullptr);
  } else {
    move.AddMove(src, dst, dst_type, nullptr);
  }
  GetMoveResolver()->EmitNativeCode(&move);
}

void CodeGeneratorX86::AddLocationAsTemp(Location location, LocationSummary* locations) {
  if (location.IsRegister()) {
    locations->AddTemp(location);
  } else if (location.IsRegisterPair()) {
    locations->AddTemp(Location::RegisterLocation(location.AsRegisterPairLow<Register>()));
    locations->AddTemp(Location::RegisterLocation(location.AsRegisterPairHigh<Register>()));
  } else {
    UNIMPLEMENTED(FATAL) << "AddLocationAsTemp not implemented for location " << location;
  }
}

void InstructionCodeGeneratorX86::HandleGoto(HInstruction* got, HBasicBlock* successor) {
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

void LocationsBuilderX86::VisitGoto(HGoto* got) {
  got->SetLocations(nullptr);
}

void InstructionCodeGeneratorX86::VisitGoto(HGoto* got) {
  HandleGoto(got, got->GetSuccessor());
}

void LocationsBuilderX86::VisitTryBoundary(HTryBoundary* try_boundary) {
  try_boundary->SetLocations(nullptr);
}

void InstructionCodeGeneratorX86::VisitTryBoundary(HTryBoundary* try_boundary) {
  HBasicBlock* successor = try_boundary->GetNormalFlowSuccessor();
  if (!successor->IsExitBlock()) {
    HandleGoto(try_boundary, successor);
  }
}

void LocationsBuilderX86::VisitExit(HExit* exit) {
  exit->SetLocations(nullptr);
}

void InstructionCodeGeneratorX86::VisitExit(HExit* exit ATTRIBUTE_UNUSED) {
}

template<class LabelType>
void InstructionCodeGeneratorX86::GenerateFPJumps(HCondition* cond,
                                                  LabelType* true_label,
                                                  LabelType* false_label) {
  if (cond->IsFPConditionTrueIfNaN()) {
    __ j(kUnordered, true_label);
  } else if (cond->IsFPConditionFalseIfNaN()) {
    __ j(kUnordered, false_label);
  }
  __ j(X86UnsignedOrFPCondition(cond->GetCondition()), true_label);
}

template<class LabelType>
void InstructionCodeGeneratorX86::GenerateLongComparesAndJumps(HCondition* cond,
                                                               LabelType* true_label,
                                                               LabelType* false_label) {
  LocationSummary* locations = cond->GetLocations();
  Location left = locations->InAt(0);
  Location right = locations->InAt(1);
  IfCondition if_cond = cond->GetCondition();

  Register left_high = left.AsRegisterPairHigh<Register>();
  Register left_low = left.AsRegisterPairLow<Register>();
  IfCondition true_high_cond = if_cond;
  IfCondition false_high_cond = cond->GetOppositeCondition();
  Condition final_condition = X86UnsignedOrFPCondition(if_cond);  // unsigned on lower part

  // Set the conditions for the test, remembering that == needs to be
  // decided using the low words.
  switch (if_cond) {
    case kCondEQ:
    case kCondNE:
      // Nothing to do.
      break;
    case kCondLT:
      false_high_cond = kCondGT;
      break;
    case kCondLE:
      true_high_cond = kCondLT;
      break;
    case kCondGT:
      false_high_cond = kCondLT;
      break;
    case kCondGE:
      true_high_cond = kCondGT;
      break;
    case kCondB:
      false_high_cond = kCondA;
      break;
    case kCondBE:
      true_high_cond = kCondB;
      break;
    case kCondA:
      false_high_cond = kCondB;
      break;
    case kCondAE:
      true_high_cond = kCondA;
      break;
  }

  if (right.IsConstant()) {
    int64_t value = right.GetConstant()->AsLongConstant()->GetValue();
    int32_t val_high = High32Bits(value);
    int32_t val_low = Low32Bits(value);

    codegen_->Compare32BitValue(left_high, val_high);
    if (if_cond == kCondNE) {
      __ j(X86Condition(true_high_cond), true_label);
    } else if (if_cond == kCondEQ) {
      __ j(X86Condition(false_high_cond), false_label);
    } else {
      __ j(X86Condition(true_high_cond), true_label);
      __ j(X86Condition(false_high_cond), false_label);
    }
    // Must be equal high, so compare the lows.
    codegen_->Compare32BitValue(left_low, val_low);
  } else if (right.IsRegisterPair()) {
    Register right_high = right.AsRegisterPairHigh<Register>();
    Register right_low = right.AsRegisterPairLow<Register>();

    __ cmpl(left_high, right_high);
    if (if_cond == kCondNE) {
      __ j(X86Condition(true_high_cond), true_label);
    } else if (if_cond == kCondEQ) {
      __ j(X86Condition(false_high_cond), false_label);
    } else {
      __ j(X86Condition(true_high_cond), true_label);
      __ j(X86Condition(false_high_cond), false_label);
    }
    // Must be equal high, so compare the lows.
    __ cmpl(left_low, right_low);
  } else {
    DCHECK(right.IsDoubleStackSlot());
    __ cmpl(left_high, Address(ESP, right.GetHighStackIndex(kX86WordSize)));
    if (if_cond == kCondNE) {
      __ j(X86Condition(true_high_cond), true_label);
    } else if (if_cond == kCondEQ) {
      __ j(X86Condition(false_high_cond), false_label);
    } else {
      __ j(X86Condition(true_high_cond), true_label);
      __ j(X86Condition(false_high_cond), false_label);
    }
    // Must be equal high, so compare the lows.
    __ cmpl(left_low, Address(ESP, right.GetStackIndex()));
  }
  // The last comparison might be unsigned.
  __ j(final_condition, true_label);
}

void InstructionCodeGeneratorX86::GenerateFPCompare(Location lhs,
                                                    Location rhs,
                                                    HInstruction* insn,
                                                    bool is_double) {
  HX86LoadFromConstantTable* const_area = insn->InputAt(1)->AsX86LoadFromConstantTable();
  if (is_double) {
    if (rhs.IsFpuRegister()) {
      __ ucomisd(lhs.AsFpuRegister<XmmRegister>(), rhs.AsFpuRegister<XmmRegister>());
    } else if (const_area != nullptr) {
      DCHECK(const_area->IsEmittedAtUseSite());
      __ ucomisd(lhs.AsFpuRegister<XmmRegister>(),
                 codegen_->LiteralDoubleAddress(
                   const_area->GetConstant()->AsDoubleConstant()->GetValue(),
                   const_area->GetLocations()->InAt(0).AsRegister<Register>()));
    } else {
      DCHECK(rhs.IsDoubleStackSlot());
      __ ucomisd(lhs.AsFpuRegister<XmmRegister>(), Address(ESP, rhs.GetStackIndex()));
    }
  } else {
    if (rhs.IsFpuRegister()) {
      __ ucomiss(lhs.AsFpuRegister<XmmRegister>(), rhs.AsFpuRegister<XmmRegister>());
    } else if (const_area != nullptr) {
      DCHECK(const_area->IsEmittedAtUseSite());
      __ ucomiss(lhs.AsFpuRegister<XmmRegister>(),
                 codegen_->LiteralFloatAddress(
                   const_area->GetConstant()->AsFloatConstant()->GetValue(),
                   const_area->GetLocations()->InAt(0).AsRegister<Register>()));
    } else {
      DCHECK(rhs.IsStackSlot());
      __ ucomiss(lhs.AsFpuRegister<XmmRegister>(), Address(ESP, rhs.GetStackIndex()));
    }
  }
}

template<class LabelType>
void InstructionCodeGeneratorX86::GenerateCompareTestAndBranch(HCondition* condition,
                                                               LabelType* true_target_in,
                                                               LabelType* false_target_in) {
  // Generated branching requires both targets to be explicit. If either of the
  // targets is nullptr (fallthrough) use and bind `fallthrough_target` instead.
  LabelType fallthrough_target;
  LabelType* true_target = true_target_in == nullptr ? &fallthrough_target : true_target_in;
  LabelType* false_target = false_target_in == nullptr ? &fallthrough_target : false_target_in;

  LocationSummary* locations = condition->GetLocations();
  Location left = locations->InAt(0);
  Location right = locations->InAt(1);

  Primitive::Type type = condition->InputAt(0)->GetType();
  switch (type) {
    case Primitive::kPrimLong:
      GenerateLongComparesAndJumps(condition, true_target, false_target);
      break;
    case Primitive::kPrimFloat:
      GenerateFPCompare(left, right, condition, false);
      GenerateFPJumps(condition, true_target, false_target);
      break;
    case Primitive::kPrimDouble:
      GenerateFPCompare(left, right, condition, true);
      GenerateFPJumps(condition, true_target, false_target);
      break;
    default:
      LOG(FATAL) << "Unexpected compare type " << type;
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
  // are set only strictly before `branch`. We can't use the eflags on long/FP
  // conditions if they are materialized due to the complex branching.
  return cond->IsCondition() &&
         cond->GetNext() == branch &&
         cond->InputAt(0)->GetType() != Primitive::kPrimLong &&
         !Primitive::IsFloatingPointType(cond->InputAt(0)->GetType());
}

template<class LabelType>
void InstructionCodeGeneratorX86::GenerateTestAndBranch(HInstruction* instruction,
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
        __ j(X86Condition(cond->AsCondition()->GetOppositeCondition()), false_target);
      } else {
        __ j(X86Condition(cond->AsCondition()->GetCondition()), true_target);
      }
    } else {
      // Materialized condition, compare against 0.
      Location lhs = instruction->GetLocations()->InAt(condition_input_index);
      if (lhs.IsRegister()) {
        __ testl(lhs.AsRegister<Register>(), lhs.AsRegister<Register>());
      } else {
        __ cmpl(Address(ESP, lhs.GetStackIndex()), Immediate(0));
      }
      if (true_target == nullptr) {
        __ j(kEqual, false_target);
      } else {
        __ j(kNotEqual, true_target);
      }
    }
  } else {
    // Condition has not been materialized, use its inputs as the comparison and
    // its condition as the branch condition.
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
    // LHS is guaranteed to be in a register (see LocationsBuilderX86::HandleCondition).
    if (rhs.IsRegister()) {
      __ cmpl(lhs.AsRegister<Register>(), rhs.AsRegister<Register>());
    } else if (rhs.IsConstant()) {
      int32_t constant = CodeGenerator::GetInt32ValueOf(rhs.GetConstant());
      codegen_->Compare32BitValue(lhs.AsRegister<Register>(), constant);
    } else {
      __ cmpl(lhs.AsRegister<Register>(), Address(ESP, rhs.GetStackIndex()));
    }
    if (true_target == nullptr) {
      __ j(X86Condition(condition->GetOppositeCondition()), false_target);
    } else {
      __ j(X86Condition(condition->GetCondition()), true_target);
    }
  }

  // If neither branch falls through (case 3), the conditional branch to `true_target`
  // was already emitted (case 2) and we need to emit a jump to `false_target`.
  if (true_target != nullptr && false_target != nullptr) {
    __ jmp(false_target);
  }
}

void LocationsBuilderX86::VisitIf(HIf* if_instr) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(if_instr);
  if (IsBooleanValueOrMaterializedCondition(if_instr->InputAt(0))) {
    locations->SetInAt(0, Location::Any());
  }
}

void InstructionCodeGeneratorX86::VisitIf(HIf* if_instr) {
  HBasicBlock* true_successor = if_instr->IfTrueSuccessor();
  HBasicBlock* false_successor = if_instr->IfFalseSuccessor();
  Label* true_target = codegen_->GoesToNextBlock(if_instr->GetBlock(), true_successor) ?
      nullptr : codegen_->GetLabelOf(true_successor);
  Label* false_target = codegen_->GoesToNextBlock(if_instr->GetBlock(), false_successor) ?
      nullptr : codegen_->GetLabelOf(false_successor);
  GenerateTestAndBranch(if_instr, /* condition_input_index */ 0, true_target, false_target);
}

void LocationsBuilderX86::VisitDeoptimize(HDeoptimize* deoptimize) {
  LocationSummary* locations = new (GetGraph()->GetArena())
      LocationSummary(deoptimize, LocationSummary::kCallOnSlowPath);
  if (IsBooleanValueOrMaterializedCondition(deoptimize->InputAt(0))) {
    locations->SetInAt(0, Location::Any());
  }
}

void InstructionCodeGeneratorX86::VisitDeoptimize(HDeoptimize* deoptimize) {
  SlowPathCode* slow_path = deopt_slow_paths_.NewSlowPath<DeoptimizationSlowPathX86>(deoptimize);
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
  // In 32 bit mode, a long condition doesn't generate a single CC either.
  HInstruction* condition = select->GetCondition();
  if (condition->IsCondition()) {
    Primitive::Type compare_type = condition->InputAt(0)->GetType();
    if (compare_type == Primitive::kPrimLong ||
        Primitive::IsFloatingPointType(compare_type)) {
      return false;
    }
  }

  // We can generate a CMOV for this Select.
  return true;
}

void LocationsBuilderX86::VisitSelect(HSelect* select) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(select);
  if (Primitive::IsFloatingPointType(select->GetType())) {
    locations->SetInAt(0, Location::RequiresFpuRegister());
    locations->SetInAt(1, Location::Any());
  } else {
    locations->SetInAt(0, Location::RequiresRegister());
    if (SelectCanUseCMOV(select)) {
      if (select->InputAt(1)->IsConstant()) {
        // Cmov can't handle a constant value.
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

void InstructionCodeGeneratorX86::GenerateIntCompare(Location lhs, Location rhs) {
  Register lhs_reg = lhs.AsRegister<Register>();
  if (rhs.IsConstant()) {
    int32_t value = CodeGenerator::GetInt32ValueOf(rhs.GetConstant());
    codegen_->Compare32BitValue(lhs_reg, value);
  } else if (rhs.IsStackSlot()) {
    __ cmpl(lhs_reg, Address(ESP, rhs.GetStackIndex()));
  } else {
    __ cmpl(lhs_reg, rhs.AsRegister<Register>());
  }
}

void InstructionCodeGeneratorX86::VisitSelect(HSelect* select) {
  LocationSummary* locations = select->GetLocations();
  DCHECK(locations->InAt(0).Equals(locations->Out()));
  if (SelectCanUseCMOV(select)) {
    // If both the condition and the source types are integer, we can generate
    // a CMOV to implement Select.

    HInstruction* select_condition = select->GetCondition();
    Condition cond = kNotEqual;

    // Figure out how to test the 'condition'.
    if (select_condition->IsCondition()) {
      HCondition* condition = select_condition->AsCondition();
      if (!condition->IsEmittedAtUseSite()) {
        // This was a previously materialized condition.
        // Can we use the existing condition code?
        if (AreEflagsSetFrom(condition, select)) {
          // Materialization was the previous instruction. Condition codes are right.
          cond = X86Condition(condition->GetCondition());
        } else {
          // No, we have to recreate the condition code.
          Register cond_reg = locations->InAt(2).AsRegister<Register>();
          __ testl(cond_reg, cond_reg);
        }
      } else {
        // We can't handle FP or long here.
        DCHECK_NE(condition->InputAt(0)->GetType(), Primitive::kPrimLong);
        DCHECK(!Primitive::IsFloatingPointType(condition->InputAt(0)->GetType()));
        LocationSummary* cond_locations = condition->GetLocations();
        GenerateIntCompare(cond_locations->InAt(0), cond_locations->InAt(1));
        cond = X86Condition(condition->GetCondition());
      }
    } else {
      // Must be a boolean condition, which needs to be compared to 0.
      Register cond_reg = locations->InAt(2).AsRegister<Register>();
      __ testl(cond_reg, cond_reg);
    }

    // If the condition is true, overwrite the output, which already contains false.
    Location false_loc = locations->InAt(0);
    Location true_loc = locations->InAt(1);
    if (select->GetType() == Primitive::kPrimLong) {
      // 64 bit conditional move.
      Register false_high = false_loc.AsRegisterPairHigh<Register>();
      Register false_low = false_loc.AsRegisterPairLow<Register>();
      if (true_loc.IsRegisterPair()) {
        __ cmovl(cond, false_high, true_loc.AsRegisterPairHigh<Register>());
        __ cmovl(cond, false_low, true_loc.AsRegisterPairLow<Register>());
      } else {
        __ cmovl(cond, false_high, Address(ESP, true_loc.GetHighStackIndex(kX86WordSize)));
        __ cmovl(cond, false_low, Address(ESP, true_loc.GetStackIndex()));
      }
    } else {
      // 32 bit conditional move.
      Register false_reg = false_loc.AsRegister<Register>();
      if (true_loc.IsRegister()) {
        __ cmovl(cond, false_reg, true_loc.AsRegister<Register>());
      } else {
        __ cmovl(cond, false_reg, Address(ESP, true_loc.GetStackIndex()));
      }
    }
  } else {
    NearLabel false_target;
    GenerateTestAndBranch<NearLabel>(
        select, /* condition_input_index */ 2, /* true_target */ nullptr, &false_target);
    codegen_->MoveLocation(locations->Out(), locations->InAt(1), select->GetType());
    __ Bind(&false_target);
  }
}

void LocationsBuilderX86::VisitNativeDebugInfo(HNativeDebugInfo* info) {
  new (GetGraph()->GetArena()) LocationSummary(info);
}

void InstructionCodeGeneratorX86::VisitNativeDebugInfo(HNativeDebugInfo*) {
  // MaybeRecordNativeDebugInfo is already called implicitly in CodeGenerator::Compile.
}

void CodeGeneratorX86::GenerateNop() {
  __ nop();
}

void LocationsBuilderX86::HandleCondition(HCondition* cond) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(cond, LocationSummary::kNoCall);
  // Handle the long/FP comparisons made in instruction simplification.
  switch (cond->InputAt(0)->GetType()) {
    case Primitive::kPrimLong: {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::Any());
      if (!cond->IsEmittedAtUseSite()) {
        locations->SetOut(Location::RequiresRegister());
      }
      break;
    }
    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      locations->SetInAt(0, Location::RequiresFpuRegister());
      if (cond->InputAt(1)->IsX86LoadFromConstantTable()) {
        DCHECK(cond->InputAt(1)->IsEmittedAtUseSite());
      } else if (cond->InputAt(1)->IsConstant()) {
        locations->SetInAt(1, Location::RequiresFpuRegister());
      } else {
        locations->SetInAt(1, Location::Any());
      }
      if (!cond->IsEmittedAtUseSite()) {
        locations->SetOut(Location::RequiresRegister());
      }
      break;
    }
    default:
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::Any());
      if (!cond->IsEmittedAtUseSite()) {
        // We need a byte register.
        locations->SetOut(Location::RegisterLocation(ECX));
      }
      break;
  }
}

void InstructionCodeGeneratorX86::HandleCondition(HCondition* cond) {
  if (cond->IsEmittedAtUseSite()) {
    return;
  }

  LocationSummary* locations = cond->GetLocations();
  Location lhs = locations->InAt(0);
  Location rhs = locations->InAt(1);
  Register reg = locations->Out().AsRegister<Register>();
  NearLabel true_label, false_label;

  switch (cond->InputAt(0)->GetType()) {
    default: {
      // Integer case.

      // Clear output register: setb only sets the low byte.
      __ xorl(reg, reg);
      GenerateIntCompare(lhs, rhs);
      __ setb(X86Condition(cond->GetCondition()), reg);
      return;
    }
    case Primitive::kPrimLong:
      GenerateLongComparesAndJumps(cond, &true_label, &false_label);
      break;
    case Primitive::kPrimFloat:
      GenerateFPCompare(lhs, rhs, cond, false);
      GenerateFPJumps(cond, &true_label, &false_label);
      break;
    case Primitive::kPrimDouble:
      GenerateFPCompare(lhs, rhs, cond, true);
      GenerateFPJumps(cond, &true_label, &false_label);
      break;
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

void LocationsBuilderX86::VisitEqual(HEqual* comp) {
  HandleCondition(comp);
}

void InstructionCodeGeneratorX86::VisitEqual(HEqual* comp) {
  HandleCondition(comp);
}

void LocationsBuilderX86::VisitNotEqual(HNotEqual* comp) {
  HandleCondition(comp);
}

void InstructionCodeGeneratorX86::VisitNotEqual(HNotEqual* comp) {
  HandleCondition(comp);
}

void LocationsBuilderX86::VisitLessThan(HLessThan* comp) {
  HandleCondition(comp);
}

void InstructionCodeGeneratorX86::VisitLessThan(HLessThan* comp) {
  HandleCondition(comp);
}

void LocationsBuilderX86::VisitLessThanOrEqual(HLessThanOrEqual* comp) {
  HandleCondition(comp);
}

void InstructionCodeGeneratorX86::VisitLessThanOrEqual(HLessThanOrEqual* comp) {
  HandleCondition(comp);
}

void LocationsBuilderX86::VisitGreaterThan(HGreaterThan* comp) {
  HandleCondition(comp);
}

void InstructionCodeGeneratorX86::VisitGreaterThan(HGreaterThan* comp) {
  HandleCondition(comp);
}

void LocationsBuilderX86::VisitGreaterThanOrEqual(HGreaterThanOrEqual* comp) {
  HandleCondition(comp);
}

void InstructionCodeGeneratorX86::VisitGreaterThanOrEqual(HGreaterThanOrEqual* comp) {
  HandleCondition(comp);
}

void LocationsBuilderX86::VisitBelow(HBelow* comp) {
  HandleCondition(comp);
}

void InstructionCodeGeneratorX86::VisitBelow(HBelow* comp) {
  HandleCondition(comp);
}

void LocationsBuilderX86::VisitBelowOrEqual(HBelowOrEqual* comp) {
  HandleCondition(comp);
}

void InstructionCodeGeneratorX86::VisitBelowOrEqual(HBelowOrEqual* comp) {
  HandleCondition(comp);
}

void LocationsBuilderX86::VisitAbove(HAbove* comp) {
  HandleCondition(comp);
}

void InstructionCodeGeneratorX86::VisitAbove(HAbove* comp) {
  HandleCondition(comp);
}

void LocationsBuilderX86::VisitAboveOrEqual(HAboveOrEqual* comp) {
  HandleCondition(comp);
}

void InstructionCodeGeneratorX86::VisitAboveOrEqual(HAboveOrEqual* comp) {
  HandleCondition(comp);
}

void LocationsBuilderX86::VisitIntConstant(HIntConstant* constant) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(constant, LocationSummary::kNoCall);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorX86::VisitIntConstant(HIntConstant* constant ATTRIBUTE_UNUSED) {
  // Will be generated at use site.
}

void LocationsBuilderX86::VisitNullConstant(HNullConstant* constant) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(constant, LocationSummary::kNoCall);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorX86::VisitNullConstant(HNullConstant* constant ATTRIBUTE_UNUSED) {
  // Will be generated at use site.
}

void LocationsBuilderX86::VisitLongConstant(HLongConstant* constant) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(constant, LocationSummary::kNoCall);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorX86::VisitLongConstant(HLongConstant* constant ATTRIBUTE_UNUSED) {
  // Will be generated at use site.
}

void LocationsBuilderX86::VisitFloatConstant(HFloatConstant* constant) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(constant, LocationSummary::kNoCall);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorX86::VisitFloatConstant(HFloatConstant* constant ATTRIBUTE_UNUSED) {
  // Will be generated at use site.
}

void LocationsBuilderX86::VisitDoubleConstant(HDoubleConstant* constant) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(constant, LocationSummary::kNoCall);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorX86::VisitDoubleConstant(HDoubleConstant* constant ATTRIBUTE_UNUSED) {
  // Will be generated at use site.
}

void LocationsBuilderX86::VisitMemoryBarrier(HMemoryBarrier* memory_barrier) {
  memory_barrier->SetLocations(nullptr);
}

void InstructionCodeGeneratorX86::VisitMemoryBarrier(HMemoryBarrier* memory_barrier) {
  codegen_->GenerateMemoryBarrier(memory_barrier->GetBarrierKind());
}

void LocationsBuilderX86::VisitReturnVoid(HReturnVoid* ret) {
  ret->SetLocations(nullptr);
}

void InstructionCodeGeneratorX86::VisitReturnVoid(HReturnVoid* ret ATTRIBUTE_UNUSED) {
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

void LocationsBuilderX86::VisitInvokeUnresolved(HInvokeUnresolved* invoke) {
  // The trampoline uses the same calling convention as dex calling conventions,
  // except instead of loading arg0/r0 with the target Method*, arg0/r0 will contain
  // the method_idx.
  HandleInvoke(invoke);
}

void InstructionCodeGeneratorX86::VisitInvokeUnresolved(HInvokeUnresolved* invoke) {
  codegen_->GenerateInvokeUnresolvedRuntimeCall(invoke);
}

void LocationsBuilderX86::VisitInvokeStaticOrDirect(HInvokeStaticOrDirect* invoke) {
  // Explicit clinit checks triggered by static invokes must have been pruned by
  // art::PrepareForRegisterAllocation.
  DCHECK(!invoke->IsStaticWithExplicitClinitCheck());

  IntrinsicLocationsBuilderX86 intrinsic(codegen_);
  if (intrinsic.TryDispatch(invoke)) {
    if (invoke->GetLocations()->CanCall() && invoke->HasPcRelativeDexCache()) {
      invoke->GetLocations()->SetInAt(invoke->GetSpecialInputIndex(), Location::Any());
    }
    return;
  }

  HandleInvoke(invoke);

  // For PC-relative dex cache the invoke has an extra input, the PC-relative address base.
  if (invoke->HasPcRelativeDexCache()) {
    invoke->GetLocations()->SetInAt(invoke->GetSpecialInputIndex(), Location::RequiresRegister());
  }
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

void LocationsBuilderX86::VisitInvokeVirtual(HInvokeVirtual* invoke) {
  IntrinsicLocationsBuilderX86 intrinsic(codegen_);
  if (intrinsic.TryDispatch(invoke)) {
    return;
  }

  HandleInvoke(invoke);
}

void LocationsBuilderX86::HandleInvoke(HInvoke* invoke) {
  InvokeDexCallingConventionVisitorX86 calling_convention_visitor;
  CodeGenerator::CreateCommonInvokeLocationSummary(invoke, &calling_convention_visitor);
}

void InstructionCodeGeneratorX86::VisitInvokeVirtual(HInvokeVirtual* invoke) {
  if (TryGenerateIntrinsicCode(invoke, codegen_)) {
    return;
  }

  codegen_->GenerateVirtualCall(invoke, invoke->GetLocations()->GetTemp(0));
  DCHECK(!codegen_->IsLeafMethod());
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
}

void LocationsBuilderX86::VisitInvokeInterface(HInvokeInterface* invoke) {
  // This call to HandleInvoke allocates a temporary (core) register
  // which is also used to transfer the hidden argument from FP to
  // core register.
  HandleInvoke(invoke);
  // Add the hidden argument.
  invoke->GetLocations()->AddTemp(Location::FpuRegisterLocation(XMM7));
}

void InstructionCodeGeneratorX86::VisitInvokeInterface(HInvokeInterface* invoke) {
  // TODO: b/18116999, our IMTs can miss an IncompatibleClassChangeError.
  LocationSummary* locations = invoke->GetLocations();
  Register temp = locations->GetTemp(0).AsRegister<Register>();
  XmmRegister hidden_reg = locations->GetTemp(1).AsFpuRegister<XmmRegister>();
  uint32_t method_offset = mirror::Class::EmbeddedImTableEntryOffset(
      invoke->GetImtIndex() % mirror::Class::kImtSize, kX86PointerSize).Uint32Value();
  Location receiver = locations->InAt(0);
  uint32_t class_offset = mirror::Object::ClassOffset().Int32Value();

  // Set the hidden argument. This is safe to do this here, as XMM7
  // won't be modified thereafter, before the `call` instruction.
  DCHECK_EQ(XMM7, hidden_reg);
  __ movl(temp, Immediate(invoke->GetDexMethodIndex()));
  __ movd(hidden_reg, temp);

  if (receiver.IsStackSlot()) {
    __ movl(temp, Address(ESP, receiver.GetStackIndex()));
    // /* HeapReference<Class> */ temp = temp->klass_
    __ movl(temp, Address(temp, class_offset));
  } else {
    // /* HeapReference<Class> */ temp = receiver->klass_
    __ movl(temp, Address(receiver.AsRegister<Register>(), class_offset));
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
  __ movl(temp, Address(temp, method_offset));
  // call temp->GetEntryPoint();
  __ call(Address(temp,
                  ArtMethod::EntryPointFromQuickCompiledCodeOffset(kX86WordSize).Int32Value()));

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

void LocationsBuilderX86::VisitX86FPNeg(HX86FPNeg* neg) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(neg, LocationSummary::kNoCall);
  DCHECK(Primitive::IsFloatingPointType(neg->GetType()));
  locations->SetInAt(0, Location::RequiresFpuRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  locations->SetOut(Location::SameAsFirstInput());
  locations->AddTemp(Location::RequiresFpuRegister());
}

void InstructionCodeGeneratorX86::VisitX86FPNeg(HX86FPNeg* neg) {
  LocationSummary* locations = neg->GetLocations();
  Location out = locations->Out();
  DCHECK(locations->InAt(0).Equals(out));

  Register constant_area = locations->InAt(1).AsRegister<Register>();
  XmmRegister mask = locations->GetTemp(0).AsFpuRegister<XmmRegister>();
  if (neg->GetType() == Primitive::kPrimFloat) {
    __ movss(mask, codegen_->LiteralInt32Address(INT32_C(0x80000000), constant_area));
    __ xorps(out.AsFpuRegister<XmmRegister>(), mask);
  } else {
     __ movsd(mask, codegen_->LiteralInt64Address(INT64_C(0x8000000000000000), constant_area));
     __ xorpd(out.AsFpuRegister<XmmRegister>(), mask);
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
        case Primitive::kPrimLong: {
          // Type conversion from long to byte is a result of code transformations.
          HInstruction* input = conversion->InputAt(0);
          Location input_location = input->IsConstant()
              ? Location::ConstantLocation(input->AsConstant())
              : Location::RegisterPairLocation(EAX, EDX);
          locations->SetInAt(0, input_location);
          // Make the output overlap to please the register allocator. This greatly simplifies
          // the validation of the linear scan implementation
          locations->SetOut(Location::RequiresRegister(), Location::kOutputOverlap);
          break;
        }
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
        case Primitive::kPrimLong:
          // Type conversion from long to byte is a result of code transformations.
          if (in.IsRegisterPair()) {
            __ movsxb(out.AsRegister<Register>(), in.AsRegisterPairLow<ByteRegister>());
          } else {
            DCHECK(in.GetConstant()->IsLongConstant());
            int64_t value = in.GetConstant()->AsLongConstant()->GetValue();
            __ movl(out.AsRegister<Register>(), Immediate(static_cast<int8_t>(value)));
          }
          break;
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
        case Primitive::kPrimLong:
          // Type conversion from long to short is a result of code transformations.
          if (in.IsRegisterPair()) {
            __ movsxw(out.AsRegister<Register>(), in.AsRegisterPairLow<Register>());
          } else if (in.IsDoubleStackSlot()) {
            __ movsxw(out.AsRegister<Register>(), Address(ESP, in.GetStackIndex()));
          } else {
            DCHECK(in.GetConstant()->IsLongConstant());
            int64_t value = in.GetConstant()->AsLongConstant()->GetValue();
            __ movl(out.AsRegister<Register>(), Immediate(static_cast<int16_t>(value)));
          }
          break;
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
          NearLabel done, nan;

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
          NearLabel done, nan;

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
          codegen_->InvokeRuntime(QUICK_ENTRY_POINT(pF2l),
                                  conversion,
                                  conversion->GetDexPc(),
                                  nullptr);
          CheckEntrypointTypes<kQuickF2l, int64_t, float>();
          break;

        case Primitive::kPrimDouble:
          // Processing a Dex `double-to-long' instruction.
          codegen_->InvokeRuntime(QUICK_ENTRY_POINT(pD2l),
                                  conversion,
                                  conversion->GetDexPc(),
                                  nullptr);
          CheckEntrypointTypes<kQuickD2l, int64_t, double>();
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      }
      break;

    case Primitive::kPrimChar:
      switch (input_type) {
        case Primitive::kPrimLong:
          // Type conversion from long to short is a result of code transformations.
          if (in.IsRegisterPair()) {
            __ movzxw(out.AsRegister<Register>(), in.AsRegisterPairLow<Register>());
          } else if (in.IsDoubleStackSlot()) {
            __ movzxw(out.AsRegister<Register>(), Address(ESP, in.GetStackIndex()));
          } else {
            DCHECK(in.GetConstant()->IsLongConstant());
            int64_t value = in.GetConstant()->AsLongConstant()->GetValue();
            __ movl(out.AsRegister<Register>(), Immediate(static_cast<uint16_t>(value)));
          }
          break;
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
      if (add->InputAt(1)->IsX86LoadFromConstantTable()) {
        DCHECK(add->InputAt(1)->IsEmittedAtUseSite());
      } else if (add->InputAt(1)->IsConstant()) {
        locations->SetInAt(1, Location::RequiresFpuRegister());
      } else {
        locations->SetInAt(1, Location::Any());
      }
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
        } else if (out.AsRegister<Register>() == second.AsRegister<Register>()) {
          __ addl(out.AsRegister<Register>(), first.AsRegister<Register>());
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
      } else if (add->InputAt(1)->IsX86LoadFromConstantTable()) {
        HX86LoadFromConstantTable* const_area = add->InputAt(1)->AsX86LoadFromConstantTable();
        DCHECK(const_area->IsEmittedAtUseSite());
        __ addss(first.AsFpuRegister<XmmRegister>(),
                 codegen_->LiteralFloatAddress(
                   const_area->GetConstant()->AsFloatConstant()->GetValue(),
                   const_area->GetLocations()->InAt(0).AsRegister<Register>()));
      } else {
        DCHECK(second.IsStackSlot());
        __ addss(first.AsFpuRegister<XmmRegister>(), Address(ESP, second.GetStackIndex()));
      }
      break;
    }

    case Primitive::kPrimDouble: {
      if (second.IsFpuRegister()) {
        __ addsd(first.AsFpuRegister<XmmRegister>(), second.AsFpuRegister<XmmRegister>());
      } else if (add->InputAt(1)->IsX86LoadFromConstantTable()) {
        HX86LoadFromConstantTable* const_area = add->InputAt(1)->AsX86LoadFromConstantTable();
        DCHECK(const_area->IsEmittedAtUseSite());
        __ addsd(first.AsFpuRegister<XmmRegister>(),
                 codegen_->LiteralDoubleAddress(
                   const_area->GetConstant()->AsDoubleConstant()->GetValue(),
                   const_area->GetLocations()->InAt(0).AsRegister<Register>()));
      } else {
        DCHECK(second.IsDoubleStackSlot());
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
      if (sub->InputAt(1)->IsX86LoadFromConstantTable()) {
        DCHECK(sub->InputAt(1)->IsEmittedAtUseSite());
      } else if (sub->InputAt(1)->IsConstant()) {
        locations->SetInAt(1, Location::RequiresFpuRegister());
      } else {
        locations->SetInAt(1, Location::Any());
      }
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
      if (second.IsFpuRegister()) {
        __ subss(first.AsFpuRegister<XmmRegister>(), second.AsFpuRegister<XmmRegister>());
      } else if (sub->InputAt(1)->IsX86LoadFromConstantTable()) {
        HX86LoadFromConstantTable* const_area = sub->InputAt(1)->AsX86LoadFromConstantTable();
        DCHECK(const_area->IsEmittedAtUseSite());
        __ subss(first.AsFpuRegister<XmmRegister>(),
                 codegen_->LiteralFloatAddress(
                   const_area->GetConstant()->AsFloatConstant()->GetValue(),
                   const_area->GetLocations()->InAt(0).AsRegister<Register>()));
      } else {
        DCHECK(second.IsStackSlot());
        __ subss(first.AsFpuRegister<XmmRegister>(), Address(ESP, second.GetStackIndex()));
      }
      break;
    }

    case Primitive::kPrimDouble: {
      if (second.IsFpuRegister()) {
        __ subsd(first.AsFpuRegister<XmmRegister>(), second.AsFpuRegister<XmmRegister>());
      } else if (sub->InputAt(1)->IsX86LoadFromConstantTable()) {
        HX86LoadFromConstantTable* const_area = sub->InputAt(1)->AsX86LoadFromConstantTable();
        DCHECK(const_area->IsEmittedAtUseSite());
        __ subsd(first.AsFpuRegister<XmmRegister>(),
                 codegen_->LiteralDoubleAddress(
                     const_area->GetConstant()->AsDoubleConstant()->GetValue(),
                     const_area->GetLocations()->InAt(0).AsRegister<Register>()));
      } else {
        DCHECK(second.IsDoubleStackSlot());
        __ subsd(first.AsFpuRegister<XmmRegister>(), Address(ESP, second.GetStackIndex()));
      }
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
      if (mul->InputAt(1)->IsIntConstant()) {
        // Can use 3 operand multiply.
        locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      } else {
        locations->SetOut(Location::SameAsFirstInput());
      }
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
      if (mul->InputAt(1)->IsX86LoadFromConstantTable()) {
        DCHECK(mul->InputAt(1)->IsEmittedAtUseSite());
      } else if (mul->InputAt(1)->IsConstant()) {
        locations->SetInAt(1, Location::RequiresFpuRegister());
      } else {
        locations->SetInAt(1, Location::Any());
      }
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
  Location out = locations->Out();

  switch (mul->GetResultType()) {
    case Primitive::kPrimInt:
      // The constant may have ended up in a register, so test explicitly to avoid
      // problems where the output may not be the same as the first operand.
      if (mul->InputAt(1)->IsIntConstant()) {
        Immediate imm(mul->InputAt(1)->AsIntConstant()->GetValue());
        __ imull(out.AsRegister<Register>(), first.AsRegister<Register>(), imm);
      } else if (second.IsRegister()) {
        DCHECK(first.Equals(out));
        __ imull(first.AsRegister<Register>(), second.AsRegister<Register>());
      } else {
        DCHECK(second.IsStackSlot());
        DCHECK(first.Equals(out));
        __ imull(first.AsRegister<Register>(), Address(ESP, second.GetStackIndex()));
      }
      break;

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
      DCHECK(first.Equals(locations->Out()));
      if (second.IsFpuRegister()) {
        __ mulss(first.AsFpuRegister<XmmRegister>(), second.AsFpuRegister<XmmRegister>());
      } else if (mul->InputAt(1)->IsX86LoadFromConstantTable()) {
        HX86LoadFromConstantTable* const_area = mul->InputAt(1)->AsX86LoadFromConstantTable();
        DCHECK(const_area->IsEmittedAtUseSite());
        __ mulss(first.AsFpuRegister<XmmRegister>(),
                 codegen_->LiteralFloatAddress(
                     const_area->GetConstant()->AsFloatConstant()->GetValue(),
                     const_area->GetLocations()->InAt(0).AsRegister<Register>()));
      } else {
        DCHECK(second.IsStackSlot());
        __ mulss(first.AsFpuRegister<XmmRegister>(), Address(ESP, second.GetStackIndex()));
      }
      break;
    }

    case Primitive::kPrimDouble: {
      DCHECK(first.Equals(locations->Out()));
      if (second.IsFpuRegister()) {
        __ mulsd(first.AsFpuRegister<XmmRegister>(), second.AsFpuRegister<XmmRegister>());
      } else if (mul->InputAt(1)->IsX86LoadFromConstantTable()) {
        HX86LoadFromConstantTable* const_area = mul->InputAt(1)->AsX86LoadFromConstantTable();
        DCHECK(const_area->IsEmittedAtUseSite());
        __ mulsd(first.AsFpuRegister<XmmRegister>(),
                 codegen_->LiteralDoubleAddress(
                     const_area->GetConstant()->AsDoubleConstant()->GetValue(),
                     const_area->GetLocations()->InAt(0).AsRegister<Register>()));
      } else {
        DCHECK(second.IsDoubleStackSlot());
        __ mulsd(first.AsFpuRegister<XmmRegister>(), Address(ESP, second.GetStackIndex()));
      }
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
  NearLabel retry;
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
  DCHECK(IsPowerOfTwo(AbsOrMin(imm)));
  uint32_t abs_imm = static_cast<uint32_t>(AbsOrMin(imm));

  Register num = locations->GetTemp(0).AsRegister<Register>();

  __ leal(num, Address(input_register, abs_imm - 1));
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

  NearLabel ndiv;
  NearLabel end;
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

      if (second.IsConstant()) {
        int32_t imm = second.GetConstant()->AsIntConstant()->GetValue();

        if (imm == 0) {
          // Do not generate anything for 0. DivZeroCheck would forbid any generated code.
        } else if (imm == 1 || imm == -1) {
          DivRemOneOrMinusOne(instruction);
        } else if (is_div && IsPowerOfTwo(AbsOrMin(imm))) {
          DivByPowerOfTwo(instruction->AsDiv());
        } else {
          DCHECK(imm <= -2 || imm >= 2);
          GenerateDivRemWithAnyConstant(instruction);
        }
      } else {
        SlowPathCode* slow_path = new (GetGraph()->GetArena()) DivRemMinusOneSlowPathX86(
            instruction, out.AsRegister<Register>(), is_div);
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
        codegen_->InvokeRuntime(QUICK_ENTRY_POINT(pLdiv),
                                instruction,
                                instruction->GetDexPc(),
                                nullptr);
        CheckEntrypointTypes<kQuickLdiv, int64_t, int64_t, int64_t>();
      } else {
        codegen_->InvokeRuntime(QUICK_ENTRY_POINT(pLmod),
                                instruction,
                                instruction->GetDexPc(),
                                nullptr);
        CheckEntrypointTypes<kQuickLmod, int64_t, int64_t, int64_t>();
      }
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
      if (div->InputAt(1)->IsX86LoadFromConstantTable()) {
        DCHECK(div->InputAt(1)->IsEmittedAtUseSite());
      } else if (div->InputAt(1)->IsConstant()) {
        locations->SetInAt(1, Location::RequiresFpuRegister());
      } else {
        locations->SetInAt(1, Location::Any());
      }
      locations->SetOut(Location::SameAsFirstInput());
      break;
    }

    default:
      LOG(FATAL) << "Unexpected div type " << div->GetResultType();
  }
}

void InstructionCodeGeneratorX86::VisitDiv(HDiv* div) {
  LocationSummary* locations = div->GetLocations();
  Location first = locations->InAt(0);
  Location second = locations->InAt(1);

  switch (div->GetResultType()) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong: {
      GenerateDivRemIntegral(div);
      break;
    }

    case Primitive::kPrimFloat: {
      if (second.IsFpuRegister()) {
        __ divss(first.AsFpuRegister<XmmRegister>(), second.AsFpuRegister<XmmRegister>());
      } else if (div->InputAt(1)->IsX86LoadFromConstantTable()) {
        HX86LoadFromConstantTable* const_area = div->InputAt(1)->AsX86LoadFromConstantTable();
        DCHECK(const_area->IsEmittedAtUseSite());
        __ divss(first.AsFpuRegister<XmmRegister>(),
                 codegen_->LiteralFloatAddress(
                   const_area->GetConstant()->AsFloatConstant()->GetValue(),
                   const_area->GetLocations()->InAt(0).AsRegister<Register>()));
      } else {
        DCHECK(second.IsStackSlot());
        __ divss(first.AsFpuRegister<XmmRegister>(), Address(ESP, second.GetStackIndex()));
      }
      break;
    }

    case Primitive::kPrimDouble: {
      if (second.IsFpuRegister()) {
        __ divsd(first.AsFpuRegister<XmmRegister>(), second.AsFpuRegister<XmmRegister>());
      } else if (div->InputAt(1)->IsX86LoadFromConstantTable()) {
        HX86LoadFromConstantTable* const_area = div->InputAt(1)->AsX86LoadFromConstantTable();
        DCHECK(const_area->IsEmittedAtUseSite());
        __ divsd(first.AsFpuRegister<XmmRegister>(),
                 codegen_->LiteralDoubleAddress(
                   const_area->GetConstant()->AsDoubleConstant()->GetValue(),
                   const_area->GetLocations()->InAt(0).AsRegister<Register>()));
      } else {
        DCHECK(second.IsDoubleStackSlot());
        __ divsd(first.AsFpuRegister<XmmRegister>(), Address(ESP, second.GetStackIndex()));
      }
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
  LocationSummary::CallKind call_kind = instruction->CanThrowIntoCatchBlock()
      ? LocationSummary::kCallOnSlowPath
      : LocationSummary::kNoCall;
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction, call_kind);
  switch (instruction->GetType()) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
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
  SlowPathCode* slow_path = new (GetGraph()->GetArena()) DivZeroCheckSlowPathX86(instruction);
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
        int32_t shift = second.GetConstant()->AsIntConstant()->GetValue() & kMaxIntShiftDistance;
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
        int32_t shift = second.GetConstant()->AsIntConstant()->GetValue() & kMaxLongShiftDistance;
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
  NearLabel done;
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
  NearLabel done;
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
  NearLabel done;
  __ shrd(loc.AsRegisterPairLow<Register>(), loc.AsRegisterPairHigh<Register>(), shifter);
  __ shrl(loc.AsRegisterPairHigh<Register>(), shifter);
  __ testl(shifter, Immediate(32));
  __ j(kEqual, &done);
  __ movl(loc.AsRegisterPairLow<Register>(), loc.AsRegisterPairHigh<Register>());
  __ movl(loc.AsRegisterPairHigh<Register>(), Immediate(0));
  __ Bind(&done);
}

void LocationsBuilderX86::VisitRor(HRor* ror) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(ror, LocationSummary::kNoCall);

  switch (ror->GetResultType()) {
    case Primitive::kPrimLong:
      // Add the temporary needed.
      locations->AddTemp(Location::RequiresRegister());
      FALLTHROUGH_INTENDED;
    case Primitive::kPrimInt:
      locations->SetInAt(0, Location::RequiresRegister());
      // The shift count needs to be in CL (unless it is a constant).
      locations->SetInAt(1, Location::ByteRegisterOrConstant(ECX, ror->InputAt(1)));
      locations->SetOut(Location::SameAsFirstInput());
      break;
    default:
      LOG(FATAL) << "Unexpected operation type " << ror->GetResultType();
      UNREACHABLE();
  }
}

void InstructionCodeGeneratorX86::VisitRor(HRor* ror) {
  LocationSummary* locations = ror->GetLocations();
  Location first = locations->InAt(0);
  Location second = locations->InAt(1);

  if (ror->GetResultType() == Primitive::kPrimInt) {
    Register first_reg = first.AsRegister<Register>();
    if (second.IsRegister()) {
      Register second_reg = second.AsRegister<Register>();
      __ rorl(first_reg, second_reg);
    } else {
      Immediate imm(second.GetConstant()->AsIntConstant()->GetValue() & kMaxIntShiftDistance);
      __ rorl(first_reg, imm);
    }
    return;
  }

  DCHECK_EQ(ror->GetResultType(), Primitive::kPrimLong);
  Register first_reg_lo = first.AsRegisterPairLow<Register>();
  Register first_reg_hi = first.AsRegisterPairHigh<Register>();
  Register temp_reg = locations->GetTemp(0).AsRegister<Register>();
  if (second.IsRegister()) {
    Register second_reg = second.AsRegister<Register>();
    DCHECK_EQ(second_reg, ECX);
    __ movl(temp_reg, first_reg_hi);
    __ shrd(first_reg_hi, first_reg_lo, second_reg);
    __ shrd(first_reg_lo, temp_reg, second_reg);
    __ movl(temp_reg, first_reg_hi);
    __ testl(second_reg, Immediate(32));
    __ cmovl(kNotEqual, first_reg_hi, first_reg_lo);
    __ cmovl(kNotEqual, first_reg_lo, temp_reg);
  } else {
    int32_t shift_amt = second.GetConstant()->AsIntConstant()->GetValue() & kMaxLongShiftDistance;
    if (shift_amt == 0) {
      // Already fine.
      return;
    }
    if (shift_amt == 32) {
      // Just swap.
      __ movl(temp_reg, first_reg_lo);
      __ movl(first_reg_lo, first_reg_hi);
      __ movl(first_reg_hi, temp_reg);
      return;
    }

    Immediate imm(shift_amt);
    // Save the constents of the low value.
    __ movl(temp_reg, first_reg_lo);

    // Shift right into low, feeding bits from high.
    __ shrd(first_reg_lo, first_reg_hi, imm);

    // Shift right into high, feeding bits from the original low.
    __ shrd(first_reg_hi, temp_reg, imm);

    // Swap if needed.
    if (shift_amt > 32) {
      __ movl(temp_reg, first_reg_lo);
      __ movl(first_reg_lo, first_reg_hi);
      __ movl(first_reg_hi, temp_reg);
    }
  }
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
  if (instruction->IsStringAlloc()) {
    locations->AddTemp(Location::RegisterLocation(kMethodRegisterArgument));
  } else {
    InvokeRuntimeCallingConvention calling_convention;
    locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
    locations->SetInAt(1, Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
  }
}

void InstructionCodeGeneratorX86::VisitNewInstance(HNewInstance* instruction) {
  // Note: if heap poisoning is enabled, the entry point takes cares
  // of poisoning the reference.
  if (instruction->IsStringAlloc()) {
    // String is allocated through StringFactory. Call NewEmptyString entry point.
    Register temp = instruction->GetLocations()->GetTemp(0).AsRegister<Register>();
    MemberOffset code_offset = ArtMethod::EntryPointFromQuickCompiledCodeOffset(kX86WordSize);
    __ fs()->movl(temp, Address::Absolute(QUICK_ENTRY_POINT(pNewEmptyString)));
    __ call(Address(temp, code_offset.Int32Value()));
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

void LocationsBuilderX86::VisitNewArray(HNewArray* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCall);
  locations->SetOut(Location::RegisterLocation(EAX));
  InvokeRuntimeCallingConvention calling_convention;
  locations->AddTemp(Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
  locations->SetInAt(1, Location::RegisterLocation(calling_convention.GetRegisterAt(2)));
}

void InstructionCodeGeneratorX86::VisitNewArray(HNewArray* instruction) {
  InvokeRuntimeCallingConvention calling_convention;
  __ movl(calling_convention.GetRegisterAt(0), Immediate(instruction->GetTypeIndex()));
  // Note: if heap poisoning is enabled, the entry point takes cares
  // of poisoning the reference.
  codegen_->InvokeRuntime(instruction->GetEntrypoint(),
                          instruction,
                          instruction->GetDexPc(),
                          nullptr);
  CheckEntrypointTypes<kQuickAllocArrayWithAccessCheck, void*, uint32_t, int32_t, ArtMethod*>();
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

void InstructionCodeGeneratorX86::VisitParameterValue(
    HParameterValue* instruction ATTRIBUTE_UNUSED) {
}

void LocationsBuilderX86::VisitCurrentMethod(HCurrentMethod* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetOut(Location::RegisterLocation(kMethodRegisterArgument));
}

void InstructionCodeGeneratorX86::VisitCurrentMethod(HCurrentMethod* instruction ATTRIBUTE_UNUSED) {
}

void LocationsBuilderX86::VisitClassTableGet(HClassTableGet* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorX86::VisitClassTableGet(HClassTableGet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  uint32_t method_offset = 0;
  if (instruction->GetTableKind() == HClassTableGet::TableKind::kVTable) {
    method_offset = mirror::Class::EmbeddedVTableEntryOffset(
        instruction->GetIndex(), kX86PointerSize).SizeValue();
  } else {
    method_offset = mirror::Class::EmbeddedImTableEntryOffset(
        instruction->GetIndex() % mirror::Class::kImtSize, kX86PointerSize).Uint32Value();
  }
  __ movl(locations->Out().AsRegister<Register>(),
          Address(locations->InAt(0).AsRegister<Register>(), method_offset));
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
      if (compare->InputAt(1)->IsX86LoadFromConstantTable()) {
        DCHECK(compare->InputAt(1)->IsEmittedAtUseSite());
      } else if (compare->InputAt(1)->IsConstant()) {
        locations->SetInAt(1, Location::RequiresFpuRegister());
      } else {
        locations->SetInAt(1, Location::Any());
      }
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

  NearLabel less, greater, done;
  Condition less_cond = kLess;

  switch (compare->InputAt(0)->GetType()) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimShort:
    case Primitive::kPrimChar:
    case Primitive::kPrimInt: {
      GenerateIntCompare(left, right);
      break;
    }
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
        codegen_->Compare32BitValue(left_high, val_high);
      }
      __ j(kLess, &less);  // Signed compare.
      __ j(kGreater, &greater);  // Signed compare.
      if (right.IsRegisterPair()) {
        __ cmpl(left_low, right.AsRegisterPairLow<Register>());
      } else if (right.IsDoubleStackSlot()) {
        __ cmpl(left_low, Address(ESP, right.GetStackIndex()));
      } else {
        DCHECK(right_is_const) << right;
        codegen_->Compare32BitValue(left_low, val_low);
      }
      less_cond = kBelow;  // for CF (unsigned).
      break;
    }
    case Primitive::kPrimFloat: {
      GenerateFPCompare(left, right, compare, false);
      __ j(kUnordered, compare->IsGtBias() ? &greater : &less);
      less_cond = kBelow;  // for CF (floats).
      break;
    }
    case Primitive::kPrimDouble: {
      GenerateFPCompare(left, right, compare, true);
      __ j(kUnordered, compare->IsGtBias() ? &greater : &less);
      less_cond = kBelow;  // for CF (floats).
      break;
    }
    default:
      LOG(FATAL) << "Unexpected type for compare operation " << compare->InputAt(0)->GetType();
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

void LocationsBuilderX86::VisitPhi(HPhi* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  for (size_t i = 0, e = instruction->InputCount(); i < e; ++i) {
    locations->SetInAt(i, Location::Any());
  }
  locations->SetOut(Location::Any());
}

void InstructionCodeGeneratorX86::VisitPhi(HPhi* instruction ATTRIBUTE_UNUSED) {
  LOG(FATAL) << "Unreachable";
}

void CodeGeneratorX86::GenerateMemoryBarrier(MemBarrierKind kind) {
  /*
   * According to the JSR-133 Cookbook, for x86 only StoreLoad/AnyAny barriers need memory fence.
   * All other barriers (LoadAny, AnyStore, StoreStore) are nops due to the x86 memory model.
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
      LOG(FATAL) << "Unexpected memory barrier " << kind;
  }
}

HInvokeStaticOrDirect::DispatchInfo CodeGeneratorX86::GetSupportedInvokeStaticOrDirectDispatch(
      const HInvokeStaticOrDirect::DispatchInfo& desired_dispatch_info,
      MethodReference target_method ATTRIBUTE_UNUSED) {
  HInvokeStaticOrDirect::DispatchInfo dispatch_info = desired_dispatch_info;

  // We disable pc-relative load when there is an irreducible loop, as the optimization
  // is incompatible with it.
  // TODO: Create as many X86ComputeBaseMethodAddress instructions
  // as needed for methods with irreducible loops.
  if (GetGraph()->HasIrreducibleLoops() &&
      (dispatch_info.method_load_kind ==
          HInvokeStaticOrDirect::MethodLoadKind::kDexCachePcRelative)) {
    dispatch_info.method_load_kind = HInvokeStaticOrDirect::MethodLoadKind::kDexCacheViaMethod;
  }
  switch (dispatch_info.code_ptr_location) {
    case HInvokeStaticOrDirect::CodePtrLocation::kCallDirectWithFixup:
    case HInvokeStaticOrDirect::CodePtrLocation::kCallDirect:
      // For direct code, we actually prefer to call via the code pointer from ArtMethod*.
      // (Though the direct CALL ptr16:32 is available for consideration).
      return HInvokeStaticOrDirect::DispatchInfo {
        dispatch_info.method_load_kind,
        HInvokeStaticOrDirect::CodePtrLocation::kCallArtMethod,
        dispatch_info.method_load_data,
        0u
      };
    default:
      return dispatch_info;
  }
}

Register CodeGeneratorX86::GetInvokeStaticOrDirectExtraParameter(HInvokeStaticOrDirect* invoke,
                                                                 Register temp) {
  DCHECK_EQ(invoke->InputCount(), invoke->GetNumberOfArguments() + 1u);
  Location location = invoke->GetLocations()->InAt(invoke->GetSpecialInputIndex());
  if (!invoke->GetLocations()->Intrinsified()) {
    return location.AsRegister<Register>();
  }
  // For intrinsics we allow any location, so it may be on the stack.
  if (!location.IsRegister()) {
    __ movl(temp, Address(ESP, location.GetStackIndex()));
    return temp;
  }
  // For register locations, check if the register was saved. If so, get it from the stack.
  // Note: There is a chance that the register was saved but not overwritten, so we could
  // save one load. However, since this is just an intrinsic slow path we prefer this
  // simple and more robust approach rather that trying to determine if that's the case.
  SlowPathCode* slow_path = GetCurrentSlowPath();
  DCHECK(slow_path != nullptr);  // For intrinsified invokes the call is emitted on the slow path.
  if (slow_path->IsCoreRegisterSaved(location.AsRegister<Register>())) {
    int stack_offset = slow_path->GetStackOffsetOfCoreRegister(location.AsRegister<Register>());
    __ movl(temp, Address(ESP, stack_offset));
    return temp;
  }
  return location.AsRegister<Register>();
}

void CodeGeneratorX86::GenerateStaticOrDirectCall(HInvokeStaticOrDirect* invoke, Location temp) {
  Location callee_method = temp;  // For all kinds except kRecursive, callee will be in temp.
  switch (invoke->GetMethodLoadKind()) {
    case HInvokeStaticOrDirect::MethodLoadKind::kStringInit:
      // temp = thread->string_init_entrypoint
      __ fs()->movl(temp.AsRegister<Register>(), Address::Absolute(invoke->GetStringInitOffset()));
      break;
    case HInvokeStaticOrDirect::MethodLoadKind::kRecursive:
      callee_method = invoke->GetLocations()->InAt(invoke->GetSpecialInputIndex());
      break;
    case HInvokeStaticOrDirect::MethodLoadKind::kDirectAddress:
      __ movl(temp.AsRegister<Register>(), Immediate(invoke->GetMethodAddress()));
      break;
    case HInvokeStaticOrDirect::MethodLoadKind::kDirectAddressWithFixup:
      __ movl(temp.AsRegister<Register>(), Immediate(/* placeholder */ 0));
      method_patches_.emplace_back(invoke->GetTargetMethod());
      __ Bind(&method_patches_.back().label);  // Bind the label at the end of the "movl" insn.
      break;
    case HInvokeStaticOrDirect::MethodLoadKind::kDexCachePcRelative: {
      Register base_reg = GetInvokeStaticOrDirectExtraParameter(invoke,
                                                                temp.AsRegister<Register>());
      __ movl(temp.AsRegister<Register>(), Address(base_reg, kDummy32BitOffset));
      // Bind a new fixup label at the end of the "movl" insn.
      uint32_t offset = invoke->GetDexCacheArrayOffset();
      __ Bind(NewPcRelativeDexCacheArrayPatch(*invoke->GetTargetMethod().dex_file, offset));
      break;
    }
    case HInvokeStaticOrDirect::MethodLoadKind::kDexCacheViaMethod: {
      Location current_method = invoke->GetLocations()->InAt(invoke->GetSpecialInputIndex());
      Register method_reg;
      Register reg = temp.AsRegister<Register>();
      if (current_method.IsRegister()) {
        method_reg = current_method.AsRegister<Register>();
      } else {
        DCHECK(invoke->GetLocations()->Intrinsified());
        DCHECK(!current_method.IsValid());
        method_reg = reg;
        __ movl(reg, Address(ESP, kCurrentMethodStackOffset));
      }
      // /* ArtMethod*[] */ temp = temp.ptr_sized_fields_->dex_cache_resolved_methods_;
      __ movl(reg, Address(method_reg,
                           ArtMethod::DexCacheResolvedMethodsOffset(kX86PointerSize).Int32Value()));
      // temp = temp[index_in_cache];
      // Note: Don't use invoke->GetTargetMethod() as it may point to a different dex file.
      uint32_t index_in_cache = invoke->GetDexMethodIndex();
      __ movl(reg, Address(reg, CodeGenerator::GetCachePointerOffset(index_in_cache)));
      break;
    }
  }

  switch (invoke->GetCodePtrLocation()) {
    case HInvokeStaticOrDirect::CodePtrLocation::kCallSelf:
      __ call(GetFrameEntryLabel());
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
      __ call(Address(callee_method.AsRegister<Register>(),
                      ArtMethod::EntryPointFromQuickCompiledCodeOffset(
                          kX86WordSize).Int32Value()));
      break;
  }

  DCHECK(!IsLeafMethod());
}

void CodeGeneratorX86::GenerateVirtualCall(HInvokeVirtual* invoke, Location temp_in) {
  Register temp = temp_in.AsRegister<Register>();
  uint32_t method_offset = mirror::Class::EmbeddedVTableEntryOffset(
      invoke->GetVTableIndex(), kX86PointerSize).Uint32Value();

  // Use the calling convention instead of the location of the receiver, as
  // intrinsics may have put the receiver in a different register. In the intrinsics
  // slow path, the arguments have been moved to the right place, so here we are
  // guaranteed that the receiver is the first register of the calling convention.
  InvokeDexCallingConvention calling_convention;
  Register receiver = calling_convention.GetRegisterAt(0);
  uint32_t class_offset = mirror::Object::ClassOffset().Int32Value();
  // /* HeapReference<Class> */ temp = receiver->klass_
  __ movl(temp, Address(receiver, class_offset));
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
  __ movl(temp, Address(temp, method_offset));
  // call temp->GetEntryPoint();
  __ call(Address(
      temp, ArtMethod::EntryPointFromQuickCompiledCodeOffset(kX86WordSize).Int32Value()));
}

void CodeGeneratorX86::RecordSimplePatch() {
  if (GetCompilerOptions().GetIncludePatchInformation()) {
    simple_patches_.emplace_back();
    __ Bind(&simple_patches_.back());
  }
}

void CodeGeneratorX86::RecordStringPatch(HLoadString* load_string) {
  string_patches_.emplace_back(load_string->GetDexFile(), load_string->GetStringIndex());
  __ Bind(&string_patches_.back().label);
}

Label* CodeGeneratorX86::NewPcRelativeDexCacheArrayPatch(const DexFile& dex_file,
                                                         uint32_t element_offset) {
  // Add the patch entry and bind its label at the end of the instruction.
  pc_relative_dex_cache_patches_.emplace_back(dex_file, element_offset);
  return &pc_relative_dex_cache_patches_.back().label;
}

void CodeGeneratorX86::EmitLinkerPatches(ArenaVector<LinkerPatch>* linker_patches) {
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
                                                              GetMethodAddressOffset(),
                                                              info.element_offset));
  }
  for (const Label& label : simple_patches_) {
    uint32_t literal_offset = label.Position() - kLabelPositionToLiteralOffsetAdjustment;
    linker_patches->push_back(LinkerPatch::RecordPosition(literal_offset));
  }
  if (GetCompilerOptions().GetCompilePic()) {
    for (const StringPatchInfo<Label>& info : string_patches_) {
      uint32_t literal_offset = info.label.Position() - kLabelPositionToLiteralOffsetAdjustment;
      linker_patches->push_back(LinkerPatch::RelativeStringPatch(literal_offset,
                                                                 &info.dex_file,
                                                                 GetMethodAddressOffset(),
                                                                 info.string_index));
    }
  } else {
    for (const StringPatchInfo<Label>& info : string_patches_) {
      uint32_t literal_offset = info.label.Position() - kLabelPositionToLiteralOffsetAdjustment;
      linker_patches->push_back(LinkerPatch::StringPatch(literal_offset,
                                                         &info.dex_file,
                                                         info.string_index));
    }
  }
}

void CodeGeneratorX86::MarkGCCard(Register temp,
                                  Register card,
                                  Register object,
                                  Register value,
                                  bool value_can_be_null) {
  NearLabel is_null;
  if (value_can_be_null) {
    __ testl(value, value);
    __ j(kEqual, &is_null);
  }
  __ fs()->movl(card, Address::Absolute(Thread::CardTableOffset<kX86WordSize>().Int32Value()));
  __ movl(temp, object);
  __ shrl(temp, Immediate(gc::accounting::CardTable::kCardShift));
  __ movb(Address(temp, card, TIMES_1, 0),
          X86ManagedRegister::FromCpuRegister(card).AsByteRegister());
  if (value_can_be_null) {
    __ Bind(&is_null);
  }
}

void LocationsBuilderX86::HandleFieldGet(HInstruction* instruction, const FieldInfo& field_info) {
  DCHECK(instruction->IsInstanceFieldGet() || instruction->IsStaticFieldGet());

  bool object_field_get_with_read_barrier =
      kEmitCompilerReadBarrier && (instruction->GetType() == Primitive::kPrimNot);
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction,
                                                   kEmitCompilerReadBarrier ?
                                                       LocationSummary::kCallOnSlowPath :
                                                       LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());

  if (Primitive::IsFloatingPointType(instruction->GetType())) {
    locations->SetOut(Location::RequiresFpuRegister());
  } else {
    // The output overlaps in case of long: we don't want the low move
    // to overwrite the object's location.  Likewise, in the case of
    // an object field get with read barriers enabled, we do not want
    // the move to overwrite the object's location, as we need it to emit
    // the read barrier.
    locations->SetOut(
        Location::RequiresRegister(),
        (object_field_get_with_read_barrier || instruction->GetType() == Primitive::kPrimLong) ?
            Location::kOutputOverlap :
            Location::kNoOutputOverlap);
  }

  if (field_info.IsVolatile() && (field_info.GetFieldType() == Primitive::kPrimLong)) {
    // Long values can be loaded atomically into an XMM using movsd.
    // So we use an XMM register as a temp to achieve atomicity (first
    // load the temp into the XMM and then copy the XMM into the
    // output, 32 bits at a time).
    locations->AddTemp(Location::RequiresFpuRegister());
  } else if (object_field_get_with_read_barrier && kUseBakerReadBarrier) {
    // We need a temporary register for the read barrier marking slow
    // path in CodeGeneratorX86::GenerateFieldLoadWithBakerReadBarrier.
    locations->AddTemp(Location::RequiresRegister());
  }
}

void InstructionCodeGeneratorX86::HandleFieldGet(HInstruction* instruction,
                                                 const FieldInfo& field_info) {
  DCHECK(instruction->IsInstanceFieldGet() || instruction->IsStaticFieldGet());

  LocationSummary* locations = instruction->GetLocations();
  Location base_loc = locations->InAt(0);
  Register base = base_loc.AsRegister<Register>();
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
      __ movl(out.AsRegister<Register>(), Address(base, offset));
      break;

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
        __ movl(out.AsRegister<Register>(), Address(base, offset));
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

  if (field_type == Primitive::kPrimNot || field_type == Primitive::kPrimLong) {
    // Potential implicit null checks, in the case of reference or
    // long fields, are handled in the previous switch statement.
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
    if (is_volatile && field_type == Primitive::kPrimDouble) {
      // In order to satisfy the semantics of volatile, this must be a single instruction store.
      locations->SetInAt(1, Location::RequiresFpuRegister());
    } else {
      locations->SetInAt(1, Location::FpuRegisterOrConstant(instruction->InputAt(1)));
    }
  } else if (is_volatile && field_type == Primitive::kPrimLong) {
    // In order to satisfy the semantics of volatile, this must be a single instruction store.
    locations->SetInAt(1, Location::RequiresRegister());

    // 64bits value can be atomically written to an address with movsd and an XMM register.
    // We need two XMM registers because there's no easier way to (bit) copy a register pair
    // into a single XMM register (we copy each pair part into the XMMs and then interleave them).
    // NB: We could make the register allocator understand fp_reg <-> core_reg moves but given the
    // isolated cases when we need this it isn't worth adding the extra complexity.
    locations->AddTemp(Location::RequiresFpuRegister());
    locations->AddTemp(Location::RequiresFpuRegister());
  } else {
    locations->SetInAt(1, Location::RegisterOrConstant(instruction->InputAt(1)));

    if (CodeGenerator::StoreNeedsWriteBarrier(field_type, instruction->InputAt(1))) {
      // Temporary registers for the write barrier.
      locations->AddTemp(Location::RequiresRegister());  // May be used for reference poisoning too.
      // Ensure the card is in a byte register.
      locations->AddTemp(Location::RegisterLocation(ECX));
    }
  }
}

void InstructionCodeGeneratorX86::HandleFieldSet(HInstruction* instruction,
                                                 const FieldInfo& field_info,
                                                 bool value_can_be_null) {
  DCHECK(instruction->IsInstanceFieldSet() || instruction->IsStaticFieldSet());

  LocationSummary* locations = instruction->GetLocations();
  Register base = locations->InAt(0).AsRegister<Register>();
  Location value = locations->InAt(1);
  bool is_volatile = field_info.IsVolatile();
  Primitive::Type field_type = field_info.GetFieldType();
  uint32_t offset = field_info.GetFieldOffset().Uint32Value();
  bool needs_write_barrier =
      CodeGenerator::StoreNeedsWriteBarrier(field_type, instruction->InputAt(1));

  if (is_volatile) {
    codegen_->GenerateMemoryBarrier(MemBarrierKind::kAnyStore);
  }

  bool maybe_record_implicit_null_check_done = false;

  switch (field_type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte: {
      __ movb(Address(base, offset), value.AsRegister<ByteRegister>());
      break;
    }

    case Primitive::kPrimShort:
    case Primitive::kPrimChar: {
      if (value.IsConstant()) {
        int16_t v = CodeGenerator::GetInt32ValueOf(value.GetConstant());
        __ movw(Address(base, offset), Immediate(v));
      } else {
        __ movw(Address(base, offset), value.AsRegister<Register>());
      }
      break;
    }

    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      if (kPoisonHeapReferences && needs_write_barrier) {
        // Note that in the case where `value` is a null reference,
        // we do not enter this block, as the reference does not
        // need poisoning.
        DCHECK_EQ(field_type, Primitive::kPrimNot);
        Register temp = locations->GetTemp(0).AsRegister<Register>();
        __ movl(temp, value.AsRegister<Register>());
        __ PoisonHeapReference(temp);
        __ movl(Address(base, offset), temp);
      } else if (value.IsConstant()) {
        int32_t v = CodeGenerator::GetInt32ValueOf(value.GetConstant());
        __ movl(Address(base, offset), Immediate(v));
      } else {
        DCHECK(value.IsRegister()) << value;
        __ movl(Address(base, offset), value.AsRegister<Register>());
      }
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
      } else if (value.IsConstant()) {
        int64_t v = CodeGenerator::GetInt64ValueOf(value.GetConstant());
        __ movl(Address(base, offset), Immediate(Low32Bits(v)));
        codegen_->MaybeRecordImplicitNullCheck(instruction);
        __ movl(Address(base, kX86WordSize + offset), Immediate(High32Bits(v)));
      } else {
        __ movl(Address(base, offset), value.AsRegisterPairLow<Register>());
        codegen_->MaybeRecordImplicitNullCheck(instruction);
        __ movl(Address(base, kX86WordSize + offset), value.AsRegisterPairHigh<Register>());
      }
      maybe_record_implicit_null_check_done = true;
      break;
    }

    case Primitive::kPrimFloat: {
      if (value.IsConstant()) {
        int32_t v = CodeGenerator::GetInt32ValueOf(value.GetConstant());
        __ movl(Address(base, offset), Immediate(v));
      } else {
        __ movss(Address(base, offset), value.AsFpuRegister<XmmRegister>());
      }
      break;
    }

    case Primitive::kPrimDouble: {
      if (value.IsConstant()) {
        int64_t v = CodeGenerator::GetInt64ValueOf(value.GetConstant());
        __ movl(Address(base, offset), Immediate(Low32Bits(v)));
        codegen_->MaybeRecordImplicitNullCheck(instruction);
        __ movl(Address(base, kX86WordSize + offset), Immediate(High32Bits(v)));
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

  if (needs_write_barrier) {
    Register temp = locations->GetTemp(0).AsRegister<Register>();
    Register card = locations->GetTemp(1).AsRegister<Register>();
    codegen_->MarkGCCard(temp, card, base, value.AsRegister<Register>(), value_can_be_null);
  }

  if (is_volatile) {
    codegen_->GenerateMemoryBarrier(MemBarrierKind::kAnyAny);
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
  HandleFieldSet(instruction, instruction->GetFieldInfo(), instruction->GetValueCanBeNull());
}

void LocationsBuilderX86::VisitInstanceFieldSet(HInstanceFieldSet* instruction) {
  HandleFieldSet(instruction, instruction->GetFieldInfo());
}

void InstructionCodeGeneratorX86::VisitInstanceFieldSet(HInstanceFieldSet* instruction) {
  HandleFieldSet(instruction, instruction->GetFieldInfo(), instruction->GetValueCanBeNull());
}

void LocationsBuilderX86::VisitInstanceFieldGet(HInstanceFieldGet* instruction) {
  HandleFieldGet(instruction, instruction->GetFieldInfo());
}

void InstructionCodeGeneratorX86::VisitInstanceFieldGet(HInstanceFieldGet* instruction) {
  HandleFieldGet(instruction, instruction->GetFieldInfo());
}

void LocationsBuilderX86::VisitUnresolvedInstanceFieldGet(
    HUnresolvedInstanceFieldGet* instruction) {
  FieldAccessCallingConventionX86 calling_convention;
  codegen_->CreateUnresolvedFieldLocationSummary(
      instruction, instruction->GetFieldType(), calling_convention);
}

void InstructionCodeGeneratorX86::VisitUnresolvedInstanceFieldGet(
    HUnresolvedInstanceFieldGet* instruction) {
  FieldAccessCallingConventionX86 calling_convention;
  codegen_->GenerateUnresolvedFieldAccess(instruction,
                                          instruction->GetFieldType(),
                                          instruction->GetFieldIndex(),
                                          instruction->GetDexPc(),
                                          calling_convention);
}

void LocationsBuilderX86::VisitUnresolvedInstanceFieldSet(
    HUnresolvedInstanceFieldSet* instruction) {
  FieldAccessCallingConventionX86 calling_convention;
  codegen_->CreateUnresolvedFieldLocationSummary(
      instruction, instruction->GetFieldType(), calling_convention);
}

void InstructionCodeGeneratorX86::VisitUnresolvedInstanceFieldSet(
    HUnresolvedInstanceFieldSet* instruction) {
  FieldAccessCallingConventionX86 calling_convention;
  codegen_->GenerateUnresolvedFieldAccess(instruction,
                                          instruction->GetFieldType(),
                                          instruction->GetFieldIndex(),
                                          instruction->GetDexPc(),
                                          calling_convention);
}

void LocationsBuilderX86::VisitUnresolvedStaticFieldGet(
    HUnresolvedStaticFieldGet* instruction) {
  FieldAccessCallingConventionX86 calling_convention;
  codegen_->CreateUnresolvedFieldLocationSummary(
      instruction, instruction->GetFieldType(), calling_convention);
}

void InstructionCodeGeneratorX86::VisitUnresolvedStaticFieldGet(
    HUnresolvedStaticFieldGet* instruction) {
  FieldAccessCallingConventionX86 calling_convention;
  codegen_->GenerateUnresolvedFieldAccess(instruction,
                                          instruction->GetFieldType(),
                                          instruction->GetFieldIndex(),
                                          instruction->GetDexPc(),
                                          calling_convention);
}

void LocationsBuilderX86::VisitUnresolvedStaticFieldSet(
    HUnresolvedStaticFieldSet* instruction) {
  FieldAccessCallingConventionX86 calling_convention;
  codegen_->CreateUnresolvedFieldLocationSummary(
      instruction, instruction->GetFieldType(), calling_convention);
}

void InstructionCodeGeneratorX86::VisitUnresolvedStaticFieldSet(
    HUnresolvedStaticFieldSet* instruction) {
  FieldAccessCallingConventionX86 calling_convention;
  codegen_->GenerateUnresolvedFieldAccess(instruction,
                                          instruction->GetFieldType(),
                                          instruction->GetFieldIndex(),
                                          instruction->GetDexPc(),
                                          calling_convention);
}

void LocationsBuilderX86::VisitNullCheck(HNullCheck* instruction) {
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

void CodeGeneratorX86::GenerateImplicitNullCheck(HNullCheck* instruction) {
  if (CanMoveNullCheckToUser(instruction)) {
    return;
  }
  LocationSummary* locations = instruction->GetLocations();
  Location obj = locations->InAt(0);

  __ testl(EAX, Address(obj.AsRegister<Register>(), 0));
  RecordPcInfo(instruction, instruction->GetDexPc());
}

void CodeGeneratorX86::GenerateExplicitNullCheck(HNullCheck* instruction) {
  SlowPathCode* slow_path = new (GetGraph()->GetArena()) NullCheckSlowPathX86(instruction);
  AddSlowPath(slow_path);

  LocationSummary* locations = instruction->GetLocations();
  Location obj = locations->InAt(0);

  if (obj.IsRegister()) {
    __ testl(obj.AsRegister<Register>(), obj.AsRegister<Register>());
  } else if (obj.IsStackSlot()) {
    __ cmpl(Address(ESP, obj.GetStackIndex()), Immediate(0));
  } else {
    DCHECK(obj.IsConstant()) << obj;
    DCHECK(obj.GetConstant()->IsNullConstant());
    __ jmp(slow_path->GetEntryLabel());
    return;
  }
  __ j(kEqual, slow_path->GetEntryLabel());
}

void InstructionCodeGeneratorX86::VisitNullCheck(HNullCheck* instruction) {
  codegen_->GenerateNullCheck(instruction);
}

void LocationsBuilderX86::VisitArrayGet(HArrayGet* instruction) {
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
    // The output overlaps in case of long: we don't want the low move
    // to overwrite the array's location.  Likewise, in the case of an
    // object array get with read barriers enabled, we do not want the
    // move to overwrite the array's location, as we need it to emit
    // the read barrier.
    locations->SetOut(
        Location::RequiresRegister(),
        (instruction->GetType() == Primitive::kPrimLong || object_array_get_with_read_barrier) ?
            Location::kOutputOverlap :
            Location::kNoOutputOverlap);
  }
  // We need a temporary register for the read barrier marking slow
  // path in CodeGeneratorX86::GenerateArrayLoadWithBakerReadBarrier.
  if (object_array_get_with_read_barrier && kUseBakerReadBarrier) {
    locations->AddTemp(Location::RequiresRegister());
  }
}

void InstructionCodeGeneratorX86::VisitArrayGet(HArrayGet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Location obj_loc = locations->InAt(0);
  Register obj = obj_loc.AsRegister<Register>();
  Location index = locations->InAt(1);
  Location out_loc = locations->Out();

  Primitive::Type type = instruction->GetType();
  switch (type) {
    case Primitive::kPrimBoolean: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(uint8_t)).Uint32Value();
      Register out = out_loc.AsRegister<Register>();
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
      Register out = out_loc.AsRegister<Register>();
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
      Register out = out_loc.AsRegister<Register>();
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
      Register out = out_loc.AsRegister<Register>();
      if (index.IsConstant()) {
        __ movzxw(out, Address(obj,
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_2) + data_offset));
      } else {
        __ movzxw(out, Address(obj, index.AsRegister<Register>(), TIMES_2, data_offset));
      }
      break;
    }

    case Primitive::kPrimInt: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(int32_t)).Uint32Value();
      Register out = out_loc.AsRegister<Register>();
      if (index.IsConstant()) {
        __ movl(out, Address(obj,
            (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_4) + data_offset));
      } else {
        __ movl(out, Address(obj, index.AsRegister<Register>(), TIMES_4, data_offset));
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
        Register out = out_loc.AsRegister<Register>();
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
          __ movl(out, Address(obj, index.AsRegister<Register>(), TIMES_4, data_offset));
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
      DCHECK_NE(obj, out_loc.AsRegisterPairLow<Register>());
      if (index.IsConstant()) {
        size_t offset = (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_8) + data_offset;
        __ movl(out_loc.AsRegisterPairLow<Register>(), Address(obj, offset));
        codegen_->MaybeRecordImplicitNullCheck(instruction);
        __ movl(out_loc.AsRegisterPairHigh<Register>(), Address(obj, offset + kX86WordSize));
      } else {
        __ movl(out_loc.AsRegisterPairLow<Register>(),
                Address(obj, index.AsRegister<Register>(), TIMES_8, data_offset));
        codegen_->MaybeRecordImplicitNullCheck(instruction);
        __ movl(out_loc.AsRegisterPairHigh<Register>(),
                Address(obj, index.AsRegister<Register>(), TIMES_8, data_offset + kX86WordSize));
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
        __ movss(out, Address(obj, index.AsRegister<Register>(), TIMES_4, data_offset));
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
        __ movsd(out, Address(obj, index.AsRegister<Register>(), TIMES_8, data_offset));
      }
      break;
    }

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << type;
      UNREACHABLE();
  }

  if (type == Primitive::kPrimNot || type == Primitive::kPrimLong) {
    // Potential implicit null checks, in the case of reference or
    // long arrays, are handled in the previous switch statement.
  } else {
    codegen_->MaybeRecordImplicitNullCheck(instruction);
  }
}

void LocationsBuilderX86::VisitArraySet(HArraySet* instruction) {
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
    locations->SetInAt(2, Location::FpuRegisterOrConstant(instruction->InputAt(2)));
  } else {
    locations->SetInAt(2, Location::RegisterOrConstant(instruction->InputAt(2)));
  }
  if (needs_write_barrier) {
    // Temporary registers for the write barrier.
    locations->AddTemp(Location::RequiresRegister());  // Possibly used for ref. poisoning too.
    // Ensure the card is in a byte register.
    locations->AddTemp(Location::RegisterLocation(ECX));
  }
}

void InstructionCodeGeneratorX86::VisitArraySet(HArraySet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Location array_loc = locations->InAt(0);
  Register array = array_loc.AsRegister<Register>();
  Location index = locations->InAt(1);
  Location value = locations->InAt(2);
  Primitive::Type value_type = instruction->GetComponentType();
  uint32_t class_offset = mirror::Object::ClassOffset().Int32Value();
  uint32_t super_offset = mirror::Class::SuperClassOffset().Int32Value();
  uint32_t component_offset = mirror::Class::ComponentTypeOffset().Int32Value();
  bool may_need_runtime_call_for_type_check = instruction->NeedsTypeCheck();
  bool needs_write_barrier =
      CodeGenerator::StoreNeedsWriteBarrier(value_type, instruction->GetValue());

  switch (value_type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte: {
      uint32_t offset = mirror::Array::DataOffset(sizeof(uint8_t)).Uint32Value();
      Address address = index.IsConstant()
          ? Address(array, (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_1) + offset)
          : Address(array, index.AsRegister<Register>(), TIMES_1, offset);
      if (value.IsRegister()) {
        __ movb(address, value.AsRegister<ByteRegister>());
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
          : Address(array, index.AsRegister<Register>(), TIMES_2, offset);
      if (value.IsRegister()) {
        __ movw(address, value.AsRegister<Register>());
      } else {
        __ movw(address, Immediate(value.GetConstant()->AsIntConstant()->GetValue()));
      }
      codegen_->MaybeRecordImplicitNullCheck(instruction);
      break;
    }

    case Primitive::kPrimNot: {
      uint32_t offset = mirror::Array::DataOffset(sizeof(int32_t)).Uint32Value();
      Address address = index.IsConstant()
          ? Address(array, (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_4) + offset)
          : Address(array, index.AsRegister<Register>(), TIMES_4, offset);

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
      Register register_value = value.AsRegister<Register>();
      NearLabel done, not_null, do_put;
      SlowPathCode* slow_path = nullptr;
      Register temp = locations->GetTemp(0).AsRegister<Register>();
      if (may_need_runtime_call_for_type_check) {
        slow_path = new (GetGraph()->GetArena()) ArraySetSlowPathX86(instruction);
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

      Register card = locations->GetTemp(1).AsRegister<Register>();
      codegen_->MarkGCCard(
          temp, card, array, value.AsRegister<Register>(), instruction->GetValueCanBeNull());
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
          : Address(array, index.AsRegister<Register>(), TIMES_4, offset);
      if (value.IsRegister()) {
        __ movl(address, value.AsRegister<Register>());
      } else {
        DCHECK(value.IsConstant()) << value;
        int32_t v = CodeGenerator::GetInt32ValueOf(value.GetConstant());
        __ movl(address, Immediate(v));
      }
      codegen_->MaybeRecordImplicitNullCheck(instruction);
      break;
    }

    case Primitive::kPrimLong: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(int64_t)).Uint32Value();
      if (index.IsConstant()) {
        size_t offset = (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_8) + data_offset;
        if (value.IsRegisterPair()) {
          __ movl(Address(array, offset), value.AsRegisterPairLow<Register>());
          codegen_->MaybeRecordImplicitNullCheck(instruction);
          __ movl(Address(array, offset + kX86WordSize), value.AsRegisterPairHigh<Register>());
        } else {
          DCHECK(value.IsConstant());
          int64_t val = value.GetConstant()->AsLongConstant()->GetValue();
          __ movl(Address(array, offset), Immediate(Low32Bits(val)));
          codegen_->MaybeRecordImplicitNullCheck(instruction);
          __ movl(Address(array, offset + kX86WordSize), Immediate(High32Bits(val)));
        }
      } else {
        if (value.IsRegisterPair()) {
          __ movl(Address(array, index.AsRegister<Register>(), TIMES_8, data_offset),
                  value.AsRegisterPairLow<Register>());
          codegen_->MaybeRecordImplicitNullCheck(instruction);
          __ movl(Address(array, index.AsRegister<Register>(), TIMES_8, data_offset + kX86WordSize),
                  value.AsRegisterPairHigh<Register>());
        } else {
          DCHECK(value.IsConstant());
          int64_t val = value.GetConstant()->AsLongConstant()->GetValue();
          __ movl(Address(array, index.AsRegister<Register>(), TIMES_8, data_offset),
                  Immediate(Low32Bits(val)));
          codegen_->MaybeRecordImplicitNullCheck(instruction);
          __ movl(Address(array, index.AsRegister<Register>(), TIMES_8, data_offset + kX86WordSize),
                  Immediate(High32Bits(val)));
        }
      }
      break;
    }

    case Primitive::kPrimFloat: {
      uint32_t offset = mirror::Array::DataOffset(sizeof(float)).Uint32Value();
      Address address = index.IsConstant()
          ? Address(array, (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_4) + offset)
          : Address(array, index.AsRegister<Register>(), TIMES_4, offset);
      if (value.IsFpuRegister()) {
        __ movss(address, value.AsFpuRegister<XmmRegister>());
      } else {
        DCHECK(value.IsConstant());
        int32_t v = bit_cast<int32_t, float>(value.GetConstant()->AsFloatConstant()->GetValue());
        __ movl(address, Immediate(v));
      }
      codegen_->MaybeRecordImplicitNullCheck(instruction);
      break;
    }

    case Primitive::kPrimDouble: {
      uint32_t offset = mirror::Array::DataOffset(sizeof(double)).Uint32Value();
      Address address = index.IsConstant()
          ? Address(array, (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_8) + offset)
          : Address(array, index.AsRegister<Register>(), TIMES_8, offset);
      if (value.IsFpuRegister()) {
        __ movsd(address, value.AsFpuRegister<XmmRegister>());
      } else {
        DCHECK(value.IsConstant());
        Address address_hi = index.IsConstant() ?
            Address(array, (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_8) +
                           offset + kX86WordSize) :
            Address(array, index.AsRegister<Register>(), TIMES_8, offset + kX86WordSize);
        int64_t v = bit_cast<int64_t, double>(value.GetConstant()->AsDoubleConstant()->GetValue());
        __ movl(address, Immediate(Low32Bits(v)));
        codegen_->MaybeRecordImplicitNullCheck(instruction);
        __ movl(address_hi, Immediate(High32Bits(v)));
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

void InstructionCodeGeneratorX86::VisitBoundsCheck(HBoundsCheck* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Location index_loc = locations->InAt(0);
  Location length_loc = locations->InAt(1);
  SlowPathCode* slow_path =
    new (GetGraph()->GetArena()) BoundsCheckSlowPathX86(instruction);

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

void LocationsBuilderX86::VisitParallelMove(HParallelMove* instruction ATTRIBUTE_UNUSED) {
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

  __ fs()->cmpw(Address::Absolute(Thread::ThreadFlagsOffset<kX86WordSize>().Int32Value()),
                Immediate(0));
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
  MoveOperands* move = moves_[index];
  Location source = move->GetSource();
  Location destination = move->GetDestination();

  if (source.IsRegister()) {
    if (destination.IsRegister()) {
      __ movl(destination.AsRegister<Register>(), source.AsRegister<Register>());
    } else if (destination.IsFpuRegister()) {
      __ movd(destination.AsFpuRegister<XmmRegister>(), source.AsRegister<Register>());
    } else {
      DCHECK(destination.IsStackSlot());
      __ movl(Address(ESP, destination.GetStackIndex()), source.AsRegister<Register>());
    }
  } else if (source.IsRegisterPair()) {
      size_t elem_size = Primitive::ComponentSize(Primitive::kPrimInt);
      // Create stack space for 2 elements.
      __ subl(ESP, Immediate(2 * elem_size));
      __ movl(Address(ESP, 0), source.AsRegisterPairLow<Register>());
      __ movl(Address(ESP, elem_size), source.AsRegisterPairHigh<Register>());
      __ movsd(destination.AsFpuRegister<XmmRegister>(), Address(ESP, 0));
      // And remove the temporary stack space we allocated.
      __ addl(ESP, Immediate(2 * elem_size));
  } else if (source.IsFpuRegister()) {
    if (destination.IsRegister()) {
      __ movd(destination.AsRegister<Register>(), source.AsFpuRegister<XmmRegister>());
    } else if (destination.IsFpuRegister()) {
      __ movaps(destination.AsFpuRegister<XmmRegister>(), source.AsFpuRegister<XmmRegister>());
    } else if (destination.IsRegisterPair()) {
      XmmRegister src_reg = source.AsFpuRegister<XmmRegister>();
      __ movd(destination.AsRegisterPairLow<Register>(), src_reg);
      __ psrlq(src_reg, Immediate(32));
      __ movd(destination.AsRegisterPairHigh<Register>(), src_reg);
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
    if (destination.IsRegisterPair()) {
      __ movl(destination.AsRegisterPairLow<Register>(), Address(ESP, source.GetStackIndex()));
      __ movl(destination.AsRegisterPairHigh<Register>(),
              Address(ESP, source.GetHighStackIndex(kX86WordSize)));
    } else if (destination.IsFpuRegister()) {
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
  MoveOperands* move = moves_[index];
  Location source = move->GetSource();
  Location destination = move->GetDestination();

  if (source.IsRegister() && destination.IsRegister()) {
    // Use XOR swap algorithm to avoid serializing XCHG instruction or using a temporary.
    DCHECK_NE(destination.AsRegister<Register>(), source.AsRegister<Register>());
    __ xorl(destination.AsRegister<Register>(), source.AsRegister<Register>());
    __ xorl(source.AsRegister<Register>(), destination.AsRegister<Register>());
    __ xorl(destination.AsRegister<Register>(), source.AsRegister<Register>());
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
  InvokeRuntimeCallingConvention calling_convention;
  CodeGenerator::CreateLoadClassLocationSummary(
      cls,
      Location::RegisterLocation(calling_convention.GetRegisterAt(0)),
      Location::RegisterLocation(EAX),
      /* code_generator_supports_read_barrier */ true);
}

void InstructionCodeGeneratorX86::VisitLoadClass(HLoadClass* cls) {
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
  Register out = out_loc.AsRegister<Register>();
  Register current_method = locations->InAt(0).AsRegister<Register>();

  if (cls->IsReferrersClass()) {
    DCHECK(!cls->CanCallRuntime());
    DCHECK(!cls->MustGenerateClinitCheck());
    // /* GcRoot<mirror::Class> */ out = current_method->declaring_class_
    GenerateGcRootFieldLoad(
        cls, out_loc, Address(current_method, ArtMethod::DeclaringClassOffset().Int32Value()));
  } else {
    // /* GcRoot<mirror::Class>[] */ out =
    //        current_method.ptr_sized_fields_->dex_cache_resolved_types_
    __ movl(out, Address(current_method,
                         ArtMethod::DexCacheResolvedTypesOffset(kX86PointerSize).Int32Value()));
    // /* GcRoot<mirror::Class> */ out = out[type_index]
    GenerateGcRootFieldLoad(
        cls, out_loc, Address(out, CodeGenerator::GetCacheOffset(cls->GetTypeIndex())));

    if (!cls->IsInDexCache() || cls->MustGenerateClinitCheck()) {
      DCHECK(cls->CanCallRuntime());
      SlowPathCode* slow_path = new (GetGraph()->GetArena()) LoadClassSlowPathX86(
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
  SlowPathCode* slow_path = new (GetGraph()->GetArena()) LoadClassSlowPathX86(
      check->GetLoadClass(), check, check->GetDexPc(), true);
  codegen_->AddSlowPath(slow_path);
  GenerateClassInitializationCheck(slow_path,
                                   check->GetLocations()->InAt(0).AsRegister<Register>());
}

void InstructionCodeGeneratorX86::GenerateClassInitializationCheck(
    SlowPathCode* slow_path, Register class_reg) {
  __ cmpl(Address(class_reg,  mirror::Class::StatusOffset().Int32Value()),
          Immediate(mirror::Class::kStatusInitialized));
  __ j(kLess, slow_path->GetEntryLabel());
  __ Bind(slow_path->GetExitLabel());
  // No need for memory fence, thanks to the X86 memory model.
}

HLoadString::LoadKind CodeGeneratorX86::GetSupportedLoadStringKind(
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
      FALLTHROUGH_INTENDED;
    case HLoadString::LoadKind::kDexCachePcRelative:
      DCHECK(!Runtime::Current()->UseJitCompilation());  // Note: boot image is also non-JIT.
      // We disable pc-relative load when there is an irreducible loop, as the optimization
      // is incompatible with it.
      // TODO: Create as many X86ComputeBaseMethodAddress instructions as needed for methods
      // with irreducible loops.
      if (GetGraph()->HasIrreducibleLoops()) {
        return HLoadString::LoadKind::kDexCacheViaMethod;
      }
      break;
    case HLoadString::LoadKind::kBootImageAddress:
      break;
    case HLoadString::LoadKind::kDexCacheAddress:
      DCHECK(Runtime::Current()->UseJitCompilation());
      break;
    case HLoadString::LoadKind::kDexCacheViaMethod:
      break;
  }
  return desired_string_load_kind;
}

void LocationsBuilderX86::VisitLoadString(HLoadString* load) {
  LocationSummary::CallKind call_kind = (load->NeedsEnvironment() || kEmitCompilerReadBarrier)
      ? LocationSummary::kCallOnSlowPath
      : LocationSummary::kNoCall;
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(load, call_kind);
  HLoadString::LoadKind load_kind = load->GetLoadKind();
  if (load_kind == HLoadString::LoadKind::kDexCacheViaMethod ||
      load_kind == HLoadString::LoadKind::kBootImageLinkTimePcRelative ||
      load_kind == HLoadString::LoadKind::kDexCachePcRelative) {
    locations->SetInAt(0, Location::RequiresRegister());
  }
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorX86::VisitLoadString(HLoadString* load) {
  LocationSummary* locations = load->GetLocations();
  Location out_loc = locations->Out();
  Register out = out_loc.AsRegister<Register>();

  switch (load->GetLoadKind()) {
    case HLoadString::LoadKind::kBootImageLinkTimeAddress: {
      DCHECK(!kEmitCompilerReadBarrier);
      __ movl(out, Immediate(/* placeholder */ 0));
      codegen_->RecordStringPatch(load);
      return;  // No dex cache slow path.
    }
    case HLoadString::LoadKind::kBootImageLinkTimePcRelative: {
      DCHECK(!kEmitCompilerReadBarrier);
      Register method_address = locations->InAt(0).AsRegister<Register>();
      __ leal(out, Address(method_address, CodeGeneratorX86::kDummy32BitOffset));
      codegen_->RecordStringPatch(load);
      return;  // No dex cache slow path.
    }
    case HLoadString::LoadKind::kBootImageAddress: {
      DCHECK(!kEmitCompilerReadBarrier);
      DCHECK_NE(load->GetAddress(), 0u);
      uint32_t address = dchecked_integral_cast<uint32_t>(load->GetAddress());
      __ movl(out, Immediate(address));
      codegen_->RecordSimplePatch();
      return;  // No dex cache slow path.
    }
    case HLoadString::LoadKind::kDexCacheAddress: {
      DCHECK_NE(load->GetAddress(), 0u);
      uint32_t address = dchecked_integral_cast<uint32_t>(load->GetAddress());
      GenerateGcRootFieldLoad(load, out_loc, Address::Absolute(address));
      break;
    }
    case HLoadString::LoadKind::kDexCachePcRelative: {
      Register base_reg = locations->InAt(0).AsRegister<Register>();
      uint32_t offset = load->GetDexCacheElementOffset();
      Label* fixup_label = codegen_->NewPcRelativeDexCacheArrayPatch(load->GetDexFile(), offset);
      GenerateGcRootFieldLoad(
          load, out_loc, Address(base_reg, CodeGeneratorX86::kDummy32BitOffset), fixup_label);
      break;
    }
    case HLoadString::LoadKind::kDexCacheViaMethod: {
      Register current_method = locations->InAt(0).AsRegister<Register>();

      // /* GcRoot<mirror::Class> */ out = current_method->declaring_class_
      GenerateGcRootFieldLoad(
          load, out_loc, Address(current_method, ArtMethod::DeclaringClassOffset().Int32Value()));

      // /* GcRoot<mirror::String>[] */ out = out->dex_cache_strings_
      __ movl(out, Address(out, mirror::Class::DexCacheStringsOffset().Int32Value()));
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
    SlowPathCode* slow_path = new (GetGraph()->GetArena()) LoadStringSlowPathX86(load);
    codegen_->AddSlowPath(slow_path);
    __ testl(out, out);
    __ j(kEqual, slow_path->GetEntryLabel());
    __ Bind(slow_path->GetExitLabel());
  }
}

static Address GetExceptionTlsAddress() {
  return Address::Absolute(Thread::ExceptionOffset<kX86WordSize>().Int32Value());
}

void LocationsBuilderX86::VisitLoadException(HLoadException* load) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(load, LocationSummary::kNoCall);
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorX86::VisitLoadException(HLoadException* load) {
  __ fs()->movl(load->GetLocations()->Out().AsRegister<Register>(), GetExceptionTlsAddress());
}

void LocationsBuilderX86::VisitClearException(HClearException* clear) {
  new (GetGraph()->GetArena()) LocationSummary(clear, LocationSummary::kNoCall);
}

void InstructionCodeGeneratorX86::VisitClearException(HClearException* clear ATTRIBUTE_UNUSED) {
  __ fs()->movl(GetExceptionTlsAddress(), Immediate(0));
}

void LocationsBuilderX86::VisitThrow(HThrow* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCall);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
}

void InstructionCodeGeneratorX86::VisitThrow(HThrow* instruction) {
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

void LocationsBuilderX86::VisitInstanceOf(HInstanceOf* instruction) {
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
  // Note that TypeCheckSlowPathX86 uses this "out" register too.
  locations->SetOut(Location::RequiresRegister());
  // When read barriers are enabled, we need a temporary register for
  // some cases.
  if (TypeCheckNeedsATemporary(type_check_kind)) {
    locations->AddTemp(Location::RequiresRegister());
  }
}

void InstructionCodeGeneratorX86::VisitInstanceOf(HInstanceOf* instruction) {
  TypeCheckKind type_check_kind = instruction->GetTypeCheckKind();
  LocationSummary* locations = instruction->GetLocations();
  Location obj_loc = locations->InAt(0);
  Register obj = obj_loc.AsRegister<Register>();
  Location cls = locations->InAt(1);
  Location out_loc = locations->Out();
  Register out = out_loc.AsRegister<Register>();
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
        __ cmpl(out, cls.AsRegister<Register>());
      } else {
        DCHECK(cls.IsStackSlot()) << cls;
        __ cmpl(out, Address(ESP, cls.GetStackIndex()));
      }

      // Classes must be equal for the instanceof to succeed.
      __ j(kNotEqual, &zero);
      __ movl(out, Immediate(1));
      __ jmp(&done);
      break;
    }

    case TypeCheckKind::kAbstractClassCheck: {
      // If the class is abstract, we eagerly fetch the super class of the
      // object to avoid doing a comparison we know will fail.
      NearLabel loop;
      __ Bind(&loop);
      // /* HeapReference<Class> */ out = out->super_class_
      GenerateReferenceLoadOneRegister(instruction, out_loc, super_offset, maybe_temp_loc);
      __ testl(out, out);
      // If `out` is null, we use it for the result, and jump to `done`.
      __ j(kEqual, &done);
      if (cls.IsRegister()) {
        __ cmpl(out, cls.AsRegister<Register>());
      } else {
        DCHECK(cls.IsStackSlot()) << cls;
        __ cmpl(out, Address(ESP, cls.GetStackIndex()));
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
        __ cmpl(out, cls.AsRegister<Register>());
      } else {
        DCHECK(cls.IsStackSlot()) << cls;
        __ cmpl(out, Address(ESP, cls.GetStackIndex()));
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
        __ cmpl(out, cls.AsRegister<Register>());
      } else {
        DCHECK(cls.IsStackSlot()) << cls;
        __ cmpl(out, Address(ESP, cls.GetStackIndex()));
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
        __ cmpl(out, cls.AsRegister<Register>());
      } else {
        DCHECK(cls.IsStackSlot()) << cls;
        __ cmpl(out, Address(ESP, cls.GetStackIndex()));
      }
      DCHECK(locations->OnlyCallsOnSlowPath());
      slow_path = new (GetGraph()->GetArena()) TypeCheckSlowPathX86(instruction,
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
      slow_path = new (GetGraph()->GetArena()) TypeCheckSlowPathX86(instruction,
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

void LocationsBuilderX86::VisitCheckCast(HCheckCast* instruction) {
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
  // Note that TypeCheckSlowPathX86 uses this "temp" register too.
  locations->AddTemp(Location::RequiresRegister());
  // When read barriers are enabled, we need an additional temporary
  // register for some cases.
  if (TypeCheckNeedsATemporary(type_check_kind)) {
    locations->AddTemp(Location::RequiresRegister());
  }
}

void InstructionCodeGeneratorX86::VisitCheckCast(HCheckCast* instruction) {
  TypeCheckKind type_check_kind = instruction->GetTypeCheckKind();
  LocationSummary* locations = instruction->GetLocations();
  Location obj_loc = locations->InAt(0);
  Register obj = obj_loc.AsRegister<Register>();
  Location cls = locations->InAt(1);
  Location temp_loc = locations->GetTemp(0);
  Register temp = temp_loc.AsRegister<Register>();
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
      new (GetGraph()->GetArena()) TypeCheckSlowPathX86(instruction,
                                                        is_type_check_slow_path_fatal);
  codegen_->AddSlowPath(type_check_slow_path);

  NearLabel done;
  // Avoid null check if we know obj is not null.
  if (instruction->MustDoNullCheck()) {
    __ testl(obj, obj);
    __ j(kEqual, &done);
  }

  // /* HeapReference<Class> */ temp = obj->klass_
  GenerateReferenceLoadTwoRegisters(instruction, temp_loc, obj_loc, class_offset, maybe_temp2_loc);

  switch (type_check_kind) {
    case TypeCheckKind::kExactCheck:
    case TypeCheckKind::kArrayCheck: {
      if (cls.IsRegister()) {
        __ cmpl(temp, cls.AsRegister<Register>());
      } else {
        DCHECK(cls.IsStackSlot()) << cls;
        __ cmpl(temp, Address(ESP, cls.GetStackIndex()));
      }
      // Jump to slow path for throwing the exception or doing a
      // more involved array check.
      __ j(kNotEqual, type_check_slow_path->GetEntryLabel());
      break;
    }

    case TypeCheckKind::kAbstractClassCheck: {
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
        __ cmpl(temp, cls.AsRegister<Register>());
      } else {
        DCHECK(cls.IsStackSlot()) << cls;
        __ cmpl(temp, Address(ESP, cls.GetStackIndex()));
      }
      __ j(kNotEqual, &loop);
      break;
    }

    case TypeCheckKind::kClassHierarchyCheck: {
      // Walk over the class hierarchy to find a match.
      NearLabel loop;
      __ Bind(&loop);
      if (cls.IsRegister()) {
        __ cmpl(temp, cls.AsRegister<Register>());
      } else {
        DCHECK(cls.IsStackSlot()) << cls;
        __ cmpl(temp, Address(ESP, cls.GetStackIndex()));
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
      break;
    }

    case TypeCheckKind::kArrayObjectCheck: {
      // Do an exact check.
      NearLabel check_non_primitive_component_type;
      if (cls.IsRegister()) {
        __ cmpl(temp, cls.AsRegister<Register>());
      } else {
        DCHECK(cls.IsStackSlot()) << cls;
        __ cmpl(temp, Address(ESP, cls.GetStackIndex()));
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
      __ jmp(type_check_slow_path->GetEntryLabel());
      break;
  }
  __ Bind(&done);

  __ Bind(type_check_slow_path->GetExitLabel());
}

void LocationsBuilderX86::VisitMonitorOperation(HMonitorOperation* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCall);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
}

void InstructionCodeGeneratorX86::VisitMonitorOperation(HMonitorOperation* instruction) {
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

void InstructionCodeGeneratorX86::GenerateReferenceLoadOneRegister(HInstruction* instruction,
                                                                   Location out,
                                                                   uint32_t offset,
                                                                   Location maybe_temp) {
  Register out_reg = out.AsRegister<Register>();
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
      __ movl(maybe_temp.AsRegister<Register>(), out_reg);
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

void InstructionCodeGeneratorX86::GenerateReferenceLoadTwoRegisters(HInstruction* instruction,
                                                                    Location out,
                                                                    Location obj,
                                                                    uint32_t offset,
                                                                    Location maybe_temp) {
  Register out_reg = out.AsRegister<Register>();
  Register obj_reg = obj.AsRegister<Register>();
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

void InstructionCodeGeneratorX86::GenerateGcRootFieldLoad(HInstruction* instruction,
                                                          Location root,
                                                          const Address& address,
                                                          Label* fixup_label) {
  Register root_reg = root.AsRegister<Register>();
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
          new (GetGraph()->GetArena()) ReadBarrierMarkSlowPathX86(instruction, root, root);
      codegen_->AddSlowPath(slow_path);

      __ fs()->cmpl(Address::Absolute(Thread::IsGcMarkingOffset<kX86WordSize>().Int32Value()),
                    Immediate(0));
      __ j(kNotEqual, slow_path->GetEntryLabel());
      __ Bind(slow_path->GetExitLabel());
    } else {
      // GC root loaded through a slow path for read barriers other
      // than Baker's.
      // /* GcRoot<mirror::Object>* */ root = address
      __ leal(root_reg, address);
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

void CodeGeneratorX86::GenerateFieldLoadWithBakerReadBarrier(HInstruction* instruction,
                                                             Location ref,
                                                             Register obj,
                                                             uint32_t offset,
                                                             Location temp,
                                                             bool needs_null_check) {
  DCHECK(kEmitCompilerReadBarrier);
  DCHECK(kUseBakerReadBarrier);

  // /* HeapReference<Object> */ ref = *(obj + offset)
  Address src(obj, offset);
  GenerateReferenceLoadWithBakerReadBarrier(instruction, ref, obj, src, temp, needs_null_check);
}

void CodeGeneratorX86::GenerateArrayLoadWithBakerReadBarrier(HInstruction* instruction,
                                                             Location ref,
                                                             Register obj,
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
      Address(obj, index.AsRegister<Register>(), TIMES_4, data_offset);
  GenerateReferenceLoadWithBakerReadBarrier(instruction, ref, obj, src, temp, needs_null_check);
}

void CodeGeneratorX86::GenerateReferenceLoadWithBakerReadBarrier(HInstruction* instruction,
                                                                 Location ref,
                                                                 Register obj,
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
  //   (we use CodeGeneratorX86::GenerateMemoryBarrier instead here,
  //   which is a no-op thanks to the x86 memory model);
  // - it performs additional checks that we do not do here for
  //   performance reasons.

  Register ref_reg = ref.AsRegister<Register>();
  Register temp_reg = temp.AsRegister<Register>();
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
  // Note that this is a no-op, thanks to the x86 memory model.
  GenerateMemoryBarrier(MemBarrierKind::kLoadAny);

  // The actual reference load.
  // /* HeapReference<Object> */ ref = *src
  __ movl(ref_reg, src);

  // Object* ref = ref_addr->AsMirrorPtr()
  __ MaybeUnpoisonHeapReference(ref_reg);

  // Slow path used to mark the object `ref` when it is gray.
  SlowPathCode* slow_path =
      new (GetGraph()->GetArena()) ReadBarrierMarkSlowPathX86(instruction, ref, ref);
  AddSlowPath(slow_path);

  // if (rb_state == ReadBarrier::gray_ptr_)
  //   ref = ReadBarrier::Mark(ref);
  __ cmpl(temp_reg, Immediate(ReadBarrier::gray_ptr_));
  __ j(kEqual, slow_path->GetEntryLabel());
  __ Bind(slow_path->GetExitLabel());
}

void CodeGeneratorX86::GenerateReadBarrierSlow(HInstruction* instruction,
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
      ReadBarrierForHeapReferenceSlowPathX86(instruction, out, ref, obj, offset, index);
  AddSlowPath(slow_path);

  __ jmp(slow_path->GetEntryLabel());
  __ Bind(slow_path->GetExitLabel());
}

void CodeGeneratorX86::MaybeGenerateReadBarrierSlow(HInstruction* instruction,
                                                    Location out,
                                                    Location ref,
                                                    Location obj,
                                                    uint32_t offset,
                                                    Location index) {
  if (kEmitCompilerReadBarrier) {
    // Baker's read barriers shall be handled by the fast path
    // (CodeGeneratorX86::GenerateReferenceLoadWithBakerReadBarrier).
    DCHECK(!kUseBakerReadBarrier);
    // If heap poisoning is enabled, unpoisoning will be taken care of
    // by the runtime within the slow path.
    GenerateReadBarrierSlow(instruction, out, ref, obj, offset, index);
  } else if (kPoisonHeapReferences) {
    __ UnpoisonHeapReference(out.AsRegister<Register>());
  }
}

void CodeGeneratorX86::GenerateReadBarrierForRootSlow(HInstruction* instruction,
                                                      Location out,
                                                      Location root) {
  DCHECK(kEmitCompilerReadBarrier);

  // Insert a slow path based read barrier *after* the GC root load.
  //
  // Note that GC roots are not affected by heap poisoning, so we do
  // not need to do anything special for this here.
  SlowPathCode* slow_path =
      new (GetGraph()->GetArena()) ReadBarrierForRootSlowPathX86(instruction, out, root);
  AddSlowPath(slow_path);

  __ jmp(slow_path->GetEntryLabel());
  __ Bind(slow_path->GetExitLabel());
}

void LocationsBuilderX86::VisitBoundType(HBoundType* instruction ATTRIBUTE_UNUSED) {
  // Nothing to do, this should be removed during prepare for register allocator.
  LOG(FATAL) << "Unreachable";
}

void InstructionCodeGeneratorX86::VisitBoundType(HBoundType* instruction ATTRIBUTE_UNUSED) {
  // Nothing to do, this should be removed during prepare for register allocator.
  LOG(FATAL) << "Unreachable";
}

// Simple implementation of packed switch - generate cascaded compare/jumps.
void LocationsBuilderX86::VisitPackedSwitch(HPackedSwitch* switch_instr) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(switch_instr, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
}

void InstructionCodeGeneratorX86::GenPackedSwitchWithCompares(Register value_reg,
                                                              int32_t lower_bound,
                                                              uint32_t num_entries,
                                                              HBasicBlock* switch_block,
                                                              HBasicBlock* default_block) {
  // Figure out the correct compare values and jump conditions.
  // Handle the first compare/branch as a special case because it might
  // jump to the default case.
  DCHECK_GT(num_entries, 2u);
  Condition first_condition;
  uint32_t index;
  const ArenaVector<HBasicBlock*>& successors = switch_block->GetSuccessors();
  if (lower_bound != 0) {
    first_condition = kLess;
    __ cmpl(value_reg, Immediate(lower_bound));
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
    __ cmpl(value_reg, Immediate(compare_to_value));
    // Jump to successors[index] if value < case_value[index].
    __ j(first_condition, codegen_->GetLabelOf(successors[index]));
    // Jump to successors[index + 1] if value == case_value[index + 1].
    __ j(kEqual, codegen_->GetLabelOf(successors[index + 1]));
  }

  if (index != num_entries) {
    // There are an odd number of entries. Handle the last one.
    DCHECK_EQ(index + 1, num_entries);
    __ cmpl(value_reg, Immediate(lower_bound + index));
    __ j(kEqual, codegen_->GetLabelOf(successors[index]));
  }

  // And the default for any other value.
  if (!codegen_->GoesToNextBlock(switch_block, default_block)) {
    __ jmp(codegen_->GetLabelOf(default_block));
  }
}

void InstructionCodeGeneratorX86::VisitPackedSwitch(HPackedSwitch* switch_instr) {
  int32_t lower_bound = switch_instr->GetStartValue();
  uint32_t num_entries = switch_instr->GetNumEntries();
  LocationSummary* locations = switch_instr->GetLocations();
  Register value_reg = locations->InAt(0).AsRegister<Register>();

  GenPackedSwitchWithCompares(value_reg,
                              lower_bound,
                              num_entries,
                              switch_instr->GetBlock(),
                              switch_instr->GetDefaultBlock());
}

void LocationsBuilderX86::VisitX86PackedSwitch(HX86PackedSwitch* switch_instr) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(switch_instr, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());

  // Constant area pointer.
  locations->SetInAt(1, Location::RequiresRegister());

  // And the temporary we need.
  locations->AddTemp(Location::RequiresRegister());
}

void InstructionCodeGeneratorX86::VisitX86PackedSwitch(HX86PackedSwitch* switch_instr) {
  int32_t lower_bound = switch_instr->GetStartValue();
  uint32_t num_entries = switch_instr->GetNumEntries();
  LocationSummary* locations = switch_instr->GetLocations();
  Register value_reg = locations->InAt(0).AsRegister<Register>();
  HBasicBlock* default_block = switch_instr->GetDefaultBlock();

  if (num_entries <= kPackedSwitchJumpTableThreshold) {
    GenPackedSwitchWithCompares(value_reg,
                                lower_bound,
                                num_entries,
                                switch_instr->GetBlock(),
                                default_block);
    return;
  }

  // Optimizing has a jump area.
  Register temp_reg = locations->GetTemp(0).AsRegister<Register>();
  Register constant_area = locations->InAt(1).AsRegister<Register>();

  // Remove the bias, if needed.
  if (lower_bound != 0) {
    __ leal(temp_reg, Address(value_reg, -lower_bound));
    value_reg = temp_reg;
  }

  // Is the value in range?
  DCHECK_GE(num_entries, 1u);
  __ cmpl(value_reg, Immediate(num_entries - 1));
  __ j(kAbove, codegen_->GetLabelOf(default_block));

  // We are in the range of the table.
  // Load (target-constant_area) from the jump table, indexing by the value.
  __ movl(temp_reg, codegen_->LiteralCaseTable(switch_instr, constant_area, value_reg));

  // Compute the actual target address by adding in constant_area.
  __ addl(temp_reg, constant_area);

  // And jump.
  __ jmp(temp_reg);
}

void LocationsBuilderX86::VisitX86ComputeBaseMethodAddress(
    HX86ComputeBaseMethodAddress* insn) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(insn, LocationSummary::kNoCall);
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorX86::VisitX86ComputeBaseMethodAddress(
    HX86ComputeBaseMethodAddress* insn) {
  LocationSummary* locations = insn->GetLocations();
  Register reg = locations->Out().AsRegister<Register>();

  // Generate call to next instruction.
  Label next_instruction;
  __ call(&next_instruction);
  __ Bind(&next_instruction);

  // Remember this offset for later use with constant area.
  codegen_->SetMethodAddressOffset(GetAssembler()->CodeSize());

  // Grab the return address off the stack.
  __ popl(reg);
}

void LocationsBuilderX86::VisitX86LoadFromConstantTable(
    HX86LoadFromConstantTable* insn) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(insn, LocationSummary::kNoCall);

  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::ConstantLocation(insn->GetConstant()));

  // If we don't need to be materialized, we only need the inputs to be set.
  if (insn->IsEmittedAtUseSite()) {
    return;
  }

  switch (insn->GetType()) {
    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      locations->SetOut(Location::RequiresFpuRegister());
      break;

    case Primitive::kPrimInt:
      locations->SetOut(Location::RequiresRegister());
      break;

    default:
      LOG(FATAL) << "Unsupported x86 constant area type " << insn->GetType();
  }
}

void InstructionCodeGeneratorX86::VisitX86LoadFromConstantTable(HX86LoadFromConstantTable* insn) {
  if (insn->IsEmittedAtUseSite()) {
    return;
  }

  LocationSummary* locations = insn->GetLocations();
  Location out = locations->Out();
  Register const_area = locations->InAt(0).AsRegister<Register>();
  HConstant *value = insn->GetConstant();

  switch (insn->GetType()) {
    case Primitive::kPrimFloat:
      __ movss(out.AsFpuRegister<XmmRegister>(),
               codegen_->LiteralFloatAddress(value->AsFloatConstant()->GetValue(), const_area));
      break;

    case Primitive::kPrimDouble:
      __ movsd(out.AsFpuRegister<XmmRegister>(),
               codegen_->LiteralDoubleAddress(value->AsDoubleConstant()->GetValue(), const_area));
      break;

    case Primitive::kPrimInt:
      __ movl(out.AsRegister<Register>(),
              codegen_->LiteralInt32Address(value->AsIntConstant()->GetValue(), const_area));
      break;

    default:
      LOG(FATAL) << "Unsupported x86 constant area type " << insn->GetType();
  }
}

/**
 * Class to handle late fixup of offsets into constant area.
 */
class RIPFixup : public AssemblerFixup, public ArenaObject<kArenaAllocCodeGenerator> {
 public:
  RIPFixup(CodeGeneratorX86& codegen, size_t offset)
      : codegen_(&codegen), offset_into_constant_area_(offset) {}

 protected:
  void SetOffset(size_t offset) { offset_into_constant_area_ = offset; }

  CodeGeneratorX86* codegen_;

 private:
  void Process(const MemoryRegion& region, int pos) OVERRIDE {
    // Patch the correct offset for the instruction.  The place to patch is the
    // last 4 bytes of the instruction.
    // The value to patch is the distance from the offset in the constant area
    // from the address computed by the HX86ComputeBaseMethodAddress instruction.
    int32_t constant_offset = codegen_->ConstantAreaStart() + offset_into_constant_area_;
    int32_t relative_position = constant_offset - codegen_->GetMethodAddressOffset();;

    // Patch in the right value.
    region.StoreUnaligned<int32_t>(pos - 4, relative_position);
  }

  // Location in constant area that the fixup refers to.
  int32_t offset_into_constant_area_;
};

/**
 * Class to handle late fixup of offsets to a jump table that will be created in the
 * constant area.
 */
class JumpTableRIPFixup : public RIPFixup {
 public:
  JumpTableRIPFixup(CodeGeneratorX86& codegen, HX86PackedSwitch* switch_instr)
      : RIPFixup(codegen, static_cast<size_t>(-1)), switch_instr_(switch_instr) {}

  void CreateJumpTable() {
    X86Assembler* assembler = codegen_->GetAssembler();

    // Ensure that the reference to the jump table has the correct offset.
    const int32_t offset_in_constant_table = assembler->ConstantAreaSize();
    SetOffset(offset_in_constant_table);

    // The label values in the jump table are computed relative to the
    // instruction addressing the constant area.
    const int32_t relative_offset = codegen_->GetMethodAddressOffset();

    // Populate the jump table with the correct values for the jump table.
    int32_t num_entries = switch_instr_->GetNumEntries();
    HBasicBlock* block = switch_instr_->GetBlock();
    const ArenaVector<HBasicBlock*>& successors = block->GetSuccessors();
    // The value that we want is the target offset - the position of the table.
    for (int32_t i = 0; i < num_entries; i++) {
      HBasicBlock* b = successors[i];
      Label* l = codegen_->GetLabelOf(b);
      DCHECK(l->IsBound());
      int32_t offset_to_block = l->Position() - relative_offset;
      assembler->AppendInt32(offset_to_block);
    }
  }

 private:
  const HX86PackedSwitch* switch_instr_;
};

void CodeGeneratorX86::Finalize(CodeAllocator* allocator) {
  // Generate the constant area if needed.
  X86Assembler* assembler = GetAssembler();
  if (!assembler->IsConstantAreaEmpty() || !fixups_to_jump_tables_.empty()) {
    // Align to 4 byte boundary to reduce cache misses, as the data is 4 and 8
    // byte values.
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

Address CodeGeneratorX86::LiteralDoubleAddress(double v, Register reg) {
  AssemblerFixup* fixup = new (GetGraph()->GetArena()) RIPFixup(*this, __ AddDouble(v));
  return Address(reg, kDummy32BitOffset, fixup);
}

Address CodeGeneratorX86::LiteralFloatAddress(float v, Register reg) {
  AssemblerFixup* fixup = new (GetGraph()->GetArena()) RIPFixup(*this, __ AddFloat(v));
  return Address(reg, kDummy32BitOffset, fixup);
}

Address CodeGeneratorX86::LiteralInt32Address(int32_t v, Register reg) {
  AssemblerFixup* fixup = new (GetGraph()->GetArena()) RIPFixup(*this, __ AddInt32(v));
  return Address(reg, kDummy32BitOffset, fixup);
}

Address CodeGeneratorX86::LiteralInt64Address(int64_t v, Register reg) {
  AssemblerFixup* fixup = new (GetGraph()->GetArena()) RIPFixup(*this, __ AddInt64(v));
  return Address(reg, kDummy32BitOffset, fixup);
}

void CodeGeneratorX86::Load32BitValue(Register dest, int32_t value) {
  if (value == 0) {
    __ xorl(dest, dest);
  } else {
    __ movl(dest, Immediate(value));
  }
}

void CodeGeneratorX86::Compare32BitValue(Register dest, int32_t value) {
  if (value == 0) {
    __ testl(dest, dest);
  } else {
    __ cmpl(dest, Immediate(value));
  }
}

Address CodeGeneratorX86::LiteralCaseTable(HX86PackedSwitch* switch_instr,
                                           Register reg,
                                           Register value) {
  // Create a fixup to be used to create and address the jump table.
  JumpTableRIPFixup* table_fixup =
      new (GetGraph()->GetArena()) JumpTableRIPFixup(*this, switch_instr);

  // We have to populate the jump tables.
  fixups_to_jump_tables_.push_back(table_fixup);

  // We want a scaled address, as we are extracting the correct offset from the table.
  return Address(reg, value, TIMES_4, kDummy32BitOffset, table_fixup);
}

// TODO: target as memory.
void CodeGeneratorX86::MoveFromReturnRegister(Location target, Primitive::Type type) {
  if (!target.IsValid()) {
    DCHECK_EQ(type, Primitive::kPrimVoid);
    return;
  }

  DCHECK_NE(type, Primitive::kPrimVoid);

  Location return_loc = InvokeDexCallingConventionVisitorX86().GetReturnLocation(type);
  if (target.Equals(return_loc)) {
    return;
  }

  // TODO: Consider pairs in the parallel move resolver, then this could be nicely merged
  //       with the else branch.
  if (type == Primitive::kPrimLong) {
    HParallelMove parallel_move(GetGraph()->GetArena());
    parallel_move.AddMove(return_loc.ToLow(), target.ToLow(), Primitive::kPrimInt, nullptr);
    parallel_move.AddMove(return_loc.ToHigh(), target.ToHigh(), Primitive::kPrimInt, nullptr);
    GetMoveResolver()->EmitNativeCode(&parallel_move);
  } else {
    // Let the parallel move resolver take care of all of this.
    HParallelMove parallel_move(GetGraph()->GetArena());
    parallel_move.AddMove(return_loc, target, type, nullptr);
    GetMoveResolver()->EmitNativeCode(&parallel_move);
  }
}

#undef __

}  // namespace x86
}  // namespace art
