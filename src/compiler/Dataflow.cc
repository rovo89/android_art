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

#include "Dalvik.h"
#include "Dataflow.h"

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
const int oatDataFlowAttributes[kMirOpLast] = {
    // 00 NOP
    DF_NOP,

    // 01 MOVE vA, vB
    DF_DA | DF_UB | DF_IS_MOVE,

    // 02 MOVE_FROM16 vAA, vBBBB
    DF_DA | DF_UB | DF_IS_MOVE,

    // 03 MOVE_16 vAAAA, vBBBB
    DF_DA | DF_UB | DF_IS_MOVE,

    // 04 MOVE_WIDE vA, vB
    DF_DA_WIDE | DF_UB_WIDE | DF_IS_MOVE,

    // 05 MOVE_WIDE_FROM16 vAA, vBBBB
    DF_DA_WIDE | DF_UB_WIDE | DF_IS_MOVE,

    // 06 MOVE_WIDE_16 vAAAA, vBBBB
    DF_DA_WIDE | DF_UB_WIDE | DF_IS_MOVE,

    // 07 MOVE_OBJECT vA, vB
    DF_DA | DF_UB | DF_NULL_TRANSFER_0 | DF_IS_MOVE | DF_CORE_A | DF_CORE_B,

    // 08 MOVE_OBJECT_FROM16 vAA, vBBBB
    DF_DA | DF_UB | DF_NULL_TRANSFER_0 | DF_IS_MOVE | DF_CORE_A | DF_CORE_B,

    // 09 MOVE_OBJECT_16 vAAAA, vBBBB
    DF_DA | DF_UB | DF_NULL_TRANSFER_0 | DF_IS_MOVE | DF_CORE_A | DF_CORE_B,

    // 0A MOVE_RESULT vAA
    DF_DA,

    // 0B MOVE_RESULT_WIDE vAA
    DF_DA_WIDE,

    // 0C MOVE_RESULT_OBJECT vAA
    DF_DA | DF_CORE_A,

    // 0D MOVE_EXCEPTION vAA
    DF_DA | DF_CORE_A,

    // 0E RETURN_VOID
    DF_NOP,

    // 0F RETURN vAA
    DF_UA,

    // 10 RETURN_WIDE vAA
    DF_UA_WIDE,

    // 11 RETURN_OBJECT vAA
    DF_UA | DF_CORE_A,

    // 12 CONST_4 vA, #+B
    DF_DA | DF_SETS_CONST,

    // 13 CONST_16 vAA, #+BBBB
    DF_DA | DF_SETS_CONST,

    // 14 CONST vAA, #+BBBBBBBB
    DF_DA | DF_SETS_CONST,

    // 15 CONST_HIGH16 VAA, #+BBBB0000
    DF_DA | DF_SETS_CONST,

    // 16 CONST_WIDE_16 vAA, #+BBBB
    DF_DA_WIDE | DF_SETS_CONST,

    // 17 CONST_WIDE_32 vAA, #+BBBBBBBB
    DF_DA_WIDE | DF_SETS_CONST,

    // 18 CONST_WIDE vAA, #+BBBBBBBBBBBBBBBB
    DF_DA_WIDE | DF_SETS_CONST,

    // 19 CONST_WIDE_HIGH16 vAA, #+BBBB000000000000
    DF_DA_WIDE | DF_SETS_CONST,

    // 1A CONST_STRING vAA, string@BBBB
    DF_DA | DF_CORE_A,

    // 1B CONST_STRING_JUMBO vAA, string@BBBBBBBB
    DF_DA | DF_CORE_A,

    // 1C CONST_CLASS vAA, type@BBBB
    DF_DA | DF_CORE_A,

    // 1D MONITOR_ENTER vAA
    DF_UA | DF_NULL_CHK_0 | DF_CORE_A,

    // 1E MONITOR_EXIT vAA
    DF_UA | DF_NULL_CHK_0 | DF_CORE_A,

    // 1F CHK_CAST vAA, type@BBBB
    DF_UA | DF_CORE_A | DF_UMS,

    // 20 INSTANCE_OF vA, vB, type@CCCC
    DF_DA | DF_UB | DF_CORE_A | DF_CORE_B | DF_UMS,

    // 21 ARRAY_LENGTH vA, vB
    DF_DA | DF_UB | DF_NULL_CHK_0 | DF_CORE_A | DF_CORE_B,

    // 22 NEW_INSTANCE vAA, type@BBBB
    DF_DA | DF_NON_NULL_DST | DF_CORE_A | DF_UMS,

    // 23 NEW_ARRAY vA, vB, type@CCCC
    DF_DA | DF_UB | DF_NON_NULL_DST | DF_CORE_A | DF_CORE_B | DF_UMS,

    // 24 FILLED_NEW_ARRAY {vD, vE, vF, vG, vA}
    DF_FORMAT_35C | DF_NON_NULL_RET | DF_UMS,

    // 25 FILLED_NEW_ARRAY_RANGE {vCCCC .. vNNNN}, type@BBBB
    DF_FORMAT_3RC | DF_NON_NULL_RET | DF_UMS,

    // 26 FILL_ARRAY_DATA vAA, +BBBBBBBB
    DF_UA | DF_CORE_A | DF_UMS,

    // 27 THROW vAA
    DF_UA | DF_CORE_A | DF_UMS,

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
    DF_DA | DF_UB_WIDE | DF_UC_WIDE | DF_FP_B | DF_FP_C | DF_CORE_A,

    // 30 CMPG_DOUBLE vAA, vBB, vCC
    DF_DA | DF_UB_WIDE | DF_UC_WIDE | DF_FP_B | DF_FP_C | DF_CORE_A,

    // 31 CMP_LONG vAA, vBB, vCC
    DF_DA | DF_UB_WIDE | DF_UC_WIDE | DF_CORE_A | DF_CORE_B | DF_CORE_C,

    // 32 IF_EQ vA, vB, +CCCC
    DF_UA | DF_UB | DF_CORE_A | DF_CORE_B,

    // 33 IF_NE vA, vB, +CCCC
    DF_UA | DF_UB | DF_CORE_A | DF_CORE_B,

    // 34 IF_LT vA, vB, +CCCC
    DF_UA | DF_UB | DF_CORE_A | DF_CORE_B,

    // 35 IF_GE vA, vB, +CCCC
    DF_UA | DF_UB | DF_CORE_A | DF_CORE_B,

    // 36 IF_GT vA, vB, +CCCC
    DF_UA | DF_UB | DF_CORE_A | DF_CORE_B,

    // 37 IF_LE vA, vB, +CCCC
    DF_UA | DF_UB | DF_CORE_A | DF_CORE_B,


    // 38 IF_EQZ vAA, +BBBB
    DF_UA | DF_CORE_A,

    // 39 IF_NEZ vAA, +BBBB
    DF_UA | DF_CORE_A,

    // 3A IF_LTZ vAA, +BBBB
    DF_UA | DF_CORE_A,

    // 3B IF_GEZ vAA, +BBBB
    DF_UA | DF_CORE_A,

    // 3C IF_GTZ vAA, +BBBB
    DF_UA | DF_CORE_A,

    // 3D IF_LEZ vAA, +BBBB
    DF_UA | DF_CORE_A,

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
    DF_DA | DF_UB | DF_UC | DF_NULL_CHK_0 | DF_RANGE_CHK_1 | DF_CORE_B | DF_CORE_C,

    // 45 AGET_WIDE vAA, vBB, vCC
    DF_DA_WIDE | DF_UB | DF_UC | DF_NULL_CHK_0 | DF_RANGE_CHK_1 | DF_CORE_B | DF_CORE_C,

    // 46 AGET_OBJECT vAA, vBB, vCC
    DF_DA | DF_UB | DF_UC | DF_NULL_CHK_0 | DF_RANGE_CHK_1 | DF_CORE_B | DF_CORE_C,

    // 47 AGET_BOOLEAN vAA, vBB, vCC
    DF_DA | DF_UB | DF_UC | DF_NULL_CHK_0 | DF_RANGE_CHK_1 | DF_CORE_B | DF_CORE_C,

    // 48 AGET_BYTE vAA, vBB, vCC
    DF_DA | DF_UB | DF_UC | DF_NULL_CHK_0 | DF_RANGE_CHK_1 | DF_CORE_B | DF_CORE_C,

    // 49 AGET_CHAR vAA, vBB, vCC
    DF_DA | DF_UB | DF_UC | DF_NULL_CHK_0 | DF_RANGE_CHK_1 | DF_CORE_B | DF_CORE_C,

    // 4A AGET_SHORT vAA, vBB, vCC
    DF_DA | DF_UB | DF_UC | DF_NULL_CHK_0 | DF_RANGE_CHK_1 | DF_CORE_B | DF_CORE_C,

    // 4B APUT vAA, vBB, vCC
    DF_UA | DF_UB | DF_UC | DF_NULL_CHK_1 | DF_RANGE_CHK_2 | DF_CORE_B | DF_CORE_C,

    // 4C APUT_WIDE vAA, vBB, vCC
    DF_UA_WIDE | DF_UB | DF_UC | DF_NULL_CHK_2 | DF_RANGE_CHK_3 | DF_CORE_B | DF_CORE_C,

    // 4D APUT_OBJECT vAA, vBB, vCC
    DF_UA | DF_UB | DF_UC | DF_NULL_CHK_1 | DF_RANGE_CHK_2 | DF_CORE_B | DF_CORE_C,

    // 4E APUT_BOOLEAN vAA, vBB, vCC
    DF_UA | DF_UB | DF_UC | DF_NULL_CHK_1 | DF_RANGE_CHK_2 | DF_CORE_B | DF_CORE_C,

    // 4F APUT_BYTE vAA, vBB, vCC
    DF_UA | DF_UB | DF_UC | DF_NULL_CHK_1 | DF_RANGE_CHK_2 | DF_CORE_B | DF_CORE_C,

    // 50 APUT_CHAR vAA, vBB, vCC
    DF_UA | DF_UB | DF_UC | DF_NULL_CHK_1 | DF_RANGE_CHK_2 | DF_CORE_B | DF_CORE_C,

    // 51 APUT_SHORT vAA, vBB, vCC
    DF_UA | DF_UB | DF_UC | DF_NULL_CHK_1 | DF_RANGE_CHK_2 | DF_CORE_B | DF_CORE_C,

    // 52 IGET vA, vB, field@CCCC
    DF_DA | DF_UB | DF_NULL_CHK_0 | DF_CORE_B,

    // 53 IGET_WIDE vA, vB, field@CCCC
    DF_DA_WIDE | DF_UB | DF_NULL_CHK_0 | DF_CORE_B,

    // 54 IGET_OBJECT vA, vB, field@CCCC
    DF_DA | DF_UB | DF_NULL_CHK_0 | DF_CORE_B,

    // 55 IGET_BOOLEAN vA, vB, field@CCCC
    DF_DA | DF_UB | DF_NULL_CHK_0 | DF_CORE_B,

    // 56 IGET_BYTE vA, vB, field@CCCC
    DF_DA | DF_UB | DF_NULL_CHK_0 | DF_CORE_B,

    // 57 IGET_CHAR vA, vB, field@CCCC
    DF_DA | DF_UB | DF_NULL_CHK_0 | DF_CORE_B,

    // 58 IGET_SHORT vA, vB, field@CCCC
    DF_DA | DF_UB | DF_NULL_CHK_0 | DF_CORE_B,

    // 59 IPUT vA, vB, field@CCCC
    DF_UA | DF_UB | DF_NULL_CHK_1 | DF_CORE_B,

    // 5A IPUT_WIDE vA, vB, field@CCCC
    DF_UA_WIDE | DF_UB | DF_NULL_CHK_2 | DF_CORE_B,

    // 5B IPUT_OBJECT vA, vB, field@CCCC
    DF_UA | DF_UB | DF_NULL_CHK_1 | DF_CORE_B,

    // 5C IPUT_BOOLEAN vA, vB, field@CCCC
    DF_UA | DF_UB | DF_NULL_CHK_1 | DF_CORE_B,

    // 5D IPUT_BYTE vA, vB, field@CCCC
    DF_UA | DF_UB | DF_NULL_CHK_1 | DF_CORE_B,

    // 5E IPUT_CHAR vA, vB, field@CCCC
    DF_UA | DF_UB | DF_NULL_CHK_1 | DF_CORE_B,

    // 5F IPUT_SHORT vA, vB, field@CCCC
    DF_UA | DF_UB | DF_NULL_CHK_1 | DF_CORE_B,

    // 60 SGET vAA, field@BBBB
    DF_DA | DF_UMS,

    // 61 SGET_WIDE vAA, field@BBBB
    DF_DA_WIDE | DF_UMS,

    // 62 SGET_OBJECT vAA, field@BBBB
    DF_DA | DF_CORE_A | DF_UMS,

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
    DF_UA_WIDE | DF_UMS,

    // 69 SPUT_OBJECT vAA, field@BBBB
    DF_UA | DF_CORE_A | DF_UMS,

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
    DF_DA_WIDE | DF_UB_WIDE | DF_CORE_A | DF_CORE_B,

    // 7E NOT_LONG vA, vB
    DF_DA_WIDE | DF_UB_WIDE | DF_CORE_A | DF_CORE_B,

    // 7F NEG_FLOAT vA, vB
    DF_DA | DF_UB | DF_FP_A | DF_FP_B,

    // 80 NEG_DOUBLE vA, vB
    DF_DA_WIDE | DF_UB_WIDE | DF_FP_A | DF_FP_B,

    // 81 INT_TO_LONG vA, vB
    DF_DA_WIDE | DF_UB | DF_CORE_A | DF_CORE_B,

    // 82 INT_TO_FLOAT vA, vB
    DF_DA | DF_UB | DF_FP_A | DF_CORE_B,

    // 83 INT_TO_DOUBLE vA, vB
    DF_DA_WIDE | DF_UB | DF_FP_A | DF_CORE_B,

    // 84 LONG_TO_INT vA, vB
    DF_DA | DF_UB_WIDE | DF_CORE_A | DF_CORE_B,

    // 85 LONG_TO_FLOAT vA, vB
    DF_DA | DF_UB_WIDE | DF_FP_A | DF_CORE_B,

    // 86 LONG_TO_DOUBLE vA, vB
    DF_DA_WIDE | DF_UB_WIDE | DF_FP_A | DF_CORE_B,

    // 87 FLOAT_TO_INT vA, vB
    DF_DA | DF_UB | DF_FP_B | DF_CORE_A,

    // 88 FLOAT_TO_LONG vA, vB
    DF_DA_WIDE | DF_UB | DF_FP_B | DF_CORE_A,

    // 89 FLOAT_TO_DOUBLE vA, vB
    DF_DA_WIDE | DF_UB | DF_FP_A | DF_FP_B,

    // 8A DOUBLE_TO_INT vA, vB
    DF_DA | DF_UB_WIDE | DF_FP_B | DF_CORE_A,

    // 8B DOUBLE_TO_LONG vA, vB
    DF_DA_WIDE | DF_UB_WIDE | DF_FP_B | DF_CORE_A,

    // 8C DOUBLE_TO_FLOAT vA, vB
    DF_DA | DF_UB_WIDE | DF_FP_A | DF_FP_B,

    // 8D INT_TO_BYTE vA, vB
    DF_DA | DF_UB | DF_CORE_A | DF_CORE_B,

    // 8E INT_TO_CHAR vA, vB
    DF_DA | DF_UB | DF_CORE_A | DF_CORE_B,

    // 8F INT_TO_SHORT vA, vB
    DF_DA | DF_UB | DF_CORE_A | DF_CORE_B,

    // 90 ADD_INT vAA, vBB, vCC
    DF_DA | DF_UB | DF_UC | DF_IS_LINEAR | DF_CORE_A | DF_CORE_B | DF_CORE_C,

    // 91 SUB_INT vAA, vBB, vCC
    DF_DA | DF_UB | DF_UC | DF_IS_LINEAR | DF_CORE_A | DF_CORE_B | DF_CORE_C,

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
    DF_DA_WIDE | DF_UB_WIDE | DF_UC_WIDE | DF_CORE_A | DF_CORE_B | DF_CORE_C,

    // 9C SUB_LONG vAA, vBB, vCC
    DF_DA_WIDE | DF_UB_WIDE | DF_UC_WIDE | DF_CORE_A | DF_CORE_B | DF_CORE_C,

    // 9D MUL_LONG vAA, vBB, vCC
    DF_DA_WIDE | DF_UB_WIDE | DF_UC_WIDE | DF_CORE_A | DF_CORE_B | DF_CORE_C,

    // 9E DIV_LONG vAA, vBB, vCC
    DF_DA_WIDE | DF_UB_WIDE | DF_UC_WIDE | DF_CORE_A | DF_CORE_B | DF_CORE_C,

    // 9F REM_LONG vAA, vBB, vCC
    DF_DA_WIDE | DF_UB_WIDE | DF_UC_WIDE | DF_CORE_A | DF_CORE_B | DF_CORE_C,

    // A0 AND_LONG vAA, vBB, vCC
    DF_DA_WIDE | DF_UB_WIDE | DF_UC_WIDE | DF_CORE_A | DF_CORE_B | DF_CORE_C,

    // A1 OR_LONG vAA, vBB, vCC
    DF_DA_WIDE | DF_UB_WIDE | DF_UC_WIDE | DF_CORE_A | DF_CORE_B | DF_CORE_C,

    // A2 XOR_LONG vAA, vBB, vCC
    DF_DA_WIDE | DF_UB_WIDE | DF_UC_WIDE | DF_CORE_A | DF_CORE_B | DF_CORE_C,

    // A3 SHL_LONG vAA, vBB, vCC
    DF_DA_WIDE | DF_UB_WIDE | DF_UC | DF_CORE_A | DF_CORE_B | DF_CORE_C,

    // A4 SHR_LONG vAA, vBB, vCC
    DF_DA_WIDE | DF_UB_WIDE | DF_UC | DF_CORE_A | DF_CORE_B | DF_CORE_C,

    // A5 USHR_LONG vAA, vBB, vCC
    DF_DA_WIDE | DF_UB_WIDE | DF_UC | DF_CORE_A | DF_CORE_B | DF_CORE_C,

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
    DF_DA_WIDE | DF_UB_WIDE | DF_UC_WIDE | DF_FP_A | DF_FP_B | DF_FP_C,

    // AC SUB_DOUBLE vAA, vBB, vCC
    DF_DA_WIDE | DF_UB_WIDE | DF_UC_WIDE | DF_FP_A | DF_FP_B | DF_FP_C,

    // AD MUL_DOUBLE vAA, vBB, vCC
    DF_DA_WIDE | DF_UB_WIDE | DF_UC_WIDE | DF_FP_A | DF_FP_B | DF_FP_C,

    // AE DIV_DOUBLE vAA, vBB, vCC
    DF_DA_WIDE | DF_UB_WIDE | DF_UC_WIDE | DF_FP_A | DF_FP_B | DF_FP_C,

    // AF REM_DOUBLE vAA, vBB, vCC
    DF_DA_WIDE | DF_UB_WIDE | DF_UC_WIDE | DF_FP_A | DF_FP_B | DF_FP_C,

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
    DF_DA_WIDE | DF_UA_WIDE | DF_UB_WIDE | DF_CORE_A | DF_CORE_B,

    // BC SUB_LONG_2ADDR vA, vB
    DF_DA_WIDE | DF_UA_WIDE | DF_UB_WIDE | DF_CORE_A | DF_CORE_B,

    // BD MUL_LONG_2ADDR vA, vB
    DF_DA_WIDE | DF_UA_WIDE | DF_UB_WIDE | DF_CORE_A | DF_CORE_B,

    // BE DIV_LONG_2ADDR vA, vB
    DF_DA_WIDE | DF_UA_WIDE | DF_UB_WIDE | DF_CORE_A | DF_CORE_B,

    // BF REM_LONG_2ADDR vA, vB
    DF_DA_WIDE | DF_UA_WIDE | DF_UB_WIDE | DF_CORE_A | DF_CORE_B,

    // C0 AND_LONG_2ADDR vA, vB
    DF_DA_WIDE | DF_UA_WIDE | DF_UB_WIDE | DF_CORE_A | DF_CORE_B,

    // C1 OR_LONG_2ADDR vA, vB
    DF_DA_WIDE | DF_UA_WIDE | DF_UB_WIDE | DF_CORE_A | DF_CORE_B,

    // C2 XOR_LONG_2ADDR vA, vB
    DF_DA_WIDE | DF_UA_WIDE | DF_UB_WIDE | DF_CORE_A | DF_CORE_B,

    // C3 SHL_LONG_2ADDR vA, vB
    DF_DA_WIDE | DF_UA_WIDE | DF_UB | DF_CORE_A | DF_CORE_B,

    // C4 SHR_LONG_2ADDR vA, vB
    DF_DA_WIDE | DF_UA_WIDE | DF_UB | DF_CORE_A | DF_CORE_B,

    // C5 USHR_LONG_2ADDR vA, vB
    DF_DA_WIDE | DF_UA_WIDE | DF_UB | DF_CORE_A | DF_CORE_B,

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
    DF_DA_WIDE | DF_UA_WIDE | DF_UB_WIDE | DF_FP_A | DF_FP_B,

    // CC SUB_DOUBLE_2ADDR vA, vB
    DF_DA_WIDE | DF_UA_WIDE | DF_UB_WIDE | DF_FP_A | DF_FP_B,

    // CD MUL_DOUBLE_2ADDR vA, vB
    DF_DA_WIDE | DF_UA_WIDE | DF_UB_WIDE | DF_FP_A | DF_FP_B,

    // CE DIV_DOUBLE_2ADDR vA, vB
    DF_DA_WIDE | DF_UA_WIDE | DF_UB_WIDE | DF_FP_A | DF_FP_B,

    // CF REM_DOUBLE_2ADDR vA, vB
    DF_DA_WIDE | DF_UA_WIDE | DF_UB_WIDE | DF_FP_A | DF_FP_B,

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
    DF_DA | DF_UB | DF_IS_LINEAR | DF_CORE_A | DF_CORE_B,

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
    DF_DA | DF_UB | DF_NULL_CHK_0 | DF_CORE_B,

    // E4 IPUT_VOLATILE
    DF_UA | DF_UB | DF_NULL_CHK_1 | DF_CORE_B,

    // E5 SGET_VOLATILE
    DF_DA | DF_UMS,

    // E6 SPUT_VOLATILE
    DF_UA | DF_UMS,

    // E7 IGET_OBJECT_VOLATILE
    DF_DA | DF_UB | DF_NULL_CHK_0 | DF_CORE_A | DF_CORE_B,

    // E8 IGET_WIDE_VOLATILE
    DF_DA_WIDE | DF_UB | DF_NULL_CHK_0 | DF_CORE_B,

    // E9 IPUT_WIDE_VOLATILE
    DF_UA_WIDE | DF_UB | DF_NULL_CHK_2 | DF_CORE_B,

    // EA SGET_WIDE_VOLATILE
    DF_DA_WIDE | DF_UMS,

    // EB SPUT_WIDE_VOLATILE
    DF_UA_WIDE | DF_UMS,

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
    DF_DA_WIDE | DF_UB | DF_NULL_CHK_0,

    // F4 IGET_OBJECT_QUICK
    DF_DA | DF_UB | DF_NULL_CHK_0,

    // F5 IPUT_QUICK
    DF_UA | DF_UB | DF_NULL_CHK_1,

    // F6 IPUT_WIDE_QUICK
    DF_UA_WIDE | DF_UB | DF_NULL_CHK_2,

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
    DF_UA | DF_UB | DF_NULL_CHK_1 | DF_CORE_A | DF_CORE_B,

    // FD SGET_OBJECT_VOLATILE
    DF_DA | DF_CORE_A | DF_UMS,

    // FE SPUT_OBJECT_VOLATILE
    DF_UA | DF_CORE_A | DF_UMS,

    // FF UNUSED_FF
    DF_NOP,

    // Beginning of extended MIR opcodes
    // 100 MIR_PHI
    DF_PHI | DF_DA | DF_NULL_TRANSFER_N,

    // 101 MIR_COPY
    DF_DA | DF_UB | DF_IS_MOVE,

    // 102 MIR_FUSED_CMPL_FLOAT
    DF_UA | DF_UB | DF_FP_A | DF_FP_B,

    // 103 MIR_FUSED_CMPG_FLOAT
    DF_UA | DF_UB | DF_FP_A | DF_FP_B,

    // 104 MIR_FUSED_CMPL_DOUBLE
    DF_UA_WIDE | DF_UB_WIDE | DF_FP_A | DF_FP_B,

    // 105 MIR_FUSED_CMPG_DOUBLE
    DF_UA_WIDE | DF_UB_WIDE | DF_FP_A | DF_FP_B,

    // 106 MIR_FUSED_CMP_LONG
    DF_UA_WIDE | DF_UB_WIDE | DF_CORE_A | DF_CORE_B,

    // 107 MIR_NOP
    DF_NOP,

    // 108 MIR_NULL_RANGE_UP_CHECK
    0,

    // 109 MIR_NULL_RANGE_DOWN_CHECK
    0,

    // 110 MIR_LOWER_BOUND
    0,
};

