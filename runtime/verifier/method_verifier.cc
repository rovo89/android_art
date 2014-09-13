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

#include "method_verifier-inl.h"

#include <iostream>

#include "base/logging.h"
#include "base/mutex-inl.h"
#include "class_linker.h"
#include "compiler_callbacks.h"
#include "dex_file-inl.h"
#include "dex_instruction-inl.h"
#include "dex_instruction_visitor.h"
#include "field_helper.h"
#include "gc/accounting/card_table-inl.h"
#include "indenter.h"
#include "intern_table.h"
#include "leb128.h"
#include "method_helper-inl.h"
#include "mirror/art_field-inl.h"
#include "mirror/art_method-inl.h"
#include "mirror/class.h"
#include "mirror/class-inl.h"
#include "mirror/dex_cache-inl.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "reg_type-inl.h"
#include "register_line-inl.h"
#include "runtime.h"
#include "scoped_thread_state_change.h"
#include "handle_scope-inl.h"
#include "verifier/dex_gc_map.h"

namespace art {
namespace verifier {

static constexpr bool kTimeVerifyMethod = !kIsDebugBuild;
static constexpr bool gDebugVerify = false;
// TODO: Add a constant to method_verifier to turn on verbose logging?

void PcToRegisterLineTable::Init(RegisterTrackingMode mode, InstructionFlags* flags,
                                 uint32_t insns_size, uint16_t registers_size,
                                 MethodVerifier* verifier) {
  DCHECK_GT(insns_size, 0U);
  register_lines_.reset(new RegisterLine*[insns_size]());
  size_ = insns_size;
  for (uint32_t i = 0; i < insns_size; i++) {
    bool interesting = false;
    switch (mode) {
      case kTrackRegsAll:
        interesting = flags[i].IsOpcode();
        break;
      case kTrackCompilerInterestPoints:
        interesting = flags[i].IsCompileTimeInfoPoint() || flags[i].IsBranchTarget();
        break;
      case kTrackRegsBranches:
        interesting = flags[i].IsBranchTarget();
        break;
      default:
        break;
    }
    if (interesting) {
      register_lines_[i] = RegisterLine::Create(registers_size, verifier);
    }
  }
}

PcToRegisterLineTable::~PcToRegisterLineTable() {
  for (size_t i = 0; i < size_; i++) {
    delete register_lines_[i];
    if (kIsDebugBuild) {
      register_lines_[i] = nullptr;
    }
  }
}

MethodVerifier::FailureKind MethodVerifier::VerifyClass(Thread* self,
                                                        mirror::Class* klass,
                                                        bool allow_soft_failures,
                                                        std::string* error) {
  if (klass->IsVerified()) {
    return kNoFailure;
  }
  bool early_failure = false;
  std::string failure_message;
  const DexFile& dex_file = klass->GetDexFile();
  const DexFile::ClassDef* class_def = klass->GetClassDef();
  mirror::Class* super = klass->GetSuperClass();
  std::string temp;
  if (super == nullptr && strcmp("Ljava/lang/Object;", klass->GetDescriptor(&temp)) != 0) {
    early_failure = true;
    failure_message = " that has no super class";
  } else if (super != nullptr && super->IsFinal()) {
    early_failure = true;
    failure_message = " that attempts to sub-class final class " + PrettyDescriptor(super);
  } else if (class_def == nullptr) {
    early_failure = true;
    failure_message = " that isn't present in dex file " + dex_file.GetLocation();
  }
  if (early_failure) {
    *error = "Verifier rejected class " + PrettyDescriptor(klass) + failure_message;
    if (Runtime::Current()->IsCompiler()) {
      ClassReference ref(&dex_file, klass->GetDexClassDefIndex());
      Runtime::Current()->GetCompilerCallbacks()->ClassRejected(ref);
    }
    return kHardFailure;
  }
  StackHandleScope<2> hs(self);
  Handle<mirror::DexCache> dex_cache(hs.NewHandle(klass->GetDexCache()));
  Handle<mirror::ClassLoader> class_loader(hs.NewHandle(klass->GetClassLoader()));
  return VerifyClass(self, &dex_file, dex_cache, class_loader, class_def, allow_soft_failures, error);
}

MethodVerifier::FailureKind MethodVerifier::VerifyClass(Thread* self,
                                                        const DexFile* dex_file,
                                                        ConstHandle<mirror::DexCache> dex_cache,
                                                        ConstHandle<mirror::ClassLoader> class_loader,
                                                        const DexFile::ClassDef* class_def,
                                                        bool allow_soft_failures,
                                                        std::string* error) {
  DCHECK(class_def != nullptr);
  const byte* class_data = dex_file->GetClassData(*class_def);
  if (class_data == nullptr) {
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
  int64_t previous_direct_method_idx = -1;
  while (it.HasNextDirectMethod()) {
    self->AllowThreadSuspension();
    uint32_t method_idx = it.GetMemberIndex();
    if (method_idx == previous_direct_method_idx) {
      // smali can create dex files with two encoded_methods sharing the same method_idx
      // http://code.google.com/p/smali/issues/detail?id=119
      it.Next();
      continue;
    }
    previous_direct_method_idx = method_idx;
    InvokeType type = it.GetMethodInvokeType(*class_def);
    mirror::ArtMethod* method =
        linker->ResolveMethod(*dex_file, method_idx, dex_cache, class_loader,
                              NullHandle<mirror::ArtMethod>(), type);
    if (method == nullptr) {
      DCHECK(self->IsExceptionPending());
      // We couldn't resolve the method, but continue regardless.
      self->ClearException();
    }
    StackHandleScope<1> hs(self);
    Handle<mirror::ArtMethod> h_method(hs.NewHandle(method));
    MethodVerifier::FailureKind result = VerifyMethod(self,
                                                      method_idx,
                                                      dex_file,
                                                      dex_cache,
                                                      class_loader,
                                                      class_def,
                                                      it.GetMethodCodeItem(),
                                                      h_method,
                                                      it.GetMethodAccessFlags(),
                                                      allow_soft_failures,
                                                      false);
    if (result != kNoFailure) {
      if (result == kHardFailure) {
        hard_fail = true;
        if (error_count > 0) {
          *error += "\n";
        }
        *error = "Verifier rejected class ";
        *error += PrettyDescriptor(dex_file->GetClassDescriptor(*class_def));
        *error += " due to bad method ";
        *error += PrettyMethod(method_idx, *dex_file);
      }
      ++error_count;
    }
    it.Next();
  }
  int64_t previous_virtual_method_idx = -1;
  while (it.HasNextVirtualMethod()) {
    self->AllowThreadSuspension();
    uint32_t method_idx = it.GetMemberIndex();
    if (method_idx == previous_virtual_method_idx) {
      // smali can create dex files with two encoded_methods sharing the same method_idx
      // http://code.google.com/p/smali/issues/detail?id=119
      it.Next();
      continue;
    }
    previous_virtual_method_idx = method_idx;
    InvokeType type = it.GetMethodInvokeType(*class_def);
    mirror::ArtMethod* method =
        linker->ResolveMethod(*dex_file, method_idx, dex_cache, class_loader,
                              NullHandle<mirror::ArtMethod>(), type);
    if (method == nullptr) {
      DCHECK(self->IsExceptionPending());
      // We couldn't resolve the method, but continue regardless.
      self->ClearException();
    }
    StackHandleScope<1> hs(self);
    Handle<mirror::ArtMethod> h_method(hs.NewHandle(method));
    MethodVerifier::FailureKind result = VerifyMethod(self,
                                                      method_idx,
                                                      dex_file,
                                                      dex_cache,
                                                      class_loader,
                                                      class_def,
                                                      it.GetMethodCodeItem(),
                                                      h_method,
                                                      it.GetMethodAccessFlags(),
                                                      allow_soft_failures,
                                                      false);
    if (result != kNoFailure) {
      if (result == kHardFailure) {
        hard_fail = true;
        if (error_count > 0) {
          *error += "\n";
        }
        *error = "Verifier rejected class ";
        *error += PrettyDescriptor(dex_file->GetClassDescriptor(*class_def));
        *error += " due to bad method ";
        *error += PrettyMethod(method_idx, *dex_file);
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

MethodVerifier::FailureKind MethodVerifier::VerifyMethod(Thread* self, uint32_t method_idx,
                                                         const DexFile* dex_file,
                                                         ConstHandle<mirror::DexCache> dex_cache,
                                                         ConstHandle<mirror::ClassLoader> class_loader,
                                                         const DexFile::ClassDef* class_def,
                                                         const DexFile::CodeItem* code_item,
                                                         ConstHandle<mirror::ArtMethod> method,
                                                         uint32_t method_access_flags,
                                                         bool allow_soft_failures,
                                                         bool need_precise_constants) {
  MethodVerifier::FailureKind result = kNoFailure;
  uint64_t start_ns = kTimeVerifyMethod ? NanoTime() : 0;

  MethodVerifier verifier(self, dex_file, dex_cache, class_loader, class_def, code_item,
                          method_idx, method, method_access_flags, true, allow_soft_failures,
                          need_precise_constants);
  if (verifier.Verify()) {
    // Verification completed, however failures may be pending that didn't cause the verification
    // to hard fail.
    CHECK(!verifier.have_pending_hard_failure_);
    if (verifier.failures_.size() != 0) {
      if (VLOG_IS_ON(verifier)) {
          verifier.DumpFailures(VLOG_STREAM(verifier) << "Soft verification failures in "
                                << PrettyMethod(method_idx, *dex_file) << "\n");
      }
      result = kSoftFailure;
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
    result = kHardFailure;
  }
  if (kTimeVerifyMethod) {
    uint64_t duration_ns = NanoTime() - start_ns;
    if (duration_ns > MsToNs(100)) {
      LOG(WARNING) << "Verification of " << PrettyMethod(method_idx, *dex_file)
                   << " took " << PrettyDuration(duration_ns);
    }
  }
  return result;
}

MethodVerifier* MethodVerifier::VerifyMethodAndDump(Thread* self, std::ostream& os,
                                                    uint32_t dex_method_idx,
                                                    const DexFile* dex_file,
                                                    ConstHandle<mirror::DexCache> dex_cache,
                                                    ConstHandle<mirror::ClassLoader> class_loader,
                                                    const DexFile::ClassDef* class_def,
                                                    const DexFile::CodeItem* code_item,
                                                    ConstHandle<mirror::ArtMethod> method,
                                                    uint32_t method_access_flags) {
  MethodVerifier* verifier = new MethodVerifier(self, dex_file, dex_cache, class_loader,
                                                class_def, code_item, dex_method_idx, method,
                                                method_access_flags, true, true, true, true);
  verifier->Verify();
  verifier->DumpFailures(os);
  os << verifier->info_messages_.str();
  verifier->Dump(os);

  return verifier;
}

MethodVerifier::MethodVerifier(Thread* self,
                               const DexFile* dex_file, ConstHandle<mirror::DexCache> dex_cache,
                               ConstHandle<mirror::ClassLoader> class_loader,
                               const DexFile::ClassDef* class_def,
                               const DexFile::CodeItem* code_item, uint32_t dex_method_idx,
                               ConstHandle<mirror::ArtMethod> method, uint32_t method_access_flags,
                               bool can_load_classes, bool allow_soft_failures,
                               bool need_precise_constants, bool verify_to_dump)
    : self_(self),
      reg_types_(can_load_classes),
      work_insn_idx_(-1),
      dex_method_idx_(dex_method_idx),
      mirror_method_(method),
      method_access_flags_(method_access_flags),
      return_type_(nullptr),
      dex_file_(dex_file),
      dex_cache_(dex_cache),
      class_loader_(class_loader),
      class_def_(class_def),
      code_item_(code_item),
      declaring_class_(nullptr),
      interesting_dex_pc_(-1),
      monitor_enter_dex_pcs_(nullptr),
      have_pending_hard_failure_(false),
      have_pending_runtime_throw_failure_(false),
      new_instance_count_(0),
      monitor_enter_count_(0),
      can_load_classes_(can_load_classes),
      allow_soft_failures_(allow_soft_failures),
      need_precise_constants_(need_precise_constants),
      has_check_casts_(false),
      has_virtual_or_interface_invokes_(false),
      verify_to_dump_(verify_to_dump) {
  Runtime::Current()->AddMethodVerifier(this);
  DCHECK(class_def != nullptr);
}

MethodVerifier::~MethodVerifier() {
  Runtime::Current()->RemoveMethodVerifier(this);
  STLDeleteElements(&failure_messages_);
}

void MethodVerifier::FindLocksAtDexPc(mirror::ArtMethod* m, uint32_t dex_pc,
                                      std::vector<uint32_t>* monitor_enter_dex_pcs) {
  Thread* self = Thread::Current();
  StackHandleScope<3> hs(self);
  Handle<mirror::DexCache> dex_cache(hs.NewHandle(m->GetDexCache()));
  Handle<mirror::ClassLoader> class_loader(hs.NewHandle(m->GetClassLoader()));
  Handle<mirror::ArtMethod> method(hs.NewHandle(m));
  MethodVerifier verifier(self, m->GetDexFile(), dex_cache, class_loader, &m->GetClassDef(),
                          m->GetCodeItem(), m->GetDexMethodIndex(), method, m->GetAccessFlags(),
                          false, true, false);
  verifier.interesting_dex_pc_ = dex_pc;
  verifier.monitor_enter_dex_pcs_ = monitor_enter_dex_pcs;
  verifier.FindLocksAtDexPc();
}

void MethodVerifier::FindLocksAtDexPc() {
  CHECK(monitor_enter_dex_pcs_ != nullptr);
  CHECK(code_item_ != nullptr);  // This only makes sense for methods with code.

  // Strictly speaking, we ought to be able to get away with doing a subset of the full method
  // verification. In practice, the phase we want relies on data structures set up by all the
  // earlier passes, so we just run the full method verification and bail out early when we've
  // got what we wanted.
  Verify();
}

mirror::ArtField* MethodVerifier::FindAccessedFieldAtDexPc(mirror::ArtMethod* m,
                                                           uint32_t dex_pc) {
  Thread* self = Thread::Current();
  StackHandleScope<3> hs(self);
  Handle<mirror::DexCache> dex_cache(hs.NewHandle(m->GetDexCache()));
  Handle<mirror::ClassLoader> class_loader(hs.NewHandle(m->GetClassLoader()));
  Handle<mirror::ArtMethod> method(hs.NewHandle(m));
  MethodVerifier verifier(self, m->GetDexFile(), dex_cache, class_loader, &m->GetClassDef(),
                          m->GetCodeItem(), m->GetDexMethodIndex(), method, m->GetAccessFlags(),
                          true, true, false);
  return verifier.FindAccessedFieldAtDexPc(dex_pc);
}

mirror::ArtField* MethodVerifier::FindAccessedFieldAtDexPc(uint32_t dex_pc) {
  CHECK(code_item_ != nullptr);  // This only makes sense for methods with code.

  // Strictly speaking, we ought to be able to get away with doing a subset of the full method
  // verification. In practice, the phase we want relies on data structures set up by all the
  // earlier passes, so we just run the full method verification and bail out early when we've
  // got what we wanted.
  bool success = Verify();
  if (!success) {
    return nullptr;
  }
  RegisterLine* register_line = reg_table_.GetLine(dex_pc);
  if (register_line == nullptr) {
    return nullptr;
  }
  const Instruction* inst = Instruction::At(code_item_->insns_ + dex_pc);
  return GetQuickFieldAccess(inst, register_line);
}

mirror::ArtMethod* MethodVerifier::FindInvokedMethodAtDexPc(mirror::ArtMethod* m,
                                                            uint32_t dex_pc) {
  Thread* self = Thread::Current();
  StackHandleScope<3> hs(self);
  Handle<mirror::DexCache> dex_cache(hs.NewHandle(m->GetDexCache()));
  Handle<mirror::ClassLoader> class_loader(hs.NewHandle(m->GetClassLoader()));
  Handle<mirror::ArtMethod> method(hs.NewHandle(m));
  MethodVerifier verifier(self, m->GetDexFile(), dex_cache, class_loader, &m->GetClassDef(),
                          m->GetCodeItem(), m->GetDexMethodIndex(), method, m->GetAccessFlags(),
                          true, true, false);
  return verifier.FindInvokedMethodAtDexPc(dex_pc);
}

mirror::ArtMethod* MethodVerifier::FindInvokedMethodAtDexPc(uint32_t dex_pc) {
  CHECK(code_item_ != nullptr);  // This only makes sense for methods with code.

  // Strictly speaking, we ought to be able to get away with doing a subset of the full method
  // verification. In practice, the phase we want relies on data structures set up by all the
  // earlier passes, so we just run the full method verification and bail out early when we've
  // got what we wanted.
  bool success = Verify();
  if (!success) {
    return nullptr;
  }
  RegisterLine* register_line = reg_table_.GetLine(dex_pc);
  if (register_line == nullptr) {
    return nullptr;
  }
  const Instruction* inst = Instruction::At(code_item_->insns_ + dex_pc);
  const bool is_range = (inst->Opcode() == Instruction::INVOKE_VIRTUAL_RANGE_QUICK);
  return GetQuickInvokedMethod(inst, register_line, is_range);
}

bool MethodVerifier::Verify() {
  // If there aren't any instructions, make sure that's expected, then exit successfully.
  if (code_item_ == nullptr) {
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
  insn_flags_.reset(new InstructionFlags[code_item_->insns_size_in_code_units_]());
  // Run through the instructions and see if the width checks out.
  bool result = ComputeWidthsAndCountOps();
  // Flag instructions guarded by a "try" block and check exception handlers.
  result = result && ScanTryCatchBlocks();
  // Perform static instruction verification.
  result = result && VerifyInstructions();
  // Perform code-flow analysis and return.
  result = result && VerifyCodeFlow();
  // Compute information for compiler.
  if (result && Runtime::Current()->IsCompiler()) {
    result = Runtime::Current()->GetCompilerCallbacks()->MethodVerified(this);
  }
  return result;
}

std::ostream& MethodVerifier::Fail(VerifyError error) {
  switch (error) {
    case VERIFY_ERROR_NO_CLASS:
    case VERIFY_ERROR_NO_FIELD:
    case VERIFY_ERROR_NO_METHOD:
    case VERIFY_ERROR_ACCESS_CLASS:
    case VERIFY_ERROR_ACCESS_FIELD:
    case VERIFY_ERROR_ACCESS_METHOD:
    case VERIFY_ERROR_INSTANTIATION:
    case VERIFY_ERROR_CLASS_CHANGE:
      if (Runtime::Current()->IsCompiler() || !can_load_classes_) {
        // If we're optimistically running verification at compile time, turn NO_xxx, ACCESS_xxx,
        // class change and instantiation errors into soft verification errors so that we re-verify
        // at runtime. We may fail to find or to agree on access because of not yet available class
        // loaders, or class loaders that will differ at runtime. In these cases, we don't want to
        // affect the soundness of the code being compiled. Instead, the generated code runs "slow
        // paths" that dynamically perform the verification and cause the behavior to be that akin
        // to an interpreter.
        error = VERIFY_ERROR_BAD_CLASS_SOFT;
      } else {
        // If we fail again at runtime, mark that this instruction would throw and force this
        // method to be executed using the interpreter with checks.
        have_pending_runtime_throw_failure_ = true;
      }
      break;
      // Indication that verification should be retried at runtime.
    case VERIFY_ERROR_BAD_CLASS_SOFT:
      if (!allow_soft_failures_) {
        have_pending_hard_failure_ = true;
      }
      break;
      // Hard verification failures at compile time will still fail at runtime, so the class is
      // marked as rejected to prevent it from being compiled.
    case VERIFY_ERROR_BAD_CLASS_HARD: {
      if (Runtime::Current()->IsCompiler()) {
        ClassReference ref(dex_file_, dex_file_->GetIndexForClassDef(*class_def_));
        Runtime::Current()->GetCompilerCallbacks()->ClassRejected(ref);
      }
      have_pending_hard_failure_ = true;
      break;
    }
  }
  failures_.push_back(error);
  std::string location(StringPrintf("%s: [0x%X] ", PrettyMethod(dex_method_idx_, *dex_file_).c_str(),
                                    work_insn_idx_));
  std::ostringstream* failure_message = new std::ostringstream(location, std::ostringstream::ate);
  failure_messages_.push_back(failure_message);
  return *failure_message;
}

std::ostream& MethodVerifier::LogVerifyInfo() {
  return info_messages_ << "VFY: " << PrettyMethod(dex_method_idx_, *dex_file_)
                        << '[' << reinterpret_cast<void*>(work_insn_idx_) << "] : ";
}

void MethodVerifier::PrependToLastFailMessage(std::string prepend) {
  size_t failure_num = failure_messages_.size();
  DCHECK_NE(failure_num, 0U);
  std::ostringstream* last_fail_message = failure_messages_[failure_num - 1];
  prepend += last_fail_message->str();
  failure_messages_[failure_num - 1] = new std::ostringstream(prepend, std::ostringstream::ate);
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
    switch (opcode) {
      case Instruction::APUT_OBJECT:
      case Instruction::CHECK_CAST:
        has_check_casts_ = true;
        break;
      case Instruction::INVOKE_VIRTUAL:
      case Instruction::INVOKE_VIRTUAL_RANGE:
      case Instruction::INVOKE_INTERFACE:
      case Instruction::INVOKE_INTERFACE_RANGE:
        has_virtual_or_interface_invokes_ = true;
        break;
      case Instruction::MONITOR_ENTER:
        monitor_enter_count++;
        break;
      case Instruction::NEW_INSTANCE:
        new_instance_count++;
        break;
      default:
        break;
    }
    size_t inst_size = inst->SizeInCodeUnits();
    insn_flags_[dex_pc].SetIsOpcode();
    dex_pc += inst_size;
    inst = inst->RelativeAt(inst_size);
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
      Fail(VERIFY_ERROR_BAD_CLASS_HARD)
          << "'try' block starts inside an instruction (" << start << ")";
      return false;
    }
    uint32_t dex_pc = start;
    const Instruction* inst = Instruction::At(code_item_->insns_ + dex_pc);
    while (dex_pc < end) {
      insn_flags_[dex_pc].SetInTry();
      size_t insn_size = inst->SizeInCodeUnits();
      dex_pc += insn_size;
      inst = inst->RelativeAt(insn_size);
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
        Fail(VERIFY_ERROR_BAD_CLASS_HARD)
            << "exception handler starts at bad address (" << dex_pc << ")";
        return false;
      }
      insn_flags_[dex_pc].SetBranchTarget();
      // Ensure exception types are resolved so that they don't need resolution to be delivered,
      // unresolved exception types will be ignored by exception delivery
      if (iterator.GetHandlerTypeIndex() != DexFile::kDexNoIndex16) {
        mirror::Class* exception_type = linker->ResolveType(*dex_file_,
                                                            iterator.GetHandlerTypeIndex(),
                                                            dex_cache_, class_loader_);
        if (exception_type == nullptr) {
          DCHECK(self_->IsExceptionPending());
          self_->ClearException();
        }
      }
    }
    handlers_ptr = iterator.EndDataPointer();
  }
  return true;
}

bool MethodVerifier::VerifyInstructions() {
  const Instruction* inst = Instruction::At(code_item_->insns_);

  /* Flag the start of the method as a branch target, and a GC point due to stack overflow errors */
  insn_flags_[0].SetBranchTarget();
  insn_flags_[0].SetCompileTimeInfoPoint();

  uint32_t insns_size = code_item_->insns_size_in_code_units_;
  for (uint32_t dex_pc = 0; dex_pc < insns_size;) {
    if (!VerifyInstruction(inst, dex_pc)) {
      DCHECK_NE(failures_.size(), 0U);
      return false;
    }
    /* Flag instructions that are garbage collection points */
    // All invoke points are marked as "Throw" points already.
    // We are relying on this to also count all the invokes as interesting.
    if (inst->IsBranch() || inst->IsSwitch() || inst->IsThrow()) {
      insn_flags_[dex_pc].SetCompileTimeInfoPoint();
    } else if (inst->IsReturn()) {
      insn_flags_[dex_pc].SetCompileTimeInfoPointAndReturn();
    }
    dex_pc += inst->SizeInCodeUnits();
    inst = inst->Next();
  }
  return true;
}

bool MethodVerifier::VerifyInstruction(const Instruction* inst, uint32_t code_offset) {
  bool result = true;
  switch (inst->GetVerifyTypeArgumentA()) {
    case Instruction::kVerifyRegA:
      result = result && CheckRegisterIndex(inst->VRegA());
      break;
    case Instruction::kVerifyRegAWide:
      result = result && CheckWideRegisterIndex(inst->VRegA());
      break;
  }
  switch (inst->GetVerifyTypeArgumentB()) {
    case Instruction::kVerifyRegB:
      result = result && CheckRegisterIndex(inst->VRegB());
      break;
    case Instruction::kVerifyRegBField:
      result = result && CheckFieldIndex(inst->VRegB());
      break;
    case Instruction::kVerifyRegBMethod:
      result = result && CheckMethodIndex(inst->VRegB());
      break;
    case Instruction::kVerifyRegBNewInstance:
      result = result && CheckNewInstance(inst->VRegB());
      break;
    case Instruction::kVerifyRegBString:
      result = result && CheckStringIndex(inst->VRegB());
      break;
    case Instruction::kVerifyRegBType:
      result = result && CheckTypeIndex(inst->VRegB());
      break;
    case Instruction::kVerifyRegBWide:
      result = result && CheckWideRegisterIndex(inst->VRegB());
      break;
  }
  switch (inst->GetVerifyTypeArgumentC()) {
    case Instruction::kVerifyRegC:
      result = result && CheckRegisterIndex(inst->VRegC());
      break;
    case Instruction::kVerifyRegCField:
      result = result && CheckFieldIndex(inst->VRegC());
      break;
    case Instruction::kVerifyRegCNewArray:
      result = result && CheckNewArray(inst->VRegC());
      break;
    case Instruction::kVerifyRegCType:
      result = result && CheckTypeIndex(inst->VRegC());
      break;
    case Instruction::kVerifyRegCWide:
      result = result && CheckWideRegisterIndex(inst->VRegC());
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
    case Instruction::kVerifyVarArgNonZero:
      // Fall-through.
    case Instruction::kVerifyVarArg: {
      if (inst->GetVerifyExtraFlags() == Instruction::kVerifyVarArgNonZero && inst->VRegA() <= 0) {
        Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "invalid arg count (" << inst->VRegA() << ") in "
                                             "non-range invoke";
        return false;
      }
      uint32_t args[Instruction::kMaxVarArgRegs];
      inst->GetVarArgs(args);
      result = result && CheckVarArgRegs(inst->VRegA(), args);
      break;
    }
    case Instruction::kVerifyVarArgRangeNonZero:
      // Fall-through.
    case Instruction::kVerifyVarArgRange:
      if (inst->GetVerifyExtraFlags() == Instruction::kVerifyVarArgRangeNonZero &&
          inst->VRegA() <= 0) {
        Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "invalid arg count (" << inst->VRegA() << ") in "
                                             "range invoke";
        return false;
      }
      result = result && CheckVarArgRangeRegs(inst->VRegA(), inst->VRegC());
      break;
    case Instruction::kVerifyError:
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "unexpected opcode " << inst->Name();
      result = false;
      break;
  }
  if (inst->GetVerifyIsRuntimeOnly() && Runtime::Current()->IsCompiler() && !verify_to_dump_) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "opcode only expected at runtime " << inst->Name();
    result = false;
  }
  return result;
}

inline bool MethodVerifier::CheckRegisterIndex(uint32_t idx) {
  if (idx >= code_item_->registers_size_) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "register index out of range (" << idx << " >= "
                                      << code_item_->registers_size_ << ")";
    return false;
  }
  return true;
}

inline bool MethodVerifier::CheckWideRegisterIndex(uint32_t idx) {
  if (idx + 1 >= code_item_->registers_size_) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "wide register index out of range (" << idx
                                      << "+1 >= " << code_item_->registers_size_ << ")";
    return false;
  }
  return true;
}

inline bool MethodVerifier::CheckFieldIndex(uint32_t idx) {
  if (idx >= dex_file_->GetHeader().field_ids_size_) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "bad field index " << idx << " (max "
                                      << dex_file_->GetHeader().field_ids_size_ << ")";
    return false;
  }
  return true;
}

inline bool MethodVerifier::CheckMethodIndex(uint32_t idx) {
  if (idx >= dex_file_->GetHeader().method_ids_size_) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "bad method index " << idx << " (max "
                                      << dex_file_->GetHeader().method_ids_size_ << ")";
    return false;
  }
  return true;
}

inline bool MethodVerifier::CheckNewInstance(uint32_t idx) {
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

inline bool MethodVerifier::CheckStringIndex(uint32_t idx) {
  if (idx >= dex_file_->GetHeader().string_ids_size_) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "bad string index " << idx << " (max "
                                      << dex_file_->GetHeader().string_ids_size_ << ")";
    return false;
  }
  return true;
}

inline bool MethodVerifier::CheckTypeIndex(uint32_t idx) {
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
    Fail(VERIFY_ERROR_BAD_CLASS_HARD)
        << "can't new-array class '" << descriptor << "' (not an array)";
    return false;
  } else if (bracket_count > 255) {
    /* It is illegal to create an array of more than 255 dimensions. */
    Fail(VERIFY_ERROR_BAD_CLASS_HARD)
        << "can't new-array class '" << descriptor << "' (exceeds limit)";
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
                                      << ", data offset " << array_data_offset
                                      << ", count " << insn_count;
    return false;
  }
  /* offset to array data table is a relative branch-style offset */
  array_data = insns + array_data_offset;
  /* make sure the table is 32-bit aligned */
  if ((reinterpret_cast<uintptr_t>(array_data) & 0x03) != 0) {
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
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "branch offset of zero not allowed at"
                                      << reinterpret_cast<void*>(cur_offset);
    return false;
  }
  // Check for 32-bit overflow. This isn't strictly necessary if we can depend on the runtime
  // to have identical "wrap-around" behavior, but it's unwise to depend on that.
  if (((int64_t) cur_offset + (int64_t) offset) != (int64_t) (cur_offset + offset)) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "branch target overflow "
                                      << reinterpret_cast<void*>(cur_offset) << " +" << offset;
    return false;
  }
  const uint32_t insn_count = code_item_->insns_size_in_code_units_;
  int32_t abs_offset = cur_offset + offset;
  if (abs_offset < 0 ||
      (uint32_t) abs_offset >= insn_count ||
      !insn_flags_[abs_offset].IsOpcode()) {
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
                                      << ", switch offset " << switch_offset
                                      << ", count " << insn_count;
    return false;
  }
  /* offset to switch table is a relative branch-style offset */
  const uint16_t* switch_insns = insns + switch_offset;
  /* make sure the table is 32-bit aligned */
  if ((reinterpret_cast<uintptr_t>(switch_insns) & 0x03) != 0) {
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
    Fail(VERIFY_ERROR_BAD_CLASS_HARD)
        << StringPrintf("wrong signature for switch table (%x, wanted %x)",
                        switch_insns[0], expected_signature);
    return false;
  }
  /* make sure the end of the switch is in range */
  if (cur_offset + switch_offset + table_size > (uint32_t) insn_count) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "invalid switch end: at " << cur_offset
                                      << ", switch offset " << switch_offset
                                      << ", end " << (cur_offset + switch_offset + table_size)
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
    if (abs_offset < 0 ||
        abs_offset >= (int32_t) insn_count ||
        !insn_flags_[abs_offset].IsOpcode()) {
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "invalid switch target " << offset
                                        << " (-> " << reinterpret_cast<void*>(abs_offset) << ") at "
                                        << reinterpret_cast<void*>(cur_offset)
                                        << "[" << targ << "]";
      return false;
    }
    insn_flags_[abs_offset].SetBranchTarget();
  }
  return true;
}

