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

.method private static $inline$False()Z
    .registers 1
    const/4 v0, 0x0
    return v0
.end method

# Test a case when one entering TryBoundary is dead but the rest of the try
# block remains live.

## CHECK-START: int TestCase.testDeadEntry(int, int, int, int) dead_code_elimination_final (before)
## CHECK: Add

## CHECK-START: int TestCase.testDeadEntry(int, int, int, int) dead_code_elimination_final (before)
## CHECK:     TryBoundary kind:entry
## CHECK:     TryBoundary kind:entry
## CHECK-NOT: TryBoundary kind:entry

## CHECK-START: int TestCase.testDeadEntry(int, int, int, int) dead_code_elimination_final (after)
## CHECK-NOT: Add

## CHECK-START: int TestCase.testDeadEntry(int, int, int, int) dead_code_elimination_final (after)
## CHECK:     TryBoundary kind:entry
## CHECK-NOT: TryBoundary kind:entry

.method public static testDeadEntry(IIII)I
    .registers 5

    invoke-static {}, LTestCase;->$inline$False()Z
    move-result v0

    if-eqz v0, :else

    add-int/2addr p0, p1

    :try_start
    div-int/2addr p0, p2

    :else
    div-int/2addr p0, p3
    :try_end
    .catchall {:try_start .. :try_end} :catch_all

    :return
    return p0

    :catch_all
    const/4 p0, -0x1
    goto :return

.end method

# Test a case when one exiting TryBoundary is dead but the rest of the try
# block remains live.

## CHECK-START: int TestCase.testDeadExit(int, int, int, int) dead_code_elimination_final (before)
## CHECK: Add

## CHECK-START: int TestCase.testDeadExit(int, int, int, int) dead_code_elimination_final (before)
## CHECK:     TryBoundary kind:exit
## CHECK:     TryBoundary kind:exit
## CHECK-NOT: TryBoundary kind:exit

## CHECK-START: int TestCase.testDeadExit(int, int, int, int) dead_code_elimination_final (after)
## CHECK-NOT: Add

## CHECK-START: int TestCase.testDeadExit(int, int, int, int) dead_code_elimination_final (after)
## CHECK:     TryBoundary kind:exit
## CHECK-NOT: TryBoundary kind:exit

.method public static testDeadExit(IIII)I
    .registers 5

    invoke-static {}, LTestCase;->$inline$False()Z
    move-result v0

    :try_start
    div-int/2addr p0, p2

    if-nez v0, :else

    div-int/2addr p0, p3
    goto :return
    :try_end
    .catchall {:try_start .. :try_end} :catch_all

    :else
    add-int/2addr p0, p1

    :return
    return p0

    :catch_all
    const/4 p0, -0x1
    goto :return

.end method

# Test that a catch block remains live and consistent if some of try blocks
# throwing into it are removed.

## CHECK-START: int TestCase.testOneTryBlockDead(int, int, int, int) dead_code_elimination_final (before)
## CHECK:     TryBoundary kind:entry
## CHECK:     TryBoundary kind:entry
## CHECK-NOT: TryBoundary kind:entry

## CHECK-START: int TestCase.testOneTryBlockDead(int, int, int, int) dead_code_elimination_final (before)
## CHECK:     TryBoundary kind:exit
## CHECK:     TryBoundary kind:exit
## CHECK-NOT: TryBoundary kind:exit

## CHECK-START: int TestCase.testOneTryBlockDead(int, int, int, int) dead_code_elimination_final (after)
## CHECK:     TryBoundary kind:entry
## CHECK-NOT: TryBoundary kind:entry

## CHECK-START: int TestCase.testOneTryBlockDead(int, int, int, int) dead_code_elimination_final (after)
## CHECK:     TryBoundary kind:exit
## CHECK-NOT: TryBoundary kind:exit

.method public static testOneTryBlockDead(IIII)I
    .registers 5

    invoke-static {}, LTestCase;->$inline$False()Z
    move-result v0

    :try_start_1
    div-int/2addr p0, p2
    :try_end_1
    .catchall {:try_start_1 .. :try_end_1} :catch_all

    if-eqz v0, :return

    :try_start_2
    div-int/2addr p0, p3
    :try_end_2
    .catchall {:try_start_2 .. :try_end_2} :catch_all

    :return
    return p0

    :catch_all
    const/4 p0, -0x1
    goto :return

.end method

# Test that try block membership is recomputed. In this test case, the try entry
# stored with the merge block gets deleted and SSAChecker would fail if it was
# not replaced with the try entry from the live branch.

.method public static testRecomputeTryMembership(IIII)I
    .registers 5

    invoke-static {}, LTestCase;->$inline$False()Z
    move-result v0

    if-eqz v0, :else

    # Dead branch
    :try_start
    div-int/2addr p0, p1
    goto :merge

    # Live branch
    :else
    div-int/2addr p0, p2

    # Merge block. Make complex so it does not get merged with the live branch.
    :merge
    div-int/2addr p0, p3
    if-eqz p0, :else2
    div-int/2addr p0, p3
    :else2
    :try_end
    .catchall {:try_start .. :try_end} :catch_all

    :return
    return p0

    :catch_all
    const/4 p0, -0x1
    goto :return

.end method
