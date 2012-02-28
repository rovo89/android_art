/*
 * Copyright (C) 2012 The Android Open Source Project
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

/*
 * This file contains codegen for the Mips ISA and is intended to be
 * includes by:
 *
 *        Codegen-$(TARGET_ARCH_VARIANT).c
 *
 */

namespace art {

// FIXME: need the following:
void genSuspendTest(CompilationUnit* cUnit, MIR* mir) {}
void genMonitorEnter(CompilationUnit* cUnit, MIR* mir, RegLocation rlSrc) {}
void genMonitorExit(CompilationUnit* cUnit, MIR* mir, RegLocation rlSrc) {}
void genCheckCast(CompilationUnit* cUnit, MIR* mir, RegLocation rlSrc) {}
void genInstanceof(CompilationUnit* cUnit, MIR* mir, RegLocation rlDest,
                   RegLocation rlSrc) {}
void genNewInstance(CompilationUnit* cUnit, MIR* mir,
                           RegLocation rlDest) {}
void genThrow(CompilationUnit* cUnit, MIR* mir, RegLocation rlSrc) {}
void genConstString(CompilationUnit* cUnit, MIR* mir,
                           RegLocation rlDest, RegLocation rlSrc) {}
void genConstClass(CompilationUnit* cUnit, MIR* mir,
                          RegLocation rlDest, RegLocation rlSrc) {}
void genArrayGet(CompilationUnit* cUnit, MIR* mir, OpSize size,
                        RegLocation rlArray, RegLocation rlIndex,
                        RegLocation rlDest, int scale) {}
void genArrayPut(CompilationUnit* cUnit, MIR* mir, OpSize size,
                        RegLocation rlArray, RegLocation rlIndex,
                        RegLocation rlSrc, int scale) {}
void genArrayObjPut(CompilationUnit* cUnit, MIR* mir,
                           RegLocation rlArray, RegLocation rlIndex,
                           RegLocation rlSrc, int scale) {}
void genIPut(CompilationUnit* cUnit, MIR* mir, OpSize size,
                    RegLocation rlSrc, RegLocation rlObj,
                    bool isLongOrDouble, bool isObject) {}
bool genArithOpInt(CompilationUnit* cUnit, MIR* mir,
                          RegLocation rlDest, RegLocation rlSrc1,
                          RegLocation rlSrc2) { return 0; }
bool genArithOpLong(CompilationUnit* cUnit, MIR* mir,
                           RegLocation rlDest, RegLocation rlSrc1,
                           RegLocation rlSrc2) { return 0; }
bool genShiftOpLong(CompilationUnit* cUnit, MIR* mir,
                           RegLocation rlDest, RegLocation rlSrc1,
                           RegLocation rlShift) { return 0; }
bool genArithOpIntLit(CompilationUnit* cUnit, MIR* mir,
                             RegLocation rlDest, RegLocation rlSrc,
                             int lit) { return 0; }
void oatArchDump(void) {};
void genDebuggerUpdate(CompilationUnit* cUnit, int32_t offset) {};




STATIC bool genConversionCall(CompilationUnit* cUnit, MIR* mir, int funcOffset,
                                     int srcSize, int tgtSize)
{
    UNIMPLEMENTED(FATAL) << "Need Mips implementation";
    return 0;
#if 0
    /*
     * Don't optimize the register usage since it calls out to support
     * functions
     */
    RegLocation rlSrc;
    RegLocation rlDest;
    oatFlushAllRegs(cUnit);   /* Send everything to home location */
    loadWordDisp(cUnit, rSELF, funcOffset, rLR);
    if (srcSize == 1) {
        rlSrc = oatGetSrc(cUnit, mir, 0);
        loadValueDirectFixed(cUnit, rlSrc, r0);
    } else {
        rlSrc = oatGetSrcWide(cUnit, mir, 0, 1);
        loadValueDirectWideFixed(cUnit, rlSrc, r0, r1);
    }
    callRuntimeHelper(cUnit, rLR);
    if (tgtSize == 1) {
        RegLocation rlResult;
        rlDest = oatGetDest(cUnit, mir, 0);
        rlResult = oatGetReturn(cUnit);
        storeValue(cUnit, rlDest, rlResult);
    } else {
        RegLocation rlResult;
        rlDest = oatGetDestWide(cUnit, mir, 0, 1);
        rlResult = oatGetReturnWide(cUnit);
        storeValueWide(cUnit, rlDest, rlResult);
    }
    return false;
#endif
}

bool genArithOpFloatPortable(CompilationUnit* cUnit, MIR* mir,
                                    RegLocation rlDest, RegLocation rlSrc1,
                                    RegLocation rlSrc2)
{
    UNIMPLEMENTED(FATAL) << "Need Mips implementation";
    return 0;
#if 0
    RegLocation rlResult;
    int funcOffset;

    switch (mir->dalvikInsn.opcode) {
        case OP_ADD_FLOAT_2ADDR:
        case OP_ADD_FLOAT:
            funcOffset = OFFSETOF_MEMBER(Thread, pFadd);
            break;
        case OP_SUB_FLOAT_2ADDR:
        case OP_SUB_FLOAT:
            funcOffset = OFFSETOF_MEMBER(Thread, pFsub);
            break;
        case OP_DIV_FLOAT_2ADDR:
        case OP_DIV_FLOAT:
            funcOffset = OFFSETOF_MEMBER(Thread, pFdiv);
            break;
        case OP_MUL_FLOAT_2ADDR:
        case OP_MUL_FLOAT:
            funcOffset = OFFSETOF_MEMBER(Thread, pFmul);
            break;
        case OP_REM_FLOAT_2ADDR:
        case OP_REM_FLOAT:
            funcOffset = OFFSETOF_MEMBER(Thread, pFmodf);
            break;
        case OP_NEG_FLOAT: {
            genNegFloat(cUnit, rlDest, rlSrc1);
            return false;
        }
        default:
            return true;
    }
    oatFlushAllRegs(cUnit);   /* Send everything to home location */
    loadWordDisp(cUnit, rSELF, funcOffset, rLR);
    loadValueDirectFixed(cUnit, rlSrc1, r0);
    loadValueDirectFixed(cUnit, rlSrc2, r1);
    callRuntimeHelper(cUnit, rLR);
    rlResult = oatGetReturn(cUnit);
    storeValue(cUnit, rlDest, rlResult);
    return false;
#endif
}

bool genArithOpDoublePortable(CompilationUnit* cUnit, MIR* mir,
                                     RegLocation rlDest, RegLocation rlSrc1,
                                     RegLocation rlSrc2)
{
    UNIMPLEMENTED(FATAL) << "Need Mips implementation";
    return 0;
#if 0
    RegLocation rlResult;
    int funcOffset;

    switch (mir->dalvikInsn.opcode) {
        case OP_ADD_DOUBLE_2ADDR:
        case OP_ADD_DOUBLE:
            funcOffset = OFFSETOF_MEMBER(Thread, pDadd);
            break;
        case OP_SUB_DOUBLE_2ADDR:
        case OP_SUB_DOUBLE:
            funcOffset = OFFSETOF_MEMBER(Thread, pDsub);
            break;
        case OP_DIV_DOUBLE_2ADDR:
        case OP_DIV_DOUBLE:
            funcOffset = OFFSETOF_MEMBER(Thread, pDdiv);
            break;
        case OP_MUL_DOUBLE_2ADDR:
        case OP_MUL_DOUBLE:
            funcOffset = OFFSETOF_MEMBER(Thread, pDmul);
            break;
        case OP_REM_DOUBLE_2ADDR:
        case OP_REM_DOUBLE:
            funcOffset = OFFSETOF_MEMBER(Thread, pFmod);
            break;
        case OP_NEG_DOUBLE: {
            genNegDouble(cUnit, rlDest, rlSrc1);
            return false;
        }
        default:
            return true;
    }
    oatFlushAllRegs(cUnit);   /* Send everything to home location */
    loadWordDisp(cUnit, rSELF, funcOffset, rLR);
    loadValueDirectWideFixed(cUnit, rlSrc1, r0, r1);
    loadValueDirectWideFixed(cUnit, rlSrc2, r2, r3);
    callRuntimeHelper(cUnit, rLR);
    rlResult = oatGetReturnWide(cUnit);
    storeValueWide(cUnit, rlDest, rlResult);
    return false;
#endif
}

bool genConversionPortable(CompilationUnit* cUnit, MIR* mir)
{
    UNIMPLEMENTED(FATAL) << "Need Mips implementation";
    return 0;
#if 0
    Opcode opcode = mir->dalvikInsn.opcode;

    switch (opcode) {
        case OP_INT_TO_FLOAT:
            return genConversionCall(cUnit, mir, OFFSETOF_MEMBER(Thread, pI2f),
                                     1, 1);
        case OP_FLOAT_TO_INT:
            return genConversionCall(cUnit, mir, OFFSETOF_MEMBER(Thread, pF2iz),
                                     1, 1);
        case OP_DOUBLE_TO_FLOAT:
            return genConversionCall(cUnit, mir, OFFSETOF_MEMBER(Thread, pD2f),
                                     2, 1);
        case OP_FLOAT_TO_DOUBLE:
            return genConversionCall(cUnit, mir, OFFSETOF_MEMBER(Thread, pF2d),
                                     1, 2);
        case OP_INT_TO_DOUBLE:
            return genConversionCall(cUnit, mir, OFFSETOF_MEMBER(Thread, pI2d),
                                     1, 2);
        case OP_DOUBLE_TO_INT:
            return genConversionCall(cUnit, mir, OFFSETOF_MEMBER(Thread, pD2iz),
                                     2, 1);
        case OP_FLOAT_TO_LONG:
            return genConversionCall(cUnit, mir, OFFSETOF_MEMBER(Thread,
                                     pF2l), 1, 2);
        case OP_LONG_TO_FLOAT:
            return genConversionCall(cUnit, mir, OFFSETOF_MEMBER(Thread, pL2f),
                                     2, 1);
        case OP_DOUBLE_TO_LONG:
            return genConversionCall(cUnit, mir, OFFSETOF_MEMBER(Thread,
                                     pD2l), 2, 2);
        case OP_LONG_TO_DOUBLE:
            return genConversionCall(cUnit, mir, OFFSETOF_MEMBER(Thread, pL2d),
                                     2, 2);
        default:
            return true;
    }
    return false;
#endif
}





STATIC RegLocation getRetLoc(CompilationUnit* cUnit);

void warnIfUnresolved(CompilationUnit* cUnit, int fieldIdx, Field* field) {
  if (field == NULL) {
    const DexFile::FieldId& field_id = cUnit->dex_file->GetFieldId(fieldIdx);
    std::string class_name(cUnit->dex_file->GetFieldDeclaringClassDescriptor(field_id));
    std::string field_name(cUnit->dex_file->GetFieldName(field_id));
    LOG(INFO) << "Field " << PrettyDescriptor(class_name) << "." << field_name
              << " unresolved at compile time";
  } else {
    // We also use the slow path for wide volatile fields.
  }
}

/*
 * Construct an s4 from two consecutive half-words of switch data.
 * This needs to check endianness because the DEX optimizer only swaps
 * half-words in instruction stream.
 *
 * "switchData" must be 32-bit aligned.
 */
#if __BYTE_ORDER == __LITTLE_ENDIAN
STATIC inline s4 s4FromSwitchData(const void* switchData) {
    return *(s4*) switchData;
}
#else
STATIC inline s4 s4FromSwitchData(const void* switchData) {
    u2* data = switchData;
    return data[0] | (((s4) data[1]) << 16);
}
#endif
/*
 * Insert a kPseudoCaseLabel at the beginning of the Dalvik
 * offset vaddr.  This label will be used to fix up the case
 * branch table during the assembly phase.  Be sure to set
 * all resource flags on this to prevent code motion across
 * target boundaries.  KeyVal is just there for debugging.
 */
STATIC MipsLIR* insertCaseLabel(CompilationUnit* cUnit, int vaddr, int keyVal)
{
    std::map<unsigned int, LIR*>::iterator it;
    it = cUnit->boundaryMap.find(vaddr);
    if (it == cUnit->boundaryMap.end()) {
        LOG(FATAL) << "Error: didn't find vaddr 0x" << std::hex << vaddr;
    }
    MipsLIR* newLabel = (MipsLIR*)oatNew(cUnit, sizeof(MipsLIR), true, kAllocLIR);
    newLabel->generic.dalvikOffset = vaddr;
    newLabel->opcode = kPseudoCaseLabel;
    newLabel->operands[0] = keyVal;
    oatInsertLIRAfter(it->second, (LIR*)newLabel);
    return newLabel;
}

STATIC void markPackedCaseLabels(CompilationUnit* cUnit, SwitchTable *tabRec)
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

STATIC void markSparseCaseLabels(CompilationUnit* cUnit, SwitchTable *tabRec)
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
        SwitchTable *tabRec = (SwitchTable *) oatGrowableListIteratorNext(
             &iterator);
        if (tabRec == NULL) break;
        if (tabRec->table[0] == kPackedSwitchSignature)
            markPackedCaseLabels(cUnit, tabRec);
        else if (tabRec->table[0] == kSparseSwitchSignature)
            markSparseCaseLabels(cUnit, tabRec);
        else {
            LOG(FATAL) << "Invalid switch table";
        }
    }
}

