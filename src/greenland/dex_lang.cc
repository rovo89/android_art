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

#include "dex_lang.h"

#include "intrinsic_helper.h"

#include "atomic.h"
#include "inferred_reg_category_map.h"
#include "object.h" // FIXME: include this in oat_compilation_unit.h
#include "oat_compilation_unit.h"
#include "stl_util.h"
#include "stringprintf.h"
#include "verifier/method_verifier.h"

#include <llvm/Analysis/Passes.h>
#include <llvm/Analysis/Verifier.h>
#include <llvm/BasicBlock.h>
#include <llvm/Function.h>
#include <llvm/Module.h>
#include <llvm/PassManager.h>
#include <llvm/Support/InstIterator.h>
#include <llvm/Transforms/Scalar.h>

namespace art {
namespace greenland {

//----------------------------------------------------------------------------
// DexLang::Context
//----------------------------------------------------------------------------
DexLang::Context::Context()
    : context_(), module_(NULL), ref_count_(1), mem_usage_(0) {
  module_ = new llvm::Module("art", context_);

  // Initialize the contents of an empty module
  // Type of "JavaObject"
  llvm::StructType::create(context_, "JavaObject");
  // Type of "Method"
  llvm::StructType::create(context_, "Method");
  // Type of "Thread"
  llvm::StructType::create(context_, "Thread");

  // Initalize the DexLang intrinsics
  intrinsic_helper_ = new IntrinsicHelper(context_, *module_);

  return;
}

DexLang::Context::~Context() {
  delete intrinsic_helper_;
  return;
}

DexLang::Context& DexLang::Context::IncRef() {
  android_atomic_inc(&ref_count_);
  return *this;
}

void DexLang::Context::DecRef() {
  int32_t old_ref_count = android_atomic_dec(&ref_count_);
  if (old_ref_count <= 1) {
    delete this;
  }
  return;
}

void DexLang::Context::AddMemUsageApproximation(size_t usage) {
  android_atomic_add(static_cast<int32_t>(usage), &mem_usage_);
  return;
}

//----------------------------------------------------------------------------
// Constructor, Destructor and APIs
//----------------------------------------------------------------------------
DexLang::DexLang(DexLang::Context& context, Compiler& compiler,
                 OatCompilationUnit& cunit)
    : dex_lang_ctx_(context.IncRef()), compiler_(compiler), cunit_(cunit),
      dex_file_(cunit.GetDexFile()), code_item_(cunit.GetCodeItem()),
      dex_cache_(cunit.GetDexCache()),
      context_(context.GetLLVMContext()), module_(context.GetOutputModule()),
      intrinsic_helper_(context.GetIntrinsicHelper()),
      irb_(context.GetLLVMContext(), context.GetOutputModule(),
           context.GetIntrinsicHelper()),
      func_(NULL), reg_alloc_bb_(NULL), arg_reg_init_bb_(NULL),
      basic_blocks_(cunit.GetCodeItem()->insns_size_in_code_units_),
      retval_(NULL), retval_jty_(kVoid),
      landing_pads_bb_(cunit.GetCodeItem()->tries_size_, NULL),
      exception_unwind_bb_(NULL), cur_try_item_offset(-1),
      require_shadow_frame(false), num_shadow_frame_entries_(0) {
  if (cunit.GetCodeItem()->tries_size_ > 0) {
    cur_try_item_offset = 0;
  }
  return;
}

DexLang::~DexLang() {
  dex_lang_ctx_.DecRef();
  return;
}

llvm::Function* DexLang::Build() {
  if (!CreateFunction() ||
      !EmitPrologue() ||
      !EmitInstructions() ||
      !EmitPrologueAllcaShadowFrame() ||
      !EmitPrologueLinkBasicBlocks() ||
      !PrettyLayoutExceptionBasicBlocks() ||
      !VerifyFunction() ||
      !OptimizeFunction() ||
      !RemoveRedundantPendingExceptionChecks()) {
    return NULL;
  }

  // NOTE: From statistic, the bitcode size is 4.5 times bigger than the
  // Dex file.  Besides, we have to convert the code unit into bytes.
  // Thus, we got our magic number 9.
  dex_lang_ctx_.AddMemUsageApproximation(
      code_item_->insns_size_in_code_units_ * 900);

  return func_;
}

llvm::Value* DexLang::AllocateDalvikReg(JType jty, unsigned reg_idx) {
  RegCategory cat = GetRegCategoryFromJType(jty);
  llvm::Type* type = irb_.GetJType(jty, kAccurate);

  DCHECK_NE(type, static_cast<llvm::Type*>(NULL));

  std::string reg_name;
  switch (cat) {
    case kRegCat1nr: {
      reg_name = StringPrintf("r%u", reg_idx);
      break;
    }
    case kRegCat2: {
      reg_name = StringPrintf("w%u", reg_idx);
      break;
    }
    case kRegObject: {
      reg_name = StringPrintf("p%u", reg_idx);
      break;
    }
    default: {
      LOG(FATAL) << "Unknown register category for allocation: " << cat;
    }
  }

  // Save current IR builder insert point
  DCHECK(reg_alloc_bb_ != NULL);
  llvm::IRBuilderBase::InsertPoint irb_ip_original = irb_.saveIP();
  irb_.SetInsertPoint(reg_alloc_bb_);

  // Alloca
  llvm::Value* reg_addr = irb_.CreateAlloca(type, 0, reg_name);

  // Restore IRBuilder insert point
  irb_.restoreIP(irb_ip_original);

  DCHECK_NE(reg_addr, static_cast<llvm::Value*>(NULL));

  return reg_addr;
}

//----------------------------------------------------------------------------
// Basic Block Helper Functions
//----------------------------------------------------------------------------
llvm::BasicBlock* DexLang::GetBasicBlock(unsigned dex_pc) {
  DCHECK(dex_pc < code_item_->insns_size_in_code_units_);

  llvm::BasicBlock* basic_block = basic_blocks_[dex_pc];

  if (!basic_block) {
    basic_block = CreateBasicBlockWithDexPC(dex_pc);
    basic_blocks_[dex_pc] = basic_block;
  }

  return basic_block;
}

llvm::BasicBlock* DexLang::CreateBasicBlockWithDexPC(unsigned dex_pc,
                                                     const char* postfix) {
  std::string name;

  if (postfix) {
    StringAppendF(&name, "B%04x.%s", dex_pc, postfix);
  } else {
    StringAppendF(&name, "B%04x", dex_pc);
  }

  return llvm::BasicBlock::Create(context_, name, func_);
}

llvm::BasicBlock* DexLang::GetNextBasicBlock(unsigned dex_pc) {
  const Instruction* insn = Instruction::At(code_item_->insns_ + dex_pc);
  return GetBasicBlock(dex_pc + insn->SizeInCodeUnits());
}

//----------------------------------------------------------------------------
// Exception Handling
//----------------------------------------------------------------------------
int32_t DexLang::GetTryItemOffset(unsigned dex_pc) {
  if (cur_try_item_offset >= 0) {
    // Search over the try item.
    do {
      const DexFile::TryItem* ti =
          DexFile::GetTryItems(*code_item_, cur_try_item_offset);
      if (dex_pc < ti->start_addr_) {
        return -1;
      }

      if (dex_pc < (ti->start_addr_ + ti->insn_count_)) {
        return cur_try_item_offset;
      }

      cur_try_item_offset++;
    } while (cur_try_item_offset < code_item_->tries_size_);

    // Search to the end of try items and Cannot find any try item corresponding
    // to the dex_pc.
    cur_try_item_offset = -1;
  }

  return cur_try_item_offset;
}

llvm::BasicBlock* DexLang::GetLandingPadBasicBlock(unsigned dex_pc) {
  // Find the try item for this address in this method
  int32_t ti_offset = GetTryItemOffset(dex_pc);

  if (ti_offset == -1) {
    return NULL; // No landing pad is available for this address.
  }

  // Check for the existing landing pad basic block
  DCHECK_GT(landing_pads_bb_.size(), static_cast<size_t>(ti_offset));
  llvm::BasicBlock* block_lpad = landing_pads_bb_[ti_offset];

  if (block_lpad != NULL) {
    // We have generated landing pad for this try item already.  Return the
    // same basic block.
    return block_lpad;
  }

  // Get try item from code item
  const DexFile::TryItem* ti = DexFile::GetTryItems(*code_item_, ti_offset);

  std::string lpadname;

#ifndef NDEBUG
  StringAppendF(&lpadname, "lpad%d_%04x_to_%04x",
                ti_offset, ti->start_addr_, ti->handler_off_);
#endif

  // Create landing pad basic block
  block_lpad = llvm::BasicBlock::Create(context_, lpadname, func_);

  // Change IRBuilder insert point
  llvm::IRBuilderBase::InsertPoint irb_ip_original = irb_.saveIP();
  irb_.SetInsertPoint(block_lpad);

  // Find catch block with matching type
  llvm::Value* method_object_addr = EmitLoadMethodObjectAddr();

  // Find catch block with matching type
  llvm::Value* ti_offset_value = irb_.getInt32(ti_offset);

  llvm::Value* catch_handler_index_value =
      EmitInvokeIntrinsic2(dex_pc, IntrinsicHelper::FindCatchBlock,
                           method_object_addr, ti_offset_value);

  // Switch instruction (Go to unwind basic block by default)
  llvm::SwitchInst* sw =
      irb_.CreateSwitch(catch_handler_index_value, GetUnwindBasicBlock());

  // Cases with matched catch block
  CatchHandlerIterator iter(*code_item_, ti->start_addr_);

  for (uint32_t c = 0; iter.HasNext(); iter.Next(), ++c) {
    sw->addCase(irb_.getInt32(c), GetBasicBlock(iter.GetHandlerAddress()));
  }

  // Restore the orignal insert point for IRBuilder
  irb_.restoreIP(irb_ip_original);

  // Cache this landing pad
  landing_pads_bb_[ti_offset] = block_lpad;

  return block_lpad;
}

llvm::BasicBlock* DexLang::GetUnwindBasicBlock() {
  // Check the existing unwinding baisc block block
  if (exception_unwind_bb_ != NULL) {
    return exception_unwind_bb_;
  }

  // Create new basic block for unwinding
  exception_unwind_bb_ =
      llvm::BasicBlock::Create(context_, "exception_unwind", func_);

  // Change IRBuilder insert point
  llvm::IRBuilderBase::InsertPoint irb_ip_original = irb_.saveIP();
  irb_.SetInsertPoint(exception_unwind_bb_);

  // Pop the shadow frame
  EmitPopShadowFrame();

  // Emit the code to return default value (zero) for the given return type.
  char ret_shorty = cunit_.GetShorty()[0];
  if (ret_shorty == 'V') {
    irb_.CreateRetVoid();
  } else {
    irb_.CreateRet(irb_.GetJZero(ret_shorty));
  }

  // Restore the orignal insert point for IRBuilder
  irb_.restoreIP(irb_ip_original);

  return exception_unwind_bb_;
}

void DexLang::EmitBranchExceptionLandingPad(unsigned dex_pc) {
  if (llvm::BasicBlock* lpad = GetLandingPadBasicBlock(dex_pc)) {
    irb_.CreateBr(lpad);
  } else {
    irb_.CreateBr(GetUnwindBasicBlock());
  }
}

void DexLang::EmitGuard_DivZeroException(unsigned dex_pc,
                                         llvm::Value* denominator,
                                         JType op_jty) {
  DCHECK(op_jty == kInt || op_jty == kLong) << op_jty;

  llvm::Constant* zero = irb_.GetJZero(op_jty);

  llvm::Value* equal_zero = irb_.CreateICmpEQ(denominator, zero);

  llvm::BasicBlock* block_exception = CreateBasicBlockWithDexPC(dex_pc, "div0");

  llvm::BasicBlock* block_continue = CreateBasicBlockWithDexPC(dex_pc, "cont");

  irb_.CreateCondBr(equal_zero, block_exception, block_continue);

  irb_.SetInsertPoint(block_exception);
  EmitUpdateDexPC(dex_pc);
  EmitInvokeIntrinsic(dex_pc, IntrinsicHelper::ThrowDivZeroException);
  EmitBranchExceptionLandingPad(dex_pc);

  irb_.SetInsertPoint(block_continue);
}

void DexLang::EmitGuard_NullPointerException(unsigned dex_pc,
                                             llvm::Value* object) {
  llvm::Value* equal_null = irb_.CreateICmpEQ(object, irb_.GetJNull());

  llvm::BasicBlock* block_exception =
    CreateBasicBlockWithDexPC(dex_pc, "nullp");

  llvm::BasicBlock* block_continue =
    CreateBasicBlockWithDexPC(dex_pc, "cont");

  irb_.CreateCondBr(equal_null, block_exception, block_continue);

  irb_.SetInsertPoint(block_exception);

  EmitUpdateDexPC(dex_pc);
  EmitInvokeIntrinsic(dex_pc, IntrinsicHelper::ThrowNullPointerException,
                      irb_.getInt32(dex_pc));
  EmitBranchExceptionLandingPad(dex_pc);

  irb_.SetInsertPoint(block_continue);
}

void
DexLang::EmitGuard_ArrayIndexOutOfBoundsException(unsigned dex_pc,
                                                  llvm::Value* array,
                                                  llvm::Value* index) {
  llvm::Value* array_len = EmitLoadArrayLength(array);

  llvm::Value* cmp = irb_.CreateICmpUGE(index, array_len);

  llvm::BasicBlock* block_exception =
    CreateBasicBlockWithDexPC(dex_pc, "overflow");

  llvm::BasicBlock* block_continue =
    CreateBasicBlockWithDexPC(dex_pc, "cont");

  irb_.CreateCondBr(cmp, block_exception, block_continue);

  irb_.SetInsertPoint(block_exception);

  EmitUpdateDexPC(dex_pc);
  EmitInvokeIntrinsic2(dex_pc, IntrinsicHelper::ThrowIndexOutOfBounds,
                       index, array_len);
  EmitBranchExceptionLandingPad(dex_pc);

  irb_.SetInsertPoint(block_continue);
  return;
}

void DexLang::EmitGuard_ArrayException(unsigned dex_pc,
                                       llvm::Value* array, llvm::Value* index) {
  EmitGuard_NullPointerException(dex_pc, array);
  EmitGuard_ArrayIndexOutOfBoundsException(dex_pc, array, index);
}

void DexLang::EmitGuard_ExceptionLandingPad(unsigned dex_pc) {
  llvm::Value* exception_pending =
      EmitInvokeIntrinsic(dex_pc, IntrinsicHelper::IsExceptionPending);

  llvm::BasicBlock* block_cont = CreateBasicBlockWithDexPC(dex_pc, "cont");

  if (llvm::BasicBlock* lpad = GetLandingPadBasicBlock(dex_pc)) {
    irb_.CreateCondBr(exception_pending, lpad, block_cont);
  } else {
    irb_.CreateCondBr(exception_pending, GetUnwindBasicBlock(), block_cont);
  }

  irb_.SetInsertPoint(block_cont);
}

//----------------------------------------------------------------------------
// Garbage Collection Safe Point
//----------------------------------------------------------------------------
void DexLang::EmitGuard_GarbageCollectionSuspend() {
  llvm::Value* thread_object_addr = EmitGetCurrentThread();
  EmitInvokeIntrinsicNoThrow(IntrinsicHelper::TestSuspend, thread_object_addr);
  return;
}

//----------------------------------------------------------------------------
// Shadow Frame
//----------------------------------------------------------------------------
void DexLang::EmitUpdateDexPC(unsigned dex_pc) {
  require_shadow_frame = true;
  EmitInvokeIntrinsicNoThrow(IntrinsicHelper::UpdateDexPC,
                             irb_.getInt32(dex_pc));
  return;
}

void DexLang::EmitPopShadowFrame() {
  EmitInvokeIntrinsicNoThrow(IntrinsicHelper::PopShadowFrame);
  return;
}

unsigned DexLang::AllocShadowFrameEntry(unsigned reg_idx) {
  return num_shadow_frame_entries_++;
}

//----------------------------------------------------------------------------
// Code Generation
//----------------------------------------------------------------------------
bool DexLang::CreateFunction() {
  std::string func_name(PrettyMethod(cunit_.GetDexMethodIndex(), *dex_file_,
                                     /* with_signature */false));
  llvm::FunctionType* func_type = GetFunctionType();

  if (func_type == NULL) {
    return false;
  }

  func_ = llvm::Function::Create(func_type, llvm::Function::ExternalLinkage,
                                 func_name, &module_);

  llvm::Function::arg_iterator arg_iter(func_->arg_begin());
  llvm::Function::arg_iterator arg_end(func_->arg_end());

  arg_iter->setName("method");
  ++arg_iter;

  if (!cunit_.IsStatic()) {
    DCHECK_NE(arg_iter, arg_end);
    arg_iter->setName("this");
    ++arg_iter;
  }

  for (unsigned i = 0; arg_iter != arg_end; ++i, ++arg_iter) {
    arg_iter->setName(StringPrintf("a%u", i));
  }

  return true;
}

llvm::FunctionType* DexLang::GetFunctionType() {
  uint32_t shorty_size;
  const char* shorty = cunit_.GetShorty(&shorty_size);
  CHECK_GE(shorty_size, 1u);

  // Get return type
  llvm::Type* ret_type = irb_.GetJType(shorty[0], kAccurate);

  // Get argument type
  std::vector<llvm::Type*> args_type;

  // method object
  args_type.push_back(irb_.GetJMethodTy());

  if (!cunit_.IsStatic()) {
    // The first argument to non-static method is "this" object pointer
    args_type.push_back(irb_.GetJObjectTy());
  }

  for (uint32_t i = 1; i < shorty_size; ++i) {
    args_type.push_back(irb_.GetJType(shorty[i], kAccurate));
  }

  return llvm::FunctionType::get(ret_type, args_type, false);
}

bool DexLang::PrepareDalvikRegs() {
  const unsigned num_regs = code_item_->registers_size_;
  const unsigned num_ins = code_item_->ins_size_;
  unsigned reg_idx = 0;

  // Registers v[0..(num_regs - num_ins - 1)] are used for local variable
  for (; reg_idx < (num_regs - num_ins); reg_idx++) {
    regs_.push_back(DalvikReg::CreateLocalVarReg(*this, reg_idx));
  }

  // Registers v[(num_regs - num_ins)..(num_regs - 1)] are used for input
  // argument
  uint32_t shorty_size;
  const char* shorty = cunit_.GetShorty(&shorty_size);

  if (!cunit_.IsStatic()) {
    // The first argument to non-static method is "this" object pointer
    regs_.push_back(DalvikReg::CreateArgReg(*this, reg_idx++, kObject));
  }

  for (unsigned i = 1; i < shorty_size; i++) {
    JType jty = GetJTypeFromShorty(shorty[i]);
    regs_.push_back(DalvikReg::CreateArgReg(*this, reg_idx++, jty));
    reg_idx++;

    if (GetRegCategoryFromJType(jty) == kRegCat2) {
      // Need a register pair to hold the value
      regs_.push_back(NULL);
      reg_idx++;
    }
  }

  CHECK_EQ(num_regs, regs_.size());

  return true;
}

bool DexLang::EmitPrologue() {
  reg_alloc_bb_ = llvm::BasicBlock::Create(context_, "prologue.alloca", func_);

  arg_reg_init_bb_ =
      llvm::BasicBlock::Create(context_, "prologue.arginit", func_);

  if (!PrepareDalvikRegs()) {
    return false;
  }

  //Store argument to dalvik register
  irb_.SetInsertPoint(arg_reg_init_bb_);
  if (!EmitPrologueAssignArgRegister()) {
    return false;
  }

  irb_.CreateBr(GetBasicBlock(0));

  return true;
}

bool DexLang::EmitPrologueAssignArgRegister() {
  llvm::Function::arg_iterator arg_iter(func_->arg_begin());

  const unsigned num_regs = code_item_->registers_size_;
  const unsigned num_ins = code_item_->ins_size_;
  unsigned reg_idx = num_regs - num_ins;

  uint32_t shorty_size;
  const char* shorty = cunit_.GetShorty(&shorty_size);

  // skip method object
  ++arg_iter;

  if (!cunit_.IsStatic()) {
    // The first argument to non-static method is "this" object pointer
    EmitStoreDalvikReg(reg_idx, kObject, kAccurate, arg_iter);
    arg_iter++;
    reg_idx++;
  }

  for (unsigned i = 1; i < shorty_size; i++, arg_iter++) {
    JType jty = GetJTypeFromShorty(shorty[i]);
    EmitStoreDalvikReg(reg_idx, jty, kAccurate, arg_iter);
    reg_idx++;

    if (GetRegCategoryFromJType(jty) == kRegCat2) {
      // Wide types
      reg_idx++;
    }
  }

  DCHECK_EQ(arg_iter, func_->arg_end());
  DCHECK_EQ(reg_idx, num_regs);

  return true;
}

bool DexLang::EmitPrologueAllcaShadowFrame() {
  if (!require_shadow_frame) {
    return true;
  }

  // Save current IR builder insert point
  llvm::IRBuilderBase::InsertPoint irb_ip_original = irb_.saveIP();

  irb_.SetInsertPoint(reg_alloc_bb_);
  EmitInvokeIntrinsicNoThrow(IntrinsicHelper::AllocaShadowFrame,
                             irb_.getInt32(num_shadow_frame_entries_));

  // Restore IRBuilder insert point
  irb_.restoreIP(irb_ip_original);

  return true;
}

bool DexLang::EmitPrologueLinkBasicBlocks() {
  irb_.SetInsertPoint(reg_alloc_bb_);
  irb_.CreateBr(arg_reg_init_bb_);
  return true;
}

bool DexLang::PrettyLayoutExceptionBasicBlocks()  {
  llvm::BasicBlock* last_non_exception_bb = &func_->back();
  DCHECK(last_non_exception_bb != NULL);

  DCHECK_NE(last_non_exception_bb, exception_unwind_bb_);
  if (exception_unwind_bb_ != NULL) {
    exception_unwind_bb_->moveAfter(last_non_exception_bb);
  }

  for (std::vector<llvm::BasicBlock*>::reverse_iterator
          landing_pads_bb_iter = landing_pads_bb_.rbegin(),
          landing_pads_bb_end = landing_pads_bb_.rend();
       landing_pads_bb_iter != landing_pads_bb_end; landing_pads_bb_iter++) {
    llvm::BasicBlock* landing_pads_bb = *landing_pads_bb_iter;
    // Move the successors (the cache handlers) first
    llvm::TerminatorInst* inst = landing_pads_bb->getTerminator();
    CHECK(inst != NULL);
    for (unsigned i = 0, e = inst->getNumSuccessors(); i != e; i++) {
      llvm::BasicBlock* catch_handler = inst->getSuccessor(i);
      // One of the catch handler is the unwind basic block which is settled
      // down earlier
      if (catch_handler != exception_unwind_bb_) {
        catch_handler->moveAfter(last_non_exception_bb);
      }
    }
    if (landing_pads_bb != NULL) {
      DCHECK_NE(last_non_exception_bb, landing_pads_bb);
      landing_pads_bb->moveAfter(last_non_exception_bb);
    }
  }

  return true;
}

bool DexLang::VerifyFunction() {
  if (llvm::verifyFunction(*func_, llvm::PrintMessageAction)) {
    LOG(INFO) << "Verification failed on function: "
              << PrettyMethod(cunit_.GetDexMethodIndex(), *dex_file_);
    return false;
  }
  return true;
}

bool DexLang::OptimizeFunction() {
  // Add optimization pass
  llvm::FunctionPassManager fpm(&module_);

  fpm.add(llvm::createTypeBasedAliasAnalysisPass());
  fpm.add(llvm::createBasicAliasAnalysisPass());

  // Perform simple optimizations first to enable the later optimization passes
  // running fast
  {
    fpm.add(llvm::createCFGSimplificationPass());

    // mem2reg
    fpm.add(llvm::createPromoteMemoryToRegisterPass());

    // Remove redundant instructions
    fpm.add(llvm::createInstructionSimplifierPass());

    // Fast CSE
    fpm.add(llvm::createEarlyCSEPass());
    fpm.add(llvm::createCorrelatedValuePropagationPass());

    // 4 + (x + 5)  ->  x + (4 + 5)
    fpm.add(llvm::createReassociatePass());

    // Clean up
    fpm.add(llvm::createCFGSimplificationPass()); // Merge & remove BBs
    fpm.add(llvm::createInstructionCombiningPass());// Clean up after everything
  }

  {
    // SCCP - Sparse conditional constant propagation
    fpm.add(llvm::createSCCPPass());

    // Global value numbering and redundant load elimination
    fpm.add(llvm::createGVNPass());

    // Clean up
    fpm.add(llvm::createCFGSimplificationPass()); // Merge & remove BBs
    fpm.add(llvm::createInstructionCombiningPass());// Clean up after everything
  }

  {
    // Reorders basic blocks to increase the number of fall-through conditional
    // branches
    fpm.add(llvm::createBlockPlacementPass());

    // Clean up
    fpm.add(llvm::createCFGSimplificationPass()); // Merge & remove BBs
  }

  // DexLang doesn't use static branch prediction in the mean time
  //fpm.add(llvm::createLowerExpectIntrinsicPass());
  {
    // Constant propagation
    fpm.add(llvm::createConstantPropagationPass());

    // Clean up
    fpm.add(llvm::createCFGSimplificationPass()); // Merge & remove BBs
    fpm.add(llvm::createInstructionCombiningPass());// Clean up after everything
  }

  {
    // Dead code elimination
    fpm.add(llvm::createDeadCodeEliminationPass());
    fpm.add(llvm::createDeadStoreEliminationPass());
    fpm.add(llvm::createAggressiveDCEPass());

    // Do constant propagation again
    fpm.add(llvm::createConstantPropagationPass());

    // Clean up
    fpm.add(llvm::createCFGSimplificationPass()); // Merge & remove BBs
    fpm.add(llvm::createInstructionCombiningPass());// Clean up after everything
  }

  // Run the per-function optimization
  fpm.doInitialization();
  fpm.run(*func_);
  fpm.doFinalization();

  return true;
}

bool DexLang::RemoveRedundantPendingExceptionChecks() {
#if 0
  const llvm::Function* exception_checking_function =
      irb_.GetIntrinsics(IntrinsicHelper::IsExceptionPending);

  std::vector<llvm::Instruction*> work_list;

  unsigned num_removed = 0;

  for (llvm::inst_iterator i = llvm::inst_begin(func_),
          e = llvm::inst_end(func_); i != e; ++i) {
    if (llvm::CallInst* call_inst = llvm::dyn_cast<llvm::CallInst>(&*i)) {
      if (call_inst->getCalledFunction() != exception_checking_function) {
        continue;
      }
    }
  }

  num_removed = work_list.size();

  for (std::vector<llvm::Instruction*>::iterator inst_iter = work_list.begin(),
          inst_end = work_list.end(); inst_iter != inst_end; inst_iter++) {
    llvm::Instruction* inst = *inst_iter;
    if (!inst->use_empty()) {
      inst->replaceAllUsesWith(irb_.getFalse());
    }
    inst->eraseFromParent();
  }

  LOG(INFO) << num_removed << " redundant pending exception check removed.";
#endif

  return true;
}

//----------------------------------------------------------------------------
// Emit* Helper Functions
//----------------------------------------------------------------------------
llvm::Value* DexLang::EmitLoadMethodObjectAddr() {
  return func_->arg_begin();
}

llvm::Value* DexLang::EmitGetCurrentThread() {
  return EmitInvokeIntrinsicNoThrow(IntrinsicHelper::GetCurrentThread);
}

llvm::Value*
DexLang::EmitInvokeIntrinsicNoThrow(IntrinsicHelper::IntrinsicId intr_id) {
  DCHECK(IntrinsicHelper::GetAttr(intr_id) & IntrinsicHelper::kAttrNoThrow);
  return irb_.CreateCall(intrinsic_helper_.GetIntrinsicFunction(intr_id));
}

llvm::Value*
DexLang::EmitInvokeIntrinsicNoThrow(IntrinsicHelper::IntrinsicId intr_id,
                                    llvm::ArrayRef<llvm::Value*> args) {
  llvm::Function* intr = intrinsic_helper_.GetIntrinsicFunction(intr_id);
  DCHECK(IntrinsicHelper::GetAttr(intr_id) & IntrinsicHelper::kAttrNoThrow);
  return irb_.CreateCall(intr, args);
}

llvm::Value*
DexLang::EmitInvokeIntrinsic(unsigned dex_pc,
                             IntrinsicHelper::IntrinsicId intr_id) {
  llvm::Function* intr = intrinsic_helper_.GetIntrinsicFunction(intr_id);
  unsigned intr_attr = IntrinsicHelper::GetAttr(intr_id);
  bool may_throw = !(intr_attr & IntrinsicHelper::kAttrNoThrow);

  // Setup PC before invocation when the intrinsics may generate the exception
  if (may_throw) {
    EmitUpdateDexPC(dex_pc);
  }

  llvm::Value* ret_val = irb_.CreateCall(intr);

  if (may_throw) {
    EmitGuard_ExceptionLandingPad(dex_pc);
  }

  return ret_val;
}

llvm::Value* DexLang::EmitInvokeIntrinsic(unsigned dex_pc,
                                          IntrinsicHelper::IntrinsicId intr_id,
                                          llvm::ArrayRef<llvm::Value*> args) {
  llvm::Function* intr = intrinsic_helper_.GetIntrinsicFunction(intr_id);
  unsigned intr_attr = IntrinsicHelper::GetAttr(intr_id);
  bool may_throw = !(intr_attr & IntrinsicHelper::kAttrNoThrow);

  // Setup PC before invocation when the intrinsics may generate the exception
  if (may_throw) {
    EmitUpdateDexPC(dex_pc);
  }

  llvm::Value* ret_val = irb_.CreateCall(intr, args);

  if (may_throw) {
    EmitGuard_ExceptionLandingPad(dex_pc);
  }

  return ret_val;
}

RegCategory DexLang::GetInferredRegCategory(unsigned dex_pc, unsigned reg_idx) {
  Compiler::MethodReference mref(dex_file_, cunit_.GetDexMethodIndex());

  const InferredRegCategoryMap* map =
    verifier::MethodVerifier::GetInferredRegCategoryMap(mref);

  CHECK_NE(map, static_cast<InferredRegCategoryMap*>(NULL));

  return map->GetRegCategory(dex_pc, reg_idx);
}

llvm::Value* DexLang::EmitLoadArrayLength(llvm::Value* array) {
  // Load array length
  return EmitInvokeIntrinsicNoThrow(IntrinsicHelper::ArrayLength, array);
}

llvm::Value*
DexLang::EmitLoadStaticStorage(unsigned dex_pc, unsigned type_idx) {
  llvm::BasicBlock* block_load_static =
    CreateBasicBlockWithDexPC(dex_pc, "load_static");

  llvm::BasicBlock* block_cont = CreateBasicBlockWithDexPC(dex_pc, "cont");

  llvm::Constant* type_idx_value = irb_.getInt32(type_idx);

  // Load static storage from dex cache
  llvm::Value* storage_object_addr =
      EmitInvokeIntrinsic(dex_pc, IntrinsicHelper::LoadClassSSBFromDexCache,
                          type_idx_value);

  llvm::BasicBlock* block_original = irb_.GetInsertBlock();

  // Test: Is the static storage of this class initialized?
  llvm::Value* equal_null =
    irb_.CreateICmpEQ(storage_object_addr, irb_.GetJNull());

  irb_.CreateCondBr(equal_null, block_load_static, block_cont);

  // Failback routine to load the class object
  irb_.SetInsertPoint(block_load_static);

  llvm::Value* method_object_addr = EmitLoadMethodObjectAddr();

  llvm::Value* thread_object_addr = EmitGetCurrentThread();

  llvm::Value* loaded_storage_object_addr =
      EmitInvokeIntrinsic3(dex_pc, IntrinsicHelper::InitializeAndLoadClassSSB,
                           type_idx_value, method_object_addr,
                           thread_object_addr);

  llvm::BasicBlock* block_after_load_static = irb_.GetInsertBlock();

  irb_.CreateBr(block_cont);

  // Now the class object must be loaded
  irb_.SetInsertPoint(block_cont);

  llvm::PHINode* phi = irb_.CreatePHI(irb_.GetJObjectTy(), 2);

  phi->addIncoming(storage_object_addr, block_original);
  phi->addIncoming(loaded_storage_object_addr, block_after_load_static);

  return phi;
}

llvm::Value* DexLang::EmitConditionResult(llvm::Value* lhs, llvm::Value* rhs,
                                          CondBranchKind cond) {
  switch (cond) {
    case kCondBranch_EQ: {
      return irb_.CreateICmpEQ(lhs, rhs);
    }
    case kCondBranch_NE: {
      return irb_.CreateICmpNE(lhs, rhs);
    }
    case kCondBranch_LT: {
      return irb_.CreateICmpSLT(lhs, rhs);
    }
    case kCondBranch_GE: {
      return irb_.CreateICmpSGE(lhs, rhs);
    }
    case kCondBranch_GT: {
      return irb_.CreateICmpSGT(lhs, rhs);
    }
    case kCondBranch_LE: {
      return irb_.CreateICmpSLE(lhs, rhs);
    }
    default: {
      // Unreachable
      LOG(FATAL) << "Unknown conditional branch kind: " << cond;
      break;
    }
  }
  return NULL;
}

llvm::Value* DexLang::EmitIntArithmResultComputation(unsigned dex_pc,
                                                     llvm::Value* lhs,
                                                     llvm::Value* rhs,
                                                     IntArithmKind arithm,
                                                     JType op_jty) {
  DCHECK((op_jty == kInt) || (op_jty == kLong)) << op_jty;

  switch (arithm) {
    case kIntArithm_Add: {
      return irb_.CreateAdd(lhs, rhs);
    }
    case kIntArithm_Sub: {
      return irb_.CreateSub(lhs, rhs);
    }
    case kIntArithm_Mul: {
      return irb_.CreateMul(lhs, rhs);
    }
    case kIntArithm_Div:
    case kIntArithm_Rem: {
      return EmitIntDivRemResultComputation(dex_pc, lhs, rhs, arithm, op_jty);
    }
    case kIntArithm_And: {
      return irb_.CreateAnd(lhs, rhs);
    }
    case kIntArithm_Or: {
      return irb_.CreateOr(lhs, rhs);
    }
    case kIntArithm_Xor: {
      return irb_.CreateXor(lhs, rhs);
    }
    default: {
      LOG(FATAL) << "Unknown integer arithmetic kind: " << arithm;
      break;
    }
  }
  return NULL;
}

llvm::Value* DexLang::EmitIntDivRemResultComputation(unsigned dex_pc,
                                                     llvm::Value* dividend,
                                                     llvm::Value* divisor,
                                                     IntArithmKind arithm,
                                                     JType op_jty) {
  // Throw exception if the divisor is 0.
  EmitGuard_DivZeroException(dex_pc, divisor, op_jty);

  // Note that it's not trivial to translate integer div/rem to sdiv/srem in
  // LLVM IR since (MININT / -1) leads undefined behavior in LLVM due to
  // overflow.

  // Select intrinsic
  bool is_div = (arithm == kIntArithm_Div);
  IntrinsicHelper::IntrinsicId arithm_intrinsic = IntrinsicHelper::UnknownId;
  switch (op_jty) {
    case kInt: {
      arithm_intrinsic = (is_div) ? IntrinsicHelper::DivInt :
                                    IntrinsicHelper::RemInt;
      break;
    }
    case kLong: {
      arithm_intrinsic = (is_div) ? IntrinsicHelper::DivLong :
                                    IntrinsicHelper::RemLong;
      break;
    }
    default: {
      LOG(FATAL) << "Unsupported " << ((is_div) ? "div" : "rem") << " operation"
                    " for type: " << op_jty;
      return NULL;
    }
  }

  return EmitInvokeIntrinsic2(dex_pc, arithm_intrinsic, dividend, divisor);
}

//----------------------------------------------------------------------------
// EmitInsn* Functions
//----------------------------------------------------------------------------
void DexLang::EmitInsn_Nop(unsigned dex_pc, const Instruction* insn) {
  uint16_t insn_signature = code_item_->insns_[dex_pc];

  if (insn_signature == Instruction::kPackedSwitchSignature ||
      insn_signature == Instruction::kSparseSwitchSignature ||
      insn_signature == Instruction::kArrayDataSignature) {
    irb_.CreateUnreachable();
  } else {
    irb_.CreateBr(GetNextBasicBlock(dex_pc));
  }
  return;
}

void DexLang::EmitInsn_Move(unsigned dex_pc, const Instruction* insn,
                            JType jty) {
  DecodedInstruction dec_insn(insn);

  llvm::Value* src_value = EmitLoadDalvikReg(dec_insn.vB, jty, kReg);
  EmitStoreDalvikReg(dec_insn.vA, jty, kReg, src_value);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
  return;
}

void DexLang::EmitInsn_MoveResult(unsigned dex_pc, const Instruction* insn,
                                  JType jty) {
  DecodedInstruction dec_insn(insn);

  CHECK(retval_ != NULL) << "move-result must immediately after an invoke-kind "
                            "instruction";
  // Check the type
  CHECK_EQ(irb_.GetJType(jty, kReg), irb_.GetJType(retval_jty_, kReg))
      << "Mismatch type between the value from the most recent invoke-kind "
         "instruction (" << retval_jty_ << ") and the kind of move-result "
         "used! (" << jty << ")";

  EmitStoreDalvikReg(dec_insn.vA, retval_jty_, kReg, retval_);

  retval_ = NULL;

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
  return;
}

void DexLang::EmitInsn_MoveException(unsigned dex_pc, const Instruction* insn) {
  DecodedInstruction dec_insn(insn);

  llvm::Value* exception_object_addr =
      EmitInvokeIntrinsicNoThrow(IntrinsicHelper::GetException);

  EmitStoreDalvikReg(dec_insn.vA, kObject, kAccurate, exception_object_addr);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
  return;
}

void DexLang::EmitInsn_ReturnVoid(unsigned dex_pc, const Instruction* insn) {
  // Garbage collection safe-point
  EmitGuard_GarbageCollectionSuspend();

  // Pop the shadow frame
  EmitPopShadowFrame();

  // Return!
  irb_.CreateRetVoid();
  return;
}

void DexLang::EmitInsn_Return(unsigned dex_pc, const Instruction* insn) {
  DecodedInstruction dec_insn(insn);

  // Garbage collection safe-point
  EmitGuard_GarbageCollectionSuspend();

  // Pop the shadow frame
  //
  // NOTE: It is important to keep this AFTER the GC safe-point.  Otherwise,
  // the return value might be collected since the shadow stack is popped.
  EmitPopShadowFrame();

  // Return!
  char ret_shorty = cunit_.GetShorty()[0];
  llvm::Value* retval = EmitLoadDalvikReg(dec_insn.vA, ret_shorty, kAccurate);

  irb_.CreateRet(retval);
  return;
}

void DexLang::EmitInsn_LoadConstant(unsigned dex_pc, const Instruction* insn,
                                    JType imm_jty) {
  DecodedInstruction dec_insn(insn);

  DCHECK(imm_jty == kInt || imm_jty == kLong) << imm_jty;

  int64_t imm = 0;

  switch (insn->Opcode()) {
    // 32-bit Immediate
    case Instruction::CONST_4:
    case Instruction::CONST_16:
    case Instruction::CONST:
    case Instruction::CONST_WIDE_16:
    case Instruction::CONST_WIDE_32: {
      imm = static_cast<int64_t>(static_cast<int32_t>(dec_insn.vB));
      break;
    }
    case Instruction::CONST_HIGH16: {
      imm = static_cast<int64_t>(static_cast<int32_t>(
            static_cast<uint32_t>(static_cast<uint16_t>(dec_insn.vB)) << 16));
      break;
    }
    // 64-bit Immediate
    case Instruction::CONST_WIDE: {
      imm = static_cast<int64_t>(dec_insn.vB_wide);
      break;
    }
    case Instruction::CONST_WIDE_HIGH16: {
      imm = static_cast<int64_t>(
            static_cast<uint64_t>(static_cast<uint16_t>(dec_insn.vB)) << 48);
      break;
    }
    // Unknown opcode for load constant (unreachable)
    default: {
      LOG(FATAL) << "Unknown opcode for load constant: " << insn->Opcode();
      break;
    }
  }

  // Store the non-object register
  llvm::Type* imm_type = irb_.GetJType(imm_jty, kAccurate);
  llvm::Constant* imm_value = llvm::ConstantInt::getSigned(imm_type, imm);
  EmitStoreDalvikReg(dec_insn.vA, imm_jty, kAccurate, imm_value);

  // Store the object register if it is possible to be null.
  //
  // FIXME: Should we use GetInferredRegCategory() here to avoid store the value
  // twice?
  if (imm_jty == kInt && imm == 0) {
    EmitStoreDalvikReg(dec_insn.vA, kObject, kAccurate, irb_.GetJNull());
  }

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
  return;
}

void DexLang::EmitInsn_LoadConstantString(unsigned dex_pc,
                                          const Instruction* insn) {
  DecodedInstruction dec_insn(insn);

  uint32_t string_idx = dec_insn.vB;
  llvm::Value* string_idx_value = irb_.getInt32(string_idx);
  IntrinsicHelper::IntrinsicId intrinsic = IntrinsicHelper::UnknownId;

  if (compiler_.CanAssumeStringIsPresentInDexCache(dex_cache_, string_idx)) {
    intrinsic = IntrinsicHelper::ConstStringFast;
  } else {
    intrinsic = IntrinsicHelper::ConstString;
  }

  llvm::Value* string_addr =
      EmitInvokeIntrinsic(dex_pc, intrinsic, string_idx_value);

  EmitStoreDalvikReg(dec_insn.vA, kObject, kAccurate, string_addr);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
  return;
}

void DexLang::EmitInsn_UnconditionalBranch(unsigned dex_pc,
                                           const Instruction* insn) {
  DecodedInstruction dec_insn(insn);

  int32_t branch_offset = dec_insn.vA;

  irb_.CreateBr(GetBasicBlock(dex_pc + branch_offset));
  return;
}

void DexLang::EmitInsn_ArrayLength(unsigned dex_pc, const Instruction* insn) {
  DecodedInstruction dec_insn(insn);

  // Get the array object address
  llvm::Value* array_addr = EmitLoadDalvikReg(dec_insn.vB, kObject, kAccurate);

  // Check whether the array address is null
  EmitGuard_NullPointerException(dex_pc, array_addr);

  // Get the array length and store it to the register
  llvm::Value* array_len = EmitLoadArrayLength(array_addr);
  EmitStoreDalvikReg(dec_insn.vA, kInt, kAccurate, array_len);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
  return;
}

void DexLang::EmitInsn_NewArray(unsigned dex_pc, const Instruction* insn) {
  DecodedInstruction dec_insn(insn);

  // Prepare argument to intrinsic
  llvm::Value* array_length = EmitLoadDalvikReg(dec_insn.vB, kInt, kAccurate);
  llvm::Value* type_idx = irb_.getInt32(dec_insn.vC);

  llvm::Value* array_addr =
      EmitInvokeIntrinsic2(dex_pc, IntrinsicHelper::NewArray,
                           array_length, type_idx);

  EmitStoreDalvikReg(dec_insn.vA, kObject, kAccurate, array_addr);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
  return;
}

void DexLang::EmitInsn_UnaryConditionalBranch(unsigned dex_pc,
                                              const Instruction* insn,
                                              CondBranchKind cond) {
  DecodedInstruction dec_insn(insn);

  int8_t src_reg_cat = GetInferredRegCategory(dex_pc, dec_insn.vA);

  DCHECK_NE(kRegUnknown, src_reg_cat);
  DCHECK_NE(kRegCat2, src_reg_cat);

  int32_t branch_offset = dec_insn.vB;

  llvm::Value* src1_value;
  llvm::Value* src2_value;

  if (src_reg_cat == kRegZero) {
    src1_value = irb_.getInt32(0);
    src2_value = irb_.getInt32(0);
  } else if (src_reg_cat == kRegCat1nr) {
    src1_value = EmitLoadDalvikReg(dec_insn.vA, kInt, kReg);
    src2_value = irb_.getInt32(0);
  } else {
    src1_value = EmitLoadDalvikReg(dec_insn.vA, kObject, kAccurate);
    src2_value = irb_.GetJNull();
  }

  llvm::Value* cond_value = EmitConditionResult(src1_value, src2_value, cond);

  irb_.CreateCondBr(cond_value,
                    GetBasicBlock(dex_pc + branch_offset),
                    GetNextBasicBlock(dex_pc));
  return;
}

void DexLang::EmitInsn_BinaryConditionalBranch(unsigned dex_pc,
                                               const Instruction* insn,
                                               CondBranchKind cond) {
  DecodedInstruction dec_insn(insn);

  int8_t src1_reg_cat = GetInferredRegCategory(dex_pc, dec_insn.vA);
  int8_t src2_reg_cat = GetInferredRegCategory(dex_pc, dec_insn.vB);

  DCHECK_NE(kRegUnknown, src1_reg_cat);
  DCHECK_NE(kRegUnknown, src2_reg_cat);
  DCHECK_NE(kRegCat2, src1_reg_cat);
  DCHECK_NE(kRegCat2, src2_reg_cat);

  int32_t branch_offset = dec_insn.vC;

  llvm::Value* src1_value;
  llvm::Value* src2_value;

  if (src1_reg_cat == kRegZero && src2_reg_cat == kRegZero) {
    src1_value = irb_.getInt32(0);
    src2_value = irb_.getInt32(0);
  } else if (src1_reg_cat != kRegZero && src2_reg_cat != kRegZero) {
    CHECK_EQ(src1_reg_cat, src2_reg_cat);

    if (src1_reg_cat == kRegCat1nr) {
      src1_value = EmitLoadDalvikReg(dec_insn.vA, kInt, kAccurate);
      src2_value = EmitLoadDalvikReg(dec_insn.vB, kInt, kAccurate);
    } else {
      src1_value = EmitLoadDalvikReg(dec_insn.vA, kObject, kAccurate);
      src2_value = EmitLoadDalvikReg(dec_insn.vB, kObject, kAccurate);
    }
  } else {
    DCHECK(src1_reg_cat == kRegZero ||
           src2_reg_cat == kRegZero);

    if (src1_reg_cat == kRegZero) {
      if (src2_reg_cat == kRegCat1nr) {
        src1_value = irb_.GetJInt(0);
        src2_value = EmitLoadDalvikReg(dec_insn.vA, kInt, kAccurate);
      } else {
        src1_value = irb_.GetJNull();
        src2_value = EmitLoadDalvikReg(dec_insn.vA, kObject, kAccurate);
      }
    } else { // src2_reg_cat == kRegZero
      if (src2_reg_cat == kRegCat1nr) {
        src1_value = EmitLoadDalvikReg(dec_insn.vA, kInt, kAccurate);
        src2_value = irb_.GetJInt(0);
      } else {
        src1_value = EmitLoadDalvikReg(dec_insn.vA, kObject, kAccurate);
        src2_value = irb_.GetJNull();
      }
    }
  }

  llvm::Value* cond_value =
    EmitConditionResult(src1_value, src2_value, cond);

  irb_.CreateCondBr(cond_value,
                    GetBasicBlock(dex_pc + branch_offset),
                    GetNextBasicBlock(dex_pc));
  return;
}

void DexLang::EmitInsn_AGet(unsigned dex_pc, const Instruction* insn,
                            JType elem_jty) {
  DecodedInstruction dec_insn(insn);

  // Select corresponding intrinsic
  IntrinsicHelper::IntrinsicId aget_intrinsic = IntrinsicHelper::UnknownId;

  switch (elem_jty) {
    case kInt: {
      aget_intrinsic = IntrinsicHelper::ArrayGet;
      break;
    }
    case kLong: {
      aget_intrinsic = IntrinsicHelper::ArrayGetWide;
      break;
    }
    case kObject: {
      aget_intrinsic = IntrinsicHelper::ArrayGetObject;
      break;
    }
    case kBoolean: {
      aget_intrinsic = IntrinsicHelper::ArrayGetBoolean;
      break;
    }
    case kByte: {
      aget_intrinsic = IntrinsicHelper::ArrayGetByte;
      break;
    }
    case kChar: {
      aget_intrinsic = IntrinsicHelper::ArrayGetChar;
      break;
    }
    case kShort: {
      aget_intrinsic = IntrinsicHelper::ArrayGetShort;
      break;
    }
    default: {
      LOG(FATAL) << "Unexpected element type got in aget instruction!";
      return;
    }
  }

  // Construct argument list passed to the intrinsic
  llvm::Value* array_addr = EmitLoadDalvikReg(dec_insn.vB, kObject, kAccurate);
  llvm::Value* index_value = EmitLoadDalvikReg(dec_insn.vC, kInt, kAccurate);

  EmitGuard_ArrayException(dex_pc, array_addr, index_value);

  llvm::Value* array_element_value = EmitInvokeIntrinsic2(dex_pc,
                                                          aget_intrinsic,
                                                          array_addr,
                                                          index_value);

  EmitStoreDalvikReg(dec_insn.vA, elem_jty, kArray, array_element_value);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
  return;
}

void DexLang::EmitInsn_APut(unsigned dex_pc, const Instruction* insn,
                            JType elem_jty) {
  DecodedInstruction dec_insn(insn);

  // Select corresponding intrinsic
  IntrinsicHelper::IntrinsicId aput_intrinsic = IntrinsicHelper::UnknownId;

  switch (elem_jty) {
    case kInt: {
      aput_intrinsic = IntrinsicHelper::ArrayPut;
      break;
    }
    case kLong: {
      aput_intrinsic = IntrinsicHelper::ArrayPutWide;
      break;
    }
    case kObject: {
      aput_intrinsic = IntrinsicHelper::ArrayPutObject;
      break;
    }
    case kBoolean: {
      aput_intrinsic = IntrinsicHelper::ArrayPutBoolean;
      break;
    }
    case kByte: {
      aput_intrinsic = IntrinsicHelper::ArrayPutByte;
      break;
    }
    case kChar: {
      aput_intrinsic = IntrinsicHelper::ArrayPutChar;
      break;
    }
    case kShort: {
      aput_intrinsic = IntrinsicHelper::ArrayPutShort;
      break;
    }
    default: {
      LOG(FATAL) << "Unexpected element type got in aput instruction!";
      return;
    }
  }

  // Construct argument list passed to the intrinsic
  llvm::Value* elem_addr = EmitLoadDalvikReg(dec_insn.vA, elem_jty, kAccurate);
  llvm::Value* array_addr = EmitLoadDalvikReg(dec_insn.vB, kObject, kAccurate);
  llvm::Value* index_value = EmitLoadDalvikReg(dec_insn.vC, kInt, kAccurate);

  EmitGuard_ArrayException(dex_pc, array_addr, index_value);

  // Check the type if an object is putting
  if (elem_jty == kObject) {
    EmitInvokeIntrinsic2(dex_pc, IntrinsicHelper::CheckPutArrayElement,
                         elem_addr, array_addr);
  }

  EmitInvokeIntrinsic3(dex_pc, aput_intrinsic,
                       elem_addr, array_addr, index_value);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));

