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

// Temporary measure until we have caught up with the Java 7 definition of Math.round. b/26327751
static constexpr bool kRoundIsPlusPointFive = false;

// Recognize intrinsics from HInvoke nodes.
class IntrinsicsRecognizer : public HOptimization {
 public:
  IntrinsicsRecognizer(HGraph* graph, CompilerDriver* driver, OptimizingCompilerStats* stats)
      : HOptimization(graph, kIntrinsicsRecognizerPassName, stats),
        driver_(driver) {}

  void Run() OVERRIDE;

  static constexpr const char* kIntrinsicsRecognizerPassName = "intrinsics_recognition";

 private:
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
#define OPTIMIZING_INTRINSICS(Name, IsStatic, NeedsEnvironment, SideEffects, Exceptions) \
      case Intrinsics::k ## Name: \
        Visit ## Name(invoke);    \
        return;
#include "intrinsics_list.h"
INTRINSICS_LIST(OPTIMIZING_INTRINSICS)
#undef INTRINSICS_LIST
#undef OPTIMIZING_INTRINSICS

      // Do not put a default case. That way the compiler will complain if we missed a case.
    }
  }

  // Define visitor methods.

#define OPTIMIZING_INTRINSICS(Name, IsStatic, NeedsEnvironment, SideEffects, Exceptions) \
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
      // Explicit clinit checks triggered by static invokes must have been
      // pruned by art::PrepareForRegisterAllocation.
      DCHECK(!invoke_static_or_direct->IsStaticWithExplicitClinitCheck());
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

#define GENERIC_OPTIMIZATION(name, bit)                \
public:                                                \
void Set##name() { SetBit(k##name); }                  \
bool Get##name() const { return IsBitSet(k##name); }   \
private:                                               \
static constexpr size_t k##name = bit

class IntrinsicOptimizations : public ValueObject {
 public:
  explicit IntrinsicOptimizations(HInvoke* invoke)
      : value_(invoke->GetIntrinsicOptimizations()) {}
  explicit IntrinsicOptimizations(const HInvoke& invoke)
      : value_(invoke.GetIntrinsicOptimizations()) {}

  static constexpr int kNumberOfGenericOptimizations = 2;
  GENERIC_OPTIMIZATION(DoesNotNeedDexCache, 0);
  GENERIC_OPTIMIZATION(DoesNotNeedEnvironment, 1);

 protected:
  bool IsBitSet(uint32_t bit) const {
    DCHECK_LT(bit, sizeof(uint32_t) * kBitsPerByte);
    return (*value_ & (1 << bit)) != 0u;
  }

  void SetBit(uint32_t bit) {
    DCHECK_LT(bit, sizeof(uint32_t) * kBitsPerByte);
    *(const_cast<uint32_t* const>(value_)) |= (1 << bit);
  }

 private:
  const uint32_t* const value_;

  DISALLOW_COPY_AND_ASSIGN(IntrinsicOptimizations);
};

#undef GENERIC_OPTIMIZATION

#define INTRINSIC_OPTIMIZATION(name, bit)                             \
public:                                                               \
void Set##name() { SetBit(k##name); }                                 \
bool Get##name() const { return IsBitSet(k##name); }                  \
private:                                                              \
static constexpr size_t k##name = bit + kNumberOfGenericOptimizations

class StringEqualsOptimizations : public IntrinsicOptimizations {
 public:
  explicit StringEqualsOptimizations(HInvoke* invoke) : IntrinsicOptimizations(invoke) {}

  INTRINSIC_OPTIMIZATION(ArgumentNotNull, 0);
  INTRINSIC_OPTIMIZATION(ArgumentIsString, 1);

 private:
  DISALLOW_COPY_AND_ASSIGN(StringEqualsOptimizations);
};

class SystemArrayCopyOptimizations : public IntrinsicOptimizations {
 public:
  explicit SystemArrayCopyOptimizations(HInvoke* invoke) : IntrinsicOptimizations(invoke) {}

