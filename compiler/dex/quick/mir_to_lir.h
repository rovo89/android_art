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

#ifndef ART_COMPILER_DEX_QUICK_MIR_TO_LIR_H_
#define ART_COMPILER_DEX_QUICK_MIR_TO_LIR_H_

#include "invoke_type.h"
#include "compiled_method.h"
#include "dex/compiler_enums.h"
#include "dex/compiler_ir.h"
#include "dex/reg_location.h"
#include "dex/reg_storage.h"
#include "dex/backend.h"
#include "dex/quick/resource_mask.h"
#include "driver/compiler_driver.h"
#include "instruction_set.h"
#include "leb128.h"
#include "entrypoints/quick/quick_entrypoints_enum.h"
#include "safe_map.h"
#include "utils/array_ref.h"
#include "utils/arena_allocator.h"
#include "utils/arena_containers.h"
#include "utils/growable_array.h"
#include "utils/stack_checks.h"

namespace art {

/*
 * TODO: refactoring pass to move these (and other) typdefs towards usage style of runtime to
 * add type safety (see runtime/offsets.h).
 */
typedef uint32_t DexOffset;          // Dex offset in code units.
typedef uint16_t NarrowDexOffset;    // For use in structs, Dex offsets range from 0 .. 0xffff.
typedef uint32_t CodeOffset;         // Native code offset in bytes.

// Set to 1 to measure cost of suspend check.
#define NO_SUSPEND 0

#define IS_BINARY_OP         (1ULL << kIsBinaryOp)
#define IS_BRANCH            (1ULL << kIsBranch)
#define IS_IT                (1ULL << kIsIT)
#define IS_MOVE              (1ULL << kIsMoveOp)
#define IS_LOAD              (1ULL << kMemLoad)
#define IS_QUAD_OP           (1ULL << kIsQuadOp)
#define IS_QUIN_OP           (1ULL << kIsQuinOp)
#define IS_SEXTUPLE_OP       (1ULL << kIsSextupleOp)
#define IS_STORE             (1ULL << kMemStore)
#define IS_TERTIARY_OP       (1ULL << kIsTertiaryOp)
#define IS_UNARY_OP          (1ULL << kIsUnaryOp)
#define IS_VOLATILE          (1ULL << kMemVolatile)
#define NEEDS_FIXUP          (1ULL << kPCRelFixup)
#define NO_OPERAND           (1ULL << kNoOperand)
#define REG_DEF0             (1ULL << kRegDef0)
#define REG_DEF1             (1ULL << kRegDef1)
#define REG_DEF2             (1ULL << kRegDef2)
#define REG_DEFA             (1ULL << kRegDefA)
#define REG_DEFD             (1ULL << kRegDefD)
#define REG_DEF_FPCS_LIST0   (1ULL << kRegDefFPCSList0)
#define REG_DEF_FPCS_LIST2   (1ULL << kRegDefFPCSList2)
#define REG_DEF_LIST0        (1ULL << kRegDefList0)
#define REG_DEF_LIST1        (1ULL << kRegDefList1)
#define REG_DEF_LR           (1ULL << kRegDefLR)
#define REG_DEF_SP           (1ULL << kRegDefSP)
#define REG_USE0             (1ULL << kRegUse0)
#define REG_USE1             (1ULL << kRegUse1)
#define REG_USE2             (1ULL << kRegUse2)
#define REG_USE3             (1ULL << kRegUse3)
#define REG_USE4             (1ULL << kRegUse4)
#define REG_USEA             (1ULL << kRegUseA)
#define REG_USEC             (1ULL << kRegUseC)
#define REG_USED             (1ULL << kRegUseD)
#define REG_USEB             (1ULL << kRegUseB)
#define REG_USE_FPCS_LIST0   (1ULL << kRegUseFPCSList0)
#define REG_USE_FPCS_LIST2   (1ULL << kRegUseFPCSList2)
#define REG_USE_LIST0        (1ULL << kRegUseList0)
#define REG_USE_LIST1        (1ULL << kRegUseList1)
#define REG_USE_LR           (1ULL << kRegUseLR)
#define REG_USE_PC           (1ULL << kRegUsePC)
#define REG_USE_SP           (1ULL << kRegUseSP)
#define SETS_CCODES          (1ULL << kSetsCCodes)
#define USES_CCODES          (1ULL << kUsesCCodes)
#define USE_FP_STACK         (1ULL << kUseFpStack)
#define REG_USE_LO           (1ULL << kUseLo)
#define REG_USE_HI           (1ULL << kUseHi)
#define REG_DEF_LO           (1ULL << kDefLo)
#define REG_DEF_HI           (1ULL << kDefHi)
#define SCALED_OFFSET_X0     (1ULL << kMemScaledx0)
#define SCALED_OFFSET_X2     (1ULL << kMemScaledx2)
#define SCALED_OFFSET_X4     (1ULL << kMemScaledx4)

// Special load/stores
#define IS_LOADX             (IS_LOAD | IS_VOLATILE)
#define IS_LOAD_OFF          (IS_LOAD | SCALED_OFFSET_X0)
#define IS_LOAD_OFF2         (IS_LOAD | SCALED_OFFSET_X2)
#define IS_LOAD_OFF4         (IS_LOAD | SCALED_OFFSET_X4)

#define IS_STOREX            (IS_STORE | IS_VOLATILE)
#define IS_STORE_OFF         (IS_STORE | SCALED_OFFSET_X0)
#define IS_STORE_OFF2        (IS_STORE | SCALED_OFFSET_X2)
#define IS_STORE_OFF4        (IS_STORE | SCALED_OFFSET_X4)

// Common combo register usage patterns.
#define REG_DEF01            (REG_DEF0 | REG_DEF1)
#define REG_DEF012           (REG_DEF0 | REG_DEF1 | REG_DEF2)
#define REG_DEF01_USE2       (REG_DEF0 | REG_DEF1 | REG_USE2)
#define REG_DEF0_USE01       (REG_DEF0 | REG_USE01)
#define REG_DEF0_USE0        (REG_DEF0 | REG_USE0)
#define REG_DEF0_USE12       (REG_DEF0 | REG_USE12)
#define REG_DEF0_USE123      (REG_DEF0 | REG_USE123)
#define REG_DEF0_USE1        (REG_DEF0 | REG_USE1)
#define REG_DEF0_USE2        (REG_DEF0 | REG_USE2)
#define REG_DEFAD_USEAD      (REG_DEFAD_USEA | REG_USED)
#define REG_DEFAD_USEA       (REG_DEFA_USEA | REG_DEFD)
#define REG_DEFA_USEA        (REG_DEFA | REG_USEA)
#define REG_USE012           (REG_USE01 | REG_USE2)
#define REG_USE014           (REG_USE01 | REG_USE4)
#define REG_USE01            (REG_USE0 | REG_USE1)
#define REG_USE02            (REG_USE0 | REG_USE2)
#define REG_USE12            (REG_USE1 | REG_USE2)
#define REG_USE23            (REG_USE2 | REG_USE3)
#define REG_USE123           (REG_USE1 | REG_USE2 | REG_USE3)

// TODO: #includes need a cleanup
#ifndef INVALID_SREG
#define INVALID_SREG (-1)
#endif

struct BasicBlock;
struct CallInfo;
struct CompilationUnit;
struct InlineMethod;
struct MIR;
struct LIR;
struct RegisterInfo;
class DexFileMethodInliner;
class MIRGraph;
class Mir2Lir;


typedef int (*NextCallInsn)(CompilationUnit*, CallInfo*, int,
                            const MethodReference& target_method,
                            uint32_t method_idx, uintptr_t direct_code,
                            uintptr_t direct_method, InvokeType type);

typedef std::vector<uint8_t> CodeBuffer;

struct UseDefMasks {
  const ResourceMask* use_mask;        // Resource mask for use.
  const ResourceMask* def_mask;        // Resource mask for def.
};

struct AssemblyInfo {
  LIR* pcrel_next;           // Chain of LIR nodes needing pc relative fixups.
};

struct LIR {
  CodeOffset offset;             // Offset of this instruction.
  NarrowDexOffset dalvik_offset;   // Offset of Dalvik opcode in code units (16-bit words).
  int16_t opcode;
  LIR* next;
  LIR* prev;
  LIR* target;
  struct {
    unsigned int alias_info:17;  // For Dalvik register disambiguation.
    bool is_nop:1;               // LIR is optimized away.
    unsigned int size:4;         // Note: size of encoded instruction is in bytes.
    bool use_def_invalid:1;      // If true, masks should not be used.
    unsigned int generation:1;   // Used to track visitation state during fixup pass.
    unsigned int fixup:8;        // Fixup kind.
  } flags;
  union {
    UseDefMasks m;               // Use & Def masks used during optimization.
    AssemblyInfo a;              // Instruction info used during assembly phase.
  } u;
  int32_t operands[5];           // [0..4] = [dest, src1, src2, extra, extra2].
};

// Target-specific initialization.
Mir2Lir* ArmCodeGenerator(CompilationUnit* const cu, MIRGraph* const mir_graph,
                          ArenaAllocator* const arena);
Mir2Lir* Arm64CodeGenerator(CompilationUnit* const cu, MIRGraph* const mir_graph,
                            ArenaAllocator* const arena);
Mir2Lir* MipsCodeGenerator(CompilationUnit* const cu, MIRGraph* const mir_graph,
                          ArenaAllocator* const arena);
Mir2Lir* X86CodeGenerator(CompilationUnit* const cu, MIRGraph* const mir_graph,
                          ArenaAllocator* const arena);

// Utility macros to traverse the LIR list.
#define NEXT_LIR(lir) (lir->next)
#define PREV_LIR(lir) (lir->prev)

// Defines for alias_info (tracks Dalvik register references).
#define DECODE_ALIAS_INFO_REG(X)        (X & 0xffff)
#define DECODE_ALIAS_INFO_WIDE_FLAG     (0x10000)
#define DECODE_ALIAS_INFO_WIDE(X)       ((X & DECODE_ALIAS_INFO_WIDE_FLAG) ? 1 : 0)
#define ENCODE_ALIAS_INFO(REG, ISWIDE)  (REG | (ISWIDE ? DECODE_ALIAS_INFO_WIDE_FLAG : 0))

#define ENCODE_REG_PAIR(low_reg, high_reg) ((low_reg & 0xff) | ((high_reg & 0xff) << 8))
#define DECODE_REG_PAIR(both_regs, low_reg, high_reg) \
  do { \
    low_reg = both_regs & 0xff; \
    high_reg = (both_regs >> 8) & 0xff; \
  } while (false)

// Mask to denote sreg as the start of a 64-bit item.  Must not interfere with low 16 bits.
#define STARTING_WIDE_SREG 0x10000

// TODO: replace these macros
#define SLOW_FIELD_PATH (cu_->enable_debug & (1 << kDebugSlowFieldPath))
#define SLOW_INVOKE_PATH (cu_->enable_debug & (1 << kDebugSlowInvokePath))
#define SLOW_STRING_PATH (cu_->enable_debug & (1 << kDebugSlowStringPath))
#define SLOW_TYPE_PATH (cu_->enable_debug & (1 << kDebugSlowTypePath))
#define EXERCISE_SLOWEST_STRING_PATH (cu_->enable_debug & (1 << kDebugSlowestStringPath))

class Mir2Lir : public Backend {
  public:
    static constexpr bool kFailOnSizeError = true && kIsDebugBuild;
    static constexpr bool kReportSizeError = true && kIsDebugBuild;

