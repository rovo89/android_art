/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include "art_code.h"

#include "art_method.h"
#include "art_method-inl.h"
#include "class_linker.h"
#include "entrypoints/runtime_asm_entrypoints.h"
#include "handle_scope.h"
#include "jit/jit.h"
#include "jit/jit_code_cache.h"
#include "mapping_table.h"
#include "oat.h"
#include "runtime.h"
#include "utils.h"

namespace art {

  // Converts a dex PC to a native PC.
uintptr_t ArtCode::ToNativeQuickPc(const uint32_t dex_pc,
                                   bool is_for_catch_handler,
                                   bool abort_on_failure)
      SHARED_REQUIRES(Locks::mutator_lock_) {
  const void* entry_point = GetQuickOatEntryPoint(sizeof(void*));
  if (IsOptimized(sizeof(void*))) {
    // Optimized code does not have a mapping table. Search for the dex-to-pc
    // mapping in stack maps.
    CodeInfo code_info = GetOptimizedCodeInfo();
    StackMapEncoding encoding = code_info.ExtractEncoding();

    // All stack maps are stored in the same CodeItem section, safepoint stack
    // maps first, then catch stack maps. We use `is_for_catch_handler` to select
    // the order of iteration.
    StackMap stack_map =
        LIKELY(is_for_catch_handler) ? code_info.GetCatchStackMapForDexPc(dex_pc, encoding)
                                     : code_info.GetStackMapForDexPc(dex_pc, encoding);
    if (stack_map.IsValid()) {
      return reinterpret_cast<uintptr_t>(entry_point) + stack_map.GetNativePcOffset(encoding);
    }
  } else {
    MappingTable table((entry_point != nullptr) ? GetMappingTable(sizeof(void*)) : nullptr);
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
  }

  if (abort_on_failure) {
    LOG(FATAL) << "Failed to find native offset for dex pc 0x" << std::hex << dex_pc
               << " in " << PrettyMethod(method_);
  }
  return UINTPTR_MAX;
}

bool ArtCode::IsOptimized(size_t pointer_size) SHARED_REQUIRES(Locks::mutator_lock_) {
  // Temporary solution for detecting if a method has been optimized: the compiler
  // does not create a GC map. Instead, the vmap table contains the stack map
  // (as in stack_map.h).
  return !method_->IsNative()
      && method_->GetEntryPointFromQuickCompiledCodePtrSize(pointer_size) != nullptr
      && GetQuickOatEntryPoint(pointer_size) != nullptr
      && GetNativeGcMap(pointer_size) == nullptr;
}

CodeInfo ArtCode::GetOptimizedCodeInfo() {
  DCHECK(IsOptimized(sizeof(void*)));
  const void* code_pointer = EntryPointToCodePointer(GetQuickOatEntryPoint(sizeof(void*)));
  DCHECK(code_pointer != nullptr);
  uint32_t offset =
      reinterpret_cast<const OatQuickMethodHeader*>(code_pointer)[-1].vmap_table_offset_;
  const void* data =
      reinterpret_cast<const void*>(reinterpret_cast<const uint8_t*>(code_pointer) - offset);
  return CodeInfo(data);
}

uintptr_t ArtCode::NativeQuickPcOffset(const uintptr_t pc) {
  const void* quick_entry_point = GetQuickOatEntryPoint(sizeof(void*));
  CHECK_NE(quick_entry_point, GetQuickToInterpreterBridge());
  CHECK_EQ(quick_entry_point,
           Runtime::Current()->GetInstrumentation()->GetQuickCodeFor(method_, sizeof(void*)));
  return pc - reinterpret_cast<uintptr_t>(quick_entry_point);
}

uint32_t ArtCode::ToDexPc(const uintptr_t pc, bool abort_on_failure) {
  const void* entry_point = GetQuickOatEntryPoint(sizeof(void*));
  uint32_t sought_offset = pc - reinterpret_cast<uintptr_t>(entry_point);
  if (IsOptimized(sizeof(void*))) {
    CodeInfo code_info = GetOptimizedCodeInfo();
    StackMapEncoding encoding = code_info.ExtractEncoding();
    StackMap stack_map = code_info.GetStackMapForNativePcOffset(sought_offset, encoding);
    if (stack_map.IsValid()) {
      return stack_map.GetDexPc(encoding);
    }
  } else {
    MappingTable table(entry_point != nullptr ? GetMappingTable(sizeof(void*)) : nullptr);
    if (table.TotalSize() == 0) {
      // NOTE: Special methods (see Mir2Lir::GenSpecialCase()) have an empty mapping
      // but they have no suspend checks and, consequently, we never call ToDexPc() for them.
      DCHECK(method_->IsNative() || method_->IsCalleeSaveMethod() || method_->IsProxyMethod())
          << PrettyMethod(method_);
      return DexFile::kDexNoIndex;   // Special no mapping case
    }
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
  }
  if (abort_on_failure) {
      LOG(FATAL) << "Failed to find Dex offset for PC offset " << reinterpret_cast<void*>(sought_offset)
             << "(PC " << reinterpret_cast<void*>(pc) << ", entry_point=" << entry_point
             << " current entry_point=" << GetQuickOatEntryPoint(sizeof(void*))
             << ") in " << PrettyMethod(method_);
  }
  return DexFile::kDexNoIndex;
}

const uint8_t* ArtCode::GetNativeGcMap(size_t pointer_size) {
  const void* code_pointer = EntryPointToCodePointer(GetQuickOatEntryPoint(pointer_size));
  if (code_pointer == nullptr) {
    return nullptr;
  }
  uint32_t offset =
      reinterpret_cast<const OatQuickMethodHeader*>(code_pointer)[-1].gc_map_offset_;
  if (UNLIKELY(offset == 0u)) {
    return nullptr;
  }
  return reinterpret_cast<const uint8_t*>(code_pointer) - offset;
}

const uint8_t* ArtCode::GetVmapTable(size_t pointer_size) {
  CHECK(!IsOptimized(pointer_size)) << "Unimplemented vmap table for optimized compiler";
  const void* code_pointer = EntryPointToCodePointer(GetQuickOatEntryPoint(pointer_size));
  if (code_pointer == nullptr) {
    return nullptr;
  }
  uint32_t offset =
      reinterpret_cast<const OatQuickMethodHeader*>(code_pointer)[-1].vmap_table_offset_;
  if (UNLIKELY(offset == 0u)) {
    return nullptr;
  }
  return reinterpret_cast<const uint8_t*>(code_pointer) - offset;
}

const uint8_t* ArtCode::GetMappingTable(size_t pointer_size) {
  const void* code_pointer = EntryPointToCodePointer(GetQuickOatEntryPoint(pointer_size));
  if (code_pointer == nullptr) {
    return nullptr;
  }
  uint32_t offset =
      reinterpret_cast<const OatQuickMethodHeader*>(code_pointer)[-1].mapping_table_offset_;
  if (UNLIKELY(offset == 0u)) {
    return nullptr;
  }
  return reinterpret_cast<const uint8_t*>(code_pointer) - offset;
}

// Counts the number of references in the parameter list of the corresponding method.
// Note: Thus does _not_ include "this" for non-static methods.
static uint32_t GetNumberOfReferenceArgsWithoutReceiver(ArtMethod* method)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  uint32_t shorty_len;
  const char* shorty = method->GetShorty(&shorty_len);
  uint32_t refs = 0;
  for (uint32_t i = 1; i < shorty_len ; ++i) {
    if (shorty[i] == 'L') {
      refs++;
    }
  }
  return refs;
}

