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

#include "throwable.h"

#include "art_method-inl.h"
#include "class-inl.h"
#include "dex_file-inl.h"
#include "gc/accounting/card_table-inl.h"
#include "object-inl.h"
#include "object_array.h"
#include "object_array-inl.h"
#include "stack_trace_element.h"
#include "utils.h"
#include "well_known_classes.h"

namespace art {
namespace mirror {

GcRoot<Class> Throwable::java_lang_Throwable_;

void Throwable::SetDetailMessage(String* new_detail_message) {
  if (Runtime::Current()->IsActiveTransaction()) {
    SetFieldObject<true>(OFFSET_OF_OBJECT_MEMBER(Throwable, detail_message_), new_detail_message);
  } else {
    SetFieldObject<false>(OFFSET_OF_OBJECT_MEMBER(Throwable, detail_message_),
                          new_detail_message);
  }
}

void Throwable::SetCause(Throwable* cause) {
  CHECK(cause != nullptr);
  CHECK(cause != this);
  Throwable* current_cause = GetFieldObject<Throwable>(OFFSET_OF_OBJECT_MEMBER(Throwable, cause_));
  CHECK(current_cause == NULL || current_cause == this);
  if (Runtime::Current()->IsActiveTransaction()) {
    SetFieldObject<true>(OFFSET_OF_OBJECT_MEMBER(Throwable, cause_), cause);
  } else {
    SetFieldObject<false>(OFFSET_OF_OBJECT_MEMBER(Throwable, cause_), cause);
  }
}

void Throwable::SetStackState(Object* state) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  CHECK(state != nullptr);
  if (Runtime::Current()->IsActiveTransaction()) {
    SetFieldObjectVolatile<true>(OFFSET_OF_OBJECT_MEMBER(Throwable, stack_state_), state);
  } else {
    SetFieldObjectVolatile<false>(OFFSET_OF_OBJECT_MEMBER(Throwable, stack_state_), state);
  }
}

bool Throwable::IsCheckedException() {
  if (InstanceOf(WellKnownClasses::ToClass(WellKnownClasses::java_lang_Error))) {
    return false;
  }
  return !InstanceOf(WellKnownClasses::ToClass(WellKnownClasses::java_lang_RuntimeException));
}

std::string Throwable::Dump() {
  std::string result(PrettyTypeOf(this));
  result += ": ";
  String* msg = GetDetailMessage();
  if (msg != NULL) {
    result += msg->ToModifiedUtf8();
  }
  result += "\n";
  Object* stack_state = GetStackState();
  // check stack state isn't missing or corrupt
  if (stack_state != nullptr && stack_state->IsObjectArray()) {
    // Decode the internal stack trace into the depth and method trace
    ObjectArray<Object>* method_trace = down_cast<ObjectArray<Object>*>(stack_state);
    int32_t depth = method_trace->GetLength() - 1;
    IntArray* pc_trace = down_cast<IntArray*>(method_trace->Get(depth));
    if (depth == 0) {
      result += "(Throwable with empty stack trace)";
    } else {
      for (int32_t i = 0; i < depth; ++i) {
        mirror::ArtMethod* method = down_cast<ArtMethod*>(method_trace->Get(i));
        uint32_t dex_pc = pc_trace->Get(i);
        int32_t line_number = method->GetLineNumFromDexPC(dex_pc);
        const char* source_file = method->GetDeclaringClassSourceFile();
        result += StringPrintf("  at %s (%s:%d)\n", PrettyMethod(method, true).c_str(),
                               source_file, line_number);
      }
    }
  } else {
    Object* stack_trace = GetStackTrace();
    if (stack_trace != nullptr && stack_trace->IsObjectArray()) {
      CHECK_EQ(stack_trace->GetClass()->GetComponentType(),
               StackTraceElement::GetStackTraceElement());
      ObjectArray<StackTraceElement>* ste_array =
          down_cast<ObjectArray<StackTraceElement>*>(stack_trace);
      if (ste_array->GetLength() == 0) {
        result += "(Throwable with empty stack trace)";
      } else {
        for (int32_t i = 0; i < ste_array->GetLength(); ++i) {
          StackTraceElement* ste = ste_array->Get(i);
          result += StringPrintf("  at %s (%s:%d)\n",
                                 ste->GetMethodName()->ToModifiedUtf8().c_str(),
                                 ste->GetFileName()->ToModifiedUtf8().c_str(),
                                 ste->GetLineNumber());
        }
      }
    } else {
      result += "(Throwable with no stack trace)";
    }
  }
  Throwable* cause = GetFieldObject<Throwable>(OFFSET_OF_OBJECT_MEMBER(Throwable, cause_));
  if (cause != nullptr && cause != this) {  // Constructor makes cause == this by default.
    result += "Caused by: ";
    result += cause->Dump();
  }
  return result;
}

void Throwable::SetClass(Class* java_lang_Throwable) {
  CHECK(java_lang_Throwable_.IsNull());
  CHECK(java_lang_Throwable != NULL);
  java_lang_Throwable_ = GcRoot<Class>(java_lang_Throwable);
}

void Throwable::ResetClass() {
  CHECK(!java_lang_Throwable_.IsNull());
  java_lang_Throwable_ = GcRoot<Class>(nullptr);
}

void Throwable::VisitRoots(RootCallback* callback, void* arg) {
  if (!java_lang_Throwable_.IsNull()) {
    java_lang_Throwable_.VisitRoot(callback, arg, 0, kRootStickyClass);
  }
}

}  // namespace mirror
}  // namespace art