STATIC void dumpSparseSwitchTable(const u2* table)
    /*
     * Sparse switch data format:
     *  ushort ident = 0x0200   magic value
     *  ushort size             number of entries in the table; > 0
     *  int keys[size]          keys, sorted low-to-high; 32-bit aligned
     *  int targets[size]       branch targets, relative to switch opcode
     *
     * Total size is (2+size*4) 16-bit code units.
     */
{
    u2 ident = table[0];
    int entries = table[1];
    int* keys = (int*)&table[2];
    int* targets = &keys[entries];
    LOG(INFO) <<  "Sparse switch table - ident:0x" << std::hex << ident <<
       ", entries: " << std::dec << entries;
    for (int i = 0; i < entries; i++) {
        LOG(INFO) << "    Key[" << keys[i] << "] -> 0x" << std::hex <<
        targets[i];
    }
}

STATIC void dumpPackedSwitchTable(const u2* table)
    /*
     * Packed switch data format:
     *  ushort ident = 0x0100   magic value
     *  ushort size             number of entries in the table
     *  int first_key           first (and lowest) switch case value
     *  int targets[size]       branch targets, relative to switch opcode
     *
     * Total size is (4+size*2) 16-bit code units.
     */
{
    u2 ident = table[0];
    int* targets = (int*)&table[4];
    int entries = table[1];
    int lowKey = s4FromSwitchData(&table[2]);
    LOG(INFO) << "Packed switch table - ident:0x" << std::hex << ident <<
        ", entries: " << std::dec << entries << ", lowKey: " << lowKey;
    for (int i = 0; i < entries; i++) {
        LOG(INFO) << "    Key[" << (i + lowKey) << "] -> 0x" << std::hex <<
            targets[i];
    }
}

