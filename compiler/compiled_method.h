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

#ifndef ART_COMPILER_COMPILED_METHOD_H_
#define ART_COMPILER_COMPILED_METHOD_H_

#include <memory>
#include <iosfwd>
#include <string>
#include <vector>

#include "arch/instruction_set.h"
#include "base/bit_utils.h"
#include "base/length_prefixed_array.h"
#include "method_reference.h"
#include "utils/array_ref.h"

namespace art {

class CompilerDriver;
class CompiledMethodStorage;

class CompiledCode {
 public:
  // For Quick to supply an code blob
  CompiledCode(CompilerDriver* compiler_driver, InstructionSet instruction_set,
               const ArrayRef<const uint8_t>& quick_code);

  virtual ~CompiledCode();

  InstructionSet GetInstructionSet() const {
    return instruction_set_;
  }

  ArrayRef<const uint8_t> GetQuickCode() const {
    return GetArray(quick_code_);
  }

  bool operator==(const CompiledCode& rhs) const;

  // To align an offset from a page-aligned value to make it suitable
  // for code storage. For example on ARM, to ensure that PC relative
  // valu computations work out as expected.
  size_t AlignCode(size_t offset) const;
  static size_t AlignCode(size_t offset, InstructionSet instruction_set);

  // returns the difference between the code address and a usable PC.
  // mainly to cope with kThumb2 where the lower bit must be set.
  size_t CodeDelta() const;
  static size_t CodeDelta(InstructionSet instruction_set);

  // Returns a pointer suitable for invoking the code at the argument
  // code_pointer address.  Mainly to cope with kThumb2 where the
  // lower bit must be set to indicate Thumb mode.
  static const void* CodePointer(const void* code_pointer,
                                 InstructionSet instruction_set);

 protected:
  template <typename T>
  static ArrayRef<const T> GetArray(const LengthPrefixedArray<T>* array) {
    if (array == nullptr) {
      return ArrayRef<const T>();
    }
    DCHECK_NE(array->size(), 0u);
    return ArrayRef<const T>(&array->At(0), array->size());
  }

  CompilerDriver* GetCompilerDriver() {
    return compiler_driver_;
  }

 private:
  CompilerDriver* const compiler_driver_;

  const InstructionSet instruction_set_;

  // Used to store the PIC code for Quick.
  const LengthPrefixedArray<uint8_t>* const quick_code_;
};

class SrcMapElem {
 public:
  uint32_t from_;
  int32_t to_;
};

inline bool operator<(const SrcMapElem& lhs, const SrcMapElem& rhs) {
  if (lhs.from_ != rhs.from_) {
    return lhs.from_ < rhs.from_;
  }
  return lhs.to_ < rhs.to_;
}

inline bool operator==(const SrcMapElem& lhs, const SrcMapElem& rhs) {
  return lhs.from_ == rhs.from_ && lhs.to_ == rhs.to_;
}

template <class Allocator>
class SrcMap FINAL : public std::vector<SrcMapElem, Allocator> {
 public:
  using std::vector<SrcMapElem, Allocator>::begin;
  using typename std::vector<SrcMapElem, Allocator>::const_iterator;
  using std::vector<SrcMapElem, Allocator>::empty;
  using std::vector<SrcMapElem, Allocator>::end;
  using std::vector<SrcMapElem, Allocator>::resize;
  using std::vector<SrcMapElem, Allocator>::shrink_to_fit;
  using std::vector<SrcMapElem, Allocator>::size;

  explicit SrcMap() {}
  explicit SrcMap(const Allocator& alloc) : std::vector<SrcMapElem, Allocator>(alloc) {}

  template <class InputIt>
  SrcMap(InputIt first, InputIt last, const Allocator& alloc)
      : std::vector<SrcMapElem, Allocator>(first, last, alloc) {}

  void push_back(const SrcMapElem& elem) {
    if (!empty()) {
      // Check that the addresses are inserted in sorted order.
      DCHECK_GE(elem.from_, this->back().from_);
      // If two consequitive entries map to the same value, ignore the later.
      // E.g. for map {{0, 1}, {4, 1}, {8, 2}}, all values in [0,8) map to 1.
      if (elem.to_ == this->back().to_) {
        return;
      }
    }
    std::vector<SrcMapElem, Allocator>::push_back(elem);
  }

