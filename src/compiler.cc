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

#include "compiler.h"

#include <vector>

#include <sys/mman.h>
#include <unistd.h>

#include "assembler.h"
#include "class_linker.h"
#include "class_loader.h"
#include "dex_cache.h"
#include "dex_verifier.h"
#include "jni_compiler.h"
#include "jni_internal.h"
#include "oat_file.h"
#include "object_utils.h"
#include "runtime.h"
#include "stl_util.h"
#include "timing_logger.h"

namespace art {

CompiledMethod* oatCompileMethod(const Compiler& compiler, const DexFile::CodeItem* code_item,
                                 uint32_t access_flags, uint32_t method_idx,
                                 const ClassLoader* class_loader,
                                 const DexFile& dex_file, InstructionSet);

namespace arm {
  ByteArray* CreateAbstractMethodErrorStub();
  CompiledInvokeStub* ArmCreateInvokeStub(bool is_static, const char* shorty);
  ByteArray* ArmCreateResolutionTrampoline(Runtime::TrampolineType type);
  ByteArray* CreateJniDlsymLookupStub();
}
namespace x86 {
  ByteArray* CreateAbstractMethodErrorStub();
  CompiledInvokeStub* X86CreateInvokeStub(bool is_static, const char* shorty);
  ByteArray* X86CreateResolutionTrampoline(Runtime::TrampolineType type);
  ByteArray* CreateJniDlsymLookupStub();
}

Compiler::Compiler(InstructionSet instruction_set,
                   bool image,
                   const std::set<std::string>* image_classes)
    : instruction_set_(instruction_set),
      jni_compiler_(instruction_set),
      compiled_classes_lock_("compiled classes lock"),
      compiled_methods_lock_("compiled method lock"),
      compiled_invoke_stubs_lock_("compiled invoke stubs lock"),
      image_(image),
      image_classes_(image_classes) {
  CHECK(!Runtime::Current()->IsStarted());
  if (!image_) {
    CHECK(image_classes_ == NULL);
  }
}

Compiler::~Compiler() {
  {
    MutexLock mu(compiled_classes_lock_);
    STLDeleteValues(&compiled_classes_);
  }
  {
    MutexLock mu(compiled_methods_lock_);
    STLDeleteValues(&compiled_methods_);
  }
  {
    MutexLock mu(compiled_invoke_stubs_lock_);
    STLDeleteValues(&compiled_invoke_stubs_);
  }
}

ByteArray* Compiler::CreateResolutionStub(InstructionSet instruction_set,
                                          Runtime::TrampolineType type) {
  if (instruction_set == kX86) {
    return x86::X86CreateResolutionTrampoline(type);
  } else {
    CHECK(instruction_set == kArm || instruction_set == kThumb2);
    // Generates resolution stub using ARM instruction set
    return arm::ArmCreateResolutionTrampoline(type);
  }
}

ByteArray* Compiler::CreateJniDlsymLookupStub(InstructionSet instruction_set) {
  switch (instruction_set) {
    case kArm:
    case kThumb2:
      return arm::CreateJniDlsymLookupStub();
    case kX86:
      return x86::CreateJniDlsymLookupStub();
    default:
      LOG(FATAL) << "Unknown InstructionSet: " << static_cast<int>(instruction_set);
      return NULL;
  }
}

ByteArray* Compiler::CreateAbstractMethodErrorStub(InstructionSet instruction_set) {
  if (instruction_set == kX86) {
    return x86::CreateAbstractMethodErrorStub();
  } else {
    CHECK(instruction_set == kArm || instruction_set == kThumb2);
    // Generates resolution stub using ARM instruction set
    return arm::CreateAbstractMethodErrorStub();
  }
}

void Compiler::CompileAll(const ClassLoader* class_loader,
                          const std::vector<const DexFile*>& dex_files) {
  DCHECK(!Runtime::Current()->IsStarted());

  TimingLogger timings("compiler");

  PreCompile(class_loader, dex_files, timings);

  Compile(class_loader, dex_files);
  timings.AddSplit("Compile");

  PostCompile(class_loader, dex_files);
  timings.AddSplit("PostCompile");

  if (timings.GetTotalNs() > MsToNs(1000)) {
    timings.Dump();
  }
}

void Compiler::CompileOne(const Method* method) {
  DCHECK(!Runtime::Current()->IsStarted());

  const ClassLoader* class_loader = method->GetDeclaringClass()->GetClassLoader();

  // Find the dex_file
  const DexCache* dex_cache = method->GetDeclaringClass()->GetDexCache();
  const DexFile& dex_file = Runtime::Current()->GetClassLinker()->FindDexFile(dex_cache);
  std::vector<const DexFile*> dex_files;
  dex_files.push_back(&dex_file);

  TimingLogger timings("CompileOne");
  PreCompile(class_loader, dex_files, timings);

  uint32_t method_idx = method->GetDexMethodIndex();
  const DexFile::CodeItem* code_item = dex_file.GetCodeItem(method->GetCodeItemOffset());
  CompileMethod(code_item, method->GetAccessFlags(), method_idx, class_loader, dex_file);

  PostCompile(class_loader, dex_files);
}

void Compiler::Resolve(const ClassLoader* class_loader,
                       const std::vector<const DexFile*>& dex_files, TimingLogger& timings) {
  for (size_t i = 0; i != dex_files.size(); ++i) {
    const DexFile* dex_file = dex_files[i];
    CHECK(dex_file != NULL);
    ResolveDexFile(class_loader, *dex_file, timings);
  }
}

void Compiler::PreCompile(const ClassLoader* class_loader,
                          const std::vector<const DexFile*>& dex_files, TimingLogger& timings) {
  Resolve(class_loader, dex_files, timings);

  Verify(class_loader, dex_files);
  timings.AddSplit("PreCompile.Verify");

  InitializeClassesWithoutClinit(class_loader, dex_files);
  timings.AddSplit("PreCompile.InitializeClassesWithoutClinit");
}

void Compiler::PostCompile(const ClassLoader* class_loader,
                           const std::vector<const DexFile*>& dex_files) {
  SetGcMaps(class_loader, dex_files);
  SetCodeAndDirectMethods(dex_files);
}

bool Compiler::IsImageClass(const std::string& descriptor) const {
  if (image_classes_ == NULL) {
    return true;
  }
  return image_classes_->find(descriptor) != image_classes_->end();
}

bool Compiler::CanAssumeTypeIsPresentInDexCache(const DexCache* dex_cache,
                                                uint32_t type_idx) const {
  if (!IsImage()) {
    return false;
  }
  Class* resolved_class = dex_cache->GetResolvedTypes()->Get(type_idx);
  if (resolved_class == NULL) {
    return false;
  }
  return IsImageClass(ClassHelper(resolved_class).GetDescriptor());
}

// Return true if the class should be skipped during compilation. We
// never skip classes in the boot class loader. However, if we have a
// non-boot class loader and we can resolve the class in the boot
// class loader, we do skip the class. This happens if an app bundles
// classes found in the boot classpath. Since at runtime we will
// select the class from the boot classpath, do not attempt to resolve
// or compile it now.
static bool SkipClass(const ClassLoader* class_loader,
                      const DexFile& dex_file,
                      const DexFile::ClassDef& class_def) {
  if (class_loader == NULL) {
    return false;
  }
  const char* descriptor = dex_file.GetClassDescriptor(class_def);
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  Class* klass = class_linker->FindClass(descriptor, NULL);
  if (klass == NULL) {
    Thread* self = Thread::Current();
    CHECK(self->IsExceptionPending());
    self->ClearException();
    return false;
  }
  return true;
}

struct Context {
  ClassLinker* class_linker;
  const ClassLoader* class_loader;
  Compiler* compiler;
  DexCache* dex_cache;
  const DexFile* dex_file;
};

typedef void Callback(Context* context, size_t index);

class WorkerThread {
 public:
  WorkerThread(Context* context, size_t begin, size_t end, Callback callback, size_t stripe)
      : context_(context), begin_(begin), end_(end), callback_(callback), stripe_(stripe) {
    CHECK_PTHREAD_CALL(pthread_create, (&pthread_, NULL, &Init, this), "compiler worker thread");
  }

