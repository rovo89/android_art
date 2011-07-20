// Copyright 2011 Google Inc. All Rights Reserved.

#include "src/thread.h"
#include <asm/ldt.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include "src/macros.h"

namespace art {

void Thread::InitCpu() {
  // Read LDT
  CHECK_EQ((size_t)LDT_ENTRY_SIZE, sizeof(uint64_t));
  uint64_t ldt_[LDT_ENTRIES];
  syscall(SYS_modify_ldt, 0, ldt_, sizeof(ldt_));
  // Create empty slot to point at current Thread*
  struct user_desc ldt_entry;
  ldt_entry.entry_number = -1;
  ldt_entry.base_addr = (unsigned int)this;
  ldt_entry.limit = 4096;
  ldt_entry.seg_32bit = 1;
  ldt_entry.contents = MODIFY_LDT_CONTENTS_DATA;
  ldt_entry.read_exec_only = 0;
  ldt_entry.limit_in_pages = 0;
  ldt_entry.seg_not_present = 0;
  ldt_entry.useable = 1;
  for (int i = 0; i < LDT_ENTRIES; i++) {
    if (ldt_[i] == 0) {
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
  asm("movw %w0, %%fs"
      :    // output
      : "q"(selector)  // input
      :);  // clobber
  // Allow easy indirection back to Thread*
  self_ = this;
  // Sanity check reads from FS goes to this Thread*
  CHECK_EQ(0, OFFSETOF_MEMBER(Thread, self_));
  Thread* self_check;
  // TODO: use our assembler to generate code
  asm("movl %%fs:0, %0"
      : "=r"(self_check)  // output
      :    // input
      :);  // clobber
  CHECK_EQ(self_check, this);
}

}  // namespace art
