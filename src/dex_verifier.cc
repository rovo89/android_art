// Copyright 2011 Google Inc. All Rights Reserved.

#include "dex_verifier.h"

#include <iostream>

#include "class_linker.h"
#include "dex_cache.h"
#include "dex_file.h"
#include "dex_instruction.h"
#include "dex_instruction_visitor.h"
#include "dex_verifier.h"
#include "logging.h"
#include "runtime.h"
#include "stringpiece.h"

namespace art {

#define k_  kRegTypeUnknown
#define kU  kRegTypeUninit
#define kX  kRegTypeConflict
#define k0  kRegTypeZero
#define k1  kRegTypeOne
#define kZ  kRegTypeBoolean
#define ky  kRegTypeConstPosByte
#define kY  kRegTypeConstByte
#define kh  kRegTypeConstPosShort
#define kH  kRegTypeConstShort
#define kc  kRegTypeConstChar
#define ki  kRegTypeConstInteger
#define kb  kRegTypePosByte
#define kB  kRegTypeByte
#define ks  kRegTypePosShort
#define kS  kRegTypeShort
#define kC  kRegTypeChar
#define kI  kRegTypeInteger
#define kF  kRegTypeFloat
#define kN  kRegTypeConstLo
#define kn  kRegTypeConstHi
#define kJ  kRegTypeLongLo
#define kj  kRegTypeLongHi
#define kD  kRegTypeDoubleLo
#define kd  kRegTypeDoubleHi

const char DexVerifier::merge_table_[kRegTypeMAX][kRegTypeMAX] =
  {
    /* chk:  _  U  X  0  1  Z  y  Y  h  H  c  i  b  B  s  S  C  I  F  N  n  J  j  D  d */
    { /*_*/ k_,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX },
    { /*U*/ kX,kU,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX },
    { /*X*/ kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX },
    { /*0*/ kX,kX,kX,k0,kZ,kZ,ky,kY,kh,kH,kc,ki,kb,kB,ks,kS,kC,kI,kF,kX,kX,kX,kX,kX,kX },
    { /*1*/ kX,kX,kX,kZ,k1,kZ,ky,kY,kh,kH,kc,ki,kb,kB,ks,kS,kC,kI,kF,kX,kX,kX,kX,kX,kX },
    { /*Z*/ kX,kX,kX,kZ,kZ,kZ,ky,kY,kh,kH,kc,ki,kb,kB,ks,kS,kC,kI,kF,kX,kX,kX,kX,kX,kX },
    { /*y*/ kX,kX,kX,ky,ky,ky,ky,kY,kh,kH,kc,ki,kb,kB,ks,kS,kC,kI,kF,kX,kX,kX,kX,kX,kX },
    { /*Y*/ kX,kX,kX,kY,kY,kY,kY,kY,kh,kH,kc,ki,kB,kB,kS,kS,kI,kI,kF,kX,kX,kX,kX,kX,kX },
    { /*h*/ kX,kX,kX,kh,kh,kh,kh,kh,kh,kH,kc,ki,ks,kS,ks,kS,kC,kI,kF,kX,kX,kX,kX,kX,kX },
    { /*H*/ kX,kX,kX,kH,kH,kH,kH,kH,kH,kH,kc,ki,kS,kS,kS,kS,kI,kI,kF,kX,kX,kX,kX,kX,kX },
    { /*c*/ kX,kX,kX,kc,kc,kc,kc,kc,kc,kc,kc,ki,kC,kI,kC,kI,kC,kI,kF,kX,kX,kX,kX,kX,kX },
    { /*i*/ kX,kX,kX,ki,ki,ki,ki,ki,ki,ki,ki,ki,kI,kI,kI,kI,kI,kI,kF,kX,kX,kX,kX,kX,kX },
    { /*b*/ kX,kX,kX,kb,kb,kb,kb,kB,ks,kS,kC,kI,kb,kB,ks,kS,kC,kI,kX,kX,kX,kX,kX,kX,kX },
    { /*B*/ kX,kX,kX,kB,kB,kB,kB,kB,kS,kS,kI,kI,kB,kB,kS,kS,kI,kI,kX,kX,kX,kX,kX,kX,kX },
    { /*s*/ kX,kX,kX,ks,ks,ks,ks,kS,ks,kS,kC,kI,ks,kS,ks,kS,kC,kI,kX,kX,kX,kX,kX,kX,kX },
    { /*S*/ kX,kX,kX,kS,kS,kS,kS,kS,kS,kS,kI,kI,kS,kS,kS,kS,kI,kI,kX,kX,kX,kX,kX,kX,kX },
    { /*C*/ kX,kX,kX,kC,kC,kC,kC,kI,kC,kI,kC,kI,kC,kI,kC,kI,kC,kI,kX,kX,kX,kX,kX,kX,kX },
    { /*I*/ kX,kX,kX,kI,kI,kI,kI,kI,kI,kI,kI,kI,kI,kI,kI,kI,kI,kI,kX,kX,kX,kX,kX,kX,kX },
    { /*F*/ kX,kX,kX,kF,kF,kF,kF,kF,kF,kF,kF,kF,kX,kX,kX,kX,kX,kX,kF,kX,kX,kX,kX,kX,kX },
    { /*N*/ kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kN,kX,kJ,kX,kD,kX },
    { /*n*/ kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kn,kX,kj,kX,kd },
    { /*J*/ kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kJ,kX,kJ,kX,kX,kX },
    { /*j*/ kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kj,kX,kj,kX,kX },
    { /*D*/ kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kD,kX,kX,kX,kD,kX },
    { /*d*/ kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kX,kd,kX,kX,kX,kd },
  };

#undef k_
#undef kU
#undef kX
#undef k0
#undef k1
#undef kZ
#undef ky
#undef kY
#undef kh
#undef kH
#undef kc
#undef ki
#undef kb
#undef kB
#undef ks
#undef kS
#undef kC
#undef kI
#undef kF
#undef kN
#undef kn
#undef kJ
#undef kj
#undef kD
#undef kd

bool DexVerifier::VerifyClass(Class* klass) {
  if (klass->IsVerified()) {
    return true;
  }
  for (size_t i = 0; i < klass->NumDirectMethods(); ++i) {
    Method* method = klass->GetDirectMethod(i);
    if (!VerifyMethod(method)) {
      LOG(ERROR) << "Verifier rejected class "
                 << klass->GetDescriptor()->ToModifiedUtf8();
      return false;
    }
  }
  for (size_t i = 0; i < klass->NumVirtualMethods(); ++i) {
    Method* method = klass->GetVirtualMethod(i);
    if (!VerifyMethod(method)) {
      LOG(ERROR) << "Verifier rejected class "
                 << klass->GetDescriptor()->ToModifiedUtf8();
      return false;
    }
  }
  return true;
}

bool DexVerifier::VerifyMethod(Method* method) {
  const DexCache* dex_cache = method->GetDeclaringClass()->GetDexCache();
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  const DexFile& dex_file = class_linker->FindDexFile(dex_cache);
  const DexFile::CodeItem* code_item =
      dex_file.GetCodeItem(method->GetCodeItemOffset());

  /*
   * Construct the verifier state container object.
   */
  VerifierData vdata(method, &dex_file, code_item);

  /*
   * If there aren't any instructions, make sure that's expected, then
   * exit successfully.
   */
  if (code_item == NULL) {
    if (!method->IsNative() && !method->IsAbstract()) {
      LOG(ERROR) << "VFY: zero-length code in concrete non-native method";
      return false;
    }
    return true;
  }

  /*
   * Sanity-check the register counts. ins + locals = registers, so make
   * sure that ins <= registers.
   */
  if (code_item->ins_size_ > code_item->registers_size_) {
    LOG(ERROR) << "VFY: bad register counts (ins=" << code_item->ins_size_
               << " regs=" << code_item->registers_size_;
    return false;
  }

  /*
   * Allocate and initialize an array to hold instruction data.
   */
  vdata.insn_flags_.reset(new InsnFlags[code_item->insns_size_]());

  /*
   * Run through the instructions and see if the width checks out.
   */
  if (!ComputeWidthsAndCountOps(&vdata)) {
    return false;
  }

  /*
   * Flag instructions guarded by a "try" block and check exception handlers.
   */
  if (!ScanTryCatchBlocks(&vdata)) {
    return false;
  }

  /*
   * Perform static instruction verification.
   */
  if (!VerifyInstructions(&vdata)) {
    return false;
  }

  /*
   * Perform code flow analysis.
   */
  if (!VerifyCodeFlow(&vdata)) {
    return false;
  }

  return true;
}

bool DexVerifier::VerifyInstructions(VerifierData* vdata) {
  const DexFile::CodeItem* code_item = vdata->code_item_;
  InsnFlags* insn_flags = vdata->insn_flags_.get();
  const byte* ptr = reinterpret_cast<const byte*>(code_item->insns_);
  const Instruction* inst = Instruction::At(ptr);

  /* Flag the start of the method as a branch target. */
  InsnSetBranchTarget(insn_flags, 0);

  uint32_t width = 0;
  uint32_t insns_size = code_item->insns_size_;

  while (width < insns_size) {
    if (!VerifyInstruction(vdata, inst, width)) {
      LOG(ERROR) << "VFY: rejecting opcode 0x" << std::hex
                 << (int) inst->Opcode() << " at 0x" << width << std::dec;
      return false;
    }

    /* Flag instructions that are garbage collection points */
    if (inst->IsBranch() || inst->IsSwitch() || inst->IsThrow() ||
        inst->IsReturn()) {
      InsnSetGcPoint(insn_flags, width);
    }

    width += inst->Size();
    inst = inst->Next();
  }
  return true;
}

bool DexVerifier::VerifyInstruction(VerifierData* vdata,
    const Instruction* inst, uint32_t code_offset) {
  const DexFile* dex_file = vdata->dex_file_;
  const DexFile::CodeItem* code_item = vdata->code_item_;
  InsnFlags* insn_flags = vdata->insn_flags_.get();
  Instruction::DecodedInstruction dec_insn(inst);
  bool result = true;

  int argumentA = inst->GetVerifyTypeArgumentA();
  int argumentB = inst->GetVerifyTypeArgumentB();
  int argumentC = inst->GetVerifyTypeArgumentC();
  int extra_flags = inst->GetVerifyExtraFlags();

  switch (argumentA) {
    case Instruction::kVerifyRegA:
      result &= CheckRegisterIndex(code_item, dec_insn.vA_);
      break;
    case Instruction::kVerifyRegAWide:
      result &= CheckWideRegisterIndex(code_item, dec_insn.vA_);
      break;
  }

  switch (argumentB) {
    case Instruction::kVerifyRegB:
      result &= CheckRegisterIndex(code_item, dec_insn.vB_);
      break;
    case Instruction::kVerifyRegBField:
      result &= CheckFieldIndex(dex_file, dec_insn.vB_);
      break;
    case Instruction::kVerifyRegBMethod:
      result &= CheckMethodIndex(dex_file, dec_insn.vB_);
      break;
    case Instruction::kVerifyRegBNewInstance:
      result &= CheckNewInstance(dex_file, dec_insn.vB_);
      break;
    case Instruction::kVerifyRegBString:
      result &= CheckStringIndex(dex_file, dec_insn.vB_);
      break;
    case Instruction::kVerifyRegBType:
      result &= CheckTypeIndex(dex_file, dec_insn.vB_);
      break;
    case Instruction::kVerifyRegBWide:
      result &= CheckWideRegisterIndex(code_item, dec_insn.vB_);
      break;
  }

  switch (argumentC) {
    case Instruction::kVerifyRegC:
      result &= CheckRegisterIndex(code_item, dec_insn.vC_);
      break;
    case Instruction::kVerifyRegCField:
      result &= CheckFieldIndex(dex_file, dec_insn.vC_);
      break;
    case Instruction::kVerifyRegCNewArray:
      result &= CheckNewArray(dex_file, dec_insn.vC_);
      break;
    case Instruction::kVerifyRegCType:
      result &= CheckTypeIndex(dex_file, dec_insn.vC_);
      break;
    case Instruction::kVerifyRegCWide:
      result &= CheckWideRegisterIndex(code_item, dec_insn.vC_);
      break;
  }

  switch (extra_flags) {
    case Instruction::kVerifyArrayData:
      result &= CheckArrayData(code_item, code_offset);
      break;
    case Instruction::kVerifyBranchTarget:
      result &= CheckBranchTarget(code_item, insn_flags, code_offset);
      break;
    case Instruction::kVerifySwitchTargets:
      result &= CheckSwitchTargets(code_item, insn_flags, code_offset);
      break;
    case Instruction::kVerifyVarArg:
      result &= CheckVarArgRegs(code_item, dec_insn.vA_, dec_insn.arg_);
      break;
    case Instruction::kVerifyVarArgRange:
      result &= CheckVarArgRangeRegs(code_item, dec_insn.vA_, dec_insn.vC_);
      break;
    case Instruction::kVerifyError:
      LOG(ERROR) << "VFY: unexpected opcode " << std::hex
                 << (int) dec_insn.opcode_ << std::dec;
      result = false;
      break;
  }

  return result;
}

bool DexVerifier::VerifyCodeFlow(VerifierData* vdata) {
  Method* method = vdata->method_;
  const DexFile::CodeItem* code_item = vdata->code_item_;
  uint16_t registers_size = code_item->registers_size_;
  uint32_t insns_size = code_item->insns_size_;
  RegisterTable reg_table;

  if (registers_size * insns_size > 4*1024*1024) {
    LOG(ERROR) << "VFY: warning: method is huge (regs=" << registers_size
               << " insns_size=" << insns_size << ")";
  }

  /* Create and initialize register lists. */
  if (!InitRegisterTable(vdata, &reg_table, kTrackRegsGcPoints)) {
    return false;
  }

  vdata->register_lines_ = reg_table.register_lines_.get();

  /* Allocate a map to hold the classes of uninitialized instances. */
  vdata->uninit_map_.reset(CreateUninitInstanceMap(vdata));

  /* Initialize register types of method arguments. */
  if (!SetTypesFromSignature(vdata, reg_table.register_lines_[0].reg_types_.get())) {
    LOG(ERROR) << "VFY: bad signature '"
               << method->GetSignature()->ToModifiedUtf8() << "' for "
               << method->GetDeclaringClass()->GetDescriptor()->ToModifiedUtf8()
               << "." << method->GetName()->ToModifiedUtf8();
    return false;
  }

  /* Perform code flow verification. */
  if (!CodeFlowVerifyMethod(vdata, &reg_table)) {
    return false;
  }

  /* Generate a register map and add it to the method. */
  UniquePtr<RegisterMap> map(GenerateRegisterMapV(vdata));
  ByteArray* header = ByteArray::Alloc(sizeof(RegisterMapHeader));
  ByteArray* data = ByteArray::Alloc(ComputeRegisterMapSize(map.get()));

  memcpy(header->GetData(), map.get()->header_, sizeof(RegisterMapHeader));
  memcpy(data->GetData(), map.get()->data_, ComputeRegisterMapSize(map.get()));

  method->SetRegisterMapHeader(header);
  method->SetRegisterMapData(data);

  return true;
}

bool DexVerifier::ComputeWidthsAndCountOps(VerifierData* vdata) {
  const uint16_t* insns = vdata->code_item_->insns_;
  uint32_t insns_size = vdata->code_item_->insns_size_;
  InsnFlags* insn_flags = vdata->insn_flags_.get();
  const byte* ptr = reinterpret_cast<const byte*>(insns);
  const Instruction* inst = Instruction::At(ptr);
  size_t new_instance_count = 0;
  size_t monitor_enter_count = 0;
  size_t width = 0;

  while (width < insns_size) {
    Instruction::Code opcode = inst->Opcode();
    if (opcode == Instruction::NEW_INSTANCE) {
      new_instance_count++;
    } else if (opcode == Instruction::MONITOR_ENTER) {
      monitor_enter_count++;
    }

    insn_flags[width] |= inst->Size();
    width += inst->Size();
    inst = inst->Next();
  }

  if (width != insns_size) {
    LOG(ERROR) << "VFY: code did not end where expected (" << width << " vs. "
               << insns_size << ")";
    return false;
  }

  vdata->new_instance_count_ = new_instance_count;
  vdata->monitor_enter_count_ = monitor_enter_count;
  return true;
}

bool DexVerifier::ScanTryCatchBlocks(VerifierData* vdata) {
  const DexFile::CodeItem* code_item = vdata->code_item_;
  InsnFlags* insn_flags = vdata->insn_flags_.get();
  uint32_t insns_size = code_item->insns_size_;
  uint32_t tries_size = code_item->tries_size_;

  if (tries_size == 0) {
    return true;
  }

  const DexFile::TryItem* tries = DexFile::dexGetTryItems(*code_item, 0);

  for (uint32_t idx = 0; idx < tries_size; idx++) {
    const DexFile::TryItem* try_item = &tries[idx];
    uint32_t start = try_item->start_addr_;
    uint32_t end = start + try_item->insn_count_;

    if ((start >= end) || (start >= insns_size) || (end > insns_size)) {
      LOG(ERROR) << "VFY: bad exception entry: startAddr=" << start
                 << " endAddr=" << end << " (size=" << insns_size << ")";
      return false;
    }

    if (InsnGetWidth(insn_flags, start) == 0) {
      LOG(ERROR) << "VFY: 'try' block starts inside an instruction ("
                 << start << ")";
      return false;
    }

    uint32_t addr;
    for (addr = start; addr < end; addr += InsnGetWidth(insn_flags, addr)) {
      InsnSetInTry(insn_flags, addr);
    }
  }

  /* Iterate over each of the handlers to verify target addresses. */
  const byte* handlers_ptr = DexFile::dexGetCatchHandlerData(*code_item, 0);
  uint32_t handlers_size = DecodeUnsignedLeb128(&handlers_ptr);
  for (uint32_t idx = 0; idx < handlers_size; idx++) {
    DexFile::CatchHandlerIterator iterator(handlers_ptr);

    for (; !iterator.HasNext(); iterator.Next()) {
      uint32_t addr = iterator.Get().address_;
      if (InsnGetWidth(insn_flags, addr) == 0) {
        LOG(ERROR) << "VFY: exception handler starts at bad address ("
                   << addr << ")";
        return false;
      }

      InsnSetBranchTarget(insn_flags, addr);
    }

    handlers_ptr = iterator.GetData();
  }

  return true;
}

bool DexVerifier::GetBranchOffset(const DexFile::CodeItem* code_item,
    const InsnFlags insn_flags[], uint32_t cur_offset, int32_t* pOffset,
    bool* pConditional, bool* selfOkay) {
  const uint16_t* insns = code_item->insns_ + cur_offset;

  switch (*insns & 0xff) {
    case Instruction::GOTO:
      *pOffset = ((int16_t) *insns) >> 8;
      *pConditional = false;
      *selfOkay = false;
      break;
    case Instruction::GOTO_32:
      *pOffset = insns[1] | (((uint32_t) insns[2]) << 16);
      *pConditional = false;
      *selfOkay = true;
      break;
    case Instruction::GOTO_16:
      *pOffset = (int16_t) insns[1];
      *pConditional = false;
      *selfOkay = false;
      break;
    case Instruction::IF_EQ:
    case Instruction::IF_NE:
    case Instruction::IF_LT:
    case Instruction::IF_GE:
    case Instruction::IF_GT:
    case Instruction::IF_LE:
    case Instruction::IF_EQZ:
    case Instruction::IF_NEZ:
    case Instruction::IF_LTZ:
    case Instruction::IF_GEZ:
    case Instruction::IF_GTZ:
    case Instruction::IF_LEZ:
      *pOffset = (int16_t) insns[1];
      *pConditional = true;
      *selfOkay = false;
      break;
    default:
      return false;
      break;
  }

  return true;
}

bool DexVerifier::CheckArrayData(const DexFile::CodeItem* code_item,
    uint32_t cur_offset) {
  const uint32_t insn_count = code_item->insns_size_;
  const uint16_t* insns = code_item->insns_ + cur_offset;
  const uint16_t* array_data;
  int32_t array_data_offset;

  DCHECK_LT(cur_offset, insn_count);

  /* make sure the start of the array data table is in range */
  array_data_offset = insns[1] | (((int32_t) insns[2]) << 16);
  if ((int32_t) cur_offset + array_data_offset < 0 ||
      cur_offset + array_data_offset + 2 >= insn_count)
  {
    LOG(ERROR) << "VFY: invalid array data start: at " << cur_offset
               << ", data offset " << array_data_offset << ", count "
               << insn_count;
    return false;
  }

  /* offset to array data table is a relative branch-style offset */
  array_data = insns + array_data_offset;

  /* make sure the table is 32-bit aligned */
  if ((((uint32_t) array_data) & 0x03) != 0) {
    LOG(ERROR) << "VFY: unaligned array data table: at " << cur_offset
               << ", data offset " << array_data_offset;
    return false;
  }

  uint32_t value_width = array_data[1];
  uint32_t value_count = *(uint32_t*) (&array_data[2]);
  uint32_t table_size = 4 + (value_width * value_count + 1) / 2;

  /* make sure the end of the switch is in range */
  if (cur_offset + array_data_offset + table_size > insn_count) {
    LOG(ERROR) << "VFY: invalid array data end: at " << cur_offset
               << ", data offset " << array_data_offset << ", end "
               << cur_offset + array_data_offset + table_size << ", count "
               << insn_count;
    return false;
  }

  return true;
}

bool DexVerifier::CheckNewInstance(const DexFile* dex_file, uint32_t idx) {
  if (idx >= dex_file->GetHeader().type_ids_size_) {
    LOG(ERROR) << "VFY: bad type index " << idx << " (max "
               << dex_file->GetHeader().type_ids_size_ << ")";
    return false;
  }

  const char* descriptor = dex_file->dexStringByTypeIdx(idx);
  if (descriptor[0] != 'L') {
    LOG(ERROR) << "VFY: can't call new-instance on type '"
               << descriptor << "'";
    return false;
  }

  return true;
}

bool DexVerifier::CheckNewArray(const DexFile* dex_file, uint32_t idx) {
  if (idx >= dex_file->GetHeader().type_ids_size_) {
    LOG(ERROR) << "VFY: bad type index " << idx << " (max "
               << dex_file->GetHeader().type_ids_size_ << ")";
    return false;
  }

  int bracket_count = 0;
  const char* descriptor = dex_file->dexStringByTypeIdx(idx);
  const char* cp = descriptor;
  while (*cp++ == '[')
    bracket_count++;

  if (bracket_count == 0) {
    /* The given class must be an array type. */
    LOG(ERROR) << "VFY: can't new-array class '" << descriptor
               << "' (not an array)";
    return false;
  } else if (bracket_count > 255) {
    /* It is illegal to create an array of more than 255 dimensions. */
    LOG(ERROR) << "VFY: can't new-array class '" << descriptor
               << "' (exceeds limit)";
    return false;
  }

  return true;
}

bool DexVerifier::CheckTypeIndex(const DexFile* dex_file, uint32_t idx) {
  if (idx >= dex_file->GetHeader().type_ids_size_) {
    LOG(ERROR) << "VFY: bad type index " << idx << " (max "
               << dex_file->GetHeader().type_ids_size_ << ")";
    return false;
  }
  return true;
}

bool DexVerifier::CheckFieldIndex(const DexFile* dex_file, uint32_t idx) {
  if (idx >= dex_file->GetHeader().field_ids_size_) {
    LOG(ERROR) << "VFY: bad field index " << idx << " (max "
               << dex_file->GetHeader().field_ids_size_ << ")";
    return false;
  }
  return true;
}

bool DexVerifier::CheckMethodIndex(const DexFile* dex_file, uint32_t idx) {
  if (idx >= dex_file->GetHeader().method_ids_size_) {
    LOG(ERROR) << "VFY: bad method index " << idx << " (max "
               << dex_file->GetHeader().method_ids_size_ << ")";
    return false;
  }
  return true;
}

bool DexVerifier::CheckStringIndex(const DexFile* dex_file, uint32_t idx) {
  if (idx >= dex_file->GetHeader().string_ids_size_) {
    LOG(ERROR) << "VFY: bad string index " << idx << " (max "
               << dex_file->GetHeader().string_ids_size_ << ")";
    return false;
  }
  return true;
}

bool DexVerifier::CheckRegisterIndex(const DexFile::CodeItem* code_item,
    uint32_t idx) {
  if (idx >= code_item->registers_size_) {
    LOG(ERROR) << "VFY: register index out of range (" << idx << " >= "
               << code_item->registers_size_ << ")";
    return false;
  }
  return true;
}

bool DexVerifier::CheckWideRegisterIndex(const DexFile::CodeItem* code_item,
    uint32_t idx) {
  if (idx + 1 >= code_item->registers_size_) {
    LOG(ERROR) << "VFY: wide register index out of range (" << idx
               << "+1 >= " << code_item->registers_size_ << ")";
    return false;
  }
  return true;
}

bool DexVerifier::CheckVarArgRegs(const DexFile::CodeItem* code_item,
    uint32_t vA, uint32_t arg[]) {
  uint16_t registers_size = code_item->registers_size_;
  uint32_t idx;

  if (vA > 5) {
    LOG(ERROR) << "VFY: invalid arg count (" << vA << ") in non-range invoke)";
    return false;
  }

  for (idx = 0; idx < vA; idx++) {
    if (arg[idx] > registers_size) {
      LOG(ERROR) << "VFY: invalid reg index (" << arg[idx]
                 << ") in non-range invoke (> " << registers_size << ")";
      return false;
    }
  }

  return true;
}

bool DexVerifier::CheckVarArgRangeRegs(const DexFile::CodeItem* code_item,
    uint32_t vA, uint32_t vC) {
  uint16_t registers_size = code_item->registers_size_;

  /*
   * vA/vC are unsigned 8-bit/16-bit quantities for /range instructions,
   * so there's no risk of integer overflow when adding them here.
   */
  if (vA + vC > registers_size) {
    LOG(ERROR) << "VFY: invalid reg index " << vA << "+" << vC
               << " in range invoke (> " << registers_size << ")";
    return false;
  }

  return true;
}

bool DexVerifier::CheckSwitchTargets(const DexFile::CodeItem* code_item,
    InsnFlags insn_flags[], uint32_t cur_offset) {
  const uint32_t insn_count = code_item->insns_size_;
  const uint16_t* insns = code_item->insns_ + cur_offset;
  const uint16_t* switch_insns;
  uint16_t expected_signature;
  uint32_t switch_count, table_size;
  int32_t switch_offset, keys_offset, targets_offset;
  int32_t offset, abs_offset;
  uint32_t targ;

  /* make sure the start of the switch is in range */
  switch_offset = insns[1] | ((int32_t) insns[2]) << 16;
  if ((int32_t) cur_offset + switch_offset < 0 ||
      cur_offset + switch_offset + 2 >= insn_count) {
    LOG(ERROR) << "VFY: invalid switch start: at " << cur_offset
               << ", switch offset " << switch_offset << ", count "
               << insn_count;
    return false;
  }

  /* offset to switch table is a relative branch-style offset */
  switch_insns = insns + switch_offset;

  /* make sure the table is 32-bit aligned */
  if ((((uint32_t) switch_insns) & 0x03) != 0) {
    LOG(ERROR) << "VFY: unaligned switch table: at " << cur_offset
               << ", switch offset " << switch_offset;
    return false;
  }

  switch_count = switch_insns[1];

  if ((*insns & 0xff) == Instruction::PACKED_SWITCH) {
    /* 0=sig, 1=count, 2/3=firstKey */
    targets_offset = 4;
    keys_offset = -1;
    expected_signature = Instruction::kPackedSwitchSignature;
  } else {
    /* 0=sig, 1=count, 2..count*2 = keys */
    keys_offset = 2;
    targets_offset = 2 + 2 * switch_count;
    expected_signature = Instruction::kSparseSwitchSignature;
  }
  table_size = targets_offset + switch_count * 2;

  if (switch_insns[0] != expected_signature) {
    LOG(ERROR) << "VFY: wrong signature for switch table (0x" << std::hex
               << switch_insns[0] << ", wanted 0x" << expected_signature << ")"
               << std::dec;
    return false;
  }

  /* make sure the end of the switch is in range */
  if (cur_offset + switch_offset + table_size > (uint32_t) insn_count) {
      LOG(ERROR) << "VFY: invalid switch end: at " << cur_offset
                 << ", switch offset " << switch_offset << ", end "
                 << cur_offset + switch_offset + table_size << ", count "
                 << insn_count;
    return false;
  }

  /* for a sparse switch, verify the keys are in ascending order */
  if (keys_offset > 0 && switch_count > 1) {
    int32_t last_key;

    last_key = switch_insns[keys_offset] |
               (switch_insns[keys_offset + 1] << 16);
    for (targ = 1; targ < switch_count; targ++) {
      int32_t key = (int32_t) switch_insns[keys_offset + targ * 2] |
                    (int32_t) (switch_insns[keys_offset + targ * 2 + 1] << 16);
      if (key <= last_key) {
        LOG(ERROR) << "VFY: invalid packed switch: last key=" << last_key
                   << ", this=" << key;
        return false;
      }

      last_key = key;
    }
  }

  /* verify each switch target */
  for (targ = 0; targ < switch_count; targ++) {
    offset = (int32_t) switch_insns[targets_offset + targ * 2] |
             (int32_t) (switch_insns[targets_offset + targ * 2 + 1] << 16);
    abs_offset = cur_offset + offset;

    if (abs_offset < 0 || abs_offset >= (int32_t) insn_count ||
        !InsnIsOpcode(insn_flags, abs_offset)) {
      LOG(ERROR) << "VFY: invalid switch target " << offset << " (-> "
                 << std::hex << abs_offset << ") at " << cur_offset << std::dec
                 << "[" << targ << "]";
      return false;
    }
    InsnSetBranchTarget(insn_flags, abs_offset);
  }

  return true;
}

bool DexVerifier::CheckBranchTarget(const DexFile::CodeItem* code_item,
    InsnFlags insn_flags[], uint32_t cur_offset) {
  const uint32_t insn_count = code_item->insns_size_;
  int32_t offset, abs_offset;
  bool isConditional, selfOkay;

  if (!GetBranchOffset(code_item, insn_flags, cur_offset, &offset,
                       &isConditional, &selfOkay))
    return false;

  if (!selfOkay && offset == 0) {
    LOG(ERROR) << "VFY: branch offset of zero not allowed at" << std::hex
               << cur_offset << std::dec;
    return false;
  }

  /*
   * Check for 32-bit overflow. This isn't strictly necessary if we can
   * depend on the VM to have identical "wrap-around" behavior, but
   * it's unwise to depend on that.
   */
  if (((int64_t) cur_offset + (int64_t) offset) !=
      (int64_t) (cur_offset + offset)) {
    LOG(ERROR) << "VFY: branch target overflow " << std::hex << cur_offset
               << std::dec << " +" << offset;
    return false;
  }
  abs_offset = cur_offset + offset;
  if (abs_offset < 0 || (uint32_t) abs_offset >= insn_count ||
      !InsnIsOpcode(insn_flags, abs_offset))
  {
    LOG(ERROR) << "VFY: invalid branch target " << offset << " (-> "
               << std::hex << abs_offset << ") at " << cur_offset << std::dec;
    return false;
  }
  InsnSetBranchTarget(insn_flags, abs_offset);

  return true;
}

bool DexVerifier::InitRegisterTable(VerifierData* vdata,
    RegisterTable* reg_table, RegisterTrackingMode track_regs_for) {
  const DexFile::CodeItem* code_item = vdata->code_item_;
  InsnFlags* insn_flags = vdata->insn_flags_.get();
  uint16_t registers_size = code_item->registers_size_;
  uint32_t insns_size = code_item->insns_size_;
  uint32_t i;

  /*
   * Every address gets a RegisterLine struct. This is wasteful, but
   * not so much that it's worth chasing through an extra level of
   * indirection.
   */
  reg_table->insn_reg_count_plus_ = registers_size + kExtraRegs;
  reg_table->register_lines_.reset(new RegisterLine[insns_size]());

  DCHECK_GT(insns_size, 0U);

  bool track_monitors;
  //if (gDvm.monitorVerification) {
    //track_monitors = (vdata->monitor_enter_count_ != 0);
  //} else {
    track_monitors = false;
  //}

  /*
   * Allocate entries in the sparse register line table.
   *
   * There is a RegisterLine associated with every address, but not
   * every RegisterLine has non-NULL pointers to storage for its fields.
   */
  for (i = 0; i < insns_size; i++) {
    bool interesting;

    switch (track_regs_for) {
      case kTrackRegsAll:
        interesting = InsnIsOpcode(insn_flags, i);
        break;
      case kTrackRegsGcPoints:
        interesting = InsnIsGcPoint(insn_flags, i) ||
                      InsnIsBranchTarget(insn_flags, i);
        break;
      case kTrackRegsBranches:
        interesting = InsnIsBranchTarget(insn_flags, i);
        break;
      default:
        return false;
    }

    if (interesting) {
      reg_table->register_lines_[i].Alloc(reg_table->insn_reg_count_plus_,
          track_monitors);
    }
  }

  /*
   * Allocate space for our "temporary" register lines.
   */
  reg_table->work_line_.Alloc(reg_table->insn_reg_count_plus_, track_monitors);
  reg_table->saved_line_.Alloc(reg_table->insn_reg_count_plus_, track_monitors);

  return true;
}

DexVerifier::UninitInstanceMap* DexVerifier::CreateUninitInstanceMap(
    VerifierData* vdata) {
  Method* method = vdata->method_;
  const DexFile::CodeItem* code_item = vdata->code_item_;
  size_t new_instance_count = vdata->new_instance_count_;

  if (IsInitMethod(method)) {
    new_instance_count++;
  }

  /*
   * Allocate the header and map as a single unit.
   *
   * TODO: consider having a static instance so we can avoid allocations.
   * I don't think the verifier is guaranteed to be single-threaded when
   * running in the VM (rather than dexopt), so that must be taken into
   * account.
   */
  UninitInstanceMap* uninit_map = new UninitInstanceMap(new_instance_count);

  size_t idx = 0;
  if (IsInitMethod(method)) {
    uninit_map->map_[idx++].addr_ = kUninitThisArgAddr;
  }

  /*
   * Run through and find the new-instance instructions.
   */
  uint32_t addr = 0;
  uint32_t insns_size = code_item->insns_size_;
  const byte* ptr = reinterpret_cast<const byte*>(code_item->insns_);
  const Instruction* inst = Instruction::At(ptr);
  while (addr < insns_size) {
    Instruction::Code opcode = inst->Opcode();
    if (opcode == Instruction::NEW_INSTANCE) {
      uninit_map->map_[idx++].addr_ = addr;
    }

    addr += inst->Size();
    inst = inst->Next();
  }

  CHECK_EQ(idx, new_instance_count);
  return uninit_map;
}

bool DexVerifier::IsInitMethod(const Method* method) {
  return (method->GetName()->Equals("<init>"));
}

Class* DexVerifier::LookupClassByDescriptor(const Method* method,
    const char* descriptor, VerifyError* failure) {
  /*
   * The compiler occasionally puts references to nonexistent classes in
   * signatures. For example, if you have a non-static inner class with no
   * constructor, the compiler provides a private <init> for you.
   * Constructing the class requires <init>(parent), but the outer class can't
   * call that because the method is private. So the compiler generates a
   * package-scope <init>(parent,bogus) method that just calls the regular
   * <init> (the "bogus" part being necessary to distinguish the signature of
   * the synthetic method). Treating the bogus class as an instance of
   * java.lang.Object allows the verifier to process the class successfully.
   */
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  const ClassLoader* class_loader =
      method->GetDeclaringClass()->GetClassLoader();
  Class* klass = class_linker->FindClass(descriptor, class_loader);

  if (klass == NULL) {
    Thread::Current()->ClearException();
    if (strchr(descriptor, '$') != NULL) {
      LOG(INFO) << "VFY: unable to find class referenced in signature ("
                << descriptor << ")";
    } else {
      LOG(ERROR) << "VFY: unable to find class referenced in signature ("
                 << descriptor << ")";
    }

    /* Check if the descriptor is an array. */
    if (descriptor[0] == '[' && descriptor[1] != '\0') {
      /*
       * There should never be a problem loading primitive arrays.
       */
      if (descriptor[1] != 'L' && descriptor[1] != '[') {
        LOG(ERROR) << "VFY: invalid char in signature in '" << descriptor
                   << "'";
        *failure = VERIFY_ERROR_GENERIC;
      }

      /*
       * Try to continue with base array type. This will let us pass basic
       * stuff (e.g. get array len) that wouldn't fly with an Object. This
       * is NOT correct if the missing type is a primitive array, but we
       * should never have a problem loading those. (I'm not convinced this
       * is correct or even useful. Just use Object here?)
       */
      klass = class_linker->FindClass("[Ljava/lang/Object;", class_loader);
    } else if (descriptor[0] == 'L') {
      /*
       * We are looking at a non-array reference descriptor;
       * try to continue with base reference type.
       */
      klass = class_linker->FindSystemClass("Ljava/lang/Object;");
    } else {
      /* We are looking at a primitive type. */
      LOG(ERROR) << "VFY: invalid char in signature in '" << descriptor << "'";
      *failure = VERIFY_ERROR_GENERIC;
    }

    if (klass == NULL) {
      *failure = VERIFY_ERROR_GENERIC;
    }
  }

  if (klass->IsPrimitive()) {
    LOG(ERROR) << "VFY: invalid use of primitive type '" << descriptor << "'";
    *failure = VERIFY_ERROR_GENERIC;
    klass = NULL;
  }

  return klass;
}

Class* DexVerifier::LookupSignatureClass(const Method* method, std::string sig,
    VerifyError* failure) {
  DCHECK_EQ(sig[0], 'L');
  size_t end = sig.find(';');

  if (end == std::string::npos) {
    LOG(ERROR) << "VFY: bad signature component '" << sig << "' (missing ';')";
    *failure = VERIFY_ERROR_GENERIC;
    return NULL;
  }

  return LookupClassByDescriptor(method, sig.substr(0, end + 1).c_str(),
      failure);
}

Class* DexVerifier::LookupSignatureArrayClass(const Method* method,
    std::string sig, VerifyError* failure) {
  DCHECK_EQ(sig[0], '[');
  size_t end = 0;

  while (sig[end] == '[')
    end++;

  if (sig[end] == 'L') {
    end = sig.find(';');
    if (end == std::string::npos) {
      LOG(ERROR) << "VFY: bad signature component '" << sig
                 << "' (missing ';')";
      *failure = VERIFY_ERROR_GENERIC;
      return NULL;
    }
  }

  return LookupClassByDescriptor(method, sig.substr(0, end + 1).c_str(),
      failure);
}

bool DexVerifier::SetTypesFromSignature(VerifierData* vdata, RegType* reg_types)
{
  Method* method = vdata->method_;
  const DexFile* dex_file = vdata->dex_file_;
  const DexFile::CodeItem* code_item = vdata->code_item_;
  UninitInstanceMap* uninit_map = vdata->uninit_map_.get();

  int arg_start = code_item->registers_size_ - code_item->ins_size_;
  int expected_args = code_item->ins_size_;   /* long/double count as two */
  int actual_args = 0;

  DCHECK_GE(arg_start, 0);      /* should have been verified earlier */

  /*
   * Include the "this" pointer.
   */
  if (!method->IsStatic()) {
    /*
     * If this is a constructor for a class other than java.lang.Object,
     * mark the first ("this") argument as uninitialized. This restricts
     * field access until the superclass constructor is called.
     */
    ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
    Class* klass_object = class_linker->FindSystemClass("Ljava/lang/Object;");
    if (IsInitMethod(method) && method->GetDeclaringClass() != klass_object) {
      int idx = SetUninitInstance(uninit_map, kUninitThisArgAddr,
          method->GetDeclaringClass());
      DCHECK_EQ(idx, 0);
      reg_types[arg_start + actual_args] = RegTypeFromUninitIndex(idx);
    } else {
      reg_types[arg_start + actual_args] =
          RegTypeFromClass(method->GetDeclaringClass());
    }
    actual_args++;
  }

  const DexFile::ProtoId& proto_id =
      dex_file->GetProtoId(method->GetProtoIdx());
  DexFile::ParameterIterator iterator(*dex_file, proto_id);
  VerifyError failure = VERIFY_ERROR_NONE;

  for (; iterator.HasNext(); iterator.Next()) {
    const char* descriptor = iterator.GetDescriptor();

    if (descriptor == NULL) {
      break;
    }

    if (actual_args >= expected_args) {
      LOG(ERROR) << "VFY: expected " << expected_args << " args, found more ("
                 << descriptor << ")";
      return false;
    }

    switch (*descriptor) {
      case 'L':
      case '[':
        /*
         * We assume that reference arguments are initialized. The only way
         * it could be otherwise (assuming the caller was verified) is if
         * the current method is <init>, but in that case it's effectively
         * considered initialized the instant we reach here (in the sense
         * that we can return without doing anything or call virtual methods).
         */
        {
          Class* klass =
              LookupClassByDescriptor(method, descriptor, &failure);
          if (failure != VERIFY_ERROR_NONE)
            return false;
          reg_types[arg_start + actual_args] = RegTypeFromClass(klass);
        }
        actual_args++;
        break;
      case 'Z':
        reg_types[arg_start + actual_args] = kRegTypeBoolean;
        actual_args++;
        break;
      case 'C':
        reg_types[arg_start + actual_args] = kRegTypeChar;
        actual_args++;
        break;
      case 'B':
        reg_types[arg_start + actual_args] = kRegTypeByte;
        actual_args++;
        break;
      case 'I':
        reg_types[arg_start + actual_args] = kRegTypeInteger;
        actual_args++;
        break;
      case 'S':
        reg_types[arg_start + actual_args] = kRegTypeShort;
        actual_args++;
        break;
      case 'F':
        reg_types[arg_start + actual_args] = kRegTypeFloat;
        actual_args++;
        break;
      case 'D':
        reg_types[arg_start + actual_args] = kRegTypeDoubleLo;
        reg_types[arg_start + actual_args +1] = kRegTypeDoubleHi;
        actual_args += 2;
        break;
      case 'J':
        reg_types[arg_start + actual_args] = kRegTypeLongLo;
        reg_types[arg_start + actual_args +1] = kRegTypeLongHi;
        actual_args += 2;
        break;
      default:
        LOG(ERROR) << "VFY: unexpected signature type char '" << descriptor
                   << "'";
        return false;
    }
  }

  if (actual_args != expected_args) {
    LOG(ERROR) << "VFY: expected " << expected_args << " args, found "
               << actual_args;
    return false;
  }

  const char* descriptor = dex_file->GetReturnTypeDescriptor(proto_id);

  /*
   * Validate return type. We don't do the type lookup; just want to make
   * sure that it has the right format. Only major difference from the
   * method argument format is that 'V' is supported.
   */
  switch (*descriptor) {
    case 'I':
    case 'C':
    case 'S':
    case 'B':
    case 'Z':
    case 'V':
    case 'F':
    case 'D':
    case 'J':
      if (*(descriptor + 1) != '\0')
        return false;
      break;
    case '[':
      /* single/multi, object/primitive */
      while (*++descriptor == '[')
        ;
      if (*descriptor == 'L') {
        while (*++descriptor != ';' && *descriptor != '\0')
          ;
        if (*descriptor != ';')
          return false;
      } else {
        if (*(descriptor+1) != '\0')
          return false;
      }
      break;
    case 'L':
      /* could be more thorough here, but shouldn't be required */
      while (*++descriptor != ';' && *descriptor != '\0')
        ;
      if (*descriptor != ';')
        return false;
      break;
    default:
      return false;
  }

  return true;
}

int DexVerifier::SetUninitInstance(UninitInstanceMap* uninit_map, int addr,
    Class* klass) {
  int idx;
  DCHECK(klass != NULL);

  /* TODO: binary search when num_entries > 8 */
  for (idx = uninit_map->num_entries_ - 1; idx >= 0; idx--) {
    if (uninit_map->map_[idx].addr_ == addr) {
      if (uninit_map->map_[idx].klass_ != NULL &&
          uninit_map->map_[idx].klass_ != klass) {
        LOG(ERROR) << "VFY: addr " << addr << " already set to "
                   << (int) uninit_map->map_[idx].klass_ << ", not setting to "
                   << (int) klass;
        return -1;          // already set to something else??
      }
      uninit_map->map_[idx].klass_ = klass;
      return idx;
    }
  }

  LOG(FATAL) << "VFY: addr " << addr << " not found in uninit map";
  return -1;
}

bool DexVerifier::CodeFlowVerifyMethod(VerifierData* vdata,
    RegisterTable* reg_table) {
  const Method* method = vdata->method_;
  const DexFile::CodeItem* code_item = vdata->code_item_;
  InsnFlags* insn_flags = vdata->insn_flags_.get();
  const uint16_t* insns = code_item->insns_;
  uint32_t insns_size = code_item->insns_size_;
  size_t insn_idx, start_guess;

  /* Begin by marking the first instruction as "changed". */
  InsnSetChanged(insn_flags, 0, true);

  start_guess = 0;

  /* Continue until no instructions are marked "changed". */
  while (true) {
    /*
     * Find the first marked one. Use "start_guess" as a way to find
     * one quickly.
     */
    for (insn_idx = start_guess; insn_idx < insns_size; insn_idx++) {
      if (InsnIsChanged(insn_flags, insn_idx))
        break;
    }

    if (insn_idx == insns_size) {
      if (start_guess != 0) {
        /* try again, starting from the top */
        start_guess = 0;
        continue;
      } else {
        /* all flags are clear */
        break;
      }
    }

    /*
     * We carry the working set of registers from instruction to instruction.
     * If this address can be the target of a branch (or throw) instruction,
     * or if we're skipping around chasing "changed" flags, we need to load
     * the set of registers from the table.
     *
     * Because we always prefer to continue on to the next instruction, we
     * should never have a situation where we have a stray "changed" flag set
     * on an instruction that isn't a branch target.
     */
    if (InsnIsBranchTarget(insn_flags, insn_idx)) {
      RegisterLine* work_line = &reg_table->work_line_;
      CopyLineFromTable(work_line, reg_table, insn_idx);
    } else {
#ifndef NDEBUG
      /*
       * Sanity check: retrieve the stored register line (assuming
       * a full table) and make sure it actually matches.
       */
      RegisterLine* register_line = GetRegisterLine(reg_table, insn_idx);
      if (register_line->reg_types_.get() != NULL && CompareLineToTable(reg_table,
          insn_idx, &reg_table->work_line_) != 0) {
        Class* klass = method->GetDeclaringClass();
        LOG(ERROR) << "HUH? work_line diverged in "
                   << klass->GetDescriptor()->ToModifiedUtf8() << "."
                   << method->GetName()->ToModifiedUtf8() << " "
                   << method->GetSignature()->ToModifiedUtf8();
      }
#endif
    }

    if (!CodeFlowVerifyInstruction(vdata, reg_table, insn_idx, &start_guess)) {
      Class* klass = method->GetDeclaringClass();
      LOG(ERROR) << "VFY: failure to verify "
                 << klass->GetDescriptor()->ToModifiedUtf8() << "."
                << method->GetName()->ToModifiedUtf8() << " "
                << method->GetSignature()->ToModifiedUtf8();
      return false;
    }

    /* Clear "changed" and mark as visited. */
    InsnSetVisited(insn_flags, insn_idx, true);
    InsnSetChanged(insn_flags, insn_idx, false);
  }

  if (DEAD_CODE_SCAN && ((method->GetAccessFlags() & kAccWritable) == 0)) {
    /*
     * Scan for dead code. There's nothing "evil" about dead code
     * (besides the wasted space), but it indicates a flaw somewhere
     * down the line, possibly in the verifier.
     *
     * If we've substituted "always throw" instructions into the stream,
     * we are almost certainly going to have some dead code.
     */
    int dead_start = -1;
    for (insn_idx = 0; insn_idx < insns_size;
         insn_idx += InsnGetWidth(insn_flags, insn_idx)) {
      /*
       * Switch-statement data doesn't get "visited" by scanner. It
       * may or may not be preceded by a padding NOP (for alignment).
       */
      if (insns[insn_idx] == Instruction::kPackedSwitchSignature ||
          insns[insn_idx] == Instruction::kSparseSwitchSignature ||
          insns[insn_idx] == Instruction::kArrayDataSignature ||
          (insns[insn_idx] == Instruction::NOP &&
           (insns[insn_idx + 1] == Instruction::kPackedSwitchSignature ||
            insns[insn_idx + 1] == Instruction::kSparseSwitchSignature ||
            insns[insn_idx + 1] == Instruction::kArrayDataSignature))) {
        InsnSetVisited(insn_flags, insn_idx, true);
      }

      if (!InsnIsVisited(insn_flags, insn_idx)) {
        if (dead_start < 0)
          dead_start = insn_idx;
      } else if (dead_start >= 0) {
        Class* klass = method->GetDeclaringClass();
        LOG(INFO) << "VFY: dead code 0x" << std::hex << dead_start << "-"
                  << insn_idx - 1 << std::dec << " in "
                  << klass->GetDescriptor()->ToModifiedUtf8() << "."
                  << method->GetName()->ToModifiedUtf8() << " "
                  << method->GetSignature()->ToModifiedUtf8();
        dead_start = -1;
      }
    }
    if (dead_start >= 0) {
      Class* klass = method->GetDeclaringClass();
      LOG(INFO) << "VFY: dead code 0x" << std::hex << dead_start << "-"
                << insn_idx - 1 << std::dec << " in "
                << klass->GetDescriptor()->ToModifiedUtf8() << "."
                << method->GetName()->ToModifiedUtf8() << " "
                << method->GetSignature()->ToModifiedUtf8();
    }
  }

  return true;
}

bool DexVerifier::CodeFlowVerifyInstruction(VerifierData* vdata,
    RegisterTable* reg_table, uint32_t insn_idx, size_t* start_guess) {
  const Method* method = vdata->method_;
  Class* klass = method->GetDeclaringClass();
  const DexFile::CodeItem* code_item = vdata->code_item_;
  InsnFlags* insn_flags = vdata->insn_flags_.get();
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  UninitInstanceMap* uninit_map = vdata->uninit_map_.get();
  const uint16_t* insns = code_item->insns_ + insn_idx;
  uint32_t insns_size = code_item->insns_size_;
  uint32_t registers_size = code_item->registers_size_;

#ifdef VERIFIER_STATS
  if (InsnIsVisited(insn_flags, insn_idx)) {
    gDvm.verifierStats.instrsReexamined++;
  } else {
    gDvm.verifierStats.instrsExamined++;
  }
#endif

  /*
   * Once we finish decoding the instruction, we need to figure out where
   * we can go from here. There are three possible ways to transfer
   * control to another statement:
   *
   * (1) Continue to the next instruction. Applies to all but
   *     unconditional branches, method returns, and exception throws.
   * (2) Branch to one or more possible locations. Applies to branches
   *     and switch statements.
   * (3) Exception handlers. Applies to any instruction that can
   *     throw an exception that is handled by an encompassing "try"
   *     block.
   *
   * We can also return, in which case there is no successor instruction
   * from this point.
   *
   * The behavior can be determined from the OpcodeFlags.
   */
  RegisterLine* work_line = &reg_table->work_line_;
  const DexFile* dex_file = vdata->dex_file_;
  const byte* ptr = reinterpret_cast<const byte*>(insns);
  const Instruction* inst = Instruction::At(ptr);
  Instruction::DecodedInstruction dec_insn(inst);
  int opcode_flag = inst->Flag();

  Class* res_class;
  int32_t branch_target = 0;
  RegType tmp_type;
  bool just_set_result = false;
  VerifyError failure = VERIFY_ERROR_NONE;

  /*
   * Make a copy of the previous register state. If the instruction
   * can throw an exception, we will copy/merge this into the "catch"
   * address rather than work_line, because we don't want the result
   * from the "successful" code path (e.g. a check-cast that "improves"
   * a type) to be visible to the exception handler.
   */
  if ((opcode_flag & Instruction::kThrow) != 0 &&
      InsnIsInTry(insn_flags, insn_idx)) {
    CopyRegisterLine(&reg_table->saved_line_, work_line,
        reg_table->insn_reg_count_plus_);
  } else {
#ifndef NDEBUG
    memset(reg_table->saved_line_.reg_types_.get(), 0xdd,
        reg_table->insn_reg_count_plus_ * sizeof(RegType));
#endif
  }

  switch (dec_insn.opcode_) {
    case Instruction::NOP:
      /*
       * A "pure" NOP has no effect on anything. Data tables start with
       * a signature that looks like a NOP; if we see one of these in
       * the course of executing code then we have a problem.
       */
      if (dec_insn.vA_ != 0) {
        LOG(ERROR) << "VFY: encountered data table in instruction stream";
        failure = VERIFY_ERROR_GENERIC;
      }
      break;

    case Instruction::MOVE:
    case Instruction::MOVE_FROM16:
    case Instruction::MOVE_16:
      CopyRegister1(work_line, dec_insn.vA_, dec_insn.vB_, kTypeCategory1nr,
          &failure);
      break;
    case Instruction::MOVE_WIDE:
    case Instruction::MOVE_WIDE_FROM16:
    case Instruction::MOVE_WIDE_16:
      CopyRegister2(work_line, dec_insn.vA_, dec_insn.vB_, &failure);
      break;
    case Instruction::MOVE_OBJECT:
    case Instruction::MOVE_OBJECT_FROM16:
    case Instruction::MOVE_OBJECT_16:
      CopyRegister1(work_line, dec_insn.vA_, dec_insn.vB_, kTypeCategoryRef,
          &failure);
      break;

    /*
     * The move-result instructions copy data out of a "pseudo-register"
     * with the results from the last method invocation. In practice we
     * might want to hold the result in an actual CPU register, so the
     * Dalvik spec requires that these only appear immediately after an
     * invoke or filled-new-array.
     *
     * These calls invalidate the "result" register. (This is now
     * redundant with the reset done below, but it can make the debug info
     * easier to read in some cases.)
     */
    case Instruction::MOVE_RESULT:
      CopyResultRegister1(work_line, registers_size, dec_insn.vA_,
          kTypeCategory1nr, &failure);
      break;
    case Instruction::MOVE_RESULT_WIDE:
      CopyResultRegister2(work_line, registers_size, dec_insn.vA_, &failure);
      break;
    case Instruction::MOVE_RESULT_OBJECT:
      CopyResultRegister1(work_line, registers_size, dec_insn.vA_,
          kTypeCategoryRef, &failure);
      break;

    case Instruction::MOVE_EXCEPTION:
      /*
       * This statement can only appear as the first instruction in an
       * exception handler (though not all exception handlers need to
       * have one of these). We verify that as part of extracting the
       * exception type from the catch block list.
       *
       * "res_class" will hold the closest common superclass of all
       * exceptions that can be handled here.
       */
      res_class = GetCaughtExceptionType(vdata, insn_idx, &failure);
      if (res_class == NULL) {
        DCHECK(failure != VERIFY_ERROR_NONE);
      } else {
        SetRegisterType(work_line, dec_insn.vA_, RegTypeFromClass(res_class));
      }
      break;

    case Instruction::RETURN_VOID:
      if (!CheckConstructorReturn(method, work_line, registers_size)) {
        failure = VERIFY_ERROR_GENERIC;
      } else if (GetMethodReturnType(dex_file, method) != kRegTypeUnknown) {
        LOG(ERROR) << "VFY: return-void not expected";
        failure = VERIFY_ERROR_GENERIC;
      }
      break;
    case Instruction::RETURN:
      if (!CheckConstructorReturn(method, work_line, registers_size)) {
        failure = VERIFY_ERROR_GENERIC;
      } else {
        /* check the method signature */
        RegType return_type = GetMethodReturnType(dex_file, method);
        CheckTypeCategory(return_type, kTypeCategory1nr, &failure);
        if (failure != VERIFY_ERROR_NONE)
          LOG(ERROR) << "VFY: return-1nr not expected";

        /*
         * compiler may generate synthetic functions that write byte
         * values into boolean fields. Also, it may use integer values
         * for boolean, byte, short, and character return types.
         */
        RegType src_type = GetRegisterType(work_line, dec_insn.vA_);
        if ((return_type == kRegTypeBoolean && src_type == kRegTypeByte) ||
           ((return_type == kRegTypeBoolean || return_type == kRegTypeByte ||
             return_type == kRegTypeShort || return_type == kRegTypeChar) &&
             src_type == kRegTypeInteger))
          return_type = src_type;

        /* check the register contents */
        VerifyRegisterType(work_line, dec_insn.vA_, return_type, &failure);
        if (failure != VERIFY_ERROR_NONE) {
          LOG(ERROR) << "VFY: return-1nr on invalid register v" << dec_insn.vA_;
        }
      }
      break;
    case Instruction::RETURN_WIDE:
      if (!CheckConstructorReturn(method, work_line, registers_size)) {
        failure = VERIFY_ERROR_GENERIC;
      } else {
        RegType return_type;

        /* check the method signature */
        return_type = GetMethodReturnType(dex_file, method);
        CheckTypeCategory(return_type, kTypeCategory2, &failure);
        if (failure != VERIFY_ERROR_NONE)
          LOG(ERROR) << "VFY: return-wide not expected";

        /* check the register contents */
        VerifyRegisterType(work_line, dec_insn.vA_, return_type, &failure);
        if (failure != VERIFY_ERROR_NONE) {
          LOG(ERROR) << "VFY: return-wide on invalid register pair v"
                     << dec_insn.vA_;
        }
      }
      break;
    case Instruction::RETURN_OBJECT:
      if (!CheckConstructorReturn(method, work_line, registers_size)) {
        failure = VERIFY_ERROR_GENERIC;
      } else {
        RegType return_type = GetMethodReturnType(dex_file, method);
        CheckTypeCategory(return_type, kTypeCategoryRef, &failure);
        if (failure != VERIFY_ERROR_NONE) {
          LOG(ERROR) << "VFY: return-object not expected";
          break;
        }

        /* return_type is the *expected* return type, not register value */
        DCHECK(return_type != kRegTypeZero);
        DCHECK(!RegTypeIsUninitReference(return_type));

        /*
         * Verify that the reference in vAA is an instance of the type
         * in "return_type". The Zero type is allowed here. If the
         * method is declared to return an interface, then any
         * initialized reference is acceptable.
         *
         * Note GetClassFromRegister fails if the register holds an
         * uninitialized reference, so we do not allow them to be
         * returned.
         */
        Class* decl_class = RegTypeInitializedReferenceToClass(return_type);
        res_class = GetClassFromRegister(work_line, dec_insn.vA_, &failure);
        if (failure != VERIFY_ERROR_NONE)
          break;
        if (res_class != NULL) {
          if (!decl_class->IsInterface() &&
              !decl_class->IsAssignableFrom(res_class)) {
            LOG(ERROR) << "VFY: returning " << std::hex
                       << res_class->GetDescriptor()->ToModifiedUtf8()
                       << " (cl=0x" << (int) res_class->GetClassLoader()
                       << "), declared "
                       << decl_class->GetDescriptor()->ToModifiedUtf8()
                       << " (cl=0x" << (int) decl_class->GetClassLoader()
                       << ")" << std::dec;
            failure = VERIFY_ERROR_GENERIC;
            break;
          }
        }
      }
      break;

    case Instruction::CONST_4:
    case Instruction::CONST_16:
    case Instruction::CONST:
      /* could be boolean, int, float, or a null reference */
      SetRegisterType(work_line, dec_insn.vA_,
          DetermineCat1Const((int32_t) dec_insn.vB_));
      break;
    case Instruction::CONST_HIGH16:
      /* could be boolean, int, float, or a null reference */
      SetRegisterType(work_line, dec_insn.vA_,
          DetermineCat1Const((int32_t) dec_insn.vB_ << 16));
      break;
    case Instruction::CONST_WIDE_16:
    case Instruction::CONST_WIDE_32:
    case Instruction::CONST_WIDE:
    case Instruction::CONST_WIDE_HIGH16:
      /* could be long or double; resolved upon use */
      SetRegisterType(work_line, dec_insn.vA_, kRegTypeConstLo);
      break;
    case Instruction::CONST_STRING:
    case Instruction::CONST_STRING_JUMBO:
      SetRegisterType(work_line, dec_insn.vA_, RegTypeFromClass(
          class_linker->FindSystemClass("Ljava/lang/String;")));
      break;
    case Instruction::CONST_CLASS:
      /* make sure we can resolve the class; access check is important */
      res_class = ResolveClassAndCheckAccess(dex_file, dec_insn.vB_, klass, &failure);
      if (res_class == NULL) {
        const char* bad_class_desc = dex_file->dexStringByTypeIdx(dec_insn.vB_);
        LOG(ERROR) << "VFY: unable to resolve const-class " << dec_insn.vB_
                   << " (" << bad_class_desc << ") in "
                   << klass->GetDescriptor()->ToModifiedUtf8();
        DCHECK(failure != VERIFY_ERROR_GENERIC);
      } else {
        SetRegisterType(work_line, dec_insn.vA_, RegTypeFromClass(
            class_linker->FindSystemClass("Ljava/lang/Class;")));
      }
      break;

    case Instruction::MONITOR_ENTER:
      HandleMonitorEnter(work_line, dec_insn.vA_, insn_idx, &failure);
      break;
    case Instruction::MONITOR_EXIT:
      /*
       * monitor-exit instructions are odd. They can throw exceptions,
       * but when they do they act as if they succeeded and the PC is
       * pointing to the following instruction. (This behavior goes back
       * to the need to handle asynchronous exceptions, a now-deprecated
       * feature that Dalvik doesn't support.)
       *
       * In practice we don't need to worry about this. The only
       * exceptions that can be thrown from monitor-exit are for a
       * null reference and -exit without a matching -enter. If the
       * structured locking checks are working, the former would have
       * failed on the -enter instruction, and the latter is impossible.
       *
       * This is fortunate, because issue 3221411 prevents us from
       * chasing the "can throw" path when monitor verification is
       * enabled. If we can fully verify the locking we can ignore
       * some catch blocks (which will show up as "dead" code when
       * we skip them here); if we can't, then the code path could be
       * "live" so we still need to check it.
       */
      if (work_line->monitor_entries_.get() != NULL)
        opcode_flag &= ~Instruction::kThrow;
      HandleMonitorExit(work_line, dec_insn.vA_, insn_idx, &failure);
      break;

    case Instruction::CHECK_CAST:
      /*
       * If this instruction succeeds, we will promote register vA to
       * the type in vB. (This could be a demotion -- not expected, so
       * we don't try to address it.)
       *
       * If it fails, an exception is thrown, which we deal with later
       * by ignoring the update to dec_insn.vA_ when branching to a handler.
       */
      res_class = ResolveClassAndCheckAccess(dex_file, dec_insn.vB_, klass, &failure);
      if (res_class == NULL) {
        const char* bad_class_desc = dex_file->dexStringByTypeIdx(dec_insn.vB_);
        LOG(ERROR) << "VFY: unable to resolve check-cast " << dec_insn.vB_
                   << " (" << bad_class_desc << ") in "
                   << klass->GetDescriptor()->ToModifiedUtf8();
        DCHECK(failure != VERIFY_ERROR_GENERIC);
      } else {
        RegType orig_type;

        orig_type = GetRegisterType(work_line, dec_insn.vA_);
        if (!RegTypeIsReference(orig_type)) {
          LOG(ERROR) << "VFY: check-cast on non-reference in v" << dec_insn.vA_;
          failure = VERIFY_ERROR_GENERIC;
          break;
        }
        SetRegisterType(work_line, dec_insn.vA_, RegTypeFromClass(res_class));
      }
      break;
    case Instruction::INSTANCE_OF:
      /* make sure we're checking a reference type */
      tmp_type = GetRegisterType(work_line, dec_insn.vB_);
      if (!RegTypeIsReference(tmp_type)) {
        LOG(ERROR) << "VFY: vB not a reference (" << tmp_type << ")";
        failure = VERIFY_ERROR_GENERIC;
        break;
      }

      /* make sure we can resolve the class; access check is important */
      res_class = ResolveClassAndCheckAccess(dex_file, dec_insn.vC_, klass, &failure);
      if (res_class == NULL) {
        const char* bad_class_desc = dex_file->dexStringByTypeIdx(dec_insn.vC_);
        LOG(ERROR) << "VFY: unable to resolve instanceof " << dec_insn.vC_
                   << " (" << bad_class_desc << ") in "
                   << klass->GetDescriptor()->ToModifiedUtf8();
        DCHECK(failure != VERIFY_ERROR_GENERIC);
      } else {
        /* result is boolean */
        SetRegisterType(work_line, dec_insn.vA_, kRegTypeBoolean);
      }
      break;

    case Instruction::ARRAY_LENGTH:
      res_class = GetClassFromRegister(work_line, dec_insn.vB_, &failure);
      if (failure != VERIFY_ERROR_NONE)
        break;
      if (res_class != NULL && !res_class->IsArrayClass()) {
        LOG(ERROR) << "VFY: array-length on non-array";
        failure = VERIFY_ERROR_GENERIC;
        break;
      }
      SetRegisterType(work_line, dec_insn.vA_, kRegTypeInteger);
      break;

    case Instruction::NEW_INSTANCE:
      res_class = ResolveClassAndCheckAccess(dex_file, dec_insn.vB_, klass, &failure);
      if (res_class == NULL) {
        const char* bad_class_desc = dex_file->dexStringByTypeIdx(dec_insn.vB_);
        LOG(ERROR) << "VFY: unable to resolve new-instance " << dec_insn.vB_
                   << " (" << bad_class_desc << ") in "
                   << klass->GetDescriptor()->ToModifiedUtf8();
        DCHECK(failure != VERIFY_ERROR_GENERIC);
      } else {
        RegType uninit_type;

        /* can't create an instance of an interface or abstract class */
        if (res_class->IsAbstract() || res_class->IsInterface()) {
          LOG(ERROR) << "VFY: new-instance on interface or abstract class"
                     << res_class->GetDescriptor()->ToModifiedUtf8();
          failure = VERIFY_ERROR_INSTANTIATION;
          break;
        }

        /* add resolved class to uninit map if not already there */
        int uidx = SetUninitInstance(uninit_map, insn_idx, res_class);
        DCHECK_GE(uidx, 0);
        uninit_type = RegTypeFromUninitIndex(uidx);

        /*
         * Any registers holding previous allocations from this address
         * that have not yet been initialized must be marked invalid.
         */
        MarkUninitRefsAsInvalid(work_line, registers_size, uninit_map,
            uninit_type);

        /* add the new uninitialized reference to the register ste */
        SetRegisterType(work_line, dec_insn.vA_, uninit_type);
      }
      break;
   case Instruction::NEW_ARRAY:
      res_class = ResolveClassAndCheckAccess(dex_file, dec_insn.vC_, klass, &failure);
      if (res_class == NULL) {
        const char* bad_class_desc = dex_file->dexStringByTypeIdx(dec_insn.vC_);
        LOG(ERROR) << "VFY: unable to resolve new-array " << dec_insn.vC_
                   << " (" << bad_class_desc << ") in "
                   << klass->GetDescriptor()->ToModifiedUtf8();
        DCHECK(failure != VERIFY_ERROR_GENERIC);
      } else if (!res_class->IsArrayClass()) {
        LOG(ERROR) << "VFY: new-array on non-array class";
        failure = VERIFY_ERROR_GENERIC;
      } else {
        /* make sure "size" register is valid type */
        VerifyRegisterType(work_line, dec_insn.vB_, kRegTypeInteger, &failure);
        /* set register type to array class */
        SetRegisterType(work_line, dec_insn.vA_, RegTypeFromClass(res_class));
      }
      break;
    case Instruction::FILLED_NEW_ARRAY:
    case Instruction::FILLED_NEW_ARRAY_RANGE:
      res_class = ResolveClassAndCheckAccess(dex_file, dec_insn.vB_, klass, &failure);
      if (res_class == NULL) {
        const char* bad_class_desc = dex_file->dexStringByTypeIdx(dec_insn.vB_);
        LOG(ERROR) << "VFY: unable to resolve filled-array " << dec_insn.vB_
                   << " (" << bad_class_desc << ") in "
                   << klass->GetDescriptor()->ToModifiedUtf8();
        DCHECK(failure != VERIFY_ERROR_GENERIC);
      } else if (!res_class->IsArrayClass()) {
        LOG(ERROR) << "VFY: filled-new-array on non-array class";
        failure = VERIFY_ERROR_GENERIC;
      } else {
        bool is_range = (dec_insn.opcode_ ==
            Instruction::FILLED_NEW_ARRAY_RANGE);

        /* check the arguments to the instruction */
        VerifyFilledNewArrayRegs(method, work_line, &dec_insn, res_class,
            is_range, &failure);
        /* filled-array result goes into "result" register */
        SetResultRegisterType(work_line, registers_size,
            RegTypeFromClass(res_class));
        just_set_result = true;
      }
      break;

    case Instruction::CMPL_FLOAT:
    case Instruction::CMPG_FLOAT:
      VerifyRegisterType(work_line, dec_insn.vB_, kRegTypeFloat, &failure);
      VerifyRegisterType(work_line, dec_insn.vC_, kRegTypeFloat, &failure);
      SetRegisterType(work_line, dec_insn.vA_, kRegTypeBoolean);
      break;
    case Instruction::CMPL_DOUBLE:
    case Instruction::CMPG_DOUBLE:
      VerifyRegisterType(work_line, dec_insn.vB_, kRegTypeDoubleLo, &failure);
      VerifyRegisterType(work_line, dec_insn.vC_, kRegTypeDoubleLo, &failure);
      SetRegisterType(work_line, dec_insn.vA_, kRegTypeBoolean);
      break;
    case Instruction::CMP_LONG:
      VerifyRegisterType(work_line, dec_insn.vB_, kRegTypeLongLo, &failure);
      VerifyRegisterType(work_line, dec_insn.vC_, kRegTypeLongLo, &failure);
      SetRegisterType(work_line, dec_insn.vA_, kRegTypeBoolean);
      break;

    case Instruction::THROW:
      res_class = GetClassFromRegister(work_line, dec_insn.vA_, &failure);
      if (failure == VERIFY_ERROR_NONE && res_class != NULL) {
        Class* throwable_class =
            class_linker->FindSystemClass("Ljava/lang/Throwable;");
        if (!throwable_class->IsAssignableFrom(res_class)) {
          LOG(ERROR) << "VFY: thrown class "
                     << res_class->GetDescriptor()->ToModifiedUtf8()
                     << " not instanceof Throwable",
          failure = VERIFY_ERROR_GENERIC;
        }
      }
      break;

    case Instruction::GOTO:
    case Instruction::GOTO_16:
    case Instruction::GOTO_32:
      /* no effect on or use of registers */
      break;

    case Instruction::PACKED_SWITCH:
    case Instruction::SPARSE_SWITCH:
      /* verify that vAA is an integer, or can be converted to one */
      VerifyRegisterType(work_line, dec_insn.vA_, kRegTypeInteger, &failure);
      break;

    case Instruction::FILL_ARRAY_DATA:
      {
        RegType value_type;
        const uint16_t *array_data;
        uint16_t elem_width;

        /* Similar to the verification done for APUT */
        res_class = GetClassFromRegister(work_line, dec_insn.vA_, &failure);
        if (failure != VERIFY_ERROR_NONE)
          break;

        /* res_class can be null if the reg type is Zero */
        if (res_class == NULL)
          break;

        Class::PrimitiveType prim_type =
            res_class->GetComponentType()->GetPrimitiveType();
        if (!res_class->IsArrayClass() ||
            prim_type == Class::kPrimNot || prim_type == Class::kPrimVoid) {
          LOG(ERROR) << "VFY: invalid fill-array-data on " <<
                res_class->GetDescriptor()->ToModifiedUtf8();
          failure = VERIFY_ERROR_GENERIC;
          break;
        }

        value_type = PrimitiveTypeToRegType(prim_type);
        DCHECK(value_type != kRegTypeUnknown);

        /*
         * Now verify if the element width in the table matches the element
         * width declared in the array
         */
        array_data = insns + (insns[1] | (((int32_t) insns[2]) << 16));
        if (array_data[0] != Instruction::kArrayDataSignature) {
          LOG(ERROR) << "VFY: invalid magic for array-data";
          failure = VERIFY_ERROR_GENERIC;
          break;
        }

        switch (prim_type) {
          case Class::kPrimBoolean:
          case Class::kPrimByte:
            elem_width = 1;
            break;
          case Class::kPrimChar:
          case Class::kPrimShort:
            elem_width = 2;
            break;
          case Class::kPrimFloat:
          case Class::kPrimInt:
            elem_width = 4;
            break;
          case Class::kPrimDouble:
          case Class::kPrimLong:
            elem_width = 8;
            break;
          default:
            elem_width = 0;
            break;
        }

        /*
         * Since we don't compress the data in Dex, expect to see equal
         * width of data stored in the table and expected from the array
         * class.
         */
        if (array_data[1] != elem_width) {
          LOG(ERROR) << "VFY: array-data size mismatch (" << array_data[1]
                     << " vs " << elem_width << ")";
          failure = VERIFY_ERROR_GENERIC;
        }
      }
      break;

    case Instruction::IF_EQ:
    case Instruction::IF_NE:
      {
        RegType type1, type2;

        type1 = GetRegisterType(work_line, dec_insn.vA_);
        type2 = GetRegisterType(work_line, dec_insn.vB_);

        /* both references? */
        if (RegTypeIsReference(type1) && RegTypeIsReference(type2))
          break;

        /* both category-1nr? */
        CheckTypeCategory(type1, kTypeCategory1nr, &failure);
        CheckTypeCategory(type2, kTypeCategory1nr, &failure);
        if (failure != VERIFY_ERROR_NONE) {
          LOG(ERROR) << "VFY: args to if-eq/if-ne must both be refs or cat1";
          break;
        }
      }
      break;
    case Instruction::IF_LT:
    case Instruction::IF_GE:
    case Instruction::IF_GT:
    case Instruction::IF_LE:
      tmp_type = GetRegisterType(work_line, dec_insn.vA_);
      CheckTypeCategory(tmp_type, kTypeCategory1nr, &failure);
      if (failure != VERIFY_ERROR_NONE) {
        LOG(ERROR) << "VFY: args to 'if' must be cat-1nr";
        break;
      }
      tmp_type = GetRegisterType(work_line, dec_insn.vB_);
      CheckTypeCategory(tmp_type, kTypeCategory1nr, &failure);
      if (failure != VERIFY_ERROR_NONE) {
        LOG(ERROR) << "VFY: args to 'if' must be cat-1nr";
        break;
      }
      break;
    case Instruction::IF_EQZ:
    case Instruction::IF_NEZ:
      tmp_type = GetRegisterType(work_line, dec_insn.vA_);
      if (RegTypeIsReference(tmp_type))
        break;
      CheckTypeCategory(tmp_type, kTypeCategory1nr, &failure);
      if (failure != VERIFY_ERROR_NONE)
        LOG(ERROR) << "VFY: expected cat-1 arg to if";
      break;
    case Instruction::IF_LTZ:
    case Instruction::IF_GEZ:
    case Instruction::IF_GTZ:
    case Instruction::IF_LEZ:
      tmp_type = GetRegisterType(work_line, dec_insn.vA_);
      CheckTypeCategory(tmp_type, kTypeCategory1nr, &failure);
      if (failure != VERIFY_ERROR_NONE)
        LOG(ERROR) << "VFY: expected cat-1 arg to if";
      break;

    case Instruction::AGET:
      tmp_type = kRegTypeConstInteger;
      goto aget_1nr_common;
    case Instruction::AGET_BOOLEAN:
      tmp_type = kRegTypeBoolean;
      goto aget_1nr_common;
    case Instruction::AGET_BYTE:
      tmp_type = kRegTypeByte;
      goto aget_1nr_common;
    case Instruction::AGET_CHAR:
      tmp_type = kRegTypeChar;
      goto aget_1nr_common;
    case Instruction::AGET_SHORT:
      tmp_type = kRegTypeShort;
      goto aget_1nr_common;
aget_1nr_common:
      {
        RegType src_type, index_type;

        index_type = GetRegisterType(work_line, dec_insn.vC_);
        CheckArrayIndexType(method, index_type, &failure);
        if (failure != VERIFY_ERROR_NONE)
          break;

        res_class = GetClassFromRegister(work_line, dec_insn.vB_, &failure);
        if (failure != VERIFY_ERROR_NONE)
          break;
        if (res_class != NULL) {
          /* verify the class */
          Class::PrimitiveType prim_type =
              res_class->GetComponentType()->GetPrimitiveType();
          if (!res_class->IsArrayClass() || prim_type == Class::kPrimNot) {
            LOG(ERROR) << "VFY: invalid aget-1nr target "
                       << res_class->GetDescriptor()->ToModifiedUtf8();
            failure = VERIFY_ERROR_GENERIC;
            break;
          }

          /* make sure array type matches instruction */
          src_type = PrimitiveTypeToRegType(prim_type);

          /* differentiate between float and int */
          if (src_type == kRegTypeFloat || src_type == kRegTypeInteger)
            tmp_type = src_type;

          if (tmp_type != src_type) {
            LOG(ERROR) << "VFY: invalid aget-1nr, array type=" << src_type
                       << " with inst type=" << tmp_type << " (on "
                       << res_class->GetDescriptor()->ToModifiedUtf8() << ")";
            failure = VERIFY_ERROR_GENERIC;
            break;
          }

        }
        SetRegisterType(work_line, dec_insn.vA_, tmp_type);
      }
      break;

    case Instruction::AGET_WIDE:
      {
        RegType dst_type, index_type;

        index_type = GetRegisterType(work_line, dec_insn.vC_);
        CheckArrayIndexType(method, index_type, &failure);
        if (failure != VERIFY_ERROR_NONE)
          break;

        res_class = GetClassFromRegister(work_line, dec_insn.vB_, &failure);
        if (failure != VERIFY_ERROR_NONE)
          break;
        if (res_class != NULL) {
          /* verify the class */
          Class::PrimitiveType prim_type =
              res_class->GetComponentType()->GetPrimitiveType();
          if (!res_class->IsArrayClass() || prim_type == Class::kPrimNot) {
            LOG(ERROR) << "VFY: invalid aget-wide target "
                       << res_class->GetDescriptor()->ToModifiedUtf8();
            failure = VERIFY_ERROR_GENERIC;
            break;
          }

          /* try to refine "dst_type" */
          switch (prim_type) {
            case Class::kPrimLong:
              dst_type = kRegTypeLongLo;
              break;
            case Class::kPrimDouble:
              dst_type = kRegTypeDoubleLo;
              break;
            default:
              LOG(ERROR) << "VFY: invalid aget-wide on "
                         << res_class->GetDescriptor()->ToModifiedUtf8();
              dst_type = kRegTypeUnknown;
              failure = VERIFY_ERROR_GENERIC;
              break;
          }
        } else {
          /*
           * Null array ref; this code path will fail at runtime. We
           * know this is either long or double, so label it const.
           */
          dst_type = kRegTypeConstLo;
        }
        SetRegisterType(work_line, dec_insn.vA_, dst_type);
      }
      break;

    case Instruction::AGET_OBJECT:
      {
        RegType dst_type, index_type;

        index_type = GetRegisterType(work_line, dec_insn.vC_);
        CheckArrayIndexType(method, index_type, &failure);
        if (failure != VERIFY_ERROR_NONE)
          break;

        /* get the class of the array we're pulling an object from */
        res_class = GetClassFromRegister(work_line, dec_insn.vB_, &failure);
        if (failure != VERIFY_ERROR_NONE)
          break;
        if (res_class != NULL) {
          Class* element_class;

          DCHECK(res_class != NULL);
          if (!res_class->IsArrayClass()) {
            LOG(ERROR) << "VFY: aget-object on non-array class";
            failure = VERIFY_ERROR_GENERIC;
            break;
          }
          DCHECK(res_class->GetComponentType() != NULL);

          /*
           * Find the element class.
           */
          element_class = res_class->GetComponentType();
          if (element_class->IsPrimitive()) {
            LOG(ERROR) << "VFY: aget-object on non-ref array class ("
                       << res_class->GetDescriptor()->ToModifiedUtf8() << ")";
            failure = VERIFY_ERROR_GENERIC;
            break;
          }

          dst_type = RegTypeFromClass(element_class);
        } else {
          /*
           * The array reference is NULL, so the current code path will
           * throw an exception. For proper merging with later code
           * paths, and correct handling of "if-eqz" tests on the
           * result of the array get, we want to treat this as a null
           * reference.
           */
          dst_type = kRegTypeZero;
        }
      SetRegisterType(work_line, dec_insn.vA_, dst_type);
      }
      break;
    case Instruction::APUT:
      tmp_type = kRegTypeInteger;
      goto aput_1nr_common;
    case Instruction::APUT_BOOLEAN:
      tmp_type = kRegTypeBoolean;
      goto aput_1nr_common;
    case Instruction::APUT_BYTE:
      tmp_type = kRegTypeByte;
      goto aput_1nr_common;
    case Instruction::APUT_CHAR:
      tmp_type = kRegTypeChar;
      goto aput_1nr_common;
    case Instruction::APUT_SHORT:
      tmp_type = kRegTypeShort;
      goto aput_1nr_common;
aput_1nr_common:
      {
        RegType src_type, dst_type, index_type;

        index_type = GetRegisterType(work_line, dec_insn.vC_);
        CheckArrayIndexType(method, index_type, &failure);
        if (failure != VERIFY_ERROR_NONE)
          break;

        res_class = GetClassFromRegister(work_line, dec_insn.vB_, &failure);
        if (failure != VERIFY_ERROR_NONE)
          break;

        /* res_class can be null if the reg type is Zero */
        if (res_class == NULL)
          break;

        Class::PrimitiveType prim_type =
            res_class->GetComponentType()->GetPrimitiveType();
        if (!res_class->IsArrayClass() || prim_type == Class::kPrimNot) {
          LOG(ERROR) << "VFY: invalid aput-1nr on "
                     << res_class->GetDescriptor()->ToModifiedUtf8();
          failure = VERIFY_ERROR_GENERIC;
          break;
        }

        /* verify that instruction matches array */
        dst_type = PrimitiveTypeToRegType(prim_type);

        /* correct if float */
        if (dst_type == kRegTypeFloat)
          tmp_type = kRegTypeFloat;

        /* make sure the source register has the correct type */
        src_type = GetRegisterType(work_line, dec_insn.vA_);
        if (!CanConvertTo1nr(src_type, tmp_type)) {
          LOG(ERROR) << "VFY: invalid reg type " << src_type
                     << " on aput instr (need " << tmp_type << ")";
          failure = VERIFY_ERROR_GENERIC;
          break;
        }

        VerifyRegisterType(work_line, dec_insn.vA_, dst_type, &failure);

        if (failure != VERIFY_ERROR_NONE || dst_type == kRegTypeUnknown ||
          tmp_type != dst_type) {
          LOG(ERROR) << "VFY: invalid aput-1nr on "
                     << res_class->GetDescriptor()->ToModifiedUtf8()
                     << " (inst=" << tmp_type << " dst=" << dst_type << ")";
          failure = VERIFY_ERROR_GENERIC;
          break;
        }
      }
      break;
    case Instruction::APUT_WIDE:
      tmp_type = GetRegisterType(work_line, dec_insn.vC_);
      CheckArrayIndexType(method, tmp_type, &failure);
      if (failure != VERIFY_ERROR_NONE)
        break;

      res_class = GetClassFromRegister(work_line, dec_insn.vB_, &failure);
      if (failure != VERIFY_ERROR_NONE)
        break;
      if (res_class != NULL) {
        Class::PrimitiveType prim_type =
            res_class->GetComponentType()->GetPrimitiveType();
        /* verify the class and try to refine "dst_type" */
        if (!res_class->IsArrayClass() || prim_type == Class::kPrimNot)
        {
          LOG(ERROR) << "VFY: invalid aput-wide on "
                     << res_class->GetDescriptor()->ToModifiedUtf8();
          failure = VERIFY_ERROR_GENERIC;
          break;
        }

        switch (prim_type) {
          case Class::kPrimLong:
            VerifyRegisterType(work_line, dec_insn.vA_, kRegTypeLongLo,
                &failure);
            break;
          case Class::kPrimDouble:
            VerifyRegisterType(work_line, dec_insn.vA_, kRegTypeDoubleLo,
                &failure);
            break;
          default:
            LOG(ERROR) << "VFY: invalid aput-wide on "
                       << res_class->GetDescriptor()->ToModifiedUtf8();
            failure = VERIFY_ERROR_GENERIC;
            break;
        }
      }
      break;
    case Instruction::APUT_OBJECT:
      tmp_type = GetRegisterType(work_line, dec_insn.vC_);
      CheckArrayIndexType(method, tmp_type, &failure);
      if (failure != VERIFY_ERROR_NONE)
        break;

      /* get the ref we're storing; Zero is okay, Uninit is not */
      res_class = GetClassFromRegister(work_line, dec_insn.vA_, &failure);
      if (failure != VERIFY_ERROR_NONE)
        break;
      if (res_class != NULL) {
        Class* array_class;
        Class* element_class;

        /*
         * Get the array class. If the array ref is null, we won't
         * have type information (and we'll crash at runtime with a
         * null pointer exception).
         */
        array_class = GetClassFromRegister(work_line, dec_insn.vB_, &failure);

        if (array_class != NULL) {
          /* see if the array holds a compatible type */
          if (!array_class->IsArrayClass()) {
            LOG(ERROR) << "VFY: invalid aput-object on "
                       << array_class->GetDescriptor()->ToModifiedUtf8();
            failure = VERIFY_ERROR_GENERIC;
            break;
          }

          /*
           * Find the element class.
           *
           * All we want to check here is that the element type is a
           * reference class. We *don't* check instanceof here, because
           * you can still put a String into a String[] after the latter
           * has been cast to an Object[].
           */
          element_class = array_class->GetComponentType();
          if (element_class->IsPrimitive()) {
            LOG(ERROR) << "VFY: invalid aput-object of "
                       << res_class->GetDescriptor()->ToModifiedUtf8()
                       << " into "
                       << array_class->GetDescriptor()->ToModifiedUtf8();
            failure = VERIFY_ERROR_GENERIC;
            break;
          }
        }
      }
      break;

    case Instruction::IGET:
      tmp_type = kRegTypeInteger;
      goto iget_1nr_common;
    case Instruction::IGET_BOOLEAN:
      tmp_type = kRegTypeBoolean;
      goto iget_1nr_common;
    case Instruction::IGET_BYTE:
      tmp_type = kRegTypeByte;
      goto iget_1nr_common;
    case Instruction::IGET_CHAR:
      tmp_type = kRegTypeChar;
      goto iget_1nr_common;
    case Instruction::IGET_SHORT:
      tmp_type = kRegTypeShort;
      goto iget_1nr_common;
iget_1nr_common:
      {
        Field* inst_field;
        RegType obj_type, field_type;

        obj_type = GetRegisterType(work_line, dec_insn.vB_);
        inst_field = GetInstField(vdata, obj_type, dec_insn.vC_, &failure);
        if (failure != VERIFY_ERROR_NONE)
          break;

        /* make sure the field's type is compatible with expectation */
        field_type =
            PrimitiveTypeToRegType(inst_field->GetType()->GetPrimitiveType());

        /* correct if float */
        if (field_type == kRegTypeFloat)
          tmp_type = kRegTypeFloat;

        if (field_type == kRegTypeUnknown || tmp_type != field_type) {
          Class* inst_field_class = inst_field->GetDeclaringClass();
          LOG(ERROR) << "VFY: invalid iget-1nr of "
                     << inst_field_class->GetDescriptor()->ToModifiedUtf8()
                     << "." << inst_field->GetName()->ToModifiedUtf8()
                     << " (inst=" << tmp_type << " field=" << field_type << ")";
          failure = VERIFY_ERROR_GENERIC;
          break;
        }

        SetRegisterType(work_line, dec_insn.vA_, tmp_type);
      }
      break;
    case Instruction::IGET_WIDE:
      {
        RegType dst_type;
        Field* inst_field;
        RegType obj_type;

        obj_type = GetRegisterType(work_line, dec_insn.vB_);
        inst_field = GetInstField(vdata, obj_type, dec_insn.vC_, &failure);
        Class* inst_field_class = inst_field->GetDeclaringClass();
        if (failure != VERIFY_ERROR_NONE)
          break;
        /* check the type, which should be prim */
        switch (inst_field->GetType()->GetPrimitiveType()) {
          case Class::kPrimDouble:
            dst_type = kRegTypeDoubleLo;
            break;
          case Class::kPrimLong:
            dst_type = kRegTypeLongLo;
            break;
          default:
            LOG(ERROR) << "VFY: invalid iget-wide of "
                       << inst_field_class->GetDescriptor()->ToModifiedUtf8()
                       << "." << inst_field->GetName()->ToModifiedUtf8();
            dst_type = kRegTypeUnknown;
            failure = VERIFY_ERROR_GENERIC;
            break;
        }
        if (failure == VERIFY_ERROR_NONE) {
          SetRegisterType(work_line, dec_insn.vA_, dst_type);
        }
      }
      break;
    case Instruction::IGET_OBJECT:
      {
        Class* field_class;
        Field* inst_field;
        RegType obj_type;

        obj_type = GetRegisterType(work_line, dec_insn.vB_);
        inst_field = GetInstField(vdata, obj_type, dec_insn.vC_, &failure);
        if (failure != VERIFY_ERROR_NONE)
          break;
        field_class = inst_field->GetType();
        if (field_class == NULL) {
          /* class not found or primitive type */
          LOG(ERROR) << "VFY: unable to recover field class from "
                     << inst_field->GetName()->ToModifiedUtf8();
          failure = VERIFY_ERROR_GENERIC;
          break;
        }
        if (failure == VERIFY_ERROR_NONE) {
          DCHECK(!field_class->IsPrimitive()) << PrettyClass(field_class);
          SetRegisterType(work_line, dec_insn.vA_,
              RegTypeFromClass(field_class));
        }
      }
      break;
    case Instruction::IPUT:
      tmp_type = kRegTypeInteger;
      goto iput_1nr_common;
    case Instruction::IPUT_BOOLEAN:
      tmp_type = kRegTypeBoolean;
      goto iput_1nr_common;
    case Instruction::IPUT_BYTE:
      tmp_type = kRegTypeByte;
      goto iput_1nr_common;
    case Instruction::IPUT_CHAR:
      tmp_type = kRegTypeChar;
      goto iput_1nr_common;
    case Instruction::IPUT_SHORT:
      tmp_type = kRegTypeShort;
      goto iput_1nr_common;
iput_1nr_common:
      {
        RegType src_type, field_type, obj_type;
        Field* inst_field;

        obj_type = GetRegisterType(work_line, dec_insn.vB_);
        inst_field = GetInstField(vdata, obj_type, dec_insn.vC_, &failure);
        if (failure != VERIFY_ERROR_NONE)
          break;
        CheckFinalFieldAccess(method, inst_field, &failure);
        if (failure != VERIFY_ERROR_NONE)
          break;

        /* get type of field we're storing into */
        field_type =
            PrimitiveTypeToRegType(inst_field->GetType()->GetPrimitiveType());
        src_type = GetRegisterType(work_line, dec_insn.vA_);

        /* correct if float */
        if (field_type == kRegTypeFloat)
          tmp_type = kRegTypeFloat;

        /*
         * compiler can generate synthetic functions that write byte values
         * into boolean fields.
         */
        if (tmp_type == kRegTypeBoolean && src_type == kRegTypeByte)
          tmp_type = kRegTypeByte;
        if (field_type == kRegTypeBoolean && src_type == kRegTypeByte)
          field_type = kRegTypeByte;

        /* make sure the source register has the correct type */
        if (!CanConvertTo1nr(src_type, tmp_type)) {
          LOG(ERROR) << "VFY: invalid reg type " << src_type
                     << " on iput instr (need " << tmp_type << ")",
          failure = VERIFY_ERROR_GENERIC;
          break;
        }

        VerifyRegisterType(work_line, dec_insn.vA_, field_type, &failure);

        if (failure != VERIFY_ERROR_NONE || field_type == kRegTypeUnknown ||
            tmp_type != field_type) {
          Class* inst_field_class = inst_field->GetDeclaringClass();
          LOG(ERROR) << "VFY: invalid iput-1nr of "
                     << inst_field_class->GetDescriptor()->ToModifiedUtf8()
                     << "." << inst_field->GetName()->ToModifiedUtf8()
                     << " (inst=" << tmp_type << " field=" << field_type << ")";
          failure = VERIFY_ERROR_GENERIC;
          break;
        }
      }
      break;
    case Instruction::IPUT_WIDE:
      Field* inst_field;
      RegType obj_type;

      obj_type = GetRegisterType(work_line, dec_insn.vB_);
      inst_field = GetInstField(vdata, obj_type, dec_insn.vC_, &failure);
      if (failure != VERIFY_ERROR_NONE)
        break;
      CheckFinalFieldAccess(method, inst_field, &failure);
      if (failure != VERIFY_ERROR_NONE)
        break;

      /* check the type, which should be prim */
      switch (inst_field->GetType()->GetPrimitiveType()) {
        case Class::kPrimDouble:
          VerifyRegisterType(work_line, dec_insn.vA_, kRegTypeDoubleLo,
              &failure);
          break;
        case Class::kPrimLong:
          VerifyRegisterType(work_line, dec_insn.vA_, kRegTypeLongLo, &failure);
          break;
        default:
          LOG(ERROR) << "VFY: invalid iput-wide of "
                     << inst_field->GetDeclaringClass()->GetDescriptor()->ToModifiedUtf8()
                     << "." << inst_field->GetName()->ToModifiedUtf8();
          failure = VERIFY_ERROR_GENERIC;
          break;
        }
      break;
    case Instruction::IPUT_OBJECT:
      {
        Class* field_class;
        Class* value_class;
        Field* inst_field;
        RegType obj_type, value_type;

        obj_type = GetRegisterType(work_line, dec_insn.vB_);
        inst_field = GetInstField(vdata, obj_type, dec_insn.vC_, &failure);
        if (failure != VERIFY_ERROR_NONE)
          break;
        CheckFinalFieldAccess(method, inst_field, &failure);
        if (failure != VERIFY_ERROR_NONE)
          break;

        field_class = inst_field->GetType();
        if (field_class == NULL) {
          LOG(ERROR) << "VFY: unable to recover field class from '"
                     << inst_field->GetName()->ToModifiedUtf8() << "'";
          failure = VERIFY_ERROR_GENERIC;
          break;
        }

        value_type = GetRegisterType(work_line, dec_insn.vA_);
        if (!RegTypeIsReference(value_type)) {
          LOG(ERROR) << "VFY: storing non-ref v" << dec_insn.vA_
                     << " into ref field '"
                     << inst_field->GetName()->ToModifiedUtf8() << "' ("
                     << field_class->GetDescriptor()->ToModifiedUtf8() << ")";
          failure = VERIFY_ERROR_GENERIC;
          break;
        }
        if (value_type != kRegTypeZero) {
          value_class = RegTypeInitializedReferenceToClass(value_type);
          if (value_class == NULL) {
            LOG(ERROR) << "VFY: storing uninit ref v" << dec_insn.vA_
                       << " into ref field";
            failure = VERIFY_ERROR_GENERIC;
            break;
          }
          /* allow if field is any interface or field is base class */
          if (!field_class->IsInterface() &&
              !field_class->IsAssignableFrom(value_class)) {
            Class* inst_field_class = inst_field->GetDeclaringClass();
            LOG(ERROR) << "VFY: storing type '"
                       << value_class->GetDescriptor()->ToModifiedUtf8()
                       << "' into field type '"
                       << field_class->GetDescriptor()->ToModifiedUtf8()
                       << "' ("
                       << inst_field_class->GetDescriptor()->ToModifiedUtf8()
                       << "." << inst_field->GetName()->ToModifiedUtf8() << ")";
            failure = VERIFY_ERROR_GENERIC;
            break;
          }
        }
      }
      break;

    case Instruction::SGET:
      tmp_type = kRegTypeInteger;
      goto sget_1nr_common;
    case Instruction::SGET_BOOLEAN:
      tmp_type = kRegTypeBoolean;
      goto sget_1nr_common;
    case Instruction::SGET_BYTE:
      tmp_type = kRegTypeByte;
      goto sget_1nr_common;
    case Instruction::SGET_CHAR:
      tmp_type = kRegTypeChar;
      goto sget_1nr_common;
    case Instruction::SGET_SHORT:
      tmp_type = kRegTypeShort;
      goto sget_1nr_common;
sget_1nr_common:
      {
        Field* static_field;
        RegType field_type;

        static_field = GetStaticField(vdata, dec_insn.vB_, &failure);
        if (failure != VERIFY_ERROR_NONE)
          break;

        /*
         * Make sure the field's type is compatible with expectation.
         * We can get ourselves into trouble if we mix & match loads
         * and stores with different widths, so rather than just checking
         * "CanConvertTo1nr" we require that the field types have equal
         * widths.
         */
        field_type =
            PrimitiveTypeToRegType(static_field->GetType()->GetPrimitiveType());

        /* correct if float */
        if (field_type == kRegTypeFloat)
          tmp_type = kRegTypeFloat;

        if (tmp_type != field_type) {
          Class* static_field_class = static_field->GetDeclaringClass();
          LOG(ERROR) << "VFY: invalid sget-1nr of "
                     << static_field_class->GetDescriptor()->ToModifiedUtf8()
                     << "." << static_field->GetName()->ToModifiedUtf8()
                     << " (inst=" << tmp_type << " actual=" << field_type
                     << ")";
          failure = VERIFY_ERROR_GENERIC;
          break;
        }

        SetRegisterType(work_line, dec_insn.vA_, tmp_type);
      }
      break;
    case Instruction::SGET_WIDE:
      {
        Field* static_field;
        RegType dst_type;

        static_field = GetStaticField(vdata, dec_insn.vB_, &failure);
        Class* static_field_class = static_field->GetDeclaringClass();
        if (failure != VERIFY_ERROR_NONE)
          break;
        /* check the type, which should be prim */
        switch (static_field->GetType()->GetPrimitiveType()) {
          case Class::kPrimDouble:
            dst_type = kRegTypeDoubleLo;
            break;
          case Class::kPrimLong:
            dst_type = kRegTypeLongLo;
            break;
          default:
            LOG(ERROR) << "VFY: invalid sget-wide of "
                       << static_field_class->GetDescriptor()->ToModifiedUtf8()
                       << "." << static_field->GetName()->ToModifiedUtf8();
            dst_type = kRegTypeUnknown;
            failure = VERIFY_ERROR_GENERIC;
            break;
        }
        if (failure == VERIFY_ERROR_NONE) {
          SetRegisterType(work_line, dec_insn.vA_, dst_type);
        }
      }
      break;
    case Instruction::SGET_OBJECT:
      {
        Field* static_field;
        Class* field_class;

        static_field = GetStaticField(vdata, dec_insn.vB_, &failure);
        if (failure != VERIFY_ERROR_NONE)
          break;
        field_class = static_field->GetType();
        if (field_class == NULL) {
          LOG(ERROR) << "VFY: unable to recover field class from '"
                     << static_field->GetName()->ToModifiedUtf8() << "'";
          failure = VERIFY_ERROR_GENERIC;
          break;
        }
        if (field_class->IsPrimitive()) {
          LOG(ERROR) << "VFY: attempt to get prim field with sget-object";
          failure = VERIFY_ERROR_GENERIC;
          break;
        }
      SetRegisterType(work_line, dec_insn.vA_, RegTypeFromClass(field_class));
      }
      break;
    case Instruction::SPUT:
      tmp_type = kRegTypeInteger;
      goto sput_1nr_common;
    case Instruction::SPUT_BOOLEAN:
      tmp_type = kRegTypeBoolean;
      goto sput_1nr_common;
    case Instruction::SPUT_BYTE:
      tmp_type = kRegTypeByte;
      goto sput_1nr_common;
    case Instruction::SPUT_CHAR:
      tmp_type = kRegTypeChar;
      goto sput_1nr_common;
    case Instruction::SPUT_SHORT:
      tmp_type = kRegTypeShort;
      goto sput_1nr_common;
sput_1nr_common:
      {
        RegType src_type, field_type;
        Field* static_field;

        static_field = GetStaticField(vdata, dec_insn.vB_, &failure);
        if (failure != VERIFY_ERROR_NONE)
          break;
        CheckFinalFieldAccess(method, static_field, &failure);
        if (failure != VERIFY_ERROR_NONE)
          break;

        /*
         * Get type of field we're storing into. We know that the
         * contents of the register match the instruction, but we also
         * need to ensure that the instruction matches the field type.
         * Using e.g. sput-short to write into a 32-bit integer field
         * can lead to trouble if we do 16-bit writes.
         */
        field_type =
            PrimitiveTypeToRegType(static_field->GetType()->GetPrimitiveType());
        src_type = GetRegisterType(work_line, dec_insn.vA_);

        /* correct if float */
        if (field_type == kRegTypeFloat)
          tmp_type = kRegTypeFloat;

        /*
         * compiler can generate synthetic functions that write byte values
         * into boolean fields.
         */
        if (tmp_type == kRegTypeBoolean && src_type == kRegTypeByte)
          tmp_type = kRegTypeByte;
        if (field_type == kRegTypeBoolean && src_type == kRegTypeByte)
          field_type = kRegTypeByte;

        /* make sure the source register has the correct type */
        if (!CanConvertTo1nr(src_type, tmp_type)) {
          LOG(ERROR) << "VFY: invalid reg type " << src_type
                     << " on sput instr (need " << tmp_type << ")";
          failure = VERIFY_ERROR_GENERIC;
          break;
        }

        VerifyRegisterType(work_line, dec_insn.vA_, field_type, &failure);

        if (failure != VERIFY_ERROR_NONE || field_type == kRegTypeUnknown ||
            tmp_type != field_type) {
          Class* static_field_class = static_field->GetDeclaringClass();
          LOG(ERROR) << "VFY: invalid sput-1nr of "
                     << static_field_class->GetDescriptor()->ToModifiedUtf8()
                     << "." << static_field->GetName()->ToModifiedUtf8()
                     << " (inst=" << tmp_type << " actual=" << field_type
                     << ")";
          failure = VERIFY_ERROR_GENERIC;
          break;
        }
      }
      break;
    case Instruction::SPUT_WIDE:
      Field* static_field;

      static_field = GetStaticField(vdata, dec_insn.vB_, &failure);
      if (failure != VERIFY_ERROR_NONE)
        break;
      CheckFinalFieldAccess(method, static_field, &failure);
      if (failure != VERIFY_ERROR_NONE)
        break;

      /* check the type, which should be prim */
      switch (static_field->GetType()->GetPrimitiveType()) {
        case Class::kPrimDouble:
          VerifyRegisterType(work_line, dec_insn.vA_, kRegTypeDoubleLo,
              &failure);
          break;
        case Class::kPrimLong:
          VerifyRegisterType(work_line, dec_insn.vA_, kRegTypeLongLo, &failure);
          break;
        default:
          LOG(ERROR) << "VFY: invalid sput-wide of "
                     << static_field->GetDeclaringClass()->GetDescriptor()->ToModifiedUtf8()
                     << "." << static_field->GetName()->ToModifiedUtf8();
          failure = VERIFY_ERROR_GENERIC;
          break;
      }
      break;
    case Instruction::SPUT_OBJECT:
      {
        Class* field_class;
        Class* value_class;
        Field* static_field;
        RegType value_type;

        static_field = GetStaticField(vdata, dec_insn.vB_, &failure);
        if (failure != VERIFY_ERROR_NONE)
          break;
        CheckFinalFieldAccess(method, static_field, &failure);
        if (failure != VERIFY_ERROR_NONE)
          break;

        field_class = static_field->GetType();
        if (field_class == NULL) {
          LOG(ERROR) << "VFY: unable to recover field class from '"
                     << static_field->GetName()->ToModifiedUtf8() << "'";
          failure = VERIFY_ERROR_GENERIC;
          break;
        }

        value_type = GetRegisterType(work_line, dec_insn.vA_);
        if (!RegTypeIsReference(value_type)) {
          LOG(ERROR) << "VFY: storing non-ref v" << dec_insn.vA_
                     << " into ref field '"
                     << static_field->GetName()->ToModifiedUtf8() << "' ("
                     << field_class->GetDescriptor()->ToModifiedUtf8() << ")",
          failure = VERIFY_ERROR_GENERIC;
          break;
        }
        if (value_type != kRegTypeZero) {
          value_class = RegTypeInitializedReferenceToClass(value_type);
          if (value_class == NULL) {
            LOG(ERROR) << "VFY: storing uninit ref v" << dec_insn.vA_
                       << " into ref field";
            failure = VERIFY_ERROR_GENERIC;
            break;
          }
          /* allow if field is any interface or field is base class */
          if (!field_class->IsInterface() &&
              !field_class->IsAssignableFrom(value_class)) {
            Class* static_field_class = static_field->GetDeclaringClass();
            LOG(ERROR) << "VFY: storing type '"
                       << value_class->GetDescriptor()->ToModifiedUtf8()
                       << "' into field type '"
                       << field_class->GetDescriptor()->ToModifiedUtf8()
                       << "' ("
                       << static_field_class->GetDescriptor()->ToModifiedUtf8()
                       << "." << static_field->GetName()->ToModifiedUtf8()
                       << ")",
            failure = VERIFY_ERROR_GENERIC;
            break;
          }
        }
      }
      break;

    case Instruction::INVOKE_VIRTUAL:
    case Instruction::INVOKE_VIRTUAL_RANGE:
    case Instruction::INVOKE_SUPER:
    case Instruction::INVOKE_SUPER_RANGE:
      {
        Method* called_method;
        RegType return_type;
        bool is_range;
        bool is_super;

        is_range =  (dec_insn.opcode_ == Instruction::INVOKE_VIRTUAL_RANGE ||
                     dec_insn.opcode_ == Instruction::INVOKE_SUPER_RANGE);
        is_super =  (dec_insn.opcode_ == Instruction::INVOKE_SUPER ||
                     dec_insn.opcode_ == Instruction::INVOKE_SUPER_RANGE);

        called_method = VerifyInvocationArgs(vdata, work_line, registers_size,
            &dec_insn, METHOD_VIRTUAL, is_range, is_super, &failure);
        if (failure != VERIFY_ERROR_NONE)
            break;
        return_type = GetMethodReturnType(dex_file, called_method);
        SetResultRegisterType(work_line, registers_size, return_type);
        just_set_result = true;
      }
      break;
    case Instruction::INVOKE_DIRECT:
    case Instruction::INVOKE_DIRECT_RANGE:
      {
        RegType return_type;
        Method* called_method;
        bool is_range;

        is_range = (dec_insn.opcode_ == Instruction::INVOKE_DIRECT_RANGE);
        called_method = VerifyInvocationArgs(vdata, work_line, registers_size,
            &dec_insn, METHOD_DIRECT, is_range, false, &failure);
        if (failure != VERIFY_ERROR_NONE)
          break;

        /*
         * Some additional checks when calling <init>. We know from
         * the invocation arg check that the "this" argument is an
         * instance of called_method->klass. Now we further restrict
         * that to require that called_method->klass is the same as
         * this->klass or this->super, allowing the latter only if
         * the "this" argument is the same as the "this" argument to
         * this method (which implies that we're in <init> ourselves).
         */
        if (IsInitMethod(called_method)) {
          RegType this_type;
          this_type = GetInvocationThis(work_line, &dec_insn, &failure);
          if (failure != VERIFY_ERROR_NONE)
            break;

          /* no null refs allowed (?) */
          if (this_type == kRegTypeZero) {
            LOG(ERROR) << "VFY: unable to initialize null ref";
            failure = VERIFY_ERROR_GENERIC;
            break;
          }

          Class* this_class;

          this_class = RegTypeReferenceToClass(this_type, uninit_map);
          DCHECK(this_class != NULL);

          /* must be in same class or in superclass */
          if (called_method->GetDeclaringClass() == this_class->GetSuperClass())
          {
            if (this_class != method->GetDeclaringClass()) {
              LOG(ERROR) << "VFY: invoke-direct <init> on super only "
                         << "allowed for 'this' in <init>";
              failure = VERIFY_ERROR_GENERIC;
              break;
            }
          }  else if (called_method->GetDeclaringClass() != this_class) {
            LOG(ERROR) << "VFY: invoke-direct <init> must be on current "
                       << "class or super";
            failure = VERIFY_ERROR_GENERIC;
            break;
          }

          /* arg must be an uninitialized reference */
          if (!RegTypeIsUninitReference(this_type)) {
            LOG(ERROR) << "VFY: can only initialize the uninitialized";
            failure = VERIFY_ERROR_GENERIC;
            break;
          }

          /*
           * Replace the uninitialized reference with an initialized
           * one, and clear the entry in the uninit map. We need to
           * do this for all registers that have the same object
           * instance in them, not just the "this" register.
           */
          MarkRefsAsInitialized(work_line, registers_size, uninit_map,
              this_type, &failure);
          if (failure != VERIFY_ERROR_NONE)
            break;
          }
        return_type = GetMethodReturnType(dex_file, called_method);
        SetResultRegisterType(work_line, registers_size, return_type);
        just_set_result = true;
      }
      break;
    case Instruction::INVOKE_STATIC:
    case Instruction::INVOKE_STATIC_RANGE:
      {
        RegType return_type;
        Method* called_method;
        bool is_range;

        is_range = (dec_insn.opcode_ == Instruction::INVOKE_STATIC_RANGE);
        called_method = VerifyInvocationArgs(vdata, work_line, registers_size,
            &dec_insn, METHOD_STATIC, is_range, false, &failure);
        if (failure != VERIFY_ERROR_NONE)
          break;

        return_type = GetMethodReturnType(dex_file, called_method);
        SetResultRegisterType(work_line, registers_size, return_type);
        just_set_result = true;
      }
      break;
    case Instruction::INVOKE_INTERFACE:
    case Instruction::INVOKE_INTERFACE_RANGE:
      {
        RegType /*this_type,*/ return_type;
        Method* abs_method;
        bool is_range;

        is_range =  (dec_insn.opcode_ == Instruction::INVOKE_INTERFACE_RANGE);
        abs_method = VerifyInvocationArgs(vdata, work_line, registers_size,
            &dec_insn, METHOD_INTERFACE, is_range, false, &failure);
        if (failure != VERIFY_ERROR_NONE)
          break;

#if 0   /* can't do this here, fails on dalvik test 052-verifier-fun */
        /*
         * Get the type of the "this" arg, which should always be an
         * interface class. Because we don't do a full merge on
         * interface classes, this might have reduced to Object.
         */
        this_type = GetInvocationThis(work_line, &dec_insn, &failure);
        if (failure != VERIFY_ERROR_NONE)
          break;

        if (this_type == kRegTypeZero) {
          /* null pointer always passes (and always fails at runtime) */
        } else {
          Class* this_class;

          this_class = RegTypeInitializedReferenceToClass(this_type);
          if (this_class == NULL) {
            LOG(ERROR) << "VFY: interface call on uninitialized";
            failure = VERIFY_ERROR_GENERIC;
            break;
          }

          /*
           * Either "this_class" needs to be the interface class that
           * defined abs_method, or abs_method's class needs to be one
           * of the interfaces implemented by "this_class". (Or, if
           * we couldn't complete the merge, this will be Object.)
           */
          if (this_class != abs_method->GetDeclaringClass() &&
              this_class != class_linker->FindSystemClass("Ljava/lang/Object;") &&
              !this_class->Implements(abs_method->GetDeclaringClass())) {
            LOG(ERROR) << "VFY: unable to match abs_method '"
                       << abs_method->GetName()->ToModifiedUtf8() << "' with "
                       << this_class->GetDescriptor()->ToModifiedUtf8()
                       << " interfaces";
            failure = VERIFY_ERROR_GENERIC;
            break;
          }
        }
#endif

        /*
         * We don't have an object instance, so we can't find the
         * concrete method. However, all of the type information is
         * in the abstract method, so we're good.
         */
        return_type = GetMethodReturnType(dex_file, abs_method);
        SetResultRegisterType(work_line, registers_size, return_type);
        just_set_result = true;
      }
      break;

    case Instruction::NEG_INT:
    case Instruction::NOT_INT:
      CheckUnop(work_line, &dec_insn, kRegTypeInteger, kRegTypeInteger,
          &failure);
      break;
    case Instruction::NEG_LONG:
    case Instruction::NOT_LONG:
      CheckUnop(work_line, &dec_insn, kRegTypeLongLo, kRegTypeLongLo, &failure);
      break;
    case Instruction::NEG_FLOAT:
      CheckUnop(work_line, &dec_insn, kRegTypeFloat, kRegTypeFloat, &failure);
      break;
    case Instruction::NEG_DOUBLE:
      CheckUnop(work_line, &dec_insn, kRegTypeDoubleLo, kRegTypeDoubleLo,
          &failure);
      break;
    case Instruction::INT_TO_LONG:
      CheckUnop(work_line, &dec_insn, kRegTypeLongLo, kRegTypeInteger,
          &failure);
      break;
    case Instruction::INT_TO_FLOAT:
      CheckUnop(work_line, &dec_insn, kRegTypeFloat, kRegTypeInteger, &failure);
      break;
    case Instruction::INT_TO_DOUBLE:
      CheckUnop(work_line, &dec_insn, kRegTypeDoubleLo, kRegTypeInteger,
          &failure);
      break;
    case Instruction::LONG_TO_INT:
      CheckUnop(work_line, &dec_insn, kRegTypeInteger, kRegTypeLongLo,
          &failure);
      break;
    case Instruction::LONG_TO_FLOAT:
      CheckUnop(work_line, &dec_insn, kRegTypeFloat, kRegTypeLongLo, &failure);
      break;
    case Instruction::LONG_TO_DOUBLE:
      CheckUnop(work_line, &dec_insn, kRegTypeDoubleLo, kRegTypeLongLo,
          &failure);
      break;
    case Instruction::FLOAT_TO_INT:
      CheckUnop(work_line, &dec_insn, kRegTypeInteger, kRegTypeFloat, &failure);
      break;
    case Instruction::FLOAT_TO_LONG:
      CheckUnop(work_line, &dec_insn, kRegTypeLongLo, kRegTypeFloat, &failure);
      break;
    case Instruction::FLOAT_TO_DOUBLE:
      CheckUnop(work_line, &dec_insn, kRegTypeDoubleLo, kRegTypeFloat,
          &failure);
      break;
    case Instruction::DOUBLE_TO_INT:
      CheckUnop(work_line, &dec_insn, kRegTypeInteger, kRegTypeDoubleLo,
          &failure);
      break;
    case Instruction::DOUBLE_TO_LONG:
      CheckUnop(work_line, &dec_insn, kRegTypeLongLo, kRegTypeDoubleLo,
          &failure);
      break;
    case Instruction::DOUBLE_TO_FLOAT:
      CheckUnop(work_line, &dec_insn, kRegTypeFloat, kRegTypeDoubleLo,
          &failure);
      break;
    case Instruction::INT_TO_BYTE:
      CheckUnop(work_line, &dec_insn, kRegTypeByte, kRegTypeInteger, &failure);
      break;
    case Instruction::INT_TO_CHAR:
      CheckUnop(work_line, &dec_insn, kRegTypeChar, kRegTypeInteger, &failure);
      break;
    case Instruction::INT_TO_SHORT:
      CheckUnop(work_line, &dec_insn, kRegTypeShort, kRegTypeInteger, &failure);
      break;

    case Instruction::ADD_INT:
    case Instruction::SUB_INT:
    case Instruction::MUL_INT:
    case Instruction::REM_INT:
    case Instruction::DIV_INT:
    case Instruction::SHL_INT:
    case Instruction::SHR_INT:
    case Instruction::USHR_INT:
      CheckBinop(work_line, &dec_insn, kRegTypeInteger, kRegTypeInteger,
          kRegTypeInteger, false, &failure);
      break;
    case Instruction::AND_INT:
    case Instruction::OR_INT:
    case Instruction::XOR_INT:
      CheckBinop(work_line, &dec_insn, kRegTypeInteger, kRegTypeInteger,
          kRegTypeInteger, true, &failure);
      break;
    case Instruction::ADD_LONG:
    case Instruction::SUB_LONG:
    case Instruction::MUL_LONG:
    case Instruction::DIV_LONG:
    case Instruction::REM_LONG:
    case Instruction::AND_LONG:
    case Instruction::OR_LONG:
    case Instruction::XOR_LONG:
      CheckBinop(work_line, &dec_insn, kRegTypeLongLo, kRegTypeLongLo,
          kRegTypeLongLo, false, &failure);
      break;
    case Instruction::SHL_LONG:
    case Instruction::SHR_LONG:
    case Instruction::USHR_LONG:
      /* shift distance is Int, making these different from other binops */
      CheckBinop(work_line, &dec_insn, kRegTypeLongLo, kRegTypeLongLo,
          kRegTypeInteger, false, &failure);
      break;
    case Instruction::ADD_FLOAT:
    case Instruction::SUB_FLOAT:
    case Instruction::MUL_FLOAT:
    case Instruction::DIV_FLOAT:
    case Instruction::REM_FLOAT:
      CheckBinop(work_line, &dec_insn, kRegTypeFloat, kRegTypeFloat,
          kRegTypeFloat, false, &failure);
      break;
    case Instruction::ADD_DOUBLE:
    case Instruction::SUB_DOUBLE:
    case Instruction::MUL_DOUBLE:
    case Instruction::DIV_DOUBLE:
    case Instruction::REM_DOUBLE:
      CheckBinop(work_line, &dec_insn, kRegTypeDoubleLo, kRegTypeDoubleLo,
          kRegTypeDoubleLo, false, &failure);
      break;
    case Instruction::ADD_INT_2ADDR:
    case Instruction::SUB_INT_2ADDR:
    case Instruction::MUL_INT_2ADDR:
    case Instruction::REM_INT_2ADDR:
    case Instruction::SHL_INT_2ADDR:
    case Instruction::SHR_INT_2ADDR:
    case Instruction::USHR_INT_2ADDR:
      CheckBinop2addr(work_line, &dec_insn, kRegTypeInteger, kRegTypeInteger,
          kRegTypeInteger, false, &failure);
      break;
    case Instruction::AND_INT_2ADDR:
    case Instruction::OR_INT_2ADDR:
    case Instruction::XOR_INT_2ADDR:
      CheckBinop2addr(work_line, &dec_insn, kRegTypeInteger, kRegTypeInteger,
          kRegTypeInteger, true, &failure);
      break;
    case Instruction::DIV_INT_2ADDR:
      CheckBinop2addr(work_line, &dec_insn, kRegTypeInteger, kRegTypeInteger,
          kRegTypeInteger, false, &failure);
      break;
    case Instruction::ADD_LONG_2ADDR:
    case Instruction::SUB_LONG_2ADDR:
    case Instruction::MUL_LONG_2ADDR:
    case Instruction::DIV_LONG_2ADDR:
    case Instruction::REM_LONG_2ADDR:
    case Instruction::AND_LONG_2ADDR:
    case Instruction::OR_LONG_2ADDR:
    case Instruction::XOR_LONG_2ADDR:
      CheckBinop2addr(work_line, &dec_insn, kRegTypeLongLo, kRegTypeLongLo,
          kRegTypeLongLo, false, &failure);
      break;
    case Instruction::SHL_LONG_2ADDR:
    case Instruction::SHR_LONG_2ADDR:
    case Instruction::USHR_LONG_2ADDR:
      CheckBinop2addr(work_line, &dec_insn, kRegTypeLongLo, kRegTypeLongLo,
          kRegTypeInteger, false, &failure);
      break;
    case Instruction::ADD_FLOAT_2ADDR:
    case Instruction::SUB_FLOAT_2ADDR:
    case Instruction::MUL_FLOAT_2ADDR:
    case Instruction::DIV_FLOAT_2ADDR:
    case Instruction::REM_FLOAT_2ADDR:
      CheckBinop2addr(work_line, &dec_insn, kRegTypeFloat, kRegTypeFloat,
          kRegTypeFloat, false, &failure);
      break;
    case Instruction::ADD_DOUBLE_2ADDR:
    case Instruction::SUB_DOUBLE_2ADDR:
    case Instruction::MUL_DOUBLE_2ADDR:
    case Instruction::DIV_DOUBLE_2ADDR:
    case Instruction::REM_DOUBLE_2ADDR:
      CheckBinop2addr(work_line, &dec_insn, kRegTypeDoubleLo, kRegTypeDoubleLo,
          kRegTypeDoubleLo, false, &failure);
      break;
    case Instruction::ADD_INT_LIT16:
    case Instruction::RSUB_INT:
    case Instruction::MUL_INT_LIT16:
    case Instruction::DIV_INT_LIT16:
    case Instruction::REM_INT_LIT16:
      CheckLitop(work_line, &dec_insn, kRegTypeInteger, kRegTypeInteger, false,
          &failure);
      break;
    case Instruction::AND_INT_LIT16:
    case Instruction::OR_INT_LIT16:
    case Instruction::XOR_INT_LIT16:
      CheckLitop(work_line, &dec_insn, kRegTypeInteger, kRegTypeInteger, true,
          &failure);
      break;
    case Instruction::ADD_INT_LIT8:
    case Instruction::RSUB_INT_LIT8:
    case Instruction::MUL_INT_LIT8:
    case Instruction::DIV_INT_LIT8:
    case Instruction::REM_INT_LIT8:
    case Instruction::SHL_INT_LIT8:
      CheckLitop(work_line, &dec_insn, kRegTypeInteger, kRegTypeInteger, false,
          &failure);
      break;
    case Instruction::SHR_INT_LIT8:
      tmp_type = AdjustForRightShift(work_line, dec_insn.vB_, dec_insn.vC_,
          false);
      CheckLitop(work_line, &dec_insn, tmp_type, kRegTypeInteger, false,
          &failure);
      break;
    case Instruction::USHR_INT_LIT8:
      tmp_type = AdjustForRightShift(work_line, dec_insn.vB_, dec_insn.vC_,
          true);
      CheckLitop(work_line, &dec_insn, tmp_type, kRegTypeInteger, false,
          &failure);
      break;
    case Instruction::AND_INT_LIT8:
    case Instruction::OR_INT_LIT8:
    case Instruction::XOR_INT_LIT8:
      CheckLitop(work_line, &dec_insn, kRegTypeInteger, kRegTypeInteger, true,
          &failure);
      break;

    /*
     * This falls into the general category of "optimized" instructions,
     * which don't generally appear during verification. Because it's
     * inserted in the course of verification, we can expect to see it here.
     */
    case Instruction::THROW_VERIFICATION_ERROR:
      break;

    /*
     * Verifying "quickened" instructions is tricky, because we have
     * discarded the original field/method information. The byte offsets
     * and vtable indices only have meaning in the context of an object
     * instance.
     *
     * If a piece of code declares a local reference variable, assigns
     * null to it, and then issues a virtual method call on it, we
     * cannot evaluate the method call during verification. This situation
     * isn't hard to handle, since we know the call will always result in an
     * NPE, and the arguments and return value don't matter. Any code that
     * depends on the result of the method call is inaccessible, so the
     * fact that we can't fully verify anything that comes after the bad
     * call is not a problem.
     *
     * We must also consider the case of multiple code paths, only some of
     * which involve a null reference. We can completely verify the method
     * if we sidestep the results of executing with a null reference.
     * For example, if on the first pass through the code we try to do a
     * virtual method invocation through a null ref, we have to skip the
     * method checks and have the method return a "wildcard" type (which
     * merges with anything to become that other thing). The move-result
     * will tell us if it's a reference, single-word numeric, or double-word
     * value. We continue to perform the verification, and at the end of
     * the function any invocations that were never fully exercised are
     * marked as null-only.
     *
     * We would do something similar for the field accesses. The field's
     * type, once known, can be used to recover the width of short integers.
     * If the object reference was null, the field-get returns the "wildcard"
     * type, which is acceptable for any operation.
     */
    case Instruction::UNUSED_EE:
    case Instruction::UNUSED_EF:
    case Instruction::UNUSED_F2:
    case Instruction::UNUSED_F3:
    case Instruction::UNUSED_F4:
    case Instruction::UNUSED_F5:
    case Instruction::UNUSED_F6:
    case Instruction::UNUSED_F7:
    case Instruction::UNUSED_F8:
    case Instruction::UNUSED_F9:
    case Instruction::UNUSED_FA:
    case Instruction::UNUSED_FB:
    //case Instruction::EXECUTE_INLINE:
    //case Instruction::EXECUTE_INLINE_RANGE:
    //case Instruction::IGET_QUICK:
    //case Instruction::IGET_WIDE_QUICK:
    //case Instruction::IGET_OBJECT_QUICK:
    //case Instruction::IPUT_QUICK:
    //case Instruction::IPUT_WIDE_QUICK:
    //case Instruction::IPUT_OBJECT_QUICK:
    //case Instruction::INVOKE_VIRTUAL_QUICK:
    //case Instruction::INVOKE_VIRTUAL_QUICK_RANGE:
    //case Instruction::INVOKE_SUPER_QUICK:
    //case Instruction::INVOKE_SUPER_QUICK_RANGE:
      /* fall through to failure */

    /*
     * These instructions are equivalent (from the verifier's point of view)
     * to the original form. The change was made for correctness rather
     * than improved performance (except for invoke-object-init, which
     * provides both). The substitution takes place after verification
     * completes, though, so we don't expect to see them here.
     */
    case Instruction::UNUSED_F0:
    case Instruction::UNUSED_F1:
    case Instruction::UNUSED_E3:
    case Instruction::UNUSED_E8:
    case Instruction::UNUSED_E7:
    case Instruction::UNUSED_E4:
    case Instruction::UNUSED_E9:
    case Instruction::UNUSED_FC:
    case Instruction::UNUSED_E5:
    case Instruction::UNUSED_EA:
    case Instruction::UNUSED_FD:
    case Instruction::UNUSED_E6:
    case Instruction::UNUSED_EB:
    case Instruction::UNUSED_FE:
    //case Instruction::INVOKE_OBJECT_INIT_RANGE:
    //case Instruction::RETURN_VOID_BARRIER:
    //case Instruction::IGET_VOLATILE:
    //case Instruction::IGET_WIDE_VOLATILE:
    //case Instruction::IGET_OBJECT_VOLATILE:
    //case Instruction::IPUT_VOLATILE:
    //case Instruction::IPUT_WIDE_VOLATILE:
    //case Instruction::IPUT_OBJECT_VOLATILE:
    //case Instruction::SGET_VOLATILE:
    //case Instruction::SGET_WIDE_VOLATILE:
    //case Instruction::SGET_OBJECT_VOLATILE:
    //case Instruction::SPUT_VOLATILE:
    //case Instruction::SPUT_WIDE_VOLATILE:
    //case Instruction::SPUT_OBJECT_VOLATILE:
      /* fall through to failure */

    /* These should never appear during verification. */
    case Instruction::UNUSED_3E:
    case Instruction::UNUSED_3F:
    case Instruction::UNUSED_40:
    case Instruction::UNUSED_41:
    case Instruction::UNUSED_42:
    case Instruction::UNUSED_43:
    case Instruction::UNUSED_73:
    case Instruction::UNUSED_79:
    case Instruction::UNUSED_7A:
    case Instruction::UNUSED_EC:
    case Instruction::UNUSED_FF:
    //case Instruction::BREAKPOINT:
    //case Instruction::DISPATCH_FF:
      failure = VERIFY_ERROR_GENERIC;
      break;

    /*
     * DO NOT add a "default" clause here. Without it the compiler will
     * complain if an instruction is missing (which is desirable).
     */
    }

  if (failure != VERIFY_ERROR_NONE) {
    if (failure == VERIFY_ERROR_GENERIC) {
      /* immediate failure, reject class */
      LOG(ERROR) << "VFY:  rejecting opcode 0x" << std::hex
                 << (int) dec_insn.opcode_ << " at 0x" << insn_idx << std::dec;
      return false;
    } else {
      /* replace opcode and continue on */
      LOG(ERROR) << "VFY: replacing opcode 0x" << std::hex
                 << (int) dec_insn.opcode_ << " at 0x" << insn_idx << std::dec;
      if (!ReplaceFailingInstruction(code_item, insn_idx, failure)) {
        LOG(ERROR) << "VFY:  rejecting opcode 0x" << std::hex
                   << (int) dec_insn.opcode_ << " at 0x" << insn_idx
                   << std::dec;
        return false;
      }
      /* IMPORTANT: method->insns may have been changed */
      insns = code_item->insns_ + insn_idx;

      /* continue on as if we just handled a throw-verification-error */
      failure = VERIFY_ERROR_NONE;
      opcode_flag = Instruction::kThrow;
    }
  }

  /*
   * If we didn't just set the result register, clear it out. This
   * ensures that you can only use "move-result" immediately after the
   * result is set. (We could check this statically, but it's not
   * expensive and it makes our debugging output cleaner.)
   */
  if (!just_set_result) {
    int reg = RESULT_REGISTER(registers_size);
    SetRegisterType(work_line, reg, kRegTypeUnknown);
    SetRegisterType(work_line, reg + 1, kRegTypeUnknown);
  }

  /* Handle "continue". Tag the next consecutive instruction. */
  if ((opcode_flag & Instruction::kContinue) != 0) {
    size_t insn_width = InsnGetWidth(insn_flags, insn_idx);
    if (insn_idx + insn_width >= insns_size) {
      LOG(ERROR) << "VFY: execution can walk off end of code area (from 0x"
                 << std::hex << insn_idx << std::dec << ")";
      return false;
    }

    /*
     * The only way to get to a move-exception instruction is to get
     * thrown there. Make sure the next instruction isn't one.
     */
    if (!CheckMoveException(code_item->insns_, insn_idx + insn_width))
      return false;

    if (GetRegisterLine(reg_table, insn_idx + insn_width)->reg_types_.get() != NULL) {
      /*
       * Merge registers into what we have for the next instruction,
       * and set the "changed" flag if needed.
       */
      if (!UpdateRegisters(insn_flags, reg_table, insn_idx + insn_width,
          work_line))
        return false;
    } else {
      /*
       * We're not recording register data for the next instruction,
       * so we don't know what the prior state was. We have to
       * assume that something has changed and re-evaluate it.
       */
      InsnSetChanged(insn_flags, insn_idx + insn_width, true);
    }
  }

  /*
   * Handle "branch". Tag the branch target.
   *
   * NOTE: instructions like Instruction::EQZ provide information about the
   * state of the register when the branch is taken or not taken. For example,
   * somebody could get a reference field, check it for zero, and if the
   * branch is taken immediately store that register in a boolean field
   * since the value is known to be zero. We do not currently account for
   * that, and will reject the code.
   *
   * TODO: avoid re-fetching the branch target
   */
  if ((opcode_flag & Instruction::kBranch) != 0) {
    bool isConditional, selfOkay;

    if (!GetBranchOffset(code_item, insn_flags, insn_idx, &branch_target,
        &isConditional, &selfOkay)) {
      /* should never happen after static verification */
      LOG(ERROR) << "VFY: bad branch at 0x" << std::hex << insn_idx << std::dec;
      return false;
    }
    DCHECK_EQ(isConditional, (opcode_flag & Instruction::kContinue) != 0);

    if (!CheckMoveException(code_item->insns_, insn_idx + branch_target))
      return false;

    /* update branch target, set "changed" if appropriate */
    if (!UpdateRegisters(insn_flags, reg_table, insn_idx + branch_target,
        work_line))
      return false;
  }

  /*
   * Handle "switch". Tag all possible branch targets.
   *
   * We've already verified that the table is structurally sound, so we
   * just need to walk through and tag the targets.
   */
  if ((opcode_flag & Instruction::kSwitch) != 0) {
    int offset_to_switch = insns[1] | (((int32_t) insns[2]) << 16);
    const uint16_t* switch_insns = insns + offset_to_switch;
    int switch_count = switch_insns[1];
    int offset_to_targets, targ;

    if ((*insns & 0xff) == Instruction::PACKED_SWITCH) {
      /* 0 = sig, 1 = count, 2/3 = first key */
      offset_to_targets = 4;
    } else {
      /* 0 = sig, 1 = count, 2..count * 2 = keys */
      DCHECK((*insns & 0xff) == Instruction::SPARSE_SWITCH);
      offset_to_targets = 2 + 2 * switch_count;
    }

    /* verify each switch target */
    for (targ = 0; targ < switch_count; targ++) {
      int offset;
      uint32_t abs_offset;

      /* offsets are 32-bit, and only partly endian-swapped */
      offset = switch_insns[offset_to_targets + targ * 2] |
         (((int32_t) switch_insns[offset_to_targets + targ * 2 + 1]) << 16);
      abs_offset = insn_idx + offset;

      DCHECK_LT(abs_offset, insns_size);

      if (!CheckMoveException(code_item->insns_, abs_offset))
        return false;

      if (!UpdateRegisters(insn_flags, reg_table, abs_offset, work_line))
        return false;
    }
  }

  /*
   * Handle instructions that can throw and that are sitting in a
   * "try" block. (If they're not in a "try" block when they throw,
   * control transfers out of the method.)
   */
  if ((opcode_flag & Instruction::kThrow) != 0 &&
      InsnIsInTry(insn_flags, insn_idx)) {
    bool has_catch_all = false;
    DexFile::CatchHandlerIterator iterator = DexFile::dexFindCatchHandler(
        *code_item, insn_idx);

    for (; !iterator.HasNext(); iterator.Next()) {
      if (iterator.Get().type_idx_ == DexFile::kDexNoIndex)
        has_catch_all = true;

      /*
       * Merge registers into the "catch" block. We want to use the
       * "savedRegs" rather than "work_regs", because at runtime the
       * exception will be thrown before the instruction modifies any
       * registers.
       */
      if (!UpdateRegisters(insn_flags, reg_table, iterator.Get().address_,
          &reg_table->saved_line_))
        return false;
    }

    /*
     * If the monitor stack depth is nonzero, there must be a "catch all"
     * handler for this instruction. This does apply to monitor-exit
     * because of async exception handling.
     */
    if (work_line->monitor_stack_top_ != 0 && !has_catch_all) {
      /*
       * The state in work_line reflects the post-execution state.
       * If the current instruction is a monitor-enter and the monitor
       * stack was empty, we don't need a catch-all (if it throws,
       * it will do so before grabbing the lock).
       */
      if (!(dec_insn.opcode_ == Instruction::MONITOR_ENTER &&
          work_line->monitor_stack_top_ == 1))
      {
        LOG(ERROR) << "VFY: no catch-all for instruction at 0x" << std::hex
                   << insn_idx << std::dec;
        return false;
      }
    }
  }

  /* If we're returning from the method, make sure monitor stack is empty. */
  if ((opcode_flag & Instruction::kReturn) != 0 &&
      work_line->monitor_stack_top_ != 0) {
    LOG(ERROR) << "VFY: return with stack depth="
               << work_line->monitor_stack_top_ << " at 0x" << std::hex
               << insn_idx << std::dec;
    return false;
  }

  /*
   * Update start_guess. Advance to the next instruction of that's
   * possible, otherwise use the branch target if one was found. If
   * neither of those exists we're in a return or throw; leave start_guess
   * alone and let the caller sort it out.
   */
  if ((opcode_flag & Instruction::kContinue) != 0) {
    *start_guess = insn_idx + InsnGetWidth(insn_flags, insn_idx);
  } else if ((opcode_flag & Instruction::kBranch) != 0) {
    /* we're still okay if branch_target is zero */
    *start_guess = insn_idx + branch_target;
  }

  DCHECK_LT(*start_guess, insns_size);
  DCHECK_NE(InsnGetWidth(insn_flags, *start_guess), 0);

  return true;
}

bool DexVerifier::ReplaceFailingInstruction(const DexFile::CodeItem* code_item,
    int insn_idx, VerifyError failure) {
  const uint16_t* insns = code_item->insns_ + insn_idx;
  const byte* ptr = reinterpret_cast<const byte*>(insns);
  const Instruction* inst = Instruction::At(ptr);
  Instruction::Code opcode = inst->Opcode();
  VerifyErrorRefType ref_type;

  /*
   * Generate the new instruction out of the old.
   *
   * First, make sure this is an instruction we're expecting to stomp on.
   */
  switch (opcode) {
    case Instruction::CONST_CLASS:            // insn[1] == class ref, 2 bytes
    case Instruction::CHECK_CAST:
    case Instruction::INSTANCE_OF:
    case Instruction::NEW_INSTANCE:
    case Instruction::NEW_ARRAY:
    case Instruction::FILLED_NEW_ARRAY:       // insn[1] == class ref, 3 bytes
    case Instruction::FILLED_NEW_ARRAY_RANGE:
      ref_type = VERIFY_ERROR_REF_CLASS;
      break;

    case Instruction::IGET:                   // insn[1] == field ref, 2 bytes
    case Instruction::IGET_BOOLEAN:
    case Instruction::IGET_BYTE:
    case Instruction::IGET_CHAR:
    case Instruction::IGET_SHORT:
    case Instruction::IGET_WIDE:
    case Instruction::IGET_OBJECT:
    case Instruction::IPUT:
    case Instruction::IPUT_BOOLEAN:
    case Instruction::IPUT_BYTE:
    case Instruction::IPUT_CHAR:
    case Instruction::IPUT_SHORT:
    case Instruction::IPUT_WIDE:
    case Instruction::IPUT_OBJECT:
    case Instruction::SGET:
    case Instruction::SGET_BOOLEAN:
    case Instruction::SGET_BYTE:
    case Instruction::SGET_CHAR:
    case Instruction::SGET_SHORT:
    case Instruction::SGET_WIDE:
    case Instruction::SGET_OBJECT:
    case Instruction::SPUT:
    case Instruction::SPUT_BOOLEAN:
    case Instruction::SPUT_BYTE:
    case Instruction::SPUT_CHAR:
    case Instruction::SPUT_SHORT:
    case Instruction::SPUT_WIDE:
    case Instruction::SPUT_OBJECT:
      ref_type = VERIFY_ERROR_REF_FIELD;
      break;

    case Instruction::INVOKE_VIRTUAL:         // insn[1] == method ref, 3 bytes
    case Instruction::INVOKE_VIRTUAL_RANGE:
    case Instruction::INVOKE_SUPER:
    case Instruction::INVOKE_SUPER_RANGE:
    case Instruction::INVOKE_DIRECT:
    case Instruction::INVOKE_DIRECT_RANGE:
    case Instruction::INVOKE_STATIC:
    case Instruction::INVOKE_STATIC_RANGE:
    case Instruction::INVOKE_INTERFACE:
    case Instruction::INVOKE_INTERFACE_RANGE:
      ref_type = VERIFY_ERROR_REF_METHOD;
      break;

    default:
      /* could handle this in a generic way, but this is probably safer */
      LOG(ERROR) << "GLITCH: verifier asked to replace opcode 0x" << std::hex
                 << (int) opcode << std::dec;
      return false;
  }

  DCHECK(inst->IsThrow());

  /* write a NOP over the third code unit, if necessary */
  int width = inst->Size();
  switch (width) {
    case 2:
      /* nothing to do */
      break;
    case 3:
      UpdateCodeUnit(insns + 2, Instruction::NOP);
      break;
    default:
      /* whoops */
      LOG(FATAL) << "ERROR: stomped a " << width
                 << "-unit instruction with a verifier error";
  }

  /* encode the opcode, with the failure code in the high byte */
  DCHECK(width == 2 || width == 3);
  uint16_t new_val = Instruction::THROW_VERIFICATION_ERROR |
      (failure << 8) | (ref_type << (8 + kVerifyErrorRefTypeShift));
  UpdateCodeUnit(insns, new_val);

  return true;
}

void DexVerifier::UpdateCodeUnit(const uint16_t* ptr, uint16_t new_val) {
  *(uint16_t*) ptr = new_val;
}

void DexVerifier::HandleMonitorEnter(RegisterLine* work_line, uint32_t reg_idx,
    uint32_t insn_idx, VerifyError* failure) {
  if (!RegTypeIsReference(GetRegisterType(work_line, reg_idx))) {
    LOG(ERROR) << "VFY: monitor-enter on non-object";
    *failure = VERIFY_ERROR_GENERIC;
    return;
  }

  if (work_line->monitor_entries_.get() == NULL) {
    return;
  }

  if (work_line->monitor_stack_top_ == kMaxMonitorStackDepth) {
    LOG(ERROR) << "VFY: monitor-enter stack overflow (" << kMaxMonitorStackDepth
               << ")";
    *failure = VERIFY_ERROR_GENERIC;
    return;
  }

  /*
   * Push an entry on the stack, and set a bit in the register flags to
   * indicate that it's associated with this register.
   */
  work_line->monitor_entries_[reg_idx] |= 1 << work_line->monitor_stack_top_;
  work_line->monitor_stack_[work_line->monitor_stack_top_++] = insn_idx;
}

void DexVerifier::HandleMonitorExit(RegisterLine* work_line, uint32_t reg_idx,
    uint32_t insn_idx, VerifyError* failure) {
  if (!RegTypeIsReference(GetRegisterType(work_line, reg_idx))) {
    LOG(ERROR) << "VFY: monitor-exit on non-object";
    *failure = VERIFY_ERROR_GENERIC;
    return;
  }

  if (work_line->monitor_entries_.get() == NULL) {
    return;
  }

  if (work_line->monitor_stack_top_ == 0) {
    LOG(ERROR) << "VFY: monitor-exit stack underflow";
    *failure = VERIFY_ERROR_GENERIC;
    return;
  }

  /*
   * Confirm that the entry at the top of the stack is associated with
   * the register. Pop the top entry off.
   */
  work_line->monitor_stack_top_--;
#ifdef BUG_3215458_FIXED
  /*
   * TODO: This code can safely be enabled if know we are working on
   * a dex file of format version 036 or later. (That is, we'll need to
   * add a check for the version number.)
   */
  if ((work_line->monitor_entries_[reg_idx] &
      (1 << work_line->monitor_stack_top_)) == 0) {
    LOG(ERROR) << "VFY: monitor-exit bit " << work_line->monitor_stack_top_
               << " not set: addr=0x" << std::hex << insn_idx << std::dec
               << " (bits[" << reg_idx << "]=" << std::hex
               << work_line->monitor_entries_[reg_idx] << std::dec << ")";
    *failure = VERIFY_ERROR_GENERIC;
    return;
  }
#endif
  work_line->monitor_stack_[work_line->monitor_stack_top_] = 0;

  /* Clear the bit from the register flags. */
  work_line->monitor_entries_[reg_idx] &= ~(1 << work_line->monitor_stack_top_);
}

Field* DexVerifier::GetInstField(VerifierData* vdata, RegType obj_type,
    int field_idx, VerifyError* failure) {
  Method* method = vdata->method_;
  const DexFile* dex_file = vdata->dex_file_;
  UninitInstanceMap* uninit_map = vdata->uninit_map_.get();
  bool must_be_local = false;

  if (!RegTypeIsReference(obj_type)) {
    LOG(ERROR) << "VFY: attempt to access field in non-reference type "
               << obj_type;
    *failure = VERIFY_ERROR_GENERIC;
    return NULL;
  }

  Field* field = ResolveFieldAndCheckAccess(dex_file, field_idx,
      method->GetDeclaringClass(), failure, false);
  if (field == NULL) {
    LOG(ERROR) << "VFY: unable to resolve instance field " << field_idx;
    return field;
  }

  if (obj_type == kRegTypeZero)
    return field;

  /*
   * Access to fields in uninitialized objects is allowed if this is
   * the <init> method for the object and the field in question is
   * declared by this class.
   */
  Class* obj_class = RegTypeReferenceToClass(obj_type, uninit_map);
  DCHECK(obj_class != NULL);
  if (RegTypeIsUninitReference(obj_type)) {
    if (!IsInitMethod(method) || method->GetDeclaringClass() != obj_class) {
      LOG(ERROR) << "VFY: attempt to access field via uninitialized ref";
      *failure = VERIFY_ERROR_GENERIC;
      return field;
    }
    must_be_local = true;
  }

  if (!field->GetDeclaringClass()->IsAssignableFrom(obj_class)) {
    LOG(ERROR) << "VFY: invalid field access (field "
               << field->GetDeclaringClass()->GetDescriptor()->ToModifiedUtf8()
               << "." << field->GetName()->ToModifiedUtf8() << ", through "
               << obj_class->GetDescriptor()->ToModifiedUtf8() << " ref)";
    *failure = VERIFY_ERROR_NO_FIELD;
    return field;
  }

  if (must_be_local) {
    bool found = false;
    /* for uninit ref, make sure it's defined by this class, not super */
    for (uint32_t i = 0; i < obj_class->NumInstanceFields(); i++) {
      found |= (field == obj_class->GetInstanceField(i));
    }
    if (!found) {
      LOG(ERROR) << "VFY: invalid constructor field access (field "
                 << field->GetName()->ToModifiedUtf8() << " in "
                 << obj_class->GetDescriptor()->ToModifiedUtf8() << ")";
      *failure = VERIFY_ERROR_GENERIC;
      return field;
    }
  }

  return field;
}

Field* DexVerifier::GetStaticField(VerifierData* vdata, int field_idx,
    VerifyError* failure) {
  Method* method = vdata->method_;
  const DexFile* dex_file = vdata->dex_file_;
  Field* field = ResolveFieldAndCheckAccess(dex_file, field_idx,
      method->GetDeclaringClass(), failure, true);
  if (field == NULL) {
    const DexFile::FieldId& field_id = dex_file->GetFieldId(field_idx);
    LOG(ERROR) << "VFY: unable to resolve static field " << field_idx << " ("
               << dex_file->GetFieldName(field_id) << ") in "
               << dex_file->GetFieldClassDescriptor(field_id);
    *failure = VERIFY_ERROR_NO_FIELD;
  }

  return field;
}

Class* DexVerifier::GetCaughtExceptionType(VerifierData* vdata, int insn_idx,
    VerifyError* failure) {
  const DexFile* dex_file = vdata->dex_file_;
  const DexFile::CodeItem* code_item = vdata->code_item_;
  Method* method = vdata->method_;
  Class* common_super = NULL;
  uint32_t handlers_size;
  const byte* handlers_ptr = DexFile::dexGetCatchHandlerData(*code_item, 0);

  if (code_item->tries_size_ != 0) {
    handlers_size = DecodeUnsignedLeb128(&handlers_ptr);
  } else {
    handlers_size = 0;
  }

  for (uint32_t i = 0; i < handlers_size; i++) {
    DexFile::CatchHandlerIterator iterator(handlers_ptr);

    for (; !iterator.HasNext(); iterator.Next()) {
      DexFile::CatchHandlerItem handler = iterator.Get();
      if (handler.address_ == (uint32_t) insn_idx) {
        ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
        Class* klass;

        if (handler.type_idx_ == DexFile::kDexNoIndex) {
          klass = class_linker->FindSystemClass("Ljava/lang/Throwable;");
        } else {
          klass = ResolveClassAndCheckAccess(dex_file, handler.type_idx_,
              method->GetDeclaringClass(), failure);
        }

        if (klass == NULL) {
          LOG(ERROR) << "VFY: unable to resolve exception class "
                     << handler.type_idx_ << " ("
                     << dex_file->dexStringByTypeIdx(handler.type_idx_) << ")";
          /* TODO: do we want to keep going?  If we don't fail this we run
           * the risk of having a non-Throwable introduced at runtime.
           * However, that won't pass an instanceof test, so is essentially
           * harmless.
           */
        } else {
          if (common_super == NULL)
            common_super = klass;
          else
            common_super = FindCommonSuperclass(klass, common_super);
        }
      }
    }

    handlers_ptr = iterator.GetData();
  }

  if (common_super == NULL) {
    /* no catch blocks, or no catches with classes we can find */
    LOG(ERROR) << "VFY: unable to find exception handler at addr 0x" << std::hex
               << insn_idx << std::dec;
    *failure = VERIFY_ERROR_GENERIC;
  }

  return common_super;
}

DexVerifier::RegType DexVerifier::GetMethodReturnType(const DexFile* dex_file,
    const Method* method) {
  Class* klass = method->GetReturnType();
  if (klass->IsPrimitive())
    return PrimitiveTypeToRegType(klass->GetPrimitiveType());
  else
    return RegTypeFromClass(klass);
}

Class* DexVerifier::GetClassFromRegister(const RegisterLine* register_line,
    uint32_t vsrc, VerifyError* failure) {
  /* get the element type of the array held in vsrc */
  RegType type = GetRegisterType(register_line, vsrc);

  /* if "always zero", we allow it to fail at runtime */
  if (type == kRegTypeZero)
    return NULL;

  if (!RegTypeIsReference(type)) {
    LOG(ERROR) << "VFY: tried to get class from non-ref register v" << vsrc
               << " (type=" << type << ")",
    *failure = VERIFY_ERROR_GENERIC;
    return NULL;
  }
  if (RegTypeIsUninitReference(type)) {
    LOG(ERROR) << "VFY: register " << vsrc << " holds uninitialized reference";
    *failure = VERIFY_ERROR_GENERIC;
    return NULL;
  }

  return RegTypeInitializedReferenceToClass(type);
}

DexVerifier::RegType DexVerifier::GetInvocationThis(
    const RegisterLine* register_line,
    const Instruction::DecodedInstruction* dec_insn, VerifyError* failure) {
  if (dec_insn->vA_ < 1) {
    LOG(ERROR) << "VFY: invoke lacks 'this'";
    *failure = VERIFY_ERROR_GENERIC;
    return kRegTypeUnknown;
  }

  /* get the element type of the array held in vsrc */
  RegType this_type = GetRegisterType(register_line, dec_insn->vC_);
  if (!RegTypeIsReference(this_type)) {
    LOG(ERROR) << "VFY: tried to get class from non-ref register v"
               << dec_insn->vC_ << " (type=" << this_type << ")";
    *failure = VERIFY_ERROR_GENERIC;
    return kRegTypeUnknown;
  }

  return this_type;
}

void DexVerifier::SetRegisterType(RegisterLine* register_line, uint32_t vdst,
    RegType new_type) {
  RegType* insn_regs = register_line->reg_types_.get();

  switch (new_type) {
    case kRegTypeUnknown:
    case kRegTypeBoolean:
    case kRegTypeOne:
    case kRegTypeConstByte:
    case kRegTypeConstPosByte:
    case kRegTypeConstShort:
    case kRegTypeConstPosShort:
    case kRegTypeConstChar:
    case kRegTypeConstInteger:
    case kRegTypeByte:
    case kRegTypePosByte:
    case kRegTypeShort:
    case kRegTypePosShort:
    case kRegTypeChar:
    case kRegTypeInteger:
    case kRegTypeFloat:
    case kRegTypeZero:
    case kRegTypeUninit:
      insn_regs[vdst] = new_type;
      break;
    case kRegTypeConstLo:
    case kRegTypeLongLo:
    case kRegTypeDoubleLo:
      insn_regs[vdst] = new_type;
      insn_regs[vdst + 1] = new_type + 1;
      break;
    case kRegTypeConstHi:
    case kRegTypeLongHi:
    case kRegTypeDoubleHi:
      /* should never set these explicitly */
      LOG(FATAL) << "BUG: explicit set of high register type";
      break;

    default:
      /* can't switch for ref types, so we check explicitly */
      if (RegTypeIsReference(new_type)) {
        insn_regs[vdst] = new_type;

        /*
         * In most circumstances we won't see a reference to a primitive
         * class here (e.g. "D"), since that would mean the object in the
         * register is actually a primitive type. It can happen as the
         * result of an assumed-successful check-cast instruction in
         * which the second argument refers to a primitive class. (In
         * practice, such an instruction will always throw an exception.)
         *
         * This is not an issue for instructions like const-class, where
         * the object in the register is a java.lang.Class instance.
         */
        break;
      }
      /* bad type - fall through */

    case kRegTypeConflict:      // should only be set during a merge
      LOG(FATAL) << "BUG: set register to unknown type " << new_type;
      break;
  }

  /*
   * Clear the monitor entry bits for this register.
   */
  if (register_line->monitor_entries_.get() != NULL)
    register_line->monitor_entries_[vdst] = 0;
}

void DexVerifier::VerifyRegisterType(RegisterLine* register_line, uint32_t vsrc,
    RegType check_type, VerifyError* failure) {
  const RegType* insn_regs = register_line->reg_types_.get();
  RegType src_type = insn_regs[vsrc];

  switch (check_type) {
    case kRegTypeFloat:
    case kRegTypeBoolean:
    case kRegTypePosByte:
    case kRegTypeByte:
    case kRegTypePosShort:
    case kRegTypeShort:
    case kRegTypeChar:
    case kRegTypeInteger:
      if (!CanConvertTo1nr(src_type, check_type)) {
        LOG(ERROR) << "VFY: register1 v" << vsrc << " type " << src_type
                   << ", wanted " << check_type;
        *failure = VERIFY_ERROR_GENERIC;
      }
      /* Update type if result is float */
      if (check_type == kRegTypeFloat) {
        SetRegisterType(register_line, vsrc, check_type);
      } else {
        /* Update const type to actual type after use */
        SetRegisterType(register_line, vsrc, ConstTypeToRegType(src_type));
      }
      break;
    case kRegTypeLongLo:
    case kRegTypeDoubleLo:
      if (insn_regs[vsrc + 1] != src_type + 1) {
        LOG(ERROR) << "VFY: register2 v" << vsrc << "-" << vsrc + 1
                   << " values " << insn_regs[vsrc] << ","
                   << insn_regs[vsrc + 1];
        *failure = VERIFY_ERROR_GENERIC;
      } else if (!CanConvertTo2(src_type, check_type)) {
        LOG(ERROR) << "VFY: register2 v" << vsrc << " type " << src_type
                   << ", wanted " << check_type;
        *failure = VERIFY_ERROR_GENERIC;
      }
      /* Update type if source is from const */
      if (src_type == kRegTypeConstLo) {
        SetRegisterType(register_line, vsrc, check_type);
      }
      break;
    case kRegTypeConstLo:
    case kRegTypeConstHi:
    case kRegTypeLongHi:
    case kRegTypeDoubleHi:
    case kRegTypeZero:
    case kRegTypeOne:
    case kRegTypeUnknown:
    case kRegTypeConflict:
      /* should never be checking for these explicitly */
      DCHECK(false);
      *failure = VERIFY_ERROR_GENERIC;
      return;
    case kRegTypeUninit:
    default:
      /* make sure check_type is initialized reference */
      if (!RegTypeIsReference(check_type)) {
        LOG(FATAL) << "VFY: unexpected check type " << check_type;
        *failure = VERIFY_ERROR_GENERIC;
        break;
      }
      if (RegTypeIsUninitReference(check_type)) {
        LOG(ERROR) << "VFY: uninitialized ref not expected as reg check";
        *failure = VERIFY_ERROR_GENERIC;
        break;
      }
      /* make sure src_type is initialized reference or always-NULL */
      if (!RegTypeIsReference(src_type)) {
        LOG(ERROR) << "VFY: register1 v" << vsrc << " type " << src_type
                   << ", wanted ref";
        *failure = VERIFY_ERROR_GENERIC;
        break;
      }
      if (RegTypeIsUninitReference(src_type)) {
        LOG(ERROR) << "VFY: register1 v" << vsrc << " holds uninitialized ref";
        *failure = VERIFY_ERROR_GENERIC;
        break;
      }
      /* if the register isn't Zero, make sure it's an instance of check */
      if (src_type != kRegTypeZero) {
        Class* src_class = RegTypeInitializedReferenceToClass(src_type);
        Class* check_class = RegTypeInitializedReferenceToClass(check_type);
        DCHECK(src_class != NULL);
        DCHECK(check_class != NULL);

        if (!check_class->IsAssignableFrom(src_class)) {
          LOG(ERROR) << "VFY: " << src_class->GetDescriptor()->ToModifiedUtf8()
                     << " is not instance of "
                     << check_class->GetDescriptor()->ToModifiedUtf8();
          *failure = VERIFY_ERROR_GENERIC;
        }
      }
      break;
  }
}

void DexVerifier::SetResultRegisterType(RegisterLine* register_line,
    const int insn_reg_count, RegType new_type) {
  SetRegisterType(register_line, RESULT_REGISTER(insn_reg_count), new_type);
}

void DexVerifier::MarkRefsAsInitialized(RegisterLine* register_line,
    int insn_reg_count, UninitInstanceMap* uninit_map, RegType uninit_type,
    VerifyError* failure) {
  RegType* insn_regs = register_line->reg_types_.get();
  Class* klass = GetUninitInstance(uninit_map,
      RegTypeToUninitIndex(uninit_type));

  if (klass == NULL) {
    LOG(ERROR) << "VFY: unable to find type=" << std::hex << uninit_type
               << std::dec << " (idx=" << RegTypeToUninitIndex(uninit_type)
               << ")";
    *failure = VERIFY_ERROR_GENERIC;
    return;
  }

  RegType init_type = RegTypeFromClass(klass);
  int changed = 0;
  for (int i = 0; i < insn_reg_count; i++) {
    if (insn_regs[i] == uninit_type) {
      insn_regs[i] = init_type;
      changed++;
    }
  }
  DCHECK_GT(changed, 0);

  return;
}

void DexVerifier::MarkUninitRefsAsInvalid(RegisterLine* register_line,
    int insn_reg_count, UninitInstanceMap* uninit_map, RegType uninit_type) {
  RegType* insn_regs = register_line->reg_types_.get();

  for (int i = 0; i < insn_reg_count; i++) {
    if (insn_regs[i] == uninit_type) {
      insn_regs[i] = kRegTypeConflict;
      if (register_line->monitor_entries_.get() != NULL)
        register_line->monitor_entries_[i] = 0;
    }
  }
}

void DexVerifier::CopyRegister1(RegisterLine* register_line, uint32_t vdst,
    uint32_t vsrc, TypeCategory cat, VerifyError* failure) {
  DCHECK(cat == kTypeCategory1nr || cat == kTypeCategoryRef);
  RegType type = GetRegisterType(register_line, vsrc);
  CheckTypeCategory(type, cat, failure);
  if (*failure != VERIFY_ERROR_NONE) {
    LOG(ERROR) << "VFY: copy1 v" << vdst << "<-v" << vsrc << " type=" << type
               << " cat=" << (int) cat;
  } else {
    SetRegisterType(register_line, vdst, type);
    if (cat == kTypeCategoryRef && register_line->monitor_entries_.get() != NULL) {
      register_line->monitor_entries_[vdst] =
          register_line->monitor_entries_[vsrc];
    }
  }
}

void DexVerifier::CopyRegister2(RegisterLine* register_line, uint32_t vdst,
    uint32_t vsrc, VerifyError* failure) {
  RegType type_l = GetRegisterType(register_line, vsrc);
  RegType type_h = GetRegisterType(register_line, vsrc + 1);

  CheckTypeCategory(type_l, kTypeCategory2, failure);
  CheckWidePair(type_l, type_h, failure);
  if (*failure != VERIFY_ERROR_NONE) {
    LOG(ERROR) << "VFY: copy2 v" << vdst << "<-v" << vsrc << " type=" << type_l
               << "/" << type_h;
  } else {
    SetRegisterType(register_line, vdst, type_l);
  }
}

void DexVerifier::CopyResultRegister1(RegisterLine* register_line,
    const int insn_reg_count, uint32_t vdst, TypeCategory cat,
    VerifyError* failure) {
  DCHECK_LT(vdst, static_cast<uint32_t>(insn_reg_count));

  uint32_t vsrc = RESULT_REGISTER(insn_reg_count);
  RegType type = GetRegisterType(register_line, vsrc);
  CheckTypeCategory(type, cat, failure);
  if (*failure != VERIFY_ERROR_NONE) {
    LOG(ERROR) << "VFY: copyRes1 v" << vdst << "<-v" << vsrc << " cat="
               << (int) cat << " type=" << type;
  } else {
    SetRegisterType(register_line, vdst, type);
    SetRegisterType(register_line, vsrc, kRegTypeUnknown);
  }
}

/*
 * Implement "move-result-wide". Copy the category-2 value from the result
 * register to another register, and reset the result register.
 */
void DexVerifier::CopyResultRegister2(RegisterLine* register_line,
    const int insn_reg_count, uint32_t vdst, VerifyError* failure) {
  DCHECK_LT(vdst, static_cast<uint32_t>(insn_reg_count));

  uint32_t vsrc = RESULT_REGISTER(insn_reg_count);
  RegType type_l = GetRegisterType(register_line, vsrc);
  RegType type_h = GetRegisterType(register_line, vsrc + 1);
  CheckTypeCategory(type_l, kTypeCategory2, failure);
  CheckWidePair(type_l, type_h, failure);
  if (*failure != VERIFY_ERROR_NONE) {
    LOG(ERROR) << "VFY: copyRes2 v" << vdst << "<-v" << vsrc << " type="
               << type_l << "/" << type_h;
  } else {
    SetRegisterType(register_line, vdst, type_l);
    SetRegisterType(register_line, vsrc, kRegTypeUnknown);
    SetRegisterType(register_line, vsrc + 1, kRegTypeUnknown);
  }
}

int DexVerifier::GetClassDepth(Class* klass) {
  int depth = 0;
  while (klass->GetSuperClass() != NULL) {
    klass = klass->GetSuperClass();
    depth++;
  }
  return depth;
}

Class* DexVerifier::DigForSuperclass(Class* c1, Class* c2) {
  int depth1, depth2;

  depth1 = GetClassDepth(c1);
  depth2 = GetClassDepth(c2);

  /* pull the deepest one up */
  if (depth1 > depth2) {
    while (depth1 > depth2) {
      c1 = c1->GetSuperClass();
      depth1--;
    }
  } else {
    while (depth2 > depth1) {
      c2 = c2->GetSuperClass();
      depth2--;
    }
  }

  /* walk up in lock-step */
  while (c1 != c2) {
    c1 = c1->GetSuperClass();
    c2 = c2->GetSuperClass();
    DCHECK(c1 != NULL);
    DCHECK(c2 != NULL);
  }

  return c1;
}

Class* DexVerifier::FindCommonArraySuperclass(Class* c1, Class* c2) {
  DCHECK(c1->IsArrayClass());
  DCHECK(c2->IsArrayClass());
  Class* e1 = c1->GetComponentType();
  Class* e2 = c2->GetComponentType();
  if (e1->IsPrimitive() || e2->IsPrimitive()) {
    return c1->GetSuperClass();  // == java.lang.Object
  }
  Class* common_elem = FindCommonSuperclass(c1->GetComponentType(), c2->GetComponentType());
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  const ClassLoader* class_loader = c1->GetClassLoader();
  std::string descriptor = "[" + common_elem->GetDescriptor()->ToModifiedUtf8();
  Class* array_class = class_linker->FindClass(descriptor.c_str(), class_loader);
  DCHECK(array_class != NULL);
  return array_class;
}

Class* DexVerifier::FindCommonSuperclass(Class* c1, Class* c2) {
  DCHECK(!c1->IsPrimitive()) << PrettyClass(c1);
  DCHECK(!c2->IsPrimitive()) << PrettyClass(c2);

  if (c1 == c2)
    return c1;

  if (c1->IsInterface() && c1->IsAssignableFrom(c2)) {
    return c1;
  }
  if (c2->IsInterface() && c2->IsAssignableFrom(c1)) {
    return c2;
  }
  if (c1->IsArrayClass() && c2->IsArrayClass()) {
    return FindCommonArraySuperclass(c1, c2);
  }

  return DigForSuperclass(c1, c2);
}

Class* DexVerifier::ResolveClassAndCheckAccess(const DexFile* dex_file,
    uint32_t class_idx, const Class* referrer, VerifyError* failure) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  Class* res_class = class_linker->ResolveType(*dex_file, class_idx, referrer);