  return;
}

void DexLang::EmitInsn_SGet(unsigned dex_pc, const Instruction* insn,
                            JType field_jty) {
  DecodedInstruction dec_insn(insn);

  uint32_t field_idx = dec_insn.vB;

  int field_offset;
  int ssb_index;
  bool is_referrers_class;
  bool is_volatile;
  bool is_fast_path = compiler_.ComputeStaticFieldInfo(field_idx, &cunit_,
                                                       field_offset, ssb_index,
                                                       is_referrers_class,
                                                       is_volatile,
                                                       /* is_put */true);

  // Select corresponding intrinsic accroding to the field type and is_fast_path
  IntrinsicHelper::IntrinsicId sget_intrinsic = IntrinsicHelper::UnknownId;

  switch (field_jty) {
    case kInt: {
      sget_intrinsic =
          (is_fast_path) ? IntrinsicHelper::StaticFieldGetFast :
                           IntrinsicHelper::StaticFieldGet;
      break;
    }
    case kLong: {
      sget_intrinsic =
          (is_fast_path) ? IntrinsicHelper::StaticFieldGetWideFast :
                           IntrinsicHelper::StaticFieldGetWide;
      break;
    }
    case kObject: {
      sget_intrinsic =
          (is_fast_path) ? IntrinsicHelper::StaticFieldGetObjectFast :
                           IntrinsicHelper::StaticFieldGetObject;
      break;
    }
    case kBoolean: {
      sget_intrinsic =
          (is_fast_path) ? IntrinsicHelper::StaticFieldGetBooleanFast :
                           IntrinsicHelper::StaticFieldGetBoolean;
      break;
    }
    case kByte: {
      sget_intrinsic =
          (is_fast_path) ? IntrinsicHelper::StaticFieldGetByteFast :
                           IntrinsicHelper::StaticFieldGetByte;
      break;
    }
    case kChar: {
      sget_intrinsic =
          (is_fast_path) ? IntrinsicHelper::StaticFieldGetCharFast :
                           IntrinsicHelper::StaticFieldGetChar;
      break;
    }
    case kShort: {
      sget_intrinsic =
          (is_fast_path) ? IntrinsicHelper::StaticFieldGetShortFast :
                           IntrinsicHelper::StaticFieldGetShort;
      break;
    }
    default: {
      LOG(FATAL) << "Unexpected element type got in sget instruction!";
      return;
    }
  }

  llvm::Constant* field_idx_value = irb_.getInt32(field_idx);

  llvm::Value* static_field_value;

  if (!is_fast_path) {
    llvm::Value* method_object_addr = EmitLoadMethodObjectAddr();

    static_field_value =
        EmitInvokeIntrinsic2(dex_pc, sget_intrinsic,
                             field_idx_value, method_object_addr);
  } else {
    DCHECK_GE(field_offset, 0);

    llvm::Value* static_storage_addr = NULL;

    if (is_referrers_class) {
      // Fast path, static storage base is this method's class
      llvm::Value* method_object_addr = EmitLoadMethodObjectAddr();

      static_storage_addr =
        EmitInvokeIntrinsic(dex_pc, IntrinsicHelper::LoadDeclaringClassSSB,
                            method_object_addr);
    } else {
      // Medium path, static storage base in a different class which
      // requires checks that the other class is initialized
      DCHECK_GE(ssb_index, 0);
      static_storage_addr = EmitLoadStaticStorage(dex_pc, ssb_index);
    }

    static_field_value =
        EmitInvokeIntrinsic3(dex_pc, sget_intrinsic,
                             static_storage_addr, irb_.getInt32(field_offset),
                             irb_.getInt1(is_volatile));
  }

  EmitStoreDalvikReg(dec_insn.vA, field_jty, kField, static_field_value);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
  return;
}