/*
 * The sparse table in the literal pool is an array of <key,displacement>
 * pairs.  For each set, we'll load them as a pair using ldmia.
 * This means that the register number of the temp we use for the key
 * must be lower than the reg for the displacement.
 *
 * The test loop will look something like:
 *
 *   adr   rBase, <table>
 *   ldr   rVal, [rSP, vRegOff]
 *   mov   rIdx, #tableSize
 * lp:
 *   ldmia rBase!, {rKey, rDisp}
 *   sub   rIdx, #1
 *   cmp   rVal, rKey
 *   ifeq
 *   add   rPC, rDisp   ; This is the branch from which we compute displacement
 *   cbnz  rIdx, lp
 */
STATIC void genSparseSwitch(CompilationUnit* cUnit, MIR* mir,
                            RegLocation rlSrc)
{
    UNIMPLEMENTED(FATAL) << "Need Mips implementation";
#if 0
    const u2* table = cUnit->insns + mir->offset + mir->dalvikInsn.vB;
    if (cUnit->printMe) {
        dumpSparseSwitchTable(table);
    }
    // Add the table to the list - we'll process it later
    SwitchTable *tabRec = (SwitchTable *)oatNew(cUnit, sizeof(SwitchTable),
                         true, kAllocData);
    tabRec->table = table;
    tabRec->vaddr = mir->offset;
    int size = table[1];
    tabRec->targets = (MipsLIR* *)oatNew(cUnit, size * sizeof(MipsLIR*), true,
                                        kAllocLIR);
    oatInsertGrowableList(cUnit, &cUnit->switchTables, (intptr_t)tabRec);

    // Get the switch value
    rlSrc = loadValue(cUnit, rlSrc, kCoreReg);
    int rBase = oatAllocTemp(cUnit);
    /* Allocate key and disp temps */
    int rKey = oatAllocTemp(cUnit);
    int rDisp = oatAllocTemp(cUnit);
    // Make sure rKey's register number is less than rDisp's number for ldmia
    if (rKey > rDisp) {
        int tmp = rDisp;
        rDisp = rKey;
        rKey = tmp;
    }
    // Materialize a pointer to the switch table
    newLIR3(cUnit, kThumb2Adr, rBase, 0, (intptr_t)tabRec);
    // Set up rIdx
    int rIdx = oatAllocTemp(cUnit);
    loadConstant(cUnit, rIdx, size);
    // Establish loop branch target
    MipsLIR* target = newLIR0(cUnit, kPseudoTargetLabel);
    target->defMask = ENCODE_ALL;
    // Load next key/disp
    newLIR2(cUnit, kThumb2LdmiaWB, rBase, (1 << rKey) | (1 << rDisp));
    opRegReg(cUnit, kOpCmp, rKey, rlSrc.lowReg);
    // Go if match. NOTE: No instruction set switch here - must stay Thumb2
    genIT(cUnit, kMipsCondEq, "");
    MipsLIR* switchBranch = newLIR1(cUnit, kThumb2AddPCR, rDisp);
    tabRec->bxInst = switchBranch;
    // Needs to use setflags encoding here
    newLIR3(cUnit, kThumb2SubsRRI12, rIdx, rIdx, 1);
    MipsLIR* branch = opCondBranch(cUnit, kMipsCondNe);
    branch->generic.target = (LIR*)target;
#endif
}

