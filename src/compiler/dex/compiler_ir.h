/*
 * Copyright (C) 2011 The Android Open Source Project
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

#ifndef ART_SRC_COMPILER_DEX_COMPILER_IR_H_
#define ART_SRC_COMPILER_DEX_COMPILER_IR_H_

#include <vector>

#include <llvm/Module.h>

#include "compiler/dex/quick/codegen.h"
#include "compiler/driver/compiler_driver.h"
#include "compiler/driver/dex_compilation_unit.h"
#include "compiler/llvm/intrinsic_helper.h"
#include "compiler/llvm/ir_builder.h"
#include "compiler_enums.h"
#include "compiler_utility.h"
#include "dex_instruction.h"
#include "safe_map.h"

namespace art {

//TODO: replace these macros
#define SLOW_FIELD_PATH (cu->enable_debug & (1 << kDebugSlowFieldPath))
#define SLOW_INVOKE_PATH (cu->enable_debug & (1 << kDebugSlowInvokePath))
#define SLOW_STRING_PATH (cu->enable_debug & (1 << kDebugSlowStringPath))
#define SLOW_TYPE_PATH (cu->enable_debug & (1 << kDebugSlowTypePath))
#define EXERCISE_SLOWEST_STRING_PATH (cu->enable_debug & \
  (1 << kDebugSlowestStringPath))

// Minimum field size to contain Dalvik v_reg number.
#define VREG_NUM_WIDTH 16

struct ArenaBitVector;
struct LIR;
class LLVMInfo;
namespace llvm {
class LlvmCompilationUnit;
}  // namespace llvm

struct PromotionMap {
  RegLocationType core_location:3;
  uint8_t core_reg;
  RegLocationType fp_location:3;
  uint8_t FpReg;
  bool first_in_pair;
};

struct RegLocation {
  RegLocationType location:3;
  unsigned wide:1;
  unsigned defined:1;   // Do we know the type?
  unsigned is_const:1;  // Constant, value in mir_graph->constant_values[].
  unsigned fp:1;        // Floating point?
  unsigned core:1;      // Non-floating point?
  unsigned ref:1;       // Something GC cares about.
  unsigned high_word:1; // High word of pair?
  unsigned home:1;      // Does this represent the home location?
  uint8_t low_reg;      // First physical register.
  uint8_t high_reg;     // 2nd physical register (if wide).
  int32_t s_reg_low;    // SSA name for low Dalvik word.
  int32_t orig_sreg;    // TODO: remove after Bitcode gen complete
                        // and consolodate usage w/ s_reg_low.
};

struct CompilerTemp {
  int s_reg;
  ArenaBitVector* bv;
};

struct CallInfo {
  int num_arg_words;    // Note: word count, not arg count.
  RegLocation* args;    // One for each word of arguments.
  RegLocation result;   // Eventual target of MOVE_RESULT.
  int opt_flags;
  InvokeType type;
  uint32_t dex_idx;
  uint32_t index;       // Method idx for invokes, type idx for FilledNewArray.
  uintptr_t direct_code;
  uintptr_t direct_method;
  RegLocation target;    // Target of following move_result.
  bool skip_this;
  bool is_range;
  int offset;            // Dalvik offset.
};

 /*
 * Data structure tracking the mapping between a Dalvik register (pair) and a
 * native register (pair). The idea is to reuse the previously loaded value
 * if possible, otherwise to keep the value in a native register as long as
 * possible.
 */
struct RegisterInfo {
  int reg;                    // Reg number
  bool in_use;                // Has it been allocated?
  bool is_temp;               // Can allocate as temp?
  bool pair;                  // Part of a register pair?
  int partner;                // If pair, other reg of pair.
  bool live;                  // Is there an associated SSA name?
  bool dirty;                 // If live, is it dirty?
  int s_reg;                  // Name of live value.
  LIR *def_start;             // Starting inst in last def sequence.
  LIR *def_end;               // Ending inst in last def sequence.
};

struct RegisterPool {
  int num_core_regs;
  RegisterInfo *core_regs;
  int next_core_reg;
  int num_fp_regs;
  RegisterInfo *FPRegs;
  int next_fp_reg;
};

#define INVALID_SREG (-1)
#define INVALID_VREG (0xFFFFU)
#define INVALID_REG (0xFF)
#define INVALID_OFFSET (0xDEADF00FU)

/* SSA encodings for special registers */
#define SSA_METHOD_BASEREG (-2)
/* First compiler temp basereg, grows smaller */
#define SSA_CTEMP_BASEREG (SSA_METHOD_BASEREG - 1)

