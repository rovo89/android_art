/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include "elf_writer_debug.h"

#include <unordered_set>

#include "base/casts.h"
#include "compiled_method.h"
#include "driver/compiler_driver.h"
#include "dex_file-inl.h"
#include "dwarf/headers.h"
#include "dwarf/register.h"
#include "elf_builder.h"
#include "oat_writer.h"
#include "utils.h"

namespace art {
namespace dwarf {

static void WriteCIE(InstructionSet isa,
                     CFIFormat format,
                     std::vector<uint8_t>* buffer) {
  // Scratch registers should be marked as undefined.  This tells the
  // debugger that its value in the previous frame is not recoverable.
  bool is64bit = Is64BitInstructionSet(isa);
  switch (isa) {
    case kArm:
    case kThumb2: {
      DebugFrameOpCodeWriter<> opcodes;
      opcodes.DefCFA(Reg::ArmCore(13), 0);  // R13(SP).
      // core registers.
      for (int reg = 0; reg < 13; reg++) {
        if (reg < 4 || reg == 12) {
          opcodes.Undefined(Reg::ArmCore(reg));
        } else {
          opcodes.SameValue(Reg::ArmCore(reg));
        }
      }
      // fp registers.
      for (int reg = 0; reg < 32; reg++) {
        if (reg < 16) {
          opcodes.Undefined(Reg::ArmFp(reg));
        } else {
          opcodes.SameValue(Reg::ArmFp(reg));
        }
      }
      auto return_reg = Reg::ArmCore(14);  // R14(LR).
      WriteCIE(is64bit, return_reg, opcodes, format, buffer);
      return;
    }
    case kArm64: {
      DebugFrameOpCodeWriter<> opcodes;
      opcodes.DefCFA(Reg::Arm64Core(31), 0);  // R31(SP).
      // core registers.
      for (int reg = 0; reg < 30; reg++) {
        if (reg < 8 || reg == 16 || reg == 17) {
          opcodes.Undefined(Reg::Arm64Core(reg));
        } else {
          opcodes.SameValue(Reg::Arm64Core(reg));
        }
      }
      // fp registers.
      for (int reg = 0; reg < 32; reg++) {
        if (reg < 8 || reg >= 16) {
          opcodes.Undefined(Reg::Arm64Fp(reg));
        } else {
          opcodes.SameValue(Reg::Arm64Fp(reg));
        }
      }
      auto return_reg = Reg::Arm64Core(30);  // R30(LR).
      WriteCIE(is64bit, return_reg, opcodes, format, buffer);
      return;
    }
    case kMips:
    case kMips64: {
      DebugFrameOpCodeWriter<> opcodes;
      opcodes.DefCFA(Reg::MipsCore(29), 0);  // R29(SP).
      // core registers.
      for (int reg = 1; reg < 26; reg++) {
        if (reg < 16 || reg == 24 || reg == 25) {  // AT, V*, A*, T*.
          opcodes.Undefined(Reg::MipsCore(reg));
        } else {
          opcodes.SameValue(Reg::MipsCore(reg));
        }
      }
      auto return_reg = Reg::MipsCore(31);  // R31(RA).
      WriteCIE(is64bit, return_reg, opcodes, format, buffer);
      return;
    }
    case kX86: {
      // FIXME: Add fp registers once libunwind adds support for them. Bug: 20491296
      constexpr bool generate_opcodes_for_x86_fp = false;
      DebugFrameOpCodeWriter<> opcodes;
      opcodes.DefCFA(Reg::X86Core(4), 4);   // R4(ESP).
      opcodes.Offset(Reg::X86Core(8), -4);  // R8(EIP).
      // core registers.
      for (int reg = 0; reg < 8; reg++) {
        if (reg <= 3) {
          opcodes.Undefined(Reg::X86Core(reg));
        } else if (reg == 4) {
          // Stack pointer.
        } else {
          opcodes.SameValue(Reg::X86Core(reg));
        }
      }
      // fp registers.
      if (generate_opcodes_for_x86_fp) {
        for (int reg = 0; reg < 8; reg++) {
          opcodes.Undefined(Reg::X86Fp(reg));
        }
      }
      auto return_reg = Reg::X86Core(8);  // R8(EIP).
      WriteCIE(is64bit, return_reg, opcodes, format, buffer);
      return;
    }
    case kX86_64: {
      DebugFrameOpCodeWriter<> opcodes;
      opcodes.DefCFA(Reg::X86_64Core(4), 8);  // R4(RSP).
      opcodes.Offset(Reg::X86_64Core(16), -8);  // R16(RIP).
      // core registers.
      for (int reg = 0; reg < 16; reg++) {
        if (reg == 4) {
          // Stack pointer.
        } else if (reg < 12 && reg != 3 && reg != 5) {  // except EBX and EBP.
          opcodes.Undefined(Reg::X86_64Core(reg));
        } else {
          opcodes.SameValue(Reg::X86_64Core(reg));
        }
      }
      // fp registers.
      for (int reg = 0; reg < 16; reg++) {
        if (reg < 12) {
          opcodes.Undefined(Reg::X86_64Fp(reg));
        } else {
          opcodes.SameValue(Reg::X86_64Fp(reg));
        }
      }
      auto return_reg = Reg::X86_64Core(16);  // R16(RIP).
      WriteCIE(is64bit, return_reg, opcodes, format, buffer);
      return;
    }
    case kNone:
      break;
  }
  LOG(FATAL) << "Can not write CIE frame for ISA " << isa;
  UNREACHABLE();
}

template<typename ElfTypes>
void WriteCFISection(ElfBuilder<ElfTypes>* builder,
                     const std::vector<OatWriter::DebugInfo>& method_infos,
                     CFIFormat format) {
  CHECK(format == dwarf::DW_DEBUG_FRAME_FORMAT ||
        format == dwarf::DW_EH_FRAME_FORMAT);
  typedef typename ElfTypes::Addr Elf_Addr;

  std::vector<uint32_t> binary_search_table;
  std::vector<uintptr_t> patch_locations;
  if (format == DW_EH_FRAME_FORMAT) {
    binary_search_table.reserve(2 * method_infos.size());
  } else {
    patch_locations.reserve(method_infos.size());
  }

  // Write .eh_frame/.debug_frame section.
  auto* cfi_section = (format == dwarf::DW_DEBUG_FRAME_FORMAT
                       ? builder->GetDebugFrame()
                       : builder->GetEhFrame());
  {
    cfi_section->Start();
    const bool is64bit = Is64BitInstructionSet(builder->GetIsa());
    const Elf_Addr text_address = builder->GetText()->GetAddress();
    const Elf_Addr cfi_address = cfi_section->GetAddress();
    const Elf_Addr cie_address = cfi_address;
    Elf_Addr buffer_address = cfi_address;
    std::vector<uint8_t> buffer;  // Small temporary buffer.
    WriteCIE(builder->GetIsa(), format, &buffer);
    cfi_section->WriteFully(buffer.data(), buffer.size());
    buffer_address += buffer.size();
    buffer.clear();
    for (const OatWriter::DebugInfo& mi : method_infos) {
      if (!mi.deduped_) {  // Only one FDE per unique address.
        ArrayRef<const uint8_t> opcodes = mi.compiled_method_->GetCFIInfo();
        if (!opcodes.empty()) {
          const Elf_Addr code_address = text_address + mi.low_pc_;
          if (format == DW_EH_FRAME_FORMAT) {
            binary_search_table.push_back(
                dchecked_integral_cast<uint32_t>(code_address));
            binary_search_table.push_back(
                dchecked_integral_cast<uint32_t>(buffer_address));
          }
          WriteFDE(is64bit, cfi_address, cie_address,
                   code_address, mi.high_pc_ - mi.low_pc_,
                   opcodes, format, buffer_address, &buffer,
                   &patch_locations);
          cfi_section->WriteFully(buffer.data(), buffer.size());
          buffer_address += buffer.size();
          buffer.clear();
        }
      }
    }
    cfi_section->End();
  }

  if (format == DW_EH_FRAME_FORMAT) {
    auto* header_section = builder->GetEhFrameHdr();
    header_section->Start();
    uint32_t header_address = dchecked_integral_cast<int32_t>(header_section->GetAddress());
    // Write .eh_frame_hdr section.
    std::vector<uint8_t> buffer;
    Writer<> header(&buffer);
    header.PushUint8(1);  // Version.
    // Encoding of .eh_frame pointer - libunwind does not honor datarel here,
    // so we have to use pcrel which means relative to the pointer's location.
    header.PushUint8(DW_EH_PE_pcrel | DW_EH_PE_sdata4);
    // Encoding of binary search table size.
    header.PushUint8(DW_EH_PE_udata4);
    // Encoding of binary search table addresses - libunwind supports only this
    // specific combination, which means relative to the start of .eh_frame_hdr.
    header.PushUint8(DW_EH_PE_datarel | DW_EH_PE_sdata4);
    // .eh_frame pointer
    header.PushInt32(cfi_section->GetAddress() - (header_address + 4u));
    // Binary search table size (number of entries).
    header.PushUint32(dchecked_integral_cast<uint32_t>(binary_search_table.size()/2));
    header_section->WriteFully(buffer.data(), buffer.size());
    // Binary search table.
    for (size_t i = 0; i < binary_search_table.size(); i++) {
      // Make addresses section-relative since we know the header address now.
      binary_search_table[i] -= header_address;
    }
    header_section->WriteFully(binary_search_table.data(), binary_search_table.size());
    header_section->End();
  } else {
    builder->WritePatches(".debug_frame.oat_patches", &patch_locations);
  }
}

template<typename ElfTypes>
void WriteDebugSections(ElfBuilder<ElfTypes>* builder,
                        const std::vector<OatWriter::DebugInfo>& method_infos) {
  typedef typename ElfTypes::Addr Elf_Addr;
  const bool is64bit = Is64BitInstructionSet(builder->GetIsa());
  Elf_Addr text_address = builder->GetText()->GetAddress();

  // Find all addresses (low_pc) which contain deduped methods.
  // The first instance of method is not marked deduped_, but the rest is.
  std::unordered_set<uint32_t> deduped_addresses;
  for (const OatWriter::DebugInfo& mi : method_infos) {
    if (mi.deduped_) {
      deduped_addresses.insert(mi.low_pc_);
    }
  }

  // Group the methods into compilation units based on source file.
  std::vector<std::vector<const OatWriter::DebugInfo*>> compilation_units;
  const char* last_source_file = nullptr;
  for (const OatWriter::DebugInfo& mi : method_infos) {
    // Attribute given instruction range only to single method.
    // Otherwise the debugger might get really confused.
    if (!mi.deduped_) {
      auto& dex_class_def = mi.dex_file_->GetClassDef(mi.class_def_index_);
      const char* source_file = mi.dex_file_->GetSourceFile(dex_class_def);
      if (compilation_units.empty() || source_file != last_source_file) {
        compilation_units.push_back(std::vector<const OatWriter::DebugInfo*>());
      }
      compilation_units.back().push_back(&mi);
      last_source_file = source_file;
    }
  }

  // Write .debug_info section.
  std::vector<uint8_t> debug_info;
  std::vector<uintptr_t> debug_info_patches;
  std::vector<uint8_t> debug_abbrev;
  std::vector<uint8_t> debug_str;
  std::vector<uint8_t> debug_line;
  std::vector<uintptr_t> debug_line_patches;
  for (const auto& compilation_unit : compilation_units) {
    uint32_t cunit_low_pc = 0xFFFFFFFFU;
    uint32_t cunit_high_pc = 0;
    for (auto method_info : compilation_unit) {
      cunit_low_pc = std::min(cunit_low_pc, method_info->low_pc_);
      cunit_high_pc = std::max(cunit_high_pc, method_info->high_pc_);
    }

    size_t debug_abbrev_offset = debug_abbrev.size();
    DebugInfoEntryWriter<> info(is64bit, &debug_abbrev);
    info.StartTag(DW_TAG_compile_unit, DW_CHILDREN_yes);
    info.WriteStrp(DW_AT_producer, "Android dex2oat", &debug_str);
    info.WriteData1(DW_AT_language, DW_LANG_Java);
    info.WriteAddr(DW_AT_low_pc, text_address + cunit_low_pc);
    info.WriteAddr(DW_AT_high_pc, text_address + cunit_high_pc);
    info.WriteData4(DW_AT_stmt_list, debug_line.size());
    for (auto method_info : compilation_unit) {
      std::string method_name = PrettyMethod(method_info->dex_method_index_,
                                             *method_info->dex_file_, true);
      if (deduped_addresses.find(method_info->low_pc_) != deduped_addresses.end()) {
        method_name += " [DEDUPED]";
      }
      info.StartTag(DW_TAG_subprogram, DW_CHILDREN_no);
      info.WriteStrp(DW_AT_name, method_name.data(), &debug_str);
      info.WriteAddr(DW_AT_low_pc, text_address + method_info->low_pc_);
      info.WriteAddr(DW_AT_high_pc, text_address + method_info->high_pc_);
      info.EndTag();  // DW_TAG_subprogram
    }
    info.EndTag();  // DW_TAG_compile_unit
    WriteDebugInfoCU(debug_abbrev_offset, info, &debug_info, &debug_info_patches);

    // Write .debug_line section.
    std::vector<FileEntry> files;
    std::unordered_map<std::string, size_t> files_map;
    std::vector<std::string> directories;
    std::unordered_map<std::string, size_t> directories_map;
    int code_factor_bits_ = 0;
    int dwarf_isa = -1;
    switch (builder->GetIsa()) {
      case kArm:  // arm actually means thumb2.
      case kThumb2:
        code_factor_bits_ = 1;  // 16-bit instuctions
        dwarf_isa = 1;  // DW_ISA_ARM_thumb.
        break;
      case kArm64:
      case kMips:
      case kMips64:
        code_factor_bits_ = 2;  // 32-bit instructions
        break;
      case kNone:
      case kX86:
      case kX86_64:
        break;
    }
    DebugLineOpCodeWriter<> opcodes(is64bit, code_factor_bits_);
    opcodes.SetAddress(text_address + cunit_low_pc);
    if (dwarf_isa != -1) {
      opcodes.SetISA(dwarf_isa);
    }
    for (const OatWriter::DebugInfo* mi : compilation_unit) {
      struct DebugInfoCallbacks {
        static bool NewPosition(void* ctx, uint32_t address, uint32_t line) {
          auto* context = reinterpret_cast<DebugInfoCallbacks*>(ctx);
          context->dex2line_.push_back({address, static_cast<int32_t>(line)});
          return false;
        }
        DefaultSrcMap dex2line_;
      } debug_info_callbacks;

      Elf_Addr method_address = text_address + mi->low_pc_;

      const DexFile* dex = mi->dex_file_;
      if (mi->code_item_ != nullptr) {
        dex->DecodeDebugInfo(mi->code_item_,
                             (mi->access_flags_ & kAccStatic) != 0,
                             mi->dex_method_index_,
                             DebugInfoCallbacks::NewPosition,
                             nullptr,
                             &debug_info_callbacks);
      }

      // Get and deduplicate directory and filename.
      int file_index = 0;  // 0 - primary source file of the compilation.
      auto& dex_class_def = dex->GetClassDef(mi->class_def_index_);
      const char* source_file = dex->GetSourceFile(dex_class_def);
      if (source_file != nullptr) {
        std::string file_name(source_file);
        size_t file_name_slash = file_name.find_last_of('/');
        std::string class_name(dex->GetClassDescriptor(dex_class_def));
        size_t class_name_slash = class_name.find_last_of('/');
        std::string full_path(file_name);

        // Guess directory from package name.
        int directory_index = 0;  // 0 - current directory of the compilation.
        if (file_name_slash == std::string::npos &&  // Just filename.
            class_name.front() == 'L' &&  // Type descriptor for a class.
            class_name_slash != std::string::npos) {  // Has package name.
          std::string package_name = class_name.substr(1, class_name_slash - 1);
          auto it = directories_map.find(package_name);
          if (it == directories_map.end()) {
            directory_index = 1 + directories.size();
            directories_map.emplace(package_name, directory_index);
            directories.push_back(package_name);
          } else {
            directory_index = it->second;
          }
          full_path = package_name + "/" + file_name;
        }

        // Add file entry.
        auto it2 = files_map.find(full_path);
        if (it2 == files_map.end()) {
          file_index = 1 + files.size();
          files_map.emplace(full_path, file_index);
          files.push_back(FileEntry {
            file_name,
            directory_index,
            0,  // Modification time - NA.
            0,  // File size - NA.
          });
        } else {
          file_index = it2->second;
        }
      }
      opcodes.SetFile(file_index);

      // Generate mapping opcodes from PC to Java lines.
      const DefaultSrcMap& dex2line_map = debug_info_callbacks.dex2line_;
      if (file_index != 0 && !dex2line_map.empty()) {
        bool first = true;
        for (SrcMapElem pc2dex : mi->compiled_method_->GetSrcMappingTable()) {
          uint32_t pc = pc2dex.from_;
          int dex_pc = pc2dex.to_;
          auto dex2line = dex2line_map.Find(static_cast<uint32_t>(dex_pc));
          if (dex2line.first) {
            int line = dex2line.second;
            if (first) {
              first = false;
              if (pc > 0) {
                // Assume that any preceding code is prologue.
                int first_line = dex2line_map.front().to_;
                // Prologue is not a sensible place for a breakpoint.
                opcodes.NegateStmt();
                opcodes.AddRow(method_address, first_line);
                opcodes.NegateStmt();
                opcodes.SetPrologueEnd();
              }
              opcodes.AddRow(method_address + pc, line);
            } else if (line != opcodes.CurrentLine()) {
              opcodes.AddRow(method_address + pc, line);
            }
          }
        }
      } else {
        // line 0 - instruction cannot be attributed to any source line.
        opcodes.AddRow(method_address, 0);
      }
    }
    opcodes.AdvancePC(text_address + cunit_high_pc);
    opcodes.EndSequence();
    WriteDebugLineTable(directories, files, opcodes, &debug_line, &debug_line_patches);
  }
  builder->WriteSection(".debug_line", &debug_line);
  builder->WritePatches(".debug_line.oat_patches", &debug_line_patches);
  builder->WriteSection(".debug_info", &debug_info);
  builder->WritePatches(".debug_info.oat_patches", &debug_info_patches);
  builder->WriteSection(".debug_abbrev", &debug_abbrev);
  builder->WriteSection(".debug_str", &debug_str);
}

// Explicit instantiations
template void WriteCFISection<ElfTypes32>(
    ElfBuilder<ElfTypes32>* builder,
    const std::vector<OatWriter::DebugInfo>& method_infos,
    CFIFormat format);
template void WriteCFISection<ElfTypes64>(
    ElfBuilder<ElfTypes64>* builder,
    const std::vector<OatWriter::DebugInfo>& method_infos,
    CFIFormat format);
template void WriteDebugSections<ElfTypes32>(
    ElfBuilder<ElfTypes32>* builder,
    const std::vector<OatWriter::DebugInfo>& method_infos);
template void WriteDebugSections<ElfTypes64>(
    ElfBuilder<ElfTypes64>* builder,
    const std::vector<OatWriter::DebugInfo>& method_infos);

}  // namespace dwarf
}  // namespace art
