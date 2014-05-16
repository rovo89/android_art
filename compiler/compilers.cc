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

#include "compilers.h"
#include "dex/mir_graph.h"
#include "dex/quick/mir_to_lir.h"
#include "elf_writer_quick.h"
#include "mirror/art_method-inl.h"

namespace art {

extern "C" void ArtInitQuickCompilerContext(art::CompilerDriver* driver);
extern "C" void ArtUnInitQuickCompilerContext(art::CompilerDriver* driver);
extern "C" art::CompiledMethod* ArtQuickCompileMethod(art::CompilerDriver* driver,
                                                      const art::DexFile::CodeItem* code_item,
                                                      uint32_t access_flags,
                                                      art::InvokeType invoke_type,
                                                      uint16_t class_def_idx,
                                                      uint32_t method_idx,
                                                      jobject class_loader,
                                                      const art::DexFile& dex_file);

extern "C" art::CompiledMethod* ArtQuickJniCompileMethod(art::CompilerDriver* driver,
                                                         uint32_t access_flags, uint32_t method_idx,
                                                         const art::DexFile& dex_file);

// Hack for CFI CIE initialization
extern std::vector<uint8_t>* X86CFIInitialization();

void QuickCompiler::Init() const {
  ArtInitQuickCompilerContext(GetCompilerDriver());
}

void QuickCompiler::UnInit() const {
  ArtUnInitQuickCompilerContext(GetCompilerDriver());
}

CompiledMethod* QuickCompiler::Compile(const DexFile::CodeItem* code_item,
                                       uint32_t access_flags,
                                       InvokeType invoke_type,
                                       uint16_t class_def_idx,
                                       uint32_t method_idx,
                                       jobject class_loader,
                                       const DexFile& dex_file) const {
  CompiledMethod* method = TryCompileWithSeaIR(code_item,
                                               access_flags,
                                               invoke_type,
                                               class_def_idx,
                                               method_idx,
                                               class_loader,
                                               dex_file);
  if (method != nullptr) {
    return method;
  }

  return ArtQuickCompileMethod(GetCompilerDriver(),
                               code_item,
                               access_flags,
                               invoke_type,
                               class_def_idx,
                               method_idx,
                               class_loader,
                               dex_file);
}

CompiledMethod* QuickCompiler::JniCompile(uint32_t access_flags,
                                          uint32_t method_idx,
                                          const DexFile& dex_file) const {
  return ArtQuickJniCompileMethod(GetCompilerDriver(), access_flags, method_idx, dex_file);
}

uintptr_t QuickCompiler::GetEntryPointOf(mirror::ArtMethod* method) const {
  return reinterpret_cast<uintptr_t>(method->GetEntryPointFromQuickCompiledCode());
}

bool QuickCompiler::WriteElf(art::File* file,
                             OatWriter* oat_writer,
                             const std::vector<const art::DexFile*>& dex_files,
                             const std::string& android_root,
                             bool is_host) const {
  return art::ElfWriterQuick::Create(file, oat_writer, dex_files, android_root, is_host,
                                     *GetCompilerDriver());
}

Backend* QuickCompiler::GetCodeGenerator(CompilationUnit* cu, void* compilation_unit) const {
  Mir2Lir* mir_to_lir = nullptr;
  switch (cu->instruction_set) {
    case kThumb2:
      mir_to_lir = ArmCodeGenerator(cu, cu->mir_graph.get(), &cu->arena);
      break;
    case kArm64:
      mir_to_lir = Arm64CodeGenerator(cu, cu->mir_graph.get(), &cu->arena);
      break;
    case kMips:
      mir_to_lir = MipsCodeGenerator(cu, cu->mir_graph.get(), &cu->arena);
      break;
    case kX86:
      mir_to_lir = X86CodeGenerator(cu, cu->mir_graph.get(), &cu->arena);
      break;
    case kX86_64:
      mir_to_lir = X86_64CodeGenerator(cu, cu->mir_graph.get(), &cu->arena);
      break;
    default:
      LOG(FATAL) << "Unexpected instruction set: " << cu->instruction_set;
  }

  /* The number of compiler temporaries depends on backend so set it up now if possible */
  if (mir_to_lir) {
    size_t max_temps = mir_to_lir->GetMaxPossibleCompilerTemps();
    bool set_max = cu->mir_graph->SetMaxAvailableNonSpecialCompilerTemps(max_temps);
    CHECK(set_max);
  }
  return mir_to_lir;
}

std::vector<uint8_t>* QuickCompiler::GetCallFrameInformationInitialization(
    const CompilerDriver& driver) const {
  if (driver.GetInstructionSet() == kX86) {
    return X86CFIInitialization();
  }
  if (driver.GetInstructionSet() == kX86_64) {
    return X86CFIInitialization();
  }
  return nullptr;
}

CompiledMethod* OptimizingCompiler::Compile(const DexFile::CodeItem* code_item,
                                            uint32_t access_flags,
                                            InvokeType invoke_type,
                                            uint16_t class_def_idx,
                                            uint32_t method_idx,
                                            jobject class_loader,
                                            const DexFile& dex_file) const {
  CompiledMethod* method = TryCompile(code_item, access_flags, invoke_type, class_def_idx,
                                      method_idx, class_loader, dex_file);
  if (method != nullptr) {
    return method;
  }

  return QuickCompiler::Compile(code_item, access_flags, invoke_type, class_def_idx, method_idx,
                                class_loader, dex_file);
}

}  // namespace art
