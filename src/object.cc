// Copyright 2011 Google Inc. All Rights Reserved.

#include "object.h"

#include <string.h>
#include <algorithm>

#include "globals.h"
#include "heap.h"
#include "logging.h"
#include "dex_cache.h"
#include "dex_file.h"

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
  } else if (param == 0) {
    return false;  // this argument
  }
  return (shorty_[param] == 'J') || (shorty_[param] == 'D');
}

static size_t ShortyCharToSize(char x) {
  switch (x) {
    case 'V': return 0;
    case '[': return kPointerSize;
    case 'L': return kPointerSize;
    case 'D': return 8;
    case 'J': return 8;
    default:  return 4;
  }
}

size_t Method::ParamSize(unsigned int param) const {
  CHECK_LT(param, NumArgs());
  if (IsStatic()) {
    param++;  // 0th argument must skip return value at start of the shorty
  } else if (param == 0) {
    return kPointerSize;  // this argument
  }
  return ShortyCharToSize(shorty_[param]);
}

size_t Method::ReturnSize() const {
  return ShortyCharToSize(shorty_[0]);
}

Method* Class::FindDeclaredDirectMethod(const StringPiece& name,
                                        const StringPiece& descriptor) {
  for (size_t i = 0; i < NumDirectMethods(); ++i) {
    Method* method = GetDirectMethod(i);
    if (String::EqualsUtf8(method->GetName(), name.data()) &&
        String::EqualsUtf8(method->GetDescriptor(), descriptor.data())) {
      return method;
    }
  }
  return NULL;
}

Method* Class::FindDirectMethod(const StringPiece& name,
                                const StringPiece& descriptor) {
  for (Class* klass = this; klass != NULL; klass = klass->GetSuperClass()) {
    Method* method = klass->FindDeclaredDirectMethod(name, descriptor);
    if (method != NULL) {
      return method;
    }
  }
  return NULL;
}

Method* Class::FindDeclaredVirtualMethod(const StringPiece& name,
                                         const StringPiece& descriptor) {
  for (size_t i = 0; i < NumVirtualMethods(); ++i) {
    Method* method = GetVirtualMethod(i);
    if (String::EqualsUtf8(method->GetName(), name.data()) &&
        String::EqualsUtf8(method->GetDescriptor(), descriptor.data())) {
      return method;
    }
  }
  return NULL;
}

Method* Class::FindVirtualMethod(const StringPiece& name,
                                 const StringPiece& descriptor) {
  for (Class* klass = this; klass != NULL; klass = klass->GetSuperClass()) {
    Method* method = klass->FindDeclaredVirtualMethod(name, descriptor);
    if (method != NULL) {
      return method;
    }
  }
  return NULL;
}

// TODO: get global references for these
Class* String::java_lang_String_ = NULL;
Class* String::char_array_ = NULL;

void String::InitClasses(Class* java_lang_String, Class* char_array) {
  java_lang_String_ = java_lang_String;
  char_array_ = char_array;
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
    os << kClassStatusNames[rhs + 1];
  } else {
    os << "Class::Status[" << static_cast<int>(rhs) << "]";
  }
  return os;
}

}  // namespace art
