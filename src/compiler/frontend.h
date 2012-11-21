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

#ifndef ART_SRC_COMPILER_COMPILER_H_
#define ART_SRC_COMPILER_COMPILER_H_

#include "dex_file.h"
#include "dex_instruction.h"

namespace llvm {
  class Module;
  class LLVMContext;
}

namespace art {
namespace greenland {
  class IntrinsicHelper;
  class IRBuilder;
}

#define COMPILER_TRACED(X)
#define COMPILER_TRACEE(X)

/*
 * Special offsets to denote method entry/exit for debugger update.
 * NOTE: bit pattern must be loadable using 1 instruction and must
 * not be a valid Dalvik offset.
 */
#define DEBUGGER_METHOD_ENTRY -1
#define DEBUGGER_METHOD_EXIT -2

/*
 * Assembly is an iterative process, and usually terminates within
 * two or three passes.  This should be high enough to handle bizarre
 * cases, but detect an infinite loop bug.
 */
#define MAX_ASSEMBLER_RETRIES 50

/* Suppress optimization if corresponding bit set */
enum optControlVector {
  kLoadStoreElimination = 0,
  kLoadHoisting,
  kSuppressLoads,
  kNullCheckElimination,
  kPromoteRegs,
  kTrackLiveTemps,
  kSkipLargeMethodOptimization,
  kSafeOptimizations,
  kBBOpt,
  kMatch,
  kPromoteCompilerTemps,
};

/* Force code generation paths for testing */
enum debugControlVector {
  kDebugDisplayMissingTargets,
  kDebugVerbose,
  kDebugDumpCFG,
  kDebugSlowFieldPath,
  kDebugSlowInvokePath,
  kDebugSlowStringPath,
  kDebugSlowTypePath,
  kDebugSlowestFieldPath,
  kDebugSlowestStringPath,
  kDebugExerciseResolveMethod,
  kDebugVerifyDataflow,
  kDebugShowMemoryUsage,
  kDebugShowNops,
  kDebugCountOpcodes,
  kDebugDumpCheckStats,
  kDebugDumpBitcodeFile,
  kDebugVerifyBitcode,
};

enum OatMethodAttributes {
  kIsCallee = 0,      /* Code is part of a callee (invoked by a hot trace) */
  kIsHot,             /* Code is part of a hot trace */
  kIsLeaf,            /* Method is leaf */
  kIsEmpty,           /* Method is empty */
  kIsThrowFree,       /* Method doesn't throw */
  kIsGetter,          /* Method fits the getter pattern */
  kIsSetter,          /* Method fits the setter pattern */
  kCannotCompile,     /* Method cannot be compiled */
};

#define METHOD_IS_CALLEE        (1 << kIsCallee)
#define METHOD_IS_HOT           (1 << kIsHot)
#define METHOD_IS_LEAF          (1 << kIsLeaf)
#define METHOD_IS_EMPTY         (1 << kIsEmpty)
#define METHOD_IS_THROW_FREE    (1 << kIsThrowFree)
#define METHOD_IS_GETTER        (1 << kIsGetter)
#define METHOD_IS_SETTER        (1 << kIsSetter)
#define METHOD_CANNOT_COMPILE   (1 << kCannotCompile)

class LLVMInfo {
  public:
    LLVMInfo();
    ~LLVMInfo();

    llvm::LLVMContext* GetLLVMContext() {
      return llvm_context_.get();
    }

    llvm::Module* GetLLVMModule() {
      return llvm_module_;
    }

    art::greenland::IntrinsicHelper* GetIntrinsicHelper() {
      return intrinsic_helper_.get();
    }

    art::greenland::IRBuilder* GetIRBuilder() {
      return ir_builder_.get();
    }

  private:
    UniquePtr<llvm::LLVMContext> llvm_context_;
    llvm::Module* llvm_module_; // Managed by context_
    UniquePtr<art::greenland::IntrinsicHelper> intrinsic_helper_;
    UniquePtr<art::greenland::IRBuilder> ir_builder_;
};

struct CompilationUnit;
struct BasicBlock;

BasicBlock* FindBlock(CompilationUnit* cUnit, unsigned int codeOffset);
void ReplaceSpecialChars(std::string& str);

}  // namespace art

extern "C" art::CompiledMethod* ArtCompileMethod(art::Compiler& compiler,
                                                 const art::DexFile::CodeItem* code_item,
                                                 uint32_t access_flags,
                                                 art::InvokeType invoke_type,
                                                 uint32_t method_idx,
                                                 jobject class_loader,
                                                 const art::DexFile& dex_file);

#endif // ART_SRC_COMPILER_COMPILER_H_
