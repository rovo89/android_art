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

#if defined(ART_USE_LLVM_COMPILER)
  // Return to caller who will handle pending exception.
  // TODO: The callee save set up is unnecessary for LLVM as it uses shadow stacks.
  __ addl(ESP, Immediate(32));
  __ popl(EBP);
  __ popl(ESI);
  __ popl(EDI);
  __ ret();
#else
  // Call never returns.
  __ int3();
#endif

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
