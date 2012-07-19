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

#include <vector>

#include "calling_convention.h"
#include "class_linker.h"
#include "compiled_method.h"
#include "compiler.h"
#include "disassembler.h"
#include "jni_internal.h"
#include "logging.h"
#include "macros.h"
#include "oat/runtime/oat_support_entrypoints.h"
#include "oat/utils/assembler.h"
#include "oat/utils/managed_register.h"
#include "oat/utils/arm/managed_register_arm.h"
#include "oat/utils/x86/managed_register_x86.h"
#include "thread.h"
#include "UniquePtr.h"

#define __ jni_asm->

namespace art {

static void CopyParameter(Assembler* jni_asm,
                          ManagedRuntimeCallingConvention* mr_conv,
                          JniCallingConvention* jni_conv,
                          size_t frame_size, size_t out_arg_size);
static void SetNativeParameter(Assembler* jni_asm,
                               JniCallingConvention* jni_conv,
                               ManagedRegister in_reg);

// Generate the JNI bridge for the given method, general contract:
// - Arguments are in the managed runtime format, either on stack or in
//   registers, a reference to the method object is supplied as part of this
//   convention.
//
CompiledMethod* ArtJniCompileMethodInternal(Compiler& compiler,
                                            uint32_t access_flags, uint32_t method_idx,
                                            const DexFile& dex_file) {
  const bool is_native = (access_flags & kAccNative) != 0;
  CHECK(is_native);
  const bool is_static = (access_flags & kAccStatic) != 0;
  const bool is_synchronized = (access_flags & kAccSynchronized) != 0;
  const char* shorty = dex_file.GetMethodShorty(dex_file.GetMethodId(method_idx));
  InstructionSet instruction_set = compiler.GetInstructionSet();
  if (instruction_set == kThumb2) {
    instruction_set = kArm;
  }
  // Calling conventions used to iterate over parameters to method
  UniquePtr<JniCallingConvention> jni_conv(
      JniCallingConvention::Create(is_static, is_synchronized, shorty, instruction_set));
  UniquePtr<ManagedRuntimeCallingConvention> mr_conv(
      ManagedRuntimeCallingConvention::Create(is_static, is_synchronized, shorty, instruction_set));

  // Assembler that holds generated instructions
  UniquePtr<Assembler> jni_asm(Assembler::Create(instruction_set));
  bool should_disassemble = false;

  // Offsets into data structures
  // TODO: if cross compiling these offsets are for the host not the target
  const Offset functions(OFFSETOF_MEMBER(JNIEnvExt, functions));
  const Offset monitor_enter(OFFSETOF_MEMBER(JNINativeInterface, MonitorEnter));
  const Offset monitor_exit(OFFSETOF_MEMBER(JNINativeInterface, MonitorExit));

  // 1. Build the frame saving all callee saves
  const size_t frame_size(jni_conv->FrameSize());
  const std::vector<ManagedRegister>& callee_save_regs = jni_conv->CalleeSaveRegisters();
  __ BuildFrame(frame_size, mr_conv->MethodRegister(), callee_save_regs, mr_conv->EntrySpills());

  // 2. Set up the StackIndirectReferenceTable
  mr_conv->ResetIterator(FrameOffset(frame_size));
  jni_conv->ResetIterator(FrameOffset(0));
  __ StoreImmediateToFrame(jni_conv->SirtNumRefsOffset(),
                           jni_conv->ReferenceCount(),
                           mr_conv->InterproceduralScratchRegister());
  __ CopyRawPtrFromThread(jni_conv->SirtLinkOffset(),
                          Thread::TopSirtOffset(),
                          mr_conv->InterproceduralScratchRegister());
  __ StoreStackOffsetToThread(Thread::TopSirtOffset(),
                              jni_conv->SirtOffset(),
                              mr_conv->InterproceduralScratchRegister());

  // 3. Place incoming reference arguments into SIRT
  jni_conv->Next();  // Skip JNIEnv*
  // 3.5. Create Class argument for static methods out of passed method
  if (is_static) {
    FrameOffset sirt_offset = jni_conv->CurrentParamSirtEntryOffset();
    // Check sirt offset is within frame
    CHECK_LT(sirt_offset.Uint32Value(), frame_size);
    __ LoadRef(jni_conv->InterproceduralScratchRegister(),
               mr_conv->MethodRegister(), Method::DeclaringClassOffset());
    __ VerifyObject(jni_conv->InterproceduralScratchRegister(), false);
    __ StoreRef(sirt_offset, jni_conv->InterproceduralScratchRegister());
    jni_conv->Next();  // in SIRT so move to next argument
  }
  while (mr_conv->HasNext()) {
    CHECK(jni_conv->HasNext());
    bool ref_param = jni_conv->IsCurrentParamAReference();
    CHECK(!ref_param || mr_conv->IsCurrentParamAReference());
    // References need placing in SIRT and the entry value passing
    if (ref_param) {
      // Compute SIRT entry, note null is placed in the SIRT but its boxed value
      // must be NULL
      FrameOffset sirt_offset = jni_conv->CurrentParamSirtEntryOffset();
      // Check SIRT offset is within frame and doesn't run into the saved segment state
      CHECK_LT(sirt_offset.Uint32Value(), frame_size);
      CHECK_NE(sirt_offset.Uint32Value(),
               jni_conv->SavedLocalReferenceCookieOffset().Uint32Value());
      bool input_in_reg = mr_conv->IsCurrentParamInRegister();
      bool input_on_stack = mr_conv->IsCurrentParamOnStack();
      CHECK(input_in_reg || input_on_stack);

      if (input_in_reg) {
        ManagedRegister in_reg  =  mr_conv->CurrentParamRegister();
        __ VerifyObject(in_reg, mr_conv->IsCurrentArgPossiblyNull());
        __ StoreRef(sirt_offset, in_reg);
      } else if (input_on_stack) {
        FrameOffset in_off  = mr_conv->CurrentParamStackOffset();
        __ VerifyObject(in_off, mr_conv->IsCurrentArgPossiblyNull());
        __ CopyRef(sirt_offset, in_off,
                   mr_conv->InterproceduralScratchRegister());
      }
    }
    mr_conv->Next();
    jni_conv->Next();
  }

  // 4. Write out the end of the quick frames.
  __ StoreStackPointerToThread(Thread::TopOfManagedStackOffset());
  __ StoreImmediateToThread(Thread::TopOfManagedStackPcOffset(), 0,
                            mr_conv->InterproceduralScratchRegister());

  // 5. Move frame down to allow space for out going args.
  const size_t out_arg_size = jni_conv->OutArgSize();
  __ IncreaseFrameSize(out_arg_size);


  // 6. Call into appropriate JniMethodStart passing Thread* so that transition out of Runnable
  //    can occur. The result is the saved JNI local state that is restored by the exit call. We
  //    abuse the JNI calling convention here, that is guaranteed to support passing 2 pointer
  //    arguments.
  uintptr_t jni_start = is_synchronized ? ENTRYPOINT_OFFSET(pJniMethodStartSynchronized)
                                        : ENTRYPOINT_OFFSET(pJniMethodStart);
  jni_conv->ResetIterator(FrameOffset(out_arg_size));
  FrameOffset locked_object_sirt_offset(0);
  if (is_synchronized) {
    // Pass object for locking.
    jni_conv->Next();  // Skip JNIEnv.
    locked_object_sirt_offset = jni_conv->CurrentParamSirtEntryOffset();
    jni_conv->ResetIterator(FrameOffset(out_arg_size));
    if (jni_conv->IsCurrentParamOnStack()) {
      FrameOffset out_off = jni_conv->CurrentParamStackOffset();
      __ CreateSirtEntry(out_off, locked_object_sirt_offset,
                         mr_conv->InterproceduralScratchRegister(),
                         false);
    } else {
      ManagedRegister out_reg = jni_conv->CurrentParamRegister();
      __ CreateSirtEntry(out_reg, locked_object_sirt_offset,
                         ManagedRegister::NoRegister(), false);
    }
    jni_conv->Next();
  }
  if (jni_conv->IsCurrentParamInRegister()) {
    __ GetCurrentThread(jni_conv->CurrentParamRegister());
    __ Call(jni_conv->CurrentParamRegister(), Offset(jni_start),
            jni_conv->InterproceduralScratchRegister());
  } else {
    __ GetCurrentThread(jni_conv->CurrentParamStackOffset(),
                        jni_conv->InterproceduralScratchRegister());
    __ Call(ThreadOffset(jni_start), jni_conv->InterproceduralScratchRegister());
  }
  if (is_synchronized) {  // Check for exceptions from monitor enter.
    __ ExceptionPoll(jni_conv->InterproceduralScratchRegister(), out_arg_size);
  }
  FrameOffset saved_cookie_offset = jni_conv->SavedLocalReferenceCookieOffset();
  __ Store(saved_cookie_offset, jni_conv->IntReturnRegister(), 4);

  // 7. Iterate over arguments placing values from managed calling convention in
  //    to the convention required for a native call (shuffling). For references
  //    place an index/pointer to the reference after checking whether it is
  //    NULL (which must be encoded as NULL).
  //    Note: we do this prior to materializing the JNIEnv* and static's jclass to
  //    give as many free registers for the shuffle as possible
  mr_conv->ResetIterator(FrameOffset(frame_size+out_arg_size));
  uint32_t args_count = 0;
  while (mr_conv->HasNext()) {
    args_count++;
    mr_conv->Next();
  }

  // Do a backward pass over arguments, so that the generated code will be "mov
  // R2, R3; mov R1, R2" instead of "mov R1, R2; mov R2, R3."
  // TODO: A reverse iterator to improve readability.
  for (uint32_t i = 0; i < args_count; ++i) {
    mr_conv->ResetIterator(FrameOffset(frame_size + out_arg_size));
    jni_conv->ResetIterator(FrameOffset(out_arg_size));
    jni_conv->Next();  // Skip JNIEnv*.
    if (is_static) {
      jni_conv->Next();  // Skip Class for now.
    }
    // Skip to the argument we're interested in.
    for (uint32_t j = 0; j < args_count - i - 1; ++j) {
      mr_conv->Next();
      jni_conv->Next();
    }
    CopyParameter(jni_asm.get(), mr_conv.get(), jni_conv.get(), frame_size, out_arg_size);
  }
  if (is_static) {
    // Create argument for Class
    mr_conv->ResetIterator(FrameOffset(frame_size+out_arg_size));
    jni_conv->ResetIterator(FrameOffset(out_arg_size));
    jni_conv->Next();  // Skip JNIEnv*
    FrameOffset sirt_offset = jni_conv->CurrentParamSirtEntryOffset();
    if (jni_conv->IsCurrentParamOnStack()) {
      FrameOffset out_off = jni_conv->CurrentParamStackOffset();
      __ CreateSirtEntry(out_off, sirt_offset,
                         mr_conv->InterproceduralScratchRegister(),
                         false);
    } else {
      ManagedRegister out_reg = jni_conv->CurrentParamRegister();
      __ CreateSirtEntry(out_reg, sirt_offset,
                         ManagedRegister::NoRegister(), false);
    }
  }

  // 8. Create 1st argument, the JNI environment ptr.
  jni_conv->ResetIterator(FrameOffset(out_arg_size));
  // Register that will hold local indirect reference table
  if (jni_conv->IsCurrentParamInRegister()) {
    ManagedRegister jni_env = jni_conv->CurrentParamRegister();
    DCHECK(!jni_env.Equals(jni_conv->InterproceduralScratchRegister()));
    __ LoadRawPtrFromThread(jni_env, Thread::JniEnvOffset());
  } else {
    FrameOffset jni_env = jni_conv->CurrentParamStackOffset();
    __ CopyRawPtrFromThread(jni_env, Thread::JniEnvOffset(),
                            jni_conv->InterproceduralScratchRegister());
  }

  // 9. Plant call to native code associated with method.
  __ Call(jni_conv->MethodStackOffset(), Method::NativeMethodOffset(),
          mr_conv->InterproceduralScratchRegister());

  // 10. Fix differences in result widths.
  if (instruction_set == kX86) {
    if (jni_conv->GetReturnType() == Primitive::kPrimByte ||
        jni_conv->GetReturnType() == Primitive::kPrimShort) {
      __ SignExtend(jni_conv->ReturnRegister(),
                    Primitive::ComponentSize(jni_conv->GetReturnType()));
    } else if (jni_conv->GetReturnType() == Primitive::kPrimBoolean ||
               jni_conv->GetReturnType() == Primitive::kPrimChar) {
      __ ZeroExtend(jni_conv->ReturnRegister(),
                    Primitive::ComponentSize(jni_conv->GetReturnType()));
    }
  }

  // 11. Save return value
  bool reference_return = jni_conv->IsReturnAReference();
  FrameOffset return_save_location = jni_conv->ReturnValueSaveLocation();
  if (jni_conv->SizeOfReturnValue() != 0 && !reference_return) {
    CHECK_LT(return_save_location.Uint32Value(), frame_size+out_arg_size);
    __ Store(return_save_location, jni_conv->ReturnRegister(), jni_conv->SizeOfReturnValue());
  }

  // 12. Call into JNI method end possibly passing a returned reference, the method and the current
  //     thread.
  {
    // Modify iterator for call, important offsets were saved above.
    size_t jni_end_arg_count = 0;
    if (reference_return) { jni_end_arg_count++; }
    if (is_synchronized) { jni_end_arg_count++; }
    const char* jni_end_shorty = jni_end_arg_count == 0 ? "I"
                                                        : (jni_end_arg_count == 1 ? "II" : "III");
    jni_conv.reset(JniCallingConvention::Create(is_static, is_synchronized, jni_end_shorty,
                                                instruction_set));
    // Ensure out arguments will fit in space taken before (we expect this due to stack alignment).
    size_t jni_end_out_arg_size = jni_conv->OutArgSize();
    CHECK_LE(jni_end_out_arg_size, out_arg_size);
    jni_conv->ResetIterator(FrameOffset(jni_end_out_arg_size));
  }
  uintptr_t jni_end;
  if (reference_return) {
    // Pass result.
    jni_end = is_synchronized ? ENTRYPOINT_OFFSET(pJniMethodEndWithReferenceSynchronized)
                              : ENTRYPOINT_OFFSET(pJniMethodEndWithReference);
    SetNativeParameter(jni_asm.get(), jni_conv.get(), jni_conv->ReturnRegister());
    jni_conv->Next();
  } else {
    jni_end = is_synchronized ? ENTRYPOINT_OFFSET(pJniMethodEndSynchronized)
                              : ENTRYPOINT_OFFSET(pJniMethodEnd);
  }
  // Pass saved local reference state.
  if (jni_conv->IsCurrentParamOnStack()) {
    FrameOffset out_off = jni_conv->CurrentParamStackOffset();
    __ Copy(out_off, saved_cookie_offset, jni_conv->InterproceduralScratchRegister(), 4);
  } else {
    ManagedRegister out_reg = jni_conv->CurrentParamRegister();
    __ Load(out_reg, saved_cookie_offset, 4);
  }
  jni_conv->Next();
  if (is_synchronized) {
    // Pass object for unlocking.
    if (jni_conv->IsCurrentParamOnStack()) {
      FrameOffset out_off = jni_conv->CurrentParamStackOffset();
      __ CreateSirtEntry(out_off, locked_object_sirt_offset,
                         jni_conv->InterproceduralScratchRegister(),
                         false);
    } else {
      ManagedRegister out_reg = jni_conv->CurrentParamRegister();
      __ CreateSirtEntry(out_reg, locked_object_sirt_offset,
                         ManagedRegister::NoRegister(), false);
    }
    jni_conv->Next();
  }
  if (jni_conv->IsCurrentParamInRegister()) {
    __ GetCurrentThread(jni_conv->CurrentParamRegister());
    __ Call(jni_conv->CurrentParamRegister(), Offset(jni_end),
            jni_conv->InterproceduralScratchRegister());
  } else {
    __ GetCurrentThread(jni_conv->CurrentParamStackOffset(),
                        jni_conv->InterproceduralScratchRegister());
    __ Call(ThreadOffset(jni_end), jni_conv->InterproceduralScratchRegister());
  }

  // 13. Reload return value
  if (jni_conv->SizeOfReturnValue() != 0 && !reference_return) {
    __ Load(mr_conv->ReturnRegister(), return_save_location, mr_conv->SizeOfReturnValue());
  }

  // 14. Move frame up now we're done with the out arg space.
  __ DecreaseFrameSize(out_arg_size);

  // 15. Process pending exceptions from JNI call or monitor exit.
  __ ExceptionPoll(jni_conv->InterproceduralScratchRegister(), 0);

  // 16. Remove activation - no need to restore callee save registers because we didn't clobber
  //     them.
  __ RemoveFrame(frame_size, std::vector<ManagedRegister>());

  // 17. Finalize code generation
  __ EmitSlowPaths();
  size_t cs = __ CodeSize();
  std::vector<uint8_t> managed_code(cs);
  MemoryRegion code(&managed_code[0], managed_code.size());
  __ FinalizeInstructions(code);
  if (should_disassemble) {
    UniquePtr<Disassembler> disassembler(Disassembler::Create(instruction_set));
    disassembler->Dump(LOG(INFO), &managed_code[0], &managed_code[managed_code.size()]);
  }
  return new CompiledMethod(instruction_set,
                            managed_code,
                            frame_size,
                            jni_conv->CoreSpillMask(),
                            jni_conv->FpSpillMask());
}

// Copy a single parameter from the managed to the JNI calling convention
static void CopyParameter(Assembler* jni_asm,
                          ManagedRuntimeCallingConvention* mr_conv,
                          JniCallingConvention* jni_conv,
                          size_t frame_size, size_t out_arg_size) {
  bool input_in_reg = mr_conv->IsCurrentParamInRegister();
  bool output_in_reg = jni_conv->IsCurrentParamInRegister();
  FrameOffset sirt_offset(0);
  bool null_allowed = false;
  bool ref_param = jni_conv->IsCurrentParamAReference();
  CHECK(!ref_param || mr_conv->IsCurrentParamAReference());
  // input may be in register, on stack or both - but not none!
  CHECK(input_in_reg || mr_conv->IsCurrentParamOnStack());
  if (output_in_reg) {  // output shouldn't straddle registers and stack
    CHECK(!jni_conv->IsCurrentParamOnStack());
  } else {
    CHECK(jni_conv->IsCurrentParamOnStack());
  }
  // References need placing in SIRT and the entry address passing
  if (ref_param) {
    null_allowed = mr_conv->IsCurrentArgPossiblyNull();
    // Compute SIRT offset. Note null is placed in the SIRT but the jobject
    // passed to the native code must be null (not a pointer into the SIRT
    // as with regular references).
    sirt_offset = jni_conv->CurrentParamSirtEntryOffset();
    // Check SIRT offset is within frame.
    CHECK_LT(sirt_offset.Uint32Value(), (frame_size + out_arg_size));
  }
  if (input_in_reg && output_in_reg) {
    ManagedRegister in_reg = mr_conv->CurrentParamRegister();
    ManagedRegister out_reg = jni_conv->CurrentParamRegister();
    if (ref_param) {
      __ CreateSirtEntry(out_reg, sirt_offset, in_reg, null_allowed);
    } else {
      if (!mr_conv->IsCurrentParamOnStack()) {
        // regular non-straddling move
        __ Move(out_reg, in_reg, mr_conv->CurrentParamSize());
      } else {
        UNIMPLEMENTED(FATAL);  // we currently don't expect to see this case
      }
    }
  } else if (!input_in_reg && !output_in_reg) {
    FrameOffset out_off = jni_conv->CurrentParamStackOffset();
    if (ref_param) {
      __ CreateSirtEntry(out_off, sirt_offset, mr_conv->InterproceduralScratchRegister(),
                         null_allowed);
    } else {
      FrameOffset in_off = mr_conv->CurrentParamStackOffset();
      size_t param_size = mr_conv->CurrentParamSize();
      CHECK_EQ(param_size, jni_conv->CurrentParamSize());
      __ Copy(out_off, in_off, mr_conv->InterproceduralScratchRegister(), param_size);
    }
  } else if (!input_in_reg && output_in_reg) {
    FrameOffset in_off = mr_conv->CurrentParamStackOffset();
    ManagedRegister out_reg = jni_conv->CurrentParamRegister();
    // Check that incoming stack arguments are above the current stack frame.
    CHECK_GT(in_off.Uint32Value(), frame_size);
    if (ref_param) {
      __ CreateSirtEntry(out_reg, sirt_offset, ManagedRegister::NoRegister(), null_allowed);
    } else {
      size_t param_size = mr_conv->CurrentParamSize();
      CHECK_EQ(param_size, jni_conv->CurrentParamSize());
      __ Load(out_reg, in_off, param_size);
    }
  } else {
    CHECK(input_in_reg && !output_in_reg);
    ManagedRegister in_reg = mr_conv->CurrentParamRegister();
    FrameOffset out_off = jni_conv->CurrentParamStackOffset();
    // Check outgoing argument is within frame
    CHECK_LT(out_off.Uint32Value(), frame_size);
    if (ref_param) {
      // TODO: recycle value in in_reg rather than reload from SIRT
      __ CreateSirtEntry(out_off, sirt_offset, mr_conv->InterproceduralScratchRegister(),
                         null_allowed);
    } else {
      size_t param_size = mr_conv->CurrentParamSize();
      CHECK_EQ(param_size, jni_conv->CurrentParamSize());
      if (!mr_conv->IsCurrentParamOnStack()) {
        // regular non-straddling store
        __ Store(out_off, in_reg, param_size);
      } else {
        // store where input straddles registers and stack
        CHECK_EQ(param_size, 8u);
        FrameOffset in_off = mr_conv->CurrentParamStackOffset();
        __ StoreSpanning(out_off, in_reg, in_off, mr_conv->InterproceduralScratchRegister());
      }
    }
  }
}

static void SetNativeParameter(Assembler* jni_asm,
                               JniCallingConvention* jni_conv,
                               ManagedRegister in_reg) {
  if (jni_conv->IsCurrentParamOnStack()) {
    FrameOffset dest = jni_conv->CurrentParamStackOffset();
    __ StoreRawPtr(dest, in_reg);
  } else {
    if (!jni_conv->CurrentParamRegister().Equals(in_reg)) {
      __ Move(jni_conv->CurrentParamRegister(), in_reg, jni_conv->CurrentParamSize());
    }
  }
}

}  // namespace art

extern "C" art::CompiledMethod* ArtJniCompileMethod(art::Compiler& compiler,
                                                    uint32_t access_flags, uint32_t method_idx,
                                                    const art::DexFile& dex_file) {
  return ArtJniCompileMethodInternal(compiler, access_flags, method_idx, dex_file);
}