void DexLang::EmitInsn_SPut(unsigned dex_pc, const Instruction* insn,
                            JType field_jty) {
  DecodedInstruction dec_insn(insn);

  uint32_t field_idx = dec_insn.vB;

  llvm::Value* new_value = EmitLoadDalvikReg(dec_insn.vA, field_jty, kField);

  int field_offset;
  int ssb_index;
  bool is_referrers_class;
  bool is_volatile;
  bool is_fast_path = compiler_.ComputeStaticFieldInfo(field_idx, &cunit_,
                                                       field_offset, ssb_index,
                                                       is_referrers_class,
                                                       is_volatile,
                                                       /* is_put */true);

  // Select corresponding intrinsic accroding to the field type and is_fast_path
  IntrinsicHelper::IntrinsicId sput_intrinsic = IntrinsicHelper::UnknownId;

  switch (field_jty) {
    case kInt: {
      sput_intrinsic =
          (is_fast_path) ? IntrinsicHelper::StaticFieldPutFast :
                           IntrinsicHelper::StaticFieldPut;
      break;
    }
    case kLong: {
      sput_intrinsic =
          (is_fast_path) ? IntrinsicHelper::StaticFieldPutWideFast :
                           IntrinsicHelper::StaticFieldPutWide;
      break;
    }
    case kObject: {
      sput_intrinsic =
          (is_fast_path) ? IntrinsicHelper::StaticFieldPutObjectFast :
                           IntrinsicHelper::StaticFieldPutObject;
      break;
    }
    case kBoolean: {
      sput_intrinsic =
          (is_fast_path) ? IntrinsicHelper::StaticFieldPutBooleanFast :
                           IntrinsicHelper::StaticFieldPutBoolean;
      break;
    }
    case kByte: {
      sput_intrinsic =
          (is_fast_path) ? IntrinsicHelper::StaticFieldPutByteFast :
                           IntrinsicHelper::StaticFieldPutByte;
      break;
    }
    case kChar: {
      sput_intrinsic =
          (is_fast_path) ? IntrinsicHelper::StaticFieldPutCharFast :
                           IntrinsicHelper::StaticFieldPutChar;
      break;
    }
    case kShort: {
      sput_intrinsic =
          (is_fast_path) ? IntrinsicHelper::StaticFieldPutShortFast :
                           IntrinsicHelper::StaticFieldPutShort;
      break;
    }
    default: {
      LOG(FATAL) << "Unexpected element type got in sput instruction!";
      return;
    }
  }

  if (!is_fast_path) {
    llvm::Constant* field_idx_value = irb_.getInt32(dec_insn.vB);

    llvm::Value* method_object_addr = EmitLoadMethodObjectAddr();

    EmitInvokeIntrinsic3(dex_pc, sput_intrinsic,
                         field_idx_value, method_object_addr, new_value);
  } else {
    DCHECK_GE(field_offset, 0);

    llvm::Value* static_storage_addr = NULL;

    if (is_referrers_class) {
      // Fast path, static storage base is this method's class
      llvm::Value* method_object_addr = EmitLoadMethodObjectAddr();

      static_storage_addr =
        EmitInvokeIntrinsic(dex_pc, IntrinsicHelper::LoadDeclaringClassSSB,
                            method_object_addr);
    } else {
      // Medium path, static storage base in a different class which
      // requires checks that the other class is initialized
      DCHECK_GE(ssb_index, 0);
      static_storage_addr = EmitLoadStaticStorage(dex_pc, ssb_index);
    }

    EmitInvokeIntrinsic4(dex_pc, sput_intrinsic,
                         static_storage_addr, irb_.getInt32(field_offset),
                         irb_.getInt1(is_volatile), new_value);
  }

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
  return;
}

