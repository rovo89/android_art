/*
 * Copyright (C) 2006 The Android Open Source Project
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

import java.util.ArrayList;

/**
 * Test some basic thread stuff.
 */
public class Main {
    static {
        System.loadLibrary("arttest");
    }

    public static void main(String[] args) throws Exception {
        System.out.println("thread test starting");
        testThreadCapacity();
        testThreadDaemons();
        testSleepZero();
        testSetName();
        testThreadPriorities();
        System.out.println("thread test done");
    }

    /*
     * Simple thread capacity test.
     */
    private static void testThreadCapacity() throws Exception {
        TestCapacityThread[] threads = new TestCapacityThread[512];
        for (int i = 0; i < 512; i++) {
            threads[i] = new TestCapacityThread();
        }

        for (TestCapacityThread thread : threads) {
            thread.start();
        }
        for (TestCapacityThread thread : threads) {
            thread.join();
        }

        System.out.println("testThreadCapacity thread count: " + TestCapacityThread.mCount);
    }

    private static class TestCapacityThread extends Thread {
        static int mCount = 0;
        public void run() {
            synchronized (TestCapacityThread.class) {
                ++mCount;
            }
            try {
                sleep(1000);
            } catch (Exception ex) {
            }
        }
    }

    private static void testThreadDaemons() {
        Thread t = new Thread(null, new TestDaemonThread(), "TestDaemonThread", 7168);

        t.setDaemon(false);

        System.out.print("testThreadDaemons starting thread '" + t.getName() + "'\n");
        t.start();

        try {
            t.join();
        } catch (InterruptedException ex) {
            ex.printStackTrace();
        }

        System.out.print("testThreadDaemons finished\n");
    }

    private static class TestDaemonThread implements Runnable {
        public void run() {
            System.out.print("testThreadDaemons @ Thread running\n");

            try {
                Thread.currentThread().setDaemon(true);
                System.out.print("testThreadDaemons @ FAILED: setDaemon() succeeded\n");
            } catch (IllegalThreadStateException itse) {
                System.out.print("testThreadDaemons @ Got expected setDaemon exception\n");
            }

            try {
                Thread.sleep(2000);
            }
            catch (InterruptedException ie) {
                System.out.print("testThreadDaemons @ Interrupted!\n");
            }
            finally {
                System.out.print("testThreadDaemons @ Thread bailing\n");
            }
        }
    }

    private static void testSleepZero() throws Exception {
        Thread.currentThread().interrupt();
        try {
            Thread.sleep(0);
            throw new AssertionError("unreachable");
        } catch (InterruptedException e) {
            if (Thread.currentThread().isInterrupted()) {
                throw new AssertionError("thread is interrupted");
            }
        }
        System.out.print("testSleepZero finished\n");
    }

    private static void testSetName() throws Exception {
        System.out.print("testSetName starting\n");
        Thread thread = new Thread() {
            @Override
            public void run() {
                System.out.print("testSetName running\n");
            }
        };
        thread.start();
        thread.setName("HelloWorld");  // b/17302037 hang if setName called after start
        if (!thread.getName().equals("HelloWorld")) {
            throw new AssertionError("Unexpected thread name: " + thread.getName());
        }
        thread.join();
        if (!thread.getName().equals("HelloWorld")) {
            throw new AssertionError("Unexpected thread name after join: " + thread.getName());
        }
        System.out.print("testSetName finished\n");
    }

    private static void testThreadPriorities() throws Exception {
        System.out.print("testThreadPriorities starting\n");

        PriorityStoringThread t1 = new PriorityStoringThread(false);
        t1.setPriority(Thread.MAX_PRIORITY);
        t1.start();
        t1.join();
        if (supportsThreadPriorities() && (t1.getNativePriority() != Thread.MAX_PRIORITY)) {
            System.out.print("thread priority for t1 was " + t1.getNativePriority() +
                " [expected Thread.MAX_PRIORITY]\n");
        }

        PriorityStoringThread t2 = new PriorityStoringThread(true);
        t2.start();
        t2.join();
        if (supportsThreadPriorities() && (t2.getNativePriority() != Thread.MAX_PRIORITY)) {
            System.out.print("thread priority for t2 was " + t2.getNativePriority() +
                " [expected Thread.MAX_PRIORITY]\n");
        }

        System.out.print("testThreadPriorities finished\n");
    }

    private static native int getNativePriority();
    private static native boolean supportsThreadPriorities();

    static class PriorityStoringThread extends Thread {
        private final boolean setPriority;
        private volatile int nativePriority;

        public PriorityStoringThread(boolean setPriority) {
            this.setPriority = setPriority;
            this.nativePriority = -1;
        }

        @Override
        public void run() {
            if (setPriority) {
                setPriority(Thread.MAX_PRIORITY);
            }

            nativePriority = Main.getNativePriority();
        }

        public int getNativePriority() {
            return nativePriority;
        }
    }
}
