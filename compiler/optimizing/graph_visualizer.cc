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

#include "graph_visualizer.h"

#include "code_generator.h"
#include "driver/dex_compilation_unit.h"
#include "nodes.h"
#include "ssa_liveness_analysis.h"

namespace art {

/**
 * HGraph visitor to generate a file suitable for the c1visualizer tool and IRHydra.
 */
class HGraphVisualizerPrinter : public HGraphVisitor {
 public:
  HGraphVisualizerPrinter(HGraph* graph,
                          std::ostream& output,
                          const char* pass_name,
                          const CodeGenerator& codegen)
      : HGraphVisitor(graph),
        output_(output),
        pass_name_(pass_name),
        codegen_(codegen),
        indent_(0) {}

  void StartTag(const char* name) {
    AddIndent();
    output_ << "begin_" << name << std::endl;
    indent_++;
  }

  void EndTag(const char* name) {
    indent_--;
    AddIndent();
    output_ << "end_" << name << std::endl;
  }

  void PrintProperty(const char* name, const char* property) {
    AddIndent();
    output_ << name << " \"" << property << "\"" << std::endl;
  }

  void PrintProperty(const char* name, const char* property, int id) {
    AddIndent();
    output_ << name << " \"" << property << id << "\"" << std::endl;
  }

  void PrintEmptyProperty(const char* name) {
    AddIndent();
    output_ << name << std::endl;
  }

  void PrintTime(const char* name) {
    AddIndent();
    output_ << name << " " << time(NULL) << std::endl;
  }

  void PrintInt(const char* name, int value) {
    AddIndent();
    output_ << name << " " << value << std::endl;
  }

  void AddIndent() {
    for (size_t i = 0; i < indent_; ++i) {
      output_ << "  ";
    }
  }

  void PrintPredecessors(HBasicBlock* block) {
    AddIndent();
    output_ << "predecessors";
    for (size_t i = 0, e = block->GetPredecessors().Size(); i < e; ++i) {
      HBasicBlock* predecessor = block->GetPredecessors().Get(i);
      output_ << " \"B" << predecessor->GetBlockId() << "\" ";
    }
    output_<< std::endl;
  }

  void PrintSuccessors(HBasicBlock* block) {
    AddIndent();
    output_ << "successors";
    for (size_t i = 0, e = block->GetSuccessors().Size(); i < e; ++i) {
      HBasicBlock* successor = block->GetSuccessors().Get(i);
      output_ << " \"B" << successor->GetBlockId() << "\" ";
    }
    output_<< std::endl;
  }

  void DumpLocation(Location location, Primitive::Type type) {
    if (location.IsRegister()) {
      if (type == Primitive::kPrimDouble || type == Primitive::kPrimFloat) {
        codegen_.DumpFloatingPointRegister(output_, location.reg().RegId());
      } else {
        codegen_.DumpCoreRegister(output_, location.reg().RegId());
      }
    } else if (location.IsConstant()) {
      output_ << "constant";
    } else if (location.IsInvalid()) {
      output_ << "invalid";
    } else if (location.IsStackSlot()) {
      output_ << location.GetStackIndex() << "(sp)";
    } else {
      DCHECK(location.IsDoubleStackSlot());
      output_ << "2x" << location.GetStackIndex() << "(sp)";
    }
  }

  void VisitParallelMove(HParallelMove* instruction) {
    output_ << instruction->DebugName();
    output_ << " (";
    for (size_t i = 0, e = instruction->NumMoves(); i < e; ++i) {
      MoveOperands* move = instruction->MoveOperandsAt(i);
      DumpLocation(move->GetSource(), Primitive::kPrimInt);
      output_ << " -> ";
      DumpLocation(move->GetDestination(), Primitive::kPrimInt);
      if (i + 1 != e) {
        output_ << ", ";
      }
    }
    output_ << ")";
  }

  void VisitInstruction(HInstruction* instruction) {
    output_ << instruction->DebugName();
    if (instruction->InputCount() > 0) {
      output_ << " [ ";
      for (HInputIterator inputs(instruction); !inputs.Done(); inputs.Advance()) {
        output_ << "v" << inputs.Current()->GetId() << " ";
      }
      output_ << "]";
    }
    if (pass_name_ == kLivenessPassName && instruction->GetLifetimePosition() != kNoLifetime) {
      output_ << " (liveness: " << instruction->GetLifetimePosition();
      if (instruction->HasLiveInterval()) {
        output_ << " ";
        const LiveInterval& interval = *instruction->GetLiveInterval();
        interval.Dump(output_);
      }
      output_ << ")";
    } else if (pass_name_ == kRegisterAllocatorPassName) {
      LocationSummary* locations = instruction->GetLocations();
      if (locations != nullptr) {
        output_ << " ( ";
        for (size_t i = 0; i < instruction->InputCount(); ++i) {
          DumpLocation(locations->InAt(i), instruction->InputAt(i)->GetType());
          output_ << " ";
        }
        output_ << ")";
        if (locations->Out().IsValid()) {
          output_ << " -> ";
          DumpLocation(locations->Out(), instruction->GetType());
        }
      }
    }
  }

