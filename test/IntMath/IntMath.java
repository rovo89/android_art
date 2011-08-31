// Copyright 2011 Google Inc. All Rights Reserved.

class IntMath {

    public static boolean mBoolean1, mBoolean2;
    public static byte mByte1, mByte2;
    public static char mChar1, mChar2;
    public static short mShort1, mShort2;
    public static int mInt1, mInt2;
    public static float mFloat1, mFloat2;
    public static long mLong1, mLong2;
    public static double mDouble1, mDouble2;
    public static volatile long mVolatileLong1, mVolatileLong2;


    private int foo_;

    public IntMath(int stuff) {
        foo_ = stuff;
    }

    public IntMath() {
        foo_ = 123;
    }

    static int constStringTest(int x) {
        /* TODO: flesh this test out when we can call string library */
        String str = "Hello World!";
        return x * 2;
    }

    static void throwNullPointerException() {
        throw new NullPointerException();
    }

    static int catchBlock(int x) {
        try {
            x += 123;
            throwNullPointerException();
        } catch (NullPointerException npe) {
            x += 456;
        }
        return x;
    }

    static int staticFieldTest(int x) {
        mBoolean1 = true;
        mBoolean2 = false;
        mByte1 = 127;
        mByte2 = -128;
        mChar1 = 32767;
        mChar2 = 65535;
        mShort1 = 32767;
        mShort2 = -32768;
        mInt1 = 65537;
        mInt2 = -65537;
        mFloat1 = 3.1415f;
        mFloat2 = -1.0f / 0.0f;                // -inf
        mLong1 = 1234605616436508552L;     // 0x1122334455667788
        mLong2 = -1234605616436508552L;
        mDouble1 = 3.1415926535;
        mDouble2 = 1.0 / 0.0;               // +inf
        mVolatileLong1 = mLong1 - 1;
        mVolatileLong2 = mLong2 + 1;

        if (!mBoolean1) { return 10; }
        if (mBoolean2) { return 11; }
        if (mByte1 != 127) { return 12; }
        if (mByte2 != -128) { return 13; }
        if (mChar1 != 32767) { return 14; }
        if (mChar2 != 65535) { return 15; }
        if (mShort1 != 32767) { return 16; }
        if (mShort2 != -32768) { return 17; }
        if (mInt1 != 65537) { return 18; }
        if (mInt2 != -65537) { return 19; }
        if (!(mFloat1 > 3.141f && mFloat1 < 3.142f)) { return 20; }
        if (mFloat2 >= mFloat1) { return 21; }
        if (mLong1 != 1234605616436508552L) { return 22; }
        if (mLong2 != -1234605616436508552L) { return 23; }
        if (!(mDouble1 > 3.141592653 && mDouble1 < 3.141592654)) { return 24; }
        if (mDouble2 <= mDouble1) { return 25; }
        if (mVolatileLong1 != 1234605616436508551L) { return 26; }
        if (mVolatileLong2 != -1234605616436508551L) { return 27; }

        return 1000 + x;
    }

    /*
     * Try to cause some unary operations.
     */
    static int unopTest(int x) {
        x = -x;
        x ^= 0xffffffff;
        return x;
    }

    static int shiftTest1() {
        final int[] mBytes = {
            0x11, 0x22, 0x33, 0x44, 0x88, 0x99, 0xaa, 0xbb
        };
        long l;
        int i1, i2;

        if (mBytes[0] != 0x11) return 20;
        if (mBytes[1] != 0x22) return 21;
        if (mBytes[2] != 0x33) return 22;
        if (mBytes[3] != 0x44) return 23;
        if (mBytes[4] != 0x88) return 24;
        if (mBytes[5] != 0x99) return 25;
        if (mBytes[6] != 0xaa) return 26;
        if (mBytes[7] != 0xbb) return 27;

        i1 = mBytes[0] | mBytes[1] << 8 | mBytes[2] << 16 | mBytes[3] << 24;
        i2 = mBytes[4] | mBytes[5] << 8 | mBytes[6] << 16 | mBytes[7] << 24;
        l = i1 | ((long)i2 << 32);

        if (i1 != 0x44332211) { return 0x80000000 | i1; }
        if (i2 != 0xbbaa9988) { return 2; }
        if (l != 0xbbaa998844332211L) { return 3; }

        l = (long)mBytes[0]
                | (long)mBytes[1] << 8
                | (long)mBytes[2] << 16
                | (long)mBytes[3] << 24
                | (long)mBytes[4] << 32
                | (long)mBytes[5] << 40
                | (long)mBytes[6] << 48
                | (long)mBytes[7] << 56;

        if (l != 0xbbaa998844332211L) { return 4; }
        return 0;
    }

