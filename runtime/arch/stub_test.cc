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
#include "mirror/art_field-inl.h"
#include "mirror/string-inl.h"

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
      runtime_->SetInstructionSet(kRuntimeISA);
      for (int i = 0; i < Runtime::kLastCalleeSaveType; i++) {
        Runtime::CalleeSaveType type = Runtime::CalleeSaveType(i);
        if (!runtime_->HasCalleeSaveMethod(type)) {
          runtime_->SetCalleeSaveMethod(runtime_->CreateCalleeSaveMethod(type), type);
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

  // Helper function needed since TEST_F makes a new class.
  Thread::tls_ptr_sized_values* GetTlsPtr(Thread* self) {
    return &self->tlsPtr_;
  }

 public:
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
        "push {r9}\n\t"
        ".cfi_adjust_cfa_offset 4\n\t"
        "mov r9, #0\n\n"
        "str r9, [sp, #-8]!\n\t"   // Push nullptr to terminate stack, +8B padding so 16B aligned
        ".cfi_adjust_cfa_offset 8\n\t"
        "ldr r9, [sp, #8]\n\t"

        // Push everything on the stack, so we don't rely on the order. What a mess. :-(
        "sub sp, sp, #20\n\t"
        "str %[arg0], [sp]\n\t"
        "str %[arg1], [sp, #4]\n\t"
        "str %[arg2], [sp, #8]\n\t"
        "str %[code], [sp, #12]\n\t"
        "str %[self], [sp, #16]\n\t"
        "ldr r0, [sp]\n\t"
        "ldr r1, [sp, #4]\n\t"
        "ldr r2, [sp, #8]\n\t"
        "ldr r3, [sp, #12]\n\t"
        "ldr r9, [sp, #16]\n\t"
        "add sp, sp, #20\n\t"

        "blx r3\n\t"                // Call the stub
        "add sp, sp, #12\n\t"       // Pop nullptr and padding
        ".cfi_adjust_cfa_offset -12\n\t"
        "pop {r1-r12, lr}\n\t"      // Restore state
        ".cfi_adjust_cfa_offset -52\n\t"
        "mov %[result], r0\n\t"     // Save the result
        : [result] "=r" (result)
          // Use the result from r0
        : [arg0] "r"(arg0), [arg1] "r"(arg1), [arg2] "r"(arg2), [code] "r"(code), [self] "r"(self)
        : );  // clobber.
#elif defined(__aarch64__)
    __asm__ __volatile__(
        "sub sp, sp, #48\n\t"          // Reserve stack space, 16B aligned
        ".cfi_adjust_cfa_offset 48\n\t"
        "stp xzr, x1,  [sp]\n\t"        // nullptr(end of quick stack), x1
        "stp x2, x3,   [sp, #16]\n\t"   // Save x2, x3
        "stp x18, x30, [sp, #32]\n\t"   // Save x18(xSELF), xLR

        // Push everything on the stack, so we don't rely on the order. What a mess. :-(
        "sub sp, sp, #48\n\t"
        "str %[arg0], [sp]\n\t"
        "str %[arg1], [sp, #8]\n\t"
        "str %[arg2], [sp, #16]\n\t"
        "str %[code], [sp, #24]\n\t"
        "str %[self], [sp, #32]\n\t"
        "ldr x0, [sp]\n\t"
        "ldr x1, [sp, #8]\n\t"
        "ldr x2, [sp, #16]\n\t"
        "ldr x3, [sp, #24]\n\t"
        "ldr x18, [sp, #32]\n\t"
        "add sp, sp, #48\n\t"

        "blr x3\n\t"              // Call the stub
        "ldp x1, x2, [sp, #8]\n\t"     // Restore x1, x2
        "ldp x3, x18, [sp, #24]\n\t"   // Restore x3, xSELF
        "ldr x30, [sp, #40]\n\t"      // Restore xLR
        "add sp, sp, #48\n\t"          // Free stack space
        ".cfi_adjust_cfa_offset -48\n\t"

        "mov %[result], x0\n\t"        // Save the result
        : [result] "=r" (result)
          // Use the result from r0
        : [arg0] "0"(arg0), [arg1] "r"(arg1), [arg2] "r"(arg2), [code] "r"(code), [self] "r"(self)
        : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");  // clobber.
#elif defined(__x86_64__)
    // Note: Uses the native convention
    // TODO: Set the thread?
    __asm__ __volatile__(
        "pushq $0\n\t"                 // Push nullptr to terminate quick stack
        "pushq $0\n\t"                 // 16B alignment padding
        ".cfi_adjust_cfa_offset 16\n\t"
        "call *%%rax\n\t"              // Call the stub
        "addq $16, %%rsp\n\t"              // Pop nullptr and padding
        ".cfi_adjust_cfa_offset -16\n\t"
        : "=a" (result)
          // Use the result from rax
        : "D"(arg0), "S"(arg1), "d"(arg2), "a"(code)
          // This places arg0 into rdi, arg1 into rsi, arg2 into rdx, and code into rax
        : "rbx", "rcx", "rbp", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15");  // clobber all
    // TODO: Should we clobber the other registers?
#else
    LOG(WARNING) << "Was asked to invoke for an architecture I do not understand.";
    result = 0;
#endif
    // Pop transition.
    self->PopManagedStackFragment(fragment);
    return result;
  }

  // TODO: Set up a frame according to referrer's specs.
  size_t Invoke3WithReferrer(size_t arg0, size_t arg1, size_t arg2, uintptr_t code, Thread* self,
                             mirror::ArtMethod* referrer) {
    // Push a transition back into managed code onto the linked list in thread.
    ManagedStack fragment;
    self->PushManagedStackFragment(&fragment);

    size_t result;
#if defined(__i386__)
    // TODO: Set the thread?
    __asm__ __volatile__(
        "pushl %[referrer]\n\t"     // Store referrer
        "call *%%edi\n\t"           // Call the stub
        "addl $4, %%esp"            // Pop referrer
        : "=a" (result)
          // Use the result from eax
          : "a"(arg0), "c"(arg1), "d"(arg2), "D"(code), [referrer]"r"(referrer)
            // This places code into edi, arg0 into eax, arg1 into ecx, and arg2 into edx
            : );  // clobber.
    // TODO: Should we clobber the other registers? EBX gets clobbered by some of the stubs,
    //       but compilation fails when declaring that.
#elif defined(__arm__)
    __asm__ __volatile__(
        "push {r1-r12, lr}\n\t"     // Save state, 13*4B = 52B
        ".cfi_adjust_cfa_offset 52\n\t"
        "push {r9}\n\t"
        ".cfi_adjust_cfa_offset 4\n\t"
        "mov r9, %[referrer]\n\n"
        "str r9, [sp, #-8]!\n\t"   // Push referrer, +8B padding so 16B aligned
        ".cfi_adjust_cfa_offset 8\n\t"
        "ldr r9, [sp, #8]\n\t"

        // Push everything on the stack, so we don't rely on the order. What a mess. :-(
        "sub sp, sp, #20\n\t"
        "str %[arg0], [sp]\n\t"
        "str %[arg1], [sp, #4]\n\t"
        "str %[arg2], [sp, #8]\n\t"
        "str %[code], [sp, #12]\n\t"
        "str %[self], [sp, #16]\n\t"
        "ldr r0, [sp]\n\t"
        "ldr r1, [sp, #4]\n\t"
        "ldr r2, [sp, #8]\n\t"
        "ldr r3, [sp, #12]\n\t"
        "ldr r9, [sp, #16]\n\t"
        "add sp, sp, #20\n\t"

        "blx r3\n\t"                // Call the stub
        "add sp, sp, #12\n\t"       // Pop nullptr and padding
        ".cfi_adjust_cfa_offset -12\n\t"
        "pop {r1-r12, lr}\n\t"      // Restore state
        ".cfi_adjust_cfa_offset -52\n\t"
        "mov %[result], r0\n\t"     // Save the result
        : [result] "=r" (result)
          // Use the result from r0
        : [arg0] "r"(arg0), [arg1] "r"(arg1), [arg2] "r"(arg2), [code] "r"(code), [self] "r"(self),
          [referrer] "r"(referrer)
        : );  // clobber.
#elif defined(__aarch64__)
    __asm__ __volatile__(
        "sub sp, sp, #48\n\t"          // Reserve stack space, 16B aligned
        ".cfi_adjust_cfa_offset 48\n\t"
        "stp %[referrer], x1, [sp]\n\t"// referrer, x1
        "stp x2, x3,   [sp, #16]\n\t"   // Save x2, x3
        "stp x18, x30, [sp, #32]\n\t"   // Save x18(xSELF), xLR

        // Push everything on the stack, so we don't rely on the order. What a mess. :-(
        "sub sp, sp, #48\n\t"
        "str %[arg0], [sp]\n\t"
        "str %[arg1], [sp, #8]\n\t"
        "str %[arg2], [sp, #16]\n\t"
        "str %[code], [sp, #24]\n\t"
        "str %[self], [sp, #32]\n\t"
        "ldr x0, [sp]\n\t"
        "ldr x1, [sp, #8]\n\t"
        "ldr x2, [sp, #16]\n\t"
        "ldr x3, [sp, #24]\n\t"
        "ldr x18, [sp, #32]\n\t"
        "add sp, sp, #48\n\t"

        "blr x3\n\t"              // Call the stub
        "ldp x1, x2, [sp, #8]\n\t"     // Restore x1, x2
        "ldp x3, x18, [sp, #24]\n\t"   // Restore x3, xSELF
        "ldr x30, [sp, #40]\n\t"      // Restore xLR
        "add sp, sp, #48\n\t"          // Free stack space
        ".cfi_adjust_cfa_offset -48\n\t"

        "mov %[result], x0\n\t"        // Save the result
        : [result] "=r" (result)
          // Use the result from r0
        : [arg0] "0"(arg0), [arg1] "r"(arg1), [arg2] "r"(arg2), [code] "r"(code), [self] "r"(self),
          [referrer] "r"(referrer)
        : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");  // clobber.
#elif defined(__x86_64__)
    // Note: Uses the native convention
    // TODO: Set the thread?
    __asm__ __volatile__(
        "pushq %[referrer]\n\t"        // Push referrer
        "pushq (%%rsp)\n\t"             // & 16B alignment padding
        ".cfi_adjust_cfa_offset 16\n\t"
        "call *%%rax\n\t"              // Call the stub
        "addq $16, %%rsp\n\t"          // Pop nullptr and padding
        ".cfi_adjust_cfa_offset -16\n\t"
        : "=a" (result)
          // Use the result from rax
          : "D"(arg0), "S"(arg1), "d"(arg2), "a"(code), [referrer] "m"(referrer)
            // This places arg0 into rdi, arg1 into rsi, arg2 into rdx, and code into rax
            : "rbx", "rcx", "rbp", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15");  // clobber all
    // TODO: Should we clobber the other registers?
#else
    LOG(WARNING) << "Was asked to invoke for an architecture I do not understand.";
    result = 0;
#endif
    // Pop transition.
    self->PopManagedStackFragment(fragment);
    return result;
  }

  // Method with 32b arg0, 64b arg1
  size_t Invoke3UWithReferrer(size_t arg0, uint64_t arg1, uintptr_t code, Thread* self,
                              mirror::ArtMethod* referrer) {
#if defined(__x86_64__) || defined(__aarch64__)
    // Just pass through.
    return Invoke3WithReferrer(arg0, arg1, 0U, code, self, referrer);
#else
    // Need to split up arguments.
    uint32_t lower = static_cast<uint32_t>(arg1 & 0xFFFFFFFF);
    uint32_t upper = static_cast<uint32_t>((arg1 >> 32) & 0xFFFFFFFF);

    return Invoke3WithReferrer(arg0, lower, upper, code, self, referrer);
#endif
  }

  // Method with 32b arg0, 32b arg1, 64b arg2
  size_t Invoke3UUWithReferrer(uint32_t arg0, uint32_t arg1, uint64_t arg2, uintptr_t code,
                               Thread* self, mirror::ArtMethod* referrer) {
#if defined(__x86_64__) || defined(__aarch64__)
    // Just pass through.
    return Invoke3WithReferrer(arg0, arg1, arg2, code, self, referrer);
#else
    // TODO: Needs 4-param invoke.
    return 0;
#endif
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

#if defined(__i386__) || defined(__arm__) || defined(__aarch64__) || defined(__x86_64__)
extern "C" void art_quick_lock_object(void);
#endif

TEST_F(StubTest, LockObject) {
#if defined(__i386__) || defined(__arm__) || defined(__aarch64__) || defined(__x86_64__)
  static constexpr size_t kThinLockLoops = 100;

  Thread* self = Thread::Current();
  // Create an object
  ScopedObjectAccess soa(self);
  // garbage is created during ClassLinker::Init

  StackHandleScope<2> hs(soa.Self());
  Handle<mirror::String> obj(
      hs.NewHandle(mirror::String::AllocFromModifiedUtf8(soa.Self(), "hello, world!")));
  LockWord lock = obj->GetLockWord(false);
  LockWord::LockState old_state = lock.GetState();
  EXPECT_EQ(LockWord::LockState::kUnlocked, old_state);

  Invoke3(reinterpret_cast<size_t>(obj.Get()), 0U, 0U,
          reinterpret_cast<uintptr_t>(&art_quick_lock_object), self);

  LockWord lock_after = obj->GetLockWord(false);
  LockWord::LockState new_state = lock_after.GetState();
  EXPECT_EQ(LockWord::LockState::kThinLocked, new_state);
  EXPECT_EQ(lock_after.ThinLockCount(), 0U);  // Thin lock starts count at zero

  for (size_t i = 1; i < kThinLockLoops; ++i) {
    Invoke3(reinterpret_cast<size_t>(obj.Get()), 0U, 0U,
              reinterpret_cast<uintptr_t>(&art_quick_lock_object), self);

    // Check we're at lock count i

    LockWord l_inc = obj->GetLockWord(false);
    LockWord::LockState l_inc_state = l_inc.GetState();
    EXPECT_EQ(LockWord::LockState::kThinLocked, l_inc_state);
    EXPECT_EQ(l_inc.ThinLockCount(), i);
  }

  // Force a fat lock by running identity hashcode to fill up lock word.
  Handle<mirror::String> obj2(hs.NewHandle(
      mirror::String::AllocFromModifiedUtf8(soa.Self(), "hello, world!")));

  obj2->IdentityHashCode();

  Invoke3(reinterpret_cast<size_t>(obj2.Get()), 0U, 0U,
          reinterpret_cast<uintptr_t>(&art_quick_lock_object), self);

  LockWord lock_after2 = obj2->GetLockWord(false);
  LockWord::LockState new_state2 = lock_after2.GetState();
  EXPECT_EQ(LockWord::LockState::kFatLocked, new_state2);
  EXPECT_NE(lock_after2.FatLockMonitor(), static_cast<Monitor*>(nullptr));

  // Test done.
#else
  LOG(INFO) << "Skipping lock_object as I don't know how to do that on " << kRuntimeISA;
  // Force-print to std::cout so it's also outside the logcat.
  std::cout << "Skipping lock_object as I don't know how to do that on " << kRuntimeISA << std::endl;
#endif
}


class RandGen {
 public:
  explicit RandGen(uint32_t seed) : val_(seed) {}

  uint32_t next() {
    val_ = val_ * 48271 % 2147483647 + 13;
    return val_;
  }

  uint32_t val_;
};


#if defined(__i386__) || defined(__arm__) || defined(__aarch64__) || defined(__x86_64__)
extern "C" void art_quick_lock_object(void);
extern "C" void art_quick_unlock_object(void);
#endif

// NO_THREAD_SAFETY_ANALYSIS as we do not want to grab exclusive mutator lock for MonitorInfo.
static void TestUnlockObject(StubTest* test) NO_THREAD_SAFETY_ANALYSIS {
#if defined(__i386__) || defined(__arm__) || defined(__aarch64__) || defined(__x86_64__)
  static constexpr size_t kThinLockLoops = 100;

  Thread* self = Thread::Current();
  // Create an object
  ScopedObjectAccess soa(self);
  // garbage is created during ClassLinker::Init
  static constexpr size_t kNumberOfLocks = 10;  // Number of objects = lock
  StackHandleScope<kNumberOfLocks + 1> hs(self);
  Handle<mirror::String> obj(
      hs.NewHandle(mirror::String::AllocFromModifiedUtf8(soa.Self(), "hello, world!")));
  LockWord lock = obj->GetLockWord(false);
  LockWord::LockState old_state = lock.GetState();
  EXPECT_EQ(LockWord::LockState::kUnlocked, old_state);

  test->Invoke3(reinterpret_cast<size_t>(obj.Get()), 0U, 0U,
                reinterpret_cast<uintptr_t>(&art_quick_unlock_object), self);
  // This should be an illegal monitor state.
  EXPECT_TRUE(self->IsExceptionPending());
  self->ClearException();

  LockWord lock_after = obj->GetLockWord(false);
  LockWord::LockState new_state = lock_after.GetState();
  EXPECT_EQ(LockWord::LockState::kUnlocked, new_state);

  test->Invoke3(reinterpret_cast<size_t>(obj.Get()), 0U, 0U,
                reinterpret_cast<uintptr_t>(&art_quick_lock_object), self);

  LockWord lock_after2 = obj->GetLockWord(false);
  LockWord::LockState new_state2 = lock_after2.GetState();
  EXPECT_EQ(LockWord::LockState::kThinLocked, new_state2);

  test->Invoke3(reinterpret_cast<size_t>(obj.Get()), 0U, 0U,
                reinterpret_cast<uintptr_t>(&art_quick_unlock_object), self);

  LockWord lock_after3 = obj->GetLockWord(false);
  LockWord::LockState new_state3 = lock_after3.GetState();
  EXPECT_EQ(LockWord::LockState::kUnlocked, new_state3);

  // Stress test:
  // Keep a number of objects and their locks in flight. Randomly lock or unlock one of them in
  // each step.

  RandGen r(0x1234);

  constexpr size_t kIterations = 10000;  // Number of iterations
  constexpr size_t kMoveToFat = 1000;     // Chance of 1:kMoveFat to make a lock fat.

  size_t counts[kNumberOfLocks];
  bool fat[kNumberOfLocks];  // Whether a lock should be thin or fat.
  Handle<mirror::String> objects[kNumberOfLocks];

  // Initialize = allocate.
  for (size_t i = 0; i < kNumberOfLocks; ++i) {
    counts[i] = 0;
    fat[i] = false;
    objects[i] = hs.NewHandle(mirror::String::AllocFromModifiedUtf8(soa.Self(), ""));
  }

  for (size_t i = 0; i < kIterations; ++i) {
    // Select which lock to update.
    size_t index = r.next() % kNumberOfLocks;

    // Make lock fat?
    if (!fat[index] && (r.next() % kMoveToFat == 0)) {
      fat[index] = true;
      objects[index]->IdentityHashCode();

      LockWord lock_iter = objects[index]->GetLockWord(false);
      LockWord::LockState iter_state = lock_iter.GetState();
      if (counts[index] == 0) {
        EXPECT_EQ(LockWord::LockState::kHashCode, iter_state);
      } else {
        EXPECT_EQ(LockWord::LockState::kFatLocked, iter_state);
      }
    } else {
      bool lock;  // Whether to lock or unlock in this step.
      if (counts[index] == 0) {
        lock = true;
      } else if (counts[index] == kThinLockLoops) {
        lock = false;
      } else {
        // Randomly.
        lock = r.next() % 2 == 0;
      }

      if (lock) {
        test->Invoke3(reinterpret_cast<size_t>(objects[index].Get()), 0U, 0U,
                       reinterpret_cast<uintptr_t>(&art_quick_lock_object), self);
        counts[index]++;
      } else {
        test->Invoke3(reinterpret_cast<size_t>(objects[index].Get()), 0U, 0U,
                      reinterpret_cast<uintptr_t>(&art_quick_unlock_object), self);
        counts[index]--;
      }

      EXPECT_FALSE(self->IsExceptionPending());

      // Check the new state.
      LockWord lock_iter = objects[index]->GetLockWord(true);
      LockWord::LockState iter_state = lock_iter.GetState();
      if (fat[index]) {
        // Abuse MonitorInfo.
        EXPECT_EQ(LockWord::LockState::kFatLocked, iter_state) << index;
        MonitorInfo info(objects[index].Get());
        EXPECT_EQ(counts[index], info.entry_count_) << index;
      } else {
        if (counts[index] > 0) {
          EXPECT_EQ(LockWord::LockState::kThinLocked, iter_state);
          EXPECT_EQ(counts[index] - 1, lock_iter.ThinLockCount());
        } else {
          EXPECT_EQ(LockWord::LockState::kUnlocked, iter_state);
        }
      }
    }
  }

  // Unlock the remaining count times and then check it's unlocked. Then deallocate.
  // Go reverse order to correctly handle Handles.
  for (size_t i = 0; i < kNumberOfLocks; ++i) {
    size_t index = kNumberOfLocks - 1 - i;
    size_t count = counts[index];
    while (count > 0) {
      test->Invoke3(reinterpret_cast<size_t>(objects[index].Get()), 0U, 0U,
                    reinterpret_cast<uintptr_t>(&art_quick_unlock_object), self);
      count--;
    }

    LockWord lock_after4 = objects[index]->GetLockWord(false);
    LockWord::LockState new_state4 = lock_after4.GetState();
    EXPECT_TRUE(LockWord::LockState::kUnlocked == new_state4
                || LockWord::LockState::kFatLocked == new_state4);
  }

  // Test done.
#else
  LOG(INFO) << "Skipping unlock_object as I don't know how to do that on " << kRuntimeISA;
  // Force-print to std::cout so it's also outside the logcat.
  std::cout << "Skipping unlock_object as I don't know how to do that on " << kRuntimeISA << std::endl;
#endif
}

TEST_F(StubTest, UnlockObject) {
  TestUnlockObject(this);
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

  StackHandleScope<2> hs(soa.Self());
  Handle<mirror::Class> c(
      hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), "[Ljava/lang/Object;")));
  Handle<mirror::Class> c2(
      hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), "[Ljava/lang/String;")));

  EXPECT_FALSE(self->IsExceptionPending());

  Invoke3(reinterpret_cast<size_t>(c.Get()), reinterpret_cast<size_t>(c.Get()), 0U,
          reinterpret_cast<uintptr_t>(&art_quick_check_cast), self);

  EXPECT_FALSE(self->IsExceptionPending());

  Invoke3(reinterpret_cast<size_t>(c2.Get()), reinterpret_cast<size_t>(c2.Get()), 0U,
          reinterpret_cast<uintptr_t>(&art_quick_check_cast), self);

  EXPECT_FALSE(self->IsExceptionPending());

  Invoke3(reinterpret_cast<size_t>(c.Get()), reinterpret_cast<size_t>(c2.Get()), 0U,
          reinterpret_cast<uintptr_t>(&art_quick_check_cast), self);

  EXPECT_FALSE(self->IsExceptionPending());

  // TODO: Make the following work. But that would require correct managed frames.

  Invoke3(reinterpret_cast<size_t>(c2.Get()), reinterpret_cast<size_t>(c.Get()), 0U,
          reinterpret_cast<uintptr_t>(&art_quick_check_cast), self);

  EXPECT_TRUE(self->IsExceptionPending());
  self->ClearException();