  if (res_class == NULL) {
    *failure = VERIFY_ERROR_NO_CLASS;
    return NULL;
  }

  /* Check if access is allowed. */
  if (!referrer->CanAccess(res_class)) {
    LOG(ERROR) << "VFY: illegal class access: "
               << referrer->GetDescriptor()->ToModifiedUtf8() << " -> "
               << res_class->GetDescriptor()->ToModifiedUtf8();
    *failure = VERIFY_ERROR_ACCESS_CLASS;
    return NULL;
  }

  return res_class;
}

Method* DexVerifier::ResolveMethodAndCheckAccess(const DexFile* dex_file,
    uint32_t method_idx, const Class* referrer, VerifyError* failure,
    bool is_direct) {
  DexCache* dex_cache = referrer->GetDexCache();
  Method* res_method = dex_cache->GetResolvedMethod(method_idx);

  if (res_method == NULL) {
    const DexFile::MethodId& method_id = dex_file->GetMethodId(method_idx);
    Class* klass = ResolveClassAndCheckAccess(dex_file, method_id.class_idx_, referrer, failure);
    if (klass == NULL) {
      DCHECK(*failure != VERIFY_ERROR_NONE);
      return NULL;
    }

    const char* name = dex_file->dexStringById(method_id.name_idx_);
    std::string signature(dex_file->CreateMethodDescriptor(method_id.proto_idx_, NULL));
    if (is_direct) {
      res_method = klass->FindDirectMethod(name, signature);
    } else if (klass->IsInterface()) {
      res_method = klass->FindInterfaceMethod(name, signature);
    } else {
      res_method = klass->FindVirtualMethod(name, signature);
    }
    if (res_method != NULL) {
      dex_cache->SetResolvedMethod(method_idx, res_method);
    } else {
      LOG(ERROR) << "VFY: couldn't find method "
                 << klass->GetDescriptor()->ToModifiedUtf8() << "." << name
                 << " " << signature;
      *failure = VERIFY_ERROR_NO_METHOD;
      return NULL;
    }
  }

  /* Check if access is allowed. */
  if (!referrer->CanAccess(res_method->GetDeclaringClass())) {
    LOG(ERROR) << "VFY: illegal method access (call "
               << res_method->GetDeclaringClass()->GetDescriptor()->ToModifiedUtf8()
               << "." << res_method->GetName() << " "
               << res_method->GetSignature() << " from "
               << referrer->GetDescriptor()->ToModifiedUtf8() << ")";
    *failure = VERIFY_ERROR_ACCESS_METHOD;
    return NULL;
  }

  return res_method;
}

