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

.class public LTestCase;

.super Ljava/lang/Object;

.method public static $inline$True()Z
  .registers 1
  const/4 v0, 1
  return v0
.end method


# CHECK-START: int TestCase.testSingleExit(int, boolean) dead_code_elimination_final (before)
# CHECK-DAG:     [[ArgX:i\d+]]  ParameterValue
# CHECK-DAG:     [[ArgY:z\d+]]  ParameterValue
# CHECK-DAG:     [[Cst1:i\d+]]  IntConstant 1
# CHECK-DAG:     [[Cst5:i\d+]]  IntConstant 5
# CHECK-DAG:     [[Cst7:i\d+]]  IntConstant 7
# CHECK-DAG:     [[PhiX:i\d+]]  Phi [ [[ArgX]] [[Add5:i\d+]] [[Add7:i\d+]] ] loop_header:[[HeaderY:B\d+]]
# CHECK-DAG:                    If [ [[ArgY]] ]                              loop_header:[[HeaderY]]
# CHECK-DAG:                    If [ [[Cst1]] ]                              loop_header:[[HeaderY]]
# CHECK-DAG:     [[Add5]]       Add [ [[PhiX]] [[Cst5]] ]                    loop_header:[[HeaderY]]
# CHECK-DAG:     [[Add7]]       Add [ [[PhiX]] [[Cst7]] ]                    loop_header:[[HeaderY]]
# CHECK-DAG:                    Return [ [[PhiX]] ]                          loop_header:null

# CHECK-START: int TestCase.testSingleExit(int, boolean) dead_code_elimination_final (after)
# CHECK-DAG:     [[ArgX:i\d+]]  ParameterValue
# CHECK-DAG:     [[ArgY:z\d+]]  ParameterValue
# CHECK-DAG:     [[Cst7:i\d+]]  IntConstant 7
# CHECK-DAG:     [[PhiX:i\d+]]  Phi [ [[ArgX]] [[AddX:i\d+]] ]               loop_header:[[HeaderY:B\d+]]
# CHECK-DAG:                    If [ [[ArgY]] ]                              loop_header:[[HeaderY]]
# CHECK-DAG:     [[AddX]]       Add [ [[PhiX]] [[Cst7]] ]                    loop_header:[[HeaderY]]
# CHECK-DAG:                    Return [ [[PhiX]] ]                          loop_header:null

.method public static testSingleExit(IZ)I
  .registers 3

  # p0 = int X
  # p1 = boolean Y
  # v0 = true

  invoke-static {}, LTestCase;->$inline$True()Z
  move-result v0

  :loop_start
  if-eqz p1, :loop_body   # cannot be determined statically
  if-nez v0, :loop_end    # will always exit

  # Dead block
  add-int/lit8 p0, p0, 5
  goto :loop_start

  # Live block
  :loop_body
  add-int/lit8 p0, p0, 7
  goto :loop_start

  :loop_end
  return p0
.end method


# CHECK-START: int TestCase.testMultipleExits(int, boolean, boolean) dead_code_elimination_final (before)
# CHECK-DAG:     [[ArgX:i\d+]]  ParameterValue
# CHECK-DAG:     [[ArgY:z\d+]]  ParameterValue
# CHECK-DAG:     [[ArgZ:z\d+]]  ParameterValue
# CHECK-DAG:     [[Cst1:i\d+]]  IntConstant 1
# CHECK-DAG:     [[Cst5:i\d+]]  IntConstant 5
# CHECK-DAG:     [[Cst7:i\d+]]  IntConstant 7
# CHECK-DAG:     [[PhiX:i\d+]]  Phi [ [[ArgX]] [[Add5:i\d+]] [[Add7:i\d+]] ] loop_header:[[HeaderY:B\d+]]
# CHECK-DAG:                    If [ [[ArgY]] ]                              loop_header:[[HeaderY]]
# CHECK-DAG:                    If [ [[ArgZ]] ]                              loop_header:[[HeaderY]]
# CHECK-DAG:                    If [ [[Cst1]] ]                              loop_header:[[HeaderY]]
# CHECK-DAG:     [[Add5]]       Add [ [[PhiX]] [[Cst5]] ]                    loop_header:[[HeaderY]]
# CHECK-DAG:     [[Add7]]       Add [ [[PhiX]] [[Cst7]] ]                    loop_header:[[HeaderY]]
# CHECK-DAG:                    Return [ [[PhiX]] ]                          loop_header:null

# CHECK-START: int TestCase.testMultipleExits(int, boolean, boolean) dead_code_elimination_final (after)
# CHECK-DAG:     [[ArgX:i\d+]]  ParameterValue
# CHECK-DAG:     [[ArgY:z\d+]]  ParameterValue
# CHECK-DAG:     [[ArgZ:z\d+]]  ParameterValue
# CHECK-DAG:     [[Cst7:i\d+]]  IntConstant 7
# CHECK-DAG:     [[PhiX:i\d+]]  Phi [ [[ArgX]] [[Add7:i\d+]] ]               loop_header:[[HeaderY:B\d+]]
# CHECK-DAG:                    If [ [[ArgY]] ]                              loop_header:[[HeaderY]]
# CHECK-DAG:     [[Add7]]       Add [ [[PhiX]] [[Cst7]] ]                    loop_header:[[HeaderY]]
# CHECK-DAG:                    If [ [[ArgZ]] ]                              loop_header:null
# CHECK-DAG:                    Return [ [[PhiX]] ]                          loop_header:null

