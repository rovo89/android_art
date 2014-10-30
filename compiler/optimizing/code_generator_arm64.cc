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

#include "entrypoints/quick/quick_entrypoints.h"
#include "gc/accounting/card_table.h"
#include "mirror/array-inl.h"
#include "mirror/art_method.h"
#include "mirror/class.h"
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

static bool IsFPType(Primitive::Type type) {
  return type == Primitive::kPrimFloat || type == Primitive::kPrimDouble;
}

// TODO: clean-up some of the constant definitions.
static constexpr size_t kHeapRefSize = sizeof(mirror::HeapReference<mirror::Object>);
static constexpr int kCurrentMethodStackOffset = 0;

namespace {
// Convenience helpers to ease conversion to and from VIXL operands.

int VIXLRegCodeFromART(int code) {
  // TODO: static check?
  DCHECK_EQ(SP, 31);
  DCHECK_EQ(WSP, 31);
  DCHECK_EQ(XZR, 32);
  DCHECK_EQ(WZR, 32);
  if (code == SP) {
    return vixl::kSPRegInternalCode;
  }
  if (code == XZR) {
    return vixl::kZeroRegCode;
  }
  return code;
}

int ARTRegCodeFromVIXL(int code) {
  // TODO: static check?
  DCHECK_EQ(SP, 31);
  DCHECK_EQ(WSP, 31);
  DCHECK_EQ(XZR, 32);
  DCHECK_EQ(WZR, 32);
  if (code == vixl::kSPRegInternalCode) {
    return SP;
  }
  if (code == vixl::kZeroRegCode) {
    return XZR;
  }
  return code;
}

Register XRegisterFrom(Location location) {
  return Register::XRegFromCode(VIXLRegCodeFromART(location.reg()));
}

Register WRegisterFrom(Location location) {
  return Register::WRegFromCode(VIXLRegCodeFromART(location.reg()));
}

Register RegisterFrom(Location location, Primitive::Type type) {
  DCHECK(type != Primitive::kPrimVoid && !IsFPType(type));
  return type == Primitive::kPrimLong ? XRegisterFrom(location) : WRegisterFrom(location);
}

Register OutputRegister(HInstruction* instr) {
  return RegisterFrom(instr->GetLocations()->Out(), instr->GetType());
}

Register InputRegisterAt(HInstruction* instr, int input_index) {
  return RegisterFrom(instr->GetLocations()->InAt(input_index),
                      instr->InputAt(input_index)->GetType());
}

int64_t Int64ConstantFrom(Location location) {
  HConstant* instr = location.GetConstant();
  return instr->IsIntConstant() ? instr->AsIntConstant()->GetValue()
                                : instr->AsLongConstant()->GetValue();
}

Operand OperandFrom(Location location, Primitive::Type type) {
  if (location.IsRegister()) {
    return Operand(RegisterFrom(location, type));
  } else {
    return Operand(Int64ConstantFrom(location));
  }
}

Operand InputOperandAt(HInstruction* instr, int input_index) {
  return OperandFrom(instr->GetLocations()->InAt(input_index),
                     instr->InputAt(input_index)->GetType());
}

MemOperand StackOperandFrom(Location location) {
  return MemOperand(sp, location.GetStackIndex());
}

MemOperand HeapOperand(const Register& base, Offset offset) {
  // A heap reference must be 32bit, so fit in a W register.
  DCHECK(base.IsW());
  return MemOperand(base.X(), offset.SizeValue());
}

MemOperand HeapOperandFrom(Location location, Primitive::Type type, Offset offset) {
  return HeapOperand(RegisterFrom(location, type), offset);
}

Location LocationFrom(const Register& reg) {
  return Location::RegisterLocation(ARTRegCodeFromVIXL(reg.code()));
}

}  // namespace

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

static const Register kRuntimeParameterCoreRegisters[] = { x0, x1, x2, x3, x4, x5, x6, x7 };
static constexpr size_t kRuntimeParameterCoreRegistersLength =
    arraysize(kRuntimeParameterCoreRegisters);
static const FPRegister kRuntimeParameterFpuRegisters[] = { };
static constexpr size_t kRuntimeParameterFpuRegistersLength = 0;

class InvokeRuntimeCallingConvention : public CallingConvention<Register, FPRegister> {
 public:
  static constexpr size_t kParameterCoreRegistersLength = arraysize(kParameterCoreRegisters);

  InvokeRuntimeCallingConvention()
      : CallingConvention(kRuntimeParameterCoreRegisters,
                          kRuntimeParameterCoreRegistersLength,
                          kRuntimeParameterFpuRegisters,
                          kRuntimeParameterFpuRegistersLength) {}

  Location GetReturnLocation(Primitive::Type return_type);

 private:
  DISALLOW_COPY_AND_ASSIGN(InvokeRuntimeCallingConvention);
};

Location InvokeRuntimeCallingConvention::GetReturnLocation(Primitive::Type return_type) {
  DCHECK_NE(return_type, Primitive::kPrimVoid);
  if (return_type == Primitive::kPrimFloat || return_type == Primitive::kPrimDouble) {
    LOG(FATAL) << "Unimplemented return type " << return_type;
  }
  return LocationFrom(x0);
}

