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

#ifndef ART_SRC_STACK_H_
#define ART_SRC_STACK_H_

#include "dex_file.h"
#include "heap.h"
#include "jni.h"
#include "macros.h"
#include "oat/runtime/context.h"
#include "trace.h"

#include <stdint.h>

namespace art {

class Method;
class Object;
class ShadowFrame;
class StackIndirectReferenceTable;
class ScopedObjectAccess;
class Thread;

class ShadowFrame {
 public:
  // Number of references contained within this shadow frame
  uint32_t NumberOfReferences() const {
    return number_of_references_;
  }

  void SetNumberOfReferences(uint32_t number_of_references) {
    number_of_references_ = number_of_references;
  }

  // Caller dex pc
  uint32_t GetDexPC() const {
    return dex_pc_;
  }

  void SetDexPC(uint32_t dex_pc) {
    dex_pc_ = dex_pc;
  }

  // Link to previous shadow frame or NULL
  ShadowFrame* GetLink() const {
    return link_;
  }

  void SetLink(ShadowFrame* frame) {
    DCHECK_NE(this, frame);
    link_ = frame;
  }

  Object* GetReference(size_t i) const {
    DCHECK_LT(i, number_of_references_);
    return references_[i];
  }

  void SetReference(size_t i, Object* object) {
    DCHECK_LT(i, number_of_references_);
    references_[i] = object;
  }

  Method* GetMethod() const {
    DCHECK_NE(method_, static_cast<void*>(NULL));
    return method_;
  }

  void SetMethod(Method* method) {
    DCHECK_NE(method, static_cast<void*>(NULL));
    method_ = method;
  }

  bool Contains(Object** shadow_frame_entry) const {
    return ((&references_[0] <= shadow_frame_entry) &&
            (shadow_frame_entry <= (&references_[number_of_references_ - 1])));
  }

  void VisitRoots(Heap::RootVisitor* visitor, void* arg) {
    size_t num_refs = NumberOfReferences();
    for (size_t j = 0; j < num_refs; j++) {
      Object* object = GetReference(j);
      if (object != NULL) {
        visitor(object, arg);
      }
    }
  }

  // Offset of link within shadow frame
  static size_t LinkOffset() {
    return OFFSETOF_MEMBER(ShadowFrame, link_);
  }

  // Offset of method within shadow frame
  static size_t MethodOffset() {
    return OFFSETOF_MEMBER(ShadowFrame, method_);
  }

  // Offset of dex pc within shadow frame
  static size_t DexPCOffset() {
    return OFFSETOF_MEMBER(ShadowFrame, dex_pc_);
  }

  // Offset of length within shadow frame
  static size_t NumberOfReferencesOffset() {
    return OFFSETOF_MEMBER(ShadowFrame, number_of_references_);
  }

  // Offset of references within shadow frame
  static size_t ReferencesOffset() {
    return OFFSETOF_MEMBER(ShadowFrame, references_);
  }

 private:
  // ShadowFrame should be allocated by the generated code directly.
  // We should not create new shadow stack in the runtime support function.
  ~ShadowFrame() {}

  uint32_t number_of_references_;
  ShadowFrame* link_;
  Method* method_;
  uint32_t dex_pc_;
  Object* references_[];

  DISALLOW_IMPLICIT_CONSTRUCTORS(ShadowFrame);
};

// The managed stack is used to record fragments of managed code stacks. Managed code stacks
// may either be shadow frames or lists of frames using fixed frame sizes. Transition records are
// necessary for transitions between code using different frame layouts and transitions into native
// code.
class PACKED ManagedStack {
 public:
  ManagedStack()
      : link_(NULL), top_shadow_frame_(NULL), top_quick_frame_(NULL), top_quick_frame_pc_(0) {}
  void PushManagedStackFragment(ManagedStack* fragment);
  void PopManagedStackFragment(const ManagedStack& record);

  ManagedStack* GetLink() const {
    return link_;
  }

  Method** GetTopQuickFrame() const {
    return top_quick_frame_;
  }

  void SetTopQuickFrame(Method** top) {
    top_quick_frame_ = top;
  }

  uintptr_t GetTopQuickFramePc() const {
    return top_quick_frame_pc_;
  }

  void SetTopQuickFramePc(uintptr_t pc) {
    top_quick_frame_pc_ = pc;
  }

