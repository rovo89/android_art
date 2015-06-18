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
.class public LSanityCheck;
.super Ljava/lang/Object;


.method public constructor <init>()V
.registers 1
   invoke-direct {p0}, Ljava/lang/Object;-><init>()V
   return-void
.end method

# This test is just here to make sure that we can at least execute basic non-lambda
# functionality such as printing (when lambdas are enabled in the runtime).
.method public static run()I
# Don't use too many registers here to avoid hitting the Stack::SanityCheck frame<2KB assert
.registers 3
    const-string v0, "SanityCheck"
    sget-object v1, Ljava/lang/System;->out:Ljava/io/PrintStream;
    invoke-virtual {v1, v0}, Ljava/io/PrintStream;->println(Ljava/lang/String;)V
    const v2, 123456
    return v2
.end method
