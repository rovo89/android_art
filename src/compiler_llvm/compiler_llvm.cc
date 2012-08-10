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

#include "compiler_llvm.h"

#include "backend_options.h"
#include "class_linker.h"
#include "compilation_unit.h"
#include "compiled_method.h"
#include "compiler.h"
#include "ir_builder.h"
#include "jni_compiler.h"
#ifndef ART_USE_DEXLANG_FRONTEND
# include "method_compiler.h"
#endif
#include "oat_compilation_unit.h"
#include "oat_file.h"
#include "stl_util.h"
#include "stub_compiler.h"
#include "utils_llvm.h"

#include <llvm/LinkAllPasses.h>
#include <llvm/LinkAllVMCore.h>
#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/Threading.h>

#if defined(ART_USE_QUICK_COMPILER)
namespace art {
void oatCompileMethodToGBC(Compiler& compiler,
                           const DexFile::CodeItem* code_item,
                           uint32_t access_flags, InvokeType invoke_type,
                           uint32_t method_idx, jobject class_loader,
                           const DexFile& dex_file,
                           llvm::Module* module,
                           llvm::LLVMContext* context,
                           greenland::IntrinsicHelper* intrinsic_helper,
                           greenland::IRBuilder* irb);
}
#endif

namespace llvm {
  extern bool TimePassesIsEnabled;
}

namespace {

pthread_once_t llvm_initialized = PTHREAD_ONCE_INIT;

void InitializeLLVM() {
  // Initialize LLVM internal data structure for multithreading
  llvm::llvm_start_multithreaded();

  // NOTE: Uncomment following line to show the time consumption of LLVM passes
  //llvm::TimePassesIsEnabled = true;

  // Initialize LLVM target-specific options.
  art::compiler_llvm::InitialBackendOptions();

  // Initialize LLVM target, MC subsystem, asm printer, and asm parser.
#if defined(ART_TARGET)
  // Don't initialize all targets on device. Just initialize the device's native target
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  llvm::InitializeNativeTargetAsmParser();
#else
  llvm::InitializeAllTargets();
  llvm::InitializeAllTargetMCs();
  llvm::InitializeAllAsmPrinters();
  llvm::InitializeAllAsmParsers();
#endif

  // Initialize LLVM optimization passes
  llvm::PassRegistry &registry = *llvm::PassRegistry::getPassRegistry();

  llvm::initializeCore(registry);
  llvm::initializeScalarOpts(registry);
  llvm::initializeIPO(registry);
  llvm::initializeAnalysis(registry);
  llvm::initializeIPA(registry);
  llvm::initializeTransformUtils(registry);
  llvm::initializeInstCombine(registry);
  llvm::initializeInstrumentation(registry);
  llvm::initializeTarget(registry);
}

// The Guard to Shutdown LLVM
// llvm::llvm_shutdown_obj llvm_guard;
// TODO: We are commenting out this line because this will cause SEGV from
// time to time.
// Two reasons: (1) the order of the destruction of static objects, or
//              (2) dlopen/dlclose side-effect on static objects.

} // anonymous namespace


