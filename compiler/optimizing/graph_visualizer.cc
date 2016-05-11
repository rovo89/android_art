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

#include <dlfcn.h>

#include <cctype>
#include <sstream>

#include "bounds_check_elimination.h"
#include "builder.h"
#include "code_generator.h"
#include "dead_code_elimination.h"
#include "disassembler.h"
#include "inliner.h"
#include "licm.h"
#include "nodes.h"
#include "optimization.h"
#include "reference_type_propagation.h"
#include "register_allocator.h"
#include "ssa_liveness_analysis.h"
#include "utils/assembler.h"

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

typedef Disassembler* create_disasm_prototype(InstructionSet instruction_set,
                                              DisassemblerOptions* options);
class HGraphVisualizerDisassembler {
 public:
  HGraphVisualizerDisassembler(InstructionSet instruction_set,
                               const uint8_t* base_address,
                               const uint8_t* end_address)
      : instruction_set_(instruction_set), disassembler_(nullptr) {
    libart_disassembler_handle_ =
        dlopen(kIsDebugBuild ? "libartd-disassembler.so" : "libart-disassembler.so", RTLD_NOW);
    if (libart_disassembler_handle_ == nullptr) {
      LOG(WARNING) << "Failed to dlopen libart-disassembler: " << dlerror();
      return;
    }
    create_disasm_prototype* create_disassembler = reinterpret_cast<create_disasm_prototype*>(
        dlsym(libart_disassembler_handle_, "create_disassembler"));
    if (create_disassembler == nullptr) {
      LOG(WARNING) << "Could not find create_disassembler entry: " << dlerror();
      return;
    }
    // Reading the disassembly from 0x0 is easier, so we print relative
    // addresses. We will only disassemble the code once everything has
    // been generated, so we can read data in literal pools.
    disassembler_ = std::unique_ptr<Disassembler>((*create_disassembler)(
            instruction_set,
            new DisassemblerOptions(/* absolute_addresses */ false,
                                    base_address,
                                    end_address,
                                    /* can_read_literals */ true)));
  }

  ~HGraphVisualizerDisassembler() {
    // We need to call ~Disassembler() before we close the library.
    disassembler_.reset();
    if (libart_disassembler_handle_ != nullptr) {
      dlclose(libart_disassembler_handle_);
    }
  }

  void Disassemble(std::ostream& output, size_t start, size_t end) const {
    if (disassembler_ == nullptr) {
      return;
    }

    const uint8_t* base = disassembler_->GetDisassemblerOptions()->base_address_;
    if (instruction_set_ == kThumb2) {
      // ARM and Thumb-2 use the same disassembler. The bottom bit of the
      // address is used to distinguish between the two.
      base += 1;
    }
    disassembler_->Dump(output, base + start, base + end);
  }

 private:
  InstructionSet instruction_set_;
  std::unique_ptr<Disassembler> disassembler_;

  void* libart_disassembler_handle_;
};


/**
 * HGraph visitor to generate a file suitable for the c1visualizer tool and IRHydra.
 */
class HGraphVisualizerPrinter : public HGraphDelegateVisitor {
 public:
  HGraphVisualizerPrinter(HGraph* graph,
                          std::ostream& output,
                          const char* pass_name,
                          bool is_after_pass,
                          bool graph_in_bad_state,
                          const CodeGenerator& codegen,
                          const DisassemblyInformation* disasm_info = nullptr)
      : HGraphDelegateVisitor(graph),
        output_(output),
        pass_name_(pass_name),
        is_after_pass_(is_after_pass),
        graph_in_bad_state_(graph_in_bad_state),
        codegen_(codegen),
        disasm_info_(disasm_info),
        disassembler_(disasm_info_ != nullptr
                      ? new HGraphVisualizerDisassembler(
                            codegen_.GetInstructionSet(),
                            codegen_.GetAssembler().CodeBufferBaseAddress(),
                            codegen_.GetAssembler().CodeBufferBaseAddress()
                                + codegen_.GetAssembler().CodeSize())
                      : nullptr),
        indent_(0) {}

  void Flush() {
    // We use "\n" instead of std::endl to avoid implicit flushing which
    // generates too many syscalls during debug-GC tests (b/27826765).
    output_ << std::flush;
  }

  void StartTag(const char* name) {
    AddIndent();
    output_ << "begin_" << name << "\n";
    indent_++;
  }

