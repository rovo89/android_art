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

STATIC bool genArithOpFloat(CompilationUnit* cUnit, MIR* mir,
                            RegLocation rlDest, RegLocation rlSrc1,
                            RegLocation rlSrc2)
{
    int op = kThumbBkpt;
    RegLocation rlResult;

    /*
     * Don't attempt to optimize register usage since these opcodes call out to
     * the handlers.
     */
    switch (mir->dalvikInsn.opcode) {
        case OP_ADD_FLOAT_2ADDR:
        case OP_ADD_FLOAT:
            op = kThumb2Vadds;
            break;
        case OP_SUB_FLOAT_2ADDR:
        case OP_SUB_FLOAT:
            op = kThumb2Vsubs;
            break;
        case OP_DIV_FLOAT_2ADDR:
        case OP_DIV_FLOAT:
            op = kThumb2Vdivs;
            break;
        case OP_MUL_FLOAT_2ADDR:
        case OP_MUL_FLOAT:
            op = kThumb2Vmuls;
            break;
        case OP_REM_FLOAT_2ADDR:
        case OP_REM_FLOAT:
        case OP_NEG_FLOAT: {
            return genArithOpFloatPortable(cUnit, mir, rlDest, rlSrc1,
                                              rlSrc2);
        }
        default:
            return true;
    }
    rlSrc1 = loadValue(cUnit, rlSrc1, kFPReg);
    rlSrc2 = loadValue(cUnit, rlSrc2, kFPReg);
    rlResult = oatEvalLoc(cUnit, rlDest, kFPReg, true);
    newLIR3(cUnit, (ArmOpcode)op, rlResult.lowReg, rlSrc1.lowReg,
            rlSrc2.lowReg);
    storeValue(cUnit, rlDest, rlResult);
    return false;
}

STATIC bool genArithOpDouble(CompilationUnit* cUnit, MIR* mir,
                             RegLocation rlDest, RegLocation rlSrc1,
                             RegLocation rlSrc2)
{
    int op = kThumbBkpt;
    RegLocation rlResult;

    switch (mir->dalvikInsn.opcode) {
        case OP_ADD_DOUBLE_2ADDR:
        case OP_ADD_DOUBLE:
            op = kThumb2Vaddd;
            break;
        case OP_SUB_DOUBLE_2ADDR:
        case OP_SUB_DOUBLE:
            op = kThumb2Vsubd;
            break;
        case OP_DIV_DOUBLE_2ADDR:
        case OP_DIV_DOUBLE:
            op = kThumb2Vdivd;
            break;
        case OP_MUL_DOUBLE_2ADDR:
        case OP_MUL_DOUBLE:
            op = kThumb2Vmuld;
            break;
        case OP_REM_DOUBLE_2ADDR:
        case OP_REM_DOUBLE:
        case OP_NEG_DOUBLE: {
            return genArithOpDoublePortable(cUnit, mir, rlDest, rlSrc1,
                                               rlSrc2);
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
    newLIR3(cUnit, (ArmOpcode)op, S2D(rlResult.lowReg, rlResult.highReg),
            S2D(rlSrc1.lowReg, rlSrc1.highReg),
            S2D(rlSrc2.lowReg, rlSrc2.highReg));
    storeValueWide(cUnit, rlDest, rlResult);
    return false;
}

STATIC bool genConversion(CompilationUnit* cUnit, MIR* mir)
{
    Opcode opcode = mir->dalvikInsn.opcode;
    int op = kThumbBkpt;
    bool longSrc = false;
    bool longDest = false;
    int srcReg;
    RegLocation rlSrc;
    RegLocation rlDest;
    RegLocation rlResult;

    switch (opcode) {
        case OP_INT_TO_FLOAT:
            longSrc = false;
            longDest = false;
            op = kThumb2VcvtIF;
            break;
        case OP_FLOAT_TO_INT:
            longSrc = false;
            longDest = false;
            op = kThumb2VcvtFI;
            break;
        case OP_DOUBLE_TO_FLOAT:
            longSrc = true;
            longDest = false;
            op = kThumb2VcvtDF;
            break;
        case OP_FLOAT_TO_DOUBLE:
            longSrc = false;
            longDest = true;
            op = kThumb2VcvtFd;
            break;
        case OP_INT_TO_DOUBLE:
            longSrc = false;
            longDest = true;
            op = kThumb2VcvtID;
            break;
        case OP_DOUBLE_TO_INT:
            longSrc = true;
            longDest = false;
            op = kThumb2VcvtDI;
            break;
        case OP_LONG_TO_DOUBLE:
        case OP_FLOAT_TO_LONG:
        case OP_LONG_TO_FLOAT:
        case OP_DOUBLE_TO_LONG:
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
        newLIR2(cUnit, (ArmOpcode)op, S2D(rlResult.lowReg, rlResult.highReg),
                srcReg);
        storeValueWide(cUnit, rlDest, rlResult);
    } else {
        rlDest = oatGetDest(cUnit, mir, 0);
        rlResult = oatEvalLoc(cUnit, rlDest, kFPReg, true);
        newLIR2(cUnit, (ArmOpcode)op, rlResult.lowReg, srcReg);
        storeValue(cUnit, rlDest, rlResult);
    }
    return false;
}

STATIC bool genCmpFP(CompilationUnit* cUnit, MIR* mir, RegLocation rlDest,
                     RegLocation rlSrc1, RegLocation rlSrc2)
{
    bool isDouble;
    int defaultResult;
    RegLocation rlResult;

    switch(mir->dalvikInsn.opcode) {
        case OP_CMPL_FLOAT:
            isDouble = false;
            defaultResult = -1;
            break;
        case OP_CMPG_FLOAT:
            isDouble = false;
            defaultResult = 1;
            break;
        case OP_CMPL_DOUBLE:
            isDouble = true;
            defaultResult = -1;
            break;
        case OP_CMPG_DOUBLE:
            isDouble = true;
            defaultResult = 1;
            break;
        default:
            return true;
    }
    if (isDouble) {
        rlSrc1 = loadValueWide(cUnit, rlSrc1, kFPReg);
        rlSrc2 = loadValueWide(cUnit, rlSrc2, kFPReg);
        oatClobberSReg(cUnit, rlDest.sRegLow);
        rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
        loadConstant(cUnit, rlResult.lowReg, defaultResult);
        newLIR2(cUnit, kThumb2Vcmpd, S2D(rlSrc1.lowReg, r1Src2.highReg),
                S2D(rlSrc2.lowReg, rlSrc2.highReg));
    } else {
        rlSrc1 = loadValue(cUnit, rlSrc1, kFPReg);
        rlSrc2 = loadValue(cUnit, rlSrc2, kFPReg);
        oatClobberSReg(cUnit, rlDest.sRegLow);
        rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
        loadConstant(cUnit, rlResult.lowReg, defaultResult);
        newLIR2(cUnit, kThumb2Vcmps, rlSrc1.lowReg, rlSrc2.lowReg);
    }
    DCHECK(!FPREG(rlResult.lowReg));
    newLIR0(cUnit, kThumb2Fmstat);

    genIT(cUnit, (defaultResult == -1) ? kArmCondGt : kArmCondMi, "");
    newLIR2(cUnit, kThumb2MovImmShift, rlResult.lowReg,
            modifiedImmediate(-defaultResult)); // Must not alter ccodes
    genBarrier(cUnit);

    genIT(cUnit, kArmCondEq, "");
    loadConstant(cUnit, rlResult.lowReg, 0);
    genBarrier(cUnit);

    storeValue(cUnit, rlDest, rlResult);
    return false;
}
