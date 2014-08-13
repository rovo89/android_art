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

#include "field_helper.h"
#include "mirror/array-inl.h"

namespace art {
namespace interpreter {

void ThrowNullPointerExceptionFromInterpreter(const ShadowFrame& shadow_frame) {
  ThrowNullPointerExceptionFromDexPC(shadow_frame.GetCurrentLocationForThrow());
}

template<FindFieldType find_type, Primitive::Type field_type, bool do_access_check>
bool DoFieldGet(Thread* self, ShadowFrame& shadow_frame, const Instruction* inst,
                uint16_t inst_data) {
  const bool is_static = (find_type == StaticObjectRead) || (find_type == StaticPrimitiveRead);
  const uint32_t field_idx = is_static ? inst->VRegB_21c() : inst->VRegC_22c();
  ArtField* f = FindFieldFromCode<find_type, do_access_check>(field_idx, shadow_frame.GetMethod(), self,
                                                              Primitive::FieldSize(field_type));
  if (UNLIKELY(f == nullptr)) {
    CHECK(self->IsExceptionPending());
    return false;
  }
  Object* obj;
  if (is_static) {
    obj = f->GetDeclaringClass();
  } else {
    obj = shadow_frame.GetVRegReference(inst->VRegB_22c(inst_data));
    if (UNLIKELY(obj == nullptr)) {
      ThrowNullPointerExceptionForFieldAccess(shadow_frame.GetCurrentLocationForThrow(), f, true);
      return false;
    }
  }
  f->GetDeclaringClass()->AssertInitializedOrInitializingInThread(self);
  // Report this field access to instrumentation if needed.
  instrumentation::Instrumentation* instrumentation = Runtime::Current()->GetInstrumentation();
  if (UNLIKELY(instrumentation->HasFieldReadListeners())) {
    Object* this_object = f->IsStatic() ? nullptr : obj;
    instrumentation->FieldReadEvent(self, this_object, shadow_frame.GetMethod(),
                                    shadow_frame.GetDexPC(), f);
  }
  uint32_t vregA = is_static ? inst->VRegA_21c(inst_data) : inst->VRegA_22c(inst_data);
  switch (field_type) {
    case Primitive::kPrimBoolean:
      shadow_frame.SetVReg(vregA, f->GetBoolean(obj));
      break;
    case Primitive::kPrimByte:
      shadow_frame.SetVReg(vregA, f->GetByte(obj));
      break;
    case Primitive::kPrimChar:
      shadow_frame.SetVReg(vregA, f->GetChar(obj));
      break;
    case Primitive::kPrimShort:
      shadow_frame.SetVReg(vregA, f->GetShort(obj));
      break;
    case Primitive::kPrimInt:
      shadow_frame.SetVReg(vregA, f->GetInt(obj));
      break;
    case Primitive::kPrimLong:
      shadow_frame.SetVRegLong(vregA, f->GetLong(obj));
      break;
    case Primitive::kPrimNot:
      shadow_frame.SetVRegReference(vregA, f->GetObject(obj));
      break;
    default:
      LOG(FATAL) << "Unreachable: " << field_type;
  }
  return true;
}

// Explicitly instantiate all DoFieldGet functions.
#define EXPLICIT_DO_FIELD_GET_TEMPLATE_DECL(_find_type, _field_type, _do_check) \
  template bool DoFieldGet<_find_type, _field_type, _do_check>(Thread* self, \
                                                               ShadowFrame& shadow_frame, \
                                                               const Instruction* inst, \
                                                               uint16_t inst_data)

#define EXPLICIT_DO_FIELD_GET_ALL_TEMPLATE_DECL(_find_type, _field_type)  \
    EXPLICIT_DO_FIELD_GET_TEMPLATE_DECL(_find_type, _field_type, false);  \
    EXPLICIT_DO_FIELD_GET_TEMPLATE_DECL(_find_type, _field_type, true);

// iget-XXX
EXPLICIT_DO_FIELD_GET_ALL_TEMPLATE_DECL(InstancePrimitiveRead, Primitive::kPrimBoolean);
EXPLICIT_DO_FIELD_GET_ALL_TEMPLATE_DECL(InstancePrimitiveRead, Primitive::kPrimByte);
EXPLICIT_DO_FIELD_GET_ALL_TEMPLATE_DECL(InstancePrimitiveRead, Primitive::kPrimChar);
EXPLICIT_DO_FIELD_GET_ALL_TEMPLATE_DECL(InstancePrimitiveRead, Primitive::kPrimShort);
EXPLICIT_DO_FIELD_GET_ALL_TEMPLATE_DECL(InstancePrimitiveRead, Primitive::kPrimInt);
EXPLICIT_DO_FIELD_GET_ALL_TEMPLATE_DECL(InstancePrimitiveRead, Primitive::kPrimLong);
EXPLICIT_DO_FIELD_GET_ALL_TEMPLATE_DECL(InstanceObjectRead, Primitive::kPrimNot);

// sget-XXX
EXPLICIT_DO_FIELD_GET_ALL_TEMPLATE_DECL(StaticPrimitiveRead, Primitive::kPrimBoolean);
EXPLICIT_DO_FIELD_GET_ALL_TEMPLATE_DECL(StaticPrimitiveRead, Primitive::kPrimByte);
EXPLICIT_DO_FIELD_GET_ALL_TEMPLATE_DECL(StaticPrimitiveRead, Primitive::kPrimChar);
EXPLICIT_DO_FIELD_GET_ALL_TEMPLATE_DECL(StaticPrimitiveRead, Primitive::kPrimShort);
EXPLICIT_DO_FIELD_GET_ALL_TEMPLATE_DECL(StaticPrimitiveRead, Primitive::kPrimInt);
EXPLICIT_DO_FIELD_GET_ALL_TEMPLATE_DECL(StaticPrimitiveRead, Primitive::kPrimLong);
EXPLICIT_DO_FIELD_GET_ALL_TEMPLATE_DECL(StaticObjectRead, Primitive::kPrimNot);

#undef EXPLICIT_DO_FIELD_GET_ALL_TEMPLATE_DECL
#undef EXPLICIT_DO_FIELD_GET_TEMPLATE_DECL

// Handles iget-quick, iget-wide-quick and iget-object-quick instructions.
// Returns true on success, otherwise throws an exception and returns false.
template<Primitive::Type field_type>
bool DoIGetQuick(ShadowFrame& shadow_frame, const Instruction* inst, uint16_t inst_data) {
  Object* obj = shadow_frame.GetVRegReference(inst->VRegB_22c(inst_data));
  if (UNLIKELY(obj == nullptr)) {
    // We lost the reference to the field index so we cannot get a more
    // precised exception message.
    ThrowNullPointerExceptionFromDexPC(shadow_frame.GetCurrentLocationForThrow());
    return false;
  }
  MemberOffset field_offset(inst->VRegC_22c());
  // Report this field access to instrumentation if needed. Since we only have the offset of
  // the field from the base of the object, we need to look for it first.
  instrumentation::Instrumentation* instrumentation = Runtime::Current()->GetInstrumentation();
  if (UNLIKELY(instrumentation->HasFieldReadListeners())) {
    ArtField* f = ArtField::FindInstanceFieldWithOffset(obj->GetClass(),
                                                        field_offset.Uint32Value());
    DCHECK(f != nullptr);
    DCHECK(!f->IsStatic());
    instrumentation->FieldReadEvent(Thread::Current(), obj, shadow_frame.GetMethod(),
                                    shadow_frame.GetDexPC(), f);
  }
  // Note: iget-x-quick instructions are only for non-volatile fields.
  const uint32_t vregA = inst->VRegA_22c(inst_data);
  switch (field_type) {
    case Primitive::kPrimInt:
      shadow_frame.SetVReg(vregA, static_cast<int32_t>(obj->GetField32(field_offset)));
      break;
    case Primitive::kPrimLong:
      shadow_frame.SetVRegLong(vregA, static_cast<int64_t>(obj->GetField64(field_offset)));
      break;
    case Primitive::kPrimNot:
      shadow_frame.SetVRegReference(vregA, obj->GetFieldObject<mirror::Object>(field_offset));
      break;
    default:
      LOG(FATAL) << "Unreachable: " << field_type;
  }
  return true;
}

// Explicitly instantiate all DoIGetQuick functions.
#define EXPLICIT_DO_IGET_QUICK_TEMPLATE_DECL(_field_type) \
  template bool DoIGetQuick<_field_type>(ShadowFrame& shadow_frame, const Instruction* inst, \
                                         uint16_t inst_data)

EXPLICIT_DO_IGET_QUICK_TEMPLATE_DECL(Primitive::kPrimInt);    // iget-quick.
EXPLICIT_DO_IGET_QUICK_TEMPLATE_DECL(Primitive::kPrimLong);   // iget-wide-quick.
EXPLICIT_DO_IGET_QUICK_TEMPLATE_DECL(Primitive::kPrimNot);    // iget-object-quick.
#undef EXPLICIT_DO_IGET_QUICK_TEMPLATE_DECL

template<Primitive::Type field_type>
static JValue GetFieldValue(const ShadowFrame& shadow_frame, uint32_t vreg)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  JValue field_value;
  switch (field_type) {
    case Primitive::kPrimBoolean:
      field_value.SetZ(static_cast<uint8_t>(shadow_frame.GetVReg(vreg)));
      break;
    case Primitive::kPrimByte:
      field_value.SetB(static_cast<int8_t>(shadow_frame.GetVReg(vreg)));
      break;
    case Primitive::kPrimChar:
      field_value.SetC(static_cast<uint16_t>(shadow_frame.GetVReg(vreg)));
      break;
    case Primitive::kPrimShort:
      field_value.SetS(static_cast<int16_t>(shadow_frame.GetVReg(vreg)));
      break;
    case Primitive::kPrimInt:
      field_value.SetI(shadow_frame.GetVReg(vreg));
      break;
    case Primitive::kPrimLong:
      field_value.SetJ(shadow_frame.GetVRegLong(vreg));
      break;
    case Primitive::kPrimNot:
      field_value.SetL(shadow_frame.GetVRegReference(vreg));
      break;
    default:
      LOG(FATAL) << "Unreachable: " << field_type;
      break;
  }
  return field_value;
}

template<FindFieldType find_type, Primitive::Type field_type, bool do_access_check,
         bool transaction_active>
bool DoFieldPut(Thread* self, const ShadowFrame& shadow_frame, const Instruction* inst,
                uint16_t inst_data) {
  bool do_assignability_check = do_access_check;
  bool is_static = (find_type == StaticObjectWrite) || (find_type == StaticPrimitiveWrite);
  uint32_t field_idx = is_static ? inst->VRegB_21c() : inst->VRegC_22c();
  ArtField* f = FindFieldFromCode<find_type, do_access_check>(field_idx, shadow_frame.GetMethod(), self,
                                                              Primitive::FieldSize(field_type));
  if (UNLIKELY(f == nullptr)) {
    CHECK(self->IsExceptionPending());
    return false;
  }
  Object* obj;
  if (is_static) {
    obj = f->GetDeclaringClass();
  } else {
    obj = shadow_frame.GetVRegReference(inst->VRegB_22c(inst_data));
    if (UNLIKELY(obj == nullptr)) {
      ThrowNullPointerExceptionForFieldAccess(shadow_frame.GetCurrentLocationForThrow(),
                                              f, false);
      return false;
    }
  }
  f->GetDeclaringClass()->AssertInitializedOrInitializingInThread(self);
  uint32_t vregA = is_static ? inst->VRegA_21c(inst_data) : inst->VRegA_22c(inst_data);
  // Report this field access to instrumentation if needed. Since we only have the offset of
  // the field from the base of the object, we need to look for it first.
  instrumentation::Instrumentation* instrumentation = Runtime::Current()->GetInstrumentation();
  if (UNLIKELY(instrumentation->HasFieldWriteListeners())) {
    JValue field_value = GetFieldValue<field_type>(shadow_frame, vregA);
    Object* this_object = f->IsStatic() ? nullptr : obj;
    instrumentation->FieldWriteEvent(self, this_object, shadow_frame.GetMethod(),
                                     shadow_frame.GetDexPC(), f, field_value);
  }
  switch (field_type) {
    case Primitive::kPrimBoolean:
      f->SetBoolean<transaction_active>(obj, shadow_frame.GetVReg(vregA));
      break;
    case Primitive::kPrimByte:
      f->SetByte<transaction_active>(obj, shadow_frame.GetVReg(vregA));
      break;
    case Primitive::kPrimChar:
      f->SetChar<transaction_active>(obj, shadow_frame.GetVReg(vregA));
      break;
    case Primitive::kPrimShort:
      f->SetShort<transaction_active>(obj, shadow_frame.GetVReg(vregA));
      break;
    case Primitive::kPrimInt:
      f->SetInt<transaction_active>(obj, shadow_frame.GetVReg(vregA));
      break;
    case Primitive::kPrimLong:
      f->SetLong<transaction_active>(obj, shadow_frame.GetVRegLong(vregA));
      break;
    case Primitive::kPrimNot: {
      Object* reg = shadow_frame.GetVRegReference(vregA);
      if (do_assignability_check && reg != nullptr) {
        // FieldHelper::GetType can resolve classes, use a handle wrapper which will restore the
        // object in the destructor.
        Class* field_class;
        {
          StackHandleScope<3> hs(self);
          HandleWrapper<mirror::ArtField> h_f(hs.NewHandleWrapper(&f));
          HandleWrapper<mirror::Object> h_reg(hs.NewHandleWrapper(&reg));
          HandleWrapper<mirror::Object> h_obj(hs.NewHandleWrapper(&obj));
          FieldHelper fh(h_f);
          field_class = fh.GetType();
        }
        if (!reg->VerifierInstanceOf(field_class)) {
          // This should never happen.
          std::string temp1, temp2, temp3;
          self->ThrowNewExceptionF(self->GetCurrentLocationForThrow(),
                                   "Ljava/lang/VirtualMachineError;",
                                   "Put '%s' that is not instance of field '%s' in '%s'",
                                   reg->GetClass()->GetDescriptor(&temp1),
                                   field_class->GetDescriptor(&temp2),
                                   f->GetDeclaringClass()->GetDescriptor(&temp3));
          return false;
        }
      }
      f->SetObj<transaction_active>(obj, reg);
      break;
    }
    default:
      LOG(FATAL) << "Unreachable: " << field_type;
  }
  return true;
}

// Explicitly instantiate all DoFieldPut functions.
#define EXPLICIT_DO_FIELD_PUT_TEMPLATE_DECL(_find_type, _field_type, _do_check, _transaction_active) \
  template bool DoFieldPut<_find_type, _field_type, _do_check, _transaction_active>(Thread* self, \
      const ShadowFrame& shadow_frame, const Instruction* inst, uint16_t inst_data)

#define EXPLICIT_DO_FIELD_PUT_ALL_TEMPLATE_DECL(_find_type, _field_type)  \
    EXPLICIT_DO_FIELD_PUT_TEMPLATE_DECL(_find_type, _field_type, false, false);  \
    EXPLICIT_DO_FIELD_PUT_TEMPLATE_DECL(_find_type, _field_type, true, false);  \
    EXPLICIT_DO_FIELD_PUT_TEMPLATE_DECL(_find_type, _field_type, false, true);  \
    EXPLICIT_DO_FIELD_PUT_TEMPLATE_DECL(_find_type, _field_type, true, true);

// iput-XXX
EXPLICIT_DO_FIELD_PUT_ALL_TEMPLATE_DECL(InstancePrimitiveWrite, Primitive::kPrimBoolean);
EXPLICIT_DO_FIELD_PUT_ALL_TEMPLATE_DECL(InstancePrimitiveWrite, Primitive::kPrimByte);
EXPLICIT_DO_FIELD_PUT_ALL_TEMPLATE_DECL(InstancePrimitiveWrite, Primitive::kPrimChar);
EXPLICIT_DO_FIELD_PUT_ALL_TEMPLATE_DECL(InstancePrimitiveWrite, Primitive::kPrimShort);
EXPLICIT_DO_FIELD_PUT_ALL_TEMPLATE_DECL(InstancePrimitiveWrite, Primitive::kPrimInt);
EXPLICIT_DO_FIELD_PUT_ALL_TEMPLATE_DECL(InstancePrimitiveWrite, Primitive::kPrimLong);
EXPLICIT_DO_FIELD_PUT_ALL_TEMPLATE_DECL(InstanceObjectWrite, Primitive::kPrimNot);

// sput-XXX
EXPLICIT_DO_FIELD_PUT_ALL_TEMPLATE_DECL(StaticPrimitiveWrite, Primitive::kPrimBoolean);
EXPLICIT_DO_FIELD_PUT_ALL_TEMPLATE_DECL(StaticPrimitiveWrite, Primitive::kPrimByte);
EXPLICIT_DO_FIELD_PUT_ALL_TEMPLATE_DECL(StaticPrimitiveWrite, Primitive::kPrimChar);
EXPLICIT_DO_FIELD_PUT_ALL_TEMPLATE_DECL(StaticPrimitiveWrite, Primitive::kPrimShort);
EXPLICIT_DO_FIELD_PUT_ALL_TEMPLATE_DECL(StaticPrimitiveWrite, Primitive::kPrimInt);
EXPLICIT_DO_FIELD_PUT_ALL_TEMPLATE_DECL(StaticPrimitiveWrite, Primitive::kPrimLong);
EXPLICIT_DO_FIELD_PUT_ALL_TEMPLATE_DECL(StaticObjectWrite, Primitive::kPrimNot);

#undef EXPLICIT_DO_FIELD_PUT_ALL_TEMPLATE_DECL
#undef EXPLICIT_DO_FIELD_PUT_TEMPLATE_DECL

template<Primitive::Type field_type, bool transaction_active>
bool DoIPutQuick(const ShadowFrame& shadow_frame, const Instruction* inst, uint16_t inst_data) {
  Object* obj = shadow_frame.GetVRegReference(inst->VRegB_22c(inst_data));
  if (UNLIKELY(obj == nullptr)) {
    // We lost the reference to the field index so we cannot get a more
    // precised exception message.
    ThrowNullPointerExceptionFromDexPC(shadow_frame.GetCurrentLocationForThrow());
    return false;
  }
  MemberOffset field_offset(inst->VRegC_22c());
  const uint32_t vregA = inst->VRegA_22c(inst_data);
  // Report this field modification to instrumentation if needed. Since we only have the offset of
  // the field from the base of the object, we need to look for it first.
  instrumentation::Instrumentation* instrumentation = Runtime::Current()->GetInstrumentation();
  if (UNLIKELY(instrumentation->HasFieldWriteListeners())) {
    ArtField* f = ArtField::FindInstanceFieldWithOffset(obj->GetClass(),
                                                        field_offset.Uint32Value());
    DCHECK(f != nullptr);
    DCHECK(!f->IsStatic());
    JValue field_value = GetFieldValue<field_type>(shadow_frame, vregA);
    instrumentation->FieldWriteEvent(Thread::Current(), obj, shadow_frame.GetMethod(),
                                     shadow_frame.GetDexPC(), f, field_value);
  }
  // Note: iput-x-quick instructions are only for non-volatile fields.
  switch (field_type) {
    case Primitive::kPrimInt:
      obj->SetField32<transaction_active>(field_offset, shadow_frame.GetVReg(vregA));
      break;
    case Primitive::kPrimLong:
      obj->SetField64<transaction_active>(field_offset, shadow_frame.GetVRegLong(vregA));
      break;
    case Primitive::kPrimNot:
      obj->SetFieldObject<transaction_active>(field_offset, shadow_frame.GetVRegReference(vregA));
      break;
    default:
      LOG(FATAL) << "Unreachable: " << field_type;
  }
  return true;
}

// Explicitly instantiate all DoIPutQuick functions.
#define EXPLICIT_DO_IPUT_QUICK_TEMPLATE_DECL(_field_type, _transaction_active) \
  template bool DoIPutQuick<_field_type, _transaction_active>(const ShadowFrame& shadow_frame, \
                                                              const Instruction* inst, \
                                                              uint16_t inst_data)

#define EXPLICIT_DO_IPUT_QUICK_ALL_TEMPLATE_DECL(_field_type)   \
  EXPLICIT_DO_IPUT_QUICK_TEMPLATE_DECL(_field_type, false);     \
  EXPLICIT_DO_IPUT_QUICK_TEMPLATE_DECL(_field_type, true);

EXPLICIT_DO_IPUT_QUICK_ALL_TEMPLATE_DECL(Primitive::kPrimInt);    // iget-quick.
EXPLICIT_DO_IPUT_QUICK_ALL_TEMPLATE_DECL(Primitive::kPrimLong);   // iget-wide-quick.
EXPLICIT_DO_IPUT_QUICK_ALL_TEMPLATE_DECL(Primitive::kPrimNot);    // iget-object-quick.
#undef EXPLICIT_DO_IPUT_QUICK_ALL_TEMPLATE_DECL
#undef EXPLICIT_DO_IPUT_QUICK_TEMPLATE_DECL

/**
 * Finds the location where this exception will be caught. We search until we reach either the top
 * frame or a native frame, in which cases this exception is considered uncaught.
 */
class CatchLocationFinder : public StackVisitor {
 public:
  explicit CatchLocationFinder(Thread* self, Handle<mirror::Throwable>* exception)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_)
    : StackVisitor(self, nullptr), self_(self), handle_scope_(self), exception_(exception),
      catch_method_(handle_scope_.NewHandle<mirror::ArtMethod>(nullptr)),
      catch_dex_pc_(DexFile::kDexNoIndex), clear_exception_(false) {
  }

