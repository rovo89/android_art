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
#include "mirror/art_method.h"
#include "mirror/art_method-inl.h"
#include "thread.h"
#include "thread-inl.h"


//
// X86 specific fault handler functions.
//

namespace art {

extern "C" void art_quick_throw_null_pointer_exception();
extern "C" void art_quick_throw_stack_overflow_from_signal();
extern "C" void art_quick_test_suspend();

// From the x86 disassembler...
enum SegmentPrefix {
  kCs = 0x2e,
  kSs = 0x36,
  kDs = 0x3e,
  kEs = 0x26,
  kFs = 0x64,
  kGs = 0x65,
};

// Get the size of an instruction in bytes.
static uint32_t GetInstructionSize(uint8_t* pc) {
  uint8_t* instruction_start = pc;
  bool have_prefixes = true;
  bool two_byte = false;

  // Skip all the prefixes.
  do {
    switch (*pc) {
        // Group 1 - lock and repeat prefixes:
      case 0xF0:
      case 0xF2:
      case 0xF3:
        // Group 2 - segment override prefixes:
      case kCs:
      case kSs:
      case kDs:
      case kEs:
      case kFs:
      case kGs:
        // Group 3 - operand size override:
      case 0x66:
        // Group 4 - address size override:
      case 0x67:
        break;
      default:
        have_prefixes = false;
        break;
    }
    if (have_prefixes) {
      pc++;
    }
  } while (have_prefixes);

#if defined(__x86_64__)
  // Skip REX is present.
  if (*pc >= 0x40 && *pc <= 0x4F) {
    ++pc;
  }
#endif

  // Check for known instructions.
  uint32_t known_length = 0;
  switch (*pc) {
  case 0x83:                // cmp [r + v], b: 4 byte instruction
    known_length = 4;
    break;
  }

  if (known_length > 0) {
    VLOG(signals) << "known instruction with length " << known_length;
    return known_length;
  }

  // Unknown instruction, work out length.

  // Work out if we have a ModR/M byte.
  uint8_t opcode = *pc++;
  if (opcode == 0xf) {
    two_byte = true;
    opcode = *pc++;
  }

  bool has_modrm = false;         // Is ModR/M byte present?
  uint8_t hi = opcode >> 4;       // Opcode high nybble.
  uint8_t lo = opcode & 0b1111;   // Opcode low nybble.

  // From the Intel opcode tables.
  if (two_byte) {
    has_modrm = true;   // TODO: all of these?
  } else if (hi < 4) {
    has_modrm = lo < 4 || (lo >= 8 && lo <= 0xb);
  } else if (hi == 6) {
    has_modrm = lo == 3 || lo == 9 || lo == 0xb;
  } else if (hi == 8) {
    has_modrm = lo != 0xd;
  } else if (hi == 0xc) {
    has_modrm = lo == 1 || lo == 2 || lo == 6 || lo == 7;
  } else if (hi == 0xd) {
    has_modrm = lo < 4;
  } else if (hi == 0xf) {
    has_modrm = lo == 6 || lo == 7;
  }

  if (has_modrm) {
    uint8_t modrm = *pc++;
    uint8_t mod = (modrm >> 6) & 0b11;
    uint8_t reg = (modrm >> 3) & 0b111;
    switch (mod) {
      case 0:
        break;
      case 1:
        if (reg == 4) {
          // SIB + 1 byte displacement.
          pc += 2;
        } else {
          pc += 1;
        }
        break;
      case 2:
        // SIB + 4 byte displacement.
        pc += 5;
        break;
      case 3:
        break;
    }
  }

  VLOG(signals) << "calculated X86 instruction size is " << (pc - instruction_start);
  return pc - instruction_start;
}

void FaultManager::GetMethodAndReturnPCAndSP(siginfo_t* siginfo, void* context,
                                             mirror::ArtMethod** out_method,
                                             uintptr_t* out_return_pc, uintptr_t* out_sp) {
  struct ucontext* uc = reinterpret_cast<struct ucontext*>(context);
  *out_sp = static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_ESP]);
  VLOG(signals) << "sp: " << std::hex << *out_sp;
  if (*out_sp == 0) {
    return;
  }

  // In the case of a stack overflow, the stack is not valid and we can't
  // get the method from the top of the stack.  However it's in EAX.
  uintptr_t* fault_addr = reinterpret_cast<uintptr_t*>(siginfo->si_addr);
  uintptr_t* overflow_addr = reinterpret_cast<uintptr_t*>(
      reinterpret_cast<uint8_t*>(*out_sp) - GetStackOverflowReservedBytes(kX86));
  if (overflow_addr == fault_addr) {
    *out_method = reinterpret_cast<mirror::ArtMethod*>(uc->uc_mcontext.gregs[REG_EAX]);
  } else {
    // The method is at the top of the stack.
    *out_method = reinterpret_cast<mirror::ArtMethod*>(reinterpret_cast<uintptr_t*>(*out_sp)[0]);
  }

  uint8_t* pc = reinterpret_cast<uint8_t*>(uc->uc_mcontext.gregs[REG_EIP]);
  VLOG(signals) << HexDump(pc, 32, true, "PC ");

  uint32_t instr_size = GetInstructionSize(pc);
  *out_return_pc = reinterpret_cast<uintptr_t>(pc + instr_size);
}