/* Return the base virtual register for a SSA name */
int SRegToVReg(const CompilationUnit* cUnit, int ssaReg)
{
    DCHECK_LT(ssaReg, (int)cUnit->ssaBaseVRegs->numUsed);
    return GET_ELEM_N(cUnit->ssaBaseVRegs, int, ssaReg);
}

int SRegToSubscript(const CompilationUnit* cUnit, int ssaReg)
{
    DCHECK(ssaReg < (int)cUnit->ssaSubscripts->numUsed);
    return GET_ELEM_N(cUnit->ssaSubscripts, int, ssaReg);
}

int getSSAUseCount(CompilationUnit* cUnit, int sReg)
{
    DCHECK(sReg < (int)cUnit->rawUseCounts.numUsed);
    return cUnit->rawUseCounts.elemList[sReg];
}


char* oatGetDalvikDisassembly(CompilationUnit* cUnit,
                              const DecodedInstruction& insn, const char* note)
{
    char buffer[256];
    Instruction::Code opcode = insn.opcode;
    int dfAttributes = oatDataFlowAttributes[opcode];
    int flags;
    char* ret;

    buffer[0] = 0;
    if ((int)opcode >= (int)kMirOpFirst) {
        if ((int)opcode == (int)kMirOpPhi) {
            strcpy(buffer, "PHI");
        } else {
            sprintf(buffer, "Opcode %#x", opcode);
        }
        flags = 0;
    } else {
        strcpy(buffer, Instruction::Name(opcode));
        flags = Instruction::Flags(opcode);
    }

    if (note)
        strcat(buffer, note);

    /* For branches, decode the instructions to print out the branch targets */
    if (flags & Instruction::kBranch) {
        Instruction::Format dalvikFormat = Instruction::FormatOf(insn.opcode);
        int offset = 0;
        switch (dalvikFormat) {
            case Instruction::k21t:
                snprintf(buffer + strlen(buffer), 256, " v%d,", insn.vA);
                offset = (int) insn.vB;
                break;
            case Instruction::k22t:
                snprintf(buffer + strlen(buffer), 256, " v%d, v%d,", insn.vA, insn.vB);
                offset = (int) insn.vC;
                break;
            case Instruction::k10t:
            case Instruction::k20t:
            case Instruction::k30t:
                offset = (int) insn.vA;
                break;
            default:
                LOG(FATAL) << "Unexpected branch format " << (int)dalvikFormat
                    << " / opcode " << (int)opcode;
        }
        snprintf(buffer + strlen(buffer), 256, " (%c%x)",
                 offset > 0 ? '+' : '-',
                 offset > 0 ? offset : -offset);
    } else if (dfAttributes & DF_FORMAT_35C) {
        unsigned int i;
        for (i = 0; i < insn.vA; i++) {
            if (i != 0) strcat(buffer, ",");
            snprintf(buffer + strlen(buffer), 256, " v%d", insn.arg[i]);
        }
    }
    else if (dfAttributes & DF_FORMAT_3RC) {
        snprintf(buffer + strlen(buffer), 256,
                 " v%d..v%d", insn.vC, insn.vC + insn.vA - 1);
    }
    else {
        if (dfAttributes & DF_A_IS_REG) {
            snprintf(buffer + strlen(buffer), 256, " v%d", insn.vA);
        }
        if (dfAttributes & DF_B_IS_REG) {
            snprintf(buffer + strlen(buffer), 256, ", v%d", insn.vB);
        }
        else if ((int)opcode < (int)kMirOpFirst) {
            snprintf(buffer + strlen(buffer), 256, ", (#%d)", insn.vB);
        }
        if (dfAttributes & DF_C_IS_REG) {
            snprintf(buffer + strlen(buffer), 256, ", v%d", insn.vC);
        }
        else if ((int)opcode < (int)kMirOpFirst) {
            snprintf(buffer + strlen(buffer), 256, ", (#%d)", insn.vC);
        }
    }
    int length = strlen(buffer) + 1;
    ret = (char*)oatNew(cUnit, length, false, kAllocDFInfo);
    memcpy(ret, buffer, length);
    return ret;
}

