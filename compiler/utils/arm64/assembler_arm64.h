/*
 * Copyright (C) 2014 The Android Open Source Project
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

#ifndef ART_COMPILER_UTILS_ARM64_ASSEMBLER_ARM64_H_
#define ART_COMPILER_UTILS_ARM64_ASSEMBLER_ARM64_H_

#include <vector>
#include <stdint.h>

#include "base/logging.h"
#include "constants_arm64.h"
#include "utils/arm64/managed_register_arm64.h"
#include "utils/assembler.h"
#include "offsets.h"
#include "utils.h"
#include "UniquePtrCompat.h"
#include "a64/macro-assembler-a64.h"
#include "a64/disasm-a64.h"

namespace art {
namespace arm64 {

#define MEM_OP(x...)      vixl::MemOperand(x)
#define COND_OP(x)        static_cast<vixl::Condition>(x)

enum Condition {
  kNoCondition = -1,
  EQ = 0,
  NE = 1,
  HS = 2,
  LO = 3,
  MI = 4,
  PL = 5,
  VS = 6,
  VC = 7,
  HI = 8,
  LS = 9,
  GE = 10,
  LT = 11,
  GT = 12,
  LE = 13,
  AL = 14,    // Always.
  NV = 15,    // Behaves as always/al.
  kMaxCondition = 16,
};

enum LoadOperandType {
  kLoadSignedByte,
  kLoadUnsignedByte,
  kLoadSignedHalfword,
  kLoadUnsignedHalfword,
  kLoadWord,
  kLoadCoreWord,
  kLoadSWord,
  kLoadDWord
};

enum StoreOperandType {
  kStoreByte,
  kStoreHalfword,
  kStoreWord,
  kStoreCoreWord,
  kStoreSWord,
  kStoreDWord
};

class Arm64Exception;

class Arm64Assembler FINAL : public Assembler {
 public:
  Arm64Assembler() : vixl_buf_(new byte[kBufferSizeArm64]),
  vixl_masm_(new vixl::MacroAssembler(vixl_buf_, kBufferSizeArm64)) {}

  virtual ~Arm64Assembler() {
    delete vixl_masm_;
    delete[] vixl_buf_;
  }

  // Emit slow paths queued during assembly.
  void EmitSlowPaths();

  // Size of generated code.
  size_t CodeSize() const;

  // Copy instructions out of assembly buffer into the given region of memory.
  void FinalizeInstructions(const MemoryRegion& region);

  // Emit code that will create an activation on the stack.
  void BuildFrame(size_t frame_size, ManagedRegister method_reg,
                  const std::vector<ManagedRegister>& callee_save_regs,
                  const ManagedRegisterEntrySpills& entry_spills) OVERRIDE;

  // Emit code that will remove an activation from the stack.
  void RemoveFrame(size_t frame_size, const std::vector<ManagedRegister>& callee_save_regs)
      OVERRIDE;

  void IncreaseFrameSize(size_t adjust) OVERRIDE;
  void DecreaseFrameSize(size_t adjust) OVERRIDE;

  // Store routines.
  void Store(FrameOffset offs, ManagedRegister src, size_t size) OVERRIDE;
  void StoreRef(FrameOffset dest, ManagedRegister src) OVERRIDE;
  void StoreRawPtr(FrameOffset dest, ManagedRegister src) OVERRIDE;
  void StoreImmediateToFrame(FrameOffset dest, uint32_t imm, ManagedRegister scratch) OVERRIDE;
  void StoreImmediateToThread64(ThreadOffset<8> dest, uint32_t imm, ManagedRegister scratch)
      OVERRIDE;
  void StoreStackOffsetToThread64(ThreadOffset<8> thr_offs, FrameOffset fr_offs,
                                  ManagedRegister scratch) OVERRIDE;
  void StoreStackPointerToThread64(ThreadOffset<8> thr_offs) OVERRIDE;
  void StoreSpanning(FrameOffset dest, ManagedRegister src, FrameOffset in_off,
                     ManagedRegister scratch) OVERRIDE;

  // Load routines.
  void Load(ManagedRegister dest, FrameOffset src, size_t size) OVERRIDE;
  void LoadFromThread64(ManagedRegister dest, ThreadOffset<8> src, size_t size) OVERRIDE;
  void LoadRef(ManagedRegister dest, FrameOffset  src) OVERRIDE;
  void LoadRef(ManagedRegister dest, ManagedRegister base, MemberOffset offs) OVERRIDE;
  void LoadRawPtr(ManagedRegister dest, ManagedRegister base, Offset offs) OVERRIDE;
  void LoadRawPtrFromThread64(ManagedRegister dest, ThreadOffset<8> offs) OVERRIDE;

  // Copying routines.
  void Move(ManagedRegister dest, ManagedRegister src, size_t size) OVERRIDE;
  void CopyRawPtrFromThread64(FrameOffset fr_offs, ThreadOffset<8> thr_offs,
                              ManagedRegister scratch) OVERRIDE;
  void CopyRawPtrToThread64(ThreadOffset<8> thr_offs, FrameOffset fr_offs, ManagedRegister scratch)
      OVERRIDE;
  void CopyRef(FrameOffset dest, FrameOffset src, ManagedRegister scratch) OVERRIDE;
  void Copy(FrameOffset dest, FrameOffset src, ManagedRegister scratch, size_t size) OVERRIDE;
  void Copy(FrameOffset dest, ManagedRegister src_base, Offset src_offset, ManagedRegister scratch,
            size_t size) OVERRIDE;
  void Copy(ManagedRegister dest_base, Offset dest_offset, FrameOffset src, ManagedRegister scratch,
            size_t size) OVERRIDE;
  void Copy(FrameOffset dest, FrameOffset src_base, Offset src_offset, ManagedRegister scratch,
            size_t size) OVERRIDE;
  void Copy(ManagedRegister dest, Offset dest_offset, ManagedRegister src, Offset src_offset,
            ManagedRegister scratch, size_t size) OVERRIDE;
  void Copy(FrameOffset dest, Offset dest_offset, FrameOffset src, Offset src_offset,
            ManagedRegister scratch, size_t size) OVERRIDE;
  void MemoryBarrier(ManagedRegister scratch) OVERRIDE;

  // Sign extension.
  void SignExtend(ManagedRegister mreg, size_t size) OVERRIDE;

  // Zero extension.
  void ZeroExtend(ManagedRegister mreg, size_t size) OVERRIDE;

  // Exploit fast access in managed code to Thread::Current().
  void GetCurrentThread(ManagedRegister tr) OVERRIDE;
  void GetCurrentThread(FrameOffset dest_offset, ManagedRegister scratch) OVERRIDE;

  // Set up out_reg to hold a Object** into the handle scope, or to be NULL if the
  // value is null and null_allowed. in_reg holds a possibly stale reference
  // that can be used to avoid loading the handle scope entry to see if the value is
  // NULL.
  void CreateHandleScopeEntry(ManagedRegister out_reg, FrameOffset handlescope_offset,
                       ManagedRegister in_reg, bool null_allowed) OVERRIDE;

  // Set up out_off to hold a Object** into the handle scope, or to be NULL if the
  // value is null and null_allowed.
  void CreateHandleScopeEntry(FrameOffset out_off, FrameOffset handlescope_offset,
                       ManagedRegister scratch, bool null_allowed) OVERRIDE;

  // src holds a handle scope entry (Object**) load this into dst.
  void LoadReferenceFromHandleScope(ManagedRegister dst, ManagedRegister src) OVERRIDE;

  // Heap::VerifyObject on src. In some cases (such as a reference to this) we
  // know that src may not be null.
  void VerifyObject(ManagedRegister src, bool could_be_null) OVERRIDE;
  void VerifyObject(FrameOffset src, bool could_be_null) OVERRIDE;

  // Call to address held at [base+offset].
  void Call(ManagedRegister base, Offset offset, ManagedRegister scratch) OVERRIDE;
  void Call(FrameOffset base, Offset offset, ManagedRegister scratch) OVERRIDE;
  void CallFromThread64(ThreadOffset<8> offset, ManagedRegister scratch) OVERRIDE;

  // Jump to address (not setting link register)
  void JumpTo(ManagedRegister m_base, Offset offs, ManagedRegister m_scratch);

  // Generate code to check if Thread::Current()->exception_ is non-null
  // and branch to a ExceptionSlowPath if it is.
  void ExceptionPoll(ManagedRegister scratch, size_t stack_adjust) OVERRIDE;

 private:
  static vixl::Register reg_x(int code) {
    CHECK(code < kNumberOfCoreRegisters) << code;
    if (code == SP) {
      return vixl::sp;
    } else if (code == XZR) {
      return vixl::xzr;
    }
    return vixl::Register::XRegFromCode(code);
  }

  static vixl::Register reg_w(int code) {
    return vixl::Register::WRegFromCode(code);
  }

  static vixl::FPRegister reg_d(int code) {
    return vixl::FPRegister::DRegFromCode(code);
  }

  static vixl::FPRegister reg_s(int code) {
    return vixl::FPRegister::SRegFromCode(code);
  }

  // Emits Exception block.
  void EmitExceptionPoll(Arm64Exception *exception);

  void StoreWToOffset(StoreOperandType type, WRegister source,
                      Register base, int32_t offset);
  void StoreToOffset(Register source, Register base, int32_t offset);
  void StoreSToOffset(SRegister source, Register base, int32_t offset);
  void StoreDToOffset(DRegister source, Register base, int32_t offset);

  void LoadImmediate(Register dest, int32_t value, Condition cond = AL);
  void Load(Arm64ManagedRegister dst, Register src, int32_t src_offset, size_t size);
  void LoadWFromOffset(LoadOperandType type, WRegister dest,
                      Register base, int32_t offset);
  void LoadFromOffset(Register dest, Register base, int32_t offset);
  void LoadSFromOffset(SRegister dest, Register base, int32_t offset);
  void LoadDFromOffset(DRegister dest, Register base, int32_t offset);
  void AddConstant(Register rd, int32_t value, Condition cond = AL);
  void AddConstant(Register rd, Register rn, int32_t value, Condition cond = AL);

  // Vixl buffer.
  byte* vixl_buf_;

  // Vixl assembler.
  vixl::MacroAssembler* vixl_masm_;

  // List of exception blocks to generate at the end of the code cache.
  std::vector<Arm64Exception*> exception_blocks_;

  // Used for testing.
  friend class Arm64ManagedRegister_VixlRegisters_Test;
};

class Arm64Exception {
 private:
  explicit Arm64Exception(Arm64ManagedRegister scratch, size_t stack_adjust)
      : scratch_(scratch), stack_adjust_(stack_adjust) {
    }

  vixl::Label* Entry() { return &exception_entry_; }

  // Register used for passing Thread::Current()->exception_ .
  const Arm64ManagedRegister scratch_;

  // Stack adjust for ExceptionPool.
  const size_t stack_adjust_;

  vixl::Label exception_entry_;

  friend class Arm64Assembler;
  DISALLOW_COPY_AND_ASSIGN(Arm64Exception);
};

}  // namespace arm64
}  // namespace art

#endif  // ART_COMPILER_UTILS_ARM64_ASSEMBLER_ARM64_H_
