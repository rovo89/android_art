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

#ifndef ART_RUNTIME_UNIQUEPTRCOMPAT_H_
#define ART_RUNTIME_UNIQUEPTRCOMPAT_H_

// Stlport doesn't declare std::unique_ptr. UniquePtr.h declares an incompatible std::swap
// prototype with libc++. This compatibility header file resolves differences between the two, in
// the future UniquePtr will become std::unique_ptr.

#ifdef ART_WITH_STLPORT

#include "UniquePtr.h"

#else   //  ART_WITH_STLPORT

#include <memory>

template <typename T>
using UniquePtr = typename std::unique_ptr<T>;

#endif  //  ART_WITH_STLPORT

#endif  // ART_RUNTIME_UNIQUEPTRCOMPAT_H_
