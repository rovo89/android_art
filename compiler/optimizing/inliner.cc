/*
 * Copyright (C) 2014 The Android Open Source Project
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

#include "inliner.h"

#include "art_method-inl.h"
#include "builder.h"
#include "class_linker.h"
#include "constant_folding.h"
#include "dead_code_elimination.h"
#include "dex/verified_method.h"
#include "dex/verification_results.h"
#include "driver/compiler_driver-inl.h"
#include "driver/compiler_options.h"
#include "driver/dex_compilation_unit.h"
#include "instruction_simplifier.h"
#include "intrinsics.h"
#include "mirror/class_loader.h"
#include "mirror/dex_cache.h"
#include "nodes.h"
#include "optimizing_compiler.h"
#include "reference_type_propagation.h"
#include "register_allocator.h"
#include "quick/inline_method_analyser.h"
#include "sharpening.h"
#include "ssa_builder.h"
#include "ssa_phi_elimination.h"
#include "scoped_thread_state_change.h"
#include "thread.h"

namespace art {

static constexpr size_t kMaximumNumberOfHInstructions = 32;

// Limit the number of dex registers that we accumulate while inlining
// to avoid creating large amount of nested environments.
static constexpr size_t kMaximumNumberOfCumulatedDexRegisters = 64;

// Avoid inlining within a huge method due to memory pressure.
static constexpr size_t kMaximumCodeUnitSize = 4096;

void HInliner::Run() {
  const CompilerOptions& compiler_options = compiler_driver_->GetCompilerOptions();
  if ((compiler_options.GetInlineDepthLimit() == 0)
      || (compiler_options.GetInlineMaxCodeUnits() == 0)) {
    return;
  }
  if (caller_compilation_unit_.GetCodeItem()->insns_size_in_code_units_ > kMaximumCodeUnitSize) {
    return;
  }
  if (graph_->IsDebuggable()) {
    // For simplicity, we currently never inline when the graph is debuggable. This avoids
    // doing some logic in the runtime to discover if a method could have been inlined.
    return;
  }
  const ArenaVector<HBasicBlock*>& blocks = graph_->GetReversePostOrder();
  DCHECK(!blocks.empty());
  HBasicBlock* next_block = blocks[0];
  for (size_t i = 0; i < blocks.size(); ++i) {
    // Because we are changing the graph when inlining, we need to remember the next block.
    // This avoids doing the inlining work again on the inlined blocks.
    if (blocks[i] != next_block) {
      continue;
    }
    HBasicBlock* block = next_block;
    next_block = (i == blocks.size() - 1) ? nullptr : blocks[i + 1];
    for (HInstruction* instruction = block->GetFirstInstruction(); instruction != nullptr;) {
      HInstruction* next = instruction->GetNext();
      HInvoke* call = instruction->AsInvoke();
      // As long as the call is not intrinsified, it is worth trying to inline.
      if (call != nullptr && call->GetIntrinsic() == Intrinsics::kNone) {
        // We use the original invoke type to ensure the resolution of the called method
        // works properly.
        if (!TryInline(call)) {
          if (kIsDebugBuild && IsCompilingWithCoreImage()) {
            std::string callee_name =
                PrettyMethod(call->GetDexMethodIndex(), *outer_compilation_unit_.GetDexFile());
            bool should_inline = callee_name.find("$inline$") != std::string::npos;
            CHECK(!should_inline) << "Could not inline " << callee_name;
          }
        } else {
          if (kIsDebugBuild && IsCompilingWithCoreImage()) {
            std::string callee_name =
                PrettyMethod(call->GetDexMethodIndex(), *outer_compilation_unit_.GetDexFile());
            bool must_not_inline = callee_name.find("$noinline$") != std::string::npos;
            CHECK(!must_not_inline) << "Should not have inlined " << callee_name;
          }
        }
      }
      instruction = next;
    }
  }
}

static bool IsMethodOrDeclaringClassFinal(ArtMethod* method)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  return method->IsFinal() || method->GetDeclaringClass()->IsFinal();
}

/**
 * Given the `resolved_method` looked up in the dex cache, try to find
 * the actual runtime target of an interface or virtual call.
 * Return nullptr if the runtime target cannot be proven.
 */