.method public static testMultipleExits(IZZ)I
  .registers 4

  # p0 = int X
  # p1 = boolean Y
  # p2 = boolean Z
  # v0 = true

  invoke-static {}, LTestCase;->$inline$True()Z
  move-result v0

  :loop_start
  if-eqz p1, :loop_body   # cannot be determined statically
  if-nez p2, :loop_end    # may exit
  if-nez v0, :loop_end    # will always exit

  # Dead block
  add-int/lit8 p0, p0, 5
  goto :loop_start

  # Live block
  :loop_body
  add-int/lit8 p0, p0, 7
  goto :loop_start

  :loop_end
  return p0
.end method


# CHECK-START: int TestCase.testExitPredecessors(int, boolean, boolean) dead_code_elimination_final (before)
# CHECK-DAG:     [[ArgX:i\d+]]  ParameterValue
# CHECK-DAG:     [[ArgY:z\d+]]  ParameterValue
# CHECK-DAG:     [[ArgZ:z\d+]]  ParameterValue
# CHECK-DAG:     [[Cst1:i\d+]]  IntConstant 1
# CHECK-DAG:     [[Cst5:i\d+]]  IntConstant 5
# CHECK-DAG:     [[Cst7:i\d+]]  IntConstant 7
# CHECK-DAG:     [[Cst9:i\d+]]  IntConstant 9
# CHECK-DAG:     [[PhiX1:i\d+]] Phi [ [[ArgX]] [[Add5:i\d+]] [[Add7:i\d+]] ] loop_header:[[HeaderY:B\d+]]
# CHECK-DAG:                    If [ [[ArgY]] ]                              loop_header:[[HeaderY]]
# CHECK-DAG:                    If [ [[ArgZ]] ]                              loop_header:[[HeaderY]]
# CHECK-DAG:     [[Mul9:i\d+]]  Mul [ [[PhiX1]] [[Cst9]] ]                   loop_header:[[HeaderY]]
# CHECK-DAG:     [[PhiX2:i\d+]] Phi [ [[PhiX1]] [[Mul9]] ]                   loop_header:[[HeaderY]]
# CHECK-DAG:                    If [ [[Cst1]] ]                              loop_header:[[HeaderY]]
# CHECK-DAG:     [[Add5]]       Add [ [[PhiX2]] [[Cst5]] ]                   loop_header:[[HeaderY]]
# CHECK-DAG:     [[Add7]]       Add [ [[PhiX1]] [[Cst7]] ]                   loop_header:[[HeaderY]]
# CHECK-DAG:                    Return [ [[PhiX2]] ]                         loop_header:null

# CHECK-START: int TestCase.testExitPredecessors(int, boolean, boolean) dead_code_elimination_final (after)
# CHECK-DAG:     [[ArgX:i\d+]]  ParameterValue
# CHECK-DAG:     [[ArgY:z\d+]]  ParameterValue
# CHECK-DAG:     [[ArgZ:z\d+]]  ParameterValue
# CHECK-DAG:     [[Cst7:i\d+]]  IntConstant 7
# CHECK-DAG:     [[Cst9:i\d+]]  IntConstant 9
# CHECK-DAG:     [[PhiX1:i\d+]] Phi [ [[ArgX]] [[Add7:i\d+]] ]               loop_header:[[HeaderY:B\d+]]
# CHECK-DAG:                    If [ [[ArgY]] ]                              loop_header:[[HeaderY]]
# CHECK-DAG:     [[Add7]]       Add [ [[PhiX1]] [[Cst7]] ]                   loop_header:[[HeaderY]]
# CHECK-DAG:                    If [ [[ArgZ]] ]                              loop_header:null
# CHECK-DAG:     [[Mul9:i\d+]]  Mul [ [[PhiX1]] [[Cst9]] ]                   loop_header:null
# CHECK-DAG:     [[PhiX2:i\d+]] Phi [ [[PhiX1]] [[Mul9]] ]                   loop_header:null
# CHECK-DAG:                    Return [ [[PhiX2]] ]                         loop_header:null

.method public static testExitPredecessors(IZZ)I
  .registers 4

  # p0 = int X
  # p1 = boolean Y
  # p2 = boolean Z
  # v0 = true

  invoke-static {}, LTestCase;->$inline$True()Z
  move-result v0

  :loop_start
  if-eqz p1, :loop_body   # cannot be determined statically

  # Additional logic which will end up outside the loop
  if-eqz p2, :skip_if
  mul-int/lit8 p0, p0, 9
  :skip_if

  if-nez v0, :loop_end    # will always take the branch

  # Dead block
  add-int/lit8 p0, p0, 5
  goto :loop_start

  # Live block
  :loop_body
  add-int/lit8 p0, p0, 7
  goto :loop_start

  :loop_end
  return p0
