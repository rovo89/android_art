/*
 * Copyright (C) 2016 The Android Open Source Project
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

#ifndef ART_COMPILER_DEBUG_ELF_DEBUG_LINE_WRITER_H_
#define ART_COMPILER_DEBUG_ELF_DEBUG_LINE_WRITER_H_

#include <vector>

#include "compiled_method.h"
#include "debug/dwarf/debug_line_opcode_writer.h"
#include "debug/dwarf/headers.h"
#include "debug/elf_compilation_unit.h"
#include "dex_file-inl.h"
#include "dex_file.h"
#include "elf_builder.h"
#include "stack_map.h"

namespace art {
namespace debug {

typedef std::vector<DexFile::PositionInfo> PositionInfos;

static bool PositionInfoCallback(void* ctx, const DexFile::PositionInfo& entry) {
  static_cast<PositionInfos*>(ctx)->push_back(entry);
  return false;
}

template<typename ElfTypes>
class ElfDebugLineWriter {
  using Elf_Addr = typename ElfTypes::Addr;

 public:
  explicit ElfDebugLineWriter(ElfBuilder<ElfTypes>* builder) : builder_(builder) {
  }

  void Start() {
    builder_->GetDebugLine()->Start();
  }

  // Write line table for given set of methods.
  // Returns the number of bytes written.
  size_t WriteCompilationUnit(ElfCompilationUnit& compilation_unit) {
    const bool is64bit = Is64BitInstructionSet(builder_->GetIsa());
    const Elf_Addr text_address = builder_->GetText()->Exists()
        ? builder_->GetText()->GetAddress()
        : 0;

    compilation_unit.debug_line_offset = builder_->GetDebugLine()->GetSize();

    std::vector<dwarf::FileEntry> files;
    std::unordered_map<std::string, size_t> files_map;
    std::vector<std::string> directories;
    std::unordered_map<std::string, size_t> directories_map;
    int code_factor_bits_ = 0;
    int dwarf_isa = -1;
    switch (builder_->GetIsa()) {
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
    dwarf::DebugLineOpCodeWriter<> opcodes(is64bit, code_factor_bits_);
    for (const MethodDebugInfo* mi : compilation_unit.methods) {
      // Ignore function if we have already generated line table for the same address.
      // It would confuse the debugger and the DWARF specification forbids it.
      if (mi->deduped) {
        continue;
      }

      ArrayRef<const SrcMapElem> src_mapping_table;
      std::vector<SrcMapElem> src_mapping_table_from_stack_maps;
      if (mi->IsFromOptimizingCompiler()) {
        // Use stack maps to create mapping table from pc to dex.
        const CodeInfo code_info(mi->compiled_method->GetVmapTable().data());
        const StackMapEncoding encoding = code_info.ExtractEncoding();
        for (uint32_t s = 0; s < code_info.GetNumberOfStackMaps(); s++) {
          StackMap stack_map = code_info.GetStackMapAt(s, encoding);
          DCHECK(stack_map.IsValid());
          // Emit only locations where we have local-variable information.
          // In particular, skip mappings inside the prologue.
          if (stack_map.HasDexRegisterMap(encoding)) {
            const uint32_t pc = stack_map.GetNativePcOffset(encoding);
            const int32_t dex = stack_map.GetDexPc(encoding);
            src_mapping_table_from_stack_maps.push_back({pc, dex});
          }
        }
        std::sort(src_mapping_table_from_stack_maps.begin(),
                  src_mapping_table_from_stack_maps.end());
        src_mapping_table = ArrayRef<const SrcMapElem>(src_mapping_table_from_stack_maps);
      } else {
        // Use the mapping table provided by the quick compiler.
        src_mapping_table = mi->compiled_method->GetSrcMappingTable();
      }

      if (src_mapping_table.empty()) {
        continue;
      }

      Elf_Addr method_address = text_address + mi->low_pc;

      PositionInfos position_infos;
      const DexFile* dex = mi->dex_file;
      if (!dex->DecodeDebugPositionInfo(mi->code_item, PositionInfoCallback, &position_infos)) {
        continue;
      }

      if (position_infos.empty()) {
        continue;
      }

      opcodes.SetAddress(method_address);
      if (dwarf_isa != -1) {
        opcodes.SetISA(dwarf_isa);
      }

      // Get and deduplicate directory and filename.
      int file_index = 0;  // 0 - primary source file of the compilation.
      auto& dex_class_def = dex->GetClassDef(mi->class_def_index);
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
          files.push_back(dwarf::FileEntry {
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
      if (file_index != 0) {
        bool first = true;
        for (SrcMapElem pc2dex : src_mapping_table) {
          uint32_t pc = pc2dex.from_;
          int dex_pc = pc2dex.to_;
          // Find mapping with address with is greater than our dex pc; then go back one step.
          auto ub = std::upper_bound(position_infos.begin(), position_infos.end(), dex_pc,
              [](uint32_t address, const DexFile::PositionInfo& entry) {
                  return address < entry.address_;
              });
          if (ub != position_infos.begin()) {
            int line = (--ub)->line_;
            if (first) {
              first = false;
              if (pc > 0) {
                // Assume that any preceding code is prologue.
                int first_line = position_infos.front().line_;
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

      opcodes.AdvancePC(text_address + mi->high_pc);
      opcodes.EndSequence();
    }
    std::vector<uint8_t> buffer;
    buffer.reserve(opcodes.data()->size() + KB);
    size_t offset = builder_->GetDebugLine()->GetSize();
    WriteDebugLineTable(directories, files, opcodes, offset, &buffer, &debug_line_patches_);
    builder_->GetDebugLine()->WriteFully(buffer.data(), buffer.size());
    return buffer.size();
  }

  void End(bool write_oat_patches) {
    builder_->GetDebugLine()->End();
    if (write_oat_patches) {
      builder_->WritePatches(".debug_line.oat_patches",
                             ArrayRef<const uintptr_t>(debug_line_patches_));
    }
  }

 private:
  ElfBuilder<ElfTypes>* builder_;
  std::vector<uintptr_t> debug_line_patches_;
};

}  // namespace debug
}  // namespace art

#endif  // ART_COMPILER_DEBUG_ELF_DEBUG_LINE_WRITER_H_

