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

#ifndef ART_COMPILER_DWARF_DEBUG_INFO_ENTRY_WRITER_H_
#define ART_COMPILER_DWARF_DEBUG_INFO_ENTRY_WRITER_H_

#include <cstdint>
#include <unordered_map>

#include "base/casts.h"
#include "dwarf/dwarf_constants.h"
#include "dwarf/expression.h"
#include "dwarf/writer.h"
#include "leb128.h"

namespace art {
namespace dwarf {

// 32-bit FNV-1a hash function which we use to find duplicate abbreviations.
// See http://en.wikipedia.org/wiki/Fowler%E2%80%93Noll%E2%80%93Vo_hash_function
template <typename Vector>
struct FNVHash {
  static_assert(std::is_same<typename Vector::value_type, uint8_t>::value, "Invalid value type");

  size_t operator()(const Vector& v) const {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < v.size(); i++) {
      hash = (hash ^ v[i]) * 16777619u;
    }
    return hash;
  }
};

/*
 * Writer for debug information entries (DIE).
 * It also handles generation of abbreviations.
 *
 * Usage:
 *   StartTag(DW_TAG_compile_unit);
 *     WriteStrp(DW_AT_producer, "Compiler name", debug_str);
 *     StartTag(DW_TAG_subprogram);
 *       WriteStrp(DW_AT_name, "Foo", debug_str);
 *     EndTag();
 *   EndTag();
 */
template <typename Vector = std::vector<uint8_t>>
class DebugInfoEntryWriter FINAL : private Writer<Vector> {
  static_assert(std::is_same<typename Vector::value_type, uint8_t>::value, "Invalid value type");

 public:
  static constexpr size_t kCompilationUnitHeaderSize = 11;

  // Start debugging information entry.
  // Returns offset of the entry in compilation unit.
  size_t StartTag(Tag tag) {
    if (inside_entry_) {
      // Write abbrev code for the previous entry.
      // Parent entry is finalized before any children are written.
      this->UpdateUleb128(abbrev_code_offset_, EndAbbrev(DW_CHILDREN_yes));
      inside_entry_ = false;
    }
    StartAbbrev(tag);
    // Abbrev code placeholder of sufficient size.
    abbrev_code_offset_ = this->data()->size();
    this->PushUleb128(NextAbbrevCode());
    depth_++;
    inside_entry_ = true;
    return abbrev_code_offset_ + kCompilationUnitHeaderSize;
  }

  // End debugging information entry.
  void EndTag() {
    DCHECK_GT(depth_, 0);
    if (inside_entry_) {
      // Write abbrev code for this entry.
      this->UpdateUleb128(abbrev_code_offset_, EndAbbrev(DW_CHILDREN_no));
      inside_entry_ = false;
      // This entry has no children and so there is no terminator.
    } else {
      // The entry has been already finalized so it must be parent entry
      // and we need to write the terminator required by DW_CHILDREN_yes.
      this->PushUint8(0);
    }
    depth_--;
  }

  void WriteAddr(Attribute attrib, uint64_t value) {
    AddAbbrevAttribute(attrib, DW_FORM_addr);
    patch_locations_.push_back(this->data()->size());
    if (is64bit_) {
      this->PushUint64(value);
    } else {
      this->PushUint32(value);
    }
  }

  void WriteBlock(Attribute attrib, const uint8_t* ptr, size_t num_bytes) {
    AddAbbrevAttribute(attrib, DW_FORM_block);
    this->PushUleb128(num_bytes);
    this->PushData(ptr, num_bytes);
  }

  void WriteExprLoc(Attribute attrib, const Expression& expr) {
    AddAbbrevAttribute(attrib, DW_FORM_exprloc);
    this->PushUleb128(dchecked_integral_cast<uint32_t>(expr.size()));
    this->PushData(expr.data());
  }

  void WriteData1(Attribute attrib, uint8_t value) {
    AddAbbrevAttribute(attrib, DW_FORM_data1);
    this->PushUint8(value);
  }

  void WriteData2(Attribute attrib, uint16_t value) {
    AddAbbrevAttribute(attrib, DW_FORM_data2);
    this->PushUint16(value);
  }

  void WriteData4(Attribute attrib, uint32_t value) {
    AddAbbrevAttribute(attrib, DW_FORM_data4);
    this->PushUint32(value);
  }

  void WriteData8(Attribute attrib, uint64_t value) {
    AddAbbrevAttribute(attrib, DW_FORM_data8);
    this->PushUint64(value);
  }

  void WriteSecOffset(Attribute attrib, uint32_t offset) {
    AddAbbrevAttribute(attrib, DW_FORM_sec_offset);
    this->PushUint32(offset);
  }

  void WriteSdata(Attribute attrib, int value) {
    AddAbbrevAttribute(attrib, DW_FORM_sdata);
    this->PushSleb128(value);
  }

  void WriteUdata(Attribute attrib, int value) {
    AddAbbrevAttribute(attrib, DW_FORM_udata);
    this->PushUleb128(value);
  }

  void WriteUdata(Attribute attrib, uint32_t value) {
    AddAbbrevAttribute(attrib, DW_FORM_udata);
    this->PushUleb128(value);
  }