bool NullPointerHandler::Action(int sig, siginfo_t* info, void* context) {
  struct ucontext *uc = reinterpret_cast<struct ucontext*>(context);
  uint8_t* pc = reinterpret_cast<uint8_t*>(uc->uc_mcontext.gregs[REG_EIP]);
  uint8_t* sp = reinterpret_cast<uint8_t*>(uc->uc_mcontext.gregs[REG_ESP]);

  uint32_t instr_size = GetInstructionSize(pc);
  // We need to arrange for the signal handler to return to the null pointer
  // exception generator.  The return address must be the address of the
  // next instruction (this instruction + instruction size).  The return address
  // is on the stack at the top address of the current frame.

  // Push the return address onto the stack.
  uint32_t retaddr = reinterpret_cast<uint32_t>(pc + instr_size);
  uint32_t* next_sp = reinterpret_cast<uint32_t*>(sp - 4);
  *next_sp = retaddr;
  uc->uc_mcontext.gregs[REG_ESP] = reinterpret_cast<uint32_t>(next_sp);

  uc->uc_mcontext.gregs[REG_EIP] =
        reinterpret_cast<uintptr_t>(art_quick_throw_null_pointer_exception);
  VLOG(signals) << "Generating null pointer exception";
  return true;
}

// A suspend check is done using the following instruction sequence:
// 0xf720f1df:         648B058C000000      mov     eax, fs:[0x8c]  ; suspend_trigger
// .. some intervening instructions.
// 0xf720f1e6:                   8500      test    eax, [eax]

