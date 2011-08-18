// Copyright 2011 Google Inc. All Rights Reserved.

#include "object.h"

#include <string.h>
#include <algorithm>

#include "class_linker.h"
#include "globals.h"
#include "heap.h"
#include "logging.h"
#include "dex_cache.h"
#include "dex_file.h"

namespace art {

bool Class::Implements(const Class* klass) const {
  DCHECK(klass != NULL);
  DCHECK(klass->IsInterface());
  // All interfaces implemented directly and by our superclass, and
  // recursively all super-interfaces of those interfaces, are listed
  // in iftable_, so we can just do a linear scan through that.
  for (size_t i = 0; i < iftable_count_; i++) {
    if (iftable_[i].GetClass() == klass) {
      return true;
    }
  }
  return false;
}

// Determine whether "this" is assignable from "klazz", where both of these
// are array classes.
//
// Consider an array class, e.g. Y[][], where Y is a subclass of X.
//   Y[][]            = Y[][] --> true (identity)
//   X[][]            = Y[][] --> true (element superclass)
//   Y                = Y[][] --> false
//   Y[]              = Y[][] --> false
//   Object           = Y[][] --> true (everything is an object)
//   Object[]         = Y[][] --> true
//   Object[][]       = Y[][] --> true
//   Object[][][]     = Y[][] --> false (too many []s)
//   Serializable     = Y[][] --> true (all arrays are Serializable)
//   Serializable[]   = Y[][] --> true
//   Serializable[][] = Y[][] --> false (unless Y is Serializable)
//
// Don't forget about primitive types.
//   int[] instanceof Object[]     --> false
//
bool Class::IsArrayAssignableFromArray(const Class* klass) const {
  DCHECK(IsArray());
  DCHECK(klass->IsArray());
  DCHECK_GT(array_rank_, 0);
  DCHECK_GT(klass->array_rank_, 0);
  DCHECK(component_type_ != NULL);
  DCHECK(klass->component_type_ != NULL);
  if (array_rank_ > klass->array_rank_) {
    // Too many []s.
    return false;
  }
  if (array_rank_ == klass->array_rank_) {
    return component_type_->IsAssignableFrom(klass->component_type_);
  }
  DCHECK_LT(array_rank_, klass->array_rank_);
  // The thing we might be assignable from has more dimensions.  We
  // must be an Object or array of Object, or a standard array
  // interface or array of standard array interfaces (the standard
  // interfaces being java/lang/Cloneable and java/io/Serializable).
  if (component_type_->IsInterface()) {
    // See if we implement our component type.  We know the
    // base element is an interface; if the array class implements
    // it, we know it's a standard array interface.
    return Implements(component_type_);
  }
  // See if this is an array of Object, Object[], etc.  We know
  // that the superclass of an array is always Object, so we
  // just compare the element type to that.
  Class* java_lang_Object = GetSuperClass();
  DCHECK(java_lang_Object != NULL);
  DCHECK(java_lang_Object->GetSuperClass() == NULL);
  return (component_type_ == java_lang_Object);
}

bool Class::IsAssignableFromArray(const Class* klass) const {
  DCHECK(!IsInterface());  // handled first in IsAssignableFrom
  DCHECK(klass->IsArray());
  if (!IsArray()) {
    // If "this" is not also an array, it must be Object.
    // klass's super should be java_lang_Object, since it is an array.
    Class* java_lang_Object = klass->GetSuperClass();
    DCHECK(java_lang_Object != NULL);
    DCHECK(java_lang_Object->GetSuperClass() == NULL);
    return this == java_lang_Object;
  }
  return IsArrayAssignableFromArray(klass);
}

bool Class::IsSubClass(const Class* klass) const {
  DCHECK(!IsInterface());
  DCHECK(!klass->IsArray());
  const Class* current = this;
  do {
    if (current == klass) {
      return true;
    }
    current = current->GetSuperClass();
  } while (current != NULL);
  return false;
}

bool Class::IsInSamePackage(const String* descriptor_string_1,
                            const String* descriptor_string_2) {
  const std::string descriptor1(descriptor_string_1->ToModifiedUtf8());
  const std::string descriptor2(descriptor_string_2->ToModifiedUtf8());

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

bool Field::GetBoolean() {
  CHECK_EQ(GetType(), 'Z');
  CHECK(IsStatic());
  return declaring_class_->static_32bit_primitives_->Get(offset_);
}

void Field::SetBoolean(bool z) {
  CHECK_EQ(GetType(), 'Z');
  CHECK(IsStatic());
  declaring_class_->static_32bit_primitives_->Set(offset_, z);
}

int8_t Field::GetByte() {
  CHECK_EQ(GetType(), 'B');
  CHECK(IsStatic());
  return declaring_class_->static_32bit_primitives_->Get(offset_);
}

void Field::SetByte(int8_t b) {
  CHECK_EQ(GetType(), 'B');
  CHECK(IsStatic());
  declaring_class_->static_32bit_primitives_->Set(offset_, b);
}

uint16_t Field::GetChar() {
  CHECK_EQ(GetType(), 'C');
  CHECK(IsStatic());
  return declaring_class_->static_32bit_primitives_->Get(offset_);
}

void Field::SetChar(uint16_t c) {
  CHECK_EQ(GetType(), 'C');
  CHECK(IsStatic());
  declaring_class_->static_32bit_primitives_->Set(offset_, c);
}

uint16_t Field::GetShort() {
  CHECK_EQ(GetType(), 'S');
  CHECK(IsStatic());
  return declaring_class_->static_32bit_primitives_->Get(offset_);
}

void Field::SetShort(uint16_t s) {
  CHECK_EQ(GetType(), 'S');
  CHECK(IsStatic());
  declaring_class_->static_32bit_primitives_->Set(offset_, s);
}

int32_t Field::GetInt() {
  CHECK_EQ(GetType(), 'I');
  CHECK(IsStatic());
  return declaring_class_->static_32bit_primitives_->Get(offset_);
}

void Field::SetInt(int32_t i) {
  CHECK_EQ(GetType(), 'I');
  CHECK(IsStatic());
  declaring_class_->static_32bit_primitives_->Set(offset_, i);
}

int64_t Field::GetLong() {
  CHECK_EQ(GetType(), 'J');
  CHECK(IsStatic());
  return declaring_class_->static_64bit_primitives_->Get(offset_);
}

void Field::SetLong(int64_t j) {
  CHECK_EQ(GetType(), 'J');
  CHECK(IsStatic());
  declaring_class_->static_64bit_primitives_->Set(offset_, j);
}

float Field::GetFloat() {
  CHECK_EQ(GetType(), 'F');
  CHECK(IsStatic());
  JValue float_bits;
  float_bits.i = declaring_class_->static_32bit_primitives_->Get(offset_);
  return float_bits.f;
}

void Field::SetFloat(float f) {
  CHECK_EQ(GetType(), 'F');
  CHECK(IsStatic());
  JValue float_bits;
  float_bits.f = f;
  declaring_class_->static_32bit_primitives_->Set(offset_, float_bits.i);
}

double Field::GetDouble() {
  CHECK_EQ(GetType(), 'D');
  CHECK(IsStatic());
  JValue double_bits;
  double_bits.j = declaring_class_->static_64bit_primitives_->Get(offset_);
  return double_bits.d;
}

void Field::SetDouble(double d) {
  CHECK_EQ(GetType(), 'D');
  CHECK(IsStatic());
  JValue double_bits;
  double_bits.d = d;
  declaring_class_->static_64bit_primitives_->Set(offset_, double_bits.j);
}

Object* Field::GetObject() {
  CHECK(GetType() == 'L' || GetType() == '[');
  CHECK(IsStatic());
  return declaring_class_->static_references_->Get(offset_);
}

const Object* Field::GetObject() const {
  CHECK(GetType() == 'L' || GetType() == '[');
  CHECK(IsStatic());
  return declaring_class_->static_references_->Get(offset_);
}

void Field::SetObject(Object* l) {
  CHECK(GetType() == 'L' || GetType() == '[');
  declaring_class_->static_references_->Set(offset_, l);  // TODO: write barrier
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

size_t Method::NumArgArrayBytes() {
  const StringPiece& shorty = GetShorty();
  size_t num_bytes = 0;
  for (int i = 1; i < shorty.size(); ++i) {
    char ch = shorty[i];
    if (ch == 'D' || ch == 'J') {
      num_bytes += 8;
    } else if (ch == 'L') {
      // Argument is a reference or an array.  The shorty descriptor
      // does not distinguish between these types.
      num_bytes += sizeof(Object*);
    } else {
      num_bytes += 4;
    }
  }
  return num_bytes;
}

// The number of reference arguments to this method including implicit this
// pointer
size_t Method::NumReferenceArgs() const {
  size_t result = IsStatic() ? 0 : 1;  // The implicit this pointer.
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

bool Method::HasSameNameAndDescriptor(const Method* that) const {
  return (this->GetName()->Equals(that->GetName()) &&
          this->GetSignature()->Equals(that->GetSignature()));
}

Method* Class::FindDeclaredDirectMethod(const StringPiece& name,
                                        const StringPiece& signature) {
  for (size_t i = 0; i < NumDirectMethods(); ++i) {
    Method* method = GetDirectMethod(i);
    if (method->GetName()->Equals(name) &&
        method->GetSignature()->Equals(signature)) {
      return method;
    }
  }
  return NULL;
}

Method* Class::FindDirectMethod(const StringPiece& name,
                                const StringPiece& signature) {
  for (Class* klass = this; klass != NULL; klass = klass->GetSuperClass()) {
    Method* method = klass->FindDeclaredDirectMethod(name, signature);
    if (method != NULL) {
      return method;
    }
  }
  return NULL;
}

Method* Class::FindDeclaredVirtualMethod(const StringPiece& name,
                                         const StringPiece& signature) {
  for (size_t i = 0; i < NumVirtualMethods(); ++i) {
    Method* method = GetVirtualMethod(i);
    if (method->GetName()->Equals(name) &&
        method->GetSignature()->Equals(signature)) {
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

Field* Class::FindDeclaredInstanceField(const StringPiece& name, const StringPiece& descriptor) {
  // Is the field in this class?
  // Interfaces are not relevant because they can't contain instance fields.
  for (size_t i = 0; i < NumInstanceFields(); ++i) {
    Field* f = GetInstanceField(i);
    if (f->GetName()->Equals(name) && f->GetDescriptor() == descriptor) {
      return f;
    }
  }
  return NULL;
}

Field* Class::FindInstanceField(const StringPiece& name, const StringPiece& descriptor) {
  // Is the field in this class, or any of its superclasses?
  // Interfaces are not relevant because they can't contain instance fields.
  for (Class* c = this; c != NULL; c = c->GetSuperClass()) {
    Field* f = c->FindDeclaredInstanceField(name, descriptor);
    if (f != NULL) {
      return f;
    }
  }
  return NULL;
}

Field* Class::FindDeclaredStaticField(const StringPiece& name, const StringPiece& descriptor) {
  for (size_t i = 0; i < NumStaticFields(); ++i) {
    Field* f = GetStaticField(i);
    if (f->GetName()->Equals(name) && f->GetDescriptor() == descriptor) {
      return f;
    }
  }
  return NULL;
}

Field* Class::FindStaticField(const StringPiece& name, const StringPiece& descriptor) {
  // Is the field in this class (or its interfaces), or any of its
  // superclasses (or their interfaces)?
  for (Class* c = this; c != NULL; c = c->GetSuperClass()) {
    // Is the field in this class?
    Field* f = c->FindDeclaredStaticField(name, descriptor);
    if (f != NULL) {
      return f;
    }

    // Is this field in any of this class' interfaces?
    for (size_t i = 0; i < c->NumInterfaces(); ++i) {
      Class* interface = c->GetInterface(i);
      f = interface->FindDeclaredStaticField(name, descriptor);
      if (f != NULL) {
        return f;
      }
    }
  }
  return NULL;
}

template<typename T>
PrimitiveArray<T>* PrimitiveArray<T>::Alloc(size_t length) {
  Array* raw_array = Array::Alloc(array_class_, length, sizeof(T));
  return down_cast<PrimitiveArray<T>*>(raw_array);
}

template <typename T> Class* PrimitiveArray<T>::array_class_ = NULL;

// Explicitly instantiate all the primitive array types.
template class PrimitiveArray<uint8_t>;   // BooleanArray
template class PrimitiveArray<int8_t>;    // ByteArray
template class PrimitiveArray<uint16_t>;  // CharArray
template class PrimitiveArray<double>;    // DoubleArray
template class PrimitiveArray<float>;     // FloatArray
template class PrimitiveArray<int32_t>;   // IntArray
template class PrimitiveArray<int64_t>;   // LongArray
template class PrimitiveArray<int16_t>;   // ShortArray

// TODO: get global references for these
Class* String::java_lang_String_ = NULL;

void String::InitClass(Class* java_lang_String) {
  java_lang_String_ = java_lang_String;
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
