/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ART_SRC_OBJECT_UTILS_H_
#define ART_SRC_OBJECT_UTILS_H_

#include "class_linker.h"
#include "dex_cache.h"
#include "dex_file.h"
#include "intern_table.h"
#include "monitor.h"
#include "object.h"
#include "runtime.h"
#include "UniquePtr.h"

#include <string>

namespace art {

class ObjectLock {
 public:
  explicit ObjectLock(Object* object) : self_(Thread::Current()), obj_(object) {
    CHECK(object != NULL);
    obj_->MonitorEnter(self_);
  }

  ~ObjectLock() {
    obj_->MonitorExit(self_);
  }

  void Wait() {
    return Monitor::Wait(self_, obj_, 0, 0, false);
  }

  void Notify() {
    obj_->Notify();
  }

  void NotifyAll() {
    obj_->NotifyAll();
  }

 private:
  Thread* self_;
  Object* obj_;
  DISALLOW_COPY_AND_ASSIGN(ObjectLock);
};

class ClassHelper {
 public:
  ClassHelper(const Class* c = NULL, ClassLinker* l = NULL)
      : class_def_(NULL),
        class_linker_(l),
        dex_cache_(NULL),
        dex_file_(NULL),
        interface_type_list_(NULL),
        klass_(c) {
  }

  void ChangeClass(const Class* new_c) {
    DCHECK(new_c != NULL);
    if (dex_cache_ != NULL) {
      DexCache* new_c_dex_cache = new_c->GetDexCache();
      if (new_c_dex_cache != dex_cache_) {
        dex_cache_ = new_c_dex_cache;
        dex_file_ = NULL;
      }
    }
    klass_ = new_c;
    interface_type_list_ = NULL;
    class_def_ = NULL;
  }

  // The returned const char* is only guaranteed to be valid for the lifetime of the ClassHelper.
  // If you need it longer, copy it into a std::string.
  const char* GetDescriptor() {
    if (UNLIKELY(klass_->IsArrayClass())) {
      return GetArrayDescriptor();
    } else if (UNLIKELY(klass_->IsPrimitive())) {
      return Primitive::Descriptor(klass_->GetPrimitiveType());
    } else if (UNLIKELY(klass_->IsProxyClass())) {
      descriptor_ = GetClassLinker()->GetDescriptorForProxy(klass_);
      return descriptor_.c_str();
    } else {
      const DexFile& dex_file = GetDexFile();
      const DexFile::TypeId& type_id = dex_file.GetTypeId(klass_->GetDexTypeIndex());
      return dex_file.GetTypeDescriptor(type_id);
    }
  }

  const char* GetArrayDescriptor() {
    std::string result("[");
    const Class* saved_klass = klass_;
    ChangeClass(klass_->GetComponentType());
    result += GetDescriptor();
    ChangeClass(saved_klass);
    descriptor_ = result;
    return descriptor_.c_str();
  }

  const DexFile::ClassDef* GetClassDef() {
    const DexFile::ClassDef* result = class_def_;
    if (result == NULL) {
      result = GetDexFile().FindClassDef(GetDescriptor());
      class_def_ = result;
    }
    return result;
  }

  uint32_t NumInterfaces() {
    if (klass_->IsPrimitive()) {
      return 0;
    } else if (klass_->IsArrayClass()) {
      return 2;
    } else if (klass_->IsProxyClass()) {
      return klass_->GetIfTable()->GetLength();
    } else {
      const DexFile::TypeList* interfaces = GetInterfaceTypeList();
      if (interfaces == NULL) {
        return 0;
      } else {
        return interfaces->Size();
      }
    }
  }

  uint16_t GetInterfaceTypeIdx(uint32_t idx) {
    DCHECK(!klass_->IsPrimitive());
    DCHECK(!klass_->IsArrayClass());
    return GetInterfaceTypeList()->GetTypeItem(idx).type_idx_;
  }

  Class* GetInterface(uint32_t idx) {
    DCHECK(!klass_->IsPrimitive());
    if (klass_->IsArrayClass()) {
      if (idx == 0) {
        return GetClassLinker()->FindSystemClass("Ljava/lang/Cloneable;");
      } else {
        DCHECK_EQ(1U, idx);
        return GetClassLinker()->FindSystemClass("Ljava/io/Serializable;");
      }
    } else if (klass_->IsProxyClass()) {
      return klass_->GetIfTable()->Get(idx)->GetInterface();
    } else {
      uint16_t type_idx = GetInterfaceTypeIdx(idx);
      Class* interface = GetDexCache()->GetResolvedType(type_idx);
      if (interface == NULL) {
        interface = GetClassLinker()->ResolveType(GetDexFile(), type_idx, klass_);
        CHECK(interface != NULL || Thread::Current()->IsExceptionPending());
      }
      return interface;
    }
  }