// The offset from fs is Thread::ThreadSuspendTriggerOffset().
// To check for a suspend check, we examine the instructions that caused
// the fault.
bool SuspensionHandler::Action(int sig, siginfo_t* info, void* context) {
  // These are the instructions to check for.  The first one is the mov eax, fs:[xxx]
  // where xxx is the offset of the suspend trigger.
  uint32_t trigger = Thread::ThreadSuspendTriggerOffset<4>().Int32Value();

  VLOG(signals) << "Checking for suspension point";
  uint8_t checkinst1[] = {0x64, 0x8b, 0x05, static_cast<uint8_t>(trigger & 0xff),
      static_cast<uint8_t>((trigger >> 8) & 0xff), 0, 0};
  uint8_t checkinst2[] = {0x85, 0x00};

  struct ucontext *uc = reinterpret_cast<struct ucontext*>(context);
  uint8_t* pc = reinterpret_cast<uint8_t*>(uc->uc_mcontext.gregs[REG_EIP]);
  uint8_t* sp = reinterpret_cast<uint8_t*>(uc->uc_mcontext.gregs[REG_ESP]);

  if (pc[0] != checkinst2[0] || pc[1] != checkinst2[1]) {
    // Second instruction is not correct (test eax,[eax]).
    VLOG(signals) << "Not a suspension point";
    return false;
  }

  // The first instruction can a little bit up the stream due to load hoisting
  // in the compiler.
  uint8_t* limit = pc - 100;   // Compiler will hoist to a max of 20 instructions.
  uint8_t* ptr = pc - sizeof(checkinst1);
  bool found = false;
  while (ptr > limit) {
    if (memcmp(ptr, checkinst1, sizeof(checkinst1)) == 0) {
      found = true;
      break;
    }
    ptr -= 1;
  }

  if (found) {
    VLOG(signals) << "suspend check match";

    // We need to arrange for the signal handler to return to the null pointer
    // exception generator.  The return address must be the address of the
    // next instruction (this instruction + 2).  The return address
    // is on the stack at the top address of the current frame.

    // Push the return address onto the stack.
    uint32_t retaddr = reinterpret_cast<uint32_t>(pc + 2);
    uint32_t* next_sp = reinterpret_cast<uint32_t*>(sp - 4);
    *next_sp = retaddr;
    uc->uc_mcontext.gregs[REG_ESP] = reinterpret_cast<uint32_t>(next_sp);

    uc->uc_mcontext.gregs[REG_EIP] = reinterpret_cast<uintptr_t>(art_quick_test_suspend);

    // Now remove the suspend trigger that caused this fault.
    Thread::Current()->RemoveSuspendTrigger();
    VLOG(signals) << "removed suspend trigger invoking test suspend";
    return true;
  }
  VLOG(signals) << "Not a suspend check match, first instruction mismatch";
  return false;
}

// The stack overflow check is done using the following instruction:
// test eax, [esp+ -xxx]
// where 'xxx' is the size of the overflow area.
//
// This is done before any frame is established in the method.  The return
// address for the previous method is on the stack at ESP.

bool StackOverflowHandler::Action(int sig, siginfo_t* info, void* context) {
  struct ucontext *uc = reinterpret_cast<struct ucontext*>(context);
  uintptr_t sp = static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_ESP]);

  uintptr_t fault_addr = reinterpret_cast<uintptr_t>(info->si_addr);
  VLOG(signals) << "fault_addr: " << std::hex << fault_addr;
  VLOG(signals) << "checking for stack overflow, sp: " << std::hex << sp <<
    ", fault_addr: " << fault_addr;

  uintptr_t overflow_addr = sp - GetStackOverflowReservedBytes(kX86);

  Thread* self = Thread::Current();
  uintptr_t pregion = reinterpret_cast<uintptr_t>(self->GetStackEnd()) -
      Thread::kStackOverflowProtectedSize;

  // Check that the fault address is the value expected for a stack overflow.
  if (fault_addr != overflow_addr) {
    VLOG(signals) << "Not a stack overflow";
    return false;
  }

  // We know this is a stack overflow.  We need to move the sp to the overflow region
  // that exists below the protected region.  Determine the address of the next
  // available valid address below the protected region.
  VLOG(signals) << "setting sp to overflow region at " << std::hex << pregion;

  // Since the compiler puts the implicit overflow
  // check before the callee save instructions, the SP is already pointing to
  // the previous frame.

  // Tell the stack overflow code where the new stack pointer should be.
  uc->uc_mcontext.gregs[REG_EAX] = pregion;

  // Now arrange for the signal handler to return to art_quick_throw_stack_overflow_from_signal.
  uc->uc_mcontext.gregs[REG_EIP] = reinterpret_cast<uintptr_t>(
    art_quick_throw_stack_overflow_from_signal);

  return true;
}
}       // namespace art
