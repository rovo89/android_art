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

#ifndef ART_COMPILER_DEBUG_ELF_SYMTAB_WRITER_H_
#define ART_COMPILER_DEBUG_ELF_SYMTAB_WRITER_H_

#include <unordered_set>

#include "debug/method_debug_info.h"
#include "elf_builder.h"
#include "utils.h"

namespace art {
namespace debug {

// The ARM specification defines three special mapping symbols
// $a, $t and $d which mark ARM, Thumb and data ranges respectively.
// These symbols can be used by tools, for example, to pretty
// print instructions correctly.  Objdump will use them if they
// exist, but it will still work well without them.
// However, these extra symbols take space, so let's just generate
// one symbol which marks the whole .text section as code.
constexpr bool kGenerateSingleArmMappingSymbol = true;

template <typename ElfTypes>
static void WriteDebugSymbols(ElfBuilder<ElfTypes>* builder,
                              const ArrayRef<const MethodDebugInfo>& method_infos,
                              bool with_signature) {
  uint64_t mapping_symbol_address = std::numeric_limits<uint64_t>::max();
  auto* strtab = builder->GetStrTab();
  auto* symtab = builder->GetSymTab();

  if (method_infos.empty()) {
    return;
  }

  // Find all addresses which contain deduped methods.
  // The first instance of method is not marked deduped_, but the rest is.
  std::unordered_set<uint64_t> deduped_addresses;
  for (const MethodDebugInfo& info : method_infos) {
    if (info.deduped) {
      deduped_addresses.insert(info.code_address);
    }
  }

  strtab->Start();
  strtab->Write("");  // strtab should start with empty string.
  std::string last_name;
  size_t last_name_offset = 0;
  for (const MethodDebugInfo& info : method_infos) {
    if (info.deduped) {
      continue;  // Add symbol only for the first instance.
    }
    size_t name_offset;
    if (info.trampoline_name != nullptr) {
      name_offset = strtab->Write(info.trampoline_name);
    } else {
      DCHECK(info.dex_file != nullptr);
      std::string name = PrettyMethod(info.dex_method_index, *info.dex_file, with_signature);
      if (deduped_addresses.find(info.code_address) != deduped_addresses.end()) {
        name += " [DEDUPED]";
      }
      // If we write method names without signature, we might see the same name multiple times.
      name_offset = (name == last_name ? last_name_offset : strtab->Write(name));
      last_name = std::move(name);
      last_name_offset = name_offset;
    }

    const auto* text = info.is_code_address_text_relative ? builder->GetText() : nullptr;
    uint64_t address = info.code_address + (text != nullptr ? text->GetAddress() : 0);
    // Add in code delta, e.g., thumb bit 0 for Thumb2 code.
    address += CompiledMethod::CodeDelta(info.isa);
    symtab->Add(name_offset, text, address, info.code_size, STB_GLOBAL, STT_FUNC);

    // Conforming to aaelf, add $t mapping symbol to indicate start of a sequence of thumb2
    // instructions, so that disassembler tools can correctly disassemble.
    // Note that even if we generate just a single mapping symbol, ARM's Streamline
    // requires it to match function symbol.  Just address 0 does not work.
    if (info.isa == kThumb2) {
      if (address < mapping_symbol_address || !kGenerateSingleArmMappingSymbol) {
        symtab->Add(strtab->Write("$t"), text, address & ~1, 0, STB_LOCAL, STT_NOTYPE);
        mapping_symbol_address = address;
      }
    }
  }
  strtab->End();

  // Symbols are buffered and written after names (because they are smaller).
  // We could also do two passes in this function to avoid the buffering.
  symtab->Start();
  symtab->Write();
  symtab->End();
}

}  // namespace debug
}  // namespace art

#endif  // ART_COMPILER_DEBUG_ELF_SYMTAB_WRITER_H_