void DexLang::EmitInsn_Invoke(unsigned dex_pc, const Instruction* insn,
                              InvokeType invoke_type, InvokeArgFmt arg_fmt) {
  DecodedInstruction dec_insn(insn);

  bool is_static = (invoke_type == kStatic);
  uint32_t callee_method_idx = dec_insn.vB;

  // Compute invoke related information for compiler decision
  int vtable_idx = -1;
  uintptr_t direct_code = 0; // Currently unused
  uintptr_t direct_method = 0;
  bool is_fast_path = compiler_.ComputeInvokeInfo(callee_method_idx, &cunit_,
                                                  invoke_type, vtable_idx,
                                                  direct_code, direct_method);

  // Load *this* actual parameter
  llvm::Value* this_addr = NULL;

  if (is_static) {
    this_addr = irb_.GetJNull();
  } else {
    // Test: Is *this* parameter equal to null?
    this_addr = (arg_fmt == kArgReg) ?
        EmitLoadDalvikReg(dec_insn.arg[0], kObject, kAccurate):
        EmitLoadDalvikReg(dec_insn.vC + 0, kObject, kAccurate);

    EmitGuard_NullPointerException(dex_pc, this_addr);
  }

  // Load the method object
  llvm::Value* callee_method_object_addr = NULL;

  llvm::Value* callee_method_idx_value = irb_.getInt32(callee_method_idx);

  if (!is_fast_path) {
    llvm::Value* caller_method_object_addr = EmitLoadMethodObjectAddr();

    llvm::Value* thread_object_addr = EmitGetCurrentThread();

    callee_method_object_addr =
        EmitInvokeIntrinsic5(dex_pc, IntrinsicHelper::GetCalleeMethodObjAddr,
                             this_addr,
                             callee_method_idx_value,
                             caller_method_object_addr,
                             thread_object_addr,
                             irb_.getInt32(static_cast<unsigned>(invoke_type)));
  } else {
    switch (invoke_type) {
      case kStatic:
      case kDirect: {
        if (direct_method != 0u &&
            direct_method != static_cast<uintptr_t>(-1)) {
          callee_method_object_addr =
            irb_.CreateIntToPtr(irb_.GetPtrEquivInt(direct_method),
                                irb_.GetJMethodTy());
        } else {
          callee_method_object_addr =
              EmitInvokeIntrinsic(dex_pc,
                                  IntrinsicHelper::GetSDCalleeMethodObjAddrFast,
                                  callee_method_idx_value);
        }
        break;
      }
      case kVirtual: {
        DCHECK(vtable_idx != -1);
        callee_method_object_addr =
            EmitInvokeIntrinsic2(dex_pc,
                                 IntrinsicHelper::GetVirtualCalleeMethodObjAddrFast,
                                 irb_.getInt32(vtable_idx), this_addr);
        break;
      }
      case kSuper: {
        LOG(FATAL) << "invoke-super should be promoted to invoke-direct in "
                      "the fast path.";
        break;
      }
      case kInterface: {
        llvm::Value* caller_method_object_addr = EmitLoadMethodObjectAddr();

        llvm::Value* thread_object_addr = EmitGetCurrentThread();

        callee_method_object_addr =
            EmitInvokeIntrinsic4(dex_pc,
                                 IntrinsicHelper::GetInterfaceCalleeMethodObjAddrFast,
                                 this_addr,
                                 callee_method_idx_value,
                                 caller_method_object_addr,
                                 thread_object_addr);
        break;
      }
    }
  }

  // Get the shorty of the callee
  uint32_t callee_shorty_size;
  const DexFile::MethodId& callee_method_id =
      dex_file_->GetMethodId(callee_method_idx);
  const char* callee_shorty =
      dex_file_->GetMethodShorty(callee_method_id, &callee_shorty_size);
  CHECK_GE(callee_shorty_size, 1u);

  JType callee_ret_jty = GetJTypeFromShorty(callee_shorty[0]);

  // Select the corresponding intrinsic according to the return type
  IntrinsicHelper::IntrinsicId invoke_intrinsic = IntrinsicHelper::UnknownId;

  if (callee_ret_jty == kVoid) {
    invoke_intrinsic = IntrinsicHelper::InvokeRetVoid;
  } else {
    switch (GetRegCategoryFromJType(callee_ret_jty)) {
      case kRegCat1nr: {
        invoke_intrinsic = IntrinsicHelper::InvokeRetCat1;
        break;
      }
      case kRegCat2: {
        invoke_intrinsic = IntrinsicHelper::InvokeRetCat2;
        break;
      }
      case kRegObject: {
        invoke_intrinsic = IntrinsicHelper::InvokeRetObject;
        break;
      }
      default: {
        LOG(FATAL) << "Unknown register category for type: "
                   << callee_ret_jty;
        break;
      }
    }
  }

  // Load arguments for invoke intrinsics
  std::vector<llvm::Value*> args;

  // Callee's method id goes first
  args.push_back(callee_method_object_addr);

  // Load arguments listing in the dec_insn
  unsigned arg_idx = 0;

  if (!is_static) {
    // Push "this" for non-static method
    args.push_back(this_addr);
    arg_idx++;
  }

  // Load argument values according to the shorty
  for (uint32_t i = 1; i < callee_shorty_size; i++) {
    unsigned reg_idx = (arg_fmt == kArgReg) ? (dec_insn.vC + arg_idx) :
                                              (dec_insn.arg[arg_idx]);
    JType jty = GetJTypeFromShorty(callee_shorty[i]);
    args.push_back(EmitLoadDalvikReg(reg_idx, jty, kAccurate));
    arg_idx++;

    if (GetRegCategoryFromJType(jty) == kRegCat2) {
      // Wide types occupied two registers
      arg_idx++;
    }
  }

  DCHECK_EQ(arg_idx, dec_insn.vA)
    << "Actual argument mismatch for callee: "
    << PrettyMethod(callee_method_idx, *dex_file_);

  llvm::Value* retval = EmitInvokeIntrinsic(dex_pc, invoke_intrinsic, args);

  // Store the return value for the subsequent move-result
  if (callee_shorty[0] != 'V') {
    retval_ = retval;
    retval_jty_ = GetJTypeFromShorty(callee_shorty[0]);
  } else {
    retval_ = NULL;
  }

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
  return;
}

