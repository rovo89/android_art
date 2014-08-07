/*
 * Copyright (C) 2014 The Android Open Source Project
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

#include "card_table-inl.h"

#include <string>

#include "atomic.h"
#include "common_runtime_test.h"
#include "handle_scope-inl.h"
#include "mirror/class-inl.h"
#include "mirror/string-inl.h"  // Strings are easiest to allocate
#include "scoped_thread_state_change.h"
#include "thread_pool.h"
#include "utils.h"

namespace art {

namespace mirror {
  class Object;
}  // namespace mirror

namespace gc {
namespace accounting {

class CardTableTest : public CommonRuntimeTest {
 public:
  std::unique_ptr<CardTable> card_table_;

  void CommonSetup() {
    if (card_table_.get() == nullptr) {
      card_table_.reset(CardTable::Create(heap_begin_, heap_size_));
      EXPECT_TRUE(card_table_.get() != nullptr);
    } else {
      ClearCardTable();
    }
  }
  // Default values for the test, not random to avoid undeterministic behaviour.
  CardTableTest() : heap_begin_(reinterpret_cast<byte*>(0x2000000)), heap_size_(2 * MB) {
  }
  void ClearCardTable() {
    card_table_->ClearCardTable();
  }
  byte* HeapBegin() const {
    return heap_begin_;
  }
  byte* HeapLimit() const {
    return HeapBegin() + heap_size_;
  }
  // Return a pseudo random card for an address.
  byte PseudoRandomCard(const byte* addr) const {
    size_t offset = RoundDown(addr - heap_begin_, CardTable::kCardSize);
    return 1 + offset % 254;
  }
  void FillRandom() {
    for (const byte* addr = HeapBegin(); addr != HeapLimit(); addr += CardTable::kCardSize) {
      EXPECT_TRUE(card_table_->AddrIsInCardTable(addr));
      byte* card = card_table_->CardFromAddr(addr);
      *card = PseudoRandomCard(addr);
    }
  }

 private:
  byte* const heap_begin_;
  const size_t heap_size_;
};

TEST_F(CardTableTest, TestMarkCard) {
  CommonSetup();
  for (const byte* addr = HeapBegin(); addr < HeapLimit(); addr += kObjectAlignment) {
    auto obj = reinterpret_cast<const mirror::Object*>(addr);
    EXPECT_EQ(card_table_->GetCard(obj), CardTable::kCardClean);
    EXPECT_TRUE(!card_table_->IsDirty(obj));
    card_table_->MarkCard(addr);
    EXPECT_TRUE(card_table_->IsDirty(obj));
    EXPECT_EQ(card_table_->GetCard(obj), CardTable::kCardDirty);
    byte* card_addr = card_table_->CardFromAddr(addr);
    EXPECT_EQ(*card_addr, CardTable::kCardDirty);
    *card_addr = CardTable::kCardClean;
    EXPECT_EQ(*card_addr, CardTable::kCardClean);
  }
}

class UpdateVisitor {
 public:
  byte operator()(byte c) const {
    return c * 93 + 123;
  }
  void operator()(byte* /*card*/, byte /*expected_value*/, byte /*new_value*/) const {
  }
};

TEST_F(CardTableTest, TestModifyCardsAtomic) {
  CommonSetup();
  FillRandom();
  const size_t delta = std::min(static_cast<size_t>(HeapLimit() - HeapBegin()),
                                8U * CardTable::kCardSize);
  UpdateVisitor visitor;
  size_t start_offset = 0;
  for (byte* cstart = HeapBegin(); cstart < HeapBegin() + delta; cstart += CardTable::kCardSize) {
    start_offset = (start_offset + kObjectAlignment) % CardTable::kCardSize;
    size_t end_offset = 0;
    for (byte* cend = HeapLimit() - delta; cend < HeapLimit(); cend += CardTable::kCardSize) {
      // Don't always start at a card boundary.
      byte* start = cstart + start_offset;
      byte* end = cend - end_offset;
      end_offset = (end_offset + kObjectAlignment) % CardTable::kCardSize;
      // Modify cards.
      card_table_->ModifyCardsAtomic(start, end, visitor, visitor);
      // Check adjacent cards not modified.
      for (byte* cur = start - CardTable::kCardSize; cur >= HeapBegin();
          cur -= CardTable::kCardSize) {
        EXPECT_EQ(card_table_->GetCard(reinterpret_cast<mirror::Object*>(cur)),
                  PseudoRandomCard(cur));
      }
      for (byte* cur = end + CardTable::kCardSize; cur < HeapLimit();
          cur += CardTable::kCardSize) {
        EXPECT_EQ(card_table_->GetCard(reinterpret_cast<mirror::Object*>(cur)),
                  PseudoRandomCard(cur));
      }
      // Verify Range.
      for (byte* cur = start; cur < AlignUp(end, CardTable::kCardSize);
          cur += CardTable::kCardSize) {
        byte* card = card_table_->CardFromAddr(cur);
        byte value = PseudoRandomCard(cur);
        EXPECT_EQ(visitor(value), *card);
        // Restore for next iteration.
        *card = value;
      }
    }
  }
}

// TODO: Add test for CardTable::Scan.
}  // namespace accounting
}  // namespace gc
}  // namespace art
