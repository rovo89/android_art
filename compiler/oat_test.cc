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

#include "arch/instruction_set_features.h"
#include "art_method-inl.h"
#include "base/unix_file/fd_file.h"
#include "class_linker.h"
#include "common_compiler_test.h"
#include "compiled_method.h"
#include "compiler.h"
#include "debug/method_debug_info.h"
#include "dex/quick/dex_file_to_method_inliner_map.h"
#include "dex/quick_compiler_callbacks.h"
#include "dex/verification_results.h"
#include "driver/compiler_driver.h"
#include "driver/compiler_options.h"
#include "elf_writer.h"
#include "elf_writer_quick.h"
#include "entrypoints/quick/quick_entrypoints.h"
#include "linker/multi_oat_relative_patcher.h"
#include "linker/vector_output_stream.h"
#include "mirror/class-inl.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "oat_file-inl.h"
#include "oat_writer.h"
#include "scoped_thread_state_change.h"
#include "utils/test_dex_file_builder.h"

namespace art {

NO_RETURN static void Usage(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::string error;
  StringAppendV(&error, fmt, ap);
  LOG(FATAL) << error;
  va_end(ap);
  UNREACHABLE();
}

class OatTest : public CommonCompilerTest {
 protected:
  static const bool kCompile = false;  // DISABLED_ due to the time to compile libcore

  void CheckMethod(ArtMethod* method,
                   const OatFile::OatMethod& oat_method,
                   const DexFile& dex_file)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    const CompiledMethod* compiled_method =
        compiler_driver_->GetCompiledMethod(MethodReference(&dex_file,
                                                            method->GetDexMethodIndex()));

    if (compiled_method == nullptr) {
      EXPECT_TRUE(oat_method.GetQuickCode() == nullptr) << PrettyMethod(method) << " "
                                                        << oat_method.GetQuickCode();
      EXPECT_EQ(oat_method.GetFrameSizeInBytes(), 0U);
      EXPECT_EQ(oat_method.GetCoreSpillMask(), 0U);
      EXPECT_EQ(oat_method.GetFpSpillMask(), 0U);
    } else {
      const void* quick_oat_code = oat_method.GetQuickCode();
      EXPECT_TRUE(quick_oat_code != nullptr) << PrettyMethod(method);
      EXPECT_EQ(oat_method.GetFrameSizeInBytes(), compiled_method->GetFrameSizeInBytes());
      EXPECT_EQ(oat_method.GetCoreSpillMask(), compiled_method->GetCoreSpillMask());
      EXPECT_EQ(oat_method.GetFpSpillMask(), compiled_method->GetFpSpillMask());
      uintptr_t oat_code_aligned = RoundDown(reinterpret_cast<uintptr_t>(quick_oat_code), 2);
      quick_oat_code = reinterpret_cast<const void*>(oat_code_aligned);
      ArrayRef<const uint8_t> quick_code = compiled_method->GetQuickCode();
      EXPECT_FALSE(quick_code.empty());
      size_t code_size = quick_code.size() * sizeof(quick_code[0]);
      EXPECT_EQ(0, memcmp(quick_oat_code, &quick_code[0], code_size))
          << PrettyMethod(method) << " " << code_size;
      CHECK_EQ(0, memcmp(quick_oat_code, &quick_code[0], code_size));
    }
  }

  void SetupCompiler(Compiler::Kind compiler_kind,
                     InstructionSet insn_set,
                     const std::vector<std::string>& compiler_options,
                     /*out*/std::string* error_msg) {
    ASSERT_TRUE(error_msg != nullptr);
    insn_features_.reset(InstructionSetFeatures::FromVariant(insn_set, "default", error_msg));
    ASSERT_TRUE(insn_features_ != nullptr) << error_msg;
    compiler_options_.reset(new CompilerOptions);
    for (const std::string& option : compiler_options) {
      compiler_options_->ParseCompilerOption(option, Usage);
    }
    verification_results_.reset(new VerificationResults(compiler_options_.get()));
    method_inliner_map_.reset(new DexFileToMethodInlinerMap);
    callbacks_.reset(new QuickCompilerCallbacks(verification_results_.get(),
                                                method_inliner_map_.get(),
                                                CompilerCallbacks::CallbackMode::kCompileApp));
    Runtime::Current()->SetCompilerCallbacks(callbacks_.get());
    timer_.reset(new CumulativeLogger("Compilation times"));
    compiler_driver_.reset(new CompilerDriver(compiler_options_.get(),
                                              verification_results_.get(),
                                              method_inliner_map_.get(),
                                              compiler_kind,
                                              insn_set,
                                              insn_features_.get(),
                                              /* boot_image */ false,
                                              /* app_image */ false,
                                              /* image_classes */ nullptr,
                                              /* compiled_classes */ nullptr,
                                              /* compiled_methods */ nullptr,
                                              /* thread_count */ 2,
                                              /* dump_stats */ true,
                                              /* dump_passes */ true,
                                              timer_.get(),
                                              /* swap_fd */ -1,
                                              /* profile_compilation_info */ nullptr));
  }

