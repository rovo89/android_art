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

#include "common_throws.h"

#include "base/logging.h"
#include "class_linker-inl.h"
#include "dex_file-inl.h"
#include "dex_instruction.h"
#include "invoke_type.h"
#include "mirror/abstract_method-inl.h"
#include "mirror/class-inl.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "object_utils.h"
#include "thread.h"

#include <sstream>

namespace art {

static void AddReferrerLocation(std::ostream& os, const mirror::AbstractMethod* referrer)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  if (referrer != NULL) {
    ClassHelper kh(referrer->GetDeclaringClass());
    std::string location(kh.GetLocation());
    if (!location.empty()) {
      os << " (accessed from " << location << ")";
    }
  }
}

static void AddReferrerLocationFromClass(std::ostream& os, mirror::Class* referrer)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  if (referrer != NULL) {
    ClassHelper kh(referrer);
    std::string location(kh.GetLocation());
    if (!location.empty()) {
      os << " (declaration of '" << PrettyDescriptor(referrer)
            << "' appears in " << location << ")";
    }
  }
}

// NullPointerException

void ThrowNullPointerExceptionForFieldAccess(mirror::Field* field, bool is_read) {
  std::ostringstream msg;
  msg << "Attempt to " << (is_read ? "read from" : "write to")
      << " field '" << PrettyField(field, true) << "' on a null object reference";
  Thread::Current()->ThrowNewException("Ljava/lang/NullPointerException;", msg.str().c_str());
}

void ThrowNullPointerExceptionForMethodAccess(mirror::AbstractMethod* caller, uint32_t method_idx,
                                              InvokeType type) {
  mirror::DexCache* dex_cache = caller->GetDeclaringClass()->GetDexCache();
  const DexFile& dex_file = *dex_cache->GetDexFile();
  std::ostringstream msg;
  msg << "Attempt to invoke " << type << " method '"
      << PrettyMethod(method_idx, dex_file, true) << "' on a null object reference";
  Thread::Current()->ThrowNewException("Ljava/lang/NullPointerException;", msg.str().c_str());
}

void ThrowNullPointerExceptionFromDexPC(mirror::AbstractMethod* throw_method, uint32_t dex_pc) {
  const DexFile::CodeItem* code = MethodHelper(throw_method).GetCodeItem();
  CHECK_LT(dex_pc, code->insns_size_in_code_units_);
  const Instruction* instr = Instruction::At(&code->insns_[dex_pc]);
  DecodedInstruction dec_insn(instr);
  switch (instr->Opcode()) {
    case Instruction::INVOKE_DIRECT:
    case Instruction::INVOKE_DIRECT_RANGE:
      ThrowNullPointerExceptionForMethodAccess(throw_method, dec_insn.vB, kDirect);
      break;
    case Instruction::INVOKE_VIRTUAL:
    case Instruction::INVOKE_VIRTUAL_RANGE:
      ThrowNullPointerExceptionForMethodAccess(throw_method, dec_insn.vB, kVirtual);
      break;
    case Instruction::INVOKE_INTERFACE:
    case Instruction::INVOKE_INTERFACE_RANGE:
      ThrowNullPointerExceptionForMethodAccess(throw_method, dec_insn.vB, kInterface);
      break;
    case Instruction::IGET:
    case Instruction::IGET_WIDE:
    case Instruction::IGET_OBJECT:
    case Instruction::IGET_BOOLEAN:
    case Instruction::IGET_BYTE:
    case Instruction::IGET_CHAR:
    case Instruction::IGET_SHORT: {
      mirror::Field* field =
          Runtime::Current()->GetClassLinker()->ResolveField(dec_insn.vC, throw_method, false);
      ThrowNullPointerExceptionForFieldAccess(field, true /* read */);
      break;
    }
    case Instruction::IPUT:
    case Instruction::IPUT_WIDE:
    case Instruction::IPUT_OBJECT:
    case Instruction::IPUT_BOOLEAN:
    case Instruction::IPUT_BYTE:
    case Instruction::IPUT_CHAR:
    case Instruction::IPUT_SHORT: {
      mirror::Field* field =
          Runtime::Current()->GetClassLinker()->ResolveField(dec_insn.vC, throw_method, false);
      ThrowNullPointerExceptionForFieldAccess(field, false /* write */);
      break;
    }
    case Instruction::AGET:
    case Instruction::AGET_WIDE:
    case Instruction::AGET_OBJECT:
    case Instruction::AGET_BOOLEAN:
    case Instruction::AGET_BYTE:
    case Instruction::AGET_CHAR:
    case Instruction::AGET_SHORT:
      Thread::Current()->ThrowNewException("Ljava/lang/NullPointerException;",
                                           "Attempt to read from null array");
      break;
    case Instruction::APUT:
    case Instruction::APUT_WIDE:
    case Instruction::APUT_OBJECT:
    case Instruction::APUT_BOOLEAN:
    case Instruction::APUT_BYTE:
    case Instruction::APUT_CHAR:
    case Instruction::APUT_SHORT:
      Thread::Current()->ThrowNewException("Ljava/lang/NullPointerException;",
                                           "Attempt to write to null array");
      break;
    case Instruction::ARRAY_LENGTH:
      Thread::Current()->ThrowNewException("Ljava/lang/NullPointerException;",
                                           "Attempt to get length of null array");
      break;
    default: {
      // TODO: We should have covered all the cases where we expect a NPE above, this
      //       message/logging is so we can improve any cases we've missed in the future.
      const DexFile& dex_file = *throw_method->GetDeclaringClass()->GetDexCache()->GetDexFile();
      std::string message("Null pointer exception during instruction '");
      message += instr->DumpString(&dex_file);
      message += "'";
      Thread::Current()->ThrowNewException("Ljava/lang/NullPointerException;", message.c_str());
      break;
    }
  }
}

