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

#include "compiler_internals.h"
#include "dex_file-inl.h"

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

ArenaBitVector::ArenaBitVector(ArenaAllocator* arena, unsigned int start_bits,
                               bool expandable, OatBitMapKind kind)
  :  arena_(arena),
     expandable_(expandable),
     kind_(kind),
     storage_size_((start_bits + 31) >> 5),
     storage_(static_cast<uint32_t*>(arena_->NewMem(storage_size_ * sizeof(uint32_t), true,
                                                    ArenaAllocator::kAllocGrowableBitMap))) {
  DCHECK_EQ(sizeof(storage_[0]), 4U);    // Assuming 32-bit units.
}

/*
 * Determine whether or not the specified bit is set.
 */
bool ArenaBitVector::IsBitSet(unsigned int num) {
  DCHECK_LT(num, storage_size_ * sizeof(uint32_t) * 8);

  unsigned int val = storage_[num >> 5] & check_masks[num & 0x1f];
  return (val != 0);
}

// Mark all bits bit as "clear".
void ArenaBitVector::ClearAllBits() {
  memset(storage_, 0, storage_size_ * sizeof(uint32_t));
}

// Mark the specified bit as "set".
/*
 * TUNING: this could have pathologically bad growth/expand behavior.  Make sure we're
 * not using it badly or change resize mechanism.
 */
void ArenaBitVector::SetBit(unsigned int num) {
  if (num >= storage_size_ * sizeof(uint32_t) * 8) {
    DCHECK(expandable_) << "Attempted to expand a non-expandable bitmap to position " << num;

    /* Round up to word boundaries for "num+1" bits */
    unsigned int new_size = (num + 1 + 31) >> 5;
    DCHECK_GT(new_size, storage_size_);
    uint32_t *new_storage =
        static_cast<uint32_t*>(arena_->NewMem(new_size * sizeof(uint32_t), false,
                                              ArenaAllocator::kAllocGrowableBitMap));
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
void ArenaBitVector::ClearBit(unsigned int num) {
  DCHECK_LT(num, storage_size_ * sizeof(uint32_t) * 8);
  storage_[num >> 5] &= ~check_masks[num & 0x1f];
}

// Copy a whole vector to the other. Sizes must match.
void ArenaBitVector::Copy(ArenaBitVector* src) {
  DCHECK_EQ(storage_size_, src->GetStorageSize());
  memcpy(storage_, src->GetRawStorage(), sizeof(uint32_t) * storage_size_);
}

// Intersect with another bit vector.  Sizes and expandability must be the same.
void ArenaBitVector::Intersect(const ArenaBitVector* src) {
  DCHECK_EQ(storage_size_, src->GetStorageSize());
  DCHECK_EQ(expandable_, src->IsExpandable());
  for (unsigned int idx = 0; idx < storage_size_; idx++) {
    storage_[idx] &= src->GetRawStorageWord(idx);
  }
}

/*
 * Union with another bit vector.  Sizes and expandability must be the same.
 */
void ArenaBitVector::Union(const ArenaBitVector* src) {
  DCHECK_EQ(storage_size_, src->GetStorageSize());
  DCHECK_EQ(expandable_, src->IsExpandable());
  for (unsigned int idx = 0; idx < storage_size_; idx++) {
    storage_[idx] |= src->GetRawStorageWord(idx);
  }
}

// Are we equal to another bit vector?  Note: expandability attributes must also match.
bool ArenaBitVector::Equal(const ArenaBitVector* src) {
  if (storage_size_ != src->GetStorageSize() ||
      expandable_ != src->IsExpandable())
    return false;

  for (unsigned int idx = 0; idx < storage_size_; idx++) {
    if (storage_[idx] != src->GetRawStorageWord(idx)) return false;
  }
  return true;
}

// Count the number of bits that are set.
int ArenaBitVector::NumSetBits()
{
  unsigned int count = 0;

  for (unsigned int word = 0; word < storage_size_; word++) {
    count += __builtin_popcount(storage_[word]);
  }
  return count;
}

// Return the position of the next set bit.  -1 means end-of-element reached.
// TUNING: Hot function.
int ArenaBitVector::Iterator::Next()
{
  // Did anything obviously change since we started?
  DCHECK_EQ(bit_size_, p_bits_->GetStorageSize() * sizeof(uint32_t) * 8);
  DCHECK_EQ(bit_storage_, p_bits_->GetRawStorage());

  if (bit_index_ >= bit_size_) return -1;

  uint32_t word_index = bit_index_ >> 5;
  uint32_t end_word_index = bit_size_ >> 5;
  uint32_t word = bit_storage_[word_index++];

  // Mask out any bits in the first word we've already considered.
  word &= ~((1 << (bit_index_ & 0x1f))-1);

  for (; word_index <= end_word_index;) {
    uint32_t bit_pos = bit_index_ & 0x1f;
    if (word == 0) {
      bit_index_ += (32 - bit_pos);
      word = bit_storage_[word_index++];
      continue;
    }
    for (; bit_pos < 32; bit_pos++) {
      if (word & (1 << bit_pos)) {
        bit_index_++;
        return bit_index_ - 1;
      }
      bit_index_++;
    }
    word = bit_storage_[word_index++];
  }
  bit_index_ = bit_size_;
  return -1;
}

/*
 * Mark specified number of bits as "set". Cannot set all bits like ClearAll
 * since there might be unused bits - setting those to one will confuse the
 * iterator.
 */
void ArenaBitVector::SetInitialBits(unsigned int num_bits)
{
  DCHECK_LE(((num_bits + 31) >> 5), storage_size_);
  unsigned int idx;
  for (idx = 0; idx < (num_bits >> 5); idx++) {
    storage_[idx] = -1;
  }
  unsigned int rem_num_bits = num_bits & 0x1f;
  if (rem_num_bits) {
    storage_[idx] = (1 << rem_num_bits) - 1;
  }
}

}  // namespace art
