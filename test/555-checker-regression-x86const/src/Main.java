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

public class Main extends UnresolvedClass {

  /// CHECK-START: float Main.callAbs(float) register (before)
  /// CHECK:       <<CurrentMethod:[ij]\d+>> CurrentMethod
  /// CHECK:       <<ParamValue:f\d+>> ParameterValue
  /// CHECK:       InvokeStaticOrDirect [<<ParamValue>>,<<CurrentMethod>>] method_name:java.lang.Math.abs
  static public float callAbs(float f) {
    // An intrinsic invoke in a method that has unresolved references will still
    // have a CurrentMethod as an argument.  The X86 pc_relative_fixups_x86 pass
    // must be able to handle Math.abs invokes that have a CurrentMethod, as both
    // the CurrentMethod and the HX86LoadFromConstantTable (for the bitmask)
    // expect to be in the 'SpecialInputIndex' input index.
    return Math.abs(f);
  }

  static public void main(String[] args) {
    expectEquals(callAbs(-6.5f), 6.5f);
  }

  public static void expectEquals(float expected, float result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }
}
