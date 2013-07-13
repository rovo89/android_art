#ifdef ART_SEA_IR_MODE
#include <llvm/Support/Threading.h>
#include "base/logging.h"
#include "dex/portable/mir_to_gbc.h"
#include "driver/compiler_driver.h"
#include "leb128.h"
#include "llvm/llvm_compilation_unit.h"
#include "mirror/object.h"
#include "runtime.h"
#include "sea_ir/sea.h"

namespace art {

static CompiledMethod* CompileMethodWithSeaIr(CompilerDriver& compiler,
                                     const CompilerBackend compiler_backend,
                                     const DexFile::CodeItem* code_item,
                                     uint32_t access_flags, InvokeType invoke_type,
                                     uint32_t class_def_idx, uint32_t method_idx,
                                     jobject class_loader, const DexFile& dex_file
#if defined(ART_USE_PORTABLE_COMPILER)
                                     , llvm::LlvmCompilationUnit* llvm_compilation_unit
#endif
) {
  // NOTE: Instead of keeping the convention from the Dalvik frontend.cc
  //       and silencing the cpplint.py warning, I just corrected the formatting.
  VLOG(compiler) << "Compiling " << PrettyMethod(method_idx, dex_file) << "...";
  sea_ir::SeaGraph* sg = sea_ir::SeaGraph::GetCurrentGraph();
  sg->CompileMethod(code_item, class_def_idx, method_idx, dex_file);
  sg->DumpSea("/tmp/temp.dot");
  CHECK(0 && "No SEA compiled function exists yet.");
  return NULL;
}


CompiledMethod* SeaIrCompileOneMethod(CompilerDriver& compiler,
                                 const CompilerBackend backend,
                                 const DexFile::CodeItem* code_item,
                                 uint32_t access_flags,
                                 InvokeType invoke_type,
                                 uint32_t class_def_idx,
                                 uint32_t method_idx,
                                 jobject class_loader,
                                 const DexFile& dex_file,
                                 llvm::LlvmCompilationUnit* llvm_compilation_unit) {
  return CompileMethodWithSeaIr(compiler, backend, code_item, access_flags, invoke_type, class_def_idx,
                       method_idx, class_loader, dex_file
#if defined(ART_USE_PORTABLE_COMPILER)
                       , llvm_compilation_unit
#endif
                       ); // NOLINT
}

extern "C" art::CompiledMethod*
    SeaIrCompileMethod(art::CompilerDriver& compiler,
                          const art::DexFile::CodeItem* code_item,
                          uint32_t access_flags, art::InvokeType invoke_type,
                          uint32_t class_def_idx, uint32_t method_idx, jobject class_loader,
                          const art::DexFile& dex_file) {
  // TODO: check method fingerprint here to determine appropriate backend type.  Until then, use build default
  art::CompilerBackend backend = compiler.GetCompilerBackend();
  return art::SeaIrCompileOneMethod(compiler, backend, code_item, access_flags, invoke_type,
                               class_def_idx, method_idx, class_loader, dex_file,
                               NULL /* use thread llvm_info */);
}
#endif

} // end namespace art
