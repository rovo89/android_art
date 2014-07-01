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

#ifndef ART_COMPILER_DEX_REG_LOCATION_H_
#define ART_COMPILER_DEX_REG_LOCATION_H_

#include "reg_storage.h"

namespace art {


/*
 * Whereas a SSA name describes a definition of a Dalvik vreg, the RegLocation describes
 * the type of an SSA name (and, can also be used by code generators to record where the
 * value is located (i.e. - physical register, frame, spill, etc.).  For each SSA name (SReg)
 * there is a RegLocation.
 * A note on SSA names:
 *   o SSA names for Dalvik vRegs v0..vN will be assigned 0..N.  These represent the "vN_0"
 *     names.  Negative SSA names represent special values not present in the Dalvik byte code.
 *     For example, SSA name -1 represents an invalid SSA name, and SSA name -2 represents the
 *     the Method pointer.  SSA names < -2 are reserved for future use.
 *   o The vN_0 names for non-argument Dalvik should in practice never be used (as they would
 *     represent the read of an undefined local variable).  The first definition of the
 *     underlying Dalvik vReg will result in a vN_1 name.
 *
 * FIXME: The orig_sreg field was added as a workaround for llvm bitcode generation.  With
 * the latest restructuring, we should be able to remove it and rely on s_reg_low throughout.
 */
struct RegLocation {
  RegLocationType location:3;
  unsigned wide:1;
  unsigned defined:1;   // Do we know the type?
  unsigned is_const:1;  // Constant, value in mir_graph->constant_values[].
  unsigned fp:1;        // Floating point?
  unsigned core:1;      // Non-floating point?
  unsigned ref:1;       // Something GC cares about.
  unsigned high_word:1;  // High word of pair?
  unsigned home:1;      // Does this represent the home location?
  RegStorage reg;       // Encoded physical registers.
  int16_t s_reg_low;    // SSA name for low Dalvik word.
  int16_t orig_sreg;    // TODO: remove after Bitcode gen complete
                        // and consolidate usage w/ s_reg_low.
};

}  // namespace art

#endif  // ART_COMPILER_DEX_REG_LOCATION_H_
