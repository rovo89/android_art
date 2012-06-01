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
#include "object.h"
#include "object_utils.h"
#include "thread_list.h"

namespace art {

bool Frame::HasMethod() const {
  return GetMethod() != NULL && (!GetMethod()->IsCalleeSaveMethod());
}

void Frame::Next() {
#if defined(ART_USE_LLVM_COMPILER)
  LOG(FATAL) << "LLVM compiler don't support this function";
#else
  size_t frame_size = GetMethod()->GetFrameSizeInBytes();
  DCHECK_NE(frame_size, 0u);
  DCHECK_LT(frame_size, 1024u);
  byte* next_sp = reinterpret_cast<byte*>(sp_) + frame_size;
  sp_ = reinterpret_cast<Method**>(next_sp);
  if (*sp_ != NULL) {
    DCHECK((*sp_)->GetClass() == Method::GetMethodClass() ||
        (*sp_)->GetClass() == Method::GetConstructorClass());
  }
#endif
}

uintptr_t Frame::GetReturnPC() const {
#if defined(ART_USE_LLVM_COMPILER)
  LOG(FATAL) << "LLVM compiler don't support this function";
  return 0;
#else
  byte* pc_addr = reinterpret_cast<byte*>(sp_) + GetMethod()->GetReturnPcOffsetInBytes();
  return *reinterpret_cast<uintptr_t*>(pc_addr);
#endif
}

void Frame::SetReturnPC(uintptr_t pc) {
#if defined(ART_USE_LLVM_COMPILER)
  LOG(FATAL) << "LLVM compiler don't support this function";
#else
  byte* pc_addr = reinterpret_cast<byte*>(sp_) + GetMethod()->GetReturnPcOffsetInBytes();
  *reinterpret_cast<uintptr_t*>(pc_addr) = pc;
#endif
}

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
int Frame::GetVRegOffset(const DexFile::CodeItem* code_item,
                         uint32_t core_spills, uint32_t fp_spills,
                         size_t frame_size, int reg) {
  DCHECK_EQ(frame_size & (kStackAlignment - 1), 0U);
  int num_spills = __builtin_popcount(core_spills) + __builtin_popcount(fp_spills) + 1 /* filler */;
  int num_ins = code_item->ins_size_;
  int num_regs = code_item->registers_size_ - num_ins;
  int locals_start = frame_size - ((num_spills + num_regs) * sizeof(uint32_t));
  if (reg == -2) {
    return 0;  // Method*
  } else if (reg <= -3) {
    return locals_start - ((reg + 1) * sizeof(uint32_t));  // Compiler temp
  } else if (reg < num_regs) {
    return locals_start + (reg * sizeof(uint32_t));        // Dalvik local reg
  } else {
    return frame_size + ((reg - num_regs) * sizeof(uint32_t)) + sizeof(uint32_t); // Dalvik in
  }
}

uint32_t Frame::GetVReg(const DexFile::CodeItem* code_item, uint32_t core_spills,
                        uint32_t fp_spills, size_t frame_size, int vreg) const {
#if defined(ART_USE_LLVM_COMPILER)
  LOG(FATAL) << "LLVM compiler don't support this function";
  return 0;
#else
  int offset = GetVRegOffset(code_item, core_spills, fp_spills, frame_size, vreg);
  byte* vreg_addr = reinterpret_cast<byte*>(sp_) + offset;
  return *reinterpret_cast<uint32_t*>(vreg_addr);
#endif
}

uint32_t Frame::GetVReg(Method* m, int vreg) const {
#if defined(ART_USE_LLVM_COMPILER)
  LOG(FATAL) << "LLVM compiler don't support this function";
  return 0;
#else
  DCHECK(m == GetMethod());
  const DexFile::CodeItem* code_item = MethodHelper(m).GetCodeItem();
  DCHECK(code_item != NULL);  // can't be NULL or how would we compile its instructions?
  uint32_t core_spills = m->GetCoreSpillMask();
  uint32_t fp_spills = m->GetFpSpillMask();
  size_t frame_size = m->GetFrameSizeInBytes();
  return GetVReg(code_item, core_spills, fp_spills, frame_size, vreg);
#endif
}

void Frame::SetVReg(Method* m, int vreg, uint32_t new_value) {
#if defined(ART_USE_LLVM_COMPILER)
  LOG(FATAL) << "LLVM compiler don't support this function";
#else
  DCHECK(m == GetMethod());
  const DexFile::CodeItem* code_item = MethodHelper(m).GetCodeItem();
  DCHECK(code_item != NULL);  // can't be NULL or how would we compile its instructions?
  uint32_t core_spills = m->GetCoreSpillMask();
  uint32_t fp_spills = m->GetFpSpillMask();
  size_t frame_size = m->GetFrameSizeInBytes();
  int offset = GetVRegOffset(code_item, core_spills, fp_spills, frame_size, vreg);
  byte* vreg_addr = reinterpret_cast<byte*>(sp_) + offset;
  *reinterpret_cast<uint32_t*>(vreg_addr) = new_value;
#endif
}

uintptr_t Frame::LoadCalleeSave(int num) const {
#if defined(ART_USE_LLVM_COMPILER)
  LOG(FATAL) << "LLVM compiler don't support this function";
  return 0;
#else
  // Callee saves are held at the top of the frame
  Method* method = GetMethod();
  DCHECK(method != NULL);
  size_t frame_size = method->GetFrameSizeInBytes();
  byte* save_addr = reinterpret_cast<byte*>(sp_) + frame_size - ((num + 1) * kPointerSize);
#if defined(__i386__)
  save_addr -= kPointerSize;  // account for return address
#endif
  return *reinterpret_cast<uintptr_t*>(save_addr);
#endif
}

Method* Frame::NextMethod() const {
#if defined(ART_USE_LLVM_COMPILER)
  LOG(FATAL) << "LLVM compiler don't support this function";
  return NULL;
#else
  byte* next_sp = reinterpret_cast<byte*>(sp_) + GetMethod()->GetFrameSizeInBytes();
  return *reinterpret_cast<Method**>(next_sp);
#endif
}

class StackGetter {
 public:
  StackGetter(JNIEnv* env, Thread* thread) : env_(env), thread_(thread), trace_(NULL) {
  }

  static void Callback(void* arg) {
    reinterpret_cast<StackGetter*>(arg)->Callback();
  }

  jobject GetTrace() {
    return trace_;
  }

 private:
  void Callback() {
    trace_ = thread_->CreateInternalStackTrace(env_);
  }

  JNIEnv* env_;
  Thread* thread_;
  jobject trace_;
};

jobject GetThreadStack(JNIEnv* env, Thread* thread) {
  ThreadList* thread_list = Runtime::Current()->GetThreadList();
  StackGetter stack_getter(env, thread);
  thread_list->RunWhileSuspended(thread, StackGetter::Callback, &stack_getter);
  return stack_getter.GetTrace();
}

}  // namespace art
