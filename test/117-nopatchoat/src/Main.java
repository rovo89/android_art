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
  public static void main(String[] args) {
    boolean executable_correct = (isPic() ?
                                  hasExecutableOat() == true :
                                  hasExecutableOat() == isDex2OatEnabled());

    System.out.println(
        "dex2oat & patchoat are " + ((isDex2OatEnabled()) ? "enabled" : "disabled") +
        ", has oat is " + hasOat() + ", has executable oat is " + (
        executable_correct ? "expected" : "not expected") + ".");

    if (!hasOat() && isDex2OatEnabled()) {
      throw new Error("Application with dex2oat enabled runs without an oat file");
    }

    System.out.println(functionCall());
  }

  public static String functionCall() {
    String arr[] = {"This", "is", "a", "function", "call"};
    String ret = "";
    for (int i = 0; i < arr.length; i++) {
      ret = ret + arr[i] + " ";
    }
    return ret.substring(0, ret.length() - 1);
  }

  static {
    System.loadLibrary("arttest");
  }

  private native static boolean isDex2OatEnabled();

  private native static boolean isPic();

  private native static boolean hasOat();

  private native static boolean hasExecutableOat();
}
