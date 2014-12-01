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
  // CHECK:   [[Const42:i[0-9]+]]  IntConstant 42
  // CHECK:   [[Neg:i[0-9]+]]      Neg [ [[Const42]] ]
  // CHECK:                        Return [ [[Neg]] ]

  // CHECK-START: int ConstantFolding.IntNegation() constant_folding (after)
  // CHECK:   [[ConstN42:i[0-9]+]] IntConstant -42
  // CHECK:                        Return [ [[ConstN42]] ]

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
  // CHECK:   [[Const1:i[0-9]+]]   IntConstant 1
  // CHECK:   [[Const2:i[0-9]+]]   IntConstant 2
  // CHECK:   [[Add:i[0-9]+]]      Add [ [[Const1]] [[Const2]] ]
  // CHECK:                        Return [ [[Add]] ]

  // CHECK-START: int ConstantFolding.IntAddition1() constant_folding (after)
  // CHECK:   [[Const3:i[0-9]+]]   IntConstant 3
  // CHECK:                        Return [ [[Const3]] ]

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
  // CHECK:   [[Const1:i[0-9]+]]   IntConstant 1
  // CHECK:   [[Const2:i[0-9]+]]   IntConstant 2
  // CHECK:   [[Const5:i[0-9]+]]   IntConstant 5
  // CHECK:   [[Const6:i[0-9]+]]   IntConstant 6
  // CHECK:   [[Add1:i[0-9]+]]     Add [ [[Const1]] [[Const2]] ]
  // CHECK:   [[Add2:i[0-9]+]]     Add [ [[Const5]] [[Const6]] ]
  // CHECK:   [[Add3:i[0-9]+]]     Add [ [[Add1]] [[Add2]] ]
  // CHECK:                        Return [ [[Add3]] ]

  // CHECK-START: int ConstantFolding.IntAddition2() constant_folding (after)
  // CHECK:   [[Const14:i[0-9]+]]  IntConstant 14
  // CHECK:                        Return [ [[Const14]] ]

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
  // CHECK:   [[Const5:i[0-9]+]]   IntConstant 5
  // CHECK:   [[Const2:i[0-9]+]]   IntConstant 2
  // CHECK:   [[Sub:i[0-9]+]]      Sub [ [[Const5]] [[Const2]] ]
  // CHECK:                        Return [ [[Sub]] ]

  // CHECK-START: int ConstantFolding.IntSubtraction() constant_folding (after)
  // CHECK:   [[Const3:i[0-9]+]]   IntConstant 3
  // CHECK:                        Return [ [[Const3]] ]

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
  // CHECK:   [[Const1:j[0-9]+]]   LongConstant 1
  // CHECK:   [[Const2:j[0-9]+]]   LongConstant 2
  // CHECK:   [[Add:j[0-9]+]]      Add [ [[Const1]] [[Const2]] ]
  // CHECK:                        Return [ [[Add]] ]

  // CHECK-START: long ConstantFolding.LongAddition() constant_folding (after)
  // CHECK:   [[Const3:j[0-9]+]]   LongConstant 3
  // CHECK:                        Return [ [[Const3]] ]

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
  // CHECK:   [[Const5:j[0-9]+]]   LongConstant 5
  // CHECK:   [[Const2:j[0-9]+]]   LongConstant 2
  // CHECK:   [[Sub:j[0-9]+]]      Sub [ [[Const5]] [[Const2]] ]
  // CHECK:                        Return [ [[Sub]] ]

  // CHECK-START: long ConstantFolding.LongSubtraction() constant_folding (after)
  // CHECK:   [[Const3:j[0-9]+]]   LongConstant 3
  // CHECK:                        Return [ [[Const3]] ]

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
  // CHECK:   [[Const5:i[0-9]+]]   IntConstant 5
  // CHECK:   [[Const2:i[0-9]+]]   IntConstant 2
  // CHECK:   [[Cond:z[0-9]+]]     GreaterThanOrEqual [ [[Const5]] [[Const2]] ]
  // CHECK:                        If [ [[Cond]] ]

  // CHECK-START: int ConstantFolding.StaticCondition() constant_folding (after)
  // CHECK:   [[Const1:i[0-9]+]]   IntConstant 1
  // CHECK:                        If [ [[Const1]] ]

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
  // CHECK:   [[Const5:i[0-9]+]]   IntConstant 5
  // CHECK:   [[Const2:i[0-9]+]]   IntConstant 2
  // CHECK:   [[Add:i[0-9]+]]      Add [ [[Const5]] [[Const2]] ]
  // CHECK:   [[Phi:i[0-9]+]]      Phi [ [[Add]] [[Sub:i[0-9]+]] ]
  // CHECK:                        Return [ [[Phi]] ]
  // CHECK:   [[Sub]]              Sub [ [[Const5]] [[Const2]] ]

  // CHECK-START: int ConstantFolding.JumpsAndConditionals(boolean) constant_folding (after)
  // CHECK:   [[Const7:i[0-9]+]]   IntConstant 7
  // CHECK:   [[Phi:i[0-9]+]]      Phi [ [[Const7]] [[Const3:i[0-9]+]] ]
  // CHECK:                        Return [ [[Phi]] ]
  // CHECK:   [[Const3]]           IntConstant 3

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