#define __ reinterpret_cast<Arm64Assembler*>(codegen->GetAssembler())->vixl_masm_->

class SlowPathCodeARM64 : public SlowPathCode {
 public:
  SlowPathCodeARM64() : entry_label_(), exit_label_() {}

  vixl::Label* GetEntryLabel() { return &entry_label_; }
  vixl::Label* GetExitLabel() { return &exit_label_; }

 private:
  vixl::Label entry_label_;
  vixl::Label exit_label_;

  DISALLOW_COPY_AND_ASSIGN(SlowPathCodeARM64);
};

class BoundsCheckSlowPathARM64 : public SlowPathCodeARM64 {
 public:
  explicit BoundsCheckSlowPathARM64(HBoundsCheck* instruction,
                                    Location index_location,
                                    Location length_location)
      : instruction_(instruction),
        index_location_(index_location),
        length_location_(length_location) {}

  virtual void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    CodeGeneratorARM64* arm64_codegen = reinterpret_cast<CodeGeneratorARM64*>(codegen);
    __ Bind(GetEntryLabel());
    InvokeRuntimeCallingConvention calling_convention;
    arm64_codegen->MoveHelper(LocationFrom(calling_convention.GetRegisterAt(0)),
                              index_location_, Primitive::kPrimInt);
    arm64_codegen->MoveHelper(LocationFrom(calling_convention.GetRegisterAt(1)),
                              length_location_, Primitive::kPrimInt);
    size_t offset = QUICK_ENTRYPOINT_OFFSET(kArm64WordSize, pThrowArrayBounds).SizeValue();
    __ Ldr(lr, MemOperand(tr, offset));
    __ Blr(lr);
    codegen->RecordPcInfo(instruction_, instruction_->GetDexPc());
  }

 private:
  HBoundsCheck* const instruction_;
  const Location index_location_;
  const Location length_location_;

  DISALLOW_COPY_AND_ASSIGN(BoundsCheckSlowPathARM64);
};

class NullCheckSlowPathARM64 : public SlowPathCodeARM64 {
 public:
  explicit NullCheckSlowPathARM64(HNullCheck* instr) : instruction_(instr) {}

  virtual void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    __ Bind(GetEntryLabel());
    int32_t offset = QUICK_ENTRYPOINT_OFFSET(kArm64WordSize, pThrowNullPointer).Int32Value();
    __ Ldr(lr, MemOperand(tr, offset));
    __ Blr(lr);
    codegen->RecordPcInfo(instruction_, instruction_->GetDexPc());
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

  virtual void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    size_t offset = QUICK_ENTRYPOINT_OFFSET(kArm64WordSize, pTestSuspend).SizeValue();
    __ Bind(GetEntryLabel());
    __ Ldr(lr, MemOperand(tr, offset));
    __ Blr(lr);
    codegen->RecordPcInfo(instruction_, instruction_->GetDexPc());
    __ B(GetReturnLabel());
  }

  vixl::Label* GetReturnLabel() {
    DCHECK(successor_ == nullptr);
    return &return_label_;
  }


 private:
  HSuspendCheck* const instruction_;
  // If not null, the block to branch to after the suspend check.
  HBasicBlock* const successor_;

  // If `successor_` is null, the label to branch to after the suspend check.
  vixl::Label return_label_;

  DISALLOW_COPY_AND_ASSIGN(SuspendCheckSlowPathARM64);
};

#undef __

Location InvokeDexCallingConventionVisitor::GetNextLocation(Primitive::Type type) {
  Location next_location;
  if (type == Primitive::kPrimVoid) {
    LOG(FATAL) << "Unreachable type " << type;
  }

  if (type == Primitive::kPrimFloat || type == Primitive::kPrimDouble) {
    LOG(FATAL) << "Unimplemented type " << type;
  }

  if (gp_index_ < calling_convention.GetNumberOfRegisters()) {
    next_location = LocationFrom(calling_convention.GetRegisterAt(gp_index_));
    if (type == Primitive::kPrimLong) {
      // Double stack slot reserved on the stack.
      stack_index_++;
    }
  } else {  // Stack.
    if (type == Primitive::kPrimLong) {
      next_location = Location::DoubleStackSlot(calling_convention.GetStackOffsetOf(stack_index_));
      // Double stack slot reserved on the stack.
      stack_index_++;
    } else {
      next_location = Location::StackSlot(calling_convention.GetStackOffsetOf(stack_index_));
    }
  }
  // Move to the next register/stack slot.
  gp_index_++;
  stack_index_++;
  return next_location;
}

CodeGeneratorARM64::CodeGeneratorARM64(HGraph* graph)
    : CodeGenerator(graph,
                    kNumberOfAllocatableRegisters,
                    kNumberOfAllocatableFloatingPointRegisters,
                    kNumberOfAllocatableRegisterPairs),
      block_labels_(nullptr),
      location_builder_(graph, this),
      instruction_visitor_(graph, this) {}

