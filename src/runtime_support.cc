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

#include "runtime_support.h"

#include "ScopedLocalRef.h"
#include "well_known_classes.h"

double art_l2d(int64_t l) {
  return static_cast<double>(l);
}

float art_l2f(int64_t l) {
  return static_cast<float>(l);
}

/*
 * Float/double conversion requires clamping to min and max of integer form.  If
 * target doesn't support this normally, use these.
 */
int64_t art_d2l(double d) {
  static const double kMaxLong = static_cast<double>(static_cast<int64_t>(0x7fffffffffffffffULL));
  static const double kMinLong = static_cast<double>(static_cast<int64_t>(0x8000000000000000ULL));
  if (d >= kMaxLong) {
    return static_cast<int64_t>(0x7fffffffffffffffULL);
  } else if (d <= kMinLong) {
    return static_cast<int64_t>(0x8000000000000000ULL);
  } else if (d != d)  { // NaN case
    return 0;
  } else {
    return static_cast<int64_t>(d);
  }
}

int64_t art_f2l(float f) {
  static const float kMaxLong = static_cast<float>(static_cast<int64_t>(0x7fffffffffffffffULL));
  static const float kMinLong = static_cast<float>(static_cast<int64_t>(0x8000000000000000ULL));
  if (f >= kMaxLong) {
    return static_cast<int64_t>(0x7fffffffffffffffULL);
  } else if (f <= kMinLong) {
    return static_cast<int64_t>(0x8000000000000000ULL);
  } else if (f != f) { // NaN case
    return 0;
  } else {
    return static_cast<int64_t>(f);
  }
}

int32_t art_d2i(double d) {
  static const double kMaxInt = static_cast<double>(static_cast<int32_t>(0x7fffffffUL));
  static const double kMinInt = static_cast<double>(static_cast<int32_t>(0x80000000UL));
  if (d >= kMaxInt) {
    return static_cast<int32_t>(0x7fffffffUL);
  } else if (d <= kMinInt) {
    return static_cast<int32_t>(0x80000000UL);
  } else if (d != d)  { // NaN case
    return 0;
  } else {
    return static_cast<int32_t>(d);
  }
}

int32_t art_f2i(float f) {
  static const float kMaxInt = static_cast<float>(static_cast<int32_t>(0x7fffffffUL));
  static const float kMinInt = static_cast<float>(static_cast<int32_t>(0x80000000UL));
  if (f >= kMaxInt) {
    return static_cast<int32_t>(0x7fffffffUL);
  } else if (f <= kMinInt) {
    return static_cast<int32_t>(0x80000000UL);
  } else if (f != f) { // NaN case
    return 0;
  } else {
    return static_cast<int32_t>(f);
  }
}

namespace art {

void ThrowNewIllegalAccessErrorClass(Thread* self,
                                     Class* referrer,
                                     Class* accessed) {
  self->ThrowNewExceptionF("Ljava/lang/IllegalAccessError;",
                           "Illegal class access: '%s' -> '%s'",
                           PrettyDescriptor(referrer).c_str(),
                           PrettyDescriptor(accessed).c_str());
}

void ThrowNewIllegalAccessErrorClassForMethodDispatch(Thread* self,
                                                      Class* referrer,
                                                      Class* accessed,
                                                      const Method* caller,
                                                      const Method* called,
                                                      InvokeType type) {
  self->ThrowNewExceptionF("Ljava/lang/IllegalAccessError;",
                           "Illegal class access ('%s' -> '%s')"
                           "in attempt to invoke %s method '%s' from '%s'",
                           PrettyDescriptor(referrer).c_str(),
                           PrettyDescriptor(accessed).c_str(),
                           ToStr<InvokeType>(type).c_str(),
                           PrettyMethod(called).c_str(),
                           PrettyMethod(caller).c_str());
}

static void ThrowNewIncompatibleClassChangeErrorClassForInterfaceDispatch(Thread* self,
                                                                          const Method* interface_method,
                                                                          Object* this_object)
    SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_) {
  std::string interface_method_name(PrettyMethod(interface_method));
  if (this_object != NULL) {
    std::string this_class_descriptor(PrettyDescriptor(this_object->GetClass()));
    std::string interface_class_descriptor(PrettyDescriptor(interface_method->GetDeclaringClass()));
    self->ThrowNewExceptionF("Ljava/lang/IncompatibleClassChangeError;",
                             "Class '%s' does not implement interface '%s' in call to '%s'",
                             this_class_descriptor.c_str(),
                             interface_class_descriptor.c_str(),
                             interface_method_name.c_str());
  } else {
    self->ThrowNewExceptionF("Ljava/lang/IncompatibleClassChangeError;",
                             "Expected '%s' to be an interface method",
                             interface_method_name.c_str());
  }
}

