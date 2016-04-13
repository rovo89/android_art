/*
 * Copyright (C) 2014 The Android Open Source Project
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

#ifndef ART_COMPILER_OPTIMIZING_OPTIMIZING_UNIT_TEST_H_
#define ART_COMPILER_OPTIMIZING_OPTIMIZING_UNIT_TEST_H_

#include "nodes.h"
#include "builder.h"
#include "common_compiler_test.h"
#include "dex_file.h"
#include "dex_instruction.h"
#include "handle_scope-inl.h"
#include "scoped_thread_state_change.h"
#include "ssa_builder.h"
#include "ssa_liveness_analysis.h"

#include "gtest/gtest.h"

namespace art {

#define NUM_INSTRUCTIONS(...)  \
  (sizeof((uint16_t[]) {__VA_ARGS__}) /sizeof(uint16_t))

#define N_REGISTERS_CODE_ITEM(NUM_REGS, ...)                            \
    { NUM_REGS, 0, 0, 0, 0, 0, NUM_INSTRUCTIONS(__VA_ARGS__), 0, __VA_ARGS__ }

#define ZERO_REGISTER_CODE_ITEM(...)   N_REGISTERS_CODE_ITEM(0, __VA_ARGS__)
#define ONE_REGISTER_CODE_ITEM(...)    N_REGISTERS_CODE_ITEM(1, __VA_ARGS__)
#define TWO_REGISTERS_CODE_ITEM(...)   N_REGISTERS_CODE_ITEM(2, __VA_ARGS__)
#define THREE_REGISTERS_CODE_ITEM(...) N_REGISTERS_CODE_ITEM(3, __VA_ARGS__)
#define FOUR_REGISTERS_CODE_ITEM(...)  N_REGISTERS_CODE_ITEM(4, __VA_ARGS__)
#define FIVE_REGISTERS_CODE_ITEM(...)  N_REGISTERS_CODE_ITEM(5, __VA_ARGS__)
#define SIX_REGISTERS_CODE_ITEM(...)   N_REGISTERS_CODE_ITEM(6, __VA_ARGS__)

LiveInterval* BuildInterval(const size_t ranges[][2],
                            size_t number_of_ranges,
                            ArenaAllocator* allocator,
                            int reg = -1,
                            HInstruction* defined_by = nullptr) {
  LiveInterval* interval = LiveInterval::MakeInterval(allocator, Primitive::kPrimInt, defined_by);
  if (defined_by != nullptr) {
    defined_by->SetLiveInterval(interval);
  }
  for (size_t i = number_of_ranges; i > 0; --i) {
    interval->AddRange(ranges[i - 1][0], ranges[i - 1][1]);
  }
  interval->SetRegister(reg);
  return interval;
}

void RemoveSuspendChecks(HGraph* graph) {
  for (HBasicBlock* block : graph->GetBlocks()) {
    if (block != nullptr) {
      for (HInstructionIterator it(block->GetInstructions()); !it.Done(); it.Advance()) {
        HInstruction* current = it.Current();
        if (current->IsSuspendCheck()) {
          current->GetBlock()->RemoveInstruction(current);
        }
      }
    }
  }
}

inline HGraph* CreateGraph(ArenaAllocator* allocator) {
  return new (allocator) HGraph(
      allocator, *reinterpret_cast<DexFile*>(allocator->Alloc(sizeof(DexFile))), -1, false,
      kRuntimeISA);
}

// Create a control-flow graph from Dex instructions.
inline HGraph* CreateCFG(ArenaAllocator* allocator,
                         const uint16_t* data,
                         Primitive::Type return_type = Primitive::kPrimInt) {
  const DexFile::CodeItem* item =
    reinterpret_cast<const DexFile::CodeItem*>(data);
  HGraph* graph = CreateGraph(allocator);

  {
    ScopedObjectAccess soa(Thread::Current());
    StackHandleScopeCollection handles(soa.Self());
    HGraphBuilder builder(graph, *item, &handles, return_type);
    bool graph_built = (builder.BuildGraph() == kAnalysisSuccess);
    return graph_built ? graph : nullptr;
  }
}

// Naive string diff data type.
typedef std::list<std::pair<std::string, std::string>> diff_t;

// An alias for the empty string used to make it clear that a line is
// removed in a diff.
static const std::string removed = "";

// Naive patch command: apply a diff to a string.
inline std::string Patch(const std::string& original, const diff_t& diff) {
  std::string result = original;
  for (const auto& p : diff) {
    std::string::size_type pos = result.find(p.first);
    DCHECK_NE(pos, std::string::npos)
        << "Could not find: \"" << p.first << "\" in \"" << result << "\"";
    result.replace(pos, p.first.size(), p.second);
  }
  return result;
}

// Returns if the instruction is removed from the graph.
inline bool IsRemoved(HInstruction* instruction) {
  return instruction->GetBlock() == nullptr;
}

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_OPTIMIZING_UNIT_TEST_H_
