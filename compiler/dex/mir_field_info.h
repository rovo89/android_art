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

#ifndef ART_COMPILER_DEX_MIR_FIELD_INFO_H_
#define ART_COMPILER_DEX_MIR_FIELD_INFO_H_

#include "base/macros.h"
#include "dex_file.h"
#include "offsets.h"

namespace art {

class CompilerDriver;
class DexCompilationUnit;

/*
 * Field info is calculated from the perspective of the compilation unit that accesses
 * the field and stored in that unit's MIRGraph. Therefore it does not need to reference the
 * dex file or method for which it has been calculated. However, we do store the declaring
 * field index, class index and dex file of the resolved field to help distinguish between fields.
 */

class MirFieldInfo {
 public:
  uint16_t FieldIndex() const {
    return field_idx_;
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

  uint16_t DeclaringClassIndex() const {
    return declaring_class_idx_;
  }

  uint16_t DeclaringFieldIndex() const {
    return declaring_field_idx_;
  }

  bool IsVolatile() const {
    return (flags_ & kFlagIsVolatile) != 0u;
  }

 protected:
  enum {
    kBitIsStatic = 0,
    kBitIsVolatile,
    kFieldInfoBitEnd
  };
  static constexpr uint16_t kFlagIsVolatile = 1u << kBitIsVolatile;
  static constexpr uint16_t kFlagIsStatic = 1u << kBitIsStatic;

  MirFieldInfo(uint16_t field_idx, uint16_t flags)
      : field_idx_(field_idx),
        flags_(flags),
        declaring_field_idx_(0u),
        declaring_class_idx_(0u),
        declaring_dex_file_(nullptr) {
  }

  // Make copy-ctor/assign/dtor protected to avoid slicing.
  MirFieldInfo(const MirFieldInfo& other) = default;
  MirFieldInfo& operator=(const MirFieldInfo& other) = default;
  ~MirFieldInfo() = default;

  // The field index in the compiling method's dex file.
  uint16_t field_idx_;
  // Flags, for volatility and derived class data.
  uint16_t flags_;
  // The field index in the dex file that defines field, 0 if unresolved.
  uint16_t declaring_field_idx_;
  // The type index of the class declaring the field, 0 if unresolved.
  uint16_t declaring_class_idx_;
  // The dex file that defines the class containing the field and the field, nullptr if unresolved.
  const DexFile* declaring_dex_file_;
};

class MirIFieldLoweringInfo : public MirFieldInfo {
 public:
  // For each requested instance field retrieve the field's declaring location (dex file, class
  // index and field index) and volatility and compute whether we can fast path the access
  // with IGET/IPUT. For fast path fields, retrieve the field offset.
  static void Resolve(CompilerDriver* compiler_driver, const DexCompilationUnit* mUnit,
                      MirIFieldLoweringInfo* field_infos, size_t count)
      LOCKS_EXCLUDED(Locks::mutator_lock_);

  // Construct an unresolved instance field lowering info.
  explicit MirIFieldLoweringInfo(uint16_t field_idx)
      : MirFieldInfo(field_idx, kFlagIsVolatile),  // Without kFlagIsStatic.
        field_offset_(0u) {
  }

  bool FastGet() const {
    return (flags_ & kFlagFastGet) != 0u;
  }

  bool FastPut() const {
    return (flags_ & kFlagFastPut) != 0u;
  }

  MemberOffset FieldOffset() const {
    return field_offset_;
  }

 private:
  enum {
    kBitFastGet = kFieldInfoBitEnd,
    kBitFastPut,
    kIFieldLoweringInfoBitEnd
  };
  COMPILE_ASSERT(kIFieldLoweringInfoBitEnd <= 16, too_many_flags);
  static constexpr uint16_t kFlagFastGet = 1u << kBitFastGet;
  static constexpr uint16_t kFlagFastPut = 1u << kBitFastPut;

  // The member offset of the field, 0u if unresolved.
  MemberOffset field_offset_;

  friend class LocalValueNumberingTest;
};

class MirSFieldLoweringInfo : public MirFieldInfo {
 public:
  // For each requested static field retrieve the field's declaring location (dex file, class
  // index and field index) and volatility and compute whether we can fast path the access with
  // IGET/IPUT. For fast path fields (at least for IGET), retrieve the information needed for
  // the field access, i.e. the field offset, whether the field is in the same class as the
  // method being compiled, whether the declaring class can be safely assumed to be initialized
  // and the type index of the declaring class in the compiled method's dex file.
  static void Resolve(CompilerDriver* compiler_driver, const DexCompilationUnit* mUnit,
                      MirSFieldLoweringInfo* field_infos, size_t count)
      LOCKS_EXCLUDED(Locks::mutator_lock_);

  // Construct an unresolved static field lowering info.
  explicit MirSFieldLoweringInfo(uint16_t field_idx)
      : MirFieldInfo(field_idx, kFlagIsVolatile | kFlagIsStatic),
        field_offset_(0u),
        storage_index_(DexFile::kDexNoIndex) {
  }

  bool FastGet() const {
    return (flags_ & kFlagFastGet) != 0u;
  }

  bool FastPut() const {
    return (flags_ & kFlagFastPut) != 0u;
  }

  bool IsReferrersClass() const {
    return (flags_ & kFlagIsReferrersClass) != 0u;
  }

  bool IsInitialized() const {
    return (flags_ & kFlagIsInitialized) != 0u;
  }

  MemberOffset FieldOffset() const {
    return field_offset_;
  }

  uint32_t StorageIndex() const {
    return storage_index_;
  }

 private:
  enum {
    kBitFastGet = kFieldInfoBitEnd,
    kBitFastPut,
    kBitIsReferrersClass,
    kBitIsInitialized,
    kSFieldLoweringInfoBitEnd
  };
  COMPILE_ASSERT(kSFieldLoweringInfoBitEnd <= 16, too_many_flags);
  static constexpr uint16_t kFlagFastGet = 1u << kBitFastGet;
  static constexpr uint16_t kFlagFastPut = 1u << kBitFastPut;
  static constexpr uint16_t kFlagIsReferrersClass = 1u << kBitIsReferrersClass;
  static constexpr uint16_t kFlagIsInitialized = 1u << kBitIsInitialized;

  // The member offset of the field, 0u if unresolved.
  MemberOffset field_offset_;
  // The type index of the declaring class in the compiling method's dex file,
  // -1 if the field is unresolved or there's no appropriate TypeId in that dex file.
  uint32_t storage_index_;

  friend class ClassInitCheckEliminationTest;
  friend class LocalValueNumberingTest;
};

}  // namespace art

#endif  // ART_COMPILER_DEX_MIR_FIELD_INFO_H_
