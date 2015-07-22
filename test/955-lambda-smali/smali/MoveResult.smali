#
#  Copyright (C) 2015 The Android Open Source Project
#
#  Licensed under the Apache License, Version 2.0 (the "License");
#  you may not use this file except in compliance with the License.
#  You may obtain a copy of the License at
#
#       http://www.apache.org/licenses/LICENSE-2.0
#
#  Unless required by applicable law or agreed to in writing, software
#  distributed under the License is distributed on an "AS IS" BASIS,
#  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#  See the License for the specific language governing permissions and
#  limitations under the License.
#
.class public LMoveResult;
.super Ljava/lang/Object;

.method public constructor <init>()V
.registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method

.method public static run()V
.registers 8
    invoke-static {}, LMoveResult;->testZ()V
    invoke-static {}, LMoveResult;->testB()V
    invoke-static {}, LMoveResult;->testS()V
    invoke-static {}, LMoveResult;->testI()V
    invoke-static {}, LMoveResult;->testC()V
    invoke-static {}, LMoveResult;->testJ()V
    invoke-static {}, LMoveResult;->testF()V
    invoke-static {}, LMoveResult;->testD()V
    invoke-static {}, LMoveResult;->testL()V

    return-void
.end method

# Test that booleans are returned correctly via move-result.
.method public static testZ()V
    .registers 6

    create-lambda v0, LMoveResult;->lambdaZ(Ljava/lang/reflect/ArtMethod;)Z
    invoke-lambda v0, {}
    move-result v2
    const v3, 1

    if-ne v3, v2, :is_not_equal
    const-string v4, "(MoveResult) testZ success"
    goto :end

:is_not_equal
    const-string v4, "(MoveResult) testZ failed"

:end
    sget-object v5, Ljava/lang/System;->out:Ljava/io/PrintStream;
    invoke-virtual {v5, v4}, Ljava/io/PrintStream;->println(Ljava/lang/String;)V
    return-void

.end method

# Lambda target for testZ. Always returns "true".
.method public static lambdaZ(Ljava/lang/reflect/ArtMethod;)Z
    .registers 3

    const v0, 1
    return v0

.end method

# Test that bytes are returned correctly via move-result.
.method public static testB()V
    .registers 6

    create-lambda v0, LMoveResult;->lambdaB(Ljava/lang/reflect/ArtMethod;)B
    invoke-lambda v0, {}
    move-result v2
    const v3, 15

    if-ne v3, v2, :is_not_equal
    const-string v4, "(MoveResult) testB success"
    goto :end

:is_not_equal
    const-string v4, "(MoveResult) testB failed"

:end
    sget-object v5, Ljava/lang/System;->out:Ljava/io/PrintStream;
    invoke-virtual {v5, v4}, Ljava/io/PrintStream;->println(Ljava/lang/String;)V
    return-void

.end method

# Lambda target for testB. Always returns "15".
.method public static lambdaB(Ljava/lang/reflect/ArtMethod;)B
    .registers 3 # 1 parameters, 2 locals

    const v0, 15
    return v0

.end method

# Test that shorts are returned correctly via move-result.
.method public static testS()V
    .registers 6

    create-lambda v0, LMoveResult;->lambdaS(Ljava/lang/reflect/ArtMethod;)S
    invoke-lambda v0, {}
    move-result v2
    const/16 v3, 31000

    if-ne v3, v2, :is_not_equal
    const-string v4, "(MoveResult) testS success"
    goto :end

:is_not_equal
    const-string v4, "(MoveResult) testS failed"

:end
    sget-object v5, Ljava/lang/System;->out:Ljava/io/PrintStream;
    invoke-virtual {v5, v4}, Ljava/io/PrintStream;->println(Ljava/lang/String;)V
    return-void

.end method

# Lambda target for testS. Always returns "31000".
.method public static lambdaS(Ljava/lang/reflect/ArtMethod;)S
    .registers 3

    const/16 v0, 31000
    return v0

.end method

# Test that ints are returned correctly via move-result.
.method public static testI()V
    .registers 6

    create-lambda v0, LMoveResult;->lambdaI(Ljava/lang/reflect/ArtMethod;)I
    invoke-lambda v0, {}
    move-result v2
    const v3, 128000

    if-ne v3, v2, :is_not_equal
    const-string v4, "(MoveResult) testI success"
    goto :end

:is_not_equal
    const-string v4, "(MoveResult) testI failed"

:end
    sget-object v5, Ljava/lang/System;->out:Ljava/io/PrintStream;
    invoke-virtual {v5, v4}, Ljava/io/PrintStream;->println(Ljava/lang/String;)V
    return-void

.end method

# Lambda target for testI. Always returns "128000".
.method public static lambdaI(Ljava/lang/reflect/ArtMethod;)I
    .registers 3

    const v0, 128000
    return v0

