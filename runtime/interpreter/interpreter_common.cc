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
#include "mirror/array-inl.h"

namespace art {
namespace interpreter {

static void UnstartedRuntimeInvoke(Thread* self, MethodHelper& mh,
                                   const DexFile::CodeItem* code_item, ShadowFrame* shadow_frame,
                                   JValue* result, size_t arg_offset)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_);

// Assign register 'src_reg' from shadow_frame to register 'dest_reg' into new_shadow_frame.
static inline void AssignRegister(ShadowFrame* new_shadow_frame, const ShadowFrame& shadow_frame,
                                  size_t dest_reg, size_t src_reg)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  // If both register locations contains the same value, the register probably holds a reference.
  // Uint required, so that sign extension does not make this wrong on 64b systems
  uint32_t src_value = shadow_frame.GetVReg(src_reg);
  mirror::Object* o = shadow_frame.GetVRegReference<kVerifyNone>(src_reg);
  if (src_value == reinterpret_cast<uintptr_t>(o)) {
    new_shadow_frame->SetVRegReference(dest_reg, o);
  } else {
    new_shadow_frame->SetVReg(dest_reg, src_value);
  }
}

void AbortTransaction(Thread* self, const char* fmt, ...) {
  CHECK(Runtime::Current()->IsActiveTransaction());
  // Throw an exception so we can abort the transaction and undo every change.
  va_list args;
  va_start(args, fmt);
  self->ThrowNewExceptionV(self->GetCurrentLocationForThrow(), "Ljava/lang/InternalError;", fmt,
                           args);
  va_end(args);
}

