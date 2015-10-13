# /*
#  * Copyright (C) 2015 The Android Open Source Project
#  *
#  * Licensed under the Apache License, Version 2.0 (the "License");
#  * you may not use this file except in compliance with the License.
#  * You may obtain a copy of the License at
#  *
#  *      http://www.apache.org/licenses/LICENSE-2.0
#  *
#  * Unless required by applicable law or agreed to in writing, software
#  * distributed under the License is distributed on an "AS IS" BASIS,
#  * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#  * See the License for the specific language governing permissions and
#  * limitations under the License.
#  */
#
# public class Displayer {
#   static {
#       System.out.println("init");
#   }
#
#   public Displayer() {
#       System.out.println("constructor");
#   }
# }

.class public LDisplayer;
.super Ljava/lang/Object;

.method public static <clinit>()V
    .locals 3
    sget-object v1, Ljava/lang/System;->out:Ljava/io/PrintStream;
    const-string v0, "init"
    invoke-virtual {v1,v0}, Ljava/io/PrintStream;->println(Ljava/lang/Object;)V
    return-void
.end method

.method public constructor <init>()V
    .locals 2
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    sget-object v1, Ljava/lang/System;->out:Ljava/io/PrintStream;
    const-string v0, "constructor"
    invoke-virtual {v1,v0}, Ljava/io/PrintStream;->println(Ljava/lang/Object;)V
    return-void
.end method
