// Copyright 2011 Google Inc. All Rights Reserved.

#include "oat_writer.h"

#include "class_linker.h"
#include "class_loader.h"
#include "file.h"
#include "os.h"
#include "stl_util.h"

namespace art {

bool OatWriter::Create(const std::string& filename, const ClassLoader* class_loader) {
  const std::vector<const DexFile*>& dex_files = ClassLoader::GetClassPath(class_loader);
  OatWriter oat_writer(dex_files, class_loader);
  return oat_writer.Write(filename);
}

OatWriter::OatWriter(const std::vector<const DexFile*>& dex_files, const ClassLoader* class_loader) {
  class_loader_ = class_loader;
  dex_files_ = &dex_files;

  size_t offset = InitOatHeader();
  offset = InitOatDexFiles(offset);
  offset = InitOatClasses(offset);
  offset = InitOatMethods(offset);
  offset = InitOatCode(offset);
  offset = InitOatCodeDexFiles(offset);

  CHECK_EQ(dex_files_->size(), oat_dex_files_.size());
  CHECK_EQ(dex_files_->size(), oat_classes_.size());
}

size_t OatWriter::InitOatHeader() {
  // create the OatHeader
  oat_header_ = new OatHeader(dex_files_);
  size_t offset = sizeof(*oat_header_);
  return offset;
}

size_t OatWriter::InitOatDexFiles(size_t offset) {
  // create the OatDexFiles
  for (size_t i = 0; i != dex_files_->size(); ++i) {
    const DexFile* dex_file = (*dex_files_)[i];
    CHECK(dex_file != NULL);
    OatDexFile* oat_dex_file = new OatDexFile(*dex_file);
    oat_dex_files_.push_back(oat_dex_file);
    offset += oat_dex_file->SizeOf();
  }
  return offset;
}

size_t OatWriter::InitOatClasses(size_t offset) {
  // create the OatClasses
  // calculate the offsets within OatDexFiles to OatClasses
  for (size_t i = 0; i != dex_files_->size(); ++i) {
    // set offset in OatDexFile to OatClasses
    oat_dex_files_[i]->classes_offset_ = offset;
    oat_dex_files_[i]->UpdateChecksum(*oat_header_);

    const DexFile* dex_file = (*dex_files_)[i];
    OatClasses* oat_classes = new OatClasses(*dex_file);
    oat_classes_.push_back(oat_classes);
    offset += oat_classes->SizeOf();
  }
  return offset;
}

size_t OatWriter::InitOatMethods(size_t offset) {
  // create the OatMethods
  // calculate the offsets within OatClasses to OatMethods
  size_t class_index = 0;
  for (size_t i = 0; i != dex_files_->size(); ++i) {
    const DexFile* dex_file = (*dex_files_)[i];
    for (size_t class_def_index = 0;
         class_def_index < dex_file->NumClassDefs();
         class_def_index++, class_index++) {
      oat_classes_[i]->methods_offsets_[class_def_index] = offset;
      const DexFile::ClassDef& class_def = dex_file->GetClassDef(class_def_index);
      const byte* class_data = dex_file->GetClassData(class_def);
      DexFile::ClassDataHeader header = dex_file->ReadClassDataHeader(&class_data);
      size_t num_direct_methods = header.direct_methods_size_;
      size_t num_virtual_methods = header.virtual_methods_size_;
      uint32_t num_methods = num_direct_methods + num_virtual_methods;
      OatMethods* oat_methods = new OatMethods(num_methods);
      oat_methods_.push_back(oat_methods);
      offset += oat_methods->SizeOf();
    }
    oat_classes_[i]->UpdateChecksum(*oat_header_);
  }
  return offset;
}

size_t OatWriter::InitOatCode(size_t offset) {
  // calculate the offsets within OatHeader to executable code
  size_t old_offset = offset;
  // required to be on a new page boundary
  offset = RoundUp(offset, kPageSize);
  oat_header_->SetExecutableOffset(offset);
  executable_offset_padding_length_ = offset - old_offset;
  return offset;
}

size_t OatWriter::InitOatCodeDexFiles(size_t offset) {
  // calculate the offsets within OatMethods
  size_t oat_class_index = 0;
  for (size_t i = 0; i != dex_files_->size(); ++i) {
    const DexFile* dex_file = (*dex_files_)[i];
    CHECK(dex_file != NULL);
    offset = InitOatCodeDexFile(offset, oat_class_index, *dex_file);
  }
  return offset;
}

size_t OatWriter::InitOatCodeDexFile(size_t offset,
                                     size_t& oat_class_index,
                                     const DexFile& dex_file) {
  for (size_t class_def_index = 0;
       class_def_index < dex_file.NumClassDefs();
       class_def_index++, oat_class_index++) {
    const DexFile::ClassDef& class_def = dex_file.GetClassDef(class_def_index);
    offset = InitOatCodeClassDef(offset, oat_class_index, dex_file, class_def);
    oat_methods_[oat_class_index]->UpdateChecksum(*oat_header_);
  }
  return offset;
}

size_t OatWriter::InitOatCodeClassDef(size_t offset,
                                      size_t oat_class_index,
                                      const DexFile& dex_file,
                                      const DexFile::ClassDef& class_def) {
  const byte* class_data = dex_file.GetClassData(class_def);
  DexFile::ClassDataHeader header = dex_file.ReadClassDataHeader(&class_data);
  size_t num_virtual_methods = header.virtual_methods_size_;
  const char* descriptor = dex_file.GetClassDescriptor(class_def);

  // TODO: remove code ByteArrays from Class/Method (and therefore ClassLoader)
  // TODO: don't write code for shared stubs
  Class* klass = Runtime::Current()->GetClassLinker()->FindClass(descriptor, class_loader_);
  CHECK(klass != NULL) << descriptor;
  CHECK_EQ(klass->GetClassLoader(), class_loader_);
  CHECK_EQ(oat_methods_[oat_class_index]->method_offsets_.size(),
           klass->NumDirectMethods() + num_virtual_methods);
  // Note that we leave the offset to the code in Method::code_
  size_t class_def_method_index = 0;
  for (size_t i = 0; i < klass->NumDirectMethods(); i++, class_def_method_index++) {
    Method* method = klass->GetDirectMethod(i);
    CHECK(method != NULL) << descriptor << " direct " << i;
    offset = InitOatCodeMethod(offset, oat_class_index, class_def_method_index, method);
  }
  // note that num_virtual_methods != klass->NumVirtualMethods() because of miranda methods
  for (size_t i = 0; i < num_virtual_methods; i++, class_def_method_index++) {
    Method* method = klass->GetVirtualMethod(i);
    CHECK(method != NULL) << descriptor << " virtual " << i;
    offset = InitOatCodeMethod(offset, oat_class_index, class_def_method_index, method);
  }
  return offset;
}

size_t OatWriter::InitOatCodeMethod(size_t offset,
                                    size_t oat_class_index,
                                    size_t class_def_method_index,
                                    Method* method) {
  Runtime* runtime = Runtime::Current();
  ByteArray* jni_stub_array = runtime->GetJniStubArray();
  ByteArray* ame_stub_array = runtime->GetAbstractMethodErrorStubArray();

  const ByteArray* code_array = method->GetCodeArray();
  if (code_array == NULL || code_array == jni_stub_array || code_array == ame_stub_array) {
    oat_methods_[oat_class_index]->method_offsets_[class_def_method_index] = 0;
    method->SetOatCodeOffset(0);
  } else {
    offset = RoundUp(offset, kArmAlignment);
    uint32_t thumb_offset = (reinterpret_cast<const int8_t*>(method->GetCode())
                             - code_array->GetData());
    uint32_t code_offset = offset + thumb_offset;
    oat_methods_[oat_class_index]->method_offsets_[class_def_method_index] = code_offset;
    method->SetOatCodeOffset(code_offset);
    offset += code_array->GetLength();
    oat_header_->UpdateChecksum(code_array->GetData(), code_array->GetLength());
  }
  return offset;
}

bool OatWriter::Write(const std::string& filename) {

  UniquePtr<File> file(OS::OpenFile(filename.c_str(), true));
  if (file.get() == NULL) {
    return false;
  }

  if (!file->WriteFully(oat_header_, sizeof(*oat_header_))) {
    PLOG(ERROR) << "Failed to write oat header to " << filename;
    return false;
  }

  if (!WriteTables(file.get())) {
    LOG(ERROR) << "Failed to write oat tables to " << filename;
    return false;
  }

  size_t code_offset = WriteCode(file.get());
  if (code_offset == 0) {
    LOG(ERROR) << "Failed to write oat code to " << filename;
    return false;
  }

  code_offset = WriteCodeDexFiles(file.get(), code_offset);
  if (code_offset == 0) {
    LOG(ERROR) << "Failed to write oat code for dex files to " << filename;
    return false;
  }

  return true;
}

bool OatWriter::WriteTables(File* file) {
  for (size_t i = 0; i != oat_dex_files_.size(); ++i) {
    if (!oat_dex_files_[i]->Write(file)) {
      PLOG(ERROR) << "Failed to write oat dex information";
      return false;
    }
  }
  for (size_t i = 0; i != oat_classes_.size(); ++i) {
    if (!oat_classes_[i]->Write(file)) {
      PLOG(ERROR) << "Failed to write oat classes information";
      return false;
    }
  }
  for (size_t i = 0; i != oat_methods_.size(); ++i) {
    if (!oat_methods_[i]->Write(file)) {
      PLOG(ERROR) << "Failed to write oat methods information";
      return false;
    }
  }
  return true;
}

size_t OatWriter::WriteCode(File* file) {
  uint32_t code_offset = oat_header_->GetExecutableOffset();
  off_t new_offset = lseek(file->Fd(), executable_offset_padding_length_, SEEK_CUR);
  if (static_cast<uint32_t>(new_offset) != code_offset) {
    PLOG(ERROR) << "Failed to seek to oat code section";
    return 0;
  }
  return code_offset;
}

size_t OatWriter::WriteCodeDexFiles(File* file, size_t code_offset) {
  for (size_t i = 0; i != oat_classes_.size(); ++i) {
    const DexFile* dex_file = (*dex_files_)[i];
    CHECK(dex_file != NULL);
    code_offset = WriteCodeDexFile(file, code_offset, *dex_file);
    if (code_offset == 0) {
      return 0;
    }
  }
  return code_offset;
}

size_t OatWriter::WriteCodeDexFile(File* file,
                                   size_t code_offset,
                                   const DexFile& dex_file) {
  for (size_t class_def_index = 0;
       class_def_index < dex_file.NumClassDefs();
       class_def_index++) {
    const DexFile::ClassDef& class_def = dex_file.GetClassDef(class_def_index);
    code_offset = WriteCodeClassDef(file, code_offset, dex_file, class_def);
    if (code_offset == 0) {
      return 0;
    }
  }
  return code_offset;
}

size_t OatWriter::WriteCodeClassDef(File* file,
                                    size_t code_offset,
                                    const DexFile& dex_file,
                                    const DexFile::ClassDef& class_def) {
  const Runtime* runtime = Runtime::Current();
  ClassLinker* class_linker = runtime->GetClassLinker();
  ByteArray* ame_stub_array = runtime->GetAbstractMethodErrorStubArray();

  const byte* class_data = dex_file.GetClassData(class_def);
  DexFile::ClassDataHeader header = dex_file.ReadClassDataHeader(&class_data);
  size_t num_virtual_methods = header.virtual_methods_size_;
  const char* descriptor = dex_file.GetClassDescriptor(class_def);
  Class* klass = class_linker->FindClass(descriptor, class_loader_);

  // TODO: deduplicate code arrays
  // Note that we clear the code array here, image_writer will use GetCodeOffset to find it
  for (size_t i = 0; i < klass->NumDirectMethods(); i++) {
    Method* method = klass->GetDirectMethod(i);
    code_offset = WriteCodeMethod(file, code_offset, method);
    if (code_offset == 0) {
      return 0;
    }
  }
  // note that num_virtual_methods != klass->NumVirtualMethods() because of miranda methods
  for (size_t i = 0; i < num_virtual_methods; i++) {
    Method* method = klass->GetVirtualMethod(i);
    code_offset = WriteCodeMethod(file, code_offset, method);
    if (code_offset == 0) {
      return 0;
    }
  }
  for (size_t i = num_virtual_methods; i < klass->NumVirtualMethods(); i++) {
    Method* method = klass->GetVirtualMethod(i);
    const ByteArray* code_array = method->GetCodeArray();
    CHECK(code_array == NULL  // if compiler not run
          || code_array == ame_stub_array)  // otherwise
            << PrettyMethod(method) << " " << code_array;
    method->SetCodeArray(NULL, kNone);
  }
  return code_offset;
}

size_t OatWriter::WriteCodeMethod(File* file,
                                  size_t code_offset,
                                  Method* method) {
  const Runtime* runtime = Runtime::Current();
  ByteArray* jni_stub_array = runtime->GetJniStubArray();
  ByteArray* ame_stub_array = runtime->GetAbstractMethodErrorStubArray();

  const ByteArray* code_array = method->GetCodeArray();
  if (code_array != NULL && code_array != jni_stub_array && code_array != ame_stub_array) {
    uint32_t aligned_code_offset = RoundUp(code_offset, kArmAlignment);
    uint32_t aligned_code_delta = aligned_code_offset - code_offset;
    if (aligned_code_delta != 0) {
      off_t new_offset = lseek(file->Fd(), aligned_code_delta, SEEK_CUR);
      if (static_cast<uint32_t>(new_offset) != aligned_code_offset) {
        PLOG(ERROR) << "Failed to seek to align oat code";
        return false;
      }
      code_offset += aligned_code_delta;
    }
    if (!file->WriteFully(code_array->GetData(), code_array->GetLength())) {
      PLOG(ERROR) << "Failed to write method code for " << PrettyMethod(method);
      return false;
    }
    code_offset += code_array->GetLength();
  }
  // preserve code offset around code clearing
  uint32_t offset = method->GetOatCodeOffset();
  method->SetCodeArray(NULL, kNone);
  method->SetOatCodeOffset(offset);
  return code_offset;
}

OatWriter::~OatWriter() {
  delete oat_header_;
  STLDeleteElements(&oat_dex_files_);
  STLDeleteElements(&oat_classes_);
  STLDeleteElements(&oat_methods_);
}

OatWriter::OatDexFile::OatDexFile(const DexFile& dex_file) {
  const std::string& location = dex_file.GetLocation();
  dex_file_location_size_ = location.size();
  dex_file_location_data_ = reinterpret_cast<const uint8_t*>(location.data());
  dex_file_checksum_ = dex_file.GetHeader().checksum_;
}

size_t OatWriter::OatDexFile::SizeOf() const {
  return sizeof(dex_file_location_size_)
          + dex_file_location_size_
          + sizeof(dex_file_checksum_)
          + sizeof(classes_offset_);
}

void OatWriter::OatDexFile::UpdateChecksum(OatHeader& oat_header) const {
  oat_header.UpdateChecksum(&dex_file_location_size_, sizeof(dex_file_location_size_));
  oat_header.UpdateChecksum(dex_file_location_data_, dex_file_location_size_);
  oat_header.UpdateChecksum(&dex_file_checksum_, sizeof(dex_file_checksum_));
  oat_header.UpdateChecksum(&classes_offset_, sizeof(classes_offset_));
}

bool OatWriter::OatDexFile::Write(File* file) const {
  if (!file->WriteFully(&dex_file_location_size_, sizeof(dex_file_location_size_))) {
    PLOG(ERROR) << "Failed to write dex file location length";
    return false;
  }
  if (!file->WriteFully(dex_file_location_data_, dex_file_location_size_)) {
    PLOG(ERROR) << "Failed to write dex file location data";
    return false;
  }
  if (!file->WriteFully(&dex_file_checksum_, sizeof(dex_file_checksum_))) {
    PLOG(ERROR) << "Failed to write dex file checksum";
    return false;
  }
  if (!file->WriteFully(&classes_offset_, sizeof(classes_offset_))) {
    PLOG(ERROR) << "Failed to write classes offset";
    return false;
  }
  return true;
}

OatWriter::OatClasses::OatClasses(const DexFile& dex_file) {
  methods_offsets_.resize(dex_file.NumClassDefs());
}

size_t OatWriter::OatClasses::SizeOf() const {
  return (sizeof(methods_offsets_[0]) * methods_offsets_.size());
}

void OatWriter::OatClasses::UpdateChecksum(OatHeader& oat_header) const {
  oat_header.UpdateChecksum(&methods_offsets_[0], SizeOf());
}

bool OatWriter::OatClasses::Write(File* file) const {
  if (!file->WriteFully(&methods_offsets_[0], SizeOf())) {
    PLOG(ERROR) << "Failed to methods offsets";
    return false;
  }
  return true;
}

OatWriter::OatMethods::OatMethods(uint32_t methods_count) {
  method_offsets_.resize(methods_count);
}

size_t OatWriter::OatMethods::SizeOf() const {
  return (sizeof(method_offsets_[0]) * method_offsets_.size());
}

void OatWriter::OatMethods::UpdateChecksum(OatHeader& oat_header) const {
  oat_header.UpdateChecksum(&method_offsets_[0], SizeOf());
}

bool OatWriter::OatMethods::Write(File* file) const {
  if (!file->WriteFully(&method_offsets_[0], SizeOf())) {
    PLOG(ERROR) << "Failed to method offsets";
    return false;
  }
  return true;
}

}  // namespace art
