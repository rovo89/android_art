// Copyright 2011 Google Inc. All Rights Reserved.

#include "jni_compiler.h"

#include <sys/mman.h>
#include <vector>

#include "assembler.h"
#include "calling_convention.h"
#include "constants.h"
#include "jni_internal.h"
#include "macros.h"
#include "managed_register.h"
#include "logging.h"
#include "thread.h"
#include "UniquePtr.h"

namespace art {

namespace arm {
ByteArray* CreateJniStub();
}

namespace x86 {
ByteArray* CreateJniStub();
}

ByteArray* JniCompiler::CreateJniStub(InstructionSet instruction_set) {
  switch (instruction_set) {
    case kArm:
    case kThumb2:
      return arm::CreateJniStub();
    case kX86:
      return x86::CreateJniStub();
    default:
      LOG(FATAL) << "Unknown InstructionSet " << (int) instruction_set;
      return NULL;
  }
}

JniCompiler::JniCompiler(InstructionSet insns) {
  if (insns == kThumb2) {
    // currently only ARM code generation is supported
    instruction_set_ = kArm;
  } else {
    instruction_set_ = insns;
  }
}

JniCompiler::~JniCompiler() {}

// Generate the JNI bridge for the given method, general contract:
// - Arguments are in the managed runtime format, either on stack or in
//   registers, a reference to the method object is supplied as part of this
//   convention.
//
void JniCompiler::Compile(Method* native_method) {
  CHECK(native_method->IsNative());

  // Calling conventions used to iterate over parameters to method
  UniquePtr<JniCallingConvention> jni_conv(
      JniCallingConvention::Create(native_method, instruction_set_));
  UniquePtr<ManagedRuntimeCallingConvention> mr_conv(
      ManagedRuntimeCallingConvention::Create(native_method, instruction_set_));

  // Assembler that holds generated instructions
  UniquePtr<Assembler> jni_asm(Assembler::Create(instruction_set_));
#define __ jni_asm->

  // Offsets into data structures
  // TODO: if cross compiling these offsets are for the host not the target
  const Offset functions(OFFSETOF_MEMBER(JNIEnvExt, functions));
  const Offset monitor_enter(OFFSETOF_MEMBER(JNINativeInterface, MonitorEnter));
  const Offset monitor_exit(OFFSETOF_MEMBER(JNINativeInterface, MonitorExit));

  // Cache of IsStatic as we call it often enough
  const bool is_static = native_method->IsStatic();

  // 1. Build the frame saving all callee saves
  const size_t frame_size(jni_conv->FrameSize());
  const std::vector<ManagedRegister>& callee_save_regs = jni_conv->CalleeSaveRegisters();
  __ BuildFrame(frame_size, mr_conv->MethodRegister(), callee_save_regs);

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
      // Check SIRT offset is within frame
      CHECK_LT(sirt_offset.Uint32Value(), frame_size);
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

  // 4. Transition from being in managed to native code. Save the top_of_managed_stack_
  // so that the managed stack can be crawled while in native code. Clear the corresponding
  // PC value that has no meaning for the this frame.
  // TODO: ensure the transition to native follow a store fence.
  __ StoreStackPointerToThread(Thread::TopOfManagedStackOffset());
  __ StoreImmediateToThread(Thread::TopOfManagedStackPcOffset(), 0,
                            mr_conv->InterproceduralScratchRegister());
  __ StoreImmediateToThread(Thread::StateOffset(), Thread::kNative,
                            mr_conv->InterproceduralScratchRegister());

  // 5. Move frame down to allow space for out going args. Do for as short a
  //    time as possible to aid profiling..
  const size_t out_arg_size = jni_conv->OutArgSize();
  __ IncreaseFrameSize(out_arg_size);

  // 6. Acquire lock for synchronized methods.
  if (native_method->IsSynchronized()) {
    // Compute arguments in registers to preserve
    mr_conv->ResetIterator(FrameOffset(frame_size + out_arg_size));
    std::vector<ManagedRegister> live_argument_regs;
    while (mr_conv->HasNext()) {
      if (mr_conv->IsCurrentParamInRegister()) {
        live_argument_regs.push_back(mr_conv->CurrentParamRegister());
      }
      mr_conv->Next();
    }

    // Copy arguments to preserve to callee save registers
    CHECK_LE(live_argument_regs.size(), callee_save_regs.size());
    for (size_t i = 0; i < live_argument_regs.size(); i++) {
      __ Move(callee_save_regs.at(i), live_argument_regs.at(i));
    }

    // Get SIRT entry for 1st argument (jclass or this) to be 1st argument to
    // monitor enter
    mr_conv->ResetIterator(FrameOffset(frame_size + out_arg_size));
    jni_conv->ResetIterator(FrameOffset(out_arg_size));
    jni_conv->Next();  // Skip JNIEnv*
    if (is_static) {
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
    } else {
      CopyParameter(jni_asm.get(), mr_conv.get(), jni_conv.get(), frame_size,
                    out_arg_size);
    }

    // Generate JNIEnv* in place and leave a copy in jni_fns_register
    jni_conv->ResetIterator(FrameOffset(out_arg_size));
    ManagedRegister jni_fns_register =
        jni_conv->InterproceduralScratchRegister();
    __ LoadRawPtrFromThread(jni_fns_register, Thread::JniEnvOffset());
    SetNativeParameter(jni_asm.get(), jni_conv.get(), jni_fns_register);

    // Call JNIEnv->MonitorEnter(object)
    __ LoadRawPtr(jni_fns_register, jni_fns_register, functions);
    __ Call(jni_fns_register, monitor_enter,
                  jni_conv->InterproceduralScratchRegister());

    // Check for exceptions
    __ ExceptionPoll(jni_conv->InterproceduralScratchRegister());

    // Restore live arguments
    for (size_t i = 0; i < live_argument_regs.size(); i++) {
      __ Move(live_argument_regs.at(i), callee_save_regs.at(i));
    }
  }

  // 7. Iterate over arguments placing values from managed calling convention in
  //    to the convention required for a native call (shuffling). For references
  //    place an index/pointer to the reference after checking whether it is
  //    NULL (which must be encoded as NULL).
  //    NB. we do this prior to materializing the JNIEnv* and static's jclass to
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
    jni_conv->Next();  // Skip JNIEnv*
    if (is_static) {
      jni_conv->Next();  // Skip Class for now
    }
    for (uint32_t j = 0; j < args_count - i - 1; ++j) {
      mr_conv->Next();
      jni_conv->Next();
    }
    CopyParameter(jni_asm.get(), mr_conv.get(), jni_conv.get(), frame_size,
                  out_arg_size);
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
  // 8. Create 1st argument, the JNI environment ptr
  jni_conv->ResetIterator(FrameOffset(out_arg_size));
  if (jni_conv->IsCurrentParamInRegister()) {
    __ LoadRawPtrFromThread(jni_conv->CurrentParamRegister(),
                            Thread::JniEnvOffset());
  } else {
    __ CopyRawPtrFromThread(jni_conv->CurrentParamStackOffset(),
                            Thread::JniEnvOffset(),
                            jni_conv->InterproceduralScratchRegister());
  }

  // 9. Plant call to native code associated with method
  if (!jni_conv->IsMethodRegisterCrushedPreCall()) {
    // Method register shouldn't have been crushed by setting up outgoing
    // arguments
    __ Call(mr_conv->MethodRegister(), Method::NativeMethodOffset(),
            mr_conv->InterproceduralScratchRegister());
  } else {
    __ Call(jni_conv->MethodStackOffset(), Method::NativeMethodOffset(),
            mr_conv->InterproceduralScratchRegister());
  }

  // 10. Release lock for synchronized methods.
  if (native_method->IsSynchronized()) {
    mr_conv->ResetIterator(FrameOffset(frame_size+out_arg_size));
    jni_conv->ResetIterator(FrameOffset(out_arg_size));
    jni_conv->Next();  // Skip JNIEnv*
    // Save return value
    FrameOffset return_save_location = jni_conv->ReturnValueSaveLocation();
    if (jni_conv->SizeOfReturnValue() != 0) {
      FrameOffset return_save_location = jni_conv->ReturnValueSaveLocation();
      CHECK_LT(return_save_location.Uint32Value(), frame_size+out_arg_size);
      __ Store(return_save_location, jni_conv->ReturnRegister(),
               jni_conv->SizeOfReturnValue());
    }
    // Get SIRT entry for 1st argument
    if (is_static) {
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
    } else {
      CopyParameter(jni_asm.get(), mr_conv.get(), jni_conv.get(), frame_size,
                    out_arg_size);
    }
    // Generate JNIEnv* in place and leave a copy in jni_env_register
    jni_conv->ResetIterator(FrameOffset(out_arg_size));
    ManagedRegister jni_env_register =
        jni_conv->InterproceduralScratchRegister();
    __ LoadRawPtrFromThread(jni_env_register, Thread::JniEnvOffset());
    SetNativeParameter(jni_asm.get(), jni_conv.get(), jni_env_register);
    // Call JNIEnv->MonitorExit(object)
    __ LoadRawPtr(jni_env_register, jni_env_register, functions);
    __ Call(jni_env_register, monitor_exit,
            jni_conv->InterproceduralScratchRegister());
    // Reload return value
    if (jni_conv->SizeOfReturnValue() != 0) {
      __ Load(jni_conv->ReturnRegister(), return_save_location,
              jni_conv->SizeOfReturnValue());
    }
  }

  // 11. Release outgoing argument area
  __ DecreaseFrameSize(out_arg_size);
  mr_conv->ResetIterator(FrameOffset(frame_size));
  jni_conv->ResetIterator(FrameOffset(0));

  // 12. Transition from being in native to managed code, possibly entering a
  //     safepoint
  CHECK(!jni_conv->InterproceduralScratchRegister()
        .Equals(jni_conv->ReturnRegister()));  // don't clobber result
  // Location to preserve result on slow path, ensuring its within the frame
  FrameOffset return_save_location = jni_conv->ReturnValueSaveLocation();
  CHECK(return_save_location.Uint32Value() < frame_size ||
        jni_conv->SizeOfReturnValue() == 0);
  __ SuspendPoll(jni_conv->InterproceduralScratchRegister(),
                 jni_conv->ReturnRegister(), return_save_location,
                 jni_conv->SizeOfReturnValue());
  __ ExceptionPoll(jni_conv->InterproceduralScratchRegister());
  __ StoreImmediateToThread(Thread::StateOffset(), Thread::kRunnable,
                            jni_conv->InterproceduralScratchRegister());


  // 13. Place result in correct register possibly loading from indirect
  //     reference table
  if (jni_conv->IsReturnAReference()) {
    __ IncreaseFrameSize(out_arg_size);
    jni_conv->ResetIterator(FrameOffset(out_arg_size));

    jni_conv->Next();  // Skip Thread* argument
    // Pass result as arg2
    SetNativeParameter(jni_asm.get(), jni_conv.get(),
                       jni_conv->ReturnRegister());

    // Pass Thread*
    jni_conv->ResetIterator(FrameOffset(out_arg_size));
    if (jni_conv->IsCurrentParamInRegister()) {
      __ GetCurrentThread(jni_conv->CurrentParamRegister());
      __ Call(jni_conv->CurrentParamRegister(),
              Offset(OFFSETOF_MEMBER(Thread, pDecodeJObjectInThread)),
              jni_conv->InterproceduralScratchRegister());
    } else {
      __ GetCurrentThread(jni_conv->CurrentParamStackOffset(),
                          jni_conv->InterproceduralScratchRegister());
      __ Call(ThreadOffset(OFFSETOF_MEMBER(Thread, pDecodeJObjectInThread)),
              jni_conv->InterproceduralScratchRegister());
    }

    __ DecreaseFrameSize(out_arg_size);
    jni_conv->ResetIterator(FrameOffset(0));
  }
  __ Move(mr_conv->ReturnRegister(), jni_conv->ReturnRegister());

  // 14. Remove SIRT from thread
  __ CopyRawPtrToThread(Thread::TopSirtOffset(), jni_conv->SirtLinkOffset(),
                        jni_conv->InterproceduralScratchRegister());

  // 15. Remove activation
  if (native_method->IsSynchronized()) {
    __ RemoveFrame(frame_size, callee_save_regs);
  } else {
    __ RemoveFrame(frame_size, std::vector<ManagedRegister>());
  }

  // 16. Finalize code generation
  __ EmitSlowPaths();
  size_t cs = __ CodeSize();
  ByteArray* managed_code = ByteArray::Alloc(cs);
  CHECK(managed_code != NULL);
  MemoryRegion code(managed_code->GetData(), managed_code->GetLength());
  __ FinalizeInstructions(code);
  native_method->SetCode(managed_code, instruction_set_);
  native_method->SetFrameSizeInBytes(frame_size);
  native_method->SetReturnPcOffsetInBytes(jni_conv->ReturnPcOffset());
  native_method->SetCoreSpillMask(jni_conv->CoreSpillMask());
  native_method->SetFpSpillMask(jni_conv->FpSpillMask());
#undef __
}

void JniCompiler::SetNativeParameter(Assembler* jni_asm,
                                     JniCallingConvention* jni_conv,
                                     ManagedRegister in_reg) {
#define __ jni_asm->
  if (jni_conv->IsCurrentParamOnStack()) {
    FrameOffset dest = jni_conv->CurrentParamStackOffset();
    __ StoreRawPtr(dest, in_reg);
  } else {
    if (!jni_conv->CurrentParamRegister().Equals(in_reg)) {
      __ Move(jni_conv->CurrentParamRegister(), in_reg);
    }
  }
#undef __
}

// Copy a single parameter from the managed to the JNI calling convention
void JniCompiler::CopyParameter(Assembler* jni_asm,
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
    CHECK_LT(sirt_offset.Uint32Value(), (frame_size+out_arg_size));
  }
#define __ jni_asm->
  if (input_in_reg && output_in_reg) {
    ManagedRegister in_reg = mr_conv->CurrentParamRegister();
    ManagedRegister out_reg = jni_conv->CurrentParamRegister();
    if (ref_param) {
      __ CreateSirtEntry(out_reg, sirt_offset, in_reg, null_allowed);
    } else {
      if (!mr_conv->IsCurrentParamOnStack()) {
        // regular non-straddling move
        __ Move(out_reg, in_reg);
      } else {
        UNIMPLEMENTED(FATAL);  // we currently don't expect to see this case
      }
    }
  } else if (!input_in_reg && !output_in_reg) {
    FrameOffset out_off = jni_conv->CurrentParamStackOffset();
    if (ref_param) {
      __ CreateSirtEntry(out_off, sirt_offset,
                         mr_conv->InterproceduralScratchRegister(),
                         null_allowed);
    } else {
      FrameOffset in_off = mr_conv->CurrentParamStackOffset();
      size_t param_size = mr_conv->CurrentParamSize();
      CHECK_EQ(param_size, jni_conv->CurrentParamSize());
      __ Copy(out_off, in_off, mr_conv->InterproceduralScratchRegister(),
              param_size);
    }
  } else if (!input_in_reg && output_in_reg) {
    FrameOffset in_off = mr_conv->CurrentParamStackOffset();
    ManagedRegister out_reg = jni_conv->CurrentParamRegister();
    // Check that incoming stack arguments are above the current stack frame.
    CHECK_GT(in_off.Uint32Value(), frame_size);
    if (ref_param) {
      __ CreateSirtEntry(out_reg, sirt_offset,
                         ManagedRegister::NoRegister(), null_allowed);
    } else {
      unsigned int param_size = mr_conv->CurrentParamSize();
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
      __ CreateSirtEntry(out_off, sirt_offset,
                         mr_conv->InterproceduralScratchRegister(),
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
        __ StoreSpanning(out_off, in_reg, in_off,
                         mr_conv->InterproceduralScratchRegister());
      }
    }
  }
#undef __
}

}  // namespace art
