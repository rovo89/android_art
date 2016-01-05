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

import java.lang.reflect.*;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

// Run on host with:
//   javac ThreadTest.java && java ThreadStress && rm *.class
// Through run-test:
//   test/run-test {run-test-args} 004-ThreadStress [Main {ThreadStress-args}]
//   (It is important to pass Main if you want to give parameters...)
//
// ThreadStress command line parameters:
//    -n X ............ number of threads
//    -d X ............ number of daemon threads
//    -o X ............ number of overall operations
//    -t X ............ number of operations per thread
//    --dumpmap ....... print the frequency map
//    -oom:X .......... frequency of OOM (double)
//    -alloc:X ........ frequency of Alloc
//    -stacktrace:X ... frequency of StackTrace
//    -exit:X ......... frequency of Exit
//    -sleep:X ........ frequency of Sleep
//    -wait:X ......... frequency of Wait
//    -timedwait:X .... frequency of TimedWait

public class Main implements Runnable {

    public static final boolean DEBUG = false;

    private static abstract class Operation {
        /**
         * Perform the action represented by this operation. Returns true if the thread should
         * continue.
         */
        public abstract boolean perform();
    }

    private final static class OOM extends Operation {
        private final static int ALLOC_SIZE = 1024;

        @Override
        public boolean perform() {
            try {
                List<byte[]> l = new ArrayList<byte[]>();
                while (true) {
                    l.add(new byte[ALLOC_SIZE]);
                }
            } catch (OutOfMemoryError e) {
            }
            return true;
        }
    }

    private final static class SigQuit extends Operation {
        private final static int sigquit;
        private final static Method kill;
        private final static int pid;

        static {
            int pidTemp = -1;
            int sigquitTemp = -1;
            Method killTemp = null;

            try {
                Class<?> osClass = Class.forName("android.system.Os");
                Method getpid = osClass.getDeclaredMethod("getpid");
                pidTemp = (Integer)getpid.invoke(null);

                Class<?> osConstants = Class.forName("android.system.OsConstants");
                Field sigquitField = osConstants.getDeclaredField("SIGQUIT");
                sigquitTemp = (Integer)sigquitField.get(null);

                killTemp = osClass.getDeclaredMethod("kill", int.class, int.class);
            } catch (Exception e) {
                if (!e.getClass().getName().equals("ErrnoException")) {
                    e.printStackTrace(System.out);
                }
            }

            pid = pidTemp;
            sigquit = sigquitTemp;
            kill = killTemp;
        }

        @Override
        public boolean perform() {
            try {
                kill.invoke(null, pid, sigquit);
            } catch (Exception e) {
                if (!e.getClass().getName().equals("ErrnoException")) {
                    e.printStackTrace(System.out);
                }
            }
            return true;
        }
    }

    private final static class Alloc extends Operation {
        private final static int ALLOC_SIZE = 1024;  // Needs to be small enough to not be in LOS.
        private final static int ALLOC_COUNT = 1024;

        @Override
        public boolean perform() {
            try {
                List<byte[]> l = new ArrayList<byte[]>();
                for (int i = 0; i < ALLOC_COUNT; i++) {
                    l.add(new byte[ALLOC_SIZE]);
                }
            } catch (OutOfMemoryError e) {
            }
            return true;
        }
    }

    private final static class LargeAlloc extends Operation {
        private final static int PAGE_SIZE = 4096;
        private final static int PAGE_SIZE_MODIFIER = 10;  // Needs to be large enough for LOS.
        private final static int ALLOC_COUNT = 100;

        @Override
        public boolean perform() {
            try {
                List<byte[]> l = new ArrayList<byte[]>();
                for (int i = 0; i < ALLOC_COUNT; i++) {
                    l.add(new byte[PAGE_SIZE_MODIFIER * PAGE_SIZE]);
                }
            } catch (OutOfMemoryError e) {
            }
            return true;
        }
    }

    private final static class StackTrace extends Operation {
        @Override
        public boolean perform() {
            Thread.currentThread().getStackTrace();
            return true;
        }
    }

    private final static class Exit extends Operation {
        @Override
        public boolean perform() {
            return false;
        }
    }

    private final static class Sleep extends Operation {
        private final static int SLEEP_TIME = 100;

