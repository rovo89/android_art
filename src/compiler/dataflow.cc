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

#include "compiler_internals.h"
#include "dataflow.h"

namespace art {

/*
 * Main table containing data flow attributes for each bytecode. The
 * first kNumPackedOpcodes entries are for Dalvik bytecode
 * instructions, where extended opcode at the MIR level are appended
 * afterwards.
 *
 * TODO - many optimization flags are incomplete - they will only limit the
 * scope of optimizations but will not cause mis-optimizations.
 */
const int oat_data_flow_attributes[kMirOpLast] = {
  // 00 NOP
  DF_NOP,

  // 01 MOVE vA, vB
  DF_DA | DF_UB | DF_IS_MOVE,

  // 02 MOVE_FROM16 vAA, vBBBB
  DF_DA | DF_UB | DF_IS_MOVE,

  // 03 MOVE_16 vAAAA, vBBBB
  DF_DA | DF_UB | DF_IS_MOVE,

  // 04 MOVE_WIDE vA, vB
  DF_DA | DF_A_WIDE | DF_UB | DF_B_WIDE | DF_IS_MOVE,

  // 05 MOVE_WIDE_FROM16 vAA, vBBBB
  DF_DA | DF_A_WIDE | DF_UB | DF_B_WIDE | DF_IS_MOVE,

  // 06 MOVE_WIDE_16 vAAAA, vBBBB
  DF_DA | DF_A_WIDE | DF_UB | DF_B_WIDE | DF_IS_MOVE,

  // 07 MOVE_OBJECT vA, vB
  DF_DA | DF_UB | DF_NULL_TRANSFER_0 | DF_IS_MOVE | DF_REF_A | DF_REF_B,

  // 08 MOVE_OBJECT_FROM16 vAA, vBBBB
  DF_DA | DF_UB | DF_NULL_TRANSFER_0 | DF_IS_MOVE | DF_REF_A | DF_REF_B,

  // 09 MOVE_OBJECT_16 vAAAA, vBBBB
  DF_DA | DF_UB | DF_NULL_TRANSFER_0 | DF_IS_MOVE | DF_REF_A | DF_REF_B,

  // 0A MOVE_RESULT vAA
  DF_DA,

  // 0B MOVE_RESULT_WIDE vAA
  DF_DA | DF_A_WIDE,

  // 0C MOVE_RESULT_OBJECT vAA
  DF_DA | DF_REF_A,

  // 0D MOVE_EXCEPTION vAA
  DF_DA | DF_REF_A,

  // 0E RETURN_VOID
  DF_NOP,

  // 0F RETURN vAA
  DF_UA,

  // 10 RETURN_WIDE vAA
  DF_UA | DF_A_WIDE,

  // 11 RETURN_OBJECT vAA
  DF_UA | DF_REF_A,

  // 12 CONST_4 vA, #+B
  DF_DA | DF_SETS_CONST,

  // 13 CONST_16 vAA, #+BBBB
  DF_DA | DF_SETS_CONST,

  // 14 CONST vAA, #+BBBBBBBB
  DF_DA | DF_SETS_CONST,

  // 15 CONST_HIGH16 VAA, #+BBBB0000
  DF_DA | DF_SETS_CONST,

  // 16 CONST_WIDE_16 vAA, #+BBBB
  DF_DA | DF_A_WIDE | DF_SETS_CONST,

  // 17 CONST_WIDE_32 vAA, #+BBBBBBBB
  DF_DA | DF_A_WIDE | DF_SETS_CONST,

  // 18 CONST_WIDE vAA, #+BBBBBBBBBBBBBBBB
  DF_DA | DF_A_WIDE | DF_SETS_CONST,

  // 19 CONST_WIDE_HIGH16 vAA, #+BBBB000000000000
  DF_DA | DF_A_WIDE | DF_SETS_CONST,

  // 1A CONST_STRING vAA, string@BBBB
  DF_DA | DF_REF_A,

  // 1B CONST_STRING_JUMBO vAA, string@BBBBBBBB
  DF_DA | DF_REF_A,

  // 1C CONST_CLASS vAA, type@BBBB
  DF_DA | DF_REF_A,

  // 1D MONITOR_ENTER vAA
  DF_UA | DF_NULL_CHK_0 | DF_REF_A,

  // 1E MONITOR_EXIT vAA
  DF_UA | DF_NULL_CHK_0 | DF_REF_A,

  // 1F CHK_CAST vAA, type@BBBB
  DF_UA | DF_REF_A | DF_UMS,

  // 20 INSTANCE_OF vA, vB, type@CCCC
  DF_DA | DF_UB | DF_CORE_A | DF_REF_B | DF_UMS,

  // 21 ARRAY_LENGTH vA, vB
  DF_DA | DF_UB | DF_NULL_CHK_0 | DF_CORE_A | DF_REF_B,

  // 22 NEW_INSTANCE vAA, type@BBBB
  DF_DA | DF_NON_NULL_DST | DF_REF_A | DF_UMS,

  // 23 NEW_ARRAY vA, vB, type@CCCC
  DF_DA | DF_UB | DF_NON_NULL_DST | DF_REF_A | DF_CORE_B | DF_UMS,

  // 24 FILLED_NEW_ARRAY {vD, vE, vF, vG, vA}
  DF_FORMAT_35C | DF_NON_NULL_RET | DF_UMS,

  // 25 FILLED_NEW_ARRAY_RANGE {vCCCC .. vNNNN}, type@BBBB
  DF_FORMAT_3RC | DF_NON_NULL_RET | DF_UMS,

  // 26 FILL_ARRAY_DATA vAA, +BBBBBBBB
  DF_UA | DF_REF_A | DF_UMS,

  // 27 THROW vAA
  DF_UA | DF_REF_A | DF_UMS,

  // 28 GOTO
  DF_NOP,

  // 29 GOTO_16
  DF_NOP,

  // 2A GOTO_32
  DF_NOP,

  // 2B PACKED_SWITCH vAA, +BBBBBBBB
  DF_UA,

  // 2C SPARSE_SWITCH vAA, +BBBBBBBB
  DF_UA,

  // 2D CMPL_FLOAT vAA, vBB, vCC
  DF_DA | DF_UB | DF_UC | DF_FP_B | DF_FP_C | DF_CORE_A,

  // 2E CMPG_FLOAT vAA, vBB, vCC
  DF_DA | DF_UB | DF_UC | DF_FP_B | DF_FP_C | DF_CORE_A,

  // 2F CMPL_DOUBLE vAA, vBB, vCC
  DF_DA | DF_UB | DF_B_WIDE | DF_UC | DF_C_WIDE | DF_FP_B | DF_FP_C | DF_CORE_A,

  // 30 CMPG_DOUBLE vAA, vBB, vCC
  DF_DA | DF_UB | DF_B_WIDE | DF_UC | DF_C_WIDE | DF_FP_B | DF_FP_C | DF_CORE_A,

  // 31 CMP_LONG vAA, vBB, vCC
  DF_DA | DF_UB | DF_B_WIDE | DF_UC | DF_C_WIDE | DF_CORE_A | DF_CORE_B | DF_CORE_C,

  // 32 IF_EQ vA, vB, +CCCC
  DF_UA | DF_UB,

  // 33 IF_NE vA, vB, +CCCC
  DF_UA | DF_UB,

  // 34 IF_LT vA, vB, +CCCC
  DF_UA | DF_UB,

  // 35 IF_GE vA, vB, +CCCC
  DF_UA | DF_UB,

  // 36 IF_GT vA, vB, +CCCC
  DF_UA | DF_UB,

  // 37 IF_LE vA, vB, +CCCC
  DF_UA | DF_UB,

  // 38 IF_EQZ vAA, +BBBB
  DF_UA,

  // 39 IF_NEZ vAA, +BBBB
  DF_UA,

  // 3A IF_LTZ vAA, +BBBB
  DF_UA,

  // 3B IF_GEZ vAA, +BBBB
  DF_UA,

  // 3C IF_GTZ vAA, +BBBB
  DF_UA,

  // 3D IF_LEZ vAA, +BBBB
  DF_UA,

  // 3E UNUSED_3E
  DF_NOP,

  // 3F UNUSED_3F
  DF_NOP,

  // 40 UNUSED_40
  DF_NOP,

  // 41 UNUSED_41
  DF_NOP,

  // 42 UNUSED_42
  DF_NOP,

  // 43 UNUSED_43
  DF_NOP,

  // 44 AGET vAA, vBB, vCC
  DF_DA | DF_UB | DF_UC | DF_NULL_CHK_0 | DF_RANGE_CHK_1 | DF_REF_B | DF_CORE_C,

  // 45 AGET_WIDE vAA, vBB, vCC
  DF_DA | DF_A_WIDE | DF_UB | DF_UC | DF_NULL_CHK_0 | DF_RANGE_CHK_1 | DF_REF_B | DF_CORE_C,

  // 46 AGET_OBJECT vAA, vBB, vCC
  DF_DA | DF_UB | DF_UC | DF_NULL_CHK_0 | DF_RANGE_CHK_1 | DF_REF_A | DF_REF_B | DF_CORE_C,

  // 47 AGET_BOOLEAN vAA, vBB, vCC
  DF_DA | DF_UB | DF_UC | DF_NULL_CHK_0 | DF_RANGE_CHK_1 | DF_REF_B | DF_CORE_C,

  // 48 AGET_BYTE vAA, vBB, vCC
  DF_DA | DF_UB | DF_UC | DF_NULL_CHK_0 | DF_RANGE_CHK_1 | DF_REF_B | DF_CORE_C,

  // 49 AGET_CHAR vAA, vBB, vCC
  DF_DA | DF_UB | DF_UC | DF_NULL_CHK_0 | DF_RANGE_CHK_1 | DF_REF_B | DF_CORE_C,

  // 4A AGET_SHORT vAA, vBB, vCC
  DF_DA | DF_UB | DF_UC | DF_NULL_CHK_0 | DF_RANGE_CHK_1 | DF_REF_B | DF_CORE_C,

  // 4B APUT vAA, vBB, vCC
  DF_UA | DF_UB | DF_UC | DF_NULL_CHK_1 | DF_RANGE_CHK_2 | DF_REF_B | DF_CORE_C,

  // 4C APUT_WIDE vAA, vBB, vCC
  DF_UA | DF_A_WIDE | DF_UB | DF_UC | DF_NULL_CHK_2 | DF_RANGE_CHK_3 | DF_REF_B | DF_CORE_C,

  // 4D APUT_OBJECT vAA, vBB, vCC
  DF_UA | DF_UB | DF_UC | DF_NULL_CHK_1 | DF_RANGE_CHK_2 | DF_REF_A | DF_REF_B | DF_CORE_C,

  // 4E APUT_BOOLEAN vAA, vBB, vCC
  DF_UA | DF_UB | DF_UC | DF_NULL_CHK_1 | DF_RANGE_CHK_2 | DF_REF_B | DF_CORE_C,

  // 4F APUT_BYTE vAA, vBB, vCC
  DF_UA | DF_UB | DF_UC | DF_NULL_CHK_1 | DF_RANGE_CHK_2 | DF_REF_B | DF_CORE_C,

  // 50 APUT_CHAR vAA, vBB, vCC
  DF_UA | DF_UB | DF_UC | DF_NULL_CHK_1 | DF_RANGE_CHK_2 | DF_REF_B | DF_CORE_C,

  // 51 APUT_SHORT vAA, vBB, vCC
  DF_UA | DF_UB | DF_UC | DF_NULL_CHK_1 | DF_RANGE_CHK_2 | DF_REF_B | DF_CORE_C,

  // 52 IGET vA, vB, field@CCCC
  DF_DA | DF_UB | DF_NULL_CHK_0 | DF_REF_B,

  // 53 IGET_WIDE vA, vB, field@CCCC
  DF_DA | DF_A_WIDE | DF_UB | DF_NULL_CHK_0 | DF_REF_B,

  // 54 IGET_OBJECT vA, vB, field@CCCC
  DF_DA | DF_UB | DF_NULL_CHK_0 | DF_REF_A | DF_REF_B,

  // 55 IGET_BOOLEAN vA, vB, field@CCCC
  DF_DA | DF_UB | DF_NULL_CHK_0 | DF_REF_B,

  // 56 IGET_BYTE vA, vB, field@CCCC
  DF_DA | DF_UB | DF_NULL_CHK_0 | DF_REF_B,

  // 57 IGET_CHAR vA, vB, field@CCCC
  DF_DA | DF_UB | DF_NULL_CHK_0 | DF_REF_B,

  // 58 IGET_SHORT vA, vB, field@CCCC
  DF_DA | DF_UB | DF_NULL_CHK_0 | DF_REF_B,

  // 59 IPUT vA, vB, field@CCCC
  DF_UA | DF_UB | DF_NULL_CHK_1 | DF_REF_B,

  // 5A IPUT_WIDE vA, vB, field@CCCC
  DF_UA | DF_A_WIDE | DF_UB | DF_NULL_CHK_2 | DF_REF_B,

  // 5B IPUT_OBJECT vA, vB, field@CCCC
  DF_UA | DF_UB | DF_NULL_CHK_1 | DF_REF_A | DF_REF_B,

  // 5C IPUT_BOOLEAN vA, vB, field@CCCC
  DF_UA | DF_UB | DF_NULL_CHK_1 | DF_REF_B,

  // 5D IPUT_BYTE vA, vB, field@CCCC
  DF_UA | DF_UB | DF_NULL_CHK_1 | DF_REF_B,

  // 5E IPUT_CHAR vA, vB, field@CCCC
  DF_UA | DF_UB | DF_NULL_CHK_1 | DF_REF_B,

  // 5F IPUT_SHORT vA, vB, field@CCCC
  DF_UA | DF_UB | DF_NULL_CHK_1 | DF_REF_B,

  // 60 SGET vAA, field@BBBB
  DF_DA | DF_UMS,

  // 61 SGET_WIDE vAA, field@BBBB
  DF_DA | DF_A_WIDE | DF_UMS,

  // 62 SGET_OBJECT vAA, field@BBBB
  DF_DA | DF_REF_A | DF_UMS,

  // 63 SGET_BOOLEAN vAA, field@BBBB
  DF_DA | DF_UMS,

  // 64 SGET_BYTE vAA, field@BBBB
  DF_DA | DF_UMS,

  // 65 SGET_CHAR vAA, field@BBBB
  DF_DA | DF_UMS,

  // 66 SGET_SHORT vAA, field@BBBB
  DF_DA | DF_UMS,

  // 67 SPUT vAA, field@BBBB
  DF_UA | DF_UMS,

  // 68 SPUT_WIDE vAA, field@BBBB
  DF_UA | DF_A_WIDE | DF_UMS,

  // 69 SPUT_OBJECT vAA, field@BBBB
  DF_UA | DF_REF_A | DF_UMS,

  // 6A SPUT_BOOLEAN vAA, field@BBBB
  DF_UA | DF_UMS,

  // 6B SPUT_BYTE vAA, field@BBBB
  DF_UA | DF_UMS,

  // 6C SPUT_CHAR vAA, field@BBBB
  DF_UA | DF_UMS,

  // 6D SPUT_SHORT vAA, field@BBBB
  DF_UA | DF_UMS,

  // 6E INVOKE_VIRTUAL {vD, vE, vF, vG, vA}
  DF_FORMAT_35C | DF_NULL_CHK_OUT0 | DF_UMS,

  // 6F INVOKE_SUPER {vD, vE, vF, vG, vA}
  DF_FORMAT_35C | DF_NULL_CHK_OUT0 | DF_UMS,

  // 70 INVOKE_DIRECT {vD, vE, vF, vG, vA}
  DF_FORMAT_35C | DF_NULL_CHK_OUT0 | DF_UMS,

  // 71 INVOKE_STATIC {vD, vE, vF, vG, vA}
  DF_FORMAT_35C | DF_UMS,

  // 72 INVOKE_INTERFACE {vD, vE, vF, vG, vA}
  DF_FORMAT_35C | DF_UMS,

  // 73 UNUSED_73
  DF_NOP,

  // 74 INVOKE_VIRTUAL_RANGE {vCCCC .. vNNNN}
  DF_FORMAT_3RC | DF_NULL_CHK_OUT0 | DF_UMS,

  // 75 INVOKE_SUPER_RANGE {vCCCC .. vNNNN}
  DF_FORMAT_3RC | DF_NULL_CHK_OUT0 | DF_UMS,

  // 76 INVOKE_DIRECT_RANGE {vCCCC .. vNNNN}
  DF_FORMAT_3RC | DF_NULL_CHK_OUT0 | DF_UMS,

  // 77 INVOKE_STATIC_RANGE {vCCCC .. vNNNN}
  DF_FORMAT_3RC | DF_UMS,

  // 78 INVOKE_INTERFACE_RANGE {vCCCC .. vNNNN}
  DF_FORMAT_3RC | DF_UMS,

  // 79 UNUSED_79
  DF_NOP,

  // 7A UNUSED_7A
  DF_NOP,

  // 7B NEG_INT vA, vB
  DF_DA | DF_UB | DF_CORE_A | DF_CORE_B,

  // 7C NOT_INT vA, vB
  DF_DA | DF_UB | DF_CORE_A | DF_CORE_B,

  // 7D NEG_LONG vA, vB
  DF_DA | DF_A_WIDE | DF_UB | DF_B_WIDE | DF_CORE_A | DF_CORE_B,

  // 7E NOT_LONG vA, vB
  DF_DA | DF_A_WIDE | DF_UB | DF_B_WIDE | DF_CORE_A | DF_CORE_B,

  // 7F NEG_FLOAT vA, vB
  DF_DA | DF_UB | DF_FP_A | DF_FP_B,

  // 80 NEG_DOUBLE vA, vB
  DF_DA | DF_A_WIDE | DF_UB | DF_B_WIDE | DF_FP_A | DF_FP_B,

  // 81 INT_TO_LONG vA, vB
  DF_DA | DF_A_WIDE | DF_UB | DF_CORE_A | DF_CORE_B,

  // 82 INT_TO_FLOAT vA, vB
  DF_DA | DF_UB | DF_FP_A | DF_CORE_B,

  // 83 INT_TO_DOUBLE vA, vB
  DF_DA | DF_A_WIDE | DF_UB | DF_FP_A | DF_CORE_B,

  // 84 LONG_TO_INT vA, vB
  DF_DA | DF_UB | DF_B_WIDE | DF_CORE_A | DF_CORE_B,

  // 85 LONG_TO_FLOAT vA, vB
  DF_DA | DF_UB | DF_B_WIDE | DF_FP_A | DF_CORE_B,

  // 86 LONG_TO_DOUBLE vA, vB
  DF_DA | DF_A_WIDE | DF_UB | DF_B_WIDE | DF_FP_A | DF_CORE_B,

  // 87 FLOAT_TO_INT vA, vB
  DF_DA | DF_UB | DF_FP_B | DF_CORE_A,

  // 88 FLOAT_TO_LONG vA, vB
  DF_DA | DF_A_WIDE | DF_UB | DF_FP_B | DF_CORE_A,

  // 89 FLOAT_TO_DOUBLE vA, vB
  DF_DA | DF_A_WIDE | DF_UB | DF_FP_A | DF_FP_B,

  // 8A DOUBLE_TO_INT vA, vB
  DF_DA | DF_UB | DF_B_WIDE | DF_FP_B | DF_CORE_A,

  // 8B DOUBLE_TO_LONG vA, vB
  DF_DA | DF_A_WIDE | DF_UB | DF_B_WIDE | DF_FP_B | DF_CORE_A,

  // 8C DOUBLE_TO_FLOAT vA, vB
  DF_DA | DF_UB | DF_B_WIDE | DF_FP_A | DF_FP_B,

  // 8D INT_TO_BYTE vA, vB
  DF_DA | DF_UB | DF_CORE_A | DF_CORE_B,

  // 8E INT_TO_CHAR vA, vB
  DF_DA | DF_UB | DF_CORE_A | DF_CORE_B,

  // 8F INT_TO_SHORT vA, vB
  DF_DA | DF_UB | DF_CORE_A | DF_CORE_B,

  // 90 ADD_INT vAA, vBB, vCC
  DF_DA | DF_UB | DF_UC | DF_CORE_A | DF_CORE_B | DF_CORE_C,

  // 91 SUB_INT vAA, vBB, vCC
  DF_DA | DF_UB | DF_UC | DF_CORE_A | DF_CORE_B | DF_CORE_C,

  // 92 MUL_INT vAA, vBB, vCC
  DF_DA | DF_UB | DF_UC | DF_CORE_A | DF_CORE_B | DF_CORE_C,

  // 93 DIV_INT vAA, vBB, vCC
  DF_DA | DF_UB | DF_UC | DF_CORE_A | DF_CORE_B | DF_CORE_C,

  // 94 REM_INT vAA, vBB, vCC
  DF_DA | DF_UB | DF_UC | DF_CORE_A | DF_CORE_B | DF_CORE_C,

  // 95 AND_INT vAA, vBB, vCC
  DF_DA | DF_UB | DF_UC | DF_CORE_A | DF_CORE_B | DF_CORE_C,

  // 96 OR_INT vAA, vBB, vCC
  DF_DA | DF_UB | DF_UC | DF_CORE_A | DF_CORE_B | DF_CORE_C,

  // 97 XOR_INT vAA, vBB, vCC
  DF_DA | DF_UB | DF_UC | DF_CORE_A | DF_CORE_B | DF_CORE_C,

  // 98 SHL_INT vAA, vBB, vCC
  DF_DA | DF_UB | DF_UC | DF_CORE_A | DF_CORE_B | DF_CORE_C,

  // 99 SHR_INT vAA, vBB, vCC
  DF_DA | DF_UB | DF_UC | DF_CORE_A | DF_CORE_B | DF_CORE_C,

  // 9A USHR_INT vAA, vBB, vCC
  DF_DA | DF_UB | DF_UC | DF_CORE_A | DF_CORE_B | DF_CORE_C,

  // 9B ADD_LONG vAA, vBB, vCC
  DF_DA | DF_A_WIDE | DF_UB | DF_B_WIDE | DF_UC | DF_C_WIDE | DF_CORE_A | DF_CORE_B | DF_CORE_C,

  // 9C SUB_LONG vAA, vBB, vCC
  DF_DA | DF_A_WIDE | DF_UB | DF_B_WIDE | DF_UC | DF_C_WIDE | DF_CORE_A | DF_CORE_B | DF_CORE_C,

  // 9D MUL_LONG vAA, vBB, vCC
  DF_DA | DF_A_WIDE | DF_UB | DF_B_WIDE | DF_UC | DF_C_WIDE | DF_CORE_A | DF_CORE_B | DF_CORE_C,

  // 9E DIV_LONG vAA, vBB, vCC
  DF_DA | DF_A_WIDE | DF_UB | DF_B_WIDE | DF_UC | DF_C_WIDE | DF_CORE_A | DF_CORE_B | DF_CORE_C,

  // 9F REM_LONG vAA, vBB, vCC
  DF_DA | DF_A_WIDE | DF_UB | DF_B_WIDE | DF_UC | DF_C_WIDE | DF_CORE_A | DF_CORE_B | DF_CORE_C,

  // A0 AND_LONG vAA, vBB, vCC
  DF_DA | DF_A_WIDE | DF_UB | DF_B_WIDE | DF_UC | DF_C_WIDE | DF_CORE_A | DF_CORE_B | DF_CORE_C,

  // A1 OR_LONG vAA, vBB, vCC
  DF_DA | DF_A_WIDE | DF_UB | DF_B_WIDE | DF_UC | DF_C_WIDE | DF_CORE_A | DF_CORE_B | DF_CORE_C,

  // A2 XOR_LONG vAA, vBB, vCC
  DF_DA | DF_A_WIDE | DF_UB | DF_B_WIDE | DF_UC | DF_C_WIDE | DF_CORE_A | DF_CORE_B | DF_CORE_C,

  // A3 SHL_LONG vAA, vBB, vCC
  DF_DA | DF_A_WIDE | DF_UB | DF_B_WIDE | DF_UC | DF_CORE_A | DF_CORE_B | DF_CORE_C,

  // A4 SHR_LONG vAA, vBB, vCC
  DF_DA | DF_A_WIDE | DF_UB | DF_B_WIDE | DF_UC | DF_CORE_A | DF_CORE_B | DF_CORE_C,

  // A5 USHR_LONG vAA, vBB, vCC
  DF_DA | DF_A_WIDE | DF_UB | DF_B_WIDE | DF_UC | DF_CORE_A | DF_CORE_B | DF_CORE_C,

  // A6 ADD_FLOAT vAA, vBB, vCC
  DF_DA | DF_UB | DF_UC | DF_FP_A | DF_FP_B | DF_FP_C,

  // A7 SUB_FLOAT vAA, vBB, vCC
  DF_DA | DF_UB | DF_UC | DF_FP_A | DF_FP_B | DF_FP_C,

  // A8 MUL_FLOAT vAA, vBB, vCC
  DF_DA | DF_UB | DF_UC | DF_FP_A | DF_FP_B | DF_FP_C,

  // A9 DIV_FLOAT vAA, vBB, vCC
  DF_DA | DF_UB | DF_UC | DF_FP_A | DF_FP_B | DF_FP_C,

  // AA REM_FLOAT vAA, vBB, vCC
  DF_DA | DF_UB | DF_UC | DF_FP_A | DF_FP_B | DF_FP_C,

  // AB ADD_DOUBLE vAA, vBB, vCC
  DF_DA | DF_A_WIDE | DF_UB | DF_B_WIDE | DF_UC | DF_C_WIDE | DF_FP_A | DF_FP_B | DF_FP_C,

  // AC SUB_DOUBLE vAA, vBB, vCC
  DF_DA | DF_A_WIDE | DF_UB | DF_B_WIDE | DF_UC | DF_C_WIDE | DF_FP_A | DF_FP_B | DF_FP_C,

  // AD MUL_DOUBLE vAA, vBB, vCC
  DF_DA | DF_A_WIDE | DF_UB | DF_B_WIDE | DF_UC | DF_C_WIDE | DF_FP_A | DF_FP_B | DF_FP_C,

  // AE DIV_DOUBLE vAA, vBB, vCC
  DF_DA | DF_A_WIDE | DF_UB | DF_B_WIDE | DF_UC | DF_C_WIDE | DF_FP_A | DF_FP_B | DF_FP_C,

  // AF REM_DOUBLE vAA, vBB, vCC
  DF_DA | DF_A_WIDE | DF_UB | DF_B_WIDE | DF_UC | DF_C_WIDE | DF_FP_A | DF_FP_B | DF_FP_C,

  // B0 ADD_INT_2ADDR vA, vB
  DF_DA | DF_UA | DF_UB | DF_CORE_A | DF_CORE_B,

  // B1 SUB_INT_2ADDR vA, vB
  DF_DA | DF_UA | DF_UB | DF_CORE_A | DF_CORE_B,

  // B2 MUL_INT_2ADDR vA, vB
  DF_DA | DF_UA | DF_UB | DF_CORE_A | DF_CORE_B,

  // B3 DIV_INT_2ADDR vA, vB
  DF_DA | DF_UA | DF_UB | DF_CORE_A | DF_CORE_B,

  // B4 REM_INT_2ADDR vA, vB
  DF_DA | DF_UA | DF_UB | DF_CORE_A | DF_CORE_B,

  // B5 AND_INT_2ADDR vA, vB
  DF_DA | DF_UA | DF_UB | DF_CORE_A | DF_CORE_B,

  // B6 OR_INT_2ADDR vA, vB
  DF_DA | DF_UA | DF_UB | DF_CORE_A | DF_CORE_B,

  // B7 XOR_INT_2ADDR vA, vB
  DF_DA | DF_UA | DF_UB | DF_CORE_A | DF_CORE_B,

  // B8 SHL_INT_2ADDR vA, vB
  DF_DA | DF_UA | DF_UB | DF_CORE_A | DF_CORE_B,

  // B9 SHR_INT_2ADDR vA, vB
  DF_DA | DF_UA | DF_UB | DF_CORE_A | DF_CORE_B,

  // BA USHR_INT_2ADDR vA, vB
  DF_DA | DF_UA | DF_UB | DF_CORE_A | DF_CORE_B,

  // BB ADD_LONG_2ADDR vA, vB
  DF_DA | DF_A_WIDE | DF_UA | DF_UB | DF_B_WIDE | DF_CORE_A | DF_CORE_B,

  // BC SUB_LONG_2ADDR vA, vB
  DF_DA | DF_A_WIDE | DF_UA | DF_UB | DF_B_WIDE | DF_CORE_A | DF_CORE_B,

  // BD MUL_LONG_2ADDR vA, vB
  DF_DA | DF_A_WIDE | DF_UA | DF_UB | DF_B_WIDE | DF_CORE_A | DF_CORE_B,

  // BE DIV_LONG_2ADDR vA, vB
  DF_DA | DF_A_WIDE | DF_UA | DF_UB | DF_B_WIDE | DF_CORE_A | DF_CORE_B,

  // BF REM_LONG_2ADDR vA, vB
  DF_DA | DF_A_WIDE | DF_UA | DF_UB | DF_B_WIDE | DF_CORE_A | DF_CORE_B,

  // C0 AND_LONG_2ADDR vA, vB
  DF_DA | DF_A_WIDE | DF_UA | DF_UB | DF_B_WIDE | DF_CORE_A | DF_CORE_B,

  // C1 OR_LONG_2ADDR vA, vB
  DF_DA | DF_A_WIDE | DF_UA | DF_UB | DF_B_WIDE | DF_CORE_A | DF_CORE_B,

  // C2 XOR_LONG_2ADDR vA, vB
  DF_DA | DF_A_WIDE | DF_UA | DF_UB | DF_B_WIDE | DF_CORE_A | DF_CORE_B,

  // C3 SHL_LONG_2ADDR vA, vB
  DF_DA | DF_A_WIDE | DF_UA | DF_UB | DF_CORE_A | DF_CORE_B,

  // C4 SHR_LONG_2ADDR vA, vB
  DF_DA | DF_A_WIDE | DF_UA | DF_UB | DF_CORE_A | DF_CORE_B,

  // C5 USHR_LONG_2ADDR vA, vB
  DF_DA | DF_A_WIDE | DF_UA | DF_UB | DF_CORE_A | DF_CORE_B,

  // C6 ADD_FLOAT_2ADDR vA, vB
  DF_DA | DF_UA | DF_UB | DF_FP_A | DF_FP_B,

  // C7 SUB_FLOAT_2ADDR vA, vB
  DF_DA | DF_UA | DF_UB | DF_FP_A | DF_FP_B,

  // C8 MUL_FLOAT_2ADDR vA, vB
  DF_DA | DF_UA | DF_UB | DF_FP_A | DF_FP_B,

  // C9 DIV_FLOAT_2ADDR vA, vB
  DF_DA | DF_UA | DF_UB | DF_FP_A | DF_FP_B,

  // CA REM_FLOAT_2ADDR vA, vB
  DF_DA | DF_UA | DF_UB | DF_FP_A | DF_FP_B,

  // CB ADD_DOUBLE_2ADDR vA, vB
  DF_DA | DF_A_WIDE | DF_UA | DF_UB | DF_B_WIDE | DF_FP_A | DF_FP_B,

  // CC SUB_DOUBLE_2ADDR vA, vB
  DF_DA | DF_A_WIDE | DF_UA | DF_UB | DF_B_WIDE | DF_FP_A | DF_FP_B,

  // CD MUL_DOUBLE_2ADDR vA, vB
  DF_DA | DF_A_WIDE | DF_UA | DF_UB | DF_B_WIDE | DF_FP_A | DF_FP_B,

  // CE DIV_DOUBLE_2ADDR vA, vB
  DF_DA | DF_A_WIDE | DF_UA | DF_UB | DF_B_WIDE | DF_FP_A | DF_FP_B,

  // CF REM_DOUBLE_2ADDR vA, vB
  DF_DA | DF_A_WIDE | DF_UA | DF_UB | DF_B_WIDE | DF_FP_A | DF_FP_B,

  // D0 ADD_INT_LIT16 vA, vB, #+CCCC
  DF_DA | DF_UB | DF_CORE_A | DF_CORE_B,

  // D1 RSUB_INT vA, vB, #+CCCC
  DF_DA | DF_UB | DF_CORE_A | DF_CORE_B,

  // D2 MUL_INT_LIT16 vA, vB, #+CCCC
  DF_DA | DF_UB | DF_CORE_A | DF_CORE_B,

  // D3 DIV_INT_LIT16 vA, vB, #+CCCC
  DF_DA | DF_UB | DF_CORE_A | DF_CORE_B,

  // D4 REM_INT_LIT16 vA, vB, #+CCCC
  DF_DA | DF_UB | DF_CORE_A | DF_CORE_B,

  // D5 AND_INT_LIT16 vA, vB, #+CCCC
  DF_DA | DF_UB | DF_CORE_A | DF_CORE_B,

  // D6 OR_INT_LIT16 vA, vB, #+CCCC
  DF_DA | DF_UB | DF_CORE_A | DF_CORE_B,

  // D7 XOR_INT_LIT16 vA, vB, #+CCCC
  DF_DA | DF_UB | DF_CORE_A | DF_CORE_B,

  // D8 ADD_INT_LIT8 vAA, vBB, #+CC
  DF_DA | DF_UB | DF_CORE_A | DF_CORE_B,

  // D9 RSUB_INT_LIT8 vAA, vBB, #+CC
  DF_DA | DF_UB | DF_CORE_A | DF_CORE_B,

  // DA MUL_INT_LIT8 vAA, vBB, #+CC
  DF_DA | DF_UB | DF_CORE_A | DF_CORE_B,

  // DB DIV_INT_LIT8 vAA, vBB, #+CC
  DF_DA | DF_UB | DF_CORE_A | DF_CORE_B,

  // DC REM_INT_LIT8 vAA, vBB, #+CC
  DF_DA | DF_UB | DF_CORE_A | DF_CORE_B,

  // DD AND_INT_LIT8 vAA, vBB, #+CC
  DF_DA | DF_UB | DF_CORE_A | DF_CORE_B,

  // DE OR_INT_LIT8 vAA, vBB, #+CC
  DF_DA | DF_UB | DF_CORE_A | DF_CORE_B,

  // DF XOR_INT_LIT8 vAA, vBB, #+CC
  DF_DA | DF_UB | DF_CORE_A | DF_CORE_B,

  // E0 SHL_INT_LIT8 vAA, vBB, #+CC
  DF_DA | DF_UB | DF_CORE_A | DF_CORE_B,

  // E1 SHR_INT_LIT8 vAA, vBB, #+CC
  DF_DA | DF_UB | DF_CORE_A | DF_CORE_B,

  // E2 USHR_INT_LIT8 vAA, vBB, #+CC
  DF_DA | DF_UB | DF_CORE_A | DF_CORE_B,

  // E3 IGET_VOLATILE
  DF_DA | DF_UB | DF_NULL_CHK_0 | DF_REF_B,

  // E4 IPUT_VOLATILE
  DF_UA | DF_UB | DF_NULL_CHK_1 | DF_REF_B,

  // E5 SGET_VOLATILE
  DF_DA | DF_UMS,

  // E6 SPUT_VOLATILE
  DF_UA | DF_UMS,

  // E7 IGET_OBJECT_VOLATILE
  DF_DA | DF_UB | DF_NULL_CHK_0 | DF_REF_A | DF_REF_B,

  // E8 IGET_WIDE_VOLATILE
  DF_DA | DF_A_WIDE | DF_UB | DF_NULL_CHK_0 | DF_REF_B,

  // E9 IPUT_WIDE_VOLATILE
  DF_UA | DF_A_WIDE | DF_UB | DF_NULL_CHK_2 | DF_REF_B,

  // EA SGET_WIDE_VOLATILE
  DF_DA | DF_A_WIDE | DF_UMS,

  // EB SPUT_WIDE_VOLATILE
  DF_UA | DF_A_WIDE | DF_UMS,

  // EC BREAKPOINT
  DF_NOP,

  // ED THROW_VERIFICATION_ERROR
  DF_NOP | DF_UMS,

  // EE EXECUTE_INLINE
  DF_FORMAT_35C,

  // EF EXECUTE_INLINE_RANGE
  DF_FORMAT_3RC,

  // F0 INVOKE_OBJECT_INIT_RANGE
  DF_NOP | DF_NULL_CHK_0,

  // F1 RETURN_VOID_BARRIER
  DF_NOP,

  // F2 IGET_QUICK
  DF_DA | DF_UB | DF_NULL_CHK_0,

  // F3 IGET_WIDE_QUICK
  DF_DA | DF_A_WIDE | DF_UB | DF_NULL_CHK_0,

  // F4 IGET_OBJECT_QUICK
  DF_DA | DF_UB | DF_NULL_CHK_0,

  // F5 IPUT_QUICK
  DF_UA | DF_UB | DF_NULL_CHK_1,

  // F6 IPUT_WIDE_QUICK
  DF_UA | DF_A_WIDE | DF_UB | DF_NULL_CHK_2,

  // F7 IPUT_OBJECT_QUICK
  DF_UA | DF_UB | DF_NULL_CHK_1,

  // F8 INVOKE_VIRTUAL_QUICK
  DF_FORMAT_35C | DF_NULL_CHK_OUT0 | DF_UMS,

  // F9 INVOKE_VIRTUAL_QUICK_RANGE
  DF_FORMAT_3RC | DF_NULL_CHK_OUT0 | DF_UMS,

  // FA INVOKE_SUPER_QUICK
  DF_FORMAT_35C | DF_NULL_CHK_OUT0 | DF_UMS,

  // FB INVOKE_SUPER_QUICK_RANGE
  DF_FORMAT_3RC | DF_NULL_CHK_OUT0 | DF_UMS,

  // FC IPUT_OBJECT_VOLATILE
  DF_UA | DF_UB | DF_NULL_CHK_1 | DF_REF_A | DF_REF_B,

  // FD SGET_OBJECT_VOLATILE
  DF_DA | DF_REF_A | DF_UMS,

  // FE SPUT_OBJECT_VOLATILE
  DF_UA | DF_REF_A | DF_UMS,

  // FF UNUSED_FF
  DF_NOP,

  // Beginning of extended MIR opcodes
  // 100 MIR_PHI
  DF_DA | DF_NULL_TRANSFER_N,

  // 101 MIR_COPY
  DF_DA | DF_UB | DF_IS_MOVE,

  // 102 MIR_FUSED_CMPL_FLOAT
  DF_UA | DF_UB | DF_FP_A | DF_FP_B,

  // 103 MIR_FUSED_CMPG_FLOAT
  DF_UA | DF_UB | DF_FP_A | DF_FP_B,

  // 104 MIR_FUSED_CMPL_DOUBLE
  DF_UA | DF_A_WIDE | DF_UB | DF_B_WIDE | DF_FP_A | DF_FP_B,

  // 105 MIR_FUSED_CMPG_DOUBLE
  DF_UA | DF_A_WIDE | DF_UB | DF_B_WIDE | DF_FP_A | DF_FP_B,

  // 106 MIR_FUSED_CMP_LONG
  DF_UA | DF_A_WIDE | DF_UB | DF_B_WIDE | DF_CORE_A | DF_CORE_B,

  // 107 MIR_NOP
  DF_NOP,

  // 108 MIR_NULL_CHECK
  0,

  // 109 MIR_RANGE_CHECK
  0,

  // 110 MIR_DIV_ZERO_CHECK
  0,

  // 111 MIR_CHECK
  0,
};

/* Return the base virtual register for a SSA name */
int SRegToVReg(const CompilationUnit* cu, int ssa_reg)
{
  DCHECK_LT(ssa_reg, static_cast<int>(cu->ssa_base_vregs->num_used));
  return GET_ELEM_N(cu->ssa_base_vregs, int, ssa_reg);
}

int SRegToSubscript(const CompilationUnit* cu, int ssa_reg)
{
  DCHECK(ssa_reg < static_cast<int>(cu->ssa_subscripts->num_used));
  return GET_ELEM_N(cu->ssa_subscripts, int, ssa_reg);
}

static int GetSSAUseCount(CompilationUnit* cu, int s_reg)
{
  DCHECK(s_reg < static_cast<int>(cu->raw_use_counts.num_used));
  return cu->raw_use_counts.elem_list[s_reg];
}

static std::string GetSSAName(const CompilationUnit* cu, int ssa_reg)
{
  return StringPrintf("v%d_%d", SRegToVReg(cu, ssa_reg), SRegToSubscript(cu, ssa_reg));
}

// Similar to GetSSAName, but if ssa name represents an immediate show that as well.
static std::string GetSSANameWithConst(const CompilationUnit* cu, int ssa_reg, bool singles_only)
{
  if (cu->reg_location[ssa_reg].is_const) {
    if (!singles_only && cu->reg_location[ssa_reg].wide) {
      int64_t immval = cu->constant_values[ssa_reg + 1];
      immval = (immval << 32) | cu->constant_values[ssa_reg];
      return StringPrintf("v%d_%d#0x%llx", SRegToVReg(cu, ssa_reg),
                          SRegToSubscript(cu, ssa_reg), immval);
    } else {
      int32_t immval = cu->constant_values[ssa_reg];
      return StringPrintf("v%d_%d#0x%x", SRegToVReg(cu, ssa_reg),
                          SRegToSubscript(cu, ssa_reg), immval);
    }
  } else {
    return StringPrintf("v%d_%d", SRegToVReg(cu, ssa_reg), SRegToSubscript(cu, ssa_reg));
  }
}


char* GetDalvikDisassembly(CompilationUnit* cu, const MIR* mir)
{
  DecodedInstruction insn = mir->dalvikInsn;
  std::string str;
  int flags = 0;
  int opcode = insn.opcode;
  char* ret;
  bool nop = false;
  SSARepresentation* ssa_rep = mir->ssa_rep;
  Instruction::Format dalvik_format = Instruction::k10x;  // Default to no-operand format
  int defs = (ssa_rep != NULL) ? ssa_rep->num_defs : 0;
  int uses = (ssa_rep != NULL) ? ssa_rep->num_uses : 0;

  // Handle special cases.
  if ((opcode == kMirOpCheck) || (opcode == kMirOpCheckPart2)) {
    str.append(extended_mir_op_names[opcode - kMirOpFirst]);
    str.append(": ");
    // Recover the original Dex instruction
    insn = mir->meta.throw_insn->dalvikInsn;
    ssa_rep = mir->meta.throw_insn->ssa_rep;
    defs = ssa_rep->num_defs;
    uses = ssa_rep->num_uses;
    opcode = insn.opcode;
  } else if (opcode == kMirOpNop) {
    str.append("[");
    insn.opcode = mir->meta.original_opcode;
    opcode = mir->meta.original_opcode;
    nop = true;
  }

  if (opcode >= kMirOpFirst) {
    str.append(extended_mir_op_names[opcode - kMirOpFirst]);
  } else {
    dalvik_format = Instruction::FormatOf(insn.opcode);
    flags = Instruction::FlagsOf(insn.opcode);
    str.append(Instruction::Name(insn.opcode));
  }

  if (opcode == kMirOpPhi) {
    int* incoming = reinterpret_cast<int*>(insn.vB);
    str.append(StringPrintf(" %s = (%s",
               GetSSANameWithConst(cu, ssa_rep->defs[0], true).c_str(),
               GetSSANameWithConst(cu, ssa_rep->uses[0], true).c_str()));
    str.append(StringPrintf(":%d",incoming[0]));
    int i;
    for (i = 1; i < uses; i++) {
      str.append(StringPrintf(", %s:%d",
                              GetSSANameWithConst(cu, ssa_rep->uses[i], true).c_str(),
                              incoming[i]));
    }
    str.append(")");
  } else if (flags & Instruction::kBranch) {
    // For branches, decode the instructions to print out the branch targets.
    int offset = 0;
    switch (dalvik_format) {
      case Instruction::k21t:
        str.append(StringPrintf(" %s,", GetSSANameWithConst(cu, ssa_rep->uses[0], false).c_str()));
        offset = insn.vB;
        break;
      case Instruction::k22t:
        str.append(StringPrintf(" %s, %s,", GetSSANameWithConst(cu, ssa_rep->uses[0], false).c_str(),
                   GetSSANameWithConst(cu, ssa_rep->uses[cu->reg_location[ssa_rep->uses[0]].wide
                                       ? 2 : 1], false).c_str()));
        offset = insn.vC;
        break;
      case Instruction::k10t:
      case Instruction::k20t:
      case Instruction::k30t:
        offset = insn.vA;
        break;
      default:
        LOG(FATAL) << "Unexpected branch format " << dalvik_format << " from " << insn.opcode;
    }
    str.append(StringPrintf(" 0x%x (%c%x)", mir->offset + offset,
                            offset > 0 ? '+' : '-', offset > 0 ? offset : -offset));
  } else {
    // For invokes-style formats, treat wide regs as a pair of singles
    bool show_singles = ((dalvik_format == Instruction::k35c) ||
                         (dalvik_format == Instruction::k3rc));
    if (defs != 0) {
      str.append(StringPrintf(" %s", GetSSANameWithConst(cu, ssa_rep->defs[0], false).c_str()));
      if (uses != 0) {
        str.append(", ");
      }
    }
    for (int i = 0; i < uses; i++) {
      str.append(
          StringPrintf(" %s", GetSSANameWithConst(cu, ssa_rep->uses[i], show_singles).c_str()));
      if (!show_singles && cu->reg_location[i].wide) {
        // For the listing, skip the high sreg.
        i++;
      }
      if (i != (uses -1)) {
        str.append(",");
      }
    }
    switch (dalvik_format) {
      case Instruction::k11n: // Add one immediate from vB
      case Instruction::k21s:
      case Instruction::k31i:
      case Instruction::k21h:
        str.append(StringPrintf(", #%d", insn.vB));
        break;
      case Instruction::k51l: // Add one wide immediate
        str.append(StringPrintf(", #%lld", insn.vB_wide));
        break;
      case Instruction::k21c: // One register, one string/type/method index
      case Instruction::k31c:
        str.append(StringPrintf(", index #%d", insn.vB));
        break;
      case Instruction::k22c: // Two registers, one string/type/method index
        str.append(StringPrintf(", index #%d", insn.vC));
        break;
      case Instruction::k22s: // Add one immediate from vC
      case Instruction::k22b:
        str.append(StringPrintf(", #%d", insn.vC));
        break;
      default:
        ; // Nothing left to print
      }
  }
  if (nop) {
    str.append("]--optimized away");
  }
  int length = str.length() + 1;
  ret = static_cast<char*>(NewMem(cu, length, false, kAllocDFInfo));
  strncpy(ret, str.c_str(), length);
  return ret;
}

/* Any register that is used before being defined is considered live-in */
static void HandleLiveInUse(CompilationUnit* cu, ArenaBitVector* use_v, ArenaBitVector* def_v,
                            ArenaBitVector* live_in_v, int dalvik_reg_id)
{
  SetBit(cu, use_v, dalvik_reg_id);
  if (!IsBitSet(def_v, dalvik_reg_id)) {
    SetBit(cu, live_in_v, dalvik_reg_id);
  }
}

/* Mark a reg as being defined */
static void HandleDef(CompilationUnit* cu, ArenaBitVector* def_v, int dalvik_reg_id)
{
  SetBit(cu, def_v, dalvik_reg_id);
}

/*
 * Find out live-in variables for natural loops. Variables that are live-in in
 * the main loop body are considered to be defined in the entry block.
 */
bool FindLocalLiveIn(CompilationUnit* cu, BasicBlock* bb)
{
  MIR* mir;
  ArenaBitVector *use_v, *def_v, *live_in_v;

  if (bb->data_flow_info == NULL) return false;

  use_v = bb->data_flow_info->use_v =
      AllocBitVector(cu, cu->num_dalvik_registers, false, kBitMapUse);
  def_v = bb->data_flow_info->def_v =
      AllocBitVector(cu, cu->num_dalvik_registers, false, kBitMapDef);
  live_in_v = bb->data_flow_info->live_in_v =
      AllocBitVector(cu, cu->num_dalvik_registers, false,
                        kBitMapLiveIn);

  for (mir = bb->first_mir_insn; mir != NULL; mir = mir->next) {
    int df_attributes = oat_data_flow_attributes[mir->dalvikInsn.opcode];
    DecodedInstruction *d_insn = &mir->dalvikInsn;

    if (df_attributes & DF_HAS_USES) {
      if (df_attributes & DF_UA) {
        HandleLiveInUse(cu, use_v, def_v, live_in_v, d_insn->vA);
        if (df_attributes & DF_A_WIDE) {
          HandleLiveInUse(cu, use_v, def_v, live_in_v, d_insn->vA+1);
        }
      }
      if (df_attributes & DF_UB) {
        HandleLiveInUse(cu, use_v, def_v, live_in_v, d_insn->vB);
        if (df_attributes & DF_B_WIDE) {
          HandleLiveInUse(cu, use_v, def_v, live_in_v, d_insn->vB+1);
        }
      }
      if (df_attributes & DF_UC) {
        HandleLiveInUse(cu, use_v, def_v, live_in_v, d_insn->vC);
        if (df_attributes & DF_C_WIDE) {
          HandleLiveInUse(cu, use_v, def_v, live_in_v, d_insn->vC+1);
        }
      }
    }
    if (df_attributes & DF_FORMAT_35C) {
      for (unsigned int i = 0; i < d_insn->vA; i++) {
        HandleLiveInUse(cu, use_v, def_v, live_in_v, d_insn->arg[i]);
      }
    }
    if (df_attributes & DF_FORMAT_3RC) {
      for (unsigned int i = 0; i < d_insn->vA; i++) {
        HandleLiveInUse(cu, use_v, def_v, live_in_v, d_insn->vC+i);
      }
    }
    if (df_attributes & DF_HAS_DEFS) {
      HandleDef(cu, def_v, d_insn->vA);
      if (df_attributes & DF_A_WIDE) {
        HandleDef(cu, def_v, d_insn->vA+1);
      }
    }
  }
  return true;
}

static int AddNewSReg(CompilationUnit* cu, int v_reg)
{
  // Compiler temps always have a subscript of 0
  int subscript = (v_reg < 0) ? 0 : ++cu->ssa_last_defs[v_reg];
  int ssa_reg = cu->num_ssa_regs++;
  InsertGrowableList(cu, cu->ssa_base_vregs, v_reg);
  InsertGrowableList(cu, cu->ssa_subscripts, subscript);
  std::string ssa_name = GetSSAName(cu, ssa_reg);
  char* name = static_cast<char*>(NewMem(cu, ssa_name.length() + 1, false, kAllocDFInfo));
  strncpy(name, ssa_name.c_str(), ssa_name.length() + 1);
  InsertGrowableList(cu, cu->ssa_strings, reinterpret_cast<uintptr_t>(name));
  DCHECK_EQ(cu->ssa_base_vregs->num_used, cu->ssa_subscripts->num_used);
  return ssa_reg;
}

/* Find out the latest SSA register for a given Dalvik register */
static void HandleSSAUse(CompilationUnit* cu, int* uses, int dalvik_reg, int reg_index)
{
  DCHECK((dalvik_reg >= 0) && (dalvik_reg < cu->num_dalvik_registers));
  uses[reg_index] = cu->vreg_to_ssa_map[dalvik_reg];
}

/* Setup a new SSA register for a given Dalvik register */
static void HandleSSADef(CompilationUnit* cu, int* defs, int dalvik_reg, int reg_index)
{
  DCHECK((dalvik_reg >= 0) && (dalvik_reg < cu->num_dalvik_registers));
  int ssa_reg = AddNewSReg(cu, dalvik_reg);
  cu->vreg_to_ssa_map[dalvik_reg] = ssa_reg;
  defs[reg_index] = ssa_reg;
}

/* Look up new SSA names for format_35c instructions */
static void DataFlowSSAFormat35C(CompilationUnit* cu, MIR* mir)
{
  DecodedInstruction *d_insn = &mir->dalvikInsn;
  int num_uses = d_insn->vA;
  int i;

  mir->ssa_rep->num_uses = num_uses;
  mir->ssa_rep->uses = static_cast<int*>(NewMem(cu, sizeof(int) * num_uses, true, kAllocDFInfo));
  // NOTE: will be filled in during type & size inference pass
  mir->ssa_rep->fp_use = static_cast<bool*>(NewMem(cu, sizeof(bool) * num_uses, true,
                                                 kAllocDFInfo));

  for (i = 0; i < num_uses; i++) {
    HandleSSAUse(cu, mir->ssa_rep->uses, d_insn->arg[i], i);
  }
}

/* Look up new SSA names for format_3rc instructions */
static void DataFlowSSAFormat3RC(CompilationUnit* cu, MIR* mir)
{
  DecodedInstruction *d_insn = &mir->dalvikInsn;
  int num_uses = d_insn->vA;
  int i;

  mir->ssa_rep->num_uses = num_uses;
  mir->ssa_rep->uses = static_cast<int*>(NewMem(cu, sizeof(int) * num_uses, true, kAllocDFInfo));
  // NOTE: will be filled in during type & size inference pass
  mir->ssa_rep->fp_use = static_cast<bool*>(NewMem(cu, sizeof(bool) * num_uses, true,
                                                 kAllocDFInfo));

  for (i = 0; i < num_uses; i++) {
    HandleSSAUse(cu, mir->ssa_rep->uses, d_insn->vC+i, i);
  }
}

/* Entry function to convert a block into SSA representation */
bool DoSSAConversion(CompilationUnit* cu, BasicBlock* bb)
{
  MIR* mir;

  if (bb->data_flow_info == NULL) return false;

  for (mir = bb->first_mir_insn; mir != NULL; mir = mir->next) {
    mir->ssa_rep = static_cast<struct SSARepresentation *>(NewMem(cu, sizeof(SSARepresentation),
                                                                 true, kAllocDFInfo));

    int df_attributes = oat_data_flow_attributes[mir->dalvikInsn.opcode];

      // If not a pseudo-op, note non-leaf or can throw
    if (static_cast<int>(mir->dalvikInsn.opcode) <
        static_cast<int>(kNumPackedOpcodes)) {
      int flags = Instruction::FlagsOf(mir->dalvikInsn.opcode);

      if (flags & Instruction::kThrow) {
        cu->attrs &= ~METHOD_IS_THROW_FREE;
      }

      if (flags & Instruction::kInvoke) {
        cu->attrs &= ~METHOD_IS_LEAF;
      }
    }

    int num_uses = 0;

    if (df_attributes & DF_FORMAT_35C) {
      DataFlowSSAFormat35C(cu, mir);
      continue;
    }

    if (df_attributes & DF_FORMAT_3RC) {
      DataFlowSSAFormat3RC(cu, mir);
      continue;
    }

    if (df_attributes & DF_HAS_USES) {
      if (df_attributes & DF_UA) {
        num_uses++;
        if (df_attributes & DF_A_WIDE) {
          num_uses ++;
        }
      }
      if (df_attributes & DF_UB) {
        num_uses++;
        if (df_attributes & DF_B_WIDE) {
          num_uses ++;
        }
      }
      if (df_attributes & DF_UC) {
        num_uses++;
        if (df_attributes & DF_C_WIDE) {
          num_uses ++;
        }
      }
    }

    if (num_uses) {
      mir->ssa_rep->num_uses = num_uses;
      mir->ssa_rep->uses = static_cast<int*>(NewMem(cu, sizeof(int) * num_uses, false,
                                                   kAllocDFInfo));
      mir->ssa_rep->fp_use = static_cast<bool*>(NewMem(cu, sizeof(bool) * num_uses, false,
                                                     kAllocDFInfo));
    }

    int num_defs = 0;

    if (df_attributes & DF_HAS_DEFS) {
      num_defs++;
      if (df_attributes & DF_A_WIDE) {
        num_defs++;
      }
    }

    if (num_defs) {
      mir->ssa_rep->num_defs = num_defs;
      mir->ssa_rep->defs = static_cast<int*>(NewMem(cu, sizeof(int) * num_defs, false,
                                                   kAllocDFInfo));
      mir->ssa_rep->fp_def = static_cast<bool*>(NewMem(cu, sizeof(bool) * num_defs, false,
                                                     kAllocDFInfo));
    }

    DecodedInstruction *d_insn = &mir->dalvikInsn;

    if (df_attributes & DF_HAS_USES) {
      num_uses = 0;
      if (df_attributes & DF_UA) {
        mir->ssa_rep->fp_use[num_uses] = df_attributes & DF_FP_A;
        HandleSSAUse(cu, mir->ssa_rep->uses, d_insn->vA, num_uses++);
        if (df_attributes & DF_A_WIDE) {
          mir->ssa_rep->fp_use[num_uses] = df_attributes & DF_FP_A;
          HandleSSAUse(cu, mir->ssa_rep->uses, d_insn->vA+1, num_uses++);
        }
      }
      if (df_attributes & DF_UB) {
        mir->ssa_rep->fp_use[num_uses] = df_attributes & DF_FP_B;
        HandleSSAUse(cu, mir->ssa_rep->uses, d_insn->vB, num_uses++);
        if (df_attributes & DF_B_WIDE) {
          mir->ssa_rep->fp_use[num_uses] = df_attributes & DF_FP_B;
          HandleSSAUse(cu, mir->ssa_rep->uses, d_insn->vB+1, num_uses++);
        }
      }
      if (df_attributes & DF_UC) {
        mir->ssa_rep->fp_use[num_uses] = df_attributes & DF_FP_C;
        HandleSSAUse(cu, mir->ssa_rep->uses, d_insn->vC, num_uses++);
        if (df_attributes & DF_C_WIDE) {
          mir->ssa_rep->fp_use[num_uses] = df_attributes & DF_FP_C;
          HandleSSAUse(cu, mir->ssa_rep->uses, d_insn->vC+1, num_uses++);
        }
      }
    }
    if (df_attributes & DF_HAS_DEFS) {
      mir->ssa_rep->fp_def[0] = df_attributes & DF_FP_A;
      HandleSSADef(cu, mir->ssa_rep->defs, d_insn->vA, 0);
      if (df_attributes & DF_A_WIDE) {
        mir->ssa_rep->fp_def[1] = df_attributes & DF_FP_A;
        HandleSSADef(cu, mir->ssa_rep->defs, d_insn->vA+1, 1);
      }
    }
  }

  if (!cu->disable_dataflow) {
    /*
     * Take a snapshot of Dalvik->SSA mapping at the end of each block. The
     * input to PHI nodes can be derived from the snapshot of all
     * predecessor blocks.
     */
    bb->data_flow_info->vreg_to_ssa_map =
        static_cast<int*>(NewMem(cu, sizeof(int) * cu->num_dalvik_registers, false,
                                 kAllocDFInfo));

    memcpy(bb->data_flow_info->vreg_to_ssa_map, cu->vreg_to_ssa_map,
           sizeof(int) * cu->num_dalvik_registers);
  }
  return true;
}

/* Setup a constant value for opcodes thare have the DF_SETS_CONST attribute */
static void SetConstant(CompilationUnit* cu, int ssa_reg, int value)
{
  SetBit(cu, cu->is_constant_v, ssa_reg);
  cu->constant_values[ssa_reg] = value;
}

bool DoConstantPropogation(CompilationUnit* cu, BasicBlock* bb)
{
  MIR* mir;
  ArenaBitVector *is_constant_v = cu->is_constant_v;

  for (mir = bb->first_mir_insn; mir != NULL; mir = mir->next) {
    int df_attributes = oat_data_flow_attributes[mir->dalvikInsn.opcode];

    DecodedInstruction *d_insn = &mir->dalvikInsn;

    if (!(df_attributes & DF_HAS_DEFS)) continue;

    /* Handle instructions that set up constants directly */
    if (df_attributes & DF_SETS_CONST) {
      if (df_attributes & DF_DA) {
        switch (d_insn->opcode) {
          case Instruction::CONST_4:
          case Instruction::CONST_16:
          case Instruction::CONST:
            SetConstant(cu, mir->ssa_rep->defs[0], d_insn->vB);
            break;
          case Instruction::CONST_HIGH16:
            SetConstant(cu, mir->ssa_rep->defs[0], d_insn->vB << 16);
            break;
          case Instruction::CONST_WIDE_16:
          case Instruction::CONST_WIDE_32:
            SetConstant(cu, mir->ssa_rep->defs[0], d_insn->vB);
            SetConstant(cu, mir->ssa_rep->defs[1], 0);
            break;
          case Instruction::CONST_WIDE:
            SetConstant(cu, mir->ssa_rep->defs[0], static_cast<int>(d_insn->vB_wide));
            SetConstant(cu, mir->ssa_rep->defs[1], static_cast<int>(d_insn->vB_wide >> 32));
            break;
          case Instruction::CONST_WIDE_HIGH16:
            SetConstant(cu, mir->ssa_rep->defs[0], 0);
            SetConstant(cu, mir->ssa_rep->defs[1], d_insn->vB << 16);
            break;
          default:
            break;
        }
      }
      /* Handle instructions that set up constants directly */
    } else if (df_attributes & DF_IS_MOVE) {
      int i;

      for (i = 0; i < mir->ssa_rep->num_uses; i++) {
        if (!IsBitSet(is_constant_v, mir->ssa_rep->uses[i])) break;
      }
      /* Move a register holding a constant to another register */
      if (i == mir->ssa_rep->num_uses) {
        SetConstant(cu, mir->ssa_rep->defs[0],
                    cu->constant_values[mir->ssa_rep->uses[0]]);
        if (df_attributes & DF_A_WIDE) {
          SetConstant(cu, mir->ssa_rep->defs[1],
                      cu->constant_values[mir->ssa_rep->uses[1]]);
        }
      }
    }
  }
  /* TODO: implement code to handle arithmetic operations */
  return true;
}

/* Setup the basic data structures for SSA conversion */
void CompilerInitializeSSAConversion(CompilationUnit* cu)
{
  int i;
  int num_dalvik_reg = cu->num_dalvik_registers;

  cu->ssa_base_vregs =
      static_cast<GrowableList*>(NewMem(cu, sizeof(GrowableList), false, kAllocDFInfo));
  cu->ssa_subscripts =
      static_cast<GrowableList*>(NewMem(cu, sizeof(GrowableList), false, kAllocDFInfo));
  cu->ssa_strings =
      static_cast<GrowableList*>(NewMem(cu, sizeof(GrowableList), false, kAllocDFInfo));
  // Create the ssa mappings, estimating the max size
  CompilerInitGrowableList(cu, cu->ssa_base_vregs,
                      num_dalvik_reg + cu->def_count + 128,
                      kListSSAtoDalvikMap);
  CompilerInitGrowableList(cu, cu->ssa_subscripts,
                      num_dalvik_reg + cu->def_count + 128,
                      kListSSAtoDalvikMap);
  CompilerInitGrowableList(cu, cu->ssa_strings,
                      num_dalvik_reg + cu->def_count + 128,
                      kListSSAtoDalvikMap);
  /*
   * Initial number of SSA registers is equal to the number of Dalvik
   * registers.
   */
  cu->num_ssa_regs = num_dalvik_reg;

  /*
   * Initialize the SSA2Dalvik map list. For the first num_dalvik_reg elements,
   * the subscript is 0 so we use the ENCODE_REG_SUB macro to encode the value
   * into "(0 << 16) | i"
   */
  for (i = 0; i < num_dalvik_reg; i++) {
    InsertGrowableList(cu, cu->ssa_base_vregs, i);
    InsertGrowableList(cu, cu->ssa_subscripts, 0);
    std::string ssa_name = GetSSAName(cu, i);
    char* name = static_cast<char*>(NewMem(cu, ssa_name.length() + 1, true, kAllocDFInfo));
    strncpy(name, ssa_name.c_str(), ssa_name.length() + 1);
    InsertGrowableList(cu, cu->ssa_strings, reinterpret_cast<uintptr_t>(name));
  }

  /*
   * Initialize the DalvikToSSAMap map. There is one entry for each
   * Dalvik register, and the SSA names for those are the same.
   */
  cu->vreg_to_ssa_map =
      static_cast<int*>(NewMem(cu, sizeof(int) * num_dalvik_reg, false, kAllocDFInfo));
  /* Keep track of the higest def for each dalvik reg */
  cu->ssa_last_defs =
      static_cast<int*>(NewMem(cu, sizeof(int) * num_dalvik_reg, false, kAllocDFInfo));

  for (i = 0; i < num_dalvik_reg; i++) {
    cu->vreg_to_ssa_map[i] = i;
    cu->ssa_last_defs[i] = 0;
  }

  /* Add ssa reg for Method* */
  cu->method_sreg = AddNewSReg(cu, SSA_METHOD_BASEREG);

  /*
   * Allocate the BasicBlockDataFlow structure for the entry and code blocks
   */
  GrowableListIterator iterator;

  GrowableListIteratorInit(&cu->block_list, &iterator);

  while (true) {
    BasicBlock* bb = reinterpret_cast<BasicBlock*>(GrowableListIteratorNext(&iterator));
    if (bb == NULL) break;
    if (bb->hidden == true) continue;
    if (bb->block_type == kDalvikByteCode ||
      bb->block_type == kEntryBlock ||
      bb->block_type == kExitBlock) {
      bb->data_flow_info = static_cast<BasicBlockDataFlow*>(NewMem(cu, sizeof(BasicBlockDataFlow),
                                                                 true, kAllocDFInfo));
      }
  }
}

/* Clear the visited flag for each BB */
bool ClearVisitedFlag(struct CompilationUnit* cu, struct BasicBlock* bb)
{
  bb->visited = false;
  return true;
}

void DataFlowAnalysisDispatcher(CompilationUnit* cu,
                                   bool (*func)(CompilationUnit*, BasicBlock*),
                                   DataFlowAnalysisMode dfa_mode,
                                   bool is_iterative)
{
  bool change = true;

  while (change) {
    change = false;

    switch (dfa_mode) {
      /* Scan all blocks and perform the operations specified in func */
      case kAllNodes:
        {
          GrowableListIterator iterator;
          GrowableListIteratorInit(&cu->block_list, &iterator);
          while (true) {
            BasicBlock* bb = reinterpret_cast<BasicBlock*>(GrowableListIteratorNext(&iterator));
            if (bb == NULL) break;
            if (bb->hidden == true) continue;
              change |= (*func)(cu, bb);
          }
        }
        break;
      /* Scan reachable blocks and perform the ops specified in func. */
      case kReachableNodes:
        {
          int num_reachable_blocks = cu->num_reachable_blocks;
          int idx;
          const GrowableList *block_list = &cu->block_list;

          for (idx = 0; idx < num_reachable_blocks; idx++) {
            int block_idx = cu->dfs_order.elem_list[idx];
            BasicBlock* bb =
                reinterpret_cast<BasicBlock*>( GrowableListGetElement(block_list, block_idx));
            change |= (*func)(cu, bb);
          }
        }
        break;

      /* Scan reachable blocks by pre-order dfs and invoke func on each. */
      case kPreOrderDFSTraversal:
        {
          int num_reachable_blocks = cu->num_reachable_blocks;
          int idx;
          const GrowableList *block_list = &cu->block_list;

          for (idx = 0; idx < num_reachable_blocks; idx++) {
            int dfs_idx = cu->dfs_order.elem_list[idx];
            BasicBlock* bb =
                reinterpret_cast<BasicBlock*>(GrowableListGetElement(block_list, dfs_idx));
            change |= (*func)(cu, bb);
            }
        }
        break;
      /* Scan reachable blocks post-order dfs and invoke func on each. */
      case kPostOrderDFSTraversal:
        {
          int num_reachable_blocks = cu->num_reachable_blocks;
          int idx;
          const GrowableList *block_list = &cu->block_list;

          for (idx = num_reachable_blocks - 1; idx >= 0; idx--) {
            int dfs_idx = cu->dfs_order.elem_list[idx];
            BasicBlock* bb =
                reinterpret_cast<BasicBlock *>( GrowableListGetElement(block_list, dfs_idx));
            change |= (*func)(cu, bb);
            }
        }
        break;
      /* Scan reachable post-order dom tree and invoke func on each. */
      case kPostOrderDOMTraversal:
        {
          int num_reachable_blocks = cu->num_reachable_blocks;
          int idx;
          const GrowableList *block_list = &cu->block_list;

          for (idx = 0; idx < num_reachable_blocks; idx++) {
            int dom_idx = cu->dom_post_order_traversal.elem_list[idx];
            BasicBlock* bb =
                reinterpret_cast<BasicBlock*>( GrowableListGetElement(block_list, dom_idx));
            change |= (*func)(cu, bb);
          }
        }
        break;
      /* Scan reachable blocks reverse post-order dfs, invoke func on each */
      case kReversePostOrderTraversal:
        {
          int num_reachable_blocks = cu->num_reachable_blocks;
          int idx;
          const GrowableList *block_list = &cu->block_list;

          for (idx = num_reachable_blocks - 1; idx >= 0; idx--) {
            int rev_idx = cu->dfs_post_order.elem_list[idx];
            BasicBlock* bb =
                reinterpret_cast<BasicBlock*>(GrowableListGetElement(block_list, rev_idx));
            change |= (*func)(cu, bb);
            }
        }
        break;
      default:
        LOG(FATAL) << "Unknown traversal mode: " << dfa_mode;
    }
    /* If is_iterative is false, exit the loop after the first iteration */
    change &= is_iterative;
  }
}

/* Advance to next strictly dominated MIR node in an extended basic block */
static MIR* AdvanceMIR(CompilationUnit* cu, BasicBlock** p_bb, MIR* mir,
                       ArenaBitVector* bv, bool clear_mark) {
  BasicBlock* bb = *p_bb;
  if (mir != NULL) {
    mir = mir->next;
    if (mir == NULL) {
      bb = bb->fall_through;
      if ((bb == NULL) || bb->predecessors->num_used != 1) {
        mir = NULL;
      } else {
        if (bv) {
          SetBit(cu, bv, bb->id);
        }
      *p_bb = bb;
      mir = bb->first_mir_insn;
      }
    }
  }
  if (mir && clear_mark) {
    mir->optimization_flags &= ~MIR_MARK;
  }
  return mir;
}

/*
 * To be used at an invoke mir.  If the logically next mir node represents
 * a move-result, return it.  Else, return NULL.  If a move-result exists,
 * it is required to immediately follow the invoke with no intervening
 * opcodes or incoming arcs.  However, if the result of the invoke is not
 * used, a move-result may not be present.
 */
MIR* FindMoveResult(CompilationUnit* cu, BasicBlock* bb, MIR* mir)
{
  BasicBlock* tbb = bb;
  mir = AdvanceMIR(cu, &tbb, mir, NULL, false);
  while (mir != NULL) {
    int opcode = mir->dalvikInsn.opcode;
    if ((mir->dalvikInsn.opcode == Instruction::MOVE_RESULT) ||
        (mir->dalvikInsn.opcode == Instruction::MOVE_RESULT_OBJECT) ||
        (mir->dalvikInsn.opcode == Instruction::MOVE_RESULT_WIDE)) {
      break;
    }
    // Keep going if pseudo op, otherwise terminate
    if (opcode < kNumPackedOpcodes) {
      mir = NULL;
    } else {
      mir = AdvanceMIR(cu, &tbb, mir, NULL, false);
    }
  }
  return mir;
}

static void SquashDupRangeChecks(CompilationUnit* cu, BasicBlock** p_bp, MIR* mir,
                                 int array_sreg, int index_sreg)
{
  while (true) {
    mir = AdvanceMIR(cu, p_bp, mir, NULL, false);
    if (!mir) {
      break;
    }
    if ((mir->ssa_rep == NULL) ||
        (mir->optimization_flags & MIR_IGNORE_RANGE_CHECK)) {
       continue;
    }
    int check_array = INVALID_SREG;
    int check_index = INVALID_SREG;
    switch (mir->dalvikInsn.opcode) {
      case Instruction::AGET:
      case Instruction::AGET_OBJECT:
      case Instruction::AGET_BOOLEAN:
      case Instruction::AGET_BYTE:
      case Instruction::AGET_CHAR:
      case Instruction::AGET_SHORT:
      case Instruction::AGET_WIDE:
        check_array = mir->ssa_rep->uses[0];
        check_index = mir->ssa_rep->uses[1];
        break;
      case Instruction::APUT:
      case Instruction::APUT_OBJECT:
      case Instruction::APUT_SHORT:
      case Instruction::APUT_CHAR:
      case Instruction::APUT_BYTE:
      case Instruction::APUT_BOOLEAN:
        check_array = mir->ssa_rep->uses[1];
        check_index = mir->ssa_rep->uses[2];
        break;
      case Instruction::APUT_WIDE:
        check_array = mir->ssa_rep->uses[2];
        check_index = mir->ssa_rep->uses[3];
      default:
        break;
    }
    if (check_array == INVALID_SREG) {
      continue;
    }
    if ((array_sreg == check_array) && (index_sreg == check_index)) {
      if (cu->verbose) {
        LOG(INFO) << "Squashing range check @ 0x" << std::hex << mir->offset;
      }
      mir->optimization_flags |= MIR_IGNORE_RANGE_CHECK;
    }
  }
}

/* Do some MIR-level basic block optimizations */
static bool BasicBlockOpt(CompilationUnit* cu, BasicBlock* bb)
{
  int num_temps = 0;

  for (MIR* mir = bb->first_mir_insn; mir != NULL; mir = mir->next) {
    // Look for interesting opcodes, skip otherwise
    Instruction::Code opcode = mir->dalvikInsn.opcode;
    switch (opcode) {
      case Instruction::AGET:
      case Instruction::AGET_OBJECT:
      case Instruction::AGET_BOOLEAN:
      case Instruction::AGET_BYTE:
      case Instruction::AGET_CHAR:
      case Instruction::AGET_SHORT:
      case Instruction::AGET_WIDE:
        if (!(mir->optimization_flags & MIR_IGNORE_RANGE_CHECK)) {
          int arr_sreg = mir->ssa_rep->uses[0];
          int idx_sreg = mir->ssa_rep->uses[1];
          BasicBlock* tbb = bb;
          SquashDupRangeChecks(cu, &tbb, mir, arr_sreg, idx_sreg);
        }
        break;
      case Instruction::APUT:
      case Instruction::APUT_OBJECT:
      case Instruction::APUT_SHORT:
      case Instruction::APUT_CHAR:
      case Instruction::APUT_BYTE:
      case Instruction::APUT_BOOLEAN:
      case Instruction::APUT_WIDE:
        if (!(mir->optimization_flags & MIR_IGNORE_RANGE_CHECK)) {
          int start = (opcode == Instruction::APUT_WIDE) ? 2 : 1;
          int arr_sreg = mir->ssa_rep->uses[start];
          int idx_sreg = mir->ssa_rep->uses[start + 1];
          BasicBlock* tbb = bb;
          SquashDupRangeChecks(cu, &tbb, mir, arr_sreg, idx_sreg);
        }
        break;
      case Instruction::CMPL_FLOAT:
      case Instruction::CMPL_DOUBLE:
      case Instruction::CMPG_FLOAT:
      case Instruction::CMPG_DOUBLE:
      case Instruction::CMP_LONG:
        if (cu->gen_bitcode) {
          // Bitcode doesn't allow this optimization.
          break;
        }
        if (mir->next != NULL) {
          MIR* mir_next = mir->next;
          Instruction::Code br_opcode = mir_next->dalvikInsn.opcode;
          ConditionCode ccode = kCondNv;
          switch(br_opcode) {
            case Instruction::IF_EQZ:
              ccode = kCondEq;
              break;
            case Instruction::IF_NEZ:
              ccode = kCondNe;
              break;
            case Instruction::IF_LTZ:
              ccode = kCondLt;
              break;
            case Instruction::IF_GEZ:
              ccode = kCondGe;
              break;
            case Instruction::IF_GTZ:
              ccode = kCondGt;
              break;
            case Instruction::IF_LEZ:
              ccode = kCondLe;
              break;
            default:
              break;
          }
          // Make sure result of cmp is used by next insn and nowhere else
          if ((ccode != kCondNv) &&
              (mir->ssa_rep->defs[0] == mir_next->ssa_rep->uses[0]) &&
              (GetSSAUseCount(cu, mir->ssa_rep->defs[0]) == 1)) {
            mir_next->dalvikInsn.arg[0] = ccode;
            switch(opcode) {
              case Instruction::CMPL_FLOAT:
                mir_next->dalvikInsn.opcode =
                    static_cast<Instruction::Code>(kMirOpFusedCmplFloat);
                break;
              case Instruction::CMPL_DOUBLE:
                mir_next->dalvikInsn.opcode =
                    static_cast<Instruction::Code>(kMirOpFusedCmplDouble);
                break;
              case Instruction::CMPG_FLOAT:
                mir_next->dalvikInsn.opcode =
                    static_cast<Instruction::Code>(kMirOpFusedCmpgFloat);
                break;
              case Instruction::CMPG_DOUBLE:
                mir_next->dalvikInsn.opcode =
                    static_cast<Instruction::Code>(kMirOpFusedCmpgDouble);
                break;
              case Instruction::CMP_LONG:
                mir_next->dalvikInsn.opcode =
                    static_cast<Instruction::Code>(kMirOpFusedCmpLong);
                break;
              default: LOG(ERROR) << "Unexpected opcode: " << opcode;
            }
            mir->dalvikInsn.opcode = static_cast<Instruction::Code>(kMirOpNop);
            mir_next->ssa_rep->num_uses = mir->ssa_rep->num_uses;
            mir_next->ssa_rep->uses = mir->ssa_rep->uses;
            mir_next->ssa_rep->fp_use = mir->ssa_rep->fp_use;
            mir_next->ssa_rep->num_defs = 0;
            mir->ssa_rep->num_uses = 0;
            mir->ssa_rep->num_defs = 0;
          }
        }
        break;
      default:
        break;
    }
  }

  if (num_temps > cu->num_compiler_temps) {
    cu->num_compiler_temps = num_temps;
  }
  return true;
}

static bool NullCheckEliminationInit(struct CompilationUnit* cu, struct BasicBlock* bb)
{
  if (bb->data_flow_info == NULL) return false;
  bb->data_flow_info->ending_null_check_v =
      AllocBitVector(cu, cu->num_ssa_regs, false, kBitMapNullCheck);
  ClearAllBits(bb->data_flow_info->ending_null_check_v);
  return true;
}

/* Collect stats on number of checks removed */
static bool CountChecks( struct CompilationUnit* cu, struct BasicBlock* bb)
{
  if (bb->data_flow_info == NULL) return false;
  for (MIR* mir = bb->first_mir_insn; mir != NULL; mir = mir->next) {
    if (mir->ssa_rep == NULL) {
      continue;
    }
    int df_attributes = oat_data_flow_attributes[mir->dalvikInsn.opcode];
    if (df_attributes & DF_HAS_NULL_CHKS) {
      cu->checkstats->null_checks++;
      if (mir->optimization_flags & MIR_IGNORE_NULL_CHECK) {
        cu->checkstats->null_checks_eliminated++;
      }
    }
    if (df_attributes & DF_HAS_RANGE_CHKS) {
      cu->checkstats->range_checks++;
      if (mir->optimization_flags & MIR_IGNORE_RANGE_CHECK) {
        cu->checkstats->range_checks_eliminated++;
      }
    }
  }
  return false;
}

/* Try to make common case the fallthrough path */
static bool LayoutBlocks(struct CompilationUnit* cu, struct BasicBlock* bb)
{
  // TODO: For now, just looking for direct throws.  Consider generalizing for profile feedback
  if (!bb->explicit_throw) {
    return false;
  }
  BasicBlock* walker = bb;
  while (true) {
    // Check termination conditions
    if ((walker->block_type == kEntryBlock) || (walker->predecessors->num_used != 1)) {
      break;
    }
    BasicBlock* prev = GET_ELEM_N(walker->predecessors, BasicBlock*, 0);
    if (prev->conditional_branch) {
      if (prev->fall_through == walker) {
        // Already done - return
        break;
      }
      DCHECK_EQ(walker, prev->taken);
      // Got one.  Flip it and exit
      Instruction::Code opcode = prev->last_mir_insn->dalvikInsn.opcode;
      switch (opcode) {
        case Instruction::IF_EQ: opcode = Instruction::IF_NE; break;
        case Instruction::IF_NE: opcode = Instruction::IF_EQ; break;
        case Instruction::IF_LT: opcode = Instruction::IF_GE; break;
        case Instruction::IF_GE: opcode = Instruction::IF_LT; break;
        case Instruction::IF_GT: opcode = Instruction::IF_LE; break;
        case Instruction::IF_LE: opcode = Instruction::IF_GT; break;
        case Instruction::IF_EQZ: opcode = Instruction::IF_NEZ; break;
        case Instruction::IF_NEZ: opcode = Instruction::IF_EQZ; break;
        case Instruction::IF_LTZ: opcode = Instruction::IF_GEZ; break;
        case Instruction::IF_GEZ: opcode = Instruction::IF_LTZ; break;
        case Instruction::IF_GTZ: opcode = Instruction::IF_LEZ; break;
        case Instruction::IF_LEZ: opcode = Instruction::IF_GTZ; break;
        default: LOG(FATAL) << "Unexpected opcode " << opcode;
      }
      prev->last_mir_insn->dalvikInsn.opcode = opcode;
      BasicBlock* t_bb = prev->taken;
      prev->taken = prev->fall_through;
      prev->fall_through = t_bb;
      break;
    }
    walker = prev;
  }
  return false;
}

/* Combine any basic blocks terminated by instructions that we now know can't throw */
static bool CombineBlocks(struct CompilationUnit* cu, struct BasicBlock* bb)
{
  // Loop here to allow combining a sequence of blocks
  while (true) {
    // Check termination conditions
    if ((bb->first_mir_insn == NULL)
        || (bb->data_flow_info == NULL)
        || (bb->block_type == kExceptionHandling)
        || (bb->block_type == kExitBlock)
        || (bb->block_type == kDead)
        || ((bb->taken == NULL) || (bb->taken->block_type != kExceptionHandling))
        || (bb->successor_block_list.block_list_type != kNotUsed)
        || (static_cast<int>(bb->last_mir_insn->dalvikInsn.opcode) != kMirOpCheck)) {
      break;
    }

    // Test the kMirOpCheck instruction
    MIR* mir = bb->last_mir_insn;
    // Grab the attributes from the paired opcode
    MIR* throw_insn = mir->meta.throw_insn;
    int df_attributes = oat_data_flow_attributes[throw_insn->dalvikInsn.opcode];
    bool can_combine = true;
    if (df_attributes & DF_HAS_NULL_CHKS) {
      can_combine &= ((throw_insn->optimization_flags & MIR_IGNORE_NULL_CHECK) != 0);
    }
    if (df_attributes & DF_HAS_RANGE_CHKS) {
      can_combine &= ((throw_insn->optimization_flags & MIR_IGNORE_RANGE_CHECK) != 0);
    }
    if (!can_combine) {
      break;
    }
    // OK - got one.  Combine
    BasicBlock* bb_next = bb->fall_through;
    DCHECK(!bb_next->catch_entry);
    DCHECK_EQ(bb_next->predecessors->num_used, 1U);
    MIR* t_mir = bb->last_mir_insn->prev;
    // Overwrite the kOpCheck insn with the paired opcode
    DCHECK_EQ(bb_next->first_mir_insn, throw_insn);
    *bb->last_mir_insn = *throw_insn;
    bb->last_mir_insn->prev = t_mir;
    // Use the successor info from the next block
    bb->successor_block_list = bb_next->successor_block_list;
    // Use the ending block linkage from the next block
    bb->fall_through = bb_next->fall_through;
    bb->taken->block_type = kDead;  // Kill the unused exception block
    bb->taken = bb_next->taken;
    // Include the rest of the instructions
    bb->last_mir_insn = bb_next->last_mir_insn;

    /*
     * NOTE: we aren't updating all dataflow info here.  Should either make sure this pass
     * happens after uses of i_dominated, dom_frontier or update the dataflow info here.
     */

    // Kill bb_next and remap now-dead id to parent
    bb_next->block_type = kDead;
    cu->block_id_map.Overwrite(bb_next->id, bb->id);

    // Now, loop back and see if we can keep going
  }
  return false;
}

/* Eliminate unnecessary null checks for a basic block. */
static bool EliminateNullChecks( struct CompilationUnit* cu, struct BasicBlock* bb)
{
  if (bb->data_flow_info == NULL) return false;

  /*
   * Set initial state.  Be conservative with catch
   * blocks and start with no assumptions about null check
   * status (except for "this").
   */
  if ((bb->block_type == kEntryBlock) | bb->catch_entry) {
    ClearAllBits(cu->temp_ssa_register_v);
    if ((cu->access_flags & kAccStatic) == 0) {
      // If non-static method, mark "this" as non-null
      int this_reg = cu->num_dalvik_registers - cu->num_ins;
      SetBit(cu, cu->temp_ssa_register_v, this_reg);
    }
  } else {
    // Starting state is intesection of all incoming arcs
    GrowableListIterator iter;
    GrowableListIteratorInit(bb->predecessors, &iter);
    BasicBlock* pred_bb = reinterpret_cast<BasicBlock*>(GrowableListIteratorNext(&iter));
    DCHECK(pred_bb != NULL);
    CopyBitVector(cu->temp_ssa_register_v,
                     pred_bb->data_flow_info->ending_null_check_v);
    while (true) {
      pred_bb = reinterpret_cast<BasicBlock*>(GrowableListIteratorNext(&iter));
      if (!pred_bb) break;
      if ((pred_bb->data_flow_info == NULL) ||
          (pred_bb->data_flow_info->ending_null_check_v == NULL)) {
        continue;
      }
      IntersectBitVectors(cu->temp_ssa_register_v,
                             cu->temp_ssa_register_v,
                             pred_bb->data_flow_info->ending_null_check_v);
    }
  }

  // Walk through the instruction in the block, updating as necessary
  for (MIR* mir = bb->first_mir_insn; mir != NULL; mir = mir->next) {
    if (mir->ssa_rep == NULL) {
        continue;
    }
    int df_attributes = oat_data_flow_attributes[mir->dalvikInsn.opcode];

    // Mark target of NEW* as non-null
    if (df_attributes & DF_NON_NULL_DST) {
      SetBit(cu, cu->temp_ssa_register_v, mir->ssa_rep->defs[0]);
    }

    // Mark non-null returns from invoke-style NEW*
    if (df_attributes & DF_NON_NULL_RET) {
      MIR* next_mir = mir->next;
      // Next should be an MOVE_RESULT_OBJECT
      if (next_mir &&
          next_mir->dalvikInsn.opcode == Instruction::MOVE_RESULT_OBJECT) {
        // Mark as null checked
        SetBit(cu, cu->temp_ssa_register_v, next_mir->ssa_rep->defs[0]);
      } else {
        if (next_mir) {
          LOG(WARNING) << "Unexpected opcode following new: " << next_mir->dalvikInsn.opcode;
        } else if (bb->fall_through) {
          // Look in next basic block
          struct BasicBlock* next_bb = bb->fall_through;
          for (MIR* tmir = next_bb->first_mir_insn; tmir != NULL;
            tmir =tmir->next) {
            if (static_cast<int>(tmir->dalvikInsn.opcode) >= static_cast<int>(kMirOpFirst)) {
              continue;
            }
            // First non-pseudo should be MOVE_RESULT_OBJECT
            if (tmir->dalvikInsn.opcode == Instruction::MOVE_RESULT_OBJECT) {
              // Mark as null checked
              SetBit(cu, cu->temp_ssa_register_v, tmir->ssa_rep->defs[0]);
            } else {
              LOG(WARNING) << "Unexpected op after new: " << tmir->dalvikInsn.opcode;
            }
            break;
          }
        }
      }
    }

    /*
     * Propagate nullcheck state on register copies (including
     * Phi pseudo copies.  For the latter, nullcheck state is
     * the "and" of all the Phi's operands.
     */
    if (df_attributes & (DF_NULL_TRANSFER_0 | DF_NULL_TRANSFER_N)) {
      int tgt_sreg = mir->ssa_rep->defs[0];
      int operands = (df_attributes & DF_NULL_TRANSFER_0) ? 1 :
          mir->ssa_rep->num_uses;
      bool null_checked = true;
      for (int i = 0; i < operands; i++) {
        null_checked &= IsBitSet(cu->temp_ssa_register_v,
        mir->ssa_rep->uses[i]);
      }
      if (null_checked) {
        SetBit(cu, cu->temp_ssa_register_v, tgt_sreg);
      }
    }

    // Already nullchecked?
    if ((df_attributes & DF_HAS_NULL_CHKS) && !(mir->optimization_flags & MIR_IGNORE_NULL_CHECK)) {
      int src_idx;
      if (df_attributes & DF_NULL_CHK_1) {
        src_idx = 1;
      } else if (df_attributes & DF_NULL_CHK_2) {
        src_idx = 2;
      } else {
        src_idx = 0;
      }
      int src_sreg = mir->ssa_rep->uses[src_idx];
        if (IsBitSet(cu->temp_ssa_register_v, src_sreg)) {
          // Eliminate the null check
          mir->optimization_flags |= MIR_IGNORE_NULL_CHECK;
        } else {
          // Mark s_reg as null-checked
          SetBit(cu, cu->temp_ssa_register_v, src_sreg);
        }
     }
  }

  // Did anything change?
  bool res = CompareBitVectors(bb->data_flow_info->ending_null_check_v,
                                  cu->temp_ssa_register_v);
  if (res) {
    CopyBitVector(bb->data_flow_info->ending_null_check_v,
                     cu->temp_ssa_register_v);
  }
  return res;
}

void NullCheckElimination(CompilationUnit *cu)
{
  if (!(cu->disable_opt & (1 << kNullCheckElimination))) {
    DCHECK(cu->temp_ssa_register_v != NULL);
    DataFlowAnalysisDispatcher(cu, NullCheckEliminationInit, kAllNodes,
                                  false /* is_iterative */);
    DataFlowAnalysisDispatcher(cu, EliminateNullChecks,
                                  kPreOrderDFSTraversal,
                                  true /* is_iterative */);
  }
}

void BasicBlockCombine(CompilationUnit* cu)
{
  DataFlowAnalysisDispatcher(cu, CombineBlocks, kPreOrderDFSTraversal, false);
}

void CodeLayout(CompilationUnit* cu)
{
  DataFlowAnalysisDispatcher(cu, LayoutBlocks, kAllNodes, false);
}

void DumpCheckStats(CompilationUnit *cu)
{
  Checkstats* stats =
      static_cast<Checkstats*>(NewMem(cu, sizeof(Checkstats), true, kAllocDFInfo));
  cu->checkstats = stats;
  DataFlowAnalysisDispatcher(cu, CountChecks, kAllNodes, false /* is_iterative */);
  if (stats->null_checks > 0) {
    float eliminated = static_cast<float>(stats->null_checks_eliminated);
    float checks = static_cast<float>(stats->null_checks);
    LOG(INFO) << "Null Checks: " << PrettyMethod(cu->method_idx, *cu->dex_file) << " "
              << stats->null_checks_eliminated << " of " << stats->null_checks << " -> "
              << (eliminated/checks) * 100.0 << "%";
    }
  if (stats->range_checks > 0) {
    float eliminated = static_cast<float>(stats->range_checks_eliminated);
    float checks = static_cast<float>(stats->range_checks);
    LOG(INFO) << "Range Checks: " << PrettyMethod(cu->method_idx, *cu->dex_file) << " "
              << stats->range_checks_eliminated << " of " << stats->range_checks << " -> "
              << (eliminated/checks) * 100.0 << "%";
  }
}

void BasicBlockOptimization(CompilationUnit *cu)
{
  if (!(cu->disable_opt & (1 << kBBOpt))) {
    CompilerInitGrowableList(cu, &cu->compiler_temps, 6, kListMisc);
    DCHECK_EQ(cu->num_compiler_temps, 0);
    DataFlowAnalysisDispatcher(cu, BasicBlockOpt,
                                  kAllNodes, false /* is_iterative */);
  }
}

static void AddLoopHeader(CompilationUnit* cu, BasicBlock* header,
                          BasicBlock* back_edge)
{
  GrowableListIterator iter;
  GrowableListIteratorInit(&cu->loop_headers, &iter);
  for (LoopInfo* loop = reinterpret_cast<LoopInfo*>(GrowableListIteratorNext(&iter));
      (loop != NULL); loop = reinterpret_cast<LoopInfo*>(GrowableListIteratorNext(&iter))) {
    if (loop->header == header) {
      InsertGrowableList(cu, &loop->incoming_back_edges,
                            reinterpret_cast<uintptr_t>(back_edge));
      return;
    }
  }
  LoopInfo* info = static_cast<LoopInfo*>(NewMem(cu, sizeof(LoopInfo), true, kAllocDFInfo));
  info->header = header;
  CompilerInitGrowableList(cu, &info->incoming_back_edges, 2, kListMisc);
  InsertGrowableList(cu, &info->incoming_back_edges, reinterpret_cast<uintptr_t>(back_edge));
  InsertGrowableList(cu, &cu->loop_headers, reinterpret_cast<uintptr_t>(info));
}

static bool FindBackEdges(struct CompilationUnit* cu, struct BasicBlock* bb)
{
  if ((bb->data_flow_info == NULL) || (bb->last_mir_insn == NULL)) {
    return false;
  }
  Instruction::Code opcode = bb->last_mir_insn->dalvikInsn.opcode;
  if (Instruction::FlagsOf(opcode) & Instruction::kBranch) {
    if (bb->taken && (bb->taken->start_offset <= bb->start_offset)) {
      DCHECK(bb->dominators != NULL);
      if (IsBitSet(bb->dominators, bb->taken->id)) {
        if (cu->verbose) {
          LOG(INFO) << "Loop backedge from 0x"
                    << std::hex << bb->last_mir_insn->offset
                    << " to 0x" << std::hex << bb->taken->start_offset;
        }
        AddLoopHeader(cu, bb->taken, bb);
      }
    }
  }
  return false;
}

static void AddBlocksToLoop(CompilationUnit* cu, ArenaBitVector* blocks,
                            BasicBlock* bb, int head_id)
{
  if (!IsBitSet(bb->dominators, head_id) ||
    IsBitSet(blocks, bb->id)) {
    return;
  }
  SetBit(cu, blocks, bb->id);
  GrowableListIterator iter;
  GrowableListIteratorInit(bb->predecessors, &iter);
  BasicBlock* pred_bb;
  for (pred_bb = reinterpret_cast<BasicBlock*>(GrowableListIteratorNext(&iter)); pred_bb != NULL;
       pred_bb = reinterpret_cast<BasicBlock*>(GrowableListIteratorNext(&iter))) {
    AddBlocksToLoop(cu, blocks, pred_bb, head_id);
  }
}

static void DumpLoops(CompilationUnit *cu)
{
  GrowableListIterator iter;
  GrowableListIteratorInit(&cu->loop_headers, &iter);
  for (LoopInfo* loop = reinterpret_cast<LoopInfo*>(GrowableListIteratorNext(&iter));
      (loop != NULL); loop = reinterpret_cast<LoopInfo*>(GrowableListIteratorNext(&iter))) {
    LOG(INFO) << "Loop head block id " << loop->header->id
              << ", offset 0x" << std::hex << loop->header->start_offset
              << ", Depth: " << loop->header->nesting_depth;
    GrowableListIterator iter;
    GrowableListIteratorInit(&loop->incoming_back_edges, &iter);
    BasicBlock* edge_bb;
    for (edge_bb = reinterpret_cast<BasicBlock*>(GrowableListIteratorNext(&iter)); edge_bb != NULL;
         edge_bb = reinterpret_cast<BasicBlock*>(GrowableListIteratorNext(&iter))) {
      LOG(INFO) << "    Backedge block id " << edge_bb->id
                << ", offset 0x" << std::hex << edge_bb->start_offset;
      ArenaBitVectorIterator b_iter;
      BitVectorIteratorInit(loop->blocks, &b_iter);
      for (int bb_id = BitVectorIteratorNext(&b_iter); bb_id != -1;
           bb_id = BitVectorIteratorNext(&b_iter)) {
        BasicBlock *bb;
        bb = reinterpret_cast<BasicBlock*>(GrowableListGetElement(&cu->block_list, bb_id));
        LOG(INFO) << "        (" << bb->id << ", 0x" << std::hex
                  << bb->start_offset << ")";
      }
    }
  }
}

void LoopDetection(CompilationUnit *cu)
{
  if (cu->disable_opt & (1 << kPromoteRegs)) {
    return;
  }
  CompilerInitGrowableList(cu, &cu->loop_headers, 6, kListMisc);
  // Find the loop headers
  DataFlowAnalysisDispatcher(cu, FindBackEdges, kAllNodes, false /* is_iterative */);
  GrowableListIterator iter;
  GrowableListIteratorInit(&cu->loop_headers, &iter);
  // Add blocks to each header
  for (LoopInfo* loop = reinterpret_cast<LoopInfo*>(GrowableListIteratorNext(&iter));
       loop != NULL; loop = reinterpret_cast<LoopInfo*>(GrowableListIteratorNext(&iter))) {
    loop->blocks = AllocBitVector(cu, cu->num_blocks, true,
                                     kBitMapMisc);
    SetBit(cu, loop->blocks, loop->header->id);
    GrowableListIterator iter;
    GrowableListIteratorInit(&loop->incoming_back_edges, &iter);
    BasicBlock* edge_bb;
    for (edge_bb = reinterpret_cast<BasicBlock*>(GrowableListIteratorNext(&iter)); edge_bb != NULL;
         edge_bb = reinterpret_cast<BasicBlock*>(GrowableListIteratorNext(&iter))) {
      AddBlocksToLoop(cu, loop->blocks, edge_bb, loop->header->id);
    }
  }
  // Compute the nesting depth of each header
  GrowableListIteratorInit(&cu->loop_headers, &iter);
  for (LoopInfo* loop = reinterpret_cast<LoopInfo*>(GrowableListIteratorNext(&iter));
       loop != NULL; loop = reinterpret_cast<LoopInfo*>(GrowableListIteratorNext(&iter))) {
    GrowableListIterator iter2;
    GrowableListIteratorInit(&cu->loop_headers, &iter2);
    LoopInfo* loop2;
    for (loop2 = reinterpret_cast<LoopInfo*>(GrowableListIteratorNext(&iter2));
         loop2 != NULL; loop2 = reinterpret_cast<LoopInfo*>(GrowableListIteratorNext(&iter2))) {
      if (IsBitSet(loop2->blocks, loop->header->id)) {
         loop->header->nesting_depth++;
      }
    }
  }
  // Assign nesting depth to each block in all loops
  GrowableListIteratorInit(&cu->loop_headers, &iter);
  for (LoopInfo* loop = reinterpret_cast<LoopInfo*>(GrowableListIteratorNext(&iter));
       (loop != NULL); loop = reinterpret_cast<LoopInfo*>(GrowableListIteratorNext(&iter))) {
    ArenaBitVectorIterator b_iter;
    BitVectorIteratorInit(loop->blocks, &b_iter);
    for (int bb_id = BitVectorIteratorNext(&b_iter); bb_id != -1;
        bb_id = BitVectorIteratorNext(&b_iter)) {
      BasicBlock *bb;
      bb = reinterpret_cast<BasicBlock*>(GrowableListGetElement(&cu->block_list, bb_id));
      bb->nesting_depth = std::max(bb->nesting_depth,
                                  loop->header->nesting_depth);
    }
  }
  if (cu->verbose) {
    DumpLoops(cu);
  }
}

/*
 * This function will make a best guess at whether the invoke will
 * end up using Method*.  It isn't critical to get it exactly right,
 * and attempting to do would involve more complexity than it's
 * worth.
 */
static bool InvokeUsesMethodStar(CompilationUnit* cu, MIR* mir)
{
  InvokeType type;
  Instruction::Code opcode = mir->dalvikInsn.opcode;
  switch (opcode) {
    case Instruction::INVOKE_STATIC:
    case Instruction::INVOKE_STATIC_RANGE:
      type = kStatic;
      break;
    case Instruction::INVOKE_DIRECT:
    case Instruction::INVOKE_DIRECT_RANGE:
      type = kDirect;
      break;
    case Instruction::INVOKE_VIRTUAL:
    case Instruction::INVOKE_VIRTUAL_RANGE:
      type = kVirtual;
      break;
    case Instruction::INVOKE_INTERFACE:
    case Instruction::INVOKE_INTERFACE_RANGE:
      return false;
    case Instruction::INVOKE_SUPER_RANGE:
    case Instruction::INVOKE_SUPER:
      type = kSuper;
      break;
    default:
      LOG(WARNING) << "Unexpected invoke op: " << opcode;
      return false;
  }
  OatCompilationUnit m_unit(cu->class_loader, cu->class_linker,
                           *cu->dex_file,
                           cu->code_item, cu->method_idx,
                           cu->access_flags);
  // TODO: add a flag so we don't counts the stats for this twice
  uint32_t dex_method_idx = mir->dalvikInsn.vB;
  int vtable_idx;
  uintptr_t direct_code;
  uintptr_t direct_method;
  bool fast_path =
      cu->compiler->ComputeInvokeInfo(dex_method_idx, &m_unit, type,
                                         vtable_idx, direct_code,
                                         direct_method) &&
      !SLOW_INVOKE_PATH;
  return (((type == kDirect) || (type == kStatic)) &&
          fast_path && ((direct_code == 0) || (direct_method == 0)));
}

/*
 * Count uses, weighting by loop nesting depth.  This code only
 * counts explicitly used s_regs.  A later phase will add implicit
 * counts for things such as Method*, null-checked references, etc.
 */
static bool CountUses(struct CompilationUnit* cu, struct BasicBlock* bb)
{
  if (bb->block_type != kDalvikByteCode) {
    return false;
  }
  for (MIR* mir = bb->first_mir_insn; (mir != NULL); mir = mir->next) {
    if (mir->ssa_rep == NULL) {
      continue;
    }
    uint32_t weight = std::min(16U, static_cast<uint32_t>(bb->nesting_depth));
    for (int i = 0; i < mir->ssa_rep->num_uses; i++) {
      int s_reg = mir->ssa_rep->uses[i];
      DCHECK_LT(s_reg, static_cast<int>(cu->use_counts.num_used));
      cu->raw_use_counts.elem_list[s_reg]++;
      cu->use_counts.elem_list[s_reg] += (1 << weight);
    }
    if (!(cu->disable_opt & (1 << kPromoteCompilerTemps))) {
      int df_attributes = oat_data_flow_attributes[mir->dalvikInsn.opcode];
      // Implicit use of Method* ? */
      if (df_attributes & DF_UMS) {
        /*
         * Some invokes will not use Method* - need to perform test similar
         * to that found in GenInvoke() to decide whether to count refs
         * for Method* on invoke-class opcodes.
         * TODO: refactor for common test here, save results for GenInvoke
         */
        int uses_method_star = true;
        if ((df_attributes & (DF_FORMAT_35C | DF_FORMAT_3RC)) &&
            !(df_attributes & DF_NON_NULL_RET)) {
          uses_method_star &= InvokeUsesMethodStar(cu, mir);
        }
        if (uses_method_star) {
          cu->raw_use_counts.elem_list[cu->method_sreg]++;
          cu->use_counts.elem_list[cu->method_sreg] += (1 << weight);
        }
      }
    }
  }
  return false;
}

void MethodUseCount(CompilationUnit *cu)
{
  CompilerInitGrowableList(cu, &cu->use_counts, cu->num_ssa_regs + 32, kListMisc);
  CompilerInitGrowableList(cu, &cu->raw_use_counts, cu->num_ssa_regs + 32, kListMisc);
  // Initialize list
  for (int i = 0; i < cu->num_ssa_regs; i++) {
    InsertGrowableList(cu, &cu->use_counts, 0);
    InsertGrowableList(cu, &cu->raw_use_counts, 0);
  }
  if (cu->disable_opt & (1 << kPromoteRegs)) {
    return;
  }
  DataFlowAnalysisDispatcher(cu, CountUses,
                                kAllNodes, false /* is_iterative */);
}

}  // namespace art
