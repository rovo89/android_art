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

#ifndef ART_RUNTIME_ART_CODE_H_
#define ART_RUNTIME_ART_CODE_H_

#include "base/mutex.h"
#include "offsets.h"
#include "quick/quick_method_frame_info.h"
#include "stack_map.h"

namespace art {

class ArtMethod;

class ArtCode FINAL {
 public:
  explicit ArtCode(ArtMethod** method) : method_(*method) {}
  explicit ArtCode(ArtMethod* method) : method_(method) {}
  ArtCode() : method_(nullptr) {}

  // Converts a dex PC to a native PC.
  uintptr_t ToNativeQuickPc(const uint32_t dex_pc,
                            bool is_for_catch_handler,
                            bool abort_on_failure = true)
      SHARED_REQUIRES(Locks::mutator_lock_);

  bool IsOptimized(size_t pointer_size) SHARED_REQUIRES(Locks::mutator_lock_);

  CodeInfo GetOptimizedCodeInfo() SHARED_REQUIRES(Locks::mutator_lock_);

  uintptr_t NativeQuickPcOffset(const uintptr_t pc) SHARED_REQUIRES(Locks::mutator_lock_);

  // Converts a native PC to a dex PC.
  uint32_t ToDexPc(const uintptr_t pc, bool abort_on_failure = true)
      SHARED_REQUIRES(Locks::mutator_lock_);

  // Callers should wrap the uint8_t* in a GcMap instance for convenient access.
  const uint8_t* GetNativeGcMap(size_t pointer_size) SHARED_REQUIRES(Locks::mutator_lock_);

  const uint8_t* GetVmapTable(size_t pointer_size) SHARED_REQUIRES(Locks::mutator_lock_);

  const uint8_t* GetMappingTable(size_t pointer_size) SHARED_REQUIRES(Locks::mutator_lock_);

  QuickMethodFrameInfo GetQuickFrameInfo() SHARED_REQUIRES(Locks::mutator_lock_);

  FrameOffset GetReturnPcOffset() SHARED_REQUIRES(Locks::mutator_lock_) {
    return FrameOffset(GetFrameSizeInBytes() - sizeof(void*));
  }

  template <bool kCheckFrameSize = true>
  uint32_t GetFrameSizeInBytes() SHARED_REQUIRES(Locks::mutator_lock_) {
    uint32_t result = GetQuickFrameInfo().FrameSizeInBytes();
    if (kCheckFrameSize) {
      DCHECK_LE(static_cast<size_t>(kStackAlignment), result);
    }
    return result;
  }

  const void* GetQuickOatEntryPoint(size_t pointer_size) SHARED_REQUIRES(Locks::mutator_lock_);

  void AssertPcIsWithinQuickCode(uintptr_t pc) SHARED_REQUIRES(Locks::mutator_lock_);

  bool PcIsWithinQuickCode(uintptr_t pc) SHARED_REQUIRES(Locks::mutator_lock_);

  FrameOffset GetHandleScopeOffset() SHARED_REQUIRES(Locks::mutator_lock_) {
    constexpr size_t handle_scope_offset = sizeof(ArtMethod*);
    DCHECK_LT(handle_scope_offset, GetFrameSizeInBytes());
    return FrameOffset(handle_scope_offset);
  }

  ArtMethod* GetMethod() const { return method_; }

 private:
  ArtMethod* method_;
};

}  // namespace art

#endif  // ART_RUNTIME_ART_CODE_H_
