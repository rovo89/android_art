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

#ifndef ART_SRC_COMPILER_LLVM_COMPILER_RUNTIME_FUNC_LIST_H_
#define ART_SRC_COMPILER_LLVM_COMPILER_RUNTIME_FUNC_LIST_H_

// NOTE: COMPILER_RUNTIME_FUNC_LIST_* should be sorted!

#define COMPILER_RUNTIME_FUNC_LIST_X86(V) \
  V(__ashldi3,          long long, long long, int) \
  V(__ashrdi3,          long long, long long, int) \
  V(__divdi3,           long long, long long, long long) \
  V(__fixdfdi,          long long, double) \
  V(__fixsfdi,          long long, float) \
  V(__fixtfdi,          long long, long double) \
  V(__fixtfsi,          int, long double) \
  V(__fixunsdfdi,       unsigned long long, double) \
  V(__fixunsdfsi,       unsigned int, double) \
  V(__fixunssfdi,       unsigned long long, float) \
  V(__fixunssfsi,       unsigned int, float) \
  V(__fixunstfdi,       unsigned long long, long double) \
  V(__fixunstfsi,       unsigned int, long double) \
  V(__fixunsxfdi,       unsigned long long, long double) \
  V(__fixunsxfsi,       unsigned int, long double) \
  V(__fixxfdi,          long long, long double) \
  V(__floatdidf,        double, long long) \
  V(__floatdisf,        float, long long) \
  V(__floatditf,        long double, long long) \
  V(__floatdixf,        long double, long long) \
  V(__floatsitf,        long double, int) \
  V(__floatundidf,      double, unsigned long long) \
  V(__floatundisf,      float, unsigned long long) \
  V(__floatunditf,      long double, unsigned long long) \
  V(__floatundixf,      long double, unsigned long long) \
  V(__floatunsitf,      long double, int) \
  V(__lshrdi3,          long long, long long, int) \
  V(__moddi3,           long long, long long, long long) \
  V(__muldi3,           long long, long long, long long) \
  V(__negdi2,           long long, long long) \
  V(__powidf2,          double, double, int) \
  V(__powisf2,          float, float, int) \
  V(__powitf2,          long double, long double, int) \
  V(__powixf2,          long double, long double, int) \
  V(__trunctfdf2,       double, long double) \
  V(__trunctfsf2,       float, long double) \
  V(__udivdi3,          unsigned long long, unsigned long long, unsigned long long) \
  V(__umoddi3,          unsigned long long, unsigned long long, unsigned long long) \
  V(ceil,               double, double) \
  V(ceilf,              float, float) \
  V(ceill,              long double, long double) \
  V(copysign,           double, double, double) \
  V(copysignf,          float, float, float) \
  V(copysignl,          long double, long double, long double) \
  V(cos,                double, double) \
  V(cosf,               float, float) \
  V(exp,                double, double) \
  V(exp2,               double, double) \
  V(exp2f,              float, float) \
  V(expf,               float, float) \
  V(floor,              double, double) \
  V(floorf,             float, float) \
  V(floorl,             long double, long double) \
  V(fma,                double, double, double, double) \
  V(fmaf,               float, float, float, float) \
  V(fmod,               double, double, double) \
  V(fmodf,              float, float, float) \
  V(log,                double, double) \
  V(log10,              double, double) \
  V(log10f,             float, float) \
  V(logf,               float, float) \
  V(memcpy,             void *, void *, const void *, size_t) \
  V(memmove,            void *, void *, const void *, size_t) \
  V(memset,             void *, void *, int, size_t) \
  V(nearbyint,          double, double) \
  V(nearbyintf,         float, float) \
  V(pow,                double, double, double) \
  V(powf,               float, float, float) \
  V(rint,               double, double) \
  V(rintf,              float, float) \
  V(sin,                double, double) \
  V(sinf,               float, float) \
  V(sqrt,               double, double) \
  V(sqrtf,              float, float) \
  V(trunc,              double, double) \
  V(truncf,             float, float) \
  V(truncl,             long double, long double)

#define COMPILER_RUNTIME_FUNC_LIST_MIPS(V) \
  V(__ashldi3,          long long, long long, int) \
  V(__ashrdi3,          long long, long long, int) \
  V(__divdi3,           long long, long long, long long) \
  V(__fixdfdi,          long long, double) \
  V(__fixsfdi,          long long, float) \
  V(__fixunsdfdi,       unsigned long long, double) \
  V(__fixunsdfsi,       unsigned int, double) \
  V(__fixunssfdi,       unsigned long long, float) \
  V(__fixunssfsi,       unsigned int, float) \
  V(__floatdidf,        double, long long) \
  V(__floatdisf,        float, long long) \
  V(__floatundidf,      double, unsigned long long) \
  V(__floatundisf,      float, unsigned long long) \
  V(__lshrdi3,          long long, long long, int) \
  V(__moddi3,           long long, long long, long long) \
  V(__muldi3,           long long, long long, long long) \
  V(__negdi2,           long long, long long) \
  V(__powidf2,          double, double, int) \
  V(__powisf2,          float, float, int) \
  V(__udivdi3,          unsigned long long, unsigned long long, unsigned long long) \
  V(__umoddi3,          unsigned long long, unsigned long long, unsigned long long) \
  V(ceil,               double, double) \
  V(ceilf,              float, float) \
  V(ceill,              long double, long double) \
  V(copysign,           double, double, double) \
  V(copysignf,          float, float, float) \
  V(copysignl,          long double, long double, long double) \
  V(cos,                double, double) \
  V(cosf,               float, float) \
  V(exp,                double, double) \
  V(exp2,               double, double) \
  V(exp2f,              float, float) \
  V(expf,               float, float) \
  V(floor,              double, double) \
  V(floorf,             float, float) \
  V(floorl,             long double, long double) \
  V(fma,                double, double, double, double) \
  V(fmaf,               float, float, float, float) \
  V(fmod,               double, double, double) \
  V(fmodf,              float, float, float) \
  V(log,                double, double) \
  V(log10,              double, double) \
  V(log10f,             float, float) \
  V(logf,               float, float) \
  V(memcpy,             void *, void *, const void *, size_t) \
  V(memmove,            void *, void *, const void *, size_t) \
  V(memset,             void *, void *, int, size_t) \
  V(nearbyint,          double, double) \
  V(nearbyintf,         float, float) \
  V(pow,                double, double, double) \
  V(powf,               float, float, float) \
  V(rint,               double, double) \
  V(rintf,              float, float) \
  V(sin,                double, double) \
  V(sinf,               float, float) \
  V(sqrt,               double, double) \
  V(sqrtf,              float, float) \
  V(trunc,              double, double) \
  V(truncf,             float, float) \
  V(truncl,             long double, long double)