  bool VisitFrame() OVERRIDE SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    mirror::ArtMethod* method = GetMethod();
    if (method == nullptr) {
      return true;
    }
    if (method->IsRuntimeMethod()) {
      // Ignore callee save method.
      DCHECK(method->IsCalleeSaveMethod());
      return true;
    }
    if (method->IsNative()) {
      return false;  // End stack walk.
    }
    DCHECK(!method->IsNative());
    uint32_t dex_pc = GetDexPc();
    if (dex_pc != DexFile::kDexNoIndex) {
      uint32_t found_dex_pc;
      {
        StackHandleScope<3> hs(self_);
        Handle<mirror::Class> exception_class(hs.NewHandle((*exception_)->GetClass()));
        Handle<mirror::ArtMethod> h_method(hs.NewHandle(method));
        found_dex_pc = mirror::ArtMethod::FindCatchBlock(h_method, exception_class, dex_pc,
                                                         &clear_exception_);
      }
      if (found_dex_pc != DexFile::kDexNoIndex) {
        catch_method_.Assign(method);
        catch_dex_pc_ = found_dex_pc;
        return false;  // End stack walk.
      }
    }
    return true;  // Continue stack walk.
  }

  ArtMethod* GetCatchMethod() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return catch_method_.Get();
  }

  uint32_t GetCatchDexPc() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return catch_dex_pc_;
  }

  bool NeedClearException() SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    return clear_exception_;
  }

 private:
  Thread* const self_;
  StackHandleScope<1> handle_scope_;
  Handle<mirror::Throwable>* exception_;
  Handle<mirror::ArtMethod> catch_method_;
  uint32_t catch_dex_pc_;
  bool clear_exception_;


  DISALLOW_COPY_AND_ASSIGN(CatchLocationFinder);
};

