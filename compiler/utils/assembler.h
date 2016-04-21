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

#ifndef ART_COMPILER_UTILS_ASSEMBLER_H_
#define ART_COMPILER_UTILS_ASSEMBLER_H_

#include <vector>

#include "arch/instruction_set.h"
#include "arch/instruction_set_features.h"
#include "arm/constants_arm.h"
#include "base/arena_allocator.h"
#include "base/arena_object.h"
#include "base/logging.h"
#include "base/macros.h"
#include "debug/dwarf/debug_frame_opcode_writer.h"
#include "label.h"
#include "managed_register.h"
#include "memory_region.h"
#include "mips/constants_mips.h"
#include "offsets.h"
#include "x86/constants_x86.h"
#include "x86_64/constants_x86_64.h"

namespace art {

class Assembler;
class AssemblerBuffer;

// Assembler fixups are positions in generated code that require processing
// after the code has been copied to executable memory. This includes building
// relocation information.
class AssemblerFixup {
 public:
  virtual void Process(const MemoryRegion& region, int position) = 0;
  virtual ~AssemblerFixup() {}

 private:
  AssemblerFixup* previous_;
  int position_;

  AssemblerFixup* previous() const { return previous_; }
  void set_previous(AssemblerFixup* previous_in) { previous_ = previous_in; }

  int position() const { return position_; }
  void set_position(int position_in) { position_ = position_in; }

  friend class AssemblerBuffer;
};

// Parent of all queued slow paths, emitted during finalization
class SlowPath : public DeletableArenaObject<kArenaAllocAssembler> {
 public:
  SlowPath() : next_(nullptr) {}
  virtual ~SlowPath() {}

  Label* Continuation() { return &continuation_; }
  Label* Entry() { return &entry_; }
  // Generate code for slow path
  virtual void Emit(Assembler *sp_asm) = 0;

 protected:
  // Entry branched to by fast path
  Label entry_;
  // Optional continuation that is branched to at the end of the slow path
  Label continuation_;
  // Next in linked list of slow paths
  SlowPath *next_;

 private:
  friend class AssemblerBuffer;
  DISALLOW_COPY_AND_ASSIGN(SlowPath);
};

class AssemblerBuffer {
 public:
  explicit AssemblerBuffer(ArenaAllocator* arena);
  ~AssemblerBuffer();

  ArenaAllocator* GetArena() {
    return arena_;
  }

  // Basic support for emitting, loading, and storing.
  template<typename T> void Emit(T value) {
    CHECK(HasEnsuredCapacity());
    *reinterpret_cast<T*>(cursor_) = value;
    cursor_ += sizeof(T);
  }

  template<typename T> T Load(size_t position) {
    CHECK_LE(position, Size() - static_cast<int>(sizeof(T)));
    return *reinterpret_cast<T*>(contents_ + position);
  }

  template<typename T> void Store(size_t position, T value) {
    CHECK_LE(position, Size() - static_cast<int>(sizeof(T)));
    *reinterpret_cast<T*>(contents_ + position) = value;
  }

  void Resize(size_t new_size) {
    if (new_size > Capacity()) {
      ExtendCapacity(new_size);
    }
    cursor_ = contents_ + new_size;
  }

  void Move(size_t newposition, size_t oldposition, size_t size) {
    // Move a chunk of the buffer from oldposition to newposition.
    DCHECK_LE(oldposition + size, Size());
    DCHECK_LE(newposition + size, Size());
    memmove(contents_ + newposition, contents_ + oldposition, size);
  }

  // Emit a fixup at the current location.
  void EmitFixup(AssemblerFixup* fixup) {
    fixup->set_previous(fixup_);
    fixup->set_position(Size());
    fixup_ = fixup;
  }

  void EnqueueSlowPath(SlowPath* slowpath) {
    if (slow_path_ == nullptr) {
      slow_path_ = slowpath;
    } else {
      SlowPath* cur = slow_path_;
      for ( ; cur->next_ != nullptr ; cur = cur->next_) {}
      cur->next_ = slowpath;
    }
  }

