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
#include "mirror/object_array-inl.h"
#include "mirror/string.h"
#include "mir_to_lir-inl.h"
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

// Macro to help instantiate.
// TODO: This might be used to only instantiate <4> on pure 32b systems.
#define INSTANTIATE(sig_part1, ...) \
  template sig_part1(ThreadOffset<4>, __VA_ARGS__); \
  template sig_part1(ThreadOffset<8>, __VA_ARGS__); \


/*
 * To save scheduling time, helper calls are broken into two parts: generation of
 * the helper target address, and the actual call to the helper.  Because x86
 * has a memory call operation, part 1 is a NOP for x86.  For other targets,
 * load arguments between the two parts.
 */
// template <size_t pointer_size>
RegStorage Mir2Lir::CallHelperSetup(ThreadOffset<4> helper_offset) {
  // All CallRuntimeHelperXXX call this first. So make a central check here.
  DCHECK_EQ(4U, GetInstructionSetPointerSize(cu_->instruction_set));

  if (cu_->instruction_set == kX86 || cu_->instruction_set == kX86_64) {
    return RegStorage::InvalidReg();
  } else {
    return LoadHelper(helper_offset);
  }
}

RegStorage Mir2Lir::CallHelperSetup(ThreadOffset<8> helper_offset) {
  // All CallRuntimeHelperXXX call this first. So make a central check here.
  DCHECK_EQ(8U, GetInstructionSetPointerSize(cu_->instruction_set));

  if (cu_->instruction_set == kX86 || cu_->instruction_set == kX86_64) {
    return RegStorage::InvalidReg();
  } else {
    return LoadHelper(helper_offset);
  }
}

/* NOTE: if r_tgt is a temp, it will be freed following use */
template <size_t pointer_size>
LIR* Mir2Lir::CallHelper(RegStorage r_tgt, ThreadOffset<pointer_size> helper_offset,
                         bool safepoint_pc, bool use_link) {
  LIR* call_inst;
  OpKind op = use_link ? kOpBlx : kOpBx;
  if (cu_->instruction_set == kX86 || cu_->instruction_set == kX86_64) {
    call_inst = OpThreadMem(op, helper_offset);
  } else {
    call_inst = OpReg(op, r_tgt);
    FreeTemp(r_tgt);
  }
  if (safepoint_pc) {
    MarkSafepointPC(call_inst);
  }
  return call_inst;
}
template LIR* Mir2Lir::CallHelper(RegStorage r_tgt, ThreadOffset<4> helper_offset,
                                        bool safepoint_pc, bool use_link);
template LIR* Mir2Lir::CallHelper(RegStorage r_tgt, ThreadOffset<8> helper_offset,
                                        bool safepoint_pc, bool use_link);

template <size_t pointer_size>
void Mir2Lir::CallRuntimeHelper(ThreadOffset<pointer_size> helper_offset, bool safepoint_pc) {
  RegStorage r_tgt = CallHelperSetup(helper_offset);
  ClobberCallerSave();
  CallHelper<pointer_size>(r_tgt, helper_offset, safepoint_pc);
}
INSTANTIATE(void Mir2Lir::CallRuntimeHelper, bool safepoint_pc)

template <size_t pointer_size>
void Mir2Lir::CallRuntimeHelperImm(ThreadOffset<pointer_size> helper_offset, int arg0, bool safepoint_pc) {
  RegStorage r_tgt = CallHelperSetup(helper_offset);
  LoadConstant(TargetReg(kArg0), arg0);
  ClobberCallerSave();
  CallHelper<pointer_size>(r_tgt, helper_offset, safepoint_pc);
}
INSTANTIATE(void Mir2Lir::CallRuntimeHelperImm, int arg0, bool safepoint_pc)

template <size_t pointer_size>
void Mir2Lir::CallRuntimeHelperReg(ThreadOffset<pointer_size> helper_offset, RegStorage arg0,
                                   bool safepoint_pc) {
  RegStorage r_tgt = CallHelperSetup(helper_offset);
  OpRegCopy(TargetReg(kArg0), arg0);
  ClobberCallerSave();
  CallHelper<pointer_size>(r_tgt, helper_offset, safepoint_pc);
}
INSTANTIATE(void Mir2Lir::CallRuntimeHelperReg, RegStorage arg0, bool safepoint_pc)

template <size_t pointer_size>
void Mir2Lir::CallRuntimeHelperRegLocation(ThreadOffset<pointer_size> helper_offset,
                                           RegLocation arg0, bool safepoint_pc) {
  RegStorage r_tgt = CallHelperSetup(helper_offset);
  if (arg0.wide == 0) {
    LoadValueDirectFixed(arg0, TargetReg(kArg0));
  } else {
    RegStorage r_tmp = RegStorage::MakeRegPair(TargetReg(kArg0), TargetReg(kArg1));
    LoadValueDirectWideFixed(arg0, r_tmp);
  }
  ClobberCallerSave();
  CallHelper<pointer_size>(r_tgt, helper_offset, safepoint_pc);
}
INSTANTIATE(void Mir2Lir::CallRuntimeHelperRegLocation, RegLocation arg0, bool safepoint_pc)

template <size_t pointer_size>
void Mir2Lir::CallRuntimeHelperImmImm(ThreadOffset<pointer_size> helper_offset, int arg0, int arg1,
                                      bool safepoint_pc) {
  RegStorage r_tgt = CallHelperSetup(helper_offset);
  LoadConstant(TargetReg(kArg0), arg0);
  LoadConstant(TargetReg(kArg1), arg1);
  ClobberCallerSave();
  CallHelper<pointer_size>(r_tgt, helper_offset, safepoint_pc);
}
INSTANTIATE(void Mir2Lir::CallRuntimeHelperImmImm, int arg0, int arg1, bool safepoint_pc)

template <size_t pointer_size>
void Mir2Lir::CallRuntimeHelperImmRegLocation(ThreadOffset<pointer_size> helper_offset, int arg0,
                                              RegLocation arg1, bool safepoint_pc) {
  RegStorage r_tgt = CallHelperSetup(helper_offset);
  if (arg1.wide == 0) {
    LoadValueDirectFixed(arg1, TargetReg(kArg1));
  } else {
    RegStorage r_tmp = RegStorage::MakeRegPair(TargetReg(kArg1), TargetReg(kArg2));
    LoadValueDirectWideFixed(arg1, r_tmp);
  }
  LoadConstant(TargetReg(kArg0), arg0);
  ClobberCallerSave();
  CallHelper<pointer_size>(r_tgt, helper_offset, safepoint_pc);
}
INSTANTIATE(void Mir2Lir::CallRuntimeHelperImmRegLocation, int arg0, RegLocation arg1,
            bool safepoint_pc)

template <size_t pointer_size>
void Mir2Lir::CallRuntimeHelperRegLocationImm(ThreadOffset<pointer_size> helper_offset,
                                              RegLocation arg0, int arg1, bool safepoint_pc) {
  RegStorage r_tgt = CallHelperSetup(helper_offset);
  LoadValueDirectFixed(arg0, TargetReg(kArg0));
  LoadConstant(TargetReg(kArg1), arg1);
  ClobberCallerSave();
  CallHelper<pointer_size>(r_tgt, helper_offset, safepoint_pc);
}
INSTANTIATE(void Mir2Lir::CallRuntimeHelperRegLocationImm, RegLocation arg0, int arg1,
            bool safepoint_pc)

