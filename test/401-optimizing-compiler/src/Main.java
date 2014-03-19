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
  }

  public static void $opt$TestInvokeStatic() {
    printStaticMethod();
    forceGCStaticMethod();
    throwStaticMethod();
  }

  public static void printStaticMethod() {
    System.out.println("In static method");
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
