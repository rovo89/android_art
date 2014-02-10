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
  DCHECK_EQ(sizeof(*storage_), 4U);  // Assuming 32-bit units.
  if (storage_ == nullptr) {
    storage_size_ = BitsToWords(start_bits);
    storage_ = static_cast<uint32_t*>(allocator_->Alloc(storage_size_ * sizeof(*storage_)));
  }
}

BitVector::~BitVector() {
  allocator_->Free(storage_);
}

/*
 * Determine whether or not the specified bit is set.
 */
bool BitVector::IsBitSet(uint32_t num) const {
  // If the index is over the size:
  if (num >= storage_size_ * sizeof(*storage_) * 8) {
    // Whether it is expandable or not, this bit does not exist: thus it is not set.
    return false;
  }

  uint32_t val = storage_[num >> 5] & check_masks[num & 0x1f];
  return (val != 0);
}

// Mark all bits bit as "clear".
void BitVector::ClearAllBits() {
  memset(storage_, 0, storage_size_ * sizeof(*storage_));
}

// Mark the specified bit as "set".
/*
 * TUNING: this could have pathologically bad growth/expand behavior.  Make sure we're
 * not using it badly or change resize mechanism.
 */
void BitVector::SetBit(uint32_t num) {
  if (num >= storage_size_ * sizeof(*storage_) * 8) {
    DCHECK(expandable_) << "Attempted to expand a non-expandable bitmap to position " << num;

    /* Round up to word boundaries for "num+1" bits */
    uint32_t new_size = BitsToWords(num + 1);
    DCHECK_GT(new_size, storage_size_);
    uint32_t *new_storage =
        static_cast<uint32_t*>(allocator_->Alloc(new_size * sizeof(*storage_)));
    memcpy(new_storage, storage_, storage_size_ * sizeof(*storage_));
    // Zero out the new storage words.
    memset(&new_storage[storage_size_], 0, (new_size - storage_size_) * sizeof(*storage_));
    // TOTO: collect stats on space wasted because of resize.
    storage_ = new_storage;
    storage_size_ = new_size;
  }

  storage_[num >> 5] |= check_masks[num & 0x1f];
}

// Mark the specified bit as "unset".
void BitVector::ClearBit(uint32_t num) {
  // If the index is over the size, we don't have to do anything, it is cleared.
  if (num < storage_size_ * sizeof(*storage_) * 8) {
    // Otherwise, go ahead and clear it.
    storage_[num >> 5] &= ~check_masks[num & 0x1f];
  }
}

bool BitVector::SameBitsSet(const BitVector *src) {
  int our_highest = GetHighestBitSet();
  int src_highest = src->GetHighestBitSet();

  // If the highest bit set is different, we are different.
  if (our_highest != src_highest) {
    return true;
  }

  // If the highest bit set is -1, both are cleared, we are the same.
  // If the highest bit set is 0, both have a unique bit set, we are the same.
  if (our_highest >= 0) {
    return true;
  }

  // Get the highest bit set's cell's index.
  int our_highest_index = (our_highest >> 5);

  // This memcmp is enough: we know that the highest bit set is the same for both:
  //   - Therefore, min_size goes up to at least that, we are thus comparing at least what we need to, but not less.
  //      ie. we are comparing all storage cells that could have difference, if both vectors have cells above our_highest_index,
  //          they are automatically at 0.
  return (memcmp(storage_, src->GetRawStorage(), our_highest_index * sizeof(*storage_)) != 0);
}

// Intersect with another bit vector.
void BitVector::Intersect(const BitVector* src) {
  uint32_t src_storage_size = src->storage_size_;

  // Get the minimum size between us and source.
  uint32_t min_size = (storage_size_ < src_storage_size) ? storage_size_ : src_storage_size;

  uint32_t idx;
  for (idx = 0; idx < min_size; idx++) {
    storage_[idx] &= src->GetRawStorageWord(idx);
  }

  // Now, due to this being an intersection, there are two possibilities:
  //   - Either src was larger than us: we don't care, all upper bits would thus be 0.
  //   - Either we are larger than src: we don't care, all upper bits would have been 0 too.
  // So all we need to do is set all remaining bits to 0.
  for (; idx < storage_size_; idx++) {
    storage_[idx] = 0;
  }
}

