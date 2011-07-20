// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_ASSEMBLER_H_
#define ART_SRC_ASSEMBLER_H_

#include "src/logging.h"
#include "src/macros.h"
#include "src/memory_region.h"

namespace art {

class Assembler;
class AssemblerBuffer;
class AssemblerFixup;

// Allow the meaning of offsets to be strongly typed
class Offset {
 public:
  explicit Offset(size_t val) : val_(val) {}
  int32_t Int32Value() const {
    return static_cast<int32_t>(val_);
  }
  uint32_t Uint32Value() const {
    return static_cast<uint32_t>(val_);
  }
 protected:
  size_t val_;
};
std::ostream& operator<<(std::ostream& os, const Offset& offs);

// Offsets relative to the current frame
class FrameOffset : public Offset {
 public:
  explicit FrameOffset(size_t val) : Offset(val) {}
  bool operator>(FrameOffset other) const { return val_ > other.val_; }
  bool operator<(FrameOffset other) const { return val_ < other.val_; }
};

// Offsets relative to the current running thread
class ThreadOffset : public Offset {
 public:
  explicit ThreadOffset(size_t val) : Offset(val) {}
};

// Offsets relative to an object
class MemberOffset : public Offset {
 public:
  explicit MemberOffset(size_t val) : Offset(val) {}
};

class Label {
 public:
  Label() : position_(0) {}

  ~Label() {
    // Assert if label is being destroyed with unresolved branches pending.
    CHECK(!IsLinked());
  }

  // Returns the position for bound and linked labels. Cannot be used
  // for unused labels.
  int Position() const {
    CHECK(!IsUnused());
    return IsBound() ? -position_ - kPointerSize : position_ - kPointerSize;
  }

  int LinkPosition() const {
    CHECK(IsLinked());
    return position_ - kWordSize;
  }

  bool IsBound() const { return position_ < 0; }
  bool IsUnused() const { return position_ == 0; }
  bool IsLinked() const { return position_ > 0; }

 private:
  int position_;

  void Reinitialize() {
    position_ = 0;
  }

  void BindTo(int position) {
    CHECK(!IsBound());
    position_ = -position - kPointerSize;
    CHECK(IsBound());
  }

  void LinkTo(int position) {
    CHECK(!IsBound());
    position_ = position + kPointerSize;
    CHECK(IsLinked());
  }

  friend class Assembler;
  DISALLOW_COPY_AND_ASSIGN(Label);
};


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
  void set_previous(AssemblerFixup* previous) { previous_ = previous; }

  int position() const { return position_; }
  void set_position(int position) { position_ = position; }

  friend class AssemblerBuffer;
};


class AssemblerBuffer {
 public:
  AssemblerBuffer();
  ~AssemblerBuffer();

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

  // Emit a fixup at the current location.
  void EmitFixup(AssemblerFixup* fixup) {
    fixup->set_previous(fixup_);
    fixup->set_position(Size());
    fixup_ = fixup;
  }

  // Get the size of the emitted code.
  size_t Size() const {
    CHECK_GE(cursor_, contents_);
    return cursor_ - contents_;
  }

  byte* contents() const { return contents_; }

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

#ifdef DEBUG

  class EnsureCapacity {
   public:
    explicit EnsureCapacity(AssemblerBuffer* buffer) {
      if (buffer->cursor() >= buffer->limit()) buffer->ExtendCapacity();
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
      if (buffer->cursor() >= buffer->limit()) buffer->ExtendCapacity();
    }
  };

  // When building the C++ tests, assertion code is enabled. To allow
  // asserting that the user of the assembler buffer has ensured the
  // capacity needed for emitting, we add a dummy method in non-debug mode.
  bool HasEnsuredCapacity() const { return true; }

#endif

  // Returns the position in the instruction stream.
  int GetPosition() { return  cursor_ - contents_; }

 private:
  // The limit is set to kMinimumGap bytes before the end of the data area.
  // This leaves enough space for the longest possible instruction and allows
  // for a single, fast space check per instruction.
  static const int kMinimumGap = 32;

  byte* contents_;
  byte* cursor_;
  byte* limit_;
  AssemblerFixup* fixup_;
  bool fixups_processed_;

  byte* cursor() const { return cursor_; }
  byte* limit() const { return limit_; }
  size_t Capacity() const {
    CHECK_GE(limit_, contents_);
    return (limit_ - contents_) + kMinimumGap;
  }

  // Process the fixup chain starting at the given fixup. The offset is
  // non-zero for fixups in the body if the preamble is non-empty.
  void ProcessFixups(const MemoryRegion& region);

  // Compute the limit based on the data area and the capacity. See
  // description of kMinimumGap for the reasoning behind the value.
  static byte* ComputeLimit(byte* data, size_t capacity) {
    return data + capacity - kMinimumGap;
  }

  void ExtendCapacity();

  friend class AssemblerFixup;
};

}  // namespace art

#if defined(__i386__)
#include "src/assembler_x86.h"
#elif defined(__arm__)
#include "src/assembler_arm.h"
#endif

#endif  // ART_SRC_ASSEMBLER_H_
