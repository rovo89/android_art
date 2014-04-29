/*
 * Copyright (C) 2014 The Android Open Source Project
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

#include "common_runtime_test.h"

#include <cstdio>

namespace art {


class StubTest : public CommonRuntimeTest {
 protected:
  // We need callee-save methods set up in the Runtime for exceptions.
  void SetUp() OVERRIDE {
    // Do the normal setup.
    CommonRuntimeTest::SetUp();

    {
      // Create callee-save methods
      ScopedObjectAccess soa(Thread::Current());
      for (int i = 0; i < Runtime::kLastCalleeSaveType; i++) {
        Runtime::CalleeSaveType type = Runtime::CalleeSaveType(i);
        if (!runtime_->HasCalleeSaveMethod(type)) {
          runtime_->SetCalleeSaveMethod(runtime_->CreateCalleeSaveMethod(kRuntimeISA, type), type);
        }
      }
    }
  }

  void SetUpRuntimeOptions(Runtime::Options *options) OVERRIDE {
    // Use a smaller heap
    for (std::pair<std::string, const void*>& pair : *options) {
      if (pair.first.find("-Xmx") == 0) {
        pair.first = "-Xmx4M";  // Smallest we can go.
      }
    }
  }

  size_t Invoke3(size_t arg0, size_t arg1, size_t arg2, uintptr_t code, Thread* self) {
    // Push a transition back into managed code onto the linked list in thread.
    ManagedStack fragment;
    self->PushManagedStackFragment(&fragment);

    size_t result;
#if defined(__i386__)
    // TODO: Set the thread?
    __asm__ __volatile__(
        "pushl $0\n\t"               // Push nullptr to terminate quick stack
        "call *%%edi\n\t"           // Call the stub
        "addl $4, %%esp"               // Pop nullptr
        : "=a" (result)
          // Use the result from eax
        : "a"(arg0), "c"(arg1), "d"(arg2), "D"(code)
          // This places code into edi, arg0 into eax, arg1 into ecx, and arg2 into edx
        : );  // clobber.
    // TODO: Should we clobber the other registers? EBX gets clobbered by some of the stubs,
    //       but compilation fails when declaring that.
#elif defined(__arm__)
    __asm__ __volatile__(
        "push {r1-r12, lr}\n\t"     // Save state, 13*4B = 52B
        ".cfi_adjust_cfa_offset 52\n\t"
        "sub sp, sp, #8\n\t"        // +8B, so 16B aligned with nullptr
        ".cfi_adjust_cfa_offset 8\n\t"
        "mov r0, %[arg0]\n\t"       // Set arg0-arg2
        "mov r1, %[arg1]\n\t"       // TODO: Any way to use constraints like on x86?
        "mov r2, %[arg2]\n\t"
        // Use r9 last as we don't know whether it was used for arg0-arg2
        "mov r9, #0\n\t"            // Push nullptr to terminate stack
        "push {r9}\n\t"
        ".cfi_adjust_cfa_offset 4\n\t"
        "mov r9, %[self]\n\t"       // Set the thread
        "blx %[code]\n\t"           // Call the stub
        "add sp, sp, #12\n\t"       // Pop nullptr and padding
        ".cfi_adjust_cfa_offset -12\n\t"
        "pop {r1-r12, lr}\n\t"      // Restore state
        ".cfi_adjust_cfa_offset -52\n\t"
        "mov %[result], r0\n\t"     // Save the result
        : [result] "=r" (result)
          // Use the result from r0
        : [arg0] "0"(arg0), [arg1] "r"(arg1), [arg2] "r"(arg2), [code] "r"(code), [self] "r"(self)
        : );  // clobber.
#elif defined(__aarch64__)
    __asm__ __volatile__(
        "sub sp, sp, #48\n\t"          // Reserve stack space, 16B aligned
        ".cfi_adjust_cfa_offset 48\n\t"
        "stp xzr, x1, [sp]\n\t"        // nullptr(end of quick stack), x1
        "stp x2, x18, [sp, #16]\n\t"   // Save x2, x18(xSELF)
        "str x30, [sp, #32]\n\t"       // Save xLR
        "mov x0, %[arg0]\n\t"          // Set arg0-arg2
        "mov x1, %[arg1]\n\t"          // TODO: Any way to use constraints like on x86?
        "mov x2, %[arg2]\n\t"
        // Use r18 last as we don't know whether it was used for arg0-arg2
        "mov x18, %[self]\n\t"         // Set the thread
        "blr %[code]\n\t"              // Call the stub
        "ldp x1, x2, [sp, #8]\n\t"     // Restore x1, x2
        "ldp x18, x30, [sp, #24]\n\t"  // Restore xSELF, xLR
        "add sp, sp, #48\n\t"          // Free stack space
        ".cfi_adjust_cfa_offset -48\n\t"
        "mov %[result], x0\n\t"        // Save the result
        : [result] "=r" (result)
          // Use the result from r0
        : [arg0] "0"(arg0), [arg1] "r"(arg1), [arg2] "r"(arg2), [code] "r"(code), [self] "r"(self)
        : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");  // clobber.
#elif defined(__x86_64__)
    // Note: Uses the native convention
    // TODO: Set the thread?
    __asm__ __volatile__(
        "pushq $0\n\t"                 // Push nullptr to terminate quick stack
        "pushq $0\n\t"                 // 16B alignment padding
        ".cfi_adjust_cfa_offset 16\n\t"
        "call *%%rax\n\t"              // Call the stub
        "addq $16, %%rsp"              // Pop nullptr and padding
        // ".cfi_adjust_cfa_offset -16\n\t"
        : "=a" (result)
          // Use the result from rax
        : "D"(arg0), "S"(arg1), "d"(arg2), "a"(code)
          // This places arg0 into rdi, arg1 into rsi, arg2 into rdx, and code into rax
        : "rcx", "rbp", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15");  // clobber all
    // TODO: Should we clobber the other registers?
#else
    LOG(WARNING) << "Was asked to invoke for an architecture I do not understand.";
    result = 0;
#endif
    // Pop transition.
    self->PopManagedStackFragment(fragment);
    return result;
  }
};


#if defined(__i386__) || defined(__x86_64__)
extern "C" void art_quick_memcpy(void);
#endif

TEST_F(StubTest, Memcpy) {
#if defined(__i386__) || defined(__x86_64__)
  Thread* self = Thread::Current();

  uint32_t orig[20];
  uint32_t trg[20];
  for (size_t i = 0; i < 20; ++i) {
    orig[i] = i;
    trg[i] = 0;
  }

  Invoke3(reinterpret_cast<size_t>(&trg[4]), reinterpret_cast<size_t>(&orig[4]),
          10 * sizeof(uint32_t), reinterpret_cast<uintptr_t>(&art_quick_memcpy), self);

  EXPECT_EQ(orig[0], trg[0]);

  for (size_t i = 1; i < 4; ++i) {
    EXPECT_NE(orig[i], trg[i]);
  }

  for (size_t i = 4; i < 14; ++i) {
    EXPECT_EQ(orig[i], trg[i]);
  }

  for (size_t i = 14; i < 20; ++i) {
    EXPECT_NE(orig[i], trg[i]);
  }

  // TODO: Test overlapping?

#else
  LOG(INFO) << "Skipping memcpy as I don't know how to do that on " << kRuntimeISA;
  // Force-print to std::cout so it's also outside the logcat.
  std::cout << "Skipping memcpy as I don't know how to do that on " << kRuntimeISA << std::endl;
#endif
}


#if defined(__i386__) || defined(__arm__)
extern "C" void art_quick_lock_object(void);
#endif

TEST_F(StubTest, LockObject) {
#if defined(__i386__) || defined(__arm__)
  Thread* self = Thread::Current();
  // Create an object
  ScopedObjectAccess soa(self);
  // garbage is created during ClassLinker::Init

  SirtRef<mirror::String> obj(soa.Self(),
                              mirror::String::AllocFromModifiedUtf8(soa.Self(), "hello, world!"));
  LockWord lock = obj->GetLockWord(false);
  LockWord::LockState old_state = lock.GetState();
  EXPECT_EQ(LockWord::LockState::kUnlocked, old_state);

  Invoke3(reinterpret_cast<size_t>(obj.get()), 0U, 0U,
          reinterpret_cast<uintptr_t>(&art_quick_lock_object), self);

  LockWord lock_after = obj->GetLockWord(false);
  LockWord::LockState new_state = lock_after.GetState();
  EXPECT_EQ(LockWord::LockState::kThinLocked, new_state);

  Invoke3(reinterpret_cast<size_t>(obj.get()), 0U, 0U,
          reinterpret_cast<uintptr_t>(&art_quick_lock_object), self);

  LockWord lock_after2 = obj->GetLockWord(false);
  LockWord::LockState new_state2 = lock_after2.GetState();
  EXPECT_EQ(LockWord::LockState::kThinLocked, new_state2);

  // TODO: Improve this test. Somehow force it to go to fat locked. But that needs another thread.

#else
  LOG(INFO) << "Skipping lock_object as I don't know how to do that on " << kRuntimeISA;
  // Force-print to std::cout so it's also outside the logcat.
  std::cout << "Skipping lock_object as I don't know how to do that on " << kRuntimeISA << std::endl;
#endif
}


#if defined(__i386__) || defined(__arm__) || defined(__aarch64__) || defined(__x86_64__)
extern "C" void art_quick_check_cast(void);
#endif

TEST_F(StubTest, CheckCast) {
#if defined(__i386__) || defined(__arm__) || defined(__aarch64__) || defined(__x86_64__)
  Thread* self = Thread::Current();
  // Find some classes.
  ScopedObjectAccess soa(self);
  // garbage is created during ClassLinker::Init

  SirtRef<mirror::Class> c(soa.Self(), class_linker_->FindSystemClass(soa.Self(),
                                                                          "[Ljava/lang/Object;"));
  SirtRef<mirror::Class> c2(soa.Self(), class_linker_->FindSystemClass(soa.Self(),
                                                                            "[Ljava/lang/String;"));

  EXPECT_FALSE(self->IsExceptionPending());

  Invoke3(reinterpret_cast<size_t>(c.get()), reinterpret_cast<size_t>(c.get()), 0U,
          reinterpret_cast<uintptr_t>(&art_quick_check_cast), self);

  EXPECT_FALSE(self->IsExceptionPending());

  Invoke3(reinterpret_cast<size_t>(c2.get()), reinterpret_cast<size_t>(c2.get()), 0U,
          reinterpret_cast<uintptr_t>(&art_quick_check_cast), self);

  EXPECT_FALSE(self->IsExceptionPending());

  Invoke3(reinterpret_cast<size_t>(c.get()), reinterpret_cast<size_t>(c2.get()), 0U,
          reinterpret_cast<uintptr_t>(&art_quick_check_cast), self);

  EXPECT_FALSE(self->IsExceptionPending());

  // TODO: Make the following work. But that would require correct managed frames.

  Invoke3(reinterpret_cast<size_t>(c2.get()), reinterpret_cast<size_t>(c.get()), 0U,
          reinterpret_cast<uintptr_t>(&art_quick_check_cast), self);

  EXPECT_TRUE(self->IsExceptionPending());
  self->ClearException();

#else
  LOG(INFO) << "Skipping check_cast as I don't know how to do that on " << kRuntimeISA;
  // Force-print to std::cout so it's also outside the logcat.
  std::cout << "Skipping check_cast as I don't know how to do that on " << kRuntimeISA << std::endl;
#endif
}


#if defined(__i386__) || defined(__arm__)
extern "C" void art_quick_aput_obj_with_null_and_bound_check(void);
// Do not check non-checked ones, we'd need handlers and stuff...
#endif

TEST_F(StubTest, APutObj) {
#if defined(__i386__) || defined(__arm__)
  Thread* self = Thread::Current();
  // Create an object
  ScopedObjectAccess soa(self);
  // garbage is created during ClassLinker::Init

  SirtRef<mirror::Class> c(soa.Self(), class_linker_->FindSystemClass(soa.Self(),
                                                                            "Ljava/lang/Object;"));
  SirtRef<mirror::Class> c2(soa.Self(), class_linker_->FindSystemClass(soa.Self(),
                                                                            "Ljava/lang/String;"));
  SirtRef<mirror::Class> ca(soa.Self(), class_linker_->FindSystemClass(soa.Self(),
                                                                            "[Ljava/lang/String;"));

  // Build a string array of size 1
  SirtRef<mirror::ObjectArray<mirror::Object> > array(soa.Self(),
            mirror::ObjectArray<mirror::Object>::Alloc(soa.Self(), ca.get(), 1));

  // Build a string -> should be assignable
  SirtRef<mirror::Object> str_obj(soa.Self(),
                                  mirror::String::AllocFromModifiedUtf8(soa.Self(), "hello, world!"));

  // Build a generic object -> should fail assigning
  SirtRef<mirror::Object> obj_obj(soa.Self(), c->AllocObject(soa.Self()));

  // Play with it...

  // 1) Success cases
  // 1.1) Assign str_obj to array[0]

  EXPECT_FALSE(self->IsExceptionPending());

  Invoke3(reinterpret_cast<size_t>(array.get()), 0U, reinterpret_cast<size_t>(str_obj.get()),
          reinterpret_cast<uintptr_t>(&art_quick_aput_obj_with_null_and_bound_check), self);

  EXPECT_FALSE(self->IsExceptionPending());

  // 1.2) Assign null to array[0]

  Invoke3(reinterpret_cast<size_t>(array.get()), 0U, reinterpret_cast<size_t>(nullptr),
          reinterpret_cast<uintptr_t>(&art_quick_aput_obj_with_null_and_bound_check), self);

  EXPECT_FALSE(self->IsExceptionPending());

  // TODO: Check _which_ exception is thrown. Then make 3) check that it's the right check order.

  // 2) Failure cases (str into str[])
  // 2.1) Array = null
  // TODO: Throwing NPE needs actual DEX code

//  Invoke3(reinterpret_cast<size_t>(nullptr), 0U, reinterpret_cast<size_t>(str_obj.get()),
//          reinterpret_cast<uintptr_t>(&art_quick_aput_obj_with_null_and_bound_check), self);
//
//  EXPECT_TRUE(self->IsExceptionPending());
//  self->ClearException();

  // 2.2) Index < 0

  Invoke3(reinterpret_cast<size_t>(array.get()), static_cast<size_t>(-1),
          reinterpret_cast<size_t>(str_obj.get()),
          reinterpret_cast<uintptr_t>(&art_quick_aput_obj_with_null_and_bound_check), self);

  EXPECT_TRUE(self->IsExceptionPending());
  self->ClearException();

  // 2.3) Index > 0

  Invoke3(reinterpret_cast<size_t>(array.get()), 1U, reinterpret_cast<size_t>(str_obj.get()),
          reinterpret_cast<uintptr_t>(&art_quick_aput_obj_with_null_and_bound_check), self);

  EXPECT_TRUE(self->IsExceptionPending());
  self->ClearException();

  // 3) Failure cases (obj into str[])

  Invoke3(reinterpret_cast<size_t>(array.get()), 0U, reinterpret_cast<size_t>(obj_obj.get()),
          reinterpret_cast<uintptr_t>(&art_quick_aput_obj_with_null_and_bound_check), self);

  EXPECT_TRUE(self->IsExceptionPending());
  self->ClearException();

  // Tests done.
#else
  LOG(INFO) << "Skipping aput_obj as I don't know how to do that on " << kRuntimeISA;
  // Force-print to std::cout so it's also outside the logcat.
  std::cout << "Skipping aput_obj as I don't know how to do that on " << kRuntimeISA << std::endl;
#endif
}


#if defined(__i386__) || defined(__arm__) || defined(__aarch64__) || defined(__x86_64__)
extern "C" void art_quick_alloc_object_rosalloc(void);
extern "C" void art_quick_alloc_object_resolved_rosalloc(void);
extern "C" void art_quick_alloc_object_initialized_rosalloc(void);
#endif

TEST_F(StubTest, AllocObject) {
  TEST_DISABLED_FOR_HEAP_REFERENCE_POISONING();

#if defined(__i386__) || defined(__arm__) || defined(__aarch64__) || defined(__x86_64__)
  // TODO: Check the "Unresolved" allocation stubs

  Thread* self = Thread::Current();
  // Create an object
  ScopedObjectAccess soa(self);
  // garbage is created during ClassLinker::Init

  SirtRef<mirror::Class> c(soa.Self(), class_linker_->FindSystemClass(soa.Self(),
                                                                      "Ljava/lang/Object;"));

  // Play with it...

  EXPECT_FALSE(self->IsExceptionPending());

  {
    // Use an arbitrary method from c to use as referrer
    size_t result = Invoke3(static_cast<size_t>(c->GetDexTypeIndex()),    // type_idx
                            reinterpret_cast<size_t>(c->GetVirtualMethod(0)),  // arbitrary
                            0U,
                            reinterpret_cast<uintptr_t>(&art_quick_alloc_object_rosalloc),
                            self);

    EXPECT_FALSE(self->IsExceptionPending());
    EXPECT_NE(reinterpret_cast<size_t>(nullptr), result);
    mirror::Object* obj = reinterpret_cast<mirror::Object*>(result);
    EXPECT_EQ(c.get(), obj->GetClass());
    VerifyObject(obj);
  }

  {
    // We can use nullptr in the second argument as we do not need a method here (not used in
    // resolved/initialized cases)
    size_t result = Invoke3(reinterpret_cast<size_t>(c.get()), reinterpret_cast<size_t>(nullptr), 0U,
                            reinterpret_cast<uintptr_t>(&art_quick_alloc_object_resolved_rosalloc),
                            self);

    EXPECT_FALSE(self->IsExceptionPending());
    EXPECT_NE(reinterpret_cast<size_t>(nullptr), result);
    mirror::Object* obj = reinterpret_cast<mirror::Object*>(result);
    EXPECT_EQ(c.get(), obj->GetClass());
    VerifyObject(obj);
  }

  {
    // We can use nullptr in the second argument as we do not need a method here (not used in
    // resolved/initialized cases)
    size_t result = Invoke3(reinterpret_cast<size_t>(c.get()), reinterpret_cast<size_t>(nullptr), 0U,
                            reinterpret_cast<uintptr_t>(&art_quick_alloc_object_initialized_rosalloc),
                            self);

    EXPECT_FALSE(self->IsExceptionPending());
    EXPECT_NE(reinterpret_cast<size_t>(nullptr), result);
    mirror::Object* obj = reinterpret_cast<mirror::Object*>(result);
    EXPECT_EQ(c.get(), obj->GetClass());
    VerifyObject(obj);
  }

  // Failure tests.

  // Out-of-memory.
  {
    Runtime::Current()->GetHeap()->SetIdealFootprint(1 * GB);

    // Array helps to fill memory faster.
    SirtRef<mirror::Class> ca(soa.Self(), class_linker_->FindSystemClass(soa.Self(),
                                                                         "[Ljava/lang/Object;"));
    std::vector<SirtRef<mirror::Object>*> sirt_refs;
    // Start allocating with 128K
    size_t length = 128 * KB / 4;
    while (length > 10) {
      SirtRef<mirror::Object>* ref = new SirtRef<mirror::Object>(soa.Self(),
                                              mirror::ObjectArray<mirror::Object>::Alloc(soa.Self(),
                                                                                         ca.get(),
                                                                                         length/4));
      if (self->IsExceptionPending() || ref->get() == nullptr) {
        self->ClearException();
        delete ref;

        // Try a smaller length
        length = length / 8;
        // Use at most half the reported free space.
        size_t mem = Runtime::Current()->GetHeap()->GetFreeMemory();
        if (length * 8 > mem) {
          length = mem / 8;
        }
      } else {
        sirt_refs.push_back(ref);
      }
    }
    LOG(DEBUG) << "Used " << sirt_refs.size() << " arrays to fill space.";

    // Allocate simple objects till it fails.
    while (!self->IsExceptionPending()) {
      SirtRef<mirror::Object>* ref = new SirtRef<mirror::Object>(soa.Self(),
                                                                 c->AllocObject(soa.Self()));
      if (!self->IsExceptionPending() && ref->get() != nullptr) {
        sirt_refs.push_back(ref);
      } else {
        delete ref;
      }
    }
    self->ClearException();

    size_t result = Invoke3(reinterpret_cast<size_t>(c.get()), reinterpret_cast<size_t>(nullptr), 0U,
                            reinterpret_cast<uintptr_t>(&art_quick_alloc_object_initialized_rosalloc),
                            self);

    EXPECT_TRUE(self->IsExceptionPending());
    self->ClearException();
    EXPECT_EQ(reinterpret_cast<size_t>(nullptr), result);

    // Release all the allocated objects.
    // Need to go backward to release SirtRef in the right order.
    auto it = sirt_refs.rbegin();
    auto end = sirt_refs.rend();
    for (; it != end; ++it) {
      delete *it;
    }
  }

  // Tests done.
#else
  LOG(INFO) << "Skipping alloc_object as I don't know how to do that on " << kRuntimeISA;
  // Force-print to std::cout so it's also outside the logcat.
  std::cout << "Skipping alloc_object as I don't know how to do that on " << kRuntimeISA << std::endl;
#endif
}


#if defined(__i386__) || defined(__arm__) || defined(__aarch64__) || defined(__x86_64__)
extern "C" void art_quick_alloc_array_rosalloc(void);
extern "C" void art_quick_alloc_array_resolved_rosalloc(void);
#endif

TEST_F(StubTest, AllocObjectArray) {
  TEST_DISABLED_FOR_HEAP_REFERENCE_POISONING();

#if defined(__i386__) || defined(__arm__) || defined(__aarch64__) || defined(__x86_64__)
  // TODO: Check the "Unresolved" allocation stubs

  Thread* self = Thread::Current();
  // Create an object
  ScopedObjectAccess soa(self);
  // garbage is created during ClassLinker::Init

  SirtRef<mirror::Class> c(soa.Self(), class_linker_->FindSystemClass(soa.Self(),
                                                                        "[Ljava/lang/Object;"));

  // Needed to have a linked method.
  SirtRef<mirror::Class> c_obj(soa.Self(), class_linker_->FindSystemClass(soa.Self(),
                                                                          "Ljava/lang/Object;"));

  // Play with it...

  EXPECT_FALSE(self->IsExceptionPending());
/*
 * For some reason this does not work, as the type_idx is artificial and outside what the
 * resolved types of c_obj allow...
 *
  {
    // Use an arbitrary method from c to use as referrer
    size_t result = Invoke3(static_cast<size_t>(c->GetDexTypeIndex()),    // type_idx
                            reinterpret_cast<size_t>(c_obj->GetVirtualMethod(0)),  // arbitrary
                            10U,
                            reinterpret_cast<uintptr_t>(&art_quick_alloc_array_rosalloc),
                            self);

    EXPECT_FALSE(self->IsExceptionPending());
    EXPECT_NE(reinterpret_cast<size_t>(nullptr), result);
    mirror::Array* obj = reinterpret_cast<mirror::Array*>(result);
    EXPECT_EQ(c.get(), obj->GetClass());
    VerifyObject(obj);
    EXPECT_EQ(obj->GetLength(), 10);
  }
*/
  {
    // We can use nullptr in the second argument as we do not need a method here (not used in
    // resolved/initialized cases)
    size_t result = Invoke3(reinterpret_cast<size_t>(c.get()), reinterpret_cast<size_t>(nullptr), 10U,
                            reinterpret_cast<uintptr_t>(&art_quick_alloc_array_resolved_rosalloc),
                            self);

    EXPECT_FALSE(self->IsExceptionPending());
    EXPECT_NE(reinterpret_cast<size_t>(nullptr), result);
    mirror::Object* obj = reinterpret_cast<mirror::Object*>(result);
    EXPECT_TRUE(obj->IsArrayInstance());
    EXPECT_TRUE(obj->IsObjectArray());
    EXPECT_EQ(c.get(), obj->GetClass());
    VerifyObject(obj);
    mirror::Array* array = reinterpret_cast<mirror::Array*>(result);
    EXPECT_EQ(array->GetLength(), 10);
  }

  // Failure tests.

  // Out-of-memory.
  {
    size_t result = Invoke3(reinterpret_cast<size_t>(c.get()), reinterpret_cast<size_t>(nullptr),
                            GB,  // that should fail...
                            reinterpret_cast<uintptr_t>(&art_quick_alloc_array_resolved_rosalloc),
                            self);

    EXPECT_TRUE(self->IsExceptionPending());
    self->ClearException();
    EXPECT_EQ(reinterpret_cast<size_t>(nullptr), result);
  }

  // Tests done.
#else
  LOG(INFO) << "Skipping alloc_array as I don't know how to do that on " << kRuntimeISA;
  // Force-print to std::cout so it's also outside the logcat.
  std::cout << "Skipping alloc_array as I don't know how to do that on " << kRuntimeISA << std::endl;
#endif
}

}  // namespace art
