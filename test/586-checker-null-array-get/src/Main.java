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
  public static Object[] getObjectArray() { return null; }
  public static long[] getLongArray() { return null; }

  public static void main(String[] args) {
    try {
      foo();
      throw new Error("Expected NullPointerException");
    } catch (NullPointerException e) {
      // Expected.
    }
  }

  /// CHECK-START: void Main.foo() load_store_elimination (after)
  /// CHECK-DAG: <<Null:l\d+>>  NullConstant
  /// CHECK-DAG: <<Check:l\d+>> NullCheck [<<Null>>]
  /// CHECK-DAG: <<Get1:j\d+>>  ArrayGet [<<Check>>,{{i\d+}}]
  /// CHECK-DAG: <<Get2:l\d+>>  ArrayGet [<<Check>>,{{i\d+}}]
  public static void foo() {
    longField = getLongArray()[0];
    objectField = getObjectArray()[0];
  }

  public static long longField;
  public static Object objectField;
}
