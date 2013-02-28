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

#include "compiler/dex/compiler_internals.h"
#include "dex_file-inl.h"
#include "gc_map.h"
#include "verifier/dex_gc_map.h"
#include "verifier/method_verifier.h"
#include "ralloc_util.h"
#include "codegen_util.h"

namespace art {

bool IsInexpensiveConstant(CompilationUnit* cu, RegLocation rl_src)
{
  bool res = false;
  if (rl_src.is_const) {
    if (rl_src.wide) {
      if (rl_src.fp) {
         res = cu->cg->InexpensiveConstantDouble(cu->mir_graph->ConstantValueWide(rl_src));
      } else {
         res = cu->cg->InexpensiveConstantLong(cu->mir_graph->ConstantValueWide(rl_src));
      }
    } else {
      if (rl_src.fp) {
         res = cu->cg->InexpensiveConstantFloat(cu->mir_graph->ConstantValue(rl_src));
      } else {
         res = cu->cg->InexpensiveConstantInt(cu->mir_graph->ConstantValue(rl_src));
      }
    }
  }
  return res;
}

void MarkSafepointPC(CompilationUnit* cu, LIR* inst)
{
  inst->def_mask = ENCODE_ALL;
  LIR* safepoint_pc = NewLIR0(cu, kPseudoSafepointPC);
  DCHECK_EQ(safepoint_pc->def_mask, ENCODE_ALL);
}

bool FastInstance(CompilationUnit* cu,  uint32_t field_idx,
                  int& field_offset, bool& is_volatile, bool is_put)
{
  return cu->compiler_driver->ComputeInstanceFieldInfo(
      field_idx, cu->mir_graph->GetCurrentDexCompilationUnit(), field_offset, is_volatile, is_put);
}

/* Convert an instruction to a NOP */
void NopLIR( LIR* lir)
{
  lir->flags.is_nop = true;
}

void SetMemRefType(CompilationUnit* cu, LIR* lir, bool is_load, int mem_type)
{
  uint64_t *mask_ptr;
  uint64_t mask = ENCODE_MEM;;
  Codegen* cg = cu->cg.get();
  DCHECK(cg->GetTargetInstFlags(lir->opcode) & (IS_LOAD | IS_STORE));
  if (is_load) {
    mask_ptr = &lir->use_mask;
  } else {
    mask_ptr = &lir->def_mask;
  }
  /* Clear out the memref flags */
  *mask_ptr &= ~mask;
  /* ..and then add back the one we need */
  switch (mem_type) {
    case kLiteral:
      DCHECK(is_load);
      *mask_ptr |= ENCODE_LITERAL;
      break;
    case kDalvikReg:
      *mask_ptr |= ENCODE_DALVIK_REG;
      break;
    case kHeapRef:
      *mask_ptr |= ENCODE_HEAP_REF;
      break;
    case kMustNotAlias:
      /* Currently only loads can be marked as kMustNotAlias */
      DCHECK(!(cg->GetTargetInstFlags(lir->opcode) & IS_STORE));
      *mask_ptr |= ENCODE_MUST_NOT_ALIAS;
      break;
    default:
      LOG(FATAL) << "Oat: invalid memref kind - " << mem_type;
  }
}

/*
 * Mark load/store instructions that access Dalvik registers through the stack.
 */
void AnnotateDalvikRegAccess(CompilationUnit* cu, LIR* lir, int reg_id, bool is_load, bool is64bit)
{
  SetMemRefType(cu, lir, is_load, kDalvikReg);

  /*
   * Store the Dalvik register id in alias_info. Mark the MSB if it is a 64-bit
   * access.
   */
  lir->alias_info = ENCODE_ALIAS_INFO(reg_id, is64bit);
}

/*
 * Mark the corresponding bit(s).
 */
void SetupRegMask(CompilationUnit* cu, uint64_t* mask, int reg)
{
  Codegen* cg = cu->cg.get();
  *mask |= cg->GetRegMaskCommon(cu, reg);
}

/*
 * Set up the proper fields in the resource mask
 */
void SetupResourceMasks(CompilationUnit* cu, LIR* lir)
{
  int opcode = lir->opcode;
  Codegen* cg = cu->cg.get();

  if (opcode <= 0) {
    lir->use_mask = lir->def_mask = 0;
    return;
  }

  uint64_t flags = cg->GetTargetInstFlags(opcode);

  if (flags & NEEDS_FIXUP) {
    lir->flags.pcRelFixup = true;
  }

  /* Get the starting size of the instruction's template */
  lir->flags.size = cg->GetInsnSize(lir);

  /* Set up the mask for resources that are updated */
  if (flags & (IS_LOAD | IS_STORE)) {
    /* Default to heap - will catch specialized classes later */
    SetMemRefType(cu, lir, flags & IS_LOAD, kHeapRef);
  }

  /*
   * Conservatively assume the branch here will call out a function that in
   * turn will trash everything.
   */
  if (flags & IS_BRANCH) {
    lir->def_mask = lir->use_mask = ENCODE_ALL;
    return;
  }

  if (flags & REG_DEF0) {
    SetupRegMask(cu, &lir->def_mask, lir->operands[0]);
  }

  if (flags & REG_DEF1) {
    SetupRegMask(cu, &lir->def_mask, lir->operands[1]);
  }


  if (flags & SETS_CCODES) {
    lir->def_mask |= ENCODE_CCODE;
  }

  if (flags & (REG_USE0 | REG_USE1 | REG_USE2 | REG_USE3)) {
    int i;

    for (i = 0; i < 4; i++) {
      if (flags & (1 << (kRegUse0 + i))) {
        SetupRegMask(cu, &lir->use_mask, lir->operands[i]);
      }
    }
  }

  if (flags & USES_CCODES) {
    lir->use_mask |= ENCODE_CCODE;
  }

  // Handle target-specific actions
  cg->SetupTargetResourceMasks(cu, lir);
}

/*
 * Debugging macros
 */
#define DUMP_RESOURCE_MASK(X)

/* Pretty-print a LIR instruction */
void DumpLIRInsn(CompilationUnit* cu, LIR* lir, unsigned char* base_addr)
{
  int offset = lir->offset;
  int dest = lir->operands[0];
  const bool dump_nop = (cu->enable_debug & (1 << kDebugShowNops));
  Codegen* cg = cu->cg.get();

  /* Handle pseudo-ops individually, and all regular insns as a group */
  switch (lir->opcode) {
    case kPseudoMethodEntry:
      LOG(INFO) << "-------- method entry "
                << PrettyMethod(cu->method_idx, *cu->dex_file);
      break;
    case kPseudoMethodExit:
      LOG(INFO) << "-------- Method_Exit";
      break;
    case kPseudoBarrier:
      LOG(INFO) << "-------- BARRIER";
      break;
    case kPseudoEntryBlock:
      LOG(INFO) << "-------- entry offset: 0x" << std::hex << dest;
      break;
    case kPseudoDalvikByteCodeBoundary:
      if (lir->operands[0] == 0) {
         lir->operands[0] = reinterpret_cast<uintptr_t>("No instruction string");
      }
      LOG(INFO) << "-------- dalvik offset: 0x" << std::hex
                << lir->dalvik_offset << " @ " << reinterpret_cast<char*>(lir->operands[0]);
      break;
    case kPseudoExitBlock:
      LOG(INFO) << "-------- exit offset: 0x" << std::hex << dest;
      break;
    case kPseudoPseudoAlign4:
      LOG(INFO) << reinterpret_cast<uintptr_t>(base_addr) + offset << " (0x" << std::hex
                << offset << "): .align4";
      break;
    case kPseudoEHBlockLabel:
      LOG(INFO) << "Exception_Handling:";
      break;
    case kPseudoTargetLabel:
    case kPseudoNormalBlockLabel:
      LOG(INFO) << "L" << reinterpret_cast<void*>(lir) << ":";
      break;
    case kPseudoThrowTarget:
      LOG(INFO) << "LT" << reinterpret_cast<void*>(lir) << ":";
      break;
    case kPseudoIntrinsicRetry:
      LOG(INFO) << "IR" << reinterpret_cast<void*>(lir) << ":";
      break;
    case kPseudoSuspendTarget:
      LOG(INFO) << "LS" << reinterpret_cast<void*>(lir) << ":";
      break;
    case kPseudoSafepointPC:
      LOG(INFO) << "LsafepointPC_0x" << std::hex << lir->offset << "_" << lir->dalvik_offset << ":";
      break;
    case kPseudoExportedPC:
      LOG(INFO) << "LexportedPC_0x" << std::hex << lir->offset << "_" << lir->dalvik_offset << ":";
      break;
    case kPseudoCaseLabel:
      LOG(INFO) << "LC" << reinterpret_cast<void*>(lir) << ": Case target 0x"
                << std::hex << lir->operands[0] << "|" << std::dec <<
        lir->operands[0];
      break;
    default:
      if (lir->flags.is_nop && !dump_nop) {
        break;
      } else {
        std::string op_name(cg->BuildInsnString(cg->GetTargetInstName(lir->opcode),
                                               lir, base_addr));
        std::string op_operands(cg->BuildInsnString(cg->GetTargetInstFmt(lir->opcode),
                                                    lir, base_addr));
        LOG(INFO) << StringPrintf("%05x: %-9s%s%s",
                                  reinterpret_cast<unsigned int>(base_addr + offset),
                                  op_name.c_str(), op_operands.c_str(),
                                  lir->flags.is_nop ? "(nop)" : "");
      }
      break;
  }

  if (lir->use_mask && (!lir->flags.is_nop || dump_nop)) {
    DUMP_RESOURCE_MASK(DumpResourceMask((LIR* ) lir, lir->use_mask, "use"));
  }
  if (lir->def_mask && (!lir->flags.is_nop || dump_nop)) {
    DUMP_RESOURCE_MASK(DumpResourceMask((LIR* ) lir, lir->def_mask, "def"));
  }
}

void DumpPromotionMap(CompilationUnit *cu)
{
  Codegen* cg = cu->cg.get();
  int num_regs = cu->num_dalvik_registers + cu->num_compiler_temps + 1;
  for (int i = 0; i < num_regs; i++) {
    PromotionMap v_reg_map = cu->promotion_map[i];
    std::string buf;
    if (v_reg_map.fp_location == kLocPhysReg) {
      StringAppendF(&buf, " : s%d", v_reg_map.FpReg & cg->FpRegMask());
    }

    std::string buf3;
    if (i < cu->num_dalvik_registers) {
      StringAppendF(&buf3, "%02d", i);
    } else if (i == cu->method_sreg) {
      buf3 = "Method*";
    } else {
      StringAppendF(&buf3, "ct%d", i - cu->num_dalvik_registers);
    }

    LOG(INFO) << StringPrintf("V[%s] -> %s%d%s", buf3.c_str(),
                              v_reg_map.core_location == kLocPhysReg ?
                              "r" : "SP+", v_reg_map.core_location == kLocPhysReg ?
                              v_reg_map.core_reg : SRegOffset(cu, i),
                              buf.c_str());
  }
}

/* Dump a mapping table */
static void DumpMappingTable(const char* table_name, const std::string& descriptor,
                             const std::string& name, const std::string& signature,
                             const std::vector<uint32_t>& v) {
  if (v.size() > 0) {
    std::string line(StringPrintf("\n  %s %s%s_%s_table[%zu] = {", table_name,
                     descriptor.c_str(), name.c_str(), signature.c_str(), v.size()));
    std::replace(line.begin(), line.end(), ';', '_');
    LOG(INFO) << line;
    for (uint32_t i = 0; i < v.size(); i+=2) {
      line = StringPrintf("    {0x%05x, 0x%04x},", v[i], v[i+1]);
      LOG(INFO) << line;
    }
    LOG(INFO) <<"  };\n\n";
  }
}

/* Dump instructions and constant pool contents */
void CodegenDump(CompilationUnit* cu)
{
  LOG(INFO) << "Dumping LIR insns for "
            << PrettyMethod(cu->method_idx, *cu->dex_file);
  LIR* lir_insn;
  int insns_size = cu->code_item->insns_size_in_code_units_;

  LOG(INFO) << "Regs (excluding ins) : " << cu->num_regs;
  LOG(INFO) << "Ins          : " << cu->num_ins;
  LOG(INFO) << "Outs         : " << cu->num_outs;
  LOG(INFO) << "CoreSpills       : " << cu->num_core_spills;
  LOG(INFO) << "FPSpills       : " << cu->num_fp_spills;
  LOG(INFO) << "CompilerTemps    : " << cu->num_compiler_temps;
  LOG(INFO) << "Frame size       : " << cu->frame_size;
  LOG(INFO) << "code size is " << cu->total_size <<
    " bytes, Dalvik size is " << insns_size * 2;
  LOG(INFO) << "expansion factor: "
            << static_cast<float>(cu->total_size) / static_cast<float>(insns_size * 2);
  DumpPromotionMap(cu);
  for (lir_insn = cu->first_lir_insn; lir_insn != NULL; lir_insn = lir_insn->next) {
    DumpLIRInsn(cu, lir_insn, 0);
  }
  for (lir_insn = cu->literal_list; lir_insn != NULL; lir_insn = lir_insn->next) {
    LOG(INFO) << StringPrintf("%x (%04x): .word (%#x)", lir_insn->offset, lir_insn->offset,
                              lir_insn->operands[0]);
  }

  const DexFile::MethodId& method_id =
      cu->dex_file->GetMethodId(cu->method_idx);
  std::string signature(cu->dex_file->GetMethodSignature(method_id));
  std::string name(cu->dex_file->GetMethodName(method_id));
  std::string descriptor(cu->dex_file->GetMethodDeclaringClassDescriptor(method_id));

  // Dump mapping tables
  DumpMappingTable("PC2Dex_MappingTable", descriptor, name, signature, cu->pc2dexMappingTable);
  DumpMappingTable("Dex2PC_MappingTable", descriptor, name, signature, cu->dex2pcMappingTable);
}


LIR* RawLIR(CompilationUnit* cu, int dalvik_offset, int opcode, int op0,
      int op1, int op2, int op3, int op4, LIR* target)
{
  LIR* insn = static_cast<LIR*>(NewMem(cu, sizeof(LIR), true, kAllocLIR));
  insn->dalvik_offset = dalvik_offset;
  insn->opcode = opcode;
  insn->operands[0] = op0;
  insn->operands[1] = op1;
  insn->operands[2] = op2;
  insn->operands[3] = op3;
  insn->operands[4] = op4;
  insn->target = target;
  SetupResourceMasks(cu, insn);
  if ((opcode == kPseudoTargetLabel) || (opcode == kPseudoSafepointPC) ||
      (opcode == kPseudoExportedPC)) {
    // Always make labels scheduling barriers
    insn->use_mask = insn->def_mask = ENCODE_ALL;
  }
  return insn;
}

/*
 * The following are building blocks to construct low-level IRs with 0 - 4
 * operands.
 */
LIR* NewLIR0(CompilationUnit* cu, int opcode)
{
  Codegen* cg = cu->cg.get();
  DCHECK(is_pseudo_opcode(opcode) || (cg->GetTargetInstFlags(opcode) & NO_OPERAND))
      << cg->GetTargetInstName(opcode) << " " << opcode << " "
      << PrettyMethod(cu->method_idx, *cu->dex_file) << " "
      << cu->current_dalvik_offset;
  LIR* insn = RawLIR(cu, cu->current_dalvik_offset, opcode);
  AppendLIR(cu, insn);
  return insn;
}

LIR* NewLIR1(CompilationUnit* cu, int opcode,
               int dest)
{
  Codegen* cg = cu->cg.get();
  DCHECK(is_pseudo_opcode(opcode) || (cg->GetTargetInstFlags(opcode) & IS_UNARY_OP))
      << cg->GetTargetInstName(opcode) << " " << opcode << " "
      << PrettyMethod(cu->method_idx, *cu->dex_file) << " "
      << cu->current_dalvik_offset;
  LIR* insn = RawLIR(cu, cu->current_dalvik_offset, opcode, dest);
  AppendLIR(cu, insn);
  return insn;
}

LIR* NewLIR2(CompilationUnit* cu, int opcode,
               int dest, int src1)
{
  Codegen* cg = cu->cg.get();
  DCHECK(is_pseudo_opcode(opcode) || (cg->GetTargetInstFlags(opcode) & IS_BINARY_OP))
      << cg->GetTargetInstName(opcode) << " " << opcode << " "
      << PrettyMethod(cu->method_idx, *cu->dex_file) << " "
      << cu->current_dalvik_offset;
  LIR* insn = RawLIR(cu, cu->current_dalvik_offset, opcode, dest, src1);
  AppendLIR(cu, insn);
  return insn;
}

LIR* NewLIR3(CompilationUnit* cu, int opcode,
               int dest, int src1, int src2)
{
  Codegen* cg = cu->cg.get();
  DCHECK(is_pseudo_opcode(opcode) || (cg->GetTargetInstFlags(opcode) & IS_TERTIARY_OP))
      << cg->GetTargetInstName(opcode) << " " << opcode << " "
      << PrettyMethod(cu->method_idx, *cu->dex_file) << " "
      << cu->current_dalvik_offset;
  LIR* insn = RawLIR(cu, cu->current_dalvik_offset, opcode, dest, src1, src2);
  AppendLIR(cu, insn);
  return insn;
}

LIR* NewLIR4(CompilationUnit* cu, int opcode,
      int dest, int src1, int src2, int info)
{
  Codegen* cg = cu->cg.get();
  DCHECK(is_pseudo_opcode(opcode) || (cg->GetTargetInstFlags(opcode) & IS_QUAD_OP))
      << cg->GetTargetInstName(opcode) << " " << opcode << " "
      << PrettyMethod(cu->method_idx, *cu->dex_file) << " "
      << cu->current_dalvik_offset;
  LIR* insn = RawLIR(cu, cu->current_dalvik_offset, opcode, dest, src1, src2, info);
  AppendLIR(cu, insn);
  return insn;
}

LIR* NewLIR5(CompilationUnit* cu, int opcode,
       int dest, int src1, int src2, int info1, int info2)
{
  Codegen* cg = cu->cg.get();
  DCHECK(is_pseudo_opcode(opcode) || (cg->GetTargetInstFlags(opcode) & IS_QUIN_OP))
      << cg->GetTargetInstName(opcode) << " " << opcode << " "
      << PrettyMethod(cu->method_idx, *cu->dex_file) << " "
      << cu->current_dalvik_offset;
  LIR* insn = RawLIR(cu, cu->current_dalvik_offset, opcode, dest, src1, src2, info1, info2);
  AppendLIR(cu, insn);
  return insn;
}

/*
 * Search the existing constants in the literal pool for an exact or close match
 * within specified delta (greater or equal to 0).
 */
LIR* ScanLiteralPool(LIR* data_target, int value, unsigned int delta)
{
  while (data_target) {
    if ((static_cast<unsigned>(value - data_target->operands[0])) <= delta)
      return data_target;
    data_target = data_target->next;
  }
  return NULL;
}

/* Search the existing constants in the literal pool for an exact wide match */
LIR* ScanLiteralPoolWide(LIR* data_target, int val_lo, int val_hi)
{
  bool lo_match = false;
  LIR* lo_target = NULL;
  while (data_target) {
    if (lo_match && (data_target->operands[0] == val_hi)) {
      // Record high word in case we need to expand this later.
      lo_target->operands[1] = val_hi;
      return lo_target;
    }
    lo_match = false;
    if (data_target->operands[0] == val_lo) {
      lo_match = true;
      lo_target = data_target;
    }
    data_target = data_target->next;
  }
  return NULL;
}

/*
 * The following are building blocks to insert constants into the pool or
 * instruction streams.
 */

/* Add a 32-bit constant to the constant pool */
LIR* AddWordData(CompilationUnit* cu, LIR* *constant_list_p, int value)
{
  /* Add the constant to the literal pool */
  if (constant_list_p) {
    LIR* new_value = static_cast<LIR*>(NewMem(cu, sizeof(LIR), true, kAllocData));
    new_value->operands[0] = value;
    new_value->next = *constant_list_p;
    *constant_list_p = new_value;
    return new_value;
  }
  return NULL;
}

/* Add a 64-bit constant to the constant pool or mixed with code */
LIR* AddWideData(CompilationUnit* cu, LIR* *constant_list_p,
               int val_lo, int val_hi)
{
  AddWordData(cu, constant_list_p, val_hi);
  return AddWordData(cu, constant_list_p, val_lo);
}

static void PushWord(std::vector<uint8_t>&buf, int data) {
  buf.push_back( data & 0xff);
  buf.push_back( (data >> 8) & 0xff);
  buf.push_back( (data >> 16) & 0xff);
  buf.push_back( (data >> 24) & 0xff);
}

static void AlignBuffer(std::vector<uint8_t>&buf, size_t offset) {
  while (buf.size() < offset) {
    buf.push_back(0);
  }
}

/* Write the literal pool to the output stream */
static void InstallLiteralPools(CompilationUnit* cu)
{
  AlignBuffer(cu->code_buffer, cu->data_offset);
  LIR* data_lir = cu->literal_list;
  while (data_lir != NULL) {
    PushWord(cu->code_buffer, data_lir->operands[0]);
    data_lir = NEXT_LIR(data_lir);
  }
  // Push code and method literals, record offsets for the compiler to patch.
  data_lir = cu->code_literal_list;
  while (data_lir != NULL) {
    uint32_t target = data_lir->operands[0];
    cu->compiler_driver->AddCodePatch(cu->dex_file,
                                      cu->method_idx,
                                      cu->invoke_type,
                                      target,
                                      static_cast<InvokeType>(data_lir->operands[1]),
                                      cu->code_buffer.size());
    const DexFile::MethodId& id = cu->dex_file->GetMethodId(target);
    // unique based on target to ensure code deduplication works
    uint32_t unique_patch_value = reinterpret_cast<uint32_t>(&id);
    PushWord(cu->code_buffer, unique_patch_value);
    data_lir = NEXT_LIR(data_lir);
  }
  data_lir = cu->method_literal_list;
  while (data_lir != NULL) {
    uint32_t target = data_lir->operands[0];
    cu->compiler_driver->AddMethodPatch(cu->dex_file,
                                        cu->method_idx,
                                        cu->invoke_type,
                                        target,
                                        static_cast<InvokeType>(data_lir->operands[1]),
                                        cu->code_buffer.size());
    const DexFile::MethodId& id = cu->dex_file->GetMethodId(target);
    // unique based on target to ensure code deduplication works
    uint32_t unique_patch_value = reinterpret_cast<uint32_t>(&id);
    PushWord(cu->code_buffer, unique_patch_value);
    data_lir = NEXT_LIR(data_lir);
  }
}

/* Write the switch tables to the output stream */
static void InstallSwitchTables(CompilationUnit* cu)
{
  GrowableListIterator iterator;
  GrowableListIteratorInit(&cu->switch_tables, &iterator);
  while (true) {
    Codegen::SwitchTable* tab_rec =
      reinterpret_cast<Codegen::SwitchTable*>(GrowableListIteratorNext( &iterator));
    if (tab_rec == NULL) break;
    AlignBuffer(cu->code_buffer, tab_rec->offset);
    /*
     * For Arm, our reference point is the address of the bx
     * instruction that does the launch, so we have to subtract
     * the auto pc-advance.  For other targets the reference point
     * is a label, so we can use the offset as-is.
     */
    int bx_offset = INVALID_OFFSET;
    switch (cu->instruction_set) {
      case kThumb2:
        bx_offset = tab_rec->anchor->offset + 4;
        break;
      case kX86:
        bx_offset = 0;
        break;
      case kMips:
        bx_offset = tab_rec->anchor->offset;
        break;
      default: LOG(FATAL) << "Unexpected instruction set: " << cu->instruction_set;
    }
    if (cu->verbose) {
      LOG(INFO) << "Switch table for offset 0x" << std::hex << bx_offset;
    }
    if (tab_rec->table[0] == Instruction::kSparseSwitchSignature) {
      const int* keys = reinterpret_cast<const int*>(&(tab_rec->table[2]));
      for (int elems = 0; elems < tab_rec->table[1]; elems++) {
        int disp = tab_rec->targets[elems]->offset - bx_offset;
        if (cu->verbose) {
          LOG(INFO) << "  Case[" << elems << "] key: 0x"
                    << std::hex << keys[elems] << ", disp: 0x"
                    << std::hex << disp;
        }
        PushWord(cu->code_buffer, keys[elems]);
        PushWord(cu->code_buffer,
          tab_rec->targets[elems]->offset - bx_offset);
      }
    } else {
      DCHECK_EQ(static_cast<int>(tab_rec->table[0]),
                static_cast<int>(Instruction::kPackedSwitchSignature));
      for (int elems = 0; elems < tab_rec->table[1]; elems++) {
        int disp = tab_rec->targets[elems]->offset - bx_offset;
        if (cu->verbose) {
          LOG(INFO) << "  Case[" << elems << "] disp: 0x"
                    << std::hex << disp;
        }
        PushWord(cu->code_buffer, tab_rec->targets[elems]->offset - bx_offset);
      }
    }
  }
}

/* Write the fill array dta to the output stream */
static void InstallFillArrayData(CompilationUnit* cu)
{
  GrowableListIterator iterator;
  GrowableListIteratorInit(&cu->fill_array_data, &iterator);
  while (true) {
    Codegen::FillArrayData *tab_rec =
        reinterpret_cast<Codegen::FillArrayData*>(GrowableListIteratorNext( &iterator));
    if (tab_rec == NULL) break;
    AlignBuffer(cu->code_buffer, tab_rec->offset);
    for (int i = 0; i < (tab_rec->size + 1) / 2; i++) {
      cu->code_buffer.push_back( tab_rec->table[i] & 0xFF);
      cu->code_buffer.push_back( (tab_rec->table[i] >> 8) & 0xFF);
    }
  }
}

static int AssignLiteralOffsetCommon(LIR* lir, int offset)
{
  for (;lir != NULL; lir = lir->next) {
    lir->offset = offset;
    offset += 4;
  }
  return offset;
}

// Make sure we have a code address for every declared catch entry
static bool VerifyCatchEntries(CompilationUnit* cu)
{
  bool success = true;
  for (std::set<uint32_t>::const_iterator it = cu->mir_graph->catches_.begin();
       it != cu->mir_graph->catches_.end(); ++it) {
    uint32_t dex_pc = *it;
    bool found = false;
    for (size_t i = 0; i < cu->dex2pcMappingTable.size(); i += 2) {
      if (dex_pc == cu->dex2pcMappingTable[i+1]) {
        found = true;
        break;
      }
    }
    if (!found) {
      LOG(INFO) << "Missing native PC for catch entry @ 0x" << std::hex << dex_pc;
      success = false;
    }
  }
  // Now, try in the other direction
  for (size_t i = 0; i < cu->dex2pcMappingTable.size(); i += 2) {
    uint32_t dex_pc = cu->dex2pcMappingTable[i+1];
    if (cu->mir_graph->catches_.find(dex_pc) == cu->mir_graph->catches_.end()) {
      LOG(INFO) << "Unexpected catch entry @ dex pc 0x" << std::hex << dex_pc;
      success = false;
    }
  }
  if (!success) {
    LOG(INFO) << "Bad dex2pcMapping table in " << PrettyMethod(cu->method_idx, *cu->dex_file);
    LOG(INFO) << "Entries @ decode: " << cu->mir_graph->catches_.size() << ", Entries in table: "
              << cu->dex2pcMappingTable.size()/2;
  }
  return success;
}


static void CreateMappingTables(CompilationUnit* cu)
{
  for (LIR* tgt_lir = cu->first_lir_insn; tgt_lir != NULL; tgt_lir = NEXT_LIR(tgt_lir)) {
    if (!tgt_lir->flags.is_nop && (tgt_lir->opcode == kPseudoSafepointPC)) {
      cu->pc2dexMappingTable.push_back(tgt_lir->offset);
      cu->pc2dexMappingTable.push_back(tgt_lir->dalvik_offset);
    }
    if (!tgt_lir->flags.is_nop && (tgt_lir->opcode == kPseudoExportedPC)) {
      cu->dex2pcMappingTable.push_back(tgt_lir->offset);
      cu->dex2pcMappingTable.push_back(tgt_lir->dalvik_offset);
    }
  }
  if (kIsDebugBuild) {
    DCHECK(VerifyCatchEntries(cu));
  }
  cu->combined_mapping_table.push_back(cu->pc2dexMappingTable.size() +
                                        cu->dex2pcMappingTable.size());
  cu->combined_mapping_table.push_back(cu->pc2dexMappingTable.size());
  cu->combined_mapping_table.insert(cu->combined_mapping_table.end(),
                                     cu->pc2dexMappingTable.begin(),
                                     cu->pc2dexMappingTable.end());
  cu->combined_mapping_table.insert(cu->combined_mapping_table.end(),
                                     cu->dex2pcMappingTable.begin(),
                                     cu->dex2pcMappingTable.end());
}

class NativePcToReferenceMapBuilder {
 public:
  NativePcToReferenceMapBuilder(std::vector<uint8_t>* table,
                                size_t entries, uint32_t max_native_offset,
                                size_t references_width) : entries_(entries),
                                references_width_(references_width), in_use_(entries),
                                table_(table) {
    // Compute width in bytes needed to hold max_native_offset.
    native_offset_width_ = 0;
    while (max_native_offset != 0) {
      native_offset_width_++;
      max_native_offset >>= 8;
    }
    // Resize table and set up header.
    table->resize((EntryWidth() * entries) + sizeof(uint32_t));
    CHECK_LT(native_offset_width_, 1U << 3);
    (*table)[0] = native_offset_width_ & 7;
    CHECK_LT(references_width_, 1U << 13);
    (*table)[0] |= (references_width_ << 3) & 0xFF;
    (*table)[1] = (references_width_ >> 5) & 0xFF;
    CHECK_LT(entries, 1U << 16);
    (*table)[2] = entries & 0xFF;
    (*table)[3] = (entries >> 8) & 0xFF;
  }

