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

  public static void assertByteEquals(byte expected, byte result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  public static void assertShortEquals(short expected, short result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

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

  public static void assertCharEquals(char expected, char result) {
    if (expected != result) {
      // Values are cast to int to display numeric values instead of
      // (UTF-16 encoded) characters.
      throw new Error("Expected: " + (int)expected + ", found: " + (int)result);
    }
  }

  public static void assertFloatEquals(float expected, float result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  public static void assertDoubleEquals(double expected, double result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  public static void assertFloatIsNaN(float result) {
    if (!Float.isNaN(result)) {
      throw new Error("Expected: NaN, found: " + result);
    }
  }

  public static void assertDoubleIsNaN(double result) {
    if (!Double.isNaN(result)) {
      throw new Error("Expected: NaN, found: " + result);
    }
  }


  public static void main(String[] args) {
    // Generate, compile and check int-to-long Dex instructions.
    byteToLong();
    shortToLong();
    intToLong();
    charToLong();

    // Generate, compile and check int-to-float Dex instructions.
    byteToFloat();
    shortToFloat();
    intToFloat();
    charToFloat();

    // Generate, compile and check int-to-double Dex instructions.
    byteToDouble();
    shortToDouble();
    intToDouble();
    charToDouble();

    // Generate, compile and check long-to-int Dex instructions.
    longToInt();

    // Generate, compile and check long-to-float Dex instructions.
    longToFloat();

    // Generate, compile and check long-to-double Dex instructions.
    longToDouble();

    // Generate, compile and check float-to-int Dex instructions.
    floatToInt();

    // Generate, compile and check float-to-long Dex instructions.
    floatToLong();

    // Generate, compile and check float-to-double Dex instructions.
    floatToDouble();

    // Generate, compile and check double-to-int Dex instructions.
    doubleToInt();

    // Generate, compile and check double-to-long Dex instructions.
    doubleToLong();

    // Generate, compile and check double-to-float Dex instructions.
    doubleToFloat();

    // Generate, compile and check int-to-byte Dex instructions.
    shortToByte();
    intToByte();
    charToByte();

    // Generate, compile and check int-to-short Dex instructions.
    byteToShort();
    intToShort();
    charToShort();

    // Generate, compile and check int-to-char Dex instructions.
    byteToChar();
    shortToChar();
    intToChar();
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
    assertLongEquals(65535L, $opt$CharToLong((char)-1));
    assertLongEquals(65485L, $opt$CharToLong((char)-51));
    assertLongEquals(32769L, $opt$CharToLong((char)-32767));  // -(2^15 - 1)
    assertLongEquals(32768L, $opt$CharToLong((char)-32768));  // -(2^15)
  }

  private static void byteToFloat() {
    assertFloatEquals(1F, $opt$ByteToFloat((byte)1));
    assertFloatEquals(0F, $opt$ByteToFloat((byte)0));
    assertFloatEquals(-1F, $opt$ByteToFloat((byte)-1));
    assertFloatEquals(51F, $opt$ByteToFloat((byte)51));
    assertFloatEquals(-51F, $opt$ByteToFloat((byte)-51));
    assertFloatEquals(127F, $opt$ByteToFloat((byte)127));  // 2^7 - 1
    assertFloatEquals(-127F, $opt$ByteToFloat((byte)-127));  // -(2^7 - 1)
    assertFloatEquals(-128F, $opt$ByteToFloat((byte)-128));  // -(2^7)
  }

  private static void shortToFloat() {
    assertFloatEquals(1F, $opt$ShortToFloat((short)1));
    assertFloatEquals(0F, $opt$ShortToFloat((short)0));
    assertFloatEquals(-1F, $opt$ShortToFloat((short)-1));
    assertFloatEquals(51F, $opt$ShortToFloat((short)51));
    assertFloatEquals(-51F, $opt$ShortToFloat((short)-51));
    assertFloatEquals(32767F, $opt$ShortToFloat((short)32767));  // 2^15 - 1
    assertFloatEquals(-32767F, $opt$ShortToFloat((short)-32767));  // -(2^15 - 1)
    assertFloatEquals(-32768F, $opt$ShortToFloat((short)-32768));  // -(2^15)
  }

  private static void intToFloat() {
    assertFloatEquals(1F, $opt$IntToFloat(1));
    assertFloatEquals(0F, $opt$IntToFloat(0));
    assertFloatEquals(-1F, $opt$IntToFloat(-1));
    assertFloatEquals(51F, $opt$IntToFloat(51));
    assertFloatEquals(-51F, $opt$IntToFloat(-51));
    assertFloatEquals(16777215F, $opt$IntToFloat(16777215));  // 2^24 - 1
    assertFloatEquals(-16777215F, $opt$IntToFloat(-16777215));  // -(2^24 - 1)
    assertFloatEquals(16777216F, $opt$IntToFloat(16777216));  // 2^24
    assertFloatEquals(-16777216F, $opt$IntToFloat(-16777216));  // -(2^24)
    assertFloatEquals(2147483647F, $opt$IntToFloat(2147483647));  // 2^31 - 1
    assertFloatEquals(-2147483648F, $opt$IntToFloat(-2147483648));  // -(2^31)
  }

  private static void charToFloat() {
    assertFloatEquals(1F, $opt$CharToFloat((char)1));
    assertFloatEquals(0F, $opt$CharToFloat((char)0));
    assertFloatEquals(51F, $opt$CharToFloat((char)51));
    assertFloatEquals(32767F, $opt$CharToFloat((char)32767));  // 2^15 - 1
    assertFloatEquals(65535F, $opt$CharToFloat((char)65535));  // 2^16 - 1
    assertFloatEquals(65535F, $opt$CharToFloat((char)-1));
    assertFloatEquals(65485F, $opt$CharToFloat((char)-51));
    assertFloatEquals(32769F, $opt$CharToFloat((char)-32767));  // -(2^15 - 1)
    assertFloatEquals(32768F, $opt$CharToFloat((char)-32768));  // -(2^15)
  }

  private static void byteToDouble() {
    assertDoubleEquals(1D, $opt$ByteToDouble((byte)1));
    assertDoubleEquals(0D, $opt$ByteToDouble((byte)0));
    assertDoubleEquals(-1D, $opt$ByteToDouble((byte)-1));
    assertDoubleEquals(51D, $opt$ByteToDouble((byte)51));
    assertDoubleEquals(-51D, $opt$ByteToDouble((byte)-51));
    assertDoubleEquals(127D, $opt$ByteToDouble((byte)127));  // 2^7 - 1
    assertDoubleEquals(-127D, $opt$ByteToDouble((byte)-127));  // -(2^7 - 1)
    assertDoubleEquals(-128D, $opt$ByteToDouble((byte)-128));  // -(2^7)
  }

  private static void shortToDouble() {
    assertDoubleEquals(1D, $opt$ShortToDouble((short)1));
    assertDoubleEquals(0D, $opt$ShortToDouble((short)0));
    assertDoubleEquals(-1D, $opt$ShortToDouble((short)-1));
    assertDoubleEquals(51D, $opt$ShortToDouble((short)51));
    assertDoubleEquals(-51D, $opt$ShortToDouble((short)-51));
    assertDoubleEquals(32767D, $opt$ShortToDouble((short)32767));  // 2^15 - 1
    assertDoubleEquals(-32767D, $opt$ShortToDouble((short)-32767));  // -(2^15 - 1)
    assertDoubleEquals(-32768D, $opt$ShortToDouble((short)-32768));  // -(2^15)
  }

  private static void intToDouble() {
    assertDoubleEquals(1D, $opt$IntToDouble(1));
    assertDoubleEquals(0D, $opt$IntToDouble(0));
    assertDoubleEquals(-1D, $opt$IntToDouble(-1));
    assertDoubleEquals(51D, $opt$IntToDouble(51));
    assertDoubleEquals(-51D, $opt$IntToDouble(-51));
    assertDoubleEquals(16777216D, $opt$IntToDouble(16777216));  // 2^24
    assertDoubleEquals(-16777216D, $opt$IntToDouble(-16777216));  // -(2^24)
    assertDoubleEquals(2147483647D, $opt$IntToDouble(2147483647));  // 2^31 - 1
    assertDoubleEquals(-2147483648D, $opt$IntToDouble(-2147483648));  // -(2^31)
  }

  private static void charToDouble() {
    assertDoubleEquals(1D, $opt$CharToDouble((char)1));
    assertDoubleEquals(0D, $opt$CharToDouble((char)0));
    assertDoubleEquals(51D, $opt$CharToDouble((char)51));
    assertDoubleEquals(32767D, $opt$CharToDouble((char)32767));  // 2^15 - 1
    assertDoubleEquals(65535D, $opt$CharToDouble((char)65535));  // 2^16 - 1
    assertDoubleEquals(65535D, $opt$CharToDouble((char)-1));
    assertDoubleEquals(65485D, $opt$CharToDouble((char)-51));
    assertDoubleEquals(32769D, $opt$CharToDouble((char)-32767));  // -(2^15 - 1)
    assertDoubleEquals(32768D, $opt$CharToDouble((char)-32768));  // -(2^15)
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

  private static void longToFloat() {
    assertFloatEquals(1F, $opt$LongToFloat(1L));
    assertFloatEquals(0F, $opt$LongToFloat(0L));
    assertFloatEquals(-1F, $opt$LongToFloat(-1L));
    assertFloatEquals(51F, $opt$LongToFloat(51L));
    assertFloatEquals(-51F, $opt$LongToFloat(-51L));
    assertFloatEquals(2147483647F, $opt$LongToFloat(2147483647L));  // 2^31 - 1
    assertFloatEquals(-2147483647F, $opt$LongToFloat(-2147483647L));  // -(2^31 - 1)
    assertFloatEquals(-2147483648F, $opt$LongToFloat(-2147483648L));  // -(2^31)
    assertFloatEquals(2147483648F, $opt$LongToFloat(2147483648L));  // (2^31)
    assertFloatEquals(-2147483649F, $opt$LongToFloat(-2147483649L));  // -(2^31 + 1)
    assertFloatEquals(4294967296F, $opt$LongToFloat(4294967296L));  // (2^32)
    assertFloatEquals(-4294967296F, $opt$LongToFloat(-4294967296L));  // -(2^32)
    assertFloatEquals(140739635871745F, $opt$LongToFloat(140739635871745L));  // 1 + 2^15 + 2^31 + 2^47
    assertFloatEquals(-140739635871745F, $opt$LongToFloat(-140739635871745L));  // -(1 + 2^15 + 2^31 + 2^47)
    assertFloatEquals(9223372036854775807F, $opt$LongToFloat(9223372036854775807L));  // 2^63 - 1
    assertFloatEquals(-9223372036854775807F, $opt$LongToFloat(-9223372036854775807L));  // -(2^63 - 1)
    assertFloatEquals(-9223372036854775808F, $opt$LongToFloat(-9223372036854775808L));  // -(2^63)
  }

  private static void longToDouble() {
    assertDoubleEquals(1D, $opt$LongToDouble(1L));
    assertDoubleEquals(0D, $opt$LongToDouble(0L));
    assertDoubleEquals(-1D, $opt$LongToDouble(-1L));
    assertDoubleEquals(51D, $opt$LongToDouble(51L));
    assertDoubleEquals(-51D, $opt$LongToDouble(-51L));
    assertDoubleEquals(2147483647D, $opt$LongToDouble(2147483647L));  // 2^31 - 1
    assertDoubleEquals(-2147483647D, $opt$LongToDouble(-2147483647L));  // -(2^31 - 1)
    assertDoubleEquals(-2147483648D, $opt$LongToDouble(-2147483648L));  // -(2^31)
    assertDoubleEquals(2147483648D, $opt$LongToDouble(2147483648L));  // (2^31)
    assertDoubleEquals(-2147483649D, $opt$LongToDouble(-2147483649L));  // -(2^31 + 1)
    assertDoubleEquals(4294967296D, $opt$LongToDouble(4294967296L));  // (2^32)
    assertDoubleEquals(-4294967296D, $opt$LongToDouble(-4294967296L));  // -(2^32)
    assertDoubleEquals(140739635871745D, $opt$LongToDouble(140739635871745L));  // 1 + 2^15 + 2^31 + 2^47
    assertDoubleEquals(-140739635871745D, $opt$LongToDouble(-140739635871745L));  // -(1 + 2^15 + 2^31 + 2^47)
    assertDoubleEquals(9223372036854775807D, $opt$LongToDouble(9223372036854775807L));  // 2^63 - 1
    assertDoubleEquals(-9223372036854775807D, $opt$LongToDouble(-9223372036854775807L));  // -(2^63 - 1)
    assertDoubleEquals(-9223372036854775808D, $opt$LongToDouble(-9223372036854775808L));  // -(2^63)
  }

  private static void floatToInt() {
    assertIntEquals(1, $opt$FloatToInt(1F));
    assertIntEquals(0, $opt$FloatToInt(0F));
    assertIntEquals(0, $opt$FloatToInt(-0F));
    assertIntEquals(-1, $opt$FloatToInt(-1F));
    assertIntEquals(51, $opt$FloatToInt(51F));
    assertIntEquals(-51, $opt$FloatToInt(-51F));
    assertIntEquals(0, $opt$FloatToInt(0.5F));
    assertIntEquals(0, $opt$FloatToInt(0.4999999F));
    assertIntEquals(0, $opt$FloatToInt(-0.4999999F));
    assertIntEquals(0, $opt$FloatToInt(-0.5F));
    assertIntEquals(42, $opt$FloatToInt(42.199F));
    assertIntEquals(-42, $opt$FloatToInt(-42.199F));
    assertIntEquals(2147483647, $opt$FloatToInt(2147483647F));  // 2^31 - 1
    assertIntEquals(-2147483648, $opt$FloatToInt(-2147483647F));  // -(2^31 - 1)
    assertIntEquals(-2147483648, $opt$FloatToInt(-2147483648F));  // -(2^31)
    assertIntEquals(2147483647, $opt$FloatToInt(2147483648F));  // (2^31)
    assertIntEquals(-2147483648, $opt$FloatToInt(-2147483649F));  // -(2^31 + 1)
    assertIntEquals(2147483647, $opt$FloatToInt(9223372036854775807F));  // 2^63 - 1
    assertIntEquals(-2147483648, $opt$FloatToInt(-9223372036854775807F));  // -(2^63 - 1)
    assertIntEquals(-2147483648, $opt$FloatToInt(-9223372036854775808F));  // -(2^63)
    assertIntEquals(0, $opt$FloatToInt(Float.NaN));
    assertIntEquals(2147483647, $opt$FloatToInt(Float.POSITIVE_INFINITY));
    assertIntEquals(-2147483648, $opt$FloatToInt(Float.NEGATIVE_INFINITY));
  }

  private static void floatToLong() {
    assertLongEquals(1L, $opt$FloatToLong(1F));
    assertLongEquals(0L, $opt$FloatToLong(0F));
    assertLongEquals(0L, $opt$FloatToLong(-0F));
    assertLongEquals(-1L, $opt$FloatToLong(-1F));
    assertLongEquals(51L, $opt$FloatToLong(51F));
    assertLongEquals(-51L, $opt$FloatToLong(-51F));
    assertLongEquals(0L, $opt$FloatToLong(0.5F));
    assertLongEquals(0L, $opt$FloatToLong(0.4999999F));
    assertLongEquals(0L, $opt$FloatToLong(-0.4999999F));
    assertLongEquals(0L, $opt$FloatToLong(-0.5F));
    assertLongEquals(42L, $opt$FloatToLong(42.199F));
    assertLongEquals(-42L, $opt$FloatToLong(-42.199F));
    assertLongEquals(2147483648L, $opt$FloatToLong(2147483647F));  // 2^31 - 1
    assertLongEquals(-2147483648L, $opt$FloatToLong(-2147483647F));  // -(2^31 - 1)
    assertLongEquals(-2147483648L, $opt$FloatToLong(-2147483648F));  // -(2^31)
    assertLongEquals(2147483648L, $opt$FloatToLong(2147483648F));  // (2^31)
    assertLongEquals(-2147483648L, $opt$FloatToLong(-2147483649F));  // -(2^31 + 1)
    assertLongEquals(9223372036854775807L, $opt$FloatToLong(9223372036854775807F));  // 2^63 - 1
    assertLongEquals(-9223372036854775808L, $opt$FloatToLong(-9223372036854775807F));  // -(2^63 - 1)
    assertLongEquals(-9223372036854775808L, $opt$FloatToLong(-9223372036854775808F));  // -(2^63)
    assertLongEquals(0L, $opt$FloatToLong(Float.NaN));
    assertLongEquals(9223372036854775807L, $opt$FloatToLong(Float.POSITIVE_INFINITY));
    assertLongEquals(-9223372036854775808L, $opt$FloatToLong(Float.NEGATIVE_INFINITY));
  }

  private static void floatToDouble() {
    assertDoubleEquals(1D, $opt$FloatToDouble(1F));
    assertDoubleEquals(0D, $opt$FloatToDouble(0F));
    assertDoubleEquals(0D, $opt$FloatToDouble(-0F));
    assertDoubleEquals(-1D, $opt$FloatToDouble(-1F));
    assertDoubleEquals(51D, $opt$FloatToDouble(51F));
    assertDoubleEquals(-51D, $opt$FloatToDouble(-51F));
    assertDoubleEquals(0.5D, $opt$FloatToDouble(0.5F));
    assertDoubleEquals(0.49999991059303284D, $opt$FloatToDouble(0.4999999F));
    assertDoubleEquals(-0.49999991059303284D, $opt$FloatToDouble(-0.4999999F));
    assertDoubleEquals(-0.5D, $opt$FloatToDouble(-0.5F));
    assertDoubleEquals(42.19900131225586D, $opt$FloatToDouble(42.199F));
    assertDoubleEquals(-42.19900131225586D, $opt$FloatToDouble(-42.199F));
    assertDoubleEquals(2147483648D, $opt$FloatToDouble(2147483647F));  // 2^31 - 1
    assertDoubleEquals(-2147483648D, $opt$FloatToDouble(-2147483647F));  // -(2^31 - 1)
    assertDoubleEquals(-2147483648D, $opt$FloatToDouble(-2147483648F));  // -(2^31)
    assertDoubleEquals(2147483648D, $opt$FloatToDouble(2147483648F));  // (2^31)
    assertDoubleEquals(-2147483648D, $opt$FloatToDouble(-2147483649F));  // -(2^31 + 1)
    assertDoubleEquals(9223372036854775807D, $opt$FloatToDouble(9223372036854775807F));  // 2^63 - 1
    assertDoubleEquals(-9223372036854775807D, $opt$FloatToDouble(-9223372036854775807F));  // -(2^63 - 1)
    assertDoubleEquals(-9223372036854775808D, $opt$FloatToDouble(-9223372036854775808F));  // -(2^63)
    assertDoubleIsNaN($opt$FloatToDouble(Float.NaN));
    assertDoubleEquals(Double.POSITIVE_INFINITY, $opt$FloatToDouble(Float.POSITIVE_INFINITY));
    assertDoubleEquals(Double.NEGATIVE_INFINITY, $opt$FloatToDouble(Float.NEGATIVE_INFINITY));
  }

  private static void doubleToInt() {
    assertIntEquals(1, $opt$DoubleToInt(1D));
    assertIntEquals(0, $opt$DoubleToInt(0D));
    assertIntEquals(0, $opt$DoubleToInt(-0D));
    assertIntEquals(-1, $opt$DoubleToInt(-1D));
    assertIntEquals(51, $opt$DoubleToInt(51D));
    assertIntEquals(-51, $opt$DoubleToInt(-51D));
    assertIntEquals(0, $opt$DoubleToInt(0.5D));
    assertIntEquals(0, $opt$DoubleToInt(0.4999999D));
    assertIntEquals(0, $opt$DoubleToInt(-0.4999999D));
    assertIntEquals(0, $opt$DoubleToInt(-0.5D));
    assertIntEquals(42, $opt$DoubleToInt(42.199D));
    assertIntEquals(-42, $opt$DoubleToInt(-42.199D));
    assertIntEquals(2147483647, $opt$DoubleToInt(2147483647D));  // 2^31 - 1
    assertIntEquals(-2147483647, $opt$DoubleToInt(-2147483647D));  // -(2^31 - 1)
    assertIntEquals(-2147483648, $opt$DoubleToInt(-2147483648D));  // -(2^31)
    assertIntEquals(2147483647, $opt$DoubleToInt(2147483648D));  // (2^31)
    assertIntEquals(-2147483648, $opt$DoubleToInt(-2147483649D));  // -(2^31 + 1)
    assertIntEquals(2147483647, $opt$DoubleToInt(9223372036854775807D));  // 2^63 - 1
    assertIntEquals(-2147483648, $opt$DoubleToInt(-9223372036854775807D));  // -(2^63 - 1)
    assertIntEquals(-2147483648, $opt$DoubleToInt(-9223372036854775808D));  // -(2^63)
    assertIntEquals(0, $opt$DoubleToInt(Double.NaN));
    assertIntEquals(2147483647, $opt$DoubleToInt(Double.POSITIVE_INFINITY));
    assertIntEquals(-2147483648, $opt$DoubleToInt(Double.NEGATIVE_INFINITY));
  }

  private static void doubleToLong() {
    assertLongEquals(1L, $opt$DoubleToLong(1D));
    assertLongEquals(0L, $opt$DoubleToLong(0D));
    assertLongEquals(0L, $opt$DoubleToLong(-0D));
    assertLongEquals(-1L, $opt$DoubleToLong(-1D));
    assertLongEquals(51L, $opt$DoubleToLong(51D));
    assertLongEquals(-51L, $opt$DoubleToLong(-51D));
    assertLongEquals(0L, $opt$DoubleToLong(0.5D));
    assertLongEquals(0L, $opt$DoubleToLong(0.4999999D));
    assertLongEquals(0L, $opt$DoubleToLong(-0.4999999D));
    assertLongEquals(0L, $opt$DoubleToLong(-0.5D));
    assertLongEquals(42L, $opt$DoubleToLong(42.199D));
    assertLongEquals(-42L, $opt$DoubleToLong(-42.199D));
    assertLongEquals(2147483647L, $opt$DoubleToLong(2147483647D));  // 2^31 - 1
    assertLongEquals(-2147483647L, $opt$DoubleToLong(-2147483647D));  // -(2^31 - 1)
    assertLongEquals(-2147483648L, $opt$DoubleToLong(-2147483648D));  // -(2^31)
    assertLongEquals(2147483648L, $opt$DoubleToLong(2147483648D));  // (2^31)
    assertLongEquals(-2147483649L, $opt$DoubleToLong(-2147483649D));  // -(2^31 + 1)
    assertLongEquals(9223372036854775807L, $opt$DoubleToLong(9223372036854775807D));  // 2^63 - 1
    assertLongEquals(-9223372036854775808L, $opt$DoubleToLong(-9223372036854775807D));  // -(2^63 - 1)
    assertLongEquals(-9223372036854775808L, $opt$DoubleToLong(-9223372036854775808D));  // -(2^63)
    assertLongEquals(0L, $opt$DoubleToLong(Double.NaN));
    assertLongEquals(9223372036854775807L, $opt$DoubleToLong(Double.POSITIVE_INFINITY));
    assertLongEquals(-9223372036854775808L, $opt$DoubleToLong(Double.NEGATIVE_INFINITY));
  }

  private static void doubleToFloat() {
    assertFloatEquals(1F, $opt$DoubleToFloat(1D));
    assertFloatEquals(0F, $opt$DoubleToFloat(0D));
    assertFloatEquals(0F, $opt$DoubleToFloat(-0D));
    assertFloatEquals(-1F, $opt$DoubleToFloat(-1D));
    assertFloatEquals(51F, $opt$DoubleToFloat(51D));
    assertFloatEquals(-51F, $opt$DoubleToFloat(-51D));
    assertFloatEquals(0.5F, $opt$DoubleToFloat(0.5D));
    assertFloatEquals(0.4999999F, $opt$DoubleToFloat(0.4999999D));
    assertFloatEquals(-0.4999999F, $opt$DoubleToFloat(-0.4999999D));
    assertFloatEquals(-0.5F, $opt$DoubleToFloat(-0.5D));
    assertFloatEquals(42.199F, $opt$DoubleToFloat(42.199D));
    assertFloatEquals(-42.199F, $opt$DoubleToFloat(-42.199D));
    assertFloatEquals(2147483648F, $opt$DoubleToFloat(2147483647D));  // 2^31 - 1
    assertFloatEquals(-2147483648F, $opt$DoubleToFloat(-2147483647D));  // -(2^31 - 1)
    assertFloatEquals(-2147483648F, $opt$DoubleToFloat(-2147483648D));  // -(2^31)
    assertFloatEquals(2147483648F, $opt$DoubleToFloat(2147483648D));  // (2^31)
    assertFloatEquals(-2147483648F, $opt$DoubleToFloat(-2147483649D));  // -(2^31 + 1)
    assertFloatEquals(9223372036854775807F, $opt$DoubleToFloat(9223372036854775807D));  // 2^63 - 1
    assertFloatEquals(-9223372036854775807F, $opt$DoubleToFloat(-9223372036854775807D));  // -(2^63 - 1)
    assertFloatEquals(-9223372036854775808F, $opt$DoubleToFloat(-9223372036854775808D));  // -(2^63)
    assertFloatIsNaN($opt$DoubleToFloat(Float.NaN));
    assertFloatEquals(Float.POSITIVE_INFINITY, $opt$DoubleToFloat(Double.POSITIVE_INFINITY));
    assertFloatEquals(Float.NEGATIVE_INFINITY, $opt$DoubleToFloat(Double.NEGATIVE_INFINITY));
  }

  private static void shortToByte() {
    assertByteEquals((byte)1, $opt$ShortToByte((short)1));
    assertByteEquals((byte)0, $opt$ShortToByte((short)0));
    assertByteEquals((byte)-1, $opt$ShortToByte((short)-1));
    assertByteEquals((byte)51, $opt$ShortToByte((short)51));
    assertByteEquals((byte)-51, $opt$ShortToByte((short)-51));
    assertByteEquals((byte)127, $opt$ShortToByte((short)127));  // 2^7 - 1
    assertByteEquals((byte)-127, $opt$ShortToByte((short)-127));  // -(2^7 - 1)
    assertByteEquals((byte)-128, $opt$ShortToByte((short)-128));  // -(2^7)
    assertByteEquals((byte)-128, $opt$ShortToByte((short)128));  // 2^7
    assertByteEquals((byte)127, $opt$ShortToByte((short)-129));  // -(2^7 + 1)
    assertByteEquals((byte)-1, $opt$ShortToByte((short)32767));  // 2^15 - 1
    assertByteEquals((byte)0, $opt$ShortToByte((short)-32768));  // -(2^15)
  }

  private static void intToByte() {
    assertByteEquals((byte)1, $opt$IntToByte(1));
    assertByteEquals((byte)0, $opt$IntToByte(0));
    assertByteEquals((byte)-1, $opt$IntToByte(-1));
    assertByteEquals((byte)51, $opt$IntToByte(51));
    assertByteEquals((byte)-51, $opt$IntToByte(-51));
    assertByteEquals((byte)127, $opt$IntToByte(127));  // 2^7 - 1
    assertByteEquals((byte)-127, $opt$IntToByte(-127));  // -(2^7 - 1)
    assertByteEquals((byte)-128, $opt$IntToByte(-128));  // -(2^7)
    assertByteEquals((byte)-128, $opt$IntToByte(128));  // 2^7
    assertByteEquals((byte)127, $opt$IntToByte(-129));  // -(2^7 + 1)
    assertByteEquals((byte)-1, $opt$IntToByte(2147483647));  // 2^31 - 1
    assertByteEquals((byte)0, $opt$IntToByte(-2147483648));  // -(2^31)
  }

  private static void charToByte() {
    assertByteEquals((byte)1, $opt$CharToByte((char)1));
    assertByteEquals((byte)0, $opt$CharToByte((char)0));
    assertByteEquals((byte)51, $opt$CharToByte((char)51));
    assertByteEquals((byte)127, $opt$CharToByte((char)127));  // 2^7 - 1
    assertByteEquals((byte)-128, $opt$CharToByte((char)128));  // 2^7
    assertByteEquals((byte)-1, $opt$CharToByte((char)32767));  // 2^15 - 1
    assertByteEquals((byte)-1, $opt$CharToByte((char)65535));  // 2^16 - 1
    assertByteEquals((byte)-1, $opt$CharToByte((char)-1));
    assertByteEquals((byte)-51, $opt$CharToByte((char)-51));
    assertByteEquals((byte)-127, $opt$CharToByte((char)-127));  // -(2^7 - 1)
    assertByteEquals((byte)-128, $opt$CharToByte((char)-128));  // -(2^7)
    assertByteEquals((byte)127, $opt$CharToByte((char)-129));  // -(2^7 + 1)
  }

  private static void byteToShort() {
    assertShortEquals((short)1, $opt$ByteToShort((byte)1));
    assertShortEquals((short)0, $opt$ByteToShort((byte)0));
    assertShortEquals((short)-1, $opt$ByteToShort((byte)-1));
    assertShortEquals((short)51, $opt$ByteToShort((byte)51));
    assertShortEquals((short)-51, $opt$ByteToShort((byte)-51));
    assertShortEquals((short)127, $opt$ByteToShort((byte)127));  // 2^7 - 1
    assertShortEquals((short)-127, $opt$ByteToShort((byte)-127));  // -(2^7 - 1)
    assertShortEquals((short)-128, $opt$ByteToShort((byte)-128));  // -(2^7)
  }

  private static void intToShort() {
    assertShortEquals((short)1, $opt$IntToShort(1));
    assertShortEquals((short)0, $opt$IntToShort(0));
    assertShortEquals((short)-1, $opt$IntToShort(-1));
    assertShortEquals((short)51, $opt$IntToShort(51));
    assertShortEquals((short)-51, $opt$IntToShort(-51));
    assertShortEquals((short)32767, $opt$IntToShort(32767));  // 2^15 - 1
    assertShortEquals((short)-32767, $opt$IntToShort(-32767));  // -(2^15 - 1)
    assertShortEquals((short)-32768, $opt$IntToShort(-32768));  // -(2^15)
    assertShortEquals((short)-32768, $opt$IntToShort(32768));  // 2^15
    assertShortEquals((short)32767, $opt$IntToShort(-32769));  // -(2^15 + 1)
    assertShortEquals((short)-1, $opt$IntToShort(2147483647));  // 2^31 - 1
    assertShortEquals((short)0, $opt$IntToShort(-2147483648));  // -(2^31)
  }

  private static void charToShort() {
    assertShortEquals((short)1, $opt$CharToShort((char)1));
    assertShortEquals((short)0, $opt$CharToShort((char)0));
    assertShortEquals((short)51, $opt$CharToShort((char)51));
    assertShortEquals((short)32767, $opt$CharToShort((char)32767));  // 2^15 - 1
    assertShortEquals((short)-32768, $opt$CharToShort((char)32768));  // 2^15
    assertShortEquals((short)-32767, $opt$CharToShort((char)32769));  // 2^15
    assertShortEquals((short)-1, $opt$CharToShort((char)65535));  // 2^16 - 1
    assertShortEquals((short)-1, $opt$CharToShort((char)-1));
    assertShortEquals((short)-51, $opt$CharToShort((char)-51));
    assertShortEquals((short)-32767, $opt$CharToShort((char)-32767));  // -(2^15 - 1)
    assertShortEquals((short)-32768, $opt$CharToShort((char)-32768));  // -(2^15)
    assertShortEquals((short)32767, $opt$CharToShort((char)-32769));  // -(2^15 + 1)
  }

  private static void byteToChar() {
    assertCharEquals((char)1, $opt$ByteToChar((byte)1));
    assertCharEquals((char)0, $opt$ByteToChar((byte)0));
    assertCharEquals((char)65535, $opt$ByteToChar((byte)-1));
    assertCharEquals((char)51, $opt$ByteToChar((byte)51));
    assertCharEquals((char)65485, $opt$ByteToChar((byte)-51));
    assertCharEquals((char)127, $opt$ByteToChar((byte)127));  // 2^7 - 1
    assertCharEquals((char)65409, $opt$ByteToChar((byte)-127));  // -(2^7 - 1)
    assertCharEquals((char)65408, $opt$ByteToChar((byte)-128));  // -(2^7)
  }

  private static void shortToChar() {
    assertCharEquals((char)1, $opt$ShortToChar((short)1));
    assertCharEquals((char)0, $opt$ShortToChar((short)0));
    assertCharEquals((char)65535, $opt$ShortToChar((short)-1));
    assertCharEquals((char)51, $opt$ShortToChar((short)51));
    assertCharEquals((char)65485, $opt$ShortToChar((short)-51));
    assertCharEquals((char)32767, $opt$ShortToChar((short)32767));  // 2^15 - 1
    assertCharEquals((char)32769, $opt$ShortToChar((short)-32767));  // -(2^15 - 1)
    assertCharEquals((char)32768, $opt$ShortToChar((short)-32768));  // -(2^15)
  }

  private static void intToChar() {
    assertCharEquals((char)1, $opt$IntToChar(1));
    assertCharEquals((char)0, $opt$IntToChar(0));
    assertCharEquals((char)65535, $opt$IntToChar(-1));
    assertCharEquals((char)51, $opt$IntToChar(51));
    assertCharEquals((char)65485, $opt$IntToChar(-51));
    assertCharEquals((char)32767, $opt$IntToChar(32767));  // 2^15 - 1
    assertCharEquals((char)32769, $opt$IntToChar(-32767));  // -(2^15 - 1)
    assertCharEquals((char)32768, $opt$IntToChar(32768));  // 2^15
    assertCharEquals((char)32768, $opt$IntToChar(-32768));  // -(2^15)
    assertCharEquals((char)65535, $opt$IntToChar(65535));  // 2^16 - 1
    assertCharEquals((char)1, $opt$IntToChar(-65535));  // -(2^16 - 1)
    assertCharEquals((char)0, $opt$IntToChar(65536));  // 2^16
    assertCharEquals((char)0, $opt$IntToChar(-65536));  // -(2^16)
    assertCharEquals((char)65535, $opt$IntToChar(2147483647));  // 2^31 - 1
    assertCharEquals((char)0, $opt$IntToChar(-2147483648));  // -(2^31)
  }


  // These methods produce int-to-long Dex instructions.
  static long $opt$ByteToLong(byte a) { return (long)a; }
  static long $opt$ShortToLong(short a) { return (long)a; }
  static long $opt$IntToLong(int a) { return (long)a; }
  static long $opt$CharToLong(int a) { return (long)a; }

  // These methods produce int-to-float Dex instructions.
  static float $opt$ByteToFloat(byte a) { return (float)a; }
  static float $opt$ShortToFloat(short a) { return (float)a; }
  static float $opt$IntToFloat(int a) { return (float)a; }
  static float $opt$CharToFloat(char a) { return (float)a; }

  // These methods produce int-to-double Dex instructions.
  static double $opt$ByteToDouble(byte a) { return (double)a; }
  static double $opt$ShortToDouble(short a) { return (double)a; }
  static double $opt$IntToDouble(int a) { return (double)a; }
  static double $opt$CharToDouble(int a) { return (double)a; }

  // These methods produce long-to-int Dex instructions.
  static int $opt$LongToInt(long a) { return (int)a; }
  static int $opt$LongLiteralToInt() { return (int)42L; }

  // This method produces a long-to-float Dex instruction.
  static float $opt$LongToFloat(long a) { return (float)a; }

  // This method produces a long-to-double Dex instruction.
  static double $opt$LongToDouble(long a) { return (double)a; }

  // This method produces a float-to-int Dex instruction.
  static int $opt$FloatToInt(float a) { return (int)a; }

  // This method produces a float-to-long Dex instruction.
  static long $opt$FloatToLong(float a){ return (long)a; }

  // This method produces a float-to-double Dex instruction.
  static double $opt$FloatToDouble(float a) { return (double)a; }

  // This method produces a double-to-int Dex instruction.
  static int $opt$DoubleToInt(double a){ return (int)a; }

  // This method produces a double-to-long Dex instruction.
  static long $opt$DoubleToLong(double a){ return (long)a; }

  // This method produces a double-to-float Dex instruction.
  static float $opt$DoubleToFloat(double a) { return (float)a; }

  // These methods produce int-to-byte Dex instructions.
  static byte $opt$ShortToByte(short a) { return (byte)a; }
  static byte $opt$IntToByte(int a) { return (byte)a; }
  static byte $opt$CharToByte(char a) { return (byte)a; }

  // These methods produce int-to-short Dex instructions.
  static short $opt$ByteToShort(byte a) { return (short)a; }
  static short $opt$IntToShort(int a) { return (short)a; }
  static short $opt$CharToShort(char a) { return (short)a; }

  // These methods produce int-to-char Dex instructions.
  static char $opt$ByteToChar(byte a) { return (char)a; }
  static char $opt$ShortToChar(short a) { return (char)a; }
  static char $opt$IntToChar(int a) { return (char)a; }
}
