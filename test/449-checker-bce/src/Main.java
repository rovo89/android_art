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

public class Main {

  // CHECK-START: int Main.sieve(int) BCE (before)
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArrayGet
  // CHECK: BoundsCheck
  // CHECK: ArraySet

  // CHECK-START: int Main.sieve(int) BCE (after)
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArrayGet
  // CHECK: BoundsCheck
  // CHECK: ArraySet

  static int sieve(int size) {
    int primeCount = 0;
    boolean[] flags = new boolean[size + 1];
    for (int i = 1; i < size; i++) flags[i] = true; // Can eliminate.
    for (int i = 2; i < size; i++) {
      if (flags[i]) { // Can eliminate.
        primeCount++;
        for (int k = i + 1; k <= size; k += i)
          flags[k - 1] = false; // Can't eliminate yet due to (k+i) may overflow.
      }
    }
    return primeCount;
  }


  // CHECK-START: void Main.narrow(int[], int) BCE (before)
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet

  // CHECK-START: void Main.narrow(int[], int) BCE (after)
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet

  static void narrow(int[] array, int offset) {
    if (offset < 0) {
      return;
    }
    if (offset < array.length) {
      // offset is in range [0, array.length-1].
      // Bounds check can be eliminated.
      array[offset] = 1;

      int biased_offset1 = offset + 1;
      // biased_offset1 is in range [1, array.length].
      if (biased_offset1 < array.length) {
        // biased_offset1 is in range [1, array.length-1].
        // Bounds check can be eliminated.
        array[biased_offset1] = 1;
      }

      int biased_offset2 = offset + 0x70000000;
      // biased_offset2 is in range [0x70000000, array.length-1+0x70000000].
      // It may overflow and be negative.
      if (biased_offset2 < array.length) {
        // Even with this test, biased_offset2 can be negative so we can't
        // eliminate this bounds check.
        array[biased_offset2] = 1;
      }

      // offset_sub1 won't underflow since offset is no less than 0.
      int offset_sub1 = offset - Integer.MAX_VALUE;
      if (offset_sub1 >= 0) {
        array[offset_sub1] = 1;  // Bounds check can be eliminated.
      }

      // offset_sub2 can underflow.
      int offset_sub2 = offset_sub1 - Integer.MAX_VALUE;
      if (offset_sub2 >= 0) {
        array[offset_sub2] = 1;  // Bounds check can't be eliminated.
      }
    }
  }


  // CHECK-START: void Main.constantIndexing1(int[]) BCE (before)
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet

  // CHECK-START: void Main.constantIndexing1(int[]) BCE (after)
  // CHECK-NOT: Deoptimize
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet

  static void constantIndexing1(int[] array) {
    array[5] = 1;
    array[4] = 1;
  }


  // CHECK-START: void Main.constantIndexing2(int[]) BCE (before)
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet

  // CHECK-START: void Main.constantIndexing2(int[]) BCE (after)
  // CHECK: LessThanOrEqual
  // CHECK: Deoptimize
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet

  static void constantIndexing2(int[] array) {
    array[1] = 1;
    array[2] = 1;
    array[3] = 1;
    array[4] = 1;
    array[-1] = 1;
  }


  // CHECK-START: int[] Main.constantIndexing3(int[], int[], boolean) BCE (before)
  // CHECK: BoundsCheck
  // CHECK: ArrayGet
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArrayGet
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArrayGet
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArrayGet
  // CHECK: BoundsCheck
  // CHECK: ArraySet

  // CHECK-START: int[] Main.constantIndexing3(int[], int[], boolean) BCE (after)
  // CHECK: LessThanOrEqual
  // CHECK: Deoptimize
  // CHECK-NOT: BoundsCheck
  // CHECK: ArrayGet
  // CHECK: LessThanOrEqual
  // CHECK: Deoptimize
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArrayGet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArrayGet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArrayGet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet

  static int[] constantIndexing3(int[] array1, int[] array2, boolean copy) {
    if (!copy) {
      return array1;
    }
    array2[0] = array1[0];
    array2[1] = array1[1];
    array2[2] = array1[2];
    array2[3] = array1[3];
    return array2;
  }


