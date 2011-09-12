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

// package java.lang;
// import java.io.IOException;
// class Object {};
// public class MyClass {
//   int f() throws Exception {
//     try {
//         g(1);
//     } catch (IOException e) {
//         return 1;
//     } catch (Exception e) {
//         return 2;
//     }
//     try {
//         g(2);
//     } catch (IOException e) {
//         return 3;
//     }
//     return 0;
//   }
//   void g(int doThrow) throws Exception {
//     if (doThrow == 1)
//         throw new Exception();
//     else if (doThrow == 2)
//         throw new IOException();
//   }
// }

static const char kMyClassExceptionHandleDex[] =
  "ZGV4CjAzNQC/bXXtLZJLN1GzLr+ncrvPSl70n8t0yAjgAwAAcAAAAHhWNBIAAAAAAAAAACgDAAAN"
  "AAAAcAAAAAcAAACkAAAAAwAAAMAAAAAAAAAAAAAAAAYAAADkAAAAAgAAABQBAACMAgAAVAEAAD4C"
  "AABGAgAASQIAAGUCAAB8AgAAkwIAAKgCAAC8AgAAygIAAM0CAADRAgAA1AIAANcCAAABAAAAAgAA"
  "AAMAAAAEAAAABQAAAAYAAAAIAAAAAQAAAAAAAAAAAAAACAAAAAYAAAAAAAAACQAAAAYAAAA4AgAA"
  "AgABAAAAAAADAAEAAAAAAAQAAQAAAAAABAAAAAoAAAAEAAIACwAAAAUAAQAAAAAABQAAAAAAAAD/"
  "////AAAAAAcAAAAAAAAACQMAAAAAAAAEAAAAAQAAAAUAAAAAAAAABwAAABgCAAATAwAAAAAAAAEA"
  "AAABAwAAAQABAAAAAADeAgAAAQAAAA4AAAABAAEAAQAAAOMCAAAEAAAAcBAFAAAADgAEAAEAAgAC"
  "AOgCAAAVAAAAEiISERIQbiAEAAMAEiBuIAQAAwASAA8ADQABECj9DQABICj6DQASMCj3AAADAAAA"
  "AwABAAcAAAADAAYAAgICDAMPAQISAAAAAwACAAEAAAD3AgAAEwAAABIQMwIIACIAAwBwEAEAAAAn"
  "ABIgMwIIACIAAgBwEAAAAAAnAA4AAAAAAAAAAAAAAAIAAAAAAAAAAwAAAFQBAAAEAAAAVAEAAAEA"
  "AAAAAAY8aW5pdD4AAUkAGkxkYWx2aWsvYW5ub3RhdGlvbi9UaHJvd3M7ABVMamF2YS9pby9JT0V4"
  "Y2VwdGlvbjsAFUxqYXZhL2xhbmcvRXhjZXB0aW9uOwATTGphdmEvbGFuZy9NeUNsYXNzOwASTGph"
  "dmEvbGFuZy9PYmplY3Q7AAxNeUNsYXNzLmphdmEAAVYAAlZJAAFmAAFnAAV2YWx1ZQADAAcOAAQA"
  "Bw4ABwAHLFFOAnYsLR4tIR4AFQEABw48aTxpAAIBAQwcARgDAAABAAWAgATcAgAAAQICgYAE8AID"
  "AIgDAQDgAwAAAA8AAAAAAAAAAQAAAAAAAAABAAAADQAAAHAAAAACAAAABwAAAKQAAAADAAAAAwAA"
  "AMAAAAAFAAAABgAAAOQAAAAGAAAAAgAAABQBAAADEAAAAQAAAFQBAAABIAAABAAAAFwBAAAGIAAA"
  "AQAAABgCAAABEAAAAQAAADgCAAACIAAADQAAAD4CAAADIAAABAAAAN4CAAAEIAAAAQAAAAEDAAAA"
  "IAAAAgAAAAkDAAAAEAAAAQAAACgDAAA=";

