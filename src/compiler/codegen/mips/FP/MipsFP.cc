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

extern void oatFlushRegWideForV5TEVFP(CompilationUnit *cUnit,
                                              int reg1, int reg2);
extern void oatFlushRegForV5TEVFP(CompilationUnit *cUnit, int reg);

/* First, flush any registers associated with this value */
void loadValueAddress(CompilationUnit *cUnit, RegLocation rlSrc,
                             int rDest)
{
    UNIMPLEMENTED(FATAL) << "Need Mips implementation";
#if 0
     rlSrc = rlSrc.wide ? oatUpdateLocWide(cUnit, rlSrc) :
                          oatUpdateLoc(cUnit, rlSrc);
     if (rlSrc.location == kLocPhysReg) {
         if (rlSrc.wide) {
             oatFlushRegWideForV5TEVFP(cUnit, rlSrc.lowReg,
                                               rlSrc.highReg);
         } else {
             oatFlushRegForV5TEVFP(cUnit, rlSrc.lowReg);
         }
     }
     opRegRegImm(cUnit, kOpAdd, rDest, rFP,
                 oatS2VReg(cUnit, rlSrc.sRegLow) << 2);
#endif
}

/*
 * TUNING: On some implementations, it is quicker to pass addresses
 * to the handlers rather than load the operands into core registers
 * and then move the values to FP regs in the handlers.  Other implementations
 * may prefer passing data in registers (and the latter approach would
 * yeild cleaner register handling - avoiding the requirement that operands
 * be flushed to memory prior to the call).
 */
