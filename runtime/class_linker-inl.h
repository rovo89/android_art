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

#ifndef ART_RUNTIME_CLASS_LINKER_INL_H_
#define ART_RUNTIME_CLASS_LINKER_INL_H_

#include "class_linker.h"
#include "mirror/art_field.h"
#include "mirror/class_loader.h"
#include "mirror/dex_cache.h"
#include "mirror/iftable.h"
#include "mirror/object_array.h"
#include "object_utils.h"
#include "handle_scope-inl.h"

namespace art {

inline bool ClassLinker::IsInBootClassPath(const char* descriptor) {
  DexFile::ClassPathEntry pair = DexFile::FindInClassPath(descriptor, boot_class_path_);
  return pair.second != nullptr;
}

inline mirror::Class* ClassLinker::FindSystemClass(Thread* self, const char* descriptor) {
  StackHandleScope<1> hs(self);
  auto class_loader = hs.NewHandle<mirror::ClassLoader>(nullptr);
  return FindClass(self, descriptor, class_loader);
}

inline mirror::Class* ClassLinker::FindArrayClass(Thread* self, mirror::Class* element_class) {
  for (size_t i = 0; i < kFindArrayCacheSize; ++i) {
    // Read the cached array class once to avoid races with other threads setting it.
    mirror::Class* array_class = find_array_class_cache_[i];
    if (array_class != nullptr && array_class->GetComponentType() == element_class) {
      return array_class;
    }
  }
  DCHECK(!element_class->IsPrimitiveVoid());
  std::string descriptor("[");
  descriptor += element_class->GetDescriptor();
  StackHandleScope<1> hs(Thread::Current());
  Handle<mirror::ClassLoader> class_loader(hs.NewHandle(element_class->GetClassLoader()));
  mirror::Class* array_class = FindClass(self, descriptor.c_str(), class_loader);
  // Benign races in storing array class and incrementing index.
  size_t victim_index = find_array_class_cache_next_victim_;
  find_array_class_cache_[victim_index] = array_class;
  find_array_class_cache_next_victim_ = (victim_index + 1) % kFindArrayCacheSize;
  return array_class;
}

inline mirror::String* ClassLinker::ResolveString(uint32_t string_idx,
                                                  mirror::ArtMethod* referrer) {
  mirror::String* resolved_string = referrer->GetDexCacheStrings()->Get(string_idx);
  if (UNLIKELY(resolved_string == NULL)) {
    mirror::Class* declaring_class = referrer->GetDeclaringClass();
    StackHandleScope<1> hs(Thread::Current());
    Handle<mirror::DexCache> dex_cache(hs.NewHandle(declaring_class->GetDexCache()));
    const DexFile& dex_file = *dex_cache->GetDexFile();
    resolved_string = ResolveString(dex_file, string_idx, dex_cache);
    if (resolved_string != nullptr) {
      DCHECK_EQ(dex_cache->GetResolvedString(string_idx), resolved_string);
    }
  }
  return resolved_string;
}

inline mirror::Class* ClassLinker::ResolveType(uint16_t type_idx,
                                               mirror::ArtMethod* referrer) {
  mirror::Class* resolved_type = referrer->GetDexCacheResolvedTypes()->Get(type_idx);
  if (UNLIKELY(resolved_type == nullptr)) {
    mirror::Class* declaring_class = referrer->GetDeclaringClass();
    StackHandleScope<2> hs(Thread::Current());
    Handle<mirror::DexCache> dex_cache(hs.NewHandle(declaring_class->GetDexCache()));
    Handle<mirror::ClassLoader> class_loader(hs.NewHandle(declaring_class->GetClassLoader()));
    const DexFile& dex_file = *dex_cache->GetDexFile();
    resolved_type = ResolveType(dex_file, type_idx, dex_cache, class_loader);
    if (resolved_type != nullptr) {
      DCHECK_EQ(dex_cache->GetResolvedType(type_idx), resolved_type);
    }
  }
  return resolved_type;
}

inline mirror::Class* ClassLinker::ResolveType(uint16_t type_idx, mirror::ArtField* referrer) {
  mirror::Class* declaring_class = referrer->GetDeclaringClass();
  mirror::DexCache* dex_cache_ptr = declaring_class->GetDexCache();
  mirror::Class* resolved_type = dex_cache_ptr->GetResolvedType(type_idx);
  if (UNLIKELY(resolved_type == NULL)) {
    StackHandleScope<2> hs(Thread::Current());
    Handle<mirror::DexCache> dex_cache(hs.NewHandle(dex_cache_ptr));
    Handle<mirror::ClassLoader> class_loader(hs.NewHandle(declaring_class->GetClassLoader()));
    const DexFile& dex_file = *dex_cache->GetDexFile();
    resolved_type = ResolveType(dex_file, type_idx, dex_cache, class_loader);
    if (resolved_type != nullptr) {
      DCHECK_EQ(dex_cache->GetResolvedType(type_idx), resolved_type);
    }
  }
  return resolved_type;
}

inline mirror::ArtMethod* ClassLinker::ResolveMethod(uint32_t method_idx,
                                                     mirror::ArtMethod* referrer,
                                                     InvokeType type) {
  mirror::ArtMethod* resolved_method =
      referrer->GetDexCacheResolvedMethods()->Get(method_idx);
  if (UNLIKELY(resolved_method == NULL || resolved_method->IsRuntimeMethod())) {
    mirror::Class* declaring_class = referrer->GetDeclaringClass();
    StackHandleScope<2> hs(Thread::Current());
    Handle<mirror::DexCache> dex_cache(hs.NewHandle(declaring_class->GetDexCache()));
    Handle<mirror::ClassLoader> class_loader(hs.NewHandle(declaring_class->GetClassLoader()));
    const DexFile& dex_file = *dex_cache->GetDexFile();
    resolved_method = ResolveMethod(dex_file, method_idx, dex_cache, class_loader, referrer, type);
    if (resolved_method != nullptr) {
      DCHECK_EQ(dex_cache->GetResolvedMethod(method_idx), resolved_method);
    }
  }
  return resolved_method;
}

inline mirror::ArtField* ClassLinker::ResolveField(uint32_t field_idx,
                                                   mirror::ArtMethod* referrer,
                                                   bool is_static) {
  mirror::Class* declaring_class = referrer->GetDeclaringClass();
  mirror::ArtField* resolved_field =
      declaring_class->GetDexCache()->GetResolvedField(field_idx);
  if (UNLIKELY(resolved_field == NULL)) {
    StackHandleScope<2> hs(Thread::Current());
    Handle<mirror::DexCache> dex_cache(hs.NewHandle(declaring_class->GetDexCache()));
    Handle<mirror::ClassLoader> class_loader(hs.NewHandle(declaring_class->GetClassLoader()));
    const DexFile& dex_file = *dex_cache->GetDexFile();
    resolved_field = ResolveField(dex_file, field_idx, dex_cache, class_loader, is_static);
    if (resolved_field != nullptr) {
      DCHECK_EQ(dex_cache->GetResolvedField(field_idx), resolved_field);
    }
  }
  return resolved_field;
}

template <class T>
inline mirror::ObjectArray<T>* ClassLinker::AllocObjectArray(Thread* self, size_t length) {
  return mirror::ObjectArray<T>::Alloc(self, GetClassRoot(kObjectArrayClass), length);
}

inline mirror::ObjectArray<mirror::Class>* ClassLinker::AllocClassArray(Thread* self,
                                                                        size_t length) {
  return mirror::ObjectArray<mirror::Class>::Alloc(self, GetClassRoot(kClassArrayClass), length);
}

inline mirror::ObjectArray<mirror::String>* ClassLinker::AllocStringArray(Thread* self,
                                                                          size_t length) {
  return mirror::ObjectArray<mirror::String>::Alloc(self, GetClassRoot(kJavaLangStringArrayClass),
                                                    length);
}

inline mirror::ObjectArray<mirror::ArtMethod>* ClassLinker::AllocArtMethodArray(Thread* self,
                                                                                size_t length) {
  return mirror::ObjectArray<mirror::ArtMethod>::Alloc(self,
      GetClassRoot(kJavaLangReflectArtMethodArrayClass), length);
}

inline mirror::IfTable* ClassLinker::AllocIfTable(Thread* self, size_t ifcount) {
  return down_cast<mirror::IfTable*>(
      mirror::IfTable::Alloc(self, GetClassRoot(kObjectArrayClass), ifcount * mirror::IfTable::kMax));
}

inline mirror::ObjectArray<mirror::ArtField>* ClassLinker::AllocArtFieldArray(Thread* self,
                                                                              size_t length) {
  return mirror::ObjectArray<mirror::ArtField>::Alloc(
      self, GetClassRoot(kJavaLangReflectArtFieldArrayClass), length);
}

inline mirror::Class* ClassLinker::GetClassRoot(ClassRoot class_root)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  DCHECK(class_roots_ != NULL);
  mirror::Class* klass = class_roots_->Get(class_root);
  DCHECK(klass != NULL);
  return klass;
}

}  // namespace art

#endif  // ART_RUNTIME_CLASS_LINKER_INL_H_
