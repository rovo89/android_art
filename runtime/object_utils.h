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

#ifndef ART_RUNTIME_OBJECT_UTILS_H_
#define ART_RUNTIME_OBJECT_UTILS_H_

#include "class_linker.h"
#include "dex_file.h"
#include "monitor.h"
#include "mirror/art_field.h"
#include "mirror/art_method.h"
#include "mirror/class.h"
#include "mirror/dex_cache.h"
#include "mirror/iftable.h"
#include "mirror/proxy.h"
#include "mirror/string.h"

#include "runtime.h"
#include "handle_scope-inl.h"

#include <string>

namespace art {

template <typename T>
class ObjectLock {
 public:
  ObjectLock(Thread* self, Handle<T> object) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : self_(self), obj_(object) {
    CHECK(object.Get() != nullptr);
    obj_->MonitorEnter(self_);
  }

  ~ObjectLock() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    obj_->MonitorExit(self_);
  }

  void WaitIgnoringInterrupts() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    Monitor::Wait(self_, obj_.Get(), 0, 0, false, kWaiting);
  }

  void Notify() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    obj_->Notify(self_);
  }

  void NotifyAll() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    obj_->NotifyAll(self_);
  }

 private:
  Thread* const self_;
  Handle<T> const obj_;
  DISALLOW_COPY_AND_ASSIGN(ObjectLock);
};

class FieldHelper {
 public:
  explicit FieldHelper(Handle<mirror::ArtField> f) : field_(f) {}

  void ChangeField(mirror::ArtField* new_f) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    DCHECK(new_f != nullptr);
    field_.Assign(new_f);
  }

  mirror::ArtField* GetField() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return field_.Get();
  }

  mirror::Class* GetType(bool resolve = true) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    uint32_t field_index = field_->GetDexFieldIndex();
    if (UNLIKELY(field_->GetDeclaringClass()->IsProxyClass())) {
      return Runtime::Current()->GetClassLinker()->FindSystemClass(Thread::Current(),
                                                                   field_->GetTypeDescriptor());
    }
    const DexFile* dex_file = field_->GetDexFile();
    const DexFile::FieldId& field_id = dex_file->GetFieldId(field_index);
    mirror::Class* type = field_->GetDexCache()->GetResolvedType(field_id.type_idx_);
    if (resolve && (type == nullptr)) {
      type = Runtime::Current()->GetClassLinker()->ResolveType(field_id.type_idx_, field_.Get());
      CHECK(type != nullptr || Thread::Current()->IsExceptionPending());
    }
    return type;
  }

  // The returned const char* is only guaranteed to be valid for the lifetime of the FieldHelper.
  // If you need it longer, copy it into a std::string.
  const char* GetDeclaringClassDescriptor()
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    uint32_t field_index = field_->GetDexFieldIndex();
    if (UNLIKELY(field_->GetDeclaringClass()->IsProxyClass())) {
      DCHECK(field_->IsStatic());
      DCHECK_LT(field_index, 2U);
      // 0 == Class[] interfaces; 1 == Class[][] throws;
      declaring_class_descriptor_ = field_->GetDeclaringClass()->GetDescriptor();
      return declaring_class_descriptor_.c_str();
    }
    const DexFile* dex_file = field_->GetDexFile();
    const DexFile::FieldId& field_id = dex_file->GetFieldId(field_index);
    return dex_file->GetFieldDeclaringClassDescriptor(field_id);
  }

 private:
  Handle<mirror::ArtField> field_;
  std::string declaring_class_descriptor_;

  DISALLOW_COPY_AND_ASSIGN(FieldHelper);
};

class MethodHelper {
 public:
  explicit MethodHelper(Handle<mirror::ArtMethod> m) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
      : method_(m), shorty_(nullptr), shorty_len_(0) {
    SetMethod(m.Get());
  }

