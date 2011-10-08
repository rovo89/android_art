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
    compiler_.reset(new Compiler(kThumb2, false));
    compiler_->CompileAll(class_loader);
  }

  ScratchFile tmp;
  bool success = OatWriter::Create(tmp.GetFilename(), class_loader, *compiler_.get());
  ASSERT_TRUE(success);

  if (compile) {  // OatWriter strips the code, regenerate to compare
    compiler_->CompileAll(class_loader);
  }
  UniquePtr<OatFile> oat_file(OatFile::Open(std::string(tmp.GetFilename()), "", NULL));
  ASSERT_TRUE(oat_file.get() != NULL);
  const OatHeader& oat_header = oat_file->GetOatHeader();
  ASSERT_EQ(1U, oat_header.GetDexFileCount());

  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();

  const DexFile& dex_file = *java_lang_dex_file_.get();
  const OatFile::OatDexFile* oat_dex_file = oat_file->GetOatDexFile(dex_file.GetLocation());
  for (size_t i = 0; i < dex_file.NumClassDefs(); i++) {
    const DexFile::ClassDef& class_def = dex_file.GetClassDef(i);
    const byte* class_data = dex_file.GetClassData(class_def);
    DexFile::ClassDataHeader header = dex_file.ReadClassDataHeader(&class_data);
    size_t num_virtual_methods = header.virtual_methods_size_;
    const char* descriptor = dex_file.GetClassDescriptor(class_def);

    UniquePtr<const OatFile::OatClass> oat_class(oat_dex_file->GetOatClass(i));

    Class* klass = class_linker->FindClass(descriptor, class_loader);

    size_t method_index = 0;
    for (size_t i = 0; i < klass->NumDirectMethods(); i++, method_index++) {
      Method* method = klass->GetDirectMethod(i);
      const OatFile::OatMethod oat_method = oat_class->GetOatMethod(method_index);
      const CompiledMethod* compiled_method = compiler_->GetCompiledMethod(method);

      if (compiled_method == NULL) {
        EXPECT_TRUE(oat_method.code_ == NULL) << PrettyMethod(method) << " " << oat_method.code_;
        EXPECT_EQ(oat_method.frame_size_in_bytes_, static_cast<uint32_t>(kStackAlignment));
        EXPECT_EQ(oat_method.return_pc_offset_in_bytes_, 0U);
        EXPECT_EQ(oat_method.core_spill_mask_, 0U);
        EXPECT_EQ(oat_method.fp_spill_mask_, 0U);
      } else {
        const void* oat_code = oat_method.code_;
        uintptr_t oat_code_aligned = RoundDown(reinterpret_cast<uintptr_t>(oat_code), 2);
        oat_code = reinterpret_cast<const void*>(oat_code_aligned);

        const std::vector<uint8_t>& code = compiled_method->GetCode();
        size_t code_size = code.size() * sizeof(code[0]);
        EXPECT_EQ(0, memcmp(oat_code, &code[0], code_size))
            << PrettyMethod(method) << " " << code_size;
        CHECK_EQ(0, memcmp(oat_code, &code[0], code_size));
        EXPECT_EQ(oat_method.frame_size_in_bytes_, compiled_method->GetFrameSizeInBytes());
        EXPECT_EQ(oat_method.return_pc_offset_in_bytes_, compiled_method->GetReturnPcOffsetInBytes());
        EXPECT_EQ(oat_method.core_spill_mask_, compiled_method->GetCoreSpillMask());
        EXPECT_EQ(oat_method.fp_spill_mask_, compiled_method->GetFpSpillMask());
      }
    }
    for (size_t i = 0; i < num_virtual_methods; i++, method_index++) {
      Method* method = klass->GetVirtualMethod(i);
      const OatFile::OatMethod oat_method = oat_class->GetOatMethod(method_index);
      const CompiledMethod* compiled_method = compiler_->GetCompiledMethod(method);

      if (compiled_method == NULL) {
        EXPECT_TRUE(oat_method.code_ == NULL) << PrettyMethod(method) << " " << oat_method.code_;
        EXPECT_EQ(oat_method.frame_size_in_bytes_, static_cast<uint32_t>(kStackAlignment));
        EXPECT_EQ(oat_method.return_pc_offset_in_bytes_, 0U);
        EXPECT_EQ(oat_method.core_spill_mask_, 0U);
        EXPECT_EQ(oat_method.fp_spill_mask_, 0U);
      } else {
        const void* oat_code = oat_method.code_;
        EXPECT_TRUE(oat_code != NULL) << PrettyMethod(method);
        uintptr_t oat_code_aligned = RoundDown(reinterpret_cast<uintptr_t>(oat_code), 2);
        oat_code = reinterpret_cast<const void*>(oat_code_aligned);

        const std::vector<uint8_t>& code = compiled_method->GetCode();
        size_t code_size = code.size() * sizeof(code[0]);
        EXPECT_EQ(0, memcmp(oat_code, &code[0], code_size))
            << PrettyMethod(method) << " " << code_size;
        CHECK_EQ(0, memcmp(oat_code, &code[0], code_size));
        EXPECT_EQ(oat_method.frame_size_in_bytes_, compiled_method->GetFrameSizeInBytes());
        EXPECT_EQ(oat_method.return_pc_offset_in_bytes_, compiled_method->GetReturnPcOffsetInBytes());
        EXPECT_EQ(oat_method.core_spill_mask_, compiled_method->GetCoreSpillMask());
        EXPECT_EQ(oat_method.fp_spill_mask_, compiled_method->GetFpSpillMask());
      }
    }
  }
}

}  // namespace art
