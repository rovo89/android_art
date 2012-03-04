// Copyright 2012 Google Inc. All Rights Reserved.

#ifndef ART_SRC_COMPILER_LLVM_RUNTIME_SUPPORT_LLVM_H_
#define ART_SRC_COMPILER_LLVM_RUNTIME_SUPPORT_LLVM_H_

namespace art {

void art_push_shadow_frame_from_code(void* new_shadow_frame);

void art_pop_shadow_frame_from_code();

bool art_is_exception_pending_from_code();

void art_test_suspend_from_code();

void art_set_current_thread_from_code(void* thread_object_addr);

}  // namespace art

#endif  // ART_SRC_COMPILER_LLVM_RUNTIME_SUPPORT_LLVM_H_