namespace art {
namespace compiler_llvm {


llvm::Module* makeLLVMModuleContents(llvm::Module* module);


CompilerLLVM::CompilerLLVM(Compiler* compiler, InstructionSet insn_set)
    : compiler_(compiler), insn_set_(insn_set),
      num_cunits_lock_("compilation unit counter lock"), num_cunits_(0),
      plt_(insn_set) {

  // Initialize LLVM libraries
  pthread_once(&llvm_initialized, InitializeLLVM);
}


CompilerLLVM::~CompilerLLVM() {
}


CompilationUnit* CompilerLLVM::AllocateCompilationUnit() {
  MutexLock GUARD(num_cunits_lock_);
  CompilationUnit* cunit = new CompilationUnit(this, num_cunits_++);
  if (!bitcode_filename_.empty()) {
    cunit->SetBitcodeFileName(StringPrintf("%s-%zu", bitcode_filename_.c_str(), num_cunits_-1));
  }
  return cunit;
}


CompiledMethod* CompilerLLVM::
CompileDexMethod(OatCompilationUnit* oat_compilation_unit, InvokeType invoke_type) {
  UniquePtr<CompilationUnit> cunit(AllocateCompilationUnit());

#if defined(ART_USE_DEXLANG_FRONTEND)
  // Run DexLang for Dex to Greenland Bitcode
  UniquePtr<greenland::DexLang> dex_lang(
      new greenland::DexLang(*cunit->GetDexLangContext(), *compiler_,
                             *oat_compilation_unit));

  llvm::Function* func = dex_lang->Build();
  CHECK(func != NULL);

  cunit->Materialize();

  return new CompiledMethod(cunit->GetInstructionSet(),
                            cunit->GetCompiledCode());
#elif defined(ART_USE_QUICK_COMPILER)
  std::string methodName(PrettyMethod(oat_compilation_unit->GetDexMethodIndex(),
                                      *oat_compilation_unit->GetDexFile()));
  if ((methodName.find("gdata2.AndroidGDataClient.createAndExecuteMethod") != std::string::npos)
      || (methodName.find("hG.a") != std::string::npos)
      || (methodName.find("hT.a(hV, java.lang.String, java.lang.String, java") != std::string::npos)
      || (methodName.find("AndroidHttpTransport.exchange") != std::string::npos)
      || (methodName.find("javax.crypto.Cipher.getCipher") != std::string::npos)
      || (methodName.find("libcore.io.IoBridge.available") != std::string::npos)
      || (methodName.find("gov.nist.javax.sip.message.SIPMessage.computeContentLength") != std::string::npos)
      || (methodName.find("gov.nist.javax.sip.message.SIPMessage.encode") != std::string::npos)
      || (methodName.find("android.appwidget.AppWidgetHostView.updateAppWidget") != std::string::npos)
      || (methodName.find("android.content.SyncStorageEngine.readAccountInfoLocked") != std::string::npos)
      || (methodName.find("android.view.AccessibilityInteractionController.findAccessibilityNodeInfosByTextUiThread") != std::string::npos)
      || (methodName.find("android.webkit.BrowserFrame.downloadStart") != std::string::npos)
      || (methodName.find("com.android.internal.os.ZygoteConnection.handleChildProc") != std::string::npos)
      || (methodName.find("com.android.internal.widget.LockPatternUtils.passwordToHash") != std::string::npos)
      || (methodName.find("com.android.internal.os.ZygoteConnection.runOnce") != std::string::npos)
      || (methodName.find("com.google.android.gles_jni.GLImpl.allowIndirectBuffers") != std::string::npos)
      || (methodName.find("com.google.android.mms.util.DrmConvertSession.convert") != std::string::npos)
      || (methodName.find("com.android.server.BackupManagerService$PerformFullRestoreTask.readAppManifest") != std::string::npos)
      || (methodName.find("com.android.server.BackupManagerService.bindToAgentSynchronous") != std::string::npos)
      || (methodName.find("org.apache.xalan.templates.ElemElement.execute") != std::string::npos)
      || (methodName.find("com.android.server.am.ActivityManagerService.getPackageForIntentSender") != std::string::npos)
      || (methodName.find("com.android.server.am.ActivityManagerService.startNextMatchingActivity") != std::string::npos)
      || (methodName.find("com.android.server.wm.WindowManagerService.viewServerWindowCommand") != std::string::npos)
      || (methodName.find("org.apache.xml.serializer.OutputPropertiesFactory.getDefaultMethodProperties") != std::string::npos)
      // APK
      || (methodName.find("org.codehaus.jackson.map.ser.std.ObjectArraySerializer") != std::string::npos)
      || (methodName.find("com.android.providers.downloads.DrmConvertSession.convert") != std::string::npos)
      || (methodName.find("com.android.providers.downloads.DownloadThread.run") != std::string::npos)
      || (methodName.find("com.android.calendar.AllInOneActivity.parseViewAction") != std::string::npos)
      || (methodName.find("com.android.vcard.VCardEntryCommitter.pushIntoContentResolver") != std::string::npos)
      || (methodName.find("com.google.android.apps.books.model.VolumeMetadata.earliestPossibleSegmentIndexForPosition") != std::string::npos)
      || (methodName.find("com.google.android.auth.GoogleAuthSession.authenticate") != std::string::npos)
      || (methodName.find("com.google.android.syncadapters.bookmarks.BookmarksSyncAdapter") != std::string::npos)
      || (methodName.find("com.google.analytics.tracking.android.GoogleAnalytics.getTracker") != std::string::npos)
      || (methodName.find("com.google.apps") != std::string::npos)
      || (methodName.find("com.android.email.MessagingController.processPendingDeletesSynchronous") != std::string::npos)
      || (methodName.find("com.google.android.gm.provider.MailEngine.getHttpClient") != std::string::npos)
      || (methodName.find("com.google.android.gms.auth.login.GLSUser") != std::string::npos)
      || (methodName.find("com.google.android.backup.BackupTransportService$1.nextRestorePackage") != std::string::npos)
      || (methodName.find("com.google.earth.EarthActivity$1.run") != std::string::npos)
      || (methodName.find("com.google.android.gsf") != std::string::npos)
      || (methodName.find("com.google.googlenav.api.c.a") != std::string::npos)
      || (methodName.find("com.google.android.music.sync.api.MusicApiClientImpl") != std::string::npos)
      || (methodName.find("com.google.android.music.utils.MusicTagUtils.getGenreName") != std::string::npos)
      || (methodName.find("com.google.android.finsky.billing.challenge.ClientLoginApi$2.onErrorResponse") != std::string::npos)
      || (methodName.find("com.google.android.apps.plus") != std::string::npos)
      || (methodName.find("com.android.settings.ApnEditor") != std::string::npos)
      || (methodName.find("com.samsung.dmexthandler.PhoneDataService$1.getIPv6SettingsValue") != std::string::npos)
      || (methodName.find("com.samsung.syncservice.SyncmlService.getClientPwd") != std::string::npos)
      || (methodName.find("com.android.systemui.statusbar.BaseStatusBar.applyLegacyRowBackground") != std::string::npos)
      || (methodName.find("com.tf") != std::string::npos)
      || (methodName.find("com.google.android.searchcommon.util.JavaNetHttpHelper.extractCharset") != std::string::npos)
      || (methodName.find("com.google.android.youtube.core.utils.Util.getAppVersionCode") != std::string::npos)
      || (methodName.find("com.google.android.ytremote.backend.deviceauth.DeviceAuthenticator.encryptToken") != std::string::npos)
      || (methodName.find("com.google.android.maps.KeyHelper.getSignatureFingerprint") != std::string::npos)
      || (methodName.find("com.android.commands.content.Content$Command.execute") != std::string::npos)
      || (methodName.find("com.android.uiautomator.core.InteractionController.getSystemLongPressTime") != std::string::npos)
      ) {
    // Use iceland
    UniquePtr<MethodCompiler> method_compiler(
        new MethodCompiler(cunit.get(), compiler_, oat_compilation_unit));

    return method_compiler->Compile();
  } else {
    // Use quick
    llvm::LLVMContext* context = cunit->GetLLVMContext();
    llvm::Module* module = cunit->GetModule();
    greenland::IntrinsicHelper* intrinsic_helper = &cunit->GetDexLangContext()->GetIntrinsicHelper();
    UniquePtr<greenland::IRBuilder> greenland_irbuilder(
        new greenland::IRBuilder(*context, *module, *intrinsic_helper));
    oatCompileMethodToGBC(*compiler_,
                          oat_compilation_unit->GetCodeItem(),
                          oat_compilation_unit->access_flags_,
                          invoke_type,
                          oat_compilation_unit->GetDexMethodIndex(),
                          oat_compilation_unit->GetClassLoader(),
                          *oat_compilation_unit->GetDexFile(),
                          module,
                          context,
                          intrinsic_helper,
                          greenland_irbuilder.get()
                          );

    cunit->SetCompiler(compiler_);
    cunit->SetOatCompilationUnit(oat_compilation_unit);

    cunit->Materialize();

    return new CompiledMethod(cunit->GetInstructionSet(),
                              cunit->GetCompiledCode());
  }
#else
  UniquePtr<MethodCompiler> method_compiler(
      new MethodCompiler(cunit.get(), compiler_, oat_compilation_unit));

  return method_compiler->Compile();
#endif
}


CompiledMethod* CompilerLLVM::
CompileNativeMethod(OatCompilationUnit* oat_compilation_unit) {
  UniquePtr<CompilationUnit> cunit(AllocateCompilationUnit());

  UniquePtr<JniCompiler> jni_compiler(
      new JniCompiler(cunit.get(), *compiler_, oat_compilation_unit));

  return jni_compiler->Compile();
}


CompiledInvokeStub* CompilerLLVM::CreateInvokeStub(bool is_static,
                                                   char const *shorty) {
  UniquePtr<CompilationUnit> cunit(AllocateCompilationUnit());

  UniquePtr<StubCompiler> stub_compiler(
    new StubCompiler(cunit.get(), *compiler_));

  return stub_compiler->CreateInvokeStub(is_static, shorty);
}


CompiledInvokeStub* CompilerLLVM::CreateProxyStub(char const *shorty) {
  UniquePtr<CompilationUnit> cunit(AllocateCompilationUnit());

  UniquePtr<StubCompiler> stub_compiler(
    new StubCompiler(cunit.get(), *compiler_));

  return stub_compiler->CreateProxyStub(shorty);
}

} // namespace compiler_llvm
} // namespace art

