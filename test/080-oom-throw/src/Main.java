/*
 * Copyright (C) 2009 The Android Open Source Project
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

public class Main {
    static char [][] holder;

    static class ArrayMemEater {
        static boolean sawOome;

        static void blowup(char[][] holder) {
            try {
                for (int i = 0; i < holder.length; ++i) {
                    holder[i] = new char[1022 * 1024];
                }
            } catch (OutOfMemoryError oome) {
                ArrayMemEater.sawOome = true;
            }
        }
    }

    static class InstanceMemEater {
        static boolean sawOome;
        static InstanceMemEater hook;

        InstanceMemEater next;
        double d1, d2, d3, d4, d5, d6, d7, d8; // Bloat this object so we fill the heap faster.

        static InstanceMemEater allocate() {
            try {
                return new InstanceMemEater();
            } catch (OutOfMemoryError e) {
                InstanceMemEater.sawOome = true;
                return null;
            }
        }

        static void confuseCompilerOptimization(InstanceMemEater instance) {
          hook = instance;
        }
    }

    static class InstanceFinalizerMemEater {
        static boolean sawOome;
        static InstanceFinalizerMemEater hook;

        InstanceFinalizerMemEater next;

        static InstanceFinalizerMemEater allocate() {
            try {
                return new InstanceFinalizerMemEater();
            } catch (OutOfMemoryError e) {
                InstanceFinalizerMemEater.sawOome = true;
                return null;
            }
        }

        static void confuseCompilerOptimization(InstanceFinalizerMemEater instance) {
            hook = instance;
        }

        protected void finalize() {}
    }

    static boolean triggerArrayOOM(char[][] holder) {
        ArrayMemEater.blowup(holder);
        return ArrayMemEater.sawOome;
    }

    static boolean triggerInstanceOOM() {
        InstanceMemEater memEater = InstanceMemEater.allocate();
        InstanceMemEater lastMemEater = memEater;
        do {
            lastMemEater.next = InstanceMemEater.allocate();
            lastMemEater = lastMemEater.next;
        } while (lastMemEater != null);
        memEater.confuseCompilerOptimization(memEater);
        InstanceMemEater.hook = null;
        return InstanceMemEater.sawOome;
    }

    static boolean triggerInstanceFinalizerOOM() {
        InstanceFinalizerMemEater memEater = InstanceFinalizerMemEater.allocate();
        InstanceFinalizerMemEater lastMemEater = memEater;
        do {
            lastMemEater.next = InstanceFinalizerMemEater.allocate();
            lastMemEater = lastMemEater.next;
        } while (lastMemEater != null);
        memEater.confuseCompilerOptimization(memEater);
        InstanceFinalizerMemEater.hook = null;
        return InstanceFinalizerMemEater.sawOome;
    }

    public static void main(String[] args) {
        // Keep holder alive to make instance OOM happen faster
        holder = new char[128 * 1024][];
        if (triggerArrayOOM(holder)) {
            System.out.println("NEW_ARRAY correctly threw OOME");
        }

        if (!triggerInstanceFinalizerOOM()) {
            System.out.println("NEW_INSTANCE (finalize) did not threw OOME");
        }

        if (triggerInstanceOOM()) {
            System.out.println("NEW_INSTANCE correctly threw OOME");
        }
    }
}
