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

#include "arena_bit_vector.h"

#include "base/allocator.h"
#include "base/arena_allocator.h"

namespace art {

template <typename ArenaAlloc>
class ArenaBitVectorAllocator FINAL : public Allocator,
    public ArenaObject<kArenaAllocGrowableBitMap> {
 public:
  explicit ArenaBitVectorAllocator(ArenaAlloc* arena) : arena_(arena) {}
  ~ArenaBitVectorAllocator() {}

  virtual void* Alloc(size_t size) {
    return arena_->Alloc(size, kArenaAllocGrowableBitMap);
  }

  virtual void Free(void*) {}  // Nop.

 private:
  ArenaAlloc* const arena_;
  DISALLOW_COPY_AND_ASSIGN(ArenaBitVectorAllocator);
};

ArenaBitVector::ArenaBitVector(ArenaAllocator* arena, unsigned int start_bits,
                               bool expandable, OatBitMapKind kind)
  :  BitVector(start_bits, expandable,
               new (arena) ArenaBitVectorAllocator<ArenaAllocator>(arena)), kind_(kind) {
  UNUSED(kind_);
}

ArenaBitVector::ArenaBitVector(ScopedArenaAllocator* arena, unsigned int start_bits,
                               bool expandable, OatBitMapKind kind)
  :  BitVector(start_bits, expandable,
               new (arena) ArenaBitVectorAllocator<ScopedArenaAllocator>(arena)), kind_(kind) {
  UNUSED(kind_);
}

}  // namespace art
