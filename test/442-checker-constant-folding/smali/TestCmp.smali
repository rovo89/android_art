# Copyright (C) 2015 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

.class public LTestCmp;

.super Ljava/lang/Object;


## CHECK-START: int TestCmp.$opt$CmpLongConstants() constant_folding (before)
## CHECK-DAG:     <<Const13:j\d+>>  LongConstant 13
## CHECK-DAG:     <<Const7:j\d+>>   LongConstant 7
## CHECK-DAG:     <<Cmp:i\d+>>      Compare [<<Const13>>,<<Const7>>]
## CHECK-DAG:                       Return [<<Cmp>>]

## CHECK-START: int TestCmp.$opt$CmpLongConstants() constant_folding (after)
## CHECK-DAG:                       LongConstant 13
## CHECK-DAG:                       LongConstant 7
## CHECK-DAG:     <<Const1:i\d+>>   IntConstant 1
## CHECK-DAG:                       Return [<<Const1>>]

## CHECK-START: int TestCmp.$opt$CmpLongConstants() constant_folding (after)
## CHECK-NOT:                       Compare

.method public static $opt$CmpLongConstants()I
   .registers 5
   const-wide v1, 13
   const-wide v3, 7
   cmp-long v0, v1, v3
   return v0
.end method

## CHECK-START: int TestCmp.$opt$CmpGtFloatConstants() constant_folding (before)
## CHECK-DAG:     <<Const11:f\d+>>  FloatConstant 11
## CHECK-DAG:     <<Const22:f\d+>>  FloatConstant 22
## CHECK-DAG:     <<Cmp:i\d+>>      Compare [<<Const11>>,<<Const22>>] bias:gt
## CHECK-DAG:                       Return [<<Cmp>>]

## CHECK-START: int TestCmp.$opt$CmpGtFloatConstants() constant_folding (after)
## CHECK-DAG:                       FloatConstant 11
## CHECK-DAG:                       FloatConstant 22
## CHECK-DAG:     <<ConstM1:i\d+>>  IntConstant -1
## CHECK-DAG:                       Return [<<ConstM1>>]

## CHECK-START: int TestCmp.$opt$CmpGtFloatConstants() constant_folding (after)
## CHECK-NOT:                       Compare

.method public static $opt$CmpGtFloatConstants()I
   .registers 3
   const v1, 11.f
   const v2, 22.f
   cmpg-float v0, v1, v2
   return v0
.end method

## CHECK-START: int TestCmp.$opt$CmpLtFloatConstants() constant_folding (before)
## CHECK-DAG:     <<Const33:f\d+>>  FloatConstant 33
## CHECK-DAG:     <<Const44:f\d+>>  FloatConstant 44
## CHECK-DAG:     <<Cmp:i\d+>>      Compare [<<Const33>>,<<Const44>>] bias:lt
## CHECK-DAG:                       Return [<<Cmp>>]

## CHECK-START: int TestCmp.$opt$CmpLtFloatConstants() constant_folding (after)
## CHECK-DAG:                       FloatConstant 33
## CHECK-DAG:                       FloatConstant 44
## CHECK-DAG:     <<ConstM1:i\d+>>  IntConstant -1
## CHECK-DAG:                       Return [<<ConstM1>>]

## CHECK-START: int TestCmp.$opt$CmpLtFloatConstants() constant_folding (after)
## CHECK-NOT:                       Compare

.method public static $opt$CmpLtFloatConstants()I
   .registers 3
   const v1, 33.f
   const v2, 44.f
   cmpl-float v0, v1, v2
   return v0
.end method

## CHECK-START: int TestCmp.$opt$CmpGtDoubleConstants() constant_folding (before)
## CHECK-DAG:     <<Const55:d\d+>>  DoubleConstant 55
## CHECK-DAG:     <<Const66:d\d+>>  DoubleConstant 66
## CHECK-DAG:     <<Cmp:i\d+>>      Compare [<<Const55>>,<<Const66>>] bias:gt
## CHECK-DAG:                       Return [<<Cmp>>]

