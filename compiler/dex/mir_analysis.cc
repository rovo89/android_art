/*
 * Copyright (C) 2013 The Android Open Source Project
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

#include <algorithm>
#include "compiler_internals.h"
#include "dataflow_iterator-inl.h"
#include "dex_instruction.h"
#include "dex_instruction-inl.h"
#include "dex/verified_method.h"
#include "dex/quick/dex_file_method_inliner.h"
#include "dex/quick/dex_file_to_method_inliner_map.h"
#include "driver/compiler_options.h"
#include "UniquePtr.h"
#include "utils/scoped_arena_containers.h"

namespace art {

  // Instruction characteristics used to statically identify computation-intensive methods.
const uint32_t MIRGraph::analysis_attributes_[kMirOpLast] = {
  // 00 NOP
  AN_NONE,

  // 01 MOVE vA, vB
  AN_MOVE,

  // 02 MOVE_FROM16 vAA, vBBBB
  AN_MOVE,

  // 03 MOVE_16 vAAAA, vBBBB
  AN_MOVE,

  // 04 MOVE_WIDE vA, vB
  AN_MOVE,

  // 05 MOVE_WIDE_FROM16 vAA, vBBBB
  AN_MOVE,

  // 06 MOVE_WIDE_16 vAAAA, vBBBB
  AN_MOVE,

  // 07 MOVE_OBJECT vA, vB
  AN_MOVE,

  // 08 MOVE_OBJECT_FROM16 vAA, vBBBB
  AN_MOVE,

  // 09 MOVE_OBJECT_16 vAAAA, vBBBB
  AN_MOVE,

  // 0A MOVE_RESULT vAA
  AN_MOVE,

  // 0B MOVE_RESULT_WIDE vAA
  AN_MOVE,

  // 0C MOVE_RESULT_OBJECT vAA
  AN_MOVE,

  // 0D MOVE_EXCEPTION vAA
  AN_MOVE,

  // 0E RETURN_VOID
  AN_BRANCH,

  // 0F RETURN vAA
  AN_BRANCH,

  // 10 RETURN_WIDE vAA
  AN_BRANCH,

  // 11 RETURN_OBJECT vAA
  AN_BRANCH,

  // 12 CONST_4 vA, #+B
  AN_SIMPLECONST,

  // 13 CONST_16 vAA, #+BBBB
  AN_SIMPLECONST,

  // 14 CONST vAA, #+BBBBBBBB
  AN_SIMPLECONST,

  // 15 CONST_HIGH16 VAA, #+BBBB0000
  AN_SIMPLECONST,

  // 16 CONST_WIDE_16 vAA, #+BBBB
  AN_SIMPLECONST,

  // 17 CONST_WIDE_32 vAA, #+BBBBBBBB
  AN_SIMPLECONST,

  // 18 CONST_WIDE vAA, #+BBBBBBBBBBBBBBBB
  AN_SIMPLECONST,

  // 19 CONST_WIDE_HIGH16 vAA, #+BBBB000000000000
  AN_SIMPLECONST,

  // 1A CONST_STRING vAA, string@BBBB
  AN_NONE,

  // 1B CONST_STRING_JUMBO vAA, string@BBBBBBBB
  AN_NONE,

  // 1C CONST_CLASS vAA, type@BBBB
  AN_NONE,

  // 1D MONITOR_ENTER vAA
  AN_NONE,

  // 1E MONITOR_EXIT vAA
  AN_NONE,

  // 1F CHK_CAST vAA, type@BBBB
  AN_NONE,

  // 20 INSTANCE_OF vA, vB, type@CCCC
  AN_NONE,

  // 21 ARRAY_LENGTH vA, vB
  AN_ARRAYOP,

  // 22 NEW_INSTANCE vAA, type@BBBB
  AN_HEAVYWEIGHT,

  // 23 NEW_ARRAY vA, vB, type@CCCC
  AN_HEAVYWEIGHT,

  // 24 FILLED_NEW_ARRAY {vD, vE, vF, vG, vA}
  AN_HEAVYWEIGHT,

  // 25 FILLED_NEW_ARRAY_RANGE {vCCCC .. vNNNN}, type@BBBB
  AN_HEAVYWEIGHT,

  // 26 FILL_ARRAY_DATA vAA, +BBBBBBBB
  AN_NONE,

  // 27 THROW vAA
  AN_HEAVYWEIGHT | AN_BRANCH,

  // 28 GOTO
  AN_BRANCH,

  // 29 GOTO_16
  AN_BRANCH,

  // 2A GOTO_32
  AN_BRANCH,

  // 2B PACKED_SWITCH vAA, +BBBBBBBB
  AN_SWITCH,

  // 2C SPARSE_SWITCH vAA, +BBBBBBBB
  AN_SWITCH,

  // 2D CMPL_FLOAT vAA, vBB, vCC
  AN_MATH | AN_FP | AN_SINGLE,

  // 2E CMPG_FLOAT vAA, vBB, vCC
  AN_MATH | AN_FP | AN_SINGLE,

  // 2F CMPL_DOUBLE vAA, vBB, vCC
  AN_MATH | AN_FP | AN_DOUBLE,

  // 30 CMPG_DOUBLE vAA, vBB, vCC
  AN_MATH | AN_FP | AN_DOUBLE,

  // 31 CMP_LONG vAA, vBB, vCC
  AN_MATH | AN_LONG,

  // 32 IF_EQ vA, vB, +CCCC
  AN_MATH | AN_BRANCH | AN_INT,

  // 33 IF_NE vA, vB, +CCCC
  AN_MATH | AN_BRANCH | AN_INT,

  // 34 IF_LT vA, vB, +CCCC
  AN_MATH | AN_BRANCH | AN_INT,

  // 35 IF_GE vA, vB, +CCCC
  AN_MATH | AN_BRANCH | AN_INT,

  // 36 IF_GT vA, vB, +CCCC
  AN_MATH | AN_BRANCH | AN_INT,

  // 37 IF_LE vA, vB, +CCCC
  AN_MATH | AN_BRANCH | AN_INT,

  // 38 IF_EQZ vAA, +BBBB
  AN_MATH | AN_BRANCH | AN_INT,

  // 39 IF_NEZ vAA, +BBBB
  AN_MATH | AN_BRANCH | AN_INT,

  // 3A IF_LTZ vAA, +BBBB
  AN_MATH | AN_BRANCH | AN_INT,

  // 3B IF_GEZ vAA, +BBBB
  AN_MATH | AN_BRANCH | AN_INT,

  // 3C IF_GTZ vAA, +BBBB
  AN_MATH | AN_BRANCH | AN_INT,

  // 3D IF_LEZ vAA, +BBBB
  AN_MATH | AN_BRANCH | AN_INT,

  // 3E UNUSED_3E
  AN_NONE,

  // 3F UNUSED_3F
  AN_NONE,

  // 40 UNUSED_40
  AN_NONE,

  // 41 UNUSED_41
  AN_NONE,

  // 42 UNUSED_42
  AN_NONE,

  // 43 UNUSED_43
  AN_NONE,

  // 44 AGET vAA, vBB, vCC
  AN_ARRAYOP,

  // 45 AGET_WIDE vAA, vBB, vCC
  AN_ARRAYOP,

  // 46 AGET_OBJECT vAA, vBB, vCC
  AN_ARRAYOP,

  // 47 AGET_BOOLEAN vAA, vBB, vCC
  AN_ARRAYOP,

  // 48 AGET_BYTE vAA, vBB, vCC
  AN_ARRAYOP,

  // 49 AGET_CHAR vAA, vBB, vCC
  AN_ARRAYOP,

  // 4A AGET_SHORT vAA, vBB, vCC
  AN_ARRAYOP,

  // 4B APUT vAA, vBB, vCC
  AN_ARRAYOP,

  // 4C APUT_WIDE vAA, vBB, vCC
  AN_ARRAYOP,

  // 4D APUT_OBJECT vAA, vBB, vCC
  AN_ARRAYOP,

  // 4E APUT_BOOLEAN vAA, vBB, vCC
  AN_ARRAYOP,

  // 4F APUT_BYTE vAA, vBB, vCC
  AN_ARRAYOP,

  // 50 APUT_CHAR vAA, vBB, vCC
  AN_ARRAYOP,

  // 51 APUT_SHORT vAA, vBB, vCC
  AN_ARRAYOP,

  // 52 IGET vA, vB, field@CCCC
  AN_NONE,

  // 53 IGET_WIDE vA, vB, field@CCCC
  AN_NONE,

  // 54 IGET_OBJECT vA, vB, field@CCCC
  AN_NONE,

  // 55 IGET_BOOLEAN vA, vB, field@CCCC
  AN_NONE,

  // 56 IGET_BYTE vA, vB, field@CCCC
  AN_NONE,

  // 57 IGET_CHAR vA, vB, field@CCCC
  AN_NONE,

  // 58 IGET_SHORT vA, vB, field@CCCC
  AN_NONE,

  // 59 IPUT vA, vB, field@CCCC
  AN_NONE,

  // 5A IPUT_WIDE vA, vB, field@CCCC
  AN_NONE,

  // 5B IPUT_OBJECT vA, vB, field@CCCC
  AN_NONE,

  // 5C IPUT_BOOLEAN vA, vB, field@CCCC
  AN_NONE,

  // 5D IPUT_BYTE vA, vB, field@CCCC
  AN_NONE,

  // 5E IPUT_CHAR vA, vB, field@CCCC
  AN_NONE,

  // 5F IPUT_SHORT vA, vB, field@CCCC
  AN_NONE,

  // 60 SGET vAA, field@BBBB
  AN_NONE,

  // 61 SGET_WIDE vAA, field@BBBB
  AN_NONE,

  // 62 SGET_OBJECT vAA, field@BBBB
  AN_NONE,

  // 63 SGET_BOOLEAN vAA, field@BBBB
  AN_NONE,

  // 64 SGET_BYTE vAA, field@BBBB
  AN_NONE,

  // 65 SGET_CHAR vAA, field@BBBB
  AN_NONE,

  // 66 SGET_SHORT vAA, field@BBBB
  AN_NONE,

  // 67 SPUT vAA, field@BBBB
  AN_NONE,

  // 68 SPUT_WIDE vAA, field@BBBB
  AN_NONE,

  // 69 SPUT_OBJECT vAA, field@BBBB
  AN_NONE,

  // 6A SPUT_BOOLEAN vAA, field@BBBB
  AN_NONE,

  // 6B SPUT_BYTE vAA, field@BBBB
  AN_NONE,

  // 6C SPUT_CHAR vAA, field@BBBB
  AN_NONE,

  // 6D SPUT_SHORT vAA, field@BBBB
  AN_NONE,

  // 6E INVOKE_VIRTUAL {vD, vE, vF, vG, vA}
  AN_INVOKE | AN_HEAVYWEIGHT,

  // 6F INVOKE_SUPER {vD, vE, vF, vG, vA}
  AN_INVOKE | AN_HEAVYWEIGHT,

  // 70 INVOKE_DIRECT {vD, vE, vF, vG, vA}
  AN_INVOKE | AN_HEAVYWEIGHT,

  // 71 INVOKE_STATIC {vD, vE, vF, vG, vA}
  AN_INVOKE | AN_HEAVYWEIGHT,

  // 72 INVOKE_INTERFACE {vD, vE, vF, vG, vA}
  AN_INVOKE | AN_HEAVYWEIGHT,

  // 73 UNUSED_73
  AN_NONE,

  // 74 INVOKE_VIRTUAL_RANGE {vCCCC .. vNNNN}
  AN_INVOKE | AN_HEAVYWEIGHT,

  // 75 INVOKE_SUPER_RANGE {vCCCC .. vNNNN}
  AN_INVOKE | AN_HEAVYWEIGHT,

  // 76 INVOKE_DIRECT_RANGE {vCCCC .. vNNNN}
  AN_INVOKE | AN_HEAVYWEIGHT,

  // 77 INVOKE_STATIC_RANGE {vCCCC .. vNNNN}
  AN_INVOKE | AN_HEAVYWEIGHT,

  // 78 INVOKE_INTERFACE_RANGE {vCCCC .. vNNNN}
  AN_INVOKE | AN_HEAVYWEIGHT,

  // 79 UNUSED_79
  AN_NONE,

  // 7A UNUSED_7A
  AN_NONE,

  // 7B NEG_INT vA, vB
  AN_MATH | AN_INT,

  // 7C NOT_INT vA, vB
  AN_MATH | AN_INT,

  // 7D NEG_LONG vA, vB
  AN_MATH | AN_LONG,

  // 7E NOT_LONG vA, vB
  AN_MATH | AN_LONG,

  // 7F NEG_FLOAT vA, vB
  AN_MATH | AN_FP | AN_SINGLE,

  // 80 NEG_DOUBLE vA, vB
  AN_MATH | AN_FP | AN_DOUBLE,

  // 81 INT_TO_LONG vA, vB
  AN_MATH | AN_INT | AN_LONG,

  // 82 INT_TO_FLOAT vA, vB
  AN_MATH | AN_FP | AN_INT | AN_SINGLE,

  // 83 INT_TO_DOUBLE vA, vB
  AN_MATH | AN_FP | AN_INT | AN_DOUBLE,

  // 84 LONG_TO_INT vA, vB
  AN_MATH | AN_INT | AN_LONG,

  // 85 LONG_TO_FLOAT vA, vB
  AN_MATH | AN_FP | AN_LONG | AN_SINGLE,

  // 86 LONG_TO_DOUBLE vA, vB
  AN_MATH | AN_FP | AN_LONG | AN_DOUBLE,

  // 87 FLOAT_TO_INT vA, vB
  AN_MATH | AN_FP | AN_INT | AN_SINGLE,

  // 88 FLOAT_TO_LONG vA, vB
  AN_MATH | AN_FP | AN_LONG | AN_SINGLE,

  // 89 FLOAT_TO_DOUBLE vA, vB
  AN_MATH | AN_FP | AN_SINGLE | AN_DOUBLE,

  // 8A DOUBLE_TO_INT vA, vB
  AN_MATH | AN_FP | AN_INT | AN_DOUBLE,

  // 8B DOUBLE_TO_LONG vA, vB
  AN_MATH | AN_FP | AN_LONG | AN_DOUBLE,

  // 8C DOUBLE_TO_FLOAT vA, vB
  AN_MATH | AN_FP | AN_SINGLE | AN_DOUBLE,

  // 8D INT_TO_BYTE vA, vB
  AN_MATH | AN_INT,

  // 8E INT_TO_CHAR vA, vB
  AN_MATH | AN_INT,

  // 8F INT_TO_SHORT vA, vB
  AN_MATH | AN_INT,

  // 90 ADD_INT vAA, vBB, vCC
  AN_MATH | AN_INT,

  // 91 SUB_INT vAA, vBB, vCC
  AN_MATH | AN_INT,

  // 92 MUL_INT vAA, vBB, vCC
  AN_MATH | AN_INT,

  // 93 DIV_INT vAA, vBB, vCC
  AN_MATH | AN_INT,

  // 94 REM_INT vAA, vBB, vCC
  AN_MATH | AN_INT,

  // 95 AND_INT vAA, vBB, vCC
  AN_MATH | AN_INT,

  // 96 OR_INT vAA, vBB, vCC
  AN_MATH | AN_INT,

  // 97 XOR_INT vAA, vBB, vCC
  AN_MATH | AN_INT,

  // 98 SHL_INT vAA, vBB, vCC
  AN_MATH | AN_INT,

  // 99 SHR_INT vAA, vBB, vCC
  AN_MATH | AN_INT,

  // 9A USHR_INT vAA, vBB, vCC
  AN_MATH | AN_INT,

  // 9B ADD_LONG vAA, vBB, vCC
  AN_MATH | AN_LONG,

  // 9C SUB_LONG vAA, vBB, vCC
  AN_MATH | AN_LONG,

  // 9D MUL_LONG vAA, vBB, vCC
  AN_MATH | AN_LONG,

  // 9E DIV_LONG vAA, vBB, vCC
  AN_MATH | AN_LONG,

  // 9F REM_LONG vAA, vBB, vCC
  AN_MATH | AN_LONG,

  // A0 AND_LONG vAA, vBB, vCC
  AN_MATH | AN_LONG,

  // A1 OR_LONG vAA, vBB, vCC
  AN_MATH | AN_LONG,

  // A2 XOR_LONG vAA, vBB, vCC
  AN_MATH | AN_LONG,

  // A3 SHL_LONG vAA, vBB, vCC
  AN_MATH | AN_LONG,

  // A4 SHR_LONG vAA, vBB, vCC
  AN_MATH | AN_LONG,

  // A5 USHR_LONG vAA, vBB, vCC
  AN_MATH | AN_LONG,

  // A6 ADD_FLOAT vAA, vBB, vCC
  AN_MATH | AN_FP | AN_SINGLE,

  // A7 SUB_FLOAT vAA, vBB, vCC
  AN_MATH | AN_FP | AN_SINGLE,

  // A8 MUL_FLOAT vAA, vBB, vCC
  AN_MATH | AN_FP | AN_SINGLE,

  // A9 DIV_FLOAT vAA, vBB, vCC
  AN_MATH | AN_FP | AN_SINGLE,

  // AA REM_FLOAT vAA, vBB, vCC
  AN_MATH | AN_FP | AN_SINGLE,

  // AB ADD_DOUBLE vAA, vBB, vCC
  AN_MATH | AN_FP | AN_DOUBLE,

  // AC SUB_DOUBLE vAA, vBB, vCC
  AN_MATH | AN_FP | AN_DOUBLE,

  // AD MUL_DOUBLE vAA, vBB, vCC
  AN_MATH | AN_FP | AN_DOUBLE,

  // AE DIV_DOUBLE vAA, vBB, vCC
  AN_MATH | AN_FP | AN_DOUBLE,

  // AF REM_DOUBLE vAA, vBB, vCC
  AN_MATH | AN_FP | AN_DOUBLE,

  // B0 ADD_INT_2ADDR vA, vB
  AN_MATH | AN_INT,

  // B1 SUB_INT_2ADDR vA, vB
  AN_MATH | AN_INT,

  // B2 MUL_INT_2ADDR vA, vB
  AN_MATH | AN_INT,

  // B3 DIV_INT_2ADDR vA, vB
  AN_MATH | AN_INT,

  // B4 REM_INT_2ADDR vA, vB
  AN_MATH | AN_INT,

  // B5 AND_INT_2ADDR vA, vB
  AN_MATH | AN_INT,

  // B6 OR_INT_2ADDR vA, vB
  AN_MATH | AN_INT,

  // B7 XOR_INT_2ADDR vA, vB
  AN_MATH | AN_INT,

  // B8 SHL_INT_2ADDR vA, vB
  AN_MATH | AN_INT,

  // B9 SHR_INT_2ADDR vA, vB
  AN_MATH | AN_INT,

  // BA USHR_INT_2ADDR vA, vB
  AN_MATH | AN_INT,

  // BB ADD_LONG_2ADDR vA, vB
  AN_MATH | AN_LONG,

  // BC SUB_LONG_2ADDR vA, vB
  AN_MATH | AN_LONG,

  // BD MUL_LONG_2ADDR vA, vB
  AN_MATH | AN_LONG,

  // BE DIV_LONG_2ADDR vA, vB
  AN_MATH | AN_LONG,

  // BF REM_LONG_2ADDR vA, vB
  AN_MATH | AN_LONG,

  // C0 AND_LONG_2ADDR vA, vB
  AN_MATH | AN_LONG,

  // C1 OR_LONG_2ADDR vA, vB
  AN_MATH | AN_LONG,

  // C2 XOR_LONG_2ADDR vA, vB
  AN_MATH | AN_LONG,

  // C3 SHL_LONG_2ADDR vA, vB
  AN_MATH | AN_LONG,

  // C4 SHR_LONG_2ADDR vA, vB
  AN_MATH | AN_LONG,

  // C5 USHR_LONG_2ADDR vA, vB
  AN_MATH | AN_LONG,

  // C6 ADD_FLOAT_2ADDR vA, vB
  AN_MATH | AN_FP | AN_SINGLE,

  // C7 SUB_FLOAT_2ADDR vA, vB
  AN_MATH | AN_FP | AN_SINGLE,

  // C8 MUL_FLOAT_2ADDR vA, vB
  AN_MATH | AN_FP | AN_SINGLE,

  // C9 DIV_FLOAT_2ADDR vA, vB
  AN_MATH | AN_FP | AN_SINGLE,

  // CA REM_FLOAT_2ADDR vA, vB
  AN_MATH | AN_FP | AN_SINGLE,

  // CB ADD_DOUBLE_2ADDR vA, vB
  AN_MATH | AN_FP | AN_DOUBLE,

  // CC SUB_DOUBLE_2ADDR vA, vB
  AN_MATH | AN_FP | AN_DOUBLE,

  // CD MUL_DOUBLE_2ADDR vA, vB
  AN_MATH | AN_FP | AN_DOUBLE,

  // CE DIV_DOUBLE_2ADDR vA, vB
  AN_MATH | AN_FP | AN_DOUBLE,

  // CF REM_DOUBLE_2ADDR vA, vB
  AN_MATH | AN_FP | AN_DOUBLE,

  // D0 ADD_INT_LIT16 vA, vB, #+CCCC
  AN_MATH | AN_INT,

  // D1 RSUB_INT vA, vB, #+CCCC
  AN_MATH | AN_INT,

  // D2 MUL_INT_LIT16 vA, vB, #+CCCC
  AN_MATH | AN_INT,

  // D3 DIV_INT_LIT16 vA, vB, #+CCCC
  AN_MATH | AN_INT,

  // D4 REM_INT_LIT16 vA, vB, #+CCCC
  AN_MATH | AN_INT,

  // D5 AND_INT_LIT16 vA, vB, #+CCCC
  AN_MATH | AN_INT,

  // D6 OR_INT_LIT16 vA, vB, #+CCCC
  AN_MATH | AN_INT,

  // D7 XOR_INT_LIT16 vA, vB, #+CCCC
  AN_MATH | AN_INT,

  // D8 ADD_INT_LIT8 vAA, vBB, #+CC
  AN_MATH | AN_INT,

  // D9 RSUB_INT_LIT8 vAA, vBB, #+CC
  AN_MATH | AN_INT,

  // DA MUL_INT_LIT8 vAA, vBB, #+CC
  AN_MATH | AN_INT,

  // DB DIV_INT_LIT8 vAA, vBB, #+CC
  AN_MATH | AN_INT,

  // DC REM_INT_LIT8 vAA, vBB, #+CC
  AN_MATH | AN_INT,

  // DD AND_INT_LIT8 vAA, vBB, #+CC
  AN_MATH | AN_INT,

  // DE OR_INT_LIT8 vAA, vBB, #+CC
  AN_MATH | AN_INT,

  // DF XOR_INT_LIT8 vAA, vBB, #+CC
  AN_MATH | AN_INT,

  // E0 SHL_INT_LIT8 vAA, vBB, #+CC
  AN_MATH | AN_INT,

  // E1 SHR_INT_LIT8 vAA, vBB, #+CC
  AN_MATH | AN_INT,

  // E2 USHR_INT_LIT8 vAA, vBB, #+CC
  AN_MATH | AN_INT,

  // E3 IGET_VOLATILE
  AN_NONE,

  // E4 IPUT_VOLATILE
  AN_NONE,

  // E5 SGET_VOLATILE
  AN_NONE,

  // E6 SPUT_VOLATILE
  AN_NONE,

  // E7 IGET_OBJECT_VOLATILE
  AN_NONE,

  // E8 IGET_WIDE_VOLATILE
  AN_NONE,

  // E9 IPUT_WIDE_VOLATILE
  AN_NONE,

  // EA SGET_WIDE_VOLATILE
  AN_NONE,

  // EB SPUT_WIDE_VOLATILE
  AN_NONE,

  // EC BREAKPOINT
  AN_NONE,

  // ED THROW_VERIFICATION_ERROR
  AN_HEAVYWEIGHT | AN_BRANCH,

  // EE EXECUTE_INLINE
  AN_NONE,

  // EF EXECUTE_INLINE_RANGE
  AN_NONE,

  // F0 INVOKE_OBJECT_INIT_RANGE
  AN_INVOKE | AN_HEAVYWEIGHT,

  // F1 RETURN_VOID_BARRIER
  AN_BRANCH,

  // F2 IGET_QUICK
  AN_NONE,

  // F3 IGET_WIDE_QUICK
  AN_NONE,

  // F4 IGET_OBJECT_QUICK
  AN_NONE,

  // F5 IPUT_QUICK
  AN_NONE,

  // F6 IPUT_WIDE_QUICK
  AN_NONE,

  // F7 IPUT_OBJECT_QUICK
  AN_NONE,

  // F8 INVOKE_VIRTUAL_QUICK
  AN_INVOKE | AN_HEAVYWEIGHT,

  // F9 INVOKE_VIRTUAL_QUICK_RANGE
  AN_INVOKE | AN_HEAVYWEIGHT,

  // FA INVOKE_SUPER_QUICK
  AN_INVOKE | AN_HEAVYWEIGHT,

  // FB INVOKE_SUPER_QUICK_RANGE
  AN_INVOKE | AN_HEAVYWEIGHT,

  // FC IPUT_OBJECT_VOLATILE
  AN_NONE,

  // FD SGET_OBJECT_VOLATILE
  AN_NONE,

  // FE SPUT_OBJECT_VOLATILE
  AN_NONE,

  // FF UNUSED_FF
  AN_NONE,

  // Beginning of extended MIR opcodes
  // 100 MIR_PHI
  AN_NONE,

  // 101 MIR_COPY
  AN_NONE,

  // 102 MIR_FUSED_CMPL_FLOAT
  AN_NONE,

  // 103 MIR_FUSED_CMPG_FLOAT
  AN_NONE,

  // 104 MIR_FUSED_CMPL_DOUBLE
  AN_NONE,

  // 105 MIR_FUSED_CMPG_DOUBLE
  AN_NONE,

  // 106 MIR_FUSED_CMP_LONG
  AN_NONE,

  // 107 MIR_NOP
  AN_NONE,

  // 108 MIR_NULL_CHECK
  AN_NONE,

  // 109 MIR_RANGE_CHECK
  AN_NONE,

  // 110 MIR_DIV_ZERO_CHECK
  AN_NONE,

  // 111 MIR_CHECK
  AN_NONE,

  // 112 MIR_CHECKPART2
  AN_NONE,

  // 113 MIR_SELECT
  AN_NONE,
};

struct MethodStats {
  int dex_instructions;
  int math_ops;
  int fp_ops;
  int array_ops;
  int branch_ops;
  int heavyweight_ops;
  bool has_computational_loop;
  bool has_switch;
  float math_ratio;
  float fp_ratio;
  float array_ratio;
  float branch_ratio;
  float heavyweight_ratio;
};

void MIRGraph::AnalyzeBlock(BasicBlock* bb, MethodStats* stats) {
  if (bb->visited || (bb->block_type != kDalvikByteCode)) {
    return;
  }
  bool computational_block = true;
  bool has_math = false;
  /*
   * For the purposes of this scan, we want to treat the set of basic blocks broken
   * by an exception edge as a single basic block.  We'll scan forward along the fallthrough
   * edges until we reach an explicit branch or return.
   */
  BasicBlock* ending_bb = bb;
  if (ending_bb->last_mir_insn != NULL) {
    uint32_t ending_flags = analysis_attributes_[ending_bb->last_mir_insn->dalvikInsn.opcode];
    while ((ending_flags & AN_BRANCH) == 0) {
      ending_bb = GetBasicBlock(ending_bb->fall_through);
      ending_flags = analysis_attributes_[ending_bb->last_mir_insn->dalvikInsn.opcode];
    }
  }
  /*
   * Ideally, we'd weight the operations by loop nesting level, but to do so we'd
   * first need to do some expensive loop detection - and the point of this is to make
   * an informed guess before investing in computation.  However, we can cheaply detect
   * many simple loop forms without having to do full dataflow analysis.
   */
  int loop_scale_factor = 1;
  // Simple for and while loops
  if ((ending_bb->taken != NullBasicBlockId) && (ending_bb->fall_through == NullBasicBlockId)) {
    if ((GetBasicBlock(ending_bb->taken)->taken == bb->id) ||
        (GetBasicBlock(ending_bb->taken)->fall_through == bb->id)) {
      loop_scale_factor = 25;
    }
  }
  // Simple do-while loop
  if ((ending_bb->taken != NullBasicBlockId) && (ending_bb->taken == bb->id)) {
    loop_scale_factor = 25;
  }

  BasicBlock* tbb = bb;
  bool done = false;
  while (!done) {
    tbb->visited = true;
    for (MIR* mir = tbb->first_mir_insn; mir != NULL; mir = mir->next) {
      if (static_cast<uint32_t>(mir->dalvikInsn.opcode) >= kMirOpFirst) {
        // Skip any MIR pseudo-op.
        continue;
      }
      uint32_t flags = analysis_attributes_[mir->dalvikInsn.opcode];
      stats->dex_instructions += loop_scale_factor;
      if ((flags & AN_BRANCH) == 0) {
        computational_block &= ((flags & AN_COMPUTATIONAL) != 0);
      } else {
        stats->branch_ops += loop_scale_factor;
      }
      if ((flags & AN_MATH) != 0) {
        stats->math_ops += loop_scale_factor;
        has_math = true;
      }
      if ((flags & AN_FP) != 0) {
        stats->fp_ops += loop_scale_factor;
      }
      if ((flags & AN_ARRAYOP) != 0) {
        stats->array_ops += loop_scale_factor;
      }
      if ((flags & AN_HEAVYWEIGHT) != 0) {
        stats->heavyweight_ops += loop_scale_factor;
      }
      if ((flags & AN_SWITCH) != 0) {
        stats->has_switch = true;
      }
    }
    if (tbb == ending_bb) {
      done = true;
    } else {
      tbb = GetBasicBlock(tbb->fall_through);
    }
  }
  if (has_math && computational_block && (loop_scale_factor > 1)) {
    stats->has_computational_loop = true;
  }
}

