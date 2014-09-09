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

import java.io.File;
import java.io.IOException;
import java.lang.reflect.Method;

public class Main {
    public static void main(String[] args) throws Exception {
        String name = System.getProperty("java.vm.name");
        if (!"Dalvik".equals(name)) {
            System.out.println("This test is not supported on " + name);
            return;
        }
        testMethodTracing();
    }

    private static void testMethodTracing() throws Exception {
        File tempFile;
        try {
            tempFile = File.createTempFile("test", ".trace");
        } catch (IOException e) {
            System.setProperty("java.io.tmpdir", "/sdcard");
            tempFile = File.createTempFile("test", ".trace");
        }
        tempFile.deleteOnExit();
        String tempFileName = tempFile.getPath();

        if (VMDebug.getMethodTracingMode() != 0) {
            VMDebug.stopMethodTracing();
        }

        System.out.println("Confirm enable/disable");
        System.out.println("status=" + VMDebug.getMethodTracingMode());
        VMDebug.startMethodTracing(tempFileName, 0, 0, false, 0);
        System.out.println("status=" + VMDebug.getMethodTracingMode());
        VMDebug.stopMethodTracing();
        System.out.println("status=" + VMDebug.getMethodTracingMode());
        if (tempFile.length() == 0) {
            System.out.println("ERROR: tracing output file is empty");
        }

        System.out.println("Confirm sampling");
        VMDebug.startMethodTracing(tempFileName, 0, 0, true, 1000);
        System.out.println("status=" + VMDebug.getMethodTracingMode());
        VMDebug.stopMethodTracing();
        System.out.println("status=" + VMDebug.getMethodTracingMode());
        if (tempFile.length() == 0) {
            System.out.println("ERROR: sample tracing output file is empty");
        }

        System.out.println("Test starting when already started");
        VMDebug.startMethodTracing(tempFileName, 0, 0, false, 0);
        System.out.println("status=" + VMDebug.getMethodTracingMode());
        VMDebug.startMethodTracing(tempFileName, 0, 0, false, 0);
        System.out.println("status=" + VMDebug.getMethodTracingMode());

        System.out.println("Test stopping when already stopped");
        VMDebug.stopMethodTracing();
        System.out.println("status=" + VMDebug.getMethodTracingMode());
        VMDebug.stopMethodTracing();
        System.out.println("status=" + VMDebug.getMethodTracingMode());

        System.out.println("Test tracing with empty filename");
        try {
            VMDebug.startMethodTracing("", 0, 0, false, 0);
            System.out.println("Should have thrown an exception");
        } catch (Exception e) {
            System.out.println("Got expected exception");
        }

        System.out.println("Test tracing with bogus (< 1024 && != 0) filesize");
        try {
            VMDebug.startMethodTracing(tempFileName, 1000, 0, false, 0);
            System.out.println("Should have thrown an exception");
        } catch (Exception e) {
            System.out.println("Got expected exception");
        }

        System.out.println("Test sampling with bogus (<= 0) interval");
        try {
            VMDebug.startMethodTracing(tempFileName, 0, 0, true, 0);
            System.out.println("Should have thrown an exception");
        } catch (Exception e) {
            System.out.println("Got expected exception");
        }

        tempFile.delete();
    }

    private static class VMDebug {
        private static final Method startMethodTracingMethod;
        private static final Method stopMethodTracingMethod;
        private static final Method getMethodTracingModeMethod;
        static {
            try {
                Class c = Class.forName("dalvik.system.VMDebug");
                startMethodTracingMethod = c.getDeclaredMethod("startMethodTracing", String.class,
                        Integer.TYPE, Integer.TYPE, Boolean.TYPE, Integer.TYPE);
                stopMethodTracingMethod = c.getDeclaredMethod("stopMethodTracing");
                getMethodTracingModeMethod = c.getDeclaredMethod("getMethodTracingMode");
            } catch (Exception e) {
                throw new RuntimeException(e);
            }
        }

        public static void startMethodTracing(String filename, int bufferSize, int flags,
                boolean samplingEnabled, int intervalUs) throws Exception {
            startMethodTracingMethod.invoke(null, filename, bufferSize, flags, samplingEnabled,
                    intervalUs);
        }
        public static void stopMethodTracing() throws Exception {
            stopMethodTracingMethod.invoke(null);
        }
        public static int getMethodTracingMode() throws Exception {
            return (int) getMethodTracingModeMethod.invoke(null);
        }
    }
}