## CHECK-START: int TestCmp.$opt$CmpGtDoubleConstants() constant_folding (after)
## CHECK-DAG:                       DoubleConstant 55
## CHECK-DAG:                       DoubleConstant 66
## CHECK-DAG:     <<ConstM1:i\d+>>  IntConstant -1
## CHECK-DAG:                       Return [<<ConstM1>>]

## CHECK-START: int TestCmp.$opt$CmpGtDoubleConstants() constant_folding (after)
## CHECK-NOT:                       Compare

.method public static $opt$CmpGtDoubleConstants()I
   .registers 5
   const-wide v1, 55.
   const-wide v3, 66.
   cmpg-double v0, v1, v3
   return v0
.end method

## CHECK-START: int TestCmp.$opt$CmpLtDoubleConstants() constant_folding (before)
## CHECK-DAG:     <<Const77:d\d+>>  DoubleConstant 77
## CHECK-DAG:     <<Const88:d\d+>>  DoubleConstant 88
## CHECK-DAG:     <<Cmp:i\d+>>      Compare [<<Const77>>,<<Const88>>] bias:lt
## CHECK-DAG:                       Return [<<Cmp>>]

## CHECK-START: int TestCmp.$opt$CmpLtDoubleConstants() constant_folding (after)
## CHECK-DAG:                       DoubleConstant 77
## CHECK-DAG:                       DoubleConstant 88
## CHECK-DAG:     <<ConstM1:i\d+>>  IntConstant -1
## CHECK-DAG:                       Return [<<ConstM1>>]

## CHECK-START: int TestCmp.$opt$CmpLtDoubleConstants() constant_folding (after)
## CHECK-NOT:                       Compare

.method public static $opt$CmpLtDoubleConstants()I
   .registers 5
   const-wide v1, 77.
   const-wide v3, 88.
   cmpl-double v0, v1, v3
   return v0
.end method


## CHECK-START: int TestCmp.$opt$CmpLongSameConstant() constant_folding (before)
## CHECK-DAG:     <<Const100:j\d+>> LongConstant 100
## CHECK-DAG:     <<Cmp:i\d+>>      Compare [<<Const100>>,<<Const100>>]
## CHECK-DAG:                       Return [<<Cmp>>]

## CHECK-START: int TestCmp.$opt$CmpLongSameConstant() constant_folding (after)
## CHECK-DAG:                       LongConstant 100
## CHECK-DAG:     <<Const0:i\d+>>   IntConstant 0
## CHECK-DAG:                       Return [<<Const0>>]

## CHECK-START: int TestCmp.$opt$CmpLongSameConstant() constant_folding (after)
## CHECK-NOT:                       Compare

.method public static $opt$CmpLongSameConstant()I
   .registers 5
   const-wide v1, 100
   const-wide v3, 100
   cmp-long v0, v1, v3
   return v0
.end method

## CHECK-START: int TestCmp.$opt$CmpGtFloatSameConstant() constant_folding (before)
## CHECK-DAG:     <<Const200:f\d+>> FloatConstant 200
## CHECK-DAG:     <<Cmp:i\d+>>      Compare [<<Const200>>,<<Const200>>] bias:gt
## CHECK-DAG:                       Return [<<Cmp>>]

## CHECK-START: int TestCmp.$opt$CmpGtFloatSameConstant() constant_folding (after)
## CHECK-DAG:                       FloatConstant 200
## CHECK-DAG:     <<Const0:i\d+>>   IntConstant 0
## CHECK-DAG:                       Return [<<Const0>>]

## CHECK-START: int TestCmp.$opt$CmpGtFloatSameConstant() constant_folding (after)
## CHECK-NOT:                       Compare

.method public static $opt$CmpGtFloatSameConstant()I
   .registers 3
   const v1, 200.f
   const v2, 200.f
   cmpg-float v0, v1, v2
   return v0
.end method