  void EmitSlowPaths(Assembler* sp_asm) {
    SlowPath* cur = slow_path_;
    SlowPath* next = nullptr;
    slow_path_ = nullptr;
    for ( ; cur != nullptr ; cur = next) {
      cur->Emit(sp_asm);
      next = cur->next_;
      delete cur;
    }
  }

  // Get the size of the emitted code.
  size_t Size() const {
    CHECK_GE(cursor_, contents_);
    return cursor_ - contents_;
  }

  uint8_t* contents() const { return contents_; }

  // Copy the assembled instructions into the specified memory block
  // and apply all fixups.
  void FinalizeInstructions(const MemoryRegion& region);

  // To emit an instruction to the assembler buffer, the EnsureCapacity helper
  // must be used to guarantee that the underlying data area is big enough to
  // hold the emitted instruction. Usage:
  //
  //     AssemblerBuffer buffer;
  //     AssemblerBuffer::EnsureCapacity ensured(&buffer);
  //     ... emit bytes for single instruction ...

#ifndef NDEBUG

  class EnsureCapacity {
   public:
    explicit EnsureCapacity(AssemblerBuffer* buffer) {
      if (buffer->cursor() > buffer->limit()) {
        buffer->ExtendCapacity(buffer->Size() + kMinimumGap);
      }
      // In debug mode, we save the assembler buffer along with the gap
      // size before we start emitting to the buffer. This allows us to
      // check that any single generated instruction doesn't overflow the
      // limit implied by the minimum gap size.
      buffer_ = buffer;
      gap_ = ComputeGap();
      // Make sure that extending the capacity leaves a big enough gap
      // for any kind of instruction.
      CHECK_GE(gap_, kMinimumGap);
      // Mark the buffer as having ensured the capacity.
      CHECK(!buffer->HasEnsuredCapacity());  // Cannot nest.
      buffer->has_ensured_capacity_ = true;
    }

    ~EnsureCapacity() {
      // Unmark the buffer, so we cannot emit after this.
      buffer_->has_ensured_capacity_ = false;
      // Make sure the generated instruction doesn't take up more
      // space than the minimum gap.
      int delta = gap_ - ComputeGap();
      CHECK_LE(delta, kMinimumGap);
    }

   private:
    AssemblerBuffer* buffer_;
    int gap_;

    int ComputeGap() { return buffer_->Capacity() - buffer_->Size(); }
  };

  bool has_ensured_capacity_;
  bool HasEnsuredCapacity() const { return has_ensured_capacity_; }

#else

  class EnsureCapacity {
   public:
    explicit EnsureCapacity(AssemblerBuffer* buffer) {
      if (buffer->cursor() > buffer->limit()) {
        buffer->ExtendCapacity(buffer->Size() + kMinimumGap);
      }
    }
  };

  // When building the C++ tests, assertion code is enabled. To allow
  // asserting that the user of the assembler buffer has ensured the
  // capacity needed for emitting, we add a dummy method in non-debug mode.
  bool HasEnsuredCapacity() const { return true; }

#endif

  // Returns the position in the instruction stream.
  int GetPosition() { return  cursor_ - contents_; }

  size_t Capacity() const {
    CHECK_GE(limit_, contents_);
    return (limit_ - contents_) + kMinimumGap;
  }

  // Unconditionally increase the capacity.
  // The provided `min_capacity` must be higher than current `Capacity()`.
  void ExtendCapacity(size_t min_capacity);

 private:
  // The limit is set to kMinimumGap bytes before the end of the data area.
  // This leaves enough space for the longest possible instruction and allows
  // for a single, fast space check per instruction.
  static const int kMinimumGap = 32;

  ArenaAllocator* arena_;
  uint8_t* contents_;
  uint8_t* cursor_;
  uint8_t* limit_;
  AssemblerFixup* fixup_;
#ifndef NDEBUG
  bool fixups_processed_;
#endif

  // Head of linked list of slow paths
  SlowPath* slow_path_;

