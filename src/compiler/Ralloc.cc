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

#include "Dalvik.h"
#include "CompilerInternals.h"
#include "Dataflow.h"
#include "codegen/Ralloc.h"

namespace art {

bool setFp(CompilationUnit* cUnit, int index, bool isFP) {
  bool change = false;
  if (cUnit->regLocation[index].highWord) {
    return change;
  }
  if (isFP && !cUnit->regLocation[index].fp) {
    cUnit->regLocation[index].fp = true;
    cUnit->regLocation[index].defined = true;
    change = true;
  }
  return change;
}

bool setCore(CompilationUnit* cUnit, int index, bool isCore) {
  bool change = false;
  if (cUnit->regLocation[index].highWord) {
    return change;
  }
  if (isCore && !cUnit->regLocation[index].defined) {
    cUnit->regLocation[index].core = true;
    cUnit->regLocation[index].defined = true;
    change = true;
  }
  return change;
}

bool setRef(CompilationUnit* cUnit, int index, bool isRef) {
  bool change = false;
  if (cUnit->regLocation[index].highWord) {
    return change;
  }
  if (isRef && !cUnit->regLocation[index].defined) {
    cUnit->regLocation[index].ref = true;
    cUnit->regLocation[index].defined = true;
    change = true;
  }
  return change;
}

bool remapNames(CompilationUnit* cUnit, BasicBlock* bb)
{
  if (bb->blockType != kDalvikByteCode && bb->blockType != kEntryBlock &&
      bb->blockType != kExitBlock)
    return false;

  for (MIR* mir = bb->firstMIRInsn; mir; mir = mir->next) {
    SSARepresentation *ssaRep = mir->ssaRep;
    if (ssaRep) {
      for (int i = 0; i < ssaRep->numUses; i++) {
        ssaRep->uses[i] = cUnit->phiAliasMap[ssaRep->uses[i]];
      }
      for (int i = 0; i < ssaRep->numDefs; i++) {
        ssaRep->defs[i] = cUnit->phiAliasMap[ssaRep->defs[i]];
      }
    }
  }
  return false;
}

// Try to find the next move result which might have an FP target
SSARepresentation* findFPMoveResult(MIR* mir)
{
  SSARepresentation* res = NULL;
  for (; mir; mir = mir->next) {
    if ((mir->dalvikInsn.opcode == Instruction::MOVE_RESULT) ||
        (mir->dalvikInsn.opcode == Instruction::MOVE_RESULT_WIDE)) {
      res = mir->ssaRep;
      break;
    }
  }
  return res;
}

/*
 * Infer types and sizes.  We don't need to track change on sizes,
 * as it doesn't propagate.  We're guaranteed at least one pass through
 * the cfg.
 */
bool inferTypeAndSize(CompilationUnit* cUnit, BasicBlock* bb)
{
  MIR *mir;
  bool changed = false;   // Did anything change?

  if (bb->dataFlowInfo == NULL) return false;
  if (bb->blockType != kDalvikByteCode && bb->blockType != kEntryBlock)
    return false;

  for (mir = bb->firstMIRInsn; mir; mir = mir->next) {
    SSARepresentation *ssaRep = mir->ssaRep;
    if (ssaRep) {
      int attrs = oatDataFlowAttributes[mir->dalvikInsn.opcode];

      // Handle defs
      if (attrs & DF_DA) {
        if (attrs & DF_CORE_A) {
          changed |= setCore(cUnit, ssaRep->defs[0], true);
        }
        if (attrs & DF_REF_A) {
          changed |= setRef(cUnit, ssaRep->defs[0], true);
        }
        if (attrs & DF_A_WIDE) {
          cUnit->regLocation[ssaRep->defs[0]].wide = true;
          cUnit->regLocation[ssaRep->defs[1]].highWord = true;
          DCHECK_EQ(SRegToVReg(cUnit, ssaRep->defs[0])+1,
          SRegToVReg(cUnit, ssaRep->defs[1]));
        }
      }

      // Handles uses
      int next = 0;
      if (attrs & DF_UA) {
        if (attrs & DF_CORE_A) {
          changed |= setCore(cUnit, ssaRep->uses[next], true);
        }
        if (attrs & DF_REF_A) {
          changed |= setRef(cUnit, ssaRep->uses[next], true);
        }
        if (attrs & DF_A_WIDE) {
          cUnit->regLocation[ssaRep->uses[next]].wide = true;
          cUnit->regLocation[ssaRep->uses[next + 1]].highWord = true;
          DCHECK_EQ(SRegToVReg(cUnit, ssaRep->uses[next])+1,
          SRegToVReg(cUnit, ssaRep->uses[next + 1]));
          next += 2;
        } else {
          next++;
        }
      }
      if (attrs & DF_UB) {
        if (attrs & DF_CORE_B) {
          changed |= setCore(cUnit, ssaRep->uses[next], true);
        }
        if (attrs & DF_REF_B) {
          changed |= setRef(cUnit, ssaRep->uses[next], true);
        }
        if (attrs & DF_B_WIDE) {
          cUnit->regLocation[ssaRep->uses[next]].wide = true;
          cUnit->regLocation[ssaRep->uses[next + 1]].highWord = true;
          DCHECK_EQ(SRegToVReg(cUnit, ssaRep->uses[next])+1,
                               SRegToVReg(cUnit, ssaRep->uses[next + 1]));
          next += 2;
        } else {
          next++;
        }
      }
      if (attrs & DF_UC) {
        if (attrs & DF_CORE_C) {
          changed |= setCore(cUnit, ssaRep->uses[next], true);
        }
        if (attrs & DF_REF_C) {
          changed |= setRef(cUnit, ssaRep->uses[next], true);
        }
        if (attrs & DF_C_WIDE) {
          cUnit->regLocation[ssaRep->uses[next]].wide = true;
          cUnit->regLocation[ssaRep->uses[next + 1]].highWord = true;
          DCHECK_EQ(SRegToVReg(cUnit, ssaRep->uses[next])+1,
          SRegToVReg(cUnit, ssaRep->uses[next + 1]));
        }
      }

      // Special-case return handling
      if ((mir->dalvikInsn.opcode == Instruction::RETURN) ||
          (mir->dalvikInsn.opcode == Instruction::RETURN_WIDE) ||
          (mir->dalvikInsn.opcode == Instruction::RETURN_OBJECT)) {
        switch(cUnit->shorty[0]) {
            case 'I':
              changed |= setCore(cUnit, ssaRep->uses[0], true);
              break;
            case 'J':
              changed |= setCore(cUnit, ssaRep->uses[0], true);
              changed |= setCore(cUnit, ssaRep->uses[1], true);
              cUnit->regLocation[ssaRep->uses[0]].wide = true;
              cUnit->regLocation[ssaRep->uses[1]].highWord = true;
              break;
            case 'F':
              changed |= setFp(cUnit, ssaRep->uses[0], true);
              break;
            case 'D':
              changed |= setFp(cUnit, ssaRep->uses[0], true);
              changed |= setFp(cUnit, ssaRep->uses[1], true);
              cUnit->regLocation[ssaRep->uses[0]].wide = true;
              cUnit->regLocation[ssaRep->uses[1]].highWord = true;
              break;
            case 'L':
              changed |= setRef(cUnit, ssaRep->uses[0], true);
              break;
            default: break;
        }
      }

      // Special-case handling for format 35c/3rc invokes
      Instruction::Code opcode = mir->dalvikInsn.opcode;
      int flags = (static_cast<int>(opcode) >= kNumPackedOpcodes)
          ? 0 : Instruction::Flags(mir->dalvikInsn.opcode);
      if ((flags & Instruction::kInvoke) &&
          (attrs & (DF_FORMAT_35C | DF_FORMAT_3RC))) {
        DCHECK_EQ(next, 0);
        int target_idx = mir->dalvikInsn.vB;
        const char* shorty = oatGetShortyFromTargetIdx(cUnit, target_idx);
        // Handle result type if floating point
        if ((shorty[0] == 'F') || (shorty[0] == 'D')) {
          // Find move-result that consumes this result
          SSARepresentation* tgtRep = findFPMoveResult(mir->next);
          // Might be in next basic block
          if (!tgtRep) {
            tgtRep = findFPMoveResult(bb->fallThrough->firstMIRInsn);
          }
          // Result might not be used at all, so no move-result
          if (tgtRep) {
            tgtRep->fpDef[0] = true;
            changed |= setFp(cUnit, tgtRep->defs[0], true);
            if (shorty[0] == 'D') {
              tgtRep->fpDef[1] = true;
              changed |= setFp(cUnit, tgtRep->defs[1], true);
            }
          }
        }
        int numUses = mir->dalvikInsn.vA;
        // If this is a non-static invoke, mark implicit "this"
        if (((mir->dalvikInsn.opcode != Instruction::INVOKE_STATIC) &&
            (mir->dalvikInsn.opcode != Instruction::INVOKE_STATIC_RANGE))) {
          cUnit->regLocation[ssaRep->uses[next]].defined = true;
          cUnit->regLocation[ssaRep->uses[next]].ref = true;
          next++;
        }
        uint32_t cpos = 1;
        if (strlen(shorty) > 1) {
          for (int i = next; i < numUses;) {
            DCHECK_LT(cpos, strlen(shorty));
            switch (shorty[cpos++]) {
              case 'D':
                ssaRep->fpUse[i] = true;
                ssaRep->fpUse[i+1] = true;
                cUnit->regLocation[ssaRep->uses[i]].wide = true;
                cUnit->regLocation[ssaRep->uses[i+1]].highWord = true;
                DCHECK_EQ(SRegToVReg(cUnit, ssaRep->uses[i])+1,
                                     SRegToVReg(cUnit, ssaRep->uses[i+1]));
                i++;
                break;
              case 'J':
                cUnit->regLocation[ssaRep->uses[i]].wide = true;
                cUnit->regLocation[ssaRep->uses[i+1]].highWord = true;
                DCHECK_EQ(SRegToVReg(cUnit, ssaRep->uses[i])+1,
                                     SRegToVReg(cUnit, ssaRep->uses[i+1]));
                changed |= setCore(cUnit, ssaRep->uses[i],true);
                i++;
                break;
              case 'F':
                ssaRep->fpUse[i] = true;
                break;
              case 'L':
                changed |= setRef(cUnit,ssaRep->uses[i], true);
                break;
              default:
                changed |= setCore(cUnit,ssaRep->uses[i], true);
                break;
            }
            i++;
          }
        }
      }

      for (int i=0; ssaRep->fpUse && i< ssaRep->numUses; i++) {
        if (ssaRep->fpUse[i])
          changed |= setFp(cUnit, ssaRep->uses[i], true);
        }
      for (int i=0; ssaRep->fpDef && i< ssaRep->numDefs; i++) {
        if (ssaRep->fpDef[i])
          changed |= setFp(cUnit, ssaRep->defs[i], true);
        }
      // Special-case handling for moves & Phi
      if (attrs & (DF_IS_MOVE | DF_NULL_TRANSFER_N)) {
        // If any of our inputs or outputs is defined, set all
        bool definedFP = false;
        bool definedCore = false;
        bool definedRef = false;
        definedFP |= (cUnit->regLocation[ssaRep->defs[0]].defined &&
                      cUnit->regLocation[ssaRep->defs[0]].fp);
        definedCore |= (cUnit->regLocation[ssaRep->defs[0]].defined &&
                        cUnit->regLocation[ssaRep->defs[0]].core);
        definedRef |= (cUnit->regLocation[ssaRep->defs[0]].defined &&
                       cUnit->regLocation[ssaRep->defs[0]].ref);
        for (int i = 0; i < ssaRep->numUses; i++) {
          definedFP |= (cUnit->regLocation[ssaRep->uses[i]].defined &&
                        cUnit->regLocation[ssaRep->uses[i]].fp);
          definedCore |= (cUnit->regLocation[ssaRep->uses[i]].defined
                          && cUnit->regLocation[ssaRep->uses[i]].core);
          definedRef |= (cUnit->regLocation[ssaRep->uses[i]].defined
                         && cUnit->regLocation[ssaRep->uses[i]].ref);
        }
        /*
         * TODO: cleaner fix
         * We don't normally expect to see a Dalvik register
         * definition used both as a floating point and core
         * value.  However, the instruction rewriting that occurs
         * during verification can eliminate some type information,
         * leaving us confused.  The real fix here is either to
         * add explicit type information to Dalvik byte codes,
         * or to recognize THROW_VERIFICATION_ERROR as
         * an unconditional branch and support dead code elimination.
         * As a workaround we can detect this situation and
         * disable register promotion (which is the only thing that
         * relies on distinctions between core and fp usages.
         */
        if ((definedFP && (definedCore | definedRef)) &&
            ((cUnit->disableOpt & (1 << kPromoteRegs)) == 0)) {
          LOG(WARNING) << PrettyMethod(cUnit->method_idx, *cUnit->dex_file)
                       << " op at block " << bb->id
                       << " has both fp and core/ref uses for same def.";
          cUnit->disableOpt |= (1 << kPromoteRegs);
        }
        changed |= setFp(cUnit, ssaRep->defs[0], definedFP);
        changed |= setCore(cUnit, ssaRep->defs[0], definedCore);
        changed |= setRef(cUnit, ssaRep->defs[0], definedRef);
        for (int i = 0; i < ssaRep->numUses; i++) {
         changed |= setFp(cUnit, ssaRep->uses[i], definedFP);
         changed |= setCore(cUnit, ssaRep->uses[i], definedCore);
         changed |= setRef(cUnit, ssaRep->uses[i], definedRef);
        }
      }
    }
  }
  return changed;
}

static const char* storageName[] = {" Frame ", "PhysReg", " Spill "};

void oatDumpRegLocTable(RegLocation* table, int count)
{
  for (int i = 0; i < count; i++) {
    LOG(INFO) << StringPrintf("Loc[%02d] : %s, %c %c %c %c %c %c%d %c%d S%d",
        i, storageName[table[i].location], table[i].wide ? 'W' : 'N',
        table[i].defined ? 'D' : 'U',
        table[i].fp ? 'F' : table[i].ref ? 'R' :'C',
        table[i].highWord ? 'H' : 'L', table[i].home ? 'h' : 't',
        oatIsFpReg(table[i].lowReg) ? 's' : 'r',
        table[i].lowReg & oatFpRegMask(),
        oatIsFpReg(table[i].highReg) ? 's' : 'r',
        table[i].highReg & oatFpRegMask(), table[i].sRegLow);
  }
}

void oatDumpRegLoc(RegLocation loc) {
  oatDumpRegLocTable(&loc, 1);
}

static const RegLocation freshLoc = {kLocDalvikFrame, 0, 0, 0, 0, 0, 0, 0, 0,
                                     INVALID_REG, INVALID_REG, INVALID_SREG,
                                     INVALID_SREG};

int oatComputeFrameSize(CompilationUnit* cUnit) {
  /* Figure out the frame size */
  static const uint32_t kAlignMask = kStackAlignment - 1;
  uint32_t size = (cUnit->numCoreSpills + cUnit->numFPSpills +
                   1 /* filler word */ + cUnit->numRegs + cUnit->numOuts +
                   cUnit->numCompilerTemps + 1 /* curMethod* */)
                   * sizeof(uint32_t);
  /* Align and set */
  return (size + kAlignMask) & ~(kAlignMask);
}

/*
 * Simple register allocation.  Some Dalvik virtual registers may
 * be promoted to physical registers.  Most of the work for temp
 * allocation is done on the fly.  We also do some initialization and
 * type inference here.
 */
void oatSimpleRegAlloc(CompilationUnit* cUnit)
{
  int i;
  RegLocation* loc;

  /* Allocate the location map */
  loc = (RegLocation*)oatNew(cUnit, cUnit->numSSARegs * sizeof(*loc), true,
                             kAllocRegAlloc);
  for (i=0; i< cUnit->numSSARegs; i++) {
    loc[i] = freshLoc;
    loc[i].sRegLow = i;
    loc[i].isConst = oatIsBitSet(cUnit->isConstantV, i);
  }

  /* Patch up the locations for Method* and the compiler temps */
  loc[cUnit->methodSReg].location = kLocCompilerTemp;
  loc[cUnit->methodSReg].defined = true;
  for (i = 0; i < cUnit->numCompilerTemps; i++) {
    CompilerTemp* ct = (CompilerTemp*)cUnit->compilerTemps.elemList[i];
    loc[ct->sReg].location = kLocCompilerTemp;
    loc[ct->sReg].defined = true;
  }

  cUnit->regLocation = loc;

  /* Allocation the promotion map */
  int numRegs = cUnit->numDalvikRegisters;
  cUnit->promotionMap =
      (PromotionMap*)oatNew(cUnit, (numRegs + cUnit->numCompilerTemps + 1) *
                            sizeof(cUnit->promotionMap[0]), true,
                            kAllocRegAlloc);

  /* Add types of incoming arguments based on signature */
  int numIns = cUnit->numIns;
  if (numIns > 0) {
    int sReg = numRegs - numIns;
    if ((cUnit->access_flags & kAccStatic) == 0) {
      // For non-static, skip past "this"
      cUnit->regLocation[sReg].defined = true;
      cUnit->regLocation[sReg].ref = true;
      sReg++;
    }
    const char* shorty = cUnit->shorty;
    int shorty_len = strlen(shorty);
    for (int i = 1; i < shorty_len; i++) {
      switch (shorty[i]) {
        case 'D':
          cUnit->regLocation[sReg].wide = true;
          cUnit->regLocation[sReg+1].highWord = true;
          cUnit->regLocation[sReg+1].fp = true;
          DCHECK_EQ(SRegToVReg(cUnit, sReg)+1, SRegToVReg(cUnit, sReg+1));
          cUnit->regLocation[sReg].fp = true;
          cUnit->regLocation[sReg].defined = true;
          sReg++;
          break;
        case 'J':
          cUnit->regLocation[sReg].wide = true;
          cUnit->regLocation[sReg+1].highWord = true;
          DCHECK_EQ(SRegToVReg(cUnit, sReg)+1, SRegToVReg(cUnit, sReg+1));
          cUnit->regLocation[sReg].core = true;
          cUnit->regLocation[sReg].defined = true;
          sReg++;
          break;
        case 'F':
          cUnit->regLocation[sReg].fp = true;
          cUnit->regLocation[sReg].defined = true;
          break;
        case 'L':
          cUnit->regLocation[sReg].ref = true;
          cUnit->regLocation[sReg].defined = true;
          break;
        default:
          cUnit->regLocation[sReg].core = true;
          cUnit->regLocation[sReg].defined = true;
          break;
        }
        sReg++;
      }
  }

#if defined(ART_USE_QUICK_COMPILER)
  if (!cUnit->genBitcode) {
    /* Remap names */
    oatDataFlowAnalysisDispatcher(cUnit, remapNames,
                                  kPreOrderDFSTraversal,
                                  false /* isIterative */);
  }
#else
  /* Remap names */
  oatDataFlowAnalysisDispatcher(cUnit, remapNames,
                                kPreOrderDFSTraversal,
                                false /* isIterative */);
#endif

  /* Do type & size inference pass */
  oatDataFlowAnalysisDispatcher(cUnit, inferTypeAndSize,
                                kPreOrderDFSTraversal,
                                true /* isIterative */);

  /*
   * Set the sRegLow field to refer to the pre-SSA name of the
   * base Dalvik virtual register.  Once we add a better register
   * allocator, remove this remapping.
   */
  for (i=0; i < cUnit->numSSARegs; i++) {
    if (cUnit->regLocation[i].location != kLocCompilerTemp) {
      int origSReg = cUnit->regLocation[i].sRegLow;
      cUnit->regLocation[i].origSReg = origSReg;
      cUnit->regLocation[i].sRegLow = SRegToVReg(cUnit, origSReg);
    }
  }

  cUnit->coreSpillMask = 0;
  cUnit->fpSpillMask = 0;
  cUnit->numCoreSpills = 0;

  oatDoPromotion(cUnit);

  /* Get easily-accessable post-promotion copy of RegLocation for Method* */
  cUnit->methodLoc = cUnit->regLocation[cUnit->methodSReg];

  if (cUnit->printMe && !(cUnit->disableOpt & (1 << kPromoteRegs))) {
    LOG(INFO) << "After Promotion";
    oatDumpRegLocTable(cUnit->regLocation, cUnit->numSSARegs);
  }

  /* Set the frame size */
  cUnit->frameSize = oatComputeFrameSize(cUnit);
}

}  // namespace art
