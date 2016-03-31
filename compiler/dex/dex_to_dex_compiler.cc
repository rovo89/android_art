/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include "dex_to_dex_compiler.h"

#include "art_field-inl.h"
#include "art_method-inl.h"
#include "base/logging.h"
#include "base/mutex.h"
#include "compiled_method.h"
#include "dex_file-inl.h"
#include "dex_instruction-inl.h"
#include "driver/compiler_driver.h"
#include "driver/dex_compilation_unit.h"
#include "mirror/class-inl.h"
#include "mirror/dex_cache.h"
#include "thread-inl.h"

namespace art {
namespace optimizer {

// Controls quickening activation.
const bool kEnableQuickening = true;
// Control check-cast elision.
const bool kEnableCheckCastEllision = true;

struct QuickenedInfo {
  QuickenedInfo(uint32_t pc, uint16_t index) : dex_pc(pc), dex_member_index(index) {}

  uint32_t dex_pc;
  uint16_t dex_member_index;
};

class DexCompiler {
 public:
  DexCompiler(art::CompilerDriver& compiler,
              const DexCompilationUnit& unit,
              DexToDexCompilationLevel dex_to_dex_compilation_level)
    : driver_(compiler),
      unit_(unit),
      dex_to_dex_compilation_level_(dex_to_dex_compilation_level) {}

  ~DexCompiler() {}

  void Compile();

  const std::vector<QuickenedInfo>& GetQuickenedInfo() const {
    return quickened_info_;
  }

 private:
  const DexFile& GetDexFile() const {
    return *unit_.GetDexFile();
  }

  bool PerformOptimizations() const {
    return dex_to_dex_compilation_level_ >= DexToDexCompilationLevel::kOptimize;
  }

  // Compiles a RETURN-VOID into a RETURN-VOID-BARRIER within a constructor where
  // a barrier is required.
  void CompileReturnVoid(Instruction* inst, uint32_t dex_pc);

  // Compiles a CHECK-CAST into 2 NOP instructions if it is known to be safe. In
  // this case, returns the second NOP instruction pointer. Otherwise, returns
  // the given "inst".
  Instruction* CompileCheckCast(Instruction* inst, uint32_t dex_pc);

  // Compiles a field access into a quick field access.
  // The field index is replaced by an offset within an Object where we can read
  // from / write to this field. Therefore, this does not involve any resolution
  // at runtime.
  // Since the field index is encoded with 16 bits, we can replace it only if the
  // field offset can be encoded with 16 bits too.
  void CompileInstanceFieldAccess(Instruction* inst, uint32_t dex_pc,
                                  Instruction::Code new_opcode, bool is_put);

  // Compiles a virtual method invocation into a quick virtual method invocation.
  // The method index is replaced by the vtable index where the corresponding
  // AbstractMethod can be found. Therefore, this does not involve any resolution
  // at runtime.
  // Since the method index is encoded with 16 bits, we can replace it only if the
  // vtable index can be encoded with 16 bits too.
  void CompileInvokeVirtual(Instruction* inst, uint32_t dex_pc,
                            Instruction::Code new_opcode, bool is_range);

  CompilerDriver& driver_;
  const DexCompilationUnit& unit_;
  const DexToDexCompilationLevel dex_to_dex_compilation_level_;

  // Filled by the compiler when quickening, in order to encode that information
  // in the .oat file. The runtime will use that information to get to the original
  // opcodes.
  std::vector<QuickenedInfo> quickened_info_;

