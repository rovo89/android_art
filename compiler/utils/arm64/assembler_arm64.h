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

#include <stdint.h>
#include <memory>
#include <vector>

#include "base/arena_containers.h"
#include "base/logging.h"
#include "constants_arm64.h"
#include "utils/arm64/managed_register_arm64.h"
#include "utils/assembler.h"
#include "offsets.h"

// TODO: make vixl clean wrt -Wshadow.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wmissing-noreturn"
#include "vixl/a64/macro-assembler-a64.h"
#include "vixl/a64/disasm-a64.h"
#pragma GCC diagnostic pop

namespace art {
namespace arm64 {

#define MEM_OP(...)      vixl::MemOperand(__VA_ARGS__)

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

class Arm64Exception {
 private:
  Arm64Exception(Arm64ManagedRegister scratch, size_t stack_adjust)
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

class Arm64Assembler FINAL : public Assembler {
 public:
  // We indicate the size of the initial code generation buffer to the VIXL
  // assembler. From there we it will automatically manage the buffer.
  explicit Arm64Assembler(ArenaAllocator* arena)
      : Assembler(arena),
        exception_blocks_(arena->Adapter(kArenaAllocAssembler)),
        vixl_masm_(new vixl::MacroAssembler(kArm64BaseBufferSize)) {}

  virtual ~Arm64Assembler() {
    delete vixl_masm_;
  }

  // Finalize the code.
  void FinalizeCode() OVERRIDE;

  // Size of generated code.
  size_t CodeSize() const OVERRIDE;
  const uint8_t* CodeBufferBaseAddress() const OVERRIDE;

  // Copy instructions out of assembly buffer into the given region of memory.
  void FinalizeInstructions(const MemoryRegion& region);

  void SpillRegisters(vixl::CPURegList registers, int offset);
  void UnspillRegisters(vixl::CPURegList registers, int offset);

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
  void LoadRef(ManagedRegister dest, FrameOffset src) OVERRIDE;
  void LoadRef(ManagedRegister dest, ManagedRegister base, MemberOffset offs,
               bool unpoison_reference) OVERRIDE;
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

  // Set up out_reg to hold a Object** into the handle scope, or to be null if the
  // value is null and null_allowed. in_reg holds a possibly stale reference
  // that can be used to avoid loading the handle scope entry to see if the value is
  // null.
  void CreateHandleScopeEntry(ManagedRegister out_reg, FrameOffset handlescope_offset,
                       ManagedRegister in_reg, bool null_allowed) OVERRIDE;

  // Set up out_off to hold a Object** into the handle scope, or to be null if the
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

  //
  // Heap poisoning.
  //

  // Poison a heap reference contained in `reg`.
  void PoisonHeapReference(vixl::Register reg);
  // Unpoison a heap reference contained in `reg`.
  void UnpoisonHeapReference(vixl::Register reg);
  // Unpoison a heap reference contained in `reg` if heap poisoning is enabled.
  void MaybeUnpoisonHeapReference(vixl::Register reg);

  void Bind(Label* label ATTRIBUTE_UNUSED) OVERRIDE {
    UNIMPLEMENTED(FATAL) << "Do not use Bind for ARM64";
  }
  void Jump(Label* label ATTRIBUTE_UNUSED) OVERRIDE {
    UNIMPLEMENTED(FATAL) << "Do not use Jump for ARM64";
  }

 private:
  static vixl::Register reg_x(int code) {
    CHECK(code < kNumberOfXRegisters) << code;
    if (code == SP) {
      return vixl::sp;
    } else if (code == XZR) {
      return vixl::xzr;
    }
    return vixl::Register::XRegFromCode(code);
  }

  static vixl::Register reg_w(int code) {
    CHECK(code < kNumberOfWRegisters) << code;
    if (code == WSP) {
      return vixl::wsp;
    } else if (code == WZR) {
      return vixl::wzr;
    }
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
                      XRegister base, int32_t offset);
  void StoreToOffset(XRegister source, XRegister base, int32_t offset);
  void StoreSToOffset(SRegister source, XRegister base, int32_t offset);
  void StoreDToOffset(DRegister source, XRegister base, int32_t offset);

  void LoadImmediate(XRegister dest, int32_t value, vixl::Condition cond = vixl::al);
  void Load(Arm64ManagedRegister dst, XRegister src, int32_t src_offset, size_t size);
  void LoadWFromOffset(LoadOperandType type, WRegister dest,
                      XRegister base, int32_t offset);
  void LoadFromOffset(XRegister dest, XRegister base, int32_t offset);
  void LoadSFromOffset(SRegister dest, XRegister base, int32_t offset);
  void LoadDFromOffset(DRegister dest, XRegister base, int32_t offset);
  void AddConstant(XRegister rd, int32_t value, vixl::Condition cond = vixl::al);
  void AddConstant(XRegister rd, XRegister rn, int32_t value, vixl::Condition cond = vixl::al);

  // List of exception blocks to generate at the end of the code cache.
  ArenaVector<std::unique_ptr<Arm64Exception>> exception_blocks_;

 public:
  // Vixl assembler.
  vixl::MacroAssembler* const vixl_masm_;

  // Used for testing.
  friend class Arm64ManagedRegister_VixlRegisters_Test;
};

}  // namespace arm64
}  // namespace art

#endif  // ART_COMPILER_UTILS_ARM64_ASSEMBLER_ARM64_H_