static ArtMethod* FindVirtualOrInterfaceTarget(HInvoke* invoke, ArtMethod* resolved_method)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  if (IsMethodOrDeclaringClassFinal(resolved_method)) {
    // No need to lookup further, the resolved method will be the target.
    return resolved_method;
  }

  HInstruction* receiver = invoke->InputAt(0);
  if (receiver->IsNullCheck()) {
    // Due to multiple levels of inlining within the same pass, it might be that
    // null check does not have the reference type of the actual receiver.
    receiver = receiver->InputAt(0);
  }
  ReferenceTypeInfo info = receiver->GetReferenceTypeInfo();
  DCHECK(info.IsValid()) << "Invalid RTI for " << receiver->DebugName();
  if (!info.IsExact()) {
    // We currently only support inlining with known receivers.
    // TODO: Remove this check, we should be able to inline final methods
    // on unknown receivers.
    return nullptr;
  } else if (info.GetTypeHandle()->IsInterface()) {
    // Statically knowing that the receiver has an interface type cannot
    // help us find what is the target method.
    return nullptr;
  } else if (!resolved_method->GetDeclaringClass()->IsAssignableFrom(info.GetTypeHandle().Get())) {
    // The method that we're trying to call is not in the receiver's class or super classes.
    return nullptr;
  }

  ClassLinker* cl = Runtime::Current()->GetClassLinker();
  size_t pointer_size = cl->GetImagePointerSize();
  if (invoke->IsInvokeInterface()) {
    resolved_method = info.GetTypeHandle()->FindVirtualMethodForInterface(
        resolved_method, pointer_size);
  } else {
    DCHECK(invoke->IsInvokeVirtual());
    resolved_method = info.GetTypeHandle()->FindVirtualMethodForVirtual(
        resolved_method, pointer_size);
  }

  if (resolved_method == nullptr) {
    // The information we had on the receiver was not enough to find
    // the target method. Since we check above the exact type of the receiver,
    // the only reason this can happen is an IncompatibleClassChangeError.
    return nullptr;
  } else if (!resolved_method->IsInvokable()) {
    // The information we had on the receiver was not enough to find
    // the target method. Since we check above the exact type of the receiver,
    // the only reason this can happen is an IncompatibleClassChangeError.
    return nullptr;
  } else if (IsMethodOrDeclaringClassFinal(resolved_method)) {
    // A final method has to be the target method.
    return resolved_method;
  } else if (info.IsExact()) {
    // If we found a method and the receiver's concrete type is statically
    // known, we know for sure the target.
    return resolved_method;
  } else {
    // Even if we did find a method, the receiver type was not enough to
    // statically find the runtime target.
    return nullptr;
  }
}

static uint32_t FindMethodIndexIn(ArtMethod* method,
                                  const DexFile& dex_file,
                                  uint32_t referrer_index)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  if (IsSameDexFile(*method->GetDexFile(), dex_file)) {
    return method->GetDexMethodIndex();
  } else {
    return method->FindDexMethodIndexInOtherDexFile(dex_file, referrer_index);
  }
}

static uint32_t FindClassIndexIn(mirror::Class* cls, const DexFile& dex_file)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  if (cls->GetDexCache() == nullptr) {
    DCHECK(cls->IsArrayClass());
    // TODO: find the class in `dex_file`.
    return DexFile::kDexNoIndex;
  } else if (cls->GetDexTypeIndex() == DexFile::kDexNoIndex16) {
    // TODO: deal with proxy classes.
    return DexFile::kDexNoIndex;
  } else if (IsSameDexFile(cls->GetDexFile(), dex_file)) {
    // Update the dex cache to ensure the class is in. The generated code will
    // consider it is. We make it safe by updating the dex cache, as other
    // dex files might also load the class, and there is no guarantee the dex
    // cache of the dex file of the class will be updated.
    if (cls->GetDexCache()->GetResolvedType(cls->GetDexTypeIndex()) == nullptr) {
      cls->GetDexCache()->SetResolvedType(cls->GetDexTypeIndex(), cls);
    }
    return cls->GetDexTypeIndex();
  } else {
    // TODO: find the class in `dex_file`.
    return DexFile::kDexNoIndex;
  }
}

bool HInliner::TryInline(HInvoke* invoke_instruction) {
  if (invoke_instruction->IsInvokeUnresolved()) {
    return false;  // Don't bother to move further if we know the method is unresolved.
  }

  uint32_t method_index = invoke_instruction->GetDexMethodIndex();
  ScopedObjectAccess soa(Thread::Current());
  const DexFile& caller_dex_file = *caller_compilation_unit_.GetDexFile();
  VLOG(compiler) << "Try inlining " << PrettyMethod(method_index, caller_dex_file);

  ClassLinker* class_linker = caller_compilation_unit_.GetClassLinker();
  // We can query the dex cache directly. The verifier has populated it already.
  ArtMethod* resolved_method;
  ArtMethod* actual_method = nullptr;
  if (invoke_instruction->IsInvokeStaticOrDirect()) {
    if (invoke_instruction->AsInvokeStaticOrDirect()->IsStringInit()) {
      VLOG(compiler) << "Not inlining a String.<init> method";
      return false;
    }
    MethodReference ref = invoke_instruction->AsInvokeStaticOrDirect()->GetTargetMethod();
    mirror::DexCache* const dex_cache = (&caller_dex_file == ref.dex_file)
        ? caller_compilation_unit_.GetDexCache().Get()
        : class_linker->FindDexCache(soa.Self(), *ref.dex_file);
    resolved_method = dex_cache->GetResolvedMethod(
        ref.dex_method_index, class_linker->GetImagePointerSize());
    // actual_method == resolved_method for direct or static calls.
    actual_method = resolved_method;
  } else {
    resolved_method = caller_compilation_unit_.GetDexCache().Get()->GetResolvedMethod(
        method_index, class_linker->GetImagePointerSize());
    if (resolved_method != nullptr) {
      // Check if we can statically find the method.
      actual_method = FindVirtualOrInterfaceTarget(invoke_instruction, resolved_method);
    }
  }

  if (resolved_method == nullptr) {
    // TODO: Can this still happen?
    // Method cannot be resolved if it is in another dex file we do not have access to.
    VLOG(compiler) << "Method cannot be resolved " << PrettyMethod(method_index, caller_dex_file);
    return false;
  }

  if (actual_method != nullptr) {
    return TryInline(invoke_instruction, actual_method);
  }
  DCHECK(!invoke_instruction->IsInvokeStaticOrDirect());

  // Check if we can use an inline cache.
  ArtMethod* caller = graph_->GetArtMethod();
  size_t pointer_size = class_linker->GetImagePointerSize();
  // Under JIT, we should always know the caller.
  DCHECK(!Runtime::Current()->UseJit() || (caller != nullptr));
  if (caller != nullptr && caller->GetProfilingInfo(pointer_size) != nullptr) {
    ProfilingInfo* profiling_info = caller->GetProfilingInfo(pointer_size);
    const InlineCache& ic = *profiling_info->GetInlineCache(invoke_instruction->GetDexPc());
    if (ic.IsUnitialized()) {
      VLOG(compiler) << "Interface or virtual call to "
                     << PrettyMethod(method_index, caller_dex_file)
                     << " is not hit and not inlined";
      return false;
    } else if (ic.IsMonomorphic()) {
      MaybeRecordStat(kMonomorphicCall);
      return TryInlineMonomorphicCall(invoke_instruction, resolved_method, ic);
    } else if (ic.IsPolymorphic()) {
      MaybeRecordStat(kPolymorphicCall);
      return TryInlinePolymorphicCall(invoke_instruction, resolved_method, ic);
    } else {
      DCHECK(ic.IsMegamorphic());
      VLOG(compiler) << "Interface or virtual call to "
                     << PrettyMethod(method_index, caller_dex_file)
                     << " is megamorphic and not inlined";
      MaybeRecordStat(kMegamorphicCall);
      return false;
    }
  }

  VLOG(compiler) << "Interface or virtual call to "
                 << PrettyMethod(method_index, caller_dex_file)
                 << " could not be statically determined";
  return false;
}

