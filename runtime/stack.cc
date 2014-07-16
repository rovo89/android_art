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

#include "stack.h"

#include "arch/context.h"
#include "base/hex_dump.h"
#include "mirror/art_method-inl.h"
#include "mirror/class-inl.h"
#include "mirror/object.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "quick/quick_method_frame_info.h"
#include "runtime.h"
#include "thread.h"
#include "thread_list.h"
#include "throw_location.h"
#include "verify_object-inl.h"
#include "vmap_table.h"

namespace art {

mirror::Object* ShadowFrame::GetThisObject() const {
  mirror::ArtMethod* m = GetMethod();
  if (m->IsStatic()) {
    return NULL;
  } else if (m->IsNative()) {
    return GetVRegReference(0);
  } else {
    const DexFile::CodeItem* code_item = m->GetCodeItem();
    CHECK(code_item != NULL) << PrettyMethod(m);
    uint16_t reg = code_item->registers_size_ - code_item->ins_size_;
    return GetVRegReference(reg);
  }
}

mirror::Object* ShadowFrame::GetThisObject(uint16_t num_ins) const {
  mirror::ArtMethod* m = GetMethod();
  if (m->IsStatic()) {
    return NULL;
  } else {
    return GetVRegReference(NumberOfVRegs() - num_ins);
  }
}

ThrowLocation ShadowFrame::GetCurrentLocationForThrow() const {
  return ThrowLocation(GetThisObject(), GetMethod(), GetDexPC());
}

size_t ManagedStack::NumJniShadowFrameReferences() const {
  size_t count = 0;
  for (const ManagedStack* current_fragment = this; current_fragment != NULL;
       current_fragment = current_fragment->GetLink()) {
    for (ShadowFrame* current_frame = current_fragment->top_shadow_frame_; current_frame != NULL;
         current_frame = current_frame->GetLink()) {
      if (current_frame->GetMethod()->IsNative()) {
        // The JNI ShadowFrame only contains references. (For indirect reference.)
        count += current_frame->NumberOfVRegs();
      }
    }
  }
  return count;
}

bool ManagedStack::ShadowFramesContain(StackReference<mirror::Object>* shadow_frame_entry) const {
  for (const ManagedStack* current_fragment = this; current_fragment != NULL;
       current_fragment = current_fragment->GetLink()) {
    for (ShadowFrame* current_frame = current_fragment->top_shadow_frame_; current_frame != NULL;
         current_frame = current_frame->GetLink()) {
      if (current_frame->Contains(shadow_frame_entry)) {
        return true;
      }
    }
  }
  return false;
}

StackVisitor::StackVisitor(Thread* thread, Context* context)
    : thread_(thread), cur_shadow_frame_(NULL),
      cur_quick_frame_(NULL), cur_quick_frame_pc_(0), num_frames_(0), cur_depth_(0),
      context_(context) {
  DCHECK(thread == Thread::Current() || thread->IsSuspended()) << *thread;
}

StackVisitor::StackVisitor(Thread* thread, Context* context, size_t num_frames)
    : thread_(thread), cur_shadow_frame_(NULL),
      cur_quick_frame_(NULL), cur_quick_frame_pc_(0), num_frames_(num_frames), cur_depth_(0),
      context_(context) {
  DCHECK(thread == Thread::Current() || thread->IsSuspended()) << *thread;
}

uint32_t StackVisitor::GetDexPc(bool abort_on_failure) const {
  if (cur_shadow_frame_ != NULL) {
    return cur_shadow_frame_->GetDexPC();
  } else if (cur_quick_frame_ != NULL) {
    return GetMethod()->ToDexPc(cur_quick_frame_pc_, abort_on_failure);
  } else {
    return 0;
  }
}

mirror::Object* StackVisitor::GetThisObject() const {
  mirror::ArtMethod* m = GetMethod();
  if (m->IsStatic()) {
    return NULL;
  } else if (m->IsNative()) {
    if (cur_quick_frame_ != NULL) {
      HandleScope* hs = reinterpret_cast<HandleScope*>(
          reinterpret_cast<char*>(cur_quick_frame_) + m->GetHandleScopeOffsetInBytes());
      return hs->GetReference(0);
    } else {
      return cur_shadow_frame_->GetVRegReference(0);
    }
  } else {
    const DexFile::CodeItem* code_item = m->GetCodeItem();
    if (code_item == NULL) {
      UNIMPLEMENTED(ERROR) << "Failed to determine this object of abstract or proxy method: "
          << PrettyMethod(m);
      return nullptr;
    } else {
      uint16_t reg = code_item->registers_size_ - code_item->ins_size_;
      return reinterpret_cast<mirror::Object*>(GetVReg(m, reg, kReferenceVReg));
    }
  }
}

size_t StackVisitor::GetNativePcOffset() const {
  DCHECK(!IsShadowFrame());
  return GetMethod()->NativePcOffset(cur_quick_frame_pc_);
}

bool StackVisitor::GetVReg(mirror::ArtMethod* m, uint16_t vreg, VRegKind kind,
                           uint32_t* val) const {
  if (cur_quick_frame_ != nullptr) {
    DCHECK(context_ != nullptr);  // You can't reliably read registers without a context.
    DCHECK(m == GetMethod());
    const void* code_pointer = m->GetQuickOatCodePointer();
    DCHECK(code_pointer != nullptr);
    const VmapTable vmap_table(m->GetVmapTable(code_pointer));
    QuickMethodFrameInfo frame_info = m->GetQuickFrameInfo(code_pointer);
    uint32_t vmap_offset;
    // TODO: IsInContext stops before spotting floating point registers.
    if (vmap_table.IsInContext(vreg, kind, &vmap_offset)) {
      bool is_float = (kind == kFloatVReg) || (kind == kDoubleLoVReg) || (kind == kDoubleHiVReg);
      uint32_t spill_mask = is_float ? frame_info.FpSpillMask() : frame_info.CoreSpillMask();
      uint32_t reg = vmap_table.ComputeRegister(spill_mask, vmap_offset, kind);
      uintptr_t ptr_val;
      bool success = is_float ? GetFPR(reg, &ptr_val) : GetGPR(reg, &ptr_val);
      if (!success) {
        return false;
      }
      bool target64 = Is64BitInstructionSet(kRuntimeISA);
      if (target64) {
        bool wide_lo = (kind == kLongLoVReg) || (kind == kDoubleLoVReg);
        bool wide_hi = (kind == kLongHiVReg) || (kind == kDoubleHiVReg);
        int64_t value_long = static_cast<int64_t>(ptr_val);
        if (wide_lo) {
          ptr_val = static_cast<uintptr_t>(value_long & 0xFFFFFFFF);
        } else if (wide_hi) {
          ptr_val = static_cast<uintptr_t>(value_long >> 32);
        }
      }
      *val = ptr_val;
      return true;
    } else {
      const DexFile::CodeItem* code_item = m->GetCodeItem();
      DCHECK(code_item != nullptr) << PrettyMethod(m);  // Can't be NULL or how would we compile
                                                        // its instructions?
      *val = *GetVRegAddr(cur_quick_frame_, code_item, frame_info.CoreSpillMask(),
                          frame_info.FpSpillMask(), frame_info.FrameSizeInBytes(), vreg);
      return true;
    }
  } else {
    *val = cur_shadow_frame_->GetVReg(vreg);
    return true;
  }
}

bool StackVisitor::GetVRegPair(mirror::ArtMethod* m, uint16_t vreg, VRegKind kind_lo,
                               VRegKind kind_hi, uint64_t* val) const {
  if (kind_lo == kLongLoVReg) {
    DCHECK_EQ(kind_hi, kLongHiVReg);
  } else if (kind_lo == kDoubleLoVReg) {
    DCHECK_EQ(kind_hi, kDoubleHiVReg);
  } else {
    LOG(FATAL) << "Expected long or double: kind_lo=" << kind_lo << ", kind_hi=" << kind_hi;
  }
  if (cur_quick_frame_ != nullptr) {
    DCHECK(context_ != nullptr);  // You can't reliably read registers without a context.
    DCHECK(m == GetMethod());
    const void* code_pointer = m->GetQuickOatCodePointer();
    DCHECK(code_pointer != nullptr);
    const VmapTable vmap_table(m->GetVmapTable(code_pointer));
    QuickMethodFrameInfo frame_info = m->GetQuickFrameInfo(code_pointer);
    uint32_t vmap_offset_lo, vmap_offset_hi;
    // TODO: IsInContext stops before spotting floating point registers.
    if (vmap_table.IsInContext(vreg, kind_lo, &vmap_offset_lo) &&
        vmap_table.IsInContext(vreg + 1, kind_hi, &vmap_offset_hi)) {
      bool is_float = (kind_lo == kDoubleLoVReg);
      uint32_t spill_mask = is_float ? frame_info.FpSpillMask() : frame_info.CoreSpillMask();
      uint32_t reg_lo = vmap_table.ComputeRegister(spill_mask, vmap_offset_lo, kind_lo);
      uint32_t reg_hi = vmap_table.ComputeRegister(spill_mask, vmap_offset_hi, kind_hi);
      uintptr_t ptr_val_lo, ptr_val_hi;
      bool success = is_float ? GetFPR(reg_lo, &ptr_val_lo) : GetGPR(reg_lo, &ptr_val_lo);
      success &= is_float ? GetFPR(reg_hi, &ptr_val_hi) : GetGPR(reg_hi, &ptr_val_hi);
      if (!success) {
        return false;
      }
      bool target64 = Is64BitInstructionSet(kRuntimeISA);
      if (target64) {
        int64_t value_long_lo = static_cast<int64_t>(ptr_val_lo);
        int64_t value_long_hi = static_cast<int64_t>(ptr_val_hi);
        ptr_val_lo = static_cast<uintptr_t>(value_long_lo & 0xFFFFFFFF);
        ptr_val_hi = static_cast<uintptr_t>(value_long_hi >> 32);
      }
      *val = (static_cast<uint64_t>(ptr_val_hi) << 32) | static_cast<uint32_t>(ptr_val_lo);
      return true;
    } else {
      const DexFile::CodeItem* code_item = m->GetCodeItem();
      DCHECK(code_item != nullptr) << PrettyMethod(m);  // Can't be NULL or how would we compile
                                                        // its instructions?
      uint32_t* addr = GetVRegAddr(cur_quick_frame_, code_item, frame_info.CoreSpillMask(),
                                   frame_info.FpSpillMask(), frame_info.FrameSizeInBytes(), vreg);
      *val = *reinterpret_cast<uint64_t*>(addr);
      return true;
    }
  } else {
    *val = cur_shadow_frame_->GetVRegLong(vreg);
    return true;
  }
}

bool StackVisitor::SetVReg(mirror::ArtMethod* m, uint16_t vreg, uint32_t new_value,
                           VRegKind kind) {
  if (cur_quick_frame_ != nullptr) {
    DCHECK(context_ != nullptr);  // You can't reliably write registers without a context.
    DCHECK(m == GetMethod());
    const void* code_pointer = m->GetQuickOatCodePointer();
    DCHECK(code_pointer != nullptr);
    const VmapTable vmap_table(m->GetVmapTable(code_pointer));
    QuickMethodFrameInfo frame_info = m->GetQuickFrameInfo(code_pointer);
    uint32_t vmap_offset;
    // TODO: IsInContext stops before spotting floating point registers.
    if (vmap_table.IsInContext(vreg, kind, &vmap_offset)) {
      bool is_float = (kind == kFloatVReg) || (kind == kDoubleLoVReg) || (kind == kDoubleHiVReg);
      uint32_t spill_mask = is_float ? frame_info.FpSpillMask() : frame_info.CoreSpillMask();
      const uint32_t reg = vmap_table.ComputeRegister(spill_mask, vmap_offset, kind);
      bool target64 = Is64BitInstructionSet(kRuntimeISA);
      // Deal with 32 or 64-bit wide registers in a way that builds on all targets.
      if (target64) {
        bool wide_lo = (kind == kLongLoVReg) || (kind == kDoubleLoVReg);
        bool wide_hi = (kind == kLongHiVReg) || (kind == kDoubleHiVReg);
        if (wide_lo || wide_hi) {
          uintptr_t old_reg_val;
          bool success = is_float ? GetFPR(reg, &old_reg_val) : GetGPR(reg, &old_reg_val);
          if (!success) {
            return false;
          }
          uint64_t new_vreg_portion = static_cast<uint64_t>(new_value);
          uint64_t old_reg_val_as_wide = static_cast<uint64_t>(old_reg_val);
          uint64_t mask = 0xffffffff;
          if (wide_lo) {
            mask = mask << 32;
          } else {
            new_vreg_portion = new_vreg_portion << 32;
          }
          new_value = static_cast<uintptr_t>((old_reg_val_as_wide & mask) | new_vreg_portion);
        }
      }
      if (is_float) {
        return SetFPR(reg, new_value);
      } else {
        return SetGPR(reg, new_value);
      }
    } else {
      const DexFile::CodeItem* code_item = m->GetCodeItem();
      DCHECK(code_item != nullptr) << PrettyMethod(m);  // Can't be NULL or how would we compile
                                                        // its instructions?
      uint32_t* addr = GetVRegAddr(cur_quick_frame_, code_item, frame_info.CoreSpillMask(),
                                   frame_info.FpSpillMask(), frame_info.FrameSizeInBytes(), vreg);
      *addr = new_value;
      return true;
    }
  } else {
    cur_shadow_frame_->SetVReg(vreg, new_value);
    return true;
  }
}

bool StackVisitor::SetVRegPair(mirror::ArtMethod* m, uint16_t vreg, uint64_t new_value,
                               VRegKind kind_lo, VRegKind kind_hi) {
  if (kind_lo == kLongLoVReg) {
    DCHECK_EQ(kind_hi, kLongHiVReg);
  } else if (kind_lo == kDoubleLoVReg) {
    DCHECK_EQ(kind_hi, kDoubleHiVReg);
  } else {
    LOG(FATAL) << "Expected long or double: kind_lo=" << kind_lo << ", kind_hi=" << kind_hi;
  }
  if (cur_quick_frame_ != nullptr) {
    DCHECK(context_ != nullptr);  // You can't reliably write registers without a context.
    DCHECK(m == GetMethod());
    const void* code_pointer = m->GetQuickOatCodePointer();
    DCHECK(code_pointer != nullptr);
    const VmapTable vmap_table(m->GetVmapTable(code_pointer));
    QuickMethodFrameInfo frame_info = m->GetQuickFrameInfo(code_pointer);
    uint32_t vmap_offset_lo, vmap_offset_hi;
    // TODO: IsInContext stops before spotting floating point registers.
    if (vmap_table.IsInContext(vreg, kind_lo, &vmap_offset_lo) &&
        vmap_table.IsInContext(vreg + 1, kind_hi, &vmap_offset_hi)) {
      bool is_float = (kind_lo == kDoubleLoVReg);
      uint32_t spill_mask = is_float ? frame_info.FpSpillMask() : frame_info.CoreSpillMask();
      uint32_t reg_lo = vmap_table.ComputeRegister(spill_mask, vmap_offset_lo, kind_lo);
      uint32_t reg_hi = vmap_table.ComputeRegister(spill_mask, vmap_offset_hi, kind_hi);
      uintptr_t new_value_lo = static_cast<uintptr_t>(new_value & 0xFFFFFFFF);
      uintptr_t new_value_hi = static_cast<uintptr_t>(new_value >> 32);
      bool target64 = Is64BitInstructionSet(kRuntimeISA);
      // Deal with 32 or 64-bit wide registers in a way that builds on all targets.
      if (target64) {
        uintptr_t old_reg_val_lo, old_reg_val_hi;
        bool success = is_float ? GetFPR(reg_lo, &old_reg_val_lo) : GetGPR(reg_lo, &old_reg_val_lo);
        success &= is_float ? GetFPR(reg_hi, &old_reg_val_hi) : GetGPR(reg_hi, &old_reg_val_hi);
        if (!success) {
          return false;
        }
        uint64_t new_vreg_portion_lo = static_cast<uint64_t>(new_value_lo);
        uint64_t new_vreg_portion_hi = static_cast<uint64_t>(new_value_hi) << 32;
        uint64_t old_reg_val_lo_as_wide = static_cast<uint64_t>(old_reg_val_lo);
        uint64_t old_reg_val_hi_as_wide = static_cast<uint64_t>(old_reg_val_hi);
        uint64_t mask_lo = static_cast<uint64_t>(0xffffffff) << 32;
        uint64_t mask_hi = 0xffffffff;
        new_value_lo = static_cast<uintptr_t>((old_reg_val_lo_as_wide & mask_lo) | new_vreg_portion_lo);
        new_value_hi = static_cast<uintptr_t>((old_reg_val_hi_as_wide & mask_hi) | new_vreg_portion_hi);
      }
      bool success = is_float ? SetFPR(reg_lo, new_value_lo) : SetGPR(reg_lo, new_value_lo);
      success &= is_float ? SetFPR(reg_hi, new_value_hi) : SetGPR(reg_hi, new_value_hi);
      return success;
    } else {
      const DexFile::CodeItem* code_item = m->GetCodeItem();
      DCHECK(code_item != nullptr) << PrettyMethod(m);  // Can't be NULL or how would we compile
                                                        // its instructions?
      uint32_t* addr = GetVRegAddr(cur_quick_frame_, code_item, frame_info.CoreSpillMask(),
                                   frame_info.FpSpillMask(), frame_info.FrameSizeInBytes(), vreg);
      *reinterpret_cast<uint64_t*>(addr) = new_value;
      return true;
    }
  } else {
    cur_shadow_frame_->SetVRegLong(vreg, new_value);
    return true;
  }
}

uintptr_t* StackVisitor::GetGPRAddress(uint32_t reg) const {
  DCHECK(cur_quick_frame_ != NULL) << "This is a quick frame routine";
  return context_->GetGPRAddress(reg);
}

bool StackVisitor::GetGPR(uint32_t reg, uintptr_t* val) const {
  DCHECK(cur_quick_frame_ != NULL) << "This is a quick frame routine";
  return context_->GetGPR(reg, val);
}

bool StackVisitor::SetGPR(uint32_t reg, uintptr_t value) {
  DCHECK(cur_quick_frame_ != NULL) << "This is a quick frame routine";
  return context_->SetGPR(reg, value);
}

bool StackVisitor::GetFPR(uint32_t reg, uintptr_t* val) const {
  DCHECK(cur_quick_frame_ != NULL) << "This is a quick frame routine";
  return context_->GetFPR(reg, val);
}

bool StackVisitor::SetFPR(uint32_t reg, uintptr_t value) {
  DCHECK(cur_quick_frame_ != NULL) << "This is a quick frame routine";
  return context_->SetFPR(reg, value);
}

uintptr_t StackVisitor::GetReturnPc() const {
  byte* sp = reinterpret_cast<byte*>(GetCurrentQuickFrame());
  DCHECK(sp != NULL);
  byte* pc_addr = sp + GetMethod()->GetReturnPcOffsetInBytes();
  return *reinterpret_cast<uintptr_t*>(pc_addr);
}

void StackVisitor::SetReturnPc(uintptr_t new_ret_pc) {
  byte* sp = reinterpret_cast<byte*>(GetCurrentQuickFrame());
  CHECK(sp != NULL);
  byte* pc_addr = sp + GetMethod()->GetReturnPcOffsetInBytes();
  *reinterpret_cast<uintptr_t*>(pc_addr) = new_ret_pc;
}

size_t StackVisitor::ComputeNumFrames(Thread* thread) {
  struct NumFramesVisitor : public StackVisitor {
    explicit NumFramesVisitor(Thread* thread)
        : StackVisitor(thread, NULL), frames(0) {}

    bool VisitFrame() OVERRIDE {
      frames++;
      return true;
    }

    size_t frames;
  };
  NumFramesVisitor visitor(thread);
  visitor.WalkStack(true);
  return visitor.frames;
}

bool StackVisitor::GetNextMethodAndDexPc(mirror::ArtMethod** next_method, uint32_t* next_dex_pc) {
  struct HasMoreFramesVisitor : public StackVisitor {
    explicit HasMoreFramesVisitor(Thread* thread, size_t num_frames, size_t frame_height)
        : StackVisitor(thread, nullptr, num_frames), frame_height_(frame_height),
          found_frame_(false), has_more_frames_(false), next_method_(nullptr), next_dex_pc_(0) {
    }

    bool VisitFrame() OVERRIDE SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
      if (found_frame_) {
        mirror::ArtMethod* method = GetMethod();
        if (method != nullptr && !method->IsRuntimeMethod()) {
          has_more_frames_ = true;
          next_method_ = method;
          next_dex_pc_ = GetDexPc();
          return false;  // End stack walk once next method is found.
        }
      } else if (GetFrameHeight() == frame_height_) {
        found_frame_ = true;
      }
      return true;
    }

    size_t frame_height_;
    bool found_frame_;
    bool has_more_frames_;
    mirror::ArtMethod* next_method_;
    uint32_t next_dex_pc_;
  };
  HasMoreFramesVisitor visitor(thread_, GetNumFrames(), GetFrameHeight());
  visitor.WalkStack(true);
  *next_method = visitor.next_method_;
  *next_dex_pc = visitor.next_dex_pc_;
  return visitor.has_more_frames_;
}