#else
  LOG(INFO) << "Skipping check_cast as I don't know how to do that on " << kRuntimeISA;
  // Force-print to std::cout so it's also outside the logcat.
  std::cout << "Skipping check_cast as I don't know how to do that on " << kRuntimeISA << std::endl;
#endif
}


#if defined(__i386__) || defined(__arm__) || defined(__aarch64__) || defined(__x86_64__)
extern "C" void art_quick_aput_obj_with_null_and_bound_check(void);
// Do not check non-checked ones, we'd need handlers and stuff...
#endif

TEST_F(StubTest, APutObj) {
  TEST_DISABLED_FOR_HEAP_REFERENCE_POISONING();

#if defined(__i386__) || defined(__arm__) || defined(__aarch64__) || defined(__x86_64__)
  Thread* self = Thread::Current();
  // Create an object
  ScopedObjectAccess soa(self);
  // garbage is created during ClassLinker::Init

  StackHandleScope<5> hs(soa.Self());
  Handle<mirror::Class> c(
      hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/Object;")));
  Handle<mirror::Class> ca(
      hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), "[Ljava/lang/String;")));

  // Build a string array of size 1
  Handle<mirror::ObjectArray<mirror::Object>> array(
      hs.NewHandle(mirror::ObjectArray<mirror::Object>::Alloc(soa.Self(), ca.Get(), 10)));

  // Build a string -> should be assignable
  Handle<mirror::String> str_obj(
      hs.NewHandle(mirror::String::AllocFromModifiedUtf8(soa.Self(), "hello, world!")));

  // Build a generic object -> should fail assigning
  Handle<mirror::Object> obj_obj(hs.NewHandle(c->AllocObject(soa.Self())));

  // Play with it...

  // 1) Success cases
  // 1.1) Assign str_obj to array[0..3]

  EXPECT_FALSE(self->IsExceptionPending());

  Invoke3(reinterpret_cast<size_t>(array.Get()), 0U, reinterpret_cast<size_t>(str_obj.Get()),
          reinterpret_cast<uintptr_t>(&art_quick_aput_obj_with_null_and_bound_check), self);

  EXPECT_FALSE(self->IsExceptionPending());
  EXPECT_EQ(str_obj.Get(), array->Get(0));

  Invoke3(reinterpret_cast<size_t>(array.Get()), 1U, reinterpret_cast<size_t>(str_obj.Get()),
          reinterpret_cast<uintptr_t>(&art_quick_aput_obj_with_null_and_bound_check), self);

  EXPECT_FALSE(self->IsExceptionPending());
  EXPECT_EQ(str_obj.Get(), array->Get(1));

  Invoke3(reinterpret_cast<size_t>(array.Get()), 2U, reinterpret_cast<size_t>(str_obj.Get()),
          reinterpret_cast<uintptr_t>(&art_quick_aput_obj_with_null_and_bound_check), self);

  EXPECT_FALSE(self->IsExceptionPending());
  EXPECT_EQ(str_obj.Get(), array->Get(2));

  Invoke3(reinterpret_cast<size_t>(array.Get()), 3U, reinterpret_cast<size_t>(str_obj.Get()),
          reinterpret_cast<uintptr_t>(&art_quick_aput_obj_with_null_and_bound_check), self);

  EXPECT_FALSE(self->IsExceptionPending());
  EXPECT_EQ(str_obj.Get(), array->Get(3));

  // 1.2) Assign null to array[0..3]

  Invoke3(reinterpret_cast<size_t>(array.Get()), 0U, reinterpret_cast<size_t>(nullptr),
          reinterpret_cast<uintptr_t>(&art_quick_aput_obj_with_null_and_bound_check), self);

  EXPECT_FALSE(self->IsExceptionPending());
  EXPECT_EQ(nullptr, array->Get(0));

  Invoke3(reinterpret_cast<size_t>(array.Get()), 1U, reinterpret_cast<size_t>(nullptr),
          reinterpret_cast<uintptr_t>(&art_quick_aput_obj_with_null_and_bound_check), self);

  EXPECT_FALSE(self->IsExceptionPending());
  EXPECT_EQ(nullptr, array->Get(1));

  Invoke3(reinterpret_cast<size_t>(array.Get()), 2U, reinterpret_cast<size_t>(nullptr),
          reinterpret_cast<uintptr_t>(&art_quick_aput_obj_with_null_and_bound_check), self);

  EXPECT_FALSE(self->IsExceptionPending());
  EXPECT_EQ(nullptr, array->Get(2));

  Invoke3(reinterpret_cast<size_t>(array.Get()), 3U, reinterpret_cast<size_t>(nullptr),
          reinterpret_cast<uintptr_t>(&art_quick_aput_obj_with_null_and_bound_check), self);

  EXPECT_FALSE(self->IsExceptionPending());
  EXPECT_EQ(nullptr, array->Get(3));

  // TODO: Check _which_ exception is thrown. Then make 3) check that it's the right check order.

  // 2) Failure cases (str into str[])
  // 2.1) Array = null
  // TODO: Throwing NPE needs actual DEX code

