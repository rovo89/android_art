// Copyright 2011 Google Inc. All Rights Reserved.

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
  dex_files.push_back(java_lang_dex_file_.get());
  bool success_oat = OatWriter::Create(tmp_oat.GetFile(), NULL, dex_files, *compiler_.get());
  ASSERT_TRUE(success_oat);

  // Force all system classes into memory
  for (size_t i = 0; i < java_lang_dex_file_->NumClassDefs(); i++) {
    const DexFile::ClassDef& class_def = java_lang_dex_file_->GetClassDef(i);
    const char* descriptor = java_lang_dex_file_->GetClassDescriptor(class_def);
    Class* klass = class_linker_->FindSystemClass(descriptor);
    EXPECT_TRUE(klass != NULL) << descriptor;
  }

  ImageWriter writer(NULL);
  ScratchFile tmp_image;
  const uintptr_t requested_image_base = 0x60000000;
  bool success_image = writer.Write(tmp_image.GetFilename(), requested_image_base,
                                    std::string(tmp_oat.GetFilename()), "");
  ASSERT_TRUE(success_image);

  {
    UniquePtr<File> file(OS::OpenFile(tmp_image.GetFilename(), false));
    ASSERT_TRUE(file.get() != NULL);
    ImageHeader image_header;
    file->ReadFully(&image_header, sizeof(image_header));
    ASSERT_TRUE(image_header.IsValid());

    ASSERT_EQ(1U, Heap::GetSpaces().size());
    Space* space = Heap::GetSpaces()[0];
    ASSERT_FALSE(space->IsImageSpace());
    ASSERT_TRUE(space != NULL);
    ASSERT_GE(sizeof(image_header) + space->Size(), static_cast<size_t>(file->Length()));
  }

  // tear down old runtime before making a new one, clearing out misc state
  delete runtime_.release();

  // don't reuse java_lang_dex_file_ so we make sure we don't get
  // lucky by pointers that happen to work referencing the earlier
  // dex.
  delete java_lang_dex_file_.release();
  UniquePtr<const DexFile> dex(DexFile::Open(GetLibCoreDexFileName(), ""));
  ASSERT_TRUE(dex.get() != NULL);

  Runtime::Options options;
  std::string image("-Ximage:");
  image.append(tmp_image.GetFilename());
  options.push_back(std::make_pair(image.c_str(), reinterpret_cast<void*>(NULL)));

  runtime_.reset(Runtime::Create(options, false));
  ASSERT_TRUE(runtime_.get() != NULL);
  class_linker_ = runtime_->GetClassLinker();

  ASSERT_TRUE(runtime_->GetJniDlsymLookupStub() != NULL);

  ASSERT_EQ(2U, Heap::GetSpaces().size());
  ASSERT_TRUE(Heap::GetSpaces()[0]->IsImageSpace());
  ASSERT_FALSE(Heap::GetSpaces()[1]->IsImageSpace());

  ImageSpace* image_space = Heap::GetSpaces()[0]->AsImageSpace();
  byte* image_begin = image_space->Begin();
  byte* image_end = image_space->End();
  CHECK_EQ(requested_image_base, reinterpret_cast<uintptr_t>(image_begin));
  for (size_t i = 0; i < dex->NumClassDefs(); i++) {
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
