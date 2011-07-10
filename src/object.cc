// Copyright 2011 Google Inc. All Rights Reserved.

#include "src/globals.h"
#include "src/logging.h"
#include "src/object.h"
#include "src/dex_file.h"
#include "src/raw_dex_file.h"

#include <string.h>

namespace art {

bool Class::IsInSamePackage(const char* descriptor1, const char* descriptor2) {
  size_t i = 0;
  while (descriptor1[i] != '\0' && descriptor1[i] == descriptor2[i]) {
    ++i;
  }
  if (strrchr(descriptor1 + i, '/') != NULL ||
      strrchr(descriptor2 + i, '/') != NULL ) {
    return false;
  } else {
    return true;
  }
}

#if 0
bool Class::IsInSamePackage(const char* descriptor1, const char* descriptor2) {
  size_t size = std::min(descriptor1.size(), descriptor2.size());
  std::pair<const char*, const char*> pos;
  pos = std::mismatch(descriptor1.data(), size, descriptor2.data());
  return !(*(pos.second).rfind('/') != npos && descriptor2.rfind('/') != npos);
}
#endif

bool Class::IsInSamePackage(const Class* that) const {
  const Class* klass1 = this;
  const Class* klass2 = that;
  if (klass1 == klass2) {
    return true;
  }
  // Class loaders must match.
  if (klass1->GetClassLoader() != klass2->GetClassLoader()) {
    return false;
  }
  // Arrays are in the same package when their element classes are.
  if (klass1->IsArray()) {
    klass1 = klass1->GetComponentType();
  }
  if (klass2->IsArray()) {
    klass2 = klass2->GetComponentType();
  }
  // Compare the package part of the descriptor string.
  return IsInSamePackage(klass1->descriptor_.data(),
                         klass2->descriptor_.data());
}

uint32_t Method::NumArgRegisters() {
  CHECK(shorty_ != NULL);
  uint32_t num_registers = 0;
  for (int i = 1; i < shorty_.length(); ++i) {
    char ch = shorty_[i];
    if (ch == 'D' || ch == 'J') {
      num_registers += 2;
    } else {
      num_registers += 1;
    }
  }
  return num_registers;
}

bool Method::HasSameArgumentTypes(const Method* that) const {
  const RawDexFile* raw1 = this->dex_file_->GetRaw();
  const RawDexFile::ProtoId& proto1 = raw1->GetProtoId(this->proto_idx_);
  const RawDexFile* raw2 = that->dex_file_->GetRaw();
  const RawDexFile::ProtoId& proto2 = raw2->GetProtoId(that->proto_idx_);

  // TODO: compare ProtoId objects for equality and exit early

  const RawDexFile::TypeList* type_list1 = raw1->GetProtoParameters(proto1);
  size_t arity1 = (type_list1 == NULL) ? 0 : type_list1->Size();
  const RawDexFile::TypeList* type_list2 = raw2->GetProtoParameters(proto2);
  size_t arity2 = (type_list2 == NULL) ? 0 : type_list2->Size();

  if (arity1 != arity2) {
    return false;
  }

  for (size_t i = 0; i < arity1; ++i) {
    uint32_t type_idx1 = type_list1->GetTypeItem(i).type_idx_;
    const char* type1 = raw1->dexStringByTypeIdx(type_idx1);

    uint32_t type_idx2 = type_list2->GetTypeItem(i).type_idx_;
    const char* type2 = raw2->dexStringByTypeIdx(type_idx2);

    if (strcmp(type1, type2) != 0) {
      return false;
    }
  }

  return true;
}

bool Method::HasSameReturnType(const Method* that) const {
  const RawDexFile* raw1 = this->dex_file_->GetRaw();
  const RawDexFile::ProtoId& proto1 = raw1->GetProtoId(this->proto_idx_);
  const char* type1 = raw1->dexStringByTypeIdx(proto1.return_type_idx_);

  const RawDexFile* raw2 = that->dex_file_->GetRaw();
  const RawDexFile::ProtoId& proto2 = raw2->GetProtoId(that->proto_idx_);
  const char* type2 = raw2->dexStringByTypeIdx(proto2.return_type_idx_);

  return (strcmp(type1, type2) == 0);
}

Method* Class::FindDirectMethodLocally(const StringPiece& name,
                                       const StringPiece& descriptor) const {
  return NULL;  // TODO
}


}  // namespace art
