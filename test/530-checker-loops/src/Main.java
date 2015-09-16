/*
 * Copyright (C) 2015 The Android Open Source Project
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

//
// Test on loop optimizations.
//
public class Main {

  static int sResult;

  //
  // Various sequence variables where bound checks can be removed from loop.
  //

  /// CHECK-START: int Main.linear(int[]) BCE (before)
  /// CHECK-DAG: BoundsCheck
  /// CHECK-START: int Main.linear(int[]) BCE (after)
  /// CHECK-NOT: BoundsCheck
  private static int linear(int[] x) {
    int result = 0;
    for (int i = 0; i < x.length; i++) {
      result += x[i];
    }
    return result;
  }

  /// CHECK-START: int Main.linearDown(int[]) BCE (before)
  /// CHECK-DAG: BoundsCheck
  /// CHECK-START: int Main.linearDown(int[]) BCE (after)
  /// CHECK-NOT: BoundsCheck
  private static int linearDown(int[] x) {
    int result = 0;
    for (int i = x.length - 1; i >= 0; i--) {
      result += x[i];
    }
    return result;
  }

  /// CHECK-START: int Main.linearObscure(int[]) BCE (before)
  /// CHECK-DAG: BoundsCheck
  /// CHECK-START: int Main.linearObscure(int[]) BCE (after)
  /// CHECK-NOT: BoundsCheck
  private static int linearObscure(int[] x) {
    int result = 0;
    for (int i = x.length - 1; i >= 0; i--) {
      int k = i + 5;
      result += x[k - 5];
    }
    return result;
  }

  /// CHECK-START: int Main.linearWhile(int[]) BCE (before)
  /// CHECK-DAG: BoundsCheck
  /// CHECK-START: int Main.linearWhile(int[]) BCE (after)
  /// CHECK-NOT: BoundsCheck
  private static int linearWhile(int[] x) {
    int i = 0;
    int result = 0;
    while (i < x.length) {
      result += x[i++];
    }
    return result;
  }

  /// CHECK-START: int Main.wrapAroundThenLinear(int[]) BCE (before)
  /// CHECK-DAG: BoundsCheck
  /// CHECK-START: int Main.wrapAroundThenLinear(int[]) BCE (after)
  /// CHECK-NOT: BoundsCheck
  private static int wrapAroundThenLinear(int[] x) {
    // Loop with wrap around (length - 1, 0, 1, 2, ..).
    int w = x.length - 1;
    int result = 0;
    for (int i = 0; i < x.length; i++) {
      result += x[w];
      w = i;
    }
    return result;
  }

  /// CHECK-START: int[] Main.linearWithParameter(int) BCE (before)
  /// CHECK-DAG: BoundsCheck
  /// CHECK-START: int[] Main.linearWithParameter(int) BCE (after)
  /// CHECK-NOT: BoundsCheck
  private static int[] linearWithParameter(int n) {
    int[] x = new int[n];
    for (int i = 0; i < n; i++) {
      x[i] = i;
    }
    return x;
  }

  /// CHECK-START: int Main.linearWithCompoundStride() BCE (before)
  /// CHECK-DAG: BoundsCheck
  /// CHECK-START: int Main.linearWithCompoundStride() BCE (after)
  /// CHECK-NOT: BoundsCheck
  private static int linearWithCompoundStride() {
    int[] x = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14 };
    int result = 0;
    for (int i = 0; i <= 12; ) {
      i++;
      result += x[i];
      i++;
    }
    return result;
  }

  /// CHECK-START: int Main.linearWithLargePositiveStride() BCE (before)
  /// CHECK-DAG: BoundsCheck
  /// CHECK-START: int Main.linearWithLargePositiveStride() BCE (after)
  /// CHECK-NOT: BoundsCheck
  private static int linearWithLargePositiveStride() {
    int[] x = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
    int result = 0;
    int k = 0;
    // Range analysis has no problem with a trip-count defined by a
    // reasonably large positive stride.
    for (int i = 1; i <= 10 * 10000000 + 1; i += 10000000) {
      result += x[k++];
    }
    return result;
  }

  /// CHECK-START: int Main.linearWithVeryLargePositiveStride() BCE (before)
  /// CHECK-DAG: BoundsCheck
  /// CHECK-START: int Main.linearWithVeryLargePositiveStride() BCE (after)
  /// CHECK-DAG: BoundsCheck
  private static int linearWithVeryLargePositiveStride() {
    int[] x = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
    int result = 0;
    int k = 0;
    // Range analysis conservatively bails due to potential of wrap-around
    // arithmetic while computing the trip-count for this very large stride.
    for (int i = 1; i < 2147483647; i += 195225786) {
      result += x[k++];
    }
    return result;
  }

  /// CHECK-START: int Main.linearWithLargeNegativeStride() BCE (before)
  /// CHECK-DAG: BoundsCheck
  /// CHECK-START: int Main.linearWithLargeNegativeStride() BCE (after)
  /// CHECK-NOT: BoundsCheck
  private static int linearWithLargeNegativeStride() {
    int[] x = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
    int result = 0;
    int k = 0;
    // Range analysis has no problem with a trip-count defined by a
    // reasonably large negative stride.
    for (int i = -1; i >= -10 * 10000000 - 1; i -= 10000000) {
      result += x[k++];
    }
    return result;
  }

  /// CHECK-START: int Main.linearWithVeryLargeNegativeStride() BCE (before)
  /// CHECK-DAG: BoundsCheck
  /// CHECK-START: int Main.linearWithVeryLargeNegativeStride() BCE (after)
  /// CHECK-DAG: BoundsCheck
  private static int linearWithVeryLargeNegativeStride() {
    int[] x = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
    int result = 0;
    int k = 0;
    // Range analysis conservatively bails due to potential of wrap-around
    // arithmetic while computing the trip-count for this very large stride.
    for (int i = -2; i > -2147483648; i -= 195225786) {
      result += x[k++];
    }
    return result;
  }

  /// CHECK-START: int Main.periodicIdiom(int) BCE (before)
  /// CHECK-DAG: BoundsCheck
  /// CHECK-START: int Main.periodicIdiom(int) BCE (after)
  /// CHECK-NOT: BoundsCheck
  private static int periodicIdiom(int tc) {
    int[] x = { 1, 3 };
    // Loop with periodic sequence (0, 1).
    int k = 0;
    int result = 0;
    for (int i = 0; i < tc; i++) {
      result += x[k];
      k = 1 - k;
    }
    return result;
  }

  /// CHECK-START: int Main.periodicSequence2(int) BCE (before)
  /// CHECK-DAG: BoundsCheck
  /// CHECK-START: int Main.periodicSequence2(int) BCE (after)
  /// CHECK-NOT: BoundsCheck
  private static int periodicSequence2(int tc) {
    int[] x = { 1, 3 };
    // Loop with periodic sequence (0, 1).
    int k = 0;
    int l = 1;
    int result = 0;
    for (int i = 0; i < tc; i++) {
      result += x[k];
      int t = l;
      l = k;
      k = t;
    }
    return result;
  }

  /// CHECK-START: int Main.periodicSequence4(int) BCE (before)
  /// CHECK-DAG: BoundsCheck
  /// CHECK-DAG: BoundsCheck
  /// CHECK-DAG: BoundsCheck
  /// CHECK-DAG: BoundsCheck
  /// CHECK-START: int Main.periodicSequence4(int) BCE (after)
  /// CHECK-NOT: BoundsCheck
  private static int periodicSequence4(int tc) {
    int[] x = { 1, 3, 5, 7 };
    // Loop with periodic sequence (0, 1, 2, 3).
    int k = 0;
    int l = 1;
    int m = 2;
    int n = 3;
    int result = 0;
    for (int i = 0; i < tc; i++) {
      result += x[k] + x[l] + x[m] + x[n];  // all used at once
      int t = n;
      n = k;
      k = l;
      l = m;
      m = t;
    }
    return result;
  }

  //
  // Cases that actually go out of bounds. These test cases
  // ensure the exceptions are thrown at the right places.
  //

  private static void lowerOOB(int[] x) {
    for (int i = -1; i < x.length; i++) {
      sResult += x[i];
    }
  }

  private static void upperOOB(int[] x) {
    for (int i = 0; i <= x.length; i++) {
      sResult += x[i];
    }
  }

  //
  // Verifier.
  //

  public static void main(String[] args) {
    int[] empty = { };
    int[] x = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };

    // Linear and wrap-around.
    expectEquals(0, linear(empty));
    expectEquals(55, linear(x));
    expectEquals(0, linearDown(empty));
    expectEquals(55, linearDown(x));
    expectEquals(0, linearObscure(empty));
    expectEquals(55, linearObscure(x));
    expectEquals(0, linearWhile(empty));
    expectEquals(55, linearWhile(x));
    expectEquals(0, wrapAroundThenLinear(empty));
    expectEquals(55, wrapAroundThenLinear(x));

    // Linear with parameter.
    sResult = 0;
    try {
      linearWithParameter(-1);
    } catch (NegativeArraySizeException e) {
      sResult = 1;
    }
    expectEquals(1, sResult);
    for (int n = 0; n < 32; n++) {
      int[] r = linearWithParameter(n);
      expectEquals(n, r.length);
      for (int i = 0; i < n; i++) {
        expectEquals(i, r[i]);
      }
    }

    // Linear with non-unit strides.
    expectEquals(56, linearWithCompoundStride());
    expectEquals(66, linearWithLargePositiveStride());
    expectEquals(66, linearWithVeryLargePositiveStride());
    expectEquals(66, linearWithLargeNegativeStride());
    expectEquals(66, linearWithVeryLargeNegativeStride());

    // Periodic adds (1, 3), one at the time.
    expectEquals(0, periodicIdiom(-1));
    for (int tc = 0; tc < 32; tc++) {
      int expected = (tc >> 1) << 2;
      if ((tc & 1) != 0)
        expected += 1;
      expectEquals(expected, periodicIdiom(tc));
    }

    // Periodic adds (1, 3), one at the time.
    expectEquals(0, periodicSequence2(-1));
    for (int tc = 0; tc < 32; tc++) {
      int expected = (tc >> 1) << 2;
      if ((tc & 1) != 0)
        expected += 1;
      expectEquals(expected, periodicSequence2(tc));
    }

    // Periodic adds (1, 3, 5, 7), all at once.
    expectEquals(0, periodicSequence4(-1));
    for (int tc = 0; tc < 32; tc++) {
      expectEquals(tc * 16, periodicSequence4(tc));
    }

    // Lower bound goes OOB.
    sResult = 0;
    try {
      lowerOOB(x);
    } catch (ArrayIndexOutOfBoundsException e) {
      sResult += 1000;
    }
    expectEquals(1000, sResult);

    // Upper bound goes OOB.
    sResult = 0;
    try {
      upperOOB(x);
    } catch (ArrayIndexOutOfBoundsException e) {
      sResult += 1000;
    }
    expectEquals(1055, sResult);

  }

  private static void expectEquals(int expected, int result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }
}
