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

import com.android.tools.perflib.heap.Instance;
import java.io.IOException;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import org.junit.Test;

public class InstanceUtilsTest {
  @Test
  public void basicString() throws IOException {
    TestDump dump = TestDump.getTestDump();
    Instance str = (Instance)dump.getDumpedThing("basicString");
    assertEquals("hello, world", InstanceUtils.asString(str));
  }

  @Test
  public void nullString() throws IOException {
    TestDump dump = TestDump.getTestDump();
    Instance obj = (Instance)dump.getDumpedThing("nullString");
    assertNull(InstanceUtils.asString(obj));
  }

  @Test
  public void notString() throws IOException {
    TestDump dump = TestDump.getTestDump();
    Instance obj = (Instance)dump.getDumpedThing("anObject");
    assertNotNull(obj);
    assertNull(InstanceUtils.asString(obj));
  }
}
