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
#include "../compiler_ir.h"
#include "ralloc_util.h"
#include "codegen_util.h"

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
 * ArgLocs is an array of location records describing the incoming arguments
 * with one location record per word of argument.
 */
void FlushIns(CompilationUnit* cUnit, RegLocation* ArgLocs, RegLocation rlMethod)
{
  /*
   * Dummy up a RegLocation for the incoming Method*
   * It will attempt to keep kArg0 live (or copy it to home location
   * if promoted).
   */
  RegLocation rlSrc = rlMethod;
  rlSrc.location = kLocPhysReg;
  rlSrc.lowReg = TargetReg(kArg0);
  rlSrc.home = false;
  MarkLive(cUnit, rlSrc.lowReg, rlSrc.sRegLow);
  StoreValue(cUnit, rlMethod, rlSrc);
  // If Method* has been promoted, explicitly flush
  if (rlMethod.location == kLocPhysReg) {
    StoreWordDisp(cUnit, TargetReg(kSp), 0, TargetReg(kArg0));
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
      RegLocation* tLoc = &ArgLocs[i];
      if ((vMap->coreLocation == kLocPhysReg) && !tLoc->fp) {
        OpRegCopy(cUnit, vMap->coreReg, TargetReg(argRegs[i]));
        needFlush = false;
      } else if ((vMap->fpLocation == kLocPhysReg) && tLoc->fp) {
        OpRegCopy(cUnit, vMap->FpReg, TargetReg(argRegs[i]));
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
        StoreBaseDisp(cUnit, TargetReg(kSp), SRegOffset(cUnit, startVReg + i),
                      TargetReg(argRegs[i]), kWord);
      }
    } else {
      // If arriving in frame & promoted
      if (vMap->coreLocation == kLocPhysReg) {
        LoadWordDisp(cUnit, TargetReg(kSp), SRegOffset(cUnit, startVReg + i),
                     vMap->coreReg);
      }
      if (vMap->fpLocation == kLocPhysReg) {
        LoadWordDisp(cUnit, TargetReg(kSp), SRegOffset(cUnit, startVReg + i),
                     vMap->FpReg);
      }
    }
  }
}

