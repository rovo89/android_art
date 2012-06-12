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

#include "dalvik_reg.h"

#include "ir_builder.h"
#include "dex_lang.h"

namespace art {
namespace greenland {

//----------------------------------------------------------------------------
// Dalvik Register
//----------------------------------------------------------------------------

DalvikReg::DalvikReg(DexLang& dex_lang, unsigned reg_idx)
: dex_lang_(dex_lang), irb_(dex_lang.GetIRBuilder()),
  reg_idx_(reg_idx), shadow_frame_entry_idx_(-1),
  reg_32_(NULL), reg_64_(NULL), reg_obj_(NULL) {
}

DalvikReg::~DalvikReg() {
}

void DalvikReg::SetShadowEntry(llvm::Value* object) {
  if (shadow_frame_entry_idx_ < 0) {
    shadow_frame_entry_idx_ = dex_lang_.AllocShadowFrameEntry(reg_idx_);
  }

  irb_.CreateCall2(irb_.GetIntrinsics(IntrinsicHelper::SetShadowFrameEntry),
                   object, irb_.getInt32(shadow_frame_entry_idx_));

  return;
}

llvm::Type* DalvikReg::GetRegCategoryEquivSizeTy(IRBuilder& irb, RegCategory reg_cat) {
  switch (reg_cat) {
  case kRegCat1nr:  return irb.GetJIntTy();
  case kRegCat2:    return irb.GetJLongTy();
  case kRegObject:  return irb.GetJObjectTy();
  default:
    LOG(FATAL) << "Unknown register category: " << reg_cat;
    return NULL;
  }
}

char DalvikReg::GetRegCategoryNamePrefix(RegCategory reg_cat) {
  switch (reg_cat) {
  case kRegCat1nr:  return 'r';
  case kRegCat2:    return 'w';
  case kRegObject:  return 'p';
  default:
    LOG(FATAL) << "Unknown register category: " << reg_cat;
    return '\0';
  }
}

inline llvm::Value* DalvikReg::RegCat1SExt(llvm::Value* value) {
  return irb_.CreateSExt(value, irb_.GetJIntTy());
}

inline llvm::Value* DalvikReg::RegCat1ZExt(llvm::Value* value) {
  return irb_.CreateZExt(value, irb_.GetJIntTy());
}

inline llvm::Value* DalvikReg::RegCat1Trunc(llvm::Value* value,
                                            llvm::Type* ty) {
  return irb_.CreateTrunc(value, ty);
}

llvm::Value* DalvikReg::GetValue(JType jty, JTypeSpace space) {
  DCHECK_NE(jty, kVoid) << "Dalvik register will never be void type";

  llvm::Value* value = NULL;
  switch (space) {
  case kReg:
  case kField:
    value = irb_.CreateLoad(GetAddr(jty));
    break;

  case kAccurate:
  case kArray:
    switch (jty) {
    case kVoid:
      LOG(FATAL) << "Dalvik register with void type has no value";
      return NULL;

    case kBoolean:
    case kChar:
    case kByte:
    case kShort:
      // NOTE: In array type space, boolean is truncated from i32 to i8, while
      // in accurate type space, boolean is truncated from i32 to i1.
      // For the other cases, array type space is equal to accurate type space.
      value = RegCat1Trunc(irb_.CreateLoad(GetAddr(jty)),
                           irb_.GetJType(jty, space));
      break;

    case kInt:
    case kLong:
    case kFloat:
    case kDouble:
    case kObject:
      value = irb_.CreateLoad(GetAddr(jty));
      break;

    default:
      LOG(FATAL) << "Unknown java type: " << jty;
      return NULL;
    }
    break;

  default:
    LOG(FATAL) << "Couldn't GetValue of JType " << jty;
    return NULL;
  }

  if (jty == kFloat || jty == kDouble) {
    value = irb_.CreateBitCast(value, irb_.GetJType(jty, space));
  }
  return value;
}

void DalvikReg::SetValue(JType jty, JTypeSpace space, llvm::Value* value) {
  DCHECK_NE(jty, kVoid) << "Dalvik register will never be void type";

  if (jty == kObject) {
    SetShadowEntry(value);
  } else if (jty == kFloat || jty == kDouble) {
    value = irb_.CreateBitCast(value, irb_.GetJType(jty, kReg));
  }

  switch (space) {
  case kReg:
  case kField:
    irb_.CreateStore(value, GetAddr(jty));
    return;

  case kAccurate:
  case kArray:
    switch (jty) {
    case kVoid:
      break;

    case kBoolean:
    case kChar:
      // NOTE: In accurate type space, we have to zero extend boolean from
      // i1 to i32, and char from i16 to i32.  In array type space, we have
      // to zero extend boolean from i8 to i32, and char from i16 to i32.
      irb_.CreateStore(RegCat1ZExt(value), GetAddr(jty));
      break;

    case kByte:
    case kShort:
      // NOTE: In accurate type space, we have to signed extend byte from
      // i8 to i32, and short from i16 to i32.  In array type space, we have
      // to sign extend byte from i8 to i32, and short from i16 to i32.
      irb_.CreateStore(RegCat1SExt(value), GetAddr(jty));
      break;

    case kInt:
    case kLong:
    case kFloat:
    case kDouble:
    case kObject:
      irb_.CreateStore(value, GetAddr(jty));
      break;

    default:
      LOG(FATAL) << "Unknown java type: " << jty;
    }
  }
}

llvm::Value* DalvikReg::GetAddr(JType jty) {
  switch (GetRegCategoryFromJType(jty)) {
  case kRegCat1nr:
    if (reg_32_ == NULL) {
      reg_32_ = dex_lang_.AllocateDalvikReg(kRegCat1nr, reg_idx_);
    }
    return reg_32_;

  case kRegCat2:
    if (reg_64_ == NULL) {
      reg_64_ = dex_lang_.AllocateDalvikReg(kRegCat2, reg_idx_);
    }
    return reg_64_;

  case kRegObject:
    if (reg_obj_ == NULL) {
      reg_obj_ = dex_lang_.AllocateDalvikReg(kRegObject, reg_idx_);
    }
    return reg_obj_;

  default:
    LOG(FATAL) << "Unexpected register category: "
               << GetRegCategoryFromJType(jty);
    return NULL;
  }
}

} // namespace greenland
} // namespace art
