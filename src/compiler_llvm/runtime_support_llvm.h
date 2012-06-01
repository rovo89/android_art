/*
 * Copyright (C) 2012 The Android Open Source Project
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

#ifndef ART_SRC_COMPILER_LLVM_RUNTIME_SUPPORT_LLVM_H_
#define ART_SRC_COMPILER_LLVM_RUNTIME_SUPPORT_LLVM_H_

#include "object.h"

namespace art {

class Method;
class Object;

//----------------------------------------------------------------------------
// Thread
//----------------------------------------------------------------------------

ShadowFrame* art_push_shadow_frame_from_code(Thread* thread, ShadowFrame* new_shadow_frame,
                                             Method* method, uint32_t size);

void art_pop_shadow_frame_from_code(void*);


//----------------------------------------------------------------------------
// Exception
//----------------------------------------------------------------------------

bool art_is_exception_pending_from_code();

void art_throw_div_zero_from_code();

void art_throw_array_bounds_from_code(int32_t length, int32_t index);

void art_throw_no_such_method_from_code(int32_t method_idx);

void art_throw_null_pointer_exception_from_code(uint32_t dex_pc);

void art_throw_stack_overflow_from_code();

void art_throw_exception_from_code(Object* exception);

int32_t art_find_catch_block_from_code(Method* current_method,
                                       uint32_t ti_offset);


void art_test_suspend_from_code(Thread* thread);

void art_set_current_thread_from_code(void* thread_object_addr);

//----------------------------------------------------------------------------
// Runtime Support Function Lookup Callback
//----------------------------------------------------------------------------

void* art_find_runtime_support_func(void* context, char const* name);

const void* art_fix_stub_from_code(Method* called);

}  // namespace art

#endif  // ART_SRC_COMPILER_LLVM_RUNTIME_SUPPORT_LLVM_H_
