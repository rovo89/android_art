/*
 * Copyright (C) 2012 The Android Open Source Project
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

#include <cassert>
#include <cctype>
#include <cstring>
#include <iostream>
#include <string>

// Should be synced with the one in lir_desc.h
#define MAX_LIR_OPERANDS  6

//----------------------------------------------------------------------------
// Operand Info
//----------------------------------------------------------------------------
class LIROperand {
 public:
  enum Type {
    UnknownType,
    RegisterType,
    ImmediateType,
    LabelType,
    FrameIndexType,
  };
};

// Copy from lir_desc.h
class LIRDescFlag {
 public:
  enum Flag {
    kNone,
    kPseudo,
    kVariadic,

    kIsLoad,
    kIsStore,
    kIsBranch,

    kDefReg0,
    kDefReg1,

    kUseReg0,
    kUseReg1,
    kUseReg2,
    kUseReg3,
    kUseReg4,
    kUseReg5,

    kSetCCodes,
    kUseCCodes,

    kNeedRelax,
  };
};

class LIROperandInfo {
 public:
  class TypeStrings {
   public:
    // Take RegisterType as example:
    //  type_string_ is "unsigned"
    //  var_string_ is "reg"
    //  set_method_ is "SetReg"
    const char* type_string_;
    const char* var_string_;
    const char* set_method_;
  };

  LIROperand::Type GetOperandType(unsigned i) const {
    assert((i < num_operands_) && "Invalid operand index!");
    return info_[i].type_;
  }

  const TypeStrings& GetOperandTypeStrings(unsigned i) const {
    return OperandTypeStrings[GetOperandType(i)];
  }

 private:
  static const TypeStrings OperandTypeStrings[];

 public:
  const char* name_;
  unsigned num_operands_;
  struct {
    enum LIROperand::Type type_;
  } info_[MAX_LIR_OPERANDS];
};

const LIROperandInfo::TypeStrings LIROperandInfo::OperandTypeStrings[] = {
  /* UnknownType    */ { "void",           "",            "",              },
  /* RegisterType   */ { "unsigned",       "reg",         "SetReg"         },
  /* ImmediateType  */ { "int32_t" ,       "imm_val",     "SetImm"         },
  /* LabelType      */ { "LIRBasicBlock*", "target",      "SetLabelTarget" },
  /* FrameIndexType */ { "int",            "frame_index", "SetFrameIndex"  }
};