  uint8_t* cursor() const { return cursor_; }
  uint8_t* limit() const { return limit_; }

  // Process the fixup chain starting at the given fixup. The offset is
  // non-zero for fixups in the body if the preamble is non-empty.
  void ProcessFixups(const MemoryRegion& region);

  // Compute the limit based on the data area and the capacity. See
  // description of kMinimumGap for the reasoning behind the value.
  static uint8_t* ComputeLimit(uint8_t* data, size_t capacity) {
    return data + capacity - kMinimumGap;
  }

  friend class AssemblerFixup;
};

// The purpose of this class is to ensure that we do not have to explicitly
// call the AdvancePC method (which is good for convenience and correctness).
class DebugFrameOpCodeWriterForAssembler FINAL
    : public dwarf::DebugFrameOpCodeWriter<> {
 public:
  struct DelayedAdvancePC {
    uint32_t stream_pos;
    uint32_t pc;
  };

  // This method is called the by the opcode writers.
  virtual void ImplicitlyAdvancePC() FINAL;

  explicit DebugFrameOpCodeWriterForAssembler(Assembler* buffer)
      : dwarf::DebugFrameOpCodeWriter<>(false /* enabled */),
        assembler_(buffer),
        delay_emitting_advance_pc_(false),
        delayed_advance_pcs_() {
  }

  ~DebugFrameOpCodeWriterForAssembler() {
    DCHECK(delayed_advance_pcs_.empty());
  }

  // Tell the writer to delay emitting advance PC info.
  // The assembler must explicitly process all the delayed advances.
  void DelayEmittingAdvancePCs() {
    delay_emitting_advance_pc_ = true;
  }

  // Override the last delayed PC. The new PC can be out of order.
  void OverrideDelayedPC(size_t pc) {
    DCHECK(delay_emitting_advance_pc_);
    DCHECK(!delayed_advance_pcs_.empty());
    delayed_advance_pcs_.back().pc = pc;
  }

  // Return the number of delayed advance PC entries.
  size_t NumberOfDelayedAdvancePCs() const {
    return delayed_advance_pcs_.size();
  }

  // Release the CFI stream and advance PC infos so that the assembler can patch it.
  std::pair<std::vector<uint8_t>, std::vector<DelayedAdvancePC>>
  ReleaseStreamAndPrepareForDelayedAdvancePC() {
    DCHECK(delay_emitting_advance_pc_);
    delay_emitting_advance_pc_ = false;
    std::pair<std::vector<uint8_t>, std::vector<DelayedAdvancePC>> result;
    result.first.swap(opcodes_);
    result.second.swap(delayed_advance_pcs_);
    return result;
  }

  // Reserve space for the CFI stream.
  void ReserveCFIStream(size_t capacity) {
    opcodes_.reserve(capacity);
  }

  // Append raw data to the CFI stream.
  void AppendRawData(const std::vector<uint8_t>& raw_data, size_t first, size_t last) {
    DCHECK_LE(0u, first);
    DCHECK_LE(first, last);
    DCHECK_LE(last, raw_data.size());
    opcodes_.insert(opcodes_.end(), raw_data.begin() + first, raw_data.begin() + last);
  }

 private:
  Assembler* assembler_;
  bool delay_emitting_advance_pc_;
  std::vector<DelayedAdvancePC> delayed_advance_pcs_;
};

class Assembler : public DeletableArenaObject<kArenaAllocAssembler> {
 public:
  static std::unique_ptr<Assembler> Create(
      ArenaAllocator* arena,
      InstructionSet instruction_set,
      const InstructionSetFeatures* instruction_set_features = nullptr);

  // Finalize the code; emit slow paths, fixup branches, add literal pool, etc.
  virtual void FinalizeCode() { buffer_.EmitSlowPaths(this); }

  // Size of generated code
  virtual size_t CodeSize() const { return buffer_.Size(); }
  virtual const uint8_t* CodeBufferBaseAddress() const { return buffer_.contents(); }

