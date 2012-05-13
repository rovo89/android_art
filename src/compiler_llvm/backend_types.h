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

#ifndef ART_SRC_COMPILER_LLVM_BACKEND_TYPES_H_
#define ART_SRC_COMPILER_LLVM_BACKEND_TYPES_H_

#include "logging.h"


namespace art {
namespace compiler_llvm {


enum JType {
  kVoid,
  kBoolean,
  kByte,
  kChar,
  kShort,
  kInt,
  kLong,
  kFloat,
  kDouble,
  kObject,
  MAX_JTYPE
};


enum JTypeSpace {
  kAccurate,
  kReg,
  kField,
  kArray,
};


enum RegCategory {
  kRegUnknown,
  kRegZero,
  kRegCat1nr,
  kRegCat2,
  kRegObject,
};


enum TBAASpecialType {
  kTBAARegister,
  kTBAAStackTemp,
  kTBAAHeapArray,
  kTBAAHeapInstance,
  kTBAAHeapStatic,
  kTBAAJRuntime,
  kTBAARuntimeInfo,
  kTBAAShadowFrame,
  kTBAAConstJObject,
  MAX_TBAA_SPECIAL_TYPE
};


inline JType GetJTypeFromShorty(char shorty_jty) {
  switch (shorty_jty) {
  case 'V':
    return kVoid;

  case 'Z':
    return kBoolean;

  case 'B':
    return kByte;

  case 'C':
    return kChar;

  case 'S':
    return kShort;

  case 'I':
    return kInt;

  case 'J':
    return kLong;

  case 'F':
    return kFloat;

  case 'D':
    return kDouble;

  case 'L':
    return kObject;

  default:
    LOG(FATAL) << "Unknown Dalvik shorty descriptor: " << shorty_jty;
    return kVoid;
  }
}


inline RegCategory GetRegCategoryFromJType(JType jty) {
  switch (jty) {
  case kVoid:
    return kRegUnknown;

  case kBoolean:
  case kByte:
  case kChar:
  case kShort:
  case kInt:
  case kFloat:
    return kRegCat1nr;

  case kLong:
  case kDouble:
    return kRegCat2;

  case kObject:
    return kRegObject;

  default:
    LOG(FATAL) << "Unknown java type: " << jty;
    return kRegUnknown;
  }
}


inline RegCategory GetRegCategoryFromShorty(char shorty) {
  return GetRegCategoryFromJType(GetJTypeFromShorty(shorty));
}


} // namespace compiler_llvm
} // namespace art


#endif // ART_SRC_COMPILER_LLVM_BACKEND_TYPES_H_
