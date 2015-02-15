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

#include "callee_save_frame.h"
#include "common_throws.h"
#include "dex_file-inl.h"
#include "dex_instruction-inl.h"
#include "entrypoints/entrypoint_utils-inl.h"
#include "gc/accounting/card_table-inl.h"
#include "instruction_set.h"
#include "interpreter/interpreter.h"
#include "mirror/art_method-inl.h"
#include "mirror/class-inl.h"
#include "mirror/dex_cache-inl.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "runtime.h"
#include "scoped_thread_state_change.h"

namespace art {

// Visits the arguments as saved to the stack by a Runtime::kRefAndArgs callee save frame.
class QuickArgumentVisitor {
  // Number of bytes for each out register in the caller method's frame.
  static constexpr size_t kBytesStackArgLocation = 4;
  // Frame size in bytes of a callee-save frame for RefsAndArgs.
  static constexpr size_t kQuickCalleeSaveFrame_RefAndArgs_FrameSize =
      GetCalleeSaveFrameSize(kRuntimeISA, Runtime::kRefsAndArgs);
#if defined(__arm__)
  // The callee save frame is pointed to by SP.
  // | argN       |  |
  // | ...        |  |
  // | arg4       |  |
  // | arg3 spill |  |  Caller's frame
  // | arg2 spill |  |
  // | arg1 spill |  |
  // | Method*    | ---
  // | LR         |
  // | ...        |    callee saves
  // | R3         |    arg3
  // | R2         |    arg2
  // | R1         |    arg1
  // | R0         |    padding
  // | Method*    |  <- sp
  static constexpr bool kQuickSoftFloatAbi = true;  // This is a soft float ABI.
  static constexpr size_t kNumQuickGprArgs = 3;  // 3 arguments passed in GPRs.
  static constexpr size_t kNumQuickFprArgs = 0;  // 0 arguments passed in FPRs.
  static constexpr size_t kQuickCalleeSaveFrame_RefAndArgs_Fpr1Offset =
      arm::ArmCalleeSaveFpr1Offset(Runtime::kRefsAndArgs);  // Offset of first FPR arg.
  static constexpr size_t kQuickCalleeSaveFrame_RefAndArgs_Gpr1Offset =
      arm::ArmCalleeSaveGpr1Offset(Runtime::kRefsAndArgs);  // Offset of first GPR arg.
  static constexpr size_t kQuickCalleeSaveFrame_RefAndArgs_LrOffset =
      arm::ArmCalleeSaveLrOffset(Runtime::kRefsAndArgs);  // Offset of return address.
  static size_t GprIndexToGprOffset(uint32_t gpr_index) {
    return gpr_index * GetBytesPerGprSpillLocation(kRuntimeISA);
  }
#elif defined(__aarch64__)
  // The callee save frame is pointed to by SP.
  // | argN       |  |
  // | ...        |  |
  // | arg4       |  |
  // | arg3 spill |  |  Caller's frame
  // | arg2 spill |  |
  // | arg1 spill |  |
  // | Method*    | ---
  // | LR         |
  // | X29        |
  // |  :         |
  // | X20        |
  // | X7         |
  // | :          |
  // | X1         |
  // | D7         |
  // |  :         |
  // | D0         |
  // |            |    padding
  // | Method*    |  <- sp
  static constexpr bool kQuickSoftFloatAbi = false;  // This is a hard float ABI.
  static constexpr size_t kNumQuickGprArgs = 7;  // 7 arguments passed in GPRs.
  static constexpr size_t kNumQuickFprArgs = 8;  // 8 arguments passed in FPRs.
  static constexpr size_t kQuickCalleeSaveFrame_RefAndArgs_Fpr1Offset =
      arm64::Arm64CalleeSaveFpr1Offset(Runtime::kRefsAndArgs);  // Offset of first FPR arg.
  static constexpr size_t kQuickCalleeSaveFrame_RefAndArgs_Gpr1Offset =
      arm64::Arm64CalleeSaveGpr1Offset(Runtime::kRefsAndArgs);  // Offset of first GPR arg.
  static constexpr size_t kQuickCalleeSaveFrame_RefAndArgs_LrOffset =
      arm64::Arm64CalleeSaveLrOffset(Runtime::kRefsAndArgs);  // Offset of return address.
  static size_t GprIndexToGprOffset(uint32_t gpr_index) {
    return gpr_index * GetBytesPerGprSpillLocation(kRuntimeISA);
  }
#elif defined(__mips__)
  // The callee save frame is pointed to by SP.
  // | argN       |  |
  // | ...        |  |
  // | arg4       |  |
  // | arg3 spill |  |  Caller's frame
  // | arg2 spill |  |
  // | arg1 spill |  |
  // | Method*    | ---
  // | RA         |
  // | ...        |    callee saves
  // | A3         |    arg3
  // | A2         |    arg2
  // | A1         |    arg1
  // | A0/Method* |  <- sp
  static constexpr bool kQuickSoftFloatAbi = true;  // This is a soft float ABI.
  static constexpr size_t kNumQuickGprArgs = 3;  // 3 arguments passed in GPRs.
  static constexpr size_t kNumQuickFprArgs = 0;  // 0 arguments passed in FPRs.
  static constexpr size_t kQuickCalleeSaveFrame_RefAndArgs_Fpr1Offset = 0;  // Offset of first FPR arg.
  static constexpr size_t kQuickCalleeSaveFrame_RefAndArgs_Gpr1Offset = 4;  // Offset of first GPR arg.
  static constexpr size_t kQuickCalleeSaveFrame_RefAndArgs_LrOffset = 60;  // Offset of return address.
  static size_t GprIndexToGprOffset(uint32_t gpr_index) {
    return gpr_index * GetBytesPerGprSpillLocation(kRuntimeISA);
  }
#elif defined(__i386__)
  // The callee save frame is pointed to by SP.
  // | argN        |  |
  // | ...         |  |
  // | arg4        |  |
  // | arg3 spill  |  |  Caller's frame
  // | arg2 spill  |  |
  // | arg1 spill  |  |
  // | Method*     | ---
  // | Return      |
  // | EBP,ESI,EDI |    callee saves
  // | EBX         |    arg3
  // | EDX         |    arg2
  // | ECX         |    arg1
  // | EAX/Method* |  <- sp
  static constexpr bool kQuickSoftFloatAbi = true;  // This is a soft float ABI.
  static constexpr size_t kNumQuickGprArgs = 3;  // 3 arguments passed in GPRs.
  static constexpr size_t kNumQuickFprArgs = 0;  // 0 arguments passed in FPRs.
  static constexpr size_t kQuickCalleeSaveFrame_RefAndArgs_Fpr1Offset = 0;  // Offset of first FPR arg.
  static constexpr size_t kQuickCalleeSaveFrame_RefAndArgs_Gpr1Offset = 4;  // Offset of first GPR arg.
  static constexpr size_t kQuickCalleeSaveFrame_RefAndArgs_LrOffset = 28;  // Offset of return address.
  static size_t GprIndexToGprOffset(uint32_t gpr_index) {
    return gpr_index * GetBytesPerGprSpillLocation(kRuntimeISA);
  }
#elif defined(__x86_64__)
  // The callee save frame is pointed to by SP.
  // | argN            |  |
  // | ...             |  |
  // | reg. arg spills |  |  Caller's frame
  // | Method*         | ---
  // | Return          |
  // | R15             |    callee save
  // | R14             |    callee save
  // | R13             |    callee save
  // | R12             |    callee save
  // | R9              |    arg5
  // | R8              |    arg4
  // | RSI/R6          |    arg1
  // | RBP/R5          |    callee save
  // | RBX/R3          |    callee save
  // | RDX/R2          |    arg2
  // | RCX/R1          |    arg3
  // | XMM7            |    float arg 8
  // | XMM6            |    float arg 7
  // | XMM5            |    float arg 6
  // | XMM4            |    float arg 5
  // | XMM3            |    float arg 4
  // | XMM2            |    float arg 3
  // | XMM1            |    float arg 2
  // | XMM0            |    float arg 1
  // | Padding         |
  // | RDI/Method*     |  <- sp
  static constexpr bool kQuickSoftFloatAbi = false;  // This is a hard float ABI.
  static constexpr size_t kNumQuickGprArgs = 5;  // 5 arguments passed in GPRs.
  static constexpr size_t kNumQuickFprArgs = 8;  // 8 arguments passed in FPRs.
  static constexpr size_t kQuickCalleeSaveFrame_RefAndArgs_Fpr1Offset = 16;  // Offset of first FPR arg.
  static constexpr size_t kQuickCalleeSaveFrame_RefAndArgs_Gpr1Offset = 80 + 4*8;  // Offset of first GPR arg.
  static constexpr size_t kQuickCalleeSaveFrame_RefAndArgs_LrOffset = 168 + 4*8;  // Offset of return address.
  static size_t GprIndexToGprOffset(uint32_t gpr_index) {
    switch (gpr_index) {
      case 0: return (4 * GetBytesPerGprSpillLocation(kRuntimeISA));
      case 1: return (1 * GetBytesPerGprSpillLocation(kRuntimeISA));
      case 2: return (0 * GetBytesPerGprSpillLocation(kRuntimeISA));
      case 3: return (5 * GetBytesPerGprSpillLocation(kRuntimeISA));
      case 4: return (6 * GetBytesPerGprSpillLocation(kRuntimeISA));
      default:
      LOG(FATAL) << "Unexpected GPR index: " << gpr_index;
      return 0;
    }
  }
#else
#error "Unsupported architecture"
#endif

 public:
  static mirror::ArtMethod* GetCallingMethod(StackReference<mirror::ArtMethod>* sp)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    DCHECK(sp->AsMirrorPtr()->IsCalleeSaveMethod());
    byte* previous_sp = reinterpret_cast<byte*>(sp) + kQuickCalleeSaveFrame_RefAndArgs_FrameSize;
    return reinterpret_cast<StackReference<mirror::ArtMethod>*>(previous_sp)->AsMirrorPtr();
  }

