/*
 * Copyright (C) 2012 The Android Open Source Project
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

#include "interpreter_common.h"

namespace art {
namespace interpreter {

// In the following macros, we expect the following local variables exist:
// - "self": the current Thread*.
// - "inst" : the current Instruction*.
// - "inst_data" : the current instruction's first 16 bits.
// - "dex_pc": the current pc.
// - "shadow_frame": the current shadow frame.
// - "mh": the current MethodHelper.
// - "currentHandlersTable": the current table of pointer to each instruction handler.

// Advance to the next instruction and updates interpreter state.
#define ADVANCE(_offset)                                                    \
  do {                                                                      \
    int32_t disp = static_cast<int32_t>(_offset);                           \
    inst = inst->RelativeAt(disp);                                          \
    dex_pc = static_cast<uint32_t>(static_cast<int32_t>(dex_pc) + disp);    \
    shadow_frame.SetDexPC(dex_pc);                                          \
    TraceExecution(shadow_frame, inst, dex_pc, mh);                         \
    inst_data = inst->Fetch16(0);                                           \
    goto *currentHandlersTable[inst->Opcode(inst_data)];                    \
  } while (false)

#define HANDLE_PENDING_EXCEPTION() goto exception_pending_label

#define POSSIBLY_HANDLE_PENDING_EXCEPTION(_is_exception_pending, _offset)   \
  do {                                                                      \
    if (UNLIKELY(_is_exception_pending)) {                                  \
      HANDLE_PENDING_EXCEPTION();                                           \
    } else {                                                                \
      ADVANCE(_offset);                                                     \
    }                                                                       \
  } while (false)

#define UPDATE_HANDLER_TABLE() \
  currentHandlersTable = handlersTable[Runtime::Current()->GetInstrumentation()->GetInterpreterHandlerTable()]

#define UNREACHABLE_CODE_CHECK()                \
  do {                                          \
    if (kIsDebugBuild) {                        \
      LOG(FATAL) << "We should not be here !";  \
    }                                           \
  } while (false)

#define HANDLE_INSTRUCTION_START(opcode) op_##opcode:  // NOLINT(whitespace/labels)
#define HANDLE_INSTRUCTION_END() UNREACHABLE_CODE_CHECK()

/**
 * Interpreter based on computed goto tables.
 *
 * Each instruction is associated to a handler. This handler is responsible for executing the
 * instruction and jump to the next instruction's handler.
 * In order to limit the cost of instrumentation, we have two handler tables:
 * - the "main" handler table: it contains handlers for normal execution of each instruction without
 * handling of instrumentation.
 * - the "alternative" handler table: it contains alternative handlers which first handle
 * instrumentation before jumping to the corresponding "normal" instruction's handler.
 *
 * When instrumentation is active, the interpreter uses the "alternative" handler table. Otherwise
 * it uses the "main" handler table.
 *
 * The current handler table is the handler table being used by the interpreter. It is updated:
 * - on backward branch (goto, if and switch instructions)
 * - after invoke
 * - when an exception is thrown.
 * This allows to support an attaching debugger to an already running application for instance.
 *
 * For a fast handler table update, handler tables are stored in an array of handler tables. Each
 * handler table is represented by the InterpreterHandlerTable enum which allows to associate it
 * to an index in this array of handler tables ((see Instrumentation::GetInterpreterHandlerTable).
 *
 * Here's the current layout of this array of handler tables:
 *
 * ---------------------+---------------+
 *                      |     NOP       | (handler for NOP instruction)
 *                      +---------------+
 *       "main"         |     MOVE      | (handler for MOVE instruction)
 *    handler table     +---------------+
 *                      |      ...      |
 *                      +---------------+
 *                      |   UNUSED_FF   | (handler for UNUSED_FF instruction)
 * ---------------------+---------------+
 *                      |     NOP       | (alternative handler for NOP instruction)
 *                      +---------------+
 *    "alternative"     |     MOVE      | (alternative handler for MOVE instruction)
 *    handler table     +---------------+
 *                      |      ...      |
 *                      +---------------+
 *                      |   UNUSED_FF   | (alternative handler for UNUSED_FF instruction)
 * ---------------------+---------------+
 *
 */
