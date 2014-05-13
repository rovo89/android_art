/*
 * Copyright (C) 2013 The Android Open Source Project
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

#ifndef ART_RUNTIME_BASE_BIT_VECTOR_H_
#define ART_RUNTIME_BASE_BIT_VECTOR_H_

#include <stdint.h>
#include <stddef.h>

#include "allocator.h"
#include "base/logging.h"
#include "utils.h"

namespace art {

/*
 * Expanding bitmap, used for tracking resources.  Bits are numbered starting
 * from zero.  All operations on a BitVector are unsynchronized.
 */
class BitVector {
  public:
    class Iterator {
      public:
        explicit Iterator(const BitVector* bit_vector)
          : p_bits_(bit_vector),
            bit_storage_(bit_vector->GetRawStorage()),
            bit_index_(0),
            bit_size_(p_bits_->storage_size_ * sizeof(uint32_t) * 8) {}

        // Return the position of the next set bit.  -1 means end-of-element reached.
        int32_t Next() {
          // Did anything obviously change since we started?
          DCHECK_EQ(bit_size_, p_bits_->GetStorageSize() * sizeof(uint32_t) * 8);
          DCHECK_EQ(bit_storage_, p_bits_->GetRawStorage());

          if (UNLIKELY(bit_index_ >= bit_size_)) {
            return -1;
          }

          uint32_t word_index = bit_index_ / 32;
          uint32_t word = bit_storage_[word_index];
          // Mask out any bits in the first word we've already considered.
          word >>= bit_index_ & 0x1f;
          if (word == 0) {
            bit_index_ &= ~0x1f;
            do {
              word_index++;
              if (UNLIKELY((word_index * 32) >= bit_size_)) {
                bit_index_ = bit_size_;
                return -1;
              }
              word = bit_storage_[word_index];
              bit_index_ += 32;
            } while (word == 0);
          }
          bit_index_ += CTZ(word) + 1;
          return bit_index_ - 1;
        }

        static void* operator new(size_t size, Allocator* allocator) {
          return allocator->Alloc(sizeof(BitVector::Iterator));
        };
        static void operator delete(void* p) {
          Iterator* it = reinterpret_cast<Iterator*>(p);
          it->p_bits_->allocator_->Free(p);
        }

      private:
        const BitVector* const p_bits_;
        const uint32_t* const bit_storage_;
        uint32_t bit_index_;           // Current index (size in bits).
        const uint32_t bit_size_;      // Size of vector in bits.

        friend class BitVector;
    };

    BitVector(uint32_t start_bits,
              bool expandable,
              Allocator* allocator,
              uint32_t storage_size = 0,
              uint32_t* storage = nullptr);

    virtual ~BitVector();

    void SetBit(uint32_t num);
    void ClearBit(uint32_t num);
    bool IsBitSet(uint32_t num) const;
    void ClearAllBits();
    void SetInitialBits(uint32_t num_bits);

    void Copy(const BitVector* src);
    void Intersect(const BitVector* src2);
    bool Union(const BitVector* src);

    // Set bits of union_with that are not in not_in.
    bool UnionIfNotIn(const BitVector* union_with, const BitVector* not_in);

    void Subtract(const BitVector* src);
    // Are we equal to another bit vector?  Note: expandability attributes must also match.
    bool Equal(const BitVector* src) {
      return (storage_size_ == src->GetStorageSize()) &&
        (expandable_ == src->IsExpandable()) &&
        (memcmp(storage_, src->GetRawStorage(), storage_size_ * sizeof(uint32_t)) == 0);
    }

    /**
     * @brief Are all the bits set the same?
     * @details expandability and size can differ as long as the same bits are set.
     */
    bool SameBitsSet(const BitVector *src);

    uint32_t NumSetBits() const;

    // Number of bits set in range [0, end).
    uint32_t NumSetBits(uint32_t end) const;

    Iterator* GetIterator() const;

    uint32_t GetStorageSize() const { return storage_size_; }
    bool IsExpandable() const { return expandable_; }
    uint32_t GetRawStorageWord(size_t idx) const { return storage_[idx]; }
    uint32_t* GetRawStorage() { return storage_; }
    const uint32_t* GetRawStorage() const { return storage_; }
    size_t GetSizeOf() const { return storage_size_ * sizeof(uint32_t); }

    /**
     * @return the highest bit set, -1 if none are set
     */
    int GetHighestBitSet() const;

    // Is bit set in storage. (No range check.)
    static bool IsBitSet(const uint32_t* storage, uint32_t num);
    // Number of bits set in range [0, end) in storage. (No range check.)
    static uint32_t NumSetBits(const uint32_t* storage, uint32_t end);

    bool EnsureSizeAndClear(unsigned int num);

    void Dump(std::ostream& os, const char* prefix) const;
    void DumpDot(FILE* file, const char* prefix, bool last_entry = false) const;

  protected:
    void DumpHelper(std::ostringstream& buffer, const char* prefix) const;

  private:
    Allocator* const allocator_;
    const bool expandable_;         // expand bitmap if we run out?
    uint32_t   storage_size_;       // current size, in 32-bit words.
    uint32_t*  storage_;
    uint32_t number_of_bits_;
};


}  // namespace art

#endif  // ART_RUNTIME_BASE_BIT_VECTOR_H_
