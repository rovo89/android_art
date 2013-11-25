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

class JniTest {
    public static void main(String[] args) {
        System.loadLibrary("arttest");
        testFindClassOnAttachedNativeThread();
        testCallStaticVoidMethodOnSubClass();
    }

    private static native void testFindClassOnAttachedNativeThread();

    private static void testCallStaticVoidMethodOnSubClass() {
        testCallStaticVoidMethodOnSubClassNative();
        if (!testCallStaticVoidMethodOnSubClass_SuperClass.executed) {
            throw new AssertionError();
        }
    }

    private static native void testCallStaticVoidMethodOnSubClassNative();

    private static class testCallStaticVoidMethodOnSubClass_SuperClass {
        private static boolean executed = false;
        private static void execute() {
            executed = true;
        }
    }

    private static class testCallStaticVoidMethodOnSubClass_SubClass
        extends testCallStaticVoidMethodOnSubClass_SuperClass {
    }
}
