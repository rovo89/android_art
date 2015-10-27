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

#ifndef ART_RUNTIME_JIT_PROFILING_INFO_H_
#define ART_RUNTIME_JIT_PROFILING_INFO_H_

#include <vector>

#include "base/macros.h"
#include "gc_root.h"

namespace art {

class ArtMethod;

namespace mirror {
class Class;
}

/**
 * Profiling info for a method, created and filled by the interpreter once the
 * method is warm, and used by the compiler to drive optimizations.
 */
class ProfilingInfo {
 public:
  static ProfilingInfo* Create(ArtMethod* method) SHARED_REQUIRES(Locks::mutator_lock_);

  // Add information from an executed INVOKE instruction to the profile.
  void AddInvokeInfo(Thread* self, uint32_t dex_pc, mirror::Class* cls);

  // NO_THREAD_SAFETY_ANALYSIS since we don't know what the callback requires.
  template<typename RootVisitorType>
  void VisitRoots(RootVisitorType& visitor) NO_THREAD_SAFETY_ANALYSIS {
    for (size_t i = 0; i < number_of_inline_caches_; ++i) {
      InlineCache* cache = &cache_[i];
      for (size_t j = 0; j < InlineCache::kIndividualCacheSize; ++j) {
        visitor.VisitRootIfNonNull(cache->classes_[j].AddressWithoutBarrier());
      }
    }
  }

 private:
  // Structure to store the classes seen at runtime for a specific instruction.
  // Once the classes_ array is full, we consider the INVOKE to be megamorphic.
  struct InlineCache {
    bool IsMonomorphic() const {
      DCHECK_GE(kIndividualCacheSize, 2);
      return !classes_[0].IsNull() && classes_[1].IsNull();
    }

    bool IsMegamorphic() const {
      for (size_t i = 0; i < kIndividualCacheSize; ++i) {
        if (classes_[i].IsNull()) {
          return false;
        }
      }
      return true;
    }

    bool IsUnitialized() const {
      return classes_[0].IsNull();
    }

    bool IsPolymorphic() const {
      DCHECK_GE(kIndividualCacheSize, 3);
      return !classes_[1].IsNull() && classes_[kIndividualCacheSize - 1].IsNull();
    }

    static constexpr uint16_t kIndividualCacheSize = 5;
    uint32_t dex_pc;
    GcRoot<mirror::Class> classes_[kIndividualCacheSize];
  };

  explicit ProfilingInfo(const std::vector<uint32_t>& entries)
      : number_of_inline_caches_(entries.size()) {
    memset(&cache_, 0, number_of_inline_caches_ * sizeof(InlineCache));
    for (size_t i = 0; i < number_of_inline_caches_; ++i) {
      cache_[i].dex_pc = entries[i];
    }
  }

  // Number of instructions we are profiling in the ArtMethod.
  const uint32_t number_of_inline_caches_;

  // Dynamically allocated array of size `number_of_inline_caches_`.
  InlineCache cache_[0];

  DISALLOW_COPY_AND_ASSIGN(ProfilingInfo);
};

}  // namespace art

#endif  // ART_RUNTIME_JIT_PROFILING_INFO_H_
