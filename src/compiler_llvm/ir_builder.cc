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

#include "ir_builder.h"

#include <llvm/Module.h>

namespace art {
namespace compiler_llvm {


//----------------------------------------------------------------------------
// General
//----------------------------------------------------------------------------

IRBuilder::IRBuilder(llvm::LLVMContext& context, llvm::Module& module)
: LLVMIRBuilder(context) {

  // Get java object type from module
  llvm::Type* jobject_struct_type =
    llvm::StructType::create(context, "JavaObject");
  jobject_type_ = jobject_struct_type->getPointerTo();
}


//----------------------------------------------------------------------------
// Type Helper Function
//----------------------------------------------------------------------------

llvm::Type* IRBuilder::getJTypeInAccurateSpace(JType jty) {
  switch (jty) {
  case kVoid:
    return getJVoidTy();

  case kBoolean:
    return getJBooleanTy();

  case kByte:
    return getJByteTy();

  case kChar:
    return getJCharTy();

  case kShort:
    return getJShortTy();

  case kInt:
    return getJIntTy();

  case kLong:
    return getJLongTy();

  case kFloat:
    return getJFloatTy();

  case kDouble:
    return getJDoubleTy();

  case kObject:
    return getJObjectTy();
  }

  LOG(FATAL) << "Unknown java type: " << jty;
  return NULL;
}


llvm::Type* IRBuilder::getJTypeInRegSpace(JType jty) {
  RegCategory regcat = GetRegCategoryFromJType(jty);

  switch (regcat) {
  case kRegUnknown:
  case kRegZero:
    LOG(FATAL) << "Register category \"Unknown\" or \"Zero\" does not have "
               << "the LLVM type";
    return NULL;

  case kRegCat1nr:
    return getInt32Ty();

  case kRegCat2:
    return getInt64Ty();

  case kRegObject:
    return getJObjectTy();
  }

  LOG(FATAL) << "Unknown register category: " << regcat;
  return NULL;
}


llvm::Type* IRBuilder::getJTypeInArraySpace(JType jty) {
  switch (jty) {
  case kVoid:
    LOG(FATAL) << "void type should not be used in array type space";
    return NULL;

  case kBoolean:
  case kByte:
    return getInt8Ty();

  case kChar:
  case kShort:
    return getInt16Ty();

  case kInt:
    return getInt32Ty();

  case kLong:
    return getInt64Ty();

  case kFloat:
    return getFloatTy();

  case kDouble:
    return getDoubleTy();

  case kObject:
    return getJObjectTy();
  }

  LOG(FATAL) << "Unknown java type: " << jty;
  return NULL;
}


} // namespace compiler_llvm
} // namespace art