  void EndTag(const char* name) {
    indent_--;
    AddIndent();
    output_ << "end_" << name << "\n";
  }

  void PrintProperty(const char* name, const char* property) {
    AddIndent();
    output_ << name << " \"" << property << "\"\n";
  }

  void PrintProperty(const char* name, const char* property, int id) {
    AddIndent();
    output_ << name << " \"" << property << id << "\"\n";
  }

  void PrintEmptyProperty(const char* name) {
    AddIndent();
    output_ << name << "\n";
  }

  void PrintTime(const char* name) {
    AddIndent();
    output_ << name << " " << time(nullptr) << "\n";
  }

  void PrintInt(const char* name, int value) {
    AddIndent();
    output_ << name << " " << value << "\n";
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
    for (HBasicBlock* predecessor : block->GetPredecessors()) {
      output_ << " \"B" << predecessor->GetBlockId() << "\" ";
    }
    if (block->IsEntryBlock() && (disasm_info_ != nullptr)) {
      output_ << " \"" << kDisassemblyBlockFrameEntry << "\" ";
    }
    output_<< "\n";
  }

  void PrintSuccessors(HBasicBlock* block) {
    AddIndent();
    output_ << "successors";
    for (HBasicBlock* successor : block->GetNormalSuccessors()) {
      output_ << " \"B" << successor->GetBlockId() << "\" ";
    }
    output_<< "\n";
  }

  void PrintExceptionHandlers(HBasicBlock* block) {
    AddIndent();
    output_ << "xhandlers";
    for (HBasicBlock* handler : block->GetExceptionalSuccessors()) {
      output_ << " \"B" << handler->GetBlockId() << "\" ";
    }
    if (block->IsExitBlock() &&
        (disasm_info_ != nullptr) &&
        !disasm_info_->GetSlowPathIntervals().empty()) {
      output_ << " \"" << kDisassemblyBlockSlowPaths << "\" ";
    }
    output_<< "\n";
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
    StartAttributeStream("is_catch_phi") << std::boolalpha << phi->IsCatchPhi() << std::noboolalpha;
  }

  void VisitMemoryBarrier(HMemoryBarrier* barrier) OVERRIDE {
    StartAttributeStream("kind") << barrier->GetBarrierKind();
  }

  void VisitMonitorOperation(HMonitorOperation* monitor) OVERRIDE {
    StartAttributeStream("kind") << (monitor->IsEnter() ? "enter" : "exit");
  }

  void VisitLoadClass(HLoadClass* load_class) OVERRIDE {
    StartAttributeStream("gen_clinit_check") << std::boolalpha
        << load_class->MustGenerateClinitCheck() << std::noboolalpha;
    StartAttributeStream("needs_access_check") << std::boolalpha
        << load_class->NeedsAccessCheck() << std::noboolalpha;
  }

  void VisitLoadString(HLoadString* load_string) OVERRIDE {
    StartAttributeStream("load_kind") << load_string->GetLoadKind();
  }

  void VisitCheckCast(HCheckCast* check_cast) OVERRIDE {
    StartAttributeStream("check_kind") << check_cast->GetTypeCheckKind();
    StartAttributeStream("must_do_null_check") << std::boolalpha
        << check_cast->MustDoNullCheck() << std::noboolalpha;
  }

  void VisitInstanceOf(HInstanceOf* instance_of) OVERRIDE {
    StartAttributeStream("check_kind") << instance_of->GetTypeCheckKind();
    StartAttributeStream("must_do_null_check") << std::boolalpha
        << instance_of->MustDoNullCheck() << std::noboolalpha;
  }

  void VisitArraySet(HArraySet* array_set) OVERRIDE {
    StartAttributeStream("value_can_be_null") << std::boolalpha
        << array_set->GetValueCanBeNull() << std::noboolalpha;
    StartAttributeStream("needs_type_check") << std::boolalpha
        << array_set->NeedsTypeCheck() << std::noboolalpha;
  }

  void VisitCompare(HCompare* compare) OVERRIDE {
    ComparisonBias bias = compare->GetBias();
    StartAttributeStream("bias") << (bias == ComparisonBias::kGtBias
                                     ? "gt"
                                     : (bias == ComparisonBias::kLtBias ? "lt" : "none"));
  }

