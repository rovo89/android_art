/*
 * Copyright (C) 2016 The Android Open Source Project
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

  // TODO: make this work when b/26700769 is done.
  //
  // CHECK-START-X86_64: int Main.bits32(int) disassembly (after)
  // CHECK-DAG: popcnt
  //
  // CHECK-START-X86_64: int Main.bits32(int) disassembly (after)
  // CHECK-NOT: call
  private static int bits32(int x) {
    return Integer.bitCount(x);
  }

  // TODO: make this work when b/26700769 is done.
  //
  // CHECK-START-X86_64: int Main.bits64(long) disassembly (after)
  // CHECK-DAG: popcnt
  //
  // CHECK-START-X86_64: int Main.bits64(long) disassembly (after)
  // CHECK-NOT: call
  private static int bits64(long x) {
    return Long.bitCount(x);
  }

  public static void main(String args[]) {
    expectEquals32(bits32(0x00000000), 0);
    expectEquals32(bits32(0x00000001), 1);
    expectEquals32(bits32(0x10000000), 1);
    expectEquals32(bits32(0x10000001), 2);
    expectEquals32(bits32(0x00000003), 2);
    expectEquals32(bits32(0x70000000), 3);
    expectEquals32(bits32(0x000F0000), 4);
    expectEquals32(bits32(0x00001111), 4);
    expectEquals32(bits32(0x11110000), 4);
    expectEquals32(bits32(0x11111111), 8);
    expectEquals32(bits32(0x12345678), 13);
    expectEquals32(bits32(0x9ABCDEF0), 19);
    expectEquals32(bits32(0xFFFFFFFF), 32);

    for (int i = 0; i < 32; i++) {
      expectEquals32(bits32(1 << i), 1);
    }

    expectEquals64(bits64(0x0000000000000000L), 0);
    expectEquals64(bits64(0x0000000000000001L), 1);
    expectEquals64(bits64(0x1000000000000000L), 1);
    expectEquals64(bits64(0x1000000000000001L), 2);
    expectEquals64(bits64(0x0000000000000003L), 2);
    expectEquals64(bits64(0x7000000000000000L), 3);
    expectEquals64(bits64(0x000F000000000000L), 4);
    expectEquals64(bits64(0x0000000011111111L), 8);
    expectEquals64(bits64(0x1111111100000000L), 8);
    expectEquals64(bits64(0x1111111111111111L), 16);
    expectEquals64(bits64(0x123456789ABCDEF1L), 33);
    expectEquals64(bits64(0xFFFFFFFFFFFFFFFFL), 64);

    for (int i = 0; i < 64; i++) {
      expectEquals64(bits64(1L << i), 1);
    }

    System.out.println("passed");
  }

  private static void expectEquals32(int expected, int result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }
  private static void expectEquals64(long expected, long result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }
}