    static int shiftTest2() {

        long    a = 0x11;
        long    b = 0x22;
        long    c = 0x33;
        long    d = 0x44;
        long    e = 0x55;
        long    f = 0x66;
        long    g = 0x77;
        long    h = 0x88;

        long    result = ((a << 56) | (b << 48) | (c << 40) | (d << 32) |
                          (e << 24) | (f << 16) | (g <<  8) | h);

        if (result != 0x1122334455667788L) { return 1; }
        return 0;
    }

    static int unsignedShiftTest() {
        byte b = -4;
        short s = -4;
        char c = 0xfffc;
        int i = -4;

        b >>>= 4;
        s >>>= 4;
        c >>>= 4;
        i >>>= 4;

        if ((int) b != -1) { return 1; }
        if ((int) s != -1) { return 2; }
        if ((int) c != 0x0fff) { return 3; }
        if (i != 268435455) { return 4; }
        return 0;
    }

    static int convTest() {

        float f;
        double d;
        int i;
        long l;

        /* int --> long */
        i = 7654;
        l = (long) i;
        if (l != 7654L) { return 1; }

        i = -7654;
        l = (long) i;
        if (l != -7654L) { return 2; }

        /* long --> int (with truncation) */
        l = 5678956789L;
        i = (int) l;
        if (i != 1383989493) { return 3; }

        l = -5678956789L;
        i = (int) l;
        if (i != -1383989493) { return 4; }
        return 0;
    }

    static int charSubTest() {

        char char1 = 0x00e9;
        char char2 = 0xffff;
        int i;

        /* chars are unsigned-expanded to ints before subtraction */
        i = char1 - char2;
        if (i != 0xffff00ea) { return 1; }
        return 0;
    }

    /*
     * We pass in the arguments and return the results so the compiler
     * doesn't do the math for us.  (x=70000, y=-3)
     */
    static int intOperTest(int x, int y) {
        int[] results = new int[10];

        /* this seems to generate "op-int" instructions */
        results[0] = x + y;
        results[1] = x - y;
        results[2] = x * y;
        results[3] = x * x;
        results[4] = x / y;
        results[5] = x % -y;
        results[6] = x & y;
        results[7] = x | y;
        results[8] = x ^ y;

        /* this seems to generate "op-int/2addr" instructions */
        results[9] = x + ((((((((x + y) - y) * y) / y) % y) & y) | y) ^ y);

        /* check this edge case while we're here (div-int/2addr) */
        int minInt = -2147483648;
        int negOne = -results[5];
        int plusOne = 1;
        int result = (((minInt + plusOne) - plusOne) / negOne) / negOne;

        if (result != minInt) { return 1;};
        if (results[0] != 69997) { return 2;};
        if (results[1] != 70003) { return 3;};
        if (results[2] != -210000) { return 4;};
        if (results[3] != 605032704) { return 5;};
        if (results[4] != -23333) { return 6;};
        if (results[5] != 1) { return 7;};
        if (results[6] != 70000) { return 8;};
        if (results[7] != -3) { return 9;};
        if (results[8] != -70003) { return 10;};
        if (results[9] != 70000) { return 11;};

        return 0;
    }

