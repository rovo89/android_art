/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

public class Main {
    public static int num_errors = 0;

    public static void reportError(String message) {
        if (num_errors == 10) {
            System.out.println("Omitting other error messages...");
        } else if (num_errors < 10) {
            System.out.println(message);
        }
        num_errors += 1;
    }

    public static void intCheckDiv(String desc, int result, int dividend, int divisor) {
        int correct_result = dividend / divisor;
        if (result != correct_result) {
            reportError(desc + "(" + dividend + ") == " + result +
                        " should be " + correct_result);
        }
    }
    public static void intCheckRem(String desc, int result, int dividend, int divisor) {
        int correct_result = dividend % divisor;
        if (result != correct_result) {
            reportError(desc + "(" + dividend + ") == " + result +
                        " should be " + correct_result);
        }
    }
    public static void longCheckDiv(String desc, long result, long dividend, long divisor) {
        long correct_result = dividend / divisor;
        if (result != correct_result) {
            reportError(desc + "(" + dividend + ") == " + result +
                        " should be " + correct_result);
        }
    }
    public static void longCheckRem(String desc, long result, long dividend, long divisor) {
        long correct_result = dividend % divisor;
        if (result != correct_result) {
            reportError(desc + "(" + dividend + ") == " + result +
                        " should be " + correct_result);
        }
    }