  bool WriteElf(File* file,
                const std::vector<const DexFile*>& dex_files,
                SafeMap<std::string, std::string>& key_value_store,
                bool verify) {
    TimingLogger timings("WriteElf", false, false);
    OatWriter oat_writer(/*compiling_boot_image*/false, &timings);
    for (const DexFile* dex_file : dex_files) {
      ArrayRef<const uint8_t> raw_dex_file(
          reinterpret_cast<const uint8_t*>(&dex_file->GetHeader()),
          dex_file->GetHeader().file_size_);
      if (!oat_writer.AddRawDexFileSource(raw_dex_file,
                                          dex_file->GetLocation().c_str(),
                                          dex_file->GetLocationChecksum())) {
        return false;
      }
    }
    return DoWriteElf(file, oat_writer, key_value_store, verify);
  }

  bool WriteElf(File* file,
                const std::vector<const char*>& dex_filenames,
                SafeMap<std::string, std::string>& key_value_store,
                bool verify) {
    TimingLogger timings("WriteElf", false, false);
    OatWriter oat_writer(/*compiling_boot_image*/false, &timings);
    for (const char* dex_filename : dex_filenames) {
      if (!oat_writer.AddDexFileSource(dex_filename, dex_filename)) {
        return false;
      }
    }
    return DoWriteElf(file, oat_writer, key_value_store, verify);
  }

  bool WriteElf(File* file,
                ScopedFd&& zip_fd,
                const char* location,
                SafeMap<std::string, std::string>& key_value_store,
                bool verify) {
    TimingLogger timings("WriteElf", false, false);
    OatWriter oat_writer(/*compiling_boot_image*/false, &timings);
    if (!oat_writer.AddZippedDexFilesSource(std::move(zip_fd), location)) {
      return false;
    }
    return DoWriteElf(file, oat_writer, key_value_store, verify);
  }

  bool DoWriteElf(File* file,
                  OatWriter& oat_writer,
                  SafeMap<std::string, std::string>& key_value_store,
                  bool verify) {
    std::unique_ptr<ElfWriter> elf_writer = CreateElfWriterQuick(
        compiler_driver_->GetInstructionSet(),
        compiler_driver_->GetInstructionSetFeatures(),
        &compiler_driver_->GetCompilerOptions(),
        file);
    elf_writer->Start();
    OutputStream* rodata = elf_writer->StartRoData();
    std::unique_ptr<MemMap> opened_dex_files_map;
    std::vector<std::unique_ptr<const DexFile>> opened_dex_files;
    if (!oat_writer.WriteAndOpenDexFiles(rodata,
                                         file,
                                         compiler_driver_->GetInstructionSet(),
                                         compiler_driver_->GetInstructionSetFeatures(),
                                         &key_value_store,
                                         verify,
                                         &opened_dex_files_map,
                                         &opened_dex_files)) {
      return false;
    }
    Runtime* runtime = Runtime::Current();
    ClassLinker* const class_linker = runtime->GetClassLinker();
    std::vector<const DexFile*> dex_files;
    for (const std::unique_ptr<const DexFile>& dex_file : opened_dex_files) {
      dex_files.push_back(dex_file.get());
      ScopedObjectAccess soa(Thread::Current());
      class_linker->RegisterDexFile(*dex_file, nullptr);
    }
    linker::MultiOatRelativePatcher patcher(compiler_driver_->GetInstructionSet(),
                                            instruction_set_features_.get());
    oat_writer.PrepareLayout(compiler_driver_.get(), nullptr, dex_files, &patcher);
    size_t rodata_size = oat_writer.GetOatHeader().GetExecutableOffset();
    size_t text_size = oat_writer.GetSize() - rodata_size;
    elf_writer->SetLoadedSectionSizes(rodata_size, text_size, oat_writer.GetBssSize());

    if (!oat_writer.WriteRodata(rodata)) {
      return false;
    }
    elf_writer->EndRoData(rodata);

    OutputStream* text = elf_writer->StartText();
    if (!oat_writer.WriteCode(text)) {
      return false;
    }
    elf_writer->EndText(text);

    if (!oat_writer.WriteHeader(elf_writer->GetStream(), 42U, 4096U, 0)) {
      return false;
    }

    elf_writer->WriteDynamicSection();
    elf_writer->WriteDebugInfo(oat_writer.GetMethodDebugInfo());
    elf_writer->WritePatchLocations(oat_writer.GetAbsolutePatchLocations());

    return elf_writer->End();
  }