bool MethodVerifier::CheckVarArgRegs(uint32_t vA, uint32_t arg[]) {
  if (vA > Instruction::kMaxVarArgRegs) {
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
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "invalid reg index " << vA << "+" << vC
                                      << " in range invoke (> " << registers_size << ")";
    return false;
  }
  return true;
}

bool MethodVerifier::VerifyCodeFlow() {
  uint16_t registers_size = code_item_->registers_size_;
  uint32_t insns_size = code_item_->insns_size_in_code_units_;

  if (registers_size * insns_size > 4*1024*1024) {
    LOG(WARNING) << "warning: method is huge (regs=" << registers_size
                 << " insns_size=" << insns_size << ")";
  }
  /* Create and initialize table holding register status */
  reg_table_.Init(kTrackCompilerInterestPoints,
                  insn_flags_.get(),
                  insns_size,
                  registers_size,
                  this);


  work_line_.reset(RegisterLine::Create(registers_size, this));
  saved_line_.reset(RegisterLine::Create(registers_size, this));

  /* Initialize register types of method arguments. */
  if (!SetTypesFromSignature()) {
    DCHECK_NE(failures_.size(), 0U);
    std::string prepend("Bad signature in ");
    prepend += PrettyMethod(dex_method_idx_, *dex_file_);
    PrependToLastFailMessage(prepend);
    return false;
  }
  /* Perform code flow verification. */
  if (!CodeFlowVerifyMethod()) {
    DCHECK_NE(failures_.size(), 0U);
    return false;
  }
  return true;
}

std::ostream& MethodVerifier::DumpFailures(std::ostream& os) {
  DCHECK_EQ(failures_.size(), failure_messages_.size());
  for (size_t i = 0; i < failures_.size(); ++i) {
      os << failure_messages_[i]->str() << "\n";
  }
  return os;
}

extern "C" void MethodVerifierGdbDump(MethodVerifier* v)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  v->Dump(std::cerr);
}

void MethodVerifier::Dump(std::ostream& os) {
  if (code_item_ == nullptr) {
    os << "Native method\n";
    return;
  }
  {
    os << "Register Types:\n";
    Indenter indent_filter(os.rdbuf(), kIndentChar, kIndentBy1Count);
    std::ostream indent_os(&indent_filter);
    reg_types_.Dump(indent_os);
  }
  os << "Dumping instructions and register lines:\n";
  Indenter indent_filter(os.rdbuf(), kIndentChar, kIndentBy1Count);
  std::ostream indent_os(&indent_filter);
  const Instruction* inst = Instruction::At(code_item_->insns_);
  for (size_t dex_pc = 0; dex_pc < code_item_->insns_size_in_code_units_;
      dex_pc += inst->SizeInCodeUnits()) {
    RegisterLine* reg_line = reg_table_.GetLine(dex_pc);
    if (reg_line != nullptr) {
      indent_os << reg_line->Dump(this) << "\n";
    }
    indent_os << StringPrintf("0x%04zx", dex_pc) << ": " << insn_flags_[dex_pc].ToString() << " ";
    const bool kDumpHexOfInstruction = false;
    if (kDumpHexOfInstruction) {
      indent_os << inst->DumpHex(5) << " ";
    }
    indent_os << inst->DumpString(dex_file_) << "\n";
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
  // Include the "this" pointer.
  size_t cur_arg = 0;
  if (!IsStatic()) {
    // If this is a constructor for a class other than java.lang.Object, mark the first ("this")
    // argument as uninitialized. This restricts field access until the superclass constructor is
    // called.
    const RegType& declaring_class = GetDeclaringClass();
    if (IsConstructor() && !declaring_class.IsJavaLangObject()) {
      reg_line->SetRegisterType(this, arg_start + cur_arg,
                                reg_types_.UninitializedThisArgument(declaring_class));
    } else {
      reg_line->SetRegisterType(this, arg_start + cur_arg, declaring_class);
    }
    cur_arg++;
  }

  const DexFile::ProtoId& proto_id =
      dex_file_->GetMethodPrototype(dex_file_->GetMethodId(dex_method_idx_));
  DexFileParameterIterator iterator(*dex_file_, proto_id);

  for (; iterator.HasNext(); iterator.Next()) {
    const char* descriptor = iterator.GetDescriptor();
    if (descriptor == nullptr) {
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
          const RegType& reg_type = ResolveClassAndCheckAccess(iterator.GetTypeIdx());
          if (!reg_type.IsNonZeroReferenceTypes()) {
            DCHECK(HasFailures());
            return false;
          }
          reg_line->SetRegisterType(this, arg_start + cur_arg, reg_type);
        }
        break;
      case 'Z':
        reg_line->SetRegisterType(this, arg_start + cur_arg, reg_types_.Boolean());
        break;
      case 'C':
        reg_line->SetRegisterType(this, arg_start + cur_arg, reg_types_.Char());
        break;
      case 'B':
        reg_line->SetRegisterType(this, arg_start + cur_arg, reg_types_.Byte());
        break;
      case 'I':
        reg_line->SetRegisterType(this, arg_start + cur_arg, reg_types_.Integer());
        break;
      case 'S':
        reg_line->SetRegisterType(this, arg_start + cur_arg, reg_types_.Short());
        break;
      case 'F':
        reg_line->SetRegisterType(this, arg_start + cur_arg, reg_types_.Float());
        break;
      case 'J':
      case 'D': {
        if (cur_arg + 1 >= expected_args) {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "expected " << expected_args
              << " args, found more (" << descriptor << ")";
          return false;
        }

        const RegType* lo_half;
        const RegType* hi_half;
        if (descriptor[0] == 'J') {
          lo_half = &reg_types_.LongLo();
          hi_half = &reg_types_.LongHi();
        } else {
          lo_half = &reg_types_.DoubleLo();
          hi_half = &reg_types_.DoubleHi();
        }
        reg_line->SetRegisterTypeWide(this, arg_start + cur_arg, *lo_half, *hi_half);
        cur_arg++;
        break;
      }
      default:
        Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "unexpected signature type char '"
                                          << descriptor << "'";
        return false;
    }
    cur_arg++;
  }
  if (cur_arg != expected_args) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "expected " << expected_args
                                      << " arguments, found " << cur_arg;
    return false;
  }
  const char* descriptor = dex_file_->GetReturnTypeDescriptor(proto_id);
  // Validate return type. We don't do the type lookup; just want to make sure that it has the right
  // format. Only major difference from the method argument format is that 'V' is supported.
  bool result;
  if (IsPrimitiveDescriptor(descriptor[0]) || descriptor[0] == 'V') {
    result = descriptor[1] == '\0';
  } else if (descriptor[0] == '[') {  // single/multi-dimensional array of object/primitive
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
    self_->AllowThreadSuspension();
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
    } else if (kIsDebugBuild) {
      /*
       * Sanity check: retrieve the stored register line (assuming
       * a full table) and make sure it actually matches.
       */
      RegisterLine* register_line = reg_table_.GetLine(insn_idx);
      if (register_line != nullptr) {
        if (work_line_->CompareLine(register_line) != 0) {
          Dump(std::cout);
          std::cout << info_messages_.str();
          LOG(FATAL) << "work_line diverged in " << PrettyMethod(dex_method_idx_, *dex_file_)
                     << "@" << reinterpret_cast<void*>(work_insn_idx_) << "\n"
                     << " work_line=" << work_line_->Dump(this) << "\n"
                     << "  expected=" << register_line->Dump(this);
        }
      }
    }
    if (!CodeFlowVerifyInstruction(&start_guess)) {
      std::string prepend(PrettyMethod(dex_method_idx_, *dex_file_));
      prepend += " failed to verify: ";
      PrependToLastFailMessage(prepend);
      return false;
    }
    /* Clear "changed" and mark as visited. */
    insn_flags_[insn_idx].SetVisited();
    insn_flags_[insn_idx].ClearChanged();
  }

  if (gDebugVerify) {
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
    for (; insn_idx < insns_size;
         insn_idx += Instruction::At(code_item_->insns_ + insn_idx)->SizeInCodeUnits()) {
      /*
       * Switch-statement data doesn't get "visited" by scanner. It
       * may or may not be preceded by a padding NOP (for alignment).
       */
      if (insns[insn_idx] == Instruction::kPackedSwitchSignature ||
          insns[insn_idx] == Instruction::kSparseSwitchSignature ||
          insns[insn_idx] == Instruction::kArrayDataSignature ||
          (insns[insn_idx] == Instruction::NOP && (insn_idx + 1 < insns_size) &&
           (insns[insn_idx + 1] == Instruction::kPackedSwitchSignature ||
            insns[insn_idx + 1] == Instruction::kSparseSwitchSignature ||
            insns[insn_idx + 1] == Instruction::kArrayDataSignature))) {
        insn_flags_[insn_idx].SetVisited();
      }

      if (!insn_flags_[insn_idx].IsVisited()) {
        if (dead_start < 0)
          dead_start = insn_idx;
      } else if (dead_start >= 0) {
        LogVerifyInfo() << "dead code " << reinterpret_cast<void*>(dead_start)
                        << "-" << reinterpret_cast<void*>(insn_idx - 1);
        dead_start = -1;
      }
    }
    if (dead_start >= 0) {
      LogVerifyInfo() << "dead code " << reinterpret_cast<void*>(dead_start)
                      << "-" << reinterpret_cast<void*>(insn_idx - 1);
    }
    // To dump the state of the verify after a method, do something like:
    // if (PrettyMethod(dex_method_idx_, *dex_file_) ==
    //     "boolean java.lang.String.equals(java.lang.Object)") {
    //   LOG(INFO) << info_messages_.str();
    // }
  }
  return true;
}

