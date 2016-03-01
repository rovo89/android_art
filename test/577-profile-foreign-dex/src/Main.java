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
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.lang.reflect.Method;
import java.lang.reflect.Constructor;
import java.util.HashMap;

public class Main {

  private static final String PROFILE_NAME = "primary.prof";
  private static final String APP_DIR_PREFIX = "app_dir_";
  private static final String FOREIGN_DEX_PROFILE_DIR = "foreign-dex";
  private static final String TEMP_FILE_NAME_PREFIX = "dummy";
  private static final String TEMP_FILE_NAME_SUFFIX = "-file";

  public static void main(String[] args) throws Exception {
    File tmpFile = null;
    File appDir = null;
    File profileFile = null;
    File foreignDexProfileDir = null;

    try {
      // Create the necessary files layout.
      tmpFile = createTempFile();
      appDir = new File(tmpFile.getParent(), APP_DIR_PREFIX + tmpFile.getName());
      appDir.mkdir();
      foreignDexProfileDir = new File(tmpFile.getParent(), FOREIGN_DEX_PROFILE_DIR);
      foreignDexProfileDir.mkdir();
      profileFile = createTempFile();

      String codePath = System.getenv("DEX_LOCATION") + "/577-profile-foreign-dex.jar";

      // Register the app with the runtime
      VMRuntime.registerAppInfo(profileFile.getPath(), appDir.getPath(),
             new String[] { codePath }, foreignDexProfileDir.getPath());

      testMarkerForForeignDex(foreignDexProfileDir);
      testMarkerForCodePath(foreignDexProfileDir);
      testMarkerForApplicationDexFile(foreignDexProfileDir, appDir);
    } finally {
      if (tmpFile != null) {
        tmpFile.delete();
      }
      if (profileFile != null) {
        profileFile.delete();
      }
      if (foreignDexProfileDir != null) {
        foreignDexProfileDir.delete();
      }
      if (appDir != null) {
        appDir.delete();
      }
    }
  }

  // Verify we actually create a marker on disk for foreign dex files.
  private static void testMarkerForForeignDex(File foreignDexProfileDir) throws Exception {
    String foreignDex = System.getenv("DEX_LOCATION") + "/577-profile-foreign-dex-ex.jar";
    loadDexFile(foreignDex);
    checkMarker(foreignDexProfileDir, foreignDex, /* exists */ true);
  }

  // Verify we do not create a marker on disk for dex files path of the code path.
  private static void testMarkerForCodePath(File foreignDexProfileDir) throws Exception {
    String codePath = System.getenv("DEX_LOCATION") + "/577-profile-foreign-dex.jar";
    loadDexFile(codePath);
    checkMarker(foreignDexProfileDir, codePath, /* exists */ false);
  }

  private static void testMarkerForApplicationDexFile(File foreignDexProfileDir, File appDir)
      throws Exception {
    // Copy the -ex jar to the application directory and load it from there.
    // This will record duplicate class conflicts but we don't care for this use case.
    File foreignDex = new File(System.getenv("DEX_LOCATION") + "/577-profile-foreign-dex-ex.jar");
    File appDex = new File(appDir, "appDex.jar");
    try {
      copyFile(foreignDex, appDex);

      loadDexFile(appDex.getAbsolutePath());
      checkMarker(foreignDexProfileDir, appDex.getAbsolutePath(), /* exists */ false);
    } finally {
      if (appDex != null) {
        appDex.delete();
      }
    }
  }

  private static void checkMarker(File foreignDexProfileDir, String dexFile, boolean exists) {
    File marker = new File(foreignDexProfileDir, dexFile.replace('/', '@'));
    boolean result_ok = exists ? marker.exists() : !marker.exists();
    if (!result_ok) {
      throw new RuntimeException("Marker test failed for:" + marker.getPath());
    }
  }

  private static void loadDexFile(String dexFile) throws Exception {
    Class pathClassLoader = Class.forName("dalvik.system.PathClassLoader");
    if (pathClassLoader == null) {
        throw new RuntimeException("Couldn't find path class loader class");
    }
    Constructor constructor =
        pathClassLoader.getDeclaredConstructor(String.class, ClassLoader.class);
    constructor.newInstance(
            dexFile, ClassLoader.getSystemClassLoader());
  }

  private static class VMRuntime {
    private static final Method registerAppInfoMethod;
    static {
      try {
        Class c = Class.forName("dalvik.system.VMRuntime");
        registerAppInfoMethod = c.getDeclaredMethod("registerAppInfo",
            String.class, String.class, String[].class, String.class);
      } catch (Exception e) {
        throw new RuntimeException(e);
      }
    }

    public static void registerAppInfo(String pkgName, String appDir,
        String[] codePath, String foreignDexProfileDir) throws Exception {
      registerAppInfoMethod.invoke(null, pkgName, appDir, codePath, foreignDexProfileDir);
    }
  }

  private static void copyFile(File fromFile, File toFile) throws Exception {
    FileInputStream in = new FileInputStream(fromFile);
    FileOutputStream out = new FileOutputStream(toFile);
    try {
      byte[] buffer = new byte[4096];
      int bytesRead;
      while ((bytesRead = in.read(buffer)) >= 0) {
          out.write(buffer, 0, bytesRead);
      }
    } finally {
      out.flush();
      try {
          out.getFD().sync();
      } catch (IOException e) {
      }
      out.close();
      in.close();
    }
  }

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