  void VisitInvoke(HInvoke* invoke) OVERRIDE {
    StartAttributeStream("dex_file_index") << invoke->GetDexMethodIndex();
    StartAttributeStream("method_name") << PrettyMethod(
        invoke->GetDexMethodIndex(), GetGraph()->GetDexFile(), /* with_signature */ false);
  }

  void VisitInvokeUnresolved(HInvokeUnresolved* invoke) OVERRIDE {
    VisitInvoke(invoke);
    StartAttributeStream("invoke_type") << invoke->GetOriginalInvokeType();
  }

  void VisitInvokeStaticOrDirect(HInvokeStaticOrDirect* invoke) OVERRIDE {
    VisitInvoke(invoke);
    StartAttributeStream("method_load_kind") << invoke->GetMethodLoadKind();
    StartAttributeStream("intrinsic") << invoke->GetIntrinsic();
    if (invoke->IsStatic()) {
      StartAttributeStream("clinit_check") << invoke->GetClinitCheckRequirement();
    }
  }

  void VisitInvokeVirtual(HInvokeVirtual* invoke) OVERRIDE {
    VisitInvoke(invoke);
    StartAttributeStream("intrinsic") << invoke->GetIntrinsic();
  }

  void VisitInstanceFieldGet(HInstanceFieldGet* iget) OVERRIDE {
    StartAttributeStream("field_name") << PrettyField(iget->GetFieldInfo().GetFieldIndex(),
                                                      iget->GetFieldInfo().GetDexFile(),
                                                      /* with type */ false);
    StartAttributeStream("field_type") << iget->GetFieldType();
  }

  void VisitInstanceFieldSet(HInstanceFieldSet* iset) OVERRIDE {
    StartAttributeStream("field_name") << PrettyField(iset->GetFieldInfo().GetFieldIndex(),
                                                      iset->GetFieldInfo().GetDexFile(),
                                                      /* with type */ false);
    StartAttributeStream("field_type") << iset->GetFieldType();
  }

  void VisitUnresolvedInstanceFieldGet(HUnresolvedInstanceFieldGet* field_access) OVERRIDE {
    StartAttributeStream("field_type") << field_access->GetFieldType();
  }

  void VisitUnresolvedInstanceFieldSet(HUnresolvedInstanceFieldSet* field_access) OVERRIDE {
    StartAttributeStream("field_type") << field_access->GetFieldType();
  }

  void VisitUnresolvedStaticFieldGet(HUnresolvedStaticFieldGet* field_access) OVERRIDE {
    StartAttributeStream("field_type") << field_access->GetFieldType();
  }

  void VisitUnresolvedStaticFieldSet(HUnresolvedStaticFieldSet* field_access) OVERRIDE {
    StartAttributeStream("field_type") << field_access->GetFieldType();
  }

  void VisitTryBoundary(HTryBoundary* try_boundary) OVERRIDE {
    StartAttributeStream("kind") << (try_boundary->IsEntry() ? "entry" : "exit");
  }

#if defined(ART_ENABLE_CODEGEN_arm) || defined(ART_ENABLE_CODEGEN_arm64)
  void VisitMultiplyAccumulate(HMultiplyAccumulate* instruction) OVERRIDE {
    StartAttributeStream("kind") << instruction->GetOpKind();
  }

