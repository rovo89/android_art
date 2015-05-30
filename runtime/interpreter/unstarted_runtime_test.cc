/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include "unstarted_runtime.h"

#include "class_linker.h"
#include "common_runtime_test.h"
#include "dex_instruction.h"
#include "handle.h"
#include "handle_scope-inl.h"
#include "interpreter/interpreter_common.h"
#include "mirror/class_loader.h"
#include "mirror/string-inl.h"
#include "runtime.h"
#include "scoped_thread_state_change.h"
#include "thread.h"

namespace art {
namespace interpreter {

class UnstartedRuntimeTest : public CommonRuntimeTest {
 protected:
  // Re-expose all UnstartedRuntime implementations so we don't need to declare a million
  // test friends.

  // Methods that intercept available libcore implementations.
#define UNSTARTED_DIRECT(Name, SigIgnored)                 \
  static void Unstarted ## Name(Thread* self,              \
                                ShadowFrame* shadow_frame, \
                                JValue* result,            \
                                size_t arg_offset)         \
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {        \
    interpreter::UnstartedRuntime::Unstarted ## Name(self, shadow_frame, result, arg_offset); \
  }
#include "unstarted_runtime_list.h"
  UNSTARTED_RUNTIME_DIRECT_LIST(UNSTARTED_DIRECT)
#undef UNSTARTED_RUNTIME_DIRECT_LIST
#undef UNSTARTED_RUNTIME_JNI_LIST
#undef UNSTARTED_DIRECT

  // Methods that are native.
#define UNSTARTED_JNI(Name, SigIgnored)                       \
  static void UnstartedJNI ## Name(Thread* self,              \
                                   ArtMethod* method,         \
                                   mirror::Object* receiver,  \
                                   uint32_t* args,            \
                                   JValue* result)            \
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {           \
    interpreter::UnstartedRuntime::UnstartedJNI ## Name(self, method, receiver, args, result); \
  }
#include "unstarted_runtime_list.h"
  UNSTARTED_RUNTIME_JNI_LIST(UNSTARTED_JNI)
#undef UNSTARTED_RUNTIME_DIRECT_LIST
#undef UNSTARTED_RUNTIME_JNI_LIST
#undef UNSTARTED_JNI
};

TEST_F(UnstartedRuntimeTest, MemoryPeekByte) {
  Thread* self = Thread::Current();

  ScopedObjectAccess soa(self);
  constexpr const uint8_t base_array[] = "abcdefghijklmnop";
  constexpr int32_t kBaseLen = sizeof(base_array) / sizeof(uint8_t);
  const uint8_t* base_ptr = base_array;

  JValue result;
  ShadowFrame* tmp = ShadowFrame::CreateDeoptimizedFrame(10, nullptr, nullptr, 0);

  for (int32_t i = 0; i < kBaseLen; ++i) {
    tmp->SetVRegLong(0, static_cast<int64_t>(reinterpret_cast<intptr_t>(base_ptr + i)));

    UnstartedMemoryPeekByte(self, tmp, &result, 0);

    EXPECT_EQ(result.GetB(), static_cast<int8_t>(base_array[i]));
  }

  ShadowFrame::DeleteDeoptimizedFrame(tmp);
}

TEST_F(UnstartedRuntimeTest, MemoryPeekShort) {
  Thread* self = Thread::Current();

  ScopedObjectAccess soa(self);
  constexpr const uint8_t base_array[] = "abcdefghijklmnop";
  constexpr int32_t kBaseLen = sizeof(base_array) / sizeof(uint8_t);
  const uint8_t* base_ptr = base_array;

  JValue result;
  ShadowFrame* tmp = ShadowFrame::CreateDeoptimizedFrame(10, nullptr, nullptr, 0);

  int32_t adjusted_length = kBaseLen - sizeof(int16_t);
  for (int32_t i = 0; i < adjusted_length; ++i) {
    tmp->SetVRegLong(0, static_cast<int64_t>(reinterpret_cast<intptr_t>(base_ptr + i)));

    UnstartedMemoryPeekShort(self, tmp, &result, 0);

    typedef int16_t unaligned_short __attribute__ ((aligned (1)));
    const unaligned_short* short_ptr = reinterpret_cast<const unaligned_short*>(base_ptr + i);
    EXPECT_EQ(result.GetS(), *short_ptr);
  }

  ShadowFrame::DeleteDeoptimizedFrame(tmp);
}