  ~WorkerThread() {
    CHECK_PTHREAD_CALL(pthread_join, (pthread_, NULL), "compiler worker shutdown");
  }

 private:
  static void* Init(void* arg) {
    WorkerThread* worker = reinterpret_cast<WorkerThread*>(arg);
    Runtime* runtime = Runtime::Current();
    runtime->AttachCurrentThread("Compiler Worker", true);
    Thread::Current()->SetState(Thread::kRunnable);
    worker->Run();
    Thread::Current()->SetState(Thread::kNative);
    runtime->DetachCurrentThread();
    return NULL;
  }

  void Run() {
    for (size_t i = begin_; i < end_; i += stripe_) {
      callback_(context_, i);
    }
  }

  pthread_t pthread_;

  Context* context_;
  size_t begin_;
  size_t end_;
  Callback* callback_;
  size_t stripe_;
};

void ForAll(Context* context, size_t begin, size_t end, Callback callback) {
  std::vector<WorkerThread*> threads;

  const size_t thread_count = static_cast<size_t>(sysconf(_SC_NPROCESSORS_ONLN));
  for (size_t i = 0; i < thread_count; ++i) {
    threads.push_back(new WorkerThread(context, begin + i, end, callback, thread_count));
  }

  // Switch to kVmWait while we're blocked waiting for the other threads to finish.
  ScopedThreadStateChange tsc(Thread::Current(), Thread::kVmWait);
  STLDeleteElements(&threads);
}

static void ResolveClassFieldsAndMethods(Context* context, size_t class_def_index) {
  const DexFile& dex_file = *context->dex_file;

  // Method and Field are the worst. We can't resolve without either
  // context from the code use (to disambiguate virtual vs direct
  // method and instance vs static field) or from class
  // definitions. While the compiler will resolve what it can as it
  // needs it, here we try to resolve fields and methods used in class
  // definitions, since many of them many never be referenced by
  // generated code.
  const DexFile::ClassDef& class_def = dex_file.GetClassDef(class_def_index);
  if (SkipClass(context->class_loader, dex_file, class_def)) {
    return;
  }

  // Note the class_data pointer advances through the headers,
  // static fields, instance fields, direct methods, and virtual
  // methods.
  const byte* class_data = dex_file.GetClassData(class_def);
  if (class_data == NULL) {
    // empty class such as a marker interface
    return;
  }
  Thread* self = Thread::Current();
  ClassLinker* class_linker = context->class_linker;
  DexCache* dex_cache = class_linker->FindDexCache(dex_file);
  ClassDataItemIterator it(dex_file, class_data);
  while (it.HasNextStaticField()) {
    Field* field = class_linker->ResolveField(dex_file, it.GetMemberIndex(), dex_cache,
                                              context->class_loader, true);
    if (field == NULL) {
      CHECK(self->IsExceptionPending());
      self->ClearException();
    }
    it.Next();
  }
  while (it.HasNextInstanceField()) {
    Field* field = class_linker->ResolveField(dex_file, it.GetMemberIndex(), dex_cache,
                                              context->class_loader, false);
    if (field == NULL) {
      CHECK(self->IsExceptionPending());
      self->ClearException();
    }
    it.Next();
  }
  while (it.HasNextDirectMethod()) {
    Method* method = class_linker->ResolveMethod(dex_file, it.GetMemberIndex(), dex_cache,
                                                 context->class_loader, true);
    if (method == NULL) {
      CHECK(self->IsExceptionPending());
      self->ClearException();
    }
    it.Next();
  }
  while (it.HasNextVirtualMethod()) {
    Method* method = class_linker->ResolveMethod(dex_file, it.GetMemberIndex(), dex_cache,
                                                 context->class_loader, false);
    if (method == NULL) {
      CHECK(self->IsExceptionPending());
      self->ClearException();
    }
    it.Next();
  }
  DCHECK(!it.HasNext());
}

static void ResolveType(Context* context, size_t type_idx) {
  // Class derived values are more complicated, they require the linker and loader.
  Thread* self = Thread::Current();
  ClassLinker* class_linker = context->class_linker;
  const DexFile& dex_file = *context->dex_file;
  Class* klass = class_linker->ResolveType(dex_file, type_idx, context->dex_cache, context->class_loader);
  if (klass == NULL) {
    CHECK(self->IsExceptionPending());
    Thread::Current()->ClearException();
  }
}

void Compiler::ResolveDexFile(const ClassLoader* class_loader, const DexFile& dex_file, TimingLogger& timings) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  DexCache* dex_cache = class_linker->FindDexCache(dex_file);