/*
 * Some code patterns cause the generation of excessively large
 * methods - in particular initialization sequences.  There isn't much
 * benefit in optimizing these methods, and the cost can be very high.
 * We attempt to identify these cases, and avoid performing most dataflow
 * analysis.  Two thresholds are used - one for known initializers and one
 * for everything else.
 */
#define MANY_BLOCKS_INITIALIZER 1000 /* Threshold for switching dataflow off */
#define MANY_BLOCKS 4000 /* Non-initializer threshold */

// Utility macros to traverse the LIR list.
#define NEXT_LIR(lir) (lir->next)
#define PREV_LIR(lir) (lir->prev)

// Defines for alias_info (tracks Dalvik register references).
#define DECODE_ALIAS_INFO_REG(X)        (X & 0xffff)
#define DECODE_ALIAS_INFO_WIDE_FLAG     (0x80000000)
#define DECODE_ALIAS_INFO_WIDE(X)       ((X & DECODE_ALIAS_INFO_WIDE_FLAG) ? 1 : 0)
#define ENCODE_ALIAS_INFO(REG, ISWIDE)  (REG | (ISWIDE ? DECODE_ALIAS_INFO_WIDE_FLAG : 0))

// Common resource macros.
#define ENCODE_CCODE            (1ULL << kCCode)
#define ENCODE_FP_STATUS        (1ULL << kFPStatus)

// Abstract memory locations.
#define ENCODE_DALVIK_REG       (1ULL << kDalvikReg)
#define ENCODE_LITERAL          (1ULL << kLiteral)
#define ENCODE_HEAP_REF         (1ULL << kHeapRef)
#define ENCODE_MUST_NOT_ALIAS   (1ULL << kMustNotAlias)

#define ENCODE_ALL              (~0ULL)
#define ENCODE_MEM              (ENCODE_DALVIK_REG | ENCODE_LITERAL | \
                                 ENCODE_HEAP_REF | ENCODE_MUST_NOT_ALIAS)

#define is_pseudo_opcode(opcode) (static_cast<int>(opcode) < 0)

struct LIR {
  int offset;               // Offset of this instruction.
  int dalvik_offset;        // Offset of Dalvik opcode.
  LIR* next;
  LIR* prev;
  LIR* target;
  int opcode;
  int operands[5];          // [0..4] = [dest, src1, src2, extra, extra2].
  struct {
    bool is_nop:1;          // LIR is optimized away.
    bool pcRelFixup:1;      // May need pc-relative fixup.
    unsigned int size:5;    // Note: size is in bytes.
    unsigned int unused:25;
  } flags;
  int alias_info;           // For Dalvik register & litpool disambiguation.
  uint64_t use_mask;        // Resource mask for use.
  uint64_t def_mask;        // Resource mask for def.
};

extern const char* extended_mir_op_names[kMirOpLast - kMirOpFirst];

struct SSARepresentation;

#define MIR_IGNORE_NULL_CHECK           (1 << kMIRIgnoreNullCheck)
#define MIR_NULL_CHECK_ONLY             (1 << kMIRNullCheckOnly)
#define MIR_IGNORE_RANGE_CHECK          (1 << kMIRIgnoreRangeCheck)
#define MIR_RANGE_CHECK_ONLY            (1 << kMIRRangeCheckOnly)
#define MIR_INLINED                     (1 << kMIRInlined)
#define MIR_INLINED_PRED                (1 << kMIRInlinedPred)
#define MIR_CALLEE                      (1 << kMIRCallee)
#define MIR_IGNORE_SUSPEND_CHECK        (1 << kMIRIgnoreSuspendCheck)
#define MIR_DUP                         (1 << kMIRDup)

struct Checkstats {
  int null_checks;
  int null_checks_eliminated;
  int range_checks;
  int range_checks_eliminated;
};

struct MIR {
  DecodedInstruction dalvikInsn;
  unsigned int width;
  unsigned int offset;
  int m_unit_index;               // From which method was this MIR included
  MIR* prev;
  MIR* next;
  SSARepresentation* ssa_rep;
  int optimization_flags;
  union {
    // Establish link between two halves of throwing instructions.
    MIR* throw_insn;
    // Saved opcode for NOP'd MIRs
    Instruction::Code original_opcode;
  } meta;
};

struct BasicBlockDataFlow {
  ArenaBitVector* use_v;
  ArenaBitVector* def_v;
  ArenaBitVector* live_in_v;
  ArenaBitVector* phi_v;
  int* vreg_to_ssa_map;
  ArenaBitVector* ending_null_check_v;
};

struct SSARepresentation {
  int num_uses;
  int* uses;
  bool* fp_use;
  int num_defs;
  int* defs;
  bool* fp_def;
};

