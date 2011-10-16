// Copyright 2011 Google Inc. All Rights Reserved.

public class ReferenceMap {
  public ReferenceMap() {
  }

  Object f() {
    Object x[] = new Object[2];
    Object y = null;
    try {
      y = new Object();
      x[2] = y;  // out-of-bound exception
    } catch(Exception ex) {
      if (y == null) {
        x[1] = new Object();
      }
    } finally {
      x[1] = y;
      refmap(0);
    };
    return y;
  }
  native int refmap(int x);

  static {
    System.loadLibrary("arttest");
  }

  public static void main(String[] args) {
    ReferenceMap rm = new ReferenceMap();
    rm.f();
  }
}
