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

#include "argument_visitor.h"
#include "callee_save_frame.h"
#include "dex_file-inl.h"
#include "interpreter/interpreter.h"
#include "invoke_arg_array_builder.h"
#include "mirror/abstract_method-inl.h"
#include "mirror/class-inl.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "object_utils.h"

namespace art {

// Visits arguments on the stack placing them into the shadow frame.
class BuildShadowFrameVisitor : public QuickArgumentVisitor {
 public:
  BuildShadowFrameVisitor(MethodHelper& caller_mh, mirror::AbstractMethod** sp,
                          ShadowFrame& sf, size_t first_arg_reg) :
    QuickArgumentVisitor(caller_mh, sp), sf_(sf), cur_reg_(first_arg_reg) {}

  virtual void Visit() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    Primitive::Type type = GetParamPrimitiveType();
    switch (type) {
      case Primitive::kPrimLong:  // Fall-through.
      case Primitive::kPrimDouble:
        if (IsSplitLongOrDouble()) {
          sf_.SetVRegLong(cur_reg_, ReadSplitLongParam());
        } else {
          sf_.SetVRegLong(cur_reg_, *reinterpret_cast<jlong*>(GetParamAddress()));
        }
        ++cur_reg_;
        break;
      case Primitive::kPrimNot:
        sf_.SetVRegReference(cur_reg_, *reinterpret_cast<mirror::Object**>(GetParamAddress()));
        break;
      case Primitive::kPrimBoolean:  // Fall-through.
      case Primitive::kPrimByte:     // Fall-through.
      case Primitive::kPrimChar:     // Fall-through.
      case Primitive::kPrimShort:    // Fall-through.
      case Primitive::kPrimInt:      // Fall-through.
      case Primitive::kPrimFloat:
        sf_.SetVReg(cur_reg_, *reinterpret_cast<jint*>(GetParamAddress()));
        break;
      case Primitive::kPrimVoid:
        LOG(FATAL) << "UNREACHABLE";
        break;
    }
    ++cur_reg_;
  }

 private:
  ShadowFrame& sf_;
  size_t cur_reg_;

  DISALLOW_COPY_AND_ASSIGN(BuildShadowFrameVisitor);
};

extern "C" uint64_t artInterpreterEntry(mirror::AbstractMethod* method, Thread* self,
                                        mirror::AbstractMethod** sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  // Ensure we don't get thread suspension until the object arguments are safely in the shadow
  // frame.
  const char* old_cause = self->StartAssertNoThreadSuspension("Building interpreter shadow frame");
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsAndArgs);

  MethodHelper mh(method);
  const DexFile::CodeItem* code_item = mh.GetCodeItem();
  uint16_t num_regs = code_item->registers_size_;
  void* memory = alloca(ShadowFrame::ComputeSize(num_regs));
  ShadowFrame* shadow_frame(ShadowFrame::Create(num_regs, NULL, // No last shadow coming from quick.
                                                method, 0, memory));
  size_t first_arg_reg = code_item->registers_size_ - code_item->ins_size_;
  BuildShadowFrameVisitor shadow_frame_builder(mh, sp, *shadow_frame, first_arg_reg);
  shadow_frame_builder.VisitArguments();
  // Push a transition back into managed code onto the linked list in thread.
  ManagedStack fragment;
  self->PushManagedStackFragment(&fragment);
  self->PushShadowFrame(shadow_frame);
  self->EndAssertNoThreadSuspension(old_cause);

  if (method->IsStatic() && !method->GetDeclaringClass()->IsInitializing()) {
    // Ensure static method's class is initialized.
    if (!Runtime::Current()->GetClassLinker()->EnsureInitialized(method->GetDeclaringClass(),
                                                                 true, true)) {
      DCHECK(Thread::Current()->IsExceptionPending());
      self->PopManagedStackFragment(fragment);
      return 0;
    }
  }

  JValue result = interpreter::EnterInterpreterFromStub(self, mh, code_item, *shadow_frame);
  // Pop transition.
  self->PopManagedStackFragment(fragment);
  return result.GetJ();
}

extern "C" void artInterpreterToQuickEntry(Thread* self, MethodHelper& mh,
                                           const DexFile::CodeItem* code_item,
                                           ShadowFrame* shadow_frame, JValue* result)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::AbstractMethod* method = shadow_frame->GetMethod();
  uint16_t arg_offset = (code_item == NULL) ? 0 : code_item->registers_size_ - code_item->ins_size_;
  ArgArray arg_array(mh.GetShorty(), mh.GetShortyLength());
  arg_array.BuildArgArray(shadow_frame, arg_offset);
  method->Invoke(self, arg_array.GetArray(), arg_array.GetNumBytes(), result, mh.GetShorty()[0]);
}

}  // namespace art