HInstanceFieldGet* HInliner::BuildGetReceiverClass(ClassLinker* class_linker,
                                                   HInstruction* receiver,
                                                   uint32_t dex_pc) const {
  ArtField* field = class_linker->GetClassRoot(ClassLinker::kJavaLangObject)->GetInstanceField(0);
  DCHECK_EQ(std::string(field->GetName()), "shadow$_klass_");
  return new (graph_->GetArena()) HInstanceFieldGet(
      receiver,
      Primitive::kPrimNot,
      field->GetOffset(),
      field->IsVolatile(),
      field->GetDexFieldIndex(),
      field->GetDeclaringClass()->GetDexClassDefIndex(),
      *field->GetDexFile(),
      handles_->NewHandle(field->GetDexCache()),
      dex_pc);
}

bool HInliner::TryInlineMonomorphicCall(HInvoke* invoke_instruction,
                                        ArtMethod* resolved_method,
                                        const InlineCache& ic) {
  DCHECK(invoke_instruction->IsInvokeVirtual() || invoke_instruction->IsInvokeInterface())
      << invoke_instruction->DebugName();

  const DexFile& caller_dex_file = *caller_compilation_unit_.GetDexFile();
  uint32_t class_index = FindClassIndexIn(ic.GetMonomorphicType(), caller_dex_file);
  if (class_index == DexFile::kDexNoIndex) {
    VLOG(compiler) << "Call to " << PrettyMethod(resolved_method)
                   << " from inline cache is not inlined because its class is not"
                   << " accessible to the caller";
    return false;
  }

  ClassLinker* class_linker = caller_compilation_unit_.GetClassLinker();
  size_t pointer_size = class_linker->GetImagePointerSize();
  if (invoke_instruction->IsInvokeInterface()) {
    resolved_method = ic.GetMonomorphicType()->FindVirtualMethodForInterface(
        resolved_method, pointer_size);
  } else {
    DCHECK(invoke_instruction->IsInvokeVirtual());
    resolved_method = ic.GetMonomorphicType()->FindVirtualMethodForVirtual(
        resolved_method, pointer_size);
  }
  DCHECK(resolved_method != nullptr);
  HInstruction* receiver = invoke_instruction->InputAt(0);
  HInstruction* cursor = invoke_instruction->GetPrevious();
  HBasicBlock* bb_cursor = invoke_instruction->GetBlock();

  if (!TryInline(invoke_instruction, resolved_method, /* do_rtp */ false)) {
    return false;
  }

  // We successfully inlined, now add a guard.
  HInstanceFieldGet* receiver_class = BuildGetReceiverClass(
      class_linker, receiver, invoke_instruction->GetDexPc());

  bool is_referrer =
      (ic.GetMonomorphicType() == outermost_graph_->GetArtMethod()->GetDeclaringClass());
  HLoadClass* load_class = new (graph_->GetArena()) HLoadClass(graph_->GetCurrentMethod(),
                                                               class_index,
                                                               caller_dex_file,
                                                               is_referrer,
                                                               invoke_instruction->GetDexPc(),
                                                               /* needs_access_check */ false,
                                                               /* is_in_dex_cache */ true);

  HNotEqual* compare = new (graph_->GetArena()) HNotEqual(load_class, receiver_class);
  HDeoptimize* deoptimize = new (graph_->GetArena()) HDeoptimize(
      compare, invoke_instruction->GetDexPc());
  // TODO: Extend reference type propagation to understand the guard.
  if (cursor != nullptr) {
    bb_cursor->InsertInstructionAfter(receiver_class, cursor);
  } else {
    bb_cursor->InsertInstructionBefore(receiver_class, bb_cursor->GetFirstInstruction());
  }
  bb_cursor->InsertInstructionAfter(load_class, receiver_class);
  bb_cursor->InsertInstructionAfter(compare, load_class);
  bb_cursor->InsertInstructionAfter(deoptimize, compare);
  deoptimize->CopyEnvironmentFrom(invoke_instruction->GetEnvironment());

  // Run type propagation to get the guard typed, and eventually propagate the
  // type of the receiver.
  ReferenceTypePropagation rtp_fixup(graph_, handles_);
  rtp_fixup.Run();

  MaybeRecordStat(kInlinedMonomorphicCall);
  return true;
}