// IllegalAccessError

void ThrowIllegalAccessErrorClass(mirror::Class* referrer, mirror::Class* accessed) {
  std::ostringstream msg;
  msg << "Illegal class access: '" << PrettyDescriptor(referrer) << "' attempting to access '"
      << PrettyDescriptor(accessed) << "'";
  AddReferrerLocationFromClass(msg, referrer);
  Thread::Current()->ThrowNewException("Ljava/lang/IllegalAccessError;", msg.str().c_str());
}

void ThrowIllegalAccessErrorClassForMethodDispatch(mirror::Class* referrer, mirror::Class* accessed,
                                                   const mirror::AbstractMethod* caller,
                                                   const mirror::AbstractMethod* called,
                                                   InvokeType type) {
  std::ostringstream msg;
  msg << "Illegal class access ('" << PrettyDescriptor(referrer) << "' attempting to access '"
      << PrettyDescriptor(accessed) << "') in attempt to invoke " << type
      << " method " << PrettyMethod(called).c_str();
  AddReferrerLocation(msg, caller);
  Thread::Current()->ThrowNewException("Ljava/lang/IllegalAccessError;", msg.str().c_str());
}

void ThrowIllegalAccessErrorMethod(mirror::Class* referrer, mirror::AbstractMethod* accessed) {
  std::ostringstream msg;
  msg << "Method '" << PrettyMethod(accessed) << "' is inaccessible to class '"
      << PrettyDescriptor(referrer) << "'";
  AddReferrerLocationFromClass(msg, referrer);
  Thread::Current()->ThrowNewException("Ljava/lang/IllegalAccessError;", msg.str().c_str());
}

void ThrowIllegalAccessErrorField(mirror::Class* referrer, mirror::Field* accessed) {
  std::ostringstream msg;
  msg << "Field '" << PrettyField(accessed, false) << "' is inaccessible to class '"
      << PrettyDescriptor(referrer) << "'";
  AddReferrerLocationFromClass(msg, referrer);
  Thread::Current()->ThrowNewException("Ljava/lang/IllegalAccessError;", msg.str().c_str());
}

void ThrowIllegalAccessErrorFinalField(const mirror::AbstractMethod* referrer,
                                       mirror::Field* accessed) {
  std::ostringstream msg;
  msg << "Final field '" << PrettyField(accessed, false) << "' cannot be written to by method '"
      << PrettyMethod(referrer) << "'";
  AddReferrerLocation(msg, referrer);
  Thread::Current()->ThrowNewException("Ljava/lang/IllegalAccessError;", msg.str().c_str());
}

// IncompatibleClassChangeError

void ThrowIncompatibleClassChangeError(InvokeType expected_type, InvokeType found_type,
                                       mirror::AbstractMethod* method,
                                       const mirror::AbstractMethod* referrer) {
  std::ostringstream msg;
  msg << "The method '" << PrettyMethod(method) << "' was expected to be of type "
      << expected_type << " but instead was found to be of type " << found_type;
  AddReferrerLocation(msg, referrer);
  Thread::Current()->ThrowNewException("Ljava/lang/IncompatibleClassChangeError;",
                                       msg.str().c_str());
}

void ThrowIncompatibleClassChangeErrorClassForInterfaceDispatch(const mirror::AbstractMethod* interface_method,
                                                                mirror::Object* this_object,
                                                                const mirror::AbstractMethod* referrer) {
  // Referrer is calling interface_method on this_object, however, the interface_method isn't
  // implemented by this_object.
  CHECK(this_object != NULL);
  std::ostringstream msg;
  msg << "Class '" << PrettyDescriptor(this_object->GetClass())
      << "' does not implement interface '"
      << PrettyDescriptor(interface_method->GetDeclaringClass())
      << "' in call to '" << PrettyMethod(interface_method) << "'";
  AddReferrerLocation(msg, referrer);
  Thread::Current()->ThrowNewException("Ljava/lang/IncompatibleClassChangeError;",
                                       msg.str().c_str());
}

void ThrowIncompatibleClassChangeErrorField(const mirror::Field* resolved_field, bool is_static,
                                            const mirror::AbstractMethod* referrer) {
  std::ostringstream msg;
  msg << "Expected '" << PrettyField(resolved_field) << "' to be a "
      << (is_static ? "static" : "instance") << " field" << " rather than a "
      << (is_static ? "instance" : "static") << " field";
  AddReferrerLocation(msg, referrer);
  Thread::Current()->ThrowNewException("Ljava/lang/IncompatibleClassChangeError;",
                                       msg.str().c_str());
}

// NoSuchMethodError

void ThrowNoSuchMethodError(InvokeType type, mirror::Class* c, const StringPiece& name,
                            const StringPiece& signature, const mirror::AbstractMethod* referrer) {
  std::ostringstream msg;
  ClassHelper kh(c);
  msg << "No " << type << " method " << name << signature
      << " in class " << kh.GetDescriptor() << " or its super classes";
  AddReferrerLocation(msg, referrer);
  Thread::Current()->ThrowNewException("Ljava/lang/NoSuchMethodError;", msg.str().c_str());
}

void ThrowNoSuchMethodError(uint32_t method_idx, const mirror::AbstractMethod* referrer) {
  mirror::DexCache* dex_cache = referrer->GetDeclaringClass()->GetDexCache();
  const DexFile& dex_file = *dex_cache->GetDexFile();
  std::ostringstream msg;
  msg << "No method '" << PrettyMethod(method_idx, dex_file, true) << "'";
  AddReferrerLocation(msg, referrer);
  Thread::Current()->ThrowNewException("Ljava/lang/NoSuchMethodError;", msg.str().c_str());
}

}  // namespace art