#define __ reinterpret_cast<Arm64Assembler*>(GetAssembler())->vixl_masm_->

void CodeGeneratorARM64::GenerateFrameEntry() {
  // TODO: Add proper support for the stack overflow check.
  UseScratchRegisterScope temps(assembler_.vixl_masm_);
  Register temp = temps.AcquireX();
  __ Add(temp, sp, -static_cast<int32_t>(GetStackOverflowReservedBytes(kArm64)));
  __ Ldr(temp, MemOperand(temp, 0));
  RecordPcInfo(nullptr, 0);

  CPURegList preserved_regs = GetFramePreservedRegisters();
  int frame_size = GetFrameSize();
  core_spill_mask_ |= preserved_regs.list();

  __ Str(w0, MemOperand(sp, -frame_size, PreIndex));
  __ PokeCPURegList(preserved_regs, frame_size - preserved_regs.TotalSizeInBytes());

  // Stack layout:
  // sp[frame_size - 8]        : lr.
  // ...                       : other preserved registers.
  // sp[frame_size - regs_size]: first preserved register.
  // ...                       : reserved frame space.
  // sp[0]                     : context pointer.
}

void CodeGeneratorARM64::GenerateFrameExit() {
  int frame_size = GetFrameSize();
  CPURegList preserved_regs = GetFramePreservedRegisters();
  __ PeekCPURegList(preserved_regs, frame_size - preserved_regs.TotalSizeInBytes());
  __ Drop(frame_size);
}

void CodeGeneratorARM64::Bind(HBasicBlock* block) {
  __ Bind(GetLabelOf(block));
}

void CodeGeneratorARM64::MoveHelper(Location destination,
                                    Location source,
                                    Primitive::Type type) {
  if (source.Equals(destination)) {
    return;
  }
  if (destination.IsRegister()) {
    Register dst = RegisterFrom(destination, type);
    if (source.IsRegister()) {
      Register src = RegisterFrom(source, type);
      DCHECK(dst.IsSameSizeAndType(src));
      __ Mov(dst, src);
    } else {
      DCHECK(dst.Is64Bits() || !source.IsDoubleStackSlot());
      __ Ldr(dst, StackOperandFrom(source));
    }
  } else {
    DCHECK(destination.IsStackSlot() || destination.IsDoubleStackSlot());
    if (source.IsRegister()) {
      __ Str(RegisterFrom(source, type), StackOperandFrom(destination));
    } else {
      UseScratchRegisterScope temps(assembler_.vixl_masm_);
      Register temp = destination.IsDoubleStackSlot() ? temps.AcquireX() : temps.AcquireW();
      __ Ldr(temp, StackOperandFrom(source));
      __ Str(temp, StackOperandFrom(destination));
    }
  }
}

void CodeGeneratorARM64::Move(HInstruction* instruction,
                              Location location,
                              HInstruction* move_for) {
  LocationSummary* locations = instruction->GetLocations();
  if (locations != nullptr && locations->Out().Equals(location)) {
    return;
  }

  Primitive::Type type = instruction->GetType();

  if (instruction->IsIntConstant() || instruction->IsLongConstant()) {
    int64_t value = instruction->IsIntConstant() ? instruction->AsIntConstant()->GetValue()
                                                 : instruction->AsLongConstant()->GetValue();
    if (location.IsRegister()) {
      Register dst = RegisterFrom(location, type);
      DCHECK((instruction->IsIntConstant() && dst.Is32Bits()) ||
             (instruction->IsLongConstant() && dst.Is64Bits()));
      __ Mov(dst, value);
    } else {
      DCHECK(location.IsStackSlot() || location.IsDoubleStackSlot());
      UseScratchRegisterScope temps(assembler_.vixl_masm_);
      Register temp = instruction->IsIntConstant() ? temps.AcquireW() : temps.AcquireX();
      __ Mov(temp, value);
      __ Str(temp, StackOperandFrom(location));
    }

  } else if (instruction->IsLoadLocal()) {
    uint32_t stack_slot = GetStackSlot(instruction->AsLoadLocal()->GetLocal());
    switch (type) {
      case Primitive::kPrimNot:
      case Primitive::kPrimBoolean:
      case Primitive::kPrimByte:
      case Primitive::kPrimChar:
      case Primitive::kPrimShort:
      case Primitive::kPrimInt:
        MoveHelper(location, Location::StackSlot(stack_slot), type);
        break;
      case Primitive::kPrimLong:
        MoveHelper(location, Location::DoubleStackSlot(stack_slot), type);
        break;
      default:
        LOG(FATAL) << "Unimplemented type" << type;
    }

  } else {
    DCHECK((instruction->GetNext() == move_for) || instruction->GetNext()->IsTemporary());
    MoveHelper(location, locations->Out(), type);
  }
}

size_t CodeGeneratorARM64::FrameEntrySpillSize() const {
  return GetFramePreservedRegistersSize();
}