  void WriteFlag(Attribute attrib, bool value) {
    AddAbbrevAttribute(attrib, DW_FORM_flag);
    this->PushUint8(value ? 1 : 0);
  }

  void WriteFlagPresent(Attribute attrib) {
    AddAbbrevAttribute(attrib, DW_FORM_flag_present);
  }

  void WriteRef4(Attribute attrib, uint32_t cu_offset) {
    AddAbbrevAttribute(attrib, DW_FORM_ref4);
    this->PushUint32(cu_offset);
  }

  void WriteRef(Attribute attrib, uint32_t cu_offset) {
    AddAbbrevAttribute(attrib, DW_FORM_ref_udata);
    this->PushUleb128(cu_offset);
  }

  void WriteString(Attribute attrib, const char* value) {
    AddAbbrevAttribute(attrib, DW_FORM_string);
    this->PushString(value);
  }

  void WriteStrp(Attribute attrib, size_t debug_str_offset) {
    AddAbbrevAttribute(attrib, DW_FORM_strp);
    this->PushUint32(dchecked_integral_cast<uint32_t>(debug_str_offset));
  }

  void WriteStrp(Attribute attrib, const char* str, size_t len,
                 std::vector<uint8_t>* debug_str) {
    AddAbbrevAttribute(attrib, DW_FORM_strp);
    this->PushUint32(debug_str->size());
    debug_str->insert(debug_str->end(), str, str + len);
    debug_str->push_back(0);
  }

  void WriteStrp(Attribute attrib, const char* str, std::vector<uint8_t>* debug_str) {
    WriteStrp(attrib, str, strlen(str), debug_str);
  }

  bool Is64bit() const { return is64bit_; }

  const std::vector<uintptr_t>& GetPatchLocations() const {
    return patch_locations_;
  }

  int Depth() const { return depth_; }

  using Writer<Vector>::data;
  using Writer<Vector>::size;
  using Writer<Vector>::UpdateUint32;

  DebugInfoEntryWriter(bool is64bitArch,
                       Vector* debug_abbrev,
                       const typename Vector::allocator_type& alloc =
                           typename Vector::allocator_type())
      : Writer<Vector>(&entries_),
        debug_abbrev_(debug_abbrev),
        current_abbrev_(alloc),
        abbrev_codes_(alloc),
        entries_(alloc),
        is64bit_(is64bitArch) {
    debug_abbrev_.PushUint8(0);  // Add abbrev table terminator.
  }

  ~DebugInfoEntryWriter() {
    DCHECK(!inside_entry_);
    DCHECK_EQ(depth_, 0);
  }

 private:
  // Start abbreviation declaration.
  void StartAbbrev(Tag tag) {
    current_abbrev_.clear();
    EncodeUnsignedLeb128(&current_abbrev_, tag);
    has_children_offset_ = current_abbrev_.size();
    current_abbrev_.push_back(0);  // Place-holder for DW_CHILDREN.
  }

  // Add attribute specification.
  void AddAbbrevAttribute(Attribute name, Form type) {
    DCHECK(inside_entry_) << "Call StartTag before adding attributes.";
    EncodeUnsignedLeb128(&current_abbrev_, name);
    EncodeUnsignedLeb128(&current_abbrev_, type);
  }

  int NextAbbrevCode() {
    return 1 + abbrev_codes_.size();
  }

  // End abbreviation declaration and return its code.
  int EndAbbrev(Children has_children) {
    DCHECK(!current_abbrev_.empty());
    current_abbrev_[has_children_offset_] = has_children;
    auto it = abbrev_codes_.insert(std::make_pair(std::move(current_abbrev_),
                                                  NextAbbrevCode()));
    int abbrev_code = it.first->second;
    if (UNLIKELY(it.second)) {  // Inserted new entry.
      const Vector& abbrev = it.first->first;
      debug_abbrev_.Pop();  // Remove abbrev table terminator.
      debug_abbrev_.PushUleb128(abbrev_code);
      debug_abbrev_.PushData(abbrev.data(), abbrev.size());
      debug_abbrev_.PushUint8(0);  // Attribute list end.
      debug_abbrev_.PushUint8(0);  // Attribute list end.
      debug_abbrev_.PushUint8(0);  // Add abbrev table terminator.
    }
    return abbrev_code;
  }

 private:
  // Fields for writing and deduplication of abbrevs.
  Writer<Vector> debug_abbrev_;
  Vector current_abbrev_;
  size_t has_children_offset_ = 0;
  std::unordered_map<Vector, int,
                     FNVHash<Vector> > abbrev_codes_;

  // Fields for writing of debugging information entries.
  Vector entries_;
  bool is64bit_;
  int depth_ = 0;
  size_t abbrev_code_offset_ = 0;  // Location to patch once we know the code.
  bool inside_entry_ = false;  // Entry ends at first child (if any).
  std::vector<uintptr_t> patch_locations_;
};

}  // namespace dwarf
}  // namespace art

#endif  // ART_COMPILER_DWARF_DEBUG_INFO_ENTRY_WRITER_H_