//  Invoke3(reinterpret_cast<size_t>(nullptr), 0U, reinterpret_cast<size_t>(str_obj.Get()),
//          reinterpret_cast<uintptr_t>(&art_quick_aput_obj_with_null_and_bound_check), self);
//
//  EXPECT_TRUE(self->IsExceptionPending());
//  self->ClearException();

  // 2.2) Index < 0

  Invoke3(reinterpret_cast<size_t>(array.Get()), static_cast<size_t>(-1),
          reinterpret_cast<size_t>(str_obj.Get()),
          reinterpret_cast<uintptr_t>(&art_quick_aput_obj_with_null_and_bound_check), self);

  EXPECT_TRUE(self->IsExceptionPending());
  self->ClearException();

  // 2.3) Index > 0

  Invoke3(reinterpret_cast<size_t>(array.Get()), 10U, reinterpret_cast<size_t>(str_obj.Get()),
          reinterpret_cast<uintptr_t>(&art_quick_aput_obj_with_null_and_bound_check), self);

  EXPECT_TRUE(self->IsExceptionPending());
  self->ClearException();

  // 3) Failure cases (obj into str[])

  Invoke3(reinterpret_cast<size_t>(array.Get()), 0U, reinterpret_cast<size_t>(obj_obj.Get()),
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

TEST_F(StubTest, AllocObject) {
  TEST_DISABLED_FOR_HEAP_REFERENCE_POISONING();

#if defined(__i386__) || defined(__arm__) || defined(__aarch64__) || defined(__x86_64__)
  // TODO: Check the "Unresolved" allocation stubs

  Thread* self = Thread::Current();
  // Create an object
  ScopedObjectAccess soa(self);
  // garbage is created during ClassLinker::Init

  StackHandleScope<2> hs(soa.Self());
  Handle<mirror::Class> c(
      hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/Object;")));

  // Play with it...

  EXPECT_FALSE(self->IsExceptionPending());
  {
    // Use an arbitrary method from c to use as referrer
    size_t result = Invoke3(static_cast<size_t>(c->GetDexTypeIndex()),    // type_idx
                            reinterpret_cast<size_t>(c->GetVirtualMethod(0)),  // arbitrary
                            0U,
                            reinterpret_cast<uintptr_t>(GetTlsPtr(self)->quick_entrypoints.pAllocObject),
                            self);

    EXPECT_FALSE(self->IsExceptionPending());
    EXPECT_NE(reinterpret_cast<size_t>(nullptr), result);
    mirror::Object* obj = reinterpret_cast<mirror::Object*>(result);
    EXPECT_EQ(c.Get(), obj->GetClass());
    VerifyObject(obj);
  }

  {
    // We can use nullptr in the second argument as we do not need a method here (not used in
    // resolved/initialized cases)
    size_t result = Invoke3(reinterpret_cast<size_t>(c.Get()), reinterpret_cast<size_t>(nullptr), 0U,
                            reinterpret_cast<uintptr_t>(GetTlsPtr(self)->quick_entrypoints.pAllocObjectResolved),
                            self);

    EXPECT_FALSE(self->IsExceptionPending());
    EXPECT_NE(reinterpret_cast<size_t>(nullptr), result);
    mirror::Object* obj = reinterpret_cast<mirror::Object*>(result);
    EXPECT_EQ(c.Get(), obj->GetClass());
    VerifyObject(obj);
  }

  {
    // We can use nullptr in the second argument as we do not need a method here (not used in
    // resolved/initialized cases)
    size_t result = Invoke3(reinterpret_cast<size_t>(c.Get()), reinterpret_cast<size_t>(nullptr), 0U,
                            reinterpret_cast<uintptr_t>(GetTlsPtr(self)->quick_entrypoints.pAllocObjectInitialized),
                            self);

    EXPECT_FALSE(self->IsExceptionPending());
    EXPECT_NE(reinterpret_cast<size_t>(nullptr), result);
    mirror::Object* obj = reinterpret_cast<mirror::Object*>(result);
    EXPECT_EQ(c.Get(), obj->GetClass());
    VerifyObject(obj);
  }

  // Failure tests.

  // Out-of-memory.
  {
    Runtime::Current()->GetHeap()->SetIdealFootprint(1 * GB);

    // Array helps to fill memory faster.
    Handle<mirror::Class> ca(
        hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), "[Ljava/lang/Object;")));

    // Use arbitrary large amount for now.
    static const size_t kMaxHandles = 1000000;
    UniquePtr<StackHandleScope<kMaxHandles> > hsp(new StackHandleScope<kMaxHandles>(self));

    std::vector<Handle<mirror::Object>> handles;
    // Start allocating with 128K
    size_t length = 128 * KB / 4;
    while (length > 10) {
      Handle<mirror::Object> h(hsp->NewHandle<mirror::Object>(
          mirror::ObjectArray<mirror::Object>::Alloc(soa.Self(), ca.Get(), length / 4)));
      if (self->IsExceptionPending() || h.Get() == nullptr) {
        self->ClearException();

        // Try a smaller length
        length = length / 8;
        // Use at most half the reported free space.
        size_t mem = Runtime::Current()->GetHeap()->GetFreeMemory();
        if (length * 8 > mem) {
          length = mem / 8;
        }
      } else {
        handles.push_back(h);
      }
    }
    LOG(INFO) << "Used " << handles.size() << " arrays to fill space.";

    // Allocate simple objects till it fails.
    while (!self->IsExceptionPending()) {
      Handle<mirror::Object> h = hsp->NewHandle(c->AllocObject(soa.Self()));
      if (!self->IsExceptionPending() && h.Get() != nullptr) {
        handles.push_back(h);
      }
    }
    self->ClearException();

    size_t result = Invoke3(reinterpret_cast<size_t>(c.Get()), reinterpret_cast<size_t>(nullptr), 0U,
                            reinterpret_cast<uintptr_t>(GetTlsPtr(self)->quick_entrypoints.pAllocObjectInitialized),
                            self);
    EXPECT_TRUE(self->IsExceptionPending());
    self->ClearException();
    EXPECT_EQ(reinterpret_cast<size_t>(nullptr), result);
  }

  // Tests done.
