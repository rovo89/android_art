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

#include "intrinsics.h"

#include "dex/quick/dex_file_method_inliner.h"
#include "dex/quick/dex_file_to_method_inliner_map.h"
#include "driver/compiler_driver.h"
#include "invoke_type.h"
#include "nodes.h"
#include "quick/inline_method_analyser.h"
#include "utils.h"

namespace art {

// Function that returns whether an intrinsic is static/direct or virtual.
static inline InvokeType GetIntrinsicInvokeType(Intrinsics i) {
  switch (i) {
    case Intrinsics::kNone:
      return kInterface;  // Non-sensical for intrinsic.
#define OPTIMIZING_INTRINSICS(Name, IsStatic) \
    case Intrinsics::k ## Name:               \
      return IsStatic;
#include "intrinsics_list.h"
INTRINSICS_LIST(OPTIMIZING_INTRINSICS)
#undef INTRINSICS_LIST
#undef OPTIMIZING_INTRINSICS
  }
  return kInterface;
}



static Primitive::Type GetType(uint64_t data, bool is_op_size) {
  if (is_op_size) {
    switch (static_cast<OpSize>(data)) {
      case kSignedByte:
        return Primitive::kPrimByte;
      case kSignedHalf:
        return Primitive::kPrimShort;
      case k32:
        return Primitive::kPrimInt;
      case k64:
        return Primitive::kPrimLong;
      default:
        LOG(FATAL) << "Unknown/unsupported op size " << data;
        UNREACHABLE();
    }
  } else {
    if ((data & kIntrinsicFlagIsLong) != 0) {
      return Primitive::kPrimLong;
    }
    if ((data & kIntrinsicFlagIsObject) != 0) {
      return Primitive::kPrimNot;
    }
    return Primitive::kPrimInt;
  }
}

static Intrinsics GetIntrinsic(InlineMethod method) {
  switch (method.opcode) {
    // Floating-point conversions.
    case kIntrinsicDoubleCvt:
      return ((method.d.data & kIntrinsicFlagToFloatingPoint) == 0) ?
          Intrinsics::kDoubleDoubleToRawLongBits : Intrinsics::kDoubleLongBitsToDouble;
    case kIntrinsicFloatCvt:
      return ((method.d.data & kIntrinsicFlagToFloatingPoint) == 0) ?
          Intrinsics::kFloatFloatToRawIntBits : Intrinsics::kFloatIntBitsToFloat;

    // Bit manipulations.
    case kIntrinsicReverseBits:
      switch (GetType(method.d.data, true)) {
        case Primitive::kPrimInt:
          return Intrinsics::kIntegerReverse;
        case Primitive::kPrimLong:
          return Intrinsics::kLongReverse;
        default:
          LOG(FATAL) << "Unknown/unsupported op size " << method.d.data;
          UNREACHABLE();
      }
    case kIntrinsicReverseBytes:
      switch (GetType(method.d.data, true)) {
        case Primitive::kPrimShort:
          return Intrinsics::kShortReverseBytes;
        case Primitive::kPrimInt:
          return Intrinsics::kIntegerReverseBytes;
        case Primitive::kPrimLong:
          return Intrinsics::kLongReverseBytes;
        default:
          LOG(FATAL) << "Unknown/unsupported op size " << method.d.data;
          UNREACHABLE();
      }

    // Abs.
    case kIntrinsicAbsDouble:
      return Intrinsics::kMathAbsDouble;
    case kIntrinsicAbsFloat:
      return Intrinsics::kMathAbsFloat;
    case kIntrinsicAbsInt:
      return Intrinsics::kMathAbsInt;
    case kIntrinsicAbsLong:
      return Intrinsics::kMathAbsLong;

    // Min/max.
    case kIntrinsicMinMaxDouble:
      return ((method.d.data & kIntrinsicFlagMin) == 0) ?
          Intrinsics::kMathMaxDoubleDouble : Intrinsics::kMathMinDoubleDouble;
    case kIntrinsicMinMaxFloat:
      return ((method.d.data & kIntrinsicFlagMin) == 0) ?
          Intrinsics::kMathMaxFloatFloat : Intrinsics::kMathMinFloatFloat;
    case kIntrinsicMinMaxInt:
      return ((method.d.data & kIntrinsicFlagMin) == 0) ?
          Intrinsics::kMathMaxIntInt : Intrinsics::kMathMinIntInt;
    case kIntrinsicMinMaxLong:
      return ((method.d.data & kIntrinsicFlagMin) == 0) ?
          Intrinsics::kMathMaxLongLong : Intrinsics::kMathMinLongLong;

    // Misc math.
    case kIntrinsicSqrt:
      return Intrinsics::kMathSqrt;
    case kIntrinsicCeil:
      return Intrinsics::kMathCeil;
    case kIntrinsicFloor:
      return Intrinsics::kMathFloor;
    case kIntrinsicRint:
      return Intrinsics::kMathRint;
    case kIntrinsicRoundDouble:
      return Intrinsics::kMathRoundDouble;
    case kIntrinsicRoundFloat:
      return Intrinsics::kMathRoundFloat;

    // System.arraycopy.
    case kIntrinsicSystemArrayCopyCharArray:
      return Intrinsics::kSystemArrayCopyChar;

    // Thread.currentThread.
    case kIntrinsicCurrentThread:
      return  Intrinsics::kThreadCurrentThread;

    // Memory.peek.
    case kIntrinsicPeek:
      switch (GetType(method.d.data, true)) {
        case Primitive::kPrimByte:
          return Intrinsics::kMemoryPeekByte;
        case Primitive::kPrimShort:
          return Intrinsics::kMemoryPeekShortNative;
        case Primitive::kPrimInt:
          return Intrinsics::kMemoryPeekIntNative;
        case Primitive::kPrimLong:
          return Intrinsics::kMemoryPeekLongNative;
        default:
          LOG(FATAL) << "Unknown/unsupported op size " << method.d.data;
          UNREACHABLE();
      }

    // Memory.poke.
    case kIntrinsicPoke:
      switch (GetType(method.d.data, true)) {
        case Primitive::kPrimByte:
          return Intrinsics::kMemoryPokeByte;
        case Primitive::kPrimShort:
          return Intrinsics::kMemoryPokeShortNative;
        case Primitive::kPrimInt:
          return Intrinsics::kMemoryPokeIntNative;
        case Primitive::kPrimLong:
          return Intrinsics::kMemoryPokeLongNative;
        default:
          LOG(FATAL) << "Unknown/unsupported op size " << method.d.data;
          UNREACHABLE();
      }

    // String.
    case kIntrinsicCharAt:
      return Intrinsics::kStringCharAt;
    case kIntrinsicCompareTo:
      return Intrinsics::kStringCompareTo;
    case kIntrinsicGetCharsNoCheck:
      return Intrinsics::kStringGetCharsNoCheck;
    case kIntrinsicIsEmptyOrLength:
      // The inliner can handle these two cases - and this is the preferred approach
      // since after inlining the call is no longer visible (as opposed to waiting
      // until codegen to handle intrinsic).
      return Intrinsics::kNone;
    case kIntrinsicIndexOf:
      return ((method.d.data & kIntrinsicFlagBase0) == 0) ?
          Intrinsics::kStringIndexOfAfter : Intrinsics::kStringIndexOf;
    case kIntrinsicNewStringFromBytes:
      return Intrinsics::kStringNewStringFromBytes;
    case kIntrinsicNewStringFromChars:
      return Intrinsics::kStringNewStringFromChars;
    case kIntrinsicNewStringFromString:
      return Intrinsics::kStringNewStringFromString;

    case kIntrinsicCas:
      switch (GetType(method.d.data, false)) {
        case Primitive::kPrimNot:
          return Intrinsics::kUnsafeCASObject;
        case Primitive::kPrimInt:
          return Intrinsics::kUnsafeCASInt;
        case Primitive::kPrimLong:
          return Intrinsics::kUnsafeCASLong;
        default:
          LOG(FATAL) << "Unknown/unsupported op size " << method.d.data;
          UNREACHABLE();
      }
    case kIntrinsicUnsafeGet: {
      const bool is_volatile = (method.d.data & kIntrinsicFlagIsVolatile);
      switch (GetType(method.d.data, false)) {
        case Primitive::kPrimInt:
          return is_volatile ? Intrinsics::kUnsafeGetVolatile : Intrinsics::kUnsafeGet;
        case Primitive::kPrimLong:
          return is_volatile ? Intrinsics::kUnsafeGetLongVolatile : Intrinsics::kUnsafeGetLong;
        case Primitive::kPrimNot:
          return is_volatile ? Intrinsics::kUnsafeGetObjectVolatile : Intrinsics::kUnsafeGetObject;
        default:
          LOG(FATAL) << "Unknown/unsupported op size " << method.d.data;
          UNREACHABLE();
      }
    }
    case kIntrinsicUnsafePut: {
      enum Sync { kNoSync, kVolatile, kOrdered };
      const Sync sync =
          ((method.d.data & kIntrinsicFlagIsVolatile) != 0) ? kVolatile :
          ((method.d.data & kIntrinsicFlagIsOrdered) != 0)  ? kOrdered :
                                                              kNoSync;
      switch (GetType(method.d.data, false)) {
        case Primitive::kPrimInt:
          switch (sync) {
            case kNoSync:
              return Intrinsics::kUnsafePut;
            case kVolatile:
              return Intrinsics::kUnsafePutVolatile;
            case kOrdered:
              return Intrinsics::kUnsafePutOrdered;
          }
          break;
        case Primitive::kPrimLong:
          switch (sync) {
            case kNoSync:
              return Intrinsics::kUnsafePutLong;
            case kVolatile:
              return Intrinsics::kUnsafePutLongVolatile;
            case kOrdered:
              return Intrinsics::kUnsafePutLongOrdered;
          }
          break;
        case Primitive::kPrimNot:
          switch (sync) {
            case kNoSync:
              return Intrinsics::kUnsafePutObject;
            case kVolatile:
              return Intrinsics::kUnsafePutObjectVolatile;
            case kOrdered:
              return Intrinsics::kUnsafePutObjectOrdered;
          }
          break;
        default:
          LOG(FATAL) << "Unknown/unsupported op size " << method.d.data;
          UNREACHABLE();
      }
      break;
    }

    // Virtual cases.

    case kIntrinsicReferenceGetReferent:
      return Intrinsics::kReferenceGetReferent;

    // Quick inliner cases. Remove after refactoring. They are here so that we can use the
    // compiler to warn on missing cases.

    case kInlineOpNop:
    case kInlineOpReturnArg:
    case kInlineOpNonWideConst:
    case kInlineOpIGet:
    case kInlineOpIPut:
      return Intrinsics::kNone;

    // String init cases, not intrinsics.

    case kInlineStringInit:
      return Intrinsics::kNone;

    // No default case to make the compiler warn on missing cases.
  }
  return Intrinsics::kNone;
}

static bool CheckInvokeType(Intrinsics intrinsic, HInvoke* invoke) {
  // The DexFileMethodInliner should have checked whether the methods are agreeing with
  // what we expect, i.e., static methods are called as such. Add another check here for
  // our expectations:
  // Whenever the intrinsic is marked as static-or-direct, report an error if we find an
  // InvokeVirtual. The other direction is not possible: we have intrinsics for virtual
  // functions that will perform a check inline. If the precise type is known, however,
  // the instruction will be sharpened to an InvokeStaticOrDirect.
  InvokeType intrinsic_type = GetIntrinsicInvokeType(intrinsic);
  InvokeType invoke_type = invoke->IsInvokeStaticOrDirect() ?
      invoke->AsInvokeStaticOrDirect()->GetInvokeType() :
      invoke->IsInvokeVirtual() ? kVirtual : kSuper;
  switch (intrinsic_type) {
    case kStatic:
      return (invoke_type == kStatic);
    case kDirect:
      return (invoke_type == kDirect);
    case kVirtual:
      // Call might be devirtualized.
      return (invoke_type == kVirtual || invoke_type == kDirect);

    default:
      return false;
  }
}

// TODO: Refactor DexFileMethodInliner and have something nicer than InlineMethod.
void IntrinsicsRecognizer::Run() {
  DexFileMethodInliner* inliner = driver_->GetMethodInlinerMap()->GetMethodInliner(dex_file_);
  DCHECK(inliner != nullptr);

  for (HReversePostOrderIterator it(*graph_); !it.Done(); it.Advance()) {
    HBasicBlock* block = it.Current();
    for (HInstructionIterator inst_it(block->GetInstructions()); !inst_it.Done();
         inst_it.Advance()) {
      HInstruction* inst = inst_it.Current();
      if (inst->IsInvoke()) {
        HInvoke* invoke = inst->AsInvoke();
        InlineMethod method;
        if (inliner->IsIntrinsic(invoke->GetDexMethodIndex(), &method)) {
          Intrinsics intrinsic = GetIntrinsic(method);

          if (intrinsic != Intrinsics::kNone) {
            if (!CheckInvokeType(intrinsic, invoke)) {
              LOG(WARNING) << "Found an intrinsic with unexpected invoke type: "
                           << intrinsic << " for "
                           << PrettyMethod(invoke->GetDexMethodIndex(), *dex_file_);
            } else {
              invoke->SetIntrinsic(intrinsic);
            }
          }
        }
      }
    }
  }
}

std::ostream& operator<<(std::ostream& os, const Intrinsics& intrinsic) {
  switch (intrinsic) {
    case Intrinsics::kNone:
      os << "No intrinsic.";
      break;
#define OPTIMIZING_INTRINSICS(Name, IsStatic) \
    case Intrinsics::k ## Name: \
      os << # Name; \
      break;
#include "intrinsics_list.h"
INTRINSICS_LIST(OPTIMIZING_INTRINSICS)
#undef STATIC_INTRINSICS_LIST
#undef VIRTUAL_INTRINSICS_LIST
#undef OPTIMIZING_INTRINSICS
  }
  return os;
}

}  // namespace art
