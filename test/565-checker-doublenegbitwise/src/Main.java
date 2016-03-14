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

  /// CHECK-START: int Main.$opt$noinline$andToOr(int, int) instruction_simplifier (before)
  /// CHECK:       <<P1:i\d+>>          ParameterValue
  /// CHECK:       <<P2:i\d+>>          ParameterValue
  /// CHECK:       <<Not1:i\d+>>        Not [<<P1>>]
  /// CHECK:       <<Not2:i\d+>>        Not [<<P2>>]
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

  /// CHECK-START: int Main.$opt$noinline$andToOr(int, int) instruction_simplifier (after)
  /// CHECK-NOT:                        And

  public static int $opt$noinline$andToOr(int a, int b) {
    if (doThrow) throw new Error();
    return ~a & ~b;
  }

  /**
   * Test transformation of Not/Not/And into Or/Not for boolean negations.
   * Note that the graph before this instruction simplification pass does not
   * contain `HBooleanNot` instructions. This is because this transformation
   * follows the optimization of `HSelect` to `HBooleanNot` occurring in the
   * same pass.
   */

  /// CHECK-START: boolean Main.$opt$noinline$booleanAndToOr(boolean, boolean) instruction_simplifier_after_bce (before)
  /// CHECK:       <<P1:z\d+>>          ParameterValue
  /// CHECK:       <<P2:z\d+>>          ParameterValue
  /// CHECK-DAG:   <<Const0:i\d+>>      IntConstant 0
  /// CHECK-DAG:   <<Const1:i\d+>>      IntConstant 1
  /// CHECK:       <<Select1:i\d+>>     Select [<<Const1>>,<<Const0>>,<<P1>>]
  /// CHECK:       <<Select2:i\d+>>     Select [<<Const1>>,<<Const0>>,<<P2>>]
  /// CHECK:       <<And:i\d+>>         And [<<Select2>>,<<Select1>>]
  /// CHECK:                            Return [<<And>>]

  /// CHECK-START: boolean Main.$opt$noinline$booleanAndToOr(boolean, boolean) instruction_simplifier_after_bce (after)
  /// CHECK:       <<Cond1:z\d+>>       ParameterValue
  /// CHECK:       <<Cond2:z\d+>>       ParameterValue
  /// CHECK:       <<Or:i\d+>>          Or [<<Cond2>>,<<Cond1>>]
  /// CHECK:       <<BooleanNot:z\d+>>  BooleanNot [<<Or>>]
  /// CHECK:                            Return [<<BooleanNot>>]

  /// CHECK-START: boolean Main.$opt$noinline$booleanAndToOr(boolean, boolean) instruction_simplifier_after_bce (after)
  /// CHECK:                            BooleanNot
  /// CHECK-NOT:                        BooleanNot

  /// CHECK-START: boolean Main.$opt$noinline$booleanAndToOr(boolean, boolean) instruction_simplifier_after_bce (after)
  /// CHECK-NOT:                        And

  public static boolean $opt$noinline$booleanAndToOr(boolean a, boolean b) {
    if (doThrow) throw new Error();
    return !a & !b;
  }

  /**
   * Test transformation of Not/Not/Or into And/Not.
   */

  /// CHECK-START: long Main.$opt$noinline$orToAnd(long, long) instruction_simplifier (before)
  /// CHECK:       <<P1:j\d+>>          ParameterValue
  /// CHECK:       <<P2:j\d+>>          ParameterValue
  /// CHECK:       <<Not1:j\d+>>        Not [<<P1>>]
  /// CHECK:       <<Not2:j\d+>>        Not [<<P2>>]
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

  /// CHECK-START: long Main.$opt$noinline$orToAnd(long, long) instruction_simplifier (after)
  /// CHECK-NOT:                        Or

  public static long $opt$noinline$orToAnd(long a, long b) {
    if (doThrow) throw new Error();
    return ~a | ~b;
  }

  /**
   * Test transformation of Not/Not/Or into Or/And for boolean negations.
   * Note that the graph before this instruction simplification pass does not
   * contain `HBooleanNot` instructions. This is because this transformation
   * follows the optimization of `HSelect` to `HBooleanNot` occurring in the
   * same pass.
   */

  /// CHECK-START: boolean Main.$opt$noinline$booleanOrToAnd(boolean, boolean) instruction_simplifier_after_bce (before)
  /// CHECK:       <<P1:z\d+>>          ParameterValue
  /// CHECK:       <<P2:z\d+>>          ParameterValue
  /// CHECK-DAG:   <<Const0:i\d+>>      IntConstant 0
  /// CHECK-DAG:   <<Const1:i\d+>>      IntConstant 1
  /// CHECK:       <<Select1:i\d+>>     Select [<<Const1>>,<<Const0>>,<<P1>>]
  /// CHECK:       <<Select2:i\d+>>     Select [<<Const1>>,<<Const0>>,<<P2>>]
  /// CHECK:       <<Or:i\d+>>          Or [<<Select2>>,<<Select1>>]
  /// CHECK:                            Return [<<Or>>]

  /// CHECK-START: boolean Main.$opt$noinline$booleanOrToAnd(boolean, boolean) instruction_simplifier_after_bce (after)
  /// CHECK:       <<Cond1:z\d+>>       ParameterValue
  /// CHECK:       <<Cond2:z\d+>>       ParameterValue
  /// CHECK:       <<And:i\d+>>         And [<<Cond2>>,<<Cond1>>]
  /// CHECK:       <<BooleanNot:z\d+>>  BooleanNot [<<And>>]
  /// CHECK:                            Return [<<BooleanNot>>]

  /// CHECK-START: boolean Main.$opt$noinline$booleanOrToAnd(boolean, boolean) instruction_simplifier_after_bce (after)
  /// CHECK:                            BooleanNot
  /// CHECK-NOT:                        BooleanNot

  /// CHECK-START: boolean Main.$opt$noinline$booleanOrToAnd(boolean, boolean) instruction_simplifier_after_bce (after)
  /// CHECK-NOT:                        Or

  public static boolean $opt$noinline$booleanOrToAnd(boolean a, boolean b) {
    if (doThrow) throw new Error();
    return !a | !b;
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
  /// CHECK:       <<Cst1:i\d+>>        IntConstant 1
  /// CHECK:       <<AddP1:i\d+>>       Add [<<P1>>,<<Cst1>>]
  /// CHECK:       <<Not1:i\d+>>        Not [<<AddP1>>]
  /// CHECK:       <<AddP2:i\d+>>       Add [<<P2>>,<<Cst1>>]
  /// CHECK:       <<Not2:i\d+>>        Not [<<AddP2>>]
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

  /// CHECK-START: int Main.$opt$noinline$regressInputsAway(int, int) instruction_simplifier (after)
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
  /// CHECK:       <<Not1:i\d+>>        Not [<<P1>>]
  /// CHECK:       <<Not2:i\d+>>        Not [<<P2>>]
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
   * Test transformation of Not/Not/Xor into Xor for boolean negations.
   * Note that the graph before this instruction simplification pass does not
   * contain `HBooleanNot` instructions. This is because this transformation
   * follows the optimization of `HSelect` to `HBooleanNot` occurring in the
   * same pass.
   */

  /// CHECK-START: boolean Main.$opt$noinline$booleanNotXorToXor(boolean, boolean) instruction_simplifier_after_bce (before)
  /// CHECK:       <<P1:z\d+>>          ParameterValue
  /// CHECK:       <<P2:z\d+>>          ParameterValue
  /// CHECK-DAG:   <<Const0:i\d+>>      IntConstant 0
  /// CHECK-DAG:   <<Const1:i\d+>>      IntConstant 1
  /// CHECK:       <<Select1:i\d+>>     Select [<<Const1>>,<<Const0>>,<<P1>>]
  /// CHECK:       <<Select2:i\d+>>     Select [<<Const1>>,<<Const0>>,<<P2>>]
  /// CHECK:       <<Xor:i\d+>>         Xor [<<Select2>>,<<Select1>>]
  /// CHECK:                            Return [<<Xor>>]

  /// CHECK-START: boolean Main.$opt$noinline$booleanNotXorToXor(boolean, boolean) instruction_simplifier_after_bce (after)
  /// CHECK:       <<Cond1:z\d+>>       ParameterValue
  /// CHECK:       <<Cond2:z\d+>>       ParameterValue
  /// CHECK:       <<Xor:i\d+>>         Xor [<<Cond2>>,<<Cond1>>]
  /// CHECK:                            Return [<<Xor>>]

  /// CHECK-START: boolean Main.$opt$noinline$booleanNotXorToXor(boolean, boolean) instruction_simplifier_after_bce (after)
  /// CHECK-NOT:                        BooleanNot

  public static boolean $opt$noinline$booleanNotXorToXor(boolean a, boolean b) {
    if (doThrow) throw new Error();
    return !a ^ !b;
  }

  /**
   * Check that no transformation is done when one Not has multiple uses.
   */

  /// CHECK-START: int Main.$opt$noinline$notMultipleUses(int, int) instruction_simplifier (before)
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