bool MIRGraph::ComputeSkipCompilation(MethodStats* stats, bool skip_default) {
  float count = stats->dex_instructions;
  stats->math_ratio = stats->math_ops / count;
  stats->fp_ratio = stats->fp_ops / count;
  stats->branch_ratio = stats->branch_ops / count;
  stats->array_ratio = stats->array_ops / count;
  stats->heavyweight_ratio = stats->heavyweight_ops / count;

  if (cu_->enable_debug & (1 << kDebugShowFilterStats)) {
    LOG(INFO) << "STATS " << stats->dex_instructions << ", math:"
              << stats->math_ratio << ", fp:"
              << stats->fp_ratio << ", br:"
              << stats->branch_ratio << ", hw:"
              << stats->heavyweight_ratio << ", arr:"
              << stats->array_ratio << ", hot:"
              << stats->has_computational_loop << ", "
              << PrettyMethod(cu_->method_idx, *cu_->dex_file);
  }

  // Computation intensive?
  if (stats->has_computational_loop && (stats->heavyweight_ratio < 0.04)) {
    return false;
  }

  // Complex, logic-intensive?
  if (cu_->compiler_driver->GetCompilerOptions().IsSmallMethod(GetNumDalvikInsns()) &&
      stats->branch_ratio > 0.3) {
    return false;
  }

  // Significant floating point?
  if (stats->fp_ratio > 0.05) {
    return false;
  }

  // Significant generic math?
  if (stats->math_ratio > 0.3) {
    return false;
  }

  // If array-intensive, compiling is probably worthwhile.
  if (stats->array_ratio > 0.1) {
    return false;
  }

  // Switch operations benefit greatly from compilation, so go ahead and spend the cycles.
  if (stats->has_switch) {
    return false;
  }

  // If significant in size and high proportion of expensive operations, skip.
  if (cu_->compiler_driver->GetCompilerOptions().IsSmallMethod(GetNumDalvikInsns()) &&
      (stats->heavyweight_ratio > 0.3)) {
    return true;
  }

  return skip_default;
}

 /*
  * Will eventually want this to be a bit more sophisticated and happen at verification time.
  */
