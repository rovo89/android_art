/*
 * Copyright (C) 2015 The Android Open Source Project
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

  private volatile Object objectField;
  private volatile int intField;
  private volatile float floatField;
  private volatile long longField;
  private volatile double doubleField;
  private volatile byte byteField;
  private volatile boolean booleanField;
  private volatile char charField;
  private volatile short shortField;

  public static void $opt$setObjectField(Main m) {
    m.objectField = null;
  }

  public static void $opt$setIntField(Main m) {
    m.intField = 0;
  }

  public static void $opt$setFloatField(Main m) {
    m.floatField = 0;
  }

  public static void $opt$setLongField(Main m) {
    m.longField = 0;
  }

  public static void $opt$setDoubleField(Main m) {
    m.doubleField = 0;
  }

  public static void $opt$setByteField(Main m) {
    m.byteField = 0;
  }

  public static void $opt$setBooleanField(Main m) {
    m.booleanField = false;
  }

  public static void $opt$setCharField(Main m) {
    m.charField = 0;
  }

  public static void $opt$setShortField(Main m) {
    m.shortField = 0;
  }

  public static Object $opt$getObjectField(Main m) {
    return m.objectField;
  }

  public static int $opt$getIntField(Main m) {
    return m.intField;
  }

  public static float $opt$getFloatField(Main m) {
    return m.floatField;
  }

  public static long $opt$getLongField(Main m) {
    return m.longField;
  }

  public static double $opt$getDoubleField(Main m) {
    return m.doubleField;
  }

  public static byte $opt$getByteField(Main m) {
    return m.byteField;
  }

  public static boolean $opt$getBooleanField(Main m) {
    return m.booleanField;
  }

  public static char $opt$getCharField(Main m) {
    return m.charField;
  }

  public static short $opt$getShortField(Main m) {
    return m.shortField;
  }

  public static void main(String[] args) {
    int methodLine = 30;
    int thisLine = 103;
    try {
      $opt$setObjectField(null);
      throw new RuntimeException("Failed to throw NullPointerException.");
    } catch (NullPointerException npe) {
      check(npe, thisLine += 2, methodLine, "$opt$setObjectField");
    }
    try {
      $opt$setIntField(null);
      throw new RuntimeException("Failed to throw NullPointerException.");
    } catch (NullPointerException npe) {
      check(npe, thisLine += 6, methodLine += 4, "$opt$setIntField");
    }
    try {
      $opt$setFloatField(null);
      throw new RuntimeException("Failed to throw NullPointerException.");
    } catch (NullPointerException npe) {
      check(npe, thisLine += 6, methodLine += 4, "$opt$setFloatField");
    }
    try {
      $opt$setLongField(null);
      throw new RuntimeException("Failed to throw NullPointerException.");
    } catch (NullPointerException npe) {
      check(npe, thisLine += 6, methodLine += 4, "$opt$setLongField");
    }
    try {
      $opt$setDoubleField(null);
      throw new RuntimeException("Failed to throw NullPointerException.");
    } catch (NullPointerException npe) {
      check(npe, thisLine += 6, methodLine += 4, "$opt$setDoubleField");
    }
    try {
      $opt$setByteField(null);
      throw new RuntimeException("Failed to throw NullPointerException.");
    } catch (NullPointerException npe) {
      check(npe, thisLine += 6, methodLine += 4, "$opt$setByteField");
    }
    try {
      $opt$setBooleanField(null);
      throw new RuntimeException("Failed to throw NullPointerException.");
    } catch (NullPointerException npe) {
      check(npe, thisLine += 6, methodLine += 4, "$opt$setBooleanField");
    }
    try {
      $opt$setCharField(null);
      throw new RuntimeException("Failed to throw NullPointerException.");
    } catch (NullPointerException npe) {
      check(npe, thisLine += 6, methodLine += 4, "$opt$setCharField");
    }
    try {
      $opt$setShortField(null);
      throw new RuntimeException("Failed to throw NullPointerException.");
    } catch (NullPointerException npe) {
      check(npe, thisLine += 6, methodLine += 4, "$opt$setShortField");
    }
    try {
      $opt$getObjectField(null);
      throw new RuntimeException("Failed to throw NullPointerException.");
    } catch (NullPointerException npe) {
      check(npe, thisLine += 6, methodLine += 4, "$opt$getObjectField");
    }
    try {
      $opt$getIntField(null);
      throw new RuntimeException("Failed to throw NullPointerException.");
    } catch (NullPointerException npe) {
      check(npe, thisLine += 6, methodLine += 4, "$opt$getIntField");
    }
    try {
      $opt$getFloatField(null);
      throw new RuntimeException("Failed to throw NullPointerException.");
    } catch (NullPointerException npe) {
      check(npe, thisLine += 6, methodLine += 4, "$opt$getFloatField");
    }
    try {
      $opt$getLongField(null);
      throw new RuntimeException("Failed to throw NullPointerException.");
    } catch (NullPointerException npe) {
      check(npe, thisLine += 6, methodLine += 4, "$opt$getLongField");
    }
    try {
      $opt$getDoubleField(null);
      throw new RuntimeException("Failed to throw NullPointerException.");
    } catch (NullPointerException npe) {
      check(npe, thisLine += 6, methodLine += 4, "$opt$getDoubleField");
    }
    try {
      $opt$getByteField(null);
      throw new RuntimeException("Failed to throw NullPointerException.");
    } catch (NullPointerException npe) {
      check(npe, thisLine += 6, methodLine += 4, "$opt$getByteField");
    }
    try {
      $opt$getBooleanField(null);
      throw new RuntimeException("Failed to throw NullPointerException.");
    } catch (NullPointerException npe) {
      check(npe, thisLine += 6, methodLine += 4, "$opt$getBooleanField");
    }
    try {
      $opt$getCharField(null);
      throw new RuntimeException("Failed to throw NullPointerException.");
    } catch (NullPointerException npe) {
      check(npe, thisLine += 6, methodLine += 4, "$opt$getCharField");
    }
    try {
      $opt$getShortField(null);
      throw new RuntimeException("Failed to throw NullPointerException.");
    } catch (NullPointerException npe) {
      check(npe, thisLine += 6, methodLine += 4, "$opt$getShortField");
    }
  }

  static void check(NullPointerException npe, int mainLine, int medthodLine, String methodName) {
    System.out.println(methodName);
    StackTraceElement[] trace = npe.getStackTrace();
    checkElement(trace[0], "Main", methodName, "Main.java", medthodLine);
    checkElement(trace[1], "Main", "main", "Main.java", mainLine);
  }

  static void checkElement(StackTraceElement element,
                           String declaringClass, String methodName,
                           String fileName, int lineNumber) {
    assertEquals(declaringClass, element.getClassName());
    assertEquals(methodName, element.getMethodName());
    assertEquals(fileName, element.getFileName());
    assertEquals(lineNumber, element.getLineNumber());
  }

  static void assertEquals(Object expected, Object actual) {
    if (!expected.equals(actual)) {
      String msg = "Expected \"" + expected + "\" but got \"" + actual + "\"";
      throw new AssertionError(msg);
    }
  }

  static void assertEquals(int expected, int actual) {
    if (expected != actual) {
      throw new AssertionError("Expected " + expected + " got " + actual);
    }
  }

}
