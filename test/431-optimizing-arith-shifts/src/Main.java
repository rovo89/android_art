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

public class Main {

  public static void expectEquals(int expected, int result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  public static void expectEquals(long expected, long result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  public static void main(String[] args) {
    shlInt();
    shlLong();
    shrInt();
    shrLong();
    ushrInt();
    ushrLong();
  }

  private static void shlInt() {
    expectEquals(48, $opt$ShlConst2(12));
    expectEquals(12, $opt$ShlConst0(12));
    expectEquals(-48, $opt$Shl(-12, 2));
    expectEquals(1024, $opt$Shl(32, 5));

    expectEquals(7, $opt$Shl(7, 0));
    expectEquals(14, $opt$Shl(7, 1));
    expectEquals(0, $opt$Shl(0, 30));

    expectEquals(1073741824L, $opt$Shl(1, 30));
    expectEquals(Integer.MIN_VALUE, $opt$Shl(1, 31));  // overflow
    expectEquals(Integer.MIN_VALUE, $opt$Shl(1073741824, 1));  // overflow
    expectEquals(1073741824, $opt$Shl(268435456, 2));

    // Only the 5 lower bits should be used for shifting (& 0x1f).
    expectEquals(7, $opt$Shl(7, 32));  // 32 & 0x1f = 0
    expectEquals(14, $opt$Shl(7, 33));  // 33 & 0x1f = 1
    expectEquals(32, $opt$Shl(1, 101));  // 101 & 0x1f = 5

    expectEquals(Integer.MIN_VALUE, $opt$Shl(1, -1));  // -1 & 0x1f = 31
    expectEquals(14, $opt$Shl(7, -31));  // -31 & 0x1f = 1
    expectEquals(7, $opt$Shl(7, -32));  // -32 & 0x1f = 0
    expectEquals(-536870912, $opt$Shl(7, -3));  // -3 & 0x1f = 29

    expectEquals(Integer.MIN_VALUE, $opt$Shl(7, Integer.MAX_VALUE));
    expectEquals(7, $opt$Shl(7, Integer.MIN_VALUE));
  }

  private static void shlLong() {
    expectEquals(48L, $opt$ShlConst2(12L));
    expectEquals(12L, $opt$ShlConst0(12L));
    expectEquals(-48L, $opt$Shl(-12L, 2L));
    expectEquals(1024L, $opt$Shl(32L, 5L));

    expectEquals(7L, $opt$Shl(7L, 0L));
    expectEquals(14L, $opt$Shl(7L, 1L));
    expectEquals(0L, $opt$Shl(0L, 30L));

    expectEquals(1073741824L, $opt$Shl(1L, 30L));
    expectEquals(2147483648L, $opt$Shl(1L, 31L));
    expectEquals(2147483648L, $opt$Shl(1073741824L, 1L));

    // Long shifts can use up to 6 lower bits.
    expectEquals(4294967296L, $opt$Shl(1L, 32L));
    expectEquals(60129542144L, $opt$Shl(7L, 33L));
    expectEquals(Long.MIN_VALUE, $opt$Shl(1L, 63L));  // overflow

    // Only the 6 lower bits should be used for shifting (& 0x3f).
    expectEquals(7L, $opt$Shl(7L, 64L));  // 64 & 0x3f = 0
    expectEquals(14L, $opt$Shl(7L, 65L));  // 65 & 0x3f = 1
    expectEquals(137438953472L, $opt$Shl(1L, 101L));  // 101 & 0x3f = 37

    expectEquals(Long.MIN_VALUE, $opt$Shl(1L, -1L));  // -1 & 0x3f = 63
    expectEquals(14L, $opt$Shl(7L, -63L));  // -63 & 0x3f = 1
    expectEquals(7L, $opt$Shl(7L, -64L));  // -64 & 0x3f = 0
    expectEquals(2305843009213693952L, $opt$Shl(1L, -3L));  // -3 & 0x3f = 61

    expectEquals(Long.MIN_VALUE, $opt$Shl(7L, Long.MAX_VALUE));
    expectEquals(7L, $opt$Shl(7L, Long.MIN_VALUE));

    // Exercise some special cases handled by backends/simplifier.
    expectEquals(24L, $opt$ShlConst1(12L));
    expectEquals(0x2345678900000000L, $opt$ShlConst32(0x123456789L));
    expectEquals(0x2490249000000000L, $opt$ShlConst33(0x12481248L));
    expectEquals(0x4920492000000000L, $opt$ShlConst34(0x12481248L));
    expectEquals(0x9240924000000000L, $opt$ShlConst35(0x12481248L));
  }

  private static void shrInt() {
    expectEquals(3, $opt$ShrConst2(12));
    expectEquals(12, $opt$ShrConst0(12));
    expectEquals(-3, $opt$Shr(-12, 2));
    expectEquals(1, $opt$Shr(32, 5));

    expectEquals(7, $opt$Shr(7, 0));
    expectEquals(3, $opt$Shr(7, 1));
    expectEquals(0, $opt$Shr(0, 30));
    expectEquals(0, $opt$Shr(1, 30));
    expectEquals(-1, $opt$Shr(-1, 30));

    expectEquals(0, $opt$Shr(Integer.MAX_VALUE, 31));
    expectEquals(-1, $opt$Shr(Integer.MIN_VALUE, 31));

    // Only the 5 lower bits should be used for shifting (& 0x1f).
    expectEquals(7, $opt$Shr(7, 32));  // 32 & 0x1f = 0
    expectEquals(3, $opt$Shr(7, 33));  // 33 & 0x1f = 1

    expectEquals(0, $opt$Shr(1, -1));  // -1 & 0x1f = 31
    expectEquals(3, $opt$Shr(7, -31));  // -31 & 0x1f = 1
    expectEquals(7, $opt$Shr(7, -32));  // -32 & 0x1f = 0
    expectEquals(-4, $opt$Shr(Integer.MIN_VALUE, -3));  // -3 & 0x1f = 29

    expectEquals(0, $opt$Shr(7, Integer.MAX_VALUE));
    expectEquals(7, $opt$Shr(7, Integer.MIN_VALUE));
  }

  private static void shrLong() {
    expectEquals(3L, $opt$ShrConst2(12L));
    expectEquals(12L, $opt$ShrConst0(12L));
    expectEquals(-3L, $opt$Shr(-12L, 2L));
    expectEquals(1, $opt$Shr(32, 5));

    expectEquals(7L, $opt$Shr(7L, 0L));
    expectEquals(3L, $opt$Shr(7L, 1L));
    expectEquals(0L, $opt$Shr(0L, 30L));
    expectEquals(0L, $opt$Shr(1L, 30L));
    expectEquals(-1L, $opt$Shr(-1L, 30L));


    expectEquals(1L, $opt$Shr(1073741824L, 30L));
    expectEquals(1L, $opt$Shr(2147483648L, 31L));
    expectEquals(1073741824L, $opt$Shr(2147483648L, 1L));

    // Long shifts can use up to 6 lower bits.
    expectEquals(1L, $opt$Shr(4294967296L, 32L));
    expectEquals(7L, $opt$Shr(60129542144L, 33L));
    expectEquals(0L, $opt$Shr(Long.MAX_VALUE, 63L));
    expectEquals(-1L, $opt$Shr(Long.MIN_VALUE, 63L));

    // Only the 6 lower bits should be used for shifting (& 0x3f).
    expectEquals(7L, $opt$Shr(7L, 64L));  // 64 & 0x3f = 0
    expectEquals(3L, $opt$Shr(7L, 65L));  // 65 & 0x3f = 1

    expectEquals(-1L, $opt$Shr(Long.MIN_VALUE, -1L));  // -1 & 0x3f = 63
    expectEquals(3L, $opt$Shr(7L, -63L));  // -63 & 0x3f = 1
    expectEquals(7L, $opt$Shr(7L, -64L));  // -64 & 0x3f = 0
    expectEquals(1L, $opt$Shr(2305843009213693952L, -3L));  // -3 & 0x3f = 61
    expectEquals(-4L, $opt$Shr(Integer.MIN_VALUE, -3));  // -3 & 0x1f = 29

    expectEquals(0L, $opt$Shr(7L, Long.MAX_VALUE));
    expectEquals(7L, $opt$Shr(7L, Long.MIN_VALUE));
  }

  private static void ushrInt() {
    expectEquals(3, $opt$UShrConst2(12));
    expectEquals(12, $opt$UShrConst0(12));
    expectEquals(1073741821, $opt$UShr(-12, 2));
    expectEquals(1, $opt$UShr(32, 5));

    expectEquals(7, $opt$UShr(7, 0));
    expectEquals(3, $opt$UShr(7, 1));
    expectEquals(0, $opt$UShr(0, 30));
    expectEquals(0, $opt$UShr(1, 30));
    expectEquals(3, $opt$UShr(-1, 30));

    expectEquals(0, $opt$UShr(Integer.MAX_VALUE, 31));
    expectEquals(1, $opt$UShr(Integer.MIN_VALUE, 31));

    // Only the 5 lower bits should be used for shifting (& 0x1f).
    expectEquals(7, $opt$UShr(7, 32));  // 32 & 0x1f = 0
    expectEquals(3, $opt$UShr(7, 33));  // 33 & 0x1f = 1

    expectEquals(0, $opt$UShr(1, -1));  // -1 & 0x1f = 31
    expectEquals(3, $opt$UShr(7, -31));  // -31 & 0x1f = 1
    expectEquals(7, $opt$UShr(7, -32));  // -32 & 0x1f = 0
    expectEquals(4, $opt$UShr(Integer.MIN_VALUE, -3));  // -3 & 0x1f = 29

    expectEquals(0, $opt$UShr(7, Integer.MAX_VALUE));
    expectEquals(7, $opt$UShr(7, Integer.MIN_VALUE));
  }

  private static void ushrLong() {
    expectEquals(3L, $opt$UShrConst2(12L));
    expectEquals(12L, $opt$UShrConst0(12L));
    expectEquals(4611686018427387901L, $opt$UShr(-12L, 2L));
    expectEquals(1, $opt$UShr(32, 5));

    expectEquals(7L, $opt$UShr(7L, 0L));
    expectEquals(3L, $opt$UShr(7L, 1L));
    expectEquals(0L, $opt$UShr(0L, 30L));
    expectEquals(0L, $opt$UShr(1L, 30L));
    expectEquals(17179869183L, $opt$UShr(-1L, 30L));


    expectEquals(1L, $opt$UShr(1073741824L, 30L));
    expectEquals(1L, $opt$UShr(2147483648L, 31L));
    expectEquals(1073741824L, $opt$UShr(2147483648L, 1L));

    // Long shifts can use use up to 6 lower bits.
    expectEquals(1L, $opt$UShr(4294967296L, 32L));
    expectEquals(7L, $opt$UShr(60129542144L, 33L));
    expectEquals(0L, $opt$UShr(Long.MAX_VALUE, 63L));
    expectEquals(1L, $opt$UShr(Long.MIN_VALUE, 63L));

    // Only the 6 lower bits should be used for shifting (& 0x3f).
    expectEquals(7L, $opt$UShr(7L, 64L));  // 64 & 0x3f = 0
    expectEquals(3L, $opt$UShr(7L, 65L));  // 65 & 0x3f = 1

    expectEquals(1L, $opt$UShr(Long.MIN_VALUE, -1L));  // -1 & 0x3f = 63
    expectEquals(3L, $opt$UShr(7L, -63L));  // -63 & 0x3f = 1
    expectEquals(7L, $opt$UShr(7L, -64L));  // -64 & 0x3f = 0
    expectEquals(1L, $opt$UShr(2305843009213693952L, -3L));  // -3 & 0x3f = 61
    expectEquals(4L, $opt$UShr(Long.MIN_VALUE, -3L));  // -3 & 0x3f = 61

    expectEquals(0L, $opt$UShr(7L, Long.MAX_VALUE));
    expectEquals(7L, $opt$UShr(7L, Long.MIN_VALUE));
  }

  static int $opt$Shl(int a, int b) {
    return a << b;
  }

  static long $opt$Shl(long a, long b) {
    return a << b;
  }

  static int $opt$Shr(int a, int b) {
    return a >> b;
  }

  static long $opt$Shr(long a, long b) {
    return a >> b;
  }

  static int $opt$UShr(int a, int b) {
    return a >>> b;
  }

  static long $opt$UShr(long a, long b) {
    return a >>> b;
  }

  static int $opt$ShlConst2(int a) {
    return a << 2;
  }

  static long $opt$ShlConst2(long a) {
    return a << 2L;
  }

  static int $opt$ShrConst2(int a) {
    return a >> 2;
  }

  static long $opt$ShrConst2(long a) {
    return a >> 2L;
  }

  static int $opt$UShrConst2(int a) {
    return a >>> 2;
  }

  static long $opt$UShrConst2(long a) {
    return a >>> 2L;
  }

  static int $opt$ShlConst0(int a) {
    return a << 0;
  }

  static long $opt$ShlConst0(long a) {
    return a << 0L;
  }

  static int $opt$ShrConst0(int a) {
    return a >> 0;
  }

  static long $opt$ShrConst0(long a) {
    return a >> 0L;
  }

  static int $opt$UShrConst0(int a) {
    return a >>> 0;
  }

  static long $opt$UShrConst0(long a) {
    return a >>> 0L;
  }

  static long $opt$ShlConst1(long a) {
    return a << 1L;
  }

  static long $opt$ShlConst32(long a) {
    return a << 32L;
  }

  static long $opt$ShlConst33(long a) {
    return a << 33L;
  }

  static long $opt$ShlConst34(long a) {
    return a << 34L;
  }

  static long $opt$ShlConst35(long a) {
    return a << 35L;
  }

}