char* getSSAName(const CompilationUnit* cUnit, int ssaReg, char* name)
{
    sprintf(name, "v%d_%d", SRegToVReg(cUnit, ssaReg),
            SRegToSubscript(cUnit, ssaReg));
    return name;
}

/*
 * Dalvik instruction disassembler with optional SSA printing.
 */
char* oatFullDisassembler(CompilationUnit* cUnit, const MIR* mir)
{
    char buffer[256];
    char operand0[32], operand1[32];
    const DecodedInstruction* insn = &mir->dalvikInsn;
    Instruction::Code opcode = insn->opcode;
    int dfAttributes = oatDataFlowAttributes[opcode];
    char* ret;
    int length;

    buffer[0] = 0;
    if (static_cast<int>(opcode) >= static_cast<int>(kMirOpFirst)) {
        if (static_cast<int>(opcode) == static_cast<int>(kMirOpPhi)) {
            snprintf(buffer, 256, "PHI %s = (%s",
                     getSSAName(cUnit, mir->ssaRep->defs[0], operand0),
                     getSSAName(cUnit, mir->ssaRep->uses[0], operand1));
            int i;
            for (i = 1; i < mir->ssaRep->numUses; i++) {
                snprintf(buffer + strlen(buffer), 256, ", %s",
                         getSSAName(cUnit, mir->ssaRep->uses[i], operand0));
            }
            snprintf(buffer + strlen(buffer), 256, ")");
        }
        else {
            sprintf(buffer, "Opcode %#x", opcode);
        }
        goto done;
    } else {
        strcpy(buffer, Instruction::Name(opcode));
    }

    /* For branches, decode the instructions to print out the branch targets */
    if (Instruction::Flags(insn->opcode) & Instruction::kBranch) {
        Instruction::Format dalvikFormat = Instruction::FormatOf(insn->opcode);
        int delta = 0;
        switch (dalvikFormat) {
            case Instruction::k21t:
                snprintf(buffer + strlen(buffer), 256, " %s, ",
                         getSSAName(cUnit, mir->ssaRep->uses[0], operand0));
                delta = (int) insn->vB;
                break;
            case Instruction::k22t:
                snprintf(buffer + strlen(buffer), 256, " %s, %s, ",
                         getSSAName(cUnit, mir->ssaRep->uses[0], operand0),
                         getSSAName(cUnit, mir->ssaRep->uses[1], operand1));
                delta = (int) insn->vC;
                break;
            case Instruction::k10t:
            case Instruction::k20t:
            case Instruction::k30t:
                delta = (int) insn->vA;
                break;
            default:
                LOG(FATAL) << "Unexpected branch format: " <<
                   (int)dalvikFormat;
        }
        snprintf(buffer + strlen(buffer), 256, " %04x",
                 mir->offset + delta);
    } else if (dfAttributes & (DF_FORMAT_35C | DF_FORMAT_3RC)) {
        unsigned int i;
        for (i = 0; i < insn->vA; i++) {
            if (i != 0) strcat(buffer, ",");
            snprintf(buffer + strlen(buffer), 256, " %s",
                     getSSAName(cUnit, mir->ssaRep->uses[i], operand0));
        }
    } else {
        int udIdx;
        if (mir->ssaRep->numDefs) {

            for (udIdx = 0; udIdx < mir->ssaRep->numDefs; udIdx++) {
                snprintf(buffer + strlen(buffer), 256, " %s",
                         getSSAName(cUnit, mir->ssaRep->defs[udIdx], operand0));
            }
            strcat(buffer, ",");
        }
        if (mir->ssaRep->numUses) {
            /* No leading ',' for the first use */
            snprintf(buffer + strlen(buffer), 256, " %s",
                     getSSAName(cUnit, mir->ssaRep->uses[0], operand0));
            for (udIdx = 1; udIdx < mir->ssaRep->numUses; udIdx++) {
                snprintf(buffer + strlen(buffer), 256, ", %s",
                         getSSAName(cUnit, mir->ssaRep->uses[udIdx], operand0));
            }
        }
        if (static_cast<int>(opcode) < static_cast<int>(kMirOpFirst)) {
            Instruction::Format dalvikFormat = Instruction::FormatOf(opcode);
            switch (dalvikFormat) {
                case Instruction::k11n:        // op vA, #+B
                case Instruction::k21s:        // op vAA, #+BBBB
                case Instruction::k21h:        // op vAA, #+BBBB00000[00000000]
                case Instruction::k31i:        // op vAA, #+BBBBBBBB
                case Instruction::k51l:        // op vAA, #+BBBBBBBBBBBBBBBB
                    snprintf(buffer + strlen(buffer), 256, " #%#x", insn->vB);
                    break;
                case Instruction::k21c:        // op vAA, thing@BBBB
                case Instruction::k31c:        // op vAA, thing@BBBBBBBB
                    snprintf(buffer + strlen(buffer), 256, " @%#x", insn->vB);
                    break;
                case Instruction::k22b:        // op vAA, vBB, #+CC
                case Instruction::k22s:        // op vA, vB, #+CCCC
                    snprintf(buffer + strlen(buffer), 256, " #%#x", insn->vC);
                    break;
                case Instruction::k22c:        // op vA, vB, thing@CCCC
                    snprintf(buffer + strlen(buffer), 256, " @%#x", insn->vC);
                    break;
                    /* No need for special printing */
                default:
                    break;
            }
        }
    }

done:
    length = strlen(buffer) + 1;
    ret = (char*) oatNew(cUnit, length, false, kAllocDFInfo);
    memcpy(ret, buffer, length);
    return ret;
}

