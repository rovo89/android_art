/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include "method_verifier.h"

#include <iostream>

#include "class_linker.h"
#include "compiler.h"
#include "dex_cache.h"
#include "dex_file.h"
#include "dex_instruction.h"
#include "dex_instruction_visitor.h"
#include "gc_map.h"
#include "intern_table.h"
#include "leb128.h"
#include "logging.h"
#include "object_utils.h"
#include "runtime.h"
#include "stringpiece.h"

#if defined(ART_USE_LLVM_COMPILER)
#include "compiler_llvm/backend_types.h"
#include "compiler_llvm/inferred_reg_category_map.h"
using namespace art::compiler_llvm;
#endif

#if defined(ART_USE_GREENLAND_COMPILER)
#include "greenland/backend_types.h"
#include "greenland/inferred_reg_category_map.h"
using namespace art::greenland;
#endif

namespace art {
namespace verifier {

static const bool gDebugVerify = false;

class InsnFlags {
 public:
  InsnFlags() : length_(0), flags_(0) {}

  void SetLengthInCodeUnits(size_t length) {
    CHECK_LT(length, 65536u);
    length_ = length;
  }
  size_t GetLengthInCodeUnits() {
    return length_;
  }
  bool IsOpcode() const {
    return length_ != 0;
  }

  void SetInTry() {
    flags_ |= 1 << kInTry;
  }
  void ClearInTry() {
    flags_ &= ~(1 << kInTry);
  }
  bool IsInTry() const {
    return (flags_ & (1 << kInTry)) != 0;
  }

  void SetBranchTarget() {
    flags_ |= 1 << kBranchTarget;
  }
  void ClearBranchTarget() {
    flags_ &= ~(1 << kBranchTarget);
  }
  bool IsBranchTarget() const {
    return (flags_ & (1 << kBranchTarget)) != 0;
  }

  void SetGcPoint() {
    flags_ |= 1 << kGcPoint;
  }
  void ClearGcPoint() {
    flags_ &= ~(1 << kGcPoint);
  }
  bool IsGcPoint() const {
    return (flags_ & (1 << kGcPoint)) != 0;
  }

  void SetVisited() {
    flags_ |= 1 << kVisited;
  }
  void ClearVisited() {
    flags_ &= ~(1 << kVisited);
  }
  bool IsVisited() const {
    return (flags_ & (1 << kVisited)) != 0;
  }

  void SetChanged() {
    flags_ |= 1 << kChanged;
  }
  void ClearChanged() {
    flags_ &= ~(1 << kChanged);
  }
  bool IsChanged() const {
    return (flags_ & (1 << kChanged)) != 0;
  }

  bool IsVisitedOrChanged() const {
    return IsVisited() || IsChanged();
  }

  std::string Dump() {
    char encoding[6];
    if (!IsOpcode()) {
      strncpy(encoding, "XXXXX", sizeof(encoding));
    } else {
      strncpy(encoding, "-----", sizeof(encoding));
      if (IsInTry())        encoding[kInTry] = 'T';
      if (IsBranchTarget()) encoding[kBranchTarget] = 'B';
      if (IsGcPoint())      encoding[kGcPoint] = 'G';
      if (IsVisited())      encoding[kVisited] = 'V';
      if (IsChanged())      encoding[kChanged] = 'C';
    }
    return std::string(encoding);
  }

 private:
  enum {
    kInTry,
    kBranchTarget,
    kGcPoint,
    kVisited,
    kChanged,
  };

  // Size of instruction in code units
  uint16_t length_;
  uint8_t flags_;
};

void PcToRegisterLineTable::Init(RegisterTrackingMode mode, InsnFlags* flags,
                                 uint32_t insns_size, uint16_t registers_size,
                                 MethodVerifier* verifier) {
  DCHECK_GT(insns_size, 0U);

  for (uint32_t i = 0; i < insns_size; i++) {
    bool interesting = false;
    switch (mode) {
      case kTrackRegsAll:
        interesting = flags[i].IsOpcode();
        break;
      case kTrackRegsGcPoints:
        interesting = flags[i].IsGcPoint() || flags[i].IsBranchTarget();
        break;
      case kTrackRegsBranches:
        interesting = flags[i].IsBranchTarget();
        break;
      default:
        break;
    }
    if (interesting) {
      pc_to_register_line_.Put(i, new RegisterLine(registers_size, verifier));
    }
  }
}

MethodVerifier::FailureKind MethodVerifier::VerifyClass(const Class* klass, std::string& error) {
  if (klass->IsVerified()) {
    return kNoFailure;
  }
  Class* super = klass->GetSuperClass();
  if (super == NULL && StringPiece(ClassHelper(klass).GetDescriptor()) != "Ljava/lang/Object;") {
    error = "Verifier rejected class ";
    error += PrettyDescriptor(klass);
    error += " that has no super class";
    return kHardFailure;
  }
  if (super != NULL && super->IsFinal()) {
    error = "Verifier rejected class ";
    error += PrettyDescriptor(klass);
    error += " that attempts to sub-class final class ";
    error += PrettyDescriptor(super);
    return kHardFailure;
  }
  ClassHelper kh(klass);
  const DexFile& dex_file = kh.GetDexFile();
  uint32_t class_def_idx;
  if (!dex_file.FindClassDefIndex(kh.GetDescriptor(), class_def_idx)) {
    error = "Verifier rejected class ";
    error += PrettyDescriptor(klass);
    error += " that isn't present in dex file ";
    error += dex_file.GetLocation();
    return kHardFailure;
  }
  return VerifyClass(&dex_file, kh.GetDexCache(), klass->GetClassLoader(), class_def_idx, error);
}

MethodVerifier::FailureKind MethodVerifier::VerifyClass(const DexFile* dex_file, DexCache* dex_cache,
    const ClassLoader* class_loader, uint32_t class_def_idx, std::string& error) {
  const DexFile::ClassDef& class_def = dex_file->GetClassDef(class_def_idx);
  const byte* class_data = dex_file->GetClassData(class_def);
  if (class_data == NULL) {
    // empty class, probably a marker interface
    return kNoFailure;
  }
  ClassDataItemIterator it(*dex_file, class_data);
  while (it.HasNextStaticField() || it.HasNextInstanceField()) {
    it.Next();
  }
  size_t error_count = 0;
  bool hard_fail = false;
  ClassLinker* linker = Runtime::Current()->GetClassLinker();
  while (it.HasNextDirectMethod()) {
    uint32_t method_idx = it.GetMemberIndex();
    Method* method = linker->ResolveMethod(*dex_file, method_idx, dex_cache, class_loader, true);
    if (method == NULL) {
      DCHECK(Thread::Current()->IsExceptionPending());
      // We couldn't resolve the method, but continue regardless.
      Thread::Current()->ClearException();
    }
    MethodVerifier::FailureKind result = VerifyMethod(method_idx, dex_file, dex_cache, class_loader,
        class_def_idx, it.GetMethodCodeItem(), method, it.GetMemberAccessFlags());
    if (result != kNoFailure) {
      if (result == kHardFailure) {
        hard_fail = true;
        if (error_count > 0) {
          error += "\n";
        }
        error = "Verifier rejected class ";
        error += PrettyDescriptor(dex_file->GetClassDescriptor(class_def));
        error += " due to bad method ";
        error += PrettyMethod(method_idx, *dex_file);
      }
      ++error_count;
    }
    it.Next();
  }
  while (it.HasNextVirtualMethod()) {
    uint32_t method_idx = it.GetMemberIndex();
    Method* method = linker->ResolveMethod(*dex_file, method_idx, dex_cache, class_loader, false);
    if (method == NULL) {
      DCHECK(Thread::Current()->IsExceptionPending());
      // We couldn't resolve the method, but continue regardless.
      Thread::Current()->ClearException();
    }
    MethodVerifier::FailureKind result = VerifyMethod(method_idx, dex_file, dex_cache, class_loader,
        class_def_idx, it.GetMethodCodeItem(), method, it.GetMemberAccessFlags());
    if (result != kNoFailure) {
      if (result == kHardFailure) {
        hard_fail = true;
        if (error_count > 0) {
          error += "\n";
        }
        error = "Verifier rejected class ";
        error += PrettyDescriptor(dex_file->GetClassDescriptor(class_def));
        error += " due to bad method ";
        error += PrettyMethod(method_idx, *dex_file);
      }
      ++error_count;
    }
    it.Next();
  }
  if (error_count == 0) {
    return kNoFailure;
  } else {
    return hard_fail ? kHardFailure : kSoftFailure;
  }
}

MethodVerifier::FailureKind MethodVerifier::VerifyMethod(uint32_t method_idx, const DexFile* dex_file,
    DexCache* dex_cache, const ClassLoader* class_loader, uint32_t class_def_idx,
    const DexFile::CodeItem* code_item, Method* method, uint32_t method_access_flags) {
  MethodVerifier verifier(dex_file, dex_cache, class_loader, class_def_idx, code_item, method_idx,
                          method, method_access_flags);
  if (verifier.Verify()) {
    // Verification completed, however failures may be pending that didn't cause the verification
    // to hard fail.
    CHECK(!verifier.have_pending_hard_failure_);
    if (verifier.failures_.size() != 0) {
      verifier.DumpFailures(LOG(INFO) << "Soft verification failures in "
                                      << PrettyMethod(method_idx, *dex_file) << "\n");
      return kSoftFailure;
    }
  } else {
    // Bad method data.
    CHECK_NE(verifier.failures_.size(), 0U);
    CHECK(verifier.have_pending_hard_failure_);
    verifier.DumpFailures(LOG(INFO) << "Verification error in "
                                    << PrettyMethod(method_idx, *dex_file) << "\n");
    if (gDebugVerify) {
      std::cout << "\n" << verifier.info_messages_.str();
      verifier.Dump(std::cout);
    }
    return kHardFailure;
  }
  return kNoFailure;
}

void MethodVerifier::VerifyMethodAndDump(Method* method) {
  CHECK(method != NULL);
  MethodHelper mh(method);
  MethodVerifier verifier(&mh.GetDexFile(), mh.GetDexCache(), mh.GetClassLoader(),
                          mh.GetClassDefIndex(), mh.GetCodeItem(), method->GetDexMethodIndex(),
                          method, method->GetAccessFlags());
  verifier.Verify();
  verifier.DumpFailures(LOG(INFO) << "Dump of method " << PrettyMethod(method) << "\n")
      << verifier.info_messages_.str() << Dumpable<MethodVerifier>(verifier);
}

MethodVerifier::MethodVerifier(const DexFile* dex_file, DexCache* dex_cache,
    const ClassLoader* class_loader, uint32_t class_def_idx, const DexFile::CodeItem* code_item,
    uint32_t method_idx, Method* method, uint32_t method_access_flags)
    : work_insn_idx_(-1),
      method_idx_(method_idx),
      foo_method_(method),
      method_access_flags_(method_access_flags),
      dex_file_(dex_file),
      dex_cache_(dex_cache),
      class_loader_(class_loader),
      class_def_idx_(class_def_idx),
      code_item_(code_item),
      have_pending_hard_failure_(false),
      have_pending_rewrite_failure_(false),
      new_instance_count_(0),
      monitor_enter_count_(0) {
}

bool MethodVerifier::Verify() {
  // If there aren't any instructions, make sure that's expected, then exit successfully.
  if (code_item_ == NULL) {
    if ((method_access_flags_ & (kAccNative | kAccAbstract)) == 0) {
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "zero-length code in concrete non-native method";
      return false;
    } else {
      return true;
    }
  }
  // Sanity-check the register counts. ins + locals = registers, so make sure that ins <= registers.
  if (code_item_->ins_size_ > code_item_->registers_size_) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "bad register counts (ins=" << code_item_->ins_size_
                                      << " regs=" << code_item_->registers_size_;
    return false;
  }
  // Allocate and initialize an array to hold instruction data.
  insn_flags_.reset(new InsnFlags[code_item_->insns_size_in_code_units_]());
  // Run through the instructions and see if the width checks out.
  bool result = ComputeWidthsAndCountOps();
  // Flag instructions guarded by a "try" block and check exception handlers.
  result = result && ScanTryCatchBlocks();
  // Perform static instruction verification.
  result = result && VerifyInstructions();
  // Perform code-flow analysis and return.
  return result && VerifyCodeFlow();
}

std::ostream& MethodVerifier::Fail(VerifyError error) {
  switch (error) {
    case VERIFY_ERROR_NO_CLASS:
    case VERIFY_ERROR_NO_FIELD:
    case VERIFY_ERROR_NO_METHOD:
    case VERIFY_ERROR_ACCESS_CLASS:
    case VERIFY_ERROR_ACCESS_FIELD:
    case VERIFY_ERROR_ACCESS_METHOD:
      if (Runtime::Current()->IsCompiler()) {
        // If we're optimistically running verification at compile time, turn NO_xxx and ACCESS_xxx
        // errors into soft verification errors so that we re-verify at runtime. We may fail to find
        // or to agree on access because of not yet available class loaders, or class loaders that
        // will differ at runtime.
        error = VERIFY_ERROR_BAD_CLASS_SOFT;
      } else {
        have_pending_rewrite_failure_ = true;
      }
      break;
      // Errors that are bad at both compile and runtime, but don't cause rejection of the class.
    case VERIFY_ERROR_CLASS_CHANGE:
    case VERIFY_ERROR_INSTANTIATION:
      have_pending_rewrite_failure_ = true;
      break;
      // Indication that verification should be retried at runtime.
    case VERIFY_ERROR_BAD_CLASS_SOFT:
      if (!Runtime::Current()->IsCompiler()) {
        // It is runtime so hard fail.
        have_pending_hard_failure_ = true;
      }
      break;
      // Hard verification failures at compile time will still fail at runtime, so the class is
      // marked as rejected to prevent it from being compiled.
    case VERIFY_ERROR_BAD_CLASS_HARD: {
      if (Runtime::Current()->IsCompiler()) {
        Compiler::ClassReference ref(dex_file_, class_def_idx_);
        AddRejectedClass(ref);
      }
      have_pending_hard_failure_ = true;
      break;
    }
  }
  failures_.push_back(error);
  std::string location(StringPrintf("%s: [0x%X]", PrettyMethod(method_idx_, *dex_file_).c_str(),
                                    work_insn_idx_));
  std::ostringstream* failure_message = new std::ostringstream(location);
  failure_messages_.push_back(failure_message);
  return *failure_message;
}

void MethodVerifier::PrependToLastFailMessage(std::string prepend) {
  size_t failure_num = failure_messages_.size();
  DCHECK_NE(failure_num, 0U);
  std::ostringstream* last_fail_message = failure_messages_[failure_num - 1];
  prepend += last_fail_message->str();
  failure_messages_[failure_num - 1] = new std::ostringstream(prepend);
  delete last_fail_message;
}

void MethodVerifier::AppendToLastFailMessage(std::string append) {
  size_t failure_num = failure_messages_.size();
  DCHECK_NE(failure_num, 0U);
  std::ostringstream* last_fail_message = failure_messages_[failure_num - 1];
  (*last_fail_message) << append;
}

bool MethodVerifier::ComputeWidthsAndCountOps() {
  const uint16_t* insns = code_item_->insns_;
  size_t insns_size = code_item_->insns_size_in_code_units_;
  const Instruction* inst = Instruction::At(insns);
  size_t new_instance_count = 0;
  size_t monitor_enter_count = 0;
  size_t dex_pc = 0;

  while (dex_pc < insns_size) {
    Instruction::Code opcode = inst->Opcode();
    if (opcode == Instruction::NEW_INSTANCE) {
      new_instance_count++;
    } else if (opcode == Instruction::MONITOR_ENTER) {
      monitor_enter_count++;
    }
    size_t inst_size = inst->SizeInCodeUnits();
    insn_flags_[dex_pc].SetLengthInCodeUnits(inst_size);
    dex_pc += inst_size;
    inst = inst->Next();
  }

  if (dex_pc != insns_size) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "code did not end where expected ("
                                      << dex_pc << " vs. " << insns_size << ")";
    return false;
  }

  new_instance_count_ = new_instance_count;
  monitor_enter_count_ = monitor_enter_count;
  return true;
}

bool MethodVerifier::ScanTryCatchBlocks() {
  uint32_t tries_size = code_item_->tries_size_;
  if (tries_size == 0) {
    return true;
  }
  uint32_t insns_size = code_item_->insns_size_in_code_units_;
  const DexFile::TryItem* tries = DexFile::GetTryItems(*code_item_, 0);

  for (uint32_t idx = 0; idx < tries_size; idx++) {
    const DexFile::TryItem* try_item = &tries[idx];
    uint32_t start = try_item->start_addr_;
    uint32_t end = start + try_item->insn_count_;
    if ((start >= end) || (start >= insns_size) || (end > insns_size)) {
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "bad exception entry: startAddr=" << start
                                        << " endAddr=" << end << " (size=" << insns_size << ")";
      return false;
    }
    if (!insn_flags_[start].IsOpcode()) {
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "'try' block starts inside an instruction (" << start << ")";
      return false;
    }
    for (uint32_t dex_pc = start; dex_pc < end;
        dex_pc += insn_flags_[dex_pc].GetLengthInCodeUnits()) {
      insn_flags_[dex_pc].SetInTry();
    }
  }
  // Iterate over each of the handlers to verify target addresses.
  const byte* handlers_ptr = DexFile::GetCatchHandlerData(*code_item_, 0);
  uint32_t handlers_size = DecodeUnsignedLeb128(&handlers_ptr);
  ClassLinker* linker = Runtime::Current()->GetClassLinker();
  for (uint32_t idx = 0; idx < handlers_size; idx++) {
    CatchHandlerIterator iterator(handlers_ptr);
    for (; iterator.HasNext(); iterator.Next()) {
      uint32_t dex_pc= iterator.GetHandlerAddress();
      if (!insn_flags_[dex_pc].IsOpcode()) {
        Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "exception handler starts at bad address (" << dex_pc << ")";
        return false;
      }
      const Instruction* inst = Instruction::At(code_item_->insns_ + dex_pc);
      if (inst->Opcode() != Instruction::MOVE_EXCEPTION) {
        Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "exception handler doesn't start with move-exception ("
                                          << dex_pc << ")";
        return false;
      }
      insn_flags_[dex_pc].SetBranchTarget();
      // Ensure exception types are resolved so that they don't need resolution to be delivered,
      // unresolved exception types will be ignored by exception delivery
      if (iterator.GetHandlerTypeIndex() != DexFile::kDexNoIndex16) {
        Class* exception_type = linker->ResolveType(*dex_file_, iterator.GetHandlerTypeIndex(),
                                                    dex_cache_, class_loader_);
        if (exception_type == NULL) {
          DCHECK(Thread::Current()->IsExceptionPending());
          Thread::Current()->ClearException();
        }
      }
    }
    handlers_ptr = iterator.EndDataPointer();
  }
  return true;
}

