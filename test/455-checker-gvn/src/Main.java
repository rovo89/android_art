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
  public static void main(String[] args) {
    System.out.println(foo(3, 4));
  }

  // CHECK-START: int Main.foo(int, int) GVN (before)
  // CHECK: Add
  // CHECK: Add
  // CHECK: Add

  // CHECK-START: int Main.foo(int, int) GVN (after)
  // CHECK: Add
  // CHECK: Add
  // CHECK-NOT: Add

  public static int foo(int x, int y) {
    int sum1 = x + y;
    int sum2 = y + x;
    return sum1 + sum2;
  }

  public static long bar(int i) {
    return i;
  }
}