  // CHECK-START: void Main.constantIndexing4(int[]) BCE (before)
  // CHECK: BoundsCheck
  // CHECK: ArraySet

  // CHECK-START: void Main.constantIndexing4(int[]) BCE (after)
  // CHECK-NOT: LessThanOrEqual
  // CHECK: BoundsCheck
  // CHECK: ArraySet

  // There is only one array access. It's not beneficial
  // to create a compare with deoptimization instruction.
  static void constantIndexing4(int[] array) {
    array[0] = 1;
  }


  // CHECK-START: void Main.constantIndexing5(int[]) BCE (before)
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet

  // CHECK-START: void Main.constantIndexing5(int[]) BCE (after)
  // CHECK-NOT: Deoptimize
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet

  static void constantIndexing5(int[] array) {
    // We don't apply the deoptimization for very large constant index
    // since it's likely to be an anomaly and will throw AIOOBE.
    array[Integer.MAX_VALUE - 1000] = 1;
    array[Integer.MAX_VALUE - 999] = 1;
    array[Integer.MAX_VALUE - 998] = 1;
  }

  // CHECK-START: void Main.loopPattern1(int[]) BCE (before)
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet

  // CHECK-START: void Main.loopPattern1(int[]) BCE (after)
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet

  static void loopPattern1(int[] array) {
    for (int i = 0; i < array.length; i++) {
      array[i] = 1;  // Bounds check can be eliminated.
    }

    for (int i = 1; i < array.length; i++) {
      array[i] = 1;  // Bounds check can be eliminated.
    }

    for (int i = 1; i < array.length - 1; i++) {
      array[i] = 1;  // Bounds check can be eliminated.
    }

    for (int i = -1; i < array.length; i++) {
      array[i] = 1;  // Bounds check can't be eliminated.
    }

    for (int i = 0; i <= array.length; i++) {
      array[i] = 1;  // Bounds check can't be eliminated.
    }

    for (int i = 0; i < array.length; i += 2) {
      // We don't have any assumption on max array length yet.
      // Bounds check can't be eliminated due to overflow concern.
      array[i] = 1;
    }

    for (int i = 1; i < array.length; i += 2) {
      // Bounds check can be eliminated since i is odd so the last
      // i that's less than array.length is at most (Integer.MAX_VALUE - 2).
      array[i] = 1;
    }
  }


  // CHECK-START: void Main.loopPattern2(int[]) BCE (before)
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet

  // CHECK-START: void Main.loopPattern2(int[]) BCE (after)
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet

  static void loopPattern2(int[] array) {
    for (int i = array.length - 1; i >= 0; i--) {
      array[i] = 1;  // Bounds check can be eliminated.
    }

    for (int i = array.length; i > 0; i--) {
      array[i - 1] = 1;  // Bounds check can be eliminated.
    }

    for (int i = array.length - 1; i > 0; i--) {
      array[i] = 1;  // Bounds check can be eliminated.
    }

    for (int i = array.length; i >= 0; i--) {
      array[i] = 1;  // Bounds check can't be eliminated.
    }

    for (int i = array.length; i >= 0; i--) {
      array[i - 1] = 1;  // Bounds check can't be eliminated.
    }

    for (int i = array.length; i > 0; i -= 20) {
      // For i >= 0, (i - 20 - 1) is guaranteed not to underflow.
      array[i - 1] = 1;  // Bounds check can be eliminated.
    }
  }


  // CHECK-START: void Main.loopPattern3(int[]) BCE (before)
  // CHECK: BoundsCheck
  // CHECK: ArraySet

  // CHECK-START: void Main.loopPattern3(int[]) BCE (after)
  // CHECK: BoundsCheck
  // CHECK: ArraySet

  static void loopPattern3(int[] array) {
    java.util.Random random = new java.util.Random();
    for (int i = 0; ; i++) {
      if (random.nextInt() % 1000 == 0 && i < array.length) {
        // Can't eliminate the bound check since not every i++ is
        // matched with a array length check, so there is some chance that i
        // overflows and is negative.
        array[i] = 1;
      }
    }
  }