bool MethodVerifier::VerifyInstructions() {
  const Instruction* inst = Instruction::At(code_item_->insns_);

  /* Flag the start of the method as a branch target. */
  insn_flags_[0].SetBranchTarget();

  uint32_t insns_size = code_item_->insns_size_in_code_units_;
  for (uint32_t dex_pc = 0; dex_pc < insns_size;) {
    if (!VerifyInstruction(inst, dex_pc)) {
      DCHECK_NE(failures_.size(), 0U);
      return false;
    }
    /* Flag instructions that are garbage collection points */
    if (inst->IsBranch() || inst->IsSwitch() || inst->IsThrow() || inst->IsReturn()) {
      insn_flags_[dex_pc].SetGcPoint();
    }
    dex_pc += inst->SizeInCodeUnits();
    inst = inst->Next();
  }
  return true;
}

bool MethodVerifier::VerifyInstruction(const Instruction* inst, uint32_t code_offset) {
  DecodedInstruction dec_insn(inst);
  bool result = true;
  switch (inst->GetVerifyTypeArgumentA()) {
    case Instruction::kVerifyRegA:
      result = result && CheckRegisterIndex(dec_insn.vA);
      break;
    case Instruction::kVerifyRegAWide:
      result = result && CheckWideRegisterIndex(dec_insn.vA);
      break;
  }
  switch (inst->GetVerifyTypeArgumentB()) {
    case Instruction::kVerifyRegB:
      result = result && CheckRegisterIndex(dec_insn.vB);
      break;
    case Instruction::kVerifyRegBField:
      result = result && CheckFieldIndex(dec_insn.vB);
      break;
    case Instruction::kVerifyRegBMethod:
      result = result && CheckMethodIndex(dec_insn.vB);
      break;
    case Instruction::kVerifyRegBNewInstance:
      result = result && CheckNewInstance(dec_insn.vB);
      break;
    case Instruction::kVerifyRegBString:
      result = result && CheckStringIndex(dec_insn.vB);
      break;
    case Instruction::kVerifyRegBType:
      result = result && CheckTypeIndex(dec_insn.vB);
      break;
    case Instruction::kVerifyRegBWide:
      result = result && CheckWideRegisterIndex(dec_insn.vB);
      break;
  }
  switch (inst->GetVerifyTypeArgumentC()) {
    case Instruction::kVerifyRegC:
      result = result && CheckRegisterIndex(dec_insn.vC);
      break;
    case Instruction::kVerifyRegCField:
      result = result && CheckFieldIndex(dec_insn.vC);
      break;
    case Instruction::kVerifyRegCNewArray:
      result = result && CheckNewArray(dec_insn.vC);
      break;
    case Instruction::kVerifyRegCType:
      result = result && CheckTypeIndex(dec_insn.vC);
      break;
    case Instruction::kVerifyRegCWide:
      result = result && CheckWideRegisterIndex(dec_insn.vC);
      break;
  }
  switch (inst->GetVerifyExtraFlags()) {
    case Instruction::kVerifyArrayData:
      result = result && CheckArrayData(code_offset);
      break;
    case Instruction::kVerifyBranchTarget:
      result = result && CheckBranchTarget(code_offset);
      break;
    case Instruction::kVerifySwitchTargets:
      result = result && CheckSwitchTargets(code_offset);
      break;
    case Instruction::kVerifyVarArg:
      result = result && CheckVarArgRegs(dec_insn.vA, dec_insn.arg);
      break;
    case Instruction::kVerifyVarArgRange:
      result = result && CheckVarArgRangeRegs(dec_insn.vA, dec_insn.vC);
      break;
    case Instruction::kVerifyError:
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "unexpected opcode " << inst->Name();
      result = false;
      break;
  }
  return result;
}

bool MethodVerifier::CheckRegisterIndex(uint32_t idx) {
  if (idx >= code_item_->registers_size_) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "register index out of range (" << idx << " >= "
                                      << code_item_->registers_size_ << ")";
    return false;
  }
  return true;
}

bool MethodVerifier::CheckWideRegisterIndex(uint32_t idx) {
  if (idx + 1 >= code_item_->registers_size_) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "wide register index out of range (" << idx
                                      << "+1 >= " << code_item_->registers_size_ << ")";
    return false;
  }
  return true;
}

bool MethodVerifier::CheckFieldIndex(uint32_t idx) {
  if (idx >= dex_file_->GetHeader().field_ids_size_) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "bad field index " << idx << " (max "
                                      << dex_file_->GetHeader().field_ids_size_ << ")";
    return false;
  }
  return true;
}

bool MethodVerifier::CheckMethodIndex(uint32_t idx) {
  if (idx >= dex_file_->GetHeader().method_ids_size_) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "bad method index " << idx << " (max "
                                      << dex_file_->GetHeader().method_ids_size_ << ")";
    return false;
  }
  return true;
}

bool MethodVerifier::CheckNewInstance(uint32_t idx) {
  if (idx >= dex_file_->GetHeader().type_ids_size_) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "bad type index " << idx << " (max "
                                      << dex_file_->GetHeader().type_ids_size_ << ")";
    return false;
  }
  // We don't need the actual class, just a pointer to the class name.
  const char* descriptor = dex_file_->StringByTypeIdx(idx);
  if (descriptor[0] != 'L') {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "can't call new-instance on type '" << descriptor << "'";
    return false;
  }
  return true;
}

bool MethodVerifier::CheckStringIndex(uint32_t idx) {
  if (idx >= dex_file_->GetHeader().string_ids_size_) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "bad string index " << idx << " (max "
                                      << dex_file_->GetHeader().string_ids_size_ << ")";
    return false;
  }
  return true;
}

bool MethodVerifier::CheckTypeIndex(uint32_t idx) {
  if (idx >= dex_file_->GetHeader().type_ids_size_) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "bad type index " << idx << " (max "
                                      << dex_file_->GetHeader().type_ids_size_ << ")";
    return false;
  }
  return true;
}

bool MethodVerifier::CheckNewArray(uint32_t idx) {
  if (idx >= dex_file_->GetHeader().type_ids_size_) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "bad type index " << idx << " (max "
                                      << dex_file_->GetHeader().type_ids_size_ << ")";
    return false;
  }
  int bracket_count = 0;
  const char* descriptor = dex_file_->StringByTypeIdx(idx);
  const char* cp = descriptor;
  while (*cp++ == '[') {
    bracket_count++;
  }
  if (bracket_count == 0) {
    /* The given class must be an array type. */
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "can't new-array class '" << descriptor << "' (not an array)";
    return false;
  } else if (bracket_count > 255) {
    /* It is illegal to create an array of more than 255 dimensions. */
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "can't new-array class '" << descriptor << "' (exceeds limit)";
    return false;
  }
  return true;
}

bool MethodVerifier::CheckArrayData(uint32_t cur_offset) {
  const uint32_t insn_count = code_item_->insns_size_in_code_units_;
  const uint16_t* insns = code_item_->insns_ + cur_offset;
  const uint16_t* array_data;
  int32_t array_data_offset;

  DCHECK_LT(cur_offset, insn_count);
  /* make sure the start of the array data table is in range */
  array_data_offset = insns[1] | (((int32_t) insns[2]) << 16);
  if ((int32_t) cur_offset + array_data_offset < 0 ||
      cur_offset + array_data_offset + 2 >= insn_count) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "invalid array data start: at " << cur_offset
                                      << ", data offset " << array_data_offset << ", count " << insn_count;
    return false;
  }
  /* offset to array data table is a relative branch-style offset */
  array_data = insns + array_data_offset;
  /* make sure the table is 32-bit aligned */
  if ((((uint32_t) array_data) & 0x03) != 0) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "unaligned array data table: at " << cur_offset
                                      << ", data offset " << array_data_offset;
    return false;
  }
  uint32_t value_width = array_data[1];
  uint32_t value_count = *reinterpret_cast<const uint32_t*>(&array_data[2]);
  uint32_t table_size = 4 + (value_width * value_count + 1) / 2;
  /* make sure the end of the switch is in range */
  if (cur_offset + array_data_offset + table_size > insn_count) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "invalid array data end: at " << cur_offset
                                      << ", data offset " << array_data_offset << ", end "
                                      << cur_offset + array_data_offset + table_size
                                      << ", count " << insn_count;
    return false;
  }
  return true;
}

bool MethodVerifier::CheckBranchTarget(uint32_t cur_offset) {
  int32_t offset;
  bool isConditional, selfOkay;
  if (!GetBranchOffset(cur_offset, &offset, &isConditional, &selfOkay)) {
    return false;
  }
  if (!selfOkay && offset == 0) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "branch offset of zero not allowed at" << reinterpret_cast<void*>(cur_offset);
    return false;
  }
  // Check for 32-bit overflow. This isn't strictly necessary if we can depend on the runtime
  // to have identical "wrap-around" behavior, but it's unwise to depend on that.
  if (((int64_t) cur_offset + (int64_t) offset) != (int64_t) (cur_offset + offset)) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "branch target overflow " << reinterpret_cast<void*>(cur_offset) << " +" << offset;
    return false;
  }
  const uint32_t insn_count = code_item_->insns_size_in_code_units_;
  int32_t abs_offset = cur_offset + offset;
  if (abs_offset < 0 || (uint32_t) abs_offset >= insn_count || !insn_flags_[abs_offset].IsOpcode()) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "invalid branch target " << offset << " (-> "
                                      << reinterpret_cast<void*>(abs_offset) << ") at "
                                      << reinterpret_cast<void*>(cur_offset);
    return false;
  }
  insn_flags_[abs_offset].SetBranchTarget();
  return true;
}

bool MethodVerifier::GetBranchOffset(uint32_t cur_offset, int32_t* pOffset, bool* pConditional,
                                  bool* selfOkay) {
  const uint16_t* insns = code_item_->insns_ + cur_offset;
  *pConditional = false;
  *selfOkay = false;
  switch (*insns & 0xff) {
    case Instruction::GOTO:
      *pOffset = ((int16_t) *insns) >> 8;
      break;
    case Instruction::GOTO_32:
      *pOffset = insns[1] | (((uint32_t) insns[2]) << 16);
      *selfOkay = true;
      break;
    case Instruction::GOTO_16:
      *pOffset = (int16_t) insns[1];
      break;
    case Instruction::IF_EQ:
    case Instruction::IF_NE:
    case Instruction::IF_LT:
    case Instruction::IF_GE:
    case Instruction::IF_GT:
    case Instruction::IF_LE:
    case Instruction::IF_EQZ:
    case Instruction::IF_NEZ:
    case Instruction::IF_LTZ:
    case Instruction::IF_GEZ:
    case Instruction::IF_GTZ:
    case Instruction::IF_LEZ:
      *pOffset = (int16_t) insns[1];
      *pConditional = true;
      break;
    default:
      return false;
      break;
  }
  return true;
}

bool MethodVerifier::CheckSwitchTargets(uint32_t cur_offset) {
  const uint32_t insn_count = code_item_->insns_size_in_code_units_;
  DCHECK_LT(cur_offset, insn_count);
  const uint16_t* insns = code_item_->insns_ + cur_offset;
  /* make sure the start of the switch is in range */
  int32_t switch_offset = insns[1] | ((int32_t) insns[2]) << 16;
  if ((int32_t) cur_offset + switch_offset < 0 || cur_offset + switch_offset + 2 >= insn_count) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "invalid switch start: at " << cur_offset
                                      << ", switch offset " << switch_offset << ", count " << insn_count;
    return false;
  }
  /* offset to switch table is a relative branch-style offset */
  const uint16_t* switch_insns = insns + switch_offset;
  /* make sure the table is 32-bit aligned */
  if ((((uint32_t) switch_insns) & 0x03) != 0) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "unaligned switch table: at " << cur_offset
                                      << ", switch offset " << switch_offset;
    return false;
  }
  uint32_t switch_count = switch_insns[1];
  int32_t keys_offset, targets_offset;
  uint16_t expected_signature;
  if ((*insns & 0xff) == Instruction::PACKED_SWITCH) {
    /* 0=sig, 1=count, 2/3=firstKey */
    targets_offset = 4;
    keys_offset = -1;
    expected_signature = Instruction::kPackedSwitchSignature;
  } else {
    /* 0=sig, 1=count, 2..count*2 = keys */
    keys_offset = 2;
    targets_offset = 2 + 2 * switch_count;
    expected_signature = Instruction::kSparseSwitchSignature;
  }
  uint32_t table_size = targets_offset + switch_count * 2;
  if (switch_insns[0] != expected_signature) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << StringPrintf("wrong signature for switch table (%x, wanted %x)",
                                                      switch_insns[0], expected_signature);
    return false;
  }
  /* make sure the end of the switch is in range */
  if (cur_offset + switch_offset + table_size > (uint32_t) insn_count) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "invalid switch end: at " << cur_offset << ", switch offset "
                                      << switch_offset << ", end "
                                      << (cur_offset + switch_offset + table_size)
                                      << ", count " << insn_count;
    return false;
  }
  /* for a sparse switch, verify the keys are in ascending order */
  if (keys_offset > 0 && switch_count > 1) {
    int32_t last_key = switch_insns[keys_offset] | (switch_insns[keys_offset + 1] << 16);
    for (uint32_t targ = 1; targ < switch_count; targ++) {
      int32_t key = (int32_t) switch_insns[keys_offset + targ * 2] |
                    (int32_t) (switch_insns[keys_offset + targ * 2 + 1] << 16);
      if (key <= last_key) {
        Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "invalid packed switch: last key=" << last_key
                                          << ", this=" << key;
        return false;
      }
      last_key = key;
    }
  }
  /* verify each switch target */
  for (uint32_t targ = 0; targ < switch_count; targ++) {
    int32_t offset = (int32_t) switch_insns[targets_offset + targ * 2] |
                     (int32_t) (switch_insns[targets_offset + targ * 2 + 1] << 16);
    int32_t abs_offset = cur_offset + offset;
    if (abs_offset < 0 || abs_offset >= (int32_t) insn_count || !insn_flags_[abs_offset].IsOpcode()) {
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "invalid switch target " << offset << " (-> "
                                        << reinterpret_cast<void*>(abs_offset) << ") at "
                                        << reinterpret_cast<void*>(cur_offset) << "[" << targ << "]";
      return false;
    }
    insn_flags_[abs_offset].SetBranchTarget();
  }
  return true;
}

bool MethodVerifier::CheckVarArgRegs(uint32_t vA, uint32_t arg[]) {
  if (vA > 5) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "invalid arg count (" << vA << ") in non-range invoke)";
    return false;
  }
  uint16_t registers_size = code_item_->registers_size_;
  for (uint32_t idx = 0; idx < vA; idx++) {
    if (arg[idx] >= registers_size) {
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "invalid reg index (" << arg[idx]
                                        << ") in non-range invoke (>= " << registers_size << ")";
      return false;
    }
  }

  return true;
}

bool MethodVerifier::CheckVarArgRangeRegs(uint32_t vA, uint32_t vC) {
  uint16_t registers_size = code_item_->registers_size_;
  // vA/vC are unsigned 8-bit/16-bit quantities for /range instructions, so there's no risk of
  // integer overflow when adding them here.
  if (vA + vC > registers_size) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "invalid reg index " << vA << "+" << vC << " in range invoke (> "
                                      << registers_size << ")";
    return false;
  }
  return true;
}

const std::vector<uint8_t>* CreateLengthPrefixedGcMap(const std::vector<uint8_t>& gc_map) {
  std::vector<uint8_t>* length_prefixed_gc_map = new std::vector<uint8_t>;
  length_prefixed_gc_map->push_back((gc_map.size() & 0xff000000) >> 24);
  length_prefixed_gc_map->push_back((gc_map.size() & 0x00ff0000) >> 16);
  length_prefixed_gc_map->push_back((gc_map.size() & 0x0000ff00) >> 8);
  length_prefixed_gc_map->push_back((gc_map.size() & 0x000000ff) >> 0);
  length_prefixed_gc_map->insert(length_prefixed_gc_map->end(),
                                 gc_map.begin(),
                                 gc_map.end());
  DCHECK_EQ(gc_map.size() + 4, length_prefixed_gc_map->size());
  DCHECK_EQ(gc_map.size(),
            static_cast<size_t>((length_prefixed_gc_map->at(0) << 24) |
                                (length_prefixed_gc_map->at(1) << 16) |
                                (length_prefixed_gc_map->at(2) << 8) |
                                (length_prefixed_gc_map->at(3) << 0)));
  return length_prefixed_gc_map;
}

