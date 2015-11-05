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
  public static int FIBCOUNT = 64;
  public static int[] fibs;

  /// CHECK-START-X86_64: int Main.test() disassembly (after)
  /// CHECK:          If
  /// CHECK-NEXT:     cmp
  /// CHECK-NEXT:     jnl/ge
  /// CHECK-NOT:      jmp
  /// CHECK:          ArrayGet
  // Checks that there is no conditional jump over a jmp. The ArrayGet is in
  // the next block.
  public static int test() {
    for (int i = 1; ; i++) {
      if (i >= FIBCOUNT) {
        return fibs[0];
      }
      fibs[i] = (i + fibs[(i - 1)]);
    }
  }

  public static void main(String[] args) {
    fibs = new int[FIBCOUNT];
    fibs[0] = 1;
    test();
  }
}