  void TestDexFileInput(bool verify, bool low_4gb);
  void TestZipFileInput(bool verify);

  std::unique_ptr<const InstructionSetFeatures> insn_features_;
  std::unique_ptr<QuickCompilerCallbacks> callbacks_;
};

class ZipBuilder {
 public:
  explicit ZipBuilder(File* zip_file) : zip_file_(zip_file) { }

  bool AddFile(const char* location, const void* data, size_t size) {
    off_t offset = lseek(zip_file_->Fd(), 0, SEEK_CUR);
    if (offset == static_cast<off_t>(-1)) {
      return false;
    }

    ZipFileHeader file_header;
    file_header.crc32 = crc32(0u, reinterpret_cast<const Bytef*>(data), size);
    file_header.compressed_size = size;
    file_header.uncompressed_size = size;
    file_header.filename_length = strlen(location);

    if (!zip_file_->WriteFully(&file_header, sizeof(file_header)) ||
        !zip_file_->WriteFully(location, file_header.filename_length) ||
        !zip_file_->WriteFully(data, size)) {
      return false;
    }

    CentralDirectoryFileHeader cdfh;
    cdfh.crc32 = file_header.crc32;
    cdfh.compressed_size = size;
    cdfh.uncompressed_size = size;
    cdfh.filename_length = file_header.filename_length;
    cdfh.relative_offset_of_local_file_header = offset;
    file_data_.push_back(FileData { cdfh, location });
    return true;
  }

  bool Finish() {
    off_t offset = lseek(zip_file_->Fd(), 0, SEEK_CUR);
    if (offset == static_cast<off_t>(-1)) {
      return false;
    }

    size_t central_directory_size = 0u;
    for (const FileData& file_data : file_data_) {
      if (!zip_file_->WriteFully(&file_data.cdfh, sizeof(file_data.cdfh)) ||
          !zip_file_->WriteFully(file_data.location, file_data.cdfh.filename_length)) {
        return false;
      }
      central_directory_size += sizeof(file_data.cdfh) + file_data.cdfh.filename_length;
    }
    EndOfCentralDirectoryRecord eocd_record;
    eocd_record.number_of_central_directory_records_on_this_disk = file_data_.size();
    eocd_record.total_number_of_central_directory_records = file_data_.size();
    eocd_record.size_of_central_directory = central_directory_size;
    eocd_record.offset_of_start_of_central_directory = offset;
    return
        zip_file_->WriteFully(&eocd_record, sizeof(eocd_record)) &&
        zip_file_->Flush() == 0;
  }

 private:
  struct PACKED(1) ZipFileHeader {
    uint32_t signature = 0x04034b50;
    uint16_t version_needed_to_extract = 10;
    uint16_t general_purpose_bit_flag = 0;
    uint16_t compression_method = 0;            // 0 = store only.
    uint16_t file_last_modification_time = 0u;
    uint16_t file_last_modification_date = 0u;
    uint32_t crc32;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint16_t filename_length;
    uint16_t extra_field_length = 0u;           // No extra fields.
  };

  struct PACKED(1) CentralDirectoryFileHeader {
    uint32_t signature = 0x02014b50;
    uint16_t version_made_by = 10;
    uint16_t version_needed_to_extract = 10;
    uint16_t general_purpose_bit_flag = 0;
    uint16_t compression_method = 0;            // 0 = store only.
    uint16_t file_last_modification_time = 0u;
    uint16_t file_last_modification_date = 0u;
    uint32_t crc32;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint16_t filename_length;
    uint16_t extra_field_length = 0u;           // No extra fields.
    uint16_t file_comment_length = 0u;          // No file comment.
    uint16_t disk_number_where_file_starts = 0u;
    uint16_t internal_file_attributes = 0u;
    uint32_t external_file_attributes = 0u;
    uint32_t relative_offset_of_local_file_header;
  };