  // For the given quick ref and args quick frame, return the caller's PC.
  static uintptr_t GetCallingPc(StackReference<mirror::ArtMethod>* sp)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    DCHECK(sp->AsMirrorPtr()->IsCalleeSaveMethod());
    byte* lr = reinterpret_cast<byte*>(sp) + kQuickCalleeSaveFrame_RefAndArgs_LrOffset;
    return *reinterpret_cast<uintptr_t*>(lr);
  }

  QuickArgumentVisitor(StackReference<mirror::ArtMethod>* sp, bool is_static, const char* shorty,
                       uint32_t shorty_len) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) :
          is_static_(is_static), shorty_(shorty), shorty_len_(shorty_len),
          gpr_args_(reinterpret_cast<byte*>(sp) + kQuickCalleeSaveFrame_RefAndArgs_Gpr1Offset),
          fpr_args_(reinterpret_cast<byte*>(sp) + kQuickCalleeSaveFrame_RefAndArgs_Fpr1Offset),
          stack_args_(reinterpret_cast<byte*>(sp) + kQuickCalleeSaveFrame_RefAndArgs_FrameSize
                      + StackArgumentStartFromShorty(is_static, shorty, shorty_len)),
          gpr_index_(0), fpr_index_(0), stack_index_(0), cur_type_(Primitive::kPrimVoid),
          is_split_long_or_double_(false) {}

  virtual ~QuickArgumentVisitor() {}

  virtual void Visit() = 0;

  Primitive::Type GetParamPrimitiveType() const {
    return cur_type_;
  }

  byte* GetParamAddress() const {
    if (!kQuickSoftFloatAbi) {
      Primitive::Type type = GetParamPrimitiveType();
      if (UNLIKELY((type == Primitive::kPrimDouble) || (type == Primitive::kPrimFloat))) {
        if ((kNumQuickFprArgs != 0) && (fpr_index_ + 1 < kNumQuickFprArgs + 1)) {
          return fpr_args_ + (fpr_index_ * GetBytesPerFprSpillLocation(kRuntimeISA));
        }
        return stack_args_ + (stack_index_ * kBytesStackArgLocation);
      }
    }
    if (gpr_index_ < kNumQuickGprArgs) {
      return gpr_args_ + GprIndexToGprOffset(gpr_index_);
    }
    return stack_args_ + (stack_index_ * kBytesStackArgLocation);
  }

  bool IsSplitLongOrDouble() const {
    if ((GetBytesPerGprSpillLocation(kRuntimeISA) == 4) || (GetBytesPerFprSpillLocation(kRuntimeISA) == 4)) {
      return is_split_long_or_double_;
    } else {
      return false;  // An optimization for when GPR and FPRs are 64bit.
    }
  }

  bool IsParamAReference() const {
    return GetParamPrimitiveType() == Primitive::kPrimNot;
  }

  bool IsParamALongOrDouble() const {
    Primitive::Type type = GetParamPrimitiveType();
    return type == Primitive::kPrimLong || type == Primitive::kPrimDouble;
  }

  uint64_t ReadSplitLongParam() const {
    DCHECK(IsSplitLongOrDouble());
    uint64_t low_half = *reinterpret_cast<uint32_t*>(GetParamAddress());
    uint64_t high_half = *reinterpret_cast<uint32_t*>(stack_args_);
    return (low_half & 0xffffffffULL) | (high_half << 32);
  }

  void VisitArguments() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    // This implementation doesn't support reg-spill area for hard float
    // ABI targets such as x86_64 and aarch64. So, for those targets whose
    // 'kQuickSoftFloatAbi' is 'false':
    //     (a) 'stack_args_' should point to the first method's argument
    //     (b) whatever the argument type it is, the 'stack_index_' should
    //         be moved forward along with every visiting.
    gpr_index_ = 0;
    fpr_index_ = 0;
    stack_index_ = 0;
    if (!is_static_) {  // Handle this.
      cur_type_ = Primitive::kPrimNot;
      is_split_long_or_double_ = false;
      Visit();
      if (!kQuickSoftFloatAbi || kNumQuickGprArgs == 0) {
        stack_index_++;
      }
      if (kNumQuickGprArgs > 0) {
        gpr_index_++;
      }
    }
    for (uint32_t shorty_index = 1; shorty_index < shorty_len_; ++shorty_index) {
      cur_type_ = Primitive::GetType(shorty_[shorty_index]);
      switch (cur_type_) {
        case Primitive::kPrimNot:
        case Primitive::kPrimBoolean:
        case Primitive::kPrimByte:
        case Primitive::kPrimChar:
        case Primitive::kPrimShort:
        case Primitive::kPrimInt:
          is_split_long_or_double_ = false;
          Visit();
          if (!kQuickSoftFloatAbi || kNumQuickGprArgs == gpr_index_) {
            stack_index_++;
          }
          if (gpr_index_ < kNumQuickGprArgs) {
            gpr_index_++;
          }
          break;
        case Primitive::kPrimFloat:
          is_split_long_or_double_ = false;
          Visit();
          if (kQuickSoftFloatAbi) {
            if (gpr_index_ < kNumQuickGprArgs) {
              gpr_index_++;
            } else {
              stack_index_++;
            }
          } else {
            if ((kNumQuickFprArgs != 0) && (fpr_index_ + 1 < kNumQuickFprArgs + 1)) {
              fpr_index_++;
            }
            stack_index_++;
          }
          break;
        case Primitive::kPrimDouble:
        case Primitive::kPrimLong:
          if (kQuickSoftFloatAbi || (cur_type_ == Primitive::kPrimLong)) {
            is_split_long_or_double_ = (GetBytesPerGprSpillLocation(kRuntimeISA) == 4) &&
                ((gpr_index_ + 1) == kNumQuickGprArgs);
            Visit();
            if (!kQuickSoftFloatAbi || kNumQuickGprArgs == gpr_index_) {
              if (kBytesStackArgLocation == 4) {
                stack_index_+= 2;
              } else {
                CHECK_EQ(kBytesStackArgLocation, 8U);
                stack_index_++;
              }
            }
            if (gpr_index_ < kNumQuickGprArgs) {
              gpr_index_++;
              if (GetBytesPerGprSpillLocation(kRuntimeISA) == 4) {
                if (gpr_index_ < kNumQuickGprArgs) {
                  gpr_index_++;
                } else if (kQuickSoftFloatAbi) {
                  stack_index_++;
                }
              }
            }
          } else {
            is_split_long_or_double_ = (GetBytesPerFprSpillLocation(kRuntimeISA) == 4) &&
                ((fpr_index_ + 1) == kNumQuickFprArgs);
            Visit();
            if ((kNumQuickFprArgs != 0) && (fpr_index_ + 1 < kNumQuickFprArgs + 1)) {
              fpr_index_++;
              if (GetBytesPerFprSpillLocation(kRuntimeISA) == 4) {
                if ((kNumQuickFprArgs != 0) && (fpr_index_ + 1 < kNumQuickFprArgs + 1)) {
                  fpr_index_++;
                }
              }
            }
            if (kBytesStackArgLocation == 4) {
              stack_index_+= 2;
            } else {
              CHECK_EQ(kBytesStackArgLocation, 8U);
              stack_index_++;
            }
          }
          break;
        default:
          LOG(FATAL) << "Unexpected type: " << cur_type_ << " in " << shorty_;
      }
    }
  }

 private:
  static size_t StackArgumentStartFromShorty(bool is_static, const char* shorty,
                                             uint32_t shorty_len) {
    if (kQuickSoftFloatAbi) {
      CHECK_EQ(kNumQuickFprArgs, 0U);
      return (kNumQuickGprArgs * GetBytesPerGprSpillLocation(kRuntimeISA))
          + sizeof(StackReference<mirror::ArtMethod>) /* StackReference<ArtMethod> */;
    } else {
      // For now, there is no reg-spill area for the targets with
      // hard float ABI. So, the offset pointing to the first method's
      // parameter ('this' for non-static methods) should be returned.
      return sizeof(StackReference<mirror::ArtMethod>);  // Skip StackReference<ArtMethod>.
    }
  }

 protected:
  const bool is_static_;
  const char* const shorty_;
  const uint32_t shorty_len_;

 private:
  byte* const gpr_args_;  // Address of GPR arguments in callee save frame.
  byte* const fpr_args_;  // Address of FPR arguments in callee save frame.
  byte* const stack_args_;  // Address of stack arguments in caller's frame.
  uint32_t gpr_index_;  // Index into spilled GPRs.
  uint32_t fpr_index_;  // Index into spilled FPRs.
  uint32_t stack_index_;  // Index into arguments on the stack.
  // The current type of argument during VisitArguments.
  Primitive::Type cur_type_;
  // Does a 64bit parameter straddle the register and stack arguments?
  bool is_split_long_or_double_;
};

// Visits arguments on the stack placing them into the shadow frame.
class BuildQuickShadowFrameVisitor FINAL : public QuickArgumentVisitor {
 public:
  BuildQuickShadowFrameVisitor(StackReference<mirror::ArtMethod>* sp, bool is_static,
                               const char* shorty, uint32_t shorty_len, ShadowFrame* sf,
                               size_t first_arg_reg) :
      QuickArgumentVisitor(sp, is_static, shorty, shorty_len), sf_(sf), cur_reg_(first_arg_reg) {}

  void Visit() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) OVERRIDE;

 private:
  ShadowFrame* const sf_;
  uint32_t cur_reg_;

  DISALLOW_COPY_AND_ASSIGN(BuildQuickShadowFrameVisitor);
};

void BuildQuickShadowFrameVisitor::Visit() {
  Primitive::Type type = GetParamPrimitiveType();
  switch (type) {
    case Primitive::kPrimLong:  // Fall-through.
    case Primitive::kPrimDouble:
      if (IsSplitLongOrDouble()) {
        sf_->SetVRegLong(cur_reg_, ReadSplitLongParam());
      } else {
        sf_->SetVRegLong(cur_reg_, *reinterpret_cast<jlong*>(GetParamAddress()));
      }
      ++cur_reg_;
      break;
    case Primitive::kPrimNot: {
        StackReference<mirror::Object>* stack_ref =
            reinterpret_cast<StackReference<mirror::Object>*>(GetParamAddress());
        sf_->SetVRegReference(cur_reg_, stack_ref->AsMirrorPtr());
      }
      break;
    case Primitive::kPrimBoolean:  // Fall-through.
    case Primitive::kPrimByte:     // Fall-through.
    case Primitive::kPrimChar:     // Fall-through.
    case Primitive::kPrimShort:    // Fall-through.
    case Primitive::kPrimInt:      // Fall-through.
    case Primitive::kPrimFloat:
      sf_->SetVReg(cur_reg_, *reinterpret_cast<jint*>(GetParamAddress()));
      break;
    case Primitive::kPrimVoid:
      LOG(FATAL) << "UNREACHABLE";
      break;
  }
  ++cur_reg_;
}

