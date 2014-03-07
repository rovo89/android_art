/*
 * Copyright (C) 2007 The Android Open Source Project
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

/**
 * Test a class with a bad finalizer.
 */
public class Main {
    public static void main(String[] args) {
        BadFinalizer bf = new BadFinalizer();

        System.out.println("About to null reference and request GC.");
        bf = null;
        Runtime.getRuntime().gc();

        for (int i = 0; i < 8; i++) {
            snooze(4000);
            Runtime.getRuntime().gc();
        }

        System.out.println("UNREACHABLE");
        System.exit(0);
    }

    public static void snooze(int ms) {
        try {
            Thread.sleep(ms);
        } catch (InterruptedException ie) {
        }
    }

    /**
     * Class with a bad finalizer.
     */
    public static class BadFinalizer {
        protected void finalize() {
            System.out.println("Finalizer started and spinning...");
            int j = 0;

            /* spin for a bit */
            long start, end;
            start = System.nanoTime();
            for (int i = 0; i < 1000000; i++) {
                j++;
            }
            end = System.nanoTime();
            System.out.println("Finalizer done spinning.");

            System.out.println("Finalizer sleeping forever now.");
            while (true) {
                snooze(10000);
            }
        }
    }
}
