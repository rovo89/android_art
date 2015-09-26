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
.class public LMain;

.super Ljava/lang/Object;

.method public static main([Ljava/lang/String;)V
    .registers 2

    invoke-static {}, LSanityCheck;->run()I
    invoke-static {}, LTrivialHelloWorld;->run()V
    invoke-static {}, LBoxUnbox;->run()V
    invoke-static {}, LMoveResult;->run()V
    invoke-static {}, LCaptureVariables;->run()V

# TODO: add tests when verification fails

    return-void
.end method
