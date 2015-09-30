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

public class Main {
    public static void main(String[] args) {
        new Main().run();
        System.out.println("finish");
    }

    public void run() {
        double a[][] = new double[200][201];
        double b[] = new double[200];
        int n = 100;

        foo1(a, n, b);
    }

    void foo1(double a[][], int n, double b[]) {
        double t;
        int i,k;

        for (i = 0; i < n; i++) {
            k = n - (i + 1);
            b[k] /= a[k][k];
            t = -b[k];
            foo2(k + 1000, t, b);
        }
    }

    void foo2(int n, double c, double b[]) {
        try {
            foo3(n, c, b);
        } catch (Exception e) {
        }
    }

    void foo3(int n, double c, double b[]) {
        int i = 0;
        for (i = 0; i < n; i++) {
            b[i + 1] += c * b[i + 1];
        }
    }
}

