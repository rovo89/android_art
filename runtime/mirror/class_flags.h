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

#ifndef ART_RUNTIME_MIRROR_CLASS_FLAGS_H_
#define ART_RUNTIME_MIRROR_CLASS_FLAGS_H_

#include <stdint.h>

namespace art {
namespace mirror {

// Object types stored in class to help GC with faster object marking.
static constexpr uint32_t kClassFlagNormal             = 0x00000000;
// Only normal objects which have no reference fields, e.g. string or primitive array or normal
// class instance.
static constexpr uint32_t kClassFlagNoReferenceFields  = 0x00000001;
static constexpr uint32_t kClassFlagString             = 0x00000004;
static constexpr uint32_t kClassFlagObjectArray        = 0x00000008;
static constexpr uint32_t kClassFlagClass              = 0x00000010;

// class is ClassLoader or one of its subclasses
static constexpr uint32_t kClassFlagClassLoader        = 0x00000020;

// class is a soft/weak/phantom ref
static constexpr uint32_t kClassFlagSoftReference      = 0x00000040;
// class is a weak reference
static constexpr uint32_t kClassFlagWeakReference      = 0x00000080;
// class is a finalizer reference
static constexpr uint32_t kClassFlagFinalizerReference = 0x00000100;
// class is a phantom reference
static constexpr uint32_t kClassFlagPhantomReference   = 0x00000200;

static constexpr uint32_t kClassFlagReference =
    kClassFlagSoftReference |
    kClassFlagWeakReference |
    kClassFlagFinalizerReference |
    kClassFlagPhantomReference;

}  // namespace mirror
}  // namespace art

#endif  // ART_RUNTIME_MIRROR_CLASS_FLAGS_H_

