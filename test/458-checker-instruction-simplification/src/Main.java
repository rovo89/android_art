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
  //
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
  }
}
