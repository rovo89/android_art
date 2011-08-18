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

std::string ReadFileToString(const char* file_name) {
  scoped_ptr<File> file(OS::OpenFile(file_name, false));
  CHECK(file != NULL);

  std::string contents;
  char buf[8 * KB];
  while (true) {
    int64_t n = file->Read(buf, sizeof(buf));
    CHECK_NE(-1, n);
    if (n == 0) {
        break;
    }
    contents.append(buf, n);
  }
  return contents;
}

TEST_F(ImageTest, WriteRead) {
  // TODO: Heap::CollectGarbage before writing
  const std::vector<Space*>& spaces = Heap::GetSpaces();
  // can't currently deal with writing a space that might have pointers between spaces
  ASSERT_EQ(1U, spaces.size());
  Space* space = spaces[0];

  ImageWriter writer;
  ScratchFile tmp;
  const int image_base = 0x50000000;
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

  // don't reuse java_lang_dex_file_ so we make sure we don't get
  // lucky by pointers that happen to work referencing the earlier
  // dex.
  delete java_lang_dex_file_.release();
  scoped_ptr<DexFile> dex(GetLibCoreDex());
  ASSERT_TRUE(dex != NULL);

  std::vector<const DexFile*> boot_class_path;
  boot_class_path.push_back(dex.get());

  Runtime::Options options;
  options.push_back(std::make_pair("bootclasspath", &boot_class_path));
  std::string boot_image("-Xbootimage:");
  boot_image.append(tmp.GetFilename());
  options.push_back(std::make_pair(boot_image.c_str(), reinterpret_cast<void*>(NULL)));

  runtime_.reset(Runtime::Create(options, false));
  ASSERT_TRUE(runtime_ != NULL);
  class_linker_ = runtime_->GetClassLinker();
  
  if (true) {
    const char* maps_file = "/proc/self/maps";
    std::string contents = ReadFileToString(maps_file);
    LG << maps_file << ":\n" << contents;
  }

  ASSERT_EQ(2U, Heap::GetSpaces().size());
  Space* boot_space = Heap::GetSpaces()[0];
  ASSERT_TRUE(boot_space != NULL);

  // TODO: need to rebuild ClassLinker::classes_ and ::intern_table_
  // byte* boot_base = boot_space->GetBase();
  // byte* boot_limit = boot_space->GetLimit();
  for (size_t i = 0; i < dex->NumClassDefs(); i++) {
    const DexFile::ClassDef class_def = dex->GetClassDef(i);
    const char* descriptor = dex->GetClassDescriptor(class_def);
    Class* klass = class_linker_->FindSystemClass(descriptor);
    EXPECT_TRUE(klass != NULL);
    // EXPECT_LT(boot_base, reinterpret_cast<byte*>(klass));
    // EXPECT_LT(reinterpret_cast<byte*>(klass), boot_limit);
  }
}

}  // namespace art
