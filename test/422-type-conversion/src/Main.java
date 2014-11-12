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

  public static void assertIntEquals(int expected, int result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  public static void assertLongEquals(long expected, long result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  public static void main(String[] args) {
    byteToLong();
    shortToLong();
    intToLong();
    charToLong();

    longToInt();
  }

  private static void byteToLong() {
    assertLongEquals(1L, $opt$ByteToLong((byte)1));
    assertLongEquals(0L, $opt$ByteToLong((byte)0));
    assertLongEquals(-1L, $opt$ByteToLong((byte)-1));
    assertLongEquals(51L, $opt$ByteToLong((byte)51));
    assertLongEquals(-51L, $opt$ByteToLong((byte)-51));
    assertLongEquals(127L, $opt$ByteToLong((byte)127));  // 2^7 - 1
    assertLongEquals(-127L, $opt$ByteToLong((byte)-127));  // -(2^7 - 1)
    assertLongEquals(-128L, $opt$ByteToLong((byte)-128));  // -(2^7)
  }

  private static void shortToLong() {
    assertLongEquals(1L, $opt$ShortToLong((short)1));
    assertLongEquals(0L, $opt$ShortToLong((short)0));
    assertLongEquals(-1L, $opt$ShortToLong((short)-1));
    assertLongEquals(51L, $opt$ShortToLong((short)51));
    assertLongEquals(-51L, $opt$ShortToLong((short)-51));
    assertLongEquals(32767L, $opt$ShortToLong((short)32767));  // 2^15 - 1
    assertLongEquals(-32767L, $opt$ShortToLong((short)-32767));  // -(2^15 - 1)
    assertLongEquals(-32768L, $opt$ShortToLong((short)-32768));  // -(2^15)
  }

  private static void intToLong() {
    assertLongEquals(1L, $opt$IntToLong(1));
    assertLongEquals(0L, $opt$IntToLong(0));
    assertLongEquals(-1L, $opt$IntToLong(-1));
    assertLongEquals(51L, $opt$IntToLong(51));
    assertLongEquals(-51L, $opt$IntToLong(-51));
    assertLongEquals(2147483647L, $opt$IntToLong(2147483647));  // 2^31 - 1
    assertLongEquals(-2147483647L, $opt$IntToLong(-2147483647));  // -(2^31 - 1)
    assertLongEquals(-2147483648L, $opt$IntToLong(-2147483648));  // -(2^31)
  }

  private static void charToLong() {
    assertLongEquals(1L, $opt$CharToLong((char)1));
    assertLongEquals(0L, $opt$CharToLong((char)0));
    assertLongEquals(51L, $opt$CharToLong((char)51));
    assertLongEquals(32767L, $opt$CharToLong((char)32767));  // 2^15 - 1
    assertLongEquals(65535L, $opt$CharToLong((char)65535));  // 2^16 - 1

    assertLongEquals(0L, $opt$CharToLong('\u0000'));
    assertLongEquals(65535L, $opt$CharToLong('\uFFFF'));  // 2^16 - 1

    assertLongEquals(65535L, $opt$CharToLong((char)-1));
    assertLongEquals(65485L, $opt$CharToLong((char)-51));
    assertLongEquals(32769L, $opt$CharToLong((char)-32767));  // -(2^15 - 1)
    assertLongEquals(32768L, $opt$CharToLong((char)-32768));  // -(2^15)
  }

  private static void longToInt() {
    assertIntEquals(1, $opt$LongToInt(1L));
    assertIntEquals(0, $opt$LongToInt(0L));
    assertIntEquals(-1, $opt$LongToInt(-1L));
    assertIntEquals(51, $opt$LongToInt(51L));
    assertIntEquals(-51, $opt$LongToInt(-51L));
    assertIntEquals(2147483647, $opt$LongToInt(2147483647L));  // 2^31 - 1
    assertIntEquals(-2147483647, $opt$LongToInt(-2147483647L));  // -(2^31 - 1)
    assertIntEquals(-2147483648, $opt$LongToInt(-2147483648L));  // -(2^31)
    assertIntEquals(-2147483648, $opt$LongToInt(2147483648L));  // (2^31)
    assertIntEquals(2147483647, $opt$LongToInt(-2147483649L));  // -(2^31 + 1)
    assertIntEquals(-1, $opt$LongToInt(9223372036854775807L));  // 2^63 - 1
    assertIntEquals(1, $opt$LongToInt(-9223372036854775807L));  // -(2^63 - 1)
    assertIntEquals(0, $opt$LongToInt(-9223372036854775808L));  // -(2^63)

    assertIntEquals(42, $opt$LongLiteralToInt());

    // Ensure long-to-int conversions truncates values as expected.
    assertLongEquals(1L, $opt$IntToLong($opt$LongToInt(4294967297L)));  // 2^32 + 1
    assertLongEquals(0L, $opt$IntToLong($opt$LongToInt(4294967296L)));  // 2^32
    assertLongEquals(-1L, $opt$IntToLong($opt$LongToInt(4294967295L)));  // 2^32 - 1
    assertLongEquals(0L, $opt$IntToLong($opt$LongToInt(0L)));
    assertLongEquals(1L, $opt$IntToLong($opt$LongToInt(-4294967295L)));  // -(2^32 - 1)
    assertLongEquals(0L, $opt$IntToLong($opt$LongToInt(-4294967296L)));  // -(2^32)
    assertLongEquals(-1, $opt$IntToLong($opt$LongToInt(-4294967297L)));  // -(2^32 + 1)
  }

  // These methods produce int-to-long Dex instructions.
  static long $opt$ByteToLong(byte a) { return a; }
  static long $opt$ShortToLong(short a) { return a; }
  static long $opt$IntToLong(int a) { return a; }
  static long $opt$CharToLong(int a) { return a; }

  // These methods produce long-to-int Dex instructions.
  static int $opt$LongToInt(long a){ return (int)a; }
  static int $opt$LongLiteralToInt(){ return (int)42L; }
}