static bool genArithOpFloat(CompilationUnit *cUnit, MIR *mir,
                            RegLocation rlDest, RegLocation rlSrc1,
                            RegLocation rlSrc2)
{
#ifdef __mips_hard_float
    int op = kMipsNop;
    RegLocation rlResult;

    /*
     * Don't attempt to optimize register usage since these opcodes call out to
     * the handlers.
     */
    switch (mir->dalvikInsn.opcode) {
        case OP_ADD_FLOAT_2ADDR:
        case OP_ADD_FLOAT:
            op = kMipsFadds;
            break;
        case OP_SUB_FLOAT_2ADDR:
        case OP_SUB_FLOAT:
            op = kMipsFsubs;
            break;
        case OP_DIV_FLOAT_2ADDR:
        case OP_DIV_FLOAT:
            op = kMipsFdivs;
            break;
        case OP_MUL_FLOAT_2ADDR:
        case OP_MUL_FLOAT:
            op = kMipsFmuls;
            break;
        case OP_REM_FLOAT_2ADDR:
        case OP_REM_FLOAT:
        case OP_NEG_FLOAT: {
            return genArithOpFloatPortable(cUnit, mir, rlDest, rlSrc1, rlSrc2);
        }
        default:
            return true;
    }
    rlSrc1 = loadValue(cUnit, rlSrc1, kFPReg);
    rlSrc2 = loadValue(cUnit, rlSrc2, kFPReg);
    rlResult = oatEvalLoc(cUnit, rlDest, kFPReg, true);
    newLIR3(cUnit, (MipsOpCode)op, rlResult.lowReg, rlSrc1.lowReg, rlSrc2.lowReg);
    storeValue(cUnit, rlDest, rlResult);

    return false;
#else
    UNIMPLEMENTED(FATAL) << "Need Mips implementation";
    return false;
#if 0
    TemplateOpcode opcode;

    /*
     * Don't attempt to optimize register usage since these opcodes call out to
     * the handlers.
     */
    switch (mir->dalvikInsn.opcode) {
        case OP_ADD_FLOAT_2ADDR:
        case OP_ADD_FLOAT:
            opcode = TEMPLATE_ADD_FLOAT_VFP;
            break;
        case OP_SUB_FLOAT_2ADDR:
        case OP_SUB_FLOAT:
            opcode = TEMPLATE_SUB_FLOAT_VFP;
            break;
        case OP_DIV_FLOAT_2ADDR:
        case OP_DIV_FLOAT:
            opcode = TEMPLATE_DIV_FLOAT_VFP;
            break;
        case OP_MUL_FLOAT_2ADDR:
        case OP_MUL_FLOAT:
            opcode = TEMPLATE_MUL_FLOAT_VFP;
            break;
        case OP_REM_FLOAT_2ADDR:
        case OP_REM_FLOAT:
        case OP_NEG_FLOAT: {
            return genArithOpFloatPortable(cUnit, mir, rlDest, rlSrc1, rlSrc2);
        }
        default:
            return true;
    }
    loadValueAddress(cUnit, rlDest, r_A0);
    oatClobber(cUnit, r_A0);
    loadValueAddress(cUnit, rlSrc1, r_A1);
    oatClobber(cUnit, r_A1);
    loadValueAddress(cUnit, rlSrc2, r_A2);
    UNIMP(FATAL) << "Need callout to handler";
#if 0
    genDispatchToHandler(cUnit, opcode);
#endif
    rlDest = oatUpdateLoc(cUnit, rlDest);
    if (rlDest.location == kLocPhysReg) {
        oatClobber(cUnit, rlDest.lowReg);
    }
    return false;
#endif
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
        case OP_ADD_DOUBLE_2ADDR:
        case OP_ADD_DOUBLE:
            op = kMipsFaddd;
            break;
        case OP_SUB_DOUBLE_2ADDR:
        case OP_SUB_DOUBLE:
            op = kMipsFsubd;
            break;
        case OP_DIV_DOUBLE_2ADDR:
        case OP_DIV_DOUBLE:
            op = kMipsFdivd;
            break;
        case OP_MUL_DOUBLE_2ADDR:
        case OP_MUL_DOUBLE:
            op = kMipsFmuld;
            break;
        case OP_REM_DOUBLE_2ADDR:
        case OP_REM_DOUBLE:
        case OP_NEG_DOUBLE: {
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
    UNIMPLEMENTED(FATAL) << "Need Mips implementation";
    return false;
#if 0
    TemplateOpcode opcode;

    switch (mir->dalvikInsn.opcode) {
        case OP_ADD_DOUBLE_2ADDR:
        case OP_ADD_DOUBLE:
            opcode = TEMPLATE_ADD_DOUBLE_VFP;
            break;
        case OP_SUB_DOUBLE_2ADDR:
        case OP_SUB_DOUBLE:
            opcode = TEMPLATE_SUB_DOUBLE_VFP;
            break;
        case OP_DIV_DOUBLE_2ADDR:
        case OP_DIV_DOUBLE:
            opcode = TEMPLATE_DIV_DOUBLE_VFP;
            break;
        case OP_MUL_DOUBLE_2ADDR:
        case OP_MUL_DOUBLE:
            opcode = TEMPLATE_MUL_DOUBLE_VFP;
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
    loadValueAddress(cUnit, rlDest, r_A0);
    oatClobber(cUnit, r_A0);
    loadValueAddress(cUnit, rlSrc1, r_A1);
    oatClobber(cUnit, r_A1);
    loadValueAddress(cUnit, rlSrc2, r_A2);
    UNIMP(FATAL) << "Need callout to handler";
#if 0
    genDispatchToHandler(cUnit, opcode);
#endif
    rlDest = oatUpdateLocWide(cUnit, rlDest);
    if (rlDest.location == kLocPhysReg) {
        oatClobber(cUnit, rlDest.lowReg);
        oatClobber(cUnit, rlDest.highReg);
    }
    return false;
#endif
#endif
}

static bool genConversion(CompilationUnit *cUnit, MIR *mir)
{
    Opcode opcode = mir->dalvikInsn.opcode;
    bool longSrc = false;
    bool longDest = false;
    RegLocation rlSrc;
    RegLocation rlDest;
#ifdef __mips_hard_float
    int op = kMipsNop;
    int srcReg;
    RegLocation rlResult;
    switch (opcode) {
        case OP_INT_TO_FLOAT:
            longSrc = false;
            longDest = false;
            op = kMipsFcvtsw;
            break;
        case OP_DOUBLE_TO_FLOAT:
            longSrc = true;
            longDest = false;
            op = kMipsFcvtsd;
            break;
        case OP_FLOAT_TO_DOUBLE:
            longSrc = false;
            longDest = true;
            op = kMipsFcvtds;
            break;
        case OP_INT_TO_DOUBLE:
            longSrc = false;
            longDest = true;
            op = kMipsFcvtdw;
            break;
        case OP_FLOAT_TO_INT:
        case OP_DOUBLE_TO_INT:
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
    UNIMPLEMENTED(FATAL) << "Need Mips implementation";
    return false;
#if 0
    TemplateOpcode templateOpcode;
    switch (opcode) {
        case OP_INT_TO_FLOAT:
            longSrc = false;
            longDest = false;
            templateOpcode = TEMPLATE_INT_TO_FLOAT_VFP;
            break;
        case OP_FLOAT_TO_INT:
            longSrc = false;
            longDest = false;
            templateOpcode = TEMPLATE_FLOAT_TO_INT_VFP;
            break;
        case OP_DOUBLE_TO_FLOAT:
            longSrc = true;
            longDest = false;
            templateOpcode = TEMPLATE_DOUBLE_TO_FLOAT_VFP;
            break;
        case OP_FLOAT_TO_DOUBLE:
            longSrc = false;
            longDest = true;
            templateOpcode = TEMPLATE_FLOAT_TO_DOUBLE_VFP;
            break;
        case OP_INT_TO_DOUBLE:
            longSrc = false;
            longDest = true;
            templateOpcode = TEMPLATE_INT_TO_DOUBLE_VFP;
            break;
        case OP_DOUBLE_TO_INT:
            longSrc = true;
            longDest = false;
            templateOpcode = TEMPLATE_DOUBLE_TO_INT_VFP;
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
    } else {
        rlSrc = oatGetSrc(cUnit, mir, 0);
    }

    if (longDest) {
        rlDest = oatGetDestWide(cUnit, mir, 0, 1);
    } else {
        rlDest = oatGetDest(cUnit, mir, 0);
    }
    loadValueAddress(cUnit, rlDest, r_A0);
    oatClobber(cUnit, r_A0);
    loadValueAddress(cUnit, rlSrc, r_A1);
    UNIMP(FATAL) << "Need callout to handler";
#if 0
    genDispatchToHandler(cUnit, templateOpcode);
#endif
    if (rlDest.wide) {
        rlDest = oatUpdateLocWide(cUnit, rlDest);
        oatClobber(cUnit, rlDest.highReg);
    } else {
        rlDest = oatUpdateLoc(cUnit, rlDest);
    }
    oatClobber(cUnit, rlDest.lowReg);
    return false;
#endif
#endif
}

static bool genCmpFP(CompilationUnit *cUnit, MIR *mir, RegLocation rlDest,
                     RegLocation rlSrc1, RegLocation rlSrc2)
{
    UNIMPLEMENTED(FATAL) << "Need Mips implementation";
    return false;
#if 0
    TemplateOpcode templateOpcode;
    RegLocation rlResult = oatGetReturn(cUnit);
    bool wide = true;

    switch(mir->dalvikInsn.opcode) {
        case OP_CMPL_FLOAT:
            templateOpcode = TEMPLATE_CMPL_FLOAT_VFP;
            wide = false;
            break;
        case OP_CMPG_FLOAT:
            templateOpcode = TEMPLATE_CMPG_FLOAT_VFP;
            wide = false;
            break;
        case OP_CMPL_DOUBLE:
            templateOpcode = TEMPLATE_CMPL_DOUBLE_VFP;
            break;
        case OP_CMPG_DOUBLE:
            templateOpcode = TEMPLATE_CMPG_DOUBLE_VFP;
            break;
        default:
            return true;
    }
    loadValueAddress(cUnit, rlSrc1, r_A0);
    oatClobber(cUnit, r_A0);
    loadValueAddress(cUnit, rlSrc2, r_A1);
    UNIMP(FATAL) << "Need callout to handler";
#if 0
    genDispatchToHandler(cUnit, templateOpcode);
#endif
    storeValue(cUnit, rlDest, rlResult);
    return false;
#endif
}

} //  namespace art
