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

// Note that $opt$ is a marker for the optimizing compiler to ensure
// it does compile the method.

public class Main {
  public static void main(String[] args) {
    Error error = null;
    try {
      $opt$TestInvokeStatic();
    } catch (Error e) {
      error = e;
    }
    System.out.println(error);

    $opt$TestInvokeNew();

    int result = $opt$TestInvokeIntParameter(42);
    if (result != 42) {
      throw new Error("Different value returned: " + result);
    }


    $opt$TestInvokeObjectParameter(new Object());

    Object a = new Object();
    Object b = $opt$TestInvokeObjectParameter(a);
    if (a != b) {
      throw new Error("Different object returned " + a + " " + b);
    }

    result = $opt$TestInvokeWith2Parameters(10, 9);
    if (result != 1) {
      throw new Error("Unexpected result: " + result);
    }

    result = $opt$TestInvokeWith3Parameters(10, 9, 1);
    if (result != 0) {
      throw new Error("Unexpected result: " + result);
    }

    result = $opt$TestInvokeWith5Parameters(10000, 1000, 100, 10, 1);
    if (result != 8889) {
      throw new Error("Unexpected result: " + result);
    }

    result = $opt$TestInvokeWith7Parameters(100, 6, 5, 4, 3, 2, 1);
    if (result != 79) {
      throw new Error("Unexpected result: " + result);
    }

    Main m = new Main();
    if (m.$opt$TestThisParameter(m) != m) {
      throw new Error("Unexpected value returned");
    }

    if (m.$opt$TestOtherParameter(new Main()) == m) {
      throw new Error("Unexpected value returned");
    }
  }

  static int $opt$TestInvokeIntParameter(int param) {
    return param;
  }

  static Object $opt$TestInvokeObjectParameter(Object a) {
    forceGCStaticMethod();
    return a;
  }

  static int $opt$TestInvokeWith2Parameters(int a, int b) {
    return a - b;
  }

  static int $opt$TestInvokeWith3Parameters(int a, int b, int c) {
    return a - b - c;
  }

  static int $opt$TestInvokeWith5Parameters(int a, int b, int c, int d, int e) {
    return a - b - c - d - e;
  }

  static int $opt$TestInvokeWith7Parameters(int a, int b, int c, int d, int e, int f, int g) {
    return a - b - c - d - e - f - g;
  }

  Object $opt$TestThisParameter(Object other) {
    forceGCStaticMethod();
    return other;
  }

  Object $opt$TestOtherParameter(Object other) {
    forceGCStaticMethod();
    return other;
  }

  public static void $opt$TestInvokeStatic() {
    printStaticMethod();
    printStaticMethodWith2Args(1, 2);
    printStaticMethodWith5Args(1, 2, 3, 4, 5);
    printStaticMethodWith7Args(1, 2, 3, 4, 5, 6, 7);
    forceGCStaticMethod();
    throwStaticMethod();
  }

  public static void $opt$TestInvokeNew() {
    Object o = new Object();
    forceGCStaticMethod();
    printStaticMethodWithObjectArg(o);
    forceGCStaticMethod();
  }

  public static void printStaticMethod() {
    System.out.println("In static method");
  }

  public static void printStaticMethodWith2Args(int a, int b) {
    System.out.println("In static method with 2 args " + a + " " + b);
  }

  public static void printStaticMethodWith5Args(int a, int b, int c, int d, int e) {
    System.out.println("In static method with 5 args "
        + a + " " + b + " " + c + " " + d + " " + e);
  }

  public static void printStaticMethodWith7Args(int a, int b, int c, int d, int e, int f, int g) {
    System.out.println("In static method with 7 args "
        + a + " " + b + " " + c + " " + d + " " + e + " " + f + " " + g);
  }

  public static void printStaticMethodWithObjectArg(Object a) {
    System.out.println("In static method with object arg " + a.getClass());
  }

  public static void forceGCStaticMethod() {
    Runtime.getRuntime().gc();
    Runtime.getRuntime().gc();
    Runtime.getRuntime().gc();
    Runtime.getRuntime().gc();
    Runtime.getRuntime().gc();
    Runtime.getRuntime().gc();
    System.out.println("Forced GC");
  }

  public static void throwStaticMethod() {
    throw new Error("Error");
  }
}
