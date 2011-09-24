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

import java.lang.reflect.*;
import java.util.*;

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

  private static boolean z = true;
  private static byte b = 8;
  private static char c = 'x';
  private static double d = Math.PI;
  private static float f = 3.14f;
  private static int i = 32;
  private static long j = 0x0123456789abcdefL;
  private static short s = 16;

  public static int test4() {
    String s = "int=" + i + " long=" + j;
    System.logI(s);
    return 123;
  }

  public static int test5() throws Exception {
    System.logI("new Thread...");
    Thread t = new Thread(new Runnable() {
      public void run() {
        System.logI("hello from a new thread!");
        System.logI(Thread.currentThread().toString());
        try {
          System.logI("sleeping for 2s");
          Thread.sleep(2*1000);
        } catch (Exception ex) { ex.printStackTrace(); }
        System.logI("finished sleeping");
        throw new RuntimeException("uncaught exception");
      }
    });
    System.logI("calling Thread.toString...");
    System.logI(t.toString());
    System.logI("calling Thread.start...");
    t.start();
    //t.join();
    System.logI("...done!");
    return 123;
  }

  public static void test6() throws Exception {
    java.lang.reflect.Field f;

    f = SystemMethods.class.getDeclaredField("z");
    System.out.println(f.getBoolean(null));
    f = SystemMethods.class.getDeclaredField("b");
    System.out.println(f.getByte(null));
    f = SystemMethods.class.getDeclaredField("c");
    System.out.println(f.getChar(null));
    f = SystemMethods.class.getDeclaredField("d");
    System.out.println(f.getDouble(null));
    f = SystemMethods.class.getDeclaredField("f");
    System.out.println(f.getFloat(null));
    f = SystemMethods.class.getDeclaredField("i");
    System.out.println(f.getInt(null));
    f = SystemMethods.class.getDeclaredField("j");
    System.out.println(f.getLong(null));
    f = SystemMethods.class.getDeclaredField("s");
    System.out.println(f.getShort(null));

    f = SystemMethods.class.getDeclaredField("z");
    f.setBoolean(null, false);
    f = SystemMethods.class.getDeclaredField("b");
    f.setByte(null, (byte) 7);
    f = SystemMethods.class.getDeclaredField("c");
    f.setChar(null, 'y');
    f = SystemMethods.class.getDeclaredField("d");
    f.setDouble(null, 2.7);
    f = SystemMethods.class.getDeclaredField("f");
    f.setFloat(null, 2.7f);
    f = SystemMethods.class.getDeclaredField("i");
    f.setInt(null, 31);
    f = SystemMethods.class.getDeclaredField("j");
    f.setLong(null, 63);
    f = SystemMethods.class.getDeclaredField("s");
    f.setShort(null, (short) 15);

    f = SystemMethods.class.getDeclaredField("z");
    System.out.println(f.getBoolean(null));
    f = SystemMethods.class.getDeclaredField("b");
    System.out.println(f.getByte(null));
    f = SystemMethods.class.getDeclaredField("c");
    System.out.println(f.getChar(null));
    f = SystemMethods.class.getDeclaredField("d");
    System.out.println(f.getDouble(null));
    f = SystemMethods.class.getDeclaredField("f");
    System.out.println(f.getFloat(null));
    f = SystemMethods.class.getDeclaredField("i");
    System.out.println(f.getInt(null));
    f = SystemMethods.class.getDeclaredField("j");
    System.out.println(f.getLong(null));
    f = SystemMethods.class.getDeclaredField("s");
    System.out.println(f.getShort(null));

    f = SystemMethods.class.getDeclaredField("z");
    f.set(null, Boolean.valueOf(true));
    f = SystemMethods.class.getDeclaredField("b");
    f.set(null, Byte.valueOf((byte) 6));
    f = SystemMethods.class.getDeclaredField("c");
    f.set(null, Character.valueOf('z'));
    f = SystemMethods.class.getDeclaredField("d");
    f.set(null, Double.valueOf(1.3));
    f = SystemMethods.class.getDeclaredField("f");
    f.set(null, Float.valueOf(1.3f));
    f = SystemMethods.class.getDeclaredField("i");
    f.set(null, Integer.valueOf(30));
    f = SystemMethods.class.getDeclaredField("j");
    f.set(null, Long.valueOf(62));
    f.set(null, Integer.valueOf(62));
    f = SystemMethods.class.getDeclaredField("s");
    f.set(null, Short.valueOf((short) 14));

    f = SystemMethods.class.getDeclaredField("z");
    System.out.println(f.getBoolean(null));
    f = SystemMethods.class.getDeclaredField("b");
    System.out.println(f.getByte(null));
    f = SystemMethods.class.getDeclaredField("c");
    System.out.println(f.getChar(null));
    f = SystemMethods.class.getDeclaredField("d");
    System.out.println(f.getDouble(null));
    f = SystemMethods.class.getDeclaredField("f");
    System.out.println(f.getFloat(null));
    f = SystemMethods.class.getDeclaredField("i");
    System.out.println(f.getInt(null));
    f = SystemMethods.class.getDeclaredField("j");
    System.out.println(f.getLong(null));
    f = SystemMethods.class.getDeclaredField("s");
    System.out.println(f.getShort(null));

    try {
      f = SystemMethods.class.getDeclaredField("s");
      f.set(null, Integer.valueOf(14));
    } catch (Exception ex) {
      ex.printStackTrace();
    }

    f = SystemMethods.class.getDeclaredField("z");
    show(f.get(null));
    f = SystemMethods.class.getDeclaredField("b");
    show(f.get(null));
    f = SystemMethods.class.getDeclaredField("c");
    show(f.get(null));
    f = SystemMethods.class.getDeclaredField("d");
    show(f.get(null));
    f = SystemMethods.class.getDeclaredField("f");
    show(f.get(null));
    f = SystemMethods.class.getDeclaredField("i");
    show(f.get(null));
    f = SystemMethods.class.getDeclaredField("j");
    show(f.get(null));
    f = SystemMethods.class.getDeclaredField("s");
    show(f.get(null));

    /*
    private static boolean z = true;
    private static byte b = 8;
    private static char c = 'x';
    private static double d = Math.PI;
    private static float f = 3.14f;
    private static int i = 32;
    private static long j = 0x0123456789abcdefL;
    private static short s = 16;
    */
  }

  private static void show(Object o) {
    System.out.println(o + " (" + (o != null ? o.getClass() : "null") + ")");
  }

  public static void test7() throws Exception {
    System.out.println(Arrays.toString(String.class.getDeclaredConstructors()));
    System.out.println(Arrays.toString(String.class.getDeclaredFields()));
    System.out.println(Arrays.toString(String.class.getDeclaredMethods()));

    System.out.println(Arrays.toString(SystemMethods.class.getInterfaces()));
    System.out.println(Arrays.toString(String.class.getInterfaces()));

//    System.out.println(SystemMethods.class.getModifiers());
//    System.out.println(String.class.getModifiers());

    System.out.println(String.class.isAssignableFrom(Object.class));
    System.out.println(Object.class.isAssignableFrom(String.class));

    System.out.println(String.class.isInstance("hello"));
    System.out.println(String.class.isInstance(123));

    java.lang.reflect.Method m;

    m = SystemMethods.class.getDeclaredMethod("IV", int.class);
    System.out.println(Arrays.toString(m.getParameterTypes()));
    show(m.invoke(null, 4444));
    System.out.println(Arrays.toString(m.getParameterTypes()));

    m = SystemMethods.class.getDeclaredMethod("IIV", int.class, int.class);
    System.out.println(Arrays.toString(m.getParameterTypes()));
    show(m.invoke(null, 1111, 2222));

    m = SystemMethods.class.getDeclaredMethod("III", int.class, int.class);
    System.out.println(Arrays.toString(m.getParameterTypes()));
    show(m.invoke(null, 1111, 2222));

    m = SystemMethods.class.getDeclaredMethod("sumArray", int[].class);
    System.out.println(Arrays.toString(m.getParameterTypes()));
    show(m.invoke(null, new int[] { 1, 2, 3, 4 }));

    m = SystemMethods.class.getDeclaredMethod("concat", String[].class);
    System.out.println(Arrays.toString(m.getParameterTypes()));
    show(m.invoke(null, (Object) new String[] { "h", "e", "l", "l", "o" }));

    m = SystemMethods.class.getDeclaredMethod("ZBCDFIJSV", boolean.class, byte.class, char.class, double.class, float.class, int.class, long.class, short.class);
    System.out.println(Arrays.toString(m.getParameterTypes()));
    show(m.invoke(null, true, (byte) 0, '1', 2, 3, 4, 5, (short) 6));

    m = SystemMethods.class.getDeclaredMethod("ZBCDLFIJSV", boolean.class, byte.class, char.class, double.class, String.class, float.class, int.class, long.class, short.class);
    System.out.println(Arrays.toString(m.getParameterTypes()));
    show(m.invoke(null, true, (byte) 0, '1', 2, "hello world", 3, 4, 5, (short) 6));

    try {
      m = SystemMethods.class.getDeclaredMethod("thrower");
      System.out.println(Arrays.toString(m.getParameterTypes()));
      show(m.invoke(null));
      System.out.println("************* should have thrown!");
    } catch (Exception ex) {
      ex.printStackTrace();
    }
  }

  private static void thrower() {
    throw new ArithmeticException("surprise!");
  }

  public static int sumArray(int[] xs) {
    int result = 0;
    for (int x : xs) {
      result += x;
    }
    return result;
  }

  public static String concat(String[] strings) {
    String result = "";
    for (String s : strings) {
      result += s;
    }
    return result;
  }

  public static void IV(int i) {
    System.out.println(i);
  }

  public static void IIV(int i, int j) {
    System.out.println(i + " " + j);
  }

  public static int III(int i, int j) {
    System.out.println(i + " " + j);
    return i + j;
  }

  public static void ZBCDFIJSV(boolean z, byte b, char c, double d, float f, int i, long l, short s) {
    System.out.println(z + " " + b + " " + c + " " + d + " " + f + " " + i + " " + l + " " + s);
  }

  public static void ZBCDLFIJSV(boolean z, byte b, char c, double d, String string, float f, int i, long l, short s) {
    System.out.println(z + " " + b + " " + c + " " + d + " " + " " + string + " " + f + " " + i + " " + l + " " + s);
  }

  public static void test8() throws Exception {
    Constructor<?> ctor;

    ctor = String.class.getConstructor(new Class[0]);
    show(ctor.newInstance((Object[]) null));

    ctor = String.class.getConstructor(char[].class, int.class, int.class);
    show(ctor.newInstance(new char[] { 'x', 'y', 'z', '!' }, 1, 2));
  }

  public static void main(String[] args) throws Exception {
    test7();
    test8();
  }
}
