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

#ifndef ART_SRC_GREENLAND_LIR_FRAME_INFO_H_
#define ART_SRC_GREENLAND_LIR_FRAME_INFO_H_

#include "logging.h"

#include <vector>

namespace art {
namespace greenland {

class LIRFrameInfo {
 private:
  struct StackObject {
    // The offset from the stack pointer
    off_t sp_offset_;

    // The object is a dead object if its size is 0
    size_t size_;

    unsigned alignment_;

    StackObject(size_t size, unsigned alignment)
        : size_(size), alignment_(alignment) { }
  };

  // stack_size_ and sp_offset_ in each StackObject will be updated by ...
  size_t stack_size_;

  std::vector<StackObject> objects_;

 public:
  LIRFrameInfo() : stack_size_(0) { }

  bool HasStackObjects() const {
    return !objects_.empty();
  }

  unsigned GetNumObjects() const {
    return objects_.size();
  }

  size_t GetStackSize() const {
    return stack_size_;
  }
  void SetStackSize(size_t stack_size) {
    stack_size_ = stack_size;
    return;
  }

  //----------------------------------------------------------------------------
  // Stack Object
  //----------------------------------------------------------------------------
  size_t GetObjectOffset(unsigned idx) const {
    DCHECK(idx < GetNumObjects()) << "Invalid frame index!";
    return objects_[idx].sp_offset_;
  }
  void SetObjectOffset(unsigned idx, off_t sp_offset) {
    DCHECK(idx < GetNumObjects()) << "Invalid frame index!";
    objects_[idx].sp_offset_ = sp_offset;
    return;
  }

  size_t GetObjectSize(unsigned idx) const {
    DCHECK(idx < GetNumObjects()) << "Invalid frame index!";
    return objects_[idx].size_;
  }

  size_t GetObjectAlignment(unsigned idx) const {
    DCHECK(idx < GetNumObjects()) << "Invalid frame index!";
    return objects_[idx].alignment_;
  }

  unsigned AllocateStackObject(size_t size, unsigned alignment = 1) {
    DCHECK(size > 0);
    objects_.push_back(StackObject(size, alignment));
    return objects_.size() - 1;
  }

  void RemoveStackObject(unsigned idx) {
    DCHECK(idx < GetNumObjects()) << "Invalid frame index!";
    objects_[idx].size_ = 0;
    return;
  }
};

} // namespace greenland
} // namespace art

#endif // ART_SRC_GREENLAND_LIR_FRAME_INFO_H_
