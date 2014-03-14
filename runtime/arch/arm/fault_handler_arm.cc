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
#include <sys/ucontext.h>
#include "base/macros.h"
#include "globals.h"
#include "base/logging.h"
#include "base/hex_dump.h"
#include "thread.h"
#include "thread-inl.h"

//
// ARM specific fault handler functions.
//

namespace art {

extern "C" void art_quick_throw_null_pointer_exception();
extern "C" void art_quick_test_suspend();

void FaultManager::GetMethodAndReturnPC(void* context, uintptr_t& method, uintptr_t& return_pc) {
  struct ucontext *uc = (struct ucontext *)context;
  struct sigcontext *sc = reinterpret_cast<struct sigcontext*>(&uc->uc_mcontext);
  uintptr_t* sp = reinterpret_cast<uint32_t*>(sc->arm_sp);
  if (sp == nullptr) {
    return;
  }

  // Work out the return PC.  This will be the address of the instruction
  // following the faulting ldr/str instruction.  This is in thumb mode so
  // the instruction might be a 16 or 32 bit one.  Also, the GC map always
  // has the bottom bit of the PC set so we also need to set that.

  // Need to work out the size of the instruction that caused the exception.
  uint8_t* ptr = reinterpret_cast<uint8_t*>(sc->arm_pc);

  uint16_t instr = ptr[0] | ptr[1] << 8;
  bool is_32bit = ((instr & 0xF000) == 0xF000) || ((instr & 0xF800) == 0xE800);
  uint32_t instr_size = is_32bit ? 4 : 2;

  // The method is at the top of the stack.
  method = sp[0];

  return_pc = (sc->arm_pc + instr_size) | 1;
}

bool NullPointerHandler::Action(int sig, siginfo_t* info, void* context) {
  // The code that looks for the catch location needs to know the value of the
  // ARM PC at the point of call.  For Null checks we insert a GC map that is immediately after
  // the load/store instruction that might cause the fault.  However the mapping table has
  // the low bits set for thumb mode so we need to set the bottom bit for the LR
  // register in order to find the mapping.

  // Need to work out the size of the instruction that caused the exception.
  struct ucontext *uc = (struct ucontext *)context;
  struct sigcontext *sc = reinterpret_cast<struct sigcontext*>(&uc->uc_mcontext);
  uint8_t* ptr = reinterpret_cast<uint8_t*>(sc->arm_pc);

  uint16_t instr = ptr[0] | ptr[1] << 8;
  bool is_32bit = ((instr & 0xF000) == 0xF000) || ((instr & 0xF800) == 0xE800);
  uint32_t instr_size = is_32bit ? 4 : 2;
  sc->arm_lr = (sc->arm_pc + instr_size) | 1;      // LR needs to point to gc map location
  sc->arm_pc = reinterpret_cast<uintptr_t>(art_quick_throw_null_pointer_exception);
  LOG(DEBUG) << "Generating null pointer exception";
  return true;
}

// A suspend check is done using the following instruction sequence:
// 0xf723c0b2: f8d902c0  ldr.w   r0, [r9, #704]  ; suspend_trigger_
// .. some intervening instruction
// 0xf723c0b6: 6800      ldr     r0, [r0, #0]

// The offset from r9 is Thread::ThreadSuspendTriggerOffset().
// To check for a suspend check, we examine the instructions that caused
// the fault (at PC-4 and PC).
bool SuspensionHandler::Action(int sig, siginfo_t* info, void* context) {
  // These are the instructions to check for.  The first one is the ldr r0,[r9,#xxx]
  // where xxx is the offset of the suspend trigger.
  uint32_t checkinst1 = 0xf8d90000 + Thread::ThreadSuspendTriggerOffset().Int32Value();
  uint16_t checkinst2 = 0x6800;

  struct ucontext *uc = (struct ucontext *)context;
  struct sigcontext *sc = reinterpret_cast<struct sigcontext*>(&uc->uc_mcontext);
  uint8_t* ptr2 = reinterpret_cast<uint8_t*>(sc->arm_pc);
  uint8_t* ptr1 = ptr2 - 4;
  LOG(DEBUG) << "checking suspend";

  uint16_t inst2 = ptr2[0] | ptr2[1] << 8;
  LOG(DEBUG) << "inst2: " << std::hex << inst2 << " checkinst2: " << checkinst2;
  if (inst2 != checkinst2) {
    // Second instruction is not good, not ours.
    return false;
  }

  // The first instruction can a little bit up the stream due to load hoisting
  // in the compiler.
  uint8_t* limit = ptr1 - 40;   // Compiler will hoist to a max of 20 instructions.
  bool found = false;
  while (ptr1 > limit) {
    uint32_t inst1 = ((ptr1[0] | ptr1[1] << 8) << 16) | (ptr1[2] | ptr1[3] << 8);
    LOG(DEBUG) << "inst1: " << std::hex << inst1 << " checkinst1: " << checkinst1;
    if (inst1 == checkinst1) {
      found = true;
      break;
    }
    ptr1 -= 2;      // Min instruction size is 2 bytes.
  }
  if (found) {
    LOG(DEBUG) << "suspend check match";
    // This is a suspend check.  Arrange for the signal handler to return to
    // art_quick_test_suspend.  Also set LR so that after the suspend check it
    // will resume the instruction (current PC + 2).  PC points to the
    // ldr r0,[r0,#0] instruction (r0 will be 0, set by the trigger).

    // NB: remember that we need to set the bottom bit of the LR register
    // to switch to thumb mode.
    LOG(DEBUG) << "arm lr: " << std::hex << sc->arm_lr;
    LOG(DEBUG) << "arm pc: " << std::hex << sc->arm_pc;
    sc->arm_lr = sc->arm_pc + 3;      // +2 + 1 (for thumb)
    sc->arm_pc = reinterpret_cast<uintptr_t>(art_quick_test_suspend);

    // Now remove the suspend trigger that caused this fault.
    Thread::Current()->RemoveSuspendTrigger();
    LOG(DEBUG) << "removed suspend trigger invoking test suspend";
    return true;
  }
  return false;
}

bool StackOverflowHandler::Action(int sig, siginfo_t* info, void* context) {
  return false;
}
}       // namespace art
