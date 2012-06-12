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

#include "greenland/runtime_entry_points.h"

#include "runtime_utils.h"
#include "runtime_support.h"
#include "thread_list.h"

using namespace art;
using namespace art::greenland;

namespace {

void art_test_suspend(Thread* thread) {
  Runtime::Current()->GetThreadList()->FullSuspendCheck(thread);
}

} // anonymous namespace

namespace art {
namespace greenland {

void InitThreadRuntimes(RuntimeEntryPoints* entry_points) {
  entry_points->TestSuspend = art_test_suspend;
}

} // namespace greenland
} // namespace art
