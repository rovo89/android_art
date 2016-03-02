# Copyright (C) 2016 The Android Open Source Project
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

.class public LSmaliTests;
.super Ljava/lang/Object;

## CHECK-START: int SmaliTests.EqualTrueRhs(boolean) instruction_simplifier (before)
## CHECK-DAG:     <<Arg:z\d+>>      ParameterValue
## CHECK-DAG:     <<Const1:i\d+>>   IntConstant 1
## CHECK-DAG:     <<Cond:z\d+>>     Equal [<<Arg>>,<<Const1>>]
## CHECK-DAG:                       If [<<Cond>>]

## CHECK-START: int SmaliTests.EqualTrueRhs(boolean) instruction_simplifier (after)
## CHECK-DAG:     <<Arg:z\d+>>      ParameterValue
## CHECK-DAG:                       If [<<Arg>>]

.method public static EqualTrueRhs(Z)I
  .registers 3

  const v0, 0x1
  const v1, 0x5
  if-eq p0, v0, :return
  const v1, 0x3
  :return
  return v1

.end method

## CHECK-START: int SmaliTests.EqualTrueLhs(boolean) instruction_simplifier (before)
## CHECK-DAG:     <<Arg:z\d+>>      ParameterValue
## CHECK-DAG:     <<Const1:i\d+>>   IntConstant 1
## CHECK-DAG:     <<Cond:z\d+>>     Equal [<<Const1>>,<<Arg>>]
## CHECK-DAG:                       If [<<Cond>>]

## CHECK-START: int SmaliTests.EqualTrueLhs(boolean) instruction_simplifier (after)
## CHECK-DAG:     <<Arg:z\d+>>      ParameterValue
## CHECK-DAG:                       If [<<Arg>>]

.method public static EqualTrueLhs(Z)I
  .registers 3

  const v0, 0x1
  const v1, 0x5
  if-eq v0, p0, :return
  const v1, 0x3
  :return
  return v1

.end method

## CHECK-START: int SmaliTests.EqualFalseRhs(boolean) instruction_simplifier (before)
## CHECK-DAG:     <<Arg:z\d+>>      ParameterValue
## CHECK-DAG:     <<Const0:i\d+>>   IntConstant 0
## CHECK-DAG:     <<Cond:z\d+>>     Equal [<<Arg>>,<<Const0>>]
## CHECK-DAG:                       If [<<Cond>>]

## CHECK-START: int SmaliTests.EqualFalseRhs(boolean) instruction_simplifier (after)
## CHECK-DAG:     <<Arg:z\d+>>      ParameterValue
## CHECK-DAG:                       If [<<Arg>>]

.method public static EqualFalseRhs(Z)I
  .registers 3

  const v0, 0x0
  const v1, 0x3
  if-eq p0, v0, :return
  const v1, 0x5
  :return
  return v1

.end method

## CHECK-START: int SmaliTests.EqualFalseLhs(boolean) instruction_simplifier (before)
## CHECK-DAG:     <<Arg:z\d+>>      ParameterValue
## CHECK-DAG:     <<Const0:i\d+>>   IntConstant 0
## CHECK-DAG:     <<Cond:z\d+>>     Equal [<<Const0>>,<<Arg>>]
## CHECK-DAG:                       If [<<Cond>>]

## CHECK-START: int SmaliTests.EqualFalseLhs(boolean) instruction_simplifier (after)
## CHECK-DAG:     <<Arg:z\d+>>      ParameterValue
## CHECK-DAG:                       If [<<Arg>>]

.method public static EqualFalseLhs(Z)I
  .registers 3

  const v0, 0x0
  const v1, 0x3
  if-eq v0, p0, :return
  const v1, 0x5
  :return
  return v1

.end method

## CHECK-START: int SmaliTests.NotEqualTrueRhs(boolean) instruction_simplifier (before)
## CHECK-DAG:     <<Arg:z\d+>>      ParameterValue
## CHECK-DAG:     <<Const1:i\d+>>   IntConstant 1
## CHECK-DAG:     <<Cond:z\d+>>     NotEqual [<<Arg>>,<<Const1>>]
## CHECK-DAG:                       If [<<Cond>>]

## CHECK-START: int SmaliTests.NotEqualTrueRhs(boolean) instruction_simplifier (after)
## CHECK-DAG:     <<Arg:z\d+>>      ParameterValue
## CHECK-DAG:                       If [<<Arg>>]

.method public static NotEqualTrueRhs(Z)I
  .registers 3

  const v0, 0x1
  const v1, 0x3
  if-ne p0, v0, :return
  const v1, 0x5
  :return
  return v1

.end method

## CHECK-START: int SmaliTests.NotEqualTrueLhs(boolean) instruction_simplifier (before)
## CHECK-DAG:     <<Arg:z\d+>>      ParameterValue
## CHECK-DAG:     <<Const1:i\d+>>   IntConstant 1
## CHECK-DAG:     <<Cond:z\d+>>     NotEqual [<<Const1>>,<<Arg>>]
## CHECK-DAG:                       If [<<Cond>>]

## CHECK-START: int SmaliTests.NotEqualTrueLhs(boolean) instruction_simplifier (after)
## CHECK-DAG:     <<Arg:z\d+>>      ParameterValue
## CHECK-DAG:                       If [<<Arg>>]

.method public static NotEqualTrueLhs(Z)I
  .registers 3

  const v0, 0x1
  const v1, 0x3
  if-ne v0, p0, :return
  const v1, 0x5
  :return
  return v1

.end method

## CHECK-START: int SmaliTests.NotEqualFalseRhs(boolean) instruction_simplifier (before)
## CHECK-DAG:     <<Arg:z\d+>>      ParameterValue
## CHECK-DAG:     <<Const0:i\d+>>   IntConstant 0
## CHECK-DAG:     <<Cond:z\d+>>     NotEqual [<<Arg>>,<<Const0>>]
## CHECK-DAG:                       If [<<Cond>>]

## CHECK-START: int SmaliTests.NotEqualFalseRhs(boolean) instruction_simplifier (after)
## CHECK-DAG:     <<Arg:z\d+>>      ParameterValue
## CHECK-DAG:                       If [<<Arg>>]

.method public static NotEqualFalseRhs(Z)I
  .registers 3

  const v0, 0x0
  const v1, 0x5
  if-ne p0, v0, :return
  const v1, 0x3
  :return
  return v1

.end method

## CHECK-START: int SmaliTests.NotEqualFalseLhs(boolean) instruction_simplifier (before)
## CHECK-DAG:     <<Arg:z\d+>>      ParameterValue
## CHECK-DAG:     <<Const0:i\d+>>   IntConstant 0
## CHECK-DAG:     <<Cond:z\d+>>     NotEqual [<<Const0>>,<<Arg>>]
## CHECK-DAG:                       If [<<Cond>>]

## CHECK-START: int SmaliTests.NotEqualFalseLhs(boolean) instruction_simplifier (after)
## CHECK-DAG:     <<Arg:z\d+>>      ParameterValue
## CHECK-DAG:                       If [<<Arg>>]

.method public static NotEqualFalseLhs(Z)I
  .registers 3

  const v0, 0x0
  const v1, 0x5
  if-ne v0, p0, :return
  const v1, 0x3
  :return
  return v1

.end method