.end method

# Test that chars are returned correctly via move-result.
.method public static testC()V
    .registers 6

    create-lambda v0, LMoveResult;->lambdaC(Ljava/lang/reflect/ArtMethod;)C
    invoke-lambda v0, {}
    move-result v2
    const v3, 65535

    if-ne v3, v2, :is_not_equal
    const-string v4, "(MoveResult) testC success"
    goto :end

:is_not_equal
    const-string v4, "(MoveResult) testC failed"

:end
    sget-object v5, Ljava/lang/System;->out:Ljava/io/PrintStream;
    invoke-virtual {v5, v4}, Ljava/io/PrintStream;->println(Ljava/lang/String;)V
    return-void

.end method

# Lambda target for testC. Always returns "65535".
.method public static lambdaC(Ljava/lang/reflect/ArtMethod;)C
    .registers 3

    const v0, 65535
    return v0

.end method

# Test that longs are returned correctly via move-result.
.method public static testJ()V
    .registers 8

    create-lambda v0, LMoveResult;->lambdaJ(Ljava/lang/reflect/ArtMethod;)J
    invoke-lambda v0, {}
    move-result v2
    const-wide v4, 0xdeadf00dc0ffee

    if-ne v4, v2, :is_not_equal
    const-string v6, "(MoveResult) testJ success"
    goto :end

:is_not_equal
    const-string v6, "(MoveResult) testJ failed"

:end
    sget-object v7, Ljava/lang/System;->out:Ljava/io/PrintStream;
    invoke-virtual {v7, v6}, Ljava/io/PrintStream;->println(Ljava/lang/String;)V
    return-void

.end method

# Lambda target for testC. Always returns "0xdeadf00dc0ffee".
.method public static lambdaJ(Ljava/lang/reflect/ArtMethod;)J
    .registers 4

    const-wide v0, 0xdeadf00dc0ffee
    return-wide v0

.end method

# Test that floats are returned correctly via move-result.
.method public static testF()V
    .registers 6

    create-lambda v0, LMoveResult;->lambdaF(Ljava/lang/reflect/ArtMethod;)F
    invoke-lambda v0, {}
    move-result v2
    const v3, infinityf

    if-ne v3, v2, :is_not_equal
    const-string v4, "(MoveResult) testF success"
    goto :end

:is_not_equal
    const-string v4, "(MoveResult) testF failed"

:end
    sget-object v5, Ljava/lang/System;->out:Ljava/io/PrintStream;
    invoke-virtual {v5, v4}, Ljava/io/PrintStream;->println(Ljava/lang/String;)V
    return-void

.end method

# Lambda target for testF. Always returns "infinityf".
.method public static lambdaF(Ljava/lang/reflect/ArtMethod;)F
    .registers 3

    const v0, infinityf
    return v0

.end method

# Test that doubles are returned correctly via move-result.
.method public static testD()V
    .registers 8

    create-lambda v0, LMoveResult;->lambdaD(Ljava/lang/reflect/ArtMethod;)D
    invoke-lambda v0, {}
    move-result-wide v2
    const-wide v4, infinity

    if-ne v4, v2, :is_not_equal
    const-string v6, "(MoveResult) testD success"
    goto :end

:is_not_equal
    const-string v6, "(MoveResult) testD failed"

:end
    sget-object v7, Ljava/lang/System;->out:Ljava/io/PrintStream;
    invoke-virtual {v7, v6}, Ljava/io/PrintStream;->println(Ljava/lang/String;)V
    return-void

.end method

# Lambda target for testD. Always returns "infinity".
.method public static lambdaD(Ljava/lang/reflect/ArtMethod;)D
    .registers 4

    const-wide v0, infinity # 123.456789
    return-wide v0

.end method


# Test that objects are returned correctly via move-result.
.method public static testL()V
    .registers 8

    create-lambda v0, LMoveResult;->lambdaL(Ljava/lang/reflect/ArtMethod;)Ljava/lang/String;
    invoke-lambda v0, {}
    move-result-object v2
    const-string v4, "Interned string"

    # relies on string interning returning identical object references
    if-ne v4, v2, :is_not_equal
    const-string v6, "(MoveResult) testL success"
    goto :end

:is_not_equal
    const-string v6, "(MoveResult) testL failed"

:end
    sget-object v7, Ljava/lang/System;->out:Ljava/io/PrintStream;
    invoke-virtual {v7, v6}, Ljava/io/PrintStream;->println(Ljava/lang/String;)V
    return-void

.end method

# Lambda target for testL. Always returns "Interned string" (string).
.method public static lambdaL(Ljava/lang/reflect/ArtMethod;)Ljava/lang/String;
    .registers 4

    const-string v0, "Interned string"
    return-object v0

.end method