  // Strings are easy in that they always are simply resolved to literals in the same file
  if (image_ && image_classes_ == NULL) {
    // TODO: Add support for loading strings referenced by image_classes_
    // See also Compiler::CanAssumeTypeIsPresentInDexCache.
    for (size_t string_idx = 0; string_idx < dex_cache->NumStrings(); string_idx++) {
      class_linker->ResolveString(dex_file, string_idx, dex_cache);
    }
    timings.AddSplit("Resolve " + dex_file.GetLocation() + " Strings");
  }

  Context context;
  context.class_linker = class_linker;
  context.class_loader = class_loader;
  context.dex_cache = dex_cache;
  context.dex_file = &dex_file;

  ForAll(&context, 0, dex_cache->NumResolvedTypes(), ResolveType);
  timings.AddSplit("Resolve " + dex_file.GetLocation() + " Types");

  ForAll(&context, 0, dex_file.NumClassDefs(), ResolveClassFieldsAndMethods);
  timings.AddSplit("Resolve " + dex_file.GetLocation() + " MethodsAndFields");
}

void Compiler::Verify(const ClassLoader* class_loader,
                      const std::vector<const DexFile*>& dex_files) {
  for (size_t i = 0; i != dex_files.size(); ++i) {
    const DexFile* dex_file = dex_files[i];
    CHECK(dex_file != NULL);
    VerifyDexFile(class_loader, *dex_file);
  }
}

