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
#include "entrypoints/entrypoint_utils.h"
#include "gc/accounting/card_table-inl.h"
#include "interpreter/interpreter.h"
#include "mirror/art_method-inl.h"
#include "mirror/class-inl.h"
#include "mirror/dex_cache-inl.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "object_utils.h"
#include "runtime.h"
#include "scoped_thread_state_change.h"

namespace art {

// Visits the arguments as saved to the stack by a Runtime::kRefAndArgs callee save frame.
class QuickArgumentVisitor {
  // Number of bytes for each out register in the caller method's frame.
  static constexpr size_t kBytesStackArgLocation = 4;
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
  static constexpr size_t kQuickCalleeSaveFrame_RefAndArgs_Fpr1Offset = 0;  // Offset of first FPR arg.
  static constexpr size_t kQuickCalleeSaveFrame_RefAndArgs_Gpr1Offset = 8;  // Offset of first GPR arg.
  static constexpr size_t kQuickCalleeSaveFrame_RefAndArgs_LrOffset = 44;  // Offset of return address.
  static constexpr size_t kQuickCalleeSaveFrame_RefAndArgs_FrameSize = 48;  // Frame size.
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
  // | X28        |
  // |  :         |
  // | X19        |
  // | X7         |
  // | :          |
  // | X1         |
  // | D15        |
  // |  :         |
  // | D0         |
  // |            |    padding
  // | Method*    |  <- sp
  static constexpr bool kQuickSoftFloatAbi = false;  // This is a hard float ABI.
  static constexpr size_t kNumQuickGprArgs = 7;  // 7 arguments passed in GPRs.
  static constexpr size_t kNumQuickFprArgs = 8;  // 8 arguments passed in FPRs.
  static constexpr size_t kQuickCalleeSaveFrame_RefAndArgs_Fpr1Offset =16;  // Offset of first FPR arg.
  static constexpr size_t kQuickCalleeSaveFrame_RefAndArgs_Gpr1Offset = 144;  // Offset of first GPR arg.
  static constexpr size_t kQuickCalleeSaveFrame_RefAndArgs_LrOffset = 296;  // Offset of return address.
  static constexpr size_t kQuickCalleeSaveFrame_RefAndArgs_FrameSize = 304;  // Frame size.
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
  static constexpr size_t kQuickCalleeSaveFrame_RefAndArgs_FrameSize = 64;  // Frame size.
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
  static constexpr size_t kQuickCalleeSaveFrame_RefAndArgs_FrameSize = 32;  // Frame size.
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
  static constexpr size_t kNumQuickGprArgs = 5;  // 3 arguments passed in GPRs.
  static constexpr size_t kNumQuickFprArgs = 8;  // 0 arguments passed in FPRs.
  static constexpr size_t kQuickCalleeSaveFrame_RefAndArgs_Fpr1Offset = 16;  // Offset of first FPR arg.
  static constexpr size_t kQuickCalleeSaveFrame_RefAndArgs_Gpr1Offset = 80;  // Offset of first GPR arg.
  static constexpr size_t kQuickCalleeSaveFrame_RefAndArgs_LrOffset = 168;  // Offset of return address.
  static constexpr size_t kQuickCalleeSaveFrame_RefAndArgs_FrameSize = 176;  // Frame size.
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
  static mirror::ArtMethod* GetCallingMethod(mirror::ArtMethod** sp)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    DCHECK((*sp)->IsCalleeSaveMethod());
    byte* previous_sp = reinterpret_cast<byte*>(sp) + kQuickCalleeSaveFrame_RefAndArgs_FrameSize;
    return *reinterpret_cast<mirror::ArtMethod**>(previous_sp);
  }

