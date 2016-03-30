/*
 * Copyright (C) 2016 The Android Open Source Project
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

  public static void main(String args[]) {
    expectEqualsByte((byte)1, booleanToByte(true));
    expectEqualsShort((short)1, booleanToShort(true));
    expectEqualsChar((char)1, booleanToChar(true));
    expectEqualsInt(1, booleanToInt(true));
    expectEqualsLong(1L, booleanToLong(true));

    expectEqualsInt(1, longToIntOfBoolean());

    System.out.println("passed");
  }

  /// CHECK-START: byte Main.booleanToByte(boolean) builder (after)
  /// CHECK:         <<Arg:z\d+>>           ParameterValue
  /// CHECK-DAG:     <<Zero:i\d+>>          IntConstant 0
  /// CHECK-DAG:     <<One:i\d+>>           IntConstant 1
  /// CHECK-DAG:     <<Cond:z\d+>>          Equal [<<Arg>>,<<Zero>>]
  /// CHECK-DAG:                            If [<<Cond>>]
  /// CHECK-DAG:     <<Phi:i\d+>>           Phi [<<One>>,<<Zero>>]
  /// CHECK-DAG:     <<IToS:b\d+>>          TypeConversion [<<Phi>>]
  /// CHECK-DAG:                            Return [<<IToS>>]

  /// CHECK-START: byte Main.booleanToByte(boolean) select_generator (after)
  /// CHECK:         <<Arg:z\d+>>           ParameterValue
  /// CHECK-DAG:     <<Zero:i\d+>>          IntConstant 0
  /// CHECK-DAG:     <<One:i\d+>>           IntConstant 1
  /// CHECK-DAG:     <<Sel:i\d+>>           Select [<<Zero>>,<<One>>,<<Arg>>]
  /// CHECK-DAG:     <<IToS:b\d+>>          TypeConversion [<<Sel>>]
  /// CHECK-DAG:                            Return [<<IToS>>]

  /// CHECK-START: byte Main.booleanToByte(boolean) instruction_simplifier_after_bce (after)
  /// CHECK:         <<Arg:z\d+>>           ParameterValue
  /// CHECK-DAG:                            Return [<<Arg>>]

  static byte booleanToByte(boolean b) {
    return (byte)(b ? 1 : 0);
  }

  /// CHECK-START: short Main.booleanToShort(boolean) builder (after)
  /// CHECK:         <<Arg:z\d+>>           ParameterValue
  /// CHECK-DAG:     <<Zero:i\d+>>          IntConstant 0
  /// CHECK-DAG:     <<One:i\d+>>           IntConstant 1
  /// CHECK-DAG:     <<Cond:z\d+>>          Equal [<<Arg>>,<<Zero>>]
  /// CHECK-DAG:                            If [<<Cond>>]
  /// CHECK-DAG:     <<Phi:i\d+>>           Phi [<<One>>,<<Zero>>]
  /// CHECK-DAG:     <<IToS:s\d+>>          TypeConversion [<<Phi>>]
  /// CHECK-DAG:                            Return [<<IToS>>]

  /// CHECK-START: short Main.booleanToShort(boolean) select_generator (after)
  /// CHECK:         <<Arg:z\d+>>           ParameterValue
  /// CHECK-DAG:     <<Zero:i\d+>>          IntConstant 0
  /// CHECK-DAG:     <<One:i\d+>>           IntConstant 1
  /// CHECK-DAG:     <<Sel:i\d+>>           Select [<<Zero>>,<<One>>,<<Arg>>]
  /// CHECK-DAG:     <<IToS:s\d+>>          TypeConversion [<<Sel>>]
  /// CHECK-DAG:                            Return [<<IToS>>]

  /// CHECK-START: short Main.booleanToShort(boolean) instruction_simplifier_after_bce (after)
  /// CHECK:         <<Arg:z\d+>>           ParameterValue
  /// CHECK-DAG:                            Return [<<Arg>>]

  static short booleanToShort(boolean b) {
    return (short)(b ? 1 : 0);
  }

  /// CHECK-START: char Main.booleanToChar(boolean) builder (after)
  /// CHECK:         <<Arg:z\d+>>           ParameterValue
  /// CHECK-DAG:     <<Zero:i\d+>>          IntConstant 0
  /// CHECK-DAG:     <<One:i\d+>>           IntConstant 1
  /// CHECK-DAG:     <<Cond:z\d+>>          Equal [<<Arg>>,<<Zero>>]
  /// CHECK-DAG:                            If [<<Cond>>]
  /// CHECK-DAG:     <<Phi:i\d+>>           Phi [<<One>>,<<Zero>>]
  /// CHECK-DAG:     <<IToC:c\d+>>          TypeConversion [<<Phi>>]
  /// CHECK-DAG:                            Return [<<IToC>>]

  /// CHECK-START: char Main.booleanToChar(boolean) select_generator (after)
  /// CHECK:         <<Arg:z\d+>>           ParameterValue
  /// CHECK-DAG:     <<Zero:i\d+>>          IntConstant 0
  /// CHECK-DAG:     <<One:i\d+>>           IntConstant 1
  /// CHECK-DAG:     <<Sel:i\d+>>           Select [<<Zero>>,<<One>>,<<Arg>>]
  /// CHECK-DAG:     <<IToC:c\d+>>          TypeConversion [<<Sel>>]
  /// CHECK-DAG:                            Return [<<IToC>>]

  /// CHECK-START: char Main.booleanToChar(boolean) instruction_simplifier_after_bce (after)
  /// CHECK:         <<Arg:z\d+>>           ParameterValue
  /// CHECK-DAG:                            Return [<<Arg>>]

  static char booleanToChar(boolean b) {
    return (char)(b ? 1 : 0);
  }

  /// CHECK-START: int Main.booleanToInt(boolean) builder (after)
  /// CHECK:         <<Arg:z\d+>>           ParameterValue
  /// CHECK-DAG:     <<Zero:i\d+>>          IntConstant 0
  /// CHECK-DAG:     <<One:i\d+>>           IntConstant 1
  /// CHECK-DAG:     <<Cond:z\d+>>          Equal [<<Arg>>,<<Zero>>]
  /// CHECK-DAG:                            If [<<Cond>>]
  /// CHECK-DAG:     <<Phi:i\d+>>           Phi [<<One>>,<<Zero>>]
  /// CHECK-DAG:                            Return [<<Phi>>]

  /// CHECK-START: int Main.booleanToInt(boolean) select_generator (after)
  /// CHECK:         <<Arg:z\d+>>           ParameterValue
  /// CHECK-DAG:     <<Zero:i\d+>>          IntConstant 0
  /// CHECK-DAG:     <<One:i\d+>>           IntConstant 1
  /// CHECK-DAG:     <<Sel:i\d+>>           Select [<<Zero>>,<<One>>,<<Arg>>]
  /// CHECK-DAG:                            Return [<<Sel>>]

  /// CHECK-START: int Main.booleanToInt(boolean) instruction_simplifier_after_bce (after)
  /// CHECK:         <<Arg:z\d+>>           ParameterValue
  /// CHECK-DAG:                            Return [<<Arg>>]

  static int booleanToInt(boolean b) {
    return b ? 1 : 0;
  }

  /// CHECK-START: long Main.booleanToLong(boolean) builder (after)
  /// CHECK:         <<Arg:z\d+>>           ParameterValue
  /// CHECK-DAG:     <<Zero:i\d+>>          IntConstant 0
  /// CHECK-DAG:     <<One:i\d+>>           IntConstant 1
  /// CHECK-DAG:     <<Cond:z\d+>>          Equal [<<Arg>>,<<Zero>>]
  /// CHECK-DAG:                            If [<<Cond>>]
  /// CHECK-DAG:     <<Phi:i\d+>>           Phi [<<One>>,<<Zero>>]
  /// CHECK-DAG:     <<IToJ:j\d+>>          TypeConversion [<<Phi>>]
  /// CHECK-DAG:                            Return [<<IToJ>>]

  /// CHECK-START: long Main.booleanToLong(boolean) select_generator (after)
  /// CHECK:         <<Arg:z\d+>>           ParameterValue
  /// CHECK-DAG:     <<Zero:i\d+>>          IntConstant 0
  /// CHECK-DAG:     <<One:i\d+>>           IntConstant 1
  /// CHECK-DAG:     <<Sel:i\d+>>           Select [<<Zero>>,<<One>>,<<Arg>>]
  /// CHECK-DAG:     <<IToJ:j\d+>>          TypeConversion [<<Sel>>]
  /// CHECK-DAG:                            Return [<<IToJ>>]

  /// CHECK-START: long Main.booleanToLong(boolean) instruction_simplifier_after_bce (after)
  /// CHECK:         <<Arg:z\d+>>           ParameterValue
  /// CHECK-DAG:     <<ZToJ:j\d+>>          TypeConversion [<<Arg>>]
  /// CHECK-DAG:                            Return [<<ZToJ>>]

  static long booleanToLong(boolean b) {
    return b ? 1 : 0;
  }

  /// CHECK-START: int Main.longToIntOfBoolean() builder (after)
  /// CHECK-DAG:     <<Method:[ij]\d+>>     CurrentMethod
  /// CHECK-DAG:     <<Sget:z\d+>>          StaticFieldGet
  /// CHECK-DAG:     <<ZToJ:j\d+>>          InvokeStaticOrDirect [<<Sget>>,<<Method>>]
  /// CHECK-DAG:     <<JToI:i\d+>>          TypeConversion [<<ZToJ>>]
  /// CHECK-DAG:                            Return [<<JToI>>]

  /// CHECK-START: int Main.longToIntOfBoolean() inliner (after)
  /// CHECK-DAG:     <<Method:[ij]\d+>>     CurrentMethod
  /// CHECK-DAG:     <<Zero:i\d+>>          IntConstant 0
  /// CHECK-DAG:     <<One:i\d+>>           IntConstant 1
  /// CHECK-DAG:     <<Sget:z\d+>>          StaticFieldGet
  /// CHECK-DAG:                            If [<<Sget>>]
  /// CHECK-DAG:     <<Phi:i\d+>>           Phi [<<One>>,<<Zero>>]
  /// CHECK-DAG:     <<IToJ:j\d+>>          TypeConversion [<<Phi>>]
  /// CHECK-DAG:     <<JToI:i\d+>>          TypeConversion [<<IToJ>>]
  /// CHECK-DAG:                            Return [<<JToI>>]

  /// CHECK-START: int Main.longToIntOfBoolean() select_generator (after)
  /// CHECK-DAG:     <<Method:[ij]\d+>>     CurrentMethod
  /// CHECK-DAG:     <<Zero:i\d+>>          IntConstant 0
  /// CHECK-DAG:     <<One:i\d+>>           IntConstant 1
  /// CHECK-DAG:     <<Sget:z\d+>>          StaticFieldGet
  /// CHECK-DAG:     <<Sel:i\d+>>           Select [<<Zero>>,<<One>>,<<Sget>>]
  /// CHECK-DAG:     <<IToJ:j\d+>>          TypeConversion [<<Sel>>]
  /// CHECK-DAG:     <<JToI:i\d+>>          TypeConversion [<<IToJ>>]
  /// CHECK-DAG:                            Return [<<JToI>>]

  /// CHECK-START: int Main.longToIntOfBoolean() instruction_simplifier_after_bce (after)
  /// CHECK-DAG:     <<Method:[ij]\d+>>     CurrentMethod
  /// CHECK-DAG:     <<Sget:z\d+>>          StaticFieldGet
  /// CHECK-DAG:                            Return [<<Sget>>]

  static int longToIntOfBoolean() {
    long l = booleanToLong(booleanField);
    return (int) l;
  }


  private static void expectEqualsByte(byte expected, byte result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  private static void expectEqualsShort(short expected, short result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  private static void expectEqualsChar(char expected, char result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  private static void expectEqualsInt(int expected, int result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  private static void expectEqualsLong(long expected, long result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }


  public static boolean booleanField = true;

}
