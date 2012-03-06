// Copyright 2012 Google Inc. All Rights Reserved.

#ifndef ART_SRC_COMPILER_LLVM_RUNTIME_SUPPORT_LLVM_H_
#define ART_SRC_COMPILER_LLVM_RUNTIME_SUPPORT_LLVM_H_

namespace art {

//----------------------------------------------------------------------------
// Thread
//----------------------------------------------------------------------------

void art_push_shadow_frame_from_code(void* new_shadow_frame);

void art_pop_shadow_frame_from_code();


//----------------------------------------------------------------------------
// Exception
//----------------------------------------------------------------------------

bool art_is_exception_pending_from_code();

void art_throw_div_zero_from_code();

void art_throw_array_bounds_from_code(int32_t length, int32_t index);

void art_throw_no_such_method_from_code(int32_t method_idx);

void art_throw_null_pointer_exception_from_code();

void art_throw_stack_overflow_from_code(void*);

void art_throw_exception_from_code(Object* exception);

int32_t art_find_catch_block_from_code(Object* exception, int32_t dex_pc);


void art_test_suspend_from_code();

void art_set_current_thread_from_code(void* thread_object_addr);


//----------------------------------------------------------------------------
// Runtime Support Function Lookup Callback
//----------------------------------------------------------------------------

void* art_find_runtime_support_func(void* context, char const* name);

}  // namespace art

#endif  // ART_SRC_COMPILER_LLVM_RUNTIME_SUPPORT_LLVM_H_