STATIC void genPackedSwitch(CompilationUnit* cUnit, MIR* mir,
                            RegLocation rlSrc)
{
    UNIMPLEMENTED(FATAL) << "Need Mips implementation";
#if 0
    const u2* table = cUnit->insns + mir->offset + mir->dalvikInsn.vB;
    if (cUnit->printMe) {
        dumpPackedSwitchTable(table);
    }
    // Add the table to the list - we'll process it later
    SwitchTable *tabRec = (SwitchTable *)oatNew(cUnit, sizeof(SwitchTable),
                                                true, kAllocData);
    tabRec->table = table;
    tabRec->vaddr = mir->offset;
    int size = table[1];
    tabRec->targets = (MipsLIR* *)oatNew(cUnit, size * sizeof(MipsLIR*), true,
                                        kAllocLIR);
    oatInsertGrowableList(cUnit, &cUnit->switchTables, (intptr_t)tabRec);

    // Get the switch value
    rlSrc = loadValue(cUnit, rlSrc, kCoreReg);
    int tableBase = oatAllocTemp(cUnit);
    // Materialize a pointer to the switch table
    newLIR3(cUnit, kThumb2Adr, tableBase, 0, (intptr_t)tabRec);
    int lowKey = s4FromSwitchData(&table[2]);
    int keyReg;
    // Remove the bias, if necessary
    if (lowKey == 0) {
        keyReg = rlSrc.lowReg;
    } else {
        keyReg = oatAllocTemp(cUnit);
        opRegRegImm(cUnit, kOpSub, keyReg, rlSrc.lowReg, lowKey);
    }
    // Bounds check - if < 0 or >= size continue following switch
    opRegImm(cUnit, kOpCmp, keyReg, size-1);
    MipsLIR* branchOver = opCondBranch(cUnit, kMipsCondHi);

    // Load the displacement from the switch table
    int dispReg = oatAllocTemp(cUnit);
    loadBaseIndexed(cUnit, tableBase, keyReg, dispReg, 2, kWord);

    // ..and go! NOTE: No instruction set switch here - must stay Thumb2
    MipsLIR* switchBranch = newLIR1(cUnit, kThumb2AddPCR, dispReg);
    tabRec->bxInst = switchBranch;

    /* branchOver target here */
    MipsLIR* target = newLIR0(cUnit, kPseudoTargetLabel);
    target->defMask = ENCODE_ALL;
    branchOver->generic.target = (LIR*)target;
#endif
}

