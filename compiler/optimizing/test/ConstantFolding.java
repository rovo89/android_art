/*
* Copyright (C) 2014 The Android Open Source Project
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

public class ConstantFolding {

  /**
   * Tiny three-register program exercising int constant folding
   * on negation.
   */

  // CHECK-START: int ConstantFolding.IntNegation() constant_folding (before)
  // CHECK-DAG:     [[Const42:i\d+]]  IntConstant 42
  // CHECK-DAG:     [[Neg:i\d+]]      Neg [ [[Const42]] ]
  // CHECK-DAG:                       Return [ [[Neg]] ]

  // CHECK-START: int ConstantFolding.IntNegation() constant_folding (after)
  // CHECK-DAG:     [[ConstN42:i\d+]] IntConstant -42
  // CHECK-DAG:                       Return [ [[ConstN42]] ]

  public static int IntNegation() {
    int x, y;
    x = 42;
    y = -x;
    return y;
  }

  /**
   * Tiny three-register program exercising int constant folding
   * on addition.
   */

  // CHECK-START: int ConstantFolding.IntAddition1() constant_folding (before)
  // CHECK-DAG:     [[Const1:i\d+]]  IntConstant 1
  // CHECK-DAG:     [[Const2:i\d+]]  IntConstant 2
  // CHECK-DAG:     [[Add:i\d+]]     Add [ [[Const1]] [[Const2]] ]
  // CHECK-DAG:                      Return [ [[Add]] ]

  // CHECK-START: int ConstantFolding.IntAddition1() constant_folding (after)
  // CHECK-DAG:     [[Const3:i\d+]]  IntConstant 3
  // CHECK-DAG:                      Return [ [[Const3]] ]

  public static int IntAddition1() {
    int a, b, c;
    a = 1;
    b = 2;
    c = a + b;
    return c;
  }

 /**
  * Small three-register program exercising int constant folding
  * on addition.
  */

  // CHECK-START: int ConstantFolding.IntAddition2() constant_folding (before)
  // CHECK-DAG:     [[Const1:i\d+]]  IntConstant 1
  // CHECK-DAG:     [[Const2:i\d+]]  IntConstant 2
  // CHECK-DAG:     [[Const5:i\d+]]  IntConstant 5
  // CHECK-DAG:     [[Const6:i\d+]]  IntConstant 6
  // CHECK-DAG:     [[Add1:i\d+]]    Add [ [[Const1]] [[Const2]] ]
  // CHECK-DAG:     [[Add2:i\d+]]    Add [ [[Const5]] [[Const6]] ]
  // CHECK-DAG:     [[Add3:i\d+]]    Add [ [[Add1]] [[Add2]] ]
  // CHECK-DAG:                      Return [ [[Add3]] ]

  // CHECK-START: int ConstantFolding.IntAddition2() constant_folding (after)
  // CHECK-DAG:     [[Const14:i\d+]] IntConstant 14
  // CHECK-DAG:                      Return [ [[Const14]] ]

  public static int IntAddition2() {
    int a, b, c;
    a = 1;
    b = 2;
    a += b;
    b = 5;
    c = 6;
    b += c;
    c = a + b;
    return c;
  }

  /**
   * Tiny three-register program exercising int constant folding
   * on subtraction.
   */

  // CHECK-START: int ConstantFolding.IntSubtraction() constant_folding (before)
  // CHECK-DAG:     [[Const5:i\d+]]  IntConstant 5
  // CHECK-DAG:     [[Const2:i\d+]]  IntConstant 2
  // CHECK-DAG:     [[Sub:i\d+]]     Sub [ [[Const5]] [[Const2]] ]
  // CHECK-DAG:                      Return [ [[Sub]] ]

  // CHECK-START: int ConstantFolding.IntSubtraction() constant_folding (after)
  // CHECK-DAG:     [[Const3:i\d+]]  IntConstant 3
  // CHECK-DAG:                      Return [ [[Const3]] ]

  public static int IntSubtraction() {
    int a, b, c;
    a = 5;
    b = 2;
    c = a - b;
    return c;
  }

  /**
   * Tiny three-register program exercising long constant folding
   * on addition.
   */

  // CHECK-START: long ConstantFolding.LongAddition() constant_folding (before)
  // CHECK-DAG:     [[Const1:j\d+]]  LongConstant 1
  // CHECK-DAG:     [[Const2:j\d+]]  LongConstant 2
  // CHECK-DAG:     [[Add:j\d+]]     Add [ [[Const1]] [[Const2]] ]
  // CHECK-DAG:                      Return [ [[Add]] ]

  // CHECK-START: long ConstantFolding.LongAddition() constant_folding (after)
  // CHECK-DAG:     [[Const3:j\d+]]  LongConstant 3
  // CHECK-DAG:                      Return [ [[Const3]] ]

  public static long LongAddition() {
    long a, b, c;
    a = 1L;
    b = 2L;
    c = a + b;
    return c;
  }

  /**
   * Tiny three-register program exercising long constant folding
   * on subtraction.
   */

  // CHECK-START: long ConstantFolding.LongSubtraction() constant_folding (before)
  // CHECK-DAG:     [[Const5:j\d+]]  LongConstant 5
  // CHECK-DAG:     [[Const2:j\d+]]  LongConstant 2
  // CHECK-DAG:     [[Sub:j\d+]]     Sub [ [[Const5]] [[Const2]] ]
  // CHECK-DAG:                      Return [ [[Sub]] ]

  // CHECK-START: long ConstantFolding.LongSubtraction() constant_folding (after)
  // CHECK-DAG:     [[Const3:j\d+]]  LongConstant 3
  // CHECK-DAG:                      Return [ [[Const3]] ]

  public static long LongSubtraction() {
    long a, b, c;
    a = 5L;
    b = 2L;
    c = a - b;
    return c;
  }

  /**
   * Three-register program with a constant (static) condition.
   */

  // CHECK-START: int ConstantFolding.StaticCondition() constant_folding (before)
  // CHECK-DAG:     [[Const5:i\d+]]  IntConstant 5
  // CHECK-DAG:     [[Const2:i\d+]]  IntConstant 2
  // CHECK-DAG:     [[Cond:z\d+]]    GreaterThanOrEqual [ [[Const5]] [[Const2]] ]
  // CHECK-DAG:                      If [ [[Cond]] ]

  // CHECK-START: int ConstantFolding.StaticCondition() constant_folding (after)
  // CHECK-DAG:     [[Const1:i\d+]]  IntConstant 1
  // CHECK-DAG:                      If [ [[Const1]] ]

  public static int StaticCondition() {
    int a, b, c;
    a = 5;
    b = 2;
    if (a < b)
      c = a + b;
    else
      c = a - b;
    return c;
  }

  /**
   * Four-variable program with jumps leading to the creation of many
   * blocks.
   *
   * The intent of this test is to ensure that all constant expressions
   * are actually evaluated at compile-time, thanks to the reverse
   * (forward) post-order traversal of the the dominator tree.
   */

  // CHECK-START: int ConstantFolding.JumpsAndConditionals(boolean) constant_folding (before)
  // CHECK-DAG:     [[Const2:i\d+]]  IntConstant 2
  // CHECK-DAG:     [[Const5:i\d+]]  IntConstant 5
  // CHECK-DAG:     [[Add:i\d+]]     Add [ [[Const5]] [[Const2]] ]
  // CHECK-DAG:     [[Sub:i\d+]]     Sub [ [[Const5]] [[Const2]] ]
  // CHECK-DAG:     [[Phi:i\d+]]     Phi [ [[Add]] [[Sub]] ]
  // CHECK-DAG:                      Return [ [[Phi]] ]

  // CHECK-START: int ConstantFolding.JumpsAndConditionals(boolean) constant_folding (after)
  // CHECK-DAG:     [[Const3:i\d+]]  IntConstant 3
  // CHECK-DAG:     [[Const7:i\d+]]  IntConstant 7
  // CHECK-DAG:     [[Phi:i\d+]]     Phi [ [[Const7]] [[Const3]] ]
  // CHECK-DAG:                      Return [ [[Phi]] ]

  public static int JumpsAndConditionals(boolean cond) {
    int a, b, c;
    a = 5;
    b = 2;
    if (cond)
      c = a + b;
    else
      c = a - b;
    return c;
  }
}
