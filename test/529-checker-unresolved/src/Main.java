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

public class Main extends UnresolvedSuperClass {

  /// CHECK-START: void Main.callInvokeUnresolvedStatic() register (before)
  /// CHECK:        InvokeUnresolved invoke_type:static
  static public void callInvokeUnresolvedStatic() {
    UnresolvedClass.staticMethod();
  }

  /// CHECK-START: void Main.callInvokeUnresolvedVirtual(UnresolvedClass) register (before)
  /// CHECK:        InvokeUnresolved invoke_type:virtual
  static public void callInvokeUnresolvedVirtual(UnresolvedClass c) {
    c.virtualMethod();
  }

  /// CHECK-START: void Main.callInvokeUnresolvedInterface(UnresolvedInterface) register (before)
  /// CHECK:        InvokeUnresolved invoke_type:interface
  static public void callInvokeUnresolvedInterface(UnresolvedInterface c) {
    c.interfaceMethod();
  }

  static public void callInvokeUnresolvedSuper(Main c) {
    c.superMethod();
  }

  /// CHECK-START: void Main.superMethod() register (before)
  /// CHECK:        InvokeUnresolved invoke_type:super
  public void superMethod() {
    super.superMethod();
  }

  /// CHECK-START: void Main.callUnresolvedStaticFieldAccess() register (before)
  /// CHECK:        UnresolvedStaticFieldSet field_type:PrimByte
  /// CHECK:        UnresolvedStaticFieldSet field_type:PrimChar
  /// CHECK:        UnresolvedStaticFieldSet field_type:PrimInt
  /// CHECK:        UnresolvedStaticFieldSet field_type:PrimLong
  /// CHECK:        UnresolvedStaticFieldSet field_type:PrimFloat
  /// CHECK:        UnresolvedStaticFieldSet field_type:PrimDouble
  /// CHECK:        UnresolvedStaticFieldSet field_type:PrimNot

  /// CHECK:        UnresolvedStaticFieldGet field_type:PrimByte
  /// CHECK:        UnresolvedStaticFieldGet field_type:PrimChar
  /// CHECK:        UnresolvedStaticFieldGet field_type:PrimInt
  /// CHECK:        UnresolvedStaticFieldGet field_type:PrimLong
  /// CHECK:        UnresolvedStaticFieldGet field_type:PrimFloat
  /// CHECK:        UnresolvedStaticFieldGet field_type:PrimDouble
  /// CHECK:        UnresolvedStaticFieldGet field_type:PrimNot
  static public void callUnresolvedStaticFieldAccess() {
    Object o = new Object();
    UnresolvedClass.staticByte = (byte)1;
    UnresolvedClass.staticChar = '1';
    UnresolvedClass.staticInt = 123456789;
    UnresolvedClass.staticLong = 123456789123456789l;
    UnresolvedClass.staticFloat = 123456789123456789f;
    UnresolvedClass.staticDouble = 123456789123456789d;
    UnresolvedClass.staticObject = o;

    expectEquals((byte)1, UnresolvedClass.staticByte);
    expectEquals('1', UnresolvedClass.staticChar);
    expectEquals(123456789, UnresolvedClass.staticInt);
    expectEquals(123456789123456789l, UnresolvedClass.staticLong);
    expectEquals(123456789123456789f, UnresolvedClass.staticFloat);
    expectEquals(123456789123456789d, UnresolvedClass.staticDouble);
    expectEquals(o, UnresolvedClass.staticObject);
  }

  /// CHECK-START: void Main.callUnresolvedInstanceFieldAccess(UnresolvedClass) register (before)
  /// CHECK:        UnresolvedInstanceFieldSet field_type:PrimByte
  /// CHECK:        UnresolvedInstanceFieldSet field_type:PrimChar
  /// CHECK:        UnresolvedInstanceFieldSet field_type:PrimInt
  /// CHECK:        UnresolvedInstanceFieldSet field_type:PrimLong
  /// CHECK:        UnresolvedInstanceFieldSet field_type:PrimFloat
  /// CHECK:        UnresolvedInstanceFieldSet field_type:PrimDouble
  /// CHECK:        UnresolvedInstanceFieldSet field_type:PrimNot

  /// CHECK:        UnresolvedInstanceFieldGet field_type:PrimByte
  /// CHECK:        UnresolvedInstanceFieldGet field_type:PrimChar
  /// CHECK:        UnresolvedInstanceFieldGet field_type:PrimInt
  /// CHECK:        UnresolvedInstanceFieldGet field_type:PrimLong
  /// CHECK:        UnresolvedInstanceFieldGet field_type:PrimFloat
  /// CHECK:        UnresolvedInstanceFieldGet field_type:PrimDouble
  /// CHECK:        UnresolvedInstanceFieldGet field_type:PrimNot
  static public void callUnresolvedInstanceFieldAccess(UnresolvedClass c) {
    Object o = new Object();
    c.instanceByte = (byte)1;
    c.instanceChar = '1';
    c.instanceInt = 123456789;
    c.instanceLong = 123456789123456789l;
    c.instanceFloat = 123456789123456789f;
    c.instanceDouble = 123456789123456789d;
    c.instanceObject = o;

    expectEquals((byte)1, c.instanceByte);
    expectEquals('1', c.instanceChar);
    expectEquals(123456789, c.instanceInt);
    expectEquals(123456789123456789l, c.instanceLong);
    expectEquals(123456789123456789f, c.instanceFloat);
    expectEquals(123456789123456789d, c.instanceDouble);
    expectEquals(o, c.instanceObject);
  }

  /// CHECK-START: void Main.main(java.lang.String[]) register (before)
  /// CHECK:        InvokeUnresolved invoke_type:direct
  static public void main(String[] args) {
    UnresolvedClass c = new UnresolvedClass();
    callInvokeUnresolvedStatic();
    callInvokeUnresolvedVirtual(c);
    callInvokeUnresolvedInterface(c);
    callInvokeUnresolvedSuper(new Main());
    callUnresolvedStaticFieldAccess();
    callUnresolvedInstanceFieldAccess(c);
  }

  public static void expectEquals(byte expected, byte result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  public static void expectEquals(char expected, char result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  public static void expectEquals(int expected, int result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  public static void expectEquals(long expected, long result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

    public static void expectEquals(float expected, float result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  public static void expectEquals(double expected, double result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  public static void expectEquals(Object expected, Object result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }
}
