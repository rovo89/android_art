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

  /// CHECK-START: int Main.compare32(int, int) intrinsics_recognition (after)
  /// CHECK-DAG: <<Result:i\d+>> InvokeStaticOrDirect intrinsic:IntegerCompare
  /// CHECK-DAG:                 Return [<<Result>>]
  private static int compare32(int x, int y) {
    return Integer.compare(x, y);
  }

  /// CHECK-START: int Main.compare64(long, long) intrinsics_recognition (after)
  /// CHECK-DAG: <<Result:i\d+>> InvokeStaticOrDirect intrinsic:LongCompare
  /// CHECK-DAG:                 Return [<<Result>>]
  private static int compare64(long x, long y) {
    return Long.compare(x, y);
  }

  public static void main(String args[]) {
    expectEquals(-1, compare32(Integer.MIN_VALUE, Integer.MIN_VALUE + 1));
    expectEquals(-1, compare32(Integer.MIN_VALUE, -1));
    expectEquals(-1, compare32(Integer.MIN_VALUE, 0));
    expectEquals(-1, compare32(Integer.MIN_VALUE, 1));
    expectEquals(-1, compare32(Integer.MIN_VALUE, Integer.MAX_VALUE));
    expectEquals(-1, compare32(-1, 0));
    expectEquals(-1, compare32(-1, 1));
    expectEquals(-1, compare32(0, 1));

    expectEquals(0, compare32(Integer.MIN_VALUE, Integer.MIN_VALUE));
    expectEquals(0, compare32(-1, -1));
    expectEquals(0, compare32(0, 0));
    expectEquals(0, compare32(1, 1));
    expectEquals(0, compare32(Integer.MAX_VALUE, Integer.MAX_VALUE));

    expectEquals(1, compare32(0, -1));
    expectEquals(1, compare32(1, -1));
    expectEquals(1, compare32(1, 0));
    expectEquals(1, compare32(Integer.MAX_VALUE, Integer.MIN_VALUE));
    expectEquals(1, compare32(Integer.MAX_VALUE, -1));
    expectEquals(1, compare32(Integer.MAX_VALUE, 0));
    expectEquals(1, compare32(Integer.MAX_VALUE, 1));
    expectEquals(1, compare32(Integer.MAX_VALUE, Integer.MAX_VALUE - 1));

    for (int i = -11; i <= 11; i++) {
      for (int j = -11; j <= 11; j++) {
        int expected = 0;
        if (i < j) expected = -1;
        else if (i > j) expected = 1;
        expectEquals(expected, compare32(i, j));
      }
    }

    expectEquals(-1, compare64(Long.MIN_VALUE, Long.MIN_VALUE + 1L));
    expectEquals(-1, compare64(Long.MIN_VALUE, -1L));
    expectEquals(-1, compare64(Long.MIN_VALUE, 0L));
    expectEquals(-1, compare64(Long.MIN_VALUE, 1L));
    expectEquals(-1, compare64(Long.MIN_VALUE, Long.MAX_VALUE));
    expectEquals(-1, compare64(-1L, 0L));
    expectEquals(-1, compare64(-1L, 1L));
    expectEquals(-1, compare64(0L, 1L));

    expectEquals(0, compare64(Long.MIN_VALUE, Long.MIN_VALUE));
    expectEquals(0, compare64(-1L, -1L));
    expectEquals(0, compare64(0L, 0L));
    expectEquals(0, compare64(1L, 1L));
    expectEquals(0, compare64(Long.MAX_VALUE, Long.MAX_VALUE));

    expectEquals(1, compare64(0L, -1L));
    expectEquals(1, compare64(1L, -1L));
    expectEquals(1, compare64(1L, 0L));
    expectEquals(1, compare64(Long.MAX_VALUE, Long.MIN_VALUE));
    expectEquals(1, compare64(Long.MAX_VALUE, -1L));
    expectEquals(1, compare64(Long.MAX_VALUE, 0L));
    expectEquals(1, compare64(Long.MAX_VALUE, 1L));
    expectEquals(1, compare64(Long.MAX_VALUE, Long.MAX_VALUE - 1L));

    expectEquals(-1, compare64(0x111111117FFFFFFFL, 0x11111111FFFFFFFFL));
    expectEquals(0, compare64(0x111111117FFFFFFFL, 0x111111117FFFFFFFL));
    expectEquals(1, compare64(0x11111111FFFFFFFFL, 0x111111117FFFFFFFL));

    for (long i = -11L; i <= 11L; i++) {
      for (long j = -11L; j <= 11L; j++) {
        int expected = 0;
        if (i < j) expected = -1;
        else if (i > j) expected = 1;
        expectEquals(expected, compare64(i, j));
      }
    }

    for (long i = Long.MIN_VALUE; i <= Long.MIN_VALUE + 11L; i++) {
      expectEquals(-1, compare64(i, 0));
    }

    for (long i = Long.MAX_VALUE; i >= Long.MAX_VALUE - 11L; i--) {
      expectEquals(1, compare64(i, 0));
    }

    System.out.println("passed");
  }

  private static void expectEquals(int expected, int result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }
}