bool MethodVerifier::CodeFlowVerifyInstruction(uint32_t* start_guess) {
  // If we're doing FindLocksAtDexPc, check whether we're at the dex pc we care about.
  // We want the state _before_ the instruction, for the case where the dex pc we're
  // interested in is itself a monitor-enter instruction (which is a likely place
  // for a thread to be suspended).
  if (monitor_enter_dex_pcs_ != nullptr && work_insn_idx_ == interesting_dex_pc_) {
    monitor_enter_dex_pcs_->clear();  // The new work line is more accurate than the previous one.
    for (size_t i = 0; i < work_line_->GetMonitorEnterCount(); ++i) {
      monitor_enter_dex_pcs_->push_back(work_line_->GetMonitorEnterDexPc(i));
    }
  }

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
  int opcode_flags = Instruction::FlagsOf(inst->Opcode());

  int32_t branch_target = 0;
  bool just_set_result = false;
  if (gDebugVerify) {
    // Generate processing back trace to debug verifier
    LogVerifyInfo() << "Processing " << inst->DumpString(dex_file_) << "\n"
                    << work_line_->Dump(this) << "\n";
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
  } else if (kIsDebugBuild) {
    saved_line_->FillWithGarbage();
  }


  // We need to ensure the work line is consistent while performing validation. When we spot a
  // peephole pattern we compute a new line for either the fallthrough instruction or the
  // branch target.
  std::unique_ptr<RegisterLine> branch_line;
  std::unique_ptr<RegisterLine> fallthrough_line;

  switch (inst->Opcode()) {
    case Instruction::NOP:
      /*
       * A "pure" NOP has no effect on anything. Data tables start with
       * a signature that looks like a NOP; if we see one of these in
       * the course of executing code then we have a problem.
       */
      if (inst->VRegA_10x() != 0) {
        Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "encountered data table in instruction stream";
      }
      break;

    case Instruction::MOVE:
      work_line_->CopyRegister1(this, inst->VRegA_12x(), inst->VRegB_12x(), kTypeCategory1nr);
      break;
    case Instruction::MOVE_FROM16:
      work_line_->CopyRegister1(this, inst->VRegA_22x(), inst->VRegB_22x(), kTypeCategory1nr);
      break;
    case Instruction::MOVE_16:
      work_line_->CopyRegister1(this, inst->VRegA_32x(), inst->VRegB_32x(), kTypeCategory1nr);
      break;
    case Instruction::MOVE_WIDE:
      work_line_->CopyRegister2(this, inst->VRegA_12x(), inst->VRegB_12x());
      break;
    case Instruction::MOVE_WIDE_FROM16:
      work_line_->CopyRegister2(this, inst->VRegA_22x(), inst->VRegB_22x());
      break;
    case Instruction::MOVE_WIDE_16:
      work_line_->CopyRegister2(this, inst->VRegA_32x(), inst->VRegB_32x());
      break;
    case Instruction::MOVE_OBJECT:
      work_line_->CopyRegister1(this, inst->VRegA_12x(), inst->VRegB_12x(), kTypeCategoryRef);
      break;
    case Instruction::MOVE_OBJECT_FROM16:
      work_line_->CopyRegister1(this, inst->VRegA_22x(), inst->VRegB_22x(), kTypeCategoryRef);
      break;
    case Instruction::MOVE_OBJECT_16:
      work_line_->CopyRegister1(this, inst->VRegA_32x(), inst->VRegB_32x(), kTypeCategoryRef);
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
      work_line_->CopyResultRegister1(this, inst->VRegA_11x(), false);
      break;
    case Instruction::MOVE_RESULT_WIDE:
      work_line_->CopyResultRegister2(this, inst->VRegA_11x());
      break;
    case Instruction::MOVE_RESULT_OBJECT:
      work_line_->CopyResultRegister1(this, inst->VRegA_11x(), true);
      break;

    case Instruction::MOVE_EXCEPTION: {
      /*
       * This statement can only appear as the first instruction in an exception handler. We verify
       * that as part of extracting the exception type from the catch block list.
       */
      const RegType& res_type = GetCaughtExceptionType();
      work_line_->SetRegisterType(this, inst->VRegA_11x(), res_type);
      break;
    }
    case Instruction::RETURN_VOID:
      if (!IsConstructor() || work_line_->CheckConstructorReturn(this)) {
        if (!GetMethodReturnType().IsConflict()) {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "return-void not expected";
        }
      }
      break;
    case Instruction::RETURN:
      if (!IsConstructor() || work_line_->CheckConstructorReturn(this)) {
        /* check the method signature */
        const RegType& return_type = GetMethodReturnType();
        if (!return_type.IsCategory1Types()) {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "unexpected non-category 1 return type "
                                            << return_type;
        } else {
          // Compilers may generate synthetic functions that write byte values into boolean fields.
          // Also, it may use integer values for boolean, byte, short, and character return types.
          const uint32_t vregA = inst->VRegA_11x();
          const RegType& src_type = work_line_->GetRegisterType(this, vregA);
          bool use_src = ((return_type.IsBoolean() && src_type.IsByte()) ||
                          ((return_type.IsBoolean() || return_type.IsByte() ||
                           return_type.IsShort() || return_type.IsChar()) &&
                           src_type.IsInteger()));
          /* check the register contents */
          bool success =
              work_line_->VerifyRegisterType(this, vregA, use_src ? src_type : return_type);
          if (!success) {
            AppendToLastFailMessage(StringPrintf(" return-1nr on invalid register v%d", vregA));
          }
        }
      }
      break;
    case Instruction::RETURN_WIDE:
      if (!IsConstructor() || work_line_->CheckConstructorReturn(this)) {
        /* check the method signature */
        const RegType& return_type = GetMethodReturnType();
        if (!return_type.IsCategory2Types()) {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "return-wide not expected";
        } else {
          /* check the register contents */
          const uint32_t vregA = inst->VRegA_11x();
          bool success = work_line_->VerifyRegisterType(this, vregA, return_type);
          if (!success) {
            AppendToLastFailMessage(StringPrintf(" return-wide on invalid register v%d", vregA));
          }
        }
      }
      break;
    case Instruction::RETURN_OBJECT:
      if (!IsConstructor() || work_line_->CheckConstructorReturn(this)) {
        const RegType& return_type = GetMethodReturnType();
        if (!return_type.IsReferenceTypes()) {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "return-object not expected";
        } else {
          /* return_type is the *expected* return type, not register value */
          DCHECK(!return_type.IsZero());
          DCHECK(!return_type.IsUninitializedReference());
          const uint32_t vregA = inst->VRegA_11x();
          const RegType& reg_type = work_line_->GetRegisterType(this, vregA);
          // Disallow returning uninitialized values and verify that the reference in vAA is an
          // instance of the "return_type"
          if (reg_type.IsUninitializedTypes()) {
            Fail(VERIFY_ERROR_BAD_CLASS_SOFT) << "returning uninitialized object '"
                                              << reg_type << "'";
          } else if (!return_type.IsAssignableFrom(reg_type)) {
            if (reg_type.IsUnresolvedTypes() || return_type.IsUnresolvedTypes()) {
              Fail(VERIFY_ERROR_NO_CLASS) << " can't resolve returned type '" << return_type
                  << "' or '" << reg_type << "'";
            } else {
              Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "returning '" << reg_type
                  << "', but expected from declaration '" << return_type << "'";
            }
          }
        }
      }
      break;

      /* could be boolean, int, float, or a null reference */
    case Instruction::CONST_4: {
      int32_t val = static_cast<int32_t>(inst->VRegB_11n() << 28) >> 28;
      work_line_->SetRegisterType(this, inst->VRegA_11n(),
                                  DetermineCat1Constant(val, need_precise_constants_));
      break;
    }
    case Instruction::CONST_16: {
      int16_t val = static_cast<int16_t>(inst->VRegB_21s());
      work_line_->SetRegisterType(this, inst->VRegA_21s(),
                                  DetermineCat1Constant(val, need_precise_constants_));
      break;
    }
    case Instruction::CONST: {
      int32_t val = inst->VRegB_31i();
      work_line_->SetRegisterType(this, inst->VRegA_31i(),
                                  DetermineCat1Constant(val, need_precise_constants_));
      break;
    }
    case Instruction::CONST_HIGH16: {
      int32_t val = static_cast<int32_t>(inst->VRegB_21h() << 16);
      work_line_->SetRegisterType(this, inst->VRegA_21h(),
                                  DetermineCat1Constant(val, need_precise_constants_));
      break;
    }
      /* could be long or double; resolved upon use */
    case Instruction::CONST_WIDE_16: {
      int64_t val = static_cast<int16_t>(inst->VRegB_21s());
      const RegType& lo = reg_types_.FromCat2ConstLo(static_cast<int32_t>(val), true);
      const RegType& hi = reg_types_.FromCat2ConstHi(static_cast<int32_t>(val >> 32), true);
      work_line_->SetRegisterTypeWide(this, inst->VRegA_21s(), lo, hi);
      break;
    }
    case Instruction::CONST_WIDE_32: {
      int64_t val = static_cast<int32_t>(inst->VRegB_31i());
      const RegType& lo = reg_types_.FromCat2ConstLo(static_cast<int32_t>(val), true);
      const RegType& hi = reg_types_.FromCat2ConstHi(static_cast<int32_t>(val >> 32), true);
      work_line_->SetRegisterTypeWide(this, inst->VRegA_31i(), lo, hi);
      break;
    }
    case Instruction::CONST_WIDE: {
      int64_t val = inst->VRegB_51l();
      const RegType& lo = reg_types_.FromCat2ConstLo(static_cast<int32_t>(val), true);
      const RegType& hi = reg_types_.FromCat2ConstHi(static_cast<int32_t>(val >> 32), true);
      work_line_->SetRegisterTypeWide(this, inst->VRegA_51l(), lo, hi);
      break;
    }
    case Instruction::CONST_WIDE_HIGH16: {
      int64_t val = static_cast<uint64_t>(inst->VRegB_21h()) << 48;
      const RegType& lo = reg_types_.FromCat2ConstLo(static_cast<int32_t>(val), true);
      const RegType& hi = reg_types_.FromCat2ConstHi(static_cast<int32_t>(val >> 32), true);
      work_line_->SetRegisterTypeWide(this, inst->VRegA_21h(), lo, hi);
      break;
    }
    case Instruction::CONST_STRING:
      work_line_->SetRegisterType(this, inst->VRegA_21c(), reg_types_.JavaLangString());
      break;
    case Instruction::CONST_STRING_JUMBO:
      work_line_->SetRegisterType(this, inst->VRegA_31c(), reg_types_.JavaLangString());
      break;
    case Instruction::CONST_CLASS: {
      // Get type from instruction if unresolved then we need an access check
      // TODO: check Compiler::CanAccessTypeWithoutChecks returns false when res_type is unresolved
      const RegType& res_type = ResolveClassAndCheckAccess(inst->VRegB_21c());
      // Register holds class, ie its type is class, on error it will hold Conflict.
      work_line_->SetRegisterType(this, inst->VRegA_21c(),
                                  res_type.IsConflict() ? res_type
                                                        : reg_types_.JavaLangClass());
      break;
    }
    case Instruction::MONITOR_ENTER:
      work_line_->PushMonitor(this, inst->VRegA_11x(), work_insn_idx_);
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
      work_line_->PopMonitor(this, inst->VRegA_11x());
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
      const bool is_checkcast = (inst->Opcode() == Instruction::CHECK_CAST);
      const uint32_t type_idx = (is_checkcast) ? inst->VRegB_21c() : inst->VRegC_22c();
      const RegType& res_type = ResolveClassAndCheckAccess(type_idx);
      if (res_type.IsConflict()) {
        // If this is a primitive type, fail HARD.
        mirror::Class* klass = dex_cache_->GetResolvedType(type_idx);
        if (klass != nullptr && klass->IsPrimitive()) {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "using primitive type "
              << dex_file_->StringByTypeIdx(type_idx) << " in instanceof in "
              << GetDeclaringClass();
          break;
        }

        DCHECK_NE(failures_.size(), 0U);
        if (!is_checkcast) {
          work_line_->SetRegisterType(this, inst->VRegA_22c(), reg_types_.Boolean());
        }
        break;  // bad class
      }
      // TODO: check Compiler::CanAccessTypeWithoutChecks returns false when res_type is unresolved
      uint32_t orig_type_reg = (is_checkcast) ? inst->VRegA_21c() : inst->VRegB_22c();
      const RegType& orig_type = work_line_->GetRegisterType(this, orig_type_reg);
      if (!res_type.IsNonZeroReferenceTypes()) {
        if (is_checkcast) {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "check-cast on unexpected class " << res_type;
        } else {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "instance-of on unexpected class " << res_type;
        }
      } else if (!orig_type.IsReferenceTypes()) {
        if (is_checkcast) {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "check-cast on non-reference in v" << orig_type_reg;
        } else {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "instance-of on non-reference in v" << orig_type_reg;
        }
      } else {
        if (is_checkcast) {
          work_line_->SetRegisterType(this, inst->VRegA_21c(), res_type);
        } else {
          work_line_->SetRegisterType(this, inst->VRegA_22c(), reg_types_.Boolean());
        }
      }
      break;
    }
    case Instruction::ARRAY_LENGTH: {
      const RegType& res_type = work_line_->GetRegisterType(this, inst->VRegB_12x());
      if (res_type.IsReferenceTypes()) {
        if (!res_type.IsArrayTypes() && !res_type.IsZero()) {  // ie not an array or null
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "array-length on non-array " << res_type;
        } else {
          work_line_->SetRegisterType(this, inst->VRegA_12x(), reg_types_.Integer());
        }
      } else {
        Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "array-length on non-array " << res_type;
      }
      break;
    }
    case Instruction::NEW_INSTANCE: {
      const RegType& res_type = ResolveClassAndCheckAccess(inst->VRegB_21c());
      if (res_type.IsConflict()) {
        DCHECK_NE(failures_.size(), 0U);
        break;  // bad class
      }
      // TODO: check Compiler::CanAccessTypeWithoutChecks returns false when res_type is unresolved
      // can't create an instance of an interface or abstract class */
      if (!res_type.IsInstantiableTypes()) {
        Fail(VERIFY_ERROR_INSTANTIATION)
            << "new-instance on primitive, interface or abstract class" << res_type;
        // Soft failure so carry on to set register type.
      }
      const RegType& uninit_type = reg_types_.Uninitialized(res_type, work_insn_idx_);
      // Any registers holding previous allocations from this address that have not yet been
      // initialized must be marked invalid.
      work_line_->MarkUninitRefsAsInvalid(this, uninit_type);
      // add the new uninitialized reference to the register state
      work_line_->SetRegisterType(this, inst->VRegA_21c(), uninit_type);
      break;
    }
    case Instruction::NEW_ARRAY:
      VerifyNewArray(inst, false, false);
      break;
    case Instruction::FILLED_NEW_ARRAY:
      VerifyNewArray(inst, true, false);
      just_set_result = true;  // Filled new array sets result register
      break;
    case Instruction::FILLED_NEW_ARRAY_RANGE:
      VerifyNewArray(inst, true, true);
      just_set_result = true;  // Filled new array range sets result register
      break;
    case Instruction::CMPL_FLOAT:
    case Instruction::CMPG_FLOAT:
      if (!work_line_->VerifyRegisterType(this, inst->VRegB_23x(), reg_types_.Float())) {
        break;
      }
      if (!work_line_->VerifyRegisterType(this, inst->VRegC_23x(), reg_types_.Float())) {
        break;
      }
      work_line_->SetRegisterType(this, inst->VRegA_23x(), reg_types_.Integer());
      break;
    case Instruction::CMPL_DOUBLE:
    case Instruction::CMPG_DOUBLE:
      if (!work_line_->VerifyRegisterTypeWide(this, inst->VRegB_23x(), reg_types_.DoubleLo(),
                                              reg_types_.DoubleHi())) {
        break;
      }
      if (!work_line_->VerifyRegisterTypeWide(this, inst->VRegC_23x(), reg_types_.DoubleLo(),
                                              reg_types_.DoubleHi())) {
        break;
      }
      work_line_->SetRegisterType(this, inst->VRegA_23x(), reg_types_.Integer());
      break;
    case Instruction::CMP_LONG:
      if (!work_line_->VerifyRegisterTypeWide(this, inst->VRegB_23x(), reg_types_.LongLo(),
                                              reg_types_.LongHi())) {
        break;
      }
      if (!work_line_->VerifyRegisterTypeWide(this, inst->VRegC_23x(), reg_types_.LongLo(),
                                              reg_types_.LongHi())) {
        break;
      }
      work_line_->SetRegisterType(this, inst->VRegA_23x(), reg_types_.Integer());
      break;
    case Instruction::THROW: {
      const RegType& res_type = work_line_->GetRegisterType(this, inst->VRegA_11x());
      if (!reg_types_.JavaLangThrowable(false).IsAssignableFrom(res_type)) {
        Fail(res_type.IsUnresolvedTypes() ? VERIFY_ERROR_NO_CLASS : VERIFY_ERROR_BAD_CLASS_SOFT)
            << "thrown class " << res_type << " not instanceof Throwable";
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
      work_line_->VerifyRegisterType(this, inst->VRegA_31t(), reg_types_.Integer());
      break;

    case Instruction::FILL_ARRAY_DATA: {
      /* Similar to the verification done for APUT */
      const RegType& array_type = work_line_->GetRegisterType(this, inst->VRegA_31t());
      /* array_type can be null if the reg type is Zero */
      if (!array_type.IsZero()) {
        if (!array_type.IsArrayTypes()) {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "invalid fill-array-data with array type "
                                            << array_type;
        } else {
          const RegType& component_type = reg_types_.GetComponentType(array_type, GetClassLoader());
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
      const RegType& reg_type1 = work_line_->GetRegisterType(this, inst->VRegA_22t());
      const RegType& reg_type2 = work_line_->GetRegisterType(this, inst->VRegB_22t());
      bool mismatch = false;
      if (reg_type1.IsZero()) {  // zero then integral or reference expected
        mismatch = !reg_type2.IsReferenceTypes() && !reg_type2.IsIntegralTypes();
      } else if (reg_type1.IsReferenceTypes()) {  // both references?
        mismatch = !reg_type2.IsReferenceTypes();
      } else {  // both integral?
        mismatch = !reg_type1.IsIntegralTypes() || !reg_type2.IsIntegralTypes();
      }
      if (mismatch) {
        Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "args to if-eq/if-ne (" << reg_type1 << ","
                                          << reg_type2 << ") must both be references or integral";
      }
      break;
    }
    case Instruction::IF_LT:
    case Instruction::IF_GE:
    case Instruction::IF_GT:
    case Instruction::IF_LE: {
      const RegType& reg_type1 = work_line_->GetRegisterType(this, inst->VRegA_22t());
      const RegType& reg_type2 = work_line_->GetRegisterType(this, inst->VRegB_22t());
      if (!reg_type1.IsIntegralTypes() || !reg_type2.IsIntegralTypes()) {
        Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "args to 'if' (" << reg_type1 << ","
                                          << reg_type2 << ") must be integral";
      }
      break;
    }
    case Instruction::IF_EQZ:
    case Instruction::IF_NEZ: {
      const RegType& reg_type = work_line_->GetRegisterType(this, inst->VRegA_21t());
      if (!reg_type.IsReferenceTypes() && !reg_type.IsIntegralTypes()) {
        Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "type " << reg_type
                                          << " unexpected as arg to if-eqz/if-nez";
      }

      // Find previous instruction - its existence is a precondition to peephole optimization.
      uint32_t instance_of_idx = 0;
      if (0 != work_insn_idx_) {
        instance_of_idx = work_insn_idx_ - 1;
        while (0 != instance_of_idx && !insn_flags_[instance_of_idx].IsOpcode()) {
          instance_of_idx--;
        }
        CHECK(insn_flags_[instance_of_idx].IsOpcode());
      } else {
        break;
      }

      const Instruction* instance_of_inst = Instruction::At(code_item_->insns_ + instance_of_idx);

      /* Check for peep-hole pattern of:
       *    ...;
       *    instance-of vX, vY, T;
       *    ifXXX vX, label ;
       *    ...;
       * label:
       *    ...;
       * and sharpen the type of vY to be type T.
       * Note, this pattern can't be if:
       *  - if there are other branches to this branch,
       *  - when vX == vY.
       */
      if (!CurrentInsnFlags()->IsBranchTarget() &&
          (Instruction::INSTANCE_OF == instance_of_inst->Opcode()) &&
          (inst->VRegA_21t() == instance_of_inst->VRegA_22c()) &&
          (instance_of_inst->VRegA_22c() != instance_of_inst->VRegB_22c())) {
        // Check the type of the instance-of is different than that of registers type, as if they
        // are the same there is no work to be done here. Check that the conversion is not to or
        // from an unresolved type as type information is imprecise. If the instance-of is to an
        // interface then ignore the type information as interfaces can only be treated as Objects
        // and we don't want to disallow field and other operations on the object. If the value
        // being instance-of checked against is known null (zero) then allow the optimization as
        // we didn't have type information. If the merge of the instance-of type with the original
        // type is assignable to the original then allow optimization. This check is performed to
        // ensure that subsequent merges don't lose type information - such as becoming an
        // interface from a class that would lose information relevant to field checks.
        const RegType& orig_type = work_line_->GetRegisterType(this, instance_of_inst->VRegB_22c());
        const RegType& cast_type = ResolveClassAndCheckAccess(instance_of_inst->VRegC_22c());

        if (!orig_type.Equals(cast_type) &&
            !cast_type.IsUnresolvedTypes() && !orig_type.IsUnresolvedTypes() &&
            cast_type.HasClass() &&             // Could be conflict type, make sure it has a class.
            !cast_type.GetClass()->IsInterface() &&
            (orig_type.IsZero() ||
                orig_type.IsStrictlyAssignableFrom(cast_type.Merge(orig_type, &reg_types_)))) {
          RegisterLine* update_line = RegisterLine::Create(code_item_->registers_size_, this);
          if (inst->Opcode() == Instruction::IF_EQZ) {
            fallthrough_line.reset(update_line);
          } else {
            branch_line.reset(update_line);
          }
          update_line->CopyFromLine(work_line_.get());
          update_line->SetRegisterType(this, instance_of_inst->VRegB_22c(), cast_type);
          if (!insn_flags_[instance_of_idx].IsBranchTarget() && 0 != instance_of_idx) {
            // See if instance-of was preceded by a move-object operation, common due to the small
            // register encoding space of instance-of, and propagate type information to the source
            // of the move-object.
            uint32_t move_idx = instance_of_idx - 1;
            while (0 != move_idx && !insn_flags_[move_idx].IsOpcode()) {
              move_idx--;
            }
            CHECK(insn_flags_[move_idx].IsOpcode());
            const Instruction* move_inst = Instruction::At(code_item_->insns_ + move_idx);
            switch (move_inst->Opcode()) {
              case Instruction::MOVE_OBJECT:
                if (move_inst->VRegA_12x() == instance_of_inst->VRegB_22c()) {
                  update_line->SetRegisterType(this, move_inst->VRegB_12x(), cast_type);
                }
                break;
              case Instruction::MOVE_OBJECT_FROM16:
                if (move_inst->VRegA_22x() == instance_of_inst->VRegB_22c()) {
                  update_line->SetRegisterType(this, move_inst->VRegB_22x(), cast_type);
                }
                break;
              case Instruction::MOVE_OBJECT_16:
                if (move_inst->VRegA_32x() == instance_of_inst->VRegB_22c()) {
                  update_line->SetRegisterType(this, move_inst->VRegB_32x(), cast_type);
                }
                break;
              default:
                break;
            }
          }
        }
      }

      break;
    }
    case Instruction::IF_LTZ:
    case Instruction::IF_GEZ:
    case Instruction::IF_GTZ:
    case Instruction::IF_LEZ: {
      const RegType& reg_type = work_line_->GetRegisterType(this, inst->VRegA_21t());
      if (!reg_type.IsIntegralTypes()) {
        Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "type " << reg_type
                                          << " unexpected as arg to if-ltz/if-gez/if-gtz/if-lez";
      }
      break;
    }
    case Instruction::AGET_BOOLEAN:
      VerifyAGet(inst, reg_types_.Boolean(), true);
      break;
    case Instruction::AGET_BYTE:
      VerifyAGet(inst, reg_types_.Byte(), true);
      break;
    case Instruction::AGET_CHAR:
      VerifyAGet(inst, reg_types_.Char(), true);
      break;
    case Instruction::AGET_SHORT:
      VerifyAGet(inst, reg_types_.Short(), true);
      break;
    case Instruction::AGET:
      VerifyAGet(inst, reg_types_.Integer(), true);
      break;
    case Instruction::AGET_WIDE:
      VerifyAGet(inst, reg_types_.LongLo(), true);
      break;
    case Instruction::AGET_OBJECT:
      VerifyAGet(inst, reg_types_.JavaLangObject(false), false);
      break;

    case Instruction::APUT_BOOLEAN:
      VerifyAPut(inst, reg_types_.Boolean(), true);
      break;
    case Instruction::APUT_BYTE:
      VerifyAPut(inst, reg_types_.Byte(), true);
      break;
    case Instruction::APUT_CHAR:
      VerifyAPut(inst, reg_types_.Char(), true);
      break;
    case Instruction::APUT_SHORT:
      VerifyAPut(inst, reg_types_.Short(), true);
      break;
    case Instruction::APUT:
      VerifyAPut(inst, reg_types_.Integer(), true);
      break;
    case Instruction::APUT_WIDE:
      VerifyAPut(inst, reg_types_.LongLo(), true);
      break;
    case Instruction::APUT_OBJECT:
      VerifyAPut(inst, reg_types_.JavaLangObject(false), false);
      break;

    case Instruction::IGET_BOOLEAN:
      VerifyISGet(inst, reg_types_.Boolean(), true, false);
      break;
    case Instruction::IGET_BYTE:
      VerifyISGet(inst, reg_types_.Byte(), true, false);
      break;
    case Instruction::IGET_CHAR:
      VerifyISGet(inst, reg_types_.Char(), true, false);
      break;
    case Instruction::IGET_SHORT:
      VerifyISGet(inst, reg_types_.Short(), true, false);
      break;
    case Instruction::IGET:
      VerifyISGet(inst, reg_types_.Integer(), true, false);
      break;
    case Instruction::IGET_WIDE:
      VerifyISGet(inst, reg_types_.LongLo(), true, false);
      break;
    case Instruction::IGET_OBJECT:
      VerifyISGet(inst, reg_types_.JavaLangObject(false), false, false);
      break;

    case Instruction::IPUT_BOOLEAN:
      VerifyISPut(inst, reg_types_.Boolean(), true, false);
      break;
    case Instruction::IPUT_BYTE:
      VerifyISPut(inst, reg_types_.Byte(), true, false);
      break;
    case Instruction::IPUT_CHAR:
      VerifyISPut(inst, reg_types_.Char(), true, false);
      break;
    case Instruction::IPUT_SHORT:
      VerifyISPut(inst, reg_types_.Short(), true, false);
      break;
    case Instruction::IPUT:
      VerifyISPut(inst, reg_types_.Integer(), true, false);
      break;
    case Instruction::IPUT_WIDE:
      VerifyISPut(inst, reg_types_.LongLo(), true, false);
      break;
    case Instruction::IPUT_OBJECT:
      VerifyISPut(inst, reg_types_.JavaLangObject(false), false, false);
      break;

    case Instruction::SGET_BOOLEAN:
      VerifyISGet(inst, reg_types_.Boolean(), true, true);
      break;
    case Instruction::SGET_BYTE:
      VerifyISGet(inst, reg_types_.Byte(), true, true);
      break;
    case Instruction::SGET_CHAR:
      VerifyISGet(inst, reg_types_.Char(), true, true);
      break;
    case Instruction::SGET_SHORT:
      VerifyISGet(inst, reg_types_.Short(), true, true);
      break;
    case Instruction::SGET:
      VerifyISGet(inst, reg_types_.Integer(), true, true);
      break;
    case Instruction::SGET_WIDE:
      VerifyISGet(inst, reg_types_.LongLo(), true, true);
      break;
    case Instruction::SGET_OBJECT:
      VerifyISGet(inst, reg_types_.JavaLangObject(false), false, true);
      break;

    case Instruction::SPUT_BOOLEAN:
      VerifyISPut(inst, reg_types_.Boolean(), true, true);
      break;
    case Instruction::SPUT_BYTE:
      VerifyISPut(inst, reg_types_.Byte(), true, true);
      break;
    case Instruction::SPUT_CHAR:
      VerifyISPut(inst, reg_types_.Char(), true, true);
      break;
    case Instruction::SPUT_SHORT:
      VerifyISPut(inst, reg_types_.Short(), true, true);
      break;
    case Instruction::SPUT:
      VerifyISPut(inst, reg_types_.Integer(), true, true);
      break;
    case Instruction::SPUT_WIDE:
      VerifyISPut(inst, reg_types_.LongLo(), true, true);
      break;
    case Instruction::SPUT_OBJECT:
      VerifyISPut(inst, reg_types_.JavaLangObject(false), false, true);
      break;

    case Instruction::INVOKE_VIRTUAL:
    case Instruction::INVOKE_VIRTUAL_RANGE:
    case Instruction::INVOKE_SUPER:
    case Instruction::INVOKE_SUPER_RANGE: {
      bool is_range = (inst->Opcode() == Instruction::INVOKE_VIRTUAL_RANGE ||
                       inst->Opcode() == Instruction::INVOKE_SUPER_RANGE);
      bool is_super = (inst->Opcode() == Instruction::INVOKE_SUPER ||
                       inst->Opcode() == Instruction::INVOKE_SUPER_RANGE);
      mirror::ArtMethod* called_method = VerifyInvocationArgs(inst, METHOD_VIRTUAL, is_range,
                                                              is_super);
      const RegType* return_type = nullptr;
      if (called_method != nullptr) {
        StackHandleScope<1> hs(self_);
        Handle<mirror::ArtMethod> h_called_method(hs.NewHandle(called_method));
        MethodHelper mh(h_called_method);
        mirror::Class* return_type_class = mh.GetReturnType(can_load_classes_);
        if (return_type_class != nullptr) {
          return_type = &reg_types_.FromClass(h_called_method->GetReturnTypeDescriptor(),
                                              return_type_class,
                                              return_type_class->CannotBeAssignedFromOtherTypes());
        } else {
          DCHECK(!can_load_classes_ || self_->IsExceptionPending());
          self_->ClearException();
        }
      }
      if (return_type == nullptr) {
        uint32_t method_idx = (is_range) ? inst->VRegB_3rc() : inst->VRegB_35c();
        const DexFile::MethodId& method_id = dex_file_->GetMethodId(method_idx);
        uint32_t return_type_idx = dex_file_->GetProtoId(method_id.proto_idx_).return_type_idx_;
        const char* descriptor = dex_file_->StringByTypeIdx(return_type_idx);
        return_type = &reg_types_.FromDescriptor(GetClassLoader(), descriptor, false);
      }
      if (!return_type->IsLowHalf()) {
        work_line_->SetResultRegisterType(this, *return_type);
      } else {
        work_line_->SetResultRegisterTypeWide(*return_type, return_type->HighHalf(&reg_types_));
      }
      just_set_result = true;
      break;
    }
    case Instruction::INVOKE_DIRECT:
    case Instruction::INVOKE_DIRECT_RANGE: {
      bool is_range = (inst->Opcode() == Instruction::INVOKE_DIRECT_RANGE);
      mirror::ArtMethod* called_method = VerifyInvocationArgs(inst, METHOD_DIRECT,
                                                                   is_range, false);
      const char* return_type_descriptor;
      bool is_constructor;
      const RegType* return_type = nullptr;
      if (called_method == nullptr) {
        uint32_t method_idx = (is_range) ? inst->VRegB_3rc() : inst->VRegB_35c();
        const DexFile::MethodId& method_id = dex_file_->GetMethodId(method_idx);
        is_constructor = strcmp("<init>", dex_file_->StringDataByIdx(method_id.name_idx_)) == 0;
        uint32_t return_type_idx = dex_file_->GetProtoId(method_id.proto_idx_).return_type_idx_;
        return_type_descriptor =  dex_file_->StringByTypeIdx(return_type_idx);
      } else {
        is_constructor = called_method->IsConstructor();
        return_type_descriptor = called_method->GetReturnTypeDescriptor();
        StackHandleScope<1> hs(self_);
        Handle<mirror::ArtMethod> h_called_method(hs.NewHandle(called_method));
        MethodHelper mh(h_called_method);
        mirror::Class* return_type_class = mh.GetReturnType(can_load_classes_);
        if (return_type_class != nullptr) {
          return_type = &reg_types_.FromClass(return_type_descriptor,
                                              return_type_class,
                                              return_type_class->CannotBeAssignedFromOtherTypes());
        } else {
          DCHECK(!can_load_classes_ || self_->IsExceptionPending());
          self_->ClearException();
        }
      }
      if (is_constructor) {
        /*
         * Some additional checks when calling a constructor. We know from the invocation arg check
         * that the "this" argument is an instance of called_method->klass. Now we further restrict
         * that to require that called_method->klass is the same as this->klass or this->super,
         * allowing the latter only if the "this" argument is the same as the "this" argument to
         * this method (which implies that we're in a constructor ourselves).
         */
        const RegType& this_type = work_line_->GetInvocationThis(this, inst, is_range);
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
        work_line_->MarkRefsAsInitialized(this, this_type);
      }
      if (return_type == nullptr) {
        return_type = &reg_types_.FromDescriptor(GetClassLoader(), return_type_descriptor,
                                                 false);
      }
      if (!return_type->IsLowHalf()) {
        work_line_->SetResultRegisterType(this, *return_type);
      } else {
        work_line_->SetResultRegisterTypeWide(*return_type, return_type->HighHalf(&reg_types_));
      }
      just_set_result = true;
      break;
    }
    case Instruction::INVOKE_STATIC:
    case Instruction::INVOKE_STATIC_RANGE: {
        bool is_range = (inst->Opcode() == Instruction::INVOKE_STATIC_RANGE);
        mirror::ArtMethod* called_method = VerifyInvocationArgs(inst,
                                                                     METHOD_STATIC,
                                                                     is_range,
                                                                     false);
        const char* descriptor;
        if (called_method == nullptr) {
          uint32_t method_idx = (is_range) ? inst->VRegB_3rc() : inst->VRegB_35c();
          const DexFile::MethodId& method_id = dex_file_->GetMethodId(method_idx);
          uint32_t return_type_idx = dex_file_->GetProtoId(method_id.proto_idx_).return_type_idx_;
          descriptor = dex_file_->StringByTypeIdx(return_type_idx);
        } else {
          descriptor = called_method->GetReturnTypeDescriptor();
        }
        const RegType& return_type = reg_types_.FromDescriptor(GetClassLoader(), descriptor, false);
        if (!return_type.IsLowHalf()) {
          work_line_->SetResultRegisterType(this, return_type);
        } else {
          work_line_->SetResultRegisterTypeWide(return_type, return_type.HighHalf(&reg_types_));
        }
        just_set_result = true;
      }
      break;
    case Instruction::INVOKE_INTERFACE:
    case Instruction::INVOKE_INTERFACE_RANGE: {
      bool is_range =  (inst->Opcode() == Instruction::INVOKE_INTERFACE_RANGE);
      mirror::ArtMethod* abs_method = VerifyInvocationArgs(inst,
                                                                METHOD_INTERFACE,
                                                                is_range,
                                                                false);
      if (abs_method != nullptr) {
        mirror::Class* called_interface = abs_method->GetDeclaringClass();
        if (!called_interface->IsInterface() && !called_interface->IsObjectClass()) {
          Fail(VERIFY_ERROR_CLASS_CHANGE) << "expected interface class in invoke-interface '"
              << PrettyMethod(abs_method) << "'";
          break;
        }
      }
      /* Get the type of the "this" arg, which should either be a sub-interface of called
       * interface or Object (see comments in RegType::JoinClass).
       */
      const RegType& this_type = work_line_->GetInvocationThis(this, inst, is_range);
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
      if (abs_method == nullptr) {
        uint32_t method_idx = (is_range) ? inst->VRegB_3rc() : inst->VRegB_35c();
        const DexFile::MethodId& method_id = dex_file_->GetMethodId(method_idx);
        uint32_t return_type_idx = dex_file_->GetProtoId(method_id.proto_idx_).return_type_idx_;
        descriptor =  dex_file_->StringByTypeIdx(return_type_idx);
      } else {
        descriptor = abs_method->GetReturnTypeDescriptor();
      }
      const RegType& return_type = reg_types_.FromDescriptor(GetClassLoader(), descriptor, false);
      if (!return_type.IsLowHalf()) {
        work_line_->SetResultRegisterType(this, return_type);
      } else {
        work_line_->SetResultRegisterTypeWide(return_type, return_type.HighHalf(&reg_types_));
      }
      just_set_result = true;
      break;
    }
    case Instruction::NEG_INT:
    case Instruction::NOT_INT:
      work_line_->CheckUnaryOp(this, inst, reg_types_.Integer(), reg_types_.Integer());
      break;
    case Instruction::NEG_LONG:
    case Instruction::NOT_LONG:
      work_line_->CheckUnaryOpWide(this, inst, reg_types_.LongLo(), reg_types_.LongHi(),
                                   reg_types_.LongLo(), reg_types_.LongHi());
      break;
    case Instruction::NEG_FLOAT:
      work_line_->CheckUnaryOp(this, inst, reg_types_.Float(), reg_types_.Float());
      break;
    case Instruction::NEG_DOUBLE:
      work_line_->CheckUnaryOpWide(this, inst, reg_types_.DoubleLo(), reg_types_.DoubleHi(),
                                   reg_types_.DoubleLo(), reg_types_.DoubleHi());
      break;
    case Instruction::INT_TO_LONG:
      work_line_->CheckUnaryOpToWide(this, inst, reg_types_.LongLo(), reg_types_.LongHi(),
                                     reg_types_.Integer());
      break;
    case Instruction::INT_TO_FLOAT:
      work_line_->CheckUnaryOp(this, inst, reg_types_.Float(), reg_types_.Integer());
      break;
    case Instruction::INT_TO_DOUBLE:
      work_line_->CheckUnaryOpToWide(this, inst, reg_types_.DoubleLo(), reg_types_.DoubleHi(),
                                     reg_types_.Integer());
      break;
    case Instruction::LONG_TO_INT:
      work_line_->CheckUnaryOpFromWide(this, inst, reg_types_.Integer(),
                                       reg_types_.LongLo(), reg_types_.LongHi());
      break;
    case Instruction::LONG_TO_FLOAT:
      work_line_->CheckUnaryOpFromWide(this, inst, reg_types_.Float(),
                                       reg_types_.LongLo(), reg_types_.LongHi());
      break;
    case Instruction::LONG_TO_DOUBLE:
      work_line_->CheckUnaryOpWide(this, inst, reg_types_.DoubleLo(), reg_types_.DoubleHi(),
                                   reg_types_.LongLo(), reg_types_.LongHi());
      break;
    case Instruction::FLOAT_TO_INT:
      work_line_->CheckUnaryOp(this, inst, reg_types_.Integer(), reg_types_.Float());
      break;
    case Instruction::FLOAT_TO_LONG:
      work_line_->CheckUnaryOpToWide(this, inst, reg_types_.LongLo(), reg_types_.LongHi(),
                                     reg_types_.Float());
      break;
    case Instruction::FLOAT_TO_DOUBLE:
      work_line_->CheckUnaryOpToWide(this, inst, reg_types_.DoubleLo(), reg_types_.DoubleHi(),
                                     reg_types_.Float());
      break;
    case Instruction::DOUBLE_TO_INT:
      work_line_->CheckUnaryOpFromWide(this, inst, reg_types_.Integer(),
                                       reg_types_.DoubleLo(), reg_types_.DoubleHi());
      break;
    case Instruction::DOUBLE_TO_LONG:
      work_line_->CheckUnaryOpWide(this, inst, reg_types_.LongLo(), reg_types_.LongHi(),
                                   reg_types_.DoubleLo(), reg_types_.DoubleHi());
      break;
    case Instruction::DOUBLE_TO_FLOAT:
      work_line_->CheckUnaryOpFromWide(this, inst, reg_types_.Float(),
                                       reg_types_.DoubleLo(), reg_types_.DoubleHi());
      break;
    case Instruction::INT_TO_BYTE:
      work_line_->CheckUnaryOp(this, inst, reg_types_.Byte(), reg_types_.Integer());
      break;
    case Instruction::INT_TO_CHAR:
      work_line_->CheckUnaryOp(this, inst, reg_types_.Char(), reg_types_.Integer());
      break;
    case Instruction::INT_TO_SHORT:
      work_line_->CheckUnaryOp(this, inst, reg_types_.Short(), reg_types_.Integer());
      break;

    case Instruction::ADD_INT:
    case Instruction::SUB_INT:
    case Instruction::MUL_INT:
    case Instruction::REM_INT:
    case Instruction::DIV_INT:
    case Instruction::SHL_INT:
    case Instruction::SHR_INT:
    case Instruction::USHR_INT:
      work_line_->CheckBinaryOp(this, inst, reg_types_.Integer(), reg_types_.Integer(),
                                reg_types_.Integer(), false);
      break;
    case Instruction::AND_INT:
    case Instruction::OR_INT:
    case Instruction::XOR_INT:
      work_line_->CheckBinaryOp(this, inst, reg_types_.Integer(), reg_types_.Integer(),
                                reg_types_.Integer(), true);
      break;
    case Instruction::ADD_LONG:
    case Instruction::SUB_LONG:
    case Instruction::MUL_LONG:
    case Instruction::DIV_LONG:
    case Instruction::REM_LONG:
    case Instruction::AND_LONG:
    case Instruction::OR_LONG:
    case Instruction::XOR_LONG:
      work_line_->CheckBinaryOpWide(this, inst, reg_types_.LongLo(), reg_types_.LongHi(),
                                    reg_types_.LongLo(), reg_types_.LongHi(),
                                    reg_types_.LongLo(), reg_types_.LongHi());
      break;
    case Instruction::SHL_LONG:
    case Instruction::SHR_LONG:
    case Instruction::USHR_LONG:
      /* shift distance is Int, making these different from other binary operations */
      work_line_->CheckBinaryOpWideShift(this, inst, reg_types_.LongLo(), reg_types_.LongHi(),
                                         reg_types_.Integer());
      break;
    case Instruction::ADD_FLOAT:
    case Instruction::SUB_FLOAT:
    case Instruction::MUL_FLOAT:
    case Instruction::DIV_FLOAT:
    case Instruction::REM_FLOAT:
      work_line_->CheckBinaryOp(this, inst, reg_types_.Float(), reg_types_.Float(),
                                reg_types_.Float(), false);
      break;
    case Instruction::ADD_DOUBLE:
    case Instruction::SUB_DOUBLE:
    case Instruction::MUL_DOUBLE:
    case Instruction::DIV_DOUBLE:
    case Instruction::REM_DOUBLE:
      work_line_->CheckBinaryOpWide(this, inst, reg_types_.DoubleLo(), reg_types_.DoubleHi(),
                                    reg_types_.DoubleLo(), reg_types_.DoubleHi(),
                                    reg_types_.DoubleLo(), reg_types_.DoubleHi());
      break;
    case Instruction::ADD_INT_2ADDR:
    case Instruction::SUB_INT_2ADDR:
    case Instruction::MUL_INT_2ADDR:
    case Instruction::REM_INT_2ADDR:
    case Instruction::SHL_INT_2ADDR:
    case Instruction::SHR_INT_2ADDR:
    case Instruction::USHR_INT_2ADDR:
      work_line_->CheckBinaryOp2addr(this, inst, reg_types_.Integer(), reg_types_.Integer(),
                                     reg_types_.Integer(), false);
      break;
    case Instruction::AND_INT_2ADDR:
    case Instruction::OR_INT_2ADDR:
    case Instruction::XOR_INT_2ADDR:
      work_line_->CheckBinaryOp2addr(this, inst, reg_types_.Integer(), reg_types_.Integer(),
                                     reg_types_.Integer(), true);
      break;
    case Instruction::DIV_INT_2ADDR:
      work_line_->CheckBinaryOp2addr(this, inst, reg_types_.Integer(), reg_types_.Integer(),
                                     reg_types_.Integer(), false);
      break;
    case Instruction::ADD_LONG_2ADDR:
    case Instruction::SUB_LONG_2ADDR:
    case Instruction::MUL_LONG_2ADDR:
    case Instruction::DIV_LONG_2ADDR:
    case Instruction::REM_LONG_2ADDR:
    case Instruction::AND_LONG_2ADDR:
    case Instruction::OR_LONG_2ADDR:
    case Instruction::XOR_LONG_2ADDR:
      work_line_->CheckBinaryOp2addrWide(this, inst, reg_types_.LongLo(), reg_types_.LongHi(),
                                         reg_types_.LongLo(), reg_types_.LongHi(),
                                         reg_types_.LongLo(), reg_types_.LongHi());
      break;
    case Instruction::SHL_LONG_2ADDR:
    case Instruction::SHR_LONG_2ADDR:
    case Instruction::USHR_LONG_2ADDR:
      work_line_->CheckBinaryOp2addrWideShift(this, inst, reg_types_.LongLo(), reg_types_.LongHi(),
                                              reg_types_.Integer());
      break;
    case Instruction::ADD_FLOAT_2ADDR:
    case Instruction::SUB_FLOAT_2ADDR:
    case Instruction::MUL_FLOAT_2ADDR:
    case Instruction::DIV_FLOAT_2ADDR:
    case Instruction::REM_FLOAT_2ADDR:
      work_line_->CheckBinaryOp2addr(this, inst, reg_types_.Float(), reg_types_.Float(),
                                     reg_types_.Float(), false);
      break;
    case Instruction::ADD_DOUBLE_2ADDR:
    case Instruction::SUB_DOUBLE_2ADDR:
    case Instruction::MUL_DOUBLE_2ADDR:
    case Instruction::DIV_DOUBLE_2ADDR:
    case Instruction::REM_DOUBLE_2ADDR:
      work_line_->CheckBinaryOp2addrWide(this, inst, reg_types_.DoubleLo(), reg_types_.DoubleHi(),
                                         reg_types_.DoubleLo(),  reg_types_.DoubleHi(),
                                         reg_types_.DoubleLo(), reg_types_.DoubleHi());
      break;
    case Instruction::ADD_INT_LIT16:
    case Instruction::RSUB_INT:
    case Instruction::MUL_INT_LIT16:
    case Instruction::DIV_INT_LIT16:
    case Instruction::REM_INT_LIT16:
      work_line_->CheckLiteralOp(this, inst, reg_types_.Integer(), reg_types_.Integer(), false,
                                 true);
      break;
    case Instruction::AND_INT_LIT16:
    case Instruction::OR_INT_LIT16:
    case Instruction::XOR_INT_LIT16:
      work_line_->CheckLiteralOp(this, inst, reg_types_.Integer(), reg_types_.Integer(), true,
                                 true);
      break;
    case Instruction::ADD_INT_LIT8:
    case Instruction::RSUB_INT_LIT8:
    case Instruction::MUL_INT_LIT8:
    case Instruction::DIV_INT_LIT8:
    case Instruction::REM_INT_LIT8:
    case Instruction::SHL_INT_LIT8:
    case Instruction::SHR_INT_LIT8:
    case Instruction::USHR_INT_LIT8:
      work_line_->CheckLiteralOp(this, inst, reg_types_.Integer(), reg_types_.Integer(), false,
                                 false);
      break;
    case Instruction::AND_INT_LIT8:
    case Instruction::OR_INT_LIT8:
    case Instruction::XOR_INT_LIT8:
      work_line_->CheckLiteralOp(this, inst, reg_types_.Integer(), reg_types_.Integer(), true,
                                 false);
      break;

    // Special instructions.
    case Instruction::RETURN_VOID_BARRIER:
      if (!IsConstructor() || IsStatic()) {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "return-void-barrier not expected";
      }
      break;
    // Note: the following instructions encode offsets derived from class linking.
    // As such they use Class*/Field*/AbstractMethod* as these offsets only have
    // meaning if the class linking and resolution were successful.
    case Instruction::IGET_QUICK:
      VerifyIGetQuick(inst, reg_types_.Integer(), true);
      break;
    case Instruction::IGET_WIDE_QUICK:
      VerifyIGetQuick(inst, reg_types_.LongLo(), true);
      break;
    case Instruction::IGET_OBJECT_QUICK:
      VerifyIGetQuick(inst, reg_types_.JavaLangObject(false), false);
      break;
    case Instruction::IPUT_QUICK:
      VerifyIPutQuick(inst, reg_types_.Integer(), true);
      break;
    case Instruction::IPUT_BOOLEAN_QUICK:
        VerifyIPutQuick(inst, reg_types_.Boolean(), true);
      break;
    case Instruction::IPUT_BYTE_QUICK:
        VerifyIPutQuick(inst, reg_types_.Byte(), true);
      break;
    case Instruction::IPUT_CHAR_QUICK:
        VerifyIPutQuick(inst, reg_types_.Char(), true);
      break;
    case Instruction::IPUT_SHORT_QUICK:
        VerifyIPutQuick(inst, reg_types_.Short(), true);
      break;
    case Instruction::IPUT_WIDE_QUICK:
      VerifyIPutQuick(inst, reg_types_.LongLo(), true);
      break;
    case Instruction::IPUT_OBJECT_QUICK:
      VerifyIPutQuick(inst, reg_types_.JavaLangObject(false), false);
      break;
    case Instruction::INVOKE_VIRTUAL_QUICK:
    case Instruction::INVOKE_VIRTUAL_RANGE_QUICK: {
      bool is_range = (inst->Opcode() == Instruction::INVOKE_VIRTUAL_RANGE_QUICK);
      mirror::ArtMethod* called_method = VerifyInvokeVirtualQuickArgs(inst, is_range);
      if (called_method != nullptr) {
        const char* descriptor = called_method->GetReturnTypeDescriptor();
        const RegType& return_type = reg_types_.FromDescriptor(GetClassLoader(), descriptor, false);
        if (!return_type.IsLowHalf()) {
          work_line_->SetResultRegisterType(this, return_type);
        } else {
          work_line_->SetResultRegisterTypeWide(return_type, return_type.HighHalf(&reg_types_));
        }
        just_set_result = true;
      }
      break;
    }

    /* These should never appear during verification. */
    case Instruction::UNUSED_3E:
    case Instruction::UNUSED_3F:
    case Instruction::UNUSED_40:
    case Instruction::UNUSED_41:
    case Instruction::UNUSED_42:
    case Instruction::UNUSED_43:
    case Instruction::UNUSED_79:
    case Instruction::UNUSED_7A:
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
    case Instruction::UNUSED_FF:
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "Unexpected opcode " << inst->DumpString(dex_file_);
      break;

    /*
     * DO NOT add a "default" clause here. Without it the compiler will
     * complain if an instruction is missing (which is desirable).
     */
  }  // end - switch (dec_insn.opcode)

  if (have_pending_hard_failure_) {
    if (Runtime::Current()->IsCompiler()) {
      /* When compiling, check that the last failure is a hard failure */
      CHECK_EQ(failures_[failures_.size() - 1], VERIFY_ERROR_BAD_CLASS_HARD);
    }
    /* immediate failure, reject class */
    info_messages_ << "Rejecting opcode " << inst->DumpString(dex_file_);
    return false;
  } else if (have_pending_runtime_throw_failure_) {
    /* checking interpreter will throw, mark following code as unreachable */
    opcode_flags = Instruction::kThrow;
  }
  /*
   * If we didn't just set the result register, clear it out. This ensures that you can only use
   * "move-result" immediately after the result is set. (We could check this statically, but it's
   * not expensive and it makes our debugging output cleaner.)
   */
  if (!just_set_result) {
    work_line_->SetResultTypeToUnknown(this);
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
    if (nullptr != branch_line.get()) {
      if (!UpdateRegisters(work_insn_idx_ + branch_target, branch_line.get(), false)) {
        return false;
      }
    } else {
      if (!UpdateRegisters(work_insn_idx_ + branch_target, work_line_.get(), false)) {
        return false;
      }
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
      if (!UpdateRegisters(abs_offset, work_line_.get(), false)) {
        return false;
      }
    }
  }

  /*
   * Handle instructions that can throw and that are sitting in a "try" block. (If they're not in a
   * "try" block when they throw, control transfers out of the method.)
   */
  if ((opcode_flags & Instruction::kThrow) != 0 && insn_flags_[work_insn_idx_].IsInTry()) {
    bool has_catch_all_handler = false;
    CatchHandlerIterator iterator(*code_item_, work_insn_idx_);

    // Need the linker to try and resolve the handled class to check if it's Throwable.
    ClassLinker* linker = Runtime::Current()->GetClassLinker();

    for (; iterator.HasNext(); iterator.Next()) {
      uint16_t handler_type_idx = iterator.GetHandlerTypeIndex();
      if (handler_type_idx == DexFile::kDexNoIndex16) {
        has_catch_all_handler = true;
      } else {
        // It is also a catch-all if it is java.lang.Throwable.
        mirror::Class* klass = linker->ResolveType(*dex_file_, handler_type_idx, dex_cache_,
                                                   class_loader_);
        if (klass != nullptr) {
          if (klass == mirror::Throwable::GetJavaLangThrowable()) {
            has_catch_all_handler = true;
          }
        } else {
          // Clear exception.
          DCHECK(self_->IsExceptionPending());
          self_->ClearException();
        }
      }
      /*
       * Merge registers into the "catch" block. We want to use the "savedRegs" rather than
       * "work_regs", because at runtime the exception will be thrown before the instruction
       * modifies any registers.
       */
      if (!UpdateRegisters(iterator.GetHandlerAddress(), saved_line_.get(), false)) {
        return false;
      }
    }

    /*
     * If the monitor stack depth is nonzero, there must be a "catch all" handler for this
     * instruction. This does apply to monitor-exit because of async exception handling.
     */
    if (work_line_->MonitorStackDepth() > 0 && !has_catch_all_handler) {
      /*
       * The state in work_line reflects the post-execution state. If the current instruction is a
       * monitor-enter and the monitor stack was empty, we don't need a catch-all (if it throws,
       * it will do so before grabbing the lock).
       */
      if (inst->Opcode() != Instruction::MONITOR_ENTER || work_line_->MonitorStackDepth() != 1) {
        Fail(VERIFY_ERROR_BAD_CLASS_HARD)
            << "expected to be within a catch-all for an instruction where a monitor is held";
        return false;
      }
    }
  }

  /* Handle "continue". Tag the next consecutive instruction.
   *  Note: Keep the code handling "continue" case below the "branch" and "switch" cases,
   *        because it changes work_line_ when performing peephole optimization
   *        and this change should not be used in those cases.
   */
  if ((opcode_flags & Instruction::kContinue) != 0) {
    DCHECK_EQ(Instruction::At(code_item_->insns_ + work_insn_idx_), inst);
    uint32_t next_insn_idx = work_insn_idx_ + inst->SizeInCodeUnits();
    if (next_insn_idx >= code_item_->insns_size_in_code_units_) {
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "Execution can walk off end of code area";
      return false;
    }
    // The only way to get to a move-exception instruction is to get thrown there. Make sure the
    // next instruction isn't one.
    if (!CheckNotMoveException(code_item_->insns_, next_insn_idx)) {
      return false;
    }
    if (nullptr != fallthrough_line.get()) {
      // Make workline consistent with fallthrough computed from peephole optimization.
      work_line_->CopyFromLine(fallthrough_line.get());
    }
    if (insn_flags_[next_insn_idx].IsReturn()) {
      // For returns we only care about the operand to the return, all other registers are dead.
      const Instruction* ret_inst = Instruction::At(code_item_->insns_ + next_insn_idx);
      Instruction::Code opcode = ret_inst->Opcode();
      if ((opcode == Instruction::RETURN_VOID) || (opcode == Instruction::RETURN_VOID_BARRIER)) {
        work_line_->MarkAllRegistersAsConflicts(this);
      } else {
        if (opcode == Instruction::RETURN_WIDE) {
          work_line_->MarkAllRegistersAsConflictsExceptWide(this, ret_inst->VRegA_11x());
        } else {
          work_line_->MarkAllRegistersAsConflictsExcept(this, ret_inst->VRegA_11x());
        }
      }
    }
    RegisterLine* next_line = reg_table_.GetLine(next_insn_idx);
    if (next_line != nullptr) {
      // Merge registers into what we have for the next instruction, and set the "changed" flag if
      // needed. If the merge changes the state of the registers then the work line will be
      // updated.
      if (!UpdateRegisters(next_insn_idx, work_line_.get(), true)) {
        return false;
      }
    } else {
      /*
       * We're not recording register data for the next instruction, so we don't know what the
       * prior state was. We have to assume that something has changed and re-evaluate it.
       */
      insn_flags_[next_insn_idx].SetChanged();
    }
  }

  /* If we're returning from the method, make sure monitor stack is empty. */
  if ((opcode_flags & Instruction::kReturn) != 0) {
    if (!work_line_->VerifyMonitorStackEmpty(this)) {
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
    DCHECK_EQ(Instruction::At(code_item_->insns_ + work_insn_idx_), inst);
    *start_guess = work_insn_idx_ + inst->SizeInCodeUnits();
  } else if ((opcode_flags & Instruction::kBranch) != 0) {
    /* we're still okay if branch_target is zero */
    *start_guess = work_insn_idx_ + branch_target;
  }

  DCHECK_LT(*start_guess, code_item_->insns_size_in_code_units_);
  DCHECK(insn_flags_[*start_guess].IsOpcode());

  return true;
}  // NOLINT(readability/fn_size)

const RegType& MethodVerifier::ResolveClassAndCheckAccess(uint32_t class_idx) {
  const char* descriptor = dex_file_->StringByTypeIdx(class_idx);
  const RegType& referrer = GetDeclaringClass();
  mirror::Class* klass = dex_cache_->GetResolvedType(class_idx);
  const RegType& result = klass != nullptr ?
      reg_types_.FromClass(descriptor, klass, klass->CannotBeAssignedFromOtherTypes()) :
      reg_types_.FromDescriptor(GetClassLoader(), descriptor, false);
  if (result.IsConflict()) {
    Fail(VERIFY_ERROR_BAD_CLASS_SOFT) << "accessing broken descriptor '" << descriptor
        << "' in " << referrer;
    return result;
  }
  if (klass == nullptr && !result.IsUnresolvedTypes()) {
    dex_cache_->SetResolvedType(class_idx, result.GetClass());
  }
  // Check if access is allowed. Unresolved types use xxxWithAccessCheck to
  // check at runtime if access is allowed and so pass here. If result is
  // primitive, skip the access check.
  if (result.IsNonZeroReferenceTypes() && !result.IsUnresolvedTypes() &&
      !referrer.IsUnresolvedTypes() && !referrer.CanAccess(result)) {
    Fail(VERIFY_ERROR_ACCESS_CLASS) << "illegal class access: '"
                                    << referrer << "' -> '" << result << "'";
  }
  return result;
}

const RegType& MethodVerifier::GetCaughtExceptionType() {
  const RegType* common_super = nullptr;
  if (code_item_->tries_size_ != 0) {
    const byte* handlers_ptr = DexFile::GetCatchHandlerData(*code_item_, 0);
    uint32_t handlers_size = DecodeUnsignedLeb128(&handlers_ptr);
    for (uint32_t i = 0; i < handlers_size; i++) {
      CatchHandlerIterator iterator(handlers_ptr);
      for (; iterator.HasNext(); iterator.Next()) {
        if (iterator.GetHandlerAddress() == (uint32_t) work_insn_idx_) {
          if (iterator.GetHandlerTypeIndex() == DexFile::kDexNoIndex16) {
            common_super = &reg_types_.JavaLangThrowable(false);
          } else {
            const RegType& exception = ResolveClassAndCheckAccess(iterator.GetHandlerTypeIndex());
            if (!reg_types_.JavaLangThrowable(false).IsAssignableFrom(exception)) {
              if (exception.IsUnresolvedTypes()) {
                // We don't know enough about the type. Fail here and let runtime handle it.
                Fail(VERIFY_ERROR_NO_CLASS) << "unresolved exception class " << exception;
                return exception;
              } else {
                Fail(VERIFY_ERROR_BAD_CLASS_SOFT) << "unexpected non-exception class " << exception;
                return reg_types_.Conflict();
              }
            } else if (common_super == nullptr) {
              common_super = &exception;
            } else if (common_super->Equals(exception)) {
              // odd case, but nothing to do
            } else {
              common_super = &common_super->Merge(exception, &reg_types_);
              CHECK(reg_types_.JavaLangThrowable(false).IsAssignableFrom(*common_super));
            }
          }
        }
      }
      handlers_ptr = iterator.EndDataPointer();
    }
  }
  if (common_super == nullptr) {
    /* no catch blocks, or no catches with classes we can find */
    Fail(VERIFY_ERROR_BAD_CLASS_SOFT) << "unable to find exception handler";
    return reg_types_.Conflict();
  }
  return *common_super;
}

mirror::ArtMethod* MethodVerifier::ResolveMethodAndCheckAccess(uint32_t dex_method_idx,
                                                               MethodType method_type) {
  const DexFile::MethodId& method_id = dex_file_->GetMethodId(dex_method_idx);
  const RegType& klass_type = ResolveClassAndCheckAccess(method_id.class_idx_);
  if (klass_type.IsConflict()) {
    std::string append(" in attempt to access method ");
    append += dex_file_->GetMethodName(method_id);
    AppendToLastFailMessage(append);
    return nullptr;
  }
  if (klass_type.IsUnresolvedTypes()) {
    return nullptr;  // Can't resolve Class so no more to do here
  }
  mirror::Class* klass = klass_type.GetClass();
  const RegType& referrer = GetDeclaringClass();
  mirror::ArtMethod* res_method = dex_cache_->GetResolvedMethod(dex_method_idx);
  if (res_method == nullptr) {
    const char* name = dex_file_->GetMethodName(method_id);
    const Signature signature = dex_file_->GetMethodSignature(method_id);

    if (method_type == METHOD_DIRECT || method_type == METHOD_STATIC) {
      res_method = klass->FindDirectMethod(name, signature);
    } else if (method_type == METHOD_INTERFACE) {
      res_method = klass->FindInterfaceMethod(name, signature);
    } else {
      res_method = klass->FindVirtualMethod(name, signature);
    }
    if (res_method != nullptr) {
      dex_cache_->SetResolvedMethod(dex_method_idx, res_method);
    } else {
      // If a virtual or interface method wasn't found with the expected type, look in
      // the direct methods. This can happen when the wrong invoke type is used or when
      // a class has changed, and will be flagged as an error in later checks.
      if (method_type == METHOD_INTERFACE || method_type == METHOD_VIRTUAL) {
        res_method = klass->FindDirectMethod(name, signature);
      }
      if (res_method == nullptr) {
        Fail(VERIFY_ERROR_NO_METHOD) << "couldn't find method "
                                     << PrettyDescriptor(klass) << "." << name
                                     << " " << signature;
        return nullptr;
      }
    }
  }
  // Make sure calls to constructors are "direct". There are additional restrictions but we don't
  // enforce them here.
  if (res_method->IsConstructor() && method_type != METHOD_DIRECT) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "rejecting non-direct call to constructor "
                                      << PrettyMethod(res_method);
    return nullptr;
  }
  // Disallow any calls to class initializers.
  if (res_method->IsClassInitializer()) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "rejecting call to class initializer "
                                      << PrettyMethod(res_method);
    return nullptr;
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
    return nullptr;
  }
  // Check that interface methods match interface classes.
  if (klass->IsInterface() && method_type != METHOD_INTERFACE) {
    Fail(VERIFY_ERROR_CLASS_CHANGE) << "non-interface method " << PrettyMethod(res_method)
                                    << " is in an interface class " << PrettyClass(klass);
    return nullptr;
  } else if (!klass->IsInterface() && method_type == METHOD_INTERFACE) {
    Fail(VERIFY_ERROR_CLASS_CHANGE) << "interface method " << PrettyMethod(res_method)
                                    << " is in a non-interface class " << PrettyClass(klass);
    return nullptr;
  }
  // See if the method type implied by the invoke instruction matches the access flags for the
  // target method.
  if ((method_type == METHOD_DIRECT && !res_method->IsDirect()) ||
      (method_type == METHOD_STATIC && !res_method->IsStatic()) ||
      ((method_type == METHOD_VIRTUAL || method_type == METHOD_INTERFACE) && res_method->IsDirect())
      ) {
    Fail(VERIFY_ERROR_CLASS_CHANGE) << "invoke type (" << method_type << ") does not match method "
                                       " type of " << PrettyMethod(res_method);
    return nullptr;
  }
  return res_method;
}

template <class T>
mirror::ArtMethod* MethodVerifier::VerifyInvocationArgsFromIterator(T* it, const Instruction* inst,
                                                                    MethodType method_type,
                                                                    bool is_range,
                                                                    mirror::ArtMethod* res_method) {
  // We use vAA as our expected arg count, rather than res_method->insSize, because we need to
  // match the call to the signature. Also, we might be calling through an abstract method
  // definition (which doesn't have register count values).
  const size_t expected_args = (is_range) ? inst->VRegA_3rc() : inst->VRegA_35c();
  /* caught by static verifier */
  DCHECK(is_range || expected_args <= 5);
  if (expected_args > code_item_->outs_size_) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "invalid argument count (" << expected_args
        << ") exceeds outsSize (" << code_item_->outs_size_ << ")";
    return nullptr;
  }

  uint32_t arg[5];
  if (!is_range) {
    inst->GetVarArgs(arg);
  }
  uint32_t sig_registers = 0;

  /*
   * Check the "this" argument, which must be an instance of the class that declared the method.
   * For an interface class, we don't do the full interface merge (see JoinClass), so we can't do a
   * rigorous check here (which is okay since we have to do it at runtime).
   */
  if (method_type != METHOD_STATIC) {
    const RegType& actual_arg_type = work_line_->GetInvocationThis(this, inst, is_range);
    if (actual_arg_type.IsConflict()) {  // GetInvocationThis failed.
      CHECK(have_pending_hard_failure_);
      return nullptr;
    }
    if (actual_arg_type.IsUninitializedReference()) {
      if (res_method) {
        if (!res_method->IsConstructor()) {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "'this' arg must be initialized";
          return nullptr;
        }
      } else {
        // Check whether the name of the called method is "<init>"
        const uint32_t method_idx = (is_range) ? inst->VRegB_3rc() : inst->VRegB_35c();
        if (strcmp(dex_file_->GetMethodName(dex_file_->GetMethodId(method_idx)), "<init>") != 0) {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "'this' arg must be initialized";
          return nullptr;
        }
      }
    }
    if (method_type != METHOD_INTERFACE && !actual_arg_type.IsZero()) {
      const RegType* res_method_class;
      if (res_method != nullptr) {
        mirror::Class* klass = res_method->GetDeclaringClass();
        std::string temp;
        res_method_class = &reg_types_.FromClass(klass->GetDescriptor(&temp), klass,
                                                 klass->CannotBeAssignedFromOtherTypes());
      } else {
        const uint32_t method_idx = (is_range) ? inst->VRegB_3rc() : inst->VRegB_35c();
        const uint16_t class_idx = dex_file_->GetMethodId(method_idx).class_idx_;
        res_method_class = &reg_types_.FromDescriptor(GetClassLoader(),
                                                      dex_file_->StringByTypeIdx(class_idx),
                                                      false);
      }
      if (!res_method_class->IsAssignableFrom(actual_arg_type)) {
        Fail(actual_arg_type.IsUnresolvedTypes() ? VERIFY_ERROR_NO_CLASS:
            VERIFY_ERROR_BAD_CLASS_SOFT) << "'this' argument '" << actual_arg_type
                << "' not instance of '" << *res_method_class << "'";
        // Continue on soft failures. We need to find possible hard failures to avoid problems in
        // the compiler.
        if (have_pending_hard_failure_) {
          return nullptr;
        }
      }
    }
    sig_registers = 1;
  }

  for ( ; it->HasNext(); it->Next()) {
    if (sig_registers >= expected_args) {
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "Rejecting invocation, expected " << inst->VRegA() <<
          " arguments, found " << sig_registers << " or more.";
      return nullptr;
    }

    const char* param_descriptor = it->GetDescriptor();

    if (param_descriptor == nullptr) {
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "Rejecting invocation because of missing signature "
          "component";
      return nullptr;
    }

    const RegType& reg_type = reg_types_.FromDescriptor(GetClassLoader(), param_descriptor, false);
    uint32_t get_reg = is_range ? inst->VRegC_3rc() + static_cast<uint32_t>(sig_registers) :
        arg[sig_registers];
    if (reg_type.IsIntegralTypes()) {
      const RegType& src_type = work_line_->GetRegisterType(this, get_reg);
      if (!src_type.IsIntegralTypes()) {
        Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "register v" << get_reg << " has type " << src_type
            << " but expected " << reg_type;
        return res_method;
      }
    } else if (!work_line_->VerifyRegisterType(this, get_reg, reg_type)) {
      // Continue on soft failures. We need to find possible hard failures to avoid problems in the
      // compiler.
      if (have_pending_hard_failure_) {
        return res_method;
      }
    }
    sig_registers += reg_type.IsLongOrDoubleTypes() ?  2 : 1;
  }
  if (expected_args != sig_registers) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "Rejecting invocation, expected " << expected_args <<
        " arguments, found " << sig_registers;
    return nullptr;
  }
  return res_method;
}

