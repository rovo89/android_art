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
#include <llvm/Transforms/Scalar.h>

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


// TODO: We may need something to manage these passes.
// TODO: We need high-level IR to analysis and do this at the IRBuilder level.
class AddSuspendCheckToLoopLatchPass : public llvm::LoopPass {
 public:
  static char ID;

  AddSuspendCheckToLoopLatchPass() : llvm::LoopPass(ID), irb_(NULL) {
    LOG(FATAL) << "Unexpected instantiation of AddSuspendCheckToLoopLatchPass";
    // NOTE: We have to declare this constructor for llvm::RegisterPass, but
    // this constructor won't work because we have no information on
    // IRBuilder.  Thus, we should place a LOG(FATAL) here.
  }

  AddSuspendCheckToLoopLatchPass(art::compiler_llvm::IRBuilder* irb)
    : llvm::LoopPass(ID), irb_(irb) {
  }

  virtual void getAnalysisUsage(llvm::AnalysisUsage &AU) const {
    AU.addRequiredID(llvm::LoopSimplifyID);

    AU.addPreserved<llvm::DominatorTree>();
    AU.addPreserved<llvm::LoopInfo>();
    AU.addPreservedID(llvm::LoopSimplifyID);
    AU.addPreserved<llvm::ScalarEvolution>();
    AU.addPreservedID(llvm::BreakCriticalEdgesID);
  }

  virtual bool runOnLoop(llvm::Loop *loop, llvm::LPPassManager &lpm) {
    CHECK_EQ(loop->getNumBackEdges(), 1U) << "Loop must be simplified!";
    llvm::BasicBlock* bb = loop->getLoopLatch();
    CHECK_NE(bb, static_cast<void*>(NULL)) << "A single loop latch must exist.";

    irb_->SetInsertPoint(bb->getTerminator());

    using namespace art::compiler_llvm::runtime_support;
    llvm::Value* runtime_func = irb_->GetRuntime(TestSuspend);
    llvm::Value* thread_object_addr = irb_->CreateCall(irb_->GetRuntime(GetCurrentThread));
    irb_->CreateCall(runtime_func, thread_object_addr);

    return true;
  }

 private:
  art::compiler_llvm::IRBuilder* irb_;
};

char AddSuspendCheckToLoopLatchPass::ID = 0;

llvm::RegisterPass<AddSuspendCheckToLoopLatchPass> reg_add_suspend_check_to_loop_latch_pass_(
  "add-suspend-check-to-loop-latch", "Add suspend check to loop latch pass", false, false);


} // end anonymous namespace

