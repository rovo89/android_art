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

  public static void main(String[] args) {
    remFloat();
    remDouble();
  }

  private static void remFloat() {
    expectApproxEquals(2F, $opt$RemConst(6F));

    expectApproxEquals(2F, $opt$Rem(5.1F, 3.1F));
    expectApproxEquals(2.1F, $opt$Rem(5.1F, 3F));
    expectApproxEquals(-2F, $opt$Rem(-5.1F, 3.1F));
    expectApproxEquals(-2.1F, $opt$Rem(-5.1F, -3F));

    expectApproxEquals(2F, $opt$Rem(6F, 4F));
    expectApproxEquals(2F, $opt$Rem(6F, -4F));
    expectApproxEquals(0F, $opt$Rem(6F, 3F));
    expectApproxEquals(0F, $opt$Rem(6F, -3F));
    expectApproxEquals(0F, $opt$Rem(6F, 1F));
    expectApproxEquals(0F, $opt$Rem(6F, -1F));
    expectApproxEquals(-1F, $opt$Rem(-7F, 3F));
    expectApproxEquals(-1F, $opt$Rem(-7F, -3F));
    expectApproxEquals(0F, $opt$Rem(6F, 6F));
    expectApproxEquals(0F, $opt$Rem(-6F, -6F));
    expectApproxEquals(7F, $opt$Rem(7F, 9F));
    expectApproxEquals(7F, $opt$Rem(7F, -9F));
    expectApproxEquals(-7F, $opt$Rem(-7F, 9F));
    expectApproxEquals(-7F, $opt$Rem(-7F, -9F));

    expectApproxEquals(0F, $opt$Rem(Float.MAX_VALUE, 1F));
    expectApproxEquals(0F, $opt$Rem(Float.MAX_VALUE, -1F));
    expectApproxEquals(0F, $opt$Rem(Float.MIN_VALUE, 1F));
    expectApproxEquals(0F, $opt$Rem(Float.MIN_VALUE, -1F));

    expectApproxEquals(0F, $opt$Rem(0F, 7F));
    expectApproxEquals(0F, $opt$Rem(0F, Float.MAX_VALUE));
    expectApproxEquals(0F, $opt$Rem(0F, Float.MIN_VALUE));

    expectNaN($opt$Rem(Float.NaN, 3F));
    expectNaN($opt$Rem(3F, Float.NaN));
    expectNaN($opt$Rem(Float.POSITIVE_INFINITY, Float.NEGATIVE_INFINITY));
    expectNaN($opt$Rem(Float.NEGATIVE_INFINITY, Float.POSITIVE_INFINITY));
    expectNaN($opt$Rem(3F, 0F));

    expectApproxEquals(4F, $opt$Rem(4F, Float.POSITIVE_INFINITY));
    expectApproxEquals(4F, $opt$Rem(4F, Float.NEGATIVE_INFINITY));
  }

  private static void remDouble() {
    expectApproxEquals(2D, $opt$RemConst(6D));

    expectApproxEquals(2D, $opt$Rem(5.1D, 3.1D));
    expectApproxEquals(2.1D, $opt$Rem(5.1D, 3D));
    expectApproxEquals(-2D, $opt$Rem(-5.1D, 3.1D));
    expectApproxEquals(-2.1D, $opt$Rem(-5.1D, -3D));

    expectApproxEquals(2D, $opt$Rem(6D, 4D));
    expectApproxEquals(2D, $opt$Rem(6D, -4D));
    expectApproxEquals(0D, $opt$Rem(6D, 3D));
    expectApproxEquals(0D, $opt$Rem(6D, -3D));
    expectApproxEquals(0D, $opt$Rem(6D, 1D));
    expectApproxEquals(0D, $opt$Rem(6D, -1D));
    expectApproxEquals(-1D, $opt$Rem(-7D, 3D));
    expectApproxEquals(-1D, $opt$Rem(-7D, -3D));
    expectApproxEquals(0D, $opt$Rem(6D, 6D));
    expectApproxEquals(0D, $opt$Rem(-6D, -6D));
    expectApproxEquals(7D, $opt$Rem(7D, 9D));
    expectApproxEquals(7D, $opt$Rem(7D, -9D));
    expectApproxEquals(-7D, $opt$Rem(-7D, 9D));
    expectApproxEquals(-7D, $opt$Rem(-7D, -9D));

    expectApproxEquals(0D, $opt$Rem(Double.MAX_VALUE, 1D));
    expectApproxEquals(0D, $opt$Rem(Double.MAX_VALUE, -1D));
    expectApproxEquals(0D, $opt$Rem(Double.MIN_VALUE, 1D));
    expectApproxEquals(0D, $opt$Rem(Double.MIN_VALUE, -1D));

    expectApproxEquals(0D, $opt$Rem(0D, 7D));
    expectApproxEquals(0D, $opt$Rem(0D, Double.MAX_VALUE));
    expectApproxEquals(0D, $opt$Rem(0D, Double.MIN_VALUE));

    expectNaN($opt$Rem(Double.NaN, 3D));
    expectNaN($opt$Rem(3D, Double.NaN));
    expectNaN($opt$Rem(Double.POSITIVE_INFINITY, Double.NEGATIVE_INFINITY));
    expectNaN($opt$Rem(Double.NEGATIVE_INFINITY, Double.POSITIVE_INFINITY));
    expectNaN($opt$Rem(3D, 0D));

    expectApproxEquals(4D, $opt$Rem(4D, Double.POSITIVE_INFINITY));
    expectApproxEquals(4D, $opt$Rem(4D, Double.NEGATIVE_INFINITY));
  }

  static float $opt$Rem(float a, float b) {
    return a % b;
  }

 static float $opt$RemConst(float a) {
    return a % 4F;
  }

  static double $opt$Rem(double a, double b) {
    return a % b;
  }

  static double $opt$RemConst(double a) {
    return a % 4D;
  }

  public static void expectApproxEquals(float a, float b) {
    float maxDelta = 0.00001F;
    boolean aproxEquals = (a > b) ? ((a - b) < maxDelta) : ((b - a) < maxDelta);
    if (!aproxEquals) {
      throw new Error("Expected: " + a + ", found: " + b
          + ", with delta: " + maxDelta + " " + (a - b));
    }
  }

  public static void expectApproxEquals(double a, double b) {
    double maxDelta = 0.00001D;
    boolean aproxEquals = (a > b) ? ((a - b) < maxDelta) : ((b - a) < maxDelta);
    if (!aproxEquals) {
      throw new Error("Expected: " + a + ", found: "
          + b + ", with delta: " + maxDelta + " " + (a - b));
    }
  }

  public static void expectNaN(float a) {
    if (a == a) {
      throw new Error("Expected NaN: " + a);
    }
  }

  public static void expectNaN(double a) {
    if (a == a) {
      throw new Error("Expected NaN: " + a);
    }
  }

}