void DexLang::EmitInsn_IntArithm(unsigned dex_pc, const Instruction* insn,
                                 IntArithmKind arithm, JType op_jty,
                                 bool is_2addr) {
  DecodedInstruction dec_insn(insn);

  DCHECK(op_jty == kInt || op_jty == kLong) << op_jty;

  llvm::Value* src1_value;
  llvm::Value* src2_value;

  if (is_2addr) {
    src1_value = EmitLoadDalvikReg(dec_insn.vA, op_jty, kAccurate);
    src2_value = EmitLoadDalvikReg(dec_insn.vB, op_jty, kAccurate);
  } else {
    src1_value = EmitLoadDalvikReg(dec_insn.vB, op_jty, kAccurate);
    src2_value = EmitLoadDalvikReg(dec_insn.vC, op_jty, kAccurate);
  }

  llvm::Value* result_value =
    EmitIntArithmResultComputation(dex_pc, src1_value, src2_value,
                                   arithm, op_jty);

  EmitStoreDalvikReg(dec_insn.vA, op_jty, kAccurate, result_value);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
  return;
}

void DexLang::EmitInsn_IntArithmImmediate(unsigned dex_pc,
                                          const Instruction* insn,
                                          IntArithmKind arithm) {
  DecodedInstruction dec_insn(insn);

  llvm::Value* src_value = EmitLoadDalvikReg(dec_insn.vB, kInt, kAccurate);

  llvm::Value* imm_value = irb_.getInt32(dec_insn.vC);

  llvm::Value* result_value =
    EmitIntArithmResultComputation(dex_pc, src_value, imm_value, arithm, kInt);

  EmitStoreDalvikReg(dec_insn.vA, kInt, kAccurate, result_value);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
  return;
}