void MethodVerifier::VerifyInvocationArgsUnresolvedMethod(const Instruction* inst,
                                                          MethodType method_type,
                                                          bool is_range) {
  // As the method may not have been resolved, make this static check against what we expect.
  // The main reason for this code block is to fail hard when we find an illegal use, e.g.,
  // wrong number of arguments or wrong primitive types, even if the method could not be resolved.
  const uint32_t method_idx = (is_range) ? inst->VRegB_3rc() : inst->VRegB_35c();
  DexFileParameterIterator it(*dex_file_,
                              dex_file_->GetProtoId(dex_file_->GetMethodId(method_idx).proto_idx_));
  VerifyInvocationArgsFromIterator<DexFileParameterIterator>(&it, inst, method_type, is_range,
                                                             nullptr);
}

class MethodParamListDescriptorIterator {
 public:
  explicit MethodParamListDescriptorIterator(mirror::ArtMethod* res_method) :
      res_method_(res_method), pos_(0), params_(res_method->GetParameterTypeList()),
      params_size_(params_ == nullptr ? 0 : params_->Size()) {
  }

  bool HasNext() {
    return pos_ < params_size_;
  }

  void Next() {
    ++pos_;
  }

  const char* GetDescriptor() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return res_method_->GetTypeDescriptorFromTypeIdx(params_->GetTypeItem(pos_).type_idx_);
  }

 private:
  mirror::ArtMethod* res_method_;
  size_t pos_;
  const DexFile::TypeList* params_;
  const size_t params_size_;
};

