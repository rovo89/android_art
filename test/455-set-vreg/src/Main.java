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
  public Main() {
  }

  int testIntVReg(int a, int b, int c, int d, int e) {
    doNativeCallSetVReg();
    return a - b - c - d - e;
  }

  long testLongVReg(long a, long b, long c, long d, long e) {
    doNativeCallSetVReg();
    return a - b - c - d - e;
  }

  float testFloatVReg(float a, float b, float c, float d, float e) {
    doNativeCallSetVReg();
    return a - b - c - d - e;
  }

  double testDoubleVReg(double a, double b, double c, double d, double e) {
    doNativeCallSetVReg();
    return a - b - c - d - e;
  }

  native void doNativeCallSetVReg();

  static {
    System.loadLibrary("arttest");
  }

  public static void main(String[] args) {
    Main rm = new Main();
    int intExpected = 5 - 4 - 3 - 2 - 1;
    int intResult = rm.testIntVReg(0, 0, 0, 0, 0);
    if (intResult != intExpected) {
      throw new Error("Expected " + intExpected + ", got " + intResult);
    }

    long longExpected = Long.MAX_VALUE - 4 - 3 - 2 - 1;
    long longResult = rm.testLongVReg(0, 0, 0, 0, 0);
    if (longResult != longExpected) {
      throw new Error("Expected " + longExpected + ", got " + longResult);
    }

    float floatExpected = 5.0f - 4.0f - 3.0f - 2.0f - 1.0f;
    float floatResult = rm.testFloatVReg(0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    if (floatResult != floatExpected) {
      throw new Error("Expected " + floatExpected + ", got " + floatResult);
    }

    double doubleExpected = 5.0 - 4.0 - 3.0 - 2.0 - 1.0;
    double doubleResult = rm.testDoubleVReg(0.0, 0.0, 0.0, 0.0, 0.0);
    if (doubleResult != doubleExpected) {
      throw new Error("Expected " + doubleExpected + ", got " + doubleResult);
    }
  }
}