struct BasicBlock {
  int id;
  int dfs_id;
  bool visited;
  bool hidden;
  bool catch_entry;
  bool explicit_throw;
  bool conditional_branch;
  bool terminated_by_return;        // Block ends with a Dalvik return opcode.
  bool dominates_return;            // Is a member of return extended basic block.
  uint16_t start_offset;
  uint16_t nesting_depth;
  BBType block_type;
  MIR* first_mir_insn;
  MIR* last_mir_insn;
  BasicBlock* fall_through;
  BasicBlock* taken;
  BasicBlock* i_dom;                // Immediate dominator.
  BasicBlockDataFlow* data_flow_info;
  GrowableList* predecessors;
  ArenaBitVector* dominators;
  ArenaBitVector* i_dominated;      // Set nodes being immediately dominated.
  ArenaBitVector* dom_frontier;     // Dominance frontier.
  struct {                          // For one-to-many successors like.
    BlockListType block_list_type;  // switch and exception handling.
    GrowableList blocks;
  } successor_block_list;
};

/*
 * The "blocks" field in "successor_block_list" points to an array of
 * elements with the type "SuccessorBlockInfo".
 * For catch blocks, key is type index for the exception.
 * For swtich blocks, key is the case value.
 */
struct SuccessorBlockInfo {
  BasicBlock* block;
  int key;
};

struct LoopAnalysis;
struct RegisterPool;
struct ArenaMemBlock;
struct Memstats;
class MIRGraph;
class Codegen;

#define NOTVISITED (-1)

struct CompilationUnit {
  CompilationUnit()
    : compiler_driver(NULL),
      class_linker(NULL),
      dex_file(NULL),
      class_loader(NULL),
      class_def_idx(0),
      method_idx(0),
      code_item(NULL),
      access_flags(0),
      invoke_type(kDirect),
      shorty(NULL),
      disable_opt(0),
      enable_debug(0),
      verbose(false),
      gen_bitcode(false),
      disable_dataflow(false),
      instruction_set(kNone),
      num_dalvik_registers(0),
      insns(NULL),
      num_ins(0),
      num_outs(0),
      num_regs(0),
      num_core_spills(0),
      num_fp_spills(0),
      num_compiler_temps(0),
      frame_size(0),
      core_spill_mask(0),
      fp_spill_mask(0),
      attributes(0),
      compiler_flip_match(false),
      arena_head(NULL),
      current_arena(NULL),
      num_arena_blocks(0),
      mstats(NULL),
      checkstats(NULL),
      mir_graph(NULL),
      cg(NULL),
      live_sreg(0),
      llvm_info(NULL),
      context(NULL),
      module(NULL),
      func(NULL),
      intrinsic_helper(NULL),
      irb(NULL),
      placeholder_bb(NULL),
      entry_bb(NULL),
      entryTarget_bb(NULL),
      temp_name(0),
      first_lir_insn(NULL),
      last_lir_insn(NULL),
      literal_list(NULL),
      method_literal_list(NULL),
      code_literal_list(NULL),
      data_offset(0),
      total_size(0),
      reg_pool(NULL),
      reg_location(NULL),
      promotion_map(NULL),
      method_sreg(0),
      block_label_list(NULL),
      current_dalvik_offset(0)
 {}
  /*
   * Fields needed/generated by common frontend and generally used throughout
   * the compiler.
  */
  CompilerDriver* compiler_driver;
  ClassLinker* class_linker;           // Linker to resolve fields and methods.
  const DexFile* dex_file;             // DexFile containing the method being compiled.
  jobject class_loader;                // compiling method's class loader.
  uint32_t class_def_idx;              // compiling method's defining class definition index.
  uint32_t method_idx;                 // compiling method's index into method_ids of DexFile.
  const DexFile::CodeItem* code_item;  // compiling method's DexFile code_item.
  uint32_t access_flags;               // compiling method's access flags.
  InvokeType invoke_type;              // compiling method's invocation type.
  const char* shorty;                  // compiling method's shorty.
  uint32_t disable_opt;                // opt_control_vector flags.
  uint32_t enable_debug;               // debugControlVector flags.
  std::vector<uint8_t> code_buffer;
  bool verbose;
  std::vector<uint32_t> combined_mapping_table;
  std::vector<uint32_t> core_vmap_table;
  std::vector<uint32_t> fp_vmap_table;
  std::vector<uint8_t> native_gc_map;
  bool gen_bitcode;
  bool disable_dataflow;               // Skip dataflow analysis if possible
  InstructionSet instruction_set;

