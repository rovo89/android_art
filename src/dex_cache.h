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
class ImageWriter;
class Method;
class String;
union JValue;

class MANAGED CodeAndDirectMethods : public IntArray {
 public:
  void* GetResolvedCode(uint32_t method_idx) const {
    return reinterpret_cast<byte*>(Get(CodeIndex(method_idx)));
  }
  Method* GetResolvedMethod(uint32_t method_idx) const {
    return reinterpret_cast<Method*>(Get(MethodIndex(method_idx)));
  }

  void SetResolvedDirectMethodTrampoline(uint32_t method_idx, ByteArray* code_array) {
    CHECK(code_array != NULL);
    int32_t code = reinterpret_cast<int32_t>(code_array->GetData());
    Set(CodeIndex(method_idx), code);
    Set(MethodIndex(method_idx), method_idx);
  }

  void SetResolvedDirectMethod(uint32_t method_idx, Method* method);

  static size_t LengthAsArray(size_t elements) {
    return kMax * elements;
  }

  // Offset of resolved method entry from start of code_and_direct_methods_
  static size_t MethodOffsetInBytes(uint32_t method_idx) {
    return (MethodIndex(method_idx) * sizeof(ElementType) + Array::DataOffset().Int32Value());
  }

  // Offset of resolved method's code_ from start of code_and_direct_methods_
  static size_t CodeOffsetInBytes(uint32_t method_idx) {
    return (CodeIndex(method_idx) * sizeof(ElementType) + Array::DataOffset().Int32Value());
  }

  size_t NumCodeAndDirectMethods() const {
    return GetLength() / kMax;
  }

 private:
  enum TupleIndex {
    kCode   = 0,
    kMethod = 1,
    kMax    = 2,
  };

  static size_t CodeIndex(uint32_t method_idx) {
    return method_idx * kMax + kCode;
  }
  static size_t MethodIndex(uint32_t method_idx) {
    return method_idx * kMax + kMethod;
  }

  // grant friend status to ImageWriter fixup code that needs to know internal layout
  friend class ImageWriter;

  DISALLOW_IMPLICIT_CONSTRUCTORS(CodeAndDirectMethods);
};

class MANAGED DexCache : public ObjectArray<Object> {
 public:
  void Init(String* location,
            ObjectArray<String>* strings,
            ObjectArray<Class>* types,
            ObjectArray<Method>* methods,
            ObjectArray<Field>* fields,
            CodeAndDirectMethods* code_and_direct_methods,
            ObjectArray<StaticStorageBase>* initialized_static_storage);

  String* GetLocation() const {
    return Get(kLocation)->AsString();
  }

  static MemberOffset StringsOffset() {
    return MemberOffset(DataOffset().Int32Value() +
                        kStrings * sizeof(Object*));
  }

  static MemberOffset ResolvedFieldsOffset() {
    return MemberOffset(DataOffset().Int32Value() +
                        kResolvedFields * sizeof(Object*));
  }

  static MemberOffset ResolvedMethodsOffset() {
    return MemberOffset(DataOffset().Int32Value() +
                        kResolvedMethods * sizeof(Object*));
  }

  size_t NumStrings() const {
    return GetStrings()->GetLength();
  }

  size_t NumResolvedTypes() const {
    return GetResolvedTypes()->GetLength();
  }

  size_t NumResolvedMethods() const {
    return GetResolvedMethods()->GetLength();
  }

  size_t NumResolvedFields() const {
    return GetResolvedFields()->GetLength();
  }

  size_t NumCodeAndDirectMethods() const {
    return GetCodeAndDirectMethods()->NumCodeAndDirectMethods();
  }

  size_t NumInitializedStaticStorage() const {
    return GetInitializedStaticStorage()->GetLength();
  }

  String* GetResolvedString(uint32_t string_idx) const {
    return GetStrings()->Get(string_idx);
  }

  void SetResolvedString(uint32_t string_idx, String* resolved) {
    GetStrings()->Set(string_idx, resolved);
  }

  Class* GetResolvedType(uint32_t type_idx) const {
    return GetResolvedTypes()->Get(type_idx);
  }

  void SetResolvedType(uint32_t type_idx, Class* resolved) {
    GetResolvedTypes()->Set(type_idx, resolved);
  }

  Method* GetResolvedMethod(uint32_t method_idx) const {
    return GetResolvedMethods()->Get(method_idx);
  }

  void SetResolvedMethod(uint32_t method_idx, Method* resolved) {
    GetResolvedMethods()->Set(method_idx, resolved);
  }

  Field* GetResolvedField(uint32_t field_idx) const {
    return GetResolvedFields()->Get(field_idx);
  }

  void SetResolvedField(uint32_t field_idx, Field* resolved) {
    GetResolvedFields()->Set(field_idx, resolved);
  }

  ObjectArray<String>* GetStrings() const {
    return static_cast<ObjectArray<String>*>(GetNonNull(kStrings));
  }
  ObjectArray<Class>* GetResolvedTypes() const {
    return static_cast<ObjectArray<Class>*>(GetNonNull(kResolvedTypes));
  }
  ObjectArray<Method>* GetResolvedMethods() const {
    return static_cast<ObjectArray<Method>*>(GetNonNull(kResolvedMethods));
  }
  ObjectArray<Field>* GetResolvedFields() const {
    return static_cast<ObjectArray<Field>*>(GetNonNull(kResolvedFields));
  }
  CodeAndDirectMethods* GetCodeAndDirectMethods() const {
    return static_cast<CodeAndDirectMethods*>(GetNonNull(kCodeAndDirectMethods));
  }
  ObjectArray<StaticStorageBase>* GetInitializedStaticStorage() const {
    return static_cast<ObjectArray<StaticStorageBase>*>(GetNonNull(kInitializedStaticStorage));
  }

  static size_t LengthAsArray() {
    return kMax;
  }

 private:

  enum ArrayIndex {
    kLocation                 = 0,
    kStrings                  = 1,
    kResolvedTypes            = 2,
    kResolvedMethods          = 3,
    kResolvedFields           = 4,
    kCodeAndDirectMethods     = 5,
    kInitializedStaticStorage = 6,
    kMax                      = 7,
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
