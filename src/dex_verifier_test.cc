// Copyright 2011 Google Inc. All Rights Reserved.

#include "class_linker.h"
#include "common_test.h"
#include "dex_file.h"
#include "dex_verifier.h"
#include "scoped_ptr.h"

#include <stdio.h>

namespace art {

class DexVerifierTest : public CommonTest {
 protected:
  void VerifyClass(ClassLoader* class_loader, const StringPiece& descriptor) {
    ASSERT_TRUE(descriptor != NULL);
    Class* klass = class_linker_->FindSystemClass(descriptor);

    // Verify the class
    ASSERT_TRUE(DexVerify::VerifyClass(klass));
  }

  void VerifyDexFile(const DexFile* dex, ClassLoader* class_loader) {
    ASSERT_TRUE(dex != NULL);

    // Verify all the classes defined in this file
    for (size_t i = 0; i < dex->NumClassDefs(); i++) {
      const DexFile::ClassDef& class_def = dex->GetClassDef(i);
      const char* descriptor = dex->GetClassDescriptor(class_def);
      VerifyClass(class_loader, descriptor);
    }
  }

};

TEST_F(DexVerifierTest, LibCore) {
  VerifyDexFile(java_lang_dex_file_.get(), NULL);
}

TEST_F(DexVerifierTest, IntMath) {
  scoped_ptr<const DexFile> dex(OpenTestDexFile("IntMath"));
  PathClassLoader* class_loader = AllocPathClassLoader(dex.get());
  Class* klass = class_linker_->FindClass("LIntMath;", class_loader);
  ASSERT_TRUE(DexVerify::VerifyClass(klass));
}

}  // namespace art
