/*
 * Copyright (C) 2011 The Android Open Source Project
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
import java.util.List;
import java.util.concurrent.BrokenBarrierException;
import java.util.concurrent.CyclicBarrier;
import java.util.concurrent.SynchronousQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;

public class Main implements Runnable {

    public final static long TIMEOUT_VALUE = 5;  // Timeout in minutes.
    public final static long MAX_SIZE = 1000;  // Maximum size of array-list to allocate.

    public static void main(String[] args) throws Exception {
        Thread[] threads = new Thread[16];

        // Use a cyclic system of synchronous queues to pass a boolean token around.
        //
        // The combinations are:
        //
        // Worker receives:    true     false    false    true
        // Worker has OOM:     false    false    true     true
        //    |
        //    v
        // Value to pass:      true     false    false    false
        // Exit out of loop:   false    true     true     true
        // Wait on in queue:   true     false    false    true
        //
        // Finally, the workers are supposed to wait on the barrier to synchronize the GC run.

        CyclicBarrier barrier = new CyclicBarrier(threads.length);
        List<SynchronousQueue<Boolean>> queues = new ArrayList<SynchronousQueue<Boolean>>(
            threads.length);
        for (int i = 0; i < threads.length; i++) {
            queues.add(new SynchronousQueue<Boolean>());
        }

        for (int i = 0; i < threads.length; i++) {
            threads[i] = new Thread(new Main(i, queues.get(i), queues.get((i + 1) % threads.length),
                                             barrier));
        }
        for (Thread thread : threads) {
            thread.start();
        }

        // Push off the cycle.
        checkTimeout(queues.get(0).offer(Boolean.TRUE, TIMEOUT_VALUE, TimeUnit.MINUTES));

        // Wait for the threads to finish.
        for (Thread thread : threads) {
            thread.join();
        }

        // Allocate objects to definitely run GC before quitting.
        try {
            for (int i = 0; i < 1000; i++) {
                new ArrayList<Object>(i);
            }
        } catch (OutOfMemoryError oom) {
        }
    }

    private static void checkTimeout(Object o) {
        checkTimeout(o != null);
    }

    private static void checkTimeout(boolean b) {
        if (!b) {
            // Something went wrong.
            System.out.println("Bad things happened, timeout.");
            System.exit(1);
        }
    }

    private final int id;
    private final SynchronousQueue<Boolean> waitOn;
    private final SynchronousQueue<Boolean> pushTo;
    private final CyclicBarrier finalBarrier;

    private Main(int id, SynchronousQueue<Boolean> waitOn, SynchronousQueue<Boolean> pushTo,
        CyclicBarrier finalBarrier) {
        this.id = id;
        this.waitOn = waitOn;
        this.pushTo = pushTo;
        this.finalBarrier = finalBarrier;
    }

    public void run() {
        try {
            work();
        } catch (Exception exc) {
            // Any exception is bad.
            exc.printStackTrace(System.err);
            System.exit(1);
        }
    }

    public void work() throws BrokenBarrierException, InterruptedException, TimeoutException {
        ArrayList<Object> l = new ArrayList<Object>();

        // Main loop.
        for (int i = 0; ; i++) {
          Boolean receivedB = waitOn.poll(TIMEOUT_VALUE, TimeUnit.MINUTES);
          checkTimeout(receivedB);
          boolean received = receivedB;

          // This is the first stage, try to allocate up till MAX_SIZE.
          boolean oom = i >= MAX_SIZE;
          try {
            l.add(new ArrayList<Object>(i));
          } catch (OutOfMemoryError oome) {
            oom = true;
          }

          if (!received || oom) {
            // First stage, always push false.
            checkTimeout(pushTo.offer(Boolean.FALSE, TIMEOUT_VALUE, TimeUnit.MINUTES));

            // If we received true, wait for the false to come around.
            if (received) {
              checkTimeout(waitOn.poll(TIMEOUT_VALUE, TimeUnit.MINUTES));
            }

            // Break out of the loop.
            break;
          } else {
            // Pass on true.
            checkTimeout(pushTo.offer(Boolean.TRUE, TIMEOUT_VALUE, TimeUnit.MINUTES));
          }
        }

        // We have reached the final point. Wait on the barrier, but at most a minute.
        finalBarrier.await(TIMEOUT_VALUE, TimeUnit.MINUTES);

        // Done.
    }
}
