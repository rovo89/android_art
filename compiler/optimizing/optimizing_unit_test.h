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
#include "dex_file.h"
#include "dex_instruction.h"
#include "ssa_liveness_analysis.h"

#include "gtest/gtest.h"

namespace art {

#define NUM_INSTRUCTIONS(...)  \
  (sizeof((uint16_t[]) {__VA_ARGS__}) /sizeof(uint16_t))

#define ZERO_REGISTER_CODE_ITEM(...)                                       \
    { 0, 0, 0, 0, 0, 0, NUM_INSTRUCTIONS(__VA_ARGS__), 0, __VA_ARGS__ }

#define ONE_REGISTER_CODE_ITEM(...)                                        \
    { 1, 0, 0, 0, 0, 0, NUM_INSTRUCTIONS(__VA_ARGS__), 0, __VA_ARGS__ }

#define TWO_REGISTERS_CODE_ITEM(...)                                       \
    { 2, 0, 0, 0, 0, 0, NUM_INSTRUCTIONS(__VA_ARGS__), 0, __VA_ARGS__ }

#define THREE_REGISTERS_CODE_ITEM(...)                                     \
    { 3, 0, 0, 0, 0, 0, NUM_INSTRUCTIONS(__VA_ARGS__), 0, __VA_ARGS__ }

LiveInterval* BuildInterval(const size_t ranges[][2],
                            size_t number_of_ranges,
                            ArenaAllocator* allocator,
                            int reg = -1) {
  LiveInterval* interval = new (allocator) LiveInterval(allocator, Primitive::kPrimInt);
  for (size_t i = number_of_ranges; i > 0; --i) {
    interval->AddRange(ranges[i - 1][0], ranges[i - 1][1]);
  }
  interval->SetRegister(reg);
  return interval;
}

void RemoveSuspendChecks(HGraph* graph) {
  for (size_t i = 0, e = graph->GetBlocks().Size(); i < e; ++i) {
    for (HInstructionIterator it(graph->GetBlocks().Get(i)->GetInstructions());
         !it.Done();
         it.Advance()) {
      HInstruction* current = it.Current();
      if (current->IsSuspendCheck()) {
        current->GetBlock()->RemoveInstruction(current);
      }
    }
  }
}

// Create a control-flow graph from Dex instructions.
inline HGraph* CreateCFG(ArenaAllocator* allocator,
                         const uint16_t* data,
                         Primitive::Type return_type = Primitive::kPrimInt) {
  HGraphBuilder builder(allocator, return_type);
  const DexFile::CodeItem* item =
    reinterpret_cast<const DexFile::CodeItem*>(data);
  HGraph* graph = builder.BuildGraph(*item);
  return graph;
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
    EXPECT_NE(pos, std::string::npos);
    result.replace(pos, p.first.size(), p.second);
  }
  return result;
}

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_OPTIMIZING_UNIT_TEST_H_