static void ThrowNewIncompatibleClassChangeErrorField(Thread* self, const Field* resolved_field,
                                               bool is_static)
    SHARED_LOCKS_REQUIRED(GlobalSynchronization::mutator_lock_) {
  self->ThrowNewExceptionF("Ljava/lang/IncompatibleClassChangeError;",
                           "Expected '%s' to be a %s field",
                           PrettyField(resolved_field).c_str(),
                           is_static ? "static" : "instance");
}

void ThrowIncompatibleClassChangeError(InvokeType expected_type, InvokeType found_type,
                                       Method* method, const Method* referrer) {
  std::ostringstream msg;
  msg << "The method '" << PrettyMethod(method) << "' was expected to be of type "
      << expected_type << " but instead was found to be of type " << found_type;
  if (referrer != NULL) {
    ClassHelper kh(referrer->GetDeclaringClass());
    std::string location(kh.GetLocation());
    if (!location.empty()) {
      msg << " (accessed from " << location << ")";
    }
  }
  Thread::Current()->ThrowNewException("Ljava/lang/IncompatibleClassChangeError;",
                                       msg.str().c_str());
}

void ThrowNoSuchMethodError(InvokeType type, Class* c, const StringPiece& name,
                            const StringPiece& signature, const Method* referrer) {
  ClassHelper kh(c);
  std::ostringstream msg;
  msg << "No " << type << " method " << name << signature
      << " in class " << kh.GetDescriptor() << " or its superclasses";
  if (referrer != NULL) {
    kh.ChangeClass(referrer->GetDeclaringClass());
    std::string location(kh.GetLocation());
    if (!location.empty()) {
      msg << " (accessed from " << location << ")";
    }
  }
  Thread::Current()->ThrowNewException("Ljava/lang/NoSuchMethodError;", msg.str().c_str());
}

void ThrowNewIllegalAccessErrorField(Thread* self,
                                     Class* referrer,
                                     Field* accessed) {
  self->ThrowNewExceptionF("Ljava/lang/IllegalAccessError;",
                           "Field '%s' is inaccessible to class '%s'",
                           PrettyField(accessed, false).c_str(),
                           PrettyDescriptor(referrer).c_str());
}

void ThrowNewIllegalAccessErrorFinalField(Thread* self,
                                          const Method* referrer,
                                          Field* accessed) {
  self->ThrowNewExceptionF("Ljava/lang/IllegalAccessError;",
                           "Final field '%s' cannot be written to by method '%s'",
                           PrettyField(accessed, false).c_str(),
                           PrettyMethod(referrer).c_str());
}

void ThrowNewIllegalAccessErrorMethod(Thread* self,
                                      Class* referrer,
                                      Method* accessed) {
  self->ThrowNewExceptionF("Ljava/lang/IllegalAccessError;",
                           "Method '%s' is inaccessible to class '%s'",
                           PrettyMethod(accessed).c_str(),
                           PrettyDescriptor(referrer).c_str());
}

void ThrowNullPointerExceptionForFieldAccess(Thread* self,
                                                           Field* field,
                                                           bool is_read) {
  self->ThrowNewExceptionF("Ljava/lang/NullPointerException;",
                           "Attempt to %s field '%s' on a null object reference",
                           is_read ? "read from" : "write to",
                           PrettyField(field, true).c_str());
}

void ThrowNullPointerExceptionForMethodAccess(Thread* self,
                                              Method* caller,
                                              uint32_t method_idx,
                                              InvokeType type) {
  const DexFile& dex_file =
      Runtime::Current()->GetClassLinker()->FindDexFile(caller->GetDeclaringClass()->GetDexCache());
  self->ThrowNewExceptionF("Ljava/lang/NullPointerException;",
                           "Attempt to invoke %s method '%s' on a null object reference",
                           ToStr<InvokeType>(type).c_str(),
                           PrettyMethod(method_idx, dex_file, true).c_str());
}

