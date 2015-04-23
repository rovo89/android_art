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

#include "base/logging.h"
#include "base/stringprintf.h"
#include "compiler_ir.h"
#include "dex/dataflow_iterator-inl.h"
#include "dex_flags.h"
#include "driver/dex_compilation_unit.h"

namespace art {

static const char* storage_name[] = {" Frame ", "PhysReg", " CompilerTemp "};

void MIRGraph::DumpRegLocTable(RegLocation* table, int count) {
  for (int i = 0; i < count; i++) {
    LOG(INFO) << StringPrintf("Loc[%02d] : %s, %c %c %c %c %c %c 0x%04x S%d",
                              table[i].orig_sreg, storage_name[table[i].location],
                              table[i].wide ? 'W' : 'N', table[i].defined ? 'D' : 'U',
                              table[i].fp ? 'F' : table[i].ref ? 'R' :'C',
                              table[i].is_const ? 'c' : 'n',
                              table[i].high_word ? 'H' : 'L', table[i].home ? 'h' : 't',
                              table[i].reg.GetRawBits(),
                              table[i].s_reg_low);
  }
}

// FIXME - will likely need to revisit all uses of this.
static const RegLocation fresh_loc = {kLocDalvikFrame, 0, 0, 0, 0, 0, 0, 0, 0,
                                      RegStorage(), INVALID_SREG, INVALID_SREG};

void MIRGraph::InitRegLocations() {
  // Allocate the location map. We also include the maximum possible temps because
  // the temp allocation initializes reg location as well (in order to deal with
  // case when it will be called after this pass).
  int max_regs = GetNumSSARegs() + GetMaxPossibleCompilerTemps();
  RegLocation* loc = arena_->AllocArray<RegLocation>(max_regs, kArenaAllocRegAlloc);
  for (int i = 0; i < GetNumSSARegs(); i++) {
    loc[i] = fresh_loc;
    loc[i].s_reg_low = i;
    loc[i].is_const = false;  // Constants will be marked by constant propagation pass later.
  }

  /* Mark the location of ArtMethod* as temporary */
  loc[GetMethodSReg()].location = kLocCompilerTemp;

  reg_location_ = loc;
}

/*
 * Set the s_reg_low field to refer to the pre-SSA name of the
 * base Dalvik virtual register.  Once we add a better register
 * allocator, remove this remapping.
 */
void MIRGraph::RemapRegLocations() {
  for (int i = 0; i < GetNumSSARegs(); i++) {
    int orig_sreg = reg_location_[i].s_reg_low;
    reg_location_[i].orig_sreg = orig_sreg;
    reg_location_[i].s_reg_low = SRegToVReg(orig_sreg);
  }
}

}  // namespace art