extern "C" uint64_t artQuickToInterpreterBridge(mirror::ArtMethod* method, Thread* self,
                                                StackReference<mirror::ArtMethod>* sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  // Ensure we don't get thread suspension until the object arguments are safely in the shadow
  // frame.
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsAndArgs);

  if (method->IsAbstract()) {
    ThrowAbstractMethodError(method);
    return 0;
  } else {
    DCHECK(!method->IsNative()) << PrettyMethod(method);
    const char* old_cause = self->StartAssertNoThreadSuspension(
        "Building interpreter shadow frame");
    const DexFile::CodeItem* code_item = method->GetCodeItem();
    DCHECK(code_item != nullptr) << PrettyMethod(method);
    uint16_t num_regs = code_item->registers_size_;
    void* memory = alloca(ShadowFrame::ComputeSize(num_regs));
    // No last shadow coming from quick.
    ShadowFrame* shadow_frame(ShadowFrame::Create(num_regs, nullptr, method, 0, memory));
    size_t first_arg_reg = code_item->registers_size_ - code_item->ins_size_;
    uint32_t shorty_len = 0;
    const char* shorty = method->GetShorty(&shorty_len);
    BuildQuickShadowFrameVisitor shadow_frame_builder(sp, method->IsStatic(), shorty, shorty_len,
                                                      shadow_frame, first_arg_reg);
    shadow_frame_builder.VisitArguments();
    // Push a transition back into managed code onto the linked list in thread.
    ManagedStack fragment;
    self->PushManagedStackFragment(&fragment);
    self->PushShadowFrame(shadow_frame);
    self->EndAssertNoThreadSuspension(old_cause);

    if (method->IsStatic() && !method->GetDeclaringClass()->IsInitialized()) {
      // Ensure static method's class is initialized.
      StackHandleScope<1> hs(self);
      Handle<mirror::Class> h_class(hs.NewHandle(method->GetDeclaringClass()));
      if (!Runtime::Current()->GetClassLinker()->EnsureInitialized(h_class, true, true)) {
        DCHECK(Thread::Current()->IsExceptionPending()) << PrettyMethod(method);
        self->PopManagedStackFragment(fragment);
        return 0;
      }
    }

    StackHandleScope<1> hs(self);
    MethodHelper mh(hs.NewHandle(method));
    JValue result = interpreter::EnterInterpreterFromStub(self, mh, code_item, *shadow_frame);
    // Pop transition.
    self->PopManagedStackFragment(fragment);
    // No need to restore the args since the method has already been run by the interpreter.
    return result.GetJ();
  }
}

// Visits arguments on the stack placing them into the args vector, Object* arguments are converted
// to jobjects.
class BuildQuickArgumentVisitor FINAL : public QuickArgumentVisitor {
 public:
  BuildQuickArgumentVisitor(StackReference<mirror::ArtMethod>* sp, bool is_static,
                            const char* shorty, uint32_t shorty_len,
                            ScopedObjectAccessUnchecked* soa, std::vector<jvalue>* args) :
      QuickArgumentVisitor(sp, is_static, shorty, shorty_len), soa_(soa), args_(args) {}

  void Visit() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) OVERRIDE;

  void FixupReferences() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

 private:
  ScopedObjectAccessUnchecked* const soa_;
  std::vector<jvalue>* const args_;
  // References which we must update when exiting in case the GC moved the objects.
  std::vector<std::pair<jobject, StackReference<mirror::Object>*>> references_;

  DISALLOW_COPY_AND_ASSIGN(BuildQuickArgumentVisitor);
};

void BuildQuickArgumentVisitor::Visit() {
  jvalue val;
  Primitive::Type type = GetParamPrimitiveType();
  switch (type) {
    case Primitive::kPrimNot: {
      StackReference<mirror::Object>* stack_ref =
          reinterpret_cast<StackReference<mirror::Object>*>(GetParamAddress());
      val.l = soa_->AddLocalReference<jobject>(stack_ref->AsMirrorPtr());
      references_.push_back(std::make_pair(val.l, stack_ref));
      break;
    }
    case Primitive::kPrimLong:  // Fall-through.
    case Primitive::kPrimDouble:
      if (IsSplitLongOrDouble()) {
        val.j = ReadSplitLongParam();
      } else {
        val.j = *reinterpret_cast<jlong*>(GetParamAddress());
      }
      break;
    case Primitive::kPrimBoolean:  // Fall-through.
    case Primitive::kPrimByte:     // Fall-through.
    case Primitive::kPrimChar:     // Fall-through.
    case Primitive::kPrimShort:    // Fall-through.
    case Primitive::kPrimInt:      // Fall-through.
    case Primitive::kPrimFloat:
      val.i = *reinterpret_cast<jint*>(GetParamAddress());
      break;
    case Primitive::kPrimVoid:
      LOG(FATAL) << "UNREACHABLE";
      val.j = 0;
      break;
  }
  args_->push_back(val);
}

void BuildQuickArgumentVisitor::FixupReferences() {
  // Fixup any references which may have changed.
  for (const auto& pair : references_) {
    pair.second->Assign(soa_->Decode<mirror::Object*>(pair.first));
    soa_->Env()->DeleteLocalRef(pair.first);
  }
}

// Handler for invocation on proxy methods. On entry a frame will exist for the proxy object method
// which is responsible for recording callee save registers. We explicitly place into jobjects the
// incoming reference arguments (so they survive GC). We invoke the invocation handler, which is a
// field within the proxy object, which will box the primitive arguments and deal with error cases.
extern "C" uint64_t artQuickProxyInvokeHandler(mirror::ArtMethod* proxy_method,
                                               mirror::Object* receiver,
                                               Thread* self, StackReference<mirror::ArtMethod>* sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  DCHECK(proxy_method->IsProxyMethod()) << PrettyMethod(proxy_method);
  const bool is_xposed = proxy_method->IsXposedHookedMethod();
  const bool is_static = proxy_method->IsStatic();
  DCHECK(is_xposed || receiver->GetClass()->IsProxyClass()) << PrettyMethod(proxy_method);
  // Ensure we don't get thread suspension until the object arguments are safely in jobjects.
  const char* old_cause =
      self->StartAssertNoThreadSuspension("Adding to IRT proxy object arguments");
  // Register the top of the managed stack, making stack crawlable.
  DCHECK_EQ(sp->AsMirrorPtr(), proxy_method) << PrettyMethod(proxy_method);
  self->SetTopOfStack(sp, 0);
  DCHECK_EQ(proxy_method->GetFrameSizeInBytes(),
            Runtime::Current()->GetCalleeSaveMethod(Runtime::kRefsAndArgs)->GetFrameSizeInBytes())
      << PrettyMethod(proxy_method);
  self->VerifyStack();
  // Start new JNI local reference state.
  JNIEnvExt* env = self->GetJniEnv();
  ScopedObjectAccessUnchecked soa(env);
  ScopedJniEnvLocalRefState env_state(env);
  // Create local ref. copies of proxy method and the receiver.
  jobject rcvr_jobj = is_static ? nullptr : soa.AddLocalReference<jobject>(receiver);

  // Placing arguments into args vector and remove the receiver.
  mirror::ArtMethod* non_proxy_method = proxy_method->GetInterfaceMethodIfProxy();
  CHECK(is_xposed || !non_proxy_method->IsStatic()) << PrettyMethod(proxy_method) << " "
                                                    << PrettyMethod(non_proxy_method);
  std::vector<jvalue> args;
  uint32_t shorty_len = 0;
  const char* shorty = proxy_method->GetShorty(&shorty_len);
  BuildQuickArgumentVisitor local_ref_visitor(sp, is_static, shorty, shorty_len, &soa, &args);

  local_ref_visitor.VisitArguments();
  DCHECK_GT(args.size(), 0U) << PrettyMethod(proxy_method);
  if (!is_static) {
    args.erase(args.begin());
  }

  if (is_xposed) {
    jmethodID proxy_methodid = soa.EncodeMethod(proxy_method);
    self->EndAssertNoThreadSuspension(old_cause);
    JValue result = InvokeXposedHandleHookedMethod(soa, shorty, rcvr_jobj, proxy_methodid, args);
    local_ref_visitor.FixupReferences();
    return result.GetJ();
  }

  // Convert proxy method into expected interface method.
  mirror::ArtMethod* interface_method = proxy_method->FindOverriddenMethod();
  DCHECK(interface_method != NULL) << PrettyMethod(proxy_method);
  DCHECK(!interface_method->IsProxyMethod()) << PrettyMethod(interface_method);
  jobject interface_method_jobj = soa.AddLocalReference<jobject>(interface_method);

  // All naked Object*s should now be in jobjects, so its safe to go into the main invoke code
  // that performs allocations.
  self->EndAssertNoThreadSuspension(old_cause);
  JValue result = InvokeProxyInvocationHandler(soa, shorty, rcvr_jobj, interface_method_jobj, args);
  // Restore references which might have moved.
  local_ref_visitor.FixupReferences();
  return result.GetJ();
}

// Read object references held in arguments from quick frames and place in a JNI local references,
// so they don't get garbage collected.
class RememberForGcArgumentVisitor FINAL : public QuickArgumentVisitor {
 public:
  RememberForGcArgumentVisitor(StackReference<mirror::ArtMethod>* sp, bool is_static,
                               const char* shorty, uint32_t shorty_len,
                               ScopedObjectAccessUnchecked* soa) :
      QuickArgumentVisitor(sp, is_static, shorty, shorty_len), soa_(soa) {}

  void Visit() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) OVERRIDE;

  void FixupReferences() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

 private:
  ScopedObjectAccessUnchecked* const soa_;
  // References which we must update when exiting in case the GC moved the objects.
  std::vector<std::pair<jobject, StackReference<mirror::Object>*> > references_;

  DISALLOW_COPY_AND_ASSIGN(RememberForGcArgumentVisitor);
};