void ThrowNullPointerExceptionFromDexPC(Thread* self, Method* throw_method, uint32_t dex_pc) {
  const DexFile::CodeItem* code = MethodHelper(throw_method).GetCodeItem();
  CHECK_LT(dex_pc, code->insns_size_in_code_units_);
  const Instruction* instr = Instruction::At(&code->insns_[dex_pc]);
  DecodedInstruction dec_insn(instr);
  switch (instr->Opcode()) {
    case Instruction::INVOKE_DIRECT:
    case Instruction::INVOKE_DIRECT_RANGE:
      ThrowNullPointerExceptionForMethodAccess(self, throw_method, dec_insn.vB, kDirect);
      break;
    case Instruction::INVOKE_VIRTUAL:
    case Instruction::INVOKE_VIRTUAL_RANGE:
      ThrowNullPointerExceptionForMethodAccess(self, throw_method, dec_insn.vB, kVirtual);
      break;
    case Instruction::IGET:
    case Instruction::IGET_WIDE:
    case Instruction::IGET_OBJECT:
    case Instruction::IGET_BOOLEAN:
    case Instruction::IGET_BYTE:
    case Instruction::IGET_CHAR:
    case Instruction::IGET_SHORT: {
      Field* field =
          Runtime::Current()->GetClassLinker()->ResolveField(dec_insn.vC, throw_method, false);
      ThrowNullPointerExceptionForFieldAccess(self, field, true /* read */);
      break;
    }
    case Instruction::IPUT:
    case Instruction::IPUT_WIDE:
    case Instruction::IPUT_OBJECT:
    case Instruction::IPUT_BOOLEAN:
    case Instruction::IPUT_BYTE:
    case Instruction::IPUT_CHAR:
    case Instruction::IPUT_SHORT: {
      Field* field =
          Runtime::Current()->GetClassLinker()->ResolveField(dec_insn.vC, throw_method, false);
      ThrowNullPointerExceptionForFieldAccess(self, field, false /* write */);
      break;
    }
    case Instruction::AGET:
    case Instruction::AGET_WIDE:
    case Instruction::AGET_OBJECT:
    case Instruction::AGET_BOOLEAN:
    case Instruction::AGET_BYTE:
    case Instruction::AGET_CHAR:
    case Instruction::AGET_SHORT:
      self->ThrowNewException("Ljava/lang/NullPointerException;",
                              "Attempt to read from null array");
      break;
    case Instruction::APUT:
    case Instruction::APUT_WIDE:
    case Instruction::APUT_OBJECT:
    case Instruction::APUT_BOOLEAN:
    case Instruction::APUT_BYTE:
    case Instruction::APUT_CHAR:
    case Instruction::APUT_SHORT:
      self->ThrowNewException("Ljava/lang/NullPointerException;",
                              "Attempt to write to null array");
      break;
    case Instruction::ARRAY_LENGTH:
      self->ThrowNewException("Ljava/lang/NullPointerException;",
                              "Attempt to get length of null array");
      break;
    default: {
      const DexFile& dex_file = Runtime::Current()->GetClassLinker()
          ->FindDexFile(throw_method->GetDeclaringClass()->GetDexCache());
      std::string message("Null pointer exception during instruction '");
      message += instr->DumpString(&dex_file);
      message += "'";
      self->ThrowNewException("Ljava/lang/NullPointerException;", message.c_str());
      break;
    }
  }
}

std::string FieldNameFromIndex(const Method* method, uint32_t ref,
                               verifier::VerifyErrorRefType ref_type, bool access) {
  CHECK_EQ(static_cast<int>(ref_type), static_cast<int>(verifier::VERIFY_ERROR_REF_FIELD));

  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  const DexFile& dex_file = class_linker->FindDexFile(method->GetDeclaringClass()->GetDexCache());

  const DexFile::FieldId& id = dex_file.GetFieldId(ref);
  std::string class_name(PrettyDescriptor(dex_file.GetFieldDeclaringClassDescriptor(id)));
  const char* field_name = dex_file.StringDataByIdx(id.name_idx_);
  if (!access) {
    return class_name + "." + field_name;
  }

  std::string result;
  result += "tried to access field ";
  result += class_name + "." + field_name;
  result += " from class ";
  result += PrettyDescriptor(method->GetDeclaringClass());
  return result;
}