  struct PACKED(1) EndOfCentralDirectoryRecord {
    uint32_t signature = 0x06054b50;
    uint16_t number_of_this_disk = 0u;
    uint16_t disk_where_central_directory_starts = 0u;
    uint16_t number_of_central_directory_records_on_this_disk;
    uint16_t total_number_of_central_directory_records;
    uint32_t size_of_central_directory;
    uint32_t offset_of_start_of_central_directory;
    uint16_t comment_length = 0u;               // No file comment.
  };

  struct FileData {
    CentralDirectoryFileHeader cdfh;
    const char* location;
  };

  File* zip_file_;
  std::vector<FileData> file_data_;
};

TEST_F(OatTest, WriteRead) {
  TimingLogger timings("OatTest::WriteRead", false, false);
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();

  // TODO: make selectable.
  Compiler::Kind compiler_kind = Compiler::kQuick;
  InstructionSet insn_set = kIsTargetBuild ? kThumb2 : kX86;
  std::string error_msg;
  SetupCompiler(compiler_kind, insn_set, std::vector<std::string>(), /*out*/ &error_msg);

  jobject class_loader = nullptr;
  if (kCompile) {
    TimingLogger timings2("OatTest::WriteRead", false, false);
    compiler_driver_->SetDexFilesForOatFile(class_linker->GetBootClassPath());
    compiler_driver_->CompileAll(class_loader, class_linker->GetBootClassPath(), &timings2);
  }

  ScratchFile tmp;
  SafeMap<std::string, std::string> key_value_store;
  key_value_store.Put(OatHeader::kImageLocationKey, "lue.art");
  bool success = WriteElf(tmp.GetFile(), class_linker->GetBootClassPath(), key_value_store, false);
  ASSERT_TRUE(success);

  if (kCompile) {  // OatWriter strips the code, regenerate to compare
    compiler_driver_->CompileAll(class_loader, class_linker->GetBootClassPath(), &timings);
  }
  std::unique_ptr<OatFile> oat_file(OatFile::Open(tmp.GetFilename(),
                                                  tmp.GetFilename(),
                                                  nullptr,
                                                  nullptr,
                                                  false,
                                                  /*low_4gb*/true,
                                                  nullptr,
                                                  &error_msg));
  ASSERT_TRUE(oat_file.get() != nullptr) << error_msg;
  const OatHeader& oat_header = oat_file->GetOatHeader();
  ASSERT_TRUE(oat_header.IsValid());
  ASSERT_EQ(class_linker->GetBootClassPath().size(), oat_header.GetDexFileCount());  // core
  ASSERT_EQ(42U, oat_header.GetImageFileLocationOatChecksum());
  ASSERT_EQ(4096U, oat_header.GetImageFileLocationOatDataBegin());
  ASSERT_EQ("lue.art", std::string(oat_header.GetStoreValueByKey(OatHeader::kImageLocationKey)));

  ASSERT_TRUE(java_lang_dex_file_ != nullptr);
  const DexFile& dex_file = *java_lang_dex_file_;
  uint32_t dex_file_checksum = dex_file.GetLocationChecksum();
  const OatFile::OatDexFile* oat_dex_file = oat_file->GetOatDexFile(dex_file.GetLocation().c_str(),
                                                                    &dex_file_checksum);
  ASSERT_TRUE(oat_dex_file != nullptr);
  CHECK_EQ(dex_file.GetLocationChecksum(), oat_dex_file->GetDexFileLocationChecksum());
  ScopedObjectAccess soa(Thread::Current());
  auto pointer_size = class_linker->GetImagePointerSize();
  for (size_t i = 0; i < dex_file.NumClassDefs(); i++) {
    const DexFile::ClassDef& class_def = dex_file.GetClassDef(i);
    const uint8_t* class_data = dex_file.GetClassData(class_def);

    size_t num_virtual_methods = 0;
    if (class_data != nullptr) {
      ClassDataItemIterator it(dex_file, class_data);
      num_virtual_methods = it.NumVirtualMethods();
    }

    const char* descriptor = dex_file.GetClassDescriptor(class_def);
    mirror::Class* klass = class_linker->FindClass(soa.Self(),
                                                   descriptor,
                                                   ScopedNullHandle<mirror::ClassLoader>());

    const OatFile::OatClass oat_class = oat_dex_file->GetOatClass(i);
    CHECK_EQ(mirror::Class::Status::kStatusNotReady, oat_class.GetStatus()) << descriptor;
    CHECK_EQ(kCompile ? OatClassType::kOatClassAllCompiled : OatClassType::kOatClassNoneCompiled,
             oat_class.GetType()) << descriptor;

    size_t method_index = 0;
    for (auto& m : klass->GetDirectMethods(pointer_size)) {
      CheckMethod(&m, oat_class.GetOatMethod(method_index), dex_file);
      ++method_index;
    }
    size_t visited_virtuals = 0;
    // TODO We should also check copied methods in this test.
    for (auto& m : klass->GetDeclaredVirtualMethods(pointer_size)) {
      if (!klass->IsInterface()) {
        EXPECT_FALSE(m.IsCopied());
      }
      CheckMethod(&m, oat_class.GetOatMethod(method_index), dex_file);
      ++method_index;
      ++visited_virtuals;
    }
    EXPECT_EQ(visited_virtuals, num_virtual_methods);
  }
}

