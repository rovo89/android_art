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
# public interface iface {
#   public static final Displayer f = new Displayer();
#
#   public static String SayHi() {
#       return "Hello";
#   }
# }

.class public abstract interface Liface;
.super Ljava/lang/Object;

.field public final static f:LDisplayer;

.method static constructor <clinit>()V
    .locals 3
    new-instance v1, LDisplayer;
    invoke-direct {v1}, LDisplayer;-><init>()V
    sput-object v1, Liface;->f:LDisplayer;
    return-void
.end method

.method public static SayHi()Ljava/lang/String;
    .locals 1
    const-string v0, "Hello"
    return-object v0
.end method

