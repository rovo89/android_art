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

  static int testLiveArgument(int arg) {
    doStaticNativeCallLiveVreg();
    return arg;
  }

  static void moveArgToCalleeSave() {
    try {
      Thread.sleep(0);
    } catch (Exception e) {
      throw new Error(e);
    }
  }

  static void testIntervalHole(int arg, boolean test) {
    // Move the argument to callee save to ensure it is in
    // a readable register.
    moveArgToCalleeSave();
    if (test) {
      staticField1 = arg;
      // The environment use of `arg` should not make it live.
      doStaticNativeCallLiveVreg();
    } else {
      staticField2 = arg;
      // The environment use of `arg` should not make it live.
      doStaticNativeCallLiveVreg();
    }
  }

  static native void doStaticNativeCallLiveVreg();

  static {
    System.loadLibrary("arttest");
  }

  public static void main(String[] args) {
    if (testLiveArgument(42) != 42) {
      throw new Error("Expected 42");
    }

    if (testLiveArgument(42) != 42) {
      throw new Error("Expected 42");
    }

    testIntervalHole(1, true);
    testIntervalHole(1, false);
  }

  static int staticField1;
  static int staticField2;
}