  // CHECK-START: void Main.constantNewArray() BCE (before)
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet

  // CHECK-START: void Main.constantNewArray() BCE (after)
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet

  static void constantNewArray() {
    int[] array = new int[10];
    for (int i = 0; i < 10; i++) {
      array[i] = 1;  // Bounds check can be eliminated.
    }

    for (int i = 0; i <= 10; i++) {
      array[i] = 1;  // Bounds check can't be eliminated.
    }

    array[0] = 1;  // Bounds check can be eliminated.
    array[9] = 1;  // Bounds check can be eliminated.
    array[10] = 1; // Bounds check can't be eliminated.
  }


  static byte readData() {
    return 1;
  }

  // CHECK-START: void Main.circularBufferProducer() BCE (before)
  // CHECK: BoundsCheck
  // CHECK: ArraySet

  // CHECK-START: void Main.circularBufferProducer() BCE (after)
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet

  static void circularBufferProducer() {
    byte[] array = new byte[4096];
    int i = 0;
    while (true) {
      array[i & (array.length - 1)] = readData();
      i++;
    }
  }


  // CHECK-START: void Main.pyramid1(int[]) BCE (before)
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet

  // CHECK-START: void Main.pyramid1(int[]) BCE (after)
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet

  // Set array to something like {0, 1, 2, 3, 2, 1, 0}.
  static void pyramid1(int[] array) {
    for (int i = 0; i < (array.length + 1) / 2; i++) {
      array[i] = i;
      array[array.length - 1 - i] = i;
    }
  }


  // CHECK-START: void Main.pyramid2(int[]) BCE (before)
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet

  // CHECK-START: void Main.pyramid2(int[]) BCE (after)
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet

  // Set array to something like {0, 1, 2, 3, 2, 1, 0}.
  static void pyramid2(int[] array) {
    for (int i = 0; i < (array.length + 1) >> 1; i++) {
      array[i] = i;
      array[array.length - 1 - i] = i;
    }
  }


  // CHECK-START: void Main.pyramid3(int[]) BCE (before)
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet

  // CHECK-START: void Main.pyramid3(int[]) BCE (after)
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet

  // Set array to something like {0, 1, 2, 3, 2, 1, 0}.
  static void pyramid3(int[] array) {
    for (int i = 0; i < (array.length + 1) >>> 1; i++) {
      array[i] = i;
      array[array.length - 1 - i] = i;
    }
  }


  // CHECK-START: boolean Main.isPyramid(int[]) BCE (before)
  // CHECK: BoundsCheck
  // CHECK: ArrayGet
  // CHECK: BoundsCheck
  // CHECK: ArrayGet

  // CHECK-START: boolean Main.isPyramid(int[]) BCE (after)
  // CHECK-NOT: BoundsCheck
  // CHECK: ArrayGet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArrayGet

  static boolean isPyramid(int[] array) {
    int i = 0;
    int j = array.length - 1;
    while (i <= j) {
      if (array[i] != i) {
        return false;
      }
      if (array[j] != i) {
        return false;
      }
      i++; j--;
    }
    return true;
  }


  // CHECK-START: void Main.bubbleSort(int[]) GVN (before)
  // CHECK: BoundsCheck
  // CHECK: ArrayGet
  // CHECK: BoundsCheck
  // CHECK: ArrayGet
  // CHECK: BoundsCheck
  // CHECK: ArrayGet
  // CHECK: BoundsCheck
  // CHECK: ArrayGet
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArraySet

  // CHECK-START: void Main.bubbleSort(int[]) GVN (after)
  // CHECK: BoundsCheck
  // CHECK: ArrayGet
  // CHECK: BoundsCheck
  // CHECK: ArrayGet
  // CHECK-NOT: ArrayGet
  // CHECK-NOT: ArrayGet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet

