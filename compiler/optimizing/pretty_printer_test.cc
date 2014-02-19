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

#include "base/stringprintf.h"
#include "builder.h"
#include "dex_instruction.h"
#include "nodes.h"
#include "pretty_printer.h"
#include "utils/arena_allocator.h"

#include "gtest/gtest.h"

namespace art {

const uint16_t data[] = { Instruction::RETURN_VOID };

const char* expected =
    "BasicBlock 0\n"
    "  Goto\n"
    "BasicBlock 1\n"
    "  ReturnVoid\n"
    "BasicBlock 2\n"
    "  Exit\n";

class StringPrettyPrinter : public HPrettyPrinter {
 public:
  explicit StringPrettyPrinter(HGraph* graph) : HPrettyPrinter(graph), str_("") { }

  virtual void PrintInt(int value) {
    str_ += StringPrintf("%d", value);
  }

  virtual void PrintString(const char* value) {
    str_ += value;
  }

  virtual void PrintNewLine() {
    str_ += '\n';
  }

  void Clear() { str_.clear(); }

  std::string str() const { return str_; }

 private:
  std::string str_;
  DISALLOW_COPY_AND_ASSIGN(StringPrettyPrinter);
};

TEST(OptimizerTest, ReturnVoid) {
  ArenaPool pool;
  ArenaAllocator allocator(&pool);
  HGraphBuilder builder(&allocator);
  HGraph* graph = builder.BuildGraph(data, data + 1);
  ASSERT_NE(graph, nullptr);
  StringPrettyPrinter printer(graph);
  printer.VisitInsertionOrder();
  ASSERT_STREQ(expected, printer.str().c_str());

  const GrowableArray<HBasicBlock*>* blocks = graph->blocks();
  ASSERT_EQ(blocks->Get(0)->predecessors()->Size(), (size_t)0);
  ASSERT_EQ(blocks->Get(1)->predecessors()->Size(), (size_t)1);
  ASSERT_EQ(blocks->Get(1)->predecessors()->Get(0), blocks->Get(0));
  ASSERT_EQ(blocks->Get(2)->predecessors()->Size(), (size_t)1);
  ASSERT_EQ(blocks->Get(2)->predecessors()->Get(0), blocks->Get(1));

  ASSERT_EQ(blocks->Get(0)->successors()->Size(), (size_t)1);
  ASSERT_EQ(blocks->Get(1)->successors()->Get(0), blocks->Get(2));
  ASSERT_EQ(blocks->Get(1)->successors()->Size(), (size_t)1);
  ASSERT_EQ(blocks->Get(1)->successors()->Get(0), blocks->Get(2));
  ASSERT_EQ(blocks->Get(2)->successors()->Size(), (size_t)0);
}

}  // namespace art