TEST_F(OatTest, OatHeaderSizeCheck) {
  // If this test is failing and you have to update these constants,
  // it is time to update OatHeader::kOatVersion
  EXPECT_EQ(72U, sizeof(OatHeader));
  EXPECT_EQ(4U, sizeof(OatMethodOffsets));
  EXPECT_EQ(20U, sizeof(OatQuickMethodHeader));
  EXPECT_EQ(132 * GetInstructionSetPointerSize(kRuntimeISA), sizeof(QuickEntryPoints));
}

TEST_F(OatTest, OatHeaderIsValid) {
  InstructionSet insn_set = kX86;
  std::string error_msg;
  std::unique_ptr<const InstructionSetFeatures> insn_features(
    InstructionSetFeatures::FromVariant(insn_set, "default", &error_msg));
  ASSERT_TRUE(insn_features.get() != nullptr) << error_msg;
  std::unique_ptr<OatHeader> oat_header(OatHeader::Create(insn_set,
                                                          insn_features.get(),
                                                          0u,
                                                          nullptr));
  ASSERT_NE(oat_header.get(), nullptr);
  ASSERT_TRUE(oat_header->IsValid());

  char* magic = const_cast<char*>(oat_header->GetMagic());
  strcpy(magic, "");  // bad magic
  ASSERT_FALSE(oat_header->IsValid());
  strcpy(magic, "oat\n000");  // bad version
  ASSERT_FALSE(oat_header->IsValid());
}

TEST_F(OatTest, EmptyTextSection) {
  TimingLogger timings("OatTest::EmptyTextSection", false, false);

  // TODO: make selectable.
  Compiler::Kind compiler_kind = Compiler::kQuick;
  InstructionSet insn_set = kRuntimeISA;
  if (insn_set == kArm) insn_set = kThumb2;
  std::string error_msg;
  std::vector<std::string> compiler_options;
  compiler_options.push_back("--compiler-filter=verify-at-runtime");
  SetupCompiler(compiler_kind, insn_set, compiler_options, /*out*/ &error_msg);

  jobject class_loader;
  {
    ScopedObjectAccess soa(Thread::Current());
    class_loader = LoadDex("Main");
  }
  ASSERT_TRUE(class_loader != nullptr);
  std::vector<const DexFile*> dex_files = GetDexFiles(class_loader);
  ASSERT_TRUE(!dex_files.empty());

  ClassLinker* const class_linker = Runtime::Current()->GetClassLinker();
  for (const DexFile* dex_file : dex_files) {
    ScopedObjectAccess soa(Thread::Current());
    class_linker->RegisterDexFile(*dex_file, soa.Decode<mirror::ClassLoader*>(class_loader));
  }
  compiler_driver_->SetDexFilesForOatFile(dex_files);
  compiler_driver_->CompileAll(class_loader, dex_files, &timings);

  ScratchFile tmp;
  SafeMap<std::string, std::string> key_value_store;
  key_value_store.Put(OatHeader::kImageLocationKey, "test.art");
  bool success = WriteElf(tmp.GetFile(), dex_files, key_value_store, false);
  ASSERT_TRUE(success);

  std::unique_ptr<OatFile> oat_file(OatFile::Open(tmp.GetFilename(),
                                                  tmp.GetFilename(),
                                                  nullptr,
                                                  nullptr,
                                                  false,
                                                  /*low_4gb*/false,
                                                  nullptr,
                                                  &error_msg));
  ASSERT_TRUE(oat_file != nullptr);
  EXPECT_LT(static_cast<size_t>(oat_file->Size()), static_cast<size_t>(tmp.GetFile()->GetLength()));
}