  const char* GetSourceFile() {
    std::string descriptor(GetDescriptor());
    const DexFile& dex_file = GetDexFile();
    const DexFile::ClassDef* dex_class_def = dex_file.FindClassDef(descriptor);
    CHECK(dex_class_def != NULL);
    return dex_file.GetSourceFile(*dex_class_def);
  }

  std::string GetLocation() {
    return GetDexCache()->GetLocation()->ToModifiedUtf8();
  }

  const DexFile& GetDexFile() {
    const DexFile* result = dex_file_;
    if (result == NULL) {
      const DexCache* dex_cache = GetDexCache();
      result = &GetClassLinker()->FindDexFile(dex_cache);
      dex_file_ = result;
    }
    return *result;
  }

 private:
  const DexFile::TypeList* GetInterfaceTypeList() {
    const DexFile::TypeList* result = interface_type_list_;
    if (result == NULL) {
      const DexFile::ClassDef* class_def = GetClassDef();
      if (class_def != NULL) {
        result =  GetDexFile().GetInterfacesList(*class_def);
        interface_type_list_ = result;
      }
    }
    return result;
  }

  DexCache* GetDexCache() {
    DexCache* result = dex_cache_;
    if (result == NULL) {
      result = klass_->GetDexCache();
      dex_cache_ = result;
    }
    return result;
  }

  ClassLinker* GetClassLinker() {
    ClassLinker* result = class_linker_;
    if (result == NULL) {
      result = Runtime::Current()->GetClassLinker();
      class_linker_ = result;
    }
    return result;
  }

  const DexFile::ClassDef* class_def_;
  ClassLinker* class_linker_;
  DexCache* dex_cache_;
  const DexFile* dex_file_;
  const DexFile::TypeList* interface_type_list_;
  const Class* klass_;
  std::string descriptor_;

  DISALLOW_COPY_AND_ASSIGN(ClassHelper);
};

class FieldHelper {
 public:
  FieldHelper() : class_linker_(NULL), dex_cache_(NULL), dex_file_(NULL), field_(NULL) {}
  explicit FieldHelper(const Field* f) : class_linker_(NULL), dex_cache_(NULL), dex_file_(NULL), field_(f) {}
  FieldHelper(const Field* f, ClassLinker* l) : class_linker_(l), dex_cache_(NULL), dex_file_(NULL),
      field_(f) {}