    /*
     * More operations, this time with 16-bit constants.  (x=77777)
     */
    static int lit16Test(int x) {

        int[] results = new int[8];

        /* try to generate op-int/lit16" instructions */
        results[0] = x + 1000;
        results[1] = 1000 - x;
        results[2] = x * 1000;
        results[3] = x / 1000;
        results[4] = x % 1000;
        results[5] = x & 1000;
        results[6] = x | -1000;
        results[7] = x ^ -1000;

        if (results[0] != 78777) { return 1; }
        if (results[1] != -76777) { return 2; }
        if (results[2] != 77777000) { return 3; }
        if (results[3] != 77) { return 4; }
        if (results[4] != 777) { return 5; }
        if (results[5] != 960) { return 6; }
        if (results[6] != -39) { return 7; }
        if (results[7] != -76855) { return 8; }
        return 0;
    }

    /*
     * More operations, this time with 8-bit constants.  (x=-55555)
     */
    static int lit8Test(int x) {

        int[] results = new int[8];

        /* try to generate op-int/lit8" instructions */
        results[0] = x + 10;
        results[1] = 10 - x;
        results[2] = x * 10;
        results[3] = x / 10;
        results[4] = x % 10;
        results[5] = x & 10;
        results[6] = x | -10;
        results[7] = x ^ -10;
        int minInt = -2147483648;
        int result = minInt / -1;
        if (result != minInt) {return 1; }
        if (results[0] != -55545) {return 2; }
        if (results[1] != 55565) {return 3; }
        if (results[2] != -555550) {return 4; }
        if (results[3] != -5555) {return 5; }
        if (results[4] != -5) {return 6; }
        if (results[5] != 8) {return 7; }
        if (results[6] != -1) {return 8; }
        if (results[7] != 55563) {return 9; }
        return 0;
    }


    /*
     * Shift some data.  (value=0xff00aa01, dist=8)
     */
    static int intShiftTest(int value, int dist) {
        int results[] = new int[4];
        results[0] = value << dist;
        results[1] = value >> dist;
        results[2] = value >>> dist;
        results[3] = (((value << dist) >> dist) >>> dist) << dist;
        if (results[0] != 0x00aa0100) {return 1; }
        if (results[1] != 0xffff00aa) {return 2; }
        if (results[2] != 0x00ff00aa) {return 3; }
        if (results[3] != 0xaa00) {return 4; }
        return 0;
    }

    /*
     * We pass in the arguments and return the results so the compiler
     * doesn't do the math for us.  (x=70000000000, y=-3)
     */
    static int longOperTest(long x, long y) {
        long[] results = new long[10];

        /* this seems to generate "op-long" instructions */
        results[0] = x + y;
        results[1] = x - y;
        results[2] = x * y;
        results[3] = x * x;
        results[4] = x / y;
        results[5] = x % -y;
        results[6] = x & y;
        results[7] = x | y;
        results[8] = x ^ y;
        /* this seems to generate "op-long/2addr" instructions */
        results[9] = x + ((((((((x + y) - y) * y) / y) % y) & y) | y) ^ y);
        /* check this edge case while we're here (div-long/2addr) */
        long minLong = -9223372036854775808L;
        long negOne = -results[5];
        long plusOne = 1;
        long result = (((minLong + plusOne) - plusOne) / negOne) / negOne;
        if (result != minLong) { return 1; }
        if (results[0] != 69999999997L) { return 2; }
        if (results[1] != 70000000003L) { return 3; }
        if (results[2] != -210000000000L) { return 4; }
        if (results[3] != -6833923606740729856L) { return 5; }    // overflow
        if (results[4] != -23333333333L) { return 6; }
        if (results[5] != 1) { return 7; }
        if (results[6] != 70000000000L) { return 8; }
        if (results[7] != -3) { return 9; }
        if (results[8] != -70000000003L) { return 10; }
        if (results[9] != 70000000000L) { return 11; }
        if (results.length != 10) { return 12; }
        return 0;
    }

