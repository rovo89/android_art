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

#ifndef ART_RUNTIME_LOCK_WORD_H_
#define ART_RUNTIME_LOCK_WORD_H_

#include <iosfwd>
#include <stdint.h>

#include "base/logging.h"

namespace art {
namespace mirror {
  class Object;
}  // namespace mirror

class Monitor;

/* The lock value itself as stored in mirror::Object::monitor_.  The MSB of the lock encodes its
 * state.  When cleared, the lock is in the "thin" state and its bits are formatted as follows:
 *
 *  |3|32222222222111|11111110000000000|
 *  |1|09876543210987|65432109876543210|
 *  |0| lock count   | thread id       |
 *
 * When set, the lock is in the "fat" state and its bits are formatted as follows:
 *
 *  |3|3222222222211111111110000000000|
 *  |1|0987654321098765432109876543210|
 *  |1| Monitor* >> 1                 |
 */
class LockWord {
 public:
  enum {
    // Number of bits to encode the state, currently just fat or thin/unlocked.
    kStateSize = 1,
    // Number of bits to encode the thin lock owner.
    kThinLockOwnerSize = 16,
    // Remaining bits are the recursive lock count.
    kThinLockCountSize = 32 - kThinLockOwnerSize - kStateSize,

    // Thin lock bits. Owner in lowest bits.
    kThinLockOwnerShift = 0,
    kThinLockOwnerMask = (1 << kThinLockOwnerSize) - 1,
    // Count in higher bits.
    kThinLockCountShift = kThinLockOwnerSize + kThinLockOwnerShift,
    kThinLockCountMask = (1 << kThinLockCountShift) - 1,
    kThinLockMaxCount = kThinLockCountMask,

    // State in the highest bits.
    kStateShift = kThinLockCountSize + kThinLockCountShift,
    kStateMask = (1 << kStateSize) - 1,
    kStateThinOrUnlocked = 0,
    kStateFat = 1,
  };

  static LockWord FromThinLockId(uint32_t thread_id, uint32_t count) {
    CHECK_LE(thread_id, static_cast<uint32_t>(kThinLockOwnerMask));
    return LockWord((thread_id << kThinLockOwnerShift) | (count << kThinLockCountShift));
  }

  enum LockState {
    kUnlocked,    // No lock owners.
    kThinLocked,  // Single uncontended owner.
    kFatLocked    // See associated monitor.
  };

  LockState GetState() const {
    if (value_ == 0) {
      return kUnlocked;
    } else if (((value_ >> kStateShift) & kStateMask) == kStateThinOrUnlocked) {
      return kThinLocked;
    } else {
      return kFatLocked;
    }
  }

  // Return the owner thin lock thread id.
  uint32_t ThinLockOwner() const;

  // Return the number of times a lock value has been locked.
  uint32_t ThinLockCount() const;

  // Return the Monitor encoded in a fat lock.
  Monitor* FatLockMonitor() const;

  // Default constructor with no lock ownership.
  LockWord();

  // Constructor a lock word for inflation to use a Monitor.
  explicit LockWord(Monitor* mon);

  bool operator==(const LockWord& rhs) {
    return GetValue() == rhs.GetValue();
  }

 private:
  explicit LockWord(uint32_t val) : value_(val) {}

  uint32_t GetValue() const {
    return value_;
  }

  // Only Object should be converting LockWords to/from uints.
  friend class mirror::Object;

  // The encoded value holding all the state.
  uint32_t value_;
};
std::ostream& operator<<(std::ostream& os, const LockWord::LockState& code);

}  // namespace art


#endif  // ART_RUNTIME_LOCK_WORD_H_
