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

#include "compiled_method.h"
#include "file.h"
#include "instruction_set.h"
#include "ir_builder.h"
#include "logging.h"
#include "os.h"

#include "runtime_support_builder_arm.h"
#include "runtime_support_builder_x86.h"

#include <llvm/ADT/OwningPtr.h>
#include <llvm/ADT/StringSet.h>
#include <llvm/ADT/Triple.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/Analysis/DebugInfo.h>
#include <llvm/Analysis/LoopPass.h>
#include <llvm/Analysis/RegionPass.h>
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
#include <llvm/PassManager.h>
#include <llvm/Support/Debug.h>
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

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>

namespace {

class UpdateFrameSizePass : public llvm::MachineFunctionPass {
 public:
  static char ID;

  UpdateFrameSizePass() : llvm::MachineFunctionPass(ID), cunit_(NULL) {
    LOG(FATAL) << "Unexpected instantiation of UpdateFrameSizePass";
    // NOTE: We have to declare this constructor for llvm::RegisterPass, but
    // this constructor won't work because we have no information on
    // CompilationUnit.  Thus, we should place a LOG(FATAL) here.
  }

  UpdateFrameSizePass(art::compiler_llvm::CompilationUnit* cunit)
    : llvm::MachineFunctionPass(ID), cunit_(cunit) {
  }

  virtual bool runOnMachineFunction(llvm::MachineFunction &MF) {
    cunit_->UpdateFrameSizeInBytes(MF.getFunction(),
                                   MF.getFrameInfo()->getStackSize());
    return false;
  }

 private:
  art::compiler_llvm::CompilationUnit* cunit_;
};

char UpdateFrameSizePass::ID = 0;

llvm::RegisterPass<UpdateFrameSizePass> reg_update_frame_size_pass_(
  "update-frame-size", "Update frame size pass", false, false);

} // end anonymous namespace

namespace art {
namespace compiler_llvm {

llvm::Module* makeLLVMModuleContents(llvm::Module* module);


CompilationUnit::CompilationUnit(InstructionSet insn_set, size_t elf_idx)
: insn_set_(insn_set), elf_idx_(elf_idx), context_(new llvm::LLVMContext()),
  mem_usage_(0), num_elf_funcs_(0) {

  // Create the module and include the runtime function declaration
  module_ = new llvm::Module("art", *context_);
  makeLLVMModuleContents(module_);

  // Create IRBuilder
  irb_.reset(new IRBuilder(*context_, *module_));

  // We always need a switch case, so just use a normal function.
  switch(insn_set_) {
    default:
      runtime_support_.reset(new RuntimeSupportBuilder(*context_, *module_, *irb_));
      break;
  case kArm:
  case kThumb2:
    runtime_support_.reset(new RuntimeSupportBuilderARM(*context_, *module_, *irb_));
    break;
  case kX86:
    runtime_support_.reset(new RuntimeSupportBuilderX86(*context_, *module_, *irb_));
    break;
  }

  runtime_support_->OptimizeRuntimeSupport();

  irb_->SetRuntimeSupport(runtime_support_.get());
}


CompilationUnit::~CompilationUnit() {
}


bool CompilationUnit::WriteBitcodeToFile(const std::string& bitcode_filename) {
  std::string errmsg;

  llvm::OwningPtr<llvm::tool_output_file> out_file(
    new llvm::tool_output_file(bitcode_filename.c_str(), errmsg,
                               llvm::raw_fd_ostream::F_Binary));


  if (!errmsg.empty()) {
    LOG(ERROR) << "Failed to create bitcode output file: " << errmsg;
    return false;
  }

  llvm::WriteBitcodeToFile(module_, out_file->os());
  out_file->keep();

  return true;
}

// TODO: Move to scoped_temp_file.h
class ScopedTempFile {
 public:
  ScopedTempFile(const std::string& filename_template)
      : filename_(filename_template), file_(NULL) {
    int fd = mkstemp(&filename_[0]);
    CHECK_NE(-1, fd);
    file_ = OS::FileFromFd(filename_.c_str(), fd);
  }