#else
  LOG(INFO) << "Skipping alloc_object as I don't know how to do that on " << kRuntimeISA;
  // Force-print to std::cout so it's also outside the logcat.
  std::cout << "Skipping alloc_object as I don't know how to do that on " << kRuntimeISA << std::endl;
#endif
}

TEST_F(StubTest, AllocObjectArray) {
  TEST_DISABLED_FOR_HEAP_REFERENCE_POISONING();

#if defined(__i386__) || defined(__arm__) || defined(__aarch64__) || defined(__x86_64__)
  // TODO: Check the "Unresolved" allocation stubs

  Thread* self = Thread::Current();
  // Create an object
  ScopedObjectAccess soa(self);
  // garbage is created during ClassLinker::Init

  StackHandleScope<2> hs(self);
  Handle<mirror::Class> c(
      hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), "[Ljava/lang/Object;")));

  // Needed to have a linked method.
  Handle<mirror::Class> c_obj(
      hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), "Ljava/lang/Object;")));

  // Play with it...

  EXPECT_FALSE(self->IsExceptionPending());

  // For some reason this does not work, as the type_idx is artificial and outside what the
  // resolved types of c_obj allow...

  if (false) {
    // Use an arbitrary method from c to use as referrer
    size_t result = Invoke3(static_cast<size_t>(c->GetDexTypeIndex()),    // type_idx
                            reinterpret_cast<size_t>(c_obj->GetVirtualMethod(0)),  // arbitrary
                            10U,
                            reinterpret_cast<uintptr_t>(GetTlsPtr(self)->quick_entrypoints.pAllocArray),
                            self);

    EXPECT_FALSE(self->IsExceptionPending());
    EXPECT_NE(reinterpret_cast<size_t>(nullptr), result);
    mirror::Array* obj = reinterpret_cast<mirror::Array*>(result);
    EXPECT_EQ(c.Get(), obj->GetClass());
    VerifyObject(obj);
    EXPECT_EQ(obj->GetLength(), 10);
  }

  {
    // We can use nullptr in the second argument as we do not need a method here (not used in
    // resolved/initialized cases)
    size_t result = Invoke3(reinterpret_cast<size_t>(c.Get()), reinterpret_cast<size_t>(nullptr), 10U,
                            reinterpret_cast<uintptr_t>(GetTlsPtr(self)->quick_entrypoints.pAllocArrayResolved),
                            self);
    EXPECT_FALSE(self->IsExceptionPending()) << PrettyTypeOf(self->GetException(nullptr));
    EXPECT_NE(reinterpret_cast<size_t>(nullptr), result);
    mirror::Object* obj = reinterpret_cast<mirror::Object*>(result);
    EXPECT_TRUE(obj->IsArrayInstance());
    EXPECT_TRUE(obj->IsObjectArray());
    EXPECT_EQ(c.Get(), obj->GetClass());
    VerifyObject(obj);
    mirror::Array* array = reinterpret_cast<mirror::Array*>(result);
    EXPECT_EQ(array->GetLength(), 10);
  }

  // Failure tests.

  // Out-of-memory.
  {
    size_t result = Invoke3(reinterpret_cast<size_t>(c.Get()), reinterpret_cast<size_t>(nullptr),
                            GB,  // that should fail...
                            reinterpret_cast<uintptr_t>(GetTlsPtr(self)->quick_entrypoints.pAllocArrayResolved),
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


#if defined(__i386__) || defined(__arm__) || defined(__aarch64__) || defined(__x86_64__)
extern "C" void art_quick_string_compareto(void);
#endif

TEST_F(StubTest, StringCompareTo) {
  TEST_DISABLED_FOR_HEAP_REFERENCE_POISONING();

#if defined(__i386__) || defined(__arm__) || defined(__aarch64__) || defined(__x86_64__)
  // TODO: Check the "Unresolved" allocation stubs

  Thread* self = Thread::Current();
  ScopedObjectAccess soa(self);
  // garbage is created during ClassLinker::Init

  // Create some strings
  // Use array so we can index into it and use a matrix for expected results
  // Setup: The first half is standard. The second half uses a non-zero offset.
  // TODO: Shared backing arrays.
  static constexpr size_t kBaseStringCount  = 7;
  const char* c[kBaseStringCount] = { "", "", "a", "aa", "ab", "aac", "aac" , };

  static constexpr size_t kStringCount = 2 * kBaseStringCount;

  StackHandleScope<kStringCount> hs(self);
  Handle<mirror::String> s[kStringCount];

  for (size_t i = 0; i < kBaseStringCount; ++i) {
    s[i] = hs.NewHandle(mirror::String::AllocFromModifiedUtf8(soa.Self(), c[i]));
  }

  RandGen r(0x1234);

  for (size_t i = kBaseStringCount; i < kStringCount; ++i) {
    s[i] = hs.NewHandle(mirror::String::AllocFromModifiedUtf8(soa.Self(), c[i - kBaseStringCount]));
    int32_t length = s[i]->GetLength();
    if (length > 1) {
      // Set a random offset and length.
      int32_t new_offset = 1 + (r.next() % (length - 1));
      int32_t rest = length - new_offset - 1;
      int32_t new_length = 1 + (rest > 0 ? r.next() % rest : 0);

      s[i]->SetField32<false>(mirror::String::CountOffset(), new_length);
      s[i]->SetField32<false>(mirror::String::OffsetOffset(), new_offset);
    }
  }

  // TODO: wide characters

  // Matrix of expectations. First component is first parameter. Note we only check against the
  // sign, not the value. As we are testing random offsets, we need to compute this and need to
  // rely on String::CompareTo being correct.
  int32_t expected[kStringCount][kStringCount];
  for (size_t x = 0; x < kStringCount; ++x) {
    for (size_t y = 0; y < kStringCount; ++y) {
      expected[x][y] = s[x]->CompareTo(s[y].Get());
    }
  }

  // Play with it...

  for (size_t x = 0; x < kStringCount; ++x) {
    for (size_t y = 0; y < kStringCount; ++y) {
      // Test string_compareto x y
      size_t result = Invoke3(reinterpret_cast<size_t>(s[x].Get()),
                              reinterpret_cast<size_t>(s[y].Get()), 0U,
                              reinterpret_cast<uintptr_t>(&art_quick_string_compareto), self);

      EXPECT_FALSE(self->IsExceptionPending());

      // The result is a 32b signed integer
      union {
        size_t r;
        int32_t i;
      } conv;
      conv.r = result;
      int32_t e = expected[x][y];
      EXPECT_TRUE(e == 0 ? conv.i == 0 : true) << "x=" << c[x] << " y=" << c[y] << " res=" <<
          conv.r;
      EXPECT_TRUE(e < 0 ? conv.i < 0 : true)   << "x=" << c[x] << " y="  << c[y] << " res=" <<
          conv.r;
      EXPECT_TRUE(e > 0 ? conv.i > 0 : true)   << "x=" << c[x] << " y=" << c[y] << " res=" <<
          conv.r;
    }
  }

  // TODO: Deallocate things.

  // Tests done.
#else
  LOG(INFO) << "Skipping string_compareto as I don't know how to do that on " << kRuntimeISA;
  // Force-print to std::cout so it's also outside the logcat.
  std::cout << "Skipping string_compareto as I don't know how to do that on " << kRuntimeISA <<
      std::endl;
#endif
}


#if defined(__i386__) || defined(__arm__) || defined(__aarch64__) || defined(__x86_64__)
extern "C" void art_quick_set32_static(void);
extern "C" void art_quick_get32_static(void);
#endif

static void GetSet32Static(Handle<mirror::Object>* obj, Handle<mirror::ArtField>* f, Thread* self,
                           mirror::ArtMethod* referrer, StubTest* test)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
#if defined(__i386__) || defined(__arm__) || defined(__aarch64__) || defined(__x86_64__)
  constexpr size_t num_values = 7;
  uint32_t values[num_values] = { 0, 1, 2, 255, 32768, 1000000, 0xFFFFFFFF };

  for (size_t i = 0; i < num_values; ++i) {
    test->Invoke3WithReferrer(static_cast<size_t>((*f)->GetDexFieldIndex()),
                              static_cast<size_t>(values[i]),
                              0U,
                              reinterpret_cast<uintptr_t>(&art_quick_set32_static),
                              self,
                              referrer);

    size_t res = test->Invoke3WithReferrer(static_cast<size_t>((*f)->GetDexFieldIndex()),
                                           0U, 0U,
                                           reinterpret_cast<uintptr_t>(&art_quick_get32_static),
                                           self,
                                           referrer);

    EXPECT_EQ(res, values[i]) << "Iteration " << i;
  }
#else
  LOG(INFO) << "Skipping set32static as I don't know how to do that on " << kRuntimeISA;
  // Force-print to std::cout so it's also outside the logcat.
  std::cout << "Skipping set32static as I don't know how to do that on " << kRuntimeISA << std::endl;
#endif
}


#if defined(__i386__) || defined(__arm__) || defined(__aarch64__) || defined(__x86_64__)
extern "C" void art_quick_set32_instance(void);
extern "C" void art_quick_get32_instance(void);
#endif

static void GetSet32Instance(Handle<mirror::Object>* obj, Handle<mirror::ArtField>* f,
                             Thread* self, mirror::ArtMethod* referrer, StubTest* test)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
#if defined(__i386__) || defined(__arm__) || defined(__aarch64__) || defined(__x86_64__)
  constexpr size_t num_values = 7;
  uint32_t values[num_values] = { 0, 1, 2, 255, 32768, 1000000, 0xFFFFFFFF };

  for (size_t i = 0; i < num_values; ++i) {
    test->Invoke3WithReferrer(static_cast<size_t>((*f)->GetDexFieldIndex()),
                              reinterpret_cast<size_t>(obj->Get()),
                              static_cast<size_t>(values[i]),
                              reinterpret_cast<uintptr_t>(&art_quick_set32_instance),
                              self,
                              referrer);

    int32_t res = f->Get()->GetInt(obj->Get());
    EXPECT_EQ(res, static_cast<int32_t>(values[i])) << "Iteration " << i;

    res++;
    f->Get()->SetInt<false>(obj->Get(), res);

    size_t res2 = test->Invoke3WithReferrer(static_cast<size_t>((*f)->GetDexFieldIndex()),
                                            reinterpret_cast<size_t>(obj->Get()),
                                            0U,
                                            reinterpret_cast<uintptr_t>(&art_quick_get32_instance),
                                            self,
                                            referrer);
    EXPECT_EQ(res, static_cast<int32_t>(res2));
  }
#else
  LOG(INFO) << "Skipping set32instance as I don't know how to do that on " << kRuntimeISA;
  // Force-print to std::cout so it's also outside the logcat.
  std::cout << "Skipping set32instance as I don't know how to do that on " << kRuntimeISA << std::endl;
#endif
}


