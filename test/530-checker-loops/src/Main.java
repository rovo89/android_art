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
  // Various sequence variables used in bound checks.
  //

  /// CHECK-START: int Main.linear(int[]) BCE (before)
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: int Main.linear(int[]) BCE (after)
  /// CHECK-NOT: BoundsCheck
  /// CHECK-NOT: Deoptimize
  private static int linear(int[] x) {
    int result = 0;
    for (int i = 0; i < x.length; i++) {
      result += x[i];
    }
    return result;
  }

  /// CHECK-START: int Main.linearDown(int[]) BCE (before)
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: int Main.linearDown(int[]) BCE (after)
  /// CHECK-NOT: BoundsCheck
  /// CHECK-NOT: Deoptimize
  private static int linearDown(int[] x) {
    int result = 0;
    for (int i = x.length - 1; i >= 0; i--) {
      result += x[i];
    }
    return result;
  }

  /// CHECK-START: int Main.linearObscure(int[]) BCE (before)
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: int Main.linearObscure(int[]) BCE (after)
  /// CHECK-NOT: BoundsCheck
  /// CHECK-NOT: Deoptimize
  private static int linearObscure(int[] x) {
    int result = 0;
    for (int i = x.length - 1; i >= 0; i--) {
      int k = i + 5;
      result += x[k - 5];
    }
    return result;
  }

  /// CHECK-START: int Main.linearVeryObscure(int[]) BCE (before)
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: int Main.linearVeryObscure(int[]) BCE (after)
  /// CHECK-NOT: BoundsCheck
  /// CHECK-NOT: Deoptimize
  private static int linearVeryObscure(int[] x) {
    int result = 0;
    for (int i = 0; i < x.length; i++) {
      int k = (-i) + (i << 5) + i - (32 * i) + 5 + (int) i;
      result += x[k - 5];
    }
    return result;
  }

  /// CHECK-START: int Main.hiddenStride(int[]) BCE (before)
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: int Main.hiddenStride(int[]) BCE (after)
  /// CHECK-NOT: BoundsCheck
  /// CHECK-NOT: Deoptimize
  static int hiddenStride(int[] a) {
    int result = 0;
    for (int i = 1; i <= 1; i++) {
      // Obscured unit stride.
      for (int j = 0; j < a.length; j += i) {
        result += a[j];
      }
    }
    return result;
  }

  /// CHECK-START: int Main.linearWhile(int[]) BCE (before)
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: int Main.linearWhile(int[]) BCE (after)
  /// CHECK-NOT: BoundsCheck
  /// CHECK-NOT: Deoptimize
  private static int linearWhile(int[] x) {
    int i = 0;
    int result = 0;
    while (i < x.length) {
      result += x[i++];
    }
    return result;
  }

  /// CHECK-START: int Main.linearThreeWayPhi(int[]) BCE (before)
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: int Main.linearThreeWayPhi(int[]) BCE (after)
  /// CHECK-NOT: BoundsCheck
  /// CHECK-NOT: Deoptimize
  private static int linearThreeWayPhi(int[] x) {
    int result = 0;
    for (int i = 0; i < x.length; ) {
      if (x[i] == 5) {
        i++;
        continue;
      }
      result += x[i++];
    }
    return result;
  }

  /// CHECK-START: int Main.linearFourWayPhi(int[]) BCE (before)
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: int Main.linearFourWayPhi(int[]) BCE (after)
  /// CHECK-NOT: BoundsCheck
  /// CHECK-NOT: Deoptimize
  private static int linearFourWayPhi(int[] x) {
    int result = 0;
    for (int i = 0; i < x.length; ) {
      if (x[i] == 5) {
        i++;
        continue;
      } else if (x[i] == 6) {
        i++;
        result += 7;
        continue;
      }
      result += x[i++];
    }
    return result;
  }

  /// CHECK-START: int Main.wrapAroundThenLinear(int[]) BCE (before)
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: int Main.wrapAroundThenLinear(int[]) BCE (after)
  /// CHECK-NOT: BoundsCheck
  /// CHECK-NOT: Deoptimize
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

  /// CHECK-START: int Main.wrapAroundThenLinearThreeWayPhi(int[]) BCE (before)
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: int Main.wrapAroundThenLinearThreeWayPhi(int[]) BCE (after)
  /// CHECK-NOT: BoundsCheck
  /// CHECK-NOT: Deoptimize
  private static int wrapAroundThenLinearThreeWayPhi(int[] x) {
    // Loop with wrap around (length - 1, 0, 1, 2, ..).
    int w = x.length - 1;
    int result = 0;
    for (int i = 0; i < x.length; ) {
       if (x[w] == 1) {
         w = i++;
         continue;
       }
       result += x[w];
       w = i++;
    }
    return result;
  }

  /// CHECK-START: int[] Main.linearWithParameter(int) BCE (before)
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: int[] Main.linearWithParameter(int) BCE (after)
  /// CHECK-NOT: BoundsCheck
  /// CHECK-NOT: Deoptimize
  private static int[] linearWithParameter(int n) {
    int[] x = new int[n];
    for (int i = 0; i < n; i++) {
      x[i] = i;
    }
    return x;
  }

  /// CHECK-START: int[] Main.linearCopy(int[]) BCE (before)
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: int[] Main.linearCopy(int[]) BCE (after)
  /// CHECK-NOT: BoundsCheck
  /// CHECK-NOT: Deoptimize
  private static int[] linearCopy(int x[]) {
    int n = x.length;
    int y[] = new int[n];
    for (int i = 0; i < n; i++) {
      y[i] = x[i];
    }
    return y;
  }

  /// CHECK-START: int Main.linearByTwo(int[]) BCE (before)
  /// CHECK-DAG: BoundsCheck
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: int Main.linearByTwo(int[]) BCE (after)
  /// CHECK-NOT: BoundsCheck
  /// CHECK-NOT: Deoptimize
  private static int linearByTwo(int x[]) {
    int n = x.length / 2;
    int result = 0;
    for (int i = 0; i < n; i++) {
      int ii = i << 1;
      result += x[ii];
      result += x[ii + 1];
    }
    return result;
  }

  /// CHECK-START: int Main.linearByTwoSkip1(int[]) BCE (before)
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: int Main.linearByTwoSkip1(int[]) BCE (after)
  /// CHECK-NOT: BoundsCheck
  /// CHECK-NOT: Deoptimize
  private static int linearByTwoSkip1(int x[]) {
    int result = 0;
    for (int i = 0; i < x.length / 2; i++) {
      result += x[2 * i];
    }
    return result;
  }

  /// CHECK-START: int Main.linearByTwoSkip2(int[]) BCE (before)
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: int Main.linearByTwoSkip2(int[]) BCE (after)
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: int Main.linearByTwoSkip2(int[]) BCE (after)
  /// CHECK-NOT: Deoptimize
  private static int linearByTwoSkip2(int x[]) {
    int result = 0;
    // This case is not optimized.
    for (int i = 0; i < x.length; i+=2) {
      result += x[i];
    }
    return result;
  }

  /// CHECK-START: int Main.linearWithCompoundStride() BCE (before)
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: int Main.linearWithCompoundStride() BCE (after)
  /// CHECK-NOT: BoundsCheck
  /// CHECK-NOT: Deoptimize
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
  //
  /// CHECK-START: int Main.linearWithLargePositiveStride() BCE (after)
  /// CHECK-NOT: BoundsCheck
  /// CHECK-NOT: Deoptimize
  private static int linearWithLargePositiveStride() {
    int[] x = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
    int result = 0;
    int k = 0;
    // Range analysis has no problem with a trip-count defined by a
    // reasonably large positive stride far away from upper bound.
    for (int i = 1; i <= 10 * 10000000 + 1; i += 10000000) {
      result += x[k++];
    }
    return result;
  }

  /// CHECK-START: int Main.linearWithVeryLargePositiveStride() BCE (before)
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: int Main.linearWithVeryLargePositiveStride() BCE (after)
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: int Main.linearWithVeryLargePositiveStride() BCE (after)
  /// CHECK-NOT: Deoptimize
  private static int linearWithVeryLargePositiveStride() {
    int[] x = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
    int result = 0;
    int k = 0;
    // Range analysis conservatively bails due to potential of wrap-around
    // arithmetic while computing the trip-count for this very large stride.
    for (int i = 1; i < Integer.MAX_VALUE; i += 195225786) {
      result += x[k++];
    }
    return result;
  }

  /// CHECK-START: int Main.linearWithLargeNegativeStride() BCE (before)
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: int Main.linearWithLargeNegativeStride() BCE (after)
  /// CHECK-NOT: BoundsCheck
  /// CHECK-NOT: Deoptimize
  private static int linearWithLargeNegativeStride() {
    int[] x = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
    int result = 0;
    int k = 0;
    // Range analysis has no problem with a trip-count defined by a
    // reasonably large negative stride far away from lower bound.
    for (int i = -1; i >= -10 * 10000000 - 1; i -= 10000000) {
      result += x[k++];
    }
    return result;
  }

  /// CHECK-START: int Main.linearWithVeryLargeNegativeStride() BCE (before)
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: int Main.linearWithVeryLargeNegativeStride() BCE (after)
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: int Main.linearWithVeryLargeNegativeStride() BCE (after)
  /// CHECK-NOT: Deoptimize
  private static int linearWithVeryLargeNegativeStride() {
    int[] x = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
    int result = 0;
    int k = 0;
    // Range analysis conservatively bails due to potential of wrap-around
    // arithmetic while computing the trip-count for this very large stride.
    for (int i = -2; i > Integer.MIN_VALUE; i -= 195225786) {
      result += x[k++];
    }
    return result;
  }

  /// CHECK-START: int Main.linearForNEUp() BCE (before)
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: int Main.linearForNEUp() BCE (after)
  /// CHECK-NOT: BoundsCheck
  /// CHECK-NOT: Deoptimize
  private static int linearForNEUp() {
    int[] x = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    int result = 0;
    for (int i = 0; i != 10; i++) {
      result += x[i];
    }
    return result;
  }

  /// CHECK-START: int Main.linearForNEDown() BCE (before)
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: int Main.linearForNEDown() BCE (after)
  /// CHECK-NOT: BoundsCheck
  /// CHECK-NOT: Deoptimize
  private static int linearForNEDown() {
    int[] x = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    int result = 0;
    for (int i = 9; i != -1; i--) {
      result += x[i];
    }
    return result;
  }

  /// CHECK-START: int Main.linearDoWhileUp() BCE (before)
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: int Main.linearDoWhileUp() BCE (after)
  /// CHECK-NOT: BoundsCheck
  /// CHECK-NOT: Deoptimize
  private static int linearDoWhileUp() {
    int[] x = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    int result = 0;
    int i = 0;
    do {
      result += x[i++];
    } while (i < 10);
    return result;
  }

  /// CHECK-START: int Main.linearDoWhileDown() BCE (before)
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: int Main.linearDoWhileDown() BCE (after)
  /// CHECK-NOT: BoundsCheck
  /// CHECK-NOT: Deoptimize
  private static int linearDoWhileDown() {
    int[] x = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    int result = 0;
    int i = 9;
    do {
      result += x[i--];
    } while (0 <= i);
    return result;
  }

  /// CHECK-START: int Main.linearShort() BCE (before)
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: int Main.linearShort() BCE (after)
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: int Main.linearShort() BCE (after)
  /// CHECK-NOT: Deoptimize
  private static int linearShort() {
    int[] x = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    int result = 0;
    // TODO: make this work
    for (short i = 0; i < 10; i++) {
      result += x[i];
    }
    return result;
  }

  /// CHECK-START: int Main.invariantFromPreLoop(int[], int) BCE (before)
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: int Main.invariantFromPreLoop(int[], int) BCE (after)
  /// CHECK-NOT: BoundsCheck
  /// CHECK-NOT: Deoptimize
  private static int invariantFromPreLoop(int[] x, int y) {
    int result = 0;
    // Strange pre-loop that sets upper bound.
    int hi;
    while (true) {
      y = y % 3;
      hi = x.length;
      if (y != 123) break;
    }
    for (int i = 0; i < hi; i++) {
       result += x[i];
    }
    return result;
  }

  /// CHECK-START: void Main.linearTriangularOnTwoArrayLengths(int) BCE (before)
  /// CHECK-DAG: BoundsCheck
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: void Main.linearTriangularOnTwoArrayLengths(int) BCE (after)
  /// CHECK-NOT: BoundsCheck
  /// CHECK-NOT: Deoptimize
  private static void linearTriangularOnTwoArrayLengths(int n) {
    int[] a = new int[n];
    for (int i = 0; i < a.length; i++) {
      int[] b = new int[i];
      for (int j = 0; j < b.length; j++) {
        // Need to know j < b.length < a.length for static bce.
        a[j] += 1;
        // Need to know just j < b.length for static bce.
        b[j] += 1;
      }
      verifyTriangular(a, b, i, n);
    }
  }

  /// CHECK-START: void Main.linearTriangularOnOneArrayLength(int) BCE (before)
  /// CHECK-DAG: BoundsCheck
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: void Main.linearTriangularOnOneArrayLength(int) BCE (after)
  /// CHECK-NOT: BoundsCheck
  /// CHECK-NOT: Deoptimize
  private static void linearTriangularOnOneArrayLength(int n) {
    int[] a = new int[n];
    for (int i = 0; i < a.length; i++) {
      int[] b = new int[i];
      for (int j = 0; j < i; j++) {
        // Need to know j < i < a.length for static bce.
        a[j] += 1;
        // Need to know just j < i for static bce.
        b[j] += 1;
      }
      verifyTriangular(a, b, i, n);
    }
  }

  /// CHECK-START: void Main.linearTriangularOnParameter(int) BCE (before)
  /// CHECK-DAG: BoundsCheck
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: void Main.linearTriangularOnParameter(int) BCE (after)
  /// CHECK-NOT: BoundsCheck
  /// CHECK-NOT: Deoptimize
  private static void linearTriangularOnParameter(int n) {
    int[] a = new int[n];
    for (int i = 0; i < n; i++) {
      int[] b = new int[i];
      for (int j = 0; j < i; j++) {
        // Need to know j < i < n for static bce.
        a[j] += 1;
        // Need to know just j < i for static bce.
        b[j] += 1;
      }
      verifyTriangular(a, b, i, n);
    }
  }

  /// CHECK-START: void Main.linearTriangularVariations(int) BCE (before)
  /// CHECK-DAG: BoundsCheck
  /// CHECK-DAG: BoundsCheck
  /// CHECK-DAG: BoundsCheck
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: void Main.linearTriangularVariations(int) BCE (after)
  /// CHECK-NOT: BoundsCheck
  /// CHECK-NOT: Deoptimize
  private static void linearTriangularVariations(int n) {
    int[] a = new int[n];
    for (int i = 0; i < n; i++) {
      for (int j = 0; j < i; j++) {
        a[j] += 1;
      }
      for (int j = i - 1; j >= 0; j--) {
        a[j] += 1;
      }
      for (int j = i; j < n; j++) {
        a[j] += 1;
      }
      for (int j = n - 1; j > i - 1; j--) {
        a[j] += 1;
      }
    }
    verifyTriangular(a);
  }

  // Verifier for triangular loops.
  private static void verifyTriangular(int[] a, int[] b, int m, int n) {
    expectEquals(n, a.length);
    for (int i = 0, k = m; i < n; i++) {
      expectEquals(a[i], k);
      if (k > 0) k--;
    }
    expectEquals(m, b.length);
    for (int i = 0; i < m; i++) {
      expectEquals(b[i], 1);
    }
  }

  // Verifier for triangular loops.
  private static void verifyTriangular(int[] a) {
    int n = a.length;
    for (int i = 0; i < n; i++) {
      expectEquals(a[i], n + n);
    }
  }

  /// CHECK-START: void Main.bubble(int[]) BCE (before)
  /// CHECK-DAG: BoundsCheck
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: void Main.bubble(int[]) BCE (after)
  /// CHECK-NOT: BoundsCheck
  /// CHECK-NOT: Deoptimize
  private static void bubble(int[] a) {
    for (int i = a.length; --i >= 0;) {
      for (int j = 0; j < i; j++) {
        if (a[j] > a[j+1]) {
          int tmp = a[j];
          a[j]  = a[j+1];
          a[j+1] = tmp;
        }
      }
    }
  }

  /// CHECK-START: int Main.periodicIdiom(int) BCE (before)
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: int Main.periodicIdiom(int) BCE (after)
  /// CHECK-NOT: BoundsCheck
  /// CHECK-NOT: Deoptimize
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
  //
  /// CHECK-START: int Main.periodicSequence2(int) BCE (after)
  /// CHECK-NOT: BoundsCheck
  /// CHECK-NOT: Deoptimize
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
  //
  /// CHECK-START: int Main.periodicSequence4(int) BCE (after)
  /// CHECK-NOT: BoundsCheck
  /// CHECK-NOT: Deoptimize
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

  /// CHECK-START: int Main.justRightUp1() BCE (before)
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: int Main.justRightUp1() BCE (after)
  /// CHECK-NOT: BoundsCheck
  /// CHECK-NOT: Deoptimize
  private static int justRightUp1() {
    int[] x = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    int result = 0;
    for (int i = Integer.MAX_VALUE - 10, k = 0; i < Integer.MAX_VALUE; i++) {
      result += x[k++];
    }
    return result;
  }

  /// CHECK-START: int Main.justRightUp2() BCE (before)
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: int Main.justRightUp2() BCE (after)
  /// CHECK-NOT: BoundsCheck
  /// CHECK-NOT: Deoptimize
  private static int justRightUp2() {
    int[] x = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    int result = 0;
    for (int i = Integer.MAX_VALUE - 10; i < Integer.MAX_VALUE; i++) {
      result += x[i - Integer.MAX_VALUE + 10];
    }
    return result;
  }

  /// CHECK-START: int Main.justRightUp3() BCE (before)
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: int Main.justRightUp3() BCE (after)
  /// CHECK-NOT: BoundsCheck
  /// CHECK-NOT: Deoptimize
  private static int justRightUp3() {
    int[] x = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    int result = 0;
    for (int i = Integer.MAX_VALUE - 10, k = 0; i <= Integer.MAX_VALUE - 1; i++) {
      result += x[k++];
    }
    return result;
  }

  /// CHECK-START: int Main.justOOBUp() BCE (before)
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: int Main.justOOBUp() BCE (after)
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: int Main.justOOBUp() BCE (after)
  /// CHECK-NOT: Deoptimize
  private static int justOOBUp() {
    int[] x = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    int result = 0;
    // Infinite loop!
    for (int i = Integer.MAX_VALUE - 9, k = 0; i <= Integer.MAX_VALUE; i++) {
      result += x[k++];
    }
    return result;
  }

  /// CHECK-START: int Main.justRightDown1() BCE (before)
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: int Main.justRightDown1() BCE (after)
  /// CHECK-NOT: BoundsCheck
  /// CHECK-NOT: Deoptimize
  private static int justRightDown1() {
    int[] x = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    int result = 0;
    for (int i = Integer.MIN_VALUE + 10, k = 0; i > Integer.MIN_VALUE; i--) {
      result += x[k++];
    }
    return result;
  }

  /// CHECK-START: int Main.justRightDown2() BCE (before)
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: int Main.justRightDown2() BCE (after)
  /// CHECK-NOT: BoundsCheck
  /// CHECK-NOT: Deoptimize
  private static int justRightDown2() {
    int[] x = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    int result = 0;
    for (int i = Integer.MIN_VALUE + 10; i > Integer.MIN_VALUE; i--) {
      result += x[Integer.MAX_VALUE + i];
    }
    return result;
  }

  /// CHECK-START: int Main.justRightDown3() BCE (before)
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: int Main.justRightDown3() BCE (after)
  /// CHECK-NOT: BoundsCheck
  /// CHECK-NOT: Deoptimize
  private static int justRightDown3() {
    int[] x = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    int result = 0;
    for (int i = Integer.MIN_VALUE + 10, k = 0; i >= Integer.MIN_VALUE + 1; i--) {
      result += x[k++];
    }
    return result;
  }

  /// CHECK-START: int Main.justOOBDown() BCE (before)
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: int Main.justOOBDown() BCE (after)
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: int Main.justOOBDown() BCE (after)
  /// CHECK-NOT: Deoptimize
  private static int justOOBDown() {
    int[] x = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    int result = 0;
    // Infinite loop!
    for (int i = Integer.MIN_VALUE + 9, k = 0; i >= Integer.MIN_VALUE; i--) {
      result += x[k++];
    }
    return result;
  }

  /// CHECK-START: void Main.lowerOOB(int[]) BCE (before)
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: void Main.lowerOOB(int[]) BCE (after)
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: void Main.lowerOOB(int[]) BCE (after)
  /// CHECK-NOT: Deoptimize
  private static void lowerOOB(int[] x) {
    // OOB!
    for (int i = -1; i < x.length; i++) {
      sResult += x[i];
    }
  }

  /// CHECK-START: void Main.upperOOB(int[]) BCE (before)
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: void Main.upperOOB(int[]) BCE (after)
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: void Main.upperOOB(int[]) BCE (after)
  /// CHECK-NOT: Deoptimize
  private static void upperOOB(int[] x) {
    // OOB!
    for (int i = 0; i <= x.length; i++) {
      sResult += x[i];
    }
  }

  /// CHECK-START: void Main.doWhileUpOOB() BCE (before)
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: void Main.doWhileUpOOB() BCE (after)
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: void Main.doWhileUpOOB() BCE (after)
  /// CHECK-NOT: Deoptimize
  private static void doWhileUpOOB() {
    int[] x = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    int i = 0;
    // OOB!
    do {
      sResult += x[i++];
    } while (i <= x.length);
  }

  /// CHECK-START: void Main.doWhileDownOOB() BCE (before)
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: void Main.doWhileDownOOB() BCE (after)
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: void Main.doWhileDownOOB() BCE (after)
  /// CHECK-NOT: Deoptimize
  private static void doWhileDownOOB() {
    int[] x = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    int i = x.length - 1;
    // OOB!
    do {
      sResult += x[i--];
    } while (-1 <= i);
  }

  /// CHECK-START: int[] Main.multiply1() BCE (before)
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: int[] Main.multiply1() BCE (after)
  /// CHECK-NOT: BoundsCheck
  /// CHECK-NOT: Deoptimize
  private static int[] multiply1() {
    int[] a = new int[10];
    try {
      for (int i = 0; i <= 3; i++) {
        for (int j = 0; j <= 3; j++) {
          // Range [0,9]: safe.
          a[i * j] += 1;
        }
      }
    } catch (Exception e) {
      a[0] += 1000;
    }
    return a;
  }

  /// CHECK-START: int[] Main.multiply2() BCE (before)
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: int[] Main.multiply2() BCE (after)
  /// CHECK-DAG: BoundsCheck
  //
  /// CHECK-START: int[] Main.multiply2() BCE (after)
  /// CHECK-NOT: Deoptimize
  static int[] multiply2() {
    int[] a = new int[10];
    try {
      for (int i = -3; i <= 3; i++) {
        for (int j = -3; j <= 3; j++) {
          // Range [-9,9]: unsafe.
          a[i * j] += 1;
        }
      }
    } catch (Exception e) {
      a[0] += 1000;
    }
    return a;
  }

  /// CHECK-START: int Main.linearDynamicBCE1(int[], int, int) BCE (before)
  /// CHECK-DAG: ArrayGet    loop:<<Loop:B\d+>>
  /// CHECK-DAG: NullCheck   loop:<<Loop>>
  /// CHECK-DAG: ArrayLength loop:<<Loop>>
  /// CHECK-DAG: BoundsCheck loop:<<Loop>>
  //
  /// CHECK-START: int Main.linearDynamicBCE1(int[], int, int) BCE (after)
  /// CHECK-DAG: ArrayGet    loop:{{B\d+}}
  /// CHECK-DAG: Deoptimize  loop:none
  //
  /// CHECK-START: int Main.linearDynamicBCE1(int[], int, int) BCE (after)
  /// CHECK-NOT: NullCheck   loop:{{B\d+}}
  /// CHECK-NOT: ArrayLength loop:{{B\d+}}
  /// CHECK-NOT: BoundsCheck loop:{{B\d+}}
  private static int linearDynamicBCE1(int[] x, int lo, int hi) {
    int result = 0;
    for (int i = lo; i < hi; i++) {
      sResult += x[i];
    }
    return result;
  }

  /// CHECK-START: int Main.linearDynamicBCE2(int[], int, int, int) BCE (before)
  /// CHECK-DAG: ArrayGet    loop:<<Loop:B\d+>>
  /// CHECK-DAG: NullCheck   loop:<<Loop>>
  /// CHECK-DAG: ArrayLength loop:<<Loop>>
  /// CHECK-DAG: BoundsCheck loop:<<Loop>>
  //
  /// CHECK-START: int Main.linearDynamicBCE2(int[], int, int, int) BCE (after)
  /// CHECK-DAG: ArrayGet    loop:{{B\d+}}
  /// CHECK-DAG: Deoptimize  loop:none
  //
  /// CHECK-START: int Main.linearDynamicBCE2(int[], int, int, int) BCE (after)
  /// CHECK-NOT: NullCheck   loop:{{B\d+}}
  /// CHECK-NOT: ArrayLength loop:{{B\d+}}
  /// CHECK-NOT: BoundsCheck loop:{{B\d+}}
  private static int linearDynamicBCE2(int[] x, int lo, int hi, int offset) {
    int result = 0;
    for (int i = lo; i < hi; i++) {
      sResult += x[offset + i];
    }
    return result;
  }

  /// CHECK-START: int Main.wrapAroundDynamicBCE(int[]) BCE (before)
  /// CHECK-DAG: ArrayGet    loop:<<Loop:B\d+>>
  /// CHECK-DAG: NullCheck   loop:<<Loop>>
  /// CHECK-DAG: ArrayLength loop:<<Loop>>
  /// CHECK-DAG: BoundsCheck loop:<<Loop>>
  //
  /// CHECK-START: int Main.wrapAroundDynamicBCE(int[]) BCE (after)
  /// CHECK-DAG: ArrayGet    loop:{{B\d+}}
  /// CHECK-DAG: Deoptimize  loop:none
  //
  /// CHECK-START: int Main.wrapAroundDynamicBCE(int[]) BCE (after)
  /// CHECK-NOT: NullCheck   loop:{{B\d+}}
  /// CHECK-NOT: ArrayLength loop:{{B\d+}}
  /// CHECK-NOT: BoundsCheck loop:{{B\d+}}
  private static int wrapAroundDynamicBCE(int[] x) {
    int w = 9;
    int result = 0;
    for (int i = 0; i < 10; i++) {
      result += x[w];
      w = i;
    }
    return result;
  }

  /// CHECK-START: int Main.periodicDynamicBCE(int[]) BCE (before)
  /// CHECK-DAG: ArrayGet    loop:<<Loop:B\d+>>
  /// CHECK-DAG: NullCheck   loop:<<Loop>>
  /// CHECK-DAG: ArrayLength loop:<<Loop>>
  /// CHECK-DAG: BoundsCheck loop:<<Loop>>
  //
  /// CHECK-START: int Main.periodicDynamicBCE(int[]) BCE (after)
  /// CHECK-DAG: ArrayGet    loop:{{B\d+}}
  /// CHECK-DAG: Deoptimize  loop:none
  //
  /// CHECK-START: int Main.periodicDynamicBCE(int[]) BCE (after)
  /// CHECK-NOT: NullCheck   loop:{{B\d+}}
  /// CHECK-NOT: ArrayLength loop:{{B\d+}}
  /// CHECK-NOT: BoundsCheck loop:{{B\d+}}
  private static int periodicDynamicBCE(int[] x) {
    int k = 0;
    int result = 0;
    for (int i = 0; i < 10; i++) {
      result += x[k];
      k = 1 - k;
    }
    return result;
  }

  /// CHECK-START: int Main.dynamicBCEPossiblyInfiniteLoop(int[], int, int) BCE (before)
  /// CHECK-DAG: ArrayGet    loop:<<Loop:B\d+>>
  /// CHECK-DAG: NullCheck   loop:<<Loop>>
  /// CHECK-DAG: ArrayLength loop:<<Loop>>
  /// CHECK-DAG: BoundsCheck loop:<<Loop>>
  //
  /// CHECK-START: int Main.dynamicBCEPossiblyInfiniteLoop(int[], int, int) BCE (after)
  /// CHECK-DAG: ArrayGet    loop:{{B\d+}}
  /// CHECK-DAG: Deoptimize  loop:none
  //
  /// CHECK-START: int Main.dynamicBCEPossiblyInfiniteLoop(int[], int, int) BCE (after)
  /// CHECK-NOT: NullCheck   loop:{{B\d+}}
  /// CHECK-NOT: ArrayLength loop:{{B\d+}}
  /// CHECK-NOT: BoundsCheck loop:{{B\d+}}
  static int dynamicBCEPossiblyInfiniteLoop(int[] x, int lo, int hi) {
    // This loop could be infinite for hi = max int. Since i is also used
    // as subscript, however, dynamic bce can proceed.
    int result = 0;
    for (int i = lo; i <= hi; i++) {
      result += x[i];
    }
    return result;
  }

  /// CHECK-START: int Main.noDynamicBCEPossiblyInfiniteLoop(int[], int, int) BCE (before)
  /// CHECK-DAG: ArrayGet    loop:<<Loop:B\d+>>
  /// CHECK-DAG: BoundsCheck loop:<<Loop>>
  //
  /// CHECK-START: int Main.noDynamicBCEPossiblyInfiniteLoop(int[], int, int) BCE (after)
  /// CHECK-DAG: ArrayGet    loop:<<Loop:B\d+>>
  /// CHECK-DAG: BoundsCheck loop:<<Loop>>
  //
  /// CHECK-START: int Main.noDynamicBCEPossiblyInfiniteLoop(int[], int, int) BCE (after)
  /// CHECK-NOT: Deoptimize
  static int noDynamicBCEPossiblyInfiniteLoop(int[] x, int lo, int hi) {
    // As above, but now the index is not used as subscript,
    // and dynamic bce is not applied.
    int result = 0;
    for (int k = 0, i = lo; i <= hi; i++) {
      result += x[k++];
    }
    return result;
  }

  /// CHECK-START: int Main.noDynamicBCEMixedInductionTypes(int[], long, long) BCE (before)
  /// CHECK-DAG: ArrayGet    loop:<<Loop:B\d+>>
  /// CHECK-DAG: BoundsCheck loop:<<Loop>>
  //
  /// CHECK-START: int Main.noDynamicBCEMixedInductionTypes(int[], long, long) BCE (after)
  /// CHECK-DAG: ArrayGet    loop:<<Loop:B\d+>>
  /// CHECK-DAG: BoundsCheck loop:<<Loop>>
  //
  /// CHECK-START: int Main.noDynamicBCEMixedInductionTypes(int[], long, long) BCE (after)
  /// CHECK-NOT: Deoptimize
  static int noDynamicBCEMixedInductionTypes(int[] x, long lo, long hi) {
    int result = 0;
    // Mix of int and long induction.
    int k = 0;
    for (long i = lo; i < hi; i++) {
      result += x[k++];
    }
    return result;
  }

  /// CHECK-START: int Main.dynamicBCEAndConstantIndices(int[], int[][], int, int) BCE (before)
  /// CHECK-DAG: {{l\d+}} ArrayGet loop:<<Loop:B\d+>>
  /// CHECK-DAG: {{l\d+}} ArrayGet loop:<<Loop>>
  /// CHECK-DAG: {{l\d+}} ArrayGet loop:<<Loop>>
  //
  /// CHECK-START: int Main.dynamicBCEAndConstantIndices(int[], int[][], int, int) BCE (after)
  //  Order matters:
  /// CHECK:              Deoptimize loop:<<Loop:B\d+>>
  //  CHECK-NOT:          Goto       loop:<<Loop>>
  /// CHECK-DAG: {{l\d+}} ArrayGet   loop:<<Loop>>
  /// CHECK-DAG: {{l\d+}} ArrayGet   loop:<<Loop>>
  /// CHECK-DAG: {{l\d+}} ArrayGet   loop:<<Loop>>
  /// CHECK:              Goto       loop:<<Loop>>
  //
  /// CHECK-START: int Main.dynamicBCEAndConstantIndices(int[], int[][], int, int) BCE (after)
  /// CHECK-DAG: Deoptimize loop:none
  static int dynamicBCEAndConstantIndices(int[] x, int[][] a, int lo, int hi) {
    // Deliberately test array length on a before the loop so that only bounds checks
    // on constant subscripts remain, making them a viable candidate for hoisting.
    if (a.length == 0) {
      return -1;
    }
    // Loop that allows BCE on x[i].
    int result = 0;
    for (int i = lo; i < hi; i++) {
      result += x[i];
      if ((i % 10) != 0) {
        // None of the subscripts inside a conditional are removed by dynamic bce,
        // making them a candidate for deoptimization based on constant indices.
        // Compiler should ensure the array loads are not subsequently hoisted
        // "above" the deoptimization "barrier" on the bounds.
        a[0][i] = 1;
        a[1][i] = 2;
        a[99][i] = 3;
      }
    }
    return result;
  }

  /// CHECK-START: int Main.dynamicBCEAndConstantIndicesAllPrimTypes(int[], boolean[], byte[], char[], short[], int[], long[], float[], double[], int, int) BCE (before)
  /// CHECK-DAG: ArrayGet    loop:<<Loop:B\d+>>
  /// CHECK-DAG: ArrayGet    loop:<<Loop>>
  /// CHECK-DAG: ArrayGet    loop:<<Loop>>
  /// CHECK-DAG: ArrayGet    loop:<<Loop>>
  /// CHECK-DAG: ArrayGet    loop:<<Loop>>
  /// CHECK-DAG: ArrayGet    loop:<<Loop>>
  /// CHECK-DAG: ArrayGet    loop:<<Loop>>
  /// CHECK-DAG: ArrayGet    loop:<<Loop>>
  /// CHECK-DAG: ArrayGet    loop:<<Loop>>
  //  For brevity, just test occurrence of at least one of each in the loop:
  /// CHECK-DAG: NullCheck   loop:<<Loop>>
  /// CHECK-DAG: ArrayLength loop:<<Loop>>
  /// CHECK-DAG: BoundsCheck loop:<<Loop>>
  //
  /// CHECK-START: int Main.dynamicBCEAndConstantIndicesAllPrimTypes(int[], boolean[], byte[], char[], short[], int[], long[], float[], double[], int, int) BCE (after)
  /// CHECK-DAG: ArrayGet    loop:<<Loop:B\d+>>
  /// CHECK-NOT: ArrayGet    loop:<<Loop>>
  //
  /// CHECK-START: int Main.dynamicBCEAndConstantIndicesAllPrimTypes(int[], boolean[], byte[], char[], short[], int[], long[], float[], double[], int, int) BCE (after)
  /// CHECK-NOT: NullCheck   loop:{{B\d+}}
  /// CHECK-NOT: ArrayLength loop:{{B\d+}}
  /// CHECK-NOT: BoundsCheck loop:{{B\d+}}
  //
  /// CHECK-START: int Main.dynamicBCEAndConstantIndicesAllPrimTypes(int[], boolean[], byte[], char[], short[], int[], long[], float[], double[], int, int) BCE (after)
  /// CHECK-DAG: Deoptimize  loop:none
  static int dynamicBCEAndConstantIndicesAllPrimTypes(int[] q,
                                                      boolean[] r,
                                                      byte[] s,
                                                      char[] t,
                                                      short[] u,
                                                      int[] v,
                                                      long[] w,
                                                      float[] x,
                                                      double[] y, int lo, int hi) {
    int result = 0;
    for (int i = lo; i < hi; i++) {
      // All constant index array references can be hoisted out of the loop during BCE on q[i].
      result += q[i] + (r[0] ? 1 : 0) + (int) s[0] + (int) t[0] + (int) u[0] + (int) v[0] +
                                        (int) w[0] + (int) x[0] + (int) y[0];
    }
    return result;
  }

  /// CHECK-START: int Main.dynamicBCEAndConstantIndexRefType(int[], java.lang.Integer[], int, int) BCE (before)
  /// CHECK-DAG: ArrayGet    loop:<<Loop:B\d+>>
  /// CHECK-DAG: NullCheck   loop:<<Loop>>
  /// CHECK-DAG: ArrayLength loop:<<Loop>>
  /// CHECK-DAG: BoundsCheck loop:<<Loop>>
  /// CHECK-DAG: ArrayGet    loop:<<Loop>>
  /// CHECK-DAG: NullCheck   loop:<<Loop>>
  /// CHECK-DAG: ArrayLength loop:<<Loop>>
  /// CHECK-DAG: BoundsCheck loop:<<Loop>>
  //
  /// CHECK-START: int Main.dynamicBCEAndConstantIndexRefType(int[], java.lang.Integer[], int, int) BCE (after)
  /// CHECK-DAG: ArrayGet    loop:<<Loop:B\d+>>
  /// CHECK-DAG: Deoptimize  loop:none
  //
  /// CHECK-START: int Main.dynamicBCEAndConstantIndexRefType(int[], java.lang.Integer[], int, int) BCE (after)
  /// CHECK-NOT: ArrayLength loop:{{B\d+}}
  /// CHECK-NOT: BoundsCheck loop:{{B\d+}}
  static int dynamicBCEAndConstantIndexRefType(int[] q, Integer[] z, int lo, int hi) {
    int result = 0;
    for (int i = lo; i < hi; i++) {
      // Similar to above, but now implicit call to intValue() may prevent hoisting
      // z[0] itself during BCE on q[i]. Therefore, we just check BCE on q[i].
      result += q[i] + z[0];
    }
    return result;
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
    expectEquals(0, linearVeryObscure(empty));
    expectEquals(55, linearVeryObscure(x));
    expectEquals(0, hiddenStride(empty));
    expectEquals(55, hiddenStride(x));
    expectEquals(0, linearWhile(empty));
    expectEquals(55, linearWhile(x));
    expectEquals(0, linearThreeWayPhi(empty));
    expectEquals(50, linearThreeWayPhi(x));
    expectEquals(0, linearFourWayPhi(empty));
    expectEquals(51, linearFourWayPhi(x));
    expectEquals(0, wrapAroundThenLinear(empty));
    expectEquals(55, wrapAroundThenLinear(x));
    expectEquals(0, wrapAroundThenLinearThreeWayPhi(empty));
    expectEquals(54, wrapAroundThenLinearThreeWayPhi(x));

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

    // Linear copy.
    expectEquals(0, linearCopy(empty).length);
    {
      int[] r = linearCopy(x);
      expectEquals(x.length, r.length);
      for (int i = 0; i < x.length; i++) {
        expectEquals(x[i], r[i]);
      }
    }

    // Linear with non-unit strides.
    expectEquals(55, linearByTwo(x));
    expectEquals(25, linearByTwoSkip1(x));
    expectEquals(25, linearByTwoSkip2(x));
    expectEquals(56, linearWithCompoundStride());
    expectEquals(66, linearWithLargePositiveStride());
    expectEquals(66, linearWithVeryLargePositiveStride());
    expectEquals(66, linearWithLargeNegativeStride());
    expectEquals(66, linearWithVeryLargeNegativeStride());

    // Special forms.
    expectEquals(55, linearForNEUp());
    expectEquals(55, linearForNEDown());
    expectEquals(55, linearDoWhileUp());
    expectEquals(55, linearDoWhileDown());
    expectEquals(55, linearShort());
    expectEquals(55, invariantFromPreLoop(x, 1));
    linearTriangularOnTwoArrayLengths(10);
    linearTriangularOnOneArrayLength(10);
    linearTriangularOnParameter(10);
    linearTriangularVariations(10);

    // Sorting.
    int[] sort = { 5, 4, 1, 9, 10, 2, 7, 6, 3, 8 };
    bubble(sort);
    for (int i = 0; i < 10; i++) {
      expectEquals(sort[i], x[i]);
    }

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

    // Large bounds.
    expectEquals(55, justRightUp1());
    expectEquals(55, justRightUp2());
    expectEquals(55, justRightUp3());
    expectEquals(55, justRightDown1());
    expectEquals(55, justRightDown2());
    expectEquals(55, justRightDown3());
    sResult = 0;
    try {
      justOOBUp();
    } catch (ArrayIndexOutOfBoundsException e) {
      sResult = 1;
    }
    expectEquals(1, sResult);
    sResult = 0;
    try {
      justOOBDown();
    } catch (ArrayIndexOutOfBoundsException e) {
      sResult = 1;
    }
    expectEquals(1, sResult);

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

    // Do while up goes OOB.
    sResult = 0;
    try {
      doWhileUpOOB();
    } catch (ArrayIndexOutOfBoundsException e) {
      sResult += 1000;
    }
    expectEquals(1055, sResult);

    // Do while down goes OOB.
    sResult = 0;
    try {
      doWhileDownOOB();
    } catch (ArrayIndexOutOfBoundsException e) {
      sResult += 1000;
    }
    expectEquals(1055, sResult);

    // Multiplication.
    {
      int[] e1 = { 7, 1, 2, 2, 1, 0, 2, 0, 0, 1 };
      int[] a1 = multiply1();
      for (int i = 0; i < 10; i++) {
        expectEquals(a1[i], e1[i]);
      }
      int[] e2 = { 1001, 0, 0, 1, 0, 0, 1, 0, 0, 1 };
      int[] a2 = multiply2();
      for (int i = 0; i < 10; i++) {
        expectEquals(a2[i], e2[i]);
      }
    }

    // Dynamic BCE.
    sResult = 0;
    try {
      linearDynamicBCE1(x, -1, x.length);
    } catch (ArrayIndexOutOfBoundsException e) {
      sResult += 1000;
    }
    expectEquals(1000, sResult);
    sResult = 0;
    linearDynamicBCE1(x, 0, x.length);
    expectEquals(55, sResult);
    sResult = 0;
    try {
      linearDynamicBCE1(x, 0, x.length + 1);
    } catch (ArrayIndexOutOfBoundsException e) {
      sResult += 1000;
    }
    expectEquals(1055, sResult);

    // Dynamic BCE with offset.
    sResult = 0;
    try {
      linearDynamicBCE2(x, 0, x.length, -1);
    } catch (ArrayIndexOutOfBoundsException e) {
      sResult += 1000;
    }
    expectEquals(1000, sResult);
    sResult = 0;
    linearDynamicBCE2(x, 0, x.length, 0);
    expectEquals(55, sResult);
    sResult = 0;
    try {
      linearDynamicBCE2(x, 0, x.length, 1);
    } catch (ArrayIndexOutOfBoundsException e) {
      sResult += 1000;
    }
    expectEquals(1054, sResult);

    // Dynamic BCE candidates.
    expectEquals(55, wrapAroundDynamicBCE(x));
    expectEquals(15, periodicDynamicBCE(x));
    expectEquals(55, dynamicBCEPossiblyInfiniteLoop(x, 0, 9));
    expectEquals(55, noDynamicBCEPossiblyInfiniteLoop(x, 0, 9));
    expectEquals(55, noDynamicBCEMixedInductionTypes(x, 0, 10));

    // Dynamic BCE combined with constant indices.
    int[][] a;
    a = new int[0][0];
    expectEquals(-1, dynamicBCEAndConstantIndices(x, a, 0, 10));
    a = new int[100][10];
    expectEquals(55, dynamicBCEAndConstantIndices(x, a, 0, 10));
    for (int i = 0; i < 10; i++) {
      expectEquals((i % 10) != 0 ? 1 : 0, a[0][i]);
      expectEquals((i % 10) != 0 ? 2 : 0, a[1][i]);
      expectEquals((i % 10) != 0 ? 3 : 0, a[99][i]);
    }
    a = new int[2][10];
    sResult = 0;
    try {
      expectEquals(55, dynamicBCEAndConstantIndices(x, a, 0, 10));
    } catch (ArrayIndexOutOfBoundsException e) {
      sResult = 1;
    }
    expectEquals(1, sResult);
    expectEquals(a[0][1], 1);
    expectEquals(a[1][1], 2);

    // Dynamic BCE combined with constant indices of all types.
    boolean[] x1 = { true };
    byte[] x2 = { 2 };
    char[] x3 = { 3 };
    short[] x4 = { 4 };
    int[] x5 = { 5 };
    long[] x6 = { 6 };
    float[] x7 = { 7 };
    double[] x8 = { 8 };
    expectEquals(415,
        dynamicBCEAndConstantIndicesAllPrimTypes(x, x1, x2, x3, x4, x5, x6, x7, x8, 0, 10));
    Integer[] x9 = { 9 };
    expectEquals(145, dynamicBCEAndConstantIndexRefType(x, x9, 0, 10));
  }

  private static void expectEquals(int expected, int result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }
}
