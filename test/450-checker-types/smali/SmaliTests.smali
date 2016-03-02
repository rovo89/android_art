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

## CHECK-START: void SmaliTests.testInstanceOf_EQ0_NotInlined(java.lang.Object) builder (after)
## CHECK-DAG:     <<Cst0:i\d+>> IntConstant 0
## CHECK-DAG:     <<IOf:z\d+>>  InstanceOf
## CHECK-DAG:                   Equal [<<IOf>>,<<Cst0>>]

## CHECK-START: void SmaliTests.testInstanceOf_EQ0_NotInlined(java.lang.Object) instruction_simplifier (before)
## CHECK:         CheckCast

## CHECK-START: void SmaliTests.testInstanceOf_EQ0_NotInlined(java.lang.Object) instruction_simplifier (after)
## CHECK-NOT:     CheckCast

.method public static testInstanceOf_EQ0_NotInlined(Ljava/lang/Object;)V
  .registers 3

  const v0, 0x0
  instance-of v1, p0, LSubclassC;
  if-eq v1, v0, :return

  check-cast p0, LSubclassC;
  invoke-virtual {p0}, LSubclassC;->$noinline$g()V

  :return
  return-void

.end method

## CHECK-START: void SmaliTests.testInstanceOf_EQ1_NotInlined(java.lang.Object) builder (after)
## CHECK-DAG:     <<Cst1:i\d+>> IntConstant 1
## CHECK-DAG:     <<IOf:z\d+>>  InstanceOf
## CHECK-DAG:                   Equal [<<IOf>>,<<Cst1>>]

## CHECK-START: void SmaliTests.testInstanceOf_EQ1_NotInlined(java.lang.Object) instruction_simplifier (before)
## CHECK:         CheckCast

## CHECK-START: void SmaliTests.testInstanceOf_EQ1_NotInlined(java.lang.Object) instruction_simplifier (after)
## CHECK-NOT:     CheckCast

.method public static testInstanceOf_EQ1_NotInlined(Ljava/lang/Object;)V
  .registers 3

  const v0, 0x1
  instance-of v1, p0, LSubclassC;
  if-eq v1, v0, :invoke
  return-void

  :invoke
  check-cast p0, LSubclassC;
  invoke-virtual {p0}, LSubclassC;->$noinline$g()V
  return-void

.end method

## CHECK-START: void SmaliTests.testInstanceOf_NE0_NotInlined(java.lang.Object) builder (after)
## CHECK-DAG:     <<Cst0:i\d+>> IntConstant 0
## CHECK-DAG:     <<IOf:z\d+>>  InstanceOf
## CHECK-DAG:                   NotEqual [<<IOf>>,<<Cst0>>]

## CHECK-START: void SmaliTests.testInstanceOf_NE0_NotInlined(java.lang.Object) instruction_simplifier (before)
## CHECK:         CheckCast

## CHECK-START: void SmaliTests.testInstanceOf_NE0_NotInlined(java.lang.Object) instruction_simplifier (after)
## CHECK-NOT:     CheckCast

.method public static testInstanceOf_NE0_NotInlined(Ljava/lang/Object;)V
  .registers 3

  const v0, 0x0
  instance-of v1, p0, LSubclassC;
  if-ne v1, v0, :invoke
  return-void

  :invoke
  check-cast p0, LSubclassC;
  invoke-virtual {p0}, LSubclassC;->$noinline$g()V
  return-void

.end method

## CHECK-START: void SmaliTests.testInstanceOf_NE1_NotInlined(java.lang.Object) builder (after)
## CHECK-DAG:     <<Cst1:i\d+>> IntConstant 1
## CHECK-DAG:     <<IOf:z\d+>>  InstanceOf
## CHECK-DAG:                   NotEqual [<<IOf>>,<<Cst1>>]

## CHECK-START: void SmaliTests.testInstanceOf_NE1_NotInlined(java.lang.Object) instruction_simplifier (before)
## CHECK:         CheckCast

## CHECK-START: void SmaliTests.testInstanceOf_NE1_NotInlined(java.lang.Object) instruction_simplifier (after)
## CHECK-NOT:     CheckCast

.method public static testInstanceOf_NE1_NotInlined(Ljava/lang/Object;)V
  .registers 3

  const v0, 0x1
  instance-of v1, p0, LSubclassC;
  if-ne v1, v0, :return

  check-cast p0, LSubclassC;
  invoke-virtual {p0}, LSubclassC;->$noinline$g()V

  :return
  return-void

.end method