  // CHECK-START: void Main.bubbleSort(int[]) BCE (after)
  // CHECK-NOT: BoundsCheck
  // CHECK: ArrayGet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArrayGet
  // CHECK-NOT: ArrayGet
  // CHECK-NOT: ArrayGet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet

  static void bubbleSort(int[] array) {
    for (int i = 0; i < array.length - 1; i++) {
      for (int j = 0; j < array.length - i - 1; j++) {
        if (array[j] > array[j + 1]) {
          int temp = array[j + 1];
          array[j + 1] = array[j];
          array[j] = temp;
        }
      }
    }
  }


  static int foo() {
    try {
      // This will cause AIOOBE.
      constantIndexing2(new int[3]);
    } catch (ArrayIndexOutOfBoundsException e) {
      return 99;
    }
    return 0;
  }


  int sum;

  // CHECK-START: void Main.foo1(int[], int, int) BCE (before)
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArrayGet

  // CHECK-START: void Main.foo1(int[], int, int) BCE (after)
  // CHECK: Phi
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArrayGet
  //  Added blocks for deoptimization.
  // CHECK: If
  // CHECK: Goto
  // CHECK: Deoptimize
  // CHECK: Deoptimize
  // CHECK: Deoptimize
  // CHECK-NOT: Deoptimize
  // CHECK: Goto
  // CHECK: Phi
  // CHECK: Goto

  void foo1(int[] array, int start, int end) {
    // Three HDeoptimize will be added. One for
    // start >= 0, one for end <= array.length,
    // and one for null check on array (to hoist null
    // check and array.length out of loop).
    for (int i = start ; i < end; i++) {
      array[i] = 1;
      sum += array[i];
    }
  }


  // CHECK-START: void Main.foo2(int[], int, int) BCE (before)
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArrayGet

  // CHECK-START: void Main.foo2(int[], int, int) BCE (after)
  // CHECK: Phi
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArrayGet
  //  Added blocks for deoptimization.
  // CHECK: If
  // CHECK: Goto
  // CHECK: Deoptimize
  // CHECK: Deoptimize
  // CHECK: Deoptimize
  // CHECK-NOT: Deoptimize
  // CHECK: Goto
  // CHECK: Phi
  // CHECK: Goto

  void foo2(int[] array, int start, int end) {
    // Three HDeoptimize will be added. One for
    // start >= 0, one for end <= array.length,
    // and one for null check on array (to hoist null
    // check and array.length out of loop).
    for (int i = start ; i <= end; i++) {
      array[i] = 1;
      sum += array[i];
    }
  }


  // CHECK-START: void Main.foo3(int[], int) BCE (before)
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArrayGet

  // CHECK-START: void Main.foo3(int[], int) BCE (after)
  // CHECK: Phi
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArrayGet
  //  Added blocks for deoptimization.
  // CHECK: If
  // CHECK: Goto
  // CHECK: Deoptimize
  // CHECK: Deoptimize
  // CHECK-NOT: Deoptimize
  // CHECK: Goto
  // CHECK: Phi
  // CHECK: Goto

  void foo3(int[] array, int end) {
    // Two HDeoptimize will be added. One for end < array.length,
    // and one for null check on array (to hoist null check
    // and array.length out of loop).
    for (int i = 3 ; i <= end; i++) {
      array[i] = 1;
      sum += array[i];
    }
  }


  // CHECK-START: void Main.foo4(int[], int) BCE (before)
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArrayGet

  // CHECK-START: void Main.foo4(int[], int) BCE (after)
  // CHECK: Phi
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArrayGet
  //  Added blocks for deoptimization.
  // CHECK: If
  // CHECK: Goto
  // CHECK: Deoptimize
  // CHECK: Deoptimize
  // CHECK-NOT: Deoptimize
  // CHECK: Goto
  // CHECK: Phi
  // CHECK: Goto

  void foo4(int[] array, int end) {
    // Two HDeoptimize will be added. One for end <= array.length,
    // and one for null check on array (to hoist null check
    // and array.length out of loop).
    for (int i = end ; i > 0; i--) {
      array[i - 1] = 1;
      sum += array[i - 1];
    }
  }


