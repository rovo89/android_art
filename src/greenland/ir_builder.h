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

#ifndef ART_SRC_GREENLAND_IR_BUILDER_H_
#define ART_SRC_GREENLAND_IR_BUILDER_H_

#include "backend_types.h"
#include "intrinsic_helper.h"

#include "logging.h"

#include <llvm/Support/IRBuilder.h>
#include <llvm/Support/NoFolder.h>
#include <llvm/Metadata.h>

namespace llvm {
  class Module;
}

namespace art {
namespace greenland {

class InserterWithDexOffset
   : public llvm::IRBuilderDefaultInserter<true> {
  public:
    InserterWithDexOffset() : node_(NULL) {}
    void InsertHelper(llvm::Instruction *I, const llvm::Twine &Name,
                      llvm::BasicBlock *BB,
                      llvm::BasicBlock::iterator InsertPt) const {
      llvm::IRBuilderDefaultInserter<true>::InsertHelper(I, Name, BB, InsertPt);
      if (node_ != NULL) {
        I->setMetadata("DexOff", node_);
      }
    }
    void SetDexOffset(llvm::MDNode* node) {
      node_ = node;
    }
  private:
    llvm::MDNode* node_;
};

typedef llvm::IRBuilder<true, llvm::NoFolder, InserterWithDexOffset> LLVMIRBuilder;

class IRBuilder : public LLVMIRBuilder {
 public:
  IRBuilder(llvm::LLVMContext& context, llvm::Module& module,
            IntrinsicHelper& intrinsic_helper);
  ~IRBuilder() { }

 public:
  //--------------------------------------------------------------------------
  // Pointer Arithmetic Helper Function
  //--------------------------------------------------------------------------
  llvm::IntegerType* GetPtrEquivIntTy() {
    return getInt32Ty();
  }

  llvm::ConstantInt* GetPtrEquivInt(int64_t i) {
    return llvm::ConstantInt::get(GetPtrEquivIntTy(), i);
  }

  //--------------------------------------------------------------------------
  // Intrinsic Helper Functions
  //--------------------------------------------------------------------------
  llvm::Function* GetIntrinsics(IntrinsicHelper::IntrinsicId instr_id) {
    return intrinsic_helper_.GetIntrinsicFunction(instr_id);
  }

  //--------------------------------------------------------------------------
  // Type Helper Functions
  //--------------------------------------------------------------------------
  llvm::Type* GetJType(char shorty_jty, JTypeSpace space) {
    return GetJType(GetJTypeFromShorty(shorty_jty), space);
  }

  llvm::Type* GetJType(JType jty, JTypeSpace space) {
    switch (space) {
    case kAccurate:
      return GetJTypeInAccurateSpace(jty);

    case kReg:
    case kField: // Currently field space is equivalent to register space.
      return GetJTypeInRegSpace(jty);

    case kArray:
      return GetJTypeInArraySpace(jty);
    }

    LOG(FATAL) << "Unknown type space: " << space;
    return NULL;
  }

  llvm::Type* GetJVoidTy() {
    return getVoidTy();
  }

  llvm::IntegerType* GetJBooleanTy() {
    return getInt1Ty();
  }

  llvm::IntegerType* GetJByteTy() {
    return getInt8Ty();
  }

  llvm::IntegerType* GetJCharTy() {
    return getInt16Ty();
  }

  llvm::IntegerType* GetJShortTy() {
    return getInt16Ty();
  }

  llvm::IntegerType* GetJIntTy() {
    return getInt32Ty();
  }

  llvm::IntegerType* GetJLongTy() {
    return getInt64Ty();
  }

  llvm::Type* GetJFloatTy() {
    return getFloatTy();
  }

  llvm::Type* GetJDoubleTy() {
    return getDoubleTy();
  }

  llvm::PointerType* GetJObjectTy() {
    return java_object_type_;
  }

  llvm::PointerType* GetJMethodTy() {
    return java_method_type_;
  }

  llvm::PointerType* GetJThreadTy() {
    return java_thread_type_;
  }

  //--------------------------------------------------------------------------
  // Constant Value Helper Function
  //--------------------------------------------------------------------------
  llvm::ConstantInt* GetJBoolean(bool is_true) {
    return (is_true) ? getTrue() : getFalse();
  }

  llvm::ConstantInt* GetJByte(int8_t i) {
    return llvm::ConstantInt::getSigned(GetJByteTy(), i);
  }

  llvm::ConstantInt* GetJChar(int16_t i) {
    return llvm::ConstantInt::getSigned(GetJCharTy(), i);
  }

  llvm::ConstantInt* GetJShort(int16_t i) {
    return llvm::ConstantInt::getSigned(GetJShortTy(), i);
  }

  llvm::ConstantInt* GetJInt(int32_t i) {
    return llvm::ConstantInt::getSigned(GetJIntTy(), i);
  }

  llvm::ConstantInt* GetJLong(int64_t i) {
    return llvm::ConstantInt::getSigned(GetJLongTy(), i);
  }

  llvm::Constant* GetJFloat(float f) {
    return llvm::ConstantFP::get(GetJFloatTy(), f);
  }

  llvm::Constant* GetJDouble(double d) {
    return llvm::ConstantFP::get(GetJDoubleTy(), d);
  }

  llvm::ConstantPointerNull* GetJNull() {
    return llvm::ConstantPointerNull::get(GetJObjectTy());
  }

  llvm::Constant* GetJZero(char shorty_jty) {
    return GetJZero(GetJTypeFromShorty(shorty_jty));
  }

  llvm::Constant* GetJZero(JType jty) {
    switch (jty) {
    case kVoid:
      LOG(FATAL) << "Zero is not a value of void type";
      return NULL;

    case kBoolean:
      return GetJBoolean(false);

    case kByte:
      return GetJByte(0);

    case kChar:
      return GetJChar(0);

    case kShort:
      return GetJShort(0);

    case kInt:
      return GetJInt(0);

    case kLong:
      return GetJLong(0);

    case kFloat:
      return GetJFloat(0.0f);

    case kDouble:
      return GetJDouble(0.0);

    case kObject:
      return GetJNull();

    default:
      LOG(FATAL) << "Unknown java type: " << jty;
      return NULL;
    }
  }

 private:
  llvm::Type* GetJTypeInAccurateSpace(JType jty);
  llvm::Type* GetJTypeInRegSpace(JType jty);
  llvm::Type* GetJTypeInArraySpace(JType jty);

  llvm::PointerType* java_object_type_;
  llvm::PointerType* java_method_type_;
  llvm::PointerType* java_thread_type_;

  IntrinsicHelper& intrinsic_helper_;
};

} // namespace greenland
} // namespace art

#endif // ART_SRC_GREENLAND_IR_BUILDER_H_
