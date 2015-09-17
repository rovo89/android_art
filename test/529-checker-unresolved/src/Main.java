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

  /// CHECK-START: void Main.main(java.lang.String[]) register (before)
  /// CHECK:        InvokeUnresolved invoke_type:direct
  static public void main(String[] args) {
    UnresolvedClass c = new UnresolvedClass();
    callInvokeUnresolvedStatic();
    callInvokeUnresolvedVirtual(c);
    callInvokeUnresolvedInterface(c);
    callInvokeUnresolvedSuper(new Main());
  }
}