mirror::ArtMethod* MethodVerifier::VerifyInvocationArgs(const Instruction* inst,
                                                             MethodType method_type,
                                                             bool is_range,
                                                             bool is_super) {
  // Resolve the method. This could be an abstract or concrete method depending on what sort of call
  // we're making.
  const uint32_t method_idx = (is_range) ? inst->VRegB_3rc() : inst->VRegB_35c();

  mirror::ArtMethod* res_method = ResolveMethodAndCheckAccess(method_idx, method_type);
  if (res_method == nullptr) {  // error or class is unresolved
    // Check what we can statically.
    if (!have_pending_hard_failure_) {
      VerifyInvocationArgsUnresolvedMethod(inst, method_type, is_range);
    }
    return nullptr;
  }

  // If we're using invoke-super(method), make sure that the executing method's class' superclass
  // has a vtable entry for the target method.
  if (is_super) {
    DCHECK(method_type == METHOD_VIRTUAL);
    const RegType& super = GetDeclaringClass().GetSuperClass(&reg_types_);
    if (super.IsUnresolvedTypes()) {
      Fail(VERIFY_ERROR_NO_METHOD) << "unknown super class in invoke-super from "
                                   << PrettyMethod(dex_method_idx_, *dex_file_)
                                   << " to super " << PrettyMethod(res_method);
      return nullptr;
    }
    mirror::Class* super_klass = super.GetClass();
    if (res_method->GetMethodIndex() >= super_klass->GetVTableLength()) {
      Fail(VERIFY_ERROR_NO_METHOD) << "invalid invoke-super from "
                                   << PrettyMethod(dex_method_idx_, *dex_file_)
                                   << " to super " << super
                                   << "." << res_method->GetName()
                                   << res_method->GetSignature();
      return nullptr;
    }
  }

  // Process the target method's signature. This signature may or may not
  MethodParamListDescriptorIterator it(res_method);
  return VerifyInvocationArgsFromIterator<MethodParamListDescriptorIterator>(&it, inst, method_type,
                                                                             is_range, res_method);
}

