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

#ifndef ART_SRC_COMPILER_LLVM_IR_BUILDER_H_
#define ART_SRC_COMPILER_LLVM_IR_BUILDER_H_

#include "backend_types.h"
#include "runtime_support_builder.h"
#include "runtime_support_func.h"

#include <llvm/Constants.h>
#include <llvm/DerivedTypes.h>
#include <llvm/Support/IRBuilder.h>
#include <llvm/Type.h>

#include <stdint.h>


namespace art {
namespace compiler_llvm {


typedef llvm::IRBuilder<> LLVMIRBuilder;
// NOTE: Here we define our own LLVMIRBuilder type alias, so that we can
// switch "preserveNames" template parameter easily.


class IRBuilder : public LLVMIRBuilder {
 public:
  //--------------------------------------------------------------------------
  // General
  //--------------------------------------------------------------------------

  IRBuilder(llvm::LLVMContext& context, llvm::Module& module);


  //--------------------------------------------------------------------------
  // Pointer Arithmetic Helper Function
  //--------------------------------------------------------------------------

  llvm::IntegerType* getPtrEquivIntTy() {
    return getInt32Ty();
  }

  size_t getSizeOfPtrEquivInt() {
    return 4;
  }

  llvm::ConstantInt* getSizeOfPtrEquivIntValue() {
    return getPtrEquivInt(getSizeOfPtrEquivInt());
  }

  llvm::ConstantInt* getPtrEquivInt(uint64_t i) {
    return llvm::ConstantInt::get(getPtrEquivIntTy(), i);
  }

  llvm::Value* CreatePtrDisp(llvm::Value* base,
                             llvm::Value* offset,
                             llvm::PointerType* ret_ty) {

    llvm::Value* base_int = CreatePtrToInt(base, getPtrEquivIntTy());
    llvm::Value* result_int = CreateAdd(base_int, offset);
    llvm::Value* result = CreateIntToPtr(result_int, ret_ty);

    return result;
  }

  llvm::Value* CreatePtrDisp(llvm::Value* base,
                             llvm::Value* bs,
                             llvm::Value* count,
                             llvm::Value* offset,
                             llvm::PointerType* ret_ty) {

    llvm::Value* block_offset = CreateMul(bs, count);
    llvm::Value* total_offset = CreateAdd(block_offset, offset);

    return CreatePtrDisp(base, total_offset, ret_ty);
  }

  llvm::Value* LoadFromObjectOffset(llvm::Value* object_addr, int32_t offset, llvm::Type* type) {
    // Convert offset to llvm::value
    llvm::Value* llvm_offset = getPtrEquivInt(offset);
    // Calculate the value's address
    llvm::Value* value_addr = CreatePtrDisp(object_addr, llvm_offset, type->getPointerTo());
    // Load
    return CreateLoad(value_addr);
  }

  void StoreToObjectOffset(llvm::Value* object_addr, int32_t offset, llvm::Value* new_value) {
    // Convert offset to llvm::value
    llvm::Value* llvm_offset = getPtrEquivInt(offset);
    // Calculate the value's address
    llvm::Value* value_addr = CreatePtrDisp(object_addr,
                                            llvm_offset,
                                            new_value->getType()->getPointerTo());
    // Store
    CreateStore(new_value, value_addr);
  }


  //--------------------------------------------------------------------------
  // Runtime Helper Function
  //--------------------------------------------------------------------------

  llvm::Function* GetRuntime(runtime_support::RuntimeId rt) {
    return runtime_support_->GetRuntimeSupportFunction(rt);
  }

  void SetRuntimeSupport(RuntimeSupportBuilder* runtime_support) {
    // Can only set once. We can't do this on constructor, because RuntimeSupportBuilder needs
    // IRBuilder.
    if (runtime_support_ == NULL && runtime_support != NULL) {
      runtime_support_ = runtime_support;
    }
  }


  //--------------------------------------------------------------------------
  // Type Helper Function
  //--------------------------------------------------------------------------

  llvm::Type* getJType(char shorty_jty, JTypeSpace space) {
    return getJType(GetJTypeFromShorty(shorty_jty), space);
  }

  llvm::Type* getJType(JType jty, JTypeSpace space) {
    switch (space) {
    case kAccurate:
      return getJTypeInAccurateSpace(jty);

    case kReg:
    case kField: // Currently field space is equivalent to register space.
      return getJTypeInRegSpace(jty);

    case kArray:
      return getJTypeInArraySpace(jty);
    }

    LOG(FATAL) << "Unknown type space: " << space;
    return NULL;
  }

