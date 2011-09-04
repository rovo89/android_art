// Copyright 2011 Google Inc. All Rights Reserved.

class Statics {
    static boolean s0 = true;
    static byte s1 = 5;
    static char s2 = 'a';
    static short s3 = (short) 65000;
    static int s4 = 2000000000;
    static long s5 = 0x123456789abcdefL;
    static float s6 = 0.5f;
    static double s7 = 16777217;
    static Object s8 = "android";
    static Object[] s9 = { "a", "b" };

    static boolean getS0() {
        return s0;
    }
    static byte getS1() {
        return s1;
    }
    static char getS2() {
        return s2;
    }
    static short getS3() {
        return s3;
    }
    static int getS4() {
        return s4;
    }
    static long getS5() {
        return s5;
    }
    static float getS6() {
        return s6;
    }
    static double getS7() {
        return s7;
    }
    static Object getS8() {
        return s8;
    }
    static Object[] getS9() {
        return s9;
    }
}
