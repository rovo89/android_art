// Copyright 2011 Google Inc. All Rights Reserved.

#include "oat_file.h"
#include "oat_writer.h"

#include "common_test.h"

namespace art {

class OatTest : public CommonTest {};

TEST_F(OatTest, WriteRead) {
  const bool compile = false;  // DISABLED_ due to the time to compile libcore

  const ClassLoader* class_loader = NULL;
  if (compile) {
    compiler_->CompileAll(class_loader);
  }

  ScratchFile tmp;
  bool success = OatWriter::Create(tmp.GetFilename(), class_loader);
  ASSERT_TRUE(success);

  if (compile) {  // OatWriter strips the code, regenerate to compare
    compiler_->CompileAll(class_loader);
  }
  UniquePtr<OatFile> oat_file(OatFile::Open(std::string(tmp.GetFilename()), "", NULL));
  ASSERT_TRUE(oat_file.get() != NULL);
  const OatHeader& oat_header = oat_file->GetOatHeader();
  ASSERT_EQ(1U, oat_header.GetDexFileCount());

  const Runtime* runtime = Runtime::Current();
  ClassLinker* class_linker = runtime->GetClassLinker();
  ByteArray* jni_stub_array = runtime->GetJniStubArray();
  ByteArray* ame_stub_array = runtime->GetAbstractMethodErrorStubArray();

  const DexFile& dex_file = *java_lang_dex_file_.get();
  const OatFile::OatDexFile& oat_dex_file = oat_file->GetOatDexFile(dex_file.GetLocation());
  for (size_t i = 0; i < dex_file.NumClassDefs(); i++) {
    const DexFile::ClassDef& class_def = dex_file.GetClassDef(i);
    const byte* class_data = dex_file.GetClassData(class_def);
    DexFile::ClassDataHeader header = dex_file.ReadClassDataHeader(&class_data);
    size_t num_virtual_methods = header.virtual_methods_size_;
    const char* descriptor = dex_file.GetClassDescriptor(class_def);

    const OatFile::OatClass oat_class = oat_dex_file.GetOatClass(i);

    Class* klass = class_linker->FindClass(descriptor, class_loader);

    size_t method_index = 0;
    for (size_t i = 0; i < klass->NumDirectMethods(); i++, method_index++) {
      Method* method = klass->GetDirectMethod(i);
      const void* oat_code = oat_class.GetMethodCode(method_index);
      uintptr_t oat_code_aligned = RoundDown(reinterpret_cast<uintptr_t>(oat_code), 2);
      oat_code = reinterpret_cast<const void*>(oat_code_aligned);
      const ByteArray* code_array = method->GetCodeArray();
      if (code_array == NULL || code_array == jni_stub_array || code_array == ame_stub_array) {
        ASSERT_TRUE(oat_code == NULL);
      } else {
        ASSERT_EQ(0, memcmp(oat_code, code_array->GetData(), code_array->GetLength()));
      }
    }
    for (size_t i = 0; i < num_virtual_methods; i++, method_index++) {
      Method* method = klass->GetVirtualMethod(i);
      const void* oat_code = oat_class.GetMethodCode(method_index);
      uintptr_t oat_code_aligned = RoundDown(reinterpret_cast<uintptr_t>(oat_code), 2);
      oat_code = reinterpret_cast<const void*>(oat_code_aligned);
      const ByteArray* code_array = method->GetCodeArray();
      if (code_array == NULL || code_array == jni_stub_array || code_array == ame_stub_array) {
        ASSERT_TRUE(oat_code == NULL);
      } else {
        ASSERT_TRUE(oat_code != NULL);
        ASSERT_EQ(0, memcmp(oat_code, code_array->GetData(), code_array->GetLength()));
      }
    }
  }
}

}  // namespace art