inline static art::compiler_llvm::CompilerLLVM* ContextOf(art::Compiler& compiler) {
  void *compiler_context = compiler.GetCompilerContext();
  CHECK(compiler_context != NULL);
  return reinterpret_cast<art::compiler_llvm::CompilerLLVM*>(compiler_context);
}

inline static const art::compiler_llvm::CompilerLLVM* ContextOf(const art::Compiler& compiler) {
  void *compiler_context = compiler.GetCompilerContext();
  CHECK(compiler_context != NULL);
  return reinterpret_cast<const art::compiler_llvm::CompilerLLVM*>(compiler_context);
}

extern "C" void ArtInitCompilerContext(art::Compiler& compiler) {
  CHECK(compiler.GetCompilerContext() == NULL);

  art::compiler_llvm::CompilerLLVM* compiler_llvm =
      new art::compiler_llvm::CompilerLLVM(&compiler,
                                           compiler.GetInstructionSet());

  compiler.SetCompilerContext(compiler_llvm);
}

extern "C" void ArtUnInitCompilerContext(art::Compiler& compiler) {
  delete ContextOf(compiler);
  compiler.SetCompilerContext(NULL);
}
extern "C" art::CompiledMethod* ArtCompileMethod(art::Compiler& compiler,
                                                 const art::DexFile::CodeItem* code_item,
                                                 uint32_t access_flags,
                                                 art::InvokeType invoke_type,
                                                 uint32_t method_idx,
                                                 jobject class_loader,
                                                 const art::DexFile& dex_file) {
  art::ClassLinker *class_linker = art::Runtime::Current()->GetClassLinker();

  art::OatCompilationUnit oat_compilation_unit(
    class_loader, class_linker, dex_file, code_item,
    method_idx, access_flags);
  art::compiler_llvm::CompilerLLVM* compiler_llvm = ContextOf(compiler);
  art::CompiledMethod* result = compiler_llvm->CompileDexMethod(&oat_compilation_unit, invoke_type);
  return result;
}