    // TODO: If necessary, this could be made target-dependent.
    static constexpr uint16_t kSmallSwitchThreshold = 5;

    /*
     * Auxiliary information describing the location of data embedded in the Dalvik
     * byte code stream.
     */
    struct EmbeddedData {
      CodeOffset offset;        // Code offset of data block.
      const uint16_t* table;      // Original dex data.
      DexOffset vaddr;            // Dalvik offset of parent opcode.
    };

    struct FillArrayData : EmbeddedData {
      int32_t size;
    };

    struct SwitchTable : EmbeddedData {
      LIR* anchor;                // Reference instruction for relative offsets.
      LIR** targets;              // Array of case targets.
    };

    /* Static register use counts */
    struct RefCounts {
      int count;
      int s_reg;
    };

    /*
     * Data structure tracking the mapping detween a Dalvik value (32 or 64 bits)
     * and native register storage.  The primary purpose is to reuse previuosly
     * loaded values, if possible, and otherwise to keep the value in register
     * storage as long as possible.
     *
     * NOTE 1: wide_value refers to the width of the Dalvik value contained in
     * this register (or pair).  For example, a 64-bit register containing a 32-bit
     * Dalvik value would have wide_value==false even though the storage container itself
     * is wide.  Similarly, a 32-bit register containing half of a 64-bit Dalvik value
     * would have wide_value==true (and additionally would have its partner field set to the
     * other half whose wide_value field would also be true.
     *
     * NOTE 2: In the case of a register pair, you can determine which of the partners
     * is the low half by looking at the s_reg names.  The high s_reg will equal low_sreg + 1.
     *
     * NOTE 3: In the case of a 64-bit register holding a Dalvik wide value, wide_value
     * will be true and partner==self.  s_reg refers to the low-order word of the Dalvik
     * value, and the s_reg of the high word is implied (s_reg + 1).
     *
     * NOTE 4: The reg and is_temp fields should always be correct.  If is_temp is false no
     * other fields have meaning. [perhaps not true, wide should work for promoted regs?]
     * If is_temp==true and live==false, no other fields have
     * meaning.  If is_temp==true and live==true, wide_value, partner, dirty, s_reg, def_start
     * and def_end describe the relationship between the temp register/register pair and
     * the Dalvik value[s] described by s_reg/s_reg+1.
     *
     * The fields used_storage, master_storage and storage_mask are used to track allocation
     * in light of potential aliasing.  For example, consider Arm's d2, which overlaps s4 & s5.
     * d2's storage mask would be 0x00000003, the two low-order bits denoting 64 bits of
     * storage use.  For s4, it would be 0x0000001; for s5 0x00000002.  These values should not
     * change once initialized.  The "used_storage" field tracks current allocation status.
     * Although each record contains this field, only the field from the largest member of
     * an aliased group is used.  In our case, it would be d2's.  The master_storage pointer
     * of d2, s4 and s5 would all point to d2's used_storage field.  Each bit in a used_storage
     * represents 32 bits of storage.  d2's used_storage would be initialized to 0xfffffffc.
     * Then, if we wanted to determine whether s4 could be allocated, we would "and"
     * s4's storage_mask with s4's *master_storage.  If the result is zero, s4 is free and
     * to allocate: *master_storage |= storage_mask.  To free, *master_storage &= ~storage_mask.
     *
     * For an X86 vector register example, storage_mask would be:
     *    0x00000001 for 32-bit view of xmm1
     *    0x00000003 for 64-bit view of xmm1
     *    0x0000000f for 128-bit view of xmm1
     *    0x000000ff for 256-bit view of ymm1   // future expansion, if needed
     *    0x0000ffff for 512-bit view of ymm1   // future expansion, if needed
     *    0xffffffff for 1024-bit view of ymm1  // future expansion, if needed
     *
     * The "liveness" of a register is handled in a similar way.  The liveness_ storage is
     * held in the widest member of an aliased set.  Note, though, that for a temp register to
     * reused as live, it must both be marked live and the associated SReg() must match the
     * desired s_reg.  This gets a little complicated when dealing with aliased registers.  All
     * members of an aliased set will share the same liveness flags, but each will individually
     * maintain s_reg_.  In this way we can know that at least one member of an
     * aliased set is live, but will only fully match on the appropriate alias view.  For example,
     * if Arm d1 is live as a double and has s_reg_ set to Dalvik v8 (which also implies v9
     * because it is wide), its aliases s2 and s3 will show as live, but will have
     * s_reg_ == INVALID_SREG.  An attempt to later AllocLiveReg() of v9 with a single-precision
     * view will fail because although s3's liveness bit is set, its s_reg_ will not match v9.
     * This will cause all members of the aliased set to be clobbered and AllocLiveReg() will
     * report that v9 is currently not live as a single (which is what we want).
     *
     * NOTE: the x86 usage is still somewhat in flux.  There are competing notions of how
     * to treat xmm registers:
     *     1. Treat them all as 128-bits wide, but denote how much data used via bytes field.
     *         o This more closely matches reality, but means you'd need to be able to get
     *           to the associated RegisterInfo struct to figure out how it's being used.
     *         o This is how 64-bit core registers will be used - always 64 bits, but the
     *           "bytes" field will be 4 for 32-bit usage and 8 for 64-bit usage.
     *     2. View the xmm registers based on contents.
     *         o A single in a xmm2 register would be k32BitVector, while a double in xmm2 would
     *           be a k64BitVector.
     *         o Note that the two uses above would be considered distinct registers (but with
     *           the aliasing mechanism, we could detect interference).
     *         o This is how aliased double and single float registers will be handled on
     *           Arm and MIPS.
     * Working plan is, for all targets, to follow mechanism 1 for 64-bit core registers, and
     * mechanism 2 for aliased float registers and x86 vector registers.
     */
    class RegisterInfo {
     public:
      RegisterInfo(RegStorage r, const ResourceMask& mask = kEncodeAll);
      ~RegisterInfo() {}
      static void* operator new(size_t size, ArenaAllocator* arena) {
        return arena->Alloc(size, kArenaAllocRegAlloc);
      }

      static const uint32_t k32SoloStorageMask     = 0x00000001;
      static const uint32_t kLowSingleStorageMask  = 0x00000001;
      static const uint32_t kHighSingleStorageMask = 0x00000002;
      static const uint32_t k64SoloStorageMask     = 0x00000003;
      static const uint32_t kLowDoubleStorageMask  = 0x00000003;
      static const uint32_t kHighDoubleStorageMask = 0x0000000c;
      static const uint32_t k128SoloStorageMask    = 0x0000000f;
      static const uint32_t k256SoloStorageMask    = 0x000000ff;
      static const uint32_t k512SoloStorageMask    = 0x0000ffff;
      static const uint32_t k1024SoloStorageMask   = 0xffffffff;

      bool InUse() { return (storage_mask_ & master_->used_storage_) != 0; }
      void MarkInUse() { master_->used_storage_ |= storage_mask_; }
      void MarkFree() { master_->used_storage_ &= ~storage_mask_; }
      // No part of the containing storage is live in this view.
      bool IsDead() { return (master_->liveness_ & storage_mask_) == 0; }
      // Liveness of this view matches.  Note: not equivalent to !IsDead().
      bool IsLive() { return (master_->liveness_ & storage_mask_) == storage_mask_; }
      void MarkLive(int s_reg) {
        // TODO: Anything useful to assert here?
        s_reg_ = s_reg;
        master_->liveness_ |= storage_mask_;
      }
      void MarkDead() {
        if (SReg() != INVALID_SREG) {
          s_reg_ = INVALID_SREG;
          master_->liveness_ &= ~storage_mask_;
          ResetDefBody();
        }
      }
      RegStorage GetReg() { return reg_; }
      void SetReg(RegStorage reg) { reg_ = reg; }
      bool IsTemp() { return is_temp_; }
      void SetIsTemp(bool val) { is_temp_ = val; }
      bool IsWide() { return wide_value_; }
      void SetIsWide(bool val) {
        wide_value_ = val;
        if (!val) {
          // If not wide, reset partner to self.
          SetPartner(GetReg());
        }
      }
      bool IsDirty() { return dirty_; }
      void SetIsDirty(bool val) { dirty_ = val; }
      RegStorage Partner() { return partner_; }
      void SetPartner(RegStorage partner) { partner_ = partner; }
      int SReg() { return (!IsTemp() || IsLive()) ? s_reg_ : INVALID_SREG; }
      const ResourceMask& DefUseMask() { return def_use_mask_; }
      void SetDefUseMask(const ResourceMask& def_use_mask) { def_use_mask_ = def_use_mask; }
      RegisterInfo* Master() { return master_; }
      void SetMaster(RegisterInfo* master) {
        master_ = master;
        if (master != this) {
          master_->aliased_ = true;
          DCHECK(alias_chain_ == nullptr);
          alias_chain_ = master_->alias_chain_;
          master_->alias_chain_ = this;
        }
      }
      bool IsAliased() { return aliased_; }
      RegisterInfo* GetAliasChain() { return alias_chain_; }
      uint32_t StorageMask() { return storage_mask_; }
      void SetStorageMask(uint32_t storage_mask) { storage_mask_ = storage_mask; }
      LIR* DefStart() { return def_start_; }
      void SetDefStart(LIR* def_start) { def_start_ = def_start; }
      LIR* DefEnd() { return def_end_; }
      void SetDefEnd(LIR* def_end) { def_end_ = def_end; }
      void ResetDefBody() { def_start_ = def_end_ = nullptr; }
      // Find member of aliased set matching storage_used; return nullptr if none.
      RegisterInfo* FindMatchingView(uint32_t storage_used) {
        RegisterInfo* res = Master();
        for (; res != nullptr; res = res->GetAliasChain()) {
          if (res->StorageMask() == storage_used)
            break;
        }
        return res;
      }

