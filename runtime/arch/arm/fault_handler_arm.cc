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
#include "base/hex_dump.h"
#include "globals.h"
#include "base/logging.h"
#include "base/hex_dump.h"
#include "mirror/art_method.h"
#include "mirror/art_method-inl.h"
#include "thread.h"
#include "thread-inl.h"

//
// ARM specific fault handler functions.
//

namespace art {

extern "C" void art_quick_throw_null_pointer_exception();
extern "C" void art_quick_throw_stack_overflow(void*);
extern "C" void art_quick_test_suspend();

// Get the size of a thumb2 instruction in bytes.
static uint32_t GetInstructionSize(uint8_t* pc) {
  uint16_t instr = pc[0] | pc[1] << 8;
  bool is_32bit = ((instr & 0xF000) == 0xF000) || ((instr & 0xF800) == 0xE800);
  uint32_t instr_size = is_32bit ? 4 : 2;
  return instr_size;
}

void FaultManager::GetMethodAndReturnPC(void* context, uintptr_t& method, uintptr_t& return_pc) {
  struct ucontext *uc = (struct ucontext *)context;
  struct sigcontext *sc = reinterpret_cast<struct sigcontext*>(&uc->uc_mcontext);
  uintptr_t* sp = reinterpret_cast<uint32_t*>(sc->arm_sp);
  LOG(DEBUG) << "sp: " << sp;
  if (sp == nullptr) {
    return;
  }

  // In the case of a stack overflow, the stack is not valid and we can't
  // get the method from the top of the stack.  However it's in r0.
  uintptr_t* fault_addr = reinterpret_cast<uintptr_t*>(sc->fault_address);
  uintptr_t* overflow_addr = reinterpret_cast<uintptr_t*>(
      reinterpret_cast<uint8_t*>(sp) - Thread::kStackOverflowReservedBytes);
  if (overflow_addr == fault_addr) {
    method = sc->arm_r0;
  } else {
    // The method is at the top of the stack.
    method = sp[0];
  }

  // Work out the return PC.  This will be the address of the instruction
  // following the faulting ldr/str instruction.  This is in thumb mode so
  // the instruction might be a 16 or 32 bit one.  Also, the GC map always
  // has the bottom bit of the PC set so we also need to set that.

  // Need to work out the size of the instruction that caused the exception.
  uint8_t* ptr = reinterpret_cast<uint8_t*>(sc->arm_pc);
  LOG(DEBUG) << "pc: " << std::hex << static_cast<void*>(ptr);
  uint32_t instr_size = GetInstructionSize(ptr);

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

  uint32_t instr_size = GetInstructionSize(ptr);
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

// Stack overflow fault handler.
//
// This checks that the fault address is equal to the current stack pointer
// minus the overflow region size (16K typically).  The instruction sequence
// that generates this signal is:
//
// sub r12,sp,#16384
// ldr.w r12,[r12,#0]
//
// The second instruction will fault if r12 is inside the protected region
// on the stack.
//
// If we determine this is a stack overflow we need to move the stack pointer
// to the overflow region below the protected region.  Because we now have
// a gap in the stack (skips over protected region), we need to arrange
// for the rest of the system to be unaware of the new stack arrangement
// and behave as if there is a fully valid stack.  We do this by placing
// a unique address onto the stack followed by
// the size of the gap.  The stack walker will detect this and skip over the
// gap.

// NB. We also need to be careful of stack alignment as the ARM EABI specifies that
// stack must be 8 byte aligned when making any calls.

// NB. The size of the gap is the difference between the previous frame's SP and
// the SP at which the size word is pushed.

bool StackOverflowHandler::Action(int sig, siginfo_t* info, void* context) {
  struct ucontext *uc = (struct ucontext *)context;
  struct sigcontext *sc = reinterpret_cast<struct sigcontext*>(&uc->uc_mcontext);
  LOG(DEBUG) << "stack overflow handler with sp at " << std::hex << &uc;
  LOG(DEBUG) << "sigcontext: " << std::hex << sc;

  uint8_t* sp = reinterpret_cast<uint8_t*>(sc->arm_sp);
  LOG(DEBUG) << "sp: " << static_cast<void*>(sp);

  uintptr_t* fault_addr = reinterpret_cast<uintptr_t*>(sc->fault_address);
  LOG(DEBUG) << "fault_addr: " << std::hex << fault_addr;
  LOG(DEBUG) << "checking for stack overflow, sp: " << std::hex << static_cast<void*>(sp) <<
    ", fault_addr: " << fault_addr;
  uintptr_t* overflow_addr = reinterpret_cast<uintptr_t*>(sp - Thread::kStackOverflowReservedBytes);

  // Check that the fault address is the value expected for a stack overflow.
  if (fault_addr != overflow_addr) {
    LOG(DEBUG) << "Not a stack overflow";
    return false;
  }

  // We know this is a stack overflow.  We need to move the sp to the overflow region
  // the exists below the protected region.  R9 contains the current Thread* so
  // we can read the stack_end from that and subtract the size of the
  // protected region.  This creates a gap in the stack that needs to be marked.
  Thread* self = reinterpret_cast<Thread*>(sc->arm_r9);

  uint8_t* prevsp = sp;
  sp = self->GetStackEnd() - Thread::kStackOverflowProtectedSize;
  LOG(DEBUG) << "setting sp to overflow region at " << std::hex << static_cast<void*>(sp);

  // We need to find the previous frame.  Remember that
  // this has not yet been fully constructed because the SP has not been
  // decremented.  So we need to work out the size of the spill portion of the
  // frame.  This consists of something like:
  //
  // 0xb6a1d49c: e92d40e0  push    {r5, r6, r7, lr}
  // 0xb6a1d4a0: ed2d8a06  vpush.f32 {s16-s21}
  //
  // The first is encoded in the ArtMethod as the spill_mask, the second as the
  // fp_spill_mask.  A population count on each will give the number of registers
  // in each mask.  Each register is 4 bytes on ARM32.

  mirror::ArtMethod* method = reinterpret_cast<mirror::ArtMethod*>(sc->arm_r0);
  uint32_t spill_mask = method->GetCoreSpillMask();
  uint32_t numcores = __builtin_popcount(spill_mask);
  uint32_t fp_spill_mask = method->GetFpSpillMask();
  uint32_t numfps = __builtin_popcount(fp_spill_mask);
  uint32_t spill_size = (numcores + numfps) * 4;
  LOG(DEBUG) << "spill size: " << spill_size;
  uint8_t* prevframe = prevsp + spill_size;
  LOG(DEBUG) << "previous frame: " << static_cast<void*>(prevframe);

  // NOTE: the ARM EABI needs an 8 byte alignment.  In the case of ARM32 a pointer
  // is 4 bytes so that, together with the offset to the previous frame is 8
  // bytes.  On other architectures we will need to align the stack.

  // Push a marker onto the stack to tell the stack walker that there is a stack
  // overflow and the stack is not contiguous.

  // First the offset from SP to the previous frame.
  sp -= sizeof(uint32_t);
  LOG(DEBUG) << "push gap of " << static_cast<uint32_t>(prevframe - sp);
  *reinterpret_cast<uint32_t*>(sp) = static_cast<uint32_t>(prevframe - sp);

  // Now the gap marker (pointer sized).
  sp -= sizeof(mirror::ArtMethod*);
  *reinterpret_cast<void**>(sp) = stack_overflow_gap_marker;

  // Now establish the stack pointer for the signal return.
  sc->arm_sp = reinterpret_cast<uintptr_t>(sp);

  // Now arrange for the signal handler to return to art_quick_throw_stack_overflow.
  // We need the LR to point to the GC map just after the fault instruction.
  uint8_t* ptr = reinterpret_cast<uint8_t*>(sc->arm_pc);
  uint32_t instr_size = GetInstructionSize(ptr);
  sc->arm_lr = (sc->arm_pc + instr_size) | 1;      // LR needs to point to gc map location
  sc->arm_pc = reinterpret_cast<uintptr_t>(art_quick_throw_stack_overflow);

  // The kernel will now return to the address in sc->arm_pc.  We have arranged the
  // stack pointer to be in the overflow region.  Throwing the exception will perform
  // a longjmp which will restore the stack pointer to the correct location for the
  // exception catch.
  return true;
}
}       // namespace art
