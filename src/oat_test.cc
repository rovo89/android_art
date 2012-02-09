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

#include "oat_file.h"
#include "oat_writer.h"

#include "common_test.h"

namespace art {

class OatTest : public CommonTest {};

TEST_F(OatTest, WriteRead) {
  const bool compile = false;  // DISABLED_ due to the time to compile libcore
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();

  SirtRef<ClassLoader> class_loader(NULL);
  if (compile) {
    compiler_.reset(new Compiler(kThumb2, false, 2, NULL));
    compiler_->CompileAll(class_loader.get(), class_linker->GetBootClassPath());
  }

  ScratchFile tmp;
  bool success = OatWriter::Create(tmp.GetFile(), class_loader.get(), class_linker->GetBootClassPath(), *compiler_.get());
  ASSERT_TRUE(success);

  if (compile) {  // OatWriter strips the code, regenerate to compare
    compiler_->CompileAll(class_loader.get(), class_linker->GetBootClassPath());
  }
  UniquePtr<OatFile> oat_file(OatFile::Open(tmp.GetFilename(), tmp.GetFilename(), NULL));
  ASSERT_TRUE(oat_file.get() != NULL);
  const OatHeader& oat_header = oat_file->GetOatHeader();
  ASSERT_EQ(1U, oat_header.GetDexFileCount());

  const DexFile* dex_file = java_lang_dex_file_;
  const OatFile::OatDexFile* oat_dex_file = oat_file->GetOatDexFile(dex_file->GetLocation());
  CHECK_EQ(dex_file->GetLocationChecksum(), oat_dex_file->GetDexFileLocationChecksum());
  for (size_t i = 0; i < dex_file->NumClassDefs(); i++) {
    const DexFile::ClassDef& class_def = dex_file->GetClassDef(i);
    const byte* class_data = dex_file->GetClassData(class_def);
    size_t num_virtual_methods =0;
    if (class_data != NULL) {
      ClassDataItemIterator it(*dex_file, class_data);
      num_virtual_methods = it.NumVirtualMethods();
    }
    const char* descriptor = dex_file->GetClassDescriptor(class_def);

    UniquePtr<const OatFile::OatClass> oat_class(oat_dex_file->GetOatClass(i));

    Class* klass = class_linker->FindClass(descriptor, class_loader.get());

    size_t method_index = 0;
    for (size_t i = 0; i < klass->NumDirectMethods(); i++, method_index++) {
      Method* method = klass->GetDirectMethod(i);
      const OatFile::OatMethod oat_method = oat_class->GetOatMethod(method_index);
      const CompiledMethod* compiled_method =
          compiler_->GetCompiledMethod(Compiler::MethodReference(dex_file,
                                                                 method->GetDexMethodIndex()));

      if (compiled_method == NULL) {
        EXPECT_TRUE(oat_method.GetCode() == NULL) << PrettyMethod(method) << " " << oat_method.GetCode();
        EXPECT_EQ(oat_method.GetFrameSizeInBytes(), static_cast<uint32_t>(kStackAlignment));
        EXPECT_EQ(oat_method.GetCoreSpillMask(), 0U);
        EXPECT_EQ(oat_method.GetFpSpillMask(), 0U);
      } else {
        const void* oat_code = oat_method.GetCode();
        uintptr_t oat_code_aligned = RoundDown(reinterpret_cast<uintptr_t>(oat_code), 2);
        oat_code = reinterpret_cast<const void*>(oat_code_aligned);

        const std::vector<uint8_t>& code = compiled_method->GetCode();
        size_t code_size = code.size() * sizeof(code[0]);
        EXPECT_EQ(0, memcmp(oat_code, &code[0], code_size))
            << PrettyMethod(method) << " " << code_size;
        CHECK_EQ(0, memcmp(oat_code, &code[0], code_size));
        EXPECT_EQ(oat_method.GetFrameSizeInBytes(), compiled_method->GetFrameSizeInBytes());
        EXPECT_EQ(oat_method.GetCoreSpillMask(), compiled_method->GetCoreSpillMask());
        EXPECT_EQ(oat_method.GetFpSpillMask(), compiled_method->GetFpSpillMask());
      }
    }
    for (size_t i = 0; i < num_virtual_methods; i++, method_index++) {
      Method* method = klass->GetVirtualMethod(i);
      const OatFile::OatMethod oat_method = oat_class->GetOatMethod(method_index);
      const CompiledMethod* compiled_method =
          compiler_->GetCompiledMethod(Compiler::MethodReference(dex_file,
                                                                 method->GetDexMethodIndex()));

      if (compiled_method == NULL) {
        EXPECT_TRUE(oat_method.GetCode() == NULL) << PrettyMethod(method) << " " << oat_method.GetCode();
        EXPECT_EQ(oat_method.GetFrameSizeInBytes(), static_cast<uint32_t>(kStackAlignment));
        EXPECT_EQ(oat_method.GetCoreSpillMask(), 0U);
        EXPECT_EQ(oat_method.GetFpSpillMask(), 0U);
      } else {
        const void* oat_code = oat_method.GetCode();
        EXPECT_TRUE(oat_code != NULL) << PrettyMethod(method);
        uintptr_t oat_code_aligned = RoundDown(reinterpret_cast<uintptr_t>(oat_code), 2);
        oat_code = reinterpret_cast<const void*>(oat_code_aligned);

        const std::vector<uint8_t>& code = compiled_method->GetCode();
        size_t code_size = code.size() * sizeof(code[0]);
        EXPECT_EQ(0, memcmp(oat_code, &code[0], code_size))
            << PrettyMethod(method) << " " << code_size;
        CHECK_EQ(0, memcmp(oat_code, &code[0], code_size));
        EXPECT_EQ(oat_method.GetFrameSizeInBytes(), compiled_method->GetFrameSizeInBytes());
        EXPECT_EQ(oat_method.GetCoreSpillMask(), compiled_method->GetCoreSpillMask());
        EXPECT_EQ(oat_method.GetFpSpillMask(), compiled_method->GetFpSpillMask());
      }
    }
  }
}

}  // namespace art
