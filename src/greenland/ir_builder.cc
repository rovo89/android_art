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
namespace greenland {

IRBuilder::IRBuilder(llvm::LLVMContext& context, llvm::Module& module,
                     IntrinsicHelper& intrinsic_helper)
    : LLVMIRBuilder(context), java_object_type_(NULL), java_method_type_(NULL),
      java_thread_type_(NULL), intrinsic_helper_(intrinsic_helper) {
  java_object_type_ = module.getTypeByName("JavaObject")->getPointerTo();
  java_method_type_ = module.getTypeByName("Method")->getPointerTo();
  java_thread_type_ = module.getTypeByName("Thread")->getPointerTo();
}

llvm::Type* IRBuilder::GetJTypeInAccurateSpace(JType jty) {
  switch (jty) {
  case kVoid:
    return GetJVoidTy();

  case kBoolean:
    return GetJBooleanTy();

  case kByte:
    return GetJByteTy();

  case kChar:
    return GetJCharTy();

  case kShort:
    return GetJShortTy();

  case kInt:
    return GetJIntTy();

  case kLong:
    return GetJLongTy();

  case kFloat:
    return GetJFloatTy();

  case kDouble:
    return GetJDoubleTy();

  case kObject:
    return GetJObjectTy();
  }

  LOG(FATAL) << "Unknown java type: " << jty;
  return NULL;
}

llvm::Type* IRBuilder::GetJTypeInRegSpace(JType jty) {
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
    return GetJObjectTy();
  }

  LOG(FATAL) << "Unknown register category: " << regcat;
  return NULL;
}

llvm::Type* IRBuilder::GetJTypeInArraySpace(JType jty) {
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
    return GetJObjectTy();
  }

  LOG(FATAL) << "Unknown java type: " << jty;
  return NULL;
}

} // namespace greenland
} // namespace art
