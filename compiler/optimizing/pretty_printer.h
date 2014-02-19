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

#ifndef ART_COMPILER_OPTIMIZING_PRETTY_PRINTER_H_
#define ART_COMPILER_OPTIMIZING_PRETTY_PRINTER_H_

#include "nodes.h"

namespace art {

class HPrettyPrinter : public HGraphVisitor {
 public:
  explicit HPrettyPrinter(HGraph* graph) : HGraphVisitor(graph) { }

  virtual void VisitInstruction(HInstruction* instruction) {
    PrintString("  ");
    PrintString(instruction->DebugName());
    PrintNewLine();
  }

  virtual void VisitBasicBlock(HBasicBlock* block) {
    PrintString("BasicBlock ");
    PrintInt(block->block_id());
    PrintNewLine();
    HGraphVisitor::VisitBasicBlock(block);
  }

  virtual void PrintNewLine() = 0;
  virtual void PrintInt(int value) = 0;
  virtual void PrintString(const char* value) = 0;

 private:
  DISALLOW_COPY_AND_ASSIGN(HPrettyPrinter);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_PRETTY_PRINTER_H_