  // Returns true and the corresponding "to" value if the mapping is found.
  // Oterwise returns false and 0.
  std::pair<bool, int32_t> Find(uint32_t from) const {
    // Finds first mapping such that lb.from_ >= from.
    auto lb = std::lower_bound(begin(), end(), SrcMapElem {from, INT32_MIN});
    if (lb != end() && lb->from_ == from) {
      // Found exact match.
      return std::make_pair(true, lb->to_);
    } else if (lb != begin()) {
      // The previous mapping is still in effect.
      return std::make_pair(true, (--lb)->to_);
    } else {
      // Not found because 'from' is smaller than first entry in the map.
      return std::make_pair(false, 0);
    }
  }
};

using DefaultSrcMap = SrcMap<std::allocator<SrcMapElem>>;

class LinkerPatch {
 public:
  // Note: We explicitly specify the underlying type of the enum because GCC
  // would otherwise select a bigger underlying type and then complain that
  //     'art::LinkerPatch::patch_type_' is too small to hold all
  //     values of 'enum class art::LinkerPatch::Type'
  // which is ridiculous given we have only a handful of values here. If we
  // choose to squeeze the Type into fewer than 8 bits, we'll have to declare
  // patch_type_ as an uintN_t and do explicit static_cast<>s.
  enum class Type : uint8_t {
    kRecordPosition,   // Just record patch position for patchoat.
    kMethod,
    kCall,
    kCallRelative,     // NOTE: Actual patching is instruction_set-dependent.
    kType,
    kString,
    kStringRelative,   // NOTE: Actual patching is instruction_set-dependent.
    kDexCacheArray,    // NOTE: Actual patching is instruction_set-dependent.
  };

  static LinkerPatch RecordPosition(size_t literal_offset) {
    return LinkerPatch(literal_offset, Type::kRecordPosition, /* target_dex_file */ nullptr);
  }

  static LinkerPatch MethodPatch(size_t literal_offset,
                                 const DexFile* target_dex_file,
                                 uint32_t target_method_idx) {
    LinkerPatch patch(literal_offset, Type::kMethod, target_dex_file);
    patch.method_idx_ = target_method_idx;
    return patch;
  }

  static LinkerPatch CodePatch(size_t literal_offset,
                               const DexFile* target_dex_file,
                               uint32_t target_method_idx) {
    LinkerPatch patch(literal_offset, Type::kCall, target_dex_file);
    patch.method_idx_ = target_method_idx;
    return patch;
  }

  static LinkerPatch RelativeCodePatch(size_t literal_offset,
                                       const DexFile* target_dex_file,
                                       uint32_t target_method_idx) {
    LinkerPatch patch(literal_offset, Type::kCallRelative, target_dex_file);
    patch.method_idx_ = target_method_idx;
    return patch;
  }

  static LinkerPatch TypePatch(size_t literal_offset,
                               const DexFile* target_dex_file,
                               uint32_t target_type_idx) {
    LinkerPatch patch(literal_offset, Type::kType, target_dex_file);
    patch.type_idx_ = target_type_idx;
    return patch;
  }

  static LinkerPatch StringPatch(size_t literal_offset,
                                 const DexFile* target_dex_file,
                                 uint32_t target_string_idx) {
    LinkerPatch patch(literal_offset, Type::kString, target_dex_file);
    patch.string_idx_ = target_string_idx;
    return patch;
  }

  static LinkerPatch RelativeStringPatch(size_t literal_offset,
                                         const DexFile* target_dex_file,
                                         uint32_t pc_insn_offset,
                                         uint32_t target_string_idx) {
    LinkerPatch patch(literal_offset, Type::kStringRelative, target_dex_file);
    patch.string_idx_ = target_string_idx;
    patch.pc_insn_offset_ = pc_insn_offset;
    return patch;
  }

  static LinkerPatch DexCacheArrayPatch(size_t literal_offset,
                                        const DexFile* target_dex_file,
                                        uint32_t pc_insn_offset,
                                        size_t element_offset) {
    DCHECK(IsUint<32>(element_offset));
    LinkerPatch patch(literal_offset, Type::kDexCacheArray, target_dex_file);
    patch.pc_insn_offset_ = pc_insn_offset;
    patch.element_offset_ = element_offset;
    return patch;
  }