  // Copy instructions out of assembly buffer into the given region of memory
  virtual void FinalizeInstructions(const MemoryRegion& region) {
    buffer_.FinalizeInstructions(region);
  }

  // TODO: Implement with disassembler.
  virtual void Comment(const char* format ATTRIBUTE_UNUSED, ...) {}

  // Emit code that will create an activation on the stack
  virtual void BuildFrame(size_t frame_size, ManagedRegister method_reg,
                          const std::vector<ManagedRegister>& callee_save_regs,
                          const ManagedRegisterEntrySpills& entry_spills) = 0;

  // Emit code that will remove an activation from the stack
  virtual void RemoveFrame(size_t frame_size,
                           const std::vector<ManagedRegister>& callee_save_regs) = 0;

  virtual void IncreaseFrameSize(size_t adjust) = 0;
  virtual void DecreaseFrameSize(size_t adjust) = 0;

  // Store routines
  virtual void Store(FrameOffset offs, ManagedRegister src, size_t size) = 0;
  virtual void StoreRef(FrameOffset dest, ManagedRegister src) = 0;
  virtual void StoreRawPtr(FrameOffset dest, ManagedRegister src) = 0;

  virtual void StoreImmediateToFrame(FrameOffset dest, uint32_t imm,
                                     ManagedRegister scratch) = 0;

  virtual void StoreImmediateToThread32(ThreadOffset<4> dest, uint32_t imm,
                                        ManagedRegister scratch);
  virtual void StoreImmediateToThread64(ThreadOffset<8> dest, uint32_t imm,
                                        ManagedRegister scratch);

  virtual void StoreStackOffsetToThread32(ThreadOffset<4> thr_offs,
                                          FrameOffset fr_offs,
                                          ManagedRegister scratch);
  virtual void StoreStackOffsetToThread64(ThreadOffset<8> thr_offs,
                                          FrameOffset fr_offs,
                                          ManagedRegister scratch);

  virtual void StoreStackPointerToThread32(ThreadOffset<4> thr_offs);
  virtual void StoreStackPointerToThread64(ThreadOffset<8> thr_offs);

  virtual void StoreSpanning(FrameOffset dest, ManagedRegister src,
                             FrameOffset in_off, ManagedRegister scratch) = 0;

  // Load routines
  virtual void Load(ManagedRegister dest, FrameOffset src, size_t size) = 0;

  virtual void LoadFromThread32(ManagedRegister dest, ThreadOffset<4> src, size_t size);
  virtual void LoadFromThread64(ManagedRegister dest, ThreadOffset<8> src, size_t size);

  virtual void LoadRef(ManagedRegister dest, FrameOffset src) = 0;
  // If unpoison_reference is true and kPoisonReference is true, then we negate the read reference.
  virtual void LoadRef(ManagedRegister dest, ManagedRegister base, MemberOffset offs,
                       bool unpoison_reference) = 0;

  virtual void LoadRawPtr(ManagedRegister dest, ManagedRegister base, Offset offs) = 0;

  virtual void LoadRawPtrFromThread32(ManagedRegister dest, ThreadOffset<4> offs);
  virtual void LoadRawPtrFromThread64(ManagedRegister dest, ThreadOffset<8> offs);

  // Copying routines
  virtual void Move(ManagedRegister dest, ManagedRegister src, size_t size) = 0;

  virtual void CopyRawPtrFromThread32(FrameOffset fr_offs, ThreadOffset<4> thr_offs,
                                      ManagedRegister scratch);
  virtual void CopyRawPtrFromThread64(FrameOffset fr_offs, ThreadOffset<8> thr_offs,
                                      ManagedRegister scratch);

  virtual void CopyRawPtrToThread32(ThreadOffset<4> thr_offs, FrameOffset fr_offs,
                                    ManagedRegister scratch);
  virtual void CopyRawPtrToThread64(ThreadOffset<8> thr_offs, FrameOffset fr_offs,
                                    ManagedRegister scratch);

  virtual void CopyRef(FrameOffset dest, FrameOffset src,
                       ManagedRegister scratch) = 0;

