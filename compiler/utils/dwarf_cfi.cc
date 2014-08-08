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

#include "leb128.h"
#include "utils.h"

#include "dwarf_cfi.h"

namespace art {

void DW_CFA_advance_loc(std::vector<uint8_t>* buf, uint32_t increment) {
  if (increment < 64) {
    // Encoding in opcode.
    buf->push_back(0x1 << 6 | increment);
  } else if (increment < 256) {
    // Single byte delta.
    buf->push_back(0x02);
    buf->push_back(increment);
  } else if (increment < 256 * 256) {
    // Two byte delta.
    buf->push_back(0x03);
    buf->push_back(increment & 0xff);
    buf->push_back((increment >> 8) & 0xff);
  } else {
    // Four byte delta.
    buf->push_back(0x04);
    PushWord(buf, increment);
  }
}

void DW_CFA_offset_extended_sf(std::vector<uint8_t>* buf, int reg, int32_t offset) {
  buf->push_back(0x11);
  EncodeUnsignedLeb128(reg, buf);
  EncodeSignedLeb128(offset, buf);
}

void DW_CFA_offset(std::vector<uint8_t>* buf, int reg, uint32_t offset) {
  buf->push_back((0x2 << 6) | reg);
  EncodeUnsignedLeb128(offset, buf);
}

void DW_CFA_def_cfa_offset(std::vector<uint8_t>* buf, int32_t offset) {
  buf->push_back(0x0e);
  EncodeUnsignedLeb128(offset, buf);
}

void DW_CFA_remember_state(std::vector<uint8_t>* buf) {
  buf->push_back(0x0a);
}

void DW_CFA_restore_state(std::vector<uint8_t>* buf) {
  buf->push_back(0x0b);
}

void WriteFDEHeader(std::vector<uint8_t>* buf, bool is_64bit) {
  // 'length' (filled in by other functions).
  if (is_64bit) {
    PushWord(buf, 0xffffffff);  // Indicates 64bit
    PushWord(buf, 0);
    PushWord(buf, 0);
  } else {
    PushWord(buf, 0);
  }

  // 'CIE_pointer' (filled in by linker).
  if (is_64bit) {
    PushWord(buf, 0);
    PushWord(buf, 0);
  } else {
    PushWord(buf, 0);
  }

  // 'initial_location' (filled in by linker).
  if (is_64bit) {
    PushWord(buf, 0);
    PushWord(buf, 0);
  } else {
    PushWord(buf, 0);
  }

  // 'address_range' (filled in by other functions).
  if (is_64bit) {
    PushWord(buf, 0);
    PushWord(buf, 0);
  } else {
    PushWord(buf, 0);
  }

  // Augmentation length: 0
  buf->push_back(0);
}

void WriteFDEAddressRange(std::vector<uint8_t>* buf, uint64_t data, bool is_64bit) {
  const size_t kOffsetOfAddressRange = is_64bit? 28 : 12;
  CHECK(buf->size() >= kOffsetOfAddressRange + (is_64bit? 8 : 4));

  uint8_t *p = buf->data() + kOffsetOfAddressRange;
  if (is_64bit) {
    p[0] = data;
    p[1] = data >> 8;
    p[2] = data >> 16;
    p[3] = data >> 24;
    p[4] = data >> 32;
    p[5] = data >> 40;
    p[6] = data >> 48;
    p[7] = data >> 56;
  } else {
    p[0] = data;
    p[1] = data >> 8;
    p[2] = data >> 16;
    p[3] = data >> 24;
  }
}

void WriteCFILength(std::vector<uint8_t>* buf, bool is_64bit) {
  uint64_t length = is_64bit ? buf->size() - 12 : buf->size() - 4;
  DCHECK_EQ((length & 0x3), 0U);

  uint8_t *p = is_64bit? buf->data() + 4 : buf->data();
  if (is_64bit) {
    p[0] = length;
    p[1] = length >> 8;
    p[2] = length >> 16;
    p[3] = length >> 24;
    p[4] = length >> 32;
    p[5] = length >> 40;
    p[6] = length >> 48;
    p[7] = length >> 56;
  } else {
    p[0] = length;
    p[1] = length >> 8;
    p[2] = length >> 16;
    p[3] = length >> 24;
  }
}

void PadCFI(std::vector<uint8_t>* buf) {
  while (buf->size() & 0x3) {
    buf->push_back(0);
  }
}

}  // namespace art