  // CHECK-START: void Main.foo5(int[], int) BCE (before)
  // CHECK: BoundsCheck
  // CHECK: ArraySet
  // CHECK: BoundsCheck
  // CHECK: ArrayGet
  // CHECK: BoundsCheck
  // CHECK: ArrayGet
  // CHECK: BoundsCheck
  // CHECK: ArrayGet

  // CHECK-START: void Main.foo5(int[], int) BCE (after)
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet
  // CHECK: Phi
  // CHECK-NOT: BoundsCheck
  // CHECK: ArrayGet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArrayGet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArrayGet
  //  Added blocks for deoptimization.
  // CHECK: If
  // CHECK: Goto
  // CHECK: Deoptimize
  // CHECK-NOT: Deoptimize
  // CHECK: Goto
  //  array.length is defined before the loop header so no phi is needed.
  // CHECK-NOT: Phi
  // CHECK: Goto

  void foo5(int[] array, int end) {
    // Bounds check in this loop can be eliminated without deoptimization.
    for (int i = array.length - 1 ; i >= 0; i--) {
      array[i] = 1;
    }
    // One HDeoptimize will be added.
    // It's for (end - 2 <= array.length - 2).
    for (int i = end - 2 ; i > 0; i--) {
      sum += array[i - 1];
      sum += array[i];
      sum += array[i + 1];
    }
  }


  // CHECK-START: void Main.foo6(int[], int, int) BCE (before)
  // CHECK: BoundsCheck
  // CHECK: ArrayGet
  // CHECK: BoundsCheck
  // CHECK: ArrayGet
  // CHECK: BoundsCheck
  // CHECK: ArrayGet
  // CHECK: BoundsCheck
  // CHECK: ArrayGet
  // CHECK: BoundsCheck
  // CHECK: ArrayGet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet

  // CHECK-START: void Main.foo6(int[], int, int) BCE (after)
  // CHECK: Phi
  // CHECK-NOT: BoundsCheck
  // CHECK: ArrayGet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArrayGet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArrayGet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArrayGet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArrayGet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet
  //  Added blocks for deoptimization.
  // CHECK: If
  // CHECK: Goto
  // CHECK: Deoptimize
  // CHECK: Deoptimize
  // CHECK: Deoptimize
  // CHECK-NOT: Deoptimize
  // CHECK: Goto
  // CHECK: Phi
  // CHECK: Goto
  // CHECK-NOT: Deoptimize

  void foo6(int[] array, int start, int end) {
    // Three HDeoptimize will be added. One for
    // start >= 2, one for end <= array.length - 3,
    // and one for null check on array (to hoist null
    // check and array.length out of loop).
    for (int i = end; i >= start; i--) {
      array[i] = (array[i-2] + array[i-1] + array[i] + array[i+1] + array[i+2]) / 5;
    }
  }


  // CHECK-START: void Main.foo7(int[], int, int, boolean) BCE (before)
  // CHECK: BoundsCheck
  // CHECK: ArrayGet
  // CHECK: BoundsCheck
  // CHECK: ArrayGet

  // CHECK-START: void Main.foo7(int[], int, int, boolean) BCE (after)
  // CHECK: Phi
  // CHECK: BoundsCheck
  // CHECK: ArrayGet
  // CHECK-NOT: BoundsCheck
  // CHECK: ArrayGet
  //  Added blocks for deoptimization.
  // CHECK: If
  // CHECK: Goto
  // CHECK: Deoptimize
  // CHECK: Deoptimize
  // CHECK: Deoptimize
  // CHECK-NOT: Deoptimize
  // CHECK: Goto
  // CHECK: Phi
  // CHECK: Goto

  void foo7(int[] array, int start, int end, boolean lowEnd) {
    // Three HDeoptimize will be added. One for
    // start >= 0, one for end <= array.length,
    // and one for null check on array (to hoist null
    // check and array.length out of loop).
    for (int i = start ; i < end; i++) {
      if (lowEnd) {
        // This array access isn't certain. So we don't
        // use +1000 offset in decision making for deoptimization
        // conditions.
        sum += array[i + 1000];
      }
      sum += array[i];
    }
  }