/*
 * Array data table format:
 *  ushort ident = 0x0300   magic value
 *  ushort width            width of each element in the table
 *  uint   size             number of elements in the table
 *  ubyte  data[size*width] table of data values (may contain a single-byte
 *                          padding at the end)
 *
 * Total size is 4+(width * size + 1)/2 16-bit code units.
 */
STATIC void genFillArrayData(CompilationUnit* cUnit, MIR* mir,
                              RegLocation rlSrc)
{
    UNIMPLEMENTED(FATAL) << "Need Mips implementation";
#if 0
    const u2* table = cUnit->insns + mir->offset + mir->dalvikInsn.vB;
    // Add the table to the list - we'll process it later
    FillArrayData *tabRec = (FillArrayData *)
         oatNew(cUnit, sizeof(FillArrayData), true, kAllocData);
    tabRec->table = table;
    tabRec->vaddr = mir->offset;
    u2 width = tabRec->table[1];
    u4 size = tabRec->table[2] | (((u4)tabRec->table[3]) << 16);
    tabRec->size = (size * width) + 8;

    oatInsertGrowableList(cUnit, &cUnit->fillArrayData, (intptr_t)tabRec);

    // Making a call - use explicit registers
    oatFlushAllRegs(cUnit);   /* Everything to home location */
    loadValueDirectFixed(cUnit, rlSrc, r0);
    loadWordDisp(cUnit, rSELF,
                 OFFSETOF_MEMBER(Thread, pHandleFillArrayDataFromCode), rLR);
    // Materialize a pointer to the fill data image
    newLIR3(cUnit, kThumb2Adr, r1, 0, (intptr_t)tabRec);
    callRuntimeHelper(cUnit, rLR);
#endif
}