#if defined(__i386__) || defined(__arm__) || defined(__aarch64__) || defined(__x86_64__)
extern "C" void art_quick_set_obj_static(void);
extern "C" void art_quick_get_obj_static(void);

static void set_and_check_static(uint32_t f_idx, mirror::Object* val, Thread* self,
                                 mirror::ArtMethod* referrer, StubTest* test)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  test->Invoke3WithReferrer(static_cast<size_t>(f_idx),
                            reinterpret_cast<size_t>(val),
                            0U,
                            reinterpret_cast<uintptr_t>(&art_quick_set_obj_static),
                            self,
                            referrer);

  size_t res = test->Invoke3WithReferrer(static_cast<size_t>(f_idx),
                                         0U, 0U,
                                         reinterpret_cast<uintptr_t>(&art_quick_get_obj_static),
                                         self,
                                         referrer);

  EXPECT_EQ(res, reinterpret_cast<size_t>(val)) << "Value " << val;
}
#endif

static void GetSetObjStatic(Handle<mirror::Object>* obj, Handle<mirror::ArtField>* f, Thread* self,
                            mirror::ArtMethod* referrer, StubTest* test)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
#if defined(__i386__) || defined(__arm__) || defined(__aarch64__) || defined(__x86_64__)
  set_and_check_static((*f)->GetDexFieldIndex(), nullptr, self, referrer, test);

  // Allocate a string object for simplicity.
  mirror::String* str = mirror::String::AllocFromModifiedUtf8(self, "Test");
  set_and_check_static((*f)->GetDexFieldIndex(), str, self, referrer, test);

  set_and_check_static((*f)->GetDexFieldIndex(), nullptr, self, referrer, test);
