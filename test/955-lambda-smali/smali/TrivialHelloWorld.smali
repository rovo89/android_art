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
.class public LTrivialHelloWorld;
.super Ljava/lang/Object;

.method public constructor <init>()V
.registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method

.method public static run()V
.registers 8
    # Trivial 0-arg hello world
    create-lambda v0, LTrivialHelloWorld;->doHelloWorld(J)V
    # TODO: create-lambda should not write to both v0 and v1
    invoke-lambda v0, {}

    # Slightly more interesting 4-arg hello world
    create-lambda v2, doHelloWorldArgs(JLjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V
    # TODO: create-lambda should not write to both v2 and v3
    const-string v4, "A"
    const-string v5, "B"
    const-string v6, "C"
    const-string v7, "D"
    invoke-lambda v2, {v4, v5, v6, v7}

    invoke-static {}, LTrivialHelloWorld;->testFailures()V

    return-void
.end method

#TODO: should use a closure type instead of jlong. 
.method public static doHelloWorld(J)V
    .registers 5 # 1 wide parameters, 3 locals

    const-string v0, "Hello world! (0-args, no closure)"

    sget-object v1, Ljava/lang/System;->out:Ljava/io/PrintStream;
    invoke-virtual {v1, v0}, Ljava/io/PrintStream;->println(Ljava/lang/String;)V

    return-void
.end method

#TODO: should use a closure type instead of jlong. 
.method public static doHelloWorldArgs(JLjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V
    .registers 9 # 1 wide parameter, 4 narrow parameters, 3 locals

    const-string v0, " Hello world! (4-args, no closure)"
    sget-object v1, Ljava/lang/System;->out:Ljava/io/PrintStream;

    invoke-virtual {v1, p2}, Ljava/io/PrintStream;->print(Ljava/lang/String;)V
    invoke-virtual {v1, p3}, Ljava/io/PrintStream;->print(Ljava/lang/String;)V
    invoke-virtual {v1, p4}, Ljava/io/PrintStream;->print(Ljava/lang/String;)V
    invoke-virtual {v1, p5}, Ljava/io/PrintStream;->print(Ljava/lang/String;)V

    invoke-virtual {v1, v0}, Ljava/io/PrintStream;->println(Ljava/lang/String;)V

    return-void
.end method

# Test exceptions are thrown as expected when used opcodes incorrectly
.method private static testFailures()V
    .registers 4 # 0 parameters, 4 locals

    const v0, 0  # v0 = null
    const v1, 0  # v1 = null
:start
    invoke-lambda v0, {}  # invoking a null lambda shall raise an NPE
:end
    return-void

:handler
    const-string v2, "Caught NPE"
    sget-object v3, Ljava/lang/System;->out:Ljava/io/PrintStream;
    invoke-virtual {v3, v2}, Ljava/io/PrintStream;->println(Ljava/lang/String;)V

    return-void

    .catch Ljava/lang/NullPointerException; {:start .. :end} :handler
.end method
