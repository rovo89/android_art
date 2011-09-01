// Copyright 2011 Google Inc. All Rights Reserved.
// Author: irogers@google.com (Ian Rogers)

#include "jni_compiler.h"

#include <sys/mman.h>
#include <vector>

#include "assembler.h"
#include "calling_convention.h"
#include "jni_internal.h"
#include "macros.h"
#include "managed_register.h"
#include "logging.h"
#include "thread.h"

namespace art {

static Object* DecodeJObjectInThread(Thread* thread, jobject obj) {
  return thread->DecodeJObject(obj);
}

// Generate the JNI bridge for the given method, general contract:
// - Arguments are in the managed runtime format, either on stack or in
//   registers, a reference to the method object is supplied as part of this
//   convention.
//
void JniCompiler::Compile(Assembler* jni_asm, Method* native_method) {
  CHECK(native_method->IsNative());
  JniCallingConvention jni_conv(native_method);
  ManagedRuntimeCallingConvention mr_conv(native_method);
  const bool is_static = native_method->IsStatic();
  static Offset functions(OFFSETOF_MEMBER(JNIEnvExt, functions));
  static Offset monitor_enter(OFFSETOF_MEMBER(JNINativeInterface, MonitorEnter));
  static Offset monitor_exit(OFFSETOF_MEMBER(JNINativeInterface, MonitorExit));

  // 1. Build the frame
  const size_t frame_size(jni_conv.FrameSize());
  const std::vector<ManagedRegister>& spill_regs = jni_conv.RegsToSpillPreCall();
  jni_asm->BuildFrame(frame_size, mr_conv.MethodRegister(), spill_regs);

  // 2. Save callee save registers that aren't callee save in the native code
  // TODO: implement computing the difference of the callee saves
  // and saving

  // 3. Set up the StackIndirectReferenceTable
  mr_conv.ResetIterator(FrameOffset(frame_size));
  jni_conv.ResetIterator(FrameOffset(0));
  jni_asm->StoreImmediateToFrame(jni_conv.SirtNumRefsOffset(),
                                 jni_conv.ReferenceCount(),
                                 mr_conv.InterproceduralScratchRegister());
  jni_asm->CopyRawPtrFromThread(jni_conv.SirtLinkOffset(),
                                Thread::TopSirtOffset(),
                                mr_conv.InterproceduralScratchRegister());
  jni_asm->StoreStackOffsetToThread(Thread::TopSirtOffset(),
                                    jni_conv.SirtOffset(),
                                    mr_conv.InterproceduralScratchRegister());

  // 4. Place incoming reference arguments into SIRT
  jni_conv.Next();  // Skip JNIEnv*
  // 4.5. Create Class argument for static methods out of passed method
  if (is_static) {
    FrameOffset sirt_offset = jni_conv.CurrentParamSirtEntryOffset();
    // Check sirt offset is within frame
    CHECK_LT(sirt_offset.Uint32Value(), frame_size);
    jni_asm->LoadRef(jni_conv.InterproceduralScratchRegister(),
                     mr_conv.MethodRegister(), Method::DeclaringClassOffset());
    jni_asm->VerifyObject(jni_conv.InterproceduralScratchRegister(), false);
    jni_asm->StoreRef(sirt_offset, jni_conv.InterproceduralScratchRegister());
    jni_conv.Next();  // in SIRT so move to next argument
  }
  while (mr_conv.HasNext()) {
    CHECK(jni_conv.HasNext());
    bool ref_param = jni_conv.IsCurrentParamAReference();
    CHECK(!ref_param || mr_conv.IsCurrentParamAReference());
    // References need placing in SIRT and the entry value passing
    if (ref_param) {
      // Compute SIRT entry, note null is placed in the SIRT but its boxed value
      // must be NULL
      FrameOffset sirt_offset = jni_conv.CurrentParamSirtEntryOffset();
      // Check SIRT offset is within frame
      CHECK_LT(sirt_offset.Uint32Value(), frame_size);
      bool input_in_reg = mr_conv.IsCurrentParamInRegister();
      bool input_on_stack = mr_conv.IsCurrentParamOnStack();
      CHECK(input_in_reg || input_on_stack);

      if (input_in_reg) {
        ManagedRegister in_reg  =  mr_conv.CurrentParamRegister();
        jni_asm->VerifyObject(in_reg, mr_conv.IsCurrentUserArg());
        jni_asm->StoreRef(sirt_offset, in_reg);
      } else if (input_on_stack) {
        FrameOffset in_off  = mr_conv.CurrentParamStackOffset();
        jni_asm->VerifyObject(in_off, mr_conv.IsCurrentUserArg());
        jni_asm->CopyRef(sirt_offset, in_off,
                         mr_conv.InterproceduralScratchRegister());
      }
    }
    mr_conv.Next();
    jni_conv.Next();
  }

  // 5. Transition from being in managed to native code
  // TODO: ensure the transition to native follow a store fence.
  jni_asm->StoreStackPointerToThread(Thread::TopOfManagedStackOffset());
  jni_asm->StoreImmediateToThread(Thread::StateOffset(), Thread::kNative,
                                  mr_conv.InterproceduralScratchRegister());

  // 6. Move frame down to allow space for out going args. Do for as short a
  //    time as possible to aid profiling..
  const size_t out_arg_size = jni_conv.OutArgSize();
  jni_asm->IncreaseFrameSize(out_arg_size);

  // 7. Acquire lock for synchronized methods.
  if (native_method->IsSynchronized()) {
    // TODO: preserve incoming arguments in registers
    mr_conv.ResetIterator(FrameOffset(frame_size+out_arg_size));
    jni_conv.ResetIterator(FrameOffset(out_arg_size));
    jni_conv.Next();  // Skip JNIEnv*
    // Get SIRT entry for 1st argument
    if (is_static) {
      FrameOffset sirt_offset = jni_conv.CurrentParamSirtEntryOffset();
      if (jni_conv.IsCurrentParamOnStack()) {
        FrameOffset out_off = jni_conv.CurrentParamStackOffset();
        jni_asm->CreateSirtEntry(out_off, sirt_offset,
                                 mr_conv.InterproceduralScratchRegister(),
                                 false);
      } else {
        ManagedRegister out_reg = jni_conv.CurrentParamRegister();
        jni_asm->CreateSirtEntry(out_reg, sirt_offset,
                                 ManagedRegister::NoRegister(), false);
      }
    } else {
      CopyParameter(jni_asm, &mr_conv, &jni_conv, frame_size, out_arg_size);
    }
    // Generate JNIEnv* in place and leave a copy in jni_fns_register
    jni_conv.ResetIterator(FrameOffset(out_arg_size));
    ManagedRegister jni_fns_register =
        jni_conv.InterproceduralScratchRegister();
    jni_asm->LoadRawPtrFromThread(jni_fns_register, Thread::JniEnvOffset());
    SetNativeParameter(jni_asm, &jni_conv, jni_fns_register);
    // Call JNIEnv->MonitorEnter(object)
    jni_asm->LoadRawPtr(jni_fns_register, jni_fns_register, functions);
    jni_asm->Call(jni_fns_register, monitor_enter,
                  jni_conv.InterproceduralScratchRegister());
    jni_asm->FillFromSpillArea(spill_regs, out_arg_size);
    jni_asm->ExceptionPoll(jni_conv.InterproceduralScratchRegister());
  }

  // 8. Iterate over arguments placing values from managed calling convention in
  //    to the convention required for a native call (shuffling). For references
  //    place an index/pointer to the reference after checking whether it is
  //    NULL (which must be encoded as NULL).
  //    NB. we do this prior to materializing the JNIEnv* and static's jclass to
  //    give as many free registers for the shuffle as possible
  mr_conv.ResetIterator(FrameOffset(frame_size+out_arg_size));
  uint32_t args_count = 0;
  while (mr_conv.HasNext()) {
    args_count++;
    mr_conv.Next();
  }

  // Do a backward pass over arguments, so that the generated code will be "mov
  // R2, R3; mov R1, R2" instead of "mov R1, R2; mov R2, R3."
  // TODO: A reverse iterator to improve readability.
  for (uint32_t i = 0; i < args_count; ++i) {
    mr_conv.ResetIterator(FrameOffset(frame_size+out_arg_size));
    jni_conv.ResetIterator(FrameOffset(out_arg_size));
    jni_conv.Next();  // Skip JNIEnv*
    if (is_static) {
      jni_conv.Next();  // Skip Class for now
    }
    for (uint32_t j = 0; j < args_count - i - 1; ++j) {
      mr_conv.Next();
      jni_conv.Next();
    }
    CopyParameter(jni_asm, &mr_conv, &jni_conv, frame_size, out_arg_size);
  }

  if (is_static) {
    // Create argument for Class
    mr_conv.ResetIterator(FrameOffset(frame_size+out_arg_size));
    jni_conv.ResetIterator(FrameOffset(out_arg_size));
    jni_conv.Next();  // Skip JNIEnv*
    FrameOffset sirt_offset = jni_conv.CurrentParamSirtEntryOffset();
    if (jni_conv.IsCurrentParamOnStack()) {
      FrameOffset out_off = jni_conv.CurrentParamStackOffset();
      jni_asm->CreateSirtEntry(out_off, sirt_offset,
                               mr_conv.InterproceduralScratchRegister(),
                               false);
    } else {
      ManagedRegister out_reg = jni_conv.CurrentParamRegister();
      jni_asm->CreateSirtEntry(out_reg, sirt_offset,
                               ManagedRegister::NoRegister(), false);
    }
  }
  // 9. Create 1st argument, the JNI environment ptr
  jni_conv.ResetIterator(FrameOffset(out_arg_size));
  if (jni_conv.IsCurrentParamInRegister()) {
    jni_asm->LoadRawPtrFromThread(jni_conv.CurrentParamRegister(),
                                  Thread::JniEnvOffset());
  } else {
    jni_asm->CopyRawPtrFromThread(jni_conv.CurrentParamStackOffset(),
                                  Thread::JniEnvOffset(),
                                  jni_conv.InterproceduralScratchRegister());
  }

  // 10. Plant call to native code associated with method
  if (!jni_conv.IsOutArgRegister(mr_conv.MethodRegister())) {
    // Method register shouldn't have been crushed by setting up outgoing
    // arguments
    jni_asm->Call(mr_conv.MethodRegister(), Method::NativeMethodOffset(),
                  mr_conv.InterproceduralScratchRegister());
  } else {
    jni_asm->Call(jni_conv.MethodStackOffset(), Method::NativeMethodOffset(),
                  mr_conv.InterproceduralScratchRegister());
  }
  // 11. Release lock for synchronized methods.
  if (native_method->IsSynchronized()) {
    mr_conv.ResetIterator(FrameOffset(frame_size+out_arg_size));
    jni_conv.ResetIterator(FrameOffset(out_arg_size));
    jni_conv.Next();  // Skip JNIEnv*
    // Save return value
    FrameOffset return_save_location = jni_conv.ReturnValueSaveLocation();
    CHECK_LT(return_save_location.Uint32Value(), frame_size+out_arg_size);
    jni_asm->Store(return_save_location, jni_conv.ReturnRegister(),
                   jni_conv.SizeOfReturnValue());
    // Get SIRT entry for 1st argument
    if (is_static) {
      FrameOffset sirt_offset = jni_conv.CurrentParamSirtEntryOffset();
      if (jni_conv.IsCurrentParamOnStack()) {
        FrameOffset out_off = jni_conv.CurrentParamStackOffset();
        jni_asm->CreateSirtEntry(out_off, sirt_offset,
                                 mr_conv.InterproceduralScratchRegister(),
                                 false);
      } else {
        ManagedRegister out_reg = jni_conv.CurrentParamRegister();
        jni_asm->CreateSirtEntry(out_reg, sirt_offset,
                                 ManagedRegister::NoRegister(), false);
      }
    } else {
      CopyParameter(jni_asm, &mr_conv, &jni_conv, frame_size, out_arg_size);
    }
    // Generate JNIEnv* in place and leave a copy in jni_env_register
    jni_conv.ResetIterator(FrameOffset(out_arg_size));
    ManagedRegister jni_env_register =
        jni_conv.InterproceduralScratchRegister();
    jni_asm->LoadRawPtrFromThread(jni_env_register, Thread::JniEnvOffset());
    SetNativeParameter(jni_asm, &jni_conv, jni_env_register);
    // Call JNIEnv->MonitorExit(object)
    jni_asm->LoadRawPtr(jni_env_register, jni_env_register, functions);
    jni_asm->Call(jni_env_register, monitor_exit,
                  jni_conv.InterproceduralScratchRegister());
    // Reload return value
    jni_asm->Load(jni_conv.ReturnRegister(), return_save_location,
                  jni_conv.SizeOfReturnValue());
  }

  // 12. Release outgoing argument area
  jni_asm->DecreaseFrameSize(out_arg_size);
  mr_conv.ResetIterator(FrameOffset(frame_size));
  jni_conv.ResetIterator(FrameOffset(0));

  // 13. Transition from being in native to managed code, possibly entering a
  //     safepoint
  CHECK(!jni_conv.InterproceduralScratchRegister()
                 .Equals(jni_conv.ReturnRegister()));  // don't clobber result
  // Location to preserve result on slow path, ensuring its within the frame
  FrameOffset return_save_location = jni_conv.ReturnValueSaveLocation();
  CHECK_LT(return_save_location.Uint32Value(), frame_size);
  jni_asm->SuspendPoll(jni_conv.InterproceduralScratchRegister(),
                       jni_conv.ReturnRegister(), return_save_location,
                       jni_conv.SizeOfReturnValue());
  jni_asm->ExceptionPoll(jni_conv.InterproceduralScratchRegister());
  jni_asm->StoreImmediateToThread(Thread::StateOffset(), Thread::kRunnable,
                                  jni_conv.InterproceduralScratchRegister());


  // 14. Place result in correct register possibly loading from indirect
  //     reference table
  if (jni_conv.IsReturnAReference()) {
    jni_asm->IncreaseFrameSize(kStackAlignment);
    jni_conv.ResetIterator(FrameOffset(kStackAlignment));

    jni_conv.Next();
    SetNativeParameter(jni_asm, &jni_conv, jni_conv.ReturnRegister());

    jni_conv.ResetIterator(FrameOffset(kStackAlignment));

    if (jni_conv.IsCurrentParamInRegister()) {
      jni_asm->GetCurrentThread(jni_conv.CurrentParamRegister());
    } else {
      jni_asm->GetCurrentThread(jni_conv.CurrentParamStackOffset(),
                                jni_conv.InterproceduralScratchRegister());
    }

    jni_asm->Call(reinterpret_cast<uintptr_t>(DecodeJObjectInThread),
                  jni_conv.InterproceduralScratchRegister());

    jni_asm->DecreaseFrameSize(kStackAlignment);
    jni_conv.ResetIterator(FrameOffset(0));
  }
  jni_asm->Move(mr_conv.ReturnRegister(), jni_conv.ReturnRegister());

  // 15. Remove SIRT from thread
  jni_asm->CopyRawPtrToThread(Thread::TopSirtOffset(), jni_conv.SirtLinkOffset(),
                              jni_conv.InterproceduralScratchRegister());

  // 16. Remove activation
  jni_asm->RemoveFrame(frame_size, spill_regs);

  // 17. Finalize code generation
  jni_asm->EmitSlowPaths();
  size_t cs = jni_asm->CodeSize();
  MemoryRegion code(AllocateCode(cs), cs);
  jni_asm->FinalizeInstructions(code);
  native_method->SetCode(reinterpret_cast<byte*>(code.pointer()), cs,
      jni_asm->GetInstructionSet());
  native_method->SetFrameSizeInBytes(frame_size);
  native_method->SetReturnPcOffsetInBytes(jni_conv.ReturnPcOffset());
}

void JniCompiler::SetNativeParameter(Assembler *jni_asm,
                                     JniCallingConvention *jni_conv,
                                     ManagedRegister in_reg) {
  if (jni_conv->IsCurrentParamOnStack()) {
    FrameOffset dest = jni_conv->CurrentParamStackOffset();
    jni_asm->StoreRawPtr(dest, in_reg);
  } else {
    if (!jni_conv->CurrentParamRegister().Equals(in_reg)) {
      jni_asm->Move(jni_conv->CurrentParamRegister(), in_reg);
    }
  }
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
  CHECK(input_in_reg || mr_conv->IsCurrentParamOnStack());
  CHECK(output_in_reg || jni_conv->IsCurrentParamOnStack());
  // References need placing in SIRT and the entry address passing
  if (ref_param) {
    null_allowed = mr_conv->IsCurrentUserArg();
    // Compute SIRT offset. Note null is placed in the SIRT but the jobject
    // passed to the native code must be null (not a pointer into the SIRT
    // as with regular references).
    sirt_offset = jni_conv->CurrentParamSirtEntryOffset();
    // Check SIRT offset is within frame.
    CHECK_LT(sirt_offset.Uint32Value(), (frame_size+out_arg_size));
  }
  if (input_in_reg && output_in_reg) {
    ManagedRegister in_reg = mr_conv->CurrentParamRegister();
    ManagedRegister out_reg = jni_conv->CurrentParamRegister();
    if (ref_param) {
      jni_asm->CreateSirtEntry(out_reg, sirt_offset, in_reg, null_allowed);
    } else {
      jni_asm->Move(out_reg, in_reg);
    }
  } else if (!input_in_reg && !output_in_reg) {
    FrameOffset out_off = jni_conv->CurrentParamStackOffset();
    if (ref_param) {
      jni_asm->CreateSirtEntry(out_off, sirt_offset,
                               mr_conv->InterproceduralScratchRegister(),
                               null_allowed);
    } else {
      FrameOffset in_off = mr_conv->CurrentParamStackOffset();
      size_t param_size = mr_conv->CurrentParamSize();
      CHECK_EQ(param_size, jni_conv->CurrentParamSize());
      jni_asm->Copy(out_off, in_off, mr_conv->InterproceduralScratchRegister(),
                    param_size);
    }
  } else if (!input_in_reg && output_in_reg) {
    FrameOffset in_off = mr_conv->CurrentParamStackOffset();
    ManagedRegister out_reg = jni_conv->CurrentParamRegister();
    // Check that incoming stack arguments are above the current stack frame.
    CHECK_GT(in_off.Uint32Value(), frame_size);
    if (ref_param) {
      jni_asm->CreateSirtEntry(out_reg, sirt_offset,
                               ManagedRegister::NoRegister(), null_allowed);
    } else {
      unsigned int param_size = mr_conv->CurrentParamSize();
      CHECK_EQ(param_size, jni_conv->CurrentParamSize());
      jni_asm->Load(out_reg, in_off, param_size);
    }
  } else {
    CHECK(input_in_reg && !output_in_reg);
    ManagedRegister in_reg = mr_conv->CurrentParamRegister();
    FrameOffset out_off = jni_conv->CurrentParamStackOffset();
    // Check outgoing argument is within frame
    CHECK_LT(out_off.Uint32Value(), frame_size);
    if (ref_param) {
      // TODO: recycle value in in_reg rather than reload from SIRT
      jni_asm->CreateSirtEntry(out_off, sirt_offset,
                               mr_conv->InterproceduralScratchRegister(),
                               null_allowed);
    } else {
      size_t param_size = mr_conv->CurrentParamSize();
      CHECK_EQ(param_size, jni_conv->CurrentParamSize());
      jni_asm->Store(out_off, in_reg, param_size);
    }
  }
}

void* JniCompiler::AllocateCode(size_t size) {
  CHECK_LT(((jni_code_top_ - jni_code_->GetAddress()) + size), jni_code_->GetLength());
  void *result = jni_code_top_;
  jni_code_top_ += size;
  return result;
}

JniCompiler::JniCompiler() {
  // TODO: this shouldn't be managed by the JniCompiler, we should have a
  // code cache.
  jni_code_.reset(MemMap::Map(kPageSize, PROT_READ | PROT_WRITE | PROT_EXEC));
  CHECK(jni_code_.get() !=  NULL);
  jni_code_top_ = jni_code_->GetAddress();
}

JniCompiler::~JniCompiler() {}

}  // namespace art