  // CLEANUP: much of this info available elsewhere.  Go to the original source?
  int num_dalvik_registers;        // method->registers_size.
  const uint16_t* insns;
  /*
   * Frame layout details.
   * NOTE: for debug support it will be necessary to add a structure
   * to map the Dalvik virtual registers to the promoted registers.
   * NOTE: "num" fields are in 4-byte words, "Size" and "Offset" in bytes.
   */
  int num_ins;
  int num_outs;
  int num_regs;            // Unlike num_dalvik_registers, does not include ins.
  int num_core_spills;
  int num_fp_spills;
  int num_compiler_temps;
  int frame_size;
  unsigned int core_spill_mask;
  unsigned int fp_spill_mask;
  unsigned int attributes;
  // If non-empty, apply optimizer/debug flags only to matching methods.
  std::string compiler_method_match;
  // Flips sense of compiler_method_match - apply flags if doesn't match.
  bool compiler_flip_match;
  ArenaMemBlock* arena_head;
  ArenaMemBlock* current_arena;
  int num_arena_blocks;
  Memstats* mstats;
  Checkstats* checkstats;
  UniquePtr<MIRGraph> mir_graph;   // MIR container.
  UniquePtr<Codegen> cg;           // Target-specific codegen.
  /*
   * Sanity checking for the register temp tracking.  The same ssa
   * name should never be associated with one temp register per
   * instruction compilation.
   */
  int live_sreg;

  // Fields for Portable
  llvm::LlvmCompilationUnit* llvm_compilation_unit;
 /*
  * Fields needed by GBC creation.  Candidates for moving to a new MIR to
  * llvm bitcode class.
  */
  LLVMInfo* llvm_info;
  std::string symbol;
  ::llvm::LLVMContext* context;
  ::llvm::Module* module;
  ::llvm::Function* func;
  art::llvm::IntrinsicHelper* intrinsic_helper;
  art::llvm::IRBuilder* irb;
  ::llvm::BasicBlock* placeholder_bb;
  ::llvm::BasicBlock* entry_bb;
  ::llvm::BasicBlock* entryTarget_bb;

  std::string bitcode_filename;
  GrowableList llvm_values;
  int32_t temp_name;
  SafeMap<int32_t, ::llvm::BasicBlock*> id_to_block_map;  // block id -> llvm bb.

 /*
  * Fields needed by the Quick backend.  Candidates for moving to a new
  * QuickBackend class.
  */
  LIR* first_lir_insn;
  LIR* last_lir_insn;
  LIR* literal_list;                   // Constants.
  LIR* method_literal_list;            // Method literals requiring patching.
  LIR* code_literal_list;              // Code literals requiring patching.
  int data_offset;                     // starting offset of literal pool.
  int total_size;                      // header + code size.
  RegisterPool* reg_pool;
  // Map SSA names to location.
  RegLocation* reg_location;
  // Keep track of Dalvik v_reg to physical register mappings.
  PromotionMap* promotion_map;
  // SSA name for Method*.
  int method_sreg;
  RegLocation method_loc;          // Describes location of method*.
  GrowableList throw_launchpads;
  GrowableList suspend_launchpads;
  GrowableList intrinsic_launchpads;
  GrowableList compiler_temps;
  LIR* block_label_list;
  /*
   * TODO: The code generation utilities don't have a built-in
   * mechanism to propagate the original Dalvik opcode address to the
   * associated generated instructions.  For the trace compiler, this wasn't
   * necessary because the interpreter handled all throws and debugging
   * requests.  For now we'll handle this by placing the Dalvik offset
   * in the CompilationUnit struct before codegen for each instruction.
   * The low-level LIR creation utilites will pull it from here.  Rework this.
   */
  int current_dalvik_offset;
  GrowableList switch_tables;
  GrowableList fill_array_data;
  SafeMap<unsigned int, unsigned int> block_id_map; // Block collapse lookup cache.
  SafeMap<unsigned int, LIR*> boundary_map; // boundary lookup cache.
  /*
   * Holds mapping from native PC to dex PC for safepoints where we may deoptimize.
   * Native PC is on the return address of the safepointed operation.  Dex PC is for
   * the instruction being executed at the safepoint.
   */
  std::vector<uint32_t> pc2dexMappingTable;
  /*
   * Holds mapping from Dex PC to native PC for catch entry points.  Native PC and Dex PC
   * immediately preceed the instruction.
   */
  std::vector<uint32_t> dex2pcMappingTable;
};

// TODO: move this
int SRegToVReg(const CompilationUnit* cu, int ssa_reg);


}  // namespace art

#endif // ART_SRC_COMPILER_DEX_COMPILER_IR_H_