static void VerifyClass(Context* context, size_t class_def_index) {
  const DexFile::ClassDef& class_def = context->dex_file->GetClassDef(class_def_index);
  const char* descriptor = context->dex_file->GetClassDescriptor(class_def);
  Class* klass = context->class_linker->FindClass(descriptor, context->class_loader);
  if (klass == NULL) {
    Thread* self = Thread::Current();
    CHECK(self->IsExceptionPending());
    self->ClearException();
    return;
  }
  CHECK(klass->IsResolved()) << PrettyClass(klass);
  context->class_linker->VerifyClass(klass);

  if (klass->IsErroneous()) {
    // ClassLinker::VerifyClass throws, which isn't useful in the compiler.
    CHECK(Thread::Current()->IsExceptionPending());
    Thread::Current()->ClearException();
    // We want to try verification again at run-time, so move back into the resolved state.
    klass->SetStatus(Class::kStatusResolved);
  }

  CHECK(klass->IsVerified() || klass->IsResolved()) << PrettyClass(klass);
  CHECK(!Thread::Current()->IsExceptionPending()) << PrettyTypeOf(Thread::Current()->GetException());
}

void Compiler::VerifyDexFile(const ClassLoader* class_loader, const DexFile& dex_file) {
  dex_file.ChangePermissions(PROT_READ | PROT_WRITE);

  Context context;
  context.class_linker = Runtime::Current()->GetClassLinker();
  context.class_loader = class_loader;
  context.dex_file = &dex_file;
  ForAll(&context, 0, dex_file.NumClassDefs(), VerifyClass);

  dex_file.ChangePermissions(PROT_READ);
}

