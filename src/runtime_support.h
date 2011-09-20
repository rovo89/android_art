// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_RUNTIME_SUPPORT_H_
#define ART_SRC_RUNTIME_SUPPORT_H_

/* Helper for both JNI and regular compiled code */
extern "C" void art_deliver_exception_from_code(void*);

#if defined(__arm__)
  /* Compiler helpers */
  extern "C" void art_check_cast_from_code(void*, void*);
  extern "C" void art_handle_fill_data_from_code(void*, void*);
  extern "C" void art_invoke_interface_trampoline(void*, void*, void*, void*);
  extern "C" void art_throw_array_bounds_from_code(int32_t index, int32_t limit);
  extern "C" void art_throw_div_zero_from_code();
  extern "C" void art_throw_null_pointer_exception_from_code();
  extern "C" void art_unlock_object_from_code(void*, void*);
  extern "C" uint64_t art_shl_long(uint64_t, uint32_t);
  extern "C" uint64_t art_shr_long(uint64_t, uint32_t);
  extern "C" uint64_t art_ushr_long(uint64_t, uint32_t);
  extern "C" void art_throw_null_pointer_exception_from_code();
  extern "C" void art_throw_div_zero_from_code();
  extern "C" void art_throw_array_bounds_from_code(int32_t index, int32_t limit);
  extern "C" void art_invoke_interface_trampoline(void*, void*, void*, void*);
  extern "C" void art_test_suspend();

  /* Conversions */
  extern "C" float __aeabi_i2f(int op1);             // OP_INT_TO_FLOAT
  extern "C" int __aeabi_f2iz(float op1);            // OP_FLOAT_TO_INT
  extern "C" float __aeabi_d2f(double op1);          // OP_DOUBLE_TO_FLOAT
  extern "C" double __aeabi_f2d(float op1);          // OP_FLOAT_TO_DOUBLE
  extern "C" double __aeabi_i2d(int op1);            // OP_INT_TO_DOUBLE
  extern "C" int __aeabi_d2iz(double op1);           // OP_DOUBLE_TO_INT
  extern "C" float __aeabi_l2f(long op1);            // OP_LONG_TO_FLOAT
  extern "C" double __aeabi_l2d(long op1);           // OP_LONG_TO_DOUBLE

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
  extern "C" long long __aeabi_lmul(long long op1, long long op2);

#endif

#endif  // ART_SRC_RUNTIME_SUPPORT_H_
