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

#ifndef ART_SRC_COMPILER_LLVM_DALVIK_REG_H_
#define ART_SRC_COMPILER_LLVM_DALVIK_REG_H_

#include "backend_types.h"

#include <stdint.h>
#include <string>

namespace llvm {
  class Type;
  class Value;
}

namespace art {
namespace greenland {

class IRBuilder;
class DexLang;

class DalvikReg {
 public:
  static llvm::Type* GetRegCategoryEquivSizeTy(IRBuilder& irb, RegCategory reg_cat);

  static char GetRegCategoryNamePrefix(RegCategory reg_cat);

  DalvikReg(DexLang& dex_lang, unsigned reg_idx);

  ~DalvikReg();

  llvm::Value* GetValue(JType jty, JTypeSpace space);

  llvm::Value* GetValue(char shorty, JTypeSpace space) {
    return GetValue(GetJTypeFromShorty(shorty), space);
  }

  void SetValue(JType jty, JTypeSpace space, llvm::Value* value);

  void SetValue(char shorty, JTypeSpace space, llvm::Value* value) {
    return SetValue(GetJTypeFromShorty(shorty), space, value);
  }

 private:
  void SetShadowEntry(llvm::Value* object);

  llvm::Value* GetAddr(JType jty);

  llvm::Value* RegCat1SExt(llvm::Value* value);
  llvm::Value* RegCat1ZExt(llvm::Value* value);

  llvm::Value* RegCat1Trunc(llvm::Value* value, llvm::Type* ty);

  DexLang& dex_lang_;
  IRBuilder& irb_;

  unsigned reg_idx_;

  int shadow_frame_entry_idx_;

  llvm::Value* reg_32_;
  llvm::Value* reg_64_;
  llvm::Value* reg_obj_;
};

} // namespace greenland
} // namespace art

#endif // ART_SRC_COMPILER_LLVM_DALVIK_REG_H_