static void MaybeModifyDexFileToFail(bool verify, std::unique_ptr<const DexFile>& data) {
  // If in verify mode (= fail the verifier mode), make sure we fail early. We'll fail already
  // because of the missing map, but that may lead to out of bounds reads.
  if (verify) {
    const_cast<DexFile::Header*>(&data->GetHeader())->checksum_++;
  }
}

void OatTest::TestDexFileInput(bool verify, bool low_4gb) {
  TimingLogger timings("OatTest::DexFileInput", false, false);

  std::vector<const char*> input_filenames;

  ScratchFile dex_file1;
  TestDexFileBuilder builder1;
  builder1.AddField("Lsome.TestClass;", "int", "someField");
  builder1.AddMethod("Lsome.TestClass;", "()I", "foo");
  std::unique_ptr<const DexFile> dex_file1_data = builder1.Build(dex_file1.GetFilename());

  MaybeModifyDexFileToFail(verify, dex_file1_data);

  bool success = dex_file1.GetFile()->WriteFully(&dex_file1_data->GetHeader(),
                                                 dex_file1_data->GetHeader().file_size_);
  ASSERT_TRUE(success);
  success = dex_file1.GetFile()->Flush() == 0;
  ASSERT_TRUE(success);
  input_filenames.push_back(dex_file1.GetFilename().c_str());

  ScratchFile dex_file2;
  TestDexFileBuilder builder2;
  builder2.AddField("Land.AnotherTestClass;", "boolean", "someOtherField");
  builder2.AddMethod("Land.AnotherTestClass;", "()J", "bar");
  std::unique_ptr<const DexFile> dex_file2_data = builder2.Build(dex_file2.GetFilename());

  MaybeModifyDexFileToFail(verify, dex_file2_data);

  success = dex_file2.GetFile()->WriteFully(&dex_file2_data->GetHeader(),
                                            dex_file2_data->GetHeader().file_size_);
  ASSERT_TRUE(success);
  success = dex_file2.GetFile()->Flush() == 0;
  ASSERT_TRUE(success);
  input_filenames.push_back(dex_file2.GetFilename().c_str());

  ScratchFile oat_file;
  SafeMap<std::string, std::string> key_value_store;
  key_value_store.Put(OatHeader::kImageLocationKey, "test.art");
  success = WriteElf(oat_file.GetFile(), input_filenames, key_value_store, verify);

  // In verify mode, we expect failure.
  if (verify) {
    ASSERT_FALSE(success);
    return;
  }

  ASSERT_TRUE(success);

  std::string error_msg;
  std::unique_ptr<OatFile> opened_oat_file(OatFile::Open(oat_file.GetFilename(),
                                                         oat_file.GetFilename(),
                                                         nullptr,
                                                         nullptr,
                                                         false,
                                                         low_4gb,
                                                         nullptr,
                                                         &error_msg));
  if (low_4gb) {
    uintptr_t begin = reinterpret_cast<uintptr_t>(opened_oat_file->Begin());
    EXPECT_EQ(begin, static_cast<uint32_t>(begin));
  }
  ASSERT_TRUE(opened_oat_file != nullptr);
  ASSERT_EQ(2u, opened_oat_file->GetOatDexFiles().size());
  std::unique_ptr<const DexFile> opened_dex_file1 =
      opened_oat_file->GetOatDexFiles()[0]->OpenDexFile(&error_msg);
  std::unique_ptr<const DexFile> opened_dex_file2 =
      opened_oat_file->GetOatDexFiles()[1]->OpenDexFile(&error_msg);

  ASSERT_EQ(dex_file1_data->GetHeader().file_size_, opened_dex_file1->GetHeader().file_size_);
  ASSERT_EQ(0, memcmp(&dex_file1_data->GetHeader(),
                      &opened_dex_file1->GetHeader(),
                      dex_file1_data->GetHeader().file_size_));
  ASSERT_EQ(dex_file1_data->GetLocation(), opened_dex_file1->GetLocation());

  ASSERT_EQ(dex_file2_data->GetHeader().file_size_, opened_dex_file2->GetHeader().file_size_);
  ASSERT_EQ(0, memcmp(&dex_file2_data->GetHeader(),
                      &opened_dex_file2->GetHeader(),
                      dex_file2_data->GetHeader().file_size_));
  ASSERT_EQ(dex_file2_data->GetLocation(), opened_dex_file2->GetLocation());
}

