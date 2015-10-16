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

  public static void assertLongEquals(long expected, long result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  /**
   * Test that the `-1` constant is not synthesized in a register and that we
   * instead simply switch between `add` and `sub` instructions with the
   * constant embedded.
   * We need two uses (or more) of the constant because the compiler always
   * delegates the immediate value handling to VIXL when there is only one use.
   */

  /// CHECK-START-ARM64: long Main.addM1(long) register (after)
  /// CHECK:     <<Arg:j\d+>>       ParameterValue
  /// CHECK:     <<ConstM1:j\d+>>   LongConstant -1
  /// CHECK-NOT:                    ParallelMove
  /// CHECK:                        Add [<<Arg>>,<<ConstM1>>]
  /// CHECK:                        Sub [<<Arg>>,<<ConstM1>>]

  /// CHECK-START-ARM64: long Main.addM1(long) disassembly (after)
  /// CHECK:                        sub x{{\d+}}, x{{\d+}}, #0x1
  /// CHECK:                        add x{{\d+}}, x{{\d+}}, #0x1

  public static long addM1(long arg) {
    return (arg + (-1)) | (arg - (-1));
  }

  public static void main(String[] args) {
    assertLongEquals(14, addM1(7));
  }
}