    public static int idiv_by_pow2_0(int x) {return x / 1;}
    public static int idiv_by_pow2_1(int x) {return x / 2;}
    public static int idiv_by_pow2_2(int x) {return x / 4;}
    public static int idiv_by_pow2_3(int x) {return x / 8;}
    public static int idiv_by_pow2_4(int x) {return x / 16;}
    public static int idiv_by_pow2_5(int x) {return x / 32;}
    public static int idiv_by_pow2_6(int x) {return x / 64;}
    public static int idiv_by_pow2_7(int x) {return x / 128;}
    public static int idiv_by_pow2_8(int x) {return x / 256;}
    public static int idiv_by_pow2_9(int x) {return x / 512;}
    public static int idiv_by_pow2_10(int x) {return x / 1024;}
    public static int idiv_by_pow2_11(int x) {return x / 2048;}
    public static int idiv_by_pow2_12(int x) {return x / 4096;}
    public static int idiv_by_pow2_13(int x) {return x / 8192;}
    public static int idiv_by_pow2_14(int x) {return x / 16384;}
    public static int idiv_by_pow2_15(int x) {return x / 32768;}
    public static int idiv_by_pow2_16(int x) {return x / 65536;}
    public static int idiv_by_pow2_17(int x) {return x / 131072;}
    public static int idiv_by_pow2_18(int x) {return x / 262144;}
    public static int idiv_by_pow2_19(int x) {return x / 524288;}
    public static int idiv_by_pow2_20(int x) {return x / 1048576;}
    public static int idiv_by_pow2_21(int x) {return x / 2097152;}
    public static int idiv_by_pow2_22(int x) {return x / 4194304;}
    public static int idiv_by_pow2_23(int x) {return x / 8388608;}
    public static int idiv_by_pow2_24(int x) {return x / 16777216;}
    public static int idiv_by_pow2_25(int x) {return x / 33554432;}
    public static int idiv_by_pow2_26(int x) {return x / 67108864;}
    public static int idiv_by_pow2_27(int x) {return x / 134217728;}
    public static int idiv_by_pow2_28(int x) {return x / 268435456;}
    public static int idiv_by_pow2_29(int x) {return x / 536870912;}
    public static int idiv_by_pow2_30(int x) {return x / 1073741824;}
    public static int idiv_by_small_0(int x) {return x / 3;}
    public static int idiv_by_small_1(int x) {return x / 5;}
    public static int idiv_by_small_2(int x) {return x / 6;}
    public static int idiv_by_small_3(int x) {return x / 7;}
    public static int idiv_by_small_4(int x) {return x / 9;}
    public static int idiv_by_small_5(int x) {return x / 10;}
    public static int idiv_by_small_6(int x) {return x / 11;}
    public static int idiv_by_small_7(int x) {return x / 12;}
    public static int idiv_by_small_8(int x) {return x / 13;}
    public static int idiv_by_small_9(int x) {return x / 14;}
    public static int idiv_by_small_10(int x) {return x / 15;}
    public static int irem_by_pow2_0(int x) {return x % 1;}
    public static int irem_by_pow2_1(int x) {return x % 2;}
    public static int irem_by_pow2_2(int x) {return x % 4;}
    public static int irem_by_pow2_3(int x) {return x % 8;}
    public static int irem_by_pow2_4(int x) {return x % 16;}
    public static int irem_by_pow2_5(int x) {return x % 32;}
    public static int irem_by_pow2_6(int x) {return x % 64;}
    public static int irem_by_pow2_7(int x) {return x % 128;}
    public static int irem_by_pow2_8(int x) {return x % 256;}
    public static int irem_by_pow2_9(int x) {return x % 512;}
    public static int irem_by_pow2_10(int x) {return x % 1024;}
    public static int irem_by_pow2_11(int x) {return x % 2048;}
    public static int irem_by_pow2_12(int x) {return x % 4096;}
    public static int irem_by_pow2_13(int x) {return x % 8192;}
    public static int irem_by_pow2_14(int x) {return x % 16384;}
    public static int irem_by_pow2_15(int x) {return x % 32768;}
    public static int irem_by_pow2_16(int x) {return x % 65536;}
    public static int irem_by_pow2_17(int x) {return x % 131072;}
    public static int irem_by_pow2_18(int x) {return x % 262144;}
    public static int irem_by_pow2_19(int x) {return x % 524288;}
    public static int irem_by_pow2_20(int x) {return x % 1048576;}
    public static int irem_by_pow2_21(int x) {return x % 2097152;}
    public static int irem_by_pow2_22(int x) {return x % 4194304;}
    public static int irem_by_pow2_23(int x) {return x % 8388608;}
    public static int irem_by_pow2_24(int x) {return x % 16777216;}
    public static int irem_by_pow2_25(int x) {return x % 33554432;}
    public static int irem_by_pow2_26(int x) {return x % 67108864;}
    public static int irem_by_pow2_27(int x) {return x % 134217728;}
    public static int irem_by_pow2_28(int x) {return x % 268435456;}
    public static int irem_by_pow2_29(int x) {return x % 536870912;}
    public static int irem_by_pow2_30(int x) {return x % 1073741824;}
    public static long ldiv_by_pow2_0(long x) {return x / 1l;}
    public static long ldiv_by_pow2_1(long x) {return x / 2l;}
    public static long ldiv_by_pow2_2(long x) {return x / 4l;}
    public static long ldiv_by_pow2_3(long x) {return x / 8l;}
    public static long ldiv_by_pow2_4(long x) {return x / 16l;}
    public static long ldiv_by_pow2_5(long x) {return x / 32l;}
    public static long ldiv_by_pow2_6(long x) {return x / 64l;}
    public static long ldiv_by_pow2_7(long x) {return x / 128l;}
    public static long ldiv_by_pow2_8(long x) {return x / 256l;}
    public static long ldiv_by_pow2_9(long x) {return x / 512l;}
    public static long ldiv_by_pow2_10(long x) {return x / 1024l;}
    public static long ldiv_by_pow2_11(long x) {return x / 2048l;}
    public static long ldiv_by_pow2_12(long x) {return x / 4096l;}
    public static long ldiv_by_pow2_13(long x) {return x / 8192l;}
    public static long ldiv_by_pow2_14(long x) {return x / 16384l;}
    public static long ldiv_by_pow2_15(long x) {return x / 32768l;}
    public static long ldiv_by_pow2_16(long x) {return x / 65536l;}
    public static long ldiv_by_pow2_17(long x) {return x / 131072l;}
    public static long ldiv_by_pow2_18(long x) {return x / 262144l;}
    public static long ldiv_by_pow2_19(long x) {return x / 524288l;}
    public static long ldiv_by_pow2_20(long x) {return x / 1048576l;}
    public static long ldiv_by_pow2_21(long x) {return x / 2097152l;}
    public static long ldiv_by_pow2_22(long x) {return x / 4194304l;}
    public static long ldiv_by_pow2_23(long x) {return x / 8388608l;}
    public static long ldiv_by_pow2_24(long x) {return x / 16777216l;}
    public static long ldiv_by_pow2_25(long x) {return x / 33554432l;}
    public static long ldiv_by_pow2_26(long x) {return x / 67108864l;}
    public static long ldiv_by_pow2_27(long x) {return x / 134217728l;}
    public static long ldiv_by_pow2_28(long x) {return x / 268435456l;}
    public static long ldiv_by_pow2_29(long x) {return x / 536870912l;}
    public static long ldiv_by_pow2_30(long x) {return x / 1073741824l;}
    public static long ldiv_by_pow2_31(long x) {return x / 2147483648l;}
    public static long ldiv_by_pow2_32(long x) {return x / 4294967296l;}
    public static long ldiv_by_pow2_33(long x) {return x / 8589934592l;}
    public static long ldiv_by_pow2_34(long x) {return x / 17179869184l;}
    public static long ldiv_by_pow2_35(long x) {return x / 34359738368l;}
    public static long ldiv_by_pow2_36(long x) {return x / 68719476736l;}
    public static long ldiv_by_pow2_37(long x) {return x / 137438953472l;}
    public static long ldiv_by_pow2_38(long x) {return x / 274877906944l;}
    public static long ldiv_by_pow2_39(long x) {return x / 549755813888l;}
    public static long ldiv_by_pow2_40(long x) {return x / 1099511627776l;}
    public static long ldiv_by_pow2_41(long x) {return x / 2199023255552l;}
    public static long ldiv_by_pow2_42(long x) {return x / 4398046511104l;}
    public static long ldiv_by_pow2_43(long x) {return x / 8796093022208l;}
    public static long ldiv_by_pow2_44(long x) {return x / 17592186044416l;}
    public static long ldiv_by_pow2_45(long x) {return x / 35184372088832l;}
    public static long ldiv_by_pow2_46(long x) {return x / 70368744177664l;}
    public static long ldiv_by_pow2_47(long x) {return x / 140737488355328l;}
    public static long ldiv_by_pow2_48(long x) {return x / 281474976710656l;}
    public static long ldiv_by_pow2_49(long x) {return x / 562949953421312l;}
    public static long ldiv_by_pow2_50(long x) {return x / 1125899906842624l;}
    public static long ldiv_by_pow2_51(long x) {return x / 2251799813685248l;}
    public static long ldiv_by_pow2_52(long x) {return x / 4503599627370496l;}
    public static long ldiv_by_pow2_53(long x) {return x / 9007199254740992l;}
    public static long ldiv_by_pow2_54(long x) {return x / 18014398509481984l;}
    public static long ldiv_by_pow2_55(long x) {return x / 36028797018963968l;}
    public static long ldiv_by_pow2_56(long x) {return x / 72057594037927936l;}
    public static long ldiv_by_pow2_57(long x) {return x / 144115188075855872l;}
    public static long ldiv_by_pow2_58(long x) {return x / 288230376151711744l;}
    public static long ldiv_by_pow2_59(long x) {return x / 576460752303423488l;}
    public static long ldiv_by_pow2_60(long x) {return x / 1152921504606846976l;}
    public static long ldiv_by_pow2_61(long x) {return x / 2305843009213693952l;}
    public static long ldiv_by_pow2_62(long x) {return x / 4611686018427387904l;}
    public static long ldiv_by_small_0(long x) {return x / 3l;}
    public static long ldiv_by_small_1(long x) {return x / 5l;}
    public static long ldiv_by_small_2(long x) {return x / 6l;}
    public static long ldiv_by_small_3(long x) {return x / 7l;}
    public static long ldiv_by_small_4(long x) {return x / 9l;}
    public static long ldiv_by_small_5(long x) {return x / 10l;}
    public static long ldiv_by_small_6(long x) {return x / 11l;}
    public static long ldiv_by_small_7(long x) {return x / 12l;}
    public static long ldiv_by_small_8(long x) {return x / 13l;}
    public static long ldiv_by_small_9(long x) {return x / 14l;}
    public static long ldiv_by_small_10(long x) {return x / 15l;}
    public static long lrem_by_pow2_0(long x) {return x % 1l;}
    public static long lrem_by_pow2_1(long x) {return x % 2l;}
    public static long lrem_by_pow2_2(long x) {return x % 4l;}
    public static long lrem_by_pow2_3(long x) {return x % 8l;}
    public static long lrem_by_pow2_4(long x) {return x % 16l;}
    public static long lrem_by_pow2_5(long x) {return x % 32l;}
    public static long lrem_by_pow2_6(long x) {return x % 64l;}
    public static long lrem_by_pow2_7(long x) {return x % 128l;}
    public static long lrem_by_pow2_8(long x) {return x % 256l;}
    public static long lrem_by_pow2_9(long x) {return x % 512l;}
    public static long lrem_by_pow2_10(long x) {return x % 1024l;}
    public static long lrem_by_pow2_11(long x) {return x % 2048l;}
    public static long lrem_by_pow2_12(long x) {return x % 4096l;}
    public static long lrem_by_pow2_13(long x) {return x % 8192l;}
    public static long lrem_by_pow2_14(long x) {return x % 16384l;}
    public static long lrem_by_pow2_15(long x) {return x % 32768l;}
    public static long lrem_by_pow2_16(long x) {return x % 65536l;}
    public static long lrem_by_pow2_17(long x) {return x % 131072l;}
    public static long lrem_by_pow2_18(long x) {return x % 262144l;}
    public static long lrem_by_pow2_19(long x) {return x % 524288l;}
    public static long lrem_by_pow2_20(long x) {return x % 1048576l;}
    public static long lrem_by_pow2_21(long x) {return x % 2097152l;}
    public static long lrem_by_pow2_22(long x) {return x % 4194304l;}
    public static long lrem_by_pow2_23(long x) {return x % 8388608l;}
    public static long lrem_by_pow2_24(long x) {return x % 16777216l;}
    public static long lrem_by_pow2_25(long x) {return x % 33554432l;}
    public static long lrem_by_pow2_26(long x) {return x % 67108864l;}
    public static long lrem_by_pow2_27(long x) {return x % 134217728l;}
    public static long lrem_by_pow2_28(long x) {return x % 268435456l;}
    public static long lrem_by_pow2_29(long x) {return x % 536870912l;}
    public static long lrem_by_pow2_30(long x) {return x % 1073741824l;}
    public static long lrem_by_pow2_31(long x) {return x % 2147483648l;}
    public static long lrem_by_pow2_32(long x) {return x % 4294967296l;}
    public static long lrem_by_pow2_33(long x) {return x % 8589934592l;}
    public static long lrem_by_pow2_34(long x) {return x % 17179869184l;}
    public static long lrem_by_pow2_35(long x) {return x % 34359738368l;}
    public static long lrem_by_pow2_36(long x) {return x % 68719476736l;}
    public static long lrem_by_pow2_37(long x) {return x % 137438953472l;}
    public static long lrem_by_pow2_38(long x) {return x % 274877906944l;}
    public static long lrem_by_pow2_39(long x) {return x % 549755813888l;}
    public static long lrem_by_pow2_40(long x) {return x % 1099511627776l;}
    public static long lrem_by_pow2_41(long x) {return x % 2199023255552l;}
    public static long lrem_by_pow2_42(long x) {return x % 4398046511104l;}
    public static long lrem_by_pow2_43(long x) {return x % 8796093022208l;}
    public static long lrem_by_pow2_44(long x) {return x % 17592186044416l;}
    public static long lrem_by_pow2_45(long x) {return x % 35184372088832l;}
    public static long lrem_by_pow2_46(long x) {return x % 70368744177664l;}
    public static long lrem_by_pow2_47(long x) {return x % 140737488355328l;}
    public static long lrem_by_pow2_48(long x) {return x % 281474976710656l;}
    public static long lrem_by_pow2_49(long x) {return x % 562949953421312l;}
    public static long lrem_by_pow2_50(long x) {return x % 1125899906842624l;}
    public static long lrem_by_pow2_51(long x) {return x % 2251799813685248l;}
    public static long lrem_by_pow2_52(long x) {return x % 4503599627370496l;}
    public static long lrem_by_pow2_53(long x) {return x % 9007199254740992l;}
    public static long lrem_by_pow2_54(long x) {return x % 18014398509481984l;}
    public static long lrem_by_pow2_55(long x) {return x % 36028797018963968l;}
    public static long lrem_by_pow2_56(long x) {return x % 72057594037927936l;}
    public static long lrem_by_pow2_57(long x) {return x % 144115188075855872l;}
    public static long lrem_by_pow2_58(long x) {return x % 288230376151711744l;}
    public static long lrem_by_pow2_59(long x) {return x % 576460752303423488l;}
    public static long lrem_by_pow2_60(long x) {return x % 1152921504606846976l;}
    public static long lrem_by_pow2_61(long x) {return x % 2305843009213693952l;}
    public static long lrem_by_pow2_62(long x) {return x % 4611686018427387904l;}

