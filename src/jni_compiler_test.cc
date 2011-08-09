// Copyright 2011 Google Inc. All Rights Reserved.

#include <sys/mman.h>

#include "assembler.h"
#include "class_linker.h"
#include "common_test.h"
#include "dex_file.h"
#include "jni_compiler.h"
#include "mem_map.h"
#include "runtime.h"
#include "scoped_ptr.h"
#include "thread.h"
#include "gtest/gtest.h"

namespace art {

class JniCompilerTest : public RuntimeTest {
 protected:
  virtual void SetUp() {
    RuntimeTest::SetUp();
    // Create thunk code that performs the native to managed transition
    thunk_code_.reset(MemMap::Map(kPageSize,
                                  PROT_READ | PROT_WRITE | PROT_EXEC,
                                  MAP_ANONYMOUS | MAP_PRIVATE));
    CHECK(thunk_code_ !=  NULL);
    Assembler thk_asm;
    // TODO: shouldn't have machine specific code in a general purpose file
#if defined(__i386__)
    thk_asm.pushl(EDI);                   // preserve EDI
    thk_asm.movl(EAX, Address(ESP, 8));   // EAX = method->GetCode()
    thk_asm.movl(EDI, Address(ESP, 12));  // EDI = method
    thk_asm.pushl(Immediate(0));          // push pad
    thk_asm.pushl(Immediate(0));          // push pad
    thk_asm.pushl(Address(ESP, 44));      // push pad  or jlong high
    thk_asm.pushl(Address(ESP, 44));      // push jint or jlong low
    thk_asm.pushl(Address(ESP, 44));      // push jint or jlong high
    thk_asm.pushl(Address(ESP, 44));      // push jint or jlong low
    thk_asm.pushl(Address(ESP, 44));      // push jobject
    thk_asm.call(EAX);                    // Continue in method->GetCode()
    thk_asm.addl(ESP, Immediate(28));     // pop arguments
    thk_asm.popl(EDI);                    // restore EDI
    thk_asm.ret();
#elif defined(__arm__)
    thk_asm.AddConstant(SP, -32);         // Build frame
    thk_asm.StoreToOffset(kStoreWord, LR, SP, 28);  // Spill link register
    thk_asm.StoreToOffset(kStoreWord, R9, SP, 24);  // Spill R9
    thk_asm.mov(R12, ShifterOperand(R0));  // R12 = method->GetCode()
    thk_asm.mov(R0,  ShifterOperand(R1));  // R0  = method
    thk_asm.mov(R9,  ShifterOperand(R2));  // R9  = Thread::Current()
    thk_asm.mov(R1,  ShifterOperand(R3));  // R1  = arg1 (jint/jlong low)
    thk_asm.LoadFromOffset(kLoadWord, R3, SP, 44);  // R3 = arg5 (pad/jlong high)
    thk_asm.StoreToOffset(kStoreWord, R3, SP, 4);
    thk_asm.LoadFromOffset(kLoadWord, R3, SP, 40);  // R3 = arg4 (jint/jlong low)
    thk_asm.StoreToOffset(kStoreWord, R3, SP, 0);
    thk_asm.LoadFromOffset(kLoadWord, R3, SP, 36);  // R3 = arg3 (jint/jlong high)
    thk_asm.LoadFromOffset(kLoadWord, R2, SP, 32);  // R2 = arg2 (jint/jlong high)
    thk_asm.blx(R12);                     // Branch and link R12
    thk_asm.LoadFromOffset(kLoadWord, LR, SP, 28);  // Fill link register
    thk_asm.LoadFromOffset(kLoadWord, R9, SP, 24);  // Fill R9
    thk_asm.AddConstant(SP, 32);          // Remove frame
    thk_asm.mov(PC, ShifterOperand(LR));  // Return
#else
#error Unimplemented
#endif
    size_t cs = thk_asm.CodeSize();
    MemoryRegion code(thunk_code_->GetAddress(), cs);
    thk_asm.FinalizeInstructions(code);
    thunk_entry1_ = reinterpret_cast<jint (*)(const void*, art::Method*,
                                              Thread*, jobject, jint, jint,
                                              jint)
                                    >(code.pointer());
    thunk_entry2_ = reinterpret_cast<jdouble (*)(const void*, art::Method*,
                                                 Thread*, jobject, jdouble,
                                                 jdouble)
                                    >(code.pointer());
  }

