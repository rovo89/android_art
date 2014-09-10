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

#include "dex/compiler_ir.h"
#include "dex/frontend.h"
#include "dex/quick/dex_file_method_inliner.h"
#include "dex/quick/dex_file_to_method_inliner_map.h"
#include "dex_file-inl.h"
#include "entrypoints/quick/quick_entrypoints.h"
#include "invoke_type.h"
#include "mirror/array.h"
#include "mirror/class-inl.h"
#include "mirror/dex_cache.h"
#include "mirror/object_array-inl.h"
#include "mirror/reference-inl.h"
#include "mirror/string.h"
#include "mir_to_lir-inl.h"
#include "scoped_thread_state_change.h"
#include "x86/codegen_x86.h"

namespace art {

// Shortcuts to repeatedly used long types.
typedef mirror::ObjectArray<mirror::Object> ObjArray;

/*
 * This source files contains "gen" codegen routines that should
 * be applicable to most targets.  Only mid-level support utilities
 * and "op" calls may be used here.
 */

void Mir2Lir::AddIntrinsicSlowPath(CallInfo* info, LIR* branch, LIR* resume) {
  class IntrinsicSlowPathPath : public Mir2Lir::LIRSlowPath {
   public:
    IntrinsicSlowPathPath(Mir2Lir* m2l, CallInfo* info, LIR* branch, LIR* resume = nullptr)
        : LIRSlowPath(m2l, info->offset, branch, resume), info_(info) {
    }

    void Compile() {
      m2l_->ResetRegPool();
      m2l_->ResetDefTracking();
      GenerateTargetLabel(kPseudoIntrinsicRetry);
      // NOTE: GenInvokeNoInline() handles MarkSafepointPC.
      m2l_->GenInvokeNoInline(info_);
      if (cont_ != nullptr) {
        m2l_->OpUnconditionalBranch(cont_);
      }
    }

   private:
    CallInfo* const info_;
  };

  AddSlowPath(new (arena_) IntrinsicSlowPathPath(this, info, branch, resume));
}

/*
 * To save scheduling time, helper calls are broken into two parts: generation of
 * the helper target address, and the actual call to the helper.  Because x86
 * has a memory call operation, part 1 is a NOP for x86.  For other targets,
 * load arguments between the two parts.
 */
// template <size_t pointer_size>
RegStorage Mir2Lir::CallHelperSetup(QuickEntrypointEnum trampoline) {
  if (cu_->instruction_set == kX86 || cu_->instruction_set == kX86_64) {
    return RegStorage::InvalidReg();
  } else {
    return LoadHelper(trampoline);
  }
}

LIR* Mir2Lir::CallHelper(RegStorage r_tgt, QuickEntrypointEnum trampoline, bool safepoint_pc,
                         bool use_link) {
  LIR* call_inst = InvokeTrampoline(use_link ? kOpBlx : kOpBx, r_tgt, trampoline);

  if (r_tgt.Valid()) {
    FreeTemp(r_tgt);
  }

  if (safepoint_pc) {
    MarkSafepointPC(call_inst);
  }
  return call_inst;
}

void Mir2Lir::CallRuntimeHelper(QuickEntrypointEnum trampoline, bool safepoint_pc) {
  RegStorage r_tgt = CallHelperSetup(trampoline);
  ClobberCallerSave();
  CallHelper(r_tgt, trampoline, safepoint_pc);
}

void Mir2Lir::CallRuntimeHelperImm(QuickEntrypointEnum trampoline, int arg0, bool safepoint_pc) {
  RegStorage r_tgt = CallHelperSetup(trampoline);
  LoadConstant(TargetReg(kArg0, kNotWide), arg0);
  ClobberCallerSave();
  CallHelper(r_tgt, trampoline, safepoint_pc);
}

void Mir2Lir::CallRuntimeHelperReg(QuickEntrypointEnum trampoline, RegStorage arg0,
                                   bool safepoint_pc) {
  RegStorage r_tgt = CallHelperSetup(trampoline);
  OpRegCopy(TargetReg(kArg0, arg0.GetWideKind()), arg0);
  ClobberCallerSave();
  CallHelper(r_tgt, trampoline, safepoint_pc);
}

void Mir2Lir::CallRuntimeHelperRegLocation(QuickEntrypointEnum trampoline, RegLocation arg0,
                                           bool safepoint_pc) {
  RegStorage r_tgt = CallHelperSetup(trampoline);
  if (arg0.wide == 0) {
    LoadValueDirectFixed(arg0, TargetReg(arg0.fp ? kFArg0 : kArg0, arg0));
  } else {
    LoadValueDirectWideFixed(arg0, TargetReg(arg0.fp ? kFArg0 : kArg0, kWide));
  }
  ClobberCallerSave();
  CallHelper(r_tgt, trampoline, safepoint_pc);
}

void Mir2Lir::CallRuntimeHelperImmImm(QuickEntrypointEnum trampoline, int arg0, int arg1,
                                      bool safepoint_pc) {
  RegStorage r_tgt = CallHelperSetup(trampoline);
  LoadConstant(TargetReg(kArg0, kNotWide), arg0);
  LoadConstant(TargetReg(kArg1, kNotWide), arg1);
  ClobberCallerSave();
  CallHelper(r_tgt, trampoline, safepoint_pc);
}

void Mir2Lir::CallRuntimeHelperImmRegLocation(QuickEntrypointEnum trampoline, int arg0,
                                              RegLocation arg1, bool safepoint_pc) {
  RegStorage r_tgt = CallHelperSetup(trampoline);
  if (arg1.wide == 0) {
    LoadValueDirectFixed(arg1, TargetReg(kArg1, arg1));
  } else {
    RegStorage r_tmp = TargetReg(cu_->instruction_set == kMips ? kArg2 : kArg1, kWide);
    LoadValueDirectWideFixed(arg1, r_tmp);
  }
  LoadConstant(TargetReg(kArg0, kNotWide), arg0);
  ClobberCallerSave();
  CallHelper(r_tgt, trampoline, safepoint_pc);
}

void Mir2Lir::CallRuntimeHelperRegLocationImm(QuickEntrypointEnum trampoline, RegLocation arg0,
                                              int arg1, bool safepoint_pc) {
  RegStorage r_tgt = CallHelperSetup(trampoline);
  DCHECK(!arg0.wide);
  LoadValueDirectFixed(arg0, TargetReg(kArg0, arg0));
  LoadConstant(TargetReg(kArg1, kNotWide), arg1);
  ClobberCallerSave();
  CallHelper(r_tgt, trampoline, safepoint_pc);
}

void Mir2Lir::CallRuntimeHelperImmReg(QuickEntrypointEnum trampoline, int arg0, RegStorage arg1,
                                      bool safepoint_pc) {
  RegStorage r_tgt = CallHelperSetup(trampoline);
  OpRegCopy(TargetReg(kArg1, arg1.GetWideKind()), arg1);
  LoadConstant(TargetReg(kArg0, kNotWide), arg0);
  ClobberCallerSave();
  CallHelper(r_tgt, trampoline, safepoint_pc);
}

void Mir2Lir::CallRuntimeHelperRegImm(QuickEntrypointEnum trampoline, RegStorage arg0, int arg1,
                                      bool safepoint_pc) {
  RegStorage r_tgt = CallHelperSetup(trampoline);
  OpRegCopy(TargetReg(kArg0, arg0.GetWideKind()), arg0);
  LoadConstant(TargetReg(kArg1, kNotWide), arg1);
  ClobberCallerSave();
  CallHelper(r_tgt, trampoline, safepoint_pc);
}

void Mir2Lir::CallRuntimeHelperImmMethod(QuickEntrypointEnum trampoline, int arg0,
                                         bool safepoint_pc) {
  RegStorage r_tgt = CallHelperSetup(trampoline);
  LoadCurrMethodDirect(TargetReg(kArg1, kRef));
  LoadConstant(TargetReg(kArg0, kNotWide), arg0);
  ClobberCallerSave();
  CallHelper(r_tgt, trampoline, safepoint_pc);
}

void Mir2Lir::CallRuntimeHelperRegMethod(QuickEntrypointEnum trampoline, RegStorage arg0,
                                         bool safepoint_pc) {
  RegStorage r_tgt = CallHelperSetup(trampoline);
  DCHECK(!IsSameReg(TargetReg(kArg1, arg0.GetWideKind()), arg0));
  RegStorage r_tmp = TargetReg(kArg0, arg0.GetWideKind());
  if (r_tmp.NotExactlyEquals(arg0)) {
    OpRegCopy(r_tmp, arg0);
  }
  LoadCurrMethodDirect(TargetReg(kArg1, kRef));
  ClobberCallerSave();
  CallHelper(r_tgt, trampoline, safepoint_pc);
}

void Mir2Lir::CallRuntimeHelperRegMethodRegLocation(QuickEntrypointEnum trampoline, RegStorage arg0,
                                                    RegLocation arg2, bool safepoint_pc) {
  RegStorage r_tgt = CallHelperSetup(trampoline);
  DCHECK(!IsSameReg(TargetReg(kArg1, arg0.GetWideKind()), arg0));
  RegStorage r_tmp = TargetReg(kArg0, arg0.GetWideKind());
  if (r_tmp.NotExactlyEquals(arg0)) {
    OpRegCopy(r_tmp, arg0);
  }
  LoadCurrMethodDirect(TargetReg(kArg1, kRef));
  LoadValueDirectFixed(arg2, TargetReg(kArg2, arg2));
  ClobberCallerSave();
  CallHelper(r_tgt, trampoline, safepoint_pc);
}

void Mir2Lir::CallRuntimeHelperRegLocationRegLocation(QuickEntrypointEnum trampoline,
                                                      RegLocation arg0, RegLocation arg1,
                                                      bool safepoint_pc) {
  RegStorage r_tgt = CallHelperSetup(trampoline);
  if (cu_->instruction_set == kArm64 || cu_->instruction_set == kX86_64) {
    RegStorage arg0_reg = TargetReg((arg0.fp) ? kFArg0 : kArg0, arg0);

    RegStorage arg1_reg;
    if (arg1.fp == arg0.fp) {
      arg1_reg = TargetReg((arg1.fp) ? kFArg1 : kArg1, arg1);
    } else {
      arg1_reg = TargetReg((arg1.fp) ? kFArg0 : kArg0, arg1);
    }

    if (arg0.wide == 0) {
      LoadValueDirectFixed(arg0, arg0_reg);
    } else {
      LoadValueDirectWideFixed(arg0, arg0_reg);
    }

    if (arg1.wide == 0) {
      LoadValueDirectFixed(arg1, arg1_reg);
    } else {
      LoadValueDirectWideFixed(arg1, arg1_reg);
    }
  } else {
    DCHECK(!cu_->target64);
    if (arg0.wide == 0) {
      LoadValueDirectFixed(arg0, TargetReg(arg0.fp ? kFArg0 : kArg0, kNotWide));
      if (arg1.wide == 0) {
        if (cu_->instruction_set == kMips) {
          LoadValueDirectFixed(arg1, TargetReg(arg1.fp ? kFArg2 : kArg1, kNotWide));
        } else {
          LoadValueDirectFixed(arg1, TargetReg(kArg1, kNotWide));
        }
      } else {
        if (cu_->instruction_set == kMips) {
          LoadValueDirectWideFixed(arg1, TargetReg(arg1.fp ? kFArg2 : kArg2, kWide));
        } else {
          LoadValueDirectWideFixed(arg1, TargetReg(kArg1, kWide));
        }
      }
    } else {
      LoadValueDirectWideFixed(arg0, TargetReg(arg0.fp ? kFArg0 : kArg0, kWide));
      if (arg1.wide == 0) {
        LoadValueDirectFixed(arg1, TargetReg(arg1.fp ? kFArg2 : kArg2, kNotWide));
      } else {
        LoadValueDirectWideFixed(arg1, TargetReg(arg1.fp ? kFArg2 : kArg2, kWide));
      }
    }
  }
  ClobberCallerSave();
  CallHelper(r_tgt, trampoline, safepoint_pc);
}

void Mir2Lir::CopyToArgumentRegs(RegStorage arg0, RegStorage arg1) {
  WideKind arg0_kind = arg0.GetWideKind();
  WideKind arg1_kind = arg1.GetWideKind();
  if (IsSameReg(arg1, TargetReg(kArg0, arg1_kind))) {
    if (IsSameReg(arg0, TargetReg(kArg1, arg0_kind))) {
      // Swap kArg0 and kArg1 with kArg2 as temp.
      OpRegCopy(TargetReg(kArg2, arg1_kind), arg1);
      OpRegCopy(TargetReg(kArg0, arg0_kind), arg0);
      OpRegCopy(TargetReg(kArg1, arg1_kind), TargetReg(kArg2, arg1_kind));
    } else {
      OpRegCopy(TargetReg(kArg1, arg1_kind), arg1);
      OpRegCopy(TargetReg(kArg0, arg0_kind), arg0);
    }
  } else {
    OpRegCopy(TargetReg(kArg0, arg0_kind), arg0);
    OpRegCopy(TargetReg(kArg1, arg1_kind), arg1);
  }
}

void Mir2Lir::CallRuntimeHelperRegReg(QuickEntrypointEnum trampoline, RegStorage arg0,
                                      RegStorage arg1, bool safepoint_pc) {
  RegStorage r_tgt = CallHelperSetup(trampoline);
  CopyToArgumentRegs(arg0, arg1);
  ClobberCallerSave();
  CallHelper(r_tgt, trampoline, safepoint_pc);
}

void Mir2Lir::CallRuntimeHelperRegRegImm(QuickEntrypointEnum trampoline, RegStorage arg0,
                                         RegStorage arg1, int arg2, bool safepoint_pc) {
  RegStorage r_tgt = CallHelperSetup(trampoline);
  CopyToArgumentRegs(arg0, arg1);
  LoadConstant(TargetReg(kArg2, kNotWide), arg2);
  ClobberCallerSave();
  CallHelper(r_tgt, trampoline, safepoint_pc);
}

void Mir2Lir::CallRuntimeHelperImmMethodRegLocation(QuickEntrypointEnum trampoline, int arg0,
                                                    RegLocation arg2, bool safepoint_pc) {
  RegStorage r_tgt = CallHelperSetup(trampoline);
  LoadValueDirectFixed(arg2, TargetReg(kArg2, arg2));
  LoadCurrMethodDirect(TargetReg(kArg1, kRef));
  LoadConstant(TargetReg(kArg0, kNotWide), arg0);
  ClobberCallerSave();
  CallHelper(r_tgt, trampoline, safepoint_pc);
}

void Mir2Lir::CallRuntimeHelperImmMethodImm(QuickEntrypointEnum trampoline, int arg0, int arg2,
                                            bool safepoint_pc) {
  RegStorage r_tgt = CallHelperSetup(trampoline);
  LoadCurrMethodDirect(TargetReg(kArg1, kRef));
  LoadConstant(TargetReg(kArg2, kNotWide), arg2);
  LoadConstant(TargetReg(kArg0, kNotWide), arg0);
  ClobberCallerSave();
  CallHelper(r_tgt, trampoline, safepoint_pc);
}

void Mir2Lir::CallRuntimeHelperImmRegLocationRegLocation(QuickEntrypointEnum trampoline, int arg0,
                                                         RegLocation arg1,
                                                         RegLocation arg2, bool safepoint_pc) {
  RegStorage r_tgt = CallHelperSetup(trampoline);
  DCHECK_EQ(static_cast<unsigned int>(arg1.wide), 0U);  // The static_cast works around an
                                                        // instantiation bug in GCC.
  LoadValueDirectFixed(arg1, TargetReg(kArg1, arg1));
  if (arg2.wide == 0) {
    LoadValueDirectFixed(arg2, TargetReg(kArg2, arg2));
  } else {
    LoadValueDirectWideFixed(arg2, TargetReg(kArg2, kWide));
  }
  LoadConstant(TargetReg(kArg0, kNotWide), arg0);
  ClobberCallerSave();
  CallHelper(r_tgt, trampoline, safepoint_pc);
}

void Mir2Lir::CallRuntimeHelperRegLocationRegLocationRegLocation(
    QuickEntrypointEnum trampoline,
    RegLocation arg0,
    RegLocation arg1,
    RegLocation arg2,
    bool safepoint_pc) {
  RegStorage r_tgt = CallHelperSetup(trampoline);
  LoadValueDirectFixed(arg0, TargetReg(kArg0, arg0));
  LoadValueDirectFixed(arg1, TargetReg(kArg1, arg1));
  LoadValueDirectFixed(arg2, TargetReg(kArg2, arg2));
  ClobberCallerSave();
  CallHelper(r_tgt, trampoline, safepoint_pc);
}

/*
 * If there are any ins passed in registers that have not been promoted
 * to a callee-save register, flush them to the frame.  Perform initial
 * assignment of promoted arguments.
 *
 * ArgLocs is an array of location records describing the incoming arguments
 * with one location record per word of argument.
 */
void Mir2Lir::FlushIns(RegLocation* ArgLocs, RegLocation rl_method) {
  /*
   * Dummy up a RegLocation for the incoming StackReference<mirror::ArtMethod>
   * It will attempt to keep kArg0 live (or copy it to home location
   * if promoted).
   */
  RegLocation rl_src = rl_method;
  rl_src.location = kLocPhysReg;
  rl_src.reg = TargetReg(kArg0, kRef);
  rl_src.home = false;
  MarkLive(rl_src);
  StoreValue(rl_method, rl_src);
  // If Method* has been promoted, explicitly flush
  if (rl_method.location == kLocPhysReg) {
    StoreRefDisp(TargetPtrReg(kSp), 0, rl_src.reg, kNotVolatile);
  }

  if (cu_->num_ins == 0) {
    return;
  }

  int start_vreg = cu_->num_dalvik_registers - cu_->num_ins;
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
  ScopedMemRefType mem_ref_type(this, ResourceMask::kDalvikReg);
  for (int i = 0; i < cu_->num_ins; i++) {
    PromotionMap* v_map = &promotion_map_[start_vreg + i];
    RegStorage reg = GetArgMappingToPhysicalReg(i);

    if (reg.Valid()) {
      // If arriving in register
      bool need_flush = true;
      RegLocation* t_loc = &ArgLocs[i];
      if ((v_map->core_location == kLocPhysReg) && !t_loc->fp) {
        OpRegCopy(RegStorage::Solo32(v_map->core_reg), reg);
        need_flush = false;
      } else if ((v_map->fp_location == kLocPhysReg) && t_loc->fp) {
        OpRegCopy(RegStorage::Solo32(v_map->fp_reg), reg);
        need_flush = false;
      } else {
        need_flush = true;
      }

      // For wide args, force flush if not fully promoted
      if (t_loc->wide) {
        PromotionMap* p_map = v_map + (t_loc->high_word ? -1 : +1);
        // Is only half promoted?
        need_flush |= (p_map->core_location != v_map->core_location) ||
            (p_map->fp_location != v_map->fp_location);
        if ((cu_->instruction_set == kThumb2) && t_loc->fp && !need_flush) {
          /*
           * In Arm, a double is represented as a pair of consecutive single float
           * registers starting at an even number.  It's possible that both Dalvik vRegs
           * representing the incoming double were independently promoted as singles - but
           * not in a form usable as a double.  If so, we need to flush - even though the
           * incoming arg appears fully in register.  At this point in the code, both
           * halves of the double are promoted.  Make sure they are in a usable form.
           */
          int lowreg_index = start_vreg + i + (t_loc->high_word ? -1 : 0);
          int low_reg = promotion_map_[lowreg_index].fp_reg;
          int high_reg = promotion_map_[lowreg_index + 1].fp_reg;
          if (((low_reg & 0x1) != 0) || (high_reg != (low_reg + 1))) {
            need_flush = true;
          }
        }
      }
      if (need_flush) {
        Store32Disp(TargetPtrReg(kSp), SRegOffset(start_vreg + i), reg);
      }
    } else {
      // If arriving in frame & promoted
      if (v_map->core_location == kLocPhysReg) {
        Load32Disp(TargetPtrReg(kSp), SRegOffset(start_vreg + i),
                   RegStorage::Solo32(v_map->core_reg));
      }
      if (v_map->fp_location == kLocPhysReg) {
        Load32Disp(TargetPtrReg(kSp), SRegOffset(start_vreg + i),
                   RegStorage::Solo32(v_map->fp_reg));
      }
    }
  }
}

static void CommonCallCodeLoadThisIntoArg1(const CallInfo* info, Mir2Lir* cg) {
  RegLocation rl_arg = info->args[0];
  cg->LoadValueDirectFixed(rl_arg, cg->TargetReg(kArg1, kRef));
}

static void CommonCallCodeLoadClassIntoArg0(const CallInfo* info, Mir2Lir* cg) {
  cg->GenNullCheck(cg->TargetReg(kArg1, kRef), info->opt_flags);
  // get this->klass_ [use kArg1, set kArg0]
  cg->LoadRefDisp(cg->TargetReg(kArg1, kRef), mirror::Object::ClassOffset().Int32Value(),
                  cg->TargetReg(kArg0, kRef),
                  kNotVolatile);
  cg->MarkPossibleNullPointerException(info->opt_flags);
}

static bool CommonCallCodeLoadCodePointerIntoInvokeTgt(const CallInfo* info,
                                                       const RegStorage* alt_from,
                                                       const CompilationUnit* cu, Mir2Lir* cg) {
  if (cu->instruction_set != kX86 && cu->instruction_set != kX86_64) {
    // Get the compiled code address [use *alt_from or kArg0, set kInvokeTgt]
    cg->LoadWordDisp(alt_from == nullptr ? cg->TargetReg(kArg0, kRef) : *alt_from,
                     mirror::ArtMethod::EntryPointFromQuickCompiledCodeOffset().Int32Value(),
                     cg->TargetPtrReg(kInvokeTgt));
    return true;
  }
  return false;
}

/*
 * Bit of a hack here - in the absence of a real scheduling pass,
 * emit the next instruction in static & direct invoke sequences.
 */
static int NextSDCallInsn(CompilationUnit* cu, CallInfo* info,
                          int state, const MethodReference& target_method,
                          uint32_t unused,
                          uintptr_t direct_code, uintptr_t direct_method,
                          InvokeType type) {
  Mir2Lir* cg = static_cast<Mir2Lir*>(cu->cg.get());
  if (direct_code != 0 && direct_method != 0) {
    switch (state) {
    case 0:  // Get the current Method* [sets kArg0]
      if (direct_code != static_cast<uintptr_t>(-1)) {
        if (cu->instruction_set != kX86 && cu->instruction_set != kX86_64) {
          cg->LoadConstant(cg->TargetPtrReg(kInvokeTgt), direct_code);
        }
      } else if (cu->instruction_set != kX86 && cu->instruction_set != kX86_64) {
        cg->LoadCodeAddress(target_method, type, kInvokeTgt);
      }
      if (direct_method != static_cast<uintptr_t>(-1)) {
        cg->LoadConstant(cg->TargetReg(kArg0, kRef), direct_method);
      } else {
        cg->LoadMethodAddress(target_method, type, kArg0);
      }
      break;
    default:
      return -1;
    }
  } else {
    RegStorage arg0_ref = cg->TargetReg(kArg0, kRef);
    switch (state) {
    case 0:  // Get the current Method* [sets kArg0]
      // TUNING: we can save a reg copy if Method* has been promoted.
      cg->LoadCurrMethodDirect(arg0_ref);
      break;
    case 1:  // Get method->dex_cache_resolved_methods_
      cg->LoadRefDisp(arg0_ref,
                      mirror::ArtMethod::DexCacheResolvedMethodsOffset().Int32Value(),
                      arg0_ref,
                      kNotVolatile);
      // Set up direct code if known.
      if (direct_code != 0) {
        if (direct_code != static_cast<uintptr_t>(-1)) {
          cg->LoadConstant(cg->TargetPtrReg(kInvokeTgt), direct_code);
        } else if (cu->instruction_set != kX86 && cu->instruction_set != kX86_64) {
          CHECK_LT(target_method.dex_method_index, target_method.dex_file->NumMethodIds());
          cg->LoadCodeAddress(target_method, type, kInvokeTgt);
        }
      }
      break;
    case 2:  // Grab target method*
      CHECK_EQ(cu->dex_file, target_method.dex_file);
      cg->LoadRefDisp(arg0_ref,
                      ObjArray::OffsetOfElement(target_method.dex_method_index).Int32Value(),
                      arg0_ref,
                      kNotVolatile);
      break;
    case 3:  // Grab the code from the method*
      if (direct_code == 0) {
        if (CommonCallCodeLoadCodePointerIntoInvokeTgt(info, &arg0_ref, cu, cg)) {
          break;                                    // kInvokeTgt := arg0_ref->entrypoint
        }
      } else if (cu->instruction_set != kX86 && cu->instruction_set != kX86_64) {
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
static int NextVCallInsn(CompilationUnit* cu, CallInfo* info,
                         int state, const MethodReference& target_method,
                         uint32_t method_idx, uintptr_t unused, uintptr_t unused2,
                         InvokeType unused3) {
  Mir2Lir* cg = static_cast<Mir2Lir*>(cu->cg.get());
  /*
   * This is the fast path in which the target virtual method is
   * fully resolved at compile time.
   */
  switch (state) {
    case 0:
      CommonCallCodeLoadThisIntoArg1(info, cg);   // kArg1 := this
      break;
    case 1:
      CommonCallCodeLoadClassIntoArg0(info, cg);  // kArg0 := kArg1->class
                                                  // Includes a null-check.
      break;
    case 2: {
      // Get this->klass_.embedded_vtable[method_idx] [usr kArg0, set kArg0]
      int32_t offset = mirror::Class::EmbeddedVTableOffset().Uint32Value() +
          method_idx * sizeof(mirror::Class::VTableEntry);
      // Load target method from embedded vtable to kArg0 [use kArg0, set kArg0]
      cg->LoadRefDisp(cg->TargetReg(kArg0, kRef), offset, cg->TargetReg(kArg0, kRef), kNotVolatile);
      break;
    }
    case 3:
      if (CommonCallCodeLoadCodePointerIntoInvokeTgt(info, nullptr, cu, cg)) {
        break;                                    // kInvokeTgt := kArg0->entrypoint
      }
      // Intentional fallthrough for X86
    default:
      return -1;
  }
  return state + 1;
}

/*
 * Emit the next instruction in an invoke interface sequence. This will do a lookup in the
 * class's IMT, calling either the actual method or art_quick_imt_conflict_trampoline if
 * more than one interface method map to the same index. Note also that we'll load the first
 * argument ("this") into kArg1 here rather than the standard LoadArgRegs.
 */
static int NextInterfaceCallInsn(CompilationUnit* cu, CallInfo* info, int state,
                                 const MethodReference& target_method,
                                 uint32_t method_idx, uintptr_t unused,
                                 uintptr_t direct_method, InvokeType unused2) {
  Mir2Lir* cg = static_cast<Mir2Lir*>(cu->cg.get());

  switch (state) {
    case 0:  // Set target method index in case of conflict [set kHiddenArg, kHiddenFpArg (x86)]
      CHECK_LT(target_method.dex_method_index, target_method.dex_file->NumMethodIds());
      cg->LoadConstant(cg->TargetReg(kHiddenArg, kNotWide), target_method.dex_method_index);
      if (cu->instruction_set == kX86) {
        cg->OpRegCopy(cg->TargetReg(kHiddenFpArg, kNotWide), cg->TargetReg(kHiddenArg, kNotWide));
      }
      break;
    case 1:
      CommonCallCodeLoadThisIntoArg1(info, cg);   // kArg1 := this
      break;
    case 2:
      CommonCallCodeLoadClassIntoArg0(info, cg);  // kArg0 := kArg1->class
                                                  // Includes a null-check.
      break;
    case 3: {  // Get target method [use kInvokeTgt, set kArg0]
      int32_t offset = mirror::Class::EmbeddedImTableOffset().Uint32Value() +
          (method_idx % mirror::Class::kImtSize) * sizeof(mirror::Class::ImTableEntry);
      // Load target method from embedded imtable to kArg0 [use kArg0, set kArg0]
      cg->LoadRefDisp(cg->TargetReg(kArg0, kRef), offset, cg->TargetReg(kArg0, kRef), kNotVolatile);
      break;
    }
    case 4:
      if (CommonCallCodeLoadCodePointerIntoInvokeTgt(info, nullptr, cu, cg)) {
        break;                                    // kInvokeTgt := kArg0->entrypoint
      }
      // Intentional fallthrough for X86
    default:
      return -1;
  }
  return state + 1;
}

static int NextInvokeInsnSP(CompilationUnit* cu, CallInfo* info,
                            QuickEntrypointEnum trampoline, int state,
                            const MethodReference& target_method, uint32_t method_idx) {
  Mir2Lir* cg = static_cast<Mir2Lir*>(cu->cg.get());


  /*
   * This handles the case in which the base method is not fully
   * resolved at compile time, we bail to a runtime helper.
   */
  if (state == 0) {
    if (cu->instruction_set != kX86 && cu->instruction_set != kX86_64) {
      // Load trampoline target
      int32_t disp;
      if (cu->target64) {
        disp = GetThreadOffset<8>(trampoline).Int32Value();
      } else {
        disp = GetThreadOffset<4>(trampoline).Int32Value();
      }
      cg->LoadWordDisp(cg->TargetPtrReg(kSelf), disp, cg->TargetPtrReg(kInvokeTgt));
    }
    // Load kArg0 with method index
    CHECK_EQ(cu->dex_file, target_method.dex_file);
    cg->LoadConstant(cg->TargetReg(kArg0, kNotWide), target_method.dex_method_index);
    return 1;
  }
  return -1;
}

static int NextStaticCallInsnSP(CompilationUnit* cu, CallInfo* info,
                                int state,
                                const MethodReference& target_method,
                                uint32_t unused, uintptr_t unused2,
                                uintptr_t unused3, InvokeType unused4) {
  return NextInvokeInsnSP(cu, info, kQuickInvokeStaticTrampolineWithAccessCheck, state,
                          target_method, 0);
}

static int NextDirectCallInsnSP(CompilationUnit* cu, CallInfo* info, int state,
                                const MethodReference& target_method,
                                uint32_t unused, uintptr_t unused2,
                                uintptr_t unused3, InvokeType unused4) {
  return NextInvokeInsnSP(cu, info, kQuickInvokeDirectTrampolineWithAccessCheck, state,
                          target_method, 0);
}

static int NextSuperCallInsnSP(CompilationUnit* cu, CallInfo* info, int state,
                               const MethodReference& target_method,
                               uint32_t unused, uintptr_t unused2,
                               uintptr_t unused3, InvokeType unused4) {
  return NextInvokeInsnSP(cu, info, kQuickInvokeSuperTrampolineWithAccessCheck, state,
                          target_method, 0);
}

static int NextVCallInsnSP(CompilationUnit* cu, CallInfo* info, int state,
                           const MethodReference& target_method,
                           uint32_t unused, uintptr_t unused2,
                           uintptr_t unused3, InvokeType unused4) {
  return NextInvokeInsnSP(cu, info, kQuickInvokeVirtualTrampolineWithAccessCheck, state,
                          target_method, 0);
}

static int NextInterfaceCallInsnWithAccessCheck(CompilationUnit* cu,
                                                CallInfo* info, int state,
                                                const MethodReference& target_method,
                                                uint32_t unused, uintptr_t unused2,
                                                uintptr_t unused3, InvokeType unused4) {
  return NextInvokeInsnSP(cu, info, kQuickInvokeInterfaceTrampolineWithAccessCheck, state,
                          target_method, 0);
}

int Mir2Lir::LoadArgRegs(CallInfo* info, int call_state,
                         NextCallInsn next_call_insn,
                         const MethodReference& target_method,
                         uint32_t vtable_idx, uintptr_t direct_code,
                         uintptr_t direct_method, InvokeType type, bool skip_this) {
  int last_arg_reg = 3 - 1;
  int arg_regs[3] = {TargetReg(kArg1, kNotWide).GetReg(), TargetReg(kArg2, kNotWide).GetReg(),
                     TargetReg(kArg3, kNotWide).GetReg()};

  int next_reg = 0;
  int next_arg = 0;
  if (skip_this) {
    next_reg++;
    next_arg++;
  }
  for (; (next_reg <= last_arg_reg) && (next_arg < info->num_arg_words); next_reg++) {
    RegLocation rl_arg = info->args[next_arg++];
    rl_arg = UpdateRawLoc(rl_arg);
    if (rl_arg.wide && (next_reg <= last_arg_reg - 1)) {
      RegStorage r_tmp(RegStorage::k64BitPair, arg_regs[next_reg], arg_regs[next_reg + 1]);
      LoadValueDirectWideFixed(rl_arg, r_tmp);
      next_reg++;
      next_arg++;
    } else {
      if (rl_arg.wide) {
        rl_arg = NarrowRegLoc(rl_arg);
        rl_arg.is_const = false;
      }
      LoadValueDirectFixed(rl_arg, RegStorage::Solo32(arg_regs[next_reg]));
    }
    call_state = next_call_insn(cu_, info, call_state, target_method, vtable_idx,
                                direct_code, direct_method, type);
  }
  return call_state;
}

/*
 * Load up to 5 arguments, the first three of which will be in
 * kArg1 .. kArg3.  On entry kArg0 contains the current method pointer,
 * and as part of the load sequence, it must be replaced with
 * the target method pointer.  Note, this may also be called
 * for "range" variants if the number of arguments is 5 or fewer.
 */
int Mir2Lir::GenDalvikArgsNoRange(CallInfo* info,
                                  int call_state, LIR** pcrLabel, NextCallInsn next_call_insn,
                                  const MethodReference& target_method,
                                  uint32_t vtable_idx, uintptr_t direct_code,
                                  uintptr_t direct_method, InvokeType type, bool skip_this) {
  RegLocation rl_arg;

  /* If no arguments, just return */
  if (info->num_arg_words == 0)
    return call_state;

  call_state = next_call_insn(cu_, info, call_state, target_method, vtable_idx,
                              direct_code, direct_method, type);

  DCHECK_LE(info->num_arg_words, 5);
  if (info->num_arg_words > 3) {
    int32_t next_use = 3;
    // Detect special case of wide arg spanning arg3/arg4
    RegLocation rl_use0 = info->args[0];
    RegLocation rl_use1 = info->args[1];
    RegLocation rl_use2 = info->args[2];
    if (((!rl_use0.wide && !rl_use1.wide) || rl_use0.wide) && rl_use2.wide) {
      RegStorage reg;
      // Wide spans, we need the 2nd half of uses[2].
      rl_arg = UpdateLocWide(rl_use2);
      if (rl_arg.location == kLocPhysReg) {
        if (rl_arg.reg.IsPair()) {
          reg = rl_arg.reg.GetHigh();
        } else {
          RegisterInfo* info = GetRegInfo(rl_arg.reg);
          info = info->FindMatchingView(RegisterInfo::kHighSingleStorageMask);
          if (info == nullptr) {
            // NOTE: For hard float convention we won't split arguments across reg/mem.
            UNIMPLEMENTED(FATAL) << "Needs hard float api.";
          }
          reg = info->GetReg();
        }
      } else {
        // kArg2 & rArg3 can safely be used here
        reg = TargetReg(kArg3, kNotWide);
        {
          ScopedMemRefType mem_ref_type(this, ResourceMask::kDalvikReg);
          Load32Disp(TargetPtrReg(kSp), SRegOffset(rl_arg.s_reg_low) + 4, reg);
        }
        call_state = next_call_insn(cu_, info, call_state, target_method,
                                    vtable_idx, direct_code, direct_method, type);
      }
      {
        ScopedMemRefType mem_ref_type(this, ResourceMask::kDalvikReg);
        Store32Disp(TargetPtrReg(kSp), (next_use + 1) * 4, reg);
      }
      call_state = next_call_insn(cu_, info, call_state, target_method, vtable_idx,
                                  direct_code, direct_method, type);
      next_use++;
    }
    // Loop through the rest
    while (next_use < info->num_arg_words) {
      RegStorage arg_reg;
      rl_arg = info->args[next_use];
      rl_arg = UpdateRawLoc(rl_arg);
      if (rl_arg.location == kLocPhysReg) {
        arg_reg = rl_arg.reg;
      } else {
        arg_reg = TargetReg(kArg2, rl_arg.wide ? kWide : kNotWide);
        if (rl_arg.wide) {
          LoadValueDirectWideFixed(rl_arg, arg_reg);
        } else {
          LoadValueDirectFixed(rl_arg, arg_reg);
        }
        call_state = next_call_insn(cu_, info, call_state, target_method,
                                    vtable_idx, direct_code, direct_method, type);
      }
      int outs_offset = (next_use + 1) * 4;
      {
        ScopedMemRefType mem_ref_type(this, ResourceMask::kDalvikReg);
        if (rl_arg.wide) {
          StoreBaseDisp(TargetPtrReg(kSp), outs_offset, arg_reg, k64, kNotVolatile);
          next_use += 2;
        } else {
          Store32Disp(TargetPtrReg(kSp), outs_offset, arg_reg);
          next_use++;
        }
      }
      call_state = next_call_insn(cu_, info, call_state, target_method, vtable_idx,
                               direct_code, direct_method, type);
    }
  }

  call_state = LoadArgRegs(info, call_state, next_call_insn,
                           target_method, vtable_idx, direct_code, direct_method,
                           type, skip_this);

  if (pcrLabel) {
    if (!cu_->compiler_driver->GetCompilerOptions().GetImplicitNullChecks()) {
      *pcrLabel = GenExplicitNullCheck(TargetReg(kArg1, kRef), info->opt_flags);
    } else {
      *pcrLabel = nullptr;
      if (!(cu_->disable_opt & (1 << kNullCheckElimination)) &&
          (info->opt_flags & MIR_IGNORE_NULL_CHECK)) {
        return call_state;
      }
      // In lieu of generating a check for kArg1 being null, we need to
      // perform a load when doing implicit checks.
      GenImplicitNullCheck(TargetReg(kArg1, kRef), info->opt_flags);
    }
  }
  return call_state;
}

// Default implementation of implicit null pointer check.
// Overridden by arch specific as necessary.
void Mir2Lir::GenImplicitNullCheck(RegStorage reg, int opt_flags) {
  if (!(cu_->disable_opt & (1 << kNullCheckElimination)) && (opt_flags & MIR_IGNORE_NULL_CHECK)) {
    return;
  }
  RegStorage tmp = AllocTemp();
  Load32Disp(reg, 0, tmp);
  MarkPossibleNullPointerException(opt_flags);
  FreeTemp(tmp);
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
int Mir2Lir::GenDalvikArgsRange(CallInfo* info, int call_state,
                                LIR** pcrLabel, NextCallInsn next_call_insn,
                                const MethodReference& target_method,
                                uint32_t vtable_idx, uintptr_t direct_code, uintptr_t direct_method,
                                InvokeType type, bool skip_this) {
  // If we can treat it as non-range (Jumbo ops will use range form)
  if (info->num_arg_words <= 5)
    return GenDalvikArgsNoRange(info, call_state, pcrLabel,
                                next_call_insn, target_method, vtable_idx,
                                direct_code, direct_method, type, skip_this);
  /*
   * First load the non-register arguments.  Both forms expect all
   * of the source arguments to be in their home frame location, so
   * scan the s_reg names and flush any that have been promoted to
   * frame backing storage.
   */
  // Scan the rest of the args - if in phys_reg flush to memory
  for (int next_arg = 0; next_arg < info->num_arg_words;) {
    RegLocation loc = info->args[next_arg];
    if (loc.wide) {
      loc = UpdateLocWide(loc);
      if ((next_arg >= 2) && (loc.location == kLocPhysReg)) {
        ScopedMemRefType mem_ref_type(this, ResourceMask::kDalvikReg);
        StoreBaseDisp(TargetPtrReg(kSp), SRegOffset(loc.s_reg_low), loc.reg, k64, kNotVolatile);
      }
      next_arg += 2;
    } else {
      loc = UpdateLoc(loc);
      if ((next_arg >= 3) && (loc.location == kLocPhysReg)) {
        ScopedMemRefType mem_ref_type(this, ResourceMask::kDalvikReg);
        Store32Disp(TargetPtrReg(kSp), SRegOffset(loc.s_reg_low), loc.reg);
      }
      next_arg++;
    }
  }

  // Logic below assumes that Method pointer is at offset zero from SP.
  DCHECK_EQ(VRegOffset(static_cast<int>(kVRegMethodPtrBaseReg)), 0);

  // The first 3 arguments are passed via registers.
  // TODO: For 64-bit, instead of hardcoding 4 for Method* size, we should either
  // get size of uintptr_t or size of object reference according to model being used.
  int outs_offset = 4 /* Method* */ + (3 * sizeof(uint32_t));
  int start_offset = SRegOffset(info->args[3].s_reg_low);
  int regs_left_to_pass_via_stack = info->num_arg_words - 3;
  DCHECK_GT(regs_left_to_pass_via_stack, 0);

  if (cu_->instruction_set == kThumb2 && regs_left_to_pass_via_stack <= 16) {
    // Use vldm/vstm pair using kArg3 as a temp
    call_state = next_call_insn(cu_, info, call_state, target_method, vtable_idx,
                             direct_code, direct_method, type);
    OpRegRegImm(kOpAdd, TargetReg(kArg3, kRef), TargetPtrReg(kSp), start_offset);
    LIR* ld = nullptr;
    {
      ScopedMemRefType mem_ref_type(this, ResourceMask::kDalvikReg);
      ld = OpVldm(TargetReg(kArg3, kRef), regs_left_to_pass_via_stack);
    }
    // TUNING: loosen barrier
    ld->u.m.def_mask = &kEncodeAll;
    call_state = next_call_insn(cu_, info, call_state, target_method, vtable_idx,
                             direct_code, direct_method, type);
    OpRegRegImm(kOpAdd, TargetReg(kArg3, kRef), TargetPtrReg(kSp), 4 /* Method* */ + (3 * 4));
    call_state = next_call_insn(cu_, info, call_state, target_method, vtable_idx,
                             direct_code, direct_method, type);
    LIR* st = nullptr;
    {
      ScopedMemRefType mem_ref_type(this, ResourceMask::kDalvikReg);
      st = OpVstm(TargetReg(kArg3, kRef), regs_left_to_pass_via_stack);
    }
    st->u.m.def_mask = &kEncodeAll;
    call_state = next_call_insn(cu_, info, call_state, target_method, vtable_idx,
                             direct_code, direct_method, type);
  } else if (cu_->instruction_set == kX86 || cu_->instruction_set == kX86_64) {
    int current_src_offset = start_offset;
    int current_dest_offset = outs_offset;

    // Only davik regs are accessed in this loop; no next_call_insn() calls.
    ScopedMemRefType mem_ref_type(this, ResourceMask::kDalvikReg);
    while (regs_left_to_pass_via_stack > 0) {
      // This is based on the knowledge that the stack itself is 16-byte aligned.
      bool src_is_16b_aligned = (current_src_offset & 0xF) == 0;
      bool dest_is_16b_aligned = (current_dest_offset & 0xF) == 0;
      size_t bytes_to_move;

      /*
       * The amount to move defaults to 32-bit. If there are 4 registers left to move, then do a
       * a 128-bit move because we won't get the chance to try to aligned. If there are more than
       * 4 registers left to move, consider doing a 128-bit only if either src or dest are aligned.
       * We do this because we could potentially do a smaller move to align.
       */
      if (regs_left_to_pass_via_stack == 4 ||
          (regs_left_to_pass_via_stack > 4 && (src_is_16b_aligned || dest_is_16b_aligned))) {
        // Moving 128-bits via xmm register.
        bytes_to_move = sizeof(uint32_t) * 4;

        // Allocate a free xmm temp. Since we are working through the calling sequence,
        // we expect to have an xmm temporary available.  AllocTempDouble will abort if
        // there are no free registers.
        RegStorage temp = AllocTempDouble();

        LIR* ld1 = nullptr;
        LIR* ld2 = nullptr;
        LIR* st1 = nullptr;
        LIR* st2 = nullptr;

        /*
         * The logic is similar for both loads and stores. If we have 16-byte alignment,
         * do an aligned move. If we have 8-byte alignment, then do the move in two
         * parts. This approach prevents possible cache line splits. Finally, fall back
         * to doing an unaligned move. In most cases we likely won't split the cache
         * line but we cannot prove it and thus take a conservative approach.
         */
        bool src_is_8b_aligned = (current_src_offset & 0x7) == 0;
        bool dest_is_8b_aligned = (current_dest_offset & 0x7) == 0;

        if (src_is_16b_aligned) {
          ld1 = OpMovRegMem(temp, TargetPtrReg(kSp), current_src_offset, kMovA128FP);
        } else if (src_is_8b_aligned) {
          ld1 = OpMovRegMem(temp, TargetPtrReg(kSp), current_src_offset, kMovLo128FP);
          ld2 = OpMovRegMem(temp, TargetPtrReg(kSp), current_src_offset + (bytes_to_move >> 1),
                            kMovHi128FP);
        } else {
          ld1 = OpMovRegMem(temp, TargetPtrReg(kSp), current_src_offset, kMovU128FP);
        }

        if (dest_is_16b_aligned) {
          st1 = OpMovMemReg(TargetPtrReg(kSp), current_dest_offset, temp, kMovA128FP);
        } else if (dest_is_8b_aligned) {
          st1 = OpMovMemReg(TargetPtrReg(kSp), current_dest_offset, temp, kMovLo128FP);
          st2 = OpMovMemReg(TargetPtrReg(kSp), current_dest_offset + (bytes_to_move >> 1),
                            temp, kMovHi128FP);
        } else {
          st1 = OpMovMemReg(TargetPtrReg(kSp), current_dest_offset, temp, kMovU128FP);
        }

        // TODO If we could keep track of aliasing information for memory accesses that are wider
        // than 64-bit, we wouldn't need to set up a barrier.
        if (ld1 != nullptr) {
          if (ld2 != nullptr) {
            // For 64-bit load we can actually set up the aliasing information.
            AnnotateDalvikRegAccess(ld1, current_src_offset >> 2, true, true);
            AnnotateDalvikRegAccess(ld2, (current_src_offset + (bytes_to_move >> 1)) >> 2, true,
                                    true);
          } else {
            // Set barrier for 128-bit load.
            ld1->u.m.def_mask = &kEncodeAll;
          }
        }
        if (st1 != nullptr) {
          if (st2 != nullptr) {
            // For 64-bit store we can actually set up the aliasing information.
            AnnotateDalvikRegAccess(st1, current_dest_offset >> 2, false, true);
            AnnotateDalvikRegAccess(st2, (current_dest_offset + (bytes_to_move >> 1)) >> 2, false,
                                    true);
          } else {
            // Set barrier for 128-bit store.
            st1->u.m.def_mask = &kEncodeAll;
          }
        }

        // Free the temporary used for the data movement.
        FreeTemp(temp);
      } else {
        // Moving 32-bits via general purpose register.
        bytes_to_move = sizeof(uint32_t);

        // Instead of allocating a new temp, simply reuse one of the registers being used
        // for argument passing.
        RegStorage temp = TargetReg(kArg3, kNotWide);

        // Now load the argument VR and store to the outs.
        Load32Disp(TargetPtrReg(kSp), current_src_offset, temp);
        Store32Disp(TargetPtrReg(kSp), current_dest_offset, temp);
      }

      current_src_offset += bytes_to_move;
      current_dest_offset += bytes_to_move;
      regs_left_to_pass_via_stack -= (bytes_to_move >> 2);
    }
  } else {
    // Generate memcpy
    OpRegRegImm(kOpAdd, TargetReg(kArg0, kRef), TargetPtrReg(kSp), outs_offset);
    OpRegRegImm(kOpAdd, TargetReg(kArg1, kRef), TargetPtrReg(kSp), start_offset);
    CallRuntimeHelperRegRegImm(kQuickMemcpy, TargetReg(kArg0, kRef), TargetReg(kArg1, kRef),
                               (info->num_arg_words - 3) * 4, false);
  }

  call_state = LoadArgRegs(info, call_state, next_call_insn,
                           target_method, vtable_idx, direct_code, direct_method,
                           type, skip_this);

  call_state = next_call_insn(cu_, info, call_state, target_method, vtable_idx,
                           direct_code, direct_method, type);
  if (pcrLabel) {
    if (!cu_->compiler_driver->GetCompilerOptions().GetImplicitNullChecks()) {
      *pcrLabel = GenExplicitNullCheck(TargetReg(kArg1, kRef), info->opt_flags);
    } else {
      *pcrLabel = nullptr;
      if (!(cu_->disable_opt & (1 << kNullCheckElimination)) &&
          (info->opt_flags & MIR_IGNORE_NULL_CHECK)) {
        return call_state;
      }
      // In lieu of generating a check for kArg1 being null, we need to
      // perform a load when doing implicit checks.
      GenImplicitNullCheck(TargetReg(kArg1, kRef), info->opt_flags);
    }
  }
  return call_state;
}

RegLocation Mir2Lir::InlineTarget(CallInfo* info) {
  RegLocation res;
  if (info->result.location == kLocInvalid) {
    res = GetReturn(LocToRegClass(info->result));
  } else {
    res = info->result;
  }
  return res;
}

RegLocation Mir2Lir::InlineTargetWide(CallInfo* info) {
  RegLocation res;
  if (info->result.location == kLocInvalid) {
    res = GetReturnWide(kCoreReg);
  } else {
    res = info->result;
  }
  return res;
}

bool Mir2Lir::GenInlinedReferenceGetReferent(CallInfo* info) {
  if (cu_->instruction_set == kMips) {
    // TODO - add Mips implementation
    return false;
  }

  // the refrence class is stored in the image dex file which might not be the same as the cu's
  // dex file. Query the reference class for the image dex file then reset to starting dex file
  // in after loading class type.
  uint16_t type_idx = 0;
  const DexFile* ref_dex_file = nullptr;
  {
    ScopedObjectAccess soa(Thread::Current());
    type_idx = mirror::Reference::GetJavaLangRefReference()->GetDexTypeIndex();
    ref_dex_file = mirror::Reference::GetJavaLangRefReference()->GetDexCache()->GetDexFile();
  }
  CHECK(LIKELY(ref_dex_file != nullptr));

  // address is either static within the image file, or needs to be patched up after compilation.
  bool unused_type_initialized;
  bool use_direct_type_ptr;
  uintptr_t direct_type_ptr;
  bool is_finalizable;
  const DexFile* old_dex = cu_->dex_file;
  cu_->dex_file = ref_dex_file;
  RegStorage reg_class = TargetReg(kArg1, kRef);
  Clobber(reg_class);
  LockTemp(reg_class);
  if (!cu_->compiler_driver->CanEmbedTypeInCode(*ref_dex_file, type_idx, &unused_type_initialized,
                                                &use_direct_type_ptr, &direct_type_ptr,
                                                &is_finalizable) || is_finalizable) {
    cu_->dex_file = old_dex;
    // address is not known and post-compile patch is not possible, cannot insert intrinsic.
    return false;
  }
  if (use_direct_type_ptr) {
    LoadConstant(reg_class, direct_type_ptr);
  } else if (cu_->dex_file == old_dex) {
    // TODO: Bug 16656190 If cu_->dex_file != old_dex the patching could retrieve the wrong class
    // since the load class is indexed only by the type_idx. We should include which dex file a
    // class is from in the LoadClassType LIR.
    LoadClassType(type_idx, kArg1);
  } else {
    cu_->dex_file = old_dex;
    return false;
  }
  cu_->dex_file = old_dex;

  // get the offset for flags in reference class.
  uint32_t slow_path_flag_offset = 0;
  uint32_t disable_flag_offset = 0;
  {
    ScopedObjectAccess soa(Thread::Current());
    mirror::Class* reference_class = mirror::Reference::GetJavaLangRefReference();
    slow_path_flag_offset = reference_class->GetSlowPathFlagOffset().Uint32Value();
    disable_flag_offset = reference_class->GetDisableIntrinsicFlagOffset().Uint32Value();
  }
  CHECK(slow_path_flag_offset && disable_flag_offset &&
        (slow_path_flag_offset != disable_flag_offset));

  // intrinsic logic start.
  RegLocation rl_obj = info->args[0];
  rl_obj = LoadValue(rl_obj);

  RegStorage reg_slow_path = AllocTemp();
  RegStorage reg_disabled = AllocTemp();
  Load32Disp(reg_class, slow_path_flag_offset, reg_slow_path);
  Load32Disp(reg_class, disable_flag_offset, reg_disabled);
  FreeTemp(reg_class);
  LIR* or_inst = OpRegRegReg(kOpOr, reg_slow_path, reg_slow_path, reg_disabled);
  FreeTemp(reg_disabled);

  // if slow path, jump to JNI path target
  LIR* slow_path_branch;
  if (or_inst->u.m.def_mask->HasBit(ResourceMask::kCCode)) {
    // Generate conditional branch only, as the OR set a condition state (we are interested in a 'Z' flag).
    slow_path_branch = OpCondBranch(kCondNe, nullptr);
  } else {
    // Generate compare and branch.
    slow_path_branch = OpCmpImmBranch(kCondNe, reg_slow_path, 0, nullptr);
  }
  FreeTemp(reg_slow_path);

  // slow path not enabled, simply load the referent of the reference object
  RegLocation rl_dest = InlineTarget(info);
  RegLocation rl_result = EvalLoc(rl_dest, kRefReg, true);
  GenNullCheck(rl_obj.reg, info->opt_flags);
  LoadRefDisp(rl_obj.reg, mirror::Reference::ReferentOffset().Int32Value(), rl_result.reg,
      kNotVolatile);
  MarkPossibleNullPointerException(info->opt_flags);
  StoreValue(rl_dest, rl_result);

  LIR* intrinsic_finish = NewLIR0(kPseudoTargetLabel);
  AddIntrinsicSlowPath(info, slow_path_branch, intrinsic_finish);

  return true;
}

bool Mir2Lir::GenInlinedCharAt(CallInfo* info) {
  if (cu_->instruction_set == kMips) {
    // TODO - add Mips implementation
    return false;
  }
  // Location of reference to data array
  int value_offset = mirror::String::ValueOffset().Int32Value();
  // Location of count
  int count_offset = mirror::String::CountOffset().Int32Value();
  // Starting offset within data array
  int offset_offset = mirror::String::OffsetOffset().Int32Value();
  // Start of char data with array_
  int data_offset = mirror::Array::DataOffset(sizeof(uint16_t)).Int32Value();

  RegLocation rl_obj = info->args[0];
  RegLocation rl_idx = info->args[1];
  rl_obj = LoadValue(rl_obj, kRefReg);
  rl_idx = LoadValue(rl_idx, kCoreReg);
  RegStorage reg_max;
  GenNullCheck(rl_obj.reg, info->opt_flags);
  bool range_check = (!(info->opt_flags & MIR_IGNORE_RANGE_CHECK));
  LIR* range_check_branch = nullptr;
  RegStorage reg_off;
  RegStorage reg_ptr;
  reg_off = AllocTemp();
  reg_ptr = AllocTempRef();
  if (range_check) {
    reg_max = AllocTemp();
    Load32Disp(rl_obj.reg, count_offset, reg_max);
    MarkPossibleNullPointerException(info->opt_flags);
  }
  Load32Disp(rl_obj.reg, offset_offset, reg_off);
  MarkPossibleNullPointerException(info->opt_flags);
  LoadRefDisp(rl_obj.reg, value_offset, reg_ptr, kNotVolatile);
  if (range_check) {
    // Set up a slow path to allow retry in case of bounds violation */
    OpRegReg(kOpCmp, rl_idx.reg, reg_max);
    FreeTemp(reg_max);
    range_check_branch = OpCondBranch(kCondUge, nullptr);
  }
  OpRegImm(kOpAdd, reg_ptr, data_offset);
  if (rl_idx.is_const) {
    OpRegImm(kOpAdd, reg_off, mir_graph_->ConstantValue(rl_idx.orig_sreg));
  } else {
    OpRegReg(kOpAdd, reg_off, rl_idx.reg);
  }
  FreeTemp(rl_obj.reg);
  if (rl_idx.location == kLocPhysReg) {
    FreeTemp(rl_idx.reg);
  }
  RegLocation rl_dest = InlineTarget(info);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  LoadBaseIndexed(reg_ptr, reg_off, rl_result.reg, 1, kUnsignedHalf);
  FreeTemp(reg_off);
  FreeTemp(reg_ptr);
  StoreValue(rl_dest, rl_result);
  if (range_check) {
    DCHECK(range_check_branch != nullptr);
    info->opt_flags |= MIR_IGNORE_NULL_CHECK;  // Record that we've already null checked.
    AddIntrinsicSlowPath(info, range_check_branch);
  }
  return true;
}

// Generates an inlined String.is_empty or String.length.
bool Mir2Lir::GenInlinedStringIsEmptyOrLength(CallInfo* info, bool is_empty) {
  if (cu_->instruction_set == kMips) {
    // TODO - add Mips implementation
    return false;
  }
  // dst = src.length();
  RegLocation rl_obj = info->args[0];
  rl_obj = LoadValue(rl_obj, kRefReg);
  RegLocation rl_dest = InlineTarget(info);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  GenNullCheck(rl_obj.reg, info->opt_flags);
  Load32Disp(rl_obj.reg, mirror::String::CountOffset().Int32Value(), rl_result.reg);
  MarkPossibleNullPointerException(info->opt_flags);
  if (is_empty) {
    // dst = (dst == 0);
    if (cu_->instruction_set == kThumb2) {
      RegStorage t_reg = AllocTemp();
      OpRegReg(kOpNeg, t_reg, rl_result.reg);
      OpRegRegReg(kOpAdc, rl_result.reg, rl_result.reg, t_reg);
    } else if (cu_->instruction_set == kArm64) {
      OpRegImm(kOpSub, rl_result.reg, 1);
      OpRegRegImm(kOpLsr, rl_result.reg, rl_result.reg, 31);
    } else {
      DCHECK(cu_->instruction_set == kX86 || cu_->instruction_set == kX86_64);
      OpRegImm(kOpSub, rl_result.reg, 1);
      OpRegImm(kOpLsr, rl_result.reg, 31);
    }
  }
  StoreValue(rl_dest, rl_result);
  return true;
}

bool Mir2Lir::GenInlinedReverseBytes(CallInfo* info, OpSize size) {
  if (cu_->instruction_set == kMips) {
    // TODO - add Mips implementation.
    return false;
  }
  RegLocation rl_src_i = info->args[0];
  RegLocation rl_i = (size == k64) ? LoadValueWide(rl_src_i, kCoreReg) : LoadValue(rl_src_i, kCoreReg);
  RegLocation rl_dest = (size == k64) ? InlineTargetWide(info) : InlineTarget(info);  // result reg
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  if (size == k64) {
    if (cu_->instruction_set == kArm64 || cu_->instruction_set == kX86_64) {
      OpRegReg(kOpRev, rl_result.reg, rl_i.reg);
      StoreValueWide(rl_dest, rl_result);
      return true;
    }
    RegStorage r_i_low = rl_i.reg.GetLow();
    if (rl_i.reg.GetLowReg() == rl_result.reg.GetLowReg()) {
      // First REV shall clobber rl_result.reg.GetReg(), save the value in a temp for the second REV.
      r_i_low = AllocTemp();
      OpRegCopy(r_i_low, rl_i.reg);
    }
    OpRegReg(kOpRev, rl_result.reg.GetLow(), rl_i.reg.GetHigh());
    OpRegReg(kOpRev, rl_result.reg.GetHigh(), r_i_low);
    if (rl_i.reg.GetLowReg() == rl_result.reg.GetLowReg()) {
      FreeTemp(r_i_low);
    }
    StoreValueWide(rl_dest, rl_result);
  } else {
    DCHECK(size == k32 || size == kSignedHalf);
    OpKind op = (size == k32) ? kOpRev : kOpRevsh;
    OpRegReg(op, rl_result.reg, rl_i.reg);
    StoreValue(rl_dest, rl_result);
  }
  return true;
}

bool Mir2Lir::GenInlinedAbsInt(CallInfo* info) {
  if (cu_->instruction_set == kMips) {
    // TODO - add Mips implementation
    return false;
  }
  RegLocation rl_src = info->args[0];
  rl_src = LoadValue(rl_src, kCoreReg);
  RegLocation rl_dest = InlineTarget(info);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  RegStorage sign_reg = AllocTemp();
  // abs(x) = y<=x>>31, (x+y)^y.
  OpRegRegImm(kOpAsr, sign_reg, rl_src.reg, 31);
  OpRegRegReg(kOpAdd, rl_result.reg, rl_src.reg, sign_reg);
  OpRegReg(kOpXor, rl_result.reg, sign_reg);
  StoreValue(rl_dest, rl_result);
  return true;
}

bool Mir2Lir::GenInlinedAbsLong(CallInfo* info) {
  if (cu_->instruction_set == kMips) {
    // TODO - add Mips implementation
    return false;
  }
  RegLocation rl_src = info->args[0];
  rl_src = LoadValueWide(rl_src, kCoreReg);
  RegLocation rl_dest = InlineTargetWide(info);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);

  // If on x86 or if we would clobber a register needed later, just copy the source first.
  if (cu_->instruction_set != kX86_64 &&
      (cu_->instruction_set == kX86 ||
       rl_result.reg.GetLowReg() == rl_src.reg.GetHighReg())) {
    OpRegCopyWide(rl_result.reg, rl_src.reg);
    if (rl_result.reg.GetLowReg() != rl_src.reg.GetLowReg() &&
        rl_result.reg.GetLowReg() != rl_src.reg.GetHighReg() &&
        rl_result.reg.GetHighReg() != rl_src.reg.GetLowReg() &&
        rl_result.reg.GetHighReg() != rl_src.reg.GetHighReg()) {
      // Reuse source registers to avoid running out of temps.
      FreeTemp(rl_src.reg);
    }
    rl_src = rl_result;
  }

  // abs(x) = y<=x>>31, (x+y)^y.
  RegStorage sign_reg;
  if (cu_->instruction_set == kX86_64) {
    sign_reg = AllocTempWide();
    OpRegRegImm(kOpAsr, sign_reg, rl_src.reg, 63);
    OpRegRegReg(kOpAdd, rl_result.reg, rl_src.reg, sign_reg);
    OpRegReg(kOpXor, rl_result.reg, sign_reg);
  } else {
    sign_reg = AllocTemp();
    OpRegRegImm(kOpAsr, sign_reg, rl_src.reg.GetHigh(), 31);
    OpRegRegReg(kOpAdd, rl_result.reg.GetLow(), rl_src.reg.GetLow(), sign_reg);
    OpRegRegReg(kOpAdc, rl_result.reg.GetHigh(), rl_src.reg.GetHigh(), sign_reg);
    OpRegReg(kOpXor, rl_result.reg.GetLow(), sign_reg);
    OpRegReg(kOpXor, rl_result.reg.GetHigh(), sign_reg);
  }
  FreeTemp(sign_reg);
  StoreValueWide(rl_dest, rl_result);
  return true;
}

bool Mir2Lir::GenInlinedReverseBits(CallInfo* info, OpSize size) {
  // Currently implemented only for ARM64
  return false;
}

bool Mir2Lir::GenInlinedMinMaxFP(CallInfo* info, bool is_min, bool is_double) {
  // Currently implemented only for ARM64
  return false;
}

bool Mir2Lir::GenInlinedCeil(CallInfo* info) {
  return false;
}

bool Mir2Lir::GenInlinedFloor(CallInfo* info) {
  return false;
}

bool Mir2Lir::GenInlinedRint(CallInfo* info) {
  return false;
}

bool Mir2Lir::GenInlinedRound(CallInfo* info, bool is_double) {
  return false;
}

bool Mir2Lir::GenInlinedFloatCvt(CallInfo* info) {
  if (cu_->instruction_set == kMips) {
    // TODO - add Mips implementation
    return false;
  }
  RegLocation rl_src = info->args[0];
  RegLocation rl_dest = InlineTarget(info);
  StoreValue(rl_dest, rl_src);
  return true;
}

bool Mir2Lir::GenInlinedDoubleCvt(CallInfo* info) {
  if (cu_->instruction_set == kMips) {
    // TODO - add Mips implementation
    return false;
  }
  RegLocation rl_src = info->args[0];
  RegLocation rl_dest = InlineTargetWide(info);
  StoreValueWide(rl_dest, rl_src);
  return true;
}

bool Mir2Lir::GenInlinedArrayCopyCharArray(CallInfo* info) {
  return false;
}


/*
 * Fast String.indexOf(I) & (II).  Tests for simple case of char <= 0xFFFF,
 * otherwise bails to standard library code.
 */
bool Mir2Lir::GenInlinedIndexOf(CallInfo* info, bool zero_based) {
  if (cu_->instruction_set == kMips) {
    // TODO - add Mips implementation
    return false;
  }
  if (cu_->instruction_set == kX86_64) {
    // TODO - add kX86_64 implementation
    return false;
  }
  RegLocation rl_obj = info->args[0];
  RegLocation rl_char = info->args[1];
  if (rl_char.is_const && (mir_graph_->ConstantValue(rl_char) & ~0xFFFF) != 0) {
    // Code point beyond 0xFFFF. Punt to the real String.indexOf().
    return false;
  }

  ClobberCallerSave();
  LockCallTemps();  // Using fixed registers
  RegStorage reg_ptr = TargetReg(kArg0, kRef);
  RegStorage reg_char = TargetReg(kArg1, kNotWide);
  RegStorage reg_start = TargetReg(kArg2, kNotWide);

  LoadValueDirectFixed(rl_obj, reg_ptr);
  LoadValueDirectFixed(rl_char, reg_char);
  if (zero_based) {
    LoadConstant(reg_start, 0);
  } else {
    RegLocation rl_start = info->args[2];     // 3rd arg only present in III flavor of IndexOf.
    LoadValueDirectFixed(rl_start, reg_start);
  }
  RegStorage r_tgt = LoadHelper(kQuickIndexOf);
  GenExplicitNullCheck(reg_ptr, info->opt_flags);
  LIR* high_code_point_branch =
      rl_char.is_const ? nullptr : OpCmpImmBranch(kCondGt, reg_char, 0xFFFF, nullptr);
  // NOTE: not a safepoint
  OpReg(kOpBlx, r_tgt);
  if (!rl_char.is_const) {
    // Add the slow path for code points beyond 0xFFFF.
    DCHECK(high_code_point_branch != nullptr);
    LIR* resume_tgt = NewLIR0(kPseudoTargetLabel);
    info->opt_flags |= MIR_IGNORE_NULL_CHECK;  // Record that we've null checked.
    AddIntrinsicSlowPath(info, high_code_point_branch, resume_tgt);
  } else {
    DCHECK_EQ(mir_graph_->ConstantValue(rl_char) & ~0xFFFF, 0);
    DCHECK(high_code_point_branch == nullptr);
  }
  RegLocation rl_return = GetReturn(kCoreReg);
  RegLocation rl_dest = InlineTarget(info);
  StoreValue(rl_dest, rl_return);
  return true;
}

/* Fast string.compareTo(Ljava/lang/string;)I. */
bool Mir2Lir::GenInlinedStringCompareTo(CallInfo* info) {
  if (cu_->instruction_set == kMips) {
    // TODO - add Mips implementation
    return false;
  }
  ClobberCallerSave();
  LockCallTemps();  // Using fixed registers
  RegStorage reg_this = TargetReg(kArg0, kRef);
  RegStorage reg_cmp = TargetReg(kArg1, kRef);

  RegLocation rl_this = info->args[0];
  RegLocation rl_cmp = info->args[1];
  LoadValueDirectFixed(rl_this, reg_this);
  LoadValueDirectFixed(rl_cmp, reg_cmp);
  RegStorage r_tgt;
  if (cu_->instruction_set != kX86 && cu_->instruction_set != kX86_64) {
    r_tgt = LoadHelper(kQuickStringCompareTo);
  } else {
    r_tgt = RegStorage::InvalidReg();
  }
  GenExplicitNullCheck(reg_this, info->opt_flags);
  info->opt_flags |= MIR_IGNORE_NULL_CHECK;  // Record that we've null checked.
  // TUNING: check if rl_cmp.s_reg_low is already null checked
  LIR* cmp_null_check_branch = OpCmpImmBranch(kCondEq, reg_cmp, 0, nullptr);
  AddIntrinsicSlowPath(info, cmp_null_check_branch);
  // NOTE: not a safepoint
  CallHelper(r_tgt, kQuickStringCompareTo, false, true);
  RegLocation rl_return = GetReturn(kCoreReg);
  RegLocation rl_dest = InlineTarget(info);
  StoreValue(rl_dest, rl_return);
  return true;
}

bool Mir2Lir::GenInlinedCurrentThread(CallInfo* info) {
  RegLocation rl_dest = InlineTarget(info);

  // Early exit if the result is unused.
  if (rl_dest.orig_sreg < 0) {
    return true;
  }

  RegLocation rl_result = EvalLoc(rl_dest, kRefReg, true);

  switch (cu_->instruction_set) {
    case kArm:
      // Fall-through.
    case kThumb2:
      // Fall-through.
    case kMips:
      Load32Disp(TargetPtrReg(kSelf), Thread::PeerOffset<4>().Int32Value(), rl_result.reg);
      break;

    case kArm64:
      LoadRefDisp(TargetPtrReg(kSelf), Thread::PeerOffset<8>().Int32Value(), rl_result.reg,
                  kNotVolatile);
      break;

    default:
      LOG(FATAL) << "Unexpected isa " << cu_->instruction_set;
  }
  StoreValue(rl_dest, rl_result);
  return true;
}

bool Mir2Lir::GenInlinedUnsafeGet(CallInfo* info,
                                  bool is_long, bool is_volatile) {
  if (cu_->instruction_set == kMips) {
    // TODO - add Mips implementation
    return false;
  }
  // Unused - RegLocation rl_src_unsafe = info->args[0];
  RegLocation rl_src_obj = info->args[1];  // Object
  RegLocation rl_src_offset = info->args[2];  // long low
  rl_src_offset = NarrowRegLoc(rl_src_offset);  // ignore high half in info->args[3]
  RegLocation rl_dest = is_long ? InlineTargetWide(info) : InlineTarget(info);  // result reg

  RegLocation rl_object = LoadValue(rl_src_obj, kRefReg);
  RegLocation rl_offset = LoadValue(rl_src_offset, kCoreReg);
  RegLocation rl_result = EvalLoc(rl_dest, LocToRegClass(rl_dest), true);
  if (is_long) {
    if (cu_->instruction_set == kX86 || cu_->instruction_set == kX86_64
        || cu_->instruction_set == kArm64) {
      LoadBaseIndexed(rl_object.reg, rl_offset.reg, rl_result.reg, 0, k64);
    } else {
      RegStorage rl_temp_offset = AllocTemp();
      OpRegRegReg(kOpAdd, rl_temp_offset, rl_object.reg, rl_offset.reg);
      LoadBaseDisp(rl_temp_offset, 0, rl_result.reg, k64, kNotVolatile);
      FreeTemp(rl_temp_offset);
    }
  } else {
    if (rl_result.ref) {
      LoadRefIndexed(rl_object.reg, rl_offset.reg, rl_result.reg, 0);
    } else {
      LoadBaseIndexed(rl_object.reg, rl_offset.reg, rl_result.reg, 0, k32);
    }
  }

  if (is_volatile) {
    GenMemBarrier(kLoadAny);
  }

  if (is_long) {
    StoreValueWide(rl_dest, rl_result);
  } else {
    StoreValue(rl_dest, rl_result);
  }
  return true;
}

bool Mir2Lir::GenInlinedUnsafePut(CallInfo* info, bool is_long,
                                  bool is_object, bool is_volatile, bool is_ordered) {
  if (cu_->instruction_set == kMips) {
    // TODO - add Mips implementation
    return false;
  }
  // Unused - RegLocation rl_src_unsafe = info->args[0];
  RegLocation rl_src_obj = info->args[1];  // Object
  RegLocation rl_src_offset = info->args[2];  // long low
  rl_src_offset = NarrowRegLoc(rl_src_offset);  // ignore high half in info->args[3]
  RegLocation rl_src_value = info->args[4];  // value to store
  if (is_volatile || is_ordered) {
    GenMemBarrier(kAnyStore);
  }
  RegLocation rl_object = LoadValue(rl_src_obj, kRefReg);
  RegLocation rl_offset = LoadValue(rl_src_offset, kCoreReg);
  RegLocation rl_value;
  if (is_long) {
    rl_value = LoadValueWide(rl_src_value, kCoreReg);
    if (cu_->instruction_set == kX86 || cu_->instruction_set == kX86_64
        || cu_->instruction_set == kArm64) {
      StoreBaseIndexed(rl_object.reg, rl_offset.reg, rl_value.reg, 0, k64);
    } else {
      RegStorage rl_temp_offset = AllocTemp();
      OpRegRegReg(kOpAdd, rl_temp_offset, rl_object.reg, rl_offset.reg);
      StoreBaseDisp(rl_temp_offset, 0, rl_value.reg, k64, kNotVolatile);
      FreeTemp(rl_temp_offset);
    }
  } else {
    rl_value = LoadValue(rl_src_value);
    if (rl_value.ref) {
      StoreRefIndexed(rl_object.reg, rl_offset.reg, rl_value.reg, 0);
    } else {
      StoreBaseIndexed(rl_object.reg, rl_offset.reg, rl_value.reg, 0, k32);
    }
  }

  // Free up the temp early, to ensure x86 doesn't run out of temporaries in MarkGCCard.
  FreeTemp(rl_offset.reg);

  if (is_volatile) {
    // Prevent reordering with a subsequent volatile load.
    // May also be needed to address store atomicity issues.
    GenMemBarrier(kAnyAny);
  }
  if (is_object) {
    MarkGCCard(rl_value.reg, rl_object.reg);
  }
  return true;
}

void Mir2Lir::GenInvoke(CallInfo* info) {
  if ((info->opt_flags & MIR_INLINED) != 0) {
    // Already inlined but we may still need the null check.
    if (info->type != kStatic &&
        ((cu_->disable_opt & (1 << kNullCheckElimination)) != 0 ||
         (info->opt_flags & MIR_IGNORE_NULL_CHECK) == 0))  {
      RegLocation rl_obj = LoadValue(info->args[0], kRefReg);
      GenNullCheck(rl_obj.reg);
    }
    return;
  }
  DCHECK(cu_->compiler_driver->GetMethodInlinerMap() != nullptr);
  if (cu_->compiler_driver->GetMethodInlinerMap()->GetMethodInliner(cu_->dex_file)
      ->GenIntrinsic(this, info)) {
    return;
  }
  GenInvokeNoInline(info);
}

static LIR* GenInvokeNoInlineCall(Mir2Lir* mir_to_lir, InvokeType type) {
  QuickEntrypointEnum trampoline;
  switch (type) {
    case kInterface:
      trampoline = kQuickInvokeInterfaceTrampolineWithAccessCheck;
      break;
    case kDirect:
      trampoline = kQuickInvokeDirectTrampolineWithAccessCheck;
      break;
    case kStatic:
      trampoline = kQuickInvokeStaticTrampolineWithAccessCheck;
      break;
    case kSuper:
      trampoline = kQuickInvokeSuperTrampolineWithAccessCheck;
      break;
    case kVirtual:
      trampoline = kQuickInvokeVirtualTrampolineWithAccessCheck;
      break;
    default:
      LOG(FATAL) << "Unexpected invoke type";
      trampoline = kQuickInvokeInterfaceTrampolineWithAccessCheck;
  }
  return mir_to_lir->InvokeTrampoline(kOpBlx, RegStorage::InvalidReg(), trampoline);
}

void Mir2Lir::GenInvokeNoInline(CallInfo* info) {
  int call_state = 0;
  LIR* null_ck;
  LIR** p_null_ck = NULL;
  NextCallInsn next_call_insn;
  FlushAllRegs();  /* Everything to home location */
  // Explicit register usage
  LockCallTemps();

  const MirMethodLoweringInfo& method_info = mir_graph_->GetMethodLoweringInfo(info->mir);
  cu_->compiler_driver->ProcessedInvoke(method_info.GetInvokeType(), method_info.StatsFlags());
  BeginInvoke(info);
  InvokeType original_type = static_cast<InvokeType>(method_info.GetInvokeType());
  info->type = static_cast<InvokeType>(method_info.GetSharpType());
  bool fast_path = method_info.FastPath();
  bool skip_this;
  if (info->type == kInterface) {
    next_call_insn = fast_path ? NextInterfaceCallInsn : NextInterfaceCallInsnWithAccessCheck;
    skip_this = fast_path;
  } else if (info->type == kDirect) {
    if (fast_path) {
      p_null_ck = &null_ck;
    }
    next_call_insn = fast_path ? NextSDCallInsn : NextDirectCallInsnSP;
    skip_this = false;
  } else if (info->type == kStatic) {
    next_call_insn = fast_path ? NextSDCallInsn : NextStaticCallInsnSP;
    skip_this = false;
  } else if (info->type == kSuper) {
    DCHECK(!fast_path);  // Fast path is a direct call.
    next_call_insn = NextSuperCallInsnSP;
    skip_this = false;
  } else {
    DCHECK_EQ(info->type, kVirtual);
    next_call_insn = fast_path ? NextVCallInsn : NextVCallInsnSP;
    skip_this = fast_path;
  }
  MethodReference target_method = method_info.GetTargetMethod();
  if (!info->is_range) {
    call_state = GenDalvikArgsNoRange(info, call_state, p_null_ck,
                                      next_call_insn, target_method, method_info.VTableIndex(),
                                      method_info.DirectCode(), method_info.DirectMethod(),
                                      original_type, skip_this);
  } else {
    call_state = GenDalvikArgsRange(info, call_state, p_null_ck,
                                    next_call_insn, target_method, method_info.VTableIndex(),
                                    method_info.DirectCode(), method_info.DirectMethod(),
                                    original_type, skip_this);
  }
  // Finish up any of the call sequence not interleaved in arg loading
  while (call_state >= 0) {
    call_state = next_call_insn(cu_, info, call_state, target_method, method_info.VTableIndex(),
                                method_info.DirectCode(), method_info.DirectMethod(), original_type);
  }
  LIR* call_inst;
  if (cu_->instruction_set != kX86 && cu_->instruction_set != kX86_64) {
    call_inst = OpReg(kOpBlx, TargetPtrReg(kInvokeTgt));
  } else {
    if (fast_path) {
      if (method_info.DirectCode() == static_cast<uintptr_t>(-1)) {
        // We can have the linker fixup a call relative.
        call_inst =
          reinterpret_cast<X86Mir2Lir*>(this)->CallWithLinkerFixup(target_method, info->type);
      } else {
        call_inst = OpMem(kOpBlx, TargetReg(kArg0, kRef),
                          mirror::ArtMethod::EntryPointFromQuickCompiledCodeOffset().Int32Value());
      }
    } else {
      call_inst = GenInvokeNoInlineCall(this, info->type);
    }
  }
  EndInvoke(info);
  MarkSafepointPC(call_inst);

  ClobberCallerSave();
  if (info->result.location != kLocInvalid) {
    // We have a following MOVE_RESULT - do it now.
    if (info->result.wide) {
      RegLocation ret_loc = GetReturnWide(LocToRegClass(info->result));
      StoreValueWide(info->result, ret_loc);
    } else {
      RegLocation ret_loc = GetReturn(LocToRegClass(info->result));
      StoreValue(info->result, ret_loc);
    }
  }
}

}  // namespace art
