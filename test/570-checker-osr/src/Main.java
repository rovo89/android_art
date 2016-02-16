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
  public static void main(String[] args) {
    new SubMain();
    System.loadLibrary(args[0]);
    if ($noinline$returnInt() != 53) {
      throw new Error("Unexpected return value");
    }
    if ($noinline$returnFloat() != 42.2f) {
      throw new Error("Unexpected return value");
    }
    if ($noinline$returnDouble() != Double.longBitsToDouble(0xF000000000001111L)) {
      throw new Error("Unexpected return value ");
    }
    if ($noinline$returnLong() != 0xFFFF000000001111L) {
      throw new Error("Unexpected return value");
    }

    try {
      $noinline$deopt();
    } catch (Exception e) {}
    DeoptimizationController.stopDeoptimization();

    $noinline$inlineCache(new Main(), /* isSecondInvocation */ false);
    if ($noinline$inlineCache(new SubMain(), /* isSecondInvocation */ true) != SubMain.class) {
      throw new Error("Unexpected return value");
    }
  }

  public static int $noinline$returnInt() {
    if (doThrow) throw new Error("");
    int i = 0;
    for (; i < 100000; ++i) {
    }
    while (!ensureInOsrCode()) {}
    System.out.println(i);
    return 53;
  }

  public static float $noinline$returnFloat() {
    if (doThrow) throw new Error("");
    int i = 0;
    for (; i < 200000; ++i) {
    }
    while (!ensureInOsrCode()) {}
    System.out.println(i);
    return 42.2f;
  }

  public static double $noinline$returnDouble() {
    if (doThrow) throw new Error("");
    int i = 0;
    for (; i < 300000; ++i) {
    }
    while (!ensureInOsrCode()) {}
    System.out.println(i);
    return Double.longBitsToDouble(0xF000000000001111L);
  }

  public static long $noinline$returnLong() {
    if (doThrow) throw new Error("");
    int i = 0;
    for (; i < 400000; ++i) {
    }
    while (!ensureInOsrCode()) {}
    System.out.println(i);
    return 0xFFFF000000001111L;
  }

  public static void $noinline$deopt() {
    if (doThrow) throw new Error("");
    int i = 0;
    for (; i < 100000; ++i) {
    }
    while (!ensureInOsrCode()) {}
    DeoptimizationController.startDeoptimization();
  }

  public static Class $noinline$inlineCache(Main m, boolean isSecondInvocation) {
    // If we are running in non-JIT mode, or were unlucky enough to get this method
    // already JITted, just return the expected value.
    if (!ensureInInterpreter()) {
      return SubMain.class;
    }

    ensureHasProfilingInfo();

    // Ensure that we have OSR code to jump to.
    if (isSecondInvocation) {
      ensureHasOsrCode();
    }

    // This call will be optimized in the OSR compiled code
    // to check and deoptimize if m is not of type 'Main'.
    Main other = m.inlineCache();

    // Jump to OSR compiled code. The second run
    // of this method will have 'm' as a SubMain, and the compiled
    // code we are jumping to will have wrongly optimize other as being a
    // 'Main'.
    if (isSecondInvocation) {
      while (!ensureInOsrCode()) {}
    }

    // We used to wrongly optimize this call and assume 'other' was a 'Main'.
    return other.returnClass();
  }

  public Main inlineCache() {
    return new Main();
  }

  public Class returnClass() {
    return Main.class;
  }

  public static int[] array = new int[4];

  public static native boolean ensureInInterpreter();
  public static native boolean ensureInOsrCode();
  public static native void ensureHasProfilingInfo();
  public static native void ensureHasOsrCode();

  public static boolean doThrow = false;
}

class SubMain extends Main {
  public Class returnClass() {
    return SubMain.class;
  }

  public Main inlineCache() {
    return new SubMain();
  }
}
