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
#include "dead_code_elimination.h"
#include "licm.h"
#include "nodes.h"
#include "optimization.h"
#include "register_allocator.h"
#include "ssa_liveness_analysis.h"

#include <cctype>
#include <sstream>

namespace art {

static bool HasWhitespace(const char* str) {
  DCHECK(str != nullptr);
  while (str[0] != 0) {
    if (isspace(str[0])) {
      return true;
    }
    str++;
  }
  return false;
}

class StringList {
 public:
  enum Format {
    kArrayBrackets,
    kSetBrackets,
  };

  // Create an empty list
  explicit StringList(Format format = kArrayBrackets) : format_(format), is_empty_(true) {}

  // Construct StringList from a linked list. List element class T
  // must provide methods `GetNext` and `Dump`.
  template<class T>
  explicit StringList(T* first_entry, Format format = kArrayBrackets) : StringList(format) {
    for (T* current = first_entry; current != nullptr; current = current->GetNext()) {
      current->Dump(NewEntryStream());
    }
  }

  std::ostream& NewEntryStream() {
    if (is_empty_) {
      is_empty_ = false;
    } else {
      sstream_ << ",";
    }
    return sstream_;
  }

 private:
  Format format_;
  bool is_empty_;
  std::ostringstream sstream_;

  friend std::ostream& operator<<(std::ostream& os, const StringList& list);
};

std::ostream& operator<<(std::ostream& os, const StringList& list) {
  switch (list.format_) {
    case StringList::kArrayBrackets: return os << "[" << list.sstream_.str() << "]";
    case StringList::kSetBrackets:   return os << "{" << list.sstream_.str() << "}";
    default:
      LOG(FATAL) << "Invalid StringList format";
      UNREACHABLE();
  }
}

/**
 * HGraph visitor to generate a file suitable for the c1visualizer tool and IRHydra.
 */
class HGraphVisualizerPrinter : public HGraphVisitor {
 public:
  HGraphVisualizerPrinter(HGraph* graph,
                          std::ostream& output,
                          const char* pass_name,
                          bool is_after_pass,
                          const CodeGenerator& codegen)
      : HGraphVisitor(graph),
        output_(output),
        pass_name_(pass_name),
        is_after_pass_(is_after_pass),
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
    output_ << name << " " << time(nullptr) << std::endl;
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