void Compiler::InitializeClassesWithoutClinit(const ClassLoader* class_loader,
                                              const std::vector<const DexFile*>& dex_files) {
  for (size_t i = 0; i != dex_files.size(); ++i) {
    const DexFile* dex_file = dex_files[i];
    CHECK(dex_file != NULL);
    InitializeClassesWithoutClinit(class_loader, *dex_file);
  }
}

void Compiler::InitializeClassesWithoutClinit(const ClassLoader* class_loader, const DexFile& dex_file) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  for (size_t class_def_index = 0; class_def_index < dex_file.NumClassDefs(); class_def_index++) {
    const DexFile::ClassDef& class_def = dex_file.GetClassDef(class_def_index);
    const char* descriptor = dex_file.GetClassDescriptor(class_def);
    Class* klass = class_linker->FindClass(descriptor, class_loader);
    if (klass != NULL) {
      class_linker->EnsureInitialized(klass, false);
      // record the final class status if necessary
      Class::Status status = klass->GetStatus();
      ClassReference ref(&dex_file, class_def_index);
      MutexLock mu(compiled_classes_lock_);
      CompiledClass* compiled_class = GetCompiledClass(ref);
      if (compiled_class == NULL) {
        compiled_class = new CompiledClass(status);
        compiled_classes_[ref] = compiled_class;
      } else {
        DCHECK_EQ(status, compiled_class->GetStatus());
      }
    }
    // clear any class not found or verification exceptions
    Thread::Current()->ClearException();
  }

  DexCache* dex_cache = class_linker->FindDexCache(dex_file);
  for (size_t type_idx = 0; type_idx < dex_cache->NumResolvedTypes(); type_idx++) {
    Class* klass = class_linker->ResolveType(dex_file, type_idx, dex_cache, class_loader);
    if (klass == NULL) {
      Thread::Current()->ClearException();
    } else if (klass->IsInitialized()) {
      dex_cache->GetInitializedStaticStorage()->Set(type_idx, klass);
    }
  }
}

void Compiler::Compile(const ClassLoader* class_loader,
                       const std::vector<const DexFile*>& dex_files) {
  for (size_t i = 0; i != dex_files.size(); ++i) {
    const DexFile* dex_file = dex_files[i];
    CHECK(dex_file != NULL);
    CompileDexFile(class_loader, *dex_file);
  }
}

void Compiler::CompileClass(Context* context, size_t class_def_index) {
  const ClassLoader* class_loader = context->class_loader;
  const DexFile& dex_file = *context->dex_file;
  const DexFile::ClassDef& class_def = dex_file.GetClassDef(class_def_index);
  if (SkipClass(class_loader, dex_file, class_def)) {
    return;
  }
  const byte* class_data = dex_file.GetClassData(class_def);
  if (class_data == NULL) {
    // empty class, probably a marker interface
    return;
  }
  ClassDataItemIterator it(dex_file, class_data);
  // Skip fields
  while (it.HasNextStaticField()) {
    it.Next();
  }
  while (it.HasNextInstanceField()) {
    it.Next();
  }
  // Compile direct methods
  while (it.HasNextDirectMethod()) {
    context->compiler->CompileMethod(it.GetMethodCodeItem(), it.GetMemberAccessFlags(),
                                     it.GetMemberIndex(), class_loader, dex_file);
    it.Next();
  }
  // Compile virtual methods
  while (it.HasNextVirtualMethod()) {
    context->compiler->CompileMethod(it.GetMethodCodeItem(), it.GetMemberAccessFlags(),
                                     it.GetMemberIndex(), class_loader, dex_file);
    it.Next();
  }
  DCHECK(!it.HasNext());
}

void Compiler::CompileDexFile(const ClassLoader* class_loader, const DexFile& dex_file) {
  Context context;
  context.class_loader = class_loader;
  context.compiler = this;
  context.dex_file = &dex_file;
  ForAll(&context, 0, dex_file.NumClassDefs(), Compiler::CompileClass);
}