void StackVisitor::DescribeStack(Thread* thread) {
  struct DescribeStackVisitor : public StackVisitor {
    explicit DescribeStackVisitor(Thread* thread)
        : StackVisitor(thread, NULL) {}

    bool VisitFrame() OVERRIDE SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
      LOG(INFO) << "Frame Id=" << GetFrameId() << " " << DescribeLocation();
      return true;
    }
  };
  DescribeStackVisitor visitor(thread);
  visitor.WalkStack(true);
}

std::string StackVisitor::DescribeLocation() const {
  std::string result("Visiting method '");
  mirror::ArtMethod* m = GetMethod();
  if (m == NULL) {
    return "upcall";
  }
  result += PrettyMethod(m);
  result += StringPrintf("' at dex PC 0x%04x", GetDexPc());
  if (!IsShadowFrame()) {
    result += StringPrintf(" (native PC %p)", reinterpret_cast<void*>(GetCurrentQuickFramePc()));
  }
  return result;
}

static instrumentation::InstrumentationStackFrame& GetInstrumentationStackFrame(Thread* thread,
                                                                                uint32_t depth) {
  CHECK_LT(depth, thread->GetInstrumentationStack()->size());
  return thread->GetInstrumentationStack()->at(depth);
}

void StackVisitor::SanityCheckFrame() const {
  if (kIsDebugBuild) {
    mirror::ArtMethod* method = GetMethod();
    CHECK_EQ(method->GetClass(), mirror::ArtMethod::GetJavaLangReflectArtMethod());
    if (cur_quick_frame_ != nullptr) {
      method->AssertPcIsWithinQuickCode(cur_quick_frame_pc_);
      // Frame sanity.
      size_t frame_size = method->GetFrameSizeInBytes();
      CHECK_NE(frame_size, 0u);
      // A rough guess at an upper size we expect to see for a frame.
      // 256 registers
      // 2 words HandleScope overhead
      // 3+3 register spills
      // TODO: this seems architecture specific for the case of JNI frames.
      // TODO: 083-compiler-regressions ManyFloatArgs shows this estimate is wrong.
      // const size_t kMaxExpectedFrameSize = (256 + 2 + 3 + 3) * sizeof(word);
      const size_t kMaxExpectedFrameSize = 2 * KB;
      CHECK_LE(frame_size, kMaxExpectedFrameSize);
      size_t return_pc_offset = method->GetReturnPcOffsetInBytes();
      CHECK_LT(return_pc_offset, frame_size);
    }
  }
}

