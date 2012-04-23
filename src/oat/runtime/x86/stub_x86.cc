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

#include "jni_internal.h"
#include "oat/runtime/oat_support_entrypoints.h"
#include "oat/utils/x86/assembler_x86.h"
#include "object.h"
#include "stack_indirect_reference_table.h"

#define __ assembler->

namespace art {
namespace x86 {

ByteArray* X86CreateResolutionTrampoline(Runtime::TrampolineType type) {
  UniquePtr<X86Assembler> assembler(static_cast<X86Assembler*>(Assembler::Create(kX86)));

#if !defined(ART_USE_LLVM_COMPILER)
  // Set up the callee save frame to conform with Runtime::CreateCalleeSaveMethod(kRefsAndArgs)
  // return address
  __ pushl(EDI);
  __ pushl(ESI);
  __ pushl(EBP);
  __ pushl(EBX);
  __ pushl(EDX);
  __ pushl(ECX);
  __ pushl(EAX);  // <-- callee save Method* to go here
  __ movl(ECX, ESP);          // save ESP
  __ pushl(Immediate(type));  // pass is_static
  __ fs()->pushl(Address::Absolute(Thread::SelfOffset()));  // Thread*
  __ pushl(ECX);              // pass ESP for Method*
  __ pushl(EAX);              // pass Method*

  // Call to resolve method.
  __ Call(ThreadOffset(ENTRYPOINT_OFFSET(pUnresolvedDirectMethodTrampolineFromCode)),
          X86ManagedRegister::FromCpuRegister(ECX));

  __ movl(EDI, EAX);  // save code pointer in EDI
  __ addl(ESP, Immediate(16));  // Pop arguments
  __ popl(EAX);  // Restore args.
  __ popl(ECX);
  __ popl(EDX);
  __ popl(EBX);
  __ popl(EBP);  // Restore callee saves.
  __ popl(ESI);
  // Swap EDI callee save with code pointer
  __ xchgl(EDI, Address(ESP,0));
  // Tail call to intended method.
  __ ret();
#else // ART_USE_LLVM_COMPILER
  __ pushl(EBP);
  __ movl(EBP, ESP);          // save ESP
  __ subl(ESP, Immediate(8));  // Align stack
  __ movl(EAX, Address(EBP,8));  // Method* called
  __ leal(EDX, Address(EBP,8));  // Method** called_addr
  __ pushl(Immediate(type));  // pass is_static
  __ fs()->pushl(Address::Absolute(Thread::SelfOffset()));  // pass thread
  __ pushl(EDX);  // pass called_addr
  __ pushl(EAX);  // pass called

  // Call to resolve method.
  __ Call(ThreadOffset(ENTRYPOINT_OFFSET(pUnresolvedDirectMethodTrampolineFromCode)),
          X86ManagedRegister::FromCpuRegister(ECX));

  __ leave();

  Label resolve_fail;  // forward declaration
  __ cmpl(EAX, Immediate(0));
  __ j(kEqual, &resolve_fail);

  __ jmp(EAX);
  // Tail call to intended method.

  __ Bind(&resolve_fail);
  __ ret();
#endif // ART_USE_LLVM_COMPILER

  assembler->EmitSlowPaths();
  size_t cs = assembler->CodeSize();
  SirtRef<ByteArray> resolution_trampoline(ByteArray::Alloc(cs));
  CHECK(resolution_trampoline.get() != NULL);
  MemoryRegion code(resolution_trampoline->GetData(), resolution_trampoline->GetLength());
  assembler->FinalizeInstructions(code);

  return resolution_trampoline.get();
}

typedef void (*ThrowAme)(Method*, Thread*);

ByteArray* CreateAbstractMethodErrorStub() {
  UniquePtr<X86Assembler> assembler(static_cast<X86Assembler*>(Assembler::Create(kX86)));

#if !defined(ART_USE_LLVM_COMPILER)
  // Set up the callee save frame to conform with Runtime::CreateCalleeSaveMethod(kSaveAll)

  // return address
  __ pushl(EDI);
  __ pushl(ESI);
  __ pushl(EBP);
  __ pushl(Immediate(0));
  __ pushl(Immediate(0));
  __ pushl(Immediate(0));
  __ pushl(Immediate(0));  // <-- callee save Method* to go here
  __ movl(ECX, ESP);       // save ESP
  __ pushl(Immediate(0));  // align frame
  __ pushl(ECX);           // pass ESP for Method*
  __ fs()->pushl(Address::Absolute(Thread::SelfOffset()));  // Thread*
  __ pushl(EAX);           // pass Method*

  // Call to throw AbstractMethodError.
  __ Call(ThreadOffset(ENTRYPOINT_OFFSET(pThrowAbstractMethodErrorFromCode)),
          X86ManagedRegister::FromCpuRegister(ECX));

  // Call never returns.
  __ int3();
#else // ART_USE_LLVM_COMPILER
  __ pushl(EBP);
  __ movl(EBP, ESP);          // save ESP
  __ subl(ESP, Immediate(12));  // Align stack
  __ pushl(ESP);  // pass sp (not use)
  __ fs()->pushl(Address::Absolute(Thread::SelfOffset()));  // pass thread*
  __ pushl(Address(EBP,8));  // pass method
  // Call to throw AbstractMethodError.
  __ Call(ThreadOffset(ENTRYPOINT_OFFSET(pThrowAbstractMethodErrorFromCode)),
          X86ManagedRegister::FromCpuRegister(ECX));
  __ leave();
  // Return to caller who will handle pending exception.
  __ ret();
#endif // ART_USE_LLVM_COMPILER

  assembler->EmitSlowPaths();

  size_t cs = assembler->CodeSize();
  SirtRef<ByteArray> abstract_stub(ByteArray::Alloc(cs));
  CHECK(abstract_stub.get() != NULL);
  MemoryRegion code(abstract_stub->GetData(), abstract_stub->GetLength());
  assembler->FinalizeInstructions(code);

  return abstract_stub.get();
}

ByteArray* CreateJniDlsymLookupStub() {
  UniquePtr<X86Assembler> assembler(static_cast<X86Assembler*>(Assembler::Create(kX86)));

  // Pad stack to ensure 16-byte alignment
  __ pushl(Immediate(0));
  __ pushl(Immediate(0));
  __ fs()->pushl(Address::Absolute(Thread::SelfOffset()));  // Thread*

  __ Call(ThreadOffset(ENTRYPOINT_OFFSET(pFindNativeMethod)),
          X86ManagedRegister::FromCpuRegister(ECX));

  __ addl(ESP, Immediate(12));

  Label no_native_code_found;  // forward declaration
  __ cmpl(EAX, Immediate(0));
  __ j(kEqual, &no_native_code_found);

  __ jmp(EAX);  // Tail call into native code

  __ Bind(&no_native_code_found);
  __ ret(); // return to caller to handle exception

  assembler->EmitSlowPaths();

  size_t cs = assembler->CodeSize();
  SirtRef<ByteArray> jni_stub(ByteArray::Alloc(cs));
  CHECK(jni_stub.get() != NULL);
  MemoryRegion code(jni_stub->GetData(), jni_stub->GetLength());
  assembler->FinalizeInstructions(code);

  return jni_stub.get();
}

} // namespace x86
} // namespace art
