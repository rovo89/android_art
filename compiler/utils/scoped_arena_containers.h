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

#ifndef ART_COMPILER_UTILS_SCOPED_ARENA_CONTAINERS_H_
#define ART_COMPILER_UTILS_SCOPED_ARENA_CONTAINERS_H_

#include <vector>
#include <set>

#include "utils/scoped_arena_allocator.h"
#include "safe_map.h"

namespace art {

template <typename T>
using ScopedArenaVector = std::vector<T, ScopedArenaAllocatorAdapter<T> >;

template <typename T, typename Comparator = std::less<T> >
using ScopedArenaSet = std::set<T, Comparator, ScopedArenaAllocatorAdapter<T> >;

template <typename K, typename V, typename Comparator = std::less<K> >
using ScopedArenaSafeMap =
    SafeMap<K, V, Comparator, ScopedArenaAllocatorAdapter<std::pair<const K, V> > >;

}  // namespace art

#endif  // ART_COMPILER_UTILS_SCOPED_ARENA_CONTAINERS_H_