  virtual void TearDown() {
    // Release thunk code
    CHECK(runtime_->DetachCurrentThread());
  }

  // Run generated code associated with method passing and returning int size
  // arguments
  jvalue RunMethod(Method* method, jvalue a, jvalue b, jvalue c, jvalue d) {
    jvalue result;
    // sanity checks
    EXPECT_NE(static_cast<void*>(NULL), method->GetCode());
    EXPECT_EQ(0u, Thread::Current()->NumShbHandles());
    EXPECT_EQ(Thread::kRunnable, Thread::Current()->GetState());
    // perform call
    result.i = (*thunk_entry1_)(method->GetCode(), method, Thread::Current(),
                                a.l, b.i, c.i, d.i);
    // sanity check post-call
    EXPECT_EQ(0u, Thread::Current()->NumShbHandles());
    EXPECT_EQ(Thread::kRunnable, Thread::Current()->GetState());
    return result;
  }

  // Run generated code associated with method passing and returning double size
  // arguments
  jvalue RunMethodD(Method* method, jvalue a, jvalue b, jvalue c) {
    jvalue result;
    // sanity checks
    EXPECT_NE(static_cast<void*>(NULL), method->GetCode());
    EXPECT_EQ(0u, Thread::Current()->NumShbHandles());
    EXPECT_EQ(Thread::kRunnable, Thread::Current()->GetState());
    // perform call
    result.d = (*thunk_entry2_)(method->GetCode(), method, Thread::Current(),
                                a.l, b.d, c.d);
    // sanity check post-call
    EXPECT_EQ(0u, Thread::Current()->NumShbHandles());
    EXPECT_EQ(Thread::kRunnable, Thread::Current()->GetState());
    return result;
  }

