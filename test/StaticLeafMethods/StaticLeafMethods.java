// Copyright 2011 Google Inc. All Rights Reserved.

class StaticLeafMethods {
    static void nop() {
    }
    static byte identity(byte x) {
        return x;
    }
    static int identity(int x) {
        return x;
    }
    static int sum(int a, int b) {
        return a + b;
    }
    static int sum(int a, int b, int c) {
        return a + b + c;
    }
    static int sum(int a, int b, int c, int d) {
        return a + b + c + d;
    }
    static int sum(int a, int b, int c, int d, int e) {
        return a + b + c + d + e;
    }
    static double identity(double x) {
        return x;
    }
    static double sum(double a, double b) {
        return a + b;
    }
    static double sum(double a, double b, double c) {
        return a + b + c;
    }
    static double sum(double a, double b, double c, double d) {
        return a + b + c + d;
    }
    static double sum(double a, double b, double c, double d, double e) {
        return a + b + c + d + e;
    }
}
