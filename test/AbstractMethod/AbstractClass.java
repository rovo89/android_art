// Copyright 2011 Google Inc. All Rights Reserved.

// Test case for AbstractMethodError, we will try to do a non-virtual call to
// foo.
abstract class AbstractClass {
  public AbstractClass() {}

  abstract void foo();
}

class ConcreteClass extends AbstractClass {
  public ConcreteClass() {}

  void foo() {
    throw new Error("This method shouldn't be called");
  }
}
