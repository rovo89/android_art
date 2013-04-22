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
#include "mirror/array.h"
#include "mirror/object-inl.h"
#include "oat/runtime/oat_support_entrypoints.h"
#include "oat/runtime/stub.h"
#include "oat/utils/mips/assembler_mips.h"
#include "stack_indirect_reference_table.h"
#include "sirt_ref.h"

#define __ assembler->

namespace art {
namespace mips {

typedef void (*ThrowAme)(mirror::AbstractMethod*, Thread*);

mirror::ByteArray* CreateAbstractMethodErrorStub() {
  UniquePtr<MipsAssembler> assembler(static_cast<MipsAssembler*>(Assembler::Create(kMips)));
#if !defined(ART_USE_PORTABLE_COMPILER)
  // Save callee saves and ready frame for exception delivery
  __ AddConstant(SP, SP, -64);
  __ StoreToOffset(kStoreWord, RA, SP, 60);
  __ StoreToOffset(kStoreWord, FP, SP, 56);
  __ StoreToOffset(kStoreWord, S7, SP, 52);
  __ StoreToOffset(kStoreWord, S6, SP, 48);
  __ StoreToOffset(kStoreWord, S5, SP, 44);
  __ StoreToOffset(kStoreWord, S4, SP, 40);
  __ StoreToOffset(kStoreWord, S3, SP, 36);
  __ StoreToOffset(kStoreWord, S2, SP, 32);
  __ StoreToOffset(kStoreWord, S1, SP, 28);
  __ StoreToOffset(kStoreWord, S0, SP, 24);

  // A0 is the Method* already
  __ Move(A1, S1);  // Pass Thread::Current() in A1
  __ Move(A2, SP);  // Pass SP in A2
  // Call to throw AbstractMethodError
  __ LoadFromOffset(kLoadWord, T9, S1, ENTRYPOINT_OFFSET(pThrowAbstractMethodErrorFromCode));
  __ Jr(T9);  // Leaf call to routine that never returns

  __ Break();
#else // ART_USE_PORTABLE_COMPILER
  // R0 is the Method* already
  __ Move(A1, S1);  // Pass Thread::Current() in A1
  // Call to throw AbstractMethodError
  __ LoadFromOffset(kLoadWord, T9, S1, ENTRYPOINT_OFFSET(pThrowAbstractMethodErrorFromCode));
  __ Jr(T9);  // Leaf call to routine that never returns

  __ Break();
#endif // ART_USE_PORTABLE_COMPILER

  assembler->EmitSlowPaths();

  size_t cs = assembler->CodeSize();
  Thread* self = Thread::Current();
  SirtRef<mirror::ByteArray> abstract_stub(self, mirror::ByteArray::Alloc(self, cs));
  CHECK(abstract_stub.get() != NULL);
  MemoryRegion code(abstract_stub->GetData(), abstract_stub->GetLength());
  assembler->FinalizeInstructions(code);

  return abstract_stub.get();
}

mirror::ByteArray* CreateJniDlsymLookupStub() {
  UniquePtr<MipsAssembler> assembler(static_cast<MipsAssembler*>(Assembler::Create(kMips)));

  // Build frame and save argument registers and RA.
  __ AddConstant(SP, SP, -32);
  __ StoreToOffset(kStoreWord, RA, SP, 28);
  __ StoreToOffset(kStoreWord, A3, SP, 24);
  __ StoreToOffset(kStoreWord, A2, SP, 20);
  __ StoreToOffset(kStoreWord, A1, SP, 16);
  __ StoreToOffset(kStoreWord, A0, SP, 12);

  __ Move(A0, S1); // Pass Thread::Current() in A0
  __ LoadFromOffset(kLoadWord, T9, S1, ENTRYPOINT_OFFSET(pFindNativeMethod));
  __ Jalr(T9); // Call FindNativeMethod

  // Restore frame, argument registers, and RA.
  __ LoadFromOffset(kLoadWord, A0, SP, 12);
  __ LoadFromOffset(kLoadWord, A1, SP, 16);
  __ LoadFromOffset(kLoadWord, A2, SP, 20);
  __ LoadFromOffset(kLoadWord, A3, SP, 24);
  __ LoadFromOffset(kLoadWord, RA, SP, 28);
  __ AddConstant(SP, SP, 32);

  Label no_native_code_found;
  __ EmitBranch(V0, ZERO, &no_native_code_found, true);
  __ Move(T9, V0); // Move result into T9
  __ Jr(T9); // If result != 0, tail call method's code
  __ Bind(&no_native_code_found, false);
  __ Jr(RA); // Return to caller to handle exception

  assembler->EmitSlowPaths();

  size_t cs = assembler->CodeSize();
  Thread* self = Thread::Current();
  SirtRef<mirror::ByteArray> jni_stub(self, mirror::ByteArray::Alloc(self, cs));
  CHECK(jni_stub.get() != NULL);
  MemoryRegion code(jni_stub->GetData(), jni_stub->GetLength());
  assembler->FinalizeInstructions(code);

  return jni_stub.get();
}

} // namespace mips
} // namespace art
