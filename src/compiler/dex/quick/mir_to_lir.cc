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

#include "object_utils.h"

#include "compiler/dex/compiler_internals.h"
#include "local_optimizations.h"
#include "codegen_util.h"
#include "ralloc_util.h"

namespace art {

/*
 * Target-independent code generation.  Use only high-level
 * load/store utilities here, or target-dependent genXX() handlers
 * when necessary.
 */
static void CompileDalvikInstruction(CompilationUnit* cu, MIR* mir, BasicBlock* bb,
                                     LIR* label_list)
{
  Codegen* cg = cu->cg.get();
  RegLocation rl_src[3];
  RegLocation rl_dest = GetBadLoc();
  RegLocation rl_result = GetBadLoc();
  Instruction::Code opcode = mir->dalvikInsn.opcode;
  int opt_flags = mir->optimization_flags;
  uint32_t vB = mir->dalvikInsn.vB;
  uint32_t vC = mir->dalvikInsn.vC;

  // Prep Src and Dest locations.
  int next_sreg = 0;
  int next_loc = 0;
  int attrs = oat_data_flow_attributes[opcode];
  rl_src[0] = rl_src[1] = rl_src[2] = GetBadLoc();
  if (attrs & DF_UA) {
    if (attrs & DF_A_WIDE) {
      rl_src[next_loc++] = GetSrcWide(cu, mir, next_sreg);
      next_sreg+= 2;
    } else {
      rl_src[next_loc++] = GetSrc(cu, mir, next_sreg);
      next_sreg++;
    }
  }
  if (attrs & DF_UB) {
    if (attrs & DF_B_WIDE) {
      rl_src[next_loc++] = GetSrcWide(cu, mir, next_sreg);
      next_sreg+= 2;
    } else {
      rl_src[next_loc++] = GetSrc(cu, mir, next_sreg);
      next_sreg++;
    }
  }
  if (attrs & DF_UC) {
    if (attrs & DF_C_WIDE) {
      rl_src[next_loc++] = GetSrcWide(cu, mir, next_sreg);
    } else {
      rl_src[next_loc++] = GetSrc(cu, mir, next_sreg);
    }
  }
  if (attrs & DF_DA) {
    if (attrs & DF_A_WIDE) {
      rl_dest = GetDestWide(cu, mir);
    } else {
      rl_dest = GetDest(cu, mir);
    }
  }
  switch (opcode) {
    case Instruction::NOP:
      break;

    case Instruction::MOVE_EXCEPTION:
      cg->GenMoveException(cu, rl_dest);
      break;

    case Instruction::RETURN_VOID:
      if (((cu->access_flags & kAccConstructor) != 0) &&
          cu->compiler->RequiresConstructorBarrier(Thread::Current(), cu->dex_file,
                                                   cu->class_def_idx)) {
        cg->GenMemBarrier(cu, kStoreStore);
      }
      if (!(cu->attrs & METHOD_IS_LEAF)) {
        cg->GenSuspendTest(cu, opt_flags);
      }
      break;

    case Instruction::RETURN:
    case Instruction::RETURN_OBJECT:
      if (!(cu->attrs & METHOD_IS_LEAF)) {
        cg->GenSuspendTest(cu, opt_flags);
      }
      cg->StoreValue(cu, GetReturn(cu, cu->shorty[0] == 'F'), rl_src[0]);
      break;

    case Instruction::RETURN_WIDE:
      if (!(cu->attrs & METHOD_IS_LEAF)) {
        cg->GenSuspendTest(cu, opt_flags);
      }
      cg->StoreValueWide(cu, GetReturnWide(cu,
                       cu->shorty[0] == 'D'), rl_src[0]);
      break;

    case Instruction::MOVE_RESULT_WIDE:
      if (opt_flags & MIR_INLINED)
        break;  // Nop - combined w/ previous invoke.
      cg->StoreValueWide(cu, rl_dest, GetReturnWide(cu, rl_dest.fp));
      break;

    case Instruction::MOVE_RESULT:
    case Instruction::MOVE_RESULT_OBJECT:
      if (opt_flags & MIR_INLINED)
        break;  // Nop - combined w/ previous invoke.
      cg->StoreValue(cu, rl_dest, GetReturn(cu, rl_dest.fp));
      break;

    case Instruction::MOVE:
    case Instruction::MOVE_OBJECT:
    case Instruction::MOVE_16:
    case Instruction::MOVE_OBJECT_16:
    case Instruction::MOVE_FROM16:
    case Instruction::MOVE_OBJECT_FROM16:
      cg->StoreValue(cu, rl_dest, rl_src[0]);
      break;

    case Instruction::MOVE_WIDE:
    case Instruction::MOVE_WIDE_16:
    case Instruction::MOVE_WIDE_FROM16:
      cg->StoreValueWide(cu, rl_dest, rl_src[0]);
      break;

    case Instruction::CONST:
    case Instruction::CONST_4:
    case Instruction::CONST_16:
      rl_result = EvalLoc(cu, rl_dest, kAnyReg, true);
      cg->LoadConstantNoClobber(cu, rl_result.low_reg, vB);
      cg->StoreValue(cu, rl_dest, rl_result);
      if (vB == 0) {
        cg->Workaround7250540(cu, rl_dest, rl_result.low_reg);
      }
      break;

    case Instruction::CONST_HIGH16:
      rl_result = EvalLoc(cu, rl_dest, kAnyReg, true);
      cg->LoadConstantNoClobber(cu, rl_result.low_reg, vB << 16);
      cg->StoreValue(cu, rl_dest, rl_result);
      if (vB == 0) {
        cg->Workaround7250540(cu, rl_dest, rl_result.low_reg);
      }
      break;

    case Instruction::CONST_WIDE_16:
    case Instruction::CONST_WIDE_32:
      rl_result = EvalLoc(cu, rl_dest, kAnyReg, true);
      cg->LoadConstantWide(cu, rl_result.low_reg, rl_result.high_reg,
                           static_cast<int64_t>(static_cast<int32_t>(vB)));
      cg->StoreValueWide(cu, rl_dest, rl_result);
      break;

    case Instruction::CONST_WIDE:
      rl_result = EvalLoc(cu, rl_dest, kAnyReg, true);
      cg->LoadConstantWide(cu, rl_result.low_reg, rl_result.high_reg, mir->dalvikInsn.vB_wide);
      cg->StoreValueWide(cu, rl_dest, rl_result);
      break;

    case Instruction::CONST_WIDE_HIGH16:
      rl_result = EvalLoc(cu, rl_dest, kAnyReg, true);
      cg->LoadConstantWide(cu, rl_result.low_reg, rl_result.high_reg,
                           static_cast<int64_t>(vB) << 48);
      cg->StoreValueWide(cu, rl_dest, rl_result);
      break;

    case Instruction::MONITOR_ENTER:
      cg->GenMonitorEnter(cu, opt_flags, rl_src[0]);
      break;

    case Instruction::MONITOR_EXIT:
      cg->GenMonitorExit(cu, opt_flags, rl_src[0]);
      break;

    case Instruction::CHECK_CAST:
      cg->GenCheckCast(cu, vB, rl_src[0]);
      break;

    case Instruction::INSTANCE_OF:
      cg->GenInstanceof(cu, vC, rl_dest, rl_src[0]);
      break;

    case Instruction::NEW_INSTANCE:
      cg->GenNewInstance(cu, vB, rl_dest);
      break;

    case Instruction::THROW:
      cg->GenThrow(cu, rl_src[0]);
      break;

    case Instruction::ARRAY_LENGTH:
      int len_offset;
      len_offset = mirror::Array::LengthOffset().Int32Value();
      rl_src[0] = cg->LoadValue(cu, rl_src[0], kCoreReg);
      cg->GenNullCheck(cu, rl_src[0].s_reg_low, rl_src[0].low_reg, opt_flags);
      rl_result = EvalLoc(cu, rl_dest, kCoreReg, true);
      cg->LoadWordDisp(cu, rl_src[0].low_reg, len_offset, rl_result.low_reg);
      cg->StoreValue(cu, rl_dest, rl_result);
      break;

    case Instruction::CONST_STRING:
    case Instruction::CONST_STRING_JUMBO:
      cg->GenConstString(cu, vB, rl_dest);
      break;

    case Instruction::CONST_CLASS:
      cg->GenConstClass(cu, vB, rl_dest);
      break;

    case Instruction::FILL_ARRAY_DATA:
      cg->GenFillArrayData(cu, vB, rl_src[0]);
      break;

    case Instruction::FILLED_NEW_ARRAY:
      cg->GenFilledNewArray(cu, cg->NewMemCallInfo(cu, bb, mir, kStatic,
                        false /* not range */));
      break;

    case Instruction::FILLED_NEW_ARRAY_RANGE:
      cg->GenFilledNewArray(cu, cg->NewMemCallInfo(cu, bb, mir, kStatic,
                        true /* range */));
      break;

    case Instruction::NEW_ARRAY:
      cg->GenNewArray(cu, vC, rl_dest, rl_src[0]);
      break;

    case Instruction::GOTO:
    case Instruction::GOTO_16:
    case Instruction::GOTO_32:
      if (bb->taken->start_offset <= mir->offset) {
        cg->GenSuspendTestAndBranch(cu, opt_flags, &label_list[bb->taken->id]);
      } else {
        cg->OpUnconditionalBranch(cu, &label_list[bb->taken->id]);
      }
      break;

    case Instruction::PACKED_SWITCH:
      cg->GenPackedSwitch(cu, vB, rl_src[0]);
      break;

    case Instruction::SPARSE_SWITCH:
      cg->GenSparseSwitch(cu, vB, rl_src[0]);
      break;

    case Instruction::CMPL_FLOAT:
    case Instruction::CMPG_FLOAT:
    case Instruction::CMPL_DOUBLE:
    case Instruction::CMPG_DOUBLE:
      cg->GenCmpFP(cu, opcode, rl_dest, rl_src[0], rl_src[1]);
      break;

    case Instruction::CMP_LONG:
      cg->GenCmpLong(cu, rl_dest, rl_src[0], rl_src[1]);
      break;

    case Instruction::IF_EQ:
    case Instruction::IF_NE:
    case Instruction::IF_LT:
    case Instruction::IF_GE:
    case Instruction::IF_GT:
    case Instruction::IF_LE: {
      LIR* taken = &label_list[bb->taken->id];
      LIR* fall_through = &label_list[bb->fall_through->id];
      bool backward_branch;
      backward_branch = (bb->taken->start_offset <= mir->offset);
      // Result known at compile time?
      if (rl_src[0].is_const && rl_src[1].is_const) {
        bool is_taken = EvaluateBranch(opcode, cu->constant_values[rl_src[0].orig_sreg],
                                       cu->constant_values[rl_src[1].orig_sreg]);
        if (is_taken && backward_branch) {
          cg->GenSuspendTest(cu, opt_flags);
        }
        int id = is_taken ? bb->taken->id : bb->fall_through->id;
        cg->OpUnconditionalBranch(cu, &label_list[id]);
      } else {
        if (backward_branch) {
          cg->GenSuspendTest(cu, opt_flags);
        }
        cg->GenCompareAndBranch(cu, opcode, rl_src[0], rl_src[1], taken,
                                fall_through);
      }
      break;
      }

    case Instruction::IF_EQZ:
    case Instruction::IF_NEZ:
    case Instruction::IF_LTZ:
    case Instruction::IF_GEZ:
    case Instruction::IF_GTZ:
    case Instruction::IF_LEZ: {
      LIR* taken = &label_list[bb->taken->id];
      LIR* fall_through = &label_list[bb->fall_through->id];
      bool backward_branch;
      backward_branch = (bb->taken->start_offset <= mir->offset);
      // Result known at compile time?
      if (rl_src[0].is_const) {
        bool is_taken = EvaluateBranch(opcode, cu->constant_values[rl_src[0].orig_sreg], 0);
        if (is_taken && backward_branch) {
          cg->GenSuspendTest(cu, opt_flags);
        }
        int id = is_taken ? bb->taken->id : bb->fall_through->id;
        cg->OpUnconditionalBranch(cu, &label_list[id]);
      } else {
        if (backward_branch) {
          cg->GenSuspendTest(cu, opt_flags);
        }
        cg->GenCompareZeroAndBranch(cu, opcode, rl_src[0], taken, fall_through);
      }
      break;
      }

    case Instruction::AGET_WIDE:
      cg->GenArrayGet(cu, opt_flags, kLong, rl_src[0], rl_src[1], rl_dest, 3);
      break;
    case Instruction::AGET:
    case Instruction::AGET_OBJECT:
      cg->GenArrayGet(cu, opt_flags, kWord, rl_src[0], rl_src[1], rl_dest, 2);
      break;
    case Instruction::AGET_BOOLEAN:
      cg->GenArrayGet(cu, opt_flags, kUnsignedByte, rl_src[0], rl_src[1], rl_dest, 0);
      break;
    case Instruction::AGET_BYTE:
      cg->GenArrayGet(cu, opt_flags, kSignedByte, rl_src[0], rl_src[1], rl_dest, 0);
      break;
    case Instruction::AGET_CHAR:
      cg->GenArrayGet(cu, opt_flags, kUnsignedHalf, rl_src[0], rl_src[1], rl_dest, 1);
      break;
    case Instruction::AGET_SHORT:
      cg->GenArrayGet(cu, opt_flags, kSignedHalf, rl_src[0], rl_src[1], rl_dest, 1);
      break;
    case Instruction::APUT_WIDE:
      cg->GenArrayPut(cu, opt_flags, kLong, rl_src[1], rl_src[2], rl_src[0], 3);
      break;
    case Instruction::APUT:
      cg->GenArrayPut(cu, opt_flags, kWord, rl_src[1], rl_src[2], rl_src[0], 2);
      break;
    case Instruction::APUT_OBJECT:
      cg->GenArrayObjPut(cu, opt_flags, rl_src[1], rl_src[2], rl_src[0], 2);
      break;
    case Instruction::APUT_SHORT:
    case Instruction::APUT_CHAR:
      cg->GenArrayPut(cu, opt_flags, kUnsignedHalf, rl_src[1], rl_src[2], rl_src[0], 1);
      break;
    case Instruction::APUT_BYTE:
    case Instruction::APUT_BOOLEAN:
      cg->GenArrayPut(cu, opt_flags, kUnsignedByte, rl_src[1], rl_src[2],
            rl_src[0], 0);
      break;

    case Instruction::IGET_OBJECT:
      cg->GenIGet(cu, vC, opt_flags, kWord, rl_dest, rl_src[0], false, true);
      break;

    case Instruction::IGET_WIDE:
      cg->GenIGet(cu, vC, opt_flags, kLong, rl_dest, rl_src[0], true, false);
      break;

    case Instruction::IGET:
      cg->GenIGet(cu, vC, opt_flags, kWord, rl_dest, rl_src[0], false, false);
      break;

    case Instruction::IGET_CHAR:
      cg->GenIGet(cu, vC, opt_flags, kUnsignedHalf, rl_dest, rl_src[0], false, false);
      break;

    case Instruction::IGET_SHORT:
      cg->GenIGet(cu, vC, opt_flags, kSignedHalf, rl_dest, rl_src[0], false, false);
      break;

    case Instruction::IGET_BOOLEAN:
    case Instruction::IGET_BYTE:
      cg->GenIGet(cu, vC, opt_flags, kUnsignedByte, rl_dest, rl_src[0], false, false);
      break;

    case Instruction::IPUT_WIDE:
      cg->GenIPut(cu, vC, opt_flags, kLong, rl_src[0], rl_src[1], true, false);
      break;

    case Instruction::IPUT_OBJECT:
      cg->GenIPut(cu, vC, opt_flags, kWord, rl_src[0], rl_src[1], false, true);
      break;

    case Instruction::IPUT:
      cg->GenIPut(cu, vC, opt_flags, kWord, rl_src[0], rl_src[1], false, false);
      break;

    case Instruction::IPUT_BOOLEAN:
    case Instruction::IPUT_BYTE:
      cg->GenIPut(cu, vC, opt_flags, kUnsignedByte, rl_src[0], rl_src[1], false, false);
      break;

    case Instruction::IPUT_CHAR:
      cg->GenIPut(cu, vC, opt_flags, kUnsignedHalf, rl_src[0], rl_src[1], false, false);
      break;

    case Instruction::IPUT_SHORT:
      cg->GenIPut(cu, vC, opt_flags, kSignedHalf, rl_src[0], rl_src[1], false, false);
      break;

    case Instruction::SGET_OBJECT:
      cg->GenSget(cu, vB, rl_dest, false, true);
      break;
    case Instruction::SGET:
    case Instruction::SGET_BOOLEAN:
    case Instruction::SGET_BYTE:
    case Instruction::SGET_CHAR:
    case Instruction::SGET_SHORT:
      cg->GenSget(cu, vB, rl_dest, false, false);
      break;

    case Instruction::SGET_WIDE:
      cg->GenSget(cu, vB, rl_dest, true, false);
      break;

    case Instruction::SPUT_OBJECT:
      cg->GenSput(cu, vB, rl_src[0], false, true);
      break;

    case Instruction::SPUT:
    case Instruction::SPUT_BOOLEAN:
    case Instruction::SPUT_BYTE:
    case Instruction::SPUT_CHAR:
    case Instruction::SPUT_SHORT:
      cg->GenSput(cu, vB, rl_src[0], false, false);
      break;

    case Instruction::SPUT_WIDE:
      cg->GenSput(cu, vB, rl_src[0], true, false);
      break;

    case Instruction::INVOKE_STATIC_RANGE:
      cg->GenInvoke(cu, cg->NewMemCallInfo(cu, bb, mir, kStatic, true));
      break;
    case Instruction::INVOKE_STATIC:
      cg->GenInvoke(cu, cg->NewMemCallInfo(cu, bb, mir, kStatic, false));
      break;

    case Instruction::INVOKE_DIRECT:
      cg->GenInvoke(cu, cg->NewMemCallInfo(cu, bb, mir, kDirect, false));
      break;
    case Instruction::INVOKE_DIRECT_RANGE:
      cg->GenInvoke(cu, cg->NewMemCallInfo(cu, bb, mir, kDirect, true));
      break;

    case Instruction::INVOKE_VIRTUAL:
      cg->GenInvoke(cu, cg->NewMemCallInfo(cu, bb, mir, kVirtual, false));
      break;
    case Instruction::INVOKE_VIRTUAL_RANGE:
      cg->GenInvoke(cu, cg->NewMemCallInfo(cu, bb, mir, kVirtual, true));
      break;

    case Instruction::INVOKE_SUPER:
      cg->GenInvoke(cu, cg->NewMemCallInfo(cu, bb, mir, kSuper, false));
      break;
    case Instruction::INVOKE_SUPER_RANGE:
      cg->GenInvoke(cu, cg->NewMemCallInfo(cu, bb, mir, kSuper, true));
      break;

    case Instruction::INVOKE_INTERFACE:
      cg->GenInvoke(cu, cg->NewMemCallInfo(cu, bb, mir, kInterface, false));
      break;
    case Instruction::INVOKE_INTERFACE_RANGE:
      cg->GenInvoke(cu, cg->NewMemCallInfo(cu, bb, mir, kInterface, true));
      break;

    case Instruction::NEG_INT:
    case Instruction::NOT_INT:
      cg->GenArithOpInt(cu, opcode, rl_dest, rl_src[0], rl_src[0]);
      break;

    case Instruction::NEG_LONG:
    case Instruction::NOT_LONG:
      cg->GenArithOpLong(cu, opcode, rl_dest, rl_src[0], rl_src[0]);
      break;

    case Instruction::NEG_FLOAT:
      cg->GenArithOpFloat(cu, opcode, rl_dest, rl_src[0], rl_src[0]);
      break;

    case Instruction::NEG_DOUBLE:
      cg->GenArithOpDouble(cu, opcode, rl_dest, rl_src[0], rl_src[0]);
      break;

    case Instruction::INT_TO_LONG:
      cg->GenIntToLong(cu, rl_dest, rl_src[0]);
      break;

    case Instruction::LONG_TO_INT:
      rl_src[0] = UpdateLocWide(cu, rl_src[0]);
      rl_src[0] = WideToNarrow(cu, rl_src[0]);
      cg->StoreValue(cu, rl_dest, rl_src[0]);
      break;

    case Instruction::INT_TO_BYTE:
    case Instruction::INT_TO_SHORT:
    case Instruction::INT_TO_CHAR:
      cg->GenIntNarrowing(cu, opcode, rl_dest, rl_src[0]);
      break;

    case Instruction::INT_TO_FLOAT:
    case Instruction::INT_TO_DOUBLE:
    case Instruction::LONG_TO_FLOAT:
    case Instruction::LONG_TO_DOUBLE:
    case Instruction::FLOAT_TO_INT:
    case Instruction::FLOAT_TO_LONG:
    case Instruction::FLOAT_TO_DOUBLE:
    case Instruction::DOUBLE_TO_INT:
    case Instruction::DOUBLE_TO_LONG:
    case Instruction::DOUBLE_TO_FLOAT:
      cg->GenConversion(cu, opcode, rl_dest, rl_src[0]);
      break;


    case Instruction::ADD_INT:
    case Instruction::ADD_INT_2ADDR:
    case Instruction::MUL_INT:
    case Instruction::MUL_INT_2ADDR:
    case Instruction::AND_INT:
    case Instruction::AND_INT_2ADDR:
    case Instruction::OR_INT:
    case Instruction::OR_INT_2ADDR:
    case Instruction::XOR_INT:
    case Instruction::XOR_INT_2ADDR:
      if (rl_src[0].is_const &&
          cu->cg->InexpensiveConstantInt(ConstantValue(cu, rl_src[0]))) {
        cg->GenArithOpIntLit(cu, opcode, rl_dest, rl_src[1],
                             cu->constant_values[rl_src[0].orig_sreg]);
      } else if (rl_src[1].is_const &&
          cu->cg->InexpensiveConstantInt(ConstantValue(cu, rl_src[1]))) {
        cg->GenArithOpIntLit(cu, opcode, rl_dest, rl_src[0],
                             cu->constant_values[rl_src[1].orig_sreg]);
      } else {
        cg->GenArithOpInt(cu, opcode, rl_dest, rl_src[0], rl_src[1]);
      }
      break;

    case Instruction::SUB_INT:
    case Instruction::SUB_INT_2ADDR:
    case Instruction::DIV_INT:
    case Instruction::DIV_INT_2ADDR:
    case Instruction::REM_INT:
    case Instruction::REM_INT_2ADDR:
    case Instruction::SHL_INT:
    case Instruction::SHL_INT_2ADDR:
    case Instruction::SHR_INT:
    case Instruction::SHR_INT_2ADDR:
    case Instruction::USHR_INT:
    case Instruction::USHR_INT_2ADDR:
      if (rl_src[1].is_const &&
          cu->cg->InexpensiveConstantInt(ConstantValue(cu, rl_src[1]))) {
        cg->GenArithOpIntLit(cu, opcode, rl_dest, rl_src[0], ConstantValue(cu, rl_src[1]));
      } else {
        cg->GenArithOpInt(cu, opcode, rl_dest, rl_src[0], rl_src[1]);
      }
      break;

    case Instruction::ADD_LONG:
    case Instruction::SUB_LONG:
    case Instruction::AND_LONG:
    case Instruction::OR_LONG:
    case Instruction::XOR_LONG:
    case Instruction::ADD_LONG_2ADDR:
    case Instruction::SUB_LONG_2ADDR:
    case Instruction::AND_LONG_2ADDR:
    case Instruction::OR_LONG_2ADDR:
    case Instruction::XOR_LONG_2ADDR:
      if (rl_src[0].is_const || rl_src[1].is_const) {
        cg->GenArithImmOpLong(cu, opcode, rl_dest, rl_src[0], rl_src[1]);
        break;
      }
      // Note: intentional fallthrough.

    case Instruction::MUL_LONG:
    case Instruction::DIV_LONG:
    case Instruction::REM_LONG:
    case Instruction::MUL_LONG_2ADDR:
    case Instruction::DIV_LONG_2ADDR:
    case Instruction::REM_LONG_2ADDR:
      cg->GenArithOpLong(cu, opcode, rl_dest, rl_src[0], rl_src[1]);
      break;

    case Instruction::SHL_LONG:
    case Instruction::SHR_LONG:
    case Instruction::USHR_LONG:
    case Instruction::SHL_LONG_2ADDR:
    case Instruction::SHR_LONG_2ADDR:
    case Instruction::USHR_LONG_2ADDR:
      if (rl_src[1].is_const) {
        cg->GenShiftImmOpLong(cu, opcode, rl_dest, rl_src[0], rl_src[1]);
      } else {
        cg->GenShiftOpLong(cu, opcode, rl_dest, rl_src[0], rl_src[1]);
      }
      break;

    case Instruction::ADD_FLOAT:
    case Instruction::SUB_FLOAT:
    case Instruction::MUL_FLOAT:
    case Instruction::DIV_FLOAT:
    case Instruction::REM_FLOAT:
    case Instruction::ADD_FLOAT_2ADDR:
    case Instruction::SUB_FLOAT_2ADDR:
    case Instruction::MUL_FLOAT_2ADDR:
    case Instruction::DIV_FLOAT_2ADDR:
    case Instruction::REM_FLOAT_2ADDR:
      cg->GenArithOpFloat(cu, opcode, rl_dest, rl_src[0], rl_src[1]);
      break;

    case Instruction::ADD_DOUBLE:
    case Instruction::SUB_DOUBLE:
    case Instruction::MUL_DOUBLE:
    case Instruction::DIV_DOUBLE:
    case Instruction::REM_DOUBLE:
    case Instruction::ADD_DOUBLE_2ADDR:
    case Instruction::SUB_DOUBLE_2ADDR:
    case Instruction::MUL_DOUBLE_2ADDR:
    case Instruction::DIV_DOUBLE_2ADDR:
    case Instruction::REM_DOUBLE_2ADDR:
      cg->GenArithOpDouble(cu, opcode, rl_dest, rl_src[0], rl_src[1]);
      break;

    case Instruction::RSUB_INT:
    case Instruction::ADD_INT_LIT16:
    case Instruction::MUL_INT_LIT16:
    case Instruction::DIV_INT_LIT16:
    case Instruction::REM_INT_LIT16:
    case Instruction::AND_INT_LIT16:
    case Instruction::OR_INT_LIT16:
    case Instruction::XOR_INT_LIT16:
    case Instruction::ADD_INT_LIT8:
    case Instruction::RSUB_INT_LIT8:
    case Instruction::MUL_INT_LIT8:
    case Instruction::DIV_INT_LIT8:
    case Instruction::REM_INT_LIT8:
    case Instruction::AND_INT_LIT8:
    case Instruction::OR_INT_LIT8:
    case Instruction::XOR_INT_LIT8:
    case Instruction::SHL_INT_LIT8:
    case Instruction::SHR_INT_LIT8:
    case Instruction::USHR_INT_LIT8:
      cg->GenArithOpIntLit(cu, opcode, rl_dest, rl_src[0], vC);
      break;

    default:
      LOG(FATAL) << "Unexpected opcode: " << opcode;
  }
}

// Process extended MIR instructions
static void HandleExtendedMethodMIR(CompilationUnit* cu, BasicBlock* bb, MIR* mir)
{
  Codegen* cg = cu->cg.get();
  switch (static_cast<ExtendedMIROpcode>(mir->dalvikInsn.opcode)) {
    case kMirOpCopy: {
      RegLocation rl_src = GetSrc(cu, mir, 0);
      RegLocation rl_dest = GetDest(cu, mir);
      cg->StoreValue(cu, rl_dest, rl_src);
      break;
    }
    case kMirOpFusedCmplFloat:
      cg->GenFusedFPCmpBranch(cu, bb, mir, false /*gt bias*/, false /*double*/);
      break;
    case kMirOpFusedCmpgFloat:
      cg->GenFusedFPCmpBranch(cu, bb, mir, true /*gt bias*/, false /*double*/);
      break;
    case kMirOpFusedCmplDouble:
      cg->GenFusedFPCmpBranch(cu, bb, mir, false /*gt bias*/, true /*double*/);
      break;
    case kMirOpFusedCmpgDouble:
      cg->GenFusedFPCmpBranch(cu, bb, mir, true /*gt bias*/, true /*double*/);
      break;
    case kMirOpFusedCmpLong:
      cg->GenFusedLongCmpBranch(cu, bb, mir);
      break;
    case kMirOpSelect:
      cg->GenSelect(cu, bb, mir);
      break;
    default:
      break;
  }
}

// Handle the content in each basic block.
static bool MethodBlockCodeGen(CompilationUnit* cu, BasicBlock* bb)
{
  if (bb->block_type == kDead) return false;
  Codegen* cg = cu->cg.get();
  cu->current_dalvik_offset = bb->start_offset;
  MIR* mir;
  LIR* label_list = cu->block_label_list;
  int block_id = bb->id;

  cu->cur_block = bb;
  label_list[block_id].operands[0] = bb->start_offset;

  // Insert the block label.
  label_list[block_id].opcode = kPseudoNormalBlockLabel;
  AppendLIR(cu, &label_list[block_id]);

  LIR* head_lir = NULL;

  // If this is a catch block, export the start address.
  if (bb->catch_entry) {
    head_lir = NewLIR0(cu, kPseudoExportedPC);
  }

  // Free temp registers and reset redundant store tracking.
  ResetRegPool(cu);
  ResetDefTracking(cu);

  ClobberAllRegs(cu);

  if (bb->block_type == kEntryBlock) {
    int start_vreg = cu->num_dalvik_registers - cu->num_ins;
    cg->GenEntrySequence(cu, &cu->reg_location[start_vreg],
                         cu->reg_location[cu->method_sreg]);
  } else if (bb->block_type == kExitBlock) {
    cg->GenExitSequence(cu);
  }

  for (mir = bb->first_mir_insn; mir != NULL; mir = mir->next) {
    ResetRegPool(cu);
    if (cu->disable_opt & (1 << kTrackLiveTemps)) {
      ClobberAllRegs(cu);
    }

    if (cu->disable_opt & (1 << kSuppressLoads)) {
      ResetDefTracking(cu);
    }

#ifndef NDEBUG
    // Reset temp tracking sanity check.
    cu->live_sreg = INVALID_SREG;
#endif

    cu->current_dalvik_offset = mir->offset;
    int opcode = mir->dalvikInsn.opcode;
    LIR* boundary_lir;

    // Mark the beginning of a Dalvik instruction for line tracking.
    char* inst_str = cu->verbose ?
       GetDalvikDisassembly(cu, mir) : NULL;
    boundary_lir = MarkBoundary(cu, mir->offset, inst_str);
    // Remember the first LIR for this block.
    if (head_lir == NULL) {
      head_lir = boundary_lir;
      // Set the first boundary_lir as a scheduling barrier.
      head_lir->def_mask = ENCODE_ALL;
    }

    if (opcode == kMirOpCheck) {
      // Combine check and work halves of throwing instruction.
      MIR* work_half = mir->meta.throw_insn;
      mir->dalvikInsn.opcode = work_half->dalvikInsn.opcode;
      opcode = work_half->dalvikInsn.opcode;
      SSARepresentation* ssa_rep = work_half->ssa_rep;
      work_half->ssa_rep = mir->ssa_rep;
      mir->ssa_rep = ssa_rep;
      work_half->dalvikInsn.opcode = static_cast<Instruction::Code>(kMirOpCheckPart2);
    }

    if (opcode >= kMirOpFirst) {
      HandleExtendedMethodMIR(cu, bb, mir);
      continue;
    }

    CompileDalvikInstruction(cu, mir, bb, label_list);
  }

  if (head_lir) {
    // Eliminate redundant loads/stores and delay stores into later slots.
    ApplyLocalOptimizations(cu, head_lir, cu->last_lir_insn);

    // Generate an unconditional branch to the fallthrough block.
    if (bb->fall_through) {
      cg->OpUnconditionalBranch(cu, &label_list[bb->fall_through->id]);
    }
  }
  return false;
}

void SpecialMIR2LIR(CompilationUnit* cu, SpecialCaseHandler special_case)
{
  Codegen* cg = cu->cg.get();
  // Find the first DalvikByteCode block.
  int num_reachable_blocks = cu->num_reachable_blocks;
  const GrowableList *block_list = &cu->block_list;
  BasicBlock*bb = NULL;
  for (int idx = 0; idx < num_reachable_blocks; idx++) {
    int dfs_index = cu->dfs_order.elem_list[idx];
    bb = reinterpret_cast<BasicBlock*>(GrowableListGetElement(block_list, dfs_index));
    if (bb->block_type == kDalvikByteCode) {
      break;
    }
  }
  if (bb == NULL) {
    return;
  }
  DCHECK_EQ(bb->start_offset, 0);
  DCHECK(bb->first_mir_insn != NULL);

  // Get the first instruction.
  MIR* mir = bb->first_mir_insn;

  // Free temp registers and reset redundant store tracking.
  ResetRegPool(cu);
  ResetDefTracking(cu);
  ClobberAllRegs(cu);

  cg->GenSpecialCase(cu, bb, mir, special_case);
}

void MethodMIR2LIR(CompilationUnit* cu)
{
  Codegen* cg = cu->cg.get();
  // Hold the labels of each block.
  cu->block_label_list =
      static_cast<LIR*>(NewMem(cu, sizeof(LIR) * cu->num_blocks, true, kAllocLIR));

  DataFlowAnalysisDispatcher(cu, MethodBlockCodeGen,
                                kPreOrderDFSTraversal, false /* Iterative */);

  cg->HandleSuspendLaunchPads(cu);

  cg->HandleThrowLaunchPads(cu);

  cg->HandleIntrinsicLaunchPads(cu);

  if (!(cu->disable_opt & (1 << kSafeOptimizations))) {
    RemoveRedundantBranches(cu);
  }
}

}  // namespace art
