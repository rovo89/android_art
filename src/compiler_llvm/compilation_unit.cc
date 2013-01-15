/*
 * Copyright (C) 2012 The Android Open Source Project
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

#include "compilation_unit.h"

#include "base/logging.h"
#include "compiled_method.h"
#include "compiler_llvm.h"
#include "instruction_set.h"
#include "ir_builder.h"
#include "os.h"

#include "runtime_support_builder_arm.h"
#include "runtime_support_builder_thumb2.h"
#include "runtime_support_builder_x86.h"

#include <llvm/ADT/OwningPtr.h>
#include <llvm/ADT/StringSet.h>
#include <llvm/ADT/Triple.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/Analysis/DebugInfo.h>
#include <llvm/Analysis/Dominators.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/LoopPass.h>
#include <llvm/Analysis/RegionPass.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/Analysis/Verifier.h>
#include <llvm/Assembly/PrintModulePass.h>
#include <llvm/Bitcode/ReaderWriter.h>
#include <llvm/CallGraphSCCPass.h>
#include <llvm/CodeGen/MachineFrameInfo.h>
#include <llvm/CodeGen/MachineFunction.h>
#include <llvm/CodeGen/MachineFunctionPass.h>
#include <llvm/DerivedTypes.h>
#include <llvm/LLVMContext.h>
#include <llvm/Module.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/PassManager.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/ELF.h>
#include <llvm/Support/FormattedStream.h>
#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/PassNameParser.h>
#include <llvm/Support/PluginLoader.h>
#include <llvm/Support/PrettyStackTrace.h>
#include <llvm/Support/Signals.h>
#include <llvm/Support/SystemUtils.h>
#include <llvm/Support/TargetRegistry.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/ToolOutputFile.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/system_error.h>
#include <llvm/Target/TargetData.h>
#include <llvm/Target/TargetLibraryInfo.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Transforms/IPO.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>
#include <llvm/Transforms/Scalar.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>

namespace art {
namespace compiler_llvm {

#if defined(ART_USE_PORTABLE_COMPILER)
llvm::FunctionPass*
CreateGBCExpanderPass(const greenland::IntrinsicHelper& intrinsic_helper, IRBuilder& irb,
                      Compiler* compiler, OatCompilationUnit* oat_compilation_unit);
#endif

llvm::Module* makeLLVMModuleContents(llvm::Module* module);


CompilationUnit::CompilationUnit(const CompilerLLVM* compiler_llvm,
                                 size_t cunit_idx)
: compiler_llvm_(compiler_llvm), cunit_idx_(cunit_idx) {
#if !defined(ART_USE_PORTABLE_COMPILER)
  context_.reset(new llvm::LLVMContext());
  module_ = new llvm::Module("art", *context_);
#else
  compiler_ = NULL;
  oat_compilation_unit_ = NULL;
  llvm_info_.reset(new LLVMInfo());
  context_.reset(llvm_info_->GetLLVMContext());
  module_ = llvm_info_->GetLLVMModule();
#endif

  // Include the runtime function declaration
  makeLLVMModuleContents(module_);

  // Create IRBuilder
  irb_.reset(new IRBuilder(*context_, *module_));

  // We always need a switch case, so just use a normal function.
  switch(GetInstructionSet()) {
  default:
    runtime_support_.reset(new RuntimeSupportBuilder(*context_, *module_, *irb_));
    break;
  case kArm:
    runtime_support_.reset(new RuntimeSupportBuilderARM(*context_, *module_, *irb_));
    break;
  case kThumb2:
    runtime_support_.reset(new RuntimeSupportBuilderThumb2(*context_, *module_, *irb_));
    break;
  case kX86:
    runtime_support_.reset(new RuntimeSupportBuilderX86(*context_, *module_, *irb_));
    break;
  }

  irb_->SetRuntimeSupport(runtime_support_.get());
}


CompilationUnit::~CompilationUnit() {
#if defined(ART_USE_PORTABLE_COMPILER)
  llvm::LLVMContext* llvm_context = context_.release(); // Managed by llvm_info_
  CHECK(llvm_context != NULL);
#endif
}


InstructionSet CompilationUnit::GetInstructionSet() const {
  return compiler_llvm_->GetInstructionSet();
}


bool CompilationUnit::Materialize() {
  std::string elf_image;

  // Compile and prelink llvm::Module
  if (!MaterializeToString(elf_image)) {
    LOG(ERROR) << "Failed to materialize compilation unit " << cunit_idx_;
    return false;
  }

#if 0
  // Dump the ELF image for debugging
  std::string filename(StringPrintf("%s/Art%zu.elf",
                                    GetArtCacheOrDie(GetAndroidData()).c_str(),
                                    cunit_idx_));
  UniquePtr<File> output(OS::OpenFile(filename.c_str(), true));
  output->WriteFully(elf_image.data(), elf_image.size());
#endif

  // Extract the .text section and prelink the code
  if (!ExtractCodeAndPrelink(elf_image)) {
    LOG(ERROR) << "Failed to extract code from compilation unit " << cunit_idx_;
    return false;
  }

  return true;
}


bool CompilationUnit::MaterializeToString(std::string& str_buffer) {
  llvm::raw_string_ostream str_os(str_buffer);
  return MaterializeToRawOStream(str_os);
}


bool CompilationUnit::MaterializeToRawOStream(llvm::raw_ostream& out_stream) {
  // Lookup the LLVM target
  const char* target_triple = NULL;
  const char* target_cpu = "";
  const char* target_attr = NULL;

  InstructionSet insn_set = GetInstructionSet();
  switch (insn_set) {
  case kThumb2:
    target_triple = "thumb-none-linux-gnueabi";
    target_cpu = "cortex-a9";
    target_attr = "+thumb2,+neon,+neonfp,+vfp3,+db";
    break;

  case kArm:
    target_triple = "armv7-none-linux-gnueabi";
    // TODO: Fix for Nexus S.
    target_cpu = "cortex-a9";
    // TODO: Fix for Xoom.
    target_attr = "+v7,+neon,+neonfp,+vfp3,+db";
    break;

  case kX86:
    target_triple = "i386-pc-linux-gnu";
    target_attr = "";
    break;

  case kMips:
    target_triple = "mipsel-unknown-linux";
    target_attr = "mips32r2";
    break;

  default:
    LOG(FATAL) << "Unknown instruction set: " << insn_set;
  }

  std::string errmsg;
  const llvm::Target* target =
    llvm::TargetRegistry::lookupTarget(target_triple, errmsg);

  CHECK(target != NULL) << errmsg;

  // Target options
  llvm::TargetOptions target_options;
  target_options.FloatABIType = llvm::FloatABI::Soft;
  target_options.NoFramePointerElim = true;
  target_options.NoFramePointerElimNonLeaf = true;
  target_options.UseSoftFloat = false;
  target_options.EnableFastISel = false;

  // Create the llvm::TargetMachine
  llvm::OwningPtr<llvm::TargetMachine> target_machine(
    target->createTargetMachine(target_triple, target_cpu, target_attr, target_options,
                                llvm::Reloc::Static, llvm::CodeModel::Small,
                                llvm::CodeGenOpt::Aggressive));

  CHECK(target_machine.get() != NULL) << "Failed to create target machine";

  // Add target data
  const llvm::TargetData* target_data = target_machine->getTargetData();

  // PassManager for code generation passes
  llvm::PassManager pm;
  pm.add(new llvm::TargetData(*target_data));

  // FunctionPassManager for optimization pass
  llvm::FunctionPassManager fpm(module_);
  fpm.add(new llvm::TargetData(*target_data));

  if (bitcode_filename_.empty()) {
    // If we don't need write the bitcode to file, add the AddSuspendCheckToLoopLatchPass to the
    // regular FunctionPass.
#if defined(ART_USE_PORTABLE_COMPILER)
    fpm.add(CreateGBCExpanderPass(*llvm_info_->GetIntrinsicHelper(), *irb_.get(),
                                  compiler_, oat_compilation_unit_));
#endif
  } else {
#if defined(ART_USE_PORTABLE_COMPILER)
    llvm::FunctionPassManager fpm2(module_);
    fpm2.add(CreateGBCExpanderPass(*llvm_info_->GetIntrinsicHelper(), *irb_.get(),
                                   compiler_, oat_compilation_unit_));
    fpm2.doInitialization();
    for (llvm::Module::iterator F = module_->begin(), E = module_->end();
         F != E; ++F) {
      fpm2.run(*F);
    }
    fpm2.doFinalization();
#endif

    // Write bitcode to file
    std::string errmsg;

    llvm::OwningPtr<llvm::tool_output_file> out_file(
      new llvm::tool_output_file(bitcode_filename_.c_str(), errmsg,
                                 llvm::raw_fd_ostream::F_Binary));


    if (!errmsg.empty()) {
      LOG(ERROR) << "Failed to create bitcode output file: " << errmsg;
      return false;
    }

    llvm::WriteBitcodeToFile(module_, out_file->os());
    out_file->keep();
  }

  // Add optimization pass
  llvm::PassManagerBuilder pm_builder;
  // TODO: Use inliner after we can do IPO.
  pm_builder.Inliner = NULL;
  //pm_builder.Inliner = llvm::createFunctionInliningPass();
  //pm_builder.Inliner = llvm::createAlwaysInlinerPass();
  //pm_builder.Inliner = llvm::createPartialInliningPass();
  pm_builder.OptLevel = 3;
  pm_builder.DisableSimplifyLibCalls = 1;
  pm_builder.DisableUnitAtATime = 1;
  pm_builder.populateFunctionPassManager(fpm);
  pm_builder.populateModulePassManager(pm);
  pm.add(llvm::createStripDeadPrototypesPass());

  // Add passes to emit ELF image
  {
    llvm::formatted_raw_ostream formatted_os(out_stream, false);

    // Ask the target to add backend passes as necessary.
    if (target_machine->addPassesToEmitFile(pm,
                                            formatted_os,
                                            llvm::TargetMachine::CGFT_ObjectFile,
                                            true)) {
      LOG(FATAL) << "Unable to generate ELF for this target";
      return false;
    }

    // Run the per-function optimization
    fpm.doInitialization();
    for (llvm::Module::iterator F = module_->begin(), E = module_->end();
         F != E; ++F) {
      fpm.run(*F);
    }
    fpm.doFinalization();

    // Run the code generation passes
    pm.run(*module_);
  }

  return true;
}


bool CompilationUnit::ExtractCodeAndPrelink(const std::string& elf_image) {
  if (GetInstructionSet() == kX86) {
    compiled_code_.push_back(0xccU);
    compiled_code_.push_back(0xccU);
    compiled_code_.push_back(0xccU);
    compiled_code_.push_back(0xccU);
    return true;
  }

  llvm::OwningPtr<llvm::MemoryBuffer> elf_image_buff(
    llvm::MemoryBuffer::getMemBuffer(llvm::StringRef(elf_image.data(),
                                                     elf_image.size())));

  llvm::OwningPtr<llvm::object::ObjectFile> elf_file(
    llvm::object::ObjectFile::createELFObjectFile(elf_image_buff.take()));

  llvm::error_code ec;

  const ProcedureLinkageTable& plt = compiler_llvm_->GetProcedureLinkageTable();

  for (llvm::object::section_iterator
       sec_iter = elf_file->begin_sections(),
       sec_end = elf_file->end_sections();
       sec_iter != sec_end; sec_iter.increment(ec)) {

    CHECK(ec == 0) << "Failed to read section because " << ec.message();

    // Read the section information
    llvm::StringRef name;
    uint64_t alignment = 0u;
    uint64_t size = 0u;

    CHECK(sec_iter->getName(name) == 0);
    CHECK(sec_iter->getSize(size) == 0);
    CHECK(sec_iter->getAlignment(alignment) == 0);

    if (name == ".data" || name == ".bss" || name == ".rodata") {
      if (size > 0) {
        LOG(FATAL) << "Compilation unit " << cunit_idx_ << " has non-empty "
                   << name.str() << " section";
      }

    } else if (name == "" || name == ".rel.text" ||
               name == ".ARM.attributes" || name == ".symtab" ||
               name == ".strtab" || name == ".shstrtab") {
      // We can ignore these sections.  We don't have to copy them into
      // the result Oat file.

    } else if (name == ".text") {
      // Ensure the alignment requirement is less than or equal to
      // kArchAlignment
      CheckCodeAlign(alignment);

      // Copy the compiled code
      llvm::StringRef contents;
      CHECK(sec_iter->getContents(contents) == 0);

      copy(contents.data(),
           contents.data() + contents.size(),
           back_inserter(compiled_code_));

      // Prelink the compiled code
      for (llvm::object::relocation_iterator
           rel_iter = sec_iter->begin_relocations(),
           rel_end = sec_iter->end_relocations(); rel_iter != rel_end;
           rel_iter.increment(ec)) {

        CHECK(ec == 0) << "Failed to read relocation because " << ec.message();

        // Read the relocation information
        llvm::object::SymbolRef sym_ref;
        uint64_t rel_offset = 0;
        uint64_t rel_type = 0;
        int64_t rel_addend = 0;

        CHECK(rel_iter->getSymbol(sym_ref) == 0);
        CHECK(rel_iter->getOffset(rel_offset) == 0);
        CHECK(rel_iter->getType(rel_type) == 0);
        CHECK(rel_iter->getAdditionalInfo(rel_addend) == 0);

        // Read the symbol related to this relocation fixup
        llvm::StringRef sym_name;
        CHECK(sym_ref.getName(sym_name) == 0);

        // Relocate the fixup.
        // TODO: Support more relocation type.
        CHECK(rel_type == llvm::ELF::R_ARM_ABS32);
        CHECK_LE(rel_offset + 4, compiled_code_.size());

        uintptr_t dest_addr = plt.GetEntryAddress(sym_name.str().c_str());
        uintptr_t final_addr = dest_addr + rel_addend;
        compiled_code_[rel_offset] = final_addr & 0xff;
        compiled_code_[rel_offset + 1] = (final_addr >> 8) & 0xff;
        compiled_code_[rel_offset + 2] = (final_addr >> 16) & 0xff;
        compiled_code_[rel_offset + 3] = (final_addr >> 24) & 0xff;
      }

    } else {
      LOG(WARNING) << "Unexpected section: " << name.str();
    }
  }

  return true;
}


// Check whether the align is less than or equal to the code alignment of
// that architecture.  Since the Oat writer only guarantee that the compiled
// method being aligned to kArchAlignment, we have no way to align the ELf
// section if the section alignment is greater than kArchAlignment.
void CompilationUnit::CheckCodeAlign(uint32_t align) const {
  InstructionSet insn_set = GetInstructionSet();
  switch (insn_set) {
  case kThumb2:
  case kArm:
    CHECK_LE(align, static_cast<uint32_t>(kArmAlignment));
    break;

  case kX86:
    CHECK_LE(align, static_cast<uint32_t>(kX86Alignment));
    break;

  case kMips:
    CHECK_LE(align, static_cast<uint32_t>(kMipsAlignment));
    break;

  default:
    LOG(FATAL) << "Unknown instruction set: " << insn_set;
  }
}


} // namespace compiler_llvm
} // namespace art