char* oatGetSSAString(CompilationUnit* cUnit, SSARepresentation* ssaRep)
{
    char buffer[256];
    char* ret;
    int i;

    buffer[0] = 0;
    for (i = 0; i < ssaRep->numDefs; i++) {
        int ssaReg = ssaRep->defs[i];
        sprintf(buffer + strlen(buffer), "s%d(v%d_%d) ", ssaReg,
                SRegToVReg(cUnit, ssaReg), SRegToSubscript(cUnit, ssaReg));
    }

    if (ssaRep->numDefs) {
        strcat(buffer, "<- ");
    }

    for (i = 0; i < ssaRep->numUses; i++) {
        int len = strlen(buffer);
        int ssaReg = ssaRep->uses[i];

        if (snprintf(buffer + len, 250 - len, "s%d(v%d_%d) ", ssaReg,
                     SRegToVReg(cUnit, ssaReg),
                     SRegToSubscript(cUnit, ssaReg))) {
            strcat(buffer, "...");
            break;
        }
    }

    int length = strlen(buffer) + 1;
    ret = (char*)oatNew(cUnit, length, false, kAllocDFInfo);
    memcpy(ret, buffer, length);
    return ret;
}

/* Any register that is used before being defined is considered live-in */
inline void handleLiveInUse(CompilationUnit* cUnit, ArenaBitVector* useV,
                            ArenaBitVector* defV, ArenaBitVector* liveInV,
                            int dalvikRegId)
{
    oatSetBit(cUnit, useV, dalvikRegId);
    if (!oatIsBitSet(defV, dalvikRegId)) {
        oatSetBit(cUnit, liveInV, dalvikRegId);
    }
}

/* Mark a reg as being defined */
inline void handleDef(CompilationUnit* cUnit, ArenaBitVector* defV,
                      int dalvikRegId)
{
    oatSetBit(cUnit, defV, dalvikRegId);
}

/*
 * Find out live-in variables for natural loops. Variables that are live-in in
 * the main loop body are considered to be defined in the entry block.
 */
bool oatFindLocalLiveIn(CompilationUnit* cUnit, BasicBlock* bb)
{
    MIR* mir;
    ArenaBitVector *useV, *defV, *liveInV;

    if (bb->dataFlowInfo == NULL) return false;

    useV = bb->dataFlowInfo->useV =
        oatAllocBitVector(cUnit, cUnit->numDalvikRegisters, false, kBitMapUse);
    defV = bb->dataFlowInfo->defV =
        oatAllocBitVector(cUnit, cUnit->numDalvikRegisters, false, kBitMapDef);
    liveInV = bb->dataFlowInfo->liveInV =
        oatAllocBitVector(cUnit, cUnit->numDalvikRegisters, false,
                          kBitMapLiveIn);

    for (mir = bb->firstMIRInsn; mir; mir = mir->next) {
        int dfAttributes =
            oatDataFlowAttributes[mir->dalvikInsn.opcode];
        DecodedInstruction *dInsn = &mir->dalvikInsn;

        if (dfAttributes & DF_HAS_USES) {
            if (dfAttributes & DF_UA) {
                handleLiveInUse(cUnit, useV, defV, liveInV, dInsn->vA);
            } else if (dfAttributes & DF_UA_WIDE) {
                handleLiveInUse(cUnit, useV, defV, liveInV, dInsn->vA);
                handleLiveInUse(cUnit, useV, defV, liveInV, dInsn->vA+1);
            }
            if (dfAttributes & DF_UB) {
                handleLiveInUse(cUnit, useV, defV, liveInV, dInsn->vB);
            } else if (dfAttributes & DF_UB_WIDE) {
                handleLiveInUse(cUnit, useV, defV, liveInV, dInsn->vB);
                handleLiveInUse(cUnit, useV, defV, liveInV, dInsn->vB+1);
            }
            if (dfAttributes & DF_UC) {
                handleLiveInUse(cUnit, useV, defV, liveInV, dInsn->vC);
            } else if (dfAttributes & DF_UC_WIDE) {
                handleLiveInUse(cUnit, useV, defV, liveInV, dInsn->vC);
                handleLiveInUse(cUnit, useV, defV, liveInV, dInsn->vC+1);
            }
        }
        if (dfAttributes & DF_FORMAT_35C) {
            for (unsigned int i = 0; i < dInsn->vA; i++) {
                handleLiveInUse(cUnit, useV, defV, liveInV, dInsn->arg[i]);
           }
        }
        if (dfAttributes & DF_FORMAT_3RC) {
            for (unsigned int i = 0; i < dInsn->vA; i++) {
                handleLiveInUse(cUnit, useV, defV, liveInV, dInsn->vC+i);
           }
        }
        if (dfAttributes & DF_HAS_DEFS) {
            handleDef(cUnit, defV, dInsn->vA);
            if (dfAttributes & DF_DA_WIDE) {
                handleDef(cUnit, defV, dInsn->vA+1);
            }
        }
    }
    return true;
}

int addNewSReg(CompilationUnit* cUnit, int vReg)
{
    // Compiler temps always have a subscript of 0
    int subscript = (vReg < 0) ? 0 : ++cUnit->SSALastDefs[vReg];
    int ssaReg = cUnit->numSSARegs++;
    oatInsertGrowableList(cUnit, cUnit->ssaBaseVRegs, vReg);
    oatInsertGrowableList(cUnit, cUnit->ssaSubscripts, subscript);
    DCHECK_EQ(cUnit->ssaBaseVRegs->numUsed, cUnit->ssaSubscripts->numUsed);
    return ssaReg;
}

/* Find out the latest SSA register for a given Dalvik register */
void handleSSAUse(CompilationUnit* cUnit, int* uses, int dalvikReg,
                  int regIndex)
{
    DCHECK((dalvikReg >= 0) && (dalvikReg < cUnit->numDalvikRegisters));
    uses[regIndex] = cUnit->vRegToSSAMap[dalvikReg];
}

/* Setup a new SSA register for a given Dalvik register */
void handleSSADef(CompilationUnit* cUnit, int* defs, int dalvikReg,
                  int regIndex)
{
    DCHECK((dalvikReg >= 0) && (dalvikReg < cUnit->numDalvikRegisters));
    int ssaReg = addNewSReg(cUnit, dalvikReg);
    cUnit->vRegToSSAMap[dalvikReg] = ssaReg;
    defs[regIndex] = ssaReg;
}

/* Look up new SSA names for format_35c instructions */
void dataFlowSSAFormat35C(CompilationUnit* cUnit, MIR* mir)
{
    DecodedInstruction *dInsn = &mir->dalvikInsn;
    int numUses = dInsn->vA;
    int i;

    mir->ssaRep->numUses = numUses;
    mir->ssaRep->uses = (int *)oatNew(cUnit, sizeof(int) * numUses, true,
                                      kAllocDFInfo);
    // NOTE: will be filled in during type & size inference pass
    mir->ssaRep->fpUse = (bool *)oatNew(cUnit, sizeof(bool) * numUses, true,
                                        kAllocDFInfo);

    for (i = 0; i < numUses; i++) {
        handleSSAUse(cUnit, mir->ssaRep->uses, dInsn->arg[i], i);
    }
}

/* Look up new SSA names for format_3rc instructions */
void dataFlowSSAFormat3RC(CompilationUnit* cUnit, MIR* mir)
{
    DecodedInstruction *dInsn = &mir->dalvikInsn;
    int numUses = dInsn->vA;
    int i;

    mir->ssaRep->numUses = numUses;
    mir->ssaRep->uses = (int *)oatNew(cUnit, sizeof(int) * numUses, true,
                                      kAllocDFInfo);
    // NOTE: will be filled in during type & size inference pass
    mir->ssaRep->fpUse = (bool *)oatNew(cUnit, sizeof(bool) * numUses, true,
                                        kAllocDFInfo);

    for (i = 0; i < numUses; i++) {
        handleSSAUse(cUnit, mir->ssaRep->uses, dInsn->vC+i, i);
    }
}