     private:
      RegStorage reg_;
      bool is_temp_;               // Can allocate as temp?
      bool wide_value_;            // Holds a Dalvik wide value (either itself, or part of a pair).
      bool dirty_;                 // If live, is it dirty?
      bool aliased_;               // Is this the master for other aliased RegisterInfo's?
      RegStorage partner_;         // If wide_value, other reg of pair or self if 64-bit register.
      int s_reg_;                  // Name of live value.
      ResourceMask def_use_mask_;  // Resources for this element.
      uint32_t used_storage_;      // 1 bit per 4 bytes of storage. Unused by aliases.
      uint32_t liveness_;          // 1 bit per 4 bytes of storage. Unused by aliases.
      RegisterInfo* master_;       // Pointer to controlling storage mask.
      uint32_t storage_mask_;      // Track allocation of sub-units.
      LIR *def_start_;             // Starting inst in last def sequence.
      LIR *def_end_;               // Ending inst in last def sequence.
      RegisterInfo* alias_chain_;  // Chain of aliased registers.
    };

    class RegisterPool {
     public:
      RegisterPool(Mir2Lir* m2l, ArenaAllocator* arena,
                   const ArrayRef<const RegStorage>& core_regs,
                   const ArrayRef<const RegStorage>& core64_regs,
                   const ArrayRef<const RegStorage>& sp_regs,
                   const ArrayRef<const RegStorage>& dp_regs,
                   const ArrayRef<const RegStorage>& reserved_regs,
                   const ArrayRef<const RegStorage>& reserved64_regs,
                   const ArrayRef<const RegStorage>& core_temps,
                   const ArrayRef<const RegStorage>& core64_temps,
                   const ArrayRef<const RegStorage>& sp_temps,
                   const ArrayRef<const RegStorage>& dp_temps);
      ~RegisterPool() {}
      static void* operator new(size_t size, ArenaAllocator* arena) {
        return arena->Alloc(size, kArenaAllocRegAlloc);
      }
      void ResetNextTemp() {
        next_core_reg_ = 0;
        next_sp_reg_ = 0;
        next_dp_reg_ = 0;
      }
      GrowableArray<RegisterInfo*> core_regs_;
      int next_core_reg_;
      GrowableArray<RegisterInfo*> core64_regs_;
      int next_core64_reg_;
      GrowableArray<RegisterInfo*> sp_regs_;    // Single precision float.
      int next_sp_reg_;
      GrowableArray<RegisterInfo*> dp_regs_;    // Double precision float.
      int next_dp_reg_;
      GrowableArray<RegisterInfo*>* ref_regs_;  // Points to core_regs_ or core64_regs_
      int* next_ref_reg_;

     private:
      Mir2Lir* const m2l_;
    };

    struct PromotionMap {
      RegLocationType core_location:3;
      uint8_t core_reg;
      RegLocationType fp_location:3;
      uint8_t fp_reg;
      bool first_in_pair;
    };

    //
    // Slow paths.  This object is used generate a sequence of code that is executed in the
    // slow path.  For example, resolving a string or class is slow as it will only be executed
    // once (after that it is resolved and doesn't need to be done again).  We want slow paths
    // to be placed out-of-line, and not require a (mispredicted, probably) conditional forward
    // branch over them.
    //
    // If you want to create a slow path, declare a class derived from LIRSlowPath and provide
    // the Compile() function that will be called near the end of the code generated by the
    // method.
    //
    // The basic flow for a slow path is:
    //
    //     CMP reg, #value
    //     BEQ fromfast
    //   cont:
    //     ...
    //     fast path code
    //     ...
    //     more code
    //     ...
    //     RETURN
    ///
    //   fromfast:
    //     ...
    //     slow path code
    //     ...
    //     B cont
    //
    // So you see we need two labels and two branches.  The first branch (called fromfast) is
    // the conditional branch to the slow path code.  The second label (called cont) is used
    // as an unconditional branch target for getting back to the code after the slow path
    // has completed.
    //

    class LIRSlowPath {
     public:
      LIRSlowPath(Mir2Lir* m2l, const DexOffset dexpc, LIR* fromfast,
                  LIR* cont = nullptr) :
        m2l_(m2l), cu_(m2l->cu_), current_dex_pc_(dexpc), fromfast_(fromfast), cont_(cont) {
          m2l->StartSlowPath(this);
      }
      virtual ~LIRSlowPath() {}
      virtual void Compile() = 0;

      static void* operator new(size_t size, ArenaAllocator* arena) {
        return arena->Alloc(size, kArenaAllocData);
      }

      LIR *GetContinuationLabel() {
        return cont_;
      }

      LIR *GetFromFast() {
        return fromfast_;
      }

     protected:
      LIR* GenerateTargetLabel(int opcode = kPseudoTargetLabel);

      Mir2Lir* const m2l_;
      CompilationUnit* const cu_;
      const DexOffset current_dex_pc_;
      LIR* const fromfast_;
      LIR* const cont_;
    };

    // Helper class for changing mem_ref_type_ until the end of current scope. See mem_ref_type_.
    class ScopedMemRefType {
     public:
      ScopedMemRefType(Mir2Lir* m2l, ResourceMask::ResourceBit new_mem_ref_type)
          : m2l_(m2l),
            old_mem_ref_type_(m2l->mem_ref_type_) {
        m2l_->mem_ref_type_ = new_mem_ref_type;
      }

      ~ScopedMemRefType() {
        m2l_->mem_ref_type_ = old_mem_ref_type_;
      }

     private:
      Mir2Lir* const m2l_;
      ResourceMask::ResourceBit old_mem_ref_type_;

      DISALLOW_COPY_AND_ASSIGN(ScopedMemRefType);
    };

    virtual ~Mir2Lir() {}

    /**
     * @brief Decodes the LIR offset.
     * @return Returns the scaled offset of LIR.
     */
    virtual size_t GetInstructionOffset(LIR* lir);

    int32_t s4FromSwitchData(const void* switch_data) {
      return *reinterpret_cast<const int32_t*>(switch_data);
    }

    /*
     * TODO: this is a trace JIT vestige, and its use should be reconsidered.  At the time
     * it was introduced, it was intended to be a quick best guess of type without having to
     * take the time to do type analysis.  Currently, though, we have a much better idea of
     * the types of Dalvik virtual registers.  Instead of using this for a best guess, why not
     * just use our knowledge of type to select the most appropriate register class?
     */
    RegisterClass RegClassBySize(OpSize size) {
      if (size == kReference) {
        return kRefReg;
      } else {
        return (size == kUnsignedHalf || size == kSignedHalf || size == kUnsignedByte ||
                size == kSignedByte) ? kCoreReg : kAnyReg;
      }
    }

    size_t CodeBufferSizeInBytes() {
      return code_buffer_.size() / sizeof(code_buffer_[0]);
    }

    static bool IsPseudoLirOp(int opcode) {
      return (opcode < 0);
    }

    /*
     * LIR operands are 32-bit integers.  Sometimes, (especially for managing
     * instructions which require PC-relative fixups), we need the operands to carry
     * pointers.  To do this, we assign these pointers an index in pointer_storage_, and
     * hold that index in the operand array.
     * TUNING: If use of these utilities becomes more common on 32-bit builds, it
     * may be worth conditionally-compiling a set of identity functions here.
     */
    uint32_t WrapPointer(void* pointer) {
      uint32_t res = pointer_storage_.Size();
      pointer_storage_.Insert(pointer);
      return res;
    }

    void* UnwrapPointer(size_t index) {
      return pointer_storage_.Get(index);
    }

    // strdup(), but allocates from the arena.
    char* ArenaStrdup(const char* str) {
      size_t len = strlen(str) + 1;
      char* res = reinterpret_cast<char*>(arena_->Alloc(len, kArenaAllocMisc));
      if (res != NULL) {
        strncpy(res, str, len);
      }
      return res;
    }

    // Shared by all targets - implemented in codegen_util.cc
    void AppendLIR(LIR* lir);
    void InsertLIRBefore(LIR* current_lir, LIR* new_lir);
    void InsertLIRAfter(LIR* current_lir, LIR* new_lir);

    /**
     * @brief Provides the maximum number of compiler temporaries that the backend can/wants
     * to place in a frame.
     * @return Returns the maximum number of compiler temporaries.
     */
    size_t GetMaxPossibleCompilerTemps() const;

    /**
     * @brief Provides the number of bytes needed in frame for spilling of compiler temporaries.
     * @return Returns the size in bytes for space needed for compiler temporary spill region.
     */
    size_t GetNumBytesForCompilerTempSpillRegion();

    DexOffset GetCurrentDexPc() const {
      return current_dalvik_offset_;
    }

    RegisterClass ShortyToRegClass(char shorty_type);
    RegisterClass LocToRegClass(RegLocation loc);
    int ComputeFrameSize();
    virtual void Materialize();
    virtual CompiledMethod* GetCompiledMethod();
    void MarkSafepointPC(LIR* inst);
    void MarkSafepointPCAfter(LIR* after);
    void SetupResourceMasks(LIR* lir);
    void SetMemRefType(LIR* lir, bool is_load, int mem_type);
    void AnnotateDalvikRegAccess(LIR* lir, int reg_id, bool is_load, bool is64bit);
    void SetupRegMask(ResourceMask* mask, int reg);
    void ClearRegMask(ResourceMask* mask, int reg);
    void DumpLIRInsn(LIR* arg, unsigned char* base_addr);
    void EliminateLoad(LIR* lir, int reg_id);
    void DumpDependentInsnPair(LIR* check_lir, LIR* this_lir, const char* type);
    void DumpPromotionMap();
    void CodegenDump();
    LIR* RawLIR(DexOffset dalvik_offset, int opcode, int op0 = 0, int op1 = 0,
                int op2 = 0, int op3 = 0, int op4 = 0, LIR* target = NULL);
    LIR* NewLIR0(int opcode);
    LIR* NewLIR1(int opcode, int dest);
    LIR* NewLIR2(int opcode, int dest, int src1);
    LIR* NewLIR2NoDest(int opcode, int src, int info);
    LIR* NewLIR3(int opcode, int dest, int src1, int src2);
    LIR* NewLIR4(int opcode, int dest, int src1, int src2, int info);
    LIR* NewLIR5(int opcode, int dest, int src1, int src2, int info1, int info2);
    LIR* ScanLiteralPool(LIR* data_target, int value, unsigned int delta);
    LIR* ScanLiteralPoolWide(LIR* data_target, int val_lo, int val_hi);
    LIR* ScanLiteralPoolMethod(LIR* data_target, const MethodReference& method);
    LIR* AddWordData(LIR* *constant_list_p, int value);
    LIR* AddWideData(LIR* *constant_list_p, int val_lo, int val_hi);
    void ProcessSwitchTables();
    void DumpSparseSwitchTable(const uint16_t* table);
    void DumpPackedSwitchTable(const uint16_t* table);
    void MarkBoundary(DexOffset offset, const char* inst_str);
    void NopLIR(LIR* lir);
    void UnlinkLIR(LIR* lir);
    bool EvaluateBranch(Instruction::Code opcode, int src1, int src2);
    bool IsInexpensiveConstant(RegLocation rl_src);
    ConditionCode FlipComparisonOrder(ConditionCode before);
    ConditionCode NegateComparison(ConditionCode before);
    virtual void InstallLiteralPools();
    void InstallSwitchTables();
    void InstallFillArrayData();
    bool VerifyCatchEntries();
    void CreateMappingTables();
    void CreateNativeGcMap();
    int AssignLiteralOffset(CodeOffset offset);
    int AssignSwitchTablesOffset(CodeOffset offset);
    int AssignFillArrayDataOffset(CodeOffset offset);
    virtual LIR* InsertCaseLabel(DexOffset vaddr, int keyVal);
    void MarkPackedCaseLabels(Mir2Lir::SwitchTable* tab_rec);
    void MarkSparseCaseLabels(Mir2Lir::SwitchTable* tab_rec);