bool MIRGraph::SkipCompilation() {
  const CompilerOptions& compiler_options = cu_->compiler_driver->GetCompilerOptions();
  CompilerOptions::CompilerFilter compiler_filter = compiler_options.GetCompilerFilter();
  if (compiler_filter == CompilerOptions::kEverything) {
    return false;
  }

  // Contains a pattern we don't want to compile?
  if (punt_to_interpreter_) {
    return true;
  }

  if (!compiler_options.IsCompilationEnabled() || compiler_filter == CompilerOptions::kProfiled) {
    return true;
  }

  // Set up compilation cutoffs based on current filter mode.
  size_t small_cutoff = 0;
  size_t default_cutoff = 0;
  switch (compiler_filter) {
    case CompilerOptions::kBalanced:
      small_cutoff = compiler_options.GetSmallMethodThreshold();
      default_cutoff = compiler_options.GetLargeMethodThreshold();
      break;
    case CompilerOptions::kSpace:
      small_cutoff = compiler_options.GetTinyMethodThreshold();
      default_cutoff = compiler_options.GetSmallMethodThreshold();
      break;
    case CompilerOptions::kSpeed:
      small_cutoff = compiler_options.GetHugeMethodThreshold();
      default_cutoff = compiler_options.GetHugeMethodThreshold();
      break;
    default:
      LOG(FATAL) << "Unexpected compiler_filter_: " << compiler_filter;
  }

  // If size < cutoff, assume we'll compile - but allow removal.
  bool skip_compilation = (GetNumDalvikInsns() >= default_cutoff);

  /*
   * Filter 1: Huge methods are likely to be machine generated, but some aren't.
   * If huge, assume we won't compile, but allow futher analysis to turn it back on.
   */
  if (compiler_options.IsHugeMethod(GetNumDalvikInsns())) {
    skip_compilation = true;
    // If we're got a huge number of basic blocks, don't bother with further analysis.
    if (static_cast<size_t>(num_blocks_) > (compiler_options.GetHugeMethodThreshold() / 2)) {
      return true;
    }
  } else if (compiler_options.IsLargeMethod(GetNumDalvikInsns()) &&
    /* If it's large and contains no branches, it's likely to be machine generated initialization */
      (GetBranchCount() == 0)) {
    return true;
  } else if (compiler_filter == CompilerOptions::kSpeed) {
    // If not huge, compile.
    return false;
  }

  // Filter 2: Skip class initializers.
  if (((cu_->access_flags & kAccConstructor) != 0) && ((cu_->access_flags & kAccStatic) != 0)) {
    return true;
  }

  // Filter 3: if this method is a special pattern, go ahead and emit the canned pattern.
  if (cu_->compiler_driver->GetMethodInlinerMap() != nullptr &&
      cu_->compiler_driver->GetMethodInlinerMap()->GetMethodInliner(cu_->dex_file)
          ->IsSpecial(cu_->method_idx)) {
    return false;
  }

  // Filter 4: if small, just compile.
  if (GetNumDalvikInsns() < small_cutoff) {
    return false;
  }

  // Analyze graph for:
  //  o floating point computation
  //  o basic blocks contained in loop with heavy arithmetic.
  //  o proportion of conditional branches.

  MethodStats stats;
  memset(&stats, 0, sizeof(stats));

  ClearAllVisitedFlags();
  AllNodesIterator iter(this);
  for (BasicBlock* bb = iter.Next(); bb != NULL; bb = iter.Next()) {
    AnalyzeBlock(bb, &stats);
  }

  return ComputeSkipCompilation(&stats, skip_compilation);
}