Location CodeGeneratorARM64::GetStackLocation(HLoadLocal* load) const {
  Primitive::Type type = load->GetType();
  switch (type) {
    case Primitive::kPrimNot:
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimInt:
      return Location::StackSlot(GetStackSlot(load->GetLocal()));
    case Primitive::kPrimLong:
      return Location::DoubleStackSlot(GetStackSlot(load->GetLocal()));
    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      LOG(FATAL) << "Unimplemented type " << type;
      break;
    case Primitive::kPrimVoid:
    default:
      LOG(FATAL) << "Unexpected type " << type;
  }
  LOG(FATAL) << "Unreachable";
  return Location::NoLocation();
}

void CodeGeneratorARM64::MarkGCCard(Register object, Register value) {
  UseScratchRegisterScope temps(assembler_.vixl_masm_);
  Register card = temps.AcquireX();
  Register temp = temps.AcquireX();
  vixl::Label done;
  __ Cbz(value, &done);
  __ Ldr(card, MemOperand(tr, Thread::CardTableOffset<kArm64WordSize>().Int32Value()));
  __ Lsr(temp, object, gc::accounting::CardTable::kCardShift);
  __ Strb(card, MemOperand(card, temp));
  __ Bind(&done);
}

void CodeGeneratorARM64::SetupBlockedRegisters() const {
  // Block reserved registers:
  //   ip0 (VIXL temporary)
  //   ip1 (VIXL temporary)
  //   xSuspend (Suspend counter)
  //   lr
  // sp is not part of the allocatable registers, so we don't need to block it.
  CPURegList reserved_core_registers = vixl_reserved_core_registers;
  reserved_core_registers.Combine(runtime_reserved_core_registers);
  // TODO: See if we should instead allow allocating but preserve those if used.
  reserved_core_registers.Combine(quick_callee_saved_registers);
  while (!reserved_core_registers.IsEmpty()) {
    blocked_core_registers_[reserved_core_registers.PopLowestIndex().code()] = true;
  }
}

Location CodeGeneratorARM64::AllocateFreeRegister(Primitive::Type type) const {
  if (type == Primitive::kPrimVoid) {
    LOG(FATAL) << "Unreachable type " << type;
  }

  if (type == Primitive::kPrimFloat || type == Primitive::kPrimDouble) {
    LOG(FATAL) << "Unimplemented support for floating-point";
  }

  ssize_t reg = FindFreeEntry(blocked_core_registers_, kNumberOfXRegisters);
  DCHECK_NE(reg, -1);
  blocked_core_registers_[reg] = true;

  if (IsFPType(type)) {
    return Location::FpuRegisterLocation(reg);
  } else {
    return Location::RegisterLocation(reg);
  }
}

void CodeGeneratorARM64::DumpCoreRegister(std::ostream& stream, int reg) const {
  stream << Arm64ManagedRegister::FromXRegister(XRegister(reg));
}

void CodeGeneratorARM64::DumpFloatingPointRegister(std::ostream& stream, int reg) const {
  stream << Arm64ManagedRegister::FromDRegister(DRegister(reg));
}

#undef __
#define __ assembler_->vixl_masm_->

InstructionCodeGeneratorARM64::InstructionCodeGeneratorARM64(HGraph* graph,
                                                             CodeGeneratorARM64* codegen)
      : HGraphVisitor(graph),
        assembler_(codegen->GetAssembler()),
        codegen_(codegen) {}

#define FOR_EACH_UNIMPLEMENTED_INSTRUCTION(M)              \
  M(ArrayGet)                                              \
  M(ArraySet)                                              \
  M(ClinitCheck)                                           \
  M(DoubleConstant)                                        \
  M(Div)                                                   \
  M(FloatConstant)                                         \
  M(LoadClass)                                             \
  M(Neg)                                                   \
  M(NewArray)                                              \
  M(ParallelMove)                                          \
  M(StaticFieldGet)                                        \
  M(StaticFieldSet)                                        \

#define UNIMPLEMENTED_INSTRUCTION_BREAK_CODE(name) name##UnimplementedInstructionBreakCode

enum UnimplementedInstructionBreakCode {
#define ENUM_UNIMPLEMENTED_INSTRUCTION(name) UNIMPLEMENTED_INSTRUCTION_BREAK_CODE(name),
  FOR_EACH_UNIMPLEMENTED_INSTRUCTION(ENUM_UNIMPLEMENTED_INSTRUCTION)
#undef ENUM_UNIMPLEMENTED_INSTRUCTION
};

#define DEFINE_UNIMPLEMENTED_INSTRUCTION_VISITORS(name)                               \
  void InstructionCodeGeneratorARM64::Visit##name(H##name* instr) {                   \
    __ Brk(UNIMPLEMENTED_INSTRUCTION_BREAK_CODE(name));                               \
  }                                                                                   \
  void LocationsBuilderARM64::Visit##name(H##name* instr) {                           \
    LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instr); \
    locations->SetOut(Location::Any());                                               \
  }
  FOR_EACH_UNIMPLEMENTED_INSTRUCTION(DEFINE_UNIMPLEMENTED_INSTRUCTION_VISITORS)
