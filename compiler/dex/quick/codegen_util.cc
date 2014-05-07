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

#include "dex/compiler_internals.h"
#include "dex_file-inl.h"
#include "gc_map.h"
#include "gc_map_builder.h"
#include "mapping_table.h"
#include "mir_to_lir-inl.h"
#include "dex/quick/dex_file_method_inliner.h"
#include "dex/quick/dex_file_to_method_inliner_map.h"
#include "dex/verification_results.h"
#include "dex/verified_method.h"
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
  inst->u.m.def_mask = ENCODE_ALL;
  LIR* safepoint_pc = NewLIR0(kPseudoSafepointPC);
  DCHECK_EQ(safepoint_pc->u.m.def_mask, ENCODE_ALL);
}

/* Remove a LIR from the list. */
void Mir2Lir::UnlinkLIR(LIR* lir) {
  if (UNLIKELY(lir == first_lir_insn_)) {
    first_lir_insn_ = lir->next;
    if (lir->next != NULL) {
      lir->next->prev = NULL;
    } else {
      DCHECK(lir->next == NULL);
      DCHECK(lir == last_lir_insn_);
      last_lir_insn_ = NULL;
    }
  } else if (lir == last_lir_insn_) {
    last_lir_insn_ = lir->prev;
    lir->prev->next = NULL;
  } else if ((lir->prev != NULL) && (lir->next != NULL)) {
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
  uint64_t *mask_ptr;
  uint64_t mask = ENCODE_MEM;
  DCHECK(GetTargetInstFlags(lir->opcode) & (IS_LOAD | IS_STORE));
  DCHECK(!lir->flags.use_def_invalid);
  if (is_load) {
    mask_ptr = &lir->u.m.use_mask;
  } else {
    mask_ptr = &lir->u.m.def_mask;
  }
  /* Clear out the memref flags */
  *mask_ptr &= ~mask;
  /* ..and then add back the one we need */
  switch (mem_type) {
    case kLiteral:
      DCHECK(is_load);
      *mask_ptr |= ENCODE_LITERAL;
      break;
    case kDalvikReg:
      *mask_ptr |= ENCODE_DALVIK_REG;
      break;
    case kHeapRef:
      *mask_ptr |= ENCODE_HEAP_REF;
      break;
    case kMustNotAlias:
      /* Currently only loads can be marked as kMustNotAlias */
      DCHECK(!(GetTargetInstFlags(lir->opcode) & IS_STORE));
      *mask_ptr |= ENCODE_MUST_NOT_ALIAS;
      break;
    default:
      LOG(FATAL) << "Oat: invalid memref kind - " << mem_type;
  }
}

/*
 * Mark load/store instructions that access Dalvik registers through the stack.
 */
void Mir2Lir::AnnotateDalvikRegAccess(LIR* lir, int reg_id, bool is_load,
                                      bool is64bit) {
  SetMemRefType(lir, is_load, kDalvikReg);

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
    case kPseudoMethodEntry:
      LOG(INFO) << "-------- method entry "
                << PrettyMethod(cu_->method_idx, *cu_->dex_file);
      break;
    case kPseudoMethodExit:
      LOG(INFO) << "-------- Method_Exit";
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
                << reinterpret_cast<char*>(UnwrapPointer(lir->operands[0]));
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
        LOG(INFO) << StringPrintf("%5p: %-9s%s%s",
                                  base_addr + offset,
                                  op_name.c_str(), op_operands.c_str(),
                                  lir->flags.is_nop ? "(nop)" : "");
      }
      break;
  }

  if (lir->u.m.use_mask && (!lir->flags.is_nop || dump_nop)) {
    DUMP_RESOURCE_MASK(DumpResourceMask(lir, lir->u.m.use_mask, "use"));
  }
  if (lir->u.m.def_mask && (!lir->flags.is_nop || dump_nop)) {
    DUMP_RESOURCE_MASK(DumpResourceMask(lir, lir->u.m.def_mask, "def"));
  }
}