    virtual void StartSlowPath(LIRSlowPath* slowpath) {}
    virtual void BeginInvoke(CallInfo* info) {}
    virtual void EndInvoke(CallInfo* info) {}


    // Handle bookkeeping to convert a wide RegLocation to a narrow RegLocation.  No code generated.
    virtual RegLocation NarrowRegLoc(RegLocation loc);

    // Shared by all targets - implemented in local_optimizations.cc
    void ConvertMemOpIntoMove(LIR* orig_lir, RegStorage dest, RegStorage src);
    void ApplyLoadStoreElimination(LIR* head_lir, LIR* tail_lir);
    void ApplyLoadHoisting(LIR* head_lir, LIR* tail_lir);
    virtual void ApplyLocalOptimizations(LIR* head_lir, LIR* tail_lir);
    virtual void ApplyArchOptimizations(LIR* head_lir, LIR* tail_lir, BasicBlock* bb) { return; };

    // Shared by all targets - implemented in ralloc_util.cc
    int GetSRegHi(int lowSreg);
    bool LiveOut(int s_reg);
    void SimpleRegAlloc();
    void Cleanup();
    void ResetRegPool();
    void CompilerInitPool(RegisterInfo* info, RegStorage* regs, int num);
    void DumpRegPool(GrowableArray<RegisterInfo*>* regs);
    void DumpCoreRegPool();
    void DumpFpRegPool();
    void DumpRegPools();
    /* Mark a temp register as dead.  Does not affect allocation state. */
    void Clobber(RegStorage reg);
    void ClobberSReg(int s_reg);
    void ClobberAliases(RegisterInfo* info, uint32_t clobber_mask);
    int SRegToPMap(int s_reg);
    void RecordCorePromotion(RegStorage reg, int s_reg);
    RegStorage AllocPreservedCoreReg(int s_reg);
    void RecordFpPromotion(RegStorage reg, int s_reg);
    RegStorage AllocPreservedFpReg(int s_reg);
    virtual RegStorage AllocPreservedSingle(int s_reg);
    virtual RegStorage AllocPreservedDouble(int s_reg);
    RegStorage AllocTempBody(GrowableArray<RegisterInfo*> &regs, int* next_temp, bool required);
    virtual RegStorage AllocTemp(bool required = true);
    virtual RegStorage AllocTempWide(bool required = true);
    virtual RegStorage AllocTempRef(bool required = true);
    virtual RegStorage AllocTempSingle(bool required = true);
    virtual RegStorage AllocTempDouble(bool required = true);
    virtual RegStorage AllocTypedTemp(bool fp_hint, int reg_class, bool required = true);
    virtual RegStorage AllocTypedTempWide(bool fp_hint, int reg_class, bool required = true);
    void FlushReg(RegStorage reg);
    void FlushRegWide(RegStorage reg);
    RegStorage AllocLiveReg(int s_reg, int reg_class, bool wide);
    RegStorage FindLiveReg(GrowableArray<RegisterInfo*> &regs, int s_reg);
    virtual void FreeTemp(RegStorage reg);
    virtual void FreeRegLocTemps(RegLocation rl_keep, RegLocation rl_free);
    virtual bool IsLive(RegStorage reg);
    virtual bool IsTemp(RegStorage reg);
    bool IsPromoted(RegStorage reg);
    bool IsDirty(RegStorage reg);
    virtual void LockTemp(RegStorage reg);
    void ResetDef(RegStorage reg);
    void NullifyRange(RegStorage reg, int s_reg);
    void MarkDef(RegLocation rl, LIR *start, LIR *finish);
    void MarkDefWide(RegLocation rl, LIR *start, LIR *finish);
    void ResetDefLoc(RegLocation rl);
    void ResetDefLocWide(RegLocation rl);
    void ResetDefTracking();
    void ClobberAllTemps();
    void FlushSpecificReg(RegisterInfo* info);
    void FlushAllRegs();
    bool RegClassMatches(int reg_class, RegStorage reg);
    void MarkLive(RegLocation loc);
    void MarkTemp(RegStorage reg);
    void UnmarkTemp(RegStorage reg);
    void MarkWide(RegStorage reg);
    void MarkNarrow(RegStorage reg);
    void MarkClean(RegLocation loc);
    void MarkDirty(RegLocation loc);
    void MarkInUse(RegStorage reg);
    void MarkFree(RegStorage reg);
    void MarkDead(RegStorage reg);
    bool CheckCorePoolSanity();
    virtual RegLocation UpdateLoc(RegLocation loc);
    virtual RegLocation UpdateLocWide(RegLocation loc);
    RegLocation UpdateRawLoc(RegLocation loc);

    /**
     * @brief Used to prepare a register location to receive a wide value.
     * @see EvalLoc
     * @param loc the location where the value will be stored.
     * @param reg_class Type of register needed.
     * @param update Whether the liveness information should be updated.
     * @return Returns the properly typed temporary in physical register pairs.
     */
    virtual RegLocation EvalLocWide(RegLocation loc, int reg_class, bool update);

    /**
     * @brief Used to prepare a register location to receive a value.
     * @param loc the location where the value will be stored.
     * @param reg_class Type of register needed.
     * @param update Whether the liveness information should be updated.
     * @return Returns the properly typed temporary in physical register.
     */
    virtual RegLocation EvalLoc(RegLocation loc, int reg_class, bool update);

    void CountRefs(RefCounts* core_counts, RefCounts* fp_counts, size_t num_regs);
    void DumpCounts(const RefCounts* arr, int size, const char* msg);
    void DoPromotion();
    int VRegOffset(int v_reg);
    int SRegOffset(int s_reg);
    RegLocation GetReturnWide(RegisterClass reg_class);
    RegLocation GetReturn(RegisterClass reg_class);
    RegisterInfo* GetRegInfo(RegStorage reg);

    // Shared by all targets - implemented in gen_common.cc.
    void AddIntrinsicSlowPath(CallInfo* info, LIR* branch, LIR* resume = nullptr);
    virtual bool HandleEasyDivRem(Instruction::Code dalvik_opcode, bool is_div,
                                  RegLocation rl_src, RegLocation rl_dest, int lit);
    bool HandleEasyMultiply(RegLocation rl_src, RegLocation rl_dest, int lit);
    virtual void HandleSlowPaths();
    void GenBarrier();
    void GenDivZeroException();
    // c_code holds condition code that's generated from testing divisor against 0.
    void GenDivZeroCheck(ConditionCode c_code);
    // reg holds divisor.
    void GenDivZeroCheck(RegStorage reg);
    void GenArrayBoundsCheck(RegStorage index, RegStorage length);
    void GenArrayBoundsCheck(int32_t index, RegStorage length);
    LIR* GenNullCheck(RegStorage reg);
    void MarkPossibleNullPointerException(int opt_flags);
    void MarkPossibleNullPointerExceptionAfter(int opt_flags, LIR* after);
    void MarkPossibleStackOverflowException();
    void ForceImplicitNullCheck(RegStorage reg, int opt_flags);
    LIR* GenNullCheck(RegStorage m_reg, int opt_flags);
    LIR* GenExplicitNullCheck(RegStorage m_reg, int opt_flags);
    virtual void GenImplicitNullCheck(RegStorage reg, int opt_flags);
    void GenCompareAndBranch(Instruction::Code opcode, RegLocation rl_src1,
                             RegLocation rl_src2, LIR* taken, LIR* fall_through);
    void GenCompareZeroAndBranch(Instruction::Code opcode, RegLocation rl_src,
                                 LIR* taken, LIR* fall_through);
    virtual void GenIntToLong(RegLocation rl_dest, RegLocation rl_src);
    void GenIntNarrowing(Instruction::Code opcode, RegLocation rl_dest,
                         RegLocation rl_src);
    void GenNewArray(uint32_t type_idx, RegLocation rl_dest,
                     RegLocation rl_src);
    void GenFilledNewArray(CallInfo* info);
    void GenSput(MIR* mir, RegLocation rl_src,
                 bool is_long_or_double, bool is_object);
    void GenSget(MIR* mir, RegLocation rl_dest,
                 bool is_long_or_double, bool is_object);
    void GenIGet(MIR* mir, int opt_flags, OpSize size,
                 RegLocation rl_dest, RegLocation rl_obj, bool is_long_or_double, bool is_object);
    void GenIPut(MIR* mir, int opt_flags, OpSize size,
                 RegLocation rl_src, RegLocation rl_obj, bool is_long_or_double, bool is_object);
    void GenArrayObjPut(int opt_flags, RegLocation rl_array, RegLocation rl_index,
                        RegLocation rl_src);

    void GenConstClass(uint32_t type_idx, RegLocation rl_dest);
    void GenConstString(uint32_t string_idx, RegLocation rl_dest);
    void GenNewInstance(uint32_t type_idx, RegLocation rl_dest);
    void GenThrow(RegLocation rl_src);
    void GenInstanceof(uint32_t type_idx, RegLocation rl_dest, RegLocation rl_src);
    void GenCheckCast(uint32_t insn_idx, uint32_t type_idx, RegLocation rl_src);
    void GenLong3Addr(OpKind first_op, OpKind second_op, RegLocation rl_dest,
                      RegLocation rl_src1, RegLocation rl_src2);
    virtual void GenShiftOpLong(Instruction::Code opcode, RegLocation rl_dest,
                        RegLocation rl_src1, RegLocation rl_shift);
    void GenArithOpIntLit(Instruction::Code opcode, RegLocation rl_dest,
                          RegLocation rl_src, int lit);
    virtual void GenArithOpLong(Instruction::Code opcode, RegLocation rl_dest,
                                RegLocation rl_src1, RegLocation rl_src2);
    void GenConversionCall(QuickEntrypointEnum trampoline, RegLocation rl_dest, RegLocation rl_src);
    virtual void GenSuspendTest(int opt_flags);
    virtual void GenSuspendTestAndBranch(int opt_flags, LIR* target);

    // This will be overridden by x86 implementation.
    virtual void GenConstWide(RegLocation rl_dest, int64_t value);
    virtual void GenArithOpInt(Instruction::Code opcode, RegLocation rl_dest,
                       RegLocation rl_src1, RegLocation rl_src2, int opt_flags);