bool HInliner::TryInlinePolymorphicCall(HInvoke* invoke_instruction,
                                        ArtMethod* resolved_method,
                                        const InlineCache& ic) {
  DCHECK(invoke_instruction->IsInvokeVirtual() || invoke_instruction->IsInvokeInterface())
      << invoke_instruction->DebugName();
  // This optimization only works under JIT for now.
  DCHECK(Runtime::Current()->UseJit());
  if (graph_->GetInstructionSet() == kMips || graph_->GetInstructionSet() == kMips64) {
    // TODO: Support HClassTableGet for mips and mips64.
    return false;
  }
  ClassLinker* class_linker = caller_compilation_unit_.GetClassLinker();
  size_t pointer_size = class_linker->GetImagePointerSize();

  DCHECK(resolved_method != nullptr);
  ArtMethod* actual_method = nullptr;
  // Check whether we are actually calling the same method among
  // the different types seen.
  for (size_t i = 0; i < InlineCache::kIndividualCacheSize; ++i) {
    if (ic.GetTypeAt(i) == nullptr) {
      break;
    }
    ArtMethod* new_method = nullptr;
    if (invoke_instruction->IsInvokeInterface()) {
      new_method = ic.GetTypeAt(i)->FindVirtualMethodForInterface(
          resolved_method, pointer_size);
    } else {
      DCHECK(invoke_instruction->IsInvokeVirtual());
      new_method = ic.GetTypeAt(i)->FindVirtualMethodForVirtual(
          resolved_method, pointer_size);
    }
    if (actual_method == nullptr) {
      actual_method = new_method;
    } else if (actual_method != new_method) {
      // Different methods, bailout.
      return false;
    }
  }

  HInstruction* receiver = invoke_instruction->InputAt(0);
  HInstruction* cursor = invoke_instruction->GetPrevious();
  HBasicBlock* bb_cursor = invoke_instruction->GetBlock();

  if (!TryInline(invoke_instruction, actual_method, /* do_rtp */ false)) {
    return false;
  }

  // We successfully inlined, now add a guard.
  HInstanceFieldGet* receiver_class = BuildGetReceiverClass(
      class_linker, receiver, invoke_instruction->GetDexPc());

  size_t method_offset = invoke_instruction->IsInvokeVirtual()
      ? actual_method->GetVtableIndex()
      : invoke_instruction->AsInvokeInterface()->GetImtIndex();

  Primitive::Type type = Is64BitInstructionSet(graph_->GetInstructionSet())
      ? Primitive::kPrimLong
      : Primitive::kPrimInt;
  HClassTableGet* class_table_get = new (graph_->GetArena()) HClassTableGet(
      receiver_class,
      type,
      invoke_instruction->IsInvokeVirtual() ? HClassTableGet::kVTable : HClassTableGet::kIMTable,
      method_offset,
      invoke_instruction->GetDexPc());

  HConstant* constant;
  if (type == Primitive::kPrimLong) {
    constant = graph_->GetLongConstant(
        reinterpret_cast<intptr_t>(actual_method), invoke_instruction->GetDexPc());
  } else {
    constant = graph_->GetIntConstant(
        reinterpret_cast<intptr_t>(actual_method), invoke_instruction->GetDexPc());
  }

  HNotEqual* compare = new (graph_->GetArena()) HNotEqual(class_table_get, constant);
  HDeoptimize* deoptimize = new (graph_->GetArena()) HDeoptimize(
      compare, invoke_instruction->GetDexPc());
  // TODO: Extend reference type propagation to understand the guard.
  if (cursor != nullptr) {
    bb_cursor->InsertInstructionAfter(receiver_class, cursor);
  } else {
    bb_cursor->InsertInstructionBefore(receiver_class, bb_cursor->GetFirstInstruction());
  }
  bb_cursor->InsertInstructionAfter(class_table_get, receiver_class);
  bb_cursor->InsertInstructionAfter(compare, class_table_get);
  bb_cursor->InsertInstructionAfter(deoptimize, compare);
  deoptimize->CopyEnvironmentFrom(invoke_instruction->GetEnvironment());

  // Run type propagation to get the guard typed.
  ReferenceTypePropagation rtp_fixup(graph_, handles_);
  rtp_fixup.Run();

  MaybeRecordStat(kInlinedPolymorphicCall);

  return true;
}

