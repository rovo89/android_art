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

static constexpr int kProtAll = PROT_READ | PROT_WRITE | PROT_EXEC;
static constexpr int kProtData = PROT_READ | PROT_WRITE;
static constexpr int kProtCode = PROT_READ | PROT_EXEC;

#define CHECKED_MPROTECT(memory, size, prot)                \
  do {                                                      \
    int rc = mprotect(memory, size, prot);                  \
    if (UNLIKELY(rc != 0)) {                                \
      errno = rc;                                           \
      PLOG(FATAL) << "Failed to mprotect jit code cache";   \
    }                                                       \
  } while (false)                                           \

JitCodeCache* JitCodeCache::Create(size_t capacity, std::string* error_msg) {
  CHECK_GT(capacity, 0U);
  CHECK_LT(capacity, kMaxCapacity);
  std::string error_str;
  // Map name specific for android_os_Debug.cpp accounting.
  MemMap* data_map = MemMap::MapAnonymous(
    "data-code-cache", nullptr, capacity, kProtAll, false, false, &error_str);
  if (data_map == nullptr) {
    std::ostringstream oss;
    oss << "Failed to create read write execute cache: " << error_str << " size=" << capacity;
    *error_msg = oss.str();
    return nullptr;
  }

  // Data cache is 1 / 4 of the map.
  // TODO: Make this variable?
  size_t data_size = RoundUp(data_map->Size() / 4, kPageSize);
  size_t code_size = data_map->Size() - data_size;
  uint8_t* divider = data_map->Begin() + data_size;

  // We need to have 32 bit offsets from method headers in code cache which point to things
  // in the data cache. If the maps are more than 4G apart, having multiple maps wouldn't work.
  MemMap* code_map = data_map->RemapAtEnd(divider, "jit-code-cache", kProtAll, &error_str);
  if (code_map == nullptr) {
    std::ostringstream oss;
    oss << "Failed to create read write execute cache: " << error_str << " size=" << capacity;
    *error_msg = oss.str();
    return nullptr;
  }
  DCHECK_EQ(code_map->Size(), code_size);
  DCHECK_EQ(code_map->Begin(), divider);
  return new JitCodeCache(code_map, data_map);
}

JitCodeCache::JitCodeCache(MemMap* code_map, MemMap* data_map)
    : lock_("Jit code cache", kJitCodeCacheLock),
      code_map_(code_map),
      data_map_(data_map),
      num_methods_(0) {

  VLOG(jit) << "Created jit code cache: data size="
            << PrettySize(data_map_->Size())
            << ", code size="
            << PrettySize(code_map_->Size());

  code_mspace_ = create_mspace_with_base(code_map_->Begin(), code_map_->Size(), false /*locked*/);
  data_mspace_ = create_mspace_with_base(data_map_->Begin(), data_map_->Size(), false /*locked*/);

  if (code_mspace_ == nullptr || data_mspace_ == nullptr) {
    PLOG(FATAL) << "create_mspace_with_base failed";
  }

  // Prevent morecore requests from the mspace.
  mspace_set_footprint_limit(code_mspace_, code_map_->Size());
  mspace_set_footprint_limit(data_mspace_, data_map_->Size());

  CHECKED_MPROTECT(code_map_->Begin(), code_map_->Size(), kProtCode);
  CHECKED_MPROTECT(data_map_->Begin(), data_map_->Size(), kProtData);
}

bool JitCodeCache::ContainsMethod(ArtMethod* method) const {
  return ContainsCodePtr(method->GetEntryPointFromQuickCompiledCode());
}

bool JitCodeCache::ContainsCodePtr(const void* ptr) const {
  return code_map_->Begin() <= ptr && ptr < code_map_->End();
}

class ScopedCodeCacheWrite {
 public:
  explicit ScopedCodeCacheWrite(MemMap* code_map) : code_map_(code_map) {
    CHECKED_MPROTECT(code_map_->Begin(), code_map_->Size(), kProtAll);
  }
  ~ScopedCodeCacheWrite() {
    CHECKED_MPROTECT(code_map_->Begin(), code_map_->Size(), kProtCode);
  }
 private:
  MemMap* const code_map_;

  DISALLOW_COPY_AND_ASSIGN(ScopedCodeCacheWrite);
};

uint8_t* JitCodeCache::CommitCode(Thread* self,
                                  const uint8_t* mapping_table,
                                  const uint8_t* vmap_table,
                                  const uint8_t* gc_map,
                                  size_t frame_size_in_bytes,
                                  size_t core_spill_mask,
                                  size_t fp_spill_mask,
                                  const uint8_t* code,
                                  size_t code_size) {
  size_t alignment = GetInstructionSetAlignment(kRuntimeISA);
  // Ensure the header ends up at expected instruction alignment.
  size_t header_size = RoundUp(sizeof(OatQuickMethodHeader), alignment);
  size_t total_size = header_size + code_size;

  OatQuickMethodHeader* method_header = nullptr;
  uint8_t* code_ptr = nullptr;

  MutexLock mu(self, lock_);
  {
    ScopedCodeCacheWrite scc(code_map_.get());
    uint8_t* result = reinterpret_cast<uint8_t*>(
        mspace_memalign(code_mspace_, alignment, total_size));
    if (result == nullptr) {
      return nullptr;
    }
    code_ptr = result + header_size;
    DCHECK_ALIGNED_PARAM(reinterpret_cast<uintptr_t>(code_ptr), alignment);

    std::copy(code, code + code_size, code_ptr);
    method_header = reinterpret_cast<OatQuickMethodHeader*>(code_ptr) - 1;
    new (method_header) OatQuickMethodHeader(
        (mapping_table == nullptr) ? 0 : code_ptr - mapping_table,
        (vmap_table == nullptr) ? 0 : code_ptr - vmap_table,
        (gc_map == nullptr) ? 0 : code_ptr - gc_map,
        frame_size_in_bytes,
        core_spill_mask,
        fp_spill_mask,
        code_size);
  }

  __builtin___clear_cache(reinterpret_cast<char*>(code_ptr),
                          reinterpret_cast<char*>(code_ptr + code_size));

  ++num_methods_;  // TODO: This is hacky but works since each method has exactly one code region.
  return reinterpret_cast<uint8_t*>(method_header);
}

size_t JitCodeCache::CodeCacheSize() {
  MutexLock mu(Thread::Current(), lock_);
  size_t bytes_allocated = 0;
  mspace_inspect_all(code_mspace_, DlmallocBytesAllocatedCallback, &bytes_allocated);
  return bytes_allocated;
}

size_t JitCodeCache::DataCacheSize() {
  MutexLock mu(Thread::Current(), lock_);
  size_t bytes_allocated = 0;
  mspace_inspect_all(data_mspace_, DlmallocBytesAllocatedCallback, &bytes_allocated);
  return bytes_allocated;
}

uint8_t* JitCodeCache::ReserveData(Thread* self, size_t size) {
  size = RoundUp(size, sizeof(void*));
  MutexLock mu(self, lock_);
  return reinterpret_cast<uint8_t*>(mspace_malloc(data_mspace_, size));
}

uint8_t* JitCodeCache::AddDataArray(Thread* self, const uint8_t* begin, const uint8_t* end) {
  uint8_t* result = ReserveData(self, end - begin);
  if (result == nullptr) {
    return nullptr;  // Out of space in the data cache.
  }
  std::copy(begin, end, result);
  return result;
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