template <size_t pointer_size>
void Mir2Lir::CallRuntimeHelperImmReg(ThreadOffset<pointer_size> helper_offset, int arg0,
                                      RegStorage arg1, bool safepoint_pc) {
  RegStorage r_tgt = CallHelperSetup(helper_offset);
  OpRegCopy(TargetReg(kArg1), arg1);
  LoadConstant(TargetReg(kArg0), arg0);
  ClobberCallerSave();
  CallHelper<pointer_size>(r_tgt, helper_offset, safepoint_pc);
}
INSTANTIATE(void Mir2Lir::CallRuntimeHelperImmReg, int arg0, RegStorage arg1, bool safepoint_pc)

template <size_t pointer_size>
void Mir2Lir::CallRuntimeHelperRegImm(ThreadOffset<pointer_size> helper_offset, RegStorage arg0,
                                      int arg1, bool safepoint_pc) {
  RegStorage r_tgt = CallHelperSetup(helper_offset);
  OpRegCopy(TargetReg(kArg0), arg0);
  LoadConstant(TargetReg(kArg1), arg1);
  ClobberCallerSave();
  CallHelper<pointer_size>(r_tgt, helper_offset, safepoint_pc);
}
INSTANTIATE(void Mir2Lir::CallRuntimeHelperRegImm, RegStorage arg0, int arg1, bool safepoint_pc)

template <size_t pointer_size>
void Mir2Lir::CallRuntimeHelperImmMethod(ThreadOffset<pointer_size> helper_offset, int arg0,
                                         bool safepoint_pc) {
  RegStorage r_tgt = CallHelperSetup(helper_offset);
  LoadCurrMethodDirect(TargetReg(kArg1));
  LoadConstant(TargetReg(kArg0), arg0);
  ClobberCallerSave();
  CallHelper<pointer_size>(r_tgt, helper_offset, safepoint_pc);
}
INSTANTIATE(void Mir2Lir::CallRuntimeHelperImmMethod, int arg0, bool safepoint_pc)

template <size_t pointer_size>
void Mir2Lir::CallRuntimeHelperRegMethod(ThreadOffset<pointer_size> helper_offset, RegStorage arg0,
                                         bool safepoint_pc) {
  RegStorage r_tgt = CallHelperSetup(helper_offset);
  DCHECK_NE(TargetReg(kArg1).GetReg(), arg0.GetReg());
  if (TargetReg(kArg0) != arg0) {
    OpRegCopy(TargetReg(kArg0), arg0);
  }
  LoadCurrMethodDirect(TargetReg(kArg1));
  ClobberCallerSave();
  CallHelper<pointer_size>(r_tgt, helper_offset, safepoint_pc);
}
INSTANTIATE(void Mir2Lir::CallRuntimeHelperRegMethod, RegStorage arg0, bool safepoint_pc)

template <size_t pointer_size>
void Mir2Lir::CallRuntimeHelperRegMethodRegLocation(ThreadOffset<pointer_size> helper_offset,
                                                    RegStorage arg0, RegLocation arg2,
                                                    bool safepoint_pc) {
  RegStorage r_tgt = CallHelperSetup(helper_offset);
  DCHECK_NE(TargetReg(kArg1).GetReg(), arg0.GetReg());
  if (TargetReg(kArg0) != arg0) {
    OpRegCopy(TargetReg(kArg0), arg0);
  }
  LoadCurrMethodDirect(TargetReg(kArg1));
  LoadValueDirectFixed(arg2, TargetReg(kArg2));
  ClobberCallerSave();
  CallHelper<pointer_size>(r_tgt, helper_offset, safepoint_pc);
}
INSTANTIATE(void Mir2Lir::CallRuntimeHelperRegMethodRegLocation, RegStorage arg0, RegLocation arg2,
            bool safepoint_pc)

template <size_t pointer_size>
void Mir2Lir::CallRuntimeHelperRegLocationRegLocation(ThreadOffset<pointer_size> helper_offset,
                                                      RegLocation arg0, RegLocation arg1,
                                                      bool safepoint_pc) {
  RegStorage r_tgt = CallHelperSetup(helper_offset);
  if (arg0.wide == 0) {
    LoadValueDirectFixed(arg0, arg0.fp ? TargetReg(kFArg0) : TargetReg(kArg0));
    if (arg1.wide == 0) {
      if (cu_->instruction_set == kMips) {
        LoadValueDirectFixed(arg1, arg1.fp ? TargetReg(kFArg2) : TargetReg(kArg1));
      } else {
        LoadValueDirectFixed(arg1, TargetReg(kArg1));
      }
    } else {
      if (cu_->instruction_set == kMips) {
        RegStorage r_tmp;
        if (arg1.fp) {
          r_tmp = RegStorage::MakeRegPair(TargetReg(kFArg2), TargetReg(kFArg3));
        } else {
          r_tmp = RegStorage::MakeRegPair(TargetReg(kArg1), TargetReg(kArg2));
        }
        LoadValueDirectWideFixed(arg1, r_tmp);
      } else {
        RegStorage r_tmp = RegStorage::MakeRegPair(TargetReg(kArg1), TargetReg(kArg2));
        LoadValueDirectWideFixed(arg1, r_tmp);
      }
    }
  } else {
    RegStorage r_tmp;
    if (arg0.fp) {
      r_tmp = RegStorage::MakeRegPair(TargetReg(kFArg0), TargetReg(kFArg1));
    } else {
      r_tmp = RegStorage::MakeRegPair(TargetReg(kArg0), TargetReg(kArg1));
    }
    LoadValueDirectWideFixed(arg0, r_tmp);
    if (arg1.wide == 0) {
      LoadValueDirectFixed(arg1, arg1.fp ? TargetReg(kFArg2) : TargetReg(kArg2));
    } else {
      RegStorage r_tmp;
      if (arg1.fp) {
        r_tmp = RegStorage::MakeRegPair(TargetReg(kFArg2), TargetReg(kFArg3));
      } else {
        r_tmp = RegStorage::MakeRegPair(TargetReg(kArg2), TargetReg(kArg3));
      }
      LoadValueDirectWideFixed(arg1, r_tmp);
    }
  }
  ClobberCallerSave();
  CallHelper<pointer_size>(r_tgt, helper_offset, safepoint_pc);
}
INSTANTIATE(void Mir2Lir::CallRuntimeHelperRegLocationRegLocation, RegLocation arg0,
            RegLocation arg1, bool safepoint_pc)

void Mir2Lir::CopyToArgumentRegs(RegStorage arg0, RegStorage arg1) {
  if (arg1.GetReg() == TargetReg(kArg0).GetReg()) {
    if (arg0.GetReg() == TargetReg(kArg1).GetReg()) {
      // Swap kArg0 and kArg1 with kArg2 as temp.
      OpRegCopy(TargetReg(kArg2), arg1);
      OpRegCopy(TargetReg(kArg0), arg0);
      OpRegCopy(TargetReg(kArg1), TargetReg(kArg2));
    } else {
      OpRegCopy(TargetReg(kArg1), arg1);
      OpRegCopy(TargetReg(kArg0), arg0);
    }
  } else {
    OpRegCopy(TargetReg(kArg0), arg0);
    OpRegCopy(TargetReg(kArg1), arg1);
  }
}