/* Entry function to convert a block into SSA representation */
bool oatDoSSAConversion(CompilationUnit* cUnit, BasicBlock* bb)
{
    MIR* mir;

    if (bb->dataFlowInfo == NULL) return false;

    for (mir = bb->firstMIRInsn; mir; mir = mir->next) {
        mir->ssaRep = (struct SSARepresentation *)
            oatNew(cUnit, sizeof(SSARepresentation), true, kAllocDFInfo);

        int dfAttributes =
            oatDataFlowAttributes[mir->dalvikInsn.opcode];

        // If not a pseudo-op, note non-leaf or can throw
        if (static_cast<int>(mir->dalvikInsn.opcode) < static_cast<int>(kNumPackedOpcodes)) {
            int flags = Instruction::Flags(mir->dalvikInsn.opcode);

            if (flags & Instruction::kThrow) {
                cUnit->attrs &= ~METHOD_IS_THROW_FREE;
            }

            if (flags & Instruction::kInvoke) {
                cUnit->attrs &= ~METHOD_IS_LEAF;
            }
        }

        int numUses = 0;

        if (dfAttributes & DF_FORMAT_35C) {
            dataFlowSSAFormat35C(cUnit, mir);
            continue;
        }

        if (dfAttributes & DF_FORMAT_3RC) {
            dataFlowSSAFormat3RC(cUnit, mir);
            continue;
        }

        if (dfAttributes & DF_HAS_USES) {
            if (dfAttributes & DF_UA) {
                numUses++;
            } else if (dfAttributes & DF_UA_WIDE) {
                numUses += 2;
            }
            if (dfAttributes & DF_UB) {
                numUses++;
            } else if (dfAttributes & DF_UB_WIDE) {
                numUses += 2;
            }
            if (dfAttributes & DF_UC) {
                numUses++;
            } else if (dfAttributes & DF_UC_WIDE) {
                numUses += 2;
            }
        }

        if (numUses) {
            mir->ssaRep->numUses = numUses;
            mir->ssaRep->uses = (int *)oatNew(cUnit, sizeof(int) * numUses,
                                              false, kAllocDFInfo);
            mir->ssaRep->fpUse = (bool *)oatNew(cUnit, sizeof(bool) * numUses,
                                                false, kAllocDFInfo);
        }

        int numDefs = 0;

        if (dfAttributes & DF_HAS_DEFS) {
            numDefs++;
            if (dfAttributes & DF_DA_WIDE) {
                numDefs++;
            }
        }

        if (numDefs) {
            mir->ssaRep->numDefs = numDefs;
            mir->ssaRep->defs = (int *)oatNew(cUnit, sizeof(int) * numDefs,
                                              false, kAllocDFInfo);
            mir->ssaRep->fpDef = (bool *)oatNew(cUnit, sizeof(bool) * numDefs,
                                                false, kAllocDFInfo);
        }

        DecodedInstruction *dInsn = &mir->dalvikInsn;

        if (dfAttributes & DF_HAS_USES) {
            numUses = 0;
            if (dfAttributes & DF_UA) {
                mir->ssaRep->fpUse[numUses] = dfAttributes & DF_FP_A;
                handleSSAUse(cUnit, mir->ssaRep->uses, dInsn->vA, numUses++);
            } else if (dfAttributes & DF_UA_WIDE) {
                mir->ssaRep->fpUse[numUses] = dfAttributes & DF_FP_A;
                handleSSAUse(cUnit, mir->ssaRep->uses, dInsn->vA, numUses++);
                mir->ssaRep->fpUse[numUses] = dfAttributes & DF_FP_A;
                handleSSAUse(cUnit, mir->ssaRep->uses, dInsn->vA+1, numUses++);
            }
            if (dfAttributes & DF_UB) {
                mir->ssaRep->fpUse[numUses] = dfAttributes & DF_FP_B;
                handleSSAUse(cUnit, mir->ssaRep->uses, dInsn->vB, numUses++);
            } else if (dfAttributes & DF_UB_WIDE) {
                mir->ssaRep->fpUse[numUses] = dfAttributes & DF_FP_B;
                handleSSAUse(cUnit, mir->ssaRep->uses, dInsn->vB, numUses++);
                mir->ssaRep->fpUse[numUses] = dfAttributes & DF_FP_B;
                handleSSAUse(cUnit, mir->ssaRep->uses, dInsn->vB+1, numUses++);
            }
            if (dfAttributes & DF_UC) {
                mir->ssaRep->fpUse[numUses] = dfAttributes & DF_FP_C;
                handleSSAUse(cUnit, mir->ssaRep->uses, dInsn->vC, numUses++);
            } else if (dfAttributes & DF_UC_WIDE) {
                mir->ssaRep->fpUse[numUses] = dfAttributes & DF_FP_C;
                handleSSAUse(cUnit, mir->ssaRep->uses, dInsn->vC, numUses++);
                mir->ssaRep->fpUse[numUses] = dfAttributes & DF_FP_C;
                handleSSAUse(cUnit, mir->ssaRep->uses, dInsn->vC+1, numUses++);
            }
        }
        if (dfAttributes & DF_HAS_DEFS) {
            mir->ssaRep->fpDef[0] = dfAttributes & DF_FP_A;
            handleSSADef(cUnit, mir->ssaRep->defs, dInsn->vA, 0);
            if (dfAttributes & DF_DA_WIDE) {
                mir->ssaRep->fpDef[1] = dfAttributes & DF_FP_A;
                handleSSADef(cUnit, mir->ssaRep->defs, dInsn->vA+1, 1);
            }
        }
    }

    if (!cUnit->disableDataflow) {
        /*
         * Take a snapshot of Dalvik->SSA mapping at the end of each block. The
         * input to PHI nodes can be derived from the snapshot of all
         * predecessor blocks.
         */
        bb->dataFlowInfo->vRegToSSAMap =
            (int *)oatNew(cUnit, sizeof(int) * cUnit->numDalvikRegisters, false,
                          kAllocDFInfo);

        memcpy(bb->dataFlowInfo->vRegToSSAMap, cUnit->vRegToSSAMap,
               sizeof(int) * cUnit->numDalvikRegisters);
    }
    return true;
}

/* Setup a constant value for opcodes thare have the DF_SETS_CONST attribute */
void setConstant(CompilationUnit* cUnit, int ssaReg, int value)
{
    oatSetBit(cUnit, cUnit->isConstantV, ssaReg);
    cUnit->constantValues[ssaReg] = value;
}

bool oatDoConstantPropagation(CompilationUnit* cUnit, BasicBlock* bb)
{
    MIR* mir;
    ArenaBitVector *isConstantV = cUnit->isConstantV;

    for (mir = bb->firstMIRInsn; mir; mir = mir->next) {
        int dfAttributes =
            oatDataFlowAttributes[mir->dalvikInsn.opcode];

        DecodedInstruction *dInsn = &mir->dalvikInsn;

        if (!(dfAttributes & DF_HAS_DEFS)) continue;

        /* Handle instructions that set up constants directly */
        if (dfAttributes & DF_SETS_CONST) {
            if (dfAttributes & DF_DA) {
                switch (dInsn->opcode) {
                    case Instruction::CONST_4:
                    case Instruction::CONST_16:
                    case Instruction::CONST:
                        setConstant(cUnit, mir->ssaRep->defs[0], dInsn->vB);
                        break;
                    case Instruction::CONST_HIGH16:
                        setConstant(cUnit, mir->ssaRep->defs[0],
                                    dInsn->vB << 16);
                        break;
                    default:
                        break;
                }
            } else if (dfAttributes & DF_DA_WIDE) {
                switch (dInsn->opcode) {
                    case Instruction::CONST_WIDE_16:
                    case Instruction::CONST_WIDE_32:
                        setConstant(cUnit, mir->ssaRep->defs[0], dInsn->vB);
                        setConstant(cUnit, mir->ssaRep->defs[1], 0);
                        break;
                    case Instruction::CONST_WIDE:
                        setConstant(cUnit, mir->ssaRep->defs[0],
                                    (int) dInsn->vB_wide);
                        setConstant(cUnit, mir->ssaRep->defs[1],
                                    (int) (dInsn->vB_wide >> 32));
                        break;
                    case Instruction::CONST_WIDE_HIGH16:
                        setConstant(cUnit, mir->ssaRep->defs[0], 0);
                        setConstant(cUnit, mir->ssaRep->defs[1],
                                    dInsn->vB << 16);
                        break;
                    default:
                        break;
                }
            }
        /* Handle instructions that set up constants directly */
        } else if (dfAttributes & DF_IS_MOVE) {
            int i;

            for (i = 0; i < mir->ssaRep->numUses; i++) {
                if (!oatIsBitSet(isConstantV, mir->ssaRep->uses[i])) break;
            }
            /* Move a register holding a constant to another register */
            if (i == mir->ssaRep->numUses) {
                setConstant(cUnit, mir->ssaRep->defs[0],
                            cUnit->constantValues[mir->ssaRep->uses[0]]);
                if (dfAttributes & DF_DA_WIDE) {
                    setConstant(cUnit, mir->ssaRep->defs[1],
                                cUnit->constantValues[mir->ssaRep->uses[1]]);
                }
            }
        }
    }
    /* TODO: implement code to handle arithmetic operations */
    return true;
}

/* Setup the basic data structures for SSA conversion */
void oatInitializeSSAConversion(CompilationUnit* cUnit)
{
    int i;
    int numDalvikReg = cUnit->numDalvikRegisters;

    cUnit->ssaBaseVRegs = (GrowableList *)oatNew(cUnit, sizeof(GrowableList),
                                                 false, kAllocDFInfo);
    cUnit->ssaSubscripts = (GrowableList *)oatNew(cUnit, sizeof(GrowableList),
                                                  false, kAllocDFInfo);
    // Create the ssa mappings, estimating the max size
    oatInitGrowableList(cUnit, cUnit->ssaBaseVRegs,
                        numDalvikReg + cUnit->defCount + 128,
                        kListSSAtoDalvikMap);
    oatInitGrowableList(cUnit, cUnit->ssaSubscripts,
                        numDalvikReg + cUnit->defCount + 128,
                        kListSSAtoDalvikMap);
    /*
     * Initial number of SSA registers is equal to the number of Dalvik
     * registers.
     */
    cUnit->numSSARegs = numDalvikReg;

    /*
     * Initialize the SSA2Dalvik map list. For the first numDalvikReg elements,
     * the subscript is 0 so we use the ENCODE_REG_SUB macro to encode the value
     * into "(0 << 16) | i"
     */
    for (i = 0; i < numDalvikReg; i++) {
        oatInsertGrowableList(cUnit, cUnit->ssaBaseVRegs, i);
        oatInsertGrowableList(cUnit, cUnit->ssaSubscripts, 0);
    }

    /*
     * Initialize the DalvikToSSAMap map. There is one entry for each
     * Dalvik register, and the SSA names for those are the same.
     */
    cUnit->vRegToSSAMap = (int *)oatNew(cUnit, sizeof(int) * numDalvikReg,
                                          false, kAllocDFInfo);
    /* Keep track of the higest def for each dalvik reg */
    cUnit->SSALastDefs = (int *)oatNew(cUnit, sizeof(int) * numDalvikReg,
                                       false, kAllocDFInfo);

    for (i = 0; i < numDalvikReg; i++) {
        cUnit->vRegToSSAMap[i] = i;
        cUnit->SSALastDefs[i] = 0;
    }

    /* Add ssa reg for Method* */
    cUnit->methodSReg = addNewSReg(cUnit, SSA_METHOD_BASEREG);

    /*
     * Allocate the BasicBlockDataFlow structure for the entry and code blocks
     */
    GrowableListIterator iterator;

    oatGrowableListIteratorInit(&cUnit->blockList, &iterator);

    while (true) {
        BasicBlock* bb = (BasicBlock *) oatGrowableListIteratorNext(&iterator);
        if (bb == NULL) break;
        if (bb->hidden == true) continue;
        if (bb->blockType == kDalvikByteCode ||
            bb->blockType == kEntryBlock ||
            bb->blockType == kExitBlock) {
            bb->dataFlowInfo = (BasicBlockDataFlow *)
                oatNew(cUnit, sizeof(BasicBlockDataFlow),
                       true, kAllocDFInfo);
        }
    }
}