    public static void intCheckAll(int x) {
        intCheckDiv("idiv_by_pow2_0", idiv_by_pow2_0(x), x, 1);
        intCheckDiv("idiv_by_pow2_1", idiv_by_pow2_1(x), x, 2);
        intCheckDiv("idiv_by_pow2_2", idiv_by_pow2_2(x), x, 4);
        intCheckDiv("idiv_by_pow2_3", idiv_by_pow2_3(x), x, 8);
        intCheckDiv("idiv_by_pow2_4", idiv_by_pow2_4(x), x, 16);
        intCheckDiv("idiv_by_pow2_5", idiv_by_pow2_5(x), x, 32);
        intCheckDiv("idiv_by_pow2_6", idiv_by_pow2_6(x), x, 64);
        intCheckDiv("idiv_by_pow2_7", idiv_by_pow2_7(x), x, 128);
        intCheckDiv("idiv_by_pow2_8", idiv_by_pow2_8(x), x, 256);
        intCheckDiv("idiv_by_pow2_9", idiv_by_pow2_9(x), x, 512);
        intCheckDiv("idiv_by_pow2_10", idiv_by_pow2_10(x), x, 1024);
        intCheckDiv("idiv_by_pow2_11", idiv_by_pow2_11(x), x, 2048);
        intCheckDiv("idiv_by_pow2_12", idiv_by_pow2_12(x), x, 4096);
        intCheckDiv("idiv_by_pow2_13", idiv_by_pow2_13(x), x, 8192);
        intCheckDiv("idiv_by_pow2_14", idiv_by_pow2_14(x), x, 16384);
        intCheckDiv("idiv_by_pow2_15", idiv_by_pow2_15(x), x, 32768);
        intCheckDiv("idiv_by_pow2_16", idiv_by_pow2_16(x), x, 65536);
        intCheckDiv("idiv_by_pow2_17", idiv_by_pow2_17(x), x, 131072);
        intCheckDiv("idiv_by_pow2_18", idiv_by_pow2_18(x), x, 262144);
        intCheckDiv("idiv_by_pow2_19", idiv_by_pow2_19(x), x, 524288);
        intCheckDiv("idiv_by_pow2_20", idiv_by_pow2_20(x), x, 1048576);
        intCheckDiv("idiv_by_pow2_21", idiv_by_pow2_21(x), x, 2097152);
        intCheckDiv("idiv_by_pow2_22", idiv_by_pow2_22(x), x, 4194304);
        intCheckDiv("idiv_by_pow2_23", idiv_by_pow2_23(x), x, 8388608);
        intCheckDiv("idiv_by_pow2_24", idiv_by_pow2_24(x), x, 16777216);
        intCheckDiv("idiv_by_pow2_25", idiv_by_pow2_25(x), x, 33554432);
        intCheckDiv("idiv_by_pow2_26", idiv_by_pow2_26(x), x, 67108864);
        intCheckDiv("idiv_by_pow2_27", idiv_by_pow2_27(x), x, 134217728);
        intCheckDiv("idiv_by_pow2_28", idiv_by_pow2_28(x), x, 268435456);
        intCheckDiv("idiv_by_pow2_29", idiv_by_pow2_29(x), x, 536870912);
        intCheckDiv("idiv_by_pow2_30", idiv_by_pow2_30(x), x, 1073741824);
        intCheckDiv("idiv_by_small_0", idiv_by_small_0(x), x, 3);
        intCheckDiv("idiv_by_small_1", idiv_by_small_1(x), x, 5);
        intCheckDiv("idiv_by_small_2", idiv_by_small_2(x), x, 6);
        intCheckDiv("idiv_by_small_3", idiv_by_small_3(x), x, 7);
        intCheckDiv("idiv_by_small_4", idiv_by_small_4(x), x, 9);
        intCheckDiv("idiv_by_small_5", idiv_by_small_5(x), x, 10);
        intCheckDiv("idiv_by_small_6", idiv_by_small_6(x), x, 11);
        intCheckDiv("idiv_by_small_7", idiv_by_small_7(x), x, 12);
        intCheckDiv("idiv_by_small_8", idiv_by_small_8(x), x, 13);
        intCheckDiv("idiv_by_small_9", idiv_by_small_9(x), x, 14);
        intCheckDiv("idiv_by_small_10", idiv_by_small_10(x), x, 15);
        intCheckRem("irem_by_pow2_0", irem_by_pow2_0(x), x, 1);
        intCheckRem("irem_by_pow2_1", irem_by_pow2_1(x), x, 2);
        intCheckRem("irem_by_pow2_2", irem_by_pow2_2(x), x, 4);
        intCheckRem("irem_by_pow2_3", irem_by_pow2_3(x), x, 8);
        intCheckRem("irem_by_pow2_4", irem_by_pow2_4(x), x, 16);
        intCheckRem("irem_by_pow2_5", irem_by_pow2_5(x), x, 32);
        intCheckRem("irem_by_pow2_6", irem_by_pow2_6(x), x, 64);
        intCheckRem("irem_by_pow2_7", irem_by_pow2_7(x), x, 128);
        intCheckRem("irem_by_pow2_8", irem_by_pow2_8(x), x, 256);
        intCheckRem("irem_by_pow2_9", irem_by_pow2_9(x), x, 512);
        intCheckRem("irem_by_pow2_10", irem_by_pow2_10(x), x, 1024);
        intCheckRem("irem_by_pow2_11", irem_by_pow2_11(x), x, 2048);
        intCheckRem("irem_by_pow2_12", irem_by_pow2_12(x), x, 4096);
        intCheckRem("irem_by_pow2_13", irem_by_pow2_13(x), x, 8192);
        intCheckRem("irem_by_pow2_14", irem_by_pow2_14(x), x, 16384);
        intCheckRem("irem_by_pow2_15", irem_by_pow2_15(x), x, 32768);
        intCheckRem("irem_by_pow2_16", irem_by_pow2_16(x), x, 65536);
        intCheckRem("irem_by_pow2_17", irem_by_pow2_17(x), x, 131072);
        intCheckRem("irem_by_pow2_18", irem_by_pow2_18(x), x, 262144);
        intCheckRem("irem_by_pow2_19", irem_by_pow2_19(x), x, 524288);
        intCheckRem("irem_by_pow2_20", irem_by_pow2_20(x), x, 1048576);
        intCheckRem("irem_by_pow2_21", irem_by_pow2_21(x), x, 2097152);
        intCheckRem("irem_by_pow2_22", irem_by_pow2_22(x), x, 4194304);
        intCheckRem("irem_by_pow2_23", irem_by_pow2_23(x), x, 8388608);
        intCheckRem("irem_by_pow2_24", irem_by_pow2_24(x), x, 16777216);
        intCheckRem("irem_by_pow2_25", irem_by_pow2_25(x), x, 33554432);
        intCheckRem("irem_by_pow2_26", irem_by_pow2_26(x), x, 67108864);
        intCheckRem("irem_by_pow2_27", irem_by_pow2_27(x), x, 134217728);
        intCheckRem("irem_by_pow2_28", irem_by_pow2_28(x), x, 268435456);
        intCheckRem("irem_by_pow2_29", irem_by_pow2_29(x), x, 536870912);
        intCheckRem("irem_by_pow2_30", irem_by_pow2_30(x), x, 1073741824);
    }

