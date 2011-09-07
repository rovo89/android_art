// Copyright 2011 Google Inc. All Rights Reserved.

class MyClass {
    native void throwException();
    native void foo();
    native int fooI(int x);
    native int fooII(int x, int y);
    native long fooJJ(long x, long y);
    native Object fooO(Object x);
    native double fooDD(double x, double y);
    native Object fooIOO(int x, Object y, Object z);
    static native Object fooSIOO(int x, Object y, Object z);
    static native int fooSII(int x, int y);
    static native double fooSDD(double x, double y);
    static synchronized native Object fooSSIOO(int x, Object y, Object z);
    static native void arraycopy(Object src, int src_pos, Object dst, int dst_pos, int length);
}
