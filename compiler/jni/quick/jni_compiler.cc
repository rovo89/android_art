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

#include <algorithm>
#include <memory>
#include <vector>
#include <fstream>

#include "base/logging.h"
#include "base/macros.h"
#include "calling_convention.h"
#include "class_linker.h"
#include "compiled_method.h"
#include "dex_file-inl.h"
#include "driver/compiler_driver.h"
#include "entrypoints/quick/quick_entrypoints.h"
#include "jni_internal.h"
#include "mirror/art_method.h"
#include "utils/assembler.h"
#include "utils/managed_register.h"
#include "utils/arm/managed_register_arm.h"
#include "utils/arm64/managed_register_arm64.h"
#include "utils/mips/managed_register_mips.h"
#include "utils/x86/managed_register_x86.h"
#include "thread.h"

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
CompiledMethod* ArtJniCompileMethodInternal(CompilerDriver* driver,
                                            uint32_t access_flags, uint32_t method_idx,
                                            const DexFile& dex_file) {
  const bool is_native = (access_flags & kAccNative) != 0;
  CHECK(is_native);
  const bool is_static = (access_flags & kAccStatic) != 0;
  const bool is_synchronized = (access_flags & kAccSynchronized) != 0;
  const char* shorty = dex_file.GetMethodShorty(dex_file.GetMethodId(method_idx));
  InstructionSet instruction_set = driver->GetInstructionSet();
  const bool is_64_bit_target = Is64BitInstructionSet(instruction_set);
  // Calling conventions used to iterate over parameters to method
  std::unique_ptr<JniCallingConvention> main_jni_conv(
      JniCallingConvention::Create(is_static, is_synchronized, shorty, instruction_set));
  bool reference_return = main_jni_conv->IsReturnAReference();

  std::unique_ptr<ManagedRuntimeCallingConvention> mr_conv(
      ManagedRuntimeCallingConvention::Create(is_static, is_synchronized, shorty, instruction_set));

  // Calling conventions to call into JNI method "end" possibly passing a returned reference, the
  //     method and the current thread.
  const char* jni_end_shorty;
  if (reference_return && is_synchronized) {
    jni_end_shorty = "ILL";
  } else if (reference_return) {
    jni_end_shorty = "IL";
  } else if (is_synchronized) {
    jni_end_shorty = "VL";
  } else {
    jni_end_shorty = "V";
  }

  std::unique_ptr<JniCallingConvention> end_jni_conv(
      JniCallingConvention::Create(is_static, is_synchronized, jni_end_shorty, instruction_set));

  // Assembler that holds generated instructions
  std::unique_ptr<Assembler> jni_asm(Assembler::Create(instruction_set));

  // Offsets into data structures
  // TODO: if cross compiling these offsets are for the host not the target
  const Offset functions(OFFSETOF_MEMBER(JNIEnvExt, functions));
  const Offset monitor_enter(OFFSETOF_MEMBER(JNINativeInterface, MonitorEnter));
  const Offset monitor_exit(OFFSETOF_MEMBER(JNINativeInterface, MonitorExit));

  // 1. Build the frame saving all callee saves
  const size_t frame_size(main_jni_conv->FrameSize());
  const std::vector<ManagedRegister>& callee_save_regs = main_jni_conv->CalleeSaveRegisters();
  __ BuildFrame(frame_size, mr_conv->MethodRegister(), callee_save_regs, mr_conv->EntrySpills());

  // 2. Set up the HandleScope
  mr_conv->ResetIterator(FrameOffset(frame_size));
  main_jni_conv->ResetIterator(FrameOffset(0));
  __ StoreImmediateToFrame(main_jni_conv->HandleScopeNumRefsOffset(),
                           main_jni_conv->ReferenceCount(),
                           mr_conv->InterproceduralScratchRegister());

  if (is_64_bit_target) {
    __ CopyRawPtrFromThread64(main_jni_conv->HandleScopeLinkOffset(),
                            Thread::TopHandleScopeOffset<8>(),
                            mr_conv->InterproceduralScratchRegister());
    __ StoreStackOffsetToThread64(Thread::TopHandleScopeOffset<8>(),
                                main_jni_conv->HandleScopeOffset(),
                                mr_conv->InterproceduralScratchRegister());
  } else {
    __ CopyRawPtrFromThread32(main_jni_conv->HandleScopeLinkOffset(),
                            Thread::TopHandleScopeOffset<4>(),
                            mr_conv->InterproceduralScratchRegister());
    __ StoreStackOffsetToThread32(Thread::TopHandleScopeOffset<4>(),
                                main_jni_conv->HandleScopeOffset(),
                                mr_conv->InterproceduralScratchRegister());
  }

  // 3. Place incoming reference arguments into handle scope
  main_jni_conv->Next();  // Skip JNIEnv*
  // 3.5. Create Class argument for static methods out of passed method
  if (is_static) {
    FrameOffset handle_scope_offset = main_jni_conv->CurrentParamHandleScopeEntryOffset();
    // Check handle scope offset is within frame
    CHECK_LT(handle_scope_offset.Uint32Value(), frame_size);
    __ LoadRef(main_jni_conv->InterproceduralScratchRegister(),
               mr_conv->MethodRegister(), mirror::ArtMethod::DeclaringClassOffset());
    __ VerifyObject(main_jni_conv->InterproceduralScratchRegister(), false);
    __ StoreRef(handle_scope_offset, main_jni_conv->InterproceduralScratchRegister());
    main_jni_conv->Next();  // in handle scope so move to next argument
  }
  while (mr_conv->HasNext()) {
    CHECK(main_jni_conv->HasNext());
    bool ref_param = main_jni_conv->IsCurrentParamAReference();
    CHECK(!ref_param || mr_conv->IsCurrentParamAReference());
    // References need placing in handle scope and the entry value passing
    if (ref_param) {
      // Compute handle scope entry, note null is placed in the handle scope but its boxed value
      // must be NULL
      FrameOffset handle_scope_offset = main_jni_conv->CurrentParamHandleScopeEntryOffset();
      // Check handle scope offset is within frame and doesn't run into the saved segment state
      CHECK_LT(handle_scope_offset.Uint32Value(), frame_size);
      CHECK_NE(handle_scope_offset.Uint32Value(),
               main_jni_conv->SavedLocalReferenceCookieOffset().Uint32Value());
      bool input_in_reg = mr_conv->IsCurrentParamInRegister();
      bool input_on_stack = mr_conv->IsCurrentParamOnStack();
      CHECK(input_in_reg || input_on_stack);

      if (input_in_reg) {
        ManagedRegister in_reg  =  mr_conv->CurrentParamRegister();
        __ VerifyObject(in_reg, mr_conv->IsCurrentArgPossiblyNull());
        __ StoreRef(handle_scope_offset, in_reg);
      } else if (input_on_stack) {
        FrameOffset in_off  = mr_conv->CurrentParamStackOffset();
        __ VerifyObject(in_off, mr_conv->IsCurrentArgPossiblyNull());
        __ CopyRef(handle_scope_offset, in_off,
                   mr_conv->InterproceduralScratchRegister());
      }
    }
    mr_conv->Next();
    main_jni_conv->Next();
  }

  // 4. Write out the end of the quick frames.
  if (is_64_bit_target) {
    __ StoreStackPointerToThread64(Thread::TopOfManagedStackOffset<8>());
    __ StoreImmediateToThread64(Thread::TopOfManagedStackPcOffset<8>(), 0,
                              mr_conv->InterproceduralScratchRegister());
  } else {
    __ StoreStackPointerToThread32(Thread::TopOfManagedStackOffset<4>());
    __ StoreImmediateToThread32(Thread::TopOfManagedStackPcOffset<4>(), 0,
                              mr_conv->InterproceduralScratchRegister());
  }

  // 5. Move frame down to allow space for out going args.
  const size_t main_out_arg_size = main_jni_conv->OutArgSize();
  size_t current_out_arg_size = main_out_arg_size;
  __ IncreaseFrameSize(main_out_arg_size);

  // 6. Call into appropriate JniMethodStart passing Thread* so that transition out of Runnable
  //    can occur. The result is the saved JNI local state that is restored by the exit call. We
  //    abuse the JNI calling convention here, that is guaranteed to support passing 2 pointer
  //    arguments.
  ThreadOffset<4> jni_start32 = is_synchronized ? QUICK_ENTRYPOINT_OFFSET(4, pJniMethodStartSynchronized)
                                                : QUICK_ENTRYPOINT_OFFSET(4, pJniMethodStart);
  ThreadOffset<8> jni_start64 = is_synchronized ? QUICK_ENTRYPOINT_OFFSET(8, pJniMethodStartSynchronized)
                                                : QUICK_ENTRYPOINT_OFFSET(8, pJniMethodStart);
  main_jni_conv->ResetIterator(FrameOffset(main_out_arg_size));
  FrameOffset locked_object_handle_scope_offset(0);
  if (is_synchronized) {
    // Pass object for locking.
    main_jni_conv->Next();  // Skip JNIEnv.
    locked_object_handle_scope_offset = main_jni_conv->CurrentParamHandleScopeEntryOffset();
    main_jni_conv->ResetIterator(FrameOffset(main_out_arg_size));
    if (main_jni_conv->IsCurrentParamOnStack()) {
      FrameOffset out_off = main_jni_conv->CurrentParamStackOffset();
      __ CreateHandleScopeEntry(out_off, locked_object_handle_scope_offset,
                         mr_conv->InterproceduralScratchRegister(),
                         false);
    } else {
      ManagedRegister out_reg = main_jni_conv->CurrentParamRegister();
      __ CreateHandleScopeEntry(out_reg, locked_object_handle_scope_offset,
                         ManagedRegister::NoRegister(), false);
    }
    main_jni_conv->Next();
  }
  if (main_jni_conv->IsCurrentParamInRegister()) {
    __ GetCurrentThread(main_jni_conv->CurrentParamRegister());
    if (is_64_bit_target) {
      __ Call(main_jni_conv->CurrentParamRegister(), Offset(jni_start64),
             main_jni_conv->InterproceduralScratchRegister());
    } else {
      __ Call(main_jni_conv->CurrentParamRegister(), Offset(jni_start32),
             main_jni_conv->InterproceduralScratchRegister());
    }
  } else {
    __ GetCurrentThread(main_jni_conv->CurrentParamStackOffset(),
                        main_jni_conv->InterproceduralScratchRegister());
    if (is_64_bit_target) {
      __ CallFromThread64(jni_start64, main_jni_conv->InterproceduralScratchRegister());
    } else {
      __ CallFromThread32(jni_start32, main_jni_conv->InterproceduralScratchRegister());
    }
  }
  if (is_synchronized) {  // Check for exceptions from monitor enter.
    __ ExceptionPoll(main_jni_conv->InterproceduralScratchRegister(), main_out_arg_size);
  }
  FrameOffset saved_cookie_offset = main_jni_conv->SavedLocalReferenceCookieOffset();
  __ Store(saved_cookie_offset, main_jni_conv->IntReturnRegister(), 4);

  // 7. Iterate over arguments placing values from managed calling convention in
  //    to the convention required for a native call (shuffling). For references
  //    place an index/pointer to the reference after checking whether it is
  //    NULL (which must be encoded as NULL).
  //    Note: we do this prior to materializing the JNIEnv* and static's jclass to
  //    give as many free registers for the shuffle as possible
  mr_conv->ResetIterator(FrameOffset(frame_size + main_out_arg_size));
  uint32_t args_count = 0;
  while (mr_conv->HasNext()) {
    args_count++;
    mr_conv->Next();
  }

  // Do a backward pass over arguments, so that the generated code will be "mov
  // R2, R3; mov R1, R2" instead of "mov R1, R2; mov R2, R3."
  // TODO: A reverse iterator to improve readability.
  for (uint32_t i = 0; i < args_count; ++i) {
    mr_conv->ResetIterator(FrameOffset(frame_size + main_out_arg_size));
    main_jni_conv->ResetIterator(FrameOffset(main_out_arg_size));
    main_jni_conv->Next();  // Skip JNIEnv*.
    if (is_static) {
      main_jni_conv->Next();  // Skip Class for now.
    }
    // Skip to the argument we're interested in.
    for (uint32_t j = 0; j < args_count - i - 1; ++j) {
      mr_conv->Next();
      main_jni_conv->Next();
    }
    CopyParameter(jni_asm.get(), mr_conv.get(), main_jni_conv.get(), frame_size, main_out_arg_size);
  }
  if (is_static) {
    // Create argument for Class
    mr_conv->ResetIterator(FrameOffset(frame_size + main_out_arg_size));
    main_jni_conv->ResetIterator(FrameOffset(main_out_arg_size));
    main_jni_conv->Next();  // Skip JNIEnv*
    FrameOffset handle_scope_offset = main_jni_conv->CurrentParamHandleScopeEntryOffset();
    if (main_jni_conv->IsCurrentParamOnStack()) {
      FrameOffset out_off = main_jni_conv->CurrentParamStackOffset();
      __ CreateHandleScopeEntry(out_off, handle_scope_offset,
                         mr_conv->InterproceduralScratchRegister(),
                         false);
    } else {
      ManagedRegister out_reg = main_jni_conv->CurrentParamRegister();
      __ CreateHandleScopeEntry(out_reg, handle_scope_offset,
                         ManagedRegister::NoRegister(), false);
    }
  }

  // 8. Create 1st argument, the JNI environment ptr.
  main_jni_conv->ResetIterator(FrameOffset(main_out_arg_size));
  // Register that will hold local indirect reference table
  if (main_jni_conv->IsCurrentParamInRegister()) {
    ManagedRegister jni_env = main_jni_conv->CurrentParamRegister();
    DCHECK(!jni_env.Equals(main_jni_conv->InterproceduralScratchRegister()));
    if (is_64_bit_target) {
      __ LoadRawPtrFromThread64(jni_env, Thread::JniEnvOffset<8>());
    } else {
      __ LoadRawPtrFromThread32(jni_env, Thread::JniEnvOffset<4>());
    }
  } else {
    FrameOffset jni_env = main_jni_conv->CurrentParamStackOffset();
    if (is_64_bit_target) {
      __ CopyRawPtrFromThread64(jni_env, Thread::JniEnvOffset<8>(),
                            main_jni_conv->InterproceduralScratchRegister());
    } else {
      __ CopyRawPtrFromThread32(jni_env, Thread::JniEnvOffset<4>(),
                            main_jni_conv->InterproceduralScratchRegister());
    }
  }

  // 9. Plant call to native code associated with method.
  __ Call(main_jni_conv->MethodStackOffset(), mirror::ArtMethod::NativeMethodOffset(),
          mr_conv->InterproceduralScratchRegister());

  // 10. Fix differences in result widths.
  if (main_jni_conv->RequiresSmallResultTypeExtension()) {
    if (main_jni_conv->GetReturnType() == Primitive::kPrimByte ||
        main_jni_conv->GetReturnType() == Primitive::kPrimShort) {
      __ SignExtend(main_jni_conv->ReturnRegister(),
                    Primitive::ComponentSize(main_jni_conv->GetReturnType()));
    } else if (main_jni_conv->GetReturnType() == Primitive::kPrimBoolean ||
               main_jni_conv->GetReturnType() == Primitive::kPrimChar) {
      __ ZeroExtend(main_jni_conv->ReturnRegister(),
                    Primitive::ComponentSize(main_jni_conv->GetReturnType()));
    }
  }

  // 11. Save return value
  FrameOffset return_save_location = main_jni_conv->ReturnValueSaveLocation();
  if (main_jni_conv->SizeOfReturnValue() != 0 && !reference_return) {
    if (instruction_set == kMips && main_jni_conv->GetReturnType() == Primitive::kPrimDouble &&
        return_save_location.Uint32Value() % 8 != 0) {
      // Ensure doubles are 8-byte aligned for MIPS
      return_save_location = FrameOffset(return_save_location.Uint32Value() + kMipsPointerSize);
    }
    CHECK_LT(return_save_location.Uint32Value(), frame_size + main_out_arg_size);
    __ Store(return_save_location, main_jni_conv->ReturnRegister(), main_jni_conv->SizeOfReturnValue());
  }

  // Increase frame size for out args if needed by the end_jni_conv.
  const size_t end_out_arg_size = end_jni_conv->OutArgSize();
  if (end_out_arg_size > current_out_arg_size) {
    size_t out_arg_size_diff = end_out_arg_size - current_out_arg_size;
    current_out_arg_size = end_out_arg_size;
    __ IncreaseFrameSize(out_arg_size_diff);
    saved_cookie_offset = FrameOffset(saved_cookie_offset.SizeValue() + out_arg_size_diff);
    locked_object_handle_scope_offset =
        FrameOffset(locked_object_handle_scope_offset.SizeValue() + out_arg_size_diff);
    return_save_location = FrameOffset(return_save_location.SizeValue() + out_arg_size_diff);
  }
  //     thread.
  end_jni_conv->ResetIterator(FrameOffset(end_out_arg_size));
  ThreadOffset<4> jni_end32(-1);
  ThreadOffset<8> jni_end64(-1);
  if (reference_return) {
    // Pass result.
    jni_end32 = is_synchronized ? QUICK_ENTRYPOINT_OFFSET(4, pJniMethodEndWithReferenceSynchronized)
                                : QUICK_ENTRYPOINT_OFFSET(4, pJniMethodEndWithReference);
    jni_end64 = is_synchronized ? QUICK_ENTRYPOINT_OFFSET(8, pJniMethodEndWithReferenceSynchronized)
                                : QUICK_ENTRYPOINT_OFFSET(8, pJniMethodEndWithReference);
    SetNativeParameter(jni_asm.get(), end_jni_conv.get(), end_jni_conv->ReturnRegister());
    end_jni_conv->Next();
  } else {
    jni_end32 = is_synchronized ? QUICK_ENTRYPOINT_OFFSET(4, pJniMethodEndSynchronized)
                                : QUICK_ENTRYPOINT_OFFSET(4, pJniMethodEnd);
    jni_end64 = is_synchronized ? QUICK_ENTRYPOINT_OFFSET(8, pJniMethodEndSynchronized)
                                : QUICK_ENTRYPOINT_OFFSET(8, pJniMethodEnd);
  }
  // Pass saved local reference state.
  if (end_jni_conv->IsCurrentParamOnStack()) {
    FrameOffset out_off = end_jni_conv->CurrentParamStackOffset();
    __ Copy(out_off, saved_cookie_offset, end_jni_conv->InterproceduralScratchRegister(), 4);
  } else {
    ManagedRegister out_reg = end_jni_conv->CurrentParamRegister();
    __ Load(out_reg, saved_cookie_offset, 4);
  }
  end_jni_conv->Next();
  if (is_synchronized) {
    // Pass object for unlocking.
    if (end_jni_conv->IsCurrentParamOnStack()) {
      FrameOffset out_off = end_jni_conv->CurrentParamStackOffset();
      __ CreateHandleScopeEntry(out_off, locked_object_handle_scope_offset,
                         end_jni_conv->InterproceduralScratchRegister(),
                         false);
    } else {
      ManagedRegister out_reg = end_jni_conv->CurrentParamRegister();
      __ CreateHandleScopeEntry(out_reg, locked_object_handle_scope_offset,
                         ManagedRegister::NoRegister(), false);
    }
    end_jni_conv->Next();
  }
  if (end_jni_conv->IsCurrentParamInRegister()) {
    __ GetCurrentThread(end_jni_conv->CurrentParamRegister());
    if (is_64_bit_target) {
      __ Call(end_jni_conv->CurrentParamRegister(), Offset(jni_end64),
              end_jni_conv->InterproceduralScratchRegister());
    } else {
      __ Call(end_jni_conv->CurrentParamRegister(), Offset(jni_end32),
              end_jni_conv->InterproceduralScratchRegister());
    }
  } else {
    __ GetCurrentThread(end_jni_conv->CurrentParamStackOffset(),
                        end_jni_conv->InterproceduralScratchRegister());
    if (is_64_bit_target) {
      __ CallFromThread64(ThreadOffset<8>(jni_end64), end_jni_conv->InterproceduralScratchRegister());
    } else {
      __ CallFromThread32(ThreadOffset<4>(jni_end32), end_jni_conv->InterproceduralScratchRegister());
    }
  }

  // 13. Reload return value
  if (main_jni_conv->SizeOfReturnValue() != 0 && !reference_return) {
    __ Load(mr_conv->ReturnRegister(), return_save_location, mr_conv->SizeOfReturnValue());
  }

  // 14. Move frame up now we're done with the out arg space.
  __ DecreaseFrameSize(current_out_arg_size);

  // 15. Process pending exceptions from JNI call or monitor exit.
  __ ExceptionPoll(main_jni_conv->InterproceduralScratchRegister(), 0);

  // 16. Remove activation - need to restore callee save registers since the GC may have changed
  //     them.
  __ RemoveFrame(frame_size, callee_save_regs);

  // 17. Finalize code generation
  __ EmitSlowPaths();
  size_t cs = __ CodeSize();
  if (instruction_set == kArm64) {
    // Test that we do not exceed the buffer size.
    CHECK(cs < arm64::kBufferSizeArm64);
  }
  std::vector<uint8_t> managed_code(cs);
  MemoryRegion code(&managed_code[0], managed_code.size());
  __ FinalizeInstructions(code);
  return new CompiledMethod(driver,
                            instruction_set,
                            managed_code,
                            frame_size,
                            main_jni_conv->CoreSpillMask(),
                            main_jni_conv->FpSpillMask());
}

