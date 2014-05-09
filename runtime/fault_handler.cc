/*
 * Copyright (C) 2008 The Android Open Source Project
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

#include "fault_handler.h"
#include <sys/mman.h>
#include <sys/ucontext.h>
#include "base/macros.h"
#include "globals.h"
#include "base/logging.h"
#include "base/hex_dump.h"
#include "thread.h"
#include "mirror/art_method-inl.h"
#include "mirror/class-inl.h"
#include "mirror/dex_cache.h"
#include "mirror/object_array-inl.h"
#include "mirror/object-inl.h"
#include "object_utils.h"
#include "scoped_thread_state_change.h"
#include "verify_object-inl.h"

namespace art {
// Static fault manger object accessed by signal handler.
FaultManager fault_manager;

// Signal handler called on SIGSEGV.
static void art_fault_handler(int sig, siginfo_t* info, void* context) {
  fault_manager.HandleFault(sig, info, context);
}

FaultManager::FaultManager() {
  sigaction(SIGSEGV, nullptr, &oldaction_);
}

FaultManager::~FaultManager() {
  sigaction(SIGSEGV, &oldaction_, nullptr);   // Restore old handler.
}

void FaultManager::Init() {
  struct sigaction action;
  action.sa_sigaction = art_fault_handler;
  sigemptyset(&action.sa_mask);
  action.sa_flags = SA_SIGINFO | SA_ONSTACK;
#if !defined(__mips__)
  action.sa_restorer = nullptr;
#endif
  sigaction(SIGSEGV, &action, &oldaction_);
}

void FaultManager::HandleFault(int sig, siginfo_t* info, void* context) {
  // BE CAREFUL ALLOCATING HERE INCLUDING USING LOG(...)
  //
  // If malloc calls abort, it will be holding its lock.
  // If the handler tries to call malloc, it will deadlock.
  VLOG(signals) << "Handling fault";
  if (IsInGeneratedCode(context, true)) {
    VLOG(signals) << "in generated code, looking for handler";
    for (const auto& handler : generated_code_handlers_) {
      VLOG(signals) << "invoking Action on handler " << handler;
      if (handler->Action(sig, info, context)) {
        return;
      }
    }
  }
  for (const auto& handler : other_handlers_) {
    if (handler->Action(sig, info, context)) {
      return;
    }
  }
  LOG(ERROR)<< "Caught unknown SIGSEGV in ART fault handler";
  oldaction_.sa_sigaction(sig, info, context);
}

void FaultManager::AddHandler(FaultHandler* handler, bool generated_code) {
  if (generated_code) {
    generated_code_handlers_.push_back(handler);
  } else {
    other_handlers_.push_back(handler);
  }
}

void FaultManager::RemoveHandler(FaultHandler* handler) {
  auto it = std::find(generated_code_handlers_.begin(), generated_code_handlers_.end(), handler);
  if (it != generated_code_handlers_.end()) {
    generated_code_handlers_.erase(it);
    return;
  }
  auto it2 = std::find(other_handlers_.begin(), other_handlers_.end(), handler);
  if (it2 != other_handlers_.end()) {
    other_handlers_.erase(it);
    return;
  }
  LOG(FATAL) << "Attempted to remove non existent handler " << handler;
}

// This function is called within the signal handler.  It checks that
// the mutator_lock is held (shared).  No annotalysis is done.
bool FaultManager::IsInGeneratedCode(void* context, bool check_dex_pc) {
  // We can only be running Java code in the current thread if it
  // is in Runnable state.
  VLOG(signals) << "Checking for generated code";
  Thread* thread = Thread::Current();
  if (thread == nullptr) {
    VLOG(signals) << "no current thread";
    return false;
  }

  ThreadState state = thread->GetState();
  if (state != kRunnable) {
    VLOG(signals) << "not runnable";
    return false;
  }

  // Current thread is runnable.
  // Make sure it has the mutator lock.
  if (!Locks::mutator_lock_->IsSharedHeld(thread)) {
    VLOG(signals) << "no lock";
    return false;
  }

  mirror::ArtMethod* method_obj = 0;
  uintptr_t return_pc = 0;
  uintptr_t sp = 0;

  // Get the architecture specific method address and return address.  These
  // are in architecture specific files in arch/<arch>/fault_handler_<arch>.
  GetMethodAndReturnPCAndSP(context, &method_obj, &return_pc, &sp);

  // If we don't have a potential method, we're outta here.
  VLOG(signals) << "potential method: " << method_obj;
  if (method_obj == 0 || !IsAligned<kObjectAlignment>(method_obj)) {
    VLOG(signals) << "no method";
    return false;
  }

  // Verify that the potential method is indeed a method.
  // TODO: check the GC maps to make sure it's an object.
  // Check that the class pointer inside the object is not null and is aligned.
  // TODO: Method might be not a heap address, and GetClass could fault.
  mirror::Class* cls = method_obj->GetClass<kVerifyNone>();
  if (cls == nullptr) {
    VLOG(signals) << "not a class";
    return false;
  }
  if (!IsAligned<kObjectAlignment>(cls)) {
    VLOG(signals) << "not aligned";
    return false;
  }


  if (!VerifyClassClass(cls)) {
    VLOG(signals) << "not a class class";
    return false;
  }

  // Now make sure the class is a mirror::ArtMethod.
  if (!cls->IsArtMethodClass()) {
    VLOG(signals) << "not a method";
    return false;
  }

  // We can be certain that this is a method now.  Check if we have a GC map
  // at the return PC address.
  if (true || kIsDebugBuild) {
    VLOG(signals) << "looking for dex pc for return pc " << std::hex << return_pc;
    const void* code = Runtime::Current()->GetInstrumentation()->GetQuickCodeFor(method_obj);
    uint32_t sought_offset = return_pc - reinterpret_cast<uintptr_t>(code);
    VLOG(signals) << "pc offset: " << std::hex << sought_offset;
  }
  uint32_t dexpc = method_obj->ToDexPc(return_pc, false);
  VLOG(signals) << "dexpc: " << dexpc;
  return !check_dex_pc || dexpc != DexFile::kDexNoIndex;
}

FaultHandler::FaultHandler(FaultManager* manager) : manager_(manager) {
}

//
// Null pointer fault handler
//
NullPointerHandler::NullPointerHandler(FaultManager* manager) : FaultHandler(manager) {
  manager_->AddHandler(this, true);
}

//
// Suspension fault handler
//
SuspensionHandler::SuspensionHandler(FaultManager* manager) : FaultHandler(manager) {
  manager_->AddHandler(this, true);
}

//
// Stack overflow fault handler
//
StackOverflowHandler::StackOverflowHandler(FaultManager* manager) : FaultHandler(manager) {
  manager_->AddHandler(this, true);
}

//
// Stack trace handler, used to help get a stack trace from SIGSEGV inside of compiled code.
//
JavaStackTraceHandler::JavaStackTraceHandler(FaultManager* manager) : FaultHandler(manager) {
  manager_->AddHandler(this, false);
}

bool JavaStackTraceHandler::Action(int sig, siginfo_t* siginfo, void* context) {
  // Make sure that we are in the generated code, but we may not have a dex pc.
  if (manager_->IsInGeneratedCode(context, false)) {
    LOG(ERROR) << "Dumping java stack trace for crash in generated code";
    mirror::ArtMethod* method = nullptr;
    uintptr_t return_pc = 0;
    uintptr_t sp = 0;
    manager_->GetMethodAndReturnPCAndSP(context, &method, &return_pc, &sp);
    Thread* self = Thread::Current();
    // Inside of generated code, sp[0] is the method, so sp is the frame.
    mirror::ArtMethod** frame = reinterpret_cast<mirror::ArtMethod**>(sp);
    self->SetTopOfStack(frame, 0);  // Since we don't necessarily have a dex pc, pass in 0.
    self->DumpJavaStack(LOG(ERROR));
  }
  return false;  // Return false since we want to propagate the fault to the main signal handler.
}

}   // namespace art

