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


#include "tbaa_info.h"

#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Constants.h>
#include <llvm/Metadata.h>
#include <llvm/Type.h>


namespace art {
namespace compiler_llvm {


llvm::MDNode* TBAAInfo::GetRootType() {
  if (root_ == NULL) {
    root_ = GenTBAANode("Art TBAA Root");
  }
  return root_;
}

llvm::MDNode* TBAAInfo::GetSpecialType(TBAASpecialType sty_id) {
  DCHECK_GE(sty_id, 0) << "Unknown TBAA special type: " << sty_id;
  DCHECK_LT(sty_id, MAX_TBAA_SPECIAL_TYPE) << "Unknown TBAA special type: " << sty_id;

  llvm::MDNode*& spec_ty = special_type_[sty_id];
  if (spec_ty == NULL) {
    switch (sty_id) {
    case kTBAARegister:         spec_ty = GenTBAANode("Register", GetRootType()); break;
    case kTBAAStackTemp:        spec_ty = GenTBAANode("StackTemp", GetRootType()); break;
    case kTBAAMemory:           spec_ty = GenTBAANode("Memory", GetRootType()); break;
    case kTBAAMemoryArray:      spec_ty = GenTBAANode("MemoryArray", GetRootType()); break;
    case kTBAAMemoryIdentified: spec_ty = GenTBAANode("MemoryIdentified", GetRootType()); break;
    case kTBAAMemoryStatic:     spec_ty = GenTBAANode("MemoryStatic", GetRootType()); break;
    case kTBAAJRuntime:         spec_ty = GenTBAANode("JRuntime", GetRootType()); break;
    case kTBAARuntimeInfo:      spec_ty = GenTBAANode("RuntimeInfo", GetRootType()); break;
    case kTBAAConstJObject:     spec_ty = GenTBAANode("ConstJObject", GetRootType(), true); break;
    default:
      LOG(FATAL) << "Unknown TBAA special type: " << sty_id;
      break;
    }
  }
  return spec_ty;
}

llvm::MDNode* TBAAInfo::GenTBAANode(llvm::StringRef name, llvm::MDNode* parent, bool read_only) {
  llvm::SmallVector<llvm::Value*, 3> array_ref;

  array_ref.push_back(llvm::MDString::get(context_, name));
  if (parent != NULL) {
    array_ref.push_back(parent);
  }
  if (read_only != false) {
    array_ref.push_back(llvm::ConstantInt::get(llvm::Type::getInt1Ty(context_), read_only));
  }

  return llvm::MDNode::get(context_, array_ref);
}


} // namespace compiler_llvm
} // namespace art
