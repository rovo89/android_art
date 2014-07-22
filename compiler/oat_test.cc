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

#include "common_compiler_test.h"
#include "compiler.h"
#include "dex/verification_results.h"
#include "dex/quick/dex_file_to_method_inliner_map.h"
#include "dex/quick_compiler_callbacks.h"
#include "entrypoints/quick/quick_entrypoints.h"
#include "mirror/art_method-inl.h"
#include "mirror/class-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/object-inl.h"
#include "oat_file-inl.h"
#include "oat_writer.h"
#include "scoped_thread_state_change.h"
#include "vector_output_stream.h"

namespace art {

class OatTest : public CommonCompilerTest {
 protected:
  static const bool kCompile = false;  // DISABLED_ due to the time to compile libcore

  void CheckMethod(mirror::ArtMethod* method,
                   const OatFile::OatMethod& oat_method,
                   const DexFile* dex_file)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    const CompiledMethod* compiled_method =
        compiler_driver_->GetCompiledMethod(MethodReference(dex_file,
                                                            method->GetDexMethodIndex()));

    if (compiled_method == NULL) {
      EXPECT_TRUE(oat_method.GetQuickCode() == NULL) << PrettyMethod(method) << " "
                                                     << oat_method.GetQuickCode();
      EXPECT_TRUE(oat_method.GetPortableCode() == NULL) << PrettyMethod(method) << " "
                                                        << oat_method.GetPortableCode();
      EXPECT_EQ(oat_method.GetFrameSizeInBytes(), 0U);
      EXPECT_EQ(oat_method.GetCoreSpillMask(), 0U);
      EXPECT_EQ(oat_method.GetFpSpillMask(), 0U);
    } else {
      const void* quick_oat_code = oat_method.GetQuickCode();
      if (quick_oat_code != nullptr) {
        EXPECT_EQ(oat_method.GetFrameSizeInBytes(), compiled_method->GetFrameSizeInBytes());
        EXPECT_EQ(oat_method.GetCoreSpillMask(), compiled_method->GetCoreSpillMask());
        EXPECT_EQ(oat_method.GetFpSpillMask(), compiled_method->GetFpSpillMask());
        uintptr_t oat_code_aligned = RoundDown(reinterpret_cast<uintptr_t>(quick_oat_code), 2);
        quick_oat_code = reinterpret_cast<const void*>(oat_code_aligned);
        const std::vector<uint8_t>* quick_code = compiled_method->GetQuickCode();
        EXPECT_TRUE(quick_code != nullptr);
        size_t code_size = quick_code->size() * sizeof(quick_code[0]);
        EXPECT_EQ(0, memcmp(quick_oat_code, &quick_code[0], code_size))
            << PrettyMethod(method) << " " << code_size;
        CHECK_EQ(0, memcmp(quick_oat_code, &quick_code[0], code_size));
      } else {
        const void* portable_oat_code = oat_method.GetPortableCode();
        EXPECT_TRUE(portable_oat_code != nullptr) << PrettyMethod(method);
        EXPECT_EQ(oat_method.GetFrameSizeInBytes(), 0U);
        EXPECT_EQ(oat_method.GetCoreSpillMask(), 0U);
        EXPECT_EQ(oat_method.GetFpSpillMask(), 0U);
        uintptr_t oat_code_aligned = RoundDown(reinterpret_cast<uintptr_t>(portable_oat_code), 2);
        portable_oat_code = reinterpret_cast<const void*>(oat_code_aligned);
        const std::vector<uint8_t>* portable_code = compiled_method->GetPortableCode();
        EXPECT_TRUE(portable_code != nullptr);
        size_t code_size = portable_code->size() * sizeof(portable_code[0]);
        EXPECT_EQ(0, memcmp(quick_oat_code, &portable_code[0], code_size))
            << PrettyMethod(method) << " " << code_size;
        CHECK_EQ(0, memcmp(quick_oat_code, &portable_code[0], code_size));
      }
    }
  }
};