  scoped_ptr<MemMap> thunk_code_;
  jint (*thunk_entry1_)(const void*, Method*, Thread*, jobject, jint, jint,
                        jint);
  jdouble (*thunk_entry2_)(const void*, Method*, Thread*, jobject, jdouble,
                           jdouble);
};

int gJava_MyClass_foo_calls = 0;
void Java_MyClass_foo(JNIEnv*, jobject) {
  EXPECT_EQ(1u, Thread::Current()->NumShbHandles());
  EXPECT_EQ(Thread::kNative, Thread::Current()->GetState());
  gJava_MyClass_foo_calls++;
}

int gJava_MyClass_fooI_calls = 0;
jint Java_MyClass_fooI(JNIEnv*, jobject, jint x) {
  EXPECT_EQ(1u, Thread::Current()->NumShbHandles());
  EXPECT_EQ(Thread::kNative, Thread::Current()->GetState());
  gJava_MyClass_fooI_calls++;
  return x;
}

int gJava_MyClass_fooII_calls = 0;
jint Java_MyClass_fooII(JNIEnv*, jobject, jint x, jint y) {
  EXPECT_EQ(1u, Thread::Current()->NumShbHandles());
  EXPECT_EQ(Thread::kNative, Thread::Current()->GetState());
  gJava_MyClass_fooII_calls++;
  return x - y;  // non-commutative operator
}

int gJava_MyClass_fooDD_calls = 0;
jdouble Java_MyClass_fooDD(JNIEnv*, jobject, jdouble x, jdouble y) {
  EXPECT_EQ(1u, Thread::Current()->NumShbHandles());
  EXPECT_EQ(Thread::kNative, Thread::Current()->GetState());
  gJava_MyClass_fooDD_calls++;
  return x - y;  // non-commutative operator
}

int gJava_MyClass_fooIOO_calls = 0;
jobject Java_MyClass_fooIOO(JNIEnv*, jobject thisObject, jint x, jobject y,
                            jobject z) {
  EXPECT_EQ(3u, Thread::Current()->NumShbHandles());
  EXPECT_EQ(Thread::kNative, Thread::Current()->GetState());
  gJava_MyClass_fooIOO_calls++;
  switch (x) {
    case 1:
      return y;
    case 2:
      return z;
    default:
      return thisObject;
  }
}

int gJava_MyClass_fooSIOO_calls = 0;
jobject Java_MyClass_fooSIOO(JNIEnv* env, jclass klass, jint x, jobject y,
                             jobject z) {
  EXPECT_EQ(Thread::Current()->GetJniEnv(), env);
  EXPECT_EQ(3u, Thread::Current()->NumShbHandles());
  EXPECT_EQ(Thread::kNative, Thread::Current()->GetState());
  gJava_MyClass_fooSIOO_calls++;
  switch (x) {
    case 1:
      return y;
    case 2:
      return z;
    default:
      return klass;
  }
}

int gJava_MyClass_fooSSIOO_calls = 0;
jobject Java_MyClass_fooSSIOO(JNIEnv*, jclass klass, jint x, jobject y,
                             jobject z) {
  EXPECT_EQ(3u, Thread::Current()->NumShbHandles());
  EXPECT_EQ(Thread::kNative, Thread::Current()->GetState());
  gJava_MyClass_fooSSIOO_calls++;
  switch (x) {
    case 1:
      return y;
    case 2:
      return z;
    default:
      return klass;
  }
}

TEST_F(JniCompilerTest, CompileAndRunNoArgMethod) {
  scoped_ptr<DexFile> dex(OpenDexFileBase64(kMyClassNativesDex));
  PathClassLoader* class_loader = AllocPathClassLoader(dex.get());

  Class* klass = class_linker_->FindClass("LMyClass;", class_loader);
  ASSERT_TRUE(klass != NULL);

  Method* method = klass->FindVirtualMethod("foo", "()V");
  ASSERT_TRUE(method != NULL);

  Assembler jni_asm;
  JniCompiler jni_compiler;
  jni_compiler.Compile(&jni_asm, method);

  // TODO: should really use JNIEnv to RegisterNative, but missing a
  // complete story on this, so hack the RegisterNative below
  // JNIEnv* env = Thread::Current()->GetJniEnv();
  // JNINativeMethod methods[] = {{"foo", "()V", (void*)&Java_MyClass_foo}};
  method->RegisterNative(reinterpret_cast<void*>(&Java_MyClass_foo));

  jvalue a;
  a.l = (jobject)NULL;
  gJava_MyClass_foo_calls = 0;
  RunMethod(method, a, a, a, a);
  EXPECT_EQ(1, gJava_MyClass_foo_calls);
  RunMethod(method, a, a, a, a);
  EXPECT_EQ(2, gJava_MyClass_foo_calls);
}

TEST_F(JniCompilerTest, CompileAndRunIntMethod) {
  scoped_ptr<DexFile> dex(OpenDexFileBase64(kMyClassNativesDex));
  PathClassLoader* class_loader = AllocPathClassLoader(dex.get());

  Class* klass = class_linker_->FindClass("LMyClass;", class_loader);
  ASSERT_TRUE(klass != NULL);

  Method* method = klass->FindVirtualMethod("fooI", "(I)I");
  ASSERT_TRUE(method != NULL);

  Assembler jni_asm;
  JniCompiler jni_compiler;
  jni_compiler.Compile(&jni_asm, method);

  // TODO: should really use JNIEnv to RegisterNative, but missing a
  // complete story on this, so hack the RegisterNative below
  method->RegisterNative(reinterpret_cast<void*>(&Java_MyClass_fooI));

  jvalue a, b, c;
  a.l = (jobject)NULL;
  b.i = 42;
  EXPECT_EQ(0, gJava_MyClass_fooI_calls);
  c = RunMethod(method, a, b, a, a);
  ASSERT_EQ(42, c.i);
  EXPECT_EQ(1, gJava_MyClass_fooI_calls);
  b.i = 0xCAFED00D;
  c = RunMethod(method, a, b, a, a);
  ASSERT_EQ((jint)0xCAFED00D, c.i);
  EXPECT_EQ(2, gJava_MyClass_fooI_calls);
}

TEST_F(JniCompilerTest, CompileAndRunIntIntMethod) {
  scoped_ptr<DexFile> dex(OpenDexFileBase64(kMyClassNativesDex));
  PathClassLoader* class_loader = AllocPathClassLoader(dex.get());

  Class* klass = class_linker_->FindClass("LMyClass;", class_loader);
  ASSERT_TRUE(klass != NULL);

  Method* method = klass->FindVirtualMethod("fooII", "(II)I");
  ASSERT_TRUE(method != NULL);

  Assembler jni_asm;
  JniCompiler jni_compiler;
  jni_compiler.Compile(&jni_asm, method);

  // TODO: should really use JNIEnv to RegisterNative, but missing a
  // complete story on this, so hack the RegisterNative below
  method->RegisterNative(reinterpret_cast<void*>(&Java_MyClass_fooII));

  jvalue a, b, c, d;
  a.l = (jobject)NULL;
  b.i = 99;
  c.i = 10;
  EXPECT_EQ(0, gJava_MyClass_fooII_calls);
  d = RunMethod(method, a, b, c, a);
  ASSERT_EQ(99 - 10, d.i);
  EXPECT_EQ(1, gJava_MyClass_fooII_calls);
  b.i = 0xCAFEBABE;
  c.i = 0xCAFED00D;
  d = RunMethod(method, a, b, c, a);
  ASSERT_EQ((jint)(0xCAFEBABE - 0xCAFED00D), d.i);
  EXPECT_EQ(2, gJava_MyClass_fooII_calls);
}


TEST_F(JniCompilerTest, CompileAndRunDoubleDoubleMethod) {
  scoped_ptr<DexFile> dex(OpenDexFileBase64(kMyClassNativesDex));
  PathClassLoader* class_loader = AllocPathClassLoader(dex.get());

  Class* klass = class_linker_->FindClass("LMyClass;", class_loader);
  ASSERT_TRUE(klass != NULL);

  Method* method = klass->FindVirtualMethod("fooDD", "(DD)D");
  ASSERT_TRUE(method != NULL);

  Assembler jni_asm;
  JniCompiler jni_compiler;
  jni_compiler.Compile(&jni_asm, method);

  // TODO: should really use JNIEnv to RegisterNative, but missing a
  // complete story on this, so hack the RegisterNative below
  method->RegisterNative(reinterpret_cast<void*>(&Java_MyClass_fooDD));

  jvalue a, b, c, d;
  a.l = (jobject)NULL;
  b.d = 99;
  c.d = 10;
  EXPECT_EQ(0, gJava_MyClass_fooDD_calls);
  d = RunMethodD(method, a, b, c);
  ASSERT_EQ(b.d - c.d, d.d);
  EXPECT_EQ(1, gJava_MyClass_fooDD_calls);
  b.d = 3.14159265358979323846;
  c.d = 0.69314718055994530942;
  d = RunMethodD(method, a, b, c);
  ASSERT_EQ(b.d - c.d, d.d);
  EXPECT_EQ(2, gJava_MyClass_fooDD_calls);
}

TEST_F(JniCompilerTest, CompileAndRunIntObjectObjectMethod) {
  scoped_ptr<DexFile> dex(OpenDexFileBase64(kMyClassNativesDex));
  PathClassLoader* class_loader = AllocPathClassLoader(dex.get());

  Class* klass = class_linker_->FindClass("LMyClass;", class_loader);
  ASSERT_TRUE(klass != NULL);

  Method* method = klass->FindVirtualMethod(
      "fooIOO",
      "(ILjava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
  ASSERT_TRUE(method != NULL);

  Assembler jni_asm;
  JniCompiler jni_compiler;
  jni_compiler.Compile(&jni_asm, method);

  // TODO: should really use JNIEnv to RegisterNative, but missing a
  // complete story on this, so hack the RegisterNative below
  method->RegisterNative(reinterpret_cast<void*>(&Java_MyClass_fooIOO));

  jvalue a, b, c, d, e;
  a.l = (jobject)NULL;
  b.i = 0;
  c.l = (jobject)NULL;
  d.l = (jobject)NULL;
  EXPECT_EQ(0, gJava_MyClass_fooIOO_calls);
  e = RunMethod(method, a, b, c, d);
  ASSERT_EQ((jobject)NULL, e.l);
  EXPECT_EQ(1, gJava_MyClass_fooIOO_calls);
  a.l = (jobject)8;
  b.i = 0;
  c.l = (jobject)NULL;
  d.l = (jobject)16;
  e = RunMethod(method, a, b, c, d);
  ASSERT_EQ((jobject)8, e.l);
  EXPECT_EQ(2, gJava_MyClass_fooIOO_calls);
  b.i = 1;
  e = RunMethod(method, a, b, c, d);
  ASSERT_EQ((jobject)NULL, e.l);
  EXPECT_EQ(3, gJava_MyClass_fooIOO_calls);
  b.i = 2;
  e = RunMethod(method, a, b, c, d);
  ASSERT_EQ((jobject)16, e.l);
  EXPECT_EQ(4, gJava_MyClass_fooIOO_calls);
  a.l = (jobject)8;
  b.i = 0;
  c.l = (jobject)16;
  d.l = (jobject)NULL;
  e = RunMethod(method, a, b, c, d);
  ASSERT_EQ((jobject)8, e.l);
  EXPECT_EQ(5, gJava_MyClass_fooIOO_calls);
  b.i = 1;
  e = RunMethod(method, a, b, c, d);
  ASSERT_EQ((jobject)16, e.l);
  EXPECT_EQ(6, gJava_MyClass_fooIOO_calls);
  b.i = 2;
  e = RunMethod(method, a, b, c, d);
  ASSERT_EQ((jobject)NULL, e.l);
  EXPECT_EQ(7, gJava_MyClass_fooIOO_calls);
}

TEST_F(JniCompilerTest, CompileAndRunStaticIntObjectObjectMethod) {
  scoped_ptr<DexFile> dex(OpenDexFileBase64(kMyClassNativesDex));
  PathClassLoader* class_loader = AllocPathClassLoader(dex.get());

  Class* klass = class_linker_->FindClass("LMyClass;", class_loader);
  ASSERT_TRUE(klass != NULL);

  Method* method = klass->FindDirectMethod(
      "fooSIOO",
      "(ILjava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
  ASSERT_TRUE(method != NULL);

  Assembler jni_asm;
  JniCompiler jni_compiler;
  jni_compiler.Compile(&jni_asm, method);

  // TODO: should really use JNIEnv to RegisterNative, but missing a
  // complete story on this, so hack the RegisterNative below
  method->RegisterNative(reinterpret_cast<void*>(&Java_MyClass_fooSIOO));

  jvalue a, b, c, d;
  a.i = 0;
  b.l = (jobject)NULL;
  c.l = (jobject)NULL;
  EXPECT_EQ(0, gJava_MyClass_fooSIOO_calls);
  d = RunMethod(method, a, b, c, a);
  ASSERT_EQ((jobject)method->GetClass(), d.l);
  EXPECT_EQ(1, gJava_MyClass_fooSIOO_calls);
  a.i = 0;
  b.l = (jobject)NULL;
  c.l = (jobject)16;
  d = RunMethod(method, a, b, c, a);
  ASSERT_EQ((jobject)method->GetClass(), d.l);
  EXPECT_EQ(2, gJava_MyClass_fooSIOO_calls);
  a.i = 1;
  d = RunMethod(method, a, b, c, a);
  ASSERT_EQ((jobject)NULL, d.l);
  EXPECT_EQ(3, gJava_MyClass_fooSIOO_calls);
  a.i = 2;
  d = RunMethod(method, a, b, c, a);
  ASSERT_EQ((jobject)16, d.l);
  EXPECT_EQ(4, gJava_MyClass_fooSIOO_calls);
  a.i = 0;
  b.l = (jobject)16;
  c.l = (jobject)NULL;
  d = RunMethod(method, a, b, c, a);
  ASSERT_EQ((jobject)method->GetClass(), d.l);
  EXPECT_EQ(5, gJava_MyClass_fooSIOO_calls);
  a.i = 1;
  d = RunMethod(method, a, b, c, a);
  ASSERT_EQ((jobject)16, d.l);
  EXPECT_EQ(6, gJava_MyClass_fooSIOO_calls);
  a.i = 2;
  d = RunMethod(method, a, b, c, a);
  ASSERT_EQ((jobject)NULL, d.l);
  EXPECT_EQ(7, gJava_MyClass_fooSIOO_calls);
}

TEST_F(JniCompilerTest, CompileAndRunStaticSynchronizedIntObjectObjectMethod) {
  scoped_ptr<DexFile> dex(OpenDexFileBase64(kMyClassNativesDex));
  PathClassLoader* class_loader = AllocPathClassLoader(dex.get());

  Class* klass = class_linker_->FindClass("LMyClass;", class_loader);
  ASSERT_TRUE(klass != NULL);

  Method* method = klass->FindDirectMethod(
      "fooSSIOO",
      "(ILjava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
  ASSERT_TRUE(method != NULL);

  Assembler jni_asm;
  JniCompiler jni_compiler;
  jni_compiler.Compile(&jni_asm, method);

  // TODO: should really use JNIEnv to RegisterNative, but missing a
  // complete story on this, so hack the RegisterNative below
  method->RegisterNative(reinterpret_cast<void*>(&Java_MyClass_fooSSIOO));

  jvalue a, b, c, d;
  a.i = 0;
  b.l = (jobject)NULL;
  c.l = (jobject)NULL;
  EXPECT_EQ(0, gJava_MyClass_fooSSIOO_calls);
  d = RunMethod(method, a, b, c, a);
  ASSERT_EQ((jobject)method->GetClass(), d.l);
  EXPECT_EQ(1, gJava_MyClass_fooSSIOO_calls);
  a.i = 0;
  b.l = (jobject)NULL;
  c.l = (jobject)16;
  d = RunMethod(method, a, b, c, a);
  ASSERT_EQ((jobject)method->GetClass(), d.l);
  EXPECT_EQ(2, gJava_MyClass_fooSSIOO_calls);
  a.i = 1;
  d = RunMethod(method, a, b, c, a);
  ASSERT_EQ((jobject)NULL, d.l);
  EXPECT_EQ(3, gJava_MyClass_fooSSIOO_calls);
  a.i = 2;
  d = RunMethod(method, a, b, c, a);
  ASSERT_EQ((jobject)16, d.l);
  EXPECT_EQ(4, gJava_MyClass_fooSSIOO_calls);
  a.i = 0;
  b.l = (jobject)16;
  c.l = (jobject)NULL;
  d = RunMethod(method, a, b, c, a);
  ASSERT_EQ((jobject)method->GetClass(), d.l);
  EXPECT_EQ(5, gJava_MyClass_fooSSIOO_calls);
  a.i = 1;
  d = RunMethod(method, a, b, c, a);
  ASSERT_EQ((jobject)16, d.l);
  EXPECT_EQ(6, gJava_MyClass_fooSSIOO_calls);
  a.i = 2;
  d = RunMethod(method, a, b, c, a);
  ASSERT_EQ((jobject)NULL, d.l);
  EXPECT_EQ(7, gJava_MyClass_fooSSIOO_calls);
}

int gSuspendCounterHandler_calls;
void SuspendCountHandler(Method** frame) {
  EXPECT_TRUE((*frame)->GetName()->Equals("fooI"));
  gSuspendCounterHandler_calls++;
  Thread::Current()->DecrementSuspendCount();
}
TEST_F(JniCompilerTest, SuspendCountAcknolewdgement) {
  scoped_ptr<DexFile> dex(OpenDexFileBase64(kMyClassNativesDex));
  PathClassLoader* class_loader = AllocPathClassLoader(dex.get());

  Class* klass = class_linker_->FindClass("LMyClass;", class_loader);
  ASSERT_TRUE(klass != NULL);

  Method* method = klass->FindVirtualMethod("fooI", "(I)I");
  ASSERT_TRUE(method != NULL);

  Assembler jni_asm;
  JniCompiler jni_compiler;
  jni_compiler.Compile(&jni_asm, method);

  // TODO: should really use JNIEnv to RegisterNative, but missing a
  // complete story on this, so hack the RegisterNative below
  method->RegisterNative(reinterpret_cast<void*>(&Java_MyClass_fooI));
  Thread::Current()->RegisterSuspendCountEntryPoint(&SuspendCountHandler);

  gSuspendCounterHandler_calls = 0;
  gJava_MyClass_fooI_calls = 0;
  jvalue a, b, c;
  a.l = (jobject)NULL;
  b.i = 42;
  c = RunMethod(method, a, b, a, a);
  ASSERT_EQ(42, c.i);
  EXPECT_EQ(1, gJava_MyClass_fooI_calls);
  EXPECT_EQ(0, gSuspendCounterHandler_calls);
  Thread::Current()->IncrementSuspendCount();
  c = RunMethod(method, a, b, a, a);
  ASSERT_EQ(42, c.i);
  EXPECT_EQ(2, gJava_MyClass_fooI_calls);
  EXPECT_EQ(1, gSuspendCounterHandler_calls);
  c = RunMethod(method, a, b, a, a);
  ASSERT_EQ(42, c.i);
  EXPECT_EQ(3, gJava_MyClass_fooI_calls);
  EXPECT_EQ(1, gSuspendCounterHandler_calls);
}

int gExceptionHandler_calls;
void ExceptionHandler(Method** frame) {
  EXPECT_TRUE((*frame)->GetName()->Equals("foo"));
  gExceptionHandler_calls++;
  Thread::Current()->ClearException();
}
TEST_F(JniCompilerTest, ExceptionHandling) {
  scoped_ptr<DexFile> dex(OpenDexFileBase64(kMyClassNativesDex));
  PathClassLoader* class_loader = AllocPathClassLoader(dex.get());

  Class* klass = class_linker_->FindClass("LMyClass;", class_loader);
  ASSERT_TRUE(klass != NULL);

  Method* method = klass->FindVirtualMethod("foo", "()V");
  ASSERT_TRUE(method != NULL);

  Assembler jni_asm;
  JniCompiler jni_compiler;
  jni_compiler.Compile(&jni_asm, method);

  // TODO: should really use JNIEnv to RegisterNative, but missing a
  // complete story on this, so hack the RegisterNative below
  method->RegisterNative(reinterpret_cast<void*>(&Java_MyClass_foo));
  Thread::Current()->RegisterExceptionEntryPoint(&ExceptionHandler);

  gExceptionHandler_calls = 0;
  gJava_MyClass_foo_calls = 0;
  jvalue a;
  a.l = (jobject)NULL;
  RunMethod(method, a, a, a, a);
  EXPECT_EQ(1, gJava_MyClass_foo_calls);
  EXPECT_EQ(0, gExceptionHandler_calls);
  // TODO: create a real exception here
  Thread::Current()->SetException(reinterpret_cast<Object*>(8));
  RunMethod(method, a, a, a, a);
  EXPECT_EQ(2, gJava_MyClass_foo_calls);
  EXPECT_EQ(1, gExceptionHandler_calls);
  RunMethod(method, a, a, a, a);
  EXPECT_EQ(3, gJava_MyClass_foo_calls);
  EXPECT_EQ(1, gExceptionHandler_calls);
}

}  // namespace art
