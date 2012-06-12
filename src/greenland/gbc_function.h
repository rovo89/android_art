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

#ifndef ART_SRC_GREENLAND_GBC_FUNCTION_H_
#define ART_SRC_GREENLAND_GBC_FUNCTION_H_

#include "gbc_context.h"

namespace llvm {
  class Function;
} // namespace llvm

namespace art {
  class OatCompilationUnit;
} // namespace art

namespace art {
namespace greenland {

class GBCContext;

class GBCFunction {
 private:
  // The GBCContext associated with this GBCFunction
  GBCContext& context_;

  // The LLVM Function in Greenland bitcode
  llvm::Function& func_;

  // The associated OatCompilationUnit
  const OatCompilationUnit& cunit_;

 public:
  GBCFunction(GBCContext& context, llvm::Function& func,
              const OatCompilationUnit& cunit)
    : context_(context.IncRef()), func_(func), cunit_(cunit) { }

  ~GBCFunction() {
    context_.DecRef();
    return;
  }

  IntrinsicHelper& GetIntrinsicHelper() {
    return context_.GetIntrinsicHelper();
  }
  const IntrinsicHelper& GetIntrinsicHelper() const {
    return context_.GetIntrinsicHelper();
  }

  llvm::Function& GetBitcode() {
    return func_;
  }
  const llvm::Function& GetBitcode() const {
    return func_;
  }

  const OatCompilationUnit& GetOatCompilationUnit() const {
    return cunit_;
  }
};

} // namespace greenland
} // namespace art

#endif // ART_SRC_GREENLAND_GBC_FUNCTION_H_
