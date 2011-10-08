// Copyright 2011 Google Inc. All Rights Reserved.

#ifndef ART_SRC_RUNTIME_SUPPORT_H_
#define ART_SRC_RUNTIME_SUPPORT_H_

#include "class_linker.h"
#include "object.h"
#include "thread_list.h"
#include "utils.h"

namespace art {

extern void CheckSuspendFromCode(Thread* thread);
extern Array* CheckAndAllocArrayFromCode(uint32_t type_idx, Method* method, int32_t component_count,
                                         Thread* self);
extern void DebugMe(Method* method, uint32_t info);
extern Object* DecodeJObjectInThread(Thread* thread, jobject obj);
extern Field* FindFieldFromCode(uint32_t field_idx, const Method* referrer, bool is_static);
extern void* FindNativeMethod(Thread* thread);
extern void ThrowAbstractMethodErrorFromCode(Method* method, Thread* thread, Method** sp);
void* UnresolvedDirectMethodTrampolineFromCode(int32_t, void*, Thread*, Runtime::TrampolineType);
extern Class* InitializeStaticStorage(uint32_t type_idx, const Method* referrer, Thread* self);
extern Class* InitializeTypeFromCode(uint32_t type_idx, Method* method);
extern void ResolveMethodFromCode(Method* method, uint32_t method_idx);
extern void LockObjectFromCode(Thread* thread, Object* obj);
extern int64_t D2L(double d);
extern int64_t F2L(float f);

}  // namespace art
/* Helper for both JNI and regular compiled code */
extern "C" void art_deliver_exception_from_code(void*);

#if defined(__arm__)
  /* Compiler helpers */
  extern "C" void* art_alloc_object_from_code(uint32_t type_idx, void* method);
  extern "C" void* art_alloc_array_from_code(uint32_t, void*, int32_t);
  extern "C" void* art_check_and_alloc_array_from_code(uint32_t, void*, int32_t);
  extern "C" void art_can_put_array_element_from_code(void*, void*);
  extern "C" void art_check_cast_from_code(void*, void*);
  extern "C" void* art_find_instance_field_from_code(uint32_t, void*);
  extern "C" void* art_find_static_field_from_code(uint32_t, void*);
  extern "C" int32_t art_get32_static_from_code(uint32_t, void*);
  extern "C" int64_t art_get64_static_from_code(uint32_t, void*);
  extern "C" void* art_get_obj_static_from_code(uint32_t, void*);
  extern "C" void art_handle_fill_data_from_code(void*, void*);
  extern "C" void* art_initialize_static_storage_from_code(uint32_t, void*);
  extern "C" void* art_resolve_string_from_code(void*, uint32_t);
  extern "C" void art_invoke_interface_trampoline(uint32_t, void*);
  extern "C" int art_set32_static_from_code(uint32_t, void*, int32_t);
  extern "C" int art_set64_static_from_code(uint32_t, void*, int64_t);
  extern "C" int art_set_obj_static_from_code(uint32_t, void*, void*);
  extern "C" void art_test_suspend();
  extern "C" void art_throw_array_bounds_from_code(int32_t index, int32_t limit);
  extern "C" void art_throw_div_zero_from_code();
  extern "C" void art_throw_neg_array_size_from_code(int32_t size);
  extern "C" void art_throw_no_such_method_from_code(int32_t method_idx);
  extern "C" void art_throw_null_pointer_exception_from_code();
  extern "C" void art_throw_stack_overflow_from_code(void*);
  extern "C" void art_throw_verification_error_from_code(int32_t src1, int32_t ref);
  extern "C" void art_lock_object_from_code(void*);
  extern "C" void art_unlock_object_from_code(void*);

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
  extern "C" uint64_t art_shl_long(uint64_t, uint32_t);
  extern "C" uint64_t art_shr_long(uint64_t, uint32_t);
  extern "C" uint64_t art_ushr_long(uint64_t, uint32_t);

#endif

#endif  // ART_SRC_RUNTIME_SUPPORT_H_