TEST_F(OatTest, DexFileInputCheckOutput) {
  TestDexFileInput(false, /*low_4gb*/false);
}

TEST_F(OatTest, DexFileInputCheckOutputLow4GB) {
  TestDexFileInput(false, /*low_4gb*/true);
}

TEST_F(OatTest, DexFileInputCheckVerifier) {
  TestDexFileInput(true, /*low_4gb*/false);
}

void OatTest::TestZipFileInput(bool verify) {
  TimingLogger timings("OatTest::DexFileInput", false, false);

  ScratchFile zip_file;
  ZipBuilder zip_builder(zip_file.GetFile());

  ScratchFile dex_file1;
  TestDexFileBuilder builder1;
  builder1.AddField("Lsome.TestClass;", "long", "someField");
  builder1.AddMethod("Lsome.TestClass;", "()D", "foo");
  std::unique_ptr<const DexFile> dex_file1_data = builder1.Build(dex_file1.GetFilename());

  MaybeModifyDexFileToFail(verify, dex_file1_data);

  bool success = dex_file1.GetFile()->WriteFully(&dex_file1_data->GetHeader(),
                                                 dex_file1_data->GetHeader().file_size_);
  ASSERT_TRUE(success);
  success = dex_file1.GetFile()->Flush() == 0;
  ASSERT_TRUE(success);
  success = zip_builder.AddFile("classes.dex",
                                &dex_file1_data->GetHeader(),
                                dex_file1_data->GetHeader().file_size_);
  ASSERT_TRUE(success);

  ScratchFile dex_file2;
  TestDexFileBuilder builder2;
  builder2.AddField("Land.AnotherTestClass;", "boolean", "someOtherField");
  builder2.AddMethod("Land.AnotherTestClass;", "()J", "bar");
  std::unique_ptr<const DexFile> dex_file2_data = builder2.Build(dex_file2.GetFilename());

  MaybeModifyDexFileToFail(verify, dex_file2_data);

  success = dex_file2.GetFile()->WriteFully(&dex_file2_data->GetHeader(),
                                            dex_file2_data->GetHeader().file_size_);
  ASSERT_TRUE(success);
  success = dex_file2.GetFile()->Flush() == 0;
  ASSERT_TRUE(success);
  success = zip_builder.AddFile("classes2.dex",
                                &dex_file2_data->GetHeader(),
                                dex_file2_data->GetHeader().file_size_);
  ASSERT_TRUE(success);

  success = zip_builder.Finish();
  ASSERT_TRUE(success) << strerror(errno);

  SafeMap<std::string, std::string> key_value_store;
  key_value_store.Put(OatHeader::kImageLocationKey, "test.art");
  {
    // Test using the AddDexFileSource() interface with the zip file.
    std::vector<const char*> input_filenames { zip_file.GetFilename().c_str() };  // NOLINT [readability/braces] [4]

    ScratchFile oat_file;
    success = WriteElf(oat_file.GetFile(), input_filenames, key_value_store, verify);

    if (verify) {
      ASSERT_FALSE(success);
    } else {
      ASSERT_TRUE(success);

      std::string error_msg;
      std::unique_ptr<OatFile> opened_oat_file(OatFile::Open(oat_file.GetFilename(),
                                                             oat_file.GetFilename(),
                                                             nullptr,
                                                             nullptr,
                                                             false,
                                                             /*low_4gb*/false,
                                                             nullptr,
                                                             &error_msg));
      ASSERT_TRUE(opened_oat_file != nullptr);
      ASSERT_EQ(2u, opened_oat_file->GetOatDexFiles().size());
      std::unique_ptr<const DexFile> opened_dex_file1 =
          opened_oat_file->GetOatDexFiles()[0]->OpenDexFile(&error_msg);
      std::unique_ptr<const DexFile> opened_dex_file2 =
          opened_oat_file->GetOatDexFiles()[1]->OpenDexFile(&error_msg);

      ASSERT_EQ(dex_file1_data->GetHeader().file_size_, opened_dex_file1->GetHeader().file_size_);
      ASSERT_EQ(0, memcmp(&dex_file1_data->GetHeader(),
                          &opened_dex_file1->GetHeader(),
                          dex_file1_data->GetHeader().file_size_));
      ASSERT_EQ(DexFile::GetMultiDexLocation(0, zip_file.GetFilename().c_str()),
                opened_dex_file1->GetLocation());

      ASSERT_EQ(dex_file2_data->GetHeader().file_size_, opened_dex_file2->GetHeader().file_size_);
      ASSERT_EQ(0, memcmp(&dex_file2_data->GetHeader(),
                          &opened_dex_file2->GetHeader(),
                          dex_file2_data->GetHeader().file_size_));
      ASSERT_EQ(DexFile::GetMultiDexLocation(1, zip_file.GetFilename().c_str()),
                opened_dex_file2->GetLocation());
    }
  }

  {
    // Test using the AddZipDexFileSource() interface with the zip file handle.
    ScopedFd zip_fd(dup(zip_file.GetFd()));
    ASSERT_NE(-1, zip_fd.get());

    ScratchFile oat_file;
    success = WriteElf(oat_file.GetFile(),
                       std::move(zip_fd),
                       zip_file.GetFilename().c_str(),
                       key_value_store,
                       verify);
    if (verify) {
      ASSERT_FALSE(success);
    } else {
      ASSERT_TRUE(success);

      std::string error_msg;
      std::unique_ptr<OatFile> opened_oat_file(OatFile::Open(oat_file.GetFilename(),
                                                             oat_file.GetFilename(),
                                                             nullptr,
                                                             nullptr,
                                                             false,
                                                             /*low_4gb*/false,
                                                             nullptr,
                                                             &error_msg));
      ASSERT_TRUE(opened_oat_file != nullptr);
      ASSERT_EQ(2u, opened_oat_file->GetOatDexFiles().size());
      std::unique_ptr<const DexFile> opened_dex_file1 =
          opened_oat_file->GetOatDexFiles()[0]->OpenDexFile(&error_msg);
      std::unique_ptr<const DexFile> opened_dex_file2 =
          opened_oat_file->GetOatDexFiles()[1]->OpenDexFile(&error_msg);

      ASSERT_EQ(dex_file1_data->GetHeader().file_size_, opened_dex_file1->GetHeader().file_size_);
      ASSERT_EQ(0, memcmp(&dex_file1_data->GetHeader(),
                          &opened_dex_file1->GetHeader(),
                          dex_file1_data->GetHeader().file_size_));
      ASSERT_EQ(DexFile::GetMultiDexLocation(0, zip_file.GetFilename().c_str()),
                opened_dex_file1->GetLocation());

      ASSERT_EQ(dex_file2_data->GetHeader().file_size_, opened_dex_file2->GetHeader().file_size_);
      ASSERT_EQ(0, memcmp(&dex_file2_data->GetHeader(),
                          &opened_dex_file2->GetHeader(),
                          dex_file2_data->GetHeader().file_size_));
      ASSERT_EQ(DexFile::GetMultiDexLocation(1, zip_file.GetFilename().c_str()),
                opened_dex_file2->GetLocation());
    }
  }
}

