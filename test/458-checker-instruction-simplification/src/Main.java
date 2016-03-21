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

import java.lang.reflect.Method;

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

  public static void assertStringEquals(String expected, String result) {
    if (expected == null ? result != null : !expected.equals(result)) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  /**
   * Tiny programs exercising optimizations of arithmetic identities.
   */

  /// CHECK-START: long Main.Add0(long) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:j\d+>>     ParameterValue
  /// CHECK-DAG:     <<Const0:j\d+>>  LongConstant 0
  /// CHECK-DAG:     <<Add:j\d+>>     Add [<<Const0>>,<<Arg>>]
  /// CHECK-DAG:                      Return [<<Add>>]

  /// CHECK-START: long Main.Add0(long) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:j\d+>>     ParameterValue
  /// CHECK-DAG:                      Return [<<Arg>>]

  /// CHECK-START: long Main.Add0(long) instruction_simplifier (after)
  /// CHECK-NOT:                        Add

  public static long Add0(long arg) {
    return 0 + arg;
  }

  /// CHECK-START: int Main.AndAllOnes(int) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:i\d+>>     ParameterValue
  /// CHECK-DAG:     <<ConstF:i\d+>>  IntConstant -1
  /// CHECK-DAG:     <<And:i\d+>>     And [<<Arg>>,<<ConstF>>]
  /// CHECK-DAG:                      Return [<<And>>]

  /// CHECK-START: int Main.AndAllOnes(int) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:i\d+>>     ParameterValue
  /// CHECK-DAG:                      Return [<<Arg>>]

  /// CHECK-START: int Main.AndAllOnes(int) instruction_simplifier (after)
  /// CHECK-NOT:                      And

  public static int AndAllOnes(int arg) {
    return arg & -1;
  }

  /// CHECK-START: int Main.UShr28And15(int) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const28:i\d+>>  IntConstant 28
  /// CHECK-DAG:     <<Const15:i\d+>>  IntConstant 15
  /// CHECK-DAG:     <<UShr:i\d+>>     UShr [<<Arg>>,<<Const28>>]
  /// CHECK-DAG:     <<And:i\d+>>      And [<<UShr>>,<<Const15>>]
  /// CHECK-DAG:                       Return [<<And>>]

  /// CHECK-START: int Main.UShr28And15(int) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const28:i\d+>>  IntConstant 28
  /// CHECK-DAG:     <<UShr:i\d+>>     UShr [<<Arg>>,<<Const28>>]
  /// CHECK-DAG:                       Return [<<UShr>>]

  /// CHECK-START: int Main.UShr28And15(int) instruction_simplifier (after)
  /// CHECK-NOT:                       And

  public static int UShr28And15(int arg) {
    return (arg >>> 28) & 15;
  }

  /// CHECK-START: long Main.UShr60And15(long) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const60:i\d+>>  IntConstant 60
  /// CHECK-DAG:     <<Const15:j\d+>>  LongConstant 15
  /// CHECK-DAG:     <<UShr:j\d+>>     UShr [<<Arg>>,<<Const60>>]
  /// CHECK-DAG:     <<And:j\d+>>      And [<<UShr>>,<<Const15>>]
  /// CHECK-DAG:                       Return [<<And>>]

  /// CHECK-START: long Main.UShr60And15(long) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const60:i\d+>>  IntConstant 60
  /// CHECK-DAG:     <<UShr:j\d+>>     UShr [<<Arg>>,<<Const60>>]
  /// CHECK-DAG:                       Return [<<UShr>>]

  /// CHECK-START: long Main.UShr60And15(long) instruction_simplifier (after)
  /// CHECK-NOT:                       And

  public static long UShr60And15(long arg) {
    return (arg >>> 60) & 15;
  }

  /// CHECK-START: int Main.UShr28And7(int) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const28:i\d+>>  IntConstant 28
  /// CHECK-DAG:     <<Const7:i\d+>>   IntConstant 7
  /// CHECK-DAG:     <<UShr:i\d+>>     UShr [<<Arg>>,<<Const28>>]
  /// CHECK-DAG:     <<And:i\d+>>      And [<<UShr>>,<<Const7>>]
  /// CHECK-DAG:                       Return [<<And>>]

  /// CHECK-START: int Main.UShr28And7(int) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const28:i\d+>>  IntConstant 28
  /// CHECK-DAG:     <<Const7:i\d+>>   IntConstant 7
  /// CHECK-DAG:     <<UShr:i\d+>>     UShr [<<Arg>>,<<Const28>>]
  /// CHECK-DAG:     <<And:i\d+>>      And [<<UShr>>,<<Const7>>]
  /// CHECK-DAG:                       Return [<<And>>]

  public static int UShr28And7(int arg) {
    return (arg >>> 28) & 7;
  }

  /// CHECK-START: long Main.UShr60And7(long) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const60:i\d+>>  IntConstant 60
  /// CHECK-DAG:     <<Const7:j\d+>>   LongConstant 7
  /// CHECK-DAG:     <<UShr:j\d+>>     UShr [<<Arg>>,<<Const60>>]
  /// CHECK-DAG:     <<And:j\d+>>      And [<<UShr>>,<<Const7>>]
  /// CHECK-DAG:                       Return [<<And>>]

  /// CHECK-START: long Main.UShr60And7(long) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const60:i\d+>>  IntConstant 60
  /// CHECK-DAG:     <<Const7:j\d+>>   LongConstant 7
  /// CHECK-DAG:     <<UShr:j\d+>>     UShr [<<Arg>>,<<Const60>>]
  /// CHECK-DAG:     <<And:j\d+>>      And [<<UShr>>,<<Const7>>]
  /// CHECK-DAG:                       Return [<<And>>]

  public static long UShr60And7(long arg) {
    return (arg >>> 60) & 7;
  }

  /// CHECK-START: int Main.Shr24And255(int) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const24:i\d+>>  IntConstant 24
  /// CHECK-DAG:     <<Const255:i\d+>> IntConstant 255
  /// CHECK-DAG:     <<Shr:i\d+>>      Shr [<<Arg>>,<<Const24>>]
  /// CHECK-DAG:     <<And:i\d+>>      And [<<Shr>>,<<Const255>>]
  /// CHECK-DAG:                       Return [<<And>>]

  /// CHECK-START: int Main.Shr24And255(int) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const24:i\d+>>  IntConstant 24
  /// CHECK-DAG:     <<UShr:i\d+>>     UShr [<<Arg>>,<<Const24>>]
  /// CHECK-DAG:                       Return [<<UShr>>]

  /// CHECK-START: int Main.Shr24And255(int) instruction_simplifier (after)
  /// CHECK-NOT:                       Shr
  /// CHECK-NOT:                       And

  public static int Shr24And255(int arg) {
    return (arg >> 24) & 255;
  }

  /// CHECK-START: long Main.Shr56And255(long) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const56:i\d+>>  IntConstant 56
  /// CHECK-DAG:     <<Const255:j\d+>> LongConstant 255
  /// CHECK-DAG:     <<Shr:j\d+>>      Shr [<<Arg>>,<<Const56>>]
  /// CHECK-DAG:     <<And:j\d+>>      And [<<Shr>>,<<Const255>>]
  /// CHECK-DAG:                       Return [<<And>>]

  /// CHECK-START: long Main.Shr56And255(long) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const56:i\d+>>  IntConstant 56
  /// CHECK-DAG:     <<UShr:j\d+>>     UShr [<<Arg>>,<<Const56>>]
  /// CHECK-DAG:                       Return [<<UShr>>]

  /// CHECK-START: long Main.Shr56And255(long) instruction_simplifier (after)
  /// CHECK-NOT:                       Shr
  /// CHECK-NOT:                       And

  public static long Shr56And255(long arg) {
    return (arg >> 56) & 255;
  }

  /// CHECK-START: int Main.Shr24And127(int) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const24:i\d+>>  IntConstant 24
  /// CHECK-DAG:     <<Const127:i\d+>> IntConstant 127
  /// CHECK-DAG:     <<Shr:i\d+>>      Shr [<<Arg>>,<<Const24>>]
  /// CHECK-DAG:     <<And:i\d+>>      And [<<Shr>>,<<Const127>>]
  /// CHECK-DAG:                       Return [<<And>>]

  /// CHECK-START: int Main.Shr24And127(int) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const24:i\d+>>  IntConstant 24
  /// CHECK-DAG:     <<Const127:i\d+>> IntConstant 127
  /// CHECK-DAG:     <<Shr:i\d+>>      Shr [<<Arg>>,<<Const24>>]
  /// CHECK-DAG:     <<And:i\d+>>      And [<<Shr>>,<<Const127>>]
  /// CHECK-DAG:                       Return [<<And>>]

  public static int Shr24And127(int arg) {
    return (arg >> 24) & 127;
  }

  /// CHECK-START: long Main.Shr56And127(long) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const56:i\d+>>  IntConstant 56
  /// CHECK-DAG:     <<Const127:j\d+>> LongConstant 127
  /// CHECK-DAG:     <<Shr:j\d+>>      Shr [<<Arg>>,<<Const56>>]
  /// CHECK-DAG:     <<And:j\d+>>      And [<<Shr>>,<<Const127>>]
  /// CHECK-DAG:                       Return [<<And>>]

  /// CHECK-START: long Main.Shr56And127(long) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const56:i\d+>>  IntConstant 56
  /// CHECK-DAG:     <<Const127:j\d+>> LongConstant 127
  /// CHECK-DAG:     <<Shr:j\d+>>      Shr [<<Arg>>,<<Const56>>]
  /// CHECK-DAG:     <<And:j\d+>>      And [<<Shr>>,<<Const127>>]
  /// CHECK-DAG:                       Return [<<And>>]

  public static long Shr56And127(long arg) {
    return (arg >> 56) & 127;
  }

  /// CHECK-START: long Main.Div1(long) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:j\d+>>     ParameterValue
  /// CHECK-DAG:     <<Const1:j\d+>>  LongConstant 1
  /// CHECK-DAG:     <<Div:j\d+>>     Div [<<Arg>>,<<Const1>>]
  /// CHECK-DAG:                      Return [<<Div>>]

  /// CHECK-START: long Main.Div1(long) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:j\d+>>     ParameterValue
  /// CHECK-DAG:                      Return [<<Arg>>]

  /// CHECK-START: long Main.Div1(long) instruction_simplifier (after)
  /// CHECK-NOT:                      Div

  public static long Div1(long arg) {
    return arg / 1;
  }

  /// CHECK-START: int Main.DivN1(int) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:     <<ConstN1:i\d+>>  IntConstant -1
  /// CHECK-DAG:     <<Div:i\d+>>      Div [<<Arg>>,<<ConstN1>>]
  /// CHECK-DAG:                       Return [<<Div>>]

  /// CHECK-START: int Main.DivN1(int) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:     <<Neg:i\d+>>      Neg [<<Arg>>]
  /// CHECK-DAG:                       Return [<<Neg>>]

  /// CHECK-START: int Main.DivN1(int) instruction_simplifier (after)
  /// CHECK-NOT:                       Div

  public static int DivN1(int arg) {
    return arg / -1;
  }

  /// CHECK-START: long Main.Mul1(long) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:j\d+>>     ParameterValue
  /// CHECK-DAG:     <<Const1:j\d+>>  LongConstant 1
  /// CHECK-DAG:     <<Mul:j\d+>>     Mul [<<Const1>>,<<Arg>>]
  /// CHECK-DAG:                      Return [<<Mul>>]

  /// CHECK-START: long Main.Mul1(long) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:j\d+>>     ParameterValue
  /// CHECK-DAG:                      Return [<<Arg>>]

  /// CHECK-START: long Main.Mul1(long) instruction_simplifier (after)
  /// CHECK-NOT:                       Mul

  public static long Mul1(long arg) {
    return arg * 1;
  }

  /// CHECK-START: int Main.MulN1(int) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:     <<ConstN1:i\d+>>  IntConstant -1
  /// CHECK-DAG:     <<Mul:i\d+>>      Mul [<<Arg>>,<<ConstN1>>]
  /// CHECK-DAG:                       Return [<<Mul>>]

  /// CHECK-START: int Main.MulN1(int) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:     <<Neg:i\d+>>      Neg [<<Arg>>]
  /// CHECK-DAG:                       Return [<<Neg>>]

  /// CHECK-START: int Main.MulN1(int) instruction_simplifier (after)
  /// CHECK-NOT:                       Mul

  public static int MulN1(int arg) {
    return arg * -1;
  }

  /// CHECK-START: long Main.MulPowerOfTwo128(long) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:j\d+>>       ParameterValue
  /// CHECK-DAG:     <<Const128:j\d+>>  LongConstant 128
  /// CHECK-DAG:     <<Mul:j\d+>>       Mul [<<Const128>>,<<Arg>>]
  /// CHECK-DAG:                        Return [<<Mul>>]

  /// CHECK-START: long Main.MulPowerOfTwo128(long) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:j\d+>>       ParameterValue
  /// CHECK-DAG:     <<Const7:i\d+>>    IntConstant 7
  /// CHECK-DAG:     <<Shl:j\d+>>       Shl [<<Arg>>,<<Const7>>]
  /// CHECK-DAG:                        Return [<<Shl>>]

  /// CHECK-START: long Main.MulPowerOfTwo128(long) instruction_simplifier (after)
  /// CHECK-NOT:                        Mul

  public static long MulPowerOfTwo128(long arg) {
    return arg * 128;
  }

  /// CHECK-START: int Main.Or0(int) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const0:i\d+>>   IntConstant 0
  /// CHECK-DAG:     <<Or:i\d+>>       Or [<<Arg>>,<<Const0>>]
  /// CHECK-DAG:                       Return [<<Or>>]

  /// CHECK-START: int Main.Or0(int) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:                       Return [<<Arg>>]

  /// CHECK-START: int Main.Or0(int) instruction_simplifier (after)
  /// CHECK-NOT:                       Or

  public static int Or0(int arg) {
    return arg | 0;
  }

  /// CHECK-START: long Main.OrSame(long) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:j\d+>>       ParameterValue
  /// CHECK-DAG:     <<Or:j\d+>>        Or [<<Arg>>,<<Arg>>]
  /// CHECK-DAG:                        Return [<<Or>>]

  /// CHECK-START: long Main.OrSame(long) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:j\d+>>       ParameterValue
  /// CHECK-DAG:                        Return [<<Arg>>]

  /// CHECK-START: long Main.OrSame(long) instruction_simplifier (after)
  /// CHECK-NOT:                        Or

  public static long OrSame(long arg) {
    return arg | arg;
  }

  /// CHECK-START: int Main.Shl0(int) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const0:i\d+>>   IntConstant 0
  /// CHECK-DAG:     <<Shl:i\d+>>      Shl [<<Arg>>,<<Const0>>]
  /// CHECK-DAG:                       Return [<<Shl>>]

  /// CHECK-START: int Main.Shl0(int) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:                       Return [<<Arg>>]

  /// CHECK-START: int Main.Shl0(int) instruction_simplifier (after)
  /// CHECK-NOT:                       Shl

  public static int Shl0(int arg) {
    return arg << 0;
  }

  /// CHECK-START: long Main.Shr0(long) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const0:i\d+>>   IntConstant 0
  /// CHECK-DAG:     <<Shr:j\d+>>      Shr [<<Arg>>,<<Const0>>]
  /// CHECK-DAG:                       Return [<<Shr>>]

  /// CHECK-START: long Main.Shr0(long) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:                       Return [<<Arg>>]

  /// CHECK-START: long Main.Shr0(long) instruction_simplifier (after)
  /// CHECK-NOT:                       Shr

  public static long Shr0(long arg) {
    return arg >> 0;
  }

  /// CHECK-START: long Main.Shr64(long) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const64:i\d+>>  IntConstant 64
  /// CHECK-DAG:     <<Shr:j\d+>>      Shr [<<Arg>>,<<Const64>>]
  /// CHECK-DAG:                       Return [<<Shr>>]

  /// CHECK-START: long Main.Shr64(long) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:                       Return [<<Arg>>]

  /// CHECK-START: long Main.Shr64(long) instruction_simplifier (after)
  /// CHECK-NOT:                       Shr

  public static long Shr64(long arg) {
    return arg >> 64;
  }

  /// CHECK-START: long Main.Sub0(long) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const0:j\d+>>   LongConstant 0
  /// CHECK-DAG:     <<Sub:j\d+>>      Sub [<<Arg>>,<<Const0>>]
  /// CHECK-DAG:                       Return [<<Sub>>]

  /// CHECK-START: long Main.Sub0(long) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:                       Return [<<Arg>>]

  /// CHECK-START: long Main.Sub0(long) instruction_simplifier (after)
  /// CHECK-NOT:                       Sub

  public static long Sub0(long arg) {
    return arg - 0;
  }

  /// CHECK-START: int Main.SubAliasNeg(int) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const0:i\d+>>   IntConstant 0
  /// CHECK-DAG:     <<Sub:i\d+>>      Sub [<<Const0>>,<<Arg>>]
  /// CHECK-DAG:                       Return [<<Sub>>]

  /// CHECK-START: int Main.SubAliasNeg(int) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:     <<Neg:i\d+>>      Neg [<<Arg>>]
  /// CHECK-DAG:                       Return [<<Neg>>]

  /// CHECK-START: int Main.SubAliasNeg(int) instruction_simplifier (after)
  /// CHECK-NOT:                       Sub

  public static int SubAliasNeg(int arg) {
    return 0 - arg;
  }

  /// CHECK-START: long Main.UShr0(long) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const0:i\d+>>   IntConstant 0
  /// CHECK-DAG:     <<UShr:j\d+>>     UShr [<<Arg>>,<<Const0>>]
  /// CHECK-DAG:                       Return [<<UShr>>]

  /// CHECK-START: long Main.UShr0(long) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:                       Return [<<Arg>>]

  /// CHECK-START: long Main.UShr0(long) instruction_simplifier (after)
  /// CHECK-NOT:                       UShr

  public static long UShr0(long arg) {
    return arg >>> 0;
  }

  /// CHECK-START: int Main.Xor0(int) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const0:i\d+>>   IntConstant 0
  /// CHECK-DAG:     <<Xor:i\d+>>      Xor [<<Arg>>,<<Const0>>]
  /// CHECK-DAG:                       Return [<<Xor>>]

  /// CHECK-START: int Main.Xor0(int) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:                       Return [<<Arg>>]

  /// CHECK-START: int Main.Xor0(int) instruction_simplifier (after)
  /// CHECK-NOT:                       Xor

  public static int Xor0(int arg) {
    return arg ^ 0;
  }

  /// CHECK-START: int Main.XorAllOnes(int) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:     <<ConstF:i\d+>>   IntConstant -1
  /// CHECK-DAG:     <<Xor:i\d+>>      Xor [<<Arg>>,<<ConstF>>]
  /// CHECK-DAG:                       Return [<<Xor>>]

  /// CHECK-START: int Main.XorAllOnes(int) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:     <<Not:i\d+>>      Not [<<Arg>>]
  /// CHECK-DAG:                       Return [<<Not>>]

  /// CHECK-START: int Main.XorAllOnes(int) instruction_simplifier (after)
  /// CHECK-NOT:                       Xor

  public static int XorAllOnes(int arg) {
    return arg ^ -1;
  }

  /**
   * Test that addition or subtraction operation with both inputs negated are
   * optimized to use a single negation after the operation.
   * The transformation tested is implemented in
   * `InstructionSimplifierVisitor::TryMoveNegOnInputsAfterBinop`.
   */

  /// CHECK-START: int Main.AddNegs1(int, int) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg1:i\d+>>     ParameterValue
  /// CHECK-DAG:     <<Arg2:i\d+>>     ParameterValue
  /// CHECK-DAG:     <<Neg1:i\d+>>     Neg [<<Arg1>>]
  /// CHECK-DAG:     <<Neg2:i\d+>>     Neg [<<Arg2>>]
  /// CHECK-DAG:     <<Add:i\d+>>      Add [<<Neg1>>,<<Neg2>>]
  /// CHECK-DAG:                       Return [<<Add>>]

  /// CHECK-START: int Main.AddNegs1(int, int) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg1:i\d+>>     ParameterValue
  /// CHECK-DAG:     <<Arg2:i\d+>>     ParameterValue
  /// CHECK-NOT:                       Neg
  /// CHECK-DAG:     <<Add:i\d+>>      Add [<<Arg1>>,<<Arg2>>]
  /// CHECK-DAG:     <<Neg:i\d+>>      Neg [<<Add>>]
  /// CHECK-DAG:                       Return [<<Neg>>]

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

  /// CHECK-START: int Main.AddNegs2(int, int) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg1:i\d+>>     ParameterValue
  /// CHECK-DAG:     <<Arg2:i\d+>>     ParameterValue
  /// CHECK-DAG:     <<Neg1:i\d+>>     Neg [<<Arg1>>]
  /// CHECK-DAG:     <<Neg2:i\d+>>     Neg [<<Arg2>>]
  /// CHECK-DAG:     <<Add1:i\d+>>     Add [<<Neg1>>,<<Neg2>>]
  /// CHECK-DAG:     <<Add2:i\d+>>     Add [<<Neg1>>,<<Neg2>>]
  /// CHECK-DAG:     <<Or:i\d+>>       Or [<<Add1>>,<<Add2>>]
  /// CHECK-DAG:                       Return [<<Or>>]

  /// CHECK-START: int Main.AddNegs2(int, int) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg1:i\d+>>     ParameterValue
  /// CHECK-DAG:     <<Arg2:i\d+>>     ParameterValue
  /// CHECK-DAG:     <<Neg1:i\d+>>     Neg [<<Arg1>>]
  /// CHECK-DAG:     <<Neg2:i\d+>>     Neg [<<Arg2>>]
  /// CHECK-DAG:     <<Add1:i\d+>>     Add [<<Neg1>>,<<Neg2>>]
  /// CHECK-DAG:     <<Add2:i\d+>>     Add [<<Neg1>>,<<Neg2>>]
  /// CHECK-NOT:                       Neg
  /// CHECK-DAG:     <<Or:i\d+>>       Or [<<Add1>>,<<Add2>>]
  /// CHECK-DAG:                       Return [<<Or>>]

  /// CHECK-START: int Main.AddNegs2(int, int) GVN (after)
  /// CHECK-DAG:     <<Arg1:i\d+>>     ParameterValue
  /// CHECK-DAG:     <<Arg2:i\d+>>     ParameterValue
  /// CHECK-DAG:     <<Neg1:i\d+>>     Neg [<<Arg1>>]
  /// CHECK-DAG:     <<Neg2:i\d+>>     Neg [<<Arg2>>]
  /// CHECK-DAG:     <<Add:i\d+>>      Add [<<Neg1>>,<<Neg2>>]
  /// CHECK-DAG:     <<Or:i\d+>>       Or [<<Add>>,<<Add>>]
  /// CHECK-DAG:                       Return [<<Or>>]

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

  /// CHECK-START: long Main.AddNegs3(long, long) instruction_simplifier (before)
  //  -------------- Arguments and initial negation operations.
  /// CHECK-DAG:     <<Arg1:j\d+>>     ParameterValue
  /// CHECK-DAG:     <<Arg2:j\d+>>     ParameterValue
  /// CHECK-DAG:     <<Neg1:j\d+>>     Neg [<<Arg1>>]
  /// CHECK-DAG:     <<Neg2:j\d+>>     Neg [<<Arg2>>]
  /// CHECK:                           Goto
  //  -------------- Loop
  /// CHECK:                           SuspendCheck
  /// CHECK:         <<Add:j\d+>>      Add [<<Neg1>>,<<Neg2>>]
  /// CHECK:                           Goto

  /// CHECK-START: long Main.AddNegs3(long, long) instruction_simplifier (after)
  //  -------------- Arguments and initial negation operations.
  /// CHECK-DAG:     <<Arg1:j\d+>>     ParameterValue
  /// CHECK-DAG:     <<Arg2:j\d+>>     ParameterValue
  /// CHECK-DAG:     <<Neg1:j\d+>>     Neg [<<Arg1>>]
  /// CHECK-DAG:     <<Neg2:j\d+>>     Neg [<<Arg2>>]
  /// CHECK:                           Goto
  //  -------------- Loop
  /// CHECK:                           SuspendCheck
  /// CHECK:         <<Add:j\d+>>      Add [<<Neg1>>,<<Neg2>>]
  /// CHECK-NOT:                       Neg
  /// CHECK:                           Goto

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

  /// CHECK-START: long Main.AddNeg1(long, long) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg1:j\d+>>     ParameterValue
  /// CHECK-DAG:     <<Arg2:j\d+>>     ParameterValue
  /// CHECK-DAG:     <<Neg:j\d+>>      Neg [<<Arg1>>]
  /// CHECK-DAG:     <<Add:j\d+>>      Add [<<Neg>>,<<Arg2>>]
  /// CHECK-DAG:                       Return [<<Add>>]

  /// CHECK-START: long Main.AddNeg1(long, long) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg1:j\d+>>     ParameterValue
  /// CHECK-DAG:     <<Arg2:j\d+>>     ParameterValue
  /// CHECK-DAG:     <<Sub:j\d+>>      Sub [<<Arg2>>,<<Arg1>>]
  /// CHECK-DAG:                       Return [<<Sub>>]

  /// CHECK-START: long Main.AddNeg1(long, long) instruction_simplifier (after)
  /// CHECK-NOT:                       Neg
  /// CHECK-NOT:                       Add

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

  /// CHECK-START: long Main.AddNeg2(long, long) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg1:j\d+>>     ParameterValue
  /// CHECK-DAG:     <<Arg2:j\d+>>     ParameterValue
  /// CHECK-DAG:     <<Neg:j\d+>>      Neg [<<Arg2>>]
  /// CHECK-DAG:     <<Add1:j\d+>>     Add [<<Arg1>>,<<Neg>>]
  /// CHECK-DAG:     <<Add2:j\d+>>     Add [<<Arg1>>,<<Neg>>]
  /// CHECK-DAG:     <<Res:j\d+>>      Or [<<Add1>>,<<Add2>>]
  /// CHECK-DAG:                       Return [<<Res>>]

  /// CHECK-START: long Main.AddNeg2(long, long) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg1:j\d+>>     ParameterValue
  /// CHECK-DAG:     <<Arg2:j\d+>>     ParameterValue
  /// CHECK-DAG:     <<Neg:j\d+>>      Neg [<<Arg2>>]
  /// CHECK-DAG:     <<Add1:j\d+>>     Add [<<Arg1>>,<<Neg>>]
  /// CHECK-DAG:     <<Add2:j\d+>>     Add [<<Arg1>>,<<Neg>>]
  /// CHECK-DAG:     <<Res:j\d+>>      Or [<<Add1>>,<<Add2>>]
  /// CHECK-DAG:                       Return [<<Res>>]

  /// CHECK-START: long Main.AddNeg2(long, long) instruction_simplifier (after)
  /// CHECK-NOT:                       Sub

  public static long AddNeg2(long arg1, long arg2) {
    long temp = -arg2;
    return (arg1 + temp) | (arg1 + temp);
  }

  /**
   * Test simplification of the `-(-var)` pattern.
   * The transformation tested is implemented in `InstructionSimplifierVisitor::VisitNeg`.
   */

  /// CHECK-START: long Main.NegNeg1(long) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:     <<Neg1:j\d+>>     Neg [<<Arg>>]
  /// CHECK-DAG:     <<Neg2:j\d+>>     Neg [<<Neg1>>]
  /// CHECK-DAG:                       Return [<<Neg2>>]

  /// CHECK-START: long Main.NegNeg1(long) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:                       Return [<<Arg>>]

  /// CHECK-START: long Main.NegNeg1(long) instruction_simplifier (after)
  /// CHECK-NOT:                       Neg

  public static long NegNeg1(long arg) {
    return -(-arg);
  }

  /**
   * Test 'multi-step' simplification, where a first transformation yields a
   * new simplification possibility for the current instruction.
   * The transformations tested are implemented in `InstructionSimplifierVisitor::VisitNeg`
   * and in `InstructionSimplifierVisitor::VisitAdd`.
   */

  /// CHECK-START: int Main.NegNeg2(int) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:     <<Neg1:i\d+>>     Neg [<<Arg>>]
  /// CHECK-DAG:     <<Neg2:i\d+>>     Neg [<<Neg1>>]
  /// CHECK-DAG:     <<Add:i\d+>>      Add [<<Neg2>>,<<Neg1>>]
  /// CHECK-DAG:                       Return [<<Add>>]

  /// CHECK-START: int Main.NegNeg2(int) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:     <<Sub:i\d+>>      Sub [<<Arg>>,<<Arg>>]
  /// CHECK-DAG:                       Return [<<Sub>>]

  /// CHECK-START: int Main.NegNeg2(int) instruction_simplifier (after)
  /// CHECK-NOT:                       Neg
  /// CHECK-NOT:                       Add

  /// CHECK-START: int Main.NegNeg2(int) constant_folding_after_inlining (after)
  /// CHECK:         <<Const0:i\d+>>   IntConstant 0
  /// CHECK-NOT:                       Neg
  /// CHECK-NOT:                       Add
  /// CHECK:                           Return [<<Const0>>]

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

  /// CHECK-START: long Main.NegNeg3(long) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const0:j\d+>>   LongConstant 0
  /// CHECK-DAG:     <<Neg:j\d+>>      Neg [<<Arg>>]
  /// CHECK-DAG:     <<Sub:j\d+>>      Sub [<<Const0>>,<<Neg>>]
  /// CHECK-DAG:                       Return [<<Sub>>]

  /// CHECK-START: long Main.NegNeg3(long) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:                       Return [<<Arg>>]

  /// CHECK-START: long Main.NegNeg3(long) instruction_simplifier (after)
  /// CHECK-NOT:                       Neg
  /// CHECK-NOT:                       Sub

  public static long NegNeg3(long arg) {
    return 0 - -arg;
  }

  /**
   * Test that a negated subtraction is simplified to a subtraction with its
   * arguments reversed.
   * The transformation tested is implemented in `InstructionSimplifierVisitor::VisitNeg`.
   */

  /// CHECK-START: int Main.NegSub1(int, int) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg1:i\d+>>     ParameterValue
  /// CHECK-DAG:     <<Arg2:i\d+>>     ParameterValue
  /// CHECK-DAG:     <<Sub:i\d+>>      Sub [<<Arg1>>,<<Arg2>>]
  /// CHECK-DAG:     <<Neg:i\d+>>      Neg [<<Sub>>]
  /// CHECK-DAG:                       Return [<<Neg>>]

  /// CHECK-START: int Main.NegSub1(int, int) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg1:i\d+>>     ParameterValue
  /// CHECK-DAG:     <<Arg2:i\d+>>     ParameterValue
  /// CHECK-DAG:     <<Sub:i\d+>>      Sub [<<Arg2>>,<<Arg1>>]
  /// CHECK-DAG:                       Return [<<Sub>>]

  /// CHECK-START: int Main.NegSub1(int, int) instruction_simplifier (after)
  /// CHECK-NOT:                       Neg

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

  /// CHECK-START: int Main.NegSub2(int, int) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg1:i\d+>>     ParameterValue
  /// CHECK-DAG:     <<Arg2:i\d+>>     ParameterValue
  /// CHECK-DAG:     <<Sub:i\d+>>      Sub [<<Arg1>>,<<Arg2>>]
  /// CHECK-DAG:     <<Neg1:i\d+>>     Neg [<<Sub>>]
  /// CHECK-DAG:     <<Neg2:i\d+>>     Neg [<<Sub>>]
  /// CHECK-DAG:     <<Or:i\d+>>       Or [<<Neg1>>,<<Neg2>>]
  /// CHECK-DAG:                       Return [<<Or>>]

  /// CHECK-START: int Main.NegSub2(int, int) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg1:i\d+>>     ParameterValue
  /// CHECK-DAG:     <<Arg2:i\d+>>     ParameterValue
  /// CHECK-DAG:     <<Sub:i\d+>>      Sub [<<Arg1>>,<<Arg2>>]
  /// CHECK-DAG:     <<Neg1:i\d+>>     Neg [<<Sub>>]
  /// CHECK-DAG:     <<Neg2:i\d+>>     Neg [<<Sub>>]
  /// CHECK-DAG:     <<Or:i\d+>>       Or [<<Neg1>>,<<Neg2>>]
  /// CHECK-DAG:                       Return [<<Or>>]

  public static int NegSub2(int arg1, int arg2) {
    int temp = arg1 - arg2;
    return -temp | -temp;
  }

  /**
   * Test simplification of the `~~var` pattern.
   * The transformation tested is implemented in `InstructionSimplifierVisitor::VisitNot`.
   */

  /// CHECK-START: long Main.NotNot1(long) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:     <<Not1:j\d+>>     Not [<<Arg>>]
  /// CHECK-DAG:     <<Not2:j\d+>>     Not [<<Not1>>]
  /// CHECK-DAG:                       Return [<<Not2>>]

  /// CHECK-START: long Main.NotNot1(long) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:                       Return [<<Arg>>]

  /// CHECK-START: long Main.NotNot1(long) instruction_simplifier (after)
  /// CHECK-NOT:                       Not

  public static long NotNot1(long arg) {
    return ~~arg;
  }

  /// CHECK-START: int Main.NotNot2(int) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:     <<Not1:i\d+>>     Not [<<Arg>>]
  /// CHECK-DAG:     <<Not2:i\d+>>     Not [<<Not1>>]
  /// CHECK-DAG:     <<Add:i\d+>>      Add [<<Not2>>,<<Not1>>]
  /// CHECK-DAG:                       Return [<<Add>>]

  /// CHECK-START: int Main.NotNot2(int) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:     <<Not:i\d+>>      Not [<<Arg>>]
  /// CHECK-DAG:     <<Add:i\d+>>      Add [<<Arg>>,<<Not>>]
  /// CHECK-DAG:                       Return [<<Add>>]

  /// CHECK-START: int Main.NotNot2(int) instruction_simplifier (after)
  /// CHECK:                           Not
  /// CHECK-NOT:                       Not

  public static int NotNot2(int arg) {
    int temp = ~arg;
    return temp + ~temp;
  }

  /**
   * Test the simplification of a subtraction with a negated argument.
   * The transformation tested is implemented in `InstructionSimplifierVisitor::VisitSub`.
   */

  /// CHECK-START: int Main.SubNeg1(int, int) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg1:i\d+>>     ParameterValue
  /// CHECK-DAG:     <<Arg2:i\d+>>     ParameterValue
  /// CHECK-DAG:     <<Neg:i\d+>>      Neg [<<Arg1>>]
  /// CHECK-DAG:     <<Sub:i\d+>>      Sub [<<Neg>>,<<Arg2>>]
  /// CHECK-DAG:                       Return [<<Sub>>]

  /// CHECK-START: int Main.SubNeg1(int, int) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg1:i\d+>>     ParameterValue
  /// CHECK-DAG:     <<Arg2:i\d+>>     ParameterValue
  /// CHECK-DAG:     <<Add:i\d+>>      Add [<<Arg1>>,<<Arg2>>]
  /// CHECK-DAG:     <<Neg:i\d+>>      Neg [<<Add>>]
  /// CHECK-DAG:                       Return [<<Neg>>]

  /// CHECK-START: int Main.SubNeg1(int, int) instruction_simplifier (after)
  /// CHECK-NOT:                       Sub

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

  /// CHECK-START: int Main.SubNeg2(int, int) instruction_simplifier (before)
  /// CHECK-DAG:     <<Arg1:i\d+>>     ParameterValue
  /// CHECK-DAG:     <<Arg2:i\d+>>     ParameterValue
  /// CHECK-DAG:     <<Neg:i\d+>>      Neg [<<Arg1>>]
  /// CHECK-DAG:     <<Sub1:i\d+>>     Sub [<<Neg>>,<<Arg2>>]
  /// CHECK-DAG:     <<Sub2:i\d+>>     Sub [<<Neg>>,<<Arg2>>]
  /// CHECK-DAG:     <<Or:i\d+>>       Or [<<Sub1>>,<<Sub2>>]
  /// CHECK-DAG:                       Return [<<Or>>]

  /// CHECK-START: int Main.SubNeg2(int, int) instruction_simplifier (after)
  /// CHECK-DAG:     <<Arg1:i\d+>>     ParameterValue
  /// CHECK-DAG:     <<Arg2:i\d+>>     ParameterValue
  /// CHECK-DAG:     <<Neg:i\d+>>      Neg [<<Arg1>>]
  /// CHECK-DAG:     <<Sub1:i\d+>>     Sub [<<Neg>>,<<Arg2>>]
  /// CHECK-DAG:     <<Sub2:i\d+>>     Sub [<<Neg>>,<<Arg2>>]
  /// CHECK-DAG:     <<Or:i\d+>>       Or [<<Sub1>>,<<Sub2>>]
  /// CHECK-DAG:                       Return [<<Or>>]

  /// CHECK-START: int Main.SubNeg2(int, int) instruction_simplifier (after)
  /// CHECK-NOT:                       Add

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

  /// CHECK-START: long Main.SubNeg3(long, long) instruction_simplifier (before)
  //  -------------- Arguments and initial negation operation.
  /// CHECK-DAG:     <<Arg1:j\d+>>     ParameterValue
  /// CHECK-DAG:     <<Arg2:j\d+>>     ParameterValue
  /// CHECK-DAG:     <<Neg:j\d+>>      Neg [<<Arg1>>]
  /// CHECK:                           Goto
  //  -------------- Loop
  /// CHECK:                           SuspendCheck
  /// CHECK:         <<Sub:j\d+>>      Sub [<<Neg>>,<<Arg2>>]
  /// CHECK:                           Goto

  /// CHECK-START: long Main.SubNeg3(long, long) instruction_simplifier (after)
  //  -------------- Arguments and initial negation operation.
  /// CHECK-DAG:     <<Arg1:j\d+>>     ParameterValue
  /// CHECK-DAG:     <<Arg2:j\d+>>     ParameterValue
  /// CHECK-DAG:     <<Neg:j\d+>>      Neg [<<Arg1>>]
  /// CHECK-DAG:                       Goto
  //  -------------- Loop
  /// CHECK:                           SuspendCheck
  /// CHECK:         <<Sub:j\d+>>      Sub [<<Neg>>,<<Arg2>>]
  /// CHECK-NOT:                       Neg
  /// CHECK:                           Goto

  public static long SubNeg3(long arg1, long arg2) {
    long res = 0;
    long temp = -arg1;
    for (long i = 0; i < 1; i++) {
      res += temp - arg2 - i;
    }
    return res;
  }

  /// CHECK-START: boolean Main.EqualBoolVsIntConst(boolean) instruction_simplifier_after_bce (before)
  /// CHECK-DAG:     <<Arg:z\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const0:i\d+>>   IntConstant 0
  /// CHECK-DAG:     <<Const1:i\d+>>   IntConstant 1
  /// CHECK-DAG:     <<Const2:i\d+>>   IntConstant 2
  /// CHECK-DAG:     <<NotArg:i\d+>>   Select [<<Const1>>,<<Const0>>,<<Arg>>]
  /// CHECK-DAG:     <<Cond:z\d+>>     Equal [<<NotArg>>,<<Const2>>]
  /// CHECK-DAG:     <<NotCond:i\d+>>  Select [<<Const1>>,<<Const0>>,<<Cond>>]
  /// CHECK-DAG:                       Return [<<NotCond>>]

  /// CHECK-START: boolean Main.EqualBoolVsIntConst(boolean) instruction_simplifier_after_bce (after)
  /// CHECK-DAG:     <<True:i\d+>>     IntConstant 1
  /// CHECK-DAG:                       Return [<<True>>]

  public static boolean EqualBoolVsIntConst(boolean arg) {
    return (arg ? 0 : 1) != 2;
  }

  /// CHECK-START: boolean Main.NotEqualBoolVsIntConst(boolean) instruction_simplifier_after_bce (before)
  /// CHECK-DAG:     <<Arg:z\d+>>      ParameterValue
  /// CHECK-DAG:     <<Const0:i\d+>>   IntConstant 0
  /// CHECK-DAG:     <<Const1:i\d+>>   IntConstant 1
  /// CHECK-DAG:     <<Const2:i\d+>>   IntConstant 2
  /// CHECK-DAG:     <<NotArg:i\d+>>   Select [<<Const1>>,<<Const0>>,<<Arg>>]
  /// CHECK-DAG:     <<Cond:z\d+>>     NotEqual [<<NotArg>>,<<Const2>>]
  /// CHECK-DAG:     <<NotCond:i\d+>>  Select [<<Const1>>,<<Const0>>,<<Cond>>]
  /// CHECK-DAG:                       Return [<<NotCond>>]

  /// CHECK-START: boolean Main.NotEqualBoolVsIntConst(boolean) instruction_simplifier_after_bce (after)
  /// CHECK-DAG:     <<False:i\d+>>    IntConstant 0
  /// CHECK-DAG:                       Return [<<False>>]

  public static boolean NotEqualBoolVsIntConst(boolean arg) {
    return (arg ? 0 : 1) == 2;
  }

  /*
   * Test simplification of double Boolean negation. Note that sometimes
   * both negations can be removed but we only expect the simplifier to
   * remove the second.
   */

  /// CHECK-START: boolean Main.NotNotBool(boolean) instruction_simplifier_after_bce (before)
  /// CHECK-DAG:     <<Arg:z\d+>>       ParameterValue
  /// CHECK-DAG:     <<Const0:i\d+>>    IntConstant 0
  /// CHECK-DAG:     <<Const1:i\d+>>    IntConstant 1
  /// CHECK-DAG:     <<NotArg:i\d+>>    Select [<<Const1>>,<<Const0>>,<<Arg>>]
  /// CHECK-DAG:     <<NotNotArg:i\d+>> Select [<<Const1>>,<<Const0>>,<<NotArg>>]
  /// CHECK-DAG:                        Return [<<NotNotArg>>]

  /// CHECK-START: boolean Main.NotNotBool(boolean) instruction_simplifier_after_bce (after)
  /// CHECK-DAG:     <<Arg:z\d+>>       ParameterValue
  /// CHECK-DAG:                        Return [<<Arg>>]

  public static boolean NegateValue(boolean arg) {
    return !arg;
  }

  public static boolean NotNotBool(boolean arg) {
    return !(NegateValue(arg));
  }

  /// CHECK-START: float Main.Div2(float) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:f\d+>>      ParameterValue
  /// CHECK-DAG:      <<Const2:f\d+>>   FloatConstant 2
  /// CHECK-DAG:      <<Div:f\d+>>      Div [<<Arg>>,<<Const2>>]
  /// CHECK-DAG:                        Return [<<Div>>]

  /// CHECK-START: float Main.Div2(float) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:f\d+>>      ParameterValue
  /// CHECK-DAG:      <<ConstP5:f\d+>>  FloatConstant 0.5
  /// CHECK-DAG:      <<Mul:f\d+>>      Mul [<<Arg>>,<<ConstP5>>]
  /// CHECK-DAG:                        Return [<<Mul>>]

  /// CHECK-START: float Main.Div2(float) instruction_simplifier (after)
  /// CHECK-NOT:                        Div

  public static float Div2(float arg) {
    return arg / 2.0f;
  }

  /// CHECK-START: double Main.Div2(double) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:d\d+>>      ParameterValue
  /// CHECK-DAG:      <<Const2:d\d+>>   DoubleConstant 2
  /// CHECK-DAG:      <<Div:d\d+>>      Div [<<Arg>>,<<Const2>>]
  /// CHECK-DAG:                        Return [<<Div>>]

  /// CHECK-START: double Main.Div2(double) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:d\d+>>      ParameterValue
  /// CHECK-DAG:      <<ConstP5:d\d+>>  DoubleConstant 0.5
  /// CHECK-DAG:      <<Mul:d\d+>>      Mul [<<Arg>>,<<ConstP5>>]
  /// CHECK-DAG:                        Return [<<Mul>>]

  /// CHECK-START: double Main.Div2(double) instruction_simplifier (after)
  /// CHECK-NOT:                        Div
  public static double Div2(double arg) {
    return arg / 2.0;
  }

  /// CHECK-START: float Main.DivMP25(float) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:f\d+>>      ParameterValue
  /// CHECK-DAG:      <<ConstMP25:f\d+>>   FloatConstant -0.25
  /// CHECK-DAG:      <<Div:f\d+>>      Div [<<Arg>>,<<ConstMP25>>]
  /// CHECK-DAG:                        Return [<<Div>>]

  /// CHECK-START: float Main.DivMP25(float) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:f\d+>>      ParameterValue
  /// CHECK-DAG:      <<ConstM4:f\d+>>  FloatConstant -4
  /// CHECK-DAG:      <<Mul:f\d+>>      Mul [<<Arg>>,<<ConstM4>>]
  /// CHECK-DAG:                        Return [<<Mul>>]

  /// CHECK-START: float Main.DivMP25(float) instruction_simplifier (after)
  /// CHECK-NOT:                        Div

  public static float DivMP25(float arg) {
    return arg / -0.25f;
  }

  /// CHECK-START: double Main.DivMP25(double) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:d\d+>>      ParameterValue
  /// CHECK-DAG:      <<ConstMP25:d\d+>>   DoubleConstant -0.25
  /// CHECK-DAG:      <<Div:d\d+>>      Div [<<Arg>>,<<ConstMP25>>]
  /// CHECK-DAG:                        Return [<<Div>>]

  /// CHECK-START: double Main.DivMP25(double) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:d\d+>>      ParameterValue
  /// CHECK-DAG:      <<ConstM4:d\d+>>  DoubleConstant -4
  /// CHECK-DAG:      <<Mul:d\d+>>      Mul [<<Arg>>,<<ConstM4>>]
  /// CHECK-DAG:                        Return [<<Mul>>]

  /// CHECK-START: double Main.DivMP25(double) instruction_simplifier (after)
  /// CHECK-NOT:                        Div
  public static double DivMP25(double arg) {
    return arg / -0.25f;
  }

  /**
   * Test strength reduction of factors of the form (2^n + 1).
   */

  /// CHECK-START: int Main.mulPow2Plus1(int) instruction_simplifier (before)
  /// CHECK-DAG:   <<Arg:i\d+>>         ParameterValue
  /// CHECK-DAG:   <<Const9:i\d+>>      IntConstant 9
  /// CHECK:                            Mul [<<Arg>>,<<Const9>>]

  /// CHECK-START: int Main.mulPow2Plus1(int) instruction_simplifier (after)
  /// CHECK-DAG:   <<Arg:i\d+>>         ParameterValue
  /// CHECK-DAG:   <<Const3:i\d+>>      IntConstant 3
  /// CHECK:       <<Shift:i\d+>>       Shl [<<Arg>>,<<Const3>>]
  /// CHECK-NEXT:                       Add [<<Arg>>,<<Shift>>]

  public static int mulPow2Plus1(int arg) {
    return arg * 9;
  }

  /**
   * Test strength reduction of factors of the form (2^n - 1).
   */

  /// CHECK-START: long Main.mulPow2Minus1(long) instruction_simplifier (before)
  /// CHECK-DAG:   <<Arg:j\d+>>         ParameterValue
  /// CHECK-DAG:   <<Const31:j\d+>>     LongConstant 31
  /// CHECK:                            Mul [<<Const31>>,<<Arg>>]

  /// CHECK-START: long Main.mulPow2Minus1(long) instruction_simplifier (after)
  /// CHECK-DAG:   <<Arg:j\d+>>         ParameterValue
  /// CHECK-DAG:   <<Const5:i\d+>>      IntConstant 5
  /// CHECK:       <<Shift:j\d+>>       Shl [<<Arg>>,<<Const5>>]
  /// CHECK-NEXT:                       Sub [<<Shift>>,<<Arg>>]

  public static long mulPow2Minus1(long arg) {
    return arg * 31;
  }

  /// CHECK-START: int Main.booleanFieldNotEqualOne() instruction_simplifier_after_bce (before)
  /// CHECK-DAG:      <<Const1:i\d+>>   IntConstant 1
  /// CHECK-DAG:      <<Const13:i\d+>>  IntConstant 13
  /// CHECK-DAG:      <<Const54:i\d+>>  IntConstant 54
  /// CHECK-DAG:      <<Field:z\d+>>    StaticFieldGet
  /// CHECK-DAG:      <<NE:z\d+>>       NotEqual [<<Field>>,<<Const1>>]
  /// CHECK-DAG:      <<Select:i\d+>>   Select [<<Const13>>,<<Const54>>,<<NE>>]
  /// CHECK-DAG:                        Return [<<Select>>]

  /// CHECK-START: int Main.booleanFieldNotEqualOne() instruction_simplifier_after_bce (after)
  /// CHECK-DAG:      <<Field:z\d+>>    StaticFieldGet
  /// CHECK-DAG:      <<Const13:i\d+>>  IntConstant 13
  /// CHECK-DAG:      <<Const54:i\d+>>  IntConstant 54
  /// CHECK-DAG:      <<Select:i\d+>>   Select [<<Const54>>,<<Const13>>,<<Field>>]
  /// CHECK-DAG:                        Return [<<Select>>]

  public static int booleanFieldNotEqualOne() {
    return (booleanField == $inline$true()) ? 13 : 54;
  }

  /// CHECK-START: int Main.booleanFieldEqualZero() instruction_simplifier_after_bce (before)
  /// CHECK-DAG:      <<Const0:i\d+>>   IntConstant 0
  /// CHECK-DAG:      <<Const13:i\d+>>  IntConstant 13
  /// CHECK-DAG:      <<Const54:i\d+>>  IntConstant 54
  /// CHECK-DAG:      <<Field:z\d+>>    StaticFieldGet
  /// CHECK-DAG:      <<NE:z\d+>>       Equal [<<Field>>,<<Const0>>]
  /// CHECK-DAG:      <<Select:i\d+>>   Select [<<Const13>>,<<Const54>>,<<NE>>]
  /// CHECK-DAG:                        Return [<<Select>>]

  /// CHECK-START: int Main.booleanFieldEqualZero() instruction_simplifier_after_bce (after)
  /// CHECK-DAG:      <<Field:z\d+>>    StaticFieldGet
  /// CHECK-DAG:      <<Const13:i\d+>>  IntConstant 13
  /// CHECK-DAG:      <<Const54:i\d+>>  IntConstant 54
  /// CHECK-DAG:      <<Select:i\d+>>   Select [<<Const54>>,<<Const13>>,<<Field>>]
  /// CHECK-DAG:                        Return [<<Select>>]

  public static int booleanFieldEqualZero() {
    return (booleanField != $inline$false()) ? 13 : 54;
  }

  /// CHECK-START: int Main.intConditionNotEqualOne(int) instruction_simplifier_after_bce (before)
  /// CHECK-DAG:      <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:      <<Const0:i\d+>>   IntConstant 0
  /// CHECK-DAG:      <<Const1:i\d+>>   IntConstant 1
  /// CHECK-DAG:      <<Const13:i\d+>>  IntConstant 13
  /// CHECK-DAG:      <<Const42:i\d+>>  IntConstant 42
  /// CHECK-DAG:      <<Const54:i\d+>>  IntConstant 54
  /// CHECK-DAG:      <<LE:z\d+>>       LessThanOrEqual [<<Arg>>,<<Const42>>]
  /// CHECK-DAG:      <<GT:i\d+>>       Select [<<Const1>>,<<Const0>>,<<LE>>]
  /// CHECK-DAG:      <<NE:z\d+>>       NotEqual [<<GT>>,<<Const1>>]
  /// CHECK-DAG:      <<Result:i\d+>>   Select [<<Const13>>,<<Const54>>,<<NE>>]
  /// CHECK-DAG:                        Return [<<Result>>]

  /// CHECK-START: int Main.intConditionNotEqualOne(int) instruction_simplifier_after_bce (after)
  /// CHECK-DAG:      <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:      <<Const13:i\d+>>  IntConstant 13
  /// CHECK-DAG:      <<Const42:i\d+>>  IntConstant 42
  /// CHECK-DAG:      <<Const54:i\d+>>  IntConstant 54
  /// CHECK-DAG:      <<Result:i\d+>>   Select [<<Const13>>,<<Const54>>,<<LE:z\d+>>]
  /// CHECK-DAG:      <<LE>>            LessThanOrEqual [<<Arg>>,<<Const42>>]
  /// CHECK-DAG:                        Return [<<Result>>]
  // Note that we match `LE` from Select because there are two identical
  // LessThanOrEqual instructions.

  public static int intConditionNotEqualOne(int i) {
    return ((i > 42) == $inline$true()) ? 13 : 54;
  }

  /// CHECK-START: int Main.intConditionEqualZero(int) instruction_simplifier_after_bce (before)
  /// CHECK-DAG:      <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:      <<Const0:i\d+>>   IntConstant 0
  /// CHECK-DAG:      <<Const1:i\d+>>   IntConstant 1
  /// CHECK-DAG:      <<Const13:i\d+>>  IntConstant 13
  /// CHECK-DAG:      <<Const42:i\d+>>  IntConstant 42
  /// CHECK-DAG:      <<Const54:i\d+>>  IntConstant 54
  /// CHECK-DAG:      <<LE:z\d+>>       LessThanOrEqual [<<Arg>>,<<Const42>>]
  /// CHECK-DAG:      <<GT:i\d+>>       Select [<<Const1>>,<<Const0>>,<<LE>>]
  /// CHECK-DAG:      <<NE:z\d+>>       Equal [<<GT>>,<<Const0>>]
  /// CHECK-DAG:      <<Result:i\d+>>   Select [<<Const13>>,<<Const54>>,<<NE>>]
  /// CHECK-DAG:                        Return [<<Result>>]

  /// CHECK-START: int Main.intConditionEqualZero(int) instruction_simplifier_after_bce (after)
  /// CHECK-DAG:      <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:      <<Const13:i\d+>>  IntConstant 13
  /// CHECK-DAG:      <<Const42:i\d+>>  IntConstant 42
  /// CHECK-DAG:      <<Const54:i\d+>>  IntConstant 54
  /// CHECK-DAG:      <<Result:i\d+>>   Select [<<Const13>>,<<Const54>>,<<LE:z\d+>>]
  /// CHECK-DAG:      <<LE>>            LessThanOrEqual [<<Arg>>,<<Const42>>]
  /// CHECK-DAG:                        Return [<<Result>>]
  // Note that we match `LE` from Select because there are two identical
  // LessThanOrEqual instructions.

  public static int intConditionEqualZero(int i) {
    return ((i > 42) != $inline$false()) ? 13 : 54;
  }

  // Test that conditions on float/double are not flipped.

  /// CHECK-START: int Main.floatConditionNotEqualOne(float) builder (after)
  /// CHECK:                            LessThanOrEqual

  /// CHECK-START: int Main.floatConditionNotEqualOne(float) instruction_simplifier_before_codegen (after)
  /// CHECK-DAG:      <<Arg:f\d+>>      ParameterValue
  /// CHECK-DAG:      <<Const13:i\d+>>  IntConstant 13
  /// CHECK-DAG:      <<Const54:i\d+>>  IntConstant 54
  /// CHECK-DAG:      <<Const42:f\d+>>  FloatConstant 42
  /// CHECK-DAG:      <<LE:z\d+>>       LessThanOrEqual [<<Arg>>,<<Const42>>]
  /// CHECK-DAG:      <<Select:i\d+>>   Select [<<Const13>>,<<Const54>>,<<LE>>]
  /// CHECK-DAG:                        Return [<<Select>>]

  public static int floatConditionNotEqualOne(float f) {
    return ((f > 42.0f) == true) ? 13 : 54;
  }

  /// CHECK-START: int Main.doubleConditionEqualZero(double) builder (after)
  /// CHECK:                            LessThanOrEqual

  /// CHECK-START: int Main.doubleConditionEqualZero(double) instruction_simplifier_before_codegen (after)
  /// CHECK-DAG:      <<Arg:d\d+>>      ParameterValue
  /// CHECK-DAG:      <<Const13:i\d+>>  IntConstant 13
  /// CHECK-DAG:      <<Const54:i\d+>>  IntConstant 54
  /// CHECK-DAG:      <<Const42:d\d+>>  DoubleConstant 42
  /// CHECK-DAG:      <<LE:z\d+>>       LessThanOrEqual [<<Arg>>,<<Const42>>]
  /// CHECK-DAG:      <<Select:i\d+>>   Select [<<Const13>>,<<Const54>>,<<LE>>]
  /// CHECK-DAG:                        Return [<<Select>>]

  public static int doubleConditionEqualZero(double d) {
    return ((d > 42.0) != false) ? 13 : 54;
  }

  /// CHECK-START: int Main.intToDoubleToInt(int) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:      <<Double:d\d+>>   TypeConversion [<<Arg>>]
  /// CHECK-DAG:      <<Int:i\d+>>      TypeConversion [<<Double>>]
  /// CHECK-DAG:                        Return [<<Int>>]

  /// CHECK-START: int Main.intToDoubleToInt(int) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:                        Return [<<Arg>>]

  /// CHECK-START: int Main.intToDoubleToInt(int) instruction_simplifier (after)
  /// CHECK-NOT:                        TypeConversion

  public static int intToDoubleToInt(int value) {
    // Lossless conversion followed by a conversion back.
    return (int) (double) value;
  }

  /// CHECK-START: java.lang.String Main.intToDoubleToIntPrint(int) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:      <<Double:d\d+>>   TypeConversion [<<Arg>>]
  /// CHECK-DAG:      {{i\d+}}          TypeConversion [<<Double>>]

  /// CHECK-START: java.lang.String Main.intToDoubleToIntPrint(int) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:      {{d\d+}}          TypeConversion [<<Arg>>]

  /// CHECK-START: java.lang.String Main.intToDoubleToIntPrint(int) instruction_simplifier (after)
  /// CHECK-DAG:                        TypeConversion
  /// CHECK-NOT:                        TypeConversion

  public static String intToDoubleToIntPrint(int value) {
    // Lossless conversion followed by a conversion back
    // with another use of the intermediate result.
    double d = (double) value;
    int i = (int) d;
    return "d=" + d + ", i=" + i;
  }

  /// CHECK-START: int Main.byteToDoubleToInt(byte) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:b\d+>>      ParameterValue
  /// CHECK-DAG:      <<Double:d\d+>>   TypeConversion [<<Arg>>]
  /// CHECK-DAG:      <<Int:i\d+>>      TypeConversion [<<Double>>]
  /// CHECK-DAG:                        Return [<<Int>>]

  /// CHECK-START: int Main.byteToDoubleToInt(byte) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:b\d+>>      ParameterValue
  /// CHECK-DAG:                        Return [<<Arg>>]

  /// CHECK-START: int Main.byteToDoubleToInt(byte) instruction_simplifier (after)
  /// CHECK-NOT:                        TypeConversion

  public static int byteToDoubleToInt(byte value) {
    // Lossless conversion followed by another conversion, use implicit conversion.
    return (int) (double) value;
  }

  /// CHECK-START: int Main.floatToDoubleToInt(float) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:f\d+>>      ParameterValue
  /// CHECK-DAG:      <<Double:d\d+>>   TypeConversion [<<Arg>>]
  /// CHECK-DAG:      <<Int:i\d+>>      TypeConversion [<<Double>>]
  /// CHECK-DAG:                        Return [<<Int>>]

  /// CHECK-START: int Main.floatToDoubleToInt(float) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:f\d+>>      ParameterValue
  /// CHECK-DAG:      <<Int:i\d+>>      TypeConversion [<<Arg>>]
  /// CHECK-DAG:                        Return [<<Int>>]

  /// CHECK-START: int Main.floatToDoubleToInt(float) instruction_simplifier (after)
  /// CHECK-DAG:                        TypeConversion
  /// CHECK-NOT:                        TypeConversion

  public static int floatToDoubleToInt(float value) {
    // Lossless conversion followed by another conversion.
    return (int) (double) value;
  }

  /// CHECK-START: java.lang.String Main.floatToDoubleToIntPrint(float) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:f\d+>>      ParameterValue
  /// CHECK-DAG:      <<Double:d\d+>>   TypeConversion [<<Arg>>]
  /// CHECK-DAG:      {{i\d+}}          TypeConversion [<<Double>>]

  /// CHECK-START: java.lang.String Main.floatToDoubleToIntPrint(float) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:f\d+>>      ParameterValue
  /// CHECK-DAG:      <<Double:d\d+>>   TypeConversion [<<Arg>>]
  /// CHECK-DAG:      {{i\d+}}          TypeConversion [<<Double>>]

  public static String floatToDoubleToIntPrint(float value) {
    // Lossless conversion followed by another conversion with
    // an extra use of the intermediate result.
    double d = (double) value;
    int i = (int) d;
    return "d=" + d + ", i=" + i;
  }

  /// CHECK-START: short Main.byteToDoubleToShort(byte) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:b\d+>>      ParameterValue
  /// CHECK-DAG:      <<Double:d\d+>>   TypeConversion [<<Arg>>]
  /// CHECK-DAG:      <<Int:i\d+>>      TypeConversion [<<Double>>]
  /// CHECK-DAG:      <<Short:s\d+>>    TypeConversion [<<Int>>]
  /// CHECK-DAG:                        Return [<<Short>>]

  /// CHECK-START: short Main.byteToDoubleToShort(byte) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:b\d+>>      ParameterValue
  /// CHECK-DAG:                        Return [<<Arg>>]

  /// CHECK-START: short Main.byteToDoubleToShort(byte) instruction_simplifier (after)
  /// CHECK-NOT:                        TypeConversion

  public static short byteToDoubleToShort(byte value) {
    // Originally, this is byte->double->int->short. The first conversion is lossless,
    // so we merge this with the second one to byte->int which we omit as it's an implicit
    // conversion. Then we eliminate the resulting byte->short as an implicit conversion.
    return (short) (double) value;
  }

  /// CHECK-START: short Main.charToDoubleToShort(char) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:c\d+>>      ParameterValue
  /// CHECK-DAG:      <<Double:d\d+>>   TypeConversion [<<Arg>>]
  /// CHECK-DAG:      <<Int:i\d+>>      TypeConversion [<<Double>>]
  /// CHECK-DAG:      <<Short:s\d+>>    TypeConversion [<<Int>>]
  /// CHECK-DAG:                        Return [<<Short>>]

  /// CHECK-START: short Main.charToDoubleToShort(char) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:c\d+>>      ParameterValue
  /// CHECK-DAG:      <<Short:s\d+>>    TypeConversion [<<Arg>>]
  /// CHECK-DAG:                        Return [<<Short>>]

  /// CHECK-START: short Main.charToDoubleToShort(char) instruction_simplifier (after)
  /// CHECK-DAG:                        TypeConversion
  /// CHECK-NOT:                        TypeConversion

  public static short charToDoubleToShort(char value) {
    // Originally, this is char->double->int->short. The first conversion is lossless,
    // so we merge this with the second one to char->int which we omit as it's an implicit
    // conversion. Then we are left with the resulting char->short conversion.
    return (short) (double) value;
  }

  /// CHECK-START: short Main.floatToIntToShort(float) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:f\d+>>      ParameterValue
  /// CHECK-DAG:      <<Int:i\d+>>      TypeConversion [<<Arg>>]
  /// CHECK-DAG:      <<Short:s\d+>>    TypeConversion [<<Int>>]
  /// CHECK-DAG:                        Return [<<Short>>]

  /// CHECK-START: short Main.floatToIntToShort(float) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:f\d+>>      ParameterValue
  /// CHECK-DAG:      <<Int:i\d+>>      TypeConversion [<<Arg>>]
  /// CHECK-DAG:      <<Short:s\d+>>    TypeConversion [<<Int>>]
  /// CHECK-DAG:                        Return [<<Short>>]

  public static short floatToIntToShort(float value) {
    // Lossy FP to integral conversion followed by another conversion: no simplification.
    return (short) value;
  }

  /// CHECK-START: int Main.intToFloatToInt(int) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:      <<Float:f\d+>>    TypeConversion [<<Arg>>]
  /// CHECK-DAG:      <<Int:i\d+>>      TypeConversion [<<Float>>]
  /// CHECK-DAG:                        Return [<<Int>>]

  /// CHECK-START: int Main.intToFloatToInt(int) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:      <<Float:f\d+>>    TypeConversion [<<Arg>>]
  /// CHECK-DAG:      <<Int:i\d+>>      TypeConversion [<<Float>>]
  /// CHECK-DAG:                        Return [<<Int>>]

  public static int intToFloatToInt(int value) {
    // Lossy integral to FP conversion followed another conversion: no simplification.
    return (int) (float) value;
  }

  /// CHECK-START: double Main.longToIntToDouble(long) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:      <<Int:i\d+>>      TypeConversion [<<Arg>>]
  /// CHECK-DAG:      <<Double:d\d+>>   TypeConversion [<<Int>>]
  /// CHECK-DAG:                        Return [<<Double>>]

  /// CHECK-START: double Main.longToIntToDouble(long) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:      <<Int:i\d+>>      TypeConversion [<<Arg>>]
  /// CHECK-DAG:      <<Double:d\d+>>   TypeConversion [<<Int>>]
  /// CHECK-DAG:                        Return [<<Double>>]

  public static double longToIntToDouble(long value) {
    // Lossy long-to-int conversion followed an integral to FP conversion: no simplification.
    return (double) (int) value;
  }

  /// CHECK-START: long Main.longToIntToLong(long) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:      <<Int:i\d+>>      TypeConversion [<<Arg>>]
  /// CHECK-DAG:      <<Long:j\d+>>     TypeConversion [<<Int>>]
  /// CHECK-DAG:                        Return [<<Long>>]

  /// CHECK-START: long Main.longToIntToLong(long) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:      <<Int:i\d+>>      TypeConversion [<<Arg>>]
  /// CHECK-DAG:      <<Long:j\d+>>     TypeConversion [<<Int>>]
  /// CHECK-DAG:                        Return [<<Long>>]

  public static long longToIntToLong(long value) {
    // Lossy long-to-int conversion followed an int-to-long conversion: no simplification.
    return (long) (int) value;
  }

  /// CHECK-START: short Main.shortToCharToShort(short) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:s\d+>>      ParameterValue
  /// CHECK-DAG:      <<Char:c\d+>>     TypeConversion [<<Arg>>]
  /// CHECK-DAG:      <<Short:s\d+>>    TypeConversion [<<Char>>]
  /// CHECK-DAG:                        Return [<<Short>>]

  /// CHECK-START: short Main.shortToCharToShort(short) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:s\d+>>      ParameterValue
  /// CHECK-DAG:                        Return [<<Arg>>]

  public static short shortToCharToShort(short value) {
    // Integral conversion followed by non-widening integral conversion to original type.
    return (short) (char) value;
  }

  /// CHECK-START: int Main.shortToLongToInt(short) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:s\d+>>      ParameterValue
  /// CHECK-DAG:      <<Long:j\d+>>     TypeConversion [<<Arg>>]
  /// CHECK-DAG:      <<Int:i\d+>>      TypeConversion [<<Long>>]
  /// CHECK-DAG:                        Return [<<Int>>]

  /// CHECK-START: int Main.shortToLongToInt(short) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:s\d+>>      ParameterValue
  /// CHECK-DAG:                        Return [<<Arg>>]

  public static int shortToLongToInt(short value) {
    // Integral conversion followed by non-widening integral conversion, use implicit conversion.
    return (int) (long) value;
  }

  /// CHECK-START: byte Main.shortToCharToByte(short) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:s\d+>>      ParameterValue
  /// CHECK-DAG:      <<Char:c\d+>>     TypeConversion [<<Arg>>]
  /// CHECK-DAG:      <<Byte:b\d+>>     TypeConversion [<<Char>>]
  /// CHECK-DAG:                        Return [<<Byte>>]

  /// CHECK-START: byte Main.shortToCharToByte(short) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:s\d+>>      ParameterValue
  /// CHECK-DAG:      <<Byte:b\d+>>     TypeConversion [<<Arg>>]
  /// CHECK-DAG:                        Return [<<Byte>>]

  public static byte shortToCharToByte(short value) {
    // Integral conversion followed by non-widening integral conversion losing bits
    // from the original type. Simplify to use only one conversion.
    return (byte) (char) value;
  }

  /// CHECK-START: java.lang.String Main.shortToCharToBytePrint(short) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:s\d+>>      ParameterValue
  /// CHECK-DAG:      <<Char:c\d+>>     TypeConversion [<<Arg>>]
  /// CHECK-DAG:      {{b\d+}}          TypeConversion [<<Char>>]

  /// CHECK-START: java.lang.String Main.shortToCharToBytePrint(short) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:s\d+>>      ParameterValue
  /// CHECK-DAG:      <<Char:c\d+>>     TypeConversion [<<Arg>>]
  /// CHECK-DAG:      {{b\d+}}          TypeConversion [<<Char>>]

  public static String shortToCharToBytePrint(short value) {
    // Integral conversion followed by non-widening integral conversion losing bits
    // from the original type with an extra use of the intermediate result.
    char c = (char) value;
    byte b = (byte) c;
    return "c=" + ((int) c) + ", b=" + ((int) b);  // implicit conversions.
  }

  /// CHECK-START: byte Main.longAnd0xffToByte(long) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:      <<Mask:j\d+>>     LongConstant 255
  /// CHECK-DAG:      <<And:j\d+>>      And [<<Mask>>,<<Arg>>]
  /// CHECK-DAG:      <<Int:i\d+>>      TypeConversion [<<And>>]
  /// CHECK-DAG:      <<Byte:b\d+>>     TypeConversion [<<Int>>]
  /// CHECK-DAG:                        Return [<<Byte>>]

  /// CHECK-START: byte Main.longAnd0xffToByte(long) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:j\d+>>      ParameterValue
  /// CHECK-DAG:      <<Byte:b\d+>>     TypeConversion [<<Arg>>]
  /// CHECK-DAG:                        Return [<<Byte>>]

  /// CHECK-START: byte Main.longAnd0xffToByte(long) instruction_simplifier (after)
  /// CHECK-NOT:                        And

  public static byte longAnd0xffToByte(long value) {
    return (byte) (value & 0xff);
  }

  /// CHECK-START: char Main.intAnd0x1ffffToChar(int) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:      <<Mask:i\d+>>     IntConstant 131071
  /// CHECK-DAG:      <<And:i\d+>>      And [<<Mask>>,<<Arg>>]
  /// CHECK-DAG:      <<Char:c\d+>>     TypeConversion [<<And>>]
  /// CHECK-DAG:                        Return [<<Char>>]

  /// CHECK-START: char Main.intAnd0x1ffffToChar(int) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:      <<Char:c\d+>>     TypeConversion [<<Arg>>]
  /// CHECK-DAG:                        Return [<<Char>>]

  /// CHECK-START: char Main.intAnd0x1ffffToChar(int) instruction_simplifier (after)
  /// CHECK-NOT:                        And

  public static char intAnd0x1ffffToChar(int value) {
    // Keeping all significant bits and one more.
    return (char) (value & 0x1ffff);
  }

  /// CHECK-START: short Main.intAnd0x17fffToShort(int) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:      <<Mask:i\d+>>     IntConstant 98303
  /// CHECK-DAG:      <<And:i\d+>>      And [<<Mask>>,<<Arg>>]
  /// CHECK-DAG:      <<Short:s\d+>>    TypeConversion [<<And>>]
  /// CHECK-DAG:                        Return [<<Short>>]

  /// CHECK-START: short Main.intAnd0x17fffToShort(int) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:      <<Mask:i\d+>>     IntConstant 98303
  /// CHECK-DAG:      <<And:i\d+>>      And [<<Mask>>,<<Arg>>]
  /// CHECK-DAG:      <<Short:s\d+>>    TypeConversion [<<And>>]
  /// CHECK-DAG:                        Return [<<Short>>]

  public static short intAnd0x17fffToShort(int value) {
    // No simplification: clearing a significant bit.
    return (short) (value & 0x17fff);
  }

  /// CHECK-START: double Main.shortAnd0xffffToShortToDouble(short) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:s\d+>>      ParameterValue
  /// CHECK-DAG:      <<Mask:i\d+>>     IntConstant 65535
  /// CHECK-DAG:      <<And:i\d+>>      And [<<Mask>>,<<Arg>>]
  /// CHECK-DAG:      <<Same:s\d+>>     TypeConversion [<<And>>]
  /// CHECK-DAG:      <<Double:d\d+>>   TypeConversion [<<Same>>]
  /// CHECK-DAG:                        Return [<<Double>>]

  /// CHECK-START: double Main.shortAnd0xffffToShortToDouble(short) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:s\d+>>      ParameterValue
  /// CHECK-DAG:      <<Double:d\d+>>   TypeConversion [<<Arg>>]
  /// CHECK-DAG:                        Return [<<Double>>]

  public static double shortAnd0xffffToShortToDouble(short value) {
    short same = (short) (value & 0xffff);
    return (double) same;
  }

  /// CHECK-START: int Main.intReverseCondition(int) instruction_simplifier (before)
  /// CHECK-DAG:      <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:      <<Const42:i\d+>>  IntConstant 42
  /// CHECK-DAG:      <<LE:z\d+>>       LessThanOrEqual [<<Const42>>,<<Arg>>]

  /// CHECK-START: int Main.intReverseCondition(int) instruction_simplifier (after)
  /// CHECK-DAG:      <<Arg:i\d+>>      ParameterValue
  /// CHECK-DAG:      <<Const42:i\d+>>  IntConstant 42
  /// CHECK-DAG:      <<GE:z\d+>>       GreaterThanOrEqual [<<Arg>>,<<Const42>>]

  public static int intReverseCondition(int i) {
    return (42 > i) ? 13 : 54;
  }

  /// CHECK-START: int Main.intReverseConditionNaN(int) instruction_simplifier (before)
  /// CHECK-DAG:      <<Const42:d\d+>>  DoubleConstant 42
  /// CHECK-DAG:      <<Result:d\d+>>   InvokeStaticOrDirect
  /// CHECK-DAG:      <<CMP:i\d+>>      Compare [<<Const42>>,<<Result>>]

  /// CHECK-START: int Main.intReverseConditionNaN(int) instruction_simplifier (after)
  /// CHECK-DAG:      <<Const42:d\d+>>  DoubleConstant 42
  /// CHECK-DAG:      <<Result:d\d+>>   InvokeStaticOrDirect
  /// CHECK-DAG:      <<EQ:z\d+>>       Equal [<<Result>>,<<Const42>>]

  public static int intReverseConditionNaN(int i) {
    return (42 != Math.sqrt(i)) ? 13 : 54;
  }

  public static int runSmaliTest(String name, boolean input) {
    try {
      Class<?> c = Class.forName("SmaliTests");
      Method m = c.getMethod(name, new Class[] { boolean.class });
      return (Integer) m.invoke(null, input);
    } catch (Exception ex) {
      throw new Error(ex);
    }
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
    assertLongEquals(Shr64(arg), arg);
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
    assertBooleanEquals(EqualBoolVsIntConst(true), true);
    assertBooleanEquals(EqualBoolVsIntConst(true), true);
    assertBooleanEquals(NotEqualBoolVsIntConst(false), false);
    assertBooleanEquals(NotEqualBoolVsIntConst(false), false);
    assertBooleanEquals(NotNotBool(true), true);
    assertBooleanEquals(NotNotBool(false), false);
    assertFloatEquals(Div2(100.0f), 50.0f);
    assertDoubleEquals(Div2(150.0), 75.0);
    assertFloatEquals(DivMP25(100.0f), -400.0f);
    assertDoubleEquals(DivMP25(150.0), -600.0);
    assertIntEquals(UShr28And15(0xc1234567), 0xc);
    assertLongEquals(UShr60And15(0xc123456787654321L), 0xcL);
    assertIntEquals(UShr28And7(0xc1234567), 0x4);
    assertLongEquals(UShr60And7(0xc123456787654321L), 0x4L);
    assertIntEquals(Shr24And255(0xc1234567), 0xc1);
    assertLongEquals(Shr56And255(0xc123456787654321L), 0xc1L);
    assertIntEquals(Shr24And127(0xc1234567), 0x41);
    assertLongEquals(Shr56And127(0xc123456787654321L), 0x41L);
    assertIntEquals(0, mulPow2Plus1(0));
    assertIntEquals(9, mulPow2Plus1(1));
    assertIntEquals(18, mulPow2Plus1(2));
    assertIntEquals(900, mulPow2Plus1(100));
    assertIntEquals(111105, mulPow2Plus1(12345));
    assertLongEquals(0, mulPow2Minus1(0));
    assertLongEquals(31, mulPow2Minus1(1));
    assertLongEquals(62, mulPow2Minus1(2));
    assertLongEquals(3100, mulPow2Minus1(100));
    assertLongEquals(382695, mulPow2Minus1(12345));

    booleanField = false;
    assertIntEquals(booleanFieldNotEqualOne(), 54);
    assertIntEquals(booleanFieldEqualZero(), 54);
    booleanField = true;
    assertIntEquals(booleanFieldNotEqualOne(), 13);
    assertIntEquals(booleanFieldEqualZero(), 13);
    assertIntEquals(intConditionNotEqualOne(6), 54);
    assertIntEquals(intConditionNotEqualOne(43), 13);
    assertIntEquals(intConditionEqualZero(6), 54);
    assertIntEquals(intConditionEqualZero(43), 13);
    assertIntEquals(floatConditionNotEqualOne(6.0f), 54);
    assertIntEquals(floatConditionNotEqualOne(43.0f), 13);
    assertIntEquals(doubleConditionEqualZero(6.0), 54);
    assertIntEquals(doubleConditionEqualZero(43.0), 13);

    assertIntEquals(1234567, intToDoubleToInt(1234567));
    assertIntEquals(Integer.MIN_VALUE, intToDoubleToInt(Integer.MIN_VALUE));
    assertIntEquals(Integer.MAX_VALUE, intToDoubleToInt(Integer.MAX_VALUE));
    assertStringEquals("d=7654321.0, i=7654321", intToDoubleToIntPrint(7654321));
    assertIntEquals(12, byteToDoubleToInt((byte) 12));
    assertIntEquals(Byte.MIN_VALUE, byteToDoubleToInt(Byte.MIN_VALUE));
    assertIntEquals(Byte.MAX_VALUE, byteToDoubleToInt(Byte.MAX_VALUE));
    assertIntEquals(11, floatToDoubleToInt(11.3f));
    assertStringEquals("d=12.25, i=12", floatToDoubleToIntPrint(12.25f));
    assertIntEquals(123, byteToDoubleToShort((byte) 123));
    assertIntEquals(Byte.MIN_VALUE, byteToDoubleToShort(Byte.MIN_VALUE));
    assertIntEquals(Byte.MAX_VALUE, byteToDoubleToShort(Byte.MAX_VALUE));
    assertIntEquals(1234, charToDoubleToShort((char) 1234));
    assertIntEquals(Character.MIN_VALUE, charToDoubleToShort(Character.MIN_VALUE));
    assertIntEquals(/* sign-extended */ -1, charToDoubleToShort(Character.MAX_VALUE));
    assertIntEquals(12345, floatToIntToShort(12345.75f));
    assertIntEquals(Short.MAX_VALUE, floatToIntToShort((float)(Short.MIN_VALUE - 1)));
    assertIntEquals(Short.MIN_VALUE, floatToIntToShort((float)(Short.MAX_VALUE + 1)));
    assertIntEquals(-54321, intToFloatToInt(-54321));
    assertDoubleEquals((double) 0x12345678, longToIntToDouble(0x1234567812345678L));
    assertDoubleEquals(0.0, longToIntToDouble(Long.MIN_VALUE));
    assertDoubleEquals(-1.0, longToIntToDouble(Long.MAX_VALUE));
    assertLongEquals(0x0000000012345678L, longToIntToLong(0x1234567812345678L));
    assertLongEquals(0xffffffff87654321L, longToIntToLong(0x1234567887654321L));
    assertLongEquals(0L, longToIntToLong(Long.MIN_VALUE));
    assertLongEquals(-1L, longToIntToLong(Long.MAX_VALUE));
    assertIntEquals((short) -5678, shortToCharToShort((short) -5678));
    assertIntEquals(Short.MIN_VALUE, shortToCharToShort(Short.MIN_VALUE));
    assertIntEquals(Short.MAX_VALUE, shortToCharToShort(Short.MAX_VALUE));
    assertIntEquals(5678, shortToLongToInt((short) 5678));
    assertIntEquals(Short.MIN_VALUE, shortToLongToInt(Short.MIN_VALUE));
    assertIntEquals(Short.MAX_VALUE, shortToLongToInt(Short.MAX_VALUE));
    assertIntEquals(0x34, shortToCharToByte((short) 0x1234));
    assertIntEquals(-0x10, shortToCharToByte((short) 0x12f0));
    assertIntEquals(0, shortToCharToByte(Short.MIN_VALUE));
    assertIntEquals(-1, shortToCharToByte(Short.MAX_VALUE));
    assertStringEquals("c=1025, b=1", shortToCharToBytePrint((short) 1025));
    assertStringEquals("c=1023, b=-1", shortToCharToBytePrint((short) 1023));
    assertStringEquals("c=65535, b=-1", shortToCharToBytePrint((short) -1));

    assertIntEquals(0x21, longAnd0xffToByte(0x1234432112344321L));
    assertIntEquals(0, longAnd0xffToByte(Long.MIN_VALUE));
    assertIntEquals(-1, longAnd0xffToByte(Long.MAX_VALUE));
    assertIntEquals(0x1234, intAnd0x1ffffToChar(0x43211234));
    assertIntEquals(0, intAnd0x1ffffToChar(Integer.MIN_VALUE));
    assertIntEquals(Character.MAX_VALUE, intAnd0x1ffffToChar(Integer.MAX_VALUE));
    assertIntEquals(0x4321, intAnd0x17fffToShort(0x87654321));
    assertIntEquals(0x0888, intAnd0x17fffToShort(0x88888888));
    assertIntEquals(0, intAnd0x17fffToShort(Integer.MIN_VALUE));
    assertIntEquals(Short.MAX_VALUE, intAnd0x17fffToShort(Integer.MAX_VALUE));

    assertDoubleEquals(0.0, shortAnd0xffffToShortToDouble((short) 0));
    assertDoubleEquals(1.0, shortAnd0xffffToShortToDouble((short) 1));
    assertDoubleEquals(-2.0, shortAnd0xffffToShortToDouble((short) -2));
    assertDoubleEquals(12345.0, shortAnd0xffffToShortToDouble((short) 12345));
    assertDoubleEquals((double)Short.MAX_VALUE, shortAnd0xffffToShortToDouble(Short.MAX_VALUE));
    assertDoubleEquals((double)Short.MIN_VALUE, shortAnd0xffffToShortToDouble(Short.MIN_VALUE));

    assertIntEquals(intReverseCondition(41), 13);
    assertIntEquals(intReverseConditionNaN(-5), 13);

    for (String condition : new String[] { "Equal", "NotEqual" }) {
      for (String constant : new String[] { "True", "False" }) {
        for (String side : new String[] { "Rhs", "Lhs" }) {
          String name = condition + constant + side;
          assertIntEquals(runSmaliTest(name, true), 5);
          assertIntEquals(runSmaliTest(name, false), 3);
        }
      }
    }
  }

  private static boolean $inline$true() { return true; }
  private static boolean $inline$false() { return false; }

  public static boolean booleanField;
}