mirror::ArtMethod* MethodVerifier::GetQuickInvokedMethod(const Instruction* inst,
                                                         RegisterLine* reg_line, bool is_range) {
  DCHECK(inst->Opcode() == Instruction::INVOKE_VIRTUAL_QUICK ||
         inst->Opcode() == Instruction::INVOKE_VIRTUAL_RANGE_QUICK);
  const RegType& actual_arg_type = reg_line->GetInvocationThis(this, inst, is_range);
  if (!actual_arg_type.HasClass()) {
    VLOG(verifier) << "Failed to get mirror::Class* from '" << actual_arg_type << "'";
    return nullptr;
  }
  mirror::Class* klass = actual_arg_type.GetClass();
  mirror::Class* dispatch_class;
  if (klass->IsInterface()) {
    // Derive Object.class from Class.class.getSuperclass().
    mirror::Class* object_klass = klass->GetClass()->GetSuperClass();
    CHECK(object_klass->IsObjectClass());
    dispatch_class = object_klass;
  } else {
    dispatch_class = klass;
  }
  CHECK(dispatch_class->HasVTable()) << PrettyDescriptor(dispatch_class);
  uint16_t vtable_index = is_range ? inst->VRegB_3rc() : inst->VRegB_35c();
  CHECK_LT(static_cast<int32_t>(vtable_index), dispatch_class->GetVTableLength())
      << PrettyDescriptor(klass);
  mirror::ArtMethod* res_method = dispatch_class->GetVTableEntry(vtable_index);
  CHECK(!self_->IsExceptionPending());
  return res_method;
}

