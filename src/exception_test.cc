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

#include <sys/mman.h>

#include "UniquePtr.h"
#include "assembler.h"
#include "class_linker.h"
#include "common_test.h"
#include "dex_file.h"
#include "gtest/gtest.h"
#include "jni_compiler.h"
#include "runtime.h"
#include "thread.h"

namespace art {

class ExceptionTest : public CommonTest {
 protected:
  virtual void SetUp() {
    CommonTest::SetUp();

    SirtRef<ClassLoader> class_loader(LoadDex("ExceptionHandle"));
    my_klass_ = class_linker_->FindClass("LExceptionHandle;", class_loader.get());
    ASSERT_TRUE(my_klass_ != NULL);

    dex_ = &Runtime::Current()->GetClassLinker()->FindDexFile(my_klass_->GetDexCache());

    for (size_t i = 0 ; i < 12; i++) {
      fake_code_.push_back(0x70000000 | i);
    }

    fake_mapping_data_.push_back(2);  // first element is count of remaining elements
    fake_mapping_data_.push_back(3);  // offset 3
    fake_mapping_data_.push_back(3);  // maps to dex offset 3

    method_f_ = my_klass_->FindVirtualMethod("f", "()I");
    ASSERT_TRUE(method_f_ != NULL);
    method_f_->SetFrameSizeInBytes(kStackAlignment);
    method_f_->SetCode(CompiledMethod::CodePointer(&fake_code_[0], kThumb2));
    method_f_->SetMappingTable(&fake_mapping_data_[0]);

    method_g_ = my_klass_->FindVirtualMethod("g", "(I)V");
    ASSERT_TRUE(method_g_ != NULL);
    method_g_->SetFrameSizeInBytes(kStackAlignment);
    method_g_->SetCode(CompiledMethod::CodePointer(&fake_code_[0], kThumb2));
    method_g_->SetMappingTable(&fake_mapping_data_[0]);
  }

  const DexFile* dex_;

  std::vector<uint8_t> fake_code_;
  std::vector<uint32_t> fake_mapping_data_;

  Method* method_f_;
  Method* method_g_;

 private:
  Class* my_klass_;
};

TEST_F(ExceptionTest, FindCatchHandler) {
  const DexFile::CodeItem* code_item = dex_->GetCodeItem(method_f_->GetCodeItemOffset());

  ASSERT_TRUE(code_item != NULL);

  ASSERT_EQ(2u, code_item->tries_size_);
  ASSERT_NE(0u, code_item->insns_size_in_code_units_);

  const struct DexFile::TryItem *t0, *t1;
  t0 = dex_->GetTryItems(*code_item, 0);
  t1 = dex_->GetTryItems(*code_item, 1);
  EXPECT_LE(t0->start_addr_, t1->start_addr_);
  {
    CatchHandlerIterator iter(*code_item, 4 /* Dex PC in the first try block */);
    EXPECT_STREQ("Ljava/io/IOException;", dex_->StringByTypeIdx(iter.GetHandlerTypeIndex()));
    ASSERT_TRUE(iter.HasNext());
    iter.Next();
    EXPECT_STREQ("Ljava/lang/Exception;", dex_->StringByTypeIdx(iter.GetHandlerTypeIndex()));
    ASSERT_TRUE(iter.HasNext());
    iter.Next();
    EXPECT_FALSE(iter.HasNext());
  }
  {
    CatchHandlerIterator iter(*code_item, 8 /* Dex PC in the second try block */);
    EXPECT_STREQ("Ljava/io/IOException;", dex_->StringByTypeIdx(iter.GetHandlerTypeIndex()));
    ASSERT_TRUE(iter.HasNext());
    iter.Next();
    EXPECT_FALSE(iter.HasNext());
  }
  {
    CatchHandlerIterator iter(*code_item, 11 /* Dex PC not in any try block */);
    EXPECT_FALSE(iter.HasNext());
  }
}

TEST_F(ExceptionTest, StackTraceElement) {
  runtime_->Start();

  std::vector<uintptr_t> fake_stack;
  ASSERT_EQ(kStackAlignment, 16);
  ASSERT_EQ(sizeof(uintptr_t), sizeof(uint32_t));

  // Create two fake stack frames with mapping data created in SetUp. We map offset 3 in the code
  // to dex pc 3, however, we set the return pc to 5 as the stack walker always subtracts two
  // from a return pc.
  const uintptr_t pc_offset = 3 + 2;

  // Create/push fake 16byte stack frame for method g
  fake_stack.push_back(reinterpret_cast<uintptr_t>(method_g_));
  fake_stack.push_back(0);
  fake_stack.push_back(0);
  fake_stack.push_back(reinterpret_cast<uintptr_t>(method_f_->GetCode()) + pc_offset);  // return pc

  // Create/push fake 16byte stack frame for method f
  fake_stack.push_back(reinterpret_cast<uintptr_t>(method_f_));
  fake_stack.push_back(0);
  fake_stack.push_back(0);
  fake_stack.push_back(0xEBAD6070);  // return pc

  // Pull Method* of NULL to terminate the trace
  fake_stack.push_back(0);

  // Set up thread to appear as if we called out of method_g_ at pc 3
  Thread* thread = Thread::Current();
  thread->SetTopOfStack(&fake_stack[0], reinterpret_cast<uintptr_t>(method_g_->GetCode()) + pc_offset);  // return pc

  JNIEnv* env = thread->GetJniEnv();
  jobject internal = thread->CreateInternalStackTrace(env);
  jobjectArray ste_array = Thread::InternalStackTraceToStackTraceElementArray(env, internal);
  ObjectArray<StackTraceElement>* trace_array =
      Decode<ObjectArray<StackTraceElement>*>(env, ste_array);

  ASSERT_TRUE(trace_array->Get(0) != NULL);
  EXPECT_STREQ("ExceptionHandle",
               trace_array->Get(0)->GetDeclaringClass()->ToModifiedUtf8().c_str());
  EXPECT_STREQ("ExceptionHandle.java", trace_array->Get(0)->GetFileName()->ToModifiedUtf8().c_str());
  EXPECT_STREQ("g", trace_array->Get(0)->GetMethodName()->ToModifiedUtf8().c_str());
  EXPECT_EQ(37, trace_array->Get(0)->GetLineNumber());

  ASSERT_TRUE(trace_array->Get(1) != NULL);
  EXPECT_STREQ("ExceptionHandle",
               trace_array->Get(1)->GetDeclaringClass()->ToModifiedUtf8().c_str());
  EXPECT_STREQ("ExceptionHandle.java", trace_array->Get(1)->GetFileName()->ToModifiedUtf8().c_str());
  EXPECT_STREQ("f", trace_array->Get(1)->GetMethodName()->ToModifiedUtf8().c_str());
  EXPECT_EQ(22, trace_array->Get(1)->GetLineNumber());
}

}  // namespace art
