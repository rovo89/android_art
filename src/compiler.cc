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

extern art::CompiledMethod* oatCompileMethod(const art::Compiler& compiler,
                                             const art::Method*,
                                             art::InstructionSet);

namespace art {

namespace arm {
  ByteArray* CreateAbstractMethodErrorStub();
  CompiledInvokeStub* ArmCreateInvokeStub(const Method* method);
  ByteArray* ArmCreateResolutionTrampoline(Runtime::TrampolineType type);
}
namespace x86 {
  ByteArray* CreateAbstractMethodErrorStub();
  CompiledInvokeStub* X86CreateInvokeStub(const Method* method);
  ByteArray* X86CreateResolutionTrampoline(Runtime::TrampolineType type);
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

ByteArray* Compiler::CreateAbstractMethodErrorStub(InstructionSet instruction_set) {
  if (instruction_set == kX86) {
    return x86::CreateAbstractMethodErrorStub();
  } else {
    CHECK(instruction_set == kArm || instruction_set == kThumb2);
    // Generates resolution stub using ARM instruction set
    return arm::CreateAbstractMethodErrorStub();
  }
}

void Compiler::CompileAll(const ClassLoader* class_loader) {
  DCHECK(!Runtime::Current()->IsStarted());
  Resolve(class_loader);
  Verify(class_loader);
  InitializeClassesWithoutClinit(class_loader);
  Compile(class_loader);
  SetCodeAndDirectMethods(class_loader);
}

void Compiler::CompileOne(const Method* method) {
  DCHECK(!Runtime::Current()->IsStarted());
  const ClassLoader* class_loader = method->GetDeclaringClass()->GetClassLoader();
  Resolve(class_loader);
  Verify(class_loader);
  InitializeClassesWithoutClinit(class_loader);
  CompileMethod(method);
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

    DexFile::ClassDataHeader header = dex_file.ReadClassDataHeader(&class_data);
    size_t num_static_fields = header.static_fields_size_;
    size_t num_instance_fields = header.instance_fields_size_;
    size_t num_direct_methods = header.direct_methods_size_;
    size_t num_virtual_methods = header.virtual_methods_size_;

    if (num_static_fields != 0) {
      uint32_t last_idx = 0;
      for (size_t i = 0; i < num_static_fields; ++i) {
        DexFile::Field dex_field;
        dex_file.dexReadClassDataField(&class_data, &dex_field, &last_idx);
        Field* field = class_linker->ResolveField(dex_file, dex_field.field_idx_, dex_cache,
                                                  class_loader, true);
        if (field == NULL) {
          Thread* self = Thread::Current();
          CHECK(self->IsExceptionPending());
          self->ClearException();
        }
      }
    }
    if (num_instance_fields != 0) {
      uint32_t last_idx = 0;
      for (size_t i = 0; i < num_instance_fields; ++i) {
        DexFile::Field dex_field;
        dex_file.dexReadClassDataField(&class_data, &dex_field, &last_idx);
        Field* field = class_linker->ResolveField(dex_file, dex_field.field_idx_, dex_cache,
                                                  class_loader, false);
        if (field == NULL) {
          Thread* self = Thread::Current();
          CHECK(self->IsExceptionPending());
          self->ClearException();
        }
      }
    }
    if (num_direct_methods != 0) {
      uint32_t last_idx = 0;
      for (size_t i = 0; i < num_direct_methods; ++i) {
        DexFile::Method dex_method;
        dex_file.dexReadClassDataMethod(&class_data, &dex_method, &last_idx);
        Method* method = class_linker->ResolveMethod(dex_file, dex_method.method_idx_, dex_cache,
                                                     class_loader, true);
        if (method == NULL) {
          Thread* self = Thread::Current();
          CHECK(self->IsExceptionPending());
          self->ClearException();
        }
      }
    }
    if (num_virtual_methods != 0) {
      uint32_t last_idx = 0;
      for (size_t i = 0; i < num_virtual_methods; ++i) {
        DexFile::Method dex_method;
        dex_file.dexReadClassDataMethod(&class_data, &dex_method, &last_idx);
        Method* method = class_linker->ResolveMethod(dex_file, dex_method.method_idx_, dex_cache,
                                                     class_loader, false);
        if (method == NULL) {
          Thread* self = Thread::Current();
          CHECK(self->IsExceptionPending());
          self->ClearException();
        }
      }
    }
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
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  for (size_t class_def_index = 0; class_def_index < dex_file.NumClassDefs(); class_def_index++) {
    const DexFile::ClassDef& class_def = dex_file.GetClassDef(class_def_index);
    const char* descriptor = dex_file.GetClassDescriptor(class_def);
    Class* klass = class_linker->FindClass(descriptor, class_loader);
    if (klass == NULL) {
      // previous verification error will cause FindClass to throw
      Thread* self = Thread::Current();
      CHECK(self->IsExceptionPending());
      self->ClearException();
    } else {
      CompileClass(klass);
    }
  }
}

void Compiler::CompileClass(const Class* klass) {
  for (size_t i = 0; i < klass->NumDirectMethods(); i++) {
    CompileMethod(klass->GetDirectMethod(i));
  }
  for (size_t i = 0; i < klass->NumVirtualMethods(); i++) {
    CompileMethod(klass->GetVirtualMethod(i));
  }
}

void Compiler::CompileMethod(const Method* method) {
  CompiledMethod* compiled_method = NULL;
  if (method->IsNative()) {
    compiled_method = jni_compiler_.Compile(method);
    CHECK(compiled_method != NULL);
  } else if (method->IsAbstract()) {
  } else {
    compiled_method = oatCompileMethod(*this, method, kThumb2);
    CHECK(compiled_method != NULL);
  }

  if (compiled_method != NULL) {
    CHECK(compiled_methods_.find(method) == compiled_methods_.end()) << PrettyMethod(method);
    compiled_methods_[method] = compiled_method;
    DCHECK(compiled_methods_.find(method) != compiled_methods_.end()) << PrettyMethod(method);
    DCHECK(GetCompiledMethod(method) != NULL) << PrettyMethod(method);
  }

  CompiledInvokeStub* compiled_invoke_stub = NULL;
  if (instruction_set_ == kX86) {
    compiled_invoke_stub = art::x86::X86CreateInvokeStub(method);
  } else {
    CHECK(instruction_set_ == kArm || instruction_set_ == kThumb2);
    // Generates invocation stub using ARM instruction set
    compiled_invoke_stub = art::arm::ArmCreateInvokeStub(method);
  }
  CHECK(compiled_invoke_stub != NULL);
  // TODO: this fails if we have an abstract method defined in more than one input dex file.
  CHECK(compiled_invoke_stubs_.find(method) == compiled_invoke_stubs_.end()) << PrettyMethod(method);
  compiled_invoke_stubs_[method] = compiled_invoke_stub;

  Thread* self = Thread::Current();
  CHECK(!self->IsExceptionPending()) << PrettyMethod(method);
}

const CompiledMethod* Compiler::GetCompiledMethod(const Method* method) const {
  MethodTable::const_iterator it = compiled_methods_.find(method);
  if (it == compiled_methods_.end()) {
    return NULL;
  }
  CHECK(it->second != NULL);
  return it->second;
}

const CompiledInvokeStub* Compiler::GetCompiledInvokeStub(const Method* method) const {
  InvokeStubTable::const_iterator it = compiled_invoke_stubs_.find(method);
  if (it == compiled_invoke_stubs_.end()) {
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
