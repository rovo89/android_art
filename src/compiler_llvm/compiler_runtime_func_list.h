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

// NOTE: COMPILER_RUNTIME_FUNC_LIST should be sorted!

#if defined(__i386__) || defined(__mips__)

#define COMPILER_RUNTIME_FUNC_LIST(V) \
  V(__ashldi3) \
  V(__ashrdi3) \
  V(__divdi3) \
  V(__fixdfdi) \
  V(__fixsfdi) \
  V(__fixtfdi) \
  V(__fixtfsi) \
  V(__fixunsdfdi) \
  V(__fixunsdfsi) \
  V(__fixunssfdi) \
  V(__fixunssfsi) \
  V(__fixunstfdi) \
  V(__fixunstfsi) \
  V(__fixunsxfdi) \
  V(__fixunsxfsi) \
  V(__fixxfdi) \
  V(__floatdidf) \
  V(__floatdisf) \
  V(__floatditf) \
  V(__floatdixf) \
  V(__floatsitf) \
  V(__floatundidf) \
  V(__floatundisf) \
  V(__floatunditf) \
  V(__floatundixf) \
  V(__floatunsitf) \
  V(__lshrdi3) \
  V(__moddi3) \
  V(__muldi3) \
  V(__negdi2) \
  V(__powidf2) \
  V(__powisf2) \
  V(__powitf2) \
  V(__powixf2) \
  V(__trunctfdf2) \
  V(__trunctfsf2) \
  V(__udivdi3) \
  V(__umoddi3) \
  V(ceil) \
  V(ceilf) \
  V(ceill) \
  V(copysign) \
  V(copysignf) \
  V(copysignl) \
  V(cos) \
  V(cosf) \
  V(cosl) \
  V(exp) \
  V(exp2) \
  V(exp2f) \
  V(exp2l) \
  V(expf) \
  V(expl) \
  V(floor) \
  V(floorf) \
  V(floorl) \
  V(fma) \
  V(fmaf) \
  V(fmal) \
  V(fmod) \
  V(fmodf) \
  V(fmodl) \
  V(log) \
  V(log10) \
  V(log10f) \
  V(log10l) \
  V(log2) \
  V(log2f) \
  V(log2l) \
  V(logf) \
  V(logl) \
  V(memcpy) \
  V(memmove) \
  V(memset) \
  V(nearbyint) \
  V(nearbyintf) \
  V(nearbyintl) \
  V(pow) \
  V(powf) \
  V(powl) \
  V(rint) \
  V(rintf) \
  V(rintl) \
  V(sin) \
  V(sinf) \
  V(sinl) \
  V(sqrt) \
  V(sqrtf) \
  V(sqrtl) \
  V(trunc) \
  V(truncf) \
  V(truncl)

#elif defined(__arm__)

#define COMPILER_RUNTIME_FUNC_LIST(V) \
  V(__aeabi_d2f) \
  V(__aeabi_d2iz) \
  V(__aeabi_d2lz) \
  V(__aeabi_d2uiz) \
  V(__aeabi_d2ulz) \
  V(__aeabi_dadd) \
  V(__aeabi_dcmpeq) \
  V(__aeabi_dcmpeq) \
  V(__aeabi_dcmpge) \
  V(__aeabi_dcmpgt) \
  V(__aeabi_dcmple) \
  V(__aeabi_dcmplt) \
  V(__aeabi_dcmpun) \
  V(__aeabi_dcmpun) \
  V(__aeabi_ddiv) \
  V(__aeabi_dmul) \
  V(__aeabi_dsub) \
  V(__aeabi_f2d) \
  V(__aeabi_f2iz) \
  V(__aeabi_f2lz) \
  V(__aeabi_f2uiz) \
  V(__aeabi_f2ulz) \
  V(__aeabi_fadd) \
  V(__aeabi_fcmpeq) \
  V(__aeabi_fcmpeq) \
  V(__aeabi_fcmpge) \
  V(__aeabi_fcmpgt) \
  V(__aeabi_fcmple) \
  V(__aeabi_fcmplt) \
  V(__aeabi_fcmpun) \
  V(__aeabi_fcmpun) \
  V(__aeabi_fdiv) \
  V(__aeabi_fmul) \
  V(__aeabi_fsub) \
  V(__aeabi_i2d) \
  V(__aeabi_i2f) \
  V(__aeabi_idiv) \
  V(__aeabi_idiv) \
  V(__aeabi_idiv) \
  V(__aeabi_l2d) \
  V(__aeabi_l2f) \
  V(__aeabi_lasr) \
  V(__aeabi_ldivmod) \
  V(__aeabi_llsl) \
  V(__aeabi_llsr) \
  V(__aeabi_lmul) \
  V(__aeabi_memcpy) \
  V(__aeabi_memmove) \
  V(__aeabi_memset) \
  V(__aeabi_ui2d) \
  V(__aeabi_ui2f) \
  V(__aeabi_uidiv) \
  V(__aeabi_uidiv) \
  V(__aeabi_uidiv) \
  V(__aeabi_ul2d) \
  V(__aeabi_ul2f) \
  V(__aeabi_uldivmod) \
  V(__moddi3) \
  V(__modsi3) \
  V(__umoddi3) \
  V(__umodsi3)

#else

#error "Unknown target platform"

#endif