  void AddEntry(uint32_t native_offset, const uint8_t* references) {
    size_t table_index = TableIndex(native_offset);
    while (in_use_[table_index]) {
      table_index = (table_index + 1) % entries_;
    }
    in_use_[table_index] = true;
    SetNativeOffset(table_index, native_offset);
    DCHECK_EQ(native_offset, GetNativeOffset(table_index));
    SetReferences(table_index, references);
  }

 private:
  size_t TableIndex(uint32_t native_offset) {
    return NativePcOffsetToReferenceMap::Hash(native_offset) % entries_;
  }

  uint32_t GetNativeOffset(size_t table_index) {
    uint32_t native_offset = 0;
    size_t table_offset = (table_index * EntryWidth()) + sizeof(uint32_t);
    for (size_t i = 0; i < native_offset_width_; i++) {
      native_offset |= (*table_)[table_offset + i] << (i * 8);
    }
    return native_offset;
  }

  void SetNativeOffset(size_t table_index, uint32_t native_offset) {
    size_t table_offset = (table_index * EntryWidth()) + sizeof(uint32_t);
    for (size_t i = 0; i < native_offset_width_; i++) {
      (*table_)[table_offset + i] = (native_offset >> (i * 8)) & 0xFF;
    }
  }

  void SetReferences(size_t table_index, const uint8_t* references) {
    size_t table_offset = (table_index * EntryWidth()) + sizeof(uint32_t);
    memcpy(&(*table_)[table_offset + native_offset_width_], references, references_width_);
  }

