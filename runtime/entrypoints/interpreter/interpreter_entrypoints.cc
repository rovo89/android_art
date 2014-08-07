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

#include "class_linker.h"
#include "interpreter/interpreter.h"
#include "mirror/art_method-inl.h"
#include "mirror/object-inl.h"
#include "reflection.h"
#include "runtime.h"
#include "stack.h"

namespace art {

// TODO: Make the MethodHelper here be compaction safe.
extern "C" void artInterpreterToCompiledCodeBridge(Thread* self, MethodHelper& mh,
                                                   const DexFile::CodeItem* code_item,
                                                   ShadowFrame* shadow_frame, JValue* result) {
  mirror::ArtMethod* method = shadow_frame->GetMethod();
  // Ensure static methods are initialized.
  if (method->IsStatic()) {
    mirror::Class* declaringClass = method->GetDeclaringClass();
    if (UNLIKELY(!declaringClass->IsInitialized())) {
      self->PushShadowFrame(shadow_frame);
      StackHandleScope<1> hs(self);
      Handle<mirror::Class> h_class(hs.NewHandle(declaringClass));
      if (UNLIKELY(!Runtime::Current()->GetClassLinker()->EnsureInitialized(h_class, true, true))) {
        self->PopShadowFrame();
        DCHECK(self->IsExceptionPending());
        return;
      }
      self->PopShadowFrame();
      CHECK(h_class->IsInitializing());
      // Reload from shadow frame in case the method moved, this is faster than adding a handle.
      method = shadow_frame->GetMethod();
    }
  }
  uint16_t arg_offset = (code_item == NULL) ? 0 : code_item->registers_size_ - code_item->ins_size_;
  if (kUsePortableCompiler) {
    InvokeWithShadowFrame(self, shadow_frame, arg_offset, mh, result);
  } else {
    method->Invoke(self, shadow_frame->GetVRegArgs(arg_offset),
                   (shadow_frame->NumberOfVRegs() - arg_offset) * sizeof(uint32_t),
                   result, mh.GetShorty());
  }
}

}  // namespace art
