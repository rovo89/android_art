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

public class Main {
    private static native void initSignalTest2();
    private static native void testSignal2();

    private static void stackOverflow() {
       stackOverflow();
    }

    public static void main(String[] args) {
        System.loadLibrary("arttest");

        System.out.println("init signal test");
        initSignalTest2();
        try {
            Object o = null;
            int hash = o.hashCode();

            // Should never get here.
            System.out.println("hash: " + hash);
            throw new AssertionError();
        } catch (NullPointerException e) {
            System.out.println("Caught NullPointerException");
        }
        try {
            stackOverflow();

            // Should never get here.
            throw new AssertionError();
        } catch (StackOverflowError e) {
            System.out.println("Caught StackOverflowError");
        }

        // Test that a signal in native code works.
        testSignal2();
    }
}