template <size_t pointer_size>
void Mir2Lir::CallRuntimeHelperRegReg(ThreadOffset<pointer_size> helper_offset, RegStorage arg0,
                                      RegStorage arg1, bool safepoint_pc) {
  RegStorage r_tgt = CallHelperSetup(helper_offset);
  CopyToArgumentRegs(arg0, arg1);
  ClobberCallerSave();
  CallHelper<pointer_size>(r_tgt, helper_offset, safepoint_pc);
}
INSTANTIATE(void Mir2Lir::CallRuntimeHelperRegReg, RegStorage arg0, RegStorage arg1,
            bool safepoint_pc)

template <size_t pointer_size>
void Mir2Lir::CallRuntimeHelperRegRegImm(ThreadOffset<pointer_size> helper_offset, RegStorage arg0,
                                         RegStorage arg1, int arg2, bool safepoint_pc) {
  RegStorage r_tgt = CallHelperSetup(helper_offset);
  CopyToArgumentRegs(arg0, arg1);
  LoadConstant(TargetReg(kArg2), arg2);
  ClobberCallerSave();
  CallHelper<pointer_size>(r_tgt, helper_offset, safepoint_pc);
}
INSTANTIATE(void Mir2Lir::CallRuntimeHelperRegRegImm, RegStorage arg0, RegStorage arg1, int arg2,
            bool safepoint_pc)

template <size_t pointer_size>
void Mir2Lir::CallRuntimeHelperImmMethodRegLocation(ThreadOffset<pointer_size> helper_offset,
                                                    int arg0, RegLocation arg2, bool safepoint_pc) {
  RegStorage r_tgt = CallHelperSetup(helper_offset);
  LoadValueDirectFixed(arg2, TargetReg(kArg2));
  LoadCurrMethodDirect(TargetReg(kArg1));
  LoadConstant(TargetReg(kArg0), arg0);
  ClobberCallerSave();
  CallHelper<pointer_size>(r_tgt, helper_offset, safepoint_pc);
}
INSTANTIATE(void Mir2Lir::CallRuntimeHelperImmMethodRegLocation, int arg0, RegLocation arg2,
            bool safepoint_pc)

template <size_t pointer_size>
void Mir2Lir::CallRuntimeHelperImmMethodImm(ThreadOffset<pointer_size> helper_offset, int arg0,
                                            int arg2, bool safepoint_pc) {
  RegStorage r_tgt = CallHelperSetup(helper_offset);
  LoadCurrMethodDirect(TargetReg(kArg1));
  LoadConstant(TargetReg(kArg2), arg2);
  LoadConstant(TargetReg(kArg0), arg0);
  ClobberCallerSave();
  CallHelper<pointer_size>(r_tgt, helper_offset, safepoint_pc);
}
INSTANTIATE(void Mir2Lir::CallRuntimeHelperImmMethodImm, int arg0, int arg2, bool safepoint_pc)

template <size_t pointer_size>
void Mir2Lir::CallRuntimeHelperImmRegLocationRegLocation(ThreadOffset<pointer_size> helper_offset,
                                                         int arg0, RegLocation arg1,
                                                         RegLocation arg2, bool safepoint_pc) {
  RegStorage r_tgt = CallHelperSetup(helper_offset);
  DCHECK_EQ(static_cast<unsigned int>(arg1.wide), 0U);  // The static_cast works around an
                                                        // instantiation bug in GCC.
  LoadValueDirectFixed(arg1, TargetReg(kArg1));
  if (arg2.wide == 0) {
    LoadValueDirectFixed(arg2, TargetReg(kArg2));
  } else {
    RegStorage r_tmp = RegStorage::MakeRegPair(TargetReg(kArg2), TargetReg(kArg3));
    LoadValueDirectWideFixed(arg2, r_tmp);
  }
  LoadConstant(TargetReg(kArg0), arg0);
  ClobberCallerSave();
  CallHelper<pointer_size>(r_tgt, helper_offset, safepoint_pc);
}
INSTANTIATE(void Mir2Lir::CallRuntimeHelperImmRegLocationRegLocation, int arg0, RegLocation arg1,
            RegLocation arg2, bool safepoint_pc)

