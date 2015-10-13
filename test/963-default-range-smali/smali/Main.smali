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
# class Main {
#   public static void main(String[] args) {
#       A a = new A();
#       System.out.println(a.SayHi("a string 0",
#                                  "a string 1",
#                                  "a string 2",
#                                  "a string 3",
#                                  "a string 4",
#                                  "a string 5",
#                                  "a string 6",
#                                  "a string 7",
#                                  "a string 8",
#                                  "a string 9"));
#       iface b = (iface)a;
#       System.out.println(b.SayHi("a string 0",
#                                  "a string 1",
#                                  "a string 2",
#                                  "a string 3",
#                                  "a string 4",
#                                  "a string 5",
#                                  "a string 6",
#                                  "a string 7",
#                                  "a string 8",
#                                  "a string 9"));
#   }
# }
.class public LMain;
.super Ljava/lang/Object;

.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, Ljava/lang/Object;-><init>()V
    return-void
.end method

.method public static main([Ljava/lang/String;)V
    .locals 15
    sget-object v12, Ljava/lang/System;->out:Ljava/io/PrintStream;

    new-instance v1, LA;
    invoke-direct {v1}, LA;-><init>()V
    const-string v2, "a string 0"
    const-string v3, "a string 1"
    const-string v4, "a string 2"
    const-string v5, "a string 3"
    const-string v6, "a string 4"
    const-string v7, "a string 5"
    const-string v8, "a string 6"
    const-string v9, "a string 7"
    const-string v10, "a string 8"
    const-string v11, "a string 9"
    invoke-virtual/range {v1 .. v11}, LA;->SayHi(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;
    move-result-object v0
    invoke-virtual {v12,v0}, Ljava/io/PrintStream;->println(Ljava/lang/Object;)V

    invoke-interface/range {v1 .. v11}, Liface;->SayHi(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;
    move-result-object v0
    invoke-virtual {v12,v0}, Ljava/io/PrintStream;->println(Ljava/lang/Object;)V

    return-void
.end method