  void VisitBitwiseNegatedRight(HBitwiseNegatedRight* instruction) OVERRIDE {
    StartAttributeStream("kind") << instruction->GetOpKind();
  }
#endif

#ifdef ART_ENABLE_CODEGEN_arm64
  void VisitArm64DataProcWithShifterOp(HArm64DataProcWithShifterOp* instruction) OVERRIDE {
    StartAttributeStream("kind") << instruction->GetInstrKind() << "+" << instruction->GetOpKind();
    if (HArm64DataProcWithShifterOp::IsShiftOp(instruction->GetOpKind())) {
      StartAttributeStream("shift") << instruction->GetShiftAmount();
    }
  }
#endif

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
    }

    if (IsPass(RegisterAllocator::kRegisterAllocatorPassName) && is_after_pass_) {
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
    }

    HLoopInformation* loop_info = instruction->GetBlock()->GetLoopInformation();
    if (loop_info == nullptr) {
      StartAttributeStream("loop") << "none";
    } else {
      StartAttributeStream("loop") << "B" << loop_info->GetHeader()->GetBlockId();
      HLoopInformation* outer = loop_info->GetPreHeader()->GetLoopInformation();
      if (outer != nullptr) {
        StartAttributeStream("outer_loop") << "B" << outer->GetHeader()->GetBlockId();
      } else {
        StartAttributeStream("outer_loop") << "none";
      }
      StartAttributeStream("irreducible")
          << std::boolalpha << loop_info->IsIrreducible() << std::noboolalpha;
    }

    if ((IsPass(HGraphBuilder::kBuilderPassName)
        || IsPass(HInliner::kInlinerPassName))
        && (instruction->GetType() == Primitive::kPrimNot)) {
      ReferenceTypeInfo info = instruction->IsLoadClass()
        ? instruction->AsLoadClass()->GetLoadedClassRTI()
        : instruction->GetReferenceTypeInfo();
      ScopedObjectAccess soa(Thread::Current());
      if (info.IsValid()) {
        StartAttributeStream("klass") << PrettyDescriptor(info.GetTypeHandle().Get());
        StartAttributeStream("can_be_null")
            << std::boolalpha << instruction->CanBeNull() << std::noboolalpha;
        StartAttributeStream("exact") << std::boolalpha << info.IsExact() << std::noboolalpha;
      } else if (instruction->IsLoadClass()) {
        StartAttributeStream("klass") << "unresolved";
      } else {
        // The NullConstant may be added to the graph during other passes that happen between
        // ReferenceTypePropagation and Inliner (e.g. InstructionSimplifier). If the inliner
        // doesn't run or doesn't inline anything, the NullConstant remains untyped.
        // So we should check NullConstants for validity only after reference type propagation.
        DCHECK(graph_in_bad_state_ ||
               (!is_after_pass_ && IsPass(HGraphBuilder::kBuilderPassName)))
            << instruction->DebugName() << instruction->GetId() << " has invalid rti "
            << (is_after_pass_ ? "after" : "before") << " pass " << pass_name_;
      }
    }
    if (disasm_info_ != nullptr) {
      DCHECK(disassembler_ != nullptr);
      // If the information is available, disassemble the code generated for
      // this instruction.
      auto it = disasm_info_->GetInstructionIntervals().find(instruction);
      if (it != disasm_info_->GetInstructionIntervals().end()
          && it->second.start != it->second.end) {
        output_ << "\n";
        disassembler_->Disassemble(output_, it->second.start, it->second.end);
      }
    }
  }

  void PrintInstructions(const HInstructionList& list) {
    for (HInstructionIterator it(list); !it.Done(); it.Advance()) {
      HInstruction* instruction = it.Current();
      int bci = 0;
      size_t num_uses = instruction->GetUses().SizeSlow();
      AddIndent();
      output_ << bci << " " << num_uses << " "
              << GetTypeId(instruction->GetType()) << instruction->GetId() << " ";
      PrintInstruction(instruction);
      output_ << " " << kEndInstructionMarker << "\n";
    }
  }

  void DumpStartOfDisassemblyBlock(const char* block_name,
                                   int predecessor_index,
                                   int successor_index) {
    StartTag("block");
    PrintProperty("name", block_name);
    PrintInt("from_bci", -1);
    PrintInt("to_bci", -1);
    if (predecessor_index != -1) {
      PrintProperty("predecessors", "B", predecessor_index);
    } else {
      PrintEmptyProperty("predecessors");
    }
    if (successor_index != -1) {
      PrintProperty("successors", "B", successor_index);
    } else {
      PrintEmptyProperty("successors");
    }
    PrintEmptyProperty("xhandlers");
    PrintEmptyProperty("flags");
    StartTag("states");
    StartTag("locals");
    PrintInt("size", 0);
    PrintProperty("method", "None");
    EndTag("locals");
    EndTag("states");
    StartTag("HIR");
  }

  void DumpEndOfDisassemblyBlock() {
    EndTag("HIR");
    EndTag("block");
  }

  void DumpDisassemblyBlockForFrameEntry() {
    DumpStartOfDisassemblyBlock(kDisassemblyBlockFrameEntry,
                                -1,
                                GetGraph()->GetEntryBlock()->GetBlockId());
    output_ << "    0 0 disasm " << kDisassemblyBlockFrameEntry << " ";
    GeneratedCodeInterval frame_entry = disasm_info_->GetFrameEntryInterval();
    if (frame_entry.start != frame_entry.end) {
      output_ << "\n";
      disassembler_->Disassemble(output_, frame_entry.start, frame_entry.end);
    }
    output_ << kEndInstructionMarker << "\n";
    DumpEndOfDisassemblyBlock();
  }

  void DumpDisassemblyBlockForSlowPaths() {
    if (disasm_info_->GetSlowPathIntervals().empty()) {
      return;
    }
    // If the graph has an exit block we attach the block for the slow paths
    // after it. Else we just add the block to the graph without linking it to
    // any other.
    DumpStartOfDisassemblyBlock(
        kDisassemblyBlockSlowPaths,
        GetGraph()->HasExitBlock() ? GetGraph()->GetExitBlock()->GetBlockId() : -1,
        -1);
    for (SlowPathCodeInfo info : disasm_info_->GetSlowPathIntervals()) {
      output_ << "    0 0 disasm " << info.slow_path->GetDescription() << "\n";
      disassembler_->Disassemble(output_, info.code_interval.start, info.code_interval.end);
      output_ << kEndInstructionMarker << "\n";
    }
    DumpEndOfDisassemblyBlock();
  }

  void Run() {
    StartTag("cfg");
    std::string pass_desc = std::string(pass_name_)
                          + " ("
                          + (is_after_pass_ ? "after" : "before")
                          + (graph_in_bad_state_ ? ", bad_state" : "")
                          + ")";
    PrintProperty("name", pass_desc.c_str());
    if (disasm_info_ != nullptr) {
      DumpDisassemblyBlockForFrameEntry();
    }
    VisitInsertionOrder();
    if (disasm_info_ != nullptr) {
      DumpDisassemblyBlockForSlowPaths();
    }
    EndTag("cfg");
    Flush();
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
    PrintExceptionHandlers(block);

    if (block->IsCatchBlock()) {
      PrintProperty("flags", "catch_block");
    } else {
      PrintEmptyProperty("flags");
    }

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
      output_ << "]\n";
    }
    EndTag("locals");
    EndTag("states");

    StartTag("HIR");
    PrintInstructions(block->GetPhis());
    PrintInstructions(block->GetInstructions());
    EndTag("HIR");
    EndTag("block");
  }

  static constexpr const char* const kEndInstructionMarker = "<|@";
  static constexpr const char* const kDisassemblyBlockFrameEntry = "FrameEntry";
  static constexpr const char* const kDisassemblyBlockSlowPaths = "SlowPaths";

 private:
  std::ostream& output_;
  const char* pass_name_;
  const bool is_after_pass_;
  const bool graph_in_bad_state_;
  const CodeGenerator& codegen_;
  const DisassemblyInformation* disasm_info_;
  std::unique_ptr<HGraphVisualizerDisassembler> disassembler_;
  size_t indent_;

  DISALLOW_COPY_AND_ASSIGN(HGraphVisualizerPrinter);
};