    /*
     * Shift some data.  (value=0xd5aa96deff00aa01, dist=16)
     */
    static long longShiftTest(long value, int dist) {
        long results[] = new long[4];
        results[0] = value << dist;
        results[1] = value >> dist;
        results[2] = value >>> dist;
        results[3] = (((value << dist) >> dist) >>> dist) << dist;
        if (results[0] != 0x96deff00aa010000L) { return results[0]; }
        if (results[1] != 0xffffd5aa96deff00L) { return results[1]; }
        if (results[2] != 0x0000d5aa96deff00L) { return results[2]; }
        if (results[3] != 0xffff96deff000000L) { return results[3]; }
        if (results.length != 4) { return 5; }

        return results[0];      // test return-long
    }

    static int switchTest(int a) {
        int res = 1234;

        switch (a) {
            case -1: res = 1; return res;
            case 0: res = 2; return res;
            case 1: /*correct*/ break;
            case 2: res = 3; return res;
            case 3: res = 4; return res;
            case 4: res = 5; return res;
            default: res = 6; return res;
        }
        switch (a) {
            case 3: res = 7; return res;
            case 4: res = 8; return res;
            default: /*correct*/ break;
        }

        a = 0x12345678;

        switch (a) {
            case 0x12345678: /*correct*/ break;
            case 0x12345679: res = 9; return res;
            default: res = 1; return res;
        }
        switch (a) {
            case 57: res = 10; return res;
            case -6: res = 11; return res;
            case 0x12345678: /*correct*/ break;
            case 22: res = 12; return res;
            case 3: res = 13; return res;
            default: res = 14; return res;
        }
        switch (a) {
            case -6: res = 15; return res;
            case 3: res = 16; return res;
            default: /*correct*/ break;
        }

        a = -5;
        switch (a) {
            case 12: res = 17; return res;
            case -5: /*correct*/ break;
            case 0: res = 18; return res;
            default: res = 19; return res;
        }

        switch (a) {
            default: /*correct*/ break;
        }
        return res;
    }
    /*
     * Test the integer comparisons in various ways.
     */
    static int testIntCompare(int minus, int plus, int plus2, int zero) {
        int res = 1111;

        if (minus > plus)
            return 1;
        if (minus >= plus)
            return 2;
        if (plus < minus)
            return 3;
        if (plus <= minus)
            return 4;
        if (plus == minus)
            return 5;
        if (plus != plus2)
            return 6;

        /* try a branch-taken */
        if (plus != minus) {
            res = res;
        } else {
            return 7;
        }

        if (minus > 0)
            return 8;
        if (minus >= 0)
            return 9;
        if (plus < 0)
            return 10;
        if (plus <= 0)
            return 11;
        if (plus == 0)
            return 12;
        if (zero != 0)
            return 13;

        if (zero == 0) {
            res = res;
        } else {
            return 14;
        }
        return res;
    }

    /*
     * Test cmp-long.
     *
     * minus=-5, alsoMinus=0xFFFFFFFF00000009, plus=4, alsoPlus=8
     */
    static int testLongCompare(long minus, long alsoMinus, long plus,
                               long alsoPlus) {
        int res = 2222;

        if (minus > plus)
            return 2;
        if (plus < minus)
            return 3;
        if (plus == minus)
            return 4;

        if (plus >= plus+1)
            return 5;
        if (minus >= minus+1)
            return 6;

        /* try a branch-taken */
        if (plus != minus) {
            res = res;
        } else {
            return 7;
        }

        /* compare when high words are equal but low words differ */
        if (plus > alsoPlus)
            return 8;
        if (alsoPlus < plus)
            return 9;
        if (alsoPlus == plus)
            return 10;

        /* high words are equal, low words have apparently different signs */
        if (minus < alsoMinus)      // bug!
            return 11;
        if (alsoMinus > minus)
            return 12;
        if (alsoMinus == minus)
            return 13;

        return res;
    }

    /*
     * Test cmpl-float and cmpg-float.
     */
    static int testFloatCompare(float minus, float plus, float plus2,
                                float nan) {

        int res = 3333;
        if (minus > plus)
            res = 1;
        if (plus < minus)
            res = 2;
        if (plus == minus)
            res = 3;
        if (plus != plus2)
            res = 4;

        if (plus <= nan)
            res = 5;
        if (plus >= nan)
            res = 6;
        if (minus <= nan)
            res = 7;
        if (minus >= nan)
            res = 8;
        if (nan >= plus)
            res = 9;
        if (nan <= plus)
            res = 10;

        if (nan == nan)
            res = 1212;

        return res;
    }