  // For the given quick ref and args quick frame, return the caller's PC.
  static uintptr_t GetCallingPc(mirror::ArtMethod** sp)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    DCHECK((*sp)->IsCalleeSaveMethod());
    byte* lr = reinterpret_cast<byte*>(sp) + kQuickCalleeSaveFrame_RefAndArgs_LrOffset;
    return *reinterpret_cast<uintptr_t*>(lr);
  }

  QuickArgumentVisitor(mirror::ArtMethod** sp, bool is_static,
                       const char* shorty, uint32_t shorty_len)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) :
      is_static_(is_static), shorty_(shorty), shorty_len_(shorty_len),
      gpr_args_(reinterpret_cast<byte*>(sp) + kQuickCalleeSaveFrame_RefAndArgs_Gpr1Offset),
      fpr_args_(reinterpret_cast<byte*>(sp) + kQuickCalleeSaveFrame_RefAndArgs_Fpr1Offset),
      stack_args_(reinterpret_cast<byte*>(sp) + kQuickCalleeSaveFrame_RefAndArgs_FrameSize
                  + StackArgumentStartFromShorty(is_static, shorty, shorty_len)),
      gpr_index_(0), fpr_index_(0), stack_index_(0), cur_type_(Primitive::kPrimVoid),
      is_split_long_or_double_(false) {
    DCHECK_EQ(kQuickCalleeSaveFrame_RefAndArgs_FrameSize,
              Runtime::Current()->GetCalleeSaveMethod(Runtime::kRefsAndArgs)->GetFrameSizeInBytes());
  }

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
          + GetBytesPerGprSpillLocation(kRuntimeISA) /* ArtMethod* */;
    } else {
      // For now, there is no reg-spill area for the targets with
      // hard float ABI. So, the offset pointing to the first method's
      // parameter ('this' for non-static methods) should be returned.
      return GetBytesPerGprSpillLocation(kRuntimeISA);  // Skip Method*.
    }
  }

  const bool is_static_;
  const char* const shorty_;
  const uint32_t shorty_len_;
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
  BuildQuickShadowFrameVisitor(mirror::ArtMethod** sp, bool is_static, const char* shorty,
                               uint32_t shorty_len, ShadowFrame* sf, size_t first_arg_reg) :
    QuickArgumentVisitor(sp, is_static, shorty, shorty_len), sf_(sf), cur_reg_(first_arg_reg) {}

  void Visit() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) OVERRIDE;

 private:
  ShadowFrame* const sf_;
  uint32_t cur_reg_;

  DISALLOW_COPY_AND_ASSIGN(BuildQuickShadowFrameVisitor);
};

