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

  static void narrow(int array[], int offset) {
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
    }
  }

  public static void main(String[] args) {
    sieve(20);
  }
}
