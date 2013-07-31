/*
 * Copyright (C) 2011 The Android Open Source Project
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

#ifndef ART_COMPILER_SEA_IR_TYPES_TYPE_INFERENCE_H_
#define ART_COMPILER_SEA_IR_TYPES_TYPE_INFERENCE_H_

#include "sea_ir/sea.h"
#include "dex_file-inl.h"
#include "verifier/reg_type.h"
#include "verifier/reg_type_cache.h"
#include "verifier/reg_type_cache.h"
namespace sea_ir {

typedef art::verifier::RegType Type;


// The type inference in SEA IR is different from the verifier in that it is concerned
// with a rich type hierarchy (TODO) usable in optimization and does not perform
// precise verification which is the job of the verifier.
class TypeInference {
 public:
  TypeInference() {
    type_cache_ = new art::verifier::RegTypeCache(false);
  }

  // Computes the types for the method with SEA IR representation provided by @graph.
  void ComputeTypes(SeaGraph* graph);

  // Returns true if @descriptor corresponds to a primitive type.
  static bool IsPrimitiveDescriptor(char descriptor);

 protected:
  art::verifier::RegTypeCache* type_cache_;
  std::map<int, const Type*> type_map_;
};

// Stores information about the exact type of  a function.
class FunctionTypeInfo {
 public:
  // @graph provides the input method SEA IR representation.
  // @types provides the input cache of types from which the
  //        parameter types of the function are found.
  FunctionTypeInfo(const SeaGraph* graph, art::verifier::RegTypeCache* types);
  // Returns the ordered vector of types corresponding to the function arguments.
  std::vector<const Type*> GetDeclaredArgumentTypes();
  // Returns the type corresponding to the class that declared the method.
  const Type& GetDeclaringClass() {
    return *declaring_class_;
  }

  bool IsConstructor() const {
    return (method_access_flags_ & kAccConstructor) != 0;
  }

  bool IsStatic() const {
    return (method_access_flags_ & kAccStatic) != 0;
  }

 protected:
  const Type* declaring_class_;
  const art::DexFile* dex_file_;
  const uint32_t dex_method_idx_;
  art::verifier::RegTypeCache* type_cache_;
  const uint32_t method_access_flags_;  // Method's access flags.
};

// The TypeInferenceVisitor visits each instruction and computes its type taking into account
//   the current type of the operands. The type is stored in the visitor.
// We may be better off by using a separate visitor type hierarchy that has return values
//   or that passes data as parameters, than to use fields to store information that should
//   in fact be returned after visiting each element. Ideally, I would prefer to use templates
//   to specify the returned value type, but I am not aware of a possible implementation
//   that does not horribly duplicate the visitor infrastructure code (version 1: no return value,
//   version 2: with template return value).
class TypeInferenceVisitor: public IRVisitor {
 public:
  TypeInferenceVisitor(SeaGraph* graph, art::verifier::RegTypeCache* types):
    graph_(graph), type_cache_(types), crt_type_() { }
  void Initialize(SeaGraph* graph) { }
  // There are no type related actions to be performed on these classes.
  void Visit(SeaGraph* graph) { }
  void Visit(Region* region) { }

  void Visit(PhiInstructionNode* instruction) {
    std::cout << "[TI] Visiting node:" << instruction->Id() << std::endl;
  }
  void Visit(SignatureNode* parameter) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    std::cout << "[TI] Visiting signature node:" << parameter->GetResultRegister() << std::endl;
    FunctionTypeInfo fti(graph_, type_cache_);
    std::vector<const Type*> arguments = fti.GetDeclaredArgumentTypes();
    crt_type_.clear();
    std::cout << "Pos:" << parameter->GetPositionInSignature() << "/" << arguments.size() <<std::endl;
    DCHECK_LT(parameter->GetPositionInSignature(), arguments.size())
      << "Signature node position not present in signature.";
    crt_type_.push_back(arguments.at(parameter->GetPositionInSignature()));
  }
  void Visit(InstructionNode* instruction) {
    std::cout << "[TI] Visiting node:" << instruction->Id() << std::endl;
  }
  void Visit(ConstInstructionNode* instruction) {
    std::cout << "[TI] Visiting node:" << instruction->Id() << std::endl;
  }
  void Visit(ReturnInstructionNode* instruction) {
    std::cout << "[TI] Visiting node:" << instruction->Id() << std::endl;
  }
  void Visit(IfNeInstructionNode* instruction) {
    std::cout << "[TI] Visiting node:" << instruction->Id() << std::endl;
  }
  void Visit(MoveResultInstructionNode* instruction) {
    std::cout << "[TI] Visiting node:" << instruction->Id() << std::endl;
  }
  void Visit(InvokeStaticInstructionNode* instruction) {
    std::cout << "[TI] Visiting node:" << instruction->Id() << std::endl;
  }
  void Visit(AddIntInstructionNode* instruction) {
    std::cout << "[TI] Visiting node:" << instruction->Id() << std::endl;
  }
  void Visit(GotoInstructionNode* instruction) {
    std::cout << "[TI] Visiting node:" << instruction->Id() << std::endl;
  }
  void Visit(IfEqzInstructionNode* instruction) {
    std::cout << "[TI] Visiting node:" << instruction->Id() << std::endl;
  }

  const Type* GetType() const {
    // TODO: Currently multiple defined types are not supported.
    if (crt_type_.size()>0) return crt_type_.at(0);
    return NULL;
  }

 protected:
  const SeaGraph* graph_;
  art::verifier::RegTypeCache* type_cache_;
  std::vector<const Type*> crt_type_;             // Stored temporarily between two calls to Visit.
};

}  // namespace sea_ir

#endif  // ART_COMPILER_SEA_IR_TYPES_TYPE_INFERENCE_H_
