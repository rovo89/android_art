// Copyright 2011 Google Inc. All Rights Reserved.

#include "dex_verifier.h"

#include <iostream>

#include "class_linker.h"
#include "dex_file.h"
#include "dex_instruction.h"
#include "dex_instruction_visitor.h"
#include "logging.h"
#include "runtime.h"
#include "stringpiece.h"

namespace art {

/*
 * Returns "true" if the flags indicate that this address holds the start
 * of an instruction.
 */
inline bool InsnIsOpcode(const uint32_t insn_flags[], int addr) {
  return (insn_flags[addr] & DexVerify::kInsnFlagWidthMask) != 0;
}

/*
 * Extract the unsigned 16-bit instruction width from "flags".
 */
inline int InsnGetWidth(const uint32_t insn_flags[], int addr) {
  return insn_flags[addr] & DexVerify::kInsnFlagWidthMask;
}

/*
 * Changed?
 */
inline bool InsnIsChanged(const uint32_t insn_flags[], int addr) {
    return (insn_flags[addr] & DexVerify::kInsnFlagChanged) != 0;
}
inline void InsnSetChanged(uint32_t insn_flags[], int addr, bool changed) {
  if (changed)
    insn_flags[addr] |= DexVerify::kInsnFlagChanged;
  else
    insn_flags[addr] &= ~DexVerify::kInsnFlagChanged;
}

/*
 * Visited?
 */
inline bool InsnIsVisited(const uint32_t insn_flags[], int addr) {
    return (insn_flags[addr] & DexVerify::kInsnFlagVisited) != 0;
}
inline void InsnSetVisited(uint32_t insn_flags[], int addr, bool changed) {
  if (changed)
    insn_flags[addr] |= DexVerify::kInsnFlagVisited;
  else
    insn_flags[addr] &= ~DexVerify::kInsnFlagVisited;
}

/*
 * Visited or changed?
 */
inline bool InsnIsVisitedOrChanged(const uint32_t insn_flags[], int addr) {
  return (insn_flags[addr] & (DexVerify::kInsnFlagVisited |
                              DexVerify::kInsnFlagChanged)) != 0;
}

/*
 * In a "try" block?
 */
inline bool InsnIsInTry(const uint32_t insn_flags[], int addr) {
  return (insn_flags[addr] & DexVerify::kInsnFlagInTry) != 0;
}
inline void InsnSetInTry(uint32_t insn_flags[], int addr, bool inTry) {
  insn_flags[addr] |= DexVerify::kInsnFlagInTry;
}

/*
 * Instruction is a branch target or exception handler?
 */
inline bool InsnIsBranchTarget(const uint32_t insn_flags[], int addr) {
  return (insn_flags[addr] & DexVerify::kInsnFlagBranchTarget) != 0;
}
inline void InsnSetBranchTarget(uint32_t insn_flags[], int addr, bool isBranch)
{
  insn_flags[addr] |= DexVerify::kInsnFlagBranchTarget;
}

/*
 * Instruction is a GC point?
 */
inline bool InsnIsGcPoint(const uint32_t insn_flags[], int addr) {
  return (insn_flags[addr] & DexVerify::kInsnFlagGcPoint) != 0;
}
inline void InsnSetGcPoint(uint32_t insn_flags[], int addr, bool isGcPoint) {
  insn_flags[addr] |= DexVerify::kInsnFlagGcPoint;
}


/*
 * Extract the relative offset from a branch instruction.
 *
 * Returns "false" on failure (e.g. this isn't a branch instruction).
 */
bool GetBranchOffset(const DexFile::CodeItem* code_item,
    const uint32_t insn_flags[], int cur_offset, int32_t* pOffset,
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


/*
 * Verify an array data table.  "cur_offset" is the offset of the
 * fill-array-data instruction.
 */
static bool CheckArrayData(const DexFile::CodeItem* code_item,
    uint32_t cur_offset) {
  const uint32_t insn_count = code_item->insns_size_;
  const uint16_t* insns = code_item->insns_ + cur_offset;
  const uint16_t* array_data;
  int32_t array_data_offset;

  assert(cur_offset < insn_count);

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

/*
 * Perform static checks on a "new-instance" instruction.  Specifically,
 * make sure the class reference isn't for an array class.
 *
 * We don't need the actual class, just a pointer to the class name.
 */
static bool CheckNewInstance(const DexFile* dex_file, uint32_t idx) {
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

/*
 * Perform static checks on a "new-array" instruction.  Specifically, make
 * sure they aren't creating an array of arrays that causes the number of
 * dimensions to exceed 255.
 */
static bool CheckNewArray(const DexFile* dex_file, uint32_t idx) {
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

/*
 * Perform static checks on an instruction that takes a class constant.
 * Ensure that the class index is in the valid range.
 */
static bool CheckTypeIndex(const DexFile* dex_file, uint32_t idx) {
  if (idx >= dex_file->GetHeader().type_ids_size_) {
    LOG(ERROR) << "VFY: bad type index " << idx << " (max "
               << dex_file->GetHeader().type_ids_size_ << ")";
    return false;
  }
  return true;
}

/*
 * Perform static checks on a field get or set instruction.  All we do
 * here is ensure that the field index is in the valid range.
 */
static bool CheckFieldIndex(const DexFile* dex_file, uint32_t idx) {
  if (idx >= dex_file->GetHeader().field_ids_size_) {
    LOG(ERROR) << "VFY: bad field index " << idx << " (max "
               << dex_file->GetHeader().field_ids_size_ << ")";
    return false;
  }
  return true;
}

/*
 * Perform static checks on a method invocation instruction.  All we do
 * here is ensure that the method index is in the valid range.
 */
static bool CheckMethodIndex(const DexFile* dex_file, uint32_t idx) {
  if (idx >= dex_file->GetHeader().method_ids_size_) {
    LOG(ERROR) << "VFY: bad method index " << idx << " (max "
               << dex_file->GetHeader().method_ids_size_ << ")";
    return false;
  }
  return true;
}

/*
 * Ensure that the string index is in the valid range.
 */
static bool CheckStringIndex(const DexFile* dex_file, uint32_t idx) {
  if (idx >= dex_file->GetHeader().string_ids_size_) {
    LOG(ERROR) << "VFY: bad string index " << idx << " (max "
               << dex_file->GetHeader().string_ids_size_ << ")";
    return false;
  }
  return true;
}

/*
 * Ensure that the register index is valid for this code item.
 */
static bool CheckRegisterIndex(const DexFile::CodeItem* code_item, uint32_t idx)
{
  if (idx >= code_item->registers_size_) {
    LOG(ERROR) << "VFY: register index out of range (" << idx << " >= "
               << code_item->registers_size_ << ")";
    return false;
  }
  return true;
}

/*
 * Ensure that the wide register index is valid for this code item.
 */
static bool CheckWideRegisterIndex(const DexFile::CodeItem* code_item,
    uint32_t idx) {
  if (idx + 1 >= code_item->registers_size_) {
    LOG(ERROR) << "VFY: wide register index out of range (" << idx
               << "+1 >= " << code_item->registers_size_ << ")";
    return false;
  }
  return true;
}

/*
 * Check the register indices used in a "vararg" instruction, such as
 * invoke-virtual or filled-new-array.
 *
 * vA holds word count (0-5), args[] have values.
 *
 * There are some tests we don't do here, e.g. we don't try to verify
 * that invoking a method that takes a double is done with consecutive
 * registers.  This requires parsing the target method signature, which
 * we will be doing later on during the code flow analysis.
 */
static bool CheckVarArgRegs(const DexFile::CodeItem* code_item, uint32_t vA,
    uint32_t arg[]) {
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

/*
 * Check the register indices used in a "vararg/range" instruction, such as
 * invoke-virtual/range or filled-new-array/range.
 *
 * vA holds word count, vC holds index of first reg.
 */
static bool CheckVarArgRangeRegs(const DexFile::CodeItem* code_item,
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

/*
 * Verify a switch table. "cur_offset" is the offset of the switch instruction.
 *
 * Updates "insnFlags", setting the "branch target" flag.
 */
static bool CheckSwitchTargets(const DexFile::CodeItem* code_item,
    uint32_t insn_flags[], uint32_t cur_offset) {
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
    LOG(ERROR) << "VFY: wrong signature for switch table (0x" << switch_insns[0]
               << ", wanted 0x" << expected_signature << ")";
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
                 << abs_offset << ") at " << cur_offset << "[" << targ << "]";
      return false;
    }
    InsnSetBranchTarget(insn_flags, abs_offset, true);
  }

  return true;
}

/*
 * Verify that the target of a branch instruction is valid.
 *
 * We don't expect code to jump directly into an exception handler, but
 * it's valid to do so as long as the target isn't a "move-exception"
 * instruction.  We verify that in a later stage.
 *
 * The VM spec doesn't forbid an instruction from branching to itself,
 * but the Dalvik spec declares that only certain instructions can do so.
 *
 * Updates "insnFlags", setting the "branch target" flag.
 */
static bool CheckBranchTarget(const DexFile::CodeItem* code_item,
    uint32_t insn_flags[], int cur_offset) {
  const int insn_count = code_item->insns_size_;
  int32_t offset, abs_offset;
  bool isConditional, selfOkay;

  if (!GetBranchOffset(code_item, insn_flags, cur_offset, &offset,
                       &isConditional, &selfOkay))
    return false;

  if (!selfOkay && offset == 0) {
    LOG(ERROR) << "VFY: branch offset of zero not allowed at" << cur_offset;
    return false;
  }

  /*
   * Check for 32-bit overflow.  This isn't strictly necessary if we can
   * depend on the VM to have identical "wrap-around" behavior, but
   * it's unwise to depend on that.
   */
  if (((int64_t) cur_offset + (int64_t) offset) !=
      (int64_t)(cur_offset + offset)) {
    LOG(ERROR) << "VFY: branch target overflow " << cur_offset << " +"
               << offset;
    return false;
  }
  abs_offset = cur_offset + offset;
  if (abs_offset < 0 || abs_offset >= insn_count ||
      !InsnIsOpcode(insn_flags, abs_offset))
  {
    LOG(ERROR) << "VFY: invalid branch target " << offset << " (-> "
               << abs_offset << ") at " << cur_offset;
    return false;
  }
  InsnSetBranchTarget(insn_flags, abs_offset, true);

  return true;
}

bool CheckInsnWidth(const uint16_t* insns, uint32_t insns_size,
    uint32_t insn_flags[]) {
  const byte* ptr = reinterpret_cast<const byte*>(insns);
  const Instruction* inst = Instruction::At(ptr);

  size_t width = 0;

  while (width < insns_size) {
    insn_flags[width] |= inst->Size();
    width += inst->Size();
    inst = inst->Next();
  }

  if (width != insns_size) {
    LOG(ERROR) << "VFY: code did not end where expected (" << width << " vs. "
               << insns_size << ")";
    return false;
  }

  return true;
}

/*
 * Set the "in try" flags for all instructions protected by "try" statements.
 * Also sets the "branch target" flags for exception handlers.
 *
 * Call this after widths have been set in "insn_flags".
 *
 * Returns "false" if something in the exception table looks fishy, but
 * we're expecting the exception table to be somewhat sane.
 */
static bool ScanTryCatchBlocks(const DexFile::CodeItem* code_item,
    uint32_t insn_flags[]) {
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
      InsnSetInTry(insn_flags, addr, true);
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

      InsnSetBranchTarget(insn_flags, addr, true);
    }

    handlers_ptr = iterator.GetData();
  }

  return true;
}



bool DexVerify::VerifyClass(Class* klass) {
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

bool DexVerify::VerifyMethod(Method* method) {
  const DexCache* dex_cache = method->GetDeclaringClass()->GetDexCache();
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  const DexFile& dex_file = class_linker->FindDexFile(dex_cache);
  const DexFile::CodeItem *code_item = dex_file.GetCodeItem(method->GetCodeItemOffset());

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
   * Sanity-check the register counts.  ins + locals = registers, so make
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
  UniquePtr<uint32_t[]> insn_flags(new uint32_t[code_item->insns_size_]());

  /*
   * Run through the instructions and see if the width checks out.
   */
  if (!CheckInsnWidth(code_item->insns_, code_item->insns_size_, insn_flags.get())) {
    return false;
  }

  /*
   * Flag instructions guarded by a "try" block and check exception handlers.
   */
  if (!ScanTryCatchBlocks(code_item, insn_flags.get())) {
    return false;
  }

  /*
   * Perform static instruction verification.
   */
  if (!VerifyInstructions(&dex_file, code_item, insn_flags.get())) {
    return false;
  }

  /*
   * TODO: Code flow analysis
   */

  return true;
}

bool DexVerify::VerifyInstructions(const DexFile* dex_file,
    const DexFile::CodeItem* code_item, uint32_t insn_flags[]) {
  const byte* ptr = reinterpret_cast<const byte*>(code_item->insns_);
  const Instruction* inst = Instruction::At(ptr);

  /* Flag the start of the method as a branch target. */
  InsnSetBranchTarget(insn_flags, 0, true);

  uint32_t width = 0;
  uint32_t insns_size = code_item->insns_size_;

  while (width < insns_size) {
    if (!VerifyInstruction(dex_file, inst, width, code_item, insn_flags)) {
      LOG(ERROR) << "VFY:  rejecting opcode 0x" << std::hex
                 << (int) inst->Opcode() << " at 0x" << width << std::dec;
      return false;
    }

    /* Flag instructions that are garbage collection points */
    if (inst->IsBranch() || inst->IsSwitch() || inst->IsThrow() ||
        inst->IsReturn()) {
      InsnSetGcPoint(insn_flags, width, true);
    }

    width += inst->Size();
    inst = inst->Next();
  }
  return true;
}

bool DexVerify::VerifyInstruction(const DexFile* dex_file,
    const Instruction* inst, uint32_t code_offset,
    const DexFile::CodeItem* code_item, uint32_t insn_flags[]) {
  Instruction::Code opcode = inst->Opcode();
  bool result = true;
  uint32_t vA, vB, vC;
  uint64_t vB_wide;
  uint32_t arg[5];

  inst->Decode(vA, vB, vB_wide, vC, arg);

  int argumentA = inst->GetVerifyTypeArgumentA();
  int argumentB = inst->GetVerifyTypeArgumentB();
  int argumentC = inst->GetVerifyTypeArgumentC();
  int extra_flags = inst->GetVerifyExtraFlags();

  switch (argumentA) {
    case Instruction::kVerifyRegA:
      result &= CheckRegisterIndex(code_item, vA);
      break;
    case Instruction::kVerifyRegAWide:
      result &= CheckWideRegisterIndex(code_item, vA);
      break;
  }

  switch (argumentB) {
    case Instruction::kVerifyRegB:
      result &= CheckRegisterIndex(code_item, vB);
      break;
    case Instruction::kVerifyRegBField:
      result &= CheckFieldIndex(dex_file, vB);
      break;
    case Instruction::kVerifyRegBMethod:
      result &= CheckMethodIndex(dex_file, vB);
      break;
    case Instruction::kVerifyRegBNewInstance:
      result &= CheckNewInstance(dex_file, vB);
      break;
    case Instruction::kVerifyRegBString:
      result &= CheckStringIndex(dex_file, vB);
      break;
    case Instruction::kVerifyRegBType:
      result &= CheckTypeIndex(dex_file, vB);
      break;
    case Instruction::kVerifyRegBWide:
      result &= CheckWideRegisterIndex(code_item, vB);
      break;
  }

  switch (argumentC) {
    case Instruction::kVerifyRegC:
      result &= CheckRegisterIndex(code_item, vC);
      break;
    case Instruction::kVerifyRegCField:
      result &= CheckFieldIndex(dex_file, vC);
      break;
    case Instruction::kVerifyRegCNewArray:
      result &= CheckNewArray(dex_file, vC);
      break;
    case Instruction::kVerifyRegCType:
      result &= CheckTypeIndex(dex_file, vC);
      break;
    case Instruction::kVerifyRegCWide:
      result &= CheckWideRegisterIndex(code_item, vC);
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
      result &= CheckVarArgRegs(code_item, vA, arg);
      break;
    case Instruction::kVerifyVarArgRange:
      result &= CheckVarArgRangeRegs(code_item, vA, vC);
      break;
    case Instruction::kVerifyError:
      LOG(ERROR) << "VFY: unexpected opcode " << (int) opcode;
      result = false;
      break;
  }

  return result;
};

}  // namespace art
