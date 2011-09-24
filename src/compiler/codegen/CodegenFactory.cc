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
 * This file contains target-independent codegen and support, and is
 * included by:
 *
 *        $(TARGET_ARCH)/Codegen-$(TARGET_ARCH_VARIANT).c
 *
 * which combines this common code with specific support found in the
 * applicable directories below this one.
 *
 * Prior to including this file, TGT_LIR should be #defined.
 * For example, for arm:
 *    #define TGT_LIR ArmLIR
 * and for x86:
 *    #define TGT_LIR X86LIR
 */


/* Load a word at base + displacement.  Displacement must be word multiple */
static TGT_LIR* loadWordDisp(CompilationUnit* cUnit, int rBase,
                             int displacement, int rDest)
{
    return loadBaseDisp(cUnit, NULL, rBase, displacement, rDest, kWord,
                        INVALID_SREG);
}

static TGT_LIR* storeWordDisp(CompilationUnit* cUnit, int rBase,
                             int displacement, int rSrc)
{
    return storeBaseDisp(cUnit, rBase, displacement, rSrc, kWord);
}

/*
 * Load a Dalvik register into a physical register.  Take care when
 * using this routine, as it doesn't perform any bookkeeping regarding
 * register liveness.  That is the responsibility of the caller.
 */
static void loadValueDirect(CompilationUnit* cUnit, RegLocation rlSrc,
                            int reg1)
{
    rlSrc = oatUpdateLoc(cUnit, rlSrc);
    if (rlSrc.location == kLocPhysReg) {
        genRegCopy(cUnit, reg1, rlSrc.lowReg);
    } else {
        assert(rlSrc.location == kLocDalvikFrame);
        loadWordDisp(cUnit, rSP, rlSrc.spOffset, reg1);
    }
}

/*
 * Similar to loadValueDirect, but clobbers and allocates the target
 * register.  Should be used when loading to a fixed register (for example,
 * loading arguments to an out of line call.
 */
static void loadValueDirectFixed(CompilationUnit* cUnit, RegLocation rlSrc,
                                 int reg1)
{
    oatClobber(cUnit, reg1);
    oatMarkInUse(cUnit, reg1);
    loadValueDirect(cUnit, rlSrc, reg1);
}

/*
 * Load a Dalvik register pair into a physical register[s].  Take care when
 * using this routine, as it doesn't perform any bookkeeping regarding
 * register liveness.  That is the responsibility of the caller.
 */
static void loadValueDirectWide(CompilationUnit* cUnit, RegLocation rlSrc,
                                int regLo, int regHi)
{
    rlSrc = oatUpdateLocWide(cUnit, rlSrc);
    if (rlSrc.location == kLocPhysReg) {
        genRegCopyWide(cUnit, regLo, regHi, rlSrc.lowReg, rlSrc.highReg);
    } else {
        assert(rlSrc.location == kLocDalvikFrame);
        loadBaseDispWide(cUnit, NULL, rSP, rlSrc.spOffset,
                         regLo, regHi, INVALID_SREG);
    }
}

/*
 * Similar to loadValueDirect, but clobbers and allocates the target
 * registers.  Should be used when loading to a fixed registers (for example,
 * loading arguments to an out of line call.
 */
static void loadValueDirectWideFixed(CompilationUnit* cUnit, RegLocation rlSrc,
                                     int regLo, int regHi)
{
    oatClobber(cUnit, regLo);
    oatClobber(cUnit, regHi);
    oatMarkInUse(cUnit, regLo);
    oatMarkInUse(cUnit, regHi);
    loadValueDirectWide(cUnit, rlSrc, regLo, regHi);
}

static RegLocation loadValue(CompilationUnit* cUnit, RegLocation rlSrc,
                             RegisterClass opKind)
{
    rlSrc = oatEvalLoc(cUnit, rlSrc, opKind, false);
    if (rlSrc.location == kLocDalvikFrame) {
        loadValueDirect(cUnit, rlSrc, rlSrc.lowReg);
        rlSrc.location = kLocPhysReg;
        oatMarkLive(cUnit, rlSrc.lowReg, rlSrc.sRegLow);
    }
    return rlSrc;
}

