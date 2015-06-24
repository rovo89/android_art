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

#include "quick_compiler.h"

#include <cstdint>

#include "art_method-inl.h"
#include "base/dumpable.h"
#include "base/logging.h"
#include "base/macros.h"
#include "base/timing_logger.h"
#include "compiler.h"
#include "dex_file-inl.h"
#include "dex_file_to_method_inliner_map.h"
#include "dex/compiler_ir.h"
#include "dex/dex_flags.h"
#include "dex/mir_graph.h"
#include "dex/pass_driver_me_opts.h"
#include "dex/pass_driver_me_post_opt.h"
#include "dex/pass_manager.h"
#include "dex/quick/mir_to_lir.h"
#include "dex/verified_method.h"
#include "driver/compiler_driver.h"
#include "driver/compiler_options.h"
#include "elf_writer_quick.h"
#include "jni/quick/jni_compiler.h"
#include "mir_to_lir.h"
#include "mirror/object.h"
#include "runtime.h"

// Specific compiler backends.
#include "dex/quick/arm/backend_arm.h"
#include "dex/quick/arm64/backend_arm64.h"
#include "dex/quick/mips/backend_mips.h"
#include "dex/quick/x86/backend_x86.h"

namespace art {

static_assert(0U == static_cast<size_t>(kNone),   "kNone not 0");
static_assert(1U == static_cast<size_t>(kArm),    "kArm not 1");
static_assert(2U == static_cast<size_t>(kArm64),  "kArm64 not 2");
static_assert(3U == static_cast<size_t>(kThumb2), "kThumb2 not 3");
static_assert(4U == static_cast<size_t>(kX86),    "kX86 not 4");
static_assert(5U == static_cast<size_t>(kX86_64), "kX86_64 not 5");
static_assert(6U == static_cast<size_t>(kMips),   "kMips not 6");
static_assert(7U == static_cast<size_t>(kMips64), "kMips64 not 7");

// Additional disabled optimizations (over generally disabled) per instruction set.
static constexpr uint32_t kDisabledOptimizationsPerISA[] = {
    // 0 = kNone.
    ~0U,
    // 1 = kArm, unused (will use kThumb2).
    ~0U,
    // 2 = kArm64.
    0,
    // 3 = kThumb2.
    0,
    // 4 = kX86.
    (1 << kLoadStoreElimination) |
    0,
    // 5 = kX86_64.
    (1 << kLoadStoreElimination) |
    0,
    // 6 = kMips.
    (1 << kLoadStoreElimination) |
    (1 << kLoadHoisting) |
    (1 << kSuppressLoads) |
    (1 << kNullCheckElimination) |
    (1 << kPromoteRegs) |
    (1 << kTrackLiveTemps) |
    (1 << kSafeOptimizations) |
    (1 << kBBOpt) |
    (1 << kMatch) |
    (1 << kPromoteCompilerTemps) |
    0,
    // 7 = kMips64.
    (1 << kLoadStoreElimination) |
    (1 << kLoadHoisting) |
    (1 << kSuppressLoads) |
    (1 << kNullCheckElimination) |
    (1 << kPromoteRegs) |
    (1 << kTrackLiveTemps) |
    (1 << kSafeOptimizations) |
    (1 << kBBOpt) |
    (1 << kMatch) |
    (1 << kPromoteCompilerTemps) |
    0
};
static_assert(sizeof(kDisabledOptimizationsPerISA) == 8 * sizeof(uint32_t),
              "kDisabledOpts unexpected");

// Supported shorty types per instruction set. null means that all are available.
// Z : boolean
// B : byte
// S : short
// C : char
// I : int
// J : long
// F : float
// D : double
// L : reference(object, array)
// V : void
static const char* kSupportedTypes[] = {
    // 0 = kNone.
    "",
    // 1 = kArm, unused (will use kThumb2).
    "",
    // 2 = kArm64.
    nullptr,
    // 3 = kThumb2.
    nullptr,
    // 4 = kX86.
    nullptr,
    // 5 = kX86_64.
    nullptr,
    // 6 = kMips.
    nullptr,
    // 7 = kMips64.
    nullptr
};
static_assert(sizeof(kSupportedTypes) == 8 * sizeof(char*), "kSupportedTypes unexpected");

static int kAllOpcodes[] = {
    Instruction::NOP,
    Instruction::MOVE,
    Instruction::MOVE_FROM16,
    Instruction::MOVE_16,
    Instruction::MOVE_WIDE,
    Instruction::MOVE_WIDE_FROM16,
    Instruction::MOVE_WIDE_16,
    Instruction::MOVE_OBJECT,
    Instruction::MOVE_OBJECT_FROM16,
    Instruction::MOVE_OBJECT_16,
    Instruction::MOVE_RESULT,
    Instruction::MOVE_RESULT_WIDE,
    Instruction::MOVE_RESULT_OBJECT,
    Instruction::MOVE_EXCEPTION,
    Instruction::RETURN_VOID,
    Instruction::RETURN,
    Instruction::RETURN_WIDE,
    Instruction::RETURN_OBJECT,
    Instruction::CONST_4,
    Instruction::CONST_16,
    Instruction::CONST,
    Instruction::CONST_HIGH16,
    Instruction::CONST_WIDE_16,
    Instruction::CONST_WIDE_32,
    Instruction::CONST_WIDE,
    Instruction::CONST_WIDE_HIGH16,
    Instruction::CONST_STRING,
    Instruction::CONST_STRING_JUMBO,
    Instruction::CONST_CLASS,
    Instruction::MONITOR_ENTER,
    Instruction::MONITOR_EXIT,
    Instruction::CHECK_CAST,
    Instruction::INSTANCE_OF,
    Instruction::ARRAY_LENGTH,
    Instruction::NEW_INSTANCE,
    Instruction::NEW_ARRAY,
    Instruction::FILLED_NEW_ARRAY,
    Instruction::FILLED_NEW_ARRAY_RANGE,
    Instruction::FILL_ARRAY_DATA,
    Instruction::THROW,
    Instruction::GOTO,
    Instruction::GOTO_16,
    Instruction::GOTO_32,
    Instruction::PACKED_SWITCH,
    Instruction::SPARSE_SWITCH,
    Instruction::CMPL_FLOAT,
    Instruction::CMPG_FLOAT,
    Instruction::CMPL_DOUBLE,
    Instruction::CMPG_DOUBLE,
    Instruction::CMP_LONG,
    Instruction::IF_EQ,
    Instruction::IF_NE,
    Instruction::IF_LT,
    Instruction::IF_GE,
    Instruction::IF_GT,
    Instruction::IF_LE,
    Instruction::IF_EQZ,
    Instruction::IF_NEZ,
    Instruction::IF_LTZ,
    Instruction::IF_GEZ,
    Instruction::IF_GTZ,
    Instruction::IF_LEZ,
    Instruction::UNUSED_3E,
    Instruction::UNUSED_3F,
    Instruction::UNUSED_40,
    Instruction::UNUSED_41,
    Instruction::UNUSED_42,
    Instruction::UNUSED_43,
    Instruction::AGET,
    Instruction::AGET_WIDE,
    Instruction::AGET_OBJECT,
    Instruction::AGET_BOOLEAN,
    Instruction::AGET_BYTE,
    Instruction::AGET_CHAR,
    Instruction::AGET_SHORT,
    Instruction::APUT,
    Instruction::APUT_WIDE,
    Instruction::APUT_OBJECT,
    Instruction::APUT_BOOLEAN,
    Instruction::APUT_BYTE,
    Instruction::APUT_CHAR,
    Instruction::APUT_SHORT,
    Instruction::IGET,
    Instruction::IGET_WIDE,
    Instruction::IGET_OBJECT,
    Instruction::IGET_BOOLEAN,
    Instruction::IGET_BYTE,
    Instruction::IGET_CHAR,
    Instruction::IGET_SHORT,
    Instruction::IPUT,
    Instruction::IPUT_WIDE,
    Instruction::IPUT_OBJECT,
    Instruction::IPUT_BOOLEAN,
    Instruction::IPUT_BYTE,
    Instruction::IPUT_CHAR,
    Instruction::IPUT_SHORT,
    Instruction::SGET,
    Instruction::SGET_WIDE,
    Instruction::SGET_OBJECT,
    Instruction::SGET_BOOLEAN,
    Instruction::SGET_BYTE,
    Instruction::SGET_CHAR,
    Instruction::SGET_SHORT,
    Instruction::SPUT,
    Instruction::SPUT_WIDE,
    Instruction::SPUT_OBJECT,
    Instruction::SPUT_BOOLEAN,
    Instruction::SPUT_BYTE,
    Instruction::SPUT_CHAR,
    Instruction::SPUT_SHORT,
    Instruction::INVOKE_VIRTUAL,
    Instruction::INVOKE_SUPER,
    Instruction::INVOKE_DIRECT,
    Instruction::INVOKE_STATIC,
    Instruction::INVOKE_INTERFACE,
    Instruction::RETURN_VOID_NO_BARRIER,
    Instruction::INVOKE_VIRTUAL_RANGE,
    Instruction::INVOKE_SUPER_RANGE,
    Instruction::INVOKE_DIRECT_RANGE,
    Instruction::INVOKE_STATIC_RANGE,
    Instruction::INVOKE_INTERFACE_RANGE,
    Instruction::UNUSED_79,
    Instruction::UNUSED_7A,
    Instruction::NEG_INT,
    Instruction::NOT_INT,
    Instruction::NEG_LONG,
    Instruction::NOT_LONG,
    Instruction::NEG_FLOAT,
    Instruction::NEG_DOUBLE,
    Instruction::INT_TO_LONG,
    Instruction::INT_TO_FLOAT,
    Instruction::INT_TO_DOUBLE,
    Instruction::LONG_TO_INT,
    Instruction::LONG_TO_FLOAT,
    Instruction::LONG_TO_DOUBLE,
    Instruction::FLOAT_TO_INT,
    Instruction::FLOAT_TO_LONG,
    Instruction::FLOAT_TO_DOUBLE,
    Instruction::DOUBLE_TO_INT,
    Instruction::DOUBLE_TO_LONG,
    Instruction::DOUBLE_TO_FLOAT,
    Instruction::INT_TO_BYTE,
    Instruction::INT_TO_CHAR,
    Instruction::INT_TO_SHORT,
    Instruction::ADD_INT,
    Instruction::SUB_INT,
    Instruction::MUL_INT,
    Instruction::DIV_INT,
    Instruction::REM_INT,
    Instruction::AND_INT,
    Instruction::OR_INT,
    Instruction::XOR_INT,
    Instruction::SHL_INT,
    Instruction::SHR_INT,
    Instruction::USHR_INT,
    Instruction::ADD_LONG,
    Instruction::SUB_LONG,
    Instruction::MUL_LONG,
    Instruction::DIV_LONG,
    Instruction::REM_LONG,
    Instruction::AND_LONG,
    Instruction::OR_LONG,
    Instruction::XOR_LONG,
    Instruction::SHL_LONG,
    Instruction::SHR_LONG,
    Instruction::USHR_LONG,
    Instruction::ADD_FLOAT,
    Instruction::SUB_FLOAT,
    Instruction::MUL_FLOAT,
    Instruction::DIV_FLOAT,
    Instruction::REM_FLOAT,
    Instruction::ADD_DOUBLE,
    Instruction::SUB_DOUBLE,
    Instruction::MUL_DOUBLE,
    Instruction::DIV_DOUBLE,
    Instruction::REM_DOUBLE,
    Instruction::ADD_INT_2ADDR,
    Instruction::SUB_INT_2ADDR,
    Instruction::MUL_INT_2ADDR,
    Instruction::DIV_INT_2ADDR,
    Instruction::REM_INT_2ADDR,
    Instruction::AND_INT_2ADDR,
    Instruction::OR_INT_2ADDR,
    Instruction::XOR_INT_2ADDR,
    Instruction::SHL_INT_2ADDR,
    Instruction::SHR_INT_2ADDR,
    Instruction::USHR_INT_2ADDR,
    Instruction::ADD_LONG_2ADDR,
    Instruction::SUB_LONG_2ADDR,
    Instruction::MUL_LONG_2ADDR,
    Instruction::DIV_LONG_2ADDR,
    Instruction::REM_LONG_2ADDR,
    Instruction::AND_LONG_2ADDR,
    Instruction::OR_LONG_2ADDR,
    Instruction::XOR_LONG_2ADDR,
    Instruction::SHL_LONG_2ADDR,
    Instruction::SHR_LONG_2ADDR,
    Instruction::USHR_LONG_2ADDR,
    Instruction::ADD_FLOAT_2ADDR,
    Instruction::SUB_FLOAT_2ADDR,
    Instruction::MUL_FLOAT_2ADDR,
    Instruction::DIV_FLOAT_2ADDR,
    Instruction::REM_FLOAT_2ADDR,
    Instruction::ADD_DOUBLE_2ADDR,
    Instruction::SUB_DOUBLE_2ADDR,
    Instruction::MUL_DOUBLE_2ADDR,
    Instruction::DIV_DOUBLE_2ADDR,
    Instruction::REM_DOUBLE_2ADDR,
    Instruction::ADD_INT_LIT16,
    Instruction::RSUB_INT,
    Instruction::MUL_INT_LIT16,
    Instruction::DIV_INT_LIT16,
    Instruction::REM_INT_LIT16,
    Instruction::AND_INT_LIT16,
    Instruction::OR_INT_LIT16,
    Instruction::XOR_INT_LIT16,
    Instruction::ADD_INT_LIT8,
    Instruction::RSUB_INT_LIT8,
    Instruction::MUL_INT_LIT8,
    Instruction::DIV_INT_LIT8,
    Instruction::REM_INT_LIT8,
    Instruction::AND_INT_LIT8,
    Instruction::OR_INT_LIT8,
    Instruction::XOR_INT_LIT8,
    Instruction::SHL_INT_LIT8,
    Instruction::SHR_INT_LIT8,
    Instruction::USHR_INT_LIT8,
    Instruction::IGET_QUICK,
    Instruction::IGET_WIDE_QUICK,
    Instruction::IGET_OBJECT_QUICK,
    Instruction::IPUT_QUICK,
    Instruction::IPUT_WIDE_QUICK,
    Instruction::IPUT_OBJECT_QUICK,
    Instruction::INVOKE_VIRTUAL_QUICK,
    Instruction::INVOKE_VIRTUAL_RANGE_QUICK,
    Instruction::IPUT_BOOLEAN_QUICK,
    Instruction::IPUT_BYTE_QUICK,
    Instruction::IPUT_CHAR_QUICK,
    Instruction::IPUT_SHORT_QUICK,
    Instruction::IGET_BOOLEAN_QUICK,
    Instruction::IGET_BYTE_QUICK,
    Instruction::IGET_CHAR_QUICK,
    Instruction::IGET_SHORT_QUICK,
    Instruction::UNUSED_F3,
    Instruction::UNUSED_F4,
    Instruction::UNUSED_F5,
    Instruction::UNUSED_F6,
    Instruction::UNUSED_F7,
    Instruction::UNUSED_F8,
    Instruction::UNUSED_F9,
    Instruction::UNUSED_FA,
    Instruction::UNUSED_FB,
    Instruction::UNUSED_FC,
    Instruction::UNUSED_FD,
    Instruction::UNUSED_FE,
    Instruction::UNUSED_FF,
    // ----- ExtendedMIROpcode -----
    kMirOpPhi,
    kMirOpCopy,
    kMirOpFusedCmplFloat,
    kMirOpFusedCmpgFloat,
    kMirOpFusedCmplDouble,
    kMirOpFusedCmpgDouble,
    kMirOpFusedCmpLong,
    kMirOpNop,
    kMirOpNullCheck,
    kMirOpRangeCheck,
    kMirOpDivZeroCheck,
    kMirOpCheck,
    kMirOpSelect,
};

static int kInvokeOpcodes[] = {
    Instruction::INVOKE_VIRTUAL,
    Instruction::INVOKE_SUPER,
    Instruction::INVOKE_DIRECT,
    Instruction::INVOKE_STATIC,
    Instruction::INVOKE_INTERFACE,
    Instruction::INVOKE_VIRTUAL_RANGE,
    Instruction::INVOKE_SUPER_RANGE,
    Instruction::INVOKE_DIRECT_RANGE,
    Instruction::INVOKE_STATIC_RANGE,
    Instruction::INVOKE_INTERFACE_RANGE,
    Instruction::INVOKE_VIRTUAL_QUICK,
    Instruction::INVOKE_VIRTUAL_RANGE_QUICK,
};

// Unsupported opcodes. null can be used when everything is supported. Size of the lists is
// recorded below.
static const int* kUnsupportedOpcodes[] = {
    // 0 = kNone.
    kAllOpcodes,
    // 1 = kArm, unused (will use kThumb2).
    kAllOpcodes,
    // 2 = kArm64.
    nullptr,
    // 3 = kThumb2.
    nullptr,
    // 4 = kX86.
    nullptr,
    // 5 = kX86_64.
    nullptr,
    // 6 = kMips.
    nullptr,
    // 7 = kMips64.
    nullptr
};
static_assert(sizeof(kUnsupportedOpcodes) == 8 * sizeof(int*), "kUnsupportedOpcodes unexpected");

// Size of the arrays stored above.
static const size_t kUnsupportedOpcodesSize[] = {
    // 0 = kNone.
    arraysize(kAllOpcodes),
    // 1 = kArm, unused (will use kThumb2).
    arraysize(kAllOpcodes),
    // 2 = kArm64.
    0,
    // 3 = kThumb2.
    0,
    // 4 = kX86.
    0,
    // 5 = kX86_64.
    0,
    // 6 = kMips.
    0,
    // 7 = kMips64.
    0
};
static_assert(sizeof(kUnsupportedOpcodesSize) == 8 * sizeof(size_t),
              "kUnsupportedOpcodesSize unexpected");

// The maximum amount of Dalvik register in a method for which we will start compiling. Tries to
// avoid an abort when we need to manage more SSA registers than we can.
static constexpr size_t kMaxAllowedDalvikRegisters = INT16_MAX / 2;

static bool CanCompileShorty(const char* shorty, InstructionSet instruction_set) {
  const char* supported_types = kSupportedTypes[instruction_set];
  if (supported_types == nullptr) {
    // Everything available.
    return true;
  }

  uint32_t shorty_size = strlen(shorty);
  CHECK_GE(shorty_size, 1u);

  for (uint32_t i = 0; i < shorty_size; i++) {
    if (strchr(supported_types, shorty[i]) == nullptr) {
      return false;
    }
  }
  return true;
}

// Skip the method that we do not support currently.
bool QuickCompiler::CanCompileMethod(uint32_t method_idx, const DexFile& dex_file,
                                     CompilationUnit* cu) const {
  // This is a limitation in mir_graph. See MirGraph::SetNumSSARegs.
  if (cu->mir_graph->GetNumOfCodeAndTempVRs() > kMaxAllowedDalvikRegisters) {
    VLOG(compiler) << "Too many dalvik registers : " << cu->mir_graph->GetNumOfCodeAndTempVRs();
    return false;
  }

  // Check whether we do have limitations at all.
  if (kSupportedTypes[cu->instruction_set] == nullptr &&
      kUnsupportedOpcodesSize[cu->instruction_set] == 0U) {
    return true;
  }

  // Check if we can compile the prototype.
  const char* shorty = dex_file.GetMethodShorty(dex_file.GetMethodId(method_idx));
  if (!CanCompileShorty(shorty, cu->instruction_set)) {
    VLOG(compiler) << "Unsupported shorty : " << shorty;
    return false;
  }

  const int *unsupport_list = kUnsupportedOpcodes[cu->instruction_set];
  int unsupport_list_size = kUnsupportedOpcodesSize[cu->instruction_set];

  for (unsigned int idx = 0; idx < cu->mir_graph->GetNumBlocks(); idx++) {
    BasicBlock* bb = cu->mir_graph->GetBasicBlock(idx);
    if (bb == nullptr) continue;
    if (bb->block_type == kDead) continue;
    for (MIR* mir = bb->first_mir_insn; mir != nullptr; mir = mir->next) {
      int opcode = mir->dalvikInsn.opcode;
      // Check if we support the byte code.
      if (std::find(unsupport_list, unsupport_list + unsupport_list_size, opcode)
          != unsupport_list + unsupport_list_size) {
        if (!MIR::DecodedInstruction::IsPseudoMirOp(opcode)) {
          VLOG(compiler) << "Unsupported dalvik byte code : "
              << mir->dalvikInsn.opcode;
        } else {
          VLOG(compiler) << "Unsupported extended MIR opcode : "
              << MIRGraph::extended_mir_op_names_[opcode - kMirOpFirst];
        }
        return false;
      }
      // Check if it invokes a prototype that we cannot support.
      if (std::find(kInvokeOpcodes, kInvokeOpcodes + arraysize(kInvokeOpcodes), opcode)
          != kInvokeOpcodes + arraysize(kInvokeOpcodes)) {
        uint32_t invoke_method_idx = mir->dalvikInsn.vB;
        const char* invoke_method_shorty = dex_file.GetMethodShorty(
            dex_file.GetMethodId(invoke_method_idx));
        if (!CanCompileShorty(invoke_method_shorty, cu->instruction_set)) {
          VLOG(compiler) << "Unsupported to invoke '"
              << PrettyMethod(invoke_method_idx, dex_file)
              << "' with shorty : " << invoke_method_shorty;
          return false;
        }
      }
    }
  }
  return true;
}

void QuickCompiler::InitCompilationUnit(CompilationUnit& cu) const {
  // Disable optimizations according to instruction set.
  cu.disable_opt |= kDisabledOptimizationsPerISA[cu.instruction_set];
  if (Runtime::Current()->UseJit()) {
    // Disable these optimizations for JIT until quickened byte codes are done being implemented.
    // TODO: Find a cleaner way to do this.
    cu.disable_opt |= 1u << kLocalValueNumbering;
  }
}

void QuickCompiler::Init() {
  CHECK(GetCompilerDriver()->GetCompilerContext() == nullptr);
}

void QuickCompiler::UnInit() const {
  CHECK(GetCompilerDriver()->GetCompilerContext() == nullptr);
}

/* Default optimizer/debug setting for the compiler. */
static uint32_t kCompilerOptimizerDisableFlags = 0 |  // Disable specific optimizations
  // (1 << kLoadStoreElimination) |
  // (1 << kLoadHoisting) |
  // (1 << kSuppressLoads) |
  // (1 << kNullCheckElimination) |
  // (1 << kClassInitCheckElimination) |
  // (1 << kGlobalValueNumbering) |
  // (1 << kGvnDeadCodeElimination) |
  // (1 << kLocalValueNumbering) |
  // (1 << kPromoteRegs) |
  // (1 << kTrackLiveTemps) |
  // (1 << kSafeOptimizations) |
  // (1 << kBBOpt) |
  // (1 << kSuspendCheckElimination) |
  // (1 << kMatch) |
  // (1 << kPromoteCompilerTemps) |
  // (1 << kSuppressExceptionEdges) |
  // (1 << kSuppressMethodInlining) |
  0;

static uint32_t kCompilerDebugFlags = 0 |     // Enable debug/testing modes
  // (1 << kDebugDisplayMissingTargets) |
  // (1 << kDebugVerbose) |
  // (1 << kDebugDumpCFG) |
  // (1 << kDebugSlowFieldPath) |
  // (1 << kDebugSlowInvokePath) |
  // (1 << kDebugSlowStringPath) |
  // (1 << kDebugSlowestFieldPath) |
  // (1 << kDebugSlowestStringPath) |
  // (1 << kDebugExerciseResolveMethod) |
  // (1 << kDebugVerifyDataflow) |
  // (1 << kDebugShowMemoryUsage) |
  // (1 << kDebugShowNops) |
  // (1 << kDebugCountOpcodes) |
  // (1 << kDebugDumpCheckStats) |
  // (1 << kDebugShowSummaryMemoryUsage) |
  // (1 << kDebugShowFilterStats) |
  // (1 << kDebugTimings) |
  // (1 << kDebugCodegenDump) |
  0;

CompiledMethod* QuickCompiler::Compile(const DexFile::CodeItem* code_item,
                                       uint32_t access_flags,
                                       InvokeType invoke_type,
                                       uint16_t class_def_idx,
                                       uint32_t method_idx,
                                       jobject class_loader,
                                       const DexFile& dex_file) const {
  // TODO: check method fingerprint here to determine appropriate backend type.  Until then, use
  // build default.
  CompilerDriver* driver = GetCompilerDriver();

  VLOG(compiler) << "Compiling " << PrettyMethod(method_idx, dex_file) << "...";
  if (Compiler::IsPathologicalCase(*code_item, method_idx, dex_file)) {
    return nullptr;
  }

  if (driver->GetVerifiedMethod(&dex_file, method_idx)->HasRuntimeThrow()) {
    return nullptr;
  }

  DCHECK(driver->GetCompilerOptions().IsCompilationEnabled());

  Runtime* const runtime = Runtime::Current();
  ClassLinker* const class_linker = runtime->GetClassLinker();
  InstructionSet instruction_set = driver->GetInstructionSet();
  if (instruction_set == kArm) {
    instruction_set = kThumb2;
  }
  CompilationUnit cu(runtime->GetArenaPool(), instruction_set, driver, class_linker);
  cu.dex_file = &dex_file;
  cu.class_def_idx = class_def_idx;
  cu.method_idx = method_idx;
  cu.access_flags = access_flags;
  cu.invoke_type = invoke_type;
  cu.shorty = dex_file.GetMethodShorty(dex_file.GetMethodId(method_idx));

  CHECK((cu.instruction_set == kThumb2) ||
        (cu.instruction_set == kArm64) ||
        (cu.instruction_set == kX86) ||
        (cu.instruction_set == kX86_64) ||
        (cu.instruction_set == kMips) ||
        (cu.instruction_set == kMips64));

  // TODO: set this from command line
  constexpr bool compiler_flip_match = false;
  const std::string compiler_method_match = "";

  bool use_match = !compiler_method_match.empty();
  bool match = use_match && (compiler_flip_match ^
      (PrettyMethod(method_idx, dex_file).find(compiler_method_match) != std::string::npos));
  if (!use_match || match) {
    cu.disable_opt = kCompilerOptimizerDisableFlags;
    cu.enable_debug = kCompilerDebugFlags;
    cu.verbose = VLOG_IS_ON(compiler) ||
        (cu.enable_debug & (1 << kDebugVerbose));
  }

  if (driver->GetCompilerOptions().HasVerboseMethods()) {
    cu.verbose = driver->GetCompilerOptions().IsVerboseMethod(PrettyMethod(method_idx, dex_file));
  }

  if (cu.verbose) {
    cu.enable_debug |= (1 << kDebugCodegenDump);
  }

  /*
   * TODO: rework handling of optimization and debug flags.  Should we split out
   * MIR and backend flags?  Need command-line setting as well.
   */

  InitCompilationUnit(cu);

  cu.StartTimingSplit("BuildMIRGraph");
  cu.mir_graph.reset(new MIRGraph(&cu, &cu.arena));

  /*
   * After creation of the MIR graph, also create the code generator.
   * The reason we do this is that optimizations on the MIR graph may need to get information
   * that is only available if a CG exists.
   */
  cu.cg.reset(GetCodeGenerator(&cu, nullptr));

  /* Gathering opcode stats? */
  if (kCompilerDebugFlags & (1 << kDebugCountOpcodes)) {
    cu.mir_graph->EnableOpcodeCounting();
  }

  /* Build the raw MIR graph */
  cu.mir_graph->InlineMethod(code_item, access_flags, invoke_type, class_def_idx, method_idx,
                             class_loader, dex_file);

  if (!CanCompileMethod(method_idx, dex_file, &cu)) {
    VLOG(compiler)  << cu.instruction_set << ": Cannot compile method : "
        << PrettyMethod(method_idx, dex_file);
    cu.EndTiming();
    return nullptr;
  }

  cu.NewTimingSplit("MIROpt:CheckFilters");
  std::string skip_message;
  if (cu.mir_graph->SkipCompilation(&skip_message)) {
    VLOG(compiler) << cu.instruction_set << ": Skipping method : "
        << PrettyMethod(method_idx, dex_file) << "  Reason = " << skip_message;
    cu.EndTiming();
    return nullptr;
  }

  /* Create the pass driver and launch it */
  PassDriverMEOpts pass_driver(GetPreOptPassManager(), GetPostOptPassManager(), &cu);
  pass_driver.Launch();

  /* For non-leaf methods check if we should skip compilation when the profiler is enabled. */
  if (cu.compiler_driver->ProfilePresent()
      && !cu.mir_graph->MethodIsLeaf()
      && cu.mir_graph->SkipCompilationByName(PrettyMethod(method_idx, dex_file))) {
    cu.EndTiming();
    return nullptr;
  }

  if (cu.enable_debug & (1 << kDebugDumpCheckStats)) {
    cu.mir_graph->DumpCheckStats();
  }

  if (kCompilerDebugFlags & (1 << kDebugCountOpcodes)) {
    cu.mir_graph->ShowOpcodeStats();
  }

  /* Reassociate sreg names with original Dalvik vreg names. */
  cu.mir_graph->RemapRegLocations();

  /* Free Arenas from the cu.arena_stack for reuse by the cu.arena in the codegen. */
  if (cu.enable_debug & (1 << kDebugShowMemoryUsage)) {
    if (cu.arena_stack.PeakBytesAllocated() > 1 * 1024 * 1024) {
      MemStats stack_stats(cu.arena_stack.GetPeakStats());
      LOG(INFO) << PrettyMethod(method_idx, dex_file) << " " << Dumpable<MemStats>(stack_stats);
    }
  }
  cu.arena_stack.Reset();

  CompiledMethod* result = nullptr;

  if (cu.mir_graph->PuntToInterpreter()) {
    VLOG(compiler) << cu.instruction_set << ": Punted method to interpreter: "
        << PrettyMethod(method_idx, dex_file);
    cu.EndTiming();
    return nullptr;
  }

  cu.cg->Materialize();

  cu.NewTimingSplit("Dedupe");  /* deduping takes up the vast majority of time in GetCompiledMethod(). */
  result = cu.cg->GetCompiledMethod();
  cu.NewTimingSplit("Cleanup");

  if (result) {
    VLOG(compiler) << cu.instruction_set << ": Compiled " << PrettyMethod(method_idx, dex_file);
  } else {
    VLOG(compiler) << cu.instruction_set << ": Deferred " << PrettyMethod(method_idx, dex_file);
  }

  if (cu.enable_debug & (1 << kDebugShowMemoryUsage)) {
    if (cu.arena.BytesAllocated() > (1 * 1024 *1024)) {
      MemStats mem_stats(cu.arena.GetMemStats());
      LOG(INFO) << PrettyMethod(method_idx, dex_file) << " " << Dumpable<MemStats>(mem_stats);
    }
  }

  if (cu.enable_debug & (1 << kDebugShowSummaryMemoryUsage)) {
    LOG(INFO) << "MEMINFO " << cu.arena.BytesAllocated() << " " << cu.mir_graph->GetNumBlocks()
                    << " " << PrettyMethod(method_idx, dex_file);
  }

  cu.EndTiming();
  driver->GetTimingsLogger()->AddLogger(cu.timings);
  return result;
}

CompiledMethod* QuickCompiler::JniCompile(uint32_t access_flags,
                                          uint32_t method_idx,
                                          const DexFile& dex_file) const {
  return ArtQuickJniCompileMethod(GetCompilerDriver(), access_flags, method_idx, dex_file);
}

uintptr_t QuickCompiler::GetEntryPointOf(ArtMethod* method) const {
  return reinterpret_cast<uintptr_t>(method->GetEntryPointFromQuickCompiledCodePtrSize(
      InstructionSetPointerSize(GetCompilerDriver()->GetInstructionSet())));
}

Mir2Lir* QuickCompiler::GetCodeGenerator(CompilationUnit* cu, void* compilation_unit) {
  UNUSED(compilation_unit);
  Mir2Lir* mir_to_lir = nullptr;
  switch (cu->instruction_set) {
    case kThumb2:
      mir_to_lir = ArmCodeGenerator(cu, cu->mir_graph.get(), &cu->arena);
      break;
    case kArm64:
      mir_to_lir = Arm64CodeGenerator(cu, cu->mir_graph.get(), &cu->arena);
      break;
    case kMips:
      // Fall-through.
    case kMips64:
      mir_to_lir = MipsCodeGenerator(cu, cu->mir_graph.get(), &cu->arena);
      break;
    case kX86:
      // Fall-through.
    case kX86_64:
      mir_to_lir = X86CodeGenerator(cu, cu->mir_graph.get(), &cu->arena);
      break;
    default:
      LOG(FATAL) << "Unexpected instruction set: " << cu->instruction_set;
  }

  /* The number of compiler temporaries depends on backend so set it up now if possible */
  if (mir_to_lir) {
    size_t max_temps = mir_to_lir->GetMaxPossibleCompilerTemps();
    bool set_max = cu->mir_graph->SetMaxAvailableNonSpecialCompilerTemps(max_temps);
    CHECK(set_max);
  }
  return mir_to_lir;
}

QuickCompiler::QuickCompiler(CompilerDriver* driver) : Compiler(driver, 100) {
  const auto& compiler_options = driver->GetCompilerOptions();
  auto* pass_manager_options = compiler_options.GetPassManagerOptions();
  pre_opt_pass_manager_.reset(new PassManager(*pass_manager_options));
  CHECK(pre_opt_pass_manager_.get() != nullptr);
  PassDriverMEOpts::SetupPasses(pre_opt_pass_manager_.get());
  pre_opt_pass_manager_->CreateDefaultPassList();
  if (pass_manager_options->GetPrintPassOptions()) {
    PassDriverMEOpts::PrintPassOptions(pre_opt_pass_manager_.get());
  }
  // TODO: Different options for pre vs post opts?
  post_opt_pass_manager_.reset(new PassManager(PassManagerOptions()));
  CHECK(post_opt_pass_manager_.get() != nullptr);
  PassDriverMEPostOpt::SetupPasses(post_opt_pass_manager_.get());
  post_opt_pass_manager_->CreateDefaultPassList();
  if (pass_manager_options->GetPrintPassOptions()) {
    PassDriverMEPostOpt::PrintPassOptions(post_opt_pass_manager_.get());
  }
}

QuickCompiler::~QuickCompiler() {
}

Compiler* CreateQuickCompiler(CompilerDriver* driver) {
  return QuickCompiler::Create(driver);
}

Compiler* QuickCompiler::Create(CompilerDriver* driver) {
  return new QuickCompiler(driver);
}

}  // namespace art