void RememberForGcArgumentVisitor::Visit() {
  if (IsParamAReference()) {
    StackReference<mirror::Object>* stack_ref =
        reinterpret_cast<StackReference<mirror::Object>*>(GetParamAddress());
    jobject reference =
        soa_->AddLocalReference<jobject>(stack_ref->AsMirrorPtr());
    references_.push_back(std::make_pair(reference, stack_ref));
  }
}

void RememberForGcArgumentVisitor::FixupReferences() {
  // Fixup any references which may have changed.
  for (const auto& pair : references_) {
    pair.second->Assign(soa_->Decode<mirror::Object*>(pair.first));
    soa_->Env()->DeleteLocalRef(pair.first);
  }
}

// Lazily resolve a method for quick. Called by stub code.
extern "C" const void* artQuickResolutionTrampoline(mirror::ArtMethod* called,
                                                    mirror::Object* receiver,
                                                    Thread* self,
                                                    StackReference<mirror::ArtMethod>* sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsAndArgs);
  // Start new JNI local reference state
  JNIEnvExt* env = self->GetJniEnv();
  ScopedObjectAccessUnchecked soa(env);
  ScopedJniEnvLocalRefState env_state(env);
  const char* old_cause = self->StartAssertNoThreadSuspension("Quick method resolution set up");

  // Compute details about the called method (avoid GCs)
  ClassLinker* linker = Runtime::Current()->GetClassLinker();
  mirror::ArtMethod* caller = QuickArgumentVisitor::GetCallingMethod(sp);
  InvokeType invoke_type;
  const DexFile* dex_file;
  uint32_t dex_method_idx;
  if (called->IsRuntimeMethod()) {
    uint32_t dex_pc = caller->ToDexPc(QuickArgumentVisitor::GetCallingPc(sp));
    const DexFile::CodeItem* code;
    dex_file = caller->GetDexFile();
    code = caller->GetCodeItem();
    CHECK_LT(dex_pc, code->insns_size_in_code_units_);
    const Instruction* instr = Instruction::At(&code->insns_[dex_pc]);
    Instruction::Code instr_code = instr->Opcode();
    bool is_range;
    switch (instr_code) {
      case Instruction::INVOKE_DIRECT:
        invoke_type = kDirect;
        is_range = false;
        break;
      case Instruction::INVOKE_DIRECT_RANGE:
        invoke_type = kDirect;
        is_range = true;
        break;
      case Instruction::INVOKE_STATIC:
        invoke_type = kStatic;
        is_range = false;
        break;
      case Instruction::INVOKE_STATIC_RANGE:
        invoke_type = kStatic;
        is_range = true;
        break;
      case Instruction::INVOKE_SUPER:
        invoke_type = kSuper;
        is_range = false;
        break;
      case Instruction::INVOKE_SUPER_RANGE:
        invoke_type = kSuper;
        is_range = true;
        break;
      case Instruction::INVOKE_VIRTUAL:
        invoke_type = kVirtual;
        is_range = false;
        break;
      case Instruction::INVOKE_VIRTUAL_RANGE:
        invoke_type = kVirtual;
        is_range = true;
        break;
      case Instruction::INVOKE_INTERFACE:
        invoke_type = kInterface;
        is_range = false;
        break;
      case Instruction::INVOKE_INTERFACE_RANGE:
        invoke_type = kInterface;
        is_range = true;
        break;
      default:
        LOG(FATAL) << "Unexpected call into trampoline: " << instr->DumpString(NULL);
        // Avoid used uninitialized warnings.
        invoke_type = kDirect;
        is_range = false;
    }
    dex_method_idx = (is_range) ? instr->VRegB_3rc() : instr->VRegB_35c();
  } else {
    invoke_type = kStatic;
    dex_file = called->GetDexFile();
    dex_method_idx = called->GetDexMethodIndex();
  }
  uint32_t shorty_len;
  const char* shorty =
      dex_file->GetMethodShorty(dex_file->GetMethodId(dex_method_idx), &shorty_len);
  RememberForGcArgumentVisitor visitor(sp, invoke_type == kStatic, shorty, shorty_len, &soa);
  visitor.VisitArguments();
  self->EndAssertNoThreadSuspension(old_cause);
  bool virtual_or_interface = invoke_type == kVirtual || invoke_type == kInterface;
  // Resolve method filling in dex cache.
  if (UNLIKELY(called->IsRuntimeMethod())) {
    StackHandleScope<1> hs(self);
    mirror::Object* dummy = nullptr;
    HandleWrapper<mirror::Object> h_receiver(
        hs.NewHandleWrapper(virtual_or_interface ? &receiver : &dummy));
    called = linker->ResolveMethod(self, dex_method_idx, &caller, invoke_type);
  }
  const void* code = NULL;
  if (LIKELY(!self->IsExceptionPending())) {
    // Incompatible class change should have been handled in resolve method.
    CHECK(!called->CheckIncompatibleClassChange(invoke_type))
        << PrettyMethod(called) << " " << invoke_type;
    if (virtual_or_interface) {
      // Refine called method based on receiver.
      CHECK(receiver != nullptr) << invoke_type;

      mirror::ArtMethod* orig_called = called;
      if (invoke_type == kVirtual) {
        called = receiver->GetClass()->FindVirtualMethodForVirtual(called);
      } else {
        called = receiver->GetClass()->FindVirtualMethodForInterface(called);
      }

      CHECK(called != nullptr) << PrettyMethod(orig_called) << " "
                               << PrettyTypeOf(receiver) << " "
                               << invoke_type << " " << orig_called->GetVtableIndex();

      // We came here because of sharpening. Ensure the dex cache is up-to-date on the method index
      // of the sharpened method.
      if (called->HasSameDexCacheResolvedMethods(caller)) {
        caller->SetDexCacheResolvedMethod(called->GetDexMethodIndex(), called);
      } else {
        // Calling from one dex file to another, need to compute the method index appropriate to
        // the caller's dex file. Since we get here only if the original called was a runtime
        // method, we've got the correct dex_file and a dex_method_idx from above.
        DCHECK_EQ(caller->GetDexFile(), dex_file);
        StackHandleScope<1> hs(self);
        MethodHelper mh(hs.NewHandle(called));
        uint32_t method_index = mh.FindDexMethodIndexInOtherDexFile(*dex_file, dex_method_idx);
        if (method_index != DexFile::kDexNoIndex) {
          caller->SetDexCacheResolvedMethod(method_index, called);
        }
      }
    }
    // Ensure that the called method's class is initialized.
    StackHandleScope<1> hs(soa.Self());
    Handle<mirror::Class> called_class(hs.NewHandle(called->GetDeclaringClass()));
    linker->EnsureInitialized(called_class, true, true);
    if (LIKELY(called_class->IsInitialized())) {
      code = called->GetEntryPointFromQuickCompiledCode();
    } else if (called_class->IsInitializing()) {
      if (invoke_type == kStatic) {
        // Class is still initializing, go to oat and grab code (trampoline must be left in place
        // until class is initialized to stop races between threads).
        code = linker->GetQuickOatCodeFor(called);
      } else {
        // No trampoline for non-static methods.
        code = called->GetEntryPointFromQuickCompiledCode();
      }
    } else {
      DCHECK(called_class->IsErroneous());
    }
  }
  CHECK_EQ(code == NULL, self->IsExceptionPending());
  // Fixup any locally saved objects may have moved during a GC.
  visitor.FixupReferences();
  // Place called method in callee-save frame to be placed as first argument to quick method.
  sp->Assign(called);
  return code;
}

/*
 * This class uses a couple of observations to unite the different calling conventions through
 * a few constants.
 *
 * 1) Number of registers used for passing is normally even, so counting down has no penalty for
 *    possible alignment.
 * 2) Known 64b architectures store 8B units on the stack, both for integral and floating point
 *    types, so using uintptr_t is OK. Also means that we can use kRegistersNeededX to denote
 *    when we have to split things
 * 3) The only soft-float, Arm, is 32b, so no widening needs to be taken into account for floats
 *    and we can use Int handling directly.
 * 4) Only 64b architectures widen, and their stack is aligned 8B anyways, so no padding code
 *    necessary when widening. Also, widening of Ints will take place implicitly, and the
 *    extension should be compatible with Aarch64, which mandates copying the available bits
 *    into LSB and leaving the rest unspecified.
 * 5) Aligning longs and doubles is necessary on arm only, and it's the same in registers and on
 *    the stack.
 * 6) There is only little endian.
 *
 *
 * Actual work is supposed to be done in a delegate of the template type. The interface is as
 * follows:
 *
 * void PushGpr(uintptr_t):   Add a value for the next GPR
 *
 * void PushFpr4(float):      Add a value for the next FPR of size 32b. Is only called if we need
 *                            padding, that is, think the architecture is 32b and aligns 64b.
 *
 * void PushFpr8(uint64_t):   Push a double. We _will_ call this on 32b, it's the callee's job to
 *                            split this if necessary. The current state will have aligned, if
 *                            necessary.
 *
 * void PushStack(uintptr_t): Push a value to the stack.
 *
 * uintptr_t PushHandleScope(mirror::Object* ref): Add a reference to the HandleScope. This _will_ have nullptr,
 *                                          as this might be important for null initialization.
 *                                          Must return the jobject, that is, the reference to the
 *                                          entry in the HandleScope (nullptr if necessary).
 *
 */