void ScanMethodLiteralPool(CompilationUnit* cUnit, LIR** methodTarget, LIR** codeTarget,
                           const DexFile* dexFile, uint32_t dexMethodIdx)
{
  LIR* curTarget = cUnit->methodLiteralList;
  LIR* nextTarget = curTarget != NULL ? curTarget->next : NULL;
  while (curTarget != NULL && nextTarget != NULL) {
    if (curTarget->operands[0] == reinterpret_cast<intptr_t>(dexFile) &&
      nextTarget->operands[0] == static_cast<int>(dexMethodIdx)) {
    *codeTarget = curTarget;
    *methodTarget = nextTarget;
    DCHECK((*codeTarget)->next == *methodTarget);
    DCHECK_EQ((*codeTarget)->operands[0], reinterpret_cast<intptr_t>(dexFile));
    DCHECK_EQ((*methodTarget)->operands[0], static_cast<int>(dexMethodIdx));
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
int NextSDCallInsn(CompilationUnit* cUnit, CallInfo* info,
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
      if (directCode != static_cast<unsigned int>(-1)) {
        LoadConstant(cUnit, TargetReg(kInvokeTgt), directCode);
      } else {
        LIR* dataTarget = ScanLiteralPool(cUnit->codeLiteralList, dexIdx, 0);
        if (dataTarget == NULL) {
          dataTarget = AddWordData(cUnit, &cUnit->codeLiteralList, dexIdx);
          dataTarget->operands[1] = type;
        }
        LIR* loadPcRel = OpPcRelLoad(cUnit, TargetReg(kInvokeTgt), dataTarget);
        AppendLIR(cUnit, loadPcRel);
        DCHECK_EQ(cUnit->instructionSet, kThumb2) << reinterpret_cast<void*>(dataTarget);
      }
      if (directMethod != static_cast<unsigned int>(-1)) {
        LoadConstant(cUnit, TargetReg(kArg0), directMethod);
      } else {
        LIR* dataTarget = ScanLiteralPool(cUnit->methodLiteralList, dexIdx, 0);
        if (dataTarget == NULL) {
          dataTarget = AddWordData(cUnit, &cUnit->methodLiteralList, dexIdx);
          dataTarget->operands[1] = type;
        }
        LIR* loadPcRel = OpPcRelLoad(cUnit, TargetReg(kArg0), dataTarget);
        AppendLIR(cUnit, loadPcRel);
        DCHECK_EQ(cUnit->instructionSet, kThumb2) << reinterpret_cast<void*>(dataTarget);
      }
      break;
    default:
      return -1;
    }
  } else {
    switch (state) {
    case 0:  // Get the current Method* [sets kArg0]
      // TUNING: we can save a reg copy if Method* has been promoted.
      LoadCurrMethodDirect(cUnit, TargetReg(kArg0));
      break;
    case 1:  // Get method->dex_cache_resolved_methods_
      LoadWordDisp(cUnit, TargetReg(kArg0),
        AbstractMethod::DexCacheResolvedMethodsOffset().Int32Value(), TargetReg(kArg0));
      // Set up direct code if known.
      if (directCode != 0) {
        if (directCode != static_cast<unsigned int>(-1)) {
          LoadConstant(cUnit, TargetReg(kInvokeTgt), directCode);
        } else {
          LIR* dataTarget = ScanLiteralPool(cUnit->codeLiteralList, dexIdx, 0);
          if (dataTarget == NULL) {
            dataTarget = AddWordData(cUnit, &cUnit->codeLiteralList, dexIdx);
            dataTarget->operands[1] = type;
          }
          LIR* loadPcRel = OpPcRelLoad(cUnit, TargetReg(kInvokeTgt), dataTarget);
          AppendLIR(cUnit, loadPcRel);
          DCHECK_EQ(cUnit->instructionSet, kThumb2) << reinterpret_cast<void*>(dataTarget);
        }
      }
      break;
    case 2:  // Grab target method*
      LoadWordDisp(cUnit, TargetReg(kArg0),
                   Array::DataOffset(sizeof(Object*)).Int32Value() + dexIdx * 4, TargetReg(kArg0));
      break;
    case 3:  // Grab the code from the method*
      if (cUnit->instructionSet != kX86) {
        if (directCode == 0) {
          LoadWordDisp(cUnit, TargetReg(kArg0), AbstractMethod::GetCodeOffset().Int32Value(),
                       TargetReg(kInvokeTgt));
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
 * kArg1 here rather than the standard LoadArgRegs.
 */
int NextVCallInsn(CompilationUnit* cUnit, CallInfo* info,
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
      LoadValueDirectFixed(cUnit, rlArg, TargetReg(kArg1));
      break;
    }
    case 1: // Is "this" null? [use kArg1]
      GenNullCheck(cUnit, info->args[0].sRegLow, TargetReg(kArg1), info->optFlags);
      // get this->klass_ [use kArg1, set kInvokeTgt]
      LoadWordDisp(cUnit, TargetReg(kArg1), Object::ClassOffset().Int32Value(),
                   TargetReg(kInvokeTgt));
      break;
    case 2: // Get this->klass_->vtable [usr kInvokeTgt, set kInvokeTgt]
      LoadWordDisp(cUnit, TargetReg(kInvokeTgt), Class::VTableOffset().Int32Value(),
                   TargetReg(kInvokeTgt));
      break;
    case 3: // Get target method [use kInvokeTgt, set kArg0]
      LoadWordDisp(cUnit, TargetReg(kInvokeTgt), (methodIdx * 4) +
                   Array::DataOffset(sizeof(Object*)).Int32Value(), TargetReg(kArg0));
      break;
    case 4: // Get the compiled code address [uses kArg0, sets kInvokeTgt]
      if (cUnit->instructionSet != kX86) {
        LoadWordDisp(cUnit, TargetReg(kArg0), AbstractMethod::GetCodeOffset().Int32Value(),
                     TargetReg(kInvokeTgt));
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
int NextInterfaceCallInsn(CompilationUnit* cUnit, CallInfo* info, int state,
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
          LoadWordDisp(cUnit, TargetReg(kSelf), trampoline, TargetReg(kInvokeTgt));
        }
        // Get the interface Method* [sets kArg0]
        if (directMethod != static_cast<unsigned int>(-1)) {
          LoadConstant(cUnit, TargetReg(kArg0), directMethod);
        } else {
          LIR* dataTarget = ScanLiteralPool(cUnit->methodLiteralList, dexIdx, 0);
          if (dataTarget == NULL) {
            dataTarget = AddWordData(cUnit, &cUnit->methodLiteralList, dexIdx);
            dataTarget->operands[1] = kInterface;
          }
          LIR* loadPcRel = OpPcRelLoad(cUnit, TargetReg(kArg0), dataTarget);
          AppendLIR(cUnit, loadPcRel);
          DCHECK_EQ(cUnit->instructionSet, kThumb2) << reinterpret_cast<void*>(dataTarget);
        }
        break;
      default:
        return -1;
    }
  } else {
    switch (state) {
      case 0:
        // Get the current Method* [sets kArg0] - TUNING: remove copy of method if it is promoted.
        LoadCurrMethodDirect(cUnit, TargetReg(kArg0));
        // Load the trampoline target [sets kInvokeTgt].
        if (cUnit->instructionSet != kX86) {
          LoadWordDisp(cUnit, TargetReg(kSelf), trampoline, TargetReg(kInvokeTgt));
        }
        break;
    case 1:  // Get method->dex_cache_resolved_methods_ [set/use kArg0]
      LoadWordDisp(cUnit, TargetReg(kArg0),
                   AbstractMethod::DexCacheResolvedMethodsOffset().Int32Value(),
                   TargetReg(kArg0));
      break;
    case 2:  // Grab target method* [set/use kArg0]
      LoadWordDisp(cUnit, TargetReg(kArg0),
                   Array::DataOffset(sizeof(Object*)).Int32Value() + dexIdx * 4,
                   TargetReg(kArg0));
      break;
    default:
      return -1;
    }
  }
  return state + 1;
}

int NextInvokeInsnSP(CompilationUnit* cUnit, CallInfo* info, int trampoline,
                     int state, uint32_t dexIdx, uint32_t methodIdx)
{
  /*
   * This handles the case in which the base method is not fully
   * resolved at compile time, we bail to a runtime helper.
   */
  if (state == 0) {
    if (cUnit->instructionSet != kX86) {
      // Load trampoline target
      LoadWordDisp(cUnit, TargetReg(kSelf), trampoline, TargetReg(kInvokeTgt));
    }
    // Load kArg0 with method index
    LoadConstant(cUnit, TargetReg(kArg0), dexIdx);
    return 1;
  }
  return -1;
}

int NextStaticCallInsnSP(CompilationUnit* cUnit, CallInfo* info,
                         int state, uint32_t dexIdx, uint32_t methodIdx,
                         uintptr_t unused, uintptr_t unused2,
                         InvokeType unused3)
{
  int trampoline = ENTRYPOINT_OFFSET(pInvokeStaticTrampolineWithAccessCheck);
  return NextInvokeInsnSP(cUnit, info, trampoline, state, dexIdx, 0);
}

int NextDirectCallInsnSP(CompilationUnit* cUnit, CallInfo* info, int state,
                         uint32_t dexIdx, uint32_t methodIdx, uintptr_t unused,
                         uintptr_t unused2, InvokeType unused3)
{
  int trampoline = ENTRYPOINT_OFFSET(pInvokeDirectTrampolineWithAccessCheck);
  return NextInvokeInsnSP(cUnit, info, trampoline, state, dexIdx, 0);
}

int NextSuperCallInsnSP(CompilationUnit* cUnit, CallInfo* info, int state,
                        uint32_t dexIdx, uint32_t methodIdx, uintptr_t unused,
                        uintptr_t unused2, InvokeType unused3)
{
  int trampoline = ENTRYPOINT_OFFSET(pInvokeSuperTrampolineWithAccessCheck);
  return NextInvokeInsnSP(cUnit, info, trampoline, state, dexIdx, 0);
}

int NextVCallInsnSP(CompilationUnit* cUnit, CallInfo* info, int state,
                    uint32_t dexIdx, uint32_t methodIdx, uintptr_t unused,
                    uintptr_t unused2, InvokeType unused3)
{
  int trampoline = ENTRYPOINT_OFFSET(pInvokeVirtualTrampolineWithAccessCheck);
  return NextInvokeInsnSP(cUnit, info, trampoline, state, dexIdx, 0);
}

int NextInterfaceCallInsnWithAccessCheck(CompilationUnit* cUnit,
                                         CallInfo* info, int state,
                                         uint32_t dexIdx, uint32_t unused,
                                         uintptr_t unused2, uintptr_t unused3,
                                         InvokeType unused4)
{
  int trampoline = ENTRYPOINT_OFFSET(pInvokeInterfaceTrampolineWithAccessCheck);
  return NextInvokeInsnSP(cUnit, info, trampoline, state, dexIdx, 0);
}

int LoadArgRegs(CompilationUnit* cUnit, CallInfo* info, int callState,
                NextCallInsn nextCallInsn, uint32_t dexIdx,
                uint32_t methodIdx, uintptr_t directCode,
                uintptr_t directMethod, InvokeType type, bool skipThis)
{
  int lastArgReg = TargetReg(kArg3);
  int nextReg = TargetReg(kArg1);
  int nextArg = 0;
  if (skipThis) {
    nextReg++;
    nextArg++;
  }
  for (; (nextReg <= lastArgReg) && (nextArg < info->numArgWords); nextReg++) {
    RegLocation rlArg = info->args[nextArg++];
    rlArg = UpdateRawLoc(cUnit, rlArg);
    if (rlArg.wide && (nextReg <= TargetReg(kArg2))) {
      LoadValueDirectWideFixed(cUnit, rlArg, nextReg, nextReg + 1);
      nextReg++;
      nextArg++;
    } else {
      rlArg.wide = false;
      LoadValueDirectFixed(cUnit, rlArg, nextReg);
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
int GenDalvikArgsNoRange(CompilationUnit* cUnit, CallInfo* info,
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
      rlArg = UpdateLocWide(cUnit, rlUse2);
      if (rlArg.location == kLocPhysReg) {
        reg = rlArg.highReg;
      } else {
        // kArg2 & rArg3 can safely be used here
        reg = TargetReg(kArg3);
        LoadWordDisp(cUnit, TargetReg(kSp), SRegOffset(cUnit, rlArg.sRegLow) + 4, reg);
        callState = nextCallInsn(cUnit, info, callState, dexIdx,
                                 methodIdx, directCode, directMethod, type);
      }
      StoreBaseDisp(cUnit, TargetReg(kSp), (nextUse + 1) * 4, reg, kWord);
      StoreBaseDisp(cUnit, TargetReg(kSp), 16 /* (3+1)*4 */, reg, kWord);
      callState = nextCallInsn(cUnit, info, callState, dexIdx, methodIdx,
                               directCode, directMethod, type);
      nextUse++;
    }
    // Loop through the rest
    while (nextUse < info->numArgWords) {
      int lowReg;
      int highReg = -1;
      rlArg = info->args[nextUse];
      rlArg = UpdateRawLoc(cUnit, rlArg);
      if (rlArg.location == kLocPhysReg) {
        lowReg = rlArg.lowReg;
        highReg = rlArg.highReg;
      } else {
        lowReg = TargetReg(kArg2);
        if (rlArg.wide) {
          highReg = TargetReg(kArg3);
          LoadValueDirectWideFixed(cUnit, rlArg, lowReg, highReg);
        } else {
          LoadValueDirectFixed(cUnit, rlArg, lowReg);
        }
        callState = nextCallInsn(cUnit, info, callState, dexIdx,
                                 methodIdx, directCode, directMethod, type);
      }
      int outsOffset = (nextUse + 1) * 4;
      if (rlArg.wide) {
        StoreBaseDispWide(cUnit, TargetReg(kSp), outsOffset, lowReg, highReg);
        nextUse += 2;
      } else {
        StoreWordDisp(cUnit, TargetReg(kSp), outsOffset, lowReg);
        nextUse++;
      }
      callState = nextCallInsn(cUnit, info, callState, dexIdx, methodIdx,
                               directCode, directMethod, type);
    }
  }

  callState = LoadArgRegs(cUnit, info, callState, nextCallInsn,
                          dexIdx, methodIdx, directCode, directMethod,
                          type, skipThis);

  if (pcrLabel) {
    *pcrLabel = GenNullCheck(cUnit, info->args[0].sRegLow, TargetReg(kArg1), info->optFlags);
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
int GenDalvikArgsRange(CompilationUnit* cUnit, CallInfo* info, int callState,
                       LIR** pcrLabel, NextCallInsn nextCallInsn,
                       uint32_t dexIdx, uint32_t methodIdx,
                       uintptr_t directCode, uintptr_t directMethod,
                       InvokeType type, bool skipThis)
{

  // If we can treat it as non-range (Jumbo ops will use range form)
  if (info->numArgWords <= 5)
    return GenDalvikArgsNoRange(cUnit, info, callState, pcrLabel,
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
      loc = UpdateLocWide(cUnit, loc);
      if ((nextArg >= 2) && (loc.location == kLocPhysReg)) {
        StoreBaseDispWide(cUnit, TargetReg(kSp), SRegOffset(cUnit, loc.sRegLow),
                          loc.lowReg, loc.highReg);
      }
      nextArg += 2;
    } else {
      loc = UpdateLoc(cUnit, loc);
      if ((nextArg >= 3) && (loc.location == kLocPhysReg)) {
        StoreBaseDisp(cUnit, TargetReg(kSp), SRegOffset(cUnit, loc.sRegLow),
                      loc.lowReg, kWord);
      }
      nextArg++;
    }
  }

  int startOffset = SRegOffset(cUnit, info->args[3].sRegLow);
  int outsOffset = 4 /* Method* */ + (3 * 4);
  if (cUnit->instructionSet != kThumb2) {
    // Generate memcpy
    OpRegRegImm(cUnit, kOpAdd, TargetReg(kArg0), TargetReg(kSp), outsOffset);
    OpRegRegImm(cUnit, kOpAdd, TargetReg(kArg1), TargetReg(kSp), startOffset);
    CallRuntimeHelperRegRegImm(cUnit, ENTRYPOINT_OFFSET(pMemcpy), TargetReg(kArg0),
                               TargetReg(kArg1), (info->numArgWords - 3) * 4, false);
  } else {
    if (info->numArgWords >= 20) {
      // Generate memcpy
      OpRegRegImm(cUnit, kOpAdd, TargetReg(kArg0), TargetReg(kSp), outsOffset);
      OpRegRegImm(cUnit, kOpAdd, TargetReg(kArg1), TargetReg(kSp), startOffset);
      CallRuntimeHelperRegRegImm(cUnit, ENTRYPOINT_OFFSET(pMemcpy), TargetReg(kArg0),
                                 TargetReg(kArg1), (info->numArgWords - 3) * 4, false);
    } else {
      // Use vldm/vstm pair using kArg3 as a temp
      int regsLeft = std::min(info->numArgWords - 3, 16);
      callState = nextCallInsn(cUnit, info, callState, dexIdx, methodIdx,
                               directCode, directMethod, type);
      OpRegRegImm(cUnit, kOpAdd, TargetReg(kArg3), TargetReg(kSp), startOffset);
      LIR* ld = OpVldm(cUnit, TargetReg(kArg3), regsLeft);
      //TUNING: loosen barrier
      ld->defMask = ENCODE_ALL;
      SetMemRefType(ld, true /* isLoad */, kDalvikReg);
      callState = nextCallInsn(cUnit, info, callState, dexIdx, methodIdx,
                               directCode, directMethod, type);
      OpRegRegImm(cUnit, kOpAdd, TargetReg(kArg3), TargetReg(kSp), 4 /* Method* */ + (3 * 4));
      callState = nextCallInsn(cUnit, info, callState, dexIdx, methodIdx,
                               directCode, directMethod, type);
      LIR* st = OpVstm(cUnit, TargetReg(kArg3), regsLeft);
      SetMemRefType(st, false /* isLoad */, kDalvikReg);
      st->defMask = ENCODE_ALL;
      callState = nextCallInsn(cUnit, info, callState, dexIdx, methodIdx,
                               directCode, directMethod, type);
    }
  }

  callState = LoadArgRegs(cUnit, info, callState, nextCallInsn,
                          dexIdx, methodIdx, directCode, directMethod,
                          type, skipThis);

  callState = nextCallInsn(cUnit, info, callState, dexIdx, methodIdx,
                           directCode, directMethod, type);
  if (pcrLabel) {
    *pcrLabel = GenNullCheck(cUnit, info->args[0].sRegLow, TargetReg(kArg1),
                             info->optFlags);
  }
  return callState;
}

RegLocation InlineTarget(CompilationUnit* cUnit, CallInfo* info)
{
  RegLocation res;
  if (info->result.location == kLocInvalid) {
    res = GetReturn(cUnit, false);
  } else {
    res = info->result;
  }
  return res;
}

RegLocation InlineTargetWide(CompilationUnit* cUnit, CallInfo* info)
{
  RegLocation res;
  if (info->result.location == kLocInvalid) {
    res = GetReturnWide(cUnit, false);
  } else {
    res = info->result;
  }
  return res;
}

bool GenInlinedCharAt(CompilationUnit* cUnit, CallInfo* info)
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
  rlObj = LoadValue(cUnit, rlObj, kCoreReg);
  rlIdx = LoadValue(cUnit, rlIdx, kCoreReg);
  int regMax;
  GenNullCheck(cUnit, rlObj.sRegLow, rlObj.lowReg, info->optFlags);
  bool rangeCheck = (!(info->optFlags & MIR_IGNORE_RANGE_CHECK));
  LIR* launchPad = NULL;
  int regOff = INVALID_REG;
  int regPtr = INVALID_REG;
  if (cUnit->instructionSet != kX86) {
    regOff = AllocTemp(cUnit);
    regPtr = AllocTemp(cUnit);
    if (rangeCheck) {
      regMax = AllocTemp(cUnit);
      LoadWordDisp(cUnit, rlObj.lowReg, countOffset, regMax);
    }
    LoadWordDisp(cUnit, rlObj.lowReg, offsetOffset, regOff);
    LoadWordDisp(cUnit, rlObj.lowReg, valueOffset, regPtr);
    if (rangeCheck) {
      // Set up a launch pad to allow retry in case of bounds violation */
      launchPad = RawLIR(cUnit, 0, kPseudoIntrinsicRetry, reinterpret_cast<uintptr_t>(info));
      InsertGrowableList(cUnit, &cUnit->intrinsicLaunchpads,
                            reinterpret_cast<uintptr_t>(launchPad));
      OpRegReg(cUnit, kOpCmp, rlIdx.lowReg, regMax);
      FreeTemp(cUnit, regMax);
      OpCondBranch(cUnit, kCondCs, launchPad);
   }
  } else {
    if (rangeCheck) {
      regMax = AllocTemp(cUnit);
      LoadWordDisp(cUnit, rlObj.lowReg, countOffset, regMax);
      // Set up a launch pad to allow retry in case of bounds violation */
      launchPad = RawLIR(cUnit, 0, kPseudoIntrinsicRetry, reinterpret_cast<uintptr_t>(info));
      InsertGrowableList(cUnit, &cUnit->intrinsicLaunchpads,
                            reinterpret_cast<uintptr_t>(launchPad));
      OpRegReg(cUnit, kOpCmp, rlIdx.lowReg, regMax);
      FreeTemp(cUnit, regMax);
      OpCondBranch(cUnit, kCondCc, launchPad);
    }
    regOff = AllocTemp(cUnit);
    regPtr = AllocTemp(cUnit);
    LoadWordDisp(cUnit, rlObj.lowReg, offsetOffset, regOff);
    LoadWordDisp(cUnit, rlObj.lowReg, valueOffset, regPtr);
  }
  OpRegImm(cUnit, kOpAdd, regPtr, dataOffset);
  OpRegReg(cUnit, kOpAdd, regOff, rlIdx.lowReg);
  FreeTemp(cUnit, rlObj.lowReg);
  FreeTemp(cUnit, rlIdx.lowReg);
  RegLocation rlDest = InlineTarget(cUnit, info);
  RegLocation rlResult = EvalLoc(cUnit, rlDest, kCoreReg, true);
  LoadBaseIndexed(cUnit, regPtr, regOff, rlResult.lowReg, 1, kUnsignedHalf);
  FreeTemp(cUnit, regOff);
  FreeTemp(cUnit, regPtr);
  StoreValue(cUnit, rlDest, rlResult);
  if (rangeCheck) {
    launchPad->operands[2] = 0;  // no resumption
  }
  // Record that we've already inlined & null checked
  info->optFlags |= (MIR_INLINED | MIR_IGNORE_NULL_CHECK);
  return true;
}

// Generates an inlined String.isEmpty or String.length.
bool GenInlinedStringIsEmptyOrLength(CompilationUnit* cUnit, CallInfo* info,
                                     bool isEmpty)
{
  if (cUnit->instructionSet == kMips) {
    // TODO - add Mips implementation
    return false;
  }
  // dst = src.length();
  RegLocation rlObj = info->args[0];
  rlObj = LoadValue(cUnit, rlObj, kCoreReg);
  RegLocation rlDest = InlineTarget(cUnit, info);
  RegLocation rlResult = EvalLoc(cUnit, rlDest, kCoreReg, true);
  GenNullCheck(cUnit, rlObj.sRegLow, rlObj.lowReg, info->optFlags);
  LoadWordDisp(cUnit, rlObj.lowReg, String::CountOffset().Int32Value(),
               rlResult.lowReg);
  if (isEmpty) {
    // dst = (dst == 0);
    if (cUnit->instructionSet == kThumb2) {
      int tReg = AllocTemp(cUnit);
      OpRegReg(cUnit, kOpNeg, tReg, rlResult.lowReg);
      OpRegRegReg(cUnit, kOpAdc, rlResult.lowReg, rlResult.lowReg, tReg);
    } else {
      DCHECK_EQ(cUnit->instructionSet, kX86);
      OpRegImm(cUnit, kOpSub, rlResult.lowReg, 1);
      OpRegImm(cUnit, kOpLsr, rlResult.lowReg, 31);
    }
  }
  StoreValue(cUnit, rlDest, rlResult);
  return true;
}

bool GenInlinedAbsInt(CompilationUnit *cUnit, CallInfo* info)
{
  if (cUnit->instructionSet == kMips) {
    // TODO - add Mips implementation
    return false;
  }
  RegLocation rlSrc = info->args[0];
  rlSrc = LoadValue(cUnit, rlSrc, kCoreReg);
  RegLocation rlDest = InlineTarget(cUnit, info);
  RegLocation rlResult = EvalLoc(cUnit, rlDest, kCoreReg, true);
  int signReg = AllocTemp(cUnit);
  // abs(x) = y<=x>>31, (x+y)^y.
  OpRegRegImm(cUnit, kOpAsr, signReg, rlSrc.lowReg, 31);
  OpRegRegReg(cUnit, kOpAdd, rlResult.lowReg, rlSrc.lowReg, signReg);
  OpRegReg(cUnit, kOpXor, rlResult.lowReg, signReg);
  StoreValue(cUnit, rlDest, rlResult);
  return true;
}

bool GenInlinedAbsLong(CompilationUnit *cUnit, CallInfo* info)
{
  if (cUnit->instructionSet == kMips) {
    // TODO - add Mips implementation
    return false;
  }
  if (cUnit->instructionSet == kThumb2) {
    RegLocation rlSrc = info->args[0];
    rlSrc = LoadValueWide(cUnit, rlSrc, kCoreReg);
    RegLocation rlDest = InlineTargetWide(cUnit, info);
    RegLocation rlResult = EvalLoc(cUnit, rlDest, kCoreReg, true);
    int signReg = AllocTemp(cUnit);
    // abs(x) = y<=x>>31, (x+y)^y.
    OpRegRegImm(cUnit, kOpAsr, signReg, rlSrc.highReg, 31);
    OpRegRegReg(cUnit, kOpAdd, rlResult.lowReg, rlSrc.lowReg, signReg);
    OpRegRegReg(cUnit, kOpAdc, rlResult.highReg, rlSrc.highReg, signReg);
    OpRegReg(cUnit, kOpXor, rlResult.lowReg, signReg);
    OpRegReg(cUnit, kOpXor, rlResult.highReg, signReg);
    StoreValueWide(cUnit, rlDest, rlResult);
    return true;
  } else {
    DCHECK_EQ(cUnit->instructionSet, kX86);
    // Reuse source registers to avoid running out of temps
    RegLocation rlSrc = info->args[0];
    rlSrc = LoadValueWide(cUnit, rlSrc, kCoreReg);
    RegLocation rlDest = InlineTargetWide(cUnit, info);
    RegLocation rlResult = EvalLoc(cUnit, rlDest, kCoreReg, true);
    OpRegCopyWide(cUnit, rlResult.lowReg, rlResult.highReg, rlSrc.lowReg, rlSrc.highReg);
    FreeTemp(cUnit, rlSrc.lowReg);
    FreeTemp(cUnit, rlSrc.highReg);
    int signReg = AllocTemp(cUnit);
    // abs(x) = y<=x>>31, (x+y)^y.
    OpRegRegImm(cUnit, kOpAsr, signReg, rlResult.highReg, 31);
    OpRegReg(cUnit, kOpAdd, rlResult.lowReg, signReg);
    OpRegReg(cUnit, kOpAdc, rlResult.highReg, signReg);
    OpRegReg(cUnit, kOpXor, rlResult.lowReg, signReg);
    OpRegReg(cUnit, kOpXor, rlResult.highReg, signReg);
    StoreValueWide(cUnit, rlDest, rlResult);
    return true;
  }
}

bool GenInlinedFloatCvt(CompilationUnit *cUnit, CallInfo* info)
{
  if (cUnit->instructionSet == kMips) {
    // TODO - add Mips implementation
    return false;
  }
  RegLocation rlSrc = info->args[0];
  RegLocation rlDest = InlineTarget(cUnit, info);
  StoreValue(cUnit, rlDest, rlSrc);
  return true;
}

bool GenInlinedDoubleCvt(CompilationUnit *cUnit, CallInfo* info)
{
  if (cUnit->instructionSet == kMips) {
    // TODO - add Mips implementation
    return false;
  }
  RegLocation rlSrc = info->args[0];
  RegLocation rlDest = InlineTargetWide(cUnit, info);
  StoreValueWide(cUnit, rlDest, rlSrc);
  return true;
}

/*
 * Fast string.indexOf(I) & (II).  Tests for simple case of char <= 0xffff,
 * otherwise bails to standard library code.
 */
bool GenInlinedIndexOf(CompilationUnit* cUnit, CallInfo* info,
                       bool zeroBased)
{
  if (cUnit->instructionSet == kMips) {
    // TODO - add Mips implementation
    return false;
  }
  ClobberCalleeSave(cUnit);
  LockCallTemps(cUnit);  // Using fixed registers
  int regPtr = TargetReg(kArg0);
  int regChar = TargetReg(kArg1);
  int regStart = TargetReg(kArg2);

  RegLocation rlObj = info->args[0];
  RegLocation rlChar = info->args[1];
  RegLocation rlStart = info->args[2];
  LoadValueDirectFixed(cUnit, rlObj, regPtr);
  LoadValueDirectFixed(cUnit, rlChar, regChar);
  if (zeroBased) {
    LoadConstant(cUnit, regStart, 0);
  } else {
    LoadValueDirectFixed(cUnit, rlStart, regStart);
  }
  int rTgt = (cUnit->instructionSet != kX86) ? LoadHelper(cUnit, ENTRYPOINT_OFFSET(pIndexOf)) : 0;
  GenNullCheck(cUnit, rlObj.sRegLow, regPtr, info->optFlags);
  LIR* launchPad = RawLIR(cUnit, 0, kPseudoIntrinsicRetry, reinterpret_cast<uintptr_t>(info));
  InsertGrowableList(cUnit, &cUnit->intrinsicLaunchpads, reinterpret_cast<uintptr_t>(launchPad));
  OpCmpImmBranch(cUnit, kCondGt, regChar, 0xFFFF, launchPad);
  // NOTE: not a safepoint
  if (cUnit->instructionSet != kX86) {
    OpReg(cUnit, kOpBlx, rTgt);
  } else {
    OpThreadMem(cUnit, kOpBlx, ENTRYPOINT_OFFSET(pIndexOf));
  }
  LIR* resumeTgt = NewLIR0(cUnit, kPseudoTargetLabel);
  launchPad->operands[2] = reinterpret_cast<uintptr_t>(resumeTgt);
  // Record that we've already inlined & null checked
  info->optFlags |= (MIR_INLINED | MIR_IGNORE_NULL_CHECK);
  RegLocation rlReturn = GetReturn(cUnit, false);
  RegLocation rlDest = InlineTarget(cUnit, info);
  StoreValue(cUnit, rlDest, rlReturn);
  return true;
}

/* Fast string.compareTo(Ljava/lang/string;)I. */
bool GenInlinedStringCompareTo(CompilationUnit* cUnit, CallInfo* info)
{
  if (cUnit->instructionSet == kMips) {
    // TODO - add Mips implementation
    return false;
  }
  ClobberCalleeSave(cUnit);
  LockCallTemps(cUnit);  // Using fixed registers
  int regThis = TargetReg(kArg0);
  int regCmp = TargetReg(kArg1);

  RegLocation rlThis = info->args[0];
  RegLocation rlCmp = info->args[1];
  LoadValueDirectFixed(cUnit, rlThis, regThis);
  LoadValueDirectFixed(cUnit, rlCmp, regCmp);
  int rTgt = (cUnit->instructionSet != kX86) ?
      LoadHelper(cUnit, ENTRYPOINT_OFFSET(pStringCompareTo)) : 0;
  GenNullCheck(cUnit, rlThis.sRegLow, regThis, info->optFlags);
  //TUNING: check if rlCmp.sRegLow is already null checked
  LIR* launchPad = RawLIR(cUnit, 0, kPseudoIntrinsicRetry, reinterpret_cast<uintptr_t>(info));
  InsertGrowableList(cUnit, &cUnit->intrinsicLaunchpads, reinterpret_cast<uintptr_t>(launchPad));
  OpCmpImmBranch(cUnit, kCondEq, regCmp, 0, launchPad);
  // NOTE: not a safepoint
  if (cUnit->instructionSet != kX86) {
    OpReg(cUnit, kOpBlx, rTgt);
  } else {
    OpThreadMem(cUnit, kOpBlx, ENTRYPOINT_OFFSET(pStringCompareTo));
  }
  launchPad->operands[2] = 0;  // No return possible
  // Record that we've already inlined & null checked
  info->optFlags |= (MIR_INLINED | MIR_IGNORE_NULL_CHECK);
  RegLocation rlReturn = GetReturn(cUnit, false);
  RegLocation rlDest = InlineTarget(cUnit, info);
  StoreValue(cUnit, rlDest, rlReturn);
  return true;
}

bool GenIntrinsic(CompilationUnit* cUnit, CallInfo* info)
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
      return GenInlinedDoubleCvt(cUnit, info);
    }
    if (tgtMethod == "double java.lang.Double.longBitsToDouble(long)") {
      return GenInlinedDoubleCvt(cUnit, info);
    }
    if (tgtMethod == "int java.lang.Float.floatToRawIntBits(float)") {
      return GenInlinedFloatCvt(cUnit, info);
    }
    if (tgtMethod == "float java.lang.Float.intBitsToFloat(int)") {
      return GenInlinedFloatCvt(cUnit, info);
    }
    if (tgtMethod == "int java.lang.Math.abs(int)" ||
        tgtMethod == "int java.lang.StrictMath.abs(int)") {
      return GenInlinedAbsInt(cUnit, info);
    }
    if (tgtMethod == "long java.lang.Math.abs(long)" ||
        tgtMethod == "long java.lang.StrictMath.abs(long)") {
      return GenInlinedAbsLong(cUnit, info);
    }
    if (tgtMethod == "int java.lang.Math.max(int, int)" ||
        tgtMethod == "int java.lang.StrictMath.max(int, int)") {
      return GenInlinedMinMaxInt(cUnit, info, false /* isMin */);
    }
    if (tgtMethod == "int java.lang.Math.min(int, int)" ||
        tgtMethod == "int java.lang.StrictMath.min(int, int)") {
      return GenInlinedMinMaxInt(cUnit, info, true /* isMin */);
    }
    if (tgtMethod == "double java.lang.Math.sqrt(double)" ||
        tgtMethod == "double java.lang.StrictMath.sqrt(double)") {
      return GenInlinedSqrt(cUnit, info);
    }
    if (tgtMethod == "char java.lang.String.charAt(int)") {
      return GenInlinedCharAt(cUnit, info);
    }
    if (tgtMethod == "int java.lang.String.compareTo(java.lang.String)") {
      return GenInlinedStringCompareTo(cUnit, info);
    }
    if (tgtMethod == "boolean java.lang.String.isEmpty()") {
      return GenInlinedStringIsEmptyOrLength(cUnit, info, true /* isEmpty */);
    }
    if (tgtMethod == "int java.lang.String.indexOf(int, int)") {
      return GenInlinedIndexOf(cUnit, info, false /* base 0 */);
    }
    if (tgtMethod == "int java.lang.String.indexOf(int)") {
      return GenInlinedIndexOf(cUnit, info, true /* base 0 */);
    }
    if (tgtMethod == "int java.lang.String.length()") {
      return GenInlinedStringIsEmptyOrLength(cUnit, info, false /* isEmpty */);
    }
  } else if (tgtMethod.find("boolean sun.misc.Unsafe.compareAndSwap") != std::string::npos) {
    if (tgtMethod == "boolean sun.misc.Unsafe.compareAndSwapInt(java.lang.Object, long, int, int)") {
      return GenInlinedCas32(cUnit, info, false);
    }
    if (tgtMethod == "boolean sun.misc.Unsafe.compareAndSwapObject(java.lang.Object, long, java.lang.Object, java.lang.Object)") {
      return GenInlinedCas32(cUnit, info, true);
    }
  }
  return false;
}

void GenInvoke(CompilationUnit* cUnit, CallInfo* info)
{
  if (GenIntrinsic(cUnit, info)) {
    return;
  }
  InvokeType originalType = info->type;  // avoiding mutation by ComputeInvokeInfo
  int callState = 0;
  LIR* nullCk;
  LIR** pNullCk = NULL;
  NextCallInsn nextCallInsn;
  FlushAllRegs(cUnit);  /* Everything to home location */
  // Explicit register usage
  LockCallTemps(cUnit);

  OatCompilationUnit mUnit(cUnit->class_loader, cUnit->class_linker,
                           *cUnit->dex_file,
                           cUnit->code_item, cUnit->method_idx,
                           cUnit->access_flags);

  uint32_t dexMethodIdx = info->index;
  int vtableIdx;
  uintptr_t directCode;
  uintptr_t directMethod;
  bool skipThis;
  bool fastPath =
    cUnit->compiler->ComputeInvokeInfo(dexMethodIdx, &mUnit, info->type,
                                       vtableIdx, directCode,
                                       directMethod)
    && !SLOW_INVOKE_PATH;
  if (info->type == kInterface) {
    if (fastPath) {
      pNullCk = &nullCk;
    }
    nextCallInsn = fastPath ? NextInterfaceCallInsn
                            : NextInterfaceCallInsnWithAccessCheck;
    skipThis = false;
  } else if (info->type == kDirect) {
    if (fastPath) {
      pNullCk = &nullCk;
    }
    nextCallInsn = fastPath ? NextSDCallInsn : NextDirectCallInsnSP;
    skipThis = false;
  } else if (info->type == kStatic) {
    nextCallInsn = fastPath ? NextSDCallInsn : NextStaticCallInsnSP;
    skipThis = false;
  } else if (info->type == kSuper) {
    DCHECK(!fastPath);  // Fast path is a direct call.
    nextCallInsn = NextSuperCallInsnSP;
    skipThis = false;
  } else {
    DCHECK_EQ(info->type, kVirtual);
    nextCallInsn = fastPath ? NextVCallInsn : NextVCallInsnSP;
    skipThis = fastPath;
  }
  if (!info->isRange) {
    callState = GenDalvikArgsNoRange(cUnit, info, callState, pNullCk,
                                     nextCallInsn, dexMethodIdx,
                                     vtableIdx, directCode, directMethod,
                                     originalType, skipThis);
  } else {
    callState = GenDalvikArgsRange(cUnit, info, callState, pNullCk,
                                   nextCallInsn, dexMethodIdx, vtableIdx,
                                   directCode, directMethod, originalType,
                                   skipThis);
  }
  // Finish up any of the call sequence not interleaved in arg loading
  while (callState >= 0) {
    callState = nextCallInsn(cUnit, info, callState, dexMethodIdx,
                             vtableIdx, directCode, directMethod,
                             originalType);
  }
  if (cUnit->enableDebug & (1 << kDebugDisplayMissingTargets)) {
    GenShowTarget(cUnit);
  }
  LIR* callInst;
  if (cUnit->instructionSet != kX86) {
    callInst = OpReg(cUnit, kOpBlx, TargetReg(kInvokeTgt));
  } else {
    if (fastPath && info->type != kInterface) {
      callInst = OpMem(cUnit, kOpBlx, TargetReg(kArg0),
                       AbstractMethod::GetCodeOffset().Int32Value());
    } else {
      int trampoline = 0;
      switch (info->type) {
      case kInterface:
        trampoline = fastPath ? ENTRYPOINT_OFFSET(pInvokeInterfaceTrampoline)
            : ENTRYPOINT_OFFSET(pInvokeInterfaceTrampolineWithAccessCheck);
        break;
      case kDirect:
        trampoline = ENTRYPOINT_OFFSET(pInvokeDirectTrampolineWithAccessCheck);
        break;
      case kStatic:
        trampoline = ENTRYPOINT_OFFSET(pInvokeStaticTrampolineWithAccessCheck);
        break;
      case kSuper:
        trampoline = ENTRYPOINT_OFFSET(pInvokeSuperTrampolineWithAccessCheck);
        break;
      case kVirtual:
        trampoline = ENTRYPOINT_OFFSET(pInvokeVirtualTrampolineWithAccessCheck);
        break;
      default:
        LOG(FATAL) << "Unexpected invoke type";
      }
      callInst = OpThreadMem(cUnit, kOpBlx, trampoline);
    }
  }
  MarkSafepointPC(cUnit, callInst);

  ClobberCalleeSave(cUnit);
  if (info->result.location != kLocInvalid) {
    // We have a following MOVE_RESULT - do it now.
    if (info->result.wide) {
      RegLocation retLoc = GetReturnWide(cUnit, info->result.fp);
      StoreValueWide(cUnit, info->result, retLoc);
    } else {
      RegLocation retLoc = GetReturn(cUnit, info->result.fp);
      StoreValue(cUnit, info->result, retLoc);
    }
  }
}

/*
 * Build an array of location records for the incoming arguments.
 * Note: one location record per word of arguments, with dummy
 * high-word loc for wide arguments.  Also pull up any following
 * MOVE_RESULT and incorporate it into the invoke.
 */
CallInfo* NewMemCallInfo(CompilationUnit* cUnit, BasicBlock* bb, MIR* mir,
                         InvokeType type, bool isRange)
{
  CallInfo* info = static_cast<CallInfo*>(NewMem(cUnit, sizeof(CallInfo), true, kAllocMisc));
  MIR* moveResultMIR = FindMoveResult(cUnit, bb, mir);
  if (moveResultMIR == NULL) {
    info->result.location = kLocInvalid;
  } else {
    info->result = GetRawDest(cUnit, moveResultMIR);
    moveResultMIR->dalvikInsn.opcode = Instruction::NOP;
  }
  info->numArgWords = mir->ssaRep->numUses;
  info->args = (info->numArgWords == 0) ? NULL : static_cast<RegLocation*>
      (NewMem(cUnit, sizeof(RegLocation) * info->numArgWords, false, kAllocMisc));
  for (int i = 0; i < info->numArgWords; i++) {
    info->args[i] = GetRawSrc(cUnit, mir, i);
  }
  info->optFlags = mir->optimizationFlags;
  info->type = type;
  info->isRange = isRange;
  info->index = mir->dalvikInsn.vB;
  info->offset = mir->offset;
  return info;
}


}  // namespace art
