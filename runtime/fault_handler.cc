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
  bool handled = false;
  LOG(DEBUG) << "Handling fault";
  if (IsInGeneratedCode(context)) {
    LOG(DEBUG) << "in generated code, looking for handler";
    for (auto& handler : handlers_) {
      LOG(DEBUG) << "invoking Action on handler " << handler;
      handled = handler->Action(sig, info, context);
      if (handled) {
        return;
      }
    }
  }

  if (!handled) {
    LOG(ERROR)<< "Caught unknown SIGSEGV in ART fault handler";
    oldaction_.sa_sigaction(sig, info, context);
  }
}

void FaultManager::AddHandler(FaultHandler* handler) {
  handlers_.push_back(handler);
}

void FaultManager::RemoveHandler(FaultHandler* handler) {
  for (Handlers::iterator i = handlers_.begin(); i != handlers_.end(); ++i) {
    FaultHandler* h = *i;
    if (h == handler) {
      handlers_.erase(i);
      return;
    }
  }
}


// This function is called within the signal handler.  It checks that
// the mutator_lock is held (shared).  No annotalysis is done.
bool FaultManager::IsInGeneratedCode(void *context) {
  // We can only be running Java code in the current thread if it
  // is in Runnable state.
  LOG(DEBUG) << "Checking for generated code";
  Thread* thread = Thread::Current();
  if (thread == nullptr) {
    LOG(DEBUG) << "no current thread";
    return false;
  }

  ThreadState state = thread->GetState();
  if (state != kRunnable) {
    LOG(DEBUG) << "not runnable";
    return false;
  }

  // Current thread is runnable.
  // Make sure it has the mutator lock.
  if (!Locks::mutator_lock_->IsSharedHeld(thread)) {
    LOG(DEBUG) << "no lock";
    return false;
  }

  uintptr_t potential_method = 0;
  uintptr_t return_pc = 0;

  // Get the architecture specific method address and return address.  These
  // are in architecture specific files in arch/<arch>/fault_handler_<arch>.cc
  GetMethodAndReturnPC(context, /*out*/potential_method, /*out*/return_pc);

  // If we don't have a potential method, we're outta here.
  LOG(DEBUG) << "potential method: " << potential_method;
  if (potential_method == 0) {
    LOG(DEBUG) << "no method";
    return false;
  }

  // Verify that the potential method is indeed a method.
  // TODO: check the GC maps to make sure it's an object.

  mirror::Object* method_obj =
      reinterpret_cast<mirror::Object*>(potential_method);

  // Check that the class pointer inside the object is not null and is aligned.
  mirror::Class* cls = method_obj->GetClass<kVerifyNone>();
  if (cls == nullptr) {
    LOG(DEBUG) << "not a class";
    return false;
  }
  if (!IsAligned<kObjectAlignment>(cls)) {
    LOG(DEBUG) << "not aligned";
    return false;
  }


  if (!VerifyClassClass(cls)) {
    LOG(DEBUG) << "not a class class";
    return false;
  }

  // Now make sure the class is a mirror::ArtMethod.
  if (!cls->IsArtMethodClass()) {
    LOG(DEBUG) << "not a method";
    return false;
  }

  // We can be certain that this is a method now.  Check if we have a GC map
  // at the return PC address.
  mirror::ArtMethod* method =
      reinterpret_cast<mirror::ArtMethod*>(potential_method);
  if (true || kIsDebugBuild) {
    LOG(DEBUG) << "looking for dex pc for return pc " << std::hex << return_pc;
    const void* code = Runtime::Current()->GetInstrumentation()->GetQuickCodeFor(method);
    uint32_t sought_offset = return_pc - reinterpret_cast<uintptr_t>(code);
    LOG(DEBUG) << "pc offset: " << std::hex << sought_offset;
  }
  uint32_t dexpc = method->ToDexPc(return_pc, false);
  LOG(DEBUG) << "dexpc: " << dexpc;
  return dexpc != DexFile::kDexNoIndex;
}

//
// Null pointer fault handler
//

NullPointerHandler::NullPointerHandler(FaultManager* manager) {
  manager->AddHandler(this);
}

//
// Suspension fault handler
//

SuspensionHandler::SuspensionHandler(FaultManager* manager) {
  manager->AddHandler(this);
}

//
// Stack overflow fault handler
//

StackOverflowHandler::StackOverflowHandler(FaultManager* manager) {
  manager->AddHandler(this);
}
}   // namespace art