#undef DEFINE_UNIMPLEMENTED_INSTRUCTION_VISITORS

#undef UNIMPLEMENTED_INSTRUCTION_BREAK_CODE

void LocationsBuilderARM64::HandleAddSub(HBinaryOperation* instr) {
  DCHECK(instr->IsAdd() || instr->IsSub());
  DCHECK_EQ(instr->InputCount(), 2U);
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
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
      LOG(FATAL) << "Unexpected " << instr->DebugName() <<  " type " << type;
      break;
    default:
      LOG(FATAL) << "Unimplemented " << instr->DebugName() << " type " << type;
  }
}

void InstructionCodeGeneratorARM64::HandleAddSub(HBinaryOperation* instr) {
  DCHECK(instr->IsAdd() || instr->IsSub());

  Primitive::Type type = instr->GetType();
  Register dst = OutputRegister(instr);
  Register lhs = InputRegisterAt(instr, 0);
  Operand rhs = InputOperandAt(instr, 1);

  switch (type) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong:
      if (instr->IsAdd()) {
        __ Add(dst, lhs, rhs);
      } else {
        __ Sub(dst, lhs, rhs);
      }
      break;

    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
      LOG(FATAL) << "Unexpected add/sub type " << type;
      break;
    default:
      LOG(FATAL) << "Unimplemented add/sub type " << type;
  }
}

void LocationsBuilderARM64::VisitAdd(HAdd* instruction) {
  HandleAddSub(instruction);
}

void InstructionCodeGeneratorARM64::VisitAdd(HAdd* instruction) {
  HandleAddSub(instruction);
}