void Compiler::CompileMethod(const DexFile::CodeItem* code_item, uint32_t access_flags,
                             uint32_t method_idx, const ClassLoader* class_loader,
                             const DexFile& dex_file) {
  CompiledMethod* compiled_method = NULL;
  uint64_t start_ns = NanoTime();
  if ((access_flags & kAccNative) != 0) {
    compiled_method = jni_compiler_.Compile(access_flags, method_idx, class_loader, dex_file);
    CHECK(compiled_method != NULL);
  } else if ((access_flags & kAccAbstract) != 0) {
  } else {
    compiled_method = oatCompileMethod(*this, code_item, access_flags, method_idx, class_loader,
                                       dex_file, kThumb2);
    CHECK(compiled_method != NULL) << PrettyMethod(method_idx, dex_file);
  }
  uint64_t duration_ns = NanoTime() - start_ns;
  if (duration_ns > MsToNs(100)) {
    LOG(WARNING) << "Compilation of " << PrettyMethod(method_idx, dex_file)
                 << " took " << PrettyDuration(duration_ns);
  }

  if (compiled_method != NULL) {
    MethodReference ref(&dex_file, method_idx);
    CHECK(GetCompiledMethod(ref) == NULL) << PrettyMethod(method_idx, dex_file);
    MutexLock mu(compiled_methods_lock_);
    compiled_methods_[ref] = compiled_method;
    DCHECK(GetCompiledMethod(ref) != NULL) << PrettyMethod(method_idx, dex_file);
  }

  const char* shorty = dex_file.GetMethodShorty(dex_file.GetMethodId(method_idx));
  bool is_static = (access_flags & kAccStatic) != 0;
  const CompiledInvokeStub* compiled_invoke_stub = FindInvokeStub(is_static, shorty);
  if (compiled_invoke_stub == NULL) {
    if (instruction_set_ == kX86) {
      compiled_invoke_stub = ::art::x86::X86CreateInvokeStub(is_static, shorty);
    } else {
      CHECK(instruction_set_ == kArm || instruction_set_ == kThumb2);
      // Generates invocation stub using ARM instruction set
      compiled_invoke_stub = ::art::arm::ArmCreateInvokeStub(is_static, shorty);
    }
    CHECK(compiled_invoke_stub != NULL);
    InsertInvokeStub(is_static, shorty, compiled_invoke_stub);
  }
  CHECK(!Thread::Current()->IsExceptionPending()) << PrettyMethod(method_idx, dex_file);
}

static std::string MakeInvokeStubKey(bool is_static, const char* shorty) {
  std::string key(shorty);
  if (is_static) {
    key += "$";  // Must not be a shorty type character.
  }
  return key;
}

const CompiledInvokeStub* Compiler::FindInvokeStub(bool is_static, const char* shorty) const {
  MutexLock mu(compiled_invoke_stubs_lock_);
  const std::string key(MakeInvokeStubKey(is_static, shorty));
  InvokeStubTable::const_iterator it = compiled_invoke_stubs_.find(key);
  if (it == compiled_invoke_stubs_.end()) {
    return NULL;
  } else {
    DCHECK(it->second != NULL);
    return it->second;
  }
}

void Compiler::InsertInvokeStub(bool is_static, const char* shorty,
                                const CompiledInvokeStub* compiled_invoke_stub) {
  MutexLock mu(compiled_invoke_stubs_lock_);
  std::string key(MakeInvokeStubKey(is_static, shorty));
  compiled_invoke_stubs_[key] = compiled_invoke_stub;
}

CompiledClass* Compiler::GetCompiledClass(ClassReference ref) const {
  MutexLock mu(compiled_classes_lock_);
  ClassTable::const_iterator it = compiled_classes_.find(ref);
  if (it == compiled_classes_.end()) {
    return NULL;
  }
  CHECK(it->second != NULL);
  return it->second;
}

CompiledMethod* Compiler::GetCompiledMethod(MethodReference ref) const {
  MutexLock mu(compiled_methods_lock_);
  MethodTable::const_iterator it = compiled_methods_.find(ref);
  if (it == compiled_methods_.end()) {
    return NULL;
  }
  CHECK(it->second != NULL);
  return it->second;
}

