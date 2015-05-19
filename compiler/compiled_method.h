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
#include <string>
#include <vector>

#include "arch/instruction_set.h"
#include "base/bit_utils.h"
#include "method_reference.h"
#include "utils/array_ref.h"
#include "utils/swap_space.h"

namespace art {

class CompilerDriver;

class CompiledCode {
 public:
  // For Quick to supply an code blob
  CompiledCode(CompilerDriver* compiler_driver, InstructionSet instruction_set,
               const ArrayRef<const uint8_t>& quick_code, bool owns_code_array);

  virtual ~CompiledCode();

  InstructionSet GetInstructionSet() const {
    return instruction_set_;
  }

  const SwapVector<uint8_t>* GetQuickCode() const {
    return quick_code_;
  }

  void SetCode(const ArrayRef<const uint8_t>* quick_code);

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

  const std::vector<uint32_t>& GetOatdataOffsetsToCompliledCodeOffset() const;
  void AddOatdataOffsetToCompliledCodeOffset(uint32_t offset);

 private:
  CompilerDriver* const compiler_driver_;

  const InstructionSet instruction_set_;

  // If we own the code array (means that we free in destructor).
  const bool owns_code_array_;

  // Used to store the PIC code for Quick.
  SwapVector<uint8_t>* quick_code_;

  // There are offsets from the oatdata symbol to where the offset to
  // the compiled method will be found. These are computed by the
  // OatWriter and then used by the ElfWriter to add relocations so
  // that MCLinker can update the values to the location in the linked .so.
  std::vector<uint32_t> oatdata_offsets_to_compiled_code_offset_;
};

class SrcMapElem {
 public:
  uint32_t from_;
  int32_t to_;

  // Lexicographical compare.
  bool operator<(const SrcMapElem& other) const {
    if (from_ != other.from_) {
      return from_ < other.from_;
    }
    return to_ < other.to_;
  }
};

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
using SwapSrcMap = SrcMap<SwapAllocator<SrcMapElem>>;


enum LinkerPatchType {
  kLinkerPatchMethod,
  kLinkerPatchCall,
  kLinkerPatchCallRelative,  // NOTE: Actual patching is instruction_set-dependent.
  kLinkerPatchType,
  kLinkerPatchDexCacheArray,  // NOTE: Actual patching is instruction_set-dependent.
};

class LinkerPatch {
 public:
  static LinkerPatch MethodPatch(size_t literal_offset,
                                 const DexFile* target_dex_file,
                                 uint32_t target_method_idx) {
    LinkerPatch patch(literal_offset, kLinkerPatchMethod, target_dex_file);
    patch.method_idx_ = target_method_idx;
    return patch;
  }

  static LinkerPatch CodePatch(size_t literal_offset,
                               const DexFile* target_dex_file,
                               uint32_t target_method_idx) {
    LinkerPatch patch(literal_offset, kLinkerPatchCall, target_dex_file);
    patch.method_idx_ = target_method_idx;
    return patch;
  }

  static LinkerPatch RelativeCodePatch(size_t literal_offset,
                                       const DexFile* target_dex_file,
                                       uint32_t target_method_idx) {
    LinkerPatch patch(literal_offset, kLinkerPatchCallRelative, target_dex_file);
    patch.method_idx_ = target_method_idx;
    return patch;
  }

  static LinkerPatch TypePatch(size_t literal_offset,
                               const DexFile* target_dex_file,
                               uint32_t target_type_idx) {
    LinkerPatch patch(literal_offset, kLinkerPatchType, target_dex_file);
    patch.type_idx_ = target_type_idx;
    return patch;
  }

  static LinkerPatch DexCacheArrayPatch(size_t literal_offset,
                                        const DexFile* target_dex_file,
                                        uint32_t pc_insn_offset,
                                        size_t element_offset) {
    DCHECK(IsUint<32>(element_offset));
    LinkerPatch patch(literal_offset, kLinkerPatchDexCacheArray, target_dex_file);
    patch.pc_insn_offset_ = pc_insn_offset;
    patch.element_offset_ = element_offset;
    return patch;
  }

  LinkerPatch(const LinkerPatch& other) = default;
  LinkerPatch& operator=(const LinkerPatch& other) = default;

  size_t LiteralOffset() const {
    return literal_offset_;
  }

  LinkerPatchType Type() const {
    return patch_type_;
  }

  bool IsPcRelative() const {
    return Type() == kLinkerPatchCallRelative || Type() == kLinkerPatchDexCacheArray;
  }