  LinkerPatch(const LinkerPatch& other) = default;
  LinkerPatch& operator=(const LinkerPatch& other) = default;

  size_t LiteralOffset() const {
    return literal_offset_;
  }

  Type GetType() const {
    return patch_type_;
  }

  bool IsPcRelative() const {
    switch (GetType()) {
      case Type::kCallRelative:
      case Type::kStringRelative:
      case Type::kDexCacheArray:
        return true;
      default:
        return false;
    }
  }

  MethodReference TargetMethod() const {
    DCHECK(patch_type_ == Type::kMethod ||
           patch_type_ == Type::kCall ||
           patch_type_ == Type::kCallRelative);
    return MethodReference(target_dex_file_, method_idx_);
  }

  const DexFile* TargetTypeDexFile() const {
    DCHECK(patch_type_ == Type::kType);
    return target_dex_file_;
  }

  uint32_t TargetTypeIndex() const {
    DCHECK(patch_type_ == Type::kType);
    return type_idx_;
  }

  const DexFile* TargetStringDexFile() const {
    DCHECK(patch_type_ == Type::kString || patch_type_ == Type::kStringRelative);
    return target_dex_file_;
  }

  uint32_t TargetStringIndex() const {
    DCHECK(patch_type_ == Type::kString || patch_type_ == Type::kStringRelative);
    return string_idx_;
  }

  const DexFile* TargetDexCacheDexFile() const {
    DCHECK(patch_type_ == Type::kDexCacheArray);
    return target_dex_file_;
  }

  size_t TargetDexCacheElementOffset() const {
    DCHECK(patch_type_ == Type::kDexCacheArray);
    return element_offset_;
  }

  uint32_t PcInsnOffset() const {
    DCHECK(patch_type_ == Type::kStringRelative || patch_type_ == Type::kDexCacheArray);
    return pc_insn_offset_;
  }

 private:
  LinkerPatch(size_t literal_offset, Type patch_type, const DexFile* target_dex_file)
      : target_dex_file_(target_dex_file),
        literal_offset_(literal_offset),
        patch_type_(patch_type) {
    cmp1_ = 0u;
    cmp2_ = 0u;
    // The compiler rejects methods that are too big, so the compiled code
    // of a single method really shouln't be anywhere close to 16MiB.
    DCHECK(IsUint<24>(literal_offset));
  }

  const DexFile* target_dex_file_;
  uint32_t literal_offset_ : 24;  // Method code size up to 16MiB.
  Type patch_type_ : 8;
  union {
    uint32_t cmp1_;             // Used for relational operators.
    uint32_t method_idx_;       // Method index for Call/Method patches.
    uint32_t type_idx_;         // Type index for Type patches.
    uint32_t string_idx_;       // String index for String patches.
    uint32_t element_offset_;   // Element offset in the dex cache arrays.
    static_assert(sizeof(method_idx_) == sizeof(cmp1_), "needed by relational operators");
    static_assert(sizeof(type_idx_) == sizeof(cmp1_), "needed by relational operators");
    static_assert(sizeof(string_idx_) == sizeof(cmp1_), "needed by relational operators");
    static_assert(sizeof(element_offset_) == sizeof(cmp1_), "needed by relational operators");
  };
  union {
    // Note: To avoid uninitialized padding on 64-bit systems, we use `size_t` for `cmp2_`.
    // This allows a hashing function to treat an array of linker patches as raw memory.
    size_t cmp2_;             // Used for relational operators.
    // Literal offset of the insn loading PC (same as literal_offset if it's the same insn,
    // may be different if the PC-relative addressing needs multiple insns).
    uint32_t pc_insn_offset_;
    static_assert(sizeof(pc_insn_offset_) <= sizeof(cmp2_), "needed by relational operators");
  };

