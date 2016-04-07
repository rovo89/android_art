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

#include "image.h"

#include <memory>
#include <string>
#include <vector>

#include "base/unix_file/fd_file.h"
#include "class_linker-inl.h"
#include "common_compiler_test.h"
#include "debug/method_debug_info.h"
#include "driver/compiler_options.h"
#include "elf_writer.h"
#include "elf_writer_quick.h"
#include "gc/space/image_space.h"
#include "image_writer.h"
#include "linker/multi_oat_relative_patcher.h"
#include "lock_word.h"
#include "mirror/object-inl.h"
#include "oat_writer.h"
#include "scoped_thread_state_change.h"
#include "signal_catcher.h"
#include "utils.h"

namespace art {

class ImageTest : public CommonCompilerTest {
 protected:
  virtual void SetUp() {
    ReserveImageSpace();
    CommonCompilerTest::SetUp();
  }
  void TestWriteRead(ImageHeader::StorageMode storage_mode);
};

void ImageTest::TestWriteRead(ImageHeader::StorageMode storage_mode) {
  CreateCompilerDriver(Compiler::kOptimizing, kRuntimeISA, kIsTargetBuild ? 2U : 16U);

  // Set inline filter values.
  compiler_options_->SetInlineDepthLimit(CompilerOptions::kDefaultInlineDepthLimit);
  compiler_options_->SetInlineMaxCodeUnits(CompilerOptions::kDefaultInlineMaxCodeUnits);

  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  // Enable write for dex2dex.
  for (const DexFile* dex_file : class_linker->GetBootClassPath()) {
    dex_file->EnableWrite();
  }
  // Create a generic location tmp file, to be the base of the .art and .oat temporary files.
  ScratchFile location;
  ScratchFile image_location(location, ".art");

  std::string image_filename(GetSystemImageFilename(image_location.GetFilename().c_str(),
                                                    kRuntimeISA));
  size_t pos = image_filename.rfind('/');
  CHECK_NE(pos, std::string::npos) << image_filename;
  std::string image_dir(image_filename, 0, pos);
  int mkdir_result = mkdir(image_dir.c_str(), 0700);
  CHECK_EQ(0, mkdir_result) << image_dir;
  ScratchFile image_file(OS::CreateEmptyFile(image_filename.c_str()));

  std::string oat_filename(image_filename, 0, image_filename.size() - 3);
  oat_filename += "oat";
  ScratchFile oat_file(OS::CreateEmptyFile(oat_filename.c_str()));

  const uintptr_t requested_image_base = ART_BASE_ADDRESS;
  std::unordered_map<const DexFile*, size_t> dex_file_to_oat_index_map;
  std::vector<const char*> oat_filename_vector(1, oat_filename.c_str());
  for (const DexFile* dex_file : class_linker->GetBootClassPath()) {
    dex_file_to_oat_index_map.emplace(dex_file, 0);
  }
  std::unique_ptr<ImageWriter> writer(new ImageWriter(*compiler_driver_,
                                                      requested_image_base,
                                                      /*compile_pic*/false,
                                                      /*compile_app_image*/false,
                                                      storage_mode,
                                                      oat_filename_vector,
                                                      dex_file_to_oat_index_map));
  // TODO: compile_pic should be a test argument.
  {
    {
      jobject class_loader = nullptr;
      TimingLogger timings("ImageTest::WriteRead", false, false);
      TimingLogger::ScopedTiming t("CompileAll", &timings);
      compiler_driver_->SetDexFilesForOatFile(class_linker->GetBootClassPath());
      compiler_driver_->CompileAll(class_loader, class_linker->GetBootClassPath(), &timings);

      t.NewTiming("WriteElf");
      SafeMap<std::string, std::string> key_value_store;
      const std::vector<const DexFile*>& dex_files = class_linker->GetBootClassPath();
      std::unique_ptr<ElfWriter> elf_writer = CreateElfWriterQuick(
          compiler_driver_->GetInstructionSet(),
          compiler_driver_->GetInstructionSetFeatures(),
          &compiler_driver_->GetCompilerOptions(),
          oat_file.GetFile());
      elf_writer->Start();
      OatWriter oat_writer(/*compiling_boot_image*/true, &timings);
      OutputStream* rodata = elf_writer->StartRoData();
      for (const DexFile* dex_file : dex_files) {
        ArrayRef<const uint8_t> raw_dex_file(
            reinterpret_cast<const uint8_t*>(&dex_file->GetHeader()),
            dex_file->GetHeader().file_size_);
        oat_writer.AddRawDexFileSource(raw_dex_file,
                                       dex_file->GetLocation().c_str(),
                                       dex_file->GetLocationChecksum());
      }
      std::unique_ptr<MemMap> opened_dex_files_map;
      std::vector<std::unique_ptr<const DexFile>> opened_dex_files;
      bool dex_files_ok = oat_writer.WriteAndOpenDexFiles(
          rodata,
          oat_file.GetFile(),
          compiler_driver_->GetInstructionSet(),
          compiler_driver_->GetInstructionSetFeatures(),
          &key_value_store,
          /* verify */ false,           // Dex files may be dex-to-dex-ed, don't verify.
          &opened_dex_files_map,
          &opened_dex_files);
      ASSERT_TRUE(dex_files_ok);

      bool image_space_ok = writer->PrepareImageAddressSpace();
      ASSERT_TRUE(image_space_ok);

      linker::MultiOatRelativePatcher patcher(compiler_driver_->GetInstructionSet(),
                                              instruction_set_features_.get());
      oat_writer.PrepareLayout(compiler_driver_.get(), writer.get(), dex_files, &patcher);
      size_t rodata_size = oat_writer.GetOatHeader().GetExecutableOffset();
      size_t text_size = oat_writer.GetSize() - rodata_size;
      elf_writer->SetLoadedSectionSizes(rodata_size, text_size, oat_writer.GetBssSize());

      writer->UpdateOatFileLayout(/* oat_index */ 0u,
                                  elf_writer->GetLoadedSize(),
                                  oat_writer.GetOatDataOffset(),
                                  oat_writer.GetSize());

      bool rodata_ok = oat_writer.WriteRodata(rodata);
      ASSERT_TRUE(rodata_ok);
      elf_writer->EndRoData(rodata);

      OutputStream* text = elf_writer->StartText();
      bool text_ok = oat_writer.WriteCode(text);
      ASSERT_TRUE(text_ok);
      elf_writer->EndText(text);

      bool header_ok = oat_writer.WriteHeader(elf_writer->GetStream(), 0u, 0u, 0u);
      ASSERT_TRUE(header_ok);

      writer->UpdateOatFileHeader(/* oat_index */ 0u, oat_writer.GetOatHeader());

      elf_writer->WriteDynamicSection();
      elf_writer->WriteDebugInfo(oat_writer.GetMethodDebugInfo());
      elf_writer->WritePatchLocations(oat_writer.GetAbsolutePatchLocations());

      bool success = elf_writer->End();
      ASSERT_TRUE(success);
    }
  }
  // Workound bug that mcld::Linker::emit closes oat_file by reopening as dup_oat.
  std::unique_ptr<File> dup_oat(OS::OpenFileReadWrite(oat_file.GetFilename().c_str()));
  ASSERT_TRUE(dup_oat.get() != nullptr);

  {
    std::vector<const char*> dup_oat_filename(1, dup_oat->GetPath().c_str());
    std::vector<const char*> dup_image_filename(1, image_file.GetFilename().c_str());
    bool success_image = writer->Write(kInvalidFd,
                                       dup_image_filename,
                                       dup_oat_filename);
    ASSERT_TRUE(success_image);
    bool success_fixup = ElfWriter::Fixup(dup_oat.get(),
                                          writer->GetOatDataBegin(0));
    ASSERT_TRUE(success_fixup);

    ASSERT_EQ(dup_oat->FlushCloseOrErase(), 0) << "Could not flush and close oat file "
                                               << oat_file.GetFilename();
  }

  uint64_t image_file_size;
  {
    std::unique_ptr<File> file(OS::OpenFileForReading(image_file.GetFilename().c_str()));
    ASSERT_TRUE(file.get() != nullptr);
    ImageHeader image_header;
    ASSERT_EQ(file->ReadFully(&image_header, sizeof(image_header)), true);
    ASSERT_TRUE(image_header.IsValid());
    const auto& bitmap_section = image_header.GetImageSection(ImageHeader::kSectionImageBitmap);
    ASSERT_GE(bitmap_section.Offset(), sizeof(image_header));
    ASSERT_NE(0U, bitmap_section.Size());

    gc::Heap* heap = Runtime::Current()->GetHeap();
    ASSERT_TRUE(heap->HaveContinuousSpaces());
    gc::space::ContinuousSpace* space = heap->GetNonMovingSpace();
    ASSERT_FALSE(space->IsImageSpace());
    ASSERT_TRUE(space != nullptr);
    ASSERT_TRUE(space->IsMallocSpace());

    image_file_size = file->GetLength();
  }

  ASSERT_TRUE(compiler_driver_->GetImageClasses() != nullptr);
  std::unordered_set<std::string> image_classes(*compiler_driver_->GetImageClasses());

  // Need to delete the compiler since it has worker threads which are attached to runtime.
  compiler_driver_.reset();

  // Tear down old runtime before making a new one, clearing out misc state.

  // Remove the reservation of the memory for use to load the image.
  // Need to do this before we reset the runtime.
  UnreserveImageSpace();
  writer.reset(nullptr);

  runtime_.reset();
  java_lang_dex_file_ = nullptr;

  MemMap::Init();
  std::unique_ptr<const DexFile> dex(LoadExpectSingleDexFile(GetLibCoreDexFileNames()[0].c_str()));

  RuntimeOptions options;
  std::string image("-Ximage:");
  image.append(image_location.GetFilename());
  options.push_back(std::make_pair(image.c_str(), static_cast<void*>(nullptr)));
  // By default the compiler this creates will not include patch information.
  options.push_back(std::make_pair("-Xnorelocate", nullptr));

  if (!Runtime::Create(options, false)) {
    LOG(FATAL) << "Failed to create runtime";
    return;
  }
  runtime_.reset(Runtime::Current());
  // Runtime::Create acquired the mutator_lock_ that is normally given away when we Runtime::Start,
  // give it away now and then switch to a more managable ScopedObjectAccess.
  Thread::Current()->TransitionFromRunnableToSuspended(kNative);
  ScopedObjectAccess soa(Thread::Current());
  ASSERT_TRUE(runtime_.get() != nullptr);
  class_linker_ = runtime_->GetClassLinker();

  gc::Heap* heap = Runtime::Current()->GetHeap();
  ASSERT_TRUE(heap->HasBootImageSpace());
  ASSERT_TRUE(heap->GetNonMovingSpace()->IsMallocSpace());

  // We loaded the runtime with an explicit image, so it must exist.
  gc::space::ImageSpace* image_space = heap->GetBootImageSpaces()[0];
  ASSERT_TRUE(image_space != nullptr);
  if (storage_mode == ImageHeader::kStorageModeUncompressed) {
    // Uncompressed, image should be smaller than file.
    ASSERT_LE(image_space->Size(), image_file_size);
  } else {
    // Compressed, file should be smaller than image.
    ASSERT_LE(image_file_size, image_space->Size());
  }

  image_space->VerifyImageAllocations();
  uint8_t* image_begin = image_space->Begin();
  uint8_t* image_end = image_space->End();
  CHECK_EQ(requested_image_base, reinterpret_cast<uintptr_t>(image_begin));
  for (size_t i = 0; i < dex->NumClassDefs(); ++i) {
    const DexFile::ClassDef& class_def = dex->GetClassDef(i);
    const char* descriptor = dex->GetClassDescriptor(class_def);
    mirror::Class* klass = class_linker_->FindSystemClass(soa.Self(), descriptor);
    EXPECT_TRUE(klass != nullptr) << descriptor;
    if (image_classes.find(descriptor) != image_classes.end()) {
      // Image classes should be located inside the image.
      EXPECT_LT(image_begin, reinterpret_cast<uint8_t*>(klass)) << descriptor;
      EXPECT_LT(reinterpret_cast<uint8_t*>(klass), image_end) << descriptor;
    } else {
      EXPECT_TRUE(reinterpret_cast<uint8_t*>(klass) >= image_end ||
                  reinterpret_cast<uint8_t*>(klass) < image_begin) << descriptor;
    }
    EXPECT_TRUE(Monitor::IsValidLockWord(klass->GetLockWord(false)));
  }

  image_file.Unlink();
  oat_file.Unlink();
  int rmdir_result = rmdir(image_dir.c_str());
  CHECK_EQ(0, rmdir_result);
}

TEST_F(ImageTest, WriteReadUncompressed) {
  TestWriteRead(ImageHeader::kStorageModeUncompressed);
}

TEST_F(ImageTest, WriteReadLZ4) {
  TestWriteRead(ImageHeader::kStorageModeLZ4);
}

TEST_F(ImageTest, WriteReadLZ4HC) {
  TestWriteRead(ImageHeader::kStorageModeLZ4HC);
}


TEST_F(ImageTest, ImageHeaderIsValid) {
    uint32_t image_begin = ART_BASE_ADDRESS;
    uint32_t image_size_ = 16 * KB;
    uint32_t image_roots = ART_BASE_ADDRESS + (1 * KB);
    uint32_t oat_checksum = 0;
    uint32_t oat_file_begin = ART_BASE_ADDRESS + (4 * KB);  // page aligned
    uint32_t oat_data_begin = ART_BASE_ADDRESS + (8 * KB);  // page aligned
    uint32_t oat_data_end = ART_BASE_ADDRESS + (9 * KB);
    uint32_t oat_file_end = ART_BASE_ADDRESS + (10 * KB);
    ImageSection sections[ImageHeader::kSectionCount];
    ImageHeader image_header(image_begin,
                             image_size_,
                             sections,
                             image_roots,
                             oat_checksum,
                             oat_file_begin,
                             oat_data_begin,
                             oat_data_end,
                             oat_file_end,
                             /*boot_image_begin*/0U,
                             /*boot_image_size*/0U,
                             /*boot_oat_begin*/0U,
                             /*boot_oat_size_*/0U,
                             sizeof(void*),
                             /*compile_pic*/false,
                             /*is_pic*/false,
                             ImageHeader::kDefaultStorageMode,
                             /*data_size*/0u);
    ASSERT_TRUE(image_header.IsValid());
    ASSERT_TRUE(!image_header.IsAppImage());

    char* magic = const_cast<char*>(image_header.GetMagic());
    strcpy(magic, "");  // bad magic
    ASSERT_FALSE(image_header.IsValid());
    strcpy(magic, "art\n000");  // bad version
    ASSERT_FALSE(image_header.IsValid());
}

}  // namespace art
