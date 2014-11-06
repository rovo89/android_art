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

// Note that $opt$ is a marker for the optimizing compiler to ensure
// it does compile the method.
public class Main {

  public static void assertEquals(long expected, long result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  public static void main(String[] args) {
    byteToLong();
    shortToLong();
    intToLong();
  }

  private static void byteToLong() {
    assertEquals(1L, $opt$ByteToLong((byte)1));
    assertEquals(0L, $opt$ByteToLong((byte)0));
    assertEquals(-1L, $opt$ByteToLong((byte)-1));
    assertEquals(51L, $opt$ByteToLong((byte)51));
    assertEquals(-51L, $opt$ByteToLong((byte)-51));
    assertEquals(127L, $opt$ByteToLong((byte)127));  // (2^7) - 1
    assertEquals(-127L, $opt$ByteToLong((byte)-127));  // -(2^7) - 1
    assertEquals(-128L, $opt$ByteToLong((byte)-128));  // -(2^7)
  }

  private static void shortToLong() {
    assertEquals(1L, $opt$ShortToLong((short)1));
    assertEquals(0L, $opt$ShortToLong((short)0));
    assertEquals(-1L, $opt$ShortToLong((short)-1));
    assertEquals(51L, $opt$ShortToLong((short)51));
    assertEquals(-51L, $opt$ShortToLong((short)-51));
    assertEquals(32767L, $opt$ShortToLong((short)32767));  // (2^15) - 1
    assertEquals(-32767L, $opt$ShortToLong((short)-32767));  // -(2^15) - 1
    assertEquals(-32768L, $opt$ShortToLong((short)-32768));  // -(2^15)
  }

  private static void intToLong() {
    assertEquals(1L, $opt$IntToLong(1));
    assertEquals(0L, $opt$IntToLong(0));
    assertEquals(-1L, $opt$IntToLong(-1));
    assertEquals(51L, $opt$IntToLong(51));
    assertEquals(-51L, $opt$IntToLong(-51));
    assertEquals(2147483647L, $opt$IntToLong(2147483647));  // (2^31) - 1
    assertEquals(-2147483647L, $opt$IntToLong(-2147483647));  // -(2^31) - 1
    assertEquals(-2147483648L, $opt$IntToLong(-2147483648));  // -(2^31)
  }

  static long $opt$ByteToLong(byte a) {
    // Translates to an int-to-long Dex instruction.
    return a;
  }

  static long $opt$ShortToLong(short a) {
    // Translates to an int-to-long Dex instruction.
    return a;
  }

  static long $opt$IntToLong(int a) {
    // Translates to an int-to-long Dex instruction.
    return a;
  }
}
