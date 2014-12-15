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

import java.util.Map;

public class Main implements Runnable {
    static final int numberOfThreads = 5;
    static final int totalOperations = 1000;

    public static void main(String[] args) throws Exception {
        final Thread[] threads = new Thread[numberOfThreads];
        for (int t = 0; t < threads.length; t++) {
            threads[t] = new Thread(new Main());
            threads[t].start();
        }
        for (Thread t : threads) {
            t.join();
        }
        System.out.println("Finishing");
    }

    public void test_getId() {
        if (Thread.currentThread().getId() <= 0) {
            System.out.println("current thread's ID is not positive");
        }
        // Check all the current threads for positive IDs.
        Map<Thread, StackTraceElement[]> stMap = Thread.getAllStackTraces();
        for (Thread thread : stMap.keySet()) {
            if (thread.getId() <= 0) {
                System.out.println("thread's ID is not positive: " + thread.getName());
            }
        }
    }

    public void run() {
        for (int i = 0; i < totalOperations; ++i) {
            test_getId();
        }
    }
}
