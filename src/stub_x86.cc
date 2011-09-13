// Copyright 2011 Google Inc. All Rights Reserved.

#include "assembler_x86.h"
#include "jni_internal.h"
#include "object.h"

#define __ assembler->

namespace art {
namespace x86 {

typedef void (*ThrowAme)(Method*, Thread*);

ByteArray* CreateAbstractMethodErrorStub(ThrowAme throw_ame) {
  UniquePtr<X86Assembler> assembler( static_cast<X86Assembler*>(Assembler::Create(kX86)) );

  // Pad stack to ensure 16-byte alignment
  __ pushl(Immediate(0));
  __ pushl(Immediate(0));
  __ fs()->pushl(Address::Absolute(Thread::SelfOffset()));  // Thread*
  __ pushl(EDI); // Method*

  // Call throw_ame to throw AbstractMethodError
  // TODO: make this PIC (throw_ame will not be in the same location after image load)
  // TODO: remove X86Assembler::Call(uintptr_t addr, ManagedRegister scratch)
  __ Call(reinterpret_cast<int32_t>(throw_ame), X86ManagedRegister::FromCpuRegister(ECX));

  // Because the call above never returns, we do not need to do ESP+=16 here.

  __ int3();

  assembler->EmitSlowPaths();

  size_t cs = assembler->CodeSize();
  ByteArray* abstract_stub = ByteArray::Alloc(cs);
  CHECK(abstract_stub != NULL);
  MemoryRegion code(abstract_stub->GetData(), abstract_stub->GetLength());
  assembler->FinalizeInstructions(code);

  return abstract_stub;
}

ByteArray* CreateJniStub() {
  UniquePtr<X86Assembler> assembler( static_cast<X86Assembler*>(Assembler::Create(kX86)) );

  // Pad stack to ensure 16-byte alignment
  __ pushl(Immediate(0));
  __ pushl(Immediate(0));
  __ pushl(Immediate(0));
  __ fs()->movl(ECX, Address::Absolute(Thread::SelfOffset()));
  __ pushl(ECX);  // Thread*

  // TODO: make this PIC (FindNativeMethod will not be in the same location after image load)
  // TODO: remove X86Assembler::Call(uintptr_t addr, ManagedRegister scratch)
  __ Call(reinterpret_cast<int32_t>(&FindNativeMethod), X86ManagedRegister::FromCpuRegister(ECX));

  __ addl(ESP, Immediate(16));

  Label no_native_code_found;  // forward declaration
  __ cmpl(EAX, Immediate(0));
  __ j(kEqual, &no_native_code_found);

  __ jmp(EAX);  // Tail call into native code

  __ Bind(&no_native_code_found);
  __ ret(); // return to caller to handle exception

  assembler->EmitSlowPaths();

  size_t cs = assembler->CodeSize();
  ByteArray* jni_stub = ByteArray::Alloc(cs);
  CHECK(jni_stub != NULL);
  MemoryRegion code(jni_stub->GetData(), jni_stub->GetLength());
  assembler->FinalizeInstructions(code);

  return jni_stub;
}

} // namespace x86
} // namespace art
