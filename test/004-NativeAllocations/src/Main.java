/*
 * Copyright (C) 2013 The Android Open Source Project
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
import java.lang.Runtime;

public class Main {
    static Object nativeLock = new Object();
    static Object deadlockLock = new Object();
    static boolean aboutToDeadlockLock = false;
    static int nativeBytes = 0;
    static Object runtime;
    static Method register_native_allocation;
    static Method register_native_free;
    static long maxMem = 0;

    static class NativeAllocation {
        private int bytes;

        NativeAllocation(int bytes, boolean testingDeadlock) throws Exception {
            this.bytes = bytes;
            register_native_allocation.invoke(runtime, bytes);
            synchronized (nativeLock) {
                if (!testingDeadlock) {
                    nativeBytes += bytes;
                    if (nativeBytes > maxMem) {
                        throw new OutOfMemoryError();
                    }
                }
            }
        }

        protected void finalize() throws Exception {
            synchronized (nativeLock) {
                nativeBytes -= bytes;
            }
            register_native_free.invoke(runtime, bytes);
            aboutToDeadlockLock = true;
            synchronized (deadlockLock) {
            }
        }
    }

    public static void main(String[] args) throws Exception {
        Class<?> vm_runtime = Class.forName("dalvik.system.VMRuntime");
        Method get_runtime = vm_runtime.getDeclaredMethod("getRuntime");
        runtime = get_runtime.invoke(null);
        register_native_allocation = vm_runtime.getDeclaredMethod("registerNativeAllocation", Integer.TYPE);
        register_native_free = vm_runtime.getDeclaredMethod("registerNativeFree", Integer.TYPE);
        maxMem = Runtime.getRuntime().maxMemory();
        int count = 16;
        int size = (int)(maxMem / 2 / count);
        int allocation_count = 256;
        NativeAllocation[] allocations = new NativeAllocation[count];
        for (int i = 0; i < allocation_count; ++i) {
            allocations[i % count] = new NativeAllocation(size, false);
        }
        // Test that we don't get a deadlock if we are holding nativeLock. If there is no timeout,
        // then we will get a finalizer timeout exception.
        aboutToDeadlockLock = false;
        synchronized (deadlockLock) {
            for (int i = 0; aboutToDeadlockLock != true; ++i) {
                allocations[i % count] = new NativeAllocation(size, true);
            }
            // Do more allocations now that the finalizer thread is deadlocked so that we force
            // finalization and timeout. 
            for (int i = 0; i < 10; ++i) {
                allocations[i % count] = new NativeAllocation(size, true);
            }
        }
        System.out.println("Test complete");
    }
}