        @Override
        public boolean perform() {
            try {
                Thread.sleep(SLEEP_TIME);
            } catch (InterruptedException ignored) {
            }
            return true;
        }
    }

    private final static class TimedWait extends Operation {
        private final static int SLEEP_TIME = 100;

        private final Object lock;

        public TimedWait(Object lock) {
            this.lock = lock;
        }

        @Override
        public boolean perform() {
            synchronized (lock) {
                try {
                    lock.wait(SLEEP_TIME, 0);
                } catch (InterruptedException ignored) {
                }
            }
            return true;
        }
    }

    private final static class Wait extends Operation {
        private final Object lock;

        public Wait(Object lock) {
            this.lock = lock;
        }

        @Override
        public boolean perform() {
            synchronized (lock) {
                try {
                    lock.wait();
                } catch (InterruptedException ignored) {
                }
            }
            return true;
        }
    }

    private final static class SyncAndWork extends Operation {
        private final Object lock;

        public SyncAndWork(Object lock) {
            this.lock = lock;
        }

        @Override
        public boolean perform() {
            synchronized (lock) {
                try {
                    Thread.sleep((int)(Math.random()*10));
                } catch (InterruptedException ignored) {
                }
            }
            return true;
        }
    }

    private final static Map<Operation, Double> createDefaultFrequencyMap(Object lock) {
        Map<Operation, Double> frequencyMap = new HashMap<Operation, Double>();
        frequencyMap.put(new OOM(), 0.005);             //  1/200
        frequencyMap.put(new SigQuit(), 0.095);         // 19/200
        frequencyMap.put(new Alloc(), 0.25);            // 50/200
        frequencyMap.put(new LargeAlloc(), 0.05);       // 10/200
        frequencyMap.put(new StackTrace(), 0.1);        // 20/200
        frequencyMap.put(new Exit(), 0.25);             // 50/200
        frequencyMap.put(new Sleep(), 0.125);           // 25/200
        frequencyMap.put(new TimedWait(lock), 0.05);    // 10/200
        frequencyMap.put(new Wait(lock), 0.075);        // 15/200

        return frequencyMap;
    }

    private final static Map<Operation, Double> createLockFrequencyMap(Object lock) {
      Map<Operation, Double> frequencyMap = new HashMap<Operation, Double>();
      frequencyMap.put(new Sleep(), 0.2);
      frequencyMap.put(new TimedWait(lock), 0.2);
      frequencyMap.put(new Wait(lock), 0.2);
      frequencyMap.put(new SyncAndWork(lock), 0.4);

      return frequencyMap;
    }

    public static void main(String[] args) throws Exception {
        parseAndRun(args);
    }

    private static Map<Operation, Double> updateFrequencyMap(Map<Operation, Double> in,
            Object lock, String arg) {
        String split[] = arg.split(":");
        if (split.length != 2) {
            throw new IllegalArgumentException("Can't split argument " + arg);
        }
        double d;
        try {
            d = Double.parseDouble(split[1]);
        } catch (Exception e) {
            throw new IllegalArgumentException(e);
        }
        if (d < 0) {
            throw new IllegalArgumentException(arg + ": value must be >= 0.");
        }
        Operation op = null;
        if (split[0].equals("-oom")) {
            op = new OOM();
        } else if (split[0].equals("-sigquit")) {
            op = new SigQuit();
        } else if (split[0].equals("-alloc")) {
            op = new Alloc();
        } else if (split[0].equals("-largealloc")) {
            op = new LargeAlloc();
        } else if (split[0].equals("-stacktrace")) {
            op = new StackTrace();
        } else if (split[0].equals("-exit")) {
            op = new Exit();
        } else if (split[0].equals("-sleep")) {
            op = new Sleep();
        } else if (split[0].equals("-wait")) {
            op = new Wait(lock);
        } else if (split[0].equals("-timedwait")) {
            op = new TimedWait(lock);
        } else {
            throw new IllegalArgumentException("Unknown arg " + arg);
        }

        if (in == null) {
            in = new HashMap<Operation, Double>();
        }
        in.put(op, d);

        return in;
    }