#define COMPILER_RUNTIME_FUNC_LIST_ARM(V) \
  V(__aeabi_d2f,        float, double) \
  V(__aeabi_d2iz,       int, double) \
  V(__aeabi_d2lz,       long long, double) \
  V(__aeabi_d2uiz,      unsigned, double) \
  V(__aeabi_d2ulz,      unsigned long long, double) \
  V(__aeabi_dadd,       double, double, double) \
  V(__aeabi_dcmpeq,     int, double, double) \
  V(__aeabi_dcmpge,     int, double, double) \
  V(__aeabi_dcmpgt,     int, double, double) \
  V(__aeabi_dcmple,     int, double, double) \
  V(__aeabi_dcmplt,     int, double, double) \
  V(__aeabi_dcmpun,     int, double, double) \
  V(__aeabi_ddiv,       double, double, double) \
  V(__aeabi_dmul,       double, double, double) \
  V(__aeabi_dsub,       double, double, double) \
  V(__aeabi_f2d,        double, float) \
  V(__aeabi_f2iz,       int, float) \
  V(__aeabi_f2lz,       long long, float) \
  V(__aeabi_f2uiz,      unsigned int, float) \
  V(__aeabi_f2ulz,      unsigned long long, float) \
  V(__aeabi_fadd,       float, float, float) \
  V(__aeabi_fcmpeq,     int, float, float) \
  V(__aeabi_fcmpge,     int, float, float) \
  V(__aeabi_fcmpgt,     int, float, float) \
  V(__aeabi_fcmple,     int, float, float) \
  V(__aeabi_fcmplt,     int, float, float) \
  V(__aeabi_fcmpun,     int, float, float) \
  V(__aeabi_fdiv,       float, float, float) \
  V(__aeabi_fmul,       float, float, float) \
  V(__aeabi_fsub,       float, float, float) \
  V(__aeabi_i2d,        double, int) \
  V(__aeabi_i2f,        float, int) \
  V(__aeabi_idiv,       int, int, int) \
  V(__aeabi_l2d,        double, long long) \
  V(__aeabi_l2f,        float, long long) \
  V(__aeabi_lasr,       long long, long long, int) \
  V(__aeabi_ldivmod,    /* value in regs */ void, long long, long long) \
  V(__aeabi_llsl,       long long, long long, int) \
  V(__aeabi_llsr,       long long, long long, int) \
  V(__aeabi_lmul,       long long, long long, long long) \
  V(__aeabi_memcpy,     void, void *, const void *, size_t) \
  V(__aeabi_memmove,    void, void *, const void *, size_t) \
  V(__aeabi_memset,     void, void *, size_t, int) /* different from stdlib */ \
  V(__aeabi_ui2d,       double, unsigned int) \
  V(__aeabi_ui2f,       float, unsigned int) \
  V(__aeabi_uidiv,      unsigned int, unsigned int, unsigned int) \
  V(__aeabi_ul2d,       double, unsigned long long) \
  V(__aeabi_ul2f,       float, unsigned long long) \
  V(__aeabi_uldivmod,   /* value in regs */ void, unsigned long long, unsigned long long) \
  V(__moddi3,           long long, long long, long long) \
  V(__modsi3,           int, int, int) \
  V(__umoddi3,          unsigned long long, unsigned long long, unsigned long long) \
  V(__umodsi3,          unsigned int, unsigned int, unsigned int) \
  V(fmod,               double, double, double) \
  V(fmodf,              float, float, float) \
  V(memcpy,             void *, void *, const void *, size_t) \
  V(memmove,            void *, void *, const void *, size_t) \
  V(memset,             void *, void *, int, size_t)


#if defined(__arm__)
#define COMPILER_RUNTIME_FUNC_LIST_NATIVE(V) COMPILER_RUNTIME_FUNC_LIST_ARM(V)
#elif defined(__mips__)
#define COMPILER_RUNTIME_FUNC_LIST_NATIVE(V) COMPILER_RUNTIME_FUNC_LIST_MIPS(V)
#elif defined(__i386__)
#define COMPILER_RUNTIME_FUNC_LIST_NATIVE(V) COMPILER_RUNTIME_FUNC_LIST_X86(V)
#else
#error "Unknown target platform"
#endif

#endif // ART_SRC_COMPILER_LLVM_COMPILER_RUNTIME_FUNC_LIST_H_