    // Shared by all targets - implemented in gen_invoke.cc.
    LIR* CallHelper(RegStorage r_tgt, QuickEntrypointEnum trampoline, bool safepoint_pc,
                    bool use_link = true);
    RegStorage CallHelperSetup(QuickEntrypointEnum trampoline);

    void CallRuntimeHelper(QuickEntrypointEnum trampoline, bool safepoint_pc);
    void CallRuntimeHelperImm(QuickEntrypointEnum trampoline, int arg0, bool safepoint_pc);
    void CallRuntimeHelperReg(QuickEntrypointEnum trampoline, RegStorage arg0, bool safepoint_pc);
    void CallRuntimeHelperRegLocation(QuickEntrypointEnum trampoline, RegLocation arg0,
                                      bool safepoint_pc);
    void CallRuntimeHelperImmImm(QuickEntrypointEnum trampoline, int arg0, int arg1,
                                 bool safepoint_pc);
    void CallRuntimeHelperImmRegLocation(QuickEntrypointEnum trampoline, int arg0, RegLocation arg1,
                                         bool safepoint_pc);
    void CallRuntimeHelperRegLocationImm(QuickEntrypointEnum trampoline, RegLocation arg0, int arg1,
                                         bool safepoint_pc);
    void CallRuntimeHelperImmReg(QuickEntrypointEnum trampoline, int arg0, RegStorage arg1,
                                 bool safepoint_pc);
    void CallRuntimeHelperRegImm(QuickEntrypointEnum trampoline, RegStorage arg0, int arg1,
                                 bool safepoint_pc);
    void CallRuntimeHelperImmMethod(QuickEntrypointEnum trampoline, int arg0, bool safepoint_pc);
    void CallRuntimeHelperRegMethod(QuickEntrypointEnum trampoline, RegStorage arg0,
                                    bool safepoint_pc);
    void CallRuntimeHelperRegMethodRegLocation(QuickEntrypointEnum trampoline, RegStorage arg0,
                                               RegLocation arg2, bool safepoint_pc);
    void CallRuntimeHelperRegLocationRegLocation(QuickEntrypointEnum trampoline, RegLocation arg0,
                                                 RegLocation arg1, bool safepoint_pc);
    void CallRuntimeHelperRegReg(QuickEntrypointEnum trampoline, RegStorage arg0, RegStorage arg1,
                                 bool safepoint_pc);
    void CallRuntimeHelperRegRegImm(QuickEntrypointEnum trampoline, RegStorage arg0,
                                    RegStorage arg1, int arg2, bool safepoint_pc);
    void CallRuntimeHelperImmMethodRegLocation(QuickEntrypointEnum trampoline, int arg0,
                                               RegLocation arg2, bool safepoint_pc);
    void CallRuntimeHelperImmMethodImm(QuickEntrypointEnum trampoline, int arg0, int arg2,
                                       bool safepoint_pc);
    void CallRuntimeHelperImmRegLocationRegLocation(QuickEntrypointEnum trampoline, int arg0,
                                                    RegLocation arg1, RegLocation arg2,
                                                    bool safepoint_pc);
    void CallRuntimeHelperRegLocationRegLocationRegLocation(QuickEntrypointEnum trampoline,
                                                            RegLocation arg0, RegLocation arg1,
                                                            RegLocation arg2,
                                                            bool safepoint_pc);
    void GenInvoke(CallInfo* info);
    void GenInvokeNoInline(CallInfo* info);
    virtual void FlushIns(RegLocation* ArgLocs, RegLocation rl_method);
    virtual int GenDalvikArgsNoRange(CallInfo* info, int call_state, LIR** pcrLabel,
                             NextCallInsn next_call_insn,
                             const MethodReference& target_method,
                             uint32_t vtable_idx,
                             uintptr_t direct_code, uintptr_t direct_method, InvokeType type,
                             bool skip_this);
    virtual int GenDalvikArgsRange(CallInfo* info, int call_state, LIR** pcrLabel,
                           NextCallInsn next_call_insn,
                           const MethodReference& target_method,
                           uint32_t vtable_idx,
                           uintptr_t direct_code, uintptr_t direct_method, InvokeType type,
                           bool skip_this);

    /**
     * @brief Used to determine the register location of destination.
     * @details This is needed during generation of inline intrinsics because it finds destination
     *  of return,
     * either the physical register or the target of move-result.
     * @param info Information about the invoke.
     * @return Returns the destination location.
     */
    RegLocation InlineTarget(CallInfo* info);

    /**
     * @brief Used to determine the wide register location of destination.
     * @see InlineTarget
     * @param info Information about the invoke.
     * @return Returns the destination location.
     */
    RegLocation InlineTargetWide(CallInfo* info);

    bool GenInlinedReferenceGetReferent(CallInfo* info);
    virtual bool GenInlinedCharAt(CallInfo* info);
    bool GenInlinedStringIsEmptyOrLength(CallInfo* info, bool is_empty);
    virtual bool GenInlinedReverseBits(CallInfo* info, OpSize size);
    bool GenInlinedReverseBytes(CallInfo* info, OpSize size);
    bool GenInlinedAbsInt(CallInfo* info);
    virtual bool GenInlinedAbsLong(CallInfo* info);
    virtual bool GenInlinedAbsFloat(CallInfo* info) = 0;
    virtual bool GenInlinedAbsDouble(CallInfo* info) = 0;
    bool GenInlinedFloatCvt(CallInfo* info);
    bool GenInlinedDoubleCvt(CallInfo* info);
    virtual bool GenInlinedCeil(CallInfo* info);
    virtual bool GenInlinedFloor(CallInfo* info);
    virtual bool GenInlinedRint(CallInfo* info);
    virtual bool GenInlinedRound(CallInfo* info, bool is_double);
    virtual bool GenInlinedArrayCopyCharArray(CallInfo* info);
    virtual bool GenInlinedIndexOf(CallInfo* info, bool zero_based);
    bool GenInlinedStringCompareTo(CallInfo* info);
    virtual bool GenInlinedCurrentThread(CallInfo* info);
    bool GenInlinedUnsafeGet(CallInfo* info, bool is_long, bool is_volatile);
    bool GenInlinedUnsafePut(CallInfo* info, bool is_long, bool is_object,
                             bool is_volatile, bool is_ordered);

    virtual int LoadArgRegs(CallInfo* info, int call_state,
                    NextCallInsn next_call_insn,
                    const MethodReference& target_method,
                    uint32_t vtable_idx,
                    uintptr_t direct_code, uintptr_t direct_method, InvokeType type,
                    bool skip_this);

    // Shared by all targets - implemented in gen_loadstore.cc.
    RegLocation LoadCurrMethod();
    void LoadCurrMethodDirect(RegStorage r_tgt);
    virtual LIR* LoadConstant(RegStorage r_dest, int value);
    // Natural word size.
    virtual LIR* LoadWordDisp(RegStorage r_base, int displacement, RegStorage r_dest) {
      return LoadBaseDisp(r_base, displacement, r_dest, kWord, kNotVolatile);
    }
    // Load 32 bits, regardless of target.
    virtual LIR* Load32Disp(RegStorage r_base, int displacement, RegStorage r_dest)  {
      return LoadBaseDisp(r_base, displacement, r_dest, k32, kNotVolatile);
    }
    // Load a reference at base + displacement and decompress into register.
    virtual LIR* LoadRefDisp(RegStorage r_base, int displacement, RegStorage r_dest,
                             VolatileKind is_volatile) {
      return LoadBaseDisp(r_base, displacement, r_dest, kReference, is_volatile);
    }
    // Load a reference at base + index and decompress into register.
    virtual LIR* LoadRefIndexed(RegStorage r_base, RegStorage r_index, RegStorage r_dest,
                                int scale) {
      return LoadBaseIndexed(r_base, r_index, r_dest, scale, kReference);
    }
    // Load Dalvik value with 32-bit memory storage.  If compressed object reference, decompress.
    virtual RegLocation LoadValue(RegLocation rl_src, RegisterClass op_kind);
    // Same as above, but derive the target register class from the location record.
    virtual RegLocation LoadValue(RegLocation rl_src);
    // Load Dalvik value with 64-bit memory storage.
    virtual RegLocation LoadValueWide(RegLocation rl_src, RegisterClass op_kind);
    // Load Dalvik value with 32-bit memory storage.  If compressed object reference, decompress.
    virtual void LoadValueDirect(RegLocation rl_src, RegStorage r_dest);
    // Load Dalvik value with 32-bit memory storage.  If compressed object reference, decompress.
    virtual void LoadValueDirectFixed(RegLocation rl_src, RegStorage r_dest);
    // Load Dalvik value with 64-bit memory storage.
    virtual void LoadValueDirectWide(RegLocation rl_src, RegStorage r_dest);
    // Load Dalvik value with 64-bit memory storage.
    virtual void LoadValueDirectWideFixed(RegLocation rl_src, RegStorage r_dest);
    // Store an item of natural word size.
    virtual LIR* StoreWordDisp(RegStorage r_base, int displacement, RegStorage r_src) {
      return StoreBaseDisp(r_base, displacement, r_src, kWord, kNotVolatile);
    }
    // Store an uncompressed reference into a compressed 32-bit container.
    virtual LIR* StoreRefDisp(RegStorage r_base, int displacement, RegStorage r_src,
                              VolatileKind is_volatile) {
      return StoreBaseDisp(r_base, displacement, r_src, kReference, is_volatile);
    }
    // Store an uncompressed reference into a compressed 32-bit container by index.
    virtual LIR* StoreRefIndexed(RegStorage r_base, RegStorage r_index, RegStorage r_src,
                                 int scale) {
      return StoreBaseIndexed(r_base, r_index, r_src, scale, kReference);
    }
    // Store 32 bits, regardless of target.
    virtual LIR* Store32Disp(RegStorage r_base, int displacement, RegStorage r_src) {
      return StoreBaseDisp(r_base, displacement, r_src, k32, kNotVolatile);
    }

    /**
     * @brief Used to do the final store in the destination as per bytecode semantics.
     * @param rl_dest The destination dalvik register location.
     * @param rl_src The source register location. Can be either physical register or dalvik register.
     */
    virtual void StoreValue(RegLocation rl_dest, RegLocation rl_src);

    /**
     * @brief Used to do the final store in a wide destination as per bytecode semantics.
     * @see StoreValue
     * @param rl_dest The destination dalvik register location.
     * @param rl_src The source register location. Can be either physical register or dalvik
     *  register.
     */
    virtual void StoreValueWide(RegLocation rl_dest, RegLocation rl_src);

