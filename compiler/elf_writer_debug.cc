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

#include "compiled_method.h"
#include "driver/compiler_driver.h"
#include "dex_file-inl.h"
#include "dwarf/headers.h"
#include "dwarf/register.h"
#include "oat_writer.h"

namespace art {
namespace dwarf {

static void WriteEhFrameCIE(InstructionSet isa, std::vector<uint8_t>* eh_frame) {
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
      auto return_address_reg = Reg::ArmCore(14);  // R14(LR).
      WriteEhFrameCIE(is64bit, return_address_reg, opcodes, eh_frame);
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
      auto return_address_reg = Reg::Arm64Core(30);  // R30(LR).
      WriteEhFrameCIE(is64bit, return_address_reg, opcodes, eh_frame);
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
      auto return_address_reg = Reg::MipsCore(31);  // R31(RA).
      WriteEhFrameCIE(is64bit, return_address_reg, opcodes, eh_frame);
      return;
    }
    case kX86: {
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
      for (int reg = 0; reg < 8; reg++) {
        opcodes.Undefined(Reg::X86Fp(reg));
      }
      auto return_address_reg = Reg::X86Core(8);  // R8(EIP).
      WriteEhFrameCIE(is64bit, return_address_reg, opcodes, eh_frame);
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
      auto return_address_reg = Reg::X86_64Core(16);  // R16(RIP).
      WriteEhFrameCIE(is64bit, return_address_reg, opcodes, eh_frame);
      return;
    }
    case kNone:
      break;
  }
  LOG(FATAL) << "Can not write CIE frame for ISA " << isa;
  UNREACHABLE();
}

void WriteEhFrame(const CompilerDriver* compiler,
                  OatWriter* oat_writer,
                  uint32_t text_section_offset,
                  std::vector<uint8_t>* eh_frame) {
  const auto& method_infos = oat_writer->GetMethodDebugInfo();
  const InstructionSet isa = compiler->GetInstructionSet();
  size_t cie_offset = eh_frame->size();
  auto* eh_frame_patches = oat_writer->GetAbsolutePatchLocationsFor(".eh_frame");
  WriteEhFrameCIE(isa, eh_frame);
  for (const OatWriter::DebugInfo& mi : method_infos) {
    const SwapVector<uint8_t>* opcodes = mi.compiled_method_->GetCFIInfo();
    if (opcodes != nullptr) {
      WriteEhFrameFDE(Is64BitInstructionSet(isa), cie_offset,
                      text_section_offset + mi.low_pc_, mi.high_pc_ - mi.low_pc_,
                      opcodes, eh_frame, eh_frame_patches);
    }
  }
}

/*
 * @brief Generate the DWARF sections.
 * @param oat_writer The Oat file Writer.
 * @param eh_frame Call Frame Information.
 * @param debug_info Compilation unit information.
 * @param debug_abbrev Abbreviations used to generate dbg_info.
 * @param debug_str Debug strings.
 * @param debug_line Line number table.
 */