void LocationsBuilderARM64::VisitArrayLength(HArrayLength* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorARM64::VisitArrayLength(HArrayLength* instruction) {
  __ Ldr(OutputRegister(instruction),
         HeapOperand(InputRegisterAt(instruction, 0), mirror::Array::LengthOffset()));
}

void LocationsBuilderARM64::VisitCompare(HCompare* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RegisterOrConstant(instruction->InputAt(1)));
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorARM64::VisitCompare(HCompare* instruction) {
  Primitive::Type in_type = instruction->InputAt(0)->GetType();

  DCHECK_EQ(in_type, Primitive::kPrimLong);
  switch (in_type) {
    case Primitive::kPrimLong: {
      vixl::Label done;
      Register result = OutputRegister(instruction);
      Register left = InputRegisterAt(instruction, 0);
      Operand right = InputOperandAt(instruction, 1);
      __ Subs(result, left, right);
      __ B(eq, &done);
      __ Mov(result, 1);
      __ Cneg(result, result, le);
      __ Bind(&done);
      break;
    }
    default:
      LOG(FATAL) << "Unimplemented compare type " << in_type;
  }
}

void LocationsBuilderARM64::VisitCondition(HCondition* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RegisterOrConstant(instruction->InputAt(1)));
  if (instruction->NeedsMaterialization()) {
    locations->SetOut(Location::RequiresRegister());
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
  __ Csel(res, vixl::Assembler::AppropriateZeroRegFor(res), Operand(1), InvertCondition(cond));
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
#undef FOR_EACH_CONDITION_INSTRUCTION

void LocationsBuilderARM64::VisitExit(HExit* exit) {
  exit->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM64::VisitExit(HExit* exit) {
  if (kIsDebugBuild) {
    down_cast<Arm64Assembler*>(GetAssembler())->Comment("Unreachable");
    __ Brk(0);    // TODO: Introduce special markers for such code locations.
  }
}

void LocationsBuilderARM64::VisitGoto(HGoto* got) {
  got->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM64::VisitGoto(HGoto* got) {
  HBasicBlock* successor = got->GetSuccessor();
  // TODO: Support for suspend checks emission.
  if (!codegen_->GoesToNextBlock(got->GetBlock(), successor)) {
    __ B(codegen_->GetLabelOf(successor));
  }
}

void LocationsBuilderARM64::VisitIf(HIf* if_instr) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(if_instr);
  HInstruction* cond = if_instr->InputAt(0);
  DCHECK(cond->IsCondition());
  if (cond->AsCondition()->NeedsMaterialization()) {
    locations->SetInAt(0, Location::RequiresRegister());
  }
}

void InstructionCodeGeneratorARM64::VisitIf(HIf* if_instr) {
  HInstruction* cond = if_instr->InputAt(0);
  DCHECK(cond->IsCondition());
  HCondition* condition = cond->AsCondition();
  vixl::Label* true_target = codegen_->GetLabelOf(if_instr->IfTrueSuccessor());
  vixl::Label* false_target = codegen_->GetLabelOf(if_instr->IfFalseSuccessor());

  // TODO: Support constant condition input in VisitIf.

  if (condition->NeedsMaterialization()) {
    // The condition instruction has been materialized, compare the output to 0.
    Location cond_val = if_instr->GetLocations()->InAt(0);
    DCHECK(cond_val.IsRegister());
    __ Cbnz(InputRegisterAt(if_instr, 0), true_target);

  } else {
    // The condition instruction has not been materialized, use its inputs as
    // the comparison and its condition as the branch condition.
    Register lhs = InputRegisterAt(condition, 0);
    Operand rhs = InputOperandAt(condition, 1);
    Condition cond = ARM64Condition(condition->GetCondition());
    if ((cond == eq || cond == ne) && rhs.IsImmediate() && (rhs.immediate() == 0)) {
      if (cond == eq) {
        __ Cbz(lhs, true_target);
      } else {
        __ Cbnz(lhs, true_target);
      }
    } else {
      __ Cmp(lhs, rhs);
      __ B(cond, true_target);
    }
  }

  if (!codegen_->GoesToNextBlock(if_instr->GetBlock(), if_instr->IfFalseSuccessor())) {
    __ B(false_target);
  }
}

void LocationsBuilderARM64::VisitInstanceFieldGet(HInstanceFieldGet* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorARM64::VisitInstanceFieldGet(HInstanceFieldGet* instruction) {
  Primitive::Type res_type = instruction->GetType();
  Register res = OutputRegister(instruction);
  Register obj = InputRegisterAt(instruction, 0);
  uint32_t offset = instruction->GetFieldOffset().Uint32Value();

  switch (res_type) {
    case Primitive::kPrimBoolean: {
      __ Ldrb(res, MemOperand(obj, offset));
      break;
    }
    case Primitive::kPrimByte: {
      __ Ldrsb(res, MemOperand(obj, offset));
      break;
    }
    case Primitive::kPrimShort: {
      __ Ldrsh(res, MemOperand(obj, offset));
      break;
    }
    case Primitive::kPrimChar: {
      __ Ldrh(res, MemOperand(obj, offset));
      break;
    }
    case Primitive::kPrimInt:
    case Primitive::kPrimNot:
    case Primitive::kPrimLong: {  // TODO: support volatile.
      DCHECK(res.IsX() == (res_type == Primitive::kPrimLong));
      __ Ldr(res, MemOperand(obj, offset));
      break;
    }

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      LOG(FATAL) << "Unimplemented register res_type " << res_type;
      break;

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable res_type " << res_type;
  }
}

void LocationsBuilderARM64::VisitInstanceFieldSet(HInstanceFieldSet* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
}

void InstructionCodeGeneratorARM64::VisitInstanceFieldSet(HInstanceFieldSet* instruction) {
  Register obj = InputRegisterAt(instruction, 0);
  Register value = InputRegisterAt(instruction, 1);
  Primitive::Type field_type = instruction->InputAt(1)->GetType();
  uint32_t offset = instruction->GetFieldOffset().Uint32Value();

  switch (field_type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte: {
      __ Strb(value, MemOperand(obj, offset));
      break;
    }

    case Primitive::kPrimShort:
    case Primitive::kPrimChar: {
      __ Strh(value, MemOperand(obj, offset));
      break;
    }

    case Primitive::kPrimInt:
    case Primitive::kPrimNot:
    case Primitive::kPrimLong: {
      DCHECK(value.IsX() == (field_type == Primitive::kPrimLong));
      __ Str(value, MemOperand(obj, offset));

      if (field_type == Primitive::kPrimNot) {
        codegen_->MarkGCCard(obj, value);
      }
      break;
    }

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      LOG(FATAL) << "Unimplemented register type " << field_type;
      break;

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << field_type;
  }
}

void LocationsBuilderARM64::VisitIntConstant(HIntConstant* constant) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(constant);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorARM64::VisitIntConstant(HIntConstant* constant) {
  // Will be generated at use site.
}

void LocationsBuilderARM64::VisitInvokeStatic(HInvokeStatic* invoke) {
  HandleInvoke(invoke);
}

void LocationsBuilderARM64::VisitInvokeVirtual(HInvokeVirtual* invoke) {
  HandleInvoke(invoke);
}

void LocationsBuilderARM64::HandleInvoke(HInvoke* invoke) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(invoke, LocationSummary::kCall);
  locations->AddTemp(LocationFrom(x0));

  InvokeDexCallingConventionVisitor calling_convention_visitor;
  for (size_t i = 0; i < invoke->InputCount(); i++) {
    HInstruction* input = invoke->InputAt(i);
    locations->SetInAt(i, calling_convention_visitor.GetNextLocation(input->GetType()));
  }

  Primitive::Type return_type = invoke->GetType();
  if (return_type != Primitive::kPrimVoid) {
    locations->SetOut(calling_convention_visitor.GetReturnLocation(return_type));
  }
}

void InstructionCodeGeneratorARM64::VisitInvokeStatic(HInvokeStatic* invoke) {
  Register temp = WRegisterFrom(invoke->GetLocations()->GetTemp(0));
  // Make sure that ArtMethod* is passed in W0 as per the calling convention
  DCHECK(temp.Is(w0));
  size_t index_in_cache = mirror::Array::DataOffset(kHeapRefSize).SizeValue() +
    invoke->GetIndexInDexCache() * kHeapRefSize;

  // TODO: Implement all kinds of calls:
  // 1) boot -> boot
  // 2) app -> boot
  // 3) app -> app
  //
  // Currently we implement the app -> app logic, which looks up in the resolve cache.

  // temp = method;
  __ Ldr(temp, MemOperand(sp, kCurrentMethodStackOffset));
  // temp = temp->dex_cache_resolved_methods_;
  __ Ldr(temp, MemOperand(temp.X(), mirror::ArtMethod::DexCacheResolvedMethodsOffset().SizeValue()));
  // temp = temp[index_in_cache];
  __ Ldr(temp, MemOperand(temp.X(), index_in_cache));
  // lr = temp->entry_point_from_quick_compiled_code_;
  __ Ldr(lr, MemOperand(temp.X(), mirror::ArtMethod::EntryPointFromQuickCompiledCodeOffset().SizeValue()));
  // lr();
  __ Blr(lr);

  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
  DCHECK(!codegen_->IsLeafMethod());
}

void InstructionCodeGeneratorARM64::VisitInvokeVirtual(HInvokeVirtual* invoke) {
  LocationSummary* locations = invoke->GetLocations();
  Location receiver = locations->InAt(0);
  Register temp = XRegisterFrom(invoke->GetLocations()->GetTemp(0));
  size_t method_offset = mirror::Class::EmbeddedVTableOffset().SizeValue() +
    invoke->GetVTableIndex() * sizeof(mirror::Class::VTableEntry);
  Offset class_offset = mirror::Object::ClassOffset();
  Offset entry_point = mirror::ArtMethod::EntryPointFromQuickCompiledCodeOffset();

  // temp = object->GetClass();
  if (receiver.IsStackSlot()) {
    __ Ldr(temp.W(), MemOperand(sp, receiver.GetStackIndex()));
    __ Ldr(temp.W(), MemOperand(temp, class_offset.SizeValue()));
  } else {
    DCHECK(receiver.IsRegister());
    __ Ldr(temp.W(), HeapOperandFrom(receiver, Primitive::kPrimNot,
                                     class_offset));
  }
  // temp = temp->GetMethodAt(method_offset);
  __ Ldr(temp.W(), MemOperand(temp, method_offset));
  // lr = temp->GetEntryPoint();
  __ Ldr(lr, MemOperand(temp, entry_point.SizeValue()));
  // lr();
  __ Blr(lr);
  DCHECK(!codegen_->IsLeafMethod());
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
}

void LocationsBuilderARM64::VisitLoadLocal(HLoadLocal* load) {
  load->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM64::VisitLoadLocal(HLoadLocal* load) {
  // Nothing to do, this is driven by the code generator.
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
}

void LocationsBuilderARM64::VisitMul(HMul* mul) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(mul, LocationSummary::kNoCall);
  switch (mul->GetResultType()) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong:
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RequiresRegister());
      locations->SetOut(Location::RequiresRegister());
      break;

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      LOG(FATAL) << "Unimplemented mul type " << mul->GetResultType();
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
      LOG(FATAL) << "Unimplemented mul type " << mul->GetResultType();
      break;

    default:
      LOG(FATAL) << "Unexpected mul type " << mul->GetResultType();
  }
}

void LocationsBuilderARM64::VisitNewInstance(HNewInstance* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCall);
  InvokeRuntimeCallingConvention calling_convention;
  locations->AddTemp(LocationFrom(calling_convention.GetRegisterAt(0)));
  locations->AddTemp(LocationFrom(calling_convention.GetRegisterAt(1)));
  locations->SetOut(calling_convention.GetReturnLocation(Primitive::kPrimNot));
}