STATIC void genIGet(CompilationUnit* cUnit, MIR* mir, OpSize size,
                    RegLocation rlDest, RegLocation rlObj,
                    bool isLongOrDouble, bool isObject)
{
    UNIMPLEMENTED(FATAL) << "Need Mips implementation";
#if 0
    int fieldOffset;
    bool isVolatile;
    uint32_t fieldIdx = mir->dalvikInsn.vC;
    bool fastPath =
        cUnit->compiler->ComputeInstanceFieldInfo(fieldIdx, cUnit,
                                                  fieldOffset, isVolatile, false);
    if (fastPath && !SLOW_FIELD_PATH) {
        RegLocation rlResult;
        RegisterClass regClass = oatRegClassBySize(size);
        DCHECK_GE(fieldOffset, 0);
        rlObj = loadValue(cUnit, rlObj, kCoreReg);
        if (isLongOrDouble) {
            DCHECK(rlDest.wide);
            genNullCheck(cUnit, rlObj.sRegLow, rlObj.lowReg, mir);/* null obj? */
            int regPtr = oatAllocTemp(cUnit);
            opRegRegImm(cUnit, kOpAdd, regPtr, rlObj.lowReg, fieldOffset);
            rlResult = oatEvalLoc(cUnit, rlDest, regClass, true);
            loadPair(cUnit, regPtr, rlResult.lowReg, rlResult.highReg);
            if (isVolatile) {
                oatGenMemBarrier(cUnit, kSY);
            }
            oatFreeTemp(cUnit, regPtr);
            storeValueWide(cUnit, rlDest, rlResult);
        } else {
            rlResult = oatEvalLoc(cUnit, rlDest, regClass, true);
            genNullCheck(cUnit, rlObj.sRegLow, rlObj.lowReg, mir);/* null object? */
            loadBaseDisp(cUnit, mir, rlObj.lowReg, fieldOffset, rlResult.lowReg,
                         kWord, rlObj.sRegLow);
            if (isVolatile) {
                oatGenMemBarrier(cUnit, kSY);
            }
            storeValue(cUnit, rlDest, rlResult);
        }
    } else {
        int getterOffset = isLongOrDouble ? OFFSETOF_MEMBER(Thread, pGet64Instance) :
                           (isObject ? OFFSETOF_MEMBER(Thread, pGetObjInstance)
                                     : OFFSETOF_MEMBER(Thread, pGet32Instance));
        loadWordDisp(cUnit, rSELF, getterOffset, rLR);
        loadValueDirect(cUnit, rlObj, r1);
        loadConstant(cUnit, r0, fieldIdx);
        callRuntimeHelper(cUnit, rLR);
        if (isLongOrDouble) {
            RegLocation rlResult = oatGetReturnWide(cUnit);
            storeValueWide(cUnit, rlDest, rlResult);
        } else {
            RegLocation rlResult = oatGetReturn(cUnit);
            storeValue(cUnit, rlDest, rlResult);
        }
    }
#endif
}

/*
 * Perform a "reg cmp imm" operation and jump to the PCR region if condition
 * satisfies.
 */
STATIC void genNegFloat(CompilationUnit *cUnit, RegLocation rlDest,
                        RegLocation rlSrc)
{
    RegLocation rlResult;
    rlSrc = loadValue(cUnit, rlSrc, kCoreReg);
    rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
    opRegRegImm(cUnit, kOpAdd, rlResult.lowReg,
                rlSrc.lowReg, 0x80000000);
    storeValue(cUnit, rlDest, rlResult);
}

STATIC void genNegDouble(CompilationUnit *cUnit, RegLocation rlDest,
                         RegLocation rlSrc)
{
    RegLocation rlResult;
    rlSrc = loadValueWide(cUnit, rlSrc, kCoreReg);
    rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
    opRegRegImm(cUnit, kOpAdd, rlResult.highReg, rlSrc.highReg,
                        0x80000000);
    genRegCopy(cUnit, rlResult.lowReg, rlSrc.lowReg);
    storeValueWide(cUnit, rlDest, rlResult);
}

STATIC void genMulLong(CompilationUnit *cUnit, RegLocation rlDest,
                       RegLocation rlSrc1, RegLocation rlSrc2)
{
    UNIMPLEMENTED(FATAL) << "Need Mips implementation";
#if 0
    RegLocation rlResult;
    loadValueDirectWideFixed(cUnit, rlSrc1, r_ARG0, r_ARG1);
    loadValueDirectWideFixed(cUnit, rlSrc2, r_ARG2, r_ARG3);
    genDispatchToHandler(cUnit, TEMPLATE_MUL_LONG);
    rlResult = oatGetReturnWide(cUnit);
    storeValueWide(cUnit, rlDest, rlResult);
#endif
}