  llvm::Type* getJVoidTy() {
    return getVoidTy();
  }

  llvm::IntegerType* getJBooleanTy() {
    return getInt1Ty();
  }

  llvm::IntegerType* getJByteTy() {
    return getInt8Ty();
  }

  llvm::IntegerType* getJCharTy() {
    return getInt16Ty();
  }

  llvm::IntegerType* getJShortTy() {
    return getInt16Ty();
  }

  llvm::IntegerType* getJIntTy() {
    return getInt32Ty();
  }

  llvm::IntegerType* getJLongTy() {
    return getInt64Ty();
  }

  llvm::Type* getJFloatTy() {
    return getFloatTy();
  }

  llvm::Type* getJDoubleTy() {
    return getDoubleTy();
  }

  llvm::PointerType* getJObjectTy() {
    return jobject_type_;
  }

  llvm::PointerType* getJEnvTy() {
    return jenv_type_;
  }

  llvm::Type* getJValueTy() {
    // NOTE: JValue is an union type, which may contains boolean, byte, char,
    // short, int, long, float, double, Object.  However, LLVM itself does
    // not support union type, so we have to return a type with biggest size,
    // then bitcast it before we use it.
    return getJLongTy();
  }

  llvm::StructType* getShadowFrameTy(uint32_t sirt_size);


  //--------------------------------------------------------------------------
  // Constant Value Helper Function
  //--------------------------------------------------------------------------

  llvm::ConstantInt* getJBoolean(bool is_true) {
    return (is_true) ? getTrue() : getFalse();
  }

  llvm::ConstantInt* getJByte(int8_t i) {
    return llvm::ConstantInt::getSigned(getJByteTy(), i);
  }

  llvm::ConstantInt* getJChar(int16_t i) {
    return llvm::ConstantInt::getSigned(getJCharTy(), i);
  }

  llvm::ConstantInt* getJShort(int16_t i) {
    return llvm::ConstantInt::getSigned(getJShortTy(), i);
  }

  llvm::ConstantInt* getJInt(int32_t i) {
    return llvm::ConstantInt::getSigned(getJIntTy(), i);
  }

  llvm::ConstantInt* getJLong(int64_t i) {
    return llvm::ConstantInt::getSigned(getJLongTy(), i);
  }

  llvm::Constant* getJFloat(float f) {
    return llvm::ConstantFP::get(getJFloatTy(), f);
  }

  llvm::Constant* getJDouble(double d) {
    return llvm::ConstantFP::get(getJDoubleTy(), d);
  }

  llvm::ConstantPointerNull* getJNull() {
    return llvm::ConstantPointerNull::get(getJObjectTy());
  }

  llvm::Constant* getJZero(char shorty_jty) {
    return getJZero(GetJTypeFromShorty(shorty_jty));
  }

  llvm::Constant* getJZero(JType jty) {
    switch (jty) {
    case kVoid:
      LOG(FATAL) << "Zero is not a value of void type";
      return NULL;

    case kBoolean:
      return getJBoolean(false);

    case kByte:
      return getJByte(0);

    case kChar:
      return getJChar(0);

    case kShort:
      return getJShort(0);

    case kInt:
      return getJInt(0);

    case kLong:
      return getJLong(0);

    case kFloat:
      return getJFloat(0.0f);

    case kDouble:
      return getJDouble(0.0);

    case kObject:
      return getJNull();
    }

    LOG(FATAL) << "Unknown java type: " << jty;
    return NULL;
  }


 private:
  //--------------------------------------------------------------------------
  // Type Helper Function (Private)
  //--------------------------------------------------------------------------

  llvm::Type* getJTypeInAccurateSpace(JType jty);
  llvm::Type* getJTypeInRegSpace(JType jty);
  llvm::Type* getJTypeInArraySpace(JType jty);


 private:
  llvm::Module* module_;

  llvm::PointerType* jobject_type_;

  llvm::PointerType* jenv_type_;

  llvm::StructType* art_frame_type_;

  RuntimeSupportBuilder* runtime_support_;

};


} // namespace compiler_llvm
} // namespace art

#endif // ART_SRC_COMPILER_LLVM_IR_BUILDER_H_