  virtual void Copy(FrameOffset dest, FrameOffset src, ManagedRegister scratch, size_t size) = 0;

  virtual void Copy(FrameOffset dest, ManagedRegister src_base, Offset src_offset,
                    ManagedRegister scratch, size_t size) = 0;

  virtual void Copy(ManagedRegister dest_base, Offset dest_offset, FrameOffset src,
                    ManagedRegister scratch, size_t size) = 0;

  virtual void Copy(FrameOffset dest, FrameOffset src_base, Offset src_offset,
                    ManagedRegister scratch, size_t size) = 0;

  virtual void Copy(ManagedRegister dest, Offset dest_offset,
                    ManagedRegister src, Offset src_offset,
                    ManagedRegister scratch, size_t size) = 0;

  virtual void Copy(FrameOffset dest, Offset dest_offset, FrameOffset src, Offset src_offset,
                    ManagedRegister scratch, size_t size) = 0;

  virtual void MemoryBarrier(ManagedRegister scratch) = 0;

  // Sign extension
  virtual void SignExtend(ManagedRegister mreg, size_t size) = 0;

  // Zero extension
  virtual void ZeroExtend(ManagedRegister mreg, size_t size) = 0;

  // Exploit fast access in managed code to Thread::Current()
  virtual void GetCurrentThread(ManagedRegister tr) = 0;
  virtual void GetCurrentThread(FrameOffset dest_offset,
                                ManagedRegister scratch) = 0;

  // Set up out_reg to hold a Object** into the handle scope, or to be null if the
  // value is null and null_allowed. in_reg holds a possibly stale reference
  // that can be used to avoid loading the handle scope entry to see if the value is
  // null.
  virtual void CreateHandleScopeEntry(ManagedRegister out_reg, FrameOffset handlescope_offset,
                               ManagedRegister in_reg, bool null_allowed) = 0;

  // Set up out_off to hold a Object** into the handle scope, or to be null if the
  // value is null and null_allowed.
  virtual void CreateHandleScopeEntry(FrameOffset out_off, FrameOffset handlescope_offset,
                               ManagedRegister scratch, bool null_allowed) = 0;

  // src holds a handle scope entry (Object**) load this into dst
  virtual void LoadReferenceFromHandleScope(ManagedRegister dst,
                                     ManagedRegister src) = 0;

  // Heap::VerifyObject on src. In some cases (such as a reference to this) we
  // know that src may not be null.
  virtual void VerifyObject(ManagedRegister src, bool could_be_null) = 0;
  virtual void VerifyObject(FrameOffset src, bool could_be_null) = 0;

  // Call to address held at [base+offset]
  virtual void Call(ManagedRegister base, Offset offset,
                    ManagedRegister scratch) = 0;
  virtual void Call(FrameOffset base, Offset offset,
                    ManagedRegister scratch) = 0;
  virtual void CallFromThread32(ThreadOffset<4> offset, ManagedRegister scratch);
  virtual void CallFromThread64(ThreadOffset<8> offset, ManagedRegister scratch);

  // Generate code to check if Thread::Current()->exception_ is non-null
  // and branch to a ExceptionSlowPath if it is.
  virtual void ExceptionPoll(ManagedRegister scratch, size_t stack_adjust) = 0;

  virtual void Bind(Label* label) = 0;
  virtual void Jump(Label* label) = 0;

  virtual ~Assembler() {}

  /**
   * @brief Buffer of DWARF's Call Frame Information opcodes.
   * @details It is used by debuggers and other tools to unwind the call stack.
   */
  DebugFrameOpCodeWriterForAssembler& cfi() { return cfi_; }

 protected:
  explicit Assembler(ArenaAllocator* arena) : buffer_(arena), cfi_(this) {}

  ArenaAllocator* GetArena() {
    return buffer_.GetArena();
  }

  AssemblerBuffer buffer_;

  DebugFrameOpCodeWriterForAssembler cfi_;
};

}  // namespace art

#endif  // ART_COMPILER_UTILS_ASSEMBLER_H_