STATIC bool partialOverlap(int sreg1, int sreg2)
{
    return abs(sreg1 - sreg2) == 1;
}

STATIC void withCarryHelper(CompilationUnit *cUnit, MipsOpCode opc,
                            RegLocation rlDest, RegLocation rlSrc1,
                            RegLocation rlSrc2, int sltuSrc1, int sltuSrc2)
{
    int tReg = oatAllocTemp(cUnit);
    newLIR3(cUnit, opc, rlDest.lowReg, rlSrc1.lowReg, rlSrc2.lowReg);
    newLIR3(cUnit, kMipsSltu, tReg, sltuSrc1, sltuSrc2);
    newLIR3(cUnit, opc, rlDest.highReg, rlSrc1.highReg, rlSrc2.highReg);
    newLIR3(cUnit, opc, rlDest.highReg, rlDest.highReg, tReg);
    oatFreeTemp(cUnit, tReg);
}

STATIC void genLong3Addr(CompilationUnit *cUnit, MIR *mir, OpKind firstOp,
                         OpKind secondOp, RegLocation rlDest,
                         RegLocation rlSrc1, RegLocation rlSrc2)
{
    UNIMPLEMENTED(FATAL) << "Need Mips implementation";
#if 0
    RegLocation rlResult;
    int carryOp = (secondOp == kOpAdc || secondOp == kOpSbc);

    if (partialOverlap(rlSrc1.sRegLow,rlSrc2.sRegLow) ||
        partialOverlap(rlSrc1.sRegLow,rlDest.sRegLow) ||
        partialOverlap(rlSrc2.sRegLow,rlDest.sRegLow)) {
        // Rare case - not enough registers to properly handle
        genInterpSingleStep(cUnit, mir);
    } else if (rlDest.sRegLow == rlSrc1.sRegLow) {
        rlResult = loadValueWide(cUnit, rlDest, kCoreReg);
        rlSrc2 = loadValueWide(cUnit, rlSrc2, kCoreReg);
        if (!carryOp) {
            opRegRegReg(cUnit, firstOp, rlResult.lowReg, rlResult.lowReg, rlSrc2.lowReg);
            opRegRegReg(cUnit, secondOp, rlResult.highReg, rlResult.highReg, rlSrc2.highReg);
        } else if (secondOp == kOpAdc) {
            withCarryHelper(cUnit, kMipsAddu, rlResult, rlResult, rlSrc2,
                            rlResult.lowReg, rlSrc2.lowReg);
        } else {
            int tReg = oatAllocTemp(cUnit);
            newLIR2(cUnit, kMipsMove, tReg, rlResult.lowReg);
            withCarryHelper(cUnit, kMipsSubu, rlResult, rlResult, rlSrc2,
                            tReg, rlResult.lowReg);
            oatFreeTemp(cUnit, tReg);
        }
        storeValueWide(cUnit, rlDest, rlResult);
    } else if (rlDest.sRegLow == rlSrc2.sRegLow) {
        rlResult = loadValueWide(cUnit, rlDest, kCoreReg);
        rlSrc1 = loadValueWide(cUnit, rlSrc1, kCoreReg);
        if (!carryOp) {
            opRegRegReg(cUnit, firstOp, rlResult.lowReg, rlSrc1.lowReg, rlResult.lowReg);
            opRegRegReg(cUnit, secondOp, rlResult.highReg, rlSrc1.highReg, rlResult.highReg);
        } else if (secondOp == kOpAdc) {
            withCarryHelper(cUnit, kMipsAddu, rlResult, rlSrc1, rlResult,
                            rlResult.lowReg, rlSrc1.lowReg);
        } else {
            withCarryHelper(cUnit, kMipsSubu, rlResult, rlSrc1, rlResult,
                            rlSrc1.lowReg, rlResult.lowReg);
        }
        storeValueWide(cUnit, rlDest, rlResult);
    } else {
        rlSrc1 = loadValueWide(cUnit, rlSrc1, kCoreReg);
        rlSrc2 = loadValueWide(cUnit, rlSrc2, kCoreReg);
        rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
        if (!carryOp) {
            opRegRegReg(cUnit, firstOp, rlResult.lowReg, rlSrc1.lowReg, rlSrc2.lowReg);
            opRegRegReg(cUnit, secondOp, rlResult.highReg, rlSrc1.highReg, rlSrc2.highReg);
        } else if (secondOp == kOpAdc) {
            withCarryHelper(cUnit, kMipsAddu, rlResult, rlSrc1, rlSrc2,
                            rlResult.lowReg, rlSrc1.lowReg);
        } else {
            withCarryHelper(cUnit, kMipsSubu, rlResult, rlSrc1, rlSrc2,
                            rlSrc1.lowReg, rlResult.lowReg);
        }
        storeValueWide(cUnit, rlDest, rlResult);
    }
#endif
}