void StackVisitor::WalkStack(bool include_transitions) {
  DCHECK(thread_ == Thread::Current() || thread_->IsSuspended());
  CHECK_EQ(cur_depth_, 0U);
  bool exit_stubs_installed = Runtime::Current()->GetInstrumentation()->AreExitStubsInstalled();
  uint32_t instrumentation_stack_depth = 0;

  for (const ManagedStack* current_fragment = thread_->GetManagedStack(); current_fragment != NULL;
       current_fragment = current_fragment->GetLink()) {
    cur_shadow_frame_ = current_fragment->GetTopShadowFrame();
    cur_quick_frame_ = current_fragment->GetTopQuickFrame();
    cur_quick_frame_pc_ = current_fragment->GetTopQuickFramePc();

    if (cur_quick_frame_ != NULL) {  // Handle quick stack frames.
      // Can't be both a shadow and a quick fragment.
      DCHECK(current_fragment->GetTopShadowFrame() == NULL);
      mirror::ArtMethod* method = cur_quick_frame_->AsMirrorPtr();
      while (method != NULL) {
        SanityCheckFrame();
        bool should_continue = VisitFrame();
        if (UNLIKELY(!should_continue)) {
          return;
        }

        if (context_ != NULL) {
          context_->FillCalleeSaves(*this);
        }
        size_t frame_size = method->GetFrameSizeInBytes();
        // Compute PC for next stack frame from return PC.
        size_t return_pc_offset = method->GetReturnPcOffsetInBytes(frame_size);
        byte* return_pc_addr = reinterpret_cast<byte*>(cur_quick_frame_) + return_pc_offset;
        uintptr_t return_pc = *reinterpret_cast<uintptr_t*>(return_pc_addr);
        if (UNLIKELY(exit_stubs_installed)) {
          // While profiling, the return pc is restored from the side stack, except when walking
          // the stack for an exception where the side stack will be unwound in VisitFrame.
          if (GetQuickInstrumentationExitPc() == return_pc) {
            const instrumentation::InstrumentationStackFrame& instrumentation_frame =
                GetInstrumentationStackFrame(thread_, instrumentation_stack_depth);
            instrumentation_stack_depth++;
            if (GetMethod() == Runtime::Current()->GetCalleeSaveMethod(Runtime::kSaveAll)) {
              // Skip runtime save all callee frames which are used to deliver exceptions.
            } else if (instrumentation_frame.interpreter_entry_) {
              mirror::ArtMethod* callee = Runtime::Current()->GetCalleeSaveMethod(Runtime::kRefsAndArgs);
              CHECK_EQ(GetMethod(), callee) << "Expected: " << PrettyMethod(callee) << " Found: "
                                            << PrettyMethod(GetMethod());
            } else if (instrumentation_frame.method_ != GetMethod()) {
              LOG(FATAL)  << "Expected: " << PrettyMethod(instrumentation_frame.method_)
                          << " Found: " << PrettyMethod(GetMethod());
            }
            if (num_frames_ != 0) {
              // Check agreement of frame Ids only if num_frames_ is computed to avoid infinite
              // recursion.
              CHECK(instrumentation_frame.frame_id_ == GetFrameId())
                    << "Expected: " << instrumentation_frame.frame_id_
                    << " Found: " << GetFrameId();
            }
            return_pc = instrumentation_frame.return_pc_;
          }
        }
        cur_quick_frame_pc_ = return_pc;
        byte* next_frame = reinterpret_cast<byte*>(cur_quick_frame_) + frame_size;
        cur_quick_frame_ = reinterpret_cast<StackReference<mirror::ArtMethod>*>(next_frame);
        cur_depth_++;
        method = cur_quick_frame_->AsMirrorPtr();
      }
    } else if (cur_shadow_frame_ != NULL) {
      do {
        SanityCheckFrame();
        bool should_continue = VisitFrame();
        if (UNLIKELY(!should_continue)) {
          return;
        }
        cur_depth_++;
        cur_shadow_frame_ = cur_shadow_frame_->GetLink();
      } while (cur_shadow_frame_ != NULL);
    }
    if (include_transitions) {
      bool should_continue = VisitFrame();
      if (!should_continue) {
        return;
      }
    }
    cur_depth_++;
  }
  if (num_frames_ != 0) {
    CHECK_EQ(cur_depth_, num_frames_);
  }
}

}  // namespace art