Field* DexVerifier::ResolveFieldAndCheckAccess(const DexFile* dex_file,
    uint32_t field_idx, const Class* referrer, VerifyError* failure,
    bool is_static) {
  DexCache* dex_cache = referrer->GetDexCache();
  Field* res_field = dex_cache->GetResolvedField(field_idx);

  if (res_field == NULL) {
    const DexFile::FieldId& field_id = dex_file->GetFieldId(field_idx);
    Class* klass = ResolveClassAndCheckAccess(dex_file, field_id.class_idx_, referrer, failure);
    if (klass == NULL) {
      DCHECK(*failure != VERIFY_ERROR_NONE);
      return NULL;
    }

    Class* field_type = ResolveClassAndCheckAccess(dex_file, field_id.type_idx_, referrer, failure);
    if (field_type == NULL) {
      DCHECK(*failure != VERIFY_ERROR_NONE);
      return NULL;
    }

    const char* name = dex_file->dexStringById(field_id.name_idx_);
    if (is_static) {
      res_field = klass->FindStaticField(name, field_type);
    } else {
      res_field = klass->FindInstanceField(name, field_type);
    }
    if (res_field != NULL) {
      dex_cache->SetResolvedfield(field_idx, res_field);
    } else {
      LOG(ERROR) << "VFY: couldn't find field "
                 << klass->GetDescriptor()->ToModifiedUtf8() << "." << name;
      *failure = VERIFY_ERROR_NO_FIELD;
      return NULL;
    }
  }

  /* Check if access is allowed. */
  if (!referrer->CanAccess(res_field->GetDeclaringClass())) {
    LOG(ERROR) << "VFY: access denied from "
               << referrer->GetDescriptor()->ToModifiedUtf8() << " to field "
               << res_field->GetDeclaringClass()->GetDescriptor()->ToModifiedUtf8()
               << "." << res_field->GetName()->ToModifiedUtf8();
    *failure = VERIFY_ERROR_ACCESS_FIELD;
    return NULL;
  }

  return res_field;
}

