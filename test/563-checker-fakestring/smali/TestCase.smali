# Copyright (C) 2016 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

.class public LTestCase;
.super Ljava/lang/Object;

# Test that all vregs holding the new-instance are updated after the
# StringFactory call.

## CHECK-START: java.lang.String TestCase.vregAliasing(byte[]) register (after)
## CHECK-DAG:                Return [<<String:l\d+>>]
## CHECK-DAG:     <<String>> InvokeStaticOrDirect  method_name:java.lang.String.<init>

.method public static vregAliasing([B)Ljava/lang/String;
   .registers 5

   # Create new instance of String and store it to v0, v1, v2.
   new-instance v0, Ljava/lang/String;
   move-object v1, v0
   move-object v2, v0

   # Call String.<init> on v1.
   const-string v3, "UTF8"
   invoke-direct {v1, p0, v3}, Ljava/lang/String;-><init>([BLjava/lang/String;)V

   # Return the object from v2.
   return-object v2

.end method

# Test usage of String new-instance before it is initialized.

## CHECK-START: void TestCase.compareNewInstance() register (after)
## CHECK-DAG:     <<Null:l\d+>>   NullConstant
## CHECK-DAG:     <<String:l\d+>> NewInstance
## CHECK-DAG:     <<Cond:z\d+>>   NotEqual [<<String>>,<<Null>>]
## CHECK-DAG:                     If [<<Cond>>]

.method public static compareNewInstance()V
   .registers 3

   new-instance v0, Ljava/lang/String;
   if-nez v0, :return

   # Will throw NullPointerException if this branch is taken.
   const v1, 0x0
   const-string v2, "UTF8"
   invoke-direct {v0, v1, v2}, Ljava/lang/String;-><init>([BLjava/lang/String;)V
   return-void

   :return
   return-void

.end method

# Test deoptimization between String's allocation and initialization.

## CHECK-START: int TestCase.deoptimizeNewInstance(int[], byte[]) register (after)
## CHECK:         <<String:l\d+>> NewInstance
## CHECK:                         Deoptimize env:[[<<String>>,{{.*]]}}
## CHECK:                         InvokeStaticOrDirect method_name:java.lang.String.<init>

.method public static deoptimizeNewInstance([I[B)I
   .registers 6

   const v2, 0x0
   const v1, 0x1

   new-instance v0, Ljava/lang/String;

   # Deoptimize here if the array is too short.
   aget v1, p0, v1
   add-int/2addr v2, v1

   # Check that we're being executed by the interpreter.
   invoke-static {}, LMain;->assertIsInterpreted()V

   # String allocation should succeed.
   const-string v3, "UTF8"
   invoke-direct {v0, p1, v3}, Ljava/lang/String;-><init>([BLjava/lang/String;)V

   # This ArrayGet will throw ArrayIndexOutOfBoundsException.
   const v1, 0x4
   aget v1, p0, v1
   add-int/2addr v2, v1

   return v2

.end method

# Test throwing and catching an exception between String's allocation and initialization.

## CHECK-START: void TestCase.catchNewInstance() register (after)
## CHECK-DAG:     <<Null:l\d+>>   NullConstant
## CHECK-DAG:     <<Zero:i\d+>>   IntConstant 0
## CHECK-DAG:     <<String:l\d+>> NewInstance
## CHECK-DAG:     <<UTF8:l\d+>>   LoadString
## CHECK-DAG:                     InvokeStaticOrDirect [<<Null>>,<<UTF8>>] env:[[<<String>>,<<Zero>>,<<UTF8>>]]
## CHECK-DAG:                     Phi [<<Null>>,<<Null>>,<<String>>] reg:0 is_catch_phi:true

.method public static catchNewInstance()V
   .registers 3

   const v0, 0x0
   :try_start
   new-instance v0, Ljava/lang/String;

   # Calling String.<init> on null byte array will throw NullPointerException
   # with v0 = new-instance.
   const v1, 0x0
   const-string v2, "UTF8"
   invoke-direct {v0, v1, v2}, Ljava/lang/String;-><init>([BLjava/lang/String;)V
   :try_end
   .catchall {:try_start .. :try_end} :catch
   return-void

   # Catch exception and test v0. Do not throw if it is not null.
   :catch
   move-exception v1
   if-nez v0, :return
   throw v1

   :return
   return-void

.end method
