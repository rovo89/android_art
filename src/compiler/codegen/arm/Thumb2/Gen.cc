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

/*
 * This file contains codegen for the Thumb2 ISA and is intended to be
 * includes by:
 *
 *        Codegen-$(TARGET_ARCH_VARIANT).c
 *
 */

#define SLOW_FIELD_PATH (cUnit->enableDebug & (1 << kDebugSlowFieldPath))
#define SLOW_INVOKE_PATH (cUnit->enableDebug & (1 << kDebugSlowInvokePath))
#define SLOW_STRING_PATH (cUnit->enableDebug & (1 << kDebugSlowStringPath))
#define SLOW_TYPE_PATH (cUnit->enableDebug & (1 << kDebugSlowTypePath))
#define EXERCISE_SLOWEST_FIELD_PATH (cUnit->enableDebug & \
    (1 << kDebugSlowestFieldPath))
#define EXERCISE_SLOWEST_STRING_PATH (cUnit->enableDebug & \
    (1 << kDebugSlowestStringPath))

STATIC RegLocation getRetLoc(CompilationUnit* cUnit);

std::string fieldNameFromIndex(const Method* method, uint32_t fieldIdx)
{
    art::ClassLinker* class_linker = art::Runtime::Current()->GetClassLinker();
    const art::DexFile& dex_file = class_linker->FindDexFile(
         method->GetDeclaringClass()->GetDexCache());
    const art::DexFile::FieldId& field_id = dex_file.GetFieldId(fieldIdx);
    std::string class_name = dex_file.dexStringByTypeIdx(field_id.class_idx_);
    std::string field_name = dex_file.dexStringById(field_id.name_idx_);
    return class_name + "." + field_name;
}