#else
  LOG(INFO) << "Skipping setObjstatic as I don't know how to do that on " << kRuntimeISA;
  // Force-print to std::cout so it's also outside the logcat.
  std::cout << "Skipping setObjstatic as I don't know how to do that on " << kRuntimeISA << std::endl;
#endif
}


#if defined(__i386__) || defined(__arm__) || defined(__aarch64__) || defined(__x86_64__)
extern "C" void art_quick_set_obj_instance(void);
extern "C" void art_quick_get_obj_instance(void);

static void set_and_check_instance(Handle<mirror::ArtField>* f, mirror::Object* trg,
                                   mirror::Object* val, Thread* self, mirror::ArtMethod* referrer,
                                   StubTest* test)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  test->Invoke3WithReferrer(static_cast<size_t>((*f)->GetDexFieldIndex()),
                            reinterpret_cast<size_t>(trg),
                            reinterpret_cast<size_t>(val),
                            reinterpret_cast<uintptr_t>(&art_quick_set_obj_instance),
                            self,
                            referrer);

  size_t res = test->Invoke3WithReferrer(static_cast<size_t>((*f)->GetDexFieldIndex()),
                                         reinterpret_cast<size_t>(trg),
                                         0U,
                                         reinterpret_cast<uintptr_t>(&art_quick_get_obj_instance),
                                         self,
                                         referrer);

  EXPECT_EQ(res, reinterpret_cast<size_t>(val)) << "Value " << val;

  EXPECT_EQ(val, f->Get()->GetObj(trg));
}
#endif

