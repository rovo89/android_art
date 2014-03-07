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
    public static void main(String[] args) throws Exception {
        System.out.println("thread test starting");
        testThreadCapacity();
        testThreadDaemons();
        testSleepZero();
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
}
