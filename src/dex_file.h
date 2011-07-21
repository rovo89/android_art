// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_DEX_FILE_H_
#define ART_SRC_DEX_FILE_H_

#include "globals.h"
#include "macros.h"
#include "object.h"

namespace art {

class Class;
class Field;
class Method;
class String;
union JValue;

class DexFile : public ObjectArray {
 public:

  enum ArrayIndexes {
    kStrings = 0,
    kClasses = 1,
    kMethods = 2,
    kFields  = 3,
    kMax     = 4,
  };

  void Init(ObjectArray* strings, ObjectArray* classes,  ObjectArray* methods,  ObjectArray* fields);

  size_t NumStrings() const {
    return GetStrings()->GetLength();
  }

  size_t NumClasses() const {
    return GetClasses()->GetLength();
  }

  size_t NumMethods() const {
    return GetMethods()->GetLength();
  }

  size_t NumFields() const {
    return GetFields()->GetLength();
  }

  String* GetResolvedString(uint32_t string_idx) const {
    return down_cast<String*>(GetStrings()->Get(string_idx));
  }

  void SetResolvedString(uint32_t string_idx, String* resolved) {
    GetStrings()->Set(string_idx, resolved);
  }

  Class* GetResolvedClass(uint32_t class_idx) const {
    return down_cast<Class*>(GetClasses()->Get(class_idx));
  }

  void SetResolvedClass(uint32_t class_idx, Class* resolved) {
    GetClasses()->Set(class_idx, resolved);
  }

  Method* GetResolvedMethod(uint32_t method_idx) const {
    return down_cast<Method*>(GetMethods()->Get(method_idx));
  }

  void SetResolvedMethod(uint32_t method_idx, Method* resolved) {
    GetMethods()->Set(method_idx, resolved);
  }

  Field* GetResolvedField(uint32_t field_idx) const {
    return down_cast<Field*>(GetFields()->Get(field_idx));
  }

  void SetResolvedfield(uint32_t field_idx, Field* resolved) {
    GetFields()->Set(field_idx, resolved);
  }

 private:
  ObjectArray* GetStrings() const {
      return down_cast<ObjectArray*>(Get(kStrings));
  }
  ObjectArray* GetClasses() const {
      return down_cast<ObjectArray*>(Get(kClasses));
  }
  ObjectArray* GetMethods() const {
      return down_cast<ObjectArray*>(Get(kMethods));
  }
  ObjectArray* GetFields() const {
      return down_cast<ObjectArray*>(Get(kFields));
  }
  DexFile();
};

}  // namespace art

#endif  // ART_SRC_DEX_FILE_H_
