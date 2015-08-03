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

#ifndef ART_RUNTIME_BASE_OUT_FWD_H_
#define ART_RUNTIME_BASE_OUT_FWD_H_

// Forward declaration for "out<T>". See <out.h> for more information.
// Other headers use only the forward declaration.

// Callers of functions that take an out<T> parameter should #include <out.h> to get outof_.
// which constructs out<T> through type inference.
namespace art {
template <typename T>
struct out;
}  // namespace art

#endif  // ART_RUNTIME_BASE_OUT_FWD_H_