TEST_F(OatTest, ZipFileInputCheckOutput) {
  TestZipFileInput(false);
}

TEST_F(OatTest, ZipFileInputCheckVerifier) {
  TestZipFileInput(true);
}

TEST_F(OatTest, UpdateChecksum) {
  InstructionSet insn_set = kX86;
  std::string error_msg;
  std::unique_ptr<const InstructionSetFeatures> insn_features(
    InstructionSetFeatures::FromVariant(insn_set, "default", &error_msg));
  ASSERT_TRUE(insn_features.get() != nullptr) << error_msg;
  std::unique_ptr<OatHeader> oat_header(OatHeader::Create(insn_set,
                                                          insn_features.get(),
                                                          0u,
                                                          nullptr));
  // The starting adler32 value is 1.
  EXPECT_EQ(1U, oat_header->GetChecksum());

  oat_header->UpdateChecksum(OatHeader::kOatMagic, sizeof(OatHeader::kOatMagic));
  EXPECT_EQ(64291151U, oat_header->GetChecksum());

  // Make sure that null data does not reset the checksum.
  oat_header->UpdateChecksum(nullptr, 0);
  EXPECT_EQ(64291151U, oat_header->GetChecksum());

  oat_header->UpdateChecksum(OatHeader::kOatMagic, sizeof(OatHeader::kOatMagic));
  EXPECT_EQ(216138397U, oat_header->GetChecksum());
}

}  // namespace art
