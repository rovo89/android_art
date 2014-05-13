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
    storage_(storage),
    number_of_bits_(start_bits) {
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

  return IsBitSet(storage_, num);
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
    number_of_bits_ = num;
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
    return false;
  }

  // If the highest bit set is -1, both are cleared, we are the same.
  // If the highest bit set is 0, both have a unique bit set, we are the same.
  if (our_highest <= 0) {
    return true;
  }

  // Get the highest bit set's cell's index
  // No need of highest + 1 here because it can't be 0 so BitsToWords will work here.
  int our_highest_index = BitsToWords(our_highest);

  // This memcmp is enough: we know that the highest bit set is the same for both:
  //   - Therefore, min_size goes up to at least that, we are thus comparing at least what we need to, but not less.
  //      ie. we are comparing all storage cells that could have difference, if both vectors have cells above our_highest_index,
  //          they are automatically at 0.
  return (memcmp(storage_, src->GetRawStorage(), our_highest_index * sizeof(*storage_)) == 0);
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
bool BitVector::Union(const BitVector* src) {
  // Get the highest bit to determine how much we need to expand.
  int highest_bit = src->GetHighestBitSet();
  bool changed = false;

  // If src has no bit set, we are done: there is no need for a union with src.
  if (highest_bit == -1) {
    return changed;
  }

  // Update src_size to how many cells we actually care about: where the bit is + 1.
  uint32_t src_size = BitsToWords(highest_bit + 1);

  // Is the storage size smaller than src's?
  if (storage_size_ < src_size) {
    changed = true;

    // Set it to reallocate.
    SetBit(highest_bit);

    // Paranoid: storage size should be big enough to hold this bit now.
    DCHECK_LT(static_cast<uint32_t> (highest_bit), storage_size_ * sizeof(*(storage_)) * 8);
  }

  for (uint32_t idx = 0; idx < src_size; idx++) {
    uint32_t existing = storage_[idx];
    uint32_t update = existing | src->GetRawStorageWord(idx);
    if (existing != update) {
      changed = true;
      storage_[idx] = update;
    }
  }
  return changed;
}

bool BitVector::UnionIfNotIn(const BitVector* union_with, const BitVector* not_in) {
  // Get the highest bit to determine how much we need to expand.
  int highest_bit = union_with->GetHighestBitSet();
  bool changed = false;

  // If src has no bit set, we are done: there is no need for a union with src.
  if (highest_bit == -1) {
    return changed;
  }

  // Update union_with_size to how many cells we actually care about: where the bit is + 1.
  uint32_t union_with_size = BitsToWords(highest_bit + 1);

  // Is the storage size smaller than src's?
  if (storage_size_ < union_with_size) {
    changed = true;

    // Set it to reallocate.
    SetBit(highest_bit);

    // Paranoid: storage size should be big enough to hold this bit now.
    DCHECK_LT(static_cast<uint32_t> (highest_bit), storage_size_ * sizeof(*(storage_)) * 8);
  }

  uint32_t not_in_size = not_in->GetStorageSize();

  uint32_t idx = 0;
  for (; idx < std::min(not_in_size, union_with_size); idx++) {
    uint32_t existing = storage_[idx];
    uint32_t update = existing |
        (union_with->GetRawStorageWord(idx) & ~not_in->GetRawStorageWord(idx));
    if (existing != update) {
      changed = true;
      storage_[idx] = update;
    }
  }

  for (; idx < union_with_size; idx++) {
    uint32_t existing = storage_[idx];
    uint32_t update = existing | union_with->GetRawStorageWord(idx);
    if (existing != update) {
      changed = true;
      storage_[idx] = update;
    }
  }
  return changed;
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
    count += POPCOUNT(storage_[word]);
  }
  return count;
}

// Count the number of bits that are set in range [0, end).
uint32_t BitVector::NumSetBits(uint32_t end) const {
  DCHECK_LE(end, storage_size_ * sizeof(*storage_) * 8);
  return NumSetBits(storage_, end);
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
    ++idx;
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

bool BitVector::EnsureSizeAndClear(unsigned int num) {
  // Check if the bitvector is expandable.
  if (IsExpandable() == false) {
    return false;
  }

  if (num > 0) {
    // Now try to expand by setting the last bit.
    SetBit(num - 1);
  }

  // We must clear all bits as per our specification.
  ClearAllBits();

  return true;
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

bool BitVector::IsBitSet(const uint32_t* storage, uint32_t num) {
  uint32_t val = storage[num >> 5] & check_masks[num & 0x1f];
  return (val != 0);
}

uint32_t BitVector::NumSetBits(const uint32_t* storage, uint32_t end) {
  uint32_t word_end = end >> 5;
  uint32_t partial_word_bits = end & 0x1f;

  uint32_t count = 0u;
  for (uint32_t word = 0u; word < word_end; word++) {
    count += POPCOUNT(storage[word]);
  }
  if (partial_word_bits != 0u) {
    count += POPCOUNT(storage[word_end] & ~(0xffffffffu << partial_word_bits));
  }
  return count;
}

void BitVector::Dump(std::ostream& os, const char *prefix) const {
  std::ostringstream buffer;
  DumpHelper(buffer, prefix);
  os << buffer.str() << std::endl;
}

void BitVector::DumpDot(FILE* file, const char* prefix, bool last_entry) const {
  std::ostringstream buffer;
  Dump(buffer, prefix);

  // Now print it to the file.
  fprintf(file, "    {%s}", buffer.str().c_str());

  // If it isn't the last entry, add a |.
  if (last_entry == false) {
    fprintf(file, "|");
  }

  // Add the \n.
  fprintf(file, "\\\n");
}

void BitVector::DumpHelper(std::ostringstream& buffer, const char* prefix) const {
  // Initialize it.
  if (prefix != nullptr) {
    buffer << prefix;
  }

  buffer << '(';
  for (size_t i = 0; i < number_of_bits_; i++) {
    buffer << IsBitSet(i);
  }
  buffer << ')';
}

}  // namespace art