DexVerifier::RegType DexVerifier::MergeTypes(RegType type1, RegType type2,
    bool* changed) {
  RegType result;

  /* Check for trivial case so we don't have to hit memory. */
  if (type1 == type2)
    return type1;

  /*
   * Use the table if we can, and reject any attempts to merge something
   * from the table with a reference type.
   *
   * Uninitialized references are composed of the enum ORed with an
   * index value. The uninitialized table entry at index zero *will*
   * show up as a simple kRegTypeUninit value. Since this cannot be
   * merged with anything but itself, the rules do the right thing.
   */
  if (type1 < kRegTypeMAX) {
    if (type2 < kRegTypeMAX) {
      result = merge_table_[type1][type2];
    } else {
      /* simple + reference == conflict, usually */
      if (type1 == kRegTypeZero)
        result = type2;
      else
        result = kRegTypeConflict;
    }
  } else {
    if (type2 < kRegTypeMAX) {
      /* reference + simple == conflict, usually */
      if (type2 == kRegTypeZero)
        result = type1;
      else
        result = kRegTypeConflict;
    } else {
      /* merging two references */
      if (RegTypeIsUninitReference(type1) ||
          RegTypeIsUninitReference(type2))
      {
        /* can't merge uninit with anything but self */
        result = kRegTypeConflict;
      } else {
        Class* klass1 = RegTypeInitializedReferenceToClass(type1);
        Class* klass2 = RegTypeInitializedReferenceToClass(type2);
        Class* merged_class = FindCommonSuperclass(klass1, klass2);
        DCHECK(merged_class != NULL);
        result = RegTypeFromClass(merged_class);
      }
    }
  }

  if (result != type1)
    *changed = true;
  return result;
}