  void ChangeMethod(mirror::ArtMethod* new_m) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    DCHECK(new_m != nullptr);
    SetMethod(new_m);
    shorty_ = nullptr;
  }

  mirror::ArtMethod* GetMethod() const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return method_->GetInterfaceMethodIfProxy();
  }

  mirror::String* GetNameAsString(Thread* self) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    const DexFile* dex_file = method_->GetDexFile();
    mirror::ArtMethod* method = method_->GetInterfaceMethodIfProxy();
    uint32_t dex_method_idx = method->GetDexMethodIndex();
    const DexFile::MethodId& method_id = dex_file->GetMethodId(dex_method_idx);
    StackHandleScope<1> hs(self);
    Handle<mirror::DexCache> dex_cache(hs.NewHandle(method->GetDexCache()));
    return Runtime::Current()->GetClassLinker()->ResolveString(*dex_file, method_id.name_idx_,
                                                               dex_cache);
  }

  const char* GetShorty() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    const char* result = shorty_;
    if (result == nullptr) {
      result = method_->GetShorty(&shorty_len_);
      shorty_ = result;
    }
    return result;
  }

  uint32_t GetShortyLength() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    if (shorty_ == nullptr) {
      GetShorty();
    }
    return shorty_len_;
  }

  // Counts the number of references in the parameter list of the corresponding method.
  // Note: Thus does _not_ include "this" for non-static methods.
  uint32_t GetNumberOfReferenceArgsWithoutReceiver() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    const char* shorty = GetShorty();
    uint32_t refs = 0;
    for (uint32_t i = 1; i < shorty_len_ ; ++i) {
      if (shorty[i] == 'L') {
        refs++;
      }
    }

    return refs;
  }

  // May cause thread suspension due to GetClassFromTypeIdx calling ResolveType this caused a large
  // number of bugs at call sites.
  mirror::Class* GetReturnType(bool resolve = true) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    mirror::ArtMethod* method = GetMethod();
    const DexFile* dex_file = method->GetDexFile();
    const DexFile::MethodId& method_id = dex_file->GetMethodId(method->GetDexMethodIndex());
    const DexFile::ProtoId& proto_id = dex_file->GetMethodPrototype(method_id);
    uint16_t return_type_idx = proto_id.return_type_idx_;
    return GetClassFromTypeIdx(return_type_idx, resolve);
  }

  size_t NumArgs() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    // "1 +" because the first in Args is the receiver.
    // "- 1" because we don't count the return type.
    return (method_->IsStatic() ? 0 : 1) + GetShortyLength() - 1;
  }

  // Get the primitive type associated with the given parameter.
  Primitive::Type GetParamPrimitiveType(size_t param) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    CHECK_LT(param, NumArgs());
    if (GetMethod()->IsStatic()) {
      param++;  // 0th argument must skip return value at start of the shorty
    } else if (param == 0) {
      return Primitive::kPrimNot;
    }
    return Primitive::GetType(GetShorty()[param]);
  }

  // Is the specified parameter a long or double, where parameter 0 is 'this' for instance methods.
  bool IsParamALongOrDouble(size_t param) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    Primitive::Type type = GetParamPrimitiveType(param);
    return type == Primitive::kPrimLong || type == Primitive::kPrimDouble;
  }

  // Is the specified parameter a reference, where parameter 0 is 'this' for instance methods.
  bool IsParamAReference(size_t param) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return GetParamPrimitiveType(param) == Primitive::kPrimNot;
  }

  bool HasSameNameAndSignature(MethodHelper* other) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    const DexFile* dex_file = method_->GetDexFile();
    const DexFile::MethodId& mid = dex_file->GetMethodId(GetMethod()->GetDexMethodIndex());
    if (method_->GetDexCache() == other->method_->GetDexCache()) {
      const DexFile::MethodId& other_mid =
          dex_file->GetMethodId(other->GetMethod()->GetDexMethodIndex());
      return mid.name_idx_ == other_mid.name_idx_ && mid.proto_idx_ == other_mid.proto_idx_;
    }
    const DexFile* other_dex_file = other->method_->GetDexFile();
    const DexFile::MethodId& other_mid =
        other_dex_file->GetMethodId(other->GetMethod()->GetDexMethodIndex());
    if (!DexFileStringEquals(dex_file, mid.name_idx_, other_dex_file, other_mid.name_idx_)) {
      return false;  // Name mismatch.
    }
    return dex_file->GetMethodSignature(mid) == other_dex_file->GetMethodSignature(other_mid);
  }

  bool HasSameSignatureWithDifferentClassLoaders(MethodHelper* other)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    if (UNLIKELY(GetReturnType() != other->GetReturnType())) {
      return false;
    }
    const DexFile::TypeList* types = method_->GetParameterTypeList();
    const DexFile::TypeList* other_types = other->method_->GetParameterTypeList();
    if (types == nullptr) {
      return (other_types == nullptr) || (other_types->Size() == 0);
    } else if (UNLIKELY(other_types == nullptr)) {
      return types->Size() == 0;
    }
    uint32_t num_types = types->Size();
    if (UNLIKELY(num_types != other_types->Size())) {
      return false;
    }
    for (uint32_t i = 0; i < num_types; ++i) {
      mirror::Class* param_type = GetClassFromTypeIdx(types->GetTypeItem(i).type_idx_);
      mirror::Class* other_param_type =
          other->GetClassFromTypeIdx(other_types->GetTypeItem(i).type_idx_);
      if (UNLIKELY(param_type != other_param_type)) {
        return false;
      }
    }
    return true;
  }

  mirror::Class* GetClassFromTypeIdx(uint16_t type_idx, bool resolve = true)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    mirror::ArtMethod* method = GetMethod();
    mirror::Class* type = method->GetDexCacheResolvedTypes()->Get(type_idx);
    if (type == nullptr && resolve) {
      type = Runtime::Current()->GetClassLinker()->ResolveType(type_idx, method);
      CHECK(type != nullptr || Thread::Current()->IsExceptionPending());
    }
    return type;
  }

  mirror::Class* GetDexCacheResolvedType(uint16_t type_idx)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return GetMethod()->GetDexCacheResolvedTypes()->Get(type_idx);
  }

  mirror::String* ResolveString(uint32_t string_idx) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    mirror::ArtMethod* method = GetMethod();
    mirror::String* s = method->GetDexCacheStrings()->Get(string_idx);
    if (UNLIKELY(s == nullptr)) {
      StackHandleScope<1> hs(Thread::Current());
      Handle<mirror::DexCache> dex_cache(hs.NewHandle(method->GetDexCache()));
      s = Runtime::Current()->GetClassLinker()->ResolveString(*method->GetDexFile(), string_idx,
                                                              dex_cache);
    }
    return s;
  }

  uint32_t FindDexMethodIndexInOtherDexFile(const DexFile& other_dexfile)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    mirror::ArtMethod* method = GetMethod();
    const DexFile* dexfile = method->GetDexFile();
    if (dexfile == &other_dexfile) {
      return method->GetDexMethodIndex();
    }
    const DexFile::MethodId& mid = dexfile->GetMethodId(method->GetDexMethodIndex());
    const char* mid_declaring_class_descriptor = dexfile->StringByTypeIdx(mid.class_idx_);
    const DexFile::StringId* other_descriptor =
        other_dexfile.FindStringId(mid_declaring_class_descriptor);
    if (other_descriptor != nullptr) {
      const DexFile::TypeId* other_type_id =
          other_dexfile.FindTypeId(other_dexfile.GetIndexForStringId(*other_descriptor));
      if (other_type_id != nullptr) {
        const char* mid_name = dexfile->GetMethodName(mid);
        const DexFile::StringId* other_name = other_dexfile.FindStringId(mid_name);
        if (other_name != nullptr) {
          uint16_t other_return_type_idx;
          std::vector<uint16_t> other_param_type_idxs;
          bool success = other_dexfile.CreateTypeList(
              dexfile->GetMethodSignature(mid).ToString(), &other_return_type_idx,
              &other_param_type_idxs);
          if (success) {
            const DexFile::ProtoId* other_sig =
                other_dexfile.FindProtoId(other_return_type_idx, other_param_type_idxs);
            if (other_sig != nullptr) {
              const  DexFile::MethodId* other_mid = other_dexfile.FindMethodId(
                  *other_type_id, *other_name, *other_sig);
              if (other_mid != nullptr) {
                return other_dexfile.GetIndexForMethodId(*other_mid);
              }
            }
          }
        }
      }
    }
    return DexFile::kDexNoIndex;
  }

  // The name_and_signature_idx MUST point to a MethodId with the same name and signature in the
  // other_dexfile, such as the method index used to resolve this method in the other_dexfile.
  uint32_t FindDexMethodIndexInOtherDexFile(const DexFile& other_dexfile,
                                            uint32_t name_and_signature_idx)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    mirror::ArtMethod* method = GetMethod();
    const DexFile* dexfile = method->GetDexFile();
    const uint32_t dex_method_idx = method->GetDexMethodIndex();
    const DexFile::MethodId& mid = dexfile->GetMethodId(dex_method_idx);
    const DexFile::MethodId& name_and_sig_mid = other_dexfile.GetMethodId(name_and_signature_idx);
    DCHECK_STREQ(dexfile->GetMethodName(mid), other_dexfile.GetMethodName(name_and_sig_mid));
    DCHECK_EQ(dexfile->GetMethodSignature(mid), other_dexfile.GetMethodSignature(name_and_sig_mid));
    if (dexfile == &other_dexfile) {
      return dex_method_idx;
    }
    const char* mid_declaring_class_descriptor = dexfile->StringByTypeIdx(mid.class_idx_);
    const DexFile::StringId* other_descriptor =
        other_dexfile.FindStringId(mid_declaring_class_descriptor);
    if (other_descriptor != nullptr) {
      const DexFile::TypeId* other_type_id =
          other_dexfile.FindTypeId(other_dexfile.GetIndexForStringId(*other_descriptor));
      if (other_type_id != nullptr) {
        const DexFile::MethodId* other_mid = other_dexfile.FindMethodId(
            *other_type_id, other_dexfile.GetStringId(name_and_sig_mid.name_idx_),
            other_dexfile.GetProtoId(name_and_sig_mid.proto_idx_));
        if (other_mid != nullptr) {
          return other_dexfile.GetIndexForMethodId(*other_mid);
        }
      }
    }
    return DexFile::kDexNoIndex;
  }

 private:
  // Set the method_ field, for proxy methods looking up the interface method via the resolved
  // methods table.
  void SetMethod(mirror::ArtMethod* method) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    method_.Assign(method);
  }

  Handle<mirror::ArtMethod> method_;
  const char* shorty_;
  uint32_t shorty_len_;

  DISALLOW_COPY_AND_ASSIGN(MethodHelper);
};

}  // namespace art

#endif  // ART_RUNTIME_OBJECT_UTILS_H_
