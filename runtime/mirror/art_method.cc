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

#include "art_method.h"

#include "art_field-inl.h"
#include "art_method-inl.h"
#include "base/stringpiece.h"
#include "class-inl.h"
#include "dex_file-inl.h"
#include "dex_instruction.h"
#include "gc/accounting/card_table-inl.h"
#include "interpreter/interpreter.h"
#include "jni_internal.h"
#include "mapping_table.h"
#include "object-inl.h"
#include "object_array.h"
#include "object_array-inl.h"
#include "scoped_thread_state_change.h"
#include "string.h"
#include "object_utils.h"
#include "well_known_classes.h"

namespace art {
namespace mirror {

extern "C" void art_portable_invoke_stub(ArtMethod*, uint32_t*, uint32_t, Thread*, JValue*, char);
extern "C" void art_quick_invoke_stub(ArtMethod*, uint32_t*, uint32_t, Thread*, JValue*,
                                      const char*);
#ifdef __LP64__
extern "C" void art_quick_invoke_static_stub(ArtMethod*, uint32_t*, uint32_t, Thread*, JValue*,
                                             const char*);
#endif

// TODO: get global references for these
Class* ArtMethod::java_lang_reflect_ArtMethod_ = NULL;

ArtMethod* ArtMethod::FromReflectedMethod(const ScopedObjectAccess& soa, jobject jlr_method) {
  mirror::ArtField* f =
      soa.DecodeField(WellKnownClasses::java_lang_reflect_AbstractMethod_artMethod);
  mirror::ArtMethod* method = f->GetObject(soa.Decode<mirror::Object*>(jlr_method))->AsArtMethod();
  DCHECK(method != nullptr);
  return method;
}


void ArtMethod::VisitRoots(RootCallback* callback, void* arg) {
  if (java_lang_reflect_ArtMethod_ != nullptr) {
    callback(reinterpret_cast<mirror::Object**>(&java_lang_reflect_ArtMethod_), arg, 0,
             kRootStickyClass);
  }
}

InvokeType ArtMethod::GetInvokeType() {
  // TODO: kSuper?
  if (GetDeclaringClass()->IsInterface()) {
    return kInterface;
  } else if (IsStatic()) {
    return kStatic;
  } else if (IsDirect()) {
    return kDirect;
  } else {
    return kVirtual;
  }
}

void ArtMethod::SetClass(Class* java_lang_reflect_ArtMethod) {
  CHECK(java_lang_reflect_ArtMethod_ == NULL);
  CHECK(java_lang_reflect_ArtMethod != NULL);
  java_lang_reflect_ArtMethod_ = java_lang_reflect_ArtMethod;
}

void ArtMethod::ResetClass() {
  CHECK(java_lang_reflect_ArtMethod_ != NULL);
  java_lang_reflect_ArtMethod_ = NULL;
}

void ArtMethod::SetDexCacheStrings(ObjectArray<String>* new_dex_cache_strings) {
  SetFieldObject<false>(OFFSET_OF_OBJECT_MEMBER(ArtMethod, dex_cache_strings_),
                        new_dex_cache_strings);
}

void ArtMethod::SetDexCacheResolvedMethods(ObjectArray<ArtMethod>* new_dex_cache_methods) {
  SetFieldObject<false>(OFFSET_OF_OBJECT_MEMBER(ArtMethod, dex_cache_resolved_methods_),
                        new_dex_cache_methods);
}

void ArtMethod::SetDexCacheResolvedTypes(ObjectArray<Class>* new_dex_cache_classes) {
  SetFieldObject<false>(OFFSET_OF_OBJECT_MEMBER(ArtMethod, dex_cache_resolved_types_),
                        new_dex_cache_classes);
}

size_t ArtMethod::NumArgRegisters(const StringPiece& shorty) {
  CHECK_LE(1, shorty.length());
  uint32_t num_registers = 0;
  for (int i = 1; i < shorty.length(); ++i) {
    char ch = shorty[i];
    if (ch == 'D' || ch == 'J') {
      num_registers += 2;
    } else {
      num_registers += 1;
    }
  }
  return num_registers;
}

bool ArtMethod::IsProxyMethod() {
  return GetDeclaringClass()->IsProxyClass();
}

ArtMethod* ArtMethod::FindOverriddenMethod() {
  if (IsStatic()) {
    return NULL;
  }
  Class* declaring_class = GetDeclaringClass();
  Class* super_class = declaring_class->GetSuperClass();
  uint16_t method_index = GetMethodIndex();
  ObjectArray<ArtMethod>* super_class_vtable = super_class->GetVTable();
  ArtMethod* result = NULL;
  // Did this method override a super class method? If so load the result from the super class'
  // vtable
  if (super_class_vtable != NULL && method_index < super_class_vtable->GetLength()) {
    result = super_class_vtable->Get(method_index);
  } else {
    // Method didn't override superclass method so search interfaces
    if (IsProxyMethod()) {
      result = GetDexCacheResolvedMethods()->Get(GetDexMethodIndex());
      CHECK_EQ(result,
               Runtime::Current()->GetClassLinker()->FindMethodForProxy(GetDeclaringClass(), this));
    } else {
      MethodHelper mh(this);
      MethodHelper interface_mh;
      IfTable* iftable = GetDeclaringClass()->GetIfTable();
      for (size_t i = 0; i < iftable->Count() && result == NULL; i++) {
        Class* interface = iftable->GetInterface(i);
        for (size_t j = 0; j < interface->NumVirtualMethods(); ++j) {
          ArtMethod* interface_method = interface->GetVirtualMethod(j);
          interface_mh.ChangeMethod(interface_method);
          if (mh.HasSameNameAndSignature(&interface_mh)) {
            result = interface_method;
            break;
          }
        }
      }
    }
  }
#ifndef NDEBUG
  MethodHelper result_mh(result);
  DCHECK(result == NULL || MethodHelper(this).HasSameNameAndSignature(&result_mh));
#endif
  return result;
}

uintptr_t ArtMethod::NativePcOffset(const uintptr_t pc) {
  const void* code = Runtime::Current()->GetInstrumentation()->GetQuickCodeFor(this);
  return pc - reinterpret_cast<uintptr_t>(code);
}

uint32_t ArtMethod::ToDexPc(const uintptr_t pc, bool abort_on_failure) {
  if (IsPortableCompiled()) {
    // Portable doesn't use the machine pc, we just use dex pc instead.
    return static_cast<uint32_t>(pc);
  }
  MappingTable table(GetMappingTable());
  if (table.TotalSize() == 0) {
    DCHECK(IsNative() || IsCalleeSaveMethod() || IsProxyMethod()) << PrettyMethod(this);
    return DexFile::kDexNoIndex;   // Special no mapping case
  }
  const void* code = Runtime::Current()->GetInstrumentation()->GetQuickCodeFor(this);
  uint32_t sought_offset = pc - reinterpret_cast<uintptr_t>(code);
  // Assume the caller wants a pc-to-dex mapping so check here first.
  typedef MappingTable::PcToDexIterator It;
  for (It cur = table.PcToDexBegin(), end = table.PcToDexEnd(); cur != end; ++cur) {
    if (cur.NativePcOffset() == sought_offset) {
      return cur.DexPc();
    }
  }
  // Now check dex-to-pc mappings.
  typedef MappingTable::DexToPcIterator It2;
  for (It2 cur = table.DexToPcBegin(), end = table.DexToPcEnd(); cur != end; ++cur) {
    if (cur.NativePcOffset() == sought_offset) {
      return cur.DexPc();
    }
  }
  if (abort_on_failure) {
      LOG(FATAL) << "Failed to find Dex offset for PC offset " << reinterpret_cast<void*>(sought_offset)
             << "(PC " << reinterpret_cast<void*>(pc) << ", code=" << code
             << ") in " << PrettyMethod(this);
  }
  return DexFile::kDexNoIndex;
}

uintptr_t ArtMethod::ToNativePc(const uint32_t dex_pc) {
  MappingTable table(GetMappingTable());
  if (table.TotalSize() == 0) {
    DCHECK_EQ(dex_pc, 0U);
    return 0;   // Special no mapping/pc == 0 case
  }
  // Assume the caller wants a dex-to-pc mapping so check here first.
  typedef MappingTable::DexToPcIterator It;
  for (It cur = table.DexToPcBegin(), end = table.DexToPcEnd(); cur != end; ++cur) {
    if (cur.DexPc() == dex_pc) {
      const void* code = Runtime::Current()->GetInstrumentation()->GetQuickCodeFor(this);
      return reinterpret_cast<uintptr_t>(code) + cur.NativePcOffset();
    }
  }
  // Now check pc-to-dex mappings.
  typedef MappingTable::PcToDexIterator It2;
  for (It2 cur = table.PcToDexBegin(), end = table.PcToDexEnd(); cur != end; ++cur) {
    if (cur.DexPc() == dex_pc) {
      const void* code = Runtime::Current()->GetInstrumentation()->GetQuickCodeFor(this);
      return reinterpret_cast<uintptr_t>(code) + cur.NativePcOffset();
    }
  }
  LOG(FATAL) << "Failed to find native offset for dex pc 0x" << std::hex << dex_pc
             << " in " << PrettyMethod(this);
  return 0;
}

uint32_t ArtMethod::FindCatchBlock(Handle<Class>& exception_type, uint32_t dex_pc,
                                   bool* has_no_move_exception, bool* exc_changed) {
  MethodHelper mh(this);
  const DexFile::CodeItem* code_item = mh.GetCodeItem();
  // Set aside the exception while we resolve its type.
  Thread* self = Thread::Current();
  ThrowLocation throw_location;
  StackHandleScope<1> hs(self);
  Handle<mirror::Throwable> exception(hs.NewHandle(self->GetException(&throw_location)));
  self->ClearException();
  // Default to handler not found.
  uint32_t found_dex_pc = DexFile::kDexNoIndex;
  // Iterate over the catch handlers associated with dex_pc.
  for (CatchHandlerIterator it(*code_item, dex_pc); it.HasNext(); it.Next()) {
    uint16_t iter_type_idx = it.GetHandlerTypeIndex();
    // Catch all case
    if (iter_type_idx == DexFile::kDexNoIndex16) {
      found_dex_pc = it.GetHandlerAddress();
      break;
    }
    // Does this catch exception type apply?
    Class* iter_exception_type = mh.GetClassFromTypeIdx(iter_type_idx);
    if (iter_exception_type == nullptr) {
      // Now have a NoClassDefFoundError as exception.
      // Note: this is not RI behavior. RI would have failed when loading the class.
      *exc_changed = true;

      // TODO: Add old exception as suppressed.
      LOG(WARNING) << "Unresolved exception class when finding catch block: "
        << mh.GetTypeDescriptorFromTypeIdx(iter_type_idx);

      // Return immediately.
      return DexFile::kDexNoIndex;
    } else if (iter_exception_type->IsAssignableFrom(exception_type.Get())) {
      found_dex_pc = it.GetHandlerAddress();
      break;
    }
  }
  if (found_dex_pc != DexFile::kDexNoIndex) {
    const Instruction* first_catch_instr =
        Instruction::At(&code_item->insns_[found_dex_pc]);
    *has_no_move_exception = (first_catch_instr->Opcode() != Instruction::MOVE_EXCEPTION);
  }
  // Put the exception back.
  if (exception.Get() != nullptr) {
    self->SetException(throw_location, exception.Get());
  }
  return found_dex_pc;
}

void ArtMethod::Invoke(Thread* self, uint32_t* args, uint32_t args_size, JValue* result,
                       const char* shorty) {
  if (kIsDebugBuild) {
    self->AssertThreadSuspensionIsAllowable();
    CHECK_EQ(kRunnable, self->GetState());
    CHECK_STREQ(MethodHelper(this).GetShorty(), shorty);
  }

  // Push a transition back into managed code onto the linked list in thread.
  ManagedStack fragment;
  self->PushManagedStackFragment(&fragment);

  Runtime* runtime = Runtime::Current();
  // Call the invoke stub, passing everything as arguments.
  if (UNLIKELY(!runtime->IsStarted())) {
    if (IsStatic()) {
      art::interpreter::EnterInterpreterFromInvoke(self, this, nullptr, args, result);
    } else {
      Object* receiver = reinterpret_cast<StackReference<Object>*>(&args[0])->AsMirrorPtr();
      art::interpreter::EnterInterpreterFromInvoke(self, this, receiver, args + 1, result);
    }
  } else {
    const bool kLogInvocationStartAndReturn = false;
    bool have_quick_code = GetEntryPointFromQuickCompiledCode() != nullptr;
    bool have_portable_code = GetEntryPointFromPortableCompiledCode() != nullptr;
    if (LIKELY(have_quick_code || have_portable_code)) {
      if (kLogInvocationStartAndReturn) {
        LOG(INFO) << StringPrintf("Invoking '%s' %s code=%p", PrettyMethod(this).c_str(),
                                  have_quick_code ? "quick" : "portable",
                                  have_quick_code ? GetEntryPointFromQuickCompiledCode()
                                                  : GetEntryPointFromPortableCompiledCode());
      }
      if (!IsPortableCompiled()) {
#ifdef __LP64__
        if (!IsStatic()) {
          (*art_quick_invoke_stub)(this, args, args_size, self, result, shorty);
        } else {
          (*art_quick_invoke_static_stub)(this, args, args_size, self, result, shorty);
        }
#else
        (*art_quick_invoke_stub)(this, args, args_size, self, result, shorty);
#endif
      } else {
        (*art_portable_invoke_stub)(this, args, args_size, self, result, shorty[0]);
      }
      if (UNLIKELY(self->GetException(nullptr) == Thread::GetDeoptimizationException())) {
        // Unusual case where we were running generated code and an
        // exception was thrown to force the activations to be removed from the
        // stack. Continue execution in the interpreter.
        self->ClearException();
        ShadowFrame* shadow_frame = self->GetAndClearDeoptimizationShadowFrame(result);
        self->SetTopOfStack(nullptr, 0);
        self->SetTopOfShadowStack(shadow_frame);
        interpreter::EnterInterpreterFromDeoptimize(self, shadow_frame, result);
      }
      if (kLogInvocationStartAndReturn) {
        LOG(INFO) << StringPrintf("Returned '%s' %s code=%p", PrettyMethod(this).c_str(),
                                  have_quick_code ? "quick" : "portable",
                                  have_quick_code ? GetEntryPointFromQuickCompiledCode()
                                                  : GetEntryPointFromPortableCompiledCode());
      }
    } else {
      LOG(INFO) << "Not invoking '" << PrettyMethod(this) << "' code=null";
      if (result != NULL) {
        result->SetJ(0);
      }
    }
  }

  // Pop transition.
  self->PopManagedStackFragment(fragment);
}

bool ArtMethod::IsRegistered() {
  void* native_method =
      GetFieldPtr<void*>(OFFSET_OF_OBJECT_MEMBER(ArtMethod, entry_point_from_jni_));
  CHECK(native_method != nullptr);
  void* jni_stub = GetJniDlsymLookupStub();
  return native_method != jni_stub;
}

void ArtMethod::RegisterNative(Thread* self, const void* native_method, bool is_fast) {
  DCHECK(Thread::Current() == self);
  CHECK(IsNative()) << PrettyMethod(this);
  CHECK(!IsFastNative()) << PrettyMethod(this);
  CHECK(native_method != NULL) << PrettyMethod(this);
  if (is_fast) {
    SetAccessFlags(GetAccessFlags() | kAccFastNative);
  }
  SetNativeMethod(native_method);
}

void ArtMethod::UnregisterNative(Thread* self) {
  CHECK(IsNative() && !IsFastNative()) << PrettyMethod(this);
  // restore stub to lookup native pointer via dlsym
  RegisterNative(self, GetJniDlsymLookupStub(), false);
}

const void* ArtMethod::GetOatCodePointer() {
  if (IsPortableCompiled() || IsNative() || IsAbstract() || IsRuntimeMethod() || IsProxyMethod()) {
    return nullptr;
  }
  Runtime* runtime = Runtime::Current();
  const void* entry_point = runtime->GetInstrumentation()->GetQuickCodeFor(this);
  // On failure, instead of nullptr we get the quick-to-interpreter-bridge (but not the trampoline).
  DCHECK(entry_point != GetQuickToInterpreterBridgeTrampoline(runtime->GetClassLinker()));
  if (entry_point == GetQuickToInterpreterBridge()) {
    return nullptr;
  }
  return EntryPointToCodePointer(entry_point);
}

const uint8_t* ArtMethod::GetMappingTable() {
  const void* code = GetOatCodePointer();
  if (code == nullptr) {
    return nullptr;
  }
  uint32_t offset = reinterpret_cast<const OatQuickMethodHeader*>(code)[-1].mapping_table_offset_;
  if (UNLIKELY(offset == 0u)) {
    return nullptr;
  }
  return reinterpret_cast<const uint8_t*>(code) - offset;
}

const uint8_t* ArtMethod::GetVmapTable() {
  const void* code = GetOatCodePointer();
  if (code == nullptr) {
    return nullptr;
  }
  uint32_t offset = reinterpret_cast<const OatQuickMethodHeader*>(code)[-1].vmap_table_offset_;
  if (UNLIKELY(offset == 0u)) {
    return nullptr;
  }
  return reinterpret_cast<const uint8_t*>(code) - offset;
}

}  // namespace mirror
}  // namespace art