/*
 * Union with another bit vector.
 */
void BitVector::Union(const BitVector* src) {
  uint32_t src_size = src->storage_size_;

  // Get our size, we use this variable for the last loop of the method:
  //   - It can change in the if block if src is of a different size.
  uint32_t size = storage_size_;

  // Is the storage size smaller than src's?
  if (storage_size_ < src_size) {
    // Get the highest bit to determine how much we need to expand.
    int highest_bit = src->GetHighestBitSet();

    // If src has no bit set, we are done: there is no need for a union with src.
    if (highest_bit == -1) {
      return;
    }

    // Set it to reallocate.
    SetBit(highest_bit);

    // Paranoid: storage size should be big enough to hold this bit now.
    DCHECK_LT(static_cast<uint32_t> (highest_bit), storage_size_ * sizeof(*(storage_)) * 8);

    //  Update the size, our size can now not be bigger than the src size
    size = storage_size_;
  }

  for (uint32_t idx = 0; idx < size; idx++) {
    storage_[idx] |= src->GetRawStorageWord(idx);
  }
}

void BitVector::Subtract(const BitVector *src) {
    uint32_t src_size = src->storage_size_;

    // We only need to operate on bytes up to the smaller of the sizes of the two operands.
    unsigned int min_size = (storage_size_ > src_size) ? src_size : storage_size_;

    // Difference until max, we know both accept it:
    //   There is no need to do more:
    //     If we are bigger than src, the upper bits are unchanged.
    //     If we are smaller than src, the non-existant upper bits are 0 and thus can't get subtracted.
    for (uint32_t idx = 0; idx < min_size; idx++) {
        storage_[idx] &= (~(src->GetRawStorageWord(idx)));
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
  DCHECK_LT(num, storage_size_ * sizeof(*storage_) * 8);
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
  // If num_bits is 0, clear everything.
  if (num_bits == 0) {
    ClearAllBits();
    return;
  }

  // Set the highest bit we want to set to get the BitVector allocated if need be.
  SetBit(num_bits - 1);

  uint32_t idx;
  // We can set every storage element with -1.
  for (idx = 0; idx < (num_bits >> 5); idx++) {
    storage_[idx] = -1;
  }

  // Handle the potentially last few bits.
  uint32_t rem_num_bits = num_bits & 0x1f;
  if (rem_num_bits != 0) {
    storage_[idx] = (1 << rem_num_bits) - 1;
  }

  // Now set the upper ones to 0.
  for (; idx < storage_size_; idx++) {
    storage_[idx] = 0;
  }
}

int BitVector::GetHighestBitSet() const {
  unsigned int max = storage_size_;
  for (int idx = max - 1; idx >= 0; idx--) {
    // If not 0, we have more work: check the bits.
    uint32_t value = storage_[idx];

    if (value != 0) {
      // Shift right for the counting.
      value /= 2;

      int cnt = 0;

      // Count the bits.
      while (value > 0) {
        value /= 2;
        cnt++;
      }

      // Return cnt + how many storage units still remain * the number of bits per unit.
      int res = cnt + (idx * (sizeof(*storage_) * 8));
      return res;
    }
  }

  // All zero, therefore return -1.
  return -1;
}

void BitVector::Copy(const BitVector *src) {
  // Get highest bit set, we only need to copy till then.
  int highest_bit = src->GetHighestBitSet();

  // If nothing is set, clear everything.
  if (highest_bit == -1) {
    ClearAllBits();
    return;
  }

  // Set upper bit to ensure right size before copy.
  SetBit(highest_bit);

  // Now set until highest bit's storage.
  uint32_t size = 1 + (highest_bit / (sizeof(*storage_) * 8));
  memcpy(storage_, src->GetRawStorage(), sizeof(*storage_) * size);

  // Set upper bits to 0.
  uint32_t left = storage_size_ - size;

  if (left > 0) {
    memset(storage_ + size, 0, sizeof(*storage_) * left);
  }
}

}  // namespace art