TEST_F(OatTest, WriteRead) {
  TimingLogger timings("OatTest::WriteRead", false, false);
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();

  // TODO: make selectable.
  Compiler::Kind compiler_kind = kUsePortableCompiler
      ? Compiler::kPortable
      : Compiler::kQuick;
  InstructionSet insn_set = kIsTargetBuild ? kThumb2 : kX86;

  InstructionSetFeatures insn_features;
  compiler_options_.reset(new CompilerOptions);
  verification_results_.reset(new VerificationResults(compiler_options_.get()));
  method_inliner_map_.reset(new DexFileToMethodInlinerMap);
  callbacks_.reset(new QuickCompilerCallbacks(verification_results_.get(),
                                              method_inliner_map_.get()));
  timer_.reset(new CumulativeLogger("Compilation times"));
  compiler_driver_.reset(new CompilerDriver(compiler_options_.get(),
                                            verification_results_.get(),
                                            method_inliner_map_.get(),
                                            compiler_kind, insn_set,
                                            insn_features, false, NULL, 2, true, true,
                                            timer_.get()));
  jobject class_loader = NULL;
  if (kCompile) {
    TimingLogger timings("OatTest::WriteRead", false, false);
    compiler_driver_->CompileAll(class_loader, class_linker->GetBootClassPath(), &timings);
  }

  ScopedObjectAccess soa(Thread::Current());
  ScratchFile tmp;
  SafeMap<std::string, std::string> key_value_store;
  key_value_store.Put(OatHeader::kImageLocationKey, "lue.art");
  OatWriter oat_writer(class_linker->GetBootClassPath(),
                       42U,
                       4096U,
                       0,
                       compiler_driver_.get(),
                       &timings,
                       &key_value_store);
  bool success = compiler_driver_->WriteElf(GetTestAndroidRoot(),
                                            !kIsTargetBuild,
                                            class_linker->GetBootClassPath(),
                                            &oat_writer,
                                            tmp.GetFile());
  ASSERT_TRUE(success);

  if (kCompile) {  // OatWriter strips the code, regenerate to compare
    compiler_driver_->CompileAll(class_loader, class_linker->GetBootClassPath(), &timings);
  }
  std::string error_msg;
  std::unique_ptr<OatFile> oat_file(OatFile::Open(tmp.GetFilename(), tmp.GetFilename(), NULL, false,
                                            &error_msg));
  ASSERT_TRUE(oat_file.get() != nullptr) << error_msg;
  const OatHeader& oat_header = oat_file->GetOatHeader();
  ASSERT_TRUE(oat_header.IsValid());
  ASSERT_EQ(1U, oat_header.GetDexFileCount());  // core
  ASSERT_EQ(42U, oat_header.GetImageFileLocationOatChecksum());
  ASSERT_EQ(4096U, oat_header.GetImageFileLocationOatDataBegin());
  ASSERT_EQ("lue.art", std::string(oat_header.GetStoreValueByKey(OatHeader::kImageLocationKey)));

  const DexFile* dex_file = java_lang_dex_file_;
  uint32_t dex_file_checksum = dex_file->GetLocationChecksum();
  const OatFile::OatDexFile* oat_dex_file = oat_file->GetOatDexFile(dex_file->GetLocation().c_str(),
                                                                    &dex_file_checksum);
  ASSERT_TRUE(oat_dex_file != nullptr);
  CHECK_EQ(dex_file->GetLocationChecksum(), oat_dex_file->GetDexFileLocationChecksum());
  for (size_t i = 0; i < dex_file->NumClassDefs(); i++) {
    const DexFile::ClassDef& class_def = dex_file->GetClassDef(i);
    const byte* class_data = dex_file->GetClassData(class_def);
    size_t num_virtual_methods = 0;
    if (class_data != NULL) {
      ClassDataItemIterator it(*dex_file, class_data);
      num_virtual_methods = it.NumVirtualMethods();
    }
    const char* descriptor = dex_file->GetClassDescriptor(class_def);
    StackHandleScope<1> hs(soa.Self());
    mirror::Class* klass = class_linker->FindClass(soa.Self(), descriptor,
                                                   NullHandle<mirror::ClassLoader>());

    const OatFile::OatClass oat_class = oat_dex_file->GetOatClass(i);
    CHECK_EQ(mirror::Class::Status::kStatusNotReady, oat_class.GetStatus()) << descriptor;
    CHECK_EQ(kCompile ? OatClassType::kOatClassAllCompiled : OatClassType::kOatClassNoneCompiled,
             oat_class.GetType()) << descriptor;

    size_t method_index = 0;
    for (size_t i = 0; i < klass->NumDirectMethods(); i++, method_index++) {
      CheckMethod(klass->GetDirectMethod(i),
                  oat_class.GetOatMethod(method_index), dex_file);
    }
    for (size_t i = 0; i < num_virtual_methods; i++, method_index++) {
      CheckMethod(klass->GetVirtualMethod(i),
                  oat_class.GetOatMethod(method_index), dex_file);
    }
  }
}

TEST_F(OatTest, OatHeaderSizeCheck) {
  // If this test is failing and you have to update these constants,
  // it is time to update OatHeader::kOatVersion
  EXPECT_EQ(84U, sizeof(OatHeader));
  EXPECT_EQ(8U, sizeof(OatMethodOffsets));
  EXPECT_EQ(24U, sizeof(OatQuickMethodHeader));
  EXPECT_EQ(79 * GetInstructionSetPointerSize(kRuntimeISA), sizeof(QuickEntryPoints));
}

TEST_F(OatTest, OatHeaderIsValid) {
    InstructionSet instruction_set = kX86;
    InstructionSetFeatures instruction_set_features;
    std::vector<const DexFile*> dex_files;
    uint32_t image_file_location_oat_checksum = 0;
    uint32_t image_file_location_oat_begin = 0;
    OatHeader* oat_header = OatHeader::Create(instruction_set,
                                              instruction_set_features,
                                              &dex_files,
                                              image_file_location_oat_checksum,
                                              image_file_location_oat_begin,
                                              nullptr);
    ASSERT_NE(oat_header, nullptr);
    ASSERT_TRUE(oat_header->IsValid());

    char* magic = const_cast<char*>(oat_header->GetMagic());
    strcpy(magic, "");  // bad magic
    ASSERT_FALSE(oat_header->IsValid());
    strcpy(magic, "oat\n000");  // bad version
    ASSERT_FALSE(oat_header->IsValid());
}

}  // namespace art
