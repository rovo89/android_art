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
#include <memory>

#include "base/logging.h"
#include "base/scoped_arena_containers.h"
#include "dataflow_iterator-inl.h"
#include "compiler_ir.h"
#include "dex_flags.h"
#include "dex_instruction-inl.h"
#include "dex/mir_field_info.h"
#include "dex/verified_method.h"
#include "dex/quick/dex_file_method_inliner.h"
#include "dex/quick/dex_file_to_method_inliner_map.h"
#include "driver/compiler_driver.h"
#include "driver/compiler_options.h"
#include "driver/dex_compilation_unit.h"
#include "utils.h"

namespace art {

enum InstructionAnalysisAttributeOps : uint8_t {
  kUninterestingOp = 0,
  kArithmeticOp,
  kFpOp,
  kSingleOp,
  kDoubleOp,
  kIntOp,
  kLongOp,
  kBranchOp,
  kInvokeOp,
  kArrayOp,
  kHeavyweightOp,
  kSimpleConstOp,
  kMoveOp,
  kSwitch
};

enum InstructionAnalysisAttributeMasks : uint16_t {
  kAnNone = 1 << kUninterestingOp,
  kAnMath = 1 << kArithmeticOp,
  kAnFp = 1 << kFpOp,
  kAnLong = 1 << kLongOp,
  kAnInt = 1 << kIntOp,
  kAnSingle = 1 << kSingleOp,
  kAnDouble = 1 << kDoubleOp,
  kAnFloatMath = 1 << kFpOp,
  kAnBranch = 1 << kBranchOp,
  kAnInvoke = 1 << kInvokeOp,
  kAnArrayOp = 1 << kArrayOp,
  kAnHeavyWeight = 1 << kHeavyweightOp,
  kAnSimpleConst = 1 << kSimpleConstOp,
  kAnMove = 1 << kMoveOp,
  kAnSwitch = 1 << kSwitch,
  kAnComputational = kAnMath | kAnArrayOp | kAnMove | kAnSimpleConst,
};

// Instruction characteristics used to statically identify computation-intensive methods.
static const uint16_t kAnalysisAttributes[kMirOpLast] = {
  // 00 NOP
  kAnNone,

  // 01 MOVE vA, vB
  kAnMove,

  // 02 MOVE_FROM16 vAA, vBBBB
  kAnMove,

  // 03 MOVE_16 vAAAA, vBBBB
  kAnMove,

  // 04 MOVE_WIDE vA, vB
  kAnMove,

  // 05 MOVE_WIDE_FROM16 vAA, vBBBB
  kAnMove,

  // 06 MOVE_WIDE_16 vAAAA, vBBBB
  kAnMove,

  // 07 MOVE_OBJECT vA, vB
  kAnMove,

  // 08 MOVE_OBJECT_FROM16 vAA, vBBBB
  kAnMove,

  // 09 MOVE_OBJECT_16 vAAAA, vBBBB
  kAnMove,

  // 0A MOVE_RESULT vAA
  kAnMove,

  // 0B MOVE_RESULT_WIDE vAA
  kAnMove,

  // 0C MOVE_RESULT_OBJECT vAA
  kAnMove,

  // 0D MOVE_EXCEPTION vAA
  kAnMove,

  // 0E RETURN_VOID
  kAnBranch,

  // 0F RETURN vAA
  kAnBranch,

  // 10 RETURN_WIDE vAA
  kAnBranch,

  // 11 RETURN_OBJECT vAA
  kAnBranch,

  // 12 CONST_4 vA, #+B
  kAnSimpleConst,

  // 13 CONST_16 vAA, #+BBBB
  kAnSimpleConst,

  // 14 CONST vAA, #+BBBBBBBB
  kAnSimpleConst,

  // 15 CONST_HIGH16 VAA, #+BBBB0000
  kAnSimpleConst,

  // 16 CONST_WIDE_16 vAA, #+BBBB
  kAnSimpleConst,

  // 17 CONST_WIDE_32 vAA, #+BBBBBBBB
  kAnSimpleConst,

  // 18 CONST_WIDE vAA, #+BBBBBBBBBBBBBBBB
  kAnSimpleConst,

  // 19 CONST_WIDE_HIGH16 vAA, #+BBBB000000000000
  kAnSimpleConst,

  // 1A CONST_STRING vAA, string@BBBB
  kAnNone,

  // 1B CONST_STRING_JUMBO vAA, string@BBBBBBBB
  kAnNone,

  // 1C CONST_CLASS vAA, type@BBBB
  kAnNone,

  // 1D MONITOR_ENTER vAA
  kAnNone,

  // 1E MONITOR_EXIT vAA
  kAnNone,

  // 1F CHK_CAST vAA, type@BBBB
  kAnNone,

  // 20 INSTANCE_OF vA, vB, type@CCCC
  kAnNone,

  // 21 ARRAY_LENGTH vA, vB
  kAnArrayOp,

  // 22 NEW_INSTANCE vAA, type@BBBB
  kAnHeavyWeight,

  // 23 NEW_ARRAY vA, vB, type@CCCC
  kAnHeavyWeight,

  // 24 FILLED_NEW_ARRAY {vD, vE, vF, vG, vA}
  kAnHeavyWeight,

  // 25 FILLED_NEW_ARRAY_RANGE {vCCCC .. vNNNN}, type@BBBB
  kAnHeavyWeight,

  // 26 FILL_ARRAY_DATA vAA, +BBBBBBBB
  kAnNone,

  // 27 THROW vAA
  kAnHeavyWeight | kAnBranch,

  // 28 GOTO
  kAnBranch,

  // 29 GOTO_16
  kAnBranch,

  // 2A GOTO_32
  kAnBranch,

  // 2B PACKED_SWITCH vAA, +BBBBBBBB
  kAnSwitch,

  // 2C SPARSE_SWITCH vAA, +BBBBBBBB
  kAnSwitch,

  // 2D CMPL_FLOAT vAA, vBB, vCC
  kAnMath | kAnFp | kAnSingle,

  // 2E CMPG_FLOAT vAA, vBB, vCC
  kAnMath | kAnFp | kAnSingle,

  // 2F CMPL_DOUBLE vAA, vBB, vCC
  kAnMath | kAnFp | kAnDouble,

  // 30 CMPG_DOUBLE vAA, vBB, vCC
  kAnMath | kAnFp | kAnDouble,

  // 31 CMP_LONG vAA, vBB, vCC
  kAnMath | kAnLong,

  // 32 IF_EQ vA, vB, +CCCC
  kAnMath | kAnBranch | kAnInt,

  // 33 IF_NE vA, vB, +CCCC
  kAnMath | kAnBranch | kAnInt,

  // 34 IF_LT vA, vB, +CCCC
  kAnMath | kAnBranch | kAnInt,

  // 35 IF_GE vA, vB, +CCCC
  kAnMath | kAnBranch | kAnInt,

  // 36 IF_GT vA, vB, +CCCC
  kAnMath | kAnBranch | kAnInt,

  // 37 IF_LE vA, vB, +CCCC
  kAnMath | kAnBranch | kAnInt,

  // 38 IF_EQZ vAA, +BBBB
  kAnMath | kAnBranch | kAnInt,

  // 39 IF_NEZ vAA, +BBBB
  kAnMath | kAnBranch | kAnInt,

  // 3A IF_LTZ vAA, +BBBB
  kAnMath | kAnBranch | kAnInt,

  // 3B IF_GEZ vAA, +BBBB
  kAnMath | kAnBranch | kAnInt,

  // 3C IF_GTZ vAA, +BBBB
  kAnMath | kAnBranch | kAnInt,

  // 3D IF_LEZ vAA, +BBBB
  kAnMath | kAnBranch | kAnInt,

  // 3E UNUSED_3E
  kAnNone,

  // 3F UNUSED_3F
  kAnNone,

  // 40 UNUSED_40
  kAnNone,

  // 41 UNUSED_41
  kAnNone,

  // 42 UNUSED_42
  kAnNone,

  // 43 UNUSED_43
  kAnNone,

  // 44 AGET vAA, vBB, vCC
  kAnArrayOp,

  // 45 AGET_WIDE vAA, vBB, vCC
  kAnArrayOp,

  // 46 AGET_OBJECT vAA, vBB, vCC
  kAnArrayOp,

  // 47 AGET_BOOLEAN vAA, vBB, vCC
  kAnArrayOp,

  // 48 AGET_BYTE vAA, vBB, vCC
  kAnArrayOp,

  // 49 AGET_CHAR vAA, vBB, vCC
  kAnArrayOp,

  // 4A AGET_SHORT vAA, vBB, vCC
  kAnArrayOp,

  // 4B APUT vAA, vBB, vCC
  kAnArrayOp,

  // 4C APUT_WIDE vAA, vBB, vCC
  kAnArrayOp,

  // 4D APUT_OBJECT vAA, vBB, vCC
  kAnArrayOp,

  // 4E APUT_BOOLEAN vAA, vBB, vCC
  kAnArrayOp,

  // 4F APUT_BYTE vAA, vBB, vCC
  kAnArrayOp,

  // 50 APUT_CHAR vAA, vBB, vCC
  kAnArrayOp,

  // 51 APUT_SHORT vAA, vBB, vCC
  kAnArrayOp,

  // 52 IGET vA, vB, field@CCCC
  kAnNone,

  // 53 IGET_WIDE vA, vB, field@CCCC
  kAnNone,

  // 54 IGET_OBJECT vA, vB, field@CCCC
  kAnNone,

  // 55 IGET_BOOLEAN vA, vB, field@CCCC
  kAnNone,

  // 56 IGET_BYTE vA, vB, field@CCCC
  kAnNone,

  // 57 IGET_CHAR vA, vB, field@CCCC
  kAnNone,

  // 58 IGET_SHORT vA, vB, field@CCCC
  kAnNone,

  // 59 IPUT vA, vB, field@CCCC
  kAnNone,

  // 5A IPUT_WIDE vA, vB, field@CCCC
  kAnNone,

  // 5B IPUT_OBJECT vA, vB, field@CCCC
  kAnNone,

  // 5C IPUT_BOOLEAN vA, vB, field@CCCC
  kAnNone,

  // 5D IPUT_BYTE vA, vB, field@CCCC
  kAnNone,

  // 5E IPUT_CHAR vA, vB, field@CCCC
  kAnNone,

  // 5F IPUT_SHORT vA, vB, field@CCCC
  kAnNone,

  // 60 SGET vAA, field@BBBB
  kAnNone,

  // 61 SGET_WIDE vAA, field@BBBB
  kAnNone,

  // 62 SGET_OBJECT vAA, field@BBBB
  kAnNone,

  // 63 SGET_BOOLEAN vAA, field@BBBB
  kAnNone,

  // 64 SGET_BYTE vAA, field@BBBB
  kAnNone,

  // 65 SGET_CHAR vAA, field@BBBB
  kAnNone,

  // 66 SGET_SHORT vAA, field@BBBB
  kAnNone,

  // 67 SPUT vAA, field@BBBB
  kAnNone,

  // 68 SPUT_WIDE vAA, field@BBBB
  kAnNone,

  // 69 SPUT_OBJECT vAA, field@BBBB
  kAnNone,

  // 6A SPUT_BOOLEAN vAA, field@BBBB
  kAnNone,

  // 6B SPUT_BYTE vAA, field@BBBB
  kAnNone,

  // 6C SPUT_CHAR vAA, field@BBBB
  kAnNone,

  // 6D SPUT_SHORT vAA, field@BBBB
  kAnNone,

  // 6E INVOKE_VIRTUAL {vD, vE, vF, vG, vA}
  kAnInvoke | kAnHeavyWeight,

  // 6F INVOKE_SUPER {vD, vE, vF, vG, vA}
  kAnInvoke | kAnHeavyWeight,

  // 70 INVOKE_DIRECT {vD, vE, vF, vG, vA}
  kAnInvoke | kAnHeavyWeight,

  // 71 INVOKE_STATIC {vD, vE, vF, vG, vA}
  kAnInvoke | kAnHeavyWeight,

  // 72 INVOKE_INTERFACE {vD, vE, vF, vG, vA}
  kAnInvoke | kAnHeavyWeight,

  // 73 RETURN_VOID_NO_BARRIER
  kAnBranch,

  // 74 INVOKE_VIRTUAL_RANGE {vCCCC .. vNNNN}
  kAnInvoke | kAnHeavyWeight,

  // 75 INVOKE_SUPER_RANGE {vCCCC .. vNNNN}
  kAnInvoke | kAnHeavyWeight,

  // 76 INVOKE_DIRECT_RANGE {vCCCC .. vNNNN}
  kAnInvoke | kAnHeavyWeight,

  // 77 INVOKE_STATIC_RANGE {vCCCC .. vNNNN}
  kAnInvoke | kAnHeavyWeight,

  // 78 INVOKE_INTERFACE_RANGE {vCCCC .. vNNNN}
  kAnInvoke | kAnHeavyWeight,

  // 79 UNUSED_79
  kAnNone,

  // 7A UNUSED_7A
  kAnNone,

  // 7B NEG_INT vA, vB
  kAnMath | kAnInt,

  // 7C NOT_INT vA, vB
  kAnMath | kAnInt,

  // 7D NEG_LONG vA, vB
  kAnMath | kAnLong,

  // 7E NOT_LONG vA, vB
  kAnMath | kAnLong,

  // 7F NEG_FLOAT vA, vB
  kAnMath | kAnFp | kAnSingle,

  // 80 NEG_DOUBLE vA, vB
  kAnMath | kAnFp | kAnDouble,

  // 81 INT_TO_LONG vA, vB
  kAnMath | kAnInt | kAnLong,

  // 82 INT_TO_FLOAT vA, vB
  kAnMath | kAnFp | kAnInt | kAnSingle,

  // 83 INT_TO_DOUBLE vA, vB
  kAnMath | kAnFp | kAnInt | kAnDouble,

  // 84 LONG_TO_INT vA, vB
  kAnMath | kAnInt | kAnLong,

  // 85 LONG_TO_FLOAT vA, vB
  kAnMath | kAnFp | kAnLong | kAnSingle,

  // 86 LONG_TO_DOUBLE vA, vB
  kAnMath | kAnFp | kAnLong | kAnDouble,

  // 87 FLOAT_TO_INT vA, vB
  kAnMath | kAnFp | kAnInt | kAnSingle,

  // 88 FLOAT_TO_LONG vA, vB
  kAnMath | kAnFp | kAnLong | kAnSingle,

  // 89 FLOAT_TO_DOUBLE vA, vB
  kAnMath | kAnFp | kAnSingle | kAnDouble,

  // 8A DOUBLE_TO_INT vA, vB
  kAnMath | kAnFp | kAnInt | kAnDouble,

  // 8B DOUBLE_TO_LONG vA, vB
  kAnMath | kAnFp | kAnLong | kAnDouble,

  // 8C DOUBLE_TO_FLOAT vA, vB
  kAnMath | kAnFp | kAnSingle | kAnDouble,

  // 8D INT_TO_BYTE vA, vB
  kAnMath | kAnInt,

  // 8E INT_TO_CHAR vA, vB
  kAnMath | kAnInt,

  // 8F INT_TO_SHORT vA, vB
  kAnMath | kAnInt,

  // 90 ADD_INT vAA, vBB, vCC
  kAnMath | kAnInt,

  // 91 SUB_INT vAA, vBB, vCC
  kAnMath | kAnInt,

  // 92 MUL_INT vAA, vBB, vCC
  kAnMath | kAnInt,

  // 93 DIV_INT vAA, vBB, vCC
  kAnMath | kAnInt,

  // 94 REM_INT vAA, vBB, vCC
  kAnMath | kAnInt,

  // 95 AND_INT vAA, vBB, vCC
  kAnMath | kAnInt,

  // 96 OR_INT vAA, vBB, vCC
  kAnMath | kAnInt,

  // 97 XOR_INT vAA, vBB, vCC
  kAnMath | kAnInt,

  // 98 SHL_INT vAA, vBB, vCC
  kAnMath | kAnInt,

  // 99 SHR_INT vAA, vBB, vCC
  kAnMath | kAnInt,

  // 9A USHR_INT vAA, vBB, vCC
  kAnMath | kAnInt,

  // 9B ADD_LONG vAA, vBB, vCC
  kAnMath | kAnLong,

  // 9C SUB_LONG vAA, vBB, vCC
  kAnMath | kAnLong,

  // 9D MUL_LONG vAA, vBB, vCC
  kAnMath | kAnLong,

  // 9E DIV_LONG vAA, vBB, vCC
  kAnMath | kAnLong,

  // 9F REM_LONG vAA, vBB, vCC
  kAnMath | kAnLong,

  // A0 AND_LONG vAA, vBB, vCC
  kAnMath | kAnLong,

  // A1 OR_LONG vAA, vBB, vCC
  kAnMath | kAnLong,

  // A2 XOR_LONG vAA, vBB, vCC
  kAnMath | kAnLong,

  // A3 SHL_LONG vAA, vBB, vCC
  kAnMath | kAnLong,

  // A4 SHR_LONG vAA, vBB, vCC
  kAnMath | kAnLong,

  // A5 USHR_LONG vAA, vBB, vCC
  kAnMath | kAnLong,

  // A6 ADD_FLOAT vAA, vBB, vCC
  kAnMath | kAnFp | kAnSingle,

  // A7 SUB_FLOAT vAA, vBB, vCC
  kAnMath | kAnFp | kAnSingle,

  // A8 MUL_FLOAT vAA, vBB, vCC
  kAnMath | kAnFp | kAnSingle,

  // A9 DIV_FLOAT vAA, vBB, vCC
  kAnMath | kAnFp | kAnSingle,

  // AA REM_FLOAT vAA, vBB, vCC
  kAnMath | kAnFp | kAnSingle,

  // AB ADD_DOUBLE vAA, vBB, vCC
  kAnMath | kAnFp | kAnDouble,

  // AC SUB_DOUBLE vAA, vBB, vCC
  kAnMath | kAnFp | kAnDouble,

  // AD MUL_DOUBLE vAA, vBB, vCC
  kAnMath | kAnFp | kAnDouble,

  // AE DIV_DOUBLE vAA, vBB, vCC
  kAnMath | kAnFp | kAnDouble,

  // AF REM_DOUBLE vAA, vBB, vCC
  kAnMath | kAnFp | kAnDouble,

  // B0 ADD_INT_2ADDR vA, vB
  kAnMath | kAnInt,

  // B1 SUB_INT_2ADDR vA, vB
  kAnMath | kAnInt,

  // B2 MUL_INT_2ADDR vA, vB
  kAnMath | kAnInt,

  // B3 DIV_INT_2ADDR vA, vB
  kAnMath | kAnInt,

  // B4 REM_INT_2ADDR vA, vB
  kAnMath | kAnInt,

  // B5 AND_INT_2ADDR vA, vB
  kAnMath | kAnInt,

  // B6 OR_INT_2ADDR vA, vB
  kAnMath | kAnInt,

  // B7 XOR_INT_2ADDR vA, vB
  kAnMath | kAnInt,

  // B8 SHL_INT_2ADDR vA, vB
  kAnMath | kAnInt,

  // B9 SHR_INT_2ADDR vA, vB
  kAnMath | kAnInt,

  // BA USHR_INT_2ADDR vA, vB
  kAnMath | kAnInt,

  // BB ADD_LONG_2ADDR vA, vB
  kAnMath | kAnLong,

  // BC SUB_LONG_2ADDR vA, vB
  kAnMath | kAnLong,

  // BD MUL_LONG_2ADDR vA, vB
  kAnMath | kAnLong,

  // BE DIV_LONG_2ADDR vA, vB
  kAnMath | kAnLong,

  // BF REM_LONG_2ADDR vA, vB
  kAnMath | kAnLong,

  // C0 AND_LONG_2ADDR vA, vB
  kAnMath | kAnLong,

  // C1 OR_LONG_2ADDR vA, vB
  kAnMath | kAnLong,

  // C2 XOR_LONG_2ADDR vA, vB
  kAnMath | kAnLong,

  // C3 SHL_LONG_2ADDR vA, vB
  kAnMath | kAnLong,

  // C4 SHR_LONG_2ADDR vA, vB
  kAnMath | kAnLong,

  // C5 USHR_LONG_2ADDR vA, vB
  kAnMath | kAnLong,

  // C6 ADD_FLOAT_2ADDR vA, vB
  kAnMath | kAnFp | kAnSingle,

  // C7 SUB_FLOAT_2ADDR vA, vB
  kAnMath | kAnFp | kAnSingle,

  // C8 MUL_FLOAT_2ADDR vA, vB
  kAnMath | kAnFp | kAnSingle,

  // C9 DIV_FLOAT_2ADDR vA, vB
  kAnMath | kAnFp | kAnSingle,

  // CA REM_FLOAT_2ADDR vA, vB
  kAnMath | kAnFp | kAnSingle,

  // CB ADD_DOUBLE_2ADDR vA, vB
  kAnMath | kAnFp | kAnDouble,

  // CC SUB_DOUBLE_2ADDR vA, vB
  kAnMath | kAnFp | kAnDouble,

  // CD MUL_DOUBLE_2ADDR vA, vB
  kAnMath | kAnFp | kAnDouble,

  // CE DIV_DOUBLE_2ADDR vA, vB
  kAnMath | kAnFp | kAnDouble,

  // CF REM_DOUBLE_2ADDR vA, vB
  kAnMath | kAnFp | kAnDouble,

  // D0 ADD_INT_LIT16 vA, vB, #+CCCC
  kAnMath | kAnInt,

  // D1 RSUB_INT vA, vB, #+CCCC
  kAnMath | kAnInt,

  // D2 MUL_INT_LIT16 vA, vB, #+CCCC
  kAnMath | kAnInt,

  // D3 DIV_INT_LIT16 vA, vB, #+CCCC
  kAnMath | kAnInt,

  // D4 REM_INT_LIT16 vA, vB, #+CCCC
  kAnMath | kAnInt,

  // D5 AND_INT_LIT16 vA, vB, #+CCCC
  kAnMath | kAnInt,

  // D6 OR_INT_LIT16 vA, vB, #+CCCC
  kAnMath | kAnInt,

  // D7 XOR_INT_LIT16 vA, vB, #+CCCC
  kAnMath | kAnInt,

  // D8 ADD_INT_LIT8 vAA, vBB, #+CC
  kAnMath | kAnInt,

  // D9 RSUB_INT_LIT8 vAA, vBB, #+CC
  kAnMath | kAnInt,

  // DA MUL_INT_LIT8 vAA, vBB, #+CC
  kAnMath | kAnInt,

  // DB DIV_INT_LIT8 vAA, vBB, #+CC
  kAnMath | kAnInt,

  // DC REM_INT_LIT8 vAA, vBB, #+CC
  kAnMath | kAnInt,

  // DD AND_INT_LIT8 vAA, vBB, #+CC
  kAnMath | kAnInt,

  // DE OR_INT_LIT8 vAA, vBB, #+CC
  kAnMath | kAnInt,

  // DF XOR_INT_LIT8 vAA, vBB, #+CC
  kAnMath | kAnInt,

  // E0 SHL_INT_LIT8 vAA, vBB, #+CC
  kAnMath | kAnInt,

  // E1 SHR_INT_LIT8 vAA, vBB, #+CC
  kAnMath | kAnInt,

  // E2 USHR_INT_LIT8 vAA, vBB, #+CC
  kAnMath | kAnInt,

  // E3 IGET_QUICK
  kAnNone,

  // E4 IGET_WIDE_QUICK
  kAnNone,

  // E5 IGET_OBJECT_QUICK
  kAnNone,

  // E6 IPUT_QUICK
  kAnNone,

  // E7 IPUT_WIDE_QUICK
  kAnNone,

  // E8 IPUT_OBJECT_QUICK
  kAnNone,

  // E9 INVOKE_VIRTUAL_QUICK
  kAnInvoke | kAnHeavyWeight,

  // EA INVOKE_VIRTUAL_RANGE_QUICK
  kAnInvoke | kAnHeavyWeight,

  // EB IPUT_BOOLEAN_QUICK
  kAnNone,

  // EC IPUT_BYTE_QUICK
  kAnNone,

  // ED IPUT_CHAR_QUICK
  kAnNone,

  // EE IPUT_SHORT_QUICK
  kAnNone,

  // EF IGET_BOOLEAN_QUICK
  kAnNone,

  // F0 IGET_BYTE_QUICK
  kAnNone,

  // F1 IGET_CHAR_QUICK
  kAnNone,

  // F2 IGET_SHORT_QUICK
  kAnNone,

  // F3 UNUSED_F3
  kAnNone,

  // F4 UNUSED_F4
  kAnNone,

  // F5 UNUSED_F5
  kAnNone,

  // F6 UNUSED_F6
  kAnNone,

  // F7 UNUSED_F7
  kAnNone,

  // F8 UNUSED_F8
  kAnNone,

  // F9 UNUSED_F9
  kAnNone,

  // FA UNUSED_FA
  kAnNone,

  // FB UNUSED_FB
  kAnNone,

  // FC UNUSED_FC
  kAnNone,

  // FD UNUSED_FD
  kAnNone,

  // FE UNUSED_FE
  kAnNone,

  // FF UNUSED_FF
  kAnNone,

  // Beginning of extended MIR opcodes
  // 100 MIR_PHI
  kAnNone,

  // 101 MIR_COPY
  kAnNone,

  // 102 MIR_FUSED_CMPL_FLOAT
  kAnNone,

  // 103 MIR_FUSED_CMPG_FLOAT
  kAnNone,

  // 104 MIR_FUSED_CMPL_DOUBLE
  kAnNone,

  // 105 MIR_FUSED_CMPG_DOUBLE
  kAnNone,

  // 106 MIR_FUSED_CMP_LONG
  kAnNone,

  // 107 MIR_NOP
  kAnNone,

  // 108 MIR_NULL_CHECK
  kAnNone,

  // 109 MIR_RANGE_CHECK
  kAnNone,

  // 10A MIR_DIV_ZERO_CHECK
  kAnNone,

  // 10B MIR_CHECK
  kAnNone,

  // 10C MIR_CHECKPART2
  kAnNone,

  // 10D MIR_SELECT
  kAnNone,

  // 10E MirOpConstVector
  kAnNone,

  // 10F MirOpMoveVector
  kAnNone,

  // 110 MirOpPackedMultiply
  kAnNone,

  // 111 MirOpPackedAddition
  kAnNone,

  // 112 MirOpPackedSubtract
  kAnNone,

  // 113 MirOpPackedShiftLeft
  kAnNone,

  // 114 MirOpPackedSignedShiftRight
  kAnNone,

  // 115 MirOpPackedUnsignedShiftRight
  kAnNone,

  // 116 MirOpPackedAnd
  kAnNone,

  // 117 MirOpPackedOr
  kAnNone,

  // 118 MirOpPackedXor
  kAnNone,

  // 119 MirOpPackedAddReduce
  kAnNone,

  // 11A MirOpPackedReduce
  kAnNone,

  // 11B MirOpPackedSet
  kAnNone,

  // 11C MirOpReserveVectorRegisters
  kAnNone,

  // 11D MirOpReturnVectorRegisters
  kAnNone,

  // 11E MirOpMemBarrier
  kAnNone,

  // 11F MirOpPackedArrayGet
  kAnArrayOp,

  // 120 MirOpPackedArrayPut
  kAnArrayOp,
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
  if (ending_bb->last_mir_insn != nullptr) {
    uint32_t ending_flags = kAnalysisAttributes[ending_bb->last_mir_insn->dalvikInsn.opcode];
    while ((ending_flags & kAnBranch) == 0) {
      ending_bb = GetBasicBlock(ending_bb->fall_through);
      ending_flags = kAnalysisAttributes[ending_bb->last_mir_insn->dalvikInsn.opcode];
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
    for (MIR* mir = tbb->first_mir_insn; mir != nullptr; mir = mir->next) {
      if (MIR::DecodedInstruction::IsPseudoMirOp(mir->dalvikInsn.opcode)) {
        // Skip any MIR pseudo-op.
        continue;
      }
      uint16_t flags = kAnalysisAttributes[mir->dalvikInsn.opcode];
      stats->dex_instructions += loop_scale_factor;
      if ((flags & kAnBranch) == 0) {
        computational_block &= ((flags & kAnComputational) != 0);
      } else {
        stats->branch_ops += loop_scale_factor;
      }
      if ((flags & kAnMath) != 0) {
        stats->math_ops += loop_scale_factor;
        has_math = true;
      }
      if ((flags & kAnFp) != 0) {
        stats->fp_ops += loop_scale_factor;
      }
      if ((flags & kAnArrayOp) != 0) {
        stats->array_ops += loop_scale_factor;
      }
      if ((flags & kAnHeavyWeight) != 0) {
        stats->heavyweight_ops += loop_scale_factor;
      }
      if ((flags & kAnSwitch) != 0) {
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

bool MIRGraph::ComputeSkipCompilation(MethodStats* stats, bool skip_default,
                                      std::string* skip_message) {
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
    *skip_message = "Is a small method with heavyweight ratio " +
                    std::to_string(stats->heavyweight_ratio);
    return true;
  }

  return skip_default;
}

 /*
  * Will eventually want this to be a bit more sophisticated and happen at verification time.
  */
bool MIRGraph::SkipCompilation(std::string* skip_message) {
  const CompilerOptions& compiler_options = cu_->compiler_driver->GetCompilerOptions();
  CompilerOptions::CompilerFilter compiler_filter = compiler_options.GetCompilerFilter();
  if (compiler_filter == CompilerOptions::kEverything) {
    return false;
  }

  // Contains a pattern we don't want to compile?
  if (PuntToInterpreter()) {
    *skip_message = "Punt to interpreter set";
    return true;
  }

  DCHECK(compiler_options.IsCompilationEnabled());

  // Set up compilation cutoffs based on current filter mode.
  size_t small_cutoff;
  size_t default_cutoff;
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
    case CompilerOptions::kTime:
      small_cutoff = compiler_options.GetHugeMethodThreshold();
      default_cutoff = compiler_options.GetHugeMethodThreshold();
      break;
    default:
      LOG(FATAL) << "Unexpected compiler_filter_: " << compiler_filter;
      UNREACHABLE();
  }

  // If size < cutoff, assume we'll compile - but allow removal.
  bool skip_compilation = (GetNumDalvikInsns() >= default_cutoff);
  if (skip_compilation) {
    *skip_message = "#Insns >= default_cutoff: " + std::to_string(GetNumDalvikInsns());
  }

  /*
   * Filter 1: Huge methods are likely to be machine generated, but some aren't.
   * If huge, assume we won't compile, but allow futher analysis to turn it back on.
   */
  if (compiler_options.IsHugeMethod(GetNumDalvikInsns())) {
    skip_compilation = true;
    *skip_message = "Huge method: " + std::to_string(GetNumDalvikInsns());
    // If we're got a huge number of basic blocks, don't bother with further analysis.
    if (static_cast<size_t>(GetNumBlocks()) > (compiler_options.GetHugeMethodThreshold() / 2)) {
      return true;
    }
  } else if (compiler_options.IsLargeMethod(GetNumDalvikInsns()) &&
    /* If it's large and contains no branches, it's likely to be machine generated initialization */
      (GetBranchCount() == 0)) {
    *skip_message = "Large method with no branches";
    return true;
  } else if (compiler_filter == CompilerOptions::kSpeed) {
    // If not huge, compile.
    return false;
  }

  // Filter 2: Skip class initializers.
  if (((cu_->access_flags & kAccConstructor) != 0) && ((cu_->access_flags & kAccStatic) != 0)) {
    *skip_message = "Class initializer";
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
  for (BasicBlock* bb = iter.Next(); bb != nullptr; bb = iter.Next()) {
    AnalyzeBlock(bb, &stats);
  }

  return ComputeSkipCompilation(&stats, skip_compilation, skip_message);
}

void MIRGraph::DoCacheFieldLoweringInfo() {
  static constexpr uint32_t kFieldIndexFlagQuickened = 0x80000000;
  // All IGET/IPUT/SGET/SPUT instructions take 2 code units and there must also be a RETURN.
  const uint32_t max_refs = (GetNumDalvikInsns() - 1u) / 2u;
  ScopedArenaAllocator allocator(&cu_->arena_stack);
  auto* field_idxs = allocator.AllocArray<uint32_t>(max_refs, kArenaAllocMisc);
  DexMemAccessType* field_types = allocator.AllocArray<DexMemAccessType>(
      max_refs, kArenaAllocMisc);
  // Find IGET/IPUT/SGET/SPUT insns, store IGET/IPUT fields at the beginning, SGET/SPUT at the end.
  size_t ifield_pos = 0u;
  size_t sfield_pos = max_refs;
  AllNodesIterator iter(this);
  for (BasicBlock* bb = iter.Next(); bb != nullptr; bb = iter.Next()) {
    if (bb->block_type != kDalvikByteCode) {
      continue;
    }
    for (MIR* mir = bb->first_mir_insn; mir != nullptr; mir = mir->next) {
      // Get field index and try to find it among existing indexes. If found, it's usually among
      // the last few added, so we'll start the search from ifield_pos/sfield_pos. Though this
      // is a linear search, it actually performs much better than map based approach.
      const bool is_iget_or_iput = IsInstructionIGetOrIPut(mir->dalvikInsn.opcode);
      const bool is_iget_or_iput_quick = IsInstructionIGetQuickOrIPutQuick(mir->dalvikInsn.opcode);
      if (is_iget_or_iput || is_iget_or_iput_quick) {
        uint32_t field_idx;
        DexMemAccessType access_type;
        if (is_iget_or_iput) {
          field_idx = mir->dalvikInsn.vC;
          access_type = IGetOrIPutMemAccessType(mir->dalvikInsn.opcode);
        } else {
          DCHECK(is_iget_or_iput_quick);
          // Set kFieldIndexFlagQuickened so that we don't deduplicate against non quickened field
          // indexes.
          field_idx = mir->offset | kFieldIndexFlagQuickened;
          access_type = IGetQuickOrIPutQuickMemAccessType(mir->dalvikInsn.opcode);
        }
        size_t i = ifield_pos;
        while (i != 0u && field_idxs[i - 1] != field_idx) {
          --i;
        }
        if (i != 0u) {
          mir->meta.ifield_lowering_info = i - 1;
          DCHECK_EQ(field_types[i - 1], access_type);
        } else {
          mir->meta.ifield_lowering_info = ifield_pos;
          field_idxs[ifield_pos] = field_idx;
          field_types[ifield_pos] = access_type;
          ++ifield_pos;
        }
      } else if (IsInstructionSGetOrSPut(mir->dalvikInsn.opcode)) {
        auto field_idx = mir->dalvikInsn.vB;
        size_t i = sfield_pos;
        while (i != max_refs && field_idxs[i] != field_idx) {
          ++i;
        }
        if (i != max_refs) {
          mir->meta.sfield_lowering_info = max_refs - i - 1u;
          DCHECK_EQ(field_types[i], SGetOrSPutMemAccessType(mir->dalvikInsn.opcode));
        } else {
          mir->meta.sfield_lowering_info = max_refs - sfield_pos;
          --sfield_pos;
          field_idxs[sfield_pos] = field_idx;
          field_types[sfield_pos] = SGetOrSPutMemAccessType(mir->dalvikInsn.opcode);
        }
      }
      DCHECK_LE(ifield_pos, sfield_pos);
    }
  }

  if (ifield_pos != 0u) {
    // Resolve instance field infos.
    DCHECK_EQ(ifield_lowering_infos_.size(), 0u);
    ifield_lowering_infos_.reserve(ifield_pos);
    for (size_t pos = 0u; pos != ifield_pos; ++pos) {
      const uint32_t field_idx = field_idxs[pos];
      const bool is_quickened = (field_idx & kFieldIndexFlagQuickened) != 0;
      const uint32_t masked_field_idx = field_idx & ~kFieldIndexFlagQuickened;
      CHECK_LT(masked_field_idx, 1u << 16);
      ifield_lowering_infos_.push_back(
          MirIFieldLoweringInfo(masked_field_idx, field_types[pos], is_quickened));
    }
    MirIFieldLoweringInfo::Resolve(cu_->compiler_driver, GetCurrentDexCompilationUnit(),
                                   ifield_lowering_infos_.data(), ifield_pos);
  }

  if (sfield_pos != max_refs) {
    // Resolve static field infos.
    DCHECK_EQ(sfield_lowering_infos_.size(), 0u);
    sfield_lowering_infos_.reserve(max_refs - sfield_pos);
    for (size_t pos = max_refs; pos != sfield_pos;) {
      --pos;
      sfield_lowering_infos_.push_back(MirSFieldLoweringInfo(field_idxs[pos], field_types[pos]));
    }
    MirSFieldLoweringInfo::Resolve(cu_->compiler_driver, GetCurrentDexCompilationUnit(),
                                   sfield_lowering_infos_.data(), max_refs - sfield_pos);
  }
}

void MIRGraph::DoCacheMethodLoweringInfo() {
  static constexpr uint16_t invoke_types[] = { kVirtual, kSuper, kDirect, kStatic, kInterface };
  static constexpr uint32_t kMethodIdxFlagQuickened = 0x80000000;

  // Embed the map value in the entry to avoid extra padding in 64-bit builds.
  struct MapEntry {
    // Map key: target_method_idx, invoke_type, devirt_target. Ordered to avoid padding.
    const MethodReference* devirt_target;
    uint32_t target_method_idx;
    uint32_t vtable_idx;
    uint16_t invoke_type;
    // Map value.
    uint32_t lowering_info_index;
  };

  struct MapEntryComparator {
    bool operator()(const MapEntry& lhs, const MapEntry& rhs) const {
      if (lhs.target_method_idx != rhs.target_method_idx) {
        return lhs.target_method_idx < rhs.target_method_idx;
      }
      if (lhs.invoke_type != rhs.invoke_type) {
        return lhs.invoke_type < rhs.invoke_type;
      }
      if (lhs.vtable_idx != rhs.vtable_idx) {
        return lhs.vtable_idx < rhs.vtable_idx;
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
  const uint32_t max_refs = (GetNumDalvikInsns() - 1u) / 3u;

  // Map invoke key (see MapEntry) to lowering info index and vice versa.
  // The invoke_map and sequential entries are essentially equivalent to Boost.MultiIndex's
  // multi_index_container with one ordered index and one sequential index.
  ScopedArenaSet<MapEntry, MapEntryComparator> invoke_map(MapEntryComparator(),
                                                          allocator.Adapter());
  const MapEntry** sequential_entries =
      allocator.AllocArray<const MapEntry*>(max_refs, kArenaAllocMisc);

  // Find INVOKE insns and their devirtualization targets.
  const VerifiedMethod* verified_method = GetCurrentDexCompilationUnit()->GetVerifiedMethod();
  AllNodesIterator iter(this);
  for (BasicBlock* bb = iter.Next(); bb != nullptr; bb = iter.Next()) {
    if (bb->block_type != kDalvikByteCode) {
      continue;
    }
    for (MIR* mir = bb->first_mir_insn; mir != nullptr; mir = mir->next) {
      const bool is_quick_invoke = IsInstructionQuickInvoke(mir->dalvikInsn.opcode);
      const bool is_invoke = IsInstructionInvoke(mir->dalvikInsn.opcode);
      if (is_quick_invoke || is_invoke) {
        uint32_t vtable_index = 0;
        uint32_t target_method_idx = 0;
        uint32_t invoke_type_idx = 0;  // Default to virtual (in case of quickened).
        DCHECK_EQ(invoke_types[invoke_type_idx], kVirtual);
        if (is_quick_invoke) {
          // We need to store the vtable index since we can't necessarily recreate it at resolve
          // phase if the dequickening resolved to an interface method.
          vtable_index = mir->dalvikInsn.vB;
          // Fake up the method index by storing the mir offset so that we can read the dequicken
          // info in resolve.
          target_method_idx = mir->offset | kMethodIdxFlagQuickened;
        } else {
          DCHECK(is_invoke);
          // Decode target method index and invoke type.
          invoke_type_idx = InvokeInstructionType(mir->dalvikInsn.opcode);
          target_method_idx = mir->dalvikInsn.vB;
        }
        // Find devirtualization target.
        // TODO: The devirt map is ordered by the dex pc here. Is there a way to get INVOKEs
        // ordered by dex pc as well? That would allow us to keep an iterator to devirt targets
        // and increment it as needed instead of making O(log n) lookups.
        const MethodReference* devirt_target = verified_method->GetDevirtTarget(mir->offset);
        // Try to insert a new entry. If the insertion fails, we will have found an old one.
        MapEntry entry = {
            devirt_target,
            target_method_idx,
            vtable_index,
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
  const size_t count = invoke_map.size();
  method_lowering_infos_.reserve(count);
  for (size_t pos = 0u; pos != count; ++pos) {
    const MapEntry* entry = sequential_entries[pos];
    const bool is_quick = (entry->target_method_idx & kMethodIdxFlagQuickened) != 0;
    const uint32_t masked_method_idx = entry->target_method_idx & ~kMethodIdxFlagQuickened;
    MirMethodLoweringInfo method_info(masked_method_idx,
                                      static_cast<InvokeType>(entry->invoke_type), is_quick);
    if (entry->devirt_target != nullptr) {
      method_info.SetDevirtualizationTarget(*entry->devirt_target);
    }
    if (is_quick) {
      method_info.SetVTableIndex(entry->vtable_idx);
    }
    method_lowering_infos_.push_back(method_info);
  }
  MirMethodLoweringInfo::Resolve(cu_->compiler_driver, GetCurrentDexCompilationUnit(),
                                 method_lowering_infos_.data(), count);
}

bool MIRGraph::SkipCompilationByName(const std::string& methodname) {
  return cu_->compiler_driver->SkipCompilation(methodname);
}

}  // namespace art