bool MethodVerifier::VerifyCodeFlow() {
  uint16_t registers_size = code_item_->registers_size_;
  uint32_t insns_size = code_item_->insns_size_in_code_units_;

  if (registers_size * insns_size > 4*1024*1024) {
    LOG(WARNING) << "warning: method is huge (regs=" << registers_size
                 << " insns_size=" << insns_size << ")";
  }
  /* Create and initialize table holding register status */
  reg_table_.Init(kTrackRegsGcPoints, insn_flags_.get(), insns_size, registers_size, this);

  work_line_.reset(new RegisterLine(registers_size, this));
  saved_line_.reset(new RegisterLine(registers_size, this));

  /* Initialize register types of method arguments. */
  if (!SetTypesFromSignature()) {
    DCHECK_NE(failures_.size(), 0U);
    std::string prepend("Bad signature in ");
    prepend += PrettyMethod(method_idx_, *dex_file_);
    PrependToLastFailMessage(prepend);
    return false;
  }
  /* Perform code flow verification. */
  if (!CodeFlowVerifyMethod()) {
    DCHECK_NE(failures_.size(), 0U);
    return false;
  }

  Compiler::MethodReference ref(dex_file_, method_idx_);

#if !defined(ART_USE_LLVM_COMPILER) && !defined(ART_USE_GREENLAND_COMPILER)

  /* Generate a register map and add it to the method. */
  UniquePtr<const std::vector<uint8_t> > map(GenerateGcMap());
  if (map.get() == NULL) {
    DCHECK_NE(failures_.size(), 0U);
    return false;  // Not a real failure, but a failure to encode
  }
#ifndef NDEBUG
  VerifyGcMap(*map);
#endif
  const std::vector<uint8_t>* gc_map = CreateLengthPrefixedGcMap(*(map.get()));
  verifier::MethodVerifier::SetGcMap(ref, *gc_map);

  if (foo_method_ != NULL) {
    foo_method_->SetGcMap(&gc_map->at(0));
  }

#else  // defined(ART_USE_LLVM_COMPILER) || defined(ART_USE_GREENLAND_COMPILER)
  /* Generate Inferred Register Category for LLVM-based Code Generator */
  const InferredRegCategoryMap* table = GenerateInferredRegCategoryMap();
  verifier::MethodVerifier::SetInferredRegCategoryMap(ref, *table);

#endif

  return true;
}

std::ostream& MethodVerifier::DumpFailures(std::ostream& os) {
  DCHECK_EQ(failures_.size(), failure_messages_.size());
  for (size_t i = 0; i < failures_.size(); ++i) {
    os << failure_messages_[i]->str() << "\n";
  }
  return os;
}

extern "C" void MethodVerifierGdbDump(MethodVerifier* v) {
  v->Dump(std::cerr);
}

void MethodVerifier::Dump(std::ostream& os) {
  if (code_item_ == NULL) {
    os << "Native method\n";
    return;
  }
  DCHECK(code_item_ != NULL);
  const Instruction* inst = Instruction::At(code_item_->insns_);
  for (size_t dex_pc = 0; dex_pc < code_item_->insns_size_in_code_units_;
      dex_pc += insn_flags_[dex_pc].GetLengthInCodeUnits()) {
    os << StringPrintf("0x%04zx", dex_pc) << ": " << insn_flags_[dex_pc].Dump()
       << " " << inst->DumpHex(5) << " " << inst->DumpString(dex_file_) << "\n";
    RegisterLine* reg_line = reg_table_.GetLine(dex_pc);
    if (reg_line != NULL) {
      os << reg_line->Dump() << "\n";
    }
    inst = inst->Next();
  }
}

static bool IsPrimitiveDescriptor(char descriptor) {
  switch (descriptor) {
    case 'I':
    case 'C':
    case 'S':
    case 'B':
    case 'Z':
    case 'F':
    case 'D':
    case 'J':
      return true;
    default:
      return false;
  }
}

bool MethodVerifier::SetTypesFromSignature() {
  RegisterLine* reg_line = reg_table_.GetLine(0);
  int arg_start = code_item_->registers_size_ - code_item_->ins_size_;
  size_t expected_args = code_item_->ins_size_;   /* long/double count as two */

  DCHECK_GE(arg_start, 0);      /* should have been verified earlier */
  //Include the "this" pointer.
  size_t cur_arg = 0;
  if (!IsStatic()) {
    // If this is a constructor for a class other than java.lang.Object, mark the first ("this")
    // argument as uninitialized. This restricts field access until the superclass constructor is
    // called.
    const RegType& declaring_class = GetDeclaringClass();
    if (IsConstructor() && !declaring_class.IsJavaLangObject()) {
      reg_line->SetRegisterType(arg_start + cur_arg,
                                reg_types_.UninitializedThisArgument(declaring_class));
    } else {
      reg_line->SetRegisterType(arg_start + cur_arg, declaring_class);
    }
    cur_arg++;
  }

  const DexFile::ProtoId& proto_id =
      dex_file_->GetMethodPrototype(dex_file_->GetMethodId(method_idx_));
  DexFileParameterIterator iterator(*dex_file_, proto_id);

  for (; iterator.HasNext(); iterator.Next()) {
    const char* descriptor = iterator.GetDescriptor();
    if (descriptor == NULL) {
      LOG(FATAL) << "Null descriptor";
    }
    if (cur_arg >= expected_args) {
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "expected " << expected_args
                                        << " args, found more (" << descriptor << ")";
      return false;
    }
    switch (descriptor[0]) {
      case 'L':
      case '[':
        // We assume that reference arguments are initialized. The only way it could be otherwise
        // (assuming the caller was verified) is if the current method is <init>, but in that case
        // it's effectively considered initialized the instant we reach here (in the sense that we
        // can return without doing anything or call virtual methods).
        {
          const RegType& reg_type = reg_types_.FromDescriptor(class_loader_, descriptor);
          reg_line->SetRegisterType(arg_start + cur_arg, reg_type);
        }
        break;
      case 'Z':
        reg_line->SetRegisterType(arg_start + cur_arg, reg_types_.Boolean());
        break;
      case 'C':
        reg_line->SetRegisterType(arg_start + cur_arg, reg_types_.Char());
        break;
      case 'B':
        reg_line->SetRegisterType(arg_start + cur_arg, reg_types_.Byte());
        break;
      case 'I':
        reg_line->SetRegisterType(arg_start + cur_arg, reg_types_.Integer());
        break;
      case 'S':
        reg_line->SetRegisterType(arg_start + cur_arg, reg_types_.Short());
        break;
      case 'F':
        reg_line->SetRegisterType(arg_start + cur_arg, reg_types_.Float());
        break;
      case 'J':
      case 'D': {
        const RegType& low_half = descriptor[0] == 'J' ? reg_types_.Long() : reg_types_.Double();
        reg_line->SetRegisterType(arg_start + cur_arg, low_half);  // implicitly sets high-register
        cur_arg++;
        break;
      }
      default:
        Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "unexpected signature type char '" << descriptor << "'";
        return false;
    }
    cur_arg++;
  }
  if (cur_arg != expected_args) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "expected " << expected_args << " arguments, found " << cur_arg;
    return false;
  }
  const char* descriptor = dex_file_->GetReturnTypeDescriptor(proto_id);
  // Validate return type. We don't do the type lookup; just want to make sure that it has the right
  // format. Only major difference from the method argument format is that 'V' is supported.
  bool result;
  if (IsPrimitiveDescriptor(descriptor[0]) || descriptor[0] == 'V') {
    result = descriptor[1] == '\0';
  } else if (descriptor[0] == '[') { // single/multi-dimensional array of object/primitive
    size_t i = 0;
    do {
      i++;
    } while (descriptor[i] == '[');  // process leading [
    if (descriptor[i] == 'L') {  // object array
      do {
        i++;  // find closing ;
      } while (descriptor[i] != ';' && descriptor[i] != '\0');
      result = descriptor[i] == ';';
    } else {  // primitive array
      result = IsPrimitiveDescriptor(descriptor[i]) && descriptor[i + 1] == '\0';
    }
  } else if (descriptor[0] == 'L') {
    // could be more thorough here, but shouldn't be required
    size_t i = 0;
    do {
      i++;
    } while (descriptor[i] != ';' && descriptor[i] != '\0');
    result = descriptor[i] == ';';
  } else {
    result = false;
  }
  if (!result) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "unexpected char in return type descriptor '"
                                      << descriptor << "'";
  }
  return result;
}

bool MethodVerifier::CodeFlowVerifyMethod() {
  const uint16_t* insns = code_item_->insns_;
  const uint32_t insns_size = code_item_->insns_size_in_code_units_;

  /* Begin by marking the first instruction as "changed". */
  insn_flags_[0].SetChanged();
  uint32_t start_guess = 0;

  /* Continue until no instructions are marked "changed". */
  while (true) {
    // Find the first marked one. Use "start_guess" as a way to find one quickly.
    uint32_t insn_idx = start_guess;
    for (; insn_idx < insns_size; insn_idx++) {
      if (insn_flags_[insn_idx].IsChanged())
        break;
    }
    if (insn_idx == insns_size) {
      if (start_guess != 0) {
        /* try again, starting from the top */
        start_guess = 0;
        continue;
      } else {
        /* all flags are clear */
        break;
      }
    }
    // We carry the working set of registers from instruction to instruction. If this address can
    // be the target of a branch (or throw) instruction, or if we're skipping around chasing
    // "changed" flags, we need to load the set of registers from the table.
    // Because we always prefer to continue on to the next instruction, we should never have a
    // situation where we have a stray "changed" flag set on an instruction that isn't a branch
    // target.
    work_insn_idx_ = insn_idx;
    if (insn_flags_[insn_idx].IsBranchTarget()) {
      work_line_->CopyFromLine(reg_table_.GetLine(insn_idx));
    } else {
#ifndef NDEBUG
      /*
       * Sanity check: retrieve the stored register line (assuming
       * a full table) and make sure it actually matches.
       */
      RegisterLine* register_line = reg_table_.GetLine(insn_idx);
      if (register_line != NULL) {
        if (work_line_->CompareLine(register_line) != 0) {
          Dump(std::cout);
          std::cout << info_messages_.str();
          LOG(FATAL) << "work_line diverged in " << PrettyMethod(method_idx_, *dex_file_)
                     << "@" << reinterpret_cast<void*>(work_insn_idx_) << "\n"
                     << " work_line=" << *work_line_ << "\n"
                     << "  expected=" << *register_line;
        }
      }
#endif
    }
    if (!CodeFlowVerifyInstruction(&start_guess)) {
      std::string prepend(PrettyMethod(method_idx_, *dex_file_));
      prepend += " failed to verify: ";
      PrependToLastFailMessage(prepend);
      return false;
    }
    /* Clear "changed" and mark as visited. */
    insn_flags_[insn_idx].SetVisited();
    insn_flags_[insn_idx].ClearChanged();
  }

  if (DEAD_CODE_SCAN && ((method_access_flags_ & kAccWritable) == 0)) {
    /*
     * Scan for dead code. There's nothing "evil" about dead code
     * (besides the wasted space), but it indicates a flaw somewhere
     * down the line, possibly in the verifier.
     *
     * If we've substituted "always throw" instructions into the stream,
     * we are almost certainly going to have some dead code.
     */
    int dead_start = -1;
    uint32_t insn_idx = 0;
    for (; insn_idx < insns_size; insn_idx += insn_flags_[insn_idx].GetLengthInCodeUnits()) {
      /*
       * Switch-statement data doesn't get "visited" by scanner. It
       * may or may not be preceded by a padding NOP (for alignment).
       */
      if (insns[insn_idx] == Instruction::kPackedSwitchSignature ||
          insns[insn_idx] == Instruction::kSparseSwitchSignature ||
          insns[insn_idx] == Instruction::kArrayDataSignature ||
          (insns[insn_idx] == Instruction::NOP &&
           (insns[insn_idx + 1] == Instruction::kPackedSwitchSignature ||
            insns[insn_idx + 1] == Instruction::kSparseSwitchSignature ||
            insns[insn_idx + 1] == Instruction::kArrayDataSignature))) {
        insn_flags_[insn_idx].SetVisited();
      }

      if (!insn_flags_[insn_idx].IsVisited()) {
        if (dead_start < 0)
          dead_start = insn_idx;
      } else if (dead_start >= 0) {
        LogVerifyInfo() << "dead code " << reinterpret_cast<void*>(dead_start) << "-" << reinterpret_cast<void*>(insn_idx - 1);
        dead_start = -1;
      }
    }
    if (dead_start >= 0) {
      LogVerifyInfo() << "dead code " << reinterpret_cast<void*>(dead_start) << "-" << reinterpret_cast<void*>(insn_idx - 1);
    }
  }
  return true;
}

