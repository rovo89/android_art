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

  // CHECK-START: int Main.div() licm (before)
  // CHECK-DAG: Div ( loop_header:{{B\d+}} )

  // CHECK-START: int Main.div() licm (after)
  // CHECK-NOT: Div ( loop_header:{{B\d+}} )

  // CHECK-START: int Main.div() licm (after)
  // CHECK-DAG: Div ( loop_header:null )

  public static int div() {
    int result = 0;
    for (int i = 0; i < 10; ++i) {
      result += staticField / 42;
    }
    return result;
  }

  // CHECK-START: int Main.innerDiv() licm (before)
  // CHECK-DAG: Div ( loop_header:{{B\d+}} )

  // CHECK-START: int Main.innerDiv() licm (after)
  // CHECK-NOT: Div ( loop_header:{{B\d+}} )

  // CHECK-START: int Main.innerDiv() licm (after)
  // CHECK-DAG: Div ( loop_header:null )

  public static int innerDiv() {
    int result = 0;
    for (int i = 0; i < 10; ++i) {
      for (int j = 0; j < 10; ++j) {
        result += staticField / 42;
      }
    }
    return result;
  }

  // CHECK-START: int Main.innerDiv2() licm (before)
  // CHECK-DAG: Mul ( loop_header:{{B4}} )

  // CHECK-START: int Main.innerDiv2() licm (after)
  // CHECK-DAG: Mul ( loop_header:{{B2}} )

  public static int innerDiv2() {
    int result = 0;
    for (int i = 0; i < 10; ++i) {
      for (int j = 0; j < 10; ++j) {
        // The operation has been hoisted out of the inner loop.
        // Note that we depend on the compiler's block numbering to
        // check if it has been moved.
        result += staticField * i;
      }
    }
    return result;
  }

  // CHECK-START: int Main.innerDiv3(int, int) licm (before)
  // CHECK-DAG: Div ( loop_header:{{B\d+}} )

  // CHECK-START: int Main.innerDiv3(int, int) licm (after)
  // CHECK-DAG: Div ( loop_header:{{B\d+}} )

  public static int innerDiv3(int a, int b) {
    int result = 0;
    while (b < 5) {
      // a might be null, so we can't hoist the operation.
      result += staticField / a;
      b++;
    }
    return result;
  }

  // CHECK-START: int Main.arrayLength(int[]) licm (before)
  // CHECK-DAG: [[NullCheck:l\d+]] NullCheck ( loop_header:{{B\d+}} )
  // CHECK-DAG:                    ArrayLength [ [[NullCheck]] ] ( loop_header:{{B\d+}} )

  // CHECK-START: int Main.arrayLength(int[]) licm (after)
  // CHECK-NOT:                    NullCheck ( loop_header:{{B\d+}} )
  // CHECK-NOT:                    ArrayLength ( loop_header:{{B\d+}} )

  // CHECK-START: int Main.arrayLength(int[]) licm (after)
  // CHECK-DAG: [[NullCheck:l\d+]] NullCheck ( loop_header:null )
  // CHECK-DAG:                    ArrayLength [ [[NullCheck]] ] ( loop_header:null )

  public static int arrayLength(int[] array) {
    int result = 0;
    for (int i = 0; i < array.length; ++i) {
      result += array[i];
    }
    return result;
  }

  public static int staticField = 42;

  public static void assertEquals(int expected, int actual) {
    if (expected != actual) {
      throw new Error("Expected " + expected + ", got " + actual);
    }
  }

  public static void main(String[] args) {
    assertEquals(10, div());
    assertEquals(100, innerDiv());
    assertEquals(12, arrayLength(new int[] { 4, 8 }));
  }
}