template<bool is_range, bool do_assignability_check>
bool DoCall(ArtMethod* method, Thread* self, ShadowFrame& shadow_frame,
            const Instruction* inst, uint16_t inst_data, JValue* result) {
  // Compute method information.
  MethodHelper mh(method);
  const DexFile::CodeItem* code_item = mh.GetCodeItem();
  const uint16_t num_ins = (is_range) ? inst->VRegA_3rc(inst_data) : inst->VRegA_35c(inst_data);
  uint16_t num_regs;
  if (LIKELY(code_item != NULL)) {
    num_regs = code_item->registers_size_;
    DCHECK_EQ(num_ins, code_item->ins_size_);
  } else {
    DCHECK(method->IsNative() || method->IsProxyMethod());
    num_regs = num_ins;
  }

  // Allocate shadow frame on the stack.
  const char* old_cause = self->StartAssertNoThreadSuspension("DoCall");
  void* memory = alloca(ShadowFrame::ComputeSize(num_regs));
  ShadowFrame* new_shadow_frame(ShadowFrame::Create(num_regs, &shadow_frame, method, 0, memory));

  // Initialize new shadow frame.
  const size_t first_dest_reg = num_regs - num_ins;
  if (do_assignability_check) {
    // Slow path: we need to do runtime check on reference assignment. We need to load the shorty
    // to get the exact type of each reference argument.
    const DexFile::TypeList* params = mh.GetParameterTypeList();
    const char* shorty = mh.GetShorty();

    // TODO: find a cleaner way to separate non-range and range information without duplicating code.
    uint32_t arg[5];  // only used in invoke-XXX.
    uint32_t vregC;   // only used in invoke-XXX-range.
    if (is_range) {
      vregC = inst->VRegC_3rc();
    } else {
      inst->GetVarArgs(arg, inst_data);
    }

    // Handle receiver apart since it's not part of the shorty.
    size_t dest_reg = first_dest_reg;
    size_t arg_offset = 0;
    if (!method->IsStatic()) {
      size_t receiver_reg = (is_range) ? vregC : arg[0];
      new_shadow_frame->SetVRegReference(dest_reg, shadow_frame.GetVRegReference(receiver_reg));
      ++dest_reg;
      ++arg_offset;
    }
    for (uint32_t shorty_pos = 0; dest_reg < num_regs; ++shorty_pos, ++dest_reg, ++arg_offset) {
      DCHECK_LT(shorty_pos + 1, mh.GetShortyLength());
      const size_t src_reg = (is_range) ? vregC + arg_offset : arg[arg_offset];
      switch (shorty[shorty_pos + 1]) {
        case 'L': {
          Object* o = shadow_frame.GetVRegReference(src_reg);
          if (do_assignability_check && o != NULL) {
            Class* arg_type = mh.GetClassFromTypeIdx(params->GetTypeItem(shorty_pos).type_idx_);
            if (arg_type == NULL) {
              CHECK(self->IsExceptionPending());
              self->EndAssertNoThreadSuspension(old_cause);
              return false;
            }
            if (!o->VerifierInstanceOf(arg_type)) {
              self->EndAssertNoThreadSuspension(old_cause);
              // This should never happen.
              self->ThrowNewExceptionF(self->GetCurrentLocationForThrow(),
                                       "Ljava/lang/VirtualMachineError;",
                                       "Invoking %s with bad arg %d, type '%s' not instance of '%s'",
                                       mh.GetName(), shorty_pos,
                                       ClassHelper(o->GetClass()).GetDescriptor(),
                                       ClassHelper(arg_type).GetDescriptor());
              return false;
            }
          }
          new_shadow_frame->SetVRegReference(dest_reg, o);
          break;
        }
        case 'J': case 'D': {
          uint64_t wide_value = (static_cast<uint64_t>(shadow_frame.GetVReg(src_reg + 1)) << 32) |
                                static_cast<uint32_t>(shadow_frame.GetVReg(src_reg));
          new_shadow_frame->SetVRegLong(dest_reg, wide_value);
          ++dest_reg;
          ++arg_offset;
          break;
        }
        default:
          new_shadow_frame->SetVReg(dest_reg, shadow_frame.GetVReg(src_reg));
          break;
      }
    }
  } else {
    // Fast path: no extra checks.
    if (is_range) {
      const uint16_t first_src_reg = inst->VRegC_3rc();
      for (size_t src_reg = first_src_reg, dest_reg = first_dest_reg; dest_reg < num_regs;
          ++dest_reg, ++src_reg) {
        AssignRegister(new_shadow_frame, shadow_frame, dest_reg, src_reg);
      }
    } else {
      DCHECK_LE(num_ins, 5U);
      uint16_t regList = inst->Fetch16(2);
      uint16_t count = num_ins;
      if (count == 5) {
        AssignRegister(new_shadow_frame, shadow_frame, first_dest_reg + 4U, (inst_data >> 8) & 0x0f);
        --count;
       }
      for (size_t arg_index = 0; arg_index < count; ++arg_index, regList >>= 4) {
        AssignRegister(new_shadow_frame, shadow_frame, first_dest_reg + arg_index, regList & 0x0f);
      }
    }
  }
  self->EndAssertNoThreadSuspension(old_cause);

  // Do the call now.
  if (LIKELY(Runtime::Current()->IsStarted())) {
    if (kIsDebugBuild && method->GetEntryPointFromInterpreter() == nullptr) {
      LOG(FATAL) << "Attempt to invoke non-executable method: " << PrettyMethod(method);
    }
    if (kIsDebugBuild && Runtime::Current()->GetInstrumentation()->IsForcedInterpretOnly() &&
        !method->IsNative() && !method->IsProxyMethod() &&
        method->GetEntryPointFromInterpreter() == artInterpreterToCompiledCodeBridge) {
      LOG(FATAL) << "Attempt to call compiled code when -Xint: " << PrettyMethod(method);
    }
    (method->GetEntryPointFromInterpreter())(self, mh, code_item, new_shadow_frame, result);
  } else {
    UnstartedRuntimeInvoke(self, mh, code_item, new_shadow_frame, result, first_dest_reg);
  }
  return !self->IsExceptionPending();
}