## CHECK-START: int TestCmp.$opt$CmpLtFloatSameConstant() constant_folding (before)
## CHECK-DAG:     <<Const300:f\d+>> FloatConstant 300
## CHECK-DAG:     <<Cmp:i\d+>>      Compare [<<Const300>>,<<Const300>>] bias:lt
## CHECK-DAG:                       Return [<<Cmp>>]

## CHECK-START: int TestCmp.$opt$CmpLtFloatSameConstant() constant_folding (after)
## CHECK-DAG:                       FloatConstant 300
## CHECK-DAG:     <<Const0:i\d+>>   IntConstant 0
## CHECK-DAG:                       Return [<<Const0>>]

## CHECK-START: int TestCmp.$opt$CmpLtFloatSameConstant() constant_folding (after)
## CHECK-NOT:                       Compare

.method public static $opt$CmpLtFloatSameConstant()I
   .registers 3
   const v1, 300.f
   const v2, 300.f
   cmpl-float v0, v1, v2
   return v0
.end method

## CHECK-START: int TestCmp.$opt$CmpGtDoubleSameConstant() constant_folding (before)
## CHECK-DAG:     <<Const400:d\d+>> DoubleConstant 400
## CHECK-DAG:     <<Cmp:i\d+>>      Compare [<<Const400>>,<<Const400>>] bias:gt
## CHECK-DAG:                       Return [<<Cmp>>]

## CHECK-START: int TestCmp.$opt$CmpGtDoubleSameConstant() constant_folding (after)
## CHECK-DAG:                       DoubleConstant 400
## CHECK-DAG:     <<Const0:i\d+>>   IntConstant 0
## CHECK-DAG:                       Return [<<Const0>>]

## CHECK-START: int TestCmp.$opt$CmpGtDoubleSameConstant() constant_folding (after)
## CHECK-NOT:                       Compare

.method public static $opt$CmpGtDoubleSameConstant()I
   .registers 5
   const-wide v1, 400.
   const-wide v3, 400.
   cmpg-double v0, v1, v3
   return v0
.end method

## CHECK-START: int TestCmp.$opt$CmpLtDoubleSameConstant() constant_folding (before)
## CHECK-DAG:     <<Const500:d\d+>> DoubleConstant 500
## CHECK-DAG:     <<Cmp:i\d+>>      Compare [<<Const500>>,<<Const500>>] bias:lt
## CHECK-DAG:                       Return [<<Cmp>>]

## CHECK-START: int TestCmp.$opt$CmpLtDoubleSameConstant() constant_folding (after)
## CHECK-DAG:                       DoubleConstant 500
## CHECK-DAG:     <<Const0:i\d+>>   IntConstant 0
## CHECK-DAG:                       Return [<<Const0>>]

## CHECK-START: int TestCmp.$opt$CmpLtDoubleSameConstant() constant_folding (after)
## CHECK-NOT:                       Compare

.method public static $opt$CmpLtDoubleSameConstant()I
   .registers 5
   const-wide v1, 500.
   const-wide v3, 500.
   cmpl-double v0, v1, v3
   return v0
.end method


## CHECK-START: int TestCmp.$opt$CmpGtFloatConstantWithNaN() constant_folding (before)
## CHECK-DAG:     <<Const44:f\d+>>  FloatConstant 44
## CHECK-DAG:     <<ConstNan:f\d+>> FloatConstant nan
## CHECK-DAG:     <<Cmp:i\d+>>      Compare [<<Const44>>,<<ConstNan>>] bias:gt
## CHECK-DAG:                       Return [<<Cmp>>]

## CHECK-START: int TestCmp.$opt$CmpGtFloatConstantWithNaN() constant_folding (after)
## CHECK-DAG:                       FloatConstant 44
## CHECK-DAG:                       FloatConstant nan
## CHECK-DAG:     <<Const1:i\d+>>   IntConstant 1
## CHECK-DAG:                       Return [<<Const1>>]

