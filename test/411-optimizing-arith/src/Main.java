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
    mul();
  }

  public static void mul() {
    expectEquals(15, $opt$Mul(5, 3));
    expectEquals(0, $opt$Mul(0, 3));
    expectEquals(0, $opt$Mul(3, 0));
    expectEquals(-3, $opt$Mul(1, -3));
    expectEquals(36, $opt$Mul(-12, -3));
    expectEquals(33, $opt$Mul(1, 3) * 11);
    expectEquals(671088645, $opt$Mul(134217729, 5)); // (2^27 + 1) * 5

    expectEquals(15L, $opt$Mul(5L, 3L));
    expectEquals(0L, $opt$Mul(0L, 3L));
    expectEquals(0L, $opt$Mul(3L, 0L));
    expectEquals(-3L, $opt$Mul(1L, -3L));
    expectEquals(36L, $opt$Mul(-12L, -3L));
    expectEquals(33L, $opt$Mul(1L, 3L) * 11);
    expectEquals(240518168583L, $opt$Mul(34359738369L, 7L)); // (2^35 + 1) * 7
  }

  static int $opt$Mul(int a, int b) {
    return a * b;
  }

  static long $opt$Mul(long a, long b) {
    return a * b;
  }

}
