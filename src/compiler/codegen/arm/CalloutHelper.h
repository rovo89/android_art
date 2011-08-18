/*
 * Copyright (C) 2011 The Android Open Source Project
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

#ifndef ART_SRC_COMPILER_CODEGEN_ARM_CALLOUT_HELPER_H_
#define ART_SRC_COMPILER_CODEGEN_ARM_CALLOUT_HELPER_H_

#include "../../Dalvik.h"

/* Helper functions used at runtime by compiled code */

/* Conversions */
extern "C" float __aeabi_i2f(int op1);             // OP_INT_TO_FLOAT
extern "C" int __aeabi_f2iz(float op1);            // OP_FLOAT_TO_INT
extern "C" float __aeabi_d2f(double op1);          // OP_DOUBLE_TO_FLOAT
extern "C" double __aeabi_f2d(float op1);          // OP_FLOAT_TO_DOUBLE
extern "C" double __aeabi_i2d(int op1);            // OP_INT_TO_DOUBLE
extern "C" int __aeabi_d2iz(double op1);           // OP_DOUBLE_TO_INT
extern "C" float __aeabi_l2f(long op1);            // OP_LONG_TO_FLOAT
extern "C" double __aeabi_l2d(long op1);           // OP_LONG_TO_DOUBLE
s8 artF2L(float op1);                // OP_FLOAT_TO_LONG
s8 artD2L(double op1);               // OP_DOUBLE_TO_LONG

/* Single-precision FP arithmetics */
extern "C" float __aeabi_fadd(float a, float b);   // OP_ADD_FLOAT[_2ADDR]
extern "C" float __aeabi_fsub(float a, float b);   // OP_SUB_FLOAT[_2ADDR]
extern "C" float __aeabi_fdiv(float a, float b);   // OP_DIV_FLOAT[_2ADDR]
extern "C" float __aeabi_fmul(float a, float b);   // OP_MUL_FLOAT[_2ADDR]
extern "C" float fmodf(float a, float b);          // OP_REM_FLOAT[_2ADDR]

/* Double-precision FP arithmetics */
extern "C" double __aeabi_dadd(double a, double b); // OP_ADD_DOUBLE[_2ADDR]
extern "C" double __aeabi_dsub(double a, double b); // OP_SUB_DOUBLE[_2ADDR]
extern "C" double __aeabi_ddiv(double a, double b); // OP_DIV_DOUBLE[_2ADDR]
extern "C" double __aeabi_dmul(double a, double b); // OP_MUL_DOUBLE[_2ADDR]
extern "C" double fmod(double a, double b);         // OP_REM_DOUBLE[_2ADDR]

/* Integer arithmetics */
extern "C" int __aeabi_idivmod(int op1, int op2);  // OP_REM_INT[_2ADDR|_LIT8|_LIT16]
extern "C" int __aeabi_idiv(int op1, int op2);     // OP_DIV_INT[_2ADDR|_LIT8|_LIT16]

/* Long long arithmetics - OP_REM_LONG[_2ADDR] & OP_DIV_LONG[_2ADDR] */
extern "C" long long __aeabi_ldivmod(long long op1, long long op2);

/* Originally declared in Sync.h */
bool dvmUnlockObject(struct Thread* self, struct Object* obj); //OP_MONITOR_EXIT
void dvmLockObject(struct Thread* self, struct Object* obj); //OP_MONITOR_ENTER

/* Originally declared in oo/TypeCheck.h */
bool dvmCanPutArrayElement(const ClassObject* elemClass,   // OP_APUT_OBJECT
                           const ClassObject* arrayClass);
int dvmInstanceofNonTrivial(const ClassObject* instance,   // OP_CHECK_CAST &&
                            const ClassObject* clazz);     // OP_INSTANCE_OF

/* Originally declared in oo/Array.h */
ArrayObject* dvmAllocArrayByClass(ClassObject* arrayClass, // OP_NEW_ARRAY
                                  size_t length, int allocFlags);
/* Originally declared in alloc/Alloc.h */
Object* dvmAllocObject(ClassObject* clazz, int flags);  // OP_NEW_INSTANCE

/*
 * The following functions are invoked through the compiler templates (declared
 * in compiler/template/armv5te/footer.S:
 *
 *      __aeabi_cdcmple         // CMPG_DOUBLE
 *      __aeabi_cfcmple         // CMPG_FLOAT
 *      dvmLockObject           // MONITOR_ENTER
 */

/* from mterp/common/FindInterface.h */
Method* dvmFindInterfaceMethodInCache(ClassObject* thisClass,
    u4 methodIdx, const Method* method, DvmDex* methodClassDex);

/* from interp/Interp.cpp */
bool dvmInterpHandleFillArrayData(ArrayObject* arrayObj, const u2* arrayData);

#endif  // ART_SRC_COMPILER_CODEGEN_ARM_CALLOUT_HELPER_H_