static void GetSetObjInstance(Handle<mirror::Object>* obj, Handle<mirror::ArtField>* f,
                              Thread* self, mirror::ArtMethod* referrer, StubTest* test)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
#if defined(__i386__) || defined(__arm__) || defined(__aarch64__) || defined(__x86_64__)
  set_and_check_instance(f, obj->Get(), nullptr, self, referrer, test);

  // Allocate a string object for simplicity.
  mirror::String* str = mirror::String::AllocFromModifiedUtf8(self, "Test");
  set_and_check_instance(f, obj->Get(), str, self, referrer, test);

  set_and_check_instance(f, obj->Get(), nullptr, self, referrer, test);
#else
  LOG(INFO) << "Skipping setObjinstance as I don't know how to do that on " << kRuntimeISA;
  // Force-print to std::cout so it's also outside the logcat.
  std::cout << "Skipping setObjinstance as I don't know how to do that on " << kRuntimeISA << std::endl;
#endif
}


// TODO: Complete these tests for 32b architectures.

#if defined(__x86_64__) || defined(__aarch64__)
extern "C" void art_quick_set64_static(void);
extern "C" void art_quick_get64_static(void);
#endif

static void GetSet64Static(Handle<mirror::Object>* obj, Handle<mirror::ArtField>* f, Thread* self,
                           mirror::ArtMethod* referrer, StubTest* test)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