.end method


# CHECK-START: int TestCase.testInnerLoop(int, boolean, boolean) dead_code_elimination_final (before)
# CHECK-DAG:     [[ArgX:i\d+]]  ParameterValue
# CHECK-DAG:     [[ArgY:z\d+]]  ParameterValue
# CHECK-DAG:     [[ArgZ:z\d+]]  ParameterValue
# CHECK-DAG:     [[Cst0:i\d+]]  IntConstant 0
# CHECK-DAG:     [[Cst1:i\d+]]  IntConstant 1
# CHECK-DAG:     [[Cst5:i\d+]]  IntConstant 5
# CHECK-DAG:     [[Cst7:i\d+]]  IntConstant 7
#
# CHECK-DAG:     [[PhiX:i\d+]]  Phi [ [[ArgX]] [[Add5:i\d+]] [[Add7:i\d+]] ] loop_header:[[HeaderY:B\d+]]
# CHECK-DAG:     [[PhiZ1:i\d+]] Phi [ [[ArgZ]] [[XorZ:i\d+]] [[PhiZ1]] ]     loop_header:[[HeaderY]]
# CHECK-DAG:                    If [ [[ArgY]] ]                              loop_header:[[HeaderY]]
#
#                               ### Inner loop ###
# CHECK-DAG:     [[PhiZ2:i\d+]] Phi [ [[PhiZ1]] [[XorZ]] ]                   loop_header:[[HeaderZ:B\d+]]
# CHECK-DAG:     [[XorZ]]       Xor [ [[PhiZ2]] [[Cst1]] ]                   loop_header:[[HeaderZ]]
# CHECK-DAG:     [[CondZ:z\d+]] Equal [ [[XorZ]] [[Cst0]] ]                  loop_header:[[HeaderZ]]
# CHECK-DAG:                    If [ [[CondZ]] ]                             loop_header:[[HeaderZ]]
#
# CHECK-DAG:     [[Add5]]       Add [ [[PhiX]] [[Cst5]] ]                    loop_header:[[HeaderY]]
# CHECK-DAG:     [[Add7]]       Add [ [[PhiX]] [[Cst7]] ]                    loop_header:[[HeaderY]]
# CHECK-DAG:                    Return [ [[PhiX]] ]                          loop_header:null

# CHECK-START: int TestCase.testInnerLoop(int, boolean, boolean) dead_code_elimination_final (after)
# CHECK-DAG:     [[ArgX:i\d+]]  ParameterValue
# CHECK-DAG:     [[ArgY:z\d+]]  ParameterValue
# CHECK-DAG:     [[ArgZ:z\d+]]  ParameterValue
# CHECK-DAG:     [[Cst0:i\d+]]  IntConstant 0
# CHECK-DAG:     [[Cst1:i\d+]]  IntConstant 1
# CHECK-DAG:     [[Cst7:i\d+]]  IntConstant 7
#
# CHECK-DAG:     [[PhiX:i\d+]]  Phi [ [[ArgX]] [[Add7:i\d+]] ]               loop_header:[[HeaderY:B\d+]]
# CHECK-DAG:     [[PhiZ1:i\d+]] Phi [ [[ArgZ]] [[PhiZ1]] ]                   loop_header:[[HeaderY]]
# CHECK-DAG:                    If [ [[ArgY]] ]                              loop_header:[[HeaderY]]
# CHECK-DAG:     [[Add7]]       Add [ [[PhiX]] [[Cst7]] ]                    loop_header:[[HeaderY]]
#
#                               ### Inner loop ###
# CHECK-DAG:     [[PhiZ2:i\d+]] Phi [ [[PhiZ1]] [[XorZ:i\d+]] ]              loop_header:[[HeaderZ:B\d+]]
# CHECK-DAG:     [[XorZ]]       Xor [ [[PhiZ2]] [[Cst1]] ]                   loop_header:[[HeaderZ]]
# CHECK-DAG:     [[CondZ:z\d+]] Equal [ [[XorZ]] [[Cst0]] ]                  loop_header:[[HeaderZ]]
# CHECK-DAG:                    If [ [[CondZ]] ]                             loop_header:[[HeaderZ]]
#
# CHECK-DAG:                    Return [ [[PhiX]] ]                          loop_header:null

.method public static testInnerLoop(IZZ)I
  .registers 4

  # p0 = int X
  # p1 = boolean Y
  # p2 = boolean Z
  # v0 = true

  invoke-static {}, LTestCase;->$inline$True()Z
  move-result v0

  :loop_start
  if-eqz p1, :loop_body   # cannot be determined statically

  # Inner loop which will end up outside its parent
  :inner_loop_start
  xor-int/lit8 p2, p2, 1
  if-eqz p2, :inner_loop_start

  if-nez v0, :loop_end    # will always take the branch

  # Dead block
  add-int/lit8 p0, p0, 5
  goto :loop_start

  # Live block
  :loop_body
  add-int/lit8 p0, p0, 7
  goto :loop_start

  :loop_end
  return p0
.end method