bool MethodVerifier::CodeFlowVerifyInstruction(uint32_t* start_guess) {
#ifdef VERIFIER_STATS
  if (CurrentInsnFlags().IsVisited()) {
    gDvm.verifierStats.instrsReexamined++;
  } else {
    gDvm.verifierStats.instrsExamined++;
  }
#endif

  /*
   * Once we finish decoding the instruction, we need to figure out where
   * we can go from here. There are three possible ways to transfer
   * control to another statement:
   *
   * (1) Continue to the next instruction. Applies to all but
   *     unconditional branches, method returns, and exception throws.
   * (2) Branch to one or more possible locations. Applies to branches
   *     and switch statements.
   * (3) Exception handlers. Applies to any instruction that can
   *     throw an exception that is handled by an encompassing "try"
   *     block.
   *
   * We can also return, in which case there is no successor instruction
   * from this point.
   *
   * The behavior can be determined from the opcode flags.
   */
  const uint16_t* insns = code_item_->insns_ + work_insn_idx_;
  const Instruction* inst = Instruction::At(insns);
  DecodedInstruction dec_insn(inst);
  int opcode_flags = Instruction::Flags(inst->Opcode());

  int32_t branch_target = 0;
  bool just_set_result = false;
  if (gDebugVerify) {
    // Generate processing back trace to debug verifier
    LogVerifyInfo() << "Processing " << inst->DumpString(dex_file_) << "\n"
                    << *work_line_.get() << "\n";
  }

  /*
   * Make a copy of the previous register state. If the instruction
   * can throw an exception, we will copy/merge this into the "catch"
   * address rather than work_line, because we don't want the result
   * from the "successful" code path (e.g. a check-cast that "improves"
   * a type) to be visible to the exception handler.
   */
  if ((opcode_flags & Instruction::kThrow) != 0 && CurrentInsnFlags()->IsInTry()) {
    saved_line_->CopyFromLine(work_line_.get());
  } else {
#ifndef NDEBUG
    saved_line_->FillWithGarbage();
#endif
  }

  switch (dec_insn.opcode) {
    case Instruction::NOP:
      /*
       * A "pure" NOP has no effect on anything. Data tables start with
       * a signature that looks like a NOP; if we see one of these in
       * the course of executing code then we have a problem.
       */
      if (dec_insn.vA != 0) {
        Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "encountered data table in instruction stream";
      }
      break;

    case Instruction::MOVE:
    case Instruction::MOVE_FROM16:
    case Instruction::MOVE_16:
      work_line_->CopyRegister1(dec_insn.vA, dec_insn.vB, kTypeCategory1nr);
      break;
    case Instruction::MOVE_WIDE:
    case Instruction::MOVE_WIDE_FROM16:
    case Instruction::MOVE_WIDE_16:
      work_line_->CopyRegister2(dec_insn.vA, dec_insn.vB);
      break;
    case Instruction::MOVE_OBJECT:
    case Instruction::MOVE_OBJECT_FROM16:
    case Instruction::MOVE_OBJECT_16:
      work_line_->CopyRegister1(dec_insn.vA, dec_insn.vB, kTypeCategoryRef);
      break;

    /*
     * The move-result instructions copy data out of a "pseudo-register"
     * with the results from the last method invocation. In practice we
     * might want to hold the result in an actual CPU register, so the
     * Dalvik spec requires that these only appear immediately after an
     * invoke or filled-new-array.
     *
     * These calls invalidate the "result" register. (This is now
     * redundant with the reset done below, but it can make the debug info
     * easier to read in some cases.)
     */
    case Instruction::MOVE_RESULT:
      work_line_->CopyResultRegister1(dec_insn.vA, false);
      break;
    case Instruction::MOVE_RESULT_WIDE:
      work_line_->CopyResultRegister2(dec_insn.vA);
      break;
    case Instruction::MOVE_RESULT_OBJECT:
      work_line_->CopyResultRegister1(dec_insn.vA, true);
      break;

    case Instruction::MOVE_EXCEPTION: {
      /*
       * This statement can only appear as the first instruction in an exception handler. We verify
       * that as part of extracting the exception type from the catch block list.
       */
      const RegType& res_type = GetCaughtExceptionType();
      work_line_->SetRegisterType(dec_insn.vA, res_type);
      break;
    }
    case Instruction::RETURN_VOID:
      if (!IsConstructor() || work_line_->CheckConstructorReturn()) {
        if (!GetMethodReturnType().IsConflict()) {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "return-void not expected";
        }
      }
      break;
    case Instruction::RETURN:
      if (!IsConstructor() || work_line_->CheckConstructorReturn()) {
        /* check the method signature */
        const RegType& return_type = GetMethodReturnType();
        if (!return_type.IsCategory1Types()) {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "unexpected non-category 1 return type " << return_type;
        } else {
          // Compilers may generate synthetic functions that write byte values into boolean fields.
          // Also, it may use integer values for boolean, byte, short, and character return types.
          const RegType& src_type = work_line_->GetRegisterType(dec_insn.vA);
          bool use_src = ((return_type.IsBoolean() && src_type.IsByte()) ||
                          ((return_type.IsBoolean() || return_type.IsByte() ||
                           return_type.IsShort() || return_type.IsChar()) &&
                           src_type.IsInteger()));
          /* check the register contents */
          bool success =
              work_line_->VerifyRegisterType(dec_insn.vA, use_src ? src_type : return_type);
          if (!success) {
            AppendToLastFailMessage(StringPrintf(" return-1nr on invalid register v%d", dec_insn.vA));
          }
        }
      }
      break;
    case Instruction::RETURN_WIDE:
      if (!IsConstructor() || work_line_->CheckConstructorReturn()) {
        /* check the method signature */
        const RegType& return_type = GetMethodReturnType();
        if (!return_type.IsCategory2Types()) {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "return-wide not expected";
        } else {
          /* check the register contents */
          bool success = work_line_->VerifyRegisterType(dec_insn.vA, return_type);
          if (!success) {
            AppendToLastFailMessage(StringPrintf(" return-wide on invalid register v%d", dec_insn.vA));
          }
        }
      }
      break;
    case Instruction::RETURN_OBJECT:
      if (!IsConstructor() || work_line_->CheckConstructorReturn()) {
        const RegType& return_type = GetMethodReturnType();
        if (!return_type.IsReferenceTypes()) {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "return-object not expected";
        } else {
          /* return_type is the *expected* return type, not register value */
          DCHECK(!return_type.IsZero());
          DCHECK(!return_type.IsUninitializedReference());
          const RegType& reg_type = work_line_->GetRegisterType(dec_insn.vA);
          // Disallow returning uninitialized values and verify that the reference in vAA is an
          // instance of the "return_type"
          if (reg_type.IsUninitializedTypes()) {
            Fail(VERIFY_ERROR_BAD_CLASS_SOFT) << "returning uninitialized object '" << reg_type << "'";
          } else if (!return_type.IsAssignableFrom(reg_type)) {
            Fail(reg_type.IsUnresolvedTypes() ? VERIFY_ERROR_BAD_CLASS_SOFT : VERIFY_ERROR_BAD_CLASS_HARD)
                << "returning '" << reg_type << "', but expected from declaration '" << return_type << "'";
          }
        }
      }
      break;

    case Instruction::CONST_4:
    case Instruction::CONST_16:
    case Instruction::CONST:
      /* could be boolean, int, float, or a null reference */
      work_line_->SetRegisterType(dec_insn.vA, reg_types_.FromCat1Const((int32_t) dec_insn.vB));
      break;
    case Instruction::CONST_HIGH16:
      /* could be boolean, int, float, or a null reference */
      work_line_->SetRegisterType(dec_insn.vA,
                                  reg_types_.FromCat1Const((int32_t) dec_insn.vB << 16));
      break;
    case Instruction::CONST_WIDE_16:
    case Instruction::CONST_WIDE_32:
    case Instruction::CONST_WIDE:
    case Instruction::CONST_WIDE_HIGH16:
      /* could be long or double; resolved upon use */
      work_line_->SetRegisterType(dec_insn.vA, reg_types_.ConstLo());
      break;
    case Instruction::CONST_STRING:
    case Instruction::CONST_STRING_JUMBO:
      work_line_->SetRegisterType(dec_insn.vA, reg_types_.JavaLangString());
      break;
    case Instruction::CONST_CLASS: {
      // Get type from instruction if unresolved then we need an access check
      // TODO: check Compiler::CanAccessTypeWithoutChecks returns false when res_type is unresolved
      const RegType& res_type = ResolveClassAndCheckAccess(dec_insn.vB);
      // Register holds class, ie its type is class, on error it will hold Conflict.
      work_line_->SetRegisterType(dec_insn.vA,
                                  res_type.IsConflict() ? res_type : reg_types_.JavaLangClass());
      break;
    }
    case Instruction::MONITOR_ENTER:
      work_line_->PushMonitor(dec_insn.vA, work_insn_idx_);
      break;
    case Instruction::MONITOR_EXIT:
      /*
       * monitor-exit instructions are odd. They can throw exceptions,
       * but when they do they act as if they succeeded and the PC is
       * pointing to the following instruction. (This behavior goes back
       * to the need to handle asynchronous exceptions, a now-deprecated
       * feature that Dalvik doesn't support.)
       *
       * In practice we don't need to worry about this. The only
       * exceptions that can be thrown from monitor-exit are for a
       * null reference and -exit without a matching -enter. If the
       * structured locking checks are working, the former would have
       * failed on the -enter instruction, and the latter is impossible.
       *
       * This is fortunate, because issue 3221411 prevents us from
       * chasing the "can throw" path when monitor verification is
       * enabled. If we can fully verify the locking we can ignore
       * some catch blocks (which will show up as "dead" code when
       * we skip them here); if we can't, then the code path could be
       * "live" so we still need to check it.
       */
      opcode_flags &= ~Instruction::kThrow;
      work_line_->PopMonitor(dec_insn.vA);
      break;

    case Instruction::CHECK_CAST:
    case Instruction::INSTANCE_OF: {
      /*
       * If this instruction succeeds, we will "downcast" register vA to the type in vB. (This
       * could be a "upcast" -- not expected, so we don't try to address it.)
       *
       * If it fails, an exception is thrown, which we deal with later by ignoring the update to
       * dec_insn.vA when branching to a handler.
       */
      bool is_checkcast = dec_insn.opcode == Instruction::CHECK_CAST;
      const RegType& res_type =
          ResolveClassAndCheckAccess(is_checkcast ? dec_insn.vB : dec_insn.vC);
      if (res_type.IsConflict()) {
        DCHECK_NE(failures_.size(), 0U);
        if (!is_checkcast) {
          work_line_->SetRegisterType(dec_insn.vA, reg_types_.Boolean());
        }
        break;  // bad class
      }
      // TODO: check Compiler::CanAccessTypeWithoutChecks returns false when res_type is unresolved
      const RegType& orig_type =
          work_line_->GetRegisterType(is_checkcast ? dec_insn.vA : dec_insn.vB);
      if (!res_type.IsNonZeroReferenceTypes()) {
        Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "check-cast on unexpected class " << res_type;
      } else if (!orig_type.IsReferenceTypes()) {
        Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "check-cast on non-reference in v" << dec_insn.vA;
      } else {
        if (is_checkcast) {
          work_line_->SetRegisterType(dec_insn.vA, res_type);
        } else {
          work_line_->SetRegisterType(dec_insn.vA, reg_types_.Boolean());
        }
      }
      break;
    }
    case Instruction::ARRAY_LENGTH: {
      const RegType& res_type = work_line_->GetRegisterType(dec_insn.vB);
      if (res_type.IsReferenceTypes()) {
        if (!res_type.IsArrayTypes() && !res_type.IsZero()) {  // ie not an array or null
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "array-length on non-array " << res_type;
        } else {
          work_line_->SetRegisterType(dec_insn.vA, reg_types_.Integer());
        }
      }
      break;
    }
    case Instruction::NEW_INSTANCE: {
      const RegType& res_type = ResolveClassAndCheckAccess(dec_insn.vB);
      if (res_type.IsConflict()) {
        DCHECK_NE(failures_.size(), 0U);
        break;  // bad class
      }
      // TODO: check Compiler::CanAccessTypeWithoutChecks returns false when res_type is unresolved
      // can't create an instance of an interface or abstract class */
      if (!res_type.IsInstantiableTypes()) {
        Fail(VERIFY_ERROR_INSTANTIATION)
            << "new-instance on primitive, interface or abstract class" << res_type;
      } else {
        const RegType& uninit_type = reg_types_.Uninitialized(res_type, work_insn_idx_);
        // Any registers holding previous allocations from this address that have not yet been
        // initialized must be marked invalid.
        work_line_->MarkUninitRefsAsInvalid(uninit_type);
        // add the new uninitialized reference to the register state
        work_line_->SetRegisterType(dec_insn.vA, uninit_type);
      }
      break;
    }
    case Instruction::NEW_ARRAY:
      VerifyNewArray(dec_insn, false, false);
      break;
    case Instruction::FILLED_NEW_ARRAY:
      VerifyNewArray(dec_insn, true, false);
      just_set_result = true;  // Filled new array sets result register
      break;
    case Instruction::FILLED_NEW_ARRAY_RANGE:
      VerifyNewArray(dec_insn, true, true);
      just_set_result = true;  // Filled new array range sets result register
      break;
    case Instruction::CMPL_FLOAT:
    case Instruction::CMPG_FLOAT:
      if (!work_line_->VerifyRegisterType(dec_insn.vB, reg_types_.Float())) {
        break;
      }
      if (!work_line_->VerifyRegisterType(dec_insn.vC, reg_types_.Float())) {
        break;
      }
      work_line_->SetRegisterType(dec_insn.vA, reg_types_.Integer());
      break;
    case Instruction::CMPL_DOUBLE:
    case Instruction::CMPG_DOUBLE:
      if (!work_line_->VerifyRegisterType(dec_insn.vB, reg_types_.Double())) {
        break;
      }
      if (!work_line_->VerifyRegisterType(dec_insn.vC, reg_types_.Double())) {
        break;
      }
      work_line_->SetRegisterType(dec_insn.vA, reg_types_.Integer());
      break;
    case Instruction::CMP_LONG:
      if (!work_line_->VerifyRegisterType(dec_insn.vB, reg_types_.Long())) {
        break;
      }
      if (!work_line_->VerifyRegisterType(dec_insn.vC, reg_types_.Long())) {
        break;
      }
      work_line_->SetRegisterType(dec_insn.vA, reg_types_.Integer());
      break;
    case Instruction::THROW: {
      const RegType& res_type = work_line_->GetRegisterType(dec_insn.vA);
      if (!reg_types_.JavaLangThrowable().IsAssignableFrom(res_type)) {
        Fail(VERIFY_ERROR_BAD_CLASS_SOFT) << "thrown class " << res_type << " not instanceof Throwable";
      }
      break;
    }
    case Instruction::GOTO:
    case Instruction::GOTO_16:
    case Instruction::GOTO_32:
      /* no effect on or use of registers */
      break;

    case Instruction::PACKED_SWITCH:
    case Instruction::SPARSE_SWITCH:
      /* verify that vAA is an integer, or can be converted to one */
      work_line_->VerifyRegisterType(dec_insn.vA, reg_types_.Integer());
      break;

    case Instruction::FILL_ARRAY_DATA: {
      /* Similar to the verification done for APUT */
      const RegType& array_type = work_line_->GetRegisterType(dec_insn.vA);
      /* array_type can be null if the reg type is Zero */
      if (!array_type.IsZero()) {
        if (!array_type.IsArrayTypes()) {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "invalid fill-array-data with array type " << array_type;
        } else {
          const RegType& component_type = reg_types_.GetComponentType(array_type, class_loader_);
          DCHECK(!component_type.IsConflict());
          if (component_type.IsNonZeroReferenceTypes()) {
            Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "invalid fill-array-data with component type "
                                              << component_type;
          } else {
            // Now verify if the element width in the table matches the element width declared in
            // the array
            const uint16_t* array_data = insns + (insns[1] | (((int32_t) insns[2]) << 16));
            if (array_data[0] != Instruction::kArrayDataSignature) {
              Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "invalid magic for array-data";
            } else {
              size_t elem_width = Primitive::ComponentSize(component_type.GetPrimitiveType());
              // Since we don't compress the data in Dex, expect to see equal width of data stored
              // in the table and expected from the array class.
              if (array_data[1] != elem_width) {
                Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "array-data size mismatch (" << array_data[1]
                                                  << " vs " << elem_width << ")";
              }
            }
          }
        }
      }
      break;
    }
    case Instruction::IF_EQ:
    case Instruction::IF_NE: {
      const RegType& reg_type1 = work_line_->GetRegisterType(dec_insn.vA);
      const RegType& reg_type2 = work_line_->GetRegisterType(dec_insn.vB);
      bool mismatch = false;
      if (reg_type1.IsZero()) {  // zero then integral or reference expected
        mismatch = !reg_type2.IsReferenceTypes() && !reg_type2.IsIntegralTypes();
      } else if (reg_type1.IsReferenceTypes()) {  // both references?
        mismatch = !reg_type2.IsReferenceTypes();
      } else {  // both integral?
        mismatch = !reg_type1.IsIntegralTypes() || !reg_type2.IsIntegralTypes();
      }
      if (mismatch) {
        Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "args to if-eq/if-ne (" << reg_type1 << "," << reg_type2
                                          << ") must both be references or integral";
      }
      break;
    }
    case Instruction::IF_LT:
    case Instruction::IF_GE:
    case Instruction::IF_GT:
    case Instruction::IF_LE: {
      const RegType& reg_type1 = work_line_->GetRegisterType(dec_insn.vA);
      const RegType& reg_type2 = work_line_->GetRegisterType(dec_insn.vB);
      if (!reg_type1.IsIntegralTypes() || !reg_type2.IsIntegralTypes()) {
        Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "args to 'if' (" << reg_type1 << ","
                                          << reg_type2 << ") must be integral";
      }
      break;
    }
    case Instruction::IF_EQZ:
    case Instruction::IF_NEZ: {
      const RegType& reg_type = work_line_->GetRegisterType(dec_insn.vA);
      if (!reg_type.IsReferenceTypes() && !reg_type.IsIntegralTypes()) {
        Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "type " << reg_type << " unexpected as arg to if-eqz/if-nez";
      }
      break;
    }
    case Instruction::IF_LTZ:
    case Instruction::IF_GEZ:
    case Instruction::IF_GTZ:
    case Instruction::IF_LEZ: {
      const RegType& reg_type = work_line_->GetRegisterType(dec_insn.vA);
      if (!reg_type.IsIntegralTypes()) {
        Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "type " << reg_type
                                          << " unexpected as arg to if-ltz/if-gez/if-gtz/if-lez";
      }
      break;
    }
    case Instruction::AGET_BOOLEAN:
      VerifyAGet(dec_insn, reg_types_.Boolean(), true);
      break;
    case Instruction::AGET_BYTE:
      VerifyAGet(dec_insn, reg_types_.Byte(), true);
      break;
    case Instruction::AGET_CHAR:
      VerifyAGet(dec_insn, reg_types_.Char(), true);
      break;
    case Instruction::AGET_SHORT:
      VerifyAGet(dec_insn, reg_types_.Short(), true);
      break;
    case Instruction::AGET:
      VerifyAGet(dec_insn, reg_types_.Integer(), true);
      break;
    case Instruction::AGET_WIDE:
      VerifyAGet(dec_insn, reg_types_.Long(), true);
      break;
    case Instruction::AGET_OBJECT:
      VerifyAGet(dec_insn, reg_types_.JavaLangObject(), false);
      break;

    case Instruction::APUT_BOOLEAN:
      VerifyAPut(dec_insn, reg_types_.Boolean(), true);
      break;
    case Instruction::APUT_BYTE:
      VerifyAPut(dec_insn, reg_types_.Byte(), true);
      break;
    case Instruction::APUT_CHAR:
      VerifyAPut(dec_insn, reg_types_.Char(), true);
      break;
    case Instruction::APUT_SHORT:
      VerifyAPut(dec_insn, reg_types_.Short(), true);
      break;
    case Instruction::APUT:
      VerifyAPut(dec_insn, reg_types_.Integer(), true);
      break;
    case Instruction::APUT_WIDE:
      VerifyAPut(dec_insn, reg_types_.Long(), true);
      break;
    case Instruction::APUT_OBJECT:
      VerifyAPut(dec_insn, reg_types_.JavaLangObject(), false);
      break;

    case Instruction::IGET_BOOLEAN:
      VerifyISGet(dec_insn, reg_types_.Boolean(), true, false);
      break;
    case Instruction::IGET_BYTE:
      VerifyISGet(dec_insn, reg_types_.Byte(), true, false);
      break;
    case Instruction::IGET_CHAR:
      VerifyISGet(dec_insn, reg_types_.Char(), true, false);
      break;
    case Instruction::IGET_SHORT:
      VerifyISGet(dec_insn, reg_types_.Short(), true, false);
      break;
    case Instruction::IGET:
      VerifyISGet(dec_insn, reg_types_.Integer(), true, false);
      break;
    case Instruction::IGET_WIDE:
      VerifyISGet(dec_insn, reg_types_.Long(), true, false);
      break;
    case Instruction::IGET_OBJECT:
      VerifyISGet(dec_insn, reg_types_.JavaLangObject(), false, false);
      break;

    case Instruction::IPUT_BOOLEAN:
      VerifyISPut(dec_insn, reg_types_.Boolean(), true, false);
      break;
    case Instruction::IPUT_BYTE:
      VerifyISPut(dec_insn, reg_types_.Byte(), true, false);
      break;
    case Instruction::IPUT_CHAR:
      VerifyISPut(dec_insn, reg_types_.Char(), true, false);
      break;
    case Instruction::IPUT_SHORT:
      VerifyISPut(dec_insn, reg_types_.Short(), true, false);
      break;
    case Instruction::IPUT:
      VerifyISPut(dec_insn, reg_types_.Integer(), true, false);
      break;
    case Instruction::IPUT_WIDE:
      VerifyISPut(dec_insn, reg_types_.Long(), true, false);
      break;
    case Instruction::IPUT_OBJECT:
      VerifyISPut(dec_insn, reg_types_.JavaLangObject(), false, false);
      break;

    case Instruction::SGET_BOOLEAN:
      VerifyISGet(dec_insn, reg_types_.Boolean(), true, true);
      break;
    case Instruction::SGET_BYTE:
      VerifyISGet(dec_insn, reg_types_.Byte(), true, true);
      break;
    case Instruction::SGET_CHAR:
      VerifyISGet(dec_insn, reg_types_.Char(), true, true);
      break;
    case Instruction::SGET_SHORT:
      VerifyISGet(dec_insn, reg_types_.Short(), true, true);
      break;
    case Instruction::SGET:
      VerifyISGet(dec_insn, reg_types_.Integer(), true, true);
      break;
    case Instruction::SGET_WIDE:
      VerifyISGet(dec_insn, reg_types_.Long(), true, true);
      break;
    case Instruction::SGET_OBJECT:
      VerifyISGet(dec_insn, reg_types_.JavaLangObject(), false, true);
      break;

    case Instruction::SPUT_BOOLEAN:
      VerifyISPut(dec_insn, reg_types_.Boolean(), true, true);
      break;
    case Instruction::SPUT_BYTE:
      VerifyISPut(dec_insn, reg_types_.Byte(), true, true);
      break;
    case Instruction::SPUT_CHAR:
      VerifyISPut(dec_insn, reg_types_.Char(), true, true);
      break;
    case Instruction::SPUT_SHORT:
      VerifyISPut(dec_insn, reg_types_.Short(), true, true);
      break;
    case Instruction::SPUT:
      VerifyISPut(dec_insn, reg_types_.Integer(), true, true);
      break;
    case Instruction::SPUT_WIDE:
      VerifyISPut(dec_insn, reg_types_.Long(), true, true);
      break;
    case Instruction::SPUT_OBJECT:
      VerifyISPut(dec_insn, reg_types_.JavaLangObject(), false, true);
      break;

    case Instruction::INVOKE_VIRTUAL:
    case Instruction::INVOKE_VIRTUAL_RANGE:
    case Instruction::INVOKE_SUPER:
    case Instruction::INVOKE_SUPER_RANGE: {
      bool is_range = (dec_insn.opcode == Instruction::INVOKE_VIRTUAL_RANGE ||
                       dec_insn.opcode == Instruction::INVOKE_SUPER_RANGE);
      bool is_super =  (dec_insn.opcode == Instruction::INVOKE_SUPER ||
                        dec_insn.opcode == Instruction::INVOKE_SUPER_RANGE);
      Method* called_method = VerifyInvocationArgs(dec_insn, METHOD_VIRTUAL, is_range, is_super);
      const char* descriptor;
      if (called_method == NULL) {
        uint32_t method_idx = dec_insn.vB;
        const DexFile::MethodId& method_id = dex_file_->GetMethodId(method_idx);
        uint32_t return_type_idx = dex_file_->GetProtoId(method_id.proto_idx_).return_type_idx_;
        descriptor =  dex_file_->StringByTypeIdx(return_type_idx);
      } else {
        descriptor = MethodHelper(called_method).GetReturnTypeDescriptor();
      }
      const RegType& return_type = reg_types_.FromDescriptor(class_loader_, descriptor);
      work_line_->SetResultRegisterType(return_type);
      just_set_result = true;
      break;
    }
    case Instruction::INVOKE_DIRECT:
    case Instruction::INVOKE_DIRECT_RANGE: {
      bool is_range = (dec_insn.opcode == Instruction::INVOKE_DIRECT_RANGE);
      Method* called_method = VerifyInvocationArgs(dec_insn, METHOD_DIRECT, is_range, false);
      const char* return_type_descriptor;
      bool is_constructor;
      if (called_method == NULL) {
        uint32_t method_idx = dec_insn.vB;
        const DexFile::MethodId& method_id = dex_file_->GetMethodId(method_idx);
        is_constructor = StringPiece(dex_file_->GetMethodName(method_id)) == "<init>";
        uint32_t return_type_idx = dex_file_->GetProtoId(method_id.proto_idx_).return_type_idx_;
        return_type_descriptor =  dex_file_->StringByTypeIdx(return_type_idx);
      } else {
        is_constructor = called_method->IsConstructor();
        return_type_descriptor = MethodHelper(called_method).GetReturnTypeDescriptor();
      }
      if (is_constructor) {
        /*
         * Some additional checks when calling a constructor. We know from the invocation arg check
         * that the "this" argument is an instance of called_method->klass. Now we further restrict
         * that to require that called_method->klass is the same as this->klass or this->super,
         * allowing the latter only if the "this" argument is the same as the "this" argument to
         * this method (which implies that we're in a constructor ourselves).
         */
        const RegType& this_type = work_line_->GetInvocationThis(dec_insn);
        if (this_type.IsConflict())  // failure.
          break;

        /* no null refs allowed (?) */
        if (this_type.IsZero()) {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "unable to initialize null ref";
          break;
        }

        /* must be in same class or in superclass */
        // const RegType& this_super_klass = this_type.GetSuperClass(&reg_types_);
        // TODO: re-enable constructor type verification
        // if (this_super_klass.IsConflict()) {
          // Unknown super class, fail so we re-check at runtime.
          // Fail(VERIFY_ERROR_BAD_CLASS_SOFT) << "super class unknown for '" << this_type << "'";
          // break;
        // }

        /* arg must be an uninitialized reference */
        if (!this_type.IsUninitializedTypes()) {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "Expected initialization on uninitialized reference "
              << this_type;
          break;
        }

        /*
         * Replace the uninitialized reference with an initialized one. We need to do this for all
         * registers that have the same object instance in them, not just the "this" register.
         */
        work_line_->MarkRefsAsInitialized(this_type);
      }
      const RegType& return_type = reg_types_.FromDescriptor(class_loader_, return_type_descriptor);
      work_line_->SetResultRegisterType(return_type);
      just_set_result = true;
      break;
    }
    case Instruction::INVOKE_STATIC:
    case Instruction::INVOKE_STATIC_RANGE: {
        bool is_range = (dec_insn.opcode == Instruction::INVOKE_STATIC_RANGE);
        Method* called_method = VerifyInvocationArgs(dec_insn, METHOD_STATIC, is_range, false);
        const char* descriptor;
        if (called_method == NULL) {
          uint32_t method_idx = dec_insn.vB;
          const DexFile::MethodId& method_id = dex_file_->GetMethodId(method_idx);
          uint32_t return_type_idx = dex_file_->GetProtoId(method_id.proto_idx_).return_type_idx_;
          descriptor =  dex_file_->StringByTypeIdx(return_type_idx);
        } else {
          descriptor = MethodHelper(called_method).GetReturnTypeDescriptor();
        }
        const RegType& return_type =  reg_types_.FromDescriptor(class_loader_, descriptor);
        work_line_->SetResultRegisterType(return_type);
        just_set_result = true;
      }
      break;
    case Instruction::INVOKE_INTERFACE:
    case Instruction::INVOKE_INTERFACE_RANGE: {
      bool is_range =  (dec_insn.opcode == Instruction::INVOKE_INTERFACE_RANGE);
      Method* abs_method = VerifyInvocationArgs(dec_insn, METHOD_INTERFACE, is_range, false);
      if (abs_method != NULL) {
        Class* called_interface = abs_method->GetDeclaringClass();
        if (!called_interface->IsInterface() && !called_interface->IsObjectClass()) {
          Fail(VERIFY_ERROR_CLASS_CHANGE) << "expected interface class in invoke-interface '"
              << PrettyMethod(abs_method) << "'";
          break;
        }
      }
      /* Get the type of the "this" arg, which should either be a sub-interface of called
       * interface or Object (see comments in RegType::JoinClass).
       */
      const RegType& this_type = work_line_->GetInvocationThis(dec_insn);
      if (this_type.IsZero()) {
        /* null pointer always passes (and always fails at runtime) */
      } else {
        if (this_type.IsUninitializedTypes()) {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "interface call on uninitialized object "
              << this_type;
          break;
        }
        // In the past we have tried to assert that "called_interface" is assignable
        // from "this_type.GetClass()", however, as we do an imprecise Join
        // (RegType::JoinClass) we don't have full information on what interfaces are
        // implemented by "this_type". For example, two classes may implement the same
        // interfaces and have a common parent that doesn't implement the interface. The
        // join will set "this_type" to the parent class and a test that this implements
        // the interface will incorrectly fail.
      }
      /*
       * We don't have an object instance, so we can't find the concrete method. However, all of
       * the type information is in the abstract method, so we're good.
       */
      const char* descriptor;
      if (abs_method == NULL) {
        uint32_t method_idx = dec_insn.vB;
        const DexFile::MethodId& method_id = dex_file_->GetMethodId(method_idx);
        uint32_t return_type_idx = dex_file_->GetProtoId(method_id.proto_idx_).return_type_idx_;
        descriptor =  dex_file_->StringByTypeIdx(return_type_idx);
      } else {
        descriptor = MethodHelper(abs_method).GetReturnTypeDescriptor();
      }
      const RegType& return_type = reg_types_.FromDescriptor(class_loader_, descriptor);
      work_line_->SetResultRegisterType(return_type);
      work_line_->SetResultRegisterType(return_type);
      just_set_result = true;
      break;
    }
    case Instruction::NEG_INT:
    case Instruction::NOT_INT:
      work_line_->CheckUnaryOp(dec_insn, reg_types_.Integer(), reg_types_.Integer());
      break;
    case Instruction::NEG_LONG:
    case Instruction::NOT_LONG:
      work_line_->CheckUnaryOp(dec_insn, reg_types_.Long(), reg_types_.Long());
      break;
    case Instruction::NEG_FLOAT:
      work_line_->CheckUnaryOp(dec_insn, reg_types_.Float(), reg_types_.Float());
      break;
    case Instruction::NEG_DOUBLE:
      work_line_->CheckUnaryOp(dec_insn, reg_types_.Double(), reg_types_.Double());
      break;
    case Instruction::INT_TO_LONG:
      work_line_->CheckUnaryOp(dec_insn, reg_types_.Long(), reg_types_.Integer());
      break;
    case Instruction::INT_TO_FLOAT:
      work_line_->CheckUnaryOp(dec_insn, reg_types_.Float(), reg_types_.Integer());
      break;
    case Instruction::INT_TO_DOUBLE:
      work_line_->CheckUnaryOp(dec_insn, reg_types_.Double(), reg_types_.Integer());
      break;
    case Instruction::LONG_TO_INT:
      work_line_->CheckUnaryOp(dec_insn, reg_types_.Integer(), reg_types_.Long());
      break;
    case Instruction::LONG_TO_FLOAT:
      work_line_->CheckUnaryOp(dec_insn, reg_types_.Float(), reg_types_.Long());
      break;
    case Instruction::LONG_TO_DOUBLE:
      work_line_->CheckUnaryOp(dec_insn, reg_types_.Double(), reg_types_.Long());
      break;
    case Instruction::FLOAT_TO_INT:
      work_line_->CheckUnaryOp(dec_insn, reg_types_.Integer(), reg_types_.Float());
      break;
    case Instruction::FLOAT_TO_LONG:
      work_line_->CheckUnaryOp(dec_insn, reg_types_.Long(), reg_types_.Float());
      break;
    case Instruction::FLOAT_TO_DOUBLE:
      work_line_->CheckUnaryOp(dec_insn, reg_types_.Double(), reg_types_.Float());
      break;
    case Instruction::DOUBLE_TO_INT:
      work_line_->CheckUnaryOp(dec_insn, reg_types_.Integer(), reg_types_.Double());
      break;
    case Instruction::DOUBLE_TO_LONG:
      work_line_->CheckUnaryOp(dec_insn, reg_types_.Long(), reg_types_.Double());
      break;
    case Instruction::DOUBLE_TO_FLOAT:
      work_line_->CheckUnaryOp(dec_insn, reg_types_.Float(), reg_types_.Double());
      break;
    case Instruction::INT_TO_BYTE:
      work_line_->CheckUnaryOp(dec_insn, reg_types_.Byte(), reg_types_.Integer());
      break;
    case Instruction::INT_TO_CHAR:
      work_line_->CheckUnaryOp(dec_insn, reg_types_.Char(), reg_types_.Integer());
      break;
    case Instruction::INT_TO_SHORT:
      work_line_->CheckUnaryOp(dec_insn, reg_types_.Short(), reg_types_.Integer());
      break;

    case Instruction::ADD_INT:
    case Instruction::SUB_INT:
    case Instruction::MUL_INT:
    case Instruction::REM_INT:
    case Instruction::DIV_INT:
    case Instruction::SHL_INT:
    case Instruction::SHR_INT:
    case Instruction::USHR_INT:
      work_line_->CheckBinaryOp(dec_insn, reg_types_.Integer(), reg_types_.Integer(), reg_types_.Integer(), false);
      break;
    case Instruction::AND_INT:
    case Instruction::OR_INT:
    case Instruction::XOR_INT:
      work_line_->CheckBinaryOp(dec_insn, reg_types_.Integer(), reg_types_.Integer(), reg_types_.Integer(), true);
      break;
    case Instruction::ADD_LONG:
    case Instruction::SUB_LONG:
    case Instruction::MUL_LONG:
    case Instruction::DIV_LONG:
    case Instruction::REM_LONG:
    case Instruction::AND_LONG:
    case Instruction::OR_LONG:
    case Instruction::XOR_LONG:
      work_line_->CheckBinaryOp(dec_insn, reg_types_.Long(), reg_types_.Long(), reg_types_.Long(), false);
      break;
    case Instruction::SHL_LONG:
    case Instruction::SHR_LONG:
    case Instruction::USHR_LONG:
      /* shift distance is Int, making these different from other binary operations */
      work_line_->CheckBinaryOp(dec_insn, reg_types_.Long(), reg_types_.Long(), reg_types_.Integer(), false);
      break;
    case Instruction::ADD_FLOAT:
    case Instruction::SUB_FLOAT:
    case Instruction::MUL_FLOAT:
    case Instruction::DIV_FLOAT:
    case Instruction::REM_FLOAT:
      work_line_->CheckBinaryOp(dec_insn, reg_types_.Float(), reg_types_.Float(), reg_types_.Float(), false);
      break;
    case Instruction::ADD_DOUBLE:
    case Instruction::SUB_DOUBLE:
    case Instruction::MUL_DOUBLE:
    case Instruction::DIV_DOUBLE:
    case Instruction::REM_DOUBLE:
      work_line_->CheckBinaryOp(dec_insn, reg_types_.Double(), reg_types_.Double(), reg_types_.Double(), false);
      break;
    case Instruction::ADD_INT_2ADDR:
    case Instruction::SUB_INT_2ADDR:
    case Instruction::MUL_INT_2ADDR:
    case Instruction::REM_INT_2ADDR:
    case Instruction::SHL_INT_2ADDR:
    case Instruction::SHR_INT_2ADDR:
    case Instruction::USHR_INT_2ADDR:
      work_line_->CheckBinaryOp2addr(dec_insn, reg_types_.Integer(), reg_types_.Integer(), reg_types_.Integer(), false);
      break;
    case Instruction::AND_INT_2ADDR:
    case Instruction::OR_INT_2ADDR:
    case Instruction::XOR_INT_2ADDR:
      work_line_->CheckBinaryOp2addr(dec_insn, reg_types_.Integer(), reg_types_.Integer(), reg_types_.Integer(), true);
      break;
    case Instruction::DIV_INT_2ADDR:
      work_line_->CheckBinaryOp2addr(dec_insn, reg_types_.Integer(), reg_types_.Integer(), reg_types_.Integer(), false);
      break;
    case Instruction::ADD_LONG_2ADDR:
    case Instruction::SUB_LONG_2ADDR:
    case Instruction::MUL_LONG_2ADDR:
    case Instruction::DIV_LONG_2ADDR:
    case Instruction::REM_LONG_2ADDR:
    case Instruction::AND_LONG_2ADDR:
    case Instruction::OR_LONG_2ADDR:
    case Instruction::XOR_LONG_2ADDR:
      work_line_->CheckBinaryOp2addr(dec_insn, reg_types_.Long(), reg_types_.Long(), reg_types_.Long(), false);
      break;
    case Instruction::SHL_LONG_2ADDR:
    case Instruction::SHR_LONG_2ADDR:
    case Instruction::USHR_LONG_2ADDR:
      work_line_->CheckBinaryOp2addr(dec_insn, reg_types_.Long(), reg_types_.Long(), reg_types_.Integer(), false);
      break;
    case Instruction::ADD_FLOAT_2ADDR:
    case Instruction::SUB_FLOAT_2ADDR:
    case Instruction::MUL_FLOAT_2ADDR:
    case Instruction::DIV_FLOAT_2ADDR:
    case Instruction::REM_FLOAT_2ADDR:
      work_line_->CheckBinaryOp2addr(dec_insn, reg_types_.Float(), reg_types_.Float(), reg_types_.Float(), false);
      break;
    case Instruction::ADD_DOUBLE_2ADDR:
    case Instruction::SUB_DOUBLE_2ADDR:
    case Instruction::MUL_DOUBLE_2ADDR:
    case Instruction::DIV_DOUBLE_2ADDR:
    case Instruction::REM_DOUBLE_2ADDR:
      work_line_->CheckBinaryOp2addr(dec_insn, reg_types_.Double(), reg_types_.Double(),  reg_types_.Double(), false);
      break;
    case Instruction::ADD_INT_LIT16:
    case Instruction::RSUB_INT:
    case Instruction::MUL_INT_LIT16:
    case Instruction::DIV_INT_LIT16:
    case Instruction::REM_INT_LIT16:
      work_line_->CheckLiteralOp(dec_insn, reg_types_.Integer(), reg_types_.Integer(), false);
      break;
    case Instruction::AND_INT_LIT16:
    case Instruction::OR_INT_LIT16:
    case Instruction::XOR_INT_LIT16:
      work_line_->CheckLiteralOp(dec_insn, reg_types_.Integer(), reg_types_.Integer(), true);
      break;
    case Instruction::ADD_INT_LIT8:
    case Instruction::RSUB_INT_LIT8:
    case Instruction::MUL_INT_LIT8:
    case Instruction::DIV_INT_LIT8:
    case Instruction::REM_INT_LIT8:
    case Instruction::SHL_INT_LIT8:
    case Instruction::SHR_INT_LIT8:
    case Instruction::USHR_INT_LIT8:
      work_line_->CheckLiteralOp(dec_insn, reg_types_.Integer(), reg_types_.Integer(), false);
      break;
    case Instruction::AND_INT_LIT8:
    case Instruction::OR_INT_LIT8:
    case Instruction::XOR_INT_LIT8:
      work_line_->CheckLiteralOp(dec_insn, reg_types_.Integer(), reg_types_.Integer(), true);
      break;

    /*
     * This falls into the general category of "optimized" instructions,
     * which don't generally appear during verification. Because it's
     * inserted in the course of verification, we can expect to see it here.
     */
    case Instruction::THROW_VERIFICATION_ERROR:
      break;

    /* These should never appear during verification. */
    case Instruction::UNUSED_EE:
    case Instruction::UNUSED_EF:
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
    case Instruction::UNUSED_F0:
    case Instruction::UNUSED_F1:
    case Instruction::UNUSED_E3:
    case Instruction::UNUSED_E8:
    case Instruction::UNUSED_E7:
    case Instruction::UNUSED_E4:
    case Instruction::UNUSED_E9:
    case Instruction::UNUSED_FC:
    case Instruction::UNUSED_E5:
    case Instruction::UNUSED_EA:
    case Instruction::UNUSED_FD:
    case Instruction::UNUSED_E6:
    case Instruction::UNUSED_EB:
    case Instruction::UNUSED_FE:
    case Instruction::UNUSED_3E:
    case Instruction::UNUSED_3F:
    case Instruction::UNUSED_40:
    case Instruction::UNUSED_41:
    case Instruction::UNUSED_42:
    case Instruction::UNUSED_43:
    case Instruction::UNUSED_73:
    case Instruction::UNUSED_79:
    case Instruction::UNUSED_7A:
    case Instruction::UNUSED_EC:
    case Instruction::UNUSED_FF:
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "Unexpected opcode " << inst->DumpString(dex_file_);
      break;

    /*
     * DO NOT add a "default" clause here. Without it the compiler will
     * complain if an instruction is missing (which is desirable).
     */
  }  // end - switch (dec_insn.opcode)

  if (have_pending_hard_failure_) {
    if (!Runtime::Current()->IsStarted()) {
      /* When compiling, check that the last failure is a hard failure */
      CHECK_EQ(failures_[failures_.size() - 1], VERIFY_ERROR_BAD_CLASS_HARD);
    }
    /* immediate failure, reject class */
    info_messages_ << "Rejecting opcode " << inst->DumpString(dex_file_);
    return false;
  } else if (have_pending_rewrite_failure_) {
    /* replace opcode and continue on */
    std::string append("Replacing opcode ");
    append += inst->DumpString(dex_file_);
    AppendToLastFailMessage(append);
    ReplaceFailingInstruction();
    /* IMPORTANT: method->insns may have been changed */
    insns = code_item_->insns_ + work_insn_idx_;
    /* continue on as if we just handled a throw-verification-error */
    opcode_flags = Instruction::kThrow;
  }
  /*
   * If we didn't just set the result register, clear it out. This ensures that you can only use
   * "move-result" immediately after the result is set. (We could check this statically, but it's
   * not expensive and it makes our debugging output cleaner.)
   */
  if (!just_set_result) {
    work_line_->SetResultTypeToUnknown();
  }

  /* Handle "continue". Tag the next consecutive instruction. */
  if ((opcode_flags & Instruction::kContinue) != 0) {
    uint32_t next_insn_idx = work_insn_idx_ + CurrentInsnFlags()->GetLengthInCodeUnits();
    if (next_insn_idx >= code_item_->insns_size_in_code_units_) {
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "Execution can walk off end of code area";
      return false;
    }
    // The only way to get to a move-exception instruction is to get thrown there. Make sure the
    // next instruction isn't one.
    if (!CheckNotMoveException(code_item_->insns_, next_insn_idx)) {
      return false;
    }
    RegisterLine* next_line = reg_table_.GetLine(next_insn_idx);
    if (next_line != NULL) {
      // Merge registers into what we have for the next instruction, and set the "changed" flag if
      // needed.
      if (!UpdateRegisters(next_insn_idx, work_line_.get())) {
        return false;
      }
    } else {
      /*
       * We're not recording register data for the next instruction, so we don't know what the prior
       * state was. We have to assume that something has changed and re-evaluate it.
       */
      insn_flags_[next_insn_idx].SetChanged();
    }
  }

  /*
   * Handle "branch". Tag the branch target.
   *
   * NOTE: instructions like Instruction::EQZ provide information about the
   * state of the register when the branch is taken or not taken. For example,
   * somebody could get a reference field, check it for zero, and if the
   * branch is taken immediately store that register in a boolean field
   * since the value is known to be zero. We do not currently account for
   * that, and will reject the code.
   *
   * TODO: avoid re-fetching the branch target
   */
  if ((opcode_flags & Instruction::kBranch) != 0) {
    bool isConditional, selfOkay;
    if (!GetBranchOffset(work_insn_idx_, &branch_target, &isConditional, &selfOkay)) {
      /* should never happen after static verification */
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "bad branch";
      return false;
    }
    DCHECK_EQ(isConditional, (opcode_flags & Instruction::kContinue) != 0);
    if (!CheckNotMoveException(code_item_->insns_, work_insn_idx_ + branch_target)) {
      return false;
    }
    /* update branch target, set "changed" if appropriate */
    if (!UpdateRegisters(work_insn_idx_ + branch_target, work_line_.get())) {
      return false;
    }
  }

  /*
   * Handle "switch". Tag all possible branch targets.
   *
   * We've already verified that the table is structurally sound, so we
   * just need to walk through and tag the targets.
   */
  if ((opcode_flags & Instruction::kSwitch) != 0) {
    int offset_to_switch = insns[1] | (((int32_t) insns[2]) << 16);
    const uint16_t* switch_insns = insns + offset_to_switch;
    int switch_count = switch_insns[1];
    int offset_to_targets, targ;

    if ((*insns & 0xff) == Instruction::PACKED_SWITCH) {
      /* 0 = sig, 1 = count, 2/3 = first key */
      offset_to_targets = 4;
    } else {
      /* 0 = sig, 1 = count, 2..count * 2 = keys */
      DCHECK((*insns & 0xff) == Instruction::SPARSE_SWITCH);
      offset_to_targets = 2 + 2 * switch_count;
    }

    /* verify each switch target */
    for (targ = 0; targ < switch_count; targ++) {
      int offset;
      uint32_t abs_offset;

      /* offsets are 32-bit, and only partly endian-swapped */
      offset = switch_insns[offset_to_targets + targ * 2] |
         (((int32_t) switch_insns[offset_to_targets + targ * 2 + 1]) << 16);
      abs_offset = work_insn_idx_ + offset;
      DCHECK_LT(abs_offset, code_item_->insns_size_in_code_units_);
      if (!CheckNotMoveException(code_item_->insns_, abs_offset)) {
        return false;
      }
      if (!UpdateRegisters(abs_offset, work_line_.get()))
        return false;
    }
  }

  /*
   * Handle instructions that can throw and that are sitting in a "try" block. (If they're not in a
   * "try" block when they throw, control transfers out of the method.)
   */
  if ((opcode_flags & Instruction::kThrow) != 0 && insn_flags_[work_insn_idx_].IsInTry()) {
    bool within_catch_all = false;
    CatchHandlerIterator iterator(*code_item_, work_insn_idx_);

    for (; iterator.HasNext(); iterator.Next()) {
      if (iterator.GetHandlerTypeIndex() == DexFile::kDexNoIndex16) {
        within_catch_all = true;
      }
      /*
       * Merge registers into the "catch" block. We want to use the "savedRegs" rather than
       * "work_regs", because at runtime the exception will be thrown before the instruction
       * modifies any registers.
       */
      if (!UpdateRegisters(iterator.GetHandlerAddress(), saved_line_.get())) {
        return false;
      }
    }

    /*
     * If the monitor stack depth is nonzero, there must be a "catch all" handler for this
     * instruction. This does apply to monitor-exit because of async exception handling.
     */
    if (work_line_->MonitorStackDepth() > 0 && !within_catch_all) {
      /*
       * The state in work_line reflects the post-execution state. If the current instruction is a
       * monitor-enter and the monitor stack was empty, we don't need a catch-all (if it throws,
       * it will do so before grabbing the lock).
       */
      if (dec_insn.opcode != Instruction::MONITOR_ENTER || work_line_->MonitorStackDepth() != 1) {
        Fail(VERIFY_ERROR_BAD_CLASS_HARD)
            << "expected to be within a catch-all for an instruction where a monitor is held";
        return false;
      }
    }
  }

  /* If we're returning from the method, make sure monitor stack is empty. */
  if ((opcode_flags & Instruction::kReturn) != 0) {
    if (!work_line_->VerifyMonitorStackEmpty()) {
      return false;
    }
  }

  /*
   * Update start_guess. Advance to the next instruction of that's
   * possible, otherwise use the branch target if one was found. If
   * neither of those exists we're in a return or throw; leave start_guess
   * alone and let the caller sort it out.
   */
  if ((opcode_flags & Instruction::kContinue) != 0) {
    *start_guess = work_insn_idx_ + insn_flags_[work_insn_idx_].GetLengthInCodeUnits();
  } else if ((opcode_flags & Instruction::kBranch) != 0) {
    /* we're still okay if branch_target is zero */
    *start_guess = work_insn_idx_ + branch_target;
  }

  DCHECK_LT(*start_guess, code_item_->insns_size_in_code_units_);
  DCHECK(insn_flags_[*start_guess].IsOpcode());

  return true;
}