  friend bool operator==(const LinkerPatch& lhs, const LinkerPatch& rhs);
  friend bool operator<(const LinkerPatch& lhs, const LinkerPatch& rhs);
};
std::ostream& operator<<(std::ostream& os, const LinkerPatch::Type& type);

inline bool operator==(const LinkerPatch& lhs, const LinkerPatch& rhs) {
  return lhs.literal_offset_ == rhs.literal_offset_ &&
      lhs.patch_type_ == rhs.patch_type_ &&
      lhs.target_dex_file_ == rhs.target_dex_file_ &&
      lhs.cmp1_ == rhs.cmp1_ &&
      lhs.cmp2_ == rhs.cmp2_;
}

inline bool operator<(const LinkerPatch& lhs, const LinkerPatch& rhs) {
  return (lhs.literal_offset_ != rhs.literal_offset_) ? lhs.literal_offset_ < rhs.literal_offset_
      : (lhs.patch_type_ != rhs.patch_type_) ? lhs.patch_type_ < rhs.patch_type_
      : (lhs.target_dex_file_ != rhs.target_dex_file_) ? lhs.target_dex_file_ < rhs.target_dex_file_
      : (lhs.cmp1_ != rhs.cmp1_) ? lhs.cmp1_ < rhs.cmp1_
      : lhs.cmp2_ < rhs.cmp2_;
}

class CompiledMethod FINAL : public CompiledCode {
 public:
  // Constructs a CompiledMethod.
  // Note: Consider using the static allocation methods below that will allocate the CompiledMethod
  //       in the swap space.
  CompiledMethod(CompilerDriver* driver,
                 InstructionSet instruction_set,
                 const ArrayRef<const uint8_t>& quick_code,
                 const size_t frame_size_in_bytes,
                 const uint32_t core_spill_mask,
                 const uint32_t fp_spill_mask,
                 const ArrayRef<const SrcMapElem>& src_mapping_table,
                 const ArrayRef<const uint8_t>& vmap_table,
                 const ArrayRef<const uint8_t>& cfi_info,
                 const ArrayRef<const LinkerPatch>& patches);

  virtual ~CompiledMethod();

  static CompiledMethod* SwapAllocCompiledMethod(
      CompilerDriver* driver,
      InstructionSet instruction_set,
      const ArrayRef<const uint8_t>& quick_code,
      const size_t frame_size_in_bytes,
      const uint32_t core_spill_mask,
      const uint32_t fp_spill_mask,
      const ArrayRef<const SrcMapElem>& src_mapping_table,
      const ArrayRef<const uint8_t>& vmap_table,
      const ArrayRef<const uint8_t>& cfi_info,
      const ArrayRef<const LinkerPatch>& patches);

  static void ReleaseSwapAllocatedCompiledMethod(CompilerDriver* driver, CompiledMethod* m);

  size_t GetFrameSizeInBytes() const {
    return frame_size_in_bytes_;
  }

  uint32_t GetCoreSpillMask() const {
    return core_spill_mask_;
  }

  uint32_t GetFpSpillMask() const {
    return fp_spill_mask_;
  }

  ArrayRef<const SrcMapElem> GetSrcMappingTable() const {
    return GetArray(src_mapping_table_);
  }

  ArrayRef<const uint8_t> GetVmapTable() const {
    return GetArray(vmap_table_);
  }

  ArrayRef<const uint8_t> GetCFIInfo() const {
    return GetArray(cfi_info_);
  }

  ArrayRef<const LinkerPatch> GetPatches() const {
    return GetArray(patches_);
  }

 private:
  // For quick code, the size of the activation used by the code.
  const size_t frame_size_in_bytes_;
  // For quick code, a bit mask describing spilled GPR callee-save registers.
  const uint32_t core_spill_mask_;
  // For quick code, a bit mask describing spilled FPR callee-save registers.
  const uint32_t fp_spill_mask_;
  // For quick code, a set of pairs (PC, DEX) mapping from native PC offset to DEX offset.
  const LengthPrefixedArray<SrcMapElem>* const src_mapping_table_;
  // For quick code, a uleb128 encoded map from GPR/FPR register to dex register. Size prefixed.
  const LengthPrefixedArray<uint8_t>* const vmap_table_;
  // For quick code, a FDE entry for the debug_frame section.
  const LengthPrefixedArray<uint8_t>* const cfi_info_;
  // For quick code, linker patches needed by the method.
  const LengthPrefixedArray<LinkerPatch>* const patches_;
};

}  // namespace art

#endif  // ART_COMPILER_COMPILED_METHOD_H_