DexVerifier::MonitorEntries DexVerifier::MergeMonitorEntries(
    MonitorEntries ents1, MonitorEntries ents2, bool* changed) {
  MonitorEntries result = ents1 & ents2;
  if (result != ents1)
    *changed = true;
  return result;
}

bool DexVerifier::UpdateRegisters(InsnFlags* insn_flags,
    RegisterTable* reg_table, int next_insn, const RegisterLine* work_line) {
  const size_t insn_reg_count_plus = reg_table->insn_reg_count_plus_;
  DCHECK(work_line != NULL);
  const RegType* work_regs = work_line->reg_types_.get();

  if (!InsnIsVisitedOrChanged(insn_flags, next_insn)) {
    /*
     * We haven't processed this instruction before, and we haven't
     * touched the registers here, so there's nothing to "merge". Copy
     * the registers over and mark it as changed. (This is the only
     * way a register can transition out of "unknown", so this is not
     * just an optimization.)
     */
    CopyLineToTable(reg_table, next_insn, work_line);
    InsnSetChanged(insn_flags, next_insn, true);
  } else {
    /* Merge registers, set Changed only if different */
    RegisterLine* target_line = GetRegisterLine(reg_table, next_insn);
    RegType* target_regs = target_line->reg_types_.get();
    MonitorEntries* work_mon_ents = work_line->monitor_entries_.get();
    MonitorEntries* target_mon_ents = target_line->monitor_entries_.get();
    bool changed = false;
    unsigned int idx;

    DCHECK(target_regs != NULL);
    if (target_mon_ents != NULL) {
      /* Monitor stacks must be identical. */
      if (target_line->monitor_stack_top_ != work_line->monitor_stack_top_) {
        LOG(ERROR) << "VFY: mismatched stack depth "
                   << target_line->monitor_stack_top_ << " vs. "
                   << work_line->monitor_stack_top_ << " at 0x"
                   << std::hex << next_insn << std::dec;
        return false;
      }
      if (memcmp(target_line->monitor_stack_.get(), work_line->monitor_stack_.get(),
                 target_line->monitor_stack_top_ * sizeof(uint32_t)) != 0) {
         LOG(ERROR) << "VFY: mismatched monitor stacks at 0x" << std::hex
                    << next_insn << std::dec;
         return false;
      }
    }

    for (idx = 0; idx < insn_reg_count_plus; idx++) {
      target_regs[idx] = MergeTypes(target_regs[idx], work_regs[idx], &changed);

      if (target_mon_ents != NULL) {
        target_mon_ents[idx] = MergeMonitorEntries(target_mon_ents[idx],
             work_mon_ents[idx], &changed);
      }
    }

    if (changed) {
      InsnSetChanged(insn_flags, next_insn, true);
    }
  }

  return true;
}