TEST_F(UnstartedRuntimeTest, MemoryPeekInt) {
  Thread* self = Thread::Current();

  ScopedObjectAccess soa(self);
  constexpr const uint8_t base_array[] = "abcdefghijklmnop";
  constexpr int32_t kBaseLen = sizeof(base_array) / sizeof(uint8_t);
  const uint8_t* base_ptr = base_array;

  JValue result;
  ShadowFrame* tmp = ShadowFrame::CreateDeoptimizedFrame(10, nullptr, nullptr, 0);

  int32_t adjusted_length = kBaseLen - sizeof(int32_t);
  for (int32_t i = 0; i < adjusted_length; ++i) {
    tmp->SetVRegLong(0, static_cast<int64_t>(reinterpret_cast<intptr_t>(base_ptr + i)));

    UnstartedMemoryPeekInt(self, tmp, &result, 0);

    typedef int32_t unaligned_int __attribute__ ((aligned (1)));
    const unaligned_int* int_ptr = reinterpret_cast<const unaligned_int*>(base_ptr + i);
    EXPECT_EQ(result.GetI(), *int_ptr);
  }

  ShadowFrame::DeleteDeoptimizedFrame(tmp);
}

TEST_F(UnstartedRuntimeTest, MemoryPeekLong) {
  Thread* self = Thread::Current();

  ScopedObjectAccess soa(self);
  constexpr const uint8_t base_array[] = "abcdefghijklmnop";
  constexpr int32_t kBaseLen = sizeof(base_array) / sizeof(uint8_t);
  const uint8_t* base_ptr = base_array;

  JValue result;
  ShadowFrame* tmp = ShadowFrame::CreateDeoptimizedFrame(10, nullptr, nullptr, 0);

  int32_t adjusted_length = kBaseLen - sizeof(int64_t);
  for (int32_t i = 0; i < adjusted_length; ++i) {
    tmp->SetVRegLong(0, static_cast<int64_t>(reinterpret_cast<intptr_t>(base_ptr + i)));

    UnstartedMemoryPeekLong(self, tmp, &result, 0);

    typedef int64_t unaligned_long __attribute__ ((aligned (1)));
    const unaligned_long* long_ptr = reinterpret_cast<const unaligned_long*>(base_ptr + i);
    EXPECT_EQ(result.GetJ(), *long_ptr);
  }

  ShadowFrame::DeleteDeoptimizedFrame(tmp);
}