    /**
     * @brief Used to do the final store to a destination as per bytecode semantics.
     * @see StoreValue
     * @param rl_dest The destination dalvik register location.
     * @param rl_src The source register location. It must be kLocPhysReg
     *
     * This is used for x86 two operand computations, where we have computed the correct
     * register value that now needs to be properly registered.  This is used to avoid an
     * extra register copy that would result if StoreValue was called.
     */
    virtual void StoreFinalValue(RegLocation rl_dest, RegLocation rl_src);

    /**
     * @brief Used to do the final store in a wide destination as per bytecode semantics.
     * @see StoreValueWide
     * @param rl_dest The destination dalvik register location.
     * @param rl_src The source register location. It must be kLocPhysReg
     *
     * This is used for x86 two operand computations, where we have computed the correct
     * register values that now need to be properly registered.  This is used to avoid an
     * extra pair of register copies that would result if StoreValueWide was called.
     */
    virtual void StoreFinalValueWide(RegLocation rl_dest, RegLocation rl_src);

    // Shared by all targets - implemented in mir_to_lir.cc.
    void CompileDalvikInstruction(MIR* mir, BasicBlock* bb, LIR* label_list);
    virtual void HandleExtendedMethodMIR(BasicBlock* bb, MIR* mir);
    bool MethodBlockCodeGen(BasicBlock* bb);
    bool SpecialMIR2LIR(const InlineMethod& special);
    virtual void MethodMIR2LIR();
    // Update LIR for verbose listings.
    void UpdateLIROffsets();

    /*
     * @brief Load the address of the dex method into the register.
     * @param target_method The MethodReference of the method to be invoked.
     * @param type How the method will be invoked.
     * @param register that will contain the code address.
     * @note register will be passed to TargetReg to get physical register.
     */
    void LoadCodeAddress(const MethodReference& target_method, InvokeType type,
                         SpecialTargetRegister symbolic_reg);

    /*
     * @brief Load the Method* of a dex method into the register.
     * @param target_method The MethodReference of the method to be invoked.
     * @param type How the method will be invoked.
     * @param register that will contain the code address.
     * @note register will be passed to TargetReg to get physical register.
     */
    virtual void LoadMethodAddress(const MethodReference& target_method, InvokeType type,
                                   SpecialTargetRegister symbolic_reg);

    /*
     * @brief Load the Class* of a Dex Class type into the register.
     * @param type How the method will be invoked.
     * @param register that will contain the code address.
     * @note register will be passed to TargetReg to get physical register.
     */
    virtual void LoadClassType(uint32_t type_idx, SpecialTargetRegister symbolic_reg);

    // Routines that work for the generic case, but may be overriden by target.
    /*
     * @brief Compare memory to immediate, and branch if condition true.
     * @param cond The condition code that when true will branch to the target.
     * @param temp_reg A temporary register that can be used if compare to memory is not
     * supported by the architecture.
     * @param base_reg The register holding the base address.
     * @param offset The offset from the base.
     * @param check_value The immediate to compare to.
     * @param target branch target (or nullptr)
     * @param compare output for getting LIR for comparison (or nullptr)
     * @returns The branch instruction that was generated.
     */
    virtual LIR* OpCmpMemImmBranch(ConditionCode cond, RegStorage temp_reg, RegStorage base_reg,
                                   int offset, int check_value, LIR* target, LIR** compare);

    // Required for target - codegen helpers.
    virtual bool SmallLiteralDivRem(Instruction::Code dalvik_opcode, bool is_div,
                                    RegLocation rl_src, RegLocation rl_dest, int lit) = 0;
    virtual bool EasyMultiply(RegLocation rl_src, RegLocation rl_dest, int lit) = 0;
    virtual LIR* CheckSuspendUsingLoad() = 0;

    virtual RegStorage LoadHelper(QuickEntrypointEnum trampoline) = 0;

    virtual LIR* LoadBaseDisp(RegStorage r_base, int displacement, RegStorage r_dest,
                              OpSize size, VolatileKind is_volatile) = 0;
    virtual LIR* LoadBaseIndexed(RegStorage r_base, RegStorage r_index, RegStorage r_dest,
                                 int scale, OpSize size) = 0;
    virtual LIR* LoadConstantNoClobber(RegStorage r_dest, int value) = 0;
    virtual LIR* LoadConstantWide(RegStorage r_dest, int64_t value) = 0;
    virtual LIR* StoreBaseDisp(RegStorage r_base, int displacement, RegStorage r_src,
                               OpSize size, VolatileKind is_volatile) = 0;
    virtual LIR* StoreBaseIndexed(RegStorage r_base, RegStorage r_index, RegStorage r_src,
                                  int scale, OpSize size) = 0;
    virtual void MarkGCCard(RegStorage val_reg, RegStorage tgt_addr_reg) = 0;

    // Required for target - register utilities.

    bool IsSameReg(RegStorage reg1, RegStorage reg2) {
      RegisterInfo* info1 = GetRegInfo(reg1);
      RegisterInfo* info2 = GetRegInfo(reg2);
      return (info1->Master() == info2->Master() &&
             (info1->StorageMask() & info2->StorageMask()) != 0);
    }

    /**
     * @brief Portable way of getting special registers from the backend.
     * @param reg Enumeration describing the purpose of the register.
     * @return Return the #RegStorage corresponding to the given purpose @p reg.
     * @note This function is currently allowed to return any suitable view of the registers
     *   (e.g. this could be 64-bit solo or 32-bit solo for 64-bit backends).
     */
    virtual RegStorage TargetReg(SpecialTargetRegister reg) = 0;

    /**
     * @brief Portable way of getting special registers from the backend.
     * @param reg Enumeration describing the purpose of the register.
     * @param wide_kind What kind of view of the special register is required.
     * @return Return the #RegStorage corresponding to the given purpose @p reg.
     *
     * @note For 32b system, wide (kWide) views only make sense for the argument registers and the
     *       return. In that case, this function should return a pair where the first component of
     *       the result will be the indicated special register.
     */
    virtual RegStorage TargetReg(SpecialTargetRegister reg, WideKind wide_kind) {
      if (wide_kind == kWide) {
        DCHECK((kArg0 <= reg && reg < kArg7) || (kFArg0 <= reg && reg < kFArg7) || (kRet0 == reg));
        COMPILE_ASSERT((kArg1 == kArg0 + 1) && (kArg2 == kArg1 + 1) && (kArg3 == kArg2 + 1) &&
                       (kArg4 == kArg3 + 1) && (kArg5 == kArg4 + 1) && (kArg6 == kArg5 + 1) &&
                       (kArg7 == kArg6 + 1), kargs_range_unexpected);
        COMPILE_ASSERT((kFArg1 == kFArg0 + 1) && (kFArg2 == kFArg1 + 1) && (kFArg3 == kFArg2 + 1) &&
                       (kFArg4 == kFArg3 + 1) && (kFArg5 == kFArg4 + 1) && (kFArg6 == kFArg5 + 1) &&
                       (kFArg7 == kFArg6 + 1), kfargs_range_unexpected);
        COMPILE_ASSERT(kRet1 == kRet0 + 1, kret_range_unexpected);
        return RegStorage::MakeRegPair(TargetReg(reg),
                                       TargetReg(static_cast<SpecialTargetRegister>(reg + 1)));
      } else {
        return TargetReg(reg);
      }
    }

    /**
     * @brief Portable way of getting a special register for storing a pointer.
     * @see TargetReg()
     */
    virtual RegStorage TargetPtrReg(SpecialTargetRegister reg) {
      return TargetReg(reg);
    }

    // Get a reg storage corresponding to the wide & ref flags of the reg location.
    virtual RegStorage TargetReg(SpecialTargetRegister reg, RegLocation loc) {
      if (loc.ref) {
        return TargetReg(reg, kRef);
      } else {
        return TargetReg(reg, loc.wide ? kWide : kNotWide);
      }
    }

    virtual RegStorage GetArgMappingToPhysicalReg(int arg_num) = 0;
    virtual RegLocation GetReturnAlt() = 0;
    virtual RegLocation GetReturnWideAlt() = 0;
    virtual RegLocation LocCReturn() = 0;
    virtual RegLocation LocCReturnRef() = 0;
    virtual RegLocation LocCReturnDouble() = 0;
    virtual RegLocation LocCReturnFloat() = 0;
    virtual RegLocation LocCReturnWide() = 0;
    virtual ResourceMask GetRegMaskCommon(const RegStorage& reg) const = 0;
    virtual void AdjustSpillMask() = 0;
    virtual void ClobberCallerSave() = 0;
    virtual void FreeCallTemps() = 0;
    virtual void LockCallTemps() = 0;
    virtual void CompilerInitializeRegAlloc() = 0;

    // Required for target - miscellaneous.
    virtual void AssembleLIR() = 0;
    virtual void DumpResourceMask(LIR* lir, const ResourceMask& mask, const char* prefix) = 0;
    virtual void SetupTargetResourceMasks(LIR* lir, uint64_t flags,
                                          ResourceMask* use_mask, ResourceMask* def_mask) = 0;
    virtual const char* GetTargetInstFmt(int opcode) = 0;
    virtual const char* GetTargetInstName(int opcode) = 0;
    virtual std::string BuildInsnString(const char* fmt, LIR* lir, unsigned char* base_addr) = 0;

    // Note: This may return kEncodeNone on architectures that do not expose a PC. The caller must
    //       take care of this.
    virtual ResourceMask GetPCUseDefEncoding() const = 0;
    virtual uint64_t GetTargetInstFlags(int opcode) = 0;
    virtual size_t GetInsnSize(LIR* lir) = 0;
    virtual bool IsUnconditionalBranch(LIR* lir) = 0;

    // Get the register class for load/store of a field.
    virtual RegisterClass RegClassForFieldLoadStore(OpSize size, bool is_volatile) = 0;

    // Required for target - Dalvik-level generators.
    virtual void GenArithImmOpLong(Instruction::Code opcode, RegLocation rl_dest,
                                   RegLocation rl_src1, RegLocation rl_src2) = 0;
    virtual void GenArithOpDouble(Instruction::Code opcode,
                                  RegLocation rl_dest, RegLocation rl_src1,
                                  RegLocation rl_src2) = 0;
    virtual void GenArithOpFloat(Instruction::Code opcode, RegLocation rl_dest,
                                 RegLocation rl_src1, RegLocation rl_src2) = 0;
    virtual void GenCmpFP(Instruction::Code opcode, RegLocation rl_dest,
                          RegLocation rl_src1, RegLocation rl_src2) = 0;
    virtual void GenConversion(Instruction::Code opcode, RegLocation rl_dest,
                               RegLocation rl_src) = 0;
    virtual bool GenInlinedCas(CallInfo* info, bool is_long, bool is_object) = 0;