bool HInliner::TryInline(HInvoke* invoke_instruction, ArtMethod* method, bool do_rtp) {
  const DexFile& caller_dex_file = *caller_compilation_unit_.GetDexFile();

  // Check whether we're allowed to inline. The outermost compilation unit is the relevant
  // dex file here (though the transitivity of an inline chain would allow checking the calller).
  if (!compiler_driver_->MayInline(method->GetDexFile(),
                                   outer_compilation_unit_.GetDexFile())) {
    if (TryPatternSubstitution(invoke_instruction, method, do_rtp)) {
      VLOG(compiler) << "Successfully replaced pattern of invoke " << PrettyMethod(method);
      MaybeRecordStat(kReplacedInvokeWithSimplePattern);
      return true;
    }
    VLOG(compiler) << "Won't inline " << PrettyMethod(method) << " in "
                   << outer_compilation_unit_.GetDexFile()->GetLocation() << " ("
                   << caller_compilation_unit_.GetDexFile()->GetLocation() << ") from "
                   << method->GetDexFile()->GetLocation();
    return false;
  }

  uint32_t method_index = FindMethodIndexIn(
      method, caller_dex_file, invoke_instruction->GetDexMethodIndex());
  if (method_index == DexFile::kDexNoIndex) {
    VLOG(compiler) << "Call to "
                   << PrettyMethod(method)
                   << " cannot be inlined because unaccessible to caller";
    return false;
  }

  bool same_dex_file = IsSameDexFile(*outer_compilation_unit_.GetDexFile(), *method->GetDexFile());

  const DexFile::CodeItem* code_item = method->GetCodeItem();

  if (code_item == nullptr) {
    VLOG(compiler) << "Method " << PrettyMethod(method)
                   << " is not inlined because it is native";
    return false;
  }

  size_t inline_max_code_units = compiler_driver_->GetCompilerOptions().GetInlineMaxCodeUnits();
  if (code_item->insns_size_in_code_units_ > inline_max_code_units) {
    VLOG(compiler) << "Method " << PrettyMethod(method)
                   << " is too big to inline: "
                   << code_item->insns_size_in_code_units_
                   << " > "
                   << inline_max_code_units;
    return false;
  }

  if (code_item->tries_size_ != 0) {
    VLOG(compiler) << "Method " << PrettyMethod(method)
                   << " is not inlined because of try block";
    return false;
  }

  if (!method->GetDeclaringClass()->IsVerified()) {
    uint16_t class_def_idx = method->GetDeclaringClass()->GetDexClassDefIndex();
    if (!compiler_driver_->IsMethodVerifiedWithoutFailures(
          method->GetDexMethodIndex(), class_def_idx, *method->GetDexFile())) {
      VLOG(compiler) << "Method " << PrettyMethod(method_index, caller_dex_file)
                     << " couldn't be verified, so it cannot be inlined";
      return false;
    }
  }

  if (invoke_instruction->IsInvokeStaticOrDirect() &&
      invoke_instruction->AsInvokeStaticOrDirect()->IsStaticWithImplicitClinitCheck()) {
    // Case of a static method that cannot be inlined because it implicitly
    // requires an initialization check of its declaring class.
    VLOG(compiler) << "Method " << PrettyMethod(method_index, caller_dex_file)
                   << " is not inlined because it is static and requires a clinit"
                   << " check that cannot be emitted due to Dex cache limitations";
    return false;
  }

  if (!TryBuildAndInline(method, invoke_instruction, same_dex_file, do_rtp)) {
    return false;
  }

  VLOG(compiler) << "Successfully inlined " << PrettyMethod(method_index, caller_dex_file);
  MaybeRecordStat(kInlinedInvoke);
  return true;
}

static HInstruction* GetInvokeInputForArgVRegIndex(HInvoke* invoke_instruction,
                                                   size_t arg_vreg_index)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  size_t input_index = 0;
  for (size_t i = 0; i < arg_vreg_index; ++i, ++input_index) {
    DCHECK_LT(input_index, invoke_instruction->GetNumberOfArguments());
    if (Primitive::Is64BitType(invoke_instruction->InputAt(input_index)->GetType())) {
      ++i;
      DCHECK_NE(i, arg_vreg_index);
    }
  }
  DCHECK_LT(input_index, invoke_instruction->GetNumberOfArguments());
  return invoke_instruction->InputAt(input_index);
}

// Try to recognize known simple patterns and replace invoke call with appropriate instructions.
bool HInliner::TryPatternSubstitution(HInvoke* invoke_instruction,
                                      ArtMethod* resolved_method,
                                      bool do_rtp) {
  InlineMethod inline_method;
  if (!InlineMethodAnalyser::AnalyseMethodCode(resolved_method, &inline_method)) {
    return false;
  }

  HInstruction* return_replacement = nullptr;
  switch (inline_method.opcode) {
    case kInlineOpNop:
      DCHECK_EQ(invoke_instruction->GetType(), Primitive::kPrimVoid);
      break;
    case kInlineOpReturnArg:
      return_replacement = GetInvokeInputForArgVRegIndex(invoke_instruction,
                                                         inline_method.d.return_data.arg);
      break;
    case kInlineOpNonWideConst:
      if (resolved_method->GetShorty()[0] == 'L') {
        DCHECK_EQ(inline_method.d.data, 0u);
        return_replacement = graph_->GetNullConstant();
      } else {
        return_replacement = graph_->GetIntConstant(static_cast<int32_t>(inline_method.d.data));
      }
      break;
    case kInlineOpIGet: {
      const InlineIGetIPutData& data = inline_method.d.ifield_data;
      if (data.method_is_static || data.object_arg != 0u) {
        // TODO: Needs null check.
        return false;
      }
      HInstruction* obj = GetInvokeInputForArgVRegIndex(invoke_instruction, data.object_arg);
      HInstanceFieldGet* iget = CreateInstanceFieldGet(resolved_method, data.field_idx, obj);
      DCHECK_EQ(iget->GetFieldOffset().Uint32Value(), data.field_offset);
      DCHECK_EQ(iget->IsVolatile() ? 1u : 0u, data.is_volatile);
      invoke_instruction->GetBlock()->InsertInstructionBefore(iget, invoke_instruction);
      return_replacement = iget;
      break;
    }
    case kInlineOpIPut: {
      const InlineIGetIPutData& data = inline_method.d.ifield_data;
      if (data.method_is_static || data.object_arg != 0u) {
        // TODO: Needs null check.
        return false;
      }
      HInstruction* obj = GetInvokeInputForArgVRegIndex(invoke_instruction, data.object_arg);
      HInstruction* value = GetInvokeInputForArgVRegIndex(invoke_instruction, data.src_arg);
      HInstanceFieldSet* iput = CreateInstanceFieldSet(resolved_method, data.field_idx, obj, value);
      DCHECK_EQ(iput->GetFieldOffset().Uint32Value(), data.field_offset);
      DCHECK_EQ(iput->IsVolatile() ? 1u : 0u, data.is_volatile);
      invoke_instruction->GetBlock()->InsertInstructionBefore(iput, invoke_instruction);
      if (data.return_arg_plus1 != 0u) {
        size_t return_arg = data.return_arg_plus1 - 1u;
        return_replacement = GetInvokeInputForArgVRegIndex(invoke_instruction, return_arg);
      }
      break;
    }
    default:
      LOG(FATAL) << "UNREACHABLE";
      UNREACHABLE();
  }

  if (return_replacement != nullptr) {
    invoke_instruction->ReplaceWith(return_replacement);
  }
  invoke_instruction->GetBlock()->RemoveInstruction(invoke_instruction);

  FixUpReturnReferenceType(resolved_method, invoke_instruction, return_replacement, do_rtp);
  return true;
}

