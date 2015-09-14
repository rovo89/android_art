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

  public static void assertIntEquals(int expected, int result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  public static void assertLongEquals(long expected, long result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  /// CHECK-START-ARM: int Main.and255(int) disassembly (after)
  /// CHECK-NOT:            movs {{r\d+}}, #255
  /// CHECK:                and {{r\d+}}, {{r\d+}}, #255

  public static int and255(int arg) {
    return arg & 255;
  }

  /// CHECK-START-ARM: int Main.and511(int) disassembly (after)
  /// CHECK:                movw {{r\d+}}, #511
  /// CHECK:                and{{(\.w)?}} {{r\d+}}, {{r\d+}}, {{r\d+}}

  public static int and511(int arg) {
    return arg & 511;
  }

  /// CHECK-START-ARM: int Main.andNot15(int) disassembly (after)
  /// CHECK-NOT:            mvn {{r\d+}}, #15
  /// CHECK:                bic {{r\d+}}, {{r\d+}}, #15

  public static int andNot15(int arg) {
    return arg & ~15;
  }

  /// CHECK-START-ARM: int Main.or255(int) disassembly (after)
  /// CHECK-NOT:            movs {{r\d+}}, #255
  /// CHECK:                orr {{r\d+}}, {{r\d+}}, #255

  public static int or255(int arg) {
    return arg | 255;
  }

  /// CHECK-START-ARM: int Main.or511(int) disassembly (after)
  /// CHECK:                movw {{r\d+}}, #511
  /// CHECK:                orr{{(\.w)?}} {{r\d+}}, {{r\d+}}, {{r\d+}}

  public static int or511(int arg) {
    return arg | 511;
  }

  /// CHECK-START-ARM: int Main.orNot15(int) disassembly (after)
  /// CHECK-NOT:            mvn {{r\d+}}, #15
  /// CHECK:                orn {{r\d+}}, {{r\d+}}, #15

  public static int orNot15(int arg) {
    return arg | ~15;
  }

  /// CHECK-START-ARM: int Main.xor255(int) disassembly (after)
  /// CHECK-NOT:            movs {{r\d+}}, #255
  /// CHECK:                eor {{r\d+}}, {{r\d+}}, #255

  public static int xor255(int arg) {
    return arg ^ 255;
  }

  /// CHECK-START-ARM: int Main.xor511(int) disassembly (after)
  /// CHECK:                movw {{r\d+}}, #511
  /// CHECK:                eor{{(\.w)?}} {{r\d+}}, {{r\d+}}, {{r\d+}}

  public static int xor511(int arg) {
    return arg ^ 511;
  }

  /// CHECK-START-ARM: int Main.xorNot15(int) disassembly (after)
  /// CHECK:                mvn {{r\d+}}, #15
  /// CHECK:                eor{{(\.w)?}} {{r\d+}}, {{r\d+}}, {{r\d+}}

  public static int xorNot15(int arg) {
    return arg ^ ~15;
  }

  /// CHECK-START-ARM: long Main.and255(long) disassembly (after)
  /// CHECK-NOT:            movs {{r\d+}}, #255
  /// CHECK-NOT:            and
  /// CHECK-NOT:            bic
  /// CHECK-DAG:            and {{r\d+}}, {{r\d+}}, #255
  /// CHECK-DAG:            movs {{r\d+}}, #0
  /// CHECK-NOT:            and
  /// CHECK-NOT:            bic

  public static long and255(long arg) {
    return arg & 255L;
  }

  /// CHECK-START-ARM: long Main.and511(long) disassembly (after)
  /// CHECK:                movw {{r\d+}}, #511
  /// CHECK-NOT:            and
  /// CHECK-NOT:            bic
  /// CHECK-DAG:            and{{(\.w)?}} {{r\d+}}, {{r\d+}}, {{r\d+}}
  /// CHECK-DAG:            movs {{r\d+}}, #0
  /// CHECK-NOT:            and
  /// CHECK-NOT:            bic

  public static long and511(long arg) {
    return arg & 511L;
  }

  /// CHECK-START-ARM: long Main.andNot15(long) disassembly (after)
  /// CHECK-NOT:            mvn {{r\d+}}, #15
  /// CHECK-NOT:            and
  /// CHECK-NOT:            bic
  /// CHECK:                bic {{r\d+}}, {{r\d+}}, #15
  /// CHECK-NOT:            and
  /// CHECK-NOT:            bic

  public static long andNot15(long arg) {
    return arg & ~15L;
  }

  /// CHECK-START-ARM: long Main.and0xfffffff00000000f(long) disassembly (after)
  /// CHECK-NOT:            movs {{r\d+}}, #15
  /// CHECK-NOT:            mvn {{r\d+}}, #15
  /// CHECK-NOT:            and
  /// CHECK-NOT:            bic
  /// CHECK-DAG:            and {{r\d+}}, {{r\d+}}, #15
  /// CHECK-DAG:            bic {{r\d+}}, {{r\d+}}, #15
  /// CHECK-NOT:            and
  /// CHECK-NOT:            bic

  public static long and0xfffffff00000000f(long arg) {
    return arg & 0xfffffff00000000fL;
  }

  /// CHECK-START-ARM: long Main.or255(long) disassembly (after)
  /// CHECK-NOT:            movs {{r\d+}}, #255
  /// CHECK-NOT:            orr
  /// CHECK-NOT:            orn
  /// CHECK:                orr {{r\d+}}, {{r\d+}}, #255
  /// CHECK-NOT:            orr
  /// CHECK-NOT:            orn

  public static long or255(long arg) {
    return arg | 255L;
  }

  /// CHECK-START-ARM: long Main.or511(long) disassembly (after)
  /// CHECK:                movw {{r\d+}}, #511
  /// CHECK-NOT:            orr
  /// CHECK-NOT:            orn
  /// CHECK:                orr{{(\.w)?}} {{r\d+}}, {{r\d+}}, {{r\d+}}
  /// CHECK-NOT:            orr
  /// CHECK-NOT:            orn

  public static long or511(long arg) {
    return arg | 511L;
  }

  /// CHECK-START-ARM: long Main.orNot15(long) disassembly (after)
  /// CHECK-NOT:            mvn {{r\d+}}, #15
  /// CHECK-NOT:            orr
  /// CHECK-NOT:            orn
  /// CHECK-DAG:            orn {{r\d+}}, {{r\d+}}, #15
  /// CHECK-DAG:            mvn {{r\d+}}, #0
  /// CHECK-NOT:            orr
  /// CHECK-NOT:            orn

  public static long orNot15(long arg) {
    return arg | ~15L;
  }

  /// CHECK-START-ARM: long Main.or0xfffffff00000000f(long) disassembly (after)
  /// CHECK-NOT:            movs {{r\d+}}, #15
  /// CHECK-NOT:            mvn {{r\d+}}, #15
  /// CHECK-NOT:            orr
  /// CHECK-NOT:            orn
  /// CHECK-DAG:            orr {{r\d+}}, {{r\d+}}, #15
  /// CHECK-DAG:            orn {{r\d+}}, {{r\d+}}, #15
  /// CHECK-NOT:            orr
  /// CHECK-NOT:            orn

  public static long or0xfffffff00000000f(long arg) {
    return arg | 0xfffffff00000000fL;
  }

  /// CHECK-START-ARM: long Main.xor255(long) disassembly (after)
  /// CHECK-NOT:            movs {{r\d+}}, #255
  /// CHECK-NOT:            eor
  /// CHECK:                eor {{r\d+}}, {{r\d+}}, #255
  /// CHECK-NOT:            eor

  public static long xor255(long arg) {
    return arg ^ 255L;
  }

  /// CHECK-START-ARM: long Main.xor511(long) disassembly (after)
  /// CHECK:                movw {{r\d+}}, #511
  /// CHECK-NOT:            eor
  /// CHECK-DAG:            eor{{(\.w)?}} {{r\d+}}, {{r\d+}}, {{r\d+}}
  /// CHECK-NOT:            eor

  public static long xor511(long arg) {
    return arg ^ 511L;
  }

  /// CHECK-START-ARM: long Main.xorNot15(long) disassembly (after)
  /// CHECK-DAG:            mvn {{r\d+}}, #15
  /// CHECK-DAG:            mov.w {{r\d+}}, #-1
  /// CHECK-NOT:            eor
  /// CHECK-DAG:            eor{{(\.w)?}} {{r\d+}}, {{r\d+}}, {{r\d+}}
  /// CHECK-DAG:            eor{{(\.w)?}} {{r\d+}}, {{r\d+}}, {{r\d+}}
  /// CHECK-NOT:            eor

  public static long xorNot15(long arg) {
    return arg ^ ~15L;
  }

  // Note: No support for partial long constant embedding.
  /// CHECK-START-ARM: long Main.xor0xfffffff00000000f(long) disassembly (after)
  /// CHECK-DAG:            movs {{r\d+}}, #15
  /// CHECK-DAG:            mvn {{r\d+}}, #15
  /// CHECK-NOT:            eor
  /// CHECK-DAG:            eor{{(\.w)?}} {{r\d+}}, {{r\d+}}, {{r\d+}}
  /// CHECK-DAG:            eor{{(\.w)?}} {{r\d+}}, {{r\d+}}, {{r\d+}}
  /// CHECK-NOT:            eor

  public static long xor0xfffffff00000000f(long arg) {
    return arg ^ 0xfffffff00000000fL;
  }

  /// CHECK-START-ARM: long Main.xor0xf00000000000000f(long) disassembly (after)
  /// CHECK-NOT:            movs {{r\d+}}, #15
  /// CHECK-NOT:            mov.w {{r\d+}}, #-268435456
  /// CHECK-NOT:            eor
  /// CHECK-DAG:            eor {{r\d+}}, {{r\d+}}, #15
  /// CHECK-DAG:            eor {{r\d+}}, {{r\d+}}, #-268435456
  /// CHECK-NOT:            eor

  public static long xor0xf00000000000000f(long arg) {
    return arg ^ 0xf00000000000000fL;
  }

  public static void main(String[] args) {
    int arg = 0x87654321;
    assertIntEquals(and255(arg), 0x21);
    assertIntEquals(and511(arg), 0x121);
    assertIntEquals(andNot15(arg), 0x87654320);
    assertIntEquals(or255(arg), 0x876543ff);
    assertIntEquals(or511(arg), 0x876543ff);
    assertIntEquals(orNot15(arg), 0xfffffff1);
    assertIntEquals(xor255(arg), 0x876543de);
    assertIntEquals(xor511(arg), 0x876542de);
    assertIntEquals(xorNot15(arg), 0x789abcd1);

    long longArg = 0x1234567887654321L;
    assertLongEquals(and255(longArg), 0x21L);
    assertLongEquals(and511(longArg), 0x121L);
    assertLongEquals(andNot15(longArg), 0x1234567887654320L);
    assertLongEquals(and0xfffffff00000000f(longArg), 0x1234567000000001L);
    assertLongEquals(or255(longArg), 0x12345678876543ffL);
    assertLongEquals(or511(longArg), 0x12345678876543ffL);
    assertLongEquals(orNot15(longArg), 0xfffffffffffffff1L);
    assertLongEquals(or0xfffffff00000000f(longArg), 0xfffffff88765432fL);
    assertLongEquals(xor255(longArg), 0x12345678876543deL);
    assertLongEquals(xor511(longArg), 0x12345678876542deL);
    assertLongEquals(xorNot15(longArg), 0xedcba987789abcd1L);
    assertLongEquals(xor0xfffffff00000000f(longArg), 0xedcba9888765432eL);
    assertLongEquals(xor0xf00000000000000f(longArg), 0xe23456788765432eL);
  }
}