mirror::ArtMethod* MethodVerifier::VerifyInvokeVirtualQuickArgs(const Instruction* inst,
                                                                     bool is_range) {
  DCHECK(Runtime::Current()->IsStarted());
  mirror::ArtMethod* res_method = GetQuickInvokedMethod(inst, work_line_.get(),
                                                             is_range);
  if (res_method == nullptr) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "Cannot infer method from " << inst->Name();
    return nullptr;
  }
  CHECK(!res_method->IsDirect() && !res_method->IsStatic());

  // We use vAA as our expected arg count, rather than res_method->insSize, because we need to
  // match the call to the signature. Also, we might be calling through an abstract method
  // definition (which doesn't have register count values).
  const RegType& actual_arg_type = work_line_->GetInvocationThis(this, inst, is_range);
  if (actual_arg_type.IsConflict()) {  // GetInvocationThis failed.
    return nullptr;
  }
  const size_t expected_args = (is_range) ? inst->VRegA_3rc() : inst->VRegA_35c();
  /* caught by static verifier */
  DCHECK(is_range || expected_args <= 5);
  if (expected_args > code_item_->outs_size_) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "invalid argument count (" << expected_args
        << ") exceeds outsSize (" << code_item_->outs_size_ << ")";
    return nullptr;
  }

  /*
   * Check the "this" argument, which must be an instance of the class that declared the method.
   * For an interface class, we don't do the full interface merge (see JoinClass), so we can't do a
   * rigorous check here (which is okay since we have to do it at runtime).
   */
  if (actual_arg_type.IsUninitializedReference() && !res_method->IsConstructor()) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "'this' arg must be initialized";
    return nullptr;
  }
  if (!actual_arg_type.IsZero()) {
    mirror::Class* klass = res_method->GetDeclaringClass();
    std::string temp;
    const RegType& res_method_class =
        reg_types_.FromClass(klass->GetDescriptor(&temp), klass,
                             klass->CannotBeAssignedFromOtherTypes());
    if (!res_method_class.IsAssignableFrom(actual_arg_type)) {
      Fail(actual_arg_type.IsUnresolvedTypes() ? VERIFY_ERROR_NO_CLASS :
          VERIFY_ERROR_BAD_CLASS_SOFT) << "'this' argument '" << actual_arg_type
          << "' not instance of '" << res_method_class << "'";
      return nullptr;
    }
  }
  /*
   * Process the target method's signature. This signature may or may not
   * have been verified, so we can't assume it's properly formed.
   */
  const DexFile::TypeList* params = res_method->GetParameterTypeList();
  size_t params_size = params == nullptr ? 0 : params->Size();
  uint32_t arg[5];
  if (!is_range) {
    inst->GetVarArgs(arg);
  }
  size_t actual_args = 1;
  for (size_t param_index = 0; param_index < params_size; param_index++) {
    if (actual_args >= expected_args) {
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "Rejecting invalid call to '" << PrettyMethod(res_method)
                                        << "'. Expected " << expected_args
                                         << " arguments, processing argument " << actual_args
                                        << " (where longs/doubles count twice).";
      return nullptr;
    }
    const char* descriptor =
        res_method->GetTypeDescriptorFromTypeIdx(params->GetTypeItem(param_index).type_idx_);
    if (descriptor == nullptr) {
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "Rejecting invocation of " << PrettyMethod(res_method)
                                        << " missing signature component";
      return nullptr;
    }
    const RegType& reg_type = reg_types_.FromDescriptor(GetClassLoader(), descriptor, false);
    uint32_t get_reg = is_range ? inst->VRegC_3rc() + actual_args : arg[actual_args];
    if (!work_line_->VerifyRegisterType(this, get_reg, reg_type)) {
      return res_method;
    }
    actual_args = reg_type.IsLongOrDoubleTypes() ? actual_args + 2 : actual_args + 1;
  }
  if (actual_args != expected_args) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "Rejecting invocation of " << PrettyMethod(res_method)
              << " expected " << expected_args << " arguments, found " << actual_args;
    return nullptr;
  } else {
    return res_method;
  }
}

void MethodVerifier::VerifyNewArray(const Instruction* inst, bool is_filled, bool is_range) {
  uint32_t type_idx;
  if (!is_filled) {
    DCHECK_EQ(inst->Opcode(), Instruction::NEW_ARRAY);
    type_idx = inst->VRegC_22c();
  } else if (!is_range) {
    DCHECK_EQ(inst->Opcode(), Instruction::FILLED_NEW_ARRAY);
    type_idx = inst->VRegB_35c();
  } else {
    DCHECK_EQ(inst->Opcode(), Instruction::FILLED_NEW_ARRAY_RANGE);
    type_idx = inst->VRegB_3rc();
  }
  const RegType& res_type = ResolveClassAndCheckAccess(type_idx);
  if (res_type.IsConflict()) {  // bad class
    DCHECK_NE(failures_.size(), 0U);
  } else {
    // TODO: check Compiler::CanAccessTypeWithoutChecks returns false when res_type is unresolved
    if (!res_type.IsArrayTypes()) {
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "new-array on non-array class " << res_type;
    } else if (!is_filled) {
      /* make sure "size" register is valid type */
      work_line_->VerifyRegisterType(this, inst->VRegB_22c(), reg_types_.Integer());
      /* set register type to array class */
      const RegType& precise_type = reg_types_.FromUninitialized(res_type);
      work_line_->SetRegisterType(this, inst->VRegA_22c(), precise_type);
    } else {
      // Verify each register. If "arg_count" is bad, VerifyRegisterType() will run off the end of
      // the list and fail. It's legal, if silly, for arg_count to be zero.
      const RegType& expected_type = reg_types_.GetComponentType(res_type, GetClassLoader());
      uint32_t arg_count = (is_range) ? inst->VRegA_3rc() : inst->VRegA_35c();
      uint32_t arg[5];
      if (!is_range) {
        inst->GetVarArgs(arg);
      }
      for (size_t ui = 0; ui < arg_count; ui++) {
        uint32_t get_reg = is_range ? inst->VRegC_3rc() + ui : arg[ui];
        if (!work_line_->VerifyRegisterType(this, get_reg, expected_type)) {
          work_line_->SetResultRegisterType(this, reg_types_.Conflict());
          return;
        }
      }
      // filled-array result goes into "result" register
      const RegType& precise_type = reg_types_.FromUninitialized(res_type);
      work_line_->SetResultRegisterType(this, precise_type);
    }
  }
}

void MethodVerifier::VerifyAGet(const Instruction* inst,
                                const RegType& insn_type, bool is_primitive) {
  const RegType& index_type = work_line_->GetRegisterType(this, inst->VRegC_23x());
  if (!index_type.IsArrayIndexTypes()) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "Invalid reg type for array index (" << index_type << ")";
  } else {
    const RegType& array_type = work_line_->GetRegisterType(this, inst->VRegB_23x());
    if (array_type.IsZero()) {
      // Null array class; this code path will fail at runtime. Infer a merge-able type from the
      // instruction type. TODO: have a proper notion of bottom here.
      if (!is_primitive || insn_type.IsCategory1Types()) {
        // Reference or category 1
        work_line_->SetRegisterType(this, inst->VRegA_23x(), reg_types_.Zero());
      } else {
        // Category 2
        work_line_->SetRegisterTypeWide(this, inst->VRegA_23x(),
                                        reg_types_.FromCat2ConstLo(0, false),
                                        reg_types_.FromCat2ConstHi(0, false));
      }
    } else if (!array_type.IsArrayTypes()) {
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "not array type " << array_type << " with aget";
    } else {
      /* verify the class */
      const RegType& component_type = reg_types_.GetComponentType(array_type, GetClassLoader());
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
        if (!component_type.IsLowHalf()) {
          work_line_->SetRegisterType(this, inst->VRegA_23x(), component_type);
        } else {
          work_line_->SetRegisterTypeWide(this, inst->VRegA_23x(), component_type,
                                          component_type.HighHalf(&reg_types_));
        }
      }
    }
  }
}

void MethodVerifier::VerifyPrimitivePut(const RegType& target_type, const RegType& insn_type,
                                        const uint32_t vregA) {
  // Primitive assignability rules are weaker than regular assignability rules.
  bool instruction_compatible;
  bool value_compatible;
  const RegType& value_type = work_line_->GetRegisterType(this, vregA);
  if (target_type.IsIntegralTypes()) {
    instruction_compatible = target_type.Equals(insn_type);
    value_compatible = value_type.IsIntegralTypes();
  } else if (target_type.IsFloat()) {
    instruction_compatible = insn_type.IsInteger();  // no put-float, so expect put-int
    value_compatible = value_type.IsFloatTypes();
  } else if (target_type.IsLong()) {
    instruction_compatible = insn_type.IsLong();
    // Additional register check: this is not checked statically (as part of VerifyInstructions),
    // as target_type depends on the resolved type of the field.
    if (instruction_compatible && work_line_->NumRegs() > vregA + 1) {
      const RegType& value_type_hi = work_line_->GetRegisterType(this, vregA + 1);
      value_compatible = value_type.IsLongTypes() && value_type.CheckWidePair(value_type_hi);
    } else {
      value_compatible = false;
    }
  } else if (target_type.IsDouble()) {
    instruction_compatible = insn_type.IsLong();  // no put-double, so expect put-long
    // Additional register check: this is not checked statically (as part of VerifyInstructions),
    // as target_type depends on the resolved type of the field.
    if (instruction_compatible && work_line_->NumRegs() > vregA + 1) {
      const RegType& value_type_hi = work_line_->GetRegisterType(this, vregA + 1);
      value_compatible = value_type.IsDoubleTypes() && value_type.CheckWidePair(value_type_hi);
    } else {
      value_compatible = false;
    }
  } else {
    instruction_compatible = false;  // reference with primitive store
    value_compatible = false;  // unused
  }
  if (!instruction_compatible) {
    // This is a global failure rather than a class change failure as the instructions and
    // the descriptors for the type should have been consistent within the same file at
    // compile time.
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "put insn has type '" << insn_type
        << "' but expected type '" << target_type << "'";
    return;
  }
  if (!value_compatible) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "unexpected value in v" << vregA
        << " of type " << value_type << " but expected " << target_type << " for put";
    return;
  }
}

void MethodVerifier::VerifyAPut(const Instruction* inst,
                                const RegType& insn_type, bool is_primitive) {
  const RegType& index_type = work_line_->GetRegisterType(this, inst->VRegC_23x());
  if (!index_type.IsArrayIndexTypes()) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "Invalid reg type for array index (" << index_type << ")";
  } else {
    const RegType& array_type = work_line_->GetRegisterType(this, inst->VRegB_23x());
    if (array_type.IsZero()) {
      // Null array type; this code path will fail at runtime. Infer a merge-able type from the
      // instruction type.
    } else if (!array_type.IsArrayTypes()) {
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "not array type " << array_type << " with aput";
    } else {
      const RegType& component_type = reg_types_.GetComponentType(array_type, GetClassLoader());
      const uint32_t vregA = inst->VRegA_23x();
      if (is_primitive) {
        VerifyPrimitivePut(component_type, insn_type, vregA);
      } else {
        if (!component_type.IsReferenceTypes()) {
          Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "primitive array type " << array_type
              << " source for aput-object";
        } else {
          // The instruction agrees with the type of array, confirm the value to be stored does too
          // Note: we use the instruction type (rather than the component type) for aput-object as
          // incompatible classes will be caught at runtime as an array store exception
          work_line_->VerifyRegisterType(this, vregA, insn_type);
        }
      }
    }
  }
}

mirror::ArtField* MethodVerifier::GetStaticField(int field_idx) {
  const DexFile::FieldId& field_id = dex_file_->GetFieldId(field_idx);
  // Check access to class
  const RegType& klass_type = ResolveClassAndCheckAccess(field_id.class_idx_);
  if (klass_type.IsConflict()) {  // bad class
    AppendToLastFailMessage(StringPrintf(" in attempt to access static field %d (%s) in %s",
                                         field_idx, dex_file_->GetFieldName(field_id),
                                         dex_file_->GetFieldDeclaringClassDescriptor(field_id)));
    return nullptr;
  }
  if (klass_type.IsUnresolvedTypes()) {
    return nullptr;  // Can't resolve Class so no more to do here, will do checking at runtime.
  }
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  mirror::ArtField* field = class_linker->ResolveFieldJLS(*dex_file_, field_idx, dex_cache_,
                                                          class_loader_);
  if (field == nullptr) {
    VLOG(verifier) << "Unable to resolve static field " << field_idx << " ("
              << dex_file_->GetFieldName(field_id) << ") in "
              << dex_file_->GetFieldDeclaringClassDescriptor(field_id);
    DCHECK(self_->IsExceptionPending());
    self_->ClearException();
    return nullptr;
  } else if (!GetDeclaringClass().CanAccessMember(field->GetDeclaringClass(),
                                                  field->GetAccessFlags())) {
    Fail(VERIFY_ERROR_ACCESS_FIELD) << "cannot access static field " << PrettyField(field)
                                    << " from " << GetDeclaringClass();
    return nullptr;
  } else if (!field->IsStatic()) {
    Fail(VERIFY_ERROR_CLASS_CHANGE) << "expected field " << PrettyField(field) << " to be static";
    return nullptr;
  }
  return field;
}

mirror::ArtField* MethodVerifier::GetInstanceField(const RegType& obj_type, int field_idx) {
  const DexFile::FieldId& field_id = dex_file_->GetFieldId(field_idx);
  // Check access to class
  const RegType& klass_type = ResolveClassAndCheckAccess(field_id.class_idx_);
  if (klass_type.IsConflict()) {
    AppendToLastFailMessage(StringPrintf(" in attempt to access instance field %d (%s) in %s",
                                         field_idx, dex_file_->GetFieldName(field_id),
                                         dex_file_->GetFieldDeclaringClassDescriptor(field_id)));
    return nullptr;
  }
  if (klass_type.IsUnresolvedTypes()) {
    return nullptr;  // Can't resolve Class so no more to do here
  }
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  mirror::ArtField* field = class_linker->ResolveFieldJLS(*dex_file_, field_idx, dex_cache_,
                                                          class_loader_);
  if (field == nullptr) {
    VLOG(verifier) << "Unable to resolve instance field " << field_idx << " ("
              << dex_file_->GetFieldName(field_id) << ") in "
              << dex_file_->GetFieldDeclaringClassDescriptor(field_id);
    DCHECK(self_->IsExceptionPending());
    self_->ClearException();
    return nullptr;
  } else if (!GetDeclaringClass().CanAccessMember(field->GetDeclaringClass(),
                                                  field->GetAccessFlags())) {
    Fail(VERIFY_ERROR_ACCESS_FIELD) << "cannot access instance field " << PrettyField(field)
                                    << " from " << GetDeclaringClass();
    return nullptr;
  } else if (field->IsStatic()) {
    Fail(VERIFY_ERROR_CLASS_CHANGE) << "expected field " << PrettyField(field)
                                    << " to not be static";
    return nullptr;
  } else if (obj_type.IsZero()) {
    // Cannot infer and check type, however, access will cause null pointer exception
    return field;
  } else if (!obj_type.IsReferenceTypes()) {
    // Trying to read a field from something that isn't a reference
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "instance field access on object that has "
                                      << "non-reference type " << obj_type;
    return nullptr;
  } else {
    mirror::Class* klass = field->GetDeclaringClass();
    const RegType& field_klass =
        reg_types_.FromClass(dex_file_->GetFieldDeclaringClassDescriptor(field_id),
                             klass, klass->CannotBeAssignedFromOtherTypes());
    if (obj_type.IsUninitializedTypes() &&
        (!IsConstructor() || GetDeclaringClass().Equals(obj_type) ||
            !field_klass.Equals(GetDeclaringClass()))) {
      // Field accesses through uninitialized references are only allowable for constructors where
      // the field is declared in this class
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "cannot access instance field " << PrettyField(field)
                                        << " of a not fully initialized object within the context"
                                        << " of " << PrettyMethod(dex_method_idx_, *dex_file_);
      return nullptr;
    } else if (!field_klass.IsAssignableFrom(obj_type)) {
      // Trying to access C1.field1 using reference of type C2, which is neither C1 or a sub-class
      // of C1. For resolution to occur the declared class of the field must be compatible with
      // obj_type, we've discovered this wasn't so, so report the field didn't exist.
      Fail(VERIFY_ERROR_NO_FIELD) << "cannot access instance field " << PrettyField(field)
                                  << " from object of type " << obj_type;
      return nullptr;
    } else {
      return field;
    }
  }
}