template <bool is_range, bool do_access_check, bool transaction_active>
bool DoFilledNewArray(const Instruction* inst, const ShadowFrame& shadow_frame,
                      Thread* self, JValue* result) {
  DCHECK(inst->Opcode() == Instruction::FILLED_NEW_ARRAY ||
         inst->Opcode() == Instruction::FILLED_NEW_ARRAY_RANGE);
  const int32_t length = is_range ? inst->VRegA_3rc() : inst->VRegA_35c();
  if (!is_range) {
    // Checks FILLED_NEW_ARRAY's length does not exceed 5 arguments.
    CHECK_LE(length, 5);
  }
  if (UNLIKELY(length < 0)) {
    ThrowNegativeArraySizeException(length);
    return false;
  }
  uint16_t type_idx = is_range ? inst->VRegB_3rc() : inst->VRegB_35c();
  Class* arrayClass = ResolveVerifyAndClinit(type_idx, shadow_frame.GetMethod(),
                                             self, false, do_access_check);
  if (UNLIKELY(arrayClass == NULL)) {
    DCHECK(self->IsExceptionPending());
    return false;
  }
  CHECK(arrayClass->IsArrayClass());
  Class* componentClass = arrayClass->GetComponentType();
  if (UNLIKELY(componentClass->IsPrimitive() && !componentClass->IsPrimitiveInt())) {
    if (componentClass->IsPrimitiveLong() || componentClass->IsPrimitiveDouble()) {
      ThrowRuntimeException("Bad filled array request for type %s",
                            PrettyDescriptor(componentClass).c_str());
    } else {
      self->ThrowNewExceptionF(shadow_frame.GetCurrentLocationForThrow(),
                               "Ljava/lang/InternalError;",
                               "Found type %s; filled-new-array not implemented for anything but 'int'",
                               PrettyDescriptor(componentClass).c_str());
    }
    return false;
  }
  Object* newArray = Array::Alloc<true>(self, arrayClass, length, arrayClass->GetComponentSize(),
                                        Runtime::Current()->GetHeap()->GetCurrentAllocator());
  if (UNLIKELY(newArray == NULL)) {
    DCHECK(self->IsExceptionPending());
    return false;
  }
  uint32_t arg[5];  // only used in filled-new-array.
  uint32_t vregC;   // only used in filled-new-array-range.
  if (is_range) {
    vregC = inst->VRegC_3rc();
  } else {
    inst->GetVarArgs(arg);
  }
  const bool is_primitive_int_component = componentClass->IsPrimitiveInt();
  for (int32_t i = 0; i < length; ++i) {
    size_t src_reg = is_range ? vregC + i : arg[i];
    if (is_primitive_int_component) {
      newArray->AsIntArray()->SetWithoutChecks<transaction_active>(i, shadow_frame.GetVReg(src_reg));
    } else {
      newArray->AsObjectArray<Object>()->SetWithoutChecks<transaction_active>(i, shadow_frame.GetVRegReference(src_reg));
    }
  }

  result->SetL(newArray);
  return true;
}

// TODO fix thread analysis: should be SHARED_LOCKS_REQUIRED(Locks::mutator_lock_).
template<typename T>
static void RecordArrayElementsInTransactionImpl(mirror::PrimitiveArray<T>* array, int32_t count)
    NO_THREAD_SAFETY_ANALYSIS {
  Runtime* runtime = Runtime::Current();
  for (int32_t i = 0; i < count; ++i) {
    runtime->RecordWriteArray(array, i, array->GetWithoutChecks(i));
  }
}

