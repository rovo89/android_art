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

#include "greenland/runtime_entry_points.h"

#include "nth_caller_visitor.h"
#include "runtime_utils.h"
#include "runtime_support.h"

using namespace art;
using namespace art::greenland;

namespace {

int32_t art_find_catch_block(Method* current_method, uint32_t ti_offset) {
  Thread* thread = art_get_current_thread();
  Class* exception_type = thread->GetException()->GetClass();
  MethodHelper mh(current_method);
  const DexFile::CodeItem* code_item = mh.GetCodeItem();
  DCHECK_LT(ti_offset, code_item->tries_size_);
  const DexFile::TryItem* try_item = DexFile::GetTryItems(*code_item, ti_offset);

  int iter_index = 0;
  // Iterate over the catch handlers associated with dex_pc
  for (CatchHandlerIterator it(*code_item, *try_item); it.HasNext(); it.Next()) {
    uint16_t iter_type_idx = it.GetHandlerTypeIndex();
    // Catch all case
    if (iter_type_idx == DexFile::kDexNoIndex16) {
      return iter_index;
    }
    // Does this catch exception type apply?
    Class* iter_exception_type = mh.GetDexCacheResolvedType(iter_type_idx);
    if (iter_exception_type == NULL) {
      // The verifier should take care of resolving all exception classes early
      LOG(WARNING) << "Unresolved exception class when finding catch block: "
          << mh.GetTypeDescriptorFromTypeIdx(iter_type_idx);
    } else if (iter_exception_type->IsAssignableFrom(exception_type)) {
      return iter_index;
    }
    ++iter_index;
  }
  // Handler not found
  return -1;
}

void art_throw_array_bounds(int32_t length, int32_t index) {
  Thread* thread = art_get_current_thread();
  thread->ThrowNewExceptionF("Ljava/lang/ArrayIndexOutOfBoundsException;",
                             "length=%d; index=%d", length, index);
}

void art_throw_null_pointer_exception(uint32_t dex_pc) {
  Thread* thread = art_get_current_thread();
  NthCallerVisitor visitor(0);
  thread->WalkStack(&visitor);
  Method* throw_method = visitor.caller;
  ThrowNullPointerExceptionFromDexPC(thread, throw_method, dex_pc);
}

} // anonymous namespace

namespace art {
namespace greenland {

void InitExceptionRuntimes(RuntimeEntryPoints* entry_points) {
  entry_points->FindCatchBlock = art_find_catch_block;
  entry_points->ThrowIndexOutOfBounds = art_throw_array_bounds;
  entry_points->ThrowNullPointerException = art_throw_null_pointer_exception;
}

} // namespace greenland
} // namespace art
