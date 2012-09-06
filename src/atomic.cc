/*
 * Copyright (C) 2010 The Android Open Source Project
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

#include "atomic.h"

#include <pthread.h>
#include <vector>

#include "mutex.h"
#include "stl_util.h"
#include "stringprintf.h"

#if defined(__APPLE__)
#include <libkern/OSAtomic.h>
#endif
#if defined(__arm__)
#include <machine/cpu-features.h>
#endif

namespace art {

#if defined(HAVE_MACOSX_IPC)
#define NEED_MAC_QUASI_ATOMICS 1

#elif defined(__i386__) || defined(__x86_64__)
#define NEED_PTHREADS_QUASI_ATOMICS 1

#elif defined(__mips__)
#define NEED_PTHREADS_QUASI_ATOMICS 1

#elif defined(__arm__)

#if defined(__ARM_HAVE_LDREXD)
#define NEED_ARM_LDREXD_QUASI_ATOMICS 1
#else
#define NEED_PTHREADS_QUASI_ATOMICS 1
#endif

#else
#error "QuasiAtomic unsupported on this platform"
#endif

// *****************************************************************************

#if NEED_ARM_LDREXD_QUASI_ATOMICS

static inline int64_t QuasiAtomicSwap64Impl(int64_t new_value, volatile int64_t* addr) {
  int64_t prev;
  int status;
  do {
    __asm__ __volatile__("@ QuasiAtomic::Swap64\n"
        "ldrexd     %0, %H0, [%3]\n"
        "strexd     %1, %4, %H4, [%3]"
        : "=&r" (prev), "=&r" (status), "+m"(*addr)
        : "r" (addr), "r" (new_value)
        : "cc");
  } while (__builtin_expect(status != 0, 0));
  return prev;
}

int64_t QuasiAtomic::Swap64(int64_t new_value, volatile int64_t* addr) {
  return QuasiAtomicSwap64Impl(new_value, addr);
}

int64_t QuasiAtomic::Swap64Sync(int64_t new_value, volatile int64_t* addr) {
  ANDROID_MEMBAR_STORE();
  int64_t old_value = QuasiAtomicSwap64Impl(new_value, addr);
  ANDROID_MEMBAR_FULL();
  return old_value;
}

int64_t QuasiAtomic::Read64(volatile const int64_t* addr) {
  int64_t value;
  __asm__ __volatile__("@ QuasiAtomic::Read64\n"
      "ldrexd     %0, %H0, [%1]"
      : "=&r" (value)
      : "r" (addr));
  return value;
}

int QuasiAtomic::Cas64(int64_t old_value, int64_t new_value, volatile int64_t* addr) {
  int64_t prev;
  int status;
  do {
    __asm__ __volatile__("@ QuasiAtomic::Cas64\n"
        "ldrexd     %0, %H0, [%3]\n"
        "mov        %1, #0\n"
        "teq        %0, %4\n"
        "teqeq      %H0, %H4\n"
        "strexdeq   %1, %5, %H5, [%3]"
        : "=&r" (prev), "=&r" (status), "+m"(*addr)
        : "r" (addr), "Ir" (old_value), "r" (new_value)
        : "cc");
  } while (__builtin_expect(status != 0, 0));
  return prev != old_value;
}

#endif

// *****************************************************************************

#if NEED_MAC_QUASI_ATOMICS

static inline int64_t QuasiAtomicSwap64Impl(int64_t value, volatile int64_t* addr) {
  int64_t old_value;
  do {
    old_value = *addr;
  } while (QuasiAtomic::Cas64(old_value, value, addr));
  return old_value;
}

int64_t QuasiAtomic::Swap64(int64_t value, volatile int64_t* addr) {
  return QuasiAtomicSwap64Impl(value, addr);
}

int64_t QuasiAtomic::Swap64Sync(int64_t value, volatile int64_t* addr) {
  ANDROID_MEMBAR_STORE();
  int64_t old_value = QuasiAtomicSwap64Impl(value, addr);
  // TUNING: barriers can be avoided on some architectures.
  ANDROID_MEMBAR_FULL();
  return old_value;
}

int64_t QuasiAtomic::Read64(volatile const int64_t* addr) {
  return OSAtomicAdd64Barrier(0, const_cast<volatile int64_t*>(addr));
}

int QuasiAtomic::Cas64(int64_t old_value, int64_t new_value, volatile int64_t* addr) {
  return OSAtomicCompareAndSwap64Barrier(old_value, new_value, const_cast<int64_t*>(addr)) == 0;
}

#endif

// *****************************************************************************

#if NEED_PTHREADS_QUASI_ATOMICS

// In the absence of a better implementation, we implement the 64-bit atomic
// operations through mutex locking.

// We stripe across a bunch of different mutexes to reduce contention.
static const size_t kSwapLockCount = 32;
static std::vector<Mutex*>* gSwapLocks;

void QuasiAtomic::Startup() {
  gSwapLocks = new std::vector<Mutex*>;
  for (size_t i = 0; i < kSwapLockCount; ++i) {
    gSwapLocks->push_back(new Mutex(StringPrintf("QuasiAtomic stripe %d", i).c_str()));
  }
}

void QuasiAtomic::Shutdown() {
  STLDeleteElements(gSwapLocks);
  delete gSwapLocks;
}

static inline Mutex& GetSwapLock(const volatile int64_t* addr) {
  return *(*gSwapLocks)[((unsigned)(void*)(addr) >> 3U) % kSwapLockCount];
}

int64_t QuasiAtomic::Swap64(int64_t value, volatile int64_t* addr) {
  MutexLock mu(GetSwapLock(addr));
  int64_t old_value = *addr;
  *addr = value;
  return old_value;
}

int64_t QuasiAtomic::Swap64Sync(int64_t value, volatile int64_t* addr) {
  // Same as QuasiAtomicSwap64 - mutex handles barrier.
  return QuasiAtomic::Swap64(value, addr);
}

int QuasiAtomic::Cas64(int64_t old_value, int64_t new_value, volatile int64_t* addr) {
  MutexLock mu(GetSwapLock(addr));
  if (*addr == old_value) {
    *addr  = new_value;
    return 0;
  }
  return 1;
}

int64_t QuasiAtomic::Read64(volatile const int64_t* addr) {
  MutexLock mu(GetSwapLock(addr));
  return *addr;
}

#else

// The other implementations don't need any special setup.
void QuasiAtomic::Startup() {}
void QuasiAtomic::Shutdown() {}

#endif

}  // namespace art