void BuildQuickShadowFrameVisitor::Visit()  {
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
                                                mirror::ArtMethod** sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  // Ensure we don't get thread suspension until the object arguments are safely in the shadow
  // frame.
  FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsAndArgs);

  if (method->IsAbstract()) {
    ThrowAbstractMethodError(method);
    return 0;
  } else {
    DCHECK(!method->IsNative()) << PrettyMethod(method);
    const char* old_cause = self->StartAssertNoThreadSuspension("Building interpreter shadow frame");
    MethodHelper mh(method);
    const DexFile::CodeItem* code_item = mh.GetCodeItem();
    DCHECK(code_item != nullptr) << PrettyMethod(method);
    uint16_t num_regs = code_item->registers_size_;
    void* memory = alloca(ShadowFrame::ComputeSize(num_regs));
    ShadowFrame* shadow_frame(ShadowFrame::Create(num_regs, NULL,  // No last shadow coming from quick.
                                                  method, 0, memory));
    size_t first_arg_reg = code_item->registers_size_ - code_item->ins_size_;
    BuildQuickShadowFrameVisitor shadow_frame_builder(sp, mh.IsStatic(), mh.GetShorty(),
                                                      mh.GetShortyLength(),
                                                      shadow_frame, first_arg_reg);
    shadow_frame_builder.VisitArguments();
    // Push a transition back into managed code onto the linked list in thread.
    ManagedStack fragment;
    self->PushManagedStackFragment(&fragment);
    self->PushShadowFrame(shadow_frame);
    self->EndAssertNoThreadSuspension(old_cause);

    if (method->IsStatic() && !method->GetDeclaringClass()->IsInitializing()) {
      // Ensure static method's class is initialized.
      StackHandleScope<1> hs(self);
      Handle<mirror::Class> h_class(hs.NewHandle(method->GetDeclaringClass()));
      if (!Runtime::Current()->GetClassLinker()->EnsureInitialized(h_class, true, true)) {
        DCHECK(Thread::Current()->IsExceptionPending()) << PrettyMethod(method);
        self->PopManagedStackFragment(fragment);
        return 0;
      }
    }

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
  BuildQuickArgumentVisitor(mirror::ArtMethod** sp, bool is_static, const char* shorty,
                            uint32_t shorty_len, ScopedObjectAccessUnchecked* soa,
                            std::vector<jvalue>* args) :
    QuickArgumentVisitor(sp, is_static, shorty, shorty_len), soa_(soa), args_(args) {}

  void Visit() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) OVERRIDE;

  void FixupReferences() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

 private:
  ScopedObjectAccessUnchecked* const soa_;
  std::vector<jvalue>* const args_;
  // References which we must update when exiting in case the GC moved the objects.
  std::vector<std::pair<jobject, StackReference<mirror::Object>*> > references_;

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
                                               Thread* self, mirror::ArtMethod** sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  DCHECK(proxy_method->IsProxyMethod()) << PrettyMethod(proxy_method);
  DCHECK(receiver->GetClass()->IsProxyClass()) << PrettyMethod(proxy_method);
  // Ensure we don't get thread suspension until the object arguments are safely in jobjects.
  const char* old_cause =
      self->StartAssertNoThreadSuspension("Adding to IRT proxy object arguments");
  // Register the top of the managed stack, making stack crawlable.
  DCHECK_EQ(*sp, proxy_method) << PrettyMethod(proxy_method);
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
  jobject rcvr_jobj = soa.AddLocalReference<jobject>(receiver);

  // Placing arguments into args vector and remove the receiver.
  MethodHelper proxy_mh(proxy_method);
  DCHECK(!proxy_mh.IsStatic()) << PrettyMethod(proxy_method);
  std::vector<jvalue> args;
  BuildQuickArgumentVisitor local_ref_visitor(sp, proxy_mh.IsStatic(), proxy_mh.GetShorty(),
                                              proxy_mh.GetShortyLength(), &soa, &args);

  local_ref_visitor.VisitArguments();
  DCHECK_GT(args.size(), 0U) << PrettyMethod(proxy_method);
  args.erase(args.begin());

  // Convert proxy method into expected interface method.
  mirror::ArtMethod* interface_method = proxy_method->FindOverriddenMethod();
  DCHECK(interface_method != NULL) << PrettyMethod(proxy_method);
  DCHECK(!interface_method->IsProxyMethod()) << PrettyMethod(interface_method);
  jobject interface_method_jobj = soa.AddLocalReference<jobject>(interface_method);

  // All naked Object*s should now be in jobjects, so its safe to go into the main invoke code
  // that performs allocations.
  self->EndAssertNoThreadSuspension(old_cause);
  JValue result = InvokeProxyInvocationHandler(soa, proxy_mh.GetShorty(),
                                               rcvr_jobj, interface_method_jobj, args);
  // Restore references which might have moved.
  local_ref_visitor.FixupReferences();
  return result.GetJ();
}