template <size_t pointer_size>
void Mir2Lir::CallRuntimeHelperRegLocationRegLocationRegLocation(ThreadOffset<pointer_size> helper_offset,
                                                                 RegLocation arg0, RegLocation arg1,
                                                                 RegLocation arg2,
                                                                 bool safepoint_pc) {
  RegStorage r_tgt = CallHelperSetup(helper_offset);
  DCHECK_EQ(static_cast<unsigned int>(arg0.wide), 0U);
  LoadValueDirectFixed(arg0, TargetReg(kArg0));
  DCHECK_EQ(static_cast<unsigned int>(arg1.wide), 0U);
  LoadValueDirectFixed(arg1, TargetReg(kArg1));
  DCHECK_EQ(static_cast<unsigned int>(arg1.wide), 0U);
  LoadValueDirectFixed(arg2, TargetReg(kArg2));
  ClobberCallerSave();
  CallHelper<pointer_size>(r_tgt, helper_offset, safepoint_pc);
}
INSTANTIATE(void Mir2Lir::CallRuntimeHelperRegLocationRegLocationRegLocation, RegLocation arg0,
            RegLocation arg1, RegLocation arg2, bool safepoint_pc)

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
   * Dummy up a RegLocation for the incoming Method*
   * It will attempt to keep kArg0 live (or copy it to home location
   * if promoted).
   */
  RegLocation rl_src = rl_method;
  rl_src.location = kLocPhysReg;
  rl_src.reg = TargetReg(kArg0);
  rl_src.home = false;
  MarkLive(rl_src);
  if (rl_method.wide) {
    StoreValueWide(rl_method, rl_src);
  } else {
    StoreValue(rl_method, rl_src);
  }
  // If Method* has been promoted, explicitly flush
  if (rl_method.location == kLocPhysReg) {
    StoreWordDisp(TargetReg(kSp), 0, TargetReg(kArg0));
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
        OpRegCopy(RegStorage::Solo32(v_map->FpReg), reg);
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
          int low_reg = promotion_map_[lowreg_index].FpReg;
          int high_reg = promotion_map_[lowreg_index + 1].FpReg;
          if (((low_reg & 0x1) != 0) || (high_reg != (low_reg + 1))) {
            need_flush = true;
          }
        }
      }
      if (need_flush) {
        Store32Disp(TargetReg(kSp), SRegOffset(start_vreg + i), reg);
      }
    } else {
      // If arriving in frame & promoted
      if (v_map->core_location == kLocPhysReg) {
        Load32Disp(TargetReg(kSp), SRegOffset(start_vreg + i), RegStorage::Solo32(v_map->core_reg));
      }
      if (v_map->fp_location == kLocPhysReg) {
        Load32Disp(TargetReg(kSp), SRegOffset(start_vreg + i), RegStorage::Solo32(v_map->FpReg));
      }
    }
  }
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
          cg->LoadConstant(cg->TargetReg(kInvokeTgt), direct_code);
        }
      } else if (cu->instruction_set != kX86 && cu->instruction_set != kX86_64) {
        cg->LoadCodeAddress(target_method, type, kInvokeTgt);
      }
      if (direct_method != static_cast<uintptr_t>(-1)) {
        cg->LoadConstant(cg->TargetReg(kArg0), direct_method);
      } else {
        cg->LoadMethodAddress(target_method, type, kArg0);
      }
      break;
    default:
      return -1;
    }
  } else {
    switch (state) {
    case 0:  // Get the current Method* [sets kArg0]
      // TUNING: we can save a reg copy if Method* has been promoted.
      cg->LoadCurrMethodDirect(cg->TargetReg(kArg0));
      break;
    case 1:  // Get method->dex_cache_resolved_methods_
      cg->LoadRefDisp(cg->TargetReg(kArg0),
                      mirror::ArtMethod::DexCacheResolvedMethodsOffset().Int32Value(),
                      cg->TargetReg(kArg0));
      // Set up direct code if known.
      if (direct_code != 0) {
        if (direct_code != static_cast<uintptr_t>(-1)) {
          cg->LoadConstant(cg->TargetReg(kInvokeTgt), direct_code);
        } else if (cu->instruction_set != kX86 && cu->instruction_set != kX86_64) {
          CHECK_LT(target_method.dex_method_index, target_method.dex_file->NumMethodIds());
          cg->LoadCodeAddress(target_method, type, kInvokeTgt);
        }
      }
      break;
    case 2:  // Grab target method*
      CHECK_EQ(cu->dex_file, target_method.dex_file);
      cg->LoadRefDisp(cg->TargetReg(kArg0),
                      ObjArray::OffsetOfElement(target_method.dex_method_index).Int32Value(),
                      cg->TargetReg(kArg0));
      break;
    case 3:  // Grab the code from the method*
      if (cu->instruction_set != kX86 && cu->instruction_set != kX86_64) {
        if (direct_code == 0) {
          cg->LoadWordDisp(cg->TargetReg(kArg0),
                           mirror::ArtMethod::EntryPointFromQuickCompiledCodeOffset().Int32Value(),
                           cg->TargetReg(kInvokeTgt));
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
    case 0: {  // Get "this" [set kArg1]
      RegLocation  rl_arg = info->args[0];
      cg->LoadValueDirectFixed(rl_arg, cg->TargetReg(kArg1));
      break;
    }
    case 1:  // Is "this" null? [use kArg1]
      cg->GenNullCheck(cg->TargetReg(kArg1), info->opt_flags);
      // get this->klass_ [use kArg1, set kInvokeTgt]
      cg->LoadRefDisp(cg->TargetReg(kArg1), mirror::Object::ClassOffset().Int32Value(),
                      cg->TargetReg(kInvokeTgt));
      cg->MarkPossibleNullPointerException(info->opt_flags);
      break;
    case 2:  // Get this->klass_->vtable [usr kInvokeTgt, set kInvokeTgt]
      cg->LoadRefDisp(cg->TargetReg(kInvokeTgt), mirror::Class::VTableOffset().Int32Value(),
                      cg->TargetReg(kInvokeTgt));
      break;
    case 3:  // Get target method [use kInvokeTgt, set kArg0]
      cg->LoadRefDisp(cg->TargetReg(kInvokeTgt),
                      ObjArray::OffsetOfElement(method_idx).Int32Value(),
                      cg->TargetReg(kArg0));
      break;
    case 4:  // Get the compiled code address [uses kArg0, sets kInvokeTgt]
      if (cu->instruction_set != kX86 && cu->instruction_set != kX86_64) {
        cg->LoadWordDisp(cg->TargetReg(kArg0),
                         mirror::ArtMethod::EntryPointFromQuickCompiledCodeOffset().Int32Value(),
                         cg->TargetReg(kInvokeTgt));
        break;
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
      cg->LoadConstant(cg->TargetReg(kHiddenArg), target_method.dex_method_index);
      if (cu->instruction_set == kX86 || cu->instruction_set == kX86_64) {
        cg->OpRegCopy(cg->TargetReg(kHiddenFpArg), cg->TargetReg(kHiddenArg));
      }
      break;
    case 1: {  // Get "this" [set kArg1]
      RegLocation  rl_arg = info->args[0];
      cg->LoadValueDirectFixed(rl_arg, cg->TargetReg(kArg1));
      break;
    }
    case 2:  // Is "this" null? [use kArg1]
      cg->GenNullCheck(cg->TargetReg(kArg1), info->opt_flags);
      // Get this->klass_ [use kArg1, set kInvokeTgt]
      cg->LoadRefDisp(cg->TargetReg(kArg1), mirror::Object::ClassOffset().Int32Value(),
                      cg->TargetReg(kInvokeTgt));
      cg->MarkPossibleNullPointerException(info->opt_flags);
      break;
    case 3:  // Get this->klass_->imtable [use kInvokeTgt, set kInvokeTgt]
      // NOTE: native pointer.
      cg->LoadRefDisp(cg->TargetReg(kInvokeTgt), mirror::Class::ImTableOffset().Int32Value(),
                      cg->TargetReg(kInvokeTgt));
      break;
    case 4:  // Get target method [use kInvokeTgt, set kArg0]
      // NOTE: native pointer.
      cg->LoadRefDisp(cg->TargetReg(kInvokeTgt),
                       ObjArray::OffsetOfElement(method_idx % ClassLinker::kImtSize).Int32Value(),
                       cg->TargetReg(kArg0));
      break;
    case 5:  // Get the compiled code address [use kArg0, set kInvokeTgt]
      if (cu->instruction_set != kX86 && cu->instruction_set != kX86_64) {
        cg->LoadWordDisp(cg->TargetReg(kArg0),
                         mirror::ArtMethod::EntryPointFromQuickCompiledCodeOffset().Int32Value(),
                         cg->TargetReg(kInvokeTgt));
        break;
      }
      // Intentional fallthrough for X86
    default:
      return -1;
  }
  return state + 1;
}

template <size_t pointer_size>
static int NextInvokeInsnSP(CompilationUnit* cu, CallInfo* info, ThreadOffset<pointer_size> trampoline,
                            int state, const MethodReference& target_method,
                            uint32_t method_idx) {
  Mir2Lir* cg = static_cast<Mir2Lir*>(cu->cg.get());
  /*
   * This handles the case in which the base method is not fully
   * resolved at compile time, we bail to a runtime helper.
   */
  if (state == 0) {
    if (cu->instruction_set != kX86 && cu->instruction_set != kX86_64) {
      // Load trampoline target
      cg->LoadWordDisp(cg->TargetReg(kSelf), trampoline.Int32Value(), cg->TargetReg(kInvokeTgt));
    }
    // Load kArg0 with method index
    CHECK_EQ(cu->dex_file, target_method.dex_file);
    cg->LoadConstant(cg->TargetReg(kArg0), target_method.dex_method_index);
    return 1;
  }
  return -1;
}

static int NextStaticCallInsnSP(CompilationUnit* cu, CallInfo* info,
                                int state,
                                const MethodReference& target_method,
                                uint32_t unused, uintptr_t unused2,
                                uintptr_t unused3, InvokeType unused4) {
  if (Is64BitInstructionSet(cu->instruction_set)) {
    ThreadOffset<8> trampoline = QUICK_ENTRYPOINT_OFFSET(8, pInvokeStaticTrampolineWithAccessCheck);
    return NextInvokeInsnSP<8>(cu, info, trampoline, state, target_method, 0);
  } else {
    ThreadOffset<4> trampoline = QUICK_ENTRYPOINT_OFFSET(4, pInvokeStaticTrampolineWithAccessCheck);
    return NextInvokeInsnSP<4>(cu, info, trampoline, state, target_method, 0);
  }
}

static int NextDirectCallInsnSP(CompilationUnit* cu, CallInfo* info, int state,
                                const MethodReference& target_method,
                                uint32_t unused, uintptr_t unused2,
                                uintptr_t unused3, InvokeType unused4) {
  if (Is64BitInstructionSet(cu->instruction_set)) {
    ThreadOffset<8> trampoline = QUICK_ENTRYPOINT_OFFSET(8, pInvokeDirectTrampolineWithAccessCheck);
    return NextInvokeInsnSP<8>(cu, info, trampoline, state, target_method, 0);
  } else {
    ThreadOffset<4> trampoline = QUICK_ENTRYPOINT_OFFSET(4, pInvokeDirectTrampolineWithAccessCheck);
    return NextInvokeInsnSP<4>(cu, info, trampoline, state, target_method, 0);
  }
}

static int NextSuperCallInsnSP(CompilationUnit* cu, CallInfo* info, int state,
                               const MethodReference& target_method,
                               uint32_t unused, uintptr_t unused2,
                               uintptr_t unused3, InvokeType unused4) {
  if (Is64BitInstructionSet(cu->instruction_set)) {
    ThreadOffset<8> trampoline = QUICK_ENTRYPOINT_OFFSET(8, pInvokeSuperTrampolineWithAccessCheck);
    return NextInvokeInsnSP<8>(cu, info, trampoline, state, target_method, 0);
  } else {
    ThreadOffset<4> trampoline = QUICK_ENTRYPOINT_OFFSET(4, pInvokeSuperTrampolineWithAccessCheck);
    return NextInvokeInsnSP<4>(cu, info, trampoline, state, target_method, 0);
  }
}

static int NextVCallInsnSP(CompilationUnit* cu, CallInfo* info, int state,
                           const MethodReference& target_method,
                           uint32_t unused, uintptr_t unused2,
                           uintptr_t unused3, InvokeType unused4) {
  if (Is64BitInstructionSet(cu->instruction_set)) {
    ThreadOffset<8> trampoline = QUICK_ENTRYPOINT_OFFSET(8, pInvokeVirtualTrampolineWithAccessCheck);
    return NextInvokeInsnSP<8>(cu, info, trampoline, state, target_method, 0);
  } else {
    ThreadOffset<4> trampoline = QUICK_ENTRYPOINT_OFFSET(4, pInvokeVirtualTrampolineWithAccessCheck);
    return NextInvokeInsnSP<4>(cu, info, trampoline, state, target_method, 0);
  }
}

static int NextInterfaceCallInsnWithAccessCheck(CompilationUnit* cu,
                                                CallInfo* info, int state,
                                                const MethodReference& target_method,
                                                uint32_t unused, uintptr_t unused2,
                                                uintptr_t unused3, InvokeType unused4) {
  if (Is64BitInstructionSet(cu->instruction_set)) {
      ThreadOffset<8> trampoline = QUICK_ENTRYPOINT_OFFSET(8, pInvokeInterfaceTrampolineWithAccessCheck);
      return NextInvokeInsnSP<8>(cu, info, trampoline, state, target_method, 0);
    } else {
      ThreadOffset<4> trampoline = QUICK_ENTRYPOINT_OFFSET(4, pInvokeInterfaceTrampolineWithAccessCheck);
      return NextInvokeInsnSP<4>(cu, info, trampoline, state, target_method, 0);
    }
}

int Mir2Lir::LoadArgRegs(CallInfo* info, int call_state,
                         NextCallInsn next_call_insn,
                         const MethodReference& target_method,
                         uint32_t vtable_idx, uintptr_t direct_code,
                         uintptr_t direct_method, InvokeType type, bool skip_this) {
  int last_arg_reg = 3 - 1;
  int arg_regs[3] = {TargetReg(kArg1).GetReg(), TargetReg(kArg2).GetReg(), TargetReg(kArg3).GetReg()};

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
        // NOTE: not correct for 64-bit core regs, but this needs rewriting for hard-float.
        reg = rl_arg.reg.IsPair() ? rl_arg.reg.GetHigh() : rl_arg.reg.DoubleToHighSingle();
      } else {
        // kArg2 & rArg3 can safely be used here
        reg = TargetReg(kArg3);
        Load32Disp(TargetReg(kSp), SRegOffset(rl_arg.s_reg_low) + 4, reg);
        call_state = next_call_insn(cu_, info, call_state, target_method,
                                    vtable_idx, direct_code, direct_method, type);
      }
      Store32Disp(TargetReg(kSp), (next_use + 1) * 4, reg);
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
        arg_reg = rl_arg.wide ? RegStorage::MakeRegPair(TargetReg(kArg2), TargetReg(kArg3)) :
            TargetReg(kArg2);
        if (rl_arg.wide) {
          LoadValueDirectWideFixed(rl_arg, arg_reg);
        } else {
          LoadValueDirectFixed(rl_arg, arg_reg);
        }
        call_state = next_call_insn(cu_, info, call_state, target_method,
                                    vtable_idx, direct_code, direct_method, type);
      }
      int outs_offset = (next_use + 1) * 4;
      if (rl_arg.wide) {
        StoreBaseDisp(TargetReg(kSp), outs_offset, arg_reg, k64);
        next_use += 2;
      } else {
        Store32Disp(TargetReg(kSp), outs_offset, arg_reg);
        next_use++;
      }
      call_state = next_call_insn(cu_, info, call_state, target_method, vtable_idx,
                               direct_code, direct_method, type);
    }
  }

  call_state = LoadArgRegs(info, call_state, next_call_insn,
                           target_method, vtable_idx, direct_code, direct_method,
                           type, skip_this);

  if (pcrLabel) {
    if (Runtime::Current()->ExplicitNullChecks()) {
      *pcrLabel = GenExplicitNullCheck(TargetReg(kArg1), info->opt_flags);
    } else {
      *pcrLabel = nullptr;
      // In lieu of generating a check for kArg1 being null, we need to
      // perform a load when doing implicit checks.
      RegStorage tmp = AllocTemp();
      Load32Disp(TargetReg(kArg1), 0, tmp);
      MarkPossibleNullPointerException(info->opt_flags);
      FreeTemp(tmp);
    }
  }
  return call_state;
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
        StoreBaseDisp(TargetReg(kSp), SRegOffset(loc.s_reg_low), loc.reg, k64);
      }
      next_arg += 2;
    } else {
      loc = UpdateLoc(loc);
      if ((next_arg >= 3) && (loc.location == kLocPhysReg)) {
        Store32Disp(TargetReg(kSp), SRegOffset(loc.s_reg_low), loc.reg);
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
    OpRegRegImm(kOpAdd, TargetReg(kArg3), TargetReg(kSp), start_offset);
    LIR* ld = OpVldm(TargetReg(kArg3), regs_left_to_pass_via_stack);
    // TUNING: loosen barrier
    ld->u.m.def_mask = ENCODE_ALL;
    SetMemRefType(ld, true /* is_load */, kDalvikReg);
    call_state = next_call_insn(cu_, info, call_state, target_method, vtable_idx,
                             direct_code, direct_method, type);
    OpRegRegImm(kOpAdd, TargetReg(kArg3), TargetReg(kSp), 4 /* Method* */ + (3 * 4));
    call_state = next_call_insn(cu_, info, call_state, target_method, vtable_idx,
                             direct_code, direct_method, type);
    LIR* st = OpVstm(TargetReg(kArg3), regs_left_to_pass_via_stack);
    SetMemRefType(st, false /* is_load */, kDalvikReg);
    st->u.m.def_mask = ENCODE_ALL;
    call_state = next_call_insn(cu_, info, call_state, target_method, vtable_idx,
                             direct_code, direct_method, type);
  } else if (cu_->instruction_set == kX86 || cu_->instruction_set == kX86_64) {
    int current_src_offset = start_offset;
    int current_dest_offset = outs_offset;

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
        // we expect to have an xmm temporary available.
        RegStorage temp = AllocTempDouble();
        DCHECK(temp.Valid());

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
          ld1 = OpMovRegMem(temp, TargetReg(kSp), current_src_offset, kMovA128FP);
        } else if (src_is_8b_aligned) {
          ld1 = OpMovRegMem(temp, TargetReg(kSp), current_src_offset, kMovLo128FP);
          ld2 = OpMovRegMem(temp, TargetReg(kSp), current_src_offset + (bytes_to_move >> 1),
                            kMovHi128FP);
        } else {
          ld1 = OpMovRegMem(temp, TargetReg(kSp), current_src_offset, kMovU128FP);
        }

        if (dest_is_16b_aligned) {
          st1 = OpMovMemReg(TargetReg(kSp), current_dest_offset, temp, kMovA128FP);
        } else if (dest_is_8b_aligned) {
          st1 = OpMovMemReg(TargetReg(kSp), current_dest_offset, temp, kMovLo128FP);
          st2 = OpMovMemReg(TargetReg(kSp), current_dest_offset + (bytes_to_move >> 1),
                            temp, kMovHi128FP);
        } else {
          st1 = OpMovMemReg(TargetReg(kSp), current_dest_offset, temp, kMovU128FP);
        }

        // TODO If we could keep track of aliasing information for memory accesses that are wider
        // than 64-bit, we wouldn't need to set up a barrier.
        if (ld1 != nullptr) {
          if (ld2 != nullptr) {
            // For 64-bit load we can actually set up the aliasing information.
            AnnotateDalvikRegAccess(ld1, current_src_offset >> 2, true, true);
            AnnotateDalvikRegAccess(ld2, (current_src_offset + (bytes_to_move >> 1)) >> 2, true, true);
          } else {
            // Set barrier for 128-bit load.
            SetMemRefType(ld1, true /* is_load */, kDalvikReg);
            ld1->u.m.def_mask = ENCODE_ALL;
          }
        }
        if (st1 != nullptr) {
          if (st2 != nullptr) {
            // For 64-bit store we can actually set up the aliasing information.
            AnnotateDalvikRegAccess(st1, current_dest_offset >> 2, false, true);
            AnnotateDalvikRegAccess(st2, (current_dest_offset + (bytes_to_move >> 1)) >> 2, false, true);
          } else {
            // Set barrier for 128-bit store.
            SetMemRefType(st1, false /* is_load */, kDalvikReg);
            st1->u.m.def_mask = ENCODE_ALL;
          }
        }

        // Free the temporary used for the data movement.
        FreeTemp(temp);
      } else {
        // Moving 32-bits via general purpose register.
        bytes_to_move = sizeof(uint32_t);

        // Instead of allocating a new temp, simply reuse one of the registers being used
        // for argument passing.
        RegStorage temp = TargetReg(kArg3);

        // Now load the argument VR and store to the outs.
        Load32Disp(TargetReg(kSp), current_src_offset, temp);
        Store32Disp(TargetReg(kSp), current_dest_offset, temp);
      }

      current_src_offset += bytes_to_move;
      current_dest_offset += bytes_to_move;
      regs_left_to_pass_via_stack -= (bytes_to_move >> 2);
    }
  } else {
    // Generate memcpy
    OpRegRegImm(kOpAdd, TargetReg(kArg0), TargetReg(kSp), outs_offset);
    OpRegRegImm(kOpAdd, TargetReg(kArg1), TargetReg(kSp), start_offset);
    if (Is64BitInstructionSet(cu_->instruction_set)) {
      CallRuntimeHelperRegRegImm(QUICK_ENTRYPOINT_OFFSET(8, pMemcpy), TargetReg(kArg0),
                                 TargetReg(kArg1), (info->num_arg_words - 3) * 4, false);
    } else {
      CallRuntimeHelperRegRegImm(QUICK_ENTRYPOINT_OFFSET(4, pMemcpy), TargetReg(kArg0),
                                 TargetReg(kArg1), (info->num_arg_words - 3) * 4, false);
    }
  }

  call_state = LoadArgRegs(info, call_state, next_call_insn,
                           target_method, vtable_idx, direct_code, direct_method,
                           type, skip_this);

  call_state = next_call_insn(cu_, info, call_state, target_method, vtable_idx,
                           direct_code, direct_method, type);
  if (pcrLabel) {
    if (Runtime::Current()->ExplicitNullChecks()) {
      *pcrLabel = GenExplicitNullCheck(TargetReg(kArg1), info->opt_flags);
    } else {
      *pcrLabel = nullptr;
      // In lieu of generating a check for kArg1 being null, we need to
      // perform a load when doing implicit checks.
      RegStorage tmp = AllocTemp();
      Load32Disp(TargetReg(kArg1), 0, tmp);
      MarkPossibleNullPointerException(info->opt_flags);
      FreeTemp(tmp);
    }
  }
  return call_state;
}

