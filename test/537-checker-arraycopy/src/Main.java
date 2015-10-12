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
  public static void main(String[] args) {
    arraycopy();
    try {
      arraycopy(new Object());
      throw new Error("Should not be here");
    } catch (ArrayStoreException ase) {
      // Ignore.
    }
    try {
      arraycopy(null);
      throw new Error("Should not be here");
    } catch (NullPointerException npe) {
      // Ignore.
    }

    try {
      arraycopy(new Object[1]);
      throw new Error("Should not be here");
    } catch (ArrayIndexOutOfBoundsException aiooe) {
      // Ignore.
    }

    arraycopy(new Object[2]);
    arraycopy(new Object[2], 0);

    try {
      arraycopy(new Object[1], 1);
      throw new Error("Should not be here");
    } catch (ArrayIndexOutOfBoundsException aiooe) {
      // Ignore.
    }
  }

  /// CHECK-START-X86_64: void Main.arraycopy() disassembly (after)
  /// CHECK:          InvokeStaticOrDirect
  /// CHECK-NOT:      test
  /// CHECK-NOT:      call
  /// CHECK:          ReturnVoid
  // Checks that the call is intrinsified and that there is no test instruction
  // when we know the source and destination are not null.
  public static void arraycopy() {
    Object[] obj = new Object[4];
    System.arraycopy(obj, 1, obj, 0, 1);
  }

  public static void arraycopy(Object obj) {
    System.arraycopy(obj, 1, obj, 0, 1);
  }

  public static void arraycopy(Object[] obj, int pos) {
    System.arraycopy(obj, pos, obj, 0, obj.length);
  }
}
