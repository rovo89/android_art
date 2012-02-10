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
#include "runtime_support_func.h"

#include <llvm/Module.h>

namespace art {
namespace compiler_llvm {


//----------------------------------------------------------------------------
// General
//----------------------------------------------------------------------------

IRBuilder::IRBuilder(llvm::LLVMContext& context, llvm::Module& module)
: LLVMIRBuilder(context) {

  // Get java object type from module
  llvm::Type* jobject_struct_type = module.getTypeByName("JavaObject");
  CHECK_NE(jobject_struct_type, static_cast<llvm::Type*>(NULL));
  jobject_type_ = jobject_struct_type->getPointerTo();

  // Create JEnv* type
  llvm::Type* jenv_struct_type = llvm::StructType::create(context, "JEnv");
  jenv_type_ = jenv_struct_type->getPointerTo();

  // Load the runtime support function declaration from module
  InitRuntimeSupportFuncDecl(module);
}


//----------------------------------------------------------------------------
// Runtime Helper Function
//----------------------------------------------------------------------------

void IRBuilder::InitRuntimeSupportFuncDecl(llvm::Module& module) {
  using namespace runtime_support;

#define GET_RUNTIME_SUPPORT_FUNC_DECL(ID, NAME) \
  do { \
    llvm::Function* fn = module.getFunction(NAME); \
    DCHECK_NE(fn, (void*)NULL) << "Function not found: " << NAME; \
    runtime_support_func_decls_[ID] = fn; \
  } while (0);

#include "runtime_support_func_list.h"
  RUNTIME_SUPPORT_FUNC_LIST(GET_RUNTIME_SUPPORT_FUNC_DECL)
#undef RUNTIME_SUPPORT_FUNC_LIST
#undef GET_RUNTIME_SUPPORT_FUNC_DECL
}


llvm::Function* IRBuilder::GetRuntime(runtime_support::RuntimeId rt) const {
  using namespace runtime_support;

  if (rt >= 0 && rt < MAX_ID){
    return runtime_support_func_decls_[rt];
  } else {
    LOG(ERROR) << "Unknown runtime function id: " << rt;
    return NULL;
  }
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
