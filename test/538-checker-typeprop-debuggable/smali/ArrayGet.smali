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

.class public LArrayGet;
.super Ljava/lang/Object;


# Test phi with fixed-type ArrayGet as an input and a matching second input.
# The phi should be typed accordingly.

## CHECK-START: void ArrayGet.matchingFixedType(float[], float) ssa_builder (after)
## CHECK-NOT: Phi

## CHECK-START-DEBUGGABLE: void ArrayGet.matchingFixedType(float[], float) ssa_builder (after)
## CHECK-DAG:  <<Arg1:f\d+>> ParameterValue
## CHECK-DAG:  <<Aget:f\d+>> ArrayGet
## CHECK-DAG:  {{f\d+}}      Phi [<<Aget>>,<<Arg1>>] reg:0
.method public static matchingFixedType([FF)V
  .registers 8

  const v0, 0x0
  const v1, 0x1

  aget v0, p0, v0       # read value
  add-float v2, v0, v1  # float use fixes type

  float-to-int v2, p1
  if-eqz v2, :after
  move v0, p1
  :after
  # v0 = Phi [ArrayGet, Arg1] => float

  invoke-static {}, Ljava/lang/System;->nanoTime()J  # create an env use
  return-void
.end method


# Test phi with fixed-type ArrayGet as an input and a conflicting second input.
# The phi should be eliminated due to the conflict.

## CHECK-START: void ArrayGet.conflictingFixedType(float[], int) ssa_builder (after)
## CHECK-NOT: Phi

## CHECK-START-DEBUGGABLE: void ArrayGet.conflictingFixedType(float[], int) ssa_builder (after)
## CHECK-NOT: Phi
.method public static conflictingFixedType([FI)V
  .registers 8

  const v0, 0x0
  const v1, 0x1

  aget v0, p0, v0       # read value
  add-float v2, v0, v1  # float use fixes type

  if-eqz p1, :after
  move v0, p1
  :after
  # v0 = Phi [ArrayGet, Arg1] => conflict

  invoke-static {}, Ljava/lang/System;->nanoTime()J  # create an env use
  return-void
.end method


# Same test as the one above, only this time tests that type of ArrayGet is not
# changed.

## CHECK-START: void ArrayGet.conflictingFixedType2(int[], float) ssa_builder (after)
## CHECK-NOT: Phi

## CHECK-START-DEBUGGABLE: void ArrayGet.conflictingFixedType2(int[], float) ssa_builder (after)
## CHECK-NOT: Phi

## CHECK-START-DEBUGGABLE: void ArrayGet.conflictingFixedType2(int[], float) ssa_builder (after)
## CHECK:     {{i\d+}} ArrayGet
.method public static conflictingFixedType2([IF)V
  .registers 8

  const v0, 0x0
  const v1, 0x1

  aget v0, p0, v0       # read value
  add-int v2, v0, v1    # int use fixes type

  float-to-int v2, p1
  if-eqz v2, :after
  move v0, p1
  :after
  # v0 = Phi [ArrayGet, Arg1] => conflict

  invoke-static {}, Ljava/lang/System;->nanoTime()J  # create an env use
  return-void
.end method


# Test phi with free-type ArrayGet as an input and a matching second input.
# The phi should be typed accordingly.

## CHECK-START: void ArrayGet.matchingFreeType(float[], float) ssa_builder (after)
## CHECK-NOT: Phi

## CHECK-START-DEBUGGABLE: void ArrayGet.matchingFreeType(float[], float) ssa_builder (after)
## CHECK-DAG:  <<Arg1:f\d+>> ParameterValue
## CHECK-DAG:  <<Aget:f\d+>> ArrayGet
## CHECK-DAG:                ArraySet [{{l\d+}},{{i\d+}},<<Aget>>]
## CHECK-DAG:  {{f\d+}}      Phi [<<Aget>>,<<Arg1>>] reg:0
.method public static matchingFreeType([FF)V
  .registers 8

  const v0, 0x0
  const v1, 0x1

  aget v0, p0, v0       # read value, should be float but has no typed use
  aput v0, p0, v1       # aput does not disambiguate the type

  float-to-int v2, p1
  if-eqz v2, :after
  move v0, p1
  :after
  # v0 = Phi [ArrayGet, Arg1] => float

  invoke-static {}, Ljava/lang/System;->nanoTime()J  # create an env use
  return-void
.end method


# Test phi with free-type ArrayGet as an input and a conflicting second input.
# The phi will be kept and typed according to the second input despite the
# conflict.

## CHECK-START: void ArrayGet.conflictingFreeType(int[], float) ssa_builder (after)
## CHECK-NOT: Phi

## CHECK-START-DEBUGGABLE: void ArrayGet.conflictingFreeType(int[], float) ssa_builder (after)
## CHECK-DAG:  <<Arg1:f\d+>> ParameterValue
## CHECK-DAG:  <<Aget:f\d+>> ArrayGet
## CHECK-DAG:                ArraySet [{{l\d+}},{{i\d+}},<<Aget>>]
## CHECK-DAG:  {{f\d+}}      Phi [<<Aget>>,<<Arg1>>] reg:0
.method public static conflictingFreeType([IF)V
  .registers 8

  const v0, 0x0
  const v1, 0x1

  aget v0, p0, v0       # read value, should be int but has no typed use
  aput v0, p0, v1       # aput does not disambiguate the type

  float-to-int v2, p1
  if-eqz v2, :after
  move v0, p1
  :after
  # v0 = Phi [ArrayGet, Arg1] => float

  invoke-static {}, Ljava/lang/System;->nanoTime()J  # create an env use
  return-void
.end method