  static size_t TopQuickFrameOffset() {
    return OFFSETOF_MEMBER(ManagedStack, top_quick_frame_);
  }

  static size_t TopQuickFramePcOffset() {
    return OFFSETOF_MEMBER(ManagedStack, top_quick_frame_pc_);
  }

  ShadowFrame* PushShadowFrame(ShadowFrame* new_top_frame) {
    ShadowFrame* old_frame = top_shadow_frame_;
    top_shadow_frame_ = new_top_frame;
    new_top_frame->SetLink(old_frame);
    return old_frame;
  }

  ShadowFrame* PopShadowFrame() {
    CHECK(top_shadow_frame_ != NULL);
    ShadowFrame* frame = top_shadow_frame_;
    top_shadow_frame_ = frame->GetLink();
    return frame;
  }

  ShadowFrame* GetTopShadowFrame() const {
    return top_shadow_frame_;
  }

  static size_t TopShadowFrameOffset() {
    return OFFSETOF_MEMBER(ManagedStack, top_shadow_frame_);
  }

  size_t NumShadowFrameReferences() const;

  bool ShadowFramesContain(Object** shadow_frame_entry) const;

 private:
  ManagedStack* link_;
  ShadowFrame* top_shadow_frame_;
  Method** top_quick_frame_;
  uintptr_t top_quick_frame_pc_;
};

class StackVisitor {
 protected:
  StackVisitor(const ManagedStack* stack, const std::vector<TraceStackFrame>* trace_stack,
               Context* context)
      SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_)
      : stack_start_(stack), trace_stack_(trace_stack), cur_shadow_frame_(NULL),
        cur_quick_frame_(NULL), cur_quick_frame_pc_(0), num_frames_(0), cur_depth_(0),
        context_(context) {}

 public:
  virtual ~StackVisitor() {}