void MIRGraph::DoCacheFieldLoweringInfo() {
  // All IGET/IPUT/SGET/SPUT instructions take 2 code units and there must also be a RETURN.
  const uint32_t max_refs = (current_code_item_->insns_size_in_code_units_ - 1u) / 2u;
  ScopedArenaAllocator allocator(&cu_->arena_stack);
  uint16_t* field_idxs =
      reinterpret_cast<uint16_t*>(allocator.Alloc(max_refs * sizeof(uint16_t), kArenaAllocMisc));

  // Find IGET/IPUT/SGET/SPUT insns, store IGET/IPUT fields at the beginning, SGET/SPUT at the end.
  size_t ifield_pos = 0u;
  size_t sfield_pos = max_refs;
  AllNodesIterator iter(this);
  for (BasicBlock* bb = iter.Next(); bb != nullptr; bb = iter.Next()) {
    if (bb->block_type != kDalvikByteCode) {
      continue;
    }
    for (MIR* mir = bb->first_mir_insn; mir != nullptr; mir = mir->next) {
      if (mir->dalvikInsn.opcode >= Instruction::IGET &&
          mir->dalvikInsn.opcode <= Instruction::SPUT_SHORT) {
        const Instruction* insn = Instruction::At(current_code_item_->insns_ + mir->offset);
        // Get field index and try to find it among existing indexes. If found, it's usually among
        // the last few added, so we'll start the search from ifield_pos/sfield_pos. Though this
        // is a linear search, it actually performs much better than map based approach.
        if (mir->dalvikInsn.opcode <= Instruction::IPUT_SHORT) {
          uint16_t field_idx = insn->VRegC_22c();
          size_t i = ifield_pos;
          while (i != 0u && field_idxs[i - 1] != field_idx) {
            --i;
          }
          if (i != 0u) {
            mir->meta.ifield_lowering_info = i - 1;
          } else {
            mir->meta.ifield_lowering_info = ifield_pos;
            field_idxs[ifield_pos++] = field_idx;
          }
        } else {
          uint16_t field_idx = insn->VRegB_21c();
          size_t i = sfield_pos;
          while (i != max_refs && field_idxs[i] != field_idx) {
            ++i;
          }
          if (i != max_refs) {
            mir->meta.sfield_lowering_info = max_refs - i - 1u;
          } else {
            mir->meta.sfield_lowering_info = max_refs - sfield_pos;
            field_idxs[--sfield_pos] = field_idx;
          }
        }
        DCHECK_LE(ifield_pos, sfield_pos);
      }
    }
  }

  if (ifield_pos != 0u) {
    // Resolve instance field infos.
    DCHECK_EQ(ifield_lowering_infos_.Size(), 0u);
    ifield_lowering_infos_.Resize(ifield_pos);
    for (size_t pos = 0u; pos != ifield_pos; ++pos) {
      ifield_lowering_infos_.Insert(MirIFieldLoweringInfo(field_idxs[pos]));
    }
    MirIFieldLoweringInfo::Resolve(cu_->compiler_driver, GetCurrentDexCompilationUnit(),
                                ifield_lowering_infos_.GetRawStorage(), ifield_pos);
  }

  if (sfield_pos != max_refs) {
    // Resolve static field infos.
    DCHECK_EQ(sfield_lowering_infos_.Size(), 0u);
    sfield_lowering_infos_.Resize(max_refs - sfield_pos);
    for (size_t pos = max_refs; pos != sfield_pos;) {
      --pos;
      sfield_lowering_infos_.Insert(MirSFieldLoweringInfo(field_idxs[pos]));
    }
    MirSFieldLoweringInfo::Resolve(cu_->compiler_driver, GetCurrentDexCompilationUnit(),
                                sfield_lowering_infos_.GetRawStorage(), max_refs - sfield_pos);
  }
}