uint32_t FindNextInstructionFollowingException(Thread* self,
                                               ShadowFrame& shadow_frame,
                                               uint32_t dex_pc,
                                               const instrumentation::Instrumentation* instrumentation) {
  self->VerifyStack();
  ThrowLocation throw_location;
  StackHandleScope<3> hs(self);
  Handle<mirror::Throwable> exception(hs.NewHandle(self->GetException(&throw_location)));
  if (!self->IsExceptionReportedToInstrumentation() && instrumentation->HasExceptionCaughtListeners()) {
    CatchLocationFinder clf(self, &exception);
    clf.WalkStack(false);
    instrumentation->ExceptionCaughtEvent(self, throw_location, clf.GetCatchMethod(),
                                          clf.GetCatchDexPc(), exception.Get());
    self->SetExceptionReportedToInstrumentation(true);
  }
  bool clear_exception = false;
  uint32_t found_dex_pc;
  {
    Handle<mirror::Class> exception_class(hs.NewHandle(exception->GetClass()));
    Handle<mirror::ArtMethod> h_method(hs.NewHandle(shadow_frame.GetMethod()));
    found_dex_pc = mirror::ArtMethod::FindCatchBlock(h_method, exception_class, dex_pc,
                                                     &clear_exception);
  }
  if (found_dex_pc == DexFile::kDexNoIndex) {
    instrumentation->MethodUnwindEvent(self, shadow_frame.GetThisObject(),
                                       shadow_frame.GetMethod(), dex_pc);
  } else {
    if (self->IsExceptionReportedToInstrumentation()) {
      instrumentation->MethodUnwindEvent(self, shadow_frame.GetThisObject(),
                                         shadow_frame.GetMethod(), dex_pc);
    }
    if (clear_exception) {
      self->ClearException();
    }
  }
  return found_dex_pc;
}

