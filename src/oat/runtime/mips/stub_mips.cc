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
#include "oat/runtime/stub.h"
#include "oat/utils/mips/assembler_mips.h"
#include "object.h"
#include "stack_indirect_reference_table.h"
#include "sirt_ref.h"

#define __ assembler->

namespace art {
namespace mips {

ByteArray* MipsCreateResolutionTrampoline(Runtime::TrampolineType type) {
  UniquePtr<MipsAssembler> assembler(static_cast<MipsAssembler*>(Assembler::Create(kMips)));
#if !defined(ART_USE_LLVM_COMPILER)
  // | Out args   |
  // | Method*    | <- SP on entry
  // | RA         |    return address into caller
  // | ...        |    callee saves
  // | A3         |    possible argument
  // | A2         |    possible argument
  // | A1         |    possible argument
  // | A0/Method* |    Callee save Method* set up by UnresolvedDirectMethodTrampolineFromCode
  // Save callee saves and ready frame for exception delivery
  __ AddConstant(SP, SP, -48);
  __ StoreToOffset(kStoreWord, RA, SP, 44);
  __ StoreToOffset(kStoreWord, FP, SP, 40);
  __ StoreToOffset(kStoreWord, S7, SP, 36);
  __ StoreToOffset(kStoreWord, S6, SP, 32);
  __ StoreToOffset(kStoreWord, S5, SP, 28);
  __ StoreToOffset(kStoreWord, S4, SP, 24);
  __ StoreToOffset(kStoreWord, S3, SP, 20);
  __ StoreToOffset(kStoreWord, S2, SP, 16);
  __ StoreToOffset(kStoreWord, A3, SP, 12);
  __ StoreToOffset(kStoreWord, A2, SP, 8);
  __ StoreToOffset(kStoreWord, A1, SP, 4);

  __ LoadFromOffset(kLoadWord, T9, S1,
                    ENTRYPOINT_OFFSET(pUnresolvedDirectMethodTrampolineFromCode));
  __ Move(A2, S1);  // Pass Thread::Current() in A2
  __ LoadImmediate(A3, type); // Pass is_static
  __ Move(A1, SP);  // Pass SP for Method** callee_addr
  __ Jalr(T9); // Call to unresolved direct method trampoline (method_idx, sp, Thread*, is_static)

  // Restore registers which may have been modified by GC
  __ LoadFromOffset(kLoadWord, A0, SP, 0);
  __ LoadFromOffset(kLoadWord, A1, SP, 4);
  __ LoadFromOffset(kLoadWord, A2, SP, 8);
  __ LoadFromOffset(kLoadWord, A3, SP, 12);
  __ LoadFromOffset(kLoadWord, S2, SP, 16);
  __ LoadFromOffset(kLoadWord, S3, SP, 20);
  __ LoadFromOffset(kLoadWord, S4, SP, 24);
  __ LoadFromOffset(kLoadWord, S5, SP, 28);
  __ LoadFromOffset(kLoadWord, S6, SP, 32);
  __ LoadFromOffset(kLoadWord, S7, SP, 36);
  __ LoadFromOffset(kLoadWord, FP, SP, 40);
  __ LoadFromOffset(kLoadWord, RA, SP, 44);
  __ AddConstant(SP, SP, 48);

  __ Move(T9, V0); // Put method's code in T9
  __ Jr(T9);  // Leaf call to method's code

  __ Break();
#else // ART_USE_LLVM_COMPILER
  // Build frame and save argument registers and RA.
  __ AddConstant(SP, SP, -32);
  __ StoreToOffset(kStoreWord, RA, SP, 28);
  __ StoreToOffset(kStoreWord, A3, SP, 24);
  __ StoreToOffset(kStoreWord, A2, SP, 20);
  __ StoreToOffset(kStoreWord, A1, SP, 16);
  __ StoreToOffset(kStoreWord, A0, SP, 12);

  __ LoadFromOffset(kLoadWord, T9, S1,
                    ENTRYPOINT_OFFSET(pUnresolvedDirectMethodTrampolineFromCode));
  __ Move(A2, S1);  // Pass Thread::Current() in A2
  __ LoadImmediate(A3, type);  // Pass is_static
  __ Move(A1, SP);  // Pass SP for Method** callee_addr
  __ Jalr(T9); // Call to unresolved direct method trampoline (callee, callee_addr, Thread*, is_static)

  // Restore frame, argument registers, and RA.
  __ LoadFromOffset(kLoadWord, A0, SP, 12);
  __ LoadFromOffset(kLoadWord, A1, SP, 16);
  __ LoadFromOffset(kLoadWord, A2, SP, 20);
  __ LoadFromOffset(kLoadWord, A3, SP, 24);
  __ LoadFromOffset(kLoadWord, RA, SP, 28);
  __ AddConstant(SP, SP, 32);

  Label resolve_fail;
  __ EmitBranch(V0, ZERO, &resolve_fail, true);
  __ Jr(V0); // If V0 != 0 tail call method's code
  __ Bind(&resolve_fail, false);
  __ Jr(RA); // Return to caller to handle exception
#endif // ART_USE_LLVM_COMPILER

  assembler->EmitSlowPaths();

  size_t cs = assembler->CodeSize();
  Thread* self = Thread::Current();
  SirtRef<ByteArray> resolution_trampoline(self, ByteArray::Alloc(self, cs));
  CHECK(resolution_trampoline.get() != NULL);
  MemoryRegion code(resolution_trampoline->GetData(), resolution_trampoline->GetLength());
  assembler->FinalizeInstructions(code);

  return resolution_trampoline.get();
}

typedef void (*ThrowAme)(AbstractMethod*, Thread*);

ByteArray* CreateAbstractMethodErrorStub() {
  UniquePtr<MipsAssembler> assembler(static_cast<MipsAssembler*>(Assembler::Create(kMips)));
#if !defined(ART_USE_LLVM_COMPILER)
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
#else // ART_USE_LLVM_COMPILER
  // R0 is the Method* already
  __ Move(A1, S1);  // Pass Thread::Current() in A1
  // Call to throw AbstractMethodError
  __ LoadFromOffset(kLoadWord, T9, S1, ENTRYPOINT_OFFSET(pThrowAbstractMethodErrorFromCode));
  __ Jr(T9);  // Leaf call to routine that never returns

  __ Break();
#endif // ART_USE_LLVM_COMPILER

  assembler->EmitSlowPaths();

  size_t cs = assembler->CodeSize();
  Thread* self = Thread::Current();
  SirtRef<ByteArray> abstract_stub(self, ByteArray::Alloc(self, cs));
  CHECK(abstract_stub.get() != NULL);
  MemoryRegion code(abstract_stub->GetData(), abstract_stub->GetLength());
  assembler->FinalizeInstructions(code);

  return abstract_stub.get();
}

ByteArray* CreateJniDlsymLookupStub() {
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
  SirtRef<ByteArray> jni_stub(self, ByteArray::Alloc(self, cs));
  CHECK(jni_stub.get() != NULL);
  MemoryRegion code(jni_stub->GetData(), jni_stub->GetLength());
  assembler->FinalizeInstructions(code);

  return jni_stub.get();
}

} // namespace mips
} // namespace art