TEST_F(UnstartedRuntimeTest, StringGetCharsNoCheck) {
  Thread* self = Thread::Current();

  ScopedObjectAccess soa(self);
  StackHandleScope<2> hs(self);
  // TODO: Actual UTF.
  constexpr const char base_string[] = "abcdefghijklmnop";
  Handle<mirror::String> h_test_string(hs.NewHandle(
      mirror::String::AllocFromModifiedUtf8(self, base_string)));
  constexpr int32_t kBaseLen = sizeof(base_string) / sizeof(char) - 1;
  Handle<mirror::CharArray> h_char_array(hs.NewHandle(
      mirror::CharArray::Alloc(self, kBaseLen)));
  // A buffer so we can make sure we only modify the elements targetted.
  uint16_t buf[kBaseLen];

  JValue result;
  ShadowFrame* tmp = ShadowFrame::CreateDeoptimizedFrame(10, nullptr, nullptr, 0);

  for (int32_t start_index = 0; start_index < kBaseLen; ++start_index) {
    for (int32_t count = 0; count <= kBaseLen; ++count) {
      for (int32_t trg_offset = 0; trg_offset < kBaseLen; ++trg_offset) {
        // Only do it when in bounds.
        if (start_index + count <= kBaseLen && trg_offset + count <= kBaseLen) {
          tmp->SetVRegReference(0, h_test_string.Get());
          tmp->SetVReg(1, start_index);
          tmp->SetVReg(2, count);
          tmp->SetVRegReference(3, h_char_array.Get());
          tmp->SetVReg(3, trg_offset);

          // Copy the char_array into buf.
          memcpy(buf, h_char_array->GetData(), kBaseLen * sizeof(uint16_t));

          UnstartedStringCharAt(self, tmp, &result, 0);

          uint16_t* data = h_char_array->GetData();

          bool success = true;

          // First segment should be unchanged.
          for (int32_t i = 0; i < trg_offset; ++i) {
            success = success && (data[i] == buf[i]);
          }
          // Second segment should be a copy.
          for (int32_t i = trg_offset; i < trg_offset + count; ++i) {
            success = success && (data[i] == buf[i - trg_offset + start_index]);
          }
          // Third segment should be unchanged.
          for (int32_t i = trg_offset + count; i < kBaseLen; ++i) {
            success = success && (data[i] == buf[i]);
          }

          EXPECT_TRUE(success);
        }
      }
    }
  }

  ShadowFrame::DeleteDeoptimizedFrame(tmp);
}

TEST_F(UnstartedRuntimeTest, StringCharAt) {
  Thread* self = Thread::Current();

  ScopedObjectAccess soa(self);
  // TODO: Actual UTF.
  constexpr const char* base_string = "abcdefghijklmnop";
  int32_t base_len = static_cast<int32_t>(strlen(base_string));
  mirror::String* test_string = mirror::String::AllocFromModifiedUtf8(self, base_string);

  JValue result;
  ShadowFrame* tmp = ShadowFrame::CreateDeoptimizedFrame(10, nullptr, nullptr, 0);

  for (int32_t i = 0; i < base_len; ++i) {
    tmp->SetVRegReference(0, test_string);
    tmp->SetVReg(1, i);

    UnstartedStringCharAt(self, tmp, &result, 0);

    EXPECT_EQ(result.GetI(), base_string[i]);
  }

  ShadowFrame::DeleteDeoptimizedFrame(tmp);
}

TEST_F(UnstartedRuntimeTest, StringInit) {
  Thread* self = Thread::Current();
  ScopedObjectAccess soa(self);
  mirror::Class* klass = mirror::String::GetJavaLangString();
  ArtMethod* method = klass->FindDeclaredDirectMethod("<init>", "(Ljava/lang/String;)V",
                                                      sizeof(void*));

  // create instruction data for invoke-direct {v0, v1} of method with fake index
  uint16_t inst_data[3] = { 0x2070, 0x0000, 0x0010 };
  const Instruction* inst = Instruction::At(inst_data);

  JValue result;
  ShadowFrame* shadow_frame = ShadowFrame::CreateDeoptimizedFrame(10, nullptr, method, 0);
  const char* base_string = "hello_world";
  mirror::String* string_arg = mirror::String::AllocFromModifiedUtf8(self, base_string);
  mirror::String* reference_empty_string = mirror::String::AllocFromModifiedUtf8(self, "");
  shadow_frame->SetVRegReference(0, reference_empty_string);
  shadow_frame->SetVRegReference(1, string_arg);

  interpreter::DoCall<false, false>(method, self, *shadow_frame, inst, inst_data[0], &result);
  mirror::String* string_result = reinterpret_cast<mirror::String*>(result.GetL());
  EXPECT_EQ(string_arg->GetLength(), string_result->GetLength());
  EXPECT_EQ(memcmp(string_arg->GetValue(), string_result->GetValue(),
                   string_arg->GetLength() * sizeof(uint16_t)), 0);

  ShadowFrame::DeleteDeoptimizedFrame(shadow_frame);
}

}  // namespace interpreter
}  // namespace art
