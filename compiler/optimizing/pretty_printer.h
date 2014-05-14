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

  void PrintPreInstruction(HInstruction* instruction) {
    PrintString("  ");
    PrintInt(instruction->GetId());
    PrintString(": ");
  }

  virtual void VisitInstruction(HInstruction* instruction) {
    PrintPreInstruction(instruction);
    PrintString(instruction->DebugName());
    PrintPostInstruction(instruction);
  }

  void PrintPostInstruction(HInstruction* instruction) {
    if (instruction->InputCount() != 0) {
      PrintString("(");
      bool first = true;
      for (HInputIterator it(instruction); !it.Done(); it.Advance()) {
        if (first) {
          first = false;
        } else {
          PrintString(", ");
        }
        PrintInt(it.Current()->GetId());
      }
      PrintString(")");
    }
    if (instruction->HasUses()) {
      PrintString(" [");
      bool first = true;
      for (HUseIterator<HInstruction> it(instruction->GetUses()); !it.Done(); it.Advance()) {
        if (first) {
          first = false;
        } else {
          PrintString(", ");
        }
        PrintInt(it.Current()->GetUser()->GetId());
      }
      PrintString("]");
    }
    PrintNewLine();
  }

  virtual void VisitBasicBlock(HBasicBlock* block) {
    PrintString("BasicBlock ");
    PrintInt(block->GetBlockId());
    const GrowableArray<HBasicBlock*>& predecessors = block->GetPredecessors();
    if (!predecessors.IsEmpty()) {
      PrintString(", pred: ");
      for (size_t i = 0; i < predecessors.Size() -1; i++) {
        PrintInt(predecessors.Get(i)->GetBlockId());
        PrintString(", ");
      }
      PrintInt(predecessors.Peek()->GetBlockId());
    }
    const GrowableArray<HBasicBlock*>& successors = block->GetSuccessors();
    if (!successors.IsEmpty()) {
      PrintString(", succ: ");
      for (size_t i = 0; i < successors.Size() - 1; i++) {
        PrintInt(successors.Get(i)->GetBlockId());
        PrintString(", ");
      }
      PrintInt(successors.Peek()->GetBlockId());
    }
    PrintNewLine();
    HGraphVisitor::VisitBasicBlock(block);
  }

  virtual void PrintNewLine() = 0;
  virtual void PrintInt(int value) = 0;
  virtual void PrintString(const char* value) = 0;

 private:
  DISALLOW_COPY_AND_ASSIGN(HPrettyPrinter);
};

class StringPrettyPrinter : public HPrettyPrinter {
 public:
  explicit StringPrettyPrinter(HGraph* graph)
      : HPrettyPrinter(graph), str_(""), current_block_(nullptr) { }

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

  virtual void VisitBasicBlock(HBasicBlock* block) {
    current_block_ = block;
    HPrettyPrinter::VisitBasicBlock(block);
  }

  virtual void VisitGoto(HGoto* gota) {
    PrintString("  ");
    PrintInt(gota->GetId());
    PrintString(": Goto ");
    PrintInt(current_block_->GetSuccessors().Get(0)->GetBlockId());
    PrintNewLine();
  }

 private:
  std::string str_;
  HBasicBlock* current_block_;

  DISALLOW_COPY_AND_ASSIGN(StringPrettyPrinter);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_PRETTY_PRINTER_H_
