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

  private static int[] a = { 10 };

  // A very particular set of operations that caused a double removal by the
  // ARM64 simplifier doing "forward" removals (b/27851582).

  /// CHECK-START-ARM64: int Main.operations() instruction_simplifier_arm64 (before)
  /// CHECK-DAG: <<Get:i\d+>> ArrayGet
  /// CHECK-DAG: <<Not:i\d+>> Not [<<Get>>]
  /// CHECK-DAG: <<Shl:i\d+>> Shl [<<Get>>,i{{\d+}}]
  /// CHECK-DAG:              And [<<Not>>,<<Shl>>]
  //
  /// CHECK-START-ARM64: int Main.operations() instruction_simplifier_arm64 (after)
  /// CHECK-DAG: <<Get:i\d+>> ArrayGet
  /// CHECK-DAG: <<Not:i\d+>> Not [<<Get>>]
  /// CHECK-DAG:              Arm64DataProcWithShifterOp [<<Not>>,<<Get>>] kind:And+LSL shift:2
  private static int operations() {
     int r = a[0];
     int n = ~r;
     int s = r << 2;
     int a = s & n;
     return a;
  }

  public static void main(String[] args) {
    if (operations() != 32) {
      System.out.println("failed");
    } else {
      System.out.println("passed");
    }
  }
}
