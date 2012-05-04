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
#include "method_compiler.h"

using namespace art;
using namespace art::compiler_llvm;


namespace {

  class DalvikLocalVarReg : public DalvikReg {
   public:
    DalvikLocalVarReg(MethodCompiler& method_compiler, uint32_t reg_idx);

    virtual void SetValue(JType jty, JTypeSpace space, llvm::Value* value);

    virtual ~DalvikLocalVarReg();

   private:
    virtual llvm::Value* GetRawAddr(JType jty, JTypeSpace space);

   private:
    uint32_t reg_idx_;
    llvm::Value* reg_32_;
    llvm::Value* reg_64_;
    llvm::Value* reg_obj_;
    llvm::Value* reg_shadow_frame_;
  };

  class DalvikRetValReg : public DalvikReg {
   public:
    DalvikRetValReg(MethodCompiler& method_compiler);

    virtual ~DalvikRetValReg();

   private:
    virtual llvm::Value* GetRawAddr(JType jty, JTypeSpace space);

   private:
    llvm::Value* reg_32_;
    llvm::Value* reg_64_;
    llvm::Value* reg_obj_;
  };

} // anonymous namespace


//----------------------------------------------------------------------------
// Dalvik Register
//----------------------------------------------------------------------------

DalvikReg* DalvikReg::CreateLocalVarReg(MethodCompiler& method_compiler,
                                        uint32_t reg_idx) {
  return new DalvikLocalVarReg(method_compiler, reg_idx);
}


DalvikReg* DalvikReg::CreateRetValReg(MethodCompiler& method_compiler) {
  return new DalvikRetValReg(method_compiler);
}


DalvikReg::DalvikReg(MethodCompiler& method_compiler)
: method_compiler_(&method_compiler), irb_(method_compiler.GetIRBuilder()) {
}


DalvikReg::~DalvikReg() {
}


inline llvm::Value* DalvikReg::RegCat1SExt(llvm::Value* value) {
  return irb_.CreateSExt(value, irb_.getJIntTy());
}


inline llvm::Value* DalvikReg::RegCat1ZExt(llvm::Value* value) {
  return irb_.CreateZExt(value, irb_.getJIntTy());
}


inline llvm::Value* DalvikReg::RegCat1Trunc(llvm::Value* value,
                                            llvm::Type* ty) {
  return irb_.CreateTrunc(value, ty);
}


llvm::Value* DalvikReg::GetValue(JType jty, JTypeSpace space) {
  DCHECK_NE(jty, kVoid) << "Dalvik register will never be void type";

  switch (space) {
  case kReg:
  case kField:
    return irb_.CreateLoad(GetAddr(jty, space));

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
      return RegCat1Trunc(irb_.CreateLoad(GetAddr(jty, space)),
                          irb_.getJType(jty, space));

    case kInt:
    case kLong:
    case kFloat:
    case kDouble:
    case kObject:
      return irb_.CreateLoad(GetAddr(jty, space));
    }
  }

  LOG(FATAL) << "Couldn't GetValue of JType " << jty;
  return NULL;
}


void DalvikReg::SetValue(JType jty, JTypeSpace space, llvm::Value* value) {
  DCHECK_NE(jty, kVoid) << "Dalvik register will never be void type";

  switch (space) {
  case kReg:
  case kField:
    irb_.CreateStore(value, GetAddr(jty, space));
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
      irb_.CreateStore(RegCat1ZExt(value), GetAddr(jty, space));
      break;

    case kByte:
    case kShort:
      // NOTE: In accurate type space, we have to signed extend byte from
      // i8 to i32, and short from i16 to i32.  In array type space, we have
      // to sign extend byte from i8 to i32, and short from i16 to i32.
      irb_.CreateStore(RegCat1SExt(value), GetAddr(jty, space));
      break;

    case kInt:
    case kLong:
    case kFloat:
    case kDouble:
    case kObject:
      irb_.CreateStore(value, GetAddr(jty, space));
      break;
    }
  }
}


llvm::Value* DalvikReg::GetAddr(JType jty, JTypeSpace space) {
  if (jty == kFloat) {
    return irb_.CreateBitCast(GetRawAddr(jty, space),
                              irb_.getJFloatTy()->getPointerTo());
  } else if (jty == kDouble) {
    return irb_.CreateBitCast(GetRawAddr(jty, space),
                              irb_.getJDoubleTy()->getPointerTo());
  } else {
    return GetRawAddr(jty, space);
  }
}


//----------------------------------------------------------------------------
// Dalvik Local Variable Register
//----------------------------------------------------------------------------

DalvikLocalVarReg::DalvikLocalVarReg(MethodCompiler& method_compiler,
                                     uint32_t reg_idx)
: DalvikReg(method_compiler), reg_idx_(reg_idx),
  reg_32_(NULL), reg_64_(NULL), reg_obj_(NULL), reg_shadow_frame_(NULL) {
}


DalvikLocalVarReg::~DalvikLocalVarReg() {
}


void DalvikLocalVarReg::SetValue(JType jty, JTypeSpace space, llvm::Value* value) {
  DalvikReg::SetValue(jty, space, value);

  if (jty == kObject) {
    DCHECK_NE(reg_shadow_frame_, static_cast<llvm::Value*>(NULL))
      << "Didn't allocate shadow frame entry.";
    irb_.CreateStore(value, reg_shadow_frame_);
  }
}


llvm::Value* DalvikLocalVarReg::GetRawAddr(JType jty, JTypeSpace space) {
  switch (GetRegCategoryFromJType(jty)) {
  case kRegCat1nr:
    if (reg_32_ == NULL) {
      reg_32_ = method_compiler_->AllocDalvikLocalVarReg(kRegCat1nr, reg_idx_);
    }
    return reg_32_;

  case kRegCat2:
    if (reg_64_ == NULL) {
      reg_64_ = method_compiler_->AllocDalvikLocalVarReg(kRegCat2, reg_idx_);
    }
    return reg_64_;

  case kRegObject:
    if (reg_obj_ == NULL) {
      reg_obj_ = method_compiler_->AllocDalvikLocalVarReg(kRegObject, reg_idx_);
      reg_shadow_frame_ = method_compiler_->AllocShadowFrameEntry(reg_idx_);
    }
    return reg_obj_;

  default:
    LOG(FATAL) << "Unexpected register category: "
               << GetRegCategoryFromJType(jty);
    return NULL;
  }
}


//----------------------------------------------------------------------------
// Dalvik Returned Value Temporary Register
//----------------------------------------------------------------------------

DalvikRetValReg::DalvikRetValReg(MethodCompiler& method_compiler)
: DalvikReg(method_compiler), reg_32_(NULL), reg_64_(NULL), reg_obj_(NULL) {
}


DalvikRetValReg::~DalvikRetValReg() {
}


llvm::Value* DalvikRetValReg::GetRawAddr(JType jty, JTypeSpace space) {
  switch (GetRegCategoryFromJType(jty)) {
  case kRegCat1nr:
    if (reg_32_ == NULL) {
      reg_32_ = method_compiler_->AllocDalvikRetValReg(kRegCat1nr);
    }
    return reg_32_;

  case kRegCat2:
    if (reg_64_ == NULL) {
      reg_64_ = method_compiler_->AllocDalvikRetValReg(kRegCat2);
    }
    return reg_64_;

  case kRegObject:
    if (reg_obj_ == NULL) {
      reg_obj_ = method_compiler_->AllocDalvikRetValReg(kRegObject);
    }
    return reg_obj_;

  default:
    LOG(FATAL) << "Unexpected register category: "
               << GetRegCategoryFromJType(jty);
    return NULL;
  }
}
