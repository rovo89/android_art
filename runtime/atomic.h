/*
 * Copyright (C) 2008 The Android Open Source Project
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

#ifndef ART_RUNTIME_ATOMIC_H_
#define ART_RUNTIME_ATOMIC_H_

#include <stdint.h>
#include <vector>

#include "base/logging.h"
#include "base/macros.h"

namespace art {

class Mutex;

template<typename T>
class Atomic {
 public:
  Atomic<T>() : value_(0) { }

  explicit Atomic<T>(T value) : value_(value) { }

  Atomic<T>& operator=(T desired) {
    Store(desired);
    return *this;
  }

  T Load() const {
    return value_;
  }

  operator T() const {
    return Load();
  }

  T FetchAndAdd(const T value) {
    return __sync_fetch_and_add(&value_, value);  // Return old_value.
  }

  T FetchAndSub(const T value) {
    return __sync_fetch_and_sub(&value_, value);  // Return old value.
  }

  T operator++() {  // Prefix operator.
    return __sync_add_and_fetch(&value_, 1);  // Return new value.
  }

  T operator++(int) {  // Postfix operator.
    return __sync_fetch_and_add(&value_, 1);  // Return old value.
  }

  T operator--() {  // Prefix operator.
    return __sync_sub_and_fetch(&value_, 1);  // Return new value.
  }

  T operator--(int) {  // Postfix operator.
    return __sync_fetch_and_sub(&value_, 1);  // Return old value.
  }

  bool CompareAndSwap(T expected_value, T desired_value) {
    return __sync_bool_compare_and_swap(&value_, expected_value, desired_value);
  }

  volatile T* Address() {
    return &value_;
  }

 private:
  // Unsafe = operator for non atomic operations on the integer.
  void Store(T desired) {
    value_ = desired;
  }

  volatile T value_;
};

typedef Atomic<int32_t> AtomicInteger;

// NOTE: Two "quasiatomic" operations on the exact same memory address
// are guaranteed to operate atomically with respect to each other,
// but no guarantees are made about quasiatomic operations mixed with
// non-quasiatomic operations on the same address, nor about
// quasiatomic operations that are performed on partially-overlapping
// memory.
class QuasiAtomic {
#if defined(__mips__) && !defined(__LP64__)
  static constexpr bool kNeedSwapMutexes = true;
#else
  static constexpr bool kNeedSwapMutexes = false;
#endif

 public:
  static void Startup();

  static void Shutdown();

  // Reads the 64-bit value at "addr" without tearing.
  static int64_t Read64(volatile const int64_t* addr) {
    if (!kNeedSwapMutexes) {
      int64_t value;
#if defined(__LP64__)
      value = *addr;
#else
#if defined(__arm__)
#if defined(__ARM_FEATURE_LPAE)
      // With LPAE support (such as Cortex-A15) then ldrd is defined not to tear.
      __asm__ __volatile__("@ QuasiAtomic::Read64\n"
        "ldrd     %0, %H0, %1"
        : "=r" (value)
        : "m" (*addr));
#else
      // Exclusive loads are defined not to tear, clearing the exclusive state isn't necessary.
      __asm__ __volatile__("@ QuasiAtomic::Read64\n"
        "ldrexd     %0, %H0, %1"
        : "=r" (value)
        : "Q" (*addr));
#endif
#elif defined(__i386__)
  __asm__ __volatile__(
      "movq     %1, %0\n"
      : "=x" (value)
      : "m" (*addr));
#else
      LOG(FATAL) << "Unsupported architecture";
#endif
#endif  // defined(__LP64__)
      return value;
    } else {
      return SwapMutexRead64(addr);
    }
  }

  // Writes to the 64-bit value at "addr" without tearing.
  static void Write64(volatile int64_t* addr, int64_t value) {
    if (!kNeedSwapMutexes) {
#if defined(__LP64__)
      *addr = value;
#else
#if defined(__arm__)
#if defined(__ARM_FEATURE_LPAE)
    // If we know that ARM architecture has LPAE (such as Cortex-A15) strd is defined not to tear.
    __asm__ __volatile__("@ QuasiAtomic::Write64\n"
      "strd     %1, %H1, %0"
      : "=m"(*addr)
      : "r" (value));
#else
    // The write is done as a swap so that the cache-line is in the exclusive state for the store.
    int64_t prev;
    int status;
    do {
      __asm__ __volatile__("@ QuasiAtomic::Write64\n"
        "ldrexd     %0, %H0, %2\n"
        "strexd     %1, %3, %H3, %2"
        : "=&r" (prev), "=&r" (status), "+Q"(*addr)
        : "r" (value)
        : "cc");
      } while (UNLIKELY(status != 0));
#endif
#elif defined(__i386__)
      __asm__ __volatile__(
        "movq     %1, %0"
        : "=m" (*addr)
        : "x" (value));
#else
      LOG(FATAL) << "Unsupported architecture";
#endif
#endif  // defined(__LP64__)
    } else {
      SwapMutexWrite64(addr, value);
    }
  }

  // Atomically compare the value at "addr" to "old_value", if equal replace it with "new_value"
  // and return true. Otherwise, don't swap, and return false.
  static bool Cas64(int64_t old_value, int64_t new_value, volatile int64_t* addr) {
    if (!kNeedSwapMutexes) {
      return __sync_bool_compare_and_swap(addr, old_value, new_value);
    } else {
      return SwapMutexCas64(old_value, new_value, addr);
    }
  }

  // Does the architecture provide reasonable atomic long operations or do we fall back on mutexes?
  static bool LongAtomicsUseMutexes() {
    return !kNeedSwapMutexes;
  }

  static void MembarLoadStore() {
  #if defined(__arm__) || defined(__aarch64__)
    __asm__ __volatile__("dmb ish" : : : "memory");
  #elif defined(__i386__) || defined(__x86_64__)
    __asm__ __volatile__("" : : : "memory");
  #elif defined(__mips__)
    __asm__ __volatile__("sync" : : : "memory");
  #else
  #error Unexpected architecture
  #endif
  }

  static void MembarLoadLoad() {
  #if defined(__arm__) || defined(__aarch64__)
    __asm__ __volatile__("dmb ish" : : : "memory");
  #elif defined(__i386__) || defined(__x86_64__)
    __asm__ __volatile__("" : : : "memory");
  #elif defined(__mips__)
    __asm__ __volatile__("sync" : : : "memory");
  #else
  #error Unexpected architecture
  #endif
  }

  static void MembarStoreStore() {
  #if defined(__arm__) || defined(__aarch64__)
    __asm__ __volatile__("dmb ishst" : : : "memory");
  #elif defined(__i386__) || defined(__x86_64__)
    __asm__ __volatile__("" : : : "memory");
  #elif defined(__mips__)
    __asm__ __volatile__("sync" : : : "memory");
  #else
  #error Unexpected architecture
  #endif
  }

  static void MembarStoreLoad() {
  #if defined(__arm__) || defined(__aarch64__)
    __asm__ __volatile__("dmb ish" : : : "memory");
  #elif defined(__i386__) || defined(__x86_64__)
    __asm__ __volatile__("mfence" : : : "memory");
  #elif defined(__mips__)
    __asm__ __volatile__("sync" : : : "memory");
  #else
  #error Unexpected architecture
  #endif
  }

 private:
  static Mutex* GetSwapMutex(const volatile int64_t* addr);
  static int64_t SwapMutexRead64(volatile const int64_t* addr);
  static void SwapMutexWrite64(volatile int64_t* addr, int64_t val);
  static bool SwapMutexCas64(int64_t old_value, int64_t new_value, volatile int64_t* addr);

  // We stripe across a bunch of different mutexes to reduce contention.
  static constexpr size_t kSwapMutexCount = 32;
  static std::vector<Mutex*>* gSwapMutexes;

  DISALLOW_COPY_AND_ASSIGN(QuasiAtomic);
};

}  // namespace art

#endif  // ART_RUNTIME_ATOMIC_H_
