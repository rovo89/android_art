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

#include "arch/context.h"
#include "art_field-inl.h"
#include "art_method-inl.h"
#include "base/stringpiece.h"
#include "class-inl.h"
#include "dex_file-inl.h"
#include "dex_instruction.h"
#include "entrypoints/entrypoint_utils.h"
#include "entrypoints/runtime_asm_entrypoints.h"
#include "gc/accounting/card_table-inl.h"
#include "interpreter/interpreter.h"
#include "jni_internal.h"
#include "mapping_table.h"
#include "method_helper-inl.h"
#include "object_array-inl.h"
#include "object_array.h"
#include "object-inl.h"
#include "scoped_thread_state_change.h"
#include "string.h"
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
GcRoot<Class> ArtMethod::java_lang_reflect_ArtMethod_;

ArtMethod* ArtMethod::FromReflectedMethod(const ScopedObjectAccessAlreadyRunnable& soa,
                                          jobject jlr_method) {
  mirror::ArtField* f =
      soa.DecodeField(WellKnownClasses::java_lang_reflect_AbstractMethod_artMethod);
  mirror::ArtMethod* method = f->GetObject(soa.Decode<mirror::Object*>(jlr_method))->AsArtMethod();
  DCHECK(method != nullptr);
  return method;
}


void ArtMethod::VisitRoots(RootCallback* callback, void* arg) {
  if (!java_lang_reflect_ArtMethod_.IsNull()) {
    java_lang_reflect_ArtMethod_.VisitRoot(callback, arg, 0, kRootStickyClass);
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
  CHECK(java_lang_reflect_ArtMethod_.IsNull());
  CHECK(java_lang_reflect_ArtMethod != NULL);
  java_lang_reflect_ArtMethod_ = GcRoot<Class>(java_lang_reflect_ArtMethod);
}

void ArtMethod::ResetClass() {
  CHECK(!java_lang_reflect_ArtMethod_.IsNull());
  java_lang_reflect_ArtMethod_ = GcRoot<Class>(nullptr);
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
  CHECK_LE(1U, shorty.length());
  uint32_t num_registers = 0;
  for (size_t i = 1; i < shorty.length(); ++i) {
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
  ArtMethod* result = NULL;
  // Did this method override a super class method? If so load the result from the super class'
  // vtable
  if (super_class->HasVTable() && method_index < super_class->GetVTableLength()) {
    result = super_class->GetVTableEntry(method_index);
  } else {
    // Method didn't override superclass method so search interfaces
    if (IsProxyMethod()) {
      result = GetDexCacheResolvedMethods()->Get(GetDexMethodIndex());
      CHECK_EQ(result,
               Runtime::Current()->GetClassLinker()->FindMethodForProxy(GetDeclaringClass(), this));
    } else {
      StackHandleScope<2> hs(Thread::Current());
      MethodHelper mh(hs.NewHandle(this));
      MutableMethodHelper interface_mh(hs.NewHandle<mirror::ArtMethod>(nullptr));
      IfTable* iftable = GetDeclaringClass()->GetIfTable();
      for (size_t i = 0; i < iftable->Count() && result == NULL; i++) {
        Class* interface = iftable->GetInterface(i);
        for (size_t j = 0; j < interface->NumVirtualMethods(); ++j) {
          interface_mh.ChangeMethod(interface->GetVirtualMethod(j));
          if (mh.HasSameNameAndSignature(&interface_mh)) {
            result = interface_mh.GetMethod();
            break;
          }
        }
      }
    }
  }
  if (kIsDebugBuild) {
    StackHandleScope<2> hs(Thread::Current());
    MethodHelper result_mh(hs.NewHandle(result));
    MethodHelper this_mh(hs.NewHandle(this));
    DCHECK(result == nullptr || this_mh.HasSameNameAndSignature(&result_mh));
  }
  return result;
}

uint32_t ArtMethod::ToDexPc(const uintptr_t pc, bool abort_on_failure) {
  if (IsPortableCompiled()) {
    // Portable doesn't use the machine pc, we just use dex pc instead.
    return static_cast<uint32_t>(pc);
  }
  const void* entry_point = GetQuickOatEntryPoint();
  MappingTable table(
      entry_point != nullptr ? GetMappingTable(EntryPointToCodePointer(entry_point)) : nullptr);
  if (table.TotalSize() == 0) {
    // NOTE: Special methods (see Mir2Lir::GenSpecialCase()) have an empty mapping
    // but they have no suspend checks and, consequently, we never call ToDexPc() for them.
    DCHECK(IsNative() || IsCalleeSaveMethod() || IsProxyMethod()) << PrettyMethod(this);
    return DexFile::kDexNoIndex;   // Special no mapping case
  }
  uint32_t sought_offset = pc - reinterpret_cast<uintptr_t>(entry_point);
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
             << "(PC " << reinterpret_cast<void*>(pc) << ", entry_point=" << entry_point
             << ") in " << PrettyMethod(this);
  }
  return DexFile::kDexNoIndex;
}

uintptr_t ArtMethod::ToNativeQuickPc(const uint32_t dex_pc) {
  const void* entry_point = GetQuickOatEntryPoint();
  MappingTable table(
      entry_point != nullptr ? GetMappingTable(EntryPointToCodePointer(entry_point)) : nullptr);
  if (table.TotalSize() == 0) {
    DCHECK_EQ(dex_pc, 0U);
    return 0;   // Special no mapping/pc == 0 case
  }
  // Assume the caller wants a dex-to-pc mapping so check here first.
  typedef MappingTable::DexToPcIterator It;
  for (It cur = table.DexToPcBegin(), end = table.DexToPcEnd(); cur != end; ++cur) {
    if (cur.DexPc() == dex_pc) {
      return reinterpret_cast<uintptr_t>(entry_point) + cur.NativePcOffset();
    }
  }
  // Now check pc-to-dex mappings.
  typedef MappingTable::PcToDexIterator It2;
  for (It2 cur = table.PcToDexBegin(), end = table.PcToDexEnd(); cur != end; ++cur) {
    if (cur.DexPc() == dex_pc) {
      return reinterpret_cast<uintptr_t>(entry_point) + cur.NativePcOffset();
    }
  }
  LOG(FATAL) << "Failed to find native offset for dex pc 0x" << std::hex << dex_pc
             << " in " << PrettyMethod(this);
  return 0;
}

uint32_t ArtMethod::FindCatchBlock(Handle<ArtMethod> h_this, Handle<Class> exception_type,
                                   uint32_t dex_pc, bool* has_no_move_exception) {
  MethodHelper mh(h_this);
  const DexFile::CodeItem* code_item = h_this->GetCodeItem();
  // Set aside the exception while we resolve its type.
  Thread* self = Thread::Current();
  ThrowLocation throw_location;
  StackHandleScope<1> hs(self);
  Handle<mirror::Throwable> exception(hs.NewHandle(self->GetException(&throw_location)));
  bool is_exception_reported = self->IsExceptionReportedToInstrumentation();
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
    if (UNLIKELY(iter_exception_type == nullptr)) {
      // Now have a NoClassDefFoundError as exception. Ignore in case the exception class was
      // removed by a pro-guard like tool.
      // Note: this is not RI behavior. RI would have failed when loading the class.
      self->ClearException();
      // Delete any long jump context as this routine is called during a stack walk which will
      // release its in use context at the end.
      delete self->GetLongJumpContext();
      LOG(WARNING) << "Unresolved exception class when finding catch block: "
        << DescriptorToDot(h_this->GetTypeDescriptorFromTypeIdx(iter_type_idx));
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
    self->SetExceptionReportedToInstrumentation(is_exception_reported);
  }
  return found_dex_pc;
}

void ArtMethod::AssertPcIsWithinQuickCode(uintptr_t pc) {
  if (IsNative() || IsRuntimeMethod() || IsProxyMethod()) {
    return;
  }
  if (pc == reinterpret_cast<uintptr_t>(GetQuickInstrumentationExitPc())) {
    return;
  }
  const void* code = GetEntryPointFromQuickCompiledCode();
  if (code == GetQuickInstrumentationEntryPoint()) {
    return;
  }
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  if (class_linker->IsQuickToInterpreterBridge(code) ||
      class_linker->IsQuickResolutionStub(code)) {
    return;
  }
  /*
   * During a stack walk, a return PC may point past-the-end of the code
   * in the case that the last instruction is a call that isn't expected to
   * return.  Thus, we check <= code + GetCodeSize().
   *
   * NOTE: For Thumb both pc and code are offset by 1 indicating the Thumb state.
   */
  CHECK(PcIsWithinQuickCode(pc))
      << PrettyMethod(this)
      << " pc=" << std::hex << pc
      << " code=" << code
      << " size=" << GetCodeSize();
}

bool ArtMethod::IsEntrypointInterpreter() {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  if (!IsPortableCompiled()) {  // Quick.
    const void* oat_quick_code = class_linker->GetOatMethodQuickCodeFor(this);
    return oat_quick_code == nullptr ||
        oat_quick_code != GetEntryPointFromQuickCompiledCode();
  } else {  // Portable.
    const void* oat_portable_code = class_linker->GetOatMethodPortableCodeFor(this);
    return oat_portable_code == nullptr ||
        oat_portable_code != GetEntryPointFromPortableCompiledCode();
  }
}

const void* ArtMethod::GetQuickOatEntryPoint() {
  if (IsPortableCompiled() || IsAbstract() || IsRuntimeMethod() || IsProxyMethod()) {
    return nullptr;
  }
  Runtime* runtime = Runtime::Current();
  ClassLinker* class_linker = runtime->GetClassLinker();
  const void* code = runtime->GetInstrumentation()->GetQuickCodeFor(this);
  // On failure, instead of nullptr we get the quick-generic-jni-trampoline for native method
  // indicating the generic JNI, or the quick-to-interpreter-bridge (but not the trampoline)
  // for non-native methods.
  if (class_linker->IsQuickToInterpreterBridge(code) ||
      class_linker->IsQuickGenericJniStub(code)) {
    return nullptr;
  }
  return code;
}

#ifndef NDEBUG
uintptr_t ArtMethod::NativeQuickPcOffset(const uintptr_t pc, const void* quick_entry_point) {
  CHECK_NE(quick_entry_point, GetQuickToInterpreterBridge());
  CHECK_EQ(quick_entry_point, Runtime::Current()->GetInstrumentation()->GetQuickCodeFor(this));
  return pc - reinterpret_cast<uintptr_t>(quick_entry_point);
}
#endif

void ArtMethod::Invoke(Thread* self, uint32_t* args, uint32_t args_size, JValue* result,
                       const char* shorty) {
  if (UNLIKELY(__builtin_frame_address(0) < self->GetStackEnd())) {
    ThrowStackOverflowError(self);
    return;
  }

  if (kIsDebugBuild) {
    self->AssertThreadSuspensionIsAllowable();
    CHECK_EQ(kRunnable, self->GetState());
    CHECK_STREQ(GetShorty(), shorty);
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

      // Ensure that we won't be accidentally calling quick/portable compiled code when -Xint.
      if (kIsDebugBuild && Runtime::Current()->GetInstrumentation()->IsForcedInterpretOnly()) {
        CHECK(IsEntrypointInterpreter())
            << "Don't call compiled code when -Xint " << PrettyMethod(this);
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
        self->SetTopOfStack(nullptr);
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

QuickMethodFrameInfo ArtMethod::GetQuickFrameInfo() {
  if (UNLIKELY(IsPortableCompiled())) {
    // Portable compiled dex bytecode or jni stub.
    return QuickMethodFrameInfo(kStackAlignment, 0u, 0u);
  }
  Runtime* runtime = Runtime::Current();
  // For Proxy method we exclude direct method (there is only one direct method - constructor).
  // Direct method is cloned from original java.lang.reflect.Proxy class together with code
  // and as a result it is executed as usual quick compiled method without any stubs.
  // So the frame info should be returned as it is a quick method not a stub.
  if (UNLIKELY(IsAbstract()) || UNLIKELY(IsProxyMethod() && !IsDirect())) {
    return runtime->GetCalleeSaveMethodFrameInfo(Runtime::kRefsAndArgs);
  }
  if (UNLIKELY(IsRuntimeMethod())) {
    return runtime->GetRuntimeMethodFrameInfo(this);
  }

  const void* entry_point = runtime->GetInstrumentation()->GetQuickCodeFor(this);
  ClassLinker* class_linker = runtime->GetClassLinker();
  // On failure, instead of nullptr we get the quick-generic-jni-trampoline for native method
  // indicating the generic JNI, or the quick-to-interpreter-bridge (but not the trampoline)
  // for non-native methods. And we really shouldn't see a failure for non-native methods here.
  DCHECK(!class_linker->IsQuickToInterpreterBridge(entry_point));

  if (class_linker->IsQuickGenericJniStub(entry_point)) {
    // Generic JNI frame.
    DCHECK(IsNative());
    StackHandleScope<1> hs(Thread::Current());
    uint32_t handle_refs =
        MethodHelper(hs.NewHandle(this)).GetNumberOfReferenceArgsWithoutReceiver() + 1;
    size_t scope_size = HandleScope::SizeOf(handle_refs);
    QuickMethodFrameInfo callee_info = runtime->GetCalleeSaveMethodFrameInfo(Runtime::kRefsAndArgs);

    // Callee saves + handle scope + method ref + alignment
    size_t frame_size = RoundUp(callee_info.FrameSizeInBytes() + scope_size
                                - sizeof(void*)  // callee-save frame stores a whole method pointer
                                + sizeof(StackReference<mirror::ArtMethod>),
                                kStackAlignment);

    return QuickMethodFrameInfo(frame_size, callee_info.CoreSpillMask(), callee_info.FpSpillMask());
  }

  const void* code_pointer = EntryPointToCodePointer(entry_point);
  return GetQuickFrameInfo(code_pointer);
}

void ArtMethod::RegisterNative(const void* native_method, bool is_fast) {
  CHECK(IsNative()) << PrettyMethod(this);
  CHECK(!IsFastNative()) << PrettyMethod(this);
  CHECK(native_method != NULL) << PrettyMethod(this);
  if (is_fast) {
    SetAccessFlags(GetAccessFlags() | kAccFastNative);
  }
  SetNativeMethod(native_method);
}

void ArtMethod::UnregisterNative() {
  CHECK(IsNative() && !IsFastNative()) << PrettyMethod(this);
  // restore stub to lookup native pointer via dlsym
  RegisterNative(GetJniDlsymLookupStub(), false);
}

}  // namespace mirror
}  // namespace art