// Read object references held in arguments from quick frames and place in a JNI local references,
// so they don't get garbage collected.
class RememberForGcArgumentVisitor FINAL : public QuickArgumentVisitor {
 public:
  RememberForGcArgumentVisitor(mirror::ArtMethod** sp, bool is_static, const char* shorty,
                               uint32_t shorty_len, ScopedObjectAccessUnchecked* soa) :
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
                                                    Thread* self, mirror::ArtMethod** sp)
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
    {
      MethodHelper mh(caller);
      dex_file = &mh.GetDexFile();
      code = mh.GetCodeItem();
    }
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
    dex_file = &MethodHelper(called).GetDexFile();
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
  if (called->IsRuntimeMethod()) {
    StackHandleScope<1> hs(self);
    Handle<mirror::Object> handle_scope_receiver(hs.NewHandle(virtual_or_interface ? receiver : nullptr));
    called = linker->ResolveMethod(dex_method_idx, caller, invoke_type);
    receiver = handle_scope_receiver.Get();
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
      if (called->GetDexCacheResolvedMethods() == caller->GetDexCacheResolvedMethods()) {
        caller->GetDexCacheResolvedMethods()->Set<false>(called->GetDexMethodIndex(), called);
      } else {
        // Calling from one dex file to another, need to compute the method index appropriate to
        // the caller's dex file. Since we get here only if the original called was a runtime
        // method, we've got the correct dex_file and a dex_method_idx from above.
        DCHECK(&MethodHelper(caller).GetDexFile() == dex_file);
        uint32_t method_index =
            MethodHelper(called).FindDexMethodIndexInOtherDexFile(*dex_file, dex_method_idx);
        if (method_index != DexFile::kDexNoIndex) {
          caller->GetDexCacheResolvedMethods()->Set<false>(method_index, called);
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
  *sp = called;
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
template <class T> class BuildGenericJniFrameStateMachine {
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
  static constexpr bool kMultiRegistersAligned = false;       // x86 not using regs, anyways
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
  explicit BuildGenericJniFrameStateMachine(T* delegate) : gpr_index_(kNumNativeGprArgs),
                                                           fpr_index_(kNumNativeFprArgs),
                                                           stack_entries_(0),
                                                           delegate_(delegate) {
    // For register alignment, we want to assume that counters (gpr_index_, fpr_index_) are even iff
    // the next register is even; counting down is just to make the compiler happy...
    CHECK_EQ(kNumNativeGprArgs % 2, 0U);
    CHECK_EQ(kNumNativeFprArgs % 2, 0U);
  }

  virtual ~BuildGenericJniFrameStateMachine() {}

  bool HavePointerGpr() {
    return gpr_index_ > 0;
  }

  void AdvancePointer(void* val) {
    if (HavePointerGpr()) {
      gpr_index_--;
      PushGpr(reinterpret_cast<uintptr_t>(val));
    } else {
      stack_entries_++;         // TODO: have a field for pointer length as multiple of 32b
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

  template <typename U, typename V> V convert(U in) {
    CHECK_LE(sizeof(U), sizeof(V));
    union { U u; V v; } tmp;
    tmp.u = in;
    return tmp.v;
  }

  void AdvanceFloat(float val) {
    if (kNativeSoftFloatAbi) {
      AdvanceInt(convert<float, uint32_t>(val));
    } else {
      if (HaveFloatFpr()) {
        fpr_index_--;
        if (kRegistersNeededForDouble == 1) {
          if (kMultiRegistersWidened) {
            PushFpr8(convert<double, uint64_t>(val));
          } else {
            // No widening, just use the bits.
            PushFpr8(convert<float, uint64_t>(val));
          }
        } else {
          PushFpr4(val);
        }
      } else {
        stack_entries_++;
        if (kRegistersNeededForDouble == 1 && kMultiRegistersWidened) {
          // Need to widen before storing: Note the "double" in the template instantiation.
          PushStack(convert<double, uintptr_t>(val));
        } else {
          PushStack(convert<float, uintptr_t>(val));
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

class ComputeGenericJniFrameSize FINAL {
 public:
  ComputeGenericJniFrameSize() : num_handle_scope_references_(0), num_stack_entries_(0) {}

  uint32_t GetStackSize() {
    return num_stack_entries_ * sizeof(uintptr_t);
  }

  // WARNING: After this, *sp won't be pointing to the method anymore!
  void ComputeLayout(mirror::ArtMethod*** m, bool is_static, const char* shorty, uint32_t shorty_len,
                     void* sp, HandleScope** table, uint32_t* handle_scope_entries,
                     uintptr_t** start_stack, uintptr_t** start_gpr, uint32_t** start_fpr,
                     void** code_return, size_t* overall_size)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    ComputeAll(is_static, shorty, shorty_len);

    mirror::ArtMethod* method = **m;

    uint8_t* sp8 = reinterpret_cast<uint8_t*>(sp);

    // First, fix up the layout of the callee-save frame.
    // We have to squeeze in the HandleScope, and relocate the method pointer.

    // "Free" the slot for the method.
    sp8 += kPointerSize;

    // Add the HandleScope.
    *handle_scope_entries = num_handle_scope_references_;
    size_t handle_scope_size = HandleScope::GetAlignedHandleScopeSize(num_handle_scope_references_);
    sp8 -= handle_scope_size;
    *table = reinterpret_cast<HandleScope*>(sp8);
    (*table)->SetNumberOfReferences(num_handle_scope_references_);

    // Add a slot for the method pointer, and fill it. Fix the pointer-pointer given to us.
    sp8 -= kPointerSize;
    uint8_t* method_pointer = sp8;
    *(reinterpret_cast<mirror::ArtMethod**>(method_pointer)) = method;
    *m = reinterpret_cast<mirror::ArtMethod**>(method_pointer);

    // Reference cookie and padding
    sp8 -= 8;
    // Store HandleScope size
    *reinterpret_cast<uint32_t*>(sp8) = static_cast<uint32_t>(handle_scope_size & 0xFFFFFFFF);

    // Next comes the native call stack.
    sp8 -= GetStackSize();
    // Now align the call stack below. This aligns by 16, as AArch64 seems to require.
    uintptr_t mask = ~0x0F;
    sp8 = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(sp8) & mask);
    *start_stack = reinterpret_cast<uintptr_t*>(sp8);

    // put fprs and gprs below
    // Assumption is OK right now, as we have soft-float arm
    size_t fregs = BuildGenericJniFrameStateMachine<ComputeGenericJniFrameSize>::kNumNativeFprArgs;
    sp8 -= fregs * sizeof(uintptr_t);
    *start_fpr = reinterpret_cast<uint32_t*>(sp8);
    size_t iregs = BuildGenericJniFrameStateMachine<ComputeGenericJniFrameSize>::kNumNativeGprArgs;
    sp8 -= iregs * sizeof(uintptr_t);
    *start_gpr = reinterpret_cast<uintptr_t*>(sp8);

    // reserve space for the code pointer
    sp8 -= kPointerSize;
    *code_return = reinterpret_cast<void*>(sp8);

    *overall_size = reinterpret_cast<uint8_t*>(sp) - sp8;

    // The new SP is stored at the end of the alloca, so it can be immediately popped
    sp8 = reinterpret_cast<uint8_t*>(sp) - 5 * KB;
    *(reinterpret_cast<uint8_t**>(sp8)) = method_pointer;
  }

  void ComputeHandleScopeOffset() { }  // nothing to do, static right now

  void ComputeAll(bool is_static, const char* shorty, uint32_t shorty_len)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    BuildGenericJniFrameStateMachine<ComputeGenericJniFrameSize> sm(this);

    // JNIEnv
    sm.AdvancePointer(nullptr);

    // Class object or this as first argument
    sm.AdvanceHandleScope(reinterpret_cast<mirror::Object*>(0x12345678));

    for (uint32_t i = 1; i < shorty_len; ++i) {
      Primitive::Type cur_type_ = Primitive::GetType(shorty[i]);
      switch (cur_type_) {
        case Primitive::kPrimNot:
          sm.AdvanceHandleScope(reinterpret_cast<mirror::Object*>(0x12345678));
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

  uintptr_t PushHandle(mirror::Object* /* ptr */) {
    num_handle_scope_references_++;
    return reinterpret_cast<uintptr_t>(nullptr);
  }

 private:
  uint32_t num_handle_scope_references_;
  uint32_t num_stack_entries_;
};

// Visits arguments on the stack placing them into a region lower down the stack for the benefit
// of transitioning into native code.
class BuildGenericJniFrameVisitor FINAL : public QuickArgumentVisitor {
 public:
  BuildGenericJniFrameVisitor(mirror::ArtMethod*** sp, bool is_static, const char* shorty,
                              uint32_t shorty_len, Thread* self) :
      QuickArgumentVisitor(*sp, is_static, shorty, shorty_len), sm_(this) {
    ComputeGenericJniFrameSize fsc;
    fsc.ComputeLayout(sp, is_static, shorty, shorty_len, *sp, &handle_scope_, &handle_scope_expected_refs_,
                      &cur_stack_arg_, &cur_gpr_reg_, &cur_fpr_reg_, &code_return_,
                      &alloca_used_size_);
    handle_scope_number_of_references_ = 0;
    cur_hs_entry_ = reinterpret_cast<StackReference<mirror::Object>*>(GetFirstHandleScopeEntry());

    // jni environment is always first argument
    sm_.AdvancePointer(self->GetJniEnv());

    if (is_static) {
      sm_.AdvanceHandleScope((**sp)->GetDeclaringClass());
    }
  }

  void Visit() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) OVERRIDE;

  void FinalizeHandleScope(Thread* self) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

  jobject GetFirstHandleScopeEntry() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return handle_scope_->GetHandle(0).ToJObject();
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

  uintptr_t PushHandle(mirror::Object* ref) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    uintptr_t tmp;
    if (ref == nullptr) {
      *cur_hs_entry_ = StackReference<mirror::Object>();
      tmp = reinterpret_cast<uintptr_t>(nullptr);
    } else {
      *cur_hs_entry_ = StackReference<mirror::Object>::FromMirrorPtr(ref);
      tmp = reinterpret_cast<uintptr_t>(cur_hs_entry_);
    }
    cur_hs_entry_++;
    handle_scope_number_of_references_++;
    return tmp;
  }

  // Size of the part of the alloca that we actually need.
  size_t GetAllocaUsedSize() {
    return alloca_used_size_;
  }

  void* GetCodeReturn() {
    return code_return_;
  }

 private:
  uint32_t handle_scope_number_of_references_;
  StackReference<mirror::Object>* cur_hs_entry_;
  HandleScope* handle_scope_;
  uint32_t handle_scope_expected_refs_;
  uintptr_t* cur_gpr_reg_;
  uint32_t* cur_fpr_reg_;
  uintptr_t* cur_stack_arg_;
  // StackReference<mirror::Object>* top_of_handle_scope_;
  void* code_return_;
  size_t alloca_used_size_;

  BuildGenericJniFrameStateMachine<BuildGenericJniFrameVisitor> sm_;

  DISALLOW_COPY_AND_ASSIGN(BuildGenericJniFrameVisitor);
};

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
  // Initialize padding entries.
  while (handle_scope_number_of_references_ < handle_scope_expected_refs_) {
    *cur_hs_entry_ = StackReference<mirror::Object>();
    cur_hs_entry_++;
    handle_scope_number_of_references_++;
  }
  handle_scope_->SetNumberOfReferences(handle_scope_expected_refs_);
  DCHECK_NE(handle_scope_expected_refs_, 0U);
  // Install HandleScope.
  self->PushHandleScope(handle_scope_);
}

extern "C" void* artFindNativeMethod();

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
extern "C" ssize_t artQuickGenericJniTrampoline(Thread* self, mirror::ArtMethod** sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::ArtMethod* called = *sp;
  DCHECK(called->IsNative()) << PrettyMethod(called, true);

  // run the visitor
  MethodHelper mh(called);

  BuildGenericJniFrameVisitor visitor(&sp, called->IsStatic(), mh.GetShorty(), mh.GetShortyLength(),
                                      self);
  visitor.VisitArguments();
  visitor.FinalizeHandleScope(self);

  // fix up managed-stack things in Thread
  self->SetTopOfStack(sp, 0);

  self->VerifyStack();

  // Start JNI, save the cookie.
  uint32_t cookie;
  if (called->IsSynchronized()) {
    cookie = JniMethodStartSynchronized(visitor.GetFirstHandleScopeEntry(), self);
    if (self->IsExceptionPending()) {
      self->PopHandleScope();
      // A negative value denotes an error.
      return -1;
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
    nativeCode = artFindNativeMethod();

    if (nativeCode == nullptr) {
      DCHECK(self->IsExceptionPending());    // There should be an exception pending now.

      // End JNI, as the assembly will move to deliver the exception.
      jobject lock = called->IsSynchronized() ? visitor.GetFirstHandleScopeEntry() : nullptr;
      if (mh.GetShorty()[0] == 'L') {
        artQuickGenericJniEndJNIRef(self, cookie, nullptr, lock);
      } else {
        artQuickGenericJniEndJNINonRef(self, cookie, lock);
      }

      return -1;
    }
    // Note that the native code pointer will be automatically set by artFindNativeMethod().
  }

  // Store the native code pointer in the stack at the right location.
  uintptr_t* code_pointer = reinterpret_cast<uintptr_t*>(visitor.GetCodeReturn());
  *code_pointer = reinterpret_cast<uintptr_t>(nativeCode);

  // 5K reserved, window_size + frame pointer used.
  size_t window_size = visitor.GetAllocaUsedSize();
  return (5 * KB) - window_size - kPointerSize;
}

/*
 * Is called after the native JNI code. Responsible for cleanup (handle scope, saved state) and
 * unlocking.
 */
extern "C" uint64_t artQuickGenericJniEndTrampoline(Thread* self, mirror::ArtMethod** sp,
                                                    jvalue result, uint64_t result_f)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  uint32_t* sp32 = reinterpret_cast<uint32_t*>(sp);
  mirror::ArtMethod* called = *sp;
  uint32_t cookie = *(sp32 - 1);

  jobject lock = nullptr;
  if (called->IsSynchronized()) {
    HandleScope* table = reinterpret_cast<HandleScope*>(
        reinterpret_cast<uint8_t*>(sp) + kPointerSize);
    lock = table->GetHandle(0).ToJObject();
  }

  MethodHelper mh(called);
  char return_shorty_char = mh.GetShorty()[0];

  if (return_shorty_char == 'L') {
    return artQuickGenericJniEndJNIRef(self, cookie, result.l, lock);
  } else {
    artQuickGenericJniEndJNINonRef(self, cookie, lock);

    switch (return_shorty_char) {
      case 'F':  // Fall-through.
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

template<InvokeType type, bool access_check>
static uint64_t artInvokeCommon(uint32_t method_idx, mirror::Object* this_object,
                                mirror::ArtMethod* caller_method,
                                Thread* self, mirror::ArtMethod** sp);

template<InvokeType type, bool access_check>
static uint64_t artInvokeCommon(uint32_t method_idx, mirror::Object* this_object,
                                mirror::ArtMethod* caller_method,
                                Thread* self, mirror::ArtMethod** sp) {
  mirror::ArtMethod* method = FindMethodFast(method_idx, this_object, caller_method, access_check,
                                             type);
  if (UNLIKELY(method == nullptr)) {
    FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsAndArgs);
    const DexFile* dex_file = caller_method->GetDeclaringClass()->GetDexCache()->GetDexFile();
    uint32_t shorty_len;
    const char* shorty =
        dex_file->GetMethodShorty(dex_file->GetMethodId(method_idx), &shorty_len);
    {
      // Remember the args in case a GC happens in FindMethodFromCode.
      ScopedObjectAccessUnchecked soa(self->GetJniEnv());
      RememberForGcArgumentVisitor visitor(sp, type == kStatic, shorty, shorty_len, &soa);
      visitor.VisitArguments();
      method = FindMethodFromCode<type, access_check>(method_idx, this_object, caller_method, self);
      visitor.FixupReferences();
    }

    if (UNLIKELY(method == NULL)) {
      CHECK(self->IsExceptionPending());
      return 0;  // failure
    }
  }
  DCHECK(!self->IsExceptionPending());
  const void* code = method->GetEntryPointFromQuickCompiledCode();

  // When we return, the caller will branch to this address, so it had better not be 0!
  DCHECK(code != nullptr) << "Code was NULL in method: " << PrettyMethod(method) << " location: "
      << MethodHelper(method).GetDexFile().GetLocation();
#ifdef __LP64__
  UNIMPLEMENTED(FATAL);
  return 0;
#else
  uint32_t method_uint = reinterpret_cast<uint32_t>(method);
  uint64_t code_uint = reinterpret_cast<uint32_t>(code);
  uint64_t result = ((code_uint << 32) | method_uint);
  return result;
#endif
}

// Explicit artInvokeCommon template function declarations to please analysis tool.
#define EXPLICIT_INVOKE_COMMON_TEMPLATE_DECL(type, access_check)                                \
  template SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)                                          \
  uint64_t artInvokeCommon<type, access_check>(uint32_t method_idx,                             \
                                               mirror::Object* this_object,                     \
                                               mirror::ArtMethod* caller_method,                \
                                               Thread* self, mirror::ArtMethod** sp)            \

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
extern "C" uint64_t artInvokeInterfaceTrampolineWithAccessCheck(uint32_t method_idx,
                                                                mirror::Object* this_object,
                                                                mirror::ArtMethod* caller_method,
                                                                Thread* self,
                                                                mirror::ArtMethod** sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return artInvokeCommon<kInterface, true>(method_idx, this_object, caller_method, self, sp);
}


extern "C" uint64_t artInvokeDirectTrampolineWithAccessCheck(uint32_t method_idx,
                                                             mirror::Object* this_object,
                                                             mirror::ArtMethod* caller_method,
                                                             Thread* self,
                                                             mirror::ArtMethod** sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return artInvokeCommon<kDirect, true>(method_idx, this_object, caller_method, self, sp);
}

extern "C" uint64_t artInvokeStaticTrampolineWithAccessCheck(uint32_t method_idx,
                                                             mirror::Object* this_object,
                                                             mirror::ArtMethod* caller_method,
                                                             Thread* self,
                                                             mirror::ArtMethod** sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return artInvokeCommon<kStatic, true>(method_idx, this_object, caller_method, self, sp);
}

extern "C" uint64_t artInvokeSuperTrampolineWithAccessCheck(uint32_t method_idx,
                                                            mirror::Object* this_object,
                                                            mirror::ArtMethod* caller_method,
                                                            Thread* self,
                                                            mirror::ArtMethod** sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return artInvokeCommon<kSuper, true>(method_idx, this_object, caller_method, self, sp);
}

extern "C" uint64_t artInvokeVirtualTrampolineWithAccessCheck(uint32_t method_idx,
                                                              mirror::Object* this_object,
                                                              mirror::ArtMethod* caller_method,
                                                              Thread* self,
                                                              mirror::ArtMethod** sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  return artInvokeCommon<kVirtual, true>(method_idx, this_object, caller_method, self, sp);
}

// Determine target of interface dispatch. This object is known non-null.
extern "C" uint64_t artInvokeInterfaceTrampoline(mirror::ArtMethod* interface_method,
                                                 mirror::Object* this_object,
                                                 mirror::ArtMethod* caller_method,
                                                 Thread* self, mirror::ArtMethod** sp)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::ArtMethod* method;
  if (LIKELY(interface_method->GetDexMethodIndex() != DexFile::kDexNoIndex)) {
    method = this_object->GetClass()->FindVirtualMethodForInterface(interface_method);
    if (UNLIKELY(method == NULL)) {
      FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsAndArgs);
      ThrowIncompatibleClassChangeErrorClassForInterfaceDispatch(interface_method, this_object,
                                                                 caller_method);
      return 0;  // Failure.
    }
  } else {
    FinishCalleeSaveFrameSetup(self, sp, Runtime::kRefsAndArgs);
    DCHECK(interface_method == Runtime::Current()->GetResolutionMethod());
    // Determine method index from calling dex instruction.
#if defined(__arm__)
    // On entry the stack pointed by sp is:
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
    // | R0         |
    // | Method*    |  <- sp
    DCHECK_EQ(48U, Runtime::Current()->GetCalleeSaveMethod(Runtime::kRefsAndArgs)->GetFrameSizeInBytes());
    uintptr_t* regs = reinterpret_cast<uintptr_t*>(reinterpret_cast<byte*>(sp) + kPointerSize);
    uintptr_t caller_pc = regs[10];
#elif defined(__i386__)
    // On entry the stack pointed by sp is:
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
    DCHECK_EQ(32U, Runtime::Current()->GetCalleeSaveMethod(Runtime::kRefsAndArgs)->GetFrameSizeInBytes());
    uintptr_t* regs = reinterpret_cast<uintptr_t*>(reinterpret_cast<byte*>(sp));
    uintptr_t caller_pc = regs[7];
#elif defined(__mips__)
    // On entry the stack pointed by sp is:
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
    DCHECK_EQ(64U, Runtime::Current()->GetCalleeSaveMethod(Runtime::kRefsAndArgs)->GetFrameSizeInBytes());
    uintptr_t* regs = reinterpret_cast<uintptr_t*>(reinterpret_cast<byte*>(sp));
    uintptr_t caller_pc = regs[15];
#else
    UNIMPLEMENTED(FATAL);
    uintptr_t caller_pc = 0;
#endif
    uint32_t dex_pc = caller_method->ToDexPc(caller_pc);
    const DexFile::CodeItem* code = MethodHelper(caller_method).GetCodeItem();
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

    const DexFile* dex_file = caller_method->GetDeclaringClass()->GetDexCache()->GetDexFile();
    uint32_t shorty_len;
    const char* shorty =
        dex_file->GetMethodShorty(dex_file->GetMethodId(dex_method_idx), &shorty_len);
    {
      // Remember the args in case a GC happens in FindMethodFromCode.
      ScopedObjectAccessUnchecked soa(self->GetJniEnv());
      RememberForGcArgumentVisitor visitor(sp, false, shorty, shorty_len, &soa);
      visitor.VisitArguments();
      method = FindMethodFromCode<kInterface, false>(dex_method_idx, this_object, caller_method,
                                                     self);
      visitor.FixupReferences();
    }

    if (UNLIKELY(method == nullptr)) {
      CHECK(self->IsExceptionPending());
      return 0;  // Failure.
    }
  }
  const void* code = method->GetEntryPointFromQuickCompiledCode();

  // When we return, the caller will branch to this address, so it had better not be 0!
  DCHECK(code != nullptr) << "Code was NULL in method: " << PrettyMethod(method) << " location: "
      << MethodHelper(method).GetDexFile().GetLocation();
#ifdef __LP64__
  UNIMPLEMENTED(FATAL);
  return 0;
#else
  uint32_t method_uint = reinterpret_cast<uint32_t>(method);
  uint64_t code_uint = reinterpret_cast<uint32_t>(code);
  uint64_t result = ((code_uint << 32) | method_uint);
  return result;
#endif
}

}  // namespace art