QuickMethodFrameInfo ArtCode::GetQuickFrameInfo() {
  Runtime* runtime = Runtime::Current();

  if (UNLIKELY(method_->IsAbstract())) {
    return runtime->GetCalleeSaveMethodFrameInfo(Runtime::kRefsAndArgs);
  }

  // This goes before IsProxyMethod since runtime methods have a null declaring class.
  if (UNLIKELY(method_->IsRuntimeMethod())) {
    return runtime->GetRuntimeMethodFrameInfo(method_);
  }

  // For Proxy method we add special handling for the direct method case  (there is only one
  // direct method - constructor). Direct method is cloned from original
  // java.lang.reflect.Proxy class together with code and as a result it is executed as usual
  // quick compiled method without any stubs. So the frame info should be returned as it is a
  // quick method not a stub. However, if instrumentation stubs are installed, the
  // instrumentation->GetQuickCodeFor() returns the artQuickProxyInvokeHandler instead of an
  // oat code pointer, thus we have to add a special case here.
  if (UNLIKELY(method_->IsProxyMethod())) {
    if (method_->IsDirect()) {
      CHECK(method_->IsConstructor());
      const void* code_pointer =
          EntryPointToCodePointer(method_->GetEntryPointFromQuickCompiledCode());
      return reinterpret_cast<const OatQuickMethodHeader*>(code_pointer)[-1].frame_info_;
    } else {
      return runtime->GetCalleeSaveMethodFrameInfo(Runtime::kRefsAndArgs);
    }
  }

  const void* entry_point = runtime->GetInstrumentation()->GetQuickCodeFor(method_, sizeof(void*));
  ClassLinker* class_linker = runtime->GetClassLinker();
  // On failure, instead of null we get the quick-generic-jni-trampoline for native method
  // indicating the generic JNI, or the quick-to-interpreter-bridge (but not the trampoline)
  // for non-native methods. And we really shouldn't see a failure for non-native methods here.
  DCHECK(!class_linker->IsQuickToInterpreterBridge(entry_point));

  if (class_linker->IsQuickGenericJniStub(entry_point)) {
    // Generic JNI frame.
    DCHECK(method_->IsNative());
    uint32_t handle_refs = GetNumberOfReferenceArgsWithoutReceiver(method_) + 1;
    size_t scope_size = HandleScope::SizeOf(handle_refs);
    QuickMethodFrameInfo callee_info = runtime->GetCalleeSaveMethodFrameInfo(Runtime::kRefsAndArgs);

    // Callee saves + handle scope + method ref + alignment
    // Note: -sizeof(void*) since callee-save frame stores a whole method pointer.
    size_t frame_size = RoundUp(callee_info.FrameSizeInBytes() - sizeof(void*) +
                                sizeof(ArtMethod*) + scope_size, kStackAlignment);
    return QuickMethodFrameInfo(frame_size, callee_info.CoreSpillMask(), callee_info.FpSpillMask());
  }

  const void* code_pointer = EntryPointToCodePointer(entry_point);
  return reinterpret_cast<const OatQuickMethodHeader*>(code_pointer)[-1].frame_info_;
}

