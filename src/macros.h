// Copyright 2010 Google
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef ART_SRC_MACROS_H_
#define ART_SRC_MACROS_H_

#include <stddef.h>  // for size_t

#define GCC_VERSION (__GNUC__ * 10000 + __GNUC_MINOR__ * 100 + __GNUC_PATCHLEVEL__)

// The COMPILE_ASSERT macro can be used to verify that a compile time
// expression is true. For example, you could use it to verify the
// size of a static array:
//
//   COMPILE_ASSERT(ARRAYSIZE(content_type_names) == CONTENT_NUM_TYPES,
//                  content_type_names_incorrect_size);
//
// or to make sure a struct is smaller than a certain size:
//
//   COMPILE_ASSERT(sizeof(foo) < 128, foo_too_large);
//
// The second argument to the macro is the name of the variable. If
// the expression is false, most compilers will issue a warning/error
// containing the name of the variable.

template <bool>
struct CompileAssert {
};

#define COMPILE_ASSERT(expr, msg) \
  typedef CompileAssert<(bool(expr))> msg[bool(expr) ? 1 : -1]

// DISALLOW_COPY_AND_ASSIGN disallows the copy and operator= functions.
// It goes in the private: declarations in a class.
#define DISALLOW_COPY_AND_ASSIGN(TypeName) \
  TypeName(const TypeName&);               \
  void operator=(const TypeName&)

// A macro to disallow all the implicit constructors, namely the
// default constructor, copy constructor and operator= functions.
//
// This should be used in the private: declarations for a class
// that wants to prevent anyone from instantiating it. This is
// especially useful for classes containing only static methods.
#define DISALLOW_IMPLICIT_CONSTRUCTORS(TypeName) \
  TypeName();                                    \
  DISALLOW_COPY_AND_ASSIGN(TypeName)

// The arraysize(arr) macro returns the # of elements in an array arr.
// The expression is a compile-time constant, and therefore can be
// used in defining new arrays, for example.  If you use arraysize on
// a pointer by mistake, you will get a compile-time error.
//
// One caveat is that arraysize() doesn't accept any array of an
// anonymous type or a type defined inside a function.  In these rare
// cases, you have to use the unsafe ARRAYSIZE_UNSAFE() macro below.  This is
// due to a limitation in C++'s template system.  The limitation might
// eventually be removed, but it hasn't happened yet.

// This template function declaration is used in defining arraysize.
// Note that the function doesn't need an implementation, as we only
// use its type.
template <typename T, size_t N>
char (&ArraySizeHelper(T (&array)[N]))[N];

#define arraysize(array) (sizeof(ArraySizeHelper(array)))

// ARRAYSIZE_UNSAFE performs essentially the same calculation as arraysize,
// but can be used on anonymous types or types defined inside
// functions.  It's less safe than arraysize as it accepts some
// (although not all) pointers.  Therefore, you should use arraysize
// whenever possible.
//
// The expression ARRAYSIZE_UNSAFE(a) is a compile-time constant of type
// size_t.
//
// ARRAYSIZE_UNSAFE catches a few type errors.  If you see a compiler error
//
//   "warning: division by zero in ..."
//
// when using ARRAYSIZE_UNSAFE, you are (wrongfully) giving it a pointer.
// You should only use ARRAYSIZE_UNSAFE on statically allocated arrays.
//
// The following comments are on the implementation details, and can
// be ignored by the users.
//
// ARRAYSIZE_UNSAFE(arr) works by inspecting sizeof(arr) (the # of bytes in
// the array) and sizeof(*(arr)) (the # of bytes in one array
// element).  If the former is divisible by the latter, perhaps arr is
// indeed an array, in which case the division result is the # of
// elements in the array.  Otherwise, arr cannot possibly be an array,
// and we generate a compiler error to prevent the code from
// compiling.
//
// Since the size of bool is implementation-defined, we need to cast
// !(sizeof(a) & sizeof(*(a))) to size_t in order to ensure the final
// result has type size_t.
//
// This macro is not perfect as it wrongfully accepts certain
// pointers, namely where the pointer size is divisible by the pointee
// size.  Since all our code has to go through a 32-bit compiler,
// where a pointer is 4 bytes, this means all pointers to a type whose
// size is 3 or greater than 4 will be (righteously) rejected.
#define ARRAYSIZE_UNSAFE(a) \
  ((sizeof(a) / sizeof(*(a))) / static_cast<size_t>(!(sizeof(a) % sizeof(*(a)))))

#define SIZEOF_MEMBER(t, f) sizeof((reinterpret_cast<t*>(4096))->f)

#define OFFSETOF_MEMBER(t, f) \
  (reinterpret_cast<char*>(&reinterpret_cast<t*>(16)->f) - reinterpret_cast<char*>(16))

#define OFFSETOF_VOLATILE_MEMBER(t, f) \
  (reinterpret_cast<volatile char*>(&reinterpret_cast<t*>(16)->f) - reinterpret_cast<volatile char*>(16))

#define PACKED __attribute__ ((__packed__))

#define LIKELY(x)       __builtin_expect((x), true)
#define UNLIKELY(x)     __builtin_expect((x), false)

// bionic and glibc both have TEMP_FAILURE_RETRY, but Mac OS' libc doesn't.
#ifndef TEMP_FAILURE_RETRY
#define TEMP_FAILURE_RETRY(exp) ({ \
  typeof (exp) _rc; \
  do { \
    _rc = (exp); \
  } while (_rc == -1 && errno == EINTR); \
  _rc; })
#endif

#endif  // ART_SRC_MACROS_H_
