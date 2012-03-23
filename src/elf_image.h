/*
 * Copyright (C) 2012 The Android Open Source Project
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

#ifndef ART_SRC_ELF_IMAGE_H_
#define ART_SRC_ELF_IMAGE_H_

#include "globals.h"
#include <string>

namespace art {

class ElfImage {
 public:
  explicit ElfImage(const std::string& str)
      : begin_(reinterpret_cast<const byte*>(str.data())), size_(str.size()) {
  }

  explicit ElfImage(const byte* begin, size_t size)
      : begin_(begin), size_(size) {
  }

  const byte* begin() const {
    return begin_;
  }

  const byte* end() const {
    return (begin_ + size_);
  }

  size_t size() const {
    return size_;
  }

 private:
  const byte* begin_;
  size_t size_;
};

} // namespace art

#endif // ART_SRC_ELF_IMAGE_H_