template<class T> class BuildNativeCallFrameStateMachine {
 public:
#if defined(__arm__)
  // TODO: These are all dummy values!
  static constexpr bool kNativeSoftFloatAbi = true;
  static constexpr size_t kNumNativeGprArgs = 4;  // 4 arguments passed in GPRs, r0-r3
  static constexpr size_t kNumNativeFprArgs = 0;  // 0 arguments passed in FPRs.

  static constexpr size_t kRegistersNeededForLong = 2;
  static constexpr size_t kRegistersNeededForDouble = 2;
  static constexpr bool kMultiRegistersAligned = true;
  static constexpr bool kMultiRegistersWidened = false;
  static constexpr bool kAlignLongOnStack = true;
  static constexpr bool kAlignDoubleOnStack = true;
#elif defined(__aarch64__)
  static constexpr bool kNativeSoftFloatAbi = false;  // This is a hard float ABI.
  static constexpr size_t kNumNativeGprArgs = 8;  // 6 arguments passed in GPRs.
  static constexpr size_t kNumNativeFprArgs = 8;  // 8 arguments passed in FPRs.

  static constexpr size_t kRegistersNeededForLong = 1;
  static constexpr size_t kRegistersNeededForDouble = 1;
  static constexpr bool kMultiRegistersAligned = false;
  static constexpr bool kMultiRegistersWidened = false;
  static constexpr bool kAlignLongOnStack = false;
  static constexpr bool kAlignDoubleOnStack = false;
#elif defined(__mips__)
  // TODO: These are all dummy values!
  static constexpr bool kNativeSoftFloatAbi = true;  // This is a hard float ABI.
  static constexpr size_t kNumNativeGprArgs = 0;  // 6 arguments passed in GPRs.
  static constexpr size_t kNumNativeFprArgs = 0;  // 8 arguments passed in FPRs.

  static constexpr size_t kRegistersNeededForLong = 2;
  static constexpr size_t kRegistersNeededForDouble = 2;
  static constexpr bool kMultiRegistersAligned = true;
  static constexpr bool kMultiRegistersWidened = true;
  static constexpr bool kAlignLongOnStack = false;
  static constexpr bool kAlignDoubleOnStack = false;
#elif defined(__i386__)
  // TODO: Check these!
  static constexpr bool kNativeSoftFloatAbi = false;  // Not using int registers for fp
  static constexpr size_t kNumNativeGprArgs = 0;  // 6 arguments passed in GPRs.
  static constexpr size_t kNumNativeFprArgs = 0;  // 8 arguments passed in FPRs.

  static constexpr size_t kRegistersNeededForLong = 2;
  static constexpr size_t kRegistersNeededForDouble = 2;
  static constexpr bool kMultiRegistersAligned = false;  // x86 not using regs, anyways
  static constexpr bool kMultiRegistersWidened = false;
  static constexpr bool kAlignLongOnStack = false;
  static constexpr bool kAlignDoubleOnStack = false;
#elif defined(__x86_64__)
  static constexpr bool kNativeSoftFloatAbi = false;  // This is a hard float ABI.
  static constexpr size_t kNumNativeGprArgs = 6;  // 6 arguments passed in GPRs.
  static constexpr size_t kNumNativeFprArgs = 8;  // 8 arguments passed in FPRs.

  static constexpr size_t kRegistersNeededForLong = 1;
  static constexpr size_t kRegistersNeededForDouble = 1;
  static constexpr bool kMultiRegistersAligned = false;
  static constexpr bool kMultiRegistersWidened = false;
  static constexpr bool kAlignLongOnStack = false;
  static constexpr bool kAlignDoubleOnStack = false;
#else
#error "Unsupported architecture"
#endif

 public:
  explicit BuildNativeCallFrameStateMachine(T* delegate)
      : gpr_index_(kNumNativeGprArgs),
        fpr_index_(kNumNativeFprArgs),
        stack_entries_(0),
        delegate_(delegate) {
    // For register alignment, we want to assume that counters (gpr_index_, fpr_index_) are even iff
    // the next register is even; counting down is just to make the compiler happy...
    CHECK_EQ(kNumNativeGprArgs % 2, 0U);
    CHECK_EQ(kNumNativeFprArgs % 2, 0U);
  }

  virtual ~BuildNativeCallFrameStateMachine() {}

  bool HavePointerGpr() {
    return gpr_index_ > 0;
  }

  void AdvancePointer(const void* val) {
    if (HavePointerGpr()) {
      gpr_index_--;
      PushGpr(reinterpret_cast<uintptr_t>(val));
    } else {
      stack_entries_++;  // TODO: have a field for pointer length as multiple of 32b
      PushStack(reinterpret_cast<uintptr_t>(val));
      gpr_index_ = 0;
    }
  }

  bool HaveHandleScopeGpr() {
    return gpr_index_ > 0;
  }

  void AdvanceHandleScope(mirror::Object* ptr) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    uintptr_t handle = PushHandle(ptr);
    if (HaveHandleScopeGpr()) {
      gpr_index_--;
      PushGpr(handle);
    } else {
      stack_entries_++;
      PushStack(handle);
      gpr_index_ = 0;
    }
  }

  bool HaveIntGpr() {
    return gpr_index_ > 0;
  }

  void AdvanceInt(uint32_t val) {
    if (HaveIntGpr()) {
      gpr_index_--;
      PushGpr(val);
    } else {
      stack_entries_++;
      PushStack(val);
      gpr_index_ = 0;
    }
  }

  bool HaveLongGpr() {
    return gpr_index_ >= kRegistersNeededForLong + (LongGprNeedsPadding() ? 1 : 0);
  }

  bool LongGprNeedsPadding() {
    return kRegistersNeededForLong > 1 &&     // only pad when using multiple registers
        kAlignLongOnStack &&                  // and when it needs alignment
        (gpr_index_ & 1) == 1;                // counter is odd, see constructor
  }

  bool LongStackNeedsPadding() {
    return kRegistersNeededForLong > 1 &&     // only pad when using multiple registers
        kAlignLongOnStack &&                  // and when it needs 8B alignment
        (stack_entries_ & 1) == 1;            // counter is odd
  }

  void AdvanceLong(uint64_t val) {
    if (HaveLongGpr()) {
      if (LongGprNeedsPadding()) {
        PushGpr(0);
        gpr_index_--;
      }
      if (kRegistersNeededForLong == 1) {
        PushGpr(static_cast<uintptr_t>(val));
      } else {
        PushGpr(static_cast<uintptr_t>(val & 0xFFFFFFFF));
        PushGpr(static_cast<uintptr_t>((val >> 32) & 0xFFFFFFFF));
      }
      gpr_index_ -= kRegistersNeededForLong;
    } else {
      if (LongStackNeedsPadding()) {
        PushStack(0);
        stack_entries_++;
      }
      if (kRegistersNeededForLong == 1) {
        PushStack(static_cast<uintptr_t>(val));
        stack_entries_++;
      } else {
        PushStack(static_cast<uintptr_t>(val & 0xFFFFFFFF));
        PushStack(static_cast<uintptr_t>((val >> 32) & 0xFFFFFFFF));
        stack_entries_ += 2;
      }
      gpr_index_ = 0;
    }
  }

  bool HaveFloatFpr() {
    return fpr_index_ > 0;
  }

  void AdvanceFloat(float val) {
    if (kNativeSoftFloatAbi) {
      AdvanceInt(bit_cast<float, uint32_t>(val));
    } else {
      if (HaveFloatFpr()) {
        fpr_index_--;
        if (kRegistersNeededForDouble == 1) {
          if (kMultiRegistersWidened) {
            PushFpr8(bit_cast<double, uint64_t>(val));
          } else {
            // No widening, just use the bits.
            PushFpr8(bit_cast<float, uint64_t>(val));
          }
        } else {
          PushFpr4(val);
        }
      } else {
        stack_entries_++;
        if (kRegistersNeededForDouble == 1 && kMultiRegistersWidened) {
          // Need to widen before storing: Note the "double" in the template instantiation.
          // Note: We need to jump through those hoops to make the compiler happy.
          DCHECK_EQ(sizeof(uintptr_t), sizeof(uint64_t));
          PushStack(static_cast<uintptr_t>(bit_cast<double, uint64_t>(val)));
        } else {
          PushStack(bit_cast<float, uintptr_t>(val));
        }
        fpr_index_ = 0;
      }
    }
  }

  bool HaveDoubleFpr() {
    return fpr_index_ >= kRegistersNeededForDouble + (DoubleFprNeedsPadding() ? 1 : 0);
  }

  bool DoubleFprNeedsPadding() {
    return kRegistersNeededForDouble > 1 &&     // only pad when using multiple registers
        kAlignDoubleOnStack &&                  // and when it needs alignment
        (fpr_index_ & 1) == 1;                  // counter is odd, see constructor
  }

  bool DoubleStackNeedsPadding() {
    return kRegistersNeededForDouble > 1 &&     // only pad when using multiple registers
        kAlignDoubleOnStack &&                  // and when it needs 8B alignment
        (stack_entries_ & 1) == 1;              // counter is odd
  }

  void AdvanceDouble(uint64_t val) {
    if (kNativeSoftFloatAbi) {
      AdvanceLong(val);
    } else {
      if (HaveDoubleFpr()) {
        if (DoubleFprNeedsPadding()) {
          PushFpr4(0);
          fpr_index_--;
        }
        PushFpr8(val);
        fpr_index_ -= kRegistersNeededForDouble;
      } else {
        if (DoubleStackNeedsPadding()) {
          PushStack(0);
          stack_entries_++;
        }
        if (kRegistersNeededForDouble == 1) {
          PushStack(static_cast<uintptr_t>(val));
          stack_entries_++;
        } else {
          PushStack(static_cast<uintptr_t>(val & 0xFFFFFFFF));
          PushStack(static_cast<uintptr_t>((val >> 32) & 0xFFFFFFFF));
          stack_entries_ += 2;
        }
        fpr_index_ = 0;
      }
    }
  }

  uint32_t getStackEntries() {
    return stack_entries_;
  }

  uint32_t getNumberOfUsedGprs() {
    return kNumNativeGprArgs - gpr_index_;
  }

  uint32_t getNumberOfUsedFprs() {
    return kNumNativeFprArgs - fpr_index_;
  }

 private:
  void PushGpr(uintptr_t val) {
    delegate_->PushGpr(val);
  }
  void PushFpr4(float val) {
    delegate_->PushFpr4(val);
  }
  void PushFpr8(uint64_t val) {
    delegate_->PushFpr8(val);
  }
  void PushStack(uintptr_t val) {
    delegate_->PushStack(val);
  }
  uintptr_t PushHandle(mirror::Object* ref) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return delegate_->PushHandle(ref);
  }

  uint32_t gpr_index_;      // Number of free GPRs
  uint32_t fpr_index_;      // Number of free FPRs
  uint32_t stack_entries_;  // Stack entries are in multiples of 32b, as floats are usually not
                            // extended
  T* delegate_;             // What Push implementation gets called
};

// Computes the sizes of register stacks and call stack area. Handling of references can be extended
// in subclasses.
//
// To handle native pointers, use "L" in the shorty for an object reference, which simulates
// them with handles.
class ComputeNativeCallFrameSize {
 public:
  ComputeNativeCallFrameSize() : num_stack_entries_(0) {}

  virtual ~ComputeNativeCallFrameSize() {}

