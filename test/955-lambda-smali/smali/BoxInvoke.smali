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
.class public LBoxInvoke;
.super Ljava/lang/Object;

.method public constructor <init>()V
.registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method

.method public static run()V
    .registers 0

    invoke-static {}, LBoxInvoke;->testBoxInvoke()V
    invoke-static {}, LBoxInvoke;->forceGC()V

    return-void
.end method

# Test that invoke-virtual works on boxed innate lambdas.
.method public static testBoxInvoke()V
    .registers 100

    # Try invoking 0-arg void return lambda
    create-lambda v0, LBoxInvoke;->doHelloWorld0(J)V
    const-string v2, "Ljava/lang/Runnable;"
    box-lambda v2, v0 # Ljava/lang/Runnable;
    invoke-interface {v2}, Ljava/lang/Runnable;->run()V

    # Try invoking 1-arg int return lambda
    create-lambda v3, LBoxInvoke;->doHelloWorld1(JLjava/lang/Object;)I
    const-string v5, "Ljava/lang/Comparable;"
    box-lambda v5, v3 # Ljava/lang/Comparable;
    const-string v6, "Hello boxing world!"
    invoke-interface {v5, v6}, Ljava/lang/Comparable;->compareTo(Ljava/lang/Object;)I
    move-result v7
    sget-object v8, Ljava/lang/System;->out:Ljava/io/PrintStream;
    invoke-virtual {v8, v7}, Ljava/io/PrintStream;->println(I)V

    return-void

    # TODO: more tests once box-lambda can take a type descriptor.

.end method

#TODO: should use a closure type instead of a long.
.method public static doHelloWorld0(J)V
    .registers 4 # 1 wide parameters, 2 locals

    const-string v0, "(BoxInvoke) Hello boxing world! (0-args, no closure) void"

    sget-object v1, Ljava/lang/System;->out:Ljava/io/PrintStream;
    invoke-virtual {v1, v0}, Ljava/io/PrintStream;->println(Ljava/lang/String;)V

    return-void
.end method

#TODO: should use a closure type instead of a long.
.method public static doHelloWorld1(JLjava/lang/Object;)I
    # J = closure, L = obj, I = return type
    .registers 6 # 1 wide parameters, 1 narrow parameter, 3 locals

    # Prints "<before> $parameter1(Object) <after>:" without the line terminator. 

    const-string v0, "(BoxInvoke) "

    sget-object v1, Ljava/lang/System;->out:Ljava/io/PrintStream;
    # System.out.print("<before>");
    invoke-virtual {v1, v0}, Ljava/io/PrintStream;->print(Ljava/lang/String;)V

    # System.out.print(obj);
    invoke-virtual {v1, p2}, Ljava/io/PrintStream;->print(Ljava/lang/Object;)V

    # System.out.print("<after>: ");
    const-string v0, "(1-args, no closure) returned: "
    invoke-virtual {v1, v0}, Ljava/io/PrintStream;->print(Ljava/lang/String;)V

    const v2, 12345678
    return v2
.end method

# Force a GC. Used to ensure our weak reference table of boxed lambdas is getting swept.
.method private static forceGC()V
    .registers 1
    invoke-static {}, Ljava/lang/Runtime;->getRuntime()Ljava/lang/Runtime;
    move-result-object v0
    invoke-virtual {v0}, Ljava/lang/Runtime;->gc()V

    return-void
.end method
