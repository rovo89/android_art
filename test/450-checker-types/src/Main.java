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


interface Interface {
  void f();
}

class Super implements Interface {
  public void f() {
    throw new RuntimeException();
  }
}

class SubclassA extends Super {
  public void f() {
    throw new RuntimeException();
  }

  public String h() {
    throw new RuntimeException();
  }

  void g() {
    throw new RuntimeException();
  }
}

class SubclassC extends SubclassA {
}

class SubclassB extends Super {
  public void f() {
    throw new RuntimeException();
  }

  void g() {
    throw new RuntimeException();
  }
}

public class Main {

  // CHECK-START: void Main.testSimpleRemove() instruction_simplifier_after_types (before)
  // CHECK:         CheckCast

  // CHECK-START: void Main.testSimpleRemove() instruction_simplifier_after_types (after)
  // CHECK-NOT:     CheckCast
  public void testSimpleRemove() {
    Super s = new SubclassA();
    ((SubclassA)s).g();
  }

  // CHECK-START: void Main.testSimpleKeep(Super) instruction_simplifier_after_types (before)
  // CHECK:         CheckCast

  // CHECK-START: void Main.testSimpleKeep(Super) instruction_simplifier_after_types (after)
  // CHECK:         CheckCast
  public void testSimpleKeep(Super s) {
    ((SubclassA)s).f();
  }

  // CHECK-START: java.lang.String Main.testClassRemove() instruction_simplifier_after_types (before)
  // CHECK:         CheckCast

  // CHECK-START: java.lang.String Main.testClassRemove() instruction_simplifier_after_types (after)
  // CHECK-NOT:     CheckCast
  public String testClassRemove() {
    Object s = SubclassA.class;
    return ((Class)s).getName();
  }

  // CHECK-START: java.lang.String Main.testClassKeep() instruction_simplifier_after_types (before)
  // CHECK:         CheckCast

  // CHECK-START: java.lang.String Main.testClassKeep() instruction_simplifier_after_types (after)
  // CHECK:         CheckCast
  public String testClassKeep() {
    Object s = SubclassA.class;
    return ((SubclassA)s).h();
  }

  // CHECK-START: void Main.testIfRemove(int) instruction_simplifier_after_types (before)
  // CHECK:         CheckCast

  // CHECK-START: void Main.testIfRemove(int) instruction_simplifier_after_types (after)
  // CHECK-NOT:     CheckCast
  public void testIfRemove(int x) {
    Super s;
    if (x % 2 == 0) {
      s = new SubclassA();
    } else {
      s = new SubclassC();
    }
    ((SubclassA)s).g();
  }

  // CHECK-START: void Main.testIfKeep(int) instruction_simplifier_after_types (before)
  // CHECK:         CheckCast

  // CHECK-START: void Main.testIfKeep(int) instruction_simplifier_after_types (after)
  // CHECK:         CheckCast
  public void testIfKeep(int x) {
    Super s;
    if (x % 2 == 0) {
      s = new SubclassA();
    } else {
      s = new SubclassB();
    }
    ((SubclassA)s).g();
  }

  // CHECK-START: void Main.testForRemove(int) instruction_simplifier_after_types (before)
  // CHECK:         CheckCast

  // CHECK-START: void Main.testForRemove(int) instruction_simplifier_after_types (after)
  // CHECK-NOT:     CheckCast
  public void testForRemove(int x) {
    Super s = new SubclassA();
    for (int i = 0 ; i < x; i++) {
      if (x % 2 == 0) {
        s = new SubclassC();
      }
    }
    ((SubclassA)s).g();
  }

  // CHECK-START: void Main.testForKeep(int) instruction_simplifier_after_types (before)
  // CHECK:         CheckCast

  // CHECK-START: void Main.testForKeep(int) instruction_simplifier_after_types (after)
  // CHECK:         CheckCast
  public void testForKeep(int x) {
    Super s = new SubclassA();
    for (int i = 0 ; i < x; i++) {
      if (x % 2 == 0) {
        s = new SubclassC();
      }
    }
    ((SubclassC)s).g();
  }

  // CHECK-START: void Main.testPhiFromCall(int) instruction_simplifier_after_types (before)
  // CHECK:         CheckCast

  // CHECK-START: void Main.testPhiFromCall(int) instruction_simplifier_after_types (after)
  // CHECK:         CheckCast
  public void testPhiFromCall(int i) {
    Object x;
    if (i % 2 == 0) {
      x = new SubclassC();
    } else {
      x = newObject();  // this one will have an unknown type.
    }
    ((SubclassC)x).g();
  }

  public Object newObject() {
    try {
      return Object.class.newInstance();
    } catch (Exception e) {
      return null;
    }
  }

  public static void main(String[] args) {
  }
}
