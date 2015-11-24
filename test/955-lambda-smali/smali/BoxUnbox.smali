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
    .registers 0

    invoke-static {}, LBoxUnbox;->testBox()V
    invoke-static {}, LBoxUnbox;->testBoxEquality()V
    invoke-static {}, LBoxUnbox;->testFailures()V
    invoke-static {}, LBoxUnbox;->testFailures2()V
    invoke-static {}, LBoxUnbox;->testFailures3()V
    invoke-static {}, LBoxUnbox;->forceGC()V

    return-void
.end method

#TODO: should use a closure type instead of ArtMethod.
.method public static doHelloWorld(J)V
    .registers 4 # 1 wide parameters, 2 locals

    const-string v0, "(BoxUnbox) Hello boxing world! (0-args, no closure)"

    sget-object v1, Ljava/lang/System;->out:Ljava/io/PrintStream;
    invoke-virtual {v1, v0}, Ljava/io/PrintStream;->println(Ljava/lang/String;)V

    return-void
.end method

# Test boxing and unboxing; the same lambda should be invoked as if there was no box.
.method private static testBox()V
    .registers 3

    create-lambda v0, LBoxUnbox;->doHelloWorld(J)V
    box-lambda v2, v0 # v2 = box(v0)
    unbox-lambda v0, v2, J # v0 = unbox(v2)
    invoke-lambda v0, {}

    return-void
.end method

# Test that boxing the same lambda twice yield the same object.
.method private static testBoxEquality()V
   .registers 6 # 0 parameters, 6 locals

    create-lambda v0, LBoxUnbox;->doHelloWorld(J)V
    box-lambda v2, v0 # v2 = box(v0)
    box-lambda v3, v0 # v3 = box(v0)

    # The objects should be not-null, and they should have the same reference
    if-eqz v2, :is_zero
    if-ne v2, v3, :is_not_equal

    const-string v4, "(BoxUnbox) Boxing repeatedly yields referentially-equal objects"
    goto :end

:is_zero
    const-string v4, "(BoxUnbox) Boxing repeatedly FAILED: boxing returned null"
    goto :end

:is_not_equal
    const-string v4, "(BoxUnbox) Boxing repeatedly FAILED: objects were not same reference"
    goto :end

:end
    sget-object v5, Ljava/lang/System;->out:Ljava/io/PrintStream;
    invoke-virtual {v5, v4}, Ljava/io/PrintStream;->println(Ljava/lang/String;)V
    return-void
.end method

# Test exceptions are thrown as expected when used opcodes incorrectly
.method private static testFailures()V
    .registers 4 # 0 parameters, 4 locals

    const v0, 0  # v0 = null
    const v1, 0  # v1 = null
:start
    unbox-lambda v2, v0, J
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
    unbox-lambda v2, v0, J
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


# Force a GC. Used to ensure our weak reference table of boxed lambdas is getting swept.
.method private static forceGC()V
    .registers 1
    invoke-static {}, Ljava/lang/Runtime;->getRuntime()Ljava/lang/Runtime;
    move-result-object v0
    invoke-virtual {v0}, Ljava/lang/Runtime;->gc()V

    return-void
.end method
