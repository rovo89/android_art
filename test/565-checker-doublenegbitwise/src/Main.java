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

  // A dummy value to defeat inlining of these routines.
  static boolean doThrow = false;

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

  /**
   * Test transformation of Not/Not/And into Or/Not.
   */

  // Note: before the instruction_simplifier pass, Xor's are used instead of
  // Not's (the simplification happens during the same pass).
  /// CHECK-START: int Main.$opt$noinline$andToOr(int, int) instruction_simplifier (before)
  /// CHECK:       <<P1:i\d+>>          ParameterValue
  /// CHECK:       <<P2:i\d+>>          ParameterValue
  /// CHECK:       <<CstM1:i\d+>>       IntConstant -1
  /// CHECK:       <<Not1:i\d+>>        Xor [<<P1>>,<<CstM1>>]
  /// CHECK:       <<Not2:i\d+>>        Xor [<<P2>>,<<CstM1>>]
  /// CHECK:       <<And:i\d+>>         And [<<Not1>>,<<Not2>>]
  /// CHECK:                            Return [<<And>>]

  /// CHECK-START: int Main.$opt$noinline$andToOr(int, int) instruction_simplifier (after)
  /// CHECK:       <<P1:i\d+>>          ParameterValue
  /// CHECK:       <<P2:i\d+>>          ParameterValue
  /// CHECK:       <<Or:i\d+>>          Or [<<P1>>,<<P2>>]
  /// CHECK:       <<Not:i\d+>>         Not [<<Or>>]
  /// CHECK:                            Return [<<Not>>]

  /// CHECK-START: int Main.$opt$noinline$andToOr(int, int) instruction_simplifier (after)
  /// CHECK:                            Not
  /// CHECK-NOT:                        Not
  /// CHECK-NOT:                        And

  public static int $opt$noinline$andToOr(int a, int b) {
    if (doThrow) throw new Error();
    return ~a & ~b;
  }

  /**
   * Test transformation of Not/Not/Or into And/Not.
   */

  // See note above.
  // The second Xor has its arguments reversed for no obvious reason.
  /// CHECK-START: long Main.$opt$noinline$orToAnd(long, long) instruction_simplifier (before)
  /// CHECK:       <<P1:j\d+>>          ParameterValue
  /// CHECK:       <<P2:j\d+>>          ParameterValue
  /// CHECK:       <<CstM1:j\d+>>       LongConstant -1
  /// CHECK:       <<Not1:j\d+>>        Xor [<<P1>>,<<CstM1>>]
  /// CHECK:       <<Not2:j\d+>>        Xor [<<CstM1>>,<<P2>>]
  /// CHECK:       <<Or:j\d+>>          Or [<<Not1>>,<<Not2>>]
  /// CHECK:                            Return [<<Or>>]

  /// CHECK-START: long Main.$opt$noinline$orToAnd(long, long) instruction_simplifier (after)
  /// CHECK:       <<P1:j\d+>>          ParameterValue
  /// CHECK:       <<P2:j\d+>>          ParameterValue
  /// CHECK:       <<And:j\d+>>         And [<<P1>>,<<P2>>]
  /// CHECK:       <<Not:j\d+>>         Not [<<And>>]
  /// CHECK:                            Return [<<Not>>]

  /// CHECK-START: long Main.$opt$noinline$orToAnd(long, long) instruction_simplifier (after)
  /// CHECK:                            Not
  /// CHECK-NOT:                        Not
  /// CHECK-NOT:                        Or

  public static long $opt$noinline$orToAnd(long a, long b) {
    if (doThrow) throw new Error();
    return ~a | ~b;
  }

  /**
   * Test that the transformation copes with inputs being separated from the
   * bitwise operations.
   * This is a regression test. The initial logic was inserting the new bitwise
   * operation incorrectly.
   */

  /// CHECK-START: int Main.$opt$noinline$regressInputsAway(int, int) instruction_simplifier (before)
  /// CHECK:       <<P1:i\d+>>          ParameterValue
  /// CHECK:       <<P2:i\d+>>          ParameterValue
  /// CHECK-DAG:   <<Cst1:i\d+>>        IntConstant 1
  /// CHECK-DAG:   <<CstM1:i\d+>>       IntConstant -1
  /// CHECK:       <<AddP1:i\d+>>       Add [<<P1>>,<<Cst1>>]
  /// CHECK:       <<Not1:i\d+>>        Xor [<<AddP1>>,<<CstM1>>]
  /// CHECK:       <<AddP2:i\d+>>       Add [<<P2>>,<<Cst1>>]
  /// CHECK:       <<Not2:i\d+>>        Xor [<<AddP2>>,<<CstM1>>]
  /// CHECK:       <<Or:i\d+>>          Or [<<Not1>>,<<Not2>>]
  /// CHECK:                            Return [<<Or>>]

  /// CHECK-START: int Main.$opt$noinline$regressInputsAway(int, int) instruction_simplifier (after)
  /// CHECK:       <<P1:i\d+>>          ParameterValue
  /// CHECK:       <<P2:i\d+>>          ParameterValue
  /// CHECK:       <<Cst1:i\d+>>        IntConstant 1
  /// CHECK:       <<AddP1:i\d+>>       Add [<<P1>>,<<Cst1>>]
  /// CHECK:       <<AddP2:i\d+>>       Add [<<P2>>,<<Cst1>>]
  /// CHECK:       <<And:i\d+>>         And [<<AddP1>>,<<AddP2>>]
  /// CHECK:       <<Not:i\d+>>         Not [<<And>>]
  /// CHECK:                            Return [<<Not>>]

  /// CHECK-START: int Main.$opt$noinline$regressInputsAway(int, int) instruction_simplifier (after)
  /// CHECK:                            Not
  /// CHECK-NOT:                        Not
  /// CHECK-NOT:                        Or

  public static int $opt$noinline$regressInputsAway(int a, int b) {
    if (doThrow) throw new Error();
    int a1 = a + 1;
    int not_a1 = ~a1;
    int b1 = b + 1;
    int not_b1 = ~b1;
    return not_a1 | not_b1;
  }

  /**
   * Test transformation of Not/Not/Xor into Xor.
   */

  // See first note above.
  /// CHECK-START: int Main.$opt$noinline$notXorToXor(int, int) instruction_simplifier (before)
  /// CHECK:       <<P1:i\d+>>          ParameterValue
  /// CHECK:       <<P2:i\d+>>          ParameterValue
  /// CHECK:       <<CstM1:i\d+>>       IntConstant -1
  /// CHECK:       <<Not1:i\d+>>        Xor [<<P1>>,<<CstM1>>]
  /// CHECK:       <<Not2:i\d+>>        Xor [<<P2>>,<<CstM1>>]
  /// CHECK:       <<Xor:i\d+>>         Xor [<<Not1>>,<<Not2>>]
  /// CHECK:                            Return [<<Xor>>]

  /// CHECK-START: int Main.$opt$noinline$notXorToXor(int, int) instruction_simplifier (after)
  /// CHECK:       <<P1:i\d+>>          ParameterValue
  /// CHECK:       <<P2:i\d+>>          ParameterValue
  /// CHECK:       <<Xor:i\d+>>         Xor [<<P1>>,<<P2>>]
  /// CHECK:                            Return [<<Xor>>]

  /// CHECK-START: int Main.$opt$noinline$notXorToXor(int, int) instruction_simplifier (after)
  /// CHECK-NOT:                        Not

  public static int $opt$noinline$notXorToXor(int a, int b) {
    if (doThrow) throw new Error();
    return ~a ^ ~b;
  }

  /**
   * Check that no transformation is done when one Not has multiple uses.
   */

  /// CHECK-START: int Main.$opt$noinline$notMultipleUses(int, int) instruction_simplifier (before)
  /// CHECK:       <<P1:i\d+>>          ParameterValue
  /// CHECK:       <<P2:i\d+>>          ParameterValue
  /// CHECK:       <<CstM1:i\d+>>       IntConstant -1
  /// CHECK:       <<One:i\d+>>         IntConstant 1
  /// CHECK:       <<Not2:i\d+>>        Xor [<<P2>>,<<CstM1>>]
  /// CHECK:       <<And2:i\d+>>        And [<<Not2>>,<<One>>]
  /// CHECK:       <<Not1:i\d+>>        Xor [<<P1>>,<<CstM1>>]
  /// CHECK:       <<And1:i\d+>>        And [<<Not1>>,<<Not2>>]
  /// CHECK:       <<Add:i\d+>>         Add [<<And2>>,<<And1>>]
  /// CHECK:                            Return [<<Add>>]

  /// CHECK-START: int Main.$opt$noinline$notMultipleUses(int, int) instruction_simplifier (after)
  /// CHECK:       <<P1:i\d+>>          ParameterValue
  /// CHECK:       <<P2:i\d+>>          ParameterValue
  /// CHECK:       <<One:i\d+>>         IntConstant 1
  /// CHECK:       <<Not2:i\d+>>        Not [<<P2>>]
  /// CHECK:       <<And2:i\d+>>        And [<<Not2>>,<<One>>]
  /// CHECK:       <<Not1:i\d+>>        Not [<<P1>>]
  /// CHECK:       <<And1:i\d+>>        And [<<Not1>>,<<Not2>>]
  /// CHECK:       <<Add:i\d+>>         Add [<<And2>>,<<And1>>]
  /// CHECK:                            Return [<<Add>>]

  /// CHECK-START: int Main.$opt$noinline$notMultipleUses(int, int) instruction_simplifier (after)
  /// CHECK-NOT:                        Or

  public static int $opt$noinline$notMultipleUses(int a, int b) {
    if (doThrow) throw new Error();
    int tmp = ~b;
    return (tmp & 0x1) + (~a & tmp);
  }

  public static void main(String[] args) {
    assertIntEquals(~0xff, $opt$noinline$andToOr(0xf, 0xff));
    assertLongEquals(~0xf, $opt$noinline$orToAnd(0xf, 0xff));
    assertIntEquals(0xf0, $opt$noinline$notXorToXor(0xf, 0xff));
    assertIntEquals(~0xff, $opt$noinline$notMultipleUses(0xf, 0xff));
  }
}