#define DEF_LIR_OPERAND_INFO(INFO_ID, NUM_OPS, INFO_DEF) \
static const LIROperandInfo OpInfo ## INFO_ID = \
  { #INFO_ID, NUM_OPS, INFO_DEF };
#include "greenland/target_lir.def"
#include "greenland/clear_target_lir.def"

static const LIROperandInfo* const OpInfos[] = {
#define DEF_LIR_OPERAND_INFO(INFO_ID, NUM_OPS, INFO_DEF) \
  &OpInfo ## INFO_ID,
#include "greenland/target_lir.def"
#include "greenland/clear_target_lir.def"
};

static const unsigned NumObInfos = sizeof(OpInfos) / sizeof(OpInfos[0]);

//----------------------------------------------------------------------------
// Data structure of LIRDesc to load {target}_lir.def
//----------------------------------------------------------------------------

class LIRDesc {
 public:
  const char* opcode_name_;
  const char* name_;
  const char* format_;
  unsigned flags;
  const LIROperandInfo& operand_info_;

  bool IsPseudo() const {
    return (flags & (1 << LIRDescFlag::kPseudo));
  }
};

//----------------------------------------------------------------------------
// Target-independent LIR Enumeration
//----------------------------------------------------------------------------
// This is here to get the number of target-independent LIRs
// (i.e., kNumTargetIndependentLIR)
enum {
#define DEF_LIR_DESC(OPCODE, ...) OPCODE,
#include "greenland/target_lir.def"
#include "greenland/clear_target_lir.def"

  kNumTargetIndependentLIR
};

//----------------------------------------------------------------------------
// ARM
//----------------------------------------------------------------------------
static const LIRDesc ARMLIR[] = {
#define DEF_LIR_DESC(OPCODE, KIND, FLAGS, NAME, FORMAT, OPS_INFO) \
  { #OPCODE, NAME, FORMAT, FLAGS, OpInfo ## OPS_INFO },
#include "greenland/arm/arm_lir.def"
};

//----------------------------------------------------------------------------
// Mips
//----------------------------------------------------------------------------
static const LIRDesc MipsLIR[] = {
#define DEF_LIR_DESC(OPCODE, KIND, FLAGS, NAME, FORMAT, OPS_INFO) \
  { #OPCODE, NAME, FORMAT, FLAGS, OpInfo ## OPS_INFO },
#include "greenland/mips/mips_lir.def"
};

//----------------------------------------------------------------------------
// X86
//----------------------------------------------------------------------------
static const LIRDesc X86LIR[] = {
#define DEF_LIR_DESC(OPCODE, KIND, FLAGS, NAME, FORMAT, OPS_INFO) \
  { #OPCODE, NAME, FORMAT, FLAGS, OpInfo ## OPS_INFO },
#include "greenland/x86/x86_lir.def"
};

class Context {
 private:
  std::ostream& out_;
  unsigned indent_;

 public:
  Context(std::ostream& out) : out_(out), indent_(0) { }

  void IncIndent() {
    indent_ += 2;
    return;
  }

  void DecIndent() {
    indent_ -= 2;
  }

  std::ostream& Indent() {
    for (unsigned i = 0; i < indent_; i += 2) {
      out_.write("  ", 2);
    }
    return out_;
  }

  std::ostream& Newline() {
    return out_ << std::endl;
  }

  std::ostream& Out() {
    return out_;
  }

  const LIRDesc* target_lirs_;
  unsigned num_target_lirs_;

  std::string lowercase_target_string_;
  std::string uppercase_target_string_;
  std::string class_name_;
};

class GenPrototype {
  const LIROperandInfo& op_info_;
  const char* preface_;
 public:
  GenPrototype(const LIROperandInfo& op_info, const char* preface = NULL)
      : op_info_(op_info), preface_(preface) { }

  friend std::ostream& operator<<(std::ostream& out, const GenPrototype& obj);
};

std::ostream& operator<<(std::ostream& out, const GenPrototype& obj) {
  out << "(";

  unsigned num_ops = obj.op_info_.num_operands_;
  if (obj.preface_ != NULL) {
    out << obj.preface_;
    if (num_ops > 0) {
      out << ", ";
    }
  }

  for (unsigned i = 0; i < num_ops; i++) {
    const LIROperandInfo::TypeStrings& type_strings =
        obj.op_info_.GetOperandTypeStrings(i);

    out << type_strings.type_string_ << " " << type_strings.var_string_ << i;

    if (i != (num_ops - 1)) {
      // Append "," if not the last one
      out << ", ";
    }
  }

  out << ")";
  return out;
}

//----------------------------------------------------------------------------

// Forward Declarations
static void GenBeginNamespace(Context& C);
static void GenCreateTargetLIR(Context& C, const LIRDesc& lir, bool proto_only);
static void GenCallSetOperands(Context& C, const LIROperandInfo& operand_info);
static void GenEndNamespace(Context& C);

static void GenLicenseNote(Context& C) {
  C.Indent() << "/*" << std::endl;
  C.Indent() << " * Copyright (C) 2012 The Android Open Source Project" << std::endl;
  C.Indent() << " *" << std::endl;
  C.Indent() << " * Licensed under the Apache License, Version 2.0 (the \"License\"" << std::endl;
  C.Indent() << " * you may not use this file except in compliance with the License." << std::endl;
  C.Indent() << " * You may obtain a copy of the License at" << std::endl;
  C.Indent() << " *" << std::endl;
  C.Indent() << " *      http://www.apache.org/licenses/LICENSE-2.0" << std::endl;
  C.Indent() << " *" << std::endl;
  C.Indent() << " * Unless required by applicable law or agreed to in writing, software" << std::endl;
  C.Indent() << " * distributed under the License is distributed on an \"AS IS\" BASIS," << std::endl;
  C.Indent() << " * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied." << std::endl;
  C.Indent() << " * See the License for the specific language governing permissions and" << std::endl;
  C.Indent() << " * limitations under the License." << std::endl;
  C.Indent() << " */" << std::endl;
  C.Newline();
  return;
}

//----------------------------------------------------------------------------

static void GenTargetLIRBuilderHeader(Context& C) {
  C.Indent() << "#ifdef GET_" << C.uppercase_target_string_ << "_LIR_BUILDER_HEADER";
  C.Newline();

  //----------------------------------------------------------------------------
  // Include Files
  //----------------------------------------------------------------------------
  C.Indent() << "#include \"greenland/target_lir_builder.h\"" << std::endl;;
  C.Indent() << "#include \"greenland/target_lir_info.h\"" << std::endl;
  C.Newline();

  GenBeginNamespace(C);

  //----------------------------------------------------------------------------
  // Forward Declarations
  //----------------------------------------------------------------------------
  C.Indent() << "class TargetLIRInfo;" << std::endl;
  C.Newline();

  C.Indent() << "class " << C.class_name_ << " : public TargetLIRBuilder {"
             << std::endl;

  //----------------------------------------------------------------------------
  // Members
  //----------------------------------------------------------------------------
  C.Indent() << " private:" << std::endl;
  C.IncIndent();
  C.Indent() << "const TargetLIRInfo& info_;" << std::endl;
  C.DecIndent();
  C.Newline();

  //----------------------------------------------------------------------------
  // APIs
  //----------------------------------------------------------------------------
  C.Indent() << " public:" << std::endl;

  C.IncIndent();

  //----------------------------------------------------------------------------
  // Constructor
  //----------------------------------------------------------------------------
  C.Indent() << C.class_name_ << "(const TargetLIRInfo& info) "
                                 ": TargetLIRBuilder(), info_(info) { }"
             << std::endl;
  C.Newline();

  //----------------------------------------------------------------------------
  // LIR* Create(unsigned opcode)
  //----------------------------------------------------------------------------
  C.Indent() << "LIR* Create(unsigned opcode) {" << std::endl;
  C.IncIndent();

  C.Indent() << "return bb_->GetParent().CreateLIR(*bb_, info_.GetLIRDesc(opcode));" << std::endl;

  C.DecIndent();
  C.Indent() << "}" << std::endl;
  C.Newline();

  //----------------------------------------------------------------------------
  // LIR* Create(LIRDesc& desc)
  //----------------------------------------------------------------------------
  C.Indent() << "LIR* Create(const LIRDesc& desc) {" << std::endl;
  C.IncIndent();

  C.Indent() << "return bb_->GetParent().CreateLIR(*bb_, desc);" << std::endl;

  C.DecIndent();
  C.Indent() << "}" << std::endl;
  C.Newline();

  //----------------------------------------------------------------------------
  // LIR* Create_*([operand...])
  //----------------------------------------------------------------------------
  for (unsigned i = 0; i < C.num_target_lirs_; i++) {
    const LIRDesc& lir = C.target_lirs_[i];
    if (!lir.IsPseudo() || (i > kNumTargetIndependentLIR)) {
      GenCreateTargetLIR(C, lir, /* proto_only */true);
    }
  }

  C.DecIndent();

  C.Indent() << "};" << std::endl;
  C.Newline();

  GenEndNamespace(C);

  C.Indent() << "#undef GET_" << C.uppercase_target_string_ << "_LIR_BUILDER_HEADER" << std::endl;
  C.Indent() << "#endif // GET_" << C.uppercase_target_string_ << "_LIR_BUILDER_HEADER" << std::endl;
  return;
}

static void GenTargetLIRBuilderImpl(Context& C) {
  C.Indent() << "#ifdef GET_" << C.uppercase_target_string_ << "_LIR_BUILDER_IMPL";
  C.Newline();

  //----------------------------------------------------------------------------
  // Include Files
  //----------------------------------------------------------------------------
  C.Indent() << "#include \"greenland/lir_desc.h\"" << std::endl;
  C.Indent() << "#include \"greenland/lir_function.h\"" << std::endl;
  C.Indent() << "#include \"greenland/" << C.lowercase_target_string_
             << "/" << C.lowercase_target_string_ << "_lir_opcodes.h\""
             << std::endl;
  C.Newline();

  GenBeginNamespace(C);

  for (unsigned i = 0; i < C.num_target_lirs_; i++) {
    const LIRDesc& lir = C.target_lirs_[i];
    if (!lir.IsPseudo() || (i > kNumTargetIndependentLIR)) {
      GenCreateTargetLIR(C, lir, /* proto_only */false);
    }
  }

  C.Newline();

  GenEndNamespace(C);

  C.Indent() << "#undef GET_" << C.uppercase_target_string_ << "_LIR_BUILDER_IMPL" << std::endl;
  C.Indent() << "#endif // GET_" << C.uppercase_target_string_ << "_LIR_BUILDER_IMPL" << std::endl;
  C.Newline();
  return;
}

//----------------------------------------------------------------------------

static void GenBeginNamespace(Context& C) {
  C.Indent() << "namespace art {" << std::endl;
  C.Indent() << "namespace greenland {" << std::endl;
  C.Newline();
  return;
}

static void GenCreateTargetLIR(Context& C, const LIRDesc& lir, bool proto_only){
  C.Indent() << "// " << lir.name_ << " " << lir.format_ << std::endl;

  if (proto_only) {
    C.Indent() << "LIR* Create_" << &lir.opcode_name_[1] /* hack: skip 'k' */
               << GenPrototype(lir.operand_info_) << ";" << std::endl;
    return;
  } else {
    C.Indent() << "LIR* " << C.class_name_ << "::Create_"
               << &lir.opcode_name_[1] /* hack: skip 'k' */
               << GenPrototype(lir.operand_info_) << " {" << std::endl;
  }

  C.IncIndent();
  C.Indent() << "LIR* lir = Create("
             << C.lowercase_target_string_ << "::" << lir.opcode_name_<< ");"
             << std::endl;
  GenCallSetOperands(C, lir.operand_info_);
  C.Indent() << "return lir;" << std::endl;
  C.DecIndent();

  C.Indent() << "}" << std::endl;
  C.Newline();
  return;
}

static void
GenCallSetOperands(Context& C, const LIROperandInfo& operand_info) {
  if (operand_info.num_operands_ > 0) {
    C.Indent() << "Set" << operand_info.name_ << "Operands(*lir, ";

    for (unsigned i = 0; i < operand_info.num_operands_; i++) {
      const LIROperandInfo::TypeStrings& type_strings =
        operand_info.GetOperandTypeStrings(i);

      C.Out() << type_strings.var_string_ << i;

      if (i != (operand_info.num_operands_ - 1)) {
        // Append "," if not the last one
        C.Out() << ", ";
      }
    }

    C.Out() << ");" << std::endl;
  }
  return;
}

static void GenEndNamespace(Context& C) {
  C.Newline();
  C.Indent() << "} // namespace greenland" << std::endl;
  C.Indent() << "} // namespace art" << std::endl;
  C.Newline();
  return;
}

//----------------------------------------------------------------------------
// Special functionality to generate Set*Operands in TargetLIRBuilder
//----------------------------------------------------------------------------
static void GenSetOperandsImpl(Context& C, const LIROperandInfo& operand_info);

static void GenSetOperands(Context& C) {
  for (unsigned i = 0; i < NumObInfos; i++) {
    const LIROperandInfo& op_info = *OpInfos[i];
    C.Indent() << "static inline void Set" << op_info.name_
               << "Operands" << GenPrototype(op_info, /* preface */"LIR& lir")
               << " {" << std::endl;
    C.IncIndent();
    GenSetOperandsImpl(C, op_info);
    C.DecIndent();
    C.Indent() << "}" << std::endl;
    C.Newline();
  }

  return;
}

static void GenSetOperandsImpl(Context& C, const LIROperandInfo& operand_info) {
  if (operand_info.num_operands_ <= 0) {
    return;
  }

  C.Indent() << "lir";
  for (unsigned i = 0; i < operand_info.num_operands_; i++) {
    const LIROperandInfo::TypeStrings& type_strings =
      operand_info.GetOperandTypeStrings(i);

    if (i != 0) {
      C.Indent() << "   ";
    }

    C.Out() << "." << type_strings.set_method_ << "(" << i << ", "
                                               << type_strings.var_string_ << i
                                               << ")";
    if (i == (operand_info.num_operands_ - 1)) {
      // Append ";" if not the last one
      C.Out() << ";";
    }
    C.Out() << std::endl;
  }

  C.Indent() << "return;" << std::endl;

  return;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: " << argv[0] << " [target]" << std::endl;
    return 1;
  }

  Context C(std::cout);

  C.lowercase_target_string_ = argv[1];
  for (size_t i = 0, e = C.lowercase_target_string_.length(); i != e; i++) {
    C.lowercase_target_string_[i] = ::tolower(C.lowercase_target_string_[i]);
  }

  // Special function to generate TargetLIRBuilder::Set*Operands(...).
  if (C.lowercase_target_string_ == "gen_set_operands") {
    GenSetOperands(C);
    return 0;
  }

  // Setup the context according to the target supplied
  if (C.lowercase_target_string_ == "arm") {
    C.target_lirs_ = ARMLIR;
    C.num_target_lirs_ = sizeof(ARMLIR) / sizeof(ARMLIR[0]);
    C.uppercase_target_string_ = "ARM";
    C.class_name_ = "ARMLIRBuilderBase";
  } else if (C.lowercase_target_string_ == "mips") {
    C.target_lirs_ = MipsLIR;
    C.num_target_lirs_ = sizeof(MipsLIR) / sizeof(MipsLIR[0]);
    C.uppercase_target_string_ = "MIPS";
    C.class_name_ = "MipsLIRBuilderBase";
  } else if (C.lowercase_target_string_ == "x86") {
    C.target_lirs_ = X86LIR;
    C.num_target_lirs_ = sizeof(X86LIR) / sizeof(X86LIR[0]);
    C.uppercase_target_string_ = "X86";
    C.class_name_ = "X86LIRBuilderBase";
  } else {
    std::cerr << "unknown target `" << argv[1] << "'!" << std::endl;
    return 1;
  }

  GenLicenseNote(C);
  GenTargetLIRBuilderHeader(C);
  GenTargetLIRBuilderImpl(C);

  return 0;
}