RegLocation Mir2Lir::InlineTarget(CallInfo* info) {
  RegLocation res;
  if (info->result.location == kLocInvalid) {
    res = GetReturn(false);
  } else {
    res = info->result;
  }
  return res;
}

RegLocation Mir2Lir::InlineTargetWide(CallInfo* info) {
  RegLocation res;
  if (info->result.location == kLocInvalid) {
    res = GetReturnWide(false);
  } else {
    res = info->result;
  }
  return res;
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
  rl_obj = LoadValue(rl_obj, kCoreReg);
  // X86 wants to avoid putting a constant index into a register.
  if (!((cu_->instruction_set == kX86 || cu_->instruction_set == kX86_64)&& rl_idx.is_const)) {
    rl_idx = LoadValue(rl_idx, kCoreReg);
  }
  RegStorage reg_max;
  GenNullCheck(rl_obj.reg, info->opt_flags);
  bool range_check = (!(info->opt_flags & MIR_IGNORE_RANGE_CHECK));
  LIR* range_check_branch = nullptr;
  RegStorage reg_off;
  RegStorage reg_ptr;
  if (cu_->instruction_set != kX86 && cu_->instruction_set != kX86_64) {
    reg_off = AllocTemp();
    reg_ptr = AllocTemp();
    if (range_check) {
      reg_max = AllocTemp();
      Load32Disp(rl_obj.reg, count_offset, reg_max);
      MarkPossibleNullPointerException(info->opt_flags);
    }
    Load32Disp(rl_obj.reg, offset_offset, reg_off);
    MarkPossibleNullPointerException(info->opt_flags);
    Load32Disp(rl_obj.reg, value_offset, reg_ptr);
    if (range_check) {
      // Set up a slow path to allow retry in case of bounds violation */
      OpRegReg(kOpCmp, rl_idx.reg, reg_max);
      FreeTemp(reg_max);
      range_check_branch = OpCondBranch(kCondUge, nullptr);
    }
    OpRegImm(kOpAdd, reg_ptr, data_offset);
  } else {
    if (range_check) {
      // On x86, we can compare to memory directly
      // Set up a launch pad to allow retry in case of bounds violation */
      if (rl_idx.is_const) {
        range_check_branch = OpCmpMemImmBranch(
            kCondUlt, RegStorage::InvalidReg(), rl_obj.reg, count_offset,
            mir_graph_->ConstantValue(rl_idx.orig_sreg), nullptr);
      } else {
        OpRegMem(kOpCmp, rl_idx.reg, rl_obj.reg, count_offset);
        range_check_branch = OpCondBranch(kCondUge, nullptr);
      }
    }
    reg_off = AllocTemp();
    reg_ptr = AllocTemp();
    Load32Disp(rl_obj.reg, offset_offset, reg_off);
    Load32Disp(rl_obj.reg, value_offset, reg_ptr);
  }
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
  if (cu_->instruction_set != kX86 && cu_->instruction_set != kX86_64) {
    LoadBaseIndexed(reg_ptr, reg_off, rl_result.reg, 1, kUnsignedHalf);
  } else {
    LoadBaseIndexedDisp(reg_ptr, reg_off, 1, data_offset, rl_result.reg, kUnsignedHalf);
  }
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
  rl_obj = LoadValue(rl_obj, kCoreReg);
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
    // TODO - add Mips implementation
    return false;
  }
  RegLocation rl_src_i = info->args[0];
  RegLocation rl_dest = (size == k64) ? InlineTargetWide(info) : InlineTarget(info);  // result reg
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  if (size == k64) {
    RegLocation rl_i = LoadValueWide(rl_src_i, kCoreReg);
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
    RegLocation rl_i = LoadValue(rl_src_i, kCoreReg);
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
  if (cu_->instruction_set == kX86 || cu_->instruction_set == kX86_64 || rl_result.reg.GetLowReg() == rl_src.reg.GetHighReg()) {
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
  RegStorage sign_reg = AllocTemp();
  OpRegRegImm(kOpAsr, sign_reg, rl_src.reg.GetHigh(), 31);
  OpRegRegReg(kOpAdd, rl_result.reg.GetLow(), rl_src.reg.GetLow(), sign_reg);
  OpRegRegReg(kOpAdc, rl_result.reg.GetHigh(), rl_src.reg.GetHigh(), sign_reg);
  OpRegReg(kOpXor, rl_result.reg.GetLow(), sign_reg);
  OpRegReg(kOpXor, rl_result.reg.GetHigh(), sign_reg);
  StoreValueWide(rl_dest, rl_result);
  return true;
}

bool Mir2Lir::GenInlinedAbsFloat(CallInfo* info) {
  if (cu_->instruction_set == kMips) {
    // TODO - add Mips implementation
    return false;
  }
  RegLocation rl_src = info->args[0];
  rl_src = LoadValue(rl_src, kCoreReg);
  RegLocation rl_dest = InlineTarget(info);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  OpRegRegImm(kOpAnd, rl_result.reg, rl_src.reg, 0x7fffffff);
  StoreValue(rl_dest, rl_result);
  return true;
}

bool Mir2Lir::GenInlinedAbsDouble(CallInfo* info) {
  if (cu_->instruction_set == kMips) {
    // TODO - add Mips implementation
    return false;
  }
  RegLocation rl_src = info->args[0];
  rl_src = LoadValueWide(rl_src, kCoreReg);
  RegLocation rl_dest = InlineTargetWide(info);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  OpRegCopyWide(rl_result.reg, rl_src.reg);
  OpRegImm(kOpAnd, rl_result.reg.GetHigh(), 0x7fffffff);
  StoreValueWide(rl_dest, rl_result);
  return true;
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

/*
 * Fast String.indexOf(I) & (II).  Tests for simple case of char <= 0xFFFF,
 * otherwise bails to standard library code.
 */
bool Mir2Lir::GenInlinedIndexOf(CallInfo* info, bool zero_based) {
  if (cu_->instruction_set == kMips) {
    // TODO - add Mips implementation
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
  RegStorage reg_ptr = TargetReg(kArg0);
  RegStorage reg_char = TargetReg(kArg1);
  RegStorage reg_start = TargetReg(kArg2);

  LoadValueDirectFixed(rl_obj, reg_ptr);
  LoadValueDirectFixed(rl_char, reg_char);
  if (zero_based) {
    LoadConstant(reg_start, 0);
  } else {
    RegLocation rl_start = info->args[2];     // 3rd arg only present in III flavor of IndexOf.
    LoadValueDirectFixed(rl_start, reg_start);
  }
  RegStorage r_tgt = Is64BitInstructionSet(cu_->instruction_set) ?
      LoadHelper(QUICK_ENTRYPOINT_OFFSET(8, pIndexOf)) :
      LoadHelper(QUICK_ENTRYPOINT_OFFSET(4, pIndexOf));
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
  RegLocation rl_return = GetReturn(false);
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
  RegStorage reg_this = TargetReg(kArg0);
  RegStorage reg_cmp = TargetReg(kArg1);

  RegLocation rl_this = info->args[0];
  RegLocation rl_cmp = info->args[1];
  LoadValueDirectFixed(rl_this, reg_this);
  LoadValueDirectFixed(rl_cmp, reg_cmp);
  RegStorage r_tgt;
  if (cu_->instruction_set != kX86 && cu_->instruction_set != kX86_64) {
    if (Is64BitInstructionSet(cu_->instruction_set)) {
      r_tgt = LoadHelper(QUICK_ENTRYPOINT_OFFSET(8, pStringCompareTo));
    } else {
      r_tgt = LoadHelper(QUICK_ENTRYPOINT_OFFSET(4, pStringCompareTo));
    }
  } else {
    r_tgt = RegStorage::InvalidReg();
  }
  GenExplicitNullCheck(reg_this, info->opt_flags);
  info->opt_flags |= MIR_IGNORE_NULL_CHECK;  // Record that we've null checked.
  // TUNING: check if rl_cmp.s_reg_low is already null checked
  LIR* cmp_null_check_branch = OpCmpImmBranch(kCondEq, reg_cmp, 0, nullptr);
  AddIntrinsicSlowPath(info, cmp_null_check_branch);
  // NOTE: not a safepoint
  if (cu_->instruction_set != kX86 && cu_->instruction_set != kX86_64) {
    OpReg(kOpBlx, r_tgt);
  } else {
    if (Is64BitInstructionSet(cu_->instruction_set)) {
      OpThreadMem(kOpBlx, QUICK_ENTRYPOINT_OFFSET(8, pStringCompareTo));
    } else {
      OpThreadMem(kOpBlx, QUICK_ENTRYPOINT_OFFSET(4, pStringCompareTo));
    }
  }
  RegLocation rl_return = GetReturn(false);
  RegLocation rl_dest = InlineTarget(info);
  StoreValue(rl_dest, rl_return);
  return true;
}

bool Mir2Lir::GenInlinedCurrentThread(CallInfo* info) {
  RegLocation rl_dest = InlineTarget(info);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);

  switch (cu_->instruction_set) {
    case kArm:
      // Fall-through.
    case kThumb2:
      // Fall-through.
    case kMips:
      Load32Disp(TargetReg(kSelf), Thread::PeerOffset<4>().Int32Value(), rl_result.reg);
      break;

    case kArm64:
      Load32Disp(TargetReg(kSelf), Thread::PeerOffset<8>().Int32Value(), rl_result.reg);
      break;

    case kX86:
      reinterpret_cast<X86Mir2Lir*>(this)->OpRegThreadMem(kOpMov, rl_result.reg,
                                                          Thread::PeerOffset<4>());
      break;

    case kX86_64:
      reinterpret_cast<X86Mir2Lir*>(this)->OpRegThreadMem(kOpMov, rl_result.reg,
                                                          Thread::PeerOffset<8>());
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

  RegLocation rl_object = LoadValue(rl_src_obj, kCoreReg);
  RegLocation rl_offset = LoadValue(rl_src_offset, kCoreReg);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  if (is_long) {
    if (cu_->instruction_set == kX86 || cu_->instruction_set == kX86_64) {
      LoadBaseIndexedDisp(rl_object.reg, rl_offset.reg, 0, 0, rl_result.reg, k64);
    } else {
      RegStorage rl_temp_offset = AllocTemp();
      OpRegRegReg(kOpAdd, rl_temp_offset, rl_object.reg, rl_offset.reg);
      LoadBaseDisp(rl_temp_offset, 0, rl_result.reg, k64);
      FreeTemp(rl_temp_offset);
    }
  } else {
    LoadBaseIndexed(rl_object.reg, rl_offset.reg, rl_result.reg, 0, k32);
  }

  if (is_volatile) {
    // Without context sensitive analysis, we must issue the most conservative barriers.
    // In this case, either a load or store may follow so we issue both barriers.
    GenMemBarrier(kLoadLoad);
    GenMemBarrier(kLoadStore);
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
    // There might have been a store before this volatile one so insert StoreStore barrier.
    GenMemBarrier(kStoreStore);
  }
  RegLocation rl_object = LoadValue(rl_src_obj, kCoreReg);
  RegLocation rl_offset = LoadValue(rl_src_offset, kCoreReg);
  RegLocation rl_value;
  if (is_long) {
    rl_value = LoadValueWide(rl_src_value, kCoreReg);
    if (cu_->instruction_set == kX86 || cu_->instruction_set == kX86_64) {
      StoreBaseIndexedDisp(rl_object.reg, rl_offset.reg, 0, 0, rl_value.reg, k64);
    } else {
      RegStorage rl_temp_offset = AllocTemp();
      OpRegRegReg(kOpAdd, rl_temp_offset, rl_object.reg, rl_offset.reg);
      StoreBaseDisp(rl_temp_offset, 0, rl_value.reg, k64);
      FreeTemp(rl_temp_offset);
    }
  } else {
    rl_value = LoadValue(rl_src_value, kCoreReg);
    StoreBaseIndexed(rl_object.reg, rl_offset.reg, rl_value.reg, 0, k32);
  }

  // Free up the temp early, to ensure x86 doesn't run out of temporaries in MarkGCCard.
  FreeTemp(rl_offset.reg);

  if (is_volatile) {
    // A load might follow the volatile store so insert a StoreLoad barrier.
    GenMemBarrier(kStoreLoad);
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
      RegLocation rl_obj = LoadValue(info->args[0], kCoreReg);
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

template <size_t pointer_size>
static LIR* GenInvokeNoInlineCall(Mir2Lir* mir_to_lir, InvokeType type) {
  ThreadOffset<pointer_size> trampoline(-1);
  switch (type) {
    case kInterface:
      trampoline = QUICK_ENTRYPOINT_OFFSET(pointer_size, pInvokeInterfaceTrampolineWithAccessCheck);
      break;
    case kDirect:
      trampoline = QUICK_ENTRYPOINT_OFFSET(pointer_size, pInvokeDirectTrampolineWithAccessCheck);
      break;
    case kStatic:
      trampoline = QUICK_ENTRYPOINT_OFFSET(pointer_size, pInvokeStaticTrampolineWithAccessCheck);
      break;
    case kSuper:
      trampoline = QUICK_ENTRYPOINT_OFFSET(pointer_size, pInvokeSuperTrampolineWithAccessCheck);
      break;
    case kVirtual:
      trampoline = QUICK_ENTRYPOINT_OFFSET(pointer_size, pInvokeVirtualTrampolineWithAccessCheck);
      break;
    default:
      LOG(FATAL) << "Unexpected invoke type";
  }
  return mir_to_lir->OpThreadMem(kOpBlx, trampoline);
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
    call_inst = OpReg(kOpBlx, TargetReg(kInvokeTgt));
  } else {
    if (fast_path) {
      if (method_info.DirectCode() == static_cast<uintptr_t>(-1)) {
        // We can have the linker fixup a call relative.
        call_inst =
          reinterpret_cast<X86Mir2Lir*>(this)->CallWithLinkerFixup(target_method, info->type);
      } else {
        call_inst = OpMem(kOpBlx, TargetReg(kArg0),
                          mirror::ArtMethod::EntryPointFromQuickCompiledCodeOffset().Int32Value());
      }
    } else {
      // TODO: Extract?
      if (Is64BitInstructionSet(cu_->instruction_set)) {
        call_inst = GenInvokeNoInlineCall<8>(this, info->type);
      } else {
        call_inst = GenInvokeNoInlineCall<4>(this, info->type);
      }
    }
  }
  MarkSafepointPC(call_inst);

  ClobberCallerSave();
  if (info->result.location != kLocInvalid) {
    // We have a following MOVE_RESULT - do it now.
    if (info->result.wide) {
      RegLocation ret_loc = GetReturnWide(info->result.fp);
      StoreValueWide(info->result, ret_loc);
    } else {
      RegLocation ret_loc = GetReturn(info->result.fp);
      StoreValue(info->result, ret_loc);
    }
  }
}

}  // namespace art
