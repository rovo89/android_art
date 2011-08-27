// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_DEX_CACHE_H_
#define ART_SRC_DEX_CACHE_H_

#include "dex_file.h"
#include "globals.h"
#include "macros.h"
#include "object.h"

namespace art {

class Class;
class Field;
class Method;
class String;
union JValue;

class CodeAndMethods : public IntArray {
 public:
  Method* GetResolvedCode(uint32_t method_idx) const {
    return reinterpret_cast<Method*>(Get(method_idx * kMax + kCode));
  }
  void* GetResolvedMethod(uint32_t method_idx) const {
    return reinterpret_cast<byte*>(Get(method_idx * kMax + kMethod));
  }

  void SetResolvedMethod(uint32_t method_idx, Method* method) {
    CHECK(method != NULL);
    // CHECK(method->GetCode() != NULL);  // TODO enable when all code is compiling
    Set(method_idx * kMax + kCode,   reinterpret_cast<int32_t>(method->GetCode()));
    Set(method_idx * kMax + kMethod, reinterpret_cast<int32_t>(method));
  }

 static size_t LengthAsArray(size_t elements) {
   return kMax * elements;
 }

 private:
  enum TupleIndex {
    kCode   = 0,
    kMethod = 1,
    kMax    = 2,
  };

};

class DexCache : public ObjectArray<Object> {
 public:
  void Init(String* location,
            ObjectArray<String>* strings,
            ObjectArray<Class>* types,
            ObjectArray<Method>* methods,
            ObjectArray<Field>* fields,
            CodeAndMethods* code_and_methods);

  String* GetLocation() const {
    return Get(kLocation)->AsString();
  }

  static MemberOffset StringsOffset() {
    return MemberOffset(DataOffset().Int32Value() +
                        kStrings * sizeof(Object*));
  }

  static MemberOffset FieldsOffset() {
    return MemberOffset(DataOffset().Int32Value() +
                        kFields * sizeof(Object*));
  }

  static MemberOffset MethodsOffset() {
    return MemberOffset(DataOffset().Int32Value() +
                        kMethods * sizeof(Object*));
  }

  size_t NumStrings() const {
    return GetStrings()->GetLength();
  }

  size_t NumTypes() const {
    return GetTypes()->GetLength();
  }

  size_t NumMethods() const {
    return GetMethods()->GetLength();
  }

  size_t NumFields() const {
    return GetFields()->GetLength();
  }

  size_t NumCodeAndMethods() const {
    return GetCodeAndMethods()->GetLength();
  }

  String* GetResolvedString(uint32_t string_idx) const {
    return GetStrings()->Get(string_idx);
  }

  void SetResolvedString(uint32_t string_idx, String* resolved) {
    GetStrings()->Set(string_idx, resolved);
  }

  Class* GetResolvedType(uint32_t type_idx) const {
    return GetTypes()->Get(type_idx);
  }

  void SetResolvedType(uint32_t type_idx, Class* resolved) {
    GetTypes()->Set(type_idx, resolved);
  }

  Method* GetResolvedMethod(uint32_t method_idx) const {
    return GetMethods()->Get(method_idx);
  }

  void SetResolvedMethod(uint32_t method_idx, Method* resolved) {
    GetMethods()->Set(method_idx, resolved);
  }

  Field* GetResolvedField(uint32_t field_idx) const {
    return GetFields()->Get(field_idx);
  }

  void SetResolvedfield(uint32_t field_idx, Field* resolved) {
    GetFields()->Set(field_idx, resolved);
  }

  ObjectArray<String>* GetStrings() const {
    return static_cast<ObjectArray<String>*>(GetNonNull(kStrings));
  }
  ObjectArray<Class>* GetTypes() const {
    return static_cast<ObjectArray<Class>*>(GetNonNull(kTypes));
  }
  ObjectArray<Method>* GetMethods() const {
    return static_cast<ObjectArray<Method>*>(GetNonNull(kMethods));
  }
  ObjectArray<Field>* GetFields() const {
    return static_cast<ObjectArray<Field>*>(GetNonNull(kFields));
  }
  CodeAndMethods* GetCodeAndMethods() const {
    return static_cast<CodeAndMethods*>(GetNonNull(kCodeAndMethods));
  }

 static size_t LengthAsArray() {
   return kMax;
 }

 private:

  enum ArrayIndex {
    kLocation       = 0,
    kStrings        = 1,
    kTypes          = 2,
    kMethods        = 3,
    kFields         = 4,
    kCodeAndMethods = 5,
    kMax            = 6,
  };

  Object* GetNonNull(ArrayIndex array_index) const {
    Object* obj = Get(array_index);
    DCHECK(obj != NULL);
    return obj;
  }
  DISALLOW_IMPLICIT_CONSTRUCTORS(DexCache);
};

}  // namespace art

#endif  // ART_SRC_DEX_CACHE_H_