const RegType& MethodVerifier::ResolveClassAndCheckAccess(uint32_t class_idx) {
  const char* descriptor = dex_file_->StringByTypeIdx(class_idx);
  const RegType& referrer = GetDeclaringClass();
  Class* klass = dex_cache_->GetResolvedType(class_idx);
  const RegType& result =
      klass != NULL ? reg_types_.FromClass(klass)
                    : reg_types_.FromDescriptor(class_loader_, descriptor);
  if (result.IsConflict()) {
    Fail(VERIFY_ERROR_BAD_CLASS_SOFT) << "accessing broken descriptor '" << descriptor
        << "' in " << referrer;
    return result;
  }
  if (klass == NULL && !result.IsUnresolvedTypes()) {
    dex_cache_->SetResolvedType(class_idx, result.GetClass());
  }
  // Check if access is allowed. Unresolved types use xxxWithAccessCheck to
  // check at runtime if access is allowed and so pass here.
  if (!result.IsUnresolvedTypes() && !referrer.IsUnresolvedTypes() && !referrer.CanAccess(result)) {
    Fail(VERIFY_ERROR_ACCESS_CLASS) << "illegal class access: '"
                                    << referrer << "' -> '" << result << "'";
  }
  return result;
}

const RegType& MethodVerifier::GetCaughtExceptionType() {
  const RegType* common_super = NULL;
  if (code_item_->tries_size_ != 0) {
    const byte* handlers_ptr = DexFile::GetCatchHandlerData(*code_item_, 0);
    uint32_t handlers_size = DecodeUnsignedLeb128(&handlers_ptr);
    for (uint32_t i = 0; i < handlers_size; i++) {
      CatchHandlerIterator iterator(handlers_ptr);
      for (; iterator.HasNext(); iterator.Next()) {
        if (iterator.GetHandlerAddress() == (uint32_t) work_insn_idx_) {
          if (iterator.GetHandlerTypeIndex() == DexFile::kDexNoIndex16) {
            common_super = &reg_types_.JavaLangThrowable();
          } else {
            const RegType& exception = ResolveClassAndCheckAccess(iterator.GetHandlerTypeIndex());
            if (common_super == NULL) {
              // Unconditionally assign for the first handler. We don't assert this is a Throwable
              // as that is caught at runtime
              common_super = &exception;
            } else if (!reg_types_.JavaLangThrowable().IsAssignableFrom(exception)) {
              // We don't know enough about the type and the common path merge will result in
              // Conflict. Fail here knowing the correct thing can be done at runtime.
              Fail(VERIFY_ERROR_BAD_CLASS_SOFT) << "unexpected non-exception class " << exception;
              return reg_types_.Conflict();
            } else if (common_super->Equals(exception)) {
              // odd case, but nothing to do
            } else {
              common_super = &common_super->Merge(exception, &reg_types_);
              CHECK(reg_types_.JavaLangThrowable().IsAssignableFrom(*common_super));
            }
          }
        }
      }
      handlers_ptr = iterator.EndDataPointer();
    }
  }
  if (common_super == NULL) {
    /* no catch blocks, or no catches with classes we can find */
    Fail(VERIFY_ERROR_BAD_CLASS_SOFT) << "unable to find exception handler";
    return reg_types_.Conflict();
  }
  return *common_super;
}

