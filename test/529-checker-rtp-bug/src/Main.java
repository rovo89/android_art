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

final class Final { }

public class Main {
  // CHECK-START: Final Main.testKeepCheckCast(java.lang.Object, boolean) reference_type_propagation (after)
  // CHECK:    [[Phi:l\d+]]     Phi
  // CHECK:    [[Class:l\d+]]   LoadClass
  // CHECK:                     CheckCast [ [[Phi]] [[Class]] ]
  // CHECK:                     Return [ [[Phi]] ]

  // CHECK-START: Final Main.testKeepCheckCast(java.lang.Object, boolean) instruction_simplifier_after_types (after)
  // CHECK:    [[Phi:l\d+]]     Phi
  // CHECK:    [[Class:l\d+]]   LoadClass
  // CHECK:                     CheckCast
  // CHECK:                     Return [ [[Phi]] ]
  public static Final testKeepCheckCast(Object o, boolean cond) {
    Object x = new Final();
    while (cond) {
      x = o;
      cond = false;
    }
    return (Final) x;
  }

  public static void main(String[] args) {
    try {
      testKeepCheckCast(new Object(), true);
      throw new Error("Expected check cast exception");
    } catch (ClassCastException e) {
      // expected
    }
  }
}
