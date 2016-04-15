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

.class public LIrreducibleLoop;
.super Ljava/lang/Object;

# Test case where liveness analysis produces linear order where loop blocks are
# not adjacent.

## CHECK-START: int IrreducibleLoop.liveness(boolean, boolean, boolean, int) builder (after)
## CHECK-DAG:     Add loop:none
## CHECK-DAG:     Mul loop:<<Loop:B\d+>>
## CHECK-DAG:     Not loop:<<Loop>>

## CHECK-START: int IrreducibleLoop.liveness(boolean, boolean, boolean, int) liveness (after)
## CHECK-DAG:     Add liveness:<<LPreEntry:\d+>>
## CHECK-DAG:     Mul liveness:<<LHeader:\d+>>
## CHECK-DAG:     Not liveness:<<LBackEdge:\d+>>
## CHECK-EVAL:    (<<LHeader>> < <<LPreEntry>>) and (<<LPreEntry>> < <<LBackEdge>>)

.method public static liveness(ZZZI)I
   .registers 10
   const/16 v0, 42

   if-eqz p0, :header

   :pre_entry
   add-int/2addr p3, p3
   invoke-static {v0}, Ljava/lang/System;->exit(I)V
   goto :body1

   :header
   mul-int/2addr p3, p3
   if-eqz p1, :body2

   :body1
   goto :body_merge

   :body2
   invoke-static {v0}, Ljava/lang/System;->exit(I)V
   goto :body_merge

   :body_merge
   if-eqz p2, :exit

   :back_edge
   not-int p3, p3
   goto :header

   :exit
   return p3

.end method