std::string MethodNameFromIndex(const Method* method, uint32_t ref,
                                verifier::VerifyErrorRefType ref_type, bool access) {
  CHECK_EQ(static_cast<int>(ref_type), static_cast<int>(verifier::VERIFY_ERROR_REF_METHOD));

  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  const DexFile& dex_file = class_linker->FindDexFile(method->GetDeclaringClass()->GetDexCache());

  const DexFile::MethodId& id = dex_file.GetMethodId(ref);
  std::string class_name(PrettyDescriptor(dex_file.GetMethodDeclaringClassDescriptor(id)));
  const char* method_name = dex_file.StringDataByIdx(id.name_idx_);
  if (!access) {
    return class_name + "." + method_name;
  }

  std::string result;
  result += "tried to access method ";
  result += class_name + "." + method_name + ":" +
      dex_file.CreateMethodSignature(id.proto_idx_, NULL);
  result += " from class ";
  result += PrettyDescriptor(method->GetDeclaringClass());
  return result;
}

// Helper function to allocate array for FILLED_NEW_ARRAY.
Array* CheckAndAllocArrayFromCode(uint32_t type_idx, Method* method, int32_t component_count,
                                  Thread* self, bool access_check) {
  if (UNLIKELY(component_count < 0)) {
    self->ThrowNewExceptionF("Ljava/lang/NegativeArraySizeException;", "%d", component_count);
    return NULL;  // Failure
  }
  Class* klass = method->GetDexCacheResolvedTypes()->Get(type_idx);
  if (UNLIKELY(klass == NULL)) {  // Not in dex cache so try to resolve
    klass = Runtime::Current()->GetClassLinker()->ResolveType(type_idx, method);
    if (klass == NULL) {  // Error
      DCHECK(Thread::Current()->IsExceptionPending());
      return NULL;  // Failure
    }
  }
  if (UNLIKELY(klass->IsPrimitive() && !klass->IsPrimitiveInt())) {
    if (klass->IsPrimitiveLong() || klass->IsPrimitiveDouble()) {
      Thread::Current()->ThrowNewExceptionF("Ljava/lang/RuntimeException;",
                                            "Bad filled array request for type %s",
                                            PrettyDescriptor(klass).c_str());
    } else {
      Thread::Current()->ThrowNewExceptionF("Ljava/lang/InternalError;",
                                            "Found type %s; filled-new-array not implemented for anything but \'int\'",
                                            PrettyDescriptor(klass).c_str());
    }
    return NULL;  // Failure
  } else {
    if (access_check) {
      Class* referrer = method->GetDeclaringClass();
      if (UNLIKELY(!referrer->CanAccess(klass))) {
        ThrowNewIllegalAccessErrorClass(self, referrer, klass);
        return NULL;  // Failure
      }
    }
    DCHECK(klass->IsArrayClass()) << PrettyClass(klass);
    return Array::Alloc(klass, component_count);
  }
}

