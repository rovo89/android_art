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

#include "bit_vector.h"

namespace art {

// TODO: profile to make sure this is still a win relative to just using shifted masks.
static uint32_t check_masks[32] = {
  0x00000001, 0x00000002, 0x00000004, 0x00000008, 0x00000010,
  0x00000020, 0x00000040, 0x00000080, 0x00000100, 0x00000200,
  0x00000400, 0x00000800, 0x00001000, 0x00002000, 0x00004000,
  0x00008000, 0x00010000, 0x00020000, 0x00040000, 0x00080000,
  0x00100000, 0x00200000, 0x00400000, 0x00800000, 0x01000000,
  0x02000000, 0x04000000, 0x08000000, 0x10000000, 0x20000000,
  0x40000000, 0x80000000 };

static inline uint32_t BitsToWords(uint32_t bits) {
  return (bits + 31) >> 5;
}

// TODO: replace excessive argument defaulting when we are at gcc 4.7
// or later on host with delegating constructor support. Specifically,
// starts_bits and storage_size/storage are mutually exclusive.
BitVector::BitVector(uint32_t start_bits,
                     bool expandable,
                     Allocator* allocator,
                     uint32_t storage_size,
                     uint32_t* storage)
  : allocator_(allocator),
    expandable_(expandable),
    storage_size_(storage_size),
    storage_(storage) {
  DCHECK_EQ(sizeof(storage_[0]), 4U);  // Assuming 32-bit units.
  if (storage_ == NULL) {
    storage_size_ = BitsToWords(start_bits);
    storage_ = static_cast<uint32_t*>(allocator_->Alloc(storage_size_ * sizeof(uint32_t)));
  }
}

BitVector::~BitVector() {
  allocator_->Free(storage_);
}

/*
 * Determine whether or not the specified bit is set.
 */
bool BitVector::IsBitSet(uint32_t num) const {
  DCHECK_LT(num, storage_size_ * sizeof(uint32_t) * 8);

  uint32_t val = storage_[num >> 5] & check_masks[num & 0x1f];
  return (val != 0);
}

// Mark all bits bit as "clear".
void BitVector::ClearAllBits() {
  memset(storage_, 0, storage_size_ * sizeof(uint32_t));
}

// Mark the specified bit as "set".
/*
 * TUNING: this could have pathologically bad growth/expand behavior.  Make sure we're
 * not using it badly or change resize mechanism.
 */
void BitVector::SetBit(uint32_t num) {
  if (num >= storage_size_ * sizeof(uint32_t) * 8) {
    DCHECK(expandable_) << "Attempted to expand a non-expandable bitmap to position " << num;

    /* Round up to word boundaries for "num+1" bits */
    uint32_t new_size = BitsToWords(num + 1);
    DCHECK_GT(new_size, storage_size_);
    uint32_t *new_storage =
        static_cast<uint32_t*>(allocator_->Alloc(new_size * sizeof(uint32_t)));
    memcpy(new_storage, storage_, storage_size_ * sizeof(uint32_t));
    // Zero out the new storage words.
    memset(&new_storage[storage_size_], 0, (new_size - storage_size_) * sizeof(uint32_t));
    // TOTO: collect stats on space wasted because of resize.
    storage_ = new_storage;
    storage_size_ = new_size;
  }

  storage_[num >> 5] |= check_masks[num & 0x1f];
}

// Mark the specified bit as "unset".
void BitVector::ClearBit(uint32_t num) {
  DCHECK_LT(num, storage_size_ * sizeof(uint32_t) * 8);
  storage_[num >> 5] &= ~check_masks[num & 0x1f];
}

// Intersect with another bit vector.  Sizes and expandability must be the same.
void BitVector::Intersect(const BitVector* src) {
  DCHECK_EQ(storage_size_, src->GetStorageSize());
  DCHECK_EQ(expandable_, src->IsExpandable());
  for (uint32_t idx = 0; idx < storage_size_; idx++) {
    storage_[idx] &= src->GetRawStorageWord(idx);
  }
}

/*
 * Union with another bit vector.  Sizes and expandability must be the same.
 */
void BitVector::Union(const BitVector* src) {
  DCHECK_EQ(storage_size_, src->GetStorageSize());
  DCHECK_EQ(expandable_, src->IsExpandable());
  for (uint32_t idx = 0; idx < storage_size_; idx++) {
    storage_[idx] |= src->GetRawStorageWord(idx);
  }
}

// Count the number of bits that are set.
uint32_t BitVector::NumSetBits() const {
  uint32_t count = 0;
  for (uint32_t word = 0; word < storage_size_; word++) {
    count += __builtin_popcount(storage_[word]);
  }
  return count;
}

// Count the number of bits that are set up through and including num.
uint32_t BitVector::NumSetBits(uint32_t num) const {
  DCHECK_LT(num, storage_size_ * sizeof(uint32_t) * 8);
  uint32_t last_word = num >> 5;
  uint32_t partial_word_bits = num & 0x1f;

  // partial_word_bits |  # |                         |                      | partial_word_mask
  //             00000 |  0 | 0xffffffff >> (31 -  0) | (1 <<  (0 + 1)) - 1  | 0x00000001
  //             00001 |  1 | 0xffffffff >> (31 -  1) | (1 <<  (1 + 1)) - 1  | 0x00000003
  //             00010 |  2 | 0xffffffff >> (31 -  2) | (1 <<  (2 + 1)) - 1  | 0x00000007
  //             ..... |
  //             11110 | 30 | 0xffffffff >> (31 - 30) | (1 << (30 + 1)) - 1  | 0x7fffffff
  //             11111 | 31 | 0xffffffff >> (31 - 31) | last_full_word++     | 0xffffffff
  uint32_t partial_word_mask = 0xffffffff >> (0x1f - partial_word_bits);

  uint32_t count = 0;
  for (uint32_t word = 0; word < last_word; word++) {
    count += __builtin_popcount(storage_[word]);
  }
  count += __builtin_popcount(storage_[last_word] & partial_word_mask);
  return count;
}

BitVector::Iterator* BitVector::GetIterator() const {
  return new (allocator_) Iterator(this);
}

/*
 * Mark specified number of bits as "set". Cannot set all bits like ClearAll
 * since there might be unused bits - setting those to one will confuse the
 * iterator.
 */
void BitVector::SetInitialBits(uint32_t num_bits) {
  DCHECK_LE(BitsToWords(num_bits), storage_size_);
  uint32_t idx;
  for (idx = 0; idx < (num_bits >> 5); idx++) {
    storage_[idx] = -1;
  }
  uint32_t rem_num_bits = num_bits & 0x1f;
  if (rem_num_bits) {
    storage_[idx] = (1 << rem_num_bits) - 1;
  }
}

}  // namespace art
