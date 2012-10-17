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

#include "interpreter.h"

#include <math.h>

#include "common_throws.h"
#include "dex_instruction.h"
#include "invoke_arg_array_builder.h"
#include "logging.h"
#include "object.h"
#include "object_utils.h"
#include "runtime_support.h"
#include "ScopedLocalRef.h"
#include "scoped_thread_state_change.h"
#include "thread.h"

namespace art {
namespace interpreter {

static void DoMonitorEnter(Thread* self, Object* ref) NO_THREAD_SAFETY_ANALYSIS {
  ref->MonitorEnter(self);
}

static void DoMonitorExit(Thread* self, Object* ref) NO_THREAD_SAFETY_ANALYSIS {
  ref->MonitorExit(self);
}

static void DoInvoke(Thread* self, MethodHelper& mh, ShadowFrame& shadow_frame,
                     const DecodedInstruction& dec_insn, InvokeType type, bool is_range,
                     JValue* result)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  Object* receiver;
  if (type == kStatic) {
    receiver = NULL;
  } else {
    receiver = shadow_frame.GetReference(dec_insn.vC);
    if (UNLIKELY(receiver == NULL)) {
      ThrowNullPointerExceptionForMethodAccess(shadow_frame.GetMethod(), dec_insn.vB, type);
      result->SetJ(0);
      return;
    }
  }
  uint32_t method_idx = dec_insn.vB;
  AbstractMethod* target_method = FindMethodFromCode(method_idx, receiver,
                                                     shadow_frame.GetMethod(), self, true,
                                                     type);
  if (UNLIKELY(target_method == NULL)) {
    CHECK(self->IsExceptionPending());
    result->SetJ(0);
    return;
  }
  mh.ChangeMethod(target_method);
  ArgArray arg_array(mh.GetShorty(), mh.GetShortyLength());
  if (is_range) {
    arg_array.BuildArgArray(shadow_frame, dec_insn.vC + (type != kStatic ? 1 : 0));
  } else {
    arg_array.BuildArgArray(shadow_frame, dec_insn.arg + (type != kStatic ? 1 : 0));
  }
  target_method->Invoke(self, receiver, arg_array.get(), result);
  if (!mh.GetReturnType()->IsPrimitive() && result->GetL() != NULL) {
    CHECK(mh.GetReturnType()->IsAssignableFrom(result->GetL()->GetClass()));
  }
  mh.ChangeMethod(shadow_frame.GetMethod());
}

static void DoFieldGet(Thread* self, ShadowFrame& shadow_frame,
                       const DecodedInstruction& dec_insn, FindFieldType find_type,
                       Primitive::Type field_type)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  bool is_static = (find_type == StaticObjectRead) || (find_type == StaticPrimitiveRead);
  uint32_t field_idx = is_static ? dec_insn.vB : dec_insn.vC;
  Field* f = FindFieldFromCode(field_idx, shadow_frame.GetMethod(), self,
                               find_type, Primitive::FieldSize(field_type));
  if (LIKELY(f != NULL)) {
    Object* obj;
    if (is_static) {
      obj = f->GetDeclaringClass();
    } else {
      obj = shadow_frame.GetReference(dec_insn.vB);
      if (UNLIKELY(obj == NULL)) {
        ThrowNullPointerExceptionForFieldAccess(f, true);
      }
    }
    switch (field_type) {
      case Primitive::kPrimBoolean:
        shadow_frame.SetVReg(dec_insn.vA, f->GetBoolean(obj));
        break;
      case Primitive::kPrimByte:
        shadow_frame.SetVReg(dec_insn.vA, f->GetByte(obj));
        break;
      case Primitive::kPrimChar:
        shadow_frame.SetVReg(dec_insn.vA, f->GetChar(obj));
        break;
      case Primitive::kPrimShort:
        shadow_frame.SetVReg(dec_insn.vA, f->GetShort(obj));
        break;
      case Primitive::kPrimInt:
        shadow_frame.SetVReg(dec_insn.vA, f->GetInt(obj));
        break;
      case Primitive::kPrimLong:
        shadow_frame.SetVRegLong(dec_insn.vA, f->GetLong(obj));
        break;
      case Primitive::kPrimNot:
        shadow_frame.SetReferenceAndVReg(dec_insn.vA, f->GetObject(obj));
        break;
      default:
        LOG(FATAL) << "Unreachable: " << field_type;
    }
  }
}

static void DoFieldPut(Thread* self, ShadowFrame& shadow_frame,
                       const DecodedInstruction& dec_insn, FindFieldType find_type,
                       Primitive::Type field_type)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  bool is_static = (find_type == StaticObjectWrite) || (find_type == StaticPrimitiveWrite);
  uint32_t field_idx = is_static ? dec_insn.vB : dec_insn.vC;
  Field* f = FindFieldFromCode(field_idx, shadow_frame.GetMethod(), self,
                               find_type, Primitive::FieldSize(field_type));
  if (LIKELY(f != NULL)) {
    Object* obj;
    if (is_static) {
      obj = f->GetDeclaringClass();
    } else {
      obj = shadow_frame.GetReference(dec_insn.vB);
      if (UNLIKELY(obj == NULL)) {
        ThrowNullPointerExceptionForFieldAccess(f, false);
      }
    }
    switch (field_type) {
      case Primitive::kPrimBoolean:
        f->SetBoolean(obj, shadow_frame.GetVReg(dec_insn.vA));
        break;
      case Primitive::kPrimByte:
        f->SetByte(obj, shadow_frame.GetVReg(dec_insn.vA));
        shadow_frame.SetVReg(dec_insn.vA, f->GetByte(obj));
        break;
      case Primitive::kPrimChar:
        f->SetChar(obj, shadow_frame.GetVReg(dec_insn.vA));
        shadow_frame.SetVReg(dec_insn.vA, f->GetChar(obj));
        break;
      case Primitive::kPrimShort:
        f->SetShort(obj, shadow_frame.GetVReg(dec_insn.vA));
        shadow_frame.SetVReg(dec_insn.vA, f->GetShort(obj));
        break;
      case Primitive::kPrimInt:
        f->SetInt(obj, shadow_frame.GetVReg(dec_insn.vA));
        shadow_frame.SetVReg(dec_insn.vA, f->GetInt(obj));
        break;
      case Primitive::kPrimLong:
        f->SetLong(obj, shadow_frame.GetVRegLong(dec_insn.vA));
        shadow_frame.SetVRegLong(dec_insn.vA, f->GetLong(obj));
        break;
      case Primitive::kPrimNot:
        f->SetObj(obj, shadow_frame.GetReference(dec_insn.vA));
        break;
      default:
        LOG(FATAL) << "Unreachable: " << field_type;
    }
  }
}

