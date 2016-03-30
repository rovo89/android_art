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

#include "dex_cache_array_fixups_arm.h"

#include "base/arena_containers.h"
#include "utils/dex_cache_arrays_layout-inl.h"

namespace art {
namespace arm {

/**
 * Finds instructions that need the dex cache arrays base as an input.
 */
class DexCacheArrayFixupsVisitor : public HGraphVisitor {
 public:
  explicit DexCacheArrayFixupsVisitor(HGraph* graph)
      : HGraphVisitor(graph),
        dex_cache_array_bases_(std::less<const DexFile*>(),
                               // Attribute memory use to code generator.
                               graph->GetArena()->Adapter(kArenaAllocCodeGenerator)) {}

  void MoveBasesIfNeeded() {
    for (const auto& entry : dex_cache_array_bases_) {
      // Bring the base closer to the first use (previously, it was in the
      // entry block) and relieve some pressure on the register allocator
      // while avoiding recalculation of the base in a loop.
      HArmDexCacheArraysBase* base = entry.second;
      base->MoveBeforeFirstUserAndOutOfLoops();
    }
  }

 private:
  void VisitLoadString(HLoadString* load_string) OVERRIDE {
    // If this is a load with PC-relative access to the dex cache methods array,
    // we need to add the dex cache arrays base as the special input.
    if (load_string->GetLoadKind() == HLoadString::LoadKind::kDexCachePcRelative) {
      // Initialize base for target dex file if needed.
      const DexFile& dex_file = load_string->GetDexFile();
      HArmDexCacheArraysBase* base = GetOrCreateDexCacheArrayBase(dex_file);
      // Update the element offset in base.
      DexCacheArraysLayout layout(kArmPointerSize, &dex_file);
      base->UpdateElementOffset(layout.StringOffset(load_string->GetStringIndex()));
      // Add the special argument base to the load.
      load_string->AddSpecialInput(base);
    }
  }

  void VisitInvokeStaticOrDirect(HInvokeStaticOrDirect* invoke) OVERRIDE {
    // If this is an invoke with PC-relative access to the dex cache methods array,
    // we need to add the dex cache arrays base as the special input.
    if (invoke->HasPcRelativeDexCache()) {
      // Initialize base for target method dex file if needed.
      MethodReference target_method = invoke->GetTargetMethod();
      HArmDexCacheArraysBase* base = GetOrCreateDexCacheArrayBase(*target_method.dex_file);
      // Update the element offset in base.
      DexCacheArraysLayout layout(kArmPointerSize, target_method.dex_file);
      base->UpdateElementOffset(layout.MethodOffset(target_method.dex_method_index));
      // Add the special argument base to the method.
      DCHECK(!invoke->HasCurrentMethodInput());
      invoke->AddSpecialInput(base);
    }
  }

  HArmDexCacheArraysBase* GetOrCreateDexCacheArrayBase(const DexFile& dex_file) {
    // Ensure we only initialize the pointer once for each dex file.
    auto lb = dex_cache_array_bases_.lower_bound(&dex_file);
    if (lb != dex_cache_array_bases_.end() &&
        !dex_cache_array_bases_.key_comp()(&dex_file, lb->first)) {
      return lb->second;
    }

    // Insert the base at the start of the entry block, move it to a better
    // position later in MoveBaseIfNeeded().
    HArmDexCacheArraysBase* base = new (GetGraph()->GetArena()) HArmDexCacheArraysBase(dex_file);
    HBasicBlock* entry_block = GetGraph()->GetEntryBlock();
    entry_block->InsertInstructionBefore(base, entry_block->GetFirstInstruction());
    dex_cache_array_bases_.PutBefore(lb, &dex_file, base);
    return base;
  }

  using DexCacheArraysBaseMap =
      ArenaSafeMap<const DexFile*, HArmDexCacheArraysBase*, std::less<const DexFile*>>;
  DexCacheArraysBaseMap dex_cache_array_bases_;
};

void DexCacheArrayFixups::Run() {
  if (graph_->HasIrreducibleLoops()) {
    // Do not run this optimization, as irreducible loops do not work with an instruction
    // that can be live-in at the irreducible loop header.
    return;
  }
  DexCacheArrayFixupsVisitor visitor(graph_);
  visitor.VisitInsertionOrder();
  visitor.MoveBasesIfNeeded();
}

}  // namespace arm
}  // namespace art