HGraphVisualizer::HGraphVisualizer(std::ostream* output,
                                   HGraph* graph,
                                   const CodeGenerator& codegen)
  : output_(output), graph_(graph), codegen_(codegen) {}

void HGraphVisualizer::PrintHeader(const char* method_name) const {
  DCHECK(output_ != nullptr);
  HGraphVisualizerPrinter printer(graph_, *output_, "", true, false, codegen_);
  printer.StartTag("compilation");
  printer.PrintProperty("name", method_name);
  printer.PrintProperty("method", method_name);
  printer.PrintTime("date");
  printer.EndTag("compilation");
  printer.Flush();
}

void HGraphVisualizer::DumpGraph(const char* pass_name,
                                 bool is_after_pass,
                                 bool graph_in_bad_state) const {
  DCHECK(output_ != nullptr);
  if (!graph_->GetBlocks().empty()) {
    HGraphVisualizerPrinter printer(graph_,
                                    *output_,
                                    pass_name,
                                    is_after_pass,
                                    graph_in_bad_state,
                                    codegen_);
    printer.Run();
  }
}

void HGraphVisualizer::DumpGraphWithDisassembly() const {
  DCHECK(output_ != nullptr);
  if (!graph_->GetBlocks().empty()) {
    HGraphVisualizerPrinter printer(graph_,
                                    *output_,
                                    "disassembly",
                                    /* is_after_pass */ true,
                                    /* graph_in_bad_state */ false,
                                    codegen_,
                                    codegen_.GetDisassemblyInformation());
    printer.Run();
  }
}

}  // namespace art