bool DexVerifier::CanConvertTo1nr(RegType src_type, RegType check_type) {
  static const char conv_tab[kRegType1nrEND - kRegType1nrSTART + 1]
                            [kRegType1nrEND - kRegType1nrSTART + 1] =
  {
    /* chk: 0  1  Z  y  Y  h  H  c  i  b  B  s  S  C  I  F */
    { /*0*/ 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 },
    { /*1*/ 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 },
    { /*Z*/ 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0 },
    { /*y*/ 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 },
    { /*Y*/ 0, 0, 0, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 1 },
    { /*h*/ 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1 },
    { /*H*/ 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 1, 0, 1, 1 },
    { /*c*/ 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 1, 1, 1 },
    { /*i*/ 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 1 },
    { /*b*/ 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 0 },
    { /*B*/ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 1, 0 },
    { /*s*/ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0 },
    { /*S*/ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0 },
    { /*C*/ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0 },
    { /*I*/ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0 },
    { /*F*/ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 },
  };

  DCHECK(check_type >= kRegType1nrSTART);
  DCHECK(check_type <= kRegType1nrEND);

  if (src_type >= kRegType1nrSTART && src_type <= kRegType1nrEND)
    return (bool) conv_tab[src_type - kRegType1nrSTART]
                          [check_type - kRegType1nrSTART];

  return false;
}