void RecordArrayElementsInTransaction(mirror::Array* array, int32_t count)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  DCHECK(Runtime::Current()->IsActiveTransaction());
  DCHECK(array != nullptr);
  DCHECK_LE(count, array->GetLength());
  Primitive::Type primitive_component_type = array->GetClass()->GetComponentType()->GetPrimitiveType();
  switch (primitive_component_type) {
    case Primitive::kPrimBoolean:
      RecordArrayElementsInTransactionImpl(array->AsBooleanArray(), count);
      break;
    case Primitive::kPrimByte:
      RecordArrayElementsInTransactionImpl(array->AsByteArray(), count);
      break;
    case Primitive::kPrimChar:
      RecordArrayElementsInTransactionImpl(array->AsCharArray(), count);
      break;
    case Primitive::kPrimShort:
      RecordArrayElementsInTransactionImpl(array->AsShortArray(), count);
      break;
    case Primitive::kPrimInt:
    case Primitive::kPrimFloat:
      RecordArrayElementsInTransactionImpl(array->AsIntArray(), count);
      break;
    case Primitive::kPrimLong:
    case Primitive::kPrimDouble:
      RecordArrayElementsInTransactionImpl(array->AsLongArray(), count);
      break;
    default:
      LOG(FATAL) << "Unsupported primitive type " << primitive_component_type
                 << " in fill-array-data";
      break;
  }
}

