// Copyright 2011 Google Inc. All Rights Reserved.

#include "compiler.h"

#include <sys/mman.h>

#include "assembler.h"
#include "class_linker.h"
#include "class_loader.h"
#include "dex_cache.h"
#include "jni_compiler.h"
#include "jni_internal.h"
#include "oat_file.h"
#include "runtime.h"
#include "stl_util.h"

art::CompiledMethod* oatCompileMethod(const art::Compiler& compiler,
                                      const art::DexFile::CodeItem* code_item,
                                      uint32_t access_flags, uint32_t method_idx,
                                      const art::ClassLoader* class_loader,
                                      const art::DexFile& dex_file, art::InstructionSet);

namespace art {

namespace arm {
  ByteArray* CreateAbstractMethodErrorStub();
  CompiledInvokeStub* ArmCreateInvokeStub(bool is_static, const char* shorty);
  ByteArray* ArmCreateResolutionTrampoline(Runtime::TrampolineType type);
  ByteArray* CreateJniDlysmLookupStub();
}
namespace x86 {
  ByteArray* CreateAbstractMethodErrorStub();
  CompiledInvokeStub* X86CreateInvokeStub(bool is_static, const char* shorty);
  ByteArray* X86CreateResolutionTrampoline(Runtime::TrampolineType type);
  ByteArray* CreateJniDlysmLookupStub();
}

Compiler::Compiler(InstructionSet instruction_set, bool image)
    : instruction_set_(instruction_set),
      jni_compiler_(instruction_set),
      image_(image),
      verbose_(false) {
  CHECK(!Runtime::Current()->IsStarted());
}

Compiler::~Compiler() {
  STLDeleteValues(&compiled_methods_);
  STLDeleteValues(&compiled_invoke_stubs_);
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

ByteArray* Compiler::CreateJniDlysmLookupStub(InstructionSet instruction_set) {
  switch (instruction_set) {
    case kArm:
    case kThumb2:
      return arm::CreateJniDlysmLookupStub();
    case kX86:
      return x86::CreateJniDlysmLookupStub();
    default:
      LOG(FATAL) << "Unknown InstructionSet " << (int) instruction_set;
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
  for (size_t i = 0; i != dex_files.size(); ++i) {
    ResolveDexFile(class_loader, *dex_files[i]);
  }
  for (size_t i = 0; i != dex_files.size(); ++i) {
    VerifyDexFile(class_loader, *dex_files[i]);
  }
  for (size_t i = 0; i != dex_files.size(); ++i) {
    InitializeClassesWithoutClinit(class_loader, *dex_files[i]);
  }
  for (size_t i = 0; i != dex_files.size(); ++i) {
    CompileDexFile(class_loader, *dex_files[i]);
  }
  for (size_t i = 0; i != dex_files.size(); ++i) {
    SetCodeAndDirectMethodsDexFile(*dex_files[i]);
  }
}

void Compiler::CompileOne(const Method* method) {
  DCHECK(!Runtime::Current()->IsStarted());
  const ClassLoader* class_loader = method->GetDeclaringClass()->GetClassLoader();
  Resolve(class_loader);
  Verify(class_loader);
  InitializeClassesWithoutClinit(class_loader);
  // Find the dex_file
  const DexCache* dex_cache = method->GetDeclaringClass()->GetDexCache();
  const DexFile& dex_file = Runtime::Current()->GetClassLinker()->FindDexFile(dex_cache);
  uint32_t method_idx = method->GetDexMethodIndex();
  const DexFile::CodeItem* code_item = dex_file.GetCodeItem(method->GetCodeItemOffset());
  CompileMethod(code_item, method->GetAccessFlags(), method_idx, class_loader, dex_file);
  SetCodeAndDirectMethods(class_loader);
}

void Compiler::Resolve(const ClassLoader* class_loader) {
  const std::vector<const DexFile*>& class_path
      = ClassLoader::GetCompileTimeClassPath(class_loader);
  for (size_t i = 0; i != class_path.size(); ++i) {
    const DexFile* dex_file = class_path[i];
    CHECK(dex_file != NULL);
    ResolveDexFile(class_loader, *dex_file);
  }
}

void Compiler::ResolveDexFile(const ClassLoader* class_loader, const DexFile& dex_file) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  DexCache* dex_cache = class_linker->FindDexCache(dex_file);

  // Strings are easy, they always are simply resolved to literals in the same file
  if (IsImage()) {  // Only resolve when we'll have an image, so compiler won't choose fast path
    for (size_t string_idx = 0; string_idx < dex_cache->NumStrings(); string_idx++) {
      class_linker->ResolveString(dex_file, string_idx, dex_cache);
    }
  }

  // Class derived values are more complicated, they require the linker and loader.
  for (size_t type_idx = 0; type_idx < dex_cache->NumResolvedTypes(); type_idx++) {
    Class* klass = class_linker->ResolveType(dex_file, type_idx, dex_cache, class_loader);
    if (klass == NULL) {
      Thread::Current()->ClearException();
    }
  }

  // Method and Field are the worst. We can't resolve without either
  // context from the code use (to disambiguate virtual vs direct
  // method and instance vs static field) or from class
  // definitions. While the compiler will resolve what it can as it
  // needs it, here we try to resolve fields and methods used in class
  // definitions, since many of them many never be referenced by
  // generated code.
  for (size_t class_def_index = 0; class_def_index < dex_file.NumClassDefs(); class_def_index++) {
    const DexFile::ClassDef& class_def = dex_file.GetClassDef(class_def_index);

    // Note the class_data pointer advances through the headers,
    // static fields, instance fields, direct methods, and virtual
    // methods.
    const byte* class_data = dex_file.GetClassData(class_def);
    if (class_data == NULL) {
      // empty class such as a marker interface
      continue;
    }
    ClassDataItemIterator it(dex_file, class_data);
    while (it.HasNextStaticField()) {
      Field* field = class_linker->ResolveField(dex_file, it.GetMemberIndex(), dex_cache,
                                                class_loader, true);
      if (field == NULL) {
        Thread* self = Thread::Current();
        CHECK(self->IsExceptionPending());
        self->ClearException();
      }
      it.Next();
    }
    while (it.HasNextInstanceField()) {
      Field* field = class_linker->ResolveField(dex_file, it.GetMemberIndex(), dex_cache,
                                                class_loader, false);
      if (field == NULL) {
        Thread* self = Thread::Current();
        CHECK(self->IsExceptionPending());
        self->ClearException();
      }
      it.Next();
    }
    while (it.HasNextDirectMethod()) {
      Method* method = class_linker->ResolveMethod(dex_file, it.GetMemberIndex(), dex_cache,
                                                   class_loader, true);
      if (method == NULL) {
        Thread* self = Thread::Current();
        CHECK(self->IsExceptionPending());
        self->ClearException();
      }
      it.Next();
    }
    while (it.HasNextVirtualMethod()) {
      Method* method = class_linker->ResolveMethod(dex_file, it.GetMemberIndex(), dex_cache,
                                                   class_loader, false);
      if (method == NULL) {
        Thread* self = Thread::Current();
        CHECK(self->IsExceptionPending());
        self->ClearException();
      }
      it.Next();
    }
    DCHECK(!it.HasNext());
  }
}

void Compiler::Verify(const ClassLoader* class_loader) {
  const std::vector<const DexFile*>& class_path
      = ClassLoader::GetCompileTimeClassPath(class_loader);
  for (size_t i = 0; i != class_path.size(); ++i) {
    const DexFile* dex_file = class_path[i];
    CHECK(dex_file != NULL);
    VerifyDexFile(class_loader, *dex_file);
  }
}

void Compiler::VerifyDexFile(const ClassLoader* class_loader, const DexFile& dex_file) {
  dex_file.ChangePermissions(PROT_READ | PROT_WRITE);
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  for (size_t class_def_index = 0; class_def_index < dex_file.NumClassDefs(); class_def_index++) {
    const DexFile::ClassDef& class_def = dex_file.GetClassDef(class_def_index);
    const char* descriptor = dex_file.GetClassDescriptor(class_def);
    Class* klass = class_linker->FindClass(descriptor, class_loader);
    if (klass == NULL) {
      Thread* self = Thread::Current();
      CHECK(self->IsExceptionPending());
      self->ClearException();
      continue;
    }
    CHECK(klass->IsResolved()) << PrettyClass(klass);
    class_linker->VerifyClass(klass);

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
  dex_file.ChangePermissions(PROT_READ);
}

void Compiler::InitializeClassesWithoutClinit(const ClassLoader* class_loader) {
  const std::vector<const DexFile*>& class_path
      = ClassLoader::GetCompileTimeClassPath(class_loader);
  for (size_t i = 0; i != class_path.size(); ++i) {
    const DexFile* dex_file = class_path[i];
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

void Compiler::Compile(const ClassLoader* class_loader) {
  const std::vector<const DexFile*>& class_path
      = ClassLoader::GetCompileTimeClassPath(class_loader);
  for (size_t i = 0; i != class_path.size(); ++i) {
    const DexFile* dex_file = class_path[i];
    CHECK(dex_file != NULL);
    CompileDexFile(class_loader, *dex_file);
  }
}

void Compiler::CompileDexFile(const ClassLoader* class_loader, const DexFile& dex_file) {
  for (size_t class_def_index = 0; class_def_index < dex_file.NumClassDefs(); class_def_index++) {
    const DexFile::ClassDef& class_def = dex_file.GetClassDef(class_def_index);
    CompileClass(class_def, class_loader, dex_file);
  }
}

void Compiler::CompileClass(const DexFile::ClassDef& class_def, const ClassLoader* class_loader,
                            const DexFile& dex_file) {
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
    CompileMethod(it.GetMethodCodeItem(), it.GetMemberAccessFlags(), it.GetMemberIndex(),
                  class_loader, dex_file);
    it.Next();
  }
  // Compile virtual methods
  while (it.HasNextVirtualMethod()) {
    CompileMethod(it.GetMethodCodeItem(), it.GetMemberAccessFlags(), it.GetMemberIndex(),
                  class_loader, dex_file);
    it.Next();
  }
  DCHECK(!it.HasNext());
}

void Compiler::CompileMethod(const DexFile::CodeItem* code_item, uint32_t access_flags,
                             uint32_t method_idx, const ClassLoader* class_loader,
                             const DexFile& dex_file) {
  CompiledMethod* compiled_method = NULL;
  if ((access_flags & kAccNative) != 0) {
    compiled_method = jni_compiler_.Compile(access_flags, method_idx, class_loader, dex_file);
    CHECK(compiled_method != NULL);
  } else if ((access_flags & kAccAbstract) != 0) {
  } else {
    compiled_method = oatCompileMethod(*this, code_item, access_flags, method_idx, class_loader,
                                       dex_file, kThumb2);
    CHECK(compiled_method != NULL);
  }

  if (compiled_method != NULL) {
    MethodReference ref(&dex_file, method_idx);
    CHECK(compiled_methods_.find(ref) == compiled_methods_.end())
        << PrettyMethod(method_idx, dex_file);
    compiled_methods_[ref] = compiled_method;
    DCHECK(compiled_methods_.find(ref) != compiled_methods_.end())
        << PrettyMethod(method_idx, dex_file);
    DCHECK(GetCompiledMethod(ref) != NULL)
        << PrettyMethod(method_idx, dex_file);
  }

  const char* shorty = dex_file.GetMethodShorty(dex_file.GetMethodId(method_idx));
  bool is_static = (access_flags & kAccStatic) != 0;
  const CompiledInvokeStub* compiled_invoke_stub = FindInvokeStub(is_static, shorty);
  if (compiled_invoke_stub == NULL) {
    if (instruction_set_ == kX86) {
      compiled_invoke_stub = art::x86::X86CreateInvokeStub(is_static, shorty);
    } else {
      CHECK(instruction_set_ == kArm || instruction_set_ == kThumb2);
      // Generates invocation stub using ARM instruction set
      compiled_invoke_stub = art::arm::ArmCreateInvokeStub(is_static, shorty);
    }
    CHECK(compiled_invoke_stub != NULL);
    InsertInvokeStub(is_static, shorty, compiled_invoke_stub);
  }
  CHECK(!Thread::Current()->IsExceptionPending()) << PrettyMethod(method_idx, dex_file);
}

static std::string MakeInvokeStubKey(bool is_static, const char* shorty) {
  std::string key(shorty);
  if (is_static) {
    key += "$";  // musn't be a shorty type character
  }
  return key;
}

const CompiledInvokeStub* Compiler::FindInvokeStub(bool is_static, const char* shorty) const {
  std::string key = MakeInvokeStubKey(is_static, shorty);
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
  std::string key = MakeInvokeStubKey(is_static, shorty);
  compiled_invoke_stubs_[key] = compiled_invoke_stub;
}

CompiledMethod* Compiler::GetCompiledMethod(MethodReference ref) const {
  MethodTable::const_iterator it = compiled_methods_.find(ref);
  if (it == compiled_methods_.end()) {
    return NULL;
  }
  CHECK(it->second != NULL);
  return it->second;
}

void Compiler::SetCodeAndDirectMethods(const ClassLoader* class_loader) {
  const std::vector<const DexFile*>& class_path
      = ClassLoader::GetCompileTimeClassPath(class_loader);
  for (size_t i = 0; i != class_path.size(); ++i) {
    const DexFile* dex_file = class_path[i];
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