void UnexpectedOpcode(const Instruction* inst, MethodHelper& mh) {
  LOG(FATAL) << "Unexpected instruction: " << inst->DumpString(mh.GetMethod()->GetDexFile());
  exit(0);  // Unreachable, keep GCC happy.
}

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
  const DexFile::CodeItem* code_item = method->GetCodeItem();
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
  StackHandleScope<1> hs(self);
  MethodHelper mh(hs.NewHandle(method));
  if (do_assignability_check) {
    // Slow path.
    // We might need to do class loading, which incurs a thread state change to kNative. So
    // register the shadow frame as under construction and allow suspension again.
    self->SetShadowFrameUnderConstruction(new_shadow_frame);
    self->EndAssertNoThreadSuspension(old_cause);

    // We need to do runtime check on reference assignment. We need to load the shorty
    // to get the exact type of each reference argument.
    const DexFile::TypeList* params = method->GetParameterTypeList();
    uint32_t shorty_len = 0;
    const char* shorty = method->GetShorty(&shorty_len);

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
      size_t receiver_reg = is_range ? vregC : arg[0];
      new_shadow_frame->SetVRegReference(dest_reg, shadow_frame.GetVRegReference(receiver_reg));
      ++dest_reg;
      ++arg_offset;
    }
    for (uint32_t shorty_pos = 0; dest_reg < num_regs; ++shorty_pos, ++dest_reg, ++arg_offset) {
      DCHECK_LT(shorty_pos + 1, shorty_len);
      const size_t src_reg = (is_range) ? vregC + arg_offset : arg[arg_offset];
      switch (shorty[shorty_pos + 1]) {
        case 'L': {
          Object* o = shadow_frame.GetVRegReference(src_reg);
          if (do_assignability_check && o != NULL) {
            Class* arg_type = mh.GetClassFromTypeIdx(params->GetTypeItem(shorty_pos).type_idx_);
            if (arg_type == NULL) {
              CHECK(self->IsExceptionPending());
              return false;
            }
            if (!o->VerifierInstanceOf(arg_type)) {
              // This should never happen.
              std::string temp1, temp2;
              self->ThrowNewExceptionF(self->GetCurrentLocationForThrow(),
                                       "Ljava/lang/VirtualMachineError;",
                                       "Invoking %s with bad arg %d, type '%s' not instance of '%s'",
                                       method->GetName(), shorty_pos,
                                       o->GetClass()->GetDescriptor(&temp1),
                                       arg_type->GetDescriptor(&temp2));
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
    // We're done with the construction.
    self->ClearShadowFrameUnderConstruction();
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
    self->EndAssertNoThreadSuspension(old_cause);
  }

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

// Helper function to deal with class loading in an unstarted runtime.
static void UnstartedRuntimeFindClass(Thread* self, Handle<mirror::String> className,
                                      Handle<mirror::ClassLoader> class_loader, JValue* result,
                                      const std::string& method_name, bool initialize_class,
                                      bool abort_if_not_found)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  CHECK(className.Get() != nullptr);
  std::string descriptor(DotToDescriptor(className->ToModifiedUtf8().c_str()));
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();

  Class* found = class_linker->FindClass(self, descriptor.c_str(), class_loader);
  if (found == nullptr && abort_if_not_found) {
    if (!self->IsExceptionPending()) {
      AbortTransaction(self, "%s failed in un-started runtime for class: %s",
                       method_name.c_str(), PrettyDescriptor(descriptor.c_str()).c_str());
    }
    return;
  }
  if (found != nullptr && initialize_class) {
    StackHandleScope<1> hs(self);
    Handle<mirror::Class> h_class(hs.NewHandle(found));
    if (!class_linker->EnsureInitialized(h_class, true, true)) {
      CHECK(self->IsExceptionPending());
      return;
    }
  }
  result->SetL(found);
}

static void UnstartedRuntimeInvoke(Thread* self, MethodHelper& mh,
                                   const DexFile::CodeItem* code_item, ShadowFrame* shadow_frame,
                                   JValue* result, size_t arg_offset) {
  // In a runtime that's not started we intercept certain methods to avoid complicated dependency
  // problems in core libraries.
  std::string name(PrettyMethod(shadow_frame->GetMethod()));
  if (name == "java.lang.Class java.lang.Class.forName(java.lang.String)") {
    // TODO: Support for the other variants that take more arguments should also be added.
    mirror::String* class_name = shadow_frame->GetVRegReference(arg_offset)->AsString();
    StackHandleScope<1> hs(self);
    Handle<mirror::String> h_class_name(hs.NewHandle(class_name));
    UnstartedRuntimeFindClass(self, h_class_name, NullHandle<mirror::ClassLoader>(), result, name,
                              true, true);
  } else if (name == "java.lang.Class java.lang.VMClassLoader.loadClass(java.lang.String, boolean)") {
    mirror::String* class_name = shadow_frame->GetVRegReference(arg_offset)->AsString();
    StackHandleScope<1> hs(self);
    Handle<mirror::String> h_class_name(hs.NewHandle(class_name));
    UnstartedRuntimeFindClass(self, h_class_name, NullHandle<mirror::ClassLoader>(), result, name,
                              false, true);
  } else if (name == "java.lang.Class java.lang.VMClassLoader.findLoadedClass(java.lang.ClassLoader, java.lang.String)") {
    mirror::String* class_name = shadow_frame->GetVRegReference(arg_offset + 1)->AsString();
    mirror::ClassLoader* class_loader =
        down_cast<mirror::ClassLoader*>(shadow_frame->GetVRegReference(arg_offset));
    StackHandleScope<2> hs(self);
    Handle<mirror::String> h_class_name(hs.NewHandle(class_name));
    Handle<mirror::ClassLoader> h_class_loader(hs.NewHandle(class_loader));
    UnstartedRuntimeFindClass(self, h_class_name, h_class_loader, result, name, false, false);
  } else if (name == "java.lang.Class java.lang.Void.lookupType()") {
    result->SetL(Runtime::Current()->GetClassLinker()->FindPrimitiveClass('V'));
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
    ObjectArray<ArtField>* fields = klass->GetIFields();
    for (int32_t i = 0; i < fields->GetLength() && found == NULL; ++i) {
      ArtField* f = fields->Get(i);
      if (name->Equals(f->GetName())) {
        found = f;
      }
    }
    if (found == NULL) {
      fields = klass->GetSFields();
      for (int32_t i = 0; i < fields->GetLength() && found == NULL; ++i) {
        ArtField* f = fields->Get(i);
        if (name->Equals(f->GetName())) {
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
    StackHandleScope<1> hs(self);
    MethodHelper mh(hs.NewHandle(shadow_frame->GetVRegReference(arg_offset)->AsArtMethod()));
    result->SetL(mh.GetNameAsString(self));
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