bool DexVerifier::CanConvertTo2(RegType src_type, RegType check_type) {
  return ((src_type == kRegTypeConstLo || src_type == check_type) &&
          (check_type == kRegTypeLongLo || check_type == kRegTypeDoubleLo));
}

DexVerifier::RegType DexVerifier::PrimitiveTypeToRegType(
    Class::PrimitiveType prim_type) {
  switch (prim_type) {
    case Class::kPrimBoolean: return kRegTypeBoolean;
    case Class::kPrimByte:    return kRegTypeByte;
    case Class::kPrimShort:   return kRegTypeShort;
    case Class::kPrimChar:    return kRegTypeChar;
    case Class::kPrimInt:     return kRegTypeInteger;
    case Class::kPrimLong:    return kRegTypeLongLo;
    case Class::kPrimFloat:   return kRegTypeFloat;
    case Class::kPrimDouble:  return kRegTypeDoubleLo;
    case Class::kPrimVoid:
    default: {
      return kRegTypeUnknown;
    }
  }
}

DexVerifier::RegType DexVerifier::ConstTypeToRegType(RegType const_type) {
  switch (const_type) {
    case kRegTypeConstPosByte: return kRegTypePosByte;
    case kRegTypeConstByte: return kRegTypeByte;
    case kRegTypeConstPosShort: return kRegTypePosShort;
    case kRegTypeConstShort: return kRegTypeShort;
    case kRegTypeConstChar: return kRegTypeChar;
    case kRegTypeConstInteger: return kRegTypeInteger;
    default: {
      return const_type;
    }
  }
}

char DexVerifier::DetermineCat1Const(int32_t value) {
  if (value < -32768)
    return kRegTypeConstInteger;
  else if (value < -128)
    return kRegTypeConstShort;
  else if (value < 0)
    return kRegTypeConstByte;
  else if (value == 0)
    return kRegTypeZero;
  else if (value == 1)
    return kRegTypeOne;
  else if (value < 128)
    return kRegTypeConstPosByte;
  else if (value < 32768)
    return kRegTypeConstPosShort;
  else if (value < 65536)
    return kRegTypeConstChar;
  else
    return kRegTypeConstInteger;
}

void DexVerifier::CheckFinalFieldAccess(const Method* method,
    const Field* field, VerifyError* failure) {
  if (!field->IsFinal())
    return;

  /* make sure we're in the same class */
  if (method->GetDeclaringClass() != field->GetDeclaringClass()) {
    LOG(ERROR) << "VFY: can't modify final field "
               << field->GetDeclaringClass()->GetDescriptor()->ToModifiedUtf8()
               << "." << field->GetName()->ToModifiedUtf8();
    *failure = VERIFY_ERROR_ACCESS_FIELD;
    return;
  }
}

void DexVerifier::CheckArrayIndexType(const Method* method, RegType reg_type,
    VerifyError* failure) {
  if (*failure == VERIFY_ERROR_NONE) {
    /*
     * The 1nr types are interchangeable at this level. We could
     * do something special if we can definitively identify it as a
     * float, but there's no real value in doing so.
     */
    CheckTypeCategory(reg_type, kTypeCategory1nr, failure);
    if (*failure != VERIFY_ERROR_NONE) {
      LOG(ERROR) << "Invalid reg type for array index (" << reg_type << ")";
    }
  }
}

bool DexVerifier::CheckConstructorReturn(const Method* method,
    const RegisterLine* register_line, const int insn_reg_count) {
  const RegType* insn_regs = register_line->reg_types_.get();

  if (!IsInitMethod(method))
    return true;

  RegType uninit_this = RegTypeFromUninitIndex(kUninitThisArgSlot);

  for (int i = 0; i < insn_reg_count; i++) {
    if (insn_regs[i] == uninit_this) {
      LOG(ERROR) << "VFY: <init> returning without calling superclass init";
      return false;
    }
  }
  return true;
}

bool DexVerifier::CheckMoveException(const uint16_t* insns, int insn_idx) {
  if ((insns[insn_idx] & 0xff) == Instruction::MOVE_EXCEPTION) {
    LOG(ERROR) << "VFY: invalid use of move-exception";
    return false;
  }
  return true;
}

void DexVerifier::CheckTypeCategory(RegType type, TypeCategory cat,
    VerifyError* failure) {
  switch (cat) {
    case kTypeCategory1nr:
      switch (type) {
        case kRegTypeZero:
        case kRegTypeOne:
        case kRegTypeBoolean:
        case kRegTypeConstPosByte:
        case kRegTypeConstByte:
        case kRegTypeConstPosShort:
        case kRegTypeConstShort:
        case kRegTypeConstChar:
        case kRegTypeConstInteger:
        case kRegTypePosByte:
        case kRegTypeByte:
        case kRegTypePosShort:
        case kRegTypeShort:
        case kRegTypeChar:
        case kRegTypeInteger:
        case kRegTypeFloat:
          break;
        default:
          *failure = VERIFY_ERROR_GENERIC;
          break;
      }
      break;
    case kTypeCategory2:
      switch (type) {
        case kRegTypeConstLo:
        case kRegTypeLongLo:
        case kRegTypeDoubleLo:
          break;
        default:
          *failure = VERIFY_ERROR_GENERIC;
         break;
      }
      break;
    case kTypeCategoryRef:
      if (type != kRegTypeZero && !RegTypeIsReference(type))
        *failure = VERIFY_ERROR_GENERIC;
      break;
    default:
      DCHECK(false);
      *failure = VERIFY_ERROR_GENERIC;
      break;
  }
}

void DexVerifier::CheckWidePair(RegType type_l, RegType type_h,
    VerifyError* failure) {
  if ((type_h != type_l + 1))
    *failure = VERIFY_ERROR_GENERIC;
}

void DexVerifier::CheckUnop(RegisterLine* register_line,
    Instruction::DecodedInstruction* dec_insn, RegType dst_type,
    RegType src_type, VerifyError* failure) {
  VerifyRegisterType(register_line, dec_insn->vB_, src_type, failure);
  SetRegisterType(register_line, dec_insn->vA_, dst_type);
}

bool DexVerifier::UpcastBooleanOp(RegisterLine* register_line, uint32_t reg1,
    uint32_t reg2) {
  RegType type1, type2;

  type1 = GetRegisterType(register_line, reg1);
  type2 = GetRegisterType(register_line, reg2);

  if ((type1 == kRegTypeBoolean || type1 == kRegTypeZero || type1 == kRegTypeOne) &&
      (type2 == kRegTypeBoolean || type2 == kRegTypeZero || type2 == kRegTypeOne)) {
    return true;
  }
  return false;
}

void DexVerifier::CheckLitop(RegisterLine* register_line,
    Instruction::DecodedInstruction* dec_insn, RegType dst_type,
    RegType src_type, bool check_boolean_op, VerifyError* failure) {
  VerifyRegisterType(register_line, dec_insn->vB_, src_type, failure);

  if ((*failure == VERIFY_ERROR_NONE) && check_boolean_op) {
    DCHECK(dst_type == kRegTypeInteger);

    /* check vB with the call, then check the constant manually */
    if (UpcastBooleanOp(register_line, dec_insn->vB_, dec_insn->vB_)
        && (dec_insn->vC_ == 0 || dec_insn->vC_ == 1)) {
      dst_type = kRegTypeBoolean;
    }
  }

  SetRegisterType(register_line, dec_insn->vA_, dst_type);
}

void DexVerifier::CheckBinop(RegisterLine* register_line,
    Instruction::DecodedInstruction* dec_insn, RegType dst_type,
    RegType src_type1, RegType src_type2, bool check_boolean_op,
    VerifyError* failure) {
  VerifyRegisterType(register_line, dec_insn->vB_, src_type1, failure);
  VerifyRegisterType(register_line, dec_insn->vC_, src_type2, failure);

  if ((*failure == VERIFY_ERROR_NONE) && check_boolean_op) {
    DCHECK(dst_type == kRegTypeInteger);
    if (UpcastBooleanOp(register_line, dec_insn->vB_, dec_insn->vC_))
      dst_type = kRegTypeBoolean;
  }

  SetRegisterType(register_line, dec_insn->vA_, dst_type);
}

void DexVerifier::CheckBinop2addr(RegisterLine* register_line,
    Instruction::DecodedInstruction* dec_insn, RegType dst_type,
    RegType src_type1, RegType src_type2, bool check_boolean_op,
    VerifyError* failure) {
  VerifyRegisterType(register_line, dec_insn->vA_, src_type1, failure);
  VerifyRegisterType(register_line, dec_insn->vB_, src_type2, failure);

  if ((*failure == VERIFY_ERROR_NONE) && check_boolean_op) {
    DCHECK(dst_type == kRegTypeInteger);
    if (UpcastBooleanOp(register_line, dec_insn->vA_, dec_insn->vB_))
      dst_type = kRegTypeBoolean;
  }

  SetRegisterType(register_line, dec_insn->vA_, dst_type);
}

DexVerifier::RegType DexVerifier::AdjustForRightShift(
    RegisterLine* register_line, int reg, unsigned int shift_count,
    bool is_unsigned_shift) {
  RegType src_type = GetRegisterType(register_line, reg);
  RegType new_type;

  /* convert const derived types to their actual types */
  src_type = ConstTypeToRegType(src_type);

  /* no-op */
  if (shift_count == 0)
    return src_type;

  /* safe defaults */
  if (is_unsigned_shift)
    new_type = kRegTypeInteger;
  else
    new_type = src_type;

  if (shift_count >= 32) {
    LOG(ERROR) << "Got unexpectedly large shift count " << shift_count;
    /* fail? */
    return new_type;
  }

  switch (src_type) {
    case kRegTypeInteger:               /* 32-bit signed value */
      if (is_unsigned_shift) {
        if (shift_count > 24)
          new_type = kRegTypePosByte;
        else if (shift_count >= 16)
          new_type = kRegTypeChar;
      } else {
        if (shift_count >= 24)
          new_type = kRegTypeByte;
        else if (shift_count >= 16)
          new_type = kRegTypeShort;
      }
      break;
    case kRegTypeShort:                 /* 16-bit signed value */
      if (is_unsigned_shift) {
        /* default (kRegTypeInteger) is correct */
      } else {
        if (shift_count >= 8)
          new_type = kRegTypeByte;
      }
      break;
    case kRegTypePosShort:              /* 15-bit unsigned value */
      if (shift_count >= 8)
        new_type = kRegTypePosByte;
      break;
    case kRegTypeChar:                  /* 16-bit unsigned value */
      if (shift_count > 8)
        new_type = kRegTypePosByte;
      break;
    case kRegTypeByte:                  /* 8-bit signed value */
      /* defaults (u=kRegTypeInteger / s=src_type) are correct */
      break;
    case kRegTypePosByte:               /* 7-bit unsigned value */
      /* always use new_type=src_type */
      new_type = src_type;
      break;
    case kRegTypeZero:                  /* 1-bit unsigned value */
    case kRegTypeOne:
    case kRegTypeBoolean:
      /* unnecessary? */
      new_type = kRegTypeZero;
      break;
    default:
      /* long, double, references; shouldn't be here! */
      DCHECK(false);
      break;
  }

  return new_type;
}

void DexVerifier::VerifyFilledNewArrayRegs(const Method* method,
    RegisterLine* register_line,
    const Instruction::DecodedInstruction* dec_insn, Class* res_class,
    bool is_range, VerifyError* failure) {
  uint32_t arg_count = dec_insn->vA_;
  RegType expected_type;
  Class::PrimitiveType elem_type;
  unsigned int ui;

  DCHECK(res_class->IsArrayClass()) << PrettyClass(res_class);
  elem_type = res_class->GetComponentType()->GetPrimitiveType();
  if (elem_type == Class::kPrimNot) {
    expected_type = RegTypeFromClass(res_class->GetComponentType());
  } else {
    expected_type = PrimitiveTypeToRegType(elem_type);
  }

  /*
   * Verify each register. If "arg_count" is bad, VerifyRegisterType()
   * will run off the end of the list and fail. It's legal, if silly,
   * for arg_count to be zero.
   */
  for (ui = 0; ui < arg_count; ui++) {
    uint32_t get_reg;

    if (is_range)
      get_reg = dec_insn->vC_ + ui;
    else
      get_reg = dec_insn->arg_[ui];

    VerifyRegisterType(register_line, get_reg, expected_type, failure);
    if (*failure != VERIFY_ERROR_NONE) {
      LOG(ERROR) << "VFY: filled-new-array arg " << ui << "(" << get_reg
                 << ") not valid";
      return;
    }
  }
}

bool DexVerifier::IsCorrectInvokeKind(MethodType method_type,
    Method* res_method) {
  switch (method_type) {
    case METHOD_DIRECT:
      return res_method->IsDirect();
    case METHOD_STATIC:
      return res_method->IsStatic();
    case METHOD_VIRTUAL:
    case METHOD_INTERFACE:
      return !res_method->IsDirect();
    default:
      return false;
  }
}

Method* DexVerifier::VerifyInvocationArgs(VerifierData* vdata,
    RegisterLine* register_line, const int insn_reg_count,
    const Instruction::DecodedInstruction* dec_insn, MethodType method_type,
    bool is_range, bool is_super, VerifyError* failure) {
  Method* method = vdata->method_;
  const DexFile* dex_file = vdata->dex_file_;
  const DexFile::CodeItem* code_item = vdata->code_item_;
  UninitInstanceMap* uninit_map = vdata->uninit_map_.get();

  Method* res_method;
  std::string sig;
  size_t sig_offset;
  int expected_args;
  int actual_args;

  /*
   * Resolve the method. This could be an abstract or concrete method
   * depending on what sort of call we're making.
   */
  res_method = ResolveMethodAndCheckAccess(dex_file, dec_insn->vB_, method->GetDeclaringClass(),
      failure, (method_type == METHOD_DIRECT || method_type == METHOD_STATIC));

  if (res_method == NULL) {
    const DexFile::MethodId& method_id = dex_file->GetMethodId(dec_insn->vB_);
    const char* method_name = dex_file->GetMethodName(method_id);
    const char* method_proto = dex_file->GetMethodPrototype(method_id);
    const char* class_descriptor = dex_file->GetMethodClassDescriptor(method_id);

    LOG(ERROR) << "VFY: unable to resolve method " << dec_insn->vB_ << ": "
               << class_descriptor << "." << method_name << " " << method_proto;
    *failure = VERIFY_ERROR_NO_METHOD;
    return NULL;
  }

  /*
   * Only time you can explicitly call a method starting with '<' is when
   * making a "direct" invocation on "<init>". There are additional
   * restrictions but we don't enforce them here.
   */
  if (res_method->GetName()->Equals("<init>")) {
    if (method_type != METHOD_DIRECT || !IsInitMethod(res_method)) {
      LOG(ERROR) << "VFY: invalid call to "
                 << res_method->GetDeclaringClass()->GetDescriptor()->ToModifiedUtf8()
                 << "." << res_method->GetName();
      goto bad_sig;
    }
  }

  /*
   * See if the method type implied by the invoke instruction matches the
   * access flags for the target method.
   */
  if (!IsCorrectInvokeKind(method_type, res_method)) {
    LOG(ERROR) << "VFY: invoke type does not match method type of "
               << res_method->GetDeclaringClass()->GetDescriptor()->ToModifiedUtf8()
               << "." << res_method->GetName()->ToModifiedUtf8();

    *failure = VERIFY_ERROR_GENERIC;
    return NULL;
  }

  /*
   * If we're using invoke-super(method), make sure that the executing
   * method's class' superclass has a vtable entry for the target method.
   */
  if (is_super) {
    DCHECK(method_type == METHOD_VIRTUAL);
    Class* super = method->GetDeclaringClass()->GetSuperClass();
    if (super == NULL || res_method->GetMethodIndex() > super->GetVTable()->GetLength()) {
      if (super == NULL) {
        LOG(ERROR) << "VFY: invalid invoke-super from "
                   << method->GetDeclaringClass()->GetDescriptor()->ToModifiedUtf8()
                   << "." << method->GetName()->ToModifiedUtf8() << " to super -."
                   << res_method->GetName()->ToModifiedUtf8()
                   << " " << res_method->GetSignature()->ToModifiedUtf8();
      } else {
        LOG(ERROR) << "VFY: invalid invoke-super from "
                   << method->GetDeclaringClass()->GetDescriptor()->ToModifiedUtf8()
                   << "." << method->GetName()->ToModifiedUtf8() << " to super "
                   << super->GetDescriptor()->ToModifiedUtf8()
                   << "." << res_method->GetName()->ToModifiedUtf8()
                   << " " << res_method->GetSignature()->ToModifiedUtf8();
      }
      *failure = VERIFY_ERROR_NO_METHOD;
      return NULL;
    }
  }

  /*
   * We use vAA as our expected arg count, rather than res_method->insSize,
   * because we need to match the call to the signature. Also, we might
   * might be calling through an abstract method definition (which doesn't
   * have register count values).
   */
  expected_args = dec_insn->vA_;
  actual_args = 0;

  /* caught by static verifier */
  DCHECK(is_range || expected_args <= 5);

  if (expected_args > code_item->outs_size_) {
    LOG(ERROR) << "VFY: invalid arg count (" << expected_args
               << ") exceeds outsSize (" << code_item->outs_size_ << ")";
    *failure = VERIFY_ERROR_GENERIC;
    return NULL;
  }

  sig = res_method->GetSignature()->ToModifiedUtf8();
  if (sig[0] != '(') {
    LOG(ERROR) << "VFY: descriptor doesn't start with '(': " << sig;
    goto bad_sig;
  }

  /*
   * Check the "this" argument, which must be an instance of the class
   * that declared the method. For an interface class, we don't do the
   * full interface merge, so we can't do a rigorous check here (which
   * is okay since we have to do it at runtime).
   */
  if (!res_method->IsStatic()) {
    Class* actual_this_ref;
    RegType actual_arg_type;

    actual_arg_type = GetInvocationThis(register_line, dec_insn, failure);
    if (*failure != VERIFY_ERROR_NONE)
      return NULL;

    if (RegTypeIsUninitReference(actual_arg_type) &&
        !res_method->GetName()->Equals("<init>")) {
      LOG(ERROR) << "VFY: 'this' arg must be initialized";
      *failure = VERIFY_ERROR_GENERIC;
      return NULL;
    }
    if (method_type != METHOD_INTERFACE && actual_arg_type != kRegTypeZero) {
      actual_this_ref = RegTypeReferenceToClass(actual_arg_type, uninit_map);
      if (!res_method->GetDeclaringClass()->IsAssignableFrom(actual_this_ref)) {
        LOG(ERROR) << "VFY: 'this' arg '"
                   << actual_this_ref->GetDescriptor()->ToModifiedUtf8()
                   << "' not instance of '"
                   << res_method->GetDeclaringClass()->GetDescriptor()->ToModifiedUtf8()
                   << "'";
        *failure = VERIFY_ERROR_GENERIC;
        return NULL;
      }
    }
    actual_args++;
  }

  /*
   * Process the target method's signature. This signature may or may not
   * have been verified, so we can't assume it's properly formed.
   */
  for (sig_offset = 1; sig_offset < sig.size(); sig_offset++) {
    if (sig[sig_offset] == ')')
      break;

    if (actual_args >= expected_args) {
      LOG(ERROR) << "VFY: expected " << expected_args << " args, found more ("
                 << sig.substr(sig_offset) << ")";
      goto bad_sig;
    }

    uint32_t get_reg;
    if (is_range)
      get_reg = dec_insn->vC_ + actual_args;
    else
      get_reg = dec_insn->arg_[actual_args];

    switch (sig[sig_offset]) {
      case 'L':
        {
          Class* klass = LookupSignatureClass(method, sig.substr(sig_offset),
              failure);
          if (*failure != VERIFY_ERROR_NONE)
            goto bad_sig;
          VerifyRegisterType(register_line, get_reg, RegTypeFromClass(klass),
              failure);
          if (*failure != VERIFY_ERROR_NONE) {
            LOG(ERROR) << "VFY: bad arg " << actual_args << " (into "
                       << klass->GetDescriptor()->ToModifiedUtf8() << ")";
            goto bad_sig;
          }
          sig_offset += sig.substr(sig_offset).find(';');
        }
        actual_args++;
        break;
      case '[':
        {
          Class* klass = LookupSignatureArrayClass(method,
              sig.substr(sig_offset), failure);
          if (*failure != VERIFY_ERROR_NONE)
            goto bad_sig;
          VerifyRegisterType(register_line, get_reg, RegTypeFromClass(klass),
              failure);
          if (*failure != VERIFY_ERROR_NONE) {
            LOG(ERROR) << "VFY: bad arg " << actual_args << " (into "
                       << klass->GetDescriptor()->ToModifiedUtf8() << ")";
            goto bad_sig;
          }
          while (sig[sig_offset] == '[')
            sig_offset++;
          if (sig[sig_offset] == 'L')
            sig_offset += sig.substr(sig_offset).find(';');
        }
        actual_args++;
        break;
      case 'Z':
        VerifyRegisterType(register_line, get_reg, kRegTypeBoolean, failure);
        actual_args++;
        break;
      case 'C':
        VerifyRegisterType(register_line, get_reg, kRegTypeChar, failure);
        actual_args++;
        break;
      case 'B':
        VerifyRegisterType(register_line, get_reg, kRegTypeByte, failure);
        actual_args++;
        break;
      case 'I':
        VerifyRegisterType(register_line, get_reg, kRegTypeInteger, failure);
        actual_args++;
        break;
      case 'S':
        VerifyRegisterType(register_line, get_reg, kRegTypeShort, failure);
        actual_args++;
        break;
      case 'F':
        VerifyRegisterType(register_line, get_reg, kRegTypeFloat, failure);
        actual_args++;
        break;
      case 'D':
        VerifyRegisterType(register_line, get_reg, kRegTypeDoubleLo, failure);
        actual_args += 2;
        break;
      case 'J':
        VerifyRegisterType(register_line, get_reg, kRegTypeLongLo, failure);
        actual_args += 2;
        break;
      default:
        LOG(ERROR) << "VFY: invocation target: bad signature type char '"
                   << sig << "'";
        goto bad_sig;
    }
  }
  if (sig[sig_offset] != ')') {
    LOG(ERROR) << "VFY: invocation target: bad signature '"
               << res_method->GetSignature()->ToModifiedUtf8() << "'";
    goto bad_sig;
  }

  if (actual_args != expected_args) {
    LOG(ERROR) << "VFY: expected " << expected_args << " args, found "
               << actual_args;
    goto bad_sig;
  }

  return res_method;

bad_sig:
  if (res_method != NULL) {
    LOG(ERROR) << "VFY:  rejecting call to "
               << res_method->GetDeclaringClass()->GetDescriptor()->ToModifiedUtf8()
               << "." << res_method->GetName()->ToModifiedUtf8() << " "
               << res_method->GetSignature()->ToModifiedUtf8();
  }

  if (*failure == VERIFY_ERROR_NONE)
    *failure = VERIFY_ERROR_GENERIC;
  return NULL;
}

