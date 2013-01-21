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

#include "vector_output_stream.h"

#include <string.h>

#include "base/logging.h"

namespace art {

VectorOutputStream::VectorOutputStream(const std::string& location, std::vector<uint8_t>& vector)
  : OutputStream(location), offset_(vector.size()), vector_(vector) {};

bool VectorOutputStream::WriteFully(const void* buffer, int64_t byte_count) {
  off_t new_offset = offset_ + byte_count;
  EnsureCapacity(new_offset);
  memcpy(&vector_[offset_], buffer, byte_count);
  offset_ = new_offset;
  return true;
}

off_t VectorOutputStream::lseek(off_t offset, int whence) {
  CHECK(whence == SEEK_SET || whence == SEEK_CUR || whence == SEEK_END) << whence;
  off_t new_offset;
  switch (whence) {
    case SEEK_SET: {
      new_offset = offset;
      break;
    }
    case SEEK_CUR: {
      new_offset = offset_ + offset;
      break;
    }
    case SEEK_END: {
      new_offset = vector_.size() + offset;
      break;
    }
    default: {
      LOG(FATAL) << whence;
      new_offset = -1;
    }
  }
  EnsureCapacity(new_offset);
  offset_ = new_offset;
  return offset_;
}

void VectorOutputStream::EnsureCapacity(off_t new_offset) {
  if (new_offset > static_cast<off_t>(vector_.size())) {
    vector_.resize(new_offset);
  }
}

}  // namespace art
