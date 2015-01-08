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

public class Inliner {

  // CHECK-START: void Inliner.InlineVoid() inliner (before)
  // CHECK-DAG:     [[Const42:i\d+]] IntConstant 42
  // CHECK-DAG:                      InvokeStaticOrDirect
  // CHECK-DAG:                      InvokeStaticOrDirect [ [[Const42]] ]

  // CHECK-START: void Inliner.InlineVoid() inliner (after)
  // CHECK-NOT:                      InvokeStaticOrDirect

  public static void InlineVoid() {
    returnVoid();
    returnVoidWithOneParameter(42);
  }

  // CHECK-START: int Inliner.InlineParameter(int) inliner (before)
  // CHECK-DAG:     [[Param:i\d+]]  ParameterValue
  // CHECK-DAG:     [[Result:i\d+]] InvokeStaticOrDirect [ [[Param]] ]
  // CHECK-DAG:                     Return [ [[Result]] ]

  // CHECK-START: int Inliner.InlineParameter(int) inliner (after)
  // CHECK-DAG:     [[Param:i\d+]]  ParameterValue
  // CHECK-DAG:                     Return [ [[Param]] ]

  public static int InlineParameter(int a) {
    return returnParameter(a);
  }

  // CHECK-START: long Inliner.InlineWideParameter(long) inliner (before)
  // CHECK-DAG:     [[Param:j\d+]]  ParameterValue
  // CHECK-DAG:     [[Result:j\d+]] InvokeStaticOrDirect [ [[Param]] ]
  // CHECK-DAG:                     Return [ [[Result]] ]

  // CHECK-START: long Inliner.InlineWideParameter(long) inliner (after)
  // CHECK-DAG:     [[Param:j\d+]]  ParameterValue
  // CHECK-DAG:                     Return [ [[Param]] ]

  public static long InlineWideParameter(long a) {
    return returnWideParameter(a);
  }

  // CHECK-START: java.lang.Object Inliner.InlineReferenceParameter(java.lang.Object) inliner (before)
  // CHECK-DAG:     [[Param:l\d+]]  ParameterValue
  // CHECK-DAG:     [[Result:l\d+]] InvokeStaticOrDirect [ [[Param]] ]
  // CHECK-DAG:                     Return [ [[Result]] ]

  // CHECK-START: java.lang.Object Inliner.InlineReferenceParameter(java.lang.Object) inliner (after)
  // CHECK-DAG:     [[Param:l\d+]]  ParameterValue
  // CHECK-DAG:                     Return [ [[Param]] ]

  public static Object InlineReferenceParameter(Object o) {
    return returnReferenceParameter(o);
  }

  // CHECK-START: int Inliner.InlineInt() inliner (before)
  // CHECK-DAG:     [[Result:i\d+]] InvokeStaticOrDirect
  // CHECK-DAG:                     Return [ [[Result]] ]

  // CHECK-START: int Inliner.InlineInt() inliner (after)
  // CHECK-DAG:     [[Const4:i\d+]] IntConstant 4
  // CHECK-DAG:                     Return [ [[Const4]] ]

  public static int InlineInt() {
    return returnInt();
  }

  // CHECK-START: long Inliner.InlineWide() inliner (before)
  // CHECK-DAG:     [[Result:j\d+]] InvokeStaticOrDirect
  // CHECK-DAG:                     Return [ [[Result]] ]

  // CHECK-START: long Inliner.InlineWide() inliner (after)
  // CHECK-DAG:     [[Const8:j\d+]] LongConstant 8
  // CHECK-DAG:                     Return [ [[Const8]] ]

  public static long InlineWide() {
    return returnWide();
  }

  // CHECK-START: int Inliner.InlineAdd() inliner (before)
  // CHECK-DAG:     [[Const3:i\d+]] IntConstant 3
  // CHECK-DAG:     [[Const5:i\d+]] IntConstant 5
  // CHECK-DAG:     [[Result:i\d+]] InvokeStaticOrDirect
  // CHECK-DAG:                     Return [ [[Result]] ]