Method* MethodVerifier::ResolveMethodAndCheckAccess(uint32_t dex_method_idx, MethodType method_type) {
  const DexFile::MethodId& method_id = dex_file_->GetMethodId(dex_method_idx);
  const RegType& klass_type = ResolveClassAndCheckAccess(method_id.class_idx_);
  if (klass_type.IsConflict()) {
    std::string append(" in attempt to access method ");
    append += dex_file_->GetMethodName(method_id);
    AppendToLastFailMessage(append);
    return NULL;
  }
  if (klass_type.IsUnresolvedTypes()) {
    return NULL;  // Can't resolve Class so no more to do here
  }
  Class* klass = klass_type.GetClass();
  const RegType& referrer = GetDeclaringClass();
  Method* res_method = dex_cache_->GetResolvedMethod(dex_method_idx);
  if (res_method == NULL) {
    const char* name = dex_file_->GetMethodName(method_id);
    std::string signature(dex_file_->CreateMethodSignature(method_id.proto_idx_, NULL));

    if (method_type == METHOD_DIRECT || method_type == METHOD_STATIC) {
      res_method = klass->FindDirectMethod(name, signature);
    } else if (method_type == METHOD_INTERFACE) {
      res_method = klass->FindInterfaceMethod(name, signature);
    } else {
      res_method = klass->FindVirtualMethod(name, signature);
    }
    if (res_method != NULL) {
      dex_cache_->SetResolvedMethod(dex_method_idx, res_method);
    } else {
      // If a virtual or interface method wasn't found with the expected type, look in
      // the direct methods. This can happen when the wrong invoke type is used or when
      // a class has changed, and will be flagged as an error in later checks.
      if (method_type == METHOD_INTERFACE || method_type == METHOD_VIRTUAL) {
        res_method = klass->FindDirectMethod(name, signature);
      }
      if (res_method == NULL) {
        Fail(VERIFY_ERROR_NO_METHOD) << "couldn't find method "
                                     << PrettyDescriptor(klass) << "." << name
                                     << " " << signature;
        return NULL;
      }
    }
  }
  // Make sure calls to constructors are "direct". There are additional restrictions but we don't
  // enforce them here.
  if (res_method->IsConstructor() && method_type != METHOD_DIRECT) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "rejecting non-direct call to constructor "
                                      << PrettyMethod(res_method);
    return NULL;
  }
  // Disallow any calls to class initializers.
  if (MethodHelper(res_method).IsClassInitializer()) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "rejecting call to class initializer "
                                      << PrettyMethod(res_method);
    return NULL;
  }
  // Check if access is allowed.
  if (!referrer.CanAccessMember(res_method->GetDeclaringClass(), res_method->GetAccessFlags())) {
    Fail(VERIFY_ERROR_ACCESS_METHOD) << "illegal method access (call " << PrettyMethod(res_method)
                                     << " from " << referrer << ")";
    return res_method;
  }
  // Check that invoke-virtual and invoke-super are not used on private methods of the same class.
  if (res_method->IsPrivate() && method_type == METHOD_VIRTUAL) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "invoke-super/virtual can't be used on private method "
                                      << PrettyMethod(res_method);
    return NULL;
  }
  // Check that interface methods match interface classes.
  if (klass->IsInterface() && method_type != METHOD_INTERFACE) {
    Fail(VERIFY_ERROR_CLASS_CHANGE) << "non-interface method " << PrettyMethod(res_method)
                                    << " is in an interface class " << PrettyClass(klass);
    return NULL;
  } else if (!klass->IsInterface() && method_type == METHOD_INTERFACE) {
    Fail(VERIFY_ERROR_CLASS_CHANGE) << "interface method " << PrettyMethod(res_method)
                                    << " is in a non-interface class " << PrettyClass(klass);
    return NULL;
  }
  // See if the method type implied by the invoke instruction matches the access flags for the
  // target method.
  if ((method_type == METHOD_DIRECT && !res_method->IsDirect()) ||
      (method_type == METHOD_STATIC && !res_method->IsStatic()) ||
      ((method_type == METHOD_VIRTUAL || method_type == METHOD_INTERFACE) && res_method->IsDirect())
      ) {
    Fail(VERIFY_ERROR_CLASS_CHANGE) << "invoke type does not match method type of "
                                    << PrettyMethod(res_method);
    return NULL;
  }
  return res_method;
}

Method* MethodVerifier::VerifyInvocationArgs(const DecodedInstruction& dec_insn,
                                             MethodType method_type, bool is_range, bool is_super) {
  // Resolve the method. This could be an abstract or concrete method depending on what sort of call
  // we're making.
  Method* res_method = ResolveMethodAndCheckAccess(dec_insn.vB, method_type);
  if (res_method == NULL) {  // error or class is unresolved
    return NULL;
  }

  // If we're using invoke-super(method), make sure that the executing method's class' superclass
  // has a vtable entry for the target method.
  if (is_super) {
    DCHECK(method_type == METHOD_VIRTUAL);
    const RegType& super = GetDeclaringClass().GetSuperClass(&reg_types_);
    if (super.IsConflict()) {  // unknown super class
      Fail(VERIFY_ERROR_NO_METHOD) << "unknown super class in invoke-super from "
                                   << PrettyMethod(method_idx_, *dex_file_)
                                   << " to super " << PrettyMethod(res_method);
      return NULL;
    }
    Class* super_klass = super.GetClass();
    if (res_method->GetMethodIndex() >= super_klass->GetVTable()->GetLength()) {
      MethodHelper mh(res_method);
      Fail(VERIFY_ERROR_NO_METHOD) << "invalid invoke-super from "
                                   << PrettyMethod(method_idx_, *dex_file_)
                                   << " to super " << super
                                   << "." << mh.GetName()
                                   << mh.GetSignature();
      return NULL;
    }
  }
  // We use vAA as our expected arg count, rather than res_method->insSize, because we need to
  // match the call to the signature. Also, we might might be calling through an abstract method
  // definition (which doesn't have register count values).
  size_t expected_args = dec_insn.vA;
  /* caught by static verifier */
  DCHECK(is_range || expected_args <= 5);
  if (expected_args > code_item_->outs_size_) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "invalid argument count (" << expected_args
        << ") exceeds outsSize (" << code_item_->outs_size_ << ")";
    return NULL;
  }

  /*
   * Check the "this" argument, which must be an instance of the class that declared the method.
   * For an interface class, we don't do the full interface merge (see JoinClass), so we can't do a
   * rigorous check here (which is okay since we have to do it at runtime).
   */
  size_t actual_args = 0;
  if (!res_method->IsStatic()) {
    const RegType& actual_arg_type = work_line_->GetInvocationThis(dec_insn);
    if (actual_arg_type.IsConflict()) {  // GetInvocationThis failed.
      return NULL;
    }
    if (actual_arg_type.IsUninitializedReference() && !res_method->IsConstructor()) {
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "'this' arg must be initialized";
      return NULL;
    }
    if (method_type != METHOD_INTERFACE && !actual_arg_type.IsZero()) {
      const RegType& res_method_class = reg_types_.FromClass(res_method->GetDeclaringClass());
      if (!res_method_class.IsAssignableFrom(actual_arg_type)) {
        Fail(VERIFY_ERROR_BAD_CLASS_SOFT) << "'this' argument '" << actual_arg_type
            << "' not instance of '" << res_method_class << "'";
        return NULL;
      }
    }
    actual_args++;
  }
  /*
   * Process the target method's signature. This signature may or may not
   * have been verified, so we can't assume it's properly formed.
   */
  MethodHelper mh(res_method);
  const DexFile::TypeList* params = mh.GetParameterTypeList();
  size_t params_size = params == NULL ? 0 : params->Size();
  for (size_t param_index = 0; param_index < params_size; param_index++) {
    if (actual_args >= expected_args) {
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "Rejecting invalid call to '" << PrettyMethod(res_method)
          << "'. Expected " << expected_args << " arguments, processing argument " << actual_args
          << " (where longs/doubles count twice).";
      return NULL;
    }
    const char* descriptor =
        mh.GetTypeDescriptorFromTypeIdx(params->GetTypeItem(param_index).type_idx_);
    if (descriptor == NULL) {
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "Rejecting invocation of " << PrettyMethod(res_method)
          << " missing signature component";
      return NULL;
    }
    const RegType& reg_type = reg_types_.FromDescriptor(class_loader_, descriptor);
    uint32_t get_reg = is_range ? dec_insn.vC + actual_args : dec_insn.arg[actual_args];
    if (!work_line_->VerifyRegisterType(get_reg, reg_type)) {
      return res_method;
    }
    actual_args = reg_type.IsLongOrDoubleTypes() ? actual_args + 2 : actual_args + 1;
  }
  if (actual_args != expected_args) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "Rejecting invocation of " << PrettyMethod(res_method)
        << " expected " << expected_args << " arguments, found " << actual_args;
    return NULL;
  } else {
    return res_method;
  }
}

