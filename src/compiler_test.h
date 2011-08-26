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
//        i1 = mBytes[0] | mBytes[1] << 8 | mBytes[2] << 16 | mBytes[3] << 24;
//        i2 = mBytes[4] | mBytes[5] << 8 | mBytes[6] << 16 | mBytes[7] << 24;
//        l = i1 | ((long)i2 << 32);
//
//        if (i1 != 0x44332211) { return 1; }
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
//     * Shift some data.  (value=0xd5aa96deff00aa01, dist=8)
//     */
//    static long longShiftTest(long value, int dist) {
//        long results[] = new long[4];
//        results[0] = value << dist;
//        results[1] = value >> dist;
//        results[2] = value >>> dist;
//        results[3] = (((value << dist) >> dist) >>> dist) << dist;
//        if (results[0] != 0x96deff00aa010000L) { return 1; }
//        if (results[1] != 0xffffd5aa96deff00L) { return 2; }
//        if (results[2] != 0x0000d5aa96deff00L) { return 3; }
//        if (results[3] != 0xffff96deff000000L) { return 4; }
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
//
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
//            res = 11;
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
//    }
//}
static const char kIntMathDex[] =
"ZGV4CjAzNQDkd6TZ8pqDxU8AeTvdZuEG1XSONR7DLLU8HgAAcAAAAHhWNBIAAAAAAAAAAJwdAABR"
"AAAAcAAAAA8AAAC0AQAADQAAAPABAAABAAAAjAIAABcAAACUAgAAAgAAAEwDAACwGgAAjAMAAPIV"
"AAD6FQAA/RUAAAAWAAADFgAAChYAABEWAAAVFgAAGhYAACEWAAAmFgAALRYAADsWAAA+FgAAQxYA"
"AEcWAABSFgAAVxYAAG4WAACDFgAAlxYAAKsWAAC/FgAAzBYAAM8WAADTFgAA1xYAANsWAADwFgAA"
"BRcAABIXAAArFwAAQBcAAEoXAABgFwAAchcAAH0XAACUFwAApxcAALQXAADNFwAA4hcAAPAXAAD7"
"FwAABRgAABMYAAAtGAAAQxgAAFIYAABtGAAAhBgAAIoYAACPGAAAlxgAAKMYAAC7GAAAzxgAANsY"
"AADzGAAABxkAABMZAAArGQAAPxkAAFIZAABxGQAAjBkAAJ4ZAAC8GQAA1hkAAOYZAAACGgAAGhoA"
"ACsaAABIGgAAYRoAAGsaAACBGgAAkxoAAKYaAADFGgAA4BoAAAEAAAACAAAAAwAAAAwAAAAPAAAA"
"EQAAABIAAAATAAAAFAAAABUAAAAXAAAAGQAAABoAAAAbAAAAHAAAAAMAAAACAAAAAAAAAAQAAAAC"
"AAAAlBUAAAUAAAACAAAAoBUAAAYAAAACAAAArBUAAAcAAAACAAAAtBUAAAgAAAACAAAAvBUAAAkA"
"AAACAAAAyBUAAAoAAAACAAAA0BUAAA0AAAADAAAA3BUAABAAAAAFAAAA5BUAAA4AAAAGAAAArBUA"
"ABcAAAAKAAAAAAAAABgAAAAKAAAA7BUAAAkABQAzAAAABAALAAAAAAAEAAAAHQAAAAQAAAAgAAAA"
"BAADACMAAAAEAAQAJgAAAAQABAApAAAABAADACoAAAAEAAMAKwAAAAQABgAsAAAABAAIAC8AAAAE"
"AAwAMgAAAAQAAAA1AAAABAAAADgAAAAEAAMAOwAAAAQAAQA+AAAABAACAEEAAAAEAAUARAAAAAQA"
"BwBHAAAABAADAEoAAAAEAAAATQAAAAUACQA0AAAABgAKAFAAAAAHAAsAAAAAAAcAAAABAAAA////"
"/wAAAAAWAAAAAAAAADkdAAAAAAAABAAAAAAAAAAHAAAAAAAAAAsAAAAAAAAAQx0AAAAAAAABAAEA"
"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"
"FwAAABQB////DxME/w8S840QjxGOQjIwBAASEA8AMjEEABIgKPwyQgQAEjAo+BIAKPYAAAQAAAAA"
"AAAAAAAAAAQAAAABAAEAAQABAAEAAAACAAAAAgAAAAIAAgAEAAAAAgACAAIAAgACAAAAAwADAAQA"
"AAADAAMAAwADAAIAAAADAAIAAgAAAAgADQABAAAADgAGPGluaXQ+AAFEAAFGAAFJAAVJRERERAAF"
"SUZGRkYAAklJAANJSUkABUlJSUlJAANJSkoABUlKSkpKAAxJbnRNYXRoLmphdmEAAUoAA0pKSQAC"
"TEkACUxJbnRNYXRoOwADTExMABVMamF2YS9pby9QcmludFN0cmVhbTsAE0xqYXZhL2xhbmcvSW50"
"ZWdlcjsAEkxqYXZhL2xhbmcvT2JqZWN0OwASTGphdmEvbGFuZy9TdHJpbmc7ABJMamF2YS9sYW5n"
"L1N5c3RlbTsAC09iamVjdC5qYXZhAAFWAAJWTAACW0kAAltKABNbTGphdmEvbGFuZy9PYmplY3Q7"
"ABNbTGphdmEvbGFuZy9TdHJpbmc7AAtjaGFyU3ViVGVzdAAXY2hhclN1YlRlc3QgRkFJTEVEOiAl"
"ZAoAE2NoYXJTdWJUZXN0IFBBU1NFRAoACGNvbnZUZXN0ABRjb252VGVzdCBGQUlMRUQ6ICVkCgAQ"
"Y29udlRlc3QgUEFTU0VECgAJZmlib25hY2NpABVmaWJvbmFjY2kgRkFJTEVEOiAlZAoAEWZpYm9u"
"YWNjaSBQQVNTRUQKAAtpbnRPcGVyVGVzdAAXaW50T3BlclRlc3QgRkFJTEVEOiAlZAoAE2ludE9w"
"ZXJUZXN0IFBBU1NFRAoADGludFNoaWZ0VGVzdAAJbGl0MTZUZXN0AAhsaXQ4VGVzdAAMbG9uZ09w"
"ZXJUZXN0ABhsb25nT3BlclRlc3QgRkFJTEVEOiAlZAoAFGxvbmdPcGVyVGVzdCBQQVNTRUQKAA1s"
"b25nU2hpZnRUZXN0ABlsb25nU2hpZnRUZXN0IEZBSUxFRDogJWQKABVsb25nU2hpZnRUZXN0IFBB"
"U1NFRAoABG1haW4AA291dAAGcHJpbnRmAApzaGlmdFRlc3QxABZzaGlmdFRlc3QxIEZBSUxFRDog"
"JWQKABJzaGlmdFRlc3QxIFBBU1NFRAoACnNoaWZ0VGVzdDIAFnNoaWZ0VGVzdDIgRkFJTEVEOiAl"
"ZAoAEnNoaWZ0VGVzdDIgUEFTU0VECgAKc3dpdGNoVGVzdAAWc3dpdGNoVGVzdCBGQUlMRUQ6ICVk"
"CgASc3dpdGNoVGVzdCBQQVNTRUQKABF0ZXN0RG91YmxlQ29tcGFyZQAddGVzdERvdWJsZUNvbXBh"
"cmUgRkFJTEVEOiAlZAoAGXRlc3REb3VibGVDb21wYXJlIFBBU1NFRAoAEHRlc3RGbG9hdENvbXBh"
"cmUAHHRlc3RGbG9hdENvbXBhcmUgRkFJTEVEOiAlZAoAGHRlc3RGbG9hdENvbXBhcmUgUEFTU0VE"
"CgAOdGVzdEludENvbXBhcmUAGnRlc3RJbnRDb21wYXJlIEZBSUxFRDogJWQKABZ0ZXN0SW50Q29t"
"cGFyZSBQQVNTRUQKAA90ZXN0TG9uZ0NvbXBhcmUAG3Rlc3RMb25nQ29tcGFyZSBGQUlMRUQ6ICVk"
"CgAXdGVzdExvbmdDb21wYXJlIFBBU1NFRAoACHVub3BUZXN0ABR1bm9wVGVzdCBGQUlMRUQ6ICVk"
"CgAQdW5vcFRlc3QgUEFTU0VECgARdW5zaWduZWRTaGlmdFRlc3QAHXVuc2lnbmVkU2hpZnRUZXN0"
"IEZBSUxFRDogJWQKABl1bnNpZ25lZFNoaWZ0VGVzdCBQQVNTRUQKAAd2YWx1ZU9mAAMABw4AAQAH"
"DgBwAAcOAFQABw4tHgIOdwJ0HS0eiVoeeVoeeAD4AwEABx0tIRovAHgCAAAHWU1LS0tLWlpaWmvj"
"LT1bAgwsAnUdlpaWlpZptJbD0wDcAQIAAAdZLUtLS3h7GpaWpQCiAQEAB1lNS0tLS0taWlt/Ankd"
"lpaHh5aWwwC+AQEAB1lNS0tLS0taWl1/AnkdlpaHeJaWwwDtAQIAAAcOTVpaWlpaaVpaauItSy14"
"AgxZAnUd4eHh4eG08MP/AREPlgCTAgIAAAdZLUtLS3jXAnsd4eHhagCCBAEAB3dpS5lLLZlLLZlL"
"LZlLLZlLLZmHLZm0LZm0lppLS5paS5rwS5qHS5qlS5ppS5kCpH8dAREUAREUAREUAREUAREUAREU"
"AREUAREUAREVAREVAREVAREVAREVAREVAA0AB1l9AREPARQPagIOWQJzHXi1ATcXwwJoHQAqAAcO"
"LS0tLS0tLS4BIxGlAKICAQAHHS5CAigdAlMdLi0tL0E9T0ECbjstQz88PTw+QR4/AnY7PEE9UQJR"
"HQEQF46LARYWqwDaAwQAAAAABw4uSwIXHQJqHUstSy1LLkstSy1LLUs8SzxLPUsAuQMEAAAAAAcO"
"LkseSx5LHksfSx5LHkseSy1LLUsuSy4A2QIEAAAAAAcOLi0CJR0CXB0tLS0tLS0tLS0vMi03MS08"
"LTwtPC08LT0vAIsDBAAAAAAHLC5LAiIdAl8dSy1LLmktaS9RSwJ7OzJLPEs+SzxLPEsABwEABw4e"
"LQBAAAdoHh4gPxpLTAAAAAEAFoGABIwHAAAUAACAgASgBwEIuAcBCMwHAQjACAEI/AgBCOwLAQj8"
"DAEI+A4BCPAQAQjgFAEJqBYBCIwfAQjsIQEI/CIBCMAlAQjgJgEI7CcBCIApAQi8KgEI1CoAAAAN"
"AAAAAAAAAAEAAAAAAAAAAQAAAFEAAABwAAAAAgAAAA8AAAC0AQAAAwAAAA0AAADwAQAABAAAAAEA"
"AACMAgAABQAAABcAAACUAgAABgAAAAIAAABMAwAAASAAABUAAACMAwAAARAAAAoAAACUFQAAAiAA"
"AFEAAADyFQAAAyAAABUAAADpGgAAACAAAAIAAAA5HQAAABAAAAEAAACcHQAA";

}  // namespace art
