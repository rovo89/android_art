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
.class public LBoxUnbox;
.super Ljava/lang/Object;

.method public constructor <init>()V
.registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method

.method public static run()V
.registers 2
    # Trivial 0-arg hello world
    create-lambda v0, LBoxUnbox;->doHelloWorld(Ljava/lang/reflect/ArtMethod;)V
    # TODO: create-lambda should not write to both v0 and v1
    invoke-lambda v0, {}

    invoke-static {}, LBoxUnbox;->testFailures()V
    invoke-static {}, LBoxUnbox;->testFailures2()V
    invoke-static {}, LBoxUnbox;->testFailures3()V

    return-void
.end method

#TODO: should use a closure type instead of ArtMethod.
.method public static doHelloWorld(Ljava/lang/reflect/ArtMethod;)V
    .registers 3 # 1 parameters, 2 locals

    const-string v0, "(BoxUnbox) Hello boxing world! (0-args, no closure)"

    sget-object v1, Ljava/lang/System;->out:Ljava/io/PrintStream;
    invoke-virtual {v1, v0}, Ljava/io/PrintStream;->println(Ljava/lang/String;)V

    return-void
.end method

# Test exceptions are thrown as expected when used opcodes incorrectly
.method private static testFailures()V
    .registers 4 # 0 parameters, 4 locals

    const v0, 0  # v0 = null
    const v1, 0  # v1 = null
:start
    unbox-lambda v2, v0, Ljava/lang/reflect/ArtMethod;
    # attempting to unbox a null lambda will throw NPE
:end
    return-void

:handler
    const-string v2, "(BoxUnbox) Caught NPE for unbox-lambda"
    sget-object v3, Ljava/lang/System;->out:Ljava/io/PrintStream;
    invoke-virtual {v3, v2}, Ljava/io/PrintStream;->println(Ljava/lang/String;)V

    return-void

    .catch Ljava/lang/NullPointerException; {:start .. :end} :handler
.end method

# Test exceptions are thrown as expected when used opcodes incorrectly
.method private static testFailures2()V
    .registers 4 # 0 parameters, 4 locals

    const v0, 0  # v0 = null
    const v1, 0  # v1 = null
:start
    box-lambda v2, v0  # attempting to box a null lambda will throw NPE
:end
    return-void

    # TODO: refactor testFailures using a goto

:handler
    const-string v2, "(BoxUnbox) Caught NPE for box-lambda"
    sget-object v3, Ljava/lang/System;->out:Ljava/io/PrintStream;
    invoke-virtual {v3, v2}, Ljava/io/PrintStream;->println(Ljava/lang/String;)V

    return-void

    .catch Ljava/lang/NullPointerException; {:start .. :end} :handler
.end method

# Test exceptions are thrown as expected when used opcodes incorrectly
.method private static testFailures3()V
    .registers 4 # 0 parameters, 4 locals

    const-string v0, "This is not a boxed lambda"
:start
    # TODO: use \FunctionalType; here instead
    unbox-lambda v2, v0, Ljava/lang/reflect/ArtMethod;
    # can't use a string, expects a lambda object here. throws ClassCastException.
:end
    return-void

    # TODO: refactor testFailures using a goto

:handler
    const-string v2, "(BoxUnbox) Caught ClassCastException for unbox-lambda"
    sget-object v3, Ljava/lang/System;->out:Ljava/io/PrintStream;
    invoke-virtual {v3, v2}, Ljava/io/PrintStream;->println(Ljava/lang/String;)V

    return-void

    .catch Ljava/lang/ClassCastException; {:start .. :end} :handler
.end method
