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

#include "gc_map.h"
#include "verifier/dex_gc_map.h"
#include "verifier/method_verifier.h"

namespace art {

void setMemRefType(LIR* lir, bool isLoad, int memType)
{
  u8 *maskPtr;
  u8 mask = ENCODE_MEM;;
  DCHECK(EncodingMap[lir->opcode].flags & (IS_LOAD | IS_STORE));
  if (isLoad) {
    maskPtr = &lir->useMask;
  } else {
    maskPtr = &lir->defMask;
  }
  /* Clear out the memref flags */
  *maskPtr &= ~mask;
  /* ..and then add back the one we need */
  switch (memType) {
    case kLiteral:
      DCHECK(isLoad);
      *maskPtr |= ENCODE_LITERAL;
      break;
    case kDalvikReg:
      *maskPtr |= ENCODE_DALVIK_REG;
      break;
    case kHeapRef:
      *maskPtr |= ENCODE_HEAP_REF;
      break;
    case kMustNotAlias:
      /* Currently only loads can be marked as kMustNotAlias */
      DCHECK(!(EncodingMap[lir->opcode].flags & IS_STORE));
      *maskPtr |= ENCODE_MUST_NOT_ALIAS;
      break;
    default:
      LOG(FATAL) << "Oat: invalid memref kind - " << memType;
  }
}

/*
 * Mark load/store instructions that access Dalvik registers through the stack.
 */
void annotateDalvikRegAccess(LIR* lir, int regId, bool isLoad, bool is64bit)
{
  setMemRefType(lir, isLoad, kDalvikReg);

  /*
   * Store the Dalvik register id in aliasInfo. Mark the MSB if it is a 64-bit
   * access.
   */
  lir->aliasInfo = ENCODE_ALIAS_INFO(regId, is64bit);
}

u8 oatGetRegMaskCommon(CompilationUnit* cUnit, int reg)
{
  return getRegMaskCommon(cUnit, reg);
}

/*
 * Mark the corresponding bit(s).
 */
inline void setupRegMask(CompilationUnit* cUnit, u8* mask, int reg)
{
  *mask |= getRegMaskCommon(cUnit, reg);
}

/* Exported version of setupRegMask */
void oatSetupRegMask(CompilationUnit* cUnit, u8* mask, int reg)
{
  setupRegMask(cUnit, mask, reg);
}

/*
 * Set up the proper fields in the resource mask
 */
void setupResourceMasks(CompilationUnit* cUnit, LIR* lir)
{
  int opcode = lir->opcode;

  if (opcode <= 0) {
    lir->useMask = lir->defMask = 0;
    return;
  }

  uint64_t flags = EncodingMap[opcode].flags;

  if (flags & NEEDS_FIXUP) {
    lir->flags.pcRelFixup = true;
  }

  /* Get the starting size of the instruction's template */
  lir->flags.size = oatGetInsnSize(lir);

  /* Set up the mask for resources that are updated */
  if (flags & (IS_LOAD | IS_STORE)) {
    /* Default to heap - will catch specialized classes later */
    setMemRefType(lir, flags & IS_LOAD, kHeapRef);
  }

  /*
   * Conservatively assume the branch here will call out a function that in
   * turn will trash everything.
   */
  if (flags & IS_BRANCH) {
    lir->defMask = lir->useMask = ENCODE_ALL;
    return;
  }

  if (flags & REG_DEF0) {
    setupRegMask(cUnit, &lir->defMask, lir->operands[0]);
  }

  if (flags & REG_DEF1) {
    setupRegMask(cUnit, &lir->defMask, lir->operands[1]);
  }


  if (flags & SETS_CCODES) {
    lir->defMask |= ENCODE_CCODE;
  }

  if (flags & (REG_USE0 | REG_USE1 | REG_USE2 | REG_USE3)) {
    int i;

    for (i = 0; i < 4; i++) {
      if (flags & (1 << (kRegUse0 + i))) {
        setupRegMask(cUnit, &lir->useMask, lir->operands[i]);
      }
    }
  }

  if (flags & USES_CCODES) {
    lir->useMask |= ENCODE_CCODE;
  }

  // Handle target-specific actions
  setupTargetResourceMasks(cUnit, lir);
}

/*
 * Debugging macros
 */
#define DUMP_RESOURCE_MASK(X)
#define DUMP_SSA_REP(X)

/* Pretty-print a LIR instruction */
void oatDumpLIRInsn(CompilationUnit* cUnit, LIR* arg, unsigned char* baseAddr)
{
  LIR* lir = (LIR*) arg;
  int offset = lir->offset;
  int dest = lir->operands[0];
  const bool dumpNop = (cUnit->enableDebug & (1 << kDebugShowNops));

  /* Handle pseudo-ops individually, and all regular insns as a group */
  switch (lir->opcode) {
    case kPseudoMethodEntry:
      LOG(INFO) << "-------- method entry "
                << PrettyMethod(cUnit->method_idx, *cUnit->dex_file);
      break;
    case kPseudoMethodExit:
      LOG(INFO) << "-------- Method_Exit";
      break;
    case kPseudoBarrier:
      LOG(INFO) << "-------- BARRIER";
      break;
    case kPseudoExtended:
      LOG(INFO) << "-------- " << (char* ) dest;
      break;
    case kPseudoSSARep:
      DUMP_SSA_REP(LOG(INFO) << "-------- kMirOpPhi: " <<  (char* ) dest);
      break;
    case kPseudoEntryBlock:
      LOG(INFO) << "-------- entry offset: 0x" << std::hex << dest;
      break;
    case kPseudoDalvikByteCodeBoundary:
      LOG(INFO) << "-------- dalvik offset: 0x" << std::hex
                << lir->dalvikOffset << " @ " << (char* )lir->operands[0];
      break;
    case kPseudoExitBlock:
      LOG(INFO) << "-------- exit offset: 0x" << std::hex << dest;
      break;
    case kPseudoPseudoAlign4:
      LOG(INFO) << (intptr_t)baseAddr + offset << " (0x" << std::hex
                << offset << "): .align4";
      break;
    case kPseudoEHBlockLabel:
      LOG(INFO) << "Exception_Handling:";
      break;
    case kPseudoTargetLabel:
    case kPseudoNormalBlockLabel:
      LOG(INFO) << "L" << (void*)lir << ":";
      break;
    case kPseudoThrowTarget:
      LOG(INFO) << "LT" << (void*)lir << ":";
      break;
    case kPseudoIntrinsicRetry:
      LOG(INFO) << "IR" << (void*)lir << ":";
      break;
    case kPseudoSuspendTarget:
      LOG(INFO) << "LS" << (void*)lir << ":";
      break;
    case kPseudoSafepointPC:
      LOG(INFO) << "LsafepointPC_0x" << std::hex << lir->offset << "_" << lir->dalvikOffset << ":";
      break;
    case kPseudoExportedPC:
      LOG(INFO) << "LexportedPC_0x" << std::hex << lir->offset << "_" << lir->dalvikOffset << ":";
      break;
    case kPseudoCaseLabel:
      LOG(INFO) << "LC" << (void*)lir << ": Case target 0x"
                << std::hex << lir->operands[0] << "|" << std::dec <<
        lir->operands[0];
      break;
    default:
      if (lir->flags.isNop && !dumpNop) {
        break;
      } else {
        std::string op_name(buildInsnString(EncodingMap[lir->opcode].name,
                                            lir, baseAddr));
        std::string op_operands(buildInsnString(EncodingMap[lir->opcode].fmt
                                              , lir, baseAddr));
        LOG(INFO) << StringPrintf("%05x: %-9s%s%s",
                                  (unsigned int)(baseAddr + offset),
                                  op_name.c_str(), op_operands.c_str(),
                                  lir->flags.isNop ? "(nop)" : "");
      }
      break;
  }

  if (lir->useMask && (!lir->flags.isNop || dumpNop)) {
    DUMP_RESOURCE_MASK(oatDumpResourceMask((LIR* ) lir, lir->useMask, "use"));
  }
  if (lir->defMask && (!lir->flags.isNop || dumpNop)) {
    DUMP_RESOURCE_MASK(oatDumpResourceMask((LIR* ) lir, lir->defMask, "def"));
  }
}

void oatDumpPromotionMap(CompilationUnit *cUnit)
{
  int numRegs = cUnit->numDalvikRegisters + cUnit->numCompilerTemps + 1;
  for (int i = 0; i < numRegs; i++) {
    PromotionMap vRegMap = cUnit->promotionMap[i];
    std::string buf;
    if (vRegMap.fpLocation == kLocPhysReg) {
      StringAppendF(&buf, " : s%d", vRegMap.fpReg & FP_REG_MASK);
    }

    std::string buf3;
    if (i < cUnit->numDalvikRegisters) {
      StringAppendF(&buf3, "%02d", i);
    } else if (i == cUnit->methodSReg) {
      buf3 = "Method*";
    } else {
      StringAppendF(&buf3, "ct%d", i - cUnit->numDalvikRegisters);
    }

    LOG(INFO) << StringPrintf("V[%s] -> %s%d%s", buf3.c_str(),
                              vRegMap.coreLocation == kLocPhysReg ?
                              "r" : "SP+", vRegMap.coreLocation == kLocPhysReg ?
                              vRegMap.coreReg : oatSRegOffset(cUnit, i),
                              buf.c_str());
  }
}

/* Dump a mapping table */
void dumpMappingTable(const char* table_name, const std::string& descriptor,
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
void oatCodegenDump(CompilationUnit* cUnit)
{
  LOG(INFO) << "Dumping LIR insns for "
            << PrettyMethod(cUnit->method_idx, *cUnit->dex_file);
  LIR* lirInsn;
  LIR* thisLIR;
  int insnsSize = cUnit->insnsSize;

  LOG(INFO) << "Regs (excluding ins) : " << cUnit->numRegs;
  LOG(INFO) << "Ins          : " << cUnit->numIns;
  LOG(INFO) << "Outs         : " << cUnit->numOuts;
  LOG(INFO) << "CoreSpills       : " << cUnit->numCoreSpills;
  LOG(INFO) << "FPSpills       : " << cUnit->numFPSpills;
  LOG(INFO) << "CompilerTemps    : " << cUnit->numCompilerTemps;
  LOG(INFO) << "Frame size       : " << cUnit->frameSize;
  LOG(INFO) << "code size is " << cUnit->totalSize <<
    " bytes, Dalvik size is " << insnsSize * 2;
  LOG(INFO) << "expansion factor: "
            << (float)cUnit->totalSize / (float)(insnsSize * 2);
  oatDumpPromotionMap(cUnit);
  for (lirInsn = cUnit->firstLIRInsn; lirInsn; lirInsn = lirInsn->next) {
    oatDumpLIRInsn(cUnit, lirInsn, 0);
  }
  for (lirInsn = cUnit->literalList; lirInsn; lirInsn = lirInsn->next) {
    thisLIR = (LIR*) lirInsn;
    LOG(INFO) << StringPrintf("%x (%04x): .word (%#x)",
                              thisLIR->offset, thisLIR->offset,
                              thisLIR->operands[0]);
  }

  const DexFile::MethodId& method_id =
      cUnit->dex_file->GetMethodId(cUnit->method_idx);
  std::string signature(cUnit->dex_file->GetMethodSignature(method_id));
  std::string name(cUnit->dex_file->GetMethodName(method_id));
  std::string descriptor(cUnit->dex_file->GetMethodDeclaringClassDescriptor(method_id));

  // Dump mapping tables
  dumpMappingTable("PC2Dex_MappingTable", descriptor, name, signature, cUnit->pc2dexMappingTable);
  dumpMappingTable("Dex2PC_MappingTable", descriptor, name, signature, cUnit->dex2pcMappingTable);
}


LIR* rawLIR(CompilationUnit* cUnit, int dalvikOffset, int opcode, int op0,
      int op1, int op2, int op3, int op4, LIR* target)
{
  LIR* insn = (LIR* ) oatNew(cUnit, sizeof(LIR), true, kAllocLIR);
  insn->dalvikOffset = dalvikOffset;
  insn->opcode = opcode;
  insn->operands[0] = op0;
  insn->operands[1] = op1;
  insn->operands[2] = op2;
  insn->operands[3] = op3;
  insn->operands[4] = op4;
  insn->target = target;
  oatSetupResourceMasks(cUnit, insn);
  if ((opcode == kPseudoTargetLabel) || (opcode == kPseudoSafepointPC) ||
      (opcode == kPseudoExportedPC)) {
    // Always make labels scheduling barriers
    insn->useMask = insn->defMask = ENCODE_ALL;
  }
  return insn;
}

/*
 * The following are building blocks to construct low-level IRs with 0 - 4
 * operands.
 */
LIR* newLIR0(CompilationUnit* cUnit, int opcode)
{
  DCHECK(isPseudoOpcode(opcode) || (EncodingMap[opcode].flags & NO_OPERAND))
      << EncodingMap[opcode].name << " " << (int)opcode << " "
      << PrettyMethod(cUnit->method_idx, *cUnit->dex_file) << " "
      << cUnit->currentDalvikOffset;
  LIR* insn = rawLIR(cUnit, cUnit->currentDalvikOffset, opcode);
  oatAppendLIR(cUnit, (LIR*) insn);
  return insn;
}

LIR* newLIR1(CompilationUnit* cUnit, int opcode,
               int dest)
{
  DCHECK(isPseudoOpcode(opcode) || (EncodingMap[opcode].flags & IS_UNARY_OP))
      << EncodingMap[opcode].name << " " << (int)opcode << " "
      << PrettyMethod(cUnit->method_idx, *cUnit->dex_file) << " "
      << cUnit->currentDalvikOffset;
  LIR* insn = rawLIR(cUnit, cUnit->currentDalvikOffset, opcode, dest);
  oatAppendLIR(cUnit, (LIR*) insn);
  return insn;
}

LIR* newLIR2(CompilationUnit* cUnit, int opcode,
               int dest, int src1)
{
  DCHECK(isPseudoOpcode(opcode) || (EncodingMap[opcode].flags & IS_BINARY_OP))
      << EncodingMap[opcode].name << " " << (int)opcode << " "
      << PrettyMethod(cUnit->method_idx, *cUnit->dex_file) << " "
      << cUnit->currentDalvikOffset;
  LIR* insn = rawLIR(cUnit, cUnit->currentDalvikOffset, opcode, dest, src1);
  oatAppendLIR(cUnit, (LIR*) insn);
  return insn;
}

LIR* newLIR3(CompilationUnit* cUnit, int opcode,
               int dest, int src1, int src2)
{
  DCHECK(isPseudoOpcode(opcode) || (EncodingMap[opcode].flags & IS_TERTIARY_OP))
      << EncodingMap[opcode].name << " " << (int)opcode << " "
      << PrettyMethod(cUnit->method_idx, *cUnit->dex_file) << " "
      << cUnit->currentDalvikOffset;
  LIR* insn = rawLIR(cUnit, cUnit->currentDalvikOffset, opcode, dest, src1,
                     src2);
  oatAppendLIR(cUnit, (LIR*) insn);
  return insn;
}

LIR* newLIR4(CompilationUnit* cUnit, int opcode,
      int dest, int src1, int src2, int info)
{
  DCHECK(isPseudoOpcode(opcode) || (EncodingMap[opcode].flags & IS_QUAD_OP))
      << EncodingMap[opcode].name << " " << (int)opcode << " "
      << PrettyMethod(cUnit->method_idx, *cUnit->dex_file) << " "
      << cUnit->currentDalvikOffset;
  LIR* insn = rawLIR(cUnit, cUnit->currentDalvikOffset, opcode, dest, src1,
                     src2, info);
  oatAppendLIR(cUnit, (LIR*) insn);
  return insn;
}

LIR* newLIR5(CompilationUnit* cUnit, int opcode,
       int dest, int src1, int src2, int info1, int info2)
{
  DCHECK(isPseudoOpcode(opcode) || (EncodingMap[opcode].flags & IS_QUIN_OP))
      << EncodingMap[opcode].name << " " << (int)opcode << " "
      << PrettyMethod(cUnit->method_idx, *cUnit->dex_file) << " "
      << cUnit->currentDalvikOffset;
  LIR* insn = rawLIR(cUnit, cUnit->currentDalvikOffset, opcode, dest, src1,
                     src2, info1, info2);
  oatAppendLIR(cUnit, (LIR*) insn);
  return insn;
}

/*
 * Search the existing constants in the literal pool for an exact or close match
 * within specified delta (greater or equal to 0).
 */
LIR* scanLiteralPool(LIR* dataTarget, int value, unsigned int delta)
{
  while (dataTarget) {
    if (((unsigned) (value - ((LIR* ) dataTarget)->operands[0])) <= delta)
      return (LIR* ) dataTarget;
    dataTarget = dataTarget->next;
  }
  return NULL;
}

/* Search the existing constants in the literal pool for an exact wide match */
LIR* scanLiteralPoolWide(LIR* dataTarget, int valLo, int valHi)
{
  bool loMatch = false;
  LIR* loTarget = NULL;
  while (dataTarget) {
    if (loMatch && (((LIR*)dataTarget)->operands[0] == valHi)) {
      return (LIR*)loTarget;
    }
    loMatch = false;
    if (((LIR*)dataTarget)->operands[0] == valLo) {
      loMatch = true;
      loTarget = dataTarget;
    }
    dataTarget = dataTarget->next;
  }
  return NULL;
}

/*
 * The following are building blocks to insert constants into the pool or
 * instruction streams.
 */

/* Add a 32-bit constant either in the constant pool */
LIR* addWordData(CompilationUnit* cUnit, LIR* *constantListP, int value)
{
  /* Add the constant to the literal pool */
  if (constantListP) {
    LIR* newValue = (LIR* ) oatNew(cUnit, sizeof(LIR), true, kAllocData);
    newValue->operands[0] = value;
    newValue->next = *constantListP;
    *constantListP = (LIR*) newValue;
    return newValue;
  }
  return NULL;
}

/* Add a 64-bit constant to the constant pool or mixed with code */
LIR* addWideData(CompilationUnit* cUnit, LIR* *constantListP,
               int valLo, int valHi)
{
  //FIXME: hard-coded little endian, need BE variant
  // Insert high word into list first
  addWordData(cUnit, constantListP, valHi);
  return addWordData(cUnit, constantListP, valLo);
}

void pushWord(std::vector<uint8_t>&buf, int data) {
  buf.push_back( data & 0xff);
  buf.push_back( (data >> 8) & 0xff);
  buf.push_back( (data >> 16) & 0xff);
  buf.push_back( (data >> 24) & 0xff);
}

void alignBuffer(std::vector<uint8_t>&buf, size_t offset) {
  while (buf.size() < offset) {
    buf.push_back(0);
  }
}

bool IsDirect(int invokeType) {
  InvokeType type = static_cast<InvokeType>(invokeType);
  return type == kStatic || type == kDirect;
}

/* Write the literal pool to the output stream */
void installLiteralPools(CompilationUnit* cUnit)
{
  alignBuffer(cUnit->codeBuffer, cUnit->dataOffset);
  LIR* dataLIR = cUnit->literalList;
  while (dataLIR != NULL) {
    pushWord(cUnit->codeBuffer, dataLIR->operands[0]);
    dataLIR = NEXT_LIR(dataLIR);
  }
  // Push code and method literals, record offsets for the compiler to patch.
  dataLIR = cUnit->codeLiteralList;
  while (dataLIR != NULL) {
    uint32_t target = dataLIR->operands[0];
    cUnit->compiler->AddCodePatch(cUnit->dex_file,
                                  cUnit->method_idx,
                                  cUnit->invoke_type,
                                  target,
                                  static_cast<InvokeType>(dataLIR->operands[1]),
                                  cUnit->codeBuffer.size());
    const DexFile::MethodId& id = cUnit->dex_file->GetMethodId(target);
    // unique based on target to ensure code deduplication works
    uint32_t unique_patch_value = reinterpret_cast<uint32_t>(&id);
    pushWord(cUnit->codeBuffer, unique_patch_value);
    dataLIR = NEXT_LIR(dataLIR);
  }
  dataLIR = cUnit->methodLiteralList;
  while (dataLIR != NULL) {
    uint32_t target = dataLIR->operands[0];
    cUnit->compiler->AddMethodPatch(cUnit->dex_file,
                                    cUnit->method_idx,
                                    cUnit->invoke_type,
                                    target,
                                    static_cast<InvokeType>(dataLIR->operands[1]),
                                    cUnit->codeBuffer.size());
    const DexFile::MethodId& id = cUnit->dex_file->GetMethodId(target);
    // unique based on target to ensure code deduplication works
    uint32_t unique_patch_value = reinterpret_cast<uint32_t>(&id);
    pushWord(cUnit->codeBuffer, unique_patch_value);
    dataLIR = NEXT_LIR(dataLIR);
  }
}

/* Write the switch tables to the output stream */
void installSwitchTables(CompilationUnit* cUnit)
{
  GrowableListIterator iterator;
  oatGrowableListIteratorInit(&cUnit->switchTables, &iterator);
  while (true) {
    SwitchTable* tabRec = (SwitchTable *) oatGrowableListIteratorNext(
       &iterator);
    if (tabRec == NULL) break;
    alignBuffer(cUnit->codeBuffer, tabRec->offset);
    /*
     * For Arm, our reference point is the address of the bx
     * instruction that does the launch, so we have to subtract
     * the auto pc-advance.  For other targets the reference point
     * is a label, so we can use the offset as-is.
     */
    int bxOffset = INVALID_OFFSET;
    switch (cUnit->instructionSet) {
      case kThumb2:
        bxOffset = tabRec->anchor->offset + 4;
        break;
      case kX86:
        bxOffset = 0;
        break;
      case kMips:
        bxOffset = tabRec->anchor->offset;
        break;
      default: LOG(FATAL) << "Unexpected instruction set: " << cUnit->instructionSet;
    }
    if (cUnit->printMe) {
      LOG(INFO) << "Switch table for offset 0x" << std::hex << bxOffset;
    }
    if (tabRec->table[0] == Instruction::kSparseSwitchSignature) {
      int* keys = (int*)&(tabRec->table[2]);
      for (int elems = 0; elems < tabRec->table[1]; elems++) {
        int disp = tabRec->targets[elems]->offset - bxOffset;
        if (cUnit->printMe) {
          LOG(INFO) << "  Case[" << elems << "] key: 0x"
                    << std::hex << keys[elems] << ", disp: 0x"
                    << std::hex << disp;
        }
        pushWord(cUnit->codeBuffer, keys[elems]);
        pushWord(cUnit->codeBuffer,
          tabRec->targets[elems]->offset - bxOffset);
      }
    } else {
      DCHECK_EQ(static_cast<int>(tabRec->table[0]),
                static_cast<int>(Instruction::kPackedSwitchSignature));
      for (int elems = 0; elems < tabRec->table[1]; elems++) {
        int disp = tabRec->targets[elems]->offset - bxOffset;
        if (cUnit->printMe) {
          LOG(INFO) << "  Case[" << elems << "] disp: 0x"
                    << std::hex << disp;
        }
        pushWord(cUnit->codeBuffer, tabRec->targets[elems]->offset - bxOffset);
      }
    }
  }
}

/* Write the fill array dta to the output stream */
void installFillArrayData(CompilationUnit* cUnit)
{
  GrowableListIterator iterator;
  oatGrowableListIteratorInit(&cUnit->fillArrayData, &iterator);
  while (true) {
    FillArrayData *tabRec = (FillArrayData *) oatGrowableListIteratorNext(
       &iterator);
    if (tabRec == NULL) break;
    alignBuffer(cUnit->codeBuffer, tabRec->offset);
    for (int i = 0; i < (tabRec->size + 1) / 2; i++) {
      cUnit->codeBuffer.push_back( tabRec->table[i] & 0xFF);
      cUnit->codeBuffer.push_back( (tabRec->table[i] >> 8) & 0xFF);
    }
  }
}

int assignLiteralOffsetCommon(LIR* lir, int offset)
{
  for (;lir != NULL; lir = lir->next) {
    lir->offset = offset;
    offset += 4;
  }
  return offset;
}

// Make sure we have a code address for every declared catch entry
bool verifyCatchEntries(CompilationUnit* cUnit)
{
  bool success = true;
  for (std::set<uint32_t>::const_iterator it = cUnit->catches.begin(); it != cUnit->catches.end(); ++it) {
    uint32_t dexPc = *it;
    bool found = false;
    for (size_t i = 0; i < cUnit->dex2pcMappingTable.size(); i += 2) {
      if (dexPc == cUnit->dex2pcMappingTable[i+1]) {
        found = true;
        break;
      }
    }
    if (!found) {
      LOG(INFO) << "Missing native PC for catch entry @ 0x" << std::hex << dexPc;
      success = false;
    }
  }
  // Now, try in the other direction
  for (size_t i = 0; i < cUnit->dex2pcMappingTable.size(); i += 2) {
    uint32_t dexPc = cUnit->dex2pcMappingTable[i+1];
    if (cUnit->catches.find(dexPc) == cUnit->catches.end()) {
      LOG(INFO) << "Unexpected catch entry @ dex pc 0x" << std::hex << dexPc;
      success = false;
    }
  }
  if (!success) {
    LOG(INFO) << "Bad dex2pcMapping table in " << PrettyMethod(cUnit->method_idx, *cUnit->dex_file);
    LOG(INFO) << "Entries @ decode: " << cUnit->catches.size() << ", Entries in table: "
              << cUnit->dex2pcMappingTable.size()/2;
  }
  return success;
}

void createMappingTables(CompilationUnit* cUnit)
{
  for (LIR* tgtLIR = (LIR *) cUnit->firstLIRInsn; tgtLIR != NULL; tgtLIR = NEXT_LIR(tgtLIR)) {
    if (!tgtLIR->flags.isNop && (tgtLIR->opcode == kPseudoSafepointPC)) {
      cUnit->pc2dexMappingTable.push_back(tgtLIR->offset);
      cUnit->pc2dexMappingTable.push_back(tgtLIR->dalvikOffset);
    }
    if (!tgtLIR->flags.isNop && (tgtLIR->opcode == kPseudoExportedPC)) {
      cUnit->dex2pcMappingTable.push_back(tgtLIR->offset);
      cUnit->dex2pcMappingTable.push_back(tgtLIR->dalvikOffset);
    }
  }
  DCHECK(verifyCatchEntries(cUnit));
  cUnit->combinedMappingTable.push_back(cUnit->pc2dexMappingTable.size() +
                                        cUnit->dex2pcMappingTable.size());
  cUnit->combinedMappingTable.push_back(cUnit->pc2dexMappingTable.size());
  cUnit->combinedMappingTable.insert(cUnit->combinedMappingTable.end(),
                                     cUnit->pc2dexMappingTable.begin(),
                                     cUnit->pc2dexMappingTable.end());
  cUnit->combinedMappingTable.insert(cUnit->combinedMappingTable.end(),
                                     cUnit->dex2pcMappingTable.begin(),
                                     cUnit->dex2pcMappingTable.end());
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

static void createNativeGcMap(CompilationUnit* cUnit) {
  const std::vector<uint32_t>& mapping_table = cUnit->pc2dexMappingTable;
  uint32_t max_native_offset = 0;
  for (size_t i = 0; i < mapping_table.size(); i += 2) {
    uint32_t native_offset = mapping_table[i + 0];
    if (native_offset > max_native_offset) {
      max_native_offset = native_offset;
    }
  }
  Compiler::MethodReference method_ref(cUnit->dex_file, cUnit->method_idx);
  const std::vector<uint8_t>* gc_map_raw = verifier::MethodVerifier::GetDexGcMap(method_ref);
  verifier::DexPcToReferenceMap dex_gc_map(&(*gc_map_raw)[4], gc_map_raw->size() - 4);
  // Compute native offset to references size.
  NativePcToReferenceMapBuilder native_gc_map_builder(&cUnit->nativeGcMap,
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
int assignLiteralOffset(CompilationUnit* cUnit, int offset)
{
  offset = assignLiteralOffsetCommon(cUnit->literalList, offset);
  offset = assignLiteralOffsetCommon(cUnit->codeLiteralList, offset);
  offset = assignLiteralOffsetCommon(cUnit->methodLiteralList, offset);
  return offset;
}

int assignSwitchTablesOffset(CompilationUnit* cUnit, int offset)
{
  GrowableListIterator iterator;
  oatGrowableListIteratorInit(&cUnit->switchTables, &iterator);
  while (true) {
    SwitchTable *tabRec = (SwitchTable *) oatGrowableListIteratorNext(
       &iterator);
    if (tabRec == NULL) break;
    tabRec->offset = offset;
    if (tabRec->table[0] == Instruction::kSparseSwitchSignature) {
      offset += tabRec->table[1] * (sizeof(int) * 2);
    } else {
      DCHECK_EQ(static_cast<int>(tabRec->table[0]),
                static_cast<int>(Instruction::kPackedSwitchSignature));
      offset += tabRec->table[1] * sizeof(int);
    }
  }
  return offset;
}

int assignFillArrayDataOffset(CompilationUnit* cUnit, int offset)
{
  GrowableListIterator iterator;
  oatGrowableListIteratorInit(&cUnit->fillArrayData, &iterator);
  while (true) {
    FillArrayData *tabRec = (FillArrayData *) oatGrowableListIteratorNext(
       &iterator);
    if (tabRec == NULL) break;
    tabRec->offset = offset;
    offset += tabRec->size;
    // word align
    offset = (offset + 3) & ~3;
    }
  return offset;
}

/*
 * Walk the compilation unit and assign offsets to instructions
 * and literals and compute the total size of the compiled unit.
 */
void oatAssignOffsets(CompilationUnit* cUnit)
{
  int offset = oatAssignInsnOffsets(cUnit);

  /* Const values have to be word aligned */
  offset = (offset + 3) & ~3;

  /* Set up offsets for literals */
  cUnit->dataOffset = offset;

  offset = assignLiteralOffset(cUnit, offset);

  offset = assignSwitchTablesOffset(cUnit, offset);

  offset = assignFillArrayDataOffset(cUnit, offset);

  cUnit->totalSize = offset;
}

/*
 * Go over each instruction in the list and calculate the offset from the top
 * before sending them off to the assembler. If out-of-range branch distance is
 * seen rearrange the instructions a bit to correct it.
 */
void oatAssembleLIR(CompilationUnit* cUnit)
{
  oatAssignOffsets(cUnit);
  /*
   * Assemble here.  Note that we generate code with optimistic assumptions
   * and if found now to work, we'll have to redo the sequence and retry.
   */

  while (true) {
    AssemblerStatus res = oatAssembleInstructions(cUnit, 0);
    if (res == kSuccess) {
      break;
    } else {
      cUnit->assemblerRetries++;
      if (cUnit->assemblerRetries > MAX_ASSEMBLER_RETRIES) {
        oatCodegenDump(cUnit);
        LOG(FATAL) << "Assembler error - too many retries";
      }
      // Redo offsets and try again
      oatAssignOffsets(cUnit);
      cUnit->codeBuffer.clear();
    }
  }

  // Install literals
  installLiteralPools(cUnit);

  // Install switch tables
  installSwitchTables(cUnit);

  // Install fill array data
  installFillArrayData(cUnit);

  // Create the mapping table and native offset to reference map.
  createMappingTables(cUnit);

  createNativeGcMap(cUnit);
}

/*
 * Insert a kPseudoCaseLabel at the beginning of the Dalvik
 * offset vaddr.  This label will be used to fix up the case
 * branch table during the assembly phase.  Be sure to set
 * all resource flags on this to prevent code motion across
 * target boundaries.  KeyVal is just there for debugging.
 */
LIR* insertCaseLabel(CompilationUnit* cUnit, int vaddr, int keyVal)
{
  SafeMap<unsigned int, LIR*>::iterator it;
  it = cUnit->boundaryMap.find(vaddr);
  if (it == cUnit->boundaryMap.end()) {
    LOG(FATAL) << "Error: didn't find vaddr 0x" << std::hex << vaddr;
  }
  LIR* newLabel = (LIR*)oatNew(cUnit, sizeof(LIR), true, kAllocLIR);
  newLabel->dalvikOffset = vaddr;
  newLabel->opcode = kPseudoCaseLabel;
  newLabel->operands[0] = keyVal;
  oatInsertLIRAfter(it->second, (LIR*)newLabel);
  return newLabel;
}

void markPackedCaseLabels(CompilationUnit* cUnit, SwitchTable *tabRec)
{
  const u2* table = tabRec->table;
  int baseVaddr = tabRec->vaddr;
  int *targets = (int*)&table[4];
  int entries = table[1];
  int lowKey = s4FromSwitchData(&table[2]);
  for (int i = 0; i < entries; i++) {
    tabRec->targets[i] = insertCaseLabel(cUnit, baseVaddr + targets[i],
                                         i + lowKey);
  }
}

void markSparseCaseLabels(CompilationUnit* cUnit, SwitchTable *tabRec)
{
  const u2* table = tabRec->table;
  int baseVaddr = tabRec->vaddr;
  int entries = table[1];
  int* keys = (int*)&table[2];
  int* targets = &keys[entries];
  for (int i = 0; i < entries; i++) {
    tabRec->targets[i] = insertCaseLabel(cUnit, baseVaddr + targets[i],
                                         keys[i]);
  }
}

void oatProcessSwitchTables(CompilationUnit* cUnit)
{
  GrowableListIterator iterator;
  oatGrowableListIteratorInit(&cUnit->switchTables, &iterator);
  while (true) {
    SwitchTable *tabRec =
        (SwitchTable *) oatGrowableListIteratorNext(&iterator);
    if (tabRec == NULL) break;
    if (tabRec->table[0] == Instruction::kPackedSwitchSignature) {
      markPackedCaseLabels(cUnit, tabRec);
    } else if (tabRec->table[0] == Instruction::kSparseSwitchSignature) {
      markSparseCaseLabels(cUnit, tabRec);
    } else {
      LOG(FATAL) << "Invalid switch table";
    }
  }
}

//FIXME: Do we have endian issues here?

void dumpSparseSwitchTable(const u2* table)
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
  u2 ident = table[0];
  int entries = table[1];
  int* keys = (int*)&table[2];
  int* targets = &keys[entries];
  LOG(INFO) <<  "Sparse switch table - ident:0x" << std::hex << ident
            << ", entries: " << std::dec << entries;
  for (int i = 0; i < entries; i++) {
    LOG(INFO) << "  Key[" << keys[i] << "] -> 0x" << std::hex << targets[i];
  }
}

void dumpPackedSwitchTable(const u2* table)
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
  u2 ident = table[0];
  int* targets = (int*)&table[4];
  int entries = table[1];
  int lowKey = s4FromSwitchData(&table[2]);
  LOG(INFO) << "Packed switch table - ident:0x" << std::hex << ident
            << ", entries: " << std::dec << entries << ", lowKey: " << lowKey;
  for (int i = 0; i < entries; i++) {
    LOG(INFO) << "  Key[" << (i + lowKey) << "] -> 0x" << std::hex
              << targets[i];
  }
}

/*
 * Set up special LIR to mark a Dalvik byte-code instruction start and
 * record it in the boundaryMap.  NOTE: in cases such as kMirOpCheck in
 * which we split a single Dalvik instruction, only the first MIR op
 * associated with a Dalvik PC should be entered into the map.
 */
LIR* markBoundary(CompilationUnit* cUnit, int offset, const char* instStr)
{
  LIR* res = newLIR1(cUnit, kPseudoDalvikByteCodeBoundary, (intptr_t) instStr);
  if (cUnit->boundaryMap.find(offset) == cUnit->boundaryMap.end()) {
    cUnit->boundaryMap.Put(offset, res);
  }
  return res;
}

}
 // namespace art
