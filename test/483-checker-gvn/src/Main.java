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

  public static final int staticField;
  public final int instanceField;

  Main() {
    instanceField = 42;
  }

  static {
    staticField = 42;
  }

  // CHECK-START: int Main.addStatic() GVN (before)
  // CHECK-DAG:     StaticFieldGet
  // CHECK-DAG:     StaticFieldGet

  // CHECK-START: int Main.addStatic() GVN (after)
  // CHECK-DAG:     StaticFieldGet
  // CHECK-NOT:     StaticFieldGet
  public static int addStatic() {
    return staticField + doACall() + staticField;
  }

  // CHECK-START: int Main.addInstance() GVN (before)
  // CHECK-DAG:     InstanceFieldGet
  // CHECK-DAG:     InstanceFieldGet

  // CHECK-START: int Main.addInstance() GVN (after)
  // CHECK-DAG:     InstanceFieldGet
  // CHECK-NOT:     InstanceFieldGet
  public int addInstance() {
    return instanceField + doACall() + instanceField;
  }

  public static int doACall() {
    try {
      // Defeat inlining.
      Thread.sleep(0);
    } catch (Throwable t ) {}
    return (int) System.currentTimeMillis();
  }

  public static void main(String[] args) {
  }
}