Field* FindFieldFromCode(uint32_t field_idx, const Method* referrer, Thread* self,
                         FindFieldType type, size_t expected_size) {
  bool is_primitive;
  bool is_set;
  bool is_static;
  switch (type) {
    case InstanceObjectRead:     is_primitive = false; is_set = false; is_static = false; break;
    case InstanceObjectWrite:    is_primitive = false; is_set = true;  is_static = false; break;
    case InstancePrimitiveRead:  is_primitive = true;  is_set = false; is_static = false; break;
    case InstancePrimitiveWrite: is_primitive = true;  is_set = true;  is_static = false; break;
    case StaticObjectRead:       is_primitive = false; is_set = false; is_static = true;  break;
    case StaticObjectWrite:      is_primitive = false; is_set = true;  is_static = true;  break;
    case StaticPrimitiveRead:    is_primitive = true;  is_set = false; is_static = true;  break;
    case StaticPrimitiveWrite:   // Keep GCC happy by having a default handler, fall-through.
    default:                     is_primitive = true;  is_set = true;  is_static = true;  break;
  }
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  Field* resolved_field = class_linker->ResolveField(field_idx, referrer, is_static);
  if (UNLIKELY(resolved_field == NULL)) {
    DCHECK(self->IsExceptionPending());  // Throw exception and unwind.
    return NULL;  // Failure.
  } else {
    if (resolved_field->IsStatic() != is_static) {
      ThrowNewIncompatibleClassChangeErrorField(self, resolved_field, is_static);
      return NULL;
    }
    Class* fields_class = resolved_field->GetDeclaringClass();
    Class* referring_class = referrer->GetDeclaringClass();
    if (UNLIKELY(!referring_class->CanAccess(fields_class) ||
                 !referring_class->CanAccessMember(fields_class,
                                                   resolved_field->GetAccessFlags()))) {
      // The referring class can't access the resolved field, this may occur as a result of a
      // protected field being made public by a sub-class. Resort to the dex file to determine
      // the correct class for the access check.
      const DexFile& dex_file = class_linker->FindDexFile(referring_class->GetDexCache());
      fields_class = class_linker->ResolveType(dex_file,
                                               dex_file.GetFieldId(field_idx).class_idx_,
                                               referring_class);
      if (UNLIKELY(!referring_class->CanAccess(fields_class))) {
        ThrowNewIllegalAccessErrorClass(self, referring_class, fields_class);
        return NULL;  // failure
      } else if (UNLIKELY(!referring_class->CanAccessMember(fields_class,
                                                            resolved_field->GetAccessFlags()))) {
        ThrowNewIllegalAccessErrorField(self, referring_class, resolved_field);
        return NULL;  // failure
      }
    }
    if (UNLIKELY(is_set && resolved_field->IsFinal() && (fields_class != referring_class))) {
      ThrowNewIllegalAccessErrorFinalField(self, referrer, resolved_field);
      return NULL;  // failure
    } else {
      FieldHelper fh(resolved_field);
      if (UNLIKELY(fh.IsPrimitiveType() != is_primitive ||
                   fh.FieldSize() != expected_size)) {
        self->ThrowNewExceptionF("Ljava/lang/NoSuchFieldError;",
                                 "Attempted read of %zd-bit %s on field '%s'",
                                 expected_size * (32 / sizeof(int32_t)),
                                 is_primitive ? "primitive" : "non-primitive",
                                 PrettyField(resolved_field, true).c_str());
        return NULL;  // failure
      } else if (!is_static) {
        // instance fields must be being accessed on an initialized class
        return resolved_field;
      } else {
        // If the class is already initializing, we must be inside <clinit>, or
        // we'd still be waiting for the lock.
        if (fields_class->IsInitializing()) {
          return resolved_field;
        } else if (Runtime::Current()->GetClassLinker()->EnsureInitialized(fields_class, true, true)) {
          return resolved_field;
        } else {
          DCHECK(self->IsExceptionPending());  // Throw exception and unwind
          return NULL;  // failure
        }
      }
    }
  }
}

