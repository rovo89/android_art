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

package com.android.ahat;

import com.android.tools.perflib.heap.ClassObj;
import com.android.tools.perflib.heap.Field;
import com.android.tools.perflib.heap.Instance;
import java.io.File;
import java.io.IOException;
import java.util.Map;

/**
 * The TestDump class is used to get an AhatSnapshot for the test-dump
 * program.
 */
public class TestDump {
  // It can take on the order of a second to parse and process the test-dump
  // hprof. To avoid repeating this overhead for each test case, we cache the
  // loaded instance of TestDump and reuse it when possible. In theory the
  // test cases should not be able to modify the cached snapshot in a way that
  // is visible to other test cases.
  private static TestDump mCachedTestDump = null;

  private AhatSnapshot mSnapshot = null;

  /**
   * Load the test-dump.hprof file.
   * The location of the file is read from the system property
   * "ahat.test.dump.hprof", which is expected to be set on the command line.
   * For example:
   *   java -Dahat.test.dump.hprof=test-dump.hprof -jar ahat-tests.jar
   *
   * An IOException is thrown if there is a failure reading the hprof file.
   */
  private TestDump() throws IOException {
      String hprof = System.getProperty("ahat.test.dump.hprof");
      mSnapshot = AhatSnapshot.fromHprof(new File(hprof));
  }

  /**
   * Get the AhatSnapshot for the test dump program.
   */
  public AhatSnapshot getAhatSnapshot() {
    return mSnapshot;
  }

  /**
   * Return the value of a field in the DumpedStuff instance in the
   * snapshot for the test-dump program.
   */
  public Object getDumpedThing(String name) {
    ClassObj main = mSnapshot.findClass("Main");
    Instance stuff = null;
    for (Map.Entry<Field, Object> fields : main.getStaticFieldValues().entrySet()) {
      if ("stuff".equals(fields.getKey().getName())) {
        stuff = (Instance) fields.getValue();
      }
    }
    return InstanceUtils.getField(stuff, name);
  }

  /**
   * Get the test dump.
   * An IOException is thrown if there is an error reading the test dump hprof
   * file.
   * To improve performance, this returns a cached instance of the TestDump
   * when possible.
   */
  public static synchronized TestDump getTestDump() throws IOException {
    if (mCachedTestDump == null) {
      mCachedTestDump = new TestDump();
    }
    return mCachedTestDump;
  }
}