static void UnstartedRuntimeInvoke(Thread* self, MethodHelper& mh,
                                   const DexFile::CodeItem* code_item, ShadowFrame* shadow_frame,
                                   JValue* result, size_t arg_offset) {
  // In a runtime that's not started we intercept certain methods to avoid complicated dependency
  // problems in core libraries.
  std::string name(PrettyMethod(shadow_frame->GetMethod()));
  if (name == "java.lang.Class java.lang.Class.forName(java.lang.String)"
      || name == "java.lang.Class java.lang.VMClassLoader.loadClass(java.lang.String, boolean)") {
    // TODO Class#forName should actually call Class::EnsureInitialized always. Support for the
    // other variants that take more arguments should also be added.
    std::string descriptor(DotToDescriptor(shadow_frame->GetVRegReference(arg_offset)->AsString()->ToModifiedUtf8().c_str()));

    StackHandleScope<1> hs(self);
    // shadow_frame.GetMethod()->GetDeclaringClass()->GetClassLoader();
    auto class_loader = hs.NewHandle<ClassLoader>(nullptr);
    Class* found = Runtime::Current()->GetClassLinker()->FindClass(self, descriptor.c_str(),
                                                                   class_loader);
    CHECK(found != NULL) << "Class.forName failed in un-started runtime for class: "
        << PrettyDescriptor(descriptor);
    result->SetL(found);
  } else if (name == "java.lang.Class java.lang.Void.lookupType()") {
    result->SetL(Runtime::Current()->GetClassLinker()->FindPrimitiveClass('V'));
  } else if (name == "java.lang.Class java.lang.VMClassLoader.findLoadedClass(java.lang.ClassLoader, java.lang.String)") {
    StackHandleScope<1> hs(self);
    Handle<ClassLoader> class_loader(
        hs.NewHandle(down_cast<mirror::ClassLoader*>(shadow_frame->GetVRegReference(arg_offset))));
    std::string descriptor(DotToDescriptor(shadow_frame->GetVRegReference(arg_offset + 1)->AsString()->ToModifiedUtf8().c_str()));

    Class* found = Runtime::Current()->GetClassLinker()->FindClass(self, descriptor.c_str(),
                                                                   class_loader);
    result->SetL(found);
  } else if (name == "java.lang.Object java.lang.Class.newInstance()") {
    Class* klass = shadow_frame->GetVRegReference(arg_offset)->AsClass();
    ArtMethod* c = klass->FindDeclaredDirectMethod("<init>", "()V");
    CHECK(c != NULL);
    StackHandleScope<1> hs(self);
    Handle<Object> obj(hs.NewHandle(klass->AllocObject(self)));
    CHECK(obj.Get() != NULL);
    EnterInterpreterFromInvoke(self, c, obj.Get(), NULL, NULL);
    result->SetL(obj.Get());
  } else if (name == "java.lang.reflect.Field java.lang.Class.getDeclaredField(java.lang.String)") {
    // Special managed code cut-out to allow field lookup in a un-started runtime that'd fail
    // going the reflective Dex way.
    Class* klass = shadow_frame->GetVRegReference(arg_offset)->AsClass();
    String* name = shadow_frame->GetVRegReference(arg_offset + 1)->AsString();
    ArtField* found = NULL;
    FieldHelper fh;
    ObjectArray<ArtField>* fields = klass->GetIFields();
    for (int32_t i = 0; i < fields->GetLength() && found == NULL; ++i) {
      ArtField* f = fields->Get(i);
      fh.ChangeField(f);
      if (name->Equals(fh.GetName())) {
        found = f;
      }
    }
    if (found == NULL) {
      fields = klass->GetSFields();
      for (int32_t i = 0; i < fields->GetLength() && found == NULL; ++i) {
        ArtField* f = fields->Get(i);
        fh.ChangeField(f);
        if (name->Equals(fh.GetName())) {
          found = f;
        }
      }
    }
    CHECK(found != NULL)
      << "Failed to find field in Class.getDeclaredField in un-started runtime. name="
      << name->ToModifiedUtf8() << " class=" << PrettyDescriptor(klass);
    // TODO: getDeclaredField calls GetType once the field is found to ensure a
    //       NoClassDefFoundError is thrown if the field's type cannot be resolved.
    Class* jlr_Field = self->DecodeJObject(WellKnownClasses::java_lang_reflect_Field)->AsClass();
    StackHandleScope<1> hs(self);
    Handle<Object> field(hs.NewHandle(jlr_Field->AllocNonMovableObject(self)));
    CHECK(field.Get() != NULL);
    ArtMethod* c = jlr_Field->FindDeclaredDirectMethod("<init>", "(Ljava/lang/reflect/ArtField;)V");
    uint32_t args[1];
    args[0] = StackReference<mirror::Object>::FromMirrorPtr(found).AsVRegValue();
    EnterInterpreterFromInvoke(self, c, field.Get(), args, NULL);
    result->SetL(field.Get());
  } else if (name == "int java.lang.Object.hashCode()") {
    Object* obj = shadow_frame->GetVRegReference(arg_offset);
    result->SetI(obj->IdentityHashCode());
  } else if (name == "java.lang.String java.lang.reflect.ArtMethod.getMethodName(java.lang.reflect.ArtMethod)") {
    ArtMethod* method = shadow_frame->GetVRegReference(arg_offset)->AsArtMethod();
    result->SetL(MethodHelper(method).GetNameAsString());
  } else if (name == "void java.lang.System.arraycopy(java.lang.Object, int, java.lang.Object, int, int)" ||
             name == "void java.lang.System.arraycopy(char[], int, char[], int, int)") {
    // Special case array copying without initializing System.
    Class* ctype = shadow_frame->GetVRegReference(arg_offset)->GetClass()->GetComponentType();
    jint srcPos = shadow_frame->GetVReg(arg_offset + 1);
    jint dstPos = shadow_frame->GetVReg(arg_offset + 3);
    jint length = shadow_frame->GetVReg(arg_offset + 4);
    if (!ctype->IsPrimitive()) {
      ObjectArray<Object>* src = shadow_frame->GetVRegReference(arg_offset)->AsObjectArray<Object>();
      ObjectArray<Object>* dst = shadow_frame->GetVRegReference(arg_offset + 2)->AsObjectArray<Object>();
      for (jint i = 0; i < length; ++i) {
        dst->Set(dstPos + i, src->Get(srcPos + i));
      }
    } else if (ctype->IsPrimitiveChar()) {
      CharArray* src = shadow_frame->GetVRegReference(arg_offset)->AsCharArray();
      CharArray* dst = shadow_frame->GetVRegReference(arg_offset + 2)->AsCharArray();
      for (jint i = 0; i < length; ++i) {
        dst->Set(dstPos + i, src->Get(srcPos + i));
      }
    } else if (ctype->IsPrimitiveInt()) {
      IntArray* src = shadow_frame->GetVRegReference(arg_offset)->AsIntArray();
      IntArray* dst = shadow_frame->GetVRegReference(arg_offset + 2)->AsIntArray();
      for (jint i = 0; i < length; ++i) {
        dst->Set(dstPos + i, src->Get(srcPos + i));
      }
    } else {
      self->ThrowNewExceptionF(self->GetCurrentLocationForThrow(), "Ljava/lang/InternalError;",
                               "Unimplemented System.arraycopy for type '%s'",
                               PrettyDescriptor(ctype).c_str());
    }
  } else  if (name == "java.lang.Object java.lang.ThreadLocal.get()") {
    std::string caller(PrettyMethod(shadow_frame->GetLink()->GetMethod()));
    if (caller == "java.lang.String java.lang.IntegralToString.convertInt(java.lang.AbstractStringBuilder, int)") {
      // Allocate non-threadlocal buffer.
      result->SetL(mirror::CharArray::Alloc(self, 11));
    } else {
      self->ThrowNewException(self->GetCurrentLocationForThrow(), "Ljava/lang/InternalError;",
                              "Unimplemented ThreadLocal.get");
    }
  } else {
    // Not special, continue with regular interpreter execution.
    artInterpreterToInterpreterBridge(self, mh, code_item, shadow_frame, result);
  }
}

