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
//    }
//}
static const char kIntMathDex[] =
  "ZGV4CjAzNQA6SlSLdo8mIHPp83OcIPmcKOauGDHMXglYHQAAcAAAAHhWNBIAAAAAAAAAALgcAABO"
  "AAAAcAAAAA8AAACoAQAADQAAAOQBAAABAAAAgAIAABYAAACIAgAAAgAAADgDAADgGQAAeAMAAFoV"
  "AABiFQAAZRUAAGgVAABrFQAAchUAAHkVAAB9FQAAghUAAIkVAACOFQAAlRUAAKMVAACmFQAAqxUA"
  "AK8VAAC6FQAAvxUAANYVAADrFQAA/xUAABMWAAAnFgAANBYAADcWAAA7FgAAPxYAAEMWAABYFgAA"
  "bRYAAHoWAACTFgAAqBYAALIWAADIFgAA2hYAAOcWAAAAFwAAFRcAACMXAAAuFwAAOBcAAEYXAABg"
  "FwAAdhcAAIUXAACgFwAAtxcAAL0XAADCFwAAyhcAANYXAADuFwAAAhgAAA4YAAAmGAAAOhgAAEYY"
  "AABeGAAAchgAAIUYAACkGAAAvxgAANEYAADvGAAACRkAABkZAAA1GQAATRkAAF4ZAAB7GQAAlBkA"
  "AJ4ZAAC0GQAAxhkAANkZAAD4GQAAExoAAAEAAAACAAAAAwAAAAwAAAAPAAAAEQAAABIAAAATAAAA"
  "FAAAABUAAAAXAAAAGQAAABoAAAAbAAAAHAAAAAMAAAACAAAAAAAAAAQAAAACAAAA/BQAAAUAAAAC"
  "AAAACBUAAAYAAAACAAAAFBUAAAcAAAACAAAAHBUAAAgAAAACAAAAJBUAAAkAAAACAAAAMBUAAAoA"
  "AAACAAAAOBUAAA0AAAADAAAARBUAABAAAAAFAAAATBUAAA4AAAAGAAAAFBUAABcAAAAKAAAAAAAA"
  "ABgAAAAKAAAAVBUAAAkABQAwAAAABAALAAAAAAAEAAAAHQAAAAQAAAAgAAAABAAEACMAAAAEAAQA"
  "JgAAAAQAAwAnAAAABAADACgAAAAEAAYAKQAAAAQACAAsAAAABAAMAC8AAAAEAAAAMgAAAAQAAAA1"
  "AAAABAADADgAAAAEAAEAOwAAAAQAAgA+AAAABAAFAEEAAAAEAAcARAAAAAQAAwBHAAAABAAAAEoA"
  "AAAFAAkAMQAAAAYACgBNAAAABwALAAAAAAAHAAAAAQAAAP////8AAAAAFgAAAAAAAABbHAAAAAAA"
  "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"
  "IBMCEACjAgoCwSATAggAowIMAsEgweAYAoh3ZlVEMyIRMQAAAjgABAASEA8AEgAo/gQAAQAAAAAA"
  "dhsAAJoAAAASEBMB0gQrA0kAAAASYA8AEiAo/hIwKPwSQCj6ElAo+CsDTAAAABQCeFY0EisCTgAA"
  "ACjuLAJSAAAAEwAOACjoEnAo5hMACAAo4xMACQAo4BMACgAo3RMACwAo2hMADAAo1xMADQAo1CwC"
  "TgAAABKwLABUAAAAEwATACjKEwAPACjHEwAQACjEEwARACjBEwASACi+ARAovAABBgD/////BAAA"
  "AAUAAAANAAAABwAAAAkAAAALAAAAAAECAAMAAAAQAAAAEgAAAAABAgB4VjQSBAAAAA8AAAAAAgUA"
  "+v///wMAAAAWAAAAOQAAAHhWNBIRAAAAFwAAABQAAAAOAAAAGgAAAAACAgD6////AwAAAAoAAAAN"
  "AAAAAAIDAPv///8AAAAADAAAABIAAAAPAAAADAAAAAoACAAAAAAArRsAAEgAAAATAFwRLwECBD0B"
  "BAASEA8AMAEEAjsBBAASICj6LwEEAjkBBAASMCj0LwEEBjgBBAASQCjuMAEECDwBBAASUCjoLwEE"
  "CDoBBAASYCjiMAECCDwBBAAScCjcLwECCDoBBQATAAgAKNUvAQgEOgEFABMACQAozjABCAQ8AQUA"
  "EwAKACjHLwEICDkBxP8TAAsAKMAGAAQAAAAAANIbAAA+AAAAEwAFDS0BAgM9AQMAEhAuAQMCOwED"
  "ABIgLQEDAjkBAwASMC0BAwQ4AQMAEkAuAQMFPAEDABJQLQEDBToBAwASYC4BAgU8AQMAEnAtAQIF"
  "OgEEABMACAAtAQUDOgEEABMACQAuAQUDPAEEABMACgAtAQUFOQEEABMACwAPAAUABAAAAAAA8xsA"
  "AEEAAAATAFcENyEEABIQDwA0IQQAEiAo/DUSBAASMCj4NhIEABJAKPQzEgQAElAo8DIyBAASYCjs"
  "MhIHAD0BBwATAAgAKOUScCjjOgEFABMACQAo3jsCBQATAAoAKNk8AgUAEwALACjUOQIFABMADAAo"
  "zzgEBQATAA0AKMo4BMn/EwAOACjFAAANAAgAAAAAAB4cAABWAAAAFgMBABMArggxAQUJPQEEABIg"
  "DwAxAQkFOwEEABIwKPoxAQkFOQEEABJAKPSbAQkDMQEJAToBBAASUCjsmwEFAzEBBQE6AQQAEmAo"
  "5DEBCQU4AQkAMQEJCz0BBwATAAgAKNkScCjXMQELCTsBBQATAAkAKNAxAQsJOQEFABMACgAoyTEB"
  "BQc7AQUAEwALACjCMQEHBT0BBQATAAwAKLsxAQcFOQG4/xMADQAotAIAAQAAAAAARxwAAAQAAAB7"
  "EN8AAP8PAAUAAAAAAAAATxwAABcAAAAUAf///w8TBP8PEvONEI8RjkIyMAQAEhAPADIxBAASICj8"
  "MkIEABIwKPgSACj2AAAEAAAAAAAAAAAAAAAEAAAAAQABAAEAAQABAAAAAgAAAAIAAAACAAIABAAA"
  "AAIAAgACAAIAAgAAAAMAAwAEAAAAAwADAAMAAwACAAAAAwACAAIAAAAIAA0AAQAAAA4ABjxpbml0"
  "PgABRAABRgABSQAFSUREREQABUlGRkZGAAJJSQADSUlJAAVJSUlJSQADSUpKAAVJSkpKSgAMSW50"
  "TWF0aC5qYXZhAAFKAANKSkkAAkxJAAlMSW50TWF0aDsAA0xMTAAVTGphdmEvaW8vUHJpbnRTdHJl"
  "YW07ABNMamF2YS9sYW5nL0ludGVnZXI7ABJMamF2YS9sYW5nL09iamVjdDsAEkxqYXZhL2xhbmcv"
  "U3RyaW5nOwASTGphdmEvbGFuZy9TeXN0ZW07AAtPYmplY3QuamF2YQABVgACVkwAAltJAAJbSgAT"
  "W0xqYXZhL2xhbmcvT2JqZWN0OwATW0xqYXZhL2xhbmcvU3RyaW5nOwALY2hhclN1YlRlc3QAF2No"
  "YXJTdWJUZXN0IEZBSUxFRDogJWQKABNjaGFyU3ViVGVzdCBQQVNTRUQKAAhjb252VGVzdAAUY29u"
  "dlRlc3QgRkFJTEVEOiAlZAoAEGNvbnZUZXN0IFBBU1NFRAoAC2ludE9wZXJUZXN0ABdpbnRPcGVy"
  "VGVzdCBGQUlMRUQ6ICVkCgATaW50T3BlclRlc3QgUEFTU0VECgAMaW50U2hpZnRUZXN0AAlsaXQx"
  "NlRlc3QACGxpdDhUZXN0AAxsb25nT3BlclRlc3QAGGxvbmdPcGVyVGVzdCBGQUlMRUQ6ICVkCgAU"
  "bG9uZ09wZXJUZXN0IFBBU1NFRAoADWxvbmdTaGlmdFRlc3QAGWxvbmdTaGlmdFRlc3QgRkFJTEVE"
  "OiAlZAoAFWxvbmdTaGlmdFRlc3QgUEFTU0VECgAEbWFpbgADb3V0AAZwcmludGYACnNoaWZ0VGVz"
  "dDEAFnNoaWZ0VGVzdDEgRkFJTEVEOiAlZAoAEnNoaWZ0VGVzdDEgUEFTU0VECgAKc2hpZnRUZXN0"
  "MgAWc2hpZnRUZXN0MiBGQUlMRUQ6ICVkCgASc2hpZnRUZXN0MiBQQVNTRUQKAApzd2l0Y2hUZXN0"
  "ABZzd2l0Y2hUZXN0IEZBSUxFRDogJWQKABJzd2l0Y2hUZXN0IFBBU1NFRAoAEXRlc3REb3VibGVD"
  "b21wYXJlAB10ZXN0RG91YmxlQ29tcGFyZSBGQUlMRUQ6ICVkCgAZdGVzdERvdWJsZUNvbXBhcmUg"
  "UEFTU0VECgAQdGVzdEZsb2F0Q29tcGFyZQAcdGVzdEZsb2F0Q29tcGFyZSBGQUlMRUQ6ICVkCgAY"
  "dGVzdEZsb2F0Q29tcGFyZSBQQVNTRUQKAA50ZXN0SW50Q29tcGFyZQAadGVzdEludENvbXBhcmUg"
  "RkFJTEVEOiAlZAoAFnRlc3RJbnRDb21wYXJlIFBBU1NFRAoAD3Rlc3RMb25nQ29tcGFyZQAbdGVz"
  "dExvbmdDb21wYXJlIEZBSUxFRDogJWQKABd0ZXN0TG9uZ0NvbXBhcmUgUEFTU0VECgAIdW5vcFRl"
  "c3QAFHVub3BUZXN0IEZBSUxFRDogJWQKABB1bm9wVGVzdCBQQVNTRUQKABF1bnNpZ25lZFNoaWZ0"
  "VGVzdAAddW5zaWduZWRTaGlmdFRlc3QgRkFJTEVEOiAlZAoAGXVuc2lnbmVkU2hpZnRUZXN0IFBB"
  "U1NFRAoAB3ZhbHVlT2YAAwAHDgABAAcOAHAABw4AVAAHDi0eAg53AnQdLR6JWh55Wh54AHgCAAAH"
  "WU1LS0tLWlpaWmvjLT1bAgwsAnUdlpaWlpZptJbD0wDcAQIAAAdZLUtLS3h7GpaWpQCiAQEAB1lN"
  "S0tLS0taWlt/AnkdlpaHh5aWwwC+AQEAB1lNS0tLS0taWl1/AnkdlpaHeJaWwwDtAQIAAAcOTVpa"
  "WlpaaVpaauItSy14AgxZAnUd4eHh4eG08MP/AREPlgCTAgIAAAdZLUtLS3jXAnsd4eHhagD5AwEA"
  "B3dpS5lLLZlLLZlLLZlLLZlLLZmHLZm0LZm0lppLS5paS5rwS5qHS5qlS5kCq38dAREUAREUAREU"
  "AREUAREUAREUAREUAREUAREVAREVAREVAREVAREVAA0AB1l9AREPARQPagIOWQJzHXi1ATcXwwJo"
  "HQAqAAcOLS0tLS0tLS4BIxGlAKICAQAHHS5CAigdAlMdLi0tL0E9T0ECbjstQz88PTw+QR4/AnY7"
  "PEE9UQJRHQEQF46LARYWqwDaAwQAAAAABw4uSwIXHQJqHUstSy1LLkstSy1LLUs8SzxLPUsAuQME"
  "AAAAAAcOLkseSx5LHksfSx5LHkseSy1LLUsuSy4A2QIEAAAAAAcOLi0CJR0CXB0tLS0tLS0tLS0v"
  "Mi03MS08LTwtPC08LT0vAIsDBAAAAAAHLC5LAiIdAl8dSy1LLmktaS9RSwJ7OzJLPEs+SzxLPEsA"
  "BwEABw4eLQBAAAdoHh4gPxpLTAAAAAEAFYGABPgGAAATAACAgASMBwEIpAcBCLgHAQisCAEInAsB"
  "CKwMAQioDgEIoBABCJAUAQnYFQEI9B0BCNQgAQjkIQEIqCQBCMglAQjUJgEI6CcBCKQpAQi8KQAN"
  "AAAAAAAAAAEAAAAAAAAAAQAAAE4AAABwAAAAAgAAAA8AAACoAQAAAwAAAA0AAADkAQAABAAAAAEA"
  "AACAAgAABQAAABYAAACIAgAABgAAAAIAAAA4AwAAASAAABQAAAB4AwAAARAAAAoAAAD8FAAAAiAA"
  "AE4AAABaFQAAAyAAABQAAAAcGgAAACAAAAIAAABbHAAAABAAAAEAAAC4HAAA";
}  // namespace art
