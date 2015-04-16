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

  public static void assertBoolEquals(boolean expected, boolean result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  /*
   * Test that zero/one constants are accepted as boolean inputs.
   */

  // CHECK-START: boolean Main.TestIntAsBoolean() inliner (before)
  // CHECK-DAG:     [[Invoke:z\d+]]  InvokeStaticOrDirect
  // CHECK-DAG:                      BooleanNot [ [[Invoke]] ]

  // CHECK-START: boolean Main.TestIntAsBoolean() inliner (after)
  // CHECK-DAG:     [[Const:i\d+]]   IntConstant 1
  // CHECK-DAG:                      BooleanNot [ [[Const]] ]

  public static boolean InlineConst() {
    return true;
  }

  public static boolean TestIntAsBoolean() {
    return InlineConst() != true ? true : false;
  }

  /*
   * Test that integer Phis are accepted as boolean inputs until we implement
   * a suitable type analysis.
   */

  // CHECK-START: boolean Main.TestPhiAsBoolean(int) inliner (before)
  // CHECK-DAG:     [[Invoke:z\d+]]  InvokeStaticOrDirect
  // CHECK-DAG:                      BooleanNot [ [[Invoke]] ]

  // CHECK-START: boolean Main.TestPhiAsBoolean(int) inliner (after)
  // CHECK-DAG:     [[Phi:i\d+]]     Phi
  // CHECK-DAG:                      BooleanNot [ [[Phi]] ]

  public static boolean f1;
  public static boolean f2;

  public static boolean InlinePhi(int x) {
    return (x == 42) ? f1 : f2;
  }

  public static boolean TestPhiAsBoolean(int x) {
    return InlinePhi(x) != true ? true : false;
  }

  public static void main(String[] args) {
    f1 = true;
    f2 = false;
    assertBoolEquals(true, TestPhiAsBoolean(0));
    assertBoolEquals(false, TestPhiAsBoolean(42));
  }
}
