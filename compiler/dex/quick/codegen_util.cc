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

#include "mir_to_lir-inl.h"

#include "base/bit_vector-inl.h"
#include "dex/mir_graph.h"
#include "driver/compiler_driver.h"
#include "driver/compiler_options.h"
#include "driver/dex_compilation_unit.h"
#include "dex_file-inl.h"
#include "gc_map.h"
#include "gc_map_builder.h"
#include "mapping_table.h"
#include "dex/quick/dex_file_method_inliner.h"
#include "dex/quick/dex_file_to_method_inliner_map.h"
#include "dex/verification_results.h"
#include "dex/verified_method.h"
#include "utils/dex_cache_arrays_layout-inl.h"
#include "verifier/dex_gc_map.h"
#include "verifier/method_verifier.h"
#include "vmap_table.h"

namespace art {

namespace {

/* Dump a mapping table */
template <typename It>
void DumpMappingTable(const char* table_name, const char* descriptor, const char* name,
                      const Signature& signature, uint32_t size, It first) {
  if (size != 0) {
    std::string line(StringPrintf("\n  %s %s%s_%s_table[%u] = {", table_name,
                     descriptor, name, signature.ToString().c_str(), size));
    std::replace(line.begin(), line.end(), ';', '_');
    LOG(INFO) << line;
    for (uint32_t i = 0; i != size; ++i) {
      line = StringPrintf("    {0x%05x, 0x%04x},", first.NativePcOffset(), first.DexPc());
      ++first;
      LOG(INFO) << line;
    }
    LOG(INFO) <<"  };\n\n";
  }
}

}  // anonymous namespace

bool Mir2Lir::IsInexpensiveConstant(RegLocation rl_src) {
  bool res = false;
  if (rl_src.is_const) {
    if (rl_src.wide) {
      // For wide registers, check whether we're the high partner. In that case we need to switch
      // to the lower one for the correct value.
      if (rl_src.high_word) {
        rl_src.high_word = false;
        rl_src.s_reg_low--;
        rl_src.orig_sreg--;
      }
      if (rl_src.fp) {
        res = InexpensiveConstantDouble(mir_graph_->ConstantValueWide(rl_src));
      } else {
        res = InexpensiveConstantLong(mir_graph_->ConstantValueWide(rl_src));
      }
    } else {
      if (rl_src.fp) {
        res = InexpensiveConstantFloat(mir_graph_->ConstantValue(rl_src));
      } else {
        res = InexpensiveConstantInt(mir_graph_->ConstantValue(rl_src));
      }
    }
  }
  return res;
}

void Mir2Lir::MarkSafepointPC(LIR* inst) {
  DCHECK(!inst->flags.use_def_invalid);
  inst->u.m.def_mask = &kEncodeAll;
  LIR* safepoint_pc = NewLIR0(kPseudoSafepointPC);
  DCHECK(safepoint_pc->u.m.def_mask->Equals(kEncodeAll));
  DCHECK(current_mir_ != nullptr || (current_dalvik_offset_ == 0 && safepoints_.empty()));
  safepoints_.emplace_back(safepoint_pc, current_mir_);
}

void Mir2Lir::MarkSafepointPCAfter(LIR* after) {
  DCHECK(!after->flags.use_def_invalid);
  after->u.m.def_mask = &kEncodeAll;
  // As NewLIR0 uses Append, we need to create the LIR by hand.
  LIR* safepoint_pc = RawLIR(current_dalvik_offset_, kPseudoSafepointPC);
  if (after->next == nullptr) {
    DCHECK_EQ(after, last_lir_insn_);
    AppendLIR(safepoint_pc);
  } else {
    InsertLIRAfter(after, safepoint_pc);
  }
  DCHECK(safepoint_pc->u.m.def_mask->Equals(kEncodeAll));
  DCHECK(current_mir_ != nullptr || (current_dalvik_offset_ == 0 && safepoints_.empty()));
  safepoints_.emplace_back(safepoint_pc, current_mir_);
}

/* Remove a LIR from the list. */
void Mir2Lir::UnlinkLIR(LIR* lir) {
  if (UNLIKELY(lir == first_lir_insn_)) {
    first_lir_insn_ = lir->next;
    if (lir->next != nullptr) {
      lir->next->prev = nullptr;
    } else {
      DCHECK(lir->next == nullptr);
      DCHECK(lir == last_lir_insn_);
      last_lir_insn_ = nullptr;
    }
  } else if (lir == last_lir_insn_) {
    last_lir_insn_ = lir->prev;
    lir->prev->next = nullptr;
  } else if ((lir->prev != nullptr) && (lir->next != nullptr)) {
    lir->prev->next = lir->next;
    lir->next->prev = lir->prev;
  }
}

/* Convert an instruction to a NOP */
void Mir2Lir::NopLIR(LIR* lir) {
  lir->flags.is_nop = true;
  if (!cu_->verbose) {
    UnlinkLIR(lir);
  }
}

void Mir2Lir::SetMemRefType(LIR* lir, bool is_load, int mem_type) {
  DCHECK(GetTargetInstFlags(lir->opcode) & (IS_LOAD | IS_STORE));
  DCHECK(!lir->flags.use_def_invalid);
  // TODO: Avoid the extra Arena allocation!
  const ResourceMask** mask_ptr;
  ResourceMask mask;
  if (is_load) {
    mask_ptr = &lir->u.m.use_mask;
  } else {
    mask_ptr = &lir->u.m.def_mask;
  }
  mask = **mask_ptr;
  /* Clear out the memref flags */
  mask.ClearBits(kEncodeMem);
  /* ..and then add back the one we need */
  switch (mem_type) {
    case ResourceMask::kLiteral:
      DCHECK(is_load);
      mask.SetBit(ResourceMask::kLiteral);
      break;
    case ResourceMask::kDalvikReg:
      mask.SetBit(ResourceMask::kDalvikReg);
      break;
    case ResourceMask::kHeapRef:
      mask.SetBit(ResourceMask::kHeapRef);
      break;
    case ResourceMask::kMustNotAlias:
      /* Currently only loads can be marked as kMustNotAlias */
      DCHECK(!(GetTargetInstFlags(lir->opcode) & IS_STORE));
      mask.SetBit(ResourceMask::kMustNotAlias);
      break;
    default:
      LOG(FATAL) << "Oat: invalid memref kind - " << mem_type;
  }
  *mask_ptr = mask_cache_.GetMask(mask);
}

/*
 * Mark load/store instructions that access Dalvik registers through the stack.
 */
void Mir2Lir::AnnotateDalvikRegAccess(LIR* lir, int reg_id, bool is_load,
                                      bool is64bit) {
  DCHECK((is_load ? lir->u.m.use_mask : lir->u.m.def_mask)->Intersection(kEncodeMem).Equals(
      kEncodeDalvikReg));

  /*
   * Store the Dalvik register id in alias_info. Mark the MSB if it is a 64-bit
   * access.
   */
  lir->flags.alias_info = ENCODE_ALIAS_INFO(reg_id, is64bit);
}

/*
 * Debugging macros
 */
#define DUMP_RESOURCE_MASK(X)

/* Pretty-print a LIR instruction */
void Mir2Lir::DumpLIRInsn(LIR* lir, unsigned char* base_addr) {
  int offset = lir->offset;
  int dest = lir->operands[0];
  const bool dump_nop = (cu_->enable_debug & (1 << kDebugShowNops));

  /* Handle pseudo-ops individually, and all regular insns as a group */
  switch (lir->opcode) {
    case kPseudoPrologueBegin:
      LOG(INFO) << "-------- PrologueBegin";
      break;
    case kPseudoPrologueEnd:
      LOG(INFO) << "-------- PrologueEnd";
      break;
    case kPseudoEpilogueBegin:
      LOG(INFO) << "-------- EpilogueBegin";
      break;
    case kPseudoEpilogueEnd:
      LOG(INFO) << "-------- EpilogueEnd";
      break;
    case kPseudoBarrier:
      LOG(INFO) << "-------- BARRIER";
      break;
    case kPseudoEntryBlock:
      LOG(INFO) << "-------- entry offset: 0x" << std::hex << dest;
      break;
    case kPseudoDalvikByteCodeBoundary:
      if (lir->operands[0] == 0) {
         // NOTE: only used for debug listings.
         lir->operands[0] = WrapPointer(ArenaStrdup("No instruction string"));
      }
      LOG(INFO) << "-------- dalvik offset: 0x" << std::hex
                << lir->dalvik_offset << " @ "
                << UnwrapPointer<char>(lir->operands[0]);
      break;
    case kPseudoExitBlock:
      LOG(INFO) << "-------- exit offset: 0x" << std::hex << dest;
      break;
    case kPseudoPseudoAlign4:
      LOG(INFO) << reinterpret_cast<uintptr_t>(base_addr) + offset << " (0x" << std::hex
                << offset << "): .align4";
      break;
    case kPseudoEHBlockLabel:
      LOG(INFO) << "Exception_Handling:";
      break;
    case kPseudoTargetLabel:
    case kPseudoNormalBlockLabel:
      LOG(INFO) << "L" << reinterpret_cast<void*>(lir) << ":";
      break;
    case kPseudoThrowTarget:
      LOG(INFO) << "LT" << reinterpret_cast<void*>(lir) << ":";
      break;
    case kPseudoIntrinsicRetry:
      LOG(INFO) << "IR" << reinterpret_cast<void*>(lir) << ":";
      break;
    case kPseudoSuspendTarget:
      LOG(INFO) << "LS" << reinterpret_cast<void*>(lir) << ":";
      break;
    case kPseudoSafepointPC:
      LOG(INFO) << "LsafepointPC_0x" << std::hex << lir->offset << "_" << lir->dalvik_offset << ":";
      break;
    case kPseudoExportedPC:
      LOG(INFO) << "LexportedPC_0x" << std::hex << lir->offset << "_" << lir->dalvik_offset << ":";
      break;
    case kPseudoCaseLabel:
      LOG(INFO) << "LC" << reinterpret_cast<void*>(lir) << ": Case target 0x"
                << std::hex << lir->operands[0] << "|" << std::dec <<
        lir->operands[0];
      break;
    default:
      if (lir->flags.is_nop && !dump_nop) {
        break;
      } else {
        std::string op_name(BuildInsnString(GetTargetInstName(lir->opcode),
                                               lir, base_addr));
        std::string op_operands(BuildInsnString(GetTargetInstFmt(lir->opcode),
                                                    lir, base_addr));
        LOG(INFO) << StringPrintf("%5p|0x%02x: %-9s%s%s",
                                  base_addr + offset,
                                  lir->dalvik_offset,
                                  op_name.c_str(), op_operands.c_str(),
                                  lir->flags.is_nop ? "(nop)" : "");
      }
      break;
  }

  if (lir->u.m.use_mask && (!lir->flags.is_nop || dump_nop)) {
    DUMP_RESOURCE_MASK(DumpResourceMask(lir, *lir->u.m.use_mask, "use"));
  }
  if (lir->u.m.def_mask && (!lir->flags.is_nop || dump_nop)) {
    DUMP_RESOURCE_MASK(DumpResourceMask(lir, *lir->u.m.def_mask, "def"));
  }
}

void Mir2Lir::DumpPromotionMap() {
  uint32_t num_regs = mir_graph_->GetNumOfCodeAndTempVRs();
  for (uint32_t i = 0; i < num_regs; i++) {
    PromotionMap v_reg_map = promotion_map_[i];
    std::string buf;
    if (v_reg_map.fp_location == kLocPhysReg) {
      StringAppendF(&buf, " : s%d", RegStorage::RegNum(v_reg_map.fp_reg));
    }

    std::string buf3;
    if (i < mir_graph_->GetNumOfCodeVRs()) {
      StringAppendF(&buf3, "%02d", i);
    } else if (i == mir_graph_->GetNumOfCodeVRs()) {
      buf3 = "Method*";
    } else {
      uint32_t diff = i - mir_graph_->GetNumOfCodeVRs();
      StringAppendF(&buf3, "ct%d", diff);
    }

    LOG(INFO) << StringPrintf("V[%s] -> %s%d%s", buf3.c_str(),
                              v_reg_map.core_location == kLocPhysReg ?
                              "r" : "SP+", v_reg_map.core_location == kLocPhysReg ?
                              v_reg_map.core_reg : SRegOffset(i),
                              buf.c_str());
  }
}

void Mir2Lir::UpdateLIROffsets() {
  // Only used for code listings.
  size_t offset = 0;
  for (LIR* lir = first_lir_insn_; lir != nullptr; lir = lir->next) {
    lir->offset = offset;
    if (!lir->flags.is_nop && !IsPseudoLirOp(lir->opcode)) {
      offset += GetInsnSize(lir);
    } else if (lir->opcode == kPseudoPseudoAlign4) {
      offset += (offset & 0x2);
    }
  }
}

void Mir2Lir::MarkGCCard(int opt_flags, RegStorage val_reg, RegStorage tgt_addr_reg) {
  DCHECK(val_reg.Valid());
  DCHECK_EQ(val_reg.Is64Bit(), cu_->target64);
  if ((opt_flags & MIR_STORE_NON_NULL_VALUE) != 0) {
    UnconditionallyMarkGCCard(tgt_addr_reg);
  } else {
    LIR* branch_over = OpCmpImmBranch(kCondEq, val_reg, 0, nullptr);
    UnconditionallyMarkGCCard(tgt_addr_reg);
    LIR* target = NewLIR0(kPseudoTargetLabel);
    branch_over->target = target;
  }
}

/* Dump instructions and constant pool contents */
void Mir2Lir::CodegenDump() {
  LOG(INFO) << "Dumping LIR insns for "
            << PrettyMethod(cu_->method_idx, *cu_->dex_file);
  LIR* lir_insn;
  int insns_size = mir_graph_->GetNumDalvikInsns();

  LOG(INFO) << "Regs (excluding ins) : " << mir_graph_->GetNumOfLocalCodeVRs();
  LOG(INFO) << "Ins          : " << mir_graph_->GetNumOfInVRs();
  LOG(INFO) << "Outs         : " << mir_graph_->GetNumOfOutVRs();
  LOG(INFO) << "CoreSpills       : " << num_core_spills_;
  LOG(INFO) << "FPSpills       : " << num_fp_spills_;
  LOG(INFO) << "CompilerTemps    : " << mir_graph_->GetNumUsedCompilerTemps();
  LOG(INFO) << "Frame size       : " << frame_size_;
  LOG(INFO) << "code size is " << total_size_ <<
    " bytes, Dalvik size is " << insns_size * 2;
  LOG(INFO) << "expansion factor: "
            << static_cast<float>(total_size_) / static_cast<float>(insns_size * 2);
  DumpPromotionMap();
  UpdateLIROffsets();
  for (lir_insn = first_lir_insn_; lir_insn != nullptr; lir_insn = lir_insn->next) {
    DumpLIRInsn(lir_insn, 0);
  }
  for (lir_insn = literal_list_; lir_insn != nullptr; lir_insn = lir_insn->next) {
    LOG(INFO) << StringPrintf("%x (%04x): .word (%#x)", lir_insn->offset, lir_insn->offset,
                              lir_insn->operands[0]);
  }

  const DexFile::MethodId& method_id =
      cu_->dex_file->GetMethodId(cu_->method_idx);
  const Signature signature = cu_->dex_file->GetMethodSignature(method_id);
  const char* name = cu_->dex_file->GetMethodName(method_id);
  const char* descriptor(cu_->dex_file->GetMethodDeclaringClassDescriptor(method_id));

  // Dump mapping tables
  if (!encoded_mapping_table_.empty()) {
    MappingTable table(&encoded_mapping_table_[0]);
    DumpMappingTable("PC2Dex_MappingTable", descriptor, name, signature,
                     table.PcToDexSize(), table.PcToDexBegin());
    DumpMappingTable("Dex2PC_MappingTable", descriptor, name, signature,
                     table.DexToPcSize(), table.DexToPcBegin());
  }
}

/*
 * Search the existing constants in the literal pool for an exact or close match
 * within specified delta (greater or equal to 0).
 */
LIR* Mir2Lir::ScanLiteralPool(LIR* data_target, int value, unsigned int delta) {
  while (data_target) {
    if ((static_cast<unsigned>(value - data_target->operands[0])) <= delta)
      return data_target;
    data_target = data_target->next;
  }
  return nullptr;
}

/* Search the existing constants in the literal pool for an exact wide match */
LIR* Mir2Lir::ScanLiteralPoolWide(LIR* data_target, int val_lo, int val_hi) {
  bool lo_match = false;
  LIR* lo_target = nullptr;
  while (data_target) {
    if (lo_match && (data_target->operands[0] == val_hi)) {
      // Record high word in case we need to expand this later.
      lo_target->operands[1] = val_hi;
      return lo_target;
    }
    lo_match = false;
    if (data_target->operands[0] == val_lo) {
      lo_match = true;
      lo_target = data_target;
    }
    data_target = data_target->next;
  }
  return nullptr;
}

/* Search the existing constants in the literal pool for an exact method match */
LIR* Mir2Lir::ScanLiteralPoolMethod(LIR* data_target, const MethodReference& method) {
  while (data_target) {
    if (static_cast<uint32_t>(data_target->operands[0]) == method.dex_method_index &&
        UnwrapPointer<DexFile>(data_target->operands[1]) == method.dex_file) {
      return data_target;
    }
    data_target = data_target->next;
  }
  return nullptr;
}

/* Search the existing constants in the literal pool for an exact class match */
LIR* Mir2Lir::ScanLiteralPoolClass(LIR* data_target, const DexFile& dex_file, uint32_t type_idx) {
  while (data_target) {
    if (static_cast<uint32_t>(data_target->operands[0]) == type_idx &&
        UnwrapPointer<DexFile>(data_target->operands[1]) == &dex_file) {
      return data_target;
    }
    data_target = data_target->next;
  }
  return nullptr;
}

/*
 * The following are building blocks to insert constants into the pool or
 * instruction streams.
 */

/* Add a 32-bit constant to the constant pool */
LIR* Mir2Lir::AddWordData(LIR* *constant_list_p, int value) {
  /* Add the constant to the literal pool */
  if (constant_list_p) {
    LIR* new_value = static_cast<LIR*>(arena_->Alloc(sizeof(LIR), kArenaAllocData));
    new_value->operands[0] = value;
    new_value->next = *constant_list_p;
    *constant_list_p = new_value;
    estimated_native_code_size_ += sizeof(value);
    return new_value;
  }
  return nullptr;
}

/* Add a 64-bit constant to the constant pool or mixed with code */
LIR* Mir2Lir::AddWideData(LIR* *constant_list_p, int val_lo, int val_hi) {
  AddWordData(constant_list_p, val_hi);
  return AddWordData(constant_list_p, val_lo);
}

/**
 * @brief Push a compressed reference which needs patching at link/patchoat-time.
 * @details This needs to be kept consistent with the code which actually does the patching in
 *   oat_writer.cc and in the patchoat tool.
 */
static void PushUnpatchedReference(CodeBuffer* buf) {
  // Note that we can safely initialize the patches to zero. The code deduplication mechanism takes
  // the patches into account when determining whether two pieces of codes are functionally
  // equivalent.
  Push32(buf, UINT32_C(0));
}

static void AlignBuffer(CodeBuffer* buf, size_t offset) {
  DCHECK_LE(buf->size(), offset);
  buf->insert(buf->end(), offset - buf->size(), 0u);
}

/* Write the literal pool to the output stream */
void Mir2Lir::InstallLiteralPools() {
  AlignBuffer(&code_buffer_, data_offset_);
  LIR* data_lir = literal_list_;
  while (data_lir != nullptr) {
    Push32(&code_buffer_, data_lir->operands[0]);
    data_lir = NEXT_LIR(data_lir);
  }
  // TODO: patches_.reserve() as needed.
  // Push code and method literals, record offsets for the compiler to patch.
  data_lir = code_literal_list_;
  while (data_lir != nullptr) {
    uint32_t target_method_idx = data_lir->operands[0];
    const DexFile* target_dex_file = UnwrapPointer<DexFile>(data_lir->operands[1]);
    patches_.push_back(LinkerPatch::CodePatch(code_buffer_.size(),
                                              target_dex_file, target_method_idx));
    PushUnpatchedReference(&code_buffer_);
    data_lir = NEXT_LIR(data_lir);
  }
  data_lir = method_literal_list_;
  while (data_lir != nullptr) {
    uint32_t target_method_idx = data_lir->operands[0];
    const DexFile* target_dex_file = UnwrapPointer<DexFile>(data_lir->operands[1]);
    patches_.push_back(LinkerPatch::MethodPatch(code_buffer_.size(),
                                                target_dex_file, target_method_idx));
    PushUnpatchedReference(&code_buffer_);
    data_lir = NEXT_LIR(data_lir);
  }
  // Push class literals.
  data_lir = class_literal_list_;
  while (data_lir != nullptr) {
    uint32_t target_type_idx = data_lir->operands[0];
    const DexFile* class_dex_file = UnwrapPointer<DexFile>(data_lir->operands[1]);
    patches_.push_back(LinkerPatch::TypePatch(code_buffer_.size(),
                                              class_dex_file, target_type_idx));
    PushUnpatchedReference(&code_buffer_);
    data_lir = NEXT_LIR(data_lir);
  }
}

/* Write the switch tables to the output stream */
void Mir2Lir::InstallSwitchTables() {
  for (Mir2Lir::SwitchTable* tab_rec : switch_tables_) {
    AlignBuffer(&code_buffer_, tab_rec->offset);
    /*
     * For Arm, our reference point is the address of the bx
     * instruction that does the launch, so we have to subtract
     * the auto pc-advance.  For other targets the reference point
     * is a label, so we can use the offset as-is.
     */
    int bx_offset = INVALID_OFFSET;
    switch (cu_->instruction_set) {
      case kThumb2:
        DCHECK(tab_rec->anchor->flags.fixup != kFixupNone);
        bx_offset = tab_rec->anchor->offset + 4;
        break;
      case kX86_64:
        // RIP relative to switch table.
        bx_offset = tab_rec->offset;
        break;
      case kX86:
      case kArm64:
      case kMips:
      case kMips64:
        bx_offset = tab_rec->anchor->offset;
        break;
      default: LOG(FATAL) << "Unexpected instruction set: " << cu_->instruction_set;
    }
    if (cu_->verbose) {
      LOG(INFO) << "Switch table for offset 0x" << std::hex << bx_offset;
    }
    if (tab_rec->table[0] == Instruction::kSparseSwitchSignature) {
      DCHECK(tab_rec->switch_mir != nullptr);
      BasicBlock* bb = mir_graph_->GetBasicBlock(tab_rec->switch_mir->bb);
      DCHECK(bb != nullptr);
      int elems = 0;
      for (SuccessorBlockInfo* successor_block_info : bb->successor_blocks) {
        int key = successor_block_info->key;
        int target = successor_block_info->block;
        LIR* boundary_lir = InsertCaseLabel(target, key);
        DCHECK(boundary_lir != nullptr);
        int disp = boundary_lir->offset - bx_offset;
        Push32(&code_buffer_, key);
        Push32(&code_buffer_, disp);
        if (cu_->verbose) {
          LOG(INFO) << "  Case[" << elems << "] key: 0x"
                    << std::hex << key << ", disp: 0x"
                    << std::hex << disp;
        }
        elems++;
      }
      DCHECK_EQ(elems, tab_rec->table[1]);
    } else {
      DCHECK_EQ(static_cast<int>(tab_rec->table[0]),
                static_cast<int>(Instruction::kPackedSwitchSignature));
      DCHECK(tab_rec->switch_mir != nullptr);
      BasicBlock* bb = mir_graph_->GetBasicBlock(tab_rec->switch_mir->bb);
      DCHECK(bb != nullptr);
      int elems = 0;
      int low_key = s4FromSwitchData(&tab_rec->table[2]);
      for (SuccessorBlockInfo* successor_block_info : bb->successor_blocks) {
        int key = successor_block_info->key;
        DCHECK_EQ(elems + low_key, key);
        int target = successor_block_info->block;
        LIR* boundary_lir = InsertCaseLabel(target, key);
        DCHECK(boundary_lir != nullptr);
        int disp = boundary_lir->offset - bx_offset;
        Push32(&code_buffer_, disp);
        if (cu_->verbose) {
          LOG(INFO) << "  Case[" << elems << "] disp: 0x"
                    << std::hex << disp;
        }
        elems++;
      }
      DCHECK_EQ(elems, tab_rec->table[1]);
    }
  }
}

/* Write the fill array dta to the output stream */
void Mir2Lir::InstallFillArrayData() {
  for (Mir2Lir::FillArrayData* tab_rec : fill_array_data_) {
    AlignBuffer(&code_buffer_, tab_rec->offset);
    for (int i = 0; i < (tab_rec->size + 1) / 2; i++) {
      code_buffer_.push_back(tab_rec->table[i] & 0xFF);
      code_buffer_.push_back((tab_rec->table[i] >> 8) & 0xFF);
    }
  }
}

static int AssignLiteralOffsetCommon(LIR* lir, CodeOffset offset) {
  for (; lir != nullptr; lir = lir->next) {
    lir->offset = offset;
    offset += 4;
  }
  return offset;
}

static int AssignLiteralPointerOffsetCommon(LIR* lir, CodeOffset offset,
                                            unsigned int element_size) {
  // Align to natural pointer size.
  offset = RoundUp(offset, element_size);
  for (; lir != nullptr; lir = lir->next) {
    lir->offset = offset;
    offset += element_size;
  }
  return offset;
}

// Make sure we have a code address for every declared catch entry
bool Mir2Lir::VerifyCatchEntries() {
  MappingTable table(&encoded_mapping_table_[0]);
  std::vector<uint32_t> dex_pcs;
  dex_pcs.reserve(table.DexToPcSize());
  for (auto it = table.DexToPcBegin(), end = table.DexToPcEnd(); it != end; ++it) {
    dex_pcs.push_back(it.DexPc());
  }
  // Sort dex_pcs, so that we can quickly check it against the ordered mir_graph_->catches_.
  std::sort(dex_pcs.begin(), dex_pcs.end());

  bool success = true;
  auto it = dex_pcs.begin(), end = dex_pcs.end();
  for (uint32_t dex_pc : mir_graph_->catches_) {
    while (it != end && *it < dex_pc) {
      LOG(INFO) << "Unexpected catch entry @ dex pc 0x" << std::hex << *it;
      ++it;
      success = false;
    }
    if (it == end || *it > dex_pc) {
      LOG(INFO) << "Missing native PC for catch entry @ 0x" << std::hex << dex_pc;
      success = false;
    } else {
      ++it;
    }
  }
  if (!success) {
    LOG(INFO) << "Bad dex2pcMapping table in " << PrettyMethod(cu_->method_idx, *cu_->dex_file);
    LOG(INFO) << "Entries @ decode: " << mir_graph_->catches_.size() << ", Entries in table: "
              << table.DexToPcSize();
  }
  return success;
}


void Mir2Lir::CreateMappingTables() {
  bool generate_src_map = cu_->compiler_driver->GetCompilerOptions().GetGenerateDebugInfo();

  uint32_t pc2dex_data_size = 0u;
  uint32_t pc2dex_entries = 0u;
  uint32_t pc2dex_offset = 0u;
  uint32_t pc2dex_dalvik_offset = 0u;
  uint32_t pc2dex_src_entries = 0u;
  uint32_t dex2pc_data_size = 0u;
  uint32_t dex2pc_entries = 0u;
  uint32_t dex2pc_offset = 0u;
  uint32_t dex2pc_dalvik_offset = 0u;
  for (LIR* tgt_lir = first_lir_insn_; tgt_lir != nullptr; tgt_lir = NEXT_LIR(tgt_lir)) {
    pc2dex_src_entries++;
    if (!tgt_lir->flags.is_nop && (tgt_lir->opcode == kPseudoSafepointPC)) {
      pc2dex_entries += 1;
      DCHECK(pc2dex_offset <= tgt_lir->offset);
      pc2dex_data_size += UnsignedLeb128Size(tgt_lir->offset - pc2dex_offset);
      pc2dex_data_size += SignedLeb128Size(static_cast<int32_t>(tgt_lir->dalvik_offset) -
                                           static_cast<int32_t>(pc2dex_dalvik_offset));
      pc2dex_offset = tgt_lir->offset;
      pc2dex_dalvik_offset = tgt_lir->dalvik_offset;
    }
    if (!tgt_lir->flags.is_nop && (tgt_lir->opcode == kPseudoExportedPC)) {
      dex2pc_entries += 1;
      DCHECK(dex2pc_offset <= tgt_lir->offset);
      dex2pc_data_size += UnsignedLeb128Size(tgt_lir->offset - dex2pc_offset);
      dex2pc_data_size += SignedLeb128Size(static_cast<int32_t>(tgt_lir->dalvik_offset) -
                                           static_cast<int32_t>(dex2pc_dalvik_offset));
      dex2pc_offset = tgt_lir->offset;
      dex2pc_dalvik_offset = tgt_lir->dalvik_offset;
    }
  }

  if (generate_src_map) {
    src_mapping_table_.reserve(pc2dex_src_entries);
  }

  uint32_t total_entries = pc2dex_entries + dex2pc_entries;
  uint32_t hdr_data_size = UnsignedLeb128Size(total_entries) + UnsignedLeb128Size(pc2dex_entries);
  uint32_t data_size = hdr_data_size + pc2dex_data_size + dex2pc_data_size;
  encoded_mapping_table_.resize(data_size);
  uint8_t* write_pos = &encoded_mapping_table_[0];
  write_pos = EncodeUnsignedLeb128(write_pos, total_entries);
  write_pos = EncodeUnsignedLeb128(write_pos, pc2dex_entries);
  DCHECK_EQ(static_cast<size_t>(write_pos - &encoded_mapping_table_[0]), hdr_data_size);
  uint8_t* write_pos2 = write_pos + pc2dex_data_size;

  bool is_in_prologue_or_epilogue = false;
  pc2dex_offset = 0u;
  pc2dex_dalvik_offset = 0u;
  dex2pc_offset = 0u;
  dex2pc_dalvik_offset = 0u;
  for (LIR* tgt_lir = first_lir_insn_; tgt_lir != nullptr; tgt_lir = NEXT_LIR(tgt_lir)) {
    if (generate_src_map && !tgt_lir->flags.is_nop && tgt_lir->opcode >= 0) {
      if (!is_in_prologue_or_epilogue) {
        src_mapping_table_.push_back(SrcMapElem({tgt_lir->offset,
                static_cast<int32_t>(tgt_lir->dalvik_offset)}));
      }
    }
    if (!tgt_lir->flags.is_nop && (tgt_lir->opcode == kPseudoSafepointPC)) {
      DCHECK(pc2dex_offset <= tgt_lir->offset);
      write_pos = EncodeUnsignedLeb128(write_pos, tgt_lir->offset - pc2dex_offset);
      write_pos = EncodeSignedLeb128(write_pos, static_cast<int32_t>(tgt_lir->dalvik_offset) -
                                     static_cast<int32_t>(pc2dex_dalvik_offset));
      pc2dex_offset = tgt_lir->offset;
      pc2dex_dalvik_offset = tgt_lir->dalvik_offset;
    }
    if (!tgt_lir->flags.is_nop && (tgt_lir->opcode == kPseudoExportedPC)) {
      DCHECK(dex2pc_offset <= tgt_lir->offset);
      write_pos2 = EncodeUnsignedLeb128(write_pos2, tgt_lir->offset - dex2pc_offset);
      write_pos2 = EncodeSignedLeb128(write_pos2, static_cast<int32_t>(tgt_lir->dalvik_offset) -
                                      static_cast<int32_t>(dex2pc_dalvik_offset));
      dex2pc_offset = tgt_lir->offset;
      dex2pc_dalvik_offset = tgt_lir->dalvik_offset;
    }
    if (tgt_lir->opcode == kPseudoPrologueBegin || tgt_lir->opcode == kPseudoEpilogueBegin) {
      is_in_prologue_or_epilogue = true;
    }
    if (tgt_lir->opcode == kPseudoPrologueEnd || tgt_lir->opcode == kPseudoEpilogueEnd) {
      is_in_prologue_or_epilogue = false;
    }
  }
  DCHECK_EQ(static_cast<size_t>(write_pos - &encoded_mapping_table_[0]),
            hdr_data_size + pc2dex_data_size);
  DCHECK_EQ(static_cast<size_t>(write_pos2 - &encoded_mapping_table_[0]), data_size);

  if (kIsDebugBuild) {
    CHECK(VerifyCatchEntries());

    // Verify the encoded table holds the expected data.
    MappingTable table(&encoded_mapping_table_[0]);
    CHECK_EQ(table.TotalSize(), total_entries);
    CHECK_EQ(table.PcToDexSize(), pc2dex_entries);
    auto it = table.PcToDexBegin();
    auto it2 = table.DexToPcBegin();
    for (LIR* tgt_lir = first_lir_insn_; tgt_lir != nullptr; tgt_lir = NEXT_LIR(tgt_lir)) {
      if (!tgt_lir->flags.is_nop && (tgt_lir->opcode == kPseudoSafepointPC)) {
        CHECK_EQ(tgt_lir->offset, it.NativePcOffset());
        CHECK_EQ(tgt_lir->dalvik_offset, it.DexPc());
        ++it;
      }
      if (!tgt_lir->flags.is_nop && (tgt_lir->opcode == kPseudoExportedPC)) {
        CHECK_EQ(tgt_lir->offset, it2.NativePcOffset());
        CHECK_EQ(tgt_lir->dalvik_offset, it2.DexPc());
        ++it2;
      }
    }
    CHECK(it == table.PcToDexEnd());
    CHECK(it2 == table.DexToPcEnd());
  }
}

void Mir2Lir::CreateNativeGcMap() {
  if (UNLIKELY((cu_->disable_opt & (1u << kPromoteRegs)) != 0u)) {
    // If we're not promoting to physical registers, it's safe to use the verifier's notion of
    // references. (We disable register promotion when type inference finds a type conflict and
    // in that the case we defer to the verifier to avoid using the compiler's conflicting info.)
    CreateNativeGcMapWithoutRegisterPromotion();
    return;
  }

  ArenaBitVector* references = new (arena_) ArenaBitVector(arena_, mir_graph_->GetNumSSARegs(),
                                                           false);

  // Calculate max native offset and max reference vreg.
  MIR* prev_mir = nullptr;
  int max_ref_vreg = -1;
  CodeOffset max_native_offset = 0u;
  for (const auto& entry : safepoints_) {
    uint32_t native_offset = entry.first->offset;
    max_native_offset = std::max(max_native_offset, native_offset);
    MIR* mir = entry.second;
    UpdateReferenceVRegs(mir, prev_mir, references);
    max_ref_vreg = std::max(max_ref_vreg, references->GetHighestBitSet());
    prev_mir = mir;
  }

#if defined(BYTE_ORDER) && (BYTE_ORDER == LITTLE_ENDIAN)
  static constexpr bool kLittleEndian = true;
#else
  static constexpr bool kLittleEndian = false;
#endif

  // Build the GC map.
  uint32_t reg_width = static_cast<uint32_t>((max_ref_vreg + 8) / 8);
  GcMapBuilder native_gc_map_builder(&native_gc_map_,
                                     safepoints_.size(),
                                     max_native_offset, reg_width);
  if (kLittleEndian) {
    for (const auto& entry : safepoints_) {
      uint32_t native_offset = entry.first->offset;
      MIR* mir = entry.second;
      UpdateReferenceVRegs(mir, prev_mir, references);
      // For little-endian, the bytes comprising the bit vector's raw storage are what we need.
      native_gc_map_builder.AddEntry(native_offset,
                                     reinterpret_cast<const uint8_t*>(references->GetRawStorage()));
      prev_mir = mir;
    }
  } else {
    ArenaVector<uint8_t> references_buffer(arena_->Adapter());
    references_buffer.resize(reg_width);
    for (const auto& entry : safepoints_) {
      uint32_t native_offset = entry.first->offset;
      MIR* mir = entry.second;
      UpdateReferenceVRegs(mir, prev_mir, references);
      // Big-endian or unknown endianness, manually translate the bit vector data.
      const auto* raw_storage = references->GetRawStorage();
      for (size_t i = 0; i != reg_width; ++i) {
        references_buffer[i] = static_cast<uint8_t>(
            raw_storage[i / sizeof(raw_storage[0])] >> (8u * (i % sizeof(raw_storage[0]))));
      }
      native_gc_map_builder.AddEntry(native_offset, &references_buffer[0]);
      prev_mir = mir;
    }
  }
}

void Mir2Lir::CreateNativeGcMapWithoutRegisterPromotion() {
  DCHECK(!encoded_mapping_table_.empty());
  MappingTable mapping_table(&encoded_mapping_table_[0]);
  uint32_t max_native_offset = 0;
  for (auto it = mapping_table.PcToDexBegin(), end = mapping_table.PcToDexEnd(); it != end; ++it) {
    uint32_t native_offset = it.NativePcOffset();
    if (native_offset > max_native_offset) {
      max_native_offset = native_offset;
    }
  }
  MethodReference method_ref(cu_->dex_file, cu_->method_idx);
  const std::vector<uint8_t>& gc_map_raw =
      mir_graph_->GetCurrentDexCompilationUnit()->GetVerifiedMethod()->GetDexGcMap();
  verifier::DexPcToReferenceMap dex_gc_map(&(gc_map_raw)[0]);
  DCHECK_EQ(gc_map_raw.size(), dex_gc_map.RawSize());
  // Compute native offset to references size.
  GcMapBuilder native_gc_map_builder(&native_gc_map_,
                                     mapping_table.PcToDexSize(),
                                     max_native_offset, dex_gc_map.RegWidth());

  for (auto it = mapping_table.PcToDexBegin(), end = mapping_table.PcToDexEnd(); it != end; ++it) {
    uint32_t native_offset = it.NativePcOffset();
    uint32_t dex_pc = it.DexPc();
    const uint8_t* references = dex_gc_map.FindBitMap(dex_pc, false);
    CHECK(references != nullptr) << "Missing ref for dex pc 0x" << std::hex << dex_pc <<
        ": " << PrettyMethod(cu_->method_idx, *cu_->dex_file);
    native_gc_map_builder.AddEntry(native_offset, references);
  }

  // Maybe not necessary, but this could help prevent errors where we access the verified method
  // after it has been deleted.
  mir_graph_->GetCurrentDexCompilationUnit()->ClearVerifiedMethod();
}

/* Determine the offset of each literal field */
int Mir2Lir::AssignLiteralOffset(CodeOffset offset) {
  offset = AssignLiteralOffsetCommon(literal_list_, offset);
  constexpr unsigned int ptr_size = sizeof(uint32_t);
  static_assert(ptr_size >= sizeof(mirror::HeapReference<mirror::Object>),
                "Pointer size cannot hold a heap reference");
  offset = AssignLiteralPointerOffsetCommon(code_literal_list_, offset, ptr_size);
  offset = AssignLiteralPointerOffsetCommon(method_literal_list_, offset, ptr_size);
  offset = AssignLiteralPointerOffsetCommon(class_literal_list_, offset, ptr_size);
  return offset;
}

int Mir2Lir::AssignSwitchTablesOffset(CodeOffset offset) {
  for (Mir2Lir::SwitchTable* tab_rec : switch_tables_) {
    tab_rec->offset = offset;
    if (tab_rec->table[0] == Instruction::kSparseSwitchSignature) {
      offset += tab_rec->table[1] * (sizeof(int) * 2);
    } else {
      DCHECK_EQ(static_cast<int>(tab_rec->table[0]),
                static_cast<int>(Instruction::kPackedSwitchSignature));
      offset += tab_rec->table[1] * sizeof(int);
    }
  }
  return offset;
}

int Mir2Lir::AssignFillArrayDataOffset(CodeOffset offset) {
  for (Mir2Lir::FillArrayData* tab_rec : fill_array_data_) {
    tab_rec->offset = offset;
    offset += tab_rec->size;
    // word align
    offset = RoundUp(offset, 4);
  }
  return offset;
}

/*
 * Insert a kPseudoCaseLabel at the beginning of the Dalvik
 * offset vaddr if pretty-printing, otherise use the standard block
 * label.  The selected label will be used to fix up the case
 * branch table during the assembly phase.  All resource flags
 * are set to prevent code motion.  KeyVal is just there for debugging.
 */
LIR* Mir2Lir::InsertCaseLabel(uint32_t bbid, int keyVal) {
  LIR* boundary_lir = &block_label_list_[bbid];
  LIR* res = boundary_lir;
  if (cu_->verbose) {
    // Only pay the expense if we're pretty-printing.
    LIR* new_label = static_cast<LIR*>(arena_->Alloc(sizeof(LIR), kArenaAllocLIR));
    BasicBlock* bb = mir_graph_->GetBasicBlock(bbid);
    DCHECK(bb != nullptr);
    new_label->dalvik_offset = bb->start_offset;
    new_label->opcode = kPseudoCaseLabel;
    new_label->operands[0] = keyVal;
    new_label->flags.fixup = kFixupLabel;
    DCHECK(!new_label->flags.use_def_invalid);
    new_label->u.m.def_mask = &kEncodeAll;
    InsertLIRAfter(boundary_lir, new_label);
  }
  return res;
}

void Mir2Lir::DumpSparseSwitchTable(const uint16_t* table) {
  /*
   * Sparse switch data format:
   *  ushort ident = 0x0200   magic value
   *  ushort size       number of entries in the table; > 0
   *  int keys[size]      keys, sorted low-to-high; 32-bit aligned
   *  int targets[size]     branch targets, relative to switch opcode
   *
   * Total size is (2+size*4) 16-bit code units.
   */
  uint16_t ident = table[0];
  int entries = table[1];
  const int32_t* keys = reinterpret_cast<const int32_t*>(&table[2]);
  const int32_t* targets = &keys[entries];
  LOG(INFO) <<  "Sparse switch table - ident:0x" << std::hex << ident
            << ", entries: " << std::dec << entries;
  for (int i = 0; i < entries; i++) {
    LOG(INFO) << "  Key[" << keys[i] << "] -> 0x" << std::hex << targets[i];
  }
}

void Mir2Lir::DumpPackedSwitchTable(const uint16_t* table) {
  /*
   * Packed switch data format:
   *  ushort ident = 0x0100   magic value
   *  ushort size       number of entries in the table
   *  int first_key       first (and lowest) switch case value
   *  int targets[size]     branch targets, relative to switch opcode
   *
   * Total size is (4+size*2) 16-bit code units.
   */
  uint16_t ident = table[0];
  const int32_t* targets = reinterpret_cast<const int32_t*>(&table[4]);
  int entries = table[1];
  int low_key = s4FromSwitchData(&table[2]);
  LOG(INFO) << "Packed switch table - ident:0x" << std::hex << ident
            << ", entries: " << std::dec << entries << ", low_key: " << low_key;
  for (int i = 0; i < entries; i++) {
    LOG(INFO) << "  Key[" << (i + low_key) << "] -> 0x" << std::hex
              << targets[i];
  }
}

/* Set up special LIR to mark a Dalvik byte-code instruction start for pretty printing */
void Mir2Lir::MarkBoundary(DexOffset offset, const char* inst_str) {
  UNUSED(offset);
  // NOTE: only used for debug listings.
  NewLIR1(kPseudoDalvikByteCodeBoundary, WrapPointer(ArenaStrdup(inst_str)));
}

// Convert relation of src1/src2 to src2/src1
ConditionCode Mir2Lir::FlipComparisonOrder(ConditionCode before) {
  ConditionCode res;
  switch (before) {
    case kCondEq: res = kCondEq; break;
    case kCondNe: res = kCondNe; break;
    case kCondLt: res = kCondGt; break;
    case kCondGt: res = kCondLt; break;
    case kCondLe: res = kCondGe; break;
    case kCondGe: res = kCondLe; break;
    default:
      LOG(FATAL) << "Unexpected ccode " << before;
      UNREACHABLE();
  }
  return res;
}

ConditionCode Mir2Lir::NegateComparison(ConditionCode before) {
  ConditionCode res;
  switch (before) {
    case kCondEq: res = kCondNe; break;
    case kCondNe: res = kCondEq; break;
    case kCondLt: res = kCondGe; break;
    case kCondGt: res = kCondLe; break;
    case kCondLe: res = kCondGt; break;
    case kCondGe: res = kCondLt; break;
    default:
      LOG(FATAL) << "Unexpected ccode " << before;
      UNREACHABLE();
  }
  return res;
}

// TODO: move to mir_to_lir.cc
Mir2Lir::Mir2Lir(CompilationUnit* cu, MIRGraph* mir_graph, ArenaAllocator* arena)
    : literal_list_(nullptr),
      method_literal_list_(nullptr),
      class_literal_list_(nullptr),
      code_literal_list_(nullptr),
      first_fixup_(nullptr),
      arena_(arena),
      cu_(cu),
      mir_graph_(mir_graph),
      switch_tables_(arena->Adapter(kArenaAllocSwitchTable)),
      fill_array_data_(arena->Adapter(kArenaAllocFillArrayData)),
      tempreg_info_(arena->Adapter()),
      reginfo_map_(arena->Adapter()),
      pointer_storage_(arena->Adapter()),
      data_offset_(0),
      total_size_(0),
      block_label_list_(nullptr),
      promotion_map_(nullptr),
      current_dalvik_offset_(0),
      current_mir_(nullptr),
      estimated_native_code_size_(0),
      reg_pool_(nullptr),
      live_sreg_(0),
      code_buffer_(mir_graph->GetArena()->Adapter()),
      encoded_mapping_table_(mir_graph->GetArena()->Adapter()),
      core_vmap_table_(mir_graph->GetArena()->Adapter()),
      fp_vmap_table_(mir_graph->GetArena()->Adapter()),
      native_gc_map_(mir_graph->GetArena()->Adapter()),
      patches_(mir_graph->GetArena()->Adapter()),
      num_core_spills_(0),
      num_fp_spills_(0),
      frame_size_(0),
      core_spill_mask_(0),
      fp_spill_mask_(0),
      first_lir_insn_(nullptr),
      last_lir_insn_(nullptr),
      slow_paths_(arena->Adapter(kArenaAllocSlowPaths)),
      mem_ref_type_(ResourceMask::kHeapRef),
      mask_cache_(arena),
      safepoints_(arena->Adapter()),
      dex_cache_arrays_layout_(cu->compiler_driver->GetDexCacheArraysLayout(cu->dex_file)),
      pc_rel_temp_(nullptr),
      dex_cache_arrays_min_offset_(std::numeric_limits<uint32_t>::max()),
      cfi_(&last_lir_insn_,
           cu->compiler_driver->GetCompilerOptions().GetGenerateDebugInfo(),
           arena),
      in_to_reg_storage_mapping_(arena) {
  switch_tables_.reserve(4);
  fill_array_data_.reserve(4);
  tempreg_info_.reserve(20);
  reginfo_map_.reserve(RegStorage::kMaxRegs);
  pointer_storage_.reserve(128);
  slow_paths_.reserve(32);
  // Reserve pointer id 0 for null.
  size_t null_idx = WrapPointer<void>(nullptr);
  DCHECK_EQ(null_idx, 0U);
}

void Mir2Lir::Materialize() {
  cu_->NewTimingSplit("RegisterAllocation");
  CompilerInitializeRegAlloc();  // Needs to happen after SSA naming

  /* Allocate Registers using simple local allocation scheme */
  SimpleRegAlloc();

  /* First try the custom light codegen for special cases. */
  DCHECK(cu_->compiler_driver->GetMethodInlinerMap() != nullptr);
  bool special_worked = cu_->compiler_driver->GetMethodInlinerMap()->GetMethodInliner(cu_->dex_file)
      ->GenSpecial(this, cu_->method_idx);

  /* Take normal path for converting MIR to LIR only if the special codegen did not succeed. */
  if (special_worked == false) {
    MethodMIR2LIR();
  }

  /* Method is not empty */
  if (first_lir_insn_) {
    /* Convert LIR into machine code. */
    AssembleLIR();

    if ((cu_->enable_debug & (1 << kDebugCodegenDump)) != 0) {
      CodegenDump();
    }
  }
}

CompiledMethod* Mir2Lir::GetCompiledMethod() {
  // Combine vmap tables - core regs, then fp regs - into vmap_table.
  Leb128EncodingVector vmap_encoder;
  if (frame_size_ > 0) {
    // Prefix the encoded data with its size.
    size_t size = core_vmap_table_.size() + 1 /* marker */ + fp_vmap_table_.size();
    vmap_encoder.Reserve(size + 1u);  // All values are likely to be one byte in ULEB128 (<128).
    vmap_encoder.PushBackUnsigned(size);
    // Core regs may have been inserted out of order - sort first.
    std::sort(core_vmap_table_.begin(), core_vmap_table_.end());
    for (size_t i = 0 ; i < core_vmap_table_.size(); ++i) {
      // Copy, stripping out the phys register sort key.
      vmap_encoder.PushBackUnsigned(
          ~(-1 << VREG_NUM_WIDTH) & (core_vmap_table_[i] + VmapTable::kEntryAdjustment));
    }
    // Push a marker to take place of lr.
    vmap_encoder.PushBackUnsigned(VmapTable::kAdjustedFpMarker);
    if (cu_->instruction_set == kThumb2) {
      // fp regs already sorted.
      for (uint32_t i = 0; i < fp_vmap_table_.size(); i++) {
        vmap_encoder.PushBackUnsigned(fp_vmap_table_[i] + VmapTable::kEntryAdjustment);
      }
    } else {
      // For other platforms regs may have been inserted out of order - sort first.
      std::sort(fp_vmap_table_.begin(), fp_vmap_table_.end());
      for (size_t i = 0 ; i < fp_vmap_table_.size(); ++i) {
        // Copy, stripping out the phys register sort key.
        vmap_encoder.PushBackUnsigned(
            ~(-1 << VREG_NUM_WIDTH) & (fp_vmap_table_[i] + VmapTable::kEntryAdjustment));
      }
    }
  } else {
    DCHECK_EQ(POPCOUNT(core_spill_mask_), 0);
    DCHECK_EQ(POPCOUNT(fp_spill_mask_), 0);
    DCHECK_EQ(core_vmap_table_.size(), 0u);
    DCHECK_EQ(fp_vmap_table_.size(), 0u);
    vmap_encoder.PushBackUnsigned(0u);  // Size is 0.
  }

  // Sort patches by literal offset for better deduplication.
  std::sort(patches_.begin(), patches_.end(), [](const LinkerPatch& lhs, const LinkerPatch& rhs) {
    return lhs.LiteralOffset() < rhs.LiteralOffset();
  });

  return CompiledMethod::SwapAllocCompiledMethod(
      cu_->compiler_driver, cu_->instruction_set,
      ArrayRef<const uint8_t>(code_buffer_),
      frame_size_, core_spill_mask_, fp_spill_mask_,
      &src_mapping_table_,
      ArrayRef<const uint8_t>(encoded_mapping_table_),
      ArrayRef<const uint8_t>(vmap_encoder.GetData()),
      ArrayRef<const uint8_t>(native_gc_map_),
      ArrayRef<const uint8_t>(*cfi_.Patch(code_buffer_.size())),
      ArrayRef<const LinkerPatch>(patches_));
}

size_t Mir2Lir::GetMaxPossibleCompilerTemps() const {
  // Chose a reasonably small value in order to contain stack growth.
  // Backends that are smarter about spill region can return larger values.
  const size_t max_compiler_temps = 10;
  return max_compiler_temps;
}

size_t Mir2Lir::GetNumBytesForCompilerTempSpillRegion() {
  // By default assume that the Mir2Lir will need one slot for each temporary.
  // If the backend can better determine temps that have non-overlapping ranges and
  // temps that do not need spilled, it can actually provide a small region.
  mir_graph_->CommitCompilerTemps();
  return mir_graph_->GetNumBytesForSpecialTemps() + mir_graph_->GetMaximumBytesForNonSpecialTemps();
}

int Mir2Lir::ComputeFrameSize() {
  /* Figure out the frame size */
  uint32_t size = num_core_spills_ * GetBytesPerGprSpillLocation(cu_->instruction_set)
                  + num_fp_spills_ * GetBytesPerFprSpillLocation(cu_->instruction_set)
                  + sizeof(uint32_t)  // Filler.
                  + mir_graph_->GetNumOfLocalCodeVRs()  * sizeof(uint32_t)
                  + mir_graph_->GetNumOfOutVRs() * sizeof(uint32_t)
                  + GetNumBytesForCompilerTempSpillRegion();
  /* Align and set */
  return RoundUp(size, kStackAlignment);
}

/*
 * Append an LIR instruction to the LIR list maintained by a compilation
 * unit
 */
void Mir2Lir::AppendLIR(LIR* lir) {
  if (first_lir_insn_ == nullptr) {
    DCHECK(last_lir_insn_ == nullptr);
    last_lir_insn_ = first_lir_insn_ = lir;
    lir->prev = lir->next = nullptr;
  } else {
    last_lir_insn_->next = lir;
    lir->prev = last_lir_insn_;
    lir->next = nullptr;
    last_lir_insn_ = lir;
  }
}

/*
 * Insert an LIR instruction before the current instruction, which cannot be the
 * first instruction.
 *
 * prev_lir <-> new_lir <-> current_lir
 */
void Mir2Lir::InsertLIRBefore(LIR* current_lir, LIR* new_lir) {
  DCHECK(current_lir->prev != nullptr);
  LIR *prev_lir = current_lir->prev;

  prev_lir->next = new_lir;
  new_lir->prev = prev_lir;
  new_lir->next = current_lir;
  current_lir->prev = new_lir;
}

/*
 * Insert an LIR instruction after the current instruction, which cannot be the
 * last instruction.
 *
 * current_lir -> new_lir -> old_next
 */
void Mir2Lir::InsertLIRAfter(LIR* current_lir, LIR* new_lir) {
  new_lir->prev = current_lir;
  new_lir->next = current_lir->next;
  current_lir->next = new_lir;
  new_lir->next->prev = new_lir;
}

bool Mir2Lir::PartiallyIntersects(RegLocation rl_src, RegLocation rl_dest) {
  DCHECK(rl_src.wide);
  DCHECK(rl_dest.wide);
  return (abs(mir_graph_->SRegToVReg(rl_src.s_reg_low) - mir_graph_->SRegToVReg(rl_dest.s_reg_low)) == 1);
}

bool Mir2Lir::Intersects(RegLocation rl_src, RegLocation rl_dest) {
  DCHECK(rl_src.wide);
  DCHECK(rl_dest.wide);
  return (abs(mir_graph_->SRegToVReg(rl_src.s_reg_low) - mir_graph_->SRegToVReg(rl_dest.s_reg_low)) <= 1);
}

LIR *Mir2Lir::OpCmpMemImmBranch(ConditionCode cond, RegStorage temp_reg, RegStorage base_reg,
                                int offset, int check_value, LIR* target, LIR** compare) {
  // Handle this for architectures that can't compare to memory.
  LIR* inst = Load32Disp(base_reg, offset, temp_reg);
  if (compare != nullptr) {
    *compare = inst;
  }
  LIR* branch = OpCmpImmBranch(cond, temp_reg, check_value, target);
  return branch;
}

void Mir2Lir::AddSlowPath(LIRSlowPath* slowpath) {
  slow_paths_.push_back(slowpath);
  ResetDefTracking();
}

void Mir2Lir::LoadCodeAddress(const MethodReference& target_method, InvokeType type,
                              SpecialTargetRegister symbolic_reg) {
  LIR* data_target = ScanLiteralPoolMethod(code_literal_list_, target_method);
  if (data_target == nullptr) {
    data_target = AddWordData(&code_literal_list_, target_method.dex_method_index);
    data_target->operands[1] = WrapPointer(const_cast<DexFile*>(target_method.dex_file));
    // NOTE: The invoke type doesn't contribute to the literal identity. In fact, we can have
    // the same method invoked with kVirtual, kSuper and kInterface but the class linker will
    // resolve these invokes to the same method, so we don't care which one we record here.
    data_target->operands[2] = type;
  }
  // Loads a code pointer. Code from oat file can be mapped anywhere.
  OpPcRelLoad(TargetPtrReg(symbolic_reg), data_target);
  DCHECK_NE(cu_->instruction_set, kMips) << reinterpret_cast<void*>(data_target);
  DCHECK_NE(cu_->instruction_set, kMips64) << reinterpret_cast<void*>(data_target);
}

void Mir2Lir::LoadMethodAddress(const MethodReference& target_method, InvokeType type,
                                SpecialTargetRegister symbolic_reg) {
  LIR* data_target = ScanLiteralPoolMethod(method_literal_list_, target_method);
  if (data_target == nullptr) {
    data_target = AddWordData(&method_literal_list_, target_method.dex_method_index);
    data_target->operands[1] = WrapPointer(const_cast<DexFile*>(target_method.dex_file));
    // NOTE: The invoke type doesn't contribute to the literal identity. In fact, we can have
    // the same method invoked with kVirtual, kSuper and kInterface but the class linker will
    // resolve these invokes to the same method, so we don't care which one we record here.
    data_target->operands[2] = type;
  }
  // Loads an ArtMethod pointer, which is not a reference.
  OpPcRelLoad(TargetPtrReg(symbolic_reg), data_target);
  DCHECK_NE(cu_->instruction_set, kMips) << reinterpret_cast<void*>(data_target);
  DCHECK_NE(cu_->instruction_set, kMips64) << reinterpret_cast<void*>(data_target);
}

void Mir2Lir::LoadClassType(const DexFile& dex_file, uint32_t type_idx,
                            SpecialTargetRegister symbolic_reg) {
  // Use the literal pool and a PC-relative load from a data word.
  LIR* data_target = ScanLiteralPoolClass(class_literal_list_, dex_file, type_idx);
  if (data_target == nullptr) {
    data_target = AddWordData(&class_literal_list_, type_idx);
    data_target->operands[1] = WrapPointer(const_cast<DexFile*>(&dex_file));
  }
  // Loads a Class pointer, which is a reference as it lives in the heap.
  OpPcRelLoad(TargetReg(symbolic_reg, kRef), data_target);
}

bool Mir2Lir::CanUseOpPcRelDexCacheArrayLoad() const {
  return false;
}

void Mir2Lir::OpPcRelDexCacheArrayLoad(const DexFile* dex_file ATTRIBUTE_UNUSED,
                                       int offset ATTRIBUTE_UNUSED,
                                       RegStorage r_dest ATTRIBUTE_UNUSED,
                                       bool wide ATTRIBUTE_UNUSED) {
  LOG(FATAL) << "No generic implementation.";
  UNREACHABLE();
}

RegLocation Mir2Lir::NarrowRegLoc(RegLocation loc) {
  if (loc.location == kLocPhysReg) {
    DCHECK(!loc.reg.Is32Bit());
    if (loc.reg.IsPair()) {
      RegisterInfo* info_lo = GetRegInfo(loc.reg.GetLow());
      RegisterInfo* info_hi = GetRegInfo(loc.reg.GetHigh());
      info_lo->SetIsWide(false);
      info_hi->SetIsWide(false);
      loc.reg = info_lo->GetReg();
    } else {
      RegisterInfo* info = GetRegInfo(loc.reg);
      RegisterInfo* info_new = info->FindMatchingView(RegisterInfo::k32SoloStorageMask);
      DCHECK(info_new != nullptr);
      if (info->IsLive() && (info->SReg() == loc.s_reg_low)) {
        info->MarkDead();
        info_new->MarkLive(loc.s_reg_low);
      }
      loc.reg = info_new->GetReg();
    }
    DCHECK(loc.reg.Valid());
  }
  loc.wide = false;
  return loc;
}

void Mir2Lir::GenMachineSpecificExtendedMethodMIR(BasicBlock* bb, MIR* mir) {
  UNUSED(bb, mir);
  LOG(FATAL) << "Unknown MIR opcode not supported on this architecture";
  UNREACHABLE();
}

void Mir2Lir::InitReferenceVRegs(BasicBlock* bb, BitVector* references) {
  // Mark the references coming from the first predecessor.
  DCHECK(bb != nullptr);
  DCHECK(bb->block_type == kEntryBlock || !bb->predecessors.empty());
  BasicBlock* first_bb =
      (bb->block_type == kEntryBlock) ? bb : mir_graph_->GetBasicBlock(bb->predecessors[0]);
  DCHECK(first_bb != nullptr);
  DCHECK(first_bb->data_flow_info != nullptr);
  DCHECK(first_bb->data_flow_info->vreg_to_ssa_map_exit != nullptr);
  const int32_t* first_vreg_to_ssa_map = first_bb->data_flow_info->vreg_to_ssa_map_exit;
  references->ClearAllBits();
  for (uint32_t vreg = 0, num_vregs = mir_graph_->GetNumOfCodeVRs(); vreg != num_vregs; ++vreg) {
    int32_t sreg = first_vreg_to_ssa_map[vreg];
    if (sreg != INVALID_SREG && mir_graph_->reg_location_[sreg].ref &&
        !mir_graph_->IsConstantNullRef(mir_graph_->reg_location_[sreg])) {
      references->SetBit(vreg);
    }
  }
  // Unmark the references that are merging with a different value.
  for (size_t i = 1u, num_pred = bb->predecessors.size(); i < num_pred; ++i) {
    BasicBlock* pred_bb = mir_graph_->GetBasicBlock(bb->predecessors[i]);
    DCHECK(pred_bb != nullptr);
    DCHECK(pred_bb->data_flow_info != nullptr);
    DCHECK(pred_bb->data_flow_info->vreg_to_ssa_map_exit != nullptr);
    const int32_t* pred_vreg_to_ssa_map = pred_bb->data_flow_info->vreg_to_ssa_map_exit;
    for (uint32_t vreg : references->Indexes()) {
      if (first_vreg_to_ssa_map[vreg] != pred_vreg_to_ssa_map[vreg]) {
        // NOTE: The BitVectorSet::IndexIterator will not check the pointed-to bit again,
        // so clearing the bit has no effect on the iterator.
        references->ClearBit(vreg);
      }
    }
  }
}

bool Mir2Lir::UpdateReferenceVRegsLocal(MIR* mir, MIR* prev_mir, BitVector* references) {
  DCHECK(mir == nullptr || mir->bb == prev_mir->bb);
  DCHECK(prev_mir != nullptr);
  while (prev_mir != nullptr) {
    if (prev_mir == mir) {
      return true;
    }
    const size_t num_defs = prev_mir->ssa_rep->num_defs;
    const int32_t* defs = prev_mir->ssa_rep->defs;
    if (num_defs == 1u && mir_graph_->reg_location_[defs[0]].ref &&
        !mir_graph_->IsConstantNullRef(mir_graph_->reg_location_[defs[0]])) {
      references->SetBit(mir_graph_->SRegToVReg(defs[0]));
    } else {
      for (size_t i = 0u; i != num_defs; ++i) {
        references->ClearBit(mir_graph_->SRegToVReg(defs[i]));
      }
    }
    prev_mir = prev_mir->next;
  }
  return false;
}

void Mir2Lir::UpdateReferenceVRegs(MIR* mir, MIR* prev_mir, BitVector* references) {
  if (mir == nullptr) {
    // Safepoint in entry sequence.
    InitReferenceVRegs(mir_graph_->GetEntryBlock(), references);
    return;
  }
  if (IsInstructionReturn(mir->dalvikInsn.opcode) ||
      mir->dalvikInsn.opcode == Instruction::RETURN_VOID_NO_BARRIER) {
    references->ClearAllBits();
    if (mir->dalvikInsn.opcode == Instruction::RETURN_OBJECT) {
      references->SetBit(mir_graph_->SRegToVReg(mir->ssa_rep->uses[0]));
    }
    return;
  }
  if (prev_mir != nullptr && mir->bb == prev_mir->bb &&
      UpdateReferenceVRegsLocal(mir, prev_mir, references)) {
    return;
  }
  BasicBlock* bb = mir_graph_->GetBasicBlock(mir->bb);
  DCHECK(bb != nullptr);
  InitReferenceVRegs(bb, references);
  bool success = UpdateReferenceVRegsLocal(mir, bb->first_mir_insn, references);
  DCHECK(success) << "MIR @0x" << std::hex << mir->offset << " not in BB#" << std::dec << mir->bb;
}

}  // namespace art