void DexLang::EmitInsn_FPArithm(unsigned dex_pc, const Instruction* insn,
                                FPArithmKind arithm, JType op_jty,
                                bool is_2addr) {
  DecodedInstruction dec_insn(insn);

  DCHECK(op_jty == kFloat || op_jty == kDouble) << op_jty;

  llvm::Value* src1_value;
  llvm::Value* src2_value;

  if (is_2addr) {
    src1_value = EmitLoadDalvikReg(dec_insn.vA, op_jty, kAccurate);
    src2_value = EmitLoadDalvikReg(dec_insn.vB, op_jty, kAccurate);
  } else {
    src1_value = EmitLoadDalvikReg(dec_insn.vB, op_jty, kAccurate);
    src2_value = EmitLoadDalvikReg(dec_insn.vC, op_jty, kAccurate);
  }

  llvm::Value* result_value;
  switch (arithm) {
    case kFPArithm_Add: {
      result_value = irb_.CreateFAdd(src1_value, src2_value);
      break;
    }
    case kFPArithm_Sub: {
      result_value = irb_.CreateFSub(src1_value, src2_value);
      break;
    }
    case kFPArithm_Mul: {
      result_value = irb_.CreateFMul(src1_value, src2_value);
      break;
    }
    case kFPArithm_Div: {
      result_value = irb_.CreateFDiv(src1_value, src2_value);
      break;
    }
    case kFPArithm_Rem: {
      result_value = irb_.CreateFRem(src1_value, src2_value);
      break;
    }
    default: {
      LOG(FATAL) << "Unknown floating-point arithmetic kind: " << arithm;
      return;
    }
  }

  EmitStoreDalvikReg(dec_insn.vA, op_jty, kAccurate, result_value);

  irb_.CreateBr(GetNextBasicBlock(dex_pc));
  return;
}

bool DexLang::EmitInstructions() {
  unsigned dex_pc = 0;
  while (dex_pc < code_item_->insns_size_in_code_units_) {
    const Instruction* insn = Instruction::At(code_item_->insns_ + dex_pc);
    if (!EmitInstruction(dex_pc, insn)) {
      return false;
    }
    dex_pc += insn->SizeInCodeUnits();
  }
  return true;
}

