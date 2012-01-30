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

class Fibonacci {

    static int fibonacci(int n) {
        if (n == 0) {
            return 0;
        }
        int x = 1;
        int y = 1;
        for (int i = 3; i <= n; i++) {
            int z = x + y;
            x = y;
            y = z;
        }
        return y;
    }

    public static void main(String[] args) {
        try {
            if (args.length == 1) {
                int x = Integer.parseInt(args[0]);
                int y = fibonacci(x); /* to warm up cache */
                System.out.printf("fibonacci(%d)=%d\n", x, y);
                y = fibonacci(x + 1);
                System.out.printf("fibonacci(%d)=%d\n", x + 1, y);
            }
        } catch (NumberFormatException ex) {}
    }
}