// Slow path method resolution
Method* FindMethodFromCode(uint32_t method_idx, Object* this_object, const Method* referrer,
                           Thread* self, bool access_check, InvokeType type) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  bool is_direct = type == kStatic || type == kDirect;
  Method* resolved_method = class_linker->ResolveMethod(method_idx, referrer, type);
  if (UNLIKELY(resolved_method == NULL)) {
    DCHECK(self->IsExceptionPending());  // Throw exception and unwind.
    return NULL;  // Failure.
  } else {
    if (!access_check) {
      if (is_direct) {
        return resolved_method;
      } else if (type == kInterface) {
        Method* interface_method =
            this_object->GetClass()->FindVirtualMethodForInterface(resolved_method);
        if (UNLIKELY(interface_method == NULL)) {
          ThrowNewIncompatibleClassChangeErrorClassForInterfaceDispatch(self,
                                                                        resolved_method,
                                                                        this_object);
          return NULL;  // Failure.
        } else {
          return interface_method;
        }
      } else {
        ObjectArray<Method>* vtable;
        uint16_t vtable_index = resolved_method->GetMethodIndex();
        if (type == kSuper) {
          vtable = referrer->GetDeclaringClass()->GetSuperClass()->GetVTable();
        } else {
          vtable = this_object->GetClass()->GetVTable();
        }
        // TODO: eliminate bounds check?
        return vtable->Get(vtable_index);
      }
    } else {
      // Incompatible class change should have been handled in resolve method.
      if (UNLIKELY(resolved_method->CheckIncompatibleClassChange(type))) {
        ThrowIncompatibleClassChangeError(type, resolved_method->GetInvokeType(), resolved_method,
                                          referrer);
        return NULL;  // Failure.
      }
      Class* methods_class = resolved_method->GetDeclaringClass();
      Class* referring_class = referrer->GetDeclaringClass();
      if (UNLIKELY(!referring_class->CanAccess(methods_class) ||
                   !referring_class->CanAccessMember(methods_class,
                                                     resolved_method->GetAccessFlags()))) {
        // The referring class can't access the resolved method, this may occur as a result of a
        // protected method being made public by implementing an interface that re-declares the
        // method public. Resort to the dex file to determine the correct class for the access check
        const DexFile& dex_file = class_linker->FindDexFile(referring_class->GetDexCache());
        methods_class = class_linker->ResolveType(dex_file,
                                                  dex_file.GetMethodId(method_idx).class_idx_,
                                                  referring_class);
        if (UNLIKELY(!referring_class->CanAccess(methods_class))) {
          ThrowNewIllegalAccessErrorClassForMethodDispatch(self, referring_class, methods_class,
                                                           referrer, resolved_method, type);
          return NULL;  // Failure.
        } else if (UNLIKELY(!referring_class->CanAccessMember(methods_class,
                                                              resolved_method->GetAccessFlags()))) {
          ThrowNewIllegalAccessErrorMethod(self, referring_class, resolved_method);
          return NULL;  // Failure.
        }
      }
      if (is_direct) {
        return resolved_method;
      } else if (type == kInterface) {
        Method* interface_method =
            this_object->GetClass()->FindVirtualMethodForInterface(resolved_method);
        if (UNLIKELY(interface_method == NULL)) {
          ThrowNewIncompatibleClassChangeErrorClassForInterfaceDispatch(self,
                                                                        resolved_method,
                                                                        this_object);
          return NULL;  // Failure.
        } else {
          return interface_method;
        }
      } else {
        ObjectArray<Method>* vtable;
        uint16_t vtable_index = resolved_method->GetMethodIndex();
        if (type == kSuper) {
          Class* super_class = referring_class->GetSuperClass();
          if (LIKELY(super_class != NULL)) {
            vtable = referring_class->GetSuperClass()->GetVTable();
          } else {
            vtable = NULL;
          }
        } else {
          vtable = this_object->GetClass()->GetVTable();
        }
        if (LIKELY(vtable != NULL &&
                   vtable_index < static_cast<uint32_t>(vtable->GetLength()))) {
          return vtable->GetWithoutChecks(vtable_index);
        } else {
          // Behavior to agree with that of the verifier.
          MethodHelper mh(resolved_method);
          ThrowNoSuchMethodError(type, resolved_method->GetDeclaringClass(), mh.GetName(),
                                 mh.GetSignature(), referrer);
          return NULL;  // Failure.
        }
      }
    }
  }
}

Class* ResolveVerifyAndClinit(uint32_t type_idx, const Method* referrer, Thread* self,
                               bool can_run_clinit, bool verify_access) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  Class* klass = class_linker->ResolveType(type_idx, referrer);
  if (UNLIKELY(klass == NULL)) {
    CHECK(self->IsExceptionPending());
    return NULL;  // Failure - Indicate to caller to deliver exception
  }
  // Perform access check if necessary.
  Class* referring_class = referrer->GetDeclaringClass();
  if (verify_access && UNLIKELY(!referring_class->CanAccess(klass))) {
    ThrowNewIllegalAccessErrorClass(self, referring_class, klass);
    return NULL;  // Failure - Indicate to caller to deliver exception
  }
  // If we're just implementing const-class, we shouldn't call <clinit>.
  if (!can_run_clinit) {
    return klass;
  }
  // If we are the <clinit> of this class, just return our storage.
  //
  // Do not set the DexCache InitializedStaticStorage, since that implies <clinit> has finished
  // running.
  if (klass == referring_class && MethodHelper(referrer).IsClassInitializer()) {
    return klass;
  }
  if (!class_linker->EnsureInitialized(klass, true, true)) {
    CHECK(self->IsExceptionPending());
    return NULL;  // Failure - Indicate to caller to deliver exception
  }
  referrer->GetDexCacheInitializedStaticStorage()->Set(type_idx, klass);
  return klass;
}

}  // namespace art
