// Copyright 2011 Google Inc. All Rights Reserved.

#include "common_test.h"
#include "image_writer.h"

#include "gtest/gtest.h"

namespace art {

class ImageTest : public RuntimeTest {};

TEST_F(ImageTest, WriteRead) {
  UseLibCoreDex();
  scoped_ptr<DexFile> libcore_dex_file(GetLibCoreDex());
  EXPECT_TRUE(libcore_dex_file.get() != NULL);

  // TODO garbage collect before writing
  const std::vector<Space*>& spaces = Heap::GetSpaces();
  // can't currently deal with writing a space that might have pointers between spaces
  CHECK_EQ(1U, spaces.size());

  ImageWriter writer;
  ScratchFile tmp;
  bool success = writer.Write(spaces[0], tmp.GetFilename(), reinterpret_cast<byte*>(0x5000000));
  EXPECT_TRUE(success);
}

}  // namespace art