static void storeValue(CompilationUnit* cUnit, RegLocation rlDest,
                       RegLocation rlSrc)
{
    LIR* defStart;
    LIR* defEnd;
    assert(!rlDest.wide);
    assert(!rlSrc.wide);
    rlSrc = oatUpdateLoc(cUnit, rlSrc);
    rlDest = oatUpdateLoc(cUnit, rlDest);
    if (rlSrc.location == kLocPhysReg) {
        if (oatIsLive(cUnit, rlSrc.lowReg) ||
            (rlDest.location == kLocPhysReg)) {
            // Src is live or Dest has assigned reg.
            rlDest = oatEvalLoc(cUnit, rlDest, kAnyReg, false);
            genRegCopy(cUnit, rlDest.lowReg, rlSrc.lowReg);
        } else {
            // Just re-assign the registers.  Dest gets Src's regs
            rlDest.lowReg = rlSrc.lowReg;
            oatClobber(cUnit, rlSrc.lowReg);
        }
    } else {
        // Load Src either into promoted Dest or temps allocated for Dest
        rlDest = oatEvalLoc(cUnit, rlDest, kAnyReg, false);
        loadValueDirect(cUnit, rlSrc, rlDest.lowReg);
    }

    // Dest is now live and dirty (until/if we flush it to home location)
    oatMarkLive(cUnit, rlDest.lowReg, rlDest.sRegLow);
    oatMarkDirty(cUnit, rlDest);


    oatResetDefLoc(cUnit, rlDest);
    if (oatIsDirty(cUnit, rlDest.lowReg) &&
        oatLiveOut(cUnit, rlDest.sRegLow)) {
        defStart = (LIR* )cUnit->lastLIRInsn;
        storeBaseDisp(cUnit, rSP, rlDest.spOffset, rlDest.lowReg, kWord);
        oatMarkClean(cUnit, rlDest);
        defEnd = (LIR* )cUnit->lastLIRInsn;
        oatMarkDef(cUnit, rlDest, defStart, defEnd);
    }
}

static RegLocation loadValueWide(CompilationUnit* cUnit, RegLocation rlSrc,
                                 RegisterClass opKind)
{
    assert(rlSrc.wide);
    rlSrc = oatEvalLoc(cUnit, rlSrc, opKind, false);
    if (rlSrc.location == kLocDalvikFrame) {
        loadValueDirectWide(cUnit, rlSrc, rlSrc.lowReg, rlSrc.highReg);
        rlSrc.location = kLocPhysReg;
        oatMarkLive(cUnit, rlSrc.lowReg, rlSrc.sRegLow);
        oatMarkLive(cUnit, rlSrc.highReg,
                            oatSRegHi(rlSrc.sRegLow));
    }
    return rlSrc;
}

static void storeValueWide(CompilationUnit* cUnit, RegLocation rlDest,
                           RegLocation rlSrc)
{
    LIR* defStart;
    LIR* defEnd;
    if (FPREG(rlSrc.lowReg)!=FPREG(rlSrc.highReg)) {
        LOG(WARNING) << "rlSrc.lowreg:" << rlSrc.lowReg << ", rlSrc.highReg:"
                     << rlSrc.highReg;
    }
    assert(FPREG(rlSrc.lowReg)==FPREG(rlSrc.highReg));
    assert(rlDest.wide);
    assert(rlSrc.wide);
    if (rlSrc.location == kLocPhysReg) {
        if (oatIsLive(cUnit, rlSrc.lowReg) ||
            oatIsLive(cUnit, rlSrc.highReg) ||
            (rlDest.location == kLocPhysReg)) {
            // Src is live or Dest has assigned reg.
            rlDest = oatEvalLoc(cUnit, rlDest, kAnyReg, false);
            genRegCopyWide(cUnit, rlDest.lowReg, rlDest.highReg,
                           rlSrc.lowReg, rlSrc.highReg);
        } else {
            // Just re-assign the registers.  Dest gets Src's regs
            rlDest.lowReg = rlSrc.lowReg;
            rlDest.highReg = rlSrc.highReg;
            oatClobber(cUnit, rlSrc.lowReg);
            oatClobber(cUnit, rlSrc.highReg);
        }
    } else {
        // Load Src either into promoted Dest or temps allocated for Dest
        rlDest = oatEvalLoc(cUnit, rlDest, kAnyReg, false);
        loadValueDirectWide(cUnit, rlSrc, rlDest.lowReg,
                            rlDest.highReg);
    }

    // Dest is now live and dirty (until/if we flush it to home location)
    oatMarkLive(cUnit, rlDest.lowReg, rlDest.sRegLow);
    oatMarkLive(cUnit, rlDest.highReg,
                        oatSRegHi(rlDest.sRegLow));
    oatMarkDirty(cUnit, rlDest);
    oatMarkPair(cUnit, rlDest.lowReg, rlDest.highReg);


    oatResetDefLocWide(cUnit, rlDest);
    if ((oatIsDirty(cUnit, rlDest.lowReg) ||
        oatIsDirty(cUnit, rlDest.highReg)) &&
        (oatLiveOut(cUnit, rlDest.sRegLow) ||
        oatLiveOut(cUnit, oatSRegHi(rlDest.sRegLow)))) {
        defStart = (LIR*)cUnit->lastLIRInsn;
        assert((oatS2VReg(cUnit, rlDest.sRegLow)+1) ==
                oatS2VReg(cUnit, oatSRegHi(rlDest.sRegLow)));
        storeBaseDispWide(cUnit, rSP, rlDest.spOffset,
                          rlDest.lowReg, rlDest.highReg);
        oatMarkClean(cUnit, rlDest);
        defEnd = (LIR*)cUnit->lastLIRInsn;
        oatMarkDefWide(cUnit, rlDest, defStart, defEnd);
    }
}
