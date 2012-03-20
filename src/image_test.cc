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

#include <string>
#include <vector>

#include "common_test.h"
#include "file.h"
#include "image.h"
#include "image_writer.h"
#include "oat_writer.h"
#include "signal_catcher.h"
#include "space.h"
#include "utils.h"

namespace art {

class ImageTest : public CommonTest {};

TEST_F(ImageTest, WriteRead) {
  ScratchFile tmp_oat;
  std::vector<const DexFile*> dex_files;
  dex_files.push_back(java_lang_dex_file_);
  bool success_oat = OatWriter::Create(tmp_oat.GetFile(), NULL, dex_files, 0, "", *compiler_.get());
  ASSERT_TRUE(success_oat);

  // Force all system classes into memory
  for (size_t i = 0; i < java_lang_dex_file_->NumClassDefs(); ++i) {
    const DexFile::ClassDef& class_def = java_lang_dex_file_->GetClassDef(i);
    const char* descriptor = java_lang_dex_file_->GetClassDescriptor(class_def);
    Class* klass = class_linker_->FindSystemClass(descriptor);
    EXPECT_TRUE(klass != NULL) << descriptor;
  }

  ImageWriter writer(NULL);
  ScratchFile tmp_image;
  const uintptr_t requested_image_base = 0x60000000;
  bool success_image = writer.Write(tmp_image.GetFilename(), requested_image_base,
                                    tmp_oat.GetFilename(), tmp_oat.GetFilename(),
                                    *compiler_.get());
  ASSERT_TRUE(success_image);

  {
    UniquePtr<File> file(OS::OpenFile(tmp_image.GetFilename().c_str(), false));
    ASSERT_TRUE(file.get() != NULL);
    ImageHeader image_header;
    file->ReadFully(&image_header, sizeof(image_header));
    ASSERT_TRUE(image_header.IsValid());

    Heap* heap = Runtime::Current()->GetHeap();
    ASSERT_EQ(1U, heap->GetSpaces().size());
    Space* space = heap->GetSpaces()[0];
    ASSERT_FALSE(space->IsImageSpace());
    ASSERT_TRUE(space != NULL);
    ASSERT_EQ(space, heap->GetAllocSpace());
    ASSERT_GE(sizeof(image_header) + space->Size(), static_cast<size_t>(file->Length()));
  }

  // tear down old runtime before making a new one, clearing out misc state
  delete runtime_.release();
  java_lang_dex_file_ = NULL;

  UniquePtr<const DexFile> dex(DexFile::Open(GetLibCoreDexFileName(), GetLibCoreDexFileName()));
  ASSERT_TRUE(dex.get() != NULL);

  Runtime::Options options;
  std::string image("-Ximage:");
  image.append(tmp_image.GetFilename());
  options.push_back(std::make_pair(image.c_str(), reinterpret_cast<void*>(NULL)));

  runtime_.reset(Runtime::Create(options, false));
  ASSERT_TRUE(runtime_.get() != NULL);
  class_linker_ = runtime_->GetClassLinker();

  ASSERT_TRUE(runtime_->GetJniDlsymLookupStub() != NULL);

  Heap* heap = Runtime::Current()->GetHeap();
  ASSERT_EQ(2U, heap->GetSpaces().size());
  ASSERT_TRUE(heap->GetSpaces()[0]->IsImageSpace());
  ASSERT_FALSE(heap->GetSpaces()[0]->IsAllocSpace());
  ASSERT_FALSE(heap->GetSpaces()[1]->IsImageSpace());
  ASSERT_TRUE(heap->GetSpaces()[1]->IsAllocSpace());
  ASSERT_TRUE(heap->GetImageSpace() != NULL);
  ASSERT_TRUE(heap->GetAllocSpace() != NULL);

  ImageSpace* image_space = heap->GetImageSpace();
  byte* image_begin = image_space->Begin();
  byte* image_end = image_space->End();
  CHECK_EQ(requested_image_base, reinterpret_cast<uintptr_t>(image_begin));
  for (size_t i = 0; i < dex->NumClassDefs(); ++i) {
    const DexFile::ClassDef& class_def = dex->GetClassDef(i);
    const char* descriptor = dex->GetClassDescriptor(class_def);
    Class* klass = class_linker_->FindSystemClass(descriptor);
    EXPECT_TRUE(klass != NULL) << descriptor;
    EXPECT_LT(image_begin, reinterpret_cast<byte*>(klass)) << descriptor;
    EXPECT_LT(reinterpret_cast<byte*>(klass), image_end) << descriptor;
    EXPECT_EQ(*klass->GetRawLockWordAddress(), 0);  // address should have been removed from monitor
  }
}

}  // namespace art
