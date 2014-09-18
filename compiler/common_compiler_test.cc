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

#include "common_compiler_test.h"

#if defined(__arm__)
#include <sys/ucontext.h>
#endif
#include <fstream>

#include "class_linker.h"
#include "compiled_method.h"
#include "dex/quick_compiler_callbacks.h"
#include "dex/verification_results.h"
#include "dex/quick/dex_file_to_method_inliner_map.h"
#include "driver/compiler_driver.h"
#include "entrypoints/entrypoint_utils.h"
#include "interpreter/interpreter.h"
#include "mirror/art_method.h"
#include "mirror/dex_cache.h"
#include "mirror/object-inl.h"
#include "scoped_thread_state_change.h"
#include "thread-inl.h"
#include "utils.h"

namespace art {

// Normally the ClassLinker supplies this.
extern "C" void art_quick_generic_jni_trampoline(mirror::ArtMethod*);

#if defined(__arm__)
// A signal handler called when have an illegal instruction.  We record the fact in
// a global boolean and then increment the PC in the signal context to return to
// the next instruction.  We know the instruction is an sdiv (4 bytes long).
static void baddivideinst(int signo, siginfo *si, void *data) {
  UNUSED(signo);
  UNUSED(si);
  struct ucontext *uc = (struct ucontext *)data;
  struct sigcontext *sc = &uc->uc_mcontext;
  sc->arm_r0 = 0;     // set R0 to #0 to signal error
  sc->arm_pc += 4;    // skip offending instruction
}

// This is in arch/arm/arm_sdiv.S.  It does the following:
// mov r1,#1
// sdiv r0,r1,r1
// bx lr
//
// the result will be the value 1 if sdiv is supported.  If it is not supported
// a SIGILL signal will be raised and the signal handler (baddivideinst) called.
// The signal handler sets r0 to #0 and then increments pc beyond the failed instruction.
// Thus if the instruction is not supported, the result of this function will be #0

extern "C" bool CheckForARMSDIVInstruction();

static InstructionSetFeatures GuessInstructionFeatures() {
  InstructionSetFeatures f;

  // Uncomment this for processing of /proc/cpuinfo.
  if (false) {
    // Look in /proc/cpuinfo for features we need.  Only use this when we can guarantee that
    // the kernel puts the appropriate feature flags in here.  Sometimes it doesn't.
    std::ifstream in("/proc/cpuinfo");
    if (in) {
      while (!in.eof()) {
        std::string line;
        std::getline(in, line);
        if (!in.eof()) {
          if (line.find("Features") != std::string::npos) {
            if (line.find("idivt") != std::string::npos) {
              f.SetHasDivideInstruction(true);
            }
          }
        }
        in.close();
      }
    } else {
      LOG(INFO) << "Failed to open /proc/cpuinfo";
    }
  }

  // See if have a sdiv instruction.  Register a signal handler and try to execute
  // an sdiv instruction.  If we get a SIGILL then it's not supported.  We can't use
  // the /proc/cpuinfo method for this because Krait devices don't always put the idivt
  // feature in the list.
  struct sigaction sa, osa;
  sa.sa_flags = SA_ONSTACK | SA_RESTART | SA_SIGINFO;
  sa.sa_sigaction = baddivideinst;
  sigaction(SIGILL, &sa, &osa);

  if (CheckForARMSDIVInstruction()) {
    f.SetHasDivideInstruction(true);
  }

  // Restore the signal handler.
  sigaction(SIGILL, &osa, nullptr);

  // Other feature guesses in here.
  return f;
}
#endif

// Given a set of instruction features from the build, parse it.  The
// input 'str' is a comma separated list of feature names.  Parse it and
// return the InstructionSetFeatures object.
static InstructionSetFeatures ParseFeatureList(std::string str) {
  InstructionSetFeatures result;
  typedef std::vector<std::string> FeatureList;
  FeatureList features;
  Split(str, ',', features);
  for (FeatureList::iterator i = features.begin(); i != features.end(); i++) {
    std::string feature = Trim(*i);
    if (feature == "default") {
      // Nothing to do.
    } else if (feature == "div") {
      // Supports divide instruction.
      result.SetHasDivideInstruction(true);
    } else if (feature == "nodiv") {
      // Turn off support for divide instruction.
      result.SetHasDivideInstruction(false);
    } else {
      LOG(FATAL) << "Unknown instruction set feature: '" << feature << "'";
    }
  }
  // Others...
  return result;
}

CommonCompilerTest::CommonCompilerTest() {}
CommonCompilerTest::~CommonCompilerTest() {}

OatFile::OatMethod CommonCompilerTest::CreateOatMethod(const void* code, const uint8_t* gc_map) {
  CHECK(code != nullptr);
  const byte* base;
  uint32_t code_offset, gc_map_offset;
  if (gc_map == nullptr) {
    base = reinterpret_cast<const byte*>(code);  // Base of data points at code.
    base -= kPointerSize;  // Move backward so that code_offset != 0.
    code_offset = kPointerSize;
    gc_map_offset = 0;
  } else {
    // TODO: 64bit support.
    base = nullptr;  // Base of data in oat file, ie 0.
    code_offset = PointerToLowMemUInt32(code);
    gc_map_offset = PointerToLowMemUInt32(gc_map);
  }
  return OatFile::OatMethod(base, code_offset, gc_map_offset);
}

void CommonCompilerTest::MakeExecutable(mirror::ArtMethod* method) {
  CHECK(method != nullptr);

  const CompiledMethod* compiled_method = nullptr;
  if (!method->IsAbstract()) {
    mirror::DexCache* dex_cache = method->GetDeclaringClass()->GetDexCache();
    const DexFile& dex_file = *dex_cache->GetDexFile();
    compiled_method =
        compiler_driver_->GetCompiledMethod(MethodReference(&dex_file,
                                                            method->GetDexMethodIndex()));
  }
  if (compiled_method != nullptr) {
    const std::vector<uint8_t>* code = compiled_method->GetQuickCode();
    const void* code_ptr;
    if (code != nullptr) {
      uint32_t code_size = code->size();
      CHECK_NE(0u, code_size);
      const std::vector<uint8_t>& vmap_table = compiled_method->GetVmapTable();
      uint32_t vmap_table_offset = vmap_table.empty() ? 0u
          : sizeof(OatQuickMethodHeader) + vmap_table.size();
      const std::vector<uint8_t>& mapping_table = compiled_method->GetMappingTable();
      uint32_t mapping_table_offset = mapping_table.empty() ? 0u
          : sizeof(OatQuickMethodHeader) + vmap_table.size() + mapping_table.size();
      OatQuickMethodHeader method_header(mapping_table_offset, vmap_table_offset,
                                         compiled_method->GetFrameSizeInBytes(),
                                         compiled_method->GetCoreSpillMask(),
                                         compiled_method->GetFpSpillMask(), code_size);

      header_code_and_maps_chunks_.push_back(std::vector<uint8_t>());
      std::vector<uint8_t>* chunk = &header_code_and_maps_chunks_.back();
      size_t size = sizeof(method_header) + code_size + vmap_table.size() + mapping_table.size();
      size_t code_offset = compiled_method->AlignCode(size - code_size);
      size_t padding = code_offset - (size - code_size);
      chunk->reserve(padding + size);
      chunk->resize(sizeof(method_header));
      memcpy(&(*chunk)[0], &method_header, sizeof(method_header));
      chunk->insert(chunk->begin(), vmap_table.begin(), vmap_table.end());
      chunk->insert(chunk->begin(), mapping_table.begin(), mapping_table.end());
      chunk->insert(chunk->begin(), padding, 0);
      chunk->insert(chunk->end(), code->begin(), code->end());
      CHECK_EQ(padding + size, chunk->size());
      code_ptr = &(*chunk)[code_offset];
    } else {
      code = compiled_method->GetPortableCode();
      code_ptr = &(*code)[0];
    }
    MakeExecutable(code_ptr, code->size());
    const void* method_code = CompiledMethod::CodePointer(code_ptr,
                                                          compiled_method->GetInstructionSet());
    LOG(INFO) << "MakeExecutable " << PrettyMethod(method) << " code=" << method_code;
    OatFile::OatMethod oat_method = CreateOatMethod(method_code, nullptr);
    oat_method.LinkMethod(method);
    method->SetEntryPointFromInterpreter(artInterpreterToCompiledCodeBridge);
  } else {
    // No code? You must mean to go into the interpreter.
    // Or the generic JNI...
    if (!method->IsNative()) {
#if defined(ART_USE_PORTABLE_COMPILER)
      const void* method_code = GetPortableToInterpreterBridge();
#else
      const void* method_code = GetQuickToInterpreterBridge();
#endif
      OatFile::OatMethod oat_method = CreateOatMethod(method_code, nullptr);
      oat_method.LinkMethod(method);
      method->SetEntryPointFromInterpreter(interpreter::artInterpreterToInterpreterBridge);
    } else {
      const void* method_code = reinterpret_cast<void*>(art_quick_generic_jni_trampoline);

      OatFile::OatMethod oat_method = CreateOatMethod(method_code, nullptr);
      oat_method.LinkMethod(method);
      method->SetEntryPointFromInterpreter(artInterpreterToCompiledCodeBridge);
    }
  }
  // Create bridges to transition between different kinds of compiled bridge.
#if defined(ART_USE_PORTABLE_COMPILER)
  if (method->GetEntryPointFromPortableCompiledCode() == nullptr) {
    method->SetEntryPointFromPortableCompiledCode(GetPortableToQuickBridge());
  } else {
    CHECK(method->GetEntryPointFromQuickCompiledCode() == nullptr);
    method->SetEntryPointFromQuickCompiledCode(GetQuickToPortableBridge());
    method->SetIsPortableCompiled();
  }
#else
  CHECK(method->GetEntryPointFromQuickCompiledCode() != nullptr);
#endif
}

void CommonCompilerTest::MakeExecutable(const void* code_start, size_t code_length) {
  CHECK(code_start != nullptr);
  CHECK_NE(code_length, 0U);
  uintptr_t data = reinterpret_cast<uintptr_t>(code_start);
  uintptr_t base = RoundDown(data, kPageSize);
  uintptr_t limit = RoundUp(data + code_length, kPageSize);
  uintptr_t len = limit - base;
  int result = mprotect(reinterpret_cast<void*>(base), len, PROT_READ | PROT_WRITE | PROT_EXEC);
  CHECK_EQ(result, 0);

  // Flush instruction cache
  // Only uses __builtin___clear_cache if GCC >= 4.3.3
#if GCC_VERSION >= 40303
  __builtin___clear_cache(reinterpret_cast<void*>(base), reinterpret_cast<void*>(base + len));
#else
  // Only warn if not Intel as Intel doesn't have cache flush instructions.
#if !defined(__i386__) && !defined(__x86_64__)
  LOG(WARNING) << "UNIMPLEMENTED: cache flush";
#endif
#endif
}

void CommonCompilerTest::MakeExecutable(mirror::ClassLoader* class_loader, const char* class_name) {
  std::string class_descriptor(DotToDescriptor(class_name));
  Thread* self = Thread::Current();
  StackHandleScope<1> hs(self);
  Handle<mirror::ClassLoader> loader(hs.NewHandle(class_loader));
  mirror::Class* klass = class_linker_->FindClass(self, class_descriptor.c_str(), loader);
  CHECK(klass != nullptr) << "Class not found " << class_name;
  for (size_t i = 0; i < klass->NumDirectMethods(); i++) {
    MakeExecutable(klass->GetDirectMethod(i));
  }
  for (size_t i = 0; i < klass->NumVirtualMethods(); i++) {
    MakeExecutable(klass->GetVirtualMethod(i));
  }
}

void CommonCompilerTest::SetUp() {
  CommonRuntimeTest::SetUp();
  {
    ScopedObjectAccess soa(Thread::Current());

    InstructionSet instruction_set = kRuntimeISA;

    // Take the default set of instruction features from the build.
    InstructionSetFeatures instruction_set_features =
        ParseFeatureList(Runtime::GetDefaultInstructionSetFeatures());

#if defined(__arm__)
    InstructionSetFeatures runtime_features = GuessInstructionFeatures();

    // for ARM, do a runtime check to make sure that the features we are passed from
    // the build match the features we actually determine at runtime.
    ASSERT_LE(instruction_set_features, runtime_features);
#endif

    runtime_->SetInstructionSet(instruction_set);
    for (int i = 0; i < Runtime::kLastCalleeSaveType; i++) {
      Runtime::CalleeSaveType type = Runtime::CalleeSaveType(i);
      if (!runtime_->HasCalleeSaveMethod(type)) {
        runtime_->SetCalleeSaveMethod(
            runtime_->CreateCalleeSaveMethod(type), type);
      }
    }

    // TODO: make selectable
    Compiler::Kind compiler_kind
    = (kUsePortableCompiler) ? Compiler::kPortable : Compiler::kQuick;
    timer_.reset(new CumulativeLogger("Compilation times"));
    compiler_driver_.reset(new CompilerDriver(compiler_options_.get(),
                                              verification_results_.get(),
                                              method_inliner_map_.get(),
                                              compiler_kind, instruction_set,
                                              instruction_set_features,
                                              true, new std::set<std::string>,
                                              2, true, true, timer_.get()));
  }
  // We typically don't generate an image in unit tests, disable this optimization by default.
  compiler_driver_->SetSupportBootImageFixup(false);
}

void CommonCompilerTest::SetUpRuntimeOptions(RuntimeOptions* options) {
  CommonRuntimeTest::SetUpRuntimeOptions(options);

  compiler_options_.reset(new CompilerOptions);
  verification_results_.reset(new VerificationResults(compiler_options_.get()));
  method_inliner_map_.reset(new DexFileToMethodInlinerMap);
  callbacks_.reset(new QuickCompilerCallbacks(verification_results_.get(),
                                              method_inliner_map_.get()));
  options->push_back(std::make_pair("compilercallbacks", callbacks_.get()));
}

void CommonCompilerTest::TearDown() {
  timer_.reset();
  compiler_driver_.reset();
  callbacks_.reset();
  method_inliner_map_.reset();
  verification_results_.reset();
  compiler_options_.reset();

  CommonRuntimeTest::TearDown();
}

void CommonCompilerTest::CompileClass(mirror::ClassLoader* class_loader, const char* class_name) {
  std::string class_descriptor(DotToDescriptor(class_name));
  Thread* self = Thread::Current();
  StackHandleScope<1> hs(self);
  Handle<mirror::ClassLoader> loader(hs.NewHandle(class_loader));
  mirror::Class* klass = class_linker_->FindClass(self, class_descriptor.c_str(), loader);
  CHECK(klass != nullptr) << "Class not found " << class_name;
  for (size_t i = 0; i < klass->NumDirectMethods(); i++) {
    CompileMethod(klass->GetDirectMethod(i));
  }
  for (size_t i = 0; i < klass->NumVirtualMethods(); i++) {
    CompileMethod(klass->GetVirtualMethod(i));
  }
}

void CommonCompilerTest::CompileMethod(mirror::ArtMethod* method) {
  CHECK(method != nullptr);
  TimingLogger timings("CommonTest::CompileMethod", false, false);
  TimingLogger::ScopedTiming t(__FUNCTION__, &timings);
  compiler_driver_->CompileOne(method, &timings);
  TimingLogger::ScopedTiming t2("MakeExecutable", &timings);
  MakeExecutable(method);
}

void CommonCompilerTest::CompileDirectMethod(Handle<mirror::ClassLoader> class_loader,
                                             const char* class_name, const char* method_name,
                                             const char* signature) {
  std::string class_descriptor(DotToDescriptor(class_name));
  Thread* self = Thread::Current();
  mirror::Class* klass = class_linker_->FindClass(self, class_descriptor.c_str(), class_loader);
  CHECK(klass != nullptr) << "Class not found " << class_name;
  mirror::ArtMethod* method = klass->FindDirectMethod(method_name, signature);
  CHECK(method != nullptr) << "Direct method not found: "
      << class_name << "." << method_name << signature;
  CompileMethod(method);
}

void CommonCompilerTest::CompileVirtualMethod(Handle<mirror::ClassLoader> class_loader, const char* class_name,
                                              const char* method_name, const char* signature)
SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  std::string class_descriptor(DotToDescriptor(class_name));
  Thread* self = Thread::Current();
  mirror::Class* klass = class_linker_->FindClass(self, class_descriptor.c_str(), class_loader);
  CHECK(klass != nullptr) << "Class not found " << class_name;
  mirror::ArtMethod* method = klass->FindVirtualMethod(method_name, signature);
  CHECK(method != NULL) << "Virtual method not found: "
      << class_name << "." << method_name << signature;
  CompileMethod(method);
}

void CommonCompilerTest::ReserveImageSpace() {
  // Reserve where the image will be loaded up front so that other parts of test set up don't
  // accidentally end up colliding with the fixed memory address when we need to load the image.
  std::string error_msg;
  image_reservation_.reset(MemMap::MapAnonymous("image reservation",
                                                reinterpret_cast<byte*>(ART_BASE_ADDRESS),
                                                (size_t)100 * 1024 * 1024,  // 100MB
                                                PROT_NONE,
                                                false /* no need for 4gb flag with fixed mmap*/,
                                                &error_msg));
  CHECK(image_reservation_.get() != nullptr) << error_msg;
}

void CommonCompilerTest::UnreserveImageSpace() {
  image_reservation_.reset();
}

}  // namespace art
