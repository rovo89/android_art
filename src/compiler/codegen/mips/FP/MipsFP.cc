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

namespace art {

bool genArithOpFloat(CompilationUnit *cUnit, MIR *mir, RegLocation rlDest,
                     RegLocation rlSrc1, RegLocation rlSrc2)
{
#ifdef __mips_hard_float
    int op = kMipsNop;
    RegLocation rlResult;

    /*
     * Don't attempt to optimize register usage since these opcodes call out to
     * the handlers.
     */
    switch (mir->dalvikInsn.opcode) {
        case Instruction::ADD_FLOAT_2ADDR:
        case Instruction::ADD_FLOAT:
            op = kMipsFadds;
            break;
        case Instruction::SUB_FLOAT_2ADDR:
        case Instruction::SUB_FLOAT:
            op = kMipsFsubs;
            break;
        case Instruction::DIV_FLOAT_2ADDR:
        case Instruction::DIV_FLOAT:
            op = kMipsFdivs;
            break;
        case Instruction::MUL_FLOAT_2ADDR:
        case Instruction::MUL_FLOAT:
            op = kMipsFmuls;
            break;
        case Instruction::REM_FLOAT_2ADDR:
        case Instruction::REM_FLOAT:
        case Instruction::NEG_FLOAT: {
            return genArithOpFloatPortable(cUnit, mir, rlDest, rlSrc1, rlSrc2);
        }
        default:
            return true;
    }
    rlSrc1 = loadValue(cUnit, rlSrc1, kFPReg);
    rlSrc2 = loadValue(cUnit, rlSrc2, kFPReg);
    rlResult = oatEvalLoc(cUnit, rlDest, kFPReg, true);
    newLIR3(cUnit, (MipsOpCode)op, rlResult.lowReg, rlSrc1.lowReg,
                    rlSrc2.lowReg);
    storeValue(cUnit, rlDest, rlResult);

    return false;
#else
    return genArithOpFloatPortable(cUnit, mir, rlDest, rlSrc1, rlSrc2);
#endif
}

static bool genArithOpDouble(CompilationUnit *cUnit, MIR *mir,
                             RegLocation rlDest, RegLocation rlSrc1,
                             RegLocation rlSrc2)
{
#ifdef __mips_hard_float
    int op = kMipsNop;
    RegLocation rlResult;

    switch (mir->dalvikInsn.opcode) {
        case Instruction::ADD_DOUBLE_2ADDR:
        case Instruction::ADD_DOUBLE:
            op = kMipsFaddd;
            break;
        case Instruction::SUB_DOUBLE_2ADDR:
        case Instruction::SUB_DOUBLE:
            op = kMipsFsubd;
            break;
        case Instruction::DIV_DOUBLE_2ADDR:
        case Instruction::DIV_DOUBLE:
            op = kMipsFdivd;
            break;
        case Instruction::MUL_DOUBLE_2ADDR:
        case Instruction::MUL_DOUBLE:
            op = kMipsFmuld;
            break;
        case Instruction::REM_DOUBLE_2ADDR:
        case Instruction::REM_DOUBLE:
        case Instruction::NEG_DOUBLE: {
            return genArithOpDoublePortable(cUnit, mir, rlDest, rlSrc1, rlSrc2);
        }
        default:
            return true;
    }
    rlSrc1 = loadValueWide(cUnit, rlSrc1, kFPReg);
    DCHECK(rlSrc1.wide);
    rlSrc2 = loadValueWide(cUnit, rlSrc2, kFPReg);
    DCHECK(rlSrc2.wide);
    rlResult = oatEvalLoc(cUnit, rlDest, kFPReg, true);
    DCHECK(rlDest.wide);
    DCHECK(rlResult.wide);
    newLIR3(cUnit, (MipsOpCode)op, S2D(rlResult.lowReg, rlResult.highReg),
            S2D(rlSrc1.lowReg, rlSrc1.highReg),
            S2D(rlSrc2.lowReg, rlSrc2.highReg));
    storeValueWide(cUnit, rlDest, rlResult);
    return false;
#else
    return genArithOpDoublePortable(cUnit, mir, rlDest, rlSrc1, rlSrc2);
#endif
}

static bool genConversion(CompilationUnit *cUnit, MIR *mir)
{
#ifdef __mips_hard_float
    Instruction::Code opcode = mir->dalvikInsn.opcode;
    bool longSrc = false;
    bool longDest = false;
    RegLocation rlSrc;
    RegLocation rlDest;
    int op = kMipsNop;
    int srcReg;
    RegLocation rlResult;
    switch (opcode) {
        case Instruction::INT_TO_FLOAT:
            longSrc = false;
            longDest = false;
            op = kMipsFcvtsw;
            break;
        case Instruction::DOUBLE_TO_FLOAT:
            longSrc = true;
            longDest = false;
            op = kMipsFcvtsd;
            break;
        case Instruction::FLOAT_TO_DOUBLE:
            longSrc = false;
            longDest = true;
            op = kMipsFcvtds;
            break;
        case Instruction::INT_TO_DOUBLE:
            longSrc = false;
            longDest = true;
            op = kMipsFcvtdw;
            break;
        case Instruction::FLOAT_TO_INT:
        case Instruction::DOUBLE_TO_INT:
        case Instruction::LONG_TO_DOUBLE:
        case Instruction::FLOAT_TO_LONG:
        case Instruction::LONG_TO_FLOAT:
        case Instruction::DOUBLE_TO_LONG:
            return genConversionPortable(cUnit, mir);
        default:
            return true;
    }
    if (longSrc) {
        rlSrc = oatGetSrcWide(cUnit, mir, 0, 1);
        rlSrc = loadValueWide(cUnit, rlSrc, kFPReg);
        srcReg = S2D(rlSrc.lowReg, rlSrc.highReg);
    } else {
        rlSrc = oatGetSrc(cUnit, mir, 0);
        rlSrc = loadValue(cUnit, rlSrc, kFPReg);
        srcReg = rlSrc.lowReg;
    }
    if (longDest) {
        rlDest = oatGetDestWide(cUnit, mir, 0, 1);
        rlResult = oatEvalLoc(cUnit, rlDest, kFPReg, true);
        newLIR2(cUnit, (MipsOpCode)op, S2D(rlResult.lowReg, rlResult.highReg), srcReg);
        storeValueWide(cUnit, rlDest, rlResult);
    } else {
        rlDest = oatGetDest(cUnit, mir, 0);
        rlResult = oatEvalLoc(cUnit, rlDest, kFPReg, true);
        newLIR2(cUnit, (MipsOpCode)op, rlResult.lowReg, srcReg);
        storeValue(cUnit, rlDest, rlResult);
    }
    return false;
#else
    return genConversionPortable(cUnit, mir);
#endif
}

static bool genCmpFP(CompilationUnit *cUnit, MIR *mir, RegLocation rlDest,
                     RegLocation rlSrc1, RegLocation rlSrc2)
{
    bool wide = true;
    int offset;

    switch (mir->dalvikInsn.opcode) {
        case Instruction::CMPL_FLOAT:
            offset = OFFSETOF_MEMBER(Thread, pCmplFloat);
            wide = false;
            break;
        case Instruction::CMPG_FLOAT:
            offset = OFFSETOF_MEMBER(Thread, pCmpgFloat);
            wide = false;
            break;
        case Instruction::CMPL_DOUBLE:
            offset = OFFSETOF_MEMBER(Thread, pCmplDouble);
            break;
        case Instruction::CMPG_DOUBLE:
            offset = OFFSETOF_MEMBER(Thread, pCmpgDouble);
            break;
        default:
            return true;
    }
    oatFlushAllRegs(cUnit);
    oatLockCallTemps(cUnit);
    if (wide) {
        loadValueDirectWideFixed(cUnit, rlSrc1, rARG0, rARG1);
        loadValueDirectWideFixed(cUnit, rlSrc2, rARG2, rARG3);
    } else {
        loadValueDirectFixed(cUnit, rlSrc1, rARG0);
        loadValueDirectFixed(cUnit, rlSrc2, rARG1);
    }
    int rTgt = loadHelper(cUnit, offset);
    opReg(cUnit, kOpBlx, rTgt);
    RegLocation rlResult = oatGetReturn(cUnit, false);
    storeValue(cUnit, rlDest, rlResult);
    return false;
}

} //  namespace art
