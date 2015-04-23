/*
 * Copyright (C) 2014 The Android Open Source Project
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

#ifndef ART_COMPILER_DEX_MIR_METHOD_INFO_H_
#define ART_COMPILER_DEX_MIR_METHOD_INFO_H_

#include "base/logging.h"
#include "base/macros.h"
#include "base/mutex.h"
#include "invoke_type.h"
#include "method_reference.h"

namespace art {

class CompilerDriver;
class DexCompilationUnit;
class DexFile;

class MirMethodInfo {
 public:
  uint16_t MethodIndex() const {
    return method_idx_;
  }

  bool IsStatic() const {
    return (flags_ & kFlagIsStatic) != 0u;
  }

  bool IsResolved() const {
    return declaring_dex_file_ != nullptr;
  }

  const DexFile* DeclaringDexFile() const {
    return declaring_dex_file_;
  }
  void SetDeclaringDexFile(const DexFile* dex_file) {
    declaring_dex_file_ = dex_file;
  }

  uint16_t DeclaringClassIndex() const {
    return declaring_class_idx_;
  }

  uint16_t DeclaringMethodIndex() const {
    return declaring_method_idx_;
  }

 protected:
  enum {
    kBitIsStatic = 0,
    kMethodInfoBitEnd
  };
  static_assert(kMethodInfoBitEnd <= 16, "Too many flags");
  static constexpr uint16_t kFlagIsStatic = 1u << kBitIsStatic;

  MirMethodInfo(uint16_t method_idx, uint16_t flags)
      : method_idx_(method_idx),
        flags_(flags),
        declaring_method_idx_(0u),
        declaring_class_idx_(0u),
        declaring_dex_file_(nullptr) {
  }

  // Make copy-ctor/assign/dtor protected to avoid slicing.
  MirMethodInfo(const MirMethodInfo& other) = default;
  MirMethodInfo& operator=(const MirMethodInfo& other) = default;
  ~MirMethodInfo() = default;

  // The method index in the compiling method's dex file.
  uint16_t method_idx_;
  // Flags, for volatility and derived class data.
  uint16_t flags_;
  // The method index in the dex file that defines the method, 0 if unresolved.
  uint16_t declaring_method_idx_;
  // The type index of the class declaring the method, 0 if unresolved.
  uint16_t declaring_class_idx_;
  // The dex file that defines the class containing the method and the method,
  // null if unresolved.
  const DexFile* declaring_dex_file_;
};

class MirMethodLoweringInfo : public MirMethodInfo {
 public:
  // For each requested method retrieve the method's declaring location (dex file, class
  // index and method index) and compute whether we can fast path the method call. For fast
  // path methods, retrieve the method's vtable index and direct code and method when applicable.
  static void Resolve(CompilerDriver* compiler_driver, const DexCompilationUnit* mUnit,
                      MirMethodLoweringInfo* method_infos, size_t count)
      LOCKS_EXCLUDED(Locks::mutator_lock_);

  MirMethodLoweringInfo(uint16_t method_idx, InvokeType type, bool is_quickened)
      : MirMethodInfo(method_idx,
                      ((type == kStatic) ? kFlagIsStatic : 0u) |
                      (static_cast<uint16_t>(type) << kBitInvokeTypeBegin) |
                      (static_cast<uint16_t>(type) << kBitSharpTypeBegin) |
                      (is_quickened ? kFlagQuickened : 0u)),
        direct_code_(0u),
        direct_method_(0u),
        target_dex_file_(nullptr),
        target_method_idx_(0u),
        vtable_idx_(0u),
        stats_flags_(0) {
  }

  void SetDevirtualizationTarget(const MethodReference& ref) {
    DCHECK(target_dex_file_ == nullptr);
    DCHECK_EQ(target_method_idx_, 0u);
    DCHECK_LE(ref.dex_method_index, 0xffffu);
    target_dex_file_ = ref.dex_file;
    target_method_idx_ = ref.dex_method_index;
  }

  bool FastPath() const {
    return (flags_ & kFlagFastPath) != 0u;
  }

  bool IsIntrinsic() const {
    return (flags_ & kFlagIsIntrinsic) != 0u;
  }

  bool IsSpecial() const {
    return (flags_ & kFlagIsSpecial) != 0u;
  }

  bool IsReferrersClass() const {
    return (flags_ & kFlagIsReferrersClass) != 0;
  }

  bool IsClassInitialized() const {
    return (flags_ & kFlagClassIsInitialized) != 0u;
  }

  // Returns true iff the method invoke is INVOKE_VIRTUAL_QUICK or INVOKE_VIRTUAL_RANGE_QUICK.
  bool IsQuickened() const {
    return (flags_ & kFlagQuickened) != 0u;
  }

  InvokeType GetInvokeType() const {
    return static_cast<InvokeType>((flags_ >> kBitInvokeTypeBegin) & kInvokeTypeMask);
  }

  art::InvokeType GetSharpType() const {
    return static_cast<InvokeType>((flags_ >> kBitSharpTypeBegin) & kInvokeTypeMask);
  }

  MethodReference GetTargetMethod() const {
    return MethodReference(target_dex_file_, target_method_idx_);
  }

  uint16_t VTableIndex() const {
    return vtable_idx_;
  }
  void SetVTableIndex(uint16_t index) {
    vtable_idx_ = index;
  }

  uintptr_t DirectCode() const {
    return direct_code_;
  }

  uintptr_t DirectMethod() const {
    return direct_method_;
  }

  int StatsFlags() const {
    return stats_flags_;
  }

  void CheckEquals(const MirMethodLoweringInfo& info) const {
    CHECK_EQ(method_idx_, info.method_idx_);
    CHECK_EQ(flags_, info.flags_);
    CHECK_EQ(declaring_method_idx_, info.declaring_method_idx_);
    CHECK_EQ(declaring_class_idx_, info.declaring_class_idx_);
    CHECK_EQ(declaring_dex_file_, info.declaring_dex_file_);
    CHECK_EQ(direct_code_, info.direct_code_);
    CHECK_EQ(direct_method_, info.direct_method_);
    CHECK_EQ(target_dex_file_, info.target_dex_file_);
    CHECK_EQ(target_method_idx_, info.target_method_idx_);
    CHECK_EQ(vtable_idx_, info.vtable_idx_);
    CHECK_EQ(stats_flags_, info.stats_flags_);
  }

 private:
  enum {
    kBitFastPath = kMethodInfoBitEnd,
    kBitIsIntrinsic,
    kBitIsSpecial,
    kBitInvokeTypeBegin,
    kBitInvokeTypeEnd = kBitInvokeTypeBegin + 3,  // 3 bits for invoke type.
    kBitSharpTypeBegin = kBitInvokeTypeEnd,
    kBitSharpTypeEnd = kBitSharpTypeBegin + 3,  // 3 bits for sharp type.
    kBitIsReferrersClass = kBitSharpTypeEnd,
    kBitClassIsInitialized,
    kBitQuickened,
    kMethodLoweringInfoBitEnd
  };
  static_assert(kMethodLoweringInfoBitEnd <= 16, "Too many flags");
  static constexpr uint16_t kFlagFastPath = 1u << kBitFastPath;
  static constexpr uint16_t kFlagIsIntrinsic = 1u << kBitIsIntrinsic;
  static constexpr uint16_t kFlagIsSpecial = 1u << kBitIsSpecial;
  static constexpr uint16_t kFlagIsReferrersClass = 1u << kBitIsReferrersClass;
  static constexpr uint16_t kFlagClassIsInitialized = 1u << kBitClassIsInitialized;
  static constexpr uint16_t kFlagQuickened = 1u << kBitQuickened;
  static constexpr uint16_t kInvokeTypeMask = 7u;
  static_assert((1u << (kBitInvokeTypeEnd - kBitInvokeTypeBegin)) - 1u == kInvokeTypeMask,
                "assert invoke type bits failed");
  static_assert((1u << (kBitSharpTypeEnd - kBitSharpTypeBegin)) - 1u == kInvokeTypeMask,
                "assert sharp type bits failed");

  uintptr_t direct_code_;
  uintptr_t direct_method_;
  // Before Resolve(), target_dex_file_ and target_method_idx_ hold the verification-based
  // devirtualized invoke target if available, null and 0u otherwise.
  // After Resolve() they hold the actual target method that will be called; it will be either
  // a devirtualized target method or the compilation's unit's dex file and MethodIndex().
  const DexFile* target_dex_file_;
  uint16_t target_method_idx_;
  uint16_t vtable_idx_;
  int stats_flags_;

  friend class MirOptimizationTest;
  friend class TypeInferenceTest;
};

}  // namespace art

#endif  // ART_COMPILER_DEX_MIR_METHOD_INFO_H_
