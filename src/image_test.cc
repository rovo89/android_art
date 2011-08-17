// Copyright 2011 Google Inc. All Rights Reserved.

#include "common_test.h"
#include "file.h"
#include "image.h"
#include "image_writer.h"
#include "os.h"
#include "space.h"

#include "gtest/gtest.h"

namespace art {

class ImageTest : public CommonTest {};

TEST_F(ImageTest, WriteRead) {
  scoped_ptr<DexFile> libcore_dex_file(GetLibCoreDex());
  EXPECT_TRUE(libcore_dex_file.get() != NULL);

  // TODO: garbage collect before writing
  const std::vector<Space*>& spaces = Heap::GetSpaces();
  // can't currently deal with writing a space that might have pointers between spaces
  ASSERT_EQ(1U, spaces.size());
  Space* space = spaces[0];

  ImageWriter writer;
  ScratchFile tmp;
  const int image_base = 0x5000000;
  bool success = writer.Write(space, tmp.GetFilename(), reinterpret_cast<byte*>(image_base));
  ASSERT_TRUE(success);

  {
    scoped_ptr<File> file(OS::OpenFile(tmp.GetFilename(), false));
    ASSERT_TRUE(file != NULL);
    ImageHeader image_header;
    file->ReadFully(&image_header, sizeof(image_header));
    ASSERT_TRUE(image_header.IsValid());
    ASSERT_GE(sizeof(image_header) + space->Size(), static_cast<size_t>(file->Length()));
  }

  // tear down old runtime and make a new one
  delete runtime_.release();
  java_lang_dex_file_.reset(GetLibCoreDex());

  std::vector<const DexFile*> boot_class_path;
  boot_class_path.push_back(java_lang_dex_file_.get());

  Runtime::Options options;
  options.push_back(std::make_pair("bootclasspath", &boot_class_path));
  std::string boot_image("-Xbootimage:");
  boot_image.append(tmp.GetFilename());
  options.push_back(std::make_pair(boot_image.c_str(), reinterpret_cast<void*>(NULL)));

  runtime_.reset(Runtime::Create(options, false));
  ASSERT_TRUE(runtime_ != NULL);
  class_linker_ = runtime_->GetClassLinker();
}

}  // namespace art