void Compiler::SetGcMaps(const ClassLoader* class_loader, const std::vector<const DexFile*>& dex_files) {
  for (size_t i = 0; i != dex_files.size(); ++i) {
    const DexFile* dex_file = dex_files[i];
    CHECK(dex_file != NULL);
    SetGcMapsDexFile(class_loader, *dex_file);
  }
}

void Compiler::SetGcMapsDexFile(const ClassLoader* class_loader, const DexFile& dex_file) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  DexCache* dex_cache = class_linker->FindDexCache(dex_file);
  for (size_t class_def_index = 0; class_def_index < dex_file.NumClassDefs(); class_def_index++) {
    const DexFile::ClassDef& class_def = dex_file.GetClassDef(class_def_index);
    const char* descriptor = dex_file.GetClassDescriptor(class_def);
    Class* klass = class_linker->FindClass(descriptor, class_loader);
    if (klass == NULL || !klass->IsVerified()) {
      Thread::Current()->ClearException();
      continue;
    }
    const byte* class_data = dex_file.GetClassData(class_def);
    if (class_data == NULL) {
      // empty class such as a marker interface
      continue;
    }
    ClassDataItemIterator it(dex_file, class_data);
    while (it.HasNextStaticField()) {
      it.Next();
    }
    while (it.HasNextInstanceField()) {
      it.Next();
    }
    while (it.HasNextDirectMethod()) {
      Method* method = class_linker->ResolveMethod(dex_file, it.GetMemberIndex(), dex_cache,
                                                   class_loader, true);
      SetGcMapsMethod(dex_file, method);
      it.Next();
    }
    while (it.HasNextVirtualMethod()) {
      Method* method = class_linker->ResolveMethod(dex_file, it.GetMemberIndex(), dex_cache,
                                                   class_loader, false);
      SetGcMapsMethod(dex_file, method);
      it.Next();
    }
  }
}

void Compiler::SetGcMapsMethod(const DexFile& dex_file, Method* method) {
  if (method == NULL) {
    Thread::Current()->ClearException();
    return;
  }
  uint16_t method_idx = method->GetDexMethodIndex();
  MethodReference ref(&dex_file, method_idx);
  CompiledMethod* compiled_method = GetCompiledMethod(ref);
  if (compiled_method == NULL) {
    return;
  }
  const std::vector<uint8_t>* gc_map = verifier::DexVerifier::GetGcMap(ref);
  if (gc_map == NULL) {
    return;
  }
  compiled_method->SetGcMap(*gc_map);
}

void Compiler::SetCodeAndDirectMethods(const std::vector<const DexFile*>& dex_files) {
  for (size_t i = 0; i != dex_files.size(); ++i) {
    const DexFile* dex_file = dex_files[i];
    CHECK(dex_file != NULL);
    SetCodeAndDirectMethodsDexFile(*dex_file);
  }
}

void Compiler::SetCodeAndDirectMethodsDexFile(const DexFile& dex_file) {
  Runtime* runtime = Runtime::Current();
  ClassLinker* class_linker = runtime->GetClassLinker();
  DexCache* dex_cache = class_linker->FindDexCache(dex_file);
  CodeAndDirectMethods* code_and_direct_methods = dex_cache->GetCodeAndDirectMethods();
  for (size_t i = 0; i < dex_cache->NumResolvedMethods(); i++) {
    Method* method = dex_cache->GetResolvedMethod(i);
    if (method == NULL || method->IsDirect()) {
      Runtime::TrampolineType type = Runtime::GetTrampolineType(method);
      ByteArray* res_trampoline = runtime->GetResolutionStubArray(type);
      code_and_direct_methods->SetResolvedDirectMethodTrampoline(i, res_trampoline);
    } else {
      // TODO: we currently leave the entry blank for resolved
      // non-direct methods.  we could put in an error stub.
    }
  }
}

}  // namespace art
