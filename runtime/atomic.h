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

#ifdef __clang__
#define ART_HAVE_STDATOMIC 1
#endif

#include <stdint.h>
#if ART_HAVE_STDATOMIC
#include <atomic>
#endif
#include <limits>
#include <vector>

#include "base/logging.h"
#include "base/macros.h"

namespace art {

class Mutex;

// QuasiAtomic encapsulates two separate facilities that we are
// trying to move away from:  "quasiatomic" 64 bit operations
// and custom memory fences.  For the time being, they remain
// exposed.  Clients should be converted to use either class Atomic
// below whenever possible, and should eventually use C++11 atomics.
// The two facilities that do not have a good C++11 analog are
// ThreadFenceForConstructor and Atomic::*JavaData.
//
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
  // This is fully ordered, i.e. it has C++11 memory_order_seq_cst
  // semantics (assuming all other accesses use a mutex if this one does).
  // This has "strong" semantics; if it fails then it is guaranteed that
  // at some point during the execution of Cas64, *addr was not equal to
  // old_value.
  static bool Cas64(int64_t old_value, int64_t new_value, volatile int64_t* addr) {
    if (!kNeedSwapMutexes) {
      return __sync_bool_compare_and_swap(addr, old_value, new_value);
    } else {
      return SwapMutexCas64(old_value, new_value, addr);
    }
  }

  // Does the architecture provide reasonable atomic long operations or do we fall back on mutexes?
  static bool LongAtomicsUseMutexes() {
    return kNeedSwapMutexes;
  }

  #if ART_HAVE_STDATOMIC

  static void ThreadFenceAcquire() {
    std::atomic_thread_fence(std::memory_order_acquire);
  }

  static void ThreadFenceRelease() {
    std::atomic_thread_fence(std::memory_order_release);
  }

  static void ThreadFenceForConstructor() {
    #if defined(__aarch64__)
      __asm__ __volatile__("dmb ishst" : : : "memory");
    #else
      std::atomic_thread_fence(std::memory_order_release);
    #endif
  }

  static void ThreadFenceSequentiallyConsistent() {
    std::atomic_thread_fence(std::memory_order_seq_cst);
  }

  #else

  static void ThreadFenceAcquire() {
  #if defined(__arm__) || defined(__aarch64__)
    __asm__ __volatile__("dmb ish" : : : "memory");
    // Could possibly use dmb ishld on aarch64
    // But currently we also use this on volatile loads
    // to enforce store atomicity.  Ishld is
    // insufficient for that purpose.
  #elif defined(__i386__) || defined(__x86_64__)
    __asm__ __volatile__("" : : : "memory");
  #elif defined(__mips__)
    __asm__ __volatile__("sync" : : : "memory");
  #else
  #error Unexpected architecture
  #endif
  }

  static void ThreadFenceRelease() {
  #if defined(__arm__) || defined(__aarch64__)
    __asm__ __volatile__("dmb ish" : : : "memory");
    // ishst doesn't order load followed by store.
  #elif defined(__i386__) || defined(__x86_64__)
    __asm__ __volatile__("" : : : "memory");
  #elif defined(__mips__)
    __asm__ __volatile__("sync" : : : "memory");
  #else
  #error Unexpected architecture
  #endif
  }