void Mir2Lir::DumpPromotionMap() {
  int num_regs = cu_->num_dalvik_registers + mir_graph_->GetNumUsedCompilerTemps();
  for (int i = 0; i < num_regs; i++) {
    PromotionMap v_reg_map = promotion_map_[i];
    std::string buf;
    if (v_reg_map.fp_location == kLocPhysReg) {
      StringAppendF(&buf, " : s%d", RegStorage::RegNum(v_reg_map.FpReg));
    }

    std::string buf3;
    if (i < cu_->num_dalvik_registers) {
      StringAppendF(&buf3, "%02d", i);
    } else if (i == mir_graph_->GetMethodSReg()) {
      buf3 = "Method*";
    } else {
      StringAppendF(&buf3, "ct%d", i - cu_->num_dalvik_registers);
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

/* Dump instructions and constant pool contents */
void Mir2Lir::CodegenDump() {
  LOG(INFO) << "Dumping LIR insns for "
            << PrettyMethod(cu_->method_idx, *cu_->dex_file);
  LIR* lir_insn;
  int insns_size = cu_->code_item->insns_size_in_code_units_;

  LOG(INFO) << "Regs (excluding ins) : " << cu_->num_regs;
  LOG(INFO) << "Ins          : " << cu_->num_ins;
  LOG(INFO) << "Outs         : " << cu_->num_outs;
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
  for (lir_insn = first_lir_insn_; lir_insn != NULL; lir_insn = lir_insn->next) {
    DumpLIRInsn(lir_insn, 0);
  }
  for (lir_insn = literal_list_; lir_insn != NULL; lir_insn = lir_insn->next) {
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
  return NULL;
}

/* Search the existing constants in the literal pool for an exact wide match */
LIR* Mir2Lir::ScanLiteralPoolWide(LIR* data_target, int val_lo, int val_hi) {
  bool lo_match = false;
  LIR* lo_target = NULL;
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
  return NULL;
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
  return NULL;
}

/* Add a 64-bit constant to the constant pool or mixed with code */
LIR* Mir2Lir::AddWideData(LIR* *constant_list_p, int val_lo, int val_hi) {
  AddWordData(constant_list_p, val_hi);
  return AddWordData(constant_list_p, val_lo);
}

static void Push32(std::vector<uint8_t>&buf, int data) {
  buf.push_back(data & 0xff);
  buf.push_back((data >> 8) & 0xff);
  buf.push_back((data >> 16) & 0xff);
  buf.push_back((data >> 24) & 0xff);
}

// Push 8 bytes on 64-bit target systems; 4 on 32-bit target systems.
static void PushPointer(std::vector<uint8_t>&buf, const void* pointer, bool target64) {
  uint64_t data = reinterpret_cast<uintptr_t>(pointer);
  if (target64) {
    Push32(buf, data & 0xFFFFFFFF);
    Push32(buf, (data >> 32) & 0xFFFFFFFF);
  } else {
    Push32(buf, static_cast<uint32_t>(data));
  }
}

static void AlignBuffer(std::vector<uint8_t>&buf, size_t offset) {
  while (buf.size() < offset) {
    buf.push_back(0);
  }
}

/* Write the literal pool to the output stream */
void Mir2Lir::InstallLiteralPools() {
  AlignBuffer(code_buffer_, data_offset_);
  LIR* data_lir = literal_list_;
  while (data_lir != NULL) {
    Push32(code_buffer_, data_lir->operands[0]);
    data_lir = NEXT_LIR(data_lir);
  }
  // Push code and method literals, record offsets for the compiler to patch.
  data_lir = code_literal_list_;
  while (data_lir != NULL) {
    uint32_t target_method_idx = data_lir->operands[0];
    const DexFile* target_dex_file =
        reinterpret_cast<const DexFile*>(UnwrapPointer(data_lir->operands[1]));
    cu_->compiler_driver->AddCodePatch(cu_->dex_file,
                                       cu_->class_def_idx,
                                       cu_->method_idx,
                                       cu_->invoke_type,
                                       target_method_idx,
                                       target_dex_file,
                                       static_cast<InvokeType>(data_lir->operands[2]),
                                       code_buffer_.size());
    const DexFile::MethodId& target_method_id = target_dex_file->GetMethodId(target_method_idx);
    // unique value based on target to ensure code deduplication works
    PushPointer(code_buffer_, &target_method_id, cu_->target64);
    data_lir = NEXT_LIR(data_lir);
  }
  data_lir = method_literal_list_;
  while (data_lir != NULL) {
    uint32_t target_method_idx = data_lir->operands[0];
    const DexFile* target_dex_file =
        reinterpret_cast<const DexFile*>(UnwrapPointer(data_lir->operands[1]));
    cu_->compiler_driver->AddMethodPatch(cu_->dex_file,
                                         cu_->class_def_idx,
                                         cu_->method_idx,
                                         cu_->invoke_type,
                                         target_method_idx,
                                         target_dex_file,
                                         static_cast<InvokeType>(data_lir->operands[2]),
                                         code_buffer_.size());
    const DexFile::MethodId& target_method_id = target_dex_file->GetMethodId(target_method_idx);
    // unique value based on target to ensure code deduplication works
    PushPointer(code_buffer_, &target_method_id, cu_->target64);
    data_lir = NEXT_LIR(data_lir);
  }
  // Push class literals.
  data_lir = class_literal_list_;
  while (data_lir != NULL) {
    uint32_t target_method_idx = data_lir->operands[0];
    cu_->compiler_driver->AddClassPatch(cu_->dex_file,
                                        cu_->class_def_idx,
                                        cu_->method_idx,
                                        target_method_idx,
                                        code_buffer_.size());
    const DexFile::TypeId& target_method_id = cu_->dex_file->GetTypeId(target_method_idx);
    // unique value based on target to ensure code deduplication works
    PushPointer(code_buffer_, &target_method_id, cu_->target64);
    data_lir = NEXT_LIR(data_lir);
  }
}

/* Write the switch tables to the output stream */
void Mir2Lir::InstallSwitchTables() {
  GrowableArray<SwitchTable*>::Iterator iterator(&switch_tables_);
  while (true) {
    Mir2Lir::SwitchTable* tab_rec = iterator.Next();
    if (tab_rec == NULL) break;
    AlignBuffer(code_buffer_, tab_rec->offset);
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
      case kX86:
      case kX86_64:
        bx_offset = 0;
        break;
      case kArm64:
      case kMips:
        bx_offset = tab_rec->anchor->offset;
        break;
      default: LOG(FATAL) << "Unexpected instruction set: " << cu_->instruction_set;
    }
    if (cu_->verbose) {
      LOG(INFO) << "Switch table for offset 0x" << std::hex << bx_offset;
    }
    if (tab_rec->table[0] == Instruction::kSparseSwitchSignature) {
      const int32_t* keys = reinterpret_cast<const int32_t*>(&(tab_rec->table[2]));
      for (int elems = 0; elems < tab_rec->table[1]; elems++) {
        int disp = tab_rec->targets[elems]->offset - bx_offset;
        if (cu_->verbose) {
          LOG(INFO) << "  Case[" << elems << "] key: 0x"
                    << std::hex << keys[elems] << ", disp: 0x"
                    << std::hex << disp;
        }
        Push32(code_buffer_, keys[elems]);
        Push32(code_buffer_,
          tab_rec->targets[elems]->offset - bx_offset);
      }
    } else {
      DCHECK_EQ(static_cast<int>(tab_rec->table[0]),
                static_cast<int>(Instruction::kPackedSwitchSignature));
      for (int elems = 0; elems < tab_rec->table[1]; elems++) {
        int disp = tab_rec->targets[elems]->offset - bx_offset;
        if (cu_->verbose) {
          LOG(INFO) << "  Case[" << elems << "] disp: 0x"
                    << std::hex << disp;
        }
        Push32(code_buffer_, tab_rec->targets[elems]->offset - bx_offset);
      }
    }
  }
}

/* Write the fill array dta to the output stream */
void Mir2Lir::InstallFillArrayData() {
  GrowableArray<FillArrayData*>::Iterator iterator(&fill_array_data_);
  while (true) {
    Mir2Lir::FillArrayData *tab_rec = iterator.Next();
    if (tab_rec == NULL) break;
    AlignBuffer(code_buffer_, tab_rec->offset);
    for (int i = 0; i < (tab_rec->size + 1) / 2; i++) {
      code_buffer_.push_back(tab_rec->table[i] & 0xFF);
      code_buffer_.push_back((tab_rec->table[i] >> 8) & 0xFF);
    }
  }
}

static int AssignLiteralOffsetCommon(LIR* lir, CodeOffset offset) {
  for (; lir != NULL; lir = lir->next) {
    lir->offset = offset;
    offset += 4;
  }
  return offset;
}

static int AssignLiteralPointerOffsetCommon(LIR* lir, CodeOffset offset,
                                            unsigned int element_size) {
  // Align to natural pointer size.
  offset = RoundUp(offset, element_size);
  for (; lir != NULL; lir = lir->next) {
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
  uint32_t pc2dex_data_size = 0u;
  uint32_t pc2dex_entries = 0u;
  uint32_t pc2dex_offset = 0u;
  uint32_t pc2dex_dalvik_offset = 0u;
  uint32_t dex2pc_data_size = 0u;
  uint32_t dex2pc_entries = 0u;
  uint32_t dex2pc_offset = 0u;
  uint32_t dex2pc_dalvik_offset = 0u;
  for (LIR* tgt_lir = first_lir_insn_; tgt_lir != NULL; tgt_lir = NEXT_LIR(tgt_lir)) {
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

  uint32_t total_entries = pc2dex_entries + dex2pc_entries;
  uint32_t hdr_data_size = UnsignedLeb128Size(total_entries) + UnsignedLeb128Size(pc2dex_entries);
  uint32_t data_size = hdr_data_size + pc2dex_data_size + dex2pc_data_size;
  encoded_mapping_table_.resize(data_size);
  uint8_t* write_pos = &encoded_mapping_table_[0];
  write_pos = EncodeUnsignedLeb128(write_pos, total_entries);
  write_pos = EncodeUnsignedLeb128(write_pos, pc2dex_entries);
  DCHECK_EQ(static_cast<size_t>(write_pos - &encoded_mapping_table_[0]), hdr_data_size);
  uint8_t* write_pos2 = write_pos + pc2dex_data_size;

  pc2dex_offset = 0u;
  pc2dex_dalvik_offset = 0u;
  dex2pc_offset = 0u;
  dex2pc_dalvik_offset = 0u;
  for (LIR* tgt_lir = first_lir_insn_; tgt_lir != NULL; tgt_lir = NEXT_LIR(tgt_lir)) {
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
    for (LIR* tgt_lir = first_lir_insn_; tgt_lir != NULL; tgt_lir = NEXT_LIR(tgt_lir)) {
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
    CHECK(references != NULL) << "Missing ref for dex pc 0x" << std::hex << dex_pc <<
        ": " << PrettyMethod(cu_->method_idx, *cu_->dex_file);
    native_gc_map_builder.AddEntry(native_offset, references);
  }
}

/* Determine the offset of each literal field */
int Mir2Lir::AssignLiteralOffset(CodeOffset offset) {
  offset = AssignLiteralOffsetCommon(literal_list_, offset);
  unsigned int ptr_size = GetInstructionSetPointerSize(cu_->instruction_set);
  offset = AssignLiteralPointerOffsetCommon(code_literal_list_, offset, ptr_size);
  offset = AssignLiteralPointerOffsetCommon(method_literal_list_, offset, ptr_size);
  offset = AssignLiteralPointerOffsetCommon(class_literal_list_, offset, ptr_size);
  return offset;
}

int Mir2Lir::AssignSwitchTablesOffset(CodeOffset offset) {
  GrowableArray<SwitchTable*>::Iterator iterator(&switch_tables_);
  while (true) {
    Mir2Lir::SwitchTable* tab_rec = iterator.Next();
    if (tab_rec == NULL) break;
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
  GrowableArray<FillArrayData*>::Iterator iterator(&fill_array_data_);
  while (true) {
    Mir2Lir::FillArrayData *tab_rec = iterator.Next();
    if (tab_rec == NULL) break;
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
LIR* Mir2Lir::InsertCaseLabel(DexOffset vaddr, int keyVal) {
  LIR* boundary_lir = &block_label_list_[mir_graph_->FindBlock(vaddr)->id];
  LIR* res = boundary_lir;
  if (cu_->verbose) {
    // Only pay the expense if we're pretty-printing.
    LIR* new_label = static_cast<LIR*>(arena_->Alloc(sizeof(LIR), kArenaAllocLIR));
    new_label->dalvik_offset = vaddr;
    new_label->opcode = kPseudoCaseLabel;
    new_label->operands[0] = keyVal;
    new_label->flags.fixup = kFixupLabel;
    DCHECK(!new_label->flags.use_def_invalid);
    new_label->u.m.def_mask = ENCODE_ALL;
    InsertLIRAfter(boundary_lir, new_label);
    res = new_label;
  }
  return res;
}

void Mir2Lir::MarkPackedCaseLabels(Mir2Lir::SwitchTable* tab_rec) {
  const uint16_t* table = tab_rec->table;
  DexOffset base_vaddr = tab_rec->vaddr;
  const int32_t *targets = reinterpret_cast<const int32_t*>(&table[4]);
  int entries = table[1];
  int low_key = s4FromSwitchData(&table[2]);
  for (int i = 0; i < entries; i++) {
    tab_rec->targets[i] = InsertCaseLabel(base_vaddr + targets[i], i + low_key);
  }
}

void Mir2Lir::MarkSparseCaseLabels(Mir2Lir::SwitchTable* tab_rec) {
  const uint16_t* table = tab_rec->table;
  DexOffset base_vaddr = tab_rec->vaddr;
  int entries = table[1];
  const int32_t* keys = reinterpret_cast<const int32_t*>(&table[2]);
  const int32_t* targets = &keys[entries];
  for (int i = 0; i < entries; i++) {
    tab_rec->targets[i] = InsertCaseLabel(base_vaddr + targets[i], keys[i]);
  }
}

void Mir2Lir::ProcessSwitchTables() {
  GrowableArray<SwitchTable*>::Iterator iterator(&switch_tables_);
  while (true) {
    Mir2Lir::SwitchTable *tab_rec = iterator.Next();
    if (tab_rec == NULL) break;
    if (tab_rec->table[0] == Instruction::kPackedSwitchSignature) {
      MarkPackedCaseLabels(tab_rec);
    } else if (tab_rec->table[0] == Instruction::kSparseSwitchSignature) {
      MarkSparseCaseLabels(tab_rec);
    } else {
      LOG(FATAL) << "Invalid switch table";
    }
  }
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
  // NOTE: only used for debug listings.
  NewLIR1(kPseudoDalvikByteCodeBoundary, WrapPointer(ArenaStrdup(inst_str)));
}

bool Mir2Lir::EvaluateBranch(Instruction::Code opcode, int32_t src1, int32_t src2) {
  bool is_taken;
  switch (opcode) {
    case Instruction::IF_EQ: is_taken = (src1 == src2); break;
    case Instruction::IF_NE: is_taken = (src1 != src2); break;
    case Instruction::IF_LT: is_taken = (src1 < src2); break;
    case Instruction::IF_GE: is_taken = (src1 >= src2); break;
    case Instruction::IF_GT: is_taken = (src1 > src2); break;
    case Instruction::IF_LE: is_taken = (src1 <= src2); break;
    case Instruction::IF_EQZ: is_taken = (src1 == 0); break;
    case Instruction::IF_NEZ: is_taken = (src1 != 0); break;
    case Instruction::IF_LTZ: is_taken = (src1 < 0); break;
    case Instruction::IF_GEZ: is_taken = (src1 >= 0); break;
    case Instruction::IF_GTZ: is_taken = (src1 > 0); break;
    case Instruction::IF_LEZ: is_taken = (src1 <= 0); break;
    default:
      LOG(FATAL) << "Unexpected opcode " << opcode;
      is_taken = false;
  }
  return is_taken;
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
      res = static_cast<ConditionCode>(0);
      LOG(FATAL) << "Unexpected ccode " << before;
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
      res = static_cast<ConditionCode>(0);
      LOG(FATAL) << "Unexpected ccode " << before;
  }
  return res;
}

// TODO: move to mir_to_lir.cc
Mir2Lir::Mir2Lir(CompilationUnit* cu, MIRGraph* mir_graph, ArenaAllocator* arena)
    : Backend(arena),
      literal_list_(NULL),
      method_literal_list_(NULL),
      class_literal_list_(NULL),
      code_literal_list_(NULL),
      first_fixup_(NULL),
      cu_(cu),
      mir_graph_(mir_graph),
      switch_tables_(arena, 4, kGrowableArraySwitchTables),
      fill_array_data_(arena, 4, kGrowableArrayFillArrayData),
      tempreg_info_(arena, 20, kGrowableArrayMisc),
      reginfo_map_(arena, RegStorage::kMaxRegs, kGrowableArrayMisc),
      pointer_storage_(arena, 128, kGrowableArrayMisc),
      data_offset_(0),
      total_size_(0),
      block_label_list_(NULL),
      promotion_map_(NULL),
      current_dalvik_offset_(0),
      estimated_native_code_size_(0),
      reg_pool_(NULL),
      live_sreg_(0),
      num_core_spills_(0),
      num_fp_spills_(0),
      frame_size_(0),
      core_spill_mask_(0),
      fp_spill_mask_(0),
      first_lir_insn_(NULL),
      last_lir_insn_(NULL),
      slow_paths_(arena, 32, kGrowableArraySlowPaths) {
  // Reserve pointer id 0 for NULL.
  size_t null_idx = WrapPointer(NULL);
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
    // mark the targets of switch statement case labels
    ProcessSwitchTables();

    /* Convert LIR into machine code. */
    AssembleLIR();

    if (cu_->verbose) {
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
    // fp regs already sorted.
    for (uint32_t i = 0; i < fp_vmap_table_.size(); i++) {
      vmap_encoder.PushBackUnsigned(fp_vmap_table_[i] + VmapTable::kEntryAdjustment);
    }
  } else {
    DCHECK_EQ(POPCOUNT(core_spill_mask_), 0);
    DCHECK_EQ(POPCOUNT(fp_spill_mask_), 0);
    DCHECK_EQ(core_vmap_table_.size(), 0u);
    DCHECK_EQ(fp_vmap_table_.size(), 0u);
    vmap_encoder.PushBackUnsigned(0u);  // Size is 0.
  }

  UniquePtr<std::vector<uint8_t> > cfi_info(ReturnCallFrameInformation());
  CompiledMethod* result =
      new CompiledMethod(cu_->compiler_driver, cu_->instruction_set, code_buffer_, frame_size_,
                         core_spill_mask_, fp_spill_mask_, encoded_mapping_table_,
                         vmap_encoder.GetData(), native_gc_map_, cfi_info.get());
  return result;
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
  return (mir_graph_->GetNumUsedCompilerTemps() * sizeof(uint32_t));
}

int Mir2Lir::ComputeFrameSize() {
  /* Figure out the frame size */
  uint32_t size = num_core_spills_ * GetBytesPerGprSpillLocation(cu_->instruction_set)
                  + num_fp_spills_ * GetBytesPerFprSpillLocation(cu_->instruction_set)
                  + sizeof(uint32_t)  // Filler.
                  + (cu_->num_regs + cu_->num_outs) * sizeof(uint32_t)
                  + GetNumBytesForCompilerTempSpillRegion();
  /* Align and set */
  return RoundUp(size, kStackAlignment);
}

/*
 * Append an LIR instruction to the LIR list maintained by a compilation
 * unit
 */
void Mir2Lir::AppendLIR(LIR* lir) {
  if (first_lir_insn_ == NULL) {
    DCHECK(last_lir_insn_ == NULL);
    last_lir_insn_ = first_lir_insn_ = lir;
    lir->prev = lir->next = NULL;
  } else {
    last_lir_insn_->next = lir;
    lir->prev = last_lir_insn_;
    lir->next = NULL;
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
  DCHECK(current_lir->prev != NULL);
  LIR *prev_lir = current_lir->prev;

  prev_lir->next = new_lir;
  new_lir->prev = prev_lir;
  new_lir->next = current_lir;
  current_lir->prev = new_lir;
}

/*
 * Insert an LIR instruction after the current instruction, which cannot be the
 * first instruction.
 *
 * current_lir -> new_lir -> old_next
 */
void Mir2Lir::InsertLIRAfter(LIR* current_lir, LIR* new_lir) {
  new_lir->prev = current_lir;
  new_lir->next = current_lir->next;
  current_lir->next = new_lir;
  new_lir->next->prev = new_lir;
}

bool Mir2Lir::IsPowerOfTwo(uint64_t x) {
  return (x & (x - 1)) == 0;
}

// Returns the index of the lowest set bit in 'x'.
int32_t Mir2Lir::LowestSetBit(uint64_t x) {
  int bit_posn = 0;
  while ((x & 0xf) == 0) {
    bit_posn += 4;
    x >>= 4;
  }
  while ((x & 1) == 0) {
    bit_posn++;
    x >>= 1;
  }
  return bit_posn;
}

bool Mir2Lir::BadOverlap(RegLocation rl_src, RegLocation rl_dest) {
  DCHECK(rl_src.wide);
  DCHECK(rl_dest.wide);
  return (abs(mir_graph_->SRegToVReg(rl_src.s_reg_low) - mir_graph_->SRegToVReg(rl_dest.s_reg_low)) == 1);
}

LIR *Mir2Lir::OpCmpMemImmBranch(ConditionCode cond, RegStorage temp_reg, RegStorage base_reg,
                                int offset, int check_value, LIR* target) {
  // Handle this for architectures that can't compare to memory.
  Load32Disp(base_reg, offset, temp_reg);
  LIR* branch = OpCmpImmBranch(cond, temp_reg, check_value, target);
  return branch;
}

void Mir2Lir::AddSlowPath(LIRSlowPath* slowpath) {
  slow_paths_.Insert(slowpath);
}

void Mir2Lir::LoadCodeAddress(const MethodReference& target_method, InvokeType type,
                              SpecialTargetRegister symbolic_reg) {
  int target_method_idx = target_method.dex_method_index;
  LIR* data_target = ScanLiteralPool(code_literal_list_, target_method_idx, 0);
  if (data_target == NULL) {
    data_target = AddWordData(&code_literal_list_, target_method_idx);
    data_target->operands[1] = WrapPointer(const_cast<DexFile*>(target_method.dex_file));
    data_target->operands[2] = type;
  }
  LIR* load_pc_rel = OpPcRelLoad(TargetReg(symbolic_reg), data_target);
  AppendLIR(load_pc_rel);
  DCHECK_NE(cu_->instruction_set, kMips) << reinterpret_cast<void*>(data_target);
}

void Mir2Lir::LoadMethodAddress(const MethodReference& target_method, InvokeType type,
                                SpecialTargetRegister symbolic_reg) {
  int target_method_idx = target_method.dex_method_index;
  LIR* data_target = ScanLiteralPool(method_literal_list_, target_method_idx, 0);
  if (data_target == NULL) {
    data_target = AddWordData(&method_literal_list_, target_method_idx);
    data_target->operands[1] = WrapPointer(const_cast<DexFile*>(target_method.dex_file));
    data_target->operands[2] = type;
  }
  LIR* load_pc_rel = OpPcRelLoad(TargetReg(symbolic_reg), data_target);
  AppendLIR(load_pc_rel);
  DCHECK_NE(cu_->instruction_set, kMips) << reinterpret_cast<void*>(data_target);
}

void Mir2Lir::LoadClassType(uint32_t type_idx, SpecialTargetRegister symbolic_reg) {
  // Use the literal pool and a PC-relative load from a data word.
  LIR* data_target = ScanLiteralPool(class_literal_list_, type_idx, 0);
  if (data_target == nullptr) {
    data_target = AddWordData(&class_literal_list_, type_idx);
  }
  LIR* load_pc_rel = OpPcRelLoad(TargetReg(symbolic_reg), data_target);
  AppendLIR(load_pc_rel);
}

std::vector<uint8_t>* Mir2Lir::ReturnCallFrameInformation() {
  // Default case is to do nothing.
  return nullptr;
}

RegLocation Mir2Lir::NarrowRegLoc(RegLocation loc) {
  loc.wide = false;
  if (loc.location == kLocPhysReg) {
    if (loc.reg.IsPair()) {
      loc.reg = loc.reg.GetLow();
    } else {
      // FIXME: temp workaround.
      // Issue here: how do we narrow to a 32-bit value in 64-bit container?
      // Probably the wrong thing to narrow the RegStorage container here.  That
      // should be a target decision.  At the RegLocation level, we're only
      // modifying the view of the Dalvik value - this is orthogonal to the storage
      // container size.  Consider this a temp workaround.
      DCHECK(loc.reg.IsDouble());
      loc.reg = loc.reg.DoubleToLowSingle();
    }
  }
  return loc;
}

}  // namespace art