  uint32_t GetStackSize() {
    return num_stack_entries_ * sizeof(uintptr_t);
  }

  uint8_t* LayoutCallStack(uint8_t* sp8) {
    sp8 -= GetStackSize();
    // Align by kStackAlignment.
    sp8 = reinterpret_cast<uint8_t*>(RoundDown(reinterpret_cast<uintptr_t>(sp8), kStackAlignment));
    return sp8;
  }

  uint8_t* LayoutCallRegisterStacks(uint8_t* sp8, uintptr_t** start_gpr, uint32_t** start_fpr) {
    // Assumption is OK right now, as we have soft-float arm
    size_t fregs = BuildNativeCallFrameStateMachine<ComputeNativeCallFrameSize>::kNumNativeFprArgs;
    sp8 -= fregs * sizeof(uintptr_t);
    *start_fpr = reinterpret_cast<uint32_t*>(sp8);
    size_t iregs = BuildNativeCallFrameStateMachine<ComputeNativeCallFrameSize>::kNumNativeGprArgs;
    sp8 -= iregs * sizeof(uintptr_t);
    *start_gpr = reinterpret_cast<uintptr_t*>(sp8);
    return sp8;
  }

  uint8_t* LayoutNativeCall(uint8_t* sp8, uintptr_t** start_stack, uintptr_t** start_gpr,
                            uint32_t** start_fpr) {
    // Native call stack.
    sp8 = LayoutCallStack(sp8);
    *start_stack = reinterpret_cast<uintptr_t*>(sp8);

    // Put fprs and gprs below.
    sp8 = LayoutCallRegisterStacks(sp8, start_gpr, start_fpr);

    // Return the new bottom.
    return sp8;
  }

  virtual void WalkHeader(BuildNativeCallFrameStateMachine<ComputeNativeCallFrameSize>* sm)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {}

  void Walk(const char* shorty, uint32_t shorty_len) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    BuildNativeCallFrameStateMachine<ComputeNativeCallFrameSize> sm(this);

    WalkHeader(&sm);

    for (uint32_t i = 1; i < shorty_len; ++i) {
      Primitive::Type cur_type_ = Primitive::GetType(shorty[i]);
      switch (cur_type_) {
        case Primitive::kPrimNot:
          sm.AdvanceHandleScope(
              reinterpret_cast<mirror::Object*>(0x12345678));
          break;

        case Primitive::kPrimBoolean:
        case Primitive::kPrimByte:
        case Primitive::kPrimChar:
        case Primitive::kPrimShort:
        case Primitive::kPrimInt:
          sm.AdvanceInt(0);
          break;
        case Primitive::kPrimFloat:
          sm.AdvanceFloat(0);
          break;
        case Primitive::kPrimDouble:
          sm.AdvanceDouble(0);
          break;
        case Primitive::kPrimLong:
          sm.AdvanceLong(0);
          break;
        default:
          LOG(FATAL) << "Unexpected type: " << cur_type_ << " in " << shorty;
      }
    }

    num_stack_entries_ = sm.getStackEntries();
  }

  void PushGpr(uintptr_t /* val */) {
    // not optimizing registers, yet
  }

  void PushFpr4(float /* val */) {
    // not optimizing registers, yet
  }

  void PushFpr8(uint64_t /* val */) {
    // not optimizing registers, yet
  }

  void PushStack(uintptr_t /* val */) {
    // counting is already done in the superclass
  }

  virtual uintptr_t PushHandle(mirror::Object* /* ptr */) {
    return reinterpret_cast<uintptr_t>(nullptr);
  }

 protected:
  uint32_t num_stack_entries_;
};

class ComputeGenericJniFrameSize FINAL : public ComputeNativeCallFrameSize {
 public:
  ComputeGenericJniFrameSize() : num_handle_scope_references_(0) {}

  // Lays out the callee-save frame. Assumes that the incorrect frame corresponding to RefsAndArgs
  // is at *m = sp. Will update to point to the bottom of the save frame.
  //
  // Note: assumes ComputeAll() has been run before.
  void LayoutCalleeSaveFrame(StackReference<mirror::ArtMethod>** m, void* sp, HandleScope** table,
                             uint32_t* handle_scope_entries)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    mirror::ArtMethod* method = (*m)->AsMirrorPtr();

    uint8_t* sp8 = reinterpret_cast<uint8_t*>(sp);

    // First, fix up the layout of the callee-save frame.
    // We have to squeeze in the HandleScope, and relocate the method pointer.

    // "Free" the slot for the method.
    sp8 += kPointerSize;  // In the callee-save frame we use a full pointer.

    // Under the callee saves put handle scope and new method stack reference.
    *handle_scope_entries = num_handle_scope_references_;

    size_t handle_scope_size = HandleScope::SizeOf(num_handle_scope_references_);
    size_t scope_and_method = handle_scope_size + sizeof(StackReference<mirror::ArtMethod>);

    sp8 -= scope_and_method;
    // Align by kStackAlignment.
    sp8 = reinterpret_cast<uint8_t*>(RoundDown(
        reinterpret_cast<uintptr_t>(sp8), kStackAlignment));

    uint8_t* sp8_table = sp8 + sizeof(StackReference<mirror::ArtMethod>);
    *table = reinterpret_cast<HandleScope*>(sp8_table);
    (*table)->SetNumberOfReferences(num_handle_scope_references_);

    // Add a slot for the method pointer, and fill it. Fix the pointer-pointer given to us.
    uint8_t* method_pointer = sp8;
    StackReference<mirror::ArtMethod>* new_method_ref =
        reinterpret_cast<StackReference<mirror::ArtMethod>*>(method_pointer);
    new_method_ref->Assign(method);
    *m = new_method_ref;
  }

  // Adds space for the cookie. Note: may leave stack unaligned.
  void LayoutCookie(uint8_t** sp) {
    // Reference cookie and padding
    *sp -= 8;
  }

  // Re-layout the callee-save frame (insert a handle-scope). Then add space for the cookie.
  // Returns the new bottom. Note: this may be unaligned.
  uint8_t* LayoutJNISaveFrame(StackReference<mirror::ArtMethod>** m, void* sp, HandleScope** table,
                              uint32_t* handle_scope_entries)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    // First, fix up the layout of the callee-save frame.
    // We have to squeeze in the HandleScope, and relocate the method pointer.
    LayoutCalleeSaveFrame(m, sp, table, handle_scope_entries);

    // The bottom of the callee-save frame is now where the method is, *m.
    uint8_t* sp8 = reinterpret_cast<uint8_t*>(*m);

    // Add space for cookie.
    LayoutCookie(&sp8);

    return sp8;
  }

  // WARNING: After this, *sp won't be pointing to the method anymore!
  uint8_t* ComputeLayout(StackReference<mirror::ArtMethod>** m, bool is_static, const char* shorty,
                         uint32_t shorty_len, HandleScope** table, uint32_t* handle_scope_entries,
                         uintptr_t** start_stack, uintptr_t** start_gpr, uint32_t** start_fpr)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    Walk(shorty, shorty_len);

    // JNI part.
    uint8_t* sp8 = LayoutJNISaveFrame(m, reinterpret_cast<void*>(*m), table, handle_scope_entries);

    sp8 = LayoutNativeCall(sp8, start_stack, start_gpr, start_fpr);

    // Return the new bottom.
    return sp8;
  }

  uintptr_t PushHandle(mirror::Object* /* ptr */) OVERRIDE;

  // Add JNIEnv* and jobj/jclass before the shorty-derived elements.
  void WalkHeader(BuildNativeCallFrameStateMachine<ComputeNativeCallFrameSize>* sm) OVERRIDE
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

 private:
  uint32_t num_handle_scope_references_;
};

uintptr_t ComputeGenericJniFrameSize::PushHandle(mirror::Object* /* ptr */) {
  num_handle_scope_references_++;
  return reinterpret_cast<uintptr_t>(nullptr);
}

void ComputeGenericJniFrameSize::WalkHeader(
    BuildNativeCallFrameStateMachine<ComputeNativeCallFrameSize>* sm) {
  // JNIEnv
  sm->AdvancePointer(nullptr);

  // Class object or this as first argument
  sm->AdvanceHandleScope(reinterpret_cast<mirror::Object*>(0x12345678));
}

// Class to push values to three separate regions. Used to fill the native call part. Adheres to
// the template requirements of BuildGenericJniFrameStateMachine.
class FillNativeCall {
 public:
  FillNativeCall(uintptr_t* gpr_regs, uint32_t* fpr_regs, uintptr_t* stack_args) :
      cur_gpr_reg_(gpr_regs), cur_fpr_reg_(fpr_regs), cur_stack_arg_(stack_args) {}

  virtual ~FillNativeCall() {}

  void Reset(uintptr_t* gpr_regs, uint32_t* fpr_regs, uintptr_t* stack_args) {
    cur_gpr_reg_ = gpr_regs;
    cur_fpr_reg_ = fpr_regs;
    cur_stack_arg_ = stack_args;
  }

  void PushGpr(uintptr_t val) {
    *cur_gpr_reg_ = val;
    cur_gpr_reg_++;
  }

  void PushFpr4(float val) {
    *cur_fpr_reg_ = val;
    cur_fpr_reg_++;
  }

  void PushFpr8(uint64_t val) {
    uint64_t* tmp = reinterpret_cast<uint64_t*>(cur_fpr_reg_);
    *tmp = val;
    cur_fpr_reg_ += 2;
  }

  void PushStack(uintptr_t val) {
    *cur_stack_arg_ = val;
    cur_stack_arg_++;
  }

  virtual uintptr_t PushHandle(mirror::Object* ref) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    LOG(FATAL) << "(Non-JNI) Native call does not use handles.";
    return 0U;
  }

 private:
  uintptr_t* cur_gpr_reg_;
  uint32_t* cur_fpr_reg_;
  uintptr_t* cur_stack_arg_;
};

