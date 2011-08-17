// Copyright 2011 Google Inc. All Rights Reserved.

#include <sys/mman.h>

#include "assembler.h"
#include "class_linker.h"
#include "common_test.h"
#include "dex_file.h"
#include "jni_compiler.h"
#include "runtime.h"
#include "thread.h"
#include "gtest/gtest.h"

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
};

TEST_F(ExceptionTest, MyClass_F_G) {
  scoped_ptr<DexFile> dex(OpenDexFileBase64(kMyClassExceptionHandleDex));
  PathClassLoader* class_loader = AllocPathClassLoader(dex.get());
  Class* klass = class_linker_->FindClass("Ljava/lang/MyClass;", class_loader);
  ASSERT_TRUE(klass != NULL);

  Method* method_f = klass->FindVirtualMethod("f", "()I");
  ASSERT_TRUE(method_f != NULL);

  const DexFile& dex_file = class_linker_->FindDexFile(klass->GetDexCache());
  const DexFile::CodeItem *code_item = dex_file.GetCodeItem(method_f->code_off_);

  ASSERT_TRUE(code_item != NULL);

  ASSERT_EQ(2u, code_item->tries_size_);
  ASSERT_NE(0u, code_item->insns_size_);

  const struct DexFile::TryItem *t0, *t1;
  t0 = dex_file.dexGetTryItems(*code_item, 0);
  t1 = dex_file.dexGetTryItems(*code_item, 1);
  EXPECT_LE(t0->start_addr_, t1->start_addr_);

  DexFile::CatchHandlerIterator iter =
    dex_file.dexFindCatchHandler(*code_item, 4 /* Dex PC in the first try block */);
  ASSERT_EQ(false, iter.End());
  EXPECT_STREQ("Ljava/io/IOException;", dex_file.dexStringByTypeIdx(iter.Get().type_idx_));
  iter.Next();
  ASSERT_EQ(false, iter.End());
  EXPECT_STREQ("Ljava/lang/Exception;", dex_file.dexStringByTypeIdx(iter.Get().type_idx_));
  iter.Next();
  ASSERT_EQ(true, iter.End());

  iter = dex_file.dexFindCatchHandler(*code_item, 8 /* Dex PC in the second try block */);
  ASSERT_EQ(false, iter.End());
  EXPECT_STREQ("Ljava/io/IOException;", dex_file.dexStringByTypeIdx(iter.Get().type_idx_));
  iter.Next();
  ASSERT_EQ(true, iter.End());
}

}  // namespace art