DexVerifier::RegisterMap* DexVerifier::GenerateRegisterMapV(VerifierData* vdata)
{
  const DexFile::CodeItem* code_item = vdata->code_item_;
  int i, bytes_for_addr, gc_point_count;

  if (code_item->registers_size_ >= 2048) {
    LOG(ERROR) << "ERROR: register map can't handle "
               << code_item->registers_size_ << " registers";
    return NULL;
  }
  uint8_t reg_width = (code_item->registers_size_ + 7) / 8;

  /*
   * Decide if we need 8 or 16 bits to hold the address. Strictly speaking
   * we only need 16 bits if we actually encode an address >= 256 -- if
   * the method has a section at the end without GC points (e.g. array
   * data) we don't need to count it. The situation is unusual, and
   * detecting it requires scanning the entire method, so we don't bother.
   */
  RegisterMapFormat format;
  if (code_item->insns_size_ < 256) {
    format = kRegMapFormatCompact8;
    bytes_for_addr = 1;
  } else {
    format = kRegMapFormatCompact16;
    bytes_for_addr = 2;
  }

  /*
   * Count up the number of GC point instructions.
   *
   * NOTE: this does not automatically include the first instruction,
   * since we don't count method entry as a GC point.
   */
  gc_point_count = 0;
  for (i = 0; i < (int) code_item->insns_size_; i++) {
    if (InsnIsGcPoint(vdata->insn_flags_.get(), i))
      gc_point_count++;
  }
  if (gc_point_count >= 65536) {
    /* We could handle this, but in practice we don't get near this. */
    LOG(ERROR) << "ERROR: register map can't handle " << gc_point_count
               << " gc points in one method";
    return NULL;
  }

  /* Calculate size of buffer to hold the map data. */
  uint32_t data_size = gc_point_count * (bytes_for_addr + reg_width);

  RegisterMap* map = new RegisterMap(format, reg_width, gc_point_count,
      data_size);

  /* Populate it. */
  uint8_t* map_data = map->data_;
  for (i = 0; i < (int) vdata->code_item_->insns_size_; i++) {
    if (InsnIsGcPoint(vdata->insn_flags_.get(), i)) {
      DCHECK(vdata->register_lines_[i].reg_types_.get() != NULL);
      if (format == kRegMapFormatCompact8) {
        *map_data++ = i;
      } else /*kRegMapFormatCompact16*/ {
        *map_data++ = i & 0xff;
        *map_data++ = i >> 8;
      }
      OutputTypeVector(vdata->register_lines_[i].reg_types_.get(),
          code_item->registers_size_, map_data);
      map_data += reg_width;
    }
  }

  DCHECK_EQ((uint32_t) map_data - (uint32_t) map->data_, data_size);

  // TODO: Remove this check when it's really running...
#if 1
  if (!VerifyMap(vdata, map)) {
    LOG(ERROR) << "Map failed to verify";
    return NULL;
  }
#endif

  /* Try to compress the map. */
  RegisterMap* compress_map = CompressMapDifferential(map);
  if (compress_map != NULL) {
    // TODO: Remove this check when it's really running...
#if 1
    /*
     * Expand the compressed map we just created, and compare it
     * to the original. Abort the VM if it doesn't match up.
     */
    UniquePtr<RegisterMap> uncompressed_map(UncompressMapDifferential(compress_map));
    if (uncompressed_map.get() == NULL) {
      LOG(ERROR) << "Map failed to uncompress - "
                 << vdata->method_->GetDeclaringClass()->GetDescriptor()->ToModifiedUtf8()
                 << "." << vdata->method_->GetName()->ToModifiedUtf8();
      delete map;
      delete compress_map;
      /* bad - compression is broken or we're out of memory */
      return NULL;
    } else {
      if (!CompareMaps(map, uncompressed_map.get())) {
        LOG(ERROR) << "Map comparison failed - "
                   << vdata->method_->GetDeclaringClass()->GetDescriptor()->ToModifiedUtf8()
                   << "." << vdata->method_->GetName()->ToModifiedUtf8();
        delete map;
        delete compress_map;
        /* bad - compression is broken */
        return NULL;
      }
    }
#endif
    delete map;
    map = compress_map;
  }

  return map;
}

DexVerifier::RegisterMap* DexVerifier::GetExpandedRegisterMapHelper(
    Method* method, RegisterMap* map) {
  RegisterMap* new_map;

  if (map == NULL)
    return NULL;

  /* TODO: sanity check to ensure this isn't called w/o external locking */

  uint8_t format = map->header_->format_;
  switch (format) {
    case kRegMapFormatCompact8:
    case kRegMapFormatCompact16:
      /* already expanded */
      return map;
    case kRegMapFormatDifferential:
      new_map = UncompressMapDifferential(map);
      break;
    default:
      LOG(ERROR) << "Unknown format " << format
                 << " in dvmGetExpandedRegisterMap";
      return NULL;
  }

  if (new_map == NULL) {
    LOG(ERROR) << "Map failed to uncompress (fmt=" << format << ") "
               << method->GetDeclaringClass()->GetDescriptor()->ToModifiedUtf8()
               << "." << method->GetName();
    return NULL;
  }

  /* Update method, and free compressed map if it was sitting on the heap. */
  ByteArray* header = ByteArray::Alloc(sizeof(RegisterMapHeader));
  ByteArray* data = ByteArray::Alloc(ComputeRegisterMapSize(map));

  memcpy(header->GetData(), map->header_, sizeof(RegisterMapHeader));
  memcpy(data->GetData(), map->data_, ComputeRegisterMapSize(map));

  method->SetRegisterMapHeader(header);
  method->SetRegisterMapData(data);

  delete map;
  return new_map;
}

const uint8_t* DexVerifier::RegisterMapGetLine(const RegisterMap* map, int addr) {
  int addr_width, line_width;
  uint8_t format = map->header_->format_;
  uint16_t num_entries = map->header_->num_entries_;

  DCHECK_GT(num_entries, 0);

  switch (format) {
    case kRegMapFormatNone:
      return NULL;
    case kRegMapFormatCompact8:
      addr_width = 1;
      break;
    case kRegMapFormatCompact16:
      addr_width = 2;
      break;
    default:
      LOG(ERROR) << "Unknown format " << format;
      return NULL;
  }

  line_width = addr_width + map->header_->reg_width_;

  /*
   * Find the appropriate entry. Many maps are very small, some are very large.
   */
  static const int kSearchThreshold = 8;
  const uint8_t* data = NULL;
  int line_addr;

  if (num_entries < kSearchThreshold) {
    int i;
    data = map->data_;
    for (i = num_entries; i > 0; i--) {
      line_addr = data[0];
      if (addr_width > 1)
        line_addr |= data[1] << 8;
      if (line_addr == addr)
        return data + addr_width;

      data += line_width;
    }
    DCHECK_EQ(data, map->data_ + line_width * num_entries);
  } else {
    int hi, lo, mid;

    lo = 0;
    hi = num_entries -1;

    while (hi >= lo) {
      mid = (hi + lo) / 2;
      data = map->data_ + line_width * mid;

      line_addr = data[0];
      if (addr_width > 1)
        line_addr |= data[1] << 8;

      if (addr > line_addr) {
        lo = mid + 1;
      } else if (addr < line_addr) {
        hi = mid - 1;
      } else {
        return data + addr_width;
      }
    }
  }

  return NULL;
}

void DexVerifier::OutputTypeVector(const RegType* regs, int insn_reg_count,
    uint8_t* data) {
  uint8_t val = 0;
  int i;

  for (i = 0; i < insn_reg_count; i++) {
    RegType type = *regs++;
    val >>= 1;
    if (IsReferenceType(type))
      val |= 0x80;        /* set hi bit */

    if ((i & 0x07) == 7)
      *data++ = val;
  }
  if ((i & 0x07) != 0) {
    /* Flush bits from last byte. */
    val >>= 8 - (i & 0x07);
    *data++ = val;
  }
}

bool DexVerifier::VerifyMap(VerifierData* vdata, const RegisterMap* map) {
  const uint8_t* raw_map = map->data_;
  uint8_t format = map->header_->format_;
  const int num_entries = map->header_->num_entries_;
  int ent;

  if ((vdata->code_item_->registers_size_ + 7) / 8 != map->header_->reg_width_) {
    LOG(ERROR) << "GLITCH: registersSize=" << vdata->code_item_->registers_size_
               << ", reg_width=" << map->header_->reg_width_;
    return false;
  }

  for (ent = 0; ent < num_entries; ent++) {
    int addr;

    switch (format) {
      case kRegMapFormatCompact8:
        addr = *raw_map++;
        break;
      case kRegMapFormatCompact16:
        addr = *raw_map++;
        addr |= (*raw_map++) << 8;
        break;
      default:
        LOG(FATAL) << "GLITCH: bad format (" << format << ")";
        return false;
    }

    const RegType* regs = vdata->register_lines_[addr].reg_types_.get();
    if (regs == NULL) {
      LOG(ERROR) << "GLITCH: addr " << addr << " has no data";
      return false;
    }

    uint8_t val = 0;
    int i;

    for (i = 0; i < vdata->code_item_->registers_size_; i++) {
      bool bit_is_ref, reg_is_ref;

      val >>= 1;
      if ((i & 0x07) == 0) {
        /* Load next byte of data. */
        val = *raw_map++;
      }

      bit_is_ref = val & 0x01;

      RegType type = regs[i];
      reg_is_ref = IsReferenceType(type);

      if (bit_is_ref != reg_is_ref) {
        LOG(ERROR) << "GLITCH: addr " << addr << " reg " << i << ": bit="
                   << bit_is_ref << " reg=" << reg_is_ref << "(" << type << ")";
        return false;
      }
    }
    /* Raw_map now points to the address field of the next entry. */
  }

  return true;
}

bool DexVerifier::CompareMaps(const RegisterMap* map1, const RegisterMap* map2)
{
  size_t size1, size2;

  size1 = ComputeRegisterMapSize(map1);
  size2 = ComputeRegisterMapSize(map2);
  if (size1 != size2) {
    LOG(ERROR) << "CompareMaps: size mismatch (" << size1 << " vs " << size2
               << ")";
    return false;
  }

  if (map1->header_->format_ != map2->header_->format_ ||
      map1->header_->reg_width_ != map2->header_->reg_width_ ||
      map1->header_->num_entries_ != map2->header_->num_entries_) {
    LOG(ERROR) << "CompareMaps: fields mismatch";
  }
  if (memcmp(map1->data_, map2->data_, size1) != 0) {
    LOG(ERROR) << "CompareMaps: data mismatch";
    return false;
  }

  return true;
}

size_t DexVerifier::ComputeRegisterMapSize(const RegisterMap* map) {
  uint8_t format = map->header_->format_;
  uint16_t num_entries = map->header_->num_entries_;

  DCHECK(map != NULL);

  switch (format) {
    case kRegMapFormatNone:
      return 1;
    case kRegMapFormatCompact8:
      return (1 + map->header_->reg_width_) * num_entries;
    case kRegMapFormatCompact16:
      return (2 + map->header_->reg_width_) * num_entries;
    case kRegMapFormatDifferential:
      {
        /* Decoded ULEB128 length. */
        const uint8_t* ptr = map->data_;
        return DecodeUnsignedLeb128(&ptr);
      }
    default:
      LOG(FATAL) << "Bad register map format " << format;
      return 0;
  }
}

int DexVerifier::ComputeBitDiff(const uint8_t* bits1, const uint8_t* bits2,
    int byte_width, int* first_bit_changed_ptr, int* num_bits_changed_ptr,
    uint8_t* leb_out_buf) {
  int num_bits_changed = 0;
  int first_bit_changed = -1;
  int leb_size = 0;
  int byte_num;

  /*
   * Run through the vectors, first comparing them at the byte level. This
   * will yield a fairly quick result if nothing has changed between them.
   */
  for (byte_num = 0; byte_num < byte_width; byte_num++) {
    uint8_t byte1 = *bits1++;
    uint8_t byte2 = *bits2++;
    if (byte1 != byte2) {
      /* Walk through the byte, identifying the changed bits. */
      int bit_num;
      for (bit_num = 0; bit_num < 8; bit_num++) {
        if (((byte1 >> bit_num) & 0x01) != ((byte2 >> bit_num) & 0x01)) {
          int bit_offset = (byte_num << 3) + bit_num;

          if (first_bit_changed < 0)
            first_bit_changed = bit_offset;
          num_bits_changed++;

          if (leb_out_buf == NULL) {
            leb_size += UnsignedLeb128Size(bit_offset);
          } else {
            uint8_t* cur_buf = leb_out_buf;
            leb_out_buf = WriteUnsignedLeb128(leb_out_buf, bit_offset);
            leb_size += leb_out_buf - cur_buf;
          }
        }
      }
    }
  }

  if (num_bits_changed > 0) {
    DCHECK_GE(first_bit_changed, 0);
  }

  if (first_bit_changed_ptr != NULL) {
    *first_bit_changed_ptr = first_bit_changed;
  }

  if (num_bits_changed_ptr != NULL) {
    *num_bits_changed_ptr = num_bits_changed;
  }

  return leb_size;
}

DexVerifier::RegisterMap* DexVerifier::CompressMapDifferential(
    const RegisterMap* map) {
  int orig_size = ComputeRegisterMapSize(map);
  uint8_t* tmp_ptr;
  int addr_width;

  uint8_t format = map->header_->format_;
  switch (format) {
    case kRegMapFormatCompact8:
      addr_width = 1;
      break;
    case kRegMapFormatCompact16:
      addr_width = 2;
      break;
    default:
      LOG(ERROR) << "ERROR: can't compress map with format=" << format;
      return NULL;
  }

  int reg_width = map->header_->reg_width_;
  int num_entries = map->header_->num_entries_;

  if (num_entries <= 1) {
    return NULL;
  }

  /*
   * We don't know how large the compressed data will be. It's possible
   * for it to expand and become larger than the original. The header
   * itself is variable-sized, so we generate everything into a temporary
   * buffer and then copy it to form-fitting storage once we know how big
   * it will be (and that it's smaller than the original).
   *
   * If we use a size that is equal to the size of the input map plus
   * a value longer than a single entry can possibly expand to, we need
   * only check for overflow at the end of each entry. The worst case
   * for a single line is (1 + <ULEB8 address> + <full copy of vector>).
   * Addresses are 16 bits, so that's (1 + 3 + reg_width).
   *
   * The initial address offset and bit vector will take up less than
   * or equal to the amount of space required when uncompressed -- large
   * initial offsets are rejected.
   */
  UniquePtr<uint8_t[]> tmp_buf(new uint8_t[orig_size + (1 + 3 + reg_width)]);

  tmp_ptr = tmp_buf.get();

  const uint8_t* map_data = map->data_;
  const uint8_t* prev_bits;
  uint16_t addr, prev_addr;

  addr = *map_data++;
  if (addr_width > 1)
    addr |= (*map_data++) << 8;

  if (addr >= 128) {
    LOG(ERROR) << "Can't compress map with starting address >= 128";
    return NULL;
  }

  /*
   * Start by writing the initial address and bit vector data. The high
   * bit of the initial address is used to indicate the required address
   * width (which the decoder can't otherwise determine without parsing
   * the compressed data).
   */
  *tmp_ptr++ = addr | (addr_width > 1 ? 0x80 : 0x00);
  memcpy(tmp_ptr, map_data, reg_width);

  prev_bits = map_data;
  prev_addr = addr;

  tmp_ptr += reg_width;
  map_data += reg_width;

  /* Loop over all following entries. */
  for (int entry = 1; entry < num_entries; entry++) {
    int addr_diff;
    uint8_t key;

    /* Pull out the address and figure out how to encode it. */
    addr = *map_data++;
    if (addr_width > 1)
      addr |= (*map_data++) << 8;

    addr_diff = addr - prev_addr;
    DCHECK_GT(addr_diff, 0);
    if (addr_diff < 8) {
      /* Small difference, encode in 3 bits. */
      key = addr_diff -1;          /* set 00000AAA */
    } else {
      /* Large difference, output escape code. */
      key = 0x07;                 /* escape code for AAA */
    }

    int num_bits_changed, first_bit_changed, leb_size;

    leb_size = ComputeBitDiff(prev_bits, map_data, reg_width,
        &first_bit_changed, &num_bits_changed, NULL);

    if (num_bits_changed == 0) {
      /* set B to 1 and CCCC to zero to indicate no bits were changed */
      key |= 0x08;
    } else if (num_bits_changed == 1 && first_bit_changed < 16) {
      /* set B to 0 and CCCC to the index of the changed bit */
      key |= first_bit_changed << 4;
    } else if (num_bits_changed < 15 && leb_size < reg_width) {
      /* set B to 1 and CCCC to the number of bits */
      key |= 0x08 | (num_bits_changed << 4);
    } else {
      /* set B to 1 and CCCC to 0x0f so we store the entire vector */
      key |= 0x08 | 0xf0;
    }

    /*
     * Encode output. Start with the key, follow with the address
     * diff (if it didn't fit in 3 bits), then the changed bit info.
     */
    *tmp_ptr++ = key;
    if ((key & 0x07) == 0x07)
      tmp_ptr = WriteUnsignedLeb128(tmp_ptr, addr_diff);

    if ((key & 0x08) != 0) {
      int bit_count = key >> 4;
      if (bit_count == 0) {
        /* nothing changed, no additional output required */
      } else if (bit_count == 15) {
        /* full vector is most compact representation */
        memcpy(tmp_ptr, map_data, reg_width);
        tmp_ptr += reg_width;
      } else {
        /* write bit indices in ULEB128 format */
        (void) ComputeBitDiff(prev_bits, map_data, reg_width,
               NULL, NULL, tmp_ptr);
        tmp_ptr += leb_size;
      }
    } else {
      /* single-bit changed, value encoded in key byte */
    }

    prev_bits = map_data;
    prev_addr = addr;
    map_data += reg_width;

    /* See if we've run past the original size. */
    if (tmp_ptr - tmp_buf.get() >= orig_size) {
      return NULL;
    }
  }

  /*
   * Create a RegisterMap with the contents.
   *
   * TODO: consider using a threshold other than merely ">=". We would
   * get poorer compression but potentially use less native heap space.
   */
  int new_data_size = tmp_ptr - tmp_buf.get();
  int new_map_size = new_data_size + UnsignedLeb128Size(new_data_size);

  if (new_map_size >= orig_size) {
    return NULL;
  }

  RegisterMap* new_map = new RegisterMap(kRegMapFormatDifferential, reg_width,
      num_entries, new_map_size);

  tmp_ptr = new_map->data_;
  tmp_ptr = WriteUnsignedLeb128(tmp_ptr, new_data_size);
  memcpy(tmp_ptr, tmp_buf.get(), new_data_size);

  return new_map;
}

DexVerifier::RegisterMap* DexVerifier::UncompressMapDifferential(
    const RegisterMap* map) {
  uint8_t format = map->header_->format_;
  RegisterMapFormat new_format;
  int reg_width, num_entries, new_addr_width, new_data_size;

  if (format != kRegMapFormatDifferential) {
    LOG(ERROR) << "Not differential (" << format << ")";
    return NULL;
  }

  reg_width = map->header_->reg_width_;
  num_entries = map->header_->num_entries_;

  /* Get the data size; we can check this at the end. */
  const uint8_t* src_ptr = map->data_;
  int expected_src_len = DecodeUnsignedLeb128(&src_ptr);
  const uint8_t* src_start = src_ptr;

  /* Get the initial address and the 16-bit address flag. */
  int addr = *src_ptr & 0x7f;
  if ((*src_ptr & 0x80) == 0) {
    new_format = kRegMapFormatCompact8;
    new_addr_width = 1;
  } else {
    new_format = kRegMapFormatCompact16;
    new_addr_width = 2;
  }
  src_ptr++;

  /* Now we know enough to allocate the new map. */
  new_data_size = (new_addr_width + reg_width) * num_entries;
  RegisterMap* new_map = new RegisterMap(new_format, reg_width, num_entries,
      new_data_size);

  /* Write the start address and initial bits to the new map. */
  uint8_t* dst_ptr = new_map->data_;

  *dst_ptr++ = addr & 0xff;
  if (new_addr_width > 1)
    *dst_ptr++ = (uint8_t) (addr >> 8);

  memcpy(dst_ptr, src_ptr, reg_width);

  int prev_addr = addr;
  const uint8_t* prev_bits = dst_ptr;    /* point at uncompressed data */

  dst_ptr += reg_width;
  src_ptr += reg_width;

  /* Walk through, uncompressing one line at a time. */
  int entry;
  for (entry = 1; entry < num_entries; entry++) {
    int addr_diff;
    uint8_t key;

    key = *src_ptr++;

    /* Get the address. */
    if ((key & 0x07) == 7) {
      /* Address diff follows in ULEB128. */
      addr_diff = DecodeUnsignedLeb128(&src_ptr);
    } else {
      addr_diff = (key & 0x07) +1;
    }

    addr = prev_addr + addr_diff;
    *dst_ptr++ = addr & 0xff;
    if (new_addr_width > 1)
      *dst_ptr++ = (uint8_t) (addr >> 8);

    /* Unpack the bits. */
    if ((key & 0x08) != 0) {
      int bit_count = (key >> 4);
      if (bit_count == 0) {
        /* No bits changed, just copy previous. */
        memcpy(dst_ptr, prev_bits, reg_width);
      } else if (bit_count == 15) {
        /* Full copy of bit vector is present; ignore prev_bits. */
        memcpy(dst_ptr, src_ptr, reg_width);
        src_ptr += reg_width;
      } else {
        /* Copy previous bits and modify listed indices. */
        memcpy(dst_ptr, prev_bits, reg_width);
        while (bit_count--) {
          int bit_index = DecodeUnsignedLeb128(&src_ptr);
          ToggleBit(dst_ptr, bit_index);
        }
      }
    } else {
      /* Copy previous bits and modify the specified one. */
      memcpy(dst_ptr, prev_bits, reg_width);

      /* One bit, from 0-15 inclusive, was changed. */
      ToggleBit(dst_ptr, key >> 4);
    }

    prev_addr = addr;
    prev_bits = dst_ptr;
    dst_ptr += reg_width;
  }

  if (dst_ptr - new_map->data_ != new_data_size) {
    LOG(ERROR) << "ERROR: output " << dst_ptr - new_map->data_
               << " bytes, expected " << new_data_size;
    free(new_map);
    return NULL;
  }

  if (src_ptr - src_start != expected_src_len) {
    LOG(ERROR) << "ERROR: consumed " << src_ptr - src_start
               << " bytes, expected " << expected_src_len;
    free(new_map);
    return NULL;
  }

  return new_map;
}

}  // namespace art
