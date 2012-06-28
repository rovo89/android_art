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

#include "compiler.h"
#include "oat/runtime/context.h"
#include "object.h"
#include "object_utils.h"
#include "thread_list.h"

namespace art {

class StackGetter {
 public:
  StackGetter(const ScopedJniThreadState& ts, Thread* thread)
      : ts_(ts), thread_(thread), trace_(NULL) {
  }

  static void Callback(void* arg) {
    reinterpret_cast<StackGetter*>(arg)->Callback();
  }

  jobject GetTrace() {
    return trace_;
  }

 private:
  void Callback() {
    trace_ = thread_->CreateInternalStackTrace(ts_);
  }

  const ScopedJniThreadState& ts_;
  Thread* const thread_;
  jobject trace_;
};

jobject GetThreadStack(const ScopedJniThreadState& ts, Thread* thread) {
  ThreadList* thread_list = Runtime::Current()->GetThreadList();
  StackGetter stack_getter(ts, thread);
  thread_list->RunWhileSuspended(thread, StackGetter::Callback, &stack_getter);
  return stack_getter.GetTrace();
}

void ManagedStack::PushManagedStackFragment(ManagedStack* fragment) {
  // Copy this top fragment into given fragment.
  memcpy(fragment, this, sizeof(ManagedStack));
  // Clear this fragment, which has become the top.
  memset(this, 0, sizeof(ManagedStack));
  // Link our top fragment onto the given fragment.
  link_ = fragment;
}

void ManagedStack::PopManagedStackFragment(const ManagedStack& fragment) {
  DCHECK(&fragment == link_);
  // Copy this given fragment back to the top.
  memcpy(this, &fragment, sizeof(ManagedStack));
}

size_t ManagedStack::NumShadowFrameReferences() const {
  size_t count = 0;
  for (const ManagedStack* current_fragment = this; current_fragment != NULL;
       current_fragment = current_fragment->GetLink()) {
    for (ShadowFrame* current_frame = current_fragment->top_shadow_frame_; current_frame != NULL;
         current_frame = current_frame->GetLink()) {
      count += current_frame->NumberOfReferences();
    }
  }
  return count;
}

bool ManagedStack::ShadowFramesContain(Object** shadow_frame_entry) const {
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

uint32_t StackVisitor::GetDexPc() const {
  if (cur_shadow_frame_ != NULL) {
    return cur_shadow_frame_->GetDexPC();
  } else if (cur_quick_frame_ != NULL) {
    return GetMethod()->ToDexPC(AdjustQuickFramePcForDexPcComputation(cur_quick_frame_pc_));
  } else {
    return 0;
  }
}

uint32_t StackVisitor::GetVReg(Method* m, int vreg) const {
  DCHECK(context_ != NULL); // You can't reliably read registers without a context.
  DCHECK(m == GetMethod());
  uint32_t core_spills = m->GetCoreSpillMask();
  const VmapTable vmap_table(m->GetVmapTableRaw());
  uint32_t vmap_offset;
  // TODO: IsInContext stops before spotting floating point registers.
  if (vmap_table.IsInContext(vreg, vmap_offset)) {
    // Compute the register we need to load from the context.
    uint32_t spill_mask = core_spills;
    CHECK_LT(vmap_offset, static_cast<uint32_t>(__builtin_popcount(spill_mask)));
    uint32_t matches = 0;
    uint32_t spill_shifts = 0;
    while (matches != (vmap_offset + 1)) {
      DCHECK_NE(spill_mask, 0u);
      matches += spill_mask & 1;  // Add 1 if the low bit is set.
      spill_mask >>= 1;
      spill_shifts++;
    }
    spill_shifts--;  // Wind back one as we want the last match.
    return GetGPR(spill_shifts);
  } else {
    const DexFile::CodeItem* code_item = MethodHelper(m).GetCodeItem();
    DCHECK(code_item != NULL) << PrettyMethod(m); // Can't be NULL or how would we compile its instructions?
    uint32_t fp_spills = m->GetFpSpillMask();
    size_t frame_size = m->GetFrameSizeInBytes();
    return GetVReg(code_item, core_spills, fp_spills, frame_size, vreg);
  }
}

void StackVisitor::SetVReg(Method* m, int vreg, uint32_t new_value) {
  DCHECK(context_ != NULL); // You can't reliably write registers without a context.
  DCHECK(m == GetMethod());
  const VmapTable vmap_table(m->GetVmapTableRaw());
  uint32_t vmap_offset;
  // TODO: IsInContext stops before spotting floating point registers.
  if (vmap_table.IsInContext(vreg, vmap_offset)) {
    UNIMPLEMENTED(FATAL);
  }
  const DexFile::CodeItem* code_item = MethodHelper(m).GetCodeItem();
  DCHECK(code_item != NULL) << PrettyMethod(m); // Can't be NULL or how would we compile its instructions?
  uint32_t core_spills = m->GetCoreSpillMask();
  uint32_t fp_spills = m->GetFpSpillMask();
  size_t frame_size = m->GetFrameSizeInBytes();
  int offset = GetVRegOffset(code_item, core_spills, fp_spills, frame_size, vreg);
  byte* vreg_addr = reinterpret_cast<byte*>(GetCurrentQuickFrame()) + offset;
  *reinterpret_cast<uint32_t*>(vreg_addr) = new_value;
}

uintptr_t StackVisitor::GetGPR(uint32_t reg) const {
  return context_->GetGPR(reg);
}

uintptr_t StackVisitor::GetReturnPc() const {
  Method** sp = GetCurrentQuickFrame();
  CHECK(sp != NULL);
  byte* pc_addr = reinterpret_cast<byte*>(sp) + GetMethod()->GetReturnPcOffsetInBytes();
  return *reinterpret_cast<uintptr_t*>(pc_addr);
}

void StackVisitor::SetReturnPc(uintptr_t new_ret_pc) {
  Method** sp = GetCurrentQuickFrame();
  CHECK(sp != NULL);
  byte* pc_addr = reinterpret_cast<byte*>(sp) + GetMethod()->GetReturnPcOffsetInBytes();
  *reinterpret_cast<uintptr_t*>(pc_addr) = new_ret_pc;
}

size_t StackVisitor::ComputeNumFrames() const {
  struct NumFramesVisitor : public StackVisitor {
    explicit NumFramesVisitor(const ManagedStack* stack,
                              const std::vector<TraceStackFrame>* trace_stack)
        : StackVisitor(stack, trace_stack, NULL), frames(0) {}

    virtual bool VisitFrame() {
      frames++;
      return true;
    }

    size_t frames;
  };

  NumFramesVisitor visitor(stack_start_, trace_stack_);
  visitor.WalkStack(true);
  return visitor.frames;
}

void StackVisitor::SanityCheckFrame() {
#ifndef NDEBUG
  Method* method = GetMethod();
  CHECK(method->GetClass() == Method::GetMethodClass() ||
        method->GetClass() == Method::GetConstructorClass());
  if (cur_quick_frame_ != NULL) {
    method->AssertPcIsWithinCode(AdjustQuickFramePcForDexPcComputation(cur_quick_frame_pc_));
    // Frame sanity.
    size_t frame_size = method->GetFrameSizeInBytes();
    CHECK_NE(frame_size, 0u);
    CHECK_LT(frame_size, 1024u);
    size_t return_pc_offset = method->GetReturnPcOffsetInBytes();
    CHECK_LT(return_pc_offset, frame_size);
  }
#endif
}

void StackVisitor::WalkStack(bool include_transitions) {
  bool method_tracing_active = Runtime::Current()->IsMethodTracingActive();
  uint32_t trace_stack_depth = 0;
  for (const ManagedStack* current_fragment = stack_start_; current_fragment != NULL;
       current_fragment = current_fragment->GetLink()) {
    cur_shadow_frame_ = current_fragment->GetTopShadowFrame();
    cur_quick_frame_ = current_fragment->GetTopQuickFrame();
    cur_quick_frame_pc_ = current_fragment->GetTopQuickFramePc();
    if (cur_quick_frame_ != NULL) {  // Handle quick stack frames.
      // Can't be both a shadow and a quick fragment.
      DCHECK(current_fragment->GetTopShadowFrame() == NULL);
      Method* method = *cur_quick_frame_;
      do {
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
        size_t return_pc_offset = method->GetReturnPcOffsetInBytes();
        byte* return_pc_addr = reinterpret_cast<byte*>(cur_quick_frame_) + return_pc_offset;
        uintptr_t return_pc = *reinterpret_cast<uintptr_t*>(return_pc_addr);
        if (UNLIKELY(method_tracing_active)) {
          // While profiling, the return pc is restored from the side stack, except when walking
          // the stack for an exception where the side stack will be unwound in VisitFrame.
          // TODO: stop using include_transitions as a proxy for is this the catch block visitor.
          if (IsTraceExitPc(return_pc) && !include_transitions) {
            // TODO: unify trace and managed stack.
            TraceStackFrame trace_frame = GetTraceStackFrame(trace_stack_depth);
            trace_stack_depth++;
            CHECK(trace_frame.method_ == GetMethod()) << "Excepted: " << PrettyMethod(method)
                << " Found: " << PrettyMethod(GetMethod());
            return_pc = trace_frame.return_pc_;
          }
        }
        cur_quick_frame_pc_ = return_pc;
        byte* next_frame = reinterpret_cast<byte*>(cur_quick_frame_) + frame_size;
        cur_quick_frame_ = reinterpret_cast<Method**>(next_frame);
        cur_depth_++;
        method = *cur_quick_frame_;
      } while (method != NULL);
    } else if (cur_shadow_frame_ != NULL) {
      do {
        SanityCheckFrame();
        bool should_continue = VisitFrame();
        if (UNLIKELY(!should_continue)) {
          return;
        }
        cur_depth_++;
        cur_shadow_frame_ = cur_shadow_frame_->GetLink();
      } while(cur_shadow_frame_ != NULL);
    }
    cur_depth_++;
    if (include_transitions) {
      bool should_continue = VisitFrame();
      if (!should_continue) {
        return;
      }
    }
  }
}

}  // namespace art