class ExceptionTest : public CommonTest {
 protected:
  virtual void SetUp() {
    CommonTest::SetUp();

    dex_.reset(OpenDexFileBase64(kMyClassExceptionHandleDex, "kMyClassExceptionHandleDex"));
    ASSERT_TRUE(dex_.get() != NULL);
    const ClassLoader* class_loader = AllocPathClassLoader(dex_.get());
    ASSERT_TRUE(class_loader != NULL);
    my_klass_ = class_linker_->FindClass("Ljava/lang/MyClass;", class_loader);
    ASSERT_TRUE(my_klass_ != NULL);
    method_f_ = my_klass_->FindVirtualMethod("f", "()I");
    ASSERT_TRUE(method_f_ != NULL);
    method_f_->SetFrameSizeInBytes(kStackAlignment);
    method_f_->SetReturnPcOffsetInBytes(4);
    method_g_ = my_klass_->FindVirtualMethod("g", "(I)V");
    ASSERT_TRUE(method_g_ != NULL);
    method_g_->SetFrameSizeInBytes(kStackAlignment);
    method_g_->SetReturnPcOffsetInBytes(4);
  }

  DexFile::CatchHandlerItem FindCatchHandlerItem(Method* method,
                                                 const char exception_type[],
                                                 uint32_t addr) {
    const DexFile::CodeItem* code_item = dex_->GetCodeItem(method->GetCodeItemOffset());
    for (DexFile::CatchHandlerIterator iter = dex_->dexFindCatchHandler(*code_item, addr);
         !iter.HasNext(); iter.Next()) {
      if (strcmp(exception_type, dex_->dexStringByTypeIdx(iter.Get().type_idx_)) == 0) {
        return iter.Get();
      }
    }
    return DexFile::CatchHandlerItem();
  }

  UniquePtr<const DexFile> dex_;

  Method* method_f_;
  Method* method_g_;

 private:
  Class* my_klass_;
};

TEST_F(ExceptionTest, FindCatchHandler) {
  const DexFile::CodeItem *code_item = dex_->GetCodeItem(method_f_->GetCodeItemOffset());

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
  enum {STACK_SIZE = 1000};
  uint32_t top_of_stack = 0;
  uintptr_t fake_stack[STACK_SIZE];
  ASSERT_EQ(kStackAlignment, 16);
  ASSERT_EQ(sizeof(uintptr_t), sizeof(uint32_t));

  // Create/push fake 16byte stack frame for method g
  fake_stack[top_of_stack++] = reinterpret_cast<uintptr_t>(method_g_);
  fake_stack[top_of_stack++] = 3;
  fake_stack[top_of_stack++] = 0;
  fake_stack[top_of_stack++] = 0;

  // Create/push fake 16byte stack frame for method f
  fake_stack[top_of_stack++] = reinterpret_cast<uintptr_t>(method_f_);
  fake_stack[top_of_stack++] = 3;
  fake_stack[top_of_stack++] = 0;
  fake_stack[top_of_stack++] = 0;

  // Pull Method* of NULL to terminate the trace
  fake_stack[top_of_stack++] = NULL;

  Thread* thread = Thread::Current();
  thread->SetTopOfStack(fake_stack);

  jobject internal = thread->CreateInternalStackTrace();
  jobjectArray ste_array =
      Thread::InternalStackTraceToStackTraceElementArray(internal,
                                                         thread->GetJniEnv());
  ObjectArray<StackTraceElement>* trace_array =
      Decode<ObjectArray<StackTraceElement>*>(thread->GetJniEnv(), ste_array);


  ASSERT_TRUE(trace_array->Get(0) != NULL);
  EXPECT_STREQ("java.lang.MyClass",
               trace_array->Get(0)->GetDeclaringClass()->ToModifiedUtf8().c_str());
  EXPECT_STREQ("MyClass.java", trace_array->Get(0)->GetFileName()->ToModifiedUtf8().c_str());
  EXPECT_STREQ("g", trace_array->Get(0)->GetMethodName()->ToModifiedUtf8().c_str());
  EXPECT_EQ(22, trace_array->Get(0)->GetLineNumber());

  ASSERT_TRUE(trace_array->Get(1) != NULL);
  EXPECT_STREQ("java.lang.MyClass",
               trace_array->Get(1)->GetDeclaringClass()->ToModifiedUtf8().c_str());
  EXPECT_STREQ("MyClass.java", trace_array->Get(1)->GetFileName()->ToModifiedUtf8().c_str());
  EXPECT_STREQ("f", trace_array->Get(1)->GetMethodName()->ToModifiedUtf8().c_str());
  EXPECT_EQ(7, trace_array->Get(1)->GetLineNumber());
}

}  // namespace art