    /**
     * @brief Used to generate code for intrinsic java\.lang\.Math methods min and max.
     * @details This is also applicable for java\.lang\.StrictMath since it is a simple algorithm
     * that applies on integers. The generated code will write the smallest or largest value
     * directly into the destination register as specified by the invoke information.
     * @param info Information about the invoke.
     * @param is_min If true generates code that computes minimum. Otherwise computes maximum.
     * @param is_long If true the value value is Long. Otherwise the value is Int.
     * @return Returns true if successfully generated
     */
    virtual bool GenInlinedMinMax(CallInfo* info, bool is_min, bool is_long) = 0;
    virtual bool GenInlinedMinMaxFP(CallInfo* info, bool is_min, bool is_double);

    virtual bool GenInlinedSqrt(CallInfo* info) = 0;
    virtual bool GenInlinedPeek(CallInfo* info, OpSize size) = 0;
    virtual bool GenInlinedPoke(CallInfo* info, OpSize size) = 0;
    virtual RegLocation GenDivRem(RegLocation rl_dest, RegStorage reg_lo, RegStorage reg_hi,
                                  bool is_div) = 0;
    virtual RegLocation GenDivRemLit(RegLocation rl_dest, RegStorage reg_lo, int lit,
                                     bool is_div) = 0;
    /*
     * @brief Generate an integer div or rem operation by a literal.
     * @param rl_dest Destination Location.
     * @param rl_src1 Numerator Location.
     * @param rl_src2 Divisor Location.
     * @param is_div 'true' if this is a division, 'false' for a remainder.
     * @param check_zero 'true' if an exception should be generated if the divisor is 0.
     */
    virtual RegLocation GenDivRem(RegLocation rl_dest, RegLocation rl_src1,
                                  RegLocation rl_src2, bool is_div, bool check_zero) = 0;
    /*
     * @brief Generate an integer div or rem operation by a literal.
     * @param rl_dest Destination Location.
     * @param rl_src Numerator Location.
     * @param lit Divisor.
     * @param is_div 'true' if this is a division, 'false' for a remainder.
     */
    virtual RegLocation GenDivRemLit(RegLocation rl_dest, RegLocation rl_src1, int lit,
                                     bool is_div) = 0;
    virtual void GenCmpLong(RegLocation rl_dest, RegLocation rl_src1, RegLocation rl_src2) = 0;

    /**
     * @brief Used for generating code that throws ArithmeticException if both registers are zero.
     * @details This is used for generating DivideByZero checks when divisor is held in two
     *  separate registers.
     * @param reg The register holding the pair of 32-bit values.
     */
    virtual void GenDivZeroCheckWide(RegStorage reg) = 0;

    virtual void GenEntrySequence(RegLocation* ArgLocs, RegLocation rl_method) = 0;
    virtual void GenExitSequence() = 0;
    virtual void GenFillArrayData(DexOffset table_offset, RegLocation rl_src) = 0;
    virtual void GenFusedFPCmpBranch(BasicBlock* bb, MIR* mir, bool gt_bias, bool is_double) = 0;
    virtual void GenFusedLongCmpBranch(BasicBlock* bb, MIR* mir) = 0;

    /*
     * @brief Handle Machine Specific MIR Extended opcodes.
     * @param bb The basic block in which the MIR is from.
     * @param mir The MIR whose opcode is not standard extended MIR.
     * @note Base class implementation will abort for unknown opcodes.
     */
    virtual void GenMachineSpecificExtendedMethodMIR(BasicBlock* bb, MIR* mir);

    /* non virtual so it doesn't have to be implemented */
    virtual void MachineSpecificPreprocessMIR(BasicBlock* bb, MIR* mir) { };

    /**
     * @brief Lowers the kMirOpSelect MIR into LIR.
     * @param bb The basic block in which the MIR is from.
     * @param mir The MIR whose opcode is kMirOpSelect.
     */
    virtual void GenSelect(BasicBlock* bb, MIR* mir) = 0;

    /**
     * @brief Generates code to select one of the given constants depending on the given opcode.
     */
    virtual void GenSelectConst32(RegStorage left_op, RegStorage right_op, ConditionCode code,
                                  int32_t true_val, int32_t false_val, RegStorage rs_dest,
                                  int dest_reg_class) = 0;

    /**
     * @brief Used to generate a memory barrier in an architecture specific way.
     * @details The last generated LIR will be considered for use as barrier. Namely,
     * if the last LIR can be updated in a way where it will serve the semantics of
     * barrier, then it will be used as such. Otherwise, a new LIR will be generated
     * that can keep the semantics.
     * @param barrier_kind The kind of memory barrier to generate.
     * @return whether a new instruction was generated.
     */
    virtual bool GenMemBarrier(MemBarrierKind barrier_kind) = 0;

    virtual void GenMoveException(RegLocation rl_dest) = 0;
    virtual void GenMultiplyByTwoBitMultiplier(RegLocation rl_src, RegLocation rl_result, int lit,
                                               int first_bit, int second_bit) = 0;
    virtual void GenNegDouble(RegLocation rl_dest, RegLocation rl_src) = 0;
    virtual void GenNegFloat(RegLocation rl_dest, RegLocation rl_src) = 0;

    // Create code for switch statements. Will decide between short and long versions below.
    void GenPackedSwitch(MIR* mir, DexOffset table_offset, RegLocation rl_src);
    void GenSparseSwitch(MIR* mir, DexOffset table_offset, RegLocation rl_src);

    // Potentially backend-specific versions of switch instructions for shorter switch statements.
    // The default implementation will create a chained compare-and-branch.
    virtual void GenSmallPackedSwitch(MIR* mir, DexOffset table_offset, RegLocation rl_src);
    virtual void GenSmallSparseSwitch(MIR* mir, DexOffset table_offset, RegLocation rl_src);
    // Backend-specific versions of switch instructions for longer switch statements.
    virtual void GenLargePackedSwitch(MIR* mir, DexOffset table_offset, RegLocation rl_src) = 0;
    virtual void GenLargeSparseSwitch(MIR* mir, DexOffset table_offset, RegLocation rl_src) = 0;

    virtual void GenArrayGet(int opt_flags, OpSize size, RegLocation rl_array,
                             RegLocation rl_index, RegLocation rl_dest, int scale) = 0;
    virtual void GenArrayPut(int opt_flags, OpSize size, RegLocation rl_array,
                             RegLocation rl_index, RegLocation rl_src, int scale,
                             bool card_mark) = 0;
    virtual void GenShiftImmOpLong(Instruction::Code opcode, RegLocation rl_dest,
                                   RegLocation rl_src1, RegLocation rl_shift) = 0;

    // Required for target - single operation generators.
    virtual LIR* OpUnconditionalBranch(LIR* target) = 0;
    virtual LIR* OpCmpBranch(ConditionCode cond, RegStorage src1, RegStorage src2, LIR* target) = 0;
    virtual LIR* OpCmpImmBranch(ConditionCode cond, RegStorage reg, int check_value,
                                LIR* target) = 0;
    virtual LIR* OpCondBranch(ConditionCode cc, LIR* target) = 0;
    virtual LIR* OpDecAndBranch(ConditionCode c_code, RegStorage reg, LIR* target) = 0;
    virtual LIR* OpFpRegCopy(RegStorage r_dest, RegStorage r_src) = 0;
    virtual LIR* OpIT(ConditionCode cond, const char* guide) = 0;
    virtual void OpEndIT(LIR* it) = 0;
    virtual LIR* OpMem(OpKind op, RegStorage r_base, int disp) = 0;
    virtual LIR* OpPcRelLoad(RegStorage reg, LIR* target) = 0;
    virtual LIR* OpReg(OpKind op, RegStorage r_dest_src) = 0;
    virtual LIR* OpBkpt() { // not abstract so it doesn't have to be implemeted for other platforms
                            return NULL;
                          };
    virtual void OpRegCopy(RegStorage r_dest, RegStorage r_src) = 0;
    virtual LIR* OpRegCopyNoInsert(RegStorage r_dest, RegStorage r_src) = 0;
    virtual LIR* OpRegImm(OpKind op, RegStorage r_dest_src1, int value) = 0;
    virtual LIR* OpRegReg(OpKind op, RegStorage r_dest_src1, RegStorage r_src2) = 0;

    /**
     * @brief Used to generate an LIR that does a load from mem to reg.
     * @param r_dest The destination physical register.
     * @param r_base The base physical register for memory operand.
     * @param offset The displacement for memory operand.
     * @param move_type Specification on the move desired (size, alignment, register kind).
     * @return Returns the generate move LIR.
     */
    virtual LIR* OpMovRegMem(RegStorage r_dest, RegStorage r_base, int offset,
                             MoveType move_type) = 0;

    /**
     * @brief Used to generate an LIR that does a store from reg to mem.
     * @param r_base The base physical register for memory operand.
     * @param offset The displacement for memory operand.
     * @param r_src The destination physical register.
     * @param bytes_to_move The number of bytes to move.
     * @param is_aligned Whether the memory location is known to be aligned.
     * @return Returns the generate move LIR.
     */
    virtual LIR* OpMovMemReg(RegStorage r_base, int offset, RegStorage r_src,
                             MoveType move_type) = 0;

    /**
     * @brief Used for generating a conditional register to register operation.
     * @param op The opcode kind.
     * @param cc The condition code that when true will perform the opcode.
     * @param r_dest The destination physical register.
     * @param r_src The source physical register.
     * @return Returns the newly created LIR or null in case of creation failure.
     */
    virtual LIR* OpCondRegReg(OpKind op, ConditionCode cc, RegStorage r_dest, RegStorage r_src) = 0;

    virtual LIR* OpRegRegImm(OpKind op, RegStorage r_dest, RegStorage r_src1, int value) = 0;
    virtual LIR* OpRegRegReg(OpKind op, RegStorage r_dest, RegStorage r_src1,
                             RegStorage r_src2) = 0;
    virtual LIR* OpTestSuspend(LIR* target) = 0;
    virtual LIR* OpVldm(RegStorage r_base, int count) = 0;
    virtual LIR* OpVstm(RegStorage r_base, int count) = 0;
    virtual void OpRegCopyWide(RegStorage dest, RegStorage src) = 0;
    virtual bool InexpensiveConstantInt(int32_t value) = 0;
    virtual bool InexpensiveConstantFloat(int32_t value) = 0;
    virtual bool InexpensiveConstantLong(int64_t value) = 0;
    virtual bool InexpensiveConstantDouble(int64_t value) = 0;
    virtual bool InexpensiveConstantInt(int32_t value, Instruction::Code opcode) {
      return InexpensiveConstantInt(value);
    }

    // May be optimized by targets.
    virtual void GenMonitorEnter(int opt_flags, RegLocation rl_src);
    virtual void GenMonitorExit(int opt_flags, RegLocation rl_src);

    // Temp workaround
    void Workaround7250540(RegLocation rl_dest, RegStorage zero_reg);

    virtual LIR* InvokeTrampoline(OpKind op, RegStorage r_tgt, QuickEntrypointEnum trampoline) = 0;