  void ChangeField(const Field* new_f) {
    DCHECK(new_f != NULL);
    if (dex_cache_ != NULL) {
      DexCache* new_f_dex_cache = new_f->GetDeclaringClass()->GetDexCache();
      if (new_f_dex_cache != dex_cache_) {
        dex_cache_ = new_f_dex_cache;
        dex_file_ = NULL;
      }
    }
    field_ = new_f;
  }
  const char* GetName() {
    uint32_t field_index = field_->GetDexFieldIndex();
    if (field_index != DexFile::kDexNoIndex) {
      const DexFile& dex_file = GetDexFile();
      return dex_file.GetFieldName(dex_file.GetFieldId(field_index));
    } else {
      // Proxy classes have a single static field called "throws"
      CHECK(field_->GetDeclaringClass()->IsProxyClass());
      DCHECK(field_->IsStatic());
      return "throws";
    }
  }
  String* GetNameAsString() {
    uint32_t field_index = field_->GetDexFieldIndex();
    if (field_index != DexFile::kDexNoIndex) {
      const DexFile& dex_file = GetDexFile();
      const DexFile::FieldId& field_id = dex_file.GetFieldId(field_index);
      return GetClassLinker()->ResolveString(dex_file, field_id.name_idx_, GetDexCache());
    } else {
      // Proxy classes have a single static field called "throws"
      CHECK(field_->GetDeclaringClass()->IsProxyClass());
      DCHECK(field_->IsStatic());
      return Runtime::Current()->GetInternTable()->InternStrong("throws");
    }
  }
  Class* GetType() {
    uint32_t field_index = field_->GetDexFieldIndex();
    if (field_index != DexFile::kDexNoIndex) {
      const DexFile& dex_file = GetDexFile();
      const DexFile::FieldId& field_id = dex_file.GetFieldId(field_index);
      Class* type = GetDexCache()->GetResolvedType(field_id.type_idx_);
      if (type == NULL) {
        type = GetClassLinker()->ResolveType(field_id.type_idx_, field_);
        CHECK(type != NULL || Thread::Current()->IsExceptionPending());
      }
      return type;
    } else {
      // Proxy classes have a single static field called "throws" whose type is Class[][]
      CHECK(field_->GetDeclaringClass()->IsProxyClass());
      DCHECK(field_->IsStatic());
      return GetClassLinker()->FindSystemClass("[[Ljava/lang/Class;");
    }
  }
  const char* GetTypeDescriptor() {
    uint32_t field_index = field_->GetDexFieldIndex();
    if (field_index != DexFile::kDexNoIndex) {
      const DexFile& dex_file = GetDexFile();
      const DexFile::FieldId& field_id = dex_file.GetFieldId(field_index);
      return dex_file.GetFieldTypeDescriptor(field_id);
    } else {
      // Proxy classes have a single static field called "throws" whose type is Class[][]
      CHECK(field_->GetDeclaringClass()->IsProxyClass());
      DCHECK(field_->IsStatic());
      return "[[Ljava/lang/Class;";
    }
  }
  Primitive::Type GetTypeAsPrimitiveType() {
    return Primitive::GetType(GetTypeDescriptor()[0]);
  }
  bool IsPrimitiveType() {
    Primitive::Type type = GetTypeAsPrimitiveType();
    return type != Primitive::kPrimNot;
  }
  size_t FieldSize() {
    Primitive::Type type = GetTypeAsPrimitiveType();
    return Primitive::FieldSize(type);
  }

  // The returned const char* is only guaranteed to be valid for the lifetime of the FieldHelper.
  // If you need it longer, copy it into a std::string.
  const char* GetDeclaringClassDescriptor() {
    uint16_t type_idx = field_->GetDeclaringClass()->GetDexTypeIndex();
    if (type_idx != DexFile::kDexNoIndex16) {
      const DexFile& dex_file = GetDexFile();
      return dex_file.GetTypeDescriptor(dex_file.GetTypeId(type_idx));
    } else {
      // Most likely a proxy class
      ClassHelper kh(field_->GetDeclaringClass());
      declaring_class_descriptor_ = kh.GetDescriptor();
      return declaring_class_descriptor_.c_str();
    }
  }

 private:
  DexCache* GetDexCache() {
    DexCache* result = dex_cache_;
    if (result == NULL) {
      result = field_->GetDeclaringClass()->GetDexCache();
      dex_cache_ = result;
    }
    return result;
  }
  ClassLinker* GetClassLinker() {
    ClassLinker* result = class_linker_;
    if (result == NULL) {
      result = Runtime::Current()->GetClassLinker();
      class_linker_ = result;
    }
    return result;
  }
  const DexFile& GetDexFile() {
    const DexFile* result = dex_file_;
    if (result == NULL) {
      const DexCache* dex_cache = GetDexCache();
      result = &GetClassLinker()->FindDexFile(dex_cache);
      dex_file_ = result;
    }
    return *result;
  }

  ClassLinker* class_linker_;
  DexCache* dex_cache_;
  const DexFile* dex_file_;
  const Field* field_;
  std::string declaring_class_descriptor_;

  DISALLOW_COPY_AND_ASSIGN(FieldHelper);
};

class MethodHelper {
 public:
  MethodHelper() : class_linker_(NULL), dex_cache_(NULL), dex_file_(NULL), method_(NULL),
      shorty_(NULL), shorty_len_(0) {}
  explicit MethodHelper(const Method* m) : class_linker_(NULL), dex_cache_(NULL), dex_file_(NULL),
      method_(NULL), shorty_(NULL), shorty_len_(0) {
    SetMethod(m);
  }
  MethodHelper(const Method* m, ClassLinker* l) : class_linker_(l), dex_cache_(NULL),
      dex_file_(NULL), method_(NULL), shorty_(NULL), shorty_len_(0) {
    SetMethod(m);
  }