  // Fence at the end of a constructor with final fields
  // or allocation.  We believe this
  // only has to order stores, and can thus be weaker than
  // release on aarch64.
  static void ThreadFenceForConstructor() {
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

  static void ThreadFenceSequentiallyConsistent() {
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
  #endif

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

#if ART_HAVE_STDATOMIC
template<typename T>
class Atomic : public std::atomic<T> {
 public:
  Atomic<T>() : std::atomic<T>() { }

  explicit Atomic<T>(T value) : std::atomic<T>(value) { }

  // Load from memory without ordering or synchronization constraints.
  T LoadRelaxed() const {
    return this->load(std::memory_order_relaxed);
  }

  // Word tearing allowed, but may race.
  // TODO: Optimize?
  // There has been some discussion of eventually disallowing word
  // tearing for Java data loads.
  T LoadJavaData() const {
    return this->load(std::memory_order_relaxed);
  }

  // Load from memory with a total ordering.
  // Corresponds exactly to a Java volatile load.
  T LoadSequentiallyConsistent() const {
    return this->load(std::memory_order_seq_cst);
  }

  // Store to memory without ordering or synchronization constraints.
  void StoreRelaxed(T desired) {
    this->store(desired, std::memory_order_relaxed);
  }

  // Word tearing allowed, but may race.
  void StoreJavaData(T desired) {
    this->store(desired, std::memory_order_relaxed);
  }

  // Store to memory with release ordering.
  void StoreRelease(T desired) {
    this->store(desired, std::memory_order_release);
  }

  // Store to memory with a total ordering.
  void StoreSequentiallyConsistent(T desired) {
    this->store(desired, std::memory_order_seq_cst);
  }

  // Atomically replace the value with desired value if it matches the expected value.
  // Participates in total ordering of atomic operations.
  bool CompareExchangeStrongSequentiallyConsistent(T expected_value, T desired_value) {
    return this->compare_exchange_strong(expected_value, desired_value, std::memory_order_seq_cst);
  }

  // The same, except it may fail spuriously.
  bool CompareExchangeWeakSequentiallyConsistent(T expected_value, T desired_value) {
    return this->compare_exchange_weak(expected_value, desired_value, std::memory_order_seq_cst);
  }

  // Atomically replace the value with desired value if it matches the expected value. Doesn't
  // imply ordering or synchronization constraints.
  bool CompareExchangeStrongRelaxed(T expected_value, T desired_value) {
    return this->compare_exchange_strong(expected_value, desired_value, std::memory_order_relaxed);
  }

  // The same, except it may fail spuriously.
  bool CompareExchangeWeakRelaxed(T expected_value, T desired_value) {
    return this->compare_exchange_weak(expected_value, desired_value, std::memory_order_relaxed);
  }

  // Atomically replace the value with desired value if it matches the expected value. Prior writes
  // made to other memory locations by the thread that did the release become visible in this
  // thread.
  bool CompareExchangeWeakAcquire(T expected_value, T desired_value) {
    return this->compare_exchange_weak(expected_value, desired_value, std::memory_order_acquire);
  }

  // Atomically replace the value with desired value if it matches the expected value. prior writes
  // to other memory locations become visible to the threads that do a consume or an acquire on the
  // same location.
  bool CompareExchangeWeakRelease(T expected_value, T desired_value) {
    return this->compare_exchange_weak(expected_value, desired_value, std::memory_order_release);
  }

  T FetchAndAddSequentiallyConsistent(const T value) {
    return this->fetch_add(value, std::memory_order_seq_cst);  // Return old_value.
  }

  T FetchAndSubSequentiallyConsistent(const T value) {
    return this->fetch_sub(value, std::memory_order_seq_cst);  // Return old value.
  }

  volatile T* Address() {
    return reinterpret_cast<T*>(this);
  }

  static T MaxValue() {
    return std::numeric_limits<T>::max();
  }
};

#else

template<typename T> class Atomic;

// Helper class for Atomic to deal separately with size 8 and small
// objects.  Should not be used directly.

template<int SZ, class T> struct AtomicHelper {
  friend class Atomic<T>;

 private:
  COMPILE_ASSERT(sizeof(T) <= 4, bad_atomic_helper_arg);

  static T LoadRelaxed(const volatile T* loc) {
    // sizeof(T) <= 4
    return *loc;
  }

  static void StoreRelaxed(volatile T* loc, T desired) {
    // sizeof(T) <= 4
    *loc = desired;
  }

  static bool CompareExchangeStrongSequentiallyConsistent(volatile T* loc,
                                                  T expected_value, T desired_value) {
    // sizeof(T) <= 4
    return __sync_bool_compare_and_swap(loc, expected_value, desired_value);
  }
};

template<class T> struct AtomicHelper<8, T> {
  friend class Atomic<T>;

 private:
  COMPILE_ASSERT(sizeof(T) == 8, bad_large_atomic_helper_arg);

  static T LoadRelaxed(const volatile T* loc) {
    // sizeof(T) == 8
    volatile const int64_t* loc_ptr =
              reinterpret_cast<volatile const int64_t*>(loc);
    return static_cast<T>(QuasiAtomic::Read64(loc_ptr));
  }

  static void StoreRelaxed(volatile T* loc, T desired) {
    // sizeof(T) == 8
    volatile int64_t* loc_ptr =
                reinterpret_cast<volatile int64_t*>(loc);
    QuasiAtomic::Write64(loc_ptr,
                         static_cast<int64_t>(desired));
  }


  static bool CompareExchangeStrongSequentiallyConsistent(volatile T* loc,
                                                  T expected_value, T desired_value) {
    // sizeof(T) == 8
    volatile int64_t* loc_ptr = reinterpret_cast<volatile int64_t*>(loc);
    return QuasiAtomic::Cas64(
                 static_cast<int64_t>(reinterpret_cast<uintptr_t>(expected_value)),
                 static_cast<int64_t>(reinterpret_cast<uintptr_t>(desired_value)), loc_ptr);
  }
};

template<typename T>
class Atomic {
 private:
  COMPILE_ASSERT(sizeof(T) <= 4 || sizeof(T) == 8, bad_atomic_arg);

 public:
  Atomic<T>() : value_(0) { }

  explicit Atomic<T>(T value) : value_(value) { }

  // Load from memory without ordering or synchronization constraints.
  T LoadRelaxed() const {
    return AtomicHelper<sizeof(T), T>::LoadRelaxed(&value_);
  }

  // Word tearing allowed, but may race.
  T LoadJavaData() const {
    return value_;
  }

  // Load from memory with a total ordering.
  T LoadSequentiallyConsistent() const;

  // Store to memory without ordering or synchronization constraints.
  void StoreRelaxed(T desired) {
    AtomicHelper<sizeof(T), T>::StoreRelaxed(&value_, desired);
  }

  // Word tearing allowed, but may race.
  void StoreJavaData(T desired) {
    value_ = desired;
  }

  // Store to memory with release ordering.
  void StoreRelease(T desired);

  // Store to memory with a total ordering.
  void StoreSequentiallyConsistent(T desired);

  // Atomically replace the value with desired value if it matches the expected value.
  // Participates in total ordering of atomic operations.
  bool CompareExchangeStrongSequentiallyConsistent(T expected_value, T desired_value) {
    return AtomicHelper<sizeof(T), T>::
        CompareExchangeStrongSequentiallyConsistent(&value_, expected_value, desired_value);
  }

  // The same, but may fail spuriously.
  bool CompareExchangeWeakSequentiallyConsistent(T expected_value, T desired_value) {
    // TODO: Take advantage of the fact that it may fail spuriously.
    return AtomicHelper<sizeof(T), T>::
        CompareExchangeStrongSequentiallyConsistent(&value_, expected_value, desired_value);
  }

  // Atomically replace the value with desired value if it matches the expected value. Doesn't
  // imply ordering or synchronization constraints.
  bool CompareExchangeStrongRelaxed(T expected_value, T desired_value) {
    // TODO: make this relaxed.
    return CompareExchangeStrongSequentiallyConsistent(expected_value, desired_value);
  }

  // The same, but may fail spuriously.
  bool CompareExchangeWeakRelaxed(T expected_value, T desired_value) {
    // TODO: Take advantage of the fact that it may fail spuriously.
    // TODO: make this relaxed.
    return CompareExchangeStrongSequentiallyConsistent(expected_value, desired_value);
  }

  // Atomically replace the value with desired value if it matches the expected value. Prior accesses
  // made to other memory locations by the thread that did the release become visible in this
  // thread.
  bool CompareExchangeWeakAcquire(T expected_value, T desired_value) {
    // TODO: make this acquire.
    return CompareExchangeWeakSequentiallyConsistent(expected_value, desired_value);
  }

  // Atomically replace the value with desired value if it matches the expected value. Prior accesses
  // to other memory locations become visible to the threads that do a consume or an acquire on the
  // same location.
  bool CompareExchangeWeakRelease(T expected_value, T desired_value) {
    // TODO: make this release.
    return CompareExchangeWeakSequentiallyConsistent(expected_value, desired_value);
  }

  volatile T* Address() {
    return &value_;
  }

  T FetchAndAddSequentiallyConsistent(const T value) {
    if (sizeof(T) <= 4) {
      return __sync_fetch_and_add(&value_, value);  // Return old value.
    } else {
      T expected;
      do {
        expected = LoadRelaxed();
      } while (!CompareExchangeWeakSequentiallyConsistent(expected, expected + value));
      return expected;
    }
  }

  T FetchAndSubSequentiallyConsistent(const T value) {
    if (sizeof(T) <= 4) {
      return __sync_fetch_and_sub(&value_, value);  // Return old value.
    } else {
      return FetchAndAddSequentiallyConsistent(-value);
    }
  }

  T operator++() {  // Prefix operator.
    if (sizeof(T) <= 4) {
      return __sync_add_and_fetch(&value_, 1);  // Return new value.
    } else {
      return FetchAndAddSequentiallyConsistent(1) + 1;
    }
  }

  T operator++(int) {  // Postfix operator.
    return FetchAndAddSequentiallyConsistent(1);
  }

  T operator--() {  // Prefix operator.
    if (sizeof(T) <= 4) {
      return __sync_sub_and_fetch(&value_, 1);  // Return new value.
    } else {
      return FetchAndSubSequentiallyConsistent(1) - 1;
    }
  }

  T operator--(int) {  // Postfix operator.
    return FetchAndSubSequentiallyConsistent(1);
  }

  static T MaxValue() {
    return std::numeric_limits<T>::max();
  }


 private:
  volatile T value_;
};
#endif

typedef Atomic<int32_t> AtomicInteger;

COMPILE_ASSERT(sizeof(AtomicInteger) == sizeof(int32_t), weird_atomic_int_size);
COMPILE_ASSERT(alignof(AtomicInteger) == alignof(int32_t),
               atomic_int_alignment_differs_from_that_of_underlying_type);
COMPILE_ASSERT(sizeof(Atomic<int64_t>) == sizeof(int64_t), weird_atomic_int64_size);
#if defined(__LP64__)
  COMPILE_ASSERT(alignof(Atomic<int64_t>) == alignof(int64_t),
                 atomic_int64_alignment_differs_from_that_of_underlying_type);
#endif
// The above fails on x86-32.
// This is OK, since we explicitly arrange for alignment of 8-byte fields.


#if !ART_HAVE_STDATOMIC
template<typename T>
inline T Atomic<T>::LoadSequentiallyConsistent() const {
  T result = value_;
  if (sizeof(T) != 8 || !QuasiAtomic::LongAtomicsUseMutexes()) {
    QuasiAtomic::ThreadFenceAcquire();
    // We optimistically assume this suffices for store atomicity.
    // On ARMv8 we strengthen ThreadFenceAcquire to make that true.
  }
  return result;
}

template<typename T>
inline void Atomic<T>::StoreRelease(T desired) {
  if (sizeof(T) != 8 || !QuasiAtomic::LongAtomicsUseMutexes()) {
    QuasiAtomic::ThreadFenceRelease();
  }
  StoreRelaxed(desired);
}

template<typename T>
inline void Atomic<T>::StoreSequentiallyConsistent(T desired) {
  if (sizeof(T) != 8 || !QuasiAtomic::LongAtomicsUseMutexes()) {
    QuasiAtomic::ThreadFenceRelease();
  }
  StoreRelaxed(desired);
  if (sizeof(T) != 8 || !QuasiAtomic::LongAtomicsUseMutexes()) {
    QuasiAtomic::ThreadFenceSequentiallyConsistent();
  }
}

#endif

}  // namespace art

#endif  // ART_RUNTIME_ATOMIC_H_