HInstanceFieldGet* HInliner::CreateInstanceFieldGet(ArtMethod* resolved_method,
                                                    uint32_t field_index,
                                                    HInstruction* obj)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  Handle<mirror::DexCache> dex_cache(handles_->NewHandle(resolved_method->GetDexCache()));
  size_t pointer_size = InstructionSetPointerSize(codegen_->GetInstructionSet());
  ArtField* resolved_field = dex_cache->GetResolvedField(field_index, pointer_size);
  DCHECK(resolved_field != nullptr);
  HInstanceFieldGet* iget = new (graph_->GetArena()) HInstanceFieldGet(
      obj,
      resolved_field->GetTypeAsPrimitiveType(),
      resolved_field->GetOffset(),
      resolved_field->IsVolatile(),
      field_index,
      resolved_field->GetDeclaringClass()->GetDexClassDefIndex(),
      *resolved_method->GetDexFile(),
      dex_cache,
      // Read barrier generates a runtime call in slow path and we need a valid
      // dex pc for the associated stack map. 0 is bogus but valid. Bug: 26854537.
      /* dex_pc */ 0);
  if (iget->GetType() == Primitive::kPrimNot) {
    ReferenceTypePropagation rtp(graph_, handles_);
    rtp.Visit(iget);
  }
  return iget;
}