    static int testDoubleCompare(double minus, double plus, double plus2,
                                 double nan) {

        int res = 4444;

        if (minus > plus)
            return 1;
        if (plus < minus)
            return 2;
        if (plus == minus)
            return 3;
        if (plus != plus2)
            return 4;

        if (plus <= nan)
            return 5;
        if (plus >= nan)
            return 6;
        if (minus <= nan)
            return 7;
        if (minus >= nan)
            return 8;
        if (nan >= plus)
            return 9;
        if (nan <= plus)
            return 10;

        if (nan == nan)
            return 11;
        return res;
    }

    static int fibonacci(int n) {
        if (n == 0) {
            return 0;
        } else if (n == 1) {
            return 1;
        } else {
            return fibonacci(n - 1) + fibonacci(n - 2);
        }
    }

    /*
      static void throwNullPointerException() {
      throw new NullPointerException("first throw");
      }

      static int throwAndCatch() {
      try {
      throwNullPointerException();
      return 1;
      } catch (NullPointerException npe) {
      return 0;
      }
      }
    */

    static int manyArgs(int a0, long a1, int a2, long a3, int a4, long a5,
                        int a6, int a7, double a8, float a9, double a10, short a11, int a12,
                        char a13, int a14, int a15, byte a16, boolean a17, int a18, int a19,
                        long a20, long a21, int a22, int a23, int a24, int a25, int a26)
    {
        if (a0 != 0) return 0;
        if (a1 !=  1L) return 1;
        if (a2 != 2) return 2;
        if (a3 != 3L) return 3;
        if (a4 != 4) return 4;
        if (a5 != 5L) return 5;
        if (a6 != 6) return 6;
        if (a7 != 7) return 7;
        if (a8 != 8.0) return 8;
        if (a9 !=  9.0f) return 9;
        if (a10 != 10.0) return 10;
        if (a11 != (short)11) return 11;
        if (a12 != 12) return 12;
        if (a13 != (char)13) return 13;
        if (a14 != 14) return 14;
        if (a15 != 15) return 15;
        if (a16 != (byte)-16) return 16;
        if (a17 !=  true) return 17;
        if (a18 != 18) return 18;
        if (a19 != 19) return 19;
        if (a20 !=  20L) return 20;
        if (a21 != 21L) return 21;
        if (a22 != 22) return 22;
        if (a23 != 23) return 23;
        if (a24 != 24) return 24;
        if (a25 != 25) return 25;
        if (a26 != 26) return 26;
        return -1;
    }

    int virtualCall(int a)
    {
        return a * 2;
    }

    void setFoo(int a)
    {
        foo_ = a;
    }

    int getFoo()
    {
        return foo_;
    }

    static int staticCall(int a)
    {
        IntMath foo = new IntMath();
        return foo.virtualCall(a);
    }

    static int testIGetPut(int a)
    {
        IntMath foo = new IntMath(99);
        IntMath foo123 = new IntMath();
        int z  = foo.getFoo();
        z += a;
        z += foo123.getFoo();
        foo.setFoo(z);
        return foo.getFoo();
    }