void MethodVerifier::VerifyNewArray(const DecodedInstruction& dec_insn, bool is_filled,
                                 bool is_range) {
  const RegType& res_type = ResolveClassAndCheckAccess(is_filled ? dec_insn.vB : dec_insn.vC);
  if (res_type.IsConflict()) {  // bad class
    DCHECK_NE(failures_.size(), 0U);
  } else {
    // TODO: check Compiler::CanAccessTypeWithoutChecks returns false when res_type is unresolved
    if (!res_type.IsArrayTypes()) {
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "new-array on non-array class " << res_type;
    } else if (!is_filled) {
      /* make sure "size" register is valid type */
      work_line_->VerifyRegisterType(dec_insn.vB, reg_types_.Integer());
      /* set register type to array class */
      work_line_->SetRegisterType(dec_insn.vA, res_type);
    } else {
      // Verify each register. If "arg_count" is bad, VerifyRegisterType() will run off the end of
      // the list and fail. It's legal, if silly, for arg_count to be zero.
      const RegType& expected_type = reg_types_.GetComponentType(res_type, class_loader_);
      uint32_t arg_count = dec_insn.vA;
      for (size_t ui = 0; ui < arg_count; ui++) {
        uint32_t get_reg = is_range ? dec_insn.vC + ui : dec_insn.arg[ui];
        if (!work_line_->VerifyRegisterType(get_reg, expected_type)) {
          work_line_->SetResultRegisterType(reg_types_.Conflict());
          return;
        }
      }
      // filled-array result goes into "result" register
      work_line_->SetResultRegisterType(res_type);
    }
  }
}

void MethodVerifier::VerifyAGet(const DecodedInstruction& dec_insn,
                             const RegType& insn_type, bool is_primitive) {
  const RegType& index_type = work_line_->GetRegisterType(dec_insn.vC);
  if (!index_type.IsArrayIndexTypes()) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "Invalid reg type for array index (" << index_type << ")";
  } else {
    const RegType& array_type = work_line_->GetRegisterType(dec_insn.vB);
    if (array_type.IsZero()) {
      // Null array class; this code path will fail at runtime. Infer a merge-able type from the
      // instruction type. TODO: have a proper notion of bottom here.
      if (!is_primitive || insn_type.IsCategory1Types()) {
        // Reference or category 1
        work_line_->SetRegisterType(dec_insn.vA, reg_types_.Zero());
      } else {
        // Category 2
        work_line_->SetRegisterType(dec_insn.vA, reg_types_.ConstLo());
      }
    } else if (!array_type.IsArrayTypes()) {
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "not array type " << array_type << " with aget";
    } else {
      /* verify the class */
      const RegType& component_type = reg_types_.GetComponentType(array_type, class_loader_);
      if (!component_type.IsReferenceTypes() && !is_primitive) {
        Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "primitive array type " << array_type
            << " source for aget-object";
      } else if (component_type.IsNonZeroReferenceTypes() && is_primitive) {
        Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "reference array type " << array_type
            << " source for category 1 aget";
      } else if (is_primitive && !insn_type.Equals(component_type) &&
                 !((insn_type.IsInteger() && component_type.IsFloat()) ||
                   (insn_type.IsLong() && component_type.IsDouble()))) {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "array type " << array_type
              << " incompatible with aget of type " << insn_type;
      } else {
          // Use knowledge of the field type which is stronger than the type inferred from the
          // instruction, which can't differentiate object types and ints from floats, longs from
          // doubles.
          work_line_->SetRegisterType(dec_insn.vA, component_type);
      }
    }
  }
}

void MethodVerifier::VerifyAPut(const DecodedInstruction& dec_insn,
                             const RegType& insn_type, bool is_primitive) {
  const RegType& index_type = work_line_->GetRegisterType(dec_insn.vC);
  if (!index_type.IsArrayIndexTypes()) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "Invalid reg type for array index (" << index_type << ")";
  } else {
    const RegType& array_type = work_line_->GetRegisterType(dec_insn.vB);
    if (array_type.IsZero()) {
      // Null array type; this code path will fail at runtime. Infer a merge-able type from the
      // instruction type.
    } else if (!array_type.IsArrayTypes()) {
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "not array type " << array_type << " with aput";
    } else {
      /* verify the class */
      const RegType& component_type = reg_types_.GetComponentType(array_type, class_loader_);
      if (!component_type.IsReferenceTypes() && !is_primitive) {
        Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "primitive array type " << array_type
            << " source for aput-object";
      } else if (component_type.IsNonZeroReferenceTypes() && is_primitive) {
        Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "reference array type " << array_type
            << " source for category 1 aput";
      } else if (is_primitive && !insn_type.Equals(component_type) &&
                 !((insn_type.IsInteger() && component_type.IsFloat()) ||
                   (insn_type.IsLong() && component_type.IsDouble()))) {
        Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "array type " << array_type
            << " incompatible with aput of type " << insn_type;
      } else {
        // The instruction agrees with the type of array, confirm the value to be stored does too
        // Note: we use the instruction type (rather than the component type) for aput-object as
        // incompatible classes will be caught at runtime as an array store exception
        work_line_->VerifyRegisterType(dec_insn.vA, is_primitive ? component_type : insn_type);
      }
    }
  }
}

Field* MethodVerifier::GetStaticField(int field_idx) {
  const DexFile::FieldId& field_id = dex_file_->GetFieldId(field_idx);
  // Check access to class
  const RegType& klass_type = ResolveClassAndCheckAccess(field_id.class_idx_);
  if (klass_type.IsConflict()) { // bad class
    AppendToLastFailMessage(StringPrintf(" in attempt to access static field %d (%s) in %s",
                                         field_idx, dex_file_->GetFieldName(field_id),
                                         dex_file_->GetFieldDeclaringClassDescriptor(field_id)));
    return NULL;
  }
  if (klass_type.IsUnresolvedTypes()) {
    return NULL;  // Can't resolve Class so no more to do here, will do checking at runtime.
  }
  Field* field = Runtime::Current()->GetClassLinker()->ResolveFieldJLS(*dex_file_, field_idx,
                                                                       dex_cache_, class_loader_);
  if (field == NULL) {
    LOG(INFO) << "unable to resolve static field " << field_idx << " ("
              << dex_file_->GetFieldName(field_id) << ") in "
              << dex_file_->GetFieldDeclaringClassDescriptor(field_id);
    DCHECK(Thread::Current()->IsExceptionPending());
    Thread::Current()->ClearException();
    return NULL;
  } else if (!GetDeclaringClass().CanAccessMember(field->GetDeclaringClass(),
                                                  field->GetAccessFlags())) {
    Fail(VERIFY_ERROR_ACCESS_FIELD) << "cannot access static field " << PrettyField(field)
                                    << " from " << GetDeclaringClass();
    return NULL;
  } else if (!field->IsStatic()) {
    Fail(VERIFY_ERROR_CLASS_CHANGE) << "expected field " << PrettyField(field) << " to be static";
    return NULL;
  } else {
    return field;
  }
}

Field* MethodVerifier::GetInstanceField(const RegType& obj_type, int field_idx) {
  const DexFile::FieldId& field_id = dex_file_->GetFieldId(field_idx);
  // Check access to class
  const RegType& klass_type = ResolveClassAndCheckAccess(field_id.class_idx_);
  if (klass_type.IsConflict()) {
    AppendToLastFailMessage(StringPrintf(" in attempt to access instance field %d (%s) in %s",
                                         field_idx, dex_file_->GetFieldName(field_id),
                                         dex_file_->GetFieldDeclaringClassDescriptor(field_id)));
    return NULL;
  }
  if (klass_type.IsUnresolvedTypes()) {
    return NULL;  // Can't resolve Class so no more to do here
  }
  Field* field = Runtime::Current()->GetClassLinker()->ResolveFieldJLS(*dex_file_, field_idx,
                                                                       dex_cache_, class_loader_);
  if (field == NULL) {
    LOG(INFO) << "unable to resolve instance field " << field_idx << " ("
              << dex_file_->GetFieldName(field_id) << ") in "
              << dex_file_->GetFieldDeclaringClassDescriptor(field_id);
    DCHECK(Thread::Current()->IsExceptionPending());
    Thread::Current()->ClearException();
    return NULL;
  } else if (!GetDeclaringClass().CanAccessMember(field->GetDeclaringClass(),
                                                  field->GetAccessFlags())) {
    Fail(VERIFY_ERROR_ACCESS_FIELD) << "cannot access instance field " << PrettyField(field)
                                    << " from " << GetDeclaringClass();
    return NULL;
  } else if (field->IsStatic()) {
    Fail(VERIFY_ERROR_CLASS_CHANGE) << "expected field " << PrettyField(field)
                                    << " to not be static";
    return NULL;
  } else if (obj_type.IsZero()) {
    // Cannot infer and check type, however, access will cause null pointer exception
    return field;
  } else {
    const RegType& field_klass = reg_types_.FromClass(field->GetDeclaringClass());
    if (obj_type.IsUninitializedTypes() &&
        (!IsConstructor() || GetDeclaringClass().Equals(obj_type) ||
            !field_klass.Equals(GetDeclaringClass()))) {
      // Field accesses through uninitialized references are only allowable for constructors where
      // the field is declared in this class
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "cannot access instance field " << PrettyField(field)
                                        << " of a not fully initialized object within the context of "
                                        << PrettyMethod(method_idx_, *dex_file_);
      return NULL;
    } else if (!field_klass.IsAssignableFrom(obj_type)) {
      // Trying to access C1.field1 using reference of type C2, which is neither C1 or a sub-class
      // of C1. For resolution to occur the declared class of the field must be compatible with
      // obj_type, we've discovered this wasn't so, so report the field didn't exist.
      Fail(VERIFY_ERROR_NO_FIELD) << "cannot access instance field " << PrettyField(field)
                                  << " from object of type " << obj_type;
      return NULL;
    } else {
      return field;
    }
  }
}

void MethodVerifier::VerifyISGet(const DecodedInstruction& dec_insn,
                              const RegType& insn_type, bool is_primitive, bool is_static) {
  uint32_t field_idx = is_static ? dec_insn.vB : dec_insn.vC;
  Field* field;
  if (is_static) {
    field = GetStaticField(field_idx);
  } else {
    const RegType& object_type = work_line_->GetRegisterType(dec_insn.vB);
    field = GetInstanceField(object_type, field_idx);
  }
  const char* descriptor;
  const ClassLoader* loader;
  if (field != NULL) {
    descriptor = FieldHelper(field).GetTypeDescriptor();
    loader = field->GetDeclaringClass()->GetClassLoader();
  } else {
    const DexFile::FieldId& field_id = dex_file_->GetFieldId(field_idx);
    descriptor = dex_file_->GetFieldTypeDescriptor(field_id);
    loader = class_loader_;
  }
  const RegType& field_type = reg_types_.FromDescriptor(loader, descriptor);
  if (is_primitive) {
    if (field_type.Equals(insn_type) ||
        (field_type.IsFloat() && insn_type.IsIntegralTypes()) ||
        (field_type.IsDouble() && insn_type.IsLongTypes())) {
      // expected that read is of the correct primitive type or that int reads are reading
      // floats or long reads are reading doubles
    } else {
      // This is a global failure rather than a class change failure as the instructions and
      // the descriptors for the type should have been consistent within the same file at
      // compile time
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "expected field " << PrettyField(field)
                                        << " to be of type '" << insn_type
                                        << "' but found type '" << field_type << "' in get";
      return;
    }
  } else {
    if (!insn_type.IsAssignableFrom(field_type)) {
      Fail(VERIFY_ERROR_BAD_CLASS_SOFT) << "expected field " << PrettyField(field)
                                        << " to be compatible with type '" << insn_type
                                        << "' but found type '" << field_type
                                        << "' in get-object";
      work_line_->SetRegisterType(dec_insn.vA, reg_types_.Conflict());
      return;
    }
  }
  work_line_->SetRegisterType(dec_insn.vA, field_type);
}

void MethodVerifier::VerifyISPut(const DecodedInstruction& dec_insn,
                              const RegType& insn_type, bool is_primitive, bool is_static) {
  uint32_t field_idx = is_static ? dec_insn.vB : dec_insn.vC;
  Field* field;
  if (is_static) {
    field = GetStaticField(field_idx);
  } else {
    const RegType& object_type = work_line_->GetRegisterType(dec_insn.vB);
    field = GetInstanceField(object_type, field_idx);
  }
  const char* descriptor;
  const ClassLoader* loader;
  if (field != NULL) {
    descriptor = FieldHelper(field).GetTypeDescriptor();
    loader = field->GetDeclaringClass()->GetClassLoader();
  } else {
    const DexFile::FieldId& field_id = dex_file_->GetFieldId(field_idx);
    descriptor = dex_file_->GetFieldTypeDescriptor(field_id);
    loader = class_loader_;
  }
  const RegType& field_type = reg_types_.FromDescriptor(loader, descriptor);
  if (field != NULL) {
    if (field->IsFinal() && field->GetDeclaringClass() != GetDeclaringClass().GetClass()) {
      Fail(VERIFY_ERROR_ACCESS_FIELD) << "cannot modify final field " << PrettyField(field)
                                      << " from other class " << GetDeclaringClass();
      return;
    }
  }
  if (is_primitive) {
    // Primitive field assignability rules are weaker than regular assignability rules
    bool instruction_compatible;
    bool value_compatible;
    const RegType& value_type = work_line_->GetRegisterType(dec_insn.vA);
    if (field_type.IsIntegralTypes()) {
      instruction_compatible = insn_type.IsIntegralTypes();
      value_compatible = value_type.IsIntegralTypes();
    } else if (field_type.IsFloat()) {
      instruction_compatible = insn_type.IsInteger();  // no [is]put-float, so expect [is]put-int
      value_compatible = value_type.IsFloatTypes();
    } else if (field_type.IsLong()) {
      instruction_compatible = insn_type.IsLong();
      value_compatible = value_type.IsLongTypes();
    } else if (field_type.IsDouble()) {
      instruction_compatible = insn_type.IsLong();  // no [is]put-double, so expect [is]put-long
      value_compatible = value_type.IsDoubleTypes();
    } else {
      instruction_compatible = false;  // reference field with primitive store
      value_compatible = false;  // unused
    }
    if (!instruction_compatible) {
      // This is a global failure rather than a class change failure as the instructions and
      // the descriptors for the type should have been consistent within the same file at
      // compile time
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "expected field " << PrettyField(field)
                                        << " to be of type '" << insn_type
                                        << "' but found type '" << field_type
                                        << "' in put";
      return;
    }
    if (!value_compatible) {
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "unexpected value in v" << dec_insn.vA
          << " of type " << value_type
          << " but expected " << field_type
          << " for store to " << PrettyField(field) << " in put";
      return;
    }
  } else {
    if (!insn_type.IsAssignableFrom(field_type)) {
      Fail(VERIFY_ERROR_BAD_CLASS_SOFT) << "expected field " << PrettyField(field)
                                        << " to be compatible with type '" << insn_type
                                        << "' but found type '" << field_type
                                        << "' in put-object";
      return;
    }
    work_line_->VerifyRegisterType(dec_insn.vA, field_type);
  }
}

bool MethodVerifier::CheckNotMoveException(const uint16_t* insns, int insn_idx) {
  if ((insns[insn_idx] & 0xff) == Instruction::MOVE_EXCEPTION) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "invalid use of move-exception";
    return false;
  }
  return true;
}

