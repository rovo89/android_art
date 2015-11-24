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
.class public LCaptureVariables;
.super Ljava/lang/Object;

.method public constructor <init>()V
.registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method

.method public static run()V
.registers 8
    # Test boolean capture
    const v2, 1           # v2 = true
    capture-variable v2, "Z"
    create-lambda v0, LCaptureVariables;->printCapturedVariable_Z(J)V
    # TODO: create-lambda should not write to both v0 and v1
    invoke-lambda v0, {}

    # Test byte capture
    const v2, 82       # v2 = 82, 'R'
    capture-variable v2, "B"
    create-lambda v0, LCaptureVariables;->printCapturedVariable_B(J)V
    # TODO: create-lambda should not write to both v0 and v1
    invoke-lambda v0, {}

    # Test char capture
    const v2, 0x2202       # v2 = 0x2202, '∂'
    capture-variable v2, "C"
    create-lambda v0, LCaptureVariables;->printCapturedVariable_C(J)V
    # TODO: create-lambda should not write to both v0 and v1
    invoke-lambda v0, {}

    # Test short capture
    const v2, 1000 # v2 = 1000
    capture-variable v2, "S"
    create-lambda v0, LCaptureVariables;->printCapturedVariable_S(J)V
    # TODO: create-lambda should not write to both v0 and v1
    invoke-lambda v0, {}

    # Test int capture
    const v2, 12345678
    capture-variable v2, "I"
    create-lambda v0, LCaptureVariables;->printCapturedVariable_I(J)V
    # TODO: create-lambda should not write to both v0 and v1
    invoke-lambda v0, {}

    # Test long capture
    const-wide v2, 0x0badf00dc0ffeeL # v2 = 3287471278325742
    capture-variable v2, "J"
    create-lambda v0, LCaptureVariables;->printCapturedVariable_J(J)V
    # TODO: create-lambda should not write to both v0 and v1
    invoke-lambda v0, {}

    # Test float capture
    const v2, infinityf
    capture-variable v2, "F"
    create-lambda v0, LCaptureVariables;->printCapturedVariable_F(J)V
    # TODO: create-lambda should not write to both v0 and v1
    invoke-lambda v0, {}

    # Test double capture
    const-wide v2, -infinity
    capture-variable v2, "D"
    create-lambda v0, LCaptureVariables;->printCapturedVariable_D(J)V
    # TODO: create-lambda should not write to both v0 and v1
    invoke-lambda v0, {}

    #TODO: capture objects and lambdas once we have support for it

    # Test capturing multiple variables
    invoke-static {}, LCaptureVariables;->testMultipleCaptures()V

    # Test failures
    invoke-static {}, LCaptureVariables;->testFailures()V

    return-void
.end method

#TODO: should use a closure type instead of a long
.method public static printCapturedVariable_Z(J)V
    .registers 5 # 1 wide parameter, 3 locals

    const-string v0, "(CaptureVariables) (0-args, 1 captured variable 'Z'): value is "

    sget-object v1, Ljava/lang/System;->out:Ljava/io/PrintStream;
    invoke-virtual {v1, v0}, Ljava/io/PrintStream;->print(Ljava/lang/String;)V

    liberate-variable v2, p0, "Z"
    invoke-virtual {v1, v2}, Ljava/io/PrintStream;->println(Z)V

    return-void
.end method

#TODO: should use a closure type instead of a long
.method public static printCapturedVariable_B(J)V
    .registers 5 # 1 wide parameter, 3 locals

    const-string v0, "(CaptureVariables) (0-args, 1 captured variable 'B'): value is "

    sget-object v1, Ljava/lang/System;->out:Ljava/io/PrintStream;
    invoke-virtual {v1, v0}, Ljava/io/PrintStream;->print(Ljava/lang/String;)V

    liberate-variable v2, p0, "B"
    invoke-virtual {v1, v2}, Ljava/io/PrintStream;->println(C)V  # no println(B), use char instead.

    return-void