static JValue Execute(Thread* self, MethodHelper& mh, const DexFile::CodeItem* code_item,
                      ShadowFrame& shadow_frame) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  const uint16_t* insns = code_item->insns_;
  const Instruction* inst = Instruction::At(insns + shadow_frame.GetDexPC());
  JValue result_register;
  while (true) {
    shadow_frame.SetDexPC(inst->GetDexPc(insns));
    DecodedInstruction dec_insn(inst);
    const bool kTracing = true;
    if (kTracing) {
      LOG(INFO) << PrettyMethod(shadow_frame.GetMethod())
                << StringPrintf("\n0x%x: %s\nReferences:",
                                inst->GetDexPc(insns), inst->DumpString(&mh.GetDexFile()).c_str());
      for (size_t i = 0; i < shadow_frame.NumberOfReferences(); ++i) {
        Object* o = shadow_frame.GetReference(i);
        if (o != NULL) {
          if (o->GetClass()->IsStringClass() && o->AsString()->GetCharArray() != NULL) {
            LOG(INFO) << i << ": java.lang.String " << static_cast<void*>(o)
                  << " \"" << o->AsString()->ToModifiedUtf8() << "\"";
          } else {
            LOG(INFO) << i << ": " << PrettyTypeOf(o) << " " << static_cast<void*>(o);
          }
        } else {
          LOG(INFO) << i << ": null";
        }
      }
      LOG(INFO) << "vregs:";
      for (size_t i = 0; i < shadow_frame.NumberOfReferences(); ++i) {
        LOG(INFO) << StringPrintf("%d: %08x", i, shadow_frame.GetVReg(i));
      }
    }
    const Instruction* next_inst = inst->Next();
    switch (dec_insn.opcode) {
      case Instruction::NOP:
        break;
      case Instruction::MOVE:
      case Instruction::MOVE_FROM16:
      case Instruction::MOVE_16:
        shadow_frame.SetVReg(dec_insn.vA, shadow_frame.GetVReg(dec_insn.vB));
        break;
      case Instruction::MOVE_WIDE:
      case Instruction::MOVE_WIDE_FROM16:
      case Instruction::MOVE_WIDE_16:
        shadow_frame.SetVRegLong(dec_insn.vA, shadow_frame.GetVRegLong(dec_insn.vB));
        break;
      case Instruction::MOVE_OBJECT:
      case Instruction::MOVE_OBJECT_FROM16:
      case Instruction::MOVE_OBJECT_16:
        shadow_frame.SetVReg(dec_insn.vA, shadow_frame.GetVReg(dec_insn.vB));
        shadow_frame.SetReference(dec_insn.vA, shadow_frame.GetReference(dec_insn.vB));
        break;
      case Instruction::MOVE_RESULT:
        shadow_frame.SetVReg(dec_insn.vA, result_register.GetI());
        break;
      case Instruction::MOVE_RESULT_WIDE:
        shadow_frame.SetVRegLong(dec_insn.vA, result_register.GetJ());
        break;
      case Instruction::MOVE_RESULT_OBJECT:
        shadow_frame.SetReferenceAndVReg(dec_insn.vA, result_register.GetL());
        break;
      case Instruction::MOVE_EXCEPTION: {
        Throwable* exception = self->GetException();
        self->ClearException();
        shadow_frame.SetReferenceAndVReg(dec_insn.vA, exception);
        break;
      }
      case Instruction::RETURN_VOID: {
        JValue result;
        result.SetJ(0);
        return result;
      }
      case Instruction::RETURN: {
        JValue result;
        result.SetJ(0);
        result.SetI(shadow_frame.GetVReg(dec_insn.vA));
        return result;
      }
      case Instruction::RETURN_WIDE: {
        JValue result;
        result.SetJ(shadow_frame.GetVRegLong(dec_insn.vA));
        return result;
      }
      case Instruction::RETURN_OBJECT: {
        JValue result;
        result.SetJ(0);
        result.SetL(shadow_frame.GetReference(dec_insn.vA));
        return result;
      }
      case Instruction::CONST_4: {
        int32_t val = (dec_insn.vB << 28) >> 28;
        shadow_frame.SetVReg(dec_insn.vA, val);
        if (val == 0) {
          shadow_frame.SetReference(dec_insn.vA, NULL);
        }
        break;
      }
      case Instruction::CONST_16: {
        int32_t val = static_cast<int16_t>(dec_insn.vB);
        shadow_frame.SetVReg(dec_insn.vA, val);
        if (val == 0) {
          shadow_frame.SetReference(dec_insn.vA, NULL);
        }
        break;
      }
      case Instruction::CONST: {
        int32_t val = dec_insn.vB;
        shadow_frame.SetVReg(dec_insn.vA, val);
        if (val == 0) {
          shadow_frame.SetReference(dec_insn.vA, NULL);
        }
        break;
      }
      case Instruction::CONST_HIGH16: {
        int32_t val = dec_insn.vB << 16;
        shadow_frame.SetVReg(dec_insn.vA, val);
        if (val == 0) {
          shadow_frame.SetReference(dec_insn.vA, NULL);
        }
        break;
      }
      case Instruction::CONST_WIDE_16: {
        int64_t val = static_cast<int16_t>(dec_insn.vB);
        shadow_frame.SetVReg(dec_insn.vA, val);
        shadow_frame.SetVReg(dec_insn.vA + 1, val >> 32);
        break;
      }
      case Instruction::CONST_WIDE_32: {
        int64_t val = static_cast<int32_t>(dec_insn.vB);
        shadow_frame.SetVReg(dec_insn.vA, val);
        shadow_frame.SetVReg(dec_insn.vA + 1, val >> 32);
        break;
      }
      case Instruction::CONST_WIDE:
        shadow_frame.SetVReg(dec_insn.vA, dec_insn.vB_wide);
        shadow_frame.SetVReg(dec_insn.vA + 1, dec_insn.vB_wide >> 32);
        break;
      case Instruction::CONST_WIDE_HIGH16:
        shadow_frame.SetVRegLong(dec_insn.vA + 1, static_cast<uint64_t>(dec_insn.vB) << 48);
        break;
      case Instruction::CONST_STRING:
      case Instruction::CONST_STRING_JUMBO: {
        if (UNLIKELY(!String::GetJavaLangString()->IsInitialized())) {
          Runtime::Current()->GetClassLinker()->EnsureInitialized(String::GetJavaLangString(),
                                                                  true, true);
        }
        String* s = mh.ResolveString(dec_insn.vB);
        shadow_frame.SetReferenceAndVReg(dec_insn.vA, s);
        break;
      }
      case Instruction::CONST_CLASS:
        shadow_frame.SetReference(dec_insn.vA, mh.ResolveClass(dec_insn.vB));
        break;
      case Instruction::MONITOR_ENTER:
        DoMonitorEnter(self, shadow_frame.GetReference(dec_insn.vA));
        break;
      case Instruction::MONITOR_EXIT:
        DoMonitorExit(self, shadow_frame.GetReference(dec_insn.vA));
        break;
      case Instruction::CHECK_CAST: {
        Class* c = mh.ResolveClass(dec_insn.vB);
        Object* obj = shadow_frame.GetReference(dec_insn.vA);
        if (UNLIKELY(obj != NULL && !obj->InstanceOf(c))) {
          self->ThrowNewExceptionF("Ljava/lang/ClassCastException;",
              "%s cannot be cast to %s",
              PrettyDescriptor(obj->GetClass()).c_str(),
              PrettyDescriptor(c).c_str());
        }
        break;
      }
      case Instruction::INSTANCE_OF: {
        Class* c = mh.ResolveClass(dec_insn.vC);
        Object* obj = shadow_frame.GetReference(dec_insn.vB);
        shadow_frame.SetVReg(dec_insn.vA, (obj != NULL && obj->InstanceOf(c)) ? 1 : 0);
        break;
      }
      case Instruction::ARRAY_LENGTH:  {
        Array* array = shadow_frame.GetReference(dec_insn.vB)->AsArray();
        if (UNLIKELY(array == NULL)) {
          ThrowNullPointerExceptionFromDexPC(shadow_frame.GetMethod(), inst->GetDexPc(insns));
          break;
        }
        shadow_frame.SetVReg(dec_insn.vA, array->GetLength());
        break;
      }
      case Instruction::NEW_INSTANCE: {
        Object* obj = AllocObjectFromCode(dec_insn.vB, shadow_frame.GetMethod(), self, true);
        shadow_frame.SetReferenceAndVReg(dec_insn.vA, obj);
        break;
      }
      case Instruction::NEW_ARRAY: {
        int32_t length = shadow_frame.GetVReg(dec_insn.vB);
        Object* obj = AllocArrayFromCode(dec_insn.vC, shadow_frame.GetMethod(), length, self, true);
        shadow_frame.SetReferenceAndVReg(dec_insn.vA, obj);
        break;
      }
      case Instruction::FILLED_NEW_ARRAY:
      case Instruction::FILLED_NEW_ARRAY_RANGE:
        UNIMPLEMENTED(FATAL) << inst->DumpString(&mh.GetDexFile());
        break;
      case Instruction::CMPL_FLOAT: {
        float val1 = shadow_frame.GetVRegFloat(dec_insn.vB);
        float val2 = shadow_frame.GetVRegFloat(dec_insn.vC);
        int32_t result;
        if (val1 == val2) {
          result = 0;
        } else if (val1 > val2) {
          result = 1;
        } else {
          result = -1;
        }
        shadow_frame.SetVReg(dec_insn.vA, result);
        break;
      }
      case Instruction::CMPG_FLOAT: {
        float val1 = shadow_frame.GetVRegFloat(dec_insn.vB);
        float val2 = shadow_frame.GetVRegFloat(dec_insn.vC);
        int32_t result;
        if (val1 == val2) {
          result = 0;
        } else if (val1 < val2) {
          result = -1;
        } else {
          result = 1;
        }
        shadow_frame.SetVReg(dec_insn.vA, result);
        break;
      }
      case Instruction::CMPL_DOUBLE: {
        double val1 = shadow_frame.GetVRegDouble(dec_insn.vB);
        double val2 = shadow_frame.GetVRegDouble(dec_insn.vC);
        int32_t result;
        if (val1 == val2) {
          result = 0;
        } else if (val1 > val2) {
          result = 1;
        } else {
          result = -1;
        }
        shadow_frame.SetVReg(dec_insn.vA, result);
        break;
      }

      case Instruction::CMPG_DOUBLE: {
        double val1 = shadow_frame.GetVRegDouble(dec_insn.vB);
        double val2 = shadow_frame.GetVRegDouble(dec_insn.vC);
        int32_t result;
        if (val1 == val2) {
          result = 0;
        } else if (val1 < val2) {
          result = -1;
        } else {
          result = 1;
        }
        shadow_frame.SetVReg(dec_insn.vA, result);
        break;
      }
      case Instruction::CMP_LONG: {
        int64_t val1 = shadow_frame.GetVRegLong(dec_insn.vB);
        int64_t val2 = shadow_frame.GetVRegLong(dec_insn.vC);
        int32_t result;
        if (val1 < val2) {
          result = -1;
        } else if (val1 == val2) {
          result = 0;
        } else {
          result = 1;
        }
        shadow_frame.SetVReg(dec_insn.vA, result);
        break;
      }
      case Instruction::THROW: {
        Throwable* t = shadow_frame.GetReference(dec_insn.vA)->AsThrowable();
        self->SetException(t);
        break;
      }
      case Instruction::GOTO:
      case Instruction::GOTO_16:
      case Instruction::GOTO_32: {
        uint32_t dex_pc = inst->GetDexPc(insns);
        next_inst = Instruction::At(insns + dex_pc + dec_insn.vA);
        break;
      }
      case Instruction::PACKED_SWITCH:
        UNIMPLEMENTED(FATAL) << inst->DumpString(&mh.GetDexFile());
        break;
      case Instruction::SPARSE_SWITCH: {
        uint32_t dex_pc = inst->GetDexPc(insns);
        const uint16_t* switchData = insns + dex_pc + dec_insn.vB;
        int32_t testVal = shadow_frame.GetVReg(dec_insn.vA);
        CHECK_EQ(switchData[0], static_cast<uint16_t>(Instruction::kSparseSwitchSignature));
        uint16_t size = switchData[1];
        CHECK_GT(size, 0);
        const int32_t* keys = reinterpret_cast<const int32_t*>(&switchData[2]);
        CHECK(IsAligned<4>(keys));
        const int32_t* entries = keys + size;
        CHECK(IsAligned<4>(entries));
        int lo = 0;
        int hi = size - 1;
        while (lo <= hi) {
          int mid = (lo + hi) / 2;
          int32_t foundVal = keys[mid];
          if (testVal < foundVal) {
            hi = mid - 1;
          } else if (testVal > foundVal) {
            lo = mid + 1;
          } else {
            next_inst = Instruction::At(insns + dex_pc + entries[mid]);
            break;
          }
        }
        break;
      }
      case Instruction::FILL_ARRAY_DATA: {
        Array* array = shadow_frame.GetReference(dec_insn.vA)->AsArray();
        if (UNLIKELY(array == NULL)) {
          Thread::Current()->ThrowNewExceptionF("Ljava/lang/NullPointerException;",
              "null array in FILL_ARRAY_DATA");
          break;
        }
        DCHECK(array->IsArrayInstance() && !array->IsObjectArray());
        uint32_t dex_pc = inst->GetDexPc(insns);
        const Instruction::ArrayDataPayload* payload =
            reinterpret_cast<const Instruction::ArrayDataPayload*>(insns + dex_pc + dec_insn.vB);
        if (UNLIKELY(static_cast<int32_t>(payload->element_count) > array->GetLength())) {
          Thread::Current()->ThrowNewExceptionF("Ljava/lang/ArrayIndexOutOfBoundsException;",
                                                "failed FILL_ARRAY_DATA; length=%d, index=%d",
                                                array->GetLength(), payload->element_count);
          break;
        }
        uint32_t size_in_bytes = payload->element_count * payload->element_width;
        memcpy(array->GetRawData(payload->element_width), payload->data, size_in_bytes);
        break;
      }
      case Instruction::IF_EQ: {
        if (shadow_frame.GetVReg(dec_insn.vA) == shadow_frame.GetVReg(dec_insn.vB)) {
          uint32_t dex_pc = inst->GetDexPc(insns);
          next_inst = Instruction::At(insns + dex_pc + dec_insn.vC);
        }
        break;
      }
      case Instruction::IF_NE: {
        if (shadow_frame.GetVReg(dec_insn.vA) != shadow_frame.GetVReg(dec_insn.vB)) {
          uint32_t dex_pc = inst->GetDexPc(insns);
          next_inst = Instruction::At(insns + dex_pc + dec_insn.vC);
        }
        break;
      }
      case Instruction::IF_LT: {
        if (shadow_frame.GetVReg(dec_insn.vA) < shadow_frame.GetVReg(dec_insn.vB)) {
          uint32_t dex_pc = inst->GetDexPc(insns);
          next_inst = Instruction::At(insns + dex_pc + dec_insn.vC);
        }
        break;
      }
      case Instruction::IF_GE: {
        if (shadow_frame.GetVReg(dec_insn.vA) >= shadow_frame.GetVReg(dec_insn.vB)) {
          uint32_t dex_pc = inst->GetDexPc(insns);
          next_inst = Instruction::At(insns + dex_pc + dec_insn.vC);
        }
        break;
      }
      case Instruction::IF_GT: {
        if (shadow_frame.GetVReg(dec_insn.vA) > shadow_frame.GetVReg(dec_insn.vB)) {
          uint32_t dex_pc = inst->GetDexPc(insns);
          next_inst = Instruction::At(insns + dex_pc + dec_insn.vC);
        }
        break;
      }
      case Instruction::IF_LE: {
        if (shadow_frame.GetVReg(dec_insn.vA) <= shadow_frame.GetVReg(dec_insn.vB)) {
          uint32_t dex_pc = inst->GetDexPc(insns);
          next_inst = Instruction::At(insns + dex_pc + dec_insn.vC);
        }
        break;
      }
      case Instruction::IF_EQZ: {
        if (shadow_frame.GetVReg(dec_insn.vA) == 0) {
          uint32_t dex_pc = inst->GetDexPc(insns);
          next_inst = Instruction::At(insns + dex_pc + dec_insn.vB);
        }
        break;
      }
      case Instruction::IF_NEZ: {
        if (shadow_frame.GetVReg(dec_insn.vA) != 0) {
          uint32_t dex_pc = inst->GetDexPc(insns);
          next_inst = Instruction::At(insns + dex_pc + dec_insn.vB);
        }
        break;
      }
      case Instruction::IF_LTZ: {
        if (shadow_frame.GetVReg(dec_insn.vA) < 0) {
          uint32_t dex_pc = inst->GetDexPc(insns);
          next_inst = Instruction::At(insns + dex_pc + dec_insn.vB);
        }
        break;
      }
      case Instruction::IF_GEZ: {
        if (shadow_frame.GetVReg(dec_insn.vA) >= 0) {
          uint32_t dex_pc = inst->GetDexPc(insns);
          next_inst = Instruction::At(insns + dex_pc + dec_insn.vB);
        }
        break;
      }
      case Instruction::IF_GTZ: {
        if (shadow_frame.GetVReg(dec_insn.vA) > 0) {
          uint32_t dex_pc = inst->GetDexPc(insns);
          next_inst = Instruction::At(insns + dex_pc + dec_insn.vB);
        }
        break;
      }
      case Instruction::IF_LEZ:  {
        if (shadow_frame.GetVReg(dec_insn.vA) <= 0) {
          uint32_t dex_pc = inst->GetDexPc(insns);
          next_inst = Instruction::At(insns + dex_pc + dec_insn.vB);
        }
        break;
      }
      case Instruction::AGET_BOOLEAN: {
        BooleanArray* a = shadow_frame.GetReference(dec_insn.vB)->AsBooleanArray();
        if (UNLIKELY(a == NULL)) {
          ThrowNullPointerExceptionFromDexPC(shadow_frame.GetMethod(), inst->GetDexPc(insns));
          break;
        }
        int32_t index = shadow_frame.GetVReg(dec_insn.vC);
        shadow_frame.SetVReg(dec_insn.vA, a->Get(index));
        break;
      }
      case Instruction::AGET_BYTE: {
        ByteArray* a = shadow_frame.GetReference(dec_insn.vB)->AsByteArray();
        if (UNLIKELY(a == NULL)) {
          ThrowNullPointerExceptionFromDexPC(shadow_frame.GetMethod(), inst->GetDexPc(insns));
          break;
        }
        int32_t index = shadow_frame.GetVReg(dec_insn.vC);
        shadow_frame.SetVReg(dec_insn.vA, a->Get(index));
        break;
      }
      case Instruction::AGET_CHAR: {
        CharArray* a = shadow_frame.GetReference(dec_insn.vB)->AsCharArray();
        if (UNLIKELY(a == NULL)) {
          ThrowNullPointerExceptionFromDexPC(shadow_frame.GetMethod(), inst->GetDexPc(insns));
          break;
        }
        int32_t index = shadow_frame.GetVReg(dec_insn.vC);
        shadow_frame.SetVReg(dec_insn.vA, a->Get(index));
        break;
      }
      case Instruction::AGET_SHORT: {
        ShortArray* a = shadow_frame.GetReference(dec_insn.vB)->AsShortArray();
        if (UNLIKELY(a == NULL)) {
          ThrowNullPointerExceptionFromDexPC(shadow_frame.GetMethod(), inst->GetDexPc(insns));
          break;
        }
        int32_t index = shadow_frame.GetVReg(dec_insn.vC);
        shadow_frame.SetVReg(dec_insn.vA, a->Get(index));
        break;
      }
      case Instruction::AGET: {
        IntArray* a = shadow_frame.GetReference(dec_insn.vB)->AsIntArray();
        if (UNLIKELY(a == NULL)) {
          ThrowNullPointerExceptionFromDexPC(shadow_frame.GetMethod(), inst->GetDexPc(insns));
          break;
        }
        int32_t index = shadow_frame.GetVReg(dec_insn.vC);
        shadow_frame.SetVReg(dec_insn.vA, a->Get(index));
        break;
      }
      case Instruction::AGET_WIDE:  {
        LongArray* a = shadow_frame.GetReference(dec_insn.vB)->AsLongArray();
        if (UNLIKELY(a == NULL)) {
          ThrowNullPointerExceptionFromDexPC(shadow_frame.GetMethod(), inst->GetDexPc(insns));
          break;
        }
        int32_t index = shadow_frame.GetVReg(dec_insn.vC);
        shadow_frame.SetVRegLong(dec_insn.vA, a->Get(index));
        break;
      }
      case Instruction::AGET_OBJECT: {
        ObjectArray<Object>* a = shadow_frame.GetReference(dec_insn.vB)->AsObjectArray<Object>();
        if (UNLIKELY(a == NULL)) {
          ThrowNullPointerExceptionFromDexPC(shadow_frame.GetMethod(), inst->GetDexPc(insns));
          break;
        }
        int32_t index = shadow_frame.GetVReg(dec_insn.vC);
        Object* o = a->Get(index);
        shadow_frame.SetReferenceAndVReg(dec_insn.vA, o);
        break;
      }
      case Instruction::APUT_BOOLEAN: {
        uint8_t val = shadow_frame.GetVReg(dec_insn.vA);
        BooleanArray* a = shadow_frame.GetReference(dec_insn.vB)->AsBooleanArray();
        if (UNLIKELY(a == NULL)) {
          ThrowNullPointerExceptionFromDexPC(shadow_frame.GetMethod(), inst->GetDexPc(insns));
          break;
        }
        int32_t index = shadow_frame.GetVReg(dec_insn.vC);
        a->Set(index, val);
        break;
      }
      case Instruction::APUT_BYTE: {
        int8_t val = shadow_frame.GetVReg(dec_insn.vA);
        ByteArray* a = shadow_frame.GetReference(dec_insn.vB)->AsByteArray();
        if (UNLIKELY(a == NULL)) {
          ThrowNullPointerExceptionFromDexPC(shadow_frame.GetMethod(), inst->GetDexPc(insns));
          break;
        }
        int32_t index = shadow_frame.GetVReg(dec_insn.vC);
        a->Set(index, val);
        break;
      }
      case Instruction::APUT_CHAR: {
        uint16_t val = shadow_frame.GetVReg(dec_insn.vA);
        CharArray* a = shadow_frame.GetReference(dec_insn.vB)->AsCharArray();
        if (UNLIKELY(a == NULL)) {
          ThrowNullPointerExceptionFromDexPC(shadow_frame.GetMethod(), inst->GetDexPc(insns));
          break;
        }
        int32_t index = shadow_frame.GetVReg(dec_insn.vC);
        a->Set(index, val);
        break;
      }
      case Instruction::APUT_SHORT: {
        int16_t val = shadow_frame.GetVReg(dec_insn.vA);
        ShortArray* a = shadow_frame.GetReference(dec_insn.vB)->AsShortArray();
        if (UNLIKELY(a == NULL)) {
          ThrowNullPointerExceptionFromDexPC(shadow_frame.GetMethod(), inst->GetDexPc(insns));
          break;
        }
        int32_t index = shadow_frame.GetVReg(dec_insn.vC);
        a->Set(index, val);
        break;
      }
      case Instruction::APUT: {
        int32_t val = shadow_frame.GetVReg(dec_insn.vA);
        IntArray* a = shadow_frame.GetReference(dec_insn.vB)->AsIntArray();
        if (UNLIKELY(a == NULL)) {
          ThrowNullPointerExceptionFromDexPC(shadow_frame.GetMethod(), inst->GetDexPc(insns));
          break;
        }
        int32_t index = shadow_frame.GetVReg(dec_insn.vC);
        a->Set(index, val);
        break;
      }
      case Instruction::APUT_WIDE: {
        int64_t val = shadow_frame.GetVRegLong(dec_insn.vA);
        LongArray* a = shadow_frame.GetReference(dec_insn.vB)->AsLongArray();
        if (UNLIKELY(a == NULL)) {
          ThrowNullPointerExceptionFromDexPC(shadow_frame.GetMethod(), inst->GetDexPc(insns));
          break;
        }
        int32_t index = shadow_frame.GetVReg(dec_insn.vC);
        a->Set(index, val);
        break;
      }
      case Instruction::APUT_OBJECT: {
        Object* val = shadow_frame.GetReference(dec_insn.vA);
        ObjectArray<Object>* a = shadow_frame.GetReference(dec_insn.vB)->AsObjectArray<Object>();
        if (UNLIKELY(a == NULL)) {
          ThrowNullPointerExceptionFromDexPC(shadow_frame.GetMethod(), inst->GetDexPc(insns));
          break;
        }
        int32_t index = shadow_frame.GetVReg(dec_insn.vC);
        a->Set(index, val);
        break;
      }
      case Instruction::IGET_BOOLEAN:
        DoFieldGet(self, shadow_frame, dec_insn, InstancePrimitiveRead, Primitive::kPrimBoolean);
        break;
      case Instruction::IGET_BYTE:
        DoFieldGet(self, shadow_frame, dec_insn, InstancePrimitiveRead, Primitive::kPrimByte);
        break;
      case Instruction::IGET_CHAR:
        DoFieldGet(self, shadow_frame, dec_insn, InstancePrimitiveRead, Primitive::kPrimChar);
        break;
      case Instruction::IGET_SHORT:
        DoFieldGet(self, shadow_frame, dec_insn, InstancePrimitiveRead, Primitive::kPrimShort);
        break;
      case Instruction::IGET:
        DoFieldGet(self, shadow_frame, dec_insn, InstancePrimitiveRead, Primitive::kPrimInt);
        break;
      case Instruction::IGET_WIDE:
        DoFieldGet(self, shadow_frame, dec_insn, InstancePrimitiveRead, Primitive::kPrimLong);
        break;
      case Instruction::IGET_OBJECT:
        DoFieldGet(self, shadow_frame, dec_insn, InstanceObjectRead, Primitive::kPrimNot);
        break;
      case Instruction::SGET_BOOLEAN:
        DoFieldGet(self, shadow_frame, dec_insn, StaticPrimitiveRead, Primitive::kPrimBoolean);
        break;
      case Instruction::SGET_BYTE:
        DoFieldGet(self, shadow_frame, dec_insn, StaticPrimitiveRead, Primitive::kPrimByte);
        break;
      case Instruction::SGET_CHAR:
        DoFieldGet(self, shadow_frame, dec_insn, StaticPrimitiveRead, Primitive::kPrimChar);
        break;
      case Instruction::SGET_SHORT:
        DoFieldGet(self, shadow_frame, dec_insn, StaticPrimitiveRead, Primitive::kPrimShort);
        break;
      case Instruction::SGET:
        DoFieldGet(self, shadow_frame, dec_insn, StaticPrimitiveRead, Primitive::kPrimInt);
        break;
      case Instruction::SGET_WIDE:
        DoFieldGet(self, shadow_frame, dec_insn, StaticPrimitiveRead, Primitive::kPrimLong);
        break;
      case Instruction::SGET_OBJECT:
        DoFieldGet(self, shadow_frame, dec_insn, StaticObjectRead, Primitive::kPrimNot);
        break;
      case Instruction::IPUT_BOOLEAN:
        DoFieldPut(self, shadow_frame, dec_insn, InstancePrimitiveWrite, Primitive::kPrimBoolean);
        break;
      case Instruction::IPUT_BYTE:
        DoFieldPut(self, shadow_frame, dec_insn, InstancePrimitiveWrite, Primitive::kPrimByte);
        break;
      case Instruction::IPUT_CHAR:
        DoFieldPut(self, shadow_frame, dec_insn, InstancePrimitiveWrite, Primitive::kPrimChar);
        break;
      case Instruction::IPUT_SHORT:
        DoFieldPut(self, shadow_frame, dec_insn, InstancePrimitiveWrite, Primitive::kPrimShort);
        break;
      case Instruction::IPUT:
        DoFieldPut(self, shadow_frame, dec_insn, InstancePrimitiveWrite, Primitive::kPrimInt);
        break;
      case Instruction::IPUT_WIDE:
        DoFieldPut(self, shadow_frame, dec_insn, InstancePrimitiveWrite, Primitive::kPrimLong);
        break;
      case Instruction::IPUT_OBJECT:
        DoFieldPut(self, shadow_frame, dec_insn, InstanceObjectWrite, Primitive::kPrimNot);
        break;
      case Instruction::SPUT_BOOLEAN:
        DoFieldPut(self, shadow_frame, dec_insn, StaticPrimitiveWrite, Primitive::kPrimBoolean);
        break;
      case Instruction::SPUT_BYTE:
        DoFieldPut(self, shadow_frame, dec_insn, StaticPrimitiveWrite, Primitive::kPrimByte);
        break;
      case Instruction::SPUT_CHAR:
        DoFieldPut(self, shadow_frame, dec_insn, StaticPrimitiveWrite, Primitive::kPrimChar);
        break;
      case Instruction::SPUT_SHORT:
        DoFieldPut(self, shadow_frame, dec_insn, StaticPrimitiveWrite, Primitive::kPrimShort);
        break;
      case Instruction::SPUT:
        DoFieldPut(self, shadow_frame, dec_insn, StaticPrimitiveWrite, Primitive::kPrimInt);
        break;
      case Instruction::SPUT_WIDE:
        DoFieldPut(self, shadow_frame, dec_insn, StaticPrimitiveWrite, Primitive::kPrimLong);
        break;
      case Instruction::SPUT_OBJECT:
        DoFieldPut(self, shadow_frame, dec_insn, StaticObjectWrite, Primitive::kPrimNot);
        break;
      case Instruction::INVOKE_VIRTUAL:
        DoInvoke(self, mh, shadow_frame, dec_insn, kVirtual, false, &result_register);
        break;
      case Instruction::INVOKE_VIRTUAL_RANGE:
        DoInvoke(self, mh, shadow_frame, dec_insn, kVirtual, true, &result_register);
        break;
      case Instruction::INVOKE_SUPER:
        DoInvoke(self, mh, shadow_frame, dec_insn, kSuper, false, &result_register);
        break;
      case Instruction::INVOKE_SUPER_RANGE:
        DoInvoke(self, mh, shadow_frame, dec_insn, kSuper, true, &result_register);
        break;
      case Instruction::INVOKE_DIRECT:
        DoInvoke(self, mh, shadow_frame, dec_insn, kDirect, false, &result_register);
        break;
      case Instruction::INVOKE_DIRECT_RANGE:
        DoInvoke(self, mh, shadow_frame, dec_insn, kDirect, true, &result_register);
        break;
      case Instruction::INVOKE_INTERFACE:
        DoInvoke(self, mh, shadow_frame, dec_insn, kInterface, false, &result_register);
        break;
      case Instruction::INVOKE_INTERFACE_RANGE:
        DoInvoke(self, mh, shadow_frame, dec_insn, kInterface, true, &result_register);
        break;
      case Instruction::INVOKE_STATIC:
        DoInvoke(self, mh, shadow_frame, dec_insn, kStatic, false, &result_register);
        break;
      case Instruction::INVOKE_STATIC_RANGE:
        DoInvoke(self, mh, shadow_frame, dec_insn, kStatic, true, &result_register);
        break;
      case Instruction::NEG_INT:
        shadow_frame.SetVReg(dec_insn.vA, -shadow_frame.GetVReg(dec_insn.vB));
        break;
      case Instruction::NOT_INT:
        shadow_frame.SetVReg(dec_insn.vA, 0 ^ shadow_frame.GetVReg(dec_insn.vB));
        break;
      case Instruction::NEG_LONG:
        shadow_frame.SetVRegLong(dec_insn.vA, -shadow_frame.GetVRegLong(dec_insn.vB));
        break;
      case Instruction::NOT_LONG:
        shadow_frame.SetVRegLong(dec_insn.vA, 0 ^ shadow_frame.GetVRegLong(dec_insn.vB));
        break;
      case Instruction::NEG_FLOAT:
        shadow_frame.SetVRegFloat(dec_insn.vA, -shadow_frame.GetVRegFloat(dec_insn.vB));
        break;
      case Instruction::NEG_DOUBLE:
        shadow_frame.SetVRegDouble(dec_insn.vA, -shadow_frame.GetVRegDouble(dec_insn.vB));
        break;
      case Instruction::INT_TO_LONG:
        shadow_frame.SetVRegLong(dec_insn.vA, shadow_frame.GetVReg(dec_insn.vB));
        break;
      case Instruction::INT_TO_FLOAT:
        shadow_frame.SetVRegFloat(dec_insn.vA, shadow_frame.GetVReg(dec_insn.vB));
        break;
      case Instruction::INT_TO_DOUBLE:
        shadow_frame.SetVRegDouble(dec_insn.vA, shadow_frame.GetVReg(dec_insn.vB));
        break;
      case Instruction::LONG_TO_INT:
        shadow_frame.SetVReg(dec_insn.vA, shadow_frame.GetVRegLong(dec_insn.vB));
        break;
      case Instruction::LONG_TO_FLOAT:
        shadow_frame.SetVRegFloat(dec_insn.vA, shadow_frame.GetVRegLong(dec_insn.vB));
        break;
      case Instruction::LONG_TO_DOUBLE:
        shadow_frame.SetVRegDouble(dec_insn.vA, shadow_frame.GetVRegLong(dec_insn.vB));
        break;
      case Instruction::FLOAT_TO_INT:
        shadow_frame.SetVReg(dec_insn.vA, shadow_frame.GetVRegFloat(dec_insn.vB));
        break;
      case Instruction::FLOAT_TO_LONG:
        shadow_frame.SetVRegLong(dec_insn.vA, shadow_frame.GetVRegFloat(dec_insn.vB));
        break;
      case Instruction::FLOAT_TO_DOUBLE:
        shadow_frame.SetVRegDouble(dec_insn.vA, shadow_frame.GetVRegFloat(dec_insn.vB));
        break;
      case Instruction::DOUBLE_TO_INT:
        shadow_frame.SetVReg(dec_insn.vA, shadow_frame.GetVRegDouble(dec_insn.vB));
        break;
      case Instruction::DOUBLE_TO_LONG:
        shadow_frame.SetVRegLong(dec_insn.vA, shadow_frame.GetVRegDouble(dec_insn.vB));
        break;
      case Instruction::DOUBLE_TO_FLOAT:
        shadow_frame.SetVRegFloat(dec_insn.vA, shadow_frame.GetVRegDouble(dec_insn.vB));
        break;
      case Instruction::INT_TO_BYTE:
        shadow_frame.SetVReg(dec_insn.vA, static_cast<int8_t>(shadow_frame.GetVReg(dec_insn.vB)));
        break;
      case Instruction::INT_TO_CHAR:
        shadow_frame.SetVReg(dec_insn.vA, static_cast<uint16_t>(shadow_frame.GetVReg(dec_insn.vB)));
        break;
      case Instruction::INT_TO_SHORT:
        shadow_frame.SetVReg(dec_insn.vA, static_cast<int16_t>(shadow_frame.GetVReg(dec_insn.vB)));
        break;
      case Instruction::ADD_INT:
        shadow_frame.SetVReg(dec_insn.vA,
                             shadow_frame.GetVReg(dec_insn.vB) + shadow_frame.GetVReg(dec_insn.vC));
        break;
      case Instruction::SUB_INT:
        shadow_frame.SetVReg(dec_insn.vA,
                             shadow_frame.GetVReg(dec_insn.vB) - shadow_frame.GetVReg(dec_insn.vC));
        break;
      case Instruction::MUL_INT:
        shadow_frame.SetVReg(dec_insn.vA,
                             shadow_frame.GetVReg(dec_insn.vB) * shadow_frame.GetVReg(dec_insn.vC));
        break;
      case Instruction::REM_INT:
        shadow_frame.SetVReg(dec_insn.vA,
                             shadow_frame.GetVReg(dec_insn.vB) % shadow_frame.GetVReg(dec_insn.vC));
        break;
      case Instruction::DIV_INT:
        shadow_frame.SetVReg(dec_insn.vA,
                             shadow_frame.GetVReg(dec_insn.vB) / shadow_frame.GetVReg(dec_insn.vC));
        break;
      case Instruction::SHL_INT:
        shadow_frame.SetVReg(dec_insn.vA,
                             shadow_frame.GetVReg(dec_insn.vB) << shadow_frame.GetVReg(dec_insn.vC));
        break;
      case Instruction::SHR_INT:
        shadow_frame.SetVReg(dec_insn.vA,
                             shadow_frame.GetVReg(dec_insn.vB) >> shadow_frame.GetVReg(dec_insn.vC));
        break;
      case Instruction::USHR_INT:
        shadow_frame.SetVReg(dec_insn.vA,
                             static_cast<uint32_t>(shadow_frame.GetVReg(dec_insn.vB)) >>
                             shadow_frame.GetVReg(dec_insn.vC));
        break;
      case Instruction::AND_INT:
        shadow_frame.SetVReg(dec_insn.vA,
                             shadow_frame.GetVReg(dec_insn.vB) & shadow_frame.GetVReg(dec_insn.vC));
        break;
      case Instruction::OR_INT:
        shadow_frame.SetVReg(dec_insn.vA,
                             shadow_frame.GetVReg(dec_insn.vB) | shadow_frame.GetVReg(dec_insn.vC));
        break;
      case Instruction::XOR_INT:
        shadow_frame.SetVReg(dec_insn.vA,
                             shadow_frame.GetVReg(dec_insn.vB) ^ shadow_frame.GetVReg(dec_insn.vC));
        break;
      case Instruction::ADD_LONG:
        shadow_frame.SetVRegLong(dec_insn.vA,
                                 shadow_frame.GetVRegLong(dec_insn.vB) +
                                 shadow_frame.GetVRegLong(dec_insn.vC));
        break;
      case Instruction::SUB_LONG:
        shadow_frame.SetVRegLong(dec_insn.vA,
                                 shadow_frame.GetVRegLong(dec_insn.vB) -
                                 shadow_frame.GetVRegLong(dec_insn.vC));
        break;
      case Instruction::MUL_LONG:
        shadow_frame.SetVRegLong(dec_insn.vA,
                                 shadow_frame.GetVRegLong(dec_insn.vB) *
                                 shadow_frame.GetVRegLong(dec_insn.vC));
        break;
      case Instruction::DIV_LONG:
        shadow_frame.SetVRegLong(dec_insn.vA,
                                 shadow_frame.GetVRegLong(dec_insn.vB) /
                                 shadow_frame.GetVRegLong(dec_insn.vC));
        break;
      case Instruction::REM_LONG:
        shadow_frame.SetVRegLong(dec_insn.vA,
                                 shadow_frame.GetVRegLong(dec_insn.vB) %
                                 shadow_frame.GetVRegLong(dec_insn.vC));
        break;
      case Instruction::AND_LONG:
        shadow_frame.SetVRegLong(dec_insn.vA,
                                 shadow_frame.GetVRegLong(dec_insn.vB) &
                                 shadow_frame.GetVRegLong(dec_insn.vC));
        break;
      case Instruction::OR_LONG:
        shadow_frame.SetVRegLong(dec_insn.vA,
                                 shadow_frame.GetVRegLong(dec_insn.vB) |
                                 shadow_frame.GetVRegLong(dec_insn.vC));
        break;
      case Instruction::XOR_LONG:
        shadow_frame.SetVRegLong(dec_insn.vA,
                                 shadow_frame.GetVRegLong(dec_insn.vB) ^
                                 shadow_frame.GetVRegLong(dec_insn.vC));
        break;
      case Instruction::SHL_LONG:
        shadow_frame.SetVRegLong(dec_insn.vA,
                                 shadow_frame.GetVRegLong(dec_insn.vB) <<
                                 shadow_frame.GetVReg(dec_insn.vC));
        break;
      case Instruction::SHR_LONG:
        shadow_frame.SetVRegLong(dec_insn.vA,
                                 shadow_frame.GetVRegLong(dec_insn.vB) >>
                                 shadow_frame.GetVReg(dec_insn.vC));
        break;
      case Instruction::USHR_LONG:
        shadow_frame.SetVRegLong(dec_insn.vA,
                                 static_cast<uint64_t>(shadow_frame.GetVRegLong(dec_insn.vB)) >>
                                 shadow_frame.GetVReg(dec_insn.vC));
        break;
      case Instruction::ADD_FLOAT:
        shadow_frame.SetVRegFloat(dec_insn.vA,
                                  shadow_frame.GetVRegFloat(dec_insn.vB) +
                                  shadow_frame.GetVRegFloat(dec_insn.vC));
        break;
      case Instruction::SUB_FLOAT:
        shadow_frame.SetVRegFloat(dec_insn.vA,
                                  shadow_frame.GetVRegFloat(dec_insn.vB) -
                                  shadow_frame.GetVRegFloat(dec_insn.vC));
        break;
      case Instruction::MUL_FLOAT:
        shadow_frame.SetVRegFloat(dec_insn.vA,
                                  shadow_frame.GetVRegFloat(dec_insn.vB) *
                                  shadow_frame.GetVRegFloat(dec_insn.vC));
        break;
      case Instruction::DIV_FLOAT:
        shadow_frame.SetVRegFloat(dec_insn.vA,
                                  shadow_frame.GetVRegFloat(dec_insn.vB) /
                                  shadow_frame.GetVRegFloat(dec_insn.vC));
        break;
      case Instruction::REM_FLOAT:
        shadow_frame.SetVRegFloat(dec_insn.vA,
                                  fmodf(shadow_frame.GetVRegFloat(dec_insn.vB),
                                        shadow_frame.GetVRegFloat(dec_insn.vC)));
        break;
      case Instruction::ADD_DOUBLE:
        shadow_frame.SetVRegDouble(dec_insn.vA,
                                   shadow_frame.GetVRegDouble(dec_insn.vB) +
                                   shadow_frame.GetVRegDouble(dec_insn.vC));
        break;
      case Instruction::SUB_DOUBLE:
        shadow_frame.SetVRegDouble(dec_insn.vA,
                                   shadow_frame.GetVRegDouble(dec_insn.vB) -
                                   shadow_frame.GetVRegDouble(dec_insn.vC));
        break;
      case Instruction::MUL_DOUBLE:
        shadow_frame.SetVRegDouble(dec_insn.vA,
                                   shadow_frame.GetVRegDouble(dec_insn.vB) *
                                   shadow_frame.GetVRegDouble(dec_insn.vC));
        break;
      case Instruction::DIV_DOUBLE:
        shadow_frame.SetVRegDouble(dec_insn.vA,
                                   shadow_frame.GetVRegDouble(dec_insn.vB) /
                                   shadow_frame.GetVRegDouble(dec_insn.vC));
        break;
      case Instruction::REM_DOUBLE:
        shadow_frame.SetVRegDouble(dec_insn.vA,
                                   fmod(shadow_frame.GetVRegDouble(dec_insn.vB),
                                        shadow_frame.GetVRegDouble(dec_insn.vC)));
        break;
      case Instruction::ADD_INT_2ADDR:
        shadow_frame.SetVReg(dec_insn.vA,
                             shadow_frame.GetVReg(dec_insn.vA) + shadow_frame.GetVReg(dec_insn.vB));
        break;
      case Instruction::SUB_INT_2ADDR:
        shadow_frame.SetVReg(dec_insn.vA,
                             shadow_frame.GetVReg(dec_insn.vA) - shadow_frame.GetVReg(dec_insn.vB));
        break;
      case Instruction::MUL_INT_2ADDR:
        shadow_frame.SetVReg(dec_insn.vA,
                             shadow_frame.GetVReg(dec_insn.vA) * shadow_frame.GetVReg(dec_insn.vB));
        break;
      case Instruction::REM_INT_2ADDR:
        shadow_frame.SetVReg(dec_insn.vA,
                             shadow_frame.GetVReg(dec_insn.vA) % shadow_frame.GetVReg(dec_insn.vB));
        break;
      case Instruction::SHL_INT_2ADDR:
        shadow_frame.SetVReg(dec_insn.vA,
                             shadow_frame.GetVReg(dec_insn.vA) << shadow_frame.GetVReg(dec_insn.vB));
        break;
      case Instruction::SHR_INT_2ADDR:
        shadow_frame.SetVReg(dec_insn.vA,
                             shadow_frame.GetVReg(dec_insn.vA) >> shadow_frame.GetVReg(dec_insn.vB));
        break;
      case Instruction::USHR_INT_2ADDR:
        shadow_frame.SetVReg(dec_insn.vA,
                             static_cast<uint32_t>(shadow_frame.GetVReg(dec_insn.vA)) >>
                             shadow_frame.GetVReg(dec_insn.vB));
        break;
      case Instruction::AND_INT_2ADDR:
        shadow_frame.SetVReg(dec_insn.vA,
                             shadow_frame.GetVReg(dec_insn.vA) & shadow_frame.GetVReg(dec_insn.vB));
        break;
      case Instruction::OR_INT_2ADDR:
        shadow_frame.SetVReg(dec_insn.vA,
                             shadow_frame.GetVReg(dec_insn.vA) | shadow_frame.GetVReg(dec_insn.vB));
        break;
      case Instruction::XOR_INT_2ADDR:
        shadow_frame.SetVReg(dec_insn.vA,
                             shadow_frame.GetVReg(dec_insn.vA) ^ shadow_frame.GetVReg(dec_insn.vB));
        break;
      case Instruction::DIV_INT_2ADDR:
        shadow_frame.SetVReg(dec_insn.vA,
                             shadow_frame.GetVReg(dec_insn.vA) / shadow_frame.GetVReg(dec_insn.vB));
        break;
      case Instruction::ADD_LONG_2ADDR:
        shadow_frame.SetVRegLong(dec_insn.vA,
                                 shadow_frame.GetVRegLong(dec_insn.vA) +
                                 shadow_frame.GetVRegLong(dec_insn.vB));
        break;
      case Instruction::SUB_LONG_2ADDR:
        shadow_frame.SetVRegLong(dec_insn.vA,
                                 shadow_frame.GetVRegLong(dec_insn.vA) -
                                 shadow_frame.GetVRegLong(dec_insn.vB));
        break;
      case Instruction::MUL_LONG_2ADDR:
        shadow_frame.SetVRegLong(dec_insn.vA,
                                 shadow_frame.GetVRegLong(dec_insn.vA) +
                                 shadow_frame.GetVRegLong(dec_insn.vB));
        break;
      case Instruction::DIV_LONG_2ADDR:
        shadow_frame.SetVRegLong(dec_insn.vA,
                                 shadow_frame.GetVRegLong(dec_insn.vA) /
                                 shadow_frame.GetVRegLong(dec_insn.vB));
        break;
      case Instruction::REM_LONG_2ADDR:
        shadow_frame.SetVRegLong(dec_insn.vA,
                                 shadow_frame.GetVRegLong(dec_insn.vA) %
                                 shadow_frame.GetVRegLong(dec_insn.vB));
        break;
      case Instruction::AND_LONG_2ADDR:
        shadow_frame.SetVRegLong(dec_insn.vA,
                                 shadow_frame.GetVRegLong(dec_insn.vA) &
                                 shadow_frame.GetVRegLong(dec_insn.vB));
        break;
      case Instruction::OR_LONG_2ADDR:
        shadow_frame.SetVRegLong(dec_insn.vA,
                                 shadow_frame.GetVRegLong(dec_insn.vA) |
                                 shadow_frame.GetVRegLong(dec_insn.vB));
        break;
      case Instruction::XOR_LONG_2ADDR:
        shadow_frame.SetVRegLong(dec_insn.vA,
                                 shadow_frame.GetVRegLong(dec_insn.vA) ^
                                 shadow_frame.GetVRegLong(dec_insn.vB));
        break;
      case Instruction::SHL_LONG_2ADDR:
        shadow_frame.SetVRegLong(dec_insn.vA,
                                 shadow_frame.GetVRegLong(dec_insn.vA) <<
                                 shadow_frame.GetVReg(dec_insn.vB));
        break;
      case Instruction::SHR_LONG_2ADDR:
        shadow_frame.SetVRegLong(dec_insn.vA,
                                 shadow_frame.GetVRegLong(dec_insn.vA) >>
                                 shadow_frame.GetVReg(dec_insn.vB));
        break;
      case Instruction::USHR_LONG_2ADDR:
        shadow_frame.SetVRegLong(dec_insn.vA,
                                 static_cast<uint64_t>(shadow_frame.GetVRegLong(dec_insn.vA)) >>
                                 shadow_frame.GetVReg(dec_insn.vB));
        break;
      case Instruction::ADD_FLOAT_2ADDR:
        shadow_frame.SetVRegFloat(dec_insn.vA,
                                  shadow_frame.GetVRegFloat(dec_insn.vA) +
                                  shadow_frame.GetVRegFloat(dec_insn.vB));
        break;
      case Instruction::SUB_FLOAT_2ADDR:
        shadow_frame.SetVRegFloat(dec_insn.vA,
                                  shadow_frame.GetVRegFloat(dec_insn.vA) -
                                  shadow_frame.GetVRegFloat(dec_insn.vB));
        break;
      case Instruction::MUL_FLOAT_2ADDR:
        shadow_frame.SetVRegFloat(dec_insn.vA,
                                  shadow_frame.GetVRegFloat(dec_insn.vA) *
                                  shadow_frame.GetVRegFloat(dec_insn.vB));
        break;
      case Instruction::DIV_FLOAT_2ADDR:
        shadow_frame.SetVRegFloat(dec_insn.vA,
                                  shadow_frame.GetVRegFloat(dec_insn.vA) /
                                  shadow_frame.GetVRegFloat(dec_insn.vB));
        break;
      case Instruction::REM_FLOAT_2ADDR:
        shadow_frame.SetVRegFloat(dec_insn.vA,
                                  fmodf(shadow_frame.GetVRegFloat(dec_insn.vA),
                                        shadow_frame.GetVRegFloat(dec_insn.vB)));
        break;
      case Instruction::ADD_DOUBLE_2ADDR:
        shadow_frame.SetVRegDouble(dec_insn.vA,
                                   shadow_frame.GetVRegDouble(dec_insn.vA) +
                                   shadow_frame.GetVRegDouble(dec_insn.vB));
        break;
      case Instruction::SUB_DOUBLE_2ADDR:
        shadow_frame.SetVRegDouble(dec_insn.vA,
                                   shadow_frame.GetVRegDouble(dec_insn.vA) -
                                   shadow_frame.GetVRegDouble(dec_insn.vB));
        break;
      case Instruction::MUL_DOUBLE_2ADDR:
        shadow_frame.SetVRegDouble(dec_insn.vA,
                                   shadow_frame.GetVRegDouble(dec_insn.vA) *
                                   shadow_frame.GetVRegDouble(dec_insn.vB));
        break;
      case Instruction::DIV_DOUBLE_2ADDR:
        shadow_frame.SetVRegDouble(dec_insn.vA,
                                   shadow_frame.GetVRegDouble(dec_insn.vA) /
                                   shadow_frame.GetVRegDouble(dec_insn.vB));
        break;
      case Instruction::REM_DOUBLE_2ADDR:
        shadow_frame.SetVRegDouble(dec_insn.vA,
                                   fmod(shadow_frame.GetVRegDouble(dec_insn.vA),
                                        shadow_frame.GetVRegDouble(dec_insn.vB)));
        break;
      case Instruction::ADD_INT_LIT16:
      case Instruction::ADD_INT_LIT8:
        shadow_frame.SetVReg(dec_insn.vA, shadow_frame.GetVReg(dec_insn.vB) + dec_insn.vC);
        break;
      case Instruction::RSUB_INT:
      case Instruction::RSUB_INT_LIT8:
        shadow_frame.SetVReg(dec_insn.vA, dec_insn.vC - shadow_frame.GetVReg(dec_insn.vB));
        break;
      case Instruction::MUL_INT_LIT16:
      case Instruction::MUL_INT_LIT8:
        shadow_frame.SetVReg(dec_insn.vA, shadow_frame.GetVReg(dec_insn.vB) * dec_insn.vC);
        break;
      case Instruction::DIV_INT_LIT16:
      case Instruction::DIV_INT_LIT8:
        shadow_frame.SetVReg(dec_insn.vA, shadow_frame.GetVReg(dec_insn.vB) / dec_insn.vC);
        break;
      case Instruction::REM_INT_LIT16:
      case Instruction::REM_INT_LIT8:
        shadow_frame.SetVReg(dec_insn.vA, shadow_frame.GetVReg(dec_insn.vB) % dec_insn.vC);
        break;
      case Instruction::AND_INT_LIT16:
      case Instruction::AND_INT_LIT8:
        shadow_frame.SetVReg(dec_insn.vA, shadow_frame.GetVReg(dec_insn.vB) & dec_insn.vC);
        break;
      case Instruction::OR_INT_LIT16:
      case Instruction::OR_INT_LIT8:
        shadow_frame.SetVReg(dec_insn.vA, shadow_frame.GetVReg(dec_insn.vB) | dec_insn.vC);
        break;
      case Instruction::XOR_INT_LIT16:
      case Instruction::XOR_INT_LIT8:
        shadow_frame.SetVReg(dec_insn.vA, shadow_frame.GetVReg(dec_insn.vB) ^ dec_insn.vC);
        break;
      case Instruction::SHL_INT_LIT8:
        shadow_frame.SetVReg(dec_insn.vA, shadow_frame.GetVReg(dec_insn.vB) << dec_insn.vC);
        break;
      case Instruction::SHR_INT_LIT8:
        shadow_frame.SetVReg(dec_insn.vA, shadow_frame.GetVReg(dec_insn.vB) >> dec_insn.vC);
        break;
      case Instruction::USHR_INT_LIT8:
        shadow_frame.SetVReg(dec_insn.vA,
                             static_cast<uint32_t>(shadow_frame.GetVReg(dec_insn.vB)) >>
                             dec_insn.vC);
        break;
      default:
        LOG(FATAL) << "Unexpected instruction: " << inst->DumpString(&mh.GetDexFile());
        break;
    }
    if (UNLIKELY(self->IsExceptionPending())) {
      uint32_t found_dex_pc =
          shadow_frame.GetMethod()->FindCatchBlock(self->GetException()->GetClass(),
                                                   inst->GetDexPc(insns));
      if (found_dex_pc == DexFile::kDexNoIndex) {
        JValue result;
        result.SetJ(0);
        return result;  // Handler in caller.
      } else {
        next_inst = Instruction::At(insns + found_dex_pc);
      }
    }
    inst = next_inst;
  }
}

