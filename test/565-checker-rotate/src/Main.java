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

  /// CHECK-START: int Main.rotateLeft32(int, int) intrinsics_recognition (after)
  /// CHECK-DAG: <<Result:i\d+>> InvokeStaticOrDirect intrinsic:IntegerRotateLeft
  /// CHECK-DAG:                 Return [<<Result>>]
  private static int rotateLeft32(int x, int y) {
    return Integer.rotateLeft(x, y);
  }

  /// CHECK-START: long Main.rotateLeft64(long, int) intrinsics_recognition (after)
  /// CHECK-DAG: <<Result:j\d+>> InvokeStaticOrDirect intrinsic:LongRotateLeft
  /// CHECK-DAG:                 Return [<<Result>>]
  private static long rotateLeft64(long x, int y) {
    return Long.rotateLeft(x, y);
  }

  /// CHECK-START: int Main.rotateRight32(int, int) intrinsics_recognition (after)
  /// CHECK-DAG: <<Result:i\d+>> InvokeStaticOrDirect intrinsic:IntegerRotateRight
  /// CHECK-DAG:                 Return [<<Result>>]
  private static int rotateRight32(int x, int y) {
    return Integer.rotateRight(x, y);
  }

  /// CHECK-START: long Main.rotateRight64(long, int) intrinsics_recognition (after)
  /// CHECK-DAG: <<Result:j\d+>> InvokeStaticOrDirect intrinsic:LongRotateRight
  /// CHECK-DAG:                 Return [<<Result>>]
  private static long rotateRight64(long x, int y) {
    return Long.rotateRight(x, y);
  }

  public static void main(String args[]) {
    expectEquals32(0x00000001, rotateLeft32(0x00000001, 0));
    expectEquals32(0x00000002, rotateLeft32(0x00000001, 1));
    expectEquals32(0x80000000, rotateLeft32(0x00000001, 31));
    expectEquals32(0x00000001, rotateLeft32(0x00000001, 32));  // overshoot
    expectEquals32(0x00000003, rotateLeft32(0x80000001, 1));
    expectEquals32(0x00000006, rotateLeft32(0x80000001, 2));
    expectEquals32(0x23456781, rotateLeft32(0x12345678, 4));
    expectEquals32(0xBCDEF09A, rotateLeft32(0x9ABCDEF0, 8));
    for (int i = 0; i < 40; i++) {  // overshoot a bit
      int j = i & 31;
      expectEquals32(0x00000000, rotateLeft32(0x00000000, i));
      expectEquals32(0xFFFFFFFF, rotateLeft32(0xFFFFFFFF, i));
      expectEquals32(1 << j, rotateLeft32(0x00000001, i));
      expectEquals32((0x12345678 << j) | (0x12345678 >>> -j),
                     rotateLeft32(0x12345678, i));
    }

    expectEquals64(0x0000000000000001L, rotateLeft64(0x0000000000000001L, 0));
    expectEquals64(0x0000000000000002L, rotateLeft64(0x0000000000000001L, 1));
    expectEquals64(0x8000000000000000L, rotateLeft64(0x0000000000000001L, 63));
    expectEquals64(0x0000000000000001L, rotateLeft64(0x0000000000000001L, 64));  // overshoot
    expectEquals64(0x0000000000000003L, rotateLeft64(0x8000000000000001L, 1));
    expectEquals64(0x0000000000000006L, rotateLeft64(0x8000000000000001L, 2));
    expectEquals64(0x23456789ABCDEF01L, rotateLeft64(0x123456789ABCDEF0L, 4));
    expectEquals64(0x3456789ABCDEF012L, rotateLeft64(0x123456789ABCDEF0L, 8));
    for (int i = 0; i < 70; i++) {  // overshoot a bit
      int j = i & 63;
      expectEquals64(0x0000000000000000L, rotateLeft64(0x0000000000000000L, i));
      expectEquals64(0xFFFFFFFFFFFFFFFFL, rotateLeft64(0xFFFFFFFFFFFFFFFFL, i));
      expectEquals64(1L << j, rotateLeft64(0x0000000000000001, i));
      expectEquals64((0x123456789ABCDEF0L << j) | (0x123456789ABCDEF0L >>> -j),
                     rotateLeft64(0x123456789ABCDEF0L, i));
    }

    expectEquals32(0x80000000, rotateRight32(0x80000000, 0));
    expectEquals32(0x40000000, rotateRight32(0x80000000, 1));
    expectEquals32(0x00000001, rotateRight32(0x80000000, 31));
    expectEquals32(0x80000000, rotateRight32(0x80000000, 32));  // overshoot
    expectEquals32(0xC0000000, rotateRight32(0x80000001, 1));
    expectEquals32(0x60000000, rotateRight32(0x80000001, 2));
    expectEquals32(0x81234567, rotateRight32(0x12345678, 4));
    expectEquals32(0xF09ABCDE, rotateRight32(0x9ABCDEF0, 8));
    for (int i = 0; i < 40; i++) {  // overshoot a bit
      int j = i & 31;
      expectEquals32(0x00000000, rotateRight32(0x00000000, i));
      expectEquals32(0xFFFFFFFF, rotateRight32(0xFFFFFFFF, i));
      expectEquals32(0x80000000 >>> j, rotateRight32(0x80000000, i));
      expectEquals32((0x12345678 >>> j) | (0x12345678 << -j),
                     rotateRight32(0x12345678, i));
    }

    expectEquals64(0x8000000000000000L, rotateRight64(0x8000000000000000L, 0));
    expectEquals64(0x4000000000000000L, rotateRight64(0x8000000000000000L, 1));
    expectEquals64(0x0000000000000001L, rotateRight64(0x8000000000000000L, 63));
    expectEquals64(0x8000000000000000L, rotateRight64(0x8000000000000000L, 64));  // overshoot
    expectEquals64(0xC000000000000000L, rotateRight64(0x8000000000000001L, 1));
    expectEquals64(0x6000000000000000L, rotateRight64(0x8000000000000001L, 2));
    expectEquals64(0x0123456789ABCDEFL, rotateRight64(0x123456789ABCDEF0L, 4));
    expectEquals64(0xF0123456789ABCDEL, rotateRight64(0x123456789ABCDEF0L, 8));
    for (int i = 0; i < 70; i++) {  // overshoot a bit
      int j = i & 63;
      expectEquals64(0x0000000000000000L, rotateRight64(0x0000000000000000L, i));
      expectEquals64(0xFFFFFFFFFFFFFFFFL, rotateRight64(0xFFFFFFFFFFFFFFFFL, i));
      expectEquals64(0x8000000000000000L >>> j, rotateRight64(0x8000000000000000L, i));
      expectEquals64((0x123456789ABCDEF0L >>> j) | (0x123456789ABCDEF0L << -j),
                     rotateRight64(0x123456789ABCDEF0L, i));
    }

    System.out.println("passed");
  }

  private static void expectEquals32(int expected, int result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }
  private static void expectEquals64(long expected, long result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }
}