// Copy a single parameter from the managed to the JNI calling convention
static void CopyParameter(Assembler* jni_asm,
                          ManagedRuntimeCallingConvention* mr_conv,
                          JniCallingConvention* jni_conv,
                          size_t frame_size, size_t out_arg_size) {
  bool input_in_reg = mr_conv->IsCurrentParamInRegister();
  bool output_in_reg = jni_conv->IsCurrentParamInRegister();
  FrameOffset handle_scope_offset(0);
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
  // References need placing in handle scope and the entry address passing
  if (ref_param) {
    null_allowed = mr_conv->IsCurrentArgPossiblyNull();
    // Compute handle scope offset. Note null is placed in the handle scope but the jobject
    // passed to the native code must be null (not a pointer into the handle scope
    // as with regular references).
    handle_scope_offset = jni_conv->CurrentParamHandleScopeEntryOffset();
    // Check handle scope offset is within frame.
    CHECK_LT(handle_scope_offset.Uint32Value(), (frame_size + out_arg_size));
  }
  if (input_in_reg && output_in_reg) {
    ManagedRegister in_reg = mr_conv->CurrentParamRegister();
    ManagedRegister out_reg = jni_conv->CurrentParamRegister();
    if (ref_param) {
      __ CreateHandleScopeEntry(out_reg, handle_scope_offset, in_reg, null_allowed);
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
      __ CreateHandleScopeEntry(out_off, handle_scope_offset, mr_conv->InterproceduralScratchRegister(),
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
      __ CreateHandleScopeEntry(out_reg, handle_scope_offset, ManagedRegister::NoRegister(), null_allowed);
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
      // TODO: recycle value in in_reg rather than reload from handle scope
      __ CreateHandleScopeEntry(out_off, handle_scope_offset, mr_conv->InterproceduralScratchRegister(),
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

extern "C" art::CompiledMethod* ArtQuickJniCompileMethod(art::CompilerDriver* compiler,
                                                         uint32_t access_flags, uint32_t method_idx,
                                                         const art::DexFile& dex_file) {
  return ArtJniCompileMethodInternal(compiler, access_flags, method_idx, dex_file);
}
