/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include "class_table.h"

#include "mirror/class-inl.h"

namespace art {

ClassTable::ClassTable() {
  Runtime* const runtime = Runtime::Current();
  classes_.push_back(ClassSet(runtime->GetHashTableMinLoadFactor(),
                              runtime->GetHashTableMaxLoadFactor()));
}

void ClassTable::FreezeSnapshot() {
  classes_.push_back(ClassSet());
}

bool ClassTable::Contains(mirror::Class* klass) {
  for (ClassSet& class_set : classes_) {
    auto it = class_set.Find(GcRoot<mirror::Class>(klass));
    if (it != class_set.end()) {
      return it->Read() == klass;
    }
  }
  return false;
}

mirror::Class* ClassTable::UpdateClass(const char* descriptor, mirror::Class* klass, size_t hash) {
  // Should only be updating latest table.
  auto existing_it = classes_.back().FindWithHash(descriptor, hash);
  if (kIsDebugBuild && existing_it == classes_.back().end()) {
    for (const ClassSet& class_set : classes_) {
      if (class_set.FindWithHash(descriptor, hash) != class_set.end()) {
        LOG(FATAL) << "Updating class found in frozen table " << descriptor;
      }
    }
    LOG(FATAL) << "Updating class not found " << descriptor;
  }
  mirror::Class* const existing = existing_it->Read();
  CHECK_NE(existing, klass) << descriptor;
  CHECK(!existing->IsResolved()) << descriptor;
  CHECK_EQ(klass->GetStatus(), mirror::Class::kStatusResolving) << descriptor;
  CHECK(!klass->IsTemp()) << descriptor;
  VerifyObject(klass);
  // Update the element in the hash set with the new class. This is safe to do since the descriptor
  // doesn't change.
  *existing_it = GcRoot<mirror::Class>(klass);
  return existing;
}

bool ClassTable::Visit(ClassVisitor* visitor) {
  for (ClassSet& class_set : classes_) {
    for (GcRoot<mirror::Class>& root : class_set) {
      if (!visitor->Visit(root.Read())) {
        return false;
      }
    }
  }
  return true;
}

size_t ClassTable::NumZygoteClasses() const {
  size_t sum = 0;
  for (size_t i = 0; i < classes_.size() - 1; ++i) {
    sum += classes_[i].Size();
  }
  return sum;
}

size_t ClassTable::NumNonZygoteClasses() const {
  return classes_.back().Size();
}

mirror::Class* ClassTable::Lookup(const char* descriptor, size_t hash) {
  for (ClassSet& class_set : classes_) {
    auto it = class_set.FindWithHash(descriptor, hash);
    if (it != class_set.end()) {
     return it->Read();
    }
  }
  return nullptr;
}

void ClassTable::Insert(mirror::Class* klass) {
  classes_.back().Insert(GcRoot<mirror::Class>(klass));
}

void ClassTable::InsertWithHash(mirror::Class* klass, size_t hash) {
  classes_.back().InsertWithHash(GcRoot<mirror::Class>(klass), hash);
}

bool ClassTable::Remove(const char* descriptor) {
  for (ClassSet& class_set : classes_) {
    auto it = class_set.Find(descriptor);
    if (it != class_set.end()) {
      class_set.Erase(it);
      return true;
    }
  }
  return false;
}

uint32_t ClassTable::ClassDescriptorHashEquals::operator()(const GcRoot<mirror::Class>& root)
    const {
  std::string temp;
  return ComputeModifiedUtf8Hash(root.Read()->GetDescriptor(&temp));
}

bool ClassTable::ClassDescriptorHashEquals::operator()(const GcRoot<mirror::Class>& a,
                                                       const GcRoot<mirror::Class>& b) const {
  DCHECK_EQ(a.Read()->GetClassLoader(), b.Read()->GetClassLoader());
  std::string temp;
  return a.Read()->DescriptorEquals(b.Read()->GetDescriptor(&temp));
}

bool ClassTable::ClassDescriptorHashEquals::operator()(const GcRoot<mirror::Class>& a,
                                                       const char* descriptor) const {
  return a.Read()->DescriptorEquals(descriptor);
}

uint32_t ClassTable::ClassDescriptorHashEquals::operator()(const char* descriptor) const {
  return ComputeModifiedUtf8Hash(descriptor);
}

bool ClassTable::InsertDexFile(mirror::Object* dex_file) {
  DCHECK(dex_file != nullptr);
  for (GcRoot<mirror::Object>& root : dex_files_) {
    if (root.Read() == dex_file) {
      return false;
    }
  }
  dex_files_.push_back(GcRoot<mirror::Object>(dex_file));
  return true;
}

size_t ClassTable::WriteToMemory(uint8_t* ptr) const {
  ClassSet combined;
  // Combine all the class sets in case there are multiple, also adjusts load factor back to
  // default in case classes were pruned.
  for (const ClassSet& class_set : classes_) {
    for (const GcRoot<mirror::Class>& root : class_set) {
      combined.Insert(root);
    }
  }
  const size_t ret = combined.WriteToMemory(ptr);
  // Sanity check.
  if (kIsDebugBuild && ptr != nullptr) {
    size_t read_count;
    ClassSet class_set(ptr, /*make copy*/false, &read_count);
    class_set.Verify();
  }
  return ret;
}

size_t ClassTable::ReadFromMemory(uint8_t* ptr) {
  size_t read_count = 0;
  classes_.insert(classes_.begin(), ClassSet(ptr, /*make copy*/false, &read_count));
  return read_count;
}

}  // namespace art
