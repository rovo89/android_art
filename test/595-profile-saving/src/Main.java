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

import java.io.File;
import java.io.IOException;
import java.lang.reflect.Method;

public class Main {

  public static void main(String[] args) throws Exception {
    System.loadLibrary(args[0]);

    File file = null;
    try {
      file = createTempFile();
      // String codePath = getDexBaseLocation();
      String codePath = System.getenv("DEX_LOCATION") + "/595-profile-saving.jar";
      VMRuntime.registerAppInfo(file.getPath(),
                                System.getenv("DEX_LOCATION"),
                                new String[] {codePath},
                                /* foreignProfileDir */ null);

      int methodIdx = $opt$noinline$testProfile();
      ensureProfileProcessing();
      if (!presentInProfile(file.getPath(), methodIdx)) {
        throw new RuntimeException("Method with index " + methodIdx + " not in the profile");
      }
    } finally {
      if (file != null) {
        file.delete();
      }
    }
  }

  public static int $opt$noinline$testProfile() {
    if (doThrow) throw new Error();
    // Make sure we have a profile info for this method without the need to loop.
    return ensureProfilingInfo("$opt$noinline$testProfile");
  }

  // Return the dex method index.
  public static native int ensureProfilingInfo(String methodName);
  // Ensures the profile saver does its usual processing.
  public static native void ensureProfileProcessing();
  // Checks if the profiles saver knows about the method.
  public static native boolean presentInProfile(String profile, int methodIdx);

  public static boolean doThrow = false;
  private static final String TEMP_FILE_NAME_PREFIX = "dummy";
  private static final String TEMP_FILE_NAME_SUFFIX = "-file";

  static native String getProfileInfoDump(
      String filename);

  private static File createTempFile() throws Exception {
    try {
      return File.createTempFile(TEMP_FILE_NAME_PREFIX, TEMP_FILE_NAME_SUFFIX);
    } catch (IOException e) {
      System.setProperty("java.io.tmpdir", "/data/local/tmp");
      try {
        return File.createTempFile(TEMP_FILE_NAME_PREFIX, TEMP_FILE_NAME_SUFFIX);
      } catch (IOException e2) {
        System.setProperty("java.io.tmpdir", "/sdcard");
        return File.createTempFile(TEMP_FILE_NAME_PREFIX, TEMP_FILE_NAME_SUFFIX);
      }
    }
  }

  private static class VMRuntime {
    private static final Method registerAppInfoMethod;
    static {
      try {
        Class<? extends Object> c = Class.forName("dalvik.system.VMRuntime");
        registerAppInfoMethod = c.getDeclaredMethod("registerAppInfo",
            String.class, String.class, String[].class, String.class);
      } catch (Exception e) {
        throw new RuntimeException(e);
      }
    }

    public static void registerAppInfo(String profile, String appDir,
                                       String[] codePaths, String foreignDir) throws Exception {
      registerAppInfoMethod.invoke(null, profile, appDir, codePaths, foreignDir);
    }
  }
}
