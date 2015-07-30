/*
 * Copyright (C) 2015 The Android Open Source Project
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

#ifndef ART_RUNTIME_BASE_OUT_H_
#define ART_RUNTIME_BASE_OUT_H_

#include <base/macros.h>
#include <base/logging.h>

#include <memory>
// A zero-overhead abstraction marker that means this value is meant to be used as an out
// parameter for functions. It mimics semantics of a pointer that the function will
// dereference and output its value into.
//
// Inspired by the 'out' language keyword in C#.
//
// Declaration example:
//   int do_work(size_t args, out<int> result);
//               // returns 0 on success, sets result, otherwise error code
//
// Use-site example:
// // (1) -- out of a local variable or field
//   int res;
//   if (do_work(1, outof(res)) {
//     cout << "success: " << res;
//   }
// // (2) -- out of an iterator
//   std::vector<int> list = {1};
//   std::vector<int>::iterator it = list.begin();
//   if (do_work(2, outof_iterator(*it)) {
//     cout << "success: " << list[0];
//   }
// // (3) -- out of a pointer
//   int* array = &some_other_value;
//   if (do_work(3, outof_ptr(array))) {
//     cout << "success: " << *array;
//   }
//
// The type will also automatically decay into a C-style pointer for compatibility
// with calling legacy code that expect pointers.
//
// Declaration example:
//   void write_data(int* res) { *res = 5; }
//
// Use-site example:
//   int data;
//   write_data(outof(res));
//   // data is now '5'
// (The other outof_* functions can be used analogously when the target is a C-style pointer).
//
// ---------------
//
// Other typical pointer operations such as addition, subtraction, etc are banned
// since there is exactly one value being output.
//
namespace art {

// Forward declarations. See below for specific functions.
template <typename T>
struct out_convertible;  // Implicitly converts to out<T> or T*.

// Helper function that automatically infers 'T'
//
// Returns a type that is implicitly convertible to either out<T> or T* depending
// on the call site.
//
// Example:
//   int do_work(size_t args, out<int> result);
//               // returns 0 on success, sets result, otherwise error code
//
// Usage:
//   int res;
//   if (do_work(1, outof(res)) {
//     cout << "success: " << res;
//   }
template <typename T>
out_convertible<T> outof(T& param) ALWAYS_INLINE;

// Helper function that automatically infers 'T' from a container<T>::iterator.
// To use when the argument is already inside an iterator.
//
// Returns a type that is implicitly convertible to either out<T> or T* depending
// on the call site.
//
// Example:
//   int do_work(size_t args, out<int> result);
//               // returns 0 on success, sets result, otherwise error code
//
// Usage:
//   std::vector<int> list = {1};
//   std::vector<int>::iterator it = list.begin();
//   if (do_work(2, outof_iterator(*it)) {
//     cout << "success: " << list[0];
//   }
template <typename It>
auto ALWAYS_INLINE outof_iterator(It iter)
    -> out_convertible<typename std::remove_reference<decltype(*iter)>::type>;

// Helper function that automatically infers 'T'.
// To use when the argument is already a pointer.
//
// ptr must be not-null, else a DCHECK failure will occur.
//
// Returns a type that is implicitly convertible to either out<T> or T* depending
// on the call site.
//
// Example:
//   int do_work(size_t args, out<int> result);
//               // returns 0 on success, sets result, otherwise error code
//
// Usage:
//   int* array = &some_other_value;
//   if (do_work(3, outof_ptr(array))) {
//     cout << "success: " << *array;
//   }
template <typename T>
out_convertible<T> outof_ptr(T* ptr) ALWAYS_INLINE;

// Zero-overhead wrapper around a non-null non-const pointer meant to be used to output
// the result of parameters. There are no other extra guarantees.
//
// The most common use case is to treat this like a typical pointer argument, for example:
//
// void write_out_5(out<int> x) {
//   *x = 5;
// }
//
// The following operations are supported:
//   operator* -> use like a pointer (guaranteed to be non-null)
//   == and != -> compare against other pointers for (in)equality
//   begin/end -> use in standard C++ algorithms as if it was an iterator
template <typename T>
struct out {
  // Has to be mutable lref. Otherwise how would you write something as output into it?
  explicit inline out(T& param)
    : param_(param) {}

  // Model a single-element iterator (or pointer) to the parameter.
  inline T& operator *() {
    return param_;
  }

  //
  // Comparison against this or other pointers.
  //
  template <typename T2>
  inline bool operator==(const T2* other) const {
    return std::addressof(param_) == other;
  }

  template <typename T2>
  inline bool operator==(const out<T>& other) const {
    return std::addressof(param_) == std::addressof(other.param_);
  }

  // An out-parameter is never null.
  inline bool operator==(std::nullptr_t) const {
    return false;
  }

  template <typename T2>
  inline bool operator!=(const T2* other) const {
    return std::addressof(param_) != other;
  }

  template <typename T2>
  inline bool operator!=(const out<T>& other) const {
    return std::addressof(param_) != std::addressof(other.param_);
  }

  // An out-parameter is never null.
  inline bool operator!=(std::nullptr_t) const {
    return true;
  }

  //
  // Iterator interface implementation. Use with standard algorithms.
  // TODO: (add items in iterator_traits if this is truly useful).
  //

  inline T* begin() {
    return std::addressof(param_);
  }

  inline const T* begin() const {
    return std::addressof(param_);
  }

  inline T* end() {
    return std::addressof(param_) + 1;
  }

  inline const T* end() const {
    return std::addressof(param_) + 1;
  }

 private:
  T& param_;
};

//
// IMPLEMENTATION DETAILS
//

//
// This intermediate type should not be used directly by user code.
//
// It enables 'outof(x)' to be passed into functions that expect either
// an out<T> **or** a regular C-style pointer (T*).
//
template <typename T>
struct out_convertible {
  explicit inline out_convertible(T& param)
    : param_(param) {
  }

  // Implicitly convert into an out<T> for standard usage.
  inline operator out<T>() {
    return out<T>(param_);
  }

  // Implicitly convert into a '*' for legacy usage.
  inline operator T*() {
    return std::addressof(param_);
  }
 private:
  T& param_;
};

// Helper function that automatically infers 'T'
template <typename T>
inline out_convertible<T> outof(T& param) {
  return out_convertible<T>(param);
}

// Helper function that automatically infers 'T'.
// To use when the argument is already inside an iterator.
template <typename It>
inline auto outof_iterator(It iter)
    -> out_convertible<typename std::remove_reference<decltype(*iter)>::type> {
  return outof(*iter);
}

// Helper function that automatically infers 'T'.
// To use when the argument is already a pointer.
template <typename T>
inline out_convertible<T> outof_ptr(T* ptr) {
  DCHECK(ptr != nullptr);
  return outof(*ptr);
}

// Helper function that automatically infers 'T'.
// Forwards an out parameter from one function into another.
template <typename T>
inline out_convertible<T> outof_forward(out<T>& out_param) {
  T& param = std::addressof(*out_param);
  return out_convertible<T>(param);
}

}  // namespace art
#endif  // ART_RUNTIME_BASE_OUT_H_
