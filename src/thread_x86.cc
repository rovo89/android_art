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

#include "thread.h"

#include <sys/syscall.h>
#include <sys/types.h>

#include "asm_support.h"
#include "macros.h"
#include "thread_list.h"

#if defined(__APPLE__)
#include <architecture/i386/table.h>
#include <i386/user_ldt.h>
#else
#include <asm/ldt.h>
#endif

namespace art {

void Thread::InitCpu() {
#if defined(__APPLE__)
  UNIMPLEMENTED(WARNING);
#else
  // TODO: create specific lock for LDT modification
  ScopedThreadListLock mutex;  // Avoid concurrent modification of the LDT

  // Read LDT
  CHECK_EQ((size_t)LDT_ENTRY_SIZE, sizeof(uint64_t));
  std::vector<uint64_t> ldt(LDT_ENTRIES);
  size_t ldt_size(sizeof(uint64_t) * ldt.size());
  memset(&ldt[0], 0, ldt_size);
  syscall(SYS_modify_ldt, 0, &ldt[0], ldt_size);
  // Create empty slot to point at current Thread*
  user_desc ldt_entry;
  memset(&ldt_entry, 0, sizeof(ldt_entry));
  ldt_entry.entry_number = -1;
  ldt_entry.base_addr = (unsigned int)this;
  ldt_entry.limit = kPageSize;
  ldt_entry.seg_32bit = 1;
  ldt_entry.contents = MODIFY_LDT_CONTENTS_DATA;
  ldt_entry.read_exec_only = 0;
  ldt_entry.limit_in_pages = 0;
  ldt_entry.seg_not_present = 0;
  ldt_entry.useable = 1;
  for (int i = 0; i < LDT_ENTRIES; i++) {
    if (ldt[i] == 0) {
      ldt_entry.entry_number = i;
      break;
    }
  }
  if (ldt_entry.entry_number >= LDT_ENTRIES) {
    LOG(FATAL) << "Failed to find available LDT slot";
  }
  // Update LDT
  CHECK_EQ(0, syscall(SYS_modify_ldt, 1, &ldt_entry, sizeof(ldt_entry)));
  // Change FS to be new LDT entry
  uint16_t table_indicator = 1 << 2;  // LDT
  uint16_t rpl = 3;  // Requested privilege level
  uint16_t selector = (ldt_entry.entry_number << 3) | table_indicator | rpl;
  // TODO: use our assembler to generate code
  asm volatile("movw %w0, %%fs"
      :    // output
      : "q"(selector)  // input
      :);  // clobber
  // Allow easy indirection back to Thread*
  self_ = this;
  // Sanity check reads from FS goes to this Thread*
  Thread* self_check;
  // TODO: use our assembler to generate code
  CHECK_EQ(THREAD_SELF_OFFSET, OFFSETOF_MEMBER(Thread, self_));
  asm volatile("movl %%fs:(%1), %0"
      : "=r"(self_check)  // output
      : "r"(THREAD_SELF_OFFSET)  // input
      :);  // clobber
  CHECK_EQ(self_check, this);
#endif
}

}  // namespace art