    public static void longCheckAll(long x) {
        longCheckDiv("ldiv_by_pow2_0", ldiv_by_pow2_0(x), x, 1l);
        longCheckDiv("ldiv_by_pow2_1", ldiv_by_pow2_1(x), x, 2l);
        longCheckDiv("ldiv_by_pow2_2", ldiv_by_pow2_2(x), x, 4l);
        longCheckDiv("ldiv_by_pow2_3", ldiv_by_pow2_3(x), x, 8l);
        longCheckDiv("ldiv_by_pow2_4", ldiv_by_pow2_4(x), x, 16l);
        longCheckDiv("ldiv_by_pow2_5", ldiv_by_pow2_5(x), x, 32l);
        longCheckDiv("ldiv_by_pow2_6", ldiv_by_pow2_6(x), x, 64l);
        longCheckDiv("ldiv_by_pow2_7", ldiv_by_pow2_7(x), x, 128l);
        longCheckDiv("ldiv_by_pow2_8", ldiv_by_pow2_8(x), x, 256l);
        longCheckDiv("ldiv_by_pow2_9", ldiv_by_pow2_9(x), x, 512l);
        longCheckDiv("ldiv_by_pow2_10", ldiv_by_pow2_10(x), x, 1024l);
        longCheckDiv("ldiv_by_pow2_11", ldiv_by_pow2_11(x), x, 2048l);
        longCheckDiv("ldiv_by_pow2_12", ldiv_by_pow2_12(x), x, 4096l);
        longCheckDiv("ldiv_by_pow2_13", ldiv_by_pow2_13(x), x, 8192l);
        longCheckDiv("ldiv_by_pow2_14", ldiv_by_pow2_14(x), x, 16384l);
        longCheckDiv("ldiv_by_pow2_15", ldiv_by_pow2_15(x), x, 32768l);
        longCheckDiv("ldiv_by_pow2_16", ldiv_by_pow2_16(x), x, 65536l);
        longCheckDiv("ldiv_by_pow2_17", ldiv_by_pow2_17(x), x, 131072l);
        longCheckDiv("ldiv_by_pow2_18", ldiv_by_pow2_18(x), x, 262144l);
        longCheckDiv("ldiv_by_pow2_19", ldiv_by_pow2_19(x), x, 524288l);
        longCheckDiv("ldiv_by_pow2_20", ldiv_by_pow2_20(x), x, 1048576l);
        longCheckDiv("ldiv_by_pow2_21", ldiv_by_pow2_21(x), x, 2097152l);
        longCheckDiv("ldiv_by_pow2_22", ldiv_by_pow2_22(x), x, 4194304l);
        longCheckDiv("ldiv_by_pow2_23", ldiv_by_pow2_23(x), x, 8388608l);
        longCheckDiv("ldiv_by_pow2_24", ldiv_by_pow2_24(x), x, 16777216l);
        longCheckDiv("ldiv_by_pow2_25", ldiv_by_pow2_25(x), x, 33554432l);
        longCheckDiv("ldiv_by_pow2_26", ldiv_by_pow2_26(x), x, 67108864l);
        longCheckDiv("ldiv_by_pow2_27", ldiv_by_pow2_27(x), x, 134217728l);
        longCheckDiv("ldiv_by_pow2_28", ldiv_by_pow2_28(x), x, 268435456l);
        longCheckDiv("ldiv_by_pow2_29", ldiv_by_pow2_29(x), x, 536870912l);
        longCheckDiv("ldiv_by_pow2_30", ldiv_by_pow2_30(x), x, 1073741824l);
        longCheckDiv("ldiv_by_pow2_31", ldiv_by_pow2_31(x), x, 2147483648l);
        longCheckDiv("ldiv_by_pow2_32", ldiv_by_pow2_32(x), x, 4294967296l);
        longCheckDiv("ldiv_by_pow2_33", ldiv_by_pow2_33(x), x, 8589934592l);
        longCheckDiv("ldiv_by_pow2_34", ldiv_by_pow2_34(x), x, 17179869184l);
        longCheckDiv("ldiv_by_pow2_35", ldiv_by_pow2_35(x), x, 34359738368l);
        longCheckDiv("ldiv_by_pow2_36", ldiv_by_pow2_36(x), x, 68719476736l);
        longCheckDiv("ldiv_by_pow2_37", ldiv_by_pow2_37(x), x, 137438953472l);
        longCheckDiv("ldiv_by_pow2_38", ldiv_by_pow2_38(x), x, 274877906944l);
        longCheckDiv("ldiv_by_pow2_39", ldiv_by_pow2_39(x), x, 549755813888l);
        longCheckDiv("ldiv_by_pow2_40", ldiv_by_pow2_40(x), x, 1099511627776l);
        longCheckDiv("ldiv_by_pow2_41", ldiv_by_pow2_41(x), x, 2199023255552l);
        longCheckDiv("ldiv_by_pow2_42", ldiv_by_pow2_42(x), x, 4398046511104l);
        longCheckDiv("ldiv_by_pow2_43", ldiv_by_pow2_43(x), x, 8796093022208l);
        longCheckDiv("ldiv_by_pow2_44", ldiv_by_pow2_44(x), x, 17592186044416l);
        longCheckDiv("ldiv_by_pow2_45", ldiv_by_pow2_45(x), x, 35184372088832l);
        longCheckDiv("ldiv_by_pow2_46", ldiv_by_pow2_46(x), x, 70368744177664l);
        longCheckDiv("ldiv_by_pow2_47", ldiv_by_pow2_47(x), x, 140737488355328l);
        longCheckDiv("ldiv_by_pow2_48", ldiv_by_pow2_48(x), x, 281474976710656l);
        longCheckDiv("ldiv_by_pow2_49", ldiv_by_pow2_49(x), x, 562949953421312l);
        longCheckDiv("ldiv_by_pow2_50", ldiv_by_pow2_50(x), x, 1125899906842624l);
        longCheckDiv("ldiv_by_pow2_51", ldiv_by_pow2_51(x), x, 2251799813685248l);
        longCheckDiv("ldiv_by_pow2_52", ldiv_by_pow2_52(x), x, 4503599627370496l);
        longCheckDiv("ldiv_by_pow2_53", ldiv_by_pow2_53(x), x, 9007199254740992l);
        longCheckDiv("ldiv_by_pow2_54", ldiv_by_pow2_54(x), x, 18014398509481984l);
        longCheckDiv("ldiv_by_pow2_55", ldiv_by_pow2_55(x), x, 36028797018963968l);
        longCheckDiv("ldiv_by_pow2_56", ldiv_by_pow2_56(x), x, 72057594037927936l);
        longCheckDiv("ldiv_by_pow2_57", ldiv_by_pow2_57(x), x, 144115188075855872l);
        longCheckDiv("ldiv_by_pow2_58", ldiv_by_pow2_58(x), x, 288230376151711744l);
        longCheckDiv("ldiv_by_pow2_59", ldiv_by_pow2_59(x), x, 576460752303423488l);
        longCheckDiv("ldiv_by_pow2_60", ldiv_by_pow2_60(x), x, 1152921504606846976l);
        longCheckDiv("ldiv_by_pow2_61", ldiv_by_pow2_61(x), x, 2305843009213693952l);
        longCheckDiv("ldiv_by_pow2_62", ldiv_by_pow2_62(x), x, 4611686018427387904l);
        longCheckDiv("ldiv_by_small_0", ldiv_by_small_0(x), x, 3l);
        longCheckDiv("ldiv_by_small_1", ldiv_by_small_1(x), x, 5l);
        longCheckDiv("ldiv_by_small_2", ldiv_by_small_2(x), x, 6l);
        longCheckDiv("ldiv_by_small_3", ldiv_by_small_3(x), x, 7l);
        longCheckDiv("ldiv_by_small_4", ldiv_by_small_4(x), x, 9l);
        longCheckDiv("ldiv_by_small_5", ldiv_by_small_5(x), x, 10l);
        longCheckDiv("ldiv_by_small_6", ldiv_by_small_6(x), x, 11l);
        longCheckDiv("ldiv_by_small_7", ldiv_by_small_7(x), x, 12l);
        longCheckDiv("ldiv_by_small_8", ldiv_by_small_8(x), x, 13l);
        longCheckDiv("ldiv_by_small_9", ldiv_by_small_9(x), x, 14l);
        longCheckDiv("ldiv_by_small_10", ldiv_by_small_10(x), x, 15l);
        longCheckRem("lrem_by_pow2_0", lrem_by_pow2_0(x), x, 1l);
        longCheckRem("lrem_by_pow2_1", lrem_by_pow2_1(x), x, 2l);
        longCheckRem("lrem_by_pow2_2", lrem_by_pow2_2(x), x, 4l);
        longCheckRem("lrem_by_pow2_3", lrem_by_pow2_3(x), x, 8l);
        longCheckRem("lrem_by_pow2_4", lrem_by_pow2_4(x), x, 16l);
        longCheckRem("lrem_by_pow2_5", lrem_by_pow2_5(x), x, 32l);
        longCheckRem("lrem_by_pow2_6", lrem_by_pow2_6(x), x, 64l);
        longCheckRem("lrem_by_pow2_7", lrem_by_pow2_7(x), x, 128l);
        longCheckRem("lrem_by_pow2_8", lrem_by_pow2_8(x), x, 256l);
        longCheckRem("lrem_by_pow2_9", lrem_by_pow2_9(x), x, 512l);
        longCheckRem("lrem_by_pow2_10", lrem_by_pow2_10(x), x, 1024l);
        longCheckRem("lrem_by_pow2_11", lrem_by_pow2_11(x), x, 2048l);
        longCheckRem("lrem_by_pow2_12", lrem_by_pow2_12(x), x, 4096l);
        longCheckRem("lrem_by_pow2_13", lrem_by_pow2_13(x), x, 8192l);
        longCheckRem("lrem_by_pow2_14", lrem_by_pow2_14(x), x, 16384l);
        longCheckRem("lrem_by_pow2_15", lrem_by_pow2_15(x), x, 32768l);
        longCheckRem("lrem_by_pow2_16", lrem_by_pow2_16(x), x, 65536l);
        longCheckRem("lrem_by_pow2_17", lrem_by_pow2_17(x), x, 131072l);
        longCheckRem("lrem_by_pow2_18", lrem_by_pow2_18(x), x, 262144l);
        longCheckRem("lrem_by_pow2_19", lrem_by_pow2_19(x), x, 524288l);
        longCheckRem("lrem_by_pow2_20", lrem_by_pow2_20(x), x, 1048576l);
        longCheckRem("lrem_by_pow2_21", lrem_by_pow2_21(x), x, 2097152l);
        longCheckRem("lrem_by_pow2_22", lrem_by_pow2_22(x), x, 4194304l);
        longCheckRem("lrem_by_pow2_23", lrem_by_pow2_23(x), x, 8388608l);
        longCheckRem("lrem_by_pow2_24", lrem_by_pow2_24(x), x, 16777216l);
        longCheckRem("lrem_by_pow2_25", lrem_by_pow2_25(x), x, 33554432l);
        longCheckRem("lrem_by_pow2_26", lrem_by_pow2_26(x), x, 67108864l);
        longCheckRem("lrem_by_pow2_27", lrem_by_pow2_27(x), x, 134217728l);
        longCheckRem("lrem_by_pow2_28", lrem_by_pow2_28(x), x, 268435456l);
        longCheckRem("lrem_by_pow2_29", lrem_by_pow2_29(x), x, 536870912l);
        longCheckRem("lrem_by_pow2_30", lrem_by_pow2_30(x), x, 1073741824l);
        longCheckRem("lrem_by_pow2_31", lrem_by_pow2_31(x), x, 2147483648l);
        longCheckRem("lrem_by_pow2_32", lrem_by_pow2_32(x), x, 4294967296l);
        longCheckRem("lrem_by_pow2_33", lrem_by_pow2_33(x), x, 8589934592l);
        longCheckRem("lrem_by_pow2_34", lrem_by_pow2_34(x), x, 17179869184l);
        longCheckRem("lrem_by_pow2_35", lrem_by_pow2_35(x), x, 34359738368l);
        longCheckRem("lrem_by_pow2_36", lrem_by_pow2_36(x), x, 68719476736l);
        longCheckRem("lrem_by_pow2_37", lrem_by_pow2_37(x), x, 137438953472l);
        longCheckRem("lrem_by_pow2_38", lrem_by_pow2_38(x), x, 274877906944l);
        longCheckRem("lrem_by_pow2_39", lrem_by_pow2_39(x), x, 549755813888l);
        longCheckRem("lrem_by_pow2_40", lrem_by_pow2_40(x), x, 1099511627776l);
        longCheckRem("lrem_by_pow2_41", lrem_by_pow2_41(x), x, 2199023255552l);
        longCheckRem("lrem_by_pow2_42", lrem_by_pow2_42(x), x, 4398046511104l);
        longCheckRem("lrem_by_pow2_43", lrem_by_pow2_43(x), x, 8796093022208l);
        longCheckRem("lrem_by_pow2_44", lrem_by_pow2_44(x), x, 17592186044416l);
        longCheckRem("lrem_by_pow2_45", lrem_by_pow2_45(x), x, 35184372088832l);
        longCheckRem("lrem_by_pow2_46", lrem_by_pow2_46(x), x, 70368744177664l);
        longCheckRem("lrem_by_pow2_47", lrem_by_pow2_47(x), x, 140737488355328l);
        longCheckRem("lrem_by_pow2_48", lrem_by_pow2_48(x), x, 281474976710656l);
        longCheckRem("lrem_by_pow2_49", lrem_by_pow2_49(x), x, 562949953421312l);
        longCheckRem("lrem_by_pow2_50", lrem_by_pow2_50(x), x, 1125899906842624l);
        longCheckRem("lrem_by_pow2_51", lrem_by_pow2_51(x), x, 2251799813685248l);
        longCheckRem("lrem_by_pow2_52", lrem_by_pow2_52(x), x, 4503599627370496l);
        longCheckRem("lrem_by_pow2_53", lrem_by_pow2_53(x), x, 9007199254740992l);
        longCheckRem("lrem_by_pow2_54", lrem_by_pow2_54(x), x, 18014398509481984l);
        longCheckRem("lrem_by_pow2_55", lrem_by_pow2_55(x), x, 36028797018963968l);
        longCheckRem("lrem_by_pow2_56", lrem_by_pow2_56(x), x, 72057594037927936l);
        longCheckRem("lrem_by_pow2_57", lrem_by_pow2_57(x), x, 144115188075855872l);
        longCheckRem("lrem_by_pow2_58", lrem_by_pow2_58(x), x, 288230376151711744l);
        longCheckRem("lrem_by_pow2_59", lrem_by_pow2_59(x), x, 576460752303423488l);
        longCheckRem("lrem_by_pow2_60", lrem_by_pow2_60(x), x, 1152921504606846976l);
        longCheckRem("lrem_by_pow2_61", lrem_by_pow2_61(x), x, 2305843009213693952l);
        longCheckRem("lrem_by_pow2_62", lrem_by_pow2_62(x), x, 4611686018427387904l);
    }