void InstructionCodeGeneratorARM64::VisitNewInstance(HNewInstance* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Register type_index = RegisterFrom(locations->GetTemp(0), Primitive::kPrimInt);
  DCHECK(type_index.Is(w0));
  Register current_method = RegisterFrom(locations->GetTemp(1), Primitive::kPrimNot);
  DCHECK(current_method.Is(w1));
  __ Ldr(current_method, MemOperand(sp, kCurrentMethodStackOffset));
  __ Mov(type_index, instruction->GetTypeIndex());
  __ Ldr(lr, MemOperand(tr, QUICK_ENTRYPOINT_OFFSET(kArm64WordSize, pAllocObjectWithAccessCheck).Int32Value()));
  __ Blr(lr);
  codegen_->RecordPcInfo(instruction, instruction->GetDexPc());
  DCHECK(!codegen_->IsLeafMethod());
}

void LocationsBuilderARM64::VisitNot(HNot* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorARM64::VisitNot(HNot* instruction) {
  switch (instruction->InputAt(0)->GetType()) {
    case Primitive::kPrimBoolean:
      __ Eor(OutputRegister(instruction), InputRegisterAt(instruction, 0), Operand(1));
      break;

    case Primitive::kPrimInt:
      __ Mvn(OutputRegister(instruction), InputOperandAt(instruction, 0));
      break;

    case Primitive::kPrimLong:
      LOG(FATAL) << "Not yet implemented type for not operation " << instruction->GetResultType();
      break;

    default:
      LOG(FATAL) << "Unexpected type for not operation " << instruction->GetResultType();
  }
}

void LocationsBuilderARM64::VisitNullCheck(HNullCheck* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  if (instruction->HasUses()) {
    locations->SetOut(Location::SameAsFirstInput());
  }
}

void InstructionCodeGeneratorARM64::VisitNullCheck(HNullCheck* instruction) {
  SlowPathCodeARM64* slow_path = new (GetGraph()->GetArena()) NullCheckSlowPathARM64(instruction);
  codegen_->AddSlowPath(slow_path);

  LocationSummary* locations = instruction->GetLocations();
  Location obj = locations->InAt(0);
  if (obj.IsRegister()) {
    __ Cbz(RegisterFrom(obj, instruction->InputAt(0)->GetType()), slow_path->GetEntryLabel());
  } else {
    DCHECK(obj.IsConstant()) << obj;
    DCHECK_EQ(obj.GetConstant()->AsIntConstant()->GetValue(), 0);
    __ B(slow_path->GetEntryLabel());
  }
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
}

void LocationsBuilderARM64::VisitPhi(HPhi* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  for (size_t i = 0, e = instruction->InputCount(); i < e; ++i) {
    locations->SetInAt(i, Location::Any());
  }
  locations->SetOut(Location::Any());
}

void InstructionCodeGeneratorARM64::VisitPhi(HPhi* instruction) {
  LOG(FATAL) << "Unreachable";
}

void LocationsBuilderARM64::VisitReturn(HReturn* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction);
  Primitive::Type return_type = instruction->InputAt(0)->GetType();

  if (return_type == Primitive::kPrimFloat || return_type == Primitive::kPrimDouble) {
    LOG(FATAL) << "Unimplemented return type " << return_type;
  }

  locations->SetInAt(0, LocationFrom(x0));
}