template<bool do_access_check, bool transaction_active>
JValue ExecuteGotoImpl(Thread* self, MethodHelper& mh, const DexFile::CodeItem* code_item,
                       ShadowFrame& shadow_frame, JValue result_register) {
  // Define handler tables:
  // - The main handler table contains execution handlers for each instruction.
  // - The alternative handler table contains prelude handlers which check for thread suspend and
  //   manage instrumentation before jumping to the execution handler.
  static const void* const handlersTable[instrumentation::kNumHandlerTables][kNumPackedOpcodes] = {
    {
    // Main handler table.
#define INSTRUCTION_HANDLER(o, code, n, f, r, i, a, v) &&op_##code,
#include "dex_instruction_list.h"
      DEX_INSTRUCTION_LIST(INSTRUCTION_HANDLER)
#undef DEX_INSTRUCTION_LIST
#undef INSTRUCTION_HANDLER
    }, {
    // Alternative handler table.
#define INSTRUCTION_HANDLER(o, code, n, f, r, i, a, v) &&alt_op_##code,
#include "dex_instruction_list.h"
      DEX_INSTRUCTION_LIST(INSTRUCTION_HANDLER)
#undef DEX_INSTRUCTION_LIST
#undef INSTRUCTION_HANDLER
    }
  };

  const bool do_assignability_check = do_access_check;
  if (UNLIKELY(!shadow_frame.HasReferenceArray())) {
    LOG(FATAL) << "Invalid shadow frame for interpreter use";
    return JValue();
  }
  self->VerifyStack();

  uint32_t dex_pc = shadow_frame.GetDexPC();
  const Instruction* inst = Instruction::At(code_item->insns_ + dex_pc);
  uint16_t inst_data;
  const void* const* currentHandlersTable;
  bool notified_method_entry_event = false;
  UPDATE_HANDLER_TABLE();
  if (LIKELY(dex_pc == 0)) {  // We are entering the method as opposed to deoptimizing..
    instrumentation::Instrumentation* instrumentation = Runtime::Current()->GetInstrumentation();
    if (UNLIKELY(instrumentation->HasMethodEntryListeners())) {
      instrumentation->MethodEnterEvent(self, shadow_frame.GetThisObject(code_item->ins_size_),
                                        shadow_frame.GetMethod(), 0);
      notified_method_entry_event = true;
    }
  }

  // Jump to first instruction.
  ADVANCE(0);
  UNREACHABLE_CODE_CHECK();

  HANDLE_INSTRUCTION_START(NOP)
    ADVANCE(1);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(MOVE)
    shadow_frame.SetVReg(inst->VRegA_12x(inst_data),
                         shadow_frame.GetVReg(inst->VRegB_12x(inst_data)));
    ADVANCE(1);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(MOVE_FROM16)
    shadow_frame.SetVReg(inst->VRegA_22x(inst_data),
                         shadow_frame.GetVReg(inst->VRegB_22x()));
    ADVANCE(2);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(MOVE_16)
    shadow_frame.SetVReg(inst->VRegA_32x(),
                         shadow_frame.GetVReg(inst->VRegB_32x()));
    ADVANCE(3);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(MOVE_WIDE)
    shadow_frame.SetVRegLong(inst->VRegA_12x(inst_data),
                             shadow_frame.GetVRegLong(inst->VRegB_12x(inst_data)));
    ADVANCE(1);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(MOVE_WIDE_FROM16)
    shadow_frame.SetVRegLong(inst->VRegA_22x(inst_data),
                             shadow_frame.GetVRegLong(inst->VRegB_22x()));
    ADVANCE(2);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(MOVE_WIDE_16)
    shadow_frame.SetVRegLong(inst->VRegA_32x(),
                             shadow_frame.GetVRegLong(inst->VRegB_32x()));
    ADVANCE(3);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(MOVE_OBJECT)
    shadow_frame.SetVRegReference(inst->VRegA_12x(inst_data),
                                  shadow_frame.GetVRegReference(inst->VRegB_12x(inst_data)));
    ADVANCE(1);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(MOVE_OBJECT_FROM16)
    shadow_frame.SetVRegReference(inst->VRegA_22x(inst_data),
                                  shadow_frame.GetVRegReference(inst->VRegB_22x()));
    ADVANCE(2);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(MOVE_OBJECT_16)
    shadow_frame.SetVRegReference(inst->VRegA_32x(),
                                  shadow_frame.GetVRegReference(inst->VRegB_32x()));
    ADVANCE(3);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(MOVE_RESULT)
    shadow_frame.SetVReg(inst->VRegA_11x(inst_data), result_register.GetI());
    ADVANCE(1);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(MOVE_RESULT_WIDE)
    shadow_frame.SetVRegLong(inst->VRegA_11x(inst_data), result_register.GetJ());
    ADVANCE(1);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(MOVE_RESULT_OBJECT)
    shadow_frame.SetVRegReference(inst->VRegA_11x(inst_data), result_register.GetL());
    ADVANCE(1);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(MOVE_EXCEPTION) {
    Throwable* exception = self->GetException(NULL);
    self->ClearException();
    shadow_frame.SetVRegReference(inst->VRegA_11x(inst_data), exception);
    ADVANCE(1);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(RETURN_VOID) {
    JValue result;
    if (do_access_check) {
      // If access checks are required then the dex-to-dex compiler and analysis of
      // whether the class has final fields hasn't been performed. Conservatively
      // perform the memory barrier now.
      QuasiAtomic::MembarStoreLoad();
    }
    if (UNLIKELY(self->TestAllFlags())) {
      CheckSuspend(self);
    }
    instrumentation::Instrumentation* instrumentation = Runtime::Current()->GetInstrumentation();
    if (UNLIKELY(instrumentation->HasMethodExitListeners())) {
      instrumentation->MethodExitEvent(self, shadow_frame.GetThisObject(code_item->ins_size_),
                                       shadow_frame.GetMethod(), dex_pc,
                                       result);
    } else if (UNLIKELY(instrumentation->HasDexPcListeners())) {
      instrumentation->DexPcMovedEvent(self, shadow_frame.GetThisObject(code_item->ins_size_),
                                       shadow_frame.GetMethod(), dex_pc);
    }
    return result;
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(RETURN_VOID_BARRIER) {
    QuasiAtomic::MembarStoreLoad();
    JValue result;
    if (UNLIKELY(self->TestAllFlags())) {
      CheckSuspend(self);
    }
    instrumentation::Instrumentation* instrumentation = Runtime::Current()->GetInstrumentation();
    if (UNLIKELY(instrumentation->HasMethodExitListeners())) {
      instrumentation->MethodExitEvent(self, shadow_frame.GetThisObject(code_item->ins_size_),
                                       shadow_frame.GetMethod(), dex_pc,
                                       result);
    } else if (UNLIKELY(instrumentation->HasDexPcListeners())) {
      instrumentation->DexPcMovedEvent(self, shadow_frame.GetThisObject(code_item->ins_size_),
                                       shadow_frame.GetMethod(), dex_pc);
    }
    return result;
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(RETURN) {
    JValue result;
    result.SetJ(0);
    result.SetI(shadow_frame.GetVReg(inst->VRegA_11x(inst_data)));
    if (UNLIKELY(self->TestAllFlags())) {
      CheckSuspend(self);
    }
    instrumentation::Instrumentation* instrumentation = Runtime::Current()->GetInstrumentation();
    if (UNLIKELY(instrumentation->HasMethodExitListeners())) {
      instrumentation->MethodExitEvent(self, shadow_frame.GetThisObject(code_item->ins_size_),
                                       shadow_frame.GetMethod(), dex_pc,
                                       result);
    } else if (UNLIKELY(instrumentation->HasDexPcListeners())) {
      instrumentation->DexPcMovedEvent(self, shadow_frame.GetThisObject(code_item->ins_size_),
                                       shadow_frame.GetMethod(), dex_pc);
    }
    return result;
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(RETURN_WIDE) {
    JValue result;
    result.SetJ(shadow_frame.GetVRegLong(inst->VRegA_11x(inst_data)));
    if (UNLIKELY(self->TestAllFlags())) {
      CheckSuspend(self);
    }
    instrumentation::Instrumentation* instrumentation = Runtime::Current()->GetInstrumentation();
    if (UNLIKELY(instrumentation->HasMethodExitListeners())) {
      instrumentation->MethodExitEvent(self, shadow_frame.GetThisObject(code_item->ins_size_),
                                       shadow_frame.GetMethod(), dex_pc,
                                       result);
    } else if (UNLIKELY(instrumentation->HasDexPcListeners())) {
      instrumentation->DexPcMovedEvent(self, shadow_frame.GetThisObject(code_item->ins_size_),
                                       shadow_frame.GetMethod(), dex_pc);
    }
    return result;
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(RETURN_OBJECT) {
    JValue result;
    if (UNLIKELY(self->TestAllFlags())) {
      CheckSuspend(self);
    }
    Object* obj_result = shadow_frame.GetVRegReference(inst->VRegA_11x(inst_data));
    result.SetJ(0);
    result.SetL(obj_result);
    if (do_assignability_check && obj_result != NULL) {
      Class* return_type = MethodHelper(shadow_frame.GetMethod()).GetReturnType();
      if (return_type == NULL) {
        // Return the pending exception.
        HANDLE_PENDING_EXCEPTION();
      }
      if (!obj_result->VerifierInstanceOf(return_type)) {
        // This should never happen.
        self->ThrowNewExceptionF(self->GetCurrentLocationForThrow(),
                                 "Ljava/lang/VirtualMachineError;",
                                 "Returning '%s' that is not instance of return type '%s'",
                                 obj_result->GetClass()->GetDescriptor().c_str(),
                                 return_type->GetDescriptor().c_str());
        HANDLE_PENDING_EXCEPTION();
      }
    }
    instrumentation::Instrumentation* instrumentation = Runtime::Current()->GetInstrumentation();
    if (UNLIKELY(instrumentation->HasMethodExitListeners())) {
      instrumentation->MethodExitEvent(self, shadow_frame.GetThisObject(code_item->ins_size_),
                                       shadow_frame.GetMethod(), dex_pc,
                                       result);
    } else if (UNLIKELY(instrumentation->HasDexPcListeners())) {
      instrumentation->DexPcMovedEvent(self, shadow_frame.GetThisObject(code_item->ins_size_),
                                       shadow_frame.GetMethod(), dex_pc);
    }
    return result;
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(CONST_4) {
    uint32_t dst = inst->VRegA_11n(inst_data);
    int32_t val = inst->VRegB_11n(inst_data);
    shadow_frame.SetVReg(dst, val);
    if (val == 0) {
      shadow_frame.SetVRegReference(dst, NULL);
    }
    ADVANCE(1);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(CONST_16) {
    uint32_t dst = inst->VRegA_21s(inst_data);
    int32_t val = inst->VRegB_21s();
    shadow_frame.SetVReg(dst, val);
    if (val == 0) {
      shadow_frame.SetVRegReference(dst, NULL);
    }
    ADVANCE(2);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(CONST) {
    uint32_t dst = inst->VRegA_31i(inst_data);
    int32_t val = inst->VRegB_31i();
    shadow_frame.SetVReg(dst, val);
    if (val == 0) {
      shadow_frame.SetVRegReference(dst, NULL);
    }
    ADVANCE(3);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(CONST_HIGH16) {
    uint32_t dst = inst->VRegA_21h(inst_data);
    int32_t val = static_cast<int32_t>(inst->VRegB_21h() << 16);
    shadow_frame.SetVReg(dst, val);
    if (val == 0) {
      shadow_frame.SetVRegReference(dst, NULL);
    }
    ADVANCE(2);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(CONST_WIDE_16)
    shadow_frame.SetVRegLong(inst->VRegA_21s(inst_data), inst->VRegB_21s());
    ADVANCE(2);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(CONST_WIDE_32)
    shadow_frame.SetVRegLong(inst->VRegA_31i(inst_data), inst->VRegB_31i());
    ADVANCE(3);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(CONST_WIDE)
    shadow_frame.SetVRegLong(inst->VRegA_51l(inst_data), inst->VRegB_51l());
    ADVANCE(5);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(CONST_WIDE_HIGH16)
    shadow_frame.SetVRegLong(inst->VRegA_21h(inst_data),
                             static_cast<uint64_t>(inst->VRegB_21h()) << 48);
    ADVANCE(2);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(CONST_STRING) {
    String* s = ResolveString(self, mh, inst->VRegB_21c());
    if (UNLIKELY(s == NULL)) {
      HANDLE_PENDING_EXCEPTION();
    } else {
      shadow_frame.SetVRegReference(inst->VRegA_21c(inst_data), s);
      ADVANCE(2);
    }
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(CONST_STRING_JUMBO) {
    String* s = ResolveString(self, mh, inst->VRegB_31c());
    if (UNLIKELY(s == NULL)) {
      HANDLE_PENDING_EXCEPTION();
    } else {
      shadow_frame.SetVRegReference(inst->VRegA_31c(inst_data), s);
      ADVANCE(3);
    }
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(CONST_CLASS) {
    Class* c = ResolveVerifyAndClinit(inst->VRegB_21c(), shadow_frame.GetMethod(),
                                      self, false, do_access_check);
    if (UNLIKELY(c == NULL)) {
      HANDLE_PENDING_EXCEPTION();
    } else {
      shadow_frame.SetVRegReference(inst->VRegA_21c(inst_data), c);
      ADVANCE(2);
    }
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(MONITOR_ENTER) {
    Object* obj = shadow_frame.GetVRegReference(inst->VRegA_11x(inst_data));
    if (UNLIKELY(obj == NULL)) {
      ThrowNullPointerExceptionFromDexPC(shadow_frame.GetCurrentLocationForThrow());
      HANDLE_PENDING_EXCEPTION();
    } else {
      DoMonitorEnter(self, obj);
      POSSIBLY_HANDLE_PENDING_EXCEPTION(self->IsExceptionPending(), 1);
    }
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(MONITOR_EXIT) {
    Object* obj = shadow_frame.GetVRegReference(inst->VRegA_11x(inst_data));
    if (UNLIKELY(obj == NULL)) {
      ThrowNullPointerExceptionFromDexPC(shadow_frame.GetCurrentLocationForThrow());
      HANDLE_PENDING_EXCEPTION();
    } else {
      DoMonitorExit(self, obj);
      POSSIBLY_HANDLE_PENDING_EXCEPTION(self->IsExceptionPending(), 1);
    }
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(CHECK_CAST) {
    Class* c = ResolveVerifyAndClinit(inst->VRegB_21c(), shadow_frame.GetMethod(),
                                      self, false, do_access_check);
    if (UNLIKELY(c == NULL)) {
      HANDLE_PENDING_EXCEPTION();
    } else {
      Object* obj = shadow_frame.GetVRegReference(inst->VRegA_21c(inst_data));
      if (UNLIKELY(obj != NULL && !obj->InstanceOf(c))) {
        ThrowClassCastException(c, obj->GetClass());
        HANDLE_PENDING_EXCEPTION();
      } else {
        ADVANCE(2);
      }
    }
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(INSTANCE_OF) {
    Class* c = ResolveVerifyAndClinit(inst->VRegC_22c(), shadow_frame.GetMethod(),
                                      self, false, do_access_check);
    if (UNLIKELY(c == NULL)) {
      HANDLE_PENDING_EXCEPTION();
    } else {
      Object* obj = shadow_frame.GetVRegReference(inst->VRegB_22c(inst_data));
      shadow_frame.SetVReg(inst->VRegA_22c(inst_data), (obj != NULL && obj->InstanceOf(c)) ? 1 : 0);
      ADVANCE(2);
    }
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(ARRAY_LENGTH) {
    Object* array = shadow_frame.GetVRegReference(inst->VRegB_12x(inst_data));
    if (UNLIKELY(array == NULL)) {
      ThrowNullPointerExceptionFromDexPC(shadow_frame.GetCurrentLocationForThrow());
      HANDLE_PENDING_EXCEPTION();
    } else {
      shadow_frame.SetVReg(inst->VRegA_12x(inst_data), array->AsArray()->GetLength());
      ADVANCE(1);
    }
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(NEW_INSTANCE) {
    Runtime* runtime = Runtime::Current();
    Object* obj = AllocObjectFromCode<do_access_check, true>(
        inst->VRegB_21c(), shadow_frame.GetMethod(), self,
        runtime->GetHeap()->GetCurrentAllocator());
    if (UNLIKELY(obj == NULL)) {
      HANDLE_PENDING_EXCEPTION();
    } else {
      // Don't allow finalizable objects to be allocated during a transaction since these can't be
      // finalized without a started runtime.
      if (transaction_active && obj->GetClass()->IsFinalizable()) {
        AbortTransaction(self, "Allocating finalizable object in transaction: %s",
                         PrettyTypeOf(obj).c_str());
        HANDLE_PENDING_EXCEPTION();
      }
      shadow_frame.SetVRegReference(inst->VRegA_21c(inst_data), obj);
      ADVANCE(2);
    }
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(NEW_ARRAY) {
    int32_t length = shadow_frame.GetVReg(inst->VRegB_22c(inst_data));
    Object* obj = AllocArrayFromCode<do_access_check, true>(
        inst->VRegC_22c(), shadow_frame.GetMethod(), length, self,
        Runtime::Current()->GetHeap()->GetCurrentAllocator());
    if (UNLIKELY(obj == NULL)) {
      HANDLE_PENDING_EXCEPTION();
    } else {
      shadow_frame.SetVRegReference(inst->VRegA_22c(inst_data), obj);
      ADVANCE(2);
    }
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(FILLED_NEW_ARRAY) {
    bool success =
        DoFilledNewArray<false, do_access_check, transaction_active>(inst, shadow_frame,
                                                                     self, &result_register);
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 3);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(FILLED_NEW_ARRAY_RANGE) {
    bool success =
        DoFilledNewArray<true, do_access_check, transaction_active>(inst, shadow_frame,
                                                                    self, &result_register);
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 3);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(FILL_ARRAY_DATA) {
    Object* obj = shadow_frame.GetVRegReference(inst->VRegA_31t(inst_data));
    if (UNLIKELY(obj == NULL)) {
      ThrowNullPointerException(NULL, "null array in FILL_ARRAY_DATA");
      HANDLE_PENDING_EXCEPTION();
    } else {
      Array* array = obj->AsArray();
      DCHECK(array->IsArrayInstance() && !array->IsObjectArray());
      const uint16_t* payload_addr = reinterpret_cast<const uint16_t*>(inst) + inst->VRegB_31t();
      const Instruction::ArrayDataPayload* payload =
          reinterpret_cast<const Instruction::ArrayDataPayload*>(payload_addr);
      if (UNLIKELY(static_cast<int32_t>(payload->element_count) > array->GetLength())) {
        self->ThrowNewExceptionF(shadow_frame.GetCurrentLocationForThrow(),
                                 "Ljava/lang/ArrayIndexOutOfBoundsException;",
                                 "failed FILL_ARRAY_DATA; length=%d, index=%d",
                                 array->GetLength(), payload->element_count);
        HANDLE_PENDING_EXCEPTION();
      } else {
        if (transaction_active) {
          RecordArrayElementsInTransaction(array, payload->element_count);
        }
        uint32_t size_in_bytes = payload->element_count * payload->element_width;
        memcpy(array->GetRawData(payload->element_width, 0), payload->data, size_in_bytes);
        ADVANCE(3);
      }
    }
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(THROW) {
    Object* exception = shadow_frame.GetVRegReference(inst->VRegA_11x(inst_data));
    if (UNLIKELY(exception == NULL)) {
      ThrowNullPointerException(NULL, "throw with null exception");
    } else if (do_assignability_check && !exception->GetClass()->IsThrowableClass()) {
      // This should never happen.
      self->ThrowNewExceptionF(self->GetCurrentLocationForThrow(),
                               "Ljava/lang/VirtualMachineError;",
                               "Throwing '%s' that is not instance of Throwable",
                               exception->GetClass()->GetDescriptor().c_str());
    } else {
      self->SetException(shadow_frame.GetCurrentLocationForThrow(), exception->AsThrowable());
    }
    HANDLE_PENDING_EXCEPTION();
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(GOTO) {
    int8_t offset = inst->VRegA_10t(inst_data);
    if (IsBackwardBranch(offset)) {
      if (UNLIKELY(self->TestAllFlags())) {
        CheckSuspend(self);
        UPDATE_HANDLER_TABLE();
      }
    }
    ADVANCE(offset);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(GOTO_16) {
    int16_t offset = inst->VRegA_20t();
    if (IsBackwardBranch(offset)) {
      if (UNLIKELY(self->TestAllFlags())) {
        CheckSuspend(self);
        UPDATE_HANDLER_TABLE();
      }
    }
    ADVANCE(offset);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(GOTO_32) {
    int32_t offset = inst->VRegA_30t();
    if (IsBackwardBranch(offset)) {
      if (UNLIKELY(self->TestAllFlags())) {
        CheckSuspend(self);
        UPDATE_HANDLER_TABLE();
      }
    }
    ADVANCE(offset);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(PACKED_SWITCH) {
    int32_t offset = DoPackedSwitch(inst, shadow_frame, inst_data);
    if (IsBackwardBranch(offset)) {
      if (UNLIKELY(self->TestAllFlags())) {
        CheckSuspend(self);
        UPDATE_HANDLER_TABLE();
      }
    }
    ADVANCE(offset);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(SPARSE_SWITCH) {
    int32_t offset = DoSparseSwitch(inst, shadow_frame, inst_data);
    if (IsBackwardBranch(offset)) {
      if (UNLIKELY(self->TestAllFlags())) {
        CheckSuspend(self);
        UPDATE_HANDLER_TABLE();
      }
    }
    ADVANCE(offset);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(CMPL_FLOAT) {
    float val1 = shadow_frame.GetVRegFloat(inst->VRegB_23x());
    float val2 = shadow_frame.GetVRegFloat(inst->VRegC_23x());
    int32_t result;
    if (val1 > val2) {
      result = 1;
    } else if (val1 == val2) {
      result = 0;
    } else {
      result = -1;
    }
    shadow_frame.SetVReg(inst->VRegA_23x(inst_data), result);
    ADVANCE(2);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(CMPG_FLOAT) {
    float val1 = shadow_frame.GetVRegFloat(inst->VRegB_23x());
    float val2 = shadow_frame.GetVRegFloat(inst->VRegC_23x());
    int32_t result;
    if (val1 < val2) {
      result = -1;
    } else if (val1 == val2) {
      result = 0;
    } else {
      result = 1;
    }
    shadow_frame.SetVReg(inst->VRegA_23x(inst_data), result);
    ADVANCE(2);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(CMPL_DOUBLE) {
    double val1 = shadow_frame.GetVRegDouble(inst->VRegB_23x());
    double val2 = shadow_frame.GetVRegDouble(inst->VRegC_23x());
    int32_t result;
    if (val1 > val2) {
      result = 1;
    } else if (val1 == val2) {
      result = 0;
    } else {
      result = -1;
    }
    shadow_frame.SetVReg(inst->VRegA_23x(inst_data), result);
    ADVANCE(2);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(CMPG_DOUBLE) {
    double val1 = shadow_frame.GetVRegDouble(inst->VRegB_23x());
    double val2 = shadow_frame.GetVRegDouble(inst->VRegC_23x());
    int32_t result;
    if (val1 < val2) {
      result = -1;
    } else if (val1 == val2) {
      result = 0;
    } else {
      result = 1;
    }
    shadow_frame.SetVReg(inst->VRegA_23x(inst_data), result);
    ADVANCE(2);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(CMP_LONG) {
    int64_t val1 = shadow_frame.GetVRegLong(inst->VRegB_23x());
    int64_t val2 = shadow_frame.GetVRegLong(inst->VRegC_23x());
    int32_t result;
    if (val1 > val2) {
      result = 1;
    } else if (val1 == val2) {
      result = 0;
    } else {
      result = -1;
    }
    shadow_frame.SetVReg(inst->VRegA_23x(inst_data), result);
    ADVANCE(2);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(IF_EQ) {
    if (shadow_frame.GetVReg(inst->VRegA_22t(inst_data)) == shadow_frame.GetVReg(inst->VRegB_22t(inst_data))) {
      int16_t offset = inst->VRegC_22t();
      if (IsBackwardBranch(offset)) {
        if (UNLIKELY(self->TestAllFlags())) {
          CheckSuspend(self);
          UPDATE_HANDLER_TABLE();
        }
      }
      ADVANCE(offset);
    } else {
      ADVANCE(2);
    }
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(IF_NE) {
    if (shadow_frame.GetVReg(inst->VRegA_22t(inst_data)) != shadow_frame.GetVReg(inst->VRegB_22t(inst_data))) {
      int16_t offset = inst->VRegC_22t();
      if (IsBackwardBranch(offset)) {
        if (UNLIKELY(self->TestAllFlags())) {
          CheckSuspend(self);
          UPDATE_HANDLER_TABLE();
        }
      }
      ADVANCE(offset);
    } else {
      ADVANCE(2);
    }
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(IF_LT) {
    if (shadow_frame.GetVReg(inst->VRegA_22t(inst_data)) < shadow_frame.GetVReg(inst->VRegB_22t(inst_data))) {
      int16_t offset = inst->VRegC_22t();
      if (IsBackwardBranch(offset)) {
        if (UNLIKELY(self->TestAllFlags())) {
          CheckSuspend(self);
          UPDATE_HANDLER_TABLE();
        }
      }
      ADVANCE(offset);
    } else {
      ADVANCE(2);
    }
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(IF_GE) {
    if (shadow_frame.GetVReg(inst->VRegA_22t(inst_data)) >= shadow_frame.GetVReg(inst->VRegB_22t(inst_data))) {
      int16_t offset = inst->VRegC_22t();
      if (IsBackwardBranch(offset)) {
        if (UNLIKELY(self->TestAllFlags())) {
          CheckSuspend(self);
          UPDATE_HANDLER_TABLE();
        }
      }
      ADVANCE(offset);
    } else {
      ADVANCE(2);
    }
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(IF_GT) {
    if (shadow_frame.GetVReg(inst->VRegA_22t(inst_data)) > shadow_frame.GetVReg(inst->VRegB_22t(inst_data))) {
      int16_t offset = inst->VRegC_22t();
      if (IsBackwardBranch(offset)) {
        if (UNLIKELY(self->TestAllFlags())) {
          CheckSuspend(self);
          UPDATE_HANDLER_TABLE();
        }
      }
      ADVANCE(offset);
    } else {
      ADVANCE(2);
    }
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(IF_LE) {
    if (shadow_frame.GetVReg(inst->VRegA_22t(inst_data)) <= shadow_frame.GetVReg(inst->VRegB_22t(inst_data))) {
      int16_t offset = inst->VRegC_22t();
      if (IsBackwardBranch(offset)) {
        if (UNLIKELY(self->TestAllFlags())) {
          CheckSuspend(self);
          UPDATE_HANDLER_TABLE();
        }
      }
      ADVANCE(offset);
    } else {
      ADVANCE(2);
    }
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(IF_EQZ) {
    if (shadow_frame.GetVReg(inst->VRegA_21t(inst_data)) == 0) {
      int16_t offset = inst->VRegB_21t();
      if (IsBackwardBranch(offset)) {
        if (UNLIKELY(self->TestAllFlags())) {
          CheckSuspend(self);
          UPDATE_HANDLER_TABLE();
        }
      }
      ADVANCE(offset);
    } else {
      ADVANCE(2);
    }
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(IF_NEZ) {
    if (shadow_frame.GetVReg(inst->VRegA_21t(inst_data)) != 0) {
      int16_t offset = inst->VRegB_21t();
      if (IsBackwardBranch(offset)) {
        if (UNLIKELY(self->TestAllFlags())) {
          CheckSuspend(self);
          UPDATE_HANDLER_TABLE();
        }
      }
      ADVANCE(offset);
    } else {
      ADVANCE(2);
    }
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(IF_LTZ) {
    if (shadow_frame.GetVReg(inst->VRegA_21t(inst_data)) < 0) {
      int16_t offset = inst->VRegB_21t();
      if (IsBackwardBranch(offset)) {
        if (UNLIKELY(self->TestAllFlags())) {
          CheckSuspend(self);
          UPDATE_HANDLER_TABLE();
        }
      }
      ADVANCE(offset);
    } else {
      ADVANCE(2);
    }
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(IF_GEZ) {
    if (shadow_frame.GetVReg(inst->VRegA_21t(inst_data)) >= 0) {
      int16_t offset = inst->VRegB_21t();
      if (IsBackwardBranch(offset)) {
        if (UNLIKELY(self->TestAllFlags())) {
          CheckSuspend(self);
          UPDATE_HANDLER_TABLE();
        }
      }
      ADVANCE(offset);
    } else {
      ADVANCE(2);
    }
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(IF_GTZ) {
    if (shadow_frame.GetVReg(inst->VRegA_21t(inst_data)) > 0) {
      int16_t offset = inst->VRegB_21t();
      if (IsBackwardBranch(offset)) {
        if (UNLIKELY(self->TestAllFlags())) {
          CheckSuspend(self);
          UPDATE_HANDLER_TABLE();
        }
      }
      ADVANCE(offset);
    } else {
      ADVANCE(2);
    }
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(IF_LEZ)  {
    if (shadow_frame.GetVReg(inst->VRegA_21t(inst_data)) <= 0) {
      int16_t offset = inst->VRegB_21t();
      if (IsBackwardBranch(offset)) {
        if (UNLIKELY(self->TestAllFlags())) {
          CheckSuspend(self);
          UPDATE_HANDLER_TABLE();
        }
      }
      ADVANCE(offset);
    } else {
      ADVANCE(2);
    }
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(AGET_BOOLEAN) {
    Object* a = shadow_frame.GetVRegReference(inst->VRegB_23x());
    if (UNLIKELY(a == NULL)) {
      ThrowNullPointerExceptionFromDexPC(shadow_frame.GetCurrentLocationForThrow());
      HANDLE_PENDING_EXCEPTION();
    } else {
      int32_t index = shadow_frame.GetVReg(inst->VRegC_23x());
      BooleanArray* array = a->AsBooleanArray();
      if (LIKELY(array->CheckIsValidIndex(index))) {
        shadow_frame.SetVReg(inst->VRegA_23x(inst_data), array->GetWithoutChecks(index));
        ADVANCE(2);
      } else {
        HANDLE_PENDING_EXCEPTION();
      }
    }
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(AGET_BYTE) {
    Object* a = shadow_frame.GetVRegReference(inst->VRegB_23x());
    if (UNLIKELY(a == NULL)) {
      ThrowNullPointerExceptionFromDexPC(shadow_frame.GetCurrentLocationForThrow());
      HANDLE_PENDING_EXCEPTION();
    } else {
      int32_t index = shadow_frame.GetVReg(inst->VRegC_23x());
      ByteArray* array = a->AsByteArray();
      if (LIKELY(array->CheckIsValidIndex(index))) {
        shadow_frame.SetVReg(inst->VRegA_23x(inst_data), array->GetWithoutChecks(index));
        ADVANCE(2);
      } else {
        HANDLE_PENDING_EXCEPTION();
      }
    }
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(AGET_CHAR) {
    Object* a = shadow_frame.GetVRegReference(inst->VRegB_23x());
    if (UNLIKELY(a == NULL)) {
      ThrowNullPointerExceptionFromDexPC(shadow_frame.GetCurrentLocationForThrow());
      HANDLE_PENDING_EXCEPTION();
    } else {
      int32_t index = shadow_frame.GetVReg(inst->VRegC_23x());
      CharArray* array = a->AsCharArray();
      if (LIKELY(array->CheckIsValidIndex(index))) {
        shadow_frame.SetVReg(inst->VRegA_23x(inst_data), array->GetWithoutChecks(index));
        ADVANCE(2);
      } else {
        HANDLE_PENDING_EXCEPTION();
      }
    }
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(AGET_SHORT) {
    Object* a = shadow_frame.GetVRegReference(inst->VRegB_23x());
    if (UNLIKELY(a == NULL)) {
      ThrowNullPointerExceptionFromDexPC(shadow_frame.GetCurrentLocationForThrow());
      HANDLE_PENDING_EXCEPTION();
    } else {
      int32_t index = shadow_frame.GetVReg(inst->VRegC_23x());
      ShortArray* array = a->AsShortArray();
      if (LIKELY(array->CheckIsValidIndex(index))) {
        shadow_frame.SetVReg(inst->VRegA_23x(inst_data), array->GetWithoutChecks(index));
        ADVANCE(2);
      } else {
        HANDLE_PENDING_EXCEPTION();
      }
    }
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(AGET) {
    Object* a = shadow_frame.GetVRegReference(inst->VRegB_23x());
    if (UNLIKELY(a == NULL)) {
      ThrowNullPointerExceptionFromDexPC(shadow_frame.GetCurrentLocationForThrow());
      HANDLE_PENDING_EXCEPTION();
    } else {
      int32_t index = shadow_frame.GetVReg(inst->VRegC_23x());
      IntArray* array = a->AsIntArray();
      if (LIKELY(array->CheckIsValidIndex(index))) {
        shadow_frame.SetVReg(inst->VRegA_23x(inst_data), array->GetWithoutChecks(index));
        ADVANCE(2);
      } else {
        HANDLE_PENDING_EXCEPTION();
      }
    }
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(AGET_WIDE)  {
    Object* a = shadow_frame.GetVRegReference(inst->VRegB_23x());
    if (UNLIKELY(a == NULL)) {
      ThrowNullPointerExceptionFromDexPC(shadow_frame.GetCurrentLocationForThrow());
      HANDLE_PENDING_EXCEPTION();
    } else {
      int32_t index = shadow_frame.GetVReg(inst->VRegC_23x());
      LongArray* array = a->AsLongArray();
      if (LIKELY(array->CheckIsValidIndex(index))) {
        shadow_frame.SetVRegLong(inst->VRegA_23x(inst_data), array->GetWithoutChecks(index));
        ADVANCE(2);
      } else {
        HANDLE_PENDING_EXCEPTION();
      }
    }
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(AGET_OBJECT) {
    Object* a = shadow_frame.GetVRegReference(inst->VRegB_23x());
    if (UNLIKELY(a == NULL)) {
      ThrowNullPointerExceptionFromDexPC(shadow_frame.GetCurrentLocationForThrow());
      HANDLE_PENDING_EXCEPTION();
    } else {
      int32_t index = shadow_frame.GetVReg(inst->VRegC_23x());
      ObjectArray<Object>* array = a->AsObjectArray<Object>();
      if (LIKELY(array->CheckIsValidIndex(index))) {
        shadow_frame.SetVRegReference(inst->VRegA_23x(inst_data), array->GetWithoutChecks(index));
        ADVANCE(2);
      } else {
        HANDLE_PENDING_EXCEPTION();
      }
    }
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(APUT_BOOLEAN) {
    Object* a = shadow_frame.GetVRegReference(inst->VRegB_23x());
    if (UNLIKELY(a == NULL)) {
      ThrowNullPointerExceptionFromDexPC(shadow_frame.GetCurrentLocationForThrow());
      HANDLE_PENDING_EXCEPTION();
    } else {
      uint8_t val = shadow_frame.GetVReg(inst->VRegA_23x(inst_data));
      int32_t index = shadow_frame.GetVReg(inst->VRegC_23x());
      BooleanArray* array = a->AsBooleanArray();
      if (LIKELY(array->CheckIsValidIndex(index))) {
        array->SetWithoutChecks<transaction_active>(index, val);
        ADVANCE(2);
      } else {
        HANDLE_PENDING_EXCEPTION();
      }
    }
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(APUT_BYTE) {
    Object* a = shadow_frame.GetVRegReference(inst->VRegB_23x());
    if (UNLIKELY(a == NULL)) {
      ThrowNullPointerExceptionFromDexPC(shadow_frame.GetCurrentLocationForThrow());
      HANDLE_PENDING_EXCEPTION();
    } else {
      int8_t val = shadow_frame.GetVReg(inst->VRegA_23x(inst_data));
      int32_t index = shadow_frame.GetVReg(inst->VRegC_23x());
      ByteArray* array = a->AsByteArray();
      if (LIKELY(array->CheckIsValidIndex(index))) {
        array->SetWithoutChecks<transaction_active>(index, val);
        ADVANCE(2);
      } else {
        HANDLE_PENDING_EXCEPTION();
      }
    }
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(APUT_CHAR) {
    Object* a = shadow_frame.GetVRegReference(inst->VRegB_23x());
    if (UNLIKELY(a == NULL)) {
      ThrowNullPointerExceptionFromDexPC(shadow_frame.GetCurrentLocationForThrow());
      HANDLE_PENDING_EXCEPTION();
    } else {
      uint16_t val = shadow_frame.GetVReg(inst->VRegA_23x(inst_data));
      int32_t index = shadow_frame.GetVReg(inst->VRegC_23x());
      CharArray* array = a->AsCharArray();
      if (LIKELY(array->CheckIsValidIndex(index))) {
        array->SetWithoutChecks<transaction_active>(index, val);
        ADVANCE(2);
      } else {
        HANDLE_PENDING_EXCEPTION();
      }
    }
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(APUT_SHORT) {
    Object* a = shadow_frame.GetVRegReference(inst->VRegB_23x());
    if (UNLIKELY(a == NULL)) {
      ThrowNullPointerExceptionFromDexPC(shadow_frame.GetCurrentLocationForThrow());
      HANDLE_PENDING_EXCEPTION();
    } else {
      int16_t val = shadow_frame.GetVReg(inst->VRegA_23x(inst_data));
      int32_t index = shadow_frame.GetVReg(inst->VRegC_23x());
      ShortArray* array = a->AsShortArray();
      if (LIKELY(array->CheckIsValidIndex(index))) {
        array->SetWithoutChecks<transaction_active>(index, val);
        ADVANCE(2);
      } else {
        HANDLE_PENDING_EXCEPTION();
      }
    }
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(APUT) {
    Object* a = shadow_frame.GetVRegReference(inst->VRegB_23x());
    if (UNLIKELY(a == NULL)) {
      ThrowNullPointerExceptionFromDexPC(shadow_frame.GetCurrentLocationForThrow());
      HANDLE_PENDING_EXCEPTION();
    } else {
      int32_t val = shadow_frame.GetVReg(inst->VRegA_23x(inst_data));
      int32_t index = shadow_frame.GetVReg(inst->VRegC_23x());
      IntArray* array = a->AsIntArray();
      if (LIKELY(array->CheckIsValidIndex(index))) {
        array->SetWithoutChecks<transaction_active>(index, val);
        ADVANCE(2);
      } else {
        HANDLE_PENDING_EXCEPTION();
      }
    }
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(APUT_WIDE) {
    Object* a = shadow_frame.GetVRegReference(inst->VRegB_23x());
    if (UNLIKELY(a == NULL)) {
      ThrowNullPointerExceptionFromDexPC(shadow_frame.GetCurrentLocationForThrow());
      HANDLE_PENDING_EXCEPTION();
    } else {
      int64_t val = shadow_frame.GetVRegLong(inst->VRegA_23x(inst_data));
      int32_t index = shadow_frame.GetVReg(inst->VRegC_23x());
      LongArray* array = a->AsLongArray();
      if (LIKELY(array->CheckIsValidIndex(index))) {
        array->SetWithoutChecks<transaction_active>(index, val);
        ADVANCE(2);
      } else {
        HANDLE_PENDING_EXCEPTION();
      }
    }
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(APUT_OBJECT) {
    Object* a = shadow_frame.GetVRegReference(inst->VRegB_23x());
    if (UNLIKELY(a == NULL)) {
      ThrowNullPointerExceptionFromDexPC(shadow_frame.GetCurrentLocationForThrow());
      HANDLE_PENDING_EXCEPTION();
    } else {
      int32_t index = shadow_frame.GetVReg(inst->VRegC_23x());
      Object* val = shadow_frame.GetVRegReference(inst->VRegA_23x(inst_data));
      ObjectArray<Object>* array = a->AsObjectArray<Object>();
      if (LIKELY(array->CheckIsValidIndex(index) && array->CheckAssignable(val))) {
        array->SetWithoutChecks<transaction_active>(index, val);
        ADVANCE(2);
      } else {
        HANDLE_PENDING_EXCEPTION();
      }
    }
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(IGET_BOOLEAN) {
    bool success = DoFieldGet<InstancePrimitiveRead, Primitive::kPrimBoolean, do_access_check>(self, shadow_frame, inst, inst_data);
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 2);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(IGET_BYTE) {
    bool success = DoFieldGet<InstancePrimitiveRead, Primitive::kPrimByte, do_access_check>(self, shadow_frame, inst, inst_data);
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 2);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(IGET_CHAR) {
    bool success = DoFieldGet<InstancePrimitiveRead, Primitive::kPrimChar, do_access_check>(self, shadow_frame, inst, inst_data);
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 2);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(IGET_SHORT) {
    bool success = DoFieldGet<InstancePrimitiveRead, Primitive::kPrimShort, do_access_check>(self, shadow_frame, inst, inst_data);
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 2);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(IGET) {
    bool success = DoFieldGet<InstancePrimitiveRead, Primitive::kPrimInt, do_access_check>(self, shadow_frame, inst, inst_data);
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 2);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(IGET_WIDE) {
    bool success = DoFieldGet<InstancePrimitiveRead, Primitive::kPrimLong, do_access_check>(self, shadow_frame, inst, inst_data);
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 2);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(IGET_OBJECT) {
    bool success = DoFieldGet<InstanceObjectRead, Primitive::kPrimNot, do_access_check>(self, shadow_frame, inst, inst_data);
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 2);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(IGET_QUICK) {
    bool success = DoIGetQuick<Primitive::kPrimInt>(shadow_frame, inst, inst_data);
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 2);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(IGET_WIDE_QUICK) {
    bool success = DoIGetQuick<Primitive::kPrimLong>(shadow_frame, inst, inst_data);
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 2);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(IGET_OBJECT_QUICK) {
    bool success = DoIGetQuick<Primitive::kPrimNot>(shadow_frame, inst, inst_data);
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 2);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(SGET_BOOLEAN) {
    bool success = DoFieldGet<StaticPrimitiveRead, Primitive::kPrimBoolean, do_access_check>(self, shadow_frame, inst, inst_data);
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 2);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(SGET_BYTE) {
    bool success = DoFieldGet<StaticPrimitiveRead, Primitive::kPrimByte, do_access_check>(self, shadow_frame, inst, inst_data);
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 2);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(SGET_CHAR) {
    bool success = DoFieldGet<StaticPrimitiveRead, Primitive::kPrimChar, do_access_check>(self, shadow_frame, inst, inst_data);
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 2);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(SGET_SHORT) {
    bool success = DoFieldGet<StaticPrimitiveRead, Primitive::kPrimShort, do_access_check>(self, shadow_frame, inst, inst_data);
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 2);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(SGET) {
    bool success = DoFieldGet<StaticPrimitiveRead, Primitive::kPrimInt, do_access_check>(self, shadow_frame, inst, inst_data);
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 2);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(SGET_WIDE) {
    bool success = DoFieldGet<StaticPrimitiveRead, Primitive::kPrimLong, do_access_check>(self, shadow_frame, inst, inst_data);
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 2);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(SGET_OBJECT) {
    bool success = DoFieldGet<StaticObjectRead, Primitive::kPrimNot, do_access_check>(self, shadow_frame, inst, inst_data);
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 2);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(IPUT_BOOLEAN) {
    bool success = DoFieldPut<InstancePrimitiveWrite, Primitive::kPrimBoolean, do_access_check, transaction_active>(self, shadow_frame, inst, inst_data);
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 2);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(IPUT_BYTE) {
    bool success = DoFieldPut<InstancePrimitiveWrite, Primitive::kPrimByte, do_access_check, transaction_active>(self, shadow_frame, inst, inst_data);
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 2);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(IPUT_CHAR) {
    bool success = DoFieldPut<InstancePrimitiveWrite, Primitive::kPrimChar, do_access_check, transaction_active>(self, shadow_frame, inst, inst_data);
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 2);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(IPUT_SHORT) {
    bool success = DoFieldPut<InstancePrimitiveWrite, Primitive::kPrimShort, do_access_check, transaction_active>(self, shadow_frame, inst, inst_data);
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 2);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(IPUT) {
    bool success = DoFieldPut<InstancePrimitiveWrite, Primitive::kPrimInt, do_access_check, transaction_active>(self, shadow_frame, inst, inst_data);
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 2);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(IPUT_WIDE) {
    bool success = DoFieldPut<InstancePrimitiveWrite, Primitive::kPrimLong, do_access_check, transaction_active>(self, shadow_frame, inst, inst_data);
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 2);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(IPUT_OBJECT) {
    bool success = DoFieldPut<InstanceObjectWrite, Primitive::kPrimNot, do_access_check, transaction_active>(self, shadow_frame, inst, inst_data);
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 2);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(IPUT_QUICK) {
    bool success = DoIPutQuick<Primitive::kPrimInt, transaction_active>(shadow_frame, inst, inst_data);
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 2);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(IPUT_WIDE_QUICK) {
    bool success = DoIPutQuick<Primitive::kPrimLong, transaction_active>(shadow_frame, inst, inst_data);
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 2);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(IPUT_OBJECT_QUICK) {
    bool success = DoIPutQuick<Primitive::kPrimNot, transaction_active>(shadow_frame, inst, inst_data);
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 2);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(SPUT_BOOLEAN) {
    bool success = DoFieldPut<StaticPrimitiveWrite, Primitive::kPrimBoolean, do_access_check, transaction_active>(self, shadow_frame, inst, inst_data);
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 2);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(SPUT_BYTE) {
    bool success = DoFieldPut<StaticPrimitiveWrite, Primitive::kPrimByte, do_access_check, transaction_active>(self, shadow_frame, inst, inst_data);
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 2);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(SPUT_CHAR) {
    bool success = DoFieldPut<StaticPrimitiveWrite, Primitive::kPrimChar, do_access_check, transaction_active>(self, shadow_frame, inst, inst_data);
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 2);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(SPUT_SHORT) {
    bool success = DoFieldPut<StaticPrimitiveWrite, Primitive::kPrimShort, do_access_check, transaction_active>(self, shadow_frame, inst, inst_data);
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 2);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(SPUT) {
    bool success = DoFieldPut<StaticPrimitiveWrite, Primitive::kPrimInt, do_access_check, transaction_active>(self, shadow_frame, inst, inst_data);
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 2);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(SPUT_WIDE) {
    bool success = DoFieldPut<StaticPrimitiveWrite, Primitive::kPrimLong, do_access_check, transaction_active>(self, shadow_frame, inst, inst_data);
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 2);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(SPUT_OBJECT) {
    bool success = DoFieldPut<StaticObjectWrite, Primitive::kPrimNot, do_access_check, transaction_active>(self, shadow_frame, inst, inst_data);
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 2);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(INVOKE_VIRTUAL) {
    bool success = DoInvoke<kVirtual, false, do_access_check>(self, shadow_frame, inst, inst_data, &result_register);
    UPDATE_HANDLER_TABLE();
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 3);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(INVOKE_VIRTUAL_RANGE) {
    bool success = DoInvoke<kVirtual, true, do_access_check>(self, shadow_frame, inst, inst_data, &result_register);
    UPDATE_HANDLER_TABLE();
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 3);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(INVOKE_SUPER) {
    bool success = DoInvoke<kSuper, false, do_access_check>(self, shadow_frame, inst, inst_data, &result_register);
    UPDATE_HANDLER_TABLE();
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 3);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(INVOKE_SUPER_RANGE) {
    bool success = DoInvoke<kSuper, true, do_access_check>(self, shadow_frame, inst, inst_data, &result_register);
    UPDATE_HANDLER_TABLE();
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 3);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(INVOKE_DIRECT) {
    bool success = DoInvoke<kDirect, false, do_access_check>(self, shadow_frame, inst, inst_data, &result_register);
    UPDATE_HANDLER_TABLE();
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 3);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(INVOKE_DIRECT_RANGE) {
    bool success = DoInvoke<kDirect, true, do_access_check>(self, shadow_frame, inst, inst_data, &result_register);
    UPDATE_HANDLER_TABLE();
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 3);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(INVOKE_INTERFACE) {
    bool success = DoInvoke<kInterface, false, do_access_check>(self, shadow_frame, inst, inst_data, &result_register);
    UPDATE_HANDLER_TABLE();
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 3);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(INVOKE_INTERFACE_RANGE) {
    bool success = DoInvoke<kInterface, true, do_access_check>(self, shadow_frame, inst, inst_data, &result_register);
    UPDATE_HANDLER_TABLE();
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 3);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(INVOKE_STATIC) {
    bool success = DoInvoke<kStatic, false, do_access_check>(self, shadow_frame, inst, inst_data, &result_register);
    UPDATE_HANDLER_TABLE();
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 3);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(INVOKE_STATIC_RANGE) {
    bool success = DoInvoke<kStatic, true, do_access_check>(self, shadow_frame, inst, inst_data, &result_register);
    UPDATE_HANDLER_TABLE();
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 3);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(INVOKE_VIRTUAL_QUICK) {
    bool success = DoInvokeVirtualQuick<false>(self, shadow_frame, inst, inst_data, &result_register);
    UPDATE_HANDLER_TABLE();
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 3);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(INVOKE_VIRTUAL_RANGE_QUICK) {
    bool success = DoInvokeVirtualQuick<true>(self, shadow_frame, inst, inst_data, &result_register);
    UPDATE_HANDLER_TABLE();
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 3);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(NEG_INT)
    shadow_frame.SetVReg(inst->VRegA_12x(inst_data), -shadow_frame.GetVReg(inst->VRegB_12x(inst_data)));
    ADVANCE(1);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(NOT_INT)
    shadow_frame.SetVReg(inst->VRegA_12x(inst_data), ~shadow_frame.GetVReg(inst->VRegB_12x(inst_data)));
    ADVANCE(1);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(NEG_LONG)
    shadow_frame.SetVRegLong(inst->VRegA_12x(inst_data), -shadow_frame.GetVRegLong(inst->VRegB_12x(inst_data)));
    ADVANCE(1);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(NOT_LONG)
    shadow_frame.SetVRegLong(inst->VRegA_12x(inst_data), ~shadow_frame.GetVRegLong(inst->VRegB_12x(inst_data)));
    ADVANCE(1);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(NEG_FLOAT)
    shadow_frame.SetVRegFloat(inst->VRegA_12x(inst_data), -shadow_frame.GetVRegFloat(inst->VRegB_12x(inst_data)));
    ADVANCE(1);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(NEG_DOUBLE)
    shadow_frame.SetVRegDouble(inst->VRegA_12x(inst_data), -shadow_frame.GetVRegDouble(inst->VRegB_12x(inst_data)));
    ADVANCE(1);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(INT_TO_LONG)
    shadow_frame.SetVRegLong(inst->VRegA_12x(inst_data), shadow_frame.GetVReg(inst->VRegB_12x(inst_data)));
    ADVANCE(1);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(INT_TO_FLOAT)
    shadow_frame.SetVRegFloat(inst->VRegA_12x(inst_data), shadow_frame.GetVReg(inst->VRegB_12x(inst_data)));
    ADVANCE(1);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(INT_TO_DOUBLE)
    shadow_frame.SetVRegDouble(inst->VRegA_12x(inst_data), shadow_frame.GetVReg(inst->VRegB_12x(inst_data)));
    ADVANCE(1);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(LONG_TO_INT)
    shadow_frame.SetVReg(inst->VRegA_12x(inst_data), shadow_frame.GetVRegLong(inst->VRegB_12x(inst_data)));
    ADVANCE(1);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(LONG_TO_FLOAT)
    shadow_frame.SetVRegFloat(inst->VRegA_12x(inst_data), shadow_frame.GetVRegLong(inst->VRegB_12x(inst_data)));
    ADVANCE(1);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(LONG_TO_DOUBLE)
    shadow_frame.SetVRegDouble(inst->VRegA_12x(inst_data), shadow_frame.GetVRegLong(inst->VRegB_12x(inst_data)));
    ADVANCE(1);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(FLOAT_TO_INT) {
    float val = shadow_frame.GetVRegFloat(inst->VRegB_12x(inst_data));
    int32_t result = art_float_to_integral<int32_t, float>(val);
    shadow_frame.SetVReg(inst->VRegA_12x(inst_data), result);
    ADVANCE(1);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(FLOAT_TO_LONG) {
    float val = shadow_frame.GetVRegFloat(inst->VRegB_12x(inst_data));
    int64_t result = art_float_to_integral<int64_t, float>(val);
    shadow_frame.SetVRegLong(inst->VRegA_12x(inst_data), result);
    ADVANCE(1);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(FLOAT_TO_DOUBLE)
    shadow_frame.SetVRegDouble(inst->VRegA_12x(inst_data), shadow_frame.GetVRegFloat(inst->VRegB_12x(inst_data)));
    ADVANCE(1);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(DOUBLE_TO_INT) {
    double val = shadow_frame.GetVRegDouble(inst->VRegB_12x(inst_data));
    int32_t result = art_float_to_integral<int32_t, double>(val);
    shadow_frame.SetVReg(inst->VRegA_12x(inst_data), result);
    ADVANCE(1);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(DOUBLE_TO_LONG) {
    double val = shadow_frame.GetVRegDouble(inst->VRegB_12x(inst_data));
    int64_t result = art_float_to_integral<int64_t, double>(val);
    shadow_frame.SetVRegLong(inst->VRegA_12x(inst_data), result);
    ADVANCE(1);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(DOUBLE_TO_FLOAT)
    shadow_frame.SetVRegFloat(inst->VRegA_12x(inst_data), shadow_frame.GetVRegDouble(inst->VRegB_12x(inst_data)));
    ADVANCE(1);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(INT_TO_BYTE)
    shadow_frame.SetVReg(inst->VRegA_12x(inst_data),
                         static_cast<int8_t>(shadow_frame.GetVReg(inst->VRegB_12x(inst_data))));
    ADVANCE(1);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(INT_TO_CHAR)
    shadow_frame.SetVReg(inst->VRegA_12x(inst_data),
                         static_cast<uint16_t>(shadow_frame.GetVReg(inst->VRegB_12x(inst_data))));
    ADVANCE(1);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(INT_TO_SHORT)
    shadow_frame.SetVReg(inst->VRegA_12x(inst_data),
                         static_cast<int16_t>(shadow_frame.GetVReg(inst->VRegB_12x(inst_data))));
    ADVANCE(1);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(ADD_INT)
    shadow_frame.SetVReg(inst->VRegA_23x(inst_data),
                         shadow_frame.GetVReg(inst->VRegB_23x()) +
                         shadow_frame.GetVReg(inst->VRegC_23x()));
    ADVANCE(2);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(SUB_INT)
    shadow_frame.SetVReg(inst->VRegA_23x(inst_data),
                         shadow_frame.GetVReg(inst->VRegB_23x()) -
                         shadow_frame.GetVReg(inst->VRegC_23x()));
    ADVANCE(2);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(MUL_INT)
    shadow_frame.SetVReg(inst->VRegA_23x(inst_data),
                         shadow_frame.GetVReg(inst->VRegB_23x()) *
                         shadow_frame.GetVReg(inst->VRegC_23x()));
    ADVANCE(2);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(DIV_INT) {
    bool success = DoIntDivide(shadow_frame, inst->VRegA_23x(inst_data),
                               shadow_frame.GetVReg(inst->VRegB_23x()),
                               shadow_frame.GetVReg(inst->VRegC_23x()));
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 2);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(REM_INT) {
    bool success = DoIntRemainder(shadow_frame, inst->VRegA_23x(inst_data),
                                  shadow_frame.GetVReg(inst->VRegB_23x()),
                                  shadow_frame.GetVReg(inst->VRegC_23x()));
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 2);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(SHL_INT)
    shadow_frame.SetVReg(inst->VRegA_23x(inst_data),
                         shadow_frame.GetVReg(inst->VRegB_23x()) <<
                         (shadow_frame.GetVReg(inst->VRegC_23x()) & 0x1f));
    ADVANCE(2);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(SHR_INT)
    shadow_frame.SetVReg(inst->VRegA_23x(inst_data),
                         shadow_frame.GetVReg(inst->VRegB_23x()) >>
                         (shadow_frame.GetVReg(inst->VRegC_23x()) & 0x1f));
    ADVANCE(2);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(USHR_INT)
    shadow_frame.SetVReg(inst->VRegA_23x(inst_data),
                         static_cast<uint32_t>(shadow_frame.GetVReg(inst->VRegB_23x())) >>
                         (shadow_frame.GetVReg(inst->VRegC_23x()) & 0x1f));
    ADVANCE(2);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(AND_INT)
    shadow_frame.SetVReg(inst->VRegA_23x(inst_data),
                         shadow_frame.GetVReg(inst->VRegB_23x()) &
                         shadow_frame.GetVReg(inst->VRegC_23x()));
    ADVANCE(2);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(OR_INT)
    shadow_frame.SetVReg(inst->VRegA_23x(inst_data),
                         shadow_frame.GetVReg(inst->VRegB_23x()) |
                         shadow_frame.GetVReg(inst->VRegC_23x()));
    ADVANCE(2);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(XOR_INT)
    shadow_frame.SetVReg(inst->VRegA_23x(inst_data),
                         shadow_frame.GetVReg(inst->VRegB_23x()) ^
                         shadow_frame.GetVReg(inst->VRegC_23x()));
    ADVANCE(2);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(ADD_LONG)
    shadow_frame.SetVRegLong(inst->VRegA_23x(inst_data),
                             shadow_frame.GetVRegLong(inst->VRegB_23x()) +
                             shadow_frame.GetVRegLong(inst->VRegC_23x()));
    ADVANCE(2);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(SUB_LONG)
    shadow_frame.SetVRegLong(inst->VRegA_23x(inst_data),
                             shadow_frame.GetVRegLong(inst->VRegB_23x()) -
                             shadow_frame.GetVRegLong(inst->VRegC_23x()));
    ADVANCE(2);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(MUL_LONG)
    shadow_frame.SetVRegLong(inst->VRegA_23x(inst_data),
                             shadow_frame.GetVRegLong(inst->VRegB_23x()) *
                             shadow_frame.GetVRegLong(inst->VRegC_23x()));
    ADVANCE(2);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(DIV_LONG) {
    bool success = DoLongDivide(shadow_frame, inst->VRegA_23x(inst_data),
                                shadow_frame.GetVRegLong(inst->VRegB_23x()),
                                shadow_frame.GetVRegLong(inst->VRegC_23x()));
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 2);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(REM_LONG) {
    bool success = DoLongRemainder(shadow_frame, inst->VRegA_23x(inst_data),
                                   shadow_frame.GetVRegLong(inst->VRegB_23x()),
                                   shadow_frame.GetVRegLong(inst->VRegC_23x()));
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 2);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(AND_LONG)
    shadow_frame.SetVRegLong(inst->VRegA_23x(inst_data),
                             shadow_frame.GetVRegLong(inst->VRegB_23x()) &
                             shadow_frame.GetVRegLong(inst->VRegC_23x()));
    ADVANCE(2);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(OR_LONG)
    shadow_frame.SetVRegLong(inst->VRegA_23x(inst_data),
                             shadow_frame.GetVRegLong(inst->VRegB_23x()) |
                             shadow_frame.GetVRegLong(inst->VRegC_23x()));
    ADVANCE(2);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(XOR_LONG)
    shadow_frame.SetVRegLong(inst->VRegA_23x(inst_data),
                             shadow_frame.GetVRegLong(inst->VRegB_23x()) ^
                             shadow_frame.GetVRegLong(inst->VRegC_23x()));
    ADVANCE(2);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(SHL_LONG)
    shadow_frame.SetVRegLong(inst->VRegA_23x(inst_data),
                             shadow_frame.GetVRegLong(inst->VRegB_23x()) <<
                             (shadow_frame.GetVReg(inst->VRegC_23x()) & 0x3f));
    ADVANCE(2);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(SHR_LONG)
    shadow_frame.SetVRegLong(inst->VRegA_23x(inst_data),
                             shadow_frame.GetVRegLong(inst->VRegB_23x()) >>
                             (shadow_frame.GetVReg(inst->VRegC_23x()) & 0x3f));
    ADVANCE(2);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(USHR_LONG)
    shadow_frame.SetVRegLong(inst->VRegA_23x(inst_data),
                             static_cast<uint64_t>(shadow_frame.GetVRegLong(inst->VRegB_23x())) >>
                             (shadow_frame.GetVReg(inst->VRegC_23x()) & 0x3f));
    ADVANCE(2);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(ADD_FLOAT)
    shadow_frame.SetVRegFloat(inst->VRegA_23x(inst_data),
                              shadow_frame.GetVRegFloat(inst->VRegB_23x()) +
                              shadow_frame.GetVRegFloat(inst->VRegC_23x()));
    ADVANCE(2);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(SUB_FLOAT)
    shadow_frame.SetVRegFloat(inst->VRegA_23x(inst_data),
                              shadow_frame.GetVRegFloat(inst->VRegB_23x()) -
                              shadow_frame.GetVRegFloat(inst->VRegC_23x()));
    ADVANCE(2);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(MUL_FLOAT)
    shadow_frame.SetVRegFloat(inst->VRegA_23x(inst_data),
                              shadow_frame.GetVRegFloat(inst->VRegB_23x()) *
                              shadow_frame.GetVRegFloat(inst->VRegC_23x()));
    ADVANCE(2);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(DIV_FLOAT)
    shadow_frame.SetVRegFloat(inst->VRegA_23x(inst_data),
                              shadow_frame.GetVRegFloat(inst->VRegB_23x()) /
                              shadow_frame.GetVRegFloat(inst->VRegC_23x()));
    ADVANCE(2);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(REM_FLOAT)
    shadow_frame.SetVRegFloat(inst->VRegA_23x(inst_data),
                              fmodf(shadow_frame.GetVRegFloat(inst->VRegB_23x()),
                                    shadow_frame.GetVRegFloat(inst->VRegC_23x())));
    ADVANCE(2);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(ADD_DOUBLE)
    shadow_frame.SetVRegDouble(inst->VRegA_23x(inst_data),
                               shadow_frame.GetVRegDouble(inst->VRegB_23x()) +
                               shadow_frame.GetVRegDouble(inst->VRegC_23x()));
    ADVANCE(2);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(SUB_DOUBLE)
    shadow_frame.SetVRegDouble(inst->VRegA_23x(inst_data),
                               shadow_frame.GetVRegDouble(inst->VRegB_23x()) -
                               shadow_frame.GetVRegDouble(inst->VRegC_23x()));
    ADVANCE(2);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(MUL_DOUBLE)
    shadow_frame.SetVRegDouble(inst->VRegA_23x(inst_data),
                               shadow_frame.GetVRegDouble(inst->VRegB_23x()) *
                               shadow_frame.GetVRegDouble(inst->VRegC_23x()));
    ADVANCE(2);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(DIV_DOUBLE)
    shadow_frame.SetVRegDouble(inst->VRegA_23x(inst_data),
                               shadow_frame.GetVRegDouble(inst->VRegB_23x()) /
                               shadow_frame.GetVRegDouble(inst->VRegC_23x()));
    ADVANCE(2);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(REM_DOUBLE)
    shadow_frame.SetVRegDouble(inst->VRegA_23x(inst_data),
                               fmod(shadow_frame.GetVRegDouble(inst->VRegB_23x()),
                                    shadow_frame.GetVRegDouble(inst->VRegC_23x())));
    ADVANCE(2);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(ADD_INT_2ADDR) {
    uint32_t vregA = inst->VRegA_12x(inst_data);
    shadow_frame.SetVReg(vregA,
                         shadow_frame.GetVReg(vregA) +
                         shadow_frame.GetVReg(inst->VRegB_12x(inst_data)));
    ADVANCE(1);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(SUB_INT_2ADDR) {
    uint32_t vregA = inst->VRegA_12x(inst_data);
    shadow_frame.SetVReg(vregA,
                         shadow_frame.GetVReg(vregA) -
                         shadow_frame.GetVReg(inst->VRegB_12x(inst_data)));
    ADVANCE(1);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(MUL_INT_2ADDR) {
    uint32_t vregA = inst->VRegA_12x(inst_data);
    shadow_frame.SetVReg(vregA,
                         shadow_frame.GetVReg(vregA) *
                         shadow_frame.GetVReg(inst->VRegB_12x(inst_data)));
    ADVANCE(1);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(DIV_INT_2ADDR) {
    uint32_t vregA = inst->VRegA_12x(inst_data);
    bool success = DoIntDivide(shadow_frame, vregA, shadow_frame.GetVReg(vregA),
                               shadow_frame.GetVReg(inst->VRegB_12x(inst_data)));
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 1);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(REM_INT_2ADDR) {
    uint32_t vregA = inst->VRegA_12x(inst_data);
    bool success = DoIntRemainder(shadow_frame, vregA, shadow_frame.GetVReg(vregA),
                                  shadow_frame.GetVReg(inst->VRegB_12x(inst_data)));
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 1);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(SHL_INT_2ADDR) {
    uint32_t vregA = inst->VRegA_12x(inst_data);
    shadow_frame.SetVReg(vregA,
                         shadow_frame.GetVReg(vregA) <<
                         (shadow_frame.GetVReg(inst->VRegB_12x(inst_data)) & 0x1f));
    ADVANCE(1);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(SHR_INT_2ADDR) {
    uint32_t vregA = inst->VRegA_12x(inst_data);
    shadow_frame.SetVReg(vregA,
                         shadow_frame.GetVReg(vregA) >>
                         (shadow_frame.GetVReg(inst->VRegB_12x(inst_data)) & 0x1f));
    ADVANCE(1);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(USHR_INT_2ADDR) {
    uint32_t vregA = inst->VRegA_12x(inst_data);
    shadow_frame.SetVReg(vregA,
                         static_cast<uint32_t>(shadow_frame.GetVReg(vregA)) >>
                         (shadow_frame.GetVReg(inst->VRegB_12x(inst_data)) & 0x1f));
    ADVANCE(1);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(AND_INT_2ADDR) {
    uint32_t vregA = inst->VRegA_12x(inst_data);
    shadow_frame.SetVReg(vregA,
                         shadow_frame.GetVReg(vregA) &
                         shadow_frame.GetVReg(inst->VRegB_12x(inst_data)));
    ADVANCE(1);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(OR_INT_2ADDR) {
    uint32_t vregA = inst->VRegA_12x(inst_data);
    shadow_frame.SetVReg(vregA,
                         shadow_frame.GetVReg(vregA) |
                         shadow_frame.GetVReg(inst->VRegB_12x(inst_data)));
    ADVANCE(1);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(XOR_INT_2ADDR) {
    uint32_t vregA = inst->VRegA_12x(inst_data);
    shadow_frame.SetVReg(vregA,
                         shadow_frame.GetVReg(vregA) ^
                         shadow_frame.GetVReg(inst->VRegB_12x(inst_data)));
    ADVANCE(1);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(ADD_LONG_2ADDR) {
    uint32_t vregA = inst->VRegA_12x(inst_data);
    shadow_frame.SetVRegLong(vregA,
                             shadow_frame.GetVRegLong(vregA) +
                             shadow_frame.GetVRegLong(inst->VRegB_12x(inst_data)));
    ADVANCE(1);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(SUB_LONG_2ADDR) {
    uint32_t vregA = inst->VRegA_12x(inst_data);
    shadow_frame.SetVRegLong(vregA,
                             shadow_frame.GetVRegLong(vregA) -
                             shadow_frame.GetVRegLong(inst->VRegB_12x(inst_data)));
    ADVANCE(1);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(MUL_LONG_2ADDR) {
    uint32_t vregA = inst->VRegA_12x(inst_data);
    shadow_frame.SetVRegLong(vregA,
                             shadow_frame.GetVRegLong(vregA) *
                             shadow_frame.GetVRegLong(inst->VRegB_12x(inst_data)));
    ADVANCE(1);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(DIV_LONG_2ADDR) {
    uint32_t vregA = inst->VRegA_12x(inst_data);
    bool success = DoLongDivide(shadow_frame, vregA, shadow_frame.GetVRegLong(vregA),
                                shadow_frame.GetVRegLong(inst->VRegB_12x(inst_data)));
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 1);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(REM_LONG_2ADDR) {
    uint32_t vregA = inst->VRegA_12x(inst_data);
    bool success = DoLongRemainder(shadow_frame, vregA, shadow_frame.GetVRegLong(vregA),
                                   shadow_frame.GetVRegLong(inst->VRegB_12x(inst_data)));
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 1);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(AND_LONG_2ADDR) {
    uint32_t vregA = inst->VRegA_12x(inst_data);
    shadow_frame.SetVRegLong(vregA,
                             shadow_frame.GetVRegLong(vregA) &
                             shadow_frame.GetVRegLong(inst->VRegB_12x(inst_data)));
    ADVANCE(1);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(OR_LONG_2ADDR) {
    uint32_t vregA = inst->VRegA_12x(inst_data);
    shadow_frame.SetVRegLong(vregA,
                             shadow_frame.GetVRegLong(vregA) |
                             shadow_frame.GetVRegLong(inst->VRegB_12x(inst_data)));
    ADVANCE(1);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(XOR_LONG_2ADDR) {
    uint32_t vregA = inst->VRegA_12x(inst_data);
    shadow_frame.SetVRegLong(vregA,
                             shadow_frame.GetVRegLong(vregA) ^
                             shadow_frame.GetVRegLong(inst->VRegB_12x(inst_data)));
    ADVANCE(1);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(SHL_LONG_2ADDR) {
    uint32_t vregA = inst->VRegA_12x(inst_data);
    shadow_frame.SetVRegLong(vregA,
                             shadow_frame.GetVRegLong(vregA) <<
                             (shadow_frame.GetVReg(inst->VRegB_12x(inst_data)) & 0x3f));
    ADVANCE(1);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(SHR_LONG_2ADDR) {
    uint32_t vregA = inst->VRegA_12x(inst_data);
    shadow_frame.SetVRegLong(vregA,
                             shadow_frame.GetVRegLong(vregA) >>
                             (shadow_frame.GetVReg(inst->VRegB_12x(inst_data)) & 0x3f));
    ADVANCE(1);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(USHR_LONG_2ADDR) {
    uint32_t vregA = inst->VRegA_12x(inst_data);
    shadow_frame.SetVRegLong(vregA,
                             static_cast<uint64_t>(shadow_frame.GetVRegLong(vregA)) >>
                             (shadow_frame.GetVReg(inst->VRegB_12x(inst_data)) & 0x3f));
    ADVANCE(1);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(ADD_FLOAT_2ADDR) {
    uint32_t vregA = inst->VRegA_12x(inst_data);
    shadow_frame.SetVRegFloat(vregA,
                              shadow_frame.GetVRegFloat(vregA) +
                              shadow_frame.GetVRegFloat(inst->VRegB_12x(inst_data)));
    ADVANCE(1);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(SUB_FLOAT_2ADDR) {
    uint32_t vregA = inst->VRegA_12x(inst_data);
    shadow_frame.SetVRegFloat(vregA,
                              shadow_frame.GetVRegFloat(vregA) -
                              shadow_frame.GetVRegFloat(inst->VRegB_12x(inst_data)));
    ADVANCE(1);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(MUL_FLOAT_2ADDR) {
    uint32_t vregA = inst->VRegA_12x(inst_data);
    shadow_frame.SetVRegFloat(vregA,
                              shadow_frame.GetVRegFloat(vregA) *
                              shadow_frame.GetVRegFloat(inst->VRegB_12x(inst_data)));
    ADVANCE(1);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(DIV_FLOAT_2ADDR) {
    uint32_t vregA = inst->VRegA_12x(inst_data);
    shadow_frame.SetVRegFloat(vregA,
                              shadow_frame.GetVRegFloat(vregA) /
                              shadow_frame.GetVRegFloat(inst->VRegB_12x(inst_data)));
    ADVANCE(1);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(REM_FLOAT_2ADDR) {
    uint32_t vregA = inst->VRegA_12x(inst_data);
    shadow_frame.SetVRegFloat(vregA,
                              fmodf(shadow_frame.GetVRegFloat(vregA),
                                    shadow_frame.GetVRegFloat(inst->VRegB_12x(inst_data))));
    ADVANCE(1);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(ADD_DOUBLE_2ADDR) {
    uint32_t vregA = inst->VRegA_12x(inst_data);
    shadow_frame.SetVRegDouble(vregA,
                               shadow_frame.GetVRegDouble(vregA) +
                               shadow_frame.GetVRegDouble(inst->VRegB_12x(inst_data)));
    ADVANCE(1);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(SUB_DOUBLE_2ADDR) {
    uint32_t vregA = inst->VRegA_12x(inst_data);
    shadow_frame.SetVRegDouble(vregA,
                               shadow_frame.GetVRegDouble(vregA) -
                               shadow_frame.GetVRegDouble(inst->VRegB_12x(inst_data)));
    ADVANCE(1);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(MUL_DOUBLE_2ADDR) {
    uint32_t vregA = inst->VRegA_12x(inst_data);
    shadow_frame.SetVRegDouble(vregA,
                               shadow_frame.GetVRegDouble(vregA) *
                               shadow_frame.GetVRegDouble(inst->VRegB_12x(inst_data)));
    ADVANCE(1);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(DIV_DOUBLE_2ADDR) {
    uint32_t vregA = inst->VRegA_12x(inst_data);
    shadow_frame.SetVRegDouble(vregA,
                               shadow_frame.GetVRegDouble(vregA) /
                               shadow_frame.GetVRegDouble(inst->VRegB_12x(inst_data)));
    ADVANCE(1);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(REM_DOUBLE_2ADDR) {
    uint32_t vregA = inst->VRegA_12x(inst_data);
    shadow_frame.SetVRegDouble(vregA,
                               fmod(shadow_frame.GetVRegDouble(vregA),
                                    shadow_frame.GetVRegDouble(inst->VRegB_12x(inst_data))));
    ADVANCE(1);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(ADD_INT_LIT16)
    shadow_frame.SetVReg(inst->VRegA_22s(inst_data),
                         shadow_frame.GetVReg(inst->VRegB_22s(inst_data)) +
                         inst->VRegC_22s());
    ADVANCE(2);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(RSUB_INT)
    shadow_frame.SetVReg(inst->VRegA_22s(inst_data),
                         inst->VRegC_22s() -
                         shadow_frame.GetVReg(inst->VRegB_22s(inst_data)));
    ADVANCE(2);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(MUL_INT_LIT16)
    shadow_frame.SetVReg(inst->VRegA_22s(inst_data),
                         shadow_frame.GetVReg(inst->VRegB_22s(inst_data)) *
                         inst->VRegC_22s());
    ADVANCE(2);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(DIV_INT_LIT16) {
    bool success = DoIntDivide(shadow_frame, inst->VRegA_22s(inst_data),
                               shadow_frame.GetVReg(inst->VRegB_22s(inst_data)), inst->VRegC_22s());
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 2);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(REM_INT_LIT16) {
    bool success = DoIntRemainder(shadow_frame, inst->VRegA_22s(inst_data),
                                  shadow_frame.GetVReg(inst->VRegB_22s(inst_data)), inst->VRegC_22s());
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 2);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(AND_INT_LIT16)
    shadow_frame.SetVReg(inst->VRegA_22s(inst_data),
                         shadow_frame.GetVReg(inst->VRegB_22s(inst_data)) &
                         inst->VRegC_22s());
    ADVANCE(2);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(OR_INT_LIT16)
    shadow_frame.SetVReg(inst->VRegA_22s(inst_data),
                         shadow_frame.GetVReg(inst->VRegB_22s(inst_data)) |
                         inst->VRegC_22s());
    ADVANCE(2);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(XOR_INT_LIT16)
    shadow_frame.SetVReg(inst->VRegA_22s(inst_data),
                         shadow_frame.GetVReg(inst->VRegB_22s(inst_data)) ^
                         inst->VRegC_22s());
    ADVANCE(2);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(ADD_INT_LIT8)
    shadow_frame.SetVReg(inst->VRegA_22b(inst_data),
                         shadow_frame.GetVReg(inst->VRegB_22b()) +
                         inst->VRegC_22b());
    ADVANCE(2);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(RSUB_INT_LIT8)
    shadow_frame.SetVReg(inst->VRegA_22b(inst_data),
                         inst->VRegC_22b() -
                         shadow_frame.GetVReg(inst->VRegB_22b()));
    ADVANCE(2);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(MUL_INT_LIT8)
    shadow_frame.SetVReg(inst->VRegA_22b(inst_data),
                         shadow_frame.GetVReg(inst->VRegB_22b()) *
                         inst->VRegC_22b());
    ADVANCE(2);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(DIV_INT_LIT8) {
    bool success = DoIntDivide(shadow_frame, inst->VRegA_22b(inst_data),
                               shadow_frame.GetVReg(inst->VRegB_22b()), inst->VRegC_22b());
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 2);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(REM_INT_LIT8) {
    bool success = DoIntRemainder(shadow_frame, inst->VRegA_22b(inst_data),
                                  shadow_frame.GetVReg(inst->VRegB_22b()), inst->VRegC_22b());
    POSSIBLY_HANDLE_PENDING_EXCEPTION(!success, 2);
  }
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(AND_INT_LIT8)
    shadow_frame.SetVReg(inst->VRegA_22b(inst_data),
                         shadow_frame.GetVReg(inst->VRegB_22b()) &
                         inst->VRegC_22b());
    ADVANCE(2);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(OR_INT_LIT8)
    shadow_frame.SetVReg(inst->VRegA_22b(inst_data),
                         shadow_frame.GetVReg(inst->VRegB_22b()) |
                         inst->VRegC_22b());
    ADVANCE(2);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(XOR_INT_LIT8)
    shadow_frame.SetVReg(inst->VRegA_22b(inst_data),
                         shadow_frame.GetVReg(inst->VRegB_22b()) ^
                         inst->VRegC_22b());
    ADVANCE(2);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(SHL_INT_LIT8)
    shadow_frame.SetVReg(inst->VRegA_22b(inst_data),
                         shadow_frame.GetVReg(inst->VRegB_22b()) <<
                         (inst->VRegC_22b() & 0x1f));
    ADVANCE(2);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(SHR_INT_LIT8)
    shadow_frame.SetVReg(inst->VRegA_22b(inst_data),
                         shadow_frame.GetVReg(inst->VRegB_22b()) >>
                         (inst->VRegC_22b() & 0x1f));
    ADVANCE(2);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(USHR_INT_LIT8)
    shadow_frame.SetVReg(inst->VRegA_22b(inst_data),
                         static_cast<uint32_t>(shadow_frame.GetVReg(inst->VRegB_22b())) >>
                         (inst->VRegC_22b() & 0x1f));
    ADVANCE(2);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(UNUSED_3E)
    UnexpectedOpcode(inst, mh);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(UNUSED_3F)
    UnexpectedOpcode(inst, mh);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(UNUSED_40)
    UnexpectedOpcode(inst, mh);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(UNUSED_41)
    UnexpectedOpcode(inst, mh);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(UNUSED_42)
    UnexpectedOpcode(inst, mh);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(UNUSED_43)
    UnexpectedOpcode(inst, mh);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(UNUSED_79)
    UnexpectedOpcode(inst, mh);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(UNUSED_7A)
    UnexpectedOpcode(inst, mh);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(UNUSED_EB)
    UnexpectedOpcode(inst, mh);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(UNUSED_EC)
    UnexpectedOpcode(inst, mh);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(UNUSED_ED)
    UnexpectedOpcode(inst, mh);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(UNUSED_EE)
    UnexpectedOpcode(inst, mh);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(UNUSED_EF)
    UnexpectedOpcode(inst, mh);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(UNUSED_F0)
    UnexpectedOpcode(inst, mh);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(UNUSED_F1)
    UnexpectedOpcode(inst, mh);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(UNUSED_F2)
    UnexpectedOpcode(inst, mh);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(UNUSED_F3)
    UnexpectedOpcode(inst, mh);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(UNUSED_F4)
    UnexpectedOpcode(inst, mh);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(UNUSED_F5)
    UnexpectedOpcode(inst, mh);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(UNUSED_F6)
    UnexpectedOpcode(inst, mh);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(UNUSED_F7)
    UnexpectedOpcode(inst, mh);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(UNUSED_F8)
    UnexpectedOpcode(inst, mh);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(UNUSED_F9)
    UnexpectedOpcode(inst, mh);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(UNUSED_FA)
    UnexpectedOpcode(inst, mh);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(UNUSED_FB)
    UnexpectedOpcode(inst, mh);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(UNUSED_FC)
    UnexpectedOpcode(inst, mh);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(UNUSED_FD)
    UnexpectedOpcode(inst, mh);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(UNUSED_FE)
    UnexpectedOpcode(inst, mh);
  HANDLE_INSTRUCTION_END();

  HANDLE_INSTRUCTION_START(UNUSED_FF)
    UnexpectedOpcode(inst, mh);
  HANDLE_INSTRUCTION_END();

  exception_pending_label: {
    CHECK(self->IsExceptionPending());
    if (UNLIKELY(self->TestAllFlags())) {
      CheckSuspend(self);
      UPDATE_HANDLER_TABLE();
    }
    Object* this_object = shadow_frame.GetThisObject(code_item->ins_size_);
    instrumentation::Instrumentation* instrumentation = Runtime::Current()->GetInstrumentation();
    uint32_t found_dex_pc = FindNextInstructionFollowingException(self, shadow_frame, dex_pc,
                                                                  this_object,
                                                                  instrumentation);
    if (found_dex_pc == DexFile::kDexNoIndex) {
      return JValue(); /* Handled in caller. */
    } else {
      int32_t displacement = static_cast<int32_t>(found_dex_pc) - static_cast<int32_t>(dex_pc);
      ADVANCE(displacement);
    }
  }

// Create alternative instruction handlers dedicated to instrumentation.
// Return instructions must not call Instrumentation::DexPcMovedEvent since they already call
// Instrumentation::MethodExited. This is to avoid posting debugger events twice for this location.
// Note: we do not use the kReturn instruction flag here (to test the instruction is a return). The
// compiler seems to not evaluate "(Instruction::FlagsOf(Instruction::code) & kReturn) != 0" to
// a constant condition that would remove the "if" statement so the test is free.
#define INSTRUMENTATION_INSTRUCTION_HANDLER(o, code, n, f, r, i, a, v)                            \
  alt_op_##code: {                                                                                \
    if (Instruction::code != Instruction::RETURN_VOID &&                                          \
        Instruction::code != Instruction::RETURN_VOID_BARRIER &&                                  \
        Instruction::code != Instruction::RETURN &&                                               \
        Instruction::code != Instruction::RETURN_WIDE &&                                          \
        Instruction::code != Instruction::RETURN_OBJECT) {                                        \
      if (LIKELY(!notified_method_entry_event)) {                                                 \
        Runtime* runtime = Runtime::Current();                                                    \
        const instrumentation::Instrumentation* instrumentation = runtime->GetInstrumentation();  \
        if (UNLIKELY(instrumentation->HasDexPcListeners())) {                                     \
          Object* this_object = shadow_frame.GetThisObject(code_item->ins_size_);                 \
          instrumentation->DexPcMovedEvent(self, this_object, shadow_frame.GetMethod(), dex_pc);  \
        }                                                                                         \
      } else {                                                                                    \
        notified_method_entry_event = false;                                                      \
      }                                                                                           \
    }                                                                                             \
    UPDATE_HANDLER_TABLE();                                                                       \
    goto *handlersTable[instrumentation::kMainHandlerTable][Instruction::code];                   \
  }
#include "dex_instruction_list.h"
      DEX_INSTRUCTION_LIST(INSTRUMENTATION_INSTRUCTION_HANDLER)
#undef DEX_INSTRUCTION_LIST
#undef INSTRUMENTATION_INSTRUCTION_HANDLER
}  // NOLINT(readability/fn_size)

// Explicit definitions of ExecuteGotoImpl.
template SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) HOT_ATTR
JValue ExecuteGotoImpl<true, false>(Thread* self, MethodHelper& mh,
                                    const DexFile::CodeItem* code_item,
                                    ShadowFrame& shadow_frame, JValue result_register);
template SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) HOT_ATTR
JValue ExecuteGotoImpl<false, false>(Thread* self, MethodHelper& mh,
                                     const DexFile::CodeItem* code_item,
                                     ShadowFrame& shadow_frame, JValue result_register);
template SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
JValue ExecuteGotoImpl<true, true>(Thread* self, MethodHelper& mh,
                                    const DexFile::CodeItem* code_item,
                                    ShadowFrame& shadow_frame, JValue result_register);
template SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
JValue ExecuteGotoImpl<false, true>(Thread* self, MethodHelper& mh,
                                     const DexFile::CodeItem* code_item,
                                     ShadowFrame& shadow_frame, JValue result_register);

}  // namespace interpreter
}  // namespace art