.end method

#TODO: should use a closure type instead of a long
.method public static printCapturedVariable_C(J)V
    .registers 5 # 1 wide parameter, 3 locals

    const-string v0, "(CaptureVariables) (0-args, 1 captured variable 'C'): value is "

    sget-object v1, Ljava/lang/System;->out:Ljava/io/PrintStream;
    invoke-virtual {v1, v0}, Ljava/io/PrintStream;->print(Ljava/lang/String;)V

    liberate-variable v2, p0, "C"
    invoke-virtual {v1, v2}, Ljava/io/PrintStream;->println(C)V

    return-void
.end method

#TODO: should use a closure type instead of a long
.method public static printCapturedVariable_S(J)V
    .registers 5 # 1 wide parameter, 3 locals

    const-string v0, "(CaptureVariables) (0-args, 1 captured variable 'S'): value is "

    sget-object v1, Ljava/lang/System;->out:Ljava/io/PrintStream;
    invoke-virtual {v1, v0}, Ljava/io/PrintStream;->print(Ljava/lang/String;)V

    liberate-variable v2, p0, "S"
    invoke-virtual {v1, v2}, Ljava/io/PrintStream;->println(I)V  # no println(S), use int instead

    return-void
.end method

#TODO: should use a closure type instead of a long
.method public static printCapturedVariable_I(J)V
    .registers 5 # 1 wide parameter, 3 locals

    const-string v0, "(CaptureVariables) (0-args, 1 captured variable 'I'): value is "

    sget-object v1, Ljava/lang/System;->out:Ljava/io/PrintStream;
    invoke-virtual {v1, v0}, Ljava/io/PrintStream;->print(Ljava/lang/String;)V

    liberate-variable v2, p0, "I"
    invoke-virtual {v1, v2}, Ljava/io/PrintStream;->println(I)V

    return-void
.end method

#TODO: should use a closure type instead of a long
.method public static printCapturedVariable_J(J)V
    .registers 6 # 1 wide parameter, 4 locals

    const-string v0, "(CaptureVariables) (0-args, 1 captured variable 'J'): value is "

    sget-object v1, Ljava/lang/System;->out:Ljava/io/PrintStream;
    invoke-virtual {v1, v0}, Ljava/io/PrintStream;->print(Ljava/lang/String;)V

    liberate-variable v2, p0, "J"
    invoke-virtual {v1, v2, v3}, Ljava/io/PrintStream;->println(J)V

    return-void
.end method

#TODO: should use a closure type instead of a long
.method public static printCapturedVariable_F(J)V
    .registers 5 # 1 parameter, 4 locals

    const-string v0, "(CaptureVariables) (0-args, 1 captured variable 'F'): value is "

    sget-object v1, Ljava/lang/System;->out:Ljava/io/PrintStream;
    invoke-virtual {v1, v0}, Ljava/io/PrintStream;->print(Ljava/lang/String;)V

    liberate-variable v2, p0, "F"
    invoke-virtual {v1, v2}, Ljava/io/PrintStream;->println(F)V

    return-void
.end method

#TODO: should use a closure type instead of a long
.method public static printCapturedVariable_D(J)V
    .registers 6 # 1 wide parameter, 4 locals

    const-string v0, "(CaptureVariables) (0-args, 1 captured variable 'D'): value is "

    sget-object v1, Ljava/lang/System;->out:Ljava/io/PrintStream;
    invoke-virtual {v1, v0}, Ljava/io/PrintStream;->print(Ljava/lang/String;)V

    liberate-variable v2, p0, "D"
    invoke-virtual {v1, v2, v3}, Ljava/io/PrintStream;->println(D)V

    return-void
.end method