/* Clear the visited flag for each BB */
bool oatClearVisitedFlag(struct CompilationUnit* cUnit, struct BasicBlock* bb)
{
    bb->visited = false;
    return true;
}

void oatDataFlowAnalysisDispatcher(CompilationUnit* cUnit,
                bool (*func)(CompilationUnit*, BasicBlock*),
                DataFlowAnalysisMode dfaMode,
                bool isIterative)
{
    bool change = true;

    while (change) {
        change = false;

        switch (dfaMode) {
        /* Scan all blocks and perform the operations specified in func */
        case kAllNodes:
            {
                GrowableListIterator iterator;
                oatGrowableListIteratorInit(&cUnit->blockList, &iterator);
                while (true) {
                    BasicBlock* bb =
                        (BasicBlock *) oatGrowableListIteratorNext(&iterator);
                    if (bb == NULL) break;
                    if (bb->hidden == true) continue;
                    change |= (*func)(cUnit, bb);
                }
            }
            break;
        /* Scan reachable blocks and perform the ops specified in func. */
        case kReachableNodes:
            {
                int numReachableBlocks = cUnit->numReachableBlocks;
                int idx;
                const GrowableList *blockList = &cUnit->blockList;

                for (idx = 0; idx < numReachableBlocks; idx++) {
                    int blockIdx = cUnit->dfsOrder.elemList[idx];
                    BasicBlock* bb =
                        (BasicBlock *) oatGrowableListGetElement(blockList,
                                                                 blockIdx);
                    change |= (*func)(cUnit, bb);
                }
            }
            break;

        /* Scan reachable blocks by pre-order dfs and invoke func on each. */
        case kPreOrderDFSTraversal:
            {
                int numReachableBlocks = cUnit->numReachableBlocks;
                int idx;
                const GrowableList *blockList = &cUnit->blockList;

                for (idx = 0; idx < numReachableBlocks; idx++) {
                    int dfsIdx = cUnit->dfsOrder.elemList[idx];
                    BasicBlock* bb =
                        (BasicBlock *) oatGrowableListGetElement(blockList,
                                                                 dfsIdx);
                    change |= (*func)(cUnit, bb);
                }
            }
            break;
        /* Scan reachable blocks post-order dfs and invoke func on each. */
        case kPostOrderDFSTraversal:
            {
                int numReachableBlocks = cUnit->numReachableBlocks;
                int idx;
                const GrowableList *blockList = &cUnit->blockList;

                for (idx = numReachableBlocks - 1; idx >= 0; idx--) {
                    int dfsIdx = cUnit->dfsOrder.elemList[idx];
                    BasicBlock* bb =
                        (BasicBlock *) oatGrowableListGetElement(blockList,
                                                                 dfsIdx);
                    change |= (*func)(cUnit, bb);
                }
            }
            break;
        /* Scan reachable post-order dom tree and invoke func on each. */
        case kPostOrderDOMTraversal:
            {
                int numReachableBlocks = cUnit->numReachableBlocks;
                int idx;
                const GrowableList *blockList = &cUnit->blockList;

                for (idx = 0; idx < numReachableBlocks; idx++) {
                    int domIdx = cUnit->domPostOrderTraversal.elemList[idx];
                    BasicBlock* bb =
                        (BasicBlock *) oatGrowableListGetElement(blockList,
                                                                 domIdx);
                    change |= (*func)(cUnit, bb);
                }
            }
            break;
        /* Scan reachable blocks reverse post-order dfs, invoke func on each */
        case kReversePostOrderTraversal:
            {
                int numReachableBlocks = cUnit->numReachableBlocks;
                int idx;
                const GrowableList *blockList = &cUnit->blockList;

                for (idx = numReachableBlocks - 1; idx >= 0; idx--) {
                    int revIdx = cUnit->dfsPostOrder.elemList[idx];
                    BasicBlock* bb =
                        (BasicBlock *) oatGrowableListGetElement(blockList,
                                                                 revIdx);
                    change |= (*func)(cUnit, bb);
                }
            }
            break;
        default:
            LOG(FATAL) << "Unknown traversal mode " << (int)dfaMode;
        }
        /* If isIterative is false, exit the loop after the first iteration */
        change &= isIterative;
    }
}