#if defined(__x86_64__) || defined(__aarch64__)
  constexpr size_t num_values = 8;
  uint64_t values[num_values] = { 0, 1, 2, 255, 32768, 1000000, 0xFFFFFFFF, 0xFFFFFFFFFFFF };

  for (size_t i = 0; i < num_values; ++i) {
    test->Invoke3UWithReferrer(static_cast<size_t>((*f)->GetDexFieldIndex()),
                               values[i],
                               reinterpret_cast<uintptr_t>(&art_quick_set64_static),
                               self,
                               referrer);

    size_t res = test->Invoke3WithReferrer(static_cast<size_t>((*f)->GetDexFieldIndex()),
                                           0U, 0U,
                                           reinterpret_cast<uintptr_t>(&art_quick_get64_static),
                                           self,
                                           referrer);

    EXPECT_EQ(res, values[i]) << "Iteration " << i;
  }
#else
  LOG(INFO) << "Skipping set64static as I don't know how to do that on " << kRuntimeISA;
  // Force-print to std::cout so it's also outside the logcat.
  std::cout << "Skipping set64static as I don't know how to do that on " << kRuntimeISA << std::endl;
#endif
}


#if defined(__x86_64__) || defined(__aarch64__)
extern "C" void art_quick_set64_instance(void);
extern "C" void art_quick_get64_instance(void);
#endif

static void GetSet64Instance(Handle<mirror::Object>* obj, Handle<mirror::ArtField>* f,
                             Thread* self, mirror::ArtMethod* referrer, StubTest* test)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
#if defined(__x86_64__) || defined(__aarch64__)
  constexpr size_t num_values = 8;
  uint64_t values[num_values] = { 0, 1, 2, 255, 32768, 1000000, 0xFFFFFFFF, 0xFFFFFFFFFFFF };

  for (size_t i = 0; i < num_values; ++i) {
    test->Invoke3WithReferrer(static_cast<size_t>((*f)->GetDexFieldIndex()),
                              reinterpret_cast<size_t>(obj->Get()),
                              static_cast<size_t>(values[i]),
                              reinterpret_cast<uintptr_t>(&art_quick_set64_instance),
                              self,
                              referrer);

    int64_t res = f->Get()->GetLong(obj->Get());
    EXPECT_EQ(res, static_cast<int64_t>(values[i])) << "Iteration " << i;

    res++;
    f->Get()->SetLong<false>(obj->Get(), res);

    size_t res2 = test->Invoke3WithReferrer(static_cast<size_t>((*f)->GetDexFieldIndex()),
                                            reinterpret_cast<size_t>(obj->Get()),
                                            0U,
                                            reinterpret_cast<uintptr_t>(&art_quick_get64_instance),
                                            self,
                                            referrer);
    EXPECT_EQ(res, static_cast<int64_t>(res2));
  }
#else
  LOG(INFO) << "Skipping set64instance as I don't know how to do that on " << kRuntimeISA;
  // Force-print to std::cout so it's also outside the logcat.
  std::cout << "Skipping set64instance as I don't know how to do that on " << kRuntimeISA << std::endl;
#endif
}

static void TestFields(Thread* self, StubTest* test, Primitive::Type test_type) {
  // garbage is created during ClassLinker::Init

  JNIEnv* env = Thread::Current()->GetJniEnv();
  jclass jc = env->FindClass("AllFields");
  CHECK(jc != NULL);
  jobject o = env->AllocObject(jc);
  CHECK(o != NULL);

  ScopedObjectAccess soa(self);
  StackHandleScope<5> hs(self);
  Handle<mirror::Object> obj(hs.NewHandle(soa.Decode<mirror::Object*>(o)));
  Handle<mirror::Class> c(hs.NewHandle(obj->GetClass()));
  // Need a method as a referrer
  Handle<mirror::ArtMethod> m(hs.NewHandle(c->GetDirectMethod(0)));

  // Play with it...

  // Static fields.
  {
    Handle<mirror::ObjectArray<mirror::ArtField>> fields(hs.NewHandle(c.Get()->GetSFields()));
    int32_t num_fields = fields->GetLength();
    for (int32_t i = 0; i < num_fields; ++i) {
      StackHandleScope<1> hs(self);
      Handle<mirror::ArtField> f(hs.NewHandle(fields->Get(i)));

      FieldHelper fh(f.Get());
      Primitive::Type type = fh.GetTypeAsPrimitiveType();
      switch (type) {
        case Primitive::Type::kPrimInt:
          if (test_type == type) {
            GetSet32Static(&obj, &f, self, m.Get(), test);
          }
          break;

        case Primitive::Type::kPrimLong:
          if (test_type == type) {
            GetSet64Static(&obj, &f, self, m.Get(), test);
          }
          break;

        case Primitive::Type::kPrimNot:
          // Don't try array.
          if (test_type == type && fh.GetTypeDescriptor()[0] != '[') {
            GetSetObjStatic(&obj, &f, self, m.Get(), test);
          }
          break;

        default:
          break;  // Skip.
      }
    }
  }

  // Instance fields.
  {
    Handle<mirror::ObjectArray<mirror::ArtField>> fields(hs.NewHandle(c.Get()->GetIFields()));
    int32_t num_fields = fields->GetLength();
    for (int32_t i = 0; i < num_fields; ++i) {
      StackHandleScope<1> hs(self);
      Handle<mirror::ArtField> f(hs.NewHandle(fields->Get(i)));

      FieldHelper fh(f.Get());
      Primitive::Type type = fh.GetTypeAsPrimitiveType();
      switch (type) {
        case Primitive::Type::kPrimInt:
          if (test_type == type) {
            GetSet32Instance(&obj, &f, self, m.Get(), test);
          }
          break;

        case Primitive::Type::kPrimLong:
          if (test_type == type) {
            GetSet64Instance(&obj, &f, self, m.Get(), test);
          }
          break;

        case Primitive::Type::kPrimNot:
          // Don't try array.
          if (test_type == type && fh.GetTypeDescriptor()[0] != '[') {
            GetSetObjInstance(&obj, &f, self, m.Get(), test);
          }
          break;

        default:
          break;  // Skip.
      }
    }
  }

  // TODO: Deallocate things.
}


TEST_F(StubTest, Fields32) {
  TEST_DISABLED_FOR_HEAP_REFERENCE_POISONING();

  Thread* self = Thread::Current();

  self->TransitionFromSuspendedToRunnable();
  LoadDex("AllFields");
  bool started = runtime_->Start();
  CHECK(started);

  TestFields(self, this, Primitive::Type::kPrimInt);
}

TEST_F(StubTest, FieldsObj) {
  TEST_DISABLED_FOR_HEAP_REFERENCE_POISONING();

  Thread* self = Thread::Current();

  self->TransitionFromSuspendedToRunnable();
  LoadDex("AllFields");
  bool started = runtime_->Start();
  CHECK(started);

  TestFields(self, this, Primitive::Type::kPrimNot);
}

TEST_F(StubTest, Fields64) {
  TEST_DISABLED_FOR_HEAP_REFERENCE_POISONING();

  Thread* self = Thread::Current();

  self->TransitionFromSuspendedToRunnable();
  LoadDex("AllFields");
  bool started = runtime_->Start();
  CHECK(started);

  TestFields(self, this, Primitive::Type::kPrimLong);
}

}  // namespace art