  char GetTypeId(Primitive::Type type) {
    // Note that Primitive::Descriptor would not work for us
    // because it does not handle reference types (that is kPrimNot).
    switch (type) {
      case Primitive::kPrimBoolean: return 'z';
      case Primitive::kPrimByte: return 'b';
      case Primitive::kPrimChar: return 'c';
      case Primitive::kPrimShort: return 's';
      case Primitive::kPrimInt: return 'i';
      case Primitive::kPrimLong: return 'j';
      case Primitive::kPrimFloat: return 'f';
      case Primitive::kPrimDouble: return 'd';
      case Primitive::kPrimNot: return 'l';
      case Primitive::kPrimVoid: return 'v';
    }
    LOG(FATAL) << "Unreachable";
    return 'v';
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

  void DumpLocation(std::ostream& stream, const Location& location) {
    if (location.IsRegister()) {
      codegen_.DumpCoreRegister(stream, location.reg());
    } else if (location.IsFpuRegister()) {
      codegen_.DumpFloatingPointRegister(stream, location.reg());
    } else if (location.IsConstant()) {
      stream << "#";
      HConstant* constant = location.GetConstant();
      if (constant->IsIntConstant()) {
        stream << constant->AsIntConstant()->GetValue();
      } else if (constant->IsLongConstant()) {
        stream << constant->AsLongConstant()->GetValue();
      }
    } else if (location.IsInvalid()) {
      stream << "invalid";
    } else if (location.IsStackSlot()) {
      stream << location.GetStackIndex() << "(sp)";
    } else if (location.IsFpuRegisterPair()) {
      codegen_.DumpFloatingPointRegister(stream, location.low());
      stream << "|";
      codegen_.DumpFloatingPointRegister(stream, location.high());
    } else if (location.IsRegisterPair()) {
      codegen_.DumpCoreRegister(stream, location.low());
      stream << "|";
      codegen_.DumpCoreRegister(stream, location.high());
    } else if (location.IsUnallocated()) {
      stream << "unallocated";
    } else {
      DCHECK(location.IsDoubleStackSlot());
      stream << "2x" << location.GetStackIndex() << "(sp)";
    }
  }

  std::ostream& StartAttributeStream(const char* name = nullptr) {
    if (name == nullptr) {
      output_ << " ";
    } else {
      DCHECK(!HasWhitespace(name)) << "Checker does not allow spaces in attributes";
      output_ << " " << name << ":";
    }
    return output_;
  }

  void VisitParallelMove(HParallelMove* instruction) OVERRIDE {
    StartAttributeStream("liveness") << instruction->GetLifetimePosition();
    StringList moves;
    for (size_t i = 0, e = instruction->NumMoves(); i < e; ++i) {
      MoveOperands* move = instruction->MoveOperandsAt(i);
      std::ostream& str = moves.NewEntryStream();
      DumpLocation(str, move->GetSource());
      str << "->";
      DumpLocation(str, move->GetDestination());
    }
    StartAttributeStream("moves") <<  moves;
  }

  void VisitIntConstant(HIntConstant* instruction) OVERRIDE {
    StartAttributeStream() << instruction->GetValue();
  }

  void VisitLongConstant(HLongConstant* instruction) OVERRIDE {
    StartAttributeStream() << instruction->GetValue();
  }

  void VisitFloatConstant(HFloatConstant* instruction) OVERRIDE {
    StartAttributeStream() << instruction->GetValue();
  }

  void VisitDoubleConstant(HDoubleConstant* instruction) OVERRIDE {
    StartAttributeStream() << instruction->GetValue();
  }

  void VisitPhi(HPhi* phi) OVERRIDE {
    StartAttributeStream("reg") << phi->GetRegNumber();
  }

  void VisitMemoryBarrier(HMemoryBarrier* barrier) OVERRIDE {
    StartAttributeStream("kind") << barrier->GetBarrierKind();
  }

  void VisitCheckCast(HCheckCast* check_cast) OVERRIDE {
    StartAttributeStream("must_do_null_check") << std::boolalpha << check_cast->MustDoNullCheck()
      << std::noboolalpha;
  }

  void VisitInstanceOf(HInstanceOf* instance_of) OVERRIDE {
    StartAttributeStream("must_do_null_check") << std::boolalpha << instance_of->MustDoNullCheck()
      << std::noboolalpha;
  }

  bool IsPass(const char* name) {
    return strcmp(pass_name_, name) == 0;
  }

  void PrintInstruction(HInstruction* instruction) {
    output_ << instruction->DebugName();
    if (instruction->InputCount() > 0) {
      StringList inputs;
      for (HInputIterator it(instruction); !it.Done(); it.Advance()) {
        inputs.NewEntryStream() << GetTypeId(it.Current()->GetType()) << it.Current()->GetId();
      }
      StartAttributeStream() << inputs;
    }
    instruction->Accept(this);
    if (instruction->HasEnvironment()) {
      StringList envs;
      for (HEnvironment* environment = instruction->GetEnvironment();
           environment != nullptr;
           environment = environment->GetParent()) {
        StringList vregs;
        for (size_t i = 0, e = environment->Size(); i < e; ++i) {
          HInstruction* insn = environment->GetInstructionAt(i);
          if (insn != nullptr) {
            vregs.NewEntryStream() << GetTypeId(insn->GetType()) << insn->GetId();
          } else {
            vregs.NewEntryStream() << "_";
          }
        }
        envs.NewEntryStream() << vregs;
      }
      StartAttributeStream("env") << envs;
    }
    if (IsPass(SsaLivenessAnalysis::kLivenessPassName)
        && is_after_pass_
        && instruction->GetLifetimePosition() != kNoLifetime) {
      StartAttributeStream("liveness") << instruction->GetLifetimePosition();
      if (instruction->HasLiveInterval()) {
        LiveInterval* interval = instruction->GetLiveInterval();
        StartAttributeStream("ranges")
            << StringList(interval->GetFirstRange(), StringList::kSetBrackets);
        StartAttributeStream("uses") << StringList(interval->GetFirstUse());
        StartAttributeStream("env_uses") << StringList(interval->GetFirstEnvironmentUse());
        StartAttributeStream("is_fixed") << interval->IsFixed();
        StartAttributeStream("is_split") << interval->IsSplit();
        StartAttributeStream("is_low") << interval->IsLowInterval();
        StartAttributeStream("is_high") << interval->IsHighInterval();
      }
    } else if (IsPass(RegisterAllocator::kRegisterAllocatorPassName) && is_after_pass_) {
      StartAttributeStream("liveness") << instruction->GetLifetimePosition();
      LocationSummary* locations = instruction->GetLocations();
      if (locations != nullptr) {
        StringList inputs;
        for (size_t i = 0; i < instruction->InputCount(); ++i) {
          DumpLocation(inputs.NewEntryStream(), locations->InAt(i));
        }
        std::ostream& attr = StartAttributeStream("locations");
        attr << inputs << "->";
        DumpLocation(attr, locations->Out());
      }
    } else if (IsPass(LICM::kLoopInvariantCodeMotionPassName)
               || IsPass(HDeadCodeElimination::kFinalDeadCodeEliminationPassName)) {
      HLoopInformation* info = instruction->GetBlock()->GetLoopInformation();
      if (info == nullptr) {
        StartAttributeStream("loop") << "none";
      } else {
        StartAttributeStream("loop") << "B" << info->GetHeader()->GetBlockId();
      }
    }
  }

  void PrintInstructions(const HInstructionList& list) {
    const char* kEndInstructionMarker = "<|@";
    for (HInstructionIterator it(list); !it.Done(); it.Advance()) {
      HInstruction* instruction = it.Current();
      int bci = 0;
      size_t num_uses = 0;
      for (HUseIterator<HInstruction*> use_it(instruction->GetUses());
           !use_it.Done();
           use_it.Advance()) {
        ++num_uses;
      }
      AddIndent();
      output_ << bci << " " << num_uses << " "
              << GetTypeId(instruction->GetType()) << instruction->GetId() << " ";
      PrintInstruction(instruction);
      output_ << " " << kEndInstructionMarker << std::endl;
    }
  }

  void Run() {
    StartTag("cfg");
    std::string pass_desc = std::string(pass_name_) + (is_after_pass_ ? " (after)" : " (before)");
    PrintProperty("name", pass_desc.c_str());
    VisitInsertionOrder();
    EndTag("cfg");
  }

  void VisitBasicBlock(HBasicBlock* block) OVERRIDE {
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
      output_ << instruction->GetId() << " " << GetTypeId(instruction->GetType())
              << instruction->GetId() << "[ ";
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
  const bool is_after_pass_;
  const CodeGenerator& codegen_;
  size_t indent_;

  DISALLOW_COPY_AND_ASSIGN(HGraphVisualizerPrinter);
};

HGraphVisualizer::HGraphVisualizer(std::ostream* output,
                                   HGraph* graph,
                                   const CodeGenerator& codegen)
  : output_(output), graph_(graph), codegen_(codegen) {}

void HGraphVisualizer::PrintHeader(const char* method_name) const {
  DCHECK(output_ != nullptr);
  HGraphVisualizerPrinter printer(graph_, *output_, "", true, codegen_);
  printer.StartTag("compilation");
  printer.PrintProperty("name", method_name);
  printer.PrintProperty("method", method_name);
  printer.PrintTime("date");
  printer.EndTag("compilation");
}

void HGraphVisualizer::DumpGraph(const char* pass_name, bool is_after_pass) const {
  DCHECK(output_ != nullptr);
  if (!graph_->GetBlocks().IsEmpty()) {
    HGraphVisualizerPrinter printer(graph_, *output_, pass_name, is_after_pass, codegen_);
    printer.Run();
  }
}

}  // namespace art
