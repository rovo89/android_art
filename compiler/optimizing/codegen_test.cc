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
#include "dex_file.h"
#include "dex_instruction.h"
#include "instruction_set.h"
#include "nodes.h"
#include "optimizing_unit_test.h"

#include "gtest/gtest.h"

namespace art {

class InternalCodeAllocator : public CodeAllocator {
 public:
  InternalCodeAllocator() { }

  virtual uint8_t* Allocate(size_t size) {
    size_ = size;
    memory_.reset(new uint8_t[size]);
    return memory_.get();
  }

  size_t GetSize() const { return size_; }
  uint8_t* GetMemory() const { return memory_.get(); }

 private:
  size_t size_;
  UniquePtr<uint8_t[]> memory_;

  DISALLOW_COPY_AND_ASSIGN(InternalCodeAllocator);
};

static void TestCode(const uint16_t* data, bool has_result = false, int32_t expected = 0) {
  ArenaPool pool;
  ArenaAllocator arena(&pool);
  HGraphBuilder builder(&arena);
  const DexFile::CodeItem* item = reinterpret_cast<const DexFile::CodeItem*>(data);
  HGraph* graph = builder.BuildGraph(*item);
  ASSERT_NE(graph, nullptr);
  InternalCodeAllocator allocator;
  CodeGenerator* codegen = CodeGenerator::Create(&arena, graph, kX86);
  codegen->Compile(&allocator);
  typedef int32_t (*fptr)();
#if defined(__i386__)
  CommonCompilerTest::MakeExecutable(allocator.GetMemory(), allocator.GetSize());
  int32_t result = reinterpret_cast<fptr>(allocator.GetMemory())();
  if (has_result) {
    CHECK_EQ(result, expected);
  }
#endif
  codegen = CodeGenerator::Create(&arena, graph, kArm);
  codegen->Compile(&allocator);
#if defined(__arm__)
  CommonCompilerTest::MakeExecutable(allocator.GetMemory(), allocator.GetSize());
  int32_t result = reinterpret_cast<fptr>(allocator.GetMemory())();
  if (has_result) {
    CHECK_EQ(result, expected);
  }
#endif
}

TEST(CodegenTest, ReturnVoid) {
  const uint16_t data[] = ZERO_REGISTER_CODE_ITEM(Instruction::RETURN_VOID);
  TestCode(data);
}

TEST(CodegenTest, CFG1) {
  const uint16_t data[] = ZERO_REGISTER_CODE_ITEM(
    Instruction::GOTO | 0x100,
    Instruction::RETURN_VOID);

  TestCode(data);
}

TEST(CodegenTest, CFG2) {
  const uint16_t data[] = ZERO_REGISTER_CODE_ITEM(
    Instruction::GOTO | 0x100,
    Instruction::GOTO | 0x100,
    Instruction::RETURN_VOID);

  TestCode(data);
}

TEST(CodegenTest, CFG3) {
  const uint16_t data1[] = ZERO_REGISTER_CODE_ITEM(
    Instruction::GOTO | 0x200,
    Instruction::RETURN_VOID,
    Instruction::GOTO | 0xFF00);

  TestCode(data1);

  const uint16_t data2[] = ZERO_REGISTER_CODE_ITEM(
    Instruction::GOTO_16, 3,
    Instruction::RETURN_VOID,
    Instruction::GOTO_16, 0xFFFF);

  TestCode(data2);

  const uint16_t data3[] = ZERO_REGISTER_CODE_ITEM(
    Instruction::GOTO_32, 4, 0,
    Instruction::RETURN_VOID,
    Instruction::GOTO_32, 0xFFFF, 0xFFFF);

  TestCode(data3);
}

TEST(CodegenTest, CFG4) {
  const uint16_t data[] = ZERO_REGISTER_CODE_ITEM(
    Instruction::RETURN_VOID,
    Instruction::GOTO | 0x100,
    Instruction::GOTO | 0xFE00);

  TestCode(data);
}

TEST(CodegenTest, CFG5) {
  const uint16_t data[] = ONE_REGISTER_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::IF_EQ, 3,
    Instruction::GOTO | 0x100,
    Instruction::RETURN_VOID);

  TestCode(data);
}

TEST(CodegenTest, IntConstant) {
  const uint16_t data[] = ONE_REGISTER_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::RETURN_VOID);

  TestCode(data);
}

TEST(CodegenTest, Return1) {
  const uint16_t data[] = ONE_REGISTER_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::RETURN | 0);

  TestCode(data, true, 0);
}

TEST(CodegenTest, Return2) {
  const uint16_t data[] = TWO_REGISTERS_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::CONST_4 | 0 | 1 << 8,
    Instruction::RETURN | 1 << 8);

  TestCode(data, true, 0);
}

TEST(CodegenTest, Return3) {
  const uint16_t data[] = TWO_REGISTERS_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::CONST_4 | 1 << 8 | 1 << 12,
    Instruction::RETURN | 1 << 8);

  TestCode(data, true, 1);
}

TEST(CodegenTest, ReturnIf1) {
  const uint16_t data[] = TWO_REGISTERS_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::CONST_4 | 1 << 8 | 1 << 12,
    Instruction::IF_EQ, 3,
    Instruction::RETURN | 0 << 8,
    Instruction::RETURN | 1 << 8);

  TestCode(data, true, 1);
}

TEST(CodegenTest, ReturnIf2) {
  const uint16_t data[] = TWO_REGISTERS_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::CONST_4 | 1 << 8 | 1 << 12,
    Instruction::IF_EQ | 0 << 4 | 1 << 8, 3,
    Instruction::RETURN | 0 << 8,
    Instruction::RETURN | 1 << 8);

  TestCode(data, true, 0);
}

TEST(CodegenTest, ReturnAdd1) {
  const uint16_t data[] = TWO_REGISTERS_CODE_ITEM(
    Instruction::CONST_4 | 3 << 12 | 0,
    Instruction::CONST_4 | 4 << 12 | 1 << 8,
    Instruction::ADD_INT, 1 << 8 | 0,
    Instruction::RETURN);

  TestCode(data, true, 7);
}

TEST(CodegenTest, ReturnAdd2) {
  const uint16_t data[] = TWO_REGISTERS_CODE_ITEM(
    Instruction::CONST_4 | 3 << 12 | 0,
    Instruction::CONST_4 | 4 << 12 | 1 << 8,
    Instruction::ADD_INT_2ADDR | 1 << 12,
    Instruction::RETURN);

  TestCode(data, true, 7);
}

TEST(CodegenTest, ReturnAdd3) {
  const uint16_t data[] = ONE_REGISTER_CODE_ITEM(
    Instruction::CONST_4 | 4 << 12 | 0 << 8,
    Instruction::ADD_INT_LIT8, 3 << 8 | 0,
    Instruction::RETURN);

  TestCode(data, true, 7);
}

TEST(CodegenTest, ReturnAdd4) {
  const uint16_t data[] = ONE_REGISTER_CODE_ITEM(
    Instruction::CONST_4 | 4 << 12 | 0 << 8,
    Instruction::ADD_INT_LIT16, 3,
    Instruction::RETURN);

  TestCode(data, true, 7);
}

}  // namespace art