  MethodReference TargetMethod() const {
    DCHECK(patch_type_ == kLinkerPatchMethod ||
           patch_type_ == kLinkerPatchCall || patch_type_ == kLinkerPatchCallRelative);
    return MethodReference(target_dex_file_, method_idx_);
  }

  const DexFile* TargetTypeDexFile() const {
    DCHECK(patch_type_ == kLinkerPatchType);
    return target_dex_file_;
  }

  uint32_t TargetTypeIndex() const {
    DCHECK(patch_type_ == kLinkerPatchType);
    return type_idx_;
  }

  const DexFile* TargetDexCacheDexFile() const {
    DCHECK(patch_type_ == kLinkerPatchDexCacheArray);
    return target_dex_file_;
  }

  size_t TargetDexCacheElementOffset() const {
    DCHECK(patch_type_ == kLinkerPatchDexCacheArray);
    return element_offset_;
  }

  uint32_t PcInsnOffset() const {
    DCHECK(patch_type_ == kLinkerPatchDexCacheArray);
    return pc_insn_offset_;
  }

 private:
  LinkerPatch(size_t literal_offset, LinkerPatchType patch_type, const DexFile* target_dex_file)
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
  LinkerPatchType patch_type_ : 8;
  union {
    uint32_t cmp1_;             // Used for relational operators.
    uint32_t method_idx_;       // Method index for Call/Method patches.
    uint32_t type_idx_;         // Type index for Type patches.
    uint32_t element_offset_;   // Element offset in the dex cache arrays.
  };
  union {
    uint32_t cmp2_;             // Used for relational operators.
    // Literal offset of the insn loading PC (same as literal_offset if it's the same insn,
    // may be different if the PC-relative addressing needs multiple insns).
    uint32_t pc_insn_offset_;
    static_assert(sizeof(pc_insn_offset_) == sizeof(cmp2_), "needed by relational operators");
  };

  friend bool operator==(const LinkerPatch& lhs, const LinkerPatch& rhs);
  friend bool operator<(const LinkerPatch& lhs, const LinkerPatch& rhs);
};

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
                 DefaultSrcMap* src_mapping_table,
                 const ArrayRef<const uint8_t>& mapping_table,
                 const ArrayRef<const uint8_t>& vmap_table,
                 const ArrayRef<const uint8_t>& native_gc_map,
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
      DefaultSrcMap* src_mapping_table,
      const ArrayRef<const uint8_t>& mapping_table,
      const ArrayRef<const uint8_t>& vmap_table,
      const ArrayRef<const uint8_t>& native_gc_map,
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

  const SwapSrcMap& GetSrcMappingTable() const {
    DCHECK(src_mapping_table_ != nullptr);
    return *src_mapping_table_;
  }

  SwapVector<uint8_t> const* GetMappingTable() const {
    return mapping_table_;
  }

  const SwapVector<uint8_t>* GetVmapTable() const {
    DCHECK(vmap_table_ != nullptr);
    return vmap_table_;
  }

  SwapVector<uint8_t> const* GetGcMap() const {
    return gc_map_;
  }

  const SwapVector<uint8_t>* GetCFIInfo() const {
    return cfi_info_;
  }

  ArrayRef<const LinkerPatch> GetPatches() const {
    return ArrayRef<const LinkerPatch>(patches_);
  }

 private:
  // Whether or not the arrays are owned by the compiled method or dedupe sets.
  const bool owns_arrays_;
  // For quick code, the size of the activation used by the code.
  const size_t frame_size_in_bytes_;
  // For quick code, a bit mask describing spilled GPR callee-save registers.
  const uint32_t core_spill_mask_;
  // For quick code, a bit mask describing spilled FPR callee-save registers.
  const uint32_t fp_spill_mask_;
  // For quick code, a set of pairs (PC, DEX) mapping from native PC offset to DEX offset.
  SwapSrcMap* src_mapping_table_;
  // For quick code, a uleb128 encoded map from native PC offset to dex PC aswell as dex PC to
  // native PC offset. Size prefixed.
  SwapVector<uint8_t>* mapping_table_;
  // For quick code, a uleb128 encoded map from GPR/FPR register to dex register. Size prefixed.
  SwapVector<uint8_t>* vmap_table_;
  // For quick code, a map keyed by native PC indices to bitmaps describing what dalvik registers
  // are live.
  SwapVector<uint8_t>* gc_map_;
  // For quick code, a FDE entry for the debug_frame section.
  SwapVector<uint8_t>* cfi_info_;
  // For quick code, linker patches needed by the method.
  const SwapVector<LinkerPatch> patches_;
};

}  // namespace art

#endif  // ART_COMPILER_COMPILED_METHOD_H_
