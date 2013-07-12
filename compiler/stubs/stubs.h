/*
 * Copyright (C) 2012 The Android Open Source Project
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

#ifndef ART_SRC_COMPILER_STUBS_STUBS_H_
#define ART_SRC_COMPILER_STUBS_STUBS_H_

#include "runtime.h"

namespace art {

namespace arm {
const std::vector<uint8_t>* CreatePortableResolutionTrampoline()
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
const std::vector<uint8_t>* CreateQuickResolutionTrampoline()
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
const std::vector<uint8_t>* CreateInterpreterToInterpreterEntry()
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
const std::vector<uint8_t>* CreateInterpreterToQuickEntry()
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
}

namespace mips {
const std::vector<uint8_t>* CreatePortableResolutionTrampoline()
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
const std::vector<uint8_t>* CreateQuickResolutionTrampoline()
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
const std::vector<uint8_t>* CreateInterpreterToInterpreterEntry()
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
const std::vector<uint8_t>* CreateInterpreterToQuickEntry()
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
}

namespace x86 {
const std::vector<uint8_t>* CreatePortableResolutionTrampoline()
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
const std::vector<uint8_t>* CreateQuickResolutionTrampoline()
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
const std::vector<uint8_t>* CreateInterpreterToInterpreterEntry()
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
const std::vector<uint8_t>* CreateInterpreterToQuickEntry()
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);
}

}  // namespace art

#endif  // ART_SRC_COMPILER_STUBS_STUBS_H_
