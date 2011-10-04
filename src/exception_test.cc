// Copyright 2011 Google Inc. All Rights Reserved.

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

    const ClassLoader* class_loader = LoadDex("ExceptionHandle");
    my_klass_ = class_linker_->FindClass("LExceptionHandle;", class_loader);
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
    method_f_->SetReturnPcOffsetInBytes(kStackAlignment - kPointerSize);
    method_f_->SetCode(CompiledMethod::CodePointer(&fake_code_[0], kThumb2));
    method_f_->SetMappingTable(&fake_mapping_data_[0]);

    method_g_ = my_klass_->FindVirtualMethod("g", "(I)V");
    ASSERT_TRUE(method_g_ != NULL);
    method_g_->SetFrameSizeInBytes(kStackAlignment);
    method_g_->SetReturnPcOffsetInBytes(kStackAlignment - kPointerSize);
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
  ASSERT_NE(0u, code_item->insns_size_);

  const struct DexFile::TryItem *t0, *t1;
  t0 = dex_->dexGetTryItems(*code_item, 0);
  t1 = dex_->dexGetTryItems(*code_item, 1);
  EXPECT_LE(t0->start_addr_, t1->start_addr_);

  DexFile::CatchHandlerIterator iter =
    dex_->dexFindCatchHandler(*code_item, 4 /* Dex PC in the first try block */);
  ASSERT_EQ(false, iter.HasNext());
  EXPECT_STREQ("Ljava/io/IOException;", dex_->dexStringByTypeIdx(iter.Get().type_idx_));
  iter.Next();
  ASSERT_EQ(false, iter.HasNext());
  EXPECT_STREQ("Ljava/lang/Exception;", dex_->dexStringByTypeIdx(iter.Get().type_idx_));
  iter.Next();
  ASSERT_EQ(true, iter.HasNext());

  iter = dex_->dexFindCatchHandler(*code_item, 8 /* Dex PC in the second try block */);
  ASSERT_EQ(false, iter.HasNext());
  EXPECT_STREQ("Ljava/io/IOException;", dex_->dexStringByTypeIdx(iter.Get().type_idx_));
  iter.Next();
  ASSERT_EQ(true, iter.HasNext());

  iter = dex_->dexFindCatchHandler(*code_item, 11 /* Dex PC not in any try block */);
  ASSERT_EQ(true, iter.HasNext());
}

TEST_F(ExceptionTest, StackTraceElement) {
  runtime_->Start();

  enum {STACK_SIZE = 1000};
  uint32_t top_of_stack = 0;
  uintptr_t fake_stack[STACK_SIZE];
  ASSERT_EQ(kStackAlignment, 16);
  ASSERT_EQ(sizeof(uintptr_t), sizeof(uint32_t));

  // Create two fake stack frames with mapping data created in SetUp. We map offset 3 in the code
  // to dex pc 3, however, we set the return pc to 5 as the stack walker always subtracts two
  // from a return pc.

  // Create/push fake 16byte stack frame for method g
  fake_stack[top_of_stack++] = reinterpret_cast<uintptr_t>(method_g_);
  fake_stack[top_of_stack++] = 0;
  fake_stack[top_of_stack++] = 0;
  fake_stack[top_of_stack++] = reinterpret_cast<uintptr_t>(method_f_->GetCode()) + 5;  // return pc

  // Create/push fake 16byte stack frame for method f
  fake_stack[top_of_stack++] = reinterpret_cast<uintptr_t>(method_f_);
  fake_stack[top_of_stack++] = 0;
  fake_stack[top_of_stack++] = 0;
  fake_stack[top_of_stack++] = 0xEBAD6070;  // return pc

  // Pull Method* of NULL to terminate the trace
  fake_stack[top_of_stack++] = NULL;

  // Set up thread to appear as if we called out of method_g_ at pc 3
  Thread* thread = Thread::Current();
  thread->SetTopOfStack(fake_stack, reinterpret_cast<uintptr_t>(method_g_->GetCode()) + 3);

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
  EXPECT_EQ(22, trace_array->Get(0)->GetLineNumber());

  ASSERT_TRUE(trace_array->Get(1) != NULL);
  EXPECT_STREQ("ExceptionHandle",
               trace_array->Get(1)->GetDeclaringClass()->ToModifiedUtf8().c_str());
  EXPECT_STREQ("ExceptionHandle.java", trace_array->Get(1)->GetFileName()->ToModifiedUtf8().c_str());
  EXPECT_STREQ("f", trace_array->Get(1)->GetMethodName()->ToModifiedUtf8().c_str());
  EXPECT_EQ(7, trace_array->Get(1)->GetLineNumber());
}

}  // namespace art