void warnIfUnresolved(CompilationUnit* cUnit, int fieldIdx, Field* field) {
  if (field == NULL) {
    LOG(INFO) << "Field " << fieldNameFromIndex(cUnit->method, fieldIdx)
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

STATIC ArmLIR* callRuntimeHelper(CompilationUnit* cUnit, int reg)
{
    oatClobberCalleeSave(cUnit);
    return opReg(cUnit, kOpBlx, reg);
}

/* Generate unconditional branch instructions */
STATIC ArmLIR* genUnconditionalBranch(CompilationUnit* cUnit, ArmLIR* target)
{
    ArmLIR* branch = opNone(cUnit, kOpUncondBr);
    branch->generic.target = (LIR*) target;
    return branch;
}

/*
 * Generate a Thumb2 IT instruction, which can nullify up to
 * four subsequent instructions based on a condition and its
 * inverse.  The condition applies to the first instruction, which
 * is executed if the condition is met.  The string "guide" consists
 * of 0 to 3 chars, and applies to the 2nd through 4th instruction.
 * A "T" means the instruction is executed if the condition is
 * met, and an "E" means the instruction is executed if the condition
 * is not met.
 */
STATIC ArmLIR* genIT(CompilationUnit* cUnit, ArmConditionCode code,
                     const char* guide)
{
    int mask;
    int condBit = code & 1;
    int altBit = condBit ^ 1;
    int mask3 = 0;
    int mask2 = 0;
    int mask1 = 0;

    //Note: case fallthroughs intentional
    switch(strlen(guide)) {
        case 3:
            mask1 = (guide[2] == 'T') ? condBit : altBit;
        case 2:
            mask2 = (guide[1] == 'T') ? condBit : altBit;
        case 1:
            mask3 = (guide[0] == 'T') ? condBit : altBit;
            break;
        case 0:
            break;
        default:
            LOG(FATAL) << "OAT: bad case in genIT";
    }
    mask = (mask3 << 3) | (mask2 << 2) | (mask1 << 1) |
           (1 << (3 - strlen(guide)));
    return newLIR2(cUnit, kThumb2It, code, mask);
}

/*
 * Insert a kArmPseudoCaseLabel at the beginning of the Dalvik
 * offset vaddr.  This label will be used to fix up the case
 * branch table during the assembly phase.  Be sure to set
 * all resource flags on this to prevent code motion across
 * target boundaries.  KeyVal is just there for debugging.
 */
STATIC ArmLIR* insertCaseLabel(CompilationUnit* cUnit, int vaddr, int keyVal)
{
    ArmLIR* lir;
    for (lir = (ArmLIR*)cUnit->firstLIRInsn; lir; lir = NEXT_LIR(lir)) {
        if ((lir->opcode == kArmPseudoDalvikByteCodeBoundary) &&
            (lir->generic.dalvikOffset == vaddr)) {
            ArmLIR* newLabel = (ArmLIR*)oatNew(sizeof(ArmLIR), true);
            newLabel->generic.dalvikOffset = vaddr;
            newLabel->opcode = kArmPseudoCaseLabel;
            newLabel->operands[0] = keyVal;
            oatInsertLIRAfter((LIR*)lir, (LIR*)newLabel);
            return newLabel;
        }
    }
    oatCodegenDump(cUnit);
    LOG(FATAL) << "Error: didn't find vaddr 0x" << std::hex << vaddr;
    return NULL; // Quiet gcc
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
    const u2* table = cUnit->insns + mir->offset + mir->dalvikInsn.vB;
    if (cUnit->printMe) {
        dumpSparseSwitchTable(table);
    }
    // Add the table to the list - we'll process it later
    SwitchTable *tabRec = (SwitchTable *)oatNew(sizeof(SwitchTable),
                         true);
    tabRec->table = table;
    tabRec->vaddr = mir->offset;
    int size = table[1];
    tabRec->targets = (ArmLIR* *)oatNew(size * sizeof(ArmLIR*), true);
    oatInsertGrowableList(&cUnit->switchTables, (intptr_t)tabRec);

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
    ArmLIR* target = newLIR0(cUnit, kArmPseudoTargetLabel);
    target->defMask = ENCODE_ALL;
    // Load next key/disp
    newLIR2(cUnit, kThumb2LdmiaWB, rBase, (1 << rKey) | (1 << rDisp));
    opRegReg(cUnit, kOpCmp, rKey, rlSrc.lowReg);
    // Go if match. NOTE: No instruction set switch here - must stay Thumb2
    genIT(cUnit, kArmCondEq, "");
    ArmLIR* switchBranch = newLIR1(cUnit, kThumb2AddPCR, rDisp);
    tabRec->bxInst = switchBranch;
    // Needs to use setflags encoding here
    newLIR3(cUnit, kThumb2SubsRRI12, rIdx, rIdx, 1);
    ArmLIR* branch = opCondBranch(cUnit, kArmCondNe);
    branch->generic.target = (LIR*)target;
}


STATIC void genPackedSwitch(CompilationUnit* cUnit, MIR* mir,
                            RegLocation rlSrc)
{
    const u2* table = cUnit->insns + mir->offset + mir->dalvikInsn.vB;
    if (cUnit->printMe) {
        dumpPackedSwitchTable(table);
    }
    // Add the table to the list - we'll process it later
    SwitchTable *tabRec = (SwitchTable *)oatNew(sizeof(SwitchTable),
                         true);
    tabRec->table = table;
    tabRec->vaddr = mir->offset;
    int size = table[1];
    tabRec->targets = (ArmLIR* *)oatNew(size * sizeof(ArmLIR*), true);
    oatInsertGrowableList(&cUnit->switchTables, (intptr_t)tabRec);

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
    ArmLIR* branchOver = opCondBranch(cUnit, kArmCondHi);

    // Load the displacement from the switch table
    int dispReg = oatAllocTemp(cUnit);
    loadBaseIndexed(cUnit, tableBase, keyReg, dispReg, 2, kWord);

    // ..and go! NOTE: No instruction set switch here - must stay Thumb2
    ArmLIR* switchBranch = newLIR1(cUnit, kThumb2AddPCR, dispReg);
    tabRec->bxInst = switchBranch;

    /* branchOver target here */
    ArmLIR* target = newLIR0(cUnit, kArmPseudoTargetLabel);
    target->defMask = ENCODE_ALL;
    branchOver->generic.target = (LIR*)target;
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
    const u2* table = cUnit->insns + mir->offset + mir->dalvikInsn.vB;
    // Add the table to the list - we'll process it later
    FillArrayData *tabRec = (FillArrayData *)
         oatNew(sizeof(FillArrayData), true);
    tabRec->table = table;
    tabRec->vaddr = mir->offset;
    u2 width = tabRec->table[1];
    u4 size = tabRec->table[2] | (((u4)tabRec->table[3]) << 16);
    tabRec->size = (size * width) + 8;

    oatInsertGrowableList(&cUnit->fillArrayData, (intptr_t)tabRec);

    // Making a call - use explicit registers
    oatFlushAllRegs(cUnit);   /* Everything to home location */
    loadValueDirectFixed(cUnit, rlSrc, r0);
    loadWordDisp(cUnit, rSELF,
                 OFFSETOF_MEMBER(Thread, pHandleFillArrayDataFromCode), rLR);
    // Materialize a pointer to the fill data image
    newLIR3(cUnit, kThumb2Adr, r1, 0, (intptr_t)tabRec);
    callRuntimeHelper(cUnit, rLR);
}

/*
 * Mark garbage collection card. Skip if the value we're storing is null.
 */
STATIC void markGCCard(CompilationUnit* cUnit, int valReg, int tgtAddrReg)
{
#ifdef CONCURRENT_GARBAGE_COLLECTOR
    // TODO: re-enable when concurrent collector is active
    int regCardBase = oatAllocTemp(cUnit);
    int regCardNo = oatAllocTemp(cUnit);
    ArmLIR* branchOver = genCmpImmBranch(cUnit, kArmCondEq, valReg, 0);
    loadWordDisp(cUnit, rSELF, Thread::CardTableOffset().Int32Value(),
                 regCardBase);
    opRegRegImm(cUnit, kOpLsr, regCardNo, tgtAddrReg, GC_CARD_SHIFT);
    storeBaseIndexed(cUnit, regCardBase, regCardNo, regCardBase, 0,
                     kUnsignedByte);
    ArmLIR* target = newLIR0(cUnit, kArmPseudoTargetLabel);
    target->defMask = ENCODE_ALL;
    branchOver->generic.target = (LIR*)target;
    oatFreeTemp(cUnit, regCardBase);
    oatFreeTemp(cUnit, regCardNo);
#endif
}

/*
 * Helper function for Iget/put when field not resolved at compile time.
 * Will trash call temps and return with the field offset in r0.
 */
STATIC void getFieldOffset(CompilationUnit* cUnit, MIR* mir, Field* fieldPtr)
{
    int fieldIdx = mir->dalvikInsn.vC;
    oatFlushAllRegs(cUnit);
    warnIfUnresolved(cUnit, fieldIdx, fieldPtr);
    oatLockCallTemps(cUnit);  // Explicit register usage
    loadCurrMethodDirect(cUnit, r1);              // arg1 <= Method*
    loadWordDisp(cUnit, r1,
                 Method::DexCacheResolvedFieldsOffset().Int32Value(), r0);
    loadWordDisp(cUnit, r0, art::Array::DataOffset().Int32Value() +
                 sizeof(int32_t*)* fieldIdx, r0);
    /*
     * For testing, omit the test for run-time resolution. This will
     * force all accesses to go through the runtime resolution path.
     */
    ArmLIR* branchOver = NULL;
    if (!EXERCISE_SLOWEST_FIELD_PATH) {
        branchOver = genCmpImmBranch(cUnit, kArmCondNe, r0, 0);
    }
    // Resolve
    loadWordDisp(cUnit, rSELF,
                 OFFSETOF_MEMBER(Thread, pFindInstanceFieldFromCode), rLR);
    loadConstant(cUnit, r0, fieldIdx);
    callRuntimeHelper(cUnit, rLR);  // resolveTypeFromCode(idx, method)
    ArmLIR* target = newLIR0(cUnit, kArmPseudoTargetLabel);
    target->defMask = ENCODE_ALL;
    if (!EXERCISE_SLOWEST_FIELD_PATH) {
        branchOver->generic.target = (LIR*)target;
    }
    // Free temps (except for r0)
    oatFreeTemp(cUnit, r1);
    oatFreeTemp(cUnit, r2);
    oatFreeTemp(cUnit, r3);
    loadWordDisp(cUnit, r0, art::Field::OffsetOffset().Int32Value(), r0);
}

STATIC void genIGet(CompilationUnit* cUnit, MIR* mir, OpSize size,
                     RegLocation rlDest, RegLocation rlObj)
{
    Field* fieldPtr = cUnit->method->GetDeclaringClass()->GetDexCache()->
        GetResolvedField(mir->dalvikInsn.vC);
    RegLocation rlResult;
    RegisterClass regClass = oatRegClassBySize(size);
    if (SLOW_FIELD_PATH || fieldPtr == NULL) {
        getFieldOffset(cUnit, mir, fieldPtr);
        // Field offset in r0
        rlObj = loadValue(cUnit, rlObj, kCoreReg);
        rlResult = oatEvalLoc(cUnit, rlDest, regClass, true);
        genNullCheck(cUnit, rlObj.sRegLow, rlObj.lowReg, mir);/* null object? */
        loadBaseIndexed(cUnit, rlObj.lowReg, r0, rlResult.lowReg, 0, kWord);
        oatGenMemBarrier(cUnit, kSY);
        storeValue(cUnit, rlDest, rlResult);
    } else {
#if ANDROID_SMP != 0
        bool isVolatile = fieldPtr->IsVolatile();
#else
        bool isVolatile = false;
#endif
        int fieldOffset = fieldPtr->GetOffset().Int32Value();
        rlObj = loadValue(cUnit, rlObj, kCoreReg);
        rlResult = oatEvalLoc(cUnit, rlDest, regClass, true);
        genNullCheck(cUnit, rlObj.sRegLow, rlObj.lowReg, mir);/* null object? */
        loadBaseDisp(cUnit, mir, rlObj.lowReg, fieldOffset, rlResult.lowReg,
                     kWord, rlObj.sRegLow);
        if (isVolatile) {
            oatGenMemBarrier(cUnit, kSY);
        }
        storeValue(cUnit, rlDest, rlResult);
    }
}

STATIC void genIPut(CompilationUnit* cUnit, MIR* mir, OpSize size,
                    RegLocation rlSrc, RegLocation rlObj, bool isObject)
{
    Field* fieldPtr = cUnit->method->GetDeclaringClass()->GetDexCache()->
        GetResolvedField(mir->dalvikInsn.vC);
    RegisterClass regClass = oatRegClassBySize(size);
    if (SLOW_FIELD_PATH || fieldPtr == NULL) {
        getFieldOffset(cUnit, mir, fieldPtr);
        // Field offset in r0
        rlObj = loadValue(cUnit, rlObj, kCoreReg);
        rlSrc = loadValue(cUnit, rlSrc, regClass);
        genNullCheck(cUnit, rlObj.sRegLow, rlObj.lowReg, mir);/* null object? */
        oatGenMemBarrier(cUnit, kSY);
        storeBaseIndexed(cUnit, rlObj.lowReg, r0, rlSrc.lowReg, 0, kWord);
    } else {
#if ANDROID_SMP != 0
        bool isVolatile = fieldPtr->IsVolatile();
#else
        bool isVolatile = false;
#endif
        int fieldOffset = fieldPtr->GetOffset().Int32Value();
        rlObj = loadValue(cUnit, rlObj, kCoreReg);
        rlSrc = loadValue(cUnit, rlSrc, regClass);
        genNullCheck(cUnit, rlObj.sRegLow, rlObj.lowReg, mir);/* null obj? */

        if (isVolatile) {
            oatGenMemBarrier(cUnit, kST);
        }
        storeBaseDisp(cUnit, rlObj.lowReg, fieldOffset, rlSrc.lowReg, kWord);
        if (isVolatile) {
            oatGenMemBarrier(cUnit, kSY);
        }
    }
    if (isObject) {
        /* NOTE: marking card based on object head */
        markGCCard(cUnit, rlSrc.lowReg, rlObj.lowReg);
    }
}

STATIC void genIGetWide(CompilationUnit* cUnit, MIR* mir, RegLocation rlDest,
                        RegLocation rlObj)
{
    RegLocation rlResult;
    Field* fieldPtr = cUnit->method->GetDeclaringClass()->GetDexCache()->
        GetResolvedField(mir->dalvikInsn.vC);
#if ANDROID_SMP != 0
    bool isVolatile = (fieldPtr == NULL) || fieldPtr->IsVolatile();
#else
    bool isVolatile = false;
#endif
    if (SLOW_FIELD_PATH || (fieldPtr == NULL) || isVolatile) {
        getFieldOffset(cUnit, mir, fieldPtr);
        // Field offset in r0
        rlObj = loadValue(cUnit, rlObj, kCoreReg);
        rlResult = oatEvalLoc(cUnit, rlDest, kAnyReg, true);
        genNullCheck(cUnit, rlObj.sRegLow, rlObj.lowReg, mir);/* null obj? */
        opRegReg(cUnit, kOpAdd, r0, rlObj.lowReg);
        loadPair(cUnit, r0, rlResult.lowReg, rlResult.highReg);
        oatGenMemBarrier(cUnit, kSY);
        storeValueWide(cUnit, rlDest, rlResult);
    } else {
        int fieldOffset = fieldPtr->GetOffset().Int32Value();
        rlObj = loadValue(cUnit, rlObj, kCoreReg);
        int regPtr = oatAllocTemp(cUnit);

        DCHECK(rlDest.wide);

        genNullCheck(cUnit, rlObj.sRegLow, rlObj.lowReg, mir);/* null obj? */
        opRegRegImm(cUnit, kOpAdd, regPtr, rlObj.lowReg, fieldOffset);
        rlResult = oatEvalLoc(cUnit, rlDest, kAnyReg, true);

        loadPair(cUnit, regPtr, rlResult.lowReg, rlResult.highReg);

        oatFreeTemp(cUnit, regPtr);
        storeValueWide(cUnit, rlDest, rlResult);
    }
}

STATIC void genIPutWide(CompilationUnit* cUnit, MIR* mir, RegLocation rlSrc,
                        RegLocation rlObj)
{
    Field* fieldPtr = cUnit->method->GetDeclaringClass()->GetDexCache()->
        GetResolvedField(mir->dalvikInsn.vC);
#if ANDROID_SMP != 0
    bool isVolatile = (fieldPtr == NULL) || fieldPtr->IsVolatile();
#else
    bool isVolatile = false;
#endif
    if (SLOW_FIELD_PATH || (fieldPtr == NULL) || isVolatile) {
        getFieldOffset(cUnit, mir, fieldPtr);
        // Field offset in r0
        rlObj = loadValue(cUnit, rlObj, kCoreReg);
        rlSrc = loadValueWide(cUnit, rlSrc, kAnyReg);
        genNullCheck(cUnit, rlObj.sRegLow, rlObj.lowReg, mir);/* null obj? */
        opRegReg(cUnit, kOpAdd, r0, rlObj.lowReg);
        oatGenMemBarrier(cUnit, kSY);
        storePair(cUnit, r0, rlSrc.lowReg, rlSrc.highReg);
    } else {
        int fieldOffset = fieldPtr->GetOffset().Int32Value();

        rlObj = loadValue(cUnit, rlObj, kCoreReg);
        int regPtr;
        rlSrc = loadValueWide(cUnit, rlSrc, kAnyReg);
        genNullCheck(cUnit, rlObj.sRegLow, rlObj.lowReg, mir);/* null obj? */
        regPtr = oatAllocTemp(cUnit);
        opRegRegImm(cUnit, kOpAdd, regPtr, rlObj.lowReg, fieldOffset);

        storePair(cUnit, regPtr, rlSrc.lowReg, rlSrc.highReg);

        oatFreeTemp(cUnit, regPtr);
    }
}

STATIC void genConstClass(CompilationUnit* cUnit, MIR* mir,
                          RegLocation rlDest, RegLocation rlSrc)
{
    art::Class* classPtr = cUnit->method->GetDexCacheResolvedTypes()->
        Get(mir->dalvikInsn.vB);
    int mReg = loadCurrMethod(cUnit);
    int resReg = oatAllocTemp(cUnit);
    RegLocation rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
    loadWordDisp(cUnit, mReg, Method::DexCacheResolvedTypesOffset().Int32Value(),
                 resReg);
    loadWordDisp(cUnit, resReg, Array::DataOffset().Int32Value() +
                 (sizeof(String*) * mir->dalvikInsn.vB), rlResult.lowReg);
    if (SLOW_TYPE_PATH || (classPtr == NULL)) {
        // Fast path, we're done - just store result
        storeValue(cUnit, rlDest, rlResult);
    } else {
        // Slow path.  Must test at runtime
        oatFlushAllRegs(cUnit);
        ArmLIR* branch1 = genCmpImmBranch(cUnit, kArmCondEq, rlResult.lowReg,
                                          0);
        // Resolved, store and hop over following code
        storeValue(cUnit, rlDest, rlResult);
        ArmLIR* branch2 = genUnconditionalBranch(cUnit,0);
        // TUNING: move slow path to end & remove unconditional branch
        ArmLIR* target1 = newLIR0(cUnit, kArmPseudoTargetLabel);
        target1->defMask = ENCODE_ALL;
        // Call out to helper, which will return resolved type in r0
        loadWordDisp(cUnit, rSELF,
                     OFFSETOF_MEMBER(Thread, pInitializeTypeFromCode), rLR);
        genRegCopy(cUnit, r1, mReg);
        loadConstant(cUnit, r0, mir->dalvikInsn.vB);
        callRuntimeHelper(cUnit, rLR);
        RegLocation rlResult = oatGetReturn(cUnit);
        storeValue(cUnit, rlDest, rlResult);
        // Rejoin code paths
        ArmLIR* target2 = newLIR0(cUnit, kArmPseudoTargetLabel);
        target2->defMask = ENCODE_ALL;
        branch1->generic.target = (LIR*)target1;
        branch2->generic.target = (LIR*)target2;
    }
}

STATIC void genConstString(CompilationUnit* cUnit, MIR* mir,
                           RegLocation rlDest, RegLocation rlSrc)
{
    /* NOTE: Most strings should be available at compile time */
    const art::String* str = cUnit->method->GetDexCacheStrings()->
        Get(mir->dalvikInsn.vB);
    if (SLOW_STRING_PATH || (str == NULL) || !cUnit->compiler->IsImage()) {
        oatFlushAllRegs(cUnit);
        oatLockCallTemps(cUnit); // Using explicit registers
        loadCurrMethodDirect(cUnit, r2);
        loadWordDisp(cUnit, r2, Method::DexCacheStringsOffset().Int32Value(),
                     r0);
        // Might call out to helper, which will return resolved string in r0
        loadWordDisp(cUnit, rSELF,
                     OFFSETOF_MEMBER(Thread, pResolveStringFromCode), rLR);
        loadWordDisp(cUnit, r0, Array::DataOffset().Int32Value() +
                 (sizeof(String*) * mir->dalvikInsn.vB), r0);
        loadConstant(cUnit, r1, mir->dalvikInsn.vB);
        opRegImm(cUnit, kOpCmp, r0, 0);  // Is resolved?
        genBarrier(cUnit);
        // For testing, always force through helper
        if (!EXERCISE_SLOWEST_STRING_PATH) {
            genIT(cUnit, kArmCondEq, "T");
        }
        genRegCopy(cUnit, r0, r2);       // .eq
        opReg(cUnit, kOpBlx, rLR);       // .eq, helper(Method*, string_idx)
        genBarrier(cUnit);
        storeValue(cUnit, rlDest, getRetLoc(cUnit));
    } else {
        int mReg = loadCurrMethod(cUnit);
        int resReg = oatAllocTemp(cUnit);
        RegLocation rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
        loadWordDisp(cUnit, mReg, Method::DexCacheStringsOffset().Int32Value(),
                     resReg);
        loadWordDisp(cUnit, resReg, Array::DataOffset().Int32Value() +
                    (sizeof(String*) * mir->dalvikInsn.vB), rlResult.lowReg);
        storeValue(cUnit, rlDest, rlResult);
    }
}

/*
 * Let helper function take care of everything.  Will
 * call Class::NewInstanceFromCode(type_idx, method);
 */
STATIC void genNewInstance(CompilationUnit* cUnit, MIR* mir,
                           RegLocation rlDest)
{
    oatFlushAllRegs(cUnit);    /* Everything to home location */
    loadWordDisp(cUnit, rSELF,
                 OFFSETOF_MEMBER(Thread, pAllocObjectFromCode), rLR);
    loadCurrMethodDirect(cUnit, r1);              // arg1 <= Method*
    loadConstant(cUnit, r0, mir->dalvikInsn.vB);  // arg0 <- type_id
    callRuntimeHelper(cUnit, rLR);
    RegLocation rlResult = oatGetReturn(cUnit);
    storeValue(cUnit, rlDest, rlResult);
}

void genThrow(CompilationUnit* cUnit, MIR* mir, RegLocation rlSrc)
{
    oatFlushAllRegs(cUnit);
    loadWordDisp(cUnit, rSELF,
                 OFFSETOF_MEMBER(Thread, pDeliverException), rLR);
    loadValueDirectFixed(cUnit, rlSrc, r0);  // Get exception object
    callRuntimeHelper(cUnit, rLR);  // art_deliver_exception(exception);
}

STATIC void genInstanceof(CompilationUnit* cUnit, MIR* mir, RegLocation rlDest,
                          RegLocation rlSrc)
{
    oatFlushAllRegs(cUnit);
    // May generate a call - use explicit registers
    oatLockCallTemps(cUnit);
    art::Class* classPtr = cUnit->method->GetDexCacheResolvedTypes()->
        Get(mir->dalvikInsn.vC);
    int classReg = r2;   // Fixed usage
    loadCurrMethodDirect(cUnit, r1);  // r1 <= current Method*
    loadValueDirectFixed(cUnit, rlSrc, r0);  /* Ref */
    loadWordDisp(cUnit, r1, Method::DexCacheResolvedTypesOffset().Int32Value(),
                 classReg);
    loadWordDisp(cUnit, classReg, Array::DataOffset().Int32Value() +
                 (sizeof(String*) * mir->dalvikInsn.vC), classReg);
    if (classPtr == NULL) {
        // Generate a runtime test
        ArmLIR* hopBranch = genCmpImmBranch(cUnit, kArmCondNe, classReg, 0);
        // Not resolved
        // Call out to helper, which will return resolved type in r0
        loadWordDisp(cUnit, rSELF,
                     OFFSETOF_MEMBER(Thread, pInitializeTypeFromCode), rLR);
        loadConstant(cUnit, r0, mir->dalvikInsn.vC);
        callRuntimeHelper(cUnit, rLR);  // resolveTypeFromCode(idx, method)
        genRegCopy(cUnit, r2, r0); // Align usage with fast path
        loadValueDirectFixed(cUnit, rlSrc, r0);  /* reload Ref */
        // Rejoin code paths
        ArmLIR* hopTarget = newLIR0(cUnit, kArmPseudoTargetLabel);
        hopTarget->defMask = ENCODE_ALL;
        hopBranch->generic.target = (LIR*)hopTarget;
    }
    /* r0 is ref, r2 is class.  If ref==null, use directly as bool result */
    ArmLIR* branch1 = genCmpImmBranch(cUnit, kArmCondEq, r0, 0);
    /* load object->clazz */
    DCHECK_EQ(Object::ClassOffset().Int32Value(), 0);
    loadWordDisp(cUnit, r0,  Object::ClassOffset().Int32Value(), r1);
    /* r0 is ref, r1 is ref->clazz, r2 is class */
    loadWordDisp(cUnit, rSELF,
                 OFFSETOF_MEMBER(Thread, pInstanceofNonTrivialFromCode), rLR);
    opRegReg(cUnit, kOpCmp, r1, r2);  // Same?
    genBarrier(cUnit);
    genIT(cUnit, kArmCondEq, "EE");   // if-convert the test
    loadConstant(cUnit, r0, 1);       // .eq case - load true
    genRegCopy(cUnit, r0, r2);        // .ne case - arg0 <= class
    opReg(cUnit, kOpBlx, rLR);        // .ne case: helper(class, ref->class)
    genBarrier(cUnit);
    oatClobberCalleeSave(cUnit);
    /* branch target here */
    ArmLIR* target = newLIR0(cUnit, kArmPseudoTargetLabel);
    target->defMask = ENCODE_ALL;
    RegLocation rlResult = oatGetReturn(cUnit);
    storeValue(cUnit, rlDest, rlResult);
    branch1->generic.target = (LIR*)target;
}

STATIC void genCheckCast(CompilationUnit* cUnit, MIR* mir, RegLocation rlSrc)
{
    oatFlushAllRegs(cUnit);
    // May generate a call - use explicit registers
    oatLockCallTemps(cUnit);
    art::Class* classPtr = cUnit->method->GetDexCacheResolvedTypes()->
        Get(mir->dalvikInsn.vB);
    int classReg = r2;   // Fixed usage
    loadCurrMethodDirect(cUnit, r1);  // r1 <= current Method*
    loadWordDisp(cUnit, r1, Method::DexCacheResolvedTypesOffset().Int32Value(),
                 classReg);
    loadWordDisp(cUnit, classReg, Array::DataOffset().Int32Value() +
                 (sizeof(String*) * mir->dalvikInsn.vB), classReg);
    if (classPtr == NULL) {
        // Generate a runtime test
        ArmLIR* hopBranch = genCmpImmBranch(cUnit, kArmCondNe, classReg, 0);
        // Not resolved
        // Call out to helper, which will return resolved type in r0
        loadWordDisp(cUnit, rSELF,
                     OFFSETOF_MEMBER(Thread, pInitializeTypeFromCode), rLR);
        loadConstant(cUnit, r0, mir->dalvikInsn.vB);
        callRuntimeHelper(cUnit, rLR);  // resolveTypeFromCode(idx, method)
        genRegCopy(cUnit, r2, r0); // Align usage with fast path
        // Rejoin code paths
        ArmLIR* hopTarget = newLIR0(cUnit, kArmPseudoTargetLabel);
        hopTarget->defMask = ENCODE_ALL;
        hopBranch->generic.target = (LIR*)hopTarget;
    }
    // At this point, r2 has class
    loadValueDirectFixed(cUnit, rlSrc, r0);  /* Ref */
    /* Null is OK - continue */
    ArmLIR* branch1 = genCmpImmBranch(cUnit, kArmCondEq, r0, 0);
    /* load object->clazz */
    DCHECK_EQ(Object::ClassOffset().Int32Value(), 0);
    loadWordDisp(cUnit, r0,  Object::ClassOffset().Int32Value(), r1);
    /* r1 now contains object->clazz */
    loadWordDisp(cUnit, rSELF,
                 OFFSETOF_MEMBER(Thread, pCheckCastFromCode), rLR);
    opRegReg(cUnit, kOpCmp, r1, r2);
    ArmLIR* branch2 = opCondBranch(cUnit, kArmCondEq); /* If equal, trivial yes */
    genRegCopy(cUnit, r0, r1);
    genRegCopy(cUnit, r1, r2);
    callRuntimeHelper(cUnit, rLR);
    /* branch target here */
    ArmLIR* target = newLIR0(cUnit, kArmPseudoTargetLabel);
    target->defMask = ENCODE_ALL;
    branch1->generic.target = (LIR*)target;
    branch2->generic.target = (LIR*)target;
}

STATIC void genNegFloat(CompilationUnit* cUnit, RegLocation rlDest,
                        RegLocation rlSrc)
{
    RegLocation rlResult;
    rlSrc = loadValue(cUnit, rlSrc, kFPReg);
    rlResult = oatEvalLoc(cUnit, rlDest, kFPReg, true);
    newLIR2(cUnit, kThumb2Vnegs, rlResult.lowReg, rlSrc.lowReg);
    storeValue(cUnit, rlDest, rlResult);
}

STATIC void genNegDouble(CompilationUnit* cUnit, RegLocation rlDest,
                         RegLocation rlSrc)
{
    RegLocation rlResult;
    rlSrc = loadValueWide(cUnit, rlSrc, kFPReg);
    rlResult = oatEvalLoc(cUnit, rlDest, kFPReg, true);
    newLIR2(cUnit, kThumb2Vnegd, S2D(rlResult.lowReg, rlResult.highReg),
            S2D(rlSrc.lowReg, rlSrc.highReg));
    storeValueWide(cUnit, rlDest, rlResult);
}

STATIC void freeRegLocTemps(CompilationUnit* cUnit, RegLocation rlKeep,
                        RegLocation rlFree)
{
    if ((rlFree.lowReg != rlKeep.lowReg) && (rlFree.lowReg != rlKeep.highReg) &&
        (rlFree.highReg != rlKeep.lowReg) && (rlFree.highReg != rlKeep.highReg)) {
        // No overlap, free both
        oatFreeTemp(cUnit, rlFree.lowReg);
        oatFreeTemp(cUnit, rlFree.highReg);
    }
}

STATIC void genLong3Addr(CompilationUnit* cUnit, MIR* mir, OpKind firstOp,
                         OpKind secondOp, RegLocation rlDest,
                         RegLocation rlSrc1, RegLocation rlSrc2)
{
    /*
     * NOTE:  This is the one place in the code in which we might have
     * as many as six live temporary registers.  There are 5 in the normal
     * set for Arm.  Until we have spill capabilities, temporarily add
     * lr to the temp set.  It is safe to do this locally, but note that
     * lr is used explicitly elsewhere in the code generator and cannot
     * normally be used as a general temp register.
     */
    RegLocation rlResult;
    oatMarkTemp(cUnit, rLR);   // Add lr to the temp pool
    oatFreeTemp(cUnit, rLR);   // and make it available
    rlSrc1 = loadValueWide(cUnit, rlSrc1, kCoreReg);
    rlSrc2 = loadValueWide(cUnit, rlSrc2, kCoreReg);
    rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
    // The longs may overlap - use intermediate temp if so
    if (rlResult.lowReg == rlSrc1.highReg) {
        int tReg = oatAllocTemp(cUnit);
        genRegCopy(cUnit, tReg, rlSrc1.highReg);
        opRegRegReg(cUnit, firstOp, rlResult.lowReg, rlSrc1.lowReg,
                    rlSrc2.lowReg);
        opRegRegReg(cUnit, secondOp, rlResult.highReg, tReg,
                    rlSrc2.highReg);
        oatFreeTemp(cUnit, tReg);
    } else {
        opRegRegReg(cUnit, firstOp, rlResult.lowReg, rlSrc1.lowReg,
                    rlSrc2.lowReg);
        opRegRegReg(cUnit, secondOp, rlResult.highReg, rlSrc1.highReg,
                    rlSrc2.highReg);
    }
    /*
     * NOTE: If rlDest refers to a frame variable in a large frame, the
     * following storeValueWide might need to allocate a temp register.
     * To further work around the lack of a spill capability, explicitly
     * free any temps from rlSrc1 & rlSrc2 that aren't still live in rlResult.
     * Remove when spill is functional.
     */
    freeRegLocTemps(cUnit, rlResult, rlSrc1);
    freeRegLocTemps(cUnit, rlResult, rlSrc2);
    storeValueWide(cUnit, rlDest, rlResult);
    oatClobber(cUnit, rLR);
    oatUnmarkTemp(cUnit, rLR);  // Remove lr from the temp pool
}

void oatInitializeRegAlloc(CompilationUnit* cUnit)
{
    int numRegs = sizeof(coreRegs)/sizeof(*coreRegs);
    int numReserved = sizeof(reservedRegs)/sizeof(*reservedRegs);
    int numTemps = sizeof(coreTemps)/sizeof(*coreTemps);
    int numFPRegs = sizeof(fpRegs)/sizeof(*fpRegs);
    int numFPTemps = sizeof(fpTemps)/sizeof(*fpTemps);
    RegisterPool *pool = (RegisterPool *)oatNew(sizeof(*pool), true);
    cUnit->regPool = pool;
    pool->numCoreRegs = numRegs;
    pool->coreRegs = (RegisterInfo *)
            oatNew(numRegs * sizeof(*cUnit->regPool->coreRegs), true);
    pool->numFPRegs = numFPRegs;
    pool->FPRegs = (RegisterInfo *)
            oatNew(numFPRegs * sizeof(*cUnit->regPool->FPRegs), true);
    oatInitPool(pool->coreRegs, coreRegs, pool->numCoreRegs);
    oatInitPool(pool->FPRegs, fpRegs, pool->numFPRegs);
    // Keep special registers from being allocated
    for (int i = 0; i < numReserved; i++) {
        if (NO_SUSPEND && (reservedRegs[i] == rSUSPEND)) {
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
    cUnit->phiAliasMap = (int*)oatNew(cUnit->numSSARegs *
                                      sizeof(cUnit->phiAliasMap[0]), false);
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

/*
 * Handle simple case (thin lock) inline.  If it's complicated, bail
 * out to the heavyweight lock/unlock routines.  We'll use dedicated
 * registers here in order to be in the right position in case we
 * to bail to dvm[Lock/Unlock]Object(self, object)
 *
 * r0 -> self pointer [arg0 for dvm[Lock/Unlock]Object
 * r1 -> object [arg1 for dvm[Lock/Unlock]Object
 * r2 -> intial contents of object->lock, later result of strex
 * r3 -> self->threadId
 * r12 -> allow to be used by utilities as general temp
 *
 * The result of the strex is 0 if we acquire the lock.
 *
 * See comments in Sync.c for the layout of the lock word.
 * Of particular interest to this code is the test for the
 * simple case - which we handle inline.  For monitor enter, the
 * simple case is thin lock, held by no-one.  For monitor exit,
 * the simple case is thin lock, held by the unlocking thread with
 * a recurse count of 0.
 *
 * A minor complication is that there is a field in the lock word
 * unrelated to locking: the hash state.  This field must be ignored, but
 * preserved.
 *
 */
STATIC void genMonitorEnter(CompilationUnit* cUnit, MIR* mir,
                            RegLocation rlSrc)
{
    ArmLIR* target;
    ArmLIR* hopTarget;
    ArmLIR* branch;
    ArmLIR* hopBranch;

    oatFlushAllRegs(cUnit);
    DCHECK_EQ(LW_SHAPE_THIN, 0);
    loadValueDirectFixed(cUnit, rlSrc, r0);  // Get obj
    oatLockCallTemps(cUnit);  // Prepare for explicit register usage
    genNullCheck(cUnit, rlSrc.sRegLow, r0, mir);
    loadWordDisp(cUnit, rSELF, Thread::ThinLockIdOffset().Int32Value(), r2);
    newLIR3(cUnit, kThumb2Ldrex, r1, r0,
            Object::MonitorOffset().Int32Value() >> 2); // Get object->lock
    // Align owner
    opRegImm(cUnit, kOpLsl, r2, LW_LOCK_OWNER_SHIFT);
    // Is lock unheld on lock or held by us (==threadId) on unlock?
    newLIR4(cUnit, kThumb2Bfi, r2, r1, 0, LW_LOCK_OWNER_SHIFT - 1);
    newLIR3(cUnit, kThumb2Bfc, r1, LW_HASH_STATE_SHIFT, LW_LOCK_OWNER_SHIFT - 1);
    hopBranch = newLIR2(cUnit, kThumb2Cbnz, r1, 0);
    newLIR4(cUnit, kThumb2Strex, r1, r2, r0,
            Object::MonitorOffset().Int32Value() >> 2);
    oatGenMemBarrier(cUnit, kSY);
    branch = newLIR2(cUnit, kThumb2Cbz, r1, 0);

    hopTarget = newLIR0(cUnit, kArmPseudoTargetLabel);
    hopTarget->defMask = ENCODE_ALL;
    hopBranch->generic.target = (LIR*)hopTarget;

    // Go expensive route - artLockObjectFromCode(self, obj);
    loadWordDisp(cUnit, rSELF, OFFSETOF_MEMBER(Thread, pLockObjectFromCode),
                 rLR);
    callRuntimeHelper(cUnit, rLR);

    // Resume here
    target = newLIR0(cUnit, kArmPseudoTargetLabel);
    target->defMask = ENCODE_ALL;
    branch->generic.target = (LIR*)target;
}

/*
 * For monitor unlock, we don't have to use ldrex/strex.  Once
 * we've determined that the lock is thin and that we own it with
 * a zero recursion count, it's safe to punch it back to the
 * initial, unlock thin state with a store word.
 */
STATIC void genMonitorExit(CompilationUnit* cUnit, MIR* mir,
                           RegLocation rlSrc)
{
    ArmLIR* target;
    ArmLIR* branch;
    ArmLIR* hopTarget;
    ArmLIR* hopBranch;

    DCHECK_EQ(LW_SHAPE_THIN, 0);
    oatFlushAllRegs(cUnit);
    loadValueDirectFixed(cUnit, rlSrc, r0);  // Get obj
    oatLockCallTemps(cUnit);  // Prepare for explicit register usage
    genNullCheck(cUnit, rlSrc.sRegLow, r0, mir);
    loadWordDisp(cUnit, r0, Object::MonitorOffset().Int32Value(), r1); // Get lock
    loadWordDisp(cUnit, rSELF, Thread::ThinLockIdOffset().Int32Value(), r2);
    // Is lock unheld on lock or held by us (==threadId) on unlock?
    opRegRegImm(cUnit, kOpAnd, r3, r1, (LW_HASH_STATE_MASK << LW_HASH_STATE_SHIFT));
    // Align owner
    opRegImm(cUnit, kOpLsl, r2, LW_LOCK_OWNER_SHIFT);
    newLIR3(cUnit, kThumb2Bfc, r1, LW_HASH_STATE_SHIFT, LW_LOCK_OWNER_SHIFT - 1);
    opRegReg(cUnit, kOpSub, r1, r2);
    hopBranch = opCondBranch(cUnit, kArmCondNe);
    oatGenMemBarrier(cUnit, kSY);
    storeWordDisp(cUnit, r0, Object::MonitorOffset().Int32Value(), r3);
    branch = opNone(cUnit, kOpUncondBr);

    hopTarget = newLIR0(cUnit, kArmPseudoTargetLabel);
    hopTarget->defMask = ENCODE_ALL;
    hopBranch->generic.target = (LIR*)hopTarget;

    // Go expensive route - UnlockObjectFromCode(obj);
    loadWordDisp(cUnit, rSELF, OFFSETOF_MEMBER(Thread, pUnlockObjectFromCode),
                 rLR);
    callRuntimeHelper(cUnit, rLR);

    // Resume here
    target = newLIR0(cUnit, kArmPseudoTargetLabel);
    target->defMask = ENCODE_ALL;
    branch->generic.target = (LIR*)target;
}

/*
 * 64-bit 3way compare function.
 *     mov   rX, #-1
 *     cmp   op1hi, op2hi
 *     blt   done
 *     bgt   flip
 *     sub   rX, op1lo, op2lo (treat as unsigned)
 *     beq   done
 *     ite   hi
 *     mov(hi)   rX, #-1
 *     mov(!hi)  rX, #1
 * flip:
 *     neg   rX
 * done:
 */
STATIC void genCmpLong(CompilationUnit* cUnit, MIR* mir,
                       RegLocation rlDest, RegLocation rlSrc1,
                       RegLocation rlSrc2)
{
    ArmLIR* target1;
    ArmLIR* target2;
    rlSrc1 = loadValueWide(cUnit, rlSrc1, kCoreReg);
    rlSrc2 = loadValueWide(cUnit, rlSrc2, kCoreReg);
    int tReg = oatAllocTemp(cUnit);
    loadConstant(cUnit, tReg, -1);
    opRegReg(cUnit, kOpCmp, rlSrc1.highReg, rlSrc2.highReg);
    ArmLIR* branch1 = opCondBranch(cUnit, kArmCondLt);
    ArmLIR* branch2 = opCondBranch(cUnit, kArmCondGt);
    opRegRegReg(cUnit, kOpSub, tReg, rlSrc1.lowReg, rlSrc2.lowReg);
    ArmLIR* branch3 = opCondBranch(cUnit, kArmCondEq);

    genIT(cUnit, kArmCondHi, "E");
    newLIR2(cUnit, kThumb2MovImmShift, tReg, modifiedImmediate(-1));
    loadConstant(cUnit, tReg, 1);
    genBarrier(cUnit);

    target2 = newLIR0(cUnit, kArmPseudoTargetLabel);
    target2->defMask = -1;
    opRegReg(cUnit, kOpNeg, tReg, tReg);

    target1 = newLIR0(cUnit, kArmPseudoTargetLabel);
    target1->defMask = -1;

    RegLocation rlTemp = LOC_C_RETURN; // Just using as template, will change
    rlTemp.lowReg = tReg;
    storeValue(cUnit, rlDest, rlTemp);
    oatFreeTemp(cUnit, tReg);

    branch1->generic.target = (LIR*)target1;
    branch2->generic.target = (LIR*)target2;
    branch3->generic.target = branch1->generic.target;
}

STATIC void genMultiplyByTwoBitMultiplier(CompilationUnit* cUnit,
        RegLocation rlSrc, RegLocation rlResult, int lit,
        int firstBit, int secondBit)
{
    opRegRegRegShift(cUnit, kOpAdd, rlResult.lowReg, rlSrc.lowReg, rlSrc.lowReg,
                     encodeShift(kArmLsl, secondBit - firstBit));
    if (firstBit != 0) {
        opRegRegImm(cUnit, kOpLsl, rlResult.lowReg, rlResult.lowReg, firstBit);
    }
}

STATIC bool genConversionCall(CompilationUnit* cUnit, MIR* mir, int funcOffset,
                                     int srcSize, int tgtSize)
{
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
}

STATIC bool genArithOpFloatPortable(CompilationUnit* cUnit, MIR* mir,
                                    RegLocation rlDest, RegLocation rlSrc1,
                                    RegLocation rlSrc2)
{
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
}

STATIC bool genArithOpDoublePortable(CompilationUnit* cUnit, MIR* mir,
                                     RegLocation rlDest, RegLocation rlSrc1,
                                     RegLocation rlSrc2)
{
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
}

STATIC bool genConversionPortable(CompilationUnit* cUnit, MIR* mir)
{
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
}

/* Generate conditional branch instructions */
STATIC ArmLIR* genConditionalBranch(CompilationUnit* cUnit,
                                    ArmConditionCode cond,
                                    ArmLIR* target)
{
    ArmLIR* branch = opCondBranch(cUnit, cond);
    branch->generic.target = (LIR*) target;
    return branch;
}

/*
 * Generate array store
 *
 */
STATIC void genArrayObjPut(CompilationUnit* cUnit, MIR* mir,
                           RegLocation rlArray, RegLocation rlIndex,
                           RegLocation rlSrc, int scale)
{
    RegisterClass regClass = oatRegClassBySize(kWord);
    int lenOffset = Array::LengthOffset().Int32Value();
    int dataOffset = Array::DataOffset().Int32Value();

    oatFlushAllRegs(cUnit);
    /* Make sure it's a legal object Put. Use direct regs at first */
    loadValueDirectFixed(cUnit, rlArray, r1);
    loadValueDirectFixed(cUnit, rlSrc, r0);

    /* null array object? */
    genNullCheck(cUnit, rlArray.sRegLow, r1, mir);
    loadWordDisp(cUnit, rSELF,
                 OFFSETOF_MEMBER(Thread, pCanPutArrayElementFromCode), rLR);
    /* Get the array's clazz */
    loadWordDisp(cUnit, r1, Object::ClassOffset().Int32Value(), r1);
    callRuntimeHelper(cUnit, rLR);
    oatFreeTemp(cUnit, r0);
    oatFreeTemp(cUnit, r1);

    // Now, redo loadValues in case they didn't survive the call

    int regPtr;
    rlArray = loadValue(cUnit, rlArray, kCoreReg);
    rlIndex = loadValue(cUnit, rlIndex, kCoreReg);

    if (oatIsTemp(cUnit, rlArray.lowReg)) {
        oatClobber(cUnit, rlArray.lowReg);
        regPtr = rlArray.lowReg;
    } else {
        regPtr = oatAllocTemp(cUnit);
        genRegCopy(cUnit, regPtr, rlArray.lowReg);
    }

    if (!(mir->optimizationFlags & MIR_IGNORE_RANGE_CHECK)) {
        int regLen = oatAllocTemp(cUnit);
        //NOTE: max live temps(4) here.
        /* Get len */
        loadWordDisp(cUnit, rlArray.lowReg, lenOffset, regLen);
        /* regPtr -> array data */
        opRegImm(cUnit, kOpAdd, regPtr, dataOffset);
        genRegRegCheck(cUnit, kArmCondCs, rlIndex.lowReg, regLen, mir,
                       kArmThrowArrayBounds);
        oatFreeTemp(cUnit, regLen);
    } else {
        /* regPtr -> array data */
        opRegImm(cUnit, kOpAdd, regPtr, dataOffset);
    }
    /* at this point, regPtr points to array, 2 live temps */
    rlSrc = loadValue(cUnit, rlSrc, regClass);
    storeBaseIndexed(cUnit, regPtr, rlIndex.lowReg, rlSrc.lowReg,
                     scale, kWord);
}

/*
 * Generate array load
 */
STATIC void genArrayGet(CompilationUnit* cUnit, MIR* mir, OpSize size,
                        RegLocation rlArray, RegLocation rlIndex,
                        RegLocation rlDest, int scale)
{
    RegisterClass regClass = oatRegClassBySize(size);
    int lenOffset = Array::LengthOffset().Int32Value();
    int dataOffset = Array::DataOffset().Int32Value();
    RegLocation rlResult;
    rlArray = loadValue(cUnit, rlArray, kCoreReg);
    rlIndex = loadValue(cUnit, rlIndex, kCoreReg);
    int regPtr;

    /* null object? */
    genNullCheck(cUnit, rlArray.sRegLow, rlArray.lowReg, mir);

    regPtr = oatAllocTemp(cUnit);

    if (!(mir->optimizationFlags & MIR_IGNORE_RANGE_CHECK)) {
        int regLen = oatAllocTemp(cUnit);
        /* Get len */
        loadWordDisp(cUnit, rlArray.lowReg, lenOffset, regLen);
        /* regPtr -> array data */
        opRegRegImm(cUnit, kOpAdd, regPtr, rlArray.lowReg, dataOffset);
        genRegRegCheck(cUnit, kArmCondCs, rlIndex.lowReg, regLen, mir,
                       kArmThrowArrayBounds);
        oatFreeTemp(cUnit, regLen);
    } else {
        /* regPtr -> array data */
        opRegRegImm(cUnit, kOpAdd, regPtr, rlArray.lowReg, dataOffset);
    }
    oatFreeTemp(cUnit, rlArray.lowReg);
    if ((size == kLong) || (size == kDouble)) {
        if (scale) {
            int rNewIndex = oatAllocTemp(cUnit);
            opRegRegImm(cUnit, kOpLsl, rNewIndex, rlIndex.lowReg, scale);
            opRegReg(cUnit, kOpAdd, regPtr, rNewIndex);
            oatFreeTemp(cUnit, rNewIndex);
        } else {
            opRegReg(cUnit, kOpAdd, regPtr, rlIndex.lowReg);
        }
        oatFreeTemp(cUnit, rlIndex.lowReg);
        rlResult = oatEvalLoc(cUnit, rlDest, regClass, true);

        loadPair(cUnit, regPtr, rlResult.lowReg, rlResult.highReg);

        oatFreeTemp(cUnit, regPtr);
        storeValueWide(cUnit, rlDest, rlResult);
    } else {
        rlResult = oatEvalLoc(cUnit, rlDest, regClass, true);

        loadBaseIndexed(cUnit, regPtr, rlIndex.lowReg, rlResult.lowReg,
                        scale, size);

        oatFreeTemp(cUnit, regPtr);
        storeValue(cUnit, rlDest, rlResult);
    }
}

/*
 * Generate array store
 *
 */
STATIC void genArrayPut(CompilationUnit* cUnit, MIR* mir, OpSize size,
                        RegLocation rlArray, RegLocation rlIndex,
                        RegLocation rlSrc, int scale)
{
    RegisterClass regClass = oatRegClassBySize(size);
    int lenOffset = Array::LengthOffset().Int32Value();
    int dataOffset = Array::DataOffset().Int32Value();

    int regPtr;
    rlArray = loadValue(cUnit, rlArray, kCoreReg);
    rlIndex = loadValue(cUnit, rlIndex, kCoreReg);

    if (oatIsTemp(cUnit, rlArray.lowReg)) {
        oatClobber(cUnit, rlArray.lowReg);
        regPtr = rlArray.lowReg;
    } else {
        regPtr = oatAllocTemp(cUnit);
        genRegCopy(cUnit, regPtr, rlArray.lowReg);
    }

    /* null object? */
    genNullCheck(cUnit, rlArray.sRegLow, rlArray.lowReg, mir);

    if (!(mir->optimizationFlags & MIR_IGNORE_RANGE_CHECK)) {
        int regLen = oatAllocTemp(cUnit);
        //NOTE: max live temps(4) here.
        /* Get len */
        loadWordDisp(cUnit, rlArray.lowReg, lenOffset, regLen);
        /* regPtr -> array data */
        opRegImm(cUnit, kOpAdd, regPtr, dataOffset);
        genRegRegCheck(cUnit, kArmCondCs, rlIndex.lowReg, regLen, mir,
                       kArmThrowArrayBounds);
        oatFreeTemp(cUnit, regLen);
    } else {
        /* regPtr -> array data */
        opRegImm(cUnit, kOpAdd, regPtr, dataOffset);
    }
    /* at this point, regPtr points to array, 2 live temps */
    if ((size == kLong) || (size == kDouble)) {
        //TUNING: specific wide routine that can handle fp regs
        if (scale) {
            int rNewIndex = oatAllocTemp(cUnit);
            opRegRegImm(cUnit, kOpLsl, rNewIndex, rlIndex.lowReg, scale);
            opRegReg(cUnit, kOpAdd, regPtr, rNewIndex);
            oatFreeTemp(cUnit, rNewIndex);
        } else {
            opRegReg(cUnit, kOpAdd, regPtr, rlIndex.lowReg);
        }
        rlSrc = loadValueWide(cUnit, rlSrc, regClass);

        storePair(cUnit, regPtr, rlSrc.lowReg, rlSrc.highReg);

        oatFreeTemp(cUnit, regPtr);
    } else {
        rlSrc = loadValue(cUnit, rlSrc, regClass);

        storeBaseIndexed(cUnit, regPtr, rlIndex.lowReg, rlSrc.lowReg,
                         scale, size);
    }
}

STATIC bool genShiftOpLong(CompilationUnit* cUnit, MIR* mir,
                           RegLocation rlDest, RegLocation rlSrc1,
                           RegLocation rlShift)
{
    int funcOffset;

    switch( mir->dalvikInsn.opcode) {
        case OP_SHL_LONG:
        case OP_SHL_LONG_2ADDR:
            funcOffset = OFFSETOF_MEMBER(Thread, pShlLong);
            break;
        case OP_SHR_LONG:
        case OP_SHR_LONG_2ADDR:
            funcOffset = OFFSETOF_MEMBER(Thread, pShrLong);
            break;
        case OP_USHR_LONG:
        case OP_USHR_LONG_2ADDR:
            funcOffset = OFFSETOF_MEMBER(Thread, pUshrLong);
            break;
        default:
            LOG(FATAL) << "Unexpected case";
            return true;
    }
    oatFlushAllRegs(cUnit);   /* Send everything to home location */
    loadWordDisp(cUnit, rSELF, funcOffset, rLR);
    loadValueDirectWideFixed(cUnit, rlSrc1, r0, r1);
    loadValueDirect(cUnit, rlShift, r2);
    callRuntimeHelper(cUnit, rLR);
    RegLocation rlResult = oatGetReturnWide(cUnit);
    storeValueWide(cUnit, rlDest, rlResult);
    return false;
}

STATIC bool genArithOpLong(CompilationUnit* cUnit, MIR* mir,
                           RegLocation rlDest, RegLocation rlSrc1,
                           RegLocation rlSrc2)
{
    RegLocation rlResult;
    OpKind firstOp = kOpBkpt;
    OpKind secondOp = kOpBkpt;
    bool callOut = false;
    bool checkZero = false;
    int funcOffset;
    int retReg = r0;

    switch (mir->dalvikInsn.opcode) {
        case OP_NOT_LONG:
            rlSrc2 = loadValueWide(cUnit, rlSrc2, kCoreReg);
            rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
            // Check for destructive overlap
            if (rlResult.lowReg == rlSrc2.highReg) {
                int tReg = oatAllocTemp(cUnit);
                genRegCopy(cUnit, tReg, rlSrc2.highReg);
                opRegReg(cUnit, kOpMvn, rlResult.lowReg, rlSrc2.lowReg);
                opRegReg(cUnit, kOpMvn, rlResult.highReg, tReg);
                oatFreeTemp(cUnit, tReg);
            } else {
                opRegReg(cUnit, kOpMvn, rlResult.lowReg, rlSrc2.lowReg);
                opRegReg(cUnit, kOpMvn, rlResult.highReg, rlSrc2.highReg);
            }
            storeValueWide(cUnit, rlDest, rlResult);
            return false;
            break;
        case OP_ADD_LONG:
        case OP_ADD_LONG_2ADDR:
            firstOp = kOpAdd;
            secondOp = kOpAdc;
            break;
        case OP_SUB_LONG:
        case OP_SUB_LONG_2ADDR:
            firstOp = kOpSub;
            secondOp = kOpSbc;
            break;
        case OP_MUL_LONG:
        case OP_MUL_LONG_2ADDR:
            callOut = true;
            retReg = r0;
            funcOffset = OFFSETOF_MEMBER(Thread, pLmul);
            break;
        case OP_DIV_LONG:
        case OP_DIV_LONG_2ADDR:
            callOut = true;
            checkZero = true;
            retReg = r0;
            funcOffset = OFFSETOF_MEMBER(Thread, pLdivmod);
            break;
        /* NOTE - result is in r2/r3 instead of r0/r1 */
        case OP_REM_LONG:
        case OP_REM_LONG_2ADDR:
            callOut = true;
            checkZero = true;
            funcOffset = OFFSETOF_MEMBER(Thread, pLdivmod);
            retReg = r2;
            break;
        case OP_AND_LONG_2ADDR:
        case OP_AND_LONG:
            firstOp = kOpAnd;
            secondOp = kOpAnd;
            break;
        case OP_OR_LONG:
        case OP_OR_LONG_2ADDR:
            firstOp = kOpOr;
            secondOp = kOpOr;
            break;
        case OP_XOR_LONG:
        case OP_XOR_LONG_2ADDR:
            firstOp = kOpXor;
            secondOp = kOpXor;
            break;
        case OP_NEG_LONG: {
            rlSrc2 = loadValueWide(cUnit, rlSrc2, kCoreReg);
            rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
            int zReg = oatAllocTemp(cUnit);
            loadConstantNoClobber(cUnit, zReg, 0);
            // Check for destructive overlap
            if (rlResult.lowReg == rlSrc2.highReg) {
                int tReg = oatAllocTemp(cUnit);
                opRegRegReg(cUnit, kOpSub, rlResult.lowReg,
                            zReg, rlSrc2.lowReg);
                opRegRegReg(cUnit, kOpSbc, rlResult.highReg,
                            zReg, tReg);
                oatFreeTemp(cUnit, tReg);
            } else {
                opRegRegReg(cUnit, kOpSub, rlResult.lowReg,
                            zReg, rlSrc2.lowReg);
                opRegRegReg(cUnit, kOpSbc, rlResult.highReg,
                            zReg, rlSrc2.highReg);
            }
            oatFreeTemp(cUnit, zReg);
            storeValueWide(cUnit, rlDest, rlResult);
            return false;
        }
        default:
            LOG(FATAL) << "Invalid long arith op";
    }
    if (!callOut) {
        genLong3Addr(cUnit, mir, firstOp, secondOp, rlDest, rlSrc1, rlSrc2);
    } else {
        oatFlushAllRegs(cUnit);   /* Send everything to home location */
        if (checkZero) {
            loadValueDirectWideFixed(cUnit, rlSrc2, r2, r3);
            loadWordDisp(cUnit, rSELF, funcOffset, rLR);
            loadValueDirectWideFixed(cUnit, rlSrc1, r0, r1);
            int tReg = oatAllocTemp(cUnit);
            newLIR4(cUnit, kThumb2OrrRRRs, tReg, r2, r3, 0);
            oatFreeTemp(cUnit, tReg);
            genCheck(cUnit, kArmCondEq, mir, kArmThrowDivZero);
        } else {
            loadWordDisp(cUnit, rSELF, funcOffset, rLR);
            loadValueDirectWideFixed(cUnit, rlSrc1, r0, r1);
            loadValueDirectWideFixed(cUnit, rlSrc2, r2, r3);
        }
        callRuntimeHelper(cUnit, rLR);
        // Adjust return regs in to handle case of rem returning r2/r3
        if (retReg == r0)
            rlResult = oatGetReturnWide(cUnit);
        else
            rlResult = oatGetReturnWideAlt(cUnit);
        storeValueWide(cUnit, rlDest, rlResult);
    }
    return false;
}

STATIC bool genArithOpInt(CompilationUnit* cUnit, MIR* mir,
                          RegLocation rlDest, RegLocation rlSrc1,
                          RegLocation rlSrc2)
{
    OpKind op = kOpBkpt;
    bool callOut = false;
    bool checkZero = false;
    bool unary = false;
    int retReg = r0;
    int funcOffset;
    RegLocation rlResult;
    bool shiftOp = false;

    switch (mir->dalvikInsn.opcode) {
        case OP_NEG_INT:
            op = kOpNeg;
            unary = true;
            break;
        case OP_NOT_INT:
            op = kOpMvn;
            unary = true;
            break;
        case OP_ADD_INT:
        case OP_ADD_INT_2ADDR:
            op = kOpAdd;
            break;
        case OP_SUB_INT:
        case OP_SUB_INT_2ADDR:
            op = kOpSub;
            break;
        case OP_MUL_INT:
        case OP_MUL_INT_2ADDR:
            op = kOpMul;
            break;
        case OP_DIV_INT:
        case OP_DIV_INT_2ADDR:
            callOut = true;
            checkZero = true;
            funcOffset = OFFSETOF_MEMBER(Thread, pIdiv);
            retReg = r0;
            break;
        /* NOTE: returns in r1 */
        case OP_REM_INT:
        case OP_REM_INT_2ADDR:
            callOut = true;
            checkZero = true;
            funcOffset = OFFSETOF_MEMBER(Thread, pIdivmod);
            retReg = r1;
            break;
        case OP_AND_INT:
        case OP_AND_INT_2ADDR:
            op = kOpAnd;
            break;
        case OP_OR_INT:
        case OP_OR_INT_2ADDR:
            op = kOpOr;
            break;
        case OP_XOR_INT:
        case OP_XOR_INT_2ADDR:
            op = kOpXor;
            break;
        case OP_SHL_INT:
        case OP_SHL_INT_2ADDR:
            shiftOp = true;
            op = kOpLsl;
            break;
        case OP_SHR_INT:
        case OP_SHR_INT_2ADDR:
            shiftOp = true;
            op = kOpAsr;
            break;
        case OP_USHR_INT:
        case OP_USHR_INT_2ADDR:
            shiftOp = true;
            op = kOpLsr;
            break;
        default:
            LOG(FATAL) << "Invalid word arith op: " <<
                (int)mir->dalvikInsn.opcode;
    }
    if (!callOut) {
        rlSrc1 = loadValue(cUnit, rlSrc1, kCoreReg);
        if (unary) {
            rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
            opRegReg(cUnit, op, rlResult.lowReg,
                     rlSrc1.lowReg);
        } else {
            rlSrc2 = loadValue(cUnit, rlSrc2, kCoreReg);
            if (shiftOp) {
                int tReg = oatAllocTemp(cUnit);
                opRegRegImm(cUnit, kOpAnd, tReg, rlSrc2.lowReg, 31);
                rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
                opRegRegReg(cUnit, op, rlResult.lowReg,
                            rlSrc1.lowReg, tReg);
                oatFreeTemp(cUnit, tReg);
            } else {
                rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
                opRegRegReg(cUnit, op, rlResult.lowReg,
                            rlSrc1.lowReg, rlSrc2.lowReg);
            }
        }
        storeValue(cUnit, rlDest, rlResult);
    } else {
        RegLocation rlResult;
        oatFlushAllRegs(cUnit);   /* Send everything to home location */
        loadValueDirectFixed(cUnit, rlSrc2, r1);
        loadWordDisp(cUnit, rSELF, funcOffset, rLR);
        loadValueDirectFixed(cUnit, rlSrc1, r0);
        if (checkZero) {
            genImmedCheck(cUnit, kArmCondEq, r1, 0, mir, kArmThrowDivZero);
        }
        callRuntimeHelper(cUnit, rLR);
        if (retReg == r0)
            rlResult = oatGetReturn(cUnit);
        else
            rlResult = oatGetReturnAlt(cUnit);
        storeValue(cUnit, rlDest, rlResult);
    }
    return false;
}

/* Check if we need to check for pending suspend request */
STATIC void genSuspendTest(CompilationUnit* cUnit, MIR* mir)
{
    if (NO_SUSPEND || mir->optimizationFlags & MIR_IGNORE_SUSPEND_CHECK) {
        return;
    }
    oatFlushAllRegs(cUnit);
    newLIR2(cUnit, kThumbSubRI8, rSUSPEND, 1);
    ArmLIR* branch = opCondBranch(cUnit, kArmCondEq);
    ArmLIR* retLab = newLIR0(cUnit, kArmPseudoTargetLabel);
    retLab->defMask = ENCODE_ALL;
    ArmLIR* target = (ArmLIR*)oatNew(sizeof(ArmLIR), true);
    target->generic.dalvikOffset = cUnit->currentDalvikOffset;
    target->opcode = kArmPseudoSuspendTarget;
    target->operands[0] = (intptr_t)retLab;
    target->operands[1] = mir->offset;
    branch->generic.target = (LIR*)target;
    oatInsertGrowableList(&cUnit->suspendLaunchpads, (intptr_t)target);
}

/*
 * The following are the first-level codegen routines that analyze the format
 * of each bytecode then either dispatch special purpose codegen routines
 * or produce corresponding Thumb instructions directly.
 */

STATIC bool isPowerOfTwo(int x)
{
    return (x & (x - 1)) == 0;
}

// Returns true if no more than two bits are set in 'x'.
STATIC bool isPopCountLE2(unsigned int x)
{
    x &= x - 1;
    return (x & (x - 1)) == 0;
}

// Returns the index of the lowest set bit in 'x'.
STATIC int lowestSetBit(unsigned int x) {
    int bit_posn = 0;
    while ((x & 0xf) == 0) {
        bit_posn += 4;
        x >>= 4;
    }
    while ((x & 1) == 0) {
        bit_posn++;
        x >>= 1;
    }
    return bit_posn;
}

// Returns true if it added instructions to 'cUnit' to divide 'rlSrc' by 'lit'
// and store the result in 'rlDest'.
STATIC bool handleEasyDivide(CompilationUnit* cUnit, Opcode dalvikOpcode,
                             RegLocation rlSrc, RegLocation rlDest, int lit)
{
    if (lit < 2 || !isPowerOfTwo(lit)) {
        return false;
    }
    int k = lowestSetBit(lit);
    if (k >= 30) {
        // Avoid special cases.
        return false;
    }
    bool div = (dalvikOpcode == OP_DIV_INT_LIT8 ||
                dalvikOpcode == OP_DIV_INT_LIT16);
    rlSrc = loadValue(cUnit, rlSrc, kCoreReg);
    RegLocation rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
    if (div) {
        int tReg = oatAllocTemp(cUnit);
        if (lit == 2) {
            // Division by 2 is by far the most common division by constant.
            opRegRegImm(cUnit, kOpLsr, tReg, rlSrc.lowReg, 32 - k);
            opRegRegReg(cUnit, kOpAdd, tReg, tReg, rlSrc.lowReg);
            opRegRegImm(cUnit, kOpAsr, rlResult.lowReg, tReg, k);
        } else {
            opRegRegImm(cUnit, kOpAsr, tReg, rlSrc.lowReg, 31);
            opRegRegImm(cUnit, kOpLsr, tReg, tReg, 32 - k);
            opRegRegReg(cUnit, kOpAdd, tReg, tReg, rlSrc.lowReg);
            opRegRegImm(cUnit, kOpAsr, rlResult.lowReg, tReg, k);
        }
    } else {
        int cReg = oatAllocTemp(cUnit);
        loadConstant(cUnit, cReg, lit - 1);
        int tReg1 = oatAllocTemp(cUnit);
        int tReg2 = oatAllocTemp(cUnit);
        if (lit == 2) {
            opRegRegImm(cUnit, kOpLsr, tReg1, rlSrc.lowReg, 32 - k);
            opRegRegReg(cUnit, kOpAdd, tReg2, tReg1, rlSrc.lowReg);
            opRegRegReg(cUnit, kOpAnd, tReg2, tReg2, cReg);
            opRegRegReg(cUnit, kOpSub, rlResult.lowReg, tReg2, tReg1);
        } else {
            opRegRegImm(cUnit, kOpAsr, tReg1, rlSrc.lowReg, 31);
            opRegRegImm(cUnit, kOpLsr, tReg1, tReg1, 32 - k);
            opRegRegReg(cUnit, kOpAdd, tReg2, tReg1, rlSrc.lowReg);
            opRegRegReg(cUnit, kOpAnd, tReg2, tReg2, cReg);
            opRegRegReg(cUnit, kOpSub, rlResult.lowReg, tReg2, tReg1);
        }
    }
    storeValue(cUnit, rlDest, rlResult);
    return true;
}

// Returns true if it added instructions to 'cUnit' to multiply 'rlSrc' by 'lit'
// and store the result in 'rlDest'.
STATIC bool handleEasyMultiply(CompilationUnit* cUnit,
                               RegLocation rlSrc, RegLocation rlDest, int lit)
{
    // Can we simplify this multiplication?
    bool powerOfTwo = false;
    bool popCountLE2 = false;
    bool powerOfTwoMinusOne = false;
    if (lit < 2) {
        // Avoid special cases.
        return false;
    } else if (isPowerOfTwo(lit)) {
        powerOfTwo = true;
    } else if (isPopCountLE2(lit)) {
        popCountLE2 = true;
    } else if (isPowerOfTwo(lit + 1)) {
        powerOfTwoMinusOne = true;
    } else {
        return false;
    }
    rlSrc = loadValue(cUnit, rlSrc, kCoreReg);
    RegLocation rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
    if (powerOfTwo) {
        // Shift.
        opRegRegImm(cUnit, kOpLsl, rlResult.lowReg, rlSrc.lowReg,
                    lowestSetBit(lit));
    } else if (popCountLE2) {
        // Shift and add and shift.
        int firstBit = lowestSetBit(lit);
        int secondBit = lowestSetBit(lit ^ (1 << firstBit));
        genMultiplyByTwoBitMultiplier(cUnit, rlSrc, rlResult, lit,
                                      firstBit, secondBit);
    } else {
        // Reverse subtract: (src << (shift + 1)) - src.
        DCHECK(powerOfTwoMinusOne);
        // TUNING: rsb dst, src, src lsl#lowestSetBit(lit + 1)
        int tReg = oatAllocTemp(cUnit);
        opRegRegImm(cUnit, kOpLsl, tReg, rlSrc.lowReg, lowestSetBit(lit + 1));
        opRegRegReg(cUnit, kOpSub, rlResult.lowReg, tReg, rlSrc.lowReg);
    }
    storeValue(cUnit, rlDest, rlResult);
    return true;
}

STATIC bool genArithOpIntLit(CompilationUnit* cUnit, MIR* mir,
                             RegLocation rlDest, RegLocation rlSrc,
                             int lit)
{
    Opcode dalvikOpcode = mir->dalvikInsn.opcode;
    RegLocation rlResult;
    OpKind op = (OpKind)0;      /* Make gcc happy */
    int shiftOp = false;
    bool isDiv = false;
    int funcOffset;

    switch (dalvikOpcode) {
        case OP_RSUB_INT_LIT8:
        case OP_RSUB_INT: {
            int tReg;
            //TUNING: add support for use of Arm rsub op
            rlSrc = loadValue(cUnit, rlSrc, kCoreReg);
            tReg = oatAllocTemp(cUnit);
            loadConstant(cUnit, tReg, lit);
            rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
            opRegRegReg(cUnit, kOpSub, rlResult.lowReg,
                        tReg, rlSrc.lowReg);
            storeValue(cUnit, rlDest, rlResult);
            return false;
            break;
        }

        case OP_ADD_INT_LIT8:
        case OP_ADD_INT_LIT16:
            op = kOpAdd;
            break;
        case OP_MUL_INT_LIT8:
        case OP_MUL_INT_LIT16: {
            if (handleEasyMultiply(cUnit, rlSrc, rlDest, lit)) {
                return false;
            }
            op = kOpMul;
            break;
        }
        case OP_AND_INT_LIT8:
        case OP_AND_INT_LIT16:
            op = kOpAnd;
            break;
        case OP_OR_INT_LIT8:
        case OP_OR_INT_LIT16:
            op = kOpOr;
            break;
        case OP_XOR_INT_LIT8:
        case OP_XOR_INT_LIT16:
            op = kOpXor;
            break;
        case OP_SHL_INT_LIT8:
            lit &= 31;
            shiftOp = true;
            op = kOpLsl;
            break;
        case OP_SHR_INT_LIT8:
            lit &= 31;
            shiftOp = true;
            op = kOpAsr;
            break;
        case OP_USHR_INT_LIT8:
            lit &= 31;
            shiftOp = true;
            op = kOpLsr;
            break;

        case OP_DIV_INT_LIT8:
        case OP_DIV_INT_LIT16:
        case OP_REM_INT_LIT8:
        case OP_REM_INT_LIT16:
            if (lit == 0) {
                genImmedCheck(cUnit, kArmCondAl, 0, 0, mir, kArmThrowDivZero);
                return false;
            }
            if (handleEasyDivide(cUnit, dalvikOpcode, rlSrc, rlDest, lit)) {
                return false;
            }
            oatFlushAllRegs(cUnit);   /* Everything to home location */
            loadValueDirectFixed(cUnit, rlSrc, r0);
            oatClobber(cUnit, r0);
            if ((dalvikOpcode == OP_DIV_INT_LIT8) ||
                (dalvikOpcode == OP_DIV_INT_LIT16)) {
                funcOffset = OFFSETOF_MEMBER(Thread, pIdiv);
                isDiv = true;
            } else {
                funcOffset = OFFSETOF_MEMBER(Thread, pIdivmod);
                isDiv = false;
            }
            loadWordDisp(cUnit, rSELF, funcOffset, rLR);
            loadConstant(cUnit, r1, lit);
            callRuntimeHelper(cUnit, rLR);
            if (isDiv)
                rlResult = oatGetReturn(cUnit);
            else
                rlResult = oatGetReturnAlt(cUnit);
            storeValue(cUnit, rlDest, rlResult);
            return false;
            break;
        default:
            return true;
    }
    rlSrc = loadValue(cUnit, rlSrc, kCoreReg);
    rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
    // Avoid shifts by literal 0 - no support in Thumb.  Change to copy
    if (shiftOp && (lit == 0)) {
        genRegCopy(cUnit, rlResult.lowReg, rlSrc.lowReg);
    } else {
        opRegRegImm(cUnit, op, rlResult.lowReg, rlSrc.lowReg, lit);
    }
    storeValue(cUnit, rlDest, rlResult);
    return false;
}

/* Architectural-specific debugging helpers go here */
void oatArchDump(void)
{
    /* Print compiled opcode in this VM instance */
    int i, start, streak;
    std::string buf;

    streak = i = 0;
    while (opcodeCoverage[i] == 0 && i < kNumPackedOpcodes) {
        i++;
    }
    if (i == kNumPackedOpcodes) {
        return;
    }
    for (start = i++, streak = 1; i < kNumPackedOpcodes; i++) {
        if (opcodeCoverage[i]) {
            streak++;
        } else {
            if (streak == 1) {
                StringAppendF(&buf, "%x,", start);
            } else {
                StringAppendF(&buf, "%x-%x,", start, start + streak - 1);
            }
            streak = 0;
            while (opcodeCoverage[i] == 0 && i < kNumPackedOpcodes) {
                i++;
            }
            if (i < kNumPackedOpcodes) {
                streak = 1;
                start = i;
            }
        }
    }
    if (streak) {
        if (streak == 1) {
            StringAppendF(&buf, "%x", start);
        } else {
            StringAppendF(&buf, "%x-%x", start, start + streak - 1);
        }
    }
    if (!buf.empty()) {
        LOG(INFO) << "dalvik.vm.oat.op = " << buf;
    }
}
