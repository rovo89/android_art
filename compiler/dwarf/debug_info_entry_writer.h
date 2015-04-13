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

#include "dwarf/dwarf_constants.h"
#include "dwarf/writer.h"
#include "leb128.h"

namespace art {
namespace dwarf {

// 32-bit FNV-1a hash function which we use to find duplicate abbreviations.
// See http://en.wikipedia.org/wiki/Fowler%E2%80%93Noll%E2%80%93Vo_hash_function
template< typename Allocator >
struct FNVHash {
  size_t operator()(const std::vector<uint8_t, Allocator>& v) const {
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
 *   StartTag(DW_TAG_compile_unit, DW_CHILDREN_yes);
 *     WriteStrp(DW_AT_producer, "Compiler name", debug_str);
 *     StartTag(DW_TAG_subprogram, DW_CHILDREN_no);
 *       WriteStrp(DW_AT_name, "Foo", debug_str);
 *     EndTag();
 *   EndTag();
 */
template< typename Allocator = std::allocator<uint8_t> >
class DebugInfoEntryWriter FINAL : private Writer<Allocator> {
 public:
  // Start debugging information entry.
  void StartTag(Tag tag, Children children) {
    DCHECK(has_children) << "This tag can not have nested tags";
    if (inside_entry_) {
      // Write abbrev code for the previous entry.
      this->UpdateUleb128(abbrev_code_offset_, EndAbbrev());
      inside_entry_ = false;
    }
    StartAbbrev(tag, children);
    // Abbrev code placeholder of sufficient size.
    abbrev_code_offset_ = this->data()->size();
    this->PushUleb128(NextAbbrevCode());
    depth_++;
    inside_entry_ = true;
    has_children = (children == DW_CHILDREN_yes);
  }

  // End debugging information entry.
  void EndTag() {
    DCHECK_GT(depth_, 0);
    if (inside_entry_) {
      // Write abbrev code for this tag.
      this->UpdateUleb128(abbrev_code_offset_, EndAbbrev());
      inside_entry_ = false;
    }
    if (has_children) {
      this->PushUint8(0);  // End of children.
    }
    depth_--;
    has_children = true;  // Parent tag obviously has children.
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

  void WriteBlock(Attribute attrib, const void* ptr, int size) {
    AddAbbrevAttribute(attrib, DW_FORM_block);
    this->PushUleb128(size);
    this->PushData(ptr, size);
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

  void WriteRef4(Attribute attrib, int cu_offset) {
    AddAbbrevAttribute(attrib, DW_FORM_ref4);
    this->PushUint32(cu_offset);
  }

  void WriteRef(Attribute attrib, int cu_offset) {
    AddAbbrevAttribute(attrib, DW_FORM_ref_udata);
    this->PushUleb128(cu_offset);
  }

  void WriteString(Attribute attrib, const char* value) {
    AddAbbrevAttribute(attrib, DW_FORM_string);
    this->PushString(value);
  }

  void WriteStrp(Attribute attrib, int address) {
    AddAbbrevAttribute(attrib, DW_FORM_strp);
    this->PushUint32(address);
  }

  void WriteStrp(Attribute attrib, const char* value, std::vector<uint8_t>* debug_str) {
    AddAbbrevAttribute(attrib, DW_FORM_strp);
    int address = debug_str->size();
    debug_str->insert(debug_str->end(), value, value + strlen(value) + 1);
    this->PushUint32(address);
  }

  bool Is64bit() const { return is64bit_; }

  const std::vector<uintptr_t>& GetPatchLocations() const {
    return patch_locations_;
  }

  using Writer<Allocator>::data;

  DebugInfoEntryWriter(bool is64bitArch,
                       std::vector<uint8_t, Allocator>* debug_abbrev,
                       const Allocator& alloc = Allocator())
      : Writer<Allocator>(&entries_),
        debug_abbrev_(debug_abbrev),
        current_abbrev_(alloc),
        abbrev_codes_(alloc),
        entries_(alloc),
        is64bit_(is64bitArch) {
    debug_abbrev_.PushUint8(0);  // Add abbrev table terminator.
  }

  ~DebugInfoEntryWriter() {
    DCHECK_EQ(depth_, 0);
  }

 private:
  // Start abbreviation declaration.
  void StartAbbrev(Tag tag, Children children) {
    DCHECK(!inside_entry_);
    current_abbrev_.clear();
    EncodeUnsignedLeb128(&current_abbrev_, tag);
    current_abbrev_.push_back(children);
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
  int EndAbbrev() {
    DCHECK(inside_entry_);
    auto it = abbrev_codes_.insert(std::make_pair(std::move(current_abbrev_),
                                                  NextAbbrevCode()));
    int abbrev_code = it.first->second;
    if (UNLIKELY(it.second)) {  // Inserted new entry.
      const std::vector<uint8_t, Allocator>& abbrev = it.first->first;
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
  Writer<Allocator> debug_abbrev_;
  std::vector<uint8_t, Allocator> current_abbrev_;
  std::unordered_map<std::vector<uint8_t, Allocator>, int,
                     FNVHash<Allocator> > abbrev_codes_;

  // Fields for writing of debugging information entries.
  std::vector<uint8_t, Allocator> entries_;
  bool is64bit_;
  int depth_ = 0;
  size_t abbrev_code_offset_ = 0;  // Location to patch once we know the code.
  bool inside_entry_ = false;  // Entry ends at first child (if any).
  bool has_children = true;
  std::vector<uintptr_t> patch_locations_;
};

}  // namespace dwarf
}  // namespace art

#endif  // ART_COMPILER_DWARF_DEBUG_INFO_ENTRY_WRITER_H_