    public static void main(String[] args) {
      int i;
      long l;

      System.out.println("Begin");

      System.out.println("Int: checking some equally spaced dividends...");
      for (i = -1000; i < 1000; i += 300) {
          intCheckAll(i);
          intCheckAll(-i);
      }

      System.out.println("Int: checking small dividends...");
      for (i = 1; i < 100; i += 1) {
          intCheckAll(i);
          intCheckAll(-i);
      }

      System.out.println("Int: checking big dividends...");
      for (i = 0; i < 100; i += 1) {
          intCheckAll(Integer.MAX_VALUE - i);
          intCheckAll(Integer.MIN_VALUE + i);
      }

      System.out.println("Long: checking some equally spaced dividends...");
      for (l = 0l; l < 1000000000000l; l += 300000000000l) {
          longCheckAll(l);
          longCheckAll(-l);
      }

      System.out.println("Long: checking small dividends...");
      for (l = 1l; l < 100l; l += 1l) {
          longCheckAll(l);
          longCheckAll(-l);
      }

      System.out.println("Long: checking big dividends...");
      for (l = 0l; l < 100l; l += 1l) {
          longCheckAll(Long.MAX_VALUE - l);
          longCheckAll(Long.MIN_VALUE + l);
      }

      System.out.println("End");
    }
}