void EnterInterpreterFromInvoke(Thread* self, AbstractMethod* method, Object* receiver,
                                JValue* args, JValue* result) {
  MethodHelper mh(method);
  const DexFile::CodeItem* code_item = mh.GetCodeItem();
  uint16_t num_regs;
  uint16_t num_ins;
  if (code_item != NULL) {
    num_regs =  code_item->registers_size_;
    num_ins = code_item->ins_size_;
  } else {
    DCHECK(method->IsNative());
    num_regs = num_ins = AbstractMethod::NumArgRegisters(mh.GetShorty());
    if (!method->IsStatic()) {
      num_regs++;
      num_ins++;
    }
  }
  // Set up shadow frame with matching number of reference slots to vregs.
  ShadowFrame* last_shadow_frame = self->GetManagedStack()->GetTopShadowFrame();
  UniquePtr<ShadowFrame> shadow_frame(ShadowFrame::Create(num_regs, num_regs,
                                                          (last_shadow_frame == NULL) ? NULL : last_shadow_frame->GetLink(),
                                                          method, 0));
  self->PushShadowFrame(shadow_frame.get());
  size_t cur_reg = num_regs - num_ins;
  if (!method->IsStatic()) {
    CHECK(receiver != NULL);
    shadow_frame->SetReferenceAndVReg(cur_reg, receiver);
    ++cur_reg;
  } else if (!method->GetDeclaringClass()->IsInitializing()) {
    Runtime::Current()->GetClassLinker()->EnsureInitialized(method->GetDeclaringClass(),
                                                            true, true);
    CHECK(method->GetDeclaringClass()->IsInitializing());
  }
  StringPiece shorty(mh.GetShorty());
  size_t arg_pos = 0;
  for (; cur_reg < num_regs; ++cur_reg, ++arg_pos) {
    DCHECK_LT(arg_pos + 1, mh.GetShortyLength());
    switch (shorty[arg_pos + 1]) {
      case 'L': {
        Object* o = args[arg_pos].GetL();
        shadow_frame->SetReferenceAndVReg(cur_reg, o);
        break;
      }
      case 'J': case 'D':
        shadow_frame->SetVRegLong(cur_reg, args[arg_pos].GetJ());
        cur_reg++;
        break;
      default:
        shadow_frame->SetVReg(cur_reg, args[arg_pos].GetI());
        break;
    }
  }
  if (!method->IsNative()) {
    JValue r = Execute(self, mh, code_item, *shadow_frame.get());
    if (result != NULL) {
      *result = r;
    }
  } else {
    // TODO: The following enters JNI code using a typedef-ed function rather than the JNI compiler,
    //       it should be removed and JNI compiled stubs used instead.
    ScopedObjectAccessUnchecked soa(self);
    if (method->IsStatic()) {
      if (shorty == "L") {
        typedef jobject (fnptr)(JNIEnv*, jclass);
        fnptr* fn = reinterpret_cast<fnptr*>(method->GetNativeMethod());
        ScopedLocalRef<jclass> klass(soa.Env(),
                                     soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
        ScopedThreadStateChange tsc(self, kNative);
        result->SetL(soa.Decode<Object*>(fn(soa.Env(), klass.get())));
      } else if (shorty == "V") {
        typedef void (fnptr)(JNIEnv*, jclass);
        fnptr* fn = reinterpret_cast<fnptr*>(method->GetNativeMethod());
        ScopedLocalRef<jclass> klass(soa.Env(),
                                     soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
        ScopedThreadStateChange tsc(self, kNative);
        fn(soa.Env(), klass.get());
      } else if (shorty == "Z") {
        typedef jboolean (fnptr)(JNIEnv*, jclass);
        fnptr* fn = reinterpret_cast<fnptr*>(method->GetNativeMethod());
        ScopedLocalRef<jclass> klass(soa.Env(),
                                     soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
        ScopedThreadStateChange tsc(self, kNative);
        result->SetZ(fn(soa.Env(), klass.get()));
      } else if (shorty == "BI") {
        typedef jbyte (fnptr)(JNIEnv*, jclass, jint);
        fnptr* fn = reinterpret_cast<fnptr*>(method->GetNativeMethod());
        ScopedLocalRef<jclass> klass(soa.Env(),
                                     soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
        ScopedThreadStateChange tsc(self, kNative);
        result->SetB(fn(soa.Env(), klass.get(), args[0].GetI()));
      } else if (shorty == "II") {
        typedef jint (fnptr)(JNIEnv*, jclass, jint);
        fnptr* fn = reinterpret_cast<fnptr*>(method->GetNativeMethod());
        ScopedLocalRef<jclass> klass(soa.Env(),
                                     soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
        ScopedThreadStateChange tsc(self, kNative);
        result->SetI(fn(soa.Env(), klass.get(), args[0].GetI()));
      } else if (shorty == "LL") {
        typedef jobject (fnptr)(JNIEnv*, jclass, jobject);
        fnptr* fn = reinterpret_cast<fnptr*>(method->GetNativeMethod());
        ScopedLocalRef<jclass> klass(soa.Env(),
                                     soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
        ScopedLocalRef<jobject> arg0(soa.Env(),
                                     soa.AddLocalReference<jobject>(args[0].GetL()));
        ScopedThreadStateChange tsc(self, kNative);
        result->SetL(soa.Decode<Object*>(fn(soa.Env(), klass.get(), arg0.get())));
      } else if (shorty == "IIZ") {
        typedef jint (fnptr)(JNIEnv*, jclass, jint, jboolean);
        fnptr* fn = reinterpret_cast<fnptr*>(method->GetNativeMethod());
        ScopedLocalRef<jclass> klass(soa.Env(),
                                     soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
        ScopedThreadStateChange tsc(self, kNative);
        result->SetI(fn(soa.Env(), klass.get(), args[0].GetI(), args[1].GetZ()));
      } else if (shorty == "ILI") {
        typedef jint (fnptr)(JNIEnv*, jclass, jobject, jint);
        fnptr* fn = reinterpret_cast<fnptr*>(method->GetNativeMethod());
        ScopedLocalRef<jclass> klass(soa.Env(),
                                     soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
        ScopedLocalRef<jobject> arg0(soa.Env(),
                                     soa.AddLocalReference<jobject>(args[0].GetL()));
        ScopedThreadStateChange tsc(self, kNative);
        result->SetI(fn(soa.Env(), klass.get(), arg0.get(), args[1].GetI()));
      } else if (shorty == "SIZ") {
        typedef jshort (fnptr)(JNIEnv*, jclass, jint, jboolean);
        fnptr* fn = reinterpret_cast<fnptr*>(method->GetNativeMethod());
        ScopedLocalRef<jclass> klass(soa.Env(),
                                     soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
        ScopedThreadStateChange tsc(self, kNative);
        result->SetS(fn(soa.Env(), klass.get(), args[0].GetI(), args[1].GetZ()));
      } else if (shorty == "VIZ") {
        typedef void (fnptr)(JNIEnv*, jclass, jint, jboolean);
        fnptr* fn = reinterpret_cast<fnptr*>(method->GetNativeMethod());
        ScopedLocalRef<jclass> klass(soa.Env(),
                                     soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
        ScopedThreadStateChange tsc(self, kNative);
        fn(soa.Env(), klass.get(), args[0].GetI(), args[1].GetZ());
      } else if (shorty == "ZLL") {
        typedef jboolean (fnptr)(JNIEnv*, jclass, jobject, jobject);
        fnptr* fn = reinterpret_cast<fnptr*>(method->GetNativeMethod());
        ScopedLocalRef<jclass> klass(soa.Env(),
                                     soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
        ScopedLocalRef<jobject> arg0(soa.Env(),
                                     soa.AddLocalReference<jobject>(args[0].GetL()));
        ScopedLocalRef<jobject> arg1(soa.Env(),
                                     soa.AddLocalReference<jobject>(args[1].GetL()));
        ScopedThreadStateChange tsc(self, kNative);
        result->SetZ(fn(soa.Env(), klass.get(), arg0.get(), arg1.get()));
      } else if (shorty == "ZILL") {
        typedef jboolean (fnptr)(JNIEnv*, jclass, jint, jobject, jobject);
        fnptr* fn = reinterpret_cast<fnptr*>(method->GetNativeMethod());
        ScopedLocalRef<jclass> klass(soa.Env(),
                                     soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
        ScopedLocalRef<jobject> arg1(soa.Env(),
                                     soa.AddLocalReference<jobject>(args[1].GetL()));
        ScopedLocalRef<jobject> arg2(soa.Env(),
                                     soa.AddLocalReference<jobject>(args[2].GetL()));
        ScopedThreadStateChange tsc(self, kNative);
        result->SetZ(fn(soa.Env(), klass.get(), args[0].GetI(), arg1.get(), arg2.get()));
      } else if (shorty == "VILII") {
        typedef void (fnptr)(JNIEnv*, jclass, jint, jobject, jint, jint);
        fnptr* fn = reinterpret_cast<fnptr*>(method->GetNativeMethod());
        ScopedLocalRef<jclass> klass(soa.Env(),
                                     soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
        ScopedLocalRef<jobject> arg1(soa.Env(),
                                     soa.AddLocalReference<jobject>(args[1].GetL()));
        ScopedThreadStateChange tsc(self, kNative);
        fn(soa.Env(), klass.get(), args[0].GetI(), arg1.get(), args[2].GetI(), args[3].GetI());
      } else if (shorty == "VLILII") {
        typedef void (fnptr)(JNIEnv*, jclass, jobject, jint, jobject, jint, jint);
        fnptr* fn = reinterpret_cast<fnptr*>(method->GetNativeMethod());
        ScopedLocalRef<jclass> klass(soa.Env(),
                                     soa.AddLocalReference<jclass>(method->GetDeclaringClass()));
        ScopedLocalRef<jobject> arg0(soa.Env(),
                                     soa.AddLocalReference<jobject>(args[0].GetL()));
        ScopedLocalRef<jobject> arg2(soa.Env(),
                                     soa.AddLocalReference<jobject>(args[2].GetL()));
        ScopedThreadStateChange tsc(self, kNative);
        fn(soa.Env(), klass.get(), arg0.get(), args[1].GetI(), arg2.get(), args[3].GetI(),
           args[4].GetI());
      } else {
        LOG(FATAL) << "Do something with static native method: " << PrettyMethod(method)
            << " shorty: " << shorty;
      }
    } else {
      if (shorty == "L") {
        typedef jobject (fnptr)(JNIEnv*, jobject);
        fnptr* fn = reinterpret_cast<fnptr*>(method->GetNativeMethod());
        ScopedLocalRef<jobject> rcvr(soa.Env(),
                                     soa.AddLocalReference<jobject>(receiver));
        ScopedThreadStateChange tsc(self, kNative);
        result->SetL(soa.Decode<Object*>(fn(soa.Env(), rcvr.get())));
      } else if (shorty == "LL") {
        typedef jobject (fnptr)(JNIEnv*, jobject, jobject);
        fnptr* fn = reinterpret_cast<fnptr*>(method->GetNativeMethod());
        ScopedLocalRef<jobject> rcvr(soa.Env(),
                                     soa.AddLocalReference<jobject>(receiver));
        ScopedLocalRef<jobject> arg0(soa.Env(),
                                     soa.AddLocalReference<jobject>(args[0].GetL()));
        ScopedThreadStateChange tsc(self, kNative);
        result->SetL(soa.Decode<Object*>(fn(soa.Env(), rcvr.get(), arg0.get())));
      } else if (shorty == "III") {
        typedef jint (fnptr)(JNIEnv*, jobject, jint, jint);
        fnptr* fn = reinterpret_cast<fnptr*>(method->GetNativeMethod());
        ScopedLocalRef<jobject> rcvr(soa.Env(),
                                     soa.AddLocalReference<jobject>(receiver));
        ScopedThreadStateChange tsc(self, kNative);
        result->SetI(fn(soa.Env(), rcvr.get(), args[0].GetI(), args[1].GetI()));
      } else {
        LOG(FATAL) << "Do something with native method: " << PrettyMethod(method)
            << " shorty: " << shorty;
      }
    }
  }
  self->PopShadowFrame();
}

}  // namespace interpreter
}  // namespace art
