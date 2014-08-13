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
#include "common_compiler_test.h"
#include "elf_fixup.h"
#include "gc/space/image_space.h"
#include "image_writer.h"
#include "lock_word.h"
#include "mirror/object-inl.h"
#include "oat_writer.h"
#include "scoped_thread_state_change.h"
#include "signal_catcher.h"
#include "utils.h"
#include "vector_output_stream.h"

namespace art {

class ImageTest : public CommonCompilerTest {
 protected:
  virtual void SetUp() {
    ReserveImageSpace();
    CommonCompilerTest::SetUp();
  }
};

TEST_F(ImageTest, WriteRead) {
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

  {
    {
      jobject class_loader = NULL;
      ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
      TimingLogger timings("ImageTest::WriteRead", false, false);
      TimingLogger::ScopedTiming t("CompileAll", &timings);
      if (kUsePortableCompiler) {
        // TODO: we disable this for portable so the test executes in a reasonable amount of time.
        //       We shouldn't need to do this.
        compiler_options_->SetCompilerFilter(CompilerOptions::kInterpretOnly);
      }
      for (const DexFile* dex_file : class_linker->GetBootClassPath()) {
        dex_file->EnableWrite();
      }
      compiler_driver_->CompileAll(class_loader, class_linker->GetBootClassPath(), &timings);

      t.NewTiming("WriteElf");
      ScopedObjectAccess soa(Thread::Current());
      SafeMap<std::string, std::string> key_value_store;
      OatWriter oat_writer(class_linker->GetBootClassPath(), 0, 0, 0, compiler_driver_.get(), &timings,
                           &key_value_store);
      bool success = compiler_driver_->WriteElf(GetTestAndroidRoot(),
                                                !kIsTargetBuild,
                                                class_linker->GetBootClassPath(),
                                                &oat_writer,
                                                oat_file.GetFile());
      ASSERT_TRUE(success);
    }
  }
  // Workound bug that mcld::Linker::emit closes oat_file by reopening as dup_oat.
  std::unique_ptr<File> dup_oat(OS::OpenFileReadWrite(oat_file.GetFilename().c_str()));
  ASSERT_TRUE(dup_oat.get() != NULL);

  const uintptr_t requested_image_base = ART_BASE_ADDRESS;
  {
    ImageWriter writer(*compiler_driver_.get());
    bool success_image = writer.Write(image_file.GetFilename(), requested_image_base,
                                      dup_oat->GetPath(), dup_oat->GetPath());
    ASSERT_TRUE(success_image);
    bool success_fixup = ElfFixup::Fixup(dup_oat.get(), writer.GetOatDataBegin());
    ASSERT_TRUE(success_fixup);
  }

  {
    std::unique_ptr<File> file(OS::OpenFileForReading(image_file.GetFilename().c_str()));
    ASSERT_TRUE(file.get() != NULL);
    ImageHeader image_header;
    file->ReadFully(&image_header, sizeof(image_header));
    ASSERT_TRUE(image_header.IsValid());
    ASSERT_GE(image_header.GetImageBitmapOffset(), sizeof(image_header));
    ASSERT_NE(0U, image_header.GetImageBitmapSize());

    gc::Heap* heap = Runtime::Current()->GetHeap();
    ASSERT_TRUE(!heap->GetContinuousSpaces().empty());
    gc::space::ContinuousSpace* space = heap->GetNonMovingSpace();
    ASSERT_FALSE(space->IsImageSpace());
    ASSERT_TRUE(space != NULL);
    ASSERT_TRUE(space->IsMallocSpace());
    ASSERT_GE(sizeof(image_header) + space->Size(), static_cast<size_t>(file->GetLength()));
  }

  ASSERT_TRUE(compiler_driver_->GetImageClasses() != NULL);
  std::set<std::string> image_classes(*compiler_driver_->GetImageClasses());

  // Need to delete the compiler since it has worker threads which are attached to runtime.
  compiler_driver_.reset();

  // Tear down old runtime before making a new one, clearing out misc state.
  runtime_.reset();
  java_lang_dex_file_ = NULL;

  std::unique_ptr<const DexFile> dex(LoadExpectSingleDexFile(GetLibCoreDexFileName().c_str()));

  // Remove the reservation of the memory for use to load the image.
  UnreserveImageSpace();

  RuntimeOptions options;
  std::string image("-Ximage:");
  image.append(image_location.GetFilename());
  options.push_back(std::make_pair(image.c_str(), reinterpret_cast<void*>(NULL)));
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
  ASSERT_TRUE(runtime_.get() != NULL);
  class_linker_ = runtime_->GetClassLinker();

  gc::Heap* heap = Runtime::Current()->GetHeap();
  ASSERT_TRUE(heap->HasImageSpace());
  ASSERT_TRUE(heap->GetNonMovingSpace()->IsMallocSpace());

  gc::space::ImageSpace* image_space = heap->GetImageSpace();
  image_space->VerifyImageAllocations();
  byte* image_begin = image_space->Begin();
  byte* image_end = image_space->End();
  CHECK_EQ(requested_image_base, reinterpret_cast<uintptr_t>(image_begin));
  for (size_t i = 0; i < dex->NumClassDefs(); ++i) {
    const DexFile::ClassDef& class_def = dex->GetClassDef(i);
    const char* descriptor = dex->GetClassDescriptor(class_def);
    mirror::Class* klass = class_linker_->FindSystemClass(soa.Self(), descriptor);
    EXPECT_TRUE(klass != nullptr) << descriptor;
    if (image_classes.find(descriptor) != image_classes.end()) {
      // Image classes should be located inside the image.
      EXPECT_LT(image_begin, reinterpret_cast<byte*>(klass)) << descriptor;
      EXPECT_LT(reinterpret_cast<byte*>(klass), image_end) << descriptor;
    } else {
      EXPECT_TRUE(reinterpret_cast<byte*>(klass) >= image_end ||
                  reinterpret_cast<byte*>(klass) < image_begin) << descriptor;
    }
    EXPECT_TRUE(Monitor::IsValidLockWord(klass->GetLockWord(false)));
  }

  image_file.Unlink();
  oat_file.Unlink();
  int rmdir_result = rmdir(image_dir.c_str());
  CHECK_EQ(0, rmdir_result);
}

TEST_F(ImageTest, ImageHeaderIsValid) {
    uint32_t image_begin = ART_BASE_ADDRESS;
    uint32_t image_size_ = 16 * KB;
    uint32_t image_bitmap_offset = 0;
    uint32_t image_bitmap_size = 0;
    uint32_t image_roots = ART_BASE_ADDRESS + (1 * KB);
    uint32_t oat_checksum = 0;
    uint32_t oat_file_begin = ART_BASE_ADDRESS + (4 * KB);  // page aligned
    uint32_t oat_data_begin = ART_BASE_ADDRESS + (8 * KB);  // page aligned
    uint32_t oat_data_end = ART_BASE_ADDRESS + (9 * KB);
    uint32_t oat_file_end = ART_BASE_ADDRESS + (10 * KB);
    ImageHeader image_header(image_begin,
                             image_size_,
                             image_bitmap_offset,
                             image_bitmap_size,
                             image_roots,
                             oat_checksum,
                             oat_file_begin,
                             oat_data_begin,
                             oat_data_end,
                             oat_file_end);
    ASSERT_TRUE(image_header.IsValid());

    char* magic = const_cast<char*>(image_header.GetMagic());
    strcpy(magic, "");  // bad magic
    ASSERT_FALSE(image_header.IsValid());
    strcpy(magic, "art\n000");  // bad version
    ASSERT_FALSE(image_header.IsValid());
}

}  // namespace art