# Test capturing more than one variable.
.method private static testMultipleCaptures()V
    .registers 4 # 0 parameters, 4 locals

    const v2, 1           # v2 = true
    capture-variable v2, "Z"

    const v2, 82       # v2 = 82, 'R'
    capture-variable v2, "B"

    const v2, 0x2202       # v2 = 0x2202, '∂'
    capture-variable v2, "C"

    const v2, 1000 # v2 = 1000
    capture-variable v2, "S"

    const v2, 12345678
    capture-variable v2, "I"

    const-wide v2, 0x0badf00dc0ffeeL # v2 = 3287471278325742
    capture-variable v2, "J"

    const v2, infinityf
    capture-variable v2, "F"

    const-wide v2, -infinity
    capture-variable v2, "D"

    create-lambda v0, LCaptureVariables;->printCapturedVariable_ZBCSIJFD(J)V
    # TODO: create-lambda should not write to both v0 and v1
    invoke-lambda v0, {}

.end method

#TODO: should use a closure type instead of a long
.method public static printCapturedVariable_ZBCSIJFD(J)V
    .registers 7 # 1 wide parameter, 5 locals

    const-string v0, "(CaptureVariables) (0-args, 8 captured variable 'ZBCSIJFD'): value is "
    const-string v4, ","

    sget-object v1, Ljava/lang/System;->out:Ljava/io/PrintStream;
    invoke-virtual {v1, v0}, Ljava/io/PrintStream;->print(Ljava/lang/String;)V

    liberate-variable v2, p0, "Z"
    invoke-virtual {v1, v2}, Ljava/io/PrintStream;->print(Z)V
    invoke-virtual {v1, v4}, Ljava/io/PrintStream;->print(Ljava/lang/String;)V

    liberate-variable v2, p0, "B"
    invoke-virtual {v1, v2}, Ljava/io/PrintStream;->print(C)V
    invoke-virtual {v1, v4}, Ljava/io/PrintStream;->print(Ljava/lang/String;)V

    liberate-variable v2, p0, "C"
    invoke-virtual {v1, v2}, Ljava/io/PrintStream;->print(C)V
    invoke-virtual {v1, v4}, Ljava/io/PrintStream;->print(Ljava/lang/String;)V

    liberate-variable v2, p0, "S"
    invoke-virtual {v1, v2}, Ljava/io/PrintStream;->print(I)V
    invoke-virtual {v1, v4}, Ljava/io/PrintStream;->print(Ljava/lang/String;)V

    liberate-variable v2, p0, "I"
    invoke-virtual {v1, v2}, Ljava/io/PrintStream;->print(I)V
    invoke-virtual {v1, v4}, Ljava/io/PrintStream;->print(Ljava/lang/String;)V

    liberate-variable v2, p0, "J"
    invoke-virtual {v1, v2, v3}, Ljava/io/PrintStream;->print(J)V
    invoke-virtual {v1, v4}, Ljava/io/PrintStream;->print(Ljava/lang/String;)V

    liberate-variable v2, p0, "F"
    invoke-virtual {v1, v2}, Ljava/io/PrintStream;->print(F)V
    invoke-virtual {v1, v4}, Ljava/io/PrintStream;->print(Ljava/lang/String;)V

    liberate-variable v2, p0, "D"
    invoke-virtual {v1, v2, v3}, Ljava/io/PrintStream;->println(D)V

    return-void
.end method

# Test exceptions are thrown as expected when used opcodes incorrectly
.method private static testFailures()V
    .registers 4 # 0 parameters, 4 locals

    const v0, 0  # v0 = null
    const v1, 0  # v1 = null
:start
    liberate-variable v0, v2, "Z" # invoking a null lambda shall raise an NPE
:end
    return-void

:handler
    const-string v2, "(CaptureVariables) Caught NPE"
    sget-object v3, Ljava/lang/System;->out:Ljava/io/PrintStream;
    invoke-virtual {v3, v2}, Ljava/io/PrintStream;->println(Ljava/lang/String;)V

    return-void

    .catch Ljava/lang/NullPointerException; {:start .. :end} :handler
.end method
