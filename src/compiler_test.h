// Copyright 2011 Google Inc. All Rights Reserved.

#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "base64.h"
#include "heap.h"
#include "thread.h"
#include "stringprintf.h"
#include "class_linker.h"
#include "dex_file.h"

#include "unicode/uclean.h"
#include "unicode/uvernum.h"

#include "gtest/gtest.h"

namespace art {

//class IntMath {
//
//    private int foo_;
//
//    public IntMath(int stuff) {
//        foo_ = stuff;
//    }
//
//    public IntMath() {
//        foo_ = 123;
//    }
//
//    /*
//     * Try to cause some unary operations.
//     */
//    static int unopTest(int x) {
//        x = -x;
//        x ^= 0xffffffff;
//        return x;
//    }
//
//    static int shiftTest1() {
//        final int[] mBytes = {
//            0x11, 0x22, 0x33, 0x44, 0x88, 0x99, 0xaa, 0xbb
//        };
//        long l;
//        int i1, i2;
//
//        if (mBytes[0] != 0x11) return 20;
//        if (mBytes[1] != 0x22) return 21;
//        if (mBytes[2] != 0x33) return 22;
//        if (mBytes[3] != 0x44) return 23;
//        if (mBytes[4] != 0x88) return 24;
//        if (mBytes[5] != 0x99) return 25;
//        if (mBytes[6] != 0xaa) return 26;
//        if (mBytes[7] != 0xbb) return 27;
//
//        i1 = mBytes[0] | mBytes[1] << 8 | mBytes[2] << 16 | mBytes[3] << 24;
//        i2 = mBytes[4] | mBytes[5] << 8 | mBytes[6] << 16 | mBytes[7] << 24;
//        l = i1 | ((long)i2 << 32);
//
//        if (i1 != 0x44332211) { return 0x80000000 | i1; }
//        if (i2 != 0xbbaa9988) { return 2; }
//        if (l != 0xbbaa998844332211L) { return 3; }
//
//        l = (long)mBytes[0]
//            | (long)mBytes[1] << 8
//            | (long)mBytes[2] << 16
//            | (long)mBytes[3] << 24
//            | (long)mBytes[4] << 32
//            | (long)mBytes[5] << 40
//            | (long)mBytes[6] << 48
//            | (long)mBytes[7] << 56;
//
//        if (l != 0xbbaa998844332211L) { return 4; }
//        return 0;
//    }
//
//    static int shiftTest2() {
//
//        long    a = 0x11;
//        long    b = 0x22;
//        long    c = 0x33;
//        long    d = 0x44;
//        long    e = 0x55;
//        long    f = 0x66;
//        long    g = 0x77;
//        long    h = 0x88;
//
//        long    result = ((a << 56) | (b << 48) | (c << 40) | (d << 32) |
//                         (e << 24) | (f << 16) | (g <<  8) | h);
//
//        if (result != 0x1122334455667788L) { return 1; }
//        return 0;
//    }
//
//    static int unsignedShiftTest() {
//        byte b = -4;
//        short s = -4;
//        char c = 0xfffc;
//        int i = -4;
//
//        b >>>= 4;
//        s >>>= 4;
//        c >>>= 4;
//        i >>>= 4;
//
//        if ((int) b != -1) { return 1; }
//        if ((int) s != -1) { return 2; }
//        if ((int) c != 0x0fff) { return 3; }
//        if (i != 268435455) { return 4; }
//        return 0;
//    }
//
//    static int convTest() {
//
//        float f;
//        double d;
//        int i;
//        long l;
//
//        /* int --> long */
//        i = 7654;
//        l = (long) i;
//        if (l != 7654L) { return 1; }
//
//        i = -7654;
//        l = (long) i;
//        if (l != -7654L) { return 2; }
//
//        /* long --> int (with truncation) */
//        l = 5678956789L;
//        i = (int) l;
//        if (i != 1383989493) { return 3; }
//
//        l = -5678956789L;
//        i = (int) l;
//        if (i != -1383989493) { return 4; }
//        return 0;
//    }
//
//    static int charSubTest() {
//
//        char char1 = 0x00e9;
//        char char2 = 0xffff;
//        int i;
//
//        /* chars are unsigned-expanded to ints before subtraction */
//        i = char1 - char2;
//        if (i != 0xffff00ea) { return 1; }
//        return 0;
//    }
//
//    /*
//     * We pass in the arguments and return the results so the compiler
//     * doesn't do the math for us.  (x=70000, y=-3)
//     */
//    static int intOperTest(int x, int y) {
//        int[] results = new int[10];
//
//        /* this seems to generate "op-int" instructions */
//        results[0] = x + y;
//        results[1] = x - y;
//        results[2] = x * y;
//        results[3] = x * x;
//        results[4] = x / y;
//        results[5] = x % -y;
//        results[6] = x & y;
//        results[7] = x | y;
//        results[8] = x ^ y;
//
//        /* this seems to generate "op-int/2addr" instructions */
//        results[9] = x + ((((((((x + y) - y) * y) / y) % y) & y) | y) ^ y);
//
//        /* check this edge case while we're here (div-int/2addr) */
//        int minInt = -2147483648;
//        int negOne = -results[5];
//        int plusOne = 1;
//        int result = (((minInt + plusOne) - plusOne) / negOne) / negOne;
//
//        if (result != minInt) { return 1;};
//        if (results[0] != 69997) { return 2;};
//        if (results[1] != 70003) { return 3;};
//        if (results[2] != -210000) { return 4;};
//        if (results[3] != 605032704) { return 5;};
//        if (results[4] != -23333) { return 6;};
//        if (results[5] != 1) { return 7;};
//        if (results[6] != 70000) { return 8;};
//        if (results[7] != -3) { return 9;};
//        if (results[8] != -70003) { return 10;};
//        if (results[9] != 70000) { return 11;};
//
//        return 0;
//    }
//
//    /*
//     * More operations, this time with 16-bit constants.  (x=77777)
//     */
//    static int lit16Test(int x) {
//
//        int[] results = new int[8];
//
//        /* try to generate op-int/lit16" instructions */
//        results[0] = x + 1000;
//        results[1] = 1000 - x;
//        results[2] = x * 1000;
//        results[3] = x / 1000;
//        results[4] = x % 1000;
//        results[5] = x & 1000;
//        results[6] = x | -1000;
//        results[7] = x ^ -1000;
//
//        if (results[0] != 78777) { return 1; }
//        if (results[1] != -76777) { return 2; }
//        if (results[2] != 77777000) { return 3; }
//        if (results[3] != 77) { return 4; }
//        if (results[4] != 777) { return 5; }
//        if (results[5] != 960) { return 6; }
//        if (results[6] != -39) { return 7; }
//        if (results[7] != -76855) { return 8; }
//        return 0;
//    }
//
//    /*
//     * More operations, this time with 8-bit constants.  (x=-55555)
//     */
//    static int lit8Test(int x) {
//
//        int[] results = new int[8];
//
//        /* try to generate op-int/lit8" instructions */
//        results[0] = x + 10;
//        results[1] = 10 - x;
//        results[2] = x * 10;
//        results[3] = x / 10;
//        results[4] = x % 10;
//        results[5] = x & 10;
//        results[6] = x | -10;
//        results[7] = x ^ -10;
//        int minInt = -2147483648;
//        int result = minInt / -1;
//        if (result != minInt) {return 1; }
//        if (results[0] != -55545) {return 2; }
//        if (results[1] != 55565) {return 3; }
//        if (results[2] != -555550) {return 4; }
//        if (results[3] != -5555) {return 5; }
//        if (results[4] != -5) {return 6; }
//        if (results[5] != 8) {return 7; }
//        if (results[6] != -1) {return 8; }
//        if (results[7] != 55563) {return 9; }
//        return 0;
//    }
//
//
//    /*
//     * Shift some data.  (value=0xff00aa01, dist=8)
//     */
//    static int intShiftTest(int value, int dist) {
//        int results[] = new int[4];
//        results[0] = value << dist;
//        results[1] = value >> dist;
//        results[2] = value >>> dist;
//        results[3] = (((value << dist) >> dist) >>> dist) << dist;
//        if (results[0] != 0x00aa0100) {return 1; }
//        if (results[1] != 0xffff00aa) {return 2; }
//        if (results[2] != 0x00ff00aa) {return 3; }
//        if (results[3] != 0xaa00) {return 4; }
//        return 0;
//    }
//
//    /*
//     * We pass in the arguments and return the results so the compiler
//     * doesn't do the math for us.  (x=70000000000, y=-3)
//     */
//    static int longOperTest(long x, long y) {
//        long[] results = new long[10];
//
//        /* this seems to generate "op-long" instructions */
//        results[0] = x + y;
//        results[1] = x - y;
//        results[2] = x * y;
//        results[3] = x * x;
//        results[4] = x / y;
//        results[5] = x % -y;
//        results[6] = x & y;
//        results[7] = x | y;
//        results[8] = x ^ y;
//        /* this seems to generate "op-long/2addr" instructions */
//        results[9] = x + ((((((((x + y) - y) * y) / y) % y) & y) | y) ^ y);
//        /* check this edge case while we're here (div-long/2addr) */
//        long minLong = -9223372036854775808L;
//        long negOne = -results[5];
//        long plusOne = 1;
//        long result = (((minLong + plusOne) - plusOne) / negOne) / negOne;
//        if (result != minLong) { return 1; }
//        if (results[0] != 69999999997L) { return 2; }
//        if (results[1] != 70000000003L) { return 3; }
//        if (results[2] != -210000000000L) { return 4; }
//        if (results[3] != -6833923606740729856L) { return 5; }    // overflow
//        if (results[4] != -23333333333L) { return 6; }
//        if (results[5] != 1) { return 7; }
//        if (results[6] != 70000000000L) { return 8; }
//        if (results[7] != -3) { return 9; }
//        if (results[8] != -70000000003L) { return 10; }
//        if (results[9] != 70000000000L) { return 11; }
//        if (results.length != 10) { return 12; }
//        return 0;
//    }
//
//    /*
//     * Shift some data.  (value=0xd5aa96deff00aa01, dist=16)
//     */
//    static long longShiftTest(long value, int dist) {
//        long results[] = new long[4];
//        results[0] = value << dist;
//        results[1] = value >> dist;
//        results[2] = value >>> dist;
//        results[3] = (((value << dist) >> dist) >>> dist) << dist;
//        if (results[0] != 0x96deff00aa010000L) { return results[0]; }
//        if (results[1] != 0xffffd5aa96deff00L) { return results[1]; }
//        if (results[2] != 0x0000d5aa96deff00L) { return results[2]; }
//        if (results[3] != 0xffff96deff000000L) { return results[3]; }
//        if (results.length != 4) { return 5; }
//
//        return results[0];      // test return-long
//    }
//
//    static int switchTest(int a) {
//        int res = 1234;
//
//        switch (a) {
//            case -1: res = 1; return res;
//            case 0: res = 2; return res;
//            case 1: /*correct*/ break;
//            case 2: res = 3; return res;
//            case 3: res = 4; return res;
//            case 4: res = 5; return res;
//            default: res = 6; return res;
//        }
//        switch (a) {
//            case 3: res = 7; return res;
//            case 4: res = 8; return res;
//            default: /*correct*/ break;
//        }
//
//        a = 0x12345678;
//
//        switch (a) {
//            case 0x12345678: /*correct*/ break;
//            case 0x12345679: res = 9; return res;
//            default: res = 1; return res;
//        }
//        switch (a) {
//            case 57: res = 10; return res;
//            case -6: res = 11; return res;
//            case 0x12345678: /*correct*/ break;
//            case 22: res = 12; return res;
//            case 3: res = 13; return res;
//            default: res = 14; return res;
//        }
//        switch (a) {
//            case -6: res = 15; return res;
//            case 3: res = 16; return res;
//            default: /*correct*/ break;
//        }
//
//        a = -5;
//        switch (a) {
//            case 12: res = 17; return res;
//            case -5: /*correct*/ break;
//            case 0: res = 18; return res;
//            default: res = 19; return res;
//        }
//
//        switch (a) {
//            default: /*correct*/ break;
//        }
//        return res;
//    }
//    /*
//     * Test the integer comparisons in various ways.
//     */
//    static int testIntCompare(int minus, int plus, int plus2, int zero) {
//        int res = 1111;
//
//        if (minus > plus)
//            return 1;
//        if (minus >= plus)
//            return 2;
//        if (plus < minus)
//            return 3;
//        if (plus <= minus)
//            return 4;
//        if (plus == minus)
//            return 5;
//        if (plus != plus2)
//            return 6;
//
//        /* try a branch-taken */
//        if (plus != minus) {
//            res = res;
//        } else {
//            return 7;
//        }
//
//        if (minus > 0)
//            return 8;
//        if (minus >= 0)
//            return 9;
//        if (plus < 0)
//            return 10;
//        if (plus <= 0)
//            return 11;
//        if (plus == 0)
//            return 12;
//        if (zero != 0)
//            return 13;
//
//        if (zero == 0) {
//            res = res;
//        } else {
//            return 14;
//        }
//        return res;
//    }
//
//    /*
//     * Test cmp-long.
//     *
//     * minus=-5, alsoMinus=0xFFFFFFFF00000009, plus=4, alsoPlus=8
//     */
//    static int testLongCompare(long minus, long alsoMinus, long plus,
//        long alsoPlus) {
//        int res = 2222;
//
//        if (minus > plus)
//            return 2;
//        if (plus < minus)
//            return 3;
//        if (plus == minus)
//            return 4;
//
//        if (plus >= plus+1)
//            return 5;
//        if (minus >= minus+1)
//            return 6;
//
//        /* try a branch-taken */
//        if (plus != minus) {
//            res = res;
//        } else {
//            return 7;
//        }
//
//        /* compare when high words are equal but low words differ */
//        if (plus > alsoPlus)
//            return 8;
//        if (alsoPlus < plus)
//            return 9;
//        if (alsoPlus == plus)
//            return 10;
//
//        /* high words are equal, low words have apparently different signs */
//        if (minus < alsoMinus)      // bug!
//            return 11;
//        if (alsoMinus > minus)
//            return 12;
//        if (alsoMinus == minus)
//            return 13;
//
//        return res;
//    }
//
//    /*
//     * Test cmpl-float and cmpg-float.
//     */
//    static int testFloatCompare(float minus, float plus, float plus2,
//        float nan) {
//
//        int res = 3333;
//        if (minus > plus)
//            res = 1;
//        if (plus < minus)
//            res = 2;
//        if (plus == minus)
//            res = 3;
//        if (plus != plus2)
//            res = 4;
//
//        if (plus <= nan)
//            res = 5;
//        if (plus >= nan)
//            res = 6;
//        if (minus <= nan)
//            res = 7;
//        if (minus >= nan)
//            res = 8;
//        if (nan >= plus)
//            res = 9;
//        if (nan <= plus)
//            res = 10;
//
//        if (nan == nan)
//            res = 1212;
//
//        return res;
//    }
//
//    static int testDoubleCompare(double minus, double plus, double plus2,
//        double nan) {
//
//        int res = 4444;
//
//        if (minus > plus)
//            return 1;
//        if (plus < minus)
//            return 2;
//        if (plus == minus)
//            return 3;
//        if (plus != plus2)
//            return 4;
//
//        if (plus <= nan)
//            return 5;
//        if (plus >= nan)
//            return 6;
//        if (minus <= nan)
//            return 7;
//        if (minus >= nan)
//            return 8;
//        if (nan >= plus)
//            return 9;
//        if (nan <= plus)
//            return 10;
//
//        if (nan == nan)
//            return 11;
//        return res;
//    }
//
//    static int fibonacci(int n) {
//        if (n == 0) {
//            return 0;
//        } else if (n == 1) {
//            return 1;
//        } else {
//            return fibonacci(n - 1) + fibonacci(n - 2);
//        }
//    }
//
///*
//    static void throwNullPointerException() {
//        throw new NullPointerException("first throw");
//    }
//
//    static int throwAndCatch() {
//        try {
//            throwNullPointerException();
//            return 1;
//        } catch (NullPointerException npe) {
//            return 0;
//        }
//    }
//*/
//
//    static int manyArgs(int a0, long a1, int a2, long a3, int a4, long a5,
//        int a6, int a7, double a8, float a9, double a10, short a11, int a12,
//        char a13, int a14, int a15, byte a16, boolean a17, int a18, int a19,
//        long a20, long a21, int a22, int a23, int a24, int a25, int a26)
//    {
//        if (a0 != 0) return 0;
//        if (a1 !=  1L) return 1;
//        if (a2 != 2) return 2;
//        if (a3 != 3L) return 3;
//        if (a4 != 4) return 4;
//        if (a5 != 5L) return 5;
//        if (a6 != 6) return 6;
//        if (a7 != 7) return 7;
//        if (a8 != 8.0) return 8;
//        if (a9 !=  9.0f) return 9;
//        if (a10 != 10.0) return 10;
//        if (a11 != (short)11) return 11;
//        if (a12 != 12) return 12;
//        if (a13 != (char)13) return 13;
//        if (a14 != 14) return 14;
//        if (a15 != 15) return 15;
//        if (a16 != (byte)-16) return 16;
//        if (a17 !=  true) return 17;
//        if (a18 != 18) return 18;
//        if (a19 != 19) return 19;
//        if (a20 !=  20L) return 20;
//        if (a21 != 21L) return 21;
//        if (a22 != 22) return 22;
//        if (a23 != 23) return 23;
//        if (a24 != 24) return 24;
//        if (a25 != 25) return 25;
//        if (a26 != 26) return 26;
//        return -1;
//    }
//
//    int virtualCall(int a)
//    {
//        return a * 2;
//    }
//
//    void setFoo(int a)
//    {
//        foo_ = a;
//    }
//
//    int getFoo()
//    {
//        return foo_;
//    }
//
//    static int staticCall(int a)
//    {
//        IntMath foo = new IntMath();
//        return foo.virtualCall(a);
//    }
//
//   static int testIGetPut(int a)
//    {
//        IntMath foo = new IntMath(99);
//        IntMath foo123 = new IntMath();
//        int z  = foo.getFoo();
//        z += a;
//        z += foo123.getFoo();
//        foo.setFoo(z);
//        return foo.getFoo();
//    }
//
//    public static void main(String[] args) {
//        int res = unopTest(38);
//        if (res == 37) {
//            System.out.printf("unopTest PASSED\n");
//        } else {
//            System.out.printf("unopTest FAILED: %d\n", res);
//        }
//        res = shiftTest1();
//        if (res == 0) {
//            System.out.printf("shiftTest1 PASSED\n");
//        } else {
//            System.out.printf("shiftTest1 FAILED: %d\n", res);
//        }
//        res = shiftTest2();
//        if (res == 0) {
//            System.out.printf("shiftTest2 PASSED\n");
//        } else {
//            System.out.printf("shiftTest2 FAILED: %d\n", res);
//        }
//        res = unsignedShiftTest();
//        if (res == 0) {
//            System.out.printf("unsignedShiftTest PASSED\n");
//        } else {
//            System.out.printf("unsignedShiftTest FAILED: %d\n", res);
//        }
//        res = convTest();
//        if (res == 0) {
//            System.out.printf("convTest PASSED\n");
//        } else {
//            System.out.printf("convTest FAILED: %d\n", res);
//        }
//        res = charSubTest();
//        if (res == 0) {
//            System.out.printf("charSubTest PASSED\n");
//        } else {
//            System.out.printf("charSubTest FAILED: %d\n", res);
//        }
//        res = intOperTest(70000, -3);
//        if (res == 0) {
//            System.out.printf("intOperTest PASSED\n");
//        } else {
//            System.out.printf("intOperTest FAILED: %d\n", res);
//        }
//        res = longOperTest(70000000000L, -3L);
//        if (res == 0) {
//            System.out.printf("longOperTest PASSED\n");
//        } else {
//            System.out.printf("longOperTest FAILED: %d\n", res);
//        }
//        long lres = longShiftTest(0xd5aa96deff00aa01L, 16);
//        if (lres == 0x96deff00aa010000L) {
//            System.out.printf("longShiftTest PASSED\n");
//        } else {
//            System.out.printf("longShiftTest FAILED: %d\n", res);
//        }
//
//        res = switchTest(1);
//        if (res == 1234) {
//            System.out.printf("switchTest PASSED\n");
//        } else {
//            System.out.printf("switchTest FAILED: %d\n", res);
//        }
//
//        res = testIntCompare(-5, 4, 4, 0);
//        if (res == 1111) {
//            System.out.printf("testIntCompare PASSED\n");
//        } else {
//            System.out.printf("testIntCompare FAILED: %d\n", res);
//        }
//
//        res = testLongCompare(-5L, -4294967287L, 4L, 8L);
//        if (res == 2222) {
//            System.out.printf("testLongCompare PASSED\n");
//        } else {
//            System.out.printf("testLongCompare FAILED: %d\n", res);
//        }
//
//        res = testFloatCompare(-5.0f, 4.0f, 4.0f, (1.0f/0.0f) / (1.0f/0.0f));
//        if (res == 3333) {
//            System.out.printf("testFloatCompare PASSED\n");
//        } else {
//            System.out.printf("testFloatCompare FAILED: %d\n", res);
//        }
//
//        res = testDoubleCompare(-5.0, 4.0, 4.0, (1.0/0.0) / (1.0/0.0));
//        if (res == 4444) {
//            System.out.printf("testDoubleCompare PASSED\n");
//        } else {
//            System.out.printf("testDoubleCompare FAILED: %d\n", res);
//        }
//
//        res = fibonacci(10);
//        if (res == 55) {
//            System.out.printf("fibonacci PASSED\n");
//        } else {
//            System.out.printf("fibonacci FAILED: %d\n", res);
//        }
//
///*
//        res = throwAndCatch();
//        if (res == 0) {
//            System.out.printf("throwAndCatch PASSED\n");
//        } else {
//            System.out.printf("throwAndCatch FAILED: %d\n", res);
//        }
//*/
//
//        res = manyArgs(0, 1L, 2, 3L, 4, 5L, 6, 7, 8.0, 9.0f, 10.0,
//                      (short)11, 12, (char)13, 14, 15, (byte)-16, true, 18,
//                      19, 20L, 21L, 22, 23, 24, 25, 26);
//        if (res == -1) {
//            System.out.printf("manyArgs PASSED\n");
//        } else {
//            System.out.printf("manyArgs FAILED: %d\n", res);
//        }
//
//        res = staticCall(3);
//        if (res == 6) {
//            System.out.printf("virtualCall PASSED\n");
//        } else {
//            System.out.printf("virtualCall FAILED: %d\n", res);
//        }
//
//        res = testIGetPut(111);
//        if (res == 333) {
//            System.out.printf("testGetPut PASSED\n");
//        } else {
//            System.out.printf("testGetPut FAILED: %d\n", res);
//        }
//    }
//}
static const char kIntMathDex[] =
"ZGV4CjAzNQC7e4Y087eJCugxNYogYY46TkuaUfNBl5W8KgAAcAAAAHhWNBIAAAAAAAAAABwqAACp"
"AAAAcAAAABMAAAAUAwAADwAAAGADAAACAAAAFAQAAB4AAAAkBAAAAgAAABQFAABoJQAAVAUAAKYd"
"AACuHQAAsR0AALQdAAC3HQAAuh0AAL0dAADEHQAAyx0AAM8dAADUHQAA2x0AAPkdAAD+HQAABR4A"
"ABMeAAAWHgAAGx4AAB8eAAAqHgAALx4AAEYeAABbHgAAbx4AAIMeAACXHgAApB4AAKceAACqHgAA"
"rh4AALIeAAC1HgAAuR4AAL0eAADSHgAA5x4AAOoeAADuHgAA8h4AAPceAAD8HgAAAR8AAAYfAAAL"
"HwAAEB8AABUfAAAaHwAAHx8AACQfAAAoHwAALR8AADIfAAA3HwAAPB8AAEEfAABGHwAASx8AAE8f"
"AABTHwAAVx8AAFsfAABfHwAAYx8AAGcfAAByHwAAfB8AAIIfAACFHwAAiB8AAI8fAACWHwAAox8A"
"ALwfAADRHwAA2x8AAPEfAAADIAAABiAAAAwgAAAPIAAAEiAAAB0gAAA0IAAARyAAAEwgAABUIAAA"
"WiAAAF0gAABlIAAAaCAAAGsgAABvIAAAcyAAAIAgAACZIAAAriAAALwgAAC/IAAAyiAAANQgAADi"
"IAAA/CAAABIhAAAhIQAAPCEAAFMhAABZIQAAYSEAAGchAABxIQAAhyEAAJkhAAChIQAAqiEAALEh"
"AAC0IQAAuSEAAMEhAADGIQAAzCEAANMhAADcIQAA5CEAAOkhAADxIQAA+iEAAP0hAAAFIgAAESIA"
"ACkiAAA9IgAASSIAAGEiAAB1IgAAgSIAAIgiAACUIgAArCIAAMAiAADTIgAA8iIAAA0jAAAfIwAA"
"PSMAAFcjAABvIwAAgyMAAJAjAACgIwAAvCMAANQjAADlIwAAAiQAABskAAAhJAAAKyQAAEEkAABT"
"JAAAZiQAAIUkAACgJAAApyQAALAkAAC9JAAA1iQAAOskAADuJAAA8SQAAPQkAAABAAAAAgAAAAMA"
"AAAEAAAABQAAAA8AAAASAAAAFAAAABUAAAAWAAAAFwAAABgAAAAaAAAAGwAAAB4AAAAfAAAAIAAA"
"ACEAAAAiAAAABQAAAAQAAAAAAAAABgAAAAQAAAAMHQAABwAAAAQAAAAYHQAACAAAAAQAAAAkHQAA"
"CQAAAAQAAAAsHQAACgAAAAQAAAA0HQAACwAAAAQAAABAHQAADAAAAAQAAAB8HQAADQAAAAQAAACE"
"HQAAEAAAAAUAAACQHQAAEwAAAAcAAACYHQAAEQAAAAgAAAAkHQAAGwAAAA0AAAAAAAAAHAAAAA0A"
"AAAkHQAAHQAAAA0AAACgHQAABgAEAFUAAAALAAcAdQAAAAYADAAAAAAABgANAAAAAAAGAAAARgAA"
"AAYAAABJAAAABgADAFAAAAAGAAAAVwAAAAYABABcAAAABgAEAF8AAAAGAAMAYQAAAAYAAwBiAAAA"
"BgAHAGMAAAAGAAkAZgAAAAYADgBrAAAABgAGAGwAAAAGAA0AfgAAAAYAAAB/AAAABgAAAIIAAAAG"
"AAMAhQAAAAYAAwCHAAAABgABAIoAAAAGAAIAjQAAAAYAAwCSAAAABgAFAJMAAAAGAAgAlgAAAAYA"
"AwCaAAAABgAAAJ0AAAAGAAMAogAAAAcACgB5AAAACAALAKEAAAAJAAwAAAAAAAkAAAABAAAA////"
"/wAAAAAZAAAAAAAAAJopAAAAAAAABgAAAAAAAAAJAAAAAAAAAA4AAAAAAAAApCkAAAAAAAABAAEA"
"AAAAAPokAAABAAAADgAAAAIAAQABAAAA/yQAAAgAAABwEB0AAQATAHsAWRAAAA4AAgACAAEAAAAG"
"JQAABgAAAHAQHQAAAFkBAAAOAAQAAAAAAAAADyUAABAAAAATAOkAFAH//wAAkQIAARQD6gD//zIy"
"BAASEw8DEgMo/gUAAAAAAAAAJCUAADIAAAATAOYdgQEWA+YdMQMBAzgDBAASEw8DEwAa4oEBFgMa"
"4jEDAQM4AwQAEiMo9RgB9QB+UgEAAACEEBQD9QB+UjIwBAASMyjoGAEL/4Gt/v///4QQFAML/4Gt"
"MjAEABJDKNsSAyjZAwABAAEAAABCJQAAFQAAABIQOQIEABIADwAyAv//2AAC/3EQBAAAAAoA2AEC"
"/nEQBAABAAoBsBAo8AAADgACAAAAAABNJQAAsAAAABI3EiYSCRJYEhUTCgoAI6QPAJAKDA1LCgQJ"
"kQoMDUsKBAWSCgwNSwoEBpIKDAxLCgQHEkqTCwwNSwsECnvalAoMCksKBAgSapULDA1LCwQKEnqW"
"CwwNSwsEChMKCACXCwwNSwsEChMKCQCQCwwNsduy27PbtNu127bbt9uwy0sLBAoVAACARAoECHuh"
"EhIVCgCAsxqTAwoBMgMDAA8FRAoECRQLbREBADK6BAABZSj3RAoEBRQLcxEBADK6BAABdSjuRAYE"
"BhQKsMv8/zKmBAASRSjlRAYEBxQHABEQJDJ2BAABhSjcEkZEBgQGEwfbpDJ2BAASZSjTRAYECDJW"
"BAASdSjNEmVEBQQFFAZwEQEAMmUFABMFCAAowhJ1RAUEBRLWMmUFABMFCQAouRMFCABEBQQFFAaN"
"7v7/MmUFABMFCgAorRMFCQBEBQQFFAZwEQEAMmUFABMFCwAooQGVKJ8KAAIAAAAAAIolAAA/AAAA"
"EkQSMxIiEhESBSNADwCYBggJSwYABZkGCAlLBgABmgYICUsGAAKYBggJuZa6lriWSwYAA0QGAAUU"
"BwABqgAydgMADwFEAQABFAaqAP//MmEEAAEhKPdEAQACFAKqAP8AMiEEAAExKO5EAQADFAIAqgAA"
"MiEEAAFBKOUBUSjjAAAJAAEAAAAAAKElAAB1AAAAEkQSMxIiEhESBRMGCAAjYA8A0IboA0sGAAXR"
"hugDSwYAAdKG6ANLBgAC04boA0sGAAPUhugDSwYABBJW1YfoA0sHAAYSZtaHGPxLBwAGEnbXhxj8"
"SwcABkQGAAUUB7kzAQAydgMADwFEAQABFAYX1P7/MmEEAAEhKPdEAQACFAJoyKIEMiEEAAExKO5E"
"AQADEwJNADIhBAABQSjmRAEABBMCCQMyIQQAElEo3hJRRAEAARMCwAMyIQQAEmEo1RJhRAEAARMC"
"2f8yIQQAEnEozBJxRAEAARQCydP+/zIhBQATAQgAKMEBUSi/AAALAAEAAAAAAMElAAB8AAAAEkYS"
"NRIkEhMSBxMICAAjgg8A2AgKCksIAgfZCAoKSwgCA9oICgpLCAIE2wgKCksIAgXcCAoKSwgCBhJY"
"3QkKCksJAggSaN4JCvZLCQIIEnjfCQr2SwkCCBUAAIDbAQD/MgEDAA8DRAgCBxQJByf//zKYBAAB"
"Qyj3RAMCAxQIDdkAADKDBAABUyjuRAMCBBQE4oX3/zJDBAABYyjlRAMCBRMETeoyQwQAElMo3UQD"
"AgYStDJDBAASYyjWElNEAwIDEwQIADJDBAAScyjNEmNEAwIDEvQyQwUAEwMIACjEEnNEAwIDFAQL"
"2QAAMkMFABMDCQAouQFzKLcRAAQAAAAAAOwlAADuAAAAEwkKACOYEAASCZsKDQ9MCggJEhmcCg0P"
"TAoICRIpnQoND0wKCAkSOZ0KDQ1MCggJEkmeCg0PTAoICRJZffqfCg0KTAoICRJpoAoND0wKCAkS"
"eaEKDQ9MCggJEwkIAKIKDQ9MCggJEwkJAJsKDQ+8+r36vvq/+sD6wfrC+rvaTAoICRkAAIASWUUJ"
"CAl9khYEAQCbCQAEvEm+KZ4GCQIxCQYAOAkEABIZDwkSCUUJCAkYC/07U0wQAAAAMQkJCzgJBAAS"
"KSjyEhlFCQgJGAsDPFNMEAAAADEJCQs4CQQAEjko5BIpRQkICRgLAEwGG8////8xCQkLOAkEABJJ"
"KNYSOUUJCAkYCwAAEPavBimhMQkJCzgJBAASWSjIEklFCQgJGAurljmR+v///zEJCQs4CQQAEmko"
"uhJZRQkICRYLAQAxCQkLOAkEABJ5KK8SaUUJCAkYCwA8U0wQAAAAMQkJCzgJBQATCQgAKKASeUUJ"
"CAkWC/3/MQkJCzgJBQATCQkAKJQTCQgARQkICRgL/cOss+////8xCQkLOAkFABMJCgAohBMJCQBF"
"CQgJGAsAPFNMEAAAADEJCQs4CQYAEwkLACkAdP8hiRMKCgAyqQYAEwkMACkAa/8SCSkAaP8NAAMA"
"AAAAACwmAABbAAAAEkkSOBInEhYSBSOQEACjAQoMTAEABaQBCgxMAQAGpQEKDEwBAAejAQoMxMHF"
"wcPBTAEACEUBAAUYAwAAAaoA/96WMQEBAzgBBQBFAQAFEAFFAQAGGAMA/96WqtX//zEBAQM4AQUA"
"RQEABijyRQEABxgDAP/elqrVAAAxAQEDOAEFAEUBAAco5EUBAAgYAwAAAP/elv//MQEBAzgBBQBF"
"AQAIKNYhATKRBQAWAQUAKNBFAQAFKM0AACcAAQAiAAAARiYAAA4DAAATASYAcRAYAAEACiUTASUA"
"AgAlADMQrgFiAQEAGgKcABIDIzMRAG4wGwAhA3EADwAAAAolOSWxAWIBAQAaAoEAEgMjMxEAbjAb"
"ACEDcQAQAAAACiU5JbQBYgEBABoChAASAyMzEQBuMBsAIQNxABkAAAAKJTkltwFiAQEAGgKfABID"
"IzMRAG4wGwAhA3EAAwAAAAolOSW6AWIBAQAaAksAEgMjMxEAbjAbACEDcQACAAAACiU5Jb0BYgEB"
"ABoCSAASAyMzEQBuMBsAIQMUAXARAQAS0nEgBgAhAAolOSW8AWIBAQAaAl4AEgMjMxEAbjAbACED"
"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"
"EvUUBP///w8SwBLDFAH8/wAAEsKNQI9DjmHiAgIEMlAEABIUDwQyUwQAEiQo/DJhBAASNCj4MkIE"
"ABJEKPQSBCjyAAACAAEAAAAAAIUpAAADAAAAUhAAAA8AAAACAAIAAAAAAIspAAADAAAAWQEAAA4A"
"AAADAAIAAAAAAJMpAAADAAAA2gACAg8AAAAEAAAAAgACAAIAAgAEAAAAAwADAAMAAwABAAAABAAA"
"AAIAAAAEAAQABAAAAAQABAAEAAQAGwAAAAQABQAEAAUABAAFAAQABAACAAMAAgAMAAQAAQAEAAQA"
"AAAOAAQABAAFAAUABAAEAAQABAAEAAAAAgAAAAUABQAEAAAABQAFAAUABQACAAAABQAEAAIAAAAK"
"ABEAAQAAABIABjxpbml0PgABQgABQwABRAABRgABSQAFSUREREQABUlGRkZGAAJJSQADSUlJAAVJ"
"SUlJSQAcSUlKSUpJSklJREZEU0lDSUlCWklJSkpJSUlJSQADSUpKAAVJSkpKSgAMSW50TWF0aC5q"
"YXZhAAFKAANKSkkAAkxJAAlMSW50TWF0aDsAA0xMTAAVTGphdmEvaW8vUHJpbnRTdHJlYW07ABNM"
"amF2YS9sYW5nL0ludGVnZXI7ABJMamF2YS9sYW5nL09iamVjdDsAEkxqYXZhL2xhbmcvU3RyaW5n"
"OwASTGphdmEvbGFuZy9TeXN0ZW07AAtPYmplY3QuamF2YQABUwABVgACVkkAAlZMAAFaAAJbSQAC"
"W0oAE1tMamF2YS9sYW5nL09iamVjdDsAE1tMamF2YS9sYW5nL1N0cmluZzsAAWEAAmEwAAJhMQAD"
"YTEwAANhMTEAA2ExMgADYTEzAANhMTQAA2ExNQADYTE2AANhMTcAA2ExOAADYTE5AAJhMgADYTIw"
"AANhMjEAA2EyMgADYTIzAANhMjQAA2EyNQADYTI2AAJhMwACYTQAAmE1AAJhNgACYTcAAmE4AAJh"
"OQAJYWxzb01pbnVzAAhhbHNvUGx1cwAEYXJncwABYgABYwAFY2hhcjEABWNoYXIyAAtjaGFyU3Vi"
"VGVzdAAXY2hhclN1YlRlc3QgRkFJTEVEOiAlZAoAE2NoYXJTdWJUZXN0IFBBU1NFRAoACGNvbnZU"
"ZXN0ABRjb252VGVzdCBGQUlMRUQ6ICVkCgAQY29udlRlc3QgUEFTU0VECgABZAAEZGlzdAABZQAB"
"ZgAJZmlib25hY2NpABVmaWJvbmFjY2kgRkFJTEVEOiAlZAoAEWZpYm9uYWNjaSBQQVNTRUQKAANm"
"b28ABmZvbzEyMwAEZm9vXwABZwAGZ2V0Rm9vAAFoAAFpAAJpMQACaTIAC2ludE9wZXJUZXN0ABdp"
"bnRPcGVyVGVzdCBGQUlMRUQ6ICVkCgATaW50T3BlclRlc3QgUEFTU0VECgAMaW50U2hpZnRUZXN0"
"AAFsAAlsaXQxNlRlc3QACGxpdDhUZXN0AAxsb25nT3BlclRlc3QAGGxvbmdPcGVyVGVzdCBGQUlM"
"RUQ6ICVkCgAUbG9uZ09wZXJUZXN0IFBBU1NFRAoADWxvbmdTaGlmdFRlc3QAGWxvbmdTaGlmdFRl"
"c3QgRkFJTEVEOiAlZAoAFWxvbmdTaGlmdFRlc3QgUEFTU0VECgAEbHJlcwAGbUJ5dGVzAARtYWlu"
"AAhtYW55QXJncwAUbWFueUFyZ3MgRkFJTEVEOiAlZAoAEG1hbnlBcmdzIFBBU1NFRAoABm1pbklu"
"dAAHbWluTG9uZwAFbWludXMAAW4AA25hbgAGbmVnT25lAANvdXQABHBsdXMABXBsdXMyAAdwbHVz"
"T25lAAZwcmludGYAA3JlcwAGcmVzdWx0AAdyZXN1bHRzAAFzAAZzZXRGb28ACnNoaWZ0VGVzdDEA"
"FnNoaWZ0VGVzdDEgRkFJTEVEOiAlZAoAEnNoaWZ0VGVzdDEgUEFTU0VECgAKc2hpZnRUZXN0MgAW"
"c2hpZnRUZXN0MiBGQUlMRUQ6ICVkCgASc2hpZnRUZXN0MiBQQVNTRUQKAApzdGF0aWNDYWxsAAVz"
"dHVmZgAKc3dpdGNoVGVzdAAWc3dpdGNoVGVzdCBGQUlMRUQ6ICVkCgASc3dpdGNoVGVzdCBQQVNT"
"RUQKABF0ZXN0RG91YmxlQ29tcGFyZQAddGVzdERvdWJsZUNvbXBhcmUgRkFJTEVEOiAlZAoAGXRl"
"c3REb3VibGVDb21wYXJlIFBBU1NFRAoAEHRlc3RGbG9hdENvbXBhcmUAHHRlc3RGbG9hdENvbXBh"
"cmUgRkFJTEVEOiAlZAoAGHRlc3RGbG9hdENvbXBhcmUgUEFTU0VECgAWdGVzdEdldFB1dCBGQUlM"
"RUQ6ICVkCgASdGVzdEdldFB1dCBQQVNTRUQKAAt0ZXN0SUdldFB1dAAOdGVzdEludENvbXBhcmUA"
"GnRlc3RJbnRDb21wYXJlIEZBSUxFRDogJWQKABZ0ZXN0SW50Q29tcGFyZSBQQVNTRUQKAA90ZXN0"
"TG9uZ0NvbXBhcmUAG3Rlc3RMb25nQ29tcGFyZSBGQUlMRUQ6ICVkCgAXdGVzdExvbmdDb21wYXJl"
"IFBBU1NFRAoABHRoaXMACHVub3BUZXN0ABR1bm9wVGVzdCBGQUlMRUQ6ICVkCgAQdW5vcFRlc3Qg"
"UEFTU0VECgARdW5zaWduZWRTaGlmdFRlc3QAHXVuc2lnbmVkU2hpZnRUZXN0IEZBSUxFRDogJWQK"
"ABl1bnNpZ25lZFNoaWZ0VGVzdCBQQVNTRUQKAAV2YWx1ZQAHdmFsdWVPZgALdmlydHVhbENhbGwA"
"F3ZpcnR1YWxDYWxsIEZBSUxFRDogJWQKABN2aXJ0dWFsQ2FsbCBQQVNTRUQKAAF4AAF5AAF6AAR6"
"ZXJvAAMABw4ACQAHDjxLAAUBhwEHDjwtAHwABw4tAwBFAj8DAUYCLQMCWgVpAGcABw4tAwBaBR4D"
"AWEGAg53AnQdLR6JWh55Wh54AIoEAXMHHS0hGi8AiwECpgGnAQdZTQMEfRBLS0tLWlpaWmvjLQMA"
"cAU8AwF1BR4DAnkFWwMDfAUCDCwCdR2WlpaWlmm0lsPTAO8BAqEBTgdZLQMAfRBLS0t4exqWlqUA"
"tQEBpgEHWU0DAH0QS0tLS0taWlt/AnkdlpaHh5aWwwDRAQGmAQdZTQMCfRBLS0tLS1paWi0DAHAF"
"LQMBfAU1AngdlpaWh3iWlsMAgAICpgGnAQcOTQMIfRFaWlpaWmlaWmriLQMAcQZLAwJ1Bi0DBHkG"
"aQMGfAYCDFkCdR3h4eHh4bTww/8BEQ+WAKYCAqEBTgdZLQMAfRFLS0t41wJ7HeHh4WoA5gQBQgcO"
"aQMlewVpqEstqEstqEstqEstqEstqIctqLQtqLQDI2oGlqlaaamHaanwaanDaanDaalpaQIOpAE1"
"EVqpWlqpaWmoAoR/HQUjARMUARMUARMUARMUARMUARMUARMUARMUBiMBExUBExUBExUBExUBExUB"
"ExUCEgETDgETFQETFQCnBBslJjE5Ojs8PT4/JygpKissLS4vMDIzNDU2NzgHDgIbOwJmHYdah1qH"
"WlqWlpaWlpaWlpaWpaWlpaWlpaWlABcAB1l9AwRrEAIbhgJmHZaWlpalpaYBEg8DAFsFARUPAwFc"
"BXkDAmEGllrEATgX0gJfLAA9AAcOLQMAJAYtAwJDBi0DBEQGLQMGTQYtAwhPBi0DClAGLQMMVwYu"
"Aw5ZBgEqEQMQfAa0ANYEASQHDloDAFQHALUCASQHDi4DAHsFQgIoLAUAAwF7BQJSHQUBBgABAgUA"
"BgEeBQEGAAECBQAGAR8FAQYAAQIFAAYBHgUBBgABAgUABgEeBQEGAAECBQAGASAFAQYAQT0+AQIF"
"AAYBAnYdBQEGAAECBQAGAR4FAQYAAQMFAAYBJQUBBgABAwUABgEgBQEGAEEBAwUABgECex0FAQYA"
"AQMFAAYBHgUBBgABAwUABgEfBQEGAAEDBQAGAR4FAQYAAQMFAAYBIAUBBgBBHj8BAwUABgECdh0F"
"AQYAAQMFAAYBHgUBBgABAwUABgEjBQEGAAEDBQAGAR8FAQYAAQMFAAYBAQEFAQYAJAUABgECUR0B"
"EBeOiwEWFqsA7AMEcnd4dAcOLgMAewVLAhcdBQACah0GAEstSy1LLkstSy1LLUs8SzxLPUsAzAME"
"cnd4dAcOLQMAewVLHkseSx5LH0seSx5LHkstSy1LLksuANwEASQHDngDAFQHWgMBVQdLAwKoAQUe"
"WjwA7AIEcnd4qQEHDi4DAHsFLQIlHQJcHS0tLS0tLS0tLS8yLTcxLTwtPC08LTwtPUAbAJ4DBHJA"
"d0EHLC4DAHsFSwIiHQJfHUstSy5pLWkvUUsCezsySzxLPks8SzxLTAARAaYBBw4eLQBOAAdoHgMA"
"QwEeAwN+DTwDAUQCHwMCWgUeHh4uPxpLS0sA0QQABw4AzAQBJAcOLQDHBAEkBw4AAAABAB2BgATU"
"CgABGAMAAgCBgAToCgGBgASICwEIpAsBCNQLAQjIDAIIhA0BCPQPAQiEEQEIgBMBCIgVAQj0GAEJ"
"vBoBCOgmAgjIKgEIzC4BCPAvAQiUMAEIhDMBCKQ0AQiwNQEI/DUBCJQ3AQjUOAEI7DgFAMQ5CQDc"
"OQwA9DkAAA0AAAAAAAAAAQAAAAAAAAABAAAAqQAAAHAAAAACAAAAEwAAABQDAAADAAAADwAAAGAD"
"AAAEAAAAAgAAABQEAAAFAAAAHgAAACQEAAAGAAAAAgAAABQFAAABIAAAHAAAAFQFAAABEAAACwAA"
"AAwdAAACIAAAqQAAAKYdAAADIAAAHAAAAPokAAAAIAAAAgAAAJopAAAAEAAAAQAAABwqAAA=";
}  // namespace art