HInstanceFieldSet* HInliner::CreateInstanceFieldSet(ArtMethod* resolved_method,
                                                    uint32_t field_index,
                                                    HInstruction* obj,
                                                    HInstruction* value)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  Handle<mirror::DexCache> dex_cache(handles_->NewHandle(resolved_method->GetDexCache()));
  size_t pointer_size = InstructionSetPointerSize(codegen_->GetInstructionSet());
  ArtField* resolved_field = dex_cache->GetResolvedField(field_index, pointer_size);
  DCHECK(resolved_field != nullptr);
  HInstanceFieldSet* iput = new (graph_->GetArena()) HInstanceFieldSet(
      obj,
      value,
      resolved_field->GetTypeAsPrimitiveType(),
      resolved_field->GetOffset(),
      resolved_field->IsVolatile(),
      field_index,
      resolved_field->GetDeclaringClass()->GetDexClassDefIndex(),
      *resolved_method->GetDexFile(),
      dex_cache,
      // Read barrier generates a runtime call in slow path and we need a valid
      // dex pc for the associated stack map. 0 is bogus but valid. Bug: 26854537.
      /* dex_pc */ 0);
  return iput;
}
bool HInliner::TryBuildAndInline(ArtMethod* resolved_method,
                                 HInvoke* invoke_instruction,
                                 bool same_dex_file,
                                 bool do_rtp) {
  ScopedObjectAccess soa(Thread::Current());
  const DexFile::CodeItem* code_item = resolved_method->GetCodeItem();
  const DexFile& callee_dex_file = *resolved_method->GetDexFile();
  uint32_t method_index = resolved_method->GetDexMethodIndex();
  ClassLinker* class_linker = caller_compilation_unit_.GetClassLinker();
  Handle<mirror::DexCache> dex_cache(handles_->NewHandle(resolved_method->GetDexCache()));
  DexCompilationUnit dex_compilation_unit(
    nullptr,
    caller_compilation_unit_.GetClassLoader(),
    class_linker,
    callee_dex_file,
    code_item,
    resolved_method->GetDeclaringClass()->GetDexClassDefIndex(),
    method_index,
    resolved_method->GetAccessFlags(),
    compiler_driver_->GetVerifiedMethod(&callee_dex_file, method_index),
    dex_cache);

  bool requires_ctor_barrier = false;

  if (dex_compilation_unit.IsConstructor()) {
    // If it's a super invocation and we already generate a barrier there's no need
    // to generate another one.
    // We identify super calls by looking at the "this" pointer. If its value is the
    // same as the local "this" pointer then we must have a super invocation.
    bool is_super_invocation = invoke_instruction->InputAt(0)->IsParameterValue()
        && invoke_instruction->InputAt(0)->AsParameterValue()->IsThis();
    if (is_super_invocation && graph_->ShouldGenerateConstructorBarrier()) {
      requires_ctor_barrier = false;
    } else {
      Thread* self = Thread::Current();
      requires_ctor_barrier = compiler_driver_->RequiresConstructorBarrier(self,
          dex_compilation_unit.GetDexFile(),
          dex_compilation_unit.GetClassDefIndex());
    }
  }

  InvokeType invoke_type = invoke_instruction->GetOriginalInvokeType();
  if (invoke_type == kInterface) {
    // We have statically resolved the dispatch. To please the class linker
    // at runtime, we change this call as if it was a virtual call.
    invoke_type = kVirtual;
  }
  HGraph* callee_graph = new (graph_->GetArena()) HGraph(
      graph_->GetArena(),
      callee_dex_file,
      method_index,
      requires_ctor_barrier,
      compiler_driver_->GetInstructionSet(),
      invoke_type,
      graph_->IsDebuggable(),
      graph_->GetCurrentInstructionId());
  callee_graph->SetArtMethod(resolved_method);

  OptimizingCompilerStats inline_stats;
  HGraphBuilder builder(callee_graph,
                        &dex_compilation_unit,
                        &outer_compilation_unit_,
                        resolved_method->GetDexFile(),
                        compiler_driver_,
                        &inline_stats,
                        resolved_method->GetQuickenedInfo(),
                        dex_cache);

  if (!builder.BuildGraph(*code_item)) {
    VLOG(compiler) << "Method " << PrettyMethod(method_index, callee_dex_file)
                   << " could not be built, so cannot be inlined";
    return false;
  }

  if (!RegisterAllocator::CanAllocateRegistersFor(*callee_graph,
                                                  compiler_driver_->GetInstructionSet())) {
    VLOG(compiler) << "Method " << PrettyMethod(method_index, callee_dex_file)
                   << " cannot be inlined because of the register allocator";
    return false;
  }

  if (callee_graph->TryBuildingSsa(handles_) != kAnalysisSuccess) {
    VLOG(compiler) << "Method " << PrettyMethod(method_index, callee_dex_file)
                   << " could not be transformed to SSA";
    return false;
  }

  size_t parameter_index = 0;
  for (HInstructionIterator instructions(callee_graph->GetEntryBlock()->GetInstructions());
       !instructions.Done();
       instructions.Advance()) {
    HInstruction* current = instructions.Current();
    if (current->IsParameterValue()) {
      HInstruction* argument = invoke_instruction->InputAt(parameter_index++);
      if (argument->IsNullConstant()) {
        current->ReplaceWith(callee_graph->GetNullConstant());
      } else if (argument->IsIntConstant()) {
        current->ReplaceWith(callee_graph->GetIntConstant(argument->AsIntConstant()->GetValue()));
      } else if (argument->IsLongConstant()) {
        current->ReplaceWith(callee_graph->GetLongConstant(argument->AsLongConstant()->GetValue()));
      } else if (argument->IsFloatConstant()) {
        current->ReplaceWith(
            callee_graph->GetFloatConstant(argument->AsFloatConstant()->GetValue()));
      } else if (argument->IsDoubleConstant()) {
        current->ReplaceWith(
            callee_graph->GetDoubleConstant(argument->AsDoubleConstant()->GetValue()));
      } else if (argument->GetType() == Primitive::kPrimNot) {
        current->SetReferenceTypeInfo(argument->GetReferenceTypeInfo());
        current->AsParameterValue()->SetCanBeNull(argument->CanBeNull());
      }
    }
  }

  // Run simple optimizations on the graph.
  HDeadCodeElimination dce(callee_graph, stats_);
  HConstantFolding fold(callee_graph);
  HSharpening sharpening(callee_graph, codegen_, dex_compilation_unit, compiler_driver_);
  InstructionSimplifier simplify(callee_graph, stats_);
  IntrinsicsRecognizer intrinsics(callee_graph, compiler_driver_);

  HOptimization* optimizations[] = {
    &intrinsics,
    &sharpening,
    &simplify,
    &fold,
    &dce,
  };

  for (size_t i = 0; i < arraysize(optimizations); ++i) {
    HOptimization* optimization = optimizations[i];
    optimization->Run();
  }

  size_t number_of_instructions_budget = kMaximumNumberOfHInstructions;
  if (depth_ + 1 < compiler_driver_->GetCompilerOptions().GetInlineDepthLimit()) {
    HInliner inliner(callee_graph,
                     outermost_graph_,
                     codegen_,
                     outer_compilation_unit_,
                     dex_compilation_unit,
                     compiler_driver_,
                     handles_,
                     stats_,
                     total_number_of_dex_registers_ + code_item->registers_size_,
                     depth_ + 1);
    inliner.Run();
    number_of_instructions_budget += inliner.number_of_inlined_instructions_;
  }

  // TODO: We should abort only if all predecessors throw. However,
  // HGraph::InlineInto currently does not handle an exit block with
  // a throw predecessor.
  HBasicBlock* exit_block = callee_graph->GetExitBlock();
  if (exit_block == nullptr) {
    VLOG(compiler) << "Method " << PrettyMethod(method_index, callee_dex_file)
                   << " could not be inlined because it has an infinite loop";
    return false;
  }

  bool has_throw_predecessor = false;
  for (HBasicBlock* predecessor : exit_block->GetPredecessors()) {
    if (predecessor->GetLastInstruction()->IsThrow()) {
      has_throw_predecessor = true;
      break;
    }
  }
  if (has_throw_predecessor) {
    VLOG(compiler) << "Method " << PrettyMethod(method_index, callee_dex_file)
                   << " could not be inlined because one branch always throws";
    return false;
  }

  HReversePostOrderIterator it(*callee_graph);
  it.Advance();  // Past the entry block, it does not contain instructions that prevent inlining.
  size_t number_of_instructions = 0;

  bool can_inline_environment =
      total_number_of_dex_registers_ < kMaximumNumberOfCumulatedDexRegisters;

  for (; !it.Done(); it.Advance()) {
    HBasicBlock* block = it.Current();

    if (block->IsLoopHeader() && block->GetLoopInformation()->IsIrreducible()) {
      // Don't inline methods with irreducible loops, they could prevent some
      // optimizations to run.
      VLOG(compiler) << "Method " << PrettyMethod(method_index, callee_dex_file)
                     << " could not be inlined because it contains an irreducible loop";
      return false;
    }

    for (HInstructionIterator instr_it(block->GetInstructions());
         !instr_it.Done();
         instr_it.Advance()) {
      if (number_of_instructions++ ==  number_of_instructions_budget) {
        VLOG(compiler) << "Method " << PrettyMethod(method_index, callee_dex_file)
                       << " is not inlined because its caller has reached"
                       << " its instruction budget limit.";
        return false;
      }
      HInstruction* current = instr_it.Current();
      if (!can_inline_environment && current->NeedsEnvironment()) {
        VLOG(compiler) << "Method " << PrettyMethod(method_index, callee_dex_file)
                       << " is not inlined because its caller has reached"
                       << " its environment budget limit.";
        return false;
      }

      if (current->IsInvokeInterface()) {
        // Disable inlining of interface calls. The cost in case of entering the
        // resolution conflict is currently too high.
        VLOG(compiler) << "Method " << PrettyMethod(method_index, callee_dex_file)
                       << " could not be inlined because it has an interface call.";
        return false;
      }

      if (!same_dex_file && current->NeedsEnvironment()) {
        VLOG(compiler) << "Method " << PrettyMethod(method_index, callee_dex_file)
                       << " could not be inlined because " << current->DebugName()
                       << " needs an environment and is in a different dex file";
        return false;
      }

      if (!same_dex_file && current->NeedsDexCacheOfDeclaringClass()) {
        VLOG(compiler) << "Method " << PrettyMethod(method_index, callee_dex_file)
                       << " could not be inlined because " << current->DebugName()
                       << " it is in a different dex file and requires access to the dex cache";
        return false;
      }

      if (current->IsNewInstance() &&
          (current->AsNewInstance()->GetEntrypoint() == kQuickAllocObjectWithAccessCheck)) {
        // Allocation entrypoint does not handle inlined frames.
        return false;
      }

      if (current->IsNewArray() &&
          (current->AsNewArray()->GetEntrypoint() == kQuickAllocArrayWithAccessCheck)) {
        // Allocation entrypoint does not handle inlined frames.
        return false;
      }

      if (current->IsUnresolvedStaticFieldGet() ||
          current->IsUnresolvedInstanceFieldGet() ||
          current->IsUnresolvedStaticFieldSet() ||
          current->IsUnresolvedInstanceFieldSet()) {
        // Entrypoint for unresolved fields does not handle inlined frames.
        return false;
      }
    }
  }
  number_of_inlined_instructions_ += number_of_instructions;

  HInstruction* return_replacement = callee_graph->InlineInto(graph_, invoke_instruction);
  if (return_replacement != nullptr) {
    DCHECK_EQ(graph_, return_replacement->GetBlock()->GetGraph());
  }
  FixUpReturnReferenceType(resolved_method, invoke_instruction, return_replacement, do_rtp);
  return true;
}

