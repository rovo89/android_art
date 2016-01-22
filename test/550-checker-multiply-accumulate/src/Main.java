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
   * Test basic merging of `MUL+ADD` into `MULADD`.
   */

  /// CHECK-START-ARM64: int Main.$opt$noinline$mulAdd(int, int, int) instruction_simplifier_arm64 (before)
  /// CHECK:       <<Acc:i\d+>>         ParameterValue
  /// CHECK:       <<Left:i\d+>>        ParameterValue
  /// CHECK:       <<Right:i\d+>>       ParameterValue
  /// CHECK:       <<Mul:i\d+>>         Mul [<<Left>>,<<Right>>]
  /// CHECK:       <<Add:i\d+>>         Add [<<Acc>>,<<Mul>>]
  /// CHECK:                            Return [<<Add>>]

  /// CHECK-START-ARM64: int Main.$opt$noinline$mulAdd(int, int, int) instruction_simplifier_arm64 (after)
  /// CHECK:       <<Acc:i\d+>>         ParameterValue
  /// CHECK:       <<Left:i\d+>>        ParameterValue
  /// CHECK:       <<Right:i\d+>>       ParameterValue
  /// CHECK:       <<MulAdd:i\d+>>      Arm64MultiplyAccumulate [<<Acc>>,<<Left>>,<<Right>>] kind:Add
  /// CHECK:                            Return [<<MulAdd>>]

  /// CHECK-START-ARM64: int Main.$opt$noinline$mulAdd(int, int, int) instruction_simplifier_arm64 (after)
  /// CHECK-NOT:                        Mul
  /// CHECK-NOT:                        Add

  /// CHECK-START-ARM64: int Main.$opt$noinline$mulAdd(int, int, int) disassembly (after)
  /// CHECK:                            madd w{{\d+}}, w{{\d+}}, w{{\d+}}, w{{\d+}}

  public static int $opt$noinline$mulAdd(int acc, int left, int right) {
    if (doThrow) throw new Error();
    return acc + left * right;
  }

  /**
   * Test basic merging of `MUL+SUB` into `MULSUB`.
   */

  /// CHECK-START-ARM64: long Main.$opt$noinline$mulSub(long, long, long) instruction_simplifier_arm64 (before)
  /// CHECK:       <<Acc:j\d+>>         ParameterValue
  /// CHECK:       <<Left:j\d+>>        ParameterValue
  /// CHECK:       <<Right:j\d+>>       ParameterValue
  /// CHECK:       <<Mul:j\d+>>         Mul [<<Left>>,<<Right>>]
  /// CHECK:       <<Sub:j\d+>>         Sub [<<Acc>>,<<Mul>>]
  /// CHECK:                            Return [<<Sub>>]

  /// CHECK-START-ARM64: long Main.$opt$noinline$mulSub(long, long, long) instruction_simplifier_arm64 (after)
  /// CHECK:       <<Acc:j\d+>>         ParameterValue
  /// CHECK:       <<Left:j\d+>>        ParameterValue
  /// CHECK:       <<Right:j\d+>>       ParameterValue
  /// CHECK:       <<MulSub:j\d+>>      Arm64MultiplyAccumulate [<<Acc>>,<<Left>>,<<Right>>] kind:Sub
  /// CHECK:                            Return [<<MulSub>>]

  /// CHECK-START-ARM64: long Main.$opt$noinline$mulSub(long, long, long) instruction_simplifier_arm64 (after)
  /// CHECK-NOT:                        Mul
  /// CHECK-NOT:                        Sub

  /// CHECK-START-ARM64: long Main.$opt$noinline$mulSub(long, long, long) disassembly (after)
  /// CHECK:                            msub x{{\d+}}, x{{\d+}}, x{{\d+}}, x{{\d+}}

  public static long $opt$noinline$mulSub(long acc, long left, long right) {
    if (doThrow) throw new Error();
    return acc - left * right;
  }

  /**
   * Test that we do not create a multiply-accumulate instruction when there
   * are other uses of the multiplication that cannot merge it.
   */

  /// CHECK-START-ARM64: int Main.$opt$noinline$multipleUses1(int, int, int) instruction_simplifier_arm64 (before)
  /// CHECK:       <<Acc:i\d+>>         ParameterValue
  /// CHECK:       <<Left:i\d+>>        ParameterValue
  /// CHECK:       <<Right:i\d+>>       ParameterValue
  /// CHECK:       <<Mul:i\d+>>         Mul [<<Left>>,<<Right>>]
  /// CHECK:       <<Add:i\d+>>         Add [<<Acc>>,<<Mul>>]
  /// CHECK:       <<Or:i\d+>>          Or [<<Mul>>,<<Add>>]
  /// CHECK:                            Return [<<Or>>]

  /// CHECK-START-ARM64: int Main.$opt$noinline$multipleUses1(int, int, int) instruction_simplifier_arm64 (after)
  /// CHECK:       <<Acc:i\d+>>         ParameterValue
  /// CHECK:       <<Left:i\d+>>        ParameterValue
  /// CHECK:       <<Right:i\d+>>       ParameterValue
  /// CHECK:       <<Mul:i\d+>>         Mul [<<Left>>,<<Right>>]
  /// CHECK:       <<Add:i\d+>>         Add [<<Acc>>,<<Mul>>]
  /// CHECK:       <<Or:i\d+>>          Or [<<Mul>>,<<Add>>]
  /// CHECK:                            Return [<<Or>>]

  /// CHECK-START-ARM64: int Main.$opt$noinline$multipleUses1(int, int, int) instruction_simplifier_arm64 (after)
  /// CHECK-NOT:                        Arm64MultiplyAccumulate

  public static int $opt$noinline$multipleUses1(int acc, int left, int right) {
    if (doThrow) throw new Error();
    int temp = left * right;
    return temp | (acc + temp);
  }

  /**
   * Test that we do not create a multiply-accumulate instruction even when all
   * uses of the multiplication can merge it.
   */

  /// CHECK-START-ARM64: long Main.$opt$noinline$multipleUses2(long, long, long) instruction_simplifier_arm64 (before)
  /// CHECK:       <<Acc:j\d+>>         ParameterValue
  /// CHECK:       <<Left:j\d+>>        ParameterValue
  /// CHECK:       <<Right:j\d+>>       ParameterValue
  /// CHECK:       <<Mul:j\d+>>         Mul [<<Left>>,<<Right>>]
  /// CHECK:       <<Add:j\d+>>         Add [<<Acc>>,<<Mul>>]
  /// CHECK:       <<Sub:j\d+>>         Sub [<<Acc>>,<<Mul>>]
  /// CHECK:       <<Res:j\d+>>         Add [<<Add>>,<<Sub>>]
  /// CHECK:                            Return [<<Res>>]

  /// CHECK-START-ARM64: long Main.$opt$noinline$multipleUses2(long, long, long) instruction_simplifier_arm64 (after)
  /// CHECK:       <<Acc:j\d+>>         ParameterValue
  /// CHECK:       <<Left:j\d+>>        ParameterValue
  /// CHECK:       <<Right:j\d+>>       ParameterValue
  /// CHECK:       <<Mul:j\d+>>         Mul [<<Left>>,<<Right>>]
  /// CHECK:       <<Add:j\d+>>         Add [<<Acc>>,<<Mul>>]
  /// CHECK:       <<Sub:j\d+>>         Sub [<<Acc>>,<<Mul>>]
  /// CHECK:       <<Res:j\d+>>         Add [<<Add>>,<<Sub>>]
  /// CHECK:                            Return [<<Res>>]

  /// CHECK-START-ARM64: long Main.$opt$noinline$multipleUses2(long, long, long) instruction_simplifier_arm64 (after)
  /// CHECK-NOT:                        Arm64MultiplyAccumulate


  public static long $opt$noinline$multipleUses2(long acc, long left, long right) {
    if (doThrow) throw new Error();
    long temp = left * right;
    return (acc + temp) + (acc - temp);
  }


  /**
   * Test the interpretation of `a * (b + 1)` as `a + (a * b)`.
   */

  /// CHECK-START-ARM64: int Main.$opt$noinline$mulPlusOne(int, int) instruction_simplifier_arm64 (before)
  /// CHECK:       <<Acc:i\d+>>         ParameterValue
  /// CHECK:       <<Var:i\d+>>         ParameterValue
  /// CHECK:       <<Const1:i\d+>>      IntConstant 1
  /// CHECK:       <<Add:i\d+>>         Add [<<Var>>,<<Const1>>]
  /// CHECK:       <<Mul:i\d+>>         Mul [<<Acc>>,<<Add>>]
  /// CHECK:                            Return [<<Mul>>]

  /// CHECK-START-ARM64: int Main.$opt$noinline$mulPlusOne(int, int) instruction_simplifier_arm64 (after)
  /// CHECK:       <<Acc:i\d+>>         ParameterValue
  /// CHECK:       <<Var:i\d+>>         ParameterValue
  /// CHECK:       <<MulAdd:i\d+>>      Arm64MultiplyAccumulate [<<Acc>>,<<Acc>>,<<Var>>] kind:Add
  /// CHECK:                            Return [<<MulAdd>>]

  /// CHECK-START-ARM64: int Main.$opt$noinline$mulPlusOne(int, int) instruction_simplifier_arm64 (after)
  /// CHECK-NOT:                        Mul
  /// CHECK-NOT:                        Add

  /// CHECK-START-ARM64: int Main.$opt$noinline$mulPlusOne(int, int) disassembly (after)
  /// CHECK:                            madd w{{\d+}}, w{{\d+}}, w{{\d+}}, w{{\d+}}

  public static int $opt$noinline$mulPlusOne(int acc, int var) {
    if (doThrow) throw new Error();
    return acc * (var + 1);
  }


  /**
   * Test the interpretation of `a * (1 - b)` as `a - (a * b)`.
   */

  /// CHECK-START-ARM64: long Main.$opt$noinline$mulMinusOne(long, long) instruction_simplifier_arm64 (before)
  /// CHECK:       <<Acc:j\d+>>         ParameterValue
  /// CHECK:       <<Var:j\d+>>         ParameterValue
  /// CHECK:       <<Const1:j\d+>>      LongConstant 1
  /// CHECK:       <<Sub:j\d+>>         Sub [<<Const1>>,<<Var>>]
  /// CHECK:       <<Mul:j\d+>>         Mul [<<Acc>>,<<Sub>>]
  /// CHECK:                            Return [<<Mul>>]

  /// CHECK-START-ARM64: long Main.$opt$noinline$mulMinusOne(long, long) instruction_simplifier_arm64 (after)
  /// CHECK:       <<Acc:j\d+>>         ParameterValue
  /// CHECK:       <<Var:j\d+>>         ParameterValue
  /// CHECK:       <<MulSub:j\d+>>      Arm64MultiplyAccumulate [<<Acc>>,<<Acc>>,<<Var>>] kind:Sub
  /// CHECK:                            Return [<<MulSub>>]

  /// CHECK-START-ARM64: long Main.$opt$noinline$mulMinusOne(long, long) instruction_simplifier_arm64 (after)
  /// CHECK-NOT:                        Mul
  /// CHECK-NOT:                        Sub

  /// CHECK-START-ARM64: long Main.$opt$noinline$mulMinusOne(long, long) disassembly (after)
  /// CHECK:                            msub x{{\d+}}, x{{\d+}}, x{{\d+}}, x{{\d+}}

  public static long $opt$noinline$mulMinusOne(long acc, long var) {
    if (doThrow) throw new Error();
    return acc * (1 - var);
  }


  public static void main(String[] args) {
    assertIntEquals(7, $opt$noinline$mulAdd(1, 2, 3));
    assertLongEquals(-26, $opt$noinline$mulSub(4, 5, 6));
    assertIntEquals(79, $opt$noinline$multipleUses1(7, 8, 9));
    assertLongEquals(20, $opt$noinline$multipleUses2(10, 11, 12));
    assertIntEquals(195, $opt$noinline$mulPlusOne(13, 14));
    assertLongEquals(-225, $opt$noinline$mulMinusOne(15, 16));
  }
}