  void PrintInstructions(const HInstructionList& list) {
    const char* kEndInstructionMarker = "<|@";
    for (HInstructionIterator it(list); !it.Done(); it.Advance()) {
      HInstruction* instruction = it.Current();
      AddIndent();
      int bci = 0;
      output_ << bci << " " << instruction->NumberOfUses() << " v" << instruction->GetId() << " ";
      instruction->Accept(this);
      output_ << kEndInstructionMarker << std::endl;
    }
  }

  void Run() {
    StartTag("cfg");
    PrintProperty("name", pass_name_);
    VisitInsertionOrder();
    EndTag("cfg");
  }

  void VisitBasicBlock(HBasicBlock* block) {
    StartTag("block");
    PrintProperty("name", "B", block->GetBlockId());
    if (block->GetLifetimeStart() != kNoLifetime) {
      // Piggy back on these fields to show the lifetime of the block.
      PrintInt("from_bci", block->GetLifetimeStart());
      PrintInt("to_bci", block->GetLifetimeEnd());
    } else {
      PrintInt("from_bci", -1);
      PrintInt("to_bci", -1);
    }
    PrintPredecessors(block);
    PrintSuccessors(block);
    PrintEmptyProperty("xhandlers");
    PrintEmptyProperty("flags");
    if (block->GetDominator() != nullptr) {
      PrintProperty("dominator", "B", block->GetDominator()->GetBlockId());
    }

    StartTag("states");
    StartTag("locals");
    PrintInt("size", 0);
    PrintProperty("method", "None");
    for (HInstructionIterator it(block->GetPhis()); !it.Done(); it.Advance()) {
      AddIndent();
      HInstruction* instruction = it.Current();
      output_ << instruction->GetId() << " v" << instruction->GetId() << "[ ";
      for (HInputIterator inputs(instruction); !inputs.Done(); inputs.Advance()) {
        output_ << inputs.Current()->GetId() << " ";
      }
      output_ << "]" << std::endl;
    }
    EndTag("locals");
    EndTag("states");

    StartTag("HIR");
    PrintInstructions(block->GetPhis());
    PrintInstructions(block->GetInstructions());
    EndTag("HIR");
    EndTag("block");
  }

 private:
  std::ostream& output_;
  const char* pass_name_;
  const CodeGenerator& codegen_;
  size_t indent_;

  DISALLOW_COPY_AND_ASSIGN(HGraphVisualizerPrinter);
};

HGraphVisualizer::HGraphVisualizer(std::ostream* output,
                                   HGraph* graph,
                                   const char* string_filter,
                                   const CodeGenerator& codegen,
                                   const DexCompilationUnit& cu)
    : output_(output), graph_(graph), codegen_(codegen), is_enabled_(false) {
  if (output == nullptr) {
    return;
  }
  std::string pretty_name = PrettyMethod(cu.GetDexMethodIndex(), *cu.GetDexFile());
  if (pretty_name.find(string_filter) == std::string::npos) {
    return;
  }

  is_enabled_ = true;
  HGraphVisualizerPrinter printer(graph, *output_, "", codegen_);
  printer.StartTag("compilation");
  printer.PrintProperty("name", pretty_name.c_str());
  printer.PrintProperty("method", pretty_name.c_str());
  printer.PrintTime("date");
  printer.EndTag("compilation");
}

HGraphVisualizer::HGraphVisualizer(std::ostream* output,
                                   HGraph* graph,
                                   const CodeGenerator& codegen,
                                   const char* name)
    : output_(output), graph_(graph), codegen_(codegen), is_enabled_(false) {
  if (output == nullptr) {
    return;
  }

  is_enabled_ = true;
  HGraphVisualizerPrinter printer(graph, *output_, "", codegen_);
  printer.StartTag("compilation");
  printer.PrintProperty("name", name);
  printer.PrintProperty("method", name);
  printer.PrintTime("date");
  printer.EndTag("compilation");
}

void HGraphVisualizer::DumpGraph(const char* pass_name) {
  if (!is_enabled_) {
    return;
  }
  HGraphVisualizerPrinter printer(graph_, *output_, pass_name, codegen_);
  printer.Run();
}

}  // namespace art
