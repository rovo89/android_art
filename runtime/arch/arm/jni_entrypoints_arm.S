/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "asm_support_arm.S"

    /*
     * Jni dlsym lookup stub.
     */
    .extern artFindNativeMethod
ENTRY art_jni_dlsym_lookup_stub
    push   {r0, r1, r2, r3, lr}           @ spill regs
    .save  {r0, r1, r2, r3, lr}
    .pad #20
    .cfi_adjust_cfa_offset 20
    sub    sp, #12                        @ pad stack pointer to align frame
    .pad #12
    .cfi_adjust_cfa_offset 12
    blx    artFindNativeMethod
    mov    r12, r0                        @ save result in r12
    add    sp, #12                        @ restore stack pointer
    .cfi_adjust_cfa_offset -12
    cbz    r0, 1f                         @ is method code null?
    pop    {r0, r1, r2, r3, lr}           @ restore regs
    .cfi_adjust_cfa_offset -20
    bx     r12                            @ if non-null, tail call to method's code
1:
    .cfi_adjust_cfa_offset 20
    pop    {r0, r1, r2, r3, pc}           @ restore regs and return to caller to handle exception
    .cfi_adjust_cfa_offset -20
END art_jni_dlsym_lookup_stub