void MethodVerifier::ReplaceFailingInstruction() {
  // Pop the failure and clear the need for rewriting.
  size_t failure_number = failures_.size();
  CHECK_NE(failure_number, 0U);
  DCHECK_EQ(failure_messages_.size(), failure_number);
  std::ostringstream* failure_message = failure_messages_[0];
  VerifyError failure = failures_[0];
  failures_.clear();
  failure_messages_.clear();
  have_pending_rewrite_failure_ = false;

  if (Runtime::Current()->IsStarted()) {
    LOG(ERROR) << "Verification attempting to replace instructions at runtime in "
               << PrettyMethod(method_idx_, *dex_file_) << " " << failure_message->str();
    return;
  }
  const Instruction* inst = Instruction::At(code_item_->insns_ + work_insn_idx_);
  DCHECK(inst->IsThrow()) << "Expected instruction that will throw " << inst->Name();
  VerifyErrorRefType ref_type;
  switch (inst->Opcode()) {
    case Instruction::CONST_CLASS:            // insn[1] == class ref, 2 code units (4 bytes)
    case Instruction::CHECK_CAST:
    case Instruction::INSTANCE_OF:
    case Instruction::NEW_INSTANCE:
    case Instruction::NEW_ARRAY:
    case Instruction::FILLED_NEW_ARRAY:       // insn[1] == class ref, 3 code units (6 bytes)
    case Instruction::FILLED_NEW_ARRAY_RANGE:
      ref_type = VERIFY_ERROR_REF_CLASS;
      break;
    case Instruction::IGET:                   // insn[1] == field ref, 2 code units (4 bytes)
    case Instruction::IGET_BOOLEAN:
    case Instruction::IGET_BYTE:
    case Instruction::IGET_CHAR:
    case Instruction::IGET_SHORT:
    case Instruction::IGET_WIDE:
    case Instruction::IGET_OBJECT:
    case Instruction::IPUT:
    case Instruction::IPUT_BOOLEAN:
    case Instruction::IPUT_BYTE:
    case Instruction::IPUT_CHAR:
    case Instruction::IPUT_SHORT:
    case Instruction::IPUT_WIDE:
    case Instruction::IPUT_OBJECT:
    case Instruction::SGET:
    case Instruction::SGET_BOOLEAN:
    case Instruction::SGET_BYTE:
    case Instruction::SGET_CHAR:
    case Instruction::SGET_SHORT:
    case Instruction::SGET_WIDE:
    case Instruction::SGET_OBJECT:
    case Instruction::SPUT:
    case Instruction::SPUT_BOOLEAN:
    case Instruction::SPUT_BYTE:
    case Instruction::SPUT_CHAR:
    case Instruction::SPUT_SHORT:
    case Instruction::SPUT_WIDE:
    case Instruction::SPUT_OBJECT:
      ref_type = VERIFY_ERROR_REF_FIELD;
      break;
    case Instruction::INVOKE_VIRTUAL:         // insn[1] == method ref, 3 code units (6 bytes)
    case Instruction::INVOKE_VIRTUAL_RANGE:
    case Instruction::INVOKE_SUPER:
    case Instruction::INVOKE_SUPER_RANGE:
    case Instruction::INVOKE_DIRECT:
    case Instruction::INVOKE_DIRECT_RANGE:
    case Instruction::INVOKE_STATIC:
    case Instruction::INVOKE_STATIC_RANGE:
    case Instruction::INVOKE_INTERFACE:
    case Instruction::INVOKE_INTERFACE_RANGE:
      ref_type = VERIFY_ERROR_REF_METHOD;
      break;
    default:
      LOG(FATAL) << "Error: verifier asked to replace instruction " << inst->DumpString(dex_file_);
      return;
  }
  uint16_t* insns = const_cast<uint16_t*>(code_item_->insns_);
  // THROW_VERIFICATION_ERROR is a 2 code unit instruction. We shouldn't be rewriting a 1 code unit
  // instruction, so assert it.
  size_t width = inst->SizeInCodeUnits();
  CHECK_GT(width, 1u);
  // If the instruction is larger than 2 code units, rewrite subsequent code unit sized chunks with
  // NOPs
  for (size_t i = 2; i < width; i++) {
    insns[work_insn_idx_ + i] = Instruction::NOP;
  }
  // Encode the opcode, with the failure code in the high byte
  uint16_t new_instruction = Instruction::THROW_VERIFICATION_ERROR |
                             (failure << 8) |  // AA - component
                             (ref_type << (8 + kVerifyErrorRefTypeShift));
  insns[work_insn_idx_] = new_instruction;
  // The 2nd code unit (higher in memory) with the reference in, comes from the instruction we
  // rewrote, so nothing to do here.
  LOG(INFO) << "Verification error, replacing instructions in "
            << PrettyMethod(method_idx_, *dex_file_) << " "
            << failure_message->str();
  if (gDebugVerify) {
    std::cout << "\n" << info_messages_.str();
    Dump(std::cout);
  }
}

bool MethodVerifier::UpdateRegisters(uint32_t next_insn, const RegisterLine* merge_line) {
  bool changed = true;
  RegisterLine* target_line = reg_table_.GetLine(next_insn);
  if (!insn_flags_[next_insn].IsVisitedOrChanged()) {
    /*
     * We haven't processed this instruction before, and we haven't touched the registers here, so
     * there's nothing to "merge". Copy the registers over and mark it as changed. (This is the
     * only way a register can transition out of "unknown", so this is not just an optimization.)
     */
    target_line->CopyFromLine(merge_line);
  } else {
    UniquePtr<RegisterLine> copy(gDebugVerify ? new RegisterLine(target_line->NumRegs(), this) : NULL);
    if (gDebugVerify) {
      copy->CopyFromLine(target_line);
    }
    changed = target_line->MergeRegisters(merge_line);
    if (have_pending_hard_failure_) {
      return false;
    }
    if (gDebugVerify && changed) {
      LogVerifyInfo() << "Merging at [" << reinterpret_cast<void*>(work_insn_idx_) << "]"
                      << " to [" << reinterpret_cast<void*>(next_insn) << "]: " << "\n"
                      << *copy.get() << "  MERGE\n"
                      << *merge_line << "  ==\n"
                      << *target_line << "\n";
    }
  }
  if (changed) {
    insn_flags_[next_insn].SetChanged();
  }
  return true;
}

InsnFlags* MethodVerifier::CurrentInsnFlags() {
  return &insn_flags_[work_insn_idx_];
}

const RegType& MethodVerifier::GetMethodReturnType() {
  const DexFile::MethodId& method_id = dex_file_->GetMethodId(method_idx_);
  const DexFile::ProtoId& proto_id = dex_file_->GetMethodPrototype(method_id);
  uint16_t return_type_idx = proto_id.return_type_idx_;
  const char* descriptor = dex_file_->GetTypeDescriptor(dex_file_->GetTypeId(return_type_idx));
  return reg_types_.FromDescriptor(class_loader_, descriptor);
}

const RegType& MethodVerifier::GetDeclaringClass() {
  if (foo_method_ != NULL) {
    return reg_types_.FromClass(foo_method_->GetDeclaringClass());
  } else {
    const DexFile::MethodId& method_id = dex_file_->GetMethodId(method_idx_);
    const char* descriptor = dex_file_->GetTypeDescriptor(dex_file_->GetTypeId(method_id.class_idx_));
    return reg_types_.FromDescriptor(class_loader_, descriptor);
  }
}

void MethodVerifier::ComputeGcMapSizes(size_t* gc_points, size_t* ref_bitmap_bits,
                                    size_t* log2_max_gc_pc) {
  size_t local_gc_points = 0;
  size_t max_insn = 0;
  size_t max_ref_reg = -1;
  for (size_t i = 0; i < code_item_->insns_size_in_code_units_; i++) {
    if (insn_flags_[i].IsGcPoint()) {
      local_gc_points++;
      max_insn = i;
      RegisterLine* line = reg_table_.GetLine(i);
      max_ref_reg = line->GetMaxNonZeroReferenceReg(max_ref_reg);
    }
  }
  *gc_points = local_gc_points;
  *ref_bitmap_bits = max_ref_reg + 1;  // if max register is 0 we need 1 bit to encode (ie +1)
  size_t i = 0;
  while ((1U << i) <= max_insn) {
    i++;
  }
  *log2_max_gc_pc = i;
}

const std::vector<uint8_t>* MethodVerifier::GenerateGcMap() {
  size_t num_entries, ref_bitmap_bits, pc_bits;
  ComputeGcMapSizes(&num_entries, &ref_bitmap_bits, &pc_bits);
  // There's a single byte to encode the size of each bitmap
  if (ref_bitmap_bits >= (8 /* bits per byte */ * 8192 /* 13-bit size */ )) {
    // TODO: either a better GC map format or per method failures
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "Cannot encode GC map for method with "
       << ref_bitmap_bits << " registers";
    return NULL;
  }
  size_t ref_bitmap_bytes = (ref_bitmap_bits + 7) / 8;
  // There are 2 bytes to encode the number of entries
  if (num_entries >= 65536) {
    // TODO: either a better GC map format or per method failures
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "Cannot encode GC map for method with "
       << num_entries << " entries";
    return NULL;
  }
  size_t pc_bytes;
  RegisterMapFormat format;
  if (pc_bits <= 8) {
    format = kRegMapFormatCompact8;
    pc_bytes = 1;
  } else if (pc_bits <= 16) {
    format = kRegMapFormatCompact16;
    pc_bytes = 2;
  } else {
    // TODO: either a better GC map format or per method failures
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "Cannot encode GC map for method with "
       << (1 << pc_bits) << " instructions (number is rounded up to nearest power of 2)";
    return NULL;
  }
  size_t table_size = ((pc_bytes + ref_bitmap_bytes) * num_entries) + 4;
  std::vector<uint8_t>* table = new std::vector<uint8_t>;
  if (table == NULL) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "Failed to encode GC map (size=" << table_size << ")";
    return NULL;
  }
  // Write table header
  table->push_back(format | ((ref_bitmap_bytes >> PcToReferenceMap::kRegMapFormatShift) &
                             ~PcToReferenceMap::kRegMapFormatMask));
  table->push_back(ref_bitmap_bytes & 0xFF);
  table->push_back(num_entries & 0xFF);
  table->push_back((num_entries >> 8) & 0xFF);
  // Write table data
  for (size_t i = 0; i < code_item_->insns_size_in_code_units_; i++) {
    if (insn_flags_[i].IsGcPoint()) {
      table->push_back(i & 0xFF);
      if (pc_bytes == 2) {
        table->push_back((i >> 8) & 0xFF);
      }
      RegisterLine* line = reg_table_.GetLine(i);
      line->WriteReferenceBitMap(*table, ref_bitmap_bytes);
    }
  }
  DCHECK_EQ(table->size(), table_size);
  return table;
}

void MethodVerifier::VerifyGcMap(const std::vector<uint8_t>& data) {
  // Check that for every GC point there is a map entry, there aren't entries for non-GC points,
  // that the table data is well formed and all references are marked (or not) in the bitmap
  PcToReferenceMap map(&data[0], data.size());
  size_t map_index = 0;
  for (size_t i = 0; i < code_item_->insns_size_in_code_units_; i++) {
    const uint8_t* reg_bitmap = map.FindBitMap(i, false);
    if (insn_flags_[i].IsGcPoint()) {
      CHECK_LT(map_index, map.NumEntries());
      CHECK_EQ(map.GetPC(map_index), i);
      CHECK_EQ(map.GetBitMap(map_index), reg_bitmap);
      map_index++;
      RegisterLine* line = reg_table_.GetLine(i);
      for (size_t j = 0; j < code_item_->registers_size_; j++) {
        if (line->GetRegisterType(j).IsNonZeroReferenceTypes()) {
          CHECK_LT(j / 8, map.RegWidth());
          CHECK_EQ((reg_bitmap[j / 8] >> (j % 8)) & 1, 1);
        } else if ((j / 8) < map.RegWidth()) {
          CHECK_EQ((reg_bitmap[j / 8] >> (j % 8)) & 1, 0);
        } else {
          // If a register doesn't contain a reference then the bitmap may be shorter than the line
        }
      }
    } else {
      CHECK(reg_bitmap == NULL);
    }
  }
}

void MethodVerifier::SetGcMap(Compiler::MethodReference ref, const std::vector<uint8_t>& gc_map) {
  MutexLock mu(*gc_maps_lock_);
  GcMapTable::iterator it = gc_maps_->find(ref);
  if (it != gc_maps_->end()) {
    delete it->second;
    gc_maps_->erase(it);
  }
  gc_maps_->Put(ref, &gc_map);
  CHECK(GetGcMap(ref) != NULL);
}

const std::vector<uint8_t>* MethodVerifier::GetGcMap(Compiler::MethodReference ref) {
  MutexLock mu(*gc_maps_lock_);
  GcMapTable::const_iterator it = gc_maps_->find(ref);
  if (it == gc_maps_->end()) {
    return NULL;
  }
  CHECK(it->second != NULL);
  return it->second;
}

Mutex* MethodVerifier::gc_maps_lock_ = NULL;
MethodVerifier::GcMapTable* MethodVerifier::gc_maps_ = NULL;

Mutex* MethodVerifier::rejected_classes_lock_ = NULL;
MethodVerifier::RejectedClassesTable* MethodVerifier::rejected_classes_ = NULL;

#if defined(ART_USE_LLVM_COMPILER) || defined(ART_USE_GREENLAND_COMPILER)
Mutex* MethodVerifier::inferred_reg_category_maps_lock_ = NULL;
MethodVerifier::InferredRegCategoryMapTable* MethodVerifier::inferred_reg_category_maps_ = NULL;
#endif

void MethodVerifier::Init() {
  gc_maps_lock_ = new Mutex("verifier GC maps lock");
  {
    MutexLock mu(*gc_maps_lock_);
    gc_maps_ = new MethodVerifier::GcMapTable;
  }

  rejected_classes_lock_ = new Mutex("verifier rejected classes lock");
  {
    MutexLock mu(*rejected_classes_lock_);
    rejected_classes_ = new MethodVerifier::RejectedClassesTable;
  }

#if defined(ART_USE_LLVM_COMPILER) || defined(ART_USE_GREENLAND_COMPILER)
  inferred_reg_category_maps_lock_ = new Mutex("verifier GC maps lock");
  {
    MutexLock mu(*inferred_reg_category_maps_lock_);
    inferred_reg_category_maps_ = new MethodVerifier::InferredRegCategoryMapTable;
  }
#endif
}

void MethodVerifier::Shutdown() {
  {
    MutexLock mu(*gc_maps_lock_);
    STLDeleteValues(gc_maps_);
    delete gc_maps_;
    gc_maps_ = NULL;
  }
  delete gc_maps_lock_;
  gc_maps_lock_ = NULL;

  {
    MutexLock mu(*rejected_classes_lock_);
    delete rejected_classes_;
    rejected_classes_ = NULL;
  }
  delete rejected_classes_lock_;
  rejected_classes_lock_ = NULL;

#if defined(ART_USE_LLVM_COMPILER) || defined(ART_USE_GREENLAND_COMPILER)
  {
    MutexLock mu(*inferred_reg_category_maps_lock_);
    STLDeleteValues(inferred_reg_category_maps_);
    delete inferred_reg_category_maps_;
    inferred_reg_category_maps_ = NULL;
  }
  delete inferred_reg_category_maps_lock_;
  inferred_reg_category_maps_lock_ = NULL;
#endif
}

void MethodVerifier::AddRejectedClass(Compiler::ClassReference ref) {
  MutexLock mu(*rejected_classes_lock_);
  rejected_classes_->insert(ref);
  CHECK(IsClassRejected(ref));
}

bool MethodVerifier::IsClassRejected(Compiler::ClassReference ref) {
  MutexLock mu(*rejected_classes_lock_);
  return (rejected_classes_->find(ref) != rejected_classes_->end());
}

#if defined(ART_USE_LLVM_COMPILER) || defined(ART_USE_GREENLAND_COMPILER)
const InferredRegCategoryMap* MethodVerifier::GenerateInferredRegCategoryMap() {
  uint32_t insns_size = code_item_->insns_size_in_code_units_;
  uint16_t regs_size = code_item_->registers_size_;

  UniquePtr<InferredRegCategoryMap> table(
    new InferredRegCategoryMap(insns_size, regs_size));

  for (size_t i = 0; i < insns_size; ++i) {
    if (RegisterLine* line = reg_table_.GetLine(i)) {
      const Instruction* inst = Instruction::At(code_item_->insns_ + i);

      // GC points
      if (inst->IsBranch() || inst->IsInvoke()) {
        for (size_t r = 0; r < regs_size; ++r) {
          const RegType &rt = line->GetRegisterType(r);
          if (rt.IsNonZeroReferenceTypes()) {
            table->SetRegCanBeObject(r);
          }
        }
      }

      /* We only use InferredRegCategoryMap in one case */
      if (inst->IsBranch()) {
        for (size_t r = 0; r < regs_size; ++r) {
          const RegType &rt = line->GetRegisterType(r);

          if (rt.IsZero()) {
            table->SetRegCategory(i, r, kRegZero);
          } else if (rt.IsCategory1Types()) {
            table->SetRegCategory(i, r, kRegCat1nr);
          } else if (rt.IsCategory2Types()) {
            table->SetRegCategory(i, r, kRegCat2);
          } else if (rt.IsReferenceTypes()) {
            table->SetRegCategory(i, r, kRegObject);
          } else {
            table->SetRegCategory(i, r, kRegUnknown);
          }
        }
      }
    }
  }

  return table.release();
}

void MethodVerifier::SetInferredRegCategoryMap(Compiler::MethodReference ref,
                                          const InferredRegCategoryMap& inferred_reg_category_map) {
  MutexLock mu(*inferred_reg_category_maps_lock_);
  const InferredRegCategoryMap* existing_inferred_reg_category_map = GetInferredRegCategoryMap(ref);

  if (existing_inferred_reg_category_map != NULL) {
    CHECK(*existing_inferred_reg_category_map == inferred_reg_category_map);
    delete existing_inferred_reg_category_map;
  }

  inferred_reg_category_maps_->Put(ref, &inferred_reg_category_map);
  CHECK(GetInferredRegCategoryMap(ref) != NULL);
}

const InferredRegCategoryMap*
MethodVerifier::GetInferredRegCategoryMap(Compiler::MethodReference ref) {
  MutexLock mu(*inferred_reg_category_maps_lock_);

  InferredRegCategoryMapTable::const_iterator it =
      inferred_reg_category_maps_->find(ref);

  if (it == inferred_reg_category_maps_->end()) {
    return NULL;
  }
  CHECK(it->second != NULL);
  return it->second;
}
#endif

}  // namespace verifier
}  // namespace art