  protected:
    Mir2Lir(CompilationUnit* cu, MIRGraph* mir_graph, ArenaAllocator* arena);

    CompilationUnit* GetCompilationUnit() {
      return cu_;
    }
    /*
     * @brief Returns the index of the lowest set bit in 'x'.
     * @param x Value to be examined.
     * @returns The bit number of the lowest bit set in the value.
     */
    int32_t LowestSetBit(uint64_t x);
    /*
     * @brief Is this value a power of two?
     * @param x Value to be examined.
     * @returns 'true' if only 1 bit is set in the value.
     */
    bool IsPowerOfTwo(uint64_t x);
    /*
     * @brief Do these SRs overlap?
     * @param rl_op1 One RegLocation
     * @param rl_op2 The other RegLocation
     * @return 'true' if the VR pairs overlap
     *
     * Check to see if a result pair has a misaligned overlap with an operand pair.  This
     * is not usual for dx to generate, but it is legal (for now).  In a future rev of
     * dex, we'll want to make this case illegal.
     */
    bool BadOverlap(RegLocation rl_op1, RegLocation rl_op2);

    /*
     * @brief Force a location (in a register) into a temporary register
     * @param loc location of result
     * @returns update location
     */
    virtual RegLocation ForceTemp(RegLocation loc);

    /*
     * @brief Force a wide location (in registers) into temporary registers
     * @param loc location of result
     * @returns update location
     */
    virtual RegLocation ForceTempWide(RegLocation loc);

    static constexpr OpSize LoadStoreOpSize(bool wide, bool ref) {
      return wide ? k64 : ref ? kReference : k32;
    }

    virtual void GenInstanceofFinal(bool use_declaring_class, uint32_t type_idx,
                                    RegLocation rl_dest, RegLocation rl_src);

    void AddSlowPath(LIRSlowPath* slowpath);

    /*
     *
     * @brief Implement Set up instanceof a class.
     * @param needs_access_check 'true' if we must check the access.
     * @param type_known_final 'true' if the type is known to be a final class.
     * @param type_known_abstract 'true' if the type is known to be an abstract class.
     * @param use_declaring_class 'true' if the type can be loaded off the current Method*.
     * @param can_assume_type_is_in_dex_cache 'true' if the type is known to be in the cache.
     * @param type_idx Type index to use if use_declaring_class is 'false'.
     * @param rl_dest Result to be set to 0 or 1.
     * @param rl_src Object to be tested.
     */
    void GenInstanceofCallingHelper(bool needs_access_check, bool type_known_final,
                                    bool type_known_abstract, bool use_declaring_class,
                                    bool can_assume_type_is_in_dex_cache,
                                    uint32_t type_idx, RegLocation rl_dest,
                                    RegLocation rl_src);
    /*
     * @brief Generate the debug_frame FDE information if possible.
     * @returns pointer to vector containg CFE information, or NULL.
     */
    virtual std::vector<uint8_t>* ReturnCallFrameInformation();

    /**
     * @brief Used to insert marker that can be used to associate MIR with LIR.
     * @details Only inserts marker if verbosity is enabled.
     * @param mir The mir that is currently being generated.
     */
    void GenPrintLabel(MIR* mir);

    /**
     * @brief Used to generate return sequence when there is no frame.
     * @details Assumes that the return registers have already been populated.
     */
    virtual void GenSpecialExitSequence() = 0;

    /**
     * @brief Used to generate code for special methods that are known to be
     * small enough to work in frameless mode.
     * @param bb The basic block of the first MIR.
     * @param mir The first MIR of the special method.
     * @param special Information about the special method.
     * @return Returns whether or not this was handled successfully. Returns false
     * if caller should punt to normal MIR2LIR conversion.
     */
    virtual bool GenSpecialCase(BasicBlock* bb, MIR* mir, const InlineMethod& special);

  protected:
    void ClobberBody(RegisterInfo* p);
    void SetCurrentDexPc(DexOffset dexpc) {
      current_dalvik_offset_ = dexpc;
    }

    /**
     * @brief Used to lock register if argument at in_position was passed that way.
     * @details Does nothing if the argument is passed via stack.
     * @param in_position The argument number whose register to lock.
     * @param wide Whether the argument is wide.
     */
    void LockArg(int in_position, bool wide = false);

    /**
     * @brief Used to load VR argument to a physical register.
     * @details The load is only done if the argument is not already in physical register.
     * LockArg must have been previously called.
     * @param in_position The argument number to load.
     * @param wide Whether the argument is 64-bit or not.
     * @return Returns the register (or register pair) for the loaded argument.
     */
    RegStorage LoadArg(int in_position, RegisterClass reg_class, bool wide = false);

    /**
     * @brief Used to load a VR argument directly to a specified register location.
     * @param in_position The argument number to place in register.
     * @param rl_dest The register location where to place argument.
     */
    void LoadArgDirect(int in_position, RegLocation rl_dest);

    /**
     * @brief Used to generate LIR for special getter method.
     * @param mir The mir that represents the iget.
     * @param special Information about the special getter method.
     * @return Returns whether LIR was successfully generated.
     */
    bool GenSpecialIGet(MIR* mir, const InlineMethod& special);

    /**
     * @brief Used to generate LIR for special setter method.
     * @param mir The mir that represents the iput.
     * @param special Information about the special setter method.
     * @return Returns whether LIR was successfully generated.
     */
    bool GenSpecialIPut(MIR* mir, const InlineMethod& special);

    /**
     * @brief Used to generate LIR for special return-args method.
     * @param mir The mir that represents the return of argument.
     * @param special Information about the special return-args method.
     * @return Returns whether LIR was successfully generated.
     */
    bool GenSpecialIdentity(MIR* mir, const InlineMethod& special);

    void AddDivZeroCheckSlowPath(LIR* branch);

    // Copy arg0 and arg1 to kArg0 and kArg1 safely, possibly using
    // kArg2 as temp.
    virtual void CopyToArgumentRegs(RegStorage arg0, RegStorage arg1);

    /**
     * @brief Load Constant into RegLocation
     * @param rl_dest Destination RegLocation
     * @param value Constant value
     */
    virtual void GenConst(RegLocation rl_dest, int value);

    /**
     * Returns true iff wide GPRs are just different views on the same physical register.
     */
    virtual bool WideGPRsAreAliases() = 0;

    /**
     * Returns true iff wide FPRs are just different views on the same physical register.
     */
    virtual bool WideFPRsAreAliases() = 0;


    enum class WidenessCheck {  // private
      kIgnoreWide,
      kCheckWide,
      kCheckNotWide
    };

    enum class RefCheck {  // private
      kIgnoreRef,
      kCheckRef,
      kCheckNotRef
    };

    enum class FPCheck {  // private
      kIgnoreFP,
      kCheckFP,
      kCheckNotFP
    };

    /**
     * Check whether a reg storage seems well-formed, that is, if a reg storage is valid,
     * that it has the expected form for the flags.
     * A flag value of 0 means ignore. A flag value of -1 means false. A flag value of 1 means true.
     */
    void CheckRegStorageImpl(RegStorage rs, WidenessCheck wide, RefCheck ref, FPCheck fp, bool fail,
                             bool report)
        const;

    /**
     * Check whether a reg location seems well-formed, that is, if a reg storage is encoded,
     * that it has the expected size.
     */
    void CheckRegLocationImpl(RegLocation rl, bool fail, bool report) const;

    // See CheckRegStorageImpl. Will print or fail depending on kFailOnSizeError and
    // kReportSizeError.
    void CheckRegStorage(RegStorage rs, WidenessCheck wide, RefCheck ref, FPCheck fp) const;
    // See CheckRegLocationImpl.
    void CheckRegLocation(RegLocation rl) const;

  public:
    // TODO: add accessors for these.
    LIR* literal_list_;                        // Constants.
    LIR* method_literal_list_;                 // Method literals requiring patching.
    LIR* class_literal_list_;                  // Class literals requiring patching.
    LIR* code_literal_list_;                   // Code literals requiring patching.
    LIR* first_fixup_;                         // Doubly-linked list of LIR nodes requiring fixups.

  protected:
    CompilationUnit* const cu_;
    MIRGraph* const mir_graph_;
    GrowableArray<SwitchTable*> switch_tables_;
    GrowableArray<FillArrayData*> fill_array_data_;
    GrowableArray<RegisterInfo*> tempreg_info_;
    GrowableArray<RegisterInfo*> reginfo_map_;
    GrowableArray<void*> pointer_storage_;
    CodeOffset current_code_offset_;    // Working byte offset of machine instructons.
    CodeOffset data_offset_;            // starting offset of literal pool.
    size_t total_size_;                   // header + code size.
    LIR* block_label_list_;
    PromotionMap* promotion_map_;
    /*
     * TODO: The code generation utilities don't have a built-in
     * mechanism to propagate the original Dalvik opcode address to the
     * associated generated instructions.  For the trace compiler, this wasn't
     * necessary because the interpreter handled all throws and debugging
     * requests.  For now we'll handle this by placing the Dalvik offset
     * in the CompilationUnit struct before codegen for each instruction.
     * The low-level LIR creation utilites will pull it from here.  Rework this.
     */
    DexOffset current_dalvik_offset_;
    size_t estimated_native_code_size_;     // Just an estimate; used to reserve code_buffer_ size.
    RegisterPool* reg_pool_;
    /*
     * Sanity checking for the register temp tracking.  The same ssa
     * name should never be associated with one temp register per
     * instruction compilation.
     */
    int live_sreg_;
    CodeBuffer code_buffer_;
    // The encoding mapping table data (dex -> pc offset and pc offset -> dex) with a size prefix.
    std::vector<uint8_t> encoded_mapping_table_;
    ArenaVector<uint32_t> core_vmap_table_;
    ArenaVector<uint32_t> fp_vmap_table_;
    std::vector<uint8_t> native_gc_map_;
    int num_core_spills_;
    int num_fp_spills_;
    int frame_size_;
    unsigned int core_spill_mask_;
    unsigned int fp_spill_mask_;
    LIR* first_lir_insn_;
    LIR* last_lir_insn_;

    GrowableArray<LIRSlowPath*> slow_paths_;

    // The memory reference type for new LIRs.
    // NOTE: Passing this as an explicit parameter by all functions that directly or indirectly
    // invoke RawLIR() would clutter the code and reduce the readability.
    ResourceMask::ResourceBit mem_ref_type_;

    // Each resource mask now takes 16-bytes, so having both use/def masks directly in a LIR
    // would consume 32 bytes per LIR. Instead, the LIR now holds only pointers to the masks
    // (i.e. 8 bytes on 32-bit arch, 16 bytes on 64-bit arch) and we use ResourceMaskCache
    // to deduplicate the masks.
    ResourceMaskCache mask_cache_;
};  // Class Mir2Lir

}  // namespace art

#endif  // ART_COMPILER_DEX_QUICK_MIR_TO_LIR_H_
