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

#include "oat/runtime/oat_support_entrypoints.h"

namespace art {

/*
 * This source files contains "gen" codegen routines that should
 * be applicable to most targets.  Only mid-level support utilities
 * and "op" calls may be used here.
 */

/*
 * If there are any ins passed in registers that have not been promoted
 * to a callee-save register, flush them to the frame.  Perform intial
 * assignment of promoted arguments.
 *
 * argLocs is an array of location records describing the incoming arguments
 * with one location record per word of argument.
 */
void flushIns(CompilationUnit* cUnit, RegLocation* argLocs, RegLocation rlMethod)
{
  /*
   * Dummy up a RegLocation for the incoming Method*
   * It will attempt to keep kArg0 live (or copy it to home location
   * if promoted).
   */
  RegLocation rlSrc = rlMethod;
  rlSrc.location = kLocPhysReg;
  rlSrc.lowReg = targetReg(kArg0);
  rlSrc.home = false;
  oatMarkLive(cUnit, rlSrc.lowReg, rlSrc.sRegLow);
  storeValue(cUnit, rlMethod, rlSrc);
  // If Method* has been promoted, explicitly flush
  if (rlMethod.location == kLocPhysReg) {
    storeWordDisp(cUnit, targetReg(kSp), 0, targetReg(kArg0));
  }

  if (cUnit->numIns == 0)
    return;
  const int numArgRegs = 3;
  static SpecialTargetRegister argRegs[] = {kArg1, kArg2, kArg3};
  int startVReg = cUnit->numDalvikRegisters - cUnit->numIns;
  /*
   * Copy incoming arguments to their proper home locations.
   * NOTE: an older version of dx had an issue in which
   * it would reuse static method argument registers.
   * This could result in the same Dalvik virtual register
   * being promoted to both core and fp regs. To account for this,
   * we only copy to the corresponding promoted physical register
   * if it matches the type of the SSA name for the incoming
   * argument.  It is also possible that long and double arguments
   * end up half-promoted.  In those cases, we must flush the promoted
   * half to memory as well.
   */
  for (int i = 0; i < cUnit->numIns; i++) {
    PromotionMap* vMap = &cUnit->promotionMap[startVReg + i];
    if (i < numArgRegs) {
      // If arriving in register
      bool needFlush = true;
      RegLocation* tLoc = &argLocs[i];
      if ((vMap->coreLocation == kLocPhysReg) && !tLoc->fp) {
        opRegCopy(cUnit, vMap->coreReg, targetReg(argRegs[i]));
        needFlush = false;
      } else if ((vMap->fpLocation == kLocPhysReg) && tLoc->fp) {
        opRegCopy(cUnit, vMap->fpReg, targetReg(argRegs[i]));
        needFlush = false;
      } else {
        needFlush = true;
      }

      // For wide args, force flush if only half is promoted
      if (tLoc->wide) {
        PromotionMap* pMap = vMap + (tLoc->highWord ? -1 : +1);
        needFlush |= (pMap->coreLocation != vMap->coreLocation) ||
            (pMap->fpLocation != vMap->fpLocation);
      }
      if (needFlush) {
        storeBaseDisp(cUnit, targetReg(kSp), oatSRegOffset(cUnit, startVReg + i),
                      targetReg(argRegs[i]), kWord);
      }
    } else {
      // If arriving in frame & promoted
      if (vMap->coreLocation == kLocPhysReg) {
        loadWordDisp(cUnit, targetReg(kSp), oatSRegOffset(cUnit, startVReg + i),
                     vMap->coreReg);
      }
      if (vMap->fpLocation == kLocPhysReg) {
        loadWordDisp(cUnit, targetReg(kSp), oatSRegOffset(cUnit, startVReg + i),
                     vMap->fpReg);
      }
    }
  }
}

void scanMethodLiteralPool(CompilationUnit* cUnit, LIR** methodTarget, LIR** codeTarget,
                           const DexFile* dexFile, uint32_t dexMethodIdx)
{
  LIR* curTarget = cUnit->methodLiteralList;
  LIR* nextTarget = curTarget != NULL ? curTarget->next : NULL;
  while (curTarget != NULL && nextTarget != NULL) {
    if (curTarget->operands[0] == (int)dexFile &&
      nextTarget->operands[0] == (int)dexMethodIdx) {
    *codeTarget = curTarget;
    *methodTarget = nextTarget;
    DCHECK((*codeTarget)->next == *methodTarget);
    DCHECK_EQ((*codeTarget)->operands[0], (int)dexFile);
    DCHECK_EQ((*methodTarget)->operands[0], (int)dexMethodIdx);
    break;
    }
    curTarget = nextTarget->next;
    nextTarget = curTarget != NULL ? curTarget->next : NULL;
  }
}

/*
 * Bit of a hack here - in the absence of a real scheduling pass,
 * emit the next instruction in static & direct invoke sequences.
 */
int nextSDCallInsn(CompilationUnit* cUnit, CallInfo* info,
                   int state, uint32_t dexIdx, uint32_t unused,
                   uintptr_t directCode, uintptr_t directMethod,
                   InvokeType type)
{
  if (cUnit->instructionSet != kThumb2) {
    // Disable sharpening
    directCode = 0;
    directMethod = 0;
  }
  if (directCode != 0 && directMethod != 0) {
    switch (state) {
    case 0:  // Get the current Method* [sets kArg0]
      if (directCode != (uintptr_t)-1) {
        loadConstant(cUnit, targetReg(kInvokeTgt), directCode);
      } else {
        LIR* dataTarget = scanLiteralPool(cUnit->codeLiteralList, dexIdx, 0);
        if (dataTarget == NULL) {
          dataTarget = addWordData(cUnit, &cUnit->codeLiteralList, dexIdx);
          dataTarget->operands[1] = type;
        }
        LIR* loadPcRel = opPcRelLoad(cUnit, targetReg(kInvokeTgt), dataTarget);
        oatAppendLIR(cUnit, loadPcRel);
        DCHECK_EQ(cUnit->instructionSet, kThumb2) << (void*)dataTarget;
      }
      if (directMethod != (uintptr_t)-1) {
        loadConstant(cUnit, targetReg(kArg0), directMethod);
      } else {
        LIR* dataTarget = scanLiteralPool(cUnit->methodLiteralList, dexIdx, 0);
        if (dataTarget == NULL) {
          dataTarget = addWordData(cUnit, &cUnit->methodLiteralList, dexIdx);
          dataTarget->operands[1] = type;
        }
        LIR* loadPcRel = opPcRelLoad(cUnit, targetReg(kArg0), dataTarget);
        oatAppendLIR(cUnit, loadPcRel);
        DCHECK_EQ(cUnit->instructionSet, kThumb2) << (void*)dataTarget;
      }
      break;
    default:
      return -1;
    }
  } else {
    switch (state) {
    case 0:  // Get the current Method* [sets kArg0]
      // TUNING: we can save a reg copy if Method* has been promoted.
      loadCurrMethodDirect(cUnit, targetReg(kArg0));
      break;
    case 1:  // Get method->dex_cache_resolved_methods_
      loadWordDisp(cUnit, targetReg(kArg0),
        AbstractMethod::DexCacheResolvedMethodsOffset().Int32Value(), targetReg(kArg0));
      // Set up direct code if known.
      if (directCode != 0) {
        if (directCode != (uintptr_t)-1) {
          loadConstant(cUnit, targetReg(kInvokeTgt), directCode);
        } else {
          LIR* dataTarget = scanLiteralPool(cUnit->codeLiteralList, dexIdx, 0);
          if (dataTarget == NULL) {
            dataTarget = addWordData(cUnit, &cUnit->codeLiteralList, dexIdx);
            dataTarget->operands[1] = type;
          }
          LIR* loadPcRel = opPcRelLoad(cUnit, targetReg(kInvokeTgt), dataTarget);
          oatAppendLIR(cUnit, loadPcRel);
          DCHECK_EQ(cUnit->instructionSet, kThumb2) << (void*)dataTarget;
        }
      }
      break;
    case 2:  // Grab target method*
      loadWordDisp(cUnit, targetReg(kArg0),
                   Array::DataOffset(sizeof(Object*)).Int32Value() + dexIdx * 4, targetReg(kArg0));
      break;
    case 3:  // Grab the code from the method*
      if (cUnit->instructionSet != kX86) {
        if (directCode == 0) {
          loadWordDisp(cUnit, targetReg(kArg0), AbstractMethod::GetCodeOffset().Int32Value(),
                       targetReg(kInvokeTgt));
        }
        break;
      }
      // Intentional fallthrough for x86
    default:
      return -1;
    }
  }
  return state + 1;
}

/*
 * Bit of a hack here - in the absence of a real scheduling pass,
 * emit the next instruction in a virtual invoke sequence.
 * We can use kLr as a temp prior to target address loading
 * Note also that we'll load the first argument ("this") into
 * kArg1 here rather than the standard loadArgRegs.
 */
int nextVCallInsn(CompilationUnit* cUnit, CallInfo* info,
                  int state, uint32_t dexIdx, uint32_t methodIdx,
                  uintptr_t unused, uintptr_t unused2, InvokeType unused3)
{
  /*
   * This is the fast path in which the target virtual method is
   * fully resolved at compile time.
   */
  switch (state) {
    case 0: {  // Get "this" [set kArg1]
      RegLocation  rlArg = info->args[0];
      loadValueDirectFixed(cUnit, rlArg, targetReg(kArg1));
      break;
    }
    case 1: // Is "this" null? [use kArg1]
      genNullCheck(cUnit, info->args[0].sRegLow, targetReg(kArg1), info->optFlags);
      // get this->klass_ [use kArg1, set kInvokeTgt]
      loadWordDisp(cUnit, targetReg(kArg1), Object::ClassOffset().Int32Value(),
                   targetReg(kInvokeTgt));
      break;
    case 2: // Get this->klass_->vtable [usr kInvokeTgt, set kInvokeTgt]
      loadWordDisp(cUnit, targetReg(kInvokeTgt), Class::VTableOffset().Int32Value(),
                   targetReg(kInvokeTgt));
      break;
    case 3: // Get target method [use kInvokeTgt, set kArg0]
      loadWordDisp(cUnit, targetReg(kInvokeTgt), (methodIdx * 4) +
                   Array::DataOffset(sizeof(Object*)).Int32Value(), targetReg(kArg0));
      break;
    case 4: // Get the compiled code address [uses kArg0, sets kInvokeTgt]
      if (cUnit->instructionSet != kX86) {
        loadWordDisp(cUnit, targetReg(kArg0), AbstractMethod::GetCodeOffset().Int32Value(),
                     targetReg(kInvokeTgt));
        break;
      }
      // Intentional fallthrough for X86
    default:
      return -1;
  }
  return state + 1;
}

/*
 * All invoke-interface calls bounce off of art_invoke_interface_trampoline,
 * which will locate the target and continue on via a tail call.
 */
int nextInterfaceCallInsn(CompilationUnit* cUnit, CallInfo* info, int state,
                          uint32_t dexIdx, uint32_t unused, uintptr_t unused2,
                          uintptr_t directMethod, InvokeType unused4)
{
  if (cUnit->instructionSet != kThumb2) {
    // Disable sharpening
    directMethod = 0;
  }
  int trampoline = (cUnit->instructionSet == kX86) ? 0
      : ENTRYPOINT_OFFSET(pInvokeInterfaceTrampoline);

  if (directMethod != 0) {
    switch (state) {
      case 0:  // Load the trampoline target [sets kInvokeTgt].
        if (cUnit->instructionSet != kX86) {
          loadWordDisp(cUnit, targetReg(kSelf), trampoline, targetReg(kInvokeTgt));
        }
        // Get the interface Method* [sets kArg0]
        if (directMethod != (uintptr_t)-1) {
          loadConstant(cUnit, targetReg(kArg0), directMethod);
        } else {
          LIR* dataTarget = scanLiteralPool(cUnit->methodLiteralList, dexIdx, 0);
          if (dataTarget == NULL) {
            dataTarget = addWordData(cUnit, &cUnit->methodLiteralList, dexIdx);
            dataTarget->operands[1] = kInterface;
          }
          LIR* loadPcRel = opPcRelLoad(cUnit, targetReg(kArg0), dataTarget);
          oatAppendLIR(cUnit, loadPcRel);
          DCHECK_EQ(cUnit->instructionSet, kThumb2) << (void*)dataTarget;
        }
        break;
      default:
        return -1;
    }
  } else {
    switch (state) {
      case 0:
        // Get the current Method* [sets kArg0] - TUNING: remove copy of method if it is promoted.
        loadCurrMethodDirect(cUnit, targetReg(kArg0));
        // Load the trampoline target [sets kInvokeTgt].
        if (cUnit->instructionSet != kX86) {
          loadWordDisp(cUnit, targetReg(kSelf), trampoline, targetReg(kInvokeTgt));
        }
        break;
    case 1:  // Get method->dex_cache_resolved_methods_ [set/use kArg0]
      loadWordDisp(cUnit, targetReg(kArg0),
                   AbstractMethod::DexCacheResolvedMethodsOffset().Int32Value(),
                   targetReg(kArg0));
      break;
    case 2:  // Grab target method* [set/use kArg0]
      loadWordDisp(cUnit, targetReg(kArg0),
                   Array::DataOffset(sizeof(Object*)).Int32Value() + dexIdx * 4,
                   targetReg(kArg0));
      break;
    default:
      return -1;
    }
  }
  return state + 1;
}

int nextInvokeInsnSP(CompilationUnit* cUnit, CallInfo* info, int trampoline,
                     int state, uint32_t dexIdx, uint32_t methodIdx)
{
  /*
   * This handles the case in which the base method is not fully
   * resolved at compile time, we bail to a runtime helper.
   */
  if (state == 0) {
    if (cUnit->instructionSet != kX86) {
      // Load trampoline target
      loadWordDisp(cUnit, targetReg(kSelf), trampoline, targetReg(kInvokeTgt));
    }
    // Load kArg0 with method index
    loadConstant(cUnit, targetReg(kArg0), dexIdx);
    return 1;
  }
  return -1;
}

int nextStaticCallInsnSP(CompilationUnit* cUnit, CallInfo* info,
                         int state, uint32_t dexIdx, uint32_t methodIdx,
                         uintptr_t unused, uintptr_t unused2,
                         InvokeType unused3)
{
  int trampoline = ENTRYPOINT_OFFSET(pInvokeStaticTrampolineWithAccessCheck);
  return nextInvokeInsnSP(cUnit, info, trampoline, state, dexIdx, 0);
}

int nextDirectCallInsnSP(CompilationUnit* cUnit, CallInfo* info, int state,
                         uint32_t dexIdx, uint32_t methodIdx, uintptr_t unused,
                         uintptr_t unused2, InvokeType unused3)
{
  int trampoline = ENTRYPOINT_OFFSET(pInvokeDirectTrampolineWithAccessCheck);
  return nextInvokeInsnSP(cUnit, info, trampoline, state, dexIdx, 0);
}

int nextSuperCallInsnSP(CompilationUnit* cUnit, CallInfo* info, int state,
                        uint32_t dexIdx, uint32_t methodIdx, uintptr_t unused,
                        uintptr_t unused2, InvokeType unused3)
{
  int trampoline = ENTRYPOINT_OFFSET(pInvokeSuperTrampolineWithAccessCheck);
  return nextInvokeInsnSP(cUnit, info, trampoline, state, dexIdx, 0);
}

int nextVCallInsnSP(CompilationUnit* cUnit, CallInfo* info, int state,
                    uint32_t dexIdx, uint32_t methodIdx, uintptr_t unused,
                    uintptr_t unused2, InvokeType unused3)
{
  int trampoline = ENTRYPOINT_OFFSET(pInvokeVirtualTrampolineWithAccessCheck);
  return nextInvokeInsnSP(cUnit, info, trampoline, state, dexIdx, 0);
}

int nextInterfaceCallInsnWithAccessCheck(CompilationUnit* cUnit,
                                         CallInfo* info, int state,
                                         uint32_t dexIdx, uint32_t unused,
                                         uintptr_t unused2, uintptr_t unused3,
                                         InvokeType unused4)
{
  int trampoline = ENTRYPOINT_OFFSET(pInvokeInterfaceTrampolineWithAccessCheck);
  return nextInvokeInsnSP(cUnit, info, trampoline, state, dexIdx, 0);
}

int loadArgRegs(CompilationUnit* cUnit, CallInfo* info, int callState,
                NextCallInsn nextCallInsn, uint32_t dexIdx,
                uint32_t methodIdx, uintptr_t directCode,
                uintptr_t directMethod, InvokeType type, bool skipThis)
{
  int lastArgReg = targetReg(kArg3);
  int nextReg = targetReg(kArg1);
  int nextArg = 0;
  if (skipThis) {
    nextReg++;
    nextArg++;
  }
  for (; (nextReg <= lastArgReg) && (nextArg < info->numArgWords); nextReg++) {
    RegLocation rlArg = info->args[nextArg++];
    rlArg = oatUpdateRawLoc(cUnit, rlArg);
    if (rlArg.wide && (nextReg <= targetReg(kArg2))) {
      loadValueDirectWideFixed(cUnit, rlArg, nextReg, nextReg + 1);
      nextReg++;
      nextArg++;
    } else {
      rlArg.wide = false;
      loadValueDirectFixed(cUnit, rlArg, nextReg);
    }
    callState = nextCallInsn(cUnit, info, callState, dexIdx, methodIdx,
                 directCode, directMethod, type);
  }
  return callState;
}

/*
 * Load up to 5 arguments, the first three of which will be in
 * kArg1 .. kArg3.  On entry kArg0 contains the current method pointer,
 * and as part of the load sequence, it must be replaced with
 * the target method pointer.  Note, this may also be called
 * for "range" variants if the number of arguments is 5 or fewer.
 */
int genDalvikArgsNoRange(CompilationUnit* cUnit, CallInfo* info,
                         int callState,
                         LIR** pcrLabel, NextCallInsn nextCallInsn,
                         uint32_t dexIdx, uint32_t methodIdx,
                         uintptr_t directCode, uintptr_t directMethod,
                         InvokeType type, bool skipThis)
{
  RegLocation rlArg;

  /* If no arguments, just return */
  if (info->numArgWords == 0)
    return callState;

  callState = nextCallInsn(cUnit, info, callState, dexIdx, methodIdx,
                           directCode, directMethod, type);

  DCHECK_LE(info->numArgWords, 5);
  if (info->numArgWords > 3) {
    int32_t nextUse = 3;
    //Detect special case of wide arg spanning arg3/arg4
    RegLocation rlUse0 = info->args[0];
    RegLocation rlUse1 = info->args[1];
    RegLocation rlUse2 = info->args[2];
    if (((!rlUse0.wide && !rlUse1.wide) || rlUse0.wide) &&
      rlUse2.wide) {
      int reg = -1;
      // Wide spans, we need the 2nd half of uses[2].
      rlArg = oatUpdateLocWide(cUnit, rlUse2);
      if (rlArg.location == kLocPhysReg) {
        reg = rlArg.highReg;
      } else {
        // kArg2 & rArg3 can safely be used here
        reg = targetReg(kArg3);
        loadWordDisp(cUnit, targetReg(kSp), oatSRegOffset(cUnit, rlArg.sRegLow) + 4, reg);
        callState = nextCallInsn(cUnit, info, callState, dexIdx,
                                 methodIdx, directCode, directMethod, type);
      }
      storeBaseDisp(cUnit, targetReg(kSp), (nextUse + 1) * 4, reg, kWord);
      storeBaseDisp(cUnit, targetReg(kSp), 16 /* (3+1)*4 */, reg, kWord);
      callState = nextCallInsn(cUnit, info, callState, dexIdx, methodIdx,
                               directCode, directMethod, type);
      nextUse++;
    }
    // Loop through the rest
    while (nextUse < info->numArgWords) {
      int lowReg;
      int highReg = -1;
      rlArg = info->args[nextUse];
      rlArg = oatUpdateRawLoc(cUnit, rlArg);
      if (rlArg.location == kLocPhysReg) {
        lowReg = rlArg.lowReg;
        highReg = rlArg.highReg;
      } else {
        lowReg = targetReg(kArg2);
        if (rlArg.wide) {
          highReg = targetReg(kArg3);
          loadValueDirectWideFixed(cUnit, rlArg, lowReg, highReg);
        } else {
          loadValueDirectFixed(cUnit, rlArg, lowReg);
        }
        callState = nextCallInsn(cUnit, info, callState, dexIdx,
                                 methodIdx, directCode, directMethod, type);
      }
      int outsOffset = (nextUse + 1) * 4;
      if (rlArg.wide) {
        storeBaseDispWide(cUnit, targetReg(kSp), outsOffset, lowReg, highReg);
        nextUse += 2;
      } else {
        storeWordDisp(cUnit, targetReg(kSp), outsOffset, lowReg);
        nextUse++;
      }
      callState = nextCallInsn(cUnit, info, callState, dexIdx, methodIdx,
                               directCode, directMethod, type);
    }
  }

  callState = loadArgRegs(cUnit, info, callState, nextCallInsn,
                          dexIdx, methodIdx, directCode, directMethod,
                          type, skipThis);

  if (pcrLabel) {
    *pcrLabel = genNullCheck(cUnit, info->args[0].sRegLow, targetReg(kArg1),
                             info->optFlags);
  }
  return callState;
}

/*
 * May have 0+ arguments (also used for jumbo).  Note that
 * source virtual registers may be in physical registers, so may
 * need to be flushed to home location before copying.  This
 * applies to arg3 and above (see below).
 *
 * Two general strategies:
 *    If < 20 arguments
 *       Pass args 3-18 using vldm/vstm block copy
 *       Pass arg0, arg1 & arg2 in kArg1-kArg3
 *    If 20+ arguments
 *       Pass args arg19+ using memcpy block copy
 *       Pass arg0, arg1 & arg2 in kArg1-kArg3
 *
 */
int genDalvikArgsRange(CompilationUnit* cUnit, CallInfo* info, int callState,
                       LIR** pcrLabel, NextCallInsn nextCallInsn,
                       uint32_t dexIdx, uint32_t methodIdx,
                       uintptr_t directCode, uintptr_t directMethod,
                       InvokeType type, bool skipThis)
{

  // If we can treat it as non-range (Jumbo ops will use range form)
  if (info->numArgWords <= 5)
    return genDalvikArgsNoRange(cUnit, info, callState, pcrLabel,
                                nextCallInsn, dexIdx, methodIdx,
                                directCode, directMethod, type, skipThis);
  /*
   * First load the non-register arguments.  Both forms expect all
   * of the source arguments to be in their home frame location, so
   * scan the sReg names and flush any that have been promoted to
   * frame backing storage.
   */
  // Scan the rest of the args - if in physReg flush to memory
  for (int nextArg = 0; nextArg < info->numArgWords;) {
    RegLocation loc = info->args[nextArg];
    if (loc.wide) {
      loc = oatUpdateLocWide(cUnit, loc);
      if ((nextArg >= 2) && (loc.location == kLocPhysReg)) {
        storeBaseDispWide(cUnit, targetReg(kSp), oatSRegOffset(cUnit, loc.sRegLow),
                          loc.lowReg, loc.highReg);
      }
      nextArg += 2;
    } else {
      loc = oatUpdateLoc(cUnit, loc);
      if ((nextArg >= 3) && (loc.location == kLocPhysReg)) {
        storeBaseDisp(cUnit, targetReg(kSp), oatSRegOffset(cUnit, loc.sRegLow),
                      loc.lowReg, kWord);
      }
      nextArg++;
    }
  }

  int startOffset = oatSRegOffset(cUnit, info->args[3].sRegLow);
  int outsOffset = 4 /* Method* */ + (3 * 4);
  if (cUnit->instructionSet != kThumb2) {
    // Generate memcpy
    opRegRegImm(cUnit, kOpAdd, targetReg(kArg0), targetReg(kSp), outsOffset);
    opRegRegImm(cUnit, kOpAdd, targetReg(kArg1), targetReg(kSp), startOffset);
    callRuntimeHelperRegRegImm(cUnit, ENTRYPOINT_OFFSET(pMemcpy), targetReg(kArg0),
                               targetReg(kArg1), (info->numArgWords - 3) * 4, false);
  } else {
    if (info->numArgWords >= 20) {
      // Generate memcpy
      opRegRegImm(cUnit, kOpAdd, targetReg(kArg0), targetReg(kSp), outsOffset);
      opRegRegImm(cUnit, kOpAdd, targetReg(kArg1), targetReg(kSp), startOffset);
      callRuntimeHelperRegRegImm(cUnit, ENTRYPOINT_OFFSET(pMemcpy), targetReg(kArg0),
                                 targetReg(kArg1), (info->numArgWords - 3) * 4, false);
    } else {
      // Use vldm/vstm pair using kArg3 as a temp
      int regsLeft = std::min(info->numArgWords - 3, 16);
      callState = nextCallInsn(cUnit, info, callState, dexIdx, methodIdx,
                               directCode, directMethod, type);
      opRegRegImm(cUnit, kOpAdd, targetReg(kArg3), targetReg(kSp), startOffset);
      LIR* ld = opVldm(cUnit, targetReg(kArg3), regsLeft);
      //TUNING: loosen barrier
      ld->defMask = ENCODE_ALL;
      setMemRefType(ld, true /* isLoad */, kDalvikReg);
      callState = nextCallInsn(cUnit, info, callState, dexIdx, methodIdx,
                               directCode, directMethod, type);
      opRegRegImm(cUnit, kOpAdd, targetReg(kArg3), targetReg(kSp), 4 /* Method* */ + (3 * 4));
      callState = nextCallInsn(cUnit, info, callState, dexIdx, methodIdx,
                               directCode, directMethod, type);
      LIR* st = opVstm(cUnit, targetReg(kArg3), regsLeft);
      setMemRefType(st, false /* isLoad */, kDalvikReg);
      st->defMask = ENCODE_ALL;
      callState = nextCallInsn(cUnit, info, callState, dexIdx, methodIdx,
                               directCode, directMethod, type);
    }
  }

  callState = loadArgRegs(cUnit, info, callState, nextCallInsn,
                          dexIdx, methodIdx, directCode, directMethod,
                          type, skipThis);

  callState = nextCallInsn(cUnit, info, callState, dexIdx, methodIdx,
                           directCode, directMethod, type);
  if (pcrLabel) {
    *pcrLabel = genNullCheck(cUnit, info->args[0].sRegLow, targetReg(kArg1),
                             info->optFlags);
  }
  return callState;
}

RegLocation inlineTarget(CompilationUnit* cUnit, CallInfo* info)
{
  RegLocation res;
  if (info->result.location == kLocInvalid) {
    res = oatGetReturn(cUnit, false);
  } else {
    res = info->result;
  }
  return res;
}

RegLocation inlineTargetWide(CompilationUnit* cUnit, CallInfo* info)
{
  RegLocation res;
  if (info->result.location == kLocInvalid) {
    res = oatGetReturnWide(cUnit, false);
  } else {
    res = info->result;
  }
  return res;
}

bool genInlinedCharAt(CompilationUnit* cUnit, CallInfo* info)
{
  if (cUnit->instructionSet == kMips) {
    // TODO - add Mips implementation
    return false;
  }
  // Location of reference to data array
  int valueOffset = String::ValueOffset().Int32Value();
  // Location of count
  int countOffset = String::CountOffset().Int32Value();
  // Starting offset within data array
  int offsetOffset = String::OffsetOffset().Int32Value();
  // Start of char data with array_
  int dataOffset = Array::DataOffset(sizeof(uint16_t)).Int32Value();

  RegLocation rlObj = info->args[0];
  RegLocation rlIdx = info->args[1];
  rlObj = loadValue(cUnit, rlObj, kCoreReg);
  rlIdx = loadValue(cUnit, rlIdx, kCoreReg);
  int regMax;
  genNullCheck(cUnit, rlObj.sRegLow, rlObj.lowReg, info->optFlags);
  bool rangeCheck = (!(info->optFlags & MIR_IGNORE_RANGE_CHECK));
  LIR* launchPad = NULL;
  int regOff = INVALID_REG;
  int regPtr = INVALID_REG;
  if (cUnit->instructionSet != kX86) {
    regOff = oatAllocTemp(cUnit);
    regPtr = oatAllocTemp(cUnit);
    if (rangeCheck) {
      regMax = oatAllocTemp(cUnit);
      loadWordDisp(cUnit, rlObj.lowReg, countOffset, regMax);
    }
    loadWordDisp(cUnit, rlObj.lowReg, offsetOffset, regOff);
    loadWordDisp(cUnit, rlObj.lowReg, valueOffset, regPtr);
    if (rangeCheck) {
      // Set up a launch pad to allow retry in case of bounds violation */
      launchPad = rawLIR(cUnit, 0, kPseudoIntrinsicRetry, (uintptr_t)info);
      oatInsertGrowableList(cUnit, &cUnit->intrinsicLaunchpads,
                            (intptr_t)launchPad);
      opRegReg(cUnit, kOpCmp, rlIdx.lowReg, regMax);
      oatFreeTemp(cUnit, regMax);
      opCondBranch(cUnit, kCondCs, launchPad);
   }
  } else {
    if (rangeCheck) {
      regMax = oatAllocTemp(cUnit);
      loadWordDisp(cUnit, rlObj.lowReg, countOffset, regMax);
      // Set up a launch pad to allow retry in case of bounds violation */
      launchPad = rawLIR(cUnit, 0, kPseudoIntrinsicRetry, (uintptr_t)info);
      oatInsertGrowableList(cUnit, &cUnit->intrinsicLaunchpads,
                            (intptr_t)launchPad);
      opRegReg(cUnit, kOpCmp, rlIdx.lowReg, regMax);
      oatFreeTemp(cUnit, regMax);
      opCondBranch(cUnit, kCondCc, launchPad);
    }
    regOff = oatAllocTemp(cUnit);
    regPtr = oatAllocTemp(cUnit);
    loadWordDisp(cUnit, rlObj.lowReg, offsetOffset, regOff);
    loadWordDisp(cUnit, rlObj.lowReg, valueOffset, regPtr);
  }
  opRegImm(cUnit, kOpAdd, regPtr, dataOffset);
  opRegReg(cUnit, kOpAdd, regOff, rlIdx.lowReg);
  oatFreeTemp(cUnit, rlObj.lowReg);
  oatFreeTemp(cUnit, rlIdx.lowReg);
  RegLocation rlDest = inlineTarget(cUnit, info);
  RegLocation rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
  loadBaseIndexed(cUnit, regPtr, regOff, rlResult.lowReg, 1, kUnsignedHalf);
  oatFreeTemp(cUnit, regOff);
  oatFreeTemp(cUnit, regPtr);
  storeValue(cUnit, rlDest, rlResult);
  if (rangeCheck) {
    launchPad->operands[2] = 0;  // no resumption
  }
  // Record that we've already inlined & null checked
  info->optFlags |= (MIR_INLINED | MIR_IGNORE_NULL_CHECK);
  return true;
}

// Generates an inlined String.isEmpty or String.length.
bool genInlinedStringIsEmptyOrLength(CompilationUnit* cUnit, CallInfo* info,
                                     bool isEmpty)
{
  if (cUnit->instructionSet == kMips) {
    // TODO - add Mips implementation
    return false;
  }
  // dst = src.length();
  RegLocation rlObj = info->args[0];
  rlObj = loadValue(cUnit, rlObj, kCoreReg);
  RegLocation rlDest = inlineTarget(cUnit, info);
  RegLocation rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
  genNullCheck(cUnit, rlObj.sRegLow, rlObj.lowReg, info->optFlags);
  loadWordDisp(cUnit, rlObj.lowReg, String::CountOffset().Int32Value(),
               rlResult.lowReg);
  if (isEmpty) {
    // dst = (dst == 0);
    if (cUnit->instructionSet == kThumb2) {
      int tReg = oatAllocTemp(cUnit);
      opRegReg(cUnit, kOpNeg, tReg, rlResult.lowReg);
      opRegRegReg(cUnit, kOpAdc, rlResult.lowReg, rlResult.lowReg, tReg);
    } else {
      DCHECK_EQ(cUnit->instructionSet, kX86);
      opRegImm(cUnit, kOpSub, rlResult.lowReg, 1);
      opRegImm(cUnit, kOpLsr, rlResult.lowReg, 31);
    }
  }
  storeValue(cUnit, rlDest, rlResult);
  return true;
}

bool genInlinedAbsInt(CompilationUnit *cUnit, CallInfo* info)
{
  if (cUnit->instructionSet == kMips) {
    // TODO - add Mips implementation
    return false;
  }
  RegLocation rlSrc = info->args[0];
  rlSrc = loadValue(cUnit, rlSrc, kCoreReg);
  RegLocation rlDest = inlineTarget(cUnit, info);
  RegLocation rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
  int signReg = oatAllocTemp(cUnit);
  // abs(x) = y<=x>>31, (x+y)^y.
  opRegRegImm(cUnit, kOpAsr, signReg, rlSrc.lowReg, 31);
  opRegRegReg(cUnit, kOpAdd, rlResult.lowReg, rlSrc.lowReg, signReg);
  opRegReg(cUnit, kOpXor, rlResult.lowReg, signReg);
  storeValue(cUnit, rlDest, rlResult);
  return true;
}

bool genInlinedAbsLong(CompilationUnit *cUnit, CallInfo* info)
{
  if (cUnit->instructionSet == kMips) {
    // TODO - add Mips implementation
    return false;
  }
  if (cUnit->instructionSet == kThumb2) {
    RegLocation rlSrc = info->args[0];
    rlSrc = loadValueWide(cUnit, rlSrc, kCoreReg);
    RegLocation rlDest = inlineTargetWide(cUnit, info);
    RegLocation rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
    int signReg = oatAllocTemp(cUnit);
    // abs(x) = y<=x>>31, (x+y)^y.
    opRegRegImm(cUnit, kOpAsr, signReg, rlSrc.highReg, 31);
    opRegRegReg(cUnit, kOpAdd, rlResult.lowReg, rlSrc.lowReg, signReg);
    opRegRegReg(cUnit, kOpAdc, rlResult.highReg, rlSrc.highReg, signReg);
    opRegReg(cUnit, kOpXor, rlResult.lowReg, signReg);
    opRegReg(cUnit, kOpXor, rlResult.highReg, signReg);
    storeValueWide(cUnit, rlDest, rlResult);
    return true;
  } else {
    DCHECK_EQ(cUnit->instructionSet, kX86);
    // Reuse source registers to avoid running out of temps
    RegLocation rlSrc = info->args[0];
    rlSrc = loadValueWide(cUnit, rlSrc, kCoreReg);
    RegLocation rlDest = inlineTargetWide(cUnit, info);
    RegLocation rlResult = oatEvalLoc(cUnit, rlDest, kCoreReg, true);
    opRegCopyWide(cUnit, rlResult.lowReg, rlResult.highReg, rlSrc.lowReg, rlSrc.highReg);
    oatFreeTemp(cUnit, rlSrc.lowReg);
    oatFreeTemp(cUnit, rlSrc.highReg);
    int signReg = oatAllocTemp(cUnit);
    // abs(x) = y<=x>>31, (x+y)^y.
    opRegRegImm(cUnit, kOpAsr, signReg, rlResult.highReg, 31);
    opRegReg(cUnit, kOpAdd, rlResult.lowReg, signReg);
    opRegReg(cUnit, kOpAdc, rlResult.highReg, signReg);
    opRegReg(cUnit, kOpXor, rlResult.lowReg, signReg);
    opRegReg(cUnit, kOpXor, rlResult.highReg, signReg);
    storeValueWide(cUnit, rlDest, rlResult);
    return true;
  }
}

bool genInlinedFloatCvt(CompilationUnit *cUnit, CallInfo* info)
{
  if (cUnit->instructionSet == kMips) {
    // TODO - add Mips implementation
    return false;
  }
  RegLocation rlSrc = info->args[0];
  RegLocation rlDest = inlineTarget(cUnit, info);
  storeValue(cUnit, rlDest, rlSrc);
  return true;
}

bool genInlinedDoubleCvt(CompilationUnit *cUnit, CallInfo* info)
{
  if (cUnit->instructionSet == kMips) {
    // TODO - add Mips implementation
    return false;
  }
  RegLocation rlSrc = info->args[0];
  RegLocation rlDest = inlineTargetWide(cUnit, info);
  storeValueWide(cUnit, rlDest, rlSrc);
  return true;
}

/*
 * Fast string.indexOf(I) & (II).  Tests for simple case of char <= 0xffff,
 * otherwise bails to standard library code.
 */
bool genInlinedIndexOf(CompilationUnit* cUnit, CallInfo* info,
                       bool zeroBased)
{
  if (cUnit->instructionSet == kMips) {
    // TODO - add Mips implementation
    return false;
  }
  oatClobberCalleeSave(cUnit);
  oatLockCallTemps(cUnit);  // Using fixed registers
  int regPtr = targetReg(kArg0);
  int regChar = targetReg(kArg1);
  int regStart = targetReg(kArg2);

  RegLocation rlObj = info->args[0];
  RegLocation rlChar = info->args[1];
  RegLocation rlStart = info->args[2];
  loadValueDirectFixed(cUnit, rlObj, regPtr);
  loadValueDirectFixed(cUnit, rlChar, regChar);
  if (zeroBased) {
    loadConstant(cUnit, regStart, 0);
  } else {
    loadValueDirectFixed(cUnit, rlStart, regStart);
  }
  int rTgt = (cUnit->instructionSet != kX86) ? loadHelper(cUnit, ENTRYPOINT_OFFSET(pIndexOf)) : 0;
  genNullCheck(cUnit, rlObj.sRegLow, regPtr, info->optFlags);
  LIR* launchPad = rawLIR(cUnit, 0, kPseudoIntrinsicRetry, (uintptr_t)info);
  oatInsertGrowableList(cUnit, &cUnit->intrinsicLaunchpads,
              (intptr_t)launchPad);
  opCmpImmBranch(cUnit, kCondGt, regChar, 0xFFFF, launchPad);
  // NOTE: not a safepoint
  if (cUnit->instructionSet != kX86) {
    opReg(cUnit, kOpBlx, rTgt);
  } else {
    opThreadMem(cUnit, kOpBlx, ENTRYPOINT_OFFSET(pIndexOf));
  }
  LIR* resumeTgt = newLIR0(cUnit, kPseudoTargetLabel);
  launchPad->operands[2] = (uintptr_t)resumeTgt;
  // Record that we've already inlined & null checked
  info->optFlags |= (MIR_INLINED | MIR_IGNORE_NULL_CHECK);
  RegLocation rlReturn = oatGetReturn(cUnit, false);
  RegLocation rlDest = inlineTarget(cUnit, info);
  storeValue(cUnit, rlDest, rlReturn);
  return true;
}

/* Fast string.compareTo(Ljava/lang/string;)I. */
bool genInlinedStringCompareTo(CompilationUnit* cUnit, CallInfo* info)
{
  if (cUnit->instructionSet == kMips) {
    // TODO - add Mips implementation
    return false;
  }
  oatClobberCalleeSave(cUnit);
  oatLockCallTemps(cUnit);  // Using fixed registers
  int regThis = targetReg(kArg0);
  int regCmp = targetReg(kArg1);

  RegLocation rlThis = info->args[0];
  RegLocation rlCmp = info->args[1];
  loadValueDirectFixed(cUnit, rlThis, regThis);
  loadValueDirectFixed(cUnit, rlCmp, regCmp);
  int rTgt = (cUnit->instructionSet != kX86) ?
      loadHelper(cUnit, ENTRYPOINT_OFFSET(pStringCompareTo)) : 0;
  genNullCheck(cUnit, rlThis.sRegLow, regThis, info->optFlags);
  //TUNING: check if rlCmp.sRegLow is already null checked
  LIR* launchPad = rawLIR(cUnit, 0, kPseudoIntrinsicRetry, (uintptr_t)info);
  oatInsertGrowableList(cUnit, &cUnit->intrinsicLaunchpads,
                        (intptr_t)launchPad);
  opCmpImmBranch(cUnit, kCondEq, regCmp, 0, launchPad);
  // NOTE: not a safepoint
  if (cUnit->instructionSet != kX86) {
    opReg(cUnit, kOpBlx, rTgt);
  } else {
    opThreadMem(cUnit, kOpBlx, ENTRYPOINT_OFFSET(pStringCompareTo));
  }
  launchPad->operands[2] = 0;  // No return possible
  // Record that we've already inlined & null checked
  info->optFlags |= (MIR_INLINED | MIR_IGNORE_NULL_CHECK);
  RegLocation rlReturn = oatGetReturn(cUnit, false);
  RegLocation rlDest = inlineTarget(cUnit, info);
  storeValue(cUnit, rlDest, rlReturn);
  return true;
}

bool genIntrinsic(CompilationUnit* cUnit, CallInfo* info)
{
  if (info->optFlags & MIR_INLINED) {
    return false;
  }
  /*
   * TODO: move these to a target-specific structured constant array
   * and use a generic match function.  The list of intrinsics may be
   * slightly different depending on target.
   * TODO: Fold this into a matching function that runs during
   * basic block building.  This should be part of the action for
   * small method inlining and recognition of the special object init
   * method.  By doing this during basic block construction, we can also
   * take advantage of/generate new useful dataflow info.
   */
  std::string tgtMethod(PrettyMethod(info->index, *cUnit->dex_file));
  if (tgtMethod.find(" java.lang") != std::string::npos) {
    if (tgtMethod == "long java.lang.Double.doubleToRawLongBits(double)") {
      return genInlinedDoubleCvt(cUnit, info);
    }
    if (tgtMethod == "double java.lang.Double.longBitsToDouble(long)") {
      return genInlinedDoubleCvt(cUnit, info);
    }
    if (tgtMethod == "int java.lang.Float.floatToRawIntBits(float)") {
      return genInlinedFloatCvt(cUnit, info);
    }
    if (tgtMethod == "float java.lang.Float.intBitsToFloat(int)") {
      return genInlinedFloatCvt(cUnit, info);
    }
    if (tgtMethod == "int java.lang.Math.abs(int)" ||
        tgtMethod == "int java.lang.StrictMath.abs(int)") {
      return genInlinedAbsInt(cUnit, info);
    }
    if (tgtMethod == "long java.lang.Math.abs(long)" ||
        tgtMethod == "long java.lang.StrictMath.abs(long)") {
      return genInlinedAbsLong(cUnit, info);
    }
    if (tgtMethod == "int java.lang.Math.max(int, int)" ||
        tgtMethod == "int java.lang.StrictMath.max(int, int)") {
      return genInlinedMinMaxInt(cUnit, info, false /* isMin */);
    }
    if (tgtMethod == "int java.lang.Math.min(int, int)" ||
        tgtMethod == "int java.lang.StrictMath.min(int, int)") {
      return genInlinedMinMaxInt(cUnit, info, true /* isMin */);
    }
    if (tgtMethod == "double java.lang.Math.sqrt(double)" ||
        tgtMethod == "double java.lang.StrictMath.sqrt(double)") {
      return genInlinedSqrt(cUnit, info);
    }
    if (tgtMethod == "char java.lang.String.charAt(int)") {
      return genInlinedCharAt(cUnit, info);
    }
    if (tgtMethod == "int java.lang.String.compareTo(java.lang.String)") {
      return genInlinedStringCompareTo(cUnit, info);
    }
    if (tgtMethod == "boolean java.lang.String.isEmpty()") {
      return genInlinedStringIsEmptyOrLength(cUnit, info, true /* isEmpty */);
    }
    if (tgtMethod == "int java.lang.String.indexOf(int, int)") {
      return genInlinedIndexOf(cUnit, info, false /* base 0 */);
    }
    if (tgtMethod == "int java.lang.String.indexOf(int)") {
      return genInlinedIndexOf(cUnit, info, true /* base 0 */);
    }
    if (tgtMethod == "int java.lang.String.length()") {
      return genInlinedStringIsEmptyOrLength(cUnit, info, false /* isEmpty */);
    }
  } else if (tgtMethod.find("boolean sun.misc.Unsafe.compareAndSwap") != std::string::npos) {
    if (tgtMethod == "boolean sun.misc.Unsafe.compareAndSwapInt(java.lang.Object, long, int, int)") {
      return genInlinedCas32(cUnit, info, false);
    }
    if (tgtMethod == "boolean sun.misc.Unsafe.compareAndSwapObject(java.lang.Object, long, java.lang.Object, java.lang.Object)") {
      return genInlinedCas32(cUnit, info, true);
    }
  }
  return false;
}


}  // namespace art
