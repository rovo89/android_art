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

#ifndef ART_SRC_INVOKE_ARG_ARRAY_BUILDER_H_
#define ART_SRC_INVOKE_ARG_ARRAY_BUILDER_H_

#include "object.h"
#include "scoped_thread_state_change.h"

namespace art {

static inline size_t NumArgArrayBytes(const char* shorty, uint32_t shorty_len) {
  size_t num_bytes = 0;
  for (size_t i = 1; i < shorty_len; ++i) {
    char ch = shorty[i];
    if (ch == 'D' || ch == 'J') {
      num_bytes += 8;
    } else if (ch == 'L') {
      // Argument is a reference or an array.  The shorty descriptor
      // does not distinguish between these types.
      num_bytes += sizeof(Object*);
    } else {
      num_bytes += 4;
    }
  }
  return num_bytes;
}

class ArgArray {
 public:
  explicit ArgArray(const char* shorty, uint32_t shorty_len)
      : shorty_(shorty), shorty_len_(shorty_len) {
    if (shorty_len - 1 < kSmallArgArraySize) {
      arg_array_ = small_arg_array_;
    } else {
      large_arg_array_.reset(new JValue[shorty_len_ - 1]);
      arg_array_ = large_arg_array_.get();
    }
  }

  JValue* get() {
    return arg_array_;
  }

  void BuildArgArray(const ScopedObjectAccess& soa, va_list ap)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    for (size_t i = 1, offset = 0; i < shorty_len_; ++i, ++offset) {
      switch (shorty_[i]) {
        case 'Z':
          arg_array_[offset].SetZ(va_arg(ap, jint));
          break;
        case 'B':
          arg_array_[offset].SetB(va_arg(ap, jint));
          break;
        case 'C':
          arg_array_[offset].SetC(va_arg(ap, jint));
          break;
        case 'S':
          arg_array_[offset].SetS(va_arg(ap, jint));
          break;
        case 'I':
          arg_array_[offset].SetI(va_arg(ap, jint));
          break;
        case 'F':
          arg_array_[offset].SetF(va_arg(ap, jdouble));
          break;
        case 'L':
          arg_array_[offset].SetL(soa.Decode<Object*>(va_arg(ap, jobject)));
          break;
        case 'D':
          arg_array_[offset].SetD(va_arg(ap, jdouble));
          break;
        case 'J':
          arg_array_[offset].SetJ(va_arg(ap, jlong));
          break;
      }
    }
  }

  void BuildArgArray(const ScopedObjectAccess& soa, jvalue* args)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    for (size_t i = 1, offset = 0; i < shorty_len_; ++i, ++offset) {
      switch (shorty_[i]) {
        case 'Z':
          arg_array_[offset].SetZ(args[offset].z);
          break;
        case 'B':
          arg_array_[offset].SetB(args[offset].b);
          break;
        case 'C':
          arg_array_[offset].SetC(args[offset].c);
          break;
        case 'S':
          arg_array_[offset].SetS(args[offset].s);
          break;
        case 'I':
          arg_array_[offset].SetI(args[offset].i);
          break;
        case 'F':
          arg_array_[offset].SetF(args[offset].f);
          break;
        case 'L':
          arg_array_[offset].SetL(soa.Decode<Object*>(args[offset].l));
          break;
        case 'D':
          arg_array_[offset].SetD(args[offset].d);
          break;
        case 'J':
          arg_array_[offset].SetJ(args[offset].j);
          break;
      }
    }
  }

  void BuildArgArray(const ShadowFrame& shadow_frame, uint32_t range_start)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    for (size_t i = 1, offset = 0; i < shorty_len_; ++i, ++offset) {
      switch (shorty_[i]) {
        case 'Z':
          arg_array_[i - 1].SetZ(shadow_frame.GetVReg(range_start + offset));
          break;
        case 'B':
          arg_array_[i - 1].SetB(shadow_frame.GetVReg(range_start + offset));
          break;
        case 'C':
          arg_array_[i - 1].SetC(shadow_frame.GetVReg(range_start + offset));
          break;
        case 'S':
          arg_array_[i - 1].SetS(shadow_frame.GetVReg(range_start + offset));
          break;
        case 'I':
          arg_array_[i - 1].SetI(shadow_frame.GetVReg(range_start + offset));
          break;
        case 'F':
          arg_array_[i - 1].SetF(shadow_frame.GetVRegFloat(range_start + offset));
          break;
        case 'L':
          arg_array_[i - 1].SetL(shadow_frame.GetReference(range_start + offset));
          break;
        case 'D':
          arg_array_[i - 1].SetD(shadow_frame.GetVRegDouble(range_start + offset));
          offset++;
          break;
        case 'J':
          arg_array_[i - 1].SetJ(shadow_frame.GetVRegLong(range_start + offset));
          offset++;
          break;
      }
    }
  }

  void BuildArgArray(const ShadowFrame& shadow_frame, const uint32_t* arg_regs)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    for (size_t i = 1, offset = 0; i < shorty_len_; ++i, ++offset) {
      switch (shorty_[i]) {
        case 'Z':
          arg_array_[i - 1].SetZ(shadow_frame.GetVReg(arg_regs[offset]));
          break;
        case 'B':
          arg_array_[i - 1].SetB(shadow_frame.GetVReg(arg_regs[offset]));
          break;
        case 'C':
          arg_array_[i - 1].SetC(shadow_frame.GetVReg(arg_regs[offset]));
          break;
        case 'S':
          arg_array_[i - 1].SetS(shadow_frame.GetVReg(arg_regs[offset]));
          break;
        case 'I':
          arg_array_[i - 1].SetI(shadow_frame.GetVReg(arg_regs[offset]));
          break;
        case 'F':
          arg_array_[i - 1].SetF(shadow_frame.GetVRegFloat(arg_regs[offset]));
          break;
        case 'L':
          arg_array_[i - 1].SetL(shadow_frame.GetReference(arg_regs[offset]));
          break;
        case 'D':
          arg_array_[i - 1].SetD(shadow_frame.GetVRegDouble(arg_regs[offset]));
          offset++;
          break;
        case 'J':
          arg_array_[i - 1].SetJ(shadow_frame.GetVRegLong(arg_regs[offset]));
          offset++;
          break;
      }
    }
  }

 private:
  enum { kSmallArgArraySize = 16 };
  const char* const shorty_;
  const uint32_t shorty_len_;
  JValue* arg_array_;
  JValue small_arg_array_[kSmallArgArraySize];
  UniquePtr<JValue[]> large_arg_array_;
};

}  // namespace art

#endif  // ART_SRC_INVOKE_ARG_ARRAY_BUILDER_H_
