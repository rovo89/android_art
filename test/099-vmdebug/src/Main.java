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
import java.util.Map;

public class Main {
    public static void main(String[] args) throws Exception {
        String name = System.getProperty("java.vm.name");
        if (!"Dalvik".equals(name)) {
            System.out.println("This test is not supported on " + name);
            return;
        }
        testMethodTracing();
        testRuntimeStat();
    }

    private static File createTempFile() throws Exception {
        try {
            return  File.createTempFile("test", ".trace");
        } catch (IOException e) {
            System.setProperty("java.io.tmpdir", "/data/local/tmp");
            try {
                return File.createTempFile("test", ".trace");
            } catch (IOException e2) {
                System.setProperty("java.io.tmpdir", "/sdcard");
                return File.createTempFile("test", ".trace");
            }
        }
    }

    private static void testMethodTracing() throws Exception {
        File tempFile = createTempFile();
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

    private static void checkNumber(String s) throws Exception {
        if (s == null) {
            System.out.println("Got null string");
            return;
        }
        long n = Long.valueOf(s);
        if (n < 0) {
            System.out.println("Got negative number " + n);
        }
    }

    private static void checkHistogram(String s) throws Exception {
        if (s == null || s.length() == 0) {
            System.out.println("Got null or empty string");
            return;
        }
        String[] buckets = s.split(",");
        long last_key = 0;
        for (int i = 0; i < buckets.length; ++i) {
            String bucket = buckets[i];
            if (bucket.length() == 0) {
                System.out.println("Got empty bucket");
                continue;
            }
            String[] kv = bucket.split(":");
            if (kv.length != 2 || kv[0].length() == 0 || kv[1].length() == 0) {
                System.out.println("Got bad bucket " + bucket);
                continue;
            }
            long key = Long.valueOf(kv[0]);
            long value = Long.valueOf(kv[1]);
            if (key < 0 || value < 0) {
                System.out.println("Got negative key or value " + bucket);
                continue;
            }
            if (key < last_key) {
                System.out.println("Got decreasing key " + bucket);
                continue;
            }
            last_key = key;
        }
    }

    private static void testRuntimeStat() throws Exception {
        // Invoke at least one GC and wait for 20 seconds or so so we get at
        // least one bucket in the histograms.
        for (int i = 0; i < 20; ++i) {
          Runtime.getRuntime().gc();
          Thread.sleep(1000L);
        }
        String gc_count = VMDebug.getRuntimeStat("art.gc.gc-count");
        String gc_time = VMDebug.getRuntimeStat("art.gc.gc-time");
        String bytes_allocated = VMDebug.getRuntimeStat("art.gc.bytes-allocated");
        String bytes_freed = VMDebug.getRuntimeStat("art.gc.bytes-freed");
        String blocking_gc_count = VMDebug.getRuntimeStat("art.gc.blocking-gc-count");
        String blocking_gc_time = VMDebug.getRuntimeStat("art.gc.blocking-gc-time");
        String gc_count_rate_histogram = VMDebug.getRuntimeStat("art.gc.gc-count-rate-histogram");
        String blocking_gc_count_rate_histogram =
            VMDebug.getRuntimeStat("art.gc.blocking-gc-count-rate-histogram");
        checkNumber(gc_count);
        checkNumber(gc_time);
        checkNumber(bytes_allocated);
        checkNumber(bytes_freed);
        checkNumber(blocking_gc_count);
        checkNumber(blocking_gc_time);
        checkHistogram(gc_count_rate_histogram);
        checkHistogram(blocking_gc_count_rate_histogram);
    }

    private static void testRuntimeStats() throws Exception {
        // Invoke at least one GC and wait for 20 seconds or so so we get at
        // least one bucket in the histograms.
        for (int i = 0; i < 20; ++i) {
          Runtime.getRuntime().gc();
          Thread.sleep(1000L);
        }
        Map<String, String> map = VMDebug.getRuntimeStats();
        String gc_count = map.get("art.gc.gc-count");
        String gc_time = map.get("art.gc.gc-time");
        String bytes_allocated = map.get("art.gc.bytes-allocated");
        String bytes_freed = map.get("art.gc.bytes-freed");
        String blocking_gc_count = map.get("art.gc.blocking-gc-count");
        String blocking_gc_time = map.get("art.gc.blocking-gc-time");
        String gc_count_rate_histogram = map.get("art.gc.gc-count-rate-histogram");
        String blocking_gc_count_rate_histogram =
            map.get("art.gc.blocking-gc-count-rate-histogram");
        checkNumber(gc_count);
        checkNumber(gc_time);
        checkNumber(bytes_allocated);
        checkNumber(bytes_freed);
        checkNumber(blocking_gc_count);
        checkNumber(blocking_gc_time);
        checkHistogram(gc_count_rate_histogram);
        checkHistogram(blocking_gc_count_rate_histogram);
    }

    private static class VMDebug {
        private static final Method startMethodTracingMethod;
        private static final Method stopMethodTracingMethod;
        private static final Method getMethodTracingModeMethod;
        private static final Method getRuntimeStatMethod;
        private static final Method getRuntimeStatsMethod;
        static {
            try {
                Class c = Class.forName("dalvik.system.VMDebug");
                startMethodTracingMethod = c.getDeclaredMethod("startMethodTracing", String.class,
                        Integer.TYPE, Integer.TYPE, Boolean.TYPE, Integer.TYPE);
                stopMethodTracingMethod = c.getDeclaredMethod("stopMethodTracing");
                getMethodTracingModeMethod = c.getDeclaredMethod("getMethodTracingMode");
                getRuntimeStatMethod = c.getDeclaredMethod("getRuntimeStat", String.class);
                getRuntimeStatsMethod = c.getDeclaredMethod("getRuntimeStats");
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
        public static String getRuntimeStat(String statName) throws Exception {
            return (String) getRuntimeStatMethod.invoke(null, statName);
        }
        public static Map<String, String> getRuntimeStats() throws Exception {
            return (Map<String, String>) getRuntimeStatsMethod.invoke(null);
        }
    }
}