## CHECK-START: int TestCmp.$opt$CmpGtFloatConstantWithNaN() constant_folding (after)
## CHECK-NOT:                       Compare

.method public static $opt$CmpGtFloatConstantWithNaN()I
   .registers 3
   const v1, 44.f
   const v2, NaNf
   cmpg-float v0, v1, v2
   return v0
.end method

## CHECK-START: int TestCmp.$opt$CmpLtFloatConstantWithNaN() constant_folding (before)
## CHECK-DAG:     <<Const44:f\d+>>  FloatConstant 44
## CHECK-DAG:     <<ConstNan:f\d+>> FloatConstant nan
## CHECK-DAG:     <<Cmp:i\d+>>      Compare [<<Const44>>,<<ConstNan>>] bias:lt
## CHECK-DAG:                       Return [<<Cmp>>]

## CHECK-START: int TestCmp.$opt$CmpLtFloatConstantWithNaN() constant_folding (after)
## CHECK-DAG:                       FloatConstant 44
## CHECK-DAG:                       FloatConstant nan
## CHECK-DAG:     <<ConstM1:i\d+>>  IntConstant -1
## CHECK-DAG:                       Return [<<ConstM1>>]

## CHECK-START: int TestCmp.$opt$CmpLtFloatConstantWithNaN() constant_folding (after)
## CHECK-NOT:                       Compare

.method public static $opt$CmpLtFloatConstantWithNaN()I
   .registers 3
   const v1, 44.f
   const v2, NaNf
   cmpl-float v0, v1, v2
   return v0
.end method

## CHECK-START: int TestCmp.$opt$CmpGtDoubleConstantWithNaN() constant_folding (before)
## CHECK-DAG:     <<Const45:d\d+>>  DoubleConstant 45
## CHECK-DAG:     <<ConstNan:d\d+>> DoubleConstant nan
## CHECK-DAG:     <<Cmp:i\d+>>      Compare [<<Const45>>,<<ConstNan>>] bias:gt
## CHECK-DAG:                       Return [<<Cmp>>]

## CHECK-START: int TestCmp.$opt$CmpGtDoubleConstantWithNaN() constant_folding (after)
## CHECK-DAG:                       DoubleConstant 45
## CHECK-DAG:                       DoubleConstant nan
## CHECK-DAG:     <<Const1:i\d+>>   IntConstant 1
## CHECK-DAG:                       Return [<<Const1>>]

## CHECK-START: int TestCmp.$opt$CmpGtDoubleConstantWithNaN() constant_folding (after)
## CHECK-NOT:                       Compare

.method public static $opt$CmpGtDoubleConstantWithNaN()I
   .registers 5
   const-wide v1, 45.
   const-wide v3, NaN
   cmpg-double v0, v1, v3
   return v0
.end method

## CHECK-START: int TestCmp.$opt$CmpLtDoubleConstantWithNaN() constant_folding (before)
## CHECK-DAG:     <<Const46:d\d+>>  DoubleConstant 46
## CHECK-DAG:     <<ConstNan:d\d+>> DoubleConstant nan
## CHECK-DAG:     <<Cmp:i\d+>>      Compare [<<Const46>>,<<ConstNan>>] bias:lt
## CHECK-DAG:                       Return [<<Cmp>>]

## CHECK-START: int TestCmp.$opt$CmpLtDoubleConstantWithNaN() constant_folding (after)
## CHECK-DAG:                       DoubleConstant 46
## CHECK-DAG:                       DoubleConstant nan
## CHECK-DAG:     <<ConstM1:i\d+>>  IntConstant -1
## CHECK-DAG:                       Return [<<ConstM1>>]

## CHECK-START: int TestCmp.$opt$CmpLtDoubleConstantWithNaN() constant_folding (after)
## CHECK-NOT:                       Compare

.method public static $opt$CmpLtDoubleConstantWithNaN()I
   .registers 5
   const-wide v1, 46.
   const-wide v3, NaN
   cmpl-double v0, v1, v3
   return v0
.end method