  // CHECK-START: int Inliner.InlineAdd() inliner (after)
  // CHECK-DAG:     [[Const3:i\d+]] IntConstant 3
  // CHECK-DAG:     [[Const5:i\d+]] IntConstant 5
  // CHECK-DAG:     [[Add:i\d+]]    Add [ [[Const3]] [[Const5]] ]
  // CHECK-DAG:                     Return [ [[Add]] ]

  public static int InlineAdd() {
    return returnAdd(3, 5);
  }

  // CHECK-START: int Inliner.InlineFieldAccess() inliner (before)
  // CHECK-DAG:     [[After:i\d+]]  InvokeStaticOrDirect
  // CHECK-DAG:                     Return [ [[After]] ]

  // CHECK-START: int Inliner.InlineFieldAccess() inliner (after)
  // CHECK-DAG:     [[Const1:i\d+]] IntConstant 1
  // CHECK-DAG:     [[Before:i\d+]] StaticFieldGet
  // CHECK-DAG:     [[After:i\d+]]  Add [ [[Before]] [[Const1]] ]
  // CHECK-DAG:                     StaticFieldSet [ {{l\d+}} [[After]] ]
  // CHECK-DAG:                     Return [ [[After]] ]

  // CHECK-START: int Inliner.InlineFieldAccess() inliner (after)
  // CHECK-NOT:                     InvokeStaticOrDirect

  public static int InlineFieldAccess() {
    return incCounter();
  }

  // CHECK-START: int Inliner.InlineWithControlFlow(boolean) inliner (before)
  // CHECK-DAG:     [[Const1:i\d+]] IntConstant 1
  // CHECK-DAG:     [[Const3:i\d+]] IntConstant 3
  // CHECK-DAG:     [[Const5:i\d+]] IntConstant 5
  // CHECK-DAG:     [[Add:i\d+]]    InvokeStaticOrDirect [ [[Const1]] [[Const3]] ]
  // CHECK-DAG:     [[Sub:i\d+]]    InvokeStaticOrDirect [ [[Const5]] [[Const3]] ]
  // CHECK-DAG:     [[Phi:i\d+]]    Phi [ [[Add]] [[Sub]] ]
  // CHECK-DAG:                     Return [ [[Phi]] ]

  // CHECK-START: int Inliner.InlineWithControlFlow(boolean) inliner (after)
  // CHECK-DAG:     [[Const1:i\d+]] IntConstant 1
  // CHECK-DAG:     [[Const3:i\d+]] IntConstant 3
  // CHECK-DAG:     [[Const5:i\d+]] IntConstant 5
  // CHECK-DAG:     [[Add:i\d+]]    Add [ [[Const1]] [[Const3]] ]
  // CHECK-DAG:     [[Sub:i\d+]]    Sub [ [[Const5]] [[Const3]] ]
  // CHECK-DAG:     [[Phi:i\d+]]    Phi [ [[Add]] [[Sub]] ]
  // CHECK-DAG:                     Return [ [[Phi]] ]

  public static int InlineWithControlFlow(boolean cond) {
    int x, const1, const3, const5;
    const1 = 1;
    const3 = 3;
    const5 = 5;
    if (cond) {
      x = returnAdd(const1, const3);
    } else {
      x = returnSub(const5, const3);
    }
    return x;
  }


  private static void returnVoid() {
    return;
  }

  private static void returnVoidWithOneParameter(int a) {
    return;
  }

  private static int returnParameter(int a) {
    return a;
  }

  private static long returnWideParameter(long a) {
    return a;
  }

  private static Object returnReferenceParameter(Object o) {
    return o;
  }

  private static int returnInt() {
    return 4;
  }

  private static long returnWide() {
    return 8L;
  }

  private static int returnAdd(int a, int b) {
    return a + b;
  }

  private static int returnSub(int a, int b) {
    return a - b;
  }

  private static int counter = 42;

  private static int incCounter() {
    return ++counter;
  }
}
