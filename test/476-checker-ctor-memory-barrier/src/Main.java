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

// TODO: Add more tests after we can inline functions with calls.

class ClassWithoutFinals {
  /// CHECK-START: void ClassWithoutFinals.<init>() register (after)
  /// CHECK-NOT: MemoryBarrier kind:StoreStore
  public ClassWithoutFinals() {}
}

class ClassWithFinals {
  public final int x;
  public ClassWithFinals obj;
  public static boolean doThrow = false;

  /// CHECK-START: void ClassWithFinals.<init>(boolean) register (after)
  /// CHECK:      MemoryBarrier kind:StoreStore
  /// CHECK-NEXT: ReturnVoid
  public ClassWithFinals(boolean cond) {
    x = 0;
    if (doThrow) {
      // avoid inlining
      throw new RuntimeException();
    }
  }

  /// CHECK-START: void ClassWithFinals.<init>() register (after)
  /// CHECK:      MemoryBarrier kind:StoreStore
  /// CHECK-NEXT: ReturnVoid
  public ClassWithFinals() {
    x = 0;
  }

  /// CHECK-START: void ClassWithFinals.<init>(int) register (after)
  /// CHECK:      MemoryBarrier kind:StoreStore
  /// CHECK:      MemoryBarrier kind:StoreStore
  /// CHECK-NEXT: ReturnVoid
  public ClassWithFinals(int x) {
    // This should have two barriers:
    //   - one for the constructor
    //   - one for the `new` which should be inlined.
    obj = new ClassWithFinals();
    this.x = x;
  }
}

class InheritFromClassWithFinals extends ClassWithFinals {
  /// CHECK-START: void InheritFromClassWithFinals.<init>() register (after)
  /// CHECK:      MemoryBarrier kind:StoreStore
  /// CHECK-NEXT: ReturnVoid

  /// CHECK-START: void InheritFromClassWithFinals.<init>() register (after)
  /// CHECK-NOT:  InvokeStaticOrDirect
  public InheritFromClassWithFinals() {
    // Should inline the super constructor.
  }

  /// CHECK-START: void InheritFromClassWithFinals.<init>(boolean) register (after)
  /// CHECK:      InvokeStaticOrDirect

  /// CHECK-START: void InheritFromClassWithFinals.<init>(boolean) register (after)
  /// CHECK-NOT:  MemoryBarrier kind:StoreStore
  public InheritFromClassWithFinals(boolean cond) {
    super(cond);
    // should not inline the super constructor
  }

  /// CHECK-START: void InheritFromClassWithFinals.<init>(int) register (after)
  /// CHECK:      MemoryBarrier kind:StoreStore
  /// CHECK:      MemoryBarrier kind:StoreStore
  /// CHECK:      ReturnVoid

  /// CHECK-START: void InheritFromClassWithFinals.<init>(int) register (after)
  /// CHECK-NOT:  InvokeStaticOrDirect
  public InheritFromClassWithFinals(int unused) {
    // Should inline the super constructor and insert a memory barrier.

    // Should inline the new instance call and insert another memory barrier.
    new InheritFromClassWithFinals();
  }
}

class HaveFinalsAndInheritFromClassWithFinals extends ClassWithFinals {
  final int y;

  /// CHECK-START: void HaveFinalsAndInheritFromClassWithFinals.<init>() register (after)
  /// CHECK:      MemoryBarrier kind:StoreStore
  /// CHECK-NEXT: ReturnVoid

  /// CHECK-START: void HaveFinalsAndInheritFromClassWithFinals.<init>() register (after)
  /// CHECK-NOT: InvokeStaticOrDirect
  public HaveFinalsAndInheritFromClassWithFinals() {
    // Should inline the super constructor and remove the memory barrier.
    y = 0;
  }

  /// CHECK-START: void HaveFinalsAndInheritFromClassWithFinals.<init>(boolean) register (after)
  /// CHECK:      InvokeStaticOrDirect
  /// CHECK:      MemoryBarrier kind:StoreStore
  /// CHECK-NEXT: ReturnVoid
  public HaveFinalsAndInheritFromClassWithFinals(boolean cond) {
    super(cond);
    // should not inline the super constructor
    y = 0;
  }

  /// CHECK-START: void HaveFinalsAndInheritFromClassWithFinals.<init>(int) register (after)
  /// CHECK:      MemoryBarrier kind:StoreStore
  /// CHECK:      MemoryBarrier kind:StoreStore
  /// CHECK:      MemoryBarrier kind:StoreStore
  /// CHECK-NEXT: ReturnVoid

  /// CHECK-START: void HaveFinalsAndInheritFromClassWithFinals.<init>(int) register (after)
  /// CHECK-NOT:  InvokeStaticOrDirect
  public HaveFinalsAndInheritFromClassWithFinals(int unused) {
    // Should inline the super constructor and keep just one memory barrier.
    y = 0;

    // Should inline new instance and keep one barrier.
    new HaveFinalsAndInheritFromClassWithFinals();
    // Should inline new instance and keep one barrier.
    new InheritFromClassWithFinals();
  }
}

public class Main {

  /// CHECK-START: ClassWithFinals Main.noInlineNoConstructorBarrier() register (after)
  /// CHECK:      InvokeStaticOrDirect

  /// CHECK-START: ClassWithFinals Main.noInlineNoConstructorBarrier() register (after)
  /// CHECK-NOT:  MemoryBarrier kind:StoreStore
  public static ClassWithFinals noInlineNoConstructorBarrier() {
    return new ClassWithFinals(false);
  }

  /// CHECK-START: void Main.inlineNew() register (after)
  /// CHECK:      MemoryBarrier kind:StoreStore
  /// CHECK-NEXT: ReturnVoid

  /// CHECK-START: void Main.inlineNew() register (after)
  /// CHECK-NOT:  InvokeStaticOrDirect
  public static void inlineNew() {
    new ClassWithFinals();
  }

  /// CHECK-START: void Main.inlineNew1() register (after)
  /// CHECK:      MemoryBarrier kind:StoreStore
  /// CHECK-NEXT: ReturnVoid

  /// CHECK-START: void Main.inlineNew1() register (after)
  /// CHECK-NOT:  InvokeStaticOrDirect
  public static void inlineNew1() {
    new InheritFromClassWithFinals();
  }

  /// CHECK-START: void Main.inlineNew2() register (after)
  /// CHECK:      MemoryBarrier kind:StoreStore
  /// CHECK-NEXT: ReturnVoid

  /// CHECK-START: void Main.inlineNew2() register (after)
  /// CHECK-NOT:  InvokeStaticOrDirect
  public static void inlineNew2() {
    new HaveFinalsAndInheritFromClassWithFinals();
  }

  /// CHECK-START: void Main.inlineNew3() register (after)
  /// CHECK:      MemoryBarrier kind:StoreStore
  /// CHECK:      MemoryBarrier kind:StoreStore
  /// CHECK-NEXT: ReturnVoid

  /// CHECK-START: void Main.inlineNew3() register (after)
  /// CHECK-NOT:  InvokeStaticOrDirect
  public static void inlineNew3() {
    new HaveFinalsAndInheritFromClassWithFinals();
    new HaveFinalsAndInheritFromClassWithFinals();
  }

  public static void main(String[] args) {}
}