/* Advance to next strictly dominated MIR node in an extended basic block */
MIR* advanceMIR(CompilationUnit* cUnit, BasicBlock** pBb, MIR* mir, ArenaBitVector* bv,
                bool clearMark) {
    BasicBlock* bb = *pBb;
    if (mir != NULL) {
        mir = mir->next;
        if (mir == NULL) {
            bb = bb->fallThrough;
            if ((bb == NULL) || bb->predecessors->numUsed != 1) {
                mir = NULL;
            } else {
                if (bv) {
                    oatSetBit(cUnit, bv, bb->id);
                }
                *pBb = bb;
                mir = bb->firstMIRInsn;
            }
        }
    }
    if (mir && clearMark) {
        mir->optimizationFlags &= ~MIR_MARK;
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
MIR* oatFindMoveResult(CompilationUnit* cUnit, BasicBlock* bb, MIR* mir,
                       bool wide)
{
    BasicBlock* tbb = bb;
    mir = advanceMIR(cUnit, &tbb, mir, NULL, false);
    while (mir != NULL) {
        if (!wide && mir->dalvikInsn.opcode == Instruction::MOVE_RESULT) {
            break;
        }
        if (wide && mir->dalvikInsn.opcode == Instruction::MOVE_RESULT_WIDE) {
            break;
        }
        // Keep going if pseudo op, otherwise terminate
        if (mir->dalvikInsn.opcode < static_cast<Instruction::Code>(kNumPackedOpcodes)) {
            mir = NULL;
        } else {
            mir = advanceMIR(cUnit, &tbb, mir, NULL, false);
        }
    }
    return mir;
}

void squashDupRangeChecks(CompilationUnit* cUnit, BasicBlock** pBp, MIR* mir,
                          int arraySreg, int indexSreg)
{
    while (true) {
       mir = advanceMIR(cUnit, pBp, mir, NULL, false);
       if (!mir) {
           break;
       }
       if ((mir->ssaRep == NULL) ||
           (mir->optimizationFlags & MIR_IGNORE_RANGE_CHECK)) {
           continue;
       }
       int checkArray = INVALID_SREG;
       int checkIndex = INVALID_SREG;
       switch (mir->dalvikInsn.opcode) {
            case Instruction::AGET:
            case Instruction::AGET_OBJECT:
            case Instruction::AGET_BOOLEAN:
            case Instruction::AGET_BYTE:
            case Instruction::AGET_CHAR:
            case Instruction::AGET_SHORT:
            case Instruction::AGET_WIDE:
                checkArray = mir->ssaRep->uses[0];
                checkIndex = mir->ssaRep->uses[1];
                break;
                break;
            case Instruction::APUT:
            case Instruction::APUT_OBJECT:
            case Instruction::APUT_SHORT:
            case Instruction::APUT_CHAR:
            case Instruction::APUT_BYTE:
            case Instruction::APUT_BOOLEAN:
                checkArray = mir->ssaRep->uses[1];
                checkIndex = mir->ssaRep->uses[2];
                break;
            case Instruction::APUT_WIDE:
                checkArray = mir->ssaRep->uses[2];
                checkIndex = mir->ssaRep->uses[3];
            default:
                break;
       }
       if (checkArray == INVALID_SREG) {
           continue;
       }
       if ((arraySreg == checkArray) && (indexSreg == checkIndex)) {
           if (cUnit->printMe) {
               LOG(INFO) << "Squashing range check @ 0x" << std::hex
                         << mir->offset;
           }
           mir->optimizationFlags |= MIR_IGNORE_RANGE_CHECK;
       }
    }
}

/* Allocate a compiler temp, return Sreg.  Reuse existing if no conflict */
int allocCompilerTempSreg(CompilationUnit* cUnit, ArenaBitVector* bv)
{
    for (int i = 0; i < cUnit->numCompilerTemps; i++) {
        CompilerTemp* ct = (CompilerTemp*)cUnit->compilerTemps.elemList[i];
        ArenaBitVector* tBv = ct->bv;
        if (!oatTestBitVectors(bv, tBv)) {
            // Combine live maps and reuse existing temp
            oatUnifyBitVectors(tBv, tBv, bv);
            return ct->sReg;
        }
    }

    // Create a new compiler temp & associated live bitmap
    CompilerTemp* ct = (CompilerTemp*)oatNew(cUnit, sizeof(CompilerTemp),
                                             true, kAllocMisc);
    ArenaBitVector *nBv = oatAllocBitVector(cUnit, cUnit->numBlocks, true,
                                            kBitMapMisc);
    oatCopyBitVector(nBv, bv);
    ct->bv = nBv;
    ct->sReg = addNewSReg(cUnit, SSA_CTEMP_BASEREG - cUnit->numCompilerTemps);
    cUnit->numCompilerTemps++;
    oatInsertGrowableList(cUnit, &cUnit->compilerTemps, (intptr_t)ct);
    DCHECK_EQ(cUnit->numCompilerTemps, (int)cUnit->compilerTemps.numUsed);
    return ct->sReg;
}

/* Creata a new MIR node for a new pseudo op. */
MIR* rawMIR(CompilationUnit* cUnit, Instruction::Code opcode, int defs, int uses)
{
    MIR* res = (MIR*)oatNew( cUnit, sizeof(MIR), true, kAllocMIR);
    res->ssaRep =(struct SSARepresentation *)
            oatNew(cUnit, sizeof(SSARepresentation), true, kAllocDFInfo);
    if (uses) {
        res->ssaRep->numUses = uses;
        res->ssaRep->uses = (int*)oatNew(cUnit, sizeof(int) * uses, false, kAllocDFInfo);
    }
    if (defs) {
        res->ssaRep->numDefs = defs;
        res->ssaRep->defs = (int*)oatNew(cUnit, sizeof(int) * defs, false, kAllocDFInfo);
        res->ssaRep->fpDef = (bool*)oatNew(cUnit, sizeof(bool) * defs, true, kAllocDFInfo);
    }
    res->dalvikInsn.opcode = opcode;
    return res;
}

/* Do some MIR-level basic block optimizations */
bool basicBlockOpt(CompilationUnit* cUnit, BasicBlock* bb)
{
    int numTemps = 0;

    for (MIR* mir = bb->firstMIRInsn; mir; mir = mir->next) {
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
                if (!(mir->optimizationFlags & MIR_IGNORE_RANGE_CHECK)) {
                    int arrSreg = mir->ssaRep->uses[0];
                    int idxSreg = mir->ssaRep->uses[1];
                    BasicBlock* tbb = bb;
                    squashDupRangeChecks(cUnit, &tbb, mir, arrSreg, idxSreg);
                }
                break;
            case Instruction::APUT:
            case Instruction::APUT_OBJECT:
            case Instruction::APUT_SHORT:
            case Instruction::APUT_CHAR:
            case Instruction::APUT_BYTE:
            case Instruction::APUT_BOOLEAN:
            case Instruction::APUT_WIDE:
                if (!(mir->optimizationFlags & MIR_IGNORE_RANGE_CHECK)) {
                    int start = (opcode == Instruction::APUT_WIDE) ? 2 : 1;
                    int arrSreg = mir->ssaRep->uses[start];
                    int idxSreg = mir->ssaRep->uses[start + 1];
                    BasicBlock* tbb = bb;
                    squashDupRangeChecks(cUnit, &tbb, mir, arrSreg, idxSreg);
                }
                break;
            case Instruction::CMPL_FLOAT:
            case Instruction::CMPL_DOUBLE:
            case Instruction::CMPG_FLOAT:
            case Instruction::CMPG_DOUBLE:
            case Instruction::CMP_LONG:
                if (mir->next != NULL) {
                    MIR* mirNext = mir->next;
                    Instruction::Code brOpcode = mirNext->dalvikInsn.opcode;
                    ConditionCode ccode = kCondNv;
                    switch(brOpcode) {
                        case Instruction::IF_EQZ:
                            ccode = kCondEq;
                            break;
                        case Instruction::IF_NEZ:
                           // ccode = kCondNe;
                            break;
                        case Instruction::IF_LTZ:
                            // ccode = kCondLt;
                            break;
                        case Instruction::IF_GEZ:
                            // ccode = kCondGe;
                            break;
                        case Instruction::IF_GTZ:
                            // ccode = kCondGt;
                            break;
                        case Instruction::IF_LEZ:
                            // ccode = kCondLe;
                            break;
                        default:
                            break;
                    }
                    // Make sure result of cmp is used by next insn and nowhere else
                    if ((ccode != kCondNv) &&
                        (mir->ssaRep->defs[0] == mirNext->ssaRep->uses[0]) &&
                        (getSSAUseCount(cUnit, mir->ssaRep->defs[0]) == 1)) {
                        mirNext->dalvikInsn.arg[0] = ccode;
                        switch(opcode) {
                            case Instruction::CMPL_FLOAT:
                                mirNext->dalvikInsn.opcode =
                                    static_cast<Instruction::Code>(kMirOpFusedCmplFloat);
                                break;
                            case Instruction::CMPL_DOUBLE:
                                mirNext->dalvikInsn.opcode =
                                    static_cast<Instruction::Code>(kMirOpFusedCmplDouble);
                                break;
                            case Instruction::CMPG_FLOAT:
                                mirNext->dalvikInsn.opcode =
                                    static_cast<Instruction::Code>(kMirOpFusedCmpgFloat);
                                break;
                            case Instruction::CMPG_DOUBLE:
                                mirNext->dalvikInsn.opcode =
                                    static_cast<Instruction::Code>(kMirOpFusedCmpgDouble);
                                break;
                            case Instruction::CMP_LONG:
                                mirNext->dalvikInsn.opcode =
                                    static_cast<Instruction::Code>(kMirOpFusedCmpLong);
                                break;
                            default: LOG(ERROR) << "Unexpected opcode: " << (int)opcode;
                        }
                        mir->dalvikInsn.opcode = static_cast<Instruction::Code>(kMirOpNop);
                        mirNext->ssaRep->numUses = mir->ssaRep->numUses;
                        mirNext->ssaRep->uses = mir->ssaRep->uses;
                        mirNext->ssaRep->fpUse = mir->ssaRep->fpUse;
                        mirNext->ssaRep->numDefs = 0;
                        mir->ssaRep->numUses = 0;
                        mir->ssaRep->numDefs = 0;
                    }
                }
                break;
            default:
                break;
        }
    }

    if (numTemps > cUnit->numCompilerTemps) {
        cUnit->numCompilerTemps = numTemps;
    }
    return true;
}

bool nullCheckEliminationInit(struct CompilationUnit* cUnit,
                              struct BasicBlock* bb)
{
    if (bb->dataFlowInfo == NULL) return false;
    bb->dataFlowInfo->endingNullCheckV =
        oatAllocBitVector(cUnit, cUnit->numSSARegs, false, kBitMapNullCheck);
    oatClearAllBits(bb->dataFlowInfo->endingNullCheckV);
    return true;
}

/* Eliminate unnecessary null checks for a basic block. */
bool eliminateNullChecks( struct CompilationUnit* cUnit, struct BasicBlock* bb)
{
    if (bb->dataFlowInfo == NULL) return false;

    /*
     * Set initial state.  Be conservative with catch
     * blocks and start with no assumptions about null check
     * status (except for "this").
     */
    if ((bb->blockType == kEntryBlock) | bb->catchEntry) {
        oatClearAllBits(cUnit->tempSSARegisterV);
        if ((cUnit->access_flags & kAccStatic) == 0) {
            // If non-static method, mark "this" as non-null
            int thisReg = cUnit->numDalvikRegisters - cUnit->numIns;
            oatSetBit(cUnit, cUnit->tempSSARegisterV, thisReg);
        }
    } else {
        // Starting state is intesection of all incoming arcs
        GrowableListIterator iter;
        oatGrowableListIteratorInit(bb->predecessors, &iter);
        BasicBlock* predBB = (BasicBlock*)oatGrowableListIteratorNext(&iter);
        DCHECK(predBB != NULL);
        oatCopyBitVector(cUnit->tempSSARegisterV,
                         predBB->dataFlowInfo->endingNullCheckV);
        while (true) {
            predBB = (BasicBlock*)oatGrowableListIteratorNext(&iter);
            if (!predBB) break;
            if ((predBB->dataFlowInfo == NULL) ||
                (predBB->dataFlowInfo->endingNullCheckV == NULL)) {
                continue;
            }
            oatIntersectBitVectors(cUnit->tempSSARegisterV,
                cUnit->tempSSARegisterV,
                predBB->dataFlowInfo->endingNullCheckV);
        }
    }

    // Walk through the instruction in the block, updating as necessary
    for (MIR* mir = bb->firstMIRInsn; mir; mir = mir->next) {
        if (mir->ssaRep == NULL) {
            continue;
        }
        int dfAttributes =
            oatDataFlowAttributes[mir->dalvikInsn.opcode];

        // Mark target of NEW* as non-null
        if (dfAttributes & DF_NON_NULL_DST) {
            oatSetBit(cUnit, cUnit->tempSSARegisterV, mir->ssaRep->defs[0]);
        }

        // Mark non-null returns from invoke-style NEW*
        if (dfAttributes & DF_NON_NULL_RET) {
            MIR* nextMir = mir->next;
            // Next should be an MOVE_RESULT_OBJECT
            if (nextMir && nextMir->dalvikInsn.opcode == Instruction::MOVE_RESULT_OBJECT) {
                // Mark as null checked
                oatSetBit(cUnit, cUnit->tempSSARegisterV,
                          nextMir->ssaRep->defs[0]);
            } else {
                if (nextMir) {
                    LOG(WARNING) << "Unexpected opcode following new: " <<
                    (int)nextMir->dalvikInsn.opcode;
                } else if (bb->fallThrough) {
                    // Look in next basic block
                    struct BasicBlock* nextBB = bb->fallThrough;
                    for (MIR* tmir = nextBB->firstMIRInsn; tmir;
                         tmir =tmir->next) {
                       if ((int)tmir->dalvikInsn.opcode >= (int)kMirOpFirst) {
                           continue;
                       }
                       // First non-pseudo should be MOVE_RESULT_OBJECT
                       if (tmir->dalvikInsn.opcode == Instruction::MOVE_RESULT_OBJECT) {
                           // Mark as null checked
                           oatSetBit(cUnit, cUnit->tempSSARegisterV,
                                     tmir->ssaRep->defs[0]);
                       } else {
                           LOG(WARNING) << "Unexpected op after new: " <<
                               (int)tmir->dalvikInsn.opcode;
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
        if (dfAttributes & (DF_NULL_TRANSFER_0 | DF_NULL_TRANSFER_N)) {
            int tgtSreg = mir->ssaRep->defs[0];
            int operands = (dfAttributes & DF_NULL_TRANSFER_0) ? 1 :
                mir->ssaRep->numUses;
            bool nullChecked = true;
            for (int i = 0; i < operands; i++) {
                nullChecked &= oatIsBitSet(cUnit->tempSSARegisterV,
                    mir->ssaRep->uses[i]);
            }
            if (nullChecked) {
                oatSetBit(cUnit, cUnit->tempSSARegisterV, tgtSreg);
            }
        }

        // Already nullchecked?
        if (dfAttributes & DF_HAS_NULL_CHKS) {
            int srcIdx;
            if (dfAttributes & DF_NULL_CHK_1) {
                srcIdx = 1;
            } else if (dfAttributes & DF_NULL_CHK_2) {
                srcIdx = 2;
            } else {
                srcIdx = 0;
            }
            int srcSreg = mir->ssaRep->uses[srcIdx];
            if (oatIsBitSet(cUnit->tempSSARegisterV, srcSreg)) {
                // Eliminate the null check
                mir->optimizationFlags |= MIR_IGNORE_NULL_CHECK;
            } else {
                // Mark sReg as null-checked
                oatSetBit(cUnit, cUnit->tempSSARegisterV, srcSreg);
            }
        }
    }

    // Did anything change?
    bool res = oatCompareBitVectors(bb->dataFlowInfo->endingNullCheckV,
                                    cUnit->tempSSARegisterV);
    if (res) {
        oatCopyBitVector(bb->dataFlowInfo->endingNullCheckV,
                         cUnit->tempSSARegisterV);
    }
    return res;
}

void oatMethodNullCheckElimination(CompilationUnit *cUnit)
{
    if (!(cUnit->disableOpt & (1 << kNullCheckElimination))) {
        DCHECK(cUnit->tempSSARegisterV != NULL);
        oatDataFlowAnalysisDispatcher(cUnit, nullCheckEliminationInit,
                                      kAllNodes,
                                      false /* isIterative */);
        oatDataFlowAnalysisDispatcher(cUnit, eliminateNullChecks,
                                      kPreOrderDFSTraversal,
                                      true /* isIterative */);
    }
}

void oatMethodBasicBlockOptimization(CompilationUnit *cUnit)
{
    if (!(cUnit->disableOpt & (1 << kBBOpt))) {
        oatInitGrowableList(cUnit, &cUnit->compilerTemps, 6, kListMisc);
        DCHECK_EQ(cUnit->numCompilerTemps, 0);
        if (!(cUnit->disableOpt & (1 << kBBOpt))) {
            oatDataFlowAnalysisDispatcher(cUnit, basicBlockOpt,
                                          kAllNodes, false /* isIterative */);
        }
    }
}

void addLoopHeader(CompilationUnit* cUnit, BasicBlock* header,
                   BasicBlock* backEdge)
{
    GrowableListIterator iter;
    oatGrowableListIteratorInit(&cUnit->loopHeaders, &iter);
    for (LoopInfo* loop = (LoopInfo*)oatGrowableListIteratorNext(&iter);
         (loop != NULL); loop = (LoopInfo*)oatGrowableListIteratorNext(&iter)) {
         if (loop->header == header) {
             oatInsertGrowableList(cUnit, &loop->incomingBackEdges,
                                   (intptr_t)backEdge);
             return;
         }
    }
    LoopInfo* info = (LoopInfo*)oatNew(cUnit, sizeof(LoopInfo), true,
                                       kAllocDFInfo);
    info->header = header;
    oatInitGrowableList(cUnit, &info->incomingBackEdges, 2, kListMisc);
    oatInsertGrowableList(cUnit, &info->incomingBackEdges, (intptr_t)backEdge);
    oatInsertGrowableList(cUnit, &cUnit->loopHeaders, (intptr_t)info);
}

bool findBackEdges(struct CompilationUnit* cUnit, struct BasicBlock* bb)
{
    if ((bb->dataFlowInfo == NULL) || (bb->lastMIRInsn == NULL)) {
        return false;
    }
    Instruction::Code opcode = bb->lastMIRInsn->dalvikInsn.opcode;
    if (Instruction::Flags(opcode) & Instruction::kBranch) {
        if (bb->taken && (bb->taken->startOffset <= bb->startOffset)) {
            DCHECK(bb->dominators != NULL);
            if (oatIsBitSet(bb->dominators, bb->taken->id)) {
                if (cUnit->printMe) {
                    LOG(INFO) << "Loop backedge from 0x"
                              << std::hex << bb->lastMIRInsn->offset
                              << " to 0x" << std::hex << bb->taken->startOffset;
                }
                addLoopHeader(cUnit, bb->taken, bb);
            }
        }
    }
    return false;
}

void addBlocksToLoop(CompilationUnit* cUnit, ArenaBitVector* blocks,
                     BasicBlock* bb, int headId)
{
    if (!oatIsBitSet(bb->dominators, headId) ||
        oatIsBitSet(blocks, bb->id)) {
        return;
    }
    oatSetBit(cUnit, blocks, bb->id);
    GrowableListIterator iter;
    oatGrowableListIteratorInit(bb->predecessors, &iter);
    BasicBlock* predBB;
    for (predBB = (BasicBlock*)oatGrowableListIteratorNext(&iter); predBB;
         predBB = (BasicBlock*)oatGrowableListIteratorNext(&iter)) {
             addBlocksToLoop(cUnit, blocks, predBB, headId);
    }
}

void oatDumpLoops(CompilationUnit *cUnit)
{
    GrowableListIterator iter;
    oatGrowableListIteratorInit(&cUnit->loopHeaders, &iter);
    for (LoopInfo* loop = (LoopInfo*)oatGrowableListIteratorNext(&iter);
         (loop != NULL); loop = (LoopInfo*)oatGrowableListIteratorNext(&iter)) {
         LOG(INFO) << "Loop head block id " << loop->header->id
                   << ", offset 0x" << std::hex << loop->header->startOffset
                   << ", Depth: " << loop->header->nestingDepth;
         GrowableListIterator iter;
         oatGrowableListIteratorInit(&loop->incomingBackEdges, &iter);
         BasicBlock* edgeBB;
         for (edgeBB = (BasicBlock*)oatGrowableListIteratorNext(&iter); edgeBB;
              edgeBB = (BasicBlock*)oatGrowableListIteratorNext(&iter)) {
              LOG(INFO) << "    Backedge block id " << edgeBB->id
                        << ", offset 0x" << std::hex << edgeBB->startOffset;
              ArenaBitVectorIterator bIter;
              oatBitVectorIteratorInit(loop->blocks, &bIter);
              for (int bbId = oatBitVectorIteratorNext(&bIter); bbId != -1;
                   bbId = oatBitVectorIteratorNext(&bIter)) {
                  BasicBlock *bb;
                  bb = (BasicBlock*)
                        oatGrowableListGetElement(&cUnit->blockList, bbId);
                  LOG(INFO) << "        (" << bb->id << ", 0x" << std::hex
                            << bb->startOffset << ")";
              }
         }
    }
}

void oatMethodLoopDetection(CompilationUnit *cUnit)
{
    if (cUnit->disableOpt & (1 << kPromoteRegs)) {
        return;
    }
    oatInitGrowableList(cUnit, &cUnit->loopHeaders, 6, kListMisc);
    // Find the loop headers
    oatDataFlowAnalysisDispatcher(cUnit, findBackEdges,
                                  kAllNodes, false /* isIterative */);
    GrowableListIterator iter;
    oatGrowableListIteratorInit(&cUnit->loopHeaders, &iter);
    // Add blocks to each header
    for (LoopInfo* loop = (LoopInfo*)oatGrowableListIteratorNext(&iter);
         loop; loop = (LoopInfo*)oatGrowableListIteratorNext(&iter)) {
         loop->blocks = oatAllocBitVector(cUnit, cUnit->numBlocks, true,
                                          kBitMapMisc);
         oatSetBit(cUnit, loop->blocks, loop->header->id);
         GrowableListIterator iter;
         oatGrowableListIteratorInit(&loop->incomingBackEdges, &iter);
         BasicBlock* edgeBB;
         for (edgeBB = (BasicBlock*)oatGrowableListIteratorNext(&iter); edgeBB;
              edgeBB = (BasicBlock*)oatGrowableListIteratorNext(&iter)) {
             addBlocksToLoop(cUnit, loop->blocks, edgeBB, loop->header->id);
         }
    }
    // Compute the nesting depth of each header
    oatGrowableListIteratorInit(&cUnit->loopHeaders, &iter);
    for (LoopInfo* loop = (LoopInfo*)oatGrowableListIteratorNext(&iter);
         loop; loop = (LoopInfo*)oatGrowableListIteratorNext(&iter)) {
        GrowableListIterator iter2;
        oatGrowableListIteratorInit(&cUnit->loopHeaders, &iter2);
        LoopInfo* loop2;
        for (loop2 = (LoopInfo*)oatGrowableListIteratorNext(&iter2);
             loop2; loop2 = (LoopInfo*)oatGrowableListIteratorNext(&iter2)) {
            if (oatIsBitSet(loop2->blocks, loop->header->id)) {
                loop->header->nestingDepth++;
            }
        }
    }
    // Assign nesting depth to each block in all loops
    oatGrowableListIteratorInit(&cUnit->loopHeaders, &iter);
    for (LoopInfo* loop = (LoopInfo*)oatGrowableListIteratorNext(&iter);
         (loop != NULL); loop = (LoopInfo*)oatGrowableListIteratorNext(&iter)) {
        ArenaBitVectorIterator bIter;
        oatBitVectorIteratorInit(loop->blocks, &bIter);
        for (int bbId = oatBitVectorIteratorNext(&bIter); bbId != -1;
            bbId = oatBitVectorIteratorNext(&bIter)) {
            BasicBlock *bb;
            bb = (BasicBlock*) oatGrowableListGetElement(&cUnit->blockList,
                                                         bbId);
            bb->nestingDepth = std::max(bb->nestingDepth,
                                        loop->header->nestingDepth);
        }
    }
    if (cUnit->printMe) {
        oatDumpLoops(cUnit);
    }
}

/*
 * This function will make a best guess at whether the invoke will
 * end up using Method*.  It isn't critical to get it exactly right,
 * and attempting to do would involve more complexity than it's
 * worth.
 */
bool invokeUsesMethodStar(CompilationUnit* cUnit, MIR* mir)
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
            LOG(WARNING) << "Unexpected invoke op: " << (int)opcode;
            return false;
    }
    OatCompilationUnit mUnit(cUnit->class_loader, cUnit->class_linker,
                             *cUnit->dex_file, *cUnit->dex_cache,
                             cUnit->code_item, cUnit->method_idx,
                             cUnit->access_flags);
    // TODO: add a flag so we don't counts the stats for this twice
    uint32_t dexMethodIdx = mir->dalvikInsn.vB;
    int vtableIdx;
    uintptr_t directCode;
    uintptr_t directMethod;
    bool fastPath =
        cUnit->compiler->ComputeInvokeInfo(dexMethodIdx, &mUnit, type,
                                           vtableIdx, directCode,
                                           directMethod) &&
        !SLOW_INVOKE_PATH;
    return (((type == kDirect) || (type == kStatic)) &&
            fastPath && ((directCode == 0) || (directMethod == 0)));
}

/*
 * Count uses, weighting by loop nesting depth.  This code only
 * counts explicitly used sRegs.  A later phase will add implicit
 * counts for things such as Method*, null-checked references, etc.
 */
bool countUses(struct CompilationUnit* cUnit, struct BasicBlock* bb)
{
    if (bb->blockType != kDalvikByteCode) {
        return false;
    }
    for (MIR* mir = bb->firstMIRInsn; (mir != NULL); mir = mir->next) {
        if (mir->ssaRep == NULL) {
            continue;
        }
        uint32_t weight = std::min(16U, (uint32_t)bb->nestingDepth);
        for (int i = 0; i < mir->ssaRep->numUses; i++) {
            int sReg = mir->ssaRep->uses[i];
            DCHECK_LT(sReg, (int)cUnit->useCounts.numUsed);
            cUnit->rawUseCounts.elemList[sReg]++;
            cUnit->useCounts.elemList[sReg] += (1 << weight);
        }
        if (!(cUnit->disableOpt & (1 << kPromoteCompilerTemps))) {
            int dfAttributes = oatDataFlowAttributes[mir->dalvikInsn.opcode];
            // Implicit use of Method* ? */
            if (dfAttributes & DF_UMS) {
                /*
                 * Some invokes will not use Method* - need to perform test similar
                 * to that found in genInvoke() to decide whether to count refs
                 * for Method* on invoke-class opcodes.
                 * TODO: refactor for common test here, save results for genInvoke
                 */
                int usesMethodStar = true;
                if ((dfAttributes & (DF_FORMAT_35C | DF_FORMAT_3RC)) &&
                    !(dfAttributes & DF_NON_NULL_RET)) {
                    usesMethodStar &= invokeUsesMethodStar(cUnit, mir);
                }
                if (usesMethodStar) {
                    cUnit->rawUseCounts.elemList[cUnit->methodSReg]++;
                    cUnit->useCounts.elemList[cUnit->methodSReg] += (1 << weight);
                }
            }
        }
    }
    return false;
}

void oatMethodUseCount(CompilationUnit *cUnit)
{
    oatInitGrowableList(cUnit, &cUnit->useCounts, cUnit->numSSARegs + 32,
                        kListMisc);
    oatInitGrowableList(cUnit, &cUnit->rawUseCounts, cUnit->numSSARegs + 32,
                        kListMisc);
    // Initialize list
    for (int i = 0; i < cUnit->numSSARegs; i++) {
        oatInsertGrowableList(cUnit, &cUnit->useCounts, 0);
        oatInsertGrowableList(cUnit, &cUnit->rawUseCounts, 0);
    }
    if (cUnit->disableOpt & (1 << kPromoteRegs)) {
        return;
    }
    oatDataFlowAnalysisDispatcher(cUnit, countUses,
                                  kAllNodes, false /* isIterative */);
}

}  // namespace art
