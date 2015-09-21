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
# public class F extends E {
#   public String SaySurprisedHi() {
#       return super.SayHi() + "!";
#   }
# }

.class public LF;
.super LE;

.method public constructor <init>()V
    .registers 1
    invoke-direct {p0}, LE;-><init>()V
    return-void
.end method

.method public SaySurprisedHi()Ljava/lang/String;
    .registers 2
    invoke-super {p0}, LE;->SayHi()Ljava/lang/String;
    move-result-object v0
    const-string v1, "!"
    invoke-virtual {v0, v1}, Ljava/lang/String;->concat(Ljava/lang/String;)Ljava/lang/String;
    move-result-object v0
    return-object v0
.end method
