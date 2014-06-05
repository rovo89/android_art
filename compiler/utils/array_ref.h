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

#ifndef ART_COMPILER_UTILS_ARRAY_REF_H_
#define ART_COMPILER_UTILS_ARRAY_REF_H_

#include <type_traits>
#include <vector>

#include "base/logging.h"

namespace art {

/**
 * @brief A container that references an array.
 *
 * @details The template class ArrayRef provides a container that references
 * an external array. This external array must remain alive while the ArrayRef
 * object is in use. The external array may be a std::vector<>-backed storage
 * or any other contiguous chunk of memory but that memory must remain valid,
 * i.e. the std::vector<> must not be resized for example.
 *
 * Except for copy/assign and insert/erase/capacity functions, the interface
 * is essentially the same as std::vector<>. Since we don't want to throw
 * exceptions, at() is also excluded.
 */
template <typename T>
class ArrayRef {
 private:
  struct tag { };

 public:
  typedef T value_type;
  typedef T& reference;
  typedef const T& const_reference;
  typedef T* pointer;
  typedef const T* const_pointer;
  typedef T* iterator;
  typedef const T* const_iterator;
  typedef std::reverse_iterator<iterator> reverse_iterator;
  typedef std::reverse_iterator<const_iterator> const_reverse_iterator;
  typedef ptrdiff_t difference_type;
  typedef size_t size_type;

  // Constructors.

  constexpr ArrayRef()
      : array_(nullptr), size_(0u) {
  }

  template <size_t size>
  constexpr ArrayRef(T (&array)[size])
    : array_(array), size_(size) {
  }

  template <typename U, size_t size>
  constexpr ArrayRef(U (&array)[size],
                     typename std::enable_if<std::is_same<T, const U>::value, tag>::type t = tag())
    : array_(array), size_(size) {
  }

  constexpr ArrayRef(T* array, size_t size)
      : array_(array), size_(size) {
  }

  template <typename U>
  constexpr ArrayRef(U* array, size_t size,
                     typename std::enable_if<std::is_same<T, const U>::value, tag>::type t = tag())
      : array_(array), size_(size) {
  }

  explicit ArrayRef(std::vector<T>& v)
      : array_(v.data()), size_(v.size()) {
  }

  template <typename U>
  ArrayRef(const std::vector<U>& v,
           typename std::enable_if<std::is_same<T, const U>::value, tag>::tag t = tag())
      : array_(v.data()), size_(v.size()) {
  }

  // Assignment operators.

  ArrayRef& operator=(const ArrayRef& other) {
    array_ = other.array_;
    size_ = other.size_;
    return *this;
  }

  template <typename U>
  typename std::enable_if<std::is_same<T, const U>::value, ArrayRef>::type&
  operator=(const ArrayRef<U>& other) {
    return *this = ArrayRef(other);
  }

  // Destructor.
  ~ArrayRef() = default;

  // Iterators.
  iterator begin() { return array_; }
  const_iterator begin() const { return array_; }
  const_iterator cbegin() const { return array_; }
  iterator end() { return array_ + size_; }
  const_iterator end() const { return array_ + size_; }
  const_iterator cend() const { return array_ + size_; }
  reverse_iterator rbegin() { return reverse_iterator(end()); }
  const_reverse_iterator rbegin() const { return const_reverse_iterator(end()); }
  const_reverse_iterator crbegin() const { return const_reverse_iterator(cend()); }
  reverse_iterator rend() { return reverse_iterator(begin()); }
  const_reverse_iterator rend() const { return const_reverse_iterator(begin()); }
  const_reverse_iterator crend() const { return const_reverse_iterator(cbegin()); }

  // Size.
  size_type size() const { return size_; }
  bool empty() const { return size() == 0u; }

  // Element access. NOTE: Not providing at().

  reference operator[](size_type n) {
    DCHECK_LT(n, size_);
    return array_[n];
  }

  const_reference operator[](size_type n) const {
    DCHECK_LT(n, size_);
    return array_[n];
  }

  reference front() {
    DCHECK_NE(size_, 0u);
    return array_[0];
  }

  const_reference front() const {
    DCHECK_NE(size_, 0u);
    return array_[0];
  }

  reference back() {
    DCHECK_NE(size_, 0u);
    return array_[size_ - 1u];
  }

  const_reference back() const {
    DCHECK_NE(size_, 0u);
    return array_[size_ - 1u];
  }

  value_type* data() { return array_; }
  const value_type* data() const { return array_; }

 private:
  T* array_;
  size_t size_;
};

}  // namespace art


#endif  // ART_COMPILER_UTILS_ARRAY_REF_H_
