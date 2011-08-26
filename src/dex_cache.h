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

class DexCache : public ObjectArray<Object> {
 public:

  enum ArrayIndex {
    kLocation = 0,
    kStrings  = 1,
    kTypes    = 2,
    kMethods  = 3,
    kFields   = 4,
    kMax      = 5,
  };

  void Init(String* location,
            ObjectArray<String>* strings,
            ObjectArray<Class>* types,
            ObjectArray<Method>* methods,
            ObjectArray<Field>* fields);

  String* GetLocation() const {
    return Get(kLocation)->AsString();
  }

  static size_t StringsOffset() {
    return kStrings * sizeof(Object*);
  }

  static size_t FieldsOffset() {
    return kFields * sizeof(Object*);
  }

  static size_t MethodsOffset() {
    return kMethods * sizeof(Object*);
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

 private:
  Object* GetNonNull(ArrayIndex array_index) const {
    Object* obj = Get(array_index);
    DCHECK(obj != NULL);
    return obj;
  }
  DISALLOW_IMPLICIT_CONSTRUCTORS(DexCache);
};

}  // namespace art

#endif  // ART_SRC_DEX_CACHE_H_
