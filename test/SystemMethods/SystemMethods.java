/*
 * Copyright (C) 2011 The Android Open Source Project
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

class SystemMethods {
  public static int test0() {
    System.logI("hello world");
    return 123;
  }

  public static int test1() {
    String[] digits = new String[] {
      "0","1","2","3","4","5","6","7","8","9","a","b","c","d","e","f",
    };
    long t = System.currentTimeMillis();
    for (int i = 7; i >= 0; --i) {
      int b = ((int) (t >> (i * 8))) & 0xff;
      System.logI(digits[(b >> 4) & 0xf]);
      System.logI(digits[b & 0xf]);
    }
    return 123;
  }

  private static String[] STRING_DIGITS = new String[] {
    "0","1","2","3","4","5","6","7","8","9","a","b","c","d","e","f",
  };

  public static int test2() {
    char[] cs = new char[20];
    long t = System.currentTimeMillis();
    StringBuilder sb = new StringBuilder(20);
    for (int i = 7; i >= 0; --i) {
      int b = ((int) (t >> (i * 8))) & 0xff;
      sb.append(STRING_DIGITS[(b >> 4) & 0xf]);
      sb.append(STRING_DIGITS[b & 0xf]);
    }
    String result = sb.toString();
    System.logI(result);
    return 123;
  }

  private static char[] DIGITS = new char[] {
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
    'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j',
    'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't',
    'u', 'v', 'w', 'x', 'y', 'z'
  };

  public static int test3() {
    long t = System.currentTimeMillis();

    long v = t;
    //        int i = (int) v;
    //        if (v >= 0 && i == v) {
    //            return intToHexString(i, false, 0);
    //        }

    int bufLen = 16;  // Max number of hex digits in a long
    char[] buf = new char[bufLen];
    int cursor = bufLen;

    do {
      buf[--cursor] = DIGITS[((int) v) & 0xF];
    } while ((v >>>= 4) != 0);

    String s = new String(buf, cursor, bufLen - cursor);
    System.logI(s);

    System.logI(IntegralToString.longToHexString(t));
    System.logI(Long.toHexString(t));
    System.logI(Long.toString(t));
    return 123;
  }

  private static int i = 4;
  private static long j = 0x0123456789abcdefL;

  public static int test4() {
    String s = "int=" + i + " long=" + j;
    System.logI(s);
    return 123;
  }
}