bool DexLang::EmitInstruction(unsigned dex_pc, const Instruction* insn) {
  // Set the IRBuilder insertion point
  irb_.SetInsertPoint(GetBasicBlock(dex_pc));

#define ARGS dex_pc, insn

  // Dispatch the instruction
  switch (insn->Opcode()) {
    case Instruction::NOP: {
      EmitInsn_Nop(ARGS);
      break;
    }
    case Instruction::MOVE:
    case Instruction::MOVE_FROM16:
    case Instruction::MOVE_16: {
      EmitInsn_Move(ARGS, kInt);
      break;
    }
    case Instruction::MOVE_WIDE:
    case Instruction::MOVE_WIDE_FROM16:
    case Instruction::MOVE_WIDE_16: {
      EmitInsn_Move(ARGS, kLong);
      break;
    }
    case Instruction::MOVE_OBJECT:
    case Instruction::MOVE_OBJECT_FROM16:
    case Instruction::MOVE_OBJECT_16: {
      EmitInsn_Move(ARGS, kObject);
      break;
    }
    case Instruction::MOVE_RESULT: {
      EmitInsn_MoveResult(ARGS, kInt);
      break;
    }
    case Instruction::MOVE_RESULT_WIDE: {
      EmitInsn_MoveResult(ARGS, kLong);
      break;
    }
    case Instruction::MOVE_RESULT_OBJECT: {
      EmitInsn_MoveResult(ARGS, kObject);
      break;
    }
    case Instruction::MOVE_EXCEPTION: {
      EmitInsn_MoveException(ARGS);
      break;
    }
    case Instruction::RETURN_VOID: {
      EmitInsn_ReturnVoid(ARGS);
      break;
    }
    case Instruction::RETURN:
    case Instruction::RETURN_WIDE:
    case Instruction::RETURN_OBJECT: {
      EmitInsn_Return(ARGS);
      break;
    }
    case Instruction::CONST_4:
    case Instruction::CONST_16:
    case Instruction::CONST:
    case Instruction::CONST_HIGH16: {
      EmitInsn_LoadConstant(ARGS, kInt);
      break;
    }
    case Instruction::CONST_WIDE_16:
    case Instruction::CONST_WIDE_32:
    case Instruction::CONST_WIDE:
    case Instruction::CONST_WIDE_HIGH16: {
      EmitInsn_LoadConstant(ARGS, kLong);
      break;
    }
    case Instruction::CONST_STRING:
    case Instruction::CONST_STRING_JUMBO: {
      EmitInsn_LoadConstantString(ARGS);
      break;
    }
    case Instruction::CONST_CLASS:
      //EmitInsn_LoadConstantClass(ARGS);
      break;

    case Instruction::MONITOR_ENTER:
      //EmitInsn_MonitorEnter(ARGS);
      break;

    case Instruction::MONITOR_EXIT:
      //EmitInsn_MonitorExit(ARGS);
      break;

    case Instruction::CHECK_CAST:
      //EmitInsn_CheckCast(ARGS);
      break;

    case Instruction::INSTANCE_OF:
      //EmitInsn_InstanceOf(ARGS);
      break;

    case Instruction::ARRAY_LENGTH: {
      EmitInsn_ArrayLength(ARGS);
      break;
    }
    case Instruction::NEW_INSTANCE:
      //EmitInsn_NewInstance(ARGS);
      break;

    case Instruction::NEW_ARRAY: {
      EmitInsn_NewArray(ARGS);
      break;
    }
    case Instruction::FILLED_NEW_ARRAY:
      //EmitInsn_FilledNewArray(ARGS, false);
      break;

    case Instruction::FILLED_NEW_ARRAY_RANGE:
      //EmitInsn_FilledNewArray(ARGS, true);
      break;

    case Instruction::FILL_ARRAY_DATA:
      //EmitInsn_FillArrayData(ARGS);
      break;

    case Instruction::THROW:
      //EmitInsn_ThrowException(ARGS);
      break;

    case Instruction::GOTO:
    case Instruction::GOTO_16:
    case Instruction::GOTO_32: {
      EmitInsn_UnconditionalBranch(ARGS);
      break;
    }
    case Instruction::PACKED_SWITCH:
      //EmitInsn_PackedSwitch(ARGS);
      break;

    case Instruction::SPARSE_SWITCH:
      //EmitInsn_SparseSwitch(ARGS);
      break;

    case Instruction::CMPL_FLOAT:
      //EmitInsn_FPCompare(ARGS, kFloat, false);
      break;

    case Instruction::CMPG_FLOAT:
      //EmitInsn_FPCompare(ARGS, kFloat, true);
      break;

    case Instruction::CMPL_DOUBLE:
      //EmitInsn_FPCompare(ARGS, kDouble, false);
      break;

    case Instruction::CMPG_DOUBLE:
      //EmitInsn_FPCompare(ARGS, kDouble, true);
      break;

    case Instruction::CMP_LONG:
      //EmitInsn_LongCompare(ARGS);
      break;

    case Instruction::IF_EQ: {
      EmitInsn_BinaryConditionalBranch(ARGS, kCondBranch_EQ);
      break;
    }
    case Instruction::IF_NE: {
      EmitInsn_BinaryConditionalBranch(ARGS, kCondBranch_NE);
      break;
    }
    case Instruction::IF_LT: {
      EmitInsn_BinaryConditionalBranch(ARGS, kCondBranch_LT);
      break;
    }
    case Instruction::IF_GE: {
      EmitInsn_BinaryConditionalBranch(ARGS, kCondBranch_GE);
      break;
    }
    case Instruction::IF_GT: {
      EmitInsn_BinaryConditionalBranch(ARGS, kCondBranch_GT);
      break;
    }
    case Instruction::IF_LE: {
      EmitInsn_BinaryConditionalBranch(ARGS, kCondBranch_LE);
      break;
    }
    case Instruction::IF_EQZ: {
      EmitInsn_UnaryConditionalBranch(ARGS, kCondBranch_EQ);
      break;
    }
    case Instruction::IF_NEZ: {
      EmitInsn_UnaryConditionalBranch(ARGS, kCondBranch_NE);
      break;
    }
    case Instruction::IF_LTZ: {
      EmitInsn_UnaryConditionalBranch(ARGS, kCondBranch_LT);
      break;
    }
    case Instruction::IF_GEZ: {
      EmitInsn_UnaryConditionalBranch(ARGS, kCondBranch_GE);
      break;
    }
    case Instruction::IF_GTZ: {
      EmitInsn_UnaryConditionalBranch(ARGS, kCondBranch_GT);
      break;
    }
    case Instruction::IF_LEZ: {
      EmitInsn_UnaryConditionalBranch(ARGS, kCondBranch_LE);
      break;
    }
    case Instruction::AGET: {
      EmitInsn_AGet(ARGS, kInt);
      break;
    }
    case Instruction::AGET_WIDE: {
      EmitInsn_AGet(ARGS, kLong);
      break;
    }
    case Instruction::AGET_OBJECT: {
      EmitInsn_AGet(ARGS, kObject);
      break;
    }
    case Instruction::AGET_BOOLEAN: {
      EmitInsn_AGet(ARGS, kBoolean);
      break;
    }
    case Instruction::AGET_BYTE: {
      EmitInsn_AGet(ARGS, kByte);
      break;
    }
    case Instruction::AGET_CHAR: {
      EmitInsn_AGet(ARGS, kChar);
      break;
    }
    case Instruction::AGET_SHORT: {
      EmitInsn_AGet(ARGS, kShort);
      break;
    }
    case Instruction::APUT: {
      EmitInsn_APut(ARGS, kInt);
      break;
    }
    case Instruction::APUT_WIDE: {
      EmitInsn_APut(ARGS, kLong);
      break;
    }
    case Instruction::APUT_OBJECT: {
      EmitInsn_APut(ARGS, kObject);
      break;
    }
    case Instruction::APUT_BOOLEAN: {
      EmitInsn_APut(ARGS, kBoolean);
      break;
    }
    case Instruction::APUT_BYTE: {
      EmitInsn_APut(ARGS, kByte);
      break;
    }
    case Instruction::APUT_CHAR: {
      EmitInsn_APut(ARGS, kChar);
      break;
    }
    case Instruction::APUT_SHORT: {
      EmitInsn_APut(ARGS, kShort);
      break;
    }
    case Instruction::IGET:
      //EmitInsn_IGet(ARGS, kInt);
      break;

    case Instruction::IGET_WIDE:
      //EmitInsn_IGet(ARGS, kLong);
      break;

    case Instruction::IGET_OBJECT:
      //EmitInsn_IGet(ARGS, kObject);
      break;

    case Instruction::IGET_BOOLEAN:
      //EmitInsn_IGet(ARGS, kBoolean);
      break;

    case Instruction::IGET_BYTE:
      //EmitInsn_IGet(ARGS, kByte);
      break;

    case Instruction::IGET_CHAR:
      //EmitInsn_IGet(ARGS, kChar);
      break;

    case Instruction::IGET_SHORT:
      //EmitInsn_IGet(ARGS, kShort);
      break;

    case Instruction::IPUT:
      //EmitInsn_IPut(ARGS, kInt);
      break;

    case Instruction::IPUT_WIDE:
      //EmitInsn_IPut(ARGS, kLong);
      break;

    case Instruction::IPUT_OBJECT:
      //EmitInsn_IPut(ARGS, kObject);
      break;

    case Instruction::IPUT_BOOLEAN:
      //EmitInsn_IPut(ARGS, kBoolean);
      break;

    case Instruction::IPUT_BYTE:
      //EmitInsn_IPut(ARGS, kByte);
      break;

    case Instruction::IPUT_CHAR:
      //EmitInsn_IPut(ARGS, kChar);
      break;

    case Instruction::IPUT_SHORT:
      //EmitInsn_IPut(ARGS, kShort);
      break;

    case Instruction::SGET: {
      EmitInsn_SGet(ARGS, kInt);
      break;
    }
    case Instruction::SGET_WIDE: {
      EmitInsn_SGet(ARGS, kLong);
      break;
    }
    case Instruction::SGET_OBJECT: {
      EmitInsn_SGet(ARGS, kObject);
      break;
    }
    case Instruction::SGET_BOOLEAN: {
      EmitInsn_SGet(ARGS, kBoolean);
      break;
    }
    case Instruction::SGET_BYTE: {
      EmitInsn_SGet(ARGS, kByte);
      break;
    }
    case Instruction::SGET_CHAR: {
      EmitInsn_SGet(ARGS, kChar);
      break;
    }
    case Instruction::SGET_SHORT: {
      EmitInsn_SGet(ARGS, kShort);
      break;
    }
    case Instruction::SPUT: {
      EmitInsn_SPut(ARGS, kInt);
      break;
    }
    case Instruction::SPUT_WIDE: {
      EmitInsn_SPut(ARGS, kLong);
      break;
    }
    case Instruction::SPUT_OBJECT: {
      EmitInsn_SPut(ARGS, kObject);
      break;
    }
    case Instruction::SPUT_BOOLEAN: {
      EmitInsn_SPut(ARGS, kBoolean);
      break;
    }
    case Instruction::SPUT_BYTE: {
      EmitInsn_SPut(ARGS, kByte);
      break;
    }
    case Instruction::SPUT_CHAR: {
      EmitInsn_SPut(ARGS, kChar);
      break;
    }
    case Instruction::SPUT_SHORT: {
      EmitInsn_SPut(ARGS, kShort);
      break;
    }
    case Instruction::INVOKE_VIRTUAL: {
      EmitInsn_Invoke(ARGS, kVirtual, kArgReg);
      break;
    }
    case Instruction::INVOKE_SUPER: {
      EmitInsn_Invoke(ARGS, kSuper, kArgReg);
      break;
    }
    case Instruction::INVOKE_DIRECT: {
      EmitInsn_Invoke(ARGS, kDirect, kArgReg);
      break;
    }
    case Instruction::INVOKE_STATIC: {
      EmitInsn_Invoke(ARGS, kStatic, kArgReg);
      break;
    }
    case Instruction::INVOKE_INTERFACE: {
      EmitInsn_Invoke(ARGS, kInterface, kArgReg);
      break;
    }
    case Instruction::INVOKE_VIRTUAL_RANGE: {
      EmitInsn_Invoke(ARGS, kVirtual, kArgRange);
      break;
    }
    case Instruction::INVOKE_SUPER_RANGE: {
      EmitInsn_Invoke(ARGS, kSuper, kArgRange);
      break;
    }
    case Instruction::INVOKE_DIRECT_RANGE: {
      EmitInsn_Invoke(ARGS, kDirect, kArgRange);
      break;
    }
    case Instruction::INVOKE_STATIC_RANGE: {
      EmitInsn_Invoke(ARGS, kStatic, kArgRange);
      break;
    }
    case Instruction::INVOKE_INTERFACE_RANGE: {
      EmitInsn_Invoke(ARGS, kInterface, kArgRange);
      break;
    }
    case Instruction::NEG_INT:
      //EmitInsn_Neg(ARGS, kInt);
      break;

    case Instruction::NOT_INT:
      //EmitInsn_Not(ARGS, kInt);
      break;

    case Instruction::NEG_LONG:
      //EmitInsn_Neg(ARGS, kLong);
      break;

    case Instruction::NOT_LONG:
      //EmitInsn_Not(ARGS, kLong);
      break;

    case Instruction::NEG_FLOAT:
      //EmitInsn_FNeg(ARGS, kFloat);
      break;

    case Instruction::NEG_DOUBLE:
      //EmitInsn_FNeg(ARGS, kDouble);
      break;

    case Instruction::INT_TO_LONG:
      //EmitInsn_SExt(ARGS);
      break;

    case Instruction::INT_TO_FLOAT:
      //EmitInsn_IntToFP(ARGS, kInt, kFloat);
      break;

    case Instruction::INT_TO_DOUBLE:
      //EmitInsn_IntToFP(ARGS, kInt, kDouble);
      break;

    case Instruction::LONG_TO_INT:
      //EmitInsn_Trunc(ARGS);
      break;

    case Instruction::LONG_TO_FLOAT:
      //EmitInsn_IntToFP(ARGS, kLong, kFloat);
      break;

    case Instruction::LONG_TO_DOUBLE:
      //EmitInsn_IntToFP(ARGS, kLong, kDouble);
      break;

    case Instruction::FLOAT_TO_INT:
      //EmitInsn_FPToInt(ARGS, kFloat, kInt, F2I);
      break;

    case Instruction::FLOAT_TO_LONG:
      //EmitInsn_FPToInt(ARGS, kFloat, kLong, F2L);
      break;

    case Instruction::FLOAT_TO_DOUBLE:
      //EmitInsn_FExt(ARGS);
      break;

    case Instruction::DOUBLE_TO_INT:
      //EmitInsn_FPToInt(ARGS, kDouble, kInt, D2I);
      break;

    case Instruction::DOUBLE_TO_LONG:
      //EmitInsn_FPToInt(ARGS, kDouble, kLong, D2L);
      break;

    case Instruction::DOUBLE_TO_FLOAT:
      //EmitInsn_FTrunc(ARGS);
      break;

    case Instruction::INT_TO_BYTE:
      //EmitInsn_TruncAndSExt(ARGS, 8);
      break;

    case Instruction::INT_TO_CHAR:
      //EmitInsn_TruncAndZExt(ARGS, 16);
      break;

    case Instruction::INT_TO_SHORT:
      //EmitInsn_TruncAndSExt(ARGS, 16);
      break;

    case Instruction::ADD_INT: {
      EmitInsn_IntArithm(ARGS, kIntArithm_Add, kInt, false);
      break;
    }
    case Instruction::SUB_INT: {
      EmitInsn_IntArithm(ARGS, kIntArithm_Sub, kInt, false);
      break;
    }
    case Instruction::MUL_INT: {
      EmitInsn_IntArithm(ARGS, kIntArithm_Mul, kInt, false);
      break;
    }
    case Instruction::DIV_INT: {
      EmitInsn_IntArithm(ARGS, kIntArithm_Div, kInt, false);
      break;
    }
    case Instruction::REM_INT: {
      EmitInsn_IntArithm(ARGS, kIntArithm_Rem, kInt, false);
      break;
    }
    case Instruction::AND_INT: {
      EmitInsn_IntArithm(ARGS, kIntArithm_And, kInt, false);
      break;
    }
    case Instruction::OR_INT: {
      EmitInsn_IntArithm(ARGS, kIntArithm_Or, kInt, false);
      break;
    }
    case Instruction::XOR_INT: {
      EmitInsn_IntArithm(ARGS, kIntArithm_Xor, kInt, false);
      break;
    }
    case Instruction::SHL_INT:
      //EmitInsn_IntShiftArithm(ARGS, kIntArithm_Shl, kInt, false);
      break;

    case Instruction::SHR_INT:
      //EmitInsn_IntShiftArithm(ARGS, kIntArithm_Shr, kInt, false);
      break;

    case Instruction::USHR_INT:
      //EmitInsn_IntShiftArithm(ARGS, kIntArithm_UShr, kInt, false);
      break;

    case Instruction::ADD_LONG: {
      EmitInsn_IntArithm(ARGS, kIntArithm_Add, kLong, false);
      break;
    }
    case Instruction::SUB_LONG: {
      EmitInsn_IntArithm(ARGS, kIntArithm_Sub, kLong, false);
      break;
    }
    case Instruction::MUL_LONG: {
      EmitInsn_IntArithm(ARGS, kIntArithm_Mul, kLong, false);
      break;
    }
    case Instruction::DIV_LONG: {
      EmitInsn_IntArithm(ARGS, kIntArithm_Div, kLong, false);
      break;
    }
    case Instruction::REM_LONG: {
      EmitInsn_IntArithm(ARGS, kIntArithm_Rem, kLong, false);
      break;
    }
    case Instruction::AND_LONG: {
      EmitInsn_IntArithm(ARGS, kIntArithm_And, kLong, false);
      break;
    }
    case Instruction::OR_LONG: {
      EmitInsn_IntArithm(ARGS, kIntArithm_Or, kLong, false);
      break;
    }
    case Instruction::XOR_LONG: {
      EmitInsn_IntArithm(ARGS, kIntArithm_Xor, kLong, false);
      break;
    }
    case Instruction::SHL_LONG:
      //EmitInsn_IntShiftArithm(ARGS, kIntArithm_Shl, kLong, false);
      break;

    case Instruction::SHR_LONG:
      //EmitInsn_IntShiftArithm(ARGS, kIntArithm_Shr, kLong, false);
      break;

    case Instruction::USHR_LONG:
      //EmitInsn_IntShiftArithm(ARGS, kIntArithm_UShr, kLong, false);
      break;

    case Instruction::ADD_FLOAT: {
      EmitInsn_FPArithm(ARGS, kFPArithm_Add, kFloat, false);
      break;
    }
    case Instruction::SUB_FLOAT: {
      EmitInsn_FPArithm(ARGS, kFPArithm_Sub, kFloat, false);
      break;
    }
    case Instruction::MUL_FLOAT: {
      EmitInsn_FPArithm(ARGS, kFPArithm_Mul, kFloat, false);
      break;
    }
    case Instruction::DIV_FLOAT: {
      EmitInsn_FPArithm(ARGS, kFPArithm_Div, kFloat, false);
      break;
    }
    case Instruction::REM_FLOAT: {
      EmitInsn_FPArithm(ARGS, kFPArithm_Rem, kFloat, false);
      break;
    }
    case Instruction::ADD_DOUBLE: {
      EmitInsn_FPArithm(ARGS, kFPArithm_Add, kDouble, false);
      break;
    }
    case Instruction::SUB_DOUBLE: {
      EmitInsn_FPArithm(ARGS, kFPArithm_Sub, kDouble, false);
      break;
    }
    case Instruction::MUL_DOUBLE: {
      EmitInsn_FPArithm(ARGS, kFPArithm_Mul, kDouble, false);
      break;
    }
    case Instruction::DIV_DOUBLE: {
      EmitInsn_FPArithm(ARGS, kFPArithm_Div, kDouble, false);
      break;
    }
    case Instruction::REM_DOUBLE: {
      EmitInsn_FPArithm(ARGS, kFPArithm_Rem, kDouble, false);
      break;
    }
    case Instruction::ADD_INT_2ADDR: {
      EmitInsn_IntArithm(ARGS, kIntArithm_Add, kInt, true);
      break;
    }
    case Instruction::SUB_INT_2ADDR: {
      EmitInsn_IntArithm(ARGS, kIntArithm_Sub, kInt, true);
      break;
    }
    case Instruction::MUL_INT_2ADDR: {
      EmitInsn_IntArithm(ARGS, kIntArithm_Mul, kInt, true);
      break;
    }
    case Instruction::DIV_INT_2ADDR: {
      EmitInsn_IntArithm(ARGS, kIntArithm_Div, kInt, true);
      break;
    }
    case Instruction::REM_INT_2ADDR: {
      EmitInsn_IntArithm(ARGS, kIntArithm_Rem, kInt, true);
      break;
    }
    case Instruction::AND_INT_2ADDR: {
      EmitInsn_IntArithm(ARGS, kIntArithm_And, kInt, true);
      break;
    }
    case Instruction::OR_INT_2ADDR: {
      EmitInsn_IntArithm(ARGS, kIntArithm_Or, kInt, true);
      break;
    }
    case Instruction::XOR_INT_2ADDR: {
      EmitInsn_IntArithm(ARGS, kIntArithm_Xor, kInt, true);
      break;
    }
    case Instruction::SHL_INT_2ADDR:
      //EmitInsn_IntShiftArithm(ARGS, kIntArithm_Shl, kInt, true);
      break;

    case Instruction::SHR_INT_2ADDR:
      //EmitInsn_IntShiftArithm(ARGS, kIntArithm_Shr, kInt, true);
      break;

    case Instruction::USHR_INT_2ADDR:
      //EmitInsn_IntShiftArithm(ARGS, kIntArithm_UShr, kInt, true);
      break;

    case Instruction::ADD_LONG_2ADDR: {
      EmitInsn_IntArithm(ARGS, kIntArithm_Add, kLong, true);
      break;
    }
    case Instruction::SUB_LONG_2ADDR: {
      EmitInsn_IntArithm(ARGS, kIntArithm_Sub, kLong, true);
      break;
    }
    case Instruction::MUL_LONG_2ADDR: {
      EmitInsn_IntArithm(ARGS, kIntArithm_Mul, kLong, true);
      break;
    }
    case Instruction::DIV_LONG_2ADDR: {
      EmitInsn_IntArithm(ARGS, kIntArithm_Div, kLong, true);
      break;
    }
    case Instruction::REM_LONG_2ADDR: {
      EmitInsn_IntArithm(ARGS, kIntArithm_Rem, kLong, true);
      break;
    }
    case Instruction::AND_LONG_2ADDR: {
      EmitInsn_IntArithm(ARGS, kIntArithm_And, kLong, true);
      break;
    }
    case Instruction::OR_LONG_2ADDR: {
      EmitInsn_IntArithm(ARGS, kIntArithm_Or, kLong, true);
      break;
    }
    case Instruction::XOR_LONG_2ADDR: {
      EmitInsn_IntArithm(ARGS, kIntArithm_Xor, kLong, true);
      break;
    }
    case Instruction::SHL_LONG_2ADDR:
      //EmitInsn_IntShiftArithm(ARGS, kIntArithm_Shl, kLong, true);
      break;

    case Instruction::SHR_LONG_2ADDR:
      //EmitInsn_IntShiftArithm(ARGS, kIntArithm_Shr, kLong, true);
      break;

    case Instruction::USHR_LONG_2ADDR:
      //EmitInsn_IntShiftArithm(ARGS, kIntArithm_UShr, kLong, true);
      break;

    case Instruction::ADD_FLOAT_2ADDR: {
      EmitInsn_FPArithm(ARGS, kFPArithm_Add, kFloat, true);
      break;
    }
    case Instruction::SUB_FLOAT_2ADDR: {
      EmitInsn_FPArithm(ARGS, kFPArithm_Sub, kFloat, true);
      break;
    }
    case Instruction::MUL_FLOAT_2ADDR: {
      EmitInsn_FPArithm(ARGS, kFPArithm_Mul, kFloat, true);
      break;
    }
    case Instruction::DIV_FLOAT_2ADDR: {
      EmitInsn_FPArithm(ARGS, kFPArithm_Div, kFloat, true);
      break;
    }
    case Instruction::REM_FLOAT_2ADDR: {
      EmitInsn_FPArithm(ARGS, kFPArithm_Rem, kFloat, true);
      break;
    }
    case Instruction::ADD_DOUBLE_2ADDR: {
      EmitInsn_FPArithm(ARGS, kFPArithm_Add, kDouble, true);
      break;
    }
    case Instruction::SUB_DOUBLE_2ADDR: {
      EmitInsn_FPArithm(ARGS, kFPArithm_Sub, kDouble, true);
      break;
    }
    case Instruction::MUL_DOUBLE_2ADDR: {
      EmitInsn_FPArithm(ARGS, kFPArithm_Mul, kDouble, true);
      break;
    }
    case Instruction::DIV_DOUBLE_2ADDR: {
      EmitInsn_FPArithm(ARGS, kFPArithm_Div, kDouble, true);
      break;
    }
    case Instruction::REM_DOUBLE_2ADDR: {
      EmitInsn_FPArithm(ARGS, kFPArithm_Rem, kDouble, true);
      break;
    }
    case Instruction::ADD_INT_LIT16:
    case Instruction::ADD_INT_LIT8: {
      EmitInsn_IntArithmImmediate(ARGS, kIntArithm_Add);
      break;
    }
    case Instruction::RSUB_INT:
    case Instruction::RSUB_INT_LIT8:
      //EmitInsn_RSubImmediate(ARGS);
      break;

    case Instruction::MUL_INT_LIT16:
    case Instruction::MUL_INT_LIT8: {
      EmitInsn_IntArithmImmediate(ARGS, kIntArithm_Mul);
      break;
    }
    case Instruction::DIV_INT_LIT16:
    case Instruction::DIV_INT_LIT8: {
      EmitInsn_IntArithmImmediate(ARGS, kIntArithm_Div);
      break;
    }
    case Instruction::REM_INT_LIT16:
    case Instruction::REM_INT_LIT8: {
      EmitInsn_IntArithmImmediate(ARGS, kIntArithm_Rem);
      break;
    }
    case Instruction::AND_INT_LIT16:
    case Instruction::AND_INT_LIT8: {
      EmitInsn_IntArithmImmediate(ARGS, kIntArithm_And);
      break;
    }
    case Instruction::OR_INT_LIT16:
    case Instruction::OR_INT_LIT8: {
      EmitInsn_IntArithmImmediate(ARGS, kIntArithm_Or);
      break;
    }
    case Instruction::XOR_INT_LIT16:
    case Instruction::XOR_INT_LIT8: {
      EmitInsn_IntArithmImmediate(ARGS, kIntArithm_Xor);
      break;
    }
    case Instruction::SHL_INT_LIT8:
      //EmitInsn_IntShiftArithmImmediate(ARGS, kIntArithm_Shl);
      break;

    case Instruction::SHR_INT_LIT8:
      //EmitInsn_IntShiftArithmImmediate(ARGS, kIntArithm_Shr);
      break;

    case Instruction::USHR_INT_LIT8:
      //EmitInsn_IntShiftArithmImmediate(ARGS, kIntArithm_UShr);
      break;

    case Instruction::UNUSED_3E:
    case Instruction::UNUSED_3F:
    case Instruction::UNUSED_40:
    case Instruction::UNUSED_41:
    case Instruction::UNUSED_42:
    case Instruction::UNUSED_43:
    case Instruction::UNUSED_73:
    case Instruction::UNUSED_79:
    case Instruction::UNUSED_7A:
    case Instruction::UNUSED_E3:
    case Instruction::UNUSED_E4:
    case Instruction::UNUSED_E5:
    case Instruction::UNUSED_E6:
    case Instruction::UNUSED_E7:
    case Instruction::UNUSED_E8:
    case Instruction::UNUSED_E9:
    case Instruction::UNUSED_EA:
    case Instruction::UNUSED_EB:
    case Instruction::UNUSED_EC:
    case Instruction::UNUSED_ED:
    case Instruction::UNUSED_EE:
    case Instruction::UNUSED_EF:
    case Instruction::UNUSED_F0:
    case Instruction::UNUSED_F1:
    case Instruction::UNUSED_F2:
    case Instruction::UNUSED_F3:
    case Instruction::UNUSED_F4:
    case Instruction::UNUSED_F5:
    case Instruction::UNUSED_F6:
    case Instruction::UNUSED_F7:
    case Instruction::UNUSED_F8:
    case Instruction::UNUSED_F9:
    case Instruction::UNUSED_FA:
    case Instruction::UNUSED_FB:
    case Instruction::UNUSED_FC:
    case Instruction::UNUSED_FD:
    case Instruction::UNUSED_FE:
    case Instruction::UNUSED_FF: {
      LOG(FATAL) << "Dex file contains UNUSED bytecode: " << insn->Opcode();
    }
  }
#undef ARGS

  return true;
}

} // namespace greenland
} // namespace art
