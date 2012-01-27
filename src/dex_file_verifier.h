// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_DEX_FILE_VERIFIER_H_
#define ART_SRC_DEX_FILE_VERIFIER_H_

#include <map>

#include "dex_file.h"

namespace art {

class DexFileVerifier {
 public:
  static bool Verify(DexFile* dex_file, const byte* begin, size_t length);

 private:
  DexFileVerifier(DexFile* dex_file, const byte* begin, size_t length)
      : dex_file_(dex_file), begin_(begin), length_(length),
        header_(&dex_file->GetHeader()), ptr_(NULL), previous_item_(NULL)  {
  }

  bool Verify();

  bool CheckPointerRange(const void* start, const void* end, const char* label) const;
  bool CheckListSize(const void* start, uint32_t count, uint32_t element_size, const char* label) const;
  bool CheckIndex(uint32_t field, uint32_t limit, const char* label) const;

  bool CheckHeader() const;
  bool CheckMap() const;

  uint32_t ReadUnsignedLittleEndian(uint32_t size);
  bool CheckAndGetHandlerOffsets(const DexFile::CodeItem* code_item,
      uint32_t* handler_offsets, uint32_t handlers_size);
  bool CheckClassDataItemField(uint32_t idx, uint32_t access_flags, bool expect_static) const;
  bool CheckClassDataItemMethod(uint32_t idx, uint32_t access_flags, uint32_t code_offset,
      bool expect_direct) const;
  bool CheckPadding(uint32_t offset, uint32_t aligned_offset);
  bool CheckEncodedValue();
  bool CheckEncodedArray();
  bool CheckEncodedAnnotation();

  bool CheckIntraClassDataItem();
  bool CheckIntraCodeItem();
  bool CheckIntraStringDataItem();
  bool CheckIntraDebugInfoItem();
  bool CheckIntraAnnotationItem();
  bool CheckIntraAnnotationsDirectoryItem();

  bool CheckIntraSectionIterate(uint32_t offset, uint32_t count, uint16_t type);
  bool CheckIntraIdSection(uint32_t offset, uint32_t count, uint16_t type);
  bool CheckIntraDataSection(uint32_t offset, uint32_t count, uint16_t type);
  bool CheckIntraSection();

  bool CheckOffsetToTypeMap(uint32_t offset, uint16_t type);
  uint16_t FindFirstClassDataDefiner(const byte* ptr) const;
  uint16_t FindFirstAnnotationsDirectoryDefiner(const byte* ptr) const;

  bool CheckInterStringIdItem();
  bool CheckInterTypeIdItem();
  bool CheckInterProtoIdItem();
  bool CheckInterFieldIdItem();
  bool CheckInterMethodIdItem();
  bool CheckInterClassDefItem();
  bool CheckInterAnnotationSetRefList();
  bool CheckInterAnnotationSetItem();
  bool CheckInterClassDataItem();
  bool CheckInterAnnotationsDirectoryItem();

  bool CheckInterSectionIterate(uint32_t offset, uint32_t count, uint16_t type);
  bool CheckInterSection();

  DexFile* dex_file_;
  const byte* begin_;
  size_t length_;
  const DexFile::Header* header_;

  std::map<uint32_t, uint16_t> offset_to_type_map_;
  const byte* ptr_;
  const void* previous_item_;
};

}  // namespace art

#endif  // ART_SRC_DEX_FILE_VERIFIER_H_