    public static void main(String[] args) {
        int res = unopTest(38);
        if (res == 37) {
            System.out.printf("unopTest PASSED\n");
        } else {
            System.out.printf("unopTest FAILED: %d\n", res);
        }
        res = shiftTest1();
        if (res == 0) {
            System.out.printf("shiftTest1 PASSED\n");
        } else {
            System.out.printf("shiftTest1 FAILED: %d\n", res);
        }
        res = shiftTest2();
        if (res == 0) {
            System.out.printf("shiftTest2 PASSED\n");
        } else {
            System.out.printf("shiftTest2 FAILED: %d\n", res);
        }
        res = unsignedShiftTest();
        if (res == 0) {
            System.out.printf("unsignedShiftTest PASSED\n");
        } else {
            System.out.printf("unsignedShiftTest FAILED: %d\n", res);
        }
        res = convTest();
        if (res == 0) {
            System.out.printf("convTest PASSED\n");
        } else {
            System.out.printf("convTest FAILED: %d\n", res);
        }
        res = charSubTest();
        if (res == 0) {
            System.out.printf("charSubTest PASSED\n");
        } else {
            System.out.printf("charSubTest FAILED: %d\n", res);
        }
        res = intOperTest(70000, -3);
        if (res == 0) {
            System.out.printf("intOperTest PASSED\n");
        } else {
            System.out.printf("intOperTest FAILED: %d\n", res);
        }
        res = longOperTest(70000000000L, -3L);
        if (res == 0) {
            System.out.printf("longOperTest PASSED\n");
        } else {
            System.out.printf("longOperTest FAILED: %d\n", res);
        }
        long lres = longShiftTest(0xd5aa96deff00aa01L, 16);
        if (lres == 0x96deff00aa010000L) {
            System.out.printf("longShiftTest PASSED\n");
        } else {
            System.out.printf("longShiftTest FAILED: %d\n", res);
        }

        res = switchTest(1);
        if (res == 1234) {
            System.out.printf("switchTest PASSED\n");
        } else {
            System.out.printf("switchTest FAILED: %d\n", res);
        }

        res = testIntCompare(-5, 4, 4, 0);
        if (res == 1111) {
            System.out.printf("testIntCompare PASSED\n");
        } else {
            System.out.printf("testIntCompare FAILED: %d\n", res);
        }

        res = testLongCompare(-5L, -4294967287L, 4L, 8L);
        if (res == 2222) {
            System.out.printf("testLongCompare PASSED\n");
        } else {
            System.out.printf("testLongCompare FAILED: %d\n", res);
        }

        res = testFloatCompare(-5.0f, 4.0f, 4.0f, (1.0f/0.0f) / (1.0f/0.0f));
        if (res == 3333) {
            System.out.printf("testFloatCompare PASSED\n");
        } else {
            System.out.printf("testFloatCompare FAILED: %d\n", res);
        }

        res = testDoubleCompare(-5.0, 4.0, 4.0, (1.0/0.0) / (1.0/0.0));
        if (res == 4444) {
            System.out.printf("testDoubleCompare PASSED\n");
        } else {
            System.out.printf("testDoubleCompare FAILED: %d\n", res);
        }

        res = fibonacci(10);
        if (res == 55) {
            System.out.printf("fibonacci PASSED\n");
        } else {
            System.out.printf("fibonacci FAILED: %d\n", res);
        }

        /*
          res = throwAndCatch();
          if (res == 0) {
          System.out.printf("throwAndCatch PASSED\n");
          } else {
          System.out.printf("throwAndCatch FAILED: %d\n", res);
          }
        */

        res = manyArgs(0, 1L, 2, 3L, 4, 5L, 6, 7, 8.0, 9.0f, 10.0,
                       (short)11, 12, (char)13, 14, 15, (byte)-16, true, 18,
                       19, 20L, 21L, 22, 23, 24, 25, 26);
        if (res == -1) {
            System.out.printf("manyArgs PASSED\n");
        } else {
            System.out.printf("manyArgs FAILED: %d\n", res);
        }

        res = staticCall(3);
        if (res == 6) {
            System.out.printf("virtualCall PASSED\n");
        } else {
            System.out.printf("virtualCall FAILED: %d\n", res);
        }

        res = testIGetPut(111);
        if (res == 333) {
            System.out.printf("testGetPut PASSED\n");
        } else {
            System.out.printf("testGetPut FAILED: %d\n", res);
        }

        res = staticFieldTest(404);
        if (res == 1404) {
            System.out.printf("staticFieldTest PASSED\n");
        } else {
            System.out.printf("staticFieldTest FAILED: %d\n", res);
        }

        res = catchBlock(1000);
        if (res == 1579) {
            System.out.printf("catchBlock PASSED\n");
        } else {
            System.out.printf("catchBlock FAILED: %d\n", res);
        }
    }
}
