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

import java.lang.reflect.Field;

import sun.misc.Unsafe;

/**
 * Checker test on the 1.8 unsafe operations. Note, this is by no means an
 * exhaustive unit test for these CAS (compare-and-swap) and fence operations.
 * Instead, this test ensures the methods are recognized as intrinsic and behave
 * as expected.
 */
public class Main {

  private static final Unsafe unsafe = getUnsafe();

  private static Thread[] sThreads = new Thread[10];

  //
  // Fields accessed by setters and adders.
  //

  public int i = 0;
  public long l = 0;
  public Object o = null;

  //
  // Setters.
  //

  /// CHECK-START: int Main.set32(java.lang.Object, long, int) intrinsics_recognition (after)
  /// CHECK-DAG: <<Result:i\d+>> InvokeVirtual intrinsic:UnsafeGetAndSetInt
  /// CHECK-DAG:                 Return [<<Result>>]
  private static int set32(Object o, long offset, int newValue) {
    return unsafe.getAndSetInt(o, offset, newValue);
  }

  /// CHECK-START: long Main.set64(java.lang.Object, long, long) intrinsics_recognition (after)
  /// CHECK-DAG: <<Result:j\d+>> InvokeVirtual intrinsic:UnsafeGetAndSetLong
  /// CHECK-DAG:                 Return [<<Result>>]
  private static long set64(Object o, long offset, long newValue) {
    return unsafe.getAndSetLong(o, offset, newValue);
  }

  /// CHECK-START: java.lang.Object Main.setObj(java.lang.Object, long, java.lang.Object) intrinsics_recognition (after)
  /// CHECK-DAG: <<Result:l\d+>> InvokeVirtual intrinsic:UnsafeGetAndSetObject
  /// CHECK-DAG:                 Return [<<Result>>]
  private static Object setObj(Object o, long offset, Object newValue) {
    return unsafe.getAndSetObject(o, offset, newValue);
  }

  //
  // Adders.
  //

  /// CHECK-START: int Main.add32(java.lang.Object, long, int) intrinsics_recognition (after)
  /// CHECK-DAG: <<Result:i\d+>> InvokeVirtual intrinsic:UnsafeGetAndAddInt
  /// CHECK-DAG:                 Return [<<Result>>]
  private static int add32(Object o, long offset, int delta) {
    return unsafe.getAndAddInt(o, offset, delta);
  }

  /// CHECK-START: long Main.add64(java.lang.Object, long, long) intrinsics_recognition (after)
  /// CHECK-DAG: <<Result:j\d+>> InvokeVirtual intrinsic:UnsafeGetAndAddLong
  /// CHECK-DAG:                 Return [<<Result>>]
  private static long add64(Object o, long offset, long delta) {
    return unsafe.getAndAddLong(o, offset, delta);
  }

  //
  // Fences (native).
  //

  /// CHECK-START: void Main.load() intrinsics_recognition (after)
  /// CHECK-DAG: InvokeVirtual intrinsic:UnsafeLoadFence
  private static void load() {
    unsafe.loadFence();
  }

  /// CHECK-START: void Main.store() intrinsics_recognition (after)
  /// CHECK-DAG: InvokeVirtual intrinsic:UnsafeStoreFence
  private static void store() {
    unsafe.storeFence();
  }

  /// CHECK-START: void Main.full() intrinsics_recognition (after)
  /// CHECK-DAG: InvokeVirtual intrinsic:UnsafeFullFence
  private static void full() {
    unsafe.fullFence();
  }

  //
  // Thread fork/join.
  //

  private static void fork(Runnable r) {
    for (int i = 0; i < 10; i++) {
      sThreads[i] = new Thread(r);
      sThreads[i].start();
    }
  }

  private static void join() {
    try {
      for (int i = 0; i < 10; i++) {
        sThreads[i].join();
      }
    } catch (InterruptedException e) {
      throw new Error("Failed join: " + e);
    }
  }

  //
  // Driver.
  //

  public static void main(String[] args) {
    System.out.println("starting");

    final Main m = new Main();

    // Get the offsets.

    final long intOffset, longOffset, objOffset;
    try {
      Field intField = Main.class.getDeclaredField("i");
      Field longField = Main.class.getDeclaredField("l");
      Field objField = Main.class.getDeclaredField("o");

      intOffset = unsafe.objectFieldOffset(intField);
      longOffset = unsafe.objectFieldOffset(longField);
      objOffset = unsafe.objectFieldOffset(objField);

    } catch (NoSuchFieldException e) {
      throw new Error("No offset: " + e);
    }

    // Some sanity within same thread.

    set32(m, intOffset, 3);
    expectEquals32(3, m.i);

    set64(m, longOffset, 7L);
    expectEquals64(7L, m.l);

    setObj(m, objOffset, m);
    expectEqualsObj(m, m.o);

    add32(m, intOffset, 11);
    expectEquals32(14, m.i);

    add64(m, longOffset, 13L);
    expectEquals64(20L, m.l);

    // Some sanity on setters within different threads.

    fork(new Runnable() {
      public void run() {
        for (int i = 0; i < 10; i++)
          set32(m, intOffset, i);
      }
    });
    join();
    expectEquals32(9, m.i);  // one thread's last value wins

    fork(new Runnable() {
      public void run() {
        for (int i = 0; i < 10; i++)
          set64(m, longOffset, (long) (100 + i));
      }
    });
    join();
    expectEquals64(109L, m.l);  // one thread's last value wins

    fork(new Runnable() {
      public void run() {
        for (int i = 0; i < 10; i++)
          setObj(m, objOffset, sThreads[i]);
      }
    });
    join();
    expectEqualsObj(sThreads[9], m.o);  // one thread's last value wins

    // Some sanity on adders within different threads.

    fork(new Runnable() {
      public void run() {
        for (int i = 0; i < 10; i++)
          add32(m, intOffset, i + 1);
      }
    });
    join();
    expectEquals32(559, m.i);  // all values accounted for

    fork(new Runnable() {
      public void run() {
        for (int i = 0; i < 10; i++)
          add64(m, longOffset, (long) (i + 1));
      }
    });
    join();
    expectEquals64(659L, m.l);  // all values accounted for

    // TODO: the fences

    System.out.println("passed");
  }

  // Use reflection to implement "Unsafe.getUnsafe()";
  private static Unsafe getUnsafe() {
    try {
      Class<?> unsafeClass = Unsafe.class;
      Field f = unsafeClass.getDeclaredField("theUnsafe");
      f.setAccessible(true);
      return (Unsafe) f.get(null);
    } catch (Exception e) {
      throw new Error("Cannot get Unsafe instance");
    }
  }

  private static void expectEquals32(int expected, int result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  private static void expectEquals64(long expected, long result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  private static void expectEqualsObj(Object expected, Object result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }
}