  // CHECK-START: void Main.foo8(int[][], int, int) BCE (before)
  // CHECK: BoundsCheck
  // CHECK: ArrayGet
  // CHECK: BoundsCheck
  // CHECK: ArraySet

  // CHECK-START: void Main.foo8(int[][], int, int) BCE (after)
  // CHECK: Phi
  // CHECK-NOT: BoundsCheck
  // CHECK: ArrayGet
  // CHECK: Phi
  // CHECK-NOT: BoundsCheck
  // CHECK: ArraySet
  //  Added blocks for deoptimization.
  // CHECK: If
  // CHECK: Goto
  // CHECK: Deoptimize
  // CHECK: Deoptimize
  // CHECK: Deoptimize
  // CHECK: Deoptimize
  // CHECK: Deoptimize
  // CHECK: Deoptimize
  // CHECK-NOT: Deoptimize
  // CHECK: Goto
  // CHECK: Phi
  // CHECK: Goto

  void foo8(int[][] matrix, int start, int end) {
    // Three HDeoptimize will be added for the outer loop.
    // start >= 0, end <= matrix.length, and null check on matrix.
    // Three HDeoptimize will be added for the inner loop
    // start >= 0 (TODO: this may be optimized away),
    // end <= row.length, and null check on row.
    for (int i = start; i < end; i++) {
      int[] row = matrix[i];
      for (int j = start; j < end; j++) {
        row[j] = 1;
      }
    }
  }


  // CHECK-START: void Main.foo9(int[]) BCE (before)
  // CHECK: NullCheck
  // CHECK: BoundsCheck
  // CHECK: ArrayGet

  // CHECK-START: void Main.foo9(int[]) BCE (after)
  //  The loop is guaranteed to be entered. No need to transform the
  //  loop for loop body entry test.
  // CHECK: Deoptimize
  // CHECK: Deoptimize
  // CHECK-NOT: Deoptimize
  // CHECK: Phi
  // CHECK-NOT: NullCheck
  // CHECK-NOT: BoundsCheck
  // CHECK: ArrayGet

  void foo9(int[] array) {
    // Two HDeoptimize will be added. One for
    // 10 <= array.length, and one for null check on array.
    for (int i = 0 ; i < 10; i++) {
      sum += array[i];
    }
  }


  // CHECK-START: void Main.partialLooping(int[], int, int) BCE (before)
  // CHECK: BoundsCheck
  // CHECK: ArraySet

  // CHECK-START: void Main.partialLooping(int[], int, int) BCE (after)
  // CHECK-NOT: Deoptimize
  // CHECK: BoundsCheck
  // CHECK: ArraySet

  void partialLooping(int[] array, int start, int end) {
    // This loop doesn't cover the full range of [start, end) so
    // adding deoptimization is too aggressive, since end can be
    // greater than array.length but the loop is never going to work on
    // more than 2 elements.
    for (int i = start; i < end; i++) {
      if (i == 2) {
        return;
      }
      array[i] = 1;
    }
  }


