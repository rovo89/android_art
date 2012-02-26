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

namespace art {

STATIC void pushWord(std::vector<uint16_t>&buf, int data) {
    buf.push_back( data & 0xffff);
    buf.push_back( (data >> 16) & 0xffff);
}

void alignBuffer(std::vector<uint16_t>&buf, size_t offset) {
    while (buf.size() < (offset/2))
        buf.push_back(0);
}

/* Write the literal pool to the output stream */
STATIC void installLiteralPools(CompilationUnit* cUnit)
{
    alignBuffer(cUnit->codeBuffer, cUnit->dataOffset);
    TGT_LIR* dataLIR = (TGT_LIR*) cUnit->literalList;
    while (dataLIR != NULL) {
        pushWord(cUnit->codeBuffer, dataLIR->operands[0]);
        dataLIR = NEXT_LIR(dataLIR);
    }
}

/* Write the switch tables to the output stream */
STATIC void installSwitchTables(CompilationUnit* cUnit)
{
    GrowableListIterator iterator;
    oatGrowableListIteratorInit(&cUnit->switchTables, &iterator);
    while (true) {
        SwitchTable* tabRec = (SwitchTable *) oatGrowableListIteratorNext(
             &iterator);
        if (tabRec == NULL) break;
        alignBuffer(cUnit->codeBuffer, tabRec->offset);
        int bxOffset = tabRec->bxInst->generic.offset + 4;
        if (cUnit->printMe) {
            LOG(INFO) << "Switch table for offset 0x" << std::hex << bxOffset;
        }
        if (tabRec->table[0] == kSparseSwitchSignature) {
            int* keys = (int*)&(tabRec->table[2]);
            for (int elems = 0; elems < tabRec->table[1]; elems++) {
                int disp = tabRec->targets[elems]->generic.offset - bxOffset;
                if (cUnit->printMe) {
                    LOG(INFO) << "    Case[" << elems << "] key: 0x" <<
                        std::hex << keys[elems] << ", disp: 0x" <<
                        std::hex << disp;
                }
                pushWord(cUnit->codeBuffer, keys[elems]);
                pushWord(cUnit->codeBuffer,
                    tabRec->targets[elems]->generic.offset - bxOffset);
            }
        } else {
            DCHECK_EQ(tabRec->table[0], kPackedSwitchSignature);
            for (int elems = 0; elems < tabRec->table[1]; elems++) {
                int disp = tabRec->targets[elems]->generic.offset - bxOffset;
                if (cUnit->printMe) {
                    LOG(INFO) << "    Case[" << elems << "] disp: 0x" <<
                        std::hex << disp;
                }
                pushWord(cUnit->codeBuffer,
                         tabRec->targets[elems]->generic.offset - bxOffset);
            }
        }
    }
}

/* Write the fill array dta to the output stream */
STATIC void installFillArrayData(CompilationUnit* cUnit)
{
    GrowableListIterator iterator;
    oatGrowableListIteratorInit(&cUnit->fillArrayData, &iterator);
    while (true) {
        FillArrayData *tabRec = (FillArrayData *) oatGrowableListIteratorNext(
             &iterator);
        if (tabRec == NULL) break;
        alignBuffer(cUnit->codeBuffer, tabRec->offset);
        for (int i = 0; i < ((tabRec->size + 1) / 2) ; i++) {
            cUnit->codeBuffer.push_back( tabRec->table[i]);
        }
    }
}

STATIC int assignLiteralOffsetCommon(LIR* lir, int offset)
{
    for (;lir != NULL; lir = lir->next) {
        lir->offset = offset;
        offset += 4;
    }
    return offset;
}

STATIC void createMappingTable(CompilationUnit* cUnit)
{
    TGT_LIR* tgtLIR;
    int currentDalvikOffset = -1;

    for (tgtLIR = (TGT_LIR *) cUnit->firstLIRInsn;
         tgtLIR;
         tgtLIR = NEXT_LIR(tgtLIR)) {
        if ((tgtLIR->opcode >= 0) && !tgtLIR->flags.isNop &&
            (currentDalvikOffset != tgtLIR->generic.dalvikOffset)) {
            // Changed - need to emit a record
            cUnit->mappingTable.push_back(tgtLIR->generic.offset);
            cUnit->mappingTable.push_back(tgtLIR->generic.dalvikOffset);
            currentDalvikOffset = tgtLIR->generic.dalvikOffset;
        }
    }
}

/* Determine the offset of each literal field */
STATIC int assignLiteralOffset(CompilationUnit* cUnit, int offset)
{
    offset = assignLiteralOffsetCommon(cUnit->literalList, offset);
    return offset;
}

STATIC int assignSwitchTablesOffset(CompilationUnit* cUnit, int offset)
{
    GrowableListIterator iterator;
    oatGrowableListIteratorInit(&cUnit->switchTables, &iterator);
    while (true) {
        SwitchTable *tabRec = (SwitchTable *) oatGrowableListIteratorNext(
             &iterator);
        if (tabRec == NULL) break;
        tabRec->offset = offset;
        if (tabRec->table[0] == kSparseSwitchSignature) {
            offset += tabRec->table[1] * (sizeof(int) * 2);
        } else {
            DCHECK_EQ(tabRec->table[0], kPackedSwitchSignature);
            offset += tabRec->table[1] * sizeof(int);
        }
    }
    return offset;
}

STATIC int assignFillArrayDataOffset(CompilationUnit* cUnit, int offset)
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

    /*
     * Create the mapping table
     */
    createMappingTable(cUnit);
}



}  // namespace art