  size_t EntryWidth() const {
    return native_offset_width_ + references_width_;
  }

  // Number of entries in the table.
  const size_t entries_;
  // Number of bytes used to encode the reference bitmap.
  const size_t references_width_;
  // Number of bytes used to encode a native offset.
  size_t native_offset_width_;
  // Entries that are in use.
  std::vector<bool> in_use_;
  // The table we're building.
  std::vector<uint8_t>* const table_;
};

static void CreateNativeGcMap(CompilationUnit* cu) {
  const std::vector<uint32_t>& mapping_table = cu->pc2dexMappingTable;
  uint32_t max_native_offset = 0;
  for (size_t i = 0; i < mapping_table.size(); i += 2) {
    uint32_t native_offset = mapping_table[i + 0];
    if (native_offset > max_native_offset) {
      max_native_offset = native_offset;
    }
  }
  CompilerDriver::MethodReference method_ref(cu->dex_file, cu->method_idx);
  const std::vector<uint8_t>* gc_map_raw = verifier::MethodVerifier::GetDexGcMap(method_ref);
  verifier::DexPcToReferenceMap dex_gc_map(&(*gc_map_raw)[4], gc_map_raw->size() - 4);
  // Compute native offset to references size.
  NativePcToReferenceMapBuilder native_gc_map_builder(&cu->native_gc_map,
                                                      mapping_table.size() / 2, max_native_offset,
                                                      dex_gc_map.RegWidth());

  for (size_t i = 0; i < mapping_table.size(); i += 2) {
    uint32_t native_offset = mapping_table[i + 0];
    uint32_t dex_pc = mapping_table[i + 1];
    const uint8_t* references = dex_gc_map.FindBitMap(dex_pc, false);
    CHECK(references != NULL) << "Missing ref for dex pc 0x" << std::hex << dex_pc;
    native_gc_map_builder.AddEntry(native_offset, references);
  }
}

/* Determine the offset of each literal field */
static int AssignLiteralOffset(CompilationUnit* cu, int offset)
{
  offset = AssignLiteralOffsetCommon(cu->literal_list, offset);
  offset = AssignLiteralOffsetCommon(cu->code_literal_list, offset);
  offset = AssignLiteralOffsetCommon(cu->method_literal_list, offset);
  return offset;
}

static int AssignSwitchTablesOffset(CompilationUnit* cu, int offset)
{
  GrowableListIterator iterator;
  GrowableListIteratorInit(&cu->switch_tables, &iterator);
  while (true) {
    Codegen::SwitchTable *tab_rec =
        reinterpret_cast<Codegen::SwitchTable*>(GrowableListIteratorNext(&iterator));
    if (tab_rec == NULL) break;
    tab_rec->offset = offset;
    if (tab_rec->table[0] == Instruction::kSparseSwitchSignature) {
      offset += tab_rec->table[1] * (sizeof(int) * 2);
    } else {
      DCHECK_EQ(static_cast<int>(tab_rec->table[0]),
                static_cast<int>(Instruction::kPackedSwitchSignature));
      offset += tab_rec->table[1] * sizeof(int);
    }
  }
  return offset;
}

static int AssignFillArrayDataOffset(CompilationUnit* cu, int offset)
{
  GrowableListIterator iterator;
  GrowableListIteratorInit(&cu->fill_array_data, &iterator);
  while (true) {
    Codegen::FillArrayData *tab_rec =
        reinterpret_cast<Codegen::FillArrayData*>(GrowableListIteratorNext(&iterator));
    if (tab_rec == NULL) break;
    tab_rec->offset = offset;
    offset += tab_rec->size;
    // word align
    offset = (offset + 3) & ~3;
    }
  return offset;
}

// LIR offset assignment.
static int AssignInsnOffsets(CompilationUnit* cu)
{
  LIR* lir;
  int offset = 0;

  for (lir = cu->first_lir_insn; lir != NULL; lir = NEXT_LIR(lir)) {
    lir->offset = offset;
    if (lir->opcode >= 0) {
      if (!lir->flags.is_nop) {
        offset += lir->flags.size;
      }
    } else if (lir->opcode == kPseudoPseudoAlign4) {
      if (offset & 0x2) {
        offset += 2;
        lir->operands[0] = 1;
      } else {
        lir->operands[0] = 0;
      }
    }
    /* Pseudo opcodes don't consume space */
  }

  return offset;
}

/*
 * Walk the compilation unit and assign offsets to instructions
 * and literals and compute the total size of the compiled unit.
 */
static void AssignOffsets(CompilationUnit* cu)
{
  int offset = AssignInsnOffsets(cu);

  /* Const values have to be word aligned */
  offset = (offset + 3) & ~3;

  /* Set up offsets for literals */
  cu->data_offset = offset;

  offset = AssignLiteralOffset(cu, offset);

  offset = AssignSwitchTablesOffset(cu, offset);

  offset = AssignFillArrayDataOffset(cu, offset);

  cu->total_size = offset;
}

/*
 * Go over each instruction in the list and calculate the offset from the top
 * before sending them off to the assembler. If out-of-range branch distance is
 * seen rearrange the instructions a bit to correct it.
 */
void AssembleLIR(CompilationUnit* cu)
{
  Codegen* cg = cu->cg.get();
  AssignOffsets(cu);
  int assembler_retries = 0;
  /*
   * Assemble here.  Note that we generate code with optimistic assumptions
   * and if found now to work, we'll have to redo the sequence and retry.
   */

  while (true) {
    AssemblerStatus res = cg->AssembleInstructions(cu, 0);
    if (res == kSuccess) {
      break;
    } else {
      assembler_retries++;
      if (assembler_retries > MAX_ASSEMBLER_RETRIES) {
        CodegenDump(cu);
        LOG(FATAL) << "Assembler error - too many retries";
      }
      // Redo offsets and try again
      AssignOffsets(cu);
      cu->code_buffer.clear();
    }
  }

  // Install literals
  InstallLiteralPools(cu);

  // Install switch tables
  InstallSwitchTables(cu);

  // Install fill array data
  InstallFillArrayData(cu);

  // Create the mapping table and native offset to reference map.
  CreateMappingTables(cu);

  CreateNativeGcMap(cu);
}

/*
 * Insert a kPseudoCaseLabel at the beginning of the Dalvik
 * offset vaddr.  This label will be used to fix up the case
 * branch table during the assembly phase.  Be sure to set
 * all resource flags on this to prevent code motion across
 * target boundaries.  KeyVal is just there for debugging.
 */
static LIR* InsertCaseLabel(CompilationUnit* cu, int vaddr, int keyVal)
{
  SafeMap<unsigned int, LIR*>::iterator it;
  it = cu->boundary_map.find(vaddr);
  if (it == cu->boundary_map.end()) {
    LOG(FATAL) << "Error: didn't find vaddr 0x" << std::hex << vaddr;
  }
  LIR* new_label = static_cast<LIR*>(NewMem(cu, sizeof(LIR), true, kAllocLIR));
  new_label->dalvik_offset = vaddr;
  new_label->opcode = kPseudoCaseLabel;
  new_label->operands[0] = keyVal;
  InsertLIRAfter(it->second, new_label);
  return new_label;
}

static void MarkPackedCaseLabels(CompilationUnit* cu, Codegen::SwitchTable *tab_rec)
{
  const uint16_t* table = tab_rec->table;
  int base_vaddr = tab_rec->vaddr;
  const int *targets = reinterpret_cast<const int*>(&table[4]);
  int entries = table[1];
  int low_key = s4FromSwitchData(&table[2]);
  for (int i = 0; i < entries; i++) {
    tab_rec->targets[i] = InsertCaseLabel(cu, base_vaddr + targets[i], i + low_key);
  }
}

static void MarkSparseCaseLabels(CompilationUnit* cu, Codegen::SwitchTable *tab_rec)
{
  const uint16_t* table = tab_rec->table;
  int base_vaddr = tab_rec->vaddr;
  int entries = table[1];
  const int* keys = reinterpret_cast<const int*>(&table[2]);
  const int* targets = &keys[entries];
  for (int i = 0; i < entries; i++) {
    tab_rec->targets[i] = InsertCaseLabel(cu, base_vaddr + targets[i], keys[i]);
  }
}

void ProcessSwitchTables(CompilationUnit* cu)
{
  GrowableListIterator iterator;
  GrowableListIteratorInit(&cu->switch_tables, &iterator);
  while (true) {
    Codegen::SwitchTable *tab_rec =
        reinterpret_cast<Codegen::SwitchTable*>(GrowableListIteratorNext(&iterator));
    if (tab_rec == NULL) break;
    if (tab_rec->table[0] == Instruction::kPackedSwitchSignature) {
      MarkPackedCaseLabels(cu, tab_rec);
    } else if (tab_rec->table[0] == Instruction::kSparseSwitchSignature) {
      MarkSparseCaseLabels(cu, tab_rec);
    } else {
      LOG(FATAL) << "Invalid switch table";
    }
  }
}

void DumpSparseSwitchTable(const uint16_t* table)
  /*
   * Sparse switch data format:
   *  ushort ident = 0x0200   magic value
   *  ushort size       number of entries in the table; > 0
   *  int keys[size]      keys, sorted low-to-high; 32-bit aligned
   *  int targets[size]     branch targets, relative to switch opcode
   *
   * Total size is (2+size*4) 16-bit code units.
   */
{
  uint16_t ident = table[0];
  int entries = table[1];
  const int* keys = reinterpret_cast<const int*>(&table[2]);
  const int* targets = &keys[entries];
  LOG(INFO) <<  "Sparse switch table - ident:0x" << std::hex << ident
            << ", entries: " << std::dec << entries;
  for (int i = 0; i < entries; i++) {
    LOG(INFO) << "  Key[" << keys[i] << "] -> 0x" << std::hex << targets[i];
  }
}

void DumpPackedSwitchTable(const uint16_t* table)
  /*
   * Packed switch data format:
   *  ushort ident = 0x0100   magic value
   *  ushort size       number of entries in the table
   *  int first_key       first (and lowest) switch case value
   *  int targets[size]     branch targets, relative to switch opcode
   *
   * Total size is (4+size*2) 16-bit code units.
   */
{
  uint16_t ident = table[0];
  const int* targets = reinterpret_cast<const int*>(&table[4]);
  int entries = table[1];
  int low_key = s4FromSwitchData(&table[2]);
  LOG(INFO) << "Packed switch table - ident:0x" << std::hex << ident
            << ", entries: " << std::dec << entries << ", low_key: " << low_key;
  for (int i = 0; i < entries; i++) {
    LOG(INFO) << "  Key[" << (i + low_key) << "] -> 0x" << std::hex
              << targets[i];
  }
}

/*
 * Set up special LIR to mark a Dalvik byte-code instruction start and
 * record it in the boundary_map.  NOTE: in cases such as kMirOpCheck in
 * which we split a single Dalvik instruction, only the first MIR op
 * associated with a Dalvik PC should be entered into the map.
 */
LIR* MarkBoundary(CompilationUnit* cu, int offset, const char* inst_str)
{
  LIR* res = NewLIR1(cu, kPseudoDalvikByteCodeBoundary, reinterpret_cast<uintptr_t>(inst_str));
  if (cu->boundary_map.find(offset) == cu->boundary_map.end()) {
    cu->boundary_map.Put(offset, res);
  }
  return res;
}

bool EvaluateBranch(Instruction::Code opcode, int32_t src1, int32_t src2)
{
  bool is_taken;
  switch (opcode) {
    case Instruction::IF_EQ: is_taken = (src1 == src2); break;
    case Instruction::IF_NE: is_taken = (src1 != src2); break;
    case Instruction::IF_LT: is_taken = (src1 < src2); break;
    case Instruction::IF_GE: is_taken = (src1 >= src2); break;
    case Instruction::IF_GT: is_taken = (src1 > src2); break;
    case Instruction::IF_LE: is_taken = (src1 <= src2); break;
    case Instruction::IF_EQZ: is_taken = (src1 == 0); break;
    case Instruction::IF_NEZ: is_taken = (src1 != 0); break;
    case Instruction::IF_LTZ: is_taken = (src1 < 0); break;
    case Instruction::IF_GEZ: is_taken = (src1 >= 0); break;
    case Instruction::IF_GTZ: is_taken = (src1 > 0); break;
    case Instruction::IF_LEZ: is_taken = (src1 <= 0); break;
    default:
      LOG(FATAL) << "Unexpected opcode " << opcode;
      is_taken = false;
  }
  return is_taken;
}

// Convert relation of src1/src2 to src2/src1
ConditionCode FlipComparisonOrder(ConditionCode before) {
  ConditionCode res;
  switch (before) {
    case kCondEq: res = kCondEq; break;
    case kCondNe: res = kCondNe; break;
    case kCondLt: res = kCondGt; break;
    case kCondGt: res = kCondLt; break;
    case kCondLe: res = kCondGe; break;
    case kCondGe: res = kCondLe; break;
    default:
      res = static_cast<ConditionCode>(0);
      LOG(FATAL) << "Unexpected ccode " << before;
  }
  return res;
}

} // namespace art
