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

#include "dex_lang.h"
#include "ir_builder.h"
#include "intrinsic_helper.h"

#include <llvm/Function.h>

using namespace art;
using namespace art::greenland;

namespace {

  class DalvikArgReg : public DalvikReg {
   public:
    DalvikArgReg(DexLang& dex_lang, unsigned reg_idx, JType jty);

    virtual llvm::Value* GetValue(JType jty, JTypeSpace space);
    virtual void SetValue(JType jty, JTypeSpace space, llvm::Value* value);

   private:
    llvm::Value* reg_addr_;
    JType jty_;

    inline void CheckJType(JType jty) const {
      CHECK_EQ(jty, jty_) << "Get value of type " << jty << " from Dalvik "
                             "argument register v" << reg_idx_ << "(type: "
                          << jty_ << ") without type coercion!";
      return;
    }
  };

  class DalvikLocalVarReg : public DalvikReg {
   public:
    DalvikLocalVarReg(DexLang& dex_lang, unsigned reg_idx);

    virtual llvm::Value* GetValue(JType jty, JTypeSpace space);
    virtual void SetValue(JType jty, JTypeSpace space, llvm::Value* value);

   private:
    llvm::Value* GetRawAddr(RegCategory cat);

   private:
    llvm::Value* reg_32_;
    llvm::Value* reg_64_;
    llvm::Value* reg_obj_;
  };
} // anonymous namespace


//----------------------------------------------------------------------------
// Dalvik Register
//----------------------------------------------------------------------------

DalvikReg* DalvikReg::CreateArgReg(DexLang& dex_lang, unsigned reg_idx,
                                   JType jty) {
  return new DalvikArgReg(dex_lang, reg_idx, jty);
}

DalvikReg* DalvikReg::CreateLocalVarReg(DexLang& dex_lang, unsigned reg_idx) {
  return new DalvikLocalVarReg(dex_lang, reg_idx);
}

DalvikReg::DalvikReg(DexLang& dex_lang, unsigned reg_idx)
    : dex_lang_(dex_lang), irb_(dex_lang.GetIRBuilder()), reg_idx_(reg_idx),
      shadow_frame_entry_idx_(-1) {
}

void DalvikReg::SetShadowEntry(llvm::Value* root_object) {
  if (shadow_frame_entry_idx_ < 0) {
    shadow_frame_entry_idx_ = dex_lang_.AllocShadowFrameEntry(reg_idx_);
  }

  irb_.CreateCall2(irb_.GetIntrinsics(IntrinsicHelper::SetShadowFrameEntry),
                  root_object, irb_.getInt32(shadow_frame_entry_idx_));

  return;
}

//----------------------------------------------------------------------------
// Dalvik Argument Register
//----------------------------------------------------------------------------
DalvikArgReg::DalvikArgReg(DexLang& dex_lang, unsigned reg_idx, JType jty)
    : DalvikReg(dex_lang, reg_idx), jty_(jty) {
  reg_addr_ = dex_lang_.AllocateDalvikReg(jty, reg_idx);
  DCHECK(reg_addr_ != NULL);
}

llvm::Value* DalvikArgReg::GetValue(JType jty, JTypeSpace space) {
  DCHECK_NE(jty, kVoid) << "Dalvik register will never be void type";

  switch (space) {
    case kReg:
    case kField: {
      // Currently, kField is almost the same with kReg.
      RegCategory cat = GetRegCategoryFromJType(jty_);
      CHECK_EQ(cat, GetRegCategoryFromJType(jty)) << "Get value of type " << jty
                                                  << "has different register "
                                                     "category from the value "
                                                     "contained in the register"
                                                  << reg_idx_ << "(type: "
                                                  << jty_ << ")";
      switch (jty_) {
        case kVoid: {
          break;
        }
        case kBoolean:
        case kChar: {
          return irb_.CreateZExt(irb_.CreateLoad(reg_addr_), irb_.GetJIntTy());
        }
        case kByte:
        case kShort: {
          return irb_.CreateSExt(irb_.CreateLoad(reg_addr_), irb_.GetJIntTy());
        }
        case kFloat: {
          return irb_.CreateBitCast(irb_.CreateLoad(reg_addr_),
                                    irb_.GetJIntTy());
        }
        case kDouble: {
          return irb_.CreateBitCast(irb_.CreateLoad(reg_addr_),
                                    irb_.GetJLongTy());
        }
        case kInt:
        case kLong:
        case kObject: {
          return irb_.CreateLoad(reg_addr_);
        }
        default: {
          LOG(FATAL) << "Unexpected register type: " << jty;
          break;
        }
      }
      break;
    }
    case kArray: {
      switch (jty) {
        case kVoid: {
          LOG(FATAL) << "Dalvik register with void type has no value";
          return NULL;
        }
        case kBoolean: {
          CheckJType(jty);
          // NOTE: In array type space, boolean is i8, while in accurate type
          // space, boolean is i1. For the other cases, array type space is
          // equal to accurate type space.
          return irb_.CreateZExt(irb_.CreateLoad(reg_addr_), irb_.GetJByteTy());
        }
        case kByte:
        case kChar:
        case kShort:
        case kInt:
        case kLong:
        case kFloat:
        case kDouble:
        case kObject: {
          CheckJType(jty);
          return irb_.CreateLoad(reg_addr_);
        }
        default: {
          LOG(FATAL) << "Unexpected register type: " << jty;
          break;
        }
      }
    }
    case kAccurate: {
      CheckJType(jty);
      return irb_.CreateLoad(reg_addr_);
    }
  }
  return NULL;
}