namespace art {
namespace compiler_llvm {

llvm::Module* makeLLVMModuleContents(llvm::Module* module);


CompilationUnit::CompilationUnit(InstructionSet insn_set, size_t elf_idx)
: cunit_lock_("compilation_unit_lock"), insn_set_(insn_set), elf_idx_(elf_idx),
  context_(new llvm::LLVMContext()), mem_usage_(0), num_elf_funcs_(0) {

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


bool CompilationUnit::Materialize(size_t thread_count) {
  MutexLock GUARD(cunit_lock_);

  if (thread_count == 1) {
    llvm::raw_string_ostream str_os(elf_image_);
    bool success = MaterializeToFile(str_os);
    LOG(INFO) << "Compilation Unit: " << elf_idx_ << (success ? " (done)" : " (failed)");

    // Free the resources
    context_.reset(NULL);
    irb_.reset(NULL);
    module_ = NULL;

    return success;
  }

  // Prepare the pipe between parent process and child process
  int pipe_fd[2];
  if (pipe(pipe_fd) == -1) {
    PLOG(FATAL) << "Failed to create pipe for CompilerWorker";
    return false;
  }

  // Fork a process to do the compilation
  pid_t pid = fork();
  if (pid < 0) {
    close(pipe_fd[0]);
    close(pipe_fd[1]);
    PLOG(FATAL) << "Failed to fork a process to do the compilation";
    return false;

  } else if (pid == 0) { // Child process
    // Close the unused pipe read end
    close(pipe_fd[0]);

    // Change process groups, so we don't get ripped by ProcessManager
    setpgid(0, 0);

    llvm::raw_fd_ostream fd_os(pipe_fd[1], /* shouldClose */true);

    // TODO: Should use exec* family instead of invoking a function.
    // Forward our compilation request to bcc.
    exit(static_cast<int>(!MaterializeToFile(fd_os)));

  } else { // Parent process
    // Close the unused pipe write end
    close(pipe_fd[1]);

    // Free the resources
    context_.reset(NULL);
    irb_.reset(NULL);
    module_ = NULL;

    // Read the result out from the pipe read end (until failure)
    const size_t buf_size = 1024;
    std::vector<uint8_t> buf(buf_size);
    while (true) {
      // Read from the pipe
      ssize_t nread = read(pipe_fd[0], &*buf.begin(), buf_size);
      if (nread < 0) {
        if (errno == EAGAIN || errno == EINTR) {
          continue;
        } else {
          LOG(ERROR) << "Unexpected error during IPC: " << strerror(errno);
        }
      }

      // Append to the end of the elf_image_
      elf_image_.append(buf.begin(), buf.begin() + nread);

      if (nread < static_cast<ssize_t>(buf_size)) { // EOF reached!
        break;
      }
    }

    close(pipe_fd[0]);

    // Wait for child to finish
    int status;
    pid_t got_pid = TEMP_FAILURE_RETRY(waitpid(pid, &status, 0));
    if (got_pid != pid) {
      PLOG(ERROR) << "waitpid failed: wanted " << pid << ", got " << got_pid;
      elf_image_.clear();
      return false;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      LOG(ERROR) << "Failed to compile the bitcode: " << WEXITSTATUS(status);
      elf_image_.clear();
      return false;
    }

    LOG(INFO) << "Compilation Unit: " << elf_idx_ << " (done)";
    return true;
  }
}


void CompilationUnit::RegisterCompiledMethod(const llvm::Function* func,
                                             CompiledMethod* compiled_method) {
  MutexLock GUARD(cunit_lock_);
  compiled_methods_map_.Put(func, compiled_method);
}


void CompilationUnit::UpdateFrameSizeInBytes(const llvm::Function* func,
                                             size_t frame_size_in_bytes) {
  MutexLock GUARD(cunit_lock_);
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

bool CompilationUnit::MaterializeToFile(llvm::raw_ostream& out_stream) {
  // Lookup the LLVM target
  char const* target_triple = NULL;
  char const* target_attr = NULL;

  switch (insn_set_) {
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
    LOG(FATAL) << "Unknown instruction set: " << insn_set_;
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
  llvm::OwningPtr<llvm::TargetMachine> target_machine(
    target->createTargetMachine(target_triple, "", target_attr, target_options,
                                llvm::Reloc::Static, llvm::CodeModel::Small,
                                llvm::CodeGenOpt::Less));

  CHECK(target_machine.get() != NULL) << "Failed to create target machine";

  // Add target data
  llvm::TargetData const* target_data = target_machine->getTargetData();

  // PassManager for code generation passes
  llvm::PassManager pm;
  pm.add(new llvm::TargetData(*target_data));

  // FunctionPassManager for optimization pass
  llvm::FunctionPassManager fpm(module_);
  fpm.add(new llvm::TargetData(*target_data));

  if (bitcode_filename_.empty()) {
    // If we don't need write the bitcode to file, add the AddSuspendCheckToLoopLatchPass to the
    // regular FunctionPass.
    fpm.add(new ::AddSuspendCheckToLoopLatchPass(irb_.get()));
  } else {
    // Run AddSuspendCheckToLoopLatchPass before we write the bitcode to file.
    llvm::FunctionPassManager fpm2(module_);
    fpm2.add(new ::AddSuspendCheckToLoopLatchPass(irb_.get()));
    fpm2.doInitialization();
    for (llvm::Module::iterator F = module_->begin(), E = module_->end();
         F != E; ++F) {
      fpm2.run(*F);
    }
    fpm2.doFinalization();


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
  //pm_builder.Inliner = llvm::createFunctionInliningPass();
  pm_builder.Inliner = llvm::createAlwaysInlinerPass();
  //pm_builder.Inliner = llvm::createPartialInliningPass();
  pm_builder.OptLevel = 3;
  pm_builder.DisableSimplifyLibCalls = 1;
  pm_builder.DisableUnitAtATime = 1;
  pm_builder.populateModulePassManager(pm);
  pm_builder.populateFunctionPassManager(fpm);

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

    // FIXME: Unable to run the UpdateFrameSizePass pass since it tries to
    //        update the value reside in the different address space.
    // Add pass to update the frame_size_in_bytes_
    //pm.add(new ::UpdateFrameSizePass(this));

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

} // namespace compiler_llvm
} // namespace art
