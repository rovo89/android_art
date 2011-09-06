// Copyright 2011 Google Inc. All Rights Reserved.

#include <string>
#include <vector>

#include "common_test.h"
#include "file.h"
#include "image.h"
#include "image_writer.h"
#include "signal_catcher.h"
#include "space.h"
#include "utils.h"

namespace art {

class ImageTest : public CommonTest {};

TEST_F(ImageTest, WriteRead) {
  // TODO: remove the touching of classes, call Compiler instead
  for (size_t i = 0; i < java_lang_dex_file_->NumClassDefs(); i++) {
    const DexFile::ClassDef& class_def = java_lang_dex_file_->GetClassDef(i);
    const char* descriptor = java_lang_dex_file_->GetClassDescriptor(class_def);
    Class* klass = class_linker_->FindSystemClass(descriptor);
    ASSERT_TRUE(klass != NULL) << descriptor;
  }

  ImageWriter writer;
  ScratchFile tmp;
  const uintptr_t image_base = 0x50000000;
  bool success = writer.Write(tmp.GetFilename(), image_base);
  ASSERT_TRUE(success);

  {
    UniquePtr<File> file(OS::OpenFile(tmp.GetFilename(), false));
    ASSERT_TRUE(file.get() != NULL);
    ImageHeader image_header;
    file->ReadFully(&image_header, sizeof(image_header));
    ASSERT_TRUE(image_header.IsValid());

    ASSERT_EQ(1U, Heap::GetSpaces().size());
    Space* space = Heap::GetSpaces()[0];
    ASSERT_TRUE(space != NULL);
    ASSERT_GE(sizeof(image_header) + space->Size(), static_cast<size_t>(file->Length()));
  }

  // tear down old runtime before making a new one, clearing out misc state
  delete runtime_.release();

  // don't reuse java_lang_dex_file_ so we make sure we don't get
  // lucky by pointers that happen to work referencing the earlier
  // dex.
  delete java_lang_dex_file_.release();
  UniquePtr<const DexFile> dex(GetLibCoreDex());
  ASSERT_TRUE(dex.get() != NULL);

  std::vector<const DexFile*> boot_class_path;
  boot_class_path.push_back(dex.get());

  Runtime::Options options;
  options.push_back(std::make_pair("bootclasspath", &boot_class_path));
  std::string boot_image("-Xbootimage:");
  boot_image.append(tmp.GetFilename());
  options.push_back(std::make_pair(boot_image.c_str(), reinterpret_cast<void*>(NULL)));

  runtime_.reset(Runtime::Create(options, false));
  ASSERT_TRUE(runtime_.get() != NULL);
  class_linker_ = runtime_->GetClassLinker();

  ASSERT_EQ(2U, Heap::GetSpaces().size());
  Space* boot_space = Heap::GetBootSpace();
  ASSERT_TRUE(boot_space != NULL);

  // enable to display maps to debug boot_base and boot_limit checking problems below
  // TODO: why does this dump show two attached threads?
  if (true) {
    SignalCatcher::HandleSigQuit();
  }

  byte* boot_base = boot_space->GetBase();
  byte* boot_limit = boot_space->GetLimit();
  for (size_t i = 0; i < dex->NumClassDefs(); i++) {
    const DexFile::ClassDef& class_def = dex->GetClassDef(i);
    const char* descriptor = dex->GetClassDescriptor(class_def);
    Class* klass = class_linker_->FindSystemClass(descriptor);
    EXPECT_TRUE(klass != NULL) << descriptor;
    EXPECT_LT(boot_base, reinterpret_cast<byte*>(klass)) << descriptor;
    EXPECT_LT(reinterpret_cast<byte*>(klass), boot_limit) << descriptor;
    EXPECT_TRUE(klass->GetMonitor() == NULL);  // address should have been removed from monitor
  }
}

}  // namespace art
