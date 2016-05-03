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

import dalvik.system.VMDebug;
import java.io.IOException;
import java.lang.ref.PhantomReference;
import java.lang.ref.ReferenceQueue;
import java.lang.ref.WeakReference;
import libcore.util.NativeAllocationRegistry;

/**
 * Program used to create a heap dump for test purposes.
 */
public class Main {
  // Keep a reference to the DumpedStuff instance so that it is not garbage
  // collected before we take the heap dump.
  public static DumpedStuff stuff;

  // We will take a heap dump that includes a single instance of this
  // DumpedStuff class. Objects stored as fields in this class can be easily
  // found in the hprof dump by searching for the instance of the DumpedStuff
  // class and reading the desired field.
  public static class DumpedStuff {
    public String basicString = "hello, world";
    public char[] charArray = "char thing".toCharArray();
    public String nullString = null;
    public Object anObject = new Object();
    public ReferenceQueue<Object> referenceQueue = new ReferenceQueue<Object>();
    public PhantomReference aPhantomReference = new PhantomReference(anObject, referenceQueue);
    public WeakReference aWeakReference = new WeakReference(anObject, referenceQueue);
    public byte[] bigArray;

    DumpedStuff() {
      int N = 1000000;
      bigArray = new byte[N];
      for (int i = 0; i < N; i++) {
        bigArray[i] = (byte)((i*i) & 0xFF);
      }

      NativeAllocationRegistry registry = new NativeAllocationRegistry(
          Main.class.getClassLoader(), 0x12345, 42);
      registry.registerNativeAllocation(anObject, 0xABCDABCD);
    }
  }

  public static void main(String[] args) throws IOException {
    if (args.length < 1) {
      System.err.println("no output file specified");
      return;
    }
    String file = args[0];

    // Allocate the instance of DumpedStuff.
    stuff = new DumpedStuff();

    // Take a heap dump that will include that instance of DumpedStuff.
    System.err.println("Dumping hprof data to " + file);
    VMDebug.dumpHprofData(file);
  }
}
