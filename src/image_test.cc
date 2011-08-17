// Copyright 2011 Google Inc. All Rights Reserved.

#include "common_test.h"
#include "image_writer.h"

#include "gtest/gtest.h"

namespace art {

class ImageTest : public CommonTest {};

TEST_F(ImageTest, WriteRead) {
  scoped_ptr<DexFile> libcore_dex_file(GetLibCoreDex());
  EXPECT_TRUE(libcore_dex_file.get() != NULL);

  // TODO garbage collect before writing
  const std::vector<Space*>& spaces = Heap::GetSpaces();
  // can't currently deal with writing a space that might have pointers between spaces
  CHECK_EQ(1U, spaces.size());

  ImageWriter writer;
  ScratchFile tmp;
  const int image_base = 0x5000000;
  bool success = writer.Write(spaces[0], tmp.GetFilename(), reinterpret_cast<byte*>(image_base));
  EXPECT_TRUE(success);

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