// Visits arguments on the stack placing them into a region lower down the stack for the benefit
// of transitioning into native code.
class BuildGenericJniFrameVisitor FINAL : public QuickArgumentVisitor {
 public:
  BuildGenericJniFrameVisitor(StackReference<mirror::ArtMethod>** sp, bool is_static,
                              const char* shorty, uint32_t shorty_len, Thread* self)
     : QuickArgumentVisitor(*sp, is_static, shorty, shorty_len),
       jni_call_(nullptr, nullptr, nullptr, nullptr), sm_(&jni_call_) {
    ComputeGenericJniFrameSize fsc;
    uintptr_t* start_gpr_reg;
    uint32_t* start_fpr_reg;
    uintptr_t* start_stack_arg;
    uint32_t handle_scope_entries;
    bottom_of_used_area_ = fsc.ComputeLayout(sp, is_static, shorty, shorty_len, &handle_scope_,
                                             &handle_scope_entries, &start_stack_arg,
                                             &start_gpr_reg, &start_fpr_reg);

    handle_scope_->SetNumberOfReferences(handle_scope_entries);
    jni_call_.Reset(start_gpr_reg, start_fpr_reg, start_stack_arg, handle_scope_);

    // jni environment is always first argument
    sm_.AdvancePointer(self->GetJniEnv());

    if (is_static) {
      sm_.AdvanceHandleScope((*sp)->AsMirrorPtr()->GetDeclaringClass());
    }
  }

  void Visit() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) OVERRIDE;

  void FinalizeHandleScope(Thread* self) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  StackReference<mirror::Object>* GetFirstHandleScopeEntry()
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return handle_scope_->GetHandle(0).GetReference();
  }

  jobject GetFirstHandleScopeJObject() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return handle_scope_->GetHandle(0).ToJObject();
  }

  void* GetBottomOfUsedArea() {
    return bottom_of_used_area_;
  }

 private:
  // A class to fill a JNI call. Adds reference/handle-scope management to FillNativeCall.
  class FillJniCall FINAL : public FillNativeCall {
   public:
    FillJniCall(uintptr_t* gpr_regs, uint32_t* fpr_regs, uintptr_t* stack_args,
                HandleScope* handle_scope) : FillNativeCall(gpr_regs, fpr_regs, stack_args),
                                             handle_scope_(handle_scope), cur_entry_(0) {}

    uintptr_t PushHandle(mirror::Object* ref) OVERRIDE SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

    void Reset(uintptr_t* gpr_regs, uint32_t* fpr_regs, uintptr_t* stack_args, HandleScope* scope) {
      FillNativeCall::Reset(gpr_regs, fpr_regs, stack_args);
      handle_scope_ = scope;
      cur_entry_ = 0U;
    }

    void ResetRemainingScopeSlots() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
      // Initialize padding entries.
      size_t expected_slots = handle_scope_->NumberOfReferences();
      while (cur_entry_ < expected_slots) {
        handle_scope_->GetHandle(cur_entry_++).Assign(nullptr);
      }
      DCHECK_NE(cur_entry_, 0U);
    }

   private:
    HandleScope* handle_scope_;
    size_t cur_entry_;
  };

  HandleScope* handle_scope_;
  FillJniCall jni_call_;
  void* bottom_of_used_area_;

  BuildNativeCallFrameStateMachine<FillJniCall> sm_;

  DISALLOW_COPY_AND_ASSIGN(BuildGenericJniFrameVisitor);
};

uintptr_t BuildGenericJniFrameVisitor::FillJniCall::PushHandle(mirror::Object* ref) {
  uintptr_t tmp;
  Handle<mirror::Object> h = handle_scope_->GetHandle(cur_entry_);
  h.Assign(ref);
  tmp = reinterpret_cast<uintptr_t>(h.ToJObject());
  cur_entry_++;
  return tmp;
}

void BuildGenericJniFrameVisitor::Visit() {
  Primitive::Type type = GetParamPrimitiveType();
  switch (type) {
    case Primitive::kPrimLong: {
      jlong long_arg;
      if (IsSplitLongOrDouble()) {
        long_arg = ReadSplitLongParam();
      } else {
        long_arg = *reinterpret_cast<jlong*>(GetParamAddress());
      }
      sm_.AdvanceLong(long_arg);
      break;
    }
    case Primitive::kPrimDouble: {
      uint64_t double_arg;
      if (IsSplitLongOrDouble()) {
        // Read into union so that we don't case to a double.
        double_arg = ReadSplitLongParam();
      } else {
        double_arg = *reinterpret_cast<uint64_t*>(GetParamAddress());
      }
      sm_.AdvanceDouble(double_arg);
      break;
    }
    case Primitive::kPrimNot: {
      StackReference<mirror::Object>* stack_ref =
          reinterpret_cast<StackReference<mirror::Object>*>(GetParamAddress());
      sm_.AdvanceHandleScope(stack_ref->AsMirrorPtr());
      break;
    }
    case Primitive::kPrimFloat:
      sm_.AdvanceFloat(*reinterpret_cast<float*>(GetParamAddress()));
      break;
    case Primitive::kPrimBoolean:  // Fall-through.
    case Primitive::kPrimByte:     // Fall-through.
    case Primitive::kPrimChar:     // Fall-through.
    case Primitive::kPrimShort:    // Fall-through.
    case Primitive::kPrimInt:      // Fall-through.
      sm_.AdvanceInt(*reinterpret_cast<jint*>(GetParamAddress()));
      break;
    case Primitive::kPrimVoid:
      LOG(FATAL) << "UNREACHABLE";
      break;
  }
}

void BuildGenericJniFrameVisitor::FinalizeHandleScope(Thread* self) {
  // Clear out rest of the scope.
  jni_call_.ResetRemainingScopeSlots();
  // Install HandleScope.
  self->PushHandleScope(handle_scope_);
}

#if defined(__arm__) || defined(__aarch64__)
extern "C" void* artFindNativeMethod();
#else
extern "C" void* artFindNativeMethod(Thread* self);
#endif

uint64_t artQuickGenericJniEndJNIRef(Thread* self, uint32_t cookie, jobject l, jobject lock) {
  if (lock != nullptr) {
    return reinterpret_cast<uint64_t>(JniMethodEndWithReferenceSynchronized(l, cookie, lock, self));
  } else {
    return reinterpret_cast<uint64_t>(JniMethodEndWithReference(l, cookie, self));
  }
}

void artQuickGenericJniEndJNINonRef(Thread* self, uint32_t cookie, jobject lock) {
  if (lock != nullptr) {
    JniMethodEndSynchronized(cookie, lock, self);
  } else {
    JniMethodEnd(cookie, self);
  }
}

/*
 * Initializes an alloca region assumed to be directly below sp for a native call:
 * Create a HandleScope and call stack and fill a mini stack with values to be pushed to registers.
 * The final element on the stack is a pointer to the native code.
 *
 * On entry, the stack has a standard callee-save frame above sp, and an alloca below it.
 * We need to fix this, as the handle scope needs to go into the callee-save frame.
 *
 * The return of this function denotes:
 * 1) How many bytes of the alloca can be released, if the value is non-negative.
 * 2) An error, if the value is negative.
 */
extern "C" TwoWordReturn artQuickGenericJniTrampoline(Thread* self,
                                                      StackReference<mirror::ArtMethod>* sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::ArtMethod* called = sp->AsMirrorPtr();
  DCHECK(called->IsNative()) << PrettyMethod(called, true);
  uint32_t shorty_len = 0;
  const char* shorty = called->GetShorty(&shorty_len);

  // Run the visitor.
  BuildGenericJniFrameVisitor visitor(&sp, called->IsStatic(), shorty, shorty_len, self);
  visitor.VisitArguments();
  visitor.FinalizeHandleScope(self);

  // Fix up managed-stack things in Thread.
  self->SetTopOfStack(sp, 0);

  self->VerifyStack();

  // Start JNI, save the cookie.
  uint32_t cookie;
  if (called->IsSynchronized()) {
    cookie = JniMethodStartSynchronized(visitor.GetFirstHandleScopeJObject(), self);
    if (self->IsExceptionPending()) {
      self->PopHandleScope();
      // A negative value denotes an error.
      return GetTwoWordFailureValue();
    }
  } else {
    cookie = JniMethodStart(self);
  }
  uint32_t* sp32 = reinterpret_cast<uint32_t*>(sp);
  *(sp32 - 1) = cookie;

  // Retrieve the stored native code.
  const void* nativeCode = called->GetNativeMethod();

  // There are two cases for the content of nativeCode:
  // 1) Pointer to the native function.
  // 2) Pointer to the trampoline for native code binding.
  // In the second case, we need to execute the binding and continue with the actual native function
  // pointer.
  DCHECK(nativeCode != nullptr);
  if (nativeCode == GetJniDlsymLookupStub()) {
#if defined(__arm__) || defined(__aarch64__)
    nativeCode = artFindNativeMethod();
#else
    nativeCode = artFindNativeMethod(self);
#endif

    if (nativeCode == nullptr) {
      DCHECK(self->IsExceptionPending());    // There should be an exception pending now.

      // End JNI, as the assembly will move to deliver the exception.
      jobject lock = called->IsSynchronized() ? visitor.GetFirstHandleScopeJObject() : nullptr;
      if (shorty[0] == 'L') {
        artQuickGenericJniEndJNIRef(self, cookie, nullptr, lock);
      } else {
        artQuickGenericJniEndJNINonRef(self, cookie, lock);
      }

      return GetTwoWordFailureValue();
    }
    // Note that the native code pointer will be automatically set by artFindNativeMethod().
  }

  // Return native code addr(lo) and bottom of alloca address(hi).
  return GetTwoWordSuccessValue(reinterpret_cast<uintptr_t>(visitor.GetBottomOfUsedArea()),
                                reinterpret_cast<uintptr_t>(nativeCode));
}

/*
 * Is called after the native JNI code. Responsible for cleanup (handle scope, saved state) and
 * unlocking.
 */