  // Return 'true' if we should continue to visit more frames, 'false' to stop.
  virtual bool VisitFrame() SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_) = 0;

  void WalkStack(bool include_transitions = false)
      SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_);

  Method* GetMethod() const {
    if (cur_shadow_frame_ != NULL) {
      return cur_shadow_frame_->GetMethod();
    } else if (cur_quick_frame_ != NULL) {
      return *cur_quick_frame_;
    } else {
      return NULL;
    }
  }

  bool IsShadowFrame() const {
    return cur_shadow_frame_ != NULL;
  }

  uintptr_t LoadCalleeSave(int num, size_t frame_size) const {
    // Callee saves are held at the top of the frame
    Method* method = GetMethod();
    DCHECK(method != NULL);
    byte* save_addr =
        reinterpret_cast<byte*>(cur_quick_frame_) + frame_size - ((num + 1) * kPointerSize);
#if defined(__i386__)
    save_addr -= kPointerSize;  // account for return address
#endif
    return *reinterpret_cast<uintptr_t*>(save_addr);
  }

  uint32_t GetDexPc() const SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_);

  // Returns the height of the stack in the managed stack frames, including transitions.
  size_t GetFrameHeight() SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_) {
    return GetNumFrames() - cur_depth_;
  }

  // Returns a frame ID for JDWP use, starting from 1.
  size_t GetFrameId() SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_) {
    return GetFrameHeight() + 1;
  }

  size_t GetNumFrames() SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_) {
    if (num_frames_ == 0) {
      num_frames_ = ComputeNumFrames();
    }
    return num_frames_;
  }

  uint32_t GetVReg(Method* m, int vreg) const
      SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_);

  void SetVReg(Method* m, int vreg, uint32_t new_value)
      SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_);

  uintptr_t GetGPR(uint32_t reg) const;

  uint32_t GetVReg(Method** cur_quick_frame, const DexFile::CodeItem* code_item,
                   uint32_t core_spills, uint32_t fp_spills, size_t frame_size, int vreg) const {
    int offset = GetVRegOffset(code_item, core_spills, fp_spills, frame_size, vreg);
    DCHECK_EQ(cur_quick_frame, GetCurrentQuickFrame());
    byte* vreg_addr = reinterpret_cast<byte*>(cur_quick_frame) + offset;
    return *reinterpret_cast<uint32_t*>(vreg_addr);
  }

  uintptr_t GetReturnPc() const;

  void SetReturnPc(uintptr_t new_ret_pc);

  /*
   * Return sp-relative offset for a Dalvik virtual register, compiler
   * spill or Method* in bytes using Method*.
   * Note that (reg >= 0) refers to a Dalvik register, (reg == -2)
   * denotes Method* and (reg <= -3) denotes a compiler temp.
   *
   *     +------------------------+
   *     | IN[ins-1]              |  {Note: resides in caller's frame}
   *     |       .                |
   *     | IN[0]                  |
   *     | caller's Method*       |
   *     +========================+  {Note: start of callee's frame}
   *     | core callee-save spill |  {variable sized}
   *     +------------------------+
   *     | fp callee-save spill   |
   *     +------------------------+
   *     | filler word            |  {For compatibility, if V[locals-1] used as wide
   *     +------------------------+
   *     | V[locals-1]            |
   *     | V[locals-2]            |
   *     |      .                 |
   *     |      .                 |  ... (reg == 2)
   *     | V[1]                   |  ... (reg == 1)
   *     | V[0]                   |  ... (reg == 0) <---- "locals_start"
   *     +------------------------+
   *     | Compiler temps         |  ... (reg == -2)
   *     |                        |  ... (reg == -3)
   *     |                        |  ... (reg == -4)
   *     +------------------------+
   *     | stack alignment padding|  {0 to (kStackAlignWords-1) of padding}
   *     +------------------------+
   *     | OUT[outs-1]            |
   *     | OUT[outs-2]            |
   *     |       .                |
   *     | OUT[0]                 |
   *     | curMethod*             |  ... (reg == -1) <<== sp, 16-byte aligned
   *     +========================+
   */
  static int GetVRegOffset(const DexFile::CodeItem* code_item,
                    uint32_t core_spills, uint32_t fp_spills,
                    size_t frame_size, int reg) {
    DCHECK_EQ(frame_size & (kStackAlignment - 1), 0U);
    int num_spills = __builtin_popcount(core_spills) + __builtin_popcount(fp_spills) + 1; // Filler.
    int num_ins = code_item->ins_size_;
    int num_regs = code_item->registers_size_ - num_ins;
    int locals_start = frame_size - ((num_spills + num_regs) * sizeof(uint32_t));
    if (reg == -2) {
      return 0;  // Method*
    } else if (reg <= -3) {
      return locals_start - ((reg + 1) * sizeof(uint32_t));  // Compiler temp.
    } else if (reg < num_regs) {
      return locals_start + (reg * sizeof(uint32_t));        // Dalvik local reg.
    } else {
      return frame_size + ((reg - num_regs) * sizeof(uint32_t)) + sizeof(uint32_t); // Dalvik in.
    }
  }

  uintptr_t GetCurrentQuickFramePc() const {
    return cur_quick_frame_pc_;
  }

  Method** GetCurrentQuickFrame() const {
    return cur_quick_frame_;
  }

  ShadowFrame* GetCurrentShadowFrame() const {
    return cur_shadow_frame_;
  }

  StackIndirectReferenceTable* GetCurrentSirt() const {
    Method** sp = GetCurrentQuickFrame();
    ++sp; // Skip Method*; SIRT comes next;
    return reinterpret_cast<StackIndirectReferenceTable*>(sp);
  }

 private:
  size_t ComputeNumFrames() const SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_);

  TraceStackFrame GetTraceStackFrame(uint32_t depth) const {
    return trace_stack_->at(trace_stack_->size() - depth - 1);
  }

  void SanityCheckFrame() const SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_);

  const ManagedStack* const stack_start_;
  const std::vector<TraceStackFrame>* const trace_stack_;
  ShadowFrame* cur_shadow_frame_;
  Method** cur_quick_frame_;
  uintptr_t cur_quick_frame_pc_;
  // Lazily computed, number of frames in the stack.
  size_t num_frames_;
  // Depth of the frame we're currently at.
  size_t cur_depth_;
 protected:
  Context* const context_;
};

static inline uintptr_t AdjustQuickFramePcForDexPcComputation(uintptr_t pc) {
  // Quick methods record a mapping from quick PCs to Dex PCs at the beginning of the code for
  // each dex instruction. When walking the stack, the return PC will be set to the instruction
  // following call which will likely be the start of the next dex instruction. Adjust the PC
  // for these cases by 2 bytes in case the return PC also has the thumb bit set.
  if (pc > 0) { pc -= 2; }
  return pc;
}

}  // namespace art

#endif  // ART_SRC_STACK_H_