  ~ScopedTempFile() {
    delete file_;
    TEMP_FAILURE_RETRY(unlink(filename_.c_str()));
  }

  int GetFd() {
    return file_->Fd();
  }

  const std::string &GetName() const {
    return filename_;
  }

  bool ReadToString(std::string &buffer) {
    off_t file_size = file_->Length();
    if (file_size <= 0) {
      buffer.clear();
      return true;
    }

    buffer.reserve(file_size);
    buffer.resize(file_size);
    return file_->ReadFully(&buffer[0], file_size);
  }

 private:
  std::string filename_;
  File *file_;
};

bool CompilationUnit::Materialize() {
  const std::string tmp_file = "/tmp/art-llvm-XXXXXX";

  // Prepare the input
  ScopedTempFile input(tmp_file);
  if (input.GetFd() < 0) {
    PLOG(ERROR) << "Failed to save the module to the file " << tmp_file;
    return false;
  }

  // Write the bitcode to the file
  if (!WriteBitcodeToFile(input.GetName())) {
    return false;
  }

  // Prepare the output
  ScopedTempFile output(tmp_file);
  if (output.GetFd() < 0) {
    PLOG(ERROR) << "Failed to prepare the output file " << tmp_file;
    return false;
  }

  // Fork a process to do the compilation
  pid_t pid = fork();
  if (pid == 0) {
    // change process groups, so we don't get ripped by ProcessManager
    setpgid(0, 0);

    // TODO: Should use exec* family instead of invoking a function.
    // Forward our compilation request to bcc.
    exit(static_cast<int>(!MaterializeFile(input.GetFd(), output.GetFd(),
                                           insn_set_)));
  } else {
    if (pid < 0) {
      LOG(FATAL) << "Failed to fork a process to do the compilation: "
                 << strerror(errno);
    }

    // Free the resources
    context_.reset(NULL);
    irb_.reset(NULL);
    module_ = NULL;

    int status;

    // Wait for child to finish
    pid_t got_pid = TEMP_FAILURE_RETRY(waitpid(pid, &status, 0));
    if (got_pid != pid) {
      PLOG(ERROR) << "waitpid failed: wanted " << pid << ", got " << got_pid;
      return false;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      LOG(ERROR) << "Failed to compile the bitcode: " << WEXITSTATUS(status);
      return false;
    }
  }

  // Read the result out from the output file
  TEMP_FAILURE_RETRY(lseek(output.GetFd(), 0, SEEK_SET));
  if (!output.ReadToString(elf_image_)) {
    LOG(ERROR) << "Failed to read the result file";
    return false;
  }

  LOG(INFO) << "Compilation Unit: " << elf_idx_ << " (done)";

  return true;
}


void CompilationUnit::RegisterCompiledMethod(const llvm::Function* func,
                                             CompiledMethod* compiled_method) {
  compiled_methods_map_.Put(func, compiled_method);
}


void CompilationUnit::UpdateFrameSizeInBytes(const llvm::Function* func,
                                             size_t frame_size_in_bytes) {
  SafeMap<const llvm::Function*, CompiledMethod*>::iterator iter =
    compiled_methods_map_.find(func);

  if (iter != compiled_methods_map_.end()) {
    CompiledMethod* compiled_method = iter->second;
    compiled_method->SetFrameSizeInBytes(frame_size_in_bytes);

    if (frame_size_in_bytes > 1728u) {
      LOG(WARNING) << "Huge frame size: " << frame_size_in_bytes
                   << " elf_idx=" << compiled_method->GetElfIndex()
                   << " elf_func_idx=" << compiled_method->GetElfFuncIndex();
    }
  }
}

bool CompilationUnit::MaterializeFile(int input_fd, int output_fd,
                                      InstructionSet insn_set) {
  // Initialize the LLVM first
  llvm::InitializeAllTargets();
  llvm::InitializeAllTargetMCs();
  llvm::InitializeAllAsmPrinters();
  llvm::InitializeAllAsmParsers();

  // Read the LLVM module from input_fd
  llvm::OwningPtr<llvm::MemoryBuffer> memory_buffer;
  llvm::error_code load_input_err =
      llvm::MemoryBuffer::getOpenFile(input_fd, "<art-llvm-module>",
                                      memory_buffer);

  if (load_input_err) {
    LOG(ERROR) << "Failed to load input for compiler into memory: "
               << load_input_err.message();
    return false;
  }

  // It's safe to use the global context now
  std::string load_module_errmsg;
  llvm::Module *module = ParseBitcodeFile(memory_buffer.get(),
                                          llvm::getGlobalContext(),
                                          &load_module_errmsg);
  if (module == NULL) {
    LOG(ERROR) << "Failed to load LLVM module to compiler: "
               << load_module_errmsg;
    return false;
  }

  // Lookup the LLVM target
  char const* target_triple = NULL;
  char const* target_attr = NULL;

  switch (insn_set) {
  case kThumb2:
    target_triple = "thumb-none-linux-gnueabi";
    target_attr = "+thumb2,+neon,+neonfp,+vfp3";
    break;

  case kArm:
    target_triple = "armv7-none-linux-gnueabi";
    // TODO: Fix for Xoom.
    target_attr = "+v7,+neon,+neonfp,+vfp3";
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
  llvm::Target const* target =
    llvm::TargetRegistry::lookupTarget(target_triple, errmsg);

  CHECK(target != NULL) << errmsg;

  // Target options
  llvm::TargetOptions target_options;
  target_options.FloatABIType = llvm::FloatABI::Soft;
  target_options.NoFramePointerElim = true;
  target_options.NoFramePointerElimNonLeaf = true;
  target_options.UseSoftFloat = false;
  target_options.EnableFastISel = true;

  // Create the llvm::TargetMachine
  llvm::TargetMachine* target_machine =
    target->createTargetMachine(target_triple, "", target_attr, target_options,
                                llvm::Reloc::Static, llvm::CodeModel::Small,
                                llvm::CodeGenOpt::Less);

  CHECK(target_machine != NULL) << "Failed to create target machine";

  // Add target data
  llvm::TargetData const* target_data = target_machine->getTargetData();

  // PassManager for code generation passes
  llvm::PassManager pm;
  pm.add(new llvm::TargetData(*target_data));

  // FunctionPassManager for optimization pass
  llvm::FunctionPassManager fpm(module);
  fpm.add(new llvm::TargetData(*target_data));

  // Add optimization pass
  llvm::PassManagerBuilder pm_builder;
  pm_builder.Inliner = llvm::createAlwaysInlinerPass();
  pm_builder.OptLevel = 1;
  pm_builder.DisableSimplifyLibCalls = 1;
  pm_builder.populateModulePassManager(pm);
  pm_builder.populateFunctionPassManager(fpm);

  // Add passes to emit ELF image
  {
    llvm::formatted_raw_ostream formatted_os(
      *(new llvm::raw_fd_ostream(output_fd, /* shouldClose */false)), true);

    // Ask the target to add backend passes as necessary.
    if (target_machine->addPassesToEmitFile(pm,
                                            formatted_os,
                                            llvm::TargetMachine::CGFT_ObjectFile,
                                            true)) {
      LOG(FATAL) << "Unable to generate ELF for this target";
      return false;
    }

    // FIXME: Unable to run the UpdateFrameSizePass pass since it tries to
    //        update the value reside in the different address space.
    // Add pass to update the frame_size_in_bytes_
    //pm.add(new ::UpdateFrameSizePass(this));

    // Run the per-function optimization
    fpm.doInitialization();
    for (llvm::Module::iterator F = module->begin(), E = module->end();
         F != E; ++F) {
      fpm.run(*F);
    }
    fpm.doFinalization();

    // Run the code generation passes
    pm.run(*module);
  }

  return true;
}

} // namespace compiler_llvm
} // namespace art
