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

#include "mark_stack.h"

#include <sys/mman.h>

#include "UniquePtr.h"
#include "globals.h"
#include "logging.h"
#include "utils.h"

namespace art {

MarkStack* MarkStack::Create() {
  UniquePtr<MarkStack> mark_stack(new MarkStack);
  mark_stack->Init();
  return mark_stack.release();
}

void MarkStack::Init() {
  size_t length = 64 * MB;
  mem_map_.reset(MemMap::MapAnonymous("dalvik-mark-stack", NULL, length, PROT_READ | PROT_WRITE));
  if (mem_map_.get() == NULL) {
    std::string maps;
    ReadFileToString("/proc/self/maps", &maps);
    LOG(FATAL) << "couldn't allocate mark stack\n" << maps;
  }
  byte* addr = mem_map_->Begin();
  CHECK(addr != NULL);

  begin_ = reinterpret_cast<const Object**>(addr);
  limit_ = reinterpret_cast<const Object**>(addr + length);

  Reset();
}

void MarkStack::Reset() {
  DCHECK(mem_map_.get() != NULL);
  DCHECK(begin_ != NULL);
  DCHECK(limit_ != NULL);
  byte* addr = const_cast<byte*>(reinterpret_cast<const byte*>(begin_));
  const size_t length = limit_ - begin_;
  ptr_ = reinterpret_cast<const Object**>(addr);
  int result = madvise(addr, length, MADV_DONTNEED);
  if (result == -1) {
    PLOG(WARNING) << "madvise failed";
  }
}

MarkStack::~MarkStack() {
  CHECK(IsEmpty());
}

}  // namespace art
