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

#ifndef ART_SRC_GREENLAND_DALVIK_REG_H_
#define ART_SRC_GREENLAND_DALVIK_REG_H_

#include "backend_types.h"

namespace llvm {
  class Type;
  class Value;
}

namespace art {
namespace greenland {

class DexLang;
class IRBuilder;

class DalvikReg {
 public:
  static DalvikReg* CreateArgReg(DexLang& dex_lang, unsigned reg_idx,
                                 JType jty);

  static DalvikReg* CreateLocalVarReg(DexLang& dex_lang, unsigned reg_idx);

  virtual ~DalvikReg() { }

  virtual llvm::Value* GetValue(JType jty, JTypeSpace space) = 0;
  llvm::Value* GetValue(char shorty, JTypeSpace space) {
    return GetValue(GetJTypeFromShorty(shorty), space);
  }

  virtual void SetValue(JType jty, JTypeSpace space, llvm::Value* value) = 0;
  void SetValue(char shorty, JTypeSpace space, llvm::Value* value) {
    return SetValue(GetJTypeFromShorty(shorty), space, value);
  }

 protected:
  DalvikReg(DexLang& dex_lang, unsigned reg_idx);

  void SetShadowEntry(llvm::Value* root_object);

 protected:
  DexLang& dex_lang_;
  IRBuilder& irb_;
  unsigned reg_idx_;
  int shadow_frame_entry_idx_;
};

} // namespace greenland
} // namespace art

#endif // ART_SRC_GREENLAND_DALVIK_REG_H_