extern "C" uint64_t artQuickGenericJniEndTrampoline(Thread* self, jvalue result, uint64_t result_f)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  StackReference<mirror::ArtMethod>* sp = self->GetManagedStack()->GetTopQuickFrame();
  uint32_t* sp32 = reinterpret_cast<uint32_t*>(sp);
  mirror::ArtMethod* called = sp->AsMirrorPtr();
  uint32_t cookie = *(sp32 - 1);

  jobject lock = nullptr;
  if (called->IsSynchronized()) {
    HandleScope* table = reinterpret_cast<HandleScope*>(reinterpret_cast<uint8_t*>(sp)
        + sizeof(StackReference<mirror::ArtMethod>));
    lock = table->GetHandle(0).ToJObject();
  }

  char return_shorty_char = called->GetShorty()[0];

  if (return_shorty_char == 'L') {
    return artQuickGenericJniEndJNIRef(self, cookie, result.l, lock);
  } else {
    artQuickGenericJniEndJNINonRef(self, cookie, lock);

    switch (return_shorty_char) {
      case 'F': {
        if (kRuntimeISA == kX86) {
          // Convert back the result to float.
          double d = bit_cast<uint64_t, double>(result_f);
          return bit_cast<float, uint32_t>(static_cast<float>(d));
        } else {
          return result_f;
        }
      }
      case 'D':
        return result_f;
      case 'Z':
        return result.z;
      case 'B':
        return result.b;
      case 'C':
        return result.c;
      case 'S':
        return result.s;
      case 'I':
        return result.i;
      case 'J':
        return result.j;
      case 'V':
        return 0;
      default:
        LOG(FATAL) << "Unexpected return shorty character " << return_shorty_char;
        return 0;
    }
  }
}

// We use TwoWordReturn to optimize scalar returns. We use the hi value for code, and the lo value
// for the method pointer.
//
// It is valid to use this, as at the usage points here (returns from C functions) we are assuming
// to hold the mutator lock (see SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) annotations).

template<InvokeType type, bool access_check>
static TwoWordReturn artInvokeCommon(uint32_t method_idx, mirror::Object* this_object,
                                     mirror::ArtMethod* caller_method,
                                     Thread* self, StackReference<mirror::ArtMethod>* sp);

template<InvokeType type, bool access_check>
static TwoWordReturn artInvokeCommon(uint32_t method_idx, mirror::Object* this_object,
                                     mirror::ArtMethod* caller_method,
                                     Thread* self, StackReference<mirror::ArtMethod>* sp) {
  mirror::ArtMethod* method = FindMethodFast(method_idx, this_object, caller_method, access_check,
                                             type);
  if (UNLIKELY(method == nullptr)) {
    FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsAndArgs);
    const DexFile* dex_file = caller_method->GetDeclaringClass()->GetDexCache()->GetDexFile();
    uint32_t shorty_len;
    const char* shorty = dex_file->GetMethodShorty(dex_file->GetMethodId(method_idx), &shorty_len);
    {
      // Remember the args in case a GC happens in FindMethodFromCode.
      ScopedObjectAccessUnchecked soa(self->GetJniEnv());
      RememberForGcArgumentVisitor visitor(sp, type == kStatic, shorty, shorty_len, &soa);
      visitor.VisitArguments();
      method = FindMethodFromCode<type, access_check>(method_idx, &this_object, &caller_method,
                                                      self);
      visitor.FixupReferences();
    }

    if (UNLIKELY(method == NULL)) {
      CHECK(self->IsExceptionPending());
      return GetTwoWordFailureValue();  // Failure.
    }
  }
  DCHECK(!self->IsExceptionPending());
  const void* code = method->GetEntryPointFromQuickCompiledCode();

  // When we return, the caller will branch to this address, so it had better not be 0!
  DCHECK(code != nullptr) << "Code was NULL in method: " << PrettyMethod(method)
                          << " location: "
                          << method->GetDexFile()->GetLocation();

  return GetTwoWordSuccessValue(reinterpret_cast<uintptr_t>(code),
                                reinterpret_cast<uintptr_t>(method));
}

// Explicit artInvokeCommon template function declarations to please analysis tool.
#define EXPLICIT_INVOKE_COMMON_TEMPLATE_DECL(type, access_check)                                \
  template SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)                                          \
  TwoWordReturn artInvokeCommon<type, access_check>(uint32_t method_idx,                        \
                                                    mirror::Object* this_object,                \
                                                    mirror::ArtMethod* caller_method,           \
                                                    Thread* self,                               \
                                                    StackReference<mirror::ArtMethod>* sp)      \

EXPLICIT_INVOKE_COMMON_TEMPLATE_DECL(kVirtual, false);
EXPLICIT_INVOKE_COMMON_TEMPLATE_DECL(kVirtual, true);
EXPLICIT_INVOKE_COMMON_TEMPLATE_DECL(kInterface, false);
EXPLICIT_INVOKE_COMMON_TEMPLATE_DECL(kInterface, true);
EXPLICIT_INVOKE_COMMON_TEMPLATE_DECL(kDirect, false);
EXPLICIT_INVOKE_COMMON_TEMPLATE_DECL(kDirect, true);
EXPLICIT_INVOKE_COMMON_TEMPLATE_DECL(kStatic, false);
EXPLICIT_INVOKE_COMMON_TEMPLATE_DECL(kStatic, true);
EXPLICIT_INVOKE_COMMON_TEMPLATE_DECL(kSuper, false);
EXPLICIT_INVOKE_COMMON_TEMPLATE_DECL(kSuper, true);
#undef EXPLICIT_INVOKE_COMMON_TEMPLATE_DECL

// See comments in runtime_support_asm.S
extern "C" TwoWordReturn artInvokeInterfaceTrampolineWithAccessCheck(
    uint32_t method_idx, mirror::Object* this_object,
    mirror::ArtMethod* caller_method, Thread* self,
    StackReference<mirror::ArtMethod>* sp)
        SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return artInvokeCommon<kInterface, true>(method_idx, this_object,
                                           caller_method, self, sp);
}

extern "C" TwoWordReturn artInvokeDirectTrampolineWithAccessCheck(
    uint32_t method_idx, mirror::Object* this_object,
    mirror::ArtMethod* caller_method, Thread* self,
    StackReference<mirror::ArtMethod>* sp)
        SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return artInvokeCommon<kDirect, true>(method_idx, this_object, caller_method,
                                        self, sp);
}

extern "C" TwoWordReturn artInvokeStaticTrampolineWithAccessCheck(
    uint32_t method_idx, mirror::Object* this_object,
    mirror::ArtMethod* caller_method, Thread* self,
    StackReference<mirror::ArtMethod>* sp)
        SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return artInvokeCommon<kStatic, true>(method_idx, this_object, caller_method,
                                        self, sp);
}

extern "C" TwoWordReturn artInvokeSuperTrampolineWithAccessCheck(
    uint32_t method_idx, mirror::Object* this_object,
    mirror::ArtMethod* caller_method, Thread* self,
    StackReference<mirror::ArtMethod>* sp)
        SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return artInvokeCommon<kSuper, true>(method_idx, this_object, caller_method,
                                       self, sp);
}

extern "C" TwoWordReturn artInvokeVirtualTrampolineWithAccessCheck(
    uint32_t method_idx, mirror::Object* this_object,
    mirror::ArtMethod* caller_method, Thread* self,
    StackReference<mirror::ArtMethod>* sp)
        SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return artInvokeCommon<kVirtual, true>(method_idx, this_object, caller_method,
                                         self, sp);
}

// Determine target of interface dispatch. This object is known non-null.
extern "C" TwoWordReturn artInvokeInterfaceTrampoline(mirror::ArtMethod* interface_method,
                                                      mirror::Object* this_object,
                                                      mirror::ArtMethod* caller_method,
                                                      Thread* self,
                                                      StackReference<mirror::ArtMethod>* sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::ArtMethod* method;
  if (LIKELY(interface_method->GetDexMethodIndex() != DexFile::kDexNoIndex)) {
    method = this_object->GetClass()->FindVirtualMethodForInterface(interface_method);
    if (UNLIKELY(method == NULL)) {
      FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsAndArgs);
      ThrowIncompatibleClassChangeErrorClassForInterfaceDispatch(interface_method, this_object,
                                                                 caller_method);
      return GetTwoWordFailureValue();  // Failure.
    }
  } else {
    FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsAndArgs);
    DCHECK(interface_method == Runtime::Current()->GetResolutionMethod());

    // Find the caller PC.
    constexpr size_t pc_offset = GetCalleeSavePCOffset(kRuntimeISA, Runtime::kRefsAndArgs);
    uintptr_t caller_pc = *reinterpret_cast<uintptr_t*>(reinterpret_cast<byte*>(sp) + pc_offset);

    // Map the caller PC to a dex PC.
    uint32_t dex_pc = caller_method->ToDexPc(caller_pc);
    const DexFile::CodeItem* code = caller_method->GetCodeItem();
    CHECK_LT(dex_pc, code->insns_size_in_code_units_);
    const Instruction* instr = Instruction::At(&code->insns_[dex_pc]);
    Instruction::Code instr_code = instr->Opcode();
    CHECK(instr_code == Instruction::INVOKE_INTERFACE ||
          instr_code == Instruction::INVOKE_INTERFACE_RANGE)
        << "Unexpected call into interface trampoline: " << instr->DumpString(NULL);
    uint32_t dex_method_idx;
    if (instr_code == Instruction::INVOKE_INTERFACE) {
      dex_method_idx = instr->VRegB_35c();
    } else {
      DCHECK_EQ(instr_code, Instruction::INVOKE_INTERFACE_RANGE);
      dex_method_idx = instr->VRegB_3rc();
    }

    const DexFile* dex_file = caller_method->GetDeclaringClass()->GetDexCache()
        ->GetDexFile();
    uint32_t shorty_len;
    const char* shorty = dex_file->GetMethodShorty(dex_file->GetMethodId(dex_method_idx),
                                                   &shorty_len);
    {
      // Remember the args in case a GC happens in FindMethodFromCode.
      ScopedObjectAccessUnchecked soa(self->GetJniEnv());
      RememberForGcArgumentVisitor visitor(sp, false, shorty, shorty_len, &soa);
      visitor.VisitArguments();
      method = FindMethodFromCode<kInterface, false>(dex_method_idx, &this_object, &caller_method,
                                                     self);
      visitor.FixupReferences();
    }

    if (UNLIKELY(method == nullptr)) {
      CHECK(self->IsExceptionPending());
      return GetTwoWordFailureValue();  // Failure.
    }
  }
  const void* code = method->GetEntryPointFromQuickCompiledCode();

  // When we return, the caller will branch to this address, so it had better not be 0!
  DCHECK(code != nullptr) << "Code was NULL in method: " << PrettyMethod(method)
                          << " location: " << method->GetDexFile()->GetLocation();

  return GetTwoWordSuccessValue(reinterpret_cast<uintptr_t>(code),
                                reinterpret_cast<uintptr_t>(method));
}

}  // namespace art
