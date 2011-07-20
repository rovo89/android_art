// Copyright 2011 Google Inc. All Rights Reserved.

#include "src/object.h"
#include <algorithm>
#include <string.h>
#include "src/globals.h"
#include "src/logging.h"
#include "src/dex_file.h"
#include "src/raw_dex_file.h"

namespace art {

bool Class::IsInSamePackage(const StringPiece& descriptor1,
                            const StringPiece& descriptor2) {
  size_t i = 0;
  while (descriptor1[i] != '\0' && descriptor1[i] == descriptor2[i]) {
    ++i;
  }
  if (descriptor1.find('/', i) != StringPiece::npos ||
      descriptor2.find('/', i) != StringPiece::npos) {
    return false;
  } else {
    return true;
  }
}

#if 0
bool Class::IsInSamePackage(const StringPiece& descriptor1,
                            const StringPiece& descriptor2) {
  size_t size = std::min(descriptor1.size(), descriptor2.size());
  std::pair<StringPiece::const_iterator, StringPiece::const_iterator> pos;
  pos = std::mismatch(descriptor1.begin(), descriptor1.begin() + size,
                      descriptor2.begin());
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
  return IsInSamePackage(klass1->descriptor_, klass2->descriptor_);
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

// The number of reference arguments to this method including implicit this
// pointer
size_t Method::NumReferenceArgs() const {
  size_t result = IsStatic() ? 0 : 1;
  for (int i = 1; i < shorty_.length(); i++) {
    if ((shorty_[i] == 'L') || (shorty_[i] == '[')) {
      result++;
    }
  }
  return result;
}

// The number of long or double arguments
size_t Method::NumLongOrDoubleArgs() const {
  size_t result = 0;
  for (int i = 1; i < shorty_.length(); i++) {
    if ((shorty_[i] == 'D') || (shorty_[i] == 'J')) {
      result++;
    }
  }
  return result;
}

// The number of reference arguments to this method before the given parameter
// index
size_t Method::NumReferenceArgsBefore(unsigned int param) const {
  CHECK_LT(param, NumArgs());
  unsigned int result = IsStatic() ? 0 : 1;
  for (unsigned int i = 1; (i < (unsigned int)shorty_.length()) &&
                           (i < (param + 1)); i++) {
    if ((shorty_[i] == 'L') || (shorty_[i] == '[')) {
      result++;
    }
  }
  return result;
}

// Is the given method parameter a reference?
bool Method::IsParamAReference(unsigned int param) const {
  CHECK_LT(param, NumArgs());
  if (IsStatic()) {
    param++;  // 0th argument must skip return value at start of the shorty
  } else if (param == 0) {
    return true;  // this argument
  }
  return ((shorty_[param] == 'L') || (shorty_[param] == '['));
}

// Is the given method parameter a long or double?
bool Method::IsParamALongOrDouble(unsigned int param) const {
  CHECK_LT(param, NumArgs());
  if (IsStatic()) {
    param++;  // 0th argument must skip return value at start of the shorty
  }
  return (shorty_[param] == 'J') || (shorty_[param] == 'D');
}

size_t Method::ParamSizeInBytes(unsigned int param) const {
  CHECK_LT(param, NumArgs());
  if (IsStatic()) {
    param++;  // 0th argument must skip return value at start of the shorty
  } else if (param == 0) {
    return kPointerSize;  // this argument
  }
  switch (shorty_[param]) {
    case '[': return kPointerSize;
    case 'L': return kPointerSize;
    case 'D': return 8;
    case 'J': return 8;
    default:  return 4;
  }
}

bool Method::HasSameArgumentTypes(const Method* that) const {
  const RawDexFile* raw1 = this->GetClass()->GetDexFile()->GetRaw();
  const RawDexFile::ProtoId& proto1 = raw1->GetProtoId(this->proto_idx_);
  const RawDexFile* raw2 = that->GetClass()->GetDexFile()->GetRaw();
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
  const RawDexFile* raw1 = this->GetClass()->GetDexFile()->GetRaw();
  const RawDexFile::ProtoId& proto1 = raw1->GetProtoId(this->proto_idx_);
  const char* type1 = raw1->dexStringByTypeIdx(proto1.return_type_idx_);

  const RawDexFile* raw2 = that->GetClass()->GetDexFile()->GetRaw();
  const RawDexFile::ProtoId& proto2 = raw2->GetProtoId(that->proto_idx_);
  const char* type2 = raw2->dexStringByTypeIdx(proto2.return_type_idx_);

  return (strcmp(type1, type2) == 0);
}

Method* Class::FindDirectMethod(const StringPiece& name) const {
  Method* result = NULL;
  for (size_t i = 0; i < NumDirectMethods(); i++) {
    Method* method = GetDirectMethod(i);
    if (method->GetName().compare(name) == 0) {
      result = method;
      break;
    }
  }
  return result;
}

Method* Class::FindVirtualMethod(const StringPiece& name) const {
  Method* result = NULL;
  for (size_t i = 0; i < NumVirtualMethods(); i++) {
    Method* method = GetVirtualMethod(i);
    if (method->GetName().compare(name) == 0) {
      result = method;
      break;
    }
  }
  return result;
}

Method* Class::FindDirectMethodLocally(const StringPiece& name,
                                       const StringPiece& descriptor) const {
  return NULL;  // TODO
}

static const char* kClassStatusNames[] = {
  "Error",
  "NotReady",
  "Idx",
  "Loaded",
  "Resolved",
  "Verifying",
  "Verified",
  "Initializing",
  "Initialized"
};
std::ostream& operator<<(std::ostream& os, const Class::Status& rhs) {
  if (rhs >= Class::kStatusError && rhs <= Class::kStatusInitialized) {
    os << kClassStatusNames[rhs - 1];
  } else {
    os << "Class::Status[" << static_cast<int>(rhs) << "]";
  }
  return os;
}

}  // namespace art
