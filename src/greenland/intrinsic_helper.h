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

#ifndef ART_SRC_GREENLAND_INTRINSIC_HELPER_H_
#define ART_SRC_GREENLAND_INTRINSIC_HELPER_H_

#include "logging.h"

#include <llvm/ADT/DenseMap.h>

namespace llvm {
  class Function;
  class FunctionType;
  class LLVMContext;
  class Module;
}

namespace art {
namespace greenland {

class IRBuilder;

class IntrinsicHelper {
 public:
  enum IntrinsicId {
#define DEF_INTRINSICS_FUNC(ID, ...) ID,
#include "intrinsic_func_list.def"
    MaxIntrinsicId,

    // Pseudo-intrinsics Id
    UnknownId
  };

  enum IntrinsicAttribute {
    kAttrNone     = 0,

    // Intrinsic that doesn't modify the memory state
    kAttrReadOnly = 1 << 0,

    // Intrinsic that never generates exception
    kAttrNoThrow  = 1 << 1,
  };

  enum IntrinsicValType {
    kNone,

    kVoidTy,

    kJavaObjectTy,
    kJavaMethodTy,
    kJavaThreadTy,

    kInt1Ty,
    kInt8Ty,
    kInt16Ty,
    kInt32Ty,
    kInt64Ty,

    kInt1ConstantTy,
    kInt8ConstantTy,
    kInt16ConstantTy,
    kInt32ConstantTy,
    kInt64ConstantTy,

    kFloatTy,
    kDoubleTy,

    kVarArgTy,
  };

  enum {
    kIntrinsicMaxArgc = 5
  };

  typedef struct IntrinsicInfo {
    const char* name_;
    unsigned attr_;
    IntrinsicValType ret_val_type_;
    IntrinsicValType arg_type_[kIntrinsicMaxArgc];
  } IntrinsicInfo;

 private:
  static const IntrinsicInfo Info[MaxIntrinsicId];

 public:
  static const IntrinsicInfo& GetInfo(IntrinsicId id) {
    DCHECK(id >= 0 && id < MaxIntrinsicId) << "Unknown Dalvik intrinsics ID: "
                                           << id;
    return Info[id];
  }

  static const char* GetName(IntrinsicId id) {
    return (id <= MaxIntrinsicId) ? GetInfo(id).name_ : "InvalidIntrinsic";
  }

  static unsigned GetAttr(IntrinsicId id) {
    return GetInfo(id).attr_;
  }

 public:
  IntrinsicHelper(llvm::LLVMContext& context, llvm::Module& module);

  inline llvm::Function* GetIntrinsicFunction(IntrinsicId id) {
    DCHECK(id >= 0 && id < MaxIntrinsicId) << "Unknown Dalvik intrinsics ID: "
                                           << id;
    return intrinsic_funcs_[id];
  }

  inline IntrinsicId GetIntrinsicId(const llvm::Function* func) const {
    llvm::DenseMap<const llvm::Function*, IntrinsicId>::const_iterator
        i = intrinsic_funcs_map_.find(func);
    if (i == intrinsic_funcs_map_.end()) {
      return UnknownId;
    } else {
      return i->second;
    }
  }

 private:
  llvm::Function* intrinsic_funcs_[MaxIntrinsicId];

  // Map a llvm::Function to its intrinsic id
  llvm::DenseMap<const llvm::Function*, IntrinsicId> intrinsic_funcs_map_;
};

} // namespace greenland
} // namespace art

#endif // ART_SRC_GREENLAND_INTRINSIC_HELPER_H_