void HInliner::FixUpReturnReferenceType(ArtMethod* resolved_method,
                                        HInvoke* invoke_instruction,
                                        HInstruction* return_replacement,
                                        bool do_rtp) {
  // Check the integrity of reference types and run another type propagation if needed.
  if (return_replacement != nullptr) {
    if (return_replacement->GetType() == Primitive::kPrimNot) {
      if (!return_replacement->GetReferenceTypeInfo().IsValid()) {
        // Make sure that we have a valid type for the return. We may get an invalid one when
        // we inline invokes with multiple branches and create a Phi for the result.
        // TODO: we could be more precise by merging the phi inputs but that requires
        // some functionality from the reference type propagation.
        DCHECK(return_replacement->IsPhi());
        size_t pointer_size = Runtime::Current()->GetClassLinker()->GetImagePointerSize();
        ReferenceTypeInfo::TypeHandle return_handle =
            handles_->NewHandle(resolved_method->GetReturnType(true /* resolve */, pointer_size));
        return_replacement->SetReferenceTypeInfo(ReferenceTypeInfo::Create(
            return_handle, return_handle->CannotBeAssignedFromOtherTypes() /* is_exact */));
      }

      if (do_rtp) {
        // If the return type is a refinement of the declared type run the type propagation again.
        ReferenceTypeInfo return_rti = return_replacement->GetReferenceTypeInfo();
        ReferenceTypeInfo invoke_rti = invoke_instruction->GetReferenceTypeInfo();
        if (invoke_rti.IsStrictSupertypeOf(return_rti)
            || (return_rti.IsExact() && !invoke_rti.IsExact())
            || !return_replacement->CanBeNull()) {
          ReferenceTypePropagation(graph_, handles_).Run();
        }
      }
    } else if (return_replacement->IsInstanceOf()) {
      if (do_rtp) {
        // Inlining InstanceOf into an If may put a tighter bound on reference types.
        ReferenceTypePropagation(graph_, handles_).Run();
      }
    }
  }
}

}  // namespace art