  INTRINSIC_OPTIMIZATION(SourceIsNotNull, 0);
  INTRINSIC_OPTIMIZATION(DestinationIsNotNull, 1);
  INTRINSIC_OPTIMIZATION(DestinationIsSource, 2);
  INTRINSIC_OPTIMIZATION(CountIsSourceLength, 3);
  INTRINSIC_OPTIMIZATION(CountIsDestinationLength, 4);
  INTRINSIC_OPTIMIZATION(DoesNotNeedTypeCheck, 5);
  INTRINSIC_OPTIMIZATION(DestinationIsTypedObjectArray, 6);
  INTRINSIC_OPTIMIZATION(DestinationIsNonPrimitiveArray, 7);
  INTRINSIC_OPTIMIZATION(DestinationIsPrimitiveArray, 8);
  INTRINSIC_OPTIMIZATION(SourceIsNonPrimitiveArray, 9);
  INTRINSIC_OPTIMIZATION(SourceIsPrimitiveArray, 10);

 private:
  DISALLOW_COPY_AND_ASSIGN(SystemArrayCopyOptimizations);
};

#undef INTRISIC_OPTIMIZATION

//
// Macros for use in the intrinsics code generators.
//

// Defines an unimplemented intrinsic: that is, a method call that is recognized as an
// intrinsic to exploit e.g. no side-effects or exceptions, but otherwise not handled
// by this architecture-specific intrinsics code generator. Eventually it is implemented
// as a true method call.
#define UNIMPLEMENTED_INTRINSIC(Arch, Name)                                               \
void IntrinsicLocationsBuilder ## Arch::Visit ## Name(HInvoke* invoke ATTRIBUTE_UNUSED) { \
}                                                                                         \
void IntrinsicCodeGenerator ## Arch::Visit ## Name(HInvoke* invoke ATTRIBUTE_UNUSED) {    \
}

// Defines a list of unreached intrinsics: that is, method calls that are recognized as
// an intrinsic, and then always converted into HIR instructions before they reach any
// architecture-specific intrinsics code generator.
#define UNREACHABLE_INTRINSIC(Arch, Name)                                \
void IntrinsicLocationsBuilder ## Arch::Visit ## Name(HInvoke* invoke) { \
  LOG(FATAL) << "Unreachable: intrinsic " << invoke->GetIntrinsic()      \
             << " should have been converted to HIR";                    \
}                                                                        \
void IntrinsicCodeGenerator ## Arch::Visit ## Name(HInvoke* invoke) {    \
  LOG(FATAL) << "Unreachable: intrinsic " << invoke->GetIntrinsic()      \
             << " should have been converted to HIR";                    \
}
#define UNREACHABLE_INTRINSICS(Arch)                \
UNREACHABLE_INTRINSIC(Arch, FloatFloatToIntBits)    \
UNREACHABLE_INTRINSIC(Arch, DoubleDoubleToLongBits) \
UNREACHABLE_INTRINSIC(Arch, FloatIsNaN)             \
UNREACHABLE_INTRINSIC(Arch, DoubleIsNaN)            \
UNREACHABLE_INTRINSIC(Arch, IntegerRotateLeft)      \
UNREACHABLE_INTRINSIC(Arch, LongRotateLeft)         \
UNREACHABLE_INTRINSIC(Arch, IntegerRotateRight)     \
UNREACHABLE_INTRINSIC(Arch, LongRotateRight)        \
UNREACHABLE_INTRINSIC(Arch, IntegerCompare)         \
UNREACHABLE_INTRINSIC(Arch, LongCompare)            \
UNREACHABLE_INTRINSIC(Arch, IntegerSignum)          \
UNREACHABLE_INTRINSIC(Arch, LongSignum)             \
UNREACHABLE_INTRINSIC(Arch, UnsafeLoadFence)        \
UNREACHABLE_INTRINSIC(Arch, UnsafeStoreFence)       \
UNREACHABLE_INTRINSIC(Arch, UnsafeFullFence)

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_INTRINSICS_H_