void InstructionCodeGeneratorARM64::VisitReturn(HReturn* instruction) {
  if (kIsDebugBuild) {
    Primitive::Type type = instruction->InputAt(0)->GetType();
    switch (type) {
      case Primitive::kPrimBoolean:
      case Primitive::kPrimByte:
      case Primitive::kPrimChar:
      case Primitive::kPrimShort:
      case Primitive::kPrimInt:
      case Primitive::kPrimNot:
        DCHECK(InputRegisterAt(instruction, 0).Is(w0));
        break;

      case Primitive::kPrimLong:
        DCHECK(InputRegisterAt(instruction, 0).Is(x0));
        break;

      default:
        LOG(FATAL) << "Unimplemented return type " << type;
    }
  }
  codegen_->GenerateFrameExit();
  __ Br(lr);
}

void LocationsBuilderARM64::VisitReturnVoid(HReturnVoid* instruction) {
  instruction->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM64::VisitReturnVoid(HReturnVoid* instruction) {
  codegen_->GenerateFrameExit();
  __ Br(lr);
}

void LocationsBuilderARM64::VisitStoreLocal(HStoreLocal* store) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(store);
  Primitive::Type field_type = store->InputAt(1)->GetType();
  switch (field_type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimInt:
    case Primitive::kPrimNot:
      locations->SetInAt(1, Location::StackSlot(codegen_->GetStackSlot(store->GetLocal())));
      break;

    case Primitive::kPrimLong:
      locations->SetInAt(1, Location::DoubleStackSlot(codegen_->GetStackSlot(store->GetLocal())));
      break;

    default:
      LOG(FATAL) << "Unimplemented local type " << field_type;
  }
}

void InstructionCodeGeneratorARM64::VisitStoreLocal(HStoreLocal* store) {
}

void LocationsBuilderARM64::VisitSub(HSub* instruction) {
  HandleAddSub(instruction);
}

void InstructionCodeGeneratorARM64::VisitSub(HSub* instruction) {
  HandleAddSub(instruction);
}

void LocationsBuilderARM64::VisitBoundsCheck(HBoundsCheck* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
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

void LocationsBuilderARM64::VisitSuspendCheck(HSuspendCheck* instruction) {
  new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCallOnSlowPath);
}

void InstructionCodeGeneratorARM64::VisitSuspendCheck(HSuspendCheck* instruction) {
  // TODO: Improve support for suspend checks.
  SuspendCheckSlowPathARM64* slow_path =
      new (GetGraph()->GetArena()) SuspendCheckSlowPathARM64(instruction, nullptr);
  codegen_->AddSlowPath(slow_path);

  __ Subs(wSuspend, wSuspend, 1);
  __ B(slow_path->GetEntryLabel(), le);
  __ Bind(slow_path->GetReturnLabel());
}

void LocationsBuilderARM64::VisitTemporary(HTemporary* temp) {
  temp->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM64::VisitTemporary(HTemporary* temp) {
  // Nothing to do, this is driven by the code generator.
}

}  // namespace arm64
}  // namespace art
