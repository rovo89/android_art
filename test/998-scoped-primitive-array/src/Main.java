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
  public Main() {}

  // Measure adds the first and last element of the array by using ScopedPrimitiveArray.
  static native long measureByteArray(long reps, byte[] arr);
  static native long measureShortArray(long reps, short[] arr);
  static native long measureIntArray(long reps, int[] arr);
  static native long measureLongArray(long reps, long[] arr);

  static void checkEq(long expected, long value) {
    if (expected != value) {
      System.out.println("error: Expected " + expected + " but got " + value);
    }
  }

  static void runPerfTest(long reps) {
    for (int length = 1; length <= 8192; length *= 8) {
      byte[] bytes = new byte[length];
      bytes[0] = 1;
      bytes[length - 1] = 2;
      short[] shorts = new short[length];
      shorts[0] = 1;
      shorts[length - 1] = 2;
      int[] ints = new int[length];
      ints[0] = 1;
      ints[length - 1] = 2;
      long[] longs = new long[length];
      longs[0] = 1;
      longs[length - 1] = 2;
      long value = 0;
      long elapsed = 0;
      long start = 0;

      start = System.nanoTime();
      value = measureByteArray(reps, bytes);
      elapsed = System.nanoTime() - start;
      System.out.println("Byte length=" + length + " ns/op=" + (double) elapsed / reps);
      checkEq(value, reps * (long) (bytes[0] + bytes[length - 1]));

      start = System.nanoTime();
      value = measureShortArray(reps, shorts);
      elapsed = System.nanoTime() - start;
      System.out.println("Short length=" + length + " ns/op=" + (double) elapsed / reps);
      checkEq(value, reps * (long) (shorts[0] + shorts[length - 1]));

      start = System.nanoTime();
      value = measureIntArray(reps, ints);
      elapsed = System.nanoTime() - start;
      System.out.println("Int length=" + length + " ns/op=" + (double) elapsed / reps);
      checkEq(value, reps * (ints[0] + ints[length - 1]));

      start = System.nanoTime();
      value = measureLongArray(reps, longs);
      elapsed = System.nanoTime() - start;
      System.out.println("Long length=" + length + " ns/op=" + (double) elapsed / reps);
      checkEq(value, reps * (longs[0] + longs[length - 1]));
    }
  }

  public static void main(String[] args) {
    System.loadLibrary(args[0]);
    long iterations = 2000000;
    if (args.length > 1) {
      iterations = Long.parseLong(args[1], 10);
    }
    runPerfTest(iterations);
    System.out.println("Done");
  }
}
