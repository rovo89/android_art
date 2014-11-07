/*
 * Copyright (C) 2015 The Android Open Source Project
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

import java.io.File;
import java.lang.reflect.Constructor;
import java.lang.reflect.Method;

/**
 * Structural hazard test.
 */
public class Main {
    public static void main(String[] args) {
        new Main().run();
    }

    private void run() {
        try {
            Class<?> bClass = getClass().getClassLoader().loadClass("A");
            System.out.println("Should really reach here.");
        } catch (Exception e) {
            e.printStackTrace(System.out);
        }

        boolean haveOatFile = hasOat();
        boolean gotError = false;
        try {
            Class<?> bClass = getClass().getClassLoader().loadClass("B");
        } catch (IncompatibleClassChangeError icce) {
            gotError = true;
        } catch (Exception e) {
            e.printStackTrace(System.out);
        }
        if (haveOatFile ^ gotError) {
            System.out.println("Did not get expected error.");
        }
        System.out.println("Done.");
    }

    static {
        System.loadLibrary("arttest");
    }

    private native static boolean hasOat();
}
