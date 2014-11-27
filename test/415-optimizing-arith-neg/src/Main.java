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

  public static void assertEquals(int expected, int result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  public static void assertEquals(long expected, long result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  public static void assertEquals(float expected, float result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  public static void assertEquals(String expected, float result) {
    if (!expected.equals(new Float(result).toString())) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  public static void assertEquals(double expected, double result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  public static void assertEquals(String expected, double result) {
    if (!expected.equals(new Double(result).toString())) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  public static void assertIsNaN(float result) {
    if (!Float.isNaN(result)) {
      throw new Error("Expected NaN: " + result);
    }
  }

  public static void assertIsNaN(double result) {
    if (!Double.isNaN(result)) {
      throw new Error("Expected NaN: " + result);
    }
  }

  public static void main(String[] args) {
    negInt();
    $opt$InplaceNegOneInt(1);

    negLong();
    $opt$InplaceNegOneLong(1L);

    negFloat();
    negDouble();
  }

  private static void negInt() {
    assertEquals(-1, $opt$NegInt(1));
    assertEquals(1, $opt$NegInt(-1));
    assertEquals(0, $opt$NegInt(0));
    assertEquals(51, $opt$NegInt(-51));
    assertEquals(-51, $opt$NegInt(51));
    assertEquals(2147483647, $opt$NegInt(-2147483647));  // -(2^31 - 1)
    assertEquals(-2147483647, $opt$NegInt(2147483647));  // 2^31 - 1
    // From the Java 7 SE Edition specification:
    // http://docs.oracle.com/javase/specs/jls/se7/html/jls-15.html#jls-15.15.4
    //
    //   For integer values, negation is the same as subtraction from
    //   zero.  The Java programming language uses two's-complement
    //   representation for integers, and the range of two's-complement
    //   values is not symmetric, so negation of the maximum negative
    //   int or long results in that same maximum negative number.
    //   Overflow occurs in this case, but no exception is thrown.
    //   For all integer values x, -x equals (~x)+1.''
    assertEquals(-2147483648, $opt$NegInt(-2147483648)); // -(2^31)
  }

  private static void $opt$InplaceNegOneInt(int a) {
    a = -a;
    assertEquals(-1, a);
  }

  private static void negLong() {
    assertEquals(-1L, $opt$NegLong(1L));
    assertEquals(1L, $opt$NegLong(-1L));
    assertEquals(0L, $opt$NegLong(0L));
    assertEquals(51L, $opt$NegLong(-51L));
    assertEquals(-51L, $opt$NegLong(51L));

    assertEquals(2147483647L, $opt$NegLong(-2147483647L));  // -(2^31 - 1)
    assertEquals(-2147483647L, $opt$NegLong(2147483647L));  // (2^31 - 1)
    assertEquals(2147483648L, $opt$NegLong(-2147483648L));  // -(2^31)
    assertEquals(-2147483648L, $opt$NegLong(2147483648L));  // 2^31

    assertEquals(9223372036854775807L, $opt$NegLong(-9223372036854775807L));  // -(2^63 - 1)
    assertEquals(-9223372036854775807L, $opt$NegLong(9223372036854775807L));  // 2^63 - 1
    // See remark regarding the negation of the maximum negative
    // (long) value in negInt().
    assertEquals(-9223372036854775808L, $opt$NegLong(-9223372036854775808L)); // -(2^63)
  }

  private static void $opt$InplaceNegOneLong(long a) {
    a = -a;
    assertEquals(-1L, a);
  }

  private static void negFloat() {
     assertEquals("-0.0", $opt$NegFloat(0F));
     assertEquals("0.0", $opt$NegFloat(-0F));
     assertEquals(-1F, $opt$NegFloat(1F));
     assertEquals(1F, $opt$NegFloat(-1F));
     assertEquals(51F, $opt$NegFloat(-51F));
     assertEquals(-51F, $opt$NegFloat(51F));

     assertEquals(-0.1F, $opt$NegFloat(0.1F));
     assertEquals(0.1F, $opt$NegFloat(-0.1F));
     assertEquals(343597.38362F, $opt$NegFloat(-343597.38362F));
     assertEquals(-343597.38362F, $opt$NegFloat(343597.38362F));

     assertEquals(-Float.MIN_NORMAL, $opt$NegFloat(Float.MIN_NORMAL));
     assertEquals(Float.MIN_NORMAL, $opt$NegFloat(-Float.MIN_NORMAL));
     assertEquals(-Float.MIN_VALUE, $opt$NegFloat(Float.MIN_VALUE));
     assertEquals(Float.MIN_VALUE, $opt$NegFloat(-Float.MIN_VALUE));
     assertEquals(-Float.MAX_VALUE, $opt$NegFloat(Float.MAX_VALUE));
     assertEquals(Float.MAX_VALUE, $opt$NegFloat(-Float.MAX_VALUE));

     assertEquals(Float.NEGATIVE_INFINITY, $opt$NegFloat(Float.POSITIVE_INFINITY));
     assertEquals(Float.POSITIVE_INFINITY, $opt$NegFloat(Float.NEGATIVE_INFINITY));
     assertIsNaN($opt$NegFloat(Float.NaN));
  }

  private static void negDouble() {
     assertEquals("-0.0", $opt$NegDouble(0D));
     assertEquals("0.0", $opt$NegDouble(-0D));
     assertEquals(-1D, $opt$NegDouble(1D));
     assertEquals(1D, $opt$NegDouble(-1D));
     assertEquals(51D, $opt$NegDouble(-51D));
     assertEquals(-51D, $opt$NegDouble(51D));

     assertEquals(-0.1D, $opt$NegDouble(0.1D));
     assertEquals(0.1D, $opt$NegDouble(-0.1D));
     assertEquals(343597.38362D, $opt$NegDouble(-343597.38362D));
     assertEquals(-343597.38362D, $opt$NegDouble(343597.38362D));

     assertEquals(-Double.MIN_NORMAL, $opt$NegDouble(Double.MIN_NORMAL));
     assertEquals(Double.MIN_NORMAL, $opt$NegDouble(-Double.MIN_NORMAL));
     assertEquals(-Double.MIN_VALUE, $opt$NegDouble(Double.MIN_VALUE));
     assertEquals(Double.MIN_VALUE, $opt$NegDouble(-Double.MIN_VALUE));
     assertEquals(-Double.MAX_VALUE, $opt$NegDouble(Double.MAX_VALUE));
     assertEquals(Double.MAX_VALUE, $opt$NegDouble(-Double.MAX_VALUE));

     assertEquals(Double.NEGATIVE_INFINITY, $opt$NegDouble(Double.POSITIVE_INFINITY));
     assertEquals(Double.POSITIVE_INFINITY, $opt$NegDouble(Double.NEGATIVE_INFINITY));
     assertIsNaN($opt$NegDouble(Double.NaN));
  }

  static int $opt$NegInt(int a){
    return -a;
  }

  static long $opt$NegLong(long a){
    return -a;
  }

  static float $opt$NegFloat(float a){
    return -a;
  }

  static double $opt$NegDouble(double a){
    return -a;
  }
}