void ArtCode::AssertPcIsWithinQuickCode(uintptr_t pc) {
  if (method_->IsNative() || method_->IsRuntimeMethod() || method_->IsProxyMethod()) {
    return;
  }
  if (pc == reinterpret_cast<uintptr_t>(GetQuickInstrumentationExitPc())) {
    return;
  }
  const void* code = method_->GetEntryPointFromQuickCompiledCode();
  if (code == GetQuickInstrumentationEntryPoint()) {
    return;
  }
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  if (class_linker->IsQuickToInterpreterBridge(code) ||
      class_linker->IsQuickResolutionStub(code)) {
    return;
  }
  // If we are the JIT then we may have just compiled the method after the
  // IsQuickToInterpreterBridge check.
  jit::Jit* const jit = Runtime::Current()->GetJit();
  if (jit != nullptr &&
      jit->GetCodeCache()->ContainsCodePtr(reinterpret_cast<const void*>(code))) {
    return;
  }

  uint32_t code_size = reinterpret_cast<const OatQuickMethodHeader*>(code)[-1].code_size_;
  CHECK(PcIsWithinQuickCode(pc))
      << PrettyMethod(method_)
      << " pc=" << std::hex << pc
      << " code=" << code
      << " size=" << code_size;
}

bool ArtCode::PcIsWithinQuickCode(uintptr_t pc) {
  /*
   * During a stack walk, a return PC may point past-the-end of the code
   * in the case that the last instruction is a call that isn't expected to
   * return.  Thus, we check <= code + GetCodeSize().
   *
   * NOTE: For Thumb both pc and code are offset by 1 indicating the Thumb state.
   */
  uintptr_t code = reinterpret_cast<uintptr_t>(EntryPointToCodePointer(
      method_->GetEntryPointFromQuickCompiledCode()));
  if (code == 0) {
    return pc == 0;
  }
  uintptr_t code_size = reinterpret_cast<const OatQuickMethodHeader*>(code)[-1].code_size_;
  return code <= pc && pc <= (code + code_size);
}

const void* ArtCode::GetQuickOatEntryPoint(size_t pointer_size) {
  if (method_->IsAbstract() || method_->IsRuntimeMethod() || method_->IsProxyMethod()) {
    return nullptr;
  }
  Runtime* runtime = Runtime::Current();
  ClassLinker* class_linker = runtime->GetClassLinker();
  const void* code = runtime->GetInstrumentation()->GetQuickCodeFor(method_, pointer_size);
  // On failure, instead of null we get the quick-generic-jni-trampoline for native method
  // indicating the generic JNI, or the quick-to-interpreter-bridge (but not the trampoline)
  // for non-native methods.
  if (class_linker->IsQuickToInterpreterBridge(code) ||
      class_linker->IsQuickGenericJniStub(code)) {
    return nullptr;
  }
  return code;
}

}  // namespace art
