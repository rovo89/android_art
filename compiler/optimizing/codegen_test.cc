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

#include "builder.h"
#include "code_generator.h"
#include "common_compiler_test.h"
#include "dex_instruction.h"
#include "instruction_set.h"
#include "nodes.h"

#include "gtest/gtest.h"

namespace art {

class ExecutableMemoryAllocator : public CodeAllocator {
 public:
  ExecutableMemoryAllocator() { }

  virtual uint8_t* Allocate(size_t size) {
    memory_.reset(new uint8_t[size]);
    CommonCompilerTest::MakeExecutable(memory_.get(), size);
    return memory_.get();
  }

  uint8_t* memory() const { return memory_.get(); }

 private:
  UniquePtr<uint8_t[]> memory_;

  DISALLOW_COPY_AND_ASSIGN(ExecutableMemoryAllocator);
};

static void TestCode(const uint16_t* data, int length) {
  ArenaPool pool;
  ArenaAllocator arena(&pool);
  HGraphBuilder builder(&arena);
  HGraph* graph = builder.BuildGraph(data, data + length);
  ASSERT_NE(graph, nullptr);
  ExecutableMemoryAllocator allocator;
  CHECK(CodeGenerator::CompileGraph(graph, kX86, &allocator));
  typedef void (*fptr)();
#if defined(__i386__)
  reinterpret_cast<fptr>(allocator.memory())();
#endif
  CHECK(CodeGenerator::CompileGraph(graph, kArm, &allocator));
#if defined(__arm__)
  reinterpret_cast<fptr>(allocator.memory())();
#endif
}

TEST(CodegenTest, ReturnVoid) {
  const uint16_t data[] = { Instruction::RETURN_VOID };
  TestCode(data, sizeof(data) / sizeof(uint16_t));
}

TEST(PrettyPrinterTest, CFG1) {
  const uint16_t data[] = {
    Instruction::GOTO | 0x100,
    Instruction::RETURN_VOID
  };

  TestCode(data, sizeof(data) / sizeof(uint16_t));
}

TEST(PrettyPrinterTest, CFG2) {
  const uint16_t data[] = {
    Instruction::GOTO | 0x100,
    Instruction::GOTO | 0x100,
    Instruction::RETURN_VOID
  };

  TestCode(data, sizeof(data) / sizeof(uint16_t));
}

TEST(PrettyPrinterTest, CFG3) {
  const uint16_t data1[] = {
    Instruction::GOTO | 0x200,
    Instruction::RETURN_VOID,
    Instruction::GOTO | 0xFF00
  };

  TestCode(data1, sizeof(data1) / sizeof(uint16_t));

  const uint16_t data2[] = {
    Instruction::GOTO_16, 3,
    Instruction::RETURN_VOID,
    Instruction::GOTO_16, 0xFFFF
  };

  TestCode(data2, sizeof(data2) / sizeof(uint16_t));

  const uint16_t data3[] = {
    Instruction::GOTO_32, 4, 0,
    Instruction::RETURN_VOID,
    Instruction::GOTO_32, 0xFFFF, 0xFFFF
  };

  TestCode(data3, sizeof(data3) / sizeof(uint16_t));
}

TEST(PrettyPrinterTest, CFG4) {
  const uint16_t data[] = {
    Instruction::RETURN_VOID,
    Instruction::GOTO | 0x100,
    Instruction::GOTO | 0xFE00
  };

  TestCode(data, sizeof(data) / sizeof(uint16_t));
}

}  // namespace art
