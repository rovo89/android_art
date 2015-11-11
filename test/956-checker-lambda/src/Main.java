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

//
// Test on lambda expressions.
//
public class Main {

  @NeedsLambda
  interface L1 {
    public void run(int a);
  }

  int d;

  public Main() {
    d = 4;
  }

  private void doit(int a) {
    int b = 2;
    // Captures parameter, local, and field. Takes its own parameter.
    // TODO: add field d as well, b/25631011 prevents this
    L1 lambda1 = (int c) -> { System.out.println("Lambda " + a + b + c); };
    lambda1.run(3);
  }

  public static void main(String[] args) {
    new Main().doit(1);
    System.out.println("passed");
  }
}
