/*
 * Copyright (C) 2016 The Android Open Source Project
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

public class Main {

  /// CHECK-START: int Main.sign32(int) intrinsics_recognition (after)
  /// CHECK-DAG: <<Result:i\d+>> InvokeStaticOrDirect intrinsic:IntegerSignum
  /// CHECK-DAG:                 Return [<<Result>>]
  private static int sign32(int x) {
    return Integer.signum(x);
  }

  /// CHECK-START: int Main.sign64(long) intrinsics_recognition (after)
  /// CHECK-DAG: <<Result:i\d+>> InvokeStaticOrDirect intrinsic:LongSignum
  /// CHECK-DAG:                 Return [<<Result>>]
  private static int sign64(long x) {
    return Long.signum(x);
  }

  public static void main(String args[]) {
    expectEquals(-1, sign32(Integer.MIN_VALUE));
    expectEquals(-1, sign32(-12345));
    expectEquals(-1, sign32(-1));
    expectEquals(0, sign32(0));
    expectEquals(1, sign32(1));
    expectEquals(1, sign32(12345));
    expectEquals(1, sign32(Integer.MAX_VALUE));

    for (int i = -11; i <= 11; i++) {
      int expected = 0;
      if (i < 0) expected = -1;
      else if (i > 0) expected = 1;
      expectEquals(expected, sign32(i));
    }

    expectEquals(-1, sign64(Long.MIN_VALUE));
    expectEquals(-1, sign64(-12345L));
    expectEquals(-1, sign64(-1L));
    expectEquals(0, sign64(0L));
    expectEquals(1, sign64(1L));
    expectEquals(1, sign64(12345L));
    expectEquals(1, sign64(Long.MAX_VALUE));

    expectEquals(-1, sign64(0x800000007FFFFFFFL));
    expectEquals(-1, sign64(0x80000000FFFFFFFFL));
    expectEquals(1, sign64(0x000000007FFFFFFFL));
    expectEquals(1, sign64(0x00000000FFFFFFFFL));
    expectEquals(1, sign64(0x7FFFFFFF7FFFFFFFL));
    expectEquals(1, sign64(0x7FFFFFFFFFFFFFFFL));

    for (long i = -11L; i <= 11L; i++) {
      int expected = 0;
      if (i < 0) expected = -1;
      else if (i > 0) expected = 1;
      expectEquals(expected, sign64(i));
    }

    for (long i = Long.MIN_VALUE; i <= Long.MIN_VALUE + 11L; i++) {
      expectEquals(-1, sign64(i));
    }

    for (long i = Long.MAX_VALUE; i >= Long.MAX_VALUE - 11L; i--) {
      expectEquals(1, sign64(i));
    }

    System.out.println("passed");
  }

  private static void expectEquals(int expected, int result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }
}
