/*
 * Copyright (C) 2015 The Android Open Source Project
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

#ifndef ART_COMPILER_OPTIMIZING_INTRINSICS_H_
#define ART_COMPILER_OPTIMIZING_INTRINSICS_H_

#include "code_generator.h"
#include "nodes.h"
#include "optimization.h"
#include "parallel_move_resolver.h"

namespace art {

class CompilerDriver;
class DexFile;

// Recognize intrinsics from HInvoke nodes.
class IntrinsicsRecognizer : public HOptimization {
 public:
  IntrinsicsRecognizer(HGraph* graph, const DexFile* dex_file, CompilerDriver* driver)
      : HOptimization(graph, true, kIntrinsicsRecognizerPassName),
        dex_file_(dex_file), driver_(driver) {}

  void Run() OVERRIDE;

  static constexpr const char* kIntrinsicsRecognizerPassName = "intrinsics_recognition";

 private:
  const DexFile* dex_file_;
  CompilerDriver* driver_;

  DISALLOW_COPY_AND_ASSIGN(IntrinsicsRecognizer);
};

class IntrinsicVisitor : public ValueObject {
 public:
  virtual ~IntrinsicVisitor() {}

  // Dispatch logic.

  void Dispatch(HInvoke* invoke) {
    switch (invoke->GetIntrinsic()) {
      case Intrinsics::kNone:
        return;
#define OPTIMIZING_INTRINSICS(Name, IsStatic) \
      case Intrinsics::k ## Name:             \
        Visit ## Name(invoke);                \
        return;
#include "intrinsics_list.h"
INTRINSICS_LIST(OPTIMIZING_INTRINSICS)
#undef INTRINSICS_LIST
#undef OPTIMIZING_INTRINSICS

      // Do not put a default case. That way the compiler will complain if we missed a case.
    }
  }

  // Define visitor methods.

#define OPTIMIZING_INTRINSICS(Name, IsStatic)                    \
  virtual void Visit ## Name(HInvoke* invoke ATTRIBUTE_UNUSED) { \
  }
#include "intrinsics_list.h"
INTRINSICS_LIST(OPTIMIZING_INTRINSICS)
#undef INTRINSICS_LIST
#undef OPTIMIZING_INTRINSICS

  static void MoveArguments(HInvoke* invoke,
                            CodeGenerator* codegen,
                            InvokeDexCallingConventionVisitor* calling_convention_visitor) {
    if (kIsDebugBuild && invoke->IsInvokeStaticOrDirect()) {
      HInvokeStaticOrDirect* invoke_static_or_direct = invoke->AsInvokeStaticOrDirect();
      // When we do not run baseline, explicit clinit checks triggered by static
      // invokes must have been pruned by art::PrepareForRegisterAllocation.
      DCHECK(codegen->IsBaseline() || !invoke_static_or_direct->IsStaticWithExplicitClinitCheck());
    }

    if (invoke->GetNumberOfArguments() == 0) {
      // No argument to move.
      return;
    }

    LocationSummary* locations = invoke->GetLocations();

    // We're moving potentially two or more locations to locations that could overlap, so we need
    // a parallel move resolver.
    HParallelMove parallel_move(codegen->GetGraph()->GetArena());

    for (size_t i = 0; i < invoke->GetNumberOfArguments(); i++) {
      HInstruction* input = invoke->InputAt(i);
      Location cc_loc = calling_convention_visitor->GetNextLocation(input->GetType());
      Location actual_loc = locations->InAt(i);

      parallel_move.AddMove(actual_loc, cc_loc, input->GetType(), nullptr);
    }

    codegen->GetMoveResolver()->EmitNativeCode(&parallel_move);
  }

 protected:
  IntrinsicVisitor() {}

 private:
  DISALLOW_COPY_AND_ASSIGN(IntrinsicVisitor);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_INTRINSICS_H_