extern "C" art::CompiledMethod* ArtJniCompileMethod(art::Compiler& compiler,
                                                    uint32_t access_flags, uint32_t method_idx,
                                                    const art::DexFile& dex_file) {
  art::ClassLinker *class_linker = art::Runtime::Current()->GetClassLinker();

  art::OatCompilationUnit oat_compilation_unit(
    NULL, class_linker, dex_file, NULL,
    method_idx, access_flags);

  art::compiler_llvm::CompilerLLVM* compiler_llvm = ContextOf(compiler);
  art::CompiledMethod* result = compiler_llvm->CompileNativeMethod(&oat_compilation_unit);
  return result;
}

extern "C" art::CompiledInvokeStub* ArtCreateInvokeStub(art::Compiler& compiler,
                                                        bool is_static,
                                                        const char* shorty,
                                                        uint32_t shorty_len) {
  art::compiler_llvm::CompilerLLVM* compiler_llvm = ContextOf(compiler);
  art::CompiledInvokeStub* result = compiler_llvm->CreateInvokeStub(is_static, shorty);
  return result;
}

extern "C" art::CompiledInvokeStub* ArtCreateProxyStub(art::Compiler& compiler,
                                                       const char* shorty,
                                                       uint32_t shorty_len) {
  art::compiler_llvm::CompilerLLVM* compiler_llvm = ContextOf(compiler);
  art::CompiledInvokeStub* result = compiler_llvm->CreateProxyStub(shorty);
  return result;
}

extern "C" void compilerLLVMSetBitcodeFileName(art::Compiler& compiler,
                                               std::string const& filename) {
  ContextOf(compiler)->SetBitcodeFileName(filename);
}