void MethodVerifier::VerifyISGet(const Instruction* inst, const RegType& insn_type,
                                 bool is_primitive, bool is_static) {
  uint32_t field_idx = is_static ? inst->VRegB_21c() : inst->VRegC_22c();
  mirror::ArtField* field;
  if (is_static) {
    field = GetStaticField(field_idx);
  } else {
    const RegType& object_type = work_line_->GetRegisterType(this, inst->VRegB_22c());
    field = GetInstanceField(object_type, field_idx);
  }
  const RegType* field_type = nullptr;
  if (field != nullptr) {
    mirror::Class* field_type_class;
    {
      StackHandleScope<1> hs(self_);
      HandleWrapper<mirror::ArtField> h_field(hs.NewHandleWrapper(&field));
      field_type_class = FieldHelper(h_field).GetType(can_load_classes_);
    }
    if (field_type_class != nullptr) {
      field_type = &reg_types_.FromClass(field->GetTypeDescriptor(), field_type_class,
                                         field_type_class->CannotBeAssignedFromOtherTypes());
    } else {
      DCHECK(!can_load_classes_ || self_->IsExceptionPending());
      self_->ClearException();
    }
  }
  if (field_type == nullptr) {
    const DexFile::FieldId& field_id = dex_file_->GetFieldId(field_idx);
    const char* descriptor = dex_file_->GetFieldTypeDescriptor(field_id);
    field_type = &reg_types_.FromDescriptor(GetClassLoader(), descriptor, false);
  }
  DCHECK(field_type != nullptr);
  const uint32_t vregA = (is_static) ? inst->VRegA_21c() : inst->VRegA_22c();
  if (is_primitive) {
    if (field_type->Equals(insn_type) ||
        (field_type->IsFloat() && insn_type.IsInteger()) ||
        (field_type->IsDouble() && insn_type.IsLong())) {
      // expected that read is of the correct primitive type or that int reads are reading
      // floats or long reads are reading doubles
    } else {
      // This is a global failure rather than a class change failure as the instructions and
      // the descriptors for the type should have been consistent within the same file at
      // compile time
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "expected field " << PrettyField(field)
                                        << " to be of type '" << insn_type
                                        << "' but found type '" << *field_type << "' in get";
      return;
    }
  } else {
    if (!insn_type.IsAssignableFrom(*field_type)) {
      Fail(VERIFY_ERROR_BAD_CLASS_SOFT) << "expected field " << PrettyField(field)
                                        << " to be compatible with type '" << insn_type
                                        << "' but found type '" << *field_type
                                        << "' in Get-object";
      work_line_->SetRegisterType(this, vregA, reg_types_.Conflict());
      return;
    }
  }
  if (!field_type->IsLowHalf()) {
    work_line_->SetRegisterType(this, vregA, *field_type);
  } else {
    work_line_->SetRegisterTypeWide(this, vregA, *field_type, field_type->HighHalf(&reg_types_));
  }
}

void MethodVerifier::VerifyISPut(const Instruction* inst, const RegType& insn_type,
                                 bool is_primitive, bool is_static) {
  uint32_t field_idx = is_static ? inst->VRegB_21c() : inst->VRegC_22c();
  mirror::ArtField* field;
  if (is_static) {
    field = GetStaticField(field_idx);
  } else {
    const RegType& object_type = work_line_->GetRegisterType(this, inst->VRegB_22c());
    field = GetInstanceField(object_type, field_idx);
  }
  const RegType* field_type = nullptr;
  if (field != nullptr) {
    if (field->IsFinal() && field->GetDeclaringClass() != GetDeclaringClass().GetClass()) {
      Fail(VERIFY_ERROR_ACCESS_FIELD) << "cannot modify final field " << PrettyField(field)
                                      << " from other class " << GetDeclaringClass();
      return;
    }
    mirror::Class* field_type_class;
    {
      StackHandleScope<1> hs(self_);
      HandleWrapper<mirror::ArtField> h_field(hs.NewHandleWrapper(&field));
      FieldHelper fh(h_field);
      field_type_class = fh.GetType(can_load_classes_);
    }
    if (field_type_class != nullptr) {
      field_type = &reg_types_.FromClass(field->GetTypeDescriptor(), field_type_class,
                                         field_type_class->CannotBeAssignedFromOtherTypes());
    } else {
      DCHECK(!can_load_classes_ || self_->IsExceptionPending());
      self_->ClearException();
    }
  }
  if (field_type == nullptr) {
    const DexFile::FieldId& field_id = dex_file_->GetFieldId(field_idx);
    const char* descriptor = dex_file_->GetFieldTypeDescriptor(field_id);
    field_type = &reg_types_.FromDescriptor(GetClassLoader(), descriptor, false);
  }
  DCHECK(field_type != nullptr);
  const uint32_t vregA = (is_static) ? inst->VRegA_21c() : inst->VRegA_22c();
  if (is_primitive) {
    VerifyPrimitivePut(*field_type, insn_type, vregA);
  } else {
    if (!insn_type.IsAssignableFrom(*field_type)) {
      Fail(VERIFY_ERROR_BAD_CLASS_SOFT) << "expected field " << PrettyField(field)
                                        << " to be compatible with type '" << insn_type
                                        << "' but found type '" << *field_type
                                        << "' in put-object";
      return;
    }
    work_line_->VerifyRegisterType(this, vregA, *field_type);
  }
}

mirror::ArtField* MethodVerifier::GetQuickFieldAccess(const Instruction* inst,
                                                      RegisterLine* reg_line) {
  DCHECK(inst->Opcode() == Instruction::IGET_QUICK ||
         inst->Opcode() == Instruction::IGET_WIDE_QUICK ||
         inst->Opcode() == Instruction::IGET_OBJECT_QUICK ||
         inst->Opcode() == Instruction::IPUT_QUICK ||
         inst->Opcode() == Instruction::IPUT_WIDE_QUICK ||
         inst->Opcode() == Instruction::IPUT_OBJECT_QUICK);
  const RegType& object_type = reg_line->GetRegisterType(this, inst->VRegB_22c());
  if (!object_type.HasClass()) {
    VLOG(verifier) << "Failed to get mirror::Class* from '" << object_type << "'";
    return nullptr;
  }
  uint32_t field_offset = static_cast<uint32_t>(inst->VRegC_22c());
  mirror::ArtField* f = mirror::ArtField::FindInstanceFieldWithOffset(object_type.GetClass(),
                                                                      field_offset);
  if (f == nullptr) {
    VLOG(verifier) << "Failed to find instance field at offset '" << field_offset
                   << "' from '" << PrettyDescriptor(object_type.GetClass()) << "'";
  }
  return f;
}

void MethodVerifier::VerifyIGetQuick(const Instruction* inst, const RegType& insn_type,
                                     bool is_primitive) {
  DCHECK(Runtime::Current()->IsStarted());
  mirror::ArtField* field = GetQuickFieldAccess(inst, work_line_.get());
  if (field == nullptr) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "Cannot infer field from " << inst->Name();
    return;
  }
  mirror::Class* field_type_class;
  {
    StackHandleScope<1> hs(self_);
    HandleWrapper<mirror::ArtField> h_field(hs.NewHandleWrapper(&field));
    FieldHelper fh(h_field);
    field_type_class = fh.GetType(can_load_classes_);
  }
  const RegType* field_type;
  if (field_type_class != nullptr) {
    field_type = &reg_types_.FromClass(field->GetTypeDescriptor(), field_type_class,
                                       field_type_class->CannotBeAssignedFromOtherTypes());
  } else {
    DCHECK(!can_load_classes_ || self_->IsExceptionPending());
    self_->ClearException();
    field_type = &reg_types_.FromDescriptor(field->GetDeclaringClass()->GetClassLoader(),
                                            field->GetTypeDescriptor(), false);
  }
  DCHECK(field_type != nullptr);
  const uint32_t vregA = inst->VRegA_22c();
  if (is_primitive) {
    if (field_type->Equals(insn_type) ||
        (field_type->IsFloat() && insn_type.IsIntegralTypes()) ||
        (field_type->IsDouble() && insn_type.IsLongTypes())) {
      // expected that read is of the correct primitive type or that int reads are reading
      // floats or long reads are reading doubles
    } else {
      // This is a global failure rather than a class change failure as the instructions and
      // the descriptors for the type should have been consistent within the same file at
      // compile time
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "expected field " << PrettyField(field)
                                        << " to be of type '" << insn_type
                                        << "' but found type '" << *field_type << "' in Get";
      return;
    }
  } else {
    if (!insn_type.IsAssignableFrom(*field_type)) {
      Fail(VERIFY_ERROR_BAD_CLASS_SOFT) << "expected field " << PrettyField(field)
                                        << " to be compatible with type '" << insn_type
                                        << "' but found type '" << *field_type
                                        << "' in get-object";
      work_line_->SetRegisterType(this, vregA, reg_types_.Conflict());
      return;
    }
  }
  if (!field_type->IsLowHalf()) {
    work_line_->SetRegisterType(this, vregA, *field_type);
  } else {
    work_line_->SetRegisterTypeWide(this, vregA, *field_type, field_type->HighHalf(&reg_types_));
  }
}

void MethodVerifier::VerifyIPutQuick(const Instruction* inst, const RegType& insn_type,
                                     bool is_primitive) {
  DCHECK(Runtime::Current()->IsStarted());
  mirror::ArtField* field = GetQuickFieldAccess(inst, work_line_.get());
  if (field == nullptr) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "Cannot infer field from " << inst->Name();
    return;
  }
  const char* descriptor = field->GetTypeDescriptor();
  mirror::ClassLoader* loader = field->GetDeclaringClass()->GetClassLoader();
  const RegType& field_type = reg_types_.FromDescriptor(loader, descriptor, false);
  if (field != nullptr) {
    if (field->IsFinal() && field->GetDeclaringClass() != GetDeclaringClass().GetClass()) {
      Fail(VERIFY_ERROR_ACCESS_FIELD) << "cannot modify final field " << PrettyField(field)
                                      << " from other class " << GetDeclaringClass();
      return;
    }
  }
  const uint32_t vregA = inst->VRegA_22c();
  if (is_primitive) {
    // Primitive field assignability rules are weaker than regular assignability rules
    bool instruction_compatible;
    bool value_compatible;
    const RegType& value_type = work_line_->GetRegisterType(this, vregA);
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
      Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "unexpected value in v" << vregA
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
    work_line_->VerifyRegisterType(this, vregA, field_type);
  }
}

bool MethodVerifier::CheckNotMoveException(const uint16_t* insns, int insn_idx) {
  if ((insns[insn_idx] & 0xff) == Instruction::MOVE_EXCEPTION) {
    Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "invalid use of move-exception";
    return false;
  }
  return true;
}

bool MethodVerifier::UpdateRegisters(uint32_t next_insn, RegisterLine* merge_line,
                                     bool update_merge_line) {
  bool changed = true;
  RegisterLine* target_line = reg_table_.GetLine(next_insn);
  if (!insn_flags_[next_insn].IsVisitedOrChanged()) {
    /*
     * We haven't processed this instruction before, and we haven't touched the registers here, so
     * there's nothing to "merge". Copy the registers over and mark it as changed. (This is the
     * only way a register can transition out of "unknown", so this is not just an optimization.)
     */
    if (!insn_flags_[next_insn].IsReturn()) {
      target_line->CopyFromLine(merge_line);
    } else {
      // Verify that the monitor stack is empty on return.
      if (!merge_line->VerifyMonitorStackEmpty(this)) {
        return false;
      }
      // For returns we only care about the operand to the return, all other registers are dead.
      // Initialize them as conflicts so they don't add to GC and deoptimization information.
      const Instruction* ret_inst = Instruction::At(code_item_->insns_ + next_insn);
      Instruction::Code opcode = ret_inst->Opcode();
      if ((opcode == Instruction::RETURN_VOID) || (opcode == Instruction::RETURN_VOID_BARRIER)) {
        target_line->MarkAllRegistersAsConflicts(this);
      } else {
        target_line->CopyFromLine(merge_line);
        if (opcode == Instruction::RETURN_WIDE) {
          target_line->MarkAllRegistersAsConflictsExceptWide(this, ret_inst->VRegA_11x());
        } else {
          target_line->MarkAllRegistersAsConflictsExcept(this, ret_inst->VRegA_11x());
        }
      }
    }
  } else {
    std::unique_ptr<RegisterLine> copy(gDebugVerify ?
                                 RegisterLine::Create(target_line->NumRegs(), this) :
                                 nullptr);
    if (gDebugVerify) {
      copy->CopyFromLine(target_line);
    }
    changed = target_line->MergeRegisters(this, merge_line);
    if (have_pending_hard_failure_) {
      return false;
    }
    if (gDebugVerify && changed) {
      LogVerifyInfo() << "Merging at [" << reinterpret_cast<void*>(work_insn_idx_) << "]"
                      << " to [" << reinterpret_cast<void*>(next_insn) << "]: " << "\n"
                      << copy->Dump(this) << "  MERGE\n"
                      << merge_line->Dump(this) << "  ==\n"
                      << target_line->Dump(this) << "\n";
    }
    if (update_merge_line && changed) {
      merge_line->CopyFromLine(target_line);
    }
  }
  if (changed) {
    insn_flags_[next_insn].SetChanged();
  }
  return true;
}

InstructionFlags* MethodVerifier::CurrentInsnFlags() {
  return &insn_flags_[work_insn_idx_];
}

const RegType& MethodVerifier::GetMethodReturnType() {
  if (return_type_ == nullptr) {
    if (mirror_method_.Get() != nullptr) {
      StackHandleScope<1> hs(self_);
      mirror::Class* return_type_class =
          MethodHelper(hs.NewHandle(mirror_method_.Get())).GetReturnType(can_load_classes_);
      if (return_type_class != nullptr) {
        return_type_ = &reg_types_.FromClass(mirror_method_->GetReturnTypeDescriptor(),
                                             return_type_class,
                                             return_type_class->CannotBeAssignedFromOtherTypes());
      } else {
        DCHECK(!can_load_classes_ || self_->IsExceptionPending());
        self_->ClearException();
      }
    }
    if (return_type_ == nullptr) {
      const DexFile::MethodId& method_id = dex_file_->GetMethodId(dex_method_idx_);
      const DexFile::ProtoId& proto_id = dex_file_->GetMethodPrototype(method_id);
      uint16_t return_type_idx = proto_id.return_type_idx_;
      const char* descriptor = dex_file_->GetTypeDescriptor(dex_file_->GetTypeId(return_type_idx));
      return_type_ = &reg_types_.FromDescriptor(GetClassLoader(), descriptor, false);
    }
  }
  return *return_type_;
}

const RegType& MethodVerifier::GetDeclaringClass() {
  if (declaring_class_ == nullptr) {
    const DexFile::MethodId& method_id = dex_file_->GetMethodId(dex_method_idx_);
    const char* descriptor
        = dex_file_->GetTypeDescriptor(dex_file_->GetTypeId(method_id.class_idx_));
    if (mirror_method_.Get() != nullptr) {
      mirror::Class* klass = mirror_method_->GetDeclaringClass();
      declaring_class_ = &reg_types_.FromClass(descriptor, klass,
                                               klass->CannotBeAssignedFromOtherTypes());
    } else {
      declaring_class_ = &reg_types_.FromDescriptor(GetClassLoader(), descriptor, false);
    }
  }
  return *declaring_class_;
}

std::vector<int32_t> MethodVerifier::DescribeVRegs(uint32_t dex_pc) {
  RegisterLine* line = reg_table_.GetLine(dex_pc);
  DCHECK(line != nullptr) << "No register line at DEX pc " << StringPrintf("0x%x", dex_pc);
  std::vector<int32_t> result;
  for (size_t i = 0; i < line->NumRegs(); ++i) {
    const RegType& type = line->GetRegisterType(this, i);
    if (type.IsConstant()) {
      result.push_back(type.IsPreciseConstant() ? kConstant : kImpreciseConstant);
      const ConstantType* const_val = down_cast<const ConstantType*>(&type);
      result.push_back(const_val->ConstantValue());
    } else if (type.IsConstantLo()) {
      result.push_back(type.IsPreciseConstantLo() ? kConstant : kImpreciseConstant);
      const ConstantType* const_val = down_cast<const ConstantType*>(&type);
      result.push_back(const_val->ConstantValueLo());
    } else if (type.IsConstantHi()) {
      result.push_back(type.IsPreciseConstantHi() ? kConstant : kImpreciseConstant);
      const ConstantType* const_val = down_cast<const ConstantType*>(&type);
      result.push_back(const_val->ConstantValueHi());
    } else if (type.IsIntegralTypes()) {
      result.push_back(kIntVReg);
      result.push_back(0);
    } else if (type.IsFloat()) {
      result.push_back(kFloatVReg);
      result.push_back(0);
    } else if (type.IsLong()) {
      result.push_back(kLongLoVReg);
      result.push_back(0);
      result.push_back(kLongHiVReg);
      result.push_back(0);
      ++i;
    } else if (type.IsDouble()) {
      result.push_back(kDoubleLoVReg);
      result.push_back(0);
      result.push_back(kDoubleHiVReg);
      result.push_back(0);
      ++i;
    } else if (type.IsUndefined() || type.IsConflict() || type.IsHighHalf()) {
      result.push_back(kUndefined);
      result.push_back(0);
    } else {
      CHECK(type.IsNonZeroReferenceTypes());
      result.push_back(kReferenceVReg);
      result.push_back(0);
    }
  }
  return result;
}

const RegType& MethodVerifier::DetermineCat1Constant(int32_t value, bool precise) {
  if (precise) {
    // Precise constant type.
    return reg_types_.FromCat1Const(value, true);
  } else {
    // Imprecise constant type.
    if (value < -32768) {
      return reg_types_.IntConstant();
    } else if (value < -128) {
      return reg_types_.ShortConstant();
    } else if (value < 0) {
      return reg_types_.ByteConstant();
    } else if (value == 0) {
      return reg_types_.Zero();
    } else if (value == 1) {
      return reg_types_.One();
    } else if (value < 128) {
      return reg_types_.PosByteConstant();
    } else if (value < 32768) {
      return reg_types_.PosShortConstant();
    } else if (value < 65536) {
      return reg_types_.CharConstant();
    } else {
      return reg_types_.IntConstant();
    }
  }
}

void MethodVerifier::Init() {
  art::verifier::RegTypeCache::Init();
}

void MethodVerifier::Shutdown() {
  verifier::RegTypeCache::ShutDown();
}

void MethodVerifier::VisitStaticRoots(RootCallback* callback, void* arg) {
  RegTypeCache::VisitStaticRoots(callback, arg);
}

void MethodVerifier::VisitRoots(RootCallback* callback, void* arg) {
  reg_types_.VisitRoots(callback, arg);
}

}  // namespace verifier
}  // namespace art
