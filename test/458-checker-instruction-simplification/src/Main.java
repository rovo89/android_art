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

  public static void assertBooleanEquals(boolean expected, boolean result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

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

  public static void assertFloatEquals(float expected, float result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  public static void assertDoubleEquals(double expected, double result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  /**
   * Tiny programs exercising optimizations of arithmetic identities.
   */

  // CHECK-START: long Main.Add0(long) instruction_simplifier (before)
  // CHECK-DAG:     [[Arg:j\d+]]     ParameterValue
  // CHECK-DAG:     [[Const0:j\d+]]  LongConstant 0
  // CHECK-DAG:     [[Add:j\d+]]     Add [ [[Const0]] [[Arg]] ]
  // CHECK-DAG:                      Return [ [[Add]] ]

  // CHECK-START: long Main.Add0(long) instruction_simplifier (after)
  // CHECK-DAG:     [[Arg:j\d+]]     ParameterValue
  // CHECK-DAG:                      Return [ [[Arg]] ]

  // CHECK-START: long Main.Add0(long) instruction_simplifier (after)
  // CHECK-NOT:                        Add

  public static long Add0(long arg) {
    return 0 + arg;
  }

  // CHECK-START: int Main.AndAllOnes(int) instruction_simplifier (before)
  // CHECK-DAG:     [[Arg:i\d+]]     ParameterValue
  // CHECK-DAG:     [[ConstF:i\d+]]  IntConstant -1
  // CHECK-DAG:     [[And:i\d+]]     And [ [[Arg]] [[ConstF]] ]
  // CHECK-DAG:                      Return [ [[And]] ]

  // CHECK-START: int Main.AndAllOnes(int) instruction_simplifier (after)
  // CHECK-DAG:     [[Arg:i\d+]]     ParameterValue
  // CHECK-DAG:                      Return [ [[Arg]] ]

  // CHECK-START: int Main.AndAllOnes(int) instruction_simplifier (after)
  // CHECK-NOT:                      And

  public static int AndAllOnes(int arg) {
    return arg & -1;
  }

  // CHECK-START: long Main.Div1(long) instruction_simplifier (before)
  // CHECK-DAG:     [[Arg:j\d+]]     ParameterValue
  // CHECK-DAG:     [[Const1:j\d+]]  LongConstant 1
  // CHECK-DAG:     [[Div:j\d+]]     Div [ [[Arg]] [[Const1]] ]
  // CHECK-DAG:                      Return [ [[Div]] ]

  // CHECK-START: long Main.Div1(long) instruction_simplifier (after)
  // CHECK-DAG:     [[Arg:j\d+]]     ParameterValue
  // CHECK-DAG:                      Return [ [[Arg]] ]

  // CHECK-START: long Main.Div1(long) instruction_simplifier (after)
  // CHECK-NOT:                      Div

  public static long Div1(long arg) {
    return arg / 1;
  }

  // CHECK-START: int Main.DivN1(int) instruction_simplifier (before)
  // CHECK-DAG:     [[Arg:i\d+]]      ParameterValue
  // CHECK-DAG:     [[ConstN1:i\d+]]  IntConstant -1
  // CHECK-DAG:     [[Div:i\d+]]      Div [ [[Arg]] [[ConstN1]] ]
  // CHECK-DAG:                       Return [ [[Div]] ]

  // CHECK-START: int Main.DivN1(int) instruction_simplifier (after)
  // CHECK-DAG:     [[Arg:i\d+]]      ParameterValue
  // CHECK-DAG:     [[Neg:i\d+]]      Neg [ [[Arg]] ]
  // CHECK-DAG:                       Return [ [[Neg]] ]

  // CHECK-START: int Main.DivN1(int) instruction_simplifier (after)
  // CHECK-NOT:                       Div

  public static int DivN1(int arg) {
    return arg / -1;
  }

  // CHECK-START: long Main.Mul1(long) instruction_simplifier (before)
  // CHECK-DAG:     [[Arg:j\d+]]     ParameterValue
  // CHECK-DAG:     [[Const1:j\d+]]  LongConstant 1
  // CHECK-DAG:     [[Mul:j\d+]]     Mul [ [[Arg]] [[Const1]] ]
  // CHECK-DAG:                      Return [ [[Mul]] ]

  // CHECK-START: long Main.Mul1(long) instruction_simplifier (after)
  // CHECK-DAG:     [[Arg:j\d+]]     ParameterValue
  // CHECK-DAG:                      Return [ [[Arg]] ]

  // CHECK-START: long Main.Mul1(long) instruction_simplifier (after)
  // CHECK-NOT:                       Mul

  public static long Mul1(long arg) {
    return arg * 1;
  }

  // CHECK-START: int Main.MulN1(int) instruction_simplifier (before)
  // CHECK-DAG:     [[Arg:i\d+]]      ParameterValue
  // CHECK-DAG:     [[ConstN1:i\d+]]  IntConstant -1
  // CHECK-DAG:     [[Mul:i\d+]]      Mul [ [[Arg]] [[ConstN1]] ]
  // CHECK-DAG:                       Return [ [[Mul]] ]

  // CHECK-START: int Main.MulN1(int) instruction_simplifier (after)
  // CHECK-DAG:     [[Arg:i\d+]]      ParameterValue
  // CHECK-DAG:     [[Neg:i\d+]]      Neg [ [[Arg]] ]
  // CHECK-DAG:                       Return [ [[Neg]] ]

  // CHECK-START: int Main.MulN1(int) instruction_simplifier (after)
  // CHECK-NOT:                       Mul

  public static int MulN1(int arg) {
    return arg * -1;
  }

  // CHECK-START: long Main.MulPowerOfTwo128(long) instruction_simplifier (before)
  // CHECK-DAG:     [[Arg:j\d+]]       ParameterValue
  // CHECK-DAG:     [[Const128:j\d+]]  LongConstant 128
  // CHECK-DAG:     [[Mul:j\d+]]       Mul [ [[Arg]] [[Const128]] ]
  // CHECK-DAG:                        Return [ [[Mul]] ]

  // CHECK-START: long Main.MulPowerOfTwo128(long) instruction_simplifier (after)
  // CHECK-DAG:     [[Arg:j\d+]]       ParameterValue
  // CHECK-DAG:     [[Const7:i\d+]]    IntConstant 7
  // CHECK-DAG:     [[Shl:j\d+]]       Shl [ [[Arg]] [[Const7]] ]
  // CHECK-DAG:                        Return [ [[Shl]] ]

  // CHECK-START: long Main.MulPowerOfTwo128(long) instruction_simplifier (after)
  // CHECK-NOT:                        Mul

  public static long MulPowerOfTwo128(long arg) {
    return arg * 128;
  }

  // CHECK-START: int Main.Or0(int) instruction_simplifier (before)
  // CHECK-DAG:     [[Arg:i\d+]]      ParameterValue
  // CHECK-DAG:     [[Const0:i\d+]]   IntConstant 0
  // CHECK-DAG:     [[Or:i\d+]]       Or [ [[Arg]] [[Const0]] ]
  // CHECK-DAG:                       Return [ [[Or]] ]

  // CHECK-START: int Main.Or0(int) instruction_simplifier (after)
  // CHECK-DAG:     [[Arg:i\d+]]      ParameterValue
  // CHECK-DAG:                       Return [ [[Arg]] ]

  // CHECK-START: int Main.Or0(int) instruction_simplifier (after)
  // CHECK-NOT:                       Or

  public static int Or0(int arg) {
    return arg | 0;
  }

  // CHECK-START: long Main.OrSame(long) instruction_simplifier (before)
  // CHECK-DAG:     [[Arg:j\d+]]       ParameterValue
  // CHECK-DAG:     [[Or:j\d+]]        Or [ [[Arg]] [[Arg]] ]
  // CHECK-DAG:                        Return [ [[Or]] ]

  // CHECK-START: long Main.OrSame(long) instruction_simplifier (after)
  // CHECK-DAG:     [[Arg:j\d+]]       ParameterValue
  // CHECK-DAG:                        Return [ [[Arg]] ]

  // CHECK-START: long Main.OrSame(long) instruction_simplifier (after)
  // CHECK-NOT:                        Or

  public static long OrSame(long arg) {
    return arg | arg;
  }

  // CHECK-START: int Main.Shl0(int) instruction_simplifier (before)
  // CHECK-DAG:     [[Arg:i\d+]]      ParameterValue
  // CHECK-DAG:     [[Const0:i\d+]]   IntConstant 0
  // CHECK-DAG:     [[Shl:i\d+]]      Shl [ [[Arg]] [[Const0]] ]
  // CHECK-DAG:                       Return [ [[Shl]] ]

  // CHECK-START: int Main.Shl0(int) instruction_simplifier (after)
  // CHECK-DAG:     [[Arg:i\d+]]      ParameterValue
  // CHECK-DAG:                       Return [ [[Arg]] ]

  // CHECK-START: int Main.Shl0(int) instruction_simplifier (after)
  // CHECK-NOT:                       Shl

  public static int Shl0(int arg) {
    return arg << 0;
  }

  // CHECK-START: int Main.Shl1(int) instruction_simplifier (before)
  // CHECK-DAG:     [[Arg:i\d+]]      ParameterValue
  // CHECK-DAG:     [[Const1:i\d+]]   IntConstant 1
  // CHECK-DAG:     [[Shl:i\d+]]      Shl [ [[Arg]] [[Const1]] ]
  // CHECK-DAG:                       Return [ [[Shl]] ]

  // CHECK-START: int Main.Shl1(int) instruction_simplifier (after)
  // CHECK-DAG:     [[Arg:i\d+]]      ParameterValue
  // CHECK-DAG:     [[Add:i\d+]]      Add [ [[Arg]] [[Arg]] ]
  // CHECK-DAG:                       Return [ [[Add]] ]

  // CHECK-START: int Main.Shl1(int) instruction_simplifier (after)
  // CHECK-NOT:                       Shl

  public static int Shl1(int arg) {
    return arg << 1;
  }

  // CHECK-START: long Main.Shr0(long) instruction_simplifier (before)
  // CHECK-DAG:     [[Arg:j\d+]]      ParameterValue
  // CHECK-DAG:     [[Const0:i\d+]]   IntConstant 0
  // CHECK-DAG:     [[Shr:j\d+]]      Shr [ [[Arg]] [[Const0]] ]
  // CHECK-DAG:                       Return [ [[Shr]] ]

  // CHECK-START: long Main.Shr0(long) instruction_simplifier (after)
  // CHECK-DAG:     [[Arg:j\d+]]      ParameterValue
  // CHECK-DAG:                       Return [ [[Arg]] ]

  // CHECK-START: long Main.Shr0(long) instruction_simplifier (after)
  // CHECK-NOT:                       Shr

  public static long Shr0(long arg) {
    return arg >> 0;
  }

  // CHECK-START: long Main.Sub0(long) instruction_simplifier (before)
  // CHECK-DAG:     [[Arg:j\d+]]      ParameterValue
  // CHECK-DAG:     [[Const0:j\d+]]   LongConstant 0
  // CHECK-DAG:     [[Sub:j\d+]]      Sub [ [[Arg]] [[Const0]] ]
  // CHECK-DAG:                       Return [ [[Sub]] ]

  // CHECK-START: long Main.Sub0(long) instruction_simplifier (after)
  // CHECK-DAG:     [[Arg:j\d+]]      ParameterValue
  // CHECK-DAG:                       Return [ [[Arg]] ]

  // CHECK-START: long Main.Sub0(long) instruction_simplifier (after)
  // CHECK-NOT:                       Sub

  public static long Sub0(long arg) {
    return arg - 0;
  }

  // CHECK-START: int Main.SubAliasNeg(int) instruction_simplifier (before)
  // CHECK-DAG:     [[Arg:i\d+]]      ParameterValue
  // CHECK-DAG:     [[Const0:i\d+]]   IntConstant 0
  // CHECK-DAG:     [[Sub:i\d+]]      Sub [ [[Const0]] [[Arg]] ]
  // CHECK-DAG:                       Return [ [[Sub]] ]

  // CHECK-START: int Main.SubAliasNeg(int) instruction_simplifier (after)
  // CHECK-DAG:     [[Arg:i\d+]]      ParameterValue
  // CHECK-DAG:     [[Neg:i\d+]]      Neg [ [[Arg]] ]
  // CHECK-DAG:                       Return [ [[Neg]] ]

  // CHECK-START: int Main.SubAliasNeg(int) instruction_simplifier (after)
  // CHECK-NOT:                       Sub

  public static int SubAliasNeg(int arg) {
    return 0 - arg;
  }

  // CHECK-START: long Main.UShr0(long) instruction_simplifier (before)
  // CHECK-DAG:     [[Arg:j\d+]]      ParameterValue
  // CHECK-DAG:     [[Const0:i\d+]]   IntConstant 0
  // CHECK-DAG:     [[UShr:j\d+]]     UShr [ [[Arg]] [[Const0]] ]
  // CHECK-DAG:                       Return [ [[UShr]] ]

  // CHECK-START: long Main.UShr0(long) instruction_simplifier (after)
  // CHECK-DAG:     [[Arg:j\d+]]      ParameterValue
  // CHECK-DAG:                       Return [ [[Arg]] ]

  // CHECK-START: long Main.UShr0(long) instruction_simplifier (after)
  // CHECK-NOT:                       UShr

  public static long UShr0(long arg) {
    return arg >>> 0;
  }

  // CHECK-START: int Main.Xor0(int) instruction_simplifier (before)
  // CHECK-DAG:     [[Arg:i\d+]]      ParameterValue
  // CHECK-DAG:     [[Const0:i\d+]]   IntConstant 0
  // CHECK-DAG:     [[Xor:i\d+]]      Xor [ [[Arg]] [[Const0]] ]
  // CHECK-DAG:                       Return [ [[Xor]] ]

  // CHECK-START: int Main.Xor0(int) instruction_simplifier (after)
  // CHECK-DAG:     [[Arg:i\d+]]      ParameterValue
  // CHECK-DAG:                       Return [ [[Arg]] ]

  // CHECK-START: int Main.Xor0(int) instruction_simplifier (after)
  // CHECK-NOT:                       Xor

  public static int Xor0(int arg) {
    return arg ^ 0;
  }

  // CHECK-START: int Main.XorAllOnes(int) instruction_simplifier (before)
  // CHECK-DAG:     [[Arg:i\d+]]      ParameterValue
  // CHECK-DAG:     [[ConstF:i\d+]]   IntConstant -1
  // CHECK-DAG:     [[Xor:i\d+]]      Xor [ [[Arg]] [[ConstF]] ]
  // CHECK-DAG:                       Return [ [[Xor]] ]

  // CHECK-START: int Main.XorAllOnes(int) instruction_simplifier (after)
  // CHECK-DAG:     [[Arg:i\d+]]      ParameterValue
  // CHECK-DAG:     [[Not:i\d+]]      Not [ [[Arg]] ]
  // CHECK-DAG:                       Return [ [[Not]] ]

  // CHECK-START: int Main.XorAllOnes(int) instruction_simplifier (after)
  // CHECK-NOT:                       Xor

  public static int XorAllOnes(int arg) {
    return arg ^ -1;
  }

  /**
   * Test that addition or subtraction operation with both inputs negated are
   * optimized to use a single negation after the operation.
   * The transformation tested is implemented in
   * `InstructionSimplifierVisitor::TryMoveNegOnInputsAfterBinop`.
   */

  // CHECK-START: int Main.AddNegs1(int, int) instruction_simplifier (before)
  // CHECK-DAG:     [[Arg1:i\d+]]     ParameterValue
  // CHECK-DAG:     [[Arg2:i\d+]]     ParameterValue
  // CHECK-DAG:     [[Neg1:i\d+]]     Neg [ [[Arg1]] ]
  // CHECK-DAG:     [[Neg2:i\d+]]     Neg [ [[Arg2]] ]
  // CHECK-DAG:     [[Add:i\d+]]      Add [ [[Neg1]] [[Neg2]] ]
  // CHECK-DAG:                       Return [ [[Add]] ]

  // CHECK-START: int Main.AddNegs1(int, int) instruction_simplifier (after)
  // CHECK-DAG:     [[Arg1:i\d+]]     ParameterValue
  // CHECK-DAG:     [[Arg2:i\d+]]     ParameterValue
  // CHECK-NOT:                       Neg
  // CHECK-DAG:     [[Add:i\d+]]      Add [ [[Arg1]] [[Arg2]] ]
  // CHECK-DAG:     [[Neg:i\d+]]      Neg [ [[Add]] ]
  // CHECK-DAG:                       Return [ [[Neg]] ]

  public static int AddNegs1(int arg1, int arg2) {
    return -arg1 + -arg2;
  }

  /**
   * This is similar to the test-case AddNegs1, but the negations have
   * multiple uses.
   * The transformation tested is implemented in
   * `InstructionSimplifierVisitor::TryMoveNegOnInputsAfterBinop`.
   * The current code won't perform the previous optimization. The
   * transformations do not look at other uses of their inputs. As they don't
   * know what will happen with other uses, they do not take the risk of
   * increasing the register pressure by creating or extending live ranges.
   */

  // CHECK-START: int Main.AddNegs2(int, int) instruction_simplifier (before)
  // CHECK-DAG:     [[Arg1:i\d+]]     ParameterValue
  // CHECK-DAG:     [[Arg2:i\d+]]     ParameterValue
  // CHECK-DAG:     [[Neg1:i\d+]]     Neg [ [[Arg1]] ]
  // CHECK-DAG:     [[Neg2:i\d+]]     Neg [ [[Arg2]] ]
  // CHECK-DAG:     [[Add1:i\d+]]     Add [ [[Neg1]] [[Neg2]] ]
  // CHECK-DAG:     [[Add2:i\d+]]     Add [ [[Neg1]] [[Neg2]] ]
  // CHECK-DAG:     [[Or:i\d+]]       Or [ [[Add1]] [[Add2]] ]
  // CHECK-DAG:                       Return [ [[Or]] ]

  // CHECK-START: int Main.AddNegs2(int, int) instruction_simplifier (after)
  // CHECK-DAG:     [[Arg1:i\d+]]     ParameterValue
  // CHECK-DAG:     [[Arg2:i\d+]]     ParameterValue
  // CHECK-DAG:     [[Neg1:i\d+]]     Neg [ [[Arg1]] ]
  // CHECK-DAG:     [[Neg2:i\d+]]     Neg [ [[Arg2]] ]
  // CHECK-DAG:     [[Add1:i\d+]]     Add [ [[Neg1]] [[Neg2]] ]
  // CHECK-DAG:     [[Add2:i\d+]]     Add [ [[Neg1]] [[Neg2]] ]
  // CHECK-NOT:                       Neg
  // CHECK-DAG:     [[Or:i\d+]]       Or [ [[Add1]] [[Add2]] ]
  // CHECK-DAG:                       Return [ [[Or]] ]

  // CHECK-START: int Main.AddNegs2(int, int) GVN (after)
  // CHECK-DAG:     [[Arg1:i\d+]]     ParameterValue
  // CHECK-DAG:     [[Arg2:i\d+]]     ParameterValue
  // CHECK-DAG:     [[Neg1:i\d+]]     Neg [ [[Arg1]] ]
  // CHECK-DAG:     [[Neg2:i\d+]]     Neg [ [[Arg2]] ]
  // CHECK-DAG:     [[Add:i\d+]]      Add [ [[Neg1]] [[Neg2]] ]
  // CHECK-DAG:     [[Or:i\d+]]       Or [ [[Add]] [[Add]] ]
  // CHECK-DAG:                       Return [ [[Or]] ]

  public static int AddNegs2(int arg1, int arg2) {
    int temp1 = -arg1;
    int temp2 = -arg2;
    return (temp1 + temp2) | (temp1 + temp2);
  }

  /**
   * This follows test-cases AddNegs1 and AddNegs2.
   * The transformation tested is implemented in
   * `InstructionSimplifierVisitor::TryMoveNegOnInputsAfterBinop`.
   * The optimization should not happen if it moves an additional instruction in
   * the loop.
   */

  // CHECK-START: long Main.AddNegs3(long, long) instruction_simplifier (before)
  // -------------- Arguments and initial negation operations.
  // CHECK-DAG:     [[Arg1:j\d+]]     ParameterValue
  // CHECK-DAG:     [[Arg2:j\d+]]     ParameterValue
  // CHECK-DAG:     [[Neg1:j\d+]]     Neg [ [[Arg1]] ]
  // CHECK-DAG:     [[Neg2:j\d+]]     Neg [ [[Arg2]] ]
  // CHECK:                           Goto
  // -------------- Loop
  // CHECK:                           SuspendCheck
  // CHECK:         [[Add:j\d+]]      Add [ [[Neg1]] [[Neg2]] ]
  // CHECK:                           Goto

  // CHECK-START: long Main.AddNegs3(long, long) instruction_simplifier (after)
  // -------------- Arguments and initial negation operations.
  // CHECK-DAG:     [[Arg1:j\d+]]     ParameterValue
  // CHECK-DAG:     [[Arg2:j\d+]]     ParameterValue
  // CHECK-DAG:     [[Neg1:j\d+]]     Neg [ [[Arg1]] ]
  // CHECK-DAG:     [[Neg2:j\d+]]     Neg [ [[Arg2]] ]
  // CHECK:                           Goto
  // -------------- Loop
  // CHECK:                           SuspendCheck
  // CHECK:         [[Add:j\d+]]      Add [ [[Neg1]] [[Neg2]] ]
  // CHECK-NOT:                       Neg
  // CHECK:                           Goto

  public static long AddNegs3(long arg1, long arg2) {
    long res = 0;
    long n_arg1 = -arg1;
    long n_arg2 = -arg2;
    for (long i = 0; i < 1; i++) {
      res += n_arg1 + n_arg2 + i;
    }
    return res;
  }

  /**
   * Test the simplification of an addition with a negated argument into a
   * subtraction.
   * The transformation tested is implemented in `InstructionSimplifierVisitor::VisitAdd`.
   */

  // CHECK-START: long Main.AddNeg1(long, long) instruction_simplifier (before)
  // CHECK-DAG:     [[Arg1:j\d+]]     ParameterValue
  // CHECK-DAG:     [[Arg2:j\d+]]     ParameterValue
  // CHECK-DAG:     [[Neg:j\d+]]      Neg [ [[Arg1]] ]
  // CHECK-DAG:     [[Add:j\d+]]      Add [ [[Neg]] [[Arg2]] ]
  // CHECK-DAG:                       Return [ [[Add]] ]

  // CHECK-START: long Main.AddNeg1(long, long) instruction_simplifier (after)
  // CHECK-DAG:     [[Arg1:j\d+]]     ParameterValue
  // CHECK-DAG:     [[Arg2:j\d+]]     ParameterValue
  // CHECK-DAG:     [[Sub:j\d+]]      Sub [ [[Arg2]] [[Arg1]] ]
  // CHECK-DAG:                       Return [ [[Sub]] ]

  // CHECK-START: long Main.AddNeg1(long, long) instruction_simplifier (after)
  // CHECK-NOT:                       Neg
  // CHECK-NOT:                       Add

  public static long AddNeg1(long arg1, long arg2) {
    return -arg1 + arg2;
  }

  /**
   * This is similar to the test-case AddNeg1, but the negation has two uses.
   * The transformation tested is implemented in `InstructionSimplifierVisitor::VisitAdd`.
   * The current code won't perform the previous optimization. The
   * transformations do not look at other uses of their inputs. As they don't
   * know what will happen with other uses, they do not take the risk of
   * increasing the register pressure by creating or extending live ranges.
   */

  // CHECK-START: long Main.AddNeg2(long, long) instruction_simplifier (before)
  // CHECK-DAG:     [[Arg1:j\d+]]     ParameterValue
  // CHECK-DAG:     [[Arg2:j\d+]]     ParameterValue
  // CHECK-DAG:     [[Neg:j\d+]]      Neg [ [[Arg2]] ]
  // CHECK-DAG:     [[Add1:j\d+]]     Add [ [[Arg1]] [[Neg]] ]
  // CHECK-DAG:     [[Add2:j\d+]]     Add [ [[Arg1]] [[Neg]] ]
  // CHECK-DAG:     [[Res:j\d+]]      Or [ [[Add1]] [[Add2]] ]
  // CHECK-DAG:                       Return [ [[Res]] ]

  // CHECK-START: long Main.AddNeg2(long, long) instruction_simplifier (after)
  // CHECK-DAG:     [[Arg1:j\d+]]     ParameterValue
  // CHECK-DAG:     [[Arg2:j\d+]]     ParameterValue
  // CHECK-DAG:     [[Neg:j\d+]]      Neg [ [[Arg2]] ]
  // CHECK-DAG:     [[Add1:j\d+]]     Add [ [[Arg1]] [[Neg]] ]
  // CHECK-DAG:     [[Add2:j\d+]]     Add [ [[Arg1]] [[Neg]] ]
  // CHECK-DAG:     [[Res:j\d+]]      Or [ [[Add1]] [[Add2]] ]
  // CHECK-DAG:                       Return [ [[Res]] ]

  // CHECK-START: long Main.AddNeg2(long, long) instruction_simplifier (after)
  // CHECK-NOT:                       Sub

  public static long AddNeg2(long arg1, long arg2) {
    long temp = -arg2;
    return (arg1 + temp) | (arg1 + temp);
  }

  /**
   * Test simplification of the `-(-var)` pattern.
   * The transformation tested is implemented in `InstructionSimplifierVisitor::VisitNeg`.
   */

  // CHECK-START: long Main.NegNeg1(long) instruction_simplifier (before)
  // CHECK-DAG:     [[Arg:j\d+]]      ParameterValue
  // CHECK-DAG:     [[Neg1:j\d+]]     Neg [ [[Arg]] ]
  // CHECK-DAG:     [[Neg2:j\d+]]     Neg [ [[Neg1]] ]
  // CHECK-DAG:                       Return [ [[Neg2]] ]

  // CHECK-START: long Main.NegNeg1(long) instruction_simplifier (after)
  // CHECK-DAG:     [[Arg:j\d+]]      ParameterValue
  // CHECK-DAG:                       Return [ [[Arg]] ]

  // CHECK-START: long Main.NegNeg1(long) instruction_simplifier (after)
  // CHECK-NOT:                       Neg

  public static long NegNeg1(long arg) {
    return -(-arg);
  }

  /**
   * Test 'multi-step' simplification, where a first transformation yields a
   * new simplification possibility for the current instruction.
   * The transformations tested are implemented in `InstructionSimplifierVisitor::VisitNeg`
   * and in `InstructionSimplifierVisitor::VisitAdd`.
   */

  // CHECK-START: int Main.NegNeg2(int) instruction_simplifier (before)
  // CHECK-DAG:     [[Arg:i\d+]]      ParameterValue
  // CHECK-DAG:     [[Neg1:i\d+]]     Neg [ [[Arg]] ]
  // CHECK-DAG:     [[Neg2:i\d+]]     Neg [ [[Neg1]] ]
  // CHECK-DAG:     [[Add:i\d+]]      Add [ [[Neg1]] [[Neg2]] ]
  // CHECK-DAG:                       Return [ [[Add]] ]

  // CHECK-START: int Main.NegNeg2(int) instruction_simplifier (after)
  // CHECK-DAG:     [[Arg:i\d+]]      ParameterValue
  // CHECK-DAG:     [[Sub:i\d+]]      Sub [ [[Arg]] [[Arg]] ]
  // CHECK-DAG:                       Return [ [[Sub]] ]

  // CHECK-START: int Main.NegNeg2(int) instruction_simplifier (after)
  // CHECK-NOT:                       Neg
  // CHECK-NOT:                       Add

  // CHECK-START: int Main.NegNeg2(int) constant_folding_after_inlining (after)
  // CHECK:         [[Const0:i\d+]]   IntConstant 0
  // CHECK-NOT:                       Neg
  // CHECK-NOT:                       Add
  // CHECK:                           Return [ [[Const0]] ]

  public static int NegNeg2(int arg) {
    int temp = -arg;
    return temp + -temp;
  }

  /**
   * Test another 'multi-step' simplification, where a first transformation
   * yields a new simplification possibility for the current instruction.
   * The transformations tested are implemented in `InstructionSimplifierVisitor::VisitNeg`
   * and in `InstructionSimplifierVisitor::VisitSub`.
   */

  // CHECK-START: long Main.NegNeg3(long) instruction_simplifier (before)
  // CHECK-DAG:     [[Arg:j\d+]]      ParameterValue
  // CHECK-DAG:     [[Const0:j\d+]]   LongConstant 0
  // CHECK-DAG:     [[Neg:j\d+]]      Neg [ [[Arg]] ]
  // CHECK-DAG:     [[Sub:j\d+]]      Sub [ [[Const0]] [[Neg]] ]
  // CHECK-DAG:                       Return [ [[Sub]] ]

  // CHECK-START: long Main.NegNeg3(long) instruction_simplifier (after)
  // CHECK-DAG:     [[Arg:j\d+]]      ParameterValue
  // CHECK-DAG:                       Return [ [[Arg]] ]

  // CHECK-START: long Main.NegNeg3(long) instruction_simplifier (after)
  // CHECK-NOT:                       Neg
  // CHECK-NOT:                       Sub

  public static long NegNeg3(long arg) {
    return 0 - -arg;
  }

  /**
   * Test that a negated subtraction is simplified to a subtraction with its
   * arguments reversed.
   * The transformation tested is implemented in `InstructionSimplifierVisitor::VisitNeg`.
   */

  // CHECK-START: int Main.NegSub1(int, int) instruction_simplifier (before)
  // CHECK-DAG:     [[Arg1:i\d+]]     ParameterValue
  // CHECK-DAG:     [[Arg2:i\d+]]     ParameterValue
  // CHECK-DAG:     [[Sub:i\d+]]      Sub [ [[Arg1]] [[Arg2]] ]
  // CHECK-DAG:     [[Neg:i\d+]]      Neg [ [[Sub]] ]
  // CHECK-DAG:                       Return [ [[Neg]] ]

  // CHECK-START: int Main.NegSub1(int, int) instruction_simplifier (after)
  // CHECK-DAG:     [[Arg1:i\d+]]     ParameterValue
  // CHECK-DAG:     [[Arg2:i\d+]]     ParameterValue
  // CHECK-DAG:     [[Sub:i\d+]]      Sub [ [[Arg2]] [[Arg1]] ]
  // CHECK-DAG:                       Return [ [[Sub]] ]

  // CHECK-START: int Main.NegSub1(int, int) instruction_simplifier (after)
  // CHECK-NOT:                       Neg

  public static int NegSub1(int arg1, int arg2) {
    return -(arg1 - arg2);
  }

  /**
   * This is similar to the test-case NegSub1, but the subtraction has
   * multiple uses.
   * The transformation tested is implemented in `InstructionSimplifierVisitor::VisitNeg`.
   * The current code won't perform the previous optimization. The
   * transformations do not look at other uses of their inputs. As they don't
   * know what will happen with other uses, they do not take the risk of
   * increasing the register pressure by creating or extending live ranges.
   */

  // CHECK-START: int Main.NegSub2(int, int) instruction_simplifier (before)
  // CHECK-DAG:     [[Arg1:i\d+]]     ParameterValue
  // CHECK-DAG:     [[Arg2:i\d+]]     ParameterValue
  // CHECK-DAG:     [[Sub:i\d+]]      Sub [ [[Arg1]] [[Arg2]] ]
  // CHECK-DAG:     [[Neg1:i\d+]]     Neg [ [[Sub]] ]
  // CHECK-DAG:     [[Neg2:i\d+]]     Neg [ [[Sub]] ]
  // CHECK-DAG:     [[Or:i\d+]]       Or [ [[Neg1]] [[Neg2]] ]
  // CHECK-DAG:                       Return [ [[Or]] ]

  // CHECK-START: int Main.NegSub2(int, int) instruction_simplifier (after)
  // CHECK-DAG:     [[Arg1:i\d+]]     ParameterValue
  // CHECK-DAG:     [[Arg2:i\d+]]     ParameterValue
  // CHECK-DAG:     [[Sub:i\d+]]      Sub [ [[Arg1]] [[Arg2]] ]
  // CHECK-DAG:     [[Neg1:i\d+]]     Neg [ [[Sub]] ]
  // CHECK-DAG:     [[Neg2:i\d+]]     Neg [ [[Sub]] ]
  // CHECK-DAG:     [[Or:i\d+]]       Or [ [[Neg1]] [[Neg2]] ]
  // CHECK-DAG:                       Return [ [[Or]] ]

  public static int NegSub2(int arg1, int arg2) {
    int temp = arg1 - arg2;
    return -temp | -temp;
  }

  /**
   * Test simplification of the `~~var` pattern.
   * The transformation tested is implemented in `InstructionSimplifierVisitor::VisitNot`.
   */

  // CHECK-START: long Main.NotNot1(long) instruction_simplifier (before)
  // CHECK-DAG:     [[Arg:j\d+]]      ParameterValue
  // CHECK-DAG:     [[ConstF1:j\d+]]  LongConstant -1
  // CHECK-DAG:     [[Xor1:j\d+]]     Xor [ [[Arg]] [[ConstF1]] ]
  // CHECK-DAG:     [[Xor2:j\d+]]     Xor [ [[Xor1]] [[ConstF1]] ]
  // CHECK-DAG:                       Return [ [[Xor2]] ]

  // CHECK-START: long Main.NotNot1(long) instruction_simplifier (after)
  // CHECK-DAG:     [[Arg:j\d+]]      ParameterValue
  // CHECK-DAG:                       Return [ [[Arg]] ]

  // CHECK-START: long Main.NotNot1(long) instruction_simplifier (after)
  // CHECK-NOT:                       Xor

  public static long NotNot1(long arg) {
    return ~~arg;
  }

  // CHECK-START: int Main.NotNot2(int) instruction_simplifier (before)
  // CHECK-DAG:     [[Arg:i\d+]]      ParameterValue
  // CHECK-DAG:     [[ConstF1:i\d+]]  IntConstant -1
  // CHECK-DAG:     [[Xor1:i\d+]]     Xor [ [[Arg]] [[ConstF1]] ]
  // CHECK-DAG:     [[Xor2:i\d+]]     Xor [ [[Xor1]] [[ConstF1]] ]
  // CHECK-DAG:     [[Add:i\d+]]      Add [ [[Xor1]] [[Xor2]] ]
  // CHECK-DAG:                       Return [ [[Add]] ]

  // CHECK-START: int Main.NotNot2(int) instruction_simplifier (after)
  // CHECK-DAG:     [[Arg:i\d+]]      ParameterValue
  // CHECK-DAG:     [[Not:i\d+]]      Not [ [[Arg]] ]
  // CHECK-DAG:     [[Add:i\d+]]      Add [ [[Not]] [[Arg]] ]
  // CHECK-DAG:                       Return [ [[Add]] ]

  // CHECK-START: int Main.NotNot2(int) instruction_simplifier (after)
  // CHECK-NOT:                       Xor

  public static int NotNot2(int arg) {
    int temp = ~arg;
    return temp + ~temp;
  }

  /**
   * Test the simplification of a subtraction with a negated argument.
   * The transformation tested is implemented in `InstructionSimplifierVisitor::VisitSub`.
   */

  // CHECK-START: int Main.SubNeg1(int, int) instruction_simplifier (before)
  // CHECK-DAG:     [[Arg1:i\d+]]     ParameterValue
  // CHECK-DAG:     [[Arg2:i\d+]]     ParameterValue
  // CHECK-DAG:     [[Neg:i\d+]]      Neg [ [[Arg1]] ]
  // CHECK-DAG:     [[Sub:i\d+]]      Sub [ [[Neg]] [[Arg2]] ]
  // CHECK-DAG:                       Return [ [[Sub]] ]

  // CHECK-START: int Main.SubNeg1(int, int) instruction_simplifier (after)
  // CHECK-DAG:     [[Arg1:i\d+]]     ParameterValue
  // CHECK-DAG:     [[Arg2:i\d+]]     ParameterValue
  // CHECK-DAG:     [[Add:i\d+]]      Add [ [[Arg1]] [[Arg2]] ]
  // CHECK-DAG:     [[Neg:i\d+]]      Neg [ [[Add]] ]
  // CHECK-DAG:                       Return [ [[Neg]] ]

  // CHECK-START: int Main.SubNeg1(int, int) instruction_simplifier (after)
  // CHECK-NOT:                       Sub

  public static int SubNeg1(int arg1, int arg2) {
    return -arg1 - arg2;
  }

  /**
   * This is similar to the test-case SubNeg1, but the negation has
   * multiple uses.
   * The transformation tested is implemented in `InstructionSimplifierVisitor::VisitSub`.
   * The current code won't perform the previous optimization. The
   * transformations do not look at other uses of their inputs. As they don't
   * know what will happen with other uses, they do not take the risk of
   * increasing the register pressure by creating or extending live ranges.
   */

  // CHECK-START: int Main.SubNeg2(int, int) instruction_simplifier (before)
  // CHECK-DAG:     [[Arg1:i\d+]]     ParameterValue
  // CHECK-DAG:     [[Arg2:i\d+]]     ParameterValue
  // CHECK-DAG:     [[Neg:i\d+]]      Neg [ [[Arg1]] ]
  // CHECK-DAG:     [[Sub1:i\d+]]     Sub [ [[Neg]] [[Arg2]] ]
  // CHECK-DAG:     [[Sub2:i\d+]]     Sub [ [[Neg]] [[Arg2]] ]
  // CHECK-DAG:     [[Or:i\d+]]       Or [ [[Sub1]] [[Sub2]] ]
  // CHECK-DAG:                       Return [ [[Or]] ]

  // CHECK-START: int Main.SubNeg2(int, int) instruction_simplifier (after)
  // CHECK-DAG:     [[Arg1:i\d+]]     ParameterValue
  // CHECK-DAG:     [[Arg2:i\d+]]     ParameterValue
  // CHECK-DAG:     [[Neg:i\d+]]      Neg [ [[Arg1]] ]
  // CHECK-DAG:     [[Sub1:i\d+]]     Sub [ [[Neg]] [[Arg2]] ]
  // CHECK-DAG:     [[Sub2:i\d+]]     Sub [ [[Neg]] [[Arg2]] ]
  // CHECK-DAG:     [[Or:i\d+]]       Or [ [[Sub1]] [[Sub2]] ]
  // CHECK-DAG:                       Return [ [[Or]] ]

  // CHECK-START: int Main.SubNeg2(int, int) instruction_simplifier (after)
  // CHECK-NOT:                       Add

  public static int SubNeg2(int arg1, int arg2) {
    int temp = -arg1;
    return (temp - arg2) | (temp - arg2);
  }

  /**
   * This follows test-cases SubNeg1 and SubNeg2.
   * The transformation tested is implemented in `InstructionSimplifierVisitor::VisitSub`.
   * The optimization should not happen if it moves an additional instruction in
   * the loop.
   */

  // CHECK-START: long Main.SubNeg3(long, long) instruction_simplifier (before)
  // -------------- Arguments and initial negation operation.
  // CHECK-DAG:     [[Arg1:j\d+]]     ParameterValue
  // CHECK-DAG:     [[Arg2:j\d+]]     ParameterValue
  // CHECK-DAG:     [[Neg:j\d+]]      Neg [ [[Arg1]] ]
  // CHECK:                           Goto
  // -------------- Loop
  // CHECK:                           SuspendCheck
  // CHECK:         [[Sub:j\d+]]      Sub [ [[Neg]] [[Arg2]] ]
  // CHECK:                           Goto

  // CHECK-START: long Main.SubNeg3(long, long) instruction_simplifier (after)
  // -------------- Arguments and initial negation operation.
  // CHECK-DAG:     [[Arg1:j\d+]]     ParameterValue
  // CHECK-DAG:     [[Arg2:j\d+]]     ParameterValue
  // CHECK-DAG:     [[Neg:j\d+]]      Neg [ [[Arg1]] ]
  // CHECK-DAG:                       Goto
  // -------------- Loop
  // CHECK:                           SuspendCheck
  // CHECK:         [[Sub:j\d+]]      Sub [ [[Neg]] [[Arg2]] ]
  // CHECK-NOT:                       Neg
  // CHECK:                           Goto

  public static long SubNeg3(long arg1, long arg2) {
    long res = 0;
    long temp = -arg1;
    for (long i = 0; i < 1; i++) {
      res += temp - arg2 - i;
    }
    return res;
  }

  // CHECK-START: int Main.EqualTrueRhs(boolean) instruction_simplifier (before)
  // CHECK-DAG:     [[Arg:z\d+]]      ParameterValue
  // CHECK-DAG:     [[Const1:i\d+]]   IntConstant 1
  // CHECK-DAG:     [[Cond:z\d+]]     Equal [ [[Arg]] [[Const1]] ]
  // CHECK-DAG:                       If [ [[Cond]] ]

  // CHECK-START: int Main.EqualTrueRhs(boolean) instruction_simplifier (after)
  // CHECK-DAG:     [[Arg:z\d+]]      ParameterValue
  // CHECK-DAG:                       If [ [[Arg]] ]

  public static int EqualTrueRhs(boolean arg) {
    return (arg != true) ? 3 : 5;
  }

  // CHECK-START: int Main.EqualTrueLhs(boolean) instruction_simplifier (before)
  // CHECK-DAG:     [[Arg:z\d+]]      ParameterValue
  // CHECK-DAG:     [[Const1:i\d+]]   IntConstant 1
  // CHECK-DAG:     [[Cond:z\d+]]     Equal [ [[Const1]] [[Arg]] ]
  // CHECK-DAG:                       If [ [[Cond]] ]

  // CHECK-START: int Main.EqualTrueLhs(boolean) instruction_simplifier (after)
  // CHECK-DAG:     [[Arg:z\d+]]      ParameterValue
  // CHECK-DAG:                       If [ [[Arg]] ]

  public static int EqualTrueLhs(boolean arg) {
    return (true != arg) ? 3 : 5;
  }

  // CHECK-START: int Main.EqualFalseRhs(boolean) instruction_simplifier (before)
  // CHECK-DAG:     [[Arg:z\d+]]      ParameterValue
  // CHECK-DAG:     [[Const0:i\d+]]   IntConstant 0
  // CHECK-DAG:     [[Cond:z\d+]]     Equal [ [[Arg]] [[Const0]] ]
  // CHECK-DAG:                       If [ [[Cond]] ]

  // CHECK-START: int Main.EqualFalseRhs(boolean) instruction_simplifier (after)
  // CHECK-DAG:     [[Arg:z\d+]]      ParameterValue
  // CHECK-DAG:     [[NotArg:z\d+]]   BooleanNot [ [[Arg]] ]
  // CHECK-DAG:                       If [ [[NotArg]] ]

  public static int EqualFalseRhs(boolean arg) {
    return (arg != false) ? 3 : 5;
  }

  // CHECK-START: int Main.EqualFalseLhs(boolean) instruction_simplifier (before)
  // CHECK-DAG:     [[Arg:z\d+]]      ParameterValue
  // CHECK-DAG:     [[Const0:i\d+]]   IntConstant 0
  // CHECK-DAG:     [[Cond:z\d+]]     Equal [ [[Const0]] [[Arg]] ]
  // CHECK-DAG:                       If [ [[Cond]] ]

  // CHECK-START: int Main.EqualFalseLhs(boolean) instruction_simplifier (after)
  // CHECK-DAG:     [[Arg:z\d+]]      ParameterValue
  // CHECK-DAG:     [[NotArg:z\d+]]   BooleanNot [ [[Arg]] ]
  // CHECK-DAG:                       If [ [[NotArg]] ]

  public static int EqualFalseLhs(boolean arg) {
    return (false != arg) ? 3 : 5;
  }

  // CHECK-START: int Main.NotEqualTrueRhs(boolean) instruction_simplifier (before)
  // CHECK-DAG:     [[Arg:z\d+]]      ParameterValue
  // CHECK-DAG:     [[Const1:i\d+]]   IntConstant 1
  // CHECK-DAG:     [[Cond:z\d+]]     NotEqual [ [[Arg]] [[Const1]] ]
  // CHECK-DAG:                       If [ [[Cond]] ]

  // CHECK-START: int Main.NotEqualTrueRhs(boolean) instruction_simplifier (after)
  // CHECK-DAG:     [[Arg:z\d+]]      ParameterValue
  // CHECK-DAG:     [[NotArg:z\d+]]   BooleanNot [ [[Arg]] ]
  // CHECK-DAG:                       If [ [[NotArg]] ]

  public static int NotEqualTrueRhs(boolean arg) {
    return (arg == true) ? 3 : 5;
  }

  // CHECK-START: int Main.NotEqualTrueLhs(boolean) instruction_simplifier (before)
  // CHECK-DAG:     [[Arg:z\d+]]      ParameterValue
  // CHECK-DAG:     [[Const1:i\d+]]   IntConstant 1
  // CHECK-DAG:     [[Cond:z\d+]]     NotEqual [ [[Const1]] [[Arg]] ]
  // CHECK-DAG:                       If [ [[Cond]] ]

  // CHECK-START: int Main.NotEqualTrueLhs(boolean) instruction_simplifier (after)
  // CHECK-DAG:     [[Arg:z\d+]]      ParameterValue
  // CHECK-DAG:     [[NotArg:z\d+]]   BooleanNot [ [[Arg]] ]
  // CHECK-DAG:                       If [ [[NotArg]] ]

  public static int NotEqualTrueLhs(boolean arg) {
    return (true == arg) ? 3 : 5;
  }

  // CHECK-START: int Main.NotEqualFalseRhs(boolean) instruction_simplifier (before)
  // CHECK-DAG:     [[Arg:z\d+]]      ParameterValue
  // CHECK-DAG:     [[Const0:i\d+]]   IntConstant 0
  // CHECK-DAG:     [[Cond:z\d+]]     NotEqual [ [[Arg]] [[Const0]] ]
  // CHECK-DAG:                       If [ [[Cond]] ]

  // CHECK-START: int Main.NotEqualFalseRhs(boolean) instruction_simplifier (after)
  // CHECK-DAG:     [[Arg:z\d+]]      ParameterValue
  // CHECK-DAG:                       If [ [[Arg]] ]

  public static int NotEqualFalseRhs(boolean arg) {
    return (arg == false) ? 3 : 5;
  }

  // CHECK-START: int Main.NotEqualFalseLhs(boolean) instruction_simplifier (before)
  // CHECK-DAG:     [[Arg:z\d+]]      ParameterValue
  // CHECK-DAG:     [[Const0:i\d+]]   IntConstant 0
  // CHECK-DAG:     [[Cond:z\d+]]     NotEqual [ [[Const0]] [[Arg]] ]
  // CHECK-DAG:                       If [ [[Cond]] ]

  // CHECK-START: int Main.NotEqualFalseLhs(boolean) instruction_simplifier (after)
  // CHECK-DAG:     [[Arg:z\d+]]      ParameterValue
  // CHECK-DAG:                       If [ [[Arg]] ]

  public static int NotEqualFalseLhs(boolean arg) {
    return (false == arg) ? 3 : 5;
  }

  /*
   * Test simplification of double Boolean negation. Note that sometimes
   * both negations can be removed but we only expect the simplifier to
   * remove the second.
   */

  // CHECK-START: boolean Main.NotNotBool(boolean) instruction_simplifier_after_types (before)
  // CHECK-DAG:     [[Arg:z\d+]]       ParameterValue
  // CHECK-DAG:     [[NotArg:z\d+]]    BooleanNot [ [[Arg]] ]
  // CHECK-DAG:     [[NotNotArg:z\d+]] BooleanNot [ [[NotArg]] ]
  // CHECK-DAG:                        Return [ [[NotNotArg]] ]

  // CHECK-START: boolean Main.NotNotBool(boolean) instruction_simplifier_after_types (after)
  // CHECK-DAG:     [[Arg:z\d+]]       ParameterValue
  // CHECK-DAG:                        BooleanNot [ [[Arg]] ]
  // CHECK-DAG:                        Return [ [[Arg]] ]

  // CHECK-START: boolean Main.NotNotBool(boolean) instruction_simplifier_after_types (after)
  // CHECK:                            BooleanNot
  // CHECK-NOT:                        BooleanNot

  public static boolean NegateValue(boolean arg) {
    return !arg;
  }

  public static boolean NotNotBool(boolean arg) {
    return !(NegateValue(arg));
  }

  // CHECK-START: float Main.Div2(float) instruction_simplifier (before)
  // CHECK-DAG:      [[Arg:f\d+]]      ParameterValue
  // CHECK-DAG:      [[Const2:f\d+]]   FloatConstant 2
  // CHECK-DAG:      [[Div:f\d+]]      Div [ [[Arg]] [[Const2]] ]
  // CHECK-DAG:                        Return [ [[Div]] ]

  // CHECK-START: float Main.Div2(float) instruction_simplifier (after)
  // CHECK-DAG:      [[Arg:f\d+]]      ParameterValue
  // CHECK-DAG:      [[ConstP5:f\d+]]  FloatConstant 0.5
  // CHECK-DAG:      [[Mul:f\d+]]      Mul [ [[Arg]] [[ConstP5]] ]
  // CHECK-DAG:                        Return [ [[Mul]] ]

  // CHECK-START: float Main.Div2(float) instruction_simplifier (after)
  // CHECK-NOT:                        Div

  public static float Div2(float arg) {
    return arg / 2.0f;
  }

  // CHECK-START: double Main.Div2(double) instruction_simplifier (before)
  // CHECK-DAG:      [[Arg:d\d+]]      ParameterValue
  // CHECK-DAG:      [[Const2:d\d+]]   DoubleConstant 2
  // CHECK-DAG:      [[Div:d\d+]]      Div [ [[Arg]] [[Const2]] ]
  // CHECK-DAG:                        Return [ [[Div]] ]

  // CHECK-START: double Main.Div2(double) instruction_simplifier (after)
  // CHECK-DAG:      [[Arg:d\d+]]      ParameterValue
  // CHECK-DAG:      [[ConstP5:d\d+]]  DoubleConstant 0.5
  // CHECK-DAG:      [[Mul:d\d+]]      Mul [ [[Arg]] [[ConstP5]] ]
  // CHECK-DAG:                        Return [ [[Mul]] ]

  // CHECK-START: double Main.Div2(double) instruction_simplifier (after)
  // CHECK-NOT:                        Div
  public static double Div2(double arg) {
    return arg / 2.0;
  }

  // CHECK-START: float Main.DivMP25(float) instruction_simplifier (before)
  // CHECK-DAG:      [[Arg:f\d+]]      ParameterValue
  // CHECK-DAG:      [[ConstMP25:f\d+]]   FloatConstant -0.25
  // CHECK-DAG:      [[Div:f\d+]]      Div [ [[Arg]] [[ConstMP25]] ]
  // CHECK-DAG:                        Return [ [[Div]] ]

  // CHECK-START: float Main.DivMP25(float) instruction_simplifier (after)
  // CHECK-DAG:      [[Arg:f\d+]]      ParameterValue
  // CHECK-DAG:      [[ConstM4:f\d+]]  FloatConstant -4
  // CHECK-DAG:      [[Mul:f\d+]]      Mul [ [[Arg]] [[ConstM4]] ]
  // CHECK-DAG:                        Return [ [[Mul]] ]

  // CHECK-START: float Main.DivMP25(float) instruction_simplifier (after)
  // CHECK-NOT:                        Div

  public static float DivMP25(float arg) {
    return arg / -0.25f;
  }

  // CHECK-START: double Main.DivMP25(double) instruction_simplifier (before)
  // CHECK-DAG:      [[Arg:d\d+]]      ParameterValue
  // CHECK-DAG:      [[ConstMP25:d\d+]]   DoubleConstant -0.25
  // CHECK-DAG:      [[Div:d\d+]]      Div [ [[Arg]] [[ConstMP25]] ]
  // CHECK-DAG:                        Return [ [[Div]] ]

  // CHECK-START: double Main.DivMP25(double) instruction_simplifier (after)
  // CHECK-DAG:      [[Arg:d\d+]]      ParameterValue
  // CHECK-DAG:      [[ConstM4:d\d+]]  DoubleConstant -4
  // CHECK-DAG:      [[Mul:d\d+]]      Mul [ [[Arg]] [[ConstM4]] ]
  // CHECK-DAG:                        Return [ [[Mul]] ]

  // CHECK-START: double Main.DivMP25(double) instruction_simplifier (after)
  // CHECK-NOT:                        Div
  public static double DivMP25(double arg) {
    return arg / -0.25f;
  }

  public static void main(String[] args) {
    int arg = 123456;

    assertLongEquals(Add0(arg), arg);
    assertIntEquals(AndAllOnes(arg), arg);
    assertLongEquals(Div1(arg), arg);
    assertIntEquals(DivN1(arg), -arg);
    assertLongEquals(Mul1(arg), arg);
    assertIntEquals(MulN1(arg), -arg);
    assertLongEquals(MulPowerOfTwo128(arg), (128 * arg));
    assertIntEquals(Or0(arg), arg);
    assertLongEquals(OrSame(arg), arg);
    assertIntEquals(Shl0(arg), arg);
    assertLongEquals(Shr0(arg), arg);
    assertLongEquals(Sub0(arg), arg);
    assertIntEquals(SubAliasNeg(arg), -arg);
    assertLongEquals(UShr0(arg), arg);
    assertIntEquals(Xor0(arg), arg);
    assertIntEquals(XorAllOnes(arg), ~arg);
    assertIntEquals(AddNegs1(arg, arg + 1), -(arg + arg + 1));
    assertIntEquals(AddNegs2(arg, arg + 1), -(arg + arg + 1));
    assertLongEquals(AddNegs3(arg, arg + 1), -(2 * arg + 1));
    assertLongEquals(AddNeg1(arg, arg + 1), 1);
    assertLongEquals(AddNeg2(arg, arg + 1), -1);
    assertLongEquals(NegNeg1(arg), arg);
    assertIntEquals(NegNeg2(arg), 0);
    assertLongEquals(NegNeg3(arg), arg);
    assertIntEquals(NegSub1(arg, arg + 1), 1);
    assertIntEquals(NegSub2(arg, arg + 1), 1);
    assertLongEquals(NotNot1(arg), arg);
    assertIntEquals(NotNot2(arg), -1);
    assertIntEquals(SubNeg1(arg, arg + 1), -(arg + arg + 1));
    assertIntEquals(SubNeg2(arg, arg + 1), -(arg + arg + 1));
    assertLongEquals(SubNeg3(arg, arg + 1), -(2 * arg + 1));
    assertIntEquals(EqualTrueRhs(true), 5);
    assertIntEquals(EqualTrueLhs(true), 5);
    assertIntEquals(EqualFalseRhs(true), 3);
    assertIntEquals(EqualFalseLhs(true), 3);
    assertIntEquals(NotEqualTrueRhs(true), 3);
    assertIntEquals(NotEqualTrueLhs(true), 3);
    assertIntEquals(NotEqualFalseRhs(true), 5);
    assertIntEquals(NotEqualFalseLhs(true), 5);
    assertBooleanEquals(NotNotBool(true), true);
    assertBooleanEquals(NotNotBool(false), false);
    assertFloatEquals(Div2(100.0f), 50.0f);
    assertDoubleEquals(Div2(150.0), 75.0);
    assertFloatEquals(DivMP25(100.0f), -400.0f);
    assertDoubleEquals(DivMP25(150.0), -600.0);
    assertLongEquals(Shl1(100), 200);
  }
}