void oatInitializeRegAlloc(CompilationUnit* cUnit)
{
    int numRegs = sizeof(coreRegs)/sizeof(*coreRegs);
    int numReserved = sizeof(reservedRegs)/sizeof(*reservedRegs);
    int numTemps = sizeof(coreTemps)/sizeof(*coreTemps);
#ifdef __mips_hard_float
    int numFPRegs = sizeof(fpRegs)/sizeof(*fpRegs);
    int numFPTemps = sizeof(fpTemps)/sizeof(*fpTemps);
#else
    int numFPRegs = 0;
    int numFPTemps = 0;
#endif
    RegisterPool *pool = (RegisterPool *)oatNew(cUnit, sizeof(*pool), true,
                                                kAllocRegAlloc);
    cUnit->regPool = pool;
    pool->numCoreRegs = numRegs;
    pool->coreRegs = (RegisterInfo *)
            oatNew(cUnit, numRegs * sizeof(*cUnit->regPool->coreRegs),
                   true, kAllocRegAlloc);
    pool->numFPRegs = numFPRegs;
    pool->FPRegs = numFPRegs == 0 ? NULL : (RegisterInfo *)
            oatNew(cUnit, numFPRegs * sizeof(*cUnit->regPool->FPRegs), true,
                   kAllocRegAlloc);
    oatInitPool(pool->coreRegs, coreRegs, pool->numCoreRegs);
    oatInitPool(pool->FPRegs, fpRegs, pool->numFPRegs);
    // Keep special registers from being allocated
    for (int i = 0; i < numReserved; i++) {
        if (NO_SUSPEND && !cUnit->genDebugger &&
            (reservedRegs[i] == rSUSPEND)) {
            //To measure cost of suspend check
            continue;
        }
        oatMarkInUse(cUnit, reservedRegs[i]);
    }
    // Mark temp regs - all others not in use can be used for promotion
    for (int i = 0; i < numTemps; i++) {
        oatMarkTemp(cUnit, coreTemps[i]);
    }
    for (int i = 0; i < numFPTemps; i++) {
        oatMarkTemp(cUnit, fpTemps[i]);
    }
    // Construct the alias map.
    cUnit->phiAliasMap = (int*)oatNew(cUnit, cUnit->numSSARegs *
                                      sizeof(cUnit->phiAliasMap[0]), false,
                                      kAllocDFInfo);
    for (int i = 0; i < cUnit->numSSARegs; i++) {
        cUnit->phiAliasMap[i] = i;
    }
    for (MIR* phi = cUnit->phiList; phi; phi = phi->meta.phiNext) {
        int defReg = phi->ssaRep->defs[0];
        for (int i = 0; i < phi->ssaRep->numUses; i++) {
           for (int j = 0; j < cUnit->numSSARegs; j++) {
               if (cUnit->phiAliasMap[j] == phi->ssaRep->uses[i]) {
                   cUnit->phiAliasMap[j] = defReg;
               }
           }
        }
    }
}

STATIC void genMonitor(CompilationUnit *cUnit, MIR *mir)
{
    UNIMPLEMENTED(FATAL) << "Need Mips implementation";
#if 0
    genMonitorPortable(cUnit, mir);
#endif
}

STATIC void genCmpLong(CompilationUnit *cUnit, MIR *mir, RegLocation rlDest,
                       RegLocation rlSrc1, RegLocation rlSrc2)
{
    UNIMPLEMENTED(FATAL) << "Need Mips implementation";
#if 0
    RegLocation rlResult;
    loadValueDirectWideFixed(cUnit, rlSrc1, r_ARG0, r_ARG1);
    loadValueDirectWideFixed(cUnit, rlSrc2, r_ARG2, r_ARG3);
    genDispatchToHandler(cUnit, TEMPLATE_CMP_LONG);
    rlResult = oatGetReturn(cUnit);
    storeValue(cUnit, rlDest, rlResult);
#endif
}

STATIC void genMultiplyByTwoBitMultiplier(CompilationUnit *cUnit,
        RegLocation rlSrc, RegLocation rlResult, int lit,
        int firstBit, int secondBit)
{
    // We can't implement "add src, src, src, lsl#shift" on Thumb, so we have
    // to do a regular multiply.
    opRegRegImm(cUnit, kOpMul, rlResult.lowReg, rlSrc.lowReg, lit);
}

}  // namespace art
