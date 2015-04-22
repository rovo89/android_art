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

#include "mir_to_lir-inl.h"

#include "arm/codegen_arm.h"
#include "dex/compiler_ir.h"
#include "dex/dex_flags.h"
#include "dex/mir_graph.h"
#include "dex/quick/dex_file_method_inliner.h"
#include "dex/quick/dex_file_to_method_inliner_map.h"
#include "dex_file-inl.h"
#include "driver/compiler_driver.h"
#include "driver/compiler_options.h"
#include "entrypoints/quick/quick_entrypoints.h"
#include "invoke_type.h"
#include "mirror/array.h"
#include "mirror/class-inl.h"
#include "mirror/dex_cache.h"
#include "mirror/object_array-inl.h"
#include "mirror/string.h"
#include "scoped_thread_state_change.h"

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
    IntrinsicSlowPathPath(Mir2Lir* m2l, CallInfo* info_in, LIR* branch_in, LIR* resume_in)
        : LIRSlowPath(m2l, branch_in, resume_in), info_(info_in) {
      DCHECK_EQ(info_in->offset, current_dex_pc_);
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

void Mir2Lir::CallRuntimeHelperRegRegLocationMethod(QuickEntrypointEnum trampoline, RegStorage arg0,
                                                    RegLocation arg1, bool safepoint_pc) {
  RegStorage r_tgt = CallHelperSetup(trampoline);
  DCHECK(!IsSameReg(TargetReg(kArg2, arg0.GetWideKind()), arg0));
  RegStorage r_tmp = TargetReg(kArg0, arg0.GetWideKind());
  if (r_tmp.NotExactlyEquals(arg0)) {
    OpRegCopy(r_tmp, arg0);
  }
  LoadValueDirectFixed(arg1, TargetReg(kArg1, arg1));
  LoadCurrMethodDirect(TargetReg(kArg2, kRef));
  ClobberCallerSave();
  CallHelper(r_tgt, trampoline, safepoint_pc);
}

void Mir2Lir::CallRuntimeHelperRegLocationRegLocation(QuickEntrypointEnum trampoline,
                                                      RegLocation arg0, RegLocation arg1,
                                                      bool safepoint_pc) {
  RegStorage r_tgt = CallHelperSetup(trampoline);
  if (cu_->instruction_set == kArm64 || cu_->instruction_set == kMips64 ||
      cu_->instruction_set == kX86_64) {
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
        // For Mips, when the 1st arg is integral, then remaining arg are passed in core reg.
        if (cu_->instruction_set == kMips) {
          LoadValueDirectFixed(arg1, TargetReg((arg1.fp && arg0.fp) ? kFArg2 : kArg1, kNotWide));
        } else {
          LoadValueDirectFixed(arg1, TargetReg(arg1.fp ? kFArg1 : kArg1, kNotWide));
        }
      } else {
        // For Mips, when the 1st arg is integral, then remaining arg are passed in core reg.
        if (cu_->instruction_set == kMips) {
          LoadValueDirectWideFixed(arg1, TargetReg((arg1.fp && arg0.fp) ? kFArg2 : kArg2, kWide));
        } else {
          LoadValueDirectWideFixed(arg1, TargetReg(arg1.fp ? kFArg1 : kArg1, kWide));
        }
      }
    } else {
      LoadValueDirectWideFixed(arg0, TargetReg(arg0.fp ? kFArg0 : kArg0, kWide));
      if (arg1.wide == 0) {
        // For Mips, when the 1st arg is integral, then remaining arg are passed in core reg.
        if (cu_->instruction_set == kMips) {
          LoadValueDirectFixed(arg1, TargetReg((arg1.fp && arg0.fp) ? kFArg2 : kArg2, kNotWide));
        } else {
          LoadValueDirectFixed(arg1, TargetReg(arg1.fp ? kFArg2 : kArg2, kNotWide));
        }
      } else {
        // For Mips, when the 1st arg is integral, then remaining arg are passed in core reg.
        if (cu_->instruction_set == kMips) {
          LoadValueDirectWideFixed(arg1, TargetReg((arg1.fp && arg0.fp) ? kFArg2 : kArg2, kWide));
        } else {
          LoadValueDirectWideFixed(arg1, TargetReg(arg1.fp ? kFArg2 : kArg2, kWide));
        }
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

void Mir2Lir::CallRuntimeHelperImmRegLocationMethod(QuickEntrypointEnum trampoline, int arg0,
                                                    RegLocation arg1, bool safepoint_pc) {
  RegStorage r_tgt = CallHelperSetup(trampoline);
  LoadValueDirectFixed(arg1, TargetReg(kArg1, arg1));
  LoadCurrMethodDirect(TargetReg(kArg2, kRef));
  LoadConstant(TargetReg(kArg0, kNotWide), arg0);
  ClobberCallerSave();
  CallHelper(r_tgt, trampoline, safepoint_pc);
}

void Mir2Lir::CallRuntimeHelperImmImmMethod(QuickEntrypointEnum trampoline, int arg0, int arg1,
                                            bool safepoint_pc) {
  RegStorage r_tgt = CallHelperSetup(trampoline);
  LoadCurrMethodDirect(TargetReg(kArg2, kRef));
  LoadConstant(TargetReg(kArg1, kNotWide), arg1);
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

void Mir2Lir::CallRuntimeHelperRegLocationRegLocationRegLocationRegLocation(
    QuickEntrypointEnum trampoline, RegLocation arg0, RegLocation arg1, RegLocation arg2,
    RegLocation arg3, bool safepoint_pc) {
  RegStorage r_tgt = CallHelperSetup(trampoline);
  LoadValueDirectFixed(arg0, TargetReg(kArg0, arg0));
  LoadValueDirectFixed(arg1, TargetReg(kArg1, arg1));
  LoadValueDirectFixed(arg2, TargetReg(kArg2, arg2));
  LoadValueDirectFixed(arg3, TargetReg(kArg3, arg3));
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
// TODO: Support 64-bit argument registers.
void Mir2Lir::FlushIns(RegLocation* ArgLocs, RegLocation rl_method) {
  /*
   * Dummy up a RegLocation for the incoming ArtMethod*
   * It will attempt to keep kArg0 live (or copy it to home location
   * if promoted).
   */
  RegLocation rl_src = rl_method;
  rl_src.location = kLocPhysReg;
  rl_src.reg = TargetReg(kArg0, kRef);
  rl_src.home = false;
  MarkLive(rl_src);
  if (cu_->target64) {
    DCHECK(rl_method.wide);
    StoreValueWide(rl_method, rl_src);
  } else {
    StoreValue(rl_method, rl_src);
  }
  // If Method* has been promoted, explicitly flush
  if (rl_method.location == kLocPhysReg) {
    StoreBaseDisp(TargetPtrReg(kSp), 0, rl_src.reg, kWord, kNotVolatile);
  }

  if (mir_graph_->GetNumOfInVRs() == 0) {
    return;
  }

  int start_vreg = mir_graph_->GetFirstInVR();
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
  RegLocation* t_loc = nullptr;
  EnsureInitializedArgMappingToPhysicalReg();
  for (uint32_t i = 0; i < mir_graph_->GetNumOfInVRs(); i += t_loc->wide ? 2 : 1) {
    // get reg corresponding to input
    RegStorage reg = in_to_reg_storage_mapping_.GetReg(i);
    t_loc = &ArgLocs[i];

    // If the wide input appeared as single, flush it and go
    // as it comes from memory.
    if (t_loc->wide && reg.Valid() && !reg.Is64Bit()) {
      // The memory already holds the half. Don't do anything.
      reg = RegStorage::InvalidReg();
    }

    if (reg.Valid()) {
      // If arriving in register.

      // We have already updated the arg location with promoted info
      // so we can be based on it.
      if (t_loc->location == kLocPhysReg) {
        // Just copy it.
        if (t_loc->wide) {
          OpRegCopyWide(t_loc->reg, reg);
        } else {
          OpRegCopy(t_loc->reg, reg);
        }
      } else {
        // Needs flush.
        int offset = SRegOffset(start_vreg + i);
        if (t_loc->ref) {
          StoreRefDisp(TargetPtrReg(kSp), offset, reg, kNotVolatile);
        } else {
          StoreBaseDisp(TargetPtrReg(kSp), offset, reg, t_loc->wide ? k64 : k32, kNotVolatile);
        }
      }
    } else {
      // If arriving in frame & promoted.
      if (t_loc->location == kLocPhysReg) {
        int offset = SRegOffset(start_vreg + i);
        if (t_loc->ref) {
          LoadRefDisp(TargetPtrReg(kSp), offset, t_loc->reg, kNotVolatile);
        } else {
          LoadBaseDisp(TargetPtrReg(kSp), offset, t_loc->reg, t_loc->wide ? k64 : k32,
                       kNotVolatile);
        }
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

static bool CommonCallCodeLoadCodePointerIntoInvokeTgt(const RegStorage* alt_from,
                                                       const CompilationUnit* cu, Mir2Lir* cg) {
  if (cu->instruction_set != kX86 && cu->instruction_set != kX86_64) {
    int32_t offset = ArtMethod::EntryPointFromQuickCompiledCodeOffset(
        InstructionSetPointerSize(cu->instruction_set)).Int32Value();
    // Get the compiled code address [use *alt_from or kArg0, set kInvokeTgt]
    cg->LoadWordDisp(alt_from == nullptr ? cg->TargetReg(kArg0, kRef) : *alt_from, offset,
                     cg->TargetPtrReg(kInvokeTgt));
    return true;
  }
  return false;
}

/*
 * Bit of a hack here - in the absence of a real scheduling pass,
 * emit the next instruction in a virtual invoke sequence.
 * We can use kLr as a temp prior to target address loading
 * Note also that we'll load the first argument ("this") into
 * kArg1 here rather than the standard GenDalvikArgs.
 */
static int NextVCallInsn(CompilationUnit* cu, CallInfo* info,
                         int state, const MethodReference& target_method,
                         uint32_t method_idx, uintptr_t, uintptr_t,
                         InvokeType) {
  UNUSED(target_method);
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
      const size_t pointer_size = InstructionSetPointerSize(
          cu->compiler_driver->GetInstructionSet());
      int32_t offset = mirror::Class::EmbeddedVTableEntryOffset(
          method_idx, pointer_size).Uint32Value();
      // Load target method from embedded vtable to kArg0 [use kArg0, set kArg0]
      cg->LoadWordDisp(cg->TargetPtrReg(kArg0), offset, cg->TargetPtrReg(kArg0));
      break;
    }
    case 3:
      if (CommonCallCodeLoadCodePointerIntoInvokeTgt(nullptr, cu, cg)) {
        break;                                    // kInvokeTgt := kArg0->entrypoint
      }
      DCHECK(cu->instruction_set == kX86 || cu->instruction_set == kX86_64);
      FALLTHROUGH_INTENDED;
    default:
      return -1;
  }
  return state + 1;
}

/*
 * Emit the next instruction in an invoke interface sequence. This will do a lookup in the
 * class's IMT, calling either the actual method or art_quick_imt_conflict_trampoline if
 * more than one interface method map to the same index. Note also that we'll load the first
 * argument ("this") into kArg1 here rather than the standard GenDalvikArgs.
 */
static int NextInterfaceCallInsn(CompilationUnit* cu, CallInfo* info, int state,
                                 const MethodReference& target_method,
                                 uint32_t method_idx, uintptr_t, uintptr_t, InvokeType) {
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
      const size_t pointer_size = InstructionSetPointerSize(
          cu->compiler_driver->GetInstructionSet());
      int32_t offset = mirror::Class::EmbeddedImTableEntryOffset(
          method_idx % mirror::Class::kImtSize, pointer_size).Uint32Value();
      // Load target method from embedded imtable to kArg0 [use kArg0, set kArg0]
      cg->LoadWordDisp(cg->TargetPtrReg(kArg0), offset, cg->TargetPtrReg(kArg0));
      break;
    }
    case 4:
      if (CommonCallCodeLoadCodePointerIntoInvokeTgt(nullptr, cu, cg)) {
        break;                                    // kInvokeTgt := kArg0->entrypoint
      }
      DCHECK(cu->instruction_set == kX86 || cu->instruction_set == kX86_64);
      FALLTHROUGH_INTENDED;
    default:
      return -1;
  }
  return state + 1;
}

static int NextInvokeInsnSP(CompilationUnit* cu, CallInfo* info,
                            QuickEntrypointEnum trampoline, int state,
                            const MethodReference& target_method, uint32_t method_idx) {
  UNUSED(info, method_idx);
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
                                uint32_t, uintptr_t, uintptr_t, InvokeType) {
  return NextInvokeInsnSP(cu, info, kQuickInvokeStaticTrampolineWithAccessCheck, state,
                          target_method, 0);
}

static int NextDirectCallInsnSP(CompilationUnit* cu, CallInfo* info, int state,
                                const MethodReference& target_method,
                                uint32_t, uintptr_t, uintptr_t, InvokeType) {
  return NextInvokeInsnSP(cu, info, kQuickInvokeDirectTrampolineWithAccessCheck, state,
                          target_method, 0);
}

static int NextSuperCallInsnSP(CompilationUnit* cu, CallInfo* info, int state,
                               const MethodReference& target_method,
                               uint32_t, uintptr_t, uintptr_t, InvokeType) {
  return NextInvokeInsnSP(cu, info, kQuickInvokeSuperTrampolineWithAccessCheck, state,
                          target_method, 0);
}

static int NextVCallInsnSP(CompilationUnit* cu, CallInfo* info, int state,
                           const MethodReference& target_method,
                           uint32_t, uintptr_t, uintptr_t, InvokeType) {
  return NextInvokeInsnSP(cu, info, kQuickInvokeVirtualTrampolineWithAccessCheck, state,
                          target_method, 0);
}

static int NextInterfaceCallInsnWithAccessCheck(CompilationUnit* cu,
                                                CallInfo* info, int state,
                                                const MethodReference& target_method,
                                                uint32_t, uintptr_t, uintptr_t, InvokeType) {
  return NextInvokeInsnSP(cu, info, kQuickInvokeInterfaceTrampolineWithAccessCheck, state,
                          target_method, 0);
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

/**
 * @brief Used to flush promoted registers if they are used as argument
 * in an invocation.
 * @param info the infromation about arguments for invocation.
 * @param start the first argument we should start to look from.
 */
void Mir2Lir::GenDalvikArgsFlushPromoted(CallInfo* info, int start) {
  if (cu_->disable_opt & (1 << kPromoteRegs)) {
    // This make sense only if promotion is enabled.
    return;
  }
  ScopedMemRefType mem_ref_type(this, ResourceMask::kDalvikReg);
  // Scan the rest of the args - if in phys_reg flush to memory
  for (size_t next_arg = start; next_arg < info->num_arg_words;) {
    RegLocation loc = info->args[next_arg];
    if (loc.wide) {
      loc = UpdateLocWide(loc);
      if (loc.location == kLocPhysReg) {
        StoreBaseDisp(TargetPtrReg(kSp), SRegOffset(loc.s_reg_low), loc.reg, k64, kNotVolatile);
      }
      next_arg += 2;
    } else {
      loc = UpdateLoc(loc);
      if (loc.location == kLocPhysReg) {
        if (loc.ref) {
          StoreRefDisp(TargetPtrReg(kSp), SRegOffset(loc.s_reg_low), loc.reg, kNotVolatile);
        } else {
          StoreBaseDisp(TargetPtrReg(kSp), SRegOffset(loc.s_reg_low), loc.reg, k32,
                        kNotVolatile);
        }
      }
      next_arg++;
    }
  }
}

/**
 * @brief Used to optimize the copying of VRs which are arguments of invocation.
 * Please note that you should flush promoted registers first if you copy.
 * If implementation does copying it may skip several of the first VRs but must copy
 * till the end. Implementation must return the number of skipped VRs
 * (it might be all VRs).
 * @see GenDalvikArgsFlushPromoted
 * @param info the information about arguments for invocation.
 * @param first the first argument we should start to look from.
 * @param count the number of remaining arguments we can handle.
 * @return the number of arguments which we did not handle. Unhandled arguments
 * must be attached to the first one.
 */
int Mir2Lir::GenDalvikArgsBulkCopy(CallInfo* info, int first, int count) {
  // call is pretty expensive, let's use it if count is big.
  if (count > 16) {
    GenDalvikArgsFlushPromoted(info, first);
    int start_offset = SRegOffset(info->args[first].s_reg_low);
    int outs_offset = StackVisitor::GetOutVROffset(first, cu_->instruction_set);

    OpRegRegImm(kOpAdd, TargetReg(kArg0, kRef), TargetPtrReg(kSp), outs_offset);
    OpRegRegImm(kOpAdd, TargetReg(kArg1, kRef), TargetPtrReg(kSp), start_offset);
    CallRuntimeHelperRegRegImm(kQuickMemcpy, TargetReg(kArg0, kRef), TargetReg(kArg1, kRef),
                               count * 4, false);
    count = 0;
  }
  return count;
}

int Mir2Lir::GenDalvikArgs(CallInfo* info, int call_state,
                           LIR** pcrLabel, NextCallInsn next_call_insn,
                           const MethodReference& target_method,
                           uint32_t vtable_idx, uintptr_t direct_code, uintptr_t direct_method,
                           InvokeType type, bool skip_this) {
  // If no arguments, just return.
  if (info->num_arg_words == 0u)
    return call_state;

  const size_t start_index = skip_this ? 1 : 0;

  // Get architecture dependent mapping between output VRs and physical registers
  // basing on shorty of method to call.
  InToRegStorageMapping in_to_reg_storage_mapping(arena_);
  {
    const char* target_shorty = mir_graph_->GetShortyFromMethodReference(target_method);
    ShortyIterator shorty_iterator(target_shorty, type == kStatic);
    in_to_reg_storage_mapping.Initialize(&shorty_iterator, GetResetedInToRegStorageMapper());
  }

  size_t stack_map_start = std::max(in_to_reg_storage_mapping.GetEndMappedIn(), start_index);
  if ((stack_map_start < info->num_arg_words) && info->args[stack_map_start].high_word) {
    // It is possible that the last mapped reg is 32 bit while arg is 64-bit.
    // It will be handled together with low part mapped to register.
    stack_map_start++;
  }
  size_t regs_left_to_pass_via_stack = info->num_arg_words - stack_map_start;

  // If it is a range case we can try to copy remaining VRs (not mapped to physical registers)
  // using more optimal algorithm.
  if (info->is_range && regs_left_to_pass_via_stack > 1) {
    regs_left_to_pass_via_stack = GenDalvikArgsBulkCopy(info, stack_map_start,
                                                        regs_left_to_pass_via_stack);
  }

  // Now handle any remaining VRs mapped to stack.
  if (in_to_reg_storage_mapping.HasArgumentsOnStack()) {
    // Two temps but do not use kArg1, it might be this which we can skip.
    // Separate single and wide - it can give some advantage.
    RegStorage regRef = TargetReg(kArg3, kRef);
    RegStorage regSingle = TargetReg(kArg3, kNotWide);
    RegStorage regWide = TargetReg(kArg2, kWide);
    for (size_t i = start_index; i < stack_map_start + regs_left_to_pass_via_stack; i++) {
      RegLocation rl_arg = info->args[i];
      rl_arg = UpdateRawLoc(rl_arg);
      RegStorage reg = in_to_reg_storage_mapping.GetReg(i);
      if (!reg.Valid()) {
        int out_offset = StackVisitor::GetOutVROffset(i, cu_->instruction_set);
        {
          ScopedMemRefType mem_ref_type(this, ResourceMask::kDalvikReg);
          if (rl_arg.wide) {
            if (rl_arg.location == kLocPhysReg) {
              StoreBaseDisp(TargetPtrReg(kSp), out_offset, rl_arg.reg, k64, kNotVolatile);
            } else {
              LoadValueDirectWideFixed(rl_arg, regWide);
              StoreBaseDisp(TargetPtrReg(kSp), out_offset, regWide, k64, kNotVolatile);
            }
          } else {
            if (rl_arg.location == kLocPhysReg) {
              if (rl_arg.ref) {
                StoreRefDisp(TargetPtrReg(kSp), out_offset, rl_arg.reg, kNotVolatile);
              } else {
                StoreBaseDisp(TargetPtrReg(kSp), out_offset, rl_arg.reg, k32, kNotVolatile);
              }
            } else {
              if (rl_arg.ref) {
                LoadValueDirectFixed(rl_arg, regRef);
                StoreRefDisp(TargetPtrReg(kSp), out_offset, regRef, kNotVolatile);
              } else {
                LoadValueDirectFixed(rl_arg, regSingle);
                StoreBaseDisp(TargetPtrReg(kSp), out_offset, regSingle, k32, kNotVolatile);
              }
            }
          }
        }
        call_state = next_call_insn(cu_, info, call_state, target_method,
                                    vtable_idx, direct_code, direct_method, type);
      }
      if (rl_arg.wide) {
        i++;
      }
    }
  }

  // Finish with VRs mapped to physical registers.
  for (size_t i = start_index; i < stack_map_start; i++) {
    RegLocation rl_arg = info->args[i];
    rl_arg = UpdateRawLoc(rl_arg);
    RegStorage reg = in_to_reg_storage_mapping.GetReg(i);
    if (reg.Valid()) {
      if (rl_arg.wide) {
        // if reg is not 64-bit (it is half of 64-bit) then handle it separately.
        if (!reg.Is64Bit()) {
          ScopedMemRefType mem_ref_type(this, ResourceMask::kDalvikReg);
          if (rl_arg.location == kLocPhysReg) {
            int out_offset = StackVisitor::GetOutVROffset(i, cu_->instruction_set);
            // Dump it to memory.
            StoreBaseDisp(TargetPtrReg(kSp), out_offset, rl_arg.reg, k64, kNotVolatile);
            LoadBaseDisp(TargetPtrReg(kSp), out_offset, reg, k32, kNotVolatile);
          } else {
            int high_offset = StackVisitor::GetOutVROffset(i + 1, cu_->instruction_set);
            // First, use target reg for high part.
            LoadBaseDisp(TargetPtrReg(kSp), SRegOffset(rl_arg.s_reg_low + 1), reg, k32,
                         kNotVolatile);
            StoreBaseDisp(TargetPtrReg(kSp), high_offset, reg, k32, kNotVolatile);
            // Now, use target reg for low part.
            LoadBaseDisp(TargetPtrReg(kSp), SRegOffset(rl_arg.s_reg_low), reg, k32, kNotVolatile);
            int low_offset = StackVisitor::GetOutVROffset(i, cu_->instruction_set);
            // And store it to the expected memory location.
            StoreBaseDisp(TargetPtrReg(kSp), low_offset, reg, k32, kNotVolatile);
          }
        } else {
          LoadValueDirectWideFixed(rl_arg, reg);
        }
      } else {
        LoadValueDirectFixed(rl_arg, reg);
      }
      call_state = next_call_insn(cu_, info, call_state, target_method, vtable_idx,
                               direct_code, direct_method, type);
    }
    if (rl_arg.wide) {
      i++;
    }
  }

  call_state = next_call_insn(cu_, info, call_state, target_method, vtable_idx,
                           direct_code, direct_method, type);
  if (pcrLabel) {
    if (!cu_->compiler_driver->GetCompilerOptions().GetImplicitNullChecks()) {
      *pcrLabel = GenExplicitNullCheck(TargetReg(kArg1, kRef), info->opt_flags);
    } else {
      *pcrLabel = nullptr;
      GenImplicitNullCheck(TargetReg(kArg1, kRef), info->opt_flags);
    }
  }
  return call_state;
}

void Mir2Lir::EnsureInitializedArgMappingToPhysicalReg() {
  if (!in_to_reg_storage_mapping_.IsInitialized()) {
    ShortyIterator shorty_iterator(cu_->shorty, cu_->invoke_type == kStatic);
    in_to_reg_storage_mapping_.Initialize(&shorty_iterator, GetResetedInToRegStorageMapper());
  }
}

RegLocation Mir2Lir::InlineTarget(CallInfo* info) {
  RegLocation res;
  if (info->result.location == kLocInvalid) {
    // If result is unused, return a sink target based on type of invoke target.
    res = GetReturn(
        ShortyToRegClass(mir_graph_->GetShortyFromMethodReference(info->method_ref)[0]));
  } else {
    res = info->result;
  }
  return res;
}

RegLocation Mir2Lir::InlineTargetWide(CallInfo* info) {
  RegLocation res;
  if (info->result.location == kLocInvalid) {
    // If result is unused, return a sink target based on type of invoke target.
    res = GetReturnWide(ShortyToRegClass(
        mir_graph_->GetShortyFromMethodReference(info->method_ref)[0]));
  } else {
    res = info->result;
  }
  return res;
}

bool Mir2Lir::GenInlinedReferenceGetReferent(CallInfo* info) {
  if (cu_->instruction_set == kMips || cu_->instruction_set == kMips64) {
    // TODO: add Mips and Mips64 implementations.
    return false;
  }

  bool use_direct_type_ptr;
  uintptr_t direct_type_ptr;
  ClassReference ref;
  if (!cu_->compiler_driver->CanEmbedReferenceTypeInCode(&ref,
        &use_direct_type_ptr, &direct_type_ptr)) {
    return false;
  }

  RegStorage reg_class = TargetReg(kArg1, kRef);
  Clobber(reg_class);
  LockTemp(reg_class);
  if (use_direct_type_ptr) {
    LoadConstant(reg_class, direct_type_ptr);
  } else {
    uint16_t type_idx = ref.first->GetClassDef(ref.second).class_idx_;
    LoadClassType(*ref.first, type_idx, kArg1);
  }

  uint32_t slow_path_flag_offset = cu_->compiler_driver->GetReferenceSlowFlagOffset();
  uint32_t disable_flag_offset = cu_->compiler_driver->GetReferenceDisableFlagOffset();
  CHECK(slow_path_flag_offset && disable_flag_offset &&
        (slow_path_flag_offset != disable_flag_offset));

  // intrinsic logic start.
  RegLocation rl_obj = info->args[0];
  rl_obj = LoadValue(rl_obj, kRefReg);

  RegStorage reg_slow_path = AllocTemp();
  RegStorage reg_disabled = AllocTemp();
  LoadBaseDisp(reg_class, slow_path_flag_offset, reg_slow_path, kSignedByte, kNotVolatile);
  LoadBaseDisp(reg_class, disable_flag_offset, reg_disabled, kSignedByte, kNotVolatile);
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
  ClobberCallerSave();  // We must clobber everything because slow path will return here
  return true;
}

bool Mir2Lir::GenInlinedCharAt(CallInfo* info) {
  // Location of char array data
  int value_offset = mirror::String::ValueOffset().Int32Value();
  // Location of count
  int count_offset = mirror::String::CountOffset().Int32Value();

  RegLocation rl_obj = info->args[0];
  RegLocation rl_idx = info->args[1];
  rl_obj = LoadValue(rl_obj, kRefReg);
  rl_idx = LoadValue(rl_idx, kCoreReg);
  RegStorage reg_max;
  GenNullCheck(rl_obj.reg, info->opt_flags);
  bool range_check = (!(info->opt_flags & MIR_IGNORE_RANGE_CHECK));
  LIR* range_check_branch = nullptr;
  if (range_check) {
    reg_max = AllocTemp();
    Load32Disp(rl_obj.reg, count_offset, reg_max);
    MarkPossibleNullPointerException(info->opt_flags);
    // Set up a slow path to allow retry in case of bounds violation
    OpRegReg(kOpCmp, rl_idx.reg, reg_max);
    FreeTemp(reg_max);
    range_check_branch = OpCondBranch(kCondUge, nullptr);
  }
  RegStorage reg_ptr = AllocTempRef();
  OpRegRegImm(kOpAdd, reg_ptr, rl_obj.reg, value_offset);
  FreeTemp(rl_obj.reg);
  RegLocation rl_dest = InlineTarget(info);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  LoadBaseIndexed(reg_ptr, rl_idx.reg, rl_result.reg, 1, kUnsignedHalf);
  FreeTemp(reg_ptr);
  StoreValue(rl_dest, rl_result);
  if (range_check) {
    DCHECK(range_check_branch != nullptr);
    info->opt_flags |= MIR_IGNORE_NULL_CHECK;  // Record that we've already null checked.
    AddIntrinsicSlowPath(info, range_check_branch);
  }
  return true;
}

bool Mir2Lir::GenInlinedStringGetCharsNoCheck(CallInfo* info) {
  if (cu_->instruction_set == kMips) {
    // TODO - add Mips implementation
    return false;
  }
  size_t char_component_size = Primitive::ComponentSize(Primitive::kPrimChar);
  // Location of data in char array buffer
  int data_offset = mirror::Array::DataOffset(char_component_size).Int32Value();
  // Location of char array data in string
  int value_offset = mirror::String::ValueOffset().Int32Value();

  RegLocation rl_obj = info->args[0];
  RegLocation rl_start = info->args[1];
  RegLocation rl_end = info->args[2];
  RegLocation rl_buffer = info->args[3];
  RegLocation rl_index = info->args[4];

  ClobberCallerSave();
  LockCallTemps();  // Using fixed registers
  RegStorage reg_dst_ptr = TargetReg(kArg0, kRef);
  RegStorage reg_src_ptr = TargetReg(kArg1, kRef);
  RegStorage reg_length = TargetReg(kArg2, kNotWide);
  RegStorage reg_tmp = TargetReg(kArg3, kNotWide);
  RegStorage reg_tmp_ptr = RegStorage(RegStorage::k64BitSolo, reg_tmp.GetRawBits() & RegStorage::kRegTypeMask);

  LoadValueDirectFixed(rl_buffer, reg_dst_ptr);
  OpRegImm(kOpAdd, reg_dst_ptr, data_offset);
  LoadValueDirectFixed(rl_index, reg_tmp);
  OpRegRegImm(kOpLsl, reg_tmp, reg_tmp, 1);
  OpRegReg(kOpAdd, reg_dst_ptr, cu_->instruction_set == kArm64 ? reg_tmp_ptr : reg_tmp);

  LoadValueDirectFixed(rl_start, reg_tmp);
  LoadValueDirectFixed(rl_end, reg_length);
  OpRegReg(kOpSub, reg_length, reg_tmp);
  OpRegRegImm(kOpLsl, reg_length, reg_length, 1);
  LoadValueDirectFixed(rl_obj, reg_src_ptr);

  OpRegImm(kOpAdd, reg_src_ptr, value_offset);
  OpRegRegImm(kOpLsl, reg_tmp, reg_tmp, 1);
  OpRegReg(kOpAdd, reg_src_ptr, cu_->instruction_set == kArm64 ? reg_tmp_ptr : reg_tmp);

  RegStorage r_tgt;
  if (cu_->instruction_set != kX86 && cu_->instruction_set != kX86_64) {
    r_tgt = LoadHelper(kQuickMemcpy);
  } else {
    r_tgt = RegStorage::InvalidReg();
  }
  // NOTE: not a safepoint
  CallHelper(r_tgt, kQuickMemcpy, false, true);

  return true;
}

// Generates an inlined String.is_empty or String.length.
bool Mir2Lir::GenInlinedStringIsEmptyOrLength(CallInfo* info, bool is_empty) {
  if (cu_->instruction_set == kMips || cu_->instruction_set == kMips64) {
    // TODO: add Mips and Mips64 implementations.
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

bool Mir2Lir::GenInlinedStringFactoryNewStringFromBytes(CallInfo* info) {
  if (cu_->instruction_set == kMips) {
    // TODO - add Mips implementation
    return false;
  }
  RegLocation rl_data = info->args[0];
  RegLocation rl_high = info->args[1];
  RegLocation rl_offset = info->args[2];
  RegLocation rl_count = info->args[3];
  rl_data = LoadValue(rl_data, kRefReg);
  LIR* data_null_check_branch = OpCmpImmBranch(kCondEq, rl_data.reg, 0, nullptr);
  AddIntrinsicSlowPath(info, data_null_check_branch);
  CallRuntimeHelperRegLocationRegLocationRegLocationRegLocation(
      kQuickAllocStringFromBytes, rl_data, rl_high, rl_offset, rl_count, true);
  RegLocation rl_return = GetReturn(kRefReg);
  RegLocation rl_dest = InlineTarget(info);
  StoreValue(rl_dest, rl_return);
  return true;
}

bool Mir2Lir::GenInlinedStringFactoryNewStringFromChars(CallInfo* info) {
  if (cu_->instruction_set == kMips) {
    // TODO - add Mips implementation
    return false;
  }
  RegLocation rl_offset = info->args[0];
  RegLocation rl_count = info->args[1];
  RegLocation rl_data = info->args[2];
  CallRuntimeHelperRegLocationRegLocationRegLocation(
      kQuickAllocStringFromChars, rl_offset, rl_count, rl_data, true);
  RegLocation rl_return = GetReturn(kRefReg);
  RegLocation rl_dest = InlineTarget(info);
  StoreValue(rl_dest, rl_return);
  return true;
}

bool Mir2Lir::GenInlinedStringFactoryNewStringFromString(CallInfo* info) {
  if (cu_->instruction_set == kMips) {
    // TODO - add Mips implementation
    return false;
  }
  RegLocation rl_string = info->args[0];
  rl_string = LoadValue(rl_string, kRefReg);
  LIR* string_null_check_branch = OpCmpImmBranch(kCondEq, rl_string.reg, 0, nullptr);
  AddIntrinsicSlowPath(info, string_null_check_branch);
  CallRuntimeHelperRegLocation(kQuickAllocStringFromString, rl_string, true);
  RegLocation rl_return = GetReturn(kRefReg);
  RegLocation rl_dest = InlineTarget(info);
  StoreValue(rl_dest, rl_return);
  return true;
}

bool Mir2Lir::GenInlinedReverseBytes(CallInfo* info, OpSize size) {
  if (cu_->instruction_set == kMips || cu_->instruction_set == kMips64) {
    // TODO: add Mips and Mips64 implementations.
    return false;
  }
  RegLocation rl_dest = IsWide(size) ? InlineTargetWide(info) : InlineTarget(info);  // result reg
  if (rl_dest.s_reg_low == INVALID_SREG) {
    // Result is unused, the code is dead. Inlining successful, no code generated.
    return true;
  }
  RegLocation rl_src_i = info->args[0];
  RegLocation rl_i = IsWide(size) ? LoadValueWide(rl_src_i, kCoreReg) : LoadValue(rl_src_i, kCoreReg);
  RegLocation rl_result = EvalLoc(rl_dest, kCoreReg, true);
  if (IsWide(size)) {
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
  RegLocation rl_dest = InlineTarget(info);
  if (rl_dest.s_reg_low == INVALID_SREG) {
    // Result is unused, the code is dead. Inlining successful, no code generated.
    return true;
  }
  RegLocation rl_src = info->args[0];
  rl_src = LoadValue(rl_src, kCoreReg);
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
  RegLocation rl_dest = InlineTargetWide(info);
  if (rl_dest.s_reg_low == INVALID_SREG) {
    // Result is unused, the code is dead. Inlining successful, no code generated.
    return true;
  }
  RegLocation rl_src = info->args[0];
  rl_src = LoadValueWide(rl_src, kCoreReg);
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
  // Currently implemented only for ARM64.
  UNUSED(info, size);
  return false;
}

bool Mir2Lir::GenInlinedMinMaxFP(CallInfo* info, bool is_min, bool is_double) {
  // Currently implemented only for ARM64.
  UNUSED(info, is_min, is_double);
  return false;
}

bool Mir2Lir::GenInlinedCeil(CallInfo* info) {
  UNUSED(info);
  return false;
}

bool Mir2Lir::GenInlinedFloor(CallInfo* info) {
  UNUSED(info);
  return false;
}

bool Mir2Lir::GenInlinedRint(CallInfo* info) {
  UNUSED(info);
  return false;
}

bool Mir2Lir::GenInlinedRound(CallInfo* info, bool is_double) {
  UNUSED(info, is_double);
  return false;
}

bool Mir2Lir::GenInlinedFloatCvt(CallInfo* info) {
  if (cu_->instruction_set == kMips || cu_->instruction_set == kMips64) {
    // TODO: add Mips and Mips64 implementations.
    return false;
  }
  RegLocation rl_dest = InlineTarget(info);
  if (rl_dest.s_reg_low == INVALID_SREG) {
    // Result is unused, the code is dead. Inlining successful, no code generated.
    return true;
  }
  RegLocation rl_src = info->args[0];
  StoreValue(rl_dest, rl_src);
  return true;
}

bool Mir2Lir::GenInlinedDoubleCvt(CallInfo* info) {
  if (cu_->instruction_set == kMips || cu_->instruction_set == kMips64) {
    // TODO: add Mips and Mips64 implementations.
    return false;
  }
  RegLocation rl_dest = InlineTargetWide(info);
  if (rl_dest.s_reg_low == INVALID_SREG) {
    // Result is unused, the code is dead. Inlining successful, no code generated.
    return true;
  }
  RegLocation rl_src = info->args[0];
  StoreValueWide(rl_dest, rl_src);
  return true;
}

bool Mir2Lir::GenInlinedArrayCopyCharArray(CallInfo* info) {
  UNUSED(info);
  return false;
}


/*
 * Fast String.indexOf(I) & (II).  Tests for simple case of char <= 0xFFFF,
 * otherwise bails to standard library code.
 */
bool Mir2Lir::GenInlinedIndexOf(CallInfo* info, bool zero_based) {
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
    ClobberCallerSave();  // We must clobber everything because slow path will return here
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
  if (cu_->instruction_set == kMips || cu_->instruction_set == kMips64) {
    // TODO: add Mips and Mips64 implementations.
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

  if (cu_->target64) {
    LoadRefDisp(TargetPtrReg(kSelf), Thread::PeerOffset<8>().Int32Value(), rl_result.reg,
                kNotVolatile);
  } else {
    Load32Disp(TargetPtrReg(kSelf), Thread::PeerOffset<4>().Int32Value(), rl_result.reg);
  }

  StoreValue(rl_dest, rl_result);
  return true;
}

bool Mir2Lir::GenInlinedUnsafeGet(CallInfo* info,
                                  bool is_long, bool is_object, bool is_volatile) {
  if (cu_->instruction_set == kMips || cu_->instruction_set == kMips64) {
    // TODO: add Mips and Mips64 implementations.
    return false;
  }
  // Unused - RegLocation rl_src_unsafe = info->args[0];
  RegLocation rl_src_obj = info->args[1];  // Object
  RegLocation rl_src_offset = info->args[2];  // long low
  rl_src_offset = NarrowRegLoc(rl_src_offset);  // ignore high half in info->args[3]
  RegLocation rl_dest = is_long ? InlineTargetWide(info) : InlineTarget(info);  // result reg

  RegLocation rl_object = LoadValue(rl_src_obj, kRefReg);
  RegLocation rl_offset = LoadValue(rl_src_offset, kCoreReg);
  RegLocation rl_result = EvalLoc(rl_dest, is_object ? kRefReg : kCoreReg, true);
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
  if (cu_->instruction_set == kMips || cu_->instruction_set == kMips64) {
    // TODO: add Mips and Mips64 implementations.
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
    rl_value = LoadValue(rl_src_value, is_object ? kRefReg : kCoreReg);
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
    MarkGCCard(0, rl_value.reg, rl_object.reg);
  }
  return true;
}

void Mir2Lir::GenInvoke(CallInfo* info) {
  DCHECK(cu_->compiler_driver->GetMethodInlinerMap() != nullptr);
  if (mir_graph_->GetMethodLoweringInfo(info->mir).IsIntrinsic()) {
    const DexFile* dex_file = info->method_ref.dex_file;
    auto* inliner = cu_->compiler_driver->GetMethodInlinerMap()->GetMethodInliner(dex_file);
    if (inliner->GenIntrinsic(this, info)) {
      return;
    }
  }
  GenInvokeNoInline(info);
}

void Mir2Lir::GenInvokeNoInline(CallInfo* info) {
  int call_state = 0;
  LIR* null_ck;
  LIR** p_null_ck = nullptr;
  NextCallInsn next_call_insn;
  FlushAllRegs();  /* Everything to home location */
  // Explicit register usage
  LockCallTemps();

  const MirMethodLoweringInfo& method_info = mir_graph_->GetMethodLoweringInfo(info->mir);
  MethodReference target_method = method_info.GetTargetMethod();
  cu_->compiler_driver->ProcessedInvoke(method_info.GetInvokeType(), method_info.StatsFlags());
  InvokeType original_type = static_cast<InvokeType>(method_info.GetInvokeType());
  info->type = method_info.GetSharpType();
  bool is_string_init = false;
  if (method_info.IsSpecial()) {
    DexFileMethodInliner* inliner = cu_->compiler_driver->GetMethodInlinerMap()->GetMethodInliner(
        target_method.dex_file);
    if (inliner->IsStringInitMethodIndex(target_method.dex_method_index)) {
      is_string_init = true;
      size_t pointer_size = GetInstructionSetPointerSize(cu_->instruction_set);
      info->string_init_offset = inliner->GetOffsetForStringInit(target_method.dex_method_index,
                                                                 pointer_size);
      info->type = kStatic;
    }
  }
  bool fast_path = method_info.FastPath();
  bool skip_this;

  if (info->type == kInterface) {
    next_call_insn = fast_path ? NextInterfaceCallInsn : NextInterfaceCallInsnWithAccessCheck;
    skip_this = fast_path;
  } else if (info->type == kDirect) {
    if (fast_path) {
      p_null_ck = &null_ck;
    }
    next_call_insn = fast_path ? GetNextSDCallInsn() : NextDirectCallInsnSP;
    skip_this = false;
  } else if (info->type == kStatic) {
    next_call_insn = fast_path ? GetNextSDCallInsn() : NextStaticCallInsnSP;
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
  call_state = GenDalvikArgs(info, call_state, p_null_ck,
                             next_call_insn, target_method, method_info.VTableIndex(),
                             method_info.DirectCode(), method_info.DirectMethod(),
                             original_type, skip_this);
  // Finish up any of the call sequence not interleaved in arg loading
  while (call_state >= 0) {
    call_state = next_call_insn(cu_, info, call_state, target_method, method_info.VTableIndex(),
                                method_info.DirectCode(), method_info.DirectMethod(),
                                original_type);
  }
  LIR* call_insn = GenCallInsn(method_info);
  MarkSafepointPC(call_insn);

  FreeCallTemps();
  if (info->result.location != kLocInvalid) {
    // We have a following MOVE_RESULT - do it now.
    RegisterClass reg_class = is_string_init ? kRefReg :
        ShortyToRegClass(mir_graph_->GetShortyFromMethodReference(info->method_ref)[0]);
    if (info->result.wide) {
      RegLocation ret_loc = GetReturnWide(reg_class);
      StoreValueWide(info->result, ret_loc);
    } else {
      RegLocation ret_loc = GetReturn(reg_class);
      StoreValue(info->result, ret_loc);
    }
  }
}

}  // namespace art
