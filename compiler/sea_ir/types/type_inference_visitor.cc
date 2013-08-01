/*
 * Copyright (C) 2013 The Android Open Source Project
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

#include "sea_ir/types/type_inference_visitor.h"
#include "sea_ir/types/type_inference.h"
#include "sea_ir/sea.h"

namespace sea_ir {

void TypeInferenceVisitor::Visit(SignatureNode* parameter) {
  std::cout << "[TI] Visiting signature node:" << parameter->GetResultRegister() << std::endl;
  FunctionTypeInfo fti(graph_, type_cache_);
  std::vector<const Type*> arguments = fti.GetDeclaredArgumentTypes();
  crt_type_.clear();
  DCHECK_LT(parameter->GetPositionInSignature(), arguments.size())
    << "Signature node position not present in signature.";
  crt_type_.push_back(arguments.at(parameter->GetPositionInSignature()));
}

}   // namespace sea_ir
