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
    System.logI("hello world");
    long t = System.currentTimeMillis();
    for (int i = 7; i >= 0; --i) {
      int b = ((int) (t >> (i * 8))) & 0xff;
      System.logI(digits[(b >> 4) & 0xf]);
      System.logI(digits[b & 0xf]);
    }
    return 123;
  }

  public static int test2() {
    System.logI("creating char[]...");
    char[] cs = new char[20];
    System.logI("...created char[]");
    String[] digits = new String[] {
      "0","1","2","3","4","5","6","7","8","9","a","b","c","d","e","f",
    };
    long t = System.currentTimeMillis();
    System.logI("creating StringBuilder...");
    StringBuilder sb = new StringBuilder(20);
    System.logI("...created StringBuilder");
    for (int i = 7; i >= 0; --i) {
      int b = ((int) (t >> (i * 8))) & 0xff;
      // TODO: StringBuilder.append(C) works, but StringBuilder.append(Ljava/lang/String;) doesn't.
      System.logI("calling append...");
      sb.append(digits[(b >> 4) & 0xf].charAt(0));
      System.logI("...called append");
      System.logI("calling append...");
      sb.append(digits[b & 0xf].charAt(0));
      System.logI("...called append");
    }
    System.logI("calling toString...");
    String result = sb.toString();
    System.logI("...called toString");
    System.logI(result);
    return 123;
  }
}