  void ChangeMethod(Method* new_m) {
    DCHECK(new_m != NULL);
    if (dex_cache_ != NULL) {
      Class* klass = new_m->GetDeclaringClass();
      if (klass->IsProxyClass()) {
        dex_cache_ = NULL;
        dex_file_ = NULL;
      } else {
        DexCache* new_m_dex_cache = klass->GetDexCache();
        if (new_m_dex_cache != dex_cache_) {
          dex_cache_ = new_m_dex_cache;
          dex_file_ = NULL;
        }
      }
    }
    SetMethod(new_m);
    shorty_ = NULL;
  }
  const char* GetName() {
    const DexFile& dex_file = GetDexFile();
    return dex_file.GetMethodName(dex_file.GetMethodId(method_->GetDexMethodIndex()));
  }
  String* GetNameAsString() {
    const DexFile& dex_file = GetDexFile();
    const DexFile::MethodId& method_id = dex_file.GetMethodId(method_->GetDexMethodIndex());
    return GetClassLinker()->ResolveString(dex_file, method_id.name_idx_, GetDexCache());
  }
  const char* GetShorty() {
    const char* result = shorty_;
    if (result == NULL) {
      const DexFile& dex_file = GetDexFile();
      result = dex_file.GetMethodShorty(dex_file.GetMethodId(method_->GetDexMethodIndex()),
                                        &shorty_len_);
      shorty_ = result;
    }
    return result;
  }
  int32_t GetShortyLength() {
    if (shorty_ == NULL) {
      GetShorty();
    }
    return shorty_len_;
  }
  const std::string GetSignature() {
    const DexFile& dex_file = GetDexFile();
    return dex_file.GetMethodSignature(dex_file.GetMethodId(method_->GetDexMethodIndex()));
  }
  const DexFile::ProtoId& GetPrototype() {
    const DexFile& dex_file = GetDexFile();
    return dex_file.GetMethodPrototype(dex_file.GetMethodId(method_->GetDexMethodIndex()));
  }
  const DexFile::TypeList* GetParameterTypeList() {
    const DexFile::ProtoId& proto = GetPrototype();
    return GetDexFile().GetProtoParameters(proto);
  }
  ObjectArray<Class>* GetParameterTypes() {
    const DexFile::TypeList* params = GetParameterTypeList();
    Class* array_class = GetClassLinker()->FindSystemClass("[Ljava/lang/Class;");
    uint32_t num_params = params == NULL ? 0 : params->Size();
    ObjectArray<Class>* result = ObjectArray<Class>::Alloc(array_class, num_params);
    for (uint32_t i = 0; i < num_params; i++) {
      Class* param_type = GetClassFromTypeIdx(params->GetTypeItem(i).type_idx_);
      result->Set(i, param_type);
    }
    return result;
  }
  Class* GetReturnType() {
    const DexFile& dex_file = GetDexFile();
    const DexFile::MethodId& method_id = dex_file.GetMethodId(method_->GetDexMethodIndex());
    const DexFile::ProtoId& proto_id = dex_file.GetMethodPrototype(method_id);
    uint16_t return_type_idx = proto_id.return_type_idx_;
    return GetClassFromTypeIdx(return_type_idx);
  }
  const char* GetReturnTypeDescriptor() {
    const DexFile& dex_file = GetDexFile();
    const DexFile::MethodId& method_id = dex_file.GetMethodId(method_->GetDexMethodIndex());
    const DexFile::ProtoId& proto_id = dex_file.GetMethodPrototype(method_id);
    uint16_t return_type_idx = proto_id.return_type_idx_;
    return dex_file.GetTypeDescriptor(dex_file.GetTypeId(return_type_idx));
  }
  int32_t GetLineNumFromNativePC(uintptr_t raw_pc) {
    const DexFile& dex_file = GetDexFile();
    return dex_file.GetLineNumFromPC(method_, method_->ToDexPC(raw_pc));
  }
  const char* GetDeclaringClassDescriptor() {
    Class* klass = method_->GetDeclaringClass();
    DCHECK(!klass->IsProxyClass());
    uint16_t type_idx = klass->GetDexTypeIndex();
    const DexFile& dex_file = GetDexFile();
    return dex_file.GetTypeDescriptor(dex_file.GetTypeId(type_idx));
  }
  const char* GetDeclaringClassSourceFile() {
    const char* descriptor = GetDeclaringClassDescriptor();
    const DexFile& dex_file = GetDexFile();
    const DexFile::ClassDef* dex_class_def = dex_file.FindClassDef(descriptor);
    CHECK(dex_class_def != NULL);
    return dex_file.GetSourceFile(*dex_class_def);
  }
  bool IsStatic() {
    return method_->IsStatic();
  }
  bool IsClassInitializer() {
    return IsStatic() && StringPiece(GetName()) == "<clinit>";
  }
  size_t NumArgs() {
    // "1 +" because the first in Args is the receiver.
    // "- 1" because we don't count the return type.
    return (IsStatic() ? 0 : 1) + GetShortyLength() - 1;
  }
  // Is the specified parameter a long or double, where parameter 0 is 'this' for instance methods
  bool IsParamALongOrDouble(size_t param) {
    CHECK_LT(param, NumArgs());
    if (IsStatic()) {
      param++;  // 0th argument must skip return value at start of the shorty
    } else if (param == 0) {
      return false;  // this argument
    }
    char ch = GetShorty()[param];
    return (ch == 'J' || ch == 'D');
  }
  // Is the specified parameter a reference, where parameter 0 is 'this' for instance methods
  bool IsParamAReference(size_t param) {
    CHECK_LT(param, NumArgs());
    if (IsStatic()) {
      param++;  // 0th argument must skip return value at start of the shorty
    } else if (param == 0) {
      return true;  // this argument
    }
    return GetShorty()[param] == 'L';  // An array also has a shorty character of 'L' (not '[')
  }
  bool HasSameNameAndSignature(MethodHelper* other) {
    if (GetDexCache() == other->GetDexCache()) {
      const DexFile& dex_file = GetDexFile();
      const DexFile::MethodId& mid = dex_file.GetMethodId(method_->GetDexMethodIndex());
      const DexFile::MethodId& other_mid =
          dex_file.GetMethodId(other->method_->GetDexMethodIndex());
      return mid.name_idx_ == other_mid.name_idx_ && mid.proto_idx_ == other_mid.proto_idx_;
    }
    StringPiece name(GetName());
    StringPiece other_name(other->GetName());
    return name == other_name && GetSignature() == other->GetSignature();
  }
  const DexFile::CodeItem* GetCodeItem() {
    return GetDexFile().GetCodeItem(method_->GetCodeItemOffset());
  }
  bool IsResolvedTypeIdx(uint16_t type_idx) const {
    return method_->GetDexCacheResolvedTypes()->Get(type_idx) != NULL;
  }
  Class* GetClassFromTypeIdx(uint16_t type_idx) {
    Class* type = method_->GetDexCacheResolvedTypes()->Get(type_idx);
    if (type == NULL) {
      type = GetClassLinker()->ResolveType(type_idx, method_);
      CHECK(type != NULL || Thread::Current()->IsExceptionPending());
    }
    return type;
  }
  const char* GetTypeDescriptorFromTypeIdx(uint16_t type_idx) {
    const DexFile& dex_file = GetDexFile();
    return dex_file.GetTypeDescriptor(dex_file.GetTypeId(type_idx));
  }
  Class* GetDexCacheResolvedType(uint16_t type_idx) {
    return GetDexCache()->GetResolvedType(type_idx);
  }
  const DexFile& GetDexFile() {
    const DexFile* result = dex_file_;
    if (result == NULL) {
      const DexCache* dex_cache = GetDexCache();
      result = &GetClassLinker()->FindDexFile(dex_cache);
      dex_file_ = result;
    }
    return *result;
  }
 private:
  // Set the method_ field, for proxy methods looking up the interface method via the resolved
  // methods table.
  void SetMethod(const Method* method) {
    if (method != NULL) {
      Class* klass = method->GetDeclaringClass();
      if (klass->IsProxyClass()) {
        method = GetClassLinker()->FindMethodForProxy(klass, method);
        CHECK(method != NULL);
      }
    }
    method_ = method;
  }
  DexCache* GetDexCache() {
    DexCache* result = dex_cache_;
    if (result == NULL) {
      Class* klass = method_->GetDeclaringClass();
      result = klass->GetDexCache();
      dex_cache_ = result;
    }
    return result;
  }
  ClassLinker* GetClassLinker() {
    ClassLinker* result = class_linker_;
    if (result == NULL) {
      result = Runtime::Current()->GetClassLinker();
      class_linker_ = result;
    }
    return result;
  }

  ClassLinker* class_linker_;
  DexCache* dex_cache_;
  const DexFile* dex_file_;
  const Method* method_;
  const char* shorty_;
  int32_t shorty_len_;

  DISALLOW_COPY_AND_ASSIGN(MethodHelper);
};

}  // namespace art

#endif  // ART_SRC_OBJECT_UTILS_H_
