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

#include <stdint.h>

namespace art {

int CmplFloat(float a, float b) {
  if (a == b) {
    return 0;
  } else if (a < b) {
    return -1;
  } else if (a > b) {
    return 1;
  }
  return -1;
}

int CmpgFloat(float a, float b) {
  if (a == b) {
    return 0;
  } else if (a < b) {
    return -1;
  } else if (a > b) {
    return 1;
  }
  return 1;
}

int CmpgDouble(double a, double b) {
  if (a == b) {
    return 0;
  } else if (a < b) {
    return -1;
  } else if (a > b) {
    return 1;
  }
  return 1;
}

int CmplDouble(double a, double b) {
  if (a == b) {
    return 0;
  } else if (a < b) {
    return -1;
  } else if (a > b) {
    return 1;
  }
  return -1;
}

/*
 * Float/double conversion requires clamping to min and max of integer form.  If
 * target doesn't support this normally, use these.
 */
int64_t D2L(double d) {
  static const double kMaxLong = (double) (int64_t) 0x7fffffffffffffffULL;
  static const double kMinLong = (double) (int64_t) 0x8000000000000000ULL;
  if (d >= kMaxLong) {
    return (int64_t) 0x7fffffffffffffffULL;
  } else if (d <= kMinLong) {
    return (int64_t) 0x8000000000000000ULL;
  } else if (d != d)  { // NaN case
    return 0;
  } else {
    return (int64_t) d;
  }
}

int64_t F2L(float f) {
  static const float kMaxLong = (float) (int64_t) 0x7fffffffffffffffULL;
  static const float kMinLong = (float) (int64_t) 0x8000000000000000ULL;
  if (f >= kMaxLong) {
    return (int64_t) 0x7fffffffffffffffULL;
  } else if (f <= kMinLong) {
    return (int64_t) 0x8000000000000000ULL;
  } else if (f != f) { // NaN case
    return 0;
  } else {
    return (int64_t) f;
  }
}

int32_t D2I(double d) {
  static const double kMaxLong = (double) (int64_t) 0x7fffffffUL;
  static const double kMinLong = (double) (int64_t) 0x80000000UL;
  if (d >= kMaxLong) {
    return (int32_t) 0x7fffffffUL;
  } else if (d <= kMinLong) {
    return (int32_t) 0x80000000UL;
  } else if (d != d)  { // NaN case
    return 0;
  } else {
    return (int32_t) d;
  }
}

int32_t F2I(float f) {
  static const float kMaxLong = (float) (int64_t) 0x7fffffffUL;
  static const float kMinLong = (float) (int64_t) 0x80000000UL;
  if (f >= kMaxLong) {
    return (int32_t) 0x7fffffffUL;
  } else if (f <= kMinLong) {
    return (int32_t) 0x80000000UL;
  } else if (f != f) { // NaN case
    return 0;
  } else {
    return (int32_t) f;
  }
}


extern "C" int64_t artLdivFromCode(int64_t a, int64_t b) {
  return a / b;
}

extern "C" int64_t artLdivmodFromCode(int64_t a, int64_t b) {
  return a % b;
}

}  // namespace art