  static void testUnknownBounds() {
    boolean caught = false;
    Main main = new Main();
    main.foo1(new int[10], 0, 10);
    if (main.sum != 10) {
      System.out.println("foo1 failed!");
    }

    caught = false;
    main = new Main();
    try {
      main.foo1(new int[10], 0, 11);
    } catch (ArrayIndexOutOfBoundsException e) {
      caught = true;
    }
    if (!caught || main.sum != 10) {
      System.out.println("foo1 exception failed!");
    }

    main = new Main();
    main.foo2(new int[10], 0, 9);
    if (main.sum != 10) {
      System.out.println("foo2 failed!");
    }

    caught = false;
    main = new Main();
    try {
      main.foo2(new int[10], 0, 10);
    } catch (ArrayIndexOutOfBoundsException e) {
      caught = true;
    }
    if (!caught || main.sum != 10) {
      System.out.println("foo2 exception failed!");
    }

    main = new Main();
    main.foo3(new int[10], 9);
    if (main.sum != 7) {
      System.out.println("foo3 failed!");
    }

    caught = false;
    main = new Main();
    try {
      main.foo3(new int[10], 10);
    } catch (ArrayIndexOutOfBoundsException e) {
      caught = true;
    }
    if (!caught || main.sum != 7) {
      System.out.println("foo3 exception failed!");
    }

    main = new Main();
    main.foo4(new int[10], 10);
    if (main.sum != 10) {
      System.out.println("foo4 failed!");
    }

    caught = false;
    main = new Main();
    try {
      main.foo4(new int[10], 11);
    } catch (ArrayIndexOutOfBoundsException e) {
      caught = true;
    }
    if (!caught || main.sum != 0) {
      System.out.println("foo4 exception failed!");
    }

    main = new Main();
    main.foo5(new int[10], 10);
    if (main.sum != 24) {
      System.out.println("foo5 failed!");
    }

    caught = false;
    main = new Main();
    try {
      main.foo5(new int[10], 11);
    } catch (ArrayIndexOutOfBoundsException e) {
      caught = true;
    }
    if (!caught || main.sum != 2) {
      System.out.println("foo5 exception failed!");
    }

    main = new Main();
    main.foo6(new int[10], 2, 7);

    main = new Main();
    int[] array9 = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    main.foo9(array9);
    if (main.sum != 45) {
      System.out.println("foo9 failed!");
    }

    main = new Main();
    int[] array = new int[4];
    main.partialLooping(new int[3], 0, 4);
    if ((array[0] != 1) && (array[1] != 1) &&
        (array[2] != 0) && (array[3] != 0)) {
      System.out.println("partialLooping failed!");
    }

    caught = false;
    main = new Main();
    try {
      main.foo6(new int[10], 2, 8);
    } catch (ArrayIndexOutOfBoundsException e) {
      caught = true;
    }
    if (!caught) {
      System.out.println("foo6 exception failed!");
    }

    caught = false;
    main = new Main();
    try {
      main.foo6(new int[10], 1, 7);
    } catch (ArrayIndexOutOfBoundsException e) {
      caught = true;
    }
    if (!caught) {
      System.out.println("foo6 exception failed!");
    }

  }

  public void testExceptionMessage() {
    short[] B1 = new short[5];
    int[] B2 = new int[5];
    Exception err = null;
    try {
      testExceptionMessage1(B1, B2, null, -1, 6);
    } catch (Exception e) {
      err = e;
    }
    System.out.println(err);
  }

  void testExceptionMessage1(short[] a1, int[] a2, long a3[], int start, int finish) {
    int j = finish + 77;
    // Bug: 22665511
    // A deoptimization will be triggered here right before the loop. Need to make
    // sure the value of j is preserved for the interpreter.
    for (int i = start; i <= finish; i++) {
      a2[j - 1] = a1[i + 1];
    }
  }

  // Make sure this method is compiled with optimizing.
  // CHECK-START: void Main.main(java.lang.String[]) register (after)
  // CHECK: ParallelMove

  public static void main(String[] args) {
    sieve(20);

    int[] array = {5, 2, 3, 7, 0, 1, 6, 4};
    bubbleSort(array);
    for (int i = 0; i < 8; i++) {
      if (array[i] != i) {
        System.out.println("bubble sort failed!");
      }
    }

    array = new int[7];
    pyramid1(array);
    if (!isPyramid(array)) {
      System.out.println("pyramid1 failed!");
    }

    array = new int[8];
    pyramid2(array);
    if (!isPyramid(array)) {
      System.out.println("pyramid2 failed!");
    }

    java.util.Arrays.fill(array, -1);
    pyramid3(array);
    if (!isPyramid(array)) {
      System.out.println("pyramid3 failed!");
    }

    // Make sure this value is kept after deoptimization.
    int i = 1;
    if (foo() + i != 100) {
      System.out.println("foo failed!");
    };

    testUnknownBounds();
    new Main().testExceptionMessage();
  }

}