void MIRGraph::DoCacheMethodLoweringInfo() {
  static constexpr uint16_t invoke_types[] = { kVirtual, kSuper, kDirect, kStatic, kInterface };

  // Embed the map value in the entry to avoid extra padding in 64-bit builds.
  struct MapEntry {
    // Map key: target_method_idx, invoke_type, devirt_target. Ordered to avoid padding.
    const MethodReference* devirt_target;
    uint16_t target_method_idx;
    uint16_t invoke_type;
    // Map value.
    uint32_t lowering_info_index;
  };

  // Sort INVOKEs by method index, then by opcode, then by devirtualization target.
  struct MapEntryComparator {
    bool operator()(const MapEntry& lhs, const MapEntry& rhs) const {
      if (lhs.target_method_idx != rhs.target_method_idx) {
        return lhs.target_method_idx < rhs.target_method_idx;
      }
      if (lhs.invoke_type != rhs.invoke_type) {
        return lhs.invoke_type < rhs.invoke_type;
      }
      if (lhs.devirt_target != rhs.devirt_target) {
        if (lhs.devirt_target == nullptr) {
          return true;
        }
        if (rhs.devirt_target == nullptr) {
          return false;
        }
        return devirt_cmp(*lhs.devirt_target, *rhs.devirt_target);
      }
      return false;
    }
    MethodReferenceComparator devirt_cmp;
  };

  ScopedArenaAllocator allocator(&cu_->arena_stack);

  // All INVOKE instructions take 3 code units and there must also be a RETURN.
  uint32_t max_refs = (current_code_item_->insns_size_in_code_units_ - 1u) / 3u;

  // Map invoke key (see MapEntry) to lowering info index and vice versa.
  // The invoke_map and sequential entries are essentially equivalent to Boost.MultiIndex's
  // multi_index_container with one ordered index and one sequential index.
  ScopedArenaSet<MapEntry, MapEntryComparator> invoke_map(MapEntryComparator(),
                                                          allocator.Adapter());
  const MapEntry** sequential_entries = reinterpret_cast<const MapEntry**>(
      allocator.Alloc(max_refs * sizeof(sequential_entries[0]), kArenaAllocMisc));

  // Find INVOKE insns and their devirtualization targets.
  AllNodesIterator iter(this);
  for (BasicBlock* bb = iter.Next(); bb != nullptr; bb = iter.Next()) {
    if (bb->block_type != kDalvikByteCode) {
      continue;
    }
    for (MIR* mir = bb->first_mir_insn; mir != nullptr; mir = mir->next) {
      if (mir->dalvikInsn.opcode >= Instruction::INVOKE_VIRTUAL &&
          mir->dalvikInsn.opcode <= Instruction::INVOKE_INTERFACE_RANGE &&
          mir->dalvikInsn.opcode != Instruction::RETURN_VOID_BARRIER) {
        // Decode target method index and invoke type.
        const Instruction* insn = Instruction::At(current_code_item_->insns_ + mir->offset);
        uint16_t target_method_idx;
        uint16_t invoke_type_idx;
        if (mir->dalvikInsn.opcode <= Instruction::INVOKE_INTERFACE) {
          target_method_idx = insn->VRegB_35c();
          invoke_type_idx = mir->dalvikInsn.opcode - Instruction::INVOKE_VIRTUAL;
        } else {
          target_method_idx = insn->VRegB_3rc();
          invoke_type_idx = mir->dalvikInsn.opcode - Instruction::INVOKE_VIRTUAL_RANGE;
        }

        // Find devirtualization target.
        // TODO: The devirt map is ordered by the dex pc here. Is there a way to get INVOKEs
        // ordered by dex pc as well? That would allow us to keep an iterator to devirt targets
        // and increment it as needed instead of making O(log n) lookups.
        const VerifiedMethod* verified_method = GetCurrentDexCompilationUnit()->GetVerifiedMethod();
        const MethodReference* devirt_target = verified_method->GetDevirtTarget(mir->offset);

        // Try to insert a new entry. If the insertion fails, we will have found an old one.
        MapEntry entry = {
            devirt_target,
            target_method_idx,
            invoke_types[invoke_type_idx],
            static_cast<uint32_t>(invoke_map.size())
        };
        auto it = invoke_map.insert(entry).first;  // Iterator to either the old or the new entry.
        mir->meta.method_lowering_info = it->lowering_info_index;
        // If we didn't actually insert, this will just overwrite an existing value with the same.
        sequential_entries[it->lowering_info_index] = &*it;
      }
    }
  }

  if (invoke_map.empty()) {
    return;
  }

  // Prepare unique method infos, set method info indexes for their MIRs.
  DCHECK_EQ(method_lowering_infos_.Size(), 0u);
  const size_t count = invoke_map.size();
  method_lowering_infos_.Resize(count);
  for (size_t pos = 0u; pos != count; ++pos) {
    const MapEntry* entry = sequential_entries[pos];
    MirMethodLoweringInfo method_info(entry->target_method_idx,
                                      static_cast<InvokeType>(entry->invoke_type));
    if (entry->devirt_target != nullptr) {
      method_info.SetDevirtualizationTarget(*entry->devirt_target);
    }
    method_lowering_infos_.Insert(method_info);
  }
  MirMethodLoweringInfo::Resolve(cu_->compiler_driver, GetCurrentDexCompilationUnit(),
                                 method_lowering_infos_.GetRawStorage(), count);
}

bool MIRGraph::SkipCompilation(const std::string& methodname) {
  return cu_->compiler_driver->SkipCompilation(methodname);
}

}  // namespace art