    private static void normalize(Map<Operation, Double> map) {
        double sum = 0;
        for (Double d : map.values()) {
            sum += d;
        }
        if (sum == 0) {
            throw new RuntimeException("No elements!");
        }
        if (sum != 1.0) {
            // Avoid ConcurrentModificationException.
            Set<Operation> tmp = new HashSet<>(map.keySet());
            for (Operation op : tmp) {
                map.put(op, map.get(op) / sum);
            }
        }
    }

    public static void parseAndRun(String[] args) throws Exception {
        int numberOfThreads = -1;
        int numberOfDaemons = -1;
        int totalOperations = -1;
        int operationsPerThread = -1;
        Object lock = new Object();
        Map<Operation, Double> frequencyMap = null;
        boolean dumpMap = false;

        if (args != null) {
            // args[0] is libarttest
            for (int i = 1; i < args.length; i++) {
                if (args[i].equals("-n")) {
                    i++;
                    numberOfThreads = Integer.parseInt(args[i]);
                } else if (args[i].equals("-d")) {
                    i++;
                    numberOfDaemons = Integer.parseInt(args[i]);
                } else if (args[i].equals("-o")) {
                    i++;
                    totalOperations = Integer.parseInt(args[i]);
                } else if (args[i].equals("-t")) {
                    i++;
                    operationsPerThread = Integer.parseInt(args[i]);
                } else if (args[i].equals("--locks-only")) {
                    lock = new Object();
                    frequencyMap = createLockFrequencyMap(lock);
                } else if (args[i].equals("--dumpmap")) {
                    dumpMap = true;
                } else {
                    frequencyMap = updateFrequencyMap(frequencyMap, lock, args[i]);
                }
            }
        }

        if (totalOperations != -1 && operationsPerThread != -1) {
            throw new IllegalArgumentException(
                    "Specified both totalOperations and operationsPerThread");
        }

        if (numberOfThreads == -1) {
            numberOfThreads = 5;
        }

        if (numberOfDaemons == -1) {
            numberOfDaemons = 3;
        }

        if (totalOperations == -1) {
            totalOperations = 1000;
        }

        if (operationsPerThread == -1) {
            operationsPerThread = totalOperations/numberOfThreads;
        }

        if (frequencyMap == null) {
            frequencyMap = createDefaultFrequencyMap(lock);
        }
        normalize(frequencyMap);

        if (dumpMap) {
            System.out.println(frequencyMap);
        }

        runTest(numberOfThreads, numberOfDaemons, operationsPerThread, lock, frequencyMap);
    }

