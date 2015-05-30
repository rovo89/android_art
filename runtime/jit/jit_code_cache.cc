/*
 * Copyright 2014 The Android Open Source Project
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

#include "jit_code_cache.h"

#include <sstream>

#include "art_method-inl.h"
#include "mem_map.h"
#include "oat_file-inl.h"

namespace art {
namespace jit {

JitCodeCache* JitCodeCache::Create(size_t capacity, std::string* error_msg) {
  CHECK_GT(capacity, 0U);
  CHECK_LT(capacity, kMaxCapacity);
  std::string error_str;
  // Map name specific for android_os_Debug.cpp accounting.
  MemMap* map = MemMap::MapAnonymous("jit-code-cache", nullptr, capacity,
                                     PROT_READ | PROT_WRITE | PROT_EXEC, false, false, &error_str);
  if (map == nullptr) {
    std::ostringstream oss;
    oss << "Failed to create read write execute cache: " << error_str << " size=" << capacity;
    *error_msg = oss.str();
    return nullptr;
  }
  return new JitCodeCache(map);
}

JitCodeCache::JitCodeCache(MemMap* mem_map)
    : lock_("Jit code cache", kJitCodeCacheLock), num_methods_(0) {
  VLOG(jit) << "Created jit code cache size=" << PrettySize(mem_map->Size());
  mem_map_.reset(mem_map);
  uint8_t* divider = mem_map->Begin() + RoundUp(mem_map->Size() / 4, kPageSize);
  // Data cache is 1 / 4 of the map. TODO: Make this variable?
  // Put data at the start.
  data_cache_ptr_ = mem_map->Begin();
  data_cache_end_ = divider;
  data_cache_begin_ = data_cache_ptr_;
  mprotect(data_cache_ptr_, data_cache_end_ - data_cache_begin_, PROT_READ | PROT_WRITE);
  // Code cache after.
  code_cache_begin_ = divider;
  code_cache_ptr_ = divider;
  code_cache_end_ = mem_map->End();
}

bool JitCodeCache::ContainsMethod(ArtMethod* method) const {
  return ContainsCodePtr(method->GetEntryPointFromQuickCompiledCode());
}

bool JitCodeCache::ContainsCodePtr(const void* ptr) const {
  return ptr >= code_cache_begin_ && ptr < code_cache_end_;
}

void JitCodeCache::FlushInstructionCache() {
  UNIMPLEMENTED(FATAL);
  // TODO: Investigate if we need to do this.
  // __clear_cache(reinterpret_cast<char*>(code_cache_begin_), static_cast<int>(CodeCacheSize()));
}

uint8_t* JitCodeCache::ReserveCode(Thread* self, size_t size) {
  MutexLock mu(self, lock_);
  if (size > CodeCacheRemain()) {
    return nullptr;
  }
  ++num_methods_;  // TODO: This is hacky but works since each method has exactly one code region.
  code_cache_ptr_ += size;
  return code_cache_ptr_ - size;
}

uint8_t* JitCodeCache::AddDataArray(Thread* self, const uint8_t* begin, const uint8_t* end) {
  MutexLock mu(self, lock_);
  const size_t size = end - begin;
  if (size > DataCacheRemain()) {
    return nullptr;  // Out of space in the data cache.
  }
  std::copy(begin, end, data_cache_ptr_);
  data_cache_ptr_ += size;
  return data_cache_ptr_ - size;
}

const void* JitCodeCache::GetCodeFor(ArtMethod* method) {
  const void* code = method->GetEntryPointFromQuickCompiledCode();
  if (ContainsCodePtr(code)) {
    return code;
  }
  MutexLock mu(Thread::Current(), lock_);
  auto it = method_code_map_.find(method);
  if (it != method_code_map_.end()) {
    return it->second;
  }
  return nullptr;
}

void JitCodeCache::SaveCompiledCode(ArtMethod* method, const void* old_code_ptr) {
  DCHECK_EQ(method->GetEntryPointFromQuickCompiledCode(), old_code_ptr);
  DCHECK(ContainsCodePtr(old_code_ptr)) << PrettyMethod(method) << " old_code_ptr="
      << old_code_ptr;
  MutexLock mu(Thread::Current(), lock_);
  auto it = method_code_map_.find(method);
  if (it != method_code_map_.end()) {
    return;
  }
  method_code_map_.Put(method, old_code_ptr);
}

}  // namespace jit
}  // namespace art