  DISALLOW_COPY_AND_ASSIGN(DexCompiler);
};

void DexCompiler::Compile() {
  DCHECK_GE(dex_to_dex_compilation_level_, DexToDexCompilationLevel::kRequired);
  const DexFile::CodeItem* code_item = unit_.GetCodeItem();
  const uint16_t* insns = code_item->insns_;
  const uint32_t insns_size = code_item->insns_size_in_code_units_;
  Instruction* inst = const_cast<Instruction*>(Instruction::At(insns));

  for (uint32_t dex_pc = 0; dex_pc < insns_size;
       inst = const_cast<Instruction*>(inst->Next()), dex_pc = inst->GetDexPc(insns)) {
    switch (inst->Opcode()) {
      case Instruction::RETURN_VOID:
        CompileReturnVoid(inst, dex_pc);
        break;

      case Instruction::CHECK_CAST:
        inst = CompileCheckCast(inst, dex_pc);
        break;

      case Instruction::IGET:
        CompileInstanceFieldAccess(inst, dex_pc, Instruction::IGET_QUICK, false);
        break;

      case Instruction::IGET_WIDE:
        CompileInstanceFieldAccess(inst, dex_pc, Instruction::IGET_WIDE_QUICK, false);
        break;

      case Instruction::IGET_OBJECT:
        CompileInstanceFieldAccess(inst, dex_pc, Instruction::IGET_OBJECT_QUICK, false);
        break;

      case Instruction::IGET_BOOLEAN:
        CompileInstanceFieldAccess(inst, dex_pc, Instruction::IGET_BOOLEAN_QUICK, false);
        break;

      case Instruction::IGET_BYTE:
        CompileInstanceFieldAccess(inst, dex_pc, Instruction::IGET_BYTE_QUICK, false);
        break;

      case Instruction::IGET_CHAR:
        CompileInstanceFieldAccess(inst, dex_pc, Instruction::IGET_CHAR_QUICK, false);
        break;

      case Instruction::IGET_SHORT:
        CompileInstanceFieldAccess(inst, dex_pc, Instruction::IGET_SHORT_QUICK, false);
        break;

      case Instruction::IPUT:
        CompileInstanceFieldAccess(inst, dex_pc, Instruction::IPUT_QUICK, true);
        break;

      case Instruction::IPUT_BOOLEAN:
        CompileInstanceFieldAccess(inst, dex_pc, Instruction::IPUT_BOOLEAN_QUICK, true);
        break;

      case Instruction::IPUT_BYTE:
        CompileInstanceFieldAccess(inst, dex_pc, Instruction::IPUT_BYTE_QUICK, true);
        break;

      case Instruction::IPUT_CHAR:
        CompileInstanceFieldAccess(inst, dex_pc, Instruction::IPUT_CHAR_QUICK, true);
        break;

      case Instruction::IPUT_SHORT:
        CompileInstanceFieldAccess(inst, dex_pc, Instruction::IPUT_SHORT_QUICK, true);
        break;

      case Instruction::IPUT_WIDE:
        CompileInstanceFieldAccess(inst, dex_pc, Instruction::IPUT_WIDE_QUICK, true);
        break;

      case Instruction::IPUT_OBJECT:
        CompileInstanceFieldAccess(inst, dex_pc, Instruction::IPUT_OBJECT_QUICK, true);
        break;

      case Instruction::INVOKE_VIRTUAL:
        CompileInvokeVirtual(inst, dex_pc, Instruction::INVOKE_VIRTUAL_QUICK, false);
        break;

      case Instruction::INVOKE_VIRTUAL_RANGE:
        CompileInvokeVirtual(inst, dex_pc, Instruction::INVOKE_VIRTUAL_RANGE_QUICK, true);
        break;

      default:
        // Nothing to do.
        break;
    }
  }
}

void DexCompiler::CompileReturnVoid(Instruction* inst, uint32_t dex_pc) {
  DCHECK_EQ(inst->Opcode(), Instruction::RETURN_VOID);
  if (unit_.IsConstructor()) {
    // Are we compiling a non clinit constructor which needs a barrier ?
    if (!unit_.IsStatic() &&
        driver_.RequiresConstructorBarrier(Thread::Current(), unit_.GetDexFile(),
                                           unit_.GetClassDefIndex())) {
      return;
    }
  }
  // Replace RETURN_VOID by RETURN_VOID_NO_BARRIER.
  VLOG(compiler) << "Replacing " << Instruction::Name(inst->Opcode())
                 << " by " << Instruction::Name(Instruction::RETURN_VOID_NO_BARRIER)
                 << " at dex pc " << StringPrintf("0x%x", dex_pc) << " in method "
                 << PrettyMethod(unit_.GetDexMethodIndex(), GetDexFile(), true);
  inst->SetOpcode(Instruction::RETURN_VOID_NO_BARRIER);
}

Instruction* DexCompiler::CompileCheckCast(Instruction* inst, uint32_t dex_pc) {
  if (!kEnableCheckCastEllision || !PerformOptimizations()) {
    return inst;
  }
  if (!driver_.IsSafeCast(&unit_, dex_pc)) {
    return inst;
  }
  // Ok, this is a safe cast. Since the "check-cast" instruction size is 2 code
  // units and a "nop" instruction size is 1 code unit, we need to replace it by
  // 2 consecutive NOP instructions.
  // Because the caller loops over instructions by calling Instruction::Next onto
  // the current instruction, we need to return the 2nd NOP instruction. Indeed,
  // its next instruction is the former check-cast's next instruction.
  VLOG(compiler) << "Removing " << Instruction::Name(inst->Opcode())
                 << " by replacing it with 2 NOPs at dex pc "
                 << StringPrintf("0x%x", dex_pc) << " in method "
                 << PrettyMethod(unit_.GetDexMethodIndex(), GetDexFile(), true);
  // We are modifying 4 consecutive bytes.
  inst->SetOpcode(Instruction::NOP);
  inst->SetVRegA_10x(0u);  // keep compliant with verifier.
  // Get to next instruction which is the second half of check-cast and replace
  // it by a NOP.
  inst = const_cast<Instruction*>(inst->Next());
  inst->SetOpcode(Instruction::NOP);
  inst->SetVRegA_10x(0u);  // keep compliant with verifier.
  return inst;
}

void DexCompiler::CompileInstanceFieldAccess(Instruction* inst,
                                             uint32_t dex_pc,
                                             Instruction::Code new_opcode,
                                             bool is_put) {
  if (!kEnableQuickening || !PerformOptimizations()) {
    return;
  }
  uint32_t field_idx = inst->VRegC_22c();
  MemberOffset field_offset(0u);
  bool is_volatile;
  bool fast_path = driver_.ComputeInstanceFieldInfo(field_idx, &unit_, is_put,
                                                    &field_offset, &is_volatile);
  if (fast_path && !is_volatile && IsUint<16>(field_offset.Int32Value())) {
    VLOG(compiler) << "Quickening " << Instruction::Name(inst->Opcode())
                   << " to " << Instruction::Name(new_opcode)
                   << " by replacing field index " << field_idx
                   << " by field offset " << field_offset.Int32Value()
                   << " at dex pc " << StringPrintf("0x%x", dex_pc) << " in method "
                   << PrettyMethod(unit_.GetDexMethodIndex(), GetDexFile(), true);
    // We are modifying 4 consecutive bytes.
    inst->SetOpcode(new_opcode);
    // Replace field index by field offset.
    inst->SetVRegC_22c(static_cast<uint16_t>(field_offset.Int32Value()));
    quickened_info_.push_back(QuickenedInfo(dex_pc, field_idx));
  }
}

void DexCompiler::CompileInvokeVirtual(Instruction* inst, uint32_t dex_pc,
                                       Instruction::Code new_opcode, bool is_range) {
  if (!kEnableQuickening || !PerformOptimizations()) {
    return;
  }
  uint32_t method_idx = is_range ? inst->VRegB_3rc() : inst->VRegB_35c();
  MethodReference target_method(&GetDexFile(), method_idx);
  InvokeType invoke_type = kVirtual;
  InvokeType original_invoke_type = invoke_type;
  int vtable_idx;
  uintptr_t direct_code;
  uintptr_t direct_method;
  // TODO: support devirtualization.
  const bool kEnableDevirtualization = false;
  bool fast_path = driver_.ComputeInvokeInfo(&unit_, dex_pc,
                                             false, kEnableDevirtualization,
                                             &invoke_type,
                                             &target_method, &vtable_idx,
                                             &direct_code, &direct_method);
  if (fast_path && original_invoke_type == invoke_type) {
    if (vtable_idx >= 0 && IsUint<16>(vtable_idx)) {
      VLOG(compiler) << "Quickening " << Instruction::Name(inst->Opcode())
                     << "(" << PrettyMethod(method_idx, GetDexFile(), true) << ")"
                     << " to " << Instruction::Name(new_opcode)
                     << " by replacing method index " << method_idx
                     << " by vtable index " << vtable_idx
                     << " at dex pc " << StringPrintf("0x%x", dex_pc) << " in method "
                     << PrettyMethod(unit_.GetDexMethodIndex(), GetDexFile(), true);
      // We are modifying 4 consecutive bytes.
      inst->SetOpcode(new_opcode);
      // Replace method index by vtable index.
      if (is_range) {
        inst->SetVRegB_3rc(static_cast<uint16_t>(vtable_idx));
      } else {
        inst->SetVRegB_35c(static_cast<uint16_t>(vtable_idx));
      }
      quickened_info_.push_back(QuickenedInfo(dex_pc, method_idx));
    }
  }
}

CompiledMethod* ArtCompileDEX(
    CompilerDriver* driver,
    const DexFile::CodeItem* code_item,
    uint32_t access_flags,
    InvokeType invoke_type ATTRIBUTE_UNUSED,
    uint16_t class_def_idx,
    uint32_t method_idx,
    jobject class_loader,
    const DexFile& dex_file,
    DexToDexCompilationLevel dex_to_dex_compilation_level) {
  DCHECK(driver != nullptr);
  if (dex_to_dex_compilation_level != DexToDexCompilationLevel::kDontDexToDexCompile) {
    ScopedObjectAccess soa(Thread::Current());
    StackHandleScope<1> hs(soa.Self());
    ClassLinker* const class_linker = Runtime::Current()->GetClassLinker();
    art::DexCompilationUnit unit(
        class_loader,
        class_linker,
        dex_file,
        code_item,
        class_def_idx,
        method_idx,
        access_flags,
        driver->GetVerifiedMethod(&dex_file, method_idx),
        hs.NewHandle(class_linker->FindDexCache(soa.Self(), dex_file)));
    art::optimizer::DexCompiler dex_compiler(*driver, unit, dex_to_dex_compilation_level);
    dex_compiler.Compile();
    if (dex_compiler.GetQuickenedInfo().empty()) {
      // No need to create a CompiledMethod if there are no quickened opcodes.
      return nullptr;
    }

    // Create a `CompiledMethod`, with the quickened information in the vmap table.
    Leb128EncodingVector<> builder;
    for (QuickenedInfo info : dex_compiler.GetQuickenedInfo()) {
      builder.PushBackUnsigned(info.dex_pc);
      builder.PushBackUnsigned(info.dex_member_index);
    }
    InstructionSet instruction_set = driver->GetInstructionSet();
    if (instruction_set == kThumb2) {
      // Don't use the thumb2 instruction set to avoid the one off code delta.
      instruction_set = kArm;
    }
    return CompiledMethod::SwapAllocCompiledMethod(
        driver,
        instruction_set,
        ArrayRef<const uint8_t>(),                   // no code
        0,
        0,
        0,
        ArrayRef<const SrcMapElem>(),                // src_mapping_table
        ArrayRef<const uint8_t>(builder.GetData()),  // vmap_table
        ArrayRef<const uint8_t>(),                   // cfi data
        ArrayRef<const LinkerPatch>());
  }
  return nullptr;
}

}  // namespace optimizer

}  // namespace art