// Explicit DoCall template function declarations.
#define EXPLICIT_DO_CALL_TEMPLATE_DECL(_is_range, _do_assignability_check)                      \
  template SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)                                          \
  bool DoCall<_is_range, _do_assignability_check>(ArtMethod* method, Thread* self,              \
                                                  ShadowFrame& shadow_frame,                    \
                                                  const Instruction* inst, uint16_t inst_data,  \
                                                  JValue* result)
EXPLICIT_DO_CALL_TEMPLATE_DECL(false, false);
EXPLICIT_DO_CALL_TEMPLATE_DECL(false, true);
EXPLICIT_DO_CALL_TEMPLATE_DECL(true, false);
EXPLICIT_DO_CALL_TEMPLATE_DECL(true, true);
#undef EXPLICIT_DO_CALL_TEMPLATE_DECL

// Explicit DoFilledNewArray template function declarations.
#define EXPLICIT_DO_FILLED_NEW_ARRAY_TEMPLATE_DECL(_is_range_, _check, _transaction_active)       \
  template SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)                                            \
  bool DoFilledNewArray<_is_range_, _check, _transaction_active>(const Instruction* inst,         \
                                                                 const ShadowFrame& shadow_frame, \
                                                                 Thread* self, JValue* result)
#define EXPLICIT_DO_FILLED_NEW_ARRAY_ALL_TEMPLATE_DECL(_transaction_active)       \
  EXPLICIT_DO_FILLED_NEW_ARRAY_TEMPLATE_DECL(false, false, _transaction_active);  \
  EXPLICIT_DO_FILLED_NEW_ARRAY_TEMPLATE_DECL(false, true, _transaction_active);   \
  EXPLICIT_DO_FILLED_NEW_ARRAY_TEMPLATE_DECL(true, false, _transaction_active);   \
  EXPLICIT_DO_FILLED_NEW_ARRAY_TEMPLATE_DECL(true, true, _transaction_active)
EXPLICIT_DO_FILLED_NEW_ARRAY_ALL_TEMPLATE_DECL(false);
EXPLICIT_DO_FILLED_NEW_ARRAY_ALL_TEMPLATE_DECL(true);
#undef EXPLICIT_DO_FILLED_NEW_ARRAY_ALL_TEMPLATE_DECL
#undef EXPLICIT_DO_FILLED_NEW_ARRAY_TEMPLATE_DECL

}  // namespace interpreter
}  // namespace art
