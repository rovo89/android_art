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

#include "register_line.h"

#include "method_verifier.h"

namespace art {
namespace verifier {

bool RegisterLine::CheckConstructorReturn() const {
  for (size_t i = 0; i < num_regs_; i++) {
    if (GetRegisterType(i).IsUninitializedThisReference() ||
        GetRegisterType(i).IsUnresolvedAndUninitializedThisReference()) {
      verifier_->Fail(VERIFY_ERROR_BAD_CLASS_SOFT)
          << "Constructor returning without calling superclass constructor";
      return false;
    }
  }
  return true;
}

bool RegisterLine::SetRegisterType(uint32_t vdst, const RegType& new_type) {
  DCHECK_LT(vdst, num_regs_);
  if (new_type.IsLowHalf()) {
    line_[vdst] = new_type.GetId();
    line_[vdst + 1] = new_type.HighHalf(verifier_->GetRegTypeCache()).GetId();
  } else if (new_type.IsHighHalf()) {
    /* should never set these explicitly */
    verifier_->Fail(VERIFY_ERROR_BAD_CLASS_SOFT) << "Explicit set of high register type";
    return false;
  } else if (new_type.IsConflict()) {  // should only be set during a merge
    verifier_->Fail(VERIFY_ERROR_BAD_CLASS_SOFT) << "Set register to unknown type " << new_type;
    return false;
  } else if (!Runtime::Current()->IsCompiler() && new_type.IsUnresolvedTypes()) {
    // Unresolvable classes at runtime are bad and marked as a rewrite error.
    verifier_->Fail(VERIFY_ERROR_NO_CLASS) << "Set register to unresolved class '"
                                           << new_type << "' at runtime";
    return false;
  } else {
    line_[vdst] = new_type.GetId();
  }
  // Clear the monitor entry bits for this register.
  ClearAllRegToLockDepths(vdst);
  return true;
}

void RegisterLine::SetResultTypeToUnknown() {
  result_[0] = RegType::kRegTypeUndefined;
  result_[1] = RegType::kRegTypeUndefined;
}

void RegisterLine::SetResultRegisterType(const RegType& new_type) {
  result_[0] = new_type.GetId();
  if (new_type.IsLowHalf()) {
    DCHECK_EQ(new_type.HighHalf(verifier_->GetRegTypeCache()).GetId(), new_type.GetId() + 1);
    result_[1] = new_type.GetId() + 1;
  } else {
    result_[1] = RegType::kRegTypeUndefined;
  }
}

const RegType& RegisterLine::GetRegisterType(uint32_t vsrc) const {
  // The register index was validated during the static pass, so we don't need to check it here.
  DCHECK_LT(vsrc, num_regs_);
  return verifier_->GetRegTypeCache()->GetFromId(line_[vsrc]);
}

const RegType& RegisterLine::GetInvocationThis(const DecodedInstruction& dec_insn) {
  if (dec_insn.vA < 1) {
    verifier_->Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "invoke lacks 'this'";
    return verifier_->GetRegTypeCache()->Conflict();
  }
  /* get the element type of the array held in vsrc */
  const RegType& this_type = GetRegisterType(dec_insn.vC);
  if (!this_type.IsReferenceTypes()) {
    verifier_->Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "tried to get class from non-reference register v"
                                                 << dec_insn.vC << " (type=" << this_type << ")";
    return verifier_->GetRegTypeCache()->Conflict();
  }
  return this_type;
}

bool RegisterLine::VerifyRegisterType(uint32_t vsrc, const RegType& check_type) {
  // Verify the src register type against the check type refining the type of the register
  const RegType& src_type = GetRegisterType(vsrc);
  if (!check_type.IsAssignableFrom(src_type)) {
    // Hard fail if one of the types is primitive, since they are concretely known.
    enum VerifyError fail_type = (!check_type.IsNonZeroReferenceTypes() ||
                                  !src_type.IsNonZeroReferenceTypes()) ?
                                  VERIFY_ERROR_BAD_CLASS_HARD : VERIFY_ERROR_BAD_CLASS_SOFT;
    verifier_->Fail(fail_type) << "register v" << vsrc << " has type " << src_type
                               << " but expected " << check_type;
    return false;
  }
  if (check_type.IsLowHalf()) {
    const RegType& src_type_h = GetRegisterType(vsrc + 1);
    if (!src_type.CheckWidePair(src_type_h)) {
      verifier_->Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "wide register v" << vsrc << " has type "
                                                   << src_type << "/" << src_type_h;
      return false;
    }
  }
  // The register at vsrc has a defined type, we know the lower-upper-bound, but this is less
  // precise than the subtype in vsrc so leave it for reference types. For primitive types
  // if they are a defined type then they are as precise as we can get, however, for constant
  // types we may wish to refine them. Unfortunately constant propagation has rendered this useless.
  return true;
}

void RegisterLine::MarkRefsAsInitialized(const RegType& uninit_type) {
  DCHECK(uninit_type.IsUninitializedTypes());
  const RegType& init_type = verifier_->GetRegTypeCache()->FromUninitialized(uninit_type);
  size_t changed = 0;
  for (size_t i = 0; i < num_regs_; i++) {
    if (GetRegisterType(i).Equals(uninit_type)) {
      line_[i] = init_type.GetId();
      changed++;
    }
  }
  DCHECK_GT(changed, 0u);
}

std::string RegisterLine::Dump() const {
  std::string result;
  for (size_t i = 0; i < num_regs_; i++) {
    result += StringPrintf("%zd:[", i);
    result += GetRegisterType(i).Dump(verifier_->GetRegTypeCache());
    result += "],";
  }
  typedef std::deque<uint32_t>::const_iterator It; // TODO: C++0x auto
  for (It it = monitors_.begin(), end = monitors_.end(); it != end ; ++it) {
    result += StringPrintf("{%d},", *it);
  }
  return result;
}

void RegisterLine::MarkUninitRefsAsInvalid(const RegType& uninit_type) {
  for (size_t i = 0; i < num_regs_; i++) {
    if (GetRegisterType(i).Equals(uninit_type)) {
      line_[i] = verifier_->GetRegTypeCache()->Conflict().GetId();
      ClearAllRegToLockDepths(i);
    }
  }
}

void RegisterLine::CopyRegister1(uint32_t vdst, uint32_t vsrc, TypeCategory cat) {
  DCHECK(cat == kTypeCategory1nr || cat == kTypeCategoryRef);
  const RegType& type = GetRegisterType(vsrc);
  if (!SetRegisterType(vdst, type)) {
    return;
  }
  if ((cat == kTypeCategory1nr && !type.IsCategory1Types()) ||
      (cat == kTypeCategoryRef && !type.IsReferenceTypes())) {
    verifier_->Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "copy1 v" << vdst << "<-v" << vsrc << " type=" << type
                                                 << " cat=" << static_cast<int>(cat);
  } else if (cat == kTypeCategoryRef) {
    CopyRegToLockDepth(vdst, vsrc);
  }
}

void RegisterLine::CopyRegister2(uint32_t vdst, uint32_t vsrc) {
  const RegType& type_l = GetRegisterType(vsrc);
  const RegType& type_h = GetRegisterType(vsrc + 1);

  if (!type_l.CheckWidePair(type_h)) {
    verifier_->Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "copy2 v" << vdst << "<-v" << vsrc
                                                 << " type=" << type_l << "/" << type_h;
  } else {
    SetRegisterType(vdst, type_l);  // implicitly sets the second half
  }
}

void RegisterLine::CopyResultRegister1(uint32_t vdst, bool is_reference) {
  const RegType& type = verifier_->GetRegTypeCache()->GetFromId(result_[0]);
  if ((!is_reference && !type.IsCategory1Types()) ||
      (is_reference && !type.IsReferenceTypes())) {
    verifier_->Fail(VERIFY_ERROR_BAD_CLASS_HARD)
        << "copyRes1 v" << vdst << "<- result0"  << " type=" << type;
  } else {
    DCHECK(verifier_->GetRegTypeCache()->GetFromId(result_[1]).IsUndefined());
    SetRegisterType(vdst, type);
    result_[0] = RegType::kRegTypeUndefined;
  }
}

/*
 * Implement "move-result-wide". Copy the category-2 value from the result
 * register to another register, and reset the result register.
 */
void RegisterLine::CopyResultRegister2(uint32_t vdst) {
  const RegType& type_l = verifier_->GetRegTypeCache()->GetFromId(result_[0]);
  const RegType& type_h = verifier_->GetRegTypeCache()->GetFromId(result_[1]);
  if (!type_l.IsCategory2Types()) {
    verifier_->Fail(VERIFY_ERROR_BAD_CLASS_HARD)
        << "copyRes2 v" << vdst << "<- result0"  << " type=" << type_l;
  } else {
    DCHECK(type_l.CheckWidePair(type_h));  // Set should never allow this case
    SetRegisterType(vdst, type_l);  // also sets the high
    result_[0] = RegType::kRegTypeUndefined;
    result_[1] = RegType::kRegTypeUndefined;
  }
}

void RegisterLine::CheckUnaryOp(const DecodedInstruction& dec_insn,
                                const RegType& dst_type, const RegType& src_type) {
  if (VerifyRegisterType(dec_insn.vB, src_type)) {
    SetRegisterType(dec_insn.vA, dst_type);
  }
}

void RegisterLine::CheckBinaryOp(const DecodedInstruction& dec_insn,
                                 const RegType& dst_type,
                                 const RegType& src_type1, const RegType& src_type2,
                                 bool check_boolean_op) {
  if (VerifyRegisterType(dec_insn.vB, src_type1) &&
      VerifyRegisterType(dec_insn.vC, src_type2)) {
    if (check_boolean_op) {
      DCHECK(dst_type.IsInteger());
      if (GetRegisterType(dec_insn.vB).IsBooleanTypes() &&
          GetRegisterType(dec_insn.vC).IsBooleanTypes()) {
        SetRegisterType(dec_insn.vA, verifier_->GetRegTypeCache()->Boolean());
        return;
      }
    }
    SetRegisterType(dec_insn.vA, dst_type);
  }
}

void RegisterLine::CheckBinaryOp2addr(const DecodedInstruction& dec_insn,
                                      const RegType& dst_type, const RegType& src_type1,
                                      const RegType& src_type2, bool check_boolean_op) {
  if (VerifyRegisterType(dec_insn.vA, src_type1) &&
      VerifyRegisterType(dec_insn.vB, src_type2)) {
    if (check_boolean_op) {
      DCHECK(dst_type.IsInteger());
      if (GetRegisterType(dec_insn.vA).IsBooleanTypes() &&
          GetRegisterType(dec_insn.vB).IsBooleanTypes()) {
        SetRegisterType(dec_insn.vA, verifier_->GetRegTypeCache()->Boolean());
        return;
      }
    }
    SetRegisterType(dec_insn.vA, dst_type);
  }
}

void RegisterLine::CheckLiteralOp(const DecodedInstruction& dec_insn,
                                  const RegType& dst_type, const RegType& src_type,
                                  bool check_boolean_op) {
  if (VerifyRegisterType(dec_insn.vB, src_type)) {
    if (check_boolean_op) {
      DCHECK(dst_type.IsInteger());
      /* check vB with the call, then check the constant manually */
      if (GetRegisterType(dec_insn.vB).IsBooleanTypes() &&
          (dec_insn.vC == 0 || dec_insn.vC == 1)) {
        SetRegisterType(dec_insn.vA, verifier_->GetRegTypeCache()->Boolean());
        return;
      }
    }
    SetRegisterType(dec_insn.vA, dst_type);
  }
}

void RegisterLine::PushMonitor(uint32_t reg_idx, int32_t insn_idx) {
  const RegType& reg_type = GetRegisterType(reg_idx);
  if (!reg_type.IsReferenceTypes()) {
    verifier_->Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "monitor-enter on non-object (" << reg_type << ")";
  } else if (monitors_.size() >= 32) {
    verifier_->Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "monitor-enter stack overflow: " << monitors_.size();
  } else {
    SetRegToLockDepth(reg_idx, monitors_.size());
    monitors_.push_back(insn_idx);
  }
}

void RegisterLine::PopMonitor(uint32_t reg_idx) {
  const RegType& reg_type = GetRegisterType(reg_idx);
  if (!reg_type.IsReferenceTypes()) {
    verifier_->Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "monitor-exit on non-object (" << reg_type << ")";
  } else if (monitors_.empty()) {
    verifier_->Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "monitor-exit stack underflow";
  } else {
    monitors_.pop_back();
    if (!IsSetLockDepth(reg_idx, monitors_.size())) {
      // Bug 3215458: Locks and unlocks are on objects, if that object is a literal then before
      // format "036" the constant collector may create unlocks on the same object but referenced
      // via different registers.
      ((verifier_->DexFileVersion() >= 36) ? verifier_->Fail(VERIFY_ERROR_BAD_CLASS_SOFT)
                                           : verifier_->LogVerifyInfo())
            << "monitor-exit not unlocking the top of the monitor stack";
    } else {
      // Record the register was unlocked
      ClearRegToLockDepth(reg_idx, monitors_.size());
    }
  }
}

bool RegisterLine::VerifyMonitorStackEmpty() {
  if (MonitorStackDepth() != 0) {
    verifier_->Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "expected empty monitor stack";
    return false;
  } else {
    return true;
  }
}

bool RegisterLine::MergeRegisters(const RegisterLine* incoming_line) {
  bool changed = false;
  for (size_t idx = 0; idx < num_regs_; idx++) {
    if (line_[idx] != incoming_line->line_[idx]) {
      const RegType& incoming_reg_type = incoming_line->GetRegisterType(idx);
      const RegType& cur_type = GetRegisterType(idx);
      const RegType& new_type = cur_type.Merge(incoming_reg_type, verifier_->GetRegTypeCache());
      changed = changed || !cur_type.Equals(new_type);
      line_[idx] = new_type.GetId();
    }
  }
  if (monitors_.size() != incoming_line->monitors_.size()) {
    verifier_->Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "mismatched stack depths (depth="
        << MonitorStackDepth() << ", incoming depth=" << incoming_line->MonitorStackDepth() << ")";
  } else if (reg_to_lock_depths_ != incoming_line->reg_to_lock_depths_) {
    for (uint32_t idx = 0; idx < num_regs_; idx++) {
      size_t depths = reg_to_lock_depths_.count(idx);
      size_t incoming_depths = incoming_line->reg_to_lock_depths_.count(idx);
      if (depths != incoming_depths) {
        if (depths == 0 || incoming_depths == 0) {
          reg_to_lock_depths_.erase(idx);
        } else {
          verifier_->Fail(VERIFY_ERROR_BAD_CLASS_HARD) << "mismatched stack depths for register v" << idx
                                                       << ": " << depths  << " != " << incoming_depths;
          break;
        }
      }
    }
  }
  return changed;
}

void RegisterLine::WriteReferenceBitMap(std::vector<uint8_t>& data, size_t max_bytes) {
  for (size_t i = 0; i < num_regs_; i += 8) {
    uint8_t val = 0;
    for (size_t j = 0; j < 8 && (i + j) < num_regs_; j++) {
      // Note: we write 1 for a Reference but not for Null
      if (GetRegisterType(i + j).IsNonZeroReferenceTypes()) {
        val |= 1 << j;
      }
    }
    if ((i / 8) >= max_bytes) {
      DCHECK_EQ(0, val);
      continue;
    }
    DCHECK_LT(i / 8, max_bytes) << "val=" << static_cast<uint32_t>(val);
    data.push_back(val);
  }
}

std::ostream& operator<<(std::ostream& os, const RegisterLine& rhs) {
  os << rhs.Dump();
  return os;
}

}  // namespace verifier
}  // namespace art
