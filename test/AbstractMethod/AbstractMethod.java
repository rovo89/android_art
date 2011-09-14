// Copyright 2011 Google Inc. All Rights Reserved.

abstract class AbstractMethod {
  abstract void callme();

  public AbstractMethod() {
  }
}

class B extends AbstractMethod {
  void callme() {
    System.out.println("B's implementation of callme");
  }
}