    public static void runTest(final int numberOfThreads, final int numberOfDaemons,
                               final int operationsPerThread, final Object lock,
                               Map<Operation, Double> frequencyMap) throws Exception {
        // Each normal thread is going to do operationsPerThread
        // operations. Each daemon thread will loop over all
        // the operations and will not stop.
        // The distribution of operations is determined by
        // the Operation.frequency values. We fill out an Operation[]
        // for each thread with the operations it is to perform. The
        // Operation[] is shuffled so that there is more random
        // interactions between the threads.

        // Fill in the Operation[] array for each thread by laying
        // down references to operation according to their desired
        // frequency.
        // The first numberOfThreads elements are normal threads, the last
        // numberOfDaemons elements are daemon threads.
        final Main[] threadStresses = new Main[numberOfThreads + numberOfDaemons];
        for (int t = 0; t < threadStresses.length; t++) {
            Operation[] operations = new Operation[operationsPerThread];
            int o = 0;
            LOOP:
            while (true) {
                for (Operation op : frequencyMap.keySet()) {
                    int freq = (int)(frequencyMap.get(op) * operationsPerThread);
                    for (int f = 0; f < freq; f++) {
                        if (o == operations.length) {
                            break LOOP;
                        }
                        operations[o] = op;
                        o++;
                    }
                }
            }
            // Randomize the operation order
            Collections.shuffle(Arrays.asList(operations));
            threadStresses[t] = t < numberOfThreads ? new Main(lock, t, operations) :
                                                      new Daemon(lock, t, operations);
        }

        // Enable to dump operation counts per thread to make sure its
        // sane compared to Operation.frequency
        if (DEBUG) {
            for (int t = 0; t < threadStresses.length; t++) {
                Operation[] operations = threadStresses[t].operations;
                Map<Operation, Integer> distribution = new HashMap<Operation, Integer>();
                for (Operation operation : operations) {
                    Integer ops = distribution.get(operation);
                    if (ops == null) {
                        ops = 1;
                    } else {
                        ops++;
                    }
                    distribution.put(operation, ops);
                }
                System.out.println("Distribution for " + t);
                for (Operation op : frequencyMap.keySet()) {
                    System.out.println(op + " = " + distribution.get(op));
                }
            }
        }

        // Create the runners for each thread. The runner Thread
        // ensures that thread that exit due to Operation.EXIT will be
        // restarted until they reach their desired
        // operationsPerThread.
        Thread[] runners = new Thread[numberOfThreads];
        for (int r = 0; r < runners.length; r++) {
            final Main ts = threadStresses[r];
            runners[r] = new Thread("Runner thread " + r) {
                final Main threadStress = ts;
                public void run() {
                    int id = threadStress.id;
                    System.out.println("Starting worker for " + id);
                    while (threadStress.nextOperation < operationsPerThread) {
                        try {
                            Thread thread = new Thread(ts, "Worker thread " + id);
                            thread.start();
                            try {
                                thread.join();
                            } catch (InterruptedException e) {
                            }

                            System.out.println("Thread exited for " + id + " with "
                                               + (operationsPerThread - threadStress.nextOperation)
                                               + " operations remaining.");
                        } catch (OutOfMemoryError e) {
                            // Ignore OOME since we need to print "Finishing worker" for the test
                            // to pass.
                        }
                    }
                    // Keep trying to print "Finishing worker" until it succeeds.
                    while (true) {
                        try {
                            System.out.println("Finishing worker");
                            break;
                        } catch (OutOfMemoryError e) {
                        }
                    }
                }
            };
        }

        // The notifier thread is a daemon just loops forever to wake
        // up threads in Operation.WAIT
        if (lock != null) {
            Thread notifier = new Thread("Notifier") {
                public void run() {
                    while (true) {
                        synchronized (lock) {
                            lock.notifyAll();
                        }
                    }
                }
            };
            notifier.setDaemon(true);
            notifier.start();
        }

        // Create and start the daemon threads.
        for (int r = 0; r < numberOfDaemons; r++) {
            Main daemon = threadStresses[numberOfThreads + r];
            Thread t = new Thread(daemon, "Daemon thread " + daemon.id);
            t.setDaemon(true);
            t.start();
        }

        for (int r = 0; r < runners.length; r++) {
            runners[r].start();
        }
        for (int r = 0; r < runners.length; r++) {
            runners[r].join();
        }
    }

    protected final Operation[] operations;
    private final Object lock;
    protected final int id;

    private int nextOperation;

    private Main(Object lock, int id, Operation[] operations) {
        this.lock = lock;
        this.id = id;
        this.operations = operations;
    }

    public void run() {
        try {
            if (DEBUG) {
                System.out.println("Starting ThreadStress " + id);
            }
            while (nextOperation < operations.length) {
                Operation operation = operations[nextOperation];
                if (DEBUG) {
                    System.out.println("ThreadStress " + id
                                       + " operation " + nextOperation
                                       + " is " + operation);
                }
                nextOperation++;
                if (!operation.perform()) {
                    return;
                }
            }
        } finally {
            if (DEBUG) {
                System.out.println("Finishing ThreadStress for " + id);
            }
        }
    }

    private static class Daemon extends Main {
        private Daemon(Object lock, int id, Operation[] operations) {
            super(lock, id, operations);
        }

        public void run() {
            try {
                if (DEBUG) {
                    System.out.println("Starting ThreadStress Daemon " + id);
                }
                int i = 0;
                while (true) {
                    Operation operation = operations[i];
                    if (DEBUG) {
                        System.out.println("ThreadStress Daemon " + id
                                           + " operation " + i
                                           + " is " + operation);
                    }
                    operation.perform();
                    i = (i + 1) % operations.length;
                }
            } catch (OutOfMemoryError e) {
                // Catch OutOfMemoryErrors since these can cause the test to fail it they print
                // the stack trace after "Finishing worker".
            } finally {
                if (DEBUG) {
                    System.out.println("Finishing ThreadStress Daemon for " + id);
                }
            }
        }
    }

}