void WriteDebugSections(const CompilerDriver* compiler,
                        OatWriter* oat_writer,
                        uint32_t text_section_offset,
                        std::vector<uint8_t>* debug_info,
                        std::vector<uint8_t>* debug_abbrev,
                        std::vector<uint8_t>* debug_str,
                        std::vector<uint8_t>* debug_line) {
  const std::vector<OatWriter::DebugInfo>& method_infos = oat_writer->GetMethodDebugInfo();
  const InstructionSet isa = compiler->GetInstructionSet();
  uint32_t cunit_low_pc = static_cast<uint32_t>(-1);
  uint32_t cunit_high_pc = 0;
  for (auto method_info : method_infos) {
    cunit_low_pc = std::min(cunit_low_pc, method_info.low_pc_);
    cunit_high_pc = std::max(cunit_high_pc, method_info.high_pc_);
  }

  // Write .debug_info section.
  size_t debug_abbrev_offset = debug_abbrev->size();
  DebugInfoEntryWriter<> info(false /* 32 bit */, debug_abbrev);
  info.StartTag(DW_TAG_compile_unit, DW_CHILDREN_yes);
  info.WriteStrp(DW_AT_producer, "Android dex2oat", debug_str);
  info.WriteData1(DW_AT_language, DW_LANG_Java);
  info.WriteAddr(DW_AT_low_pc, cunit_low_pc + text_section_offset);
  info.WriteAddr(DW_AT_high_pc, cunit_high_pc + text_section_offset);
  info.WriteData4(DW_AT_stmt_list, debug_line->size());
  for (auto method_info : method_infos) {
    std::string method_name = PrettyMethod(method_info.dex_method_index_,
                                           *method_info.dex_file_, true);
    if (method_info.deduped_) {
      // TODO We should place the DEDUPED tag on the first instance of a deduplicated symbol
      // so that it will show up in a debuggerd crash report.
      method_name += " [ DEDUPED ]";
    }
    info.StartTag(DW_TAG_subprogram, DW_CHILDREN_no);
    info.WriteStrp(DW_AT_name, method_name.data(), debug_str);
    info.WriteAddr(DW_AT_low_pc, method_info.low_pc_ + text_section_offset);
    info.WriteAddr(DW_AT_high_pc, method_info.high_pc_ + text_section_offset);
    info.EndTag();  // DW_TAG_subprogram
  }
  info.EndTag();  // DW_TAG_compile_unit
  auto* debug_info_patches = oat_writer->GetAbsolutePatchLocationsFor(".debug_info");
  WriteDebugInfoCU(debug_abbrev_offset, info, debug_info, debug_info_patches);

  // TODO: in gdb info functions <regexp> - reports Java functions, but
  // source file is <unknown> because .debug_line is formed as one
  // compilation unit. To fix this it is possible to generate
  // a separate compilation unit for every distinct Java source.
  // Each of the these compilation units can have several non-adjacent
  // method ranges.

  // Write .debug_line section.
  std::vector<FileEntry> files;
  std::unordered_map<std::string, size_t> files_map;
  std::vector<std::string> directories;
  std::unordered_map<std::string, size_t> directories_map;
  int code_factor_bits_ = 0;
  int dwarf_isa = -1;
  switch (isa) {
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
  DebugLineOpCodeWriter<> opcodes(false /* 32bit */, code_factor_bits_);
  opcodes.SetAddress(text_section_offset + cunit_low_pc);
  if (dwarf_isa != -1) {
    opcodes.SetISA(dwarf_isa);
  }
  for (const OatWriter::DebugInfo& mi : method_infos) {
    // Addresses in the line table should be unique and increasing.
    if (mi.deduped_) {
      continue;
    }

    struct DebugInfoCallbacks {
      static bool NewPosition(void* ctx, uint32_t address, uint32_t line) {
        auto* context = reinterpret_cast<DebugInfoCallbacks*>(ctx);
        context->dex2line_.push_back({address, static_cast<int32_t>(line)});
        return false;
      }
      DefaultSrcMap dex2line_;
    } debug_info_callbacks;

    const DexFile* dex = mi.dex_file_;
    if (mi.code_item_ != nullptr) {
      dex->DecodeDebugInfo(mi.code_item_,
                           (mi.access_flags_ & kAccStatic) != 0,
                           mi.dex_method_index_,
                           DebugInfoCallbacks::NewPosition,
                           nullptr,
                           &debug_info_callbacks);
    }

    // Get and deduplicate directory and filename.
    int file_index = 0;  // 0 - primary source file of the compilation.
    auto& dex_class_def = dex->GetClassDef(mi.class_def_index_);
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
    uint32_t low_pc = text_section_offset + mi.low_pc_;
    if (file_index != 0 && !dex2line_map.empty()) {
      bool first = true;
      for (SrcMapElem pc2dex : mi.compiled_method_->GetSrcMappingTable()) {
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
              opcodes.AddRow(low_pc, first_line);
              opcodes.NegateStmt();
              opcodes.SetPrologueEnd();
            }
            opcodes.AddRow(low_pc + pc, line);
          } else if (line != opcodes.CurrentLine()) {
            opcodes.AddRow(low_pc + pc, line);
          }
        }
      }
    } else {
      // line 0 - instruction cannot be attributed to any source line.
      opcodes.AddRow(low_pc, 0);
    }
  }
  opcodes.AdvancePC(text_section_offset + cunit_high_pc);
  opcodes.EndSequence();
  auto* debug_line_patches = oat_writer->GetAbsolutePatchLocationsFor(".debug_line");
  WriteDebugLineTable(directories, files, opcodes, debug_line, debug_line_patches);
}

}  // namespace dwarf
}  // namespace art
