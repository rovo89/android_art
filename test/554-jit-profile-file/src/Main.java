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

import java.io.File;
import java.io.IOException;
import java.lang.reflect.Method;
import java.util.HashMap;

public class Main {

  public void coldMethod() {
    hotMethod();
  }

  public String hotMethod() {
    HashMap<String, String> map = new HashMap<String, String>();
    for (int i = 0; i < 10; i++) {
      map.put("" + i, "" + i + 1);
    }
    return map.get("1");
  }

  private static final String PKG_NAME = "test.package";
  private static final String APP_DIR_PREFIX = "app_dir_";
  private static final String CODE_CACHE = "code_cache";
  private static final String PROFILE_FILE = PKG_NAME + ".prof";
  private static final String TEMP_FILE_NAME_PREFIX = "dummy";
  private static final String TEMP_FILE_NAME_SUFFIX = "-file";
  private static final int JIT_INVOCATION_COUNT = 101;

  /* needs to match Runtime:: kProfileBackground */
  private static final int PROFILE_BACKGROUND = 1;

  public static void main(String[] args) throws Exception {
    System.loadLibrary(args[0]);

    File file = null;
    File appDir = null;
    File profileDir = null;
    File profileFile = null;
    try {
      // We don't know where we have rights to create the code_cache. So create
      // a dummy temporary file and get its parent directory. That will serve as
      // the app directory.
      file = createTempFile();
      appDir = new File(file.getParent(), APP_DIR_PREFIX + file.getName());
      appDir.mkdir();
      profileDir = new File(appDir, CODE_CACHE);
      profileDir.mkdir();

      // Registering the app info will set the profile file name.
      VMRuntime.registerAppInfo(PKG_NAME, appDir.getPath());

      // Make sure the hot methods are jitted.
      Main m = new Main();
      OtherDex o = new OtherDex();
      for (int i = 0; i < JIT_INVOCATION_COUNT; i++) {
        m.hotMethod();
        o.hotMethod();
      }

      // Sleep for 1 second to make sure that the methods had a chance to get compiled.
      Thread.sleep(1000);
      // Updating the process state to BACKGROUND will trigger profile saving.
      VMRuntime.updateProcessState(PROFILE_BACKGROUND);

      // Check that the profile file exists.
      profileFile = new File(profileDir, PROFILE_FILE);
      if (!profileFile.exists()) {
        throw new RuntimeException("No profile file found");
      }
      // Dump the profile file.
      // We know what methods are hot and we compare with the golden `expected` output.
      System.out.println(getProfileInfoDump(profileFile.getPath()));
    } finally {
      if (file != null) {
        file.delete();
      }
      if (profileFile != null) {
        profileFile.delete();
      }
      if (profileDir != null) {
        profileDir.delete();
      }
      if (appDir != null) {
        appDir.delete();
      }
    }
  }

  private static class VMRuntime {
    private static final Method registerAppInfoMethod;
    private static final Method updateProcessStateMethod;
    private static final Method getRuntimeMethod;
    static {
      try {
        Class c = Class.forName("dalvik.system.VMRuntime");
        registerAppInfoMethod = c.getDeclaredMethod("registerAppInfo",
            String.class, String.class, String.class);
        updateProcessStateMethod = c.getDeclaredMethod("updateProcessState", Integer.TYPE);
        getRuntimeMethod = c.getDeclaredMethod("getRuntime");
      } catch (Exception e) {
        throw new RuntimeException(e);
      }
    }

    public static void registerAppInfo(String pkgName, String appDir) throws Exception {
      registerAppInfoMethod.invoke(null, pkgName, appDir, null);
    }
    public static void updateProcessState(int state) throws Exception {
      Object runtime = getRuntimeMethod.invoke(null);
      updateProcessStateMethod.invoke(runtime, state);
    }
  }

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
}
