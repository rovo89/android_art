/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include "common_test.h"

namespace art {

class HeapTest : public CommonTest {};

TEST_F(HeapTest, ClearGrowthLimit) {
  int64_t max_memory_before = Heap::GetMaxMemory();
  int64_t total_memory_before = Heap::GetTotalMemory();
  Heap::ClearGrowthLimit();
  int64_t max_memory_after = Heap::GetMaxMemory();
  int64_t total_memory_after = Heap::GetTotalMemory();
  EXPECT_GE(max_memory_after, max_memory_before);
  EXPECT_GE(total_memory_after, total_memory_before);
}

TEST_F(HeapTest, GarbageCollectClassLinkerInit) {
  // garbage is created during ClassLinker::Init

  Class* c = class_linker_->FindSystemClass("[Ljava/lang/Object;");
  for (size_t i = 0; i < 1024; ++i) {
    SirtRef<ObjectArray<Object> > array(ObjectArray<Object>::Alloc(c, 2048));
    for (size_t j = 0; j < 2048; ++j) {
      array->Set(j, String::AllocFromModifiedUtf8("hello, world!"));
    }
  }

  Heap::CollectGarbage(false);
}

}  // namespace art
