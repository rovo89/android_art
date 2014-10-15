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

#include "class_linker.h"
#include "compiled_method.h"
#include "dex/quick_compiler_callbacks.h"
#include "dex/verification_results.h"
#include "dex/quick/dex_file_to_method_inliner_map.h"
#include "driver/compiler_driver.h"
#include "interpreter/interpreter.h"
#include "mirror/art_method.h"
#include "mirror/dex_cache.h"
#include "mirror/object-inl.h"
#include "scoped_thread_state_change.h"
#include "thread-inl.h"
#include "utils.h"

namespace art {

CommonCompilerTest::CommonCompilerTest() {}
CommonCompilerTest::~CommonCompilerTest() {}

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
    bool is_portable = (code == nullptr);
    if (!is_portable) {
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
    class_linker_->SetEntryPointsToCompiledCode(method, method_code, is_portable);
  } else {
    // No code? You must mean to go into the interpreter.
    // Or the generic JNI...
    class_linker_->SetEntryPointsToInterpreter(method);
  }
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

    const InstructionSet instruction_set = kRuntimeISA;
    // Take the default set of instruction features from the build.
    instruction_set_features_.reset(InstructionSetFeatures::FromCppDefines());

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
                                              instruction_set_features_.get(),
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

void CommonCompilerTest::CompileVirtualMethod(Handle<mirror::ClassLoader> class_loader,
                                              const char* class_name, const char* method_name,
                                              const char* signature) {
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
  MemMap::Init();
  image_reservation_.reset(MemMap::MapAnonymous("image reservation",
                                                reinterpret_cast<uint8_t*>(ART_BASE_ADDRESS),
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
