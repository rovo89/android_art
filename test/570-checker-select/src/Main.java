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

  /// CHECK-START: int Main.BoolCond_IntVarVar(boolean, int, int) register (after)
  /// CHECK:               Select [{{i\d+}},{{i\d+}},{{z\d+}}]

  public static int BoolCond_IntVarVar(boolean cond, int x, int y) {
    return cond ? x : y;
  }

  /// CHECK-START: int Main.BoolCond_IntVarCst(boolean, int) register (after)
  /// CHECK:               Select [{{i\d+}},{{i\d+}},{{z\d+}}]

  public static int BoolCond_IntVarCst(boolean cond, int x) {
    return cond ? x : 1;
  }

  /// CHECK-START: int Main.BoolCond_IntCstVar(boolean, int) register (after)
  /// CHECK:               Select [{{i\d+}},{{i\d+}},{{z\d+}}]

  public static int BoolCond_IntCstVar(boolean cond, int y) {
    return cond ? 1 : y;
  }

  /// CHECK-START: float Main.BoolCond_FloatVarVar(boolean, float, float) register (after)
  /// CHECK:               Select [{{f\d+}},{{f\d+}},{{z\d+}}]

  public static float BoolCond_FloatVarVar(boolean cond, float x, float y) {
    return cond ? x : y;
  }

  /// CHECK-START: float Main.BoolCond_FloatVarCst(boolean, float) register (after)
  /// CHECK:               Select [{{f\d+}},{{f\d+}},{{z\d+}}]

  public static float BoolCond_FloatVarCst(boolean cond, float x) {
    return cond ? x : 1.0f;
  }

  /// CHECK-START: float Main.BoolCond_FloatCstVar(boolean, float) register (after)
  /// CHECK:               Select [{{f\d+}},{{f\d+}},{{z\d+}}]

  public static float BoolCond_FloatCstVar(boolean cond, float y) {
    return cond ? 1.0f : y;
  }

  /// CHECK-START: int Main.IntNonmatCond_IntVarVar(int, int, int, int) register (after)
  /// CHECK:            <<Cond:z\d+>> LessThanOrEqual [{{i\d+}},{{i\d+}}]
  /// CHECK-NEXT:                     Select [{{i\d+}},{{i\d+}},<<Cond>>]

  public static int IntNonmatCond_IntVarVar(int a, int b, int x, int y) {
    return a > b ? x : y;
  }

  /// CHECK-START: int Main.IntMatCond_IntVarVar(int, int, int, int) register (after)
  /// CHECK:            <<Cond:z\d+>> LessThanOrEqual [{{i\d+}},{{i\d+}}]
  /// CHECK-NEXT:       <<Sel:i\d+>>  Select [{{i\d+}},{{i\d+}},{{z\d+}}]
  /// CHECK-NEXT:                     Add [<<Cond>>,<<Sel>>]

  public static int IntMatCond_IntVarVar(int a, int b, int x, int y) {
    int result = (a > b ? x : y);
    return result + (a > b ? 0 : 1);
  }

  /// CHECK-START: int Main.FloatLtNonmatCond_IntVarVar(float, float, int, int) register (after)
  /// CHECK:            <<Cond:z\d+>> LessThanOrEqual [{{f\d+}},{{f\d+}}]
  /// CHECK-NEXT:                     Select [{{i\d+}},{{i\d+}},<<Cond>>]

  public static int FloatLtNonmatCond_IntVarVar(float a, float b, int x, int y) {
    return a > b ? x : y;
  }

  /// CHECK-START: int Main.FloatGtNonmatCond_IntVarVar(float, float, int, int) register (after)
  /// CHECK:            <<Cond:z\d+>> GreaterThanOrEqual [{{f\d+}},{{f\d+}}]
  /// CHECK-NEXT:                     Select [{{i\d+}},{{i\d+}},<<Cond>>]

  public static int FloatGtNonmatCond_IntVarVar(float a, float b, int x, int y) {
    return a < b ? x : y;
  }

  /// CHECK-START: float Main.FloatGtNonmatCond_FloatVarVar(float, float, float, float) register (after)
  /// CHECK:            <<Cond:z\d+>> GreaterThanOrEqual [{{f\d+}},{{f\d+}}]
  /// CHECK-NEXT:                     Select [{{f\d+}},{{f\d+}},<<Cond>>]

  public static float FloatGtNonmatCond_FloatVarVar(float a, float b, float x, float y) {
    return a < b ? x : y;
  }

  /// CHECK-START: int Main.FloatLtMatCond_IntVarVar(float, float, int, int) register (after)
  /// CHECK:            <<Cond:z\d+>> LessThanOrEqual [{{f\d+}},{{f\d+}}]
  /// CHECK-NEXT:       <<Sel:i\d+>>  Select [{{i\d+}},{{i\d+}},<<Cond>>]
  /// CHECK-NEXT:                     Add [<<Cond>>,<<Sel>>]

  public static int FloatLtMatCond_IntVarVar(float a, float b, int x, int y) {
    int result = (a > b ? x : y);
    return result + (a > b ? 0 : 1);
  }

  /// CHECK-START: int Main.FloatGtMatCond_IntVarVar(float, float, int, int) register (after)
  /// CHECK:            <<Cond:z\d+>> GreaterThanOrEqual [{{f\d+}},{{f\d+}}]
  /// CHECK-NEXT:       <<Sel:i\d+>>  Select [{{i\d+}},{{i\d+}},<<Cond>>]
  /// CHECK-NEXT:                     Add [<<Cond>>,<<Sel>>]

  public static int FloatGtMatCond_IntVarVar(float a, float b, int x, int y) {
    int result = (a < b ? x : y);
    return result + (a < b ? 0 : 1);
  }

  /// CHECK-START: float Main.FloatGtMatCond_FloatVarVar(float, float, float, float) register (after)
  /// CHECK:            <<Cond:z\d+>> GreaterThanOrEqual
  /// CHECK-NEXT:       <<Sel:f\d+>>  Select [{{f\d+}},{{f\d+}},<<Cond>>]
  /// CHECK-NEXT:                     TypeConversion [<<Cond>>]

  public static float FloatGtMatCond_FloatVarVar(float a, float b, float x, float y) {
    float result = (a < b ? x : y);
    return result + (a < b ? 0 : 1);
  }

  public static void assertEqual(int expected, int actual) {
    if (expected != actual) {
      throw new Error("Assertion failed: " + expected + " != " + actual);
    }
  }

  public static void assertEqual(float expected, float actual) {
    if (expected != actual) {
      throw new Error("Assertion failed: " + expected + " != " + actual);
    }
  }

  public static void main(String[] args) {
    assertEqual(5, BoolCond_IntVarVar(true, 5, 7));
    assertEqual(7, BoolCond_IntVarVar(false, 5, 7));
    assertEqual(5, BoolCond_IntVarCst(true, 5));
    assertEqual(1, BoolCond_IntVarCst(false, 5));
    assertEqual(1, BoolCond_IntCstVar(true, 7));
    assertEqual(7, BoolCond_IntCstVar(false, 7));

    assertEqual(5, BoolCond_FloatVarVar(true, 5, 7));
    assertEqual(7, BoolCond_FloatVarVar(false, 5, 7));
    assertEqual(5, BoolCond_FloatVarCst(true, 5));
    assertEqual(1, BoolCond_FloatVarCst(false, 5));
    assertEqual(1, BoolCond_FloatCstVar(true, 7));
    assertEqual(7, BoolCond_FloatCstVar(false, 7));

    assertEqual(5, IntNonmatCond_IntVarVar(3, 2, 5, 7));
    assertEqual(7, IntNonmatCond_IntVarVar(2, 3, 5, 7));
    assertEqual(5, IntMatCond_IntVarVar(3, 2, 5, 7));
    assertEqual(8, IntMatCond_IntVarVar(2, 3, 5, 7));

    assertEqual(5, FloatLtNonmatCond_IntVarVar(3, 2, 5, 7));
    assertEqual(7, FloatLtNonmatCond_IntVarVar(2, 3, 5, 7));
    assertEqual(7, FloatLtNonmatCond_IntVarVar(Float.NaN, 2, 5, 7));
    assertEqual(7, FloatLtNonmatCond_IntVarVar(2, Float.NaN, 5, 7));

    assertEqual(5, FloatGtNonmatCond_IntVarVar(2, 3, 5, 7));
    assertEqual(7, FloatGtNonmatCond_IntVarVar(3, 2, 5, 7));
    assertEqual(7, FloatGtNonmatCond_IntVarVar(Float.NaN, 2, 5, 7));
    assertEqual(7, FloatGtNonmatCond_IntVarVar(2, Float.NaN, 5, 7));

    assertEqual(5, FloatGtNonmatCond_FloatVarVar(2, 3, 5, 7));
    assertEqual(7, FloatGtNonmatCond_FloatVarVar(3, 2, 5, 7));
    assertEqual(7, FloatGtNonmatCond_FloatVarVar(Float.NaN, 2, 5, 7));
    assertEqual(7, FloatGtNonmatCond_FloatVarVar(2, Float.NaN, 5, 7));

    assertEqual(5, FloatLtMatCond_IntVarVar(3, 2, 5, 7));
    assertEqual(8, FloatLtMatCond_IntVarVar(2, 3, 5, 7));
    assertEqual(8, FloatLtMatCond_IntVarVar(Float.NaN, 2, 5, 7));
    assertEqual(8, FloatLtMatCond_IntVarVar(2, Float.NaN, 5, 7));

    assertEqual(5, FloatGtMatCond_IntVarVar(2, 3, 5, 7));
    assertEqual(8, FloatGtMatCond_IntVarVar(3, 2, 5, 7));
    assertEqual(8, FloatGtMatCond_IntVarVar(Float.NaN, 2, 5, 7));
    assertEqual(8, FloatGtMatCond_IntVarVar(2, Float.NaN, 5, 7));

    assertEqual(5, FloatGtMatCond_FloatVarVar(2, 3, 5, 7));
    assertEqual(8, FloatGtMatCond_FloatVarVar(3, 2, 5, 7));
    assertEqual(8, FloatGtMatCond_FloatVarVar(Float.NaN, 2, 5, 7));
    assertEqual(8, FloatGtMatCond_FloatVarVar(2, Float.NaN, 5, 7));
  }
}