void DalvikArgReg::SetValue(JType jty, JTypeSpace space, llvm::Value* value) {
  if ((jty == jty_) && (space == kAccurate)) {
    irb_.CreateStore(value, reg_addr_);
    if (jty == kObject) {
      SetShadowEntry(value);
    }
  } else {
    LOG(FATAL) << "Normal .dex file doesn't use argument register for method-"
                  "local variable!";
  }
  return;
}

//----------------------------------------------------------------------------
// Dalvik Local Variable Register
//----------------------------------------------------------------------------

DalvikLocalVarReg::DalvikLocalVarReg(DexLang& dex_lang, unsigned reg_idx)
    : DalvikReg(dex_lang, reg_idx), reg_32_(NULL), reg_64_(NULL),
      reg_obj_(NULL) {
}

llvm::Value* DalvikLocalVarReg::GetRawAddr(RegCategory cat) {
  switch (cat) {
    case kRegCat1nr: {
      if (reg_32_ == NULL) {
        reg_32_ = dex_lang_.AllocateDalvikReg(kInt, reg_idx_);
      }
      return reg_32_;
    }
    case kRegCat2: {
      if (reg_64_ == NULL) {
        reg_64_ = dex_lang_.AllocateDalvikReg(kLong, reg_idx_);
      }
      return reg_64_;
    }
    case kRegObject: {
      if (reg_obj_ == NULL) {
        reg_obj_ = dex_lang_.AllocateDalvikReg(kObject, reg_idx_);
      }
      return reg_obj_;
    }
    default: {
      LOG(FATAL) << "Unexpected register category: " << cat;
      return NULL;
    }
  }
  return NULL;
}

llvm::Value* DalvikLocalVarReg::GetValue(JType jty, JTypeSpace space) {
  DCHECK_NE(jty, kVoid) << "Dalvik register will never be void type";

  switch (space) {
    case kReg:
    case kField: {
      // float and double require bitcast to get their value from the integer
      // register.
      DCHECK((jty != kFloat) && (jty != kDouble));
      return irb_.CreateLoad(GetRawAddr(GetRegCategoryFromJType(jty)));
    }
    case kAccurate:
    case kArray: {
      switch (jty) {
        case kVoid: {
          LOG(FATAL) << "Dalvik register with void type has no value";
          return NULL;
        }
        case kBoolean:
        case kChar:
        case kByte:
        case kShort: {
          // NOTE: In array type space, boolean is truncated from i32 to i8,
          // while in accurate type space, boolean is truncated from i32 to i1.
          // For the other cases, array type space is equal to accurate type
          // space.
          return irb_.CreateTrunc(irb_.CreateLoad(GetRawAddr(kRegCat1nr)),
                                  irb_.GetJType(jty, space));
        }
        case kFloat: {
          return irb_.CreateBitCast(irb_.CreateLoad(GetRawAddr(kRegCat1nr)),
                                    irb_.GetJType(jty, space));
        }
        case kDouble: {
          return irb_.CreateBitCast(irb_.CreateLoad(GetRawAddr(kRegCat2)),
                                    irb_.GetJType(jty, space));
        }
        case kInt:
        case kLong:
        case kObject: {
          return irb_.CreateLoad(GetRawAddr(GetRegCategoryFromJType(jty)));
        }
        default: {
          LOG(FATAL) << "Unexpected register type: " << jty;
          break;
        }
      }
    }
    default: {
      LOG(FATAL) << "Unexpected register space: " << space;
      break;
    }
  }
  return NULL;
}

void DalvikLocalVarReg::SetValue(JType jty, JTypeSpace space,
                                 llvm::Value* value) {
  DCHECK_NE(jty, kVoid) << "Dalvik register should never hold void type";

  if (jty == kObject) {
    SetShadowEntry(value);
  }

  switch (space) {
    case kReg:
    case kField: {
      // float and double require bitcast to get their value from the integer
      // register.
      DCHECK((jty != kFloat) && (jty != kDouble));
      irb_.CreateStore(value, GetRawAddr(GetRegCategoryFromJType(jty)));
      return;
    }
    case kAccurate:
    case kArray: {
      switch (jty) {
        case kVoid: {
          break;
        }
        case kBoolean:
        case kChar: {
          // NOTE: In accurate type space, we have to zero extend boolean from
          // i1 to i32, and char from i16 to i32.  In array type space, we have
          // to zero extend boolean from i8 to i32, and char from i16 to i32.
          value = irb_.CreateZExt(value, irb_.GetJIntTy());
          irb_.CreateStore(value, GetRawAddr(kRegCat1nr));
          break;
        }
        case kByte:
        case kShort: {
          // NOTE: In accurate type space, we have to signed extend byte from
          // i8 to i32, and short from i16 to i32.  In array type space, we have
          // to sign extend byte from i8 to i32, and short from i16 to i32.
          value = irb_.CreateSExt(value, irb_.GetJIntTy());
          irb_.CreateStore(value, GetRawAddr(kRegCat1nr));
          break;
        }
        case kFloat: {
          value = irb_.CreateBitCast(value, irb_.GetJIntTy());
          irb_.CreateStore(value, GetRawAddr(kRegCat1nr));
          break;
        }
        case kDouble: {
          value = irb_.CreateBitCast(value, irb_.GetJLongTy());
          irb_.CreateStore(value, GetRawAddr(kRegCat2));
          break;
        }
        case kInt:
        case kLong:
        case kObject: {
          irb_.CreateStore(value, GetRawAddr(GetRegCategoryFromJType(jty)));
          break;
        }
        default: {
          LOG(FATAL) << "Unexpected register type: " << jty;
          return;
        }
      }
      return;
    }
    default: {
      LOG(FATAL) << "Unexpected register space: " << space;
      return;
    }
  }
}

